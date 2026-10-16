// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "memcache_multi_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool memcache_multi_argument_equal(const oasp_builtin_kv_argument *argument, const char *name) {
    size_t length = strlen(name);
    if (argument->length != length)
        return false;
    for (size_t i = 0; i < length; i++)
        if (tolower((unsigned char)argument->data[i]) != tolower((unsigned char)name[i]))
            return false;
    return true;
}

static bool memcache_multi_argument_integer(const oasp_builtin_kv_argument *argument, int64_t *value) {
    if (!argument->length || argument->length >= 64)
        return false;
    char text[64];
    memcpy(text, argument->data, argument->length);
    text[argument->length] = '\0';
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno || end != text + argument->length)
        return false;
    *value = parsed;
    return true;
}

static bool memcache_multi_append_token(oasp_buffer *buffer, const oasp_builtin_kv_argument *argument) {
    return oasp_buffer_append(buffer, argument->data, argument->length);
}

static bool memcache_multi_encode_command(oasp_runtime *runtime, const oasp_value *arguments, size_t argument_count, oasp_slice *request, oasp_memcache_multi_command_kind *kind,
                                          const char **error) {
    size_t count = oasp_builtin_kv_argument_count(arguments, argument_count);
    if (!count || count > SIZE_MAX / sizeof(oasp_builtin_kv_argument)) {
        *error = "Memcache command name is required";
        return false;
    }
    oasp_builtin_kv_argument *flat = oasp_runtime_alloc(runtime, count * sizeof(*flat), _Alignof(oasp_builtin_kv_argument));
    size_t index = 0;
    size_t encoded_length = 0;
    if (!flat || !oasp_builtin_kv_collect_arguments(runtime, arguments, argument_count, flat, count, &index, &encoded_length) ||
        index != count) {
        *error = "unable to encode Memcache command";
        return false;
    }
    oasp_buffer buffer = {0};
    if (encoded_length > SIZE_MAX - 64 || !oasp_buffer_init(&buffer, encoded_length + 64)) {
        *error = "unable to allocate Memcache command buffer";
        return false;
    }
    bool ok = false;
    if ((memcache_multi_argument_equal(&flat[0], "get") ||
         memcache_multi_argument_equal(&flat[0], "gets")) &&
        count == 2 && oasp_builtin_memcache_key_valid(flat[1].data, flat[1].length)) {
        *kind = memcache_multi_argument_equal(&flat[0], "gets") ? OASP_MEMCACHE_MULTI_GETS : OASP_MEMCACHE_MULTI_GET;
        ok = memcache_multi_append_token(&buffer, &flat[0]) &&
             oasp_buffer_append(&buffer, " ", 1) &&
             memcache_multi_append_token(&buffer, &flat[1]) &&
             oasp_buffer_append(&buffer, "\r\n", 2);
    } else if (memcache_multi_argument_equal(&flat[0], "set") ||
               memcache_multi_argument_equal(&flat[0], "add") ||
               memcache_multi_argument_equal(&flat[0], "replace") ||
               memcache_multi_argument_equal(&flat[0], "append") ||
               memcache_multi_argument_equal(&flat[0], "prepend") ||
               memcache_multi_argument_equal(&flat[0], "cas")) {
        bool cas = memcache_multi_argument_equal(&flat[0], "cas");
        size_t minimum = cas ? 4 : 3;
        int64_t cas_value = 0;
        int64_t flags = 0;
        int64_t ttl = 0;
        size_t flags_index = cas ? 4 : 3;
        size_t ttl_index = cas ? 5 : 4;
        bool valid = count >= minimum && count <= minimum + 2 &&
                     oasp_builtin_memcache_key_valid(flat[1].data, flat[1].length);
        if (cas)
            valid = valid && memcache_multi_argument_integer(&flat[3], &cas_value) && cas_value >= 0;
        if (count > flags_index)
            valid = valid && memcache_multi_argument_integer(&flat[flags_index], &flags) && flags >= 0;
        if (count > ttl_index)
            valid = valid && memcache_multi_argument_integer(&flat[ttl_index], &ttl) && ttl >= 0;
        char header[1024];
        int header_length =
            cas ? snprintf(header, sizeof(header), "%.*s %.*s %lld %lld %zu %lld\r\n",
                           (int)flat[0].length, flat[0].data, (int)flat[1].length, flat[1].data,
                           (long long)flags, (long long)ttl, flat[2].length, (long long)cas_value)
                : snprintf(header, sizeof(header), "%.*s %.*s %lld %lld %zu\r\n",
                           (int)flat[0].length, flat[0].data, (int)flat[1].length, flat[1].data,
                           (long long)flags, (long long)ttl, flat[2].length);
        *kind = OASP_MEMCACHE_MULTI_STORAGE;
        ok = valid && header_length > 0 && (size_t)header_length < sizeof(header) &&
             oasp_buffer_append(&buffer, header, (size_t)header_length) &&
             oasp_buffer_append(&buffer, flat[2].data, flat[2].length) &&
             oasp_buffer_append(&buffer, "\r\n", 2);
    } else if (memcache_multi_argument_equal(&flat[0], "delete") && count == 2 &&
               oasp_builtin_memcache_key_valid(flat[1].data, flat[1].length)) {
        *kind = OASP_MEMCACHE_MULTI_DELETE;
        ok = oasp_buffer_append(&buffer, "delete ", 7) &&
             memcache_multi_append_token(&buffer, &flat[1]) &&
             oasp_buffer_append(&buffer, "\r\n", 2);
    } else if (memcache_multi_argument_equal(&flat[0], "touch") && count == 3 &&
               oasp_builtin_memcache_key_valid(flat[1].data, flat[1].length)) {
        int64_t ttl = 0;
        *kind = OASP_MEMCACHE_MULTI_TOUCH;
        ok = memcache_multi_argument_integer(&flat[2], &ttl) && ttl >= 0 &&
             oasp_buffer_append(&buffer, "touch ", 6) &&
             memcache_multi_append_token(&buffer, &flat[1]) &&
             oasp_buffer_append(&buffer, " ", 1) &&
             memcache_multi_append_token(&buffer, &flat[2]) &&
             oasp_buffer_append(&buffer, "\r\n", 2);
    } else if ((memcache_multi_argument_equal(&flat[0], "incr") ||
                memcache_multi_argument_equal(&flat[0], "decr")) &&
               count == 3 && oasp_builtin_memcache_key_valid(flat[1].data, flat[1].length)) {
        int64_t delta = 0;
        *kind = OASP_MEMCACHE_MULTI_COUNTER;
        ok = memcache_multi_argument_integer(&flat[2], &delta) && delta >= 0 &&
             memcache_multi_append_token(&buffer, &flat[0]) &&
             oasp_buffer_append(&buffer, " ", 1) &&
             memcache_multi_append_token(&buffer, &flat[1]) &&
             oasp_buffer_append(&buffer, " ", 1) &&
             memcache_multi_append_token(&buffer, &flat[2]) &&
             oasp_buffer_append(&buffer, "\r\n", 2);
    } else if (memcache_multi_argument_equal(&flat[0], "version") && count == 1) {
        *kind = OASP_MEMCACHE_MULTI_VERSION;
        ok = oasp_buffer_append(&buffer, "version\r\n", 9);
    } else if ((memcache_multi_argument_equal(&flat[0], "flush_all") ||
                memcache_multi_argument_equal(&flat[0], "flushall")) &&
               (count == 1 || count == 2)) {
        int64_t delay = 0;
        *kind = OASP_MEMCACHE_MULTI_FLUSH;
        ok = (count == 1 || (memcache_multi_argument_integer(&flat[1], &delay) && delay >= 0)) &&
             oasp_buffer_append(&buffer, "flush_all", 9);
        if (ok && count == 2)
            ok = oasp_buffer_append(&buffer, " ", 1) && memcache_multi_append_token(&buffer, &flat[1]);
        if (ok)
            ok = oasp_buffer_append(&buffer, "\r\n", 2);
    } else {
        *error = "unsupported or invalid MemcacheMulti command arguments";
    }
    char *copy = ok ? oasp_runtime_copy(runtime, buffer.data, buffer.length) : NULL;
    size_t length = buffer.length;
    oasp_buffer_dispose(&buffer);
    if (!copy) {
        if (!*error)
            *error = "unable to encode Memcache command";
        return false;
    }
    *request = (oasp_slice){copy, length};
    return true;
}

static bool memcache_multi_client_configure(oasp_runtime *runtime, oasp_memcache_multi_client *client, const char *configuration) {
    char *copy = oasp_runtime_copy(runtime, configuration, strlen(configuration));
    if (!copy)
        return false;
    bool valid = true;
    char *cursor = copy;
    while (*cursor) {
        char *entry = cursor;
        char *separator = strchr(cursor, ';');
        if (separator) {
            *separator = '\0';
            cursor = separator + 1;
        } else {
            cursor += strlen(cursor);
        }
        char *equals = strchr(entry, '=');
        if (!equals)
            continue;
        *equals++ = '\0';
        while (isspace((unsigned char)*entry))
            entry++;
        while (isspace((unsigned char)*equals))
            equals++;
        char *key_end = entry + strlen(entry);
        char *value_end = equals + strlen(equals);
        while (key_end > entry && isspace((unsigned char)key_end[-1]))
            *--key_end = '\0';
        while (value_end > equals && isspace((unsigned char)value_end[-1]))
            *--value_end = '\0';
        oasp_slice key = {entry, strlen(entry)};
        if (oasp_ascii_equal(key, "host") || oasp_ascii_equal(key, "server"))
            client->host = oasp_component_string(runtime, equals);
        else if (oasp_ascii_equal(key, "port"))
            client->port = atoi(equals);
        else if (oasp_ascii_equal(key, "timeout"))
            client->timeout_ms = strtoll(equals, NULL, 10);
        else if (oasp_ascii_equal(key, "protocol"))
            valid = valid && !strcasecmp(equals, "text");
        else if (oasp_ascii_equal(key, "tls"))
            valid = valid && (!strcasecmp(equals, "false") || !strcmp(equals, "0") || !strcasecmp(equals, "off"));
    }
    return valid;
}

void *oasp_builtin_memcache_multi_create(oasp_runtime *runtime) {
    oasp_memcache_multi *multi = oasp_runtime_alloc(runtime, sizeof(*multi), _Alignof(oasp_memcache_multi));
    if (!multi)
        return NULL;
    memset(multi, 0, sizeof(*multi));
    multi->component_context = runtime->component_context;
    multi->max_concurrency = 8;
    multi->timeout_ms = 30000;
    multi->max_request_bytes = 4194304;
    multi->max_response_bytes = 16777216;
    return multi;
}

static void memcache_multi_trace_value(oasp_allocator *allocator, const oasp_value *value) {
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_trace(allocator, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_trace(allocator, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(allocator, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(allocator, value->as.object);
}

void oasp_builtin_memcache_multi_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI) {
        const oasp_memcache_multi *multi = object->native;
        for (oasp_object *item = multi->first; item; item = ((oasp_memcache_multi_client *)item->native)->next)
            oasp_allocator_trace(allocator, item);
    } else if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI_CLIENT) {
        const oasp_memcache_multi_client *client = object->native;
        oasp_allocator_trace(allocator, client->owner);
        oasp_allocator_trace(allocator, client->next);
        oasp_allocator_trace(allocator, client->host.data);
        for (oasp_object *item = client->first; item; item = ((oasp_memcache_multi_command *)item->native)->next)
            oasp_allocator_trace(allocator, item);
    } else if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI_COMMAND) {
        const oasp_memcache_multi_command *command = object->native;
        oasp_allocator_trace(allocator, command->owner);
        oasp_allocator_trace(allocator, command->next);
        oasp_allocator_trace(allocator, command->request.data);
        oasp_allocator_trace(allocator, command->error_message.data);
        memcache_multi_trace_value(allocator, &command->value);
    }
}

static bool memcache_multi_get_batch(oasp_memcache_multi *multi, oasp_slice member, oasp_value *result) {
    if (oasp_ascii_equal(member, "count") || oasp_ascii_equal(member, "commandcount"))
        *result = oasp_value_int((int64_t)multi->command_count);
    else if (oasp_ascii_equal(member, "clientcount"))
        *result = oasp_value_int((int64_t)multi->client_count);
    else if (oasp_ascii_equal(member, "maxconcurrency"))
        *result = oasp_value_int((int64_t)multi->max_concurrency);
    else if (oasp_ascii_equal(member, "timeout"))
        *result = oasp_value_int(multi->timeout_ms);
    else if (oasp_ascii_equal(member, "maxrequestbytes"))
        *result = oasp_value_int((int64_t)multi->max_request_bytes);
    else if (oasp_ascii_equal(member, "maxresponsebytes"))
        *result = oasp_value_int((int64_t)multi->max_response_bytes);
    else
        return false;
    return true;
}

bool oasp_builtin_memcache_multi_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (!object || !object->native)
        return false;
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI)
        return memcache_multi_get_batch(object->native, member, result);
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI_CLIENT) {
        oasp_memcache_multi_client *client = object->native;
        if (oasp_ascii_equal(member, "count"))
            *result = oasp_value_int((int64_t)client->count);
        else if (oasp_ascii_equal(member, "host"))
            *result = oasp_value_string(client->host.data, client->host.length);
        else if (oasp_ascii_equal(member, "port"))
            *result = oasp_value_int(client->port);
        else if (oasp_ascii_equal(member, "timeout"))
            *result = oasp_value_int(client->timeout_ms);
        else
            return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI_COMMAND) {
        oasp_memcache_multi_command *command = object->native;
        if (oasp_ascii_equal(member, "value"))
            *result = command->value;
        else if (oasp_ascii_equal(member, "errorcode"))
            *result = oasp_value_int(command->error_code);
        else if (oasp_ascii_equal(member, "errormessage"))
            *result = oasp_value_string(command->error_message.data, command->error_message.length);
        else if (oasp_ascii_equal(member, "completed"))
            *result = oasp_value_bool(command->completed);
        else if (oasp_ascii_equal(member, "maybeexecuted"))
            *result = oasp_value_bool(command->maybe_executed);
        else if (oasp_ascii_equal(member, "flags"))
            *result = oasp_value_int(command->flags);
        else if (oasp_ascii_equal(member, "cas"))
            *result = oasp_value_int(command->cas);
        else
            return false;
        return true;
    }
    return false;
}

bool oasp_builtin_memcache_multi_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!object || object->component_kind != OASP_COMPONENT_MEMCACHE_MULTI)
        return false;
    oasp_memcache_multi *multi = object->native;
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "maxconcurrency")) {
        if (!oasp_value_to_int(value, &integer) || integer < 1 || integer > 256)
            oasp_runtime_raise(runtime, 5, "MaxConcurrency must be between 1 and 256", object->component_name, line);
        else
            multi->max_concurrency = (size_t)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "timeout")) {
        if (!oasp_value_to_int(value, &integer) || integer < 1)
            oasp_runtime_raise(runtime, 5, "Timeout must be a positive millisecond value", object->component_name, line);
        else
            multi->timeout_ms = integer;
        return true;
    }
    if (oasp_ascii_equal(member, "maxrequestbytes") || oasp_ascii_equal(member, "maxresponsebytes")) {
        if (!oasp_value_to_int(value, &integer) || integer < 1 || (uint64_t)integer > SIZE_MAX)
            oasp_runtime_raise(runtime, 5, "MemcacheMulti byte limits must be positive", object->component_name, line);
        else if (oasp_ascii_equal(member, "maxrequestbytes"))
            multi->max_request_bytes = (size_t)integer;
        else
            multi->max_response_bytes = (size_t)integer;
        return true;
    }
    return false;
}

static oasp_object *memcache_multi_create_client(oasp_runtime *runtime, oasp_object *owner_object, const char *configuration, size_t line) {
    oasp_memcache_multi *multi = owner_object->native;
    oasp_object *client_object = oasp_component_object(runtime, OASP_COMPONENT_MEMCACHE_MULTI_CLIENT, "OpenASP.MemcacheMultiClient", NULL);
    if (!client_object)
        return NULL;
    oasp_gc_root root;
    oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &client_object, 1, NULL, 0);
    oasp_memcache_multi_client *client = oasp_runtime_alloc(runtime, sizeof(*client), _Alignof(oasp_memcache_multi_client));
    if (!client) {
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    memset(client, 0, sizeof(*client));
    client->owner = multi;
    client->component_context = multi->component_context;
    client->host = (oasp_slice){"127.0.0.1", 9};
    client->port = 11211;
    client->timeout_ms = 5000;
    client->state = OASP_MEMCACHE_MULTI_IDLE;
    client_object->native = client;
    if (!memcache_multi_client_configure(runtime, client, configuration) || !client->host.length ||
        client->port < 1 || client->port > 65535 || client->timeout_ms < 1) {
        oasp_runtime_gc_root_leave(runtime, &root);
        oasp_runtime_raise(runtime, 5, "invalid MemcacheMulti client configuration", owner_object->component_name, line);
        return NULL;
    }
    oasp_object *previous = multi->last;
    if (previous)
        ((oasp_memcache_multi_client *)previous->native)->next = client_object;
    else
        multi->first = client_object;
    multi->last = client_object;
    multi->client_count++;
    if (runtime->allocator) {
        oasp_allocator_write_barrier(runtime->allocator, client_object, client);
        oasp_allocator_write_barrier(runtime->allocator, client, multi);
        oasp_allocator_write_barrier(runtime->allocator, multi, client_object);
        if (previous)
            oasp_allocator_write_barrier(runtime->allocator, previous->native, client_object);
    }
    oasp_runtime_gc_root_leave(runtime, &root);
    return client_object;
}

static oasp_object *memcache_multi_create_command(oasp_runtime *runtime, oasp_object *client_object, oasp_value *arguments, size_t argument_count, size_t line) {
    oasp_memcache_multi_client *client = client_object->native;
    oasp_memcache_multi *multi = client->owner;
    oasp_object *command_object = oasp_component_object(runtime, OASP_COMPONENT_MEMCACHE_MULTI_COMMAND, "OpenASP.MemcacheMultiCommand", NULL);
    if (!command_object)
        return NULL;
    oasp_gc_root root;
    oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &command_object, 1, NULL, 0);
    oasp_memcache_multi_command *command = oasp_runtime_alloc(runtime, sizeof(*command), _Alignof(oasp_memcache_multi_command));
    if (!command) {
        oasp_runtime_gc_root_leave(runtime, &root);
        return NULL;
    }
    memset(command, 0, sizeof(*command));
    command->owner = client;
    command->value = oasp_value_empty();
    command_object->native = command;
    oasp_slice request = {0};
    const char *error = NULL;
    if (!memcache_multi_encode_command(runtime, arguments, argument_count, &request, &command->kind, &error)) {
        oasp_runtime_gc_root_leave(runtime, &root);
        oasp_runtime_raise(runtime, 5, error, client_object->component_name, line);
        return NULL;
    }
    if (request.length > multi->max_request_bytes || multi->request_bytes > multi->max_request_bytes - request.length) {
        oasp_runtime_gc_root_leave(runtime, &root);
        oasp_runtime_raise(runtime, 7, "MemcacheMulti request size limit exceeded", client_object->component_name, line);
        return NULL;
    }
    command->request = request;
    oasp_object *previous = client->last;
    if (previous)
        ((oasp_memcache_multi_command *)previous->native)->next = command_object;
    else
        client->first = command_object;
    client->last = command_object;
    client->count++;
    multi->command_count++;
    multi->request_bytes += request.length;
    if (runtime->allocator) {
        oasp_allocator_write_barrier(runtime->allocator, command_object, command);
        oasp_allocator_write_barrier(runtime->allocator, command, client);
        oasp_allocator_write_barrier(runtime->allocator, command, request.data);
        oasp_allocator_write_barrier(runtime->allocator, client, command_object);
        if (previous)
            oasp_allocator_write_barrier(runtime->allocator, previous->native, command_object);
    }
    oasp_runtime_gc_root_leave(runtime, &root);
    return command_object;
}

bool oasp_builtin_memcache_multi_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                      size_t line, oasp_value *result) {
    if (!object || !object->native)
        return false;
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI && oasp_ascii_equal(member, "execute") && !argument_count) {
        oasp_object *rooted_object = object;
        oasp_gc_root root;
        oasp_runtime_gc_root_enter(runtime, &root, NULL, 0, &rooted_object, 1, NULL, 0);
        (void)oasp_builtin_memcache_multi_execute(runtime, object, object->native, line);
        oasp_runtime_gc_root_leave(runtime, &root);
        *result = oasp_value_empty();
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI && oasp_ascii_equal(member, "createclient")) {
        if (argument_count != 1) {
            oasp_runtime_raise(runtime, 450, "CreateClient requires one configuration argument", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        char *configuration = oasp_value_format(runtime->arena, &arguments[0]);
        oasp_object *client = configuration ? memcache_multi_create_client(runtime, object, configuration, line) : NULL;
        if (!client && !runtime->error_number)
            oasp_runtime_raise(runtime, 7, "unable to create MemcacheMulti client", object->component_name, line);
        *result = client ? oasp_component_object_value(client) : oasp_value_empty();
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_MEMCACHE_MULTI_CLIENT && oasp_ascii_equal(member, "createcommand")) {
        if (!argument_count) {
            oasp_runtime_raise(runtime, 450, "CreateCommand requires at least one argument", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        oasp_object *command = memcache_multi_create_command(runtime, object, arguments, argument_count, line);
        if (!command && !runtime->error_number)
            oasp_runtime_raise(runtime, 7, "unable to create MemcacheMulti command", object->component_name, line);
        *result = command ? oasp_component_object_value(command) : oasp_value_empty();
        return true;
    }
    return false;
}
