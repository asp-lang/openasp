// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "builtin_internal.h"

#include <errno.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

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

static ssize_t websocket_read(oasp_websocket *websocket, void *data, size_t length) {
    if (websocket->tls)
        return SSL_read(websocket->tls, data, (int)length);
    return recv(websocket->fd, data, length, 0);
}

static ssize_t websocket_write(oasp_websocket *websocket, const void *data, size_t length) {
    if (websocket->tls)
        return SSL_write(websocket->tls, data, (int)length);
    return send(websocket->fd, data, length, 0);
}

static bool websocket_read_exact(oasp_websocket *websocket, void *data, size_t length) {
    unsigned char *output = data;
    while (length) {
        ssize_t count = websocket_read(websocket, output, length);
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

static bool websocket_write_all(oasp_websocket *websocket, const void *data, size_t length) {
    const unsigned char *input = data;
    while (length) {
        ssize_t count = websocket_write(websocket, input, length);
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

static bool websocket_frame(oasp_websocket *websocket, int opcode, const unsigned char *payload, size_t length) {
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
    bool ok = websocket_write_all(websocket, header, header_length) && websocket_write_all(websocket, payload, length);
    free(encoded);
    return ok;
}

static bool websocket_http_header(oasp_websocket *websocket, oasp_buffer *header) {
    if (!oasp_buffer_init(header, 1024))
        return false;
    while (header->length < 65536) {
        char byte;
        if (websocket_read(websocket, &byte, 1) != 1 || !oasp_buffer_append_byte(header, byte))
            return false;
        if (header->length >= 4 && !memcmp(header->data + header->length - 4, "\r\n\r\n", 4))
            return true;
    }
    return false;
}

static bool websocket_accept_key(const char *key, size_t key_length, char output[29]) {
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

static const char *websocket_find_header(const oasp_buffer *header, const char *name) {
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

static bool websocket_connect(oasp_runtime *runtime, oasp_websocket *websocket, const char *url, const char *subprotocol) {
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
        !websocket_accept_key((const char *)encoded_key, 24, expected))
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
    bool request_ok = websocket_write_all(websocket, request.data, request.length);
    oasp_buffer_dispose(&request);
    oasp_buffer header = {0};
    bool header_ok = request_ok && websocket_http_header(websocket, &header) && header.length >= 12 && !memcmp(header.data, "HTTP/1.1 101", 12) &&
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

static bool websocket_accept(oasp_runtime *runtime, oasp_websocket *websocket, oasp_socket *accepted, const char *subprotocol) {
    websocket->fd = accepted->fd;
    accepted->fd = -1;
    accepted->opened = false;
    accepted->state = 0;
    websocket->client = false;
    oasp_buffer header = {0};
    if (!websocket_http_header(websocket, &header))
        goto failed;
    const char *key_header = websocket_find_header(&header, "Sec-WebSocket-Key:");
    if (!key_header)
        goto failed;
    const char *key = key_header + 18;
    while (*key == ' ' || *key == '\t')
        key++;
    const char *key_end = strstr(key, "\r\n");
    char accept_key[29];
    if (!key_end || !websocket_accept_key(key, (size_t)(key_end - key), accept_key))
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
    bool ok = websocket_write_all(websocket, response.data, response.length);
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

static bool websocket_receive(oasp_runtime *runtime, oasp_websocket *websocket, oasp_value *result) {
    for (;;) {
        unsigned char header[2];
        if (!websocket_read_exact(websocket, header, sizeof(header)))
            return false;
        websocket->final = (header[0] & 0x80) != 0;
        int opcode = header[0] & 15;
        uint64_t length = header[1] & 127;
        if (length == 126) {
            unsigned char extended[2];
            if (!websocket_read_exact(websocket, extended, sizeof(extended)))
                return false;
            length = ((uint64_t)extended[0] << 8) | extended[1];
        } else if (length == 127) {
            unsigned char extended[8];
            if (!websocket_read_exact(websocket, extended, sizeof(extended)))
                return false;
            length = 0;
            for (size_t i = 0; i < 8; i++)
                length = (length << 8) | extended[i];
        }
        if (length > SIZE_MAX - 1)
            return false;
        unsigned char mask[4] = {0};
        bool masked = (header[1] & 0x80) != 0;
        if (masked && !websocket_read_exact(websocket, mask, sizeof(mask)))
            return false;
        unsigned char *payload = oasp_runtime_alloc(runtime, (size_t)length + 1, _Alignof(char));
        if (!payload || !websocket_read_exact(websocket, payload, (size_t)length))
            return false;
        for (size_t i = 0; masked && i < (size_t)length; i++)
            payload[i] ^= mask[i % 4];
        payload[length] = '\0';
        if (opcode == 9) {
            if (!websocket_frame(websocket, 10, payload, (size_t)length))
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

static const unsigned char *websocket_value_bytes(oasp_runtime *runtime, const oasp_value *value, size_t *length) {
    if (value->kind == OASP_VALUE_STRING) {
        *length = value->as.string.length;
        return (const unsigned char *)value->as.string.data;
    }
    char *text = oasp_value_format(runtime->arena, value);
    *length = text ? strlen(text) : 0;
    return (const unsigned char *)text;
}

bool oasp_builtin_websocket_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_WEBSOCKET)
        return false;
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

bool oasp_builtin_websocket_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind != OASP_COMPONENT_WEBSOCKET)
        return false;
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

bool oasp_builtin_websocket_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                                 size_t line, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_WEBSOCKET)
        return false;
    oasp_websocket *websocket = object->native;
    if (oasp_ascii_equal(member, "connect") && count) {
        char *url = oasp_value_format(runtime->arena, &arguments[0]);
        char *protocol = count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
        if (!url || !websocket_connect(runtime, websocket, url, protocol))
            oasp_runtime_raise(runtime, 12000 + errno, "Connect failed with WebSocket error", object->component_name, line);
    } else if (oasp_ascii_equal(member, "accept") && count && arguments[0].kind == OASP_VALUE_OBJECT &&
               arguments[0].as.object->component_kind == OASP_COMPONENT_SOCKET) {
        oasp_socket *accepted = arguments[0].as.object->native;
        char *protocol = count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
        if (!accepted->opened || accepted->state != 1 || !oasp_ascii_equal(accepted->protocol, "tcp") ||
            !websocket_accept(runtime, websocket, accepted, protocol))
            oasp_runtime_raise(runtime, 12000 + errno, "Server handshake failed with WebSocket error", object->component_name, line);
    } else {
        int opcode = oasp_ascii_equal(member, "sendtext") ? 1 : oasp_ascii_equal(member, "sendbinary") ? 2 :
                     oasp_ascii_equal(member, "ping") ? 9 : oasp_ascii_equal(member, "pong") ? 10 : 0;
        if (opcode) {
            size_t length = 0;
            const unsigned char *payload = count ? websocket_value_bytes(runtime, &arguments[0], &length) : (const unsigned char *)"";
            bool ok = websocket->opened && websocket_frame(websocket, opcode, payload, length);
            if (!ok)
                oasp_runtime_raise(runtime, 12000 + errno, "WebSocket send failed", object->component_name, line);
            *result = oasp_value_bool(ok);
            return true;
        }
        if (oasp_ascii_equal(member, "receive") || oasp_ascii_equal(member, "recv")) {
            if (!websocket->opened || !websocket_receive(runtime, websocket, result)) {
                oasp_runtime_raise(runtime, 12000 + errno, "Receive failed with WebSocket error", object->component_name, line);
                *result = oasp_value_empty();
            }
            return true;
        }
        if (!oasp_ascii_equal(member, "close"))
            return false;
        if (websocket->opened)
            (void)websocket_frame(websocket, 8, (const unsigned char *)"", 0);
        oasp_builtin_websocket_finalize(websocket);
    }
    *result = oasp_value_empty();
    return true;
}
