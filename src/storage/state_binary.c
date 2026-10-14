// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Implements the versioned little-endian state codec. Encoders append to a
 * checked heap buffer; decoders advance a cursor bounded by `end` and allocate
 * reconstructed graphs in the destination runtime. Depth/rank caps prevent
 * malicious persistent files from exhausting the C stack or metadata space.
 */

#include "state_binary.h"

#include "component.h"
#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OASP_STATE_BINARY_MAX_DEPTH 128
#define OASP_STATE_BINARY_MAX_RANK 64

/* Format signatures include the payload kind and current on-disk version. */
static const unsigned char oasp_state_bindings_magic[8] = {'O', 'A', 'S', 'P', 'S', 0, 0, 1};
static const unsigned char oasp_state_value_magic[8] = {'O', 'A', 'S', 'P', 'V', 0, 0, 1};

typedef struct {
    /* Half-open unread byte range [cursor, end). */
    const unsigned char *cursor;
    const unsigned char *end;
} oasp_state_reader;

/* Fixed-width writers produce host-independent little-endian integers. */
static bool oasp_state_write_u8(oasp_buffer *buffer, uint8_t value) {
    return oasp_buffer_append(buffer, &value, sizeof(value));
}

static bool oasp_state_write_u32(oasp_buffer *buffer, uint32_t value) {
    unsigned char bytes[4];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (unsigned char)(value >> (i * 8));
    return oasp_buffer_append(buffer, bytes, sizeof(bytes));
}

static bool oasp_state_write_u64(oasp_buffer *buffer, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (unsigned char)(value >> (i * 8));
    return oasp_buffer_append(buffer, bytes, sizeof(bytes));
}

static bool oasp_state_write_slice(oasp_buffer *buffer, oasp_slice value) {
    return value.length <= UINT32_MAX && oasp_state_write_u32(buffer, (uint32_t)value.length) &&
           oasp_buffer_append(buffer, value.data, value.length);
}

static bool oasp_state_encode_value_data(oasp_buffer *buffer, const oasp_value *value, size_t depth);

/*
 * Writes list count before entries so decoders can bound work. Names, constant
 * flags, and recursive values preserve exactly the persisted binding state.
 */
static bool oasp_state_encode_bindings_data(oasp_buffer *buffer, const oasp_binding *bindings, size_t depth) {
    if (depth > OASP_STATE_BINARY_MAX_DEPTH)
        return false;
    uint64_t count = 0;
    for (const oasp_binding *binding = bindings; binding; binding = binding->next) {
        if (count == UINT64_MAX)
            return false;
        count++;
    }
    if (!oasp_state_write_u64(buffer, count))
        return false;
    for (const oasp_binding *binding = bindings; binding; binding = binding->next)
        if (!oasp_state_write_slice(buffer, binding->name) || !oasp_state_write_u8(buffer, binding->constant ? 1 : 0) ||
            !oasp_state_encode_value_data(buffer, &binding->value, depth + 1))
            return false;
    return true;
}

/*
 * Writes a kind tag followed by kind-specific payload. Arrays recurse over
 * row-major items; only dictionaries and script-class objects are persistable,
 * because arbitrary native component handles cannot survive process teardown.
 */
static bool oasp_state_encode_value_data(oasp_buffer *buffer, const oasp_value *value, size_t depth) {
    if (!value || depth > OASP_STATE_BINARY_MAX_DEPTH || value->kind < OASP_VALUE_EMPTY || value->kind > OASP_VALUE_DATE ||
        !oasp_state_write_u8(buffer, (uint8_t)value->kind))
        return false;
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    case OASP_VALUE_BOOL:
        return oasp_state_write_u8(buffer, value->as.boolean ? 1 : 0);
    case OASP_VALUE_INT: {
        uint64_t bits;
        memcpy(&bits, &value->as.integer, sizeof(bits));
        return oasp_state_write_u64(buffer, bits);
    }
    case OASP_VALUE_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &value->as.floating, sizeof(bits));
        return oasp_state_write_u64(buffer, bits);
    }
    case OASP_VALUE_STRING:
        return oasp_state_write_slice(buffer, value->as.string);
    case OASP_VALUE_DATE:
        return oasp_state_write_slice(buffer, value->as.date);
    case OASP_VALUE_ARRAY: {
        const oasp_array *array = value->as.array;
        if (!array || array->rank > OASP_STATE_BINARY_MAX_RANK || array->rank > UINT32_MAX ||
            !oasp_state_write_u32(buffer, (uint32_t)array->rank) || !oasp_state_write_u64(buffer, (uint64_t)array->length))
            return false;
        for (size_t i = 0; i < array->rank; i++) {
            uint64_t bits;
            memcpy(&bits, &array->bounds[i], sizeof(bits));
            if (!oasp_state_write_u64(buffer, bits))
                return false;
        }
        for (size_t i = 0; i < array->length; i++)
            if (!oasp_state_encode_value_data(buffer, &array->items[i], depth + 1))
                return false;
        return true;
    }
    case OASP_VALUE_OBJECT: {
        const oasp_object *object = value->as.object;
        int compare_mode = 0;
        size_t count = 0;
        if (!object)
            return false;
        if (oasp_component_dictionary_info(object, &compare_mode, &count)) {
            if (!oasp_state_write_u8(buffer, 1) || !oasp_state_write_u32(buffer, (uint32_t)compare_mode) ||
                !oasp_state_write_u64(buffer, (uint64_t)count))
                return false;
            for (size_t i = 0; i < count; i++) {
                const oasp_value *key = NULL;
                const oasp_value *item = NULL;
                if (!oasp_component_dictionary_item(object, i, &key, &item) ||
                    !oasp_state_encode_value_data(buffer, key, depth + 1) ||
                    !oasp_state_encode_value_data(buffer, item, depth + 1))
                    return false;
            }
            return true;
        }
        char *xml = NULL;
        size_t xml_length = 0;
        if (oasp_component_xml_snapshot(object, &xml, &xml_length)) {
            bool written = oasp_state_write_u8(buffer, 3) &&
                           oasp_state_write_slice(buffer, (oasp_slice){xml, xml_length});
            free(xml);
            return written;
        }
        return object->class_def && oasp_state_write_u8(buffer, 2) && oasp_state_write_slice(buffer, object->class_def->name) &&
               oasp_state_encode_bindings_data(buffer, object->fields, depth + 1);
    }
    }
    return false;
}

/* Transfers buffer ownership to caller-provided output pointers. */
static bool oasp_state_finish_buffer(oasp_buffer *buffer, char **data, size_t *length) {
    if (!data || !length) {
        oasp_buffer_dispose(buffer);
        return false;
    }
    *data = buffer->data;
    *length = buffer->length;
    return true;
}

bool oasp_state_binary_encode_bindings(const oasp_binding *bindings, char **data, size_t *length) {
    oasp_buffer buffer;
    if (!oasp_buffer_init(&buffer, 256))
        return false;
    if (!oasp_buffer_append(&buffer, oasp_state_bindings_magic, sizeof(oasp_state_bindings_magic)) ||
        !oasp_state_encode_bindings_data(&buffer, bindings, 0)) {
        oasp_buffer_dispose(&buffer);
        return false;
    }
    return oasp_state_finish_buffer(&buffer, data, length);
}

bool oasp_state_binary_encode_value(const oasp_value *value, char **data, size_t *length) {
    oasp_buffer buffer;
    if (!oasp_buffer_init(&buffer, 128))
        return false;
    if (!oasp_buffer_append(&buffer, oasp_state_value_magic, sizeof(oasp_state_value_magic)) ||
        !oasp_state_encode_value_data(&buffer, value, 0)) {
        oasp_buffer_dispose(&buffer);
        return false;
    }
    return oasp_state_finish_buffer(&buffer, data, length);
}

/* Bounds-checks before advancing; an optional output borrows image bytes. */
static bool oasp_state_read_bytes(oasp_state_reader *reader, size_t length, const unsigned char **data) {
    if (!reader || length > (size_t)(reader->end - reader->cursor))
        return false;
    if (data)
        *data = reader->cursor;
    reader->cursor += length;
    return true;
}

/* Fixed-width readers reverse the little-endian representation without casts. */
static bool oasp_state_read_u8(oasp_state_reader *reader, uint8_t *value) {
    const unsigned char *bytes;
    if (!value || !oasp_state_read_bytes(reader, 1, &bytes))
        return false;
    *value = bytes[0];
    return true;
}

static bool oasp_state_read_u32(oasp_state_reader *reader, uint32_t *value) {
    const unsigned char *bytes;
    if (!value || !oasp_state_read_bytes(reader, 4, &bytes))
        return false;
    *value = 0;
    for (size_t i = 0; i < 4; i++)
        *value |= (uint32_t)bytes[i] << (i * 8);
    return true;
}

static bool oasp_state_read_u64(oasp_state_reader *reader, uint64_t *value) {
    const unsigned char *bytes;
    if (!value || !oasp_state_read_bytes(reader, 8, &bytes))
        return false;
    *value = 0;
    for (size_t i = 0; i < 8; i++)
        *value |= (uint64_t)bytes[i] << (i * 8);
    return true;
}

/* Reads a length-prefixed byte slice and copies it into runtime-owned storage. */
static bool oasp_state_read_slice(oasp_runtime *runtime, oasp_state_reader *reader, oasp_slice *value) {
    uint32_t length;
    const unsigned char *bytes;
    if (!runtime || !value || !oasp_state_read_u32(reader, &length) || !oasp_state_read_bytes(reader, length, &bytes))
        return false;
    value->data = oasp_runtime_copy(runtime, (const char *)bytes, length);
    value->length = length;
    return value->data != NULL;
}

static bool oasp_state_decode_value_data(oasp_runtime *runtime, oasp_state_reader *reader, oasp_value *value, size_t depth);

/*
 * Reconstructs exactly the declared number of linked bindings. Each node is
 * runtime-managed and rooted by the partially built list while recursive value
 * decoding may allocate.
 */
static bool oasp_state_decode_bindings_data(oasp_runtime *runtime, oasp_state_reader *reader, oasp_binding **bindings, size_t depth) {
    uint64_t encoded_count;
    if (!runtime || !bindings || depth > OASP_STATE_BINARY_MAX_DEPTH || !oasp_state_read_u64(reader, &encoded_count) ||
        encoded_count > SIZE_MAX || encoded_count > (uint64_t)(reader->end - reader->cursor) / 6 + 1)
        return false;
    oasp_binding *first = NULL;
    oasp_binding **tail = &first;
    for (size_t i = 0; i < (size_t)encoded_count; i++) {
        oasp_binding *binding = oasp_runtime_alloc_binding(runtime);
        uint8_t constant;
        if (!binding)
            return false;
        memset(binding, 0, sizeof(*binding));
        if (!oasp_state_read_slice(runtime, reader, &binding->name) || !oasp_state_read_u8(reader, &constant) || constant > 1 ||
            !oasp_state_decode_value_data(runtime, reader, &binding->value, depth + 1))
            return false;
        binding->constant = constant != 0;
        *tail = binding;
        tail = &binding->next;
    }
    *bindings = first;
    return true;
}

/*
 * Validates tags and all declared sizes before allocating payloads. Array
 * length is recomputed from dimensions to reject inconsistent images; object
 * subtypes are restored only through supported component/class constructors.
 */
static bool oasp_state_decode_value_data(oasp_runtime *runtime, oasp_state_reader *reader, oasp_value *value, size_t depth) {
    uint8_t kind;
    if (!runtime || !value || depth > OASP_STATE_BINARY_MAX_DEPTH || !oasp_state_read_u8(reader, &kind) || kind > OASP_VALUE_DATE)
        return false;
    *value = oasp_value_empty();
    value->kind = (oasp_value_kind)kind;
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    case OASP_VALUE_BOOL: {
        uint8_t boolean;
        if (!oasp_state_read_u8(reader, &boolean) || boolean > 1)
            return false;
        value->as.boolean = boolean != 0;
        return true;
    }
    case OASP_VALUE_INT: {
        uint64_t integer;
        if (!oasp_state_read_u64(reader, &integer))
            return false;
        memcpy(&value->as.integer, &integer, sizeof(integer));
        return true;
    }
    case OASP_VALUE_FLOAT: {
        uint64_t bits;
        if (!oasp_state_read_u64(reader, &bits))
            return false;
        memcpy(&value->as.floating, &bits, sizeof(bits));
        return true;
    }
    case OASP_VALUE_STRING:
        return oasp_state_read_slice(runtime, reader, &value->as.string);
    case OASP_VALUE_DATE:
        return oasp_state_read_slice(runtime, reader, &value->as.date);
    case OASP_VALUE_ARRAY: {
        uint32_t rank;
        uint64_t encoded_length;
        if (!oasp_state_read_u32(reader, &rank) || rank > OASP_STATE_BINARY_MAX_RANK ||
            !oasp_state_read_u64(reader, &encoded_length) || encoded_length > SIZE_MAX ||
            encoded_length > (uint64_t)(reader->end - reader->cursor))
            return false;
        size_t length = (size_t)encoded_length;
        if (length && length > SIZE_MAX / sizeof(oasp_value))
            return false;
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return false;
        memset(array, 0, sizeof(*array));
        array->rank = rank;
        array->length = length;
        if (rank) {
            array->bounds = oasp_runtime_alloc(runtime, (size_t)rank * sizeof(*array->bounds), _Alignof(int64_t));
            if (!array->bounds)
                return false;
            for (size_t i = 0; i < rank; i++) {
                uint64_t bound;
                if (!oasp_state_read_u64(reader, &bound))
                    return false;
                memcpy(&array->bounds[i], &bound, sizeof(bound));
            }
        }
        if (length) {
            array->items = oasp_runtime_alloc_values(runtime, length);
            if (!array->items)
                return false;
            for (size_t i = 0; i < length; i++)
                if (!oasp_state_decode_value_data(runtime, reader, &array->items[i], depth + 1))
                    return false;
        }
        value->as.array = array;
        return true;
    }
    case OASP_VALUE_OBJECT: {
        uint8_t object_kind;
        if (!oasp_state_read_u8(reader, &object_kind))
            return false;
        if (object_kind == 1) {
            uint32_t encoded_mode;
            uint64_t encoded_count;
            if (!oasp_state_read_u32(reader, &encoded_mode) || !oasp_state_read_u64(reader, &encoded_count) ||
                encoded_count > SIZE_MAX || encoded_count > (uint64_t)(reader->end - reader->cursor) / 2)
                return false;
            size_t count = (size_t)encoded_count;
            if (count && count > SIZE_MAX / sizeof(oasp_value))
                return false;
            oasp_value *keys = count ? oasp_runtime_alloc(runtime, count * sizeof(*keys), _Alignof(oasp_value)) : NULL;
            oasp_value *items = count ? oasp_runtime_alloc(runtime, count * sizeof(*items), _Alignof(oasp_value)) : NULL;
            if (count && (!keys || !items))
                return false;
            for (size_t i = 0; i < count; i++)
                if (!oasp_state_decode_value_data(runtime, reader, &keys[i], depth + 1) ||
                    !oasp_state_decode_value_data(runtime, reader, &items[i], depth + 1))
                    return false;
            value->as.object = oasp_component_dictionary_restore(runtime, (int32_t)encoded_mode, keys, items, count);
            return value->as.object != NULL;
        }
        if (object_kind == 2) {
            oasp_object *object = oasp_runtime_alloc_object(runtime);
            oasp_class_def *definition = oasp_runtime_alloc(runtime, sizeof(*definition), _Alignof(oasp_class_def));
            if (!object || !definition)
                return false;
            memset(object, 0, sizeof(*object));
            memset(definition, 0, sizeof(*definition));
            if (!oasp_state_read_slice(runtime, reader, &definition->name) ||
                !oasp_state_decode_bindings_data(runtime, reader, &object->fields, depth + 1))
                return false;
            object->class_def = definition;
            value->as.object = object;
            return true;
        }
        if (object_kind == 3) {
            oasp_slice xml;
            if (!oasp_state_read_slice(runtime, reader, &xml))
                return false;
            return oasp_component_xml_restore(runtime, xml, &value->as.object);
        }
        return false;
    }
    }
    return false;
}

/* Initializes a reader only when the complete versioned magic header matches. */
static bool oasp_state_reader_init(oasp_state_reader *reader, const void *data, size_t length, const unsigned char magic[8]) {
    if (!reader || !data || length < 8 || memcmp(data, magic, 8))
        return false;
    reader->cursor = (const unsigned char *)data + 8;
    reader->end = (const unsigned char *)data + length;
    return true;
}

/* Public decoders additionally require full input consumption, rejecting suffix data. */
bool oasp_state_binary_decode_bindings(oasp_runtime *runtime, const void *data, size_t length, oasp_binding **bindings) {
    oasp_state_reader reader;
    return oasp_state_reader_init(&reader, data, length, oasp_state_bindings_magic) &&
           oasp_state_decode_bindings_data(runtime, &reader, bindings, 0) && reader.cursor == reader.end;
}

bool oasp_state_binary_decode_value(oasp_runtime *runtime, const void *data, size_t length, oasp_value *value) {
    oasp_state_reader reader;
    return oasp_state_reader_init(&reader, data, length, oasp_state_value_magic) &&
           oasp_state_decode_value_data(runtime, &reader, value, 0) && reader.cursor == reader.end;
}
