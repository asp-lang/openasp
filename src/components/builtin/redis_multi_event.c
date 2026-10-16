// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "redis_multi_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#else
#error "RedisMulti requires epoll or kqueue"
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define OASP_REDIS_MULTI_EVENT_LIMIT 64
#define OASP_REDIS_MULTI_PARSE_DEPTH 64

enum {
    OASP_REDIS_EVENT_READ = 1,
    OASP_REDIS_EVENT_WRITE = 2,
    OASP_REDIS_EVENT_ERROR = 4
};

typedef struct {
    oasp_redis_multi_client *client;
    unsigned flags;
} redis_multi_event;

typedef struct {
    int descriptor;
} redis_multi_loop;

typedef enum {
    REDIS_PARSE_COMPLETE,
    REDIS_PARSE_INCOMPLETE,
    REDIS_PARSE_INVALID
} redis_parse_status;

static int64_t redis_multi_now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

#if defined(__linux__)
static bool redis_multi_loop_init(redis_multi_loop *loop) {
    loop->descriptor = epoll_create1(EPOLL_CLOEXEC);
    return loop->descriptor >= 0;
}

static bool redis_multi_loop_add(redis_multi_loop *loop, oasp_redis_multi_client *client, bool writing) {
    struct epoll_event event = {0};
    event.events = (writing ? EPOLLOUT : EPOLLIN) | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = client;
    if (epoll_ctl(loop->descriptor, EPOLL_CTL_ADD, client->connection->fd, &event))
        return false;
    client->registered = true;
    return true;
}

static bool redis_multi_loop_set(redis_multi_loop *loop, oasp_redis_multi_client *client, bool writing) {
    struct epoll_event event = {0};
    event.events = (writing ? EPOLLOUT : EPOLLIN) | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = client;
    return epoll_ctl(loop->descriptor, EPOLL_CTL_MOD, client->connection->fd, &event) == 0;
}

static void redis_multi_loop_remove(redis_multi_loop *loop, oasp_redis_multi_client *client) {
    if (client->registered)
        (void)epoll_ctl(loop->descriptor, EPOLL_CTL_DEL, client->connection->fd, NULL);
    client->registered = false;
}

static int redis_multi_loop_wait(redis_multi_loop *loop, redis_multi_event *output, size_t capacity, int timeout_ms) {
    struct epoll_event events[OASP_REDIS_MULTI_EVENT_LIMIT];
    int count = epoll_wait(loop->descriptor, events, (int)capacity, timeout_ms);
    for (int i = 0; i < count; i++) {
        unsigned flags = 0;
        if (events[i].events & EPOLLIN)
            flags |= OASP_REDIS_EVENT_READ;
        if (events[i].events & EPOLLOUT)
            flags |= OASP_REDIS_EVENT_WRITE;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            flags |= OASP_REDIS_EVENT_ERROR;
        output[i] = (redis_multi_event){events[i].data.ptr, flags};
    }
    return count;
}
#else
static bool redis_multi_loop_init(redis_multi_loop *loop) {
    loop->descriptor = kqueue();
    if (loop->descriptor < 0)
        return false;
    (void)fcntl(loop->descriptor, F_SETFD, FD_CLOEXEC);
    return true;
}

static bool redis_multi_loop_add(redis_multi_loop *loop, oasp_redis_multi_client *client, bool writing) {
    struct kevent changes[2];
    EV_SET(&changes[0], client->connection->fd, EVFILT_READ, EV_ADD | EV_DISABLE, 0, 0, client);
    EV_SET(&changes[1], client->connection->fd, EVFILT_WRITE, EV_ADD | (writing ? EV_ENABLE : EV_DISABLE), 0, 0, client);
    if (kevent(loop->descriptor, changes, 2, NULL, 0, NULL))
        return false;
    if (!writing) {
        struct kevent enable;
        EV_SET(&enable, client->connection->fd, EVFILT_READ, EV_ENABLE, 0, 0, client);
        if (kevent(loop->descriptor, &enable, 1, NULL, 0, NULL))
            return false;
    }
    client->registered = true;
    return true;
}

static bool redis_multi_loop_set(redis_multi_loop *loop, oasp_redis_multi_client *client, bool writing) {
    struct kevent changes[2];
    EV_SET(&changes[0], client->connection->fd, EVFILT_READ, writing ? EV_DISABLE : EV_ENABLE, 0, 0, client);
    EV_SET(&changes[1], client->connection->fd, EVFILT_WRITE, writing ? EV_ENABLE : EV_DISABLE, 0, 0, client);
    return kevent(loop->descriptor, changes, 2, NULL, 0, NULL) == 0;
}

static void redis_multi_loop_remove(redis_multi_loop *loop, oasp_redis_multi_client *client) {
    if (client->registered) {
        struct kevent changes[2];
        EV_SET(&changes[0], client->connection->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        EV_SET(&changes[1], client->connection->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        (void)kevent(loop->descriptor, changes, 2, NULL, 0, NULL);
    }
    client->registered = false;
}

static int redis_multi_loop_wait(redis_multi_loop *loop, redis_multi_event *output, size_t capacity, int timeout_ms) {
    struct kevent events[OASP_REDIS_MULTI_EVENT_LIMIT];
    struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    int count = kevent(loop->descriptor, NULL, 0, events, (int)capacity, &timeout);
    for (int i = 0; i < count; i++) {
        unsigned flags = events[i].filter == EVFILT_READ ? OASP_REDIS_EVENT_READ : OASP_REDIS_EVENT_WRITE;
        if (events[i].flags & (EV_ERROR | EV_EOF))
            flags |= OASP_REDIS_EVENT_ERROR;
        output[i] = (redis_multi_event){events[i].udata, flags};
    }
    return count;
}
#endif

static void redis_multi_loop_dispose(redis_multi_loop *loop) {
    if (loop->descriptor >= 0)
        close(loop->descriptor);
    loop->descriptor = -1;
}

static void redis_multi_command_error(oasp_runtime *runtime, oasp_redis_multi_command *command, int code, const char *message, size_t message_length,
                                      bool maybe_executed) {
    char *copy = message_length ? oasp_runtime_copy(runtime, message, message_length) : NULL;
    command->value = oasp_value_empty();
    command->error_code = code;
    command->error_message = (oasp_slice){copy ? copy : "", copy ? message_length : 0};
    command->completed = true;
    command->maybe_executed = maybe_executed;
    if (runtime->allocator && copy)
        oasp_allocator_write_barrier(runtime->allocator, command, copy);
}

static void redis_multi_fail_client(oasp_runtime *runtime, oasp_redis_multi_client *client, int code, const char *message) {
    size_t length = strlen(message);
    for (oasp_object *item = client->first; item; item = ((oasp_redis_multi_command *)item->native)->next) {
        oasp_redis_multi_command *command = item->native;
        if (!command->completed)
            redis_multi_command_error(runtime, command, code, message, length, client->business_bytes_sent);
    }
    if (client->connection)
        oasp_kv_pool_entry_close(client->connection);
    client->state = OASP_REDIS_MULTI_FAILED;
}

static void redis_multi_fail_errno(oasp_runtime *runtime, oasp_redis_multi_client *client, int error) {
    if (!error)
        error = EIO;
    redis_multi_fail_client(runtime, client, error, strerror(error));
}

static bool redis_multi_parse_number(const char *data, size_t length, long long *value) {
    if (!length || length >= 64)
        return false;
    char text[64];
    memcpy(text, data, length);
    text[length] = '\0';
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno || end != text + length)
        return false;
    *value = parsed;
    return true;
}

static bool redis_multi_find_line(const char *data, size_t length, size_t start, size_t *line_end) {
    for (size_t i = start; i + 1 < length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            *line_end = i;
            return true;
        }
    }
    return false;
}

static redis_parse_status redis_multi_scan_value(const char *data, size_t length, size_t offset, size_t depth, size_t *end) {
    if (depth > OASP_REDIS_MULTI_PARSE_DEPTH)
        return REDIS_PARSE_INVALID;
    if (offset >= length)
        return REDIS_PARSE_INCOMPLETE;
    char marker = data[offset];
    size_t line_end = 0;
    if (!redis_multi_find_line(data, length, offset + 1, &line_end))
        return REDIS_PARSE_INCOMPLETE;
    size_t after_line = line_end + 2;
    if (marker == '+' || marker == '-' || marker == ':' || marker == ',' || marker == '#' ||
        marker == '_' || marker == '(') {
        *end = after_line;
        return REDIS_PARSE_COMPLETE;
    }
    long long count = 0;
    if (!redis_multi_parse_number(data + offset + 1, line_end - offset - 1, &count))
        return REDIS_PARSE_INVALID;
    if (marker == '$' || marker == '!' || marker == '=') {
        if (count < 0) {
            *end = after_line;
            return count == -1 ? REDIS_PARSE_COMPLETE : REDIS_PARSE_INVALID;
        }
        if ((uint64_t)count > SIZE_MAX - after_line - 2)
            return REDIS_PARSE_INVALID;
        size_t frame_end = after_line + (size_t)count + 2;
        if (frame_end > length)
            return REDIS_PARSE_INCOMPLETE;
        if (data[frame_end - 2] != '\r' || data[frame_end - 1] != '\n')
            return REDIS_PARSE_INVALID;
        *end = frame_end;
        return REDIS_PARSE_COMPLETE;
    }
    if (marker != '*' && marker != '~' && marker != '>' && marker != '%' && marker != '|')
        return REDIS_PARSE_INVALID;
    if (count < 0) {
        *end = after_line;
        return count == -1 ? REDIS_PARSE_COMPLETE : REDIS_PARSE_INVALID;
    }
    size_t elements = (size_t)count;
    if ((marker == '%' || marker == '|') && elements > SIZE_MAX / 2)
        return REDIS_PARSE_INVALID;
    if (marker == '%' || marker == '|')
        elements *= 2;
    size_t cursor = after_line;
    for (size_t i = 0; i < elements; i++) {
        redis_parse_status status = redis_multi_scan_value(data, length, cursor, depth + 1, &cursor);
        if (status != REDIS_PARSE_COMPLETE)
            return status;
    }
    *end = cursor;
    return REDIS_PARSE_COMPLETE;
}

static oasp_array *redis_multi_array_alloc(oasp_runtime *runtime, size_t count) {
    if (count > SIZE_MAX / sizeof(oasp_value))
        return NULL;
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
    if (runtime->allocator) {
        oasp_allocator_write_barrier(runtime->allocator, array, array->bounds);
        if (array->items)
            oasp_allocator_write_barrier(runtime->allocator, array, array->items);
    }
    return array;
}

static bool redis_multi_decode_value(oasp_runtime *runtime, const char *data, size_t length, size_t *offset, size_t depth,
                                     oasp_value *result, oasp_slice *error) {
    if (depth > OASP_REDIS_MULTI_PARSE_DEPTH || *offset >= length)
        return false;
    size_t start = *offset;
    char marker = data[start];
    size_t line_end = 0;
    if (!redis_multi_find_line(data, length, start + 1, &line_end))
        return false;
    size_t line_length = line_end - start - 1;
    const char *line = data + start + 1;
    *offset = line_end + 2;
    if (marker == '+' || marker == '(') {
        char *copy = oasp_runtime_copy(runtime, line, line_length);
        if (!copy)
            return false;
        *result = oasp_value_string(copy, line_length);
        return true;
    }
    if (marker == '-' || marker == '!') {
        const char *message = line;
        size_t message_length = line_length;
        if (marker == '!') {
            long long bulk_length = 0;
            if (!redis_multi_parse_number(line, line_length, &bulk_length) || bulk_length < 0 ||
                (uint64_t)bulk_length > SIZE_MAX - *offset - 2)
                return false;
            message = data + *offset;
            message_length = (size_t)bulk_length;
            *offset += message_length + 2;
        }
        char *copy = oasp_runtime_copy(runtime, message, message_length);
        if (!copy)
            return false;
        *error = (oasp_slice){copy, message_length};
        *result = oasp_value_empty();
        return true;
    }
    if (marker == ':') {
        long long value = 0;
        if (!redis_multi_parse_number(line, line_length, &value))
            return false;
        *result = oasp_value_int(value);
        return true;
    }
    if (marker == '#') {
        *result = oasp_value_bool(line_length == 1 && line[0] == 't');
        return line_length == 1 && (line[0] == 't' || line[0] == 'f');
    }
    if (marker == '_') {
        *result = oasp_value_null();
        return line_length == 0;
    }
    if (marker == ',') {
        if (line_length >= 128)
            return false;
        char text[128];
        memcpy(text, line, line_length);
        text[line_length] = '\0';
        char *end = NULL;
        double value = strtod(text, &end);
        if (end != text + line_length)
            return false;
        *result = oasp_value_float(value);
        return true;
    }
    long long count = 0;
    if (!redis_multi_parse_number(line, line_length, &count))
        return false;
    if (marker == '$' || marker == '=') {
        if (count == -1) {
            *result = oasp_value_null();
            return true;
        }
        if (count < 0 || (uint64_t)count > SIZE_MAX - *offset - 2)
            return false;
        size_t value_length = (size_t)count;
        char *copy = oasp_runtime_copy(runtime, data + *offset, value_length);
        if (!copy)
            return false;
        *offset += value_length + 2;
        *result = oasp_value_string(copy, value_length);
        return true;
    }
    if (marker != '*' && marker != '~' && marker != '>' && marker != '%' && marker != '|')
        return false;
    if (count == -1) {
        *result = oasp_value_null();
        return true;
    }
    if (count < 0)
        return false;
    size_t elements = (size_t)count;
    if ((marker == '%' || marker == '|') && elements > SIZE_MAX / 2)
        return false;
    if (marker == '%' || marker == '|')
        elements *= 2;
    oasp_array *array = redis_multi_array_alloc(runtime, elements);
    if (!array)
        return false;
    for (size_t i = 0; i < elements; i++) {
        if (!redis_multi_decode_value(runtime, data, length, offset, depth + 1, &array->items[i], error))
            return false;
        oasp_runtime_write_barrier(runtime, array->items, &array->items[i]);
    }
    result->kind = OASP_VALUE_ARRAY;
    result->as.array = array;
    return true;
}

static bool redis_multi_append_bulk(oasp_buffer *buffer, const char *data, size_t length) {
    char header[64];
    int header_length = snprintf(header, sizeof(header), "$%zu\r\n", length);
    return header_length > 0 && (size_t)header_length < sizeof(header) &&
           oasp_buffer_append(buffer, header, (size_t)header_length) &&
           oasp_buffer_append(buffer, data, length) &&
           oasp_buffer_append(buffer, "\r\n", 2);
}

static bool redis_multi_append_setup_command(oasp_buffer *buffer, const char *name, size_t argument_count, const oasp_slice *arguments) {
    char header[64];
    int header_length = snprintf(header, sizeof(header), "*%zu\r\n", argument_count + 1);
    if (header_length <= 0 || (size_t)header_length >= sizeof(header) ||
        !oasp_buffer_append(buffer, header, (size_t)header_length) ||
        !redis_multi_append_bulk(buffer, name, strlen(name)))
        return false;
    for (size_t i = 0; i < argument_count; i++)
        if (!redis_multi_append_bulk(buffer, arguments[i].data, arguments[i].length))
            return false;
    return true;
}

static bool redis_multi_build_commands(oasp_redis_multi_client *client) {
    oasp_buffer_dispose(&client->output);
    if (!oasp_buffer_init(&client->output, 1024))
        return false;
    for (oasp_object *item = client->first; item; item = ((oasp_redis_multi_command *)item->native)->next) {
        oasp_redis_multi_command *command = item->native;
        if (!oasp_buffer_append(&client->output, command->request.data, command->request.length))
            return false;
    }
    client->output_offset = 0;
    client->reply_command = client->first;
    client->state = OASP_REDIS_MULTI_COMMAND_WRITE;
    return true;
}

static bool redis_multi_build_setup(oasp_redis_multi_client *client) {
    oasp_buffer_dispose(&client->output);
    if (!oasp_buffer_init(&client->output, 256))
        return false;
    client->setup_replies = 0;
    if (!client->reused && client->password.length) {
        oasp_slice arguments[2];
        size_t count = 0;
        if (client->username.length)
            arguments[count++] = client->username;
        arguments[count++] = client->password;
        if (!redis_multi_append_setup_command(&client->output, "AUTH", count, arguments))
            return false;
        client->setup_replies++;
    }
    if ((!client->reused && client->database) ||
        (client->reused && client->connection->redis_database != client->database)) {
        char database[32];
        int length = snprintf(database, sizeof(database), "%d", client->database);
        oasp_slice argument = {database, length > 0 ? (size_t)length : 0};
        if (length <= 0 || (size_t)length >= sizeof(database) ||
            !redis_multi_append_setup_command(&client->output, "SELECT", 1, &argument))
            return false;
        client->setup_replies++;
    }
    client->output_offset = 0;
    if (client->setup_replies)
        client->state = OASP_REDIS_MULTI_SETUP_WRITE;
    else if (!redis_multi_build_commands(client))
        return false;
    return true;
}

static bool redis_multi_reserve_setup_bytes(oasp_redis_multi_client *client) {
    if (!client->setup_replies)
        return true;
    oasp_redis_multi *multi = client->owner;
    if (client->output.length > multi->max_request_bytes - multi->wire_request_bytes)
        return false;
    multi->wire_request_bytes += client->output.length;
    return true;
}

static void redis_multi_release_client(oasp_redis_multi_client *client) {
    if (client->connection) {
        int fd = client->connection->fd;
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        if (client->state == OASP_REDIS_MULTI_DONE) {
            client->connection->protocol_clean = true;
            client->connection->redis_database = client->database;
            client->connection->reader.start = 0;
            client->connection->reader.end = 0;
        } else {
            oasp_kv_pool_entry_close(client->connection);
        }
        if (client->connection_owned) {
            oasp_kv_pool_destroy(client->connection);
            client->connection = NULL;
            client->connection_owned = false;
        } else {
            oasp_kv_pool_release(&client->connection);
        }
    }
    oasp_buffer_dispose(&client->output);
    oasp_buffer_dispose(&client->input);
    client->registered = false;
}

static bool redis_multi_endpoint_strings(oasp_redis_multi_client *client, char **endpoint, char **identity) {
    size_t endpoint_size = client->host.length + 32;
    size_t identity_size = client->host.length + client->username.length + client->password.length + 64;
    *endpoint = malloc(endpoint_size);
    *identity = malloc(identity_size);
    if (!*endpoint || !*identity) {
        free(*endpoint);
        free(*identity);
        *endpoint = NULL;
        *identity = NULL;
        return false;
    }
    int endpoint_length = snprintf(*endpoint, endpoint_size, "%.*s:%d", (int)client->host.length, client->host.data, client->port);
    int identity_length = snprintf(*identity, identity_size, "%.*s:%d:%.*s:%.*s", (int)client->host.length, client->host.data,
                                   client->port, (int)client->username.length, client->username.data,
                                   (int)client->password.length, client->password.data);
    if (endpoint_length < 0 || identity_length < 0) {
        free(*endpoint);
        free(*identity);
        *endpoint = NULL;
        *identity = NULL;
        return false;
    }
    return true;
}

static int redis_multi_connect(oasp_redis_multi_client *client) {
    char port[16];
    (void)snprintf(port, sizeof(port), "%d", client->port);
    struct addrinfo *addresses = NULL;
    if (!oasp_builtin_socket_network_address(client->host.data, port, SOCK_STREAM, false, &addresses))
        return errno ? errno : EHOSTUNREACH;
    int descriptor = -1;
    int last_error = ECONNREFUSED;
    bool connecting = false;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (descriptor < 0) {
            last_error = errno;
            continue;
        }
        int flags = fcntl(descriptor, F_GETFL, 0);
        if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) || fcntl(descriptor, F_SETFD, FD_CLOEXEC)) {
            last_error = errno;
            close(descriptor);
            descriptor = -1;
            continue;
        }
        if (!connect(descriptor, address->ai_addr, address->ai_addrlen)) {
            connecting = false;
            break;
        }
        if (errno == EINPROGRESS) {
            connecting = true;
            break;
        }
        last_error = errno;
        close(descriptor);
        descriptor = -1;
    }
    freeaddrinfo(addresses);
    if (descriptor < 0)
        return last_error;
    client->connection->fd = descriptor;
    client->connection->reader.start = 0;
    client->connection->reader.end = 0;
    client->connection->protocol_clean = true;
    client->connection->redis_database = 0;
    client->state = connecting ? OASP_REDIS_MULTI_CONNECTING : OASP_REDIS_MULTI_IDLE;
    return 0;
}

static bool redis_multi_activate(oasp_runtime *runtime, redis_multi_loop *loop, oasp_redis_multi_client *client, int64_t now) {
    client->state = OASP_REDIS_MULTI_IDLE;
    client->registered = false;
    client->reused = false;
    client->business_bytes_sent = false;
    client->setup_received = 0;
    client->replies_received = 0;
    client->response_bytes = 0;
    client->input_offset = 0;
    client->reply_command = client->first;
    client->deadline_ms = now + client->timeout_ms;
    for (oasp_object *item = client->first; item; item = ((oasp_redis_multi_command *)item->native)->next) {
        oasp_redis_multi_command *command = item->native;
        command->value = oasp_value_empty();
        command->error_message = (oasp_slice){"", 0};
        command->error_code = 0;
        command->completed = false;
        command->maybe_executed = false;
    }
    if (!oasp_buffer_init(&client->input, 8192)) {
        redis_multi_fail_client(runtime, client, 7, "unable to allocate Redis response buffer");
        return false;
    }
    char *endpoint = NULL;
    char *identity = NULL;
    if (!redis_multi_endpoint_strings(client, &endpoint, &identity)) {
        redis_multi_fail_client(runtime, client, 7, "unable to allocate Redis endpoint");
        return false;
    }
    client->connection = oasp_kv_pool_acquire(client->component_context, true, endpoint, identity);
    free(endpoint);
    free(identity);
    if (!client->connection) {
        redis_multi_fail_errno(runtime, client, errno);
        return false;
    }
    client->connection_owned = client->component_context == NULL;
    if (client->connection->fd >= 0) {
        int flags = fcntl(client->connection->fd, F_GETFL, 0);
        if (flags < 0 || fcntl(client->connection->fd, F_SETFL, flags | O_NONBLOCK)) {
            redis_multi_fail_errno(runtime, client, errno);
            return false;
        }
        client->reused = true;
    } else {
        int error = redis_multi_connect(client);
        if (error) {
            redis_multi_fail_errno(runtime, client, error);
            return false;
        }
    }
    if (client->state != OASP_REDIS_MULTI_CONNECTING) {
        if (!redis_multi_build_setup(client)) {
            redis_multi_fail_client(runtime, client, 7, "unable to build Redis setup pipeline");
            return false;
        }
        if (!redis_multi_reserve_setup_bytes(client)) {
            redis_multi_fail_client(runtime, client, EMSGSIZE, "RedisMulti request size limit exceeded");
            return false;
        }
    }
    bool writing = client->state == OASP_REDIS_MULTI_CONNECTING ||
                   client->state == OASP_REDIS_MULTI_SETUP_WRITE ||
                   client->state == OASP_REDIS_MULTI_COMMAND_WRITE;
    if (!redis_multi_loop_add(loop, client, writing)) {
        redis_multi_fail_errno(runtime, client, errno);
        return false;
    }
    return true;
}

static bool redis_multi_finish_connect(oasp_runtime *runtime, redis_multi_loop *loop, oasp_redis_multi_client *client) {
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(client->connection->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
        redis_multi_fail_errno(runtime, client, error ? error : errno);
        return false;
    }
    if (!redis_multi_build_setup(client)) {
        redis_multi_fail_client(runtime, client, 7, "unable to build Redis setup pipeline");
        return false;
    }
    if (!redis_multi_reserve_setup_bytes(client)) {
        redis_multi_fail_client(runtime, client, EMSGSIZE, "RedisMulti request size limit exceeded");
        return false;
    }
    if (!redis_multi_loop_set(loop, client, true)) {
        redis_multi_fail_errno(runtime, client, errno);
        return false;
    }
    return true;
}

static bool redis_multi_handle_write(oasp_runtime *runtime, redis_multi_loop *loop, oasp_redis_multi_client *client) {
    if (client->state == OASP_REDIS_MULTI_CONNECTING)
        return redis_multi_finish_connect(runtime, loop, client);
    while (client->output_offset < client->output.length) {
        ssize_t count = send(client->connection->fd, client->output.data + client->output_offset,
                             client->output.length - client->output_offset, MSG_NOSIGNAL);
        if (count > 0) {
            if (client->state == OASP_REDIS_MULTI_COMMAND_WRITE)
                client->business_bytes_sent = true;
            client->output_offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        redis_multi_fail_errno(runtime, client, errno);
        return false;
    }
    client->state = client->state == OASP_REDIS_MULTI_SETUP_WRITE
                        ? OASP_REDIS_MULTI_SETUP_READ
                        : OASP_REDIS_MULTI_COMMAND_READ;
    if (!redis_multi_loop_set(loop, client, false)) {
        redis_multi_fail_errno(runtime, client, errno);
        return false;
    }
    return true;
}

static void redis_multi_store_result(oasp_runtime *runtime, oasp_redis_multi_command *command, oasp_value value) {
    command->value = value;
    command->error_code = 0;
    command->error_message = (oasp_slice){"", 0};
    command->completed = true;
    command->maybe_executed = false;
    if (!runtime->allocator)
        return;
    if (value.kind == OASP_VALUE_STRING)
        oasp_allocator_write_barrier(runtime->allocator, command, value.as.string.data);
    else if (value.kind == OASP_VALUE_DATE)
        oasp_allocator_write_barrier(runtime->allocator, command, value.as.date.data);
    else if (value.kind == OASP_VALUE_ARRAY)
        oasp_allocator_write_barrier(runtime->allocator, command, value.as.array);
    else if (value.kind == OASP_VALUE_OBJECT)
        oasp_allocator_write_barrier(runtime->allocator, command, value.as.object);
}

static bool redis_multi_consume_frames(oasp_runtime *runtime, redis_multi_loop *loop, oasp_redis_multi_client *client) {
    while (client->input_offset < client->input.length) {
        size_t frame_end = 0;
        redis_parse_status status = redis_multi_scan_value(client->input.data, client->input.length, client->input_offset, 0, &frame_end);
        if (status == REDIS_PARSE_INCOMPLETE)
            return true;
        if (status == REDIS_PARSE_INVALID) {
            redis_multi_fail_client(runtime, client, 3000, "invalid Redis protocol response");
            return false;
        }
        size_t cursor = client->input_offset;
        oasp_value value = oasp_value_empty();
        oasp_slice error = {"", 0};
        if (!redis_multi_decode_value(runtime, client->input.data, frame_end, &cursor, 0, &value, &error) || cursor != frame_end) {
            redis_multi_fail_client(runtime, client, 3000, "invalid Redis protocol response");
            return false;
        }
        client->input_offset = frame_end;
        if (client->state == OASP_REDIS_MULTI_SETUP_READ) {
            if (error.length) {
                redis_multi_fail_client(runtime, client, 3000, error.data);
                return false;
            }
            client->setup_received++;
            if (client->setup_received == client->setup_replies) {
                client->connection->redis_database = client->database;
                client->input.length = 0;
                client->input_offset = 0;
                if (client->input.data)
                    client->input.data[0] = '\0';
                if (!redis_multi_build_commands(client)) {
                    redis_multi_fail_client(runtime, client, 7, "unable to build Redis command pipeline");
                    return false;
                }
                if (!redis_multi_loop_set(loop, client, true)) {
                    redis_multi_fail_errno(runtime, client, errno);
                    return false;
                }
                return true;
            }
            continue;
        }
        if (!client->reply_command) {
            redis_multi_fail_client(runtime, client, 3000, "Redis returned more responses than commands");
            return false;
        }
        oasp_redis_multi_command *command = client->reply_command->native;
        client->reply_command = command->next;
        client->replies_received++;
        if (error.length)
            redis_multi_command_error(runtime, command, 3000, error.data, error.length, false);
        else
            redis_multi_store_result(runtime, command, value);
        if (client->replies_received == client->count) {
            if (client->input_offset != client->input.length) {
                redis_multi_fail_client(runtime, client, 3000, "Redis returned more responses than commands");
                return false;
            }
            client->state = OASP_REDIS_MULTI_DONE;
            return true;
        }
    }
    if (client->input_offset == client->input.length) {
        client->input.length = 0;
        client->input_offset = 0;
        if (client->input.data)
            client->input.data[0] = '\0';
    } else if (client->input_offset) {
        size_t remaining = client->input.length - client->input_offset;
        memmove(client->input.data, client->input.data + client->input_offset, remaining);
        client->input.length = remaining;
        client->input_offset = 0;
        client->input.data[remaining] = '\0';
    }
    return true;
}

static bool redis_multi_handle_read(oasp_runtime *runtime, redis_multi_loop *loop, oasp_redis_multi_client *client) {
    char buffer[8192];
    for (;;) {
        ssize_t count = recv(client->connection->fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            size_t received = (size_t)count;
            if (client->owner->response_bytes >= client->owner->max_response_bytes ||
                received > client->owner->max_response_bytes - client->owner->response_bytes) {
                redis_multi_fail_client(runtime, client, EMSGSIZE, "RedisMulti response size limit exceeded");
                return false;
            }
            client->response_bytes += received;
            client->owner->response_bytes += received;
            if (!oasp_buffer_append(&client->input, buffer, received)) {
                redis_multi_fail_client(runtime, client, 7, "unable to grow Redis response buffer");
                return false;
            }
            if (!redis_multi_consume_frames(runtime, loop, client))
                return false;
            if (client->state == OASP_REDIS_MULTI_DONE || client->state == OASP_REDIS_MULTI_COMMAND_WRITE)
                return true;
            continue;
        }
        if (!count) {
            redis_multi_fail_client(runtime, client, ECONNRESET, "Redis connection closed before all responses arrived");
            return false;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        redis_multi_fail_errno(runtime, client, errno);
        return false;
    }
}

static bool redis_multi_client_active(const oasp_redis_multi_client *client) {
    return client->state != OASP_REDIS_MULTI_IDLE &&
           client->state != OASP_REDIS_MULTI_DONE &&
           client->state != OASP_REDIS_MULTI_FAILED;
}

static int redis_multi_wait_timeout(oasp_redis_multi *multi, int64_t global_deadline, int64_t now) {
    int64_t deadline = global_deadline;
    for (oasp_object *item = multi->first; item; item = ((oasp_redis_multi_client *)item->native)->next) {
        oasp_redis_multi_client *client = item->native;
        if (redis_multi_client_active(client) && client->deadline_ms < deadline)
            deadline = client->deadline_ms;
    }
    int64_t remaining = deadline - now;
    if (remaining <= 0)
        return 0;
    return remaining > 100 ? 100 : (int)remaining;
}

static void redis_multi_expire(oasp_runtime *runtime, oasp_redis_multi *multi, int64_t global_deadline, int64_t now) {
    bool global_expired = now >= global_deadline;
    for (oasp_object *item = multi->first; item; item = ((oasp_redis_multi_client *)item->native)->next) {
        oasp_redis_multi_client *client = item->native;
        if (redis_multi_client_active(client) && (global_expired || now >= client->deadline_ms))
            redis_multi_fail_client(runtime, client, 258, global_expired ? "RedisMulti batch timed out" : "Redis client timed out");
    }
}

static void redis_multi_fail_pending(oasp_runtime *runtime, oasp_object *pending, int code, const char *message) {
    for (oasp_object *item = pending; item; item = ((oasp_redis_multi_client *)item->native)->next)
        redis_multi_fail_client(runtime, item->native, code, message);
}

static void redis_multi_reap_clients(redis_multi_loop *loop, oasp_redis_multi *multi, size_t *active) {
    for (oasp_object *item = multi->first; item; item = ((oasp_redis_multi_client *)item->native)->next) {
        oasp_redis_multi_client *client = item->native;
        if (client->registered &&
            (client->state == OASP_REDIS_MULTI_DONE || client->state == OASP_REDIS_MULTI_FAILED)) {
            redis_multi_loop_remove(loop, client);
            redis_multi_release_client(client);
            if (*active)
                (*active)--;
        }
    }
}

static void redis_multi_reset_results(oasp_redis_multi *multi) {
    for (oasp_object *client_item = multi->first; client_item;
         client_item = ((oasp_redis_multi_client *)client_item->native)->next) {
        oasp_redis_multi_client *client = client_item->native;
        client->state = OASP_REDIS_MULTI_IDLE;
        for (oasp_object *command_item = client->first; command_item;
             command_item = ((oasp_redis_multi_command *)command_item->native)->next) {
            oasp_redis_multi_command *command = command_item->native;
            command->value = oasp_value_empty();
            command->error_message = (oasp_slice){"", 0};
            command->error_code = 0;
            command->completed = false;
            command->maybe_executed = false;
        }
    }
}

bool oasp_builtin_redis_multi_execute(oasp_runtime *runtime, oasp_object *object, oasp_redis_multi *multi, size_t line) {
    if (multi->executing) {
        oasp_runtime_raise(runtime, 5, "RedisMulti batch is already executing", object->component_name, line);
        return false;
    }
    if (multi->request_bytes > multi->max_request_bytes) {
        oasp_runtime_raise(runtime, 7, "RedisMulti request size limit exceeded", object->component_name, line);
        return false;
    }
    redis_multi_reset_results(multi);
    redis_multi_loop loop = {.descriptor = -1};
    if (!redis_multi_loop_init(&loop)) {
        oasp_runtime_raise(runtime, 7, "unable to initialize RedisMulti event loop", object->component_name, line);
        return false;
    }
    multi->executing = true;
    multi->wire_request_bytes = multi->request_bytes;
    multi->response_bytes = 0;
    int64_t now = redis_multi_now_ms();
    int64_t global_deadline = now + multi->timeout_ms;
    oasp_object *pending = multi->first;
    size_t active = 0;
    redis_multi_event events[OASP_REDIS_MULTI_EVENT_LIMIT];
    while (pending || active) {
        while (pending && active < multi->max_concurrency) {
            oasp_redis_multi_client *client = pending->native;
            pending = client->next;
            if (!client->count) {
                client->state = OASP_REDIS_MULTI_DONE;
                continue;
            }
            if (redis_multi_activate(runtime, &loop, client, now))
                active++;
            else
                redis_multi_release_client(client);
        }
        if (!active)
            continue;
        if (runtime->request && oasp_request_deadline_expired(runtime->request)) {
            redis_multi_fail_pending(runtime, pending, 258, "request execution deadline exceeded");
            pending = NULL;
            for (oasp_object *item = multi->first; item; item = ((oasp_redis_multi_client *)item->native)->next) {
                oasp_redis_multi_client *client = item->native;
                if (redis_multi_client_active(client))
                    redis_multi_fail_client(runtime, client, 258, "request execution deadline exceeded");
            }
        }
        now = redis_multi_now_ms();
        redis_multi_expire(runtime, multi, global_deadline, now);
        redis_multi_reap_clients(&loop, multi, &active);
        if (!active)
            continue;
        int timeout = redis_multi_wait_timeout(multi, global_deadline, now);
        int count = redis_multi_loop_wait(&loop, events, OASP_REDIS_MULTI_EVENT_LIMIT, timeout);
        if (count < 0 && errno != EINTR) {
            int error = errno;
            redis_multi_fail_pending(runtime, pending, error, strerror(error));
            pending = NULL;
            for (oasp_object *item = multi->first; item; item = ((oasp_redis_multi_client *)item->native)->next) {
                oasp_redis_multi_client *client = item->native;
                if (redis_multi_client_active(client))
                    redis_multi_fail_errno(runtime, client, error);
            }
        }
        for (int i = 0; i < count; i++) {
            oasp_redis_multi_client *client = events[i].client;
            if (!client || !redis_multi_client_active(client))
                continue;
            if ((events[i].flags & OASP_REDIS_EVENT_WRITE) &&
                !redis_multi_handle_write(runtime, &loop, client))
                continue;
            if ((events[i].flags & OASP_REDIS_EVENT_READ) &&
                redis_multi_client_active(client) &&
                !redis_multi_handle_read(runtime, &loop, client))
                continue;
            if ((events[i].flags & OASP_REDIS_EVENT_ERROR) &&
                redis_multi_client_active(client)) {
                int error = 0;
                socklen_t length = sizeof(error);
                if (getsockopt(client->connection->fd, SOL_SOCKET, SO_ERROR, &error, &length))
                    error = errno;
                redis_multi_fail_errno(runtime, client, error ? error : ECONNRESET);
            }
        }
        now = redis_multi_now_ms();
        redis_multi_expire(runtime, multi, global_deadline, now);
        redis_multi_reap_clients(&loop, multi, &active);
        oasp_runtime_gc_safepoint(runtime);
    }
    redis_multi_loop_dispose(&loop);
    multi->executing = false;
    return true;
}
