// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "test_harness.h"

#include <string.h>

void test_count_finalizer(void *context) {
    (*(int *)context)++;
}

int test_append_response(void *context, const void *data, size_t length) {
    test_response_sink *sink = context;
    if (length > sizeof(sink->data) - sink->length)
        return -1;
    memcpy(sink->data + sink->length, data, length);
    sink->length += length;
    sink->calls++;
    return 0;
}
