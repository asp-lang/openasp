// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/module_image.h`): Compiler pipeline. Arena-owned tokens, AST
 * nodes, symbols, bytecode, and immutable images move through this stage without per-node
 * frees. Length/count variables bound every indexed operation and malformed input must fail
 * before execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_MODULE_IMAGE_H
#define OPENASP_MODULE_IMAGE_H

#include "bytecode.h"
#include "preprocessor.h"

typedef struct {
    oasp_slice path;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t status_seconds;
    int64_t status_nanoseconds;
    int64_t file_size;
    uint64_t content_hash;
} oasp_module_image_dependency;

typedef struct {
    const char *path;
    const oasp_preprocess_dependency *dependencies;
    size_t dependency_count;
    char *const *modules;
    size_t module_count;
    /* NULL omits debug data; a non-NULL slice is copied into the image. */
    const oasp_slice *debug_source;
} oasp_module_image_metadata;

typedef struct {
    oasp_bytecode bytecode;
    const unsigned char *image_base;
    const void *encoded_dependencies;
    size_t dependency_count;
    const void *encoded_modules;
    size_t module_count;
    oasp_slice debug_source;
    bool has_debug_info;
} oasp_module_image;

bool oasp_module_image_encode(const oasp_bytecode *bytecode, const oasp_module_image_metadata *metadata, oasp_buffer *output);
/* The encoded data must remain alive while the decoded bytecode is in use. */
bool oasp_module_image_decode(oasp_arena *arena, const void *data, size_t size, oasp_module_image *image);
bool oasp_module_image_valid(const void *data, size_t size, const char *expected_path);
/* Dependency check for an immutable image that already passed oasp_module_image_valid(). */
bool oasp_module_image_fresh(const void *data, size_t size, const char *expected_path);
bool oasp_module_image_path(const oasp_module_image *image, oasp_slice *result);
bool oasp_module_image_dependency_at(const oasp_module_image *image, size_t index, oasp_module_image_dependency *result);
bool oasp_module_image_module_at(const oasp_module_image *image, size_t index, oasp_slice *result);
bool oasp_module_image_debug_source(const oasp_module_image *image, oasp_slice *result);
void oasp_module_image_dispose(oasp_module_image *image);

#endif
