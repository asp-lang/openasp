// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "memcache_multi_internal.h"

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
#error "MemcacheMulti requires epoll or kqueue"
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define OASP_MEMCACHE_MULTI_EVENT_LIMIT 64

enum {
    OASP_MEMCACHE_EVENT_READ = 1,
    OASP_MEMCACHE_EVENT_WRITE = 2,
    OASP_MEMCACHE_EVENT_ERROR = 4
};

typedef struct {
    oasp_memcache_multi_client *client;
    unsigned flags;
} memcache_multi_event;

typedef struct {
    int descriptor;
} memcache_multi_loop;

typedef enum {
    MEMCACHE_PARSE_COMPLETE,
    MEMCACHE_PARSE_INCOMPLETE,
    MEMCACHE_PARSE_INVALID
} memcache_parse_status;

static int64_t memcache_multi_now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

#if defined(__linux__)
static bool memcache_multi_loop_init(memcache_multi_loop *loop) {
    loop->descriptor = epoll_create1(EPOLL_CLOEXEC);
    return loop->descriptor >= 0;
}

static bool memcache_multi_loop_add(memcache_multi_loop *loop, oasp_memcache_multi_client *client, bool writing) {
    struct epoll_event event = {0};
    event.events = (writing ? EPOLLOUT : EPOLLIN) | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = client;
    if (epoll_ctl(loop->descriptor, EPOLL_CTL_ADD, client->connection->fd, &event))
        return false;
    client->registered = true;
    return true;
}

static bool memcache_multi_loop_set(memcache_multi_loop *loop, oasp_memcache_multi_client *client, bool writing) {
    struct epoll_event event = {0};
    event.events = (writing ? EPOLLOUT : EPOLLIN) | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = client;
    return epoll_ctl(loop->descriptor, EPOLL_CTL_MOD, client->connection->fd, &event) == 0;
}

static void memcache_multi_loop_remove(memcache_multi_loop *loop, oasp_memcache_multi_client *client) {
    if (client->registered)
        (void)epoll_ctl(loop->descriptor, EPOLL_CTL_DEL, client->connection->fd, NULL);
    client->registered = false;
}

static int memcache_multi_loop_wait(memcache_multi_loop *loop, memcache_multi_event *output, size_t capacity, int timeout_ms) {
    struct epoll_event events[OASP_MEMCACHE_MULTI_EVENT_LIMIT];
    int count = epoll_wait(loop->descriptor, events, (int)capacity, timeout_ms);
    for (int i = 0; i < count; i++) {
        unsigned flags = 0;
        if (events[i].events & EPOLLIN)
            flags |= OASP_MEMCACHE_EVENT_READ;
        if (events[i].events & EPOLLOUT)
            flags |= OASP_MEMCACHE_EVENT_WRITE;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            flags |= OASP_MEMCACHE_EVENT_ERROR;
        output[i] = (memcache_multi_event){events[i].data.ptr, flags};
    }
    return count;
}
#else
static bool memcache_multi_loop_init(memcache_multi_loop *loop) {
    loop->descriptor = kqueue();
    if (loop->descriptor < 0)
        return false;
    (void)fcntl(loop->descriptor, F_SETFD, FD_CLOEXEC);
    return true;
}

static bool memcache_multi_loop_add(memcache_multi_loop *loop, oasp_memcache_multi_client *client, bool writing) {
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

static bool memcache_multi_loop_set(memcache_multi_loop *loop, oasp_memcache_multi_client *client, bool writing) {
    struct kevent changes[2];
    EV_SET(&changes[0], client->connection->fd, EVFILT_READ, writing ? EV_DISABLE : EV_ENABLE, 0, 0, client);
    EV_SET(&changes[1], client->connection->fd, EVFILT_WRITE, writing ? EV_ENABLE : EV_DISABLE, 0, 0, client);
    return kevent(loop->descriptor, changes, 2, NULL, 0, NULL) == 0;
}

static void memcache_multi_loop_remove(memcache_multi_loop *loop, oasp_memcache_multi_client *client) {
    if (client->registered) {
        struct kevent changes[2];
        EV_SET(&changes[0], client->connection->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        EV_SET(&changes[1], client->connection->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        (void)kevent(loop->descriptor, changes, 2, NULL, 0, NULL);
    }
    client->registered = false;
}

static int memcache_multi_loop_wait(memcache_multi_loop *loop, memcache_multi_event *output, size_t capacity, int timeout_ms) {
    struct kevent events[OASP_MEMCACHE_MULTI_EVENT_LIMIT];
    struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    int count = kevent(loop->descriptor, NULL, 0, events, (int)capacity, &timeout);
    for (int i = 0; i < count; i++) {
        unsigned flags = events[i].filter == EVFILT_READ ? OASP_MEMCACHE_EVENT_READ : OASP_MEMCACHE_EVENT_WRITE;
        if (events[i].flags & (EV_ERROR | EV_EOF))
            flags |= OASP_MEMCACHE_EVENT_ERROR;
        output[i] = (memcache_multi_event){events[i].udata, flags};
    }
    return count;
}
#endif

static void memcache_multi_loop_dispose(memcache_multi_loop *loop) {
    if (loop->descriptor >= 0)
        close(loop->descriptor);
    loop->descriptor = -1;
}

static void memcache_multi_command_error(oasp_runtime *runtime, oasp_memcache_multi_command *command, int code, const char *message, size_t message_length,
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

static void memcache_multi_fail_client(oasp_runtime *runtime, oasp_memcache_multi_client *client, int code, const char *message) {
    size_t length = strlen(message);
    for (oasp_object *item = client->first; item; item = ((oasp_memcache_multi_command *)item->native)->next) {
        oasp_memcache_multi_command *command = item->native;
        if (!command->completed)
            memcache_multi_command_error(runtime, command, code, message, length, client->business_bytes_sent);
    }
    if (client->connection)
        oasp_kv_pool_entry_close(client->connection);
    client->state = OASP_MEMCACHE_MULTI_FAILED;
}

static void memcache_multi_fail_errno(oasp_runtime *runtime, oasp_memcache_multi_client *client, int error) {
    if (!error)
        error = EIO;
    memcache_multi_fail_client(runtime, client, error, strerror(error));
}


static bool memcache_multi_build_commands(oasp_memcache_multi_client *client) {
    oasp_buffer_dispose(&client->output);
    if (!oasp_buffer_init(&client->output, 1024))
        return false;
    for (oasp_object *item = client->first; item; item = ((oasp_memcache_multi_command *)item->native)->next) {
        oasp_memcache_multi_command *command = item->native;
        if (!oasp_buffer_append(&client->output, command->request.data, command->request.length))
            return false;
    }
    client->output_offset = 0;
    client->reply_command = client->first;
    client->state = OASP_MEMCACHE_MULTI_COMMAND_WRITE;
    return true;
}


static void memcache_multi_release_client(oasp_memcache_multi_client *client) {
    if (client->connection) {
        int fd = client->connection->fd;
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        if (client->state == OASP_MEMCACHE_MULTI_DONE) {
            client->connection->protocol_clean = true;
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

static bool memcache_multi_endpoint_strings(oasp_memcache_multi_client *client, char **endpoint, char **identity) {
    size_t endpoint_size = client->host.length + 32;
    size_t identity_size = client->host.length + 40;
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
    int identity_length = snprintf(*identity, identity_size, "%.*s:%d:text:0", (int)client->host.length,
                                   client->host.data, client->port);
    if (endpoint_length < 0 || identity_length < 0) {
        free(*endpoint);
        free(*identity);
        *endpoint = NULL;
        *identity = NULL;
        return false;
    }
    return true;
}

static int memcache_multi_connect(oasp_memcache_multi_client *client) {
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
    client->state = connecting ? OASP_MEMCACHE_MULTI_CONNECTING : OASP_MEMCACHE_MULTI_IDLE;
    return 0;
}

static bool memcache_multi_activate(oasp_runtime *runtime, memcache_multi_loop *loop, oasp_memcache_multi_client *client, int64_t now) {
    client->state = OASP_MEMCACHE_MULTI_IDLE;
    client->registered = false;
    client->business_bytes_sent = false;
    client->replies_received = 0;
    client->input_offset = 0;
    client->reply_command = client->first;
    client->deadline_ms = now + client->timeout_ms;
    for (oasp_object *item = client->first; item; item = ((oasp_memcache_multi_command *)item->native)->next) {
        oasp_memcache_multi_command *command = item->native;
        command->value = oasp_value_empty();
        command->error_message = (oasp_slice){"", 0};
        command->error_code = 0;
        command->flags = 0;
        command->cas = 0;
        command->completed = false;
        command->maybe_executed = false;
    }
    if (!oasp_buffer_init(&client->input, 8192)) {
        memcache_multi_fail_client(runtime, client, 7, "unable to allocate Memcache response buffer");
        return false;
    }
    char *endpoint = NULL;
    char *identity = NULL;
    if (!memcache_multi_endpoint_strings(client, &endpoint, &identity)) {
        memcache_multi_fail_client(runtime, client, 7, "unable to allocate Memcache endpoint");
        return false;
    }
    client->connection = oasp_kv_pool_acquire(client->component_context, false, endpoint, identity);
    free(endpoint);
    free(identity);
    if (!client->connection) {
        memcache_multi_fail_errno(runtime, client, errno);
        return false;
    }
    client->connection_owned = client->component_context == NULL;
    if (client->connection->fd >= 0) {
        int flags = fcntl(client->connection->fd, F_GETFL, 0);
        if (flags < 0 || fcntl(client->connection->fd, F_SETFL, flags | O_NONBLOCK)) {
            memcache_multi_fail_errno(runtime, client, errno);
            return false;
        }
    } else {
        int error = memcache_multi_connect(client);
        if (error) {
            memcache_multi_fail_errno(runtime, client, error);
            return false;
        }
    }
    if (client->state != OASP_MEMCACHE_MULTI_CONNECTING) {
        if (!memcache_multi_build_commands(client)) {
            memcache_multi_fail_client(runtime, client, 7, "unable to build Memcache command pipeline");
            return false;
        }
    }
    bool writing = client->state == OASP_MEMCACHE_MULTI_CONNECTING ||
                   client->state == OASP_MEMCACHE_MULTI_COMMAND_WRITE;
    if (!memcache_multi_loop_add(loop, client, writing)) {
        memcache_multi_fail_errno(runtime, client, errno);
        return false;
    }
    return true;
}

static bool memcache_multi_finish_connect(oasp_runtime *runtime, memcache_multi_loop *loop, oasp_memcache_multi_client *client) {
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(client->connection->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
        memcache_multi_fail_errno(runtime, client, error ? error : errno);
        return false;
    }
    if (!memcache_multi_build_commands(client)) {
        memcache_multi_fail_client(runtime, client, 7, "unable to build Memcache command pipeline");
        return false;
    }
    if (!memcache_multi_loop_set(loop, client, true)) {
        memcache_multi_fail_errno(runtime, client, errno);
        return false;
    }
    return true;
}

static bool memcache_multi_handle_write(oasp_runtime *runtime, memcache_multi_loop *loop, oasp_memcache_multi_client *client) {
    if (client->state == OASP_MEMCACHE_MULTI_CONNECTING)
        return memcache_multi_finish_connect(runtime, loop, client);
    while (client->output_offset < client->output.length) {
        ssize_t count = send(client->connection->fd, client->output.data + client->output_offset,
                             client->output.length - client->output_offset, MSG_NOSIGNAL);
        if (count > 0) {
            if (client->state == OASP_MEMCACHE_MULTI_COMMAND_WRITE)
                client->business_bytes_sent = true;
            client->output_offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        memcache_multi_fail_errno(runtime, client, errno);
        return false;
    }
    client->state = OASP_MEMCACHE_MULTI_COMMAND_READ;
    if (!memcache_multi_loop_set(loop, client, false)) {
        memcache_multi_fail_errno(runtime, client, errno);
        return false;
    }
    return true;
}

static void memcache_multi_store_result(oasp_runtime *runtime, oasp_memcache_multi_command *command, oasp_value value) {
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

static bool memcache_multi_response_line(const char *data, size_t length, size_t offset, size_t *line_end) {
    for (size_t i = offset; i + 1 < length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            *line_end = i;
            return true;
        }
    }
    return false;
}

static bool memcache_multi_line_equal(const char *line, size_t length, const char *value) {
    size_t value_length = strlen(value);
    return length == value_length && !memcmp(line, value, value_length);
}

static bool memcache_multi_line_prefix(const char *line, size_t length, const char *value) {
    size_t value_length = strlen(value);
    return length >= value_length && !memcmp(line, value, value_length);
}

static memcache_parse_status memcache_multi_decode_response(oasp_runtime *runtime, oasp_memcache_multi_command *command, const char *data, size_t length, size_t offset,
                                                            size_t *frame_end, oasp_value *value, oasp_slice *error) {
    size_t line_end = 0;
    if (!memcache_multi_response_line(data, length, offset, &line_end))
        return MEMCACHE_PARSE_INCOMPLETE;
    const char *line = data + offset;
    size_t line_length = line_end - offset;
    size_t after_line = line_end + 2;
    if (memcache_multi_line_equal(line, line_length, "ERROR") ||
        memcache_multi_line_prefix(line, line_length, "CLIENT_ERROR ") ||
        memcache_multi_line_prefix(line, line_length, "SERVER_ERROR ")) {
        char *copy = oasp_runtime_copy(runtime, line, line_length);
        if (!copy)
            return MEMCACHE_PARSE_INVALID;
        *error = (oasp_slice){copy, line_length};
        *frame_end = after_line;
        return MEMCACHE_PARSE_COMPLETE;
    }
    if (command->kind == OASP_MEMCACHE_MULTI_GET || command->kind == OASP_MEMCACHE_MULTI_GETS) {
        if (memcache_multi_line_equal(line, line_length, "END")) {
            *value = oasp_value_null();
            *frame_end = after_line;
            return MEMCACHE_PARSE_COMPLETE;
        }
        long long flags = 0;
        long long cas = 0;
        size_t value_length = 0;
        int fields = sscanf(line, "VALUE %*250s %lld %zu %lld", &flags, &value_length, &cas);
        if (fields < 2 || flags < 0 || (command->kind == OASP_MEMCACHE_MULTI_GETS && fields < 3) ||
            after_line > SIZE_MAX - 7 ||
            value_length > SIZE_MAX - after_line - 7)
            return MEMCACHE_PARSE_INVALID;
        size_t value_end = after_line + value_length;
        size_t end = value_end + 7;
        if (end > length)
            return MEMCACHE_PARSE_INCOMPLETE;
        if (memcmp(data + value_end, "\r\nEND\r\n", 7))
            return MEMCACHE_PARSE_INVALID;
        char *copy = oasp_runtime_copy(runtime, data + after_line, value_length);
        if (!copy)
            return MEMCACHE_PARSE_INVALID;
        command->flags = flags;
        command->cas = fields >= 3 ? cas : 0;
        *value = oasp_value_string(copy, value_length);
        *frame_end = end;
        return MEMCACHE_PARSE_COMPLETE;
    }
    if (command->kind == OASP_MEMCACHE_MULTI_STORAGE) {
        if (!memcache_multi_line_equal(line, line_length, "STORED") &&
            !memcache_multi_line_equal(line, line_length, "NOT_STORED") &&
            !memcache_multi_line_equal(line, line_length, "EXISTS") &&
            !memcache_multi_line_equal(line, line_length, "NOT_FOUND"))
            return MEMCACHE_PARSE_INVALID;
        *value = oasp_value_bool(memcache_multi_line_equal(line, line_length, "STORED"));
    } else if (command->kind == OASP_MEMCACHE_MULTI_DELETE) {
        if (!memcache_multi_line_equal(line, line_length, "DELETED") &&
            !memcache_multi_line_equal(line, line_length, "NOT_FOUND"))
            return MEMCACHE_PARSE_INVALID;
        *value = oasp_value_bool(memcache_multi_line_equal(line, line_length, "DELETED"));
    } else if (command->kind == OASP_MEMCACHE_MULTI_TOUCH) {
        if (!memcache_multi_line_equal(line, line_length, "TOUCHED") &&
            !memcache_multi_line_equal(line, line_length, "NOT_FOUND"))
            return MEMCACHE_PARSE_INVALID;
        *value = oasp_value_bool(memcache_multi_line_equal(line, line_length, "TOUCHED"));
    } else if (command->kind == OASP_MEMCACHE_MULTI_COUNTER) {
        if (memcache_multi_line_equal(line, line_length, "NOT_FOUND")) {
            *value = oasp_value_null();
        } else {
            long long number = 0;
            if (!line_length || line_length >= 64)
                return MEMCACHE_PARSE_INVALID;
            char text[64];
            memcpy(text, line, line_length);
            text[line_length] = '\0';
            char *end = NULL;
            errno = 0;
            number = strtoll(text, &end, 10);
            if (errno || end != text + line_length || number < 0)
                return MEMCACHE_PARSE_INVALID;
            *value = oasp_value_int(number);
        }
    } else if (command->kind == OASP_MEMCACHE_MULTI_VERSION) {
        if (!memcache_multi_line_prefix(line, line_length, "VERSION ") || line_length == 8)
            return MEMCACHE_PARSE_INVALID;
        char *copy = oasp_runtime_copy(runtime, line + 8, line_length - 8);
        if (!copy)
            return MEMCACHE_PARSE_INVALID;
        *value = oasp_value_string(copy, line_length - 8);
    } else if (command->kind == OASP_MEMCACHE_MULTI_FLUSH) {
        if (!memcache_multi_line_equal(line, line_length, "OK"))
            return MEMCACHE_PARSE_INVALID;
        *value = oasp_value_bool(true);
    } else {
        return MEMCACHE_PARSE_INVALID;
    }
    *frame_end = after_line;
    return MEMCACHE_PARSE_COMPLETE;
}

static bool memcache_multi_consume_frames(oasp_runtime *runtime, oasp_memcache_multi_client *client) {
    while (client->input_offset < client->input.length) {
        if (!client->reply_command) {
            memcache_multi_fail_client(runtime, client, 13000, "Memcache returned more responses than commands");
            return false;
        }
        oasp_memcache_multi_command *command = client->reply_command->native;
        size_t frame_end = 0;
        oasp_value value = oasp_value_empty();
        oasp_slice error = {"", 0};
        memcache_parse_status status =
            memcache_multi_decode_response(runtime, command, client->input.data, client->input.length,
                                           client->input_offset, &frame_end, &value, &error);
        if (status == MEMCACHE_PARSE_INCOMPLETE)
            return true;
        if (status == MEMCACHE_PARSE_INVALID) {
            memcache_multi_fail_client(runtime, client, 13000, "invalid Memcache protocol response");
            return false;
        }
        client->input_offset = frame_end;
        client->reply_command = command->next;
        client->replies_received++;
        if (error.length)
            memcache_multi_command_error(runtime, command, 13000, error.data, error.length, false);
        else
            memcache_multi_store_result(runtime, command, value);
        if (client->replies_received == client->count) {
            if (client->input_offset != client->input.length) {
                memcache_multi_fail_client(runtime, client, 13000, "Memcache returned more responses than commands");
                return false;
            }
            client->state = OASP_MEMCACHE_MULTI_DONE;
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

static bool memcache_multi_handle_read(oasp_runtime *runtime, oasp_memcache_multi_client *client) {
    char buffer[8192];
    for (;;) {
        ssize_t count = recv(client->connection->fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            size_t received = (size_t)count;
            if (client->owner->response_bytes >= client->owner->max_response_bytes ||
                received > client->owner->max_response_bytes - client->owner->response_bytes) {
                memcache_multi_fail_client(runtime, client, EMSGSIZE, "MemcacheMulti response size limit exceeded");
                return false;
            }
            client->owner->response_bytes += received;
            if (!oasp_buffer_append(&client->input, buffer, received)) {
                memcache_multi_fail_client(runtime, client, 7, "unable to grow Memcache response buffer");
                return false;
            }
            if (!memcache_multi_consume_frames(runtime, client))
                return false;
            if (client->state == OASP_MEMCACHE_MULTI_DONE || client->state == OASP_MEMCACHE_MULTI_COMMAND_WRITE)
                return true;
            continue;
        }
        if (!count) {
            memcache_multi_fail_client(runtime, client, ECONNRESET, "Memcache connection closed before all responses arrived");
            return false;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        memcache_multi_fail_errno(runtime, client, errno);
        return false;
    }
}

static bool memcache_multi_client_active(const oasp_memcache_multi_client *client) {
    return client->state != OASP_MEMCACHE_MULTI_IDLE &&
           client->state != OASP_MEMCACHE_MULTI_DONE &&
           client->state != OASP_MEMCACHE_MULTI_FAILED;
}

static int memcache_multi_wait_timeout(oasp_memcache_multi *multi, int64_t global_deadline, int64_t now) {
    int64_t deadline = global_deadline;
    for (oasp_object *item = multi->first; item; item = ((oasp_memcache_multi_client *)item->native)->next) {
        oasp_memcache_multi_client *client = item->native;
        if (memcache_multi_client_active(client) && client->deadline_ms < deadline)
            deadline = client->deadline_ms;
    }
    int64_t remaining = deadline - now;
    if (remaining <= 0)
        return 0;
    return remaining > 100 ? 100 : (int)remaining;
}

static void memcache_multi_expire(oasp_runtime *runtime, oasp_memcache_multi *multi, int64_t global_deadline, int64_t now) {
    bool global_expired = now >= global_deadline;
    for (oasp_object *item = multi->first; item; item = ((oasp_memcache_multi_client *)item->native)->next) {
        oasp_memcache_multi_client *client = item->native;
        if (memcache_multi_client_active(client) && (global_expired || now >= client->deadline_ms))
            memcache_multi_fail_client(runtime, client, 258, global_expired ? "MemcacheMulti batch timed out" : "Memcache client timed out");
    }
}

static void memcache_multi_fail_pending(oasp_runtime *runtime, oasp_object *pending, int code, const char *message) {
    for (oasp_object *item = pending; item; item = ((oasp_memcache_multi_client *)item->native)->next)
        memcache_multi_fail_client(runtime, item->native, code, message);
}

static void memcache_multi_reap_clients(memcache_multi_loop *loop, oasp_memcache_multi *multi, size_t *active) {
    for (oasp_object *item = multi->first; item; item = ((oasp_memcache_multi_client *)item->native)->next) {
        oasp_memcache_multi_client *client = item->native;
        if (client->registered &&
            (client->state == OASP_MEMCACHE_MULTI_DONE || client->state == OASP_MEMCACHE_MULTI_FAILED)) {
            memcache_multi_loop_remove(loop, client);
            memcache_multi_release_client(client);
            if (*active)
                (*active)--;
        }
    }
}

static void memcache_multi_reset_results(oasp_memcache_multi *multi) {
    for (oasp_object *client_item = multi->first; client_item;
         client_item = ((oasp_memcache_multi_client *)client_item->native)->next) {
        oasp_memcache_multi_client *client = client_item->native;
        client->state = OASP_MEMCACHE_MULTI_IDLE;
        for (oasp_object *command_item = client->first; command_item;
             command_item = ((oasp_memcache_multi_command *)command_item->native)->next) {
            oasp_memcache_multi_command *command = command_item->native;
            command->value = oasp_value_empty();
            command->error_message = (oasp_slice){"", 0};
            command->error_code = 0;
            command->flags = 0;
            command->cas = 0;
            command->completed = false;
            command->maybe_executed = false;
        }
    }
}

bool oasp_builtin_memcache_multi_execute(oasp_runtime *runtime, oasp_object *object, oasp_memcache_multi *multi, size_t line) {
    if (multi->executing) {
        oasp_runtime_raise(runtime, 5, "MemcacheMulti batch is already executing", object->component_name, line);
        return false;
    }
    if (multi->request_bytes > multi->max_request_bytes) {
        oasp_runtime_raise(runtime, 7, "MemcacheMulti request size limit exceeded", object->component_name, line);
        return false;
    }
    memcache_multi_reset_results(multi);
    memcache_multi_loop loop = {.descriptor = -1};
    if (!memcache_multi_loop_init(&loop)) {
        oasp_runtime_raise(runtime, 7, "unable to initialize MemcacheMulti event loop", object->component_name, line);
        return false;
    }
    multi->executing = true;
    multi->response_bytes = 0;
    int64_t now = memcache_multi_now_ms();
    int64_t global_deadline = now + multi->timeout_ms;
    oasp_object *pending = multi->first;
    size_t active = 0;
    memcache_multi_event events[OASP_MEMCACHE_MULTI_EVENT_LIMIT];
    while (pending || active) {
        while (pending && active < multi->max_concurrency) {
            oasp_memcache_multi_client *client = pending->native;
            pending = client->next;
            if (!client->count) {
                client->state = OASP_MEMCACHE_MULTI_DONE;
                continue;
            }
            if (memcache_multi_activate(runtime, &loop, client, now))
                active++;
            else
                memcache_multi_release_client(client);
        }
        if (!active)
            continue;
        if (runtime->request && oasp_request_deadline_expired(runtime->request)) {
            memcache_multi_fail_pending(runtime, pending, 258, "request execution deadline exceeded");
            pending = NULL;
            for (oasp_object *item = multi->first; item; item = ((oasp_memcache_multi_client *)item->native)->next) {
                oasp_memcache_multi_client *client = item->native;
                if (memcache_multi_client_active(client))
                    memcache_multi_fail_client(runtime, client, 258, "request execution deadline exceeded");
            }
        }
        now = memcache_multi_now_ms();
        memcache_multi_expire(runtime, multi, global_deadline, now);
        memcache_multi_reap_clients(&loop, multi, &active);
        if (!active)
            continue;
        int timeout = memcache_multi_wait_timeout(multi, global_deadline, now);
        int count = memcache_multi_loop_wait(&loop, events, OASP_MEMCACHE_MULTI_EVENT_LIMIT, timeout);
        if (count < 0 && errno != EINTR) {
            int error = errno;
            memcache_multi_fail_pending(runtime, pending, error, strerror(error));
            pending = NULL;
            for (oasp_object *item = multi->first; item; item = ((oasp_memcache_multi_client *)item->native)->next) {
                oasp_memcache_multi_client *client = item->native;
                if (memcache_multi_client_active(client))
                    memcache_multi_fail_errno(runtime, client, error);
            }
        }
        for (int i = 0; i < count; i++) {
            oasp_memcache_multi_client *client = events[i].client;
            if (!client || !memcache_multi_client_active(client))
                continue;
            if ((events[i].flags & OASP_MEMCACHE_EVENT_WRITE) &&
                !memcache_multi_handle_write(runtime, &loop, client))
                continue;
            if ((events[i].flags & OASP_MEMCACHE_EVENT_READ) &&
                memcache_multi_client_active(client) &&
                !memcache_multi_handle_read(runtime, client))
                continue;
            if ((events[i].flags & OASP_MEMCACHE_EVENT_ERROR) &&
                memcache_multi_client_active(client)) {
                int error = 0;
                socklen_t length = sizeof(error);
                if (getsockopt(client->connection->fd, SOL_SOCKET, SO_ERROR, &error, &length))
                    error = errno;
                memcache_multi_fail_errno(runtime, client, error ? error : ECONNRESET);
            }
        }
        now = memcache_multi_now_ms();
        memcache_multi_expire(runtime, multi, global_deadline, now);
        memcache_multi_reap_clients(&loop, multi, &active);
        oasp_runtime_gc_safepoint(runtime);
    }
    memcache_multi_loop_dispose(&loop);
    multi->executing = false;
    return true;
}
