// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "builtin_internal.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

void oasp_builtin_socket_finalize(void *context) {
    oasp_socket *socket = context;
    if (socket->fd >= 0)
        close(socket->fd);
    if (socket->state == 3 && oasp_ascii_equal(socket->protocol, "unix") && socket->host.data)
        (void)unlink(socket->host.data);
    socket->fd = -1;
    socket->opened = false;
    socket->state = 0;
}

oasp_slice oasp_builtin_socket_protocol(oasp_runtime *runtime, const char *value) {
    while (*value && isspace((unsigned char)*value))
        value++;
    size_t length = strlen(value);
    while (length && isspace((unsigned char)value[length - 1]))
        length--;
    if (!length || oasp_ascii_equal_n(value, length, "tcp") || oasp_ascii_equal_n(value, length, "stream"))
        return (oasp_slice){"tcp", 3};
    if (oasp_ascii_equal_n(value, length, "udp") || oasp_ascii_equal_n(value, length, "datagram"))
        return (oasp_slice){"udp", 3};
    if (oasp_ascii_equal_n(value, length, "unix") || oasp_ascii_equal_n(value, length, "local") || oasp_ascii_equal_n(value, length, "unixstream"))
        return (oasp_slice){"unix", 4};
    char *copy = oasp_runtime_copy(runtime, value, length);
    return (oasp_slice){copy, length};
}

void oasp_builtin_socket_timeout_apply(const oasp_socket *socket, int fd) {
    if (socket->timeout <= 0)
        return;
    struct timeval timeout = {(time_t)(socket->timeout / 1000), (suseconds_t)((socket->timeout % 1000) * 1000)};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

bool oasp_builtin_socket_attach(oasp_socket *socket, int fd, int state) {
    if (socket->fd >= 0)
        close(socket->fd);
    socket->fd = fd;
    socket->opened = true;
    socket->state = state;
    oasp_builtin_socket_timeout_apply(socket, fd);
    return true;
}

bool oasp_builtin_socket_network_address(const char *host, const char *port, int type, bool passive, struct addrinfo **addresses) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    return getaddrinfo(host && *host ? host : NULL, port, &hints, addresses) == 0;
}

bool oasp_builtin_socket_connect_native(oasp_socket *endpoint) {
    if (oasp_ascii_equal(endpoint->protocol, "unix")) {
        if (!endpoint->host.data || !endpoint->host.length)
            return false;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        if (endpoint->host.length >= sizeof(address.sun_path)) {
            close(fd);
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(address.sun_path, endpoint->host.data, endpoint->host.length);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
            close(fd);
            return false;
        }
        return oasp_builtin_socket_attach(endpoint, fd, 1);
    }
    int type = oasp_ascii_equal(endpoint->protocol, "udp") ? SOCK_DGRAM : SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    if (!oasp_builtin_socket_network_address(endpoint->host.data, endpoint->port.data, type, false, &addresses))
        return false;
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
    return fd >= 0 && oasp_builtin_socket_attach(endpoint, fd, 1);
}

bool oasp_builtin_socket_listen_native(oasp_runtime *runtime, oasp_socket *endpoint, int backlog) {
    if (oasp_ascii_equal(endpoint->protocol, "unix")) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        if (!endpoint->host.data || endpoint->host.length >= sizeof(address.sun_path)) {
            close(fd);
            return false;
        }
        memcpy(address.sun_path, endpoint->host.data, endpoint->host.length);
        (void)unlink(address.sun_path);
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, backlog)) {
            close(fd);
            return false;
        }
        return oasp_builtin_socket_attach(endpoint, fd, 3);
    }
    struct addrinfo *addresses = NULL;
    if (!oasp_builtin_socket_network_address(endpoint->host.data, endpoint->port.data, SOCK_STREAM, true, &addresses))
        return false;
    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        int enabled = 1;
        if (fd >= 0)
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (fd >= 0 && !bind(fd, address->ai_addr, address->ai_addrlen) && !listen(fd, backlog))
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        return false;
    struct sockaddr_storage address;
    socklen_t address_length = sizeof(address);
    if (!getsockname(fd, (struct sockaddr *)&address, &address_length)) {
        int port = address.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&address)->sin_port) : ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
        char text[16];
        int length = snprintf(text, sizeof(text), "%d", port);
        endpoint->port.data = oasp_runtime_copy(runtime, text, (size_t)length);
        endpoint->port.length = (size_t)length;
    }
    return oasp_builtin_socket_attach(endpoint, fd, 3);
}

bool oasp_builtin_socket_bind_udp(oasp_runtime *runtime, oasp_socket *endpoint) {
    struct addrinfo *addresses = NULL;
    if (!oasp_builtin_socket_network_address(endpoint->host.data, endpoint->port.data, SOCK_DGRAM, true, &addresses))
        return false;
    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd >= 0 && !bind(fd, address->ai_addr, address->ai_addrlen))
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        return false;
    struct sockaddr_storage address;
    socklen_t address_length = sizeof(address);
    if (!getsockname(fd, (struct sockaddr *)&address, &address_length)) {
        int port = address.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&address)->sin_port) : ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
        char text[16];
        int length = snprintf(text, sizeof(text), "%d", port);
        endpoint->port.data = oasp_runtime_copy(runtime, text, (size_t)length);
        endpoint->port.length = (size_t)length;
    }
    return oasp_builtin_socket_attach(endpoint, fd, 2);
}

void oasp_builtin_socket_raise(oasp_runtime *runtime, oasp_object *object, const char *operation, size_t line) {
    char message[160];
    (void)snprintf(message, sizeof(message), "%s failed with socket error %d", operation, errno);
    oasp_runtime_raise(runtime, 10000 + errno, message, object->component_name, line);
}

static const unsigned char *socket_value_bytes(oasp_runtime *runtime, const oasp_value *value, size_t *length) {
    if (value->kind == OASP_VALUE_STRING) {
        *length = value->as.string.length;
        return (const unsigned char *)value->as.string.data;
    }
    char *text = oasp_value_format(runtime->arena, value);
    *length = text ? strlen(text) : 0;
    return (const unsigned char *)text;
}

bool oasp_builtin_socket_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *result) {
    (void)runtime;
    if (object->component_kind != OASP_COMPONENT_SOCKET)
        return false;
    oasp_socket *socket = object->native;
    if (oasp_ascii_equal(member, "protocol") || oasp_ascii_equal(member, "sockettype") || oasp_ascii_equal(member, "type"))
        *result = oasp_value_string(socket->protocol.data, socket->protocol.length);
    else if (oasp_ascii_equal(member, "host") || oasp_ascii_equal(member, "address") || oasp_ascii_equal(member, "path"))
        *result = oasp_value_string(socket->host.data, socket->host.length);
    else if (oasp_ascii_equal(member, "port") || oasp_ascii_equal(member, "localport"))
        *result = oasp_value_string(socket->port.data, socket->port.length);
    else if (oasp_ascii_equal(member, "remoteport"))
        *result = oasp_value_int(socket->peer_port);
    else if (oasp_ascii_equal(member, "timeout"))
        *result = oasp_value_int(socket->timeout);
    else if (oasp_ascii_equal(member, "state"))
        *result = oasp_value_int(socket->opened ? socket->state : 0);
    else if (oasp_ascii_equal(member, "localaddress"))
        *result = socket->state == 1 ? oasp_value_string("", 0) : oasp_value_string(socket->host.data, socket->host.length);
    else if (oasp_ascii_equal(member, "remoteaddress") || oasp_ascii_equal(member, "lastpeeraddress"))
        *result = oasp_value_string(socket->peer_host.length ? socket->peer_host.data : socket->host.data,
                                    socket->peer_host.length ? socket->peer_host.length : socket->host.length);
    else
        return false;
    return true;
}

bool oasp_builtin_socket_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind != OASP_COMPONENT_SOCKET)
        return false;
    oasp_socket *socket = object->native;
    if (oasp_ascii_equal(member, "timeout")) {
        (void)oasp_value_to_int(value, &socket->timeout);
        if (socket->fd >= 0)
            oasp_builtin_socket_timeout_apply(socket, socket->fd);
        return true;
    }
    char *text = oasp_value_format(runtime->arena, value);
    if (oasp_ascii_equal(member, "protocol") || oasp_ascii_equal(member, "sockettype") || oasp_ascii_equal(member, "type")) {
        if (socket->opened)
            oasp_runtime_raise(runtime, 5, "cannot change protocol while socket is open", object->component_name, line);
        else {
            oasp_slice protocol = oasp_builtin_socket_protocol(runtime, text);
            if (protocol.data && (oasp_ascii_equal(protocol, "tcp") || oasp_ascii_equal(protocol, "udp") || oasp_ascii_equal(protocol, "unix")))
                socket->protocol = protocol;
            else
                oasp_runtime_raise(runtime, 5, "unsupported socket protocol", object->component_name, line);
        }
        return true;
    }
    if (oasp_ascii_equal(member, "host") || oasp_ascii_equal(member, "address") || oasp_ascii_equal(member, "path")) {
        socket->host = oasp_component_string(runtime, text);
        return true;
    }
    if (oasp_ascii_equal(member, "port")) {
        socket->port = oasp_component_string(runtime, text);
        return true;
    }
    return false;
}

bool oasp_builtin_socket_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                              size_t line, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_SOCKET)
        return false;
    oasp_socket *socket = object->native;
    if (oasp_ascii_equal(member, "open") || oasp_ascii_equal(member, "connect")) {
        size_t offset = 0;
        if (oasp_ascii_equal(member, "open") && count) {
            char *protocol_name = oasp_value_format(runtime->arena, &arguments[0]);
            oasp_slice protocol = oasp_builtin_socket_protocol(runtime, protocol_name ? protocol_name : "");
            if (!oasp_ascii_equal(protocol, "tcp") && !oasp_ascii_equal(protocol, "udp") && !oasp_ascii_equal(protocol, "unix")) {
                oasp_runtime_raise(runtime, 5, "unsupported socket protocol", object->component_name, line);
                *result = oasp_value_empty();
                return true;
            }
            socket->protocol = protocol;
            offset = 1;
        }
        if (count > offset)
            socket->host = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[offset]));
        if (!oasp_ascii_equal(socket->protocol, "unix") && count > offset + 1)
            socket->port = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[offset + 1]));
        if (count > offset + (oasp_ascii_equal(socket->protocol, "unix") ? 1 : 2))
            (void)oasp_value_to_int(&arguments[offset + (oasp_ascii_equal(socket->protocol, "unix") ? 1 : 2)], &socket->timeout);
        if (!socket->host.length || (!oasp_ascii_equal(socket->protocol, "unix") && !socket->port.length))
            oasp_runtime_raise(runtime, 5, "host and port are required", object->component_name, line);
        else if (!oasp_builtin_socket_connect_native(socket))
            oasp_builtin_socket_raise(runtime, object, "Connect", line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "bind") && count >= 2) {
        socket->protocol = (oasp_slice){"udp", 3};
        socket->host = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[0]));
        socket->port = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[1]));
        if (!oasp_builtin_socket_bind_udp(runtime, socket))
            oasp_builtin_socket_raise(runtime, object, "Bind", line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "listen") && count) {
        socket->host = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[0]));
        int64_t backlog = 128;
        if (oasp_ascii_equal(socket->protocol, "unix")) {
            if (count > 1) (void)oasp_value_to_int(&arguments[1], &backlog);
        } else {
            if (count > 1) socket->port = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[1]));
            if (count > 2) (void)oasp_value_to_int(&arguments[2], &backlog);
        }
        if (!oasp_builtin_socket_listen_native(runtime, socket, backlog > 0 ? (int)backlog : 128))
            oasp_builtin_socket_raise(runtime, object, "Listen", line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "accept")) {
        if (!socket->opened || socket->state != 3) {
            oasp_runtime_raise(runtime, 5, "Accept requires a listening socket", object->component_name, line);
            *result = oasp_value_nothing();
            return true;
        }
        int64_t timeout = socket->timeout;
        if (count) (void)oasp_value_to_int(&arguments[0], &timeout);
        struct pollfd descriptor = {socket->fd, POLLIN, 0};
        if (poll(&descriptor, 1, (int)timeout) <= 0) {
            errno = ETIMEDOUT;
            oasp_builtin_socket_raise(runtime, object, "Accept", line);
            *result = oasp_value_nothing();
            return true;
        }
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        int fd = accept(socket->fd, (struct sockaddr *)&peer, &peer_length);
        if (fd < 0) {
            oasp_builtin_socket_raise(runtime, object, "Accept", line);
            *result = oasp_value_nothing();
            return true;
        }
        oasp_socket *child = oasp_runtime_alloc(runtime, sizeof(*child), _Alignof(oasp_socket));
        if (!child) { close(fd); *result = oasp_value_empty(); return true; }
        memset(child, 0, sizeof(*child));
        child->fd = -1; child->protocol = socket->protocol; child->host = socket->host;
        child->port = socket->port; child->timeout = socket->timeout;
        (void)oasp_builtin_socket_attach(child, fd, 1);
        char host[NI_MAXHOST], service[NI_MAXSERV];
        if (!getnameinfo((struct sockaddr *)&peer, peer_length, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV)) {
            child->peer_host = oasp_component_string(runtime, host);
            child->peer_port = atoi(service);
        }
        if (!oasp_runtime_defer(runtime, child, oasp_builtin_socket_finalize, child)) {
            oasp_builtin_socket_finalize(child); *result = oasp_value_empty(); return true;
        }
        oasp_object *child_object = oasp_component_object(runtime, OASP_COMPONENT_SOCKET, "OpenASP.Socket", child);
        *result = child_object ? oasp_component_object_value(child_object) : oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "write") || oasp_ascii_equal(member, "send")) {
        if (!socket->opened || socket->fd < 0) {
            oasp_runtime_raise(runtime, 91, "Send requires an open socket", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        size_t length = 0, sent = 0;
        const unsigned char *bytes = count ? socket_value_bytes(runtime, &arguments[0], &length) : NULL;
        while (sent < length) {
            ssize_t written = send(socket->fd, bytes + sent, length - sent, 0);
            if (written > 0) sent += (size_t)written;
            else if (written < 0 && errno == EINTR) continue;
            else { oasp_builtin_socket_raise(runtime, object, "Send", line); break; }
        }
        *result = oasp_value_int((int64_t)sent);
        return true;
    }
    if (oasp_ascii_equal(member, "read") || oasp_ascii_equal(member, "receive") || oasp_ascii_equal(member, "recv")) {
        int64_t maximum = 65536;
        if (count) (void)oasp_value_to_int(&arguments[0], &maximum);
        if (maximum <= 0) maximum = 65536;
        if (!socket->opened || socket->fd < 0) {
            oasp_runtime_raise(runtime, 91, "Receive requires an open socket", object->component_name, line);
            *result = oasp_value_string("", 0);
            return true;
        }
        char *data = oasp_runtime_alloc(runtime, (size_t)maximum + 1, _Alignof(char));
        ssize_t received = data ? recv(socket->fd, data, (size_t)maximum, 0) : -1;
        if (received < 0) {
            oasp_builtin_socket_raise(runtime, object, "Receive", line);
            *result = oasp_value_string("", 0);
        } else {
            data[received] = '\0';
            *result = oasp_value_string(data, (size_t)received);
        }
        return true;
    }
    if (oasp_ascii_equal(member, "sendto") && count >= 3) {
        char *host = oasp_value_format(runtime->arena, &arguments[0]);
        char *port = oasp_value_format(runtime->arena, &arguments[1]);
        size_t length = 0;
        const unsigned char *bytes = socket_value_bytes(runtime, &arguments[2], &length);
        struct addrinfo *addresses = NULL;
        ssize_t sent = -1;
        if (socket->opened && socket->fd >= 0 &&
            oasp_builtin_socket_network_address(host, port, SOCK_DGRAM, false, &addresses))
            sent = sendto(socket->fd, bytes, length, 0, addresses->ai_addr, addresses->ai_addrlen);
        if (addresses)
            freeaddrinfo(addresses);
        if (sent < 0)
            oasp_builtin_socket_raise(runtime, object, "SendTo", line);
        *result = oasp_value_int(sent < 0 ? 0 : sent);
        return true;
    }
    if (oasp_ascii_equal(member, "receivefrom") || oasp_ascii_equal(member, "recvfrom")) {
        int64_t maximum = 65536;
        if (count)
            (void)oasp_value_to_int(&arguments[0], &maximum);
        if (maximum <= 0)
            maximum = 65536;
        char *data = oasp_runtime_alloc(runtime, (size_t)maximum + 1, _Alignof(char));
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        ssize_t received = socket->opened && data ? recvfrom(socket->fd, data, (size_t)maximum, 0,
                                                             (struct sockaddr *)&peer, &peer_length) : -1;
        if (received < 0) {
            oasp_builtin_socket_raise(runtime, object, "ReceiveFrom", line);
            *result = oasp_value_empty();
            return true;
        }
        char host[NI_MAXHOST] = "";
        char service[NI_MAXSERV] = "0";
        (void)getnameinfo((struct sockaddr *)&peer, peer_length, host, sizeof(host), service, sizeof(service),
                          NI_NUMERICHOST | NI_NUMERICSERV);
        socket->peer_host = oasp_component_string(runtime, host);
        socket->peer_port = atoi(service);
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array) {
            *result = oasp_value_empty();
            return true;
        }
        memset(array, 0, sizeof(*array));
        array->rank = 1;
        array->length = 3;
        array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
        array->items = oasp_runtime_alloc_values(runtime, 3);
        if (!array->bounds || !array->items) {
            *result = oasp_value_empty();
            return true;
        }
        array->bounds[0] = 2;
        array->items[0] = oasp_value_string(data, (size_t)received);
        array->items[1] = oasp_value_string(socket->peer_host.data, socket->peer_host.length);
        array->items[2] = oasp_value_int(socket->peer_port);
        *result = (oasp_value){.kind = OASP_VALUE_ARRAY, .as.array = array};
        return true;
    }
    if (oasp_ascii_equal(member, "close")) {
        oasp_builtin_socket_finalize(socket);
        *result = oasp_value_empty();
        return true;
    }
    return false;
}
