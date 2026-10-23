// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdlib.h>

int oasp_ai_attach_grad(oasp_ai_tensor *output, oasp_ai_operator operation, oasp_ai_tensor *const *inputs, size_t input_count, double argument,
                        int32_t axis, oasp_ai_error *error) {
    int required = 0;
    for (size_t index = 0; index < input_count; index++)
        required |= !!(inputs[index]->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD);
    if (!required)
        return 0;
    oasp_ai_autograd_node *node = calloc(1, sizeof(*node));
    if (node) {
        node->inputs = calloc(input_count, sizeof(*node->inputs));
        node->versions = calloc(input_count, sizeof(*node->versions));
    }
    if (!node || !node->inputs || !node->versions) {
        free(node ? node->inputs : NULL);
        free(node ? node->versions : NULL);
        free(node);
        oasp_ai_set_error(error, 7, "Autograd", "unable to allocate autograd node", "Reduce graph size or memory pressure.");
        return -1;
    }
    atomic_init(&node->references, 1);
    node->operation = operation;
    node->input_count = input_count;
    node->argument = argument;
    node->axis = axis;
    for (size_t index = 0; index < input_count; index++) {
        node->inputs[index] = inputs[index];
        node->versions[index] = inputs[index]->storage->version;
        oasp_ai_tensor_retain(inputs[index]);
    }
    output->grad_fn = node;
    output->desc.flags |= OASP_AI_TENSOR_REQUIRES_GRAD;
    return 0;
}

static oasp_ai_tensor *sum_to_shape(oasp_ai_tensor *gradient, const oasp_ai_tensor *target, oasp_ai_error *error) {
    if (gradient->desc.rank == target->desc.rank) {
        int equal = 1;
        for (uint32_t axis = 0; axis < target->desc.rank; axis++)
            equal &= gradient->desc.shape[axis] == target->desc.shape[axis];
        if (equal) {
            oasp_ai_tensor *result = oasp_ai_tensor_like(gradient, 0, error);
            if (result)
                result->desc.flags &= ~OASP_AI_TENSOR_REQUIRES_GRAD;
            return result;
        }
    }
    oasp_ai_tensor *result = oasp_ai_tensor_like((oasp_ai_tensor *)target, 1, error);
    if (!result)
        return NULL;
    result->desc.flags &= ~OASP_AI_TENSOR_REQUIRES_GRAD;
    int32_t backend_index = oasp_ai_tensor_backend_index(gradient);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {gradient};
        oasp_ai_tensor *outputs[] = {result};
        if (backend_index != oasp_ai_tensor_backend_index(target) ||
            oasp_ai_backend_execute(gradient->context, backend_index, OASP_AI_BACKEND_OP_SUM_TO_SHAPE,
                                    inputs, 1, outputs, 1, NULL, 0, error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
        return result;
    }
    for (uint64_t index = 0; index < gradient->element_count; index++) {
        uint64_t remaining = index;
        uint64_t target_index = 0;
        uint64_t multiplier = 1;
        for (uint32_t reverse = 0; reverse < target->desc.rank; reverse++) {
            uint32_t target_axis = target->desc.rank - reverse - 1;
            uint32_t gradient_axis = gradient->desc.rank - reverse - 1;
            uint64_t coordinate = remaining % (uint64_t)gradient->desc.shape[gradient_axis];
            remaining /= (uint64_t)gradient->desc.shape[gradient_axis];
            if (target->desc.shape[target_axis] != 1)
                target_index += coordinate * multiplier;
            multiplier *= (uint64_t)target->desc.shape[target_axis];
        }
        oasp_ai_tensor_write(result, target_index, oasp_ai_tensor_read(result, target_index) + oasp_ai_tensor_read(gradient, index));
    }
    return result;
}

int oasp_ai_accumulate_gradient(oasp_ai_tensor *tensor, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    if (!tensor || !gradient)
        return -1;
    oasp_ai_tensor *shaped = sum_to_shape(gradient, tensor, error);
    if (!shaped)
        return -1;
    if (!tensor->grad)
        tensor->grad = shaped;
    else {
        int32_t backend_index = oasp_ai_tensor_backend_index(tensor->grad);
        if (backend_index >= 0) {
            oasp_ai_tensor *sum = oasp_ai_tensor_like(tensor->grad, 1, error);
            oasp_ai_tensor *inputs[] = {tensor->grad, shaped};
            oasp_ai_tensor *outputs[] = {sum};
            if (!sum || oasp_ai_backend_execute(tensor->context, backend_index, OASP_AI_OP_ADD,
                                                inputs, 2, outputs, 1, NULL, 0, error)) {
                oasp_ai_tensor_release(sum);
                oasp_ai_tensor_release(shaped);
                return -1;
            }
            oasp_ai_tensor_release(tensor->grad);
            tensor->grad = sum;
        } else
            for (uint64_t index = 0; index < tensor->element_count; index++)
                oasp_ai_tensor_write(tensor->grad, index, oasp_ai_tensor_read(tensor->grad, index) + oasp_ai_tensor_read(shaped, index));
        oasp_ai_tensor_release(shaped);
    }
    return 0;
}

typedef struct {
    oasp_ai_tensor **items;
    size_t count;
    size_t capacity;
} tensor_list;

static int list_contains(const tensor_list *list, const oasp_ai_tensor *tensor) {
    for (size_t index = 0; index < list->count; index++)
        if (list->items[index] == tensor)
            return 1;
    return 0;
}

static int build_postorder(oasp_ai_tensor *tensor, tensor_list *visited, tensor_list *order) {
    if (list_contains(visited, tensor))
        return 0;
    oasp_ai_tensor **grown = oasp_ai_grow_array(visited->items, sizeof(*visited->items), &visited->capacity, visited->count + 1);
    if (!grown)
        return -1;
    visited->items = grown;
    visited->items[visited->count++] = tensor;
    if (tensor->grad_fn)
        for (size_t index = 0; index < tensor->grad_fn->input_count; index++)
            if (build_postorder(tensor->grad_fn->inputs[index], visited, order))
                return -1;
    grown = oasp_ai_grow_array(order->items, sizeof(*order->items), &order->capacity, order->count + 1);
    if (!grown)
        return -1;
    order->items = grown;
    order->items[order->count++] = tensor;
    return 0;
}

static oasp_ai_tensor *gradient_unary(oasp_ai_tensor *output, oasp_ai_tensor *input, oasp_ai_tensor *gradient, oasp_ai_operator operation, double argument,
                                      oasp_ai_error *error) {
    oasp_ai_tensor *result = oasp_ai_tensor_like(input, 1, error);
    if (!result)
        return NULL;
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {input, output, gradient};
        oasp_ai_tensor *outputs[] = {result};
        oasp_ai_backward_attributes attributes = {.forward_op = operation, .argument = argument};
        if (oasp_ai_backend_execute(input->context, backend_index, OASP_AI_BACKEND_OP_UNARY_BACKWARD,
                                    inputs, 3, outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
        return result;
    }
    for (uint64_t index = 0; index < input->element_count; index++) {
        double x = oasp_ai_tensor_read(input, index);
        double y = oasp_ai_tensor_read(output, index);
        double derivative;
        switch (operation) {
        case OASP_AI_OP_EXP:
            derivative = y;
            break;
        case OASP_AI_OP_LOG:
            derivative = 1 / x;
            break;
        case OASP_AI_OP_SQRT:
            derivative = 0.5 / y;
            break;
        case OASP_AI_OP_RELU:
            derivative = x > 0;
            break;
        case OASP_AI_OP_LEAKY_RELU:
            derivative = x > 0 ? 1 : argument;
            break;
        case OASP_AI_OP_SIGMOID:
            derivative = y * (1 - y);
            break;
        case OASP_AI_OP_TANH:
            derivative = 1 - y * y;
            break;
        case OASP_AI_OP_SILU: {
            double sigmoid = 1 / (1 + exp(-x));
            derivative = sigmoid + x * sigmoid * (1 - sigmoid);
            break;
        }
        case OASP_AI_OP_GELU: {
            double cube = x * x * x;
            double inner = 0.7978845608028654 * (x + 0.044715 * cube);
            double tanh_inner = tanh(inner);
            derivative = 0.5 * (1 + tanh_inner) +
                         0.5 * x * (1 - tanh_inner * tanh_inner) * 0.7978845608028654 * (1 + 0.134145 * x * x);
            break;
        }
        default:
            derivative = 1;
            break;
        }
        oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(gradient, index) * derivative);
    }
    return result;
}

static uint64_t broadcast_input_index(const oasp_ai_tensor *input, const oasp_ai_tensor *output, uint64_t output_index) {
    uint64_t index = 0;
    uint64_t multiplier = 1;
    for (uint32_t reverse = 0; reverse < input->desc.rank; reverse++) {
        uint32_t input_axis = input->desc.rank - reverse - 1;
        uint32_t output_axis = output->desc.rank - reverse - 1;
        uint64_t coordinate = output_index % (uint64_t)output->desc.shape[output_axis];
        output_index /= (uint64_t)output->desc.shape[output_axis];
        if (input->desc.shape[input_axis] != 1)
            index += coordinate * multiplier;
        multiplier *= (uint64_t)input->desc.shape[input_axis];
    }
    return index;
}

static int backward_binary(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_autograd_node *node = output->grad_fn;
    oasp_ai_tensor *left = node->inputs[0];
    oasp_ai_tensor *right = node->inputs[1];
    oasp_ai_tensor *left_gradient = oasp_ai_tensor_like(output, 1, error);
    oasp_ai_tensor *right_gradient = oasp_ai_tensor_like(output, 1, error);
    if (!left_gradient || !right_gradient) {
        oasp_ai_tensor_release(left_gradient);
        oasp_ai_tensor_release(right_gradient);
        return -1;
    }
    int32_t backend_index = oasp_ai_tensor_backend_index(output);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {left, right, gradient};
        oasp_ai_tensor *outputs[] = {left_gradient, right_gradient};
        oasp_ai_backward_attributes attributes = {.forward_op = node->operation};
        int status = oasp_ai_backend_execute(output->context, backend_index, OASP_AI_BACKEND_OP_BINARY_BACKWARD,
                                             inputs, 3, outputs, 2, &attributes, sizeof(attributes), error);
        if (!status && (left->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(left, left_gradient, error);
        if (!status && (right->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(right, right_gradient, error);
        oasp_ai_tensor_release(left_gradient);
        oasp_ai_tensor_release(right_gradient);
        return status;
    }
    for (uint64_t index = 0; index < output->element_count; index++) {
        double upstream = oasp_ai_tensor_read(gradient, index);
        double left_value = oasp_ai_tensor_read(left, broadcast_input_index(left, output, index));
        double right_value = oasp_ai_tensor_read(right, broadcast_input_index(right, output, index));
        double left_factor = 1;
        double right_factor = 1;
        if (node->operation == OASP_AI_OP_SUB)
            right_factor = -1;
        else if (node->operation == OASP_AI_OP_MUL) {
            left_factor = right_value;
            right_factor = left_value;
        } else if (node->operation == OASP_AI_OP_DIV) {
            left_factor = 1 / right_value;
            right_factor = -left_value / (right_value * right_value);
        } else if (node->operation == OASP_AI_OP_POW) {
            left_factor = right_value * pow(left_value, right_value - 1);
            right_factor = pow(left_value, right_value) * log(left_value);
        } else if (node->operation == OASP_AI_OP_MAXIMUM) {
            left_factor = left_value > right_value ? 1 : left_value == right_value ? 0.5 : 0;
            right_factor = right_value > left_value ? 1 : left_value == right_value ? 0.5 : 0;
        }
        oasp_ai_tensor_write(left_gradient, index, upstream * left_factor);
        oasp_ai_tensor_write(right_gradient, index, upstream * right_factor);
    }
    int status = 0;
    if (left->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD)
        status = oasp_ai_accumulate_gradient(left, left_gradient, error);
    if (!status && (right->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(right, right_gradient, error);
    oasp_ai_tensor_release(left_gradient);
    oasp_ai_tensor_release(right_gradient);
    return status;
}

static int backward_matmul(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_tensor *left = output->grad_fn->inputs[0];
    oasp_ai_tensor *right = output->grad_fn->inputs[1];
    oasp_ai_tensor *right_t = oasp_ai_tensor_transpose(right, right->desc.rank - 2, right->desc.rank - 1, error);
    oasp_ai_tensor *left_t = oasp_ai_tensor_transpose(left, left->desc.rank - 2, left->desc.rank - 1, error);
    oasp_ai_tensor *left_gradient = right_t ? oasp_ai_matmul(output->context, gradient, right_t, error) : NULL;
    oasp_ai_tensor *right_gradient = left_t ? oasp_ai_matmul(output->context, left_t, gradient, error) : NULL;
    int status = !left_gradient || !right_gradient;
    if (!status && (left->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(left, left_gradient, error);
    if (!status && (right->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(right, right_gradient, error);
    oasp_ai_tensor_release(right_t);
    oasp_ai_tensor_release(left_t);
    oasp_ai_tensor_release(left_gradient);
    oasp_ai_tensor_release(right_gradient);
    return status;
}

static int backward_cross_entropy(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_tensor *logits = output->grad_fn->inputs[0];
    oasp_ai_tensor *labels = output->grad_fn->inputs[1];
    oasp_ai_tensor *normalizer = output->grad_fn->inputs[2];
    oasp_ai_tensor *weight = output->grad_fn->parameters[1] ? output->grad_fn->inputs[3] : NULL;
    oasp_ai_reduction reduction = (oasp_ai_reduction)output->grad_fn->axis;
    int64_t ignore_index = output->grad_fn->parameters[0];
    double label_smoothing = output->grad_fn->argument;
    int32_t backend_index = oasp_ai_tensor_backend_index(logits);
    if (backend_index >= 0) {
        oasp_ai_tensor *logits_gradient = oasp_ai_tensor_like(logits, 1, error);
        oasp_ai_tensor *inputs[] = {logits, labels, gradient, normalizer, weight};
        size_t input_count = weight ? 5 : 4;
        oasp_ai_tensor *outputs[] = {logits_gradient};
        oasp_ai_cross_entropy_attributes attributes = {
            .reduction = reduction,
            .ignore_index = ignore_index,
            .label_smoothing = label_smoothing,
            .has_weight = weight != NULL,
        };
        int status = !logits_gradient ||
                     oasp_ai_backend_execute(output->context, backend_index, OASP_AI_BACKEND_OP_CROSS_ENTROPY_BACKWARD,
                                             inputs, input_count, outputs, 1, &attributes,
                                             sizeof(attributes), error);
        if (!status)
            status = oasp_ai_accumulate_gradient(logits, logits_gradient, error);
        oasp_ai_tensor_release(logits_gradient);
        return status;
    }
    oasp_ai_tensor *probabilities = oasp_ai_softmax(output->context, logits, 1, error);
    if (!probabilities)
        return -1;
    uint64_t batch = (uint64_t)logits->desc.shape[0];
    uint64_t classes = (uint64_t)logits->desc.shape[1];
    double denominator = oasp_ai_tensor_read(normalizer, 0);
    double weight_sum = 0;
    for (uint64_t column = 0; column < classes; column++)
        weight_sum += weight ? oasp_ai_tensor_read(weight, column) : 1;
    for (uint64_t row = 0; row < batch; row++) {
        int64_t label = (int64_t)oasp_ai_tensor_read(labels, row);
        if (label == ignore_index) {
            for (uint64_t column = 0; column < classes; column++)
                oasp_ai_tensor_write(probabilities, row * classes + column, 0);
            continue;
        }
        double target_weight = weight ? oasp_ai_tensor_read(weight, (uint64_t)label) : 1;
        double upstream = oasp_ai_tensor_read(
            gradient, reduction == OASP_AI_REDUCTION_NONE ? row : 0);
        if (reduction == OASP_AI_REDUCTION_MEAN)
            upstream /= denominator;
        for (uint64_t column = 0; column < classes; column++) {
            uint64_t index = row * classes + column;
            double probability = oasp_ai_tensor_read(probabilities, index);
            double class_weight = weight ? oasp_ai_tensor_read(weight, column) : 1;
            double value =
                (1 - label_smoothing) * target_weight *
                    (probability - (column == (uint64_t)label)) +
                label_smoothing / (double)classes *
                    (probability * weight_sum - class_weight);
            oasp_ai_tensor_write(probabilities, index, value * upstream);
        }
    }
    int status = oasp_ai_accumulate_gradient(logits, probabilities, error);
    oasp_ai_tensor_release(probabilities);
    return status;
}

static int backward_nll(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_tensor *input = output->grad_fn->inputs[0];
    oasp_ai_tensor *target = output->grad_fn->inputs[1];
    oasp_ai_reduction reduction = (oasp_ai_reduction)output->grad_fn->argument;
    oasp_ai_tensor *input_gradient = oasp_ai_tensor_like(input, 1, error);
    if (!input_gradient)
        return -1;
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    int status = 0;
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {target, gradient};
        oasp_ai_tensor *outputs[] = {input_gradient};
        oasp_ai_loss_attributes attributes = {.reduction = reduction};
        status = oasp_ai_backend_execute(output->context, backend_index,
                                         OASP_AI_BACKEND_OP_NLL_BACKWARD, inputs, 2,
                                         outputs, 1, &attributes, sizeof(attributes), error);
    } else {
        uint64_t batch = (uint64_t)input->desc.shape[0];
        uint64_t classes = (uint64_t)input->desc.shape[1];
        for (uint64_t row = 0; row < batch; row++) {
            uint64_t label = (uint64_t)oasp_ai_tensor_read(target, row);
            double upstream = oasp_ai_tensor_read(
                gradient, reduction == OASP_AI_REDUCTION_NONE ? row : 0);
            if (reduction == OASP_AI_REDUCTION_MEAN)
                upstream /= (double)batch;
            oasp_ai_tensor_write(input_gradient, row * classes + label, -upstream);
        }
    }
    if (!status && (input->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(input, input_gradient, error);
    oasp_ai_tensor_release(input_gradient);
    return status;
}

static int backward_reduce(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_tensor *input = output->grad_fn->inputs[0];
    oasp_ai_tensor *expanded = oasp_ai_tensor_like(input, 1, error);
    if (!expanded)
        return -1;
    int32_t axis = output->grad_fn->axis;
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {gradient};
        oasp_ai_tensor *outputs[] = {expanded};
        oasp_ai_reduce_backward_attributes attributes = {
            .forward_op = output->grad_fn->operation,
            .axis = axis,
            .keep_dimensions = (int32_t)output->grad_fn->argument,
        };
        int status = oasp_ai_backend_execute(output->context, backend_index, OASP_AI_BACKEND_OP_REDUCE_BACKWARD,
                                             inputs, 1, outputs, 1, &attributes, sizeof(attributes), error);
        if (!status)
            status = oasp_ai_accumulate_gradient(input, expanded, error);
        oasp_ai_tensor_release(expanded);
        return status;
    }
    uint64_t reduction = axis < 0 ? input->element_count : (uint64_t)input->desc.shape[axis];
    uint64_t inner = 1;
    if (axis >= 0)
        for (uint32_t current = (uint32_t)axis + 1; current < input->desc.rank; current++)
            inner *= (uint64_t)input->desc.shape[current];
    for (uint64_t index = 0; index < input->element_count; index++) {
        uint64_t gradient_index = axis < 0 ? 0 : (index / (reduction * inner)) * inner + index % inner;
        double value = oasp_ai_tensor_read(gradient, gradient_index);
        if (output->grad_fn->operation == OASP_AI_OP_MEAN)
            value /= (double)reduction;
        oasp_ai_tensor_write(expanded, index, value);
    }
    int status = oasp_ai_accumulate_gradient(input, expanded, error);
    oasp_ai_tensor_release(expanded);
    return status;
}

static int backward_softmax(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_tensor *input = output->grad_fn->inputs[0];
    int32_t axis = output->grad_fn->axis;
    oasp_ai_tensor *input_gradient = oasp_ai_tensor_like(input, 1, error);
    if (!input_gradient)
        return -1;
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {output, gradient};
        oasp_ai_tensor *outputs[] = {input_gradient};
        oasp_ai_axis_attributes attributes = {.axis = axis};
        int status = oasp_ai_backend_execute(output->context, backend_index, OASP_AI_BACKEND_OP_SOFTMAX_BACKWARD,
                                             inputs, 2, outputs, 1, &attributes, sizeof(attributes), error);
        if (!status)
            status = oasp_ai_accumulate_gradient(input, input_gradient, error);
        oasp_ai_tensor_release(input_gradient);
        return status;
    }
    uint64_t inner = 1;
    for (uint32_t current = (uint32_t)axis + 1; current < input->desc.rank; current++)
        inner *= (uint64_t)input->desc.shape[current];
    uint64_t width = (uint64_t)input->desc.shape[axis];
    uint64_t outer = input->element_count / (width * inner);
    for (uint64_t group = 0; group < outer; group++)
        for (uint64_t position = 0; position < inner; position++) {
            double dot = 0;
            for (uint64_t item = 0; item < width; item++) {
                uint64_t index = group * width * inner + item * inner + position;
                dot += oasp_ai_tensor_read(gradient, index) * oasp_ai_tensor_read(output, index);
            }
            for (uint64_t item = 0; item < width; item++) {
                uint64_t index = group * width * inner + item * inner + position;
                double value = oasp_ai_tensor_read(output, index) * (oasp_ai_tensor_read(gradient, index) - dot);
                oasp_ai_tensor_write(input_gradient, index, value);
            }
        }
    int status = oasp_ai_accumulate_gradient(input, input_gradient, error);
    oasp_ai_tensor_release(input_gradient);
    return status;
}

static int backward_view(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_autograd_node *node = output->grad_fn;
    oasp_ai_tensor *input = node->inputs[0];
    oasp_ai_tensor *input_gradient = NULL;
    if (node->operation == OASP_AI_OP_RESHAPE)
        input_gradient = oasp_ai_tensor_reshape(gradient, input->desc.shape, input->desc.rank, error);
    else if (node->operation == OASP_AI_OP_TRANSPOSE)
        input_gradient = oasp_ai_tensor_transpose(gradient, (uint32_t)node->axis, (uint32_t)node->argument, error);
    else if (node->operation == OASP_AI_OP_CAST)
        input_gradient = oasp_ai_tensor_cast(gradient, input->desc.dtype, error);
    else if (node->operation == OASP_AI_OP_CONTIGUOUS)
        input_gradient = oasp_ai_tensor_clone(gradient, error);
    else if (node->operation == OASP_AI_OP_SLICE) {
        input_gradient = oasp_ai_tensor_like(input, 1, error);
        if (input_gradient) {
            uint32_t sliced_axis = (uint32_t)node->axis;
            uint64_t coordinates[OASP_AI_MAX_RANK];
            for (uint64_t index = 0; index < output->element_count; index++) {
                uint64_t remaining = index;
                for (uint32_t axis = output->desc.rank; axis; axis--) {
                    coordinates[axis - 1] = remaining % (uint64_t)output->desc.shape[axis - 1];
                    remaining /= (uint64_t)output->desc.shape[axis - 1];
                }
                coordinates[sliced_axis] =
                    (uint64_t)node->parameters[0] + coordinates[sliced_axis] * (uint64_t)node->parameters[2];
                uint64_t target = 0;
                for (uint32_t axis = 0; axis < input->desc.rank; axis++)
                    target = target * (uint64_t)input->desc.shape[axis] + coordinates[axis];
                oasp_ai_tensor_write(input_gradient, target, oasp_ai_tensor_read(gradient, index));
            }
        }
    }
    if (!input_gradient)
        return -1;
    int status = oasp_ai_accumulate_gradient(input, input_gradient, error);
    oasp_ai_tensor_release(input_gradient);
    return status;
}

static int backward_normalization(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_autograd_node *node = output->grad_fn;
    oasp_ai_tensor *input = node->inputs[0];
    oasp_ai_tensor *weight = node->input_count > 1 ? node->inputs[1] : NULL;
    oasp_ai_tensor *bias = node->input_count > 2 ? node->inputs[2] : NULL;
    int rms_only = node->operation == OASP_AI_OP_RMS_NORM;
    uint64_t width = (uint64_t)input->desc.shape[input->desc.rank - 1];
    uint64_t rows = input->element_count / width;
    oasp_ai_tensor *input_gradient = oasp_ai_tensor_like(input, 1, error);
    oasp_ai_tensor *weight_gradient = weight ? oasp_ai_tensor_like(weight, 1, error) : NULL;
    oasp_ai_tensor *bias_gradient = bias ? oasp_ai_tensor_like(bias, 1, error) : NULL;
    if (!input_gradient || (weight && !weight_gradient) || (bias && !bias_gradient)) {
        oasp_ai_tensor_release(input_gradient);
        oasp_ai_tensor_release(weight_gradient);
        oasp_ai_tensor_release(bias_gradient);
        return -1;
    }
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[4] = {input, NULL, NULL, gradient};
        size_t input_count = 1;
        if (weight)
            inputs[input_count++] = weight;
        if (bias)
            inputs[input_count++] = bias;
        inputs[input_count++] = gradient;
        oasp_ai_tensor *outputs[3] = {input_gradient, NULL, NULL};
        size_t output_count = 1;
        if (weight_gradient)
            outputs[output_count++] = weight_gradient;
        if (bias_gradient)
            outputs[output_count++] = bias_gradient;
        oasp_ai_norm_attributes attributes = {
            .epsilon = node->argument,
            .rms_only = rms_only,
        };
        int status = oasp_ai_backend_execute(output->context, backend_index, OASP_AI_BACKEND_OP_NORM_BACKWARD,
                                             inputs, input_count, outputs, output_count,
                                             &attributes, sizeof(attributes), error);
        if (!status && (input->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(input, input_gradient, error);
        if (!status && weight && (weight->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(weight, weight_gradient, error);
        if (!status && bias && (bias->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(bias, bias_gradient, error);
        oasp_ai_tensor_release(input_gradient);
        oasp_ai_tensor_release(weight_gradient);
        oasp_ai_tensor_release(bias_gradient);
        return status;
    }
    for (uint64_t row = 0; row < rows; row++) {
        double mean = 0;
        if (!rms_only) {
            for (uint64_t column = 0; column < width; column++)
                mean += oasp_ai_tensor_read(input, row * width + column);
            mean /= (double)width;
        }
        double variance = 0;
        for (uint64_t column = 0; column < width; column++) {
            double centered = oasp_ai_tensor_read(input, row * width + column) - (rms_only ? 0 : mean);
            variance += centered * centered;
        }
        double inverse = 1 / sqrt(variance / (double)width + node->argument);
        double sum_gradient = 0;
        double sum_gradient_normalized = 0;
        for (uint64_t column = 0; column < width; column++) {
            uint64_t index = row * width + column;
            double normalized = (oasp_ai_tensor_read(input, index) - (rms_only ? 0 : mean)) * inverse;
            double upstream = oasp_ai_tensor_read(gradient, index);
            double scaled = upstream * (weight ? oasp_ai_tensor_read(weight, column) : 1);
            sum_gradient += scaled;
            sum_gradient_normalized += scaled * normalized;
            if (weight_gradient)
                oasp_ai_tensor_write(weight_gradient, column, oasp_ai_tensor_read(weight_gradient, column) + upstream * normalized);
            if (bias_gradient)
                oasp_ai_tensor_write(bias_gradient, column, oasp_ai_tensor_read(bias_gradient, column) + upstream);
        }
        for (uint64_t column = 0; column < width; column++) {
            uint64_t index = row * width + column;
            double normalized = (oasp_ai_tensor_read(input, index) - (rms_only ? 0 : mean)) * inverse;
            double scaled = oasp_ai_tensor_read(gradient, index) * (weight ? oasp_ai_tensor_read(weight, column) : 1);
            double value = rms_only ? inverse * (scaled - normalized * sum_gradient_normalized / (double)width)
                                    : inverse * (scaled - sum_gradient / (double)width -
                                                 normalized * sum_gradient_normalized / (double)width);
            oasp_ai_tensor_write(input_gradient, index, value);
        }
    }
    int status = (input->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD) ? oasp_ai_accumulate_gradient(input, input_gradient, error) : 0;
    if (!status && weight && (weight->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(weight, weight_gradient, error);
    if (!status && bias && (bias->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(bias, bias_gradient, error);
    oasp_ai_tensor_release(input_gradient);
    oasp_ai_tensor_release(weight_gradient);
    oasp_ai_tensor_release(bias_gradient);
    return status;
}

static int backward_attention(oasp_ai_tensor *output, oasp_ai_tensor *gradient, oasp_ai_error *error) {
    oasp_ai_autograd_node *node = output->grad_fn;
    oasp_ai_tensor *query = node->inputs[0];
    oasp_ai_tensor *key = node->inputs[1];
    oasp_ai_tensor *value = node->inputs[2];
    oasp_ai_tensor *mask = node->input_count > 3 ? node->inputs[3] : NULL;
    uint32_t rank = query->desc.rank;
    uint64_t queries = (uint64_t)query->desc.shape[rank - 2];
    uint64_t keys = (uint64_t)key->desc.shape[rank - 2];
    uint64_t width = (uint64_t)query->desc.shape[rank - 1];
    uint64_t value_width = (uint64_t)value->desc.shape[rank - 1];
    uint64_t batches = query->element_count / (queries * width);
    int32_t backend_index = oasp_ai_tensor_backend_index(query);
    if (backend_index >= 0) {
        oasp_ai_tensor *query_gradient = oasp_ai_tensor_like(query, 1, error);
        oasp_ai_tensor *key_gradient = oasp_ai_tensor_like(key, 1, error);
        oasp_ai_tensor *value_gradient = oasp_ai_tensor_like(value, 1, error);
        if (!query_gradient || !key_gradient || !value_gradient) {
            oasp_ai_tensor_release(query_gradient);
            oasp_ai_tensor_release(key_gradient);
            oasp_ai_tensor_release(value_gradient);
            return -1;
        }
        oasp_ai_tensor *inputs[5] = {query, key, value, mask, gradient};
        size_t input_count = mask ? 5 : 4;
        if (!mask)
            inputs[3] = gradient;
        oasp_ai_tensor *outputs[] = {query_gradient, key_gradient, value_gradient};
        oasp_ai_attention_attributes attributes = {.causal = (int32_t)node->argument};
        int status = oasp_ai_backend_execute(output->context, backend_index, OASP_AI_BACKEND_OP_ATTENTION_BACKWARD,
                                             inputs, input_count, outputs, 3, &attributes, sizeof(attributes), error);
        if (!status && (query->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(query, query_gradient, error);
        if (!status && (key->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(key, key_gradient, error);
        if (!status && (value->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
            status = oasp_ai_accumulate_gradient(value, value_gradient, error);
        oasp_ai_tensor_release(query_gradient);
        oasp_ai_tensor_release(key_gradient);
        oasp_ai_tensor_release(value_gradient);
        return status;
    }
    oasp_ai_tensor score_shape = {.desc = query->desc, .element_count = batches * queries * keys};
    score_shape.desc.shape[rank - 1] = (int64_t)keys;
    double *probabilities = malloc((size_t)(queries * keys) * sizeof(*probabilities));
    double *probability_gradient = malloc((size_t)(queries * keys) * sizeof(*probability_gradient));
    oasp_ai_tensor *query_gradient = oasp_ai_tensor_like(query, 1, error);
    oasp_ai_tensor *key_gradient = oasp_ai_tensor_like(key, 1, error);
    oasp_ai_tensor *value_gradient = oasp_ai_tensor_like(value, 1, error);
    if (!probabilities || !probability_gradient || !query_gradient || !key_gradient || !value_gradient) {
        free(probabilities);
        free(probability_gradient);
        oasp_ai_tensor_release(query_gradient);
        oasp_ai_tensor_release(key_gradient);
        oasp_ai_tensor_release(value_gradient);
        return -1;
    }
    double scale = 1 / sqrt((double)width);
    for (uint64_t batch = 0; batch < batches; batch++) {
        for (uint64_t row = 0; row < queries; row++) {
            double maximum = -INFINITY;
            for (uint64_t column = 0; column < keys; column++) {
                double score = 0;
                for (uint64_t inner = 0; inner < width; inner++)
                    score += oasp_ai_tensor_read(query, batch * queries * width + row * width + inner) *
                             oasp_ai_tensor_read(key, batch * keys * width + column * width + inner);
                int masked = ((int)node->argument && column > row);
                if (mask)
                    masked |= !oasp_ai_tensor_read(mask, broadcast_input_index(mask, &score_shape, batch * queries * keys + row * keys + column));
                probabilities[row * keys + column] = masked ? -INFINITY : score * scale;
                if (probabilities[row * keys + column] > maximum)
                    maximum = probabilities[row * keys + column];
            }
            double total = 0;
            if (maximum == -INFINITY) {
                for (uint64_t column = 0; column < keys; column++)
                    probabilities[row * keys + column] = 0;
                continue;
            }
            for (uint64_t column = 0; column < keys; column++) {
                double probability = exp(probabilities[row * keys + column] - maximum);
                probabilities[row * keys + column] = probability;
                total += probability;
            }
            for (uint64_t column = 0; column < keys; column++)
                probabilities[row * keys + column] /= total;
        }
        for (uint64_t row = 0; row < queries; row++)
            for (uint64_t column = 0; column < keys; column++) {
                double sum = 0;
                for (uint64_t channel = 0; channel < value_width; channel++)
                    sum += oasp_ai_tensor_read(gradient, batch * queries * value_width + row * value_width + channel) *
                           oasp_ai_tensor_read(value, batch * keys * value_width + column * value_width + channel);
                probability_gradient[row * keys + column] = sum;
                for (uint64_t channel = 0; channel < value_width; channel++) {
                    uint64_t target = batch * keys * value_width + column * value_width + channel;
                    double addition = probabilities[row * keys + column] *
                                      oasp_ai_tensor_read(gradient, batch * queries * value_width + row * value_width + channel);
                    oasp_ai_tensor_write(value_gradient, target, oasp_ai_tensor_read(value_gradient, target) + addition);
                }
            }
        for (uint64_t row = 0; row < queries; row++) {
            double dot = 0;
            for (uint64_t column = 0; column < keys; column++)
                dot += probability_gradient[row * keys + column] * probabilities[row * keys + column];
            for (uint64_t column = 0; column < keys; column++) {
                double score_gradient = probabilities[row * keys + column] * (probability_gradient[row * keys + column] - dot) * scale;
                for (uint64_t channel = 0; channel < width; channel++) {
                    uint64_t query_index = batch * queries * width + row * width + channel;
                    uint64_t key_index = batch * keys * width + column * width + channel;
                    oasp_ai_tensor_write(query_gradient, query_index,
                                         oasp_ai_tensor_read(query_gradient, query_index) +
                                             score_gradient * oasp_ai_tensor_read(key, key_index));
                    oasp_ai_tensor_write(key_gradient, key_index,
                                         oasp_ai_tensor_read(key_gradient, key_index) +
                                             score_gradient * oasp_ai_tensor_read(query, query_index));
                }
            }
        }
    }
    int status = (query->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD) ? oasp_ai_accumulate_gradient(query, query_gradient, error) : 0;
    if (!status && (key->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(key, key_gradient, error);
    if (!status && (value->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(value, value_gradient, error);
    free(probabilities);
    free(probability_gradient);
    oasp_ai_tensor_release(query_gradient);
    oasp_ai_tensor_release(key_gradient);
    oasp_ai_tensor_release(value_gradient);
    return status;
}

static int backward_node(oasp_ai_tensor *output, oasp_ai_error *error) {
    oasp_ai_autograd_node *node = output->grad_fn;
    if (!node || !output->grad)
        return 0;
    for (size_t index = 0; index < node->input_count; index++) {
        if (node->versions[index] != node->inputs[index]->storage->version) {
            oasp_ai_set_error(error, 5, "Backward", "a tensor needed for backward was modified in place", "Clone the tensor or avoid mutation before Backward.");
            return -1;
        }
    }
    if (node->operation >= OASP_AI_OP_ADD && node->operation <= OASP_AI_OP_MAXIMUM)
        return backward_binary(output, output->grad, error);
    if (node->operation == OASP_AI_OP_MATMUL)
        return backward_matmul(output, output->grad, error);
    if (node->operation == OASP_AI_OP_CROSS_ENTROPY)
        return backward_cross_entropy(output, output->grad, error);
    if (node->operation == OASP_AI_OP_NLL)
        return backward_nll(output, output->grad, error);
    if (node->operation == OASP_AI_OP_CTC_LOSS)
        return oasp_ai_ctc_loss_backward(output, output->grad, error);
    if (node->operation == OASP_AI_OP_SUM || node->operation == OASP_AI_OP_MEAN)
        return backward_reduce(output, output->grad, error);
    if (node->operation == OASP_AI_OP_SOFTMAX)
        return backward_softmax(output, output->grad, error);
    if (node->operation == OASP_AI_OP_LAYER_NORM || node->operation == OASP_AI_OP_RMS_NORM)
        return backward_normalization(output, output->grad, error);
    if (node->operation == OASP_AI_OP_ATTENTION)
        return backward_attention(output, output->grad, error);
    if (node->operation == OASP_AI_OP_RESHAPE || node->operation == OASP_AI_OP_TRANSPOSE || node->operation == OASP_AI_OP_SLICE ||
        node->operation == OASP_AI_OP_CAST || node->operation == OASP_AI_OP_CONTIGUOUS)
        return backward_view(output, output->grad, error);
    if (node->input_count == 1) {
        oasp_ai_tensor *input = node->inputs[0];
        oasp_ai_tensor *gradient = gradient_unary(output, input, output->grad, node->operation, node->argument, error);
        int status = gradient ? oasp_ai_accumulate_gradient(input, gradient, error) : -1;
        oasp_ai_tensor_release(gradient);
        return status;
    }
    return 0;
}

int oasp_ai_tensor_backward(oasp_ai_tensor *tensor, oasp_ai_tensor *gradient, int retain_graph, oasp_ai_error *error) {
    if (!tensor || !(tensor->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD)) {
        oasp_ai_set_error(error, 5, "Backward", "tensor does not require gradients", "Set RequiresGrad before the forward pass.");
        return -1;
    }
    uint64_t started = oasp_ai_now_nanoseconds();
    oasp_ai_tensor *initial = gradient;
    if (!initial) {
        if (tensor->element_count != 1) {
            oasp_ai_set_error(error, 5, "Backward", "non-scalar output requires an initial gradient", "Pass a gradient with the same shape.");
            return -1;
        }
        initial = oasp_ai_tensor_like(tensor, 1, error);
        if (initial) {
            int32_t backend_index = oasp_ai_tensor_backend_index(initial);
            if (backend_index >= 0) {
                oasp_ai_tensor *outputs[] = {initial};
                oasp_ai_fill_attributes attributes = {.value = 1};
                if (oasp_ai_backend_execute(tensor->context, backend_index, OASP_AI_BACKEND_OP_FILL,
                                            NULL, 0, outputs, 1, &attributes, sizeof(attributes), error)) {
                    oasp_ai_tensor_release(initial);
                    initial = NULL;
                }
            } else
                oasp_ai_tensor_write(initial, 0, 1);
        }
    } else
        oasp_ai_tensor_retain(initial);
    if (!initial)
        return -1;
    oasp_ai_tensor_release(tensor->grad);
    tensor->grad = initial;
    tensor_list visited = {0};
    tensor_list order = {0};
    if (build_postorder(tensor, &visited, &order)) {
        free(visited.items);
        free(order.items);
        oasp_ai_set_error(error, 7, "Backward", "unable to build backward traversal", "Reduce graph size or memory pressure.");
        return -1;
    }
    int status = 0;
    for (size_t index = order.count; index && !status; index--)
        status = backward_node(order.items[index - 1], error);
    if (!retain_graph)
        for (size_t index = 0; index < order.count; index++) {
            oasp_ai_tensor *current = order.items[index];
            if (current != tensor && current->grad_fn) {
                oasp_ai_autograd_node *node = current->grad_fn;
                current->grad_fn = NULL;
                if (atomic_fetch_sub_explicit(&node->references, 1, memory_order_acq_rel) == 1) {
                    for (size_t input = 0; input < node->input_count; input++)
                        oasp_ai_tensor_release(node->inputs[input]);
                    free(node->inputs);
                    free(node->versions);
                    free(node);
                }
            }
        }
    free(visited.items);
    free(order.items);
    tensor->context->metrics.backward_nanoseconds += oasp_ai_now_nanoseconds() - started;
    return status;
}
