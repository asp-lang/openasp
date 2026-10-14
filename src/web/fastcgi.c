// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/web/fastcgi.c`): HTTP/FastCGI transport. It converts bounded wire
 * records into request-owned data, streams response records, and supervises workers. Descriptor
 * and length variables are validated at system-call boundaries to keep malformed clients
 * isolated.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "fastcgi.h"
#include "log.h"
#include "openasp.h"
#include "component.h"
#include "shared_cache.h"
#include "dbg.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

enum { FCGI_BEGIN_REQUEST = 1, FCGI_END_REQUEST = 3, FCGI_PARAMS = 4, FCGI_STDIN = 5, FCGI_STDOUT = 6 };
enum { OASP_WORKER_RECYCLE = 75 };

typedef struct {
    /* Arena-backed storage; capacity grows geometrically within one request. */
    char *data;
    size_t length;
    size_t capacity;
    oasp_arena *arena;
} byte_buffer;

typedef struct fcgi_param {
    /* Decoded name/value strings point into the request arena. */
    struct fcgi_param *next;
    char *name;
    char *value;
} fcgi_param;

typedef struct {
    /* Protocol identity plus buffers accumulated from PARAMS and STDIN records. */
    uint16_t request_id;
    bool keep_connection;
    oasp_arena *arena;
    byte_buffer encoded_params;
    byte_buffer body;
} fcgi_request;

typedef struct {
    /* Cross-process accept count used to enforce one global max-accepts limit. */
    atomic_long accepted;
} fcgi_shared_state;

typedef enum {
    FCGI_READ_ERROR = -1,
    FCGI_READ_LIMIT = 0,
    FCGI_READ_OK = 1
} fcgi_read_result;

#if defined(MAP_ANONYMOUS)
#define OASP_MAP_ANONYMOUS MAP_ANONYMOUS
#else
#define OASP_MAP_ANONYMOUS MAP_ANON
#endif

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t server_listener = -1;

static bool read_exact(int fd, void *data, size_t length);

/* Signal-safe shutdown: close wakes a worker or parent blocked in accept(). */
static void stop_server(int signal_number) {
    (void)signal_number;
    running = 0;
    if (server_listener >= 0) {
        close((int)server_listener);
        server_listener = -1;
    }
}

/* Installs one restart-free termination handler with an empty signal mask. */
static bool install_signal_handler(int signal_number) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_server;
    if (sigemptyset(&action.sa_mask))
        return false;
    return sigaction(signal_number, &action, NULL) == 0;
}

/* Ensures `length` additional bytes fit, checking addition and growth overflow. */
static bool buffer_reserve(byte_buffer *buffer, size_t length) {
    if (!buffer->arena || length > SIZE_MAX - buffer->length)
        return false;
    if (buffer->length + length > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->length + length) {
            if (capacity > SIZE_MAX / 2)
                return false;
            capacity *= 2;
        }
        char *grown = oasp_arena_alloc(buffer->arena, capacity, _Alignof(char));
        if (!grown)
            return false;
        if (buffer->length)
            memcpy(grown, buffer->data, buffer->length);
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    return true;
}

/* Appends caller-owned bytes into arena-owned request storage. */
static bool buffer_append(byte_buffer *buffer, const void *data, size_t length) {
    if (!length)
        return true;
    if (!data || !buffer_reserve(buffer, length))
        return false;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return true;
}

/* Reserves then receives an exact protocol payload directly into the buffer. */
static bool buffer_read(int fd, byte_buffer *buffer, size_t length) {
    if (!length)
        return true;
    if (length > SIZE_MAX - buffer->length)
        return false;
    size_t old_length = buffer->length;
    if (!buffer_reserve(buffer, length))
        return false;
    if (!read_exact(fd, buffer->data + old_length, length)) {
        return false;
    }
    buffer->length += length;
    return true;
}

/* Clears borrowed buffer metadata; the containing arena owns actual storage. */
static void buffer_dispose(byte_buffer *buffer) {
    memset(buffer, 0, sizeof(*buffer));
}

/* Resets all per-request allocations in one operation and invalidates views. */
static void fcgi_request_dispose(fcgi_request *request) {
    if (!request)
        return;
    oasp_arena_reset(request->arena);
    memset(request, 0, sizeof(*request));
}

/* Consumes padding or ignored record payload without allocating from the request. */
static bool discard_exact(int fd, size_t length) {
    char buffer[4096];
    while (length) {
        size_t chunk = length < sizeof(buffer) ? length : sizeof(buffer);
        if (!read_exact(fd, buffer, chunk))
            return false;
        length -= chunk;
    }
    return true;
}

/* Receives exactly `length` bytes, retrying interrupted system calls. */
static bool read_exact(int fd, void *data, size_t length) {
    char *output = data;
    while (length) {
        ssize_t count = recv(fd, output, length, 0);
        if (count == 0)
            return false;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        output += count;
        length -= (size_t)count;
    }
    return true;
}

/* Sends exactly `length` bytes and suppresses SIGPIPE on supported platforms. */
static bool write_all(int fd, const void *data, size_t length) {
    const char *input = data;
    while (length) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t count = send(fd, input, length, flags);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        input += count;
        length -= (size_t)count;
    }
    return true;
}

/* Serializes one FastCGI record header, payload, and protocol-required padding. */
static bool send_record(int fd, uint8_t type, uint16_t request_id, const void *content, size_t length) {
    const unsigned char *bytes = content;
    do {
        size_t chunk = length > UINT16_MAX ? UINT16_MAX : length;
        unsigned char header[8] = {1, type, (unsigned char)(request_id >> 8), (unsigned char)request_id, (unsigned char)(chunk >> 8), (unsigned char)chunk,
                                   0, 0};
        if (!write_all(fd, header, sizeof(header)) || (chunk && !write_all(fd, bytes, chunk)))
            return false;
        if (chunk)
            bytes += chunk;
        length -= chunk;
    } while (length);
    return true;
}

/* Performs overflow-safe single-buffer limit accounting; zero means unlimited. */
static bool exceeds_limit(size_t current, size_t added, size_t limit) {
    return added > SIZE_MAX - current || (limit && (current > limit || added > limit - current));
}

/* Applies the aggregate request cap without overflowing either intermediate sum. */
static bool exceeds_combined_limit(size_t first, size_t second, size_t added, size_t limit) {
    return second > SIZE_MAX - first || exceeds_limit(first + second, added, limit);
}

/*
 * Reads records until both PARAMS and STDIN terminate. Unknown record types are
 * drained to keep framing synchronized; malformed lengths and configured limit
 * violations are distinguished so callers can return the appropriate status.
 */
static fcgi_read_result read_request(int fd, fcgi_request *request, oasp_arena *arena, const oasp_fastcgi_limits *limits) {
    oasp_arena_reset(arena);
    memset(request, 0, sizeof(*request));
    request->arena = arena;
    request->encoded_params.arena = request->arena;
    request->body.arena = request->arena;
    bool began = false;
    for (;;) {
        unsigned char header[8];
        if (!read_exact(fd, header, sizeof(header)))
            return FCGI_READ_ERROR;
        if (header[0] != 1)
            return FCGI_READ_ERROR;
        uint8_t type = header[1];
        uint16_t request_id = (uint16_t)((uint16_t)header[2] << 8 | header[3]);
        size_t content_length = (size_t)((uint16_t)header[4] << 8 | header[5]);
        size_t padding_length = header[6];
        bool end_stdin = false;
        if (type == FCGI_BEGIN_REQUEST && content_length >= 3) {
            unsigned char begin[3];
            if (!read_exact(fd, begin, sizeof(begin)) || !discard_exact(fd, content_length - sizeof(begin)))
                return FCGI_READ_ERROR;
            request->request_id = request_id;
            request->keep_connection = (begin[2] & 1u) != 0;
            began = true;
        } else if (began && request_id == request->request_id && type == FCGI_PARAMS && content_length) {
            if (exceeds_limit(request->encoded_params.length, content_length, limits->max_params_bytes) ||
                exceeds_combined_limit(request->encoded_params.length, request->body.length,
                                       content_length, limits->max_request_bytes))
                return FCGI_READ_LIMIT;
            if (!buffer_read(fd, &request->encoded_params, content_length))
                return FCGI_READ_ERROR;
        } else if (began && request_id == request->request_id && type == FCGI_STDIN) {
            if (exceeds_limit(request->body.length, content_length, limits->max_body_bytes) ||
                exceeds_combined_limit(request->encoded_params.length, request->body.length,
                                       content_length, limits->max_request_bytes))
                return FCGI_READ_LIMIT;
            if (content_length && !buffer_read(fd, &request->body, content_length))
                return FCGI_READ_ERROR;
            end_stdin = !content_length;
        } else if (!discard_exact(fd, content_length)) {
            return FCGI_READ_ERROR;
        }
        if (!discard_exact(fd, padding_length))
            return FCGI_READ_ERROR;
        if (end_stdin)
            return FCGI_READ_OK;
    }
}

/* Decodes FastCGI's one-byte or four-byte length form with bounds checks. */
static bool decode_length(const unsigned char *data, size_t length, size_t *offset, uint32_t *value) {
    if (*offset >= length)
        return false;
    unsigned char first = data[(*offset)++];
    if (!(first & 0x80u)) {
        *value = first;
        return true;
    }
    if (length - *offset < 3)
        return false;
    *value = ((uint32_t)(first & 0x7fu) << 24) | ((uint32_t)data[*offset] << 16) | ((uint32_t)data[*offset + 1] << 8) | data[*offset + 2];
    *offset += 3;
    return true;
}

/* Converts the encoded parameter stream into an arena-backed linked list. */
static bool decode_params(const byte_buffer *encoded, fcgi_param **result) {
    fcgi_param *first = NULL;
    fcgi_param **tail = &first;
    size_t offset = 0;
    while (offset < encoded->length) {
        uint32_t name_length;
        uint32_t value_length;
        if (!decode_length((const unsigned char *)encoded->data, encoded->length, &offset, &name_length) ||
            !decode_length((const unsigned char *)encoded->data, encoded->length, &offset, &value_length) || name_length > encoded->length - offset ||
            value_length > encoded->length - offset - name_length)
            return false;
        fcgi_param *parameter = oasp_arena_alloc(encoded->arena, sizeof(*parameter), _Alignof(fcgi_param));
        if (!parameter)
            return false;
        memset(parameter, 0, sizeof(*parameter));
        parameter->name = oasp_arena_alloc(encoded->arena, (size_t)name_length + 1, _Alignof(char));
        parameter->value = oasp_arena_alloc(encoded->arena, (size_t)value_length + 1, _Alignof(char));
        if (!parameter->name || !parameter->value)
            return false;
        memcpy(parameter->name, encoded->data + offset, name_length);
        parameter->name[name_length] = '\0';
        memcpy(parameter->value, encoded->data + offset + name_length, value_length);
        parameter->value[value_length] = '\0';
        offset += name_length + value_length;
        *tail = parameter;
        tail = &parameter->next;
    }
    *result = first;
    return true;
}

/* Performs case-sensitive CGI variable lookup and returns `fallback` when absent. */
static const char *param_get(fcgi_param *parameters, const char *name, const char *fallback) {
    for (fcgi_param *parameter = parameters; parameter; parameter = parameter->next)
        if (!strcmp(parameter->name, name))
            return parameter->value;
    return fallback;
}

/*
 * Resolves a CGI script below root one path component at a time. Legacy ASP
 * sites commonly link to files with casing that differs from the archive, so
 * each component is matched case-insensitively without relaxing the root
 * boundary or following a symlink outside it.
 */
static bool resolve_script_path(const char *root, const char *script, char resolved[PATH_MAX]) {
    char canonical_root[PATH_MAX];
    char canonical_script[PATH_MAX];
    char current[PATH_MAX];
    const char *relative;
    size_t root_length;
    if (!root || !script || !realpath(root, canonical_root))
        return false;
    /*
     * SCRIPT_FILENAME may use a symlinked system prefix (for example /var on
     * macOS), while the configured root has already been canonicalized.
     * Normalize an existing exact-path request before enforcing the boundary.
     * If this fails, retain component-wise case-insensitive resolution below.
     */
    if (realpath(script, canonical_script))
        script = canonical_script;
    root_length = strlen(canonical_root);
    if (strncmp(script, canonical_root, root_length) ||
        (script[root_length] && script[root_length] != '/')) {
        return false;
    }
    relative = script + root_length;
    while (*relative == '/')
        relative++;
    if (!*relative || root_length >= sizeof(current))
        return false;
    memcpy(current, canonical_root, root_length + 1);
    while (*relative) {
        const char *end = strchr(relative, '/');
        size_t component_length = end ? (size_t)(end - relative) : strlen(relative);
        char component[NAME_MAX + 1];
        char matched_name[NAME_MAX + 1];
        struct dirent *entry;
        DIR *directory;
        bool found = false;
        bool case_match = false;
        if (!component_length || component_length > NAME_MAX ||
            (component_length == 1 && relative[0] == '.') ||
            (component_length == 2 && relative[0] == '.' && relative[1] == '.')) {
            return false;
        }
        memcpy(component, relative, component_length);
        component[component_length] = '\0';
        directory = opendir(current);
        if (!directory)
            return false;
        while ((entry = readdir(directory)) != NULL) {
            if (!strcmp(entry->d_name, component)) {
                memcpy(matched_name, entry->d_name, component_length + 1);
                found = true;
                break;
            }
            if (!case_match && !strcasecmp(entry->d_name, component)) {
                memcpy(matched_name, entry->d_name, component_length + 1);
                case_match = true;
            }
        }
        closedir(directory);
        if (!found && case_match)
            found = true;
        if (!found)
            return false;
        size_t current_length = strlen(current);
        size_t entry_length = strlen(matched_name);
        if (current_length > SIZE_MAX - entry_length - 2 ||
            current_length + entry_length + 2 > PATH_MAX) {
            return false;
        }
        memcpy(resolved, current, current_length);
        resolved[current_length] = '/';
        memcpy(resolved + current_length + 1, matched_name, entry_length + 1);
        if (!realpath(resolved, current) ||
            strncmp(current, canonical_root, root_length) ||
            (current[root_length] && current[root_length] != '/')) {
            return false;
        }
        if (!end) {
            struct stat metadata;
            if (stat(current, &metadata) != 0 || !S_ISREG(metadata.st_mode))
                return false;
            memcpy(resolved, current, strlen(current) + 1);
            return true;
        }
        relative = end + 1;
        while (*relative == '/')
            relative++;
        if (!*relative)
            return false;
    }
    return false;
}

static int send_not_found_response(oasp_request *request) {
    if (oasp_request_set_status(request, "404 Not Found") ||
        oasp_request_set_content_type(request, "text/plain; charset=utf-8")) {
        return -1;
    }
    oasp_request_write(request, "Not Found\n", 10);
    return 0;
}

typedef struct {
    int fd;
    uint16_t request_id;
    oasp_request *request;
    bool started;
} fcgi_output_context;

/* Emits status/content type and validated application headers before body data. */
static bool send_response_headers(fcgi_output_context *context, const char *status, const char *content_type, bool include_application_headers) {
    byte_buffer headers = {.arena = oasp_request_arena(context->request)};
    bool ok = buffer_append(&headers, "Status: ", 8) && buffer_append(&headers, status, strlen(status)) &&
              buffer_append(&headers, "\r\nContent-Type: ", 16) && buffer_append(&headers, content_type, strlen(content_type)) &&
              buffer_append(&headers, "\r\n", 2);
    size_t header_count = include_application_headers ? oasp_request_header_count(context->request) : 0;
    for (size_t i = 0; ok && i < header_count; i++) {
        const char *name = oasp_request_header_name(context->request, i);
        const char *value = oasp_request_header_value(context->request, i);
        ok = name && value && buffer_append(&headers, name, strlen(name)) && buffer_append(&headers, ": ", 2) &&
             buffer_append(&headers, value, strlen(value)) && buffer_append(&headers, "\r\n", 2);
    }
    if (ok)
        ok = buffer_append(&headers, "\r\n", 2) && send_record(context->fd, FCGI_STDOUT, context->request_id, headers.data, headers.length);
    buffer_dispose(&headers);
    if (ok)
        context->started = true;
    return ok;
}

/* Response sink callback that chunks arbitrary body data into FastCGI records. */
static int stream_response(void *opaque, const void *data, size_t length) {
    fcgi_output_context *context = opaque;
    if (!context->started &&
        !send_response_headers(context, oasp_request_status(context->request), oasp_request_content_type(context->request), true)) {
        if (getenv("OPENASP_TRACE_FASTCGI"))
            fprintf(stderr, "OpenASP FastCGI trace: response headers failed request=%u errno=%d (%s)\n",
                    context->request_id, errno, strerror(errno));
        return -1;
    }
    if (!length || send_record(context->fd, FCGI_STDOUT, context->request_id, data, length))
        return 0;
    if (getenv("OPENASP_TRACE_FASTCGI"))
        fprintf(stderr, "OpenASP FastCGI trace: response body failed request=%u length=%zu errno=%d (%s)\n",
                context->request_id, length, errno, strerror(errno));
    return -1;
}

/*
 * Maps CGI parameters into an OpenASP request, renders one script, and emits a
 * complete FastCGI response. All transient data belongs to the supplied
 * request/response/scratch arenas and is invalid after the worker resets them.
 */
static bool handle_request(int fd, const char *root, oasp_engine *engine, fcgi_request *incoming, const oasp_fastcgi_limits *limits,
                           oasp_arena *request_arena, oasp_arena *response_arena, oasp_arena *scratch_arena, const oasp_debugger *debugger) {
    fcgi_param *parameters = NULL;
    if (!decode_params(&incoming->encoded_params, &parameters))
        return false;
    const char *script = param_get(parameters, "SCRIPT_FILENAME", NULL);
    const char *method = param_get(parameters, "REQUEST_METHOD", "GET");
    const char *query = param_get(parameters, "QUERY_STRING", "");
    char resolved_script[PATH_MAX];
    if (!script)
        return false;
    if (!buffer_append(&incoming->body, "", 1))
        return false;
    oasp_request *request = oasp_request_create_with_arenas(root, query, "", method, request_arena,
                                                            response_arena, scratch_arena);
    fcgi_output_context output = {.fd = fd, .request_id = incoming->request_id, .request = request};
    if (request && oasp_request_set_limits(request, &limits->request)) {
        oasp_request_destroy(request);
        request = NULL;
    }
    if (request)
        oasp_request_set_output_sink(request, stream_response, &output);
    for (fcgi_param *parameter = parameters; request && parameter; parameter = parameter->next)
        if (oasp_request_set_server_variable(request, parameter->name, parameter->value)) {
            oasp_request_destroy(request);
            request = NULL;
        }
    if (request && !param_get(parameters, "HTTP_COOKIE", NULL))
        (void)oasp_request_set_server_variable(request, "HTTP_COOKIE", "");
    if (request && oasp_request_set_body(request, incoming->body.data, incoming->body.length - 1)) {
        oasp_request_destroy(request);
        request = NULL;
    }
    bool script_found = resolve_script_path(root, script, resolved_script);
    int status = request ? (script_found
                                ? (debugger ? oasp_engine_render_file_debug(engine, request, resolved_script, debugger)
                                            : oasp_engine_render_file(engine, request, resolved_script))
                                : send_not_found_response(request))
                         : -1;
    bool ok;
    if (status == 0 && oasp_request_flush(request) == 0) {
        ok = true;
    } else if (output.started) {
        ok = true;
    } else {
        ok = send_response_headers(&output,
                                   request && oasp_request_deadline_expired(request)
                                       ? "504 Gateway Timeout"
                                       : "500 Internal Server Error",
                                   "text/plain; charset=utf-8", false);
    }
    if (ok)
        ok = send_record(fd, FCGI_STDOUT, incoming->request_id, NULL, 0);
    unsigned char end_request[8] = {0};
    if (status != 0) {
        end_request[3] = 1;
        oasp_log_write(OASP_LOG_ERROR, "request %u failed after response_started=%d: %s",
                       incoming->request_id, output.started ? 1 : 0,
                       request ? oasp_request_error(request) : "request initialization failed");
    }
    if (ok)
        ok = send_record(fd, FCGI_END_REQUEST, incoming->request_id, end_request, sizeof(end_request));
    oasp_request_destroy(request);
    return ok;
}

/* Returns a small deterministic 413 response after bounded input was rejected. */
static bool send_limit_response(int fd, uint16_t request_id) {
    static const char response[] = "Status: 413 Content Too Large\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nrequest input limit exceeded";
    unsigned char end_request[8] = {0};
    return send_record(fd, FCGI_STDOUT, request_id, response, sizeof(response) - 1) &&
           send_record(fd, FCGI_STDOUT, request_id, NULL, 0) &&
           send_record(fd, FCGI_END_REQUEST, request_id, end_request, sizeof(end_request));
}

/* Applies symmetric receive/send deadlines; zero deliberately disables them. */
static bool set_client_timeout(int fd, int milliseconds) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
        return false;
#endif
    if (!milliseconds)
        return true;
    struct timeval timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000
    };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

/* Creates a close-on-exec IPv4 listener and binds the configured address/port. */
static int listen_tcp(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 || bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 128)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Replaces a stale socket path, binds a close-on-exec Unix listener, and listens. */
static int listen_unix(const char *path) {
    struct stat metadata;
    if (!lstat(path, &metadata)) {
        if (!S_ISSOCK(metadata.st_mode)) {
            printf("path exists and is not a socket\n");
            return -1;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
        if (probe >= 0 && connect(probe, (struct sockaddr *)&address, sizeof(address)) == 0) {
            close(probe);
            printf("socket is already active\n");
            return -1;
        }
        if (probe >= 0)
            close(probe);
        if (unlink(path))
            return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || chmod(path, 0660) || listen(fd, 128)) {
        close(fd);
        return -1;
    }
    return fd;
}

typedef struct {
    int listener;
    int client;
    FILE *input;
    FILE *output;
    oasp_dbg debugger;
    bool active;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} debug_worker_agent;

static void debug_agent_close_client(debug_worker_agent *agent) {
    if (!agent || !agent->active)
        return;
    oasp_dbg_dispose(&agent->debugger);
    fclose(agent->input);
    fclose(agent->output);
    agent->input = NULL;
    agent->output = NULL;
    agent->client = -1;
    agent->active = false;
}

static void debug_agent_dispose(debug_worker_agent *agent) {
    if (!agent)
        return;
    debug_agent_close_client(agent);
    if (agent->listener >= 0)
        close(agent->listener);
    if (agent->path[0])
        (void)unlink(agent->path);
    memset(agent, 0, sizeof(*agent));
    agent->listener = -1;
    agent->client = -1;
}

static bool debug_agent_init(debug_worker_agent *agent, const char *base_path) {
    memset(agent, 0, sizeof(*agent));
    agent->listener = -1;
    agent->client = -1;
    if (!base_path)
        return true;
    int length = snprintf(agent->path, sizeof(agent->path), "%s.%ld", base_path, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(agent->path))
        return false;
    agent->listener = listen_unix(agent->path);
    if (agent->listener < 0 || chmod(agent->path, 0600)) {
        debug_agent_dispose(agent);
        return false;
    }
    return true;
}

/* Replaces a disconnected controller; debugger state is session-local by design. */
static void debug_agent_accept(debug_worker_agent *agent) {
    if (!agent || agent->listener < 0)
        return;
    int client = accept(agent->listener, NULL, NULL);
    if (client < 0)
        return;
    debug_agent_close_client(agent);
    int output_fd = dup(client);
    FILE *input = output_fd >= 0 ? fdopen(client, "r") : NULL;
    FILE *output = output_fd >= 0 ? fdopen(output_fd, "w") : NULL;
    if (!input || !output) {
        if (input)
            fclose(input);
        else
            close(client);
        if (output)
            fclose(output);
        else if (output_fd >= 0)
            close(output_fd);
        return;
    }
    agent->client = client;
    agent->input = input;
    agent->output = output;
    oasp_dbg_init(&agent->debugger, input, output);
    agent->active = true;
}

/* EOF is observed before starting a request so a dropped controller cannot abort it. */
static bool debug_agent_disconnected(const debug_worker_agent *agent) {
    if (!agent || !agent->active)
        return false;
    char byte;
    ssize_t received = recv(agent->client, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    return received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

static bool debug_proxy_write(int destination, const void *data, size_t length) {
    const char *cursor = data;
    while (length) {
        ssize_t written = send(destination, cursor, length, 0);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        cursor += written;
        length -= (size_t)written;
    }
    return true;
}

static bool debug_proxy_line(int client, char line[256]) {
    size_t length = 0;
    while (length + 1 < 256) {
        char character;
        ssize_t received = recv(client, &character, 1, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received != 1)
            return false;
        if (character == '\n') {
            line[length] = '\0';
            return true;
        }
        if (character != '\r')
            line[length++] = character;
    }
    return false;
}

static bool debug_token_equal(const char *left, const char *right) {
    size_t left_length = left ? strlen(left) : 0;
    size_t right_length = right ? strlen(right) : 0;
    size_t length = left_length > right_length ? left_length : right_length;
    unsigned difference = (unsigned)(left_length ^ right_length);
    for (size_t i = 0; i < length; i++) {
        unsigned char a = i < left_length ? (unsigned char)left[i] : 0;
        unsigned char b = i < right_length ? (unsigned char)right[i] : 0;
        difference |= (unsigned)(a ^ b);
    }
    return difference == 0;
}

static void debug_proxy_client(int client, bool require_token, const char *token, const char *base_path) {
    char line[256];
    if ((require_token && (!debug_proxy_line(client, line) || strncmp(line, "auth ", 5) ||
                           !debug_token_equal(line + 5, token))) ||
        !debug_proxy_line(client, line) || strncmp(line, "target ", 7)) {
        (void)debug_proxy_write(client, "error authentication or target required\n", 40);
        close(client);
        return;
    }
    char *end = NULL;
    errno = 0;
    long pid = strtol(line + 7, &end, 10);
    if (errno || !end || *end || pid <= 0) {
        (void)debug_proxy_write(client, "error invalid target\n", 21);
        close(client);
        return;
    }
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int length = snprintf(path, sizeof(path), "%s.%ld", base_path, pid);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        close(client);
        return;
    }
    int agent = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (agent < 0 || connect(agent, (struct sockaddr *)&address, sizeof(address))) {
        (void)debug_proxy_write(client, "error target unavailable\n", 23);
        if (agent >= 0)
            close(agent);
        close(client);
        return;
    }
    char attached[64];
    int attached_length = snprintf(attached, sizeof(attached), "attached %ld\n", pid);
    if (attached_length < 0 || !debug_proxy_write(client, attached, (size_t)attached_length)) {
        close(agent);
        close(client);
        return;
    }
    for (;;) {
        struct pollfd descriptors[2] = {{.fd = client, .events = POLLIN}, {.fd = agent, .events = POLLIN}};
        if (poll(descriptors, 2, -1) <= 0)
            break;
        char buffer[4096];
        for (size_t i = 0; i < 2; i++) {
            if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            ssize_t received = recv(descriptors[i].fd, buffer, sizeof(buffer), 0);
            int other = descriptors[i ? 0 : 1].fd;
            if (received <= 0 || !debug_proxy_write(other, buffer, (size_t)received))
                goto out;
        }
    }
out:
    close(agent);
    close(client);
}

static pid_t debug_broker_start(int unix_listener, int tcp_listener, const char *token, const char *base_path) {
    pid_t child = fork();
    if (child)
        return child;
    (void)signal(SIGCHLD, SIG_IGN);
    while (running) {
        struct pollfd descriptors[2] = {{.fd = unix_listener, .events = POLLIN}, {.fd = tcp_listener, .events = POLLIN}};
        nfds_t count = tcp_listener >= 0 ? 2 : 1;
        int ready = poll(descriptors, count, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        for (nfds_t i = 0; i < count; i++) {
            if (!(descriptors[i].revents & POLLIN))
                continue;
            int client = accept(descriptors[i].fd, NULL, NULL);
            if (client < 0)
                continue;
            pid_t proxy = fork();
            if (!proxy) {
                debug_proxy_client(client, descriptors[i].fd == tcp_listener, token, base_path);
                _exit(0);
            }
            close(client);
        }
    }
    _exit(0);
}

/* Releases idle engine, allocator, component, and libc pages between requests. */
static void release_allocator_pressure(oasp_engine *engine) {
    oasp_engine_release_memory(engine);
#if defined(__APPLE__)
    (void)malloc_zone_pressure_relief(NULL, 0);
#elif defined(__linux__)
    (void)malloc_trim(0);
#endif
}

/*
 * Owns one reusable engine and four reusable arenas. The accept counter is
 * incremented atomically across workers; request count and idle timers decide
 * when this worker recycles or returns unused pages.
 */
static int run_worker_process(int listener, const char *root, long max_accepts, long max_requests, int memory_idle_ms,
                              long memory_relief_requests, const oasp_fastcgi_limits *limits, fcgi_shared_state *shared, oasp_shared_cache *cache, const char *state_directory, const char *debug_path, const oasp_fastcgi_config *config) {
    oasp_engine *engine = oasp_engine_create_with_cache(cache);
    char external_error[512] = {0};
    if (engine && config->external_components_enabled &&
        (!oasp_engine_load_external_components(engine, config->external_modules, config->external_module_count,
                                               config->external_components_require_sha256, external_error, sizeof(external_error)) ||
         !oasp_engine_external_worker_init(engine, external_error, sizeof(external_error)))) {
        oasp_log_write(OASP_LOG_ERROR, "external component worker initialization failed: %s", external_error);
        oasp_engine_destroy(engine);
        return 1;
    }
    oasp_arena *input_arena = oasp_arena_create_mapped(16 * 1024);
    oasp_arena *request_arena = oasp_arena_create_mapped(16 * 1024);
    oasp_arena *response_arena = oasp_arena_create_mapped(16 * 1024);
    oasp_arena *scratch_arena = oasp_arena_create_mapped(64 * 1024);
    bool resources_ready = engine && input_arena && request_arena && response_arena && scratch_arena;
    int state_result = resources_ready ? oasp_engine_enable_shared_state(engine, state_directory, root) : -1;
    if (!resources_ready || state_result) {
        const char *stage = !engine                  ? "engine"
                            : !input_arena           ? "input arena"
                            : !request_arena         ? "request arena"
                            : !response_arena        ? "response arena"
                            : !scratch_arena         ? "scratch arena"
                                                     : "shared SQLite state store";
        oasp_log_write(OASP_LOG_ERROR,
                       "worker initialization failed at %s; verify memory availability and that opcache-dir is writable",
                       stage);
        oasp_arena_destroy(scratch_arena);
        oasp_arena_destroy(response_arena);
        oasp_arena_destroy(request_arena);
        oasp_arena_destroy(input_arena);
        oasp_engine_external_worker_shutdown(engine);
        oasp_engine_destroy(engine);
        return 1;
    }
    debug_worker_agent agent;
    if (!debug_agent_init(&agent, debug_path)) {
        oasp_log_write(OASP_LOG_ERROR, "worker debugger agent initialization failed: %s", strerror(errno));
        oasp_arena_destroy(scratch_arena);
        oasp_arena_destroy(response_arena);
        oasp_arena_destroy(request_arena);
        oasp_arena_destroy(input_arena);
        oasp_engine_external_worker_shutdown(engine);
        oasp_engine_destroy(engine);
        return 1;
    }
    long handled = 0;
    bool pressure_pending = false;
    while (running) {
        struct pollfd descriptors[3] = {
            {.fd = listener, .events = POLLIN},
            {.fd = agent.listener, .events = POLLIN},
            {.fd = agent.client, .events = 0},
        };
        nfds_t descriptor_count = agent.listener >= 0 ? 2 : 1;
        if (agent.active)
            descriptor_count = 3;
        int ready;
        do {
            ready = poll(descriptors, descriptor_count, memory_idle_ms > 0 ? memory_idle_ms : -1);
        } while (ready < 0 && errno == EINTR && running);
        if (!running)
            break;
        if (ready == 0) {
            if (pressure_pending) {
                release_allocator_pressure(engine);
                pressure_pending = false;
            }
            continue;
        }
        if (ready < 0)
            break;
        if (agent.listener >= 0 && (descriptors[1].revents & POLLIN))
            debug_agent_accept(&agent);
        if (agent.active && (descriptors[2].revents & (POLLHUP | POLLERR | POLLNVAL)))
            debug_agent_close_client(&agent);
        if (!(descriptors[0].revents & POLLIN))
            continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!set_client_timeout(client, limits->client_timeout_ms)) {
            close(client);
            continue;
        }
        long sequence = atomic_fetch_add_explicit(&shared->accepted, 1, memory_order_relaxed);
        if (max_accepts && sequence >= max_accepts) {
            close(client);
            break;
        }
        bool limit_reached = max_accepts && sequence + 1 >= max_accepts;
        bool keep_connection;
        do {
            fcgi_request request;
            fcgi_read_result read_result = read_request(client, &request, input_arena, limits);
            if (read_result != FCGI_READ_OK) {
                if (read_result == FCGI_READ_LIMIT && request.request_id)
                    (void)send_limit_response(client, request.request_id);
                fcgi_request_dispose(&request);
                break;
            }
            keep_connection = request.keep_connection;
            if (debug_agent_disconnected(&agent))
                debug_agent_close_client(&agent);
            (void)handle_request(client, root, engine, &request, limits, request_arena, response_arena, scratch_arena,
                                 agent.active ? &agent.debugger.interface : NULL);
            fcgi_request_dispose(&request);
            oasp_arena_reset(scratch_arena);
            oasp_arena_reset(response_arena);
            oasp_arena_reset(request_arena);
            handled++;
            if (agent.active && agent.debugger.quit)
                debug_agent_close_client(&agent);
            pressure_pending = true;
            if (memory_relief_requests > 0 && handled % memory_relief_requests == 0) {
                release_allocator_pressure(engine);
                pressure_pending = false;
            }
            if (max_requests && handled >= max_requests)
                keep_connection = false;
        } while (keep_connection);
        close(client);
        if (max_requests && handled >= max_requests) {
            debug_agent_dispose(&agent);
            oasp_arena_destroy(scratch_arena);
            oasp_arena_destroy(response_arena);
            oasp_arena_destroy(request_arena);
            oasp_arena_destroy(input_arena);
            oasp_engine_external_worker_shutdown(engine);
            oasp_engine_destroy(engine);
            return OASP_WORKER_RECYCLE;
        }
        if (limit_reached)
            break;
    }
    debug_agent_dispose(&agent);
    oasp_arena_destroy(scratch_arena);
    oasp_arena_destroy(response_arena);
    oasp_arena_destroy(request_arena);
    oasp_arena_destroy(input_arena);
    oasp_engine_external_worker_shutdown(engine);
    oasp_engine_destroy(engine);
    return 0;
}

static void delay_worker_restart(unsigned failures) {
    unsigned shift = failures > 6 ? 6 : failures;
    long milliseconds = 50L << shift;
    if (milliseconds > 5000)
        milliseconds = 5000;
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (running && nanosleep(&delay, &delay) && errno == EINTR) {
    }
}

/*
 * Creates shared cache/state before forking, supervises unexpected exits, and
 * replaces workers until shutdown or the global accept limit. Child processes
 * use _exit() so inherited parent buffers and cleanup do not run twice.
 */
static int run_worker_pool(int listener, const oasp_fastcgi_config *config) {
    const char *root = config->root;
    long max_accepts = config->max_accepts;
    long max_requests = config->max_requests;
    int memory_idle_ms = config->memory_idle_ms;
    long memory_relief_requests = config->memory_relief_requests;
    const oasp_fastcgi_limits *limits = &config->limits;
    int process_count = config->workers;
    const char *cache_directory = config->cache_directory;
    fcgi_shared_state *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED | OASP_MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        return 1;
    atomic_init(&shared->accepted, 0);
    pid_t *processes = calloc((size_t)process_count, sizeof(*processes));
    if (!processes) {
        munmap(shared, sizeof(*shared));
        return 1;
    }
    oasp_shared_cache *cache = oasp_shared_cache_create(config->cache_capacity, cache_directory);
    if (!cache) {
        munmap(shared, sizeof(*shared));
        return 1;
    }
    oasp_shared_cache_set_validation_interval(cache, config->validation_interval);
    oasp_shared_cache_set_debug_info(cache, config->gen_debug_info);
    if (process_count == 1) {
        int result;
        do {
            result = run_worker_process(listener, root, max_accepts, max_requests, memory_idle_ms, memory_relief_requests,
                                        limits, shared, cache, cache_directory,
                                        config->debug_enabled ? config->debug_unix_path : NULL, config);
        } while (running && result == OASP_WORKER_RECYCLE && (!max_accepts || atomic_load_explicit(&shared->accepted, memory_order_relaxed) < max_accepts));
        munmap(shared, sizeof(*shared));
        oasp_shared_cache_destroy(cache);
        return result == OASP_WORKER_RECYCLE ? 0 : result;
    }
    int started = 0;
    for (; started < process_count; started++) {
        pid_t child = fork();
        if (child < 0)
            break;
        if (child == 0) {
            int result =
                run_worker_process(listener, root, max_accepts, max_requests, memory_idle_ms, memory_relief_requests,
                                   limits, shared, cache, cache_directory,
                                   config->debug_enabled ? config->debug_unix_path : NULL, config);
            close(listener);
            _exit(result);
        }
        processes[started] = child;
    }
    int result = started == process_count ? 0 : 1;
    int remaining = started;
    unsigned restart_failures = 0;
    long failure_accepted = -1;
    if (result) {
        for (int i = 0; i < started; i++)
            if (processes[i] > 0)
                (void)kill(processes[i], SIGTERM);
    }
    while (remaining > 0) {
        int status = 0;
        pid_t child = waitpid(-1, &status, 0);
        if (child < 0) {
            if (errno == EINTR) {
                if (!running)
                    break;
                continue;
            }
            result = 1;
            break;
        }
        int slot = -1;
        for (int i = 0; i < started; i++)
            if (processes[i] == child) {
                slot = i;
                processes[i] = 0;
                break;
            }
        oasp_shared_cache_recover_process(cache, (int64_t)child);
        if (config->debug_enabled) {
            char debug_agent_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
            int path_length = snprintf(debug_agent_path, sizeof(debug_agent_path), "%s.%ld",
                                       config->debug_unix_path, (long)child);
            if (path_length > 0 && (size_t)path_length < sizeof(debug_agent_path))
                (void)unlink(debug_agent_path);
        }
        remaining--;
        bool recycle = WIFEXITED(status) && WEXITSTATUS(status) == OASP_WORKER_RECYCLE;
        bool clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!recycle && !clean_exit) {
            if (WIFSIGNALED(status))
                oasp_log_write(OASP_LOG_ERROR, "worker %ld terminated by signal %d; restarting",
                               (long)child, WTERMSIG(status));
            else
                oasp_log_write(OASP_LOG_ERROR, "worker %ld exited with status %d; restarting",
                               (long)child, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
        long accepted = atomic_load_explicit(&shared->accepted, memory_order_relaxed);
        if (!running || result || (max_accepts && accepted >= max_accepts))
            break;
        if (slot >= 0) {
            if (!recycle && !clean_exit) {
                if (accepted != failure_accepted)
                    restart_failures = 0;
                failure_accepted = accepted;
                restart_failures++;
                delay_worker_restart(restart_failures);
                if (!running)
                    break;
            } else {
                restart_failures = 0;
                failure_accepted = accepted;
            }
            pid_t replacement = fork();
            if (replacement == 0) {
                int worker_result =
                    run_worker_process(listener, root, max_accepts, max_requests, memory_idle_ms,
                                       memory_relief_requests, limits, shared, cache, cache_directory,
                                       config->debug_enabled ? config->debug_unix_path : NULL, config);
                close(listener);
                _exit(worker_result);
            }
            if (replacement < 0) {
                result = 1;
                break;
            }
            processes[slot] = replacement;
            remaining++;
        }
    }
    for (int i = 0; i < started; i++)
        if (processes[i] > 0)
            (void)kill(processes[i], SIGTERM);
    while (remaining > 0) {
        pid_t child = waitpid(-1, NULL, 0);
        if (child > 0)
            remaining--;
        else if (child < 0 && errno != EINTR)
            break;
    }
    free(processes);
    munmap(shared, sizeof(*shared));
    oasp_shared_cache_destroy(cache);
    return result;
}

/* Validates configuration, owns the listener lifecycle, and drives the worker pool. */
int oasp_fastcgi_run(const oasp_fastcgi_config *config) {
    if (!config || !config->root || !config->cache_directory || config->workers < 1 ||
        (config->debug_enabled && (!config->debug_unix_path || !*config->debug_unix_path)))
        return 2;
    if (config->debug_tcp_port &&
        (!config->debug_tcp_host || strcmp(config->debug_tcp_host, "127.0.0.1") ||
         !config->debug_token || !*config->debug_token)) {
        oasp_log_write(OASP_LOG_ERROR, "debug TCP must bind 127.0.0.1 and use a non-empty token");
        return 2;
    }
    running = 1;
    int listener = config->unix_path ? listen_unix(config->unix_path)
                                     : listen_tcp(config->host, config->port);
    if (listener < 0) {
        oasp_log_write(OASP_LOG_ERROR, "cannot create listener: %s", strerror(errno));
        return 2;
    }
    server_listener = listener;
    if (!install_signal_handler(SIGINT) || !install_signal_handler(SIGTERM)) {
        oasp_log_write(OASP_LOG_ERROR, "cannot install signal handlers: %s", strerror(errno));
        close(listener);
        if (config->unix_path)
            (void)unlink(config->unix_path);
        return 2;
    }
    if (config->unix_path)
        oasp_log_write(OASP_LOG_INFO, "fastcgi listening on unix:%s workers=%d",
                       config->unix_path, config->workers);
    else
        oasp_log_write(OASP_LOG_INFO, "fastcgi listening on %s:%d workers=%d",
                       config->host, config->port, config->workers);
    if (config->workers > 256) {
        oasp_log_write(OASP_LOG_ERROR, "workers exceeds the hard process limit of 256");
        close(listener);
        if (config->unix_path)
            (void)unlink(config->unix_path);
        return 2;
    }
    int debug_unix_listener = -1;
    int debug_tcp_listener = -1;
    pid_t debug_broker = -1;
    if (config->debug_enabled) {
        debug_unix_listener = listen_unix(config->debug_unix_path);
        if (debug_unix_listener < 0 || chmod(config->debug_unix_path, 0600)) {
            oasp_log_write(OASP_LOG_ERROR, "cannot create debugger Unix broker: %s", strerror(errno));
            if (debug_unix_listener >= 0)
                close(debug_unix_listener);
            close(listener);
            if (config->unix_path)
                (void)unlink(config->unix_path);
            return 2;
        }
        if (config->debug_tcp_port) {
            debug_tcp_listener = listen_tcp(config->debug_tcp_host, config->debug_tcp_port);
            if (debug_tcp_listener < 0) {
                oasp_log_write(OASP_LOG_ERROR, "cannot create debugger TCP broker: %s", strerror(errno));
                close(debug_unix_listener);
                (void)unlink(config->debug_unix_path);
                close(listener);
                if (config->unix_path)
                    (void)unlink(config->unix_path);
                return 2;
            }
        }
        debug_broker = debug_broker_start(debug_unix_listener, debug_tcp_listener, config->debug_token,
                                          config->debug_unix_path);
        if (debug_broker < 0) {
            close(debug_unix_listener);
            if (debug_tcp_listener >= 0)
                close(debug_tcp_listener);
            (void)unlink(config->debug_unix_path);
            close(listener);
            if (config->unix_path)
                (void)unlink(config->unix_path);
            return 2;
        }
        oasp_log_write(OASP_LOG_NOTICE, "debug broker listening on unix:%s", config->debug_unix_path);
    }
    int result = run_worker_pool(listener, config);
    if (debug_broker > 0) {
        (void)kill(debug_broker, SIGTERM);
        (void)waitpid(debug_broker, NULL, 0);
    }
    if (debug_unix_listener >= 0)
        close(debug_unix_listener);
    if (debug_tcp_listener >= 0)
        close(debug_tcp_listener);
    if (config->debug_enabled)
        (void)unlink(config->debug_unix_path);
    if (server_listener >= 0)
        close(listener);
    server_listener = -1;
    if (config->unix_path)
        (void)unlink(config->unix_path);
    return result;
}
