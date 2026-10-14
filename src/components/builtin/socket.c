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
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
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

void oasp_builtin_websocket_finalize(void *context) {
    oasp_websocket *websocket = context;
    if (websocket->tls) {
        (void)SSL_shutdown(websocket->tls);
        SSL_free(websocket->tls);
    }
    if (websocket->tls_context)
        SSL_CTX_free(websocket->tls_context);
    if (websocket->fd >= 0)
        close(websocket->fd);
    websocket->tls = NULL;
    websocket->tls_context = NULL;
    websocket->fd = -1;
    websocket->opened = false;
}

ssize_t oasp_builtin_websocket_read(oasp_websocket *websocket, void *data, size_t length) {
    if (websocket->tls)
        return SSL_read(websocket->tls, data, (int)length);
    return recv(websocket->fd, data, length, 0);
}

ssize_t oasp_builtin_websocket_write(oasp_websocket *websocket, const void *data, size_t length) {
    if (websocket->tls)
        return SSL_write(websocket->tls, data, (int)length);
    return send(websocket->fd, data, length, 0);
}

bool oasp_builtin_websocket_read_exact(oasp_websocket *websocket, void *data, size_t length) {
    unsigned char *output = data;
    while (length) {
        ssize_t count = oasp_builtin_websocket_read(websocket, output, length);
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

bool oasp_builtin_websocket_write_all(oasp_websocket *websocket, const void *data, size_t length) {
    const unsigned char *input = data;
    while (length) {
        ssize_t count = oasp_builtin_websocket_write(websocket, input, length);
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

bool oasp_builtin_websocket_frame(oasp_websocket *websocket, int opcode, const unsigned char *payload, size_t length) {
    unsigned char header[14];
    size_t header_length = 2;
    header[0] = (unsigned char)(0x80 | opcode);
    header[1] = websocket->client ? 0x80 : 0;
    if (length <= 125) {
        header[1] |= (unsigned char)length;
    } else if (length <= UINT16_MAX) {
        header[1] |= 126;
        header[2] = (unsigned char)(length >> 8);
        header[3] = (unsigned char)length;
        header_length = 4;
    } else {
        header[1] |= 127;
        uint64_t encoded = length;
        for (size_t i = 0; i < 8; i++)
            header[2 + i] = (unsigned char)(encoded >> (56 - i * 8));
        header_length = 10;
    }
    unsigned char mask[4];
    unsigned char *encoded = NULL;
    if (websocket->client) {
        if (RAND_bytes(mask, sizeof(mask)) != 1)
            return false;
        memcpy(header + header_length, mask, sizeof(mask));
        header_length += sizeof(mask);
        if (length) {
            encoded = malloc(length);
            if (!encoded)
                return false;
            for (size_t i = 0; i < length; i++)
                encoded[i] = payload[i] ^ mask[i % sizeof(mask)];
            payload = encoded;
        }
    }
    bool ok = oasp_builtin_websocket_write_all(websocket, header, header_length) && oasp_builtin_websocket_write_all(websocket, payload, length);
    free(encoded);
    return ok;
}

bool oasp_builtin_websocket_http_header(oasp_websocket *websocket, oasp_buffer *header) {
    if (!oasp_buffer_init(header, 1024))
        return false;
    while (header->length < 65536) {
        char byte;
        if (oasp_builtin_websocket_read(websocket, &byte, 1) != 1 || !oasp_buffer_append_byte(header, byte))
            return false;
        if (header->length >= 4 && !memcmp(header->data + header->length - 4, "\r\n\r\n", 4))
            return true;
    }
    return false;
}

bool oasp_builtin_websocket_accept_key(const char *key, size_t key_length, char output[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool ok = context && EVP_DigestInit_ex(context, EVP_sha1(), NULL) == 1 && EVP_DigestUpdate(context, key, key_length) == 1 &&
              EVP_DigestUpdate(context, guid, sizeof(guid) - 1) == 1 && EVP_DigestFinal_ex(context, digest, &digest_length) == 1 &&
              EVP_EncodeBlock((unsigned char *)output, digest, (int)digest_length) == 28;
    EVP_MD_CTX_free(context);
    if (ok)
        output[28] = '\0';
    return ok;
}

static const char *oasp_builtin_websocket_find_header(const oasp_buffer *header, const char *name) {
    size_t length = strlen(name);
    const char *cursor = header->data;
    const char *end = header->data + header->length;
    while (cursor + length <= end) {
        if (!strncasecmp(cursor, name, length))
            return cursor;
        const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!newline)
            break;
        cursor = newline + 1;
    }
    return NULL;
}

bool oasp_builtin_websocket_connect(oasp_runtime *runtime, oasp_websocket *websocket, const char *url, const char *subprotocol) {
    bool secure;
    const char *authority;
    if (!strncasecmp(url, "ws://", 5)) {
        secure = false;
        authority = url + 5;
    } else if (!strncasecmp(url, "wss://", 6)) {
        secure = true;
        authority = url + 6;
    } else {
        errno = EINVAL;
        return false;
    }
    const char *path = strchr(authority, '/');
    const char *authority_end = path ? path : authority + strlen(authority);
    const char *colon = memchr(authority, ':', (size_t)(authority_end - authority));
    size_t host_length = (size_t)((colon ? colon : authority_end) - authority);
    char *host = oasp_runtime_copy(runtime, authority, host_length);
    const char *port = colon ? oasp_runtime_copy(runtime, colon + 1, (size_t)(authority_end - colon - 1)) : secure ? "443" : "80";
    struct addrinfo *addresses = NULL;
    if (!host || !port || !oasp_builtin_socket_network_address(host, port, SOCK_STREAM, false, &addresses))
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
    if (fd < 0)
        return false;
    websocket->fd = fd;
    oasp_socket timeout_socket = {.timeout = websocket->timeout_ms};
    oasp_builtin_socket_timeout_apply(&timeout_socket, fd);
    if (secure) {
        websocket->tls_context = SSL_CTX_new(TLS_client_method());
        if (!websocket->tls_context)
            goto failed;
        if (websocket->tls_verify) {
            SSL_CTX_set_verify(websocket->tls_context, SSL_VERIFY_PEER, NULL);
            if (websocket->ca_file.length) {
                char *ca = oasp_runtime_copy(runtime, websocket->ca_file.data, websocket->ca_file.length);
                if (!ca || SSL_CTX_load_verify_locations(websocket->tls_context, ca, NULL) != 1)
                    goto failed;
            } else if (SSL_CTX_set_default_verify_paths(websocket->tls_context) != 1) {
                goto failed;
            }
        } else {
            SSL_CTX_set_verify(websocket->tls_context, SSL_VERIFY_NONE, NULL);
        }
        websocket->tls = SSL_new(websocket->tls_context);
        if (!websocket->tls || SSL_set_tlsext_host_name(websocket->tls, host) != 1 ||
            (websocket->tls_verify && X509_VERIFY_PARAM_set1_host(SSL_get0_param(websocket->tls), host, 0) != 1) ||
            SSL_set_fd(websocket->tls, fd) != 1 || SSL_connect(websocket->tls) != 1)
            goto failed;
    }
    unsigned char random_key[16];
    unsigned char encoded_key[25];
    char expected[29];
    if (RAND_bytes(random_key, sizeof(random_key)) != 1 || EVP_EncodeBlock(encoded_key, random_key, sizeof(random_key)) != 24 ||
        !oasp_builtin_websocket_accept_key((const char *)encoded_key, 24, expected))
        goto failed;
    oasp_buffer request = {0};
    if (!oasp_buffer_init(&request, 512))
        goto failed;
    const char *request_path = path ? path : "/";
    (void)oasp_buffer_append(&request, "GET ", 4);
    (void)oasp_buffer_append(&request, request_path, strlen(request_path));
    (void)oasp_buffer_append(&request, " HTTP/1.1\r\nHost: ", 17);
    (void)oasp_buffer_append(&request, authority, (size_t)(authority_end - authority));
    (void)oasp_buffer_append(&request,
                             "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ",
                             89);
    (void)oasp_buffer_append(&request, encoded_key, 24);
    if (subprotocol && *subprotocol) {
        (void)oasp_buffer_append(&request, "\r\nSec-WebSocket-Protocol: ", 26);
        (void)oasp_buffer_append(&request, subprotocol, strlen(subprotocol));
    }
    (void)oasp_buffer_append(&request, "\r\n\r\n", 4);
    bool request_ok = oasp_builtin_websocket_write_all(websocket, request.data, request.length);
    oasp_buffer_dispose(&request);
    oasp_buffer header = {0};
    bool header_ok = request_ok && oasp_builtin_websocket_http_header(websocket, &header) && header.length >= 12 && !memcmp(header.data, "HTTP/1.1 101", 12) &&
                     strstr(header.data, expected);
    oasp_buffer_dispose(&header);
    if (!header_ok)
        goto failed;
    websocket->opened = true;
    websocket->client = true;
    websocket->url = oasp_component_string(runtime, url);
    websocket->subprotocol = oasp_component_string(runtime, subprotocol);
    return true;

failed:
    oasp_builtin_websocket_finalize(websocket);
    return false;
}

bool oasp_builtin_websocket_accept(oasp_runtime *runtime, oasp_websocket *websocket, oasp_socket *accepted, const char *subprotocol) {
    websocket->fd = accepted->fd;
    accepted->fd = -1;
    accepted->opened = false;
    accepted->state = 0;
    websocket->client = false;
    oasp_buffer header = {0};
    if (!oasp_builtin_websocket_http_header(websocket, &header))
        goto failed;
    const char *key_header = oasp_builtin_websocket_find_header(&header, "Sec-WebSocket-Key:");
    if (!key_header)
        goto failed;
    const char *key = key_header + 18;
    while (*key == ' ' || *key == '\t')
        key++;
    const char *key_end = strstr(key, "\r\n");
    char accept_key[29];
    if (!key_end || !oasp_builtin_websocket_accept_key(key, (size_t)(key_end - key), accept_key))
        goto failed;
    oasp_buffer response = {0};
    if (!oasp_buffer_init(&response, 256))
        goto failed;
    (void)oasp_buffer_append(&response,
                             "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: ",
                             97);
    (void)oasp_buffer_append(&response, accept_key, 28);
    if (subprotocol && *subprotocol) {
        (void)oasp_buffer_append(&response, "\r\nSec-WebSocket-Protocol: ", 26);
        (void)oasp_buffer_append(&response, subprotocol, strlen(subprotocol));
    }
    (void)oasp_buffer_append(&response, "\r\n\r\n", 4);
    bool ok = oasp_builtin_websocket_write_all(websocket, response.data, response.length);
    oasp_buffer_dispose(&response);
    oasp_buffer_dispose(&header);
    if (!ok)
        goto failed_without_header;
    websocket->opened = true;
    websocket->subprotocol = oasp_component_string(runtime, subprotocol);
    return true;

failed:
    oasp_buffer_dispose(&header);
failed_without_header:
    oasp_builtin_websocket_finalize(websocket);
    return false;
}

bool oasp_builtin_websocket_receive(oasp_runtime *runtime, oasp_websocket *websocket, oasp_value *result) {
    for (;;) {
        unsigned char header[2];
        if (!oasp_builtin_websocket_read_exact(websocket, header, sizeof(header)))
            return false;
        websocket->final = (header[0] & 0x80) != 0;
        int opcode = header[0] & 15;
        uint64_t length = header[1] & 127;
        if (length == 126) {
            unsigned char extended[2];
            if (!oasp_builtin_websocket_read_exact(websocket, extended, sizeof(extended)))
                return false;
            length = ((uint64_t)extended[0] << 8) | extended[1];
        } else if (length == 127) {
            unsigned char extended[8];
            if (!oasp_builtin_websocket_read_exact(websocket, extended, sizeof(extended)))
                return false;
            length = 0;
            for (size_t i = 0; i < 8; i++)
                length = (length << 8) | extended[i];
        }
        if (length > SIZE_MAX - 1)
            return false;
        unsigned char mask[4] = {0};
        bool masked = (header[1] & 0x80) != 0;
        if (masked && !oasp_builtin_websocket_read_exact(websocket, mask, sizeof(mask)))
            return false;
        unsigned char *payload = oasp_runtime_alloc(runtime, (size_t)length + 1, _Alignof(char));
        if (!payload || !oasp_builtin_websocket_read_exact(websocket, payload, (size_t)length))
            return false;
        for (size_t i = 0; masked && i < (size_t)length; i++)
            payload[i] ^= mask[i % 4];
        payload[length] = '\0';
        if (opcode == 9) {
            if (!oasp_builtin_websocket_frame(websocket, 10, payload, (size_t)length))
                return false;
            continue;
        }
        websocket->opcode = opcode;
        websocket->message_type = opcode == 1    ? (oasp_slice){"text", 4}
                                  : opcode == 2  ? (oasp_slice){"binary", 6}
                                  : opcode == 8  ? (oasp_slice){"close", 5}
                                  : opcode == 10 ? (oasp_slice){"pong", 4}
                                                 : (oasp_slice){"continuation", 12};
        if (opcode == 8) {
            websocket->close_code = length >= 2 ? ((int)payload[0] << 8) | payload[1] : 0;
            websocket->opened = false;
        }
        *result = oasp_value_string((const char *)payload, (size_t)length);
        return true;
    }
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
    if (object->component_kind == OASP_COMPONENT_WEBSOCKET) {
        oasp_websocket *websocket = object->native;
        if (oasp_ascii_equal(member, "state"))
            *result = oasp_value_int(websocket->opened ? 1 : 0);
        else if (oasp_ascii_equal(member, "messagetype"))
            *result = oasp_value_string(websocket->message_type.data, websocket->message_type.length);
        else if (oasp_ascii_equal(member, "opcode"))
            *result = oasp_value_int(websocket->opcode);
        else if (oasp_ascii_equal(member, "final"))
            *result = oasp_value_bool(websocket->final);
        else if (oasp_ascii_equal(member, "closecode"))
            *result = oasp_value_int(websocket->close_code);
        else
            return false;
        return true;
    }
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
    if (object->component_kind == OASP_COMPONENT_WEBSOCKET) {
        oasp_websocket *websocket = object->native;
        int64_t integer = 0;
        if (oasp_ascii_equal(member, "timeout")) {
            if (oasp_value_to_int(value, &integer) && integer > 0)
                websocket->timeout_ms = integer;
            else
                oasp_runtime_raise(runtime, 5, "invalid WebSocket timeout", object->component_name, line);
            return true;
        }
        if (oasp_ascii_equal(member, "tlsverify") || oasp_ascii_equal(member, "cafile")) {
            if (oasp_ascii_equal(member, "tlsverify"))
                websocket->tls_verify = oasp_value_truthy(value);
            else
                websocket->ca_file = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
            return true;
        }
        return false;
    }
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

static bool socket_websocket_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                  size_t line, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_WEBSOCKET)
        return false;
    oasp_websocket *websocket = object->native;
    if (oasp_ascii_equal(member, "connect") && count) {
        char *url = oasp_value_format(runtime->arena, &arguments[0]);
        char *protocol = count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
        if (!url || !oasp_builtin_websocket_connect(runtime, websocket, url, protocol))
            oasp_runtime_raise(runtime, 12000 + errno, "Connect failed with WebSocket error", object->component_name, line);
    } else if (oasp_ascii_equal(member, "accept") && count && arguments[0].kind == OASP_VALUE_OBJECT &&
               arguments[0].as.object->component_kind == OASP_COMPONENT_SOCKET) {
        oasp_socket *accepted = arguments[0].as.object->native;
        char *protocol = count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
        if (!accepted->opened || accepted->state != 1 || !oasp_ascii_equal(accepted->protocol, "tcp") ||
            !oasp_builtin_websocket_accept(runtime, websocket, accepted, protocol))
            oasp_runtime_raise(runtime, 12000 + errno, "Server handshake failed with WebSocket error", object->component_name, line);
    } else {
        int opcode = oasp_ascii_equal(member, "sendtext") ? 1 : oasp_ascii_equal(member, "sendbinary") ? 2 :
                     oasp_ascii_equal(member, "ping") ? 9 : oasp_ascii_equal(member, "pong") ? 10 : 0;
        if (opcode) {
            size_t length = 0;
            const unsigned char *payload = count ? socket_value_bytes(runtime, &arguments[0], &length) : (const unsigned char *)"";
            bool ok = websocket->opened && oasp_builtin_websocket_frame(websocket, opcode, payload, length);
            if (!ok)
                oasp_runtime_raise(runtime, 12000 + errno, "WebSocket send failed", object->component_name, line);
            *result = oasp_value_bool(ok);
            return true;
        }
        if (oasp_ascii_equal(member, "receive") || oasp_ascii_equal(member, "recv")) {
            if (!websocket->opened || !oasp_builtin_websocket_receive(runtime, websocket, result)) {
                oasp_runtime_raise(runtime, 12000 + errno, "Receive failed with WebSocket error", object->component_name, line);
                *result = oasp_value_empty();
            }
            return true;
        }
        if (!oasp_ascii_equal(member, "close"))
            return false;
        if (websocket->opened)
            (void)oasp_builtin_websocket_frame(websocket, 8, (const unsigned char *)"", 0);
        oasp_builtin_websocket_finalize(websocket);
    }
    *result = oasp_value_empty();
    return true;
}

bool oasp_builtin_socket_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                              size_t line, oasp_value *result) {
    if (socket_websocket_call(runtime, object, member, arguments, count, line, result))
        return true;
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
