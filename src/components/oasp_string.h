// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_OASP_STRING_COMPAT_H
#define OPENASP_OASP_STRING_COMPAT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t length;
    char data[];
} oasp_string_t;

static inline const char *oasp_string_cstr(const oasp_string_t *value) {
    return value ? value->data : "";
}

static inline int64_t oasp_string_len(const oasp_string_t *value) {
    return value ? value->length : 0;
}

static inline oasp_string_t *oasp_string_from_bytes(const void *data, int64_t length) {
    if (length < 0 || (uint64_t)length > SIZE_MAX - sizeof(oasp_string_t) - 1)
        return NULL;
    oasp_string_t *value = (oasp_string_t *)malloc(sizeof(*value) + (size_t)length + 1);
    if (!value)
        return NULL;
    value->length = length;
    if (length)
        memcpy(value->data, data, (size_t)length);
    value->data[length] = '\0';
    return value;
}

static inline oasp_string_t *oasp_string_from_cstr(const char *text) {
    return oasp_string_from_bytes(text ? text : "", (int64_t)strlen(text ? text : ""));
}

#endif
