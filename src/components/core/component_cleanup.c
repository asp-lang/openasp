// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"

static void trace_value(oasp_allocator *allocator, const oasp_value *value) {
    if (!value)
        return;
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_trace(allocator, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_trace(allocator, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(allocator, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(allocator, value->as.object);
}

static void trace_slice(oasp_allocator *allocator, oasp_slice value) {
    oasp_allocator_trace(allocator, value.data);
}

void oasp_builtin_component_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    if (!allocator || !object || !object->native)
        return;
    oasp_allocator_trace(allocator, object->native);
    switch (object->component_kind) {
    case OASP_COMPONENT_DICTIONARY:
        oasp_builtin_dictionary_gc_trace(allocator, object->native);
        break;
    case OASP_COMPONENT_REGEXP:
    case OASP_COMPONENT_MATCH:
        oasp_builtin_regexp_gc_trace(allocator, object);
        break;
    case OASP_COMPONENT_STREAM:
        trace_slice(allocator, ((oasp_stream *)object->native)->charset);
        break;
    case OASP_COMPONENT_XML:
        trace_slice(allocator, ((oasp_xml_document *)object->native)->error_reason);
        break;
    case OASP_COMPONENT_XML_NODE:
        oasp_allocator_trace(allocator, ((oasp_xml_node *)object->native)->owner);
        break;
    case OASP_COMPONENT_HTTP: {
        oasp_http *http = object->native;
        trace_slice(allocator, http->method);
        trace_slice(allocator, http->url);
        trace_slice(allocator, http->content_type);
        trace_slice(allocator, http->ca_file);
        break;
    }
    case OASP_COMPONENT_OPENAI: {
        oasp_openai *openai = object->native;
        trace_slice(allocator, openai->api_key);
        trace_slice(allocator, openai->base_url);
        trace_slice(allocator, openai->model);
        trace_slice(allocator, openai->last_response);
        break;
    }
    case OASP_COMPONENT_ADO_CONNECTION: {
        oasp_ado_connection *connection = object->native;
        for (oasp_ado_recordset *recordset = connection->active_recordsets; recordset; recordset = recordset->next_active)
            oasp_allocator_trace(allocator, recordset);
        trace_slice(allocator, connection->pool_key);
        trace_slice(allocator, connection->database_path);
        trace_slice(allocator, connection->connection_string);
        break;
    }
    case OASP_COMPONENT_ADO_RECORDSET: {
        oasp_ado_recordset *recordset = object->native;
        oasp_allocator_trace(allocator, recordset->columns);
        oasp_allocator_trace(allocator, recordset->connection);
        trace_slice(allocator, recordset->source);
        for (size_t i = 0; i < recordset->column_count; i++)
            oasp_allocator_trace(allocator, recordset->columns[i].data);
        break;
    }
    case OASP_COMPONENT_ADO_FIELD:
        oasp_allocator_trace(allocator, ((oasp_ado_field *)object->native)->recordset);
        break;
    case OASP_COMPONENT_ADO_COMMAND: {
        oasp_ado_command *command = object->native;
        oasp_allocator_trace(allocator, command->connection);
        oasp_allocator_trace(allocator, command->parameters);
        trace_slice(allocator, command->command_text);
        break;
    }
    case OASP_COMPONENT_ADO_PARAMETERS: {
        oasp_ado_parameters *parameters = object->native;
        for (oasp_ado_parameter_link *link = parameters->first; link; link = link->next) {
            oasp_allocator_trace(allocator, link);
            oasp_allocator_trace(allocator, link->parameter);
        }
        break;
    }
    case OASP_COMPONENT_ADO_PARAMETER: {
        oasp_ado_parameter *parameter = object->native;
        trace_slice(allocator, parameter->name);
        trace_value(allocator, &parameter->value);
        break;
    }
    case OASP_COMPONENT_SOCKET: {
        oasp_socket *socket = object->native;
        trace_slice(allocator, socket->protocol);
        trace_slice(allocator, socket->host);
        trace_slice(allocator, socket->port);
        trace_slice(allocator, socket->peer_host);
        break;
    }
    case OASP_COMPONENT_PROCESS: {
        oasp_process *process = object->native;
        trace_slice(allocator, process->standard_output);
        trace_slice(allocator, process->standard_error);
        break;
    }
    case OASP_COMPONENT_WEBSOCKET: {
        oasp_websocket *socket = object->native;
        trace_slice(allocator, socket->url);
        trace_slice(allocator, socket->subprotocol);
        trace_slice(allocator, socket->ca_file);
        trace_slice(allocator, socket->message_type);
        break;
    }
    case OASP_COMPONENT_MEMCACHE: {
        oasp_memcache *cache = object->native;
        trace_slice(allocator, cache->host);
        trace_slice(allocator, cache->protocol);
        trace_slice(allocator, cache->ca_file);
        break;
    }
    case OASP_COMPONENT_REDIS: {
        oasp_redis *redis = object->native;
        trace_slice(allocator, redis->host);
        trace_slice(allocator, redis->username);
        trace_slice(allocator, redis->password);
        break;
    }
    case OASP_COMPONENT_MATCH_COLLECTION:
    case OASP_COMPONENT_SUBMATCH_COLLECTION:
    case OASP_COMPONENT_XML_NODE_LIST:
    case OASP_COMPONENT_FILE_COLLECTION:
    case OASP_COMPONENT_FOLDER_COLLECTION:
    case OASP_COMPONENT_ADO_FIELDS: {
        oasp_component_collection *collection = object->native;
        oasp_allocator_trace(allocator, collection->items);
        for (size_t i = 0; i < collection->count; i++)
            trace_value(allocator, &collection->items[i]);
        break;
    }
    case OASP_COMPONENT_FILE:
    case OASP_COMPONENT_FOLDER:
        trace_slice(allocator, ((oasp_file_info *)object->native)->path);
        break;
    default:
        break;
    }
}
