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

bool oasp_builtin_kv_write_all(oasp_kv_pool_entry *connection, const void *data, size_t length) {
    if (!connection || connection->fd < 0)
        return false;
    const unsigned char *input = data;
    while (length) {
        ssize_t count = send(connection->fd, input, length, 0);
        if (count > 0) {
            input += count;
            length -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool oasp_builtin_kv_read_fill(oasp_kv_pool_entry *connection) {
    oasp_protocol_reader *reader = &connection->reader;
    if (reader->start && reader->start < reader->end) {
        memmove(reader->data, reader->data + reader->start, reader->end - reader->start);
        reader->end -= reader->start;
        reader->start = 0;
    } else if (reader->start == reader->end) {
        reader->start = 0;
        reader->end = 0;
    }
    if (reader->end == sizeof(reader->data)) {
        errno = EMSGSIZE;
        return false;
    }
    for (;;) {
        ssize_t count = recv(connection->fd, reader->data + reader->end, sizeof(reader->data) - reader->end, 0);
        if (count > 0) {
            reader->end += (size_t)count;
            return true;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
}

bool oasp_builtin_kv_read_exact(oasp_kv_pool_entry *connection, void *data, size_t length) {
    unsigned char *output = data;
    while (length) {
        oasp_protocol_reader *reader = &connection->reader;
        size_t available = reader->end - reader->start;
        if (available) {
            size_t count = available < length ? available : length;
            memcpy(output, reader->data + reader->start, count);
            reader->start += count;
            output += count;
            length -= count;
            continue;
        }
        if (length < sizeof(reader->data)) {
            if (!oasp_builtin_kv_read_fill(connection))
                return false;
            continue;
        }
        ssize_t count = recv(connection->fd, output, length, 0);
        if (count > 0) {
            output += count;
            length -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool oasp_builtin_kv_read_line(oasp_kv_pool_entry *connection, oasp_slice *line) {
    for (;;) {
        oasp_protocol_reader *reader = &connection->reader;
        for (size_t index = reader->start; index + 1 < reader->end; index++) {
            if (reader->data[index] == '\r' && reader->data[index + 1] == '\n') {
                reader->data[index] = '\0';
                line->data = (const char *)reader->data + reader->start;
                line->length = index - reader->start;
                reader->start = index + 2;
                return true;
            }
        }
        if (!oasp_builtin_kv_read_fill(connection))
            return false;
    }
}

void oasp_builtin_redis_finalize(void *context) {
    oasp_redis *redis = context;
    if (redis->connection_owned) {
        oasp_kv_pool_destroy(redis->connection);
        redis->connection = NULL;
        redis->connection_owned = false;
    } else {
        oasp_kv_pool_release(&redis->connection);
    }
    redis->opened = false;
}

size_t oasp_builtin_kv_argument_count(const oasp_value *arguments, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t addition = arguments[i].kind == OASP_VALUE_ARRAY
                              ? oasp_builtin_kv_argument_count(arguments[i].as.array->items, arguments[i].as.array->length)
                              : 1;
        if (addition > SIZE_MAX - total)
            return 0;
        total += addition;
    }
    return total;
}

bool oasp_builtin_kv_collect_arguments(oasp_runtime *runtime, const oasp_value *arguments, size_t count, oasp_builtin_kv_argument *output, size_t capacity,
                                       size_t *index, size_t *encoded_length) {
    for (size_t i = 0; i < count; i++) {
        if (arguments[i].kind == OASP_VALUE_ARRAY) {
            oasp_array *array = arguments[i].as.array;
            if (!oasp_builtin_kv_collect_arguments(runtime, array->items, array->length, output, capacity, index, encoded_length))
                return false;
        } else {
            if (*index >= capacity)
                return false;
            const char *text = NULL;
            size_t length = 0;
            if (arguments[i].kind == OASP_VALUE_STRING) {
                text = arguments[i].as.string.data;
                length = arguments[i].as.string.length;
            } else if (arguments[i].kind == OASP_VALUE_DATE) {
                text = arguments[i].as.date.data;
                length = arguments[i].as.date.length;
            } else {
                text = oasp_value_format(runtime->format_arena, &arguments[i]);
                length = text ? strlen(text) : 0;
            }
            if (!text)
                return false;
            char header[64];
            int header_length = snprintf(header, sizeof(header), "$%zu\r\n", length);
            if (header_length <= 0 || (size_t)header_length >= sizeof(header) || length > SIZE_MAX - *encoded_length - (size_t)header_length - 2) {
                errno = EOVERFLOW;
                return false;
            }
            output[*index] = (oasp_builtin_kv_argument){text, length};
            (*index)++;
            *encoded_length += (size_t)header_length + length + 2;
        }
    }
    return true;
}

static oasp_array *oasp_component_array_alloc(oasp_runtime *runtime, size_t count) {
    if (count > SIZE_MAX / sizeof(oasp_value)) {
        errno = EOVERFLOW;
        return NULL;
    }
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return NULL;
    memset(array, 0, sizeof(*array));
    array->rank = 1;
    array->length = count;
    array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
    array->items = count ? oasp_runtime_alloc_values(runtime, count) : NULL;
    if (!array->bounds || (count && !array->items))
        return NULL;
    array->bounds[0] = count ? (int64_t)count - 1 : -1;
    return array;
}

bool oasp_builtin_redis_read_value(oasp_runtime *runtime, oasp_kv_pool_entry *connection, size_t depth, oasp_value *result, oasp_slice *error) {
    if (depth > 64) {
        errno = EOVERFLOW;
        return false;
    }
    unsigned char marker = 0;
    if (!oasp_builtin_kv_read_exact(connection, &marker, 1))
        return false;
    oasp_slice line = {0};
    if (!oasp_builtin_kv_read_line(connection, &line))
        return false;
    if (marker == '+') {
        *result = redis_slice_value(runtime, line.data, line.length);
    } else if (marker == '-') {
        char *copy = oasp_runtime_copy(runtime, line.data, line.length);
        if (copy)
            *error = (oasp_slice){copy, line.length};
        *result = oasp_value_empty();
    } else if (marker == ':') {
        *result = oasp_value_int(strtoll(line.data, NULL, 10));
    } else if (marker == '#') {
        *result = oasp_value_bool(line.length && line.data[0] == 't');
    } else if (marker == '_') {
        *result = oasp_value_null();
    } else if (marker == ',') {
        *result = oasp_value_float(strtod(line.data, NULL));
    } else if (marker == '$') {
        long long bulk_length = strtoll(line.data, NULL, 10);
        if (bulk_length < 0) {
            *result = oasp_value_null();
        } else if ((uint64_t)bulk_length > SIZE_MAX - 1) {
            errno = EOVERFLOW;
            return false;
        } else {
            char *data = oasp_runtime_alloc(runtime, (size_t)bulk_length + 1, _Alignof(char));
            char suffix[2];
            if (!data || !oasp_builtin_kv_read_exact(connection, data, (size_t)bulk_length) ||
                !oasp_builtin_kv_read_exact(connection, suffix, sizeof(suffix)) || memcmp(suffix, "\r\n", 2)) {
                return false;
            }
            data[bulk_length] = '\0';
            *result = oasp_value_string(data, (size_t)bulk_length);
        }
    } else if (marker == '*' || marker == '~' || marker == '>' || marker == '%' || marker == '|') {
        long long element_count = strtoll(line.data, NULL, 10);
        if (element_count < 0) {
            *result = oasp_value_null();
        } else {
            size_t count = (size_t)element_count;
            if (marker == '%' || marker == '|') {
                if (count > SIZE_MAX / 2) {
                    errno = EOVERFLOW;
                    return false;
                }
                count *= 2;
            }
            oasp_array *array = oasp_component_array_alloc(runtime, count);
            if (!array)
                return false;
            for (size_t i = 0; i < count; i++) {
                if (!oasp_builtin_redis_read_value(runtime, connection, depth + 1, &array->items[i], error))
                    return false;
                if (error->length)
                    return true;
            }
            result->kind = OASP_VALUE_ARRAY;
            result->as.array = array;
        }
    } else {
        errno = EPROTO;
        return false;
    }
    return true;
}

bool oasp_builtin_redis_command_equal(const oasp_builtin_kv_argument *argument, const char *name) {
    size_t length = strlen(name);
    return argument->length == length && !strncasecmp(argument->data, name, length);
}

bool oasp_builtin_redis_command(oasp_runtime *runtime, oasp_object *object, const oasp_value *arguments, size_t count, size_t line,
                                oasp_value *result) {
    oasp_redis *redis = object->native;
    oasp_kv_pool_entry *connection = redis->connection;
    if (!redis->opened || !connection || connection->fd < 0) {
        oasp_runtime_raise(runtime, 91, "Redis connection is not open", object->component_name, line);
        return false;
    }
    size_t total = oasp_builtin_kv_argument_count(arguments, count);
    if (!total) {
        oasp_runtime_raise(runtime, 5, "Redis command name is required", object->component_name, line);
        return false;
    }
    oasp_builtin_kv_argument *flat = oasp_runtime_alloc(runtime, total * sizeof(*flat), _Alignof(oasp_builtin_kv_argument));
    char header[64];
    int header_length = snprintf(header, sizeof(header), "*%zu\r\n", total);
    size_t index = 0;
    size_t request_length = header_length > 0 ? (size_t)header_length : 0;
    bool encoded = flat && header_length > 0 && (size_t)header_length < sizeof(header) &&
                   oasp_builtin_kv_collect_arguments(runtime, arguments, count, flat, total, &index, &request_length) && index == total;
    char *request = encoded && request_length < SIZE_MAX ? oasp_runtime_alloc(runtime, request_length + 1, _Alignof(char)) : NULL;
    char *cursor = request;
    if (request) {
        memcpy(cursor, header, (size_t)header_length);
        cursor += header_length;
        for (size_t i = 0; i < total; i++) {
            int length = snprintf(cursor, request_length + 1 - (size_t)(cursor - request), "$%zu\r\n", flat[i].length);
            if (length <= 0) {
                request = NULL;
                break;
            }
            cursor += length;
            memcpy(cursor, flat[i].data, flat[i].length);
            cursor += flat[i].length;
            memcpy(cursor, "\r\n", 2);
            cursor += 2;
        }
    }
    bool sent = request && (size_t)(cursor - request) == request_length && oasp_builtin_kv_write_all(connection, request, request_length);
    oasp_slice error = {"", 0};
    if (!sent || !oasp_builtin_redis_read_value(runtime, connection, 0, result, &error)) {
        oasp_kv_pool_entry_close(connection);
        redis->opened = false;
        oasp_runtime_raise(runtime, 3000, "Redis protocol error", object->component_name, line);
        return false;
    }
    if (error.length) {
        char *message = oasp_runtime_copy(runtime, error.data, error.length);
        oasp_runtime_raise(runtime, 3000, message ? message : "Redis error", object->component_name, line);
        return false;
    }
    if (oasp_builtin_redis_command_equal(&flat[0], "MULTI") || oasp_builtin_redis_command_equal(&flat[0], "WATCH") ||
        oasp_builtin_redis_command_equal(&flat[0], "SUBSCRIBE") || oasp_builtin_redis_command_equal(&flat[0], "PSUBSCRIBE") ||
        oasp_builtin_redis_command_equal(&flat[0], "MONITOR"))
        connection->protocol_clean = false;
    else if (oasp_builtin_redis_command_equal(&flat[0], "EXEC") || oasp_builtin_redis_command_equal(&flat[0], "DISCARD") ||
             oasp_builtin_redis_command_equal(&flat[0], "UNWATCH"))
        connection->protocol_clean = true;
    else if (oasp_builtin_redis_command_equal(&flat[0], "QUIT")) {
        oasp_kv_pool_entry_close(connection);
        redis->opened = false;
    } else if (total > 1 && oasp_builtin_redis_command_equal(&flat[0], "SELECT")) {
        char *end = NULL;
        long database = strtol(flat[1].data, &end, 10);
        if (end == flat[1].data + flat[1].length && database >= 0 && database <= INT_MAX)
            connection->redis_database = (int)database;
    }
    return true;
}

bool oasp_builtin_redis_configure(oasp_runtime *runtime, oasp_redis *redis, const char *configuration) {
    char *copy = oasp_runtime_copy(runtime, configuration, strlen(configuration));
    if (!copy)
        return false;
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
            redis->host = oasp_component_string(runtime, equals);
        else if (oasp_ascii_equal(key, "port"))
            redis->port = atoi(equals);
        else if (oasp_ascii_equal(key, "username") || oasp_ascii_equal(key, "user"))
            redis->username = oasp_component_string(runtime, equals);
        else if (oasp_ascii_equal(key, "password"))
            redis->password = oasp_component_string(runtime, equals);
        else if (oasp_ascii_equal(key, "database") || oasp_ascii_equal(key, "db"))
            redis->database = atoi(equals);
        else if (oasp_ascii_equal(key, "timeout"))
            redis->timeout_ms = strtoll(equals, NULL, 10);
    }
    return true;
}

bool oasp_builtin_redis_open(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t count, size_t line) {
    oasp_redis *redis = object->native;
    oasp_builtin_redis_finalize(redis);
    if (count) {
        char *first = oasp_value_format(runtime->arena, &arguments[0]);
        if (first && strchr(first, '=')) {
            if (!oasp_builtin_redis_configure(runtime, redis, first))
                return false;
        } else {
            redis->host = oasp_component_string(runtime, first ? first : "");
            int64_t integer = 0;
            if (count > 1 && oasp_value_to_int(&arguments[1], &integer))
                redis->port = (int)integer;
            if (count > 2)
                redis->password = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[2]));
            if (count > 3 && oasp_value_to_int(&arguments[3], &integer))
                redis->database = (int)integer;
            if (count > 4)
                redis->username = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[4]));
        }
    }
    size_t endpoint_length = redis->host.length + 32;
    size_t identity_length = redis->host.length + redis->username.length + redis->password.length + 64;
    char *endpoint = oasp_runtime_alloc(runtime, endpoint_length, _Alignof(char));
    char *identity = oasp_runtime_alloc(runtime, identity_length, _Alignof(char));
    if (!endpoint || !identity ||
        snprintf(endpoint, endpoint_length, "%.*s:%d", (int)redis->host.length, redis->host.data, redis->port) < 0 ||
        snprintf(identity, identity_length, "%.*s:%d:%.*s:%.*s", (int)redis->host.length, redis->host.data, redis->port,
                 (int)redis->username.length, redis->username.data, (int)redis->password.length, redis->password.data) < 0) {
        oasp_runtime_raise(runtime, 3709, "Redis connection pool allocation failed", object->component_name, line);
        return false;
    }
    redis->connection = oasp_kv_pool_acquire(redis->component_context, true, endpoint, identity);
    if (!redis->connection) {
        oasp_runtime_raise(runtime, 3709, "Redis connection pool limit reached", object->component_name, line);
        return false;
    }
    redis->connection_owned = redis->component_context == NULL;
    if (redis->connection->fd >= 0) {
        oasp_socket timeout = {.timeout = redis->timeout_ms};
        oasp_builtin_socket_timeout_apply(&timeout, redis->connection->fd);
        redis->opened = true;
        if (redis->connection->redis_database != redis->database) {
            oasp_value select[2] = {oasp_value_string("SELECT", 6), oasp_value_int(redis->database)};
            oasp_value reply = oasp_value_empty();
            if (!oasp_builtin_redis_command(runtime, object, select, 2, line, &reply)) {
                oasp_builtin_redis_finalize(redis);
                return false;
            }
            redis->connection->redis_database = redis->database;
        }
        return true;
    }
    char port[16];
    (void)snprintf(port, sizeof(port), "%d", redis->port);
    struct addrinfo *addresses = NULL;
    if (!oasp_builtin_socket_network_address(redis->host.data, port, SOCK_STREAM, false, &addresses)) {
        oasp_runtime_raise(runtime, 3709, "Redis connection failed", object->component_name, line);
        oasp_builtin_redis_finalize(redis);
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
        oasp_runtime_raise(runtime, 3709, "Redis connection failed", object->component_name, line);
        oasp_builtin_redis_finalize(redis);
        return false;
    }
    oasp_socket timeout = {.timeout = redis->timeout_ms};
    oasp_builtin_socket_timeout_apply(&timeout, fd);
    redis->connection->fd = fd;
    redis->connection->reader.start = 0;
    redis->connection->reader.end = 0;
    redis->connection->protocol_clean = true;
    redis->connection->redis_database = 0;
    redis->opened = true;
    oasp_value reply = oasp_value_empty();
    if (redis->password.length) {
        oasp_value auth[3];
        size_t auth_count = 0;
        auth[auth_count++] = oasp_value_string("AUTH", 4);
        if (redis->username.length)
            auth[auth_count++] = oasp_value_string(redis->username.data, redis->username.length);
        auth[auth_count++] = oasp_value_string(redis->password.data, redis->password.length);
        if (!oasp_builtin_redis_command(runtime, object, auth, auth_count, line, &reply)) {
            oasp_builtin_redis_finalize(redis);
            return false;
        }
    }
    if (redis->database) {
        oasp_value select[2] = {oasp_value_string("SELECT", 6), oasp_value_int(redis->database)};
        if (!oasp_builtin_redis_command(runtime, object, select, 2, line, &reply)) {
            oasp_builtin_redis_finalize(redis);
            return false;
        }
        redis->connection->redis_database = redis->database;
    }
    return true;
}

static int oasp_builtin_redis_minimum_arguments(oasp_slice member) {
    if (oasp_ascii_equal(member, "ping"))
        return 0;
    if (oasp_ascii_equal(member, "get") || oasp_ascii_equal(member, "del") || oasp_ascii_equal(member, "exists") || oasp_ascii_equal(member, "ttl") ||
        oasp_ascii_equal(member, "incr") || oasp_ascii_equal(member, "decr") || oasp_ascii_equal(member, "hgetall") || oasp_ascii_equal(member, "lpop") ||
        oasp_ascii_equal(member, "rpop") || oasp_ascii_equal(member, "smembers") || oasp_ascii_equal(member, "select") || oasp_ascii_equal(member, "auth"))
        return 1;
    if (oasp_ascii_equal(member, "set") || oasp_ascii_equal(member, "expire") || oasp_ascii_equal(member, "lpush") || oasp_ascii_equal(member, "rpush") ||
        oasp_ascii_equal(member, "sadd") || oasp_ascii_equal(member, "hget"))
        return 2;
    if (oasp_ascii_equal(member, "hset") || oasp_ascii_equal(member, "setex") || oasp_ascii_equal(member, "lrange"))
        return 3;
    return -1;
}

bool oasp_builtin_redis_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                             size_t line, oasp_value *result) {
    oasp_redis *redis = object->native;
    if (oasp_ascii_equal(member, "open")) {
        (void)oasp_builtin_redis_open(runtime, object, arguments, count, line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "close")) {
        oasp_builtin_redis_finalize(redis);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "execute")) {
        if (!count) {
            oasp_runtime_raise(runtime, 450, "Execute requires at least 1 argument(s)", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        (void)oasp_builtin_redis_command(runtime, object, arguments, count, line, result);
        return true;
    }
    int minimum = oasp_builtin_redis_minimum_arguments(member);
    if (minimum < 0)
        return false;
    if (count < (size_t)minimum) {
        char message[128];
        (void)snprintf(message, sizeof(message), "%.*s requires at least %d argument(s)", (int)member.length, member.data, minimum);
        oasp_runtime_raise(runtime, 450, message, object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    oasp_value *command = oasp_runtime_alloc(runtime, (count + 1) * sizeof(*command), _Alignof(oasp_value));
    if (!command) {
        *result = oasp_value_empty();
        return true;
    }
    command[0] = oasp_value_string(member.data, member.length);
    if (oasp_ascii_equal(member, "setex")) {
        command[1] = arguments[0];
        command[2] = arguments[2];
        command[3] = arguments[1];
    } else {
        memcpy(command + 1, arguments, count * sizeof(*command));
    }
    (void)oasp_builtin_redis_command(runtime, object, command, count + 1, line, result);
    if (!runtime->error_number && oasp_ascii_equal(member, "select")) {
        int64_t database = 0;
        if (oasp_value_to_int(&arguments[0], &database)) {
            redis->database = (int)database;
            if (redis->connection)
                redis->connection->redis_database = (int)database;
        }
    }
    return true;
}

bool oasp_builtin_redis_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind != OASP_COMPONENT_REDIS)
        return false;
    oasp_redis *redis = object->native;
    if (oasp_ascii_equal(member, "host") || oasp_ascii_equal(member, "username") || oasp_ascii_equal(member, "password")) {
        oasp_slice assigned = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
        if (oasp_ascii_equal(member, "host"))
            redis->host = assigned;
        else if (oasp_ascii_equal(member, "username"))
            redis->username = assigned;
        else
            redis->password = assigned;
        return true;
    }
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "port")) {
        if (oasp_value_to_int(value, &integer) && integer > 0 && integer <= 65535)
            redis->port = (int)integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid Redis port", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "database")) {
        if (oasp_value_to_int(value, &integer) && integer >= 0)
            redis->database = (int)integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid Redis database", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "timeout")) {
        if (oasp_value_to_int(value, &integer) && integer > 0)
            redis->timeout_ms = integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid Redis timeout", object->component_name, line);
        return true;
    }
    return false;
}

bool oasp_builtin_redis_get_property(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_REDIS)
        return false;
    oasp_redis *redis = object->native;
    if (oasp_ascii_equal(member, "host"))
        *result = oasp_value_string(redis->host.data, redis->host.length);
    else if (oasp_ascii_equal(member, "port"))
        *result = oasp_value_int(redis->port);
    else if (oasp_ascii_equal(member, "database"))
        *result = oasp_value_int(redis->database);
    else if (oasp_ascii_equal(member, "timeout"))
        *result = oasp_value_int(redis->timeout_ms);
    else if (oasp_ascii_equal(member, "state"))
        *result = oasp_value_int(redis->opened ? 1 : 0);
    else
        return false;
    return true;
}
