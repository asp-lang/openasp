// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/preprocessor.h`): Compiler pipeline. Arena-owned tokens, AST
 * nodes, symbols, bytecode, and immutable images move through this stage without per-node
 * frees. Length/count variables bound every indexed operation and malformed input must fail
 * before execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_PREPROCESSOR_H
#define OPENASP_PREPROCESSOR_H

#include "internal.h"

typedef struct {
    char *path;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t status_seconds;
    int64_t status_nanoseconds;
    int64_t file_size;
    uint64_t content_hash;
} oasp_preprocess_dependency;

typedef bool (*oasp_preprocess_module_test)(void *context, const char *path);

typedef struct {
    oasp_buffer output;
    oasp_preprocess_dependency *dependencies;
    size_t dependency_count;
    size_t dependency_capacity;
    char **modules;
    size_t module_count;
    size_t module_capacity;
    const char *error;
    char error_path[4096];
    size_t error_line;
} oasp_preprocess_result;

bool oasp_preprocess_file(const char *root, const char *path, oasp_preprocess_result *result);
bool oasp_preprocess_file_hybrid(const char *root, const char *path, oasp_preprocess_module_test module_test, void *module_context, oasp_preprocess_result *result);
void oasp_preprocess_dispose(oasp_preprocess_result *result);

#endif
