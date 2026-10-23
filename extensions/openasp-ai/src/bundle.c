#define _XOPEN_SOURCE 700

// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <limits.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    BUNDLE_VERSION = 2,
    BUNDLE_MAX_ENTRY = 1024,
    BUNDLE_MAX_NAME = 4096,
    BUNDLE_MAX_TENSORS = 1000000,
    BUNDLE_MAX_SIGNATURE = 65536,
    BUNDLE_DIGEST_SIZE = 32
};

typedef struct {
    FILE *file;
    EVP_MD_CTX *digest;
} bundle_writer_stream;

typedef struct {
    FILE *file;
    uint64_t remaining;
} bundle_reader_stream;

static int stream_write(bundle_writer_stream *stream, const void *data, size_t size) {
    return (size && fwrite(data, 1, size, stream->file) != size) ||
                   (size && EVP_DigestUpdate(stream->digest, data, size) != 1)
               ? -1
               : 0;
}

static int stream_u32(bundle_writer_stream *stream, uint32_t value) {
    unsigned char bytes[] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return stream_write(stream, bytes, sizeof(bytes));
}

static int stream_u64(bundle_writer_stream *stream, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned index = 0; index < 8; index++)
        bytes[index] = (unsigned char)(value >> (index * 8));
    return stream_write(stream, bytes, sizeof(bytes));
}

static int stream_f64(bundle_writer_stream *stream, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return stream_u64(stream, bits);
}

static int stream_string(bundle_writer_stream *stream, const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length > UINT32_MAX || stream_u32(stream, (uint32_t)length) || stream_write(stream, value, length) ? -1 : 0;
}

static int write_module_tree(bundle_writer_stream *stream, const oasp_ai_module *module, const char *prefix) {
    if (stream_string(stream, prefix) || stream_string(stream, module->name) || stream_u32(stream, module->kind) ||
        stream_u32(stream, module->training != 0) || stream_u64(stream, module->feature_size))
        return -1;
    uint64_t argument;
    memcpy(&argument, &module->argument, sizeof(argument));
    if (stream_u64(stream, argument) || stream_u32(stream, (uint32_t)module->child_count))
        return -1;
    for (size_t index = 0; index < module->child_count; index++) {
        char path[BUNDLE_MAX_NAME + 1];
        int length = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", module->children[index].name);
        if (length < 0 || (size_t)length >= sizeof(path) ||
            write_module_tree(stream, module->children[index].module, path))
            return -1;
    }
    return 0;
}

static int write_tensor(bundle_writer_stream *stream, const oasp_ai_state_entry *entry, oasp_ai_error *error) {
    oasp_ai_tensor *tensor = oasp_ai_tensor_contiguous(entry->tensor, error);
    if (!tensor)
        return -1;
    uint64_t bytes = oasp_ai_tensor_bytes(tensor);
    oasp_ai_quantization_info quantization = {0};
    oasp_ai_tensor_quantization(tensor, &quantization);
    int failed = stream_u32(stream, entry->kind) || stream_string(stream, entry->name) ||
                 stream_u32(stream, tensor->desc.dtype) || stream_u32(stream, tensor->desc.rank);
    for (uint32_t axis = 0; axis < tensor->desc.rank && !failed; axis++)
        failed = stream_u64(stream, (uint64_t)tensor->desc.shape[axis]);
    if (!failed)
        failed = stream_u32(stream, quantization.enabled ? quantization.bits : 0) ||
                 stream_u32(stream, quantization.enabled ? (uint32_t)quantization.axis : 0) ||
                 stream_f64(stream, quantization.enabled ? quantization.scale : 0) ||
                 stream_u64(stream, quantization.enabled ? (uint64_t)quantization.zero_point : 0) ||
                 stream_string(stream, quantization.enabled ? quantization.calibration_method : "") ||
                 stream_u64(stream, quantization.enabled ? (uint64_t)quantization.channel_count : 0);
    for (size_t channel = 0; channel < quantization.channel_count && !failed; channel++)
        failed = stream_f64(stream, quantization.channel_scales[channel]);
    if (!failed)
        failed = stream_u64(stream, bytes);
    const unsigned char *data = oasp_ai_tensor_const_data(tensor);
    for (uint64_t position = 0; position < bytes && !failed;) {
        size_t amount = bytes - position < 65536 ? (size_t)(bytes - position) : 65536;
        failed = stream_write(stream, data + position, amount);
        position += amount;
    }
    oasp_ai_tensor_release(tensor);
    return failed ? -1 : 0;
}

static int sign_digest(const char *private_key_pem, const unsigned char digest[32], unsigned char **signature, size_t *signature_size) {
    *signature = NULL;
    *signature_size = 0;
    if (!private_key_pem)
        return 0;
    BIO *bio = BIO_new_mem_buf(private_key_pem, -1);
    EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
    EVP_MD_CTX *context = key ? EVP_MD_CTX_new() : NULL;
    int failed = !context || EVP_DigestSignInit(context, NULL, EVP_sha256(), NULL, key) != 1 ||
                 EVP_DigestSign(context, NULL, signature_size, digest, 32) != 1 ||
                 *signature_size > BUNDLE_MAX_SIGNATURE;
    if (!failed) {
        *signature = malloc(*signature_size);
        failed = !*signature || EVP_DigestSign(context, *signature, signature_size, digest, 32) != 1;
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    BIO_free(bio);
    if (failed) {
        free(*signature);
        *signature = NULL;
        *signature_size = 0;
    }
    return failed ? -1 : 0;
}

typedef struct {
    oasp_ai_module *module;
    const char *entry_class;
    const void *bytecode;
    size_t bytecode_size;
    const void *configuration;
    size_t configuration_size;
    const void *dependencies;
    size_t dependencies_size;
    const oasp_ai_bundle_security *security;
} bundle_writer;

static int write_bundle(FILE *file, void *opaque, oasp_ai_error *error) {
    bundle_writer *writer = opaque;
    oasp_ai_state_dict *state = oasp_ai_module_state_dict(writer->module, error);
    EVP_MD_CTX *digest_context = EVP_MD_CTX_new();
    bundle_writer_stream stream = {file, digest_context};
    int failed = !state || !digest_context || EVP_DigestInit_ex(digest_context, EVP_sha256(), NULL) != 1 ||
                 stream_write(&stream, "OASPBND2", 8) || stream_u32(&stream, BUNDLE_VERSION) ||
                 stream_u32(&stream, OASP_AI_VERSION_MAJOR) || stream_u32(&stream, OASP_AI_VERSION_MINOR) ||
                 stream_u64(&stream, oasp_ai_module_schema_hash(writer->module)) ||
                 stream_string(&stream, writer->entry_class) ||
                 stream_u64(&stream, writer->bytecode_size) ||
                 stream_write(&stream, writer->bytecode, writer->bytecode_size) ||
                 stream_u64(&stream, writer->configuration_size) ||
                 stream_write(&stream, writer->configuration, writer->configuration_size) ||
                 stream_u64(&stream, writer->dependencies_size) ||
                 stream_write(&stream, writer->dependencies, writer->dependencies_size) ||
                 write_module_tree(&stream, writer->module, "") ||
                 state->count > UINT32_MAX || stream_u32(&stream, (uint32_t)state->count);
    for (size_t index = 0; index < state->count && !failed; index++)
        failed = write_tensor(&stream, &state->entries[index], error);
    unsigned char digest[BUNDLE_DIGEST_SIZE];
    unsigned digest_size = 0;
    if (!failed)
        failed = EVP_DigestFinal_ex(digest_context, digest, &digest_size) != 1 || digest_size != sizeof(digest);
    unsigned char *signature = NULL;
    size_t signature_size = 0;
    if (!failed)
        failed = sign_digest(writer->security ? writer->security->signing_private_key_pem : NULL,
                             digest, &signature, &signature_size);
    unsigned char size_bytes[4] = {(unsigned char)signature_size, (unsigned char)(signature_size >> 8),
                                   (unsigned char)(signature_size >> 16), (unsigned char)(signature_size >> 24)};
    if (!failed)
        failed = fwrite(digest, 1, sizeof(digest), file) != sizeof(digest) ||
                 (signature_size && fwrite(signature, 1, signature_size, file) != signature_size) ||
                 fwrite(size_bytes, 1, sizeof(size_bytes), file) != sizeof(size_bytes);
    free(signature);
    EVP_MD_CTX_free(digest_context);
    oasp_ai_state_dict_release(state);
    if (failed)
        oasp_ai_set_error(error, 5, "SaveBundle", "unable to create signed ASP Bundle",
                          "Check model state, signing key, and disk capacity.");
    return failed ? -1 : 0;
}

int oasp_ai_save_bundle(const char *path, oasp_ai_module *module, const char *entry_class, const void *bytecode, size_t bytecode_size,
                        const void *configuration, size_t configuration_size, const void *dependency_manifest, size_t dependency_manifest_size, const oasp_ai_bundle_security *security, oasp_ai_error *error) {
    if (!module || !entry_class || !*entry_class || strlen(entry_class) > BUNDLE_MAX_ENTRY ||
        (bytecode_size && !bytecode) || (configuration_size && !configuration) ||
        (dependency_manifest_size && !dependency_manifest)) {
        oasp_ai_set_error(error, 5, "SaveBundle", "bundle arguments are invalid",
                          "Pass bytecode, entry class, configuration, and dependencies.");
        return -1;
    }
    bundle_writer writer = {module, entry_class, bytecode, bytecode_size, configuration, configuration_size,
                            dependency_manifest, dependency_manifest_size, security};
    return oasp_ai_write_atomic(path, write_bundle, &writer, error);
}

static int stream_read(bundle_reader_stream *stream, void *output, size_t size) {
    if (size > stream->remaining)
        return -1;
    unsigned char discard[65536];
    size_t pending = size;
    while (pending) {
        size_t amount = pending < sizeof(discard) ? pending : sizeof(discard);
        void *destination = output ? (unsigned char *)output + size - pending : discard;
        if (fread(destination, 1, amount, stream->file) != amount)
            return -1;
        pending -= amount;
    }
    stream->remaining -= size;
    return 0;
}

static int read_u32(bundle_reader_stream *stream, uint32_t *value) {
    unsigned char bytes[4];
    if (stream_read(stream, bytes, sizeof(bytes)))
        return -1;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 0;
}

static int read_u64(bundle_reader_stream *stream, uint64_t *value) {
    unsigned char bytes[8];
    if (stream_read(stream, bytes, sizeof(bytes)))
        return -1;
    *value = 0;
    for (unsigned index = 0; index < 8; index++)
        *value |= (uint64_t)bytes[index] << (index * 8);
    return 0;
}

static int read_f64(bundle_reader_stream *stream, double *value) {
    uint64_t bits;
    if (read_u64(stream, &bits))
        return -1;
    memcpy(value, &bits, sizeof(*value));
    return 0;
}

static char *read_string(bundle_reader_stream *stream, uint32_t maximum) {
    uint32_t length;
    if (read_u32(stream, &length) || length > maximum || length > stream->remaining)
        return NULL;
    char *value = malloc((size_t)length + 1);
    if (!value || stream_read(stream, value, length)) {
        free(value);
        return NULL;
    }
    value[length] = '\0';
    return value;
}

static int read_blob(bundle_reader_stream *stream, void **output, size_t *output_size, uint64_t maximum) {
    uint64_t size;
    if (read_u64(stream, &size) || size > maximum || size > SIZE_MAX || size > stream->remaining)
        return -1;
    void *data = size ? malloc((size_t)size) : NULL;
    if ((size && !data) || stream_read(stream, data, (size_t)size)) {
        free(data);
        return -1;
    }
    *output = data;
    *output_size = (size_t)size;
    return 0;
}

static int skip_module_tree(bundle_reader_stream *stream, uint32_t depth) {
    if (depth > 128)
        return -1;
    char *path = read_string(stream, BUNDLE_MAX_NAME);
    char *name = read_string(stream, BUNDLE_MAX_NAME);
    uint32_t kind = 0;
    uint32_t training = 0;
    uint64_t feature_size = 0;
    uint64_t argument = 0;
    uint32_t children = 0;
    int failed = !path || !name || read_u32(stream, &kind) || kind > OASP_AI_MODULE_EMBEDDING ||
                 read_u32(stream, &training) || training > 1 || read_u64(stream, &feature_size) ||
                 read_u64(stream, &argument) || read_u32(stream, &children) || children > BUNDLE_MAX_TENSORS;
    free(path);
    free(name);
    for (uint32_t index = 0; index < children && !failed; index++)
        failed = skip_module_tree(stream, depth + 1);
    return failed ? -1 : 0;
}

static oasp_ai_tensor *read_tensor(bundle_reader_stream *stream, oasp_ai_context *context, char **name, oasp_ai_state_kind *kind, oasp_ai_error *error) {
    uint32_t raw_kind;
    uint32_t dtype;
    uint32_t rank;
    if (read_u32(stream, &raw_kind) || raw_kind > OASP_AI_STATE_BUFFER ||
        !(*name = read_string(stream, BUNDLE_MAX_NAME)) || read_u32(stream, &dtype) ||
        dtype > OASP_AI_DTYPE_F64 || read_u32(stream, &rank) || !rank || rank > OASP_AI_MAX_RANK)
        return NULL;
    oasp_ai_tensor_desc desc = {.dtype = (oasp_ai_dtype)dtype, .rank = rank};
    for (uint32_t axis = 0; axis < rank; axis++) {
        uint64_t dimension;
        if (read_u64(stream, &dimension) || !dimension || dimension > INT64_MAX)
            return NULL;
        desc.shape[axis] = (int64_t)dimension;
    }
    uint32_t bits;
    uint32_t raw_axis;
    double scale;
    uint64_t zero_point;
    uint64_t channel_count;
    char *calibration = NULL;
    if (read_u32(stream, &bits) || read_u32(stream, &raw_axis) || read_f64(stream, &scale) ||
        read_u64(stream, &zero_point) || !(calibration = read_string(stream, 31)) ||
        read_u64(stream, &channel_count) || channel_count > UINT32_MAX) {
        free(calibration);
        return NULL;
    }
    double *channel_scales = channel_count ? malloc((size_t)channel_count * sizeof(*channel_scales)) : NULL;
    int failed = channel_count && !channel_scales;
    for (uint64_t channel = 0; channel < channel_count && !failed; channel++)
        failed = read_f64(stream, &channel_scales[channel]) || !(channel_scales[channel] > 0);
    uint64_t bytes = 0;
    uint64_t elements = 0;
    if (!failed)
        failed = read_u64(stream, &bytes) || bytes > context->options.maximum_tensor_bytes ||
                 bytes > SIZE_MAX || bytes > stream->remaining ||
                 oasp_ai_checked_elements(desc.shape, desc.rank, &elements, error) ||
                 elements > UINT64_MAX / oasp_ai_dtype_size(desc.dtype) ||
                 elements * oasp_ai_dtype_size(desc.dtype) != bytes;
    void *data = !failed ? malloc((size_t)bytes) : NULL;
    if (!failed)
        failed = !data || stream_read(stream, data, (size_t)bytes);
    oasp_ai_tensor *tensor = failed ? NULL : oasp_ai_tensor_create(context, &desc, data, (size_t)bytes, error);
    int32_t axis = (int32_t)raw_axis;
    if (tensor && bits) {
        if ((bits != 4 && bits != 8) || axis < -1 || axis >= (int32_t)rank || !(scale > 0) ||
            (channel_count && (axis < 0 || channel_count != (uint64_t)desc.shape[axis]))) {
            oasp_ai_tensor_release(tensor);
            tensor = NULL;
        } else {
            tensor->quantized_bits = bits;
            tensor->quantized_axis = axis;
            tensor->quantized_scale = scale;
            tensor->quantized_zero_point = (int64_t)zero_point;
            tensor->quantized_channel_scales = channel_scales;
            tensor->quantized_channel_count = (size_t)channel_count;
            memcpy(tensor->quantized_calibration, calibration, strlen(calibration) + 1);
            channel_scales = NULL;
        }
    } else if (tensor && (raw_axis || scale || zero_point || *calibration || channel_count)) {
        oasp_ai_tensor_release(tensor);
        tensor = NULL;
    }
    free(data);
    free(channel_scales);
    free(calibration);
    *kind = (oasp_ai_state_kind)raw_kind;
    return tensor;
}

static int path_allowed(const char *path, const char *allowed_root) {
    if (!allowed_root)
        return 0;
    char actual[PATH_MAX];
    char root[PATH_MAX];
    if (!realpath(path, actual) || !realpath(allowed_root, root))
        return -1;
    size_t length = strlen(root);
    return strncmp(actual, root, length) || (actual[length] && actual[length] != '/') ? -1 : 0;
}

static int hex_digest(const unsigned char digest[32], const char *expected) {
    if (!expected)
        return 0;
    if (strlen(expected) != 64)
        return -1;
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < 32; index++)
        if (expected[index * 2] != hex[digest[index] >> 4] || expected[index * 2 + 1] != hex[digest[index] & 15])
            return -1;
    return 0;
}

static int verify_signature(const char *public_key_pem, const unsigned char digest[32], const unsigned char *signature, size_t signature_size) {
    if (!public_key_pem)
        return signature_size ? -1 : 0;
    BIO *bio = BIO_new_mem_buf(public_key_pem, -1);
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    EVP_MD_CTX *context = key ? EVP_MD_CTX_new() : NULL;
    int failed = !context || EVP_DigestVerifyInit(context, NULL, EVP_sha256(), NULL, key) != 1 ||
                 EVP_DigestVerify(context, signature, signature_size, digest, 32) != 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    BIO_free(bio);
    return failed ? -1 : 0;
}

static int verify_bundle(FILE *file, uint64_t file_size, const oasp_ai_bundle_security *security, uint64_t *payload_size) {
    if (file_size < BUNDLE_DIGEST_SIZE + 4 || file_size - 4 > LONG_MAX ||
        fseek(file, (long)(file_size - 4), SEEK_SET))
        return -1;
    unsigned char size_bytes[4];
    if (fread(size_bytes, 1, sizeof(size_bytes), file) != sizeof(size_bytes))
        return -1;
    uint32_t signature_size = (uint32_t)size_bytes[0] | (uint32_t)size_bytes[1] << 8 |
                              (uint32_t)size_bytes[2] << 16 | (uint32_t)size_bytes[3] << 24;
    if (signature_size > BUNDLE_MAX_SIGNATURE || signature_size > file_size - BUNDLE_DIGEST_SIZE - 4)
        return -1;
    *payload_size = file_size - BUNDLE_DIGEST_SIZE - 4 - signature_size;
    EVP_MD_CTX *digest_context = EVP_MD_CTX_new();
    unsigned char digest[BUNDLE_DIGEST_SIZE];
    unsigned char expected[BUNDLE_DIGEST_SIZE];
    unsigned digest_size = 0;
    unsigned char chunk[65536];
    int failed = !*payload_size || !digest_context || fseek(file, 0, SEEK_SET) ||
                 EVP_DigestInit_ex(digest_context, EVP_sha256(), NULL) != 1;
    uint64_t remaining = *payload_size;
    while (!failed && remaining) {
        size_t amount = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        failed = fread(chunk, 1, amount, file) != amount || EVP_DigestUpdate(digest_context, chunk, amount) != 1;
        remaining -= amount;
    }
    if (!failed)
        failed = EVP_DigestFinal_ex(digest_context, digest, &digest_size) != 1 ||
                 digest_size != sizeof(digest) || fread(expected, 1, sizeof(expected), file) != sizeof(expected) ||
                 memcmp(digest, expected, sizeof(digest)) ||
                 hex_digest(digest, security ? security->expected_sha256 : NULL);
    unsigned char *signature = !failed && signature_size ? malloc(signature_size) : NULL;
    if (!failed)
        failed = (signature_size && (!signature || fread(signature, 1, signature_size, file) != signature_size)) ||
                 ((!security || !security->allow_unsigned) && !signature_size) ||
                 verify_signature(security ? security->trusted_public_key_pem : NULL,
                                  digest, signature, signature_size) ||
                 fseek(file, 0, SEEK_SET);
    free(signature);
    EVP_MD_CTX_free(digest_context);
    return failed ? -1 : 0;
}

int oasp_ai_load_bundle(oasp_ai_context *context, const char *path, oasp_ai_module *module, char **entry_class, void **bytecode,
                        size_t *bytecode_size, void **configuration, size_t *configuration_size, void **dependency_manifest, size_t *dependency_manifest_size, const oasp_ai_bundle_security *security, oasp_ai_error *error) {
    if (!context || !path || !module || !entry_class || !bytecode || !bytecode_size || !configuration || !configuration_size ||
        !dependency_manifest || !dependency_manifest_size ||
        path_allowed(path, security ? security->allowed_root : NULL)) {
        oasp_ai_set_error(error, 70, "LoadBundle", "bundle path is outside the allowed root",
                          "Use a canonical path under the configured bundle root.");
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file)
            fclose(file);
        return -1;
    }
    long length = ftell(file);
    uint64_t file_size = length < 0 ? 0 : (uint64_t)length;
    uint64_t payload_size = 0;
    int failed = length < 52 || file_size > context->options.maximum_model_bytes ||
                 verify_bundle(file, file_size, security, &payload_size);
    bundle_reader_stream stream = {file, payload_size};
    char magic[8];
    uint32_t version = 0;
    uint32_t runtime_major = 0;
    uint32_t runtime_minor = 0;
    uint64_t schema_hash = 0;
    if (!failed)
        failed = stream_read(&stream, magic, 8) || memcmp(magic, "OASPBND2", 8) ||
                 read_u32(&stream, &version) || version != BUNDLE_VERSION ||
                 read_u32(&stream, &runtime_major) || runtime_major > OASP_AI_VERSION_MAJOR ||
                 read_u32(&stream, &runtime_minor) || read_u64(&stream, &schema_hash) ||
                 schema_hash != oasp_ai_module_schema_hash(module);
    char *loaded_entry = failed ? NULL : read_string(&stream, BUNDLE_MAX_ENTRY);
    void *loaded_bytecode = NULL;
    size_t loaded_bytecode_size = 0;
    void *loaded_configuration = NULL;
    size_t loaded_configuration_size = 0;
    void *loaded_dependencies = NULL;
    size_t loaded_dependencies_size = 0;
    if (!failed)
        failed = !loaded_entry ||
                 read_blob(&stream, &loaded_bytecode, &loaded_bytecode_size, context->options.maximum_model_bytes) ||
                 read_blob(&stream, &loaded_configuration, &loaded_configuration_size, context->options.maximum_model_bytes) ||
                 read_blob(&stream, &loaded_dependencies, &loaded_dependencies_size, context->options.maximum_model_bytes) ||
                 skip_module_tree(&stream, 0);
    uint32_t tensor_count = 0;
    if (!failed)
        failed = read_u32(&stream, &tensor_count) || tensor_count > BUNDLE_MAX_TENSORS;
    oasp_ai_state_dict *loaded_state = !failed ? calloc(1, sizeof(*loaded_state)) : NULL;
    if (!failed && !loaded_state)
        failed = 1;
    if (loaded_state) {
        atomic_init(&loaded_state->references, 1);
        loaded_state->entries = tensor_count ? calloc(tensor_count, sizeof(*loaded_state->entries)) : NULL;
        if (tensor_count && !loaded_state->entries)
            failed = 1;
    }
    for (uint32_t index = 0; index < tensor_count && !failed; index++) {
        oasp_ai_state_entry *entry = &loaded_state->entries[loaded_state->count];
        entry->tensor = read_tensor(&stream, context, &entry->name, &entry->kind, error);
        failed = !entry->tensor;
        if (!failed)
            loaded_state->count++;
        else {
            free(entry->name);
            oasp_ai_tensor_release(entry->tensor);
            memset(entry, 0, sizeof(*entry));
        }
    }
    oasp_ai_state_diagnostics diagnostics = {0};
    if (!failed)
        failed = stream.remaining || oasp_ai_module_load_state_dict(module, loaded_state, 1, &diagnostics, error);
    oasp_ai_state_diagnostics_free(&diagnostics);
    oasp_ai_state_dict_release(loaded_state);
    fclose(file);
    if (failed) {
        free(loaded_entry);
        free(loaded_bytecode);
        free(loaded_configuration);
        free(loaded_dependencies);
        oasp_ai_set_error(error, 5, "LoadBundle", "bundle signature, manifest, module tree, or StateDict is invalid",
                          "Use a signed trusted bundle built for the matching runtime and module schema.");
        return -1;
    }
    *entry_class = loaded_entry;
    *bytecode = loaded_bytecode;
    *bytecode_size = loaded_bytecode_size;
    *configuration = loaded_configuration;
    *configuration_size = loaded_configuration_size;
    *dependency_manifest = loaded_dependencies;
    *dependency_manifest_size = loaded_dependencies_size;
    (void)runtime_minor;
    return 0;
}
