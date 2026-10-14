// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/web/request.c`): HTTP/FastCGI transport. It converts bounded wire
 * records into request-owned data, streams response records, and supervises workers. Descriptor
 * and length variables are validated at system-call boundaries to keep malformed clients
 * isolated.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _XOPEN_SOURCE 700

#include "openasp.h"
#include "memory.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef struct oasp_response_header {
    struct oasp_response_header *next;
    char *name;
    char *value;
} oasp_response_header;

typedef struct oasp_response_cookie {
    struct oasp_response_cookie *next;
    char *name;
    char *value;
    char *path;
    char *domain;
    char *expires;
    bool secure;
    bool http_only;
} oasp_response_cookie;

typedef struct oasp_server_variable {
    struct oasp_server_variable *next;
    char *name;
    char *value;
} oasp_server_variable;

enum { OASP_RESPONSE_CHUNK_SIZE = 4096 };

typedef struct oasp_response_chunk {
    struct oasp_response_chunk *next;
    size_t length;
    char data[OASP_RESPONSE_CHUNK_SIZE + 1];
} oasp_response_chunk;

struct oasp_request {
    oasp_arena *arena;
    oasp_arena *response_arena;
    oasp_arena *scratch_arena;
    oasp_arena_checkpoint arena_checkpoint;
    oasp_arena_checkpoint response_checkpoint;
    oasp_arena_checkpoint scratch_checkpoint;
    char *root;
    char *path;
    char *query;
    char *body;
    size_t body_length;
    char *method;
    char *cookie_header;
    oasp_response_chunk *output;
    oasp_response_chunk *output_tail;
    size_t output_length;
    size_t max_buffered_response_bytes;
    oasp_response_sink_fn output_sink;
    void *output_sink_context;
    bool buffered;
    bool output_committed;
    bool output_error;
    bool binary_started;
    char *status;
    char *content_type;
    char *charset;
    oasp_response_header *headers;
    oasp_response_header **header_tail;
    size_t header_count;
    size_t response_metadata_bytes;
    size_t max_response_header_bytes;
    size_t max_response_header_count;
    oasp_response_cookie *cookies;
    oasp_server_variable *server_variables;
    struct timespec deadline;
    bool has_deadline;
    const char *request_error;
    oasp_memory_manager memory;
    bool memory_enabled;
    bool owns_arenas;
};

static char *oasp_arena_strdup(oasp_arena *arena, const char *value) {
    if (!value)
        value = "";
    size_t length = strlen(value);
    char *copy = oasp_arena_alloc(arena, length + 1, _Alignof(char));
    if (!copy)
        return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

oasp_request *oasp_request_create_with_arenas(const char *root, const char *query, const char *body, const char *method, oasp_arena *arena,
                                              oasp_arena *response_arena, oasp_arena *scratch_arena) {
    if (!arena || !response_arena || !scratch_arena)
        return NULL;
    oasp_arena_checkpoint arena_checkpoint = oasp_arena_checkpoint_save(arena);
    oasp_arena_checkpoint response_checkpoint = oasp_arena_checkpoint_save(response_arena);
    oasp_arena_checkpoint scratch_checkpoint = oasp_arena_checkpoint_save(scratch_arena);
    oasp_request *request = oasp_arena_alloc(arena, sizeof(*request), _Alignof(oasp_request));
    if (!request)
        return NULL;
    memset(request, 0, sizeof(*request));
    request->arena = arena;
    request->response_arena = response_arena;
    request->scratch_arena = scratch_arena;
    request->arena_checkpoint = arena_checkpoint;
    request->response_checkpoint = response_checkpoint;
    request->scratch_checkpoint = scratch_checkpoint;
    char canonical_root[PATH_MAX];
    const char *resolved_root = realpath(root ? root : ".", canonical_root) ? canonical_root : (root ? root : ".");
    request->root = oasp_arena_strdup(arena, resolved_root);
    request->path = oasp_arena_strdup(arena, "");
    request->query = oasp_arena_strdup(arena, query);
    request->body = oasp_arena_strdup(arena, body);
    request->body_length = body ? strlen(body) : 0;
    request->method = oasp_arena_strdup(arena, method ? method : "GET");
    request->cookie_header = oasp_arena_strdup(arena, getenv("HTTP_COOKIE"));
    request->status = oasp_arena_strdup(arena, "200 OK");
    request->content_type = oasp_arena_strdup(arena, "text/html");
    request->buffered = true;
    request->header_tail = &request->headers;
    if (!request->root || !request->path || !request->query || !request->body || !request->method ||
        !request->cookie_header || !request->status || !request->content_type) {
        oasp_arena_checkpoint_restore(scratch_arena, scratch_checkpoint);
        oasp_arena_checkpoint_restore(response_arena, response_checkpoint);
        oasp_arena_checkpoint_restore(arena, arena_checkpoint);
        return NULL;
    }
    return request;
}

oasp_request *oasp_request_create(const char *root, const char *query, const char *body, const char *method) {
    oasp_arena *arena = oasp_arena_create_mapped(16 * 1024);
    oasp_arena *response_arena = oasp_arena_create_mapped(16 * 1024);
    oasp_arena *scratch_arena = oasp_arena_create_mapped(64 * 1024);
    oasp_request *request = oasp_request_create_with_arenas(root, query, body, method, arena, response_arena, scratch_arena);
    if (!request) {
        oasp_arena_destroy(scratch_arena);
        oasp_arena_destroy(response_arena);
        oasp_arena_destroy(arena);
        return NULL;
    }
    request->owns_arenas = true;
    return request;
}

void oasp_request_destroy(oasp_request *request) {
    if (!request)
        return;
    oasp_arena *arena = request->arena;
    oasp_arena *response_arena = request->response_arena;
    oasp_arena *scratch_arena = request->scratch_arena;
    oasp_arena_checkpoint arena_checkpoint = request->arena_checkpoint;
    oasp_arena_checkpoint response_checkpoint = request->response_checkpoint;
    oasp_arena_checkpoint scratch_checkpoint = request->scratch_checkpoint;
    bool owns_arenas = request->owns_arenas;
    oasp_arena_unbind_memory(scratch_arena);
    oasp_arena_unbind_memory(response_arena);
    oasp_arena_unbind_memory(arena);
    if (owns_arenas) {
        oasp_arena_destroy(scratch_arena);
        oasp_arena_destroy(response_arena);
        oasp_arena_destroy(arena);
    } else {
        oasp_arena_checkpoint_restore(scratch_arena, scratch_checkpoint);
        oasp_arena_checkpoint_restore(response_arena, response_checkpoint);
        oasp_arena_checkpoint_restore(arena, arena_checkpoint);
    }
}

oasp_arena *oasp_request_arena(oasp_request *request) { return request ? request->arena : NULL; }
oasp_arena *oasp_request_scratch_arena(oasp_request *request) { return request ? request->scratch_arena : NULL; }
int oasp_request_set_server_variable(oasp_request *request, const char *name, const char *value) {
    if (!request || !name || !value)
        return -1;
    oasp_server_variable *variable = request->server_variables;
    while (variable && strcasecmp(variable->name, name))
        variable = variable->next;
    if (!variable) {
        variable = oasp_arena_alloc(request->arena, sizeof(*variable), _Alignof(oasp_server_variable));
        if (!variable)
            return -1;
        variable->name = oasp_arena_strdup(request->arena, name);
        if (!variable->name)
            return -1;
        variable->next = request->server_variables;
        request->server_variables = variable;
    }
    variable->value = oasp_arena_strdup(request->arena, value);
    if (!variable->value)
        return -1;
    if (!strcasecmp(name, "HTTP_COOKIE")) {
        char *cookie = oasp_arena_strdup(request->arena, value);
        if (!cookie)
            return -1;
        request->cookie_header = cookie;
    }
    return 0;
}

const char *oasp_request_server_variable(const oasp_request *request, const char *name) {
    if (!request || !name)
        return NULL;
    for (const oasp_server_variable *variable = request->server_variables; variable; variable = variable->next)
        if (!strcasecmp(variable->name, name))
            return variable->value;
    return getenv(name);
}

const char *oasp_request_root(const oasp_request *request) { return request->root; }
const char *oasp_request_path(const oasp_request *request) { return request ? request->path : ""; }
void oasp_request_set_path(oasp_request *request, const char *path) {
    if (!request)
        return;
    char canonical[PATH_MAX];
    const char *resolved = path && realpath(path, canonical) ? canonical : (path ? path : "");
    char *copy = oasp_arena_strdup(request->arena, resolved);
    if (!copy)
        return;
    request->path = copy;
}
const char *oasp_request_method(const oasp_request *request) { return request->method; }
const char *oasp_request_query_string(const oasp_request *request) { return request ? request->query : ""; }
const char *oasp_request_body(const oasp_request *request) { return request ? request->body : ""; }
size_t oasp_request_body_length(const oasp_request *request) { return request ? request->body_length : 0; }

int oasp_request_set_limits(oasp_request *request, const oasp_request_limits *limits) {
    if (!request || !limits)
        return -1;
    request->max_buffered_response_bytes = limits->max_buffered_response_bytes;
    request->max_response_header_bytes = limits->max_response_header_bytes;
    request->max_response_header_count = limits->max_response_header_count;
    oasp_arena_unbind_memory(request->scratch_arena);
    oasp_arena_unbind_memory(request->response_arena);
    oasp_arena_unbind_memory(request->arena);
    request->memory_enabled = false;
    if (limits->memory_limit) {
        oasp_memory_manager_init(&request->memory, limits->memory_limit);
        if (!oasp_arena_bind_memory(request->arena, &request->memory) ||
            !oasp_arena_bind_memory(request->response_arena, &request->memory) ||
            !oasp_arena_bind_memory(request->scratch_arena, &request->memory)) {
            oasp_arena_unbind_memory(request->scratch_arena);
            oasp_arena_unbind_memory(request->response_arena);
            oasp_arena_unbind_memory(request->arena);
            request->request_error = oasp_memory_error(&request->memory);
            return -1;
        }
        request->memory_enabled = true;
    }
    request->has_deadline = limits->request_timeout_ms != 0;
    if (request->has_deadline) {
        if (clock_gettime(CLOCK_MONOTONIC, &request->deadline))
            return -1;
        unsigned long seconds = limits->request_timeout_ms / 1000;
        unsigned long nanoseconds = (limits->request_timeout_ms % 1000) * 1000000ul;
        if (seconds > (unsigned long)(INT64_MAX - request->deadline.tv_sec))
            return -1;
        request->deadline.tv_sec += (time_t)seconds;
        request->deadline.tv_nsec += (long)nanoseconds;
        if (request->deadline.tv_nsec >= 1000000000L) {
            request->deadline.tv_sec++;
            request->deadline.tv_nsec -= 1000000000L;
        }
    }
    return 0;
}

int oasp_request_deadline_expired(const oasp_request *request) {
    if (!request || !request->has_deadline)
        return 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return now.tv_sec > request->deadline.tv_sec ||
           (now.tv_sec == request->deadline.tv_sec && now.tv_nsec >= request->deadline.tv_nsec);
}

int oasp_request_failed(const oasp_request *request) {
    return request && (request->request_error ||
                       (request->memory_enabled && request->memory.allocation_failures));
}

const char *oasp_request_error(const oasp_request *request) {
    if (!request)
        return "request operation failed";
    const char *memory_error = request->memory_enabled
                                   ? oasp_memory_error(&request->memory)
                                   : NULL;
    return memory_error ? memory_error
                        : request->request_error ? request->request_error : "request operation failed";
}

size_t oasp_request_memory_usage(const oasp_request *request) {
    return request && request->memory_enabled ? request->memory.current : 0;
}

size_t oasp_request_peak_memory_usage(const oasp_request *request) {
    return request && request->memory_enabled ? request->memory.peak : 0;
}

oasp_memory_manager *oasp_request_memory_manager(oasp_request *request) {
    return request && request->memory_enabled ? &request->memory : NULL;
}

int oasp_request_set_body(oasp_request *request, const void *body, size_t length) {
    if (!request || (!body && length))
        return -1;
    char *copy = oasp_arena_alloc(request->arena, length + 1, _Alignof(char));
    if (!copy)
        return -1;
    if (length)
        memcpy(copy, body, length);
    copy[length] = '\0';
    request->body = copy;
    request->body_length = length;
    return 0;
}

static int oasp_hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static const char *oasp_request_parameter(const oasp_request *request, const char *parameters, const char *name, bool *found) {
    if (found)
        *found = false;
    size_t name_length = strlen(name);
    const char *cursor = parameters;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length > name_length && cursor[name_length] == '=' &&
            !strncasecmp(cursor, name, name_length)) {
            char *value = oasp_arena_alloc(request->arena, length - name_length, _Alignof(char));
            if (!value)
                return NULL;
            size_t out = 0;
            for (size_t i = name_length + 1; i < length; i++) {
                /*
                 * Legacy IE clients submit JavaScript escape() Unicode units as
                 * %uXXXX. Preserve those units for ASP Unescape instead of
                 * consuming "%u" as a malformed byte escape.
                 */
                if (cursor[i] == '%' && i + 5 < length && cursor[i + 1] == 'u' &&
                    oasp_hex(cursor[i + 2]) >= 0 && oasp_hex(cursor[i + 3]) >= 0 &&
                    oasp_hex(cursor[i + 4]) >= 0 && oasp_hex(cursor[i + 5]) >= 0) {
                    memcpy(value + out, cursor + i, 6);
                    out += 6;
                    i += 5;
                    continue;
                }
                if (cursor[i] == '%' && i + 2 < length) {
                    int high = oasp_hex(cursor[++i]);
                    int low = oasp_hex(cursor[++i]);
                    value[out++] = high >= 0 && low >= 0 ? (char)(high * 16 + low) : '%';
                } else {
                    value[out++] = cursor[i] == '+' ? ' ' : cursor[i];
                }
            }
            value[out] = '\0';
            if (found)
                *found = true;
            return value;
        }
        cursor = end ? end + 1 : cursor + length;
    }
    return "";
}

const char *oasp_request_query(const oasp_request *request, const char *name) { return oasp_request_parameter(request, request->query, name, NULL); }

const char *oasp_request_form(const oasp_request *request, const char *name) { return oasp_request_parameter(request, request->body, name, NULL); }

int oasp_request_query_has(const oasp_request *request, const char *name) {
    bool found = false;
    (void)oasp_request_parameter(request, request->query, name, &found);
    return found;
}

int oasp_request_form_has(const oasp_request *request, const char *name) {
    bool found = false;
    (void)oasp_request_parameter(request, request->body, name, &found);
    return found;
}

static size_t oasp_request_parameter_count(const char *parameters) {
    size_t count = 0;
    const char *cursor = parameters ? parameters : "";
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length)
            count++;
        cursor = end ? end + 1 : cursor + length;
    }
    return count;
}

size_t oasp_request_query_count(const oasp_request *request) { return request ? oasp_request_parameter_count(request->query) : 0; }

size_t oasp_request_form_count(const oasp_request *request) { return request ? oasp_request_parameter_count(request->body) : 0; }

const char *oasp_request_cookie_value(const oasp_request *request, const char *name) {
    if (!request || !name)
        return "";
    size_t name_length = strlen(name);
    const char *cursor = request->cookie_header;
    while (*cursor) {
        while (*cursor == ';' || *cursor == ' ' || *cursor == '\t')
            cursor++;
        const char *end = strchr(cursor, ';');
        if (!end)
            end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        const char *name_end = equals ? equals : end;
        while (name_end > cursor && (name_end[-1] == ' ' || name_end[-1] == '\t'))
            name_end--;
        if (equals && (size_t)(name_end - cursor) == name_length && !memcmp(cursor, name, name_length)) {
            const char *value = equals + 1;
            while (value < end && (*value == ' ' || *value == '\t'))
                value++;
            const char *value_end = end;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
                value_end--;
            size_t length = (size_t)(value_end - value);
            char *copy = oasp_arena_alloc(request->arena, length + 1, _Alignof(char));
            if (!copy)
                return "";
            memcpy(copy, value, length);
            copy[length] = '\0';
            return copy;
        }
        cursor = *end ? end + 1 : end;
    }
    return "";
}

const char *oasp_request_cookie_subvalue(const oasp_request *request, const char *name, const char *subkey) {
    if (!request || !name || !subkey)
        return "";
    const char *cookie = oasp_request_cookie_value(request, name);
    size_t subkey_length = strlen(subkey);
    const char *cursor = cookie;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        if (!end)
            end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        if (equals && (size_t)(equals - cursor) == subkey_length && !strncasecmp(cursor, subkey, subkey_length)) {
            size_t length = (size_t)(end - equals - 1);
            char *copy = oasp_arena_alloc(request->arena, length + 1, _Alignof(char));
            if (!copy)
                return "";
            memcpy(copy, equals + 1, length);
            copy[length] = '\0';
            return copy;
        }
        cursor = *end ? end + 1 : end;
    }
    return "";
}

static bool oasp_response_output_ignorable(const oasp_request *request) {
    size_t length = 0;
    const char *output = oasp_request_output(request, &length);
    for (size_t i = 0; i < length;) {
        unsigned char byte = (unsigned char)output[i];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            i++;
            continue;
        }
        if (i + 2 < length && byte == 0xef && (unsigned char)output[i + 1] == 0xbb &&
            (unsigned char)output[i + 2] == 0xbf) {
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

static bool oasp_response_is_xml(const oasp_request *request) {
    const char *content_type = request ? request->content_type : NULL;
    if (!content_type)
        return false;
    size_t length = strcspn(content_type, ";");
    return (length == 8 && !strncasecmp(content_type, "text/xml", length)) ||
           (length == 15 && !strncasecmp(content_type, "application/xml", length)) ||
           (length > 4 && !strncasecmp(content_type + length - 4, "+xml", 4));
}

void oasp_request_write(oasp_request *request, const char *text, size_t length) {
    if (!request || !text || !length)
        return;
    if (request->output_length && length >= 5 && oasp_response_is_xml(request) &&
        oasp_response_output_ignorable(request) && !memcmp(text, "<?xml", 5))
        oasp_request_truncate_output(request, 0);
    if (length > SIZE_MAX - request->output_length ||
        (request->buffered && request->max_buffered_response_bytes &&
         (request->output_length > request->max_buffered_response_bytes ||
          length > request->max_buffered_response_bytes - request->output_length))) {
        request->output_error = true;
        request->request_error = "buffered response limit exceeded";
        return;
    }
    while (length) {
        if (!request->output_tail || request->output_tail->length == OASP_RESPONSE_CHUNK_SIZE) {
            oasp_response_chunk *chunk = oasp_arena_alloc(request->response_arena, sizeof(*chunk), _Alignof(oasp_response_chunk));
            if (!chunk) {
                request->output_error = true;
                request->request_error = "out of memory while buffering response";
                return;
            }
            memset(chunk, 0, sizeof(*chunk));
            if (request->output_tail)
                request->output_tail->next = chunk;
            else
                request->output = chunk;
            request->output_tail = chunk;
        }
        size_t available = OASP_RESPONSE_CHUNK_SIZE - request->output_tail->length;
        size_t copied = length < available ? length : available;
        memcpy(request->output_tail->data + request->output_tail->length, text, copied);
        request->output_tail->length += copied;
        request->output_tail->data[request->output_tail->length] = '\0';
        request->output_length += copied;
        text += copied;
        length -= copied;
    }
    if (!request->buffered && !oasp_response_output_ignorable(request))
        (void)oasp_request_flush(request);
}

void oasp_request_binary_write(oasp_request *request, const char *data, size_t length) {
    if (!request || !data || !length)
        return;
    if (!request->binary_started && strncasecmp(request->content_type, "text/", 5)) {
        request->binary_started = true;
        bool ignorable = true;
        size_t output_length = 0;
        const char *output = oasp_request_output(request, &output_length);
        for (size_t i = 0; i < output_length;) {
            unsigned char byte = (unsigned char)output[i];
            if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
                i++;
                continue;
            }
            if (i + 2 < output_length && byte == 0xef && (unsigned char)output[i + 1] == 0xbb &&
                (unsigned char)output[i + 2] == 0xbf) {
                i += 3;
                continue;
            }
            ignorable = false;
            break;
        }
        if (ignorable)
            oasp_request_truncate_output(request, 0);
    }
    oasp_request_write(request, data, length);
}

void oasp_request_set_output_sink(oasp_request *request, oasp_response_sink_fn sink, void *context) {
    if (!request || request->output_committed)
        return;
    request->output_sink = sink;
    request->output_sink_context = context;
}

int oasp_request_set_buffered(oasp_request *request, int buffered) {
    if (!request)
        return -1;
    if (!buffered && request->buffered && request->output_length) {
        if (oasp_response_output_ignorable(request))
            oasp_request_truncate_output(request, 0);
        else if (oasp_request_flush(request))
            return -1;
    }
    request->buffered = buffered != 0;
    return 0;
}

int oasp_request_buffered(const oasp_request *request) { return request && request->buffered; }

int oasp_request_flush(oasp_request *request) {
    if (!request || request->output_error)
        return -1;
    if (!request->output_sink)
        return 0;
    if (!request->output) {
        if (request->output_sink(request->output_sink_context, NULL, 0)) {
            request->output_error = true;
            request->request_error = "response stream write failed";
            return -1;
        }
    } else {
        for (const oasp_response_chunk *chunk = request->output; chunk; chunk = chunk->next)
            if (request->output_sink(request->output_sink_context, chunk->data, chunk->length)) {
                request->output_error = true;
                request->request_error = "response stream write failed";
                return -1;
            }
    }
    request->output_committed = true;
    oasp_arena_checkpoint_restore(request->response_arena, request->response_checkpoint);
    request->output = NULL;
    request->output_tail = NULL;
    request->output_length = 0;
    return 0;
}

int oasp_request_output_committed(const oasp_request *request) { return request && request->output_committed; }

const char *oasp_request_output(const oasp_request *request, size_t *length) {
    if (length)
        *length = request ? request->output_length : 0;
    if (!request || !request->output)
        return "";
    if (!request->output->next)
        return request->output->data;
    oasp_request *mutable_request = (oasp_request *)request;
    char *flattened = oasp_arena_alloc(mutable_request->response_arena, request->output_length + 1, _Alignof(char));
    if (!flattened)
        return "";
    size_t offset = 0;
    for (const oasp_response_chunk *chunk = request->output; chunk; chunk = chunk->next) {
        memcpy(flattened + offset, chunk->data, chunk->length);
        offset += chunk->length;
    }
    flattened[offset] = '\0';
    return flattened;
}

void oasp_request_truncate_output(oasp_request *request, size_t length) {
    if (!request || length > request->output_length)
        return;
    if (!length) {
        oasp_arena_checkpoint_restore(request->response_arena, request->response_checkpoint);
        request->output = NULL;
        request->output_tail = NULL;
        request->output_length = 0;
        return;
    }
    size_t retained = 0;
    oasp_response_chunk *chunk = request->output;
    while (chunk && retained + chunk->length < length) {
        retained += chunk->length;
        chunk = chunk->next;
    }
    if (!chunk)
        return;
    chunk->length = length - retained;
    chunk->data[chunk->length] = '\0';
    chunk->next = NULL;
    request->output_tail = chunk;
    request->output_length = length;
}

static int oasp_replace_string(oasp_request *request, char **destination, const char *value) {
    size_t length = strlen(value ? value : "") + 1;
    if (request->max_response_header_bytes &&
        (request->response_metadata_bytes > request->max_response_header_bytes ||
         length > request->max_response_header_bytes - request->response_metadata_bytes)) {
        request->request_error = "response header byte limit exceeded";
        return -1;
    }
    char *copy = oasp_arena_strdup(request->arena, value);
    if (!copy)
        return -1;
    request->response_metadata_bytes += length;
    *destination = copy;
    return 0;
}

int oasp_request_set_status(oasp_request *request, const char *status) {
    if (!request || request->output_committed || !status || !*status || strchr(status, '\r') || strchr(status, '\n'))
        return -1;
    return oasp_replace_string(request, &request->status, status);
}

const char *oasp_request_status(const oasp_request *request) { return request ? request->status : "500 Internal Server Error"; }

int oasp_request_set_content_type(oasp_request *request, const char *content_type) {
    if (!request || request->output_committed || !content_type || !*content_type || strchr(content_type, '\r') || strchr(content_type, '\n'))
        return -1;
    return oasp_replace_string(request, &request->content_type, content_type);
}

const char *oasp_request_content_type(const oasp_request *request) { return request ? request->content_type : "text/plain; charset=utf-8"; }

static bool oasp_is_charset_name(const char *value) {
    if (!value || !*value)
        return false;
    for (; *value; value++)
        if (!((*value >= 'a' && *value <= 'z') || (*value >= 'A' && *value <= 'Z') ||
              (*value >= '0' && *value <= '9') || *value == '-' || *value == '_' || *value == '.'))
            return false;
    return true;
}

static bool oasp_content_type_parameter_is_charset(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t'))
        start++;
    const char *name_end = start;
    while (name_end < end && *name_end != '=' && *name_end != ' ' && *name_end != '\t')
        name_end++;
    while (name_end < end && (*name_end == ' ' || *name_end == '\t'))
        name_end++;
    return name_end < end && *name_end == '=' && (size_t)(name_end - start) == 7 && !strncasecmp(start, "charset", 7);
}

int oasp_request_set_charset(oasp_request *request, const char *charset) {
    if (!request || request->output_committed || !oasp_is_charset_name(charset))
        return -1;
    const char *content_type = request->content_type;
    size_t content_length = strlen(content_type);
    size_t charset_length = strlen(charset);
    char *updated = malloc(content_length + charset_length + 11);
    if (!updated)
        return -1;
    size_t written = 0;
    const char *segment = content_type;
    const char *separator = strchr(segment, ';');
    size_t media_length = separator ? (size_t)(separator - segment) : content_length;
    memcpy(updated, segment, media_length);
    written = media_length;
    while (separator) {
        const char *next = strchr(separator + 1, ';');
        const char *end = next ? next : content_type + content_length;
        if (!oasp_content_type_parameter_is_charset(separator + 1, end)) {
            size_t parameter_length = (size_t)(end - separator);
            memcpy(updated + written, separator, parameter_length);
            written += parameter_length;
        }
        separator = next;
    }
    memcpy(updated + written, "; charset=", 10);
    written += 10;
    memcpy(updated + written, charset, charset_length + 1);
    int result = oasp_replace_string(request, &request->content_type, updated);
    if (!result)
        result = oasp_replace_string(request, &request->charset, charset);
    free(updated);
    return result;
}

const char *oasp_request_charset(const oasp_request *request) { return request && request->charset ? request->charset : ""; }

int oasp_request_set_codepage(oasp_request *request, int codepage) {
    const char *charset;
    switch (codepage) {
    case 65001:
        charset = "utf-8";
        break;
    case 936:
        charset = "gb2312";
        break;
    case 950:
        charset = "big5";
        break;
    case 932:
        charset = "shift_jis";
        break;
    case 949:
        charset = "euc-kr";
        break;
    case 1252:
        charset = "windows-1252";
        break;
    default:
        return -1;
    }
    return oasp_request_set_charset(request, charset);
}

int oasp_request_add_header(oasp_request *request, const char *name, const char *value) {
    if (!request || request->output_committed || !name || !*name || strchr(name, '\r') || strchr(name, '\n') ||
        (value && (strchr(value, '\r') || strchr(value, '\n'))))
        return -1;
    size_t name_length = strlen(name) + 1;
    size_t value_length = strlen(value ? value : "") + 1;
    size_t allocation = sizeof(oasp_response_header);
    if (name_length > SIZE_MAX - allocation || value_length > SIZE_MAX - allocation - name_length ||
        (request->max_response_header_count && request->header_count >= request->max_response_header_count) ||
        (request->max_response_header_bytes &&
         (request->response_metadata_bytes > request->max_response_header_bytes ||
          allocation + name_length + value_length >
              request->max_response_header_bytes - request->response_metadata_bytes))) {
        request->request_error = request->max_response_header_count &&
                                         request->header_count >= request->max_response_header_count
                                     ? "response header count limit exceeded"
                                     : "response header byte limit exceeded";
        return -1;
    }
    oasp_response_header *header = oasp_arena_alloc(request->arena, sizeof(*header), _Alignof(oasp_response_header));
    if (!header)
        return -1;
    memset(header, 0, sizeof(*header));
    header->name = oasp_arena_strdup(request->arena, name);
    header->value = oasp_arena_strdup(request->arena, value);
    if (!header->name || !header->value)
        return -1;
    *request->header_tail = header;
    request->header_tail = &header->next;
    request->header_count++;
    request->response_metadata_bytes += allocation + name_length + value_length;
    return 0;
}

size_t oasp_request_header_count(const oasp_request *request) { return request ? request->header_count : 0; }

static const oasp_response_header *oasp_request_header_at(const oasp_request *request, size_t index) {
    const oasp_response_header *header = request ? request->headers : NULL;
    while (header && index) {
        header = header->next;
        index--;
    }
    return header;
}

const char *oasp_request_header_name(const oasp_request *request, size_t index) {
    const oasp_response_header *header = oasp_request_header_at(request, index);
    return header ? header->name : NULL;
}

const char *oasp_request_header_value(const oasp_request *request, size_t index) {
    const oasp_response_header *header = oasp_request_header_at(request, index);
    return header ? header->value : NULL;
}

static oasp_response_cookie *oasp_request_cookie(oasp_request *request, const char *name, bool create) {
    for (oasp_response_cookie *cookie = request ? request->cookies : NULL; cookie; cookie = cookie->next)
        if (!strcasecmp(cookie->name, name))
            return cookie;
    if (!request || !create)
        return NULL;
    size_t allocation = sizeof(oasp_response_cookie) + strlen(name) + 2;
    if (request->max_response_header_bytes &&
        (request->response_metadata_bytes > request->max_response_header_bytes ||
         allocation > request->max_response_header_bytes - request->response_metadata_bytes)) {
        request->request_error = "response cookie byte limit exceeded";
        return NULL;
    }
    oasp_response_cookie *cookie = oasp_arena_alloc(request->arena, sizeof(*cookie), _Alignof(oasp_response_cookie));
    if (!cookie)
        return NULL;
    memset(cookie, 0, sizeof(*cookie));
    cookie->name = oasp_arena_strdup(request->arena, name);
    cookie->value = oasp_arena_strdup(request->arena, "");
    if (!cookie->name || !cookie->value)
        return NULL;
    cookie->next = request->cookies;
    request->cookies = cookie;
    request->response_metadata_bytes += allocation;
    return cookie;
}

static int oasp_request_refresh_cookie_header(oasp_request *request, oasp_response_cookie *cookie) {
    size_t length = strlen(cookie->name) + strlen(cookie->value) + 2;
    if (cookie->path)
        length += strlen(cookie->path) + 7;
    if (cookie->domain)
        length += strlen(cookie->domain) + 9;
    if (cookie->expires)
        length += strlen(cookie->expires) + 10;
    if (cookie->secure)
        length += 8;
    if (cookie->http_only)
        length += 10;
    if (request->max_response_header_bytes &&
        (request->response_metadata_bytes > request->max_response_header_bytes ||
         length + 1 > request->max_response_header_bytes - request->response_metadata_bytes)) {
        request->request_error = "response cookie byte limit exceeded";
        return -1;
    }
    char *value = oasp_arena_alloc(request->arena, length + 1, _Alignof(char));
    if (!value)
        return -1;
    request->response_metadata_bytes += length + 1;
    int count = snprintf(value, length + 1, "%s=%s", cookie->name, cookie->value);
    if (cookie->path)
        count += snprintf(value + count, length + 1 - (size_t)count, "; Path=%s", cookie->path);
    if (cookie->domain)
        count += snprintf(value + count, length + 1 - (size_t)count, "; Domain=%s", cookie->domain);
    if (cookie->expires)
        count += snprintf(value + count, length + 1 - (size_t)count, "; Expires=%s", cookie->expires);
    if (cookie->secure)
        count += snprintf(value + count, length + 1 - (size_t)count, "; Secure");
    if (cookie->http_only)
        (void)snprintf(value + count, length + 1 - (size_t)count, "; HttpOnly");
    oasp_response_header *header = request->headers;
    while (header) {
        if (!strcasecmp(header->name, "Set-Cookie") && !strncasecmp(header->value, cookie->name, strlen(cookie->name)) &&
            header->value[strlen(cookie->name)] == '=') {
            return oasp_replace_string(request, &header->value, value);
        }
        header = header->next;
    }
    return oasp_request_add_header(request, "Set-Cookie", value);
}

int oasp_request_set_cookie(oasp_request *request, const char *name, const char *value) {
    if (!request || request->output_committed || !name || !*name || strchr(name, '=') || strchr(name, ';') ||
        strchr(name, '\r') || strchr(name, '\n') || (value && (strchr(value, '\r') || strchr(value, '\n'))))
        return -1;
    oasp_response_cookie *cookie = oasp_request_cookie(request, name, true);
    if (!cookie || oasp_replace_string(request, &cookie->value, value))
        return -1;
    return oasp_request_refresh_cookie_header(request, cookie);
}

int oasp_request_set_cookie_subvalue(oasp_request *request, const char *name, const char *subkey, const char *value) {
    if (!request || request->output_committed || !name || !*name || !subkey || !*subkey || strchr(name, '=') ||
        strchr(name, ';') || strchr(subkey, '=') || strchr(subkey, '&') || strchr(subkey, '\r') || strchr(subkey, '\n') ||
        (value && (strchr(value, '\r') || strchr(value, '\n'))))
        return -1;
    oasp_response_cookie *cookie = oasp_request_cookie(request, name, true);
    if (!cookie)
        return -1;
    const char *source = cookie->value ? cookie->value : "";
    size_t source_length = strlen(source);
    size_t subkey_length = strlen(subkey);
    size_t value_length = value ? strlen(value) : 0;
    size_t capacity = source_length + subkey_length + value_length + 3;
    char *combined = oasp_arena_alloc(request->arena, capacity, _Alignof(char));
    if (!combined)
        return -1;
    size_t length = 0;
    bool replaced = false;
    const char *cursor = source;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        if (!end)
            end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        bool matches = equals && (size_t)(equals - cursor) == subkey_length && !strncasecmp(cursor, subkey, subkey_length);
        if (length)
            combined[length++] = '&';
        if (matches) {
            memcpy(combined + length, subkey, subkey_length);
            length += subkey_length;
            combined[length++] = '=';
            if (value_length) {
                memcpy(combined + length, value, value_length);
                length += value_length;
            }
            replaced = true;
        } else {
            size_t segment_length = (size_t)(end - cursor);
            memcpy(combined + length, cursor, segment_length);
            length += segment_length;
        }
        cursor = *end ? end + 1 : end;
    }
    if (!replaced) {
        if (length)
            combined[length++] = '&';
        memcpy(combined + length, subkey, subkey_length);
        length += subkey_length;
        combined[length++] = '=';
        if (value_length) {
            memcpy(combined + length, value, value_length);
            length += value_length;
        }
    }
    combined[length] = '\0';
    if (oasp_replace_string(request, &cookie->value, combined))
        return -1;
    return oasp_request_refresh_cookie_header(request, cookie);
}

int oasp_request_set_cookie_attribute(oasp_request *request, const char *name, const char *attribute, const char *value) {
    if (!request || request->output_committed || !name || !*name || strchr(name, '=') || strchr(name, ';') ||
        strchr(name, '\r') || strchr(name, '\n') || !attribute || strchr(attribute, '\r') ||
        strchr(attribute, '\n') || (value && (strchr(value, '\r') || strchr(value, '\n'))))
        return -1;
    oasp_response_cookie *cookie = oasp_request_cookie(request, name, true);
    if (!cookie)
        return -1;
    if (!strcasecmp(attribute, "path")) {
        if (oasp_replace_string(request, &cookie->path, value))
            return -1;
    } else if (!strcasecmp(attribute, "domain")) {
        if (oasp_replace_string(request, &cookie->domain, value))
            return -1;
    } else if (!strcasecmp(attribute, "expires")) {
        if (oasp_replace_string(request, &cookie->expires, value))
            return -1;
    } else if (!strcasecmp(attribute, "secure")) {
        cookie->secure = value && strcasecmp(value, "false") && strcmp(value, "0");
    } else if (!strcasecmp(attribute, "httponly")) {
        cookie->http_only = value && strcasecmp(value, "false") && strcmp(value, "0");
    } else {
        return -1;
    }
    return oasp_request_refresh_cookie_header(request, cookie);
}
