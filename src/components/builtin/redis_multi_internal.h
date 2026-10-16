// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_REDIS_MULTI_INTERNAL_H
#define OPENASP_REDIS_MULTI_INTERNAL_H

#include "component_internal.h"

typedef enum {
    OASP_REDIS_MULTI_IDLE,
    OASP_REDIS_MULTI_CONNECTING,
    OASP_REDIS_MULTI_SETUP_WRITE,
    OASP_REDIS_MULTI_SETUP_READ,
    OASP_REDIS_MULTI_COMMAND_WRITE,
    OASP_REDIS_MULTI_COMMAND_READ,
    OASP_REDIS_MULTI_DONE,
    OASP_REDIS_MULTI_FAILED
} oasp_redis_multi_state;

typedef struct oasp_redis_multi oasp_redis_multi;
typedef struct oasp_redis_multi_client oasp_redis_multi_client;
typedef struct oasp_redis_multi_command oasp_redis_multi_command;

struct oasp_redis_multi_command {
    oasp_object *next;
    oasp_redis_multi_client *owner;
    oasp_slice request;
    oasp_slice error_message;
    oasp_value value;
    int error_code;
    bool completed;
    bool maybe_executed;
};

struct oasp_redis_multi_client {
    oasp_object *next;
    oasp_object *first;
    oasp_object *last;
    oasp_redis_multi *owner;
    oasp_component_context *component_context;
    oasp_kv_pool_entry *connection;
    oasp_slice host;
    oasp_slice username;
    oasp_slice password;
    oasp_buffer output;
    oasp_buffer input;
    oasp_object *reply_command;
    size_t count;
    size_t output_offset;
    size_t input_offset;
    size_t setup_replies;
    size_t setup_received;
    size_t replies_received;
    size_t response_bytes;
    int port;
    int database;
    int64_t timeout_ms;
    int64_t deadline_ms;
    oasp_redis_multi_state state;
    bool connection_owned;
    bool registered;
    bool reused;
    bool business_bytes_sent;
};

struct oasp_redis_multi {
    oasp_object *first;
    oasp_object *last;
    oasp_component_context *component_context;
    size_t client_count;
    size_t command_count;
    size_t request_bytes;
    size_t wire_request_bytes;
    size_t response_bytes;
    size_t max_concurrency;
    size_t max_request_bytes;
    size_t max_response_bytes;
    int64_t timeout_ms;
    bool executing;
};

bool oasp_builtin_redis_multi_execute(oasp_runtime *runtime, oasp_object *object, oasp_redis_multi *multi, size_t line);

#endif
