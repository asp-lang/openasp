// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/log/log.c`): Process-wide logging subsystem. Configuration is established
 * before fork, regular files use append-only descriptors and daily names, and bounded stack
 * buffers keep failure reporting allocation-free and single-write per record.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * The FastCGI server forks only after logging is configured, so workers inherit
 * this descriptor and can append complete records without a lock.  Each worker
 * has a private copy of the date cache and independently reopens at midnight.
 */
typedef struct {
    char *base_path;
    char daily_suffix[9];
    int descriptor;
    bool owns_descriptor;
    bool enabled;
    oasp_log_level threshold;
} oasp_logger;

static oasp_logger logger = {.descriptor = STDERR_FILENO, .enabled = true, .threshold = OASP_LOG_INFO};

/* Stores a printf-style initialization error without requiring a logger. */
static void set_error(char *error, size_t error_size, const char *format, ...) {
    if (!error || !error_size)
        return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

/* Returns the stable lowercase spelling written into each log record. */
static const char *level_name(oasp_log_level level) {
    static const char *const names[] = {"debug", "info", "notice", "warn", "error"};
    if (level < OASP_LOG_DEBUG || level > OASP_LOG_ERROR)
        return "error";
    return names[level];
}

bool oasp_log_parse_level(const char *text, oasp_log_level *result) {
    if (!text || !result)
        return false;
    for (int level = OASP_LOG_DEBUG; level <= OASP_LOG_ERROR; level++) {
        if (!strcasecmp(text, level_name((oasp_log_level)level))) {
            *result = (oasp_log_level)level;
            return true;
        }
    }
    return false;
}

/*
 * Produces local calendar text once per write.  Logging is a diagnostic path,
 * so correctness across timezone and daylight-saving changes is preferred over
 * maintaining a second-level shared cache that would require synchronization.
 */
static bool local_clock(time_t now, struct tm *local, char suffix[9]) {
    if (!localtime_r(&now, local))
        return false;
    return strftime(suffix, 9, "%Y%m%d", local) == 8;
}

/*
 * Opens the current day's append-only file.  O_APPEND gives each process a
 * kernel-mediated append position; mode 0644 remains subject to the operator's
 * umask.  The parent directory is intentionally not created implicitly because
 * a misspelled production path must fail loudly.
 */
static bool open_daily_file(time_t now, char *error, size_t error_size) {
    struct tm local;
    char suffix[9];
    if (!local_clock(now, &local, suffix)) {
        set_error(error, error_size, "cannot convert the current time for log rotation");
        return false;
    }
    size_t base_length = strlen(logger.base_path);
    if (base_length > SIZE_MAX - sizeof(suffix)) {
        set_error(error, error_size, "log-file path is too long");
        return false;
    }
    size_t path_size = base_length + 1 + sizeof(suffix);
    char *path = malloc(path_size);
    if (!path) {
        set_error(error, error_size, "out of memory while building the daily log path");
        return false;
    }
    (void)snprintf(path, path_size, "%s-%s", logger.base_path, suffix);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0)
        set_error(error, error_size, "cannot open log file '%s': %s; create its parent directory and check permissions",
                  path, strerror(errno));
    free(path);
    if (descriptor < 0)
        return false;
    if (logger.owns_descriptor && logger.descriptor >= 0)
        (void)close(logger.descriptor);
    logger.descriptor = descriptor;
    logger.owns_descriptor = true;
    memcpy(logger.daily_suffix, suffix, sizeof(suffix));
    return true;
}

bool oasp_log_open(const char *path, oasp_log_level level, char *error, size_t error_size) {
    if (!path || !*path) {
        set_error(error, error_size, "log-file requires a non-empty path, stderr, stdout, or off");
        return false;
    }
    oasp_log_close();
    logger.threshold = level;
    if (!strcasecmp(path, "off")) {
        logger.enabled = false;
        return true;
    }
    logger.enabled = true;
    if (!strcasecmp(path, "stderr")) {
        logger.descriptor = STDERR_FILENO;
        return true;
    }
    if (!strcasecmp(path, "stdout")) {
        logger.descriptor = STDOUT_FILENO;
        return true;
    }
    logger.base_path = strdup(path);
    if (!logger.base_path) {
        set_error(error, error_size, "out of memory while storing log-file");
        logger.enabled = false;
        return false;
    }
    if (!open_daily_file(time(NULL), error, error_size)) {
        free(logger.base_path);
        logger.base_path = NULL;
        logger.enabled = false;
        return false;
    }
    return true;
}

void oasp_log_close(void) {
    if (logger.owns_descriptor && logger.descriptor >= 0)
        (void)close(logger.descriptor);
    free(logger.base_path);
    logger.base_path = NULL;
    logger.daily_suffix[0] = '\0';
    logger.descriptor = STDERR_FILENO;
    logger.owns_descriptor = false;
    logger.enabled = true;
    logger.threshold = OASP_LOG_INFO;
}

/*
 * Reopens a regular log after the local date changes.  Reopen errors are sent
 * to the old descriptor because recursively invoking the logger would loop.
 */
static void rotate_if_needed(time_t now) {
    if (!logger.base_path)
        return;
    struct tm local;
    char suffix[9];
    if (!local_clock(now, &local, suffix) || !strcmp(suffix, logger.daily_suffix))
        return;
    char error[512] = {0};
    if (!open_daily_file(now, error, sizeof(error)) && logger.descriptor >= 0) {
        char fallback[640];
        int length = snprintf(fallback, sizeof(fallback), "openasp-fpm: log rotation failed: %s\n", error);
        if (length > 0) {
            size_t fallback_length = (size_t)length < sizeof(fallback) ? (size_t)length : sizeof(fallback) - 1;
            ssize_t written = write(logger.descriptor, fallback, fallback_length);
            (void)written;
        }
    }
}

void oasp_log_write(oasp_log_level level, const char *format, ...) {
    if (!logger.enabled || !format || level < logger.threshold)
        return;
    time_t now = time(NULL);
    rotate_if_needed(now);
    if (logger.descriptor < 0)
        return;
    struct tm local;
    char timestamp[20] = "0000/00/00 00:00:00";
    char unused_suffix[9];
    if (local_clock(now, &local, unused_suffix))
        (void)strftime(timestamp, sizeof(timestamp), "%Y/%m/%d %H:%M:%S", &local);
    char line[4096];
    int prefix = snprintf(line, sizeof(line), "%s [%s] %ld: ", timestamp, level_name(level), (long)getpid());
    if (prefix < 0 || (size_t)prefix >= sizeof(line) - 2)
        return;
    va_list arguments;
    va_start(arguments, format);
    int body = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 1, format, arguments);
    va_end(arguments);
    if (body < 0)
        return;
    size_t length = (size_t)prefix + ((size_t)body < sizeof(line) - (size_t)prefix - 1
                                         ? (size_t)body
                                         : sizeof(line) - (size_t)prefix - 2);
    line[length++] = '\n';
    ssize_t written;
    do {
        written = write(logger.descriptor, line, length);
    } while (written < 0 && errno == EINTR);
}
