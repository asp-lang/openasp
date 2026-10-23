// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "json_compat.h"
#include <limits.h>
#include <string.h>
#include <strings.h>

static oasp_value http_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static const unsigned char *http_value_bytes(oasp_runtime *runtime, const oasp_value *value, size_t *length) {
    if (value->kind == OASP_VALUE_STRING) {
        *length = value->as.string.length;
        return (const unsigned char *)value->as.string.data;
    }
    char *text = oasp_value_format(runtime->arena, value);
    *length = text ? strlen(text) : 0;
    return (const unsigned char *)text;
}

static size_t oasp_http_write(char *data, size_t size, size_t count, void *context) {
    oasp_http *http = context;
    size_t length = size * count;
    if (http->max_response_bytes && length > http->max_response_bytes - http->response.length)
        return 0;
    return oasp_buffer_append(&http->response, data, length) ? length : 0;
}

static size_t oasp_http_header(char *data, size_t size, size_t count, void *context) {
    oasp_http *http = context;
    size_t length = size * count;
    return oasp_buffer_append(&http->response_headers, data, length) ? length : 0;
}

oasp_object *oasp_builtin_http_create(oasp_runtime *runtime, const char *name) {
    oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_HTTP, name, NULL);
    if (!object)
        return NULL;
    oasp_gc_root root;
    oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &object, 1, NULL, 0);
    oasp_http *http = oasp_runtime_alloc(runtime, sizeof(*http), _Alignof(oasp_http));
    if (!http) {
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    memset(http, 0, sizeof(*http));
    if (!oasp_buffer_init(&http->response, 4096)) {
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    if (!oasp_buffer_init(&http->response_headers, 1024)) {
        oasp_buffer_dispose(&http->response);
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    http->method = (oasp_slice){"GET", 3};
    http->url = (oasp_slice){"", 0};
    http->body = (oasp_slice){"", 0};
    http->content_type = (oasp_slice){"", 0};
    http->ca_file = (oasp_slice){"", 0};
    http->timeout_ms = 30000;
    http->max_response_bytes = 16777216;
    http->tls_verify = true;
    ptrdiff_t external_bytes = (ptrdiff_t)(http->response.capacity + http->response_headers.capacity);
    if (!oasp_runtime_account_external(runtime, http, external_bytes)) {
        oasp_buffer_dispose(&http->response);
        oasp_buffer_dispose(&http->response_headers);
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    if (!oasp_runtime_defer(runtime, http, oasp_builtin_http_finalize, http)) {
        (void)oasp_runtime_account_external(runtime, http, -external_bytes);
        oasp_buffer_dispose(&http->response);
        oasp_buffer_dispose(&http->response_headers);
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    object->native = http;
    if (runtime->allocator)
        oasp_allocator_write_barrier(runtime->allocator, object, http);
    oasp_runtime_gc_root_leave(runtime, &root);
    return object;
}

void oasp_builtin_http_finalize(void *context) {
    oasp_http *http = context;
    oasp_builtin_http_easy_cleanup(http);
    oasp_buffer_dispose(&http->response);
    oasp_buffer_dispose(&http->response_headers);
    if (http->headers)
        curl_slist_free_all(http->headers);
    http->headers = NULL;
}

void oasp_builtin_http_easy_cleanup(oasp_http *http) {
    if (!http || !http->easy)
        return;
    curl_easy_cleanup(http->easy);
    http->easy = NULL;
}

bool oasp_builtin_http_prepare(oasp_runtime *runtime, oasp_http *http, long default_timeout_ms) {
    if (!runtime || !http || http->easy)
        return false;
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;
    http->easy = curl;
    http->response.length = 0;
    http->response.data[0] = '\0';
    http->response_headers.length = 0;
    http->response_headers.data[0] = '\0';
    http->content_type = (oasp_slice){"", 0};
    http->status = 0;
    http->error_code = CURLE_OK;
    http->error_message[0] = '\0';
    http->completed = false;
    http->ready_state = 1;
    const char *url = http->url.data;
    const char *method = http->method.data;
    long timeout_ms = http->timeout_explicit ? http->timeout_ms : default_timeout_ms;
    if (!url || !method ||
        curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, *method ? method : "GET") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oasp_http_write) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, http) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, oasp_http_header) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, http) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http->headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 30000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, http->tls_verify ? 1L : 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, http->tls_verify ? 2L : 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, http->error_message) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PRIVATE, http) != CURLE_OK)
        goto failed;
    if (http->ca_file.length) {
        if (curl_easy_setopt(curl, CURLOPT_CAINFO, http->ca_file.data) != CURLE_OK)
            goto failed;
    }
    if (http->body.data && (http->body.length || !strcasecmp(method, "POST"))) {
        if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, http->body.data) != CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)http->body.length) != CURLE_OK)
            goto failed;
    }
    return true;

failed:
    oasp_builtin_http_easy_cleanup(http);
    return false;
}

void oasp_builtin_http_complete(oasp_runtime *runtime, oasp_http *http, CURLcode status) {
    if (!runtime || !http || !http->easy)
        return;
    http->error_code = status;
    curl_easy_getinfo(http->easy, CURLINFO_RESPONSE_CODE, &http->status);
    if (status == CURLE_OK) {
        char *content_type = NULL;
        curl_easy_getinfo(http->easy, CURLINFO_CONTENT_TYPE, &content_type);
        if (content_type) {
            size_t length = strlen(content_type);
            char *copy = oasp_runtime_copy(runtime, content_type, length);
            if (copy)
                http->content_type = (oasp_slice){copy, length};
        }
    } else if (!http->error_message[0]) {
        const char *message = curl_easy_strerror(status);
        snprintf(http->error_message, sizeof(http->error_message), "%s", message);
    }
    http->ready_state = 4;
    http->completed = true;
}

bool oasp_builtin_http_perform(oasp_runtime *runtime, oasp_object *object, oasp_http *http, const char *body, size_t line) {
    oasp_object *rooted_object = object;
    oasp_gc_root root;
    oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &rooted_object, 1, NULL, 0);
    oasp_slice saved_body = http->body;
    if (body)
        http->body = (oasp_slice){body, strlen(body)};
    if (!oasp_builtin_http_prepare(runtime, http, http->timeout_ms > 0 ? http->timeout_ms : 30000L)) {
        http->body = saved_body;
        oasp_runtime_gc_root_leave(runtime, &root);
        oasp_runtime_raise(runtime, 7, "unable to initialize HTTP request", object->component_name, line);
        return false;
    }
    CURLcode status = curl_easy_perform(http->easy);
    oasp_builtin_http_complete(runtime, http, status);
    oasp_builtin_http_easy_cleanup(http);
    http->body = saved_body;
    if (status != CURLE_OK)
        oasp_runtime_raise(runtime, 12004, "HTTP request failed", object->component_name, line);
    oasp_runtime_gc_root_leave(runtime, &root);
    return status == CURLE_OK;
}

oasp_value oasp_builtin_http_response_header(oasp_runtime *runtime, const oasp_http *http, const char *name) {
    size_t name_length = strlen(name);
    const char *cursor = http->response_headers.data;
    const char *end = cursor + http->response_headers.length;
    const char *matched = NULL;
    size_t matched_length = 0;
    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!line_end)
            line_end = end;
        const char *colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon && (size_t)(colon - cursor) == name_length && !strncasecmp(cursor, name, name_length)) {
            const char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t'))
                value++;
            const char *value_end = line_end;
            while (value_end > value && (value_end[-1] == '\r' || value_end[-1] == '\n'))
                value_end--;
            matched = value;
            matched_length = (size_t)(value_end - value);
        }
        cursor = line_end < end ? line_end + 1 : end;
    }
    return matched ? http_slice_value(runtime, matched, matched_length) : oasp_value_string("", 0);
}

bool oasp_builtin_openai_request(oasp_runtime *runtime, oasp_object *object, oasp_openai *openai, const char *endpoint, const char *payload,
                                 size_t line, oasp_value *result) {
    const char *key = openai->api_key.data;
    size_t key_length = openai->api_key.length;
    size_t start = 0;
    while (start < key_length && isspace((unsigned char)key[start]))
        start++;
    size_t end = key_length;
    while (end > start && isspace((unsigned char)key[end - 1]))
        end--;
    if (start == end) {
        oasp_runtime_raise(runtime, 5,
                           "APIKey is required; assign OpenASP.OpenAI.APIKey before "
                           "sending a request",
                           object->component_name, line);
        return true;
    }
    if (memchr(key, '\r', key_length) || memchr(key, '\n', key_length)) {
        oasp_runtime_raise(runtime, 5, "APIKey cannot contain CR or LF characters", object->component_name, line);
        return true;
    }

    oasp_buffer url = {0};
    oasp_buffer authorization = {0};
    oasp_http http;
    memset(&http, 0, sizeof(http));
    if (!oasp_buffer_init(&url, openai->base_url.length + strlen(endpoint) + 1) || !oasp_buffer_init(&authorization, key_length + 24) ||
        !oasp_buffer_init(&http.response, 4096) || !oasp_buffer_init(&http.response_headers, 1024)) {
        oasp_buffer_dispose(&url);
        oasp_buffer_dispose(&authorization);
        oasp_builtin_http_finalize(&http);
        return true;
    }
    (void)oasp_buffer_append(&url, openai->base_url.data, openai->base_url.length);
    (void)oasp_buffer_append(&url, endpoint, strlen(endpoint));
    (void)oasp_buffer_append(&authorization, "Authorization: Bearer ", 22);
    (void)oasp_buffer_append(&authorization, key, key_length);
    http.headers = curl_slist_append(http.headers, authorization.data);
    http.headers = curl_slist_append(http.headers, "Content-Type: application/json");
    http.method = (oasp_slice){"POST", 4};
    http.url = (oasp_slice){url.data, url.length};
    http.timeout_ms = openai->timeout_ms;
    http.max_response_bytes = openai->max_response_bytes;
    http.tls_verify = true;

    bool ok = http.headers && oasp_builtin_http_perform(runtime, object, &http, payload, line);
    openai->status = http.status;
    if (ok) {
        char *response = oasp_runtime_copy(runtime, http.response.data, http.response.length);
        if (response) {
            openai->last_response = (oasp_slice){response, http.response.length};
            *result = oasp_value_string(response, http.response.length);
        }
    }
    oasp_builtin_http_finalize(&http);
    oasp_buffer_dispose(&authorization);
    oasp_buffer_dispose(&url);
    return true;
}

char *oasp_builtin_openai_payload(oasp_runtime *runtime, oasp_openai *openai, const oasp_value *input, bool responses) {
    size_t length = 0;
    const unsigned char *bytes = http_value_bytes(runtime, input, &length);
    oasp_value parsed;
    if (oasp_builtin_json_parse(runtime, input, &parsed))
        return oasp_runtime_copy(runtime, (const char *)bytes, length);

    struct json_object *payload = json_object_new_object();
    struct json_object *model = json_object_new_string_len(openai->model.data, (int)openai->model.length);
    struct json_object *content = json_object_new_string_len((const char *)bytes, (int)length);
    if (!payload || !model || !content)
        goto failed;
    if (json_object_object_add(payload, "model", model))
        goto failed;
    model = NULL;
    if (responses) {
        if (json_object_object_add(payload, "input", content))
            goto failed;
        content = NULL;
    } else {
        struct json_object *messages = json_object_new_array();
        struct json_object *message = json_object_new_object();
        struct json_object *role = json_object_new_string("user");
        if (!messages || !message || !role) {
            if (messages)
                oasp_json_object_put(messages);
            if (message)
                oasp_json_object_put(message);
            if (role)
                oasp_json_object_put(role);
            goto failed;
        }
        if (json_object_object_add(message, "role", role)) {
            oasp_json_object_put(messages);
            oasp_json_object_put(message);
            oasp_json_object_put(role);
            goto failed;
        }
        role = NULL;
        if (json_object_object_add(message, "content", content)) {
            oasp_json_object_put(messages);
            oasp_json_object_put(message);
            goto failed;
        }
        content = NULL;
        if (json_object_array_add(messages, message)) {
            oasp_json_object_put(messages);
            oasp_json_object_put(message);
            goto failed;
        }
        message = NULL;
        if (json_object_object_add(payload, "messages", messages)) {
            oasp_json_object_put(messages);
            goto failed;
        }
        messages = NULL;
    }
    const char *text = json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
    char *result = text ? oasp_runtime_copy(runtime, text, strlen(text)) : NULL;
    oasp_json_object_put(payload);
    return result;

failed:
    if (model)
        oasp_json_object_put(model);
    if (content)
        oasp_json_object_put(content);
    if (payload)
        oasp_json_object_put(payload);
    return NULL;
}

bool oasp_builtin_http_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind == OASP_COMPONENT_HTTP) {
        oasp_http *http = object->native;
        if (oasp_ascii_equal(member, "timeout")) {
            int64_t timeout = 0;
            if (oasp_value_to_int(value, &timeout) && timeout > 0 && timeout <= LONG_MAX) {
                http->timeout_ms = (long)timeout;
                http->timeout_explicit = true;
            } else {
                oasp_runtime_raise(runtime, 5, "invalid HTTP timeout", object->component_name, line);
            }
            return true;
        }
        if (oasp_ascii_equal(member, "body")) {
            char *text = oasp_value_format(runtime->arena, value);
            if (text)
                http->body = (oasp_slice){text, strlen(text)};
            return true;
        }
        if (oasp_ascii_equal(member, "maxresponsebytes")) {
            int64_t maximum = 0;
            if (oasp_value_to_int(value, &maximum) && maximum > 0)
                http->max_response_bytes = (size_t)maximum;
            else
                oasp_runtime_raise(runtime, 5, "invalid maximum response size", object->component_name, line);
            return true;
        }
        if (oasp_ascii_equal(member, "tlsverify")) {
            http->tls_verify = oasp_value_truthy(value);
            return true;
        }
        if (oasp_ascii_equal(member, "cafile")) {
            char *text = oasp_value_format(runtime->arena, value);
            if (text)
                http->ca_file = (oasp_slice){text, strlen(text)};
            return true;
        }
    }
    if (object->component_kind != OASP_COMPONENT_OPENAI)
        return false;
    oasp_openai *openai = object->native;
    if (oasp_ascii_equal(member, "apikey") || oasp_ascii_equal(member, "baseurl") || oasp_ascii_equal(member, "model")) {
        char *text = oasp_value_format(runtime->arena, value);
        size_t length = text ? strlen(text) : 0;
        if (oasp_ascii_equal(member, "baseurl"))
            while (length && text[length - 1] == '/')
                length--;
        if (oasp_ascii_equal(member, "apikey"))
            openai->api_key = (oasp_slice){text, length};
        else if (oasp_ascii_equal(member, "baseurl"))
            openai->base_url = (oasp_slice){text, length};
        else
            openai->model = (oasp_slice){text, length};
        return true;
    }
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "timeout")) {
        if (!oasp_value_to_int(value, &integer) || integer <= 0 || integer > LONG_MAX)
            oasp_runtime_raise(runtime, 5, "invalid timeout", object->component_name, line);
        else
            openai->timeout_ms = (long)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "idletimeout")) {
        if (!oasp_value_to_int(value, &integer) || integer <= 0 || integer > LONG_MAX)
            oasp_runtime_raise(runtime, 5, "invalid stream idle timeout", object->component_name, line);
        else
            openai->idle_timeout_ms = (long)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "maxeventbytes") || oasp_ascii_equal(member, "maxbufferedbytes")) {
        if (!oasp_value_to_int(value, &integer) || integer <= 0)
            oasp_runtime_raise(runtime, 5, "invalid OpenAI stream buffer limit", object->component_name, line);
        else if (oasp_ascii_equal(member, "maxeventbytes"))
            openai->max_event_bytes = (size_t)integer;
        else
            openai->max_buffered_bytes = (size_t)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "maxresponsebytes")) {
        if (!oasp_value_to_int(value, &integer) || integer <= 0)
            oasp_runtime_raise(runtime, 5, "invalid maximum response size", object->component_name, line);
        else
            openai->max_response_bytes = (size_t)integer;
        return true;
    }
    return false;
}

bool oasp_builtin_http_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                            size_t line, oasp_value *result) {
    if (object->component_kind == OASP_COMPONENT_OPENAI) {
        bool chat_stream = oasp_ascii_equal(member, "openchatstream");
        bool responses_stream = oasp_ascii_equal(member, "openresponsesstream");
        bool custom_stream = oasp_ascii_equal(member, "openstream");
        bool synchronous = oasp_ascii_equal(member, "chatcompletions") || oasp_ascii_equal(member, "responses");
        if ((!synchronous && !chat_stream && !responses_stream && !custom_stream) ||
            (custom_stream ? argument_count != 2 : argument_count != 1))
            return false;
        oasp_openai *openai = object->native;
        bool responses = oasp_ascii_equal(member, "responses") || responses_stream;
        char *payload = custom_stream ? oasp_value_format(runtime->arena, &arguments[1])
                                      : oasp_builtin_openai_payload(runtime, openai, &arguments[0], responses);
        if (!payload) {
            oasp_runtime_raise(runtime, 7, "out of memory", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        if (chat_stream || responses_stream || custom_stream) {
            oasp_value rooted_payload = oasp_value_string(payload, strlen(payload));
            oasp_object *rooted_owner = object;
            oasp_gc_root root;
            oasp_runtime_gc_root_enter(runtime, &root, &rooted_payload, 1, &rooted_owner, 1, NULL, 0);
            char *endpoint = custom_stream ? oasp_value_format(runtime->arena, &arguments[0])
                                           : responses_stream ? "/responses" : "/chat/completions";
            oasp_object *stream = endpoint ? oasp_builtin_openai_stream_create(runtime, object, endpoint, payload, line) : NULL;
            oasp_runtime_gc_root_leave(runtime, &root);
            *result = stream ? oasp_component_object_value(stream) : oasp_value_empty();
            return true;
        }
        return oasp_builtin_openai_request(runtime, object, openai,
                                           responses ? "/responses" : "/chat/completions", payload, line, result);
    }
    if (object->component_kind != OASP_COMPONENT_HTTP)
        return false;
    oasp_http *http = object->native;
    if (oasp_ascii_equal(member, "settimeouts") && argument_count == 4) {
        long timeout = 0;
        for (size_t i = 0; i < argument_count; i++) {
            int64_t value = 0;
            if (!oasp_value_to_int(&arguments[i], &value) || value < 0) {
                oasp_runtime_raise(runtime, 5, "invalid HTTP timeout", object->component_name, line);
                *result = oasp_value_empty();
                return true;
            }
            if (value > timeout)
                timeout = value > LONG_MAX ? LONG_MAX : (long)value;
        }
        http->timeout_ms = timeout;
    } else if (oasp_ascii_equal(member, "open") && argument_count >= 2) {
        char *method = oasp_value_format(runtime->arena, &arguments[0]);
        char *url = oasp_value_format(runtime->arena, &arguments[1]);
        if (method && url) {
            http->method = (oasp_slice){method, strlen(method)};
            http->url = (oasp_slice){url, strlen(url)};
            http->ready_state = 1;
            http->completed = false;
            http->error_code = CURLE_OK;
            http->error_message[0] = '\0';
        }
    } else if ((oasp_ascii_equal(member, "setrequestheader") || oasp_ascii_equal(member, "setheader")) && argument_count >= 2) {
        char *name = oasp_value_format(runtime->arena, &arguments[0]);
        char *value = oasp_value_format(runtime->arena, &arguments[1]);
        oasp_buffer header = {0};
        if (name && value && oasp_buffer_init(&header, strlen(name) + strlen(value) + 3)) {
            (void)oasp_buffer_append(&header, name, strlen(name));
            (void)oasp_buffer_append(&header, ": ", 2);
            (void)oasp_buffer_append(&header, value, strlen(value));
            http->headers = curl_slist_append(http->headers, header.data);
            oasp_buffer_dispose(&header);
        }
    } else if (oasp_ascii_equal(member, "clearheaders")) {
        if (http->headers)
            curl_slist_free_all(http->headers);
        http->headers = NULL;
    } else if (oasp_ascii_equal(member, "send")) {
        (void)oasp_builtin_http_perform(runtime, object, http,
                                        argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL, line);
    } else if (oasp_ascii_equal(member, "getresponseheader")) {
        char *name = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
        *result = name ? oasp_builtin_http_response_header(runtime, http, name) : oasp_value_string("", 0);
        return true;
    } else if ((oasp_ascii_equal(member, "get") || oasp_ascii_equal(member, "post")) && argument_count) {
        char *url = oasp_value_format(runtime->arena, &arguments[0]);
        http->url = (oasp_slice){url, url ? strlen(url) : 0};
        http->method = oasp_ascii_equal(member, "get") ? (oasp_slice){"GET", 3} : (oasp_slice){"POST", 4};
        (void)oasp_builtin_http_perform(runtime, object, http,
                                        argument_count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : NULL, line);
        *result = http_slice_value(runtime, http->response.data, http->response.length);
        return true;
    } else {
        return false;
    }
    *result = oasp_value_empty();
    return true;
}
