#define _POSIX_C_SOURCE 200809L

// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

enum {
    CHECKPOINT_VERSION = 3,
    CHECKPOINT_MAX_TENSORS = 1000000,
    CHECKPOINT_MAX_NAME = 4096,
    CHECKPOINT_DIGEST_SIZE = 32
};

typedef struct {
    EVP_MD_CTX *digest;
    FILE *file;
} checkpoint_stream;

static int stream_write(checkpoint_stream *stream, const void *data, size_t size) {
    return (size && fwrite(data, 1, size, stream->file) != size) ||
                   (size && EVP_DigestUpdate(stream->digest, data, size) != 1)
               ? -1
               : 0;
}

static int stream_u32(checkpoint_stream *stream, uint32_t value) {
    unsigned char bytes[] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return stream_write(stream, bytes, sizeof(bytes));
}

static int stream_u64(checkpoint_stream *stream, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned index = 0; index < 8; index++)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return stream_write(stream, bytes, sizeof(bytes));
}

static int stream_f64(checkpoint_stream *stream, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return stream_u64(stream, bits);
}

static int stream_string(checkpoint_stream *stream, const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length > UINT32_MAX || stream_u32(stream, (uint32_t)length) || stream_write(stream, value, length) ? -1 : 0;
}

static int stream_tensor(checkpoint_stream *stream, const char *name, oasp_ai_tensor *tensor, oasp_ai_error *error) {
    oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(tensor, error);
    if (!contiguous)
        return -1;
    uint64_t bytes = oasp_ai_tensor_bytes(contiguous);
    int status = stream_string(stream, name) || stream_u32(stream, tensor->desc.dtype) || stream_u32(stream, tensor->desc.rank);
    for (uint32_t axis = 0; axis < tensor->desc.rank && !status; axis++)
        status = stream_u64(stream, (uint64_t)tensor->desc.shape[axis]);
    oasp_ai_quantization_info quantization = {0};
    oasp_ai_tensor_quantization(tensor, &quantization);
    if (!status)
        status = stream_u32(stream, quantization.enabled ? quantization.bits : 0) ||
                 stream_u32(stream, quantization.enabled ? (uint32_t)quantization.axis : 0) ||
                 stream_f64(stream, quantization.enabled ? quantization.scale : 0) ||
                 stream_u64(stream, quantization.enabled ? (uint64_t)quantization.zero_point : 0) ||
                 stream_string(stream, quantization.enabled ? quantization.calibration_method : "") ||
                 stream_u32(stream, quantization.enabled ? (uint32_t)quantization.channel_count : 0);
    for (size_t index = 0; index < quantization.channel_count && !status; index++)
        status = stream_f64(stream, quantization.channel_scales[index]);
    if (!status)
        status = stream_u64(stream, bytes);
    const unsigned char *data = oasp_ai_tensor_const_data(contiguous);
    for (uint64_t position = 0; position < bytes && !status;) {
        size_t amount = bytes - position < 65536 ? (size_t)(bytes - position) : 65536;
        status = stream_write(stream, data + position, amount);
        position += amount;
    }
    oasp_ai_tensor_release(contiguous);
    return status;
}

typedef struct {
    oasp_ai_module *module;
    oasp_ai_optimizer *optimizer;
    const oasp_ai_checkpoint_state *state;
} checkpoint_writer;

static int save_checkpoint_stream(FILE *file, void *opaque, oasp_ai_error *error) {
    checkpoint_writer *writer = opaque;
    const oasp_ai_checkpoint_state empty = {0};
    const oasp_ai_checkpoint_state *state = writer->state ? writer->state : &empty;
    oasp_ai_state_dict *dictionary = oasp_ai_module_state_dict(writer->module, error);
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    checkpoint_stream stream = {digest, file};
    if (!dictionary || !digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1)
        goto failure;
    uint32_t flags = (writer->optimizer ? 1u : 0u) | (state->scheduler ? 2u : 0u) | (state->loss_scaler ? 4u : 0u) |
                     (state->data_iterator ? 8u : 0u);
    if (stream_write(&stream, "OASPCKP3", 8) || stream_u32(&stream, CHECKPOINT_VERSION) || stream_u32(&stream, flags) ||
        stream_u32(&stream, OASP_AI_VERSION_MAJOR) || stream_u32(&stream, OASP_AI_VERSION_MINOR) ||
        stream_u32(&stream, OASP_AI_OPERATOR_SET_MAJOR) || stream_u32(&stream, OASP_AI_OPERATOR_SET_MINOR) ||
        stream_u64(&stream, oasp_ai_module_schema_hash(writer->module)) ||
        stream_u64(&stream, writer->module->context->rng_counter) || stream_u64(&stream, state->epoch) ||
        stream_u64(&stream, state->global_step) || stream_u64(&stream, state->metadata_size) ||
        stream_write(&stream, state->metadata, state->metadata_size) ||
        stream_u32(&stream, (uint32_t)dictionary->count))
        goto failure;
    for (size_t index = 0; index < dictionary->count; index++)
        if (stream_u32(&stream, dictionary->entries[index].kind) ||
            stream_tensor(&stream, dictionary->entries[index].name, dictionary->entries[index].tensor, error))
            goto failure;
    oasp_ai_optimizer *optimizer = writer->optimizer;
    if (optimizer) {
        if (stream_u32(&stream, optimizer->kind) || stream_u64(&stream, optimizer->step) ||
            stream_f64(&stream, optimizer->learning_rate) || stream_f64(&stream, optimizer->weight_decay) ||
            stream_f64(&stream, optimizer->momentum) || stream_f64(&stream, optimizer->beta1) ||
            stream_f64(&stream, optimizer->beta2) || stream_f64(&stream, optimizer->epsilon) ||
            stream_u32(&stream, (uint32_t)optimizer->parameter_count))
            goto failure;
        for (size_t index = 0; index < optimizer->parameter_count; index++) {
            const char *name = NULL;
            oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, &name);
            int status = !parameter || stream_string(&stream, name) || stream_u32(&stream, optimizer->first_moment[index] != NULL);
            if (!status && optimizer->first_moment[index])
                status = stream_tensor(&stream, "", optimizer->first_moment[index], error);
            if (!status)
                status = stream_u32(&stream, optimizer->second_moment[index] != NULL);
            if (!status && optimizer->second_moment[index])
                status = stream_tensor(&stream, "", optimizer->second_moment[index], error);
            oasp_ai_tensor_release(parameter);
            if (status)
                goto failure;
        }
    }
    if (state->scheduler &&
        (stream_u32(&stream, state->scheduler->kind) || stream_u64(&stream, state->scheduler->total_steps) ||
         stream_u64(&stream, state->scheduler->warmup_or_step_size) || stream_u64(&stream, state->scheduler->step) ||
         stream_f64(&stream, state->scheduler->initial_learning_rate) || stream_f64(&stream, state->scheduler->factor_or_minimum)))
        goto failure;
    if (state->loss_scaler &&
        (stream_f64(&stream, state->loss_scaler->scale) || stream_f64(&stream, state->loss_scaler->growth_factor) ||
         stream_f64(&stream, state->loss_scaler->backoff_factor) || stream_u32(&stream, state->loss_scaler->growth_interval) ||
         stream_u32(&stream, state->loss_scaler->finite_steps)))
        goto failure;
    if (state->data_iterator &&
        (stream_u64(&stream, state->data_iterator->epoch) || stream_u64(&stream, state->data_iterator->cursor) ||
         stream_u64(&stream, state->data_iterator->loader->options.seed) ||
         stream_u32(&stream, state->data_iterator->loader->options.shuffle != 0)))
        goto failure;
    unsigned char checksum[CHECKPOINT_DIGEST_SIZE];
    unsigned checksum_size = 0;
    if (EVP_DigestFinal_ex(digest, checksum, &checksum_size) != 1 || checksum_size != sizeof(checksum) ||
        fwrite(checksum, 1, sizeof(checksum), file) != sizeof(checksum))
        goto failure;
    EVP_MD_CTX_free(digest);
    oasp_ai_state_dict_release(dictionary);
    return 0;

failure:
    EVP_MD_CTX_free(digest);
    oasp_ai_state_dict_release(dictionary);
    if (!error || !error->message)
        oasp_ai_set_error(error, 5, "SaveCheckpoint", "unable to serialize training state", "Check state consistency and disk capacity.");
    return -1;
}

int oasp_ai_save_checkpoint_full(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, const oasp_ai_checkpoint_state *state, oasp_ai_error *error) {
    if (!module || (state && state->metadata_size && !state->metadata) ||
        (state && state->scheduler && state->scheduler->optimizer != optimizer)) {
        oasp_ai_set_error(error, 5, "SaveCheckpoint", "checkpoint objects are inconsistent",
                          "Use scheduler and optimizer objects from the same Module.");
        return -1;
    }
    checkpoint_writer writer = {module, optimizer, state};
    return oasp_ai_write_atomic(path, save_checkpoint_stream, &writer, error);
}

typedef struct {
    FILE *file;
    uint64_t remaining;
} checkpoint_reader;

static int reader_bytes(checkpoint_reader *reader, void *data, size_t size) {
    if (size > reader->remaining)
        return -1;
    unsigned char discard[65536];
    size_t pending = size;
    while (pending) {
        size_t amount = pending < sizeof(discard) ? pending : sizeof(discard);
        void *destination = data ? (unsigned char *)data + size - pending : discard;
        if (fread(destination, 1, amount, reader->file) != amount)
            return -1;
        pending -= amount;
    }
    reader->remaining -= size;
    return 0;
}

static int reader_u32(checkpoint_reader *reader, uint32_t *value) {
    unsigned char bytes[4];
    if (reader_bytes(reader, bytes, sizeof(bytes)))
        return -1;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 0;
}

static int reader_u64(checkpoint_reader *reader, uint64_t *value) {
    unsigned char bytes[8];
    if (reader_bytes(reader, bytes, sizeof(bytes)))
        return -1;
    *value = 0;
    for (unsigned index = 0; index < 8; index++)
        *value |= (uint64_t)bytes[index] << (index * 8);
    return 0;
}

static int reader_f64(checkpoint_reader *reader, double *value) {
    uint64_t bits;
    if (reader_u64(reader, &bits))
        return -1;
    memcpy(value, &bits, sizeof(*value));
    return 0;
}

static char *reader_string(checkpoint_reader *reader, uint32_t maximum) {
    uint32_t length;
    if (reader_u32(reader, &length) || length > maximum || length > reader->remaining)
        return NULL;
    char *value = malloc((size_t)length + 1);
    if (!value)
        return NULL;
    if (reader_bytes(reader, value, length)) {
        free(value);
        return NULL;
    }
    value[length] = '\0';
    return value;
}

static oasp_ai_tensor *reader_tensor(checkpoint_reader *reader, oasp_ai_context *context, char **name, oasp_ai_error *error) {
    *name = reader_string(reader, CHECKPOINT_MAX_NAME);
    uint32_t dtype;
    uint32_t rank;
    oasp_ai_tensor_desc desc = {0};
    if (!*name || reader_u32(reader, &dtype) || dtype > OASP_AI_DTYPE_F64 || reader_u32(reader, &rank) ||
        !rank || rank > OASP_AI_MAX_RANK)
        return NULL;
    desc.dtype = (oasp_ai_dtype)dtype;
    desc.rank = rank;
    for (uint32_t axis = 0; axis < rank; axis++) {
        uint64_t dimension;
        if (reader_u64(reader, &dimension) || !dimension || dimension > INT64_MAX)
            return NULL;
        desc.shape[axis] = (int64_t)dimension;
    }
    uint32_t bits;
    uint32_t axis;
    double scale;
    uint64_t zero_point;
    uint32_t channel_count;
    char *calibration = NULL;
    if (reader_u32(reader, &bits) || reader_u32(reader, &axis) || reader_f64(reader, &scale) ||
        reader_u64(reader, &zero_point) || !(calibration = reader_string(reader, 31)) ||
        reader_u32(reader, &channel_count) || channel_count > UINT32_C(1000000)) {
        free(calibration);
        return NULL;
    }
    double *channel_scales = channel_count ? malloc((size_t)channel_count * sizeof(*channel_scales)) : NULL;
    int failed = channel_count && !channel_scales;
    for (uint32_t index = 0; index < channel_count && !failed; index++)
        failed = reader_f64(reader, &channel_scales[index]);
    uint64_t bytes;
    uint64_t elements;
    if (!failed)
        failed = reader_u64(reader, &bytes) || bytes > context->options.maximum_tensor_bytes || bytes > SIZE_MAX ||
                 oasp_ai_checked_elements(desc.shape, desc.rank, &elements, error) ||
                 elements > UINT64_MAX / oasp_ai_dtype_size(desc.dtype) || elements * oasp_ai_dtype_size(desc.dtype) != bytes ||
                 bytes > reader->remaining;
    void *data = !failed ? malloc((size_t)bytes) : NULL;
    if (!failed)
        failed = !data || reader_bytes(reader, data, (size_t)bytes);
    oasp_ai_tensor *tensor = failed ? NULL : oasp_ai_tensor_create(context, &desc, data, (size_t)bytes, error);
    free(data);
    if (tensor && bits) {
        int32_t signed_axis = (int32_t)axis;
        if ((bits != 4 && bits != 8) || signed_axis < -1 || signed_axis >= (int32_t)rank || !(scale > 0) ||
            (channel_count && (signed_axis < 0 || channel_count != (uint32_t)desc.shape[signed_axis]))) {
            oasp_ai_tensor_release(tensor);
            tensor = NULL;
        } else {
            tensor->quantized_bits = bits;
            tensor->quantized_axis = signed_axis;
            tensor->quantized_scale = scale;
            tensor->quantized_zero_point = (int64_t)zero_point;
            tensor->quantized_channel_scales = channel_scales;
            tensor->quantized_channel_count = channel_count;
            channel_scales = NULL;
            memcpy(tensor->quantized_calibration, calibration, strlen(calibration) + 1);
        }
    }
    free(channel_scales);
    free(calibration);
    return tensor;
}

static ssize_t parameter_index(oasp_ai_module *module, const char *name) {
    size_t count = oasp_ai_module_parameter_count(module);
    for (size_t index = 0; index < count; index++) {
        const char *candidate = NULL;
        oasp_ai_tensor *tensor = oasp_ai_module_parameter(module, index, &candidate);
        int match = candidate && !strcmp(candidate, name);
        oasp_ai_tensor_release(tensor);
        if (match)
            return (ssize_t)index;
    }
    return -1;
}

typedef struct {
    uint64_t step;
    double learning_rate;
    double weight_decay;
    double momentum;
    double beta1;
    double beta2;
    double epsilon;
    oasp_ai_tensor **first;
    oasp_ai_tensor **second;
    size_t count;
} staged_optimizer;

static void staged_optimizer_clear(staged_optimizer *staged) {
    for (size_t index = 0; index < staged->count; index++) {
        oasp_ai_tensor_release(staged->first ? staged->first[index] : NULL);
        oasp_ai_tensor_release(staged->second ? staged->second[index] : NULL);
    }
    free(staged->first);
    free(staged->second);
    memset(staged, 0, sizeof(*staged));
}

static int stage_optimizer(checkpoint_reader *reader, oasp_ai_optimizer *optimizer, staged_optimizer *staged, oasp_ai_error *error) {
    uint32_t kind = 0;
    uint64_t step;
    double learning_rate;
    double weight_decay;
    double momentum;
    double beta1;
    double beta2;
    double epsilon;
    uint32_t count;
    if (!optimizer || reader_u32(reader, &kind) || kind != (uint32_t)optimizer->kind || reader_u64(reader, &step) ||
        reader_f64(reader, &learning_rate) || reader_f64(reader, &weight_decay) || reader_f64(reader, &momentum) ||
        reader_f64(reader, &beta1) || reader_f64(reader, &beta2) || reader_f64(reader, &epsilon) ||
        reader_u32(reader, &count) || count != optimizer->parameter_count)
        return -1;
    oasp_ai_tensor **first = count ? calloc(count, sizeof(*first)) : NULL;
    oasp_ai_tensor **second = count ? calloc(count, sizeof(*second)) : NULL;
    unsigned char *seen = count ? calloc(count, 1) : NULL;
    int failed = count && (!first || !second || !seen);
    for (uint32_t item = 0; item < count && !failed; item++) {
        char *name = reader_string(reader, CHECKPOINT_MAX_NAME);
        ssize_t target = name ? parameter_index(optimizer->module, name) : -1;
        uint32_t present;
        failed = target < 0 || seen[target] || reader_u32(reader, &present) || present > 1;
        seen[target < 0 ? 0 : target] = target >= 0;
        for (unsigned slot = 0; slot < 2 && !failed; slot++) {
            if (slot)
                failed = reader_u32(reader, &present) || present > 1;
            if (!failed && present) {
                char *ignored = NULL;
                oasp_ai_tensor *tensor = reader_tensor(reader, optimizer->module->context, &ignored, error);
                free(ignored);
                failed = !tensor;
                if (!failed)
                    (slot ? second : first)[target] = tensor;
            }
        }
        free(name);
    }
    for (uint32_t index = 0; index < count && !failed; index++)
        failed = !seen[index];
    if (failed)
        for (uint32_t index = 0; index < count; index++) {
            oasp_ai_tensor_release(first ? first[index] : NULL);
            oasp_ai_tensor_release(second ? second[index] : NULL);
        }
    free(seen);
    if (failed) {
        free(first);
        free(second);
        return -1;
    }
    *staged = (staged_optimizer){
        .step = step,
        .learning_rate = learning_rate,
        .weight_decay = weight_decay,
        .momentum = momentum,
        .beta1 = beta1,
        .beta2 = beta2,
        .epsilon = epsilon,
        .first = first,
        .second = second,
        .count = count,
    };
    return 0;
}

static void commit_optimizer(oasp_ai_optimizer *optimizer, staged_optimizer *staged) {
    for (size_t index = 0; index < staged->count; index++) {
        oasp_ai_tensor_release(optimizer->first_moment[index]);
        oasp_ai_tensor_release(optimizer->second_moment[index]);
        optimizer->first_moment[index] = staged->first[index];
        optimizer->second_moment[index] = staged->second[index];
        staged->first[index] = NULL;
        staged->second[index] = NULL;
    }
    optimizer->step = staged->step;
    optimizer->learning_rate = staged->learning_rate;
    optimizer->weight_decay = staged->weight_decay;
    optimizer->momentum = staged->momentum;
    optimizer->beta1 = staged->beta1;
    optimizer->beta2 = staged->beta2;
    optimizer->epsilon = staged->epsilon;
}

static int verify_checkpoint_digest(FILE *file, uint64_t payload_size) {
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    unsigned char chunk[65536];
    unsigned char actual[CHECKPOINT_DIGEST_SIZE];
    unsigned char expected[CHECKPOINT_DIGEST_SIZE];
    unsigned actual_size = 0;
    int failed = !digest || fseek(file, 0, SEEK_SET) || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1;
    uint64_t remaining = payload_size;
    while (!failed && remaining) {
        size_t amount = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        failed = fread(chunk, 1, amount, file) != amount || EVP_DigestUpdate(digest, chunk, amount) != 1;
        remaining -= amount;
    }
    if (!failed)
        failed = EVP_DigestFinal_ex(digest, actual, &actual_size) != 1 || actual_size != sizeof(actual) ||
                 fread(expected, 1, sizeof(expected), file) != sizeof(expected) ||
                 memcmp(actual, expected, sizeof(actual)) || fseek(file, 0, SEEK_SET);
    EVP_MD_CTX_free(digest);
    return failed ? -1 : 0;
}

int oasp_ai_load_checkpoint_full(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, oasp_ai_checkpoint_state *state, int strict,
                                 oasp_ai_error *error) {
    FILE *file = module && path ? fopen(path, "rb") : NULL;
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file)
            fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length < 64 || (uint64_t)length > module->context->options.maximum_model_bytes || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return -1;
    }
    uint64_t payload_size = (uint64_t)length - CHECKPOINT_DIGEST_SIZE;
    int failed = verify_checkpoint_digest(file, payload_size);
    checkpoint_reader reader = {file, payload_size};
    char magic[8];
    uint32_t version = 0;
    uint32_t flags = 0;
    uint32_t runtime_major = 0;
    uint32_t runtime_minor = 0;
    uint32_t operator_major = 0;
    uint32_t operator_minor = 0;
    uint64_t schema_hash = 0;
    uint64_t rng_counter = 0;
    uint64_t epoch = 0;
    uint64_t global_step = 0;
    uint64_t metadata_size = 0;
    if (failed || reader_bytes(&reader, magic, 8) || memcmp(magic, "OASPCKP3", 8) || reader_u32(&reader, &version) ||
        version != CHECKPOINT_VERSION || reader_u32(&reader, &flags) || reader_u32(&reader, &runtime_major) ||
        reader_u32(&reader, &runtime_minor) || runtime_major > OASP_AI_VERSION_MAJOR ||
        reader_u32(&reader, &operator_major) || reader_u32(&reader, &operator_minor) ||
        operator_major > OASP_AI_OPERATOR_SET_MAJOR || reader_u64(&reader, &schema_hash) ||
        (strict && schema_hash != oasp_ai_module_schema_hash(module)) || reader_u64(&reader, &rng_counter) ||
        reader_u64(&reader, &epoch) || reader_u64(&reader, &global_step) || reader_u64(&reader, &metadata_size) ||
        flags & ~15u || metadata_size > reader.remaining || metadata_size > SIZE_MAX)
        failed = 1;
    void *metadata = !failed && metadata_size ? malloc((size_t)metadata_size) : NULL;
    if (!failed && ((metadata_size && !metadata) || reader_bytes(&reader, metadata, (size_t)metadata_size)))
        failed = 1;
    uint32_t state_count = 0;
    if (!failed)
        failed = reader_u32(&reader, &state_count) || state_count > CHECKPOINT_MAX_TENSORS;
    oasp_ai_state_dict *dictionary = !failed ? calloc(1, sizeof(*dictionary)) : NULL;
    if (!failed && !dictionary)
        failed = 1;
    if (dictionary) {
        atomic_init(&dictionary->references, 1);
        dictionary->entries = state_count ? calloc(state_count, sizeof(*dictionary->entries)) : NULL;
        if (state_count && !dictionary->entries)
            failed = 1;
    }
    for (uint32_t index = 0; index < state_count && !failed; index++) {
        uint32_t kind = 0;
        if (reader_u32(&reader, &kind) || kind > OASP_AI_STATE_BUFFER) {
            failed = 1;
            break;
        }
        oasp_ai_state_entry *entry = &dictionary->entries[dictionary->count];
        entry->kind = (oasp_ai_state_kind)kind;
        entry->tensor = reader_tensor(&reader, module->context, &entry->name, error);
        failed = !entry->tensor;
        if (!failed)
            dictionary->count++;
        else {
            free(entry->name);
            oasp_ai_tensor_release(entry->tensor);
            memset(entry, 0, sizeof(*entry));
        }
    }
    staged_optimizer staged = {0};
    if (!failed && (flags & 1u))
        failed = stage_optimizer(&reader, optimizer, &staged, error);
    uint32_t scheduler_kind = 0;
    uint64_t scheduler_total_steps = 0;
    uint64_t scheduler_warmup = 0;
    uint64_t scheduler_step = 0;
    double scheduler_initial = 0;
    double scheduler_factor = 0;
    if (!failed && (flags & 2u)) {
        failed = !state || !state->scheduler || reader_u32(&reader, &scheduler_kind) ||
                 scheduler_kind != (uint32_t)state->scheduler->kind ||
                 reader_u64(&reader, &scheduler_total_steps) || reader_u64(&reader, &scheduler_warmup) ||
                 reader_u64(&reader, &scheduler_step) || reader_f64(&reader, &scheduler_initial) ||
                 reader_f64(&reader, &scheduler_factor) || !scheduler_total_steps ||
                 scheduler_step > scheduler_total_steps || !(scheduler_initial > 0);
    }
    double scaler_scale = 0;
    double scaler_growth = 0;
    double scaler_backoff = 0;
    uint32_t scaler_interval = 0;
    uint32_t scaler_finite = 0;
    if (!failed && (flags & 4u)) {
        failed = !state || !state->loss_scaler || reader_f64(&reader, &scaler_scale) ||
                 reader_f64(&reader, &scaler_growth) || reader_f64(&reader, &scaler_backoff) ||
                 reader_u32(&reader, &scaler_interval) || reader_u32(&reader, &scaler_finite) ||
                 !(scaler_scale > 0) || !(scaler_growth > 1) || !(scaler_backoff > 0 && scaler_backoff < 1) ||
                 !scaler_interval;
    }
    uint64_t iterator_epoch = 0;
    uint64_t iterator_cursor = 0;
    uint64_t iterator_seed = 0;
    uint32_t iterator_shuffle = 0;
    if (!failed && (flags & 8u)) {
        failed = !state || !state->data_iterator || reader_u64(&reader, &iterator_epoch) ||
                 reader_u64(&reader, &iterator_cursor) || reader_u64(&reader, &iterator_seed) ||
                 reader_u32(&reader, &iterator_shuffle) ||
                 iterator_seed != state->data_iterator->loader->options.seed ||
                 iterator_shuffle != (uint32_t)(state->data_iterator->loader->options.shuffle != 0) ||
                 iterator_epoch != state->data_iterator->epoch ||
                 iterator_cursor > state->data_iterator->loader->dataset->sample_count;
    }
    if (!failed)
        failed = reader.remaining != 0;
    oasp_ai_state_diagnostics diagnostics = {0};
    if (!failed)
        failed = oasp_ai_module_load_state_dict(module, dictionary, strict, &diagnostics, error);
    oasp_ai_state_diagnostics_free(&diagnostics);
    if (!failed) {
        if (flags & 1u)
            commit_optimizer(optimizer, &staged);
        if (flags & 2u) {
            state->scheduler->total_steps = scheduler_total_steps;
            state->scheduler->warmup_or_step_size = scheduler_warmup;
            state->scheduler->step = scheduler_step;
            state->scheduler->initial_learning_rate = scheduler_initial;
            state->scheduler->factor_or_minimum = scheduler_factor;
        }
        if (flags & 4u) {
            state->loss_scaler->scale = scaler_scale;
            state->loss_scaler->growth_factor = scaler_growth;
            state->loss_scaler->backoff_factor = scaler_backoff;
            state->loss_scaler->growth_interval = scaler_interval;
            state->loss_scaler->finite_steps = scaler_finite;
        }
        if (flags & 8u)
            state->data_iterator->cursor = iterator_cursor;
        module->context->rng_counter = rng_counter;
        if (state) {
            free((void *)state->metadata);
            state->metadata = metadata;
            state->metadata_size = (size_t)metadata_size;
            state->epoch = epoch;
            state->global_step = global_step;
        } else
            free(metadata);
    } else
        free(metadata);
    staged_optimizer_clear(&staged);
    oasp_ai_state_dict_release(dictionary);
    fclose(file);
    if (failed) {
        oasp_ai_set_error(error, 5, "LoadCheckpoint", "checkpoint is incompatible, truncated, or corrupted",
                          "Restore a matching module, optimizer, scheduler, scaler, and DataLoader state.");
        return -1;
    }
    (void)runtime_minor;
    (void)operator_minor;
    return 0;
}

void oasp_ai_checkpoint_state_clear(oasp_ai_checkpoint_state *state) {
    if (!state)
        return;
    free((void *)state->metadata);
    state->metadata = NULL;
    state->metadata_size = 0;
}

int oasp_ai_save_checkpoint(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, const void *metadata, size_t metadata_size,
                            oasp_ai_error *error) {
    oasp_ai_checkpoint_state state = {.metadata = metadata, .metadata_size = metadata_size};
    return oasp_ai_save_checkpoint_full(path, module, optimizer, &state, error);
}

int oasp_ai_load_checkpoint(const char *path, oasp_ai_module *module, oasp_ai_optimizer *optimizer, void **metadata, size_t *metadata_size,
                            oasp_ai_error *error) {
    oasp_ai_checkpoint_state state = {0};
    if (oasp_ai_load_checkpoint_full(path, module, optimizer, &state, 1, error))
        return -1;
    if (metadata)
        *metadata = (void *)state.metadata;
    else
        free((void *)state.metadata);
    if (metadata_size)
        *metadata_size = state.metadata_size;
    return 0;
}
