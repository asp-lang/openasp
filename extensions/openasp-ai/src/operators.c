// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static oasp_ai_dtype promoted_dtype(oasp_ai_dtype left, oasp_ai_dtype right) {
    if (left == OASP_AI_DTYPE_F64 || right == OASP_AI_DTYPE_F64)
        return OASP_AI_DTYPE_F64;
    if (left >= OASP_AI_DTYPE_F16 || right >= OASP_AI_DTYPE_F16)
        return OASP_AI_DTYPE_F32;
    if (left == OASP_AI_DTYPE_I64 || right == OASP_AI_DTYPE_I64)
        return OASP_AI_DTYPE_I64;
    return OASP_AI_DTYPE_I32;
}

static int broadcast_shape(const oasp_ai_tensor *left, const oasp_ai_tensor *right, oasp_ai_tensor_desc *desc, oasp_ai_error *error) {
    desc->rank = left->desc.rank > right->desc.rank ? left->desc.rank : right->desc.rank;
    desc->dtype = promoted_dtype(left->desc.dtype, right->desc.dtype);
    for (uint32_t axis = 0; axis < desc->rank; axis++) {
        int left_axis = (int)axis - (int)(desc->rank - left->desc.rank);
        int right_axis = (int)axis - (int)(desc->rank - right->desc.rank);
        int64_t left_size = left_axis >= 0 ? left->desc.shape[left_axis] : 1;
        int64_t right_size = right_axis >= 0 ? right->desc.shape[right_axis] : 1;
        if (left_size != right_size && left_size != 1 && right_size != 1) {
            oasp_ai_set_error(error, 5, "Broadcast", "tensor shapes are not broadcast-compatible", "Align dimensions from the right; each pair must match or be one.");
            return -1;
        }
        desc->shape[axis] = left_size > right_size ? left_size : right_size;
    }
    return 0;
}

static uint64_t broadcast_index(const oasp_ai_tensor *input, const oasp_ai_tensor_desc *output, uint64_t output_index) {
    uint64_t index = 0;
    uint64_t multiplier = 1;
    for (uint32_t reverse = 0; reverse < input->desc.rank; reverse++) {
        uint32_t input_axis = input->desc.rank - reverse - 1;
        uint32_t output_axis = output->rank - reverse - 1;
        uint64_t coordinate = output_index % (uint64_t)output->shape[output_axis];
        output_index /= (uint64_t)output->shape[output_axis];
        if (input->desc.shape[input_axis] != 1)
            index += coordinate * multiplier;
        multiplier *= (uint64_t)input->desc.shape[input_axis];
    }
    return index;
}

static double apply_binary(oasp_ai_operator operation, double left, double right) {
    switch (operation) {
    case OASP_AI_OP_ADD:
        return left + right;
    case OASP_AI_OP_SUB:
        return left - right;
    case OASP_AI_OP_MUL:
        return left * right;
    case OASP_AI_OP_DIV:
        return left / right;
    case OASP_AI_OP_POW:
        return pow(left, right);
    case OASP_AI_OP_MAXIMUM:
        return left > right ? left : right;
    default:
        return NAN;
    }
}

oasp_ai_tensor *oasp_ai_binary(oasp_ai_context *context, oasp_ai_operator operation, oasp_ai_tensor *left, oasp_ai_tensor *right, oasp_ai_error *error) {
    uint64_t started = oasp_ai_now_nanoseconds();
    oasp_ai_tensor_desc desc = {0};
    if (!context || !left || !right || broadcast_shape(left, right, &desc, error))
        return NULL;
    if (operation < OASP_AI_OP_ADD || operation > OASP_AI_OP_MAXIMUM) {
        oasp_ai_set_error(error, 5, "Binary", "unsupported binary operator", "Use Add, Sub, Mul, Div, Pow, or Maximum.");
        return NULL;
    }
    oasp_ai_tensor *operands[] = {left, right};
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(operands, 2, &backend_index, error))
        return NULL;
    oasp_ai_tensor *result = backend_index >= 0 ? oasp_ai_tensor_create_on_backend(context, &desc, backend_index, error)
                                                : oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    if (backend_index >= 0) {
        oasp_ai_tensor *outputs[] = {result};
        if (oasp_ai_backend_execute(context, backend_index, operation, operands, 2, outputs, 1, NULL, 0, error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else
        for (uint64_t index = 0; index < result->element_count; index++) {
            double left_value = oasp_ai_tensor_read(left, broadcast_index(left, &desc, index));
            double right_value = oasp_ai_tensor_read(right, broadcast_index(right, &desc, index));
            oasp_ai_tensor_write(result, index, apply_binary(operation, left_value, right_value));
        }
    oasp_ai_tensor *inputs[] = {left, right};
    if (oasp_ai_attach_grad(result, operation, inputs, 2, 0, -1, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    context->metrics.operator_calls++;
    context->metrics.operator_nanoseconds += oasp_ai_now_nanoseconds() - started;
    return result;
}

static double apply_unary(oasp_ai_operator operation, double value, double argument) {
    switch (operation) {
    case OASP_AI_OP_EXP:
        return exp(value);
    case OASP_AI_OP_LOG:
        return log(value);
    case OASP_AI_OP_SQRT:
        return sqrt(value);
    case OASP_AI_OP_RELU:
        return value > 0 ? value : 0;
    case OASP_AI_OP_LEAKY_RELU:
        return value > 0 ? value : argument * value;
    case OASP_AI_OP_SIGMOID:
        return 1 / (1 + exp(-value));
    case OASP_AI_OP_TANH:
        return tanh(value);
    case OASP_AI_OP_GELU:
        return 0.5 * value * (1 + tanh(0.7978845608028654 * (value + 0.044715 * value * value * value)));
    case OASP_AI_OP_SILU:
        return value / (1 + exp(-value));
    case OASP_AI_OP_TEMPERATURE:
        return value / argument;
    default:
        return NAN;
    }
}

oasp_ai_tensor *oasp_ai_unary(oasp_ai_context *context, oasp_ai_operator operation, oasp_ai_tensor *input, double argument, oasp_ai_error *error) {
    uint64_t started = oasp_ai_now_nanoseconds();
    if (!context || !input) {
        oasp_ai_set_error(error, 5, "Unary", "input tensor is required", "Pass a valid Tensor.");
        return NULL;
    }
    oasp_ai_tensor *result = oasp_ai_tensor_like(input, 1, error);
    if (!result)
        return NULL;
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {input};
        oasp_ai_tensor *outputs[] = {result};
        oasp_ai_unary_attributes attributes = {.argument = argument};
        if (oasp_ai_backend_execute(context, backend_index, operation, inputs, 1, outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else
        for (uint64_t index = 0; index < input->element_count; index++)
            oasp_ai_tensor_write(result, index, apply_unary(operation, oasp_ai_tensor_read(input, index), argument));
    oasp_ai_tensor *inputs[] = {input};
    if (oasp_ai_attach_grad(result, operation, inputs, 1, argument, -1, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    context->metrics.operator_calls++;
    context->metrics.operator_nanoseconds += oasp_ai_now_nanoseconds() - started;
    return result;
}

oasp_ai_tensor *oasp_ai_reduce(oasp_ai_context *context, oasp_ai_operator operation, oasp_ai_tensor *input, int32_t axis, int keep_dimensions,
                               oasp_ai_error *error) {
    if (!context || !input || axis < -1 || axis >= (int32_t)input->desc.rank) {
        oasp_ai_set_error(error, 5, "Reduce", "reduction axis is out of range", "Use -1 for all elements or an axis smaller than tensor rank.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = {.dtype = input->desc.dtype};
    if (axis == -1) {
        desc.rank = 1;
        desc.shape[0] = 1;
    } else {
        desc.rank = keep_dimensions ? input->desc.rank : input->desc.rank - 1;
        if (!desc.rank) {
            desc.rank = 1;
            desc.shape[0] = 1;
        } else {
            uint32_t output_axis = 0;
            for (uint32_t current = 0; current < input->desc.rank; current++) {
                if ((int32_t)current == axis) {
                    if (keep_dimensions)
                        desc.shape[output_axis++] = 1;
                } else
                    desc.shape[output_axis++] = input->desc.shape[current];
            }
        }
    }
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    oasp_ai_tensor *result = backend_index >= 0 ? oasp_ai_tensor_create_on_backend(context, &desc, backend_index, error)
                                                : oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    uint64_t reduction = axis == -1 ? input->element_count : (uint64_t)input->desc.shape[axis];
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {input};
        oasp_ai_tensor *outputs[] = {result};
        oasp_ai_reduce_attributes attributes = {.axis = axis, .keep_dimensions = keep_dimensions};
        if (oasp_ai_backend_execute(context, backend_index, operation, inputs, 1, outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else for (uint64_t output = 0; output < result->element_count; output++) {
        double aggregate = operation == OASP_AI_OP_MIN ? DBL_MAX : operation == OASP_AI_OP_MAX ? -DBL_MAX : 0;
        uint64_t best = 0;
        for (uint64_t item = 0; item < reduction; item++) {
            uint64_t input_index;
            if (axis == -1)
                input_index = item;
            else {
                uint64_t inner = 1;
                for (uint32_t current = (uint32_t)axis + 1; current < input->desc.rank; current++)
                    inner *= (uint64_t)input->desc.shape[current];
                input_index = (output / inner) * reduction * inner + item * inner + output % inner;
            }
            double value = oasp_ai_tensor_read(input, input_index);
            if (operation == OASP_AI_OP_MAX || operation == OASP_AI_OP_MIN) {
                if ((operation == OASP_AI_OP_MAX && value > aggregate) || (operation == OASP_AI_OP_MIN && value < aggregate)) {
                    aggregate = value;
                    best = item;
                }
            } else if (operation == OASP_AI_OP_NORM)
                aggregate += value * value;
            else
                aggregate += value;
        }
        if (operation == OASP_AI_OP_MEAN)
            aggregate /= (double)reduction;
        else if (operation == OASP_AI_OP_NORM)
            aggregate = sqrt(aggregate);
        else if (operation == OASP_AI_OP_ARGMAX)
            aggregate = (double)best;
        oasp_ai_tensor_write(result, output, aggregate);
    }
    oasp_ai_tensor *inputs[] = {input};
    if (operation == OASP_AI_OP_SUM || operation == OASP_AI_OP_MEAN)
        oasp_ai_attach_grad(result, operation, inputs, 1, keep_dimensions, axis, error);
    return result;
}

oasp_ai_tensor *oasp_ai_matmul(oasp_ai_context *context, oasp_ai_tensor *left, oasp_ai_tensor *right, oasp_ai_error *error) {
    uint64_t started = oasp_ai_now_nanoseconds();
    if (!context || !left || !right || left->desc.rank < 2 || right->desc.rank < 2) {
        oasp_ai_set_error(error, 5, "MatMul", "MatMul requires tensors with rank at least two", "Reshape vectors into matrices before MatMul.");
        return NULL;
    }
    int64_t rows = left->desc.shape[left->desc.rank - 2];
    int64_t shared = left->desc.shape[left->desc.rank - 1];
    int64_t right_shared = right->desc.shape[right->desc.rank - 2];
    int64_t columns = right->desc.shape[right->desc.rank - 1];
    if (shared != right_shared || left->desc.rank != right->desc.rank) {
        oasp_ai_set_error(error, 5, "MatMul", "incompatible matrix shapes", "The inner dimensions and batch ranks must match.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = {.dtype = promoted_dtype(left->desc.dtype, right->desc.dtype), .rank = left->desc.rank};
    for (uint32_t axis = 0; axis + 2 < desc.rank; axis++) {
        int64_t left_batch = left->desc.shape[axis];
        int64_t right_batch = right->desc.shape[axis];
        if (left_batch != right_batch && left_batch != 1 && right_batch != 1) {
            oasp_ai_set_error(error, 5, "MatMul", "incompatible batch dimensions", "Batch dimensions must match or be one.");
            return NULL;
        }
        desc.shape[axis] = left_batch > right_batch ? left_batch : right_batch;
    }
    desc.shape[desc.rank - 2] = rows;
    desc.shape[desc.rank - 1] = columns;
    oasp_ai_tensor *operands[] = {left, right};
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(operands, 2, &backend_index, error))
        return NULL;
    oasp_ai_tensor *result = backend_index >= 0 ? oasp_ai_tensor_create_on_backend(context, &desc, backend_index, error)
                                                : oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    uint64_t batches = result->element_count / (uint64_t)(rows * columns);
    uint64_t left_matrix = (uint64_t)(rows * shared);
    uint64_t right_matrix = (uint64_t)(shared * columns);
    if (backend_index >= 0) {
        oasp_ai_tensor *outputs[] = {result};
        if (oasp_ai_backend_execute(context, backend_index, OASP_AI_OP_MATMUL, operands, 2, outputs, 1, NULL, 0, error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else for (uint64_t batch = 0; batch < batches; batch++) {
        uint64_t left_batch = left->element_count == left_matrix ? 0 : batch;
        uint64_t right_batch = right->element_count == right_matrix ? 0 : batch;
        for (int64_t row = 0; row < rows; row++)
            for (int64_t column = 0; column < columns; column++) {
                double sum = 0;
                for (int64_t inner = 0; inner < shared; inner++)
                    sum += oasp_ai_tensor_read(left, left_batch * left_matrix + (uint64_t)(row * shared + inner)) *
                           oasp_ai_tensor_read(right, right_batch * right_matrix + (uint64_t)(inner * columns + column));
                oasp_ai_tensor_write(result, batch * (uint64_t)(rows * columns) + (uint64_t)(row * columns + column), sum);
            }
    }
    oasp_ai_tensor *inputs[] = {left, right};
    oasp_ai_attach_grad(result, OASP_AI_OP_MATMUL, inputs, 2, 0, -1, error);
    context->metrics.operator_calls++;
    context->metrics.operator_nanoseconds += oasp_ai_now_nanoseconds() - started;
    return result;
}

oasp_ai_tensor *oasp_ai_linear(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *weight, oasp_ai_tensor *bias, oasp_ai_error *error) {
    if (!weight || weight->desc.rank != 2) {
        oasp_ai_set_error(error, 5, "Linear", "weight must be a rank-two tensor", "Use weight shape [input_features, output_features].");
        return NULL;
    }
    oasp_ai_tensor *result = oasp_ai_matmul(context, input, weight, error);
    if (!result || !bias)
        return result;
    oasp_ai_tensor *with_bias = oasp_ai_binary(context, OASP_AI_OP_ADD, result, bias, error);
    oasp_ai_tensor_release(result);
    return with_bias;
}

oasp_ai_tensor *oasp_ai_softmax(oasp_ai_context *context, oasp_ai_tensor *input, int32_t axis, oasp_ai_error *error) {
    if (!input || axis < 0 || axis >= (int32_t)input->desc.rank) {
        oasp_ai_set_error(error, 5, "Softmax", "softmax axis is out of range", "Use an axis smaller than tensor rank.");
        return NULL;
    }
    oasp_ai_tensor *result = oasp_ai_tensor_like(input, 1, error);
    if (!result)
        return NULL;
    int32_t backend_index = oasp_ai_tensor_backend_index(input);
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {input};
        oasp_ai_tensor *outputs[] = {result};
        oasp_ai_axis_attributes attributes = {.axis = axis};
        if (oasp_ai_backend_execute(context, backend_index, OASP_AI_OP_SOFTMAX, inputs, 1, outputs, 1,
                                    &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    }
    uint64_t inner = 1;
    for (uint32_t current = (uint32_t)axis + 1; current < input->desc.rank; current++)
        inner *= (uint64_t)input->desc.shape[current];
    uint64_t width = (uint64_t)input->desc.shape[axis];
    uint64_t outer = input->element_count / (width * inner);
    if (backend_index < 0) for (uint64_t group = 0; group < outer; group++)
        for (uint64_t position = 0; position < inner; position++) {
            double maximum = -DBL_MAX;
            for (uint64_t item = 0; item < width; item++) {
                double value = oasp_ai_tensor_read(input, group * width * inner + item * inner + position);
                if (value > maximum)
                    maximum = value;
            }
            if (maximum == -INFINITY) {
                for (uint64_t item = 0; item < width; item++)
                    oasp_ai_tensor_write(result, group * width * inner + item * inner + position, 0);
                continue;
            }
            double total = 0;
            for (uint64_t item = 0; item < width; item++) {
                double value = exp(oasp_ai_tensor_read(input, group * width * inner + item * inner + position) - maximum);
                total += value;
                oasp_ai_tensor_write(result, group * width * inner + item * inner + position, value);
            }
            for (uint64_t item = 0; item < width; item++) {
                uint64_t index = group * width * inner + item * inner + position;
                oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(result, index) / total);
            }
        }
    oasp_ai_tensor *inputs[] = {input};
    if (oasp_ai_attach_grad(result, OASP_AI_OP_SOFTMAX, inputs, 1, 0, axis, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    context->metrics.operator_calls++;
    return result;
}

oasp_ai_tensor *oasp_ai_layer_norm(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *weight, oasp_ai_tensor *bias, double epsilon,
                                   int rms_only, oasp_ai_error *error) {
    if (!input || !input->desc.rank) {
        oasp_ai_set_error(error, 5, "LayerNorm", "input tensor is required", "Pass a floating-point Tensor.");
        return NULL;
    }
    uint64_t width = (uint64_t)input->desc.shape[input->desc.rank - 1];
    if ((weight && weight->element_count != width) || (bias && bias->element_count != width)) {
        oasp_ai_set_error(error, 5, "LayerNorm", "weight or bias shape does not match the last dimension", "Use one value per normalized feature.");
        return NULL;
    }
    oasp_ai_tensor *result = oasp_ai_tensor_like(input, 1, error);
    if (!result)
        return NULL;
    oasp_ai_tensor *operands[3] = {input, weight, bias};
    size_t input_count = bias ? 3 : weight ? 2 : 1;
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(operands, input_count, &backend_index, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    if (backend_index >= 0) {
        oasp_ai_tensor *outputs[] = {result};
        oasp_ai_norm_attributes attributes = {.epsilon = epsilon, .rms_only = rms_only};
        if (oasp_ai_backend_execute(context, backend_index, rms_only ? OASP_AI_OP_RMS_NORM : OASP_AI_OP_LAYER_NORM,
                                    operands, input_count, outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else for (uint64_t row = 0; row < input->element_count / width; row++) {
        double mean = 0;
        for (uint64_t column = 0; column < width; column++)
            mean += oasp_ai_tensor_read(input, row * width + column);
        mean /= (double)width;
        double variance = 0;
        for (uint64_t column = 0; column < width; column++) {
            double value = oasp_ai_tensor_read(input, row * width + column);
            double centered = rms_only ? value : value - mean;
            variance += centered * centered;
        }
        double inverse = 1 / sqrt(variance / (double)width + epsilon);
        for (uint64_t column = 0; column < width; column++) {
            double value = oasp_ai_tensor_read(input, row * width + column);
            value = (rms_only ? value : value - mean) * inverse;
            if (weight)
                value *= oasp_ai_tensor_read(weight, column);
            if (bias)
                value += oasp_ai_tensor_read(bias, column);
            oasp_ai_tensor_write(result, row * width + column, value);
        }
    }
    if (oasp_ai_attach_grad(result, rms_only ? OASP_AI_OP_RMS_NORM : OASP_AI_OP_LAYER_NORM, operands, input_count, epsilon, -1, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    context->metrics.operator_calls++;
    return result;
}

oasp_ai_tensor *oasp_ai_cross_entropy_ex(oasp_ai_context *context, oasp_ai_tensor *logits, oasp_ai_tensor *labels, oasp_ai_tensor *weight, int64_t ignore_index,
                                         double label_smoothing, oasp_ai_reduction reduction, oasp_ai_error *error) {
    if (!context || !logits || !labels || logits->desc.rank != 2 || labels->desc.rank != 1 ||
        labels->desc.dtype != OASP_AI_DTYPE_I64 || labels->element_count != (uint64_t)logits->desc.shape[0] ||
        reduction > OASP_AI_REDUCTION_SUM || label_smoothing < 0 || label_smoothing > 1) {
        oasp_ai_set_error(error, 5, "CrossEntropy",
                          "expected logits [batch, classes], int64 labels [batch], valid smoothing, and none/mean/sum reduction",
                          "Check tensor shapes, label dtype, label smoothing, and reduction.");
        return NULL;
    }
    uint64_t batch = (uint64_t)logits->desc.shape[0];
    uint64_t classes = (uint64_t)logits->desc.shape[1];
    if (weight && (weight->desc.rank != 1 || weight->element_count != classes ||
                   (weight->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))) {
        oasp_ai_set_error(error, 5, "CrossEntropy", "weight must contain one non-trainable value per class",
                          "Pass a float weight Tensor shaped [classes] without RequiresGrad.");
        return NULL;
    }
    oasp_ai_tensor *operands[] = {logits, labels, weight};
    size_t operand_count = weight ? 3 : 2;
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(operands, operand_count, &backend_index, error))
        return NULL;
    oasp_ai_tensor_desc result_desc = {
        .dtype = OASP_AI_DTYPE_F32,
        .rank = 1,
        .shape = {reduction == OASP_AI_REDUCTION_NONE ? (int64_t)batch : 1},
    };
    oasp_ai_tensor *result = backend_index >= 0
                                 ? oasp_ai_tensor_create_on_backend(context, &result_desc, backend_index, error)
                                 : oasp_ai_tensor_create(context, &result_desc, NULL, 0, error);
    oasp_ai_tensor_desc scalar_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {1}};
    oasp_ai_tensor *normalizer = backend_index >= 0
                                     ? oasp_ai_tensor_create_on_backend(context, &scalar_desc,
                                                                        backend_index, error)
                                     : oasp_ai_tensor_create(context, &scalar_desc, NULL, 0, error);
    if (!result || !normalizer) {
        oasp_ai_tensor_release(result);
        oasp_ai_tensor_release(normalizer);
        return NULL;
    }
    oasp_ai_cross_entropy_attributes attributes = {
        .reduction = reduction,
        .ignore_index = ignore_index,
        .label_smoothing = label_smoothing,
        .has_weight = weight != NULL,
    };
    if (backend_index >= 0) {
        oasp_ai_tensor *outputs[] = {result, normalizer};
        if (oasp_ai_backend_execute(context, backend_index, OASP_AI_OP_CROSS_ENTROPY,
                                    operands, operand_count, outputs, 2, &attributes,
                                    sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            oasp_ai_tensor_release(normalizer);
            return NULL;
        }
    } else {
        double aggregate = 0;
        double denominator = 0;
        for (uint64_t row = 0; row < batch; row++) {
            int64_t label = (int64_t)oasp_ai_tensor_read(labels, row);
            if (label == ignore_index) {
                if (reduction == OASP_AI_REDUCTION_NONE)
                    oasp_ai_tensor_write(result, row, 0);
                continue;
            }
            if (label < 0 || (uint64_t)label >= classes) {
                oasp_ai_tensor_release(result);
                oasp_ai_tensor_release(normalizer);
                oasp_ai_set_error(error, 5, "CrossEntropy", "label is outside the class range",
                                  "Ensure each non-ignored label is in [0, classes).");
                return NULL;
            }
            double maximum = -DBL_MAX;
            for (uint64_t column = 0; column < classes; column++)
                maximum = fmax(maximum, oasp_ai_tensor_read(logits, row * classes + column));
            double exponential_sum = 0;
            for (uint64_t column = 0; column < classes; column++)
                exponential_sum += exp(oasp_ai_tensor_read(logits, row * classes + column) - maximum);
            double log_sum_exp = log(exponential_sum) + maximum;
            double target_weight = weight ? oasp_ai_tensor_read(weight, (uint64_t)label) : 1;
            double negative_log_likelihood =
                (log_sum_exp - oasp_ai_tensor_read(logits, row * classes + (uint64_t)label)) * target_weight;
            double smooth_loss = 0;
            for (uint64_t column = 0; column < classes; column++) {
                double class_weight = weight ? oasp_ai_tensor_read(weight, column) : 1;
                smooth_loss += (log_sum_exp - oasp_ai_tensor_read(logits, row * classes + column)) * class_weight;
            }
            double row_loss = (1 - label_smoothing) * negative_log_likelihood +
                              label_smoothing * smooth_loss / (double)classes;
            if (reduction == OASP_AI_REDUCTION_NONE)
                oasp_ai_tensor_write(result, row, row_loss);
            else
                aggregate += row_loss;
            denominator += target_weight;
        }
        if (reduction != OASP_AI_REDUCTION_NONE)
            oasp_ai_tensor_write(result, 0, reduction == OASP_AI_REDUCTION_MEAN
                                                ? aggregate / denominator
                                                : aggregate);
        oasp_ai_tensor_write(normalizer, 0,
                             reduction == OASP_AI_REDUCTION_MEAN ? denominator : 1);
    }
    oasp_ai_tensor *saved_inputs[] = {logits, labels, normalizer, weight};
    size_t saved_input_count = weight ? 4 : 3;
    if (oasp_ai_attach_grad(result, OASP_AI_OP_CROSS_ENTROPY, saved_inputs, saved_input_count,
                            label_smoothing, (int32_t)reduction, error)) {
        oasp_ai_tensor_release(result);
        oasp_ai_tensor_release(normalizer);
        return NULL;
    }
    oasp_ai_tensor_release(normalizer);
    if (result->grad_fn) {
        result->grad_fn->parameters[0] = ignore_index;
        result->grad_fn->parameters[1] = weight != NULL;
    }
    return result;
}

oasp_ai_tensor *oasp_ai_cross_entropy(oasp_ai_context *context, oasp_ai_tensor *logits, oasp_ai_tensor *labels, oasp_ai_error *error) {
    return oasp_ai_cross_entropy_ex(context, logits, labels, NULL, -100, 0,
                                    OASP_AI_REDUCTION_MEAN, error);
}

oasp_ai_tensor *oasp_ai_nll_loss(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target, oasp_ai_reduction reduction, oasp_ai_error *error) {
    if (!context || !input || !target || input->desc.rank != 2 || target->desc.rank != 1 ||
        input->desc.shape[0] != target->desc.shape[0] ||
        target->desc.dtype != OASP_AI_DTYPE_I64 || reduction > OASP_AI_REDUCTION_SUM) {
        oasp_ai_set_error(error, 5, "NLLLoss",
                          "expected log-probabilities [batch, classes], int64 labels [batch], and none/mean/sum reduction",
                          "Apply LogSoftmax before NLLLoss and check label shape and reduction.");
        return NULL;
    }
    oasp_ai_tensor *operands[] = {input, target};
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(operands, 2, &backend_index, error))
        return NULL;
    oasp_ai_tensor_desc desc = {.dtype = input->desc.dtype, .rank = 1};
    desc.shape[0] = reduction == OASP_AI_REDUCTION_NONE ? input->desc.shape[0] : 1;
    oasp_ai_tensor *result = backend_index >= 0
                                 ? oasp_ai_tensor_create_on_backend(context, &desc, backend_index, error)
                                 : oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    oasp_ai_loss_attributes attributes = {.reduction = reduction};
    if (backend_index >= 0) {
        oasp_ai_tensor *outputs[] = {result};
        if (oasp_ai_backend_execute(context, backend_index, OASP_AI_OP_NLL, operands, 2,
                                    outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else {
        uint64_t batch = (uint64_t)input->desc.shape[0];
        uint64_t classes = (uint64_t)input->desc.shape[1];
        double total = 0;
        for (uint64_t row = 0; row < batch; row++) {
            int64_t label = (int64_t)oasp_ai_tensor_read(target, row);
            if (label < 0 || (uint64_t)label >= classes) {
                oasp_ai_tensor_release(result);
                oasp_ai_set_error(error, 5, "NLLLoss", "label is outside the class range",
                                  "Ensure every label is in [0, classes).");
                return NULL;
            }
            double value = -oasp_ai_tensor_read(input, row * classes + (uint64_t)label);
            if (reduction == OASP_AI_REDUCTION_NONE)
                oasp_ai_tensor_write(result, row, value);
            else
                total += value;
        }
        if (reduction != OASP_AI_REDUCTION_NONE)
            oasp_ai_tensor_write(result, 0, reduction == OASP_AI_REDUCTION_MEAN
                                                ? total / (double)batch
                                                : total);
    }
    if (oasp_ai_attach_grad(result, OASP_AI_OP_NLL, operands, 2, (double)reduction, -1, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    context->metrics.operator_calls++;
    return result;
}

oasp_ai_tensor *oasp_ai_attention(oasp_ai_context *context, oasp_ai_tensor *query, oasp_ai_tensor *key, oasp_ai_tensor *value, oasp_ai_tensor *mask,
                                  int causal, oasp_ai_error *error) {
    if (!query || !key || !value || query->desc.rank != key->desc.rank || key->desc.rank != value->desc.rank || query->desc.rank < 2) {
        oasp_ai_set_error(error, 5, "Attention", "Q, K, and V ranks must match and be at least two", "Use [..., sequence, channels] tensors.");
        return NULL;
    }
    uint32_t rank = query->desc.rank;
    uint64_t queries = (uint64_t)query->desc.shape[rank - 2];
    uint64_t keys = (uint64_t)key->desc.shape[rank - 2];
    uint64_t width = (uint64_t)query->desc.shape[rank - 1];
    uint64_t value_width = (uint64_t)value->desc.shape[rank - 1];
    if ((uint64_t)key->desc.shape[rank - 1] != width || (uint64_t)value->desc.shape[rank - 2] != keys) {
        oasp_ai_set_error(error, 5, "Attention", "Attention channel or sequence dimensions do not match", "Check Q/K channels and K/V sequence lengths.");
        return NULL;
    }
    oasp_ai_tensor_desc output_desc = query->desc;
    output_desc.shape[rank - 1] = (int64_t)value_width;
    oasp_ai_tensor_desc scores_desc = query->desc;
    scores_desc.shape[rank - 1] = (int64_t)keys;
    oasp_ai_tensor *operands[4] = {query, key, value, mask};
    size_t input_count = mask ? 4 : 3;
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(operands, input_count, &backend_index, error))
        return NULL;
    oasp_ai_tensor *output = backend_index >= 0 ? oasp_ai_tensor_create_on_backend(context, &output_desc, backend_index, error)
                                                : oasp_ai_tensor_create(context, &output_desc, NULL, 0, error);
    if (!output)
        return NULL;
    if (backend_index >= 0) {
        oasp_ai_tensor *outputs[] = {output};
        oasp_ai_attention_attributes attributes = {.causal = causal};
        if (oasp_ai_backend_execute(context, backend_index, OASP_AI_OP_ATTENTION, operands, input_count,
                                    outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(output);
            return NULL;
        }
        if (oasp_ai_attach_grad(output, OASP_AI_OP_ATTENTION, operands, input_count, causal, -1, error)) {
            oasp_ai_tensor_release(output);
            return NULL;
        }
        context->metrics.operator_calls++;
        return output;
    }
    int64_t score_shape[] = {(int64_t)queries, (int64_t)keys};
    oasp_ai_tensor *scores = oasp_ai_tensor_full(context, score_shape, 2, OASP_AI_DTYPE_F32, 0, error);
    if (!scores) {
        oasp_ai_tensor_release(output);
        oasp_ai_tensor_release(scores);
        return NULL;
    }
    uint64_t batches = query->element_count / (queries * width);
    for (uint64_t batch = 0; batch < batches; batch++) {
        for (uint64_t row = 0; row < queries; row++)
            for (uint64_t column = 0; column < keys; column++) {
                double score = 0;
                for (uint64_t inner = 0; inner < width; inner++)
                    score += oasp_ai_tensor_read(query, batch * queries * width + row * width + inner) *
                             oasp_ai_tensor_read(key, batch * keys * width + column * width + inner);
                score /= sqrt((double)width);
                uint64_t score_index = batch * queries * keys + row * keys + column;
                if ((causal && column > row) || (mask && !oasp_ai_tensor_read(mask, broadcast_index(mask, &scores_desc, score_index))))
                    score = -INFINITY;
                oasp_ai_tensor_write(scores, row * keys + column, score);
            }
        oasp_ai_tensor *probabilities = oasp_ai_softmax(context, scores, 1, error);
        if (!probabilities) {
            oasp_ai_tensor_release(output);
            oasp_ai_tensor_release(scores);
            return NULL;
        }
        for (uint64_t row = 0; row < queries; row++)
            for (uint64_t column = 0; column < value_width; column++) {
                double sum = 0;
                for (uint64_t item = 0; item < keys; item++)
                    sum += oasp_ai_tensor_read(probabilities, row * keys + item) *
                           oasp_ai_tensor_read(value, batch * keys * value_width + item * value_width + column);
                oasp_ai_tensor_write(output, batch * queries * value_width + row * value_width + column, sum);
            }
        oasp_ai_tensor_release(probabilities);
    }
    oasp_ai_tensor_release(scores);
    if (oasp_ai_attach_grad(output, OASP_AI_OP_ATTENTION, operands, input_count, causal, -1, error)) {
        oasp_ai_tensor_release(output);
        return NULL;
    }
    context->metrics.operator_calls++;
    return output;
}
