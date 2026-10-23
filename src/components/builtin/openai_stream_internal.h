// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_OPENAI_STREAM_INTERNAL_H
#define OPENASP_OPENAI_STREAM_INTERNAL_H

#include "component_internal.h"

#include <curl/curl.h>

typedef enum {
    OASP_SSE_INCOMPLETE,
    OASP_SSE_SKIPPED,
    OASP_SSE_EVENT,
    OASP_SSE_LIMIT,
    OASP_SSE_INVALID
} oasp_sse_status;

typedef struct {
    oasp_buffer input;
    oasp_buffer data;
    oasp_buffer event_type;
} oasp_sse_parser;

typedef struct oasp_openai_stream {
    oasp_runtime *runtime;
    oasp_object *owner;
    CURLM *multi;
    CURL *easy;
    struct curl_slist *headers;
    oasp_sse_parser parser;
    oasp_buffer url;
    oasp_buffer payload;
    oasp_buffer authorization;
    oasp_slice delta;
    oasp_slice data;
    oasp_slice event_type;
    oasp_slice finish_reason;
    oasp_slice usage;
    long status;
    long error_code;
    long timeout_ms;
    long idle_timeout_ms;
    size_t max_event_bytes;
    size_t max_buffered_bytes;
    size_t max_response_bytes;
    size_t response_bytes;
    size_t external_bytes;
    int64_t last_activity_ms;
    char error_message[CURL_ERROR_SIZE];
    char curl_error_message[CURL_ERROR_SIZE];
    bool transfer_done;
    bool completed;
    bool closed;
    bool added;
    bool paused;
} oasp_openai_stream;

bool oasp_openai_sse_init(oasp_sse_parser *parser);
void oasp_openai_sse_dispose(oasp_sse_parser *parser);
oasp_sse_status oasp_openai_sse_next(oasp_sse_parser *parser, size_t max_event_bytes);

#endif
