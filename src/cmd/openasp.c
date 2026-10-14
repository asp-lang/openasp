// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

/*
 * Module overview (`src/cmd/openasp.c`): Command and configuration layer. It translates
 * operator input into validated engine/server state, owns copied option strings, and reports
 * startup failures with actionable diagnostics before transferring control to the runtime.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "component.h"
#include "fpm_config.h"
#include "runtime.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s <page.asp> [--root DIR] [--query QUERY] [--body BODY] "
            "[--method METHOD] [--runtime-mode=service|batch] "
            "[--emit-oac FILE] [--gen-dbg-info=on|off] "
            "[--memory-limit=SIZE|-1] "
            "[--gc-heap-limit=SIZE] [--gc-young-size=SIZE] "
            "[--gc-old-threshold=SIZE] [--gc-step=N] [--gc-diagnostics] [-c FILE]\n",
            program);
}

static int parse_size(const char *text, size_t *result) {
    if (!text || !*text || !result)
        return 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    size_t multiplier = 1;
    if (end && (*end == 'k' || *end == 'K'))
        multiplier = 1024;
    else if (end && (*end == 'm' || *end == 'M'))
        multiplier = 1024 * 1024;
    else if (end && (*end == 'g' || *end == 'G'))
        multiplier = 1024 * 1024 * 1024;
    else if (!end || *end)
        return 0;
    if (*end)
        end++;
    if (*end || value > SIZE_MAX / multiplier)
        return 0;
    *result = (size_t)value * multiplier;
    return 1;
}

static int parse_memory_limit(const char *text, size_t *result) {
    if (text && !strcmp(text, "-1")) {
        *result = 0;
        return 1;
    }
    return parse_size(text, result) && *result != 0;
}

static int parse_on_off(const char *text, bool *result) {
    if (!strcmp(text, "on")) {
        *result = true;
        return 1;
    }
    if (!strcmp(text, "off")) {
        *result = false;
        return 1;
    }
    return 0;
}

static bool has_oac_suffix(const char *path) {
    size_t length = path ? strlen(path) : 0;
    return length > 4 && !strcmp(path + length - 4, ".oac");
}

int main(int argc, char **argv) {
    oasp_print_version(stderr);
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *page = NULL, *root = NULL, *query = "", *body = "", *method = "GET";
    const char *runtime_mode = "service";
    const char *oac_path = NULL;
    const char *config_path = NULL;
    bool gen_debug_info = false;
    size_t memory_limit = 128u * 1024u * 1024u;
    oasp_gc_config gc_config = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc)
            config_path = argv[++i];
        else if (!strcmp(argv[i], "--query") && i + 1 < argc)
            query = argv[++i];
        else if (!strcmp(argv[i], "--body") && i + 1 < argc)
            body = argv[++i];
        else if (!strcmp(argv[i], "--method") && i + 1 < argc)
            method = argv[++i];
        else if (!strncmp(argv[i], "--shell-enable=", 15))
            (void)setenv("OPENASP_SHELL_ENABLE", argv[i] + 15, 1);
        else if (!strncmp(argv[i], "--process-enable=", 17))
            (void)setenv("OPENASP_PROCESS_ENABLE", argv[i] + 17, 1);
        else if (!strncmp(argv[i], "--runtime-mode=", 15))
            runtime_mode = argv[i] + 15;
        else if (!strcmp(argv[i], "--emit-oac") && i + 1 < argc)
            oac_path = argv[++i];
        else if (!strncmp(argv[i], "--emit-oac=", 11))
            oac_path = argv[i] + 11;
        else if (!strncmp(argv[i], "--gen-dbg-info=", 15)) {
            if (!parse_on_off(argv[i] + 15, &gen_debug_info)) {
                fprintf(stderr, "openasp: --gen-dbg-info must be on or off\n");
                return 2;
            }
        }
        else if (!strncmp(argv[i], "--memory-limit=", 15)) {
            if (!parse_memory_limit(argv[i] + 15, &memory_limit)) {
                usage(argv[0]);
                return 2;
            }
        }
        else if (!strncmp(argv[i], "--gc-heap-limit=", 16)) {
            if (!parse_size(argv[i] + 16, &gc_config.heap_limit)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(argv[i], "--gc-young-size=", 16)) {
            if (!parse_size(argv[i] + 16, &gc_config.young_limit)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(argv[i], "--gc-old-threshold=", 19)) {
            if (!parse_size(argv[i] + 19, &gc_config.major_threshold)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(argv[i], "--gc-step=", 10)) {
            if (!parse_size(argv[i] + 10, &gc_config.major_step)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--gc-diagnostics"))
            gc_config.diagnostics = true;
        else if (argv[i][0] != '-')
            page = argv[i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!page) {
        usage(argv[0]);
        return 2;
    }
    oasp_fpm_options config_options;
    bool have_config = false;
    char config_error[512] = {0};
    if (config_path) {
        if (!oasp_fpm_options_init(&config_options, config_error, sizeof(config_error)) ||
            !oasp_fpm_options_load_yaml(&config_options, config_path, false, config_error, sizeof(config_error))) {
            fprintf(stderr, "openasp: %s\n", config_error);
            oasp_fpm_options_dispose(&config_options);
            return 2;
        }
        have_config = true;
        if (!root)
            root = config_options.server.root;
    }
    if (strcmp(runtime_mode, "batch") && strcmp(runtime_mode, "service")) {
        fprintf(stderr, "openasp: --runtime-mode must be batch or service\n");
        return 2;
    }
    if (gen_debug_info && !oac_path) {
        fprintf(stderr, "openasp: --gen-dbg-info=on requires --emit-oac FILE\n");
        if (have_config) oasp_fpm_options_dispose(&config_options);
        return 2;
    }
    if (oac_path && !has_oac_suffix(oac_path)) {
        fprintf(stderr, "openasp: --emit-oac destination must end in .oac\n");
        if (have_config) oasp_fpm_options_dispose(&config_options);
        return 2;
    }
    oasp_request *request = oasp_request_create(root ? root : ".", query, body, method);
    if (!request) {
        fputs("openasp: out of memory\n", stderr);
        if (have_config) oasp_fpm_options_dispose(&config_options);
        return 1;
    }
    oasp_request_limits limits = {.memory_limit = memory_limit};
    if (oasp_request_set_limits(request, &limits)) {
        fprintf(stderr, "openasp: %s\n", oasp_request_error(request));
        oasp_request_destroy(request);
        if (have_config) oasp_fpm_options_dispose(&config_options);
        return 1;
    }
    oasp_gc_stats gc_stats = {0};
    oasp_oac_options oac_options = {oac_path, gen_debug_info};
    int result;
    oasp_engine *external_engine = NULL;
    if (have_config && config_options.external_components_enabled) {
        external_engine = oasp_engine_create();
        if (!external_engine || !oasp_engine_load_external_components(external_engine, config_options.external_modules,
                                                              config_options.external_module_count,
                                                              config_options.external_components_require_sha256,
                                                              config_error, sizeof(config_error)) ||
            !oasp_engine_external_worker_init(external_engine, config_error, sizeof(config_error))) {
            fprintf(stderr, "openasp: external component initialization failed: %s\n", config_error);
            oasp_engine_destroy(external_engine);
            oasp_request_destroy(request);
            oasp_fpm_options_dispose(&config_options);
            return 1;
        }
    }
    if (!strcmp(runtime_mode, "service")) {
        if (external_engine)
            result = oac_path ? oasp_engine_render_file_service_oac(external_engine, request, page, &gc_config,
                                                                   &gc_stats, &oac_options)
                              : oasp_engine_render_file_service(external_engine, request, page, &gc_config, &gc_stats);
        else
            result = oac_path ? oasp_render_file_service_oac(request, page, &gc_config, &gc_stats, &oac_options)
                              : oasp_render_file_service(request, page, &gc_config, &gc_stats);
    } else if (external_engine)
        result = oac_path ? oasp_engine_render_file_oac(external_engine, request, page, &oac_options)
                          : oasp_engine_render_file(external_engine, request, page);
    else
        result = oac_path ? oasp_render_file_oac(request, page, &oac_options) : oasp_render_file(request, page);
    if (!result) {
        size_t length = 0;
        const char *output = oasp_request_output(request, &length);
        fwrite(output, 1, length, stdout);
    } else {
        fprintf(stderr, "openasp: cannot render %s\n", page);
        if (oasp_request_failed(request))
            fprintf(stderr, "openasp: %s\n", oasp_request_error(request));
        if (!strcmp(runtime_mode, "service") && gc_stats.allocation_failures)
            fprintf(stderr,
                    "openasp: service GC heap limit reached; raise --gc-heap-limit "
                    "or reduce retained script state\n");
    }
    oasp_request_destroy(request);
    if (external_engine) {
        oasp_engine_external_worker_shutdown(external_engine);
        oasp_engine_destroy(external_engine);
    }
    if (have_config)
        oasp_fpm_options_dispose(&config_options);
    return result ? 1 : 0;
}
