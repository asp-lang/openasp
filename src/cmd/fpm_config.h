// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/cmd/fpm_config.h`): Command and configuration layer. It translates
 * operator input into validated engine/server state, owns copied option strings, and reports
 * startup failures with actionable diagnostics before transferring control to the runtime.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_FPM_CONFIG_H
#define OPENASP_FPM_CONFIG_H

#include "fastcgi.h"
#include "log.h"
#include "openasp/ext_manifest.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Owns all strings loaded from defaults, YAML, or command-line overrides.
 * `server` contains borrowed pointers into the owned fields and is therefore
 * valid only until oasp_fpm_options_dispose() is called.
 */
typedef struct {
    oasp_fastcgi_config server;
    bool daemon;
    oasp_log_level log_level;
    char *root;
    char *host;
    char *unix_path;
    char *cache_directory;
    char *debug_unix_path;
    char *debug_tcp_host;
    char *debug_token;
    char *log_file;
    char *shell_enable;
    char *process_enable;
    bool external_components_enabled;
    bool external_components_require_sha256;
    oasp_external_module_config *external_modules;
    size_t external_module_count;
} oasp_fpm_options;

/* Initializes a complete configuration with production-safe built-in defaults. */
bool oasp_fpm_options_init(oasp_fpm_options *options, char *error, size_t error_size);

/* Releases every string owned by `options` and clears borrowed server pointers. */
void oasp_fpm_options_dispose(oasp_fpm_options *options);

/* Validates and applies one canonical option name without its leading `--`. */
bool oasp_fpm_options_set(oasp_fpm_options *options, const char *key, const char *value, char *error, size_t error_size);

/* Loads the supported top-level scalar YAML subset from `path`. */
bool oasp_fpm_options_load_yaml(oasp_fpm_options *options, const char *path, bool optional, char *error, size_t error_size);

#endif
