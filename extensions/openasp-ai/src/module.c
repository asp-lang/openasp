// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

oasp_ai_module *oasp_ai_module_create(oasp_ai_context *context, const char *name, oasp_ai_error *error) {
    if (!context || !name || !*name) {
        oasp_ai_set_error(error, 5, "Module", "module name is required", "Pass a stable non-empty module name.");
        return NULL;
    }
    oasp_ai_module *module = calloc(1, sizeof(*module));
    if (!module) {
        oasp_ai_set_error(error, 7, "Module", "unable to allocate module", "Reduce process memory pressure.");
        return NULL;
    }
    module->name = oasp_ai_string_duplicate(name);
    if (!module->name) {
        free(module);
        oasp_ai_set_error(error, 7, "Module", "unable to copy module name", "Reduce process memory pressure.");
        return NULL;
    }
    atomic_init(&module->references, 1);
    module->context = context;
    module->training = 1;
    return module;
}

oasp_ai_module *oasp_ai_module_linear(oasp_ai_context *context, uint64_t input_features, uint64_t output_features, int bias, oasp_ai_error *error) {
    if (!input_features || !output_features || input_features > INT64_MAX || output_features > INT64_MAX)
        return NULL;
    oasp_ai_module *module = oasp_ai_module_create(context, "Linear", error);
    int64_t weight_shape[] = {(int64_t)input_features, (int64_t)output_features};
    int64_t bias_shape[] = {(int64_t)output_features};
    double deviation = sqrt(2.0 / (double)input_features);
    oasp_ai_tensor *weight = module ? oasp_ai_tensor_random(context, weight_shape, 2, OASP_AI_DTYPE_F32, 0, deviation, 1, error) : NULL;
    oasp_ai_tensor *bias_tensor = module && bias ? oasp_ai_tensor_full(context, bias_shape, 1, OASP_AI_DTYPE_F32, 0, error) : NULL;
    if (!module || !weight || (bias && !bias_tensor) || oasp_ai_module_register_parameter(module, "weight", weight, error) ||
        (bias && oasp_ai_module_register_parameter(module, "bias", bias_tensor, error))) {
        oasp_ai_tensor_release(weight);
        oasp_ai_tensor_release(bias_tensor);
        oasp_ai_module_release(module);
        return NULL;
    }
    module->kind = OASP_AI_MODULE_LINEAR;
    module->feature_size = output_features;
    oasp_ai_tensor_release(weight);
    oasp_ai_tensor_release(bias_tensor);
    return module;
}

oasp_ai_module *oasp_ai_module_embedding(oasp_ai_context *context, uint64_t vocabulary, uint64_t dimensions, oasp_ai_error *error) {
    if (!vocabulary || !dimensions || vocabulary > INT64_MAX || dimensions > INT64_MAX)
        return NULL;
    oasp_ai_module *module = oasp_ai_module_create(context, "Embedding", error);
    int64_t shape[] = {(int64_t)vocabulary, (int64_t)dimensions};
    oasp_ai_tensor *weight = module ? oasp_ai_tensor_random(context, shape, 2, OASP_AI_DTYPE_F32, 0, 0.02, 1, error) : NULL;
    if (!module || !weight || oasp_ai_module_register_parameter(module, "weight", weight, error)) {
        oasp_ai_tensor_release(weight);
        oasp_ai_module_release(module);
        return NULL;
    }
    module->kind = OASP_AI_MODULE_EMBEDDING;
    module->feature_size = dimensions;
    oasp_ai_tensor_release(weight);
    return module;
}

oasp_ai_module *oasp_ai_module_activation(oasp_ai_context *context, oasp_ai_module_kind kind, double argument, oasp_ai_error *error) {
    if (kind != OASP_AI_MODULE_RELU && kind != OASP_AI_MODULE_DROPOUT && kind != OASP_AI_MODULE_SEQUENTIAL)
        return NULL;
    const char *name = kind == OASP_AI_MODULE_RELU ? "ReLU" : kind == OASP_AI_MODULE_DROPOUT ? "Dropout" : "Sequential";
    oasp_ai_module *module = oasp_ai_module_create(context, name, error);
    if (module) {
        module->kind = kind;
        module->argument = argument;
    }
    return module;
}

oasp_ai_module *oasp_ai_module_normalization(oasp_ai_context *context, uint64_t dimensions, double epsilon, int rms_only, oasp_ai_error *error) {
    if (!dimensions || dimensions > INT64_MAX || epsilon <= 0)
        return NULL;
    oasp_ai_module *module = oasp_ai_module_create(context, rms_only ? "RMSNorm" : "LayerNorm", error);
    int64_t shape[] = {(int64_t)dimensions};
    oasp_ai_tensor *weight = module ? oasp_ai_tensor_full(context, shape, 1, OASP_AI_DTYPE_F32, 1, error) : NULL;
    oasp_ai_tensor *bias = module && !rms_only ? oasp_ai_tensor_full(context, shape, 1, OASP_AI_DTYPE_F32, 0, error) : NULL;
    if (!module || !weight || (!rms_only && !bias) || oasp_ai_module_register_parameter(module, "weight", weight, error) ||
        (!rms_only && oasp_ai_module_register_parameter(module, "bias", bias, error))) {
        oasp_ai_tensor_release(weight);
        oasp_ai_tensor_release(bias);
        oasp_ai_module_release(module);
        return NULL;
    }
    module->kind = rms_only ? OASP_AI_MODULE_RMS_NORM : OASP_AI_MODULE_LAYER_NORM;
    module->argument = epsilon;
    module->feature_size = dimensions;
    oasp_ai_tensor_release(weight);
    oasp_ai_tensor_release(bias);
    return module;
}

void oasp_ai_module_retain(oasp_ai_module *module) {
    if (module)
        atomic_fetch_add_explicit(&module->references, 1, memory_order_relaxed);
}

void oasp_ai_module_release(oasp_ai_module *module) {
    if (!module || atomic_fetch_sub_explicit(&module->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t index = 0; index < module->parameter_count; index++) {
        free(module->parameters[index].name);
        oasp_ai_tensor_release(module->parameters[index].tensor);
    }
    for (size_t index = 0; index < module->buffer_count; index++) {
        free(module->buffers[index].name);
        oasp_ai_tensor_release(module->buffers[index].tensor);
    }
    for (size_t index = 0; index < module->child_count; index++) {
        free(module->children[index].name);
        oasp_ai_module_release(module->children[index].module);
    }
    free(module->parameters);
    free(module->buffers);
    free(module->children);
    free(module->name);
    free(module);
}

static int valid_registration_name(const char *name) {
    if (!name || !*name || strchr(name, '.'))
        return 0;
    for (const unsigned char *current = (const unsigned char *)name; *current; current++)
        if (!((*current >= 'a' && *current <= 'z') || (*current >= 'A' && *current <= 'Z') || (*current >= '0' && *current <= '9') || *current == '_'))
            return 0;
    return 1;
}

static int name_exists(const oasp_ai_module *module, const char *name) {
    for (size_t index = 0; index < module->parameter_count; index++)
        if (!strcmp(module->parameters[index].name, name))
            return 1;
    for (size_t index = 0; index < module->buffer_count; index++)
        if (!strcmp(module->buffers[index].name, name))
            return 1;
    for (size_t index = 0; index < module->child_count; index++)
        if (!strcmp(module->children[index].name, name))
            return 1;
    return 0;
}

static int module_contains(const oasp_ai_module *root, const oasp_ai_module *target) {
    if (root == target)
        return 1;
    for (size_t index = 0; index < root->child_count; index++)
        if (module_contains(root->children[index].module, target))
            return 1;
    return 0;
}

int oasp_ai_module_register_module(oasp_ai_module *module, const char *name, oasp_ai_module *child, oasp_ai_error *error) {
    if (!module || !child || !valid_registration_name(name) || name_exists(module, name)) {
        oasp_ai_set_error(error, 5, "RegisterModule", "module registration name is invalid or already used", "Use a unique identifier without dots.");
        return -1;
    }
    for (size_t index = 0; index < module->child_count; index++)
        if (module->children[index].module == child)
            return 0;
    if (module_contains(child, module)) {
        oasp_ai_set_error(error, 5, "RegisterModule", "module registration would create a cycle", "Remove the parent reference from the child module.");
        return -1;
    }
    oasp_ai_named_module *grown =
        oasp_ai_grow_array(module->children, sizeof(*module->children), &module->child_capacity, module->child_count + 1);
    if (!grown)
        return -1;
    module->children = grown;
    oasp_ai_named_module *entry = &module->children[module->child_count++];
    entry->name = oasp_ai_string_duplicate(name);
    entry->module = child;
    if (!entry->name) {
        module->child_count--;
        return -1;
    }
    oasp_ai_module_retain(child);
    return 0;
}

static int register_tensor(oasp_ai_module *module, const char *name, oasp_ai_tensor *tensor, int parameter, int persistent,
                           oasp_ai_error *error) {
    if (!module || !tensor || !valid_registration_name(name) || name_exists(module, name)) {
        oasp_ai_set_error(error, 5, parameter ? "RegisterParameter" : "RegisterBuffer", "tensor registration name is invalid or already used",
                          "Use a unique identifier without dots.");
        return -1;
    }
    oasp_ai_named_tensor **items = parameter ? &module->parameters : &module->buffers;
    size_t *count = parameter ? &module->parameter_count : &module->buffer_count;
    size_t *capacity = parameter ? &module->parameter_capacity : &module->buffer_capacity;
    oasp_ai_named_tensor *grown = oasp_ai_grow_array(*items, sizeof(**items), capacity, *count + 1);
    if (!grown)
        return -1;
    *items = grown;
    oasp_ai_named_tensor *entry = &grown[(*count)++];
    entry->name = oasp_ai_string_duplicate(name);
    entry->tensor = tensor;
    entry->persistent = persistent;
    if (!entry->name) {
        (*count)--;
        return -1;
    }
    oasp_ai_tensor_retain(tensor);
    if (parameter) {
        tensor->desc.flags |= OASP_AI_TENSOR_PARAMETER;
        if (oasp_ai_tensor_requires_grad(tensor, 1, error)) {
            tensor->desc.flags &= ~OASP_AI_TENSOR_PARAMETER;
            oasp_ai_tensor_release(tensor);
            free(entry->name);
            memset(entry, 0, sizeof(*entry));
            (*count)--;
            return -1;
        }
    }
    return 0;
}

int oasp_ai_module_register_parameter(oasp_ai_module *module, const char *name, oasp_ai_tensor *tensor, oasp_ai_error *error) {
    return register_tensor(module, name, tensor, 1, 1, error);
}

int oasp_ai_module_register_buffer(oasp_ai_module *module, const char *name, oasp_ai_tensor *tensor, int persistent, oasp_ai_error *error) {
    return register_tensor(module, name, tensor, 0, persistent, error);
}

size_t oasp_ai_module_parameter_count(const oasp_ai_module *module) {
    if (!module)
        return 0;
    size_t count = module->parameter_count;
    for (size_t index = 0; index < module->child_count; index++)
        count += oasp_ai_module_parameter_count(module->children[index].module);
    return count;
}

static oasp_ai_tensor *module_parameter_at(const oasp_ai_module *module, size_t *index, char *path, size_t path_size, size_t prefix,
                                           const char **name) {
    if (*index < module->parameter_count) {
        const char *leaf = module->parameters[*index].name;
        size_t length = strlen(leaf);
        if (prefix + length + 1 <= path_size) {
            memcpy(path + prefix, leaf, length + 1);
            *name = path;
        }
        return module->parameters[*index].tensor;
    }
    *index -= module->parameter_count;
    for (size_t child = 0; child < module->child_count; child++) {
        size_t count = oasp_ai_module_parameter_count(module->children[child].module);
        if (*index < count) {
            size_t length = strlen(module->children[child].name);
            if (prefix + length + 2 > path_size)
                return NULL;
            memcpy(path + prefix, module->children[child].name, length);
            path[prefix + length] = '.';
            return module_parameter_at(module->children[child].module, index, path, path_size, prefix + length + 1, name);
        }
        *index -= count;
    }
    return NULL;
}

oasp_ai_tensor *oasp_ai_module_parameter(const oasp_ai_module *module, size_t index, const char **name) {
    static _Thread_local char path[4096];
    if (!module)
        return NULL;
    const char *ignored = NULL;
    oasp_ai_tensor *tensor = module_parameter_at(module, &index, path, sizeof(path), 0, name ? name : &ignored);
    if (tensor)
        oasp_ai_tensor_retain(tensor);
    return tensor;
}

void oasp_ai_module_train(oasp_ai_module *module, int training) {
    if (!module)
        return;
    module->training = !!training;
    for (size_t index = 0; index < module->child_count; index++)
        oasp_ai_module_train(module->children[index].module, training);
}

int oasp_ai_module_add(oasp_ai_module *module, oasp_ai_module *child, oasp_ai_error *error) {
    if (!module || module->kind != OASP_AI_MODULE_SEQUENTIAL) {
        oasp_ai_set_error(error, 5, "Sequential.Add", "Add is only valid on a Sequential module", "Create the module with ai.Sequential().");
        return -1;
    }
    char name[32];
    snprintf(name, sizeof(name), "%zu", module->child_count);
    return oasp_ai_module_register_module(module, name, child, error);
}

oasp_ai_tensor *oasp_ai_module_forward(oasp_ai_module *module, oasp_ai_tensor *input, oasp_ai_error *error) {
    if (!module || !input)
        return NULL;
    if (module->kind == OASP_AI_MODULE_GENERIC) {
        oasp_ai_set_error(error, 5, "Module.Forward", "generic Module is a state container and has no native Forward",
                          "Implement Forward in the ASP Class or use a built-in layer.");
        return NULL;
    }
    if (module->kind == OASP_AI_MODULE_SEQUENTIAL) {
        oasp_ai_tensor *current = input;
        oasp_ai_tensor_retain(current);
        for (size_t index = 0; index < module->child_count; index++) {
            oasp_ai_tensor *next = oasp_ai_module_forward(module->children[index].module, current, error);
            oasp_ai_tensor_release(current);
            if (!next)
                return NULL;
            current = next;
        }
        return current;
    }
    if (module->kind == OASP_AI_MODULE_LINEAR) {
        oasp_ai_tensor *weight = module->parameters[0].tensor;
        oasp_ai_tensor *bias = module->parameter_count > 1 ? module->parameters[1].tensor : NULL;
        return oasp_ai_linear(module->context, input, weight, bias, error);
    }
    if (module->kind == OASP_AI_MODULE_RELU)
        return oasp_ai_unary(module->context, OASP_AI_OP_RELU, input, 0, error);
    if (module->kind == OASP_AI_MODULE_DROPOUT) {
        if (!module->training || module->argument == 0)
            return oasp_ai_tensor_clone(input, error);
        if (module->argument < 0 || module->argument >= 1) {
            oasp_ai_set_error(error, 5, "Dropout", "dropout probability must be in [0,1)", "Pass a valid probability.");
            return NULL;
        }
        oasp_ai_tensor *random = oasp_ai_tensor_random(module->context, input->desc.shape, input->desc.rank, OASP_AI_DTYPE_F32, 0, 1, 0, error);
        oasp_ai_tensor *result = oasp_ai_tensor_like(input, 1, error);
        if (!random || !result) {
            oasp_ai_tensor_release(random);
            oasp_ai_tensor_release(result);
            return NULL;
        }
        double scale = 1 / (1 - module->argument);
        for (uint64_t index = 0; index < input->element_count; index++)
            oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(random, index) >= module->argument ? oasp_ai_tensor_read(input, index) * scale : 0);
        oasp_ai_tensor_release(random);
        return result;
    }
    if (module->kind == OASP_AI_MODULE_LAYER_NORM || module->kind == OASP_AI_MODULE_RMS_NORM) {
        oasp_ai_tensor *weight = module->parameters[0].tensor;
        oasp_ai_tensor *bias = module->kind == OASP_AI_MODULE_LAYER_NORM ? module->parameters[1].tensor : NULL;
        return oasp_ai_layer_norm(module->context, input, weight, bias, module->argument, module->kind == OASP_AI_MODULE_RMS_NORM, error);
    }
    if (module->kind == OASP_AI_MODULE_EMBEDDING)
        return oasp_ai_gather(module->context, module->parameters[0].tensor, input, 0, error);
    return NULL;
}
