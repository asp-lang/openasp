// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component.h"
#include "builtin_internal.h"
#include "component_object.h"

#include <string.h>

static bool component_get_properties(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    if (object->component_kind == OASP_COMPONENT_DICTIONARY && oasp_ascii_equal(member, "removeall"))
        return oasp_component_call(runtime, object, member, NULL, 0, line, result);
    if (object->component_kind == OASP_COMPONENT_STREAM &&
        (oasp_ascii_equal(member, "open") || oasp_ascii_equal(member, "close") ||
         oasp_ascii_equal(member, "seteos") || oasp_ascii_equal(member, "readtext") ||
         oasp_ascii_equal(member, "read")))
        return oasp_component_call(runtime, object, member, NULL, 0, line, result);
    if (object->component_kind == OASP_COMPONENT_OPENAI_STREAM &&
        (oasp_ascii_equal(member, "read") || oasp_ascii_equal(member, "close")))
        return oasp_component_call(runtime, object, member, NULL, 0, line, result);
    if ((object->component_kind == OASP_COMPONENT_ADO_CONNECTION && oasp_ascii_equal(member, "close")) ||
        (object->component_kind == OASP_COMPONENT_ADO_RECORDSET &&
         (oasp_ascii_equal(member, "close") || oasp_ascii_equal(member, "movenext") ||
          oasp_ascii_equal(member, "movefirst") || oasp_ascii_equal(member, "getrows"))))
        return oasp_component_call(runtime, object, member, NULL, 0, line, result);
    if (object->component_kind == OASP_COMPONENT_REGEXP && oasp_builtin_regexp_get(object, member, result))
        return true;
    if (oasp_builtin_stream_get(object, member, result) ||
        oasp_builtin_fso_get(runtime, object, member, line, result) ||
        oasp_builtin_xml_get(runtime, object, member, line, result) ||
        oasp_builtin_ado_get(runtime, object, member, line, result) ||
        oasp_builtin_process_get(object, member, result) ||
        oasp_builtin_redis_get_property(object, member, result) ||
        oasp_builtin_redis_multi_get(object, member, result) ||
        oasp_builtin_memcache_multi_get(object, member, result) ||
        oasp_builtin_memcache_get_property(object, member, result) ||
        oasp_builtin_openai_stream_get(object, member, result) ||
        oasp_builtin_websocket_get(object, member, result) ||
        oasp_builtin_socket_get(runtime, object, member, result))
        return true;
    if (object->component_kind == OASP_COMPONENT_HTTP) {
        oasp_http *http = object->native;
        if (oasp_ascii_equal(member, "status")) {
            *result = oasp_value_int(http->status);
            return true;
        }
        if (oasp_ascii_equal(member, "readystate")) {
            *result = oasp_value_int(http->ready_state);
            return true;
        }
        if (oasp_ascii_equal(member, "responsetext")) {
            *result = oasp_value_string(http->response.data, http->response.length);
            return true;
        }
        if (oasp_ascii_equal(member, "body")) {
            *result = oasp_value_string(http->body.data, http->body.length);
            return true;
        }
        if (oasp_ascii_equal(member, "contenttype")) {
            *result = oasp_value_string(http->content_type.data, http->content_type.length);
            return true;
        }
        if (oasp_ascii_equal(member, "completed")) {
            *result = oasp_value_bool(http->completed);
            return true;
        }
        if (oasp_ascii_equal(member, "errorcode")) {
            *result = oasp_value_int(http->error_code);
            return true;
        }
        if (oasp_ascii_equal(member, "errormessage")) {
            *result = oasp_value_string(http->error_message, strlen(http->error_message));
            return true;
        }
        if (oasp_ascii_equal(member, "tlsverify")) {
            *result = oasp_value_bool(http->tls_verify);
            return true;
        }
        if (oasp_ascii_equal(member, "cafile")) {
            *result = oasp_value_string(http->ca_file.data, http->ca_file.length);
            return true;
        }
        if (oasp_ascii_equal(member, "timeout")) {
            *result = oasp_value_int(http->timeout_ms);
            return true;
        }
        if (oasp_ascii_equal(member, "maxresponsebytes")) {
            *result = oasp_value_int((int64_t)http->max_response_bytes);
            return true;
        }
    }
    if (oasp_builtin_http_multi_get(object, member, result))
        return true;
    if (object->component_kind == OASP_COMPONENT_OPENAI) {
        oasp_openai *openai = object->native;
        if (oasp_ascii_equal(member, "status")) {
            *result = oasp_value_int(openai->status);
            return true;
        }
        if (oasp_ascii_equal(member, "lastresponse")) {
            *result = oasp_value_string(openai->last_response.data, openai->last_response.length);
            return true;
        }
        if (oasp_ascii_equal(member, "timeout")) {
            *result = oasp_value_int(openai->timeout_ms);
            return true;
        }
        if (oasp_ascii_equal(member, "idletimeout")) {
            *result = oasp_value_int(openai->idle_timeout_ms);
            return true;
        }
        if (oasp_ascii_equal(member, "maxeventbytes")) {
            *result = oasp_value_int((int64_t)openai->max_event_bytes);
            return true;
        }
        if (oasp_ascii_equal(member, "maxbufferedbytes")) {
            *result = oasp_value_int((int64_t)openai->max_buffered_bytes);
            return true;
        }
        if (oasp_ascii_equal(member, "maxresponsebytes")) {
            *result = oasp_value_int((int64_t)openai->max_response_bytes);
            return true;
        }
    }
    if (object->component_kind == OASP_COMPONENT_MATCH_COLLECTION ||
        object->component_kind == OASP_COMPONENT_SUBMATCH_COLLECTION ||
        object->component_kind == OASP_COMPONENT_XML_NODE_LIST ||
        object->component_kind == OASP_COMPONENT_FILE_COLLECTION ||
        object->component_kind == OASP_COMPONENT_FOLDER_COLLECTION ||
        object->component_kind == OASP_COMPONENT_ADO_FIELDS) {
        oasp_component_collection *collection = object->native;
        if (oasp_ascii_equal(member, "count") || oasp_ascii_equal(member, "length")) {
            *result = oasp_value_int((int64_t)collection->count);
            return true;
        }
    }
    if (object->component_kind == OASP_COMPONENT_MATCH &&
        oasp_builtin_regexp_match_get(runtime, object, member, result))
        return true;
    oasp_component_missing(runtime, object, member, line);
    *result = oasp_value_empty();
    return true;
}

bool oasp_builtin_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    if (!object || object->component_kind == OASP_COMPONENT_NONE)
        return false;
    if (object->component_kind == OASP_COMPONENT_DICTIONARY &&
        oasp_builtin_dictionary_get(runtime, object, member, result))
        return true;
    return component_get_properties(runtime, object, member, line, result);
}

bool oasp_builtin_component_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                                   oasp_value *result) {
    return oasp_builtin_ado_get_id(runtime, object, member_id, member, line, result) ||
           oasp_builtin_component_get(runtime, object, member, line, result);
}

bool oasp_builtin_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!object || object->component_kind == OASP_COMPONENT_NONE)
        return false;
    if ((object->component_kind == OASP_COMPONENT_DICTIONARY &&
         oasp_ascii_equal(member, "comparemode") &&
         oasp_builtin_dictionary_set(runtime, object, member, value, line)) ||
        (object->component_kind == OASP_COMPONENT_REGEXP &&
         oasp_builtin_regexp_set(runtime, object, member, value)) ||
        oasp_builtin_stream_set(runtime, object, member, value, line) ||
        oasp_builtin_xml_set(runtime, object, member, value, line) ||
        oasp_builtin_ado_set(runtime, object, member, value, line) ||
        oasp_builtin_http_set(runtime, object, member, value, line) ||
        oasp_builtin_http_multi_set(runtime, object, member, value, line) ||
        oasp_builtin_redis_multi_set(runtime, object, member, value, line) ||
        oasp_builtin_memcache_multi_set(runtime, object, member, value, line) ||
        oasp_builtin_process_set(runtime, object, member, value, line) ||
        oasp_builtin_websocket_set(runtime, object, member, value, line) ||
        oasp_builtin_socket_set(runtime, object, member, value, line) ||
        oasp_builtin_memcache_set(runtime, object, member, value, line) ||
        oasp_builtin_redis_set(runtime, object, member, value, line))
        return true;
    oasp_component_missing(runtime, object, member, line);
    return true;
}

bool oasp_builtin_component_call_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                                    size_t argument_count, size_t line, oasp_value *result) {
    if (!object || object->component_kind == OASP_COMPONENT_NONE)
        return false;
    if (oasp_builtin_ado_call(runtime, object, member_id, member, arguments, argument_count, line, result))
        return true;
    if (object->component_kind == OASP_COMPONENT_JSON || object->component_kind == OASP_COMPONENT_CRYPTO) {
        bool handled = object->component_kind == OASP_COMPONENT_JSON
                           ? oasp_builtin_json_call(runtime, member, arguments, argument_count, line, result)
                           : oasp_builtin_crypto_call(runtime, member, arguments, argument_count, line, result);
        if (handled)
            return true;
        oasp_component_missing(runtime, object, member, line);
        return true;
    }
    if ((object->component_kind == OASP_COMPONENT_SHELL &&
         oasp_builtin_shell_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_PROCESS &&
         oasp_builtin_process_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_WEBSOCKET &&
         oasp_builtin_websocket_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_SOCKET &&
         oasp_builtin_socket_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_MEMCACHE &&
         oasp_builtin_memcache_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_REDIS &&
         oasp_builtin_redis_call(runtime, object, member, arguments, argument_count, line, result)) ||
        ((object->component_kind == OASP_COMPONENT_REDIS_MULTI ||
          object->component_kind == OASP_COMPONENT_REDIS_MULTI_CLIENT) &&
         oasp_builtin_redis_multi_call(runtime, object, member, arguments, argument_count, line, result)) ||
        ((object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI ||
          object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI_CLIENT) &&
         oasp_builtin_memcache_multi_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_HTTP_MULTI &&
         oasp_builtin_http_multi_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_OPENAI_STREAM &&
         oasp_builtin_openai_stream_call(runtime, object, member, argument_count, line, result)) ||
        ((object->component_kind == OASP_COMPONENT_HTTP || object->component_kind == OASP_COMPONENT_OPENAI) &&
         oasp_builtin_http_call(runtime, object, member, arguments, argument_count, line, result)) ||
        oasp_builtin_stream_call(runtime, object, member, arguments, argument_count, line, result) ||
        oasp_builtin_fso_call(runtime, object, member, arguments, argument_count, line, result) ||
        oasp_builtin_xml_call(runtime, object, member, arguments, argument_count, line, result) ||
        (object->component_kind == OASP_COMPONENT_REGEXP &&
         oasp_builtin_regexp_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_MATCH &&
         oasp_builtin_regexp_match_call(runtime, object, member, arguments, argument_count, line, result)) ||
        (object->component_kind == OASP_COMPONENT_DICTIONARY &&
         oasp_builtin_dictionary_call(runtime, object, member, arguments, argument_count, line, result)))
        return true;
    if ((object->component_kind == OASP_COMPONENT_MATCH_COLLECTION ||
         object->component_kind == OASP_COMPONENT_SUBMATCH_COLLECTION ||
         object->component_kind == OASP_COMPONENT_XML_NODE_LIST) &&
        oasp_ascii_equal(member, "item"))
        return oasp_component_default(runtime, object, arguments, argument_count, line, result);
    oasp_component_missing(runtime, object, member, line);
    *result = oasp_value_empty();
    return true;
}
