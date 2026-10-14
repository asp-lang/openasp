/*
 * Copyright (c) 2026 https://asp-lang.org
 * SPDX-License-Identifier: MIT
 *
 * Native FastCGI load generator for long-running FPM benchmarks. It opens one
 * Unix-domain connection per request so it exercises the same request lifecycle
 * as the Python benchmark driver without making the Python GIL the throughput
 * ceiling.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    FCGI_BEGIN_REQUEST = 1,
    FCGI_END_REQUEST = 3,
    FCGI_PARAMS = 4,
    FCGI_STDIN = 5,
    FCGI_STDOUT = 6,
    FCGI_RESPONDER = 1,
    FCGI_HEADER_SIZE = 8,
    FCGI_BEGIN_BODY_SIZE = 8,
    RESPONSE_HEADER_LIMIT = 8192
};

typedef struct {
    const char *socket_path;
    const char *root;
    const char *cookie;
    const char **paths;
    size_t path_count;
    unsigned long long requests;
    unsigned int concurrency;
    atomic_ullong next_request;
    atomic_ullong successful_requests;
    atomic_ullong received_bytes;
    atomic_int failed;
    char error[256];
    pthread_mutex_t error_lock;
} load_context;

typedef struct {
    load_context *context;
    unsigned int index;
} worker_context;

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --socket PATH --root PATH --cookie NAME=VALUE --requests N --concurrency N "
            "--path PATH [--path PATH ...]\n",
            program);
}

static int parse_unsigned_long_long(const char *text, unsigned long long *value) {
    char *end = NULL;
    unsigned long long parsed;
    if (!text || !*text)
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !end || *end)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_unsigned_int(const char *text, unsigned int *value) {
    unsigned long long parsed;
    if (parse_unsigned_long_long(text, &parsed) || parsed == 0 || parsed > UINT32_MAX)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

static double now_seconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void set_error(load_context *context, const char *message) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&context->failed, &expected, 1))
        return;
    pthread_mutex_lock(&context->error_lock);
    snprintf(context->error, sizeof(context->error), "%s", message);
    pthread_mutex_unlock(&context->error_lock);
}

static int send_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length) {
        ssize_t written = send(fd, cursor, length, 0);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0)
            return -1;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int receive_exact(int fd, void *data, size_t length) {
    unsigned char *cursor = data;
    while (length) {
        ssize_t received = recv(fd, cursor, length, 0);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (received == 0)
            return -1;
        cursor += (size_t)received;
        length -= (size_t)received;
    }
    return 0;
}

static int send_record(int fd, unsigned char kind, const void *content, uint16_t length) {
    unsigned char header[FCGI_HEADER_SIZE] = {
        1, kind, 0, 1, (unsigned char)(length >> 8), (unsigned char)length, 0, 0
    };
    return send_all(fd, header, sizeof(header)) ||
           (length && send_all(fd, content, length));
}

static int append_length(unsigned char **cursor, unsigned char *end, size_t length) {
    if (length < 128) {
        if (*cursor == end)
            return -1;
        *(*cursor)++ = (unsigned char)length;
        return 0;
    }
    if ((size_t)(end - *cursor) < 4 || length > 0x7fffffffU)
        return -1;
    *(*cursor)++ = (unsigned char)(0x80U | (length >> 24));
    *(*cursor)++ = (unsigned char)(length >> 16);
    *(*cursor)++ = (unsigned char)(length >> 8);
    *(*cursor)++ = (unsigned char)length;
    return 0;
}

static int append_parameter(unsigned char **cursor, unsigned char *end, const char *name, const char *value) {
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    if (append_length(cursor, end, name_length) || append_length(cursor, end, value_length) ||
        (size_t)(end - *cursor) < name_length + value_length)
        return -1;
    memcpy(*cursor, name, name_length);
    *cursor += name_length;
    memcpy(*cursor, value, value_length);
    *cursor += value_length;
    return 0;
}

static int response_is_success(const unsigned char *headers, size_t length) {
    static const char content_type[] = "content-type:";
    static const char status[] = "status: 2";
    size_t index;
    for (index = 0; index + sizeof(status) - 1 <= length; index++) {
        if (!strncasecmp((const char *)headers + index, status, sizeof(status) - 1))
            return 1;
    }
    for (index = 0; index + sizeof(content_type) - 1 <= length; index++) {
        if (!strncasecmp((const char *)headers + index, content_type, sizeof(content_type) - 1))
            return 1;
    }
    return 0;
}

static int open_socket(const char *path) {
    struct sockaddr_un address;
    int fd;
    size_t length = strlen(path);
    if (length >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, length + 1);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address))) {
        close(fd);
        return -1;
    }
    return fd;
}

static int transact(load_context *context, unsigned long long request_index, unsigned int thread_index, unsigned long long *received_bytes) {
    unsigned char parameters[4096];
    unsigned char begin[FCGI_BEGIN_BODY_SIZE] = {0, FCGI_RESPONDER, 0, 0, 0, 0, 0, 0};
    unsigned char header[FCGI_HEADER_SIZE];
    unsigned char content[65535];
    unsigned char response_headers[RESPONSE_HEADER_LIMIT];
    unsigned char *cursor = parameters;
    unsigned char *end = parameters + sizeof(parameters);
    char script_filename[2048];
    char script_name[1024];
    char request_uri[1200];
    char query[512] = "";
    char cookie[128];
    const char *path = context->paths[request_index % context->path_count];
    const char *question = strchr(path, '?');
    size_t path_length = question ? (size_t)(question - path) : strlen(path);
    size_t response_header_length = 0;
    int fd;
    int complete = 0;

    if (!path_length || path_length >= sizeof(script_name))
        return -1;
    memcpy(script_name, path, path_length);
    script_name[path_length] = '\0';
    if (question)
        snprintf(query, sizeof(query), "%s", question + 1);
    if (snprintf(script_filename, sizeof(script_filename), "%s/%s", context->root, script_name) >=
        (int)sizeof(script_filename))
        return -1;
    if (snprintf(request_uri, sizeof(request_uri), "/%s%s%s", script_name, question ? "?" : "", query) >=
        (int)sizeof(request_uri))
        return -1;
    if (snprintf(cookie, sizeof(cookie), "%s-%u", context->cookie, thread_index) >= (int)sizeof(cookie))
        return -1;

    if (append_parameter(&cursor, end, "SCRIPT_FILENAME", script_filename) ||
        append_parameter(&cursor, end, "SCRIPT_NAME", request_uri) ||
        append_parameter(&cursor, end, "REQUEST_METHOD", "GET") ||
        append_parameter(&cursor, end, "QUERY_STRING", query) ||
        append_parameter(&cursor, end, "REQUEST_URI", request_uri) ||
        append_parameter(&cursor, end, "DOCUMENT_ROOT", context->root) ||
        append_parameter(&cursor, end, "SERVER_PROTOCOL", "HTTP/1.1") ||
        append_parameter(&cursor, end, "GATEWAY_INTERFACE", "CGI/1.1") ||
        append_parameter(&cursor, end, "SERVER_SOFTWARE", "openasp-native-benchmark") ||
        append_parameter(&cursor, end, "SERVER_NAME", "localhost") ||
        append_parameter(&cursor, end, "SERVER_PORT", "80") ||
        append_parameter(&cursor, end, "REDIRECT_STATUS", "200") ||
        append_parameter(&cursor, end, "HTTP_COOKIE", cookie))
        return -1;

    fd = open_socket(context->socket_path);
    if (fd < 0)
        return -1;
    if (send_record(fd, FCGI_BEGIN_REQUEST, begin, sizeof(begin)) ||
        send_record(fd, FCGI_PARAMS, parameters, (uint16_t)(cursor - parameters)) ||
        send_record(fd, FCGI_PARAMS, NULL, 0) || send_record(fd, FCGI_STDIN, NULL, 0))
        goto failure;
    while (!complete) {
        uint16_t length;
        unsigned char padding;
        if (receive_exact(fd, header, sizeof(header)) || header[0] != 1 || header[2] != 0 || header[3] != 1)
            goto failure;
        length = (uint16_t)(((uint16_t)header[4] << 8) | header[5]);
        padding = header[6];
        if (length && receive_exact(fd, content, length))
            goto failure;
        if (padding) {
            unsigned char discarded[256];
            if (receive_exact(fd, discarded, padding))
                goto failure;
        }
        if (header[1] == FCGI_STDOUT) {
            size_t copied = length;
            if (copied > RESPONSE_HEADER_LIMIT - response_header_length)
                copied = RESPONSE_HEADER_LIMIT - response_header_length;
            if (copied) {
                memcpy(response_headers + response_header_length, content, copied);
                response_header_length += copied;
            }
            *received_bytes += length;
        } else if (header[1] == FCGI_END_REQUEST) {
            complete = 1;
        }
    }
    close(fd);
    return response_is_success(response_headers, response_header_length) ? 0 : -1;

failure:
    close(fd);
    return -1;
}

static void *run_worker(void *argument) {
    worker_context *worker = argument;
    load_context *context = worker->context;
    for (;;) {
        unsigned long long index = atomic_fetch_add(&context->next_request, 1);
        unsigned long long bytes = 0;
        if (index >= context->requests || atomic_load(&context->failed))
            return NULL;
        if (transact(context, index, worker->index, &bytes)) {
            set_error(context, "FastCGI request failed or returned a non-success response");
            return NULL;
        }
        atomic_fetch_add(&context->successful_requests, 1);
        atomic_fetch_add(&context->received_bytes, bytes);
    }
}

int main(int argc, char **argv) {
    load_context context = {0};
    pthread_t *threads = NULL;
    worker_context *workers = NULL;
    double started;
    double elapsed;
    int index;

    pthread_mutex_init(&context.error_lock, NULL);
    for (index = 1; index < argc; index++) {
        const char *value;
        if (!strcmp(argv[index], "--socket") || !strcmp(argv[index], "--root") || !strcmp(argv[index], "--cookie") ||
            !strcmp(argv[index], "--requests") || !strcmp(argv[index], "--concurrency") ||
            !strcmp(argv[index], "--path")) {
            if (++index == argc) {
                usage(argv[0]);
                return 2;
            }
            value = argv[index];
            if (!strcmp(argv[index - 1], "--socket"))
                context.socket_path = value;
            else if (!strcmp(argv[index - 1], "--root"))
                context.root = value;
            else if (!strcmp(argv[index - 1], "--cookie"))
                context.cookie = value;
            else if (!strcmp(argv[index - 1], "--requests")) {
                if (parse_unsigned_long_long(value, &context.requests) || !context.requests)
                    return 2;
            } else if (!strcmp(argv[index - 1], "--concurrency")) {
                if (parse_unsigned_int(value, &context.concurrency))
                    return 2;
            } else {
                const char **paths = realloc(context.paths, (context.path_count + 1) * sizeof(const char *));
                if (!paths)
                    return 1;
                context.paths = paths;
                context.paths[context.path_count++] = value;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!context.socket_path || !context.root || !context.cookie || !context.requests ||
        !context.concurrency || !context.path_count) {
        usage(argv[0]);
        return 2;
    }
    threads = calloc(context.concurrency, sizeof(*threads));
    workers = calloc(context.concurrency, sizeof(*workers));
    if (!threads || !workers)
        return 1;
    started = now_seconds();
    for (index = 0; index < (int)context.concurrency; index++) {
        workers[index].context = &context;
        workers[index].index = (unsigned int)index;
        if (pthread_create(&threads[index], NULL, run_worker, &workers[index])) {
            set_error(&context, "unable to create load generator thread");
            context.concurrency = (unsigned int)index;
            break;
        }
    }
    for (index = 0; index < (int)context.concurrency; index++)
        pthread_join(threads[index], NULL);
    elapsed = now_seconds() - started;
    if (atomic_load(&context.failed) || atomic_load(&context.successful_requests) != context.requests) {
        fprintf(stderr, "fastcgi-load failed after %llu requests: %s\n",
                atomic_load(&context.successful_requests), context.error[0] ? context.error : "incomplete run");
        return 1;
    }
    printf("{\"requests\":%llu,\"successful_requests\":%llu,\"concurrency\":%u,"
           "\"elapsed_seconds\":%.9f,\"requests_per_second\":%.6f,\"bytes_received\":%llu}\n",
           context.requests, atomic_load(&context.successful_requests), context.concurrency, elapsed,
           (double)context.requests / elapsed, atomic_load(&context.received_bytes));
    free(workers);
    free(threads);
    free(context.paths);
    pthread_mutex_destroy(&context.error_lock);
    return 0;
}
