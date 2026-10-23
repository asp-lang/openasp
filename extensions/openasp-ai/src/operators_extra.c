// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

oasp_ai_tensor *oasp_ai_concat(oasp_ai_context *context, oasp_ai_tensor *const *inputs, size_t input_count, uint32_t axis, oasp_ai_error *error) {
    if (!context || !inputs || !input_count || !inputs[0] || axis >= inputs[0]->desc.rank) {
        oasp_ai_set_error(error, 5, "Concat", "invalid Concat inputs or axis", "Pass tensors with matching ranks and a valid axis.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = inputs[0]->desc;
    desc.shape[axis] = 0;
    for (size_t input = 0; input < input_count; input++) {
        if (!inputs[input] || inputs[input]->desc.rank != desc.rank || inputs[input]->desc.dtype != desc.dtype)
            goto incompatible;
        for (uint32_t current = 0; current < desc.rank; current++)
            if (current != axis && inputs[input]->desc.shape[current] != desc.shape[current])
                goto incompatible;
        if (inputs[input]->desc.shape[axis] > INT64_MAX - desc.shape[axis])
            goto incompatible;
        desc.shape[axis] += inputs[input]->desc.shape[axis];
    }
    oasp_ai_tensor *result = oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    uint64_t inner = 1;
    for (uint32_t current = axis + 1; current < desc.rank; current++)
        inner *= (uint64_t)desc.shape[current];
    uint64_t outer = result->element_count / ((uint64_t)desc.shape[axis] * inner);
    for (uint64_t group = 0; group < outer; group++) {
        uint64_t destination = group * (uint64_t)desc.shape[axis] * inner;
        for (size_t input = 0; input < input_count; input++) {
            uint64_t block = (uint64_t)inputs[input]->desc.shape[axis] * inner;
            uint64_t source = group * block;
            for (uint64_t item = 0; item < block; item++)
                oasp_ai_tensor_write(result, destination + item, oasp_ai_tensor_read(inputs[input], source + item));
            destination += block;
        }
    }
    return result;

incompatible:
    oasp_ai_set_error(error, 5, "Concat", "Concat tensor shapes or dtypes do not match", "All non-concatenated dimensions and dtypes must match.");
    return NULL;
}

oasp_ai_tensor *oasp_ai_where(oasp_ai_context *context, oasp_ai_tensor *condition, oasp_ai_tensor *when_true, oasp_ai_tensor *when_false, oasp_ai_error *error) {
    if (!condition || !when_true || !when_false || condition->element_count != when_true->element_count ||
        when_true->element_count != when_false->element_count) {
        oasp_ai_set_error(error, 5, "Where", "Where inputs must have equal element counts", "Broadcast inputs explicitly before Where.");
        return NULL;
    }
    oasp_ai_tensor *result = oasp_ai_tensor_like(when_true, 1, error);
    if (!result)
        return NULL;
    for (uint64_t index = 0; index < result->element_count; index++)
        oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(condition, index) != 0 ? oasp_ai_tensor_read(when_true, index)
                                                                                       : oasp_ai_tensor_read(when_false, index));
    context->metrics.operator_calls++;
    return result;
}

oasp_ai_tensor *oasp_ai_gather(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *indices, uint32_t axis, oasp_ai_error *error) {
    if (!context || !input || !indices || axis >= input->desc.rank || axis != 0) {
        oasp_ai_set_error(error, 5, "Gather", "reference Gather currently requires axis zero", "Transpose the selected axis to zero before Gather.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = input->desc;
    uint64_t row_size = input->element_count / (uint64_t)input->desc.shape[0];
    if (indices->desc.rank + input->desc.rank - 1 > OASP_AI_MAX_RANK) {
        oasp_ai_set_error(error, 5, "Gather", "Gather output rank exceeds eight", "Reduce index or input rank.");
        return NULL;
    }
    desc.rank = indices->desc.rank + input->desc.rank - 1;
    for (uint32_t current = 0; current < indices->desc.rank; current++)
        desc.shape[current] = indices->desc.shape[current];
    for (uint32_t current = 1; current < input->desc.rank; current++)
        desc.shape[indices->desc.rank + current - 1] = input->desc.shape[current];
    oasp_ai_tensor *result = oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    for (uint64_t item = 0; item < indices->element_count; item++) {
        int64_t selected = (int64_t)oasp_ai_tensor_read(indices, item);
        if (selected < 0 || selected >= input->desc.shape[0]) {
            oasp_ai_tensor_release(result);
            oasp_ai_set_error(error, 5, "Gather", "index is outside the selected axis", "Clamp or validate token/index values.");
            return NULL;
        }
        for (uint64_t offset = 0; offset < row_size; offset++)
            oasp_ai_tensor_write(result, item * row_size + offset, oasp_ai_tensor_read(input, (uint64_t)selected * row_size + offset));
    }
    return result;
}

oasp_ai_tensor *oasp_ai_conv2d(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *weight, oasp_ai_tensor *bias, uint32_t stride_height,
                               uint32_t stride_width, uint32_t padding_height, uint32_t padding_width, oasp_ai_error *error) {
    if (!input || !weight || input->desc.rank != 4 || weight->desc.rank != 4 || !stride_height || !stride_width ||
        input->desc.shape[1] != weight->desc.shape[1]) {
        oasp_ai_set_error(error, 5, "Conv2D", "expected NCHW input and OIHW weights", "Check ranks, channels, and positive strides.");
        return NULL;
    }
    int64_t output_height = (input->desc.shape[2] + 2 * padding_height - weight->desc.shape[2]) / stride_height + 1;
    int64_t output_width = (input->desc.shape[3] + 2 * padding_width - weight->desc.shape[3]) / stride_width + 1;
    if (output_height <= 0 || output_width <= 0)
        return NULL;
    oasp_ai_tensor_desc desc = {.dtype = input->desc.dtype,
                                .rank = 4,
                                .shape = {input->desc.shape[0], weight->desc.shape[0], output_height, output_width}};
    oasp_ai_tensor *result = oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    uint64_t batches = (uint64_t)desc.shape[0];
    uint64_t input_channels = (uint64_t)input->desc.shape[1];
    uint64_t input_height = (uint64_t)input->desc.shape[2];
    uint64_t input_width = (uint64_t)input->desc.shape[3];
    uint64_t output_channels = (uint64_t)desc.shape[1];
    uint64_t kernel_height = (uint64_t)weight->desc.shape[2];
    uint64_t kernel_width = (uint64_t)weight->desc.shape[3];
    for (uint64_t batch = 0; batch < batches; batch++)
        for (uint64_t output_channel = 0; output_channel < output_channels; output_channel++)
            for (int64_t y = 0; y < output_height; y++)
                for (int64_t x = 0; x < output_width; x++) {
                    double sum = bias ? oasp_ai_tensor_read(bias, output_channel) : 0;
                    for (uint64_t input_channel = 0; input_channel < input_channels; input_channel++)
                        for (uint64_t ky = 0; ky < kernel_height; ky++)
                            for (uint64_t kx = 0; kx < kernel_width; kx++) {
                                int64_t iy = y * stride_height + (int64_t)ky - padding_height;
                                int64_t ix = x * stride_width + (int64_t)kx - padding_width;
                                if (iy < 0 || ix < 0 || iy >= (int64_t)input_height || ix >= (int64_t)input_width)
                                    continue;
                                uint64_t input_index = ((batch * input_channels + input_channel) * input_height + (uint64_t)iy) * input_width + (uint64_t)ix;
                                uint64_t weight_index =
                                    ((output_channel * input_channels + input_channel) * kernel_height + ky) * kernel_width + kx;
                                sum += oasp_ai_tensor_read(input, input_index) * oasp_ai_tensor_read(weight, weight_index);
                            }
                    uint64_t output_index =
                        ((batch * output_channels + output_channel) * (uint64_t)output_height + (uint64_t)y) * (uint64_t)output_width + (uint64_t)x;
                    oasp_ai_tensor_write(result, output_index, sum);
                }
    return result;
}

oasp_ai_tensor *oasp_ai_pool2d(oasp_ai_context *context, oasp_ai_tensor *input, uint32_t kernel_height, uint32_t kernel_width, uint32_t stride_height,
                               uint32_t stride_width, int average, oasp_ai_error *error) {
    if (!input || input->desc.rank != 4 || !kernel_height || !kernel_width || !stride_height || !stride_width)
        return NULL;
    int64_t output_height = (input->desc.shape[2] - kernel_height) / stride_height + 1;
    int64_t output_width = (input->desc.shape[3] - kernel_width) / stride_width + 1;
    oasp_ai_tensor_desc desc = input->desc;
    desc.shape[2] = output_height;
    desc.shape[3] = output_width;
    oasp_ai_tensor *result = oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    uint64_t planes = (uint64_t)input->desc.shape[0] * (uint64_t)input->desc.shape[1];
    uint64_t input_height = (uint64_t)input->desc.shape[2];
    uint64_t input_width = (uint64_t)input->desc.shape[3];
    for (uint64_t plane = 0; plane < planes; plane++)
        for (int64_t y = 0; y < output_height; y++)
            for (int64_t x = 0; x < output_width; x++) {
                double aggregate = average ? 0 : -DBL_MAX;
                for (uint32_t ky = 0; ky < kernel_height; ky++)
                    for (uint32_t kx = 0; kx < kernel_width; kx++) {
                        uint64_t index =
                            (plane * input_height + (uint64_t)y * stride_height + ky) * input_width + (uint64_t)x * stride_width + kx;
                        double value = oasp_ai_tensor_read(input, index);
                        aggregate = average ? aggregate + value : (value > aggregate ? value : aggregate);
                    }
                if (average)
                    aggregate /= kernel_height * kernel_width;
                oasp_ai_tensor_write(result, (plane * (uint64_t)output_height + (uint64_t)y) * (uint64_t)output_width + (uint64_t)x, aggregate);
            }
    return result;
}

oasp_ai_tensor *oasp_ai_rope(oasp_ai_context *context, oasp_ai_tensor *input, uint64_t position_offset, double base, oasp_ai_error *error) {
    if (!input || input->desc.rank < 2 || input->desc.shape[input->desc.rank - 1] % 2 || base <= 1) {
        oasp_ai_set_error(error, 5, "RoPE", "RoPE requires an even final dimension and base greater than one", "Check head dimension and RoPE base.");
        return NULL;
    }
    oasp_ai_tensor *result = oasp_ai_tensor_like(input, 1, error);
    if (!result)
        return NULL;
    uint64_t width = (uint64_t)input->desc.shape[input->desc.rank - 1];
    uint64_t sequence = (uint64_t)input->desc.shape[input->desc.rank - 2];
    uint64_t groups = input->element_count / (sequence * width);
    for (uint64_t group = 0; group < groups; group++)
        for (uint64_t position = 0; position < sequence; position++)
            for (uint64_t pair = 0; pair < width / 2; pair++) {
                double angle = (double)(position + position_offset) / pow(base, (double)(2 * pair) / (double)width);
                uint64_t first = (group * sequence + position) * width + 2 * pair;
                double x = oasp_ai_tensor_read(input, first);
                double y = oasp_ai_tensor_read(input, first + 1);
                oasp_ai_tensor_write(result, first, x * cos(angle) - y * sin(angle));
                oasp_ai_tensor_write(result, first + 1, x * sin(angle) + y * cos(angle));
            }
    context->metrics.operator_calls++;
    return result;
}
