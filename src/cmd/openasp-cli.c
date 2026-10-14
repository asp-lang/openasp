// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/cmd/openasp-cli.c`): Command and configuration layer. It translates
 * operator input into validated engine/server state, owns copied option strings, and reports
 * startup failures with actionable diagnostics before transferring control to the runtime.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "cli_shell.h"
#include "interactive.h"
#include "internal.h"
#include "line_editor.h"
#include "version.h"

#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [--root DIR] [--memory-limit=SIZE|-1] [--shell-enable=on|off]\n", program);
}

static bool parse_on_off(const char *text, bool *result) {
    if (!strcmp(text, "on")) {
        *result = true;
        return true;
    }
    if (!strcmp(text, "off")) {
        *result = false;
        return true;
    }
    return false;
}

static bool parse_memory_limit(const char *text, size_t *result) {
    if (!strcmp(text, "-1")) {
        *result = 0;
        return true;
    }
    if (!text || !*text || text[0] == '-')
        return false;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    size_t multiplier = 1;
    if (*end == 'k' || *end == 'K')
        multiplier = 1024u;
    else if (*end == 'm' || *end == 'M')
        multiplier = 1024u * 1024u;
    else if (*end == 'g' || *end == 'G')
        multiplier = 1024u * 1024u * 1024u;
    else if (*end)
        return false;
    if (*end)
        end++;
    if (*end || !value || value > SIZE_MAX / multiplier)
        return false;
    *result = (size_t)value * multiplier;
    return true;
}

static bool command_is(const char *line, size_t length, const char *command) {
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        length--;
    return strlen(command) == length && !memcmp(line, command, length);
}

static bool terminal_color_enabled(void) {
    const char *no_color = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) && (!no_color || !*no_color) &&
           (!term || strcmp(term, "dumb"));
}

int main(int argc, char **argv) {
    (void)setlocale(LC_CTYPE, "");
    oasp_print_version(stderr);
    const char *root = ".";
    size_t memory_limit = 128u * 1024u * 1024u;
    bool shell_enabled = true;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            root = argv[++i];
            continue;
        }
        if (!strncmp(argv[i], "--memory-limit=", 15) &&
            parse_memory_limit(argv[i] + 15, &memory_limit))
            continue;
        if (!strncmp(argv[i], "--shell-enable=", 15)) {
            if (!parse_on_off(argv[i] + 15, &shell_enabled)) {
                fputs("openasp-cli: --shell-enable must be on or off\n", stderr);
                return 2;
            }
            continue;
        }
        usage(argv[0]);
        return 2;
    }
    if (setenv("OPENASP_SHELL_ENABLE", shell_enabled ? "on" : "off", 1) != 0) {
        fputs("openasp-cli: cannot apply --shell-enable setting\n", stderr);
        return 1;
    }

    oasp_interactive *interactive = oasp_interactive_create(root, memory_limit);
    if (!interactive) {
        fputs("openasp-cli: cannot initialize interactive runtime\n", stderr);
        return 1;
    }
    bool terminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool color = terminal && terminal_color_enabled();
    if (terminal)
        puts("OpenASP interactive CLI. Enter :help for commands.");

    char *line = NULL;
    size_t line_capacity = 0;
    oasp_buffer source;
    if (!oasp_buffer_init(&source, 256)) {
        oasp_interactive_destroy(interactive);
        return 1;
    }
    bool continuation = false;
    int result = 0;
    for (;;) {
        const char *prompt = continuation
            ? (color ? "\x1b[1;38;5;171m...>\x1b[0m " : "...> ")
            : (color ? "\x1b[1;38;5;45mOpenASP>\x1b[0m " : "OpenASP> ");
        ssize_t line_length = terminal ? oasp_line_editor_readline(prompt, &line, &line_capacity)
                                       : getline(&line, &line_capacity, stdin);
        if (line_length < 0)
            break;
        if (!continuation && command_is(line, (size_t)line_length, ":quit"))
            break;
        if (!continuation && command_is(line, (size_t)line_length, ":help")) {
            puts(":help  show commands");
            puts(":quit  exit");
            puts("? expr evaluate and print an expression");
            puts("! command execute a shell command (--shell-enable=off disables)");
            continue;
        }
        if (!continuation && line_length > 0 && line[0] == '!') {
            if (!shell_enabled) {
                fputs("openasp-cli: shell commands are disabled; restart with --shell-enable=on\n",
                      stderr);
                result = 1;
                continue;
            }
            if (oasp_cli_shell_execute(line + 1, (size_t)line_length - 1) != 0)
                result = 1;
            continue;
        }

        bool expression = false;
        const char *input = line;
        size_t input_length = (size_t)line_length;
        if (!continuation && input_length && input[0] == '?') {
            expression = true;
            input++;
            input_length--;
            while (input_length && (*input == ' ' || *input == '\t')) {
                input++;
                input_length--;
            }
            while (input_length && (input[input_length - 1] == '\n' ||
                                    input[input_length - 1] == '\r'))
                input_length--;
        }
        if (!oasp_buffer_append(&source, input, input_length)) {
            fputs("openasp-cli: out of memory\n", stderr);
            result = 1;
            break;
        }

        const char *output = NULL;
        size_t output_length = 0;
        oasp_interactive_result executed =
            oasp_interactive_execute(interactive, source.data, source.length,
                                     expression, &output, &output_length);
        if (executed == OASP_INTERACTIVE_INCOMPLETE) {
            continuation = true;
            continue;
        }
        if (executed == OASP_INTERACTIVE_ERROR)
            result = 1;
        else if (output_length) {
            fwrite(output, 1, output_length, stdout);
            if (output[output_length - 1] != '\n')
                fputc('\n', stdout);
        }
        source.length = 0;
        source.data[0] = '\0';
        continuation = false;
    }
    if (continuation) {
        fputs("openasp-cli: incomplete input at end of file\n", stderr);
        result = 1;
    }
    free(line);
    oasp_buffer_dispose(&source);
    oasp_interactive_destroy(interactive);
    return result;
}
