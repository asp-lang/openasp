// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_CLI_SHELL_H
#define OPENASP_CLI_SHELL_H

#include <stddef.h>

/*
 * Executes one shell command with inherited standard streams. Returns the
 * command exit status, 128 plus its terminating signal, or -1 on host failure.
 */
int oasp_cli_shell_execute(const char *command, size_t length);

#endif
