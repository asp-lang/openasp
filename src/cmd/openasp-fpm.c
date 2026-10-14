// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/cmd/openasp-fpm.c`): Command and configuration layer. It translates
 * operator input into validated engine/server state, owns copied option strings, and reports
 * startup failures with actionable diagnostics before transferring control to the runtime.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "fpm_config.h"
#include "log.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define OASP_FPM_DEFAULT_CONFIG "/etc/openasp-fpm.yaml"

/* Prints the complete command surface; YAML uses the same names without `--`. */
static void usage(void) {
    puts("openasp-fpm: FastCGI server for OpenASP");
    puts("Usage: openasp-fpm [-c <path>] [--daemon=on|off] [--root <dir>] "
         "[--host <127.0.0.1> --port <9000> | --unix-socket <path>] "
         "[--max-accepts <n>] [--max-requests <n>] [--memory-idle-ms <n>] [--memory-relief-requests <n>] "
         "[--workers <n>] "
         "[--max-params-bytes <n>] [--max-body-bytes <n>] [--max-request-bytes <n>] "
         "[--client-timeout-ms <n>] [--request-timeout-ms <n>] "
         "[--memory-limit <SIZE|-1>] "
         "[--max-buffered-response-bytes <n>] [--max-response-header-bytes <n>] [--max-response-headers <n>] "
         "[--opcache-memory <MiB>] [--opcache-dir <path>] "
         "[--opcache-validate-timestamps=on|off] [--opcache-revalidate-seconds <n>] "
         "[--gen-dbg-info=on|off] "
         "[--debug-enable=on|off] [--debug-unix-socket <path>] "
         "[--debug-tcp-host <127.0.0.1> --debug-tcp-port <n> --debug-token <token>] "
         "[--log-file <off|stderr|stdout|path>] [--log-level <debug|info|notice|warn|error>] "
         "[--shell-enable=on|off] [--process-enable=on|off]");
    puts("Configuration priority: command line > YAML file > built-in defaults.");
}

/* Reports a startup error, releases option-owned strings, and returns EX_USAGE-like status 2. */
static int fail_options(oasp_fpm_options *options, const char *message) {
    fprintf(stderr, "openasp-fpm: %s\n", message);
    oasp_fpm_options_dispose(options);
    return 2;
}

/* Rejects missing CLI values before forwarding canonical key/value text to the shared setter. */
static bool set_option(oasp_fpm_options *options, const char *key, const char *value, char *error, size_t error_size) {
    if (value && *value)
        return oasp_fpm_options_set(options, key, value, error, error_size);
    (void)snprintf(error, error_size, "option --%s requires a non-empty value", key);
    return false;
}

/*
 * Performs the conventional double fork: the first child creates a new
 * session, the second cannot reacquire a controlling terminal, and standard
 * descriptors are redirected only after configuration and log setup.
 *
 * Returns 1 in the launching parent, 0 in the final daemon, and -1 when the
 * first fork fails. Child-only failures use _exit() because inherited stdio
 * buffers must not be flushed twice.
 */
static int daemonize_process(void) {
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child > 0)
        return 1;
    if (setsid() < 0)
        _exit(2);
    child = fork();
    if (child < 0)
        _exit(2);
    if (child > 0)
        _exit(0);
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
        _exit(2);
    if (dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0)
        _exit(2);
    if (null_fd > STDERR_FILENO)
        close(null_fd);
    return 0;
}

/* Matches `--name=value` exactly and returns a borrowed pointer to its value. */
static bool option_with_equals(const char *argument, const char *name, const char **value) {
    size_t length = strlen(name);
    if (strncmp(argument, name, length) || argument[length] != '=')
        return false;
    *value = argument + length + 1;
    return true;
}

/*
 * Applies precedence in three passes: locate the requested YAML path, load
 * defaults plus YAML, then replay every CLI option. Environment gates and the
 * logger are installed before daemonization; the validated server projection
 * is finally handed to the blocking FastCGI supervisor.
 */
int main(int argc, char **argv) {
    oasp_print_version(stderr);
    const char *config_path = OASP_FPM_DEFAULT_CONFIG;
    bool config_optional = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "openasp-fpm: %s requires a configuration file path\n", argv[i]);
                return 2;
            }
            const char *candidate = argv[++i];
            if (*candidate) {
                config_path = candidate;
                config_optional = false;
            } else {
                config_path = OASP_FPM_DEFAULT_CONFIG;
                config_optional = true;
            }
        }
    }

    char error[1024] = {0};
    oasp_fpm_options options;
    if (!oasp_fpm_options_init(&options, error, sizeof(error))) {
        fprintf(stderr, "openasp-fpm: %s\n", error);
        return 2;
    }
    if (!oasp_fpm_options_load_yaml(&options, config_path, config_optional, error, sizeof(error)))
        return fail_options(&options, error);

    bool command_host_or_port = false;
    bool command_unix = false;
    for (int i = 1; i < argc; i++) {
        const char *key = NULL;
        const char *value = NULL;
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            i++;
            continue;
        }
        if (!strcmp(argv[i], "--root"))
            key = "root";
        else if (!strcmp(argv[i], "--host")) {
            key = "host";
            command_host_or_port = true;
        } else if (!strcmp(argv[i], "--port")) {
            key = "port";
            command_host_or_port = true;
        } else if (!strcmp(argv[i], "--unix-socket")) {
            key = "unix-socket";
            command_unix = true;
        } else if (!strcmp(argv[i], "--max-accepts"))
            key = "max-accepts";
        else if (!strcmp(argv[i], "--max-requests"))
            key = "max-requests";
        else if (!strcmp(argv[i], "--memory-idle-ms"))
            key = "memory-idle-ms";
        else if (!strcmp(argv[i], "--memory-relief-requests"))
            key = "memory-relief-requests";
        else if (!strcmp(argv[i], "--workers"))
            key = "workers";
        else if (!strcmp(argv[i], "--max-params-bytes"))
            key = "max-params-bytes";
        else if (!strcmp(argv[i], "--max-body-bytes"))
            key = "max-body-bytes";
        else if (!strcmp(argv[i], "--max-request-bytes"))
            key = "max-request-bytes";
        else if (!strcmp(argv[i], "--client-timeout-ms"))
            key = "client-timeout-ms";
        else if (!strcmp(argv[i], "--request-timeout-ms"))
            key = "request-timeout-ms";
        else if (!strcmp(argv[i], "--memory-limit"))
            key = "memory-limit";
        else if (!strcmp(argv[i], "--max-buffered-response-bytes"))
            key = "max-buffered-response-bytes";
        else if (!strcmp(argv[i], "--max-response-header-bytes"))
            key = "max-response-header-bytes";
        else if (!strcmp(argv[i], "--max-response-headers"))
            key = "max-response-headers";
        else if (!strcmp(argv[i], "--opcache-memory"))
            key = "opcache-memory";
        else if (!strcmp(argv[i], "--opcache-dir"))
            key = "opcache-dir";
        else if (!strcmp(argv[i], "--opcache-revalidate-seconds"))
            key = "opcache-revalidate-seconds";
        else if (!strcmp(argv[i], "--gen-dbg-info"))
            key = "gen-dbg-info";
        else if (!strcmp(argv[i], "--debug-enable"))
            key = "debug-enable";
        else if (!strcmp(argv[i], "--debug-unix-socket"))
            key = "debug-unix-socket";
        else if (!strcmp(argv[i], "--debug-tcp-host"))
            key = "debug-tcp-host";
        else if (!strcmp(argv[i], "--debug-tcp-port"))
            key = "debug-tcp-port";
        else if (!strcmp(argv[i], "--debug-token"))
            key = "debug-token";
        else if (!strcmp(argv[i], "--log-file"))
            key = "log-file";
        else if (!strcmp(argv[i], "--log-level"))
            key = "log-level";
        if (key) {
            if (i + 1 >= argc)
                return fail_options(&options, "option requires a value; run --help for usage");
            value = argv[++i];
        } else if (option_with_equals(argv[i], "--opcache-validate-timestamps", &value))
            key = "opcache-validate-timestamps";
        else if (option_with_equals(argv[i], "--gen-dbg-info", &value))
            key = "gen-dbg-info";
        else if (option_with_equals(argv[i], "--debug-enable", &value))
            key = "debug-enable";
        else if (option_with_equals(argv[i], "--shell-enable", &value))
            key = "shell-enable";
        else if (option_with_equals(argv[i], "--process-enable", &value))
            key = "process-enable";
        else if (option_with_equals(argv[i], "--daemon", &value))
            key = "daemon";
        else {
            usage();
            return fail_options(&options, "unknown or incomplete option; run --help for usage");
        }
        if (!set_option(&options, key, value, error, sizeof(error)))
            return fail_options(&options, error);
    }
    if (command_unix && command_host_or_port)
        return fail_options(&options, "--unix-socket cannot be combined with --host or --port");
    if (options.server.debug_enabled && (!options.server.debug_unix_path || !*options.server.debug_unix_path))
        return fail_options(&options, "debug-enable requires --debug-unix-socket <path>");
    if (options.server.debug_tcp_port && !options.server.debug_enabled)
        return fail_options(&options, "debug TCP requires --debug-enable=on");
    if (options.server.debug_tcp_port &&
        (!options.server.debug_tcp_host || !*options.server.debug_tcp_host || !options.server.debug_token || !*options.server.debug_token))
        return fail_options(&options, "debug TCP requires --debug-tcp-host, --debug-tcp-port, and --debug-token");
    if (options.shell_enable && setenv("OPENASP_SHELL_ENABLE", options.shell_enable, 1))
        return fail_options(&options, "cannot apply shell-enable environment setting");
    if (options.process_enable && setenv("OPENASP_PROCESS_ENABLE", options.process_enable, 1))
        return fail_options(&options, "cannot apply process-enable environment setting");
    if (!oasp_log_open(options.log_file, options.log_level, error, sizeof(error)))
        return fail_options(&options, error);
    if (options.daemon) {
        int daemon_result = daemonize_process();
        if (daemon_result < 0) {
            (void)snprintf(error, sizeof(error), "cannot enter daemon mode: %s", strerror(errno));
            oasp_log_write(OASP_LOG_ERROR, "%s", error);
            oasp_log_close();
            return fail_options(&options, error);
        }
        if (daemon_result > 0) {
            oasp_log_close();
            oasp_fpm_options_dispose(&options);
            return 0;
        }
    }
    oasp_log_write(OASP_LOG_NOTICE, "server starting in %s mode", options.daemon ? "daemon" : "foreground");
    int result = oasp_fastcgi_run(&options.server);
    oasp_log_write(result ? OASP_LOG_ERROR : OASP_LOG_NOTICE, "server stopped with status %d", result);
    oasp_log_close();
    oasp_fpm_options_dispose(&options);
    return result;
}
