// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"

#include <limits.h>
#include <string.h>

static void http_multi_mark_failed(oasp_http *http, CURLcode status) {
    http->error_code = status;
    http->ready_state = 4;
    http->completed = true;
    snprintf(http->error_message, sizeof(http->error_message), "%s", curl_easy_strerror(status));
}

static void http_multi_read_messages(oasp_runtime *runtime, CURLM *handle, size_t *active) {
    int remaining = 0;
    CURLMsg *message = NULL;
    while ((message = curl_multi_info_read(handle, &remaining))) {
        if (message->msg != CURLMSG_DONE)
            continue;
        oasp_http *http = NULL;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &http);
        curl_multi_remove_handle(handle, message->easy_handle);
        if (http) {
            oasp_builtin_http_complete(runtime, http, message->data.result);
            oasp_builtin_http_easy_cleanup(http);
        }
        if (*active)
            (*active)--;
    }
}

static void http_multi_cancel(CURLM *handle, oasp_http_multi *multi, CURLcode status) {
    for (oasp_object *request = multi->first; request; request = ((oasp_http *)request->native)->next_multi_request) {
        oasp_http *http = request->native;
        if (http->easy) {
            curl_multi_remove_handle(handle, http->easy);
            oasp_builtin_http_easy_cleanup(http);
        }
        if (!http->completed)
            http_multi_mark_failed(http, status);
    }
}

static CURLMcode http_multi_schedule(oasp_runtime *runtime, CURLM *handle, oasp_http_multi *multi, oasp_object **pending, size_t *active) {
    while (*pending && *active < (size_t)multi->max_concurrency) {
        oasp_object *request = *pending;
        oasp_http *http = request->native;
        *pending = http->next_multi_request;
        if (!oasp_builtin_http_prepare(runtime, http, multi->timeout_ms)) {
            http_multi_mark_failed(http, CURLE_FAILED_INIT);
            continue;
        }
        CURLMcode status = curl_multi_add_handle(handle, http->easy);
        if (status != CURLM_OK) {
            oasp_builtin_http_easy_cleanup(http);
            http_multi_mark_failed(http, CURLE_FAILED_INIT);
            return status;
        }
        (*active)++;
    }
    return CURLM_OK;
}

static bool http_multi_execute(oasp_runtime *runtime, oasp_object *object, oasp_http_multi *multi, size_t line) {
    if (multi->executing) {
        oasp_runtime_raise(runtime, 5, "HTTP multi batch is already executing", object->component_name, line);
        return false;
    }
    CURLM *handle = curl_multi_init();
    if (!handle) {
        oasp_runtime_raise(runtime, 7, "unable to initialize HTTP multi handle", object->component_name, line);
        return false;
    }
    multi->executing = true;
    CURLMcode multi_status = curl_multi_setopt(handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, multi->max_concurrency);
    if (multi_status == CURLM_OK)
        multi_status = curl_multi_setopt(handle, CURLMOPT_MAX_HOST_CONNECTIONS, multi->max_concurrency);
    if (multi_status == CURLM_OK)
        multi_status = curl_multi_setopt(handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    oasp_object *pending = multi->first;
    size_t active = 0;
    int running = 0;
    while (multi_status == CURLM_OK && (pending || active)) {
        multi_status = http_multi_schedule(runtime, handle, multi, &pending, &active);
        if (multi_status != CURLM_OK || !active)
            continue;
        multi_status = curl_multi_perform(handle, &running);
        http_multi_read_messages(runtime, handle, &active);
        if (multi_status != CURLM_OK || !active)
            continue;
        if (runtime->request && oasp_request_deadline_expired(runtime->request)) {
            oasp_runtime_raise(runtime, 258, "request execution deadline exceeded", object->component_name, line);
            break;
        }
        multi_status = curl_multi_poll(handle, NULL, 0, 100, NULL);
        if (multi_status == CURLM_OK)
            multi_status = curl_multi_perform(handle, &running);
        http_multi_read_messages(runtime, handle, &active);
        oasp_runtime_gc_safepoint(runtime);
    }

    bool ok = multi_status == CURLM_OK && !pending && active == 0;
    if (!ok) {
        http_multi_cancel(handle, multi, CURLE_ABORTED_BY_CALLBACK);
        if (multi_status != CURLM_OK)
            oasp_runtime_raise(runtime, 12004, curl_multi_strerror(multi_status), object->component_name, line);
    }
    curl_multi_cleanup(handle);
    multi->executing = false;
    return ok;
}

bool oasp_builtin_http_multi_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_HTTP_MULTI)
        return false;
    oasp_http_multi *multi = object->native;
    if (oasp_ascii_equal(member, "count"))
        *result = oasp_value_int((int64_t)multi->count);
    else if (oasp_ascii_equal(member, "maxconcurrency"))
        *result = oasp_value_int(multi->max_concurrency);
    else if (oasp_ascii_equal(member, "timeout"))
        *result = oasp_value_int(multi->timeout_ms);
    else
        return false;
    return true;
}

bool oasp_builtin_http_multi_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!object || object->component_kind != OASP_COMPONENT_HTTP_MULTI)
        return false;
    oasp_http_multi *multi = object->native;
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "maxconcurrency")) {
        if (!oasp_value_to_int(value, &integer) || integer < 1 || integer > 256)
            oasp_runtime_raise(runtime, 5, "MaxConcurrency must be between 1 and 256", object->component_name, line);
        else
            multi->max_concurrency = (long)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "timeout")) {
        if (!oasp_value_to_int(value, &integer) || integer < 1 || integer > LONG_MAX)
            oasp_runtime_raise(runtime, 5, "Timeout must be a positive millisecond value", object->component_name, line);
        else
            multi->timeout_ms = (long)integer;
        return true;
    }
    return false;
}

bool oasp_builtin_http_multi_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                  size_t line, oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_HTTP_MULTI)
        return false;
    oasp_http_multi *multi = object->native;
    if (oasp_ascii_equal(member, "execute") && !argument_count) {
        oasp_object *rooted_object = object;
        oasp_gc_root root;
        oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &rooted_object, 1, NULL, 0);
        (void)http_multi_execute(runtime, object, multi, line);
        oasp_runtime_gc_root_leave(runtime, &root);
        *result = oasp_value_empty();
        return true;
    }
    if (!oasp_ascii_equal(member, "createrequest") || argument_count < 2)
        return false;
    oasp_object *rooted_object = object;
    oasp_gc_root owner_root;
    oasp_runtime_gc_root_enter(runtime, &owner_root, NULL, 0, &rooted_object, 1, NULL, 0);
    char *method_text = oasp_value_format(runtime->arena, &arguments[0]);
    char *url_text = oasp_value_format(runtime->arena, &arguments[1]);
    if (!method_text || !url_text) {
        oasp_runtime_gc_root_leave(runtime, &owner_root);
        oasp_runtime_raise(runtime, 7, "out of memory", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    size_t method_length = strlen(method_text);
    size_t url_length = strlen(url_text);
    oasp_object *request = oasp_builtin_http_create(runtime, "OpenASP.HttpRequest");
    if (!request) {
        oasp_runtime_gc_root_leave(runtime, &owner_root);
        oasp_runtime_raise(runtime, 7, "unable to create HTTP request", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    oasp_gc_root root;
    oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &request, 1, NULL, 0);
    char *method = oasp_runtime_copy(runtime, method_text, method_length);
    oasp_http *http = request->native;
    http->method = (oasp_slice){method, method ? method_length : 0};
    if (runtime->allocator && method)
        oasp_allocator_write_barrier(runtime->allocator, http, method);
    char *url = method ? oasp_runtime_copy(runtime, url_text, url_length) : NULL;
    if (!url) {
        oasp_runtime_gc_root_leave(runtime, &root);
        oasp_runtime_gc_root_leave(runtime, &owner_root);
        oasp_runtime_raise(runtime, 7, "unable to create HTTP request", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    http->url = (oasp_slice){url, url_length};
    if (runtime->allocator)
        oasp_allocator_write_barrier(runtime->allocator, http, url);
    http->ready_state = 1;
    http->multi_owner = multi;
    oasp_object *previous = multi->last;
    if (previous)
        ((oasp_http *)previous->native)->next_multi_request = request;
    else
        multi->first = request;
    multi->last = request;
    multi->count++;
    if (runtime->allocator) {
        oasp_allocator_write_barrier(runtime->allocator, multi, request);
        oasp_allocator_write_barrier(runtime->allocator, http, multi);
        if (previous)
            oasp_allocator_write_barrier(runtime->allocator, previous->native, request);
    }
    oasp_runtime_gc_root_leave(runtime, &root);
    oasp_runtime_gc_root_leave(runtime, &owner_root);
    *result = oasp_component_object_value(request);
    return true;
}
