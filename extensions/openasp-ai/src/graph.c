// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <stdlib.h>
#include <string.h>

oasp_ai_ir_value *oasp_ai_graph_add_value_internal(oasp_ai_graph *graph, const char *name, const oasp_ai_tensor_desc *desc, oasp_ai_error *error) {
    oasp_ai_ir_value *grown = oasp_ai_grow_array(graph->values, sizeof(*graph->values), &graph->value_capacity, graph->value_count + 1);
    if (!grown) {
        oasp_ai_set_error(error, 7, "Graph", "unable to grow the SSA value table", "Reduce graph size or memory pressure.");
        return NULL;
    }
    graph->values = grown;
    oasp_ai_ir_value *value = &graph->values[graph->value_count++];
    memset(value, 0, sizeof(*value));
    value->id = (uint32_t)graph->value_count;
    value->producer = UINT32_MAX;
    value->desc = *desc;
    int has_strides = 0;
    for (uint32_t axis = 0; axis < value->desc.rank; axis++)
        has_strides |= value->desc.strides[axis] != 0;
    if (!has_strides) {
        int64_t stride = 1;
        for (uint32_t axis = value->desc.rank; axis; axis--) {
            value->desc.strides[axis - 1] = stride;
            if (value->desc.shape[axis - 1] > 0)
                stride *= value->desc.shape[axis - 1];
        }
        value->desc.flags |= OASP_AI_TENSOR_CONTIGUOUS;
    }
    value->name = oasp_ai_string_duplicate(name);
    return value;
}

oasp_ai_graph *oasp_ai_graph_create(oasp_ai_context *context, const char *name, oasp_ai_error *error) {
    if (!context || !name || !*name) {
        oasp_ai_set_error(error, 5, "Graph", "graph name is required", "Pass a stable graph name.");
        return NULL;
    }
    oasp_ai_graph *graph = calloc(1, sizeof(*graph));
    if (!graph)
        return NULL;
    graph->name = oasp_ai_string_duplicate(name);
    if (!graph->name) {
        free(graph);
        return NULL;
    }
    atomic_init(&graph->references, 1);
    graph->context = context;
    return graph;
}

static void graph_retain(oasp_ai_graph *graph) {
    if (graph)
        atomic_fetch_add_explicit(&graph->references, 1, memory_order_relaxed);
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t graph_cache_key(oasp_ai_graph *graph, const char *device) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_bytes(hash, device, strlen(device));
    hash = hash_bytes(hash, &graph->context->options.deterministic, sizeof(graph->context->options.deterministic));
    for (size_t index = 0; index < graph->value_count; index++) {
        oasp_ai_ir_value *value = &graph->values[index];
        hash = hash_bytes(hash, &value->desc.dtype, sizeof(value->desc.dtype));
        hash = hash_bytes(hash, &value->desc.rank, sizeof(value->desc.rank));
        hash = hash_bytes(hash, value->desc.shape, value->desc.rank * sizeof(*value->desc.shape));
        if (value->constant) {
            if (value->constant->desc.flags & OASP_AI_TENSOR_CONTIGUOUS) {
                uint32_t checksum = oasp_ai_crc32(oasp_ai_tensor_const_data(value->constant),
                                                  (size_t)oasp_ai_tensor_bytes(value->constant));
                hash = hash_bytes(hash, &checksum, sizeof(checksum));
            } else
                for (uint64_t element = 0; element < value->constant->element_count; element++) {
                    double scalar = oasp_ai_tensor_read(value->constant, element);
                    hash = hash_bytes(hash, &scalar, sizeof(scalar));
                }
            hash = hash_bytes(hash, &value->constant->quantized_bits, sizeof(value->constant->quantized_bits));
            hash = hash_bytes(hash, &value->constant->quantized_axis, sizeof(value->constant->quantized_axis));
            hash = hash_bytes(hash, &value->constant->quantized_scale, sizeof(value->constant->quantized_scale));
            hash = hash_bytes(hash, &value->constant->quantized_zero_point, sizeof(value->constant->quantized_zero_point));
            hash = hash_bytes(hash, value->constant->quantized_calibration,
                              sizeof(value->constant->quantized_calibration));
            hash = hash_bytes(hash, value->constant->quantized_channel_scales,
                              value->constant->quantized_channel_count * sizeof(*value->constant->quantized_channel_scales));
        }
    }
    for (size_t index = 0; index < graph->operation_count; index++) {
        oasp_ai_ir_operation *operation = &graph->operations[index];
        hash = hash_bytes(hash, &operation->op_id, sizeof(operation->op_id));
        hash = hash_bytes(hash, &operation->op_version, sizeof(operation->op_version));
        hash = hash_bytes(hash, operation->inputs, operation->input_count * sizeof(*operation->inputs));
        hash = hash_bytes(hash, operation->attributes, operation->attributes_size);
    }
    hash = hash_bytes(hash, graph->outputs, graph->output_count * sizeof(*graph->outputs));
    return hash;
}

void oasp_ai_graph_release(oasp_ai_graph *graph) {
    if (!graph || atomic_fetch_sub_explicit(&graph->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t index = 0; index < graph->value_count; index++) {
        free(graph->values[index].name);
        oasp_ai_tensor_release(graph->values[index].constant);
    }
    for (size_t index = 0; index < graph->operation_count; index++) {
        free(graph->operations[index].inputs);
        free(graph->operations[index].attributes);
    }
    free(graph->name);
    free(graph->values);
    free(graph->operations);
    free(graph->inputs);
    free(graph->outputs);
    free(graph);
}

uint32_t oasp_ai_graph_input(oasp_ai_graph *graph, const char *name, const oasp_ai_tensor_desc *desc, oasp_ai_error *error) {
    if (!graph || !desc || !desc->rank || desc->rank > OASP_AI_MAX_RANK)
        return 0;
    for (uint32_t axis = 0; axis < desc->rank; axis++)
        if (desc->shape[axis] <= 0 && desc->shape[axis] != -1) {
            oasp_ai_set_error(error, 5, "Graph.Input", "graph dimensions must be positive or dynamic",
                              "Use -1 for a symbolic dimension or a positive fixed dimension.");
            return 0;
        }
    oasp_ai_ir_value *value = oasp_ai_graph_add_value_internal(graph, name, desc, error);
    if (!value)
        return 0;
    uint32_t *grown = oasp_ai_grow_array(graph->inputs, sizeof(*graph->inputs), &graph->input_capacity, graph->input_count + 1);
    if (!grown) {
        graph->value_count--;
        free(value->name);
        return 0;
    }
    graph->inputs = grown;
    graph->inputs[graph->input_count++] = value->id;
    return value->id;
}

uint32_t oasp_ai_graph_constant(oasp_ai_graph *graph, const char *name, oasp_ai_tensor *tensor, oasp_ai_error *error) {
    if (!graph || !tensor)
        return 0;
    oasp_ai_ir_value *value = oasp_ai_graph_add_value_internal(graph, name, &tensor->desc, error);
    if (!value)
        return 0;
    value->constant = tensor;
    oasp_ai_tensor_retain(tensor);
    return value->id;
}

static int infer_binary(const oasp_ai_tensor_desc *left, const oasp_ai_tensor_desc *right, oasp_ai_tensor_desc *output) {
    output->rank = left->rank > right->rank ? left->rank : right->rank;
    output->dtype = left->dtype > right->dtype ? left->dtype : right->dtype;
    for (uint32_t axis = 0; axis < output->rank; axis++) {
        int left_axis = (int)axis - (int)(output->rank - left->rank);
        int right_axis = (int)axis - (int)(output->rank - right->rank);
        int64_t left_size = left_axis >= 0 ? left->shape[left_axis] : 1;
        int64_t right_size = right_axis >= 0 ? right->shape[right_axis] : 1;
        if (left_size != right_size && left_size != 1 && right_size != 1 && left_size != -1 && right_size != -1)
            return -1;
        output->shape[axis] = left_size == -1 || right_size == -1 ? -1 : left_size > right_size ? left_size : right_size;
    }
    return 0;
}

static int infer_operation(oasp_ai_graph *graph, oasp_ai_operator operation, const uint32_t *inputs, size_t input_count, oasp_ai_tensor_desc *output,
                           oasp_ai_error *error) {
    for (size_t index = 0; index < input_count; index++)
        if (!inputs[index] || inputs[index] > graph->value_count) {
            oasp_ai_set_error(error, 5, "Graph", "operation references an unknown SSA value", "Use value IDs returned by this graph.");
            return -1;
        }
    if (!input_count)
        return -1;
    const oasp_ai_tensor_desc *first = &graph->values[inputs[0] - 1].desc;
    *output = *first;
    if (operation >= OASP_AI_OP_ADD && operation <= OASP_AI_OP_MAXIMUM) {
        if (input_count != 2 || infer_binary(first, &graph->values[inputs[1] - 1].desc, output)) {
            oasp_ai_set_error(error, 5, "Graph", "binary operation shapes are incompatible", "Use broadcast-compatible input shapes.");
            return -1;
        }
    } else if (operation == OASP_AI_OP_MATMUL || operation == OASP_AI_OP_BATCHED_MATMUL) {
        if (input_count != 2 || first->rank < 2) {
            oasp_ai_set_error(error, 5, "Graph", "MatMul requires two rank-two-or-higher values", "Check graph input ranks.");
            return -1;
        }
        const oasp_ai_tensor_desc *right = &graph->values[inputs[1] - 1].desc;
        if (right->rank != first->rank ||
            (first->shape[first->rank - 1] != -1 && right->shape[right->rank - 2] != -1 &&
             first->shape[first->rank - 1] != right->shape[right->rank - 2])) {
            oasp_ai_set_error(error, 5, "Graph", "MatMul dimensions are incompatible", "The inner dimensions and ranks must match.");
            return -1;
        }
        output->shape[output->rank - 1] = right->shape[right->rank - 1];
    } else if (operation == OASP_AI_OP_CROSS_ENTROPY) {
        output->rank = 1;
        output->shape[0] = 1;
        output->dtype = OASP_AI_DTYPE_F32;
    } else if (operation == OASP_AI_OP_ATTENTION && input_count >= 3) {
        const oasp_ai_tensor_desc *value = &graph->values[inputs[2] - 1].desc;
        if (value->rank != first->rank)
            return -1;
        output->shape[output->rank - 1] = value->shape[value->rank - 1];
    } else if (operation == OASP_AI_OP_GATHER && input_count == 2) {
        const oasp_ai_tensor_desc *indices = &graph->values[inputs[1] - 1].desc;
        if (indices->rank + first->rank - 1 > OASP_AI_MAX_RANK)
            return -1;
        output->rank = indices->rank + first->rank - 1;
        for (uint32_t axis = 0; axis < indices->rank; axis++)
            output->shape[axis] = indices->shape[axis];
        for (uint32_t axis = 1; axis < first->rank; axis++)
            output->shape[indices->rank + axis - 1] = first->shape[axis];
    }
    return 0;
}

uint32_t oasp_ai_graph_operation(oasp_ai_graph *graph, oasp_ai_operator operation, const uint32_t *inputs, size_t input_count, const void *attributes,
                                 size_t attributes_size, oasp_ai_error *error) {
    if (!graph || !inputs || !input_count || operation < OASP_AI_OP_ZEROS || operation > OASP_AI_OP_SOFTMAX) {
        oasp_ai_set_error(error, 5, "Graph", "invalid graph operation", "Pass a standard operator and at least one input.");
        return 0;
    }
    oasp_ai_tensor_desc output_desc = {0};
    if (infer_operation(graph, operation, inputs, input_count, &output_desc, error))
        return 0;
    oasp_ai_ir_operation *grown =
        oasp_ai_grow_array(graph->operations, sizeof(*graph->operations), &graph->operation_capacity, graph->operation_count + 1);
    if (!grown)
        return 0;
    graph->operations = grown;
    oasp_ai_ir_operation *record = &graph->operations[graph->operation_count];
    memset(record, 0, sizeof(*record));
    record->inputs = malloc(input_count * sizeof(*record->inputs));
    record->attributes = attributes_size ? malloc(attributes_size) : NULL;
    if (!record->inputs || (attributes_size && !record->attributes)) {
        free(record->inputs);
        free(record->attributes);
        return 0;
    }
    memcpy(record->inputs, inputs, input_count * sizeof(*inputs));
    if (attributes_size)
        memcpy(record->attributes, attributes, attributes_size);
    record->op_id = (uint32_t)operation;
    record->op_version = 1;
    record->input_count = input_count;
    record->attributes_size = attributes_size;
    char name[32];
    snprintf(name, sizeof(name), "value.%zu", graph->value_count + 1);
    oasp_ai_ir_value *output = oasp_ai_graph_add_value_internal(graph, name, &output_desc, error);
    if (!output) {
        free(record->inputs);
        free(record->attributes);
        return 0;
    }
    output->producer = (uint32_t)graph->operation_count;
    record->output = output->id;
    graph->operation_count++;
    return output->id;
}

int oasp_ai_graph_set_outputs(oasp_ai_graph *graph, const uint32_t *outputs, size_t output_count, oasp_ai_error *error) {
    if (!graph || !outputs || !output_count) {
        oasp_ai_set_error(error, 5, "Graph", "graph requires at least one output", "Pass SSA value IDs produced by this graph.");
        return -1;
    }
    uint32_t *copy = malloc(output_count * sizeof(*copy));
    if (!copy)
        return -1;
    for (size_t index = 0; index < output_count; index++)
        if (!outputs[index] || outputs[index] > graph->value_count) {
            free(copy);
            oasp_ai_set_error(error, 5, "Graph", "graph output references an unknown SSA value", "Use value IDs produced by this graph.");
            return -1;
        }
    memcpy(copy, outputs, output_count * sizeof(*copy));
    free(graph->outputs);
    graph->outputs = copy;
    graph->output_count = output_count;
    return 0;
}

int oasp_ai_graph_verify(oasp_ai_graph *graph, oasp_ai_error *error) {
    if (!graph || !graph->input_count || !graph->output_count) {
        oasp_ai_set_error(error, 5, "Verify", "graph must have inputs and outputs", "Declare graph inputs and call SetOutputs.");
        return -1;
    }
    for (size_t operation = 0; operation < graph->operation_count; operation++) {
        oasp_ai_ir_operation *record = &graph->operations[operation];
        if (record->output > graph->value_count || graph->values[record->output - 1].producer != operation) {
            oasp_ai_set_error(error, 5, "Verify", "SSA output has an invalid producer", "Rebuild the graph; every value must have exactly one producer.");
            return -1;
        }
        for (size_t input = 0; input < record->input_count; input++) {
            uint32_t value_id = record->inputs[input];
            if (!value_id || value_id >= record->output) {
                oasp_ai_set_error(error, 5, "Verify", "operation uses a value before its definition", "Construct operations in dependency order.");
                return -1;
            }
        }
    }
    return 0;
}

static int select_graph_backend(oasp_ai_graph *graph, const char *device, oasp_ai_error *error) {
    if (!strncmp(device, "cpu", 3))
        return -1;
    int selected = oasp_ai_backend_find(graph->context, device, error);
    if (selected < 0)
        return -1;
    for (size_t backend_index = (size_t)selected; backend_index == (size_t)selected; backend_index++) {
        oasp_ai_registered_backend *backend = &graph->context->backends[backend_index];
        const oasp_ai_backend_api *api = backend->descriptor->api;
        if (!api->query_capabilities || !api->alloc || !api->free || !api->copy || !api->wait_event ||
            !api->synchronize || !api->compile_graph || !api->execute_graph || !api->destroy_executable)
            return -1;
        for (size_t value_index = 0; value_index < graph->value_count; value_index++)
            for (uint32_t axis = 0; axis < graph->values[value_index].desc.rank; axis++)
                if (graph->values[value_index].desc.shape[axis] < 1)
                    return -1;
        for (size_t operation_index = 0; operation_index < graph->operation_count; operation_index++) {
            oasp_ai_ir_operation *operation = &graph->operations[operation_index];
            oasp_ai_ir_value *input = &graph->values[operation->inputs[0] - 1];
            oasp_ai_ir_value *output = &graph->values[operation->output - 1];
            oasp_ai_capability_query query = {
                .struct_size = sizeof(query),
                .op_id = operation->op_id,
                .op_version = operation->op_version,
                .flags = operation->flags,
                .input_dtype = input->desc.dtype,
                .output_dtype = output->desc.dtype,
                .minimum_rank = input->desc.rank,
                .maximum_rank = input->desc.rank,
                .deterministic = graph->context->options.deterministic,
                .training = graph->context->options.training_enabled,
            };
            if (!backend->descriptor->api->query_capabilities ||
                backend->descriptor->api->query_capabilities(backend->device, &query, error) || !query.supported ||
                (graph->context->options.deterministic && !query.deterministic))
                return -1;
        }
        return (int)backend_index;
    }
    return -1;
}

oasp_ai_plan *oasp_ai_graph_compile(oasp_ai_graph *graph, const char *device, oasp_ai_error *error) {
    if (oasp_ai_graph_verify(graph, error))
        return NULL;
    uint64_t started = oasp_ai_now_nanoseconds();
    const char *target_device = device ? device : "cpu:0";
    uint64_t cache_key = graph_cache_key(graph, target_device);
    pthread_mutex_lock(&graph->context->mutex);
    for (size_t index = 0; index < graph->context->plan_cache_count; index++)
        if (graph->context->plan_cache[index].key == cache_key) {
            oasp_ai_plan *cached = graph->context->plan_cache[index].plan;
            graph->context->plan_cache[index].last_used = ++graph->context->plan_cache_clock;
            graph->context->metrics.graph_cache_hits++;
            atomic_fetch_add_explicit(&cached->references, 1, memory_order_relaxed);
            pthread_mutex_unlock(&graph->context->mutex);
            return cached;
        }
    pthread_mutex_unlock(&graph->context->mutex);
    int backend_index = select_graph_backend(graph, target_device, error);
    if (strncmp(target_device, "cpu", 3) && backend_index < 0 && graph->context->options.strict_device) {
        oasp_ai_set_error(error, 5, "Compile", "requested backend cannot execute the complete graph",
                          "Install a backend supporting every operator and static shape, or disable StrictDevice for whole-graph CPU fallback.");
        return NULL;
    }
    oasp_ai_plan *plan = calloc(1, sizeof(*plan));
    if (plan)
        plan->steps = calloc(graph->operation_count, sizeof(*plan->steps));
    if (plan)
        plan->value_slots = calloc(graph->value_count, sizeof(*plan->value_slots));
    if (!plan || (graph->operation_count && !plan->steps) || (graph->value_count && !plan->value_slots)) {
        free(plan ? plan->value_slots : NULL);
        free(plan);
        return NULL;
    }
    atomic_init(&plan->references, 1);
    plan->backend_index = -1;
    plan->graph = graph;
    graph_retain(graph);
    plan->step_count = graph->operation_count;
    plan->cache_key = cache_key;
    plan->device = oasp_ai_string_duplicate(target_device);
    if (!plan->device) {
        oasp_ai_plan_release(plan);
        return NULL;
    }
    size_t *last_use = calloc(graph->value_count, sizeof(*last_use));
    size_t *slot_end = calloc(graph->value_count, sizeof(*slot_end));
    if ((graph->value_count && !last_use) || (graph->value_count && !slot_end)) {
        free(last_use);
        free(slot_end);
        oasp_ai_plan_release(plan);
        return NULL;
    }
    for (size_t index = 0; index < graph->operation_count; index++)
        for (size_t input = 0; input < graph->operations[index].input_count; input++)
            last_use[graph->operations[index].inputs[input] - 1] = index + 1;
    for (size_t index = 0; index < graph->output_count; index++)
        last_use[graph->outputs[index] - 1] = graph->operation_count + 1;
    for (size_t value_index = 0; value_index < graph->value_count; value_index++) {
        size_t start = graph->values[value_index].producer == UINT32_MAX ? 0 : (size_t)graph->values[value_index].producer + 1;
        uint32_t slot = plan->slot_count;
        for (uint32_t candidate = 0; candidate < plan->slot_count; candidate++)
            if (slot_end[candidate] < start) {
                slot = candidate;
                break;
            }
        if (slot == plan->slot_count)
            plan->slot_count++;
        plan->value_slots[value_index] = slot;
        slot_end[slot] = last_use[value_index] > start ? last_use[value_index] : start;
    }
    free(last_use);
    free(slot_end);
    for (size_t index = 0; index < plan->step_count; index++) {
        oasp_ai_ir_operation *operation = &graph->operations[index];
        plan->steps[index].op_id = operation->op_id;
        plan->steps[index].op_version = operation->op_version;
        plan->steps[index].flags = operation->flags;
        plan->steps[index].input_count = operation->input_count;
        plan->steps[index].output_slot = plan->value_slots[operation->output - 1];
        plan->steps[index].input_slots = malloc(operation->input_count * sizeof(*plan->steps[index].input_slots));
        plan->steps[index].attributes = operation->attributes_size ? malloc(operation->attributes_size) : NULL;
        plan->steps[index].attributes_size = operation->attributes_size;
        if (!plan->steps[index].input_slots || (operation->attributes_size && !plan->steps[index].attributes)) {
            oasp_ai_plan_release(plan);
            return NULL;
        }
        for (size_t input = 0; input < operation->input_count; input++)
            plan->steps[index].input_slots[input] = plan->value_slots[operation->inputs[input] - 1];
        if (operation->attributes_size)
            memcpy(plan->steps[index].attributes, operation->attributes, operation->attributes_size);
    }
    if (backend_index >= 0) {
        oasp_ai_registered_backend *backend = &graph->context->backends[backend_index];
        oasp_ai_graph_desc descriptor = {.struct_size = sizeof(descriptor)};
        if (!oasp_ai_backend_ir_build(graph, &plan->backend_ir, &plan->backend_ir_size, error)) {
            descriptor.core_ir = plan->backend_ir;
            descriptor.core_ir_size = plan->backend_ir_size;
            if (backend->descriptor->api->compile_graph &&
                !backend->descriptor->api->compile_graph(backend->device, &descriptor, &plan->executable, error))
                plan->backend_index = backend_index;
        }
        if (plan->backend_index < 0) {
            if (plan->executable && backend->descriptor->api->destroy_executable)
                backend->descriptor->api->destroy_executable(plan->executable);
            plan->executable = 0;
            free(plan->backend_ir);
            plan->backend_ir = NULL;
            plan->backend_ir_size = 0;
            if (graph->context->options.strict_device) {
                oasp_ai_plan_release(plan);
                return NULL;
            }
        }
    }
    pthread_mutex_lock(&graph->context->mutex);
    for (size_t index = 0; index < graph->context->plan_cache_count; index++)
        if (graph->context->plan_cache[index].key == cache_key) {
            oasp_ai_plan *cached = graph->context->plan_cache[index].plan;
            graph->context->plan_cache[index].last_used = ++graph->context->plan_cache_clock;
            graph->context->metrics.graph_cache_hits++;
            atomic_fetch_add_explicit(&cached->references, 1, memory_order_relaxed);
            pthread_mutex_unlock(&graph->context->mutex);
            oasp_ai_plan_release(plan);
            return cached;
        }
    graph->context->metrics.graph_compile_count++;
    graph->context->metrics.graph_compile_nanoseconds += oasp_ai_now_nanoseconds() - started;
    if (strncmp(target_device, "cpu", 3) && plan->backend_index < 0)
        graph->context->metrics.fallback_count++;
    size_t maximum = graph->context->options.maximum_plan_cache_entries;
    if (maximum) {
        if (graph->context->plan_cache_count == maximum) {
            size_t victim = SIZE_MAX;
            uint64_t oldest = UINT64_MAX;
            for (size_t index = 0; index < graph->context->plan_cache_count; index++)
                if (atomic_load_explicit(&graph->context->plan_cache[index].plan->references, memory_order_relaxed) == 1 &&
                    graph->context->plan_cache[index].last_used < oldest) {
                    oldest = graph->context->plan_cache[index].last_used;
                    victim = index;
                }
            if (victim != SIZE_MAX) {
                oasp_ai_plan_release(graph->context->plan_cache[victim].plan);
                graph->context->plan_cache[victim] = graph->context->plan_cache[--graph->context->plan_cache_count];
                graph->context->metrics.graph_cache_evictions++;
            }
        }
        if (graph->context->plan_cache_count < maximum) {
            oasp_ai_plan_cache_entry *grown =
                realloc(graph->context->plan_cache, (graph->context->plan_cache_count + 1) * sizeof(*grown));
            if (grown) {
                graph->context->plan_cache = grown;
                atomic_fetch_add_explicit(&plan->references, 1, memory_order_relaxed);
                grown[graph->context->plan_cache_count++] =
                    (oasp_ai_plan_cache_entry){cache_key, ++graph->context->plan_cache_clock, plan};
            }
        }
    }
    pthread_mutex_unlock(&graph->context->mutex);
    return plan;
}

void oasp_ai_plan_release(oasp_ai_plan *plan) {
    if (!plan || atomic_fetch_sub_explicit(&plan->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t index = 0; index < plan->step_count; index++) {
        free(plan->steps[index].input_slots);
        free(plan->steps[index].attributes);
    }
    if (plan->backend_index >= 0 && plan->executable) {
        oasp_ai_registered_backend *backend = &plan->graph->context->backends[plan->backend_index];
        if (backend->descriptor->api->destroy_executable)
            backend->descriptor->api->destroy_executable(plan->executable);
    }
    free(plan->backend_ir);
    free(plan->steps);
    free(plan->value_slots);
    free(plan->device);
    oasp_ai_graph_release(plan->graph);
    free(plan);
}

static oasp_ai_tensor *execute_step(oasp_ai_plan *plan, oasp_ai_exec_step *step, oasp_ai_tensor **slots, oasp_ai_error *error) {
    oasp_ai_tensor *first = slots[step->input_slots[0]];
    oasp_ai_tensor *second = step->input_count > 1 ? slots[step->input_slots[1]] : NULL;
    if (step->op_id >= OASP_AI_OP_ADD && step->op_id <= OASP_AI_OP_MAXIMUM)
        return oasp_ai_binary(plan->graph->context, (oasp_ai_operator)step->op_id, first, second, error);
    if (step->op_id >= OASP_AI_OP_EXP && step->op_id <= OASP_AI_OP_SILU) {
        double argument = step->attributes_size == sizeof(double) ? *(const double *)step->attributes : 0.01;
        return oasp_ai_unary(plan->graph->context, (oasp_ai_operator)step->op_id, first, argument, error);
    }
    if (step->op_id == OASP_AI_OP_MATMUL || step->op_id == OASP_AI_OP_BATCHED_MATMUL)
        return oasp_ai_matmul(plan->graph->context, first, second, error);
    if (step->op_id == OASP_AI_OP_CROSS_ENTROPY)
        return oasp_ai_cross_entropy(plan->graph->context, first, second, error);
    if (step->op_id == OASP_AI_OP_SOFTMAX) {
        int32_t axis = step->attributes_size == sizeof(axis) ? *(const int32_t *)step->attributes : (int32_t)first->desc.rank - 1;
        return oasp_ai_softmax(plan->graph->context, first, axis, error);
    }
    if (step->op_id == OASP_AI_OP_LAYER_NORM || step->op_id == OASP_AI_OP_RMS_NORM) {
        oasp_ai_tensor *weight = step->input_count > 1 ? second : NULL;
        oasp_ai_tensor *bias = step->input_count > 2 ? slots[step->input_slots[2]] : NULL;
        double epsilon = step->attributes_size == sizeof(epsilon) ? *(const double *)step->attributes : 1e-5;
        return oasp_ai_layer_norm(plan->graph->context, first, weight, bias, epsilon, step->op_id == OASP_AI_OP_RMS_NORM, error);
    }
    if (step->op_id == OASP_AI_OP_ATTENTION) {
        int causal = step->attributes_size == sizeof(causal) ? *(const int *)step->attributes : 0;
        oasp_ai_tensor *value = step->input_count > 2 ? slots[step->input_slots[2]] : NULL;
        oasp_ai_tensor *mask = step->input_count > 3 ? slots[step->input_slots[3]] : NULL;
        return oasp_ai_attention(plan->graph->context, first, second, value, mask, causal, error);
    }
    if (step->op_id == OASP_AI_OP_GATHER) {
        uint32_t axis = step->attributes_size == sizeof(axis) ? *(const uint32_t *)step->attributes : 0;
        return oasp_ai_gather(plan->graph->context, first, second, axis, error);
    }
    if (step->op_id == OASP_AI_OP_CONTIGUOUS)
        return oasp_ai_tensor_contiguous(first, error);
    oasp_ai_set_error(error, 5, "ExecutePlan", "operator has no reference CPU execution rule", "Install a capable backend or decompose the operator.");
    return NULL;
}

static int execute_backend_graph(oasp_ai_plan *plan, oasp_ai_tensor *const *inputs, size_t input_count, oasp_ai_tensor ***outputs, size_t *output_count,
                                 oasp_ai_error *error) {
    oasp_ai_registered_backend *backend = &plan->graph->context->backends[plan->backend_index];
    const oasp_ai_backend_api *api = backend->descriptor->api;
    size_t count = plan->graph->output_count;
    oasp_ai_backend_storage *input_storage = calloc(input_count, sizeof(*input_storage));
    oasp_ai_backend_storage *output_storage = calloc(count, sizeof(*output_storage));
    oasp_ai_tensor **contiguous_inputs = calloc(input_count, sizeof(*contiguous_inputs));
    unsigned char *owned_input_storage = calloc(input_count, 1);
    oasp_ai_tensor **result = calloc(count, sizeof(*result));
    int failed = !input_storage || !output_storage || !contiguous_inputs || !owned_input_storage || !result;
    uint64_t copied_to_device = 0;
    uint64_t copied_to_host = 0;
    for (size_t index = 0; index < input_count && !failed; index++) {
        int32_t input_backend = oasp_ai_tensor_backend_index(inputs[index]);
        if (input_backend == plan->backend_index && (inputs[index]->desc.flags & OASP_AI_TENSOR_CONTIGUOUS)) {
            input_storage[index] = inputs[index]->storage->backend_storage;
            continue;
        }
        contiguous_inputs[index] = oasp_ai_tensor_contiguous(inputs[index], error);
        uint64_t bytes = contiguous_inputs[index] ? oasp_ai_tensor_bytes(contiguous_inputs[index]) : 0;
        oasp_ai_alloc_desc allocation = {
            .struct_size = sizeof(allocation),
            .bytes = bytes,
            .alignment = 64,
            .memory_kind = OASP_AI_MEMORY_DEVICE,
        };
        failed = !contiguous_inputs[index] || api->alloc(backend->device, &allocation, &input_storage[index], error);
        owned_input_storage[index] = !failed;
        unsigned char *staging = NULL;
        const void *source_host = contiguous_inputs[index] ? oasp_ai_tensor_const_data(contiguous_inputs[index]) : NULL;
        if (!failed && !source_host) {
            staging = malloc((size_t)bytes);
            failed = !staging || oasp_ai_tensor_copy_to_host(contiguous_inputs[index], staging, (size_t)bytes, error);
            source_host = staging;
        }
        oasp_ai_copy_desc copy = {
            .struct_size = sizeof(copy),
            .destination = input_storage[index],
            .bytes = bytes,
            .source_host = source_host,
            .source_kind = OASP_AI_MEMORY_HOST,
            .destination_kind = OASP_AI_MEMORY_DEVICE,
        };
        oasp_ai_event copied = 0;
        if (!failed)
            failed = api->copy(backend->transfer_stream, &copy, &copied, error) ||
                     api->wait_event(backend->compute_stream, copied, error);
        free(staging);
        copied_to_device += bytes;
    }
    for (size_t index = 0; index < count && !failed; index++) {
        oasp_ai_ir_value *value = &plan->graph->values[plan->graph->outputs[index] - 1];
        result[index] = oasp_ai_tensor_create_on_backend(plan->graph->context, &value->desc, plan->backend_index, error);
        uint64_t bytes = result[index] ? oasp_ai_tensor_bytes(result[index]) : 0;
        (void)bytes;
        failed = !result[index];
        if (!failed)
            output_storage[index] = result[index]->storage->backend_storage;
    }
    oasp_ai_event completed = 0;
    oasp_ai_graph_bindings bindings = {
        .struct_size = sizeof(bindings),
        .inputs = input_storage,
        .input_count = input_count,
        .outputs = output_storage,
        .output_count = count,
    };
    uint64_t queued = oasp_ai_now_nanoseconds();
    if (!failed)
        failed = api->execute_graph(backend->compute_stream, plan->executable, &bindings, &completed, error) ||
                 api->wait_event(backend->transfer_stream, completed, error);
    pthread_mutex_lock(&plan->graph->context->mutex);
    plan->graph->context->metrics.queue_nanoseconds += oasp_ai_now_nanoseconds() - queued;
    plan->graph->context->metrics.host_to_device_bytes += copied_to_device;
    plan->graph->context->metrics.device_to_host_bytes += copied_to_host;
    plan->graph->context->metrics.synchronization_count += !failed;
    pthread_mutex_unlock(&plan->graph->context->mutex);
    for (size_t index = 0; index < input_count; index++) {
        if (input_storage && owned_input_storage && owned_input_storage[index])
            api->free(backend->device, input_storage[index]);
        oasp_ai_tensor_release(contiguous_inputs ? contiguous_inputs[index] : NULL);
    }
    for (size_t index = 0; index < count; index++) {
        if (failed)
            oasp_ai_tensor_release(result ? result[index] : NULL);
    }
    free(input_storage);
    free(output_storage);
    free(contiguous_inputs);
    free(owned_input_storage);
    if (failed) {
        free(result);
        return -1;
    }
    *outputs = result;
    *output_count = count;
    return 0;
}

int oasp_ai_plan_execute(oasp_ai_plan *plan, oasp_ai_tensor *const *inputs, size_t input_count, oasp_ai_tensor ***outputs, size_t *output_count,
                         oasp_ai_error *error) {
    if (!plan || !inputs || input_count != plan->graph->input_count || !outputs || !output_count) {
        oasp_ai_set_error(error, 5, "ExecutePlan", "input count does not match the compiled graph", "Pass one Tensor for every graph input.");
        return -1;
    }
    for (size_t index = 0; index < input_count; index++) {
        const oasp_ai_tensor_desc *expected = &plan->graph->values[plan->graph->inputs[index] - 1].desc;
        const oasp_ai_tensor_desc *actual = inputs[index] ? &inputs[index]->desc : NULL;
        int compatible = actual && actual->rank == expected->rank && actual->dtype == expected->dtype;
        for (uint32_t axis = 0; compatible && axis < expected->rank; axis++)
            compatible = expected->shape[axis] == -1 || expected->shape[axis] == actual->shape[axis];
        if (!compatible) {
            oasp_ai_set_error(error, 5, "ExecutePlan", "input shape or dtype does not match the graph signature",
                              "Pass a Tensor matching all fixed dimensions and the declared dtype.");
            return -1;
        }
    }
    if (plan->backend_index >= 0)
        return execute_backend_graph(plan, inputs, input_count, outputs, output_count, error);
    oasp_ai_tensor **slots = calloc(plan->slot_count, sizeof(*slots));
    if (!slots)
        return -1;
    int status = 0;
    for (size_t index = 0; index < plan->graph->value_count; index++)
        if (plan->graph->values[index].constant) {
            uint32_t slot = plan->value_slots[index];
            slots[slot] = plan->graph->values[index].constant;
            oasp_ai_tensor_retain(slots[slot]);
        }
    for (size_t index = 0; index < input_count; index++) {
        uint32_t slot = plan->value_slots[plan->graph->inputs[index] - 1];
        oasp_ai_tensor_release(slots[slot]);
        slots[slot] = inputs[index];
        oasp_ai_tensor_retain(slots[slot]);
    }
    for (size_t index = 0; index < plan->step_count; index++) {
        oasp_ai_exec_step *step = &plan->steps[index];
        oasp_ai_tensor *output = execute_step(plan, step, slots, error);
        if (!output) {
            status = -1;
            break;
        }
        oasp_ai_tensor_release(slots[step->output_slot]);
        slots[step->output_slot] = output;
    }
    oasp_ai_tensor **result = NULL;
    if (!status)
        result = calloc(plan->graph->output_count, sizeof(*result));
    if (!status && !result)
        status = -1;
    if (!status)
        for (size_t index = 0; index < plan->graph->output_count; index++) {
            result[index] = slots[plan->value_slots[plan->graph->outputs[index] - 1]];
            oasp_ai_tensor_retain(result[index]);
        }
    for (uint32_t index = 0; index < plan->slot_count; index++)
        oasp_ai_tensor_release(slots[index]);
    free(slots);
    if (status) {
        free(result);
        return -1;
    }
    *outputs = result;
    *output_count = plan->graph->output_count;
    return 0;
}
