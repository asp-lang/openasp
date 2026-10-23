// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXT_MANIFEST_H
#define OPENASP_EXT_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OASP_EXTERNAL_MODE_UNSET,
    OASP_EXTERNAL_MODE_IN_PROCESS,
    OASP_EXTERNAL_MODE_ISOLATED
} oasp_external_mode;

enum {
    OASP_EXTERNAL_CAP_CRYPTO = UINT64_C(1) << 0,
    OASP_EXTERNAL_CAP_FILESYSTEM = UINT64_C(1) << 1,
    OASP_EXTERNAL_CAP_NETWORK = UINT64_C(1) << 2,
    OASP_EXTERNAL_CAP_DATABASE = UINT64_C(1) << 3,
    OASP_EXTERNAL_CAP_PROCESS = UINT64_C(1) << 4,
    OASP_EXTERNAL_CAP_THREADS = UINT64_C(1) << 5
};

/*
 * Owned configuration parsed from the single OpenASP YAML file. The loader
 * borrows this data for its lifetime; callers release it through the owning
 * FPM/CLI options structure after the engine is shut down.
 */
typedef struct oasp_external_module_config {
    char *id;
    char *library;
    char *sha256;
    char **prog_ids;
    size_t prog_id_count;
    uint64_t capabilities;
    oasp_external_mode mode;
    size_t max_instances;
    size_t max_message_bytes;
    size_t max_external_bytes;
    size_t max_worker_external_bytes;
    size_t max_model_external_bytes;
    size_t max_device_external_bytes;
    unsigned long call_timeout_ms;
} oasp_external_module_config;

#endif
