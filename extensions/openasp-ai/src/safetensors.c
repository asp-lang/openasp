// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} text_buffer;

static int text_append(text_buffer *buffer, const char *text) {
    size_t length = strlen(text);
    if (buffer->length > SIZE_MAX - length - 1)
        return -1;
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 1024;
        while (capacity < buffer->length + length + 1) {
            if (capacity > SIZE_MAX / 2)
                return -1;
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (!grown)
            return -1;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length + 1);
    buffer->length += length;
    return 0;
}

static int text_u64(text_buffer *buffer, uint64_t value) {
    char number[32];
    snprintf(number, sizeof(number), "%llu", (unsigned long long)value);
    return text_append(buffer, number);
}

static const char *safetensors_dtype(oasp_ai_dtype dtype) {
    static const char *const names[] = {"BOOL", "U8", "I8", "I16", "I32", "I64", "F16", "BF16", "F32", "F64"};
    return dtype <= OASP_AI_DTYPE_F64 ? names[dtype] : NULL;
}

static int json_name(text_buffer *buffer, const char *name) {
    if (text_append(buffer, "\""))
        return -1;
    for (const unsigned char *current = (const unsigned char *)name; *current; current++) {
        if (*current < 0x20 || *current == '"' || *current == '\\')
            return -1;
        char value[2] = {(char)*current, '\0'};
        if (text_append(buffer, value))
            return -1;
    }
    return text_append(buffer, "\"");
}

typedef struct {
    oasp_ai_state_dict *state;
} safetensors_writer;

static int write_u64_le(FILE *file, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned index = 0; index < 8; index++)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) ? 0 : -1;
}

static int save_safetensors(FILE *file, void *opaque, oasp_ai_error *error) {
    safetensors_writer *writer = opaque;
    text_buffer header = {0};
    uint64_t offset = 0;
    int status = text_append(&header, "{");
    for (size_t index = 0; index < writer->state->count && !status; index++) {
        oasp_ai_state_entry *entry = &writer->state->entries[index];
        uint64_t bytes = oasp_ai_tensor_bytes(entry->tensor);
        status = (index && text_append(&header, ",")) || json_name(&header, entry->name) ||
                 text_append(&header, ":{\"dtype\":\"") || text_append(&header, safetensors_dtype(entry->tensor->desc.dtype)) ||
                 text_append(&header, "\",\"shape\":[");
        for (uint32_t axis = 0; axis < entry->tensor->desc.rank && !status; axis++)
            status = (axis && text_append(&header, ",")) || text_u64(&header, (uint64_t)entry->tensor->desc.shape[axis]);
        if (!status)
            status = text_append(&header, "],\"data_offsets\":[") || text_u64(&header, offset) || text_append(&header, ",") ||
                     text_u64(&header, offset + bytes) || text_append(&header, "]}");
        if (UINT64_MAX - offset < bytes)
            status = -1;
        offset += bytes;
    }
    if (!status)
        status = text_append(&header, "}");
    while (!status && header.length % 8)
        status = text_append(&header, " ");
    if (!status)
        status = write_u64_le(file, header.length) || fwrite(header.data, 1, header.length, file) != header.length;
    for (size_t index = 0; index < writer->state->count && !status; index++) {
        oasp_ai_tensor *tensor = writer->state->entries[index].tensor;
        size_t bytes = (size_t)oasp_ai_tensor_bytes(tensor);
        unsigned char chunk[UINT16_MAX];
        for (size_t position = 0; position < bytes && !status;) {
            size_t amount = bytes - position < sizeof(chunk) ? bytes - position : sizeof(chunk);
            oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(tensor, error);
            if (!contiguous) {
                status = -1;
                break;
            }
            const unsigned char *data = oasp_ai_tensor_const_data(contiguous);
            memcpy(chunk, data + position, amount);
            oasp_ai_tensor_release(contiguous);
            status = fwrite(chunk, 1, amount, file) != amount;
            position += amount;
        }
    }
    free(header.data);
    if (status)
        oasp_ai_set_error(error, 5, "SaveSafeTensors", "unable to write SafeTensors data",
                          "Check tensor layout, disk capacity, and output permissions.");
    return status;
}

int oasp_ai_module_save_safetensors(oasp_ai_module *module, const char *path, oasp_ai_error *error) {
    oasp_ai_state_dict *state = oasp_ai_module_state_dict(module, error);
    if (!state)
        return -1;
    safetensors_writer writer = {state};
    int status = oasp_ai_write_atomic(path, save_safetensors, &writer, error);
    oasp_ai_state_dict_release(state);
    return status;
}

static uint64_t read_u64_le(const unsigned char *data) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; index++)
        value |= (uint64_t)data[index] << (index * 8);
    return value;
}

static void skip_space(const char **cursor, const char *end) {
    while (*cursor < end && isspace((unsigned char)**cursor))
        (*cursor)++;
}

static int expect(const char **cursor, const char *end, char character) {
    skip_space(cursor, end);
    if (*cursor >= end || **cursor != character)
        return -1;
    (*cursor)++;
    return 0;
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int parse_hex4(const char **cursor, const char *end, uint32_t *value) {
    if ((size_t)(end - *cursor) < 4)
        return -1;
    *value = 0;
    for (unsigned index = 0; index < 4; index++) {
        int digit = hex_value((*cursor)[index]);
        if (digit < 0)
            return -1;
        *value = *value * 16 + (uint32_t)digit;
    }
    *cursor += 4;
    return 0;
}

static char *parse_string(const char **cursor, const char *end) {
    if (expect(cursor, end, '"'))
        return NULL;
    char *value = malloc((size_t)(end - *cursor) + 1);
    size_t length = 0;
    if (!value)
        return NULL;
    while (*cursor < end && **cursor != '"') {
        unsigned char character = (unsigned char)*(*cursor)++;
        if (character < 0x20)
            goto failure;
        if (character != '\\') {
            value[length++] = (char)character;
            continue;
        }
        if (*cursor >= end)
            goto failure;
        character = (unsigned char)*(*cursor)++;
        if (character == '"' || character == '\\' || character == '/')
            value[length++] = (char)character;
        else if (character == 'b')
            value[length++] = '\b';
        else if (character == 'f')
            value[length++] = '\f';
        else if (character == 'n')
            value[length++] = '\n';
        else if (character == 'r')
            value[length++] = '\r';
        else if (character == 't')
            value[length++] = '\t';
        else if (character == 'u') {
            uint32_t codepoint;
            if (parse_hex4(cursor, end, &codepoint))
                goto failure;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                uint32_t low;
                if ((size_t)(end - *cursor) < 6 || (*cursor)[0] != '\\' || (*cursor)[1] != 'u') {
                    goto failure;
                }
                *cursor += 2;
                if (parse_hex4(cursor, end, &low) || low < 0xdc00 || low > 0xdfff)
                    goto failure;
                codepoint = UINT32_C(0x10000) + ((codepoint - UINT32_C(0xd800)) << 10) + (low - UINT32_C(0xdc00));
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
                goto failure;
            if (codepoint < 0x80)
                value[length++] = (char)codepoint;
            else if (codepoint < 0x800) {
                value[length++] = (char)(0xc0 | codepoint >> 6);
                value[length++] = (char)(0x80 | (codepoint & 0x3f));
            } else if (codepoint < 0x10000) {
                value[length++] = (char)(0xe0 | codepoint >> 12);
                value[length++] = (char)(0x80 | (codepoint >> 6 & 0x3f));
                value[length++] = (char)(0x80 | (codepoint & 0x3f));
            } else {
                value[length++] = (char)(0xf0 | codepoint >> 18);
                value[length++] = (char)(0x80 | (codepoint >> 12 & 0x3f));
                value[length++] = (char)(0x80 | (codepoint >> 6 & 0x3f));
                value[length++] = (char)(0x80 | (codepoint & 0x3f));
            }
        } else
            goto failure;
    }
    if (*cursor >= end || **cursor != '"')
        goto failure;
    (*cursor)++;
    value[length] = '\0';
    return value;

failure:
    free(value);
    return NULL;
}

static int parse_u64(const char **cursor, const char *end, uint64_t *value) {
    skip_space(cursor, end);
    if (*cursor >= end || !isdigit((unsigned char)**cursor))
        return -1;
    uint64_t result = 0;
    do {
        unsigned digit = (unsigned)(**cursor - '0');
        if (result > (UINT64_MAX - digit) / 10)
            return -1;
        result = result * 10 + digit;
        (*cursor)++;
    } while (*cursor < end && isdigit((unsigned char)**cursor));
    *value = result;
    return 0;
}

static int parse_dtype(const char *name, oasp_ai_dtype *dtype) {
    for (int value = OASP_AI_DTYPE_BOOL; value <= OASP_AI_DTYPE_F64; value++)
        if (!strcmp(name, safetensors_dtype((oasp_ai_dtype)value))) {
            *dtype = (oasp_ai_dtype)value;
            return 0;
        }
    return -1;
}

static int parse_tensor_entry(const char **cursor, const char *end, oasp_ai_state_entry *entry, uint64_t *begin, uint64_t *finish,
                              oasp_ai_context *context, const unsigned char *data, size_t data_size, oasp_ai_error *error) {
    char *field = NULL;
    char *dtype_name = NULL;
    oasp_ai_tensor_desc desc = {0};
    int status = expect(cursor, end, '{');
    unsigned fields = 0;
    while (!status) {
        skip_space(cursor, end);
        if (*cursor < end && **cursor == '}') {
            (*cursor)++;
            break;
        }
        if (fields)
            status = expect(cursor, end, ',');
        field = status ? NULL : parse_string(cursor, end);
        status = !field || expect(cursor, end, ':');
        if (!status && !strcmp(field, "dtype")) {
            if (fields & 1u)
                status = -1;
            dtype_name = parse_string(cursor, end);
            status = status || !dtype_name || parse_dtype(dtype_name, &desc.dtype);
            fields |= 1u;
        } else if (!status && !strcmp(field, "shape")) {
            if (fields & 2u)
                status = -1;
            status = expect(cursor, end, '[');
            while (!status) {
                skip_space(cursor, end);
                if (*cursor < end && **cursor == ']') {
                    (*cursor)++;
                    break;
                }
                if (desc.rank == OASP_AI_MAX_RANK)
                    status = -1;
                uint64_t dimension = 0;
                if (!status)
                    status = parse_u64(cursor, end, &dimension) || !dimension || dimension > INT64_MAX;
                if (!status)
                    desc.shape[desc.rank++] = (int64_t)dimension;
                skip_space(cursor, end);
                if (!status && *cursor < end && **cursor == ',')
                    (*cursor)++;
                else if (!status && (*cursor >= end || **cursor != ']'))
                    status = -1;
            }
            fields |= 2u;
        } else if (!status && !strcmp(field, "data_offsets")) {
            if (fields & 4u)
                status = -1;
            status = status || expect(cursor, end, '[') || parse_u64(cursor, end, begin) || expect(cursor, end, ',') ||
                     parse_u64(cursor, end, finish) || expect(cursor, end, ']');
            fields |= 4u;
        } else if (!status)
            status = -1;
        free(field);
        field = NULL;
    }
    status = status || fields != 7u || !dtype_name || !desc.rank || *begin > *finish || *finish > data_size;
    uint64_t elements = 0;
    if (!status)
        status = oasp_ai_checked_elements(desc.shape, desc.rank, &elements, error) ||
                 elements > UINT64_MAX / oasp_ai_dtype_size(desc.dtype) ||
                 elements * oasp_ai_dtype_size(desc.dtype) != *finish - *begin;
    if (!status)
        entry->tensor = oasp_ai_tensor_create(context, &desc, data + *begin, (size_t)(*finish - *begin), error);
    free(dtype_name);
    return status || !entry->tensor ? -1 : 0;
}

static int parse_metadata(const char **cursor, const char *end) {
    if (expect(cursor, end, '{'))
        return -1;
    int first = 1;
    for (;;) {
        skip_space(cursor, end);
        if (*cursor < end && **cursor == '}') {
            (*cursor)++;
            return 0;
        }
        if (!first && expect(cursor, end, ','))
            return -1;
        char *key = parse_string(cursor, end);
        int failed = !key || expect(cursor, end, ':');
        char *value = failed ? NULL : parse_string(cursor, end);
        failed = failed || !value;
        free(key);
        free(value);
        if (failed)
            return -1;
        first = 0;
    }
}

int oasp_ai_module_load_safetensors(oasp_ai_module *module, const char *path, int strict, oasp_ai_state_diagnostics *diagnostics, oasp_ai_error *error) {
    FILE *file = module && path ? fopen(path, "rb") : NULL;
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file)
            fclose(file);
        oasp_ai_set_error(error, 2, "LoadSafeTensors", "unable to open SafeTensors file", "Check the path and permissions.");
        return -1;
    }
    long file_length = ftell(file);
    if (file_length < 10 || (uint64_t)file_length > module->context->options.maximum_model_bytes || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        oasp_ai_set_error(error, 5, "LoadSafeTensors", "SafeTensors file exceeds configured limits", "Use a valid model within MaximumModelBytes.");
        return -1;
    }
    unsigned char *bytes = malloc((size_t)file_length);
    int failed = !bytes || fread(bytes, 1, (size_t)file_length, file) != (size_t)file_length;
    fclose(file);
    uint64_t header_size = failed ? 0 : read_u64_le(bytes);
    if (failed || header_size > (uint64_t)file_length - 8 || header_size > UINT32_C(67108864)) {
        free(bytes);
        oasp_ai_set_error(error, 5, "LoadSafeTensors", "invalid SafeTensors header", "Restore an intact standard SafeTensors file.");
        return -1;
    }
    const char *cursor = (const char *)bytes + 8;
    const char *end = cursor + header_size;
    const unsigned char *data = bytes + 8 + header_size;
    size_t data_size = (size_t)file_length - 8 - (size_t)header_size;
    oasp_ai_state_dict *state = calloc(1, sizeof(*state));
    if (!state || expect(&cursor, end, '{')) {
        free(state);
        free(bytes);
        return -1;
    }
    atomic_init(&state->references, 1);
    uint64_t *begins = NULL;
    uint64_t *finishes = NULL;
    int metadata_seen = 0;
    int first = 1;
    while (!failed) {
        skip_space(&cursor, end);
        if (cursor < end && *cursor == '}') {
            cursor++;
            break;
        }
        if (!first && expect(&cursor, end, ',')) {
            failed = 1;
            break;
        }
        if (state->count >= UINT32_C(1000000)) {
            failed = 1;
            break;
        }
        char *name = parse_string(&cursor, end);
        failed = !name || expect(&cursor, end, ':');
        if (!failed && !strcmp(name, "__metadata__")) {
            failed = metadata_seen || parse_metadata(&cursor, end);
            metadata_seen = 1;
            free(name);
            first = 0;
            continue;
        }
        for (size_t index = 0; index < state->count && !failed; index++)
            failed = !strcmp(state->entries[index].name, name);
        oasp_ai_state_entry *grown = realloc(state->entries, (state->count + 1) * sizeof(*grown));
        uint64_t *grown_begins = realloc(begins, (state->count + 1) * sizeof(*grown_begins));
        uint64_t *grown_finishes = realloc(finishes, (state->count + 1) * sizeof(*grown_finishes));
        if (failed || !grown || !grown_begins || !grown_finishes) {
            free(name);
            if (grown)
                state->entries = grown;
            if (grown_begins)
                begins = grown_begins;
            if (grown_finishes)
                finishes = grown_finishes;
            failed = 1;
            break;
        }
        state->entries = grown;
        begins = grown_begins;
        finishes = grown_finishes;
        oasp_ai_state_entry *entry = &state->entries[state->count];
        memset(entry, 0, sizeof(*entry));
        entry->name = name;
        entry->kind = OASP_AI_STATE_PARAMETER;
        uint64_t begin = 0;
        uint64_t finish = 0;
        failed = parse_tensor_entry(&cursor, end, entry, &begin, &finish, module->context, data, data_size, error);
        for (size_t index = 0; index < state->count && !failed; index++)
            failed = begin < finishes[index] && begins[index] < finish;
        if (!failed) {
            begins[state->count] = begin;
            finishes[state->count] = finish;
            state->count++;
        } else {
            free(entry->name);
            oasp_ai_tensor_release(entry->tensor);
            memset(entry, 0, sizeof(*entry));
        }
        first = 0;
    }
    skip_space(&cursor, end);
    if (cursor != end)
        failed = 1;
    int status = failed ? -1 : oasp_ai_module_load_state_dict(module, state, strict, diagnostics, error);
    oasp_ai_state_dict_release(state);
    free(begins);
    free(finishes);
    free(bytes);
    if (failed)
        oasp_ai_set_error(error, 5, "LoadSafeTensors", "SafeTensors metadata or offsets are invalid",
                          "Use a standard file with bounded contiguous tensor ranges.");
    return status;
}
