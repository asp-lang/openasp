// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/module_image.c`): Compiler pipeline. Arena-owned tokens, AST
 * nodes, symbols, bytecode, and immutable images move through this stage without per-node
 * frees. Length/count variables bound every indexed operation and malformed input must fail
 * before execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700

#include "module_image.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define OASP_IMAGE_MAGIC UINT64_C(0x4f4153504d4f4432)
#define OASP_IMAGE_ABI 23u
#define OASP_IMAGE_FLAG_ERROR_AWARE UINT64_C(0x01)
#define OASP_IMAGE_FLAG_READS_SESSION UINT64_C(0x02)
#define OASP_IMAGE_FLAG_WRITES_SESSION UINT64_C(0x04)
#define OASP_IMAGE_FLAG_READS_APPLICATION UINT64_C(0x08)
#define OASP_IMAGE_FLAG_WRITES_APPLICATION UINT64_C(0x10)
#define OASP_IMAGE_FLAG_DEBUG_INFO UINT64_C(0x20)

typedef struct {
    uint32_t offset;
    uint32_t length;
} oasp_image_slice;

typedef struct {
    oasp_image_slice path;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t status_seconds;
    int64_t status_nanoseconds;
    int64_t file_size;
    uint64_t content_hash;
} oasp_image_dependency;

typedef struct {
    uint8_t kind;
    uint8_t reserved[7];
    uint64_t payload;
    oasp_image_slice text;
} oasp_image_constant;

typedef struct {
    oasp_image_slice name;
    uint32_t slot_offset;
    uint32_t slot_count;
    uint32_t parameter_offset;
    uint32_t parameter_count;
    uint32_t entry;
    uint32_t limit;
    uint32_t class_index;
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
} oasp_image_routine;

typedef struct {
    oasp_image_slice name;
    uint8_t flags;
    uint8_t reserved[7];
} oasp_image_parameter;

typedef struct {
    oasp_image_slice name;
    uint32_t entry;
    uint32_t limit;
    uint32_t routine_offset;
    uint32_t routine_count;
    uint32_t flags;
} oasp_image_class;

typedef struct {
    uint64_t magic;
    uint32_t abi;
    uint32_t header_size;
    uint64_t total_size;
    uint64_t checksum;
    oasp_image_slice path;
    oasp_image_slice debug_source;
    uint32_t dependency_offset;
    uint32_t dependency_count;
    uint32_t module_offset;
    uint32_t module_count;
    uint32_t instruction_offset;
    uint32_t instruction_count;
    uint32_t name_offset;
    uint32_t name_count;
    uint32_t constant_offset;
    uint32_t constant_count;
    uint32_t global_slot_offset;
    uint32_t global_slot_count;
    uint32_t routine_offset;
    uint32_t routine_count;
    uint32_t parameter_offset;
    uint32_t parameter_count;
    uint32_t routine_slot_offset;
    uint32_t routine_slot_count;
    uint32_t class_offset;
    uint32_t class_count;
    uint64_t loop_count;
    uint64_t max_stack;
    uint64_t flags;
    uint64_t opcode_counts[OASP_BC_COUNT];
} oasp_image_header;

static bool oasp_u32(size_t value, uint32_t *result) {
    if (value > UINT32_MAX)
        return false;
    *result = (uint32_t)value;
    return true;
}

static bool oasp_file_content_hash(const char *path, uint64_t *result) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned char buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        for (size_t i = 0; i < count; i++) {
            hash ^= buffer[i];
            hash *= UINT64_C(1099511628211);
        }
    bool ok = !ferror(file) && fclose(file) == 0;
    if (ok)
        *result = hash;
    return ok;
}

static bool oasp_range(size_t size, uint32_t offset, size_t count, size_t width) {
    return offset <= size && count <= SIZE_MAX / width && count * width <= size - offset;
}

static bool oasp_aligned(uint32_t offset, size_t alignment) { return offset % alignment == 0; }

static uint64_t oasp_hash(uint64_t hash, const void *data, size_t length) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t oasp_checksum(const void *data, size_t size) {
    size_t offset = offsetof(oasp_image_header, checksum);
    uint64_t zero = 0;
    uint64_t hash = oasp_hash(UINT64_C(1469598103934665603), data, offset);
    hash = oasp_hash(hash, &zero, sizeof(zero));
    return oasp_hash(hash, (const unsigned char *)data + offset + sizeof(zero), size - offset - sizeof(zero));
}

static bool oasp_align(oasp_buffer *output) {
    static const unsigned char zeros[8] = {0};
    size_t padding = (8 - output->length % 8) % 8;
    return !padding || oasp_buffer_append(output, zeros, padding);
}

static bool oasp_append_slice(oasp_buffer *output, const char *data, size_t length, oasp_image_slice *slice) {
    return oasp_u32(output->length, &slice->offset) && oasp_u32(length, &slice->length) &&
           (!length || oasp_buffer_append(output, data, length));
}

static bool oasp_append_section(oasp_buffer *output, const void *items, size_t count, size_t width, uint32_t *offset) {
    return oasp_align(output) && oasp_u32(output->length, offset) && count <= SIZE_MAX / width &&
           (!count || oasp_buffer_append(output, items, count * width));
}

static bool oasp_encode_value(oasp_buffer *output, const oasp_value *value, oasp_image_constant *encoded) {
    memset(encoded, 0, sizeof(*encoded));
    encoded->kind = (uint8_t)value->kind;
    switch (value->kind) {
    case OASP_VALUE_BOOL:
        encoded->payload = value->as.boolean ? 1 : 0;
        return true;
    case OASP_VALUE_INT:
        memcpy(&encoded->payload, &value->as.integer, sizeof(value->as.integer));
        return true;
    case OASP_VALUE_FLOAT:
        memcpy(&encoded->payload, &value->as.floating, sizeof(value->as.floating));
        return true;
    case OASP_VALUE_STRING:
        return oasp_append_slice(output, value->as.string.data, value->as.string.length, &encoded->text);
    case OASP_VALUE_DATE:
        return oasp_append_slice(output, value->as.date.data, value->as.date.length, &encoded->text);
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    default:
        return false;
    }
}

bool oasp_module_image_encode(const oasp_bytecode *bytecode, const oasp_module_image_metadata *metadata, oasp_buffer *output) {
    if (!bytecode || !bytecode->instructions || !metadata || !metadata->path || !output ||
        !oasp_buffer_init(output, 4096))
        return false;
    const char *path = metadata->path;
    const oasp_preprocess_dependency *dependencies = metadata->dependencies;
    size_t dependency_count = metadata->dependency_count;
    char *const *modules = metadata->modules;
    size_t module_count = metadata->module_count;
    oasp_image_header header;
    memset(&header, 0, sizeof(header));
    if (!oasp_buffer_append(output, &header, sizeof(header)) ||
        !oasp_append_slice(output, path, strlen(path), &header.path) ||
        (metadata->debug_source &&
         !oasp_append_slice(output, metadata->debug_source->data, metadata->debug_source->length,
                            &header.debug_source)))
        goto fail;

    size_t routine_slot_count = 0;
    size_t parameter_count = 0;
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        if (routine_slot_count > SIZE_MAX - bytecode->routines[i].slot_count ||
            parameter_count > SIZE_MAX - bytecode->routines[i].parameter_count)
            goto fail;
        routine_slot_count += bytecode->routines[i].slot_count;
        parameter_count += bytecode->routines[i].parameter_count;
    }
    oasp_image_dependency *disk_dependencies = dependency_count ? calloc(dependency_count, sizeof(*disk_dependencies)) : NULL;
    oasp_image_slice *disk_modules = module_count ? calloc(module_count, sizeof(*disk_modules)) : NULL;
    oasp_image_slice *disk_names = bytecode->name_count ? calloc(bytecode->name_count, sizeof(*disk_names)) : NULL;
    oasp_image_constant *disk_constants = bytecode->constant_count ? calloc(bytecode->constant_count, sizeof(*disk_constants)) : NULL;
    oasp_image_slice *disk_globals = bytecode->global_slot_count ? calloc(bytecode->global_slot_count, sizeof(*disk_globals)) : NULL;
    oasp_image_routine *disk_routines = bytecode->routine_count ? calloc(bytecode->routine_count, sizeof(*disk_routines)) : NULL;
    oasp_image_parameter *disk_parameters = parameter_count ? calloc(parameter_count, sizeof(*disk_parameters)) : NULL;
    oasp_image_slice *disk_slots = routine_slot_count ? calloc(routine_slot_count, sizeof(*disk_slots)) : NULL;
    oasp_image_class *disk_classes = bytecode->class_count ? calloc(bytecode->class_count, sizeof(*disk_classes)) : NULL;
    if ((dependency_count && !disk_dependencies) || (module_count && !disk_modules) || (bytecode->name_count && !disk_names) ||
        (bytecode->constant_count && !disk_constants) || (bytecode->global_slot_count && !disk_globals) ||
        (bytecode->routine_count && !disk_routines) || (parameter_count && !disk_parameters) ||
        (routine_slot_count && !disk_slots) || (bytecode->class_count && !disk_classes))
        goto fail_arrays;

    for (size_t i = 0; i < dependency_count; i++) {
        if (!oasp_append_slice(output, dependencies[i].path, strlen(dependencies[i].path), &disk_dependencies[i].path))
            goto fail_arrays;
        disk_dependencies[i].modified_seconds = dependencies[i].modified_seconds;
        disk_dependencies[i].modified_nanoseconds = dependencies[i].modified_nanoseconds;
        disk_dependencies[i].status_seconds = dependencies[i].status_seconds;
        disk_dependencies[i].status_nanoseconds = dependencies[i].status_nanoseconds;
        disk_dependencies[i].file_size = dependencies[i].file_size;
        disk_dependencies[i].content_hash = dependencies[i].content_hash;
    }
    for (size_t i = 0; i < module_count; i++)
        if (!oasp_append_slice(output, modules[i], strlen(modules[i]), &disk_modules[i]))
            goto fail_arrays;
    for (size_t i = 0; i < bytecode->name_count; i++)
        if (!oasp_append_slice(output, bytecode->names[i].data, bytecode->names[i].length, &disk_names[i]))
            goto fail_arrays;
    for (size_t i = 0; i < bytecode->constant_count; i++)
        if (!oasp_encode_value(output, &bytecode->constants[i], &disk_constants[i]))
            goto fail_arrays;
    for (size_t i = 0; i < bytecode->global_slot_count; i++)
        if (!oasp_append_slice(output, bytecode->global_slot_names[i].data, bytecode->global_slot_names[i].length, &disk_globals[i]))
            goto fail_arrays;

    size_t slot_cursor = 0;
    size_t parameter_cursor = 0;
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        const oasp_routine_bytecode *source = &bytecode->routines[i];
        oasp_image_routine *destination = &disk_routines[i];
        if (!oasp_append_slice(output, source->name.data, source->name.length, &destination->name) ||
            !oasp_u32(slot_cursor, &destination->slot_offset) || !oasp_u32(source->slot_count, &destination->slot_count) ||
            !oasp_u32(parameter_cursor, &destination->parameter_offset) ||
            !oasp_u32(source->parameter_count, &destination->parameter_count) || !oasp_u32(source->entry, &destination->entry) ||
            !oasp_u32(source->limit, &destination->limit) ||
            !oasp_u32(source->class_index == SIZE_MAX ? UINT32_MAX : source->class_index, &destination->class_index))
            goto fail_arrays;
        destination->kind = (uint8_t)source->kind;
        destination->flags = (source->is_default ? 1u : 0u) |
                             (source->error_aware ? 2u : 0u);
        for (size_t j = 0; j < source->slot_count; j++)
            if (!oasp_append_slice(output, source->slot_names[j].data, source->slot_names[j].length, &disk_slots[slot_cursor++]))
                goto fail_arrays;
        for (size_t j = 0; j < source->parameter_count; j++) {
            oasp_image_parameter *parameter = &disk_parameters[parameter_cursor++];
            if (!oasp_append_slice(output, source->parameters[j].name.data, source->parameters[j].name.length, &parameter->name))
                goto fail_arrays;
            parameter->flags = (source->parameters[j].by_value ? 1u : 0u) | (source->parameters[j].array ? 2u : 0u);
        }
    }
    for (size_t i = 0; i < bytecode->class_count; i++) {
        const oasp_class_bytecode *source = &bytecode->classes[i];
        oasp_image_class *destination = &disk_classes[i];
        if (!oasp_append_slice(output, source->name.data, source->name.length, &destination->name) ||
            !oasp_u32(source->entry, &destination->entry) || !oasp_u32(source->limit, &destination->limit) ||
            !oasp_u32(source->routine_offset, &destination->routine_offset) ||
            !oasp_u32(source->routine_count, &destination->routine_count))
            goto fail_arrays;
        destination->flags = source->error_aware ? 1u : 0u;
    }

    if (!oasp_u32(dependency_count, &header.dependency_count) || !oasp_u32(module_count, &header.module_count) ||
        !oasp_u32(bytecode->length, &header.instruction_count) || !oasp_u32(bytecode->name_count, &header.name_count) ||
        !oasp_u32(bytecode->constant_count, &header.constant_count) ||
        !oasp_u32(bytecode->global_slot_count, &header.global_slot_count) ||
        !oasp_u32(bytecode->routine_count, &header.routine_count) || !oasp_u32(parameter_count, &header.parameter_count) ||
        !oasp_u32(routine_slot_count, &header.routine_slot_count) || !oasp_u32(bytecode->class_count, &header.class_count) ||
        !oasp_append_section(output, disk_dependencies, dependency_count, sizeof(*disk_dependencies), &header.dependency_offset) ||
        !oasp_append_section(output, disk_modules, module_count, sizeof(*disk_modules), &header.module_offset) ||
        !oasp_append_section(output, bytecode->instructions, bytecode->length, sizeof(*bytecode->instructions), &header.instruction_offset) ||
        !oasp_append_section(output, disk_names, bytecode->name_count, sizeof(*disk_names), &header.name_offset) ||
        !oasp_append_section(output, disk_constants, bytecode->constant_count, sizeof(*disk_constants), &header.constant_offset) ||
        !oasp_append_section(output, disk_globals, bytecode->global_slot_count, sizeof(*disk_globals), &header.global_slot_offset) ||
        !oasp_append_section(output, disk_routines, bytecode->routine_count, sizeof(*disk_routines), &header.routine_offset) ||
        !oasp_append_section(output, disk_parameters, parameter_count, sizeof(*disk_parameters), &header.parameter_offset) ||
        !oasp_append_section(output, disk_slots, routine_slot_count, sizeof(*disk_slots), &header.routine_slot_offset) ||
        !oasp_append_section(output, disk_classes, bytecode->class_count, sizeof(*disk_classes), &header.class_offset))
        goto fail_arrays;
    header.magic = OASP_IMAGE_MAGIC;
    header.abi = OASP_IMAGE_ABI;
    header.header_size = sizeof(header);
    header.total_size = output->length;
    header.loop_count = bytecode->loop_count;
    header.max_stack = bytecode->max_stack;
    header.flags = (bytecode->root_error_aware ? OASP_IMAGE_FLAG_ERROR_AWARE : 0) |
                   ((bytecode->state_effects & OASP_STATE_READS_SESSION) ? OASP_IMAGE_FLAG_READS_SESSION : 0) |
                   ((bytecode->state_effects & OASP_STATE_WRITES_SESSION) ? OASP_IMAGE_FLAG_WRITES_SESSION : 0) |
                   ((bytecode->state_effects & OASP_STATE_READS_APPLICATION) ? OASP_IMAGE_FLAG_READS_APPLICATION : 0) |
                   ((bytecode->state_effects & OASP_STATE_WRITES_APPLICATION) ? OASP_IMAGE_FLAG_WRITES_APPLICATION : 0) |
                   (metadata->debug_source ? OASP_IMAGE_FLAG_DEBUG_INFO : 0);
    memcpy(header.opcode_counts, bytecode->opcode_counts, sizeof(header.opcode_counts));
    memcpy(output->data, &header, sizeof(header));
    ((oasp_image_header *)output->data)->checksum = oasp_checksum(output->data, output->length);
    free(disk_dependencies);
    free(disk_modules);
    free(disk_names);
    free(disk_constants);
    free(disk_globals);
    free(disk_routines);
    free(disk_parameters);
    free(disk_slots);
    free(disk_classes);
    return true;

fail_arrays:
    free(disk_dependencies);
    free(disk_modules);
    free(disk_names);
    free(disk_constants);
    free(disk_globals);
    free(disk_routines);
    free(disk_parameters);
    free(disk_slots);
    free(disk_classes);
fail:
    oasp_buffer_dispose(output);
    return false;
}

static const oasp_image_header *oasp_header(const void *data, size_t size) {
    if (!data || (uintptr_t)data % _Alignof(oasp_image_header) != 0 || size < sizeof(oasp_image_header))
        return NULL;
    const oasp_image_header *header = data;
    if (header->magic != OASP_IMAGE_MAGIC || header->abi != OASP_IMAGE_ABI || header->header_size != sizeof(*header) ||
        header->total_size != size || header->checksum != oasp_checksum(data, size) ||
        !oasp_aligned(header->dependency_offset, _Alignof(oasp_image_dependency)) ||
        !oasp_aligned(header->module_offset, _Alignof(oasp_image_slice)) ||
        !oasp_aligned(header->instruction_offset, _Alignof(oasp_instruction)) ||
        !oasp_aligned(header->name_offset, _Alignof(oasp_image_slice)) ||
        !oasp_aligned(header->constant_offset, _Alignof(oasp_image_constant)) ||
        !oasp_aligned(header->global_slot_offset, _Alignof(oasp_image_slice)) ||
        !oasp_aligned(header->routine_offset, _Alignof(oasp_image_routine)) ||
        !oasp_aligned(header->parameter_offset, _Alignof(oasp_image_parameter)) ||
        !oasp_aligned(header->routine_slot_offset, _Alignof(oasp_image_slice)) ||
        !oasp_aligned(header->class_offset, _Alignof(oasp_image_class)) ||
        !oasp_range(size, header->path.offset, header->path.length, 1) ||
        !oasp_range(size, header->debug_source.offset, header->debug_source.length, 1) ||
        !oasp_range(size, header->dependency_offset, header->dependency_count, sizeof(oasp_image_dependency)) ||
        !oasp_range(size, header->module_offset, header->module_count, sizeof(oasp_image_slice)) ||
        !oasp_range(size, header->instruction_offset, header->instruction_count, sizeof(oasp_instruction)) ||
        !oasp_range(size, header->name_offset, header->name_count, sizeof(oasp_image_slice)) ||
        !oasp_range(size, header->constant_offset, header->constant_count, sizeof(oasp_image_constant)) ||
        !oasp_range(size, header->global_slot_offset, header->global_slot_count, sizeof(oasp_image_slice)) ||
        !oasp_range(size, header->routine_offset, header->routine_count, sizeof(oasp_image_routine)) ||
        !oasp_range(size, header->parameter_offset, header->parameter_count, sizeof(oasp_image_parameter)) ||
        !oasp_range(size, header->routine_slot_offset, header->routine_slot_count, sizeof(oasp_image_slice)) ||
        !oasp_range(size, header->class_offset, header->class_count, sizeof(oasp_image_class)))
        return NULL;
    return header;
}

static bool oasp_slice_valid(const oasp_image_header *header, oasp_image_slice slice) {
    return oasp_range((size_t)header->total_size, slice.offset, slice.length, 1);
}

static bool oasp_image_fresh(const oasp_image_header *header, const char *bytes, const char *expected_path) {
    if (!header || !bytes || !expected_path)
        return false;
    size_t path_length = strlen(expected_path);
    if (header->path.length != path_length || memcmp(bytes + header->path.offset, expected_path, path_length))
        return false;
    const oasp_image_dependency *dependencies = (const void *)(bytes + header->dependency_offset);
    for (size_t i = 0; i < header->dependency_count; i++) {
        if (!oasp_slice_valid(header, dependencies[i].path) || dependencies[i].path.length >= 4096)
            return false;
        char path[4096];
        memcpy(path, bytes + dependencies[i].path.offset, dependencies[i].path.length);
        path[dependencies[i].path.length] = '\0';
        struct stat metadata;
        if (stat(path, &metadata))
            return false;
#if defined(__APPLE__)
        int64_t nanoseconds = (int64_t)metadata.st_mtimespec.tv_nsec;
#else
        int64_t nanoseconds = (int64_t)metadata.st_mtim.tv_nsec;
#endif
        int64_t status_nanoseconds;
#if defined(__APPLE__)
        status_nanoseconds = (int64_t)metadata.st_ctimespec.tv_nsec;
#else
        status_nanoseconds = (int64_t)metadata.st_ctim.tv_nsec;
#endif
        if ((int64_t)metadata.st_mtime != dependencies[i].modified_seconds ||
            nanoseconds != dependencies[i].modified_nanoseconds || (int64_t)metadata.st_size != dependencies[i].file_size)
            return false;
        if ((int64_t)metadata.st_ctime != dependencies[i].status_seconds ||
            status_nanoseconds != dependencies[i].status_nanoseconds) {
            uint64_t content_hash;
            if (oasp_file_content_hash(path, &content_hash) && content_hash != dependencies[i].content_hash)
                return false;
        }
    }
    return true;
}

bool oasp_module_image_valid(const void *data, size_t size, const char *expected_path) {
    const oasp_image_header *header = oasp_header(data, size);
    return header && oasp_image_fresh(header, data, expected_path);
}

bool oasp_module_image_fresh(const void *data, size_t size, const char *expected_path) {
    if (!data || !expected_path || (uintptr_t)data % _Alignof(oasp_image_header) != 0 || size < sizeof(oasp_image_header))
        return false;
    const oasp_image_header *header = data;
    if (header->magic != OASP_IMAGE_MAGIC || header->abi != OASP_IMAGE_ABI || header->header_size != sizeof(*header) ||
        header->total_size != size || !oasp_range(size, header->path.offset, header->path.length, 1) ||
        !oasp_range(size, header->dependency_offset, header->dependency_count, sizeof(oasp_image_dependency)))
        return false;
    return oasp_image_fresh(header, data, expected_path);
}

static bool oasp_view_slice(const char *bytes, const oasp_image_header *header, oasp_image_slice encoded, oasp_slice *slice) {
    if (!oasp_slice_valid(header, encoded))
        return false;
    slice->data = bytes + encoded.offset;
    slice->length = encoded.length;
    return true;
}

static bool oasp_decode_constant(const char *bytes, const oasp_image_header *header, const oasp_image_constant *encoded, oasp_value *value) {
    memset(value, 0, sizeof(*value));
    if (encoded->kind > OASP_VALUE_DATE)
        return false;
    value->kind = (oasp_value_kind)encoded->kind;
    switch (value->kind) {
    case OASP_VALUE_BOOL:
        value->as.boolean = encoded->payload != 0;
        return true;
    case OASP_VALUE_INT:
        memcpy(&value->as.integer, &encoded->payload, sizeof(value->as.integer));
        return true;
    case OASP_VALUE_FLOAT:
        memcpy(&value->as.floating, &encoded->payload, sizeof(value->as.floating));
        return true;
    case OASP_VALUE_STRING:
        return oasp_view_slice(bytes, header, encoded->text, &value->as.string);
    case OASP_VALUE_DATE:
        return oasp_view_slice(bytes, header, encoded->text, &value->as.date);
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    default:
        return false;
    }
}

static const oasp_image_header *oasp_bytecode_header(const oasp_bytecode *bytecode) {
    if (!bytecode || !bytecode->image_base || bytecode->image_size < sizeof(oasp_image_header))
        return NULL;
    const oasp_image_header *header = (const void *)bytecode->image_base;
    return header->total_size == bytecode->image_size ? header : NULL;
}

bool oasp_bytecode_name_at(const oasp_bytecode *bytecode, size_t index, oasp_slice *result) {
    if (!bytecode || !result || index >= bytecode->name_count)
        return false;
    if (!bytecode->image_base) {
        *result = bytecode->names[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_slice *names = bytecode->image_names;
    return header && oasp_view_slice((const char *)bytecode->image_base, header, names[index], result);
}

bool oasp_bytecode_constant_at(const oasp_bytecode *bytecode, size_t index, oasp_value *result) {
    if (!bytecode || !result || index >= bytecode->constant_count)
        return false;
    if (!bytecode->image_base) {
        *result = bytecode->constants[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_constant *constants = bytecode->image_constants;
    return header && oasp_decode_constant((const char *)bytecode->image_base, header, &constants[index], result);
}

bool oasp_bytecode_global_slot_at(const oasp_bytecode *bytecode, size_t index, oasp_slice *result) {
    if (!bytecode || !result || index >= bytecode->global_slot_count)
        return false;
    if (!bytecode->image_base) {
        *result = bytecode->global_slot_names[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_slice *slots = bytecode->image_global_slots;
    return header && oasp_view_slice((const char *)bytecode->image_base, header, slots[index], result);
}

bool oasp_bytecode_routine_name_at(const oasp_bytecode *bytecode, size_t index, oasp_slice *result) {
    if (!bytecode || !result || index >= bytecode->routine_count)
        return false;
    if (!bytecode->image_base) {
        *result = bytecode->routines[index].name;
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_routine *routines = bytecode->image_routines;
    return header && oasp_view_slice((const char *)bytecode->image_base, header, routines[index].name, result);
}

bool oasp_bytecode_routine_at(const oasp_bytecode *bytecode, size_t index, oasp_routine_bytecode *result) {
    if (!bytecode || !result || index >= bytecode->routine_count)
        return false;
    if (!bytecode->image_base) {
        *result = bytecode->routines[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_routine *routines = bytecode->image_routines;
    if (!header)
        return false;
    const oasp_image_routine *source = &routines[index];
    memset(result, 0, sizeof(*result));
    if (!oasp_view_slice((const char *)bytecode->image_base, header, source->name, &result->name) ||
        source->slot_offset > header->routine_slot_count ||
        source->slot_count > header->routine_slot_count - source->slot_offset ||
        source->parameter_offset > header->parameter_count ||
        source->parameter_count > header->parameter_count - source->parameter_offset ||
        source->entry >= source->limit || source->limit > header->instruction_count ||
        source->kind > OASP_ROUTINE_PROPERTY_SET ||
        (source->class_index != UINT32_MAX && source->class_index >= header->class_count))
        return false;
    result->slot_count = source->slot_count;
    result->slot_offset = source->slot_offset;
    result->parameter_count = source->parameter_count;
    result->parameter_offset = source->parameter_offset;
    result->entry = source->entry;
    result->limit = source->limit;
    result->class_index = source->class_index == UINT32_MAX ? SIZE_MAX : source->class_index;
    result->kind = (oasp_routine_kind)source->kind;
    result->is_default = (source->flags & 1u) != 0;
    result->error_aware = (source->flags & 2u) != 0;
    return true;
}

bool oasp_bytecode_routine_parameter(const oasp_bytecode *bytecode, const oasp_routine_bytecode *routine, size_t index, oasp_parameter_bytecode *result) {
    if (!bytecode || !routine || !result || index >= routine->parameter_count)
        return false;
    if (!bytecode->image_base) {
        *result = routine->parameters[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_parameter *parameters = bytecode->image_parameters;
    if (!header || routine->parameter_offset > header->parameter_count ||
        routine->parameter_count > header->parameter_count - routine->parameter_offset)
        return false;
    const oasp_image_parameter *source = &parameters[routine->parameter_offset + index];
    memset(result, 0, sizeof(*result));
    if (!oasp_view_slice((const char *)bytecode->image_base, header, source->name, &result->name))
        return false;
    result->by_value = (source->flags & 1u) != 0;
    result->array = (source->flags & 2u) != 0;
    return true;
}

bool oasp_bytecode_routine_parameter_at(const oasp_bytecode *bytecode, size_t routine_index, size_t index, oasp_parameter_bytecode *result) {
    oasp_routine_bytecode routine;
    return oasp_bytecode_routine_at(bytecode, routine_index, &routine) &&
           oasp_bytecode_routine_parameter(bytecode, &routine, index, result);
}

bool oasp_bytecode_routine_slot(const oasp_bytecode *bytecode, const oasp_routine_bytecode *routine, size_t index, oasp_slice *result) {
    if (!bytecode || !routine || !result || index >= routine->slot_count)
        return false;
    if (!bytecode->image_base) {
        *result = routine->slot_names[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_slice *slots = bytecode->image_routine_slots;
    return header && routine->slot_offset <= header->routine_slot_count &&
           routine->slot_count <= header->routine_slot_count - routine->slot_offset &&
           oasp_view_slice((const char *)bytecode->image_base, header, slots[routine->slot_offset + index], result);
}

bool oasp_bytecode_routine_slot_at(const oasp_bytecode *bytecode, size_t routine_index, size_t index, oasp_slice *result) {
    oasp_routine_bytecode routine;
    return oasp_bytecode_routine_at(bytecode, routine_index, &routine) &&
           oasp_bytecode_routine_slot(bytecode, &routine, index, result);
}

bool oasp_bytecode_class_at(const oasp_bytecode *bytecode, size_t index, oasp_class_bytecode *result) {
    if (!bytecode || !result || index >= bytecode->class_count)
        return false;
    if (!bytecode->image_base) {
        *result = bytecode->classes[index];
        return true;
    }
    const oasp_image_header *header = oasp_bytecode_header(bytecode);
    const oasp_image_class *classes = bytecode->image_classes;
    if (!header)
        return false;
    const oasp_image_class *source = &classes[index];
    memset(result, 0, sizeof(*result));
    if (!oasp_view_slice((const char *)bytecode->image_base, header, source->name, &result->name) ||
        source->entry >= source->limit || source->limit > header->instruction_count ||
        source->routine_offset > header->routine_count ||
        source->routine_count > header->routine_count - source->routine_offset)
        return false;
    result->entry = source->entry;
    result->limit = source->limit;
    result->routine_offset = source->routine_offset;
    result->routine_count = source->routine_count;
    result->error_aware = (source->flags & 1u) != 0;
    return true;
}

bool oasp_module_image_path(const oasp_module_image *image, oasp_slice *result) {
    if (!image || !result || !image->image_base)
        return false;
    const oasp_image_header *header = (const void *)image->image_base;
    return header->total_size == image->bytecode.image_size &&
           oasp_view_slice((const char *)image->image_base, header, header->path, result);
}

bool oasp_module_image_dependency_at(const oasp_module_image *image, size_t index, oasp_module_image_dependency *result) {
    if (!image || !result || index >= image->dependency_count || !image->image_base)
        return false;
    const oasp_image_header *header = (const void *)image->image_base;
    const oasp_image_dependency *dependencies = image->encoded_dependencies;
    if (header->total_size != image->bytecode.image_size ||
        !oasp_view_slice((const char *)image->image_base, header, dependencies[index].path, &result->path))
        return false;
    result->modified_seconds = dependencies[index].modified_seconds;
    result->modified_nanoseconds = dependencies[index].modified_nanoseconds;
    result->status_seconds = dependencies[index].status_seconds;
    result->status_nanoseconds = dependencies[index].status_nanoseconds;
    result->file_size = dependencies[index].file_size;
    result->content_hash = dependencies[index].content_hash;
    return true;
}

bool oasp_module_image_module_at(const oasp_module_image *image, size_t index, oasp_slice *result) {
    if (!image || !result || index >= image->module_count || !image->image_base)
        return false;
    const oasp_image_header *header = (const void *)image->image_base;
    const oasp_image_slice *modules = image->encoded_modules;
    return header->total_size == image->bytecode.image_size &&
           oasp_view_slice((const char *)image->image_base, header, modules[index], result);
}

bool oasp_module_image_debug_source(const oasp_module_image *image, oasp_slice *result) {
    if (!image || !result || !image->has_debug_info || !image->image_base)
        return false;
    const oasp_image_header *header = (const void *)image->image_base;
    return header->total_size == image->bytecode.image_size &&
           oasp_view_slice((const char *)image->image_base, header, header->debug_source, result);
}

static size_t oasp_source_line_count(oasp_slice source) {
    size_t count = 1;
    for (size_t i = 0; i < source.length; i++) {
        if (source.data[i] == '\r') {
            if (i + 1 < source.length && source.data[i + 1] == '\n')
                i++;
            count++;
        } else if (source.data[i] == '\n') {
            count++;
        }
    }
    return count;
}

bool oasp_module_image_decode(oasp_arena *arena, const void *data, size_t size, oasp_module_image *image) {
    (void)arena;
    if (!image)
        return false;
    memset(image, 0, sizeof(*image));
    const oasp_image_header *header = oasp_header(data, size);
    if (!header)
        return false;
    const unsigned char *bytes = data;
    oasp_bytecode *bytecode = &image->bytecode;
    bytecode->image_base = bytes;
    bytecode->image_size = size;
    bytecode->instructions = (oasp_instruction *)(void *)(bytes + header->instruction_offset);
    bytecode->length = header->instruction_count;
    bytecode->loop_count = header->loop_count;
    bytecode->root_error_aware = (header->flags & OASP_IMAGE_FLAG_ERROR_AWARE) != 0;
    bytecode->state_effects =
        ((header->flags & OASP_IMAGE_FLAG_READS_SESSION) ? OASP_STATE_READS_SESSION : 0) |
        ((header->flags & OASP_IMAGE_FLAG_WRITES_SESSION) ? OASP_STATE_WRITES_SESSION : 0) |
        ((header->flags & OASP_IMAGE_FLAG_READS_APPLICATION) ? OASP_STATE_READS_APPLICATION : 0) |
        ((header->flags & OASP_IMAGE_FLAG_WRITES_APPLICATION) ? OASP_STATE_WRITES_APPLICATION : 0);
    bytecode->image_names = bytes + header->name_offset;
    bytecode->name_count = header->name_count;
    bytecode->image_constants = bytes + header->constant_offset;
    bytecode->constant_count = header->constant_count;
    bytecode->image_global_slots = bytes + header->global_slot_offset;
    bytecode->global_slot_count = header->global_slot_count;
    bytecode->image_routines = bytes + header->routine_offset;
    bytecode->routine_count = header->routine_count;
    bytecode->image_parameters = bytes + header->parameter_offset;
    bytecode->image_routine_slots = bytes + header->routine_slot_offset;
    bytecode->image_classes = bytes + header->class_offset;
    bytecode->class_count = header->class_count;
    image->image_base = bytes;
    image->encoded_dependencies = bytes + header->dependency_offset;
    image->dependency_count = header->dependency_count;
    image->encoded_modules = bytes + header->module_offset;
    image->module_count = header->module_count;
    image->has_debug_info = (header->flags & OASP_IMAGE_FLAG_DEBUG_INFO) != 0;

    oasp_slice slice;
    oasp_value value;
    oasp_routine_bytecode routine;
    oasp_parameter_bytecode parameter;
    oasp_class_bytecode class_def;
    oasp_module_image_dependency dependency;
    if (!oasp_module_image_path(image, &slice))
        goto fail;
    if (image->has_debug_info) {
        if (!oasp_module_image_debug_source(image, &image->debug_source))
            goto fail;
    } else if (header->debug_source.offset || header->debug_source.length) {
        goto fail;
    }
    for (size_t i = 0; i < image->dependency_count; i++)
        if (!oasp_module_image_dependency_at(image, i, &dependency))
            goto fail;
    for (size_t i = 0; i < bytecode->name_count; i++)
        if (!oasp_bytecode_name_at(bytecode, i, &slice))
            goto fail;
    for (size_t i = 0; i < bytecode->constant_count; i++)
        if (!oasp_bytecode_constant_at(bytecode, i, &value))
            goto fail;
    for (size_t i = 0; i < bytecode->global_slot_count; i++)
        if (!oasp_bytecode_global_slot_at(bytecode, i, &slice))
            goto fail;
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        if (!oasp_bytecode_routine_at(bytecode, i, &routine))
            goto fail;
        for (size_t j = 0; j < routine.slot_count; j++)
            if (!oasp_bytecode_routine_slot_at(bytecode, i, j, &slice))
                goto fail;
        for (size_t j = 0; j < routine.parameter_count; j++)
            if (!oasp_bytecode_routine_parameter_at(bytecode, i, j, &parameter))
                goto fail;
    }
    for (size_t i = 0; i < bytecode->class_count; i++)
        if (!oasp_bytecode_class_at(bytecode, i, &class_def))
            goto fail;
    for (size_t i = 0; i < image->module_count; i++)
        if (!oasp_module_image_module_at(image, i, &slice))
            goto fail;
    if (!oasp_bytecode_validate(bytecode))
        goto fail;
    if (image->has_debug_info) {
        size_t source_lines = oasp_source_line_count(image->debug_source);
        for (size_t i = 0; i < bytecode->length; i++)
            if (bytecode->instructions[i].line > source_lines)
                goto fail;
    }
    return true;

fail:
    oasp_module_image_dispose(image);
    return false;
}

void oasp_module_image_dispose(oasp_module_image *image) {
    if (image)
        memset(image, 0, sizeof(*image));
}
