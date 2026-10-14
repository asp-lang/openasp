// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_context.h"
#include "component_object.h"
#include "ext_loader.h"

#include <string.h>

oasp_value oasp_component_create(oasp_runtime *runtime, oasp_slice prog_id, size_t line) {
    oasp_value external = oasp_value_empty();
    if (oasp_external_component_create(runtime, runtime ? runtime->component_context : NULL, prog_id, line, &external))
        return external;
    if (oasp_ascii_equal(prog_id, "shell.application") && !oasp_builtin_feature_enabled("OPENASP_SHELL_ENABLE")) {
        oasp_runtime_raise(runtime, 70,
                           "Shell.Application is disabled; restart OpenASP with "
                           "--shell-enable=on to enable command execution",
                           "Shell.Application", line);
        return oasp_value_empty();
    }
    if (oasp_ascii_equal(prog_id, "openasp.process") && !oasp_builtin_feature_enabled("OPENASP_PROCESS_ENABLE")) {
        oasp_runtime_raise(runtime, 70,
                           "OpenASP.Process is disabled; restart OpenASP with "
                           "--process-enable=on to enable process execution",
                           "OpenASP.Process", line);
        return oasp_value_empty();
    }
    const oasp_component_registration *registration = oasp_builtin_component_registration(prog_id);
    if (!registration) {
        oasp_runtime_raise(runtime, 429, "unsupported CreateObject provider", "Server", line);
        return oasp_value_empty();
    }

    oasp_object *object = oasp_component_object(runtime, registration->kind, registration->canonical_name, NULL);
    if (!object)
        return oasp_value_empty();
    if (registration->kind == OASP_COMPONENT_DICTIONARY) {
        oasp_dictionary *dictionary = oasp_builtin_dictionary_create(
            runtime, !strcmp(registration->canonical_name, "OpenASP.JSON.Object") ? 0 : 1);
        if (!dictionary)
            return oasp_value_empty();
        object->native = dictionary;
    } else if (registration->kind == OASP_COMPONENT_REGEXP) {
        oasp_regexp *expression = oasp_builtin_regexp_create(runtime);
        if (!expression)
            return oasp_value_empty();
        object->native = expression;
    } else if (registration->kind == OASP_COMPONENT_STREAM) {
        object->native = oasp_builtin_stream_create(runtime);
        if (!object->native)
            return oasp_value_empty();
    } else if (registration->kind == OASP_COMPONENT_XML) {
        object->native = oasp_builtin_xml_create(runtime);
        if (!object->native)
            return oasp_value_empty();
    } else if (registration->kind == OASP_COMPONENT_HTTP) {
        oasp_http *http = oasp_runtime_alloc(runtime, sizeof(*http), _Alignof(oasp_http));
        if (!http || !oasp_buffer_init(&http->response, 4096))
            return oasp_value_empty();
        if (!oasp_buffer_init(&http->response_headers, 1024)) {
            oasp_buffer_dispose(&http->response);
            return oasp_value_empty();
        }
        http->headers = NULL;
        http->method = (oasp_slice){"GET", 3};
        http->url = (oasp_slice){"", 0};
        http->content_type = (oasp_slice){"", 0};
        http->ca_file = (oasp_slice){"", 0};
        http->status = 0;
        http->timeout_ms = 30000;
        http->max_response_bytes = 16777216;
        http->ready_state = 0;
        http->tls_verify = true;
        if (!oasp_runtime_account_external(runtime, http,
                                           (ptrdiff_t)(http->response.capacity +
                                                       http->response_headers.capacity))) {
            oasp_buffer_dispose(&http->response);
            oasp_buffer_dispose(&http->response_headers);
            return oasp_value_empty();
        }
        if (!oasp_runtime_defer(runtime, http, oasp_builtin_http_finalize, http)) {
            oasp_buffer_dispose(&http->response);
            oasp_buffer_dispose(&http->response_headers);
            return oasp_value_empty();
        }
        object->native = http;
    } else if (registration->kind == OASP_COMPONENT_OPENAI) {
        oasp_openai *openai = oasp_runtime_alloc(runtime, sizeof(*openai), _Alignof(oasp_openai));
        if (!openai)
            return oasp_value_empty();
        memset(openai, 0, sizeof(*openai));
        openai->base_url = (oasp_slice){"https://api.openai.com/v1", 25};
        openai->model = (oasp_slice){"gpt-4o-mini", 11};
        openai->timeout_ms = 120000;
        openai->max_response_bytes = 16777216;
        object->native = openai;
    } else if (registration->kind == OASP_COMPONENT_ADO_CONNECTION ||
               registration->kind == OASP_COMPONENT_ADO_RECORDSET ||
               registration->kind == OASP_COMPONENT_ADO_COMMAND) {
        object->native = oasp_builtin_ado_create(runtime, registration->kind);
        if (!object->native)
            return oasp_value_empty();
    } else if (registration->kind == OASP_COMPONENT_SOCKET) {
        oasp_socket *socket = oasp_runtime_alloc(runtime, sizeof(*socket), _Alignof(oasp_socket));
        if (!socket)
            return oasp_value_empty();
        memset(socket, 0, sizeof(*socket));
        socket->protocol = (oasp_slice){"tcp", 3};
        socket->timeout = 30000;
        socket->fd = -1;
        if (!oasp_runtime_defer(runtime, socket, oasp_builtin_socket_finalize, socket))
            return oasp_value_empty();
        object->native = socket;
    } else if (registration->kind == OASP_COMPONENT_REDIS) {
        oasp_redis *redis = oasp_runtime_alloc(runtime, sizeof(*redis), _Alignof(oasp_redis));
        if (!redis)
            return oasp_value_empty();
        memset(redis, 0, sizeof(*redis));
        redis->component_context = runtime->component_context;
        redis->host = (oasp_slice){"127.0.0.1", 9};
        redis->port = 6379;
        redis->timeout_ms = 5000;
        if (!oasp_runtime_defer(runtime, redis, oasp_builtin_redis_finalize, redis))
            return oasp_value_empty();
        object->native = redis;
    } else if (registration->kind == OASP_COMPONENT_PROCESS) {
        oasp_process *process = oasp_runtime_alloc(runtime, sizeof(*process), _Alignof(oasp_process));
        if (!process)
            return oasp_value_empty();
        memset(process, 0, sizeof(*process));
        process->exit_code = -1;
        process->timeout_ms = 30000;
        process->max_output_bytes = 4194304;
        if (!oasp_runtime_defer(runtime, process, oasp_builtin_process_finalize, process))
            return oasp_value_empty();
        object->native = process;
    } else if (registration->kind == OASP_COMPONENT_WEBSOCKET) {
        oasp_websocket *websocket = oasp_runtime_alloc(runtime, sizeof(*websocket), _Alignof(oasp_websocket));
        if (!websocket)
            return oasp_value_empty();
        memset(websocket, 0, sizeof(*websocket));
        websocket->fd = -1;
        websocket->timeout_ms = 15000;
        websocket->tls_verify = true;
        if (!oasp_runtime_defer(runtime, websocket, oasp_builtin_websocket_finalize, websocket))
            return oasp_value_empty();
        object->native = websocket;
    } else if (registration->kind == OASP_COMPONENT_MEMCACHE) {
        oasp_memcache *cache = oasp_runtime_alloc(runtime, sizeof(*cache), _Alignof(oasp_memcache));
        if (!cache)
            return oasp_value_empty();
        memset(cache, 0, sizeof(*cache));
        cache->component_context = runtime->component_context;
        cache->host = (oasp_slice){"127.0.0.1", 9};
        cache->protocol = (oasp_slice){"text", 4};
        cache->port = 11211;
        cache->timeout_ms = 5000;
        cache->tls_verify = true;
        if (!oasp_runtime_defer(runtime, cache, oasp_builtin_memcache_finalize, cache))
            return oasp_value_empty();
        object->native = cache;
    }
    return oasp_component_object_value(object);
}
