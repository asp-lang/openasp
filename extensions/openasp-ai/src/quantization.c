// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int oasp_ai_tensor_quantization(const oasp_ai_tensor *tensor, oasp_ai_quantization_info *info) {
    if (!tensor || !info)
        return -1;
    *info = (oasp_ai_quantization_info){
        .enabled = tensor->quantized_bits != 0,
        .bits = tensor->quantized_bits,
        .axis = tensor->quantized_axis,
        .scale = tensor->quantized_scale,
        .zero_point = tensor->quantized_zero_point,
        .channel_scales = tensor->quantized_channel_scales,
        .channel_count = tensor->quantized_channel_count,
        .calibration_method = tensor->quantized_calibration,
    };
    return 0;
}

static uint64_t channel_for_index(const oasp_ai_tensor *tensor, int32_t axis, uint64_t index) {
    uint64_t divisor = 1;
    for (uint32_t current = tensor->desc.rank; current > (uint32_t)axis + 1; current--)
        divisor *= (uint64_t)tensor->desc.shape[current - 1];
    return index / divisor % (uint64_t)tensor->desc.shape[axis];
}

oasp_ai_tensor *oasp_ai_tensor_quantize(oasp_ai_tensor *tensor, uint32_t bits, int32_t axis, const char *method, oasp_ai_error *error) {
    if (!tensor || (bits != 4 && bits != 8) || axis < -1 || axis >= (int32_t)tensor->desc.rank ||
        tensor->desc.dtype < OASP_AI_DTYPE_F16 || tensor->quantized_bits ||
        (method && strcmp(method, "minmax") && strcmp(method, "absmax"))) {
        oasp_ai_set_error(error, 5, "Quantize", "invalid quantization configuration",
                          "Use an unquantized floating Tensor, 4 or 8 bits, axis -1 or a valid channel axis, and minmax/absmax calibration.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = tensor->desc;
    desc.dtype = OASP_AI_DTYPE_I8;
    desc.flags &= ~OASP_AI_TENSOR_REQUIRES_GRAD;
    oasp_ai_tensor *result = oasp_ai_tensor_create(tensor->context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    result->quantized_bits = bits;
    result->quantized_axis = axis;
    result->quantized_zero_point = 0;
    const char *calibration = method ? method : "absmax";
    memcpy(result->quantized_calibration, calibration, strlen(calibration) + 1);
    uint64_t channels = axis < 0 ? 1 : (uint64_t)tensor->desc.shape[axis];
    double *maximums = calloc((size_t)channels, sizeof(*maximums));
    if (!maximums) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    for (uint64_t index = 0; index < tensor->element_count; index++) {
        uint64_t channel = axis < 0 ? 0 : channel_for_index(tensor, axis, index);
        double absolute = fabs(oasp_ai_tensor_read(tensor, index));
        if (absolute > maximums[channel])
            maximums[channel] = absolute;
    }
    double divisor = bits == 4 ? 7.0 : 127.0;
    if (axis < 0)
        result->quantized_scale = maximums[0] ? maximums[0] / divisor : 1;
    else {
        result->quantized_channel_count = (size_t)channels;
        result->quantized_channel_scales = malloc((size_t)channels * sizeof(*result->quantized_channel_scales));
        if (!result->quantized_channel_scales) {
            free(maximums);
            oasp_ai_tensor_release(result);
            return NULL;
        }
        for (uint64_t channel = 0; channel < channels; channel++)
            result->quantized_channel_scales[channel] = maximums[channel] ? maximums[channel] / divisor : 1;
        result->quantized_scale = result->quantized_channel_scales[0];
    }
    free(maximums);
    for (uint64_t index = 0; index < tensor->element_count; index++)
        oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(tensor, index));
    return result;
}

oasp_ai_tensor *oasp_ai_tensor_dequantize(oasp_ai_tensor *tensor, oasp_ai_dtype dtype, oasp_ai_error *error) {
    if (!tensor || !tensor->quantized_bits || dtype < OASP_AI_DTYPE_F16) {
        oasp_ai_set_error(error, 5, "Dequantize", "a quantized Tensor and floating destination dtype are required",
                          "Quantize the Tensor first and choose float16, bfloat16, float32, or float64.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = tensor->desc;
    desc.dtype = dtype;
    oasp_ai_tensor *result = oasp_ai_tensor_create(tensor->context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    for (uint64_t index = 0; index < tensor->element_count; index++)
        oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(tensor, index));
    tensor->context->metrics.fallback_count++;
    return result;
}
