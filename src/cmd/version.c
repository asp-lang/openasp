// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "version.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int oasp_version_descriptor(FILE *stream) {
    return stream ? fileno(stream) : -1;
}

static bool oasp_version_color_enabled(FILE *stream) {
    const char *no_color = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    int descriptor = oasp_version_descriptor(stream);
    return descriptor >= 0 && isatty(descriptor) && (!no_color || !*no_color) &&
           (!term || strcmp(term, "dumb"));
}

typedef struct {
    size_t columns;
    size_t rows;
} oasp_terminal_size;

static oasp_terminal_size oasp_terminal_dimensions(FILE *stream) {
    struct winsize size = {0};
    int descriptor = oasp_version_descriptor(stream);
    if (descriptor >= 0 && ioctl(descriptor, TIOCGWINSZ, &size) == 0)
        return (oasp_terminal_size){size.ws_col ? size.ws_col : 80, size.ws_row ? size.ws_row : 24};
    return (oasp_terminal_size){80, 24};
}

static void oasp_print_padding(FILE *stream, size_t terminal_columns, size_t content_columns) {
    size_t padding = terminal_columns > content_columns ? (terminal_columns - content_columns) / 2 : 0;
    for (size_t i = 0; i < padding; i++)
        fputc(' ', stream);
}

static bool oasp_print_centered(FILE *stream, size_t terminal_columns, const char *style, const char *text, size_t text_columns) {
    if (!text || !text_columns || text_columns >= terminal_columns)
        return false;
    oasp_print_padding(stream, terminal_columns, text_columns);
    fprintf(stream, "%s%s\x1b[0m\n", style, text);
    return true;
}

static void oasp_print_terminal_banner(FILE *stream) {
    static const char *const logo[] = {
        " ██████╗ ██████╗ ███████╗███╗   ██╗ █████╗ ███████╗██████╗ ",
        "██╔═══██╗██╔══██╗██╔════╝████╗  ██║██╔══██╗██╔════╝██╔══██╗",
        "██║   ██║██████╔╝█████╗  ██╔██╗ ██║███████║███████╗██████╔╝",
        "██║   ██║██╔═══╝ ██╔══╝  ██║╚██╗██║██╔══██║╚════██║██╔═══╝ ",
        "╚██████╔╝██║     ███████╗██║ ╚████║██║  ██║███████║██║     ",
        " ╚═════╝ ╚═╝     ╚══════╝╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝╚═╝     ",
    };
    static const char *const colors[] = {
        "\x1b[38;5;45m",
        "\x1b[38;5;39m",
        "\x1b[38;5;69m",
        "\x1b[38;5;99m",
        "\x1b[38;5;135m",
        "\x1b[38;5;171m",
    };
    oasp_terminal_size terminal = oasp_terminal_dimensions(stream);
    if (terminal.columns > 59 && terminal.rows > 8) {
        for (size_t i = 0; i < sizeof(logo) / sizeof(logo[0]); i++) {
            oasp_print_padding(stream, terminal.columns, 59);
            fprintf(stream, "%s%s\x1b[0m\n", colors[i], logo[i]);
        }
    } else if (terminal.columns > 7) {
        oasp_print_padding(stream, terminal.columns, 7);
        fputs("\x1b[1;38;5;45mOPEN\x1b[38;5;171mASP\x1b[0m\n", stream);
    } else if (terminal.columns > 1) {
        oasp_print_centered(stream, terminal.columns, "\x1b[1;38;5;45m", "O", 1);
    }

    if (terminal.rows > 1) {
        char version[160];
        int version_length = snprintf(version, sizeof(version), "OpenASP %s  %s  Built %s",
                                      OPENASP_VERSION, OPENASP_PLATFORM, OPENASP_BUILD_DATE);
        if (version_length <= 0 || (size_t)version_length >= sizeof(version) ||
            !oasp_print_centered(stream, terminal.columns, "\x1b[38;5;250m",
                                 version, (size_t)version_length)) {
            static const char compact_version[] = "OpenASP " OPENASP_VERSION;
            if (!oasp_print_centered(stream, terminal.columns, "\x1b[38;5;250m",
                                     compact_version, sizeof(compact_version) - 1))
                (void)oasp_print_centered(stream, terminal.columns, "\x1b[38;5;250m", "OpenASP", 7);
        }
    }

    if (terminal.rows > 2) {
        static const char full_copyright[] = " " OPENASP_COPYRIGHT " ";
        static const char compact_copyright[] = " 2026 asp-lang.org ";
        static const char domain_copyright[] = " asp-lang.org ";
        if (!oasp_print_centered(stream, terminal.columns, "\x1b[97;44m",
                                 full_copyright, sizeof(full_copyright) - 1) &&
            !oasp_print_centered(stream, terminal.columns, "\x1b[97;44m",
                                 compact_copyright, sizeof(compact_copyright) - 1) &&
            !oasp_print_centered(stream, terminal.columns, "\x1b[97;44m",
                                 domain_copyright, sizeof(domain_copyright) - 1))
            (void)oasp_print_centered(stream, terminal.columns, "\x1b[97;44m", " ASP ", 5);
    }
}

void oasp_print_version(FILE *stream) {
    if (!stream)
        return;
    if (oasp_version_color_enabled(stream)) {
        oasp_print_terminal_banner(stream);
        return;
    }
    fprintf(stream, "OpenASP %s %s Built %s %s\n", OPENASP_VERSION, OPENASP_PLATFORM,
            OPENASP_BUILD_DATE, OPENASP_COPYRIGHT);
}
