// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    WEIGHTS_VERSION = 2,
    WEIGHTS_MAX_TENSORS = 1000000,
    WEIGHTS_MAX_NAME = 4096,
    WEIGHTS_MAX_CALIBRATION = 31,
    WEIGHTS_LAYOUT_ROW_MAJOR = 1
};

typedef struct {
    oasp_ai_state_entry *entry;
    oasp_ai_tensor *contiguous;
    uint64_t offset;
    uint64_t length;
    uint32_t checksum;
} weight_snapshot;

static int write_bytes(FILE *file, const void *data, size_t size) {
    return size && fwrite(data, 1, size, file) != size ? -1 : 0;
}

static int read_bytes(FILE *file, void *data, size_t size) {
    return size && fread(data, 1, size, file) != size ? -1 : 0;
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char data[] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return write_bytes(file, data, sizeof(data));
}

static int write_u64(FILE *file, uint64_t value) {
    unsigned char data[8];
    for (unsigned index = 0; index < 8; index++)
        data[index] = (unsigned char)(value >> (index * 8));
    return write_bytes(file, data, sizeof(data));
}

static int read_u32(FILE *file, uint32_t *value) {
    unsigned char data[4];
    if (read_bytes(file, data, sizeof(data)))
        return -1;
    *value = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    return 0;
}

static int read_u64(FILE *file, uint64_t *value) {
    unsigned char data[8];
    if (read_bytes(file, data, sizeof(data)))
        return -1;
    *value = 0;
    for (unsigned index = 0; index < 8; index++)
        *value |= (uint64_t)data[index] << (index * 8);
    return 0;
}

static int write_f64(FILE *file, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return write_u64(file, bits);
}

static int read_f64(FILE *file, double *value) {
    uint64_t bits;
    if (read_u64(file, &bits))
        return -1;
    memcpy(value, &bits, sizeof(*value));
    return 0;
}

static int write_string(FILE *file, const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length > UINT32_MAX || write_u32(file, (uint32_t)length) || write_bytes(file, value, length) ? -1 : 0;
}

static char *read_string(FILE *file, uint32_t maximum) {
    uint32_t length;
    if (read_u32(file, &length) || length > maximum)
        return NULL;
    char *value = malloc((size_t)length + 1);
    if (!value || read_bytes(file, value, length)) {
        free(value);
        return NULL;
    }
    value[length] = '\0';
    return value;
}

static int add_u64(uint64_t *value, uint64_t amount) {
    if (*value > UINT64_MAX - amount)
        return -1;
    *value += amount;
    return 0;
}

static int snapshot_index_size(const weight_snapshot *snapshot, uint64_t *size) {
    const oasp_ai_tensor *tensor = snapshot->entry->tensor;
    oasp_ai_quantization_info quantization = {0};
    oasp_ai_tensor_quantization(tensor, &quantization);
    size_t name_length = strlen(snapshot->entry->name);
    size_t calibration_length = quantization.enabled ? strlen(quantization.calibration_method) : 0;
    uint64_t fixed = 4 + name_length + 4 * 4 + (uint64_t)tensor->desc.rank * 16 + 8 + 8 + 4 +
                     4 + 4 + 8 + 8 + 4 + calibration_length + 8;
    if (quantization.channel_count > UINT64_MAX / 8 || add_u64(&fixed, (uint64_t)quantization.channel_count * 8))
        return -1;
    return add_u64(size, fixed);
}

static void release_snapshots(weight_snapshot *snapshots, size_t count) {
    for (size_t index = 0; index < count; index++)
        oasp_ai_tensor_release(snapshots[index].contiguous);
    free(snapshots);
}

typedef struct {
    oasp_ai_module *module;
} weights_writer;

static int write_weights(FILE *file, void *opaque, oasp_ai_error *error) {
    oasp_ai_module *module = ((weights_writer *)opaque)->module;
    oasp_ai_state_dict *state = oasp_ai_module_state_dict(module, error);
    if (!state || state->count > UINT32_MAX) {
        oasp_ai_state_dict_release(state);
        return -1;
    }
    weight_snapshot *snapshots = state->count ? calloc(state->count, sizeof(*snapshots)) : NULL;
    uint64_t data_offset = 8 + 4 + 4 + 8;
    int failed = state->count && !snapshots;
    for (size_t index = 0; index < state->count && !failed; index++) {
        snapshots[index].entry = &state->entries[index];
        oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(state->entries[index].tensor, error);
        snapshots[index].contiguous = contiguous ? oasp_ai_tensor_to_device(contiguous, "cpu:0", error) : NULL;
        oasp_ai_tensor_release(contiguous);
        snapshots[index].length = oasp_ai_tensor_bytes(state->entries[index].tensor);
        failed = !snapshots[index].contiguous || snapshot_index_size(&snapshots[index], &data_offset);
    }
    for (size_t index = 0; index < state->count && !failed; index++) {
        snapshots[index].offset = data_offset;
        snapshots[index].checksum = oasp_ai_crc32(oasp_ai_tensor_const_data(snapshots[index].contiguous),
                                                  (size_t)snapshots[index].length);
        failed = add_u64(&data_offset, snapshots[index].length);
    }
    if (!failed)
        failed = write_bytes(file, "OASPWGT2", 8) || write_u32(file, WEIGHTS_VERSION) ||
                 write_u32(file, (uint32_t)state->count) || write_u64(file, oasp_ai_module_schema_hash(module));
    for (size_t index = 0; index < state->count && !failed; index++) {
        oasp_ai_state_entry *entry = snapshots[index].entry;
        oasp_ai_tensor *tensor = entry->tensor;
        oasp_ai_quantization_info quantization = {0};
        oasp_ai_tensor_quantization(tensor, &quantization);
        failed = write_string(file, entry->name) || write_u32(file, entry->kind) ||
                 write_u32(file, tensor->desc.dtype) || write_u32(file, tensor->desc.rank) ||
                 write_u32(file, WEIGHTS_LAYOUT_ROW_MAJOR);
        for (uint32_t axis = 0; axis < tensor->desc.rank && !failed; axis++)
            failed = write_u64(file, (uint64_t)tensor->desc.shape[axis]);
        for (uint32_t axis = 0; axis < tensor->desc.rank && !failed; axis++)
            failed = write_u64(file, (uint64_t)snapshots[index].contiguous->desc.strides[axis]);
        if (!failed)
            failed = write_u64(file, snapshots[index].offset) || write_u64(file, snapshots[index].length) ||
                     write_u32(file, snapshots[index].checksum) ||
                     write_u32(file, quantization.enabled ? quantization.bits : 0) ||
                     write_u32(file, quantization.enabled ? (uint32_t)quantization.axis : 0) ||
                     write_f64(file, quantization.enabled ? quantization.scale : 0) ||
                     write_u64(file, quantization.enabled ? (uint64_t)quantization.zero_point : 0) ||
                     write_string(file, quantization.enabled ? quantization.calibration_method : "") ||
                     write_u64(file, quantization.enabled ? (uint64_t)quantization.channel_count : 0);
        for (size_t channel = 0; channel < quantization.channel_count && !failed; channel++)
            failed = write_f64(file, quantization.channel_scales[channel]);
    }
    unsigned char chunk[65536];
    for (size_t index = 0; index < state->count && !failed; index++) {
        const unsigned char *data = oasp_ai_tensor_const_data(snapshots[index].contiguous);
        for (uint64_t position = 0; position < snapshots[index].length && !failed;) {
            size_t amount = snapshots[index].length - position < sizeof(chunk)
                                ? (size_t)(snapshots[index].length - position)
                                : sizeof(chunk);
            memcpy(chunk, data + position, amount);
            failed = write_bytes(file, chunk, amount);
            position += amount;
        }
    }
    release_snapshots(snapshots, state->count);
    oasp_ai_state_dict_release(state);
    if (failed)
        oasp_ai_set_error(error, 5, "SaveWeights", "unable to serialize StateDict",
                          "Check tensor metadata, parameter consistency, and available disk space.");
    return failed ? -1 : 0;
}

int oasp_ai_module_save_weights(oasp_ai_module *module, const char *path, oasp_ai_error *error) {
    if (!module || !path) {
        oasp_ai_set_error(error, 5, "SaveWeights", "module and path are required", "Pass a valid Module and output path.");
        return -1;
    }
    weights_writer writer = {module};
    return oasp_ai_write_atomic(path, write_weights, &writer, error);
}

static int set_quantization(oasp_ai_tensor *tensor, uint32_t bits, uint32_t axis, double scale, uint64_t zero_point,
                            const char *calibration, double *channel_scales, uint64_t channel_count) {
    if (!bits) {
        if (axis || scale || zero_point || *calibration || channel_count)
            return -1;
        return 0;
    }
    int32_t signed_axis = (int32_t)axis;
    if ((bits != 4 && bits != 8) || signed_axis < -1 || signed_axis >= (int32_t)tensor->desc.rank || !(scale > 0) ||
        channel_count > SIZE_MAX / sizeof(*channel_scales) ||
        (channel_count && (signed_axis < 0 || channel_count != (uint64_t)tensor->desc.shape[signed_axis])))
        return -1;
    tensor->quantized_bits = bits;
    tensor->quantized_axis = signed_axis;
    tensor->quantized_scale = scale;
    tensor->quantized_zero_point = (int64_t)zero_point;
    tensor->quantized_channel_scales = channel_scales;
    tensor->quantized_channel_count = (size_t)channel_count;
    memcpy(tensor->quantized_calibration, calibration, strlen(calibration) + 1);
    return 0;
}

static int read_weight_entry(FILE *file, oasp_ai_context *context, uint64_t file_size, oasp_ai_state_entry *entry, uint64_t *offset,
                             uint64_t *length, uint32_t *checksum, oasp_ai_error *error) {
    uint32_t kind;
    uint32_t dtype;
    uint32_t rank;
    uint32_t layout;
    entry->name = read_string(file, WEIGHTS_MAX_NAME);
    if (!entry->name || read_u32(file, &kind) || kind > OASP_AI_STATE_BUFFER ||
        read_u32(file, &dtype) || dtype > OASP_AI_DTYPE_F64 || read_u32(file, &rank) ||
        !rank || rank > OASP_AI_MAX_RANK || read_u32(file, &layout) || layout != WEIGHTS_LAYOUT_ROW_MAJOR)
        return -1;
    oasp_ai_tensor_desc desc = {.dtype = (oasp_ai_dtype)dtype, .rank = rank};
    for (uint32_t axis = 0; axis < rank; axis++) {
        uint64_t dimension;
        if (read_u64(file, &dimension) || !dimension || dimension > INT64_MAX)
            return -1;
        desc.shape[axis] = (int64_t)dimension;
    }
    for (uint32_t axis = 0; axis < rank; axis++) {
        uint64_t stride;
        if (read_u64(file, &stride) || stride > INT64_MAX)
            return -1;
        desc.strides[axis] = (int64_t)stride;
    }
    uint32_t bits;
    uint32_t quantized_axis;
    double scale;
    uint64_t zero_point;
    uint64_t channel_count;
    char *calibration = NULL;
    if (read_u64(file, offset) || read_u64(file, length) || read_u32(file, checksum) ||
        read_u32(file, &bits) || read_u32(file, &quantized_axis) || read_f64(file, &scale) ||
        read_u64(file, &zero_point) || !(calibration = read_string(file, WEIGHTS_MAX_CALIBRATION)) ||
        read_u64(file, &channel_count) || channel_count > UINT32_MAX ||
        *offset > file_size || *length > file_size - *offset || *length > context->options.maximum_tensor_bytes ||
        *length > SIZE_MAX) {
        free(calibration);
        return -1;
    }
    double *channel_scales = channel_count ? malloc((size_t)channel_count * sizeof(*channel_scales)) : NULL;
    int failed = channel_count && !channel_scales;
    for (uint64_t channel = 0; channel < channel_count && !failed; channel++)
        failed = read_f64(file, &channel_scales[channel]);
    uint64_t elements;
    if (!failed)
        failed = oasp_ai_checked_elements(desc.shape, desc.rank, &elements, error) ||
                 elements > UINT64_MAX / oasp_ai_dtype_size(desc.dtype) ||
                 elements * oasp_ai_dtype_size(desc.dtype) != *length;
    long resume = ftell(file);
    void *data = !failed ? malloc((size_t)*length) : NULL;
    if (!failed)
        failed = !data || *offset > LONG_MAX || fseek(file, (long)*offset, SEEK_SET) ||
                 read_bytes(file, data, (size_t)*length) || oasp_ai_crc32(data, (size_t)*length) != *checksum ||
                 fseek(file, resume, SEEK_SET);
    entry->tensor = failed ? NULL : oasp_ai_tensor_create(context, &desc, data, (size_t)*length, error);
    if (!failed && (!entry->tensor || set_quantization(entry->tensor, bits, quantized_axis, scale, zero_point,
                                                       calibration, channel_scales, channel_count)))
        failed = 1;
    if (!failed)
        channel_scales = NULL;
    free(channel_scales);
    free(calibration);
    free(data);
    entry->kind = (oasp_ai_state_kind)kind;
    return failed ? -1 : 0;
}

int oasp_ai_module_load_weights(oasp_ai_module *module, const char *path, int strict, oasp_ai_error *error) {
    FILE *file = module && path ? fopen(path, "rb") : NULL;
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file)
            fclose(file);
        oasp_ai_set_error(error, 2, "LoadWeights", "unable to open weights file", "Check the model path and permissions.");
        return -1;
    }
    long end = ftell(file);
    uint64_t file_size = end < 0 ? 0 : (uint64_t)end;
    if (end < 24 || file_size > module->context->options.maximum_model_bytes || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        oasp_ai_set_error(error, 5, "LoadWeights", "weights file size is invalid", "Use a valid file within MaximumModelBytes.");
        return -1;
    }
    char magic[8];
    uint32_t version = 0;
    uint32_t count = 0;
    uint64_t schema_hash = 0;
    int failed = read_bytes(file, magic, sizeof(magic)) || memcmp(magic, "OASPWGT2", 8) ||
                 read_u32(file, &version) || version != WEIGHTS_VERSION || read_u32(file, &count) ||
                 count > WEIGHTS_MAX_TENSORS || read_u64(file, &schema_hash) ||
                 (strict && schema_hash != oasp_ai_module_schema_hash(module));
    oasp_ai_state_dict *state = !failed ? calloc(1, sizeof(*state)) : NULL;
    uint64_t *offsets = count ? calloc(count, sizeof(*offsets)) : NULL;
    uint64_t *lengths = count ? calloc(count, sizeof(*lengths)) : NULL;
    uint32_t *checksums = count ? calloc(count, sizeof(*checksums)) : NULL;
    if (!failed && (!state || (count && (!offsets || !lengths || !checksums))))
        failed = 1;
    if (state) {
        atomic_init(&state->references, 1);
        state->entries = count ? calloc(count, sizeof(*state->entries)) : NULL;
        if (count && !state->entries)
            failed = 1;
    }
    for (uint32_t index = 0; index < count && !failed; index++) {
        failed = read_weight_entry(file, module->context, file_size, &state->entries[index],
                                   &offsets[index], &lengths[index], &checksums[index], error);
        if (!failed) {
            for (uint32_t previous = 0; previous < index && !failed; previous++)
                failed = !strcmp(state->entries[previous].name, state->entries[index].name) ||
                         (offsets[index] < offsets[previous] + lengths[previous] &&
                          offsets[previous] < offsets[index] + lengths[index]);
            state->count++;
        }
        if (failed) {
            free(state->entries[index].name);
            oasp_ai_tensor_release(state->entries[index].tensor);
            memset(&state->entries[index], 0, sizeof(state->entries[index]));
        }
    }
    oasp_ai_state_diagnostics diagnostics = {0};
    int status = failed ? -1 : oasp_ai_module_load_state_dict(module, state, strict, &diagnostics, error);
    oasp_ai_state_diagnostics_free(&diagnostics);
    oasp_ai_state_dict_release(state);
    free(offsets);
    free(lengths);
    free(checksums);
    if (fclose(file))
        status = -1;
    if (failed) {
        oasp_ai_set_error(error, 5, "LoadWeights", "weights index, tensor data, or checksum is invalid",
                          "Use an intact OpenASP .oaspweights version 2 file built for this module.");
        return -1;
    }
    return status;
}
