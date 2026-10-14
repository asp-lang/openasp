// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_TOOL_FILE_H
#define OPENASP_TOOL_FILE_H

#include <stdbool.h>
#include <stddef.h>

bool oasp_tool_read_file(const char *path, unsigned char **data, size_t *size, char *error, size_t error_size);

#endif
