// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "openai_stream_internal.h"

#include <json-c/json.h>
#include "json_compat.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define OASP_OPENAI_STREAM_ERROR_TRANSPORT 12004
#define OASP_OPENAI_STREAM_ERROR_SSE 12005
#define OASP_OPENAI_STREAM_ERROR_LIMIT 12006
#define OASP_OPENAI_STREAM_ERROR_IDLE 12007

static int64_t openai_stream_now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void openai_stream_set_error(oasp_openai_stream *stream, long code, const char *message) {
    if (stream->error_code)
        return;
    stream->error_code = code;
    snprintf(stream->error_message, sizeof(stream->error_message), "%s", message ? message : "OpenAI stream failed");
    stream->completed = true;
}

static bool openai_stream_account_buffers(oasp_openai_stream *stream) {
    size_t current = stream->parser.input.capacity + stream->parser.data.capacity + stream->parser.event_type.capacity +
                     stream->url.capacity + stream->payload.capacity + stream->authorization.capacity;
    if (current <= stream->external_bytes)
        return true;
    size_t growth = current - stream->external_bytes;
    if (growth > PTRDIFF_MAX || !oasp_runtime_account_external(stream->runtime, stream, (ptrdiff_t)growth))
        return false;
    stream->external_bytes = current;
    return true;
}

static size_t openai_stream_write(char *data, size_t size, size_t count, void *context) {
    oasp_openai_stream *stream = context;
    if (size && count > SIZE_MAX / size) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "OpenAI stream response size overflow");
        return 0;
    }
    size_t length = size * count;
    if (stream->max_response_bytes &&
        (stream->response_bytes >= stream->max_response_bytes || length > stream->max_response_bytes - stream->response_bytes)) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "OpenAI stream response limit exceeded");
        return 0;
    }
    if (stream->max_buffered_bytes && length > stream->max_buffered_bytes) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "OpenAI stream network block exceeds buffer limit");
        return 0;
    }
    if (stream->max_buffered_bytes && length > stream->max_buffered_bytes - stream->parser.input.length) {
        stream->paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    if (!oasp_buffer_append(&stream->parser.input, data, length) || !openai_stream_account_buffers(stream)) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "unable to buffer OpenAI stream response");
        return 0;
    }
    stream->response_bytes += length;
    stream->last_activity_ms = openai_stream_now_ms();
    return length;
}

static void openai_stream_cleanup_handles(oasp_openai_stream *stream) {
    if (!stream)
        return;
    if (stream->multi && stream->easy && stream->added) {
        curl_multi_remove_handle(stream->multi, stream->easy);
        stream->added = false;
    }
    if (stream->easy) {
        curl_easy_cleanup(stream->easy);
        stream->easy = NULL;
    }
    if (stream->multi) {
        curl_multi_cleanup(stream->multi);
        stream->multi = NULL;
    }
}

static bool openai_stream_add_header(oasp_openai_stream *stream, const char *header) {
    struct curl_slist *updated = curl_slist_append(stream->headers, header);
    if (!updated)
        return false;
    stream->headers = updated;
    return true;
}

void oasp_builtin_openai_stream_finalize(void *context) {
    oasp_openai_stream *stream = context;
    openai_stream_cleanup_handles(stream);
    if (stream->headers)
        curl_slist_free_all(stream->headers);
    stream->headers = NULL;
    oasp_openai_sse_dispose(&stream->parser);
    oasp_buffer_dispose(&stream->url);
    oasp_buffer_dispose(&stream->payload);
    oasp_buffer_dispose(&stream->authorization);
}

static bool openai_stream_append_payload(oasp_openai_stream *stream, const char *payload) {
    struct json_object *json = json_tokener_parse(payload);
    if (!json || json_object_get_type(json) != json_type_object) {
        if (json)
            oasp_json_object_put(json);
        return false;
    }
    struct json_object *enabled = json_object_new_boolean(1);
    if (!enabled || json_object_object_add(json, "stream", enabled)) {
        if (enabled)
            oasp_json_object_put(enabled);
        oasp_json_object_put(json);
        return false;
    }
    const char *encoded = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
    bool ok = encoded && oasp_buffer_append(&stream->payload, encoded, strlen(encoded));
    oasp_json_object_put(json);
    return ok;
}

static bool openai_stream_copy_slice(oasp_runtime *runtime, oasp_openai_stream *stream, oasp_slice *target, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    if (!copy)
        return false;
    *target = (oasp_slice){copy, length};
    if (runtime->allocator)
        oasp_allocator_write_barrier(runtime->allocator, stream, copy);
    return true;
}

static struct json_object *openai_stream_member(struct json_object *object, const char *name) {
    struct json_object *value = NULL;
    return object && json_object_object_get_ex(object, name, &value) ? value : NULL;
}

static bool openai_stream_copy_json_string(oasp_runtime *runtime, oasp_openai_stream *stream, oasp_slice *target, struct json_object *value) {
    if (!value || json_object_get_type(value) != json_type_string)
        return true;
    return openai_stream_copy_slice(runtime, stream, target, json_object_get_string(value), (size_t)json_object_get_string_len(value));
}

static bool openai_stream_copy_json(oasp_runtime *runtime, oasp_openai_stream *stream, oasp_slice *target, struct json_object *value) {
    if (!value)
        return true;
    const char *text = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    return text && openai_stream_copy_slice(runtime, stream, target, text, strlen(text));
}

static bool openai_stream_publish(oasp_runtime *runtime, oasp_openai_stream *stream) {
    stream->delta = (oasp_slice){"", 0};
    stream->finish_reason = (oasp_slice){"", 0};
    stream->usage = (oasp_slice){"", 0};
    const char *event = stream->parser.event_type.length ? stream->parser.event_type.data : "message";
    size_t event_length = stream->parser.event_type.length ? stream->parser.event_type.length : 7;
    if (!openai_stream_copy_slice(runtime, stream, &stream->data, stream->parser.data.data, stream->parser.data.length) ||
        !openai_stream_copy_slice(runtime, stream, &stream->event_type, event, event_length))
        return false;

    if (stream->parser.data.length > INT_MAX)
        return true;
    struct json_tokener *tokener = json_tokener_new();
    struct json_object *json = tokener ? json_tokener_parse_ex(tokener, stream->parser.data.data, (int)stream->parser.data.length) : NULL;
    bool valid = tokener && json_tokener_get_error(tokener) == json_tokener_success && json;
    if (tokener)
        json_tokener_free(tokener);
    if (!valid) {
        if (json)
            oasp_json_object_put(json);
        return true;
    }

    struct json_object *type = openai_stream_member(json, "type");
    if (!stream->parser.event_type.length && type && json_object_get_type(type) == json_type_string) {
        if (!openai_stream_copy_json_string(runtime, stream, &stream->event_type, type))
            goto failed;
    }
    struct json_object *choices = openai_stream_member(json, "choices");
    if (choices && json_object_get_type(choices) == json_type_array && json_object_array_length(choices)) {
        struct json_object *choice = json_object_array_get_idx(choices, 0);
        struct json_object *delta = openai_stream_member(choice, "delta");
        if (!openai_stream_copy_json_string(runtime, stream, &stream->delta, openai_stream_member(delta, "content")) ||
            !openai_stream_copy_json_string(runtime, stream, &stream->finish_reason, openai_stream_member(choice, "finish_reason")))
            goto failed;
    } else {
        if (!openai_stream_copy_json_string(runtime, stream, &stream->delta, openai_stream_member(json, "delta")))
            goto failed;
    }
    struct json_object *usage = openai_stream_member(json, "usage");
    struct json_object *response = openai_stream_member(json, "response");
    if (!usage)
        usage = openai_stream_member(response, "usage");
    if (!openai_stream_copy_json(runtime, stream, &stream->usage, usage))
        goto failed;

    struct json_object *error = openai_stream_member(json, "error");
    if (error) {
        struct json_object *message = openai_stream_member(error, "message");
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_SSE,
                                message && json_object_get_type(message) == json_type_string
                                    ? json_object_get_string(message)
                                    : "OpenAI stream returned an error event");
    }
    oasp_json_object_put(json);
    return !stream->error_code;

failed:
    oasp_json_object_put(json);
    return false;
}

static void openai_stream_finish_transfer(oasp_openai_stream *stream, CURLcode status) {
    stream->transfer_done = true;
    curl_easy_getinfo(stream->easy, CURLINFO_RESPONSE_CODE, &stream->status);
    if (stream->owner && stream->owner->native)
        ((oasp_openai *)stream->owner->native)->status = stream->status;
    if (status != CURLE_OK && !stream->error_code) {
        const char *message = stream->curl_error_message[0] ? stream->curl_error_message : curl_easy_strerror(status);
        openai_stream_set_error(stream, status, message);
    } else if ((stream->status < 200 || stream->status >= 300) && !stream->error_code) {
        char message[96];
        snprintf(message, sizeof(message), "OpenAI stream returned HTTP status %ld", stream->status);
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_TRANSPORT, message);
    }
}

static void openai_stream_read_messages(oasp_openai_stream *stream) {
    int remaining = 0;
    CURLMsg *message = NULL;
    while ((message = curl_multi_info_read(stream->multi, &remaining))) {
        if (message->msg != CURLMSG_DONE || message->easy_handle != stream->easy)
            continue;
        if (stream->added) {
            curl_multi_remove_handle(stream->multi, stream->easy);
            stream->added = false;
        }
        openai_stream_finish_transfer(stream, message->data.result);
    }
}

static bool openai_stream_drive(oasp_runtime *runtime, oasp_openai_stream *stream, size_t line) {
    int running = 0;
    if (stream->paused) {
        stream->paused = false;
        CURLcode resumed = curl_easy_pause(stream->easy, CURLPAUSE_CONT);
        if (resumed != CURLE_OK) {
            openai_stream_set_error(stream, resumed, curl_easy_strerror(resumed));
            return false;
        }
    }
    CURLMcode status = curl_multi_perform(stream->multi, &running);
    openai_stream_read_messages(stream);
    if (status != CURLM_OK) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_TRANSPORT, curl_multi_strerror(status));
        return false;
    }
    if (stream->transfer_done || stream->error_code)
        return false;
    if (runtime->request && oasp_request_deadline_expired(runtime->request)) {
        openai_stream_set_error(stream, 258, "request execution deadline exceeded");
        return false;
    }
    int64_t now = openai_stream_now_ms();
    if (stream->idle_timeout_ms > 0 && now - stream->last_activity_ms >= stream->idle_timeout_ms) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_IDLE, "OpenAI stream idle timeout exceeded");
        return false;
    }
    long remaining = stream->idle_timeout_ms > 0 ? stream->idle_timeout_ms - (long)(now - stream->last_activity_ms) : 100;
    int poll_timeout = remaining < 100 ? (remaining > 0 ? (int)remaining : 0) : 100;
    status = curl_multi_poll(stream->multi, NULL, 0, poll_timeout, NULL);
    if (status != CURLM_OK) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_TRANSPORT, curl_multi_strerror(status));
        return false;
    }
    status = curl_multi_perform(stream->multi, &running);
    openai_stream_read_messages(stream);
    if (status != CURLM_OK) {
        openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_TRANSPORT, curl_multi_strerror(status));
        return false;
    }
    curl_easy_getinfo(stream->easy, CURLINFO_RESPONSE_CODE, &stream->status);
    if (stream->owner && stream->owner->native)
        ((oasp_openai *)stream->owner->native)->status = stream->status;
    oasp_runtime_gc_safepoint(runtime);
    (void)line;
    return true;
}

static bool openai_stream_read(oasp_runtime *runtime, oasp_object *object, size_t line, oasp_value *result) {
    oasp_openai_stream *stream = object->native;
    if (stream->closed || stream->completed || stream->error_code) {
        if (stream->error_code)
            openai_stream_cleanup_handles(stream);
        *result = oasp_value_bool(false);
        return true;
    }
    stream->delta = (oasp_slice){"", 0};
    stream->data = (oasp_slice){"", 0};
    stream->event_type = (oasp_slice){"", 0};
    stream->finish_reason = (oasp_slice){"", 0};
    stream->usage = (oasp_slice){"", 0};
    for (;;) {
        if (stream->status && (stream->status < 200 || stream->status >= 300)) {
            char message[96];
            snprintf(message, sizeof(message), "OpenAI stream returned HTTP status %ld", stream->status);
            openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_TRANSPORT, message);
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
        oasp_sse_status parsed = oasp_openai_sse_next(&stream->parser, stream->max_event_bytes);
        if (!openai_stream_account_buffers(stream)) {
            openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "OpenAI stream memory limit exceeded");
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
        if (parsed == OASP_SSE_EVENT) {
            if (stream->parser.data.length == 6 && !memcmp(stream->parser.data.data, "[DONE]", 6)) {
                curl_easy_getinfo(stream->easy, CURLINFO_RESPONSE_CODE, &stream->status);
                if (stream->owner && stream->owner->native)
                    ((oasp_openai *)stream->owner->native)->status = stream->status;
                openai_stream_cleanup_handles(stream);
                stream->completed = true;
                *result = oasp_value_bool(false);
                return true;
            }
            if (!openai_stream_publish(runtime, stream)) {
                if (!stream->error_code)
                    openai_stream_set_error(stream, 7, "unable to publish OpenAI stream event");
                openai_stream_cleanup_handles(stream);
                *result = oasp_value_bool(false);
                return true;
            }
            *result = oasp_value_bool(true);
            return true;
        }
        if (parsed == OASP_SSE_LIMIT) {
            openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "OpenAI SSE event size limit exceeded");
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
        if (parsed == OASP_SSE_INVALID) {
            openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_SSE, "invalid OpenAI SSE event");
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
        if (parsed == OASP_SSE_SKIPPED)
            continue;
        if (stream->transfer_done) {
            if (stream->parser.input.length)
                openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_SSE, "truncated OpenAI SSE event");
            else if (!stream->error_code)
                stream->completed = true;
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
        if (stream->paused) {
            openai_stream_set_error(stream, OASP_OPENAI_STREAM_ERROR_LIMIT, "OpenAI SSE event exceeds pending buffer limit");
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
        if (!openai_stream_drive(runtime, stream, line) && (stream->transfer_done || stream->error_code)) {
            openai_stream_cleanup_handles(stream);
            *result = oasp_value_bool(false);
            return true;
        }
    }
}

static bool openai_stream_valid_key(const oasp_openai *openai) {
    size_t start = 0;
    while (start < openai->api_key.length && (openai->api_key.data[start] == ' ' || openai->api_key.data[start] == '\t'))
        start++;
    return start < openai->api_key.length && !memchr(openai->api_key.data, '\r', openai->api_key.length) &&
           !memchr(openai->api_key.data, '\n', openai->api_key.length);
}

oasp_object *oasp_builtin_openai_stream_create(oasp_runtime *runtime, oasp_object *owner, const char *endpoint, const char *payload, size_t line) {
    oasp_openai *openai = owner->native;
    if (!openai_stream_valid_key(openai)) {
        oasp_runtime_raise(runtime, 5, "APIKey is required and cannot contain CR or LF characters", owner->component_name, line);
        return NULL;
    }
    if (!endpoint || endpoint[0] != '/' || strchr(endpoint, '\r') || strchr(endpoint, '\n')) {
        oasp_runtime_raise(runtime, 5, "OpenAI stream endpoint must start with '/' and contain no CR or LF", owner->component_name, line);
        return NULL;
    }
    oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_OPENAI_STREAM, "OpenASP.OpenAIStream", NULL);
    if (!object)
        return NULL;
    oasp_gc_root root;
    oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &object, 1, NULL, 0);
    oasp_openai_stream *stream = oasp_runtime_alloc(runtime, sizeof(*stream), _Alignof(oasp_openai_stream));
    if (!stream) {
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    memset(stream, 0, sizeof(*stream));
    stream->runtime = runtime;
    stream->owner = owner;
    object->native = stream;
    if (runtime->allocator) {
        oasp_allocator_write_barrier(runtime->allocator, object, stream);
        oasp_allocator_write_barrier(runtime->allocator, stream, owner);
    }
    stream->timeout_ms = openai->timeout_ms;
    stream->idle_timeout_ms = openai->idle_timeout_ms;
    stream->max_event_bytes = openai->max_event_bytes;
    stream->max_buffered_bytes = openai->max_buffered_bytes;
    stream->max_response_bytes = openai->max_response_bytes;
    bool initialized = oasp_openai_sse_init(&stream->parser) &&
                       oasp_buffer_init(&stream->url, openai->base_url.length + strlen(endpoint) + 1) &&
                       oasp_buffer_init(&stream->payload, strlen(payload) + 32) &&
                       oasp_buffer_init(&stream->authorization, openai->api_key.length + 24);
    if (!initialized)
        goto failed;
    if (!oasp_buffer_append(&stream->url, openai->base_url.data, openai->base_url.length) ||
        !oasp_buffer_append(&stream->url, endpoint, strlen(endpoint)) ||
        !openai_stream_append_payload(stream, payload) ||
        !oasp_buffer_append(&stream->authorization, "Authorization: Bearer ", 22) ||
        !oasp_buffer_append(&stream->authorization, openai->api_key.data, openai->api_key.length))
        goto failed;
    if (!openai_stream_add_header(stream, stream->authorization.data) ||
        !openai_stream_add_header(stream, "Content-Type: application/json") ||
        !openai_stream_add_header(stream, "Accept: text/event-stream"))
        goto failed;
    stream->easy = curl_easy_init();
    stream->multi = curl_multi_init();
    if (!stream->easy || !stream->multi)
        goto failed;
    if (curl_easy_setopt(stream->easy, CURLOPT_URL, stream->url.data) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_POSTFIELDS, stream->payload.data) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)stream->payload.length) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_HTTPHEADER, stream->headers) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_WRITEFUNCTION, openai_stream_write) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_WRITEDATA, stream) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_TIMEOUT_MS, stream->timeout_ms) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
        curl_easy_setopt(stream->easy, CURLOPT_ERRORBUFFER, stream->curl_error_message) != CURLE_OK ||
        curl_multi_add_handle(stream->multi, stream->easy) != CURLM_OK)
        goto failed;
    stream->added = true;
    stream->last_activity_ms = openai_stream_now_ms();
    if (!openai_stream_account_buffers(stream))
        goto failed;
    if (!oasp_runtime_defer(runtime, stream, oasp_builtin_openai_stream_finalize, stream)) {
        (void)oasp_runtime_account_external(runtime, stream, -(ptrdiff_t)stream->external_bytes);
        stream->external_bytes = 0;
        goto failed;
    }
    oasp_runtime_gc_root_leave(runtime, &root);
    return object;

failed:
    oasp_builtin_openai_stream_finalize(stream);
    oasp_runtime_gc_root_leave(runtime, &root);
    oasp_runtime_raise(runtime, 7, "unable to initialize OpenAI stream", owner->component_name, line);
    return NULL;
}

bool oasp_builtin_openai_stream_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_OPENAI_STREAM)
        return false;
    oasp_openai_stream *stream = object->native;
    if (oasp_ascii_equal(member, "delta"))
        *result = oasp_value_string(stream->delta.data, stream->delta.length);
    else if (oasp_ascii_equal(member, "data"))
        *result = oasp_value_string(stream->data.data, stream->data.length);
    else if (oasp_ascii_equal(member, "eventtype"))
        *result = oasp_value_string(stream->event_type.data, stream->event_type.length);
    else if (oasp_ascii_equal(member, "finishreason"))
        *result = oasp_value_string(stream->finish_reason.data, stream->finish_reason.length);
    else if (oasp_ascii_equal(member, "usage"))
        *result = oasp_value_string(stream->usage.data, stream->usage.length);
    else if (oasp_ascii_equal(member, "status"))
        *result = oasp_value_int(stream->status);
    else if (oasp_ascii_equal(member, "completed"))
        *result = oasp_value_bool(stream->completed);
    else if (oasp_ascii_equal(member, "errorcode"))
        *result = oasp_value_int(stream->error_code);
    else if (oasp_ascii_equal(member, "errormessage"))
        *result = oasp_value_string(stream->error_message, strlen(stream->error_message));
    else if (oasp_ascii_equal(member, "responsebytes"))
        *result = oasp_value_int((int64_t)stream->response_bytes);
    else
        return false;
    return true;
}

bool oasp_builtin_openai_stream_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t argument_count, size_t line,
                                     oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_OPENAI_STREAM || argument_count)
        return false;
    if (oasp_ascii_equal(member, "read"))
        return openai_stream_read(runtime, object, line, result);
    if (oasp_ascii_equal(member, "close")) {
        oasp_openai_stream *stream = object->native;
        openai_stream_cleanup_handles(stream);
        stream->closed = true;
        stream->completed = true;
        *result = oasp_value_empty();
        return true;
    }
    return false;
}

void oasp_builtin_openai_stream_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    const oasp_openai_stream *stream = object->native;
    oasp_allocator_trace(allocator, stream->owner);
    oasp_allocator_trace(allocator, stream->delta.data);
    oasp_allocator_trace(allocator, stream->data.data);
    oasp_allocator_trace(allocator, stream->event_type.data);
    oasp_allocator_trace(allocator, stream->finish_reason.data);
    oasp_allocator_trace(allocator, stream->usage.data);
}
