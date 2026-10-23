// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component_metadata.h"

#include <stdlib.h>
#include <string.h>

#define METADATA_LIMIT (1024u * 1024u)
#define METADATA_DEPTH_LIMIT 16u
#define METADATA_ITEM_LIMIT 65536u

static const unsigned char metadata_magic[] = {'O', 'A', 'I', 'M', 'E', 'T', 'A', '1'};

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} metadata_writer;

typedef struct {
    const unsigned char *data;
    size_t remaining;
} metadata_reader;

enum {
    META_EMPTY,
    META_NULL,
    META_BOOL,
    META_I64,
    META_F64,
    META_DATE,
    META_STRING,
    META_BYTES,
    META_ARRAY,
    META_MAP
};

static int metadata_error(oasp_ext_error *error, const char *message) {
    if (error)
        *error = (oasp_ext_error){
            .struct_size = sizeof(*error),
            .code = 5,
            .category = OASP_EXT_ERROR_ARGUMENT,
            .message = message,
            .message_length = strlen(message),
            .source = "OpenASP.AI",
            .source_length = 10,
        };
    return OASP_EXT_ERROR;
}

static int writer_append(metadata_writer *writer, const void *data, size_t size) {
    if (size > METADATA_LIMIT || writer->size > METADATA_LIMIT - size)
        return -1;
    if (writer->size + size > writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity : 256;
        while (capacity < writer->size + size) {
            if (capacity > METADATA_LIMIT / 2) {
                capacity = METADATA_LIMIT;
                break;
            }
            capacity *= 2;
        }
        unsigned char *resized = realloc(writer->data, capacity);
        if (!resized)
            return -1;
        writer->data = resized;
        writer->capacity = capacity;
    }
    if (size)
        memcpy(writer->data + writer->size, data, size);
    writer->size += size;
    return 0;
}

static int writer_u8(metadata_writer *writer, unsigned char value) {
    return writer_append(writer, &value, sizeof(value));
}

static int writer_u64(metadata_writer *writer, uint64_t value) {
    unsigned char encoded[8];
    for (size_t index = 0; index < sizeof(encoded); index++)
        encoded[index] = (unsigned char)(value >> (index * 8));
    return writer_append(writer, encoded, sizeof(encoded));
}

static int encode_value(oasp_ext_call_context *context, const oasp_ext_value_api *values, const oasp_ext_object_api *objects, const oasp_ext_value *value, metadata_writer *writer,
                        unsigned depth, oasp_ext_error *error);

static int encode_array(oasp_ext_call_context *context, const oasp_ext_value_api *values, const oasp_ext_object_api *objects, const oasp_ext_value *value, metadata_writer *writer,
                        unsigned depth, oasp_ext_error *error) {
    size_t rank = values->array_rank(context, value);
    if (!rank || rank > 8)
        return metadata_error(error, "checkpoint metadata arrays must have between one and eight dimensions");
    int64_t bounds[8];
    if (values->array_bounds(context, value, bounds, rank) != OASP_EXT_OK || writer_u64(writer, rank))
        return metadata_error(error, "unable to read checkpoint metadata array bounds");
    uint64_t count = 1;
    for (size_t axis = 0; axis < rank; axis++) {
        if (bounds[axis] < -1 || writer_u64(writer, (uint64_t)bounds[axis]))
            return metadata_error(error, "checkpoint metadata array has invalid bounds");
        uint64_t extent = (uint64_t)(bounds[axis] + 1);
        if (extent && count > METADATA_ITEM_LIMIT / extent)
            return metadata_error(error, "checkpoint metadata array exceeds the item limit");
        count *= extent;
    }
    int64_t indices[8] = {0};
    for (uint64_t ordinal = 0; ordinal < count; ordinal++) {
        uint64_t cursor = ordinal;
        for (size_t axis = rank; axis-- > 0;) {
            uint64_t extent = (uint64_t)(bounds[axis] + 1);
            indices[axis] = (int64_t)(cursor % extent);
            cursor /= extent;
        }
        oasp_ext_value *item = NULL;
        if (values->array_get(context, value, indices, rank, &item) != OASP_EXT_OK || !item)
            return metadata_error(error, "unable to read checkpoint metadata array item");
        int status = encode_value(context, values, objects, item, writer, depth + 1, error);
        values->release(context, item);
        if (status != OASP_EXT_OK)
            return status;
    }
    return OASP_EXT_OK;
}

static int encode_object(oasp_ext_call_context *context, const oasp_ext_value_api *values, const oasp_ext_object_api *objects, const oasp_ext_value *value, metadata_writer *writer,
                         unsigned depth, oasp_ext_error *error) {
    oasp_ext_value *keys = NULL;
    oasp_ext_error object_error = {.struct_size = sizeof(object_error)};
    if (objects->get(context, value, "Keys", 4, &keys, &object_error) != OASP_EXT_OK || !keys ||
        values->kind(context, keys) != OASP_EXT_VALUE_ARRAY) {
        if (keys)
            values->release(context, keys);
        return metadata_error(error, "checkpoint metadata objects must expose a Dictionary-compatible Keys array");
    }
    size_t rank = values->array_rank(context, keys);
    int64_t bound = -1;
    if (rank != 1 || values->array_bounds(context, keys, &bound, 1) != OASP_EXT_OK || bound < -1 ||
        (uint64_t)(bound + 1) > METADATA_ITEM_LIMIT || writer_u8(writer, META_MAP) ||
        writer_u64(writer, (uint64_t)(bound + 1))) {
        values->release(context, keys);
        return metadata_error(error, "checkpoint metadata dictionary exceeds the item limit");
    }
    for (int64_t index = 0; index <= bound; index++) {
        oasp_ext_value *key = NULL;
        oasp_ext_value *item = NULL;
        oasp_ext_bytes text = {0};
        if (values->array_get(context, keys, &index, 1, &key) != OASP_EXT_OK || !key ||
            values->kind(context, key) != OASP_EXT_VALUE_STRING ||
            values->get_data(context, key, &text) != OASP_EXT_OK || text.length > METADATA_LIMIT ||
            writer_u64(writer, text.length) || writer_append(writer, text.data, text.length) ||
            objects->invoke_default(context, value, (const oasp_ext_value *const *)&key, 1, &item,
                                    &object_error) != OASP_EXT_OK || !item) {
            if (item)
                values->release(context, item);
            if (key)
                values->release(context, key);
            values->release(context, keys);
            return metadata_error(error, "unable to read checkpoint metadata dictionary entry");
        }
        int status = encode_value(context, values, objects, item, writer, depth + 1, error);
        values->release(context, item);
        values->release(context, key);
        if (status != OASP_EXT_OK) {
            values->release(context, keys);
            return status;
        }
    }
    values->release(context, keys);
    return OASP_EXT_OK;
}

static int encode_value(oasp_ext_call_context *context, const oasp_ext_value_api *values, const oasp_ext_object_api *objects, const oasp_ext_value *value, metadata_writer *writer,
                        unsigned depth, oasp_ext_error *error) {
    if (depth > METADATA_DEPTH_LIMIT)
        return metadata_error(error, "checkpoint metadata exceeds the nesting limit");
    oasp_ext_value_kind kind = values->kind(context, value);
    if (kind == OASP_EXT_VALUE_OBJECT)
        return encode_object(context, values, objects, value, writer, depth, error);
    if (kind == OASP_EXT_VALUE_ARRAY) {
        if (writer_u8(writer, META_ARRAY))
            return metadata_error(error, "checkpoint metadata exceeds the byte limit");
        return encode_array(context, values, objects, value, writer, depth, error);
    }
    unsigned char tag;
    switch (kind) {
    case OASP_EXT_VALUE_EMPTY: tag = META_EMPTY; break;
    case OASP_EXT_VALUE_NULL: tag = META_NULL; break;
    case OASP_EXT_VALUE_BOOL: tag = META_BOOL; break;
    case OASP_EXT_VALUE_I64: tag = META_I64; break;
    case OASP_EXT_VALUE_F64: tag = META_F64; break;
    case OASP_EXT_VALUE_DATE: tag = META_DATE; break;
    case OASP_EXT_VALUE_STRING: tag = META_STRING; break;
    case OASP_EXT_VALUE_BYTES: tag = META_BYTES; break;
    default: return metadata_error(error, "checkpoint metadata contains an unsupported value");
    }
    if (writer_u8(writer, tag))
        return metadata_error(error, "checkpoint metadata exceeds the byte limit");
    if (kind == OASP_EXT_VALUE_EMPTY || kind == OASP_EXT_VALUE_NULL)
        return OASP_EXT_OK;
    if (kind == OASP_EXT_VALUE_BOOL) {
        int boolean;
        return values->get_bool(context, value, &boolean) == OASP_EXT_OK && !writer_u8(writer, boolean != 0)
                   ? OASP_EXT_OK : metadata_error(error, "unable to serialize checkpoint metadata boolean");
    }
    if (kind == OASP_EXT_VALUE_I64) {
        int64_t integer;
        return values->get_i64(context, value, &integer) == OASP_EXT_OK && !writer_u64(writer, (uint64_t)integer)
                   ? OASP_EXT_OK : metadata_error(error, "unable to serialize checkpoint metadata integer");
    }
    if (kind == OASP_EXT_VALUE_F64) {
        double number;
        uint64_t bits;
        if (values->get_f64(context, value, &number) != OASP_EXT_OK)
            return metadata_error(error, "unable to serialize checkpoint metadata number");
        memcpy(&bits, &number, sizeof(bits));
        return writer_u64(writer, bits) ? metadata_error(error, "checkpoint metadata exceeds the byte limit") : OASP_EXT_OK;
    }
    oasp_ext_bytes bytes;
    if (values->get_data(context, value, &bytes) != OASP_EXT_OK || writer_u64(writer, bytes.length) ||
        writer_append(writer, bytes.data, bytes.length))
        return metadata_error(error, "checkpoint metadata text or bytes exceeds the byte limit");
    return OASP_EXT_OK;
}

int oasp_ai_metadata_encode(oasp_ext_call_context *context, const oasp_ext_value_api *values, const oasp_ext_object_api *objects, const oasp_ext_value *value, void **data,
                            size_t *size, oasp_ext_error *error) {
    if (!context || !values || !objects || !value || !data || !size)
        return metadata_error(error, "invalid checkpoint metadata serializer arguments");
    metadata_writer writer = {0};
    if (writer_append(&writer, metadata_magic, sizeof(metadata_magic)) ||
        encode_value(context, values, objects, value, &writer, 0, error) != OASP_EXT_OK) {
        free(writer.data);
        return OASP_EXT_ERROR;
    }
    *data = writer.data;
    *size = writer.size;
    return OASP_EXT_OK;
}

static int reader_take(metadata_reader *reader, void *data, size_t size) {
    if (size > reader->remaining)
        return -1;
    if (data && size)
        memcpy(data, reader->data, size);
    reader->data += size;
    reader->remaining -= size;
    return 0;
}

static int reader_u8(metadata_reader *reader, unsigned char *value) {
    return reader_take(reader, value, sizeof(*value));
}

static int reader_u64(metadata_reader *reader, uint64_t *value) {
    unsigned char encoded[8];
    if (reader_take(reader, encoded, sizeof(encoded)))
        return -1;
    *value = 0;
    for (size_t index = 0; index < sizeof(encoded); index++)
        *value |= (uint64_t)encoded[index] << (index * 8);
    return 0;
}

static oasp_ext_value *decode_value(oasp_ext_call_context *context, const oasp_ext_value_api *values, metadata_reader *reader, unsigned depth, oasp_ext_error *error) {
    unsigned char tag;
    if (depth > METADATA_DEPTH_LIMIT || reader_u8(reader, &tag))
        return metadata_error(error, "checkpoint metadata is truncated or too deeply nested"), NULL;
    if (tag == META_EMPTY)
        return values->new_empty(context);
    if (tag == META_NULL)
        return values->new_null(context);
    if (tag == META_BOOL) {
        unsigned char boolean;
        return reader_u8(reader, &boolean) || boolean > 1
                   ? (metadata_error(error, "checkpoint metadata boolean is invalid"), NULL)
                   : values->new_bool(context, boolean);
    }
    if (tag == META_I64 || tag == META_F64) {
        uint64_t encoded;
        if (reader_u64(reader, &encoded))
            return metadata_error(error, "checkpoint metadata number is truncated"), NULL;
        if (tag == META_I64)
            return values->new_i64(context, (int64_t)encoded);
        double number;
        memcpy(&number, &encoded, sizeof(number));
        return values->new_f64(context, number);
    }
    if (tag == META_DATE || tag == META_STRING || tag == META_BYTES) {
        uint64_t length;
        if (reader_u64(reader, &length) || length > reader->remaining)
            return metadata_error(error, "checkpoint metadata data value is truncated"), NULL;
        oasp_ext_value *value = tag == META_DATE ? values->new_date(context, reader->data, (size_t)length)
                              : tag == META_STRING ? values->new_string(context, reader->data, (size_t)length)
                                                   : values->new_bytes(context, reader->data, (size_t)length);
        reader_take(reader, NULL, (size_t)length);
        return value;
    }
    if (tag == META_ARRAY) {
        uint64_t rank;
        int64_t bounds[8];
        if (reader_u64(reader, &rank) || !rank || rank > 8)
            return metadata_error(error, "checkpoint metadata array rank is invalid"), NULL;
        uint64_t count = 1;
        for (uint64_t axis = 0; axis < rank; axis++) {
            uint64_t bound;
            if (reader_u64(reader, &bound) || (bound > INT64_MAX && bound != UINT64_MAX))
                return metadata_error(error, "checkpoint metadata array bounds are invalid"), NULL;
            bounds[axis] = (int64_t)bound;
            uint64_t extent = (uint64_t)(bounds[axis] + 1);
            if (extent && count > METADATA_ITEM_LIMIT / extent)
                return metadata_error(error, "checkpoint metadata array exceeds the item limit"), NULL;
            count *= extent;
        }
        oasp_ext_value *array = values->new_array(context, bounds, (size_t)rank);
        if (!array)
            return metadata_error(error, "unable to allocate checkpoint metadata array"), NULL;
        int64_t indices[8] = {0};
        for (uint64_t ordinal = 0; ordinal < count; ordinal++) {
            uint64_t cursor = ordinal;
            for (size_t axis = (size_t)rank; axis-- > 0;) {
                uint64_t extent = (uint64_t)bounds[axis] + 1;
                indices[axis] = (int64_t)(cursor % extent);
                cursor /= extent;
            }
            oasp_ext_value *item = decode_value(context, values, reader, depth + 1, error);
            if (!item || values->array_set(context, array, indices, (size_t)rank, item) != OASP_EXT_OK) {
                if (item)
                    values->release(context, item);
                values->release(context, array);
                return metadata_error(error, "unable to restore checkpoint metadata array"), NULL;
            }
            values->release(context, item);
        }
        return array;
    }
    if (tag == META_MAP) {
        uint64_t count;
        if (reader_u64(reader, &count) || count > METADATA_ITEM_LIMIT)
            return metadata_error(error, "checkpoint metadata dictionary size is invalid"), NULL;
        oasp_ext_value *map = values->new_map(context);
        if (!map)
            return metadata_error(error, "unable to allocate checkpoint metadata dictionary"), NULL;
        for (uint64_t index = 0; index < count; index++) {
            uint64_t key_length;
            if (reader_u64(reader, &key_length) || key_length > reader->remaining) {
                values->release(context, map);
                return metadata_error(error, "checkpoint metadata dictionary key is truncated"), NULL;
            }
            const unsigned char *key = reader->data;
            reader_take(reader, NULL, (size_t)key_length);
            oasp_ext_value *item = decode_value(context, values, reader, depth + 1, error);
            if (!item || values->map_set(context, map, key, (size_t)key_length, item) != OASP_EXT_OK) {
                if (item)
                    values->release(context, item);
                values->release(context, map);
                return metadata_error(error, "unable to restore checkpoint metadata dictionary"), NULL;
            }
            values->release(context, item);
        }
        return map;
    }
    return metadata_error(error, "checkpoint metadata contains an unknown type tag"), NULL;
}

int oasp_ai_metadata_is_encoded(const void *data, size_t size) {
    return data && size >= sizeof(metadata_magic) && !memcmp(data, metadata_magic, sizeof(metadata_magic));
}

oasp_ext_value *oasp_ai_metadata_decode(oasp_ext_call_context *context, const oasp_ext_value_api *values, const void *data, size_t size, oasp_ext_error *error) {
    if (!oasp_ai_metadata_is_encoded(data, size))
        return NULL;
    metadata_reader reader = {
        .data = (const unsigned char *)data + sizeof(metadata_magic),
        .remaining = size - sizeof(metadata_magic),
    };
    oasp_ext_value *result = decode_value(context, values, &reader, 0, error);
    if (result && reader.remaining) {
        values->release(context, result);
        metadata_error(error, "checkpoint metadata has trailing data");
        return NULL;
    }
    return result;
}
