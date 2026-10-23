// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static size_t state_count(const oasp_ai_module *module) {
    size_t count = module->parameter_count;
    for (size_t index = 0; index < module->buffer_count; index++)
        count += module->buffers[index].persistent != 0;
    for (size_t index = 0; index < module->child_count; index++)
        count += state_count(module->children[index].module);
    return count;
}

static int append_state(const oasp_ai_module *module, oasp_ai_state_dict *state, char *path, size_t path_size, size_t prefix) {
    for (size_t group = 0; group < 2; group++) {
        const oasp_ai_named_tensor *items = group ? module->buffers : module->parameters;
        size_t count = group ? module->buffer_count : module->parameter_count;
        for (size_t index = 0; index < count; index++) {
            if (group && !items[index].persistent)
                continue;
            size_t length = strlen(items[index].name);
            if (prefix + length + 1 > path_size)
                return -1;
            memcpy(path + prefix, items[index].name, length + 1);
            oasp_ai_state_entry *entry = &state->entries[state->count++];
            entry->name = oasp_ai_string_duplicate(path);
            entry->tensor = items[index].tensor;
            entry->kind = group ? OASP_AI_STATE_BUFFER : OASP_AI_STATE_PARAMETER;
            if (!entry->name)
                return -1;
            oasp_ai_tensor_retain(entry->tensor);
        }
    }
    for (size_t index = 0; index < module->child_count; index++) {
        size_t length = strlen(module->children[index].name);
        if (prefix + length + 2 > path_size)
            return -1;
        memcpy(path + prefix, module->children[index].name, length);
        path[prefix + length] = '.';
        if (append_state(module->children[index].module, state, path, path_size, prefix + length + 1))
            return -1;
    }
    return 0;
}

oasp_ai_state_dict *oasp_ai_module_state_dict(const oasp_ai_module *module, oasp_ai_error *error) {
    if (!module) {
        oasp_ai_set_error(error, 5, "StateDict", "module is required", "Pass a valid Module.");
        return NULL;
    }
    oasp_ai_state_dict *state = calloc(1, sizeof(*state));
    size_t count = state_count(module);
    if (state)
        state->entries = count ? calloc(count, sizeof(*state->entries)) : NULL;
    if (!state || (count && !state->entries)) {
        free(state);
        oasp_ai_set_error(error, 7, "StateDict", "unable to allocate state dictionary", "Reduce process memory pressure.");
        return NULL;
    }
    atomic_init(&state->references, 1);
    char path[4096];
    if (append_state(module, state, path, sizeof(path), 0)) {
        oasp_ai_state_dict_release(state);
        oasp_ai_set_error(error, 5, "StateDict", "a state name exceeds the supported length", "Use shorter module registration names.");
        return NULL;
    }
    return state;
}

void oasp_ai_state_dict_release(oasp_ai_state_dict *state) {
    if (!state || atomic_fetch_sub_explicit(&state->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t index = 0; index < state->count; index++) {
        free(state->entries[index].name);
        oasp_ai_tensor_release(state->entries[index].tensor);
    }
    free(state->entries);
    free(state);
}

size_t oasp_ai_state_dict_count(const oasp_ai_state_dict *state) {
    return state ? state->count : 0;
}

oasp_ai_tensor *oasp_ai_state_dict_tensor(const oasp_ai_state_dict *state, size_t index, const char **name, oasp_ai_state_kind *kind) {
    if (!state || index >= state->count)
        return NULL;
    if (name)
        *name = state->entries[index].name;
    if (kind)
        *kind = state->entries[index].kind;
    oasp_ai_tensor_retain(state->entries[index].tensor);
    return state->entries[index].tensor;
}

static int append_diagnostic(char ***items, size_t *count, const char *name) {
    char **grown = realloc(*items, (*count + 1) * sizeof(**items));
    if (!grown)
        return -1;
    *items = grown;
    grown[*count] = oasp_ai_string_duplicate(name);
    if (!grown[*count])
        return -1;
    (*count)++;
    return 0;
}

void oasp_ai_state_diagnostics_free(oasp_ai_state_diagnostics *diagnostics) {
    if (!diagnostics)
        return;
    char ***groups[] = {&diagnostics->missing, &diagnostics->unexpected, &diagnostics->shape_mismatch, &diagnostics->dtype_mismatch};
    size_t *counts[] = {&diagnostics->missing_count, &diagnostics->unexpected_count, &diagnostics->shape_mismatch_count,
                        &diagnostics->dtype_mismatch_count};
    for (size_t group = 0; group < 4; group++) {
        for (size_t index = 0; index < *counts[group]; index++)
            free((*groups[group])[index]);
        free(*groups[group]);
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
}

static ssize_t find_state(const oasp_ai_state_dict *state, const char *name) {
    for (size_t index = 0; index < state->count; index++)
        if (!strcmp(state->entries[index].name, name))
            return (ssize_t)index;
    return -1;
}

static void free_quantization_copies(double **copies, size_t count) {
    if (!copies)
        return;
    for (size_t index = 0; index < count; index++)
        free(copies[index]);
    free(copies);
}

static int copy_state_tensor(oasp_ai_tensor *destination, const oasp_ai_tensor *source, oasp_ai_error *error) {
    size_t bytes = (size_t)oasp_ai_tensor_bytes(destination);
    if (!(destination->desc.flags & OASP_AI_TENSOR_CONTIGUOUS)) {
        for (uint64_t index = 0; index < destination->element_count; index++)
            oasp_ai_tensor_write(destination, index, oasp_ai_tensor_read(source, index));
        return 0;
    }
    void *staging = malloc(bytes);
    if (!staging || oasp_ai_tensor_copy_to_host(source, staging, bytes, error)) {
        free(staging);
        return -1;
    }
    int32_t backend_index = oasp_ai_tensor_backend_index(destination);
    if (backend_index < 0)
        memcpy(oasp_ai_tensor_data(destination), staging, bytes);
    else {
        oasp_ai_registered_backend *backend = &destination->context->backends[backend_index];
        oasp_ai_copy_desc copy = {
            .struct_size = sizeof(copy),
            .destination = destination->storage->backend_storage,
            .destination_offset = destination->offset * oasp_ai_dtype_size(destination->desc.dtype),
            .bytes = bytes,
            .source_host = staging,
            .source_kind = OASP_AI_MEMORY_HOST,
            .destination_kind = OASP_AI_MEMORY_DEVICE,
        };
        oasp_ai_event copied = 0;
        if (backend->descriptor->api->copy(backend->transfer_stream, &copy, &copied, error) ||
            backend->descriptor->api->wait_event(backend->compute_stream, copied, error)) {
            free(staging);
            return -1;
        }
        destination->context->metrics.host_to_device_bytes += bytes;
    }
    free(staging);
    return 0;
}

int oasp_ai_module_load_state_dict(oasp_ai_module *module, const oasp_ai_state_dict *source, int strict, oasp_ai_state_diagnostics *diagnostics, oasp_ai_error *error) {
    oasp_ai_state_diagnostics local = {0};
    oasp_ai_state_diagnostics *result = diagnostics ? diagnostics : &local;
    memset(result, 0, sizeof(*result));
    oasp_ai_state_dict *target = oasp_ai_module_state_dict(module, error);
    unsigned char *used = source && source->count ? calloc(source->count, 1) : NULL;
    double **quantization_copies = target && target->count ? calloc(target->count, sizeof(*quantization_copies)) : NULL;
    if (!target || !source || (source->count && !used) || (target->count && !quantization_copies))
        goto failure;
    for (size_t index = 0; index < target->count; index++) {
        ssize_t found = find_state(source, target->entries[index].name);
        if (found < 0) {
            if (append_diagnostic(&result->missing, &result->missing_count, target->entries[index].name))
                goto failure;
            continue;
        }
        used[found] = 1;
        oasp_ai_tensor *destination = target->entries[index].tensor;
        oasp_ai_tensor *input = source->entries[found].tensor;
        if (destination->desc.dtype != input->desc.dtype) {
            if (append_diagnostic(&result->dtype_mismatch, &result->dtype_mismatch_count, target->entries[index].name))
                goto failure;
            continue;
        }
        int shape_matches = destination->desc.rank == input->desc.rank;
        for (uint32_t axis = 0; axis < destination->desc.rank && shape_matches; axis++)
            shape_matches = destination->desc.shape[axis] == input->desc.shape[axis];
        if (!shape_matches) {
            if (append_diagnostic(&result->shape_mismatch, &result->shape_mismatch_count, target->entries[index].name))
                goto failure;
            continue;
        }
        if (input->quantized_bits) {
            int32_t axis = input->quantized_axis;
            if ((input->quantized_bits != 4 && input->quantized_bits != 8) || axis < -1 ||
                axis >= (int32_t)input->desc.rank || !(input->quantized_scale > 0) ||
                (input->quantized_channel_count &&
                 (axis < 0 || input->quantized_channel_count != (size_t)input->desc.shape[axis])))
                goto failure;
            if (input->quantized_channel_count) {
                quantization_copies[index] =
                    malloc(input->quantized_channel_count * sizeof(*quantization_copies[index]));
                if (!quantization_copies[index])
                    goto failure;
                memcpy(quantization_copies[index], input->quantized_channel_scales,
                       input->quantized_channel_count * sizeof(*quantization_copies[index]));
            }
        }
    }
    for (size_t index = 0; index < source->count; index++)
        if (!used[index] && append_diagnostic(&result->unexpected, &result->unexpected_count, source->entries[index].name))
            goto failure;
    if (strict && (result->missing_count || result->unexpected_count || result->shape_mismatch_count || result->dtype_mismatch_count)) {
        free(used);
        free_quantization_copies(quantization_copies, target->count);
        oasp_ai_state_dict_release(target);
        oasp_ai_set_error(error, 5, "LoadStateDict", "state dictionary does not match the module",
                          "Inspect missing, unexpected, shape_mismatch, and dtype_mismatch diagnostics.");
        if (!diagnostics)
            oasp_ai_state_diagnostics_free(result);
        return -1;
    }
    for (size_t index = 0; index < target->count; index++) {
        ssize_t found = find_state(source, target->entries[index].name);
        if (found < 0)
            continue;
        oasp_ai_tensor *destination = target->entries[index].tensor;
        oasp_ai_tensor *input = source->entries[found].tensor;
        int compatible = destination->desc.dtype == input->desc.dtype && destination->desc.rank == input->desc.rank;
        for (uint32_t axis = 0; axis < destination->desc.rank && compatible; axis++)
            compatible = destination->desc.shape[axis] == input->desc.shape[axis];
        if (!compatible)
            continue;
        if (copy_state_tensor(destination, input, error))
            goto failure;
        free(destination->quantized_channel_scales);
        destination->quantized_bits = input->quantized_bits;
        destination->quantized_axis = input->quantized_axis;
        destination->quantized_scale = input->quantized_scale;
        destination->quantized_zero_point = input->quantized_zero_point;
        destination->quantized_channel_scales = quantization_copies[index];
        destination->quantized_channel_count = input->quantized_channel_count;
        memcpy(destination->quantized_calibration, input->quantized_calibration,
               sizeof(destination->quantized_calibration));
        quantization_copies[index] = NULL;
        destination->storage->version++;
        destination->version++;
    }
    free(used);
    free_quantization_copies(quantization_copies, target->count);
    oasp_ai_state_dict_release(target);
    if (!diagnostics)
        oasp_ai_state_diagnostics_free(result);
    return 0;

failure:
    free(used);
    free_quantization_copies(quantization_copies, target ? target->count : 0);
    oasp_ai_state_dict_release(target);
    if (!error || !error->message)
        oasp_ai_set_error(error, 7, "LoadStateDict", "unable to load state dictionary", "Reduce memory pressure and validate the state.");
    if (!diagnostics)
        oasp_ai_state_diagnostics_free(result);
    return -1;
}

static int convert_module(oasp_ai_module *module, oasp_ai_dtype dtype, oasp_ai_error *error) {
    for (size_t group = 0; group < 2; group++) {
        oasp_ai_named_tensor *items = group ? module->buffers : module->parameters;
        size_t count = group ? module->buffer_count : module->parameter_count;
        for (size_t index = 0; index < count; index++) {
            oasp_ai_tensor *converted = oasp_ai_tensor_cast(items[index].tensor, dtype, error);
            if (!converted)
                return -1;
            if (!group) {
                converted->desc.flags |= OASP_AI_TENSOR_PARAMETER;
                if (oasp_ai_tensor_requires_grad(converted, 1, error)) {
                    oasp_ai_tensor_release(converted);
                    return -1;
                }
            }
            oasp_ai_tensor_release(items[index].tensor);
            items[index].tensor = converted;
        }
    }
    for (size_t index = 0; index < module->child_count; index++)
        if (convert_module(module->children[index].module, dtype, error))
            return -1;
    return 0;
}

int oasp_ai_module_to_dtype(oasp_ai_module *module, oasp_ai_dtype dtype, oasp_ai_error *error) {
    if (!module || !oasp_ai_dtype_size(dtype)) {
        oasp_ai_set_error(error, 5, "Module.ToDType", "module or dtype is invalid", "Pass a valid Module and supported dtype.");
        return -1;
    }
    return convert_module(module, dtype, error);
}

int oasp_ai_module_to_device(oasp_ai_module *module, const char *device, oasp_ai_error *error) {
    if (!module || !device) {
        oasp_ai_set_error(error, 5, "Module.ToDevice", "module and device are required", "Pass an available device name.");
        return -1;
    }
    if (oasp_ai_backend_find(module->context, device, error) == -2)
        return -1;
    for (size_t group = 0; group < 2; group++) {
        oasp_ai_named_tensor *items = group ? module->buffers : module->parameters;
        size_t count = group ? module->buffer_count : module->parameter_count;
        for (size_t index = 0; index < count; index++) {
            oasp_ai_tensor *migrated = oasp_ai_tensor_to_device(items[index].tensor, device, error);
            if (!migrated)
                return -1;
            oasp_ai_tensor_release(items[index].tensor);
            items[index].tensor = migrated;
        }
    }
    for (size_t index = 0; index < module->child_count; index++)
        if (oasp_ai_module_to_device(module->children[index].module, device, error))
            return -1;
    return 0;
}

uint64_t oasp_ai_module_schema_hash(const oasp_ai_module *module) {
    oasp_ai_state_dict *state = oasp_ai_module_state_dict(module, NULL);
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!state)
        return 0;
    for (size_t index = 0; index < state->count; index++) {
        const unsigned char *name = (const unsigned char *)state->entries[index].name;
        while (*name) {
            hash ^= *name++;
            hash *= UINT64_C(1099511628211);
        }
        hash ^= state->entries[index].kind;
        hash *= UINT64_C(1099511628211);
        hash ^= state->entries[index].tensor->desc.dtype;
        hash *= UINT64_C(1099511628211);
        for (uint32_t axis = 0; axis < state->entries[index].tensor->desc.rank; axis++) {
            hash ^= (uint64_t)state->entries[index].tensor->desc.shape[axis];
            hash *= UINT64_C(1099511628211);
        }
    }
    oasp_ai_state_dict_release(state);
    return hash;
}
