// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_LINE_EDITOR_H
#define OPENASP_LINE_EDITOR_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Reads one editable terminal line and includes the trailing newline in the
 * returned length. The caller owns `*line`; storage is reused across calls.
 */
ssize_t oasp_line_editor_readline(const char *prompt, char **line, size_t *capacity);

#endif
