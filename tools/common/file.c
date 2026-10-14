// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "file.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool oasp_tool_read_file(const char *path, unsigned char **data, size_t *size, char *error, size_t error_size) {
    if (!path || !data || !size || !error || !error_size)
        return false;
    *data = NULL;
    *size = 0;
    error[0] = '\0';
    FILE *input = fopen(path, "rb");
    if (!input) {
        snprintf(error, error_size, "cannot open '%s': %s", path, strerror(errno));
        return false;
    }
    struct stat metadata;
    if (fstat(fileno(input), &metadata) || metadata.st_size <= 0 ||
        (uintmax_t)metadata.st_size > SIZE_MAX) {
        snprintf(error, error_size, "'%s' is empty, too large, or unreadable", path);
        fclose(input);
        return false;
    }
    *size = (size_t)metadata.st_size;
    *data = malloc(*size);
    if (!*data) {
        snprintf(error, error_size, "cannot allocate %zu bytes for '%s'", *size, path);
        fclose(input);
        return false;
    }
    size_t consumed = fread(*data, 1, *size, input);
    if (consumed != *size || ferror(input)) {
        snprintf(error, error_size, "cannot read '%s': %s", path,
                 ferror(input) ? strerror(errno) : "short read");
        free(*data);
        *data = NULL;
        *size = 0;
        fclose(input);
        return false;
    }
    if (fclose(input)) {
        snprintf(error, error_size, "cannot close '%s': %s", path, strerror(errno));
        free(*data);
        *data = NULL;
        *size = 0;
        return false;
    }
    return true;
}
