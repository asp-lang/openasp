// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_TEST_HARNESS_H
#define OPENASP_TEST_HARNESS_H

#include <stddef.h>

typedef struct {
    char data[16384];
    size_t length;
    size_t calls;
} test_response_sink;

void test_count_finalizer(void *context);
int test_append_response(void *context, const void *data, size_t length);

#define count_finalizer test_count_finalizer
#define append_response test_append_response

#endif
