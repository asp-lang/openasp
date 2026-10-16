// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_MEMCACHE_MULTI_INTERNAL_H
#define OPENASP_MEMCACHE_MULTI_INTERNAL_H

#include "component_internal.h"

typedef enum {
    OASP_MEMCACHE_MULTI_IDLE,
    OASP_MEMCACHE_MULTI_CONNECTING,
    OASP_MEMCACHE_MULTI_COMMAND_WRITE,
    OASP_MEMCACHE_MULTI_COMMAND_READ,
    OASP_MEMCACHE_MULTI_DONE,
    OASP_MEMCACHE_MULTI_FAILED
} oasp_memcache_multi_state;

typedef enum {
    OASP_MEMCACHE_MULTI_GET,
    OASP_MEMCACHE_MULTI_GETS,
    OASP_MEMCACHE_MULTI_STORAGE,
    OASP_MEMCACHE_MULTI_DELETE,
    OASP_MEMCACHE_MULTI_TOUCH,
    OASP_MEMCACHE_MULTI_COUNTER,
    OASP_MEMCACHE_MULTI_VERSION,
    OASP_MEMCACHE_MULTI_FLUSH
} oasp_memcache_multi_command_kind;

typedef struct oasp_memcache_multi oasp_memcache_multi;
typedef struct oasp_memcache_multi_client oasp_memcache_multi_client;
typedef struct oasp_memcache_multi_command oasp_memcache_multi_command;

struct oasp_memcache_multi_command {
    oasp_object *next;
    oasp_memcache_multi_client *owner;
    oasp_slice request;
    oasp_slice error_message;
    oasp_value value;
    int64_t flags;
    int64_t cas;
    int error_code;
    oasp_memcache_multi_command_kind kind;
    bool completed;
    bool maybe_executed;
};

struct oasp_memcache_multi_client {
    oasp_object *next;
    oasp_object *first;
    oasp_object *last;
    oasp_memcache_multi *owner;
    oasp_component_context *component_context;
    oasp_kv_pool_entry *connection;
    oasp_slice host;
    oasp_buffer output;
    oasp_buffer input;
    oasp_object *reply_command;
    size_t count;
    size_t output_offset;
    size_t input_offset;
    size_t replies_received;
    int port;
    int64_t timeout_ms;
    int64_t deadline_ms;
    oasp_memcache_multi_state state;
    bool connection_owned;
    bool registered;
    bool business_bytes_sent;
};

struct oasp_memcache_multi {
    oasp_object *first;
    oasp_object *last;
    oasp_component_context *component_context;
    size_t client_count;
    size_t command_count;
    size_t request_bytes;
    size_t response_bytes;
    size_t max_concurrency;
    size_t max_request_bytes;
    size_t max_response_bytes;
    int64_t timeout_ms;
    bool executing;
};

bool oasp_builtin_memcache_multi_execute(oasp_runtime *runtime, oasp_object *object, oasp_memcache_multi *multi, size_t line);

#endif
