// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/log/log.h`): Process-wide logging subsystem. Configuration is established
 * before fork, regular files use append-only descriptors and daily names, and bounded stack
 * buffers keep failure reporting allocation-free and single-write per record.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_LOG_H
#define OPENASP_LOG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Ordered severities used by the process-wide logger.  A configured level
 * admits that level and every level below it in this declaration.
 */
typedef enum {
    OASP_LOG_DEBUG = 0,
    OASP_LOG_INFO = 1,
    OASP_LOG_NOTICE = 2,
    OASP_LOG_WARN = 3,
    OASP_LOG_ERROR = 4
} oasp_log_level;

/*
 * Converts a case-insensitive configuration value into a severity.  `result`
 * receives the parsed value and is unchanged when parsing fails.
 */
bool oasp_log_parse_level(const char *text, oasp_log_level *result);

/*
 * Configures the process-wide logger before workers are forked.  `path`
 * accepts "off", "stderr", "stdout", or a base file path.  File paths are
 * opened as `<path>-YYYYMMDD` and automatically reopened after a date change.
 * On failure, `error` receives an actionable diagnostic.
 */
bool oasp_log_open(const char *path, oasp_log_level level, char *error, size_t error_size);

/* Flushes ownership state and closes a file descriptor opened by the logger. */
void oasp_log_close(void);

/*
 * Emits one timestamped line when `level` meets the configured threshold.
 * Formatting is bounded so a worker performs one append write per event and
 * cannot allocate memory while reporting a failure.
 */
void oasp_log_write(oasp_log_level level, const char *format, ...);

#endif
