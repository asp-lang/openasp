// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/cmd/fpm_config.c`): Command and configuration layer. It translates
 * operator input into validated engine/server state, owns copied option strings, and reports
 * startup failures with actionable diagnostics before transferring control to the runtime.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "fpm_config.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Writes a bounded parser/validation diagnostic when the caller supplied storage. */
static void set_error(char *error, size_t error_size, const char *format, ...) {
    if (!error || !error_size)
        return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

/*
 * Atomically replaces one owned option string: allocation happens before the
 * old value is freed, so an out-of-memory failure leaves configuration intact.
 */
static bool replace_string(char **destination, const char *value, char *error, size_t error_size) {
    char *copy = strdup(value);
    if (!copy) {
        set_error(error, error_size, "out of memory while storing configuration value");
        return false;
    }
    free(*destination);
    *destination = copy;
    return true;
}

/* Parses decimal digits only and rejects signs, suffixes, overflow, and trailing text. */
static bool parse_unsigned(const char *text, unsigned long long maximum, unsigned long long *result) {
    if (!text || !*text)
        return false;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; cursor++)
        if (*cursor < '0' || *cursor > '9')
            return false;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end || value > maximum)
        return false;
    *result = value;
    return true;
}

/* Applies an unsigned configuration scalar to a platform `long` destination. */
static bool set_long(long *destination, const char *key, const char *value, char *error, size_t error_size) {
    unsigned long long parsed;
    if (!parse_unsigned(value, LONG_MAX, &parsed)) {
        set_error(error, error_size, "invalid %s value '%s'; expected an unsigned integer in range 0..%ld",
                  key, value, LONG_MAX);
        return false;
    }
    *destination = (long)parsed;
    return true;
}

/* Parses an integer and enforces the option-specific inclusive range. */
static bool set_int(int *destination, const char *key, const char *value, int minimum, int maximum,
                    char *error, size_t error_size) {
    unsigned long long parsed;
    if (!parse_unsigned(value, (unsigned long long)maximum, &parsed) || parsed < (unsigned long long)minimum) {
        set_error(error, error_size, "invalid %s value '%s'; expected an integer in range %d..%d",
                  key, value, minimum, maximum);
        return false;
    }
    *destination = (int)parsed;
    return true;
}

/* Parses byte/count values without narrowing through `long` on 64-bit platforms. */
static bool set_size(size_t *destination, const char *key, const char *value, char *error, size_t error_size) {
    unsigned long long parsed;
    if (!parse_unsigned(value, SIZE_MAX, &parsed)) {
        set_error(error, error_size, "invalid %s value '%s'; expected an unsigned integer", key, value);
        return false;
    }
    *destination = (size_t)parsed;
    return true;
}

static bool set_memory_limit(size_t *destination, const char *key, const char *value, char *error, size_t error_size) {
    if (!strcmp(value, "-1")) {
        *destination = 0;
        return true;
    }
    if (!value || !*value || value[0] == '-') {
        set_error(error, error_size, "invalid %s value '%s'; expected SIZE or -1", key, value ? value : "");
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    size_t multiplier = 1;
    if (*end == 'k' || *end == 'K')
        multiplier = 1024u;
    else if (*end == 'm' || *end == 'M')
        multiplier = 1024u * 1024u;
    else if (*end == 'g' || *end == 'G')
        multiplier = 1024u * 1024u * 1024u;
    else if (*end)
        multiplier = 0;
    if (*end)
        end++;
    if (errno == ERANGE || *end || !parsed || !multiplier || parsed > SIZE_MAX / multiplier) {
        set_error(error, error_size, "invalid %s value '%s'; expected positive bytes with optional K, M, G suffix, or -1",
                  key, value);
        return false;
    }
    *destination = (size_t)parsed * multiplier;
    return true;
}

/* Accepts the explicit YAML/CLI spellings `on` and `off`; implicit booleans are rejected. */
static bool set_on_off(bool *destination, const char *key, const char *value, char *error, size_t error_size) {
    if (!strcmp(value, "on")) {
        *destination = true;
        return true;
    }
    if (!strcmp(value, "off")) {
        *destination = false;
        return true;
    }
    set_error(error, error_size, "invalid %s value '%s'; expected on or off", key, value);
    return false;
}

/*
 * Establishes all defaults and synchronizes borrowed pointers in `server`.
 * Initialization uses a temporary-directory cache unique to the effective UID
 * unless OPENASP_CACHE_DIR supplies an operator-managed location.
 */
bool oasp_fpm_options_init(oasp_fpm_options *options, char *error, size_t error_size) {
    if (!options)
        return false;
    memset(options, 0, sizeof(*options));
    const char *temporary = getenv("TMPDIR");
    if (!temporary || !*temporary)
        temporary = "/tmp";
    char default_cache_directory[4096];
    const char *environment_cache = getenv("OPENASP_CACHE_DIR");
    if (environment_cache && *environment_cache) {
        if (snprintf(default_cache_directory, sizeof(default_cache_directory), "%s", environment_cache) >=
            (int)sizeof(default_cache_directory)) {
            set_error(error, error_size, "OPENASP_CACHE_DIR is too long");
            return false;
        }
    } else if (snprintf(default_cache_directory, sizeof(default_cache_directory), "%s/openasp-opcache-%lu",
                        temporary, (unsigned long)getuid()) >= (int)sizeof(default_cache_directory)) {
        set_error(error, error_size, "temporary directory path is too long");
        return false;
    }
    options->root = strdup(".");
    options->host = strdup("127.0.0.1");
    options->cache_directory = strdup(default_cache_directory);
    options->log_file = strdup("stderr");
    if (!options->root || !options->host || !options->cache_directory || !options->log_file) {
        set_error(error, error_size, "out of memory while initializing FPM configuration");
        oasp_fpm_options_dispose(options);
        return false;
    }
    options->server.root = options->root;
    options->server.host = options->host;
    options->server.port = 9000;
    options->server.max_requests = 10000;
    options->server.memory_idle_ms = 30000;
    options->server.memory_relief_requests = 64;
    options->server.workers = 1;
    options->server.cache_capacity = 64u * 1024u * 1024u;
    options->server.cache_directory = options->cache_directory;
    options->server.limits.max_params_bytes = 1u * 1024u * 1024u;
    options->server.limits.max_body_bytes = 64u * 1024u * 1024u;
    options->server.limits.max_request_bytes = 65u * 1024u * 1024u;
    options->server.limits.client_timeout_ms = 30000;
    options->server.limits.request.max_buffered_response_bytes = 64u * 1024u * 1024u;
    options->server.limits.request.memory_limit = 128u * 1024u * 1024u;
    options->server.limits.request.max_response_header_bytes = 64u * 1024u;
    options->server.limits.request.max_response_header_count = 256;
    options->server.limits.request.request_timeout_ms = 30000;
    options->log_level = OASP_LOG_INFO;
    return true;
}

/* Frees every owned string and zeroes the aggregate to make accidental reuse visible. */
void oasp_fpm_options_dispose(oasp_fpm_options *options) {
    if (!options)
        return;
    free(options->root);
    free(options->host);
    free(options->unix_path);
    free(options->cache_directory);
    free(options->debug_unix_path);
    free(options->debug_tcp_host);
    free(options->debug_token);
    free(options->log_file);
    free(options->shell_enable);
    free(options->process_enable);
    for (size_t i = 0; i < options->external_module_count; i++) {
        oasp_external_module_config *module = &options->external_modules[i];
        free(module->id);
        free(module->library);
        free(module->sha256);
        for (size_t j = 0; j < module->prog_id_count; j++)
            free(module->prog_ids[j]);
        free(module->prog_ids);
    }
    free(options->external_modules);
    memset(options, 0, sizeof(*options));
}

static bool external_append_string(char ***items, size_t *count, const char *value, char *error, size_t error_size) {
    char **grown = realloc(*items, (*count + 1) * sizeof(**items));
    char *copy = strdup(value);
    if (!grown || !copy) {
        free(copy);
        set_error(error, error_size, "out of memory while storing external component configuration");
        return false;
    }
    *items = grown;
    (*items)[(*count)++] = copy;
    return true;
}

static bool external_valid_id(const char *id) {
    size_t length = id ? strlen(id) : 0;
    if (!length || length > 63)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '-'))
            return false;
    return true;
}

static bool external_capability(const char *name, uint64_t *bit) {
    static const struct { const char *name; uint64_t bit; } values[] = {
        {"crypto", OASP_EXTERNAL_CAP_CRYPTO}, {"filesystem", OASP_EXTERNAL_CAP_FILESYSTEM},
        {"network", OASP_EXTERNAL_CAP_NETWORK}, {"database", OASP_EXTERNAL_CAP_DATABASE},
        {"process", OASP_EXTERNAL_CAP_PROCESS}, {"threads", OASP_EXTERNAL_CAP_THREADS}
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        if (!strcmp(name, values[i].name)) { *bit = values[i].bit; return true; }
    return false;
}

static bool external_finish_module(oasp_external_module_config *module, bool require_sha256, char *error, size_t error_size) {
    if (!module)
        return true;
    if (!external_valid_id(module->id) || !module->library || module->library[0] != '/' ||
        strchr(module->library, '\n') || !module->prog_id_count ||
        (module->mode != OASP_EXTERNAL_MODE_IN_PROCESS && module->mode != OASP_EXTERNAL_MODE_ISOLATED) ||
        (require_sha256 && (!module->sha256 || strlen(module->sha256) != 64))) {
        set_error(error, error_size, "external component requires valid id, absolute library, mode, sha256, and non-empty prog-ids");
        return false;
    }
    if (module->sha256)
        for (size_t i = 0; module->sha256[i]; i++)
            if (!((module->sha256[i] >= '0' && module->sha256[i] <= '9') ||
                  (module->sha256[i] >= 'a' && module->sha256[i] <= 'f'))) {
                set_error(error, error_size, "external component sha256 must contain 64 lowercase hexadecimal characters");
                return false;
            }
    return true;
}

/*
 * Central option dispatcher shared by YAML and command-line parsing. Each
 * branch validates before mutation; listener options also clear conflicting
 * Unix/TCP state so the last source at the highest precedence wins.
 */
bool oasp_fpm_options_set(oasp_fpm_options *options, const char *key, const char *value, char *error, size_t error_size) {
    if (!options || !key || !value) {
        set_error(error, error_size, "invalid configuration setter arguments");
        return false;
    }
    if (!strcmp(key, "root")) {
        if (!replace_string(&options->root, value, error, error_size))
            return false;
        options->server.root = options->root;
    } else if (!strcmp(key, "host")) {
        if (!replace_string(&options->host, value, error, error_size))
            return false;
        options->server.host = options->host;
        free(options->unix_path);
        options->unix_path = NULL;
        options->server.unix_path = NULL;
    } else if (!strcmp(key, "port")) {
        if (!set_int(&options->server.port, key, value, 1, 65535, error, error_size))
            return false;
        free(options->unix_path);
        options->unix_path = NULL;
        options->server.unix_path = NULL;
    } else if (!strcmp(key, "unix-socket")) {
        if (!replace_string(&options->unix_path, value, error, error_size))
            return false;
        options->server.unix_path = options->unix_path;
    } else if (!strcmp(key, "max-accepts")) {
        return set_long(&options->server.max_accepts, key, value, error, error_size);
    } else if (!strcmp(key, "max-requests")) {
        return set_long(&options->server.max_requests, key, value, error, error_size);
    } else if (!strcmp(key, "memory-idle-ms")) {
        return set_int(&options->server.memory_idle_ms, key, value, 0, INT_MAX, error, error_size);
    } else if (!strcmp(key, "memory-relief-requests")) {
        return set_long(&options->server.memory_relief_requests, key, value, error, error_size);
    } else if (!strcmp(key, "workers")) {
        return set_int(&options->server.workers, key, value, 1, INT_MAX, error, error_size);
    } else if (!strcmp(key, "max-params-bytes")) {
        return set_size(&options->server.limits.max_params_bytes, key, value, error, error_size);
    } else if (!strcmp(key, "max-body-bytes")) {
        return set_size(&options->server.limits.max_body_bytes, key, value, error, error_size);
    } else if (!strcmp(key, "max-request-bytes")) {
        return set_size(&options->server.limits.max_request_bytes, key, value, error, error_size);
    } else if (!strcmp(key, "client-timeout-ms")) {
        return set_int(&options->server.limits.client_timeout_ms, key, value, 0, INT_MAX, error, error_size);
    } else if (!strcmp(key, "request-timeout-ms")) {
        unsigned long long parsed;
        if (!parse_unsigned(value, ULONG_MAX, &parsed)) {
            set_error(error, error_size, "invalid %s value '%s'; expected an unsigned integer", key, value);
            return false;
        }
        options->server.limits.request.request_timeout_ms = (unsigned long)parsed;
    } else if (!strcmp(key, "memory-limit")) {
        return set_memory_limit(&options->server.limits.request.memory_limit, key, value, error, error_size);
    } else if (!strcmp(key, "max-buffered-response-bytes")) {
        return set_size(&options->server.limits.request.max_buffered_response_bytes, key, value, error, error_size);
    } else if (!strcmp(key, "max-response-header-bytes")) {
        return set_size(&options->server.limits.request.max_response_header_bytes, key, value, error, error_size);
    } else if (!strcmp(key, "max-response-headers")) {
        return set_size(&options->server.limits.request.max_response_header_count, key, value, error, error_size);
    } else if (!strcmp(key, "opcache-memory")) {
        size_t megabytes;
        if (!set_size(&megabytes, key, value, error, error_size) || !megabytes ||
            megabytes > SIZE_MAX / (1024u * 1024u)) {
            set_error(error, error_size, "invalid %s value '%s'; expected positive MiB without a suffix", key, value);
            return false;
        }
        options->server.cache_capacity = megabytes * 1024u * 1024u;
    } else if (!strcmp(key, "opcache-dir")) {
        if (!replace_string(&options->cache_directory, value, error, error_size))
            return false;
        options->server.cache_directory = options->cache_directory;
    } else if (!strcmp(key, "opcache-validate-timestamps")) {
        bool enabled;
        if (!set_on_off(&enabled, key, value, error, error_size))
            return false;
        options->server.validation_interval = enabled ? 0 : -1;
    } else if (!strcmp(key, "opcache-revalidate-seconds")) {
        return set_long(&options->server.validation_interval, key, value, error, error_size);
    } else if (!strcmp(key, "gen-dbg-info")) {
        return set_on_off(&options->server.gen_debug_info, key, value, error, error_size);
    } else if (!strcmp(key, "debug-enable")) {
        return set_on_off(&options->server.debug_enabled, key, value, error, error_size);
    } else if (!strcmp(key, "debug-unix-socket")) {
        if (!replace_string(&options->debug_unix_path, value, error, error_size))
            return false;
        options->server.debug_unix_path = options->debug_unix_path;
    } else if (!strcmp(key, "debug-tcp-host")) {
        if (!replace_string(&options->debug_tcp_host, value, error, error_size))
            return false;
        options->server.debug_tcp_host = options->debug_tcp_host;
    } else if (!strcmp(key, "debug-tcp-port")) {
        return set_int(&options->server.debug_tcp_port, key, value, 1, 65535, error, error_size);
    } else if (!strcmp(key, "debug-token")) {
        if (!replace_string(&options->debug_token, value, error, error_size))
            return false;
        options->server.debug_token = options->debug_token;
    } else if (!strcmp(key, "shell-enable")) {
        bool enabled;
        if (!set_on_off(&enabled, key, value, error, error_size) ||
            !replace_string(&options->shell_enable, enabled ? "on" : "off", error, error_size))
            return false;
    } else if (!strcmp(key, "process-enable")) {
        bool enabled;
        if (!set_on_off(&enabled, key, value, error, error_size) ||
            !replace_string(&options->process_enable, enabled ? "on" : "off", error, error_size))
            return false;
    } else if (!strcmp(key, "daemon")) {
        return set_on_off(&options->daemon, key, value, error, error_size);
    } else if (!strcmp(key, "log-file")) {
        return replace_string(&options->log_file, value, error, error_size);
    } else if (!strcmp(key, "log-level")) {
        oasp_log_level level;
        if (!oasp_log_parse_level(value, &level)) {
            set_error(error, error_size,
                      "invalid log-level value '%s'; expected debug, info, notice, warn, or error", value);
            return false;
        }
        options->log_level = level;
    } else {
        set_error(error, error_size, "unknown configuration key '%s'; use an openasp-fpm option name without '--'", key);
        return false;
    }
    return true;
}

/* Trims ASCII YAML spacing in place and returns the first retained byte. */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t')
        text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return text;
}

/*
 * Parses one scalar from the deliberately small YAML subset. Quotes protect
 * `#` characters; unquoted comments start only after whitespace. Escape
 * expansion and nested YAML are intentionally unsupported and rejected.
 */
static bool parse_scalar(char *value, char **result, char *error, size_t error_size) {
    value = trim(value);
    if (!*value) {
        *result = value;
        return true;
    }
    if (value[0] == '\'' || value[0] == '"') {
        char quote = value[0];
        char *closing = NULL;
        for (char *cursor = value + 1; *cursor; cursor++) {
            if (*cursor == quote && (quote == '\'' || cursor[-1] != '\\')) {
                closing = cursor;
                break;
            }
        }
        if (!closing) {
            set_error(error, error_size, "unterminated quoted scalar");
            return false;
        }
        char *suffix = trim(closing + 1);
        if (*suffix && *suffix != '#') {
            set_error(error, error_size, "unexpected text after quoted scalar");
            return false;
        }
        *closing = '\0';
        value++;
    } else {
        for (char *cursor = value; *cursor; cursor++) {
            if (*cursor == '#' && (cursor == value || cursor[-1] == ' ' || cursor[-1] == '\t')) {
                *cursor = '\0';
                break;
            }
        }
        value = trim(value);
    }
    *result = value;
    return true;
}

/*
 * Loads top-level `key: scalar` records in source order. `optional` suppresses
 * only ENOENT for the default path; malformed and unreadable explicit files
 * always include the path and line number in the returned error.
 */
bool oasp_fpm_options_load_yaml(oasp_fpm_options *options, const char *path, bool optional, char *error, size_t error_size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        if (optional && errno == ENOENT)
            return true;
        set_error(error, error_size, "cannot open configuration file '%s': %s; check the path and permissions",
                  path, strerror(errno));
        return false;
    }
    char *line = NULL;
    size_t capacity = 0;
    size_t line_number = 0;
    bool success = true;
    bool external_block = false;
    enum { EXTERNAL_NONE, EXTERNAL_CAPABILITIES, EXTERNAL_PROG_IDS } external_list = EXTERNAL_NONE;
    oasp_external_module_config *module = NULL;
    while (getline(&line, &capacity, file) >= 0) {
        line_number++;
        if (strchr(line, '\t')) {
            set_error(error, error_size, "%s:%zu: tabs are not allowed in YAML indentation", path, line_number);
            success = false;
            break;
        }
        size_t indent = 0;
        while (line[indent] == ' ')
            indent++;
        char *content = trim(line);
        if (!*content || *content == '#' || !strcmp(content, "---") || !strcmp(content, "..."))
            continue;
        if (!indent && !strcmp(content, "external-components:")) {
            external_block = true;
            external_list = EXTERNAL_NONE;
            continue;
        }
        if (!indent)
            external_block = false;
        if (external_block) {
            char *value = NULL;
            char detail[256] = {0};
            if (indent == 2 && !strncmp(content, "enabled:", 8)) {
                if (!parse_scalar(content + 8, &value, detail, sizeof(detail)) ||
                    !set_on_off(&options->external_components_enabled, "external-components.enabled", value, error, error_size)) {
                    success = false;
                    break;
                }
                continue;
            }
            if (indent == 2 && !strncmp(content, "require-sha256:", 15)) {
                if (!parse_scalar(content + 15, &value, detail, sizeof(detail)) ||
                    !set_on_off(&options->external_components_require_sha256, "external-components.require-sha256", value, error, error_size)) {
                    success = false;
                    break;
                }
                continue;
            }
            if (indent == 2 && !strcmp(content, "modules:")) {
                external_list = EXTERNAL_NONE;
                continue;
            }
            if (indent == 4 && !strncmp(content, "- id:", 5)) {
                if (!external_finish_module(module, options->external_components_require_sha256, error, error_size)) {
                    success = false;
                    break;
                }
                oasp_external_module_config *grown = realloc(options->external_modules,
                    (options->external_module_count + 1) * sizeof(*grown));
                if (!grown || !parse_scalar(content + 5, &value, detail, sizeof(detail))) {
                    set_error(error, error_size, "%s:%zu: invalid external module id", path, line_number);
                    success = false;
                    break;
                }
                options->external_modules = grown;
                module = &grown[options->external_module_count++];
                memset(module, 0, sizeof(*module));
                module->id = strdup(value);
                external_list = EXTERNAL_NONE;
                continue;
            }
            if (indent == 6 && module) {
                char *colon = strchr(content, ':');
                if (!colon) {
                    success = false;
                    set_error(error, error_size, "%s:%zu: expected external module key", path, line_number);
                    break;
                }
                *colon = '\0';
                char *key = content;
                if (!strcmp(key, "capabilities") || !strcmp(key, "prog-ids")) {
                    if (*trim(colon + 1)) {
                        success = false;
                        set_error(error, error_size, "%s:%zu: external list must use block entries", path, line_number);
                        break;
                    }
                    external_list = !strcmp(key, "capabilities") ? EXTERNAL_CAPABILITIES : EXTERNAL_PROG_IDS;
                    continue;
                }
                external_list = EXTERNAL_NONE;
                if (!parse_scalar(colon + 1, &value, detail, sizeof(detail))) {
                    success = false;
                    set_error(error, error_size, "%s:%zu: %s", path, line_number, detail);
                    break;
                }
                if (!strcmp(key, "library"))
                    success = replace_string(&module->library, value, error, error_size);
                else if (!strcmp(key, "sha256"))
                    success = replace_string(&module->sha256, value, error, error_size);
                else if (!strcmp(key, "mode")) {
                    if (!strcmp(value, "in-process")) module->mode = OASP_EXTERNAL_MODE_IN_PROCESS;
                    else if (!strcmp(value, "isolated")) module->mode = OASP_EXTERNAL_MODE_ISOLATED;
                    else { set_error(error, error_size, "%s:%zu: mode must be in-process or isolated", path, line_number); success = false; }
                } else {
                    set_error(error, error_size, "%s:%zu: unknown external module key '%s'", path, line_number, key);
                    success = false;
                }
                if (!success) break;
                continue;
            }
            if (indent == 8 && module && !strncmp(content, "- ", 2) && external_list != EXTERNAL_NONE) {
                if (!parse_scalar(content + 2, &value, detail, sizeof(detail))) {
                    success = false;
                    break;
                }
                if (external_list == EXTERNAL_PROG_IDS)
                    success = external_append_string(&module->prog_ids, &module->prog_id_count, value, error, error_size);
                else {
                    uint64_t bit = 0;
                    success = external_capability(value, &bit);
                    if (success) module->capabilities |= bit;
                    else set_error(error, error_size, "%s:%zu: unknown external capability '%s'", path, line_number, value);
                }
                if (!success) break;
                continue;
            }
            set_error(error, error_size, "%s:%zu: invalid external-components indentation or key", path, line_number);
            success = false;
            break;
        }
        if (content != line) {
            set_error(error, error_size, "%s:%zu: nested or indented YAML is not supported; use top-level 'key: value'",
                      path, line_number);
            success = false;
            break;
        }
        char *colon = strchr(content, ':');
        if (!colon) {
            set_error(error, error_size, "%s:%zu: expected 'key: value'", path, line_number);
            success = false;
            break;
        }
        *colon = '\0';
        char *key = trim(content);
        char *value;
        char scalar_error[256] = {0};
        if (!*key || strchr(key, ' ') || strchr(key, '\t') ||
            !parse_scalar(colon + 1, &value, scalar_error, sizeof(scalar_error))) {
            if (*scalar_error)
                set_error(error, error_size, "%s:%zu: %s", path, line_number, scalar_error);
            else
                set_error(error, error_size, "%s:%zu: invalid configuration key", path, line_number);
            success = false;
            break;
        }
        if (!*value)
            continue;
        char detail[512] = {0};
        if (!oasp_fpm_options_set(options, key, value, detail, sizeof(detail))) {
            set_error(error, error_size, "%s:%zu: %s", path, line_number, detail);
            success = false;
            break;
        }
    }
    if (success && !external_finish_module(module, options->external_components_require_sha256, error, error_size))
        success = false;
    options->server.external_components_enabled = options->external_components_enabled;
    options->server.external_components_require_sha256 = options->external_components_require_sha256;
    options->server.external_modules = options->external_modules;
    options->server.external_module_count = options->external_module_count;
    if (success && ferror(file)) {
        set_error(error, error_size, "cannot read configuration file '%s': %s", path, strerror(errno));
        success = false;
    }
    free(line);
    fclose(file);
    return success;
}
