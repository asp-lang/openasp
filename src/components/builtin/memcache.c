// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static oasp_value redis_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static const unsigned char *redis_value_bytes(oasp_runtime *runtime, const oasp_value *value, size_t *length) {
    if (value->kind == OASP_VALUE_STRING) {
        *length = value->as.string.length;
        return (const unsigned char *)value->as.string.data;
    }
    char *text = oasp_value_format(runtime->arena, value);
    *length = text ? strlen(text) : 0;
    return (const unsigned char *)text;
}

void oasp_builtin_memcache_finalize(void *context) {
    oasp_memcache *cache = context;
    if (cache->connection_owned) {
        oasp_kv_pool_destroy(cache->connection);
        cache->connection = NULL;
        cache->connection_owned = false;
    } else {
        oasp_kv_pool_release(&cache->connection);
    }
    cache->opened = false;
}

bool oasp_builtin_memcache_open(oasp_runtime *runtime, oasp_object *object, size_t line) {
    oasp_memcache *cache = object->native;
    oasp_builtin_memcache_finalize(cache);
    if (cache->tls_enabled || !oasp_ascii_equal(cache->protocol, "text")) {
        errno = ENOTSUP;
        return false;
    }
    size_t endpoint_length = cache->host.length + 32;
    size_t identity_length = cache->host.length + cache->protocol.length + 48;
    char *endpoint = oasp_runtime_alloc(runtime, endpoint_length, _Alignof(char));
    char *identity = oasp_runtime_alloc(runtime, identity_length, _Alignof(char));
    if (!endpoint || !identity ||
        snprintf(endpoint, endpoint_length, "%.*s:%d", (int)cache->host.length, cache->host.data, cache->port) < 0 ||
        snprintf(identity, identity_length, "%.*s:%d:%.*s:%d", (int)cache->host.length, cache->host.data, cache->port,
                 (int)cache->protocol.length, cache->protocol.data, cache->tls_enabled ? 1 : 0) < 0) {
        errno = ENOMEM;
        return false;
    }
    cache->connection = oasp_kv_pool_acquire(cache->component_context, false, endpoint, identity);
    if (!cache->connection)
        return false;
    cache->connection_owned = cache->component_context == NULL;
    if (cache->connection->fd >= 0) {
        oasp_socket timeout = {.timeout = cache->timeout_ms};
        oasp_builtin_socket_timeout_apply(&timeout, cache->connection->fd);
        cache->opened = true;
        return true;
    }
    char port[16];
    (void)snprintf(port, sizeof(port), "%d", cache->port);
    struct addrinfo *addresses = NULL;
    if (!oasp_builtin_socket_network_address(cache->host.data, port, SOCK_STREAM, false, &addresses)) {
        oasp_builtin_memcache_finalize(cache);
        return false;
    }
    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd >= 0 && !connect(fd, address->ai_addr, address->ai_addrlen))
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        oasp_builtin_memcache_finalize(cache);
        return false;
    }
    oasp_socket timeout = {.timeout = cache->timeout_ms};
    oasp_builtin_socket_timeout_apply(&timeout, fd);
    cache->connection->fd = fd;
    cache->connection->reader.start = 0;
    cache->connection->reader.end = 0;
    cache->connection->protocol_clean = true;
    cache->opened = true;
    (void)line;
    return true;
}

bool oasp_builtin_memcache_send_line(oasp_memcache *cache, const char *line) {
    return cache->opened && cache->connection && cache->connection->fd >= 0 &&
           oasp_builtin_kv_write_all(cache->connection, line, strlen(line));
}

void oasp_builtin_memcache_raise(oasp_runtime *runtime, oasp_object *object, const char *operation, size_t line) {
    oasp_memcache *cache = object->native;
    char message[160];
    (void)snprintf(message, sizeof(message), "%s failed with Memcache error %d", operation, errno);
    if (cache->connection)
        oasp_kv_pool_entry_close(cache->connection);
    cache->opened = false;
    oasp_runtime_raise(runtime, 13000 + errno, message, object->component_name, line);
}

bool oasp_builtin_memcache_key_valid(const char *key, size_t length);

bool oasp_builtin_memcache_storage(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                   size_t line, oasp_value *result) {
    oasp_memcache *cache = object->native;
    if (count < 2 || !cache->opened)
        return false;
    char *key = oasp_value_format(runtime->arena, &arguments[0]);
    size_t value_length = 0;
    const unsigned char *value = redis_value_bytes(runtime, &arguments[1], &value_length);
    int64_t flags = 0;
    int64_t ttl = 0;
    int64_t cas = 0;
    bool is_cas = oasp_ascii_equal(member, "cas");
    if (!oasp_builtin_memcache_key_valid(key, key ? strlen(key) : 0)) {
        oasp_runtime_raise(runtime, 5, "invalid Memcache key", object->component_name, line);
        return true;
    }
    if (is_cas) {
        if (count < 3)
            return false;
        (void)oasp_value_to_int(&arguments[2], &cas);
        if (count > 3)
            (void)oasp_value_to_int(&arguments[3], &flags);
        if (count > 4)
            (void)oasp_value_to_int(&arguments[4], &ttl);
    } else {
        if (count > 2)
            (void)oasp_value_to_int(&arguments[2], &flags);
        if (count > 3)
            (void)oasp_value_to_int(&arguments[3], &ttl);
    }
    char header[1024];
    int header_length =
        is_cas ? snprintf(header, sizeof(header), "cas %s %lld %lld %zu %lld\r\n", key, (long long)flags, (long long)ttl, value_length, (long long)cas)
               : snprintf(header, sizeof(header), "%.*s %s %lld %lld %zu\r\n", (int)member.length, member.data, key, (long long)flags, (long long)ttl,
                          value_length);
    oasp_slice response = {0};
    bool ok = header_length > 0 && (size_t)header_length < sizeof(header) &&
              oasp_builtin_kv_write_all(cache->connection, header, (size_t)header_length) &&
              oasp_builtin_kv_write_all(cache->connection, value, value_length) &&
              oasp_builtin_kv_write_all(cache->connection, "\r\n", 2) &&
              oasp_builtin_kv_read_line(cache->connection, &response);
    *result = oasp_value_bool(ok && response.length == 6 && !memcmp(response.data, "STORED", 6));
    if (!ok)
        oasp_builtin_memcache_raise(runtime, object, "Storage", line);
    return true;
}

bool oasp_builtin_memcache_get(oasp_runtime *runtime, oasp_object *object, const char *key, bool with_cas, size_t line,
                               oasp_value *result) {
    oasp_memcache *cache = object->native;
    char command[1024];
    int command_length = oasp_builtin_memcache_key_valid(key, key ? strlen(key) : 0)
                             ? snprintf(command, sizeof(command), "%s %s\r\n", with_cas ? "gets" : "get", key)
                             : -1;
    oasp_slice response = {0};
    if (command_length <= 0 || (size_t)command_length >= sizeof(command)) {
        oasp_runtime_raise(runtime, 5, "invalid Memcache key", object->component_name, line);
        return true;
    }
    if (!oasp_builtin_memcache_send_line(cache, command) || !oasp_builtin_kv_read_line(cache->connection, &response)) {
        oasp_builtin_memcache_raise(runtime, object, "Get", line);
        return true;
    }
    if (response.length == 3 && !memcmp(response.data, "END", 3)) {
        cache->last_flags = 0;
        cache->last_cas = 0;
        *result = oasp_value_null();
        return true;
    }
    long long flags = 0;
    long long cas = 0;
    size_t length = 0;
    int fields = sscanf(response.data, "VALUE %*s %lld %zu %lld", &flags, &length, &cas);
    char *data = oasp_runtime_alloc(runtime, length + 1, _Alignof(char));
    char suffix[2];
    bool ok = fields >= 2 && data && oasp_builtin_kv_read_exact(cache->connection, data, length) &&
              oasp_builtin_kv_read_exact(cache->connection, suffix, sizeof(suffix)) && !memcmp(suffix, "\r\n", 2) &&
              oasp_builtin_kv_read_line(cache->connection, &response) && response.length == 3 && !memcmp(response.data, "END", 3);
    if (!ok) {
        oasp_builtin_memcache_raise(runtime, object, "Get", line);
        return true;
    }
    data[length] = '\0';
    cache->last_flags = flags;
    cache->last_cas = fields >= 3 ? cas : 0;
    *result = oasp_value_string(data, length);
    return true;
}

bool oasp_builtin_memcache_key_valid(const char *key, size_t length) {
    if (!key || !length || length > 250)
        return false;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)key[i] <= 32 || (unsigned char)key[i] == 127)
            return false;
    return true;
}

static bool oasp_dictionary_append_owned(oasp_runtime *runtime, oasp_dictionary *dictionary, oasp_slice key, oasp_value value) {
    oasp_dictionary_entry *entry = oasp_runtime_alloc(runtime, sizeof(*entry), _Alignof(oasp_dictionary_entry));
    if (!entry)
        return false;
    memset(entry, 0, sizeof(*entry));
    entry->key = oasp_value_string(key.data, key.length);
    entry->value = value;
    oasp_runtime_write_barrier(runtime, entry, &entry->key);
    oasp_runtime_write_barrier(runtime, entry, &entry->value);
    if (dictionary->last) {
        dictionary->last->next = entry;
        if (runtime->allocator)
            oasp_allocator_write_barrier(runtime->allocator, dictionary->last, entry);
    } else {
        dictionary->first = entry;
    }
    dictionary->last = entry;
    if (runtime->allocator)
        oasp_allocator_write_barrier(runtime->allocator, dictionary, entry);
    dictionary->count++;
    return true;
}

bool oasp_builtin_memcache_get_multi(oasp_runtime *runtime, oasp_object *object, const oasp_value *arguments, size_t count, size_t line,
                                     oasp_value *result) {
    oasp_memcache *cache = object->native;
    size_t key_count = oasp_builtin_kv_argument_count(arguments, count);
    oasp_builtin_kv_argument *keys = key_count ? oasp_runtime_alloc(runtime, key_count * sizeof(*keys), _Alignof(oasp_builtin_kv_argument)) : NULL;
    size_t index = 0;
    size_t ignored = 0;
    if (!key_count || !keys || !oasp_builtin_kv_collect_arguments(runtime, arguments, count, keys, key_count, &index, &ignored) || index != key_count) {
        oasp_runtime_raise(runtime, 450, "GetMulti requires one or more keys", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    size_t command_length = 5;
    for (size_t i = 0; i < key_count; i++) {
        if (!oasp_builtin_memcache_key_valid(keys[i].data, keys[i].length) || keys[i].length > SIZE_MAX - command_length - 1) {
            oasp_runtime_raise(runtime, 5, "invalid Memcache key", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        command_length += keys[i].length + 1;
    }
    char *command = oasp_runtime_alloc(runtime, command_length, _Alignof(char));
    if (!command) {
        *result = oasp_value_empty();
        return true;
    }
    char *cursor = command;
    memcpy(cursor, "get", 3);
    cursor += 3;
    for (size_t i = 0; i < key_count; i++) {
        *cursor++ = ' ';
        memcpy(cursor, keys[i].data, keys[i].length);
        cursor += keys[i].length;
    }
    memcpy(cursor, "\r\n", 2);
    oasp_dictionary *dictionary = oasp_runtime_alloc(runtime, sizeof(*dictionary), _Alignof(oasp_dictionary));
    if (!dictionary || !oasp_builtin_kv_write_all(cache->connection, command, command_length)) {
        oasp_builtin_memcache_raise(runtime, object, "GetMulti", line);
        *result = oasp_value_empty();
        return true;
    }
    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->compare_mode = 0;
    for (;;) {
        oasp_slice response = {0};
        if (!oasp_builtin_kv_read_line(cache->connection, &response)) {
            oasp_builtin_memcache_raise(runtime, object, "GetMulti", line);
            *result = oasp_value_empty();
            return true;
        }
        if (response.length == 3 && !memcmp(response.data, "END", 3))
            break;
        char key_buffer[251];
        long long flags = 0;
        size_t length = 0;
        if (sscanf(response.data, "VALUE %250s %lld %zu", key_buffer, &flags, &length) != 3) {
            errno = EPROTO;
            oasp_builtin_memcache_raise(runtime, object, "GetMulti", line);
            *result = oasp_value_empty();
            return true;
        }
        char *key = oasp_runtime_copy(runtime, key_buffer, strlen(key_buffer));
        char *data = length < SIZE_MAX ? oasp_runtime_alloc(runtime, length + 1, _Alignof(char)) : NULL;
        char suffix[2];
        if (!key || !data || !oasp_builtin_kv_read_exact(cache->connection, data, length) ||
            !oasp_builtin_kv_read_exact(cache->connection, suffix, sizeof(suffix)) || memcmp(suffix, "\r\n", 2)) {
            oasp_builtin_memcache_raise(runtime, object, "GetMulti", line);
            *result = oasp_value_empty();
            return true;
        }
        data[length] = '\0';
        oasp_value value = oasp_value_string(data, length);
        if (!oasp_dictionary_append_owned(runtime, dictionary, (oasp_slice){key, strlen(key)}, value)) {
            *result = oasp_value_empty();
            return true;
        }
        cache->last_flags = flags;
    }
    oasp_object *dictionary_object = oasp_component_object(runtime, OASP_COMPONENT_DICTIONARY, "Scripting.Dictionary", dictionary);
    *result = dictionary_object ? oasp_component_object_value(dictionary_object) : oasp_value_empty();
    return true;
}

bool oasp_builtin_memcache_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                size_t line, oasp_value *result) {
    oasp_memcache *cache = object->native;
    if (oasp_ascii_equal(member, "open") || oasp_ascii_equal(member, "connect")) {
        if (count)
            cache->host = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[0]));
        int64_t integer = 0;
        if (count > 1 && oasp_value_to_int(&arguments[1], &integer))
            cache->port = (int)integer;
        if (count > 2 && oasp_value_to_int(&arguments[2], &integer))
            cache->timeout_ms = integer;
        if (!oasp_builtin_memcache_open(runtime, object, line))
            oasp_builtin_memcache_raise(runtime, object, "Open", line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "close")) {
        oasp_builtin_memcache_finalize(cache);
        *result = oasp_value_empty();
        return true;
    }
    if (!cache->opened) {
        oasp_runtime_raise(runtime, 91, "operation requires an open Memcache connection", object->component_name, line);
        return true;
    }
    if ((oasp_ascii_equal(member, "get") || oasp_ascii_equal(member, "gets")) && count) {
        char *key = oasp_value_format(runtime->arena, &arguments[0]);
        return oasp_builtin_memcache_get(runtime, object, key, oasp_ascii_equal(member, "gets"), line, result);
    }
    if (oasp_ascii_equal(member, "getmulti"))
        return oasp_builtin_memcache_get_multi(runtime, object, arguments, count, line, result);
    if (oasp_ascii_equal(member, "set") || oasp_ascii_equal(member, "add") || oasp_ascii_equal(member, "replace") || oasp_ascii_equal(member, "append") ||
        oasp_ascii_equal(member, "prepend") || oasp_ascii_equal(member, "cas"))
        return oasp_builtin_memcache_storage(runtime, object, member, arguments, count, line, result);
    if ((oasp_ascii_equal(member, "delete") || oasp_ascii_equal(member, "touch") || oasp_ascii_equal(member, "incr") || oasp_ascii_equal(member, "decr")) &&
        count) {
        char *key = oasp_value_format(runtime->arena, &arguments[0]);
        int64_t number = 0;
        if (count > 1)
            (void)oasp_value_to_int(&arguments[1], &number);
        char command[1024];
        int length = oasp_ascii_equal(member, "delete")
                         ? snprintf(command, sizeof(command), "delete %s\r\n", key)
                         : snprintf(command, sizeof(command), "%.*s %s %lld\r\n", (int)member.length, member.data, key, (long long)number);
        oasp_slice response = {0};
        bool ok = length > 0 && (size_t)length < sizeof(command) && oasp_builtin_memcache_send_line(cache, command) &&
                  oasp_builtin_kv_read_line(cache->connection, &response);
        if (!ok) {
            oasp_builtin_memcache_raise(runtime, object, "Mutation", line);
            return true;
        }
        if (oasp_ascii_equal(member, "incr") || oasp_ascii_equal(member, "decr"))
            *result = oasp_value_int(strtoll(response.data, NULL, 10));
        else
            *result = oasp_value_bool((response.length == 7 && !memcmp(response.data, "DELETED", 7)) ||
                                      (response.length == 7 && !memcmp(response.data, "TOUCHED", 7)));
        return true;
    }
    if (oasp_ascii_equal(member, "version") || oasp_ascii_equal(member, "flushall")) {
        const char *command = oasp_ascii_equal(member, "version") ? "version\r\n" : "flush_all\r\n";
        oasp_slice response = {0};
        bool ok = oasp_builtin_memcache_send_line(cache, command) && oasp_builtin_kv_read_line(cache->connection, &response);
        if (!ok)
            oasp_builtin_memcache_raise(runtime, object, "Command", line);
        else if (oasp_ascii_equal(member, "version")) {
            size_t length = response.length >= 8 ? response.length - 8 : 0;
            *result = redis_slice_value(runtime, response.data + 8, length);
        } else {
            *result = oasp_value_bool(response.length == 2 && !memcmp(response.data, "OK", 2));
        }
        return true;
    }
    if (oasp_ascii_equal(member, "stats")) {
        if (!oasp_builtin_memcache_send_line(cache, "stats\r\n")) {
            oasp_builtin_memcache_raise(runtime, object, "Stats", line);
            return true;
        }
        oasp_dictionary *dictionary = oasp_builtin_dictionary_create(runtime, 0);
        if (!dictionary)
            return true;
        oasp_object *dictionary_object = oasp_component_object(runtime, OASP_COMPONENT_DICTIONARY, "OpenASP.JSON.Object", dictionary);
        if (!dictionary_object)
            return true;
        bool complete = false;
        for (;;) {
            oasp_slice response = {0};
            if (!oasp_builtin_kv_read_line(cache->connection, &response))
                break;
            if (response.length == 3 && !memcmp(response.data, "END", 3)) {
                complete = true;
                break;
            }
            char key[256];
            char value[1024];
            if (sscanf(response.data, "STAT %255s %1023s", key, value) == 2) {
                oasp_value key_value = redis_slice_value(runtime, key, strlen(key));
                oasp_value value_value = redis_slice_value(runtime, value, strlen(value));
                (void)oasp_builtin_dictionary_assign(runtime, dictionary_object, &key_value, &value_value, line);
            }
        }
        if (!complete) {
            oasp_builtin_memcache_raise(runtime, object, "Stats", line);
            *result = oasp_value_empty();
            return true;
        }
        *result = dictionary_object ? oasp_component_object_value(dictionary_object) : oasp_value_empty();
        return true;
    }
    return false;
}

bool oasp_builtin_memcache_get_property(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_MEMCACHE)
        return false;
    oasp_memcache *cache = object->native;
    if (oasp_ascii_equal(member, "host"))
        *result = oasp_value_string(cache->host.data, cache->host.length);
    else if (oasp_ascii_equal(member, "port"))
        *result = oasp_value_int(cache->port);
    else if (oasp_ascii_equal(member, "protocol"))
        *result = oasp_value_string(cache->protocol.data, cache->protocol.length);
    else if (oasp_ascii_equal(member, "timeout"))
        *result = oasp_value_int(cache->timeout_ms);
    else if (oasp_ascii_equal(member, "tls"))
        *result = oasp_value_bool(cache->tls_enabled);
    else if (oasp_ascii_equal(member, "tlsverify"))
        *result = oasp_value_bool(cache->tls_verify);
    else if (oasp_ascii_equal(member, "cafile"))
        *result = oasp_value_string(cache->ca_file.data, cache->ca_file.length);
    else if (oasp_ascii_equal(member, "state"))
        *result = oasp_value_int(cache->opened ? 1 : 0);
    else if (oasp_ascii_equal(member, "lastflags"))
        *result = oasp_value_int(cache->last_flags);
    else if (oasp_ascii_equal(member, "lastcas"))
        *result = oasp_value_int(cache->last_cas);
    else
        return false;
    return true;
}

bool oasp_builtin_memcache_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind != OASP_COMPONENT_MEMCACHE)
        return false;
    oasp_memcache *cache = object->native;
    if (cache->opened) {
        oasp_runtime_raise(runtime, 5, "close Memcache before changing configuration", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "host") || oasp_ascii_equal(member, "protocol") || oasp_ascii_equal(member, "cafile")) {
        oasp_slice assigned = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
        if (oasp_ascii_equal(member, "host"))
            cache->host = assigned;
        else if (oasp_ascii_equal(member, "protocol"))
            cache->protocol = assigned;
        else
            cache->ca_file = assigned;
        return true;
    }
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "port")) {
        if (oasp_value_to_int(value, &integer) && integer > 0 && integer <= 65535)
            cache->port = (int)integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid Memcache port", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "timeout")) {
        if (oasp_value_to_int(value, &integer) && integer > 0)
            cache->timeout_ms = integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid Memcache timeout", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "tls") || oasp_ascii_equal(member, "tlsverify")) {
        if (oasp_ascii_equal(member, "tls"))
            cache->tls_enabled = oasp_value_truthy(value);
        else
            cache->tls_verify = oasp_value_truthy(value);
        return true;
    }
    return false;
}
