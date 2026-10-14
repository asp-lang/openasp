// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/web/fastcgi.h`): HTTP/FastCGI transport. It converts bounded wire
 * records into request-owned data, streams response records, and supervises workers. Descriptor
 * and length variables are validated at system-call boundaries to keep malformed clients
 * isolated.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_FASTCGI_H
#define OPENASP_FASTCGI_H

#include "openasp.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct oasp_external_module_config oasp_external_module_config;

typedef struct {
    /* Maximum aggregate bytes accepted across all encoded PARAMS records. */
    size_t max_params_bytes;
    /* Maximum bytes accepted across all STDIN records. */
    size_t max_body_bytes;
    /* Combined PARAMS and STDIN cap, preventing split-limit bypasses. */
    size_t max_request_bytes;
    /* Receive timeout applied to each accepted client socket. */
    int client_timeout_ms;
    /* Interpreter-side output, header, and execution limits. */
    oasp_request_limits request;
} oasp_fastcgi_limits;

/* Complete immutable server configuration borrowed for oasp_fastcgi_run(). */
typedef struct {
    /* Document root and one mutually exclusive TCP or Unix listener. */
    const char *root;
    const char *host;
    const char *unix_path;
    int port;
    /* Global accept limit and per-worker recycle/allocator relief controls. */
    long max_accepts;
    long max_requests;
    int memory_idle_ms;
    long memory_relief_requests;
    int workers;
    /* Parent-created compiled-module cache shared by forked workers. */
    size_t cache_capacity;
    const char *cache_directory;
    long validation_interval;
    bool gen_debug_info;
    /* Remote debugger is disabled unless explicitly enabled. */
    bool debug_enabled;
    const char *debug_unix_path;
    const char *debug_tcp_host;
    int debug_tcp_port;
    const char *debug_token;
    bool external_components_enabled;
    bool external_components_require_sha256;
    const oasp_external_module_config *external_modules;
    size_t external_module_count;
    oasp_fastcgi_limits limits;
} oasp_fastcgi_config;

/*
 * Runs the blocking listener and worker supervisor until a signal, accept
 * limit, or fatal setup error. Returns zero after orderly shutdown, one for an
 * operational failure, and two for invalid configuration or listener setup.
 */
int oasp_fastcgi_run(const oasp_fastcgi_config *config);

#endif
