#define _POSIX_C_SOURCE 200809L

// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

enum {
    OAIR_SECTION_MANIFEST = 1,
    OAIR_SECTION_SIGNATURES,
    OAIR_SECTION_VALUES,
    OAIR_SECTION_OPERATIONS,
    OAIR_SECTION_TENSOR_DATA,
    OAIR_SECTION_CACHE
};

enum {
    OAIR_REQUIRED = 1u,
    OAIR_MAX_SECTIONS = 64u,
    OAIR_MAX_VALUES = 10000000u,
    OAIR_MAX_OPERATIONS = 10000000u,
    OAIR_HEADER_SIZE = 72u,
    OAIR_SECTION_ENTRY_SIZE = 64u
};

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
    unsigned char sha256[32];
} oair_section;

typedef struct {
    uint32_t bits;
    int32_t axis;
    double scale;
    int64_t zero_point;
    double *channel_scales;
    size_t channel_count;
    char calibration[32];
} oair_quantization;

static int put_bytes(FILE *file, const void *data, size_t size) {
    return size && fwrite(data, 1, size, file) != size ? -1 : 0;
}

static int get_bytes(FILE *file, void *data, size_t size) {
    return size && fread(data, 1, size, file) != size ? -1 : 0;
}

static int put_u16(FILE *file, uint16_t value) {
    unsigned char data[] = {(unsigned char)value, (unsigned char)(value >> 8)};
    return put_bytes(file, data, sizeof(data));
}

static int put_u32(FILE *file, uint32_t value) {
    unsigned char data[] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return put_bytes(file, data, sizeof(data));
}

static int put_u64(FILE *file, uint64_t value) {
    unsigned char data[8];
    for (unsigned index = 0; index < 8; index++)
        data[index] = (unsigned char)(value >> (index * 8));
    return put_bytes(file, data, sizeof(data));
}

static int get_u16(FILE *file, uint16_t *value) {
    unsigned char data[2];
    if (get_bytes(file, data, sizeof(data)))
        return -1;
    *value = (uint16_t)((uint16_t)data[0] | (uint16_t)data[1] << 8);
    return 0;
}

static int get_u32(FILE *file, uint32_t *value) {
    unsigned char data[4];
    if (get_bytes(file, data, sizeof(data)))
        return -1;
    *value = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    return 0;
}

static int get_u64(FILE *file, uint64_t *value) {
    unsigned char data[8];
    if (get_bytes(file, data, sizeof(data)))
        return -1;
    *value = 0;
    for (unsigned index = 0; index < 8; index++)
        *value |= (uint64_t)data[index] << (index * 8);
    return 0;
}

static int put_string(FILE *file, const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length > UINT32_MAX || put_u32(file, (uint32_t)length) || put_bytes(file, value, length) ? -1 : 0;
}

static char *get_string(FILE *file, uint32_t maximum) {
    uint32_t length;
    if (get_u32(file, &length) || length > maximum)
        return NULL;
    char *value = malloc((size_t)length + 1);
    if (!value || get_bytes(file, value, length)) {
        free(value);
        return NULL;
    }
    value[length] = '\0';
    return value;
}

static int begin_section(FILE *file, oair_section *section, uint32_t type, uint32_t flags) {
    long offset = ftell(file);
    if (offset < 0)
        return -1;
    *section = (oair_section){.type = type, .flags = flags, .offset = (uint64_t)offset};
    return 0;
}

static int finish_section(FILE *file, oair_section *section) {
    long finish = ftell(file);
    if (finish < 0 || (uint64_t)finish < section->offset || fflush(file))
        return -1;
    section->length = (uint64_t)finish - section->offset;
    if (fseek(file, (long)section->offset, SEEK_SET))
        return -1;
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    unsigned digest_size = 0;
    unsigned char chunk[65536];
    uint64_t remaining = section->length;
    int status = !digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1;
    while (!status && remaining) {
        size_t amount = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        status = fread(chunk, 1, amount, file) != amount || EVP_DigestUpdate(digest, chunk, amount) != 1;
        remaining -= amount;
    }
    if (!status)
        status = EVP_DigestFinal_ex(digest, section->sha256, &digest_size) != 1 || digest_size != 32;
    EVP_MD_CTX_free(digest);
    return status || fseek(file, finish, SEEK_SET) ? -1 : 0;
}

static int write_header(FILE *file, uint32_t section_count, uint64_t footer_offset, uint64_t file_size) {
    unsigned char reserved[24] = {0};
    return fseek(file, 0, SEEK_SET) || put_bytes(file, "OASPMDL2", 8) || put_u16(file, 2) || put_u16(file, 0) ||
           put_u16(file, OASP_AI_OPERATOR_SET_MAJOR) || put_u16(file, OASP_AI_OPERATOR_SET_MINOR) ||
           put_u32(file, UINT32_C(0x01020304)) || put_u32(file, 0) || put_u32(file, section_count) || put_u32(file, 0) ||
           put_u64(file, footer_offset) || put_u64(file, file_size) || put_bytes(file, reserved, sizeof(reserved));
}

static int write_manifest(FILE *file, oasp_ai_graph *graph) {
    return put_string(file, graph->name) || put_u32(file, OASP_AI_VERSION_MAJOR) || put_u32(file, OASP_AI_VERSION_MINOR) ||
           put_u32(file, OASP_AI_OPERATOR_SET_MAJOR) || put_u32(file, OASP_AI_OPERATOR_SET_MINOR) ||
           put_string(file, "reference-cpu") || put_u32(file, 0);
}

static int write_signatures(FILE *file, oasp_ai_graph *graph) {
    if (put_u32(file, (uint32_t)graph->input_count) || put_u32(file, (uint32_t)graph->output_count))
        return -1;
    for (size_t group = 0; group < 2; group++) {
        uint32_t *items = group ? graph->outputs : graph->inputs;
        size_t count = group ? graph->output_count : graph->input_count;
        for (size_t index = 0; index < count; index++)
            if (put_u32(file, items[index]))
                return -1;
    }
    return 0;
}

static int write_values(FILE *file, oasp_ai_graph *graph) {
    if (put_u32(file, (uint32_t)graph->value_count))
        return -1;
    uint64_t tensor_offset = 0;
    for (size_t index = 0; index < graph->value_count; index++) {
        oasp_ai_ir_value *value = &graph->values[index];
        uint64_t bytes = value->constant ? oasp_ai_tensor_bytes(value->constant) : 0;
        if (put_u32(file, value->id) || put_string(file, value->name) || put_u32(file, value->desc.dtype) ||
            put_u32(file, value->desc.rank) || put_u32(file, value->constant != NULL))
            return -1;
        for (uint32_t axis = 0; axis < value->desc.rank; axis++)
            if (put_u64(file, (uint64_t)value->desc.shape[axis]) || put_u64(file, (uint64_t)value->desc.strides[axis]))
                return -1;
        uint64_t value_offset = value->constant ? tensor_offset : 0;
        if (put_u64(file, value_offset) || put_u64(file, bytes))
            return -1;
        if (value->constant && UINT64_MAX - tensor_offset < bytes)
            return -1;
        if (value->constant)
            tensor_offset += bytes;
        oasp_ai_quantization_info quantization = {0};
        oasp_ai_tensor_quantization(value->constant, &quantization);
        if (put_u32(file, quantization.enabled ? quantization.bits : 0) ||
            put_u32(file, quantization.enabled ? (uint32_t)quantization.axis : 0) ||
            put_u64(file, quantization.enabled ? (uint64_t)quantization.channel_count : 0))
            return -1;
        if (quantization.enabled) {
            uint64_t scale_bits;
            uint64_t zero_bits = (uint64_t)quantization.zero_point;
            memcpy(&scale_bits, &quantization.scale, sizeof(scale_bits));
            if (put_u64(file, scale_bits) || put_u64(file, zero_bits) || put_string(file, quantization.calibration_method))
                return -1;
            for (size_t channel = 0; channel < quantization.channel_count; channel++) {
                memcpy(&scale_bits, &quantization.channel_scales[channel], sizeof(scale_bits));
                if (put_u64(file, scale_bits))
                    return -1;
            }
        }
    }
    return 0;
}

static int write_operations(FILE *file, oasp_ai_graph *graph) {
    if (put_u32(file, (uint32_t)graph->operation_count))
        return -1;
    for (size_t index = 0; index < graph->operation_count; index++) {
        oasp_ai_ir_operation *operation = &graph->operations[index];
        if (put_u32(file, operation->op_id) || put_u16(file, operation->op_version) || put_u16(file, operation->flags) ||
            put_u32(file, operation->output) || put_u32(file, (uint32_t)operation->input_count))
            return -1;
        for (size_t input = 0; input < operation->input_count; input++)
            if (put_u32(file, operation->inputs[input]))
                return -1;
        if (put_u64(file, operation->attributes_size) || put_bytes(file, operation->attributes, operation->attributes_size))
            return -1;
    }
    return 0;
}

static int write_tensors(FILE *file, oasp_ai_graph *graph, oasp_ai_error *error) {
    unsigned char chunk[65536];
    for (size_t index = 0; index < graph->value_count; index++) {
        oasp_ai_tensor *tensor = graph->values[index].constant;
        if (!tensor)
            continue;
        oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(tensor, error);
        if (!contiguous)
            return -1;
        const unsigned char *data = oasp_ai_tensor_const_data(contiguous);
        size_t bytes = (size_t)oasp_ai_tensor_bytes(contiguous);
        int status = 0;
        for (size_t position = 0; position < bytes && !status;) {
            size_t amount = bytes - position < sizeof(chunk) ? bytes - position : sizeof(chunk);
            memcpy(chunk, data + position, amount);
            status = put_bytes(file, chunk, amount);
            position += amount;
        }
        oasp_ai_tensor_release(contiguous);
        if (status)
            return -1;
    }
    return 0;
}

typedef struct {
    oasp_ai_graph *graph;
} oair_writer;

static int save_oair(FILE *file, void *opaque, oasp_ai_error *error) {
    oasp_ai_graph *graph = ((oair_writer *)opaque)->graph;
    unsigned char empty[OAIR_HEADER_SIZE] = {0};
    oair_section sections[5];
    if (put_bytes(file, empty, sizeof(empty)))
        return -1;
    if (begin_section(file, &sections[0], OAIR_SECTION_MANIFEST, OAIR_REQUIRED) || write_manifest(file, graph) ||
        finish_section(file, &sections[0]) ||
        begin_section(file, &sections[1], OAIR_SECTION_SIGNATURES, OAIR_REQUIRED) || write_signatures(file, graph) ||
        finish_section(file, &sections[1]) ||
        begin_section(file, &sections[2], OAIR_SECTION_VALUES, OAIR_REQUIRED) || write_values(file, graph) ||
        finish_section(file, &sections[2]) ||
        begin_section(file, &sections[3], OAIR_SECTION_OPERATIONS, OAIR_REQUIRED) || write_operations(file, graph) ||
        finish_section(file, &sections[3]) ||
        begin_section(file, &sections[4], OAIR_SECTION_TENSOR_DATA, OAIR_REQUIRED) || write_tensors(file, graph, error) ||
        finish_section(file, &sections[4]))
        return -1;
    long footer = ftell(file);
    if (footer < 0 || put_bytes(file, "OAIRIDX2", 8) || put_u32(file, 5) || put_u32(file, 0))
        return -1;
    for (size_t index = 0; index < 5; index++)
        if (put_u32(file, sections[index].type) || put_u32(file, sections[index].flags) || put_u64(file, sections[index].offset) ||
            put_u64(file, sections[index].length) || put_bytes(file, sections[index].sha256, sizeof(sections[index].sha256)) ||
            put_u64(file, 0))
            return -1;
    long finish = ftell(file);
    if (finish < 0 || write_header(file, 5, (uint64_t)footer, (uint64_t)finish) || fseek(file, finish, SEEK_SET))
        return -1;
    return 0;
}

int oasp_ai_graph_save(oasp_ai_graph *graph, const char *path, oasp_ai_error *error) {
    if (oasp_ai_graph_verify(graph, error))
        return -1;
    oair_writer writer = {graph};
    return oasp_ai_write_atomic(path, save_oair, &writer, error);
}

static int digest_section(FILE *file, const oair_section *section, unsigned char digest_value[32]) {
    if (section->offset > LONG_MAX || fseek(file, (long)section->offset, SEEK_SET))
        return -1;
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    unsigned digest_size = 0;
    unsigned char chunk[65536];
    uint64_t remaining = section->length;
    int status = !digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1;
    while (!status && remaining) {
        size_t amount = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        status = fread(chunk, 1, amount, file) != amount || EVP_DigestUpdate(digest, chunk, amount) != 1;
        remaining -= amount;
    }
    if (!status)
        status = EVP_DigestFinal_ex(digest, digest_value, &digest_size) != 1 || digest_size != 32;
    EVP_MD_CTX_free(digest);
    return status;
}

static const oair_section *find_section(const oair_section *sections, uint32_t count, uint32_t type) {
    for (uint32_t index = 0; index < count; index++)
        if (sections[index].type == type)
            return &sections[index];
    return NULL;
}

static int seek_section(FILE *file, const oair_section *sections, uint32_t count, uint32_t type) {
    const oair_section *section = find_section(sections, count, type);
    return !section || section->offset > LONG_MAX || fseek(file, (long)section->offset, SEEK_SET) ? -1 : 0;
}

static void free_quantization(oair_quantization *quantization, size_t count) {
    if (!quantization)
        return;
    for (size_t index = 0; index < count; index++)
        free(quantization[index].channel_scales);
    free(quantization);
}

static int load_values(FILE *file, const oair_section *sections, uint32_t section_count, oasp_ai_graph *graph, uint64_t **offsets,
                       uint64_t **lengths, oair_quantization **quantization, oasp_ai_error *error) {
    uint32_t count;
    if (seek_section(file, sections, section_count, OAIR_SECTION_VALUES) || get_u32(file, &count) || count > OAIR_MAX_VALUES)
        return -1;
    *offsets = count ? calloc(count, sizeof(**offsets)) : NULL;
    *lengths = count ? calloc(count, sizeof(**lengths)) : NULL;
    *quantization = count ? calloc(count, sizeof(**quantization)) : NULL;
    if (count && (!*offsets || !*lengths || !*quantization))
        return -1;
    for (uint32_t index = 0; index < count; index++) {
        uint32_t id;
        uint32_t dtype;
        uint32_t rank;
        uint32_t constant;
        oasp_ai_tensor_desc desc = {0};
        char *name = NULL;
        if (get_u32(file, &id) || id != index + 1 || !(name = get_string(file, 4096)) || get_u32(file, &dtype) ||
            dtype > OASP_AI_DTYPE_F64 || get_u32(file, &rank) || !rank || rank > OASP_AI_MAX_RANK || get_u32(file, &constant)) {
            free(name);
            return -1;
        }
        desc.dtype = (oasp_ai_dtype)dtype;
        desc.rank = rank;
        for (uint32_t axis = 0; axis < rank; axis++) {
            uint64_t dimension;
            uint64_t stride;
            if (get_u64(file, &dimension) || get_u64(file, &stride) || !dimension ||
                (dimension > INT64_MAX && dimension != UINT64_MAX) || stride > INT64_MAX) {
                free(name);
                return -1;
            }
            desc.shape[axis] = (int64_t)dimension;
            desc.strides[axis] = (int64_t)stride;
        }
        if (get_u64(file, &(*offsets)[index]) || get_u64(file, &(*lengths)[index])) {
            free(name);
            return -1;
        }
        uint32_t quantized_bits;
        uint32_t quantized_axis;
        uint64_t channel_count;
        if (get_u32(file, &quantized_bits) || get_u32(file, &quantized_axis) || get_u64(file, &channel_count) ||
            channel_count > UINT32_MAX) {
            free(name);
            return -1;
        }
        oasp_ai_ir_value *value = oasp_ai_graph_add_value_internal(graph, name, &desc, error);
        free(name);
        if (!value)
            return -1;
        if (quantized_bits) {
            uint64_t scale_bits;
            uint64_t zero_bits;
            char *calibration = NULL;
            if ((quantized_bits != 4 && quantized_bits != 8) || get_u64(file, &scale_bits) || get_u64(file, &zero_bits) ||
                !(calibration = get_string(file, 31))) {
                free(calibration);
                return -1;
            }
            oair_quantization *metadata = &(*quantization)[index];
            metadata->bits = quantized_bits;
            metadata->axis = (int32_t)quantized_axis;
            memcpy(&metadata->scale, &scale_bits, sizeof(metadata->scale));
            metadata->zero_point = (int64_t)zero_bits;
            metadata->channel_count = (size_t)channel_count;
            memcpy(metadata->calibration, calibration, strlen(calibration) + 1);
            free(calibration);
            if (metadata->axis < -1 || metadata->axis >= (int32_t)rank || !(metadata->scale > 0) ||
                (channel_count && (metadata->axis < 0 || channel_count != (uint64_t)desc.shape[metadata->axis])))
                return -1;
            metadata->channel_scales = channel_count ? malloc((size_t)channel_count * sizeof(*metadata->channel_scales)) : NULL;
            if (channel_count && !metadata->channel_scales)
                return -1;
            for (uint64_t channel = 0; channel < channel_count; channel++) {
                if (get_u64(file, &scale_bits))
                    return -1;
                memcpy(&metadata->channel_scales[channel], &scale_bits, sizeof(metadata->channel_scales[channel]));
                if (!(metadata->channel_scales[channel] > 0))
                    return -1;
            }
        } else if (quantized_axis || channel_count) {
            return -1;
        }
        if (!constant && ((*offsets)[index] || (*lengths)[index]))
            return -1;
    }
    return 0;
}

static int load_operations(FILE *file, const oair_section *sections, uint32_t section_count, oasp_ai_graph *graph) {
    uint32_t count;
    if (seek_section(file, sections, section_count, OAIR_SECTION_OPERATIONS) || get_u32(file, &count) || count > OAIR_MAX_OPERATIONS)
        return -1;
    for (uint32_t index = 0; index < count; index++) {
        oasp_ai_ir_operation *grown =
            oasp_ai_grow_array(graph->operations, sizeof(*graph->operations), &graph->operation_capacity, graph->operation_count + 1);
        if (!grown)
            return -1;
        graph->operations = grown;
        oasp_ai_ir_operation *operation = &graph->operations[graph->operation_count++];
        memset(operation, 0, sizeof(*operation));
        uint32_t input_count;
        if (get_u32(file, &operation->op_id) || get_u16(file, &operation->op_version) || get_u16(file, &operation->flags) ||
            get_u32(file, &operation->output) || get_u32(file, &input_count) || !input_count || input_count > graph->value_count)
            return -1;
        operation->input_count = input_count;
        operation->inputs = malloc((size_t)input_count * sizeof(*operation->inputs));
        if (!operation->inputs)
            return -1;
        for (uint32_t input = 0; input < input_count; input++)
            if (get_u32(file, &operation->inputs[input]))
                return -1;
        uint64_t attributes_size;
        if (get_u64(file, &attributes_size) || attributes_size > UINT32_MAX)
            return -1;
        operation->attributes_size = (size_t)attributes_size;
        operation->attributes = attributes_size ? malloc((size_t)attributes_size) : NULL;
        if ((attributes_size && !operation->attributes) || get_bytes(file, operation->attributes, (size_t)attributes_size) ||
            !operation->output || operation->output > graph->value_count)
            return -1;
        graph->values[operation->output - 1].producer = index;
    }
    return 0;
}

oasp_ai_graph *oasp_ai_graph_load(oasp_ai_context *context, const char *path, oasp_ai_error *error) {
    FILE *file = context && path ? fopen(path, "rb") : NULL;
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file)
            fclose(file);
        oasp_ai_set_error(error, 2, "LoadModel", "unable to open model", "Check the model path and permissions.");
        return NULL;
    }
    long actual_size = ftell(file);
    if (actual_size < (long)OAIR_HEADER_SIZE || (uint64_t)actual_size > context->options.maximum_model_bytes ||
        fseek(file, 0, SEEK_SET)) {
        fclose(file);
        oasp_ai_set_error(error, 5, "LoadModel", "model size is invalid", "Use a model within MaximumModelBytes.");
        return NULL;
    }
    char magic[8];
    uint16_t major;
    uint16_t minor;
    uint16_t operator_major;
    uint16_t operator_minor;
    uint32_t endian;
    uint32_t flags;
    uint32_t section_count;
    uint32_t reserved;
    uint64_t footer_offset;
    uint64_t declared_size;
    unsigned char ignored[24];
    if (get_bytes(file, magic, sizeof(magic)) || memcmp(magic, "OASPMDL2", 8) || get_u16(file, &major) || get_u16(file, &minor) ||
        major != 2 || get_u16(file, &operator_major) || get_u16(file, &operator_minor) ||
        operator_major > OASP_AI_OPERATOR_SET_MAJOR || get_u32(file, &endian) || endian != UINT32_C(0x01020304) ||
        get_u32(file, &flags) || get_u32(file, &section_count) || !section_count || section_count > OAIR_MAX_SECTIONS ||
        get_u32(file, &reserved) || get_u64(file, &footer_offset) || get_u64(file, &declared_size) ||
        get_bytes(file, ignored, sizeof(ignored)) || declared_size != (uint64_t)actual_size ||
        footer_offset > declared_size - 16 - (uint64_t)section_count * OAIR_SECTION_ENTRY_SIZE ||
        footer_offset > LONG_MAX || fseek(file, (long)footer_offset, SEEK_SET)) {
        fclose(file);
        oasp_ai_set_error(error, 5, "LoadModel", "invalid OAIR header", "Use a compatible OpenASP model.");
        return NULL;
    }
    (void)minor;
    (void)operator_minor;
    (void)flags;
    (void)reserved;
    char index_magic[8];
    uint32_t indexed_count;
    if (get_bytes(file, index_magic, 8) || memcmp(index_magic, "OAIRIDX2", 8) || get_u32(file, &indexed_count) ||
        indexed_count != section_count || get_u32(file, &reserved)) {
        fclose(file);
        return NULL;
    }
    oair_section *sections = calloc(section_count, sizeof(*sections));
    int failed = !sections;
    for (uint32_t index = 0; index < section_count && !failed; index++) {
        uint64_t entry_reserved;
        failed = get_u32(file, &sections[index].type) || get_u32(file, &sections[index].flags) ||
                 get_u64(file, &sections[index].offset) || get_u64(file, &sections[index].length) ||
                 get_bytes(file, sections[index].sha256, 32) || get_u64(file, &entry_reserved) ||
                 sections[index].offset < OAIR_HEADER_SIZE || sections[index].offset > footer_offset ||
                 sections[index].length > footer_offset - sections[index].offset;
        for (uint32_t previous = 0; previous < index && !failed; previous++)
            failed = sections[previous].type == sections[index].type;
    }
    for (uint32_t index = 0; index < section_count && !failed; index++) {
        unsigned char digest[32];
        failed = digest_section(file, &sections[index], digest) || memcmp(digest, sections[index].sha256, sizeof(digest));
        if (!failed && sections[index].type > OAIR_SECTION_CACHE && (sections[index].flags & OAIR_REQUIRED))
            failed = 1;
    }
    const uint32_t required_types[] = {OAIR_SECTION_MANIFEST, OAIR_SECTION_SIGNATURES, OAIR_SECTION_VALUES,
                                       OAIR_SECTION_OPERATIONS, OAIR_SECTION_TENSOR_DATA};
    for (size_t index = 0; index < sizeof(required_types) / sizeof(required_types[0]) && !failed; index++)
        failed = !find_section(sections, section_count, required_types[index]);
    char *name = NULL;
    if (!failed) {
        failed = seek_section(file, sections, section_count, OAIR_SECTION_MANIFEST);
        name = failed ? NULL : get_string(file, 4096);
        uint32_t runtime_major;
        uint32_t runtime_minor;
        uint32_t op_major;
        uint32_t op_minor;
        char *backend = NULL;
        uint32_t dependency_count;
        failed = !name || get_u32(file, &runtime_major) || get_u32(file, &runtime_minor) || runtime_major > OASP_AI_VERSION_MAJOR ||
                 get_u32(file, &op_major) || get_u32(file, &op_minor) || op_major > OASP_AI_OPERATOR_SET_MAJOR ||
                 !(backend = get_string(file, 4096)) || get_u32(file, &dependency_count) || dependency_count;
        free(backend);
        (void)runtime_minor;
        (void)op_minor;
    }
    oasp_ai_graph *graph = failed ? NULL : oasp_ai_graph_create(context, name, error);
    free(name);
    uint64_t *offsets = NULL;
    uint64_t *lengths = NULL;
    oair_quantization *quantization = NULL;
    if (!graph || load_values(file, sections, section_count, graph, &offsets, &lengths, &quantization, error))
        failed = 1;
    if (!failed) {
        const oair_section *tensor_section = find_section(sections, section_count, OAIR_SECTION_TENSOR_DATA);
        for (size_t index = 0; index < graph->value_count && !failed; index++) {
            if (!lengths[index])
                continue;
            uint64_t expected = 0;
            if (graph->values[index].desc.shape[0] != -1 &&
                (!oasp_ai_checked_elements(graph->values[index].desc.shape, graph->values[index].desc.rank, &expected, error))) {
                size_t element_size = oasp_ai_dtype_size(graph->values[index].desc.dtype);
                failed = expected > UINT64_MAX / element_size || expected * element_size != lengths[index];
            } else
                failed = 1;
            failed = failed || offsets[index] > tensor_section->length || lengths[index] > tensor_section->length - offsets[index] ||
                     tensor_section->offset + offsets[index] > LONG_MAX ||
                     fseek(file, (long)(tensor_section->offset + offsets[index]), SEEK_SET);
            void *data = !failed ? malloc((size_t)lengths[index]) : NULL;
            if (!failed)
                failed = !data || get_bytes(file, data, (size_t)lengths[index]);
            if (!failed)
                graph->values[index].constant =
                    oasp_ai_tensor_create(context, &graph->values[index].desc, data, (size_t)lengths[index], error);
            free(data);
            failed = failed || !graph->values[index].constant;
            if (!failed && quantization[index].bits) {
                oasp_ai_tensor *tensor = graph->values[index].constant;
                tensor->quantized_bits = quantization[index].bits;
                tensor->quantized_axis = quantization[index].axis;
                tensor->quantized_scale = quantization[index].scale;
                tensor->quantized_zero_point = quantization[index].zero_point;
                tensor->quantized_channel_scales = quantization[index].channel_scales;
                tensor->quantized_channel_count = quantization[index].channel_count;
                memcpy(tensor->quantized_calibration, quantization[index].calibration,
                       sizeof(tensor->quantized_calibration));
                quantization[index].channel_scales = NULL;
            }
        }
    }
    if (!failed)
        failed = load_operations(file, sections, section_count, graph);
    if (!failed) {
        uint32_t input_count;
        uint32_t output_count;
        failed = seek_section(file, sections, section_count, OAIR_SECTION_SIGNATURES) || get_u32(file, &input_count) ||
                 get_u32(file, &output_count) || input_count > graph->value_count || output_count > graph->value_count;
        graph->inputs = !failed && input_count ? malloc((size_t)input_count * sizeof(*graph->inputs)) : NULL;
        graph->outputs = !failed && output_count ? malloc((size_t)output_count * sizeof(*graph->outputs)) : NULL;
        failed = failed || (input_count && !graph->inputs) || (output_count && !graph->outputs);
        graph->input_count = graph->input_capacity = failed ? 0 : input_count;
        graph->output_count = failed ? 0 : output_count;
        for (uint32_t group = 0; group < 2 && !failed; group++) {
            uint32_t *items = group ? graph->outputs : graph->inputs;
            uint32_t count = group ? output_count : input_count;
            for (uint32_t index = 0; index < count && !failed; index++)
                failed = get_u32(file, &items[index]) || !items[index] || items[index] > graph->value_count;
        }
    }
    free(offsets);
    free(lengths);
    free_quantization(quantization, graph ? graph->value_count : 0);
    free(sections);
    fclose(file);
    if (!failed && !oasp_ai_graph_verify(graph, error))
        return graph;
    oasp_ai_graph_release(graph);
    oasp_ai_set_error(error, 5, "LoadModel", "OAIR sections are invalid, unsupported, or corrupted",
                      "Restore a trusted compatible model and verify its SHA-256 section checksums.");
    return NULL;
}
