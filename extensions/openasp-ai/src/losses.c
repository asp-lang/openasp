// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <stdint.h>
#include <stdlib.h>

static int validate_pair(oasp_ai_context *context, oasp_ai_tensor *input, oasp_ai_tensor *target,
                         oasp_ai_reduction reduction, int allow_batch_mean, const char *operation,
                         oasp_ai_error *error) {
    if (!context || !input || !target || reduction > OASP_AI_REDUCTION_BATCH_MEAN ||
        (!allow_batch_mean && reduction == OASP_AI_REDUCTION_BATCH_MEAN)) {
        oasp_ai_set_error(error, 5, operation, "invalid loss arguments or reduction",
                          "Pass two tensors and use none, mean, or sum reduction.");
        return -1;
    }
    if (input->desc.rank != target->desc.rank || input->element_count != target->element_count) {
        oasp_ai_set_error(error, 5, operation, "input and target shapes must match",
                          "Reshape or broadcast the target explicitly before computing the loss.");
        return -1;
    }
    for (uint32_t axis = 0; axis < input->desc.rank; axis++)
        if (input->desc.shape[axis] != target->desc.shape[axis]) {
            oasp_ai_set_error(error, 5, operation, "input and target shapes must match",
                              "Reshape or broadcast the target explicitly before computing the loss.");
            return -1;
        }
    oasp_ai_tensor *tensors[] = {input, target};
    int32_t backend_index;
    return oasp_ai_tensor_same_device(tensors, 2, &backend_index, error);
}

static oasp_ai_tensor *constant_like(oasp_ai_tensor *reference, double value, oasp_ai_error *error) {
    oasp_ai_tensor_desc desc = {.dtype = reference->desc.dtype, .rank = 1, .shape = {1}};
    int32_t backend_index = oasp_ai_tensor_backend_index(reference);
    oasp_ai_tensor *result = backend_index >= 0
                                 ? oasp_ai_tensor_create_on_backend(reference->context, &desc,
                                                                    backend_index, error)
                                 : oasp_ai_tensor_create(reference->context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    if (backend_index >= 0) {
        oasp_ai_fill_attributes attributes = {.value = value};
        oasp_ai_tensor *outputs[] = {result};
        if (oasp_ai_backend_execute(reference->context, backend_index, OASP_AI_BACKEND_OP_FILL,
                                    NULL, 0, outputs, 1, &attributes, sizeof(attributes), error)) {
            oasp_ai_tensor_release(result);
            return NULL;
        }
    } else
        for (uint64_t index = 0; index < result->element_count; index++)
            oasp_ai_tensor_write(result, index, value);
    return result;
}

static oasp_ai_tensor *finish_loss(oasp_ai_tensor *elementwise, oasp_ai_reduction reduction,
                                   oasp_ai_error *error) {
    if (!elementwise)
        return NULL;
    if (reduction == OASP_AI_REDUCTION_NONE)
        return elementwise;
    double batch = elementwise->desc.rank ? (double)elementwise->desc.shape[0] : 1;
    oasp_ai_operator operation = reduction == OASP_AI_REDUCTION_MEAN ? OASP_AI_OP_MEAN : OASP_AI_OP_SUM;
    oasp_ai_tensor *result = oasp_ai_reduce(elementwise->context, operation, elementwise, -1, 0, error);
    oasp_ai_tensor_release(elementwise);
    if (!result || reduction != OASP_AI_REDUCTION_BATCH_MEAN)
        return result;
    oasp_ai_tensor *divisor = constant_like(result, batch, error);
    oasp_ai_tensor *mean = divisor ? oasp_ai_binary(result->context, OASP_AI_OP_DIV, result, divisor, error) : NULL;
    oasp_ai_tensor_release(divisor);
    oasp_ai_tensor_release(result);
    return mean;
}

static oasp_ai_tensor *absolute_tensor(oasp_ai_tensor *input, oasp_ai_error *error) {
    oasp_ai_tensor *negative_one = constant_like(input, -1, error);
    oasp_ai_tensor *negative = negative_one
                                   ? oasp_ai_binary(input->context, OASP_AI_OP_MUL, input, negative_one, error)
                                   : NULL;
    oasp_ai_tensor *absolute = negative
                                   ? oasp_ai_binary(input->context, OASP_AI_OP_MAXIMUM, input, negative, error)
                                   : NULL;
    oasp_ai_tensor_release(negative);
    oasp_ai_tensor_release(negative_one);
    return absolute;
}

static oasp_ai_tensor *clamp_probability(oasp_ai_tensor *input, oasp_ai_error *error) {
    oasp_ai_tensor *epsilon = constant_like(input, 1e-7, error);
    oasp_ai_tensor *lower = epsilon
                                ? oasp_ai_binary(input->context, OASP_AI_OP_MAXIMUM, input, epsilon, error)
                                : NULL;
    oasp_ai_tensor *negative_one = lower ? constant_like(lower, -1, error) : NULL;
    oasp_ai_tensor *negative = negative_one
                                   ? oasp_ai_binary(input->context, OASP_AI_OP_MUL, lower, negative_one, error)
                                   : NULL;
    oasp_ai_tensor *negative_upper = negative ? constant_like(negative, -(1.0 - 1e-7), error) : NULL;
    oasp_ai_tensor *maximum = negative_upper
                                  ? oasp_ai_binary(input->context, OASP_AI_OP_MAXIMUM, negative,
                                                   negative_upper, error)
                                  : NULL;
    oasp_ai_tensor *result = maximum
                                 ? oasp_ai_binary(input->context, OASP_AI_OP_MUL, maximum,
                                                  negative_one, error)
                                 : NULL;
    oasp_ai_tensor_release(maximum);
    oasp_ai_tensor_release(negative_upper);
    oasp_ai_tensor_release(negative);
    oasp_ai_tensor_release(negative_one);
    oasp_ai_tensor_release(lower);
    oasp_ai_tensor_release(epsilon);
    return result;
}

oasp_ai_tensor *oasp_ai_mse_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                 oasp_ai_tensor *target, oasp_ai_reduction reduction,
                                 oasp_ai_error *error) {
    if (validate_pair(context, input, target, reduction, 0, "MSELoss", error))
        return NULL;
    oasp_ai_tensor *difference = oasp_ai_binary(context, OASP_AI_OP_SUB, input, target, error);
    oasp_ai_tensor *squared = difference
                                  ? oasp_ai_binary(context, OASP_AI_OP_MUL, difference, difference, error)
                                  : NULL;
    oasp_ai_tensor_release(difference);
    return finish_loss(squared, reduction, error);
}

oasp_ai_tensor *oasp_ai_l1_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                oasp_ai_tensor *target, oasp_ai_reduction reduction,
                                oasp_ai_error *error) {
    if (validate_pair(context, input, target, reduction, 0, "L1Loss", error))
        return NULL;
    oasp_ai_tensor *difference = oasp_ai_binary(context, OASP_AI_OP_SUB, input, target, error);
    oasp_ai_tensor *absolute = difference ? absolute_tensor(difference, error) : NULL;
    oasp_ai_tensor_release(difference);
    return finish_loss(absolute, reduction, error);
}

oasp_ai_tensor *oasp_ai_bce_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                 oasp_ai_tensor *target, oasp_ai_reduction reduction,
                                 oasp_ai_error *error) {
    if (validate_pair(context, input, target, reduction, 0, "BCELoss", error))
        return NULL;
    oasp_ai_tensor *probability = clamp_probability(input, error);
    oasp_ai_tensor *one = probability ? constant_like(probability, 1, error) : NULL;
    oasp_ai_tensor *log_probability = probability
                                          ? oasp_ai_unary(context, OASP_AI_OP_LOG, probability, 0, error)
                                          : NULL;
    oasp_ai_tensor *first = log_probability
                                ? oasp_ai_binary(context, OASP_AI_OP_MUL, target,
                                                 log_probability, error)
                                : NULL;
    oasp_ai_tensor *one_minus_target = one
                                           ? oasp_ai_binary(context, OASP_AI_OP_SUB, one, target, error)
                                           : NULL;
    oasp_ai_tensor *one_minus_probability = one
                                                ? oasp_ai_binary(context, OASP_AI_OP_SUB, one,
                                                                 probability, error)
                                                : NULL;
    oasp_ai_tensor *log_inverse = one_minus_probability
                                      ? oasp_ai_unary(context, OASP_AI_OP_LOG,
                                                      one_minus_probability, 0, error)
                                      : NULL;
    oasp_ai_tensor *second = log_inverse
                                 ? oasp_ai_binary(context, OASP_AI_OP_MUL, one_minus_target,
                                                  log_inverse, error)
                                 : NULL;
    oasp_ai_tensor *sum = first && second
                              ? oasp_ai_binary(context, OASP_AI_OP_ADD, first, second, error)
                              : NULL;
    oasp_ai_tensor *negative_one = sum ? constant_like(sum, -1, error) : NULL;
    oasp_ai_tensor *elementwise = negative_one
                                      ? oasp_ai_binary(context, OASP_AI_OP_MUL, sum,
                                                       negative_one, error)
                                      : NULL;
    oasp_ai_tensor_release(negative_one);
    oasp_ai_tensor_release(sum);
    oasp_ai_tensor_release(second);
    oasp_ai_tensor_release(log_inverse);
    oasp_ai_tensor_release(one_minus_probability);
    oasp_ai_tensor_release(one_minus_target);
    oasp_ai_tensor_release(first);
    oasp_ai_tensor_release(log_probability);
    oasp_ai_tensor_release(one);
    oasp_ai_tensor_release(probability);
    return finish_loss(elementwise, reduction, error);
}

oasp_ai_tensor *oasp_ai_bce_with_logits_loss_ex(oasp_ai_context *context, oasp_ai_tensor *input,
                                                oasp_ai_tensor *target,
                                                oasp_ai_tensor *positive_weight,
                                                oasp_ai_reduction reduction,
                                                oasp_ai_error *error) {
    if (validate_pair(context, input, target, reduction, 0, "BCEWithLogitsLoss", error))
        return NULL;
    if (positive_weight) {
        oasp_ai_tensor *tensors[] = {input, target, positive_weight};
        int32_t backend_index;
        if ((positive_weight->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD) ||
            oasp_ai_tensor_same_device(tensors, 3, &backend_index, error)) {
            if (positive_weight->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD)
                oasp_ai_set_error(error, 5, "BCEWithLogitsLoss",
                                  "positive weight must not require gradients",
                                  "Use a constant positive-weight Tensor.");
            return NULL;
        }
    }
    oasp_ai_tensor *zero = constant_like(input, 0, error);
    oasp_ai_tensor *one = zero ? constant_like(input, 1, error) : NULL;
    oasp_ai_tensor *negative_one = one ? constant_like(input, -1, error) : NULL;
    oasp_ai_tensor *negative_input = negative_one
                                         ? oasp_ai_binary(context, OASP_AI_OP_MUL, input,
                                                          negative_one, error)
                                         : NULL;
    oasp_ai_tensor *maximum_negative = negative_input && zero
                                           ? oasp_ai_binary(context, OASP_AI_OP_MAXIMUM,
                                                            negative_input, zero, error)
                                           : NULL;
    oasp_ai_tensor *absolute = absolute_tensor(input, error);
    oasp_ai_tensor *negative_absolute = negative_one
                                            ? oasp_ai_binary(context, OASP_AI_OP_MUL, absolute,
                                                             negative_one, error)
                                            : NULL;
    oasp_ai_tensor *exponential = negative_absolute
                                      ? oasp_ai_unary(context, OASP_AI_OP_EXP,
                                                      negative_absolute, 0, error)
                                      : NULL;
    oasp_ai_tensor *one_plus = one
                                   ? oasp_ai_binary(context, OASP_AI_OP_ADD, one, exponential, error)
                                   : NULL;
    oasp_ai_tensor *log_term = one_plus
                                   ? oasp_ai_unary(context, OASP_AI_OP_LOG, one_plus, 0, error)
                                   : NULL;
    oasp_ai_tensor *softplus_negative = maximum_negative && log_term
                                            ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                             maximum_negative, log_term, error)
                                            : NULL;
    oasp_ai_tensor *one_minus_target = one
                                           ? oasp_ai_binary(context, OASP_AI_OP_SUB, one, target, error)
                                           : NULL;
    oasp_ai_tensor *linear = one_minus_target
                                 ? oasp_ai_binary(context, OASP_AI_OP_MUL, one_minus_target,
                                                  input, error)
                                 : NULL;
    oasp_ai_tensor *log_weight = NULL;
    if (positive_weight) {
        oasp_ai_tensor *weight_delta =
            oasp_ai_binary(context, OASP_AI_OP_SUB, positive_weight, one, error);
        oasp_ai_tensor *weighted_target = weight_delta
                                              ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                               weight_delta, target, error)
                                              : NULL;
        log_weight = weighted_target
                         ? oasp_ai_binary(context, OASP_AI_OP_ADD, one, weighted_target, error)
                         : NULL;
        oasp_ai_tensor_release(weighted_target);
        oasp_ai_tensor_release(weight_delta);
    } else {
        log_weight = one;
        oasp_ai_tensor_retain(log_weight);
    }
    oasp_ai_tensor *weighted_softplus = log_weight && softplus_negative
                                            ? oasp_ai_binary(context, OASP_AI_OP_MUL, log_weight,
                                                             softplus_negative, error)
                                            : NULL;
    oasp_ai_tensor *elementwise = linear && weighted_softplus
                                      ? oasp_ai_binary(context, OASP_AI_OP_ADD, linear,
                                                       weighted_softplus, error)
                                      : NULL;
    oasp_ai_tensor_release(weighted_softplus);
    oasp_ai_tensor_release(log_weight);
    oasp_ai_tensor_release(linear);
    oasp_ai_tensor_release(one_minus_target);
    oasp_ai_tensor_release(softplus_negative);
    oasp_ai_tensor_release(log_term);
    oasp_ai_tensor_release(one_plus);
    oasp_ai_tensor_release(exponential);
    oasp_ai_tensor_release(negative_absolute);
    oasp_ai_tensor_release(absolute);
    oasp_ai_tensor_release(maximum_negative);
    oasp_ai_tensor_release(negative_input);
    oasp_ai_tensor_release(negative_one);
    oasp_ai_tensor_release(one);
    oasp_ai_tensor_release(zero);
    return finish_loss(elementwise, reduction, error);
}

oasp_ai_tensor *oasp_ai_bce_with_logits_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                             oasp_ai_tensor *target,
                                             oasp_ai_reduction reduction,
                                             oasp_ai_error *error) {
    return oasp_ai_bce_with_logits_loss_ex(context, input, target, NULL, reduction, error);
}

oasp_ai_tensor *oasp_ai_kl_div_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                    oasp_ai_tensor *target, oasp_ai_reduction reduction,
                                    int log_target, oasp_ai_error *error) {
    if (validate_pair(context, input, target, reduction, 1, "KLDivLoss", error))
        return NULL;
    oasp_ai_tensor *weight;
    oasp_ai_tensor *difference;
    if (log_target) {
        weight = oasp_ai_unary(context, OASP_AI_OP_EXP, target, 0, error);
        difference = oasp_ai_binary(context, OASP_AI_OP_SUB, target, input, error);
    } else {
        oasp_ai_tensor *clamped = clamp_probability(target, error);
        weight = clamped ? oasp_ai_tensor_clone(clamped, error) : NULL;
        oasp_ai_tensor *log_target_tensor = clamped
                                                ? oasp_ai_unary(context, OASP_AI_OP_LOG,
                                                                clamped, 0, error)
                                                : NULL;
        difference = log_target_tensor
                         ? oasp_ai_binary(context, OASP_AI_OP_SUB, log_target_tensor, input, error)
                         : NULL;
        oasp_ai_tensor_release(log_target_tensor);
        oasp_ai_tensor_release(clamped);
    }
    oasp_ai_tensor *elementwise = weight && difference
                                      ? oasp_ai_binary(context, OASP_AI_OP_MUL, weight,
                                                       difference, error)
                                      : NULL;
    oasp_ai_tensor_release(difference);
    oasp_ai_tensor_release(weight);
    return finish_loss(elementwise, reduction, error);
}

oasp_ai_tensor *oasp_ai_huber_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                   oasp_ai_tensor *target, double delta,
                                   oasp_ai_reduction reduction, oasp_ai_error *error) {
    if (delta <= 0 || validate_pair(context, input, target, reduction, 0, "HuberLoss", error)) {
        if (delta <= 0)
            oasp_ai_set_error(error, 5, "HuberLoss", "delta must be positive",
                              "Pass a delta greater than zero.");
        return NULL;
    }
    oasp_ai_tensor *difference = oasp_ai_binary(context, OASP_AI_OP_SUB, input, target, error);
    oasp_ai_tensor *absolute = difference ? absolute_tensor(difference, error) : NULL;
    oasp_ai_tensor *delta_tensor = absolute ? constant_like(absolute, delta, error) : NULL;
    oasp_ai_tensor *above_delta = delta_tensor
                                      ? oasp_ai_binary(context, OASP_AI_OP_SUB, absolute,
                                                       delta_tensor, error)
                                      : NULL;
    oasp_ai_tensor *excess = above_delta
                                 ? oasp_ai_unary(context, OASP_AI_OP_RELU, above_delta, 0, error)
                                 : NULL;
    oasp_ai_tensor *quadratic = excess
                                    ? oasp_ai_binary(context, OASP_AI_OP_SUB, absolute, excess, error)
                                    : NULL;
    oasp_ai_tensor *squared = quadratic
                                  ? oasp_ai_binary(context, OASP_AI_OP_MUL, quadratic,
                                                   quadratic, error)
                                  : NULL;
    oasp_ai_tensor *half = squared ? constant_like(squared, 0.5, error) : NULL;
    oasp_ai_tensor *quadratic_term = half
                                         ? oasp_ai_binary(context, OASP_AI_OP_MUL, squared, half, error)
                                         : NULL;
    oasp_ai_tensor *linear_term = delta_tensor && excess
                                      ? oasp_ai_binary(context, OASP_AI_OP_MUL, delta_tensor,
                                                       excess, error)
                                      : NULL;
    oasp_ai_tensor *elementwise = quadratic_term && linear_term
                                      ? oasp_ai_binary(context, OASP_AI_OP_ADD, quadratic_term,
                                                       linear_term, error)
                                      : NULL;
    oasp_ai_tensor_release(linear_term);
    oasp_ai_tensor_release(quadratic_term);
    oasp_ai_tensor_release(half);
    oasp_ai_tensor_release(squared);
    oasp_ai_tensor_release(quadratic);
    oasp_ai_tensor_release(excess);
    oasp_ai_tensor_release(above_delta);
    oasp_ai_tensor_release(delta_tensor);
    oasp_ai_tensor_release(absolute);
    oasp_ai_tensor_release(difference);
    return finish_loss(elementwise, reduction, error);
}

oasp_ai_tensor *oasp_ai_smooth_l1_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                       oasp_ai_tensor *target, double beta,
                                       oasp_ai_reduction reduction, oasp_ai_error *error) {
    if (beta < 0 || validate_pair(context, input, target, reduction, 0, "SmoothL1Loss", error)) {
        if (beta < 0)
            oasp_ai_set_error(error, 5, "SmoothL1Loss", "beta must be non-negative",
                              "Use beta zero for L1Loss behavior or a positive transition width.");
        return NULL;
    }
    if (beta == 0)
        return oasp_ai_l1_loss(context, input, target, reduction, error);
    oasp_ai_tensor *difference = oasp_ai_binary(context, OASP_AI_OP_SUB, input, target, error);
    oasp_ai_tensor *absolute = difference ? absolute_tensor(difference, error) : NULL;
    oasp_ai_tensor *beta_tensor = absolute ? constant_like(absolute, beta, error) : NULL;
    oasp_ai_tensor *above_beta = beta_tensor
                                     ? oasp_ai_binary(context, OASP_AI_OP_SUB, absolute,
                                                      beta_tensor, error)
                                     : NULL;
    oasp_ai_tensor *excess = above_beta
                                 ? oasp_ai_unary(context, OASP_AI_OP_RELU, above_beta, 0, error)
                                 : NULL;
    oasp_ai_tensor *quadratic = excess
                                    ? oasp_ai_binary(context, OASP_AI_OP_SUB, absolute, excess, error)
                                    : NULL;
    oasp_ai_tensor *squared = quadratic
                                  ? oasp_ai_binary(context, OASP_AI_OP_MUL, quadratic,
                                                   quadratic, error)
                                  : NULL;
    oasp_ai_tensor *coefficient = squared ? constant_like(squared, 0.5 / beta, error) : NULL;
    oasp_ai_tensor *quadratic_term = coefficient
                                         ? oasp_ai_binary(context, OASP_AI_OP_MUL, squared,
                                                          coefficient, error)
                                         : NULL;
    oasp_ai_tensor *elementwise = quadratic_term && excess
                                      ? oasp_ai_binary(context, OASP_AI_OP_ADD, quadratic_term,
                                                       excess, error)
                                      : NULL;
    oasp_ai_tensor_release(quadratic_term);
    oasp_ai_tensor_release(coefficient);
    oasp_ai_tensor_release(squared);
    oasp_ai_tensor_release(quadratic);
    oasp_ai_tensor_release(excess);
    oasp_ai_tensor_release(above_beta);
    oasp_ai_tensor_release(beta_tensor);
    oasp_ai_tensor_release(absolute);
    oasp_ai_tensor_release(difference);
    return finish_loss(elementwise, reduction, error);
}

static oasp_ai_tensor *power_with_scalar(oasp_ai_tensor *input, double exponent,
                                         oasp_ai_error *error) {
    oasp_ai_tensor *power = constant_like(input, exponent, error);
    oasp_ai_tensor *result = power
                                 ? oasp_ai_binary(input->context, OASP_AI_OP_POW, input, power, error)
                                 : NULL;
    oasp_ai_tensor_release(power);
    return result;
}

oasp_ai_tensor *oasp_ai_focal_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                   oasp_ai_tensor *target, double alpha, double gamma,
                                   oasp_ai_reduction reduction, oasp_ai_error *error) {
    if (!context || !input || !target || gamma < 0 || alpha > 1 ||
        reduction > OASP_AI_REDUCTION_SUM) {
        oasp_ai_set_error(error, 5, "FocalLoss", "invalid focal-loss arguments",
                          "Use gamma >= 0, alpha in [0,1] or -1, and none/mean/sum reduction.");
        return NULL;
    }
    oasp_ai_tensor *elementwise = NULL;
    if (target->desc.dtype == OASP_AI_DTYPE_I64) {
        if (input->desc.rank != 2 || target->desc.rank != 1 ||
            input->desc.shape[0] != target->desc.shape[0]) {
            oasp_ai_set_error(error, 5, "FocalLoss",
                              "multiclass focal loss expects logits [batch, classes] and labels [batch]",
                              "Pass int64 class labels or same-shaped float binary targets.");
            return NULL;
        }
        oasp_ai_tensor *cross_entropy = oasp_ai_cross_entropy_ex(
            context, input, target, NULL, -100, 0, OASP_AI_REDUCTION_NONE, error);
        oasp_ai_tensor *negative_one = cross_entropy ? constant_like(cross_entropy, -1, error) : NULL;
        oasp_ai_tensor *negative_loss = negative_one
                                            ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                             cross_entropy, negative_one, error)
                                            : NULL;
        oasp_ai_tensor *probability = negative_loss
                                          ? oasp_ai_unary(context, OASP_AI_OP_EXP,
                                                          negative_loss, 0, error)
                                          : NULL;
        oasp_ai_tensor *one = probability ? constant_like(probability, 1, error) : NULL;
        oasp_ai_tensor *one_minus_probability = one
                                                    ? oasp_ai_binary(context, OASP_AI_OP_SUB,
                                                                     one, probability, error)
                                                    : NULL;
        oasp_ai_tensor *modulation = one_minus_probability
                                         ? power_with_scalar(one_minus_probability, gamma, error)
                                         : NULL;
        elementwise = modulation
                          ? oasp_ai_binary(context, OASP_AI_OP_MUL, cross_entropy,
                                           modulation, error)
                          : NULL;
        if (elementwise && alpha >= 0) {
            oasp_ai_tensor *alpha_tensor = constant_like(elementwise, alpha, error);
            oasp_ai_tensor *weighted = alpha_tensor
                                           ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                            elementwise, alpha_tensor, error)
                                           : NULL;
            oasp_ai_tensor_release(alpha_tensor);
            oasp_ai_tensor_release(elementwise);
            elementwise = weighted;
        }
        oasp_ai_tensor_release(modulation);
        oasp_ai_tensor_release(one_minus_probability);
        oasp_ai_tensor_release(one);
        oasp_ai_tensor_release(probability);
        oasp_ai_tensor_release(negative_loss);
        oasp_ai_tensor_release(negative_one);
        oasp_ai_tensor_release(cross_entropy);
    } else {
        if (validate_pair(context, input, target, reduction, 0, "FocalLoss", error))
            return NULL;
        oasp_ai_tensor *probability = oasp_ai_unary(context, OASP_AI_OP_SIGMOID,
                                                    input, 0, error);
        oasp_ai_tensor *one = probability ? constant_like(probability, 1, error) : NULL;
        oasp_ai_tensor *one_minus_probability = one
                                                    ? oasp_ai_binary(context, OASP_AI_OP_SUB,
                                                                     one, probability, error)
                                                    : NULL;
        oasp_ai_tensor *one_minus_target = one
                                               ? oasp_ai_binary(context, OASP_AI_OP_SUB,
                                                                one, target, error)
                                               : NULL;
        oasp_ai_tensor *positive = probability
                                       ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                        probability, target, error)
                                       : NULL;
        oasp_ai_tensor *negative = one_minus_probability && one_minus_target
                                       ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                        one_minus_probability,
                                                        one_minus_target, error)
                                       : NULL;
        oasp_ai_tensor *probability_target = positive && negative
                                                 ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                                  positive, negative, error)
                                                 : NULL;
        oasp_ai_tensor *one_minus_target_probability = probability_target
                                                           ? oasp_ai_binary(
                                                                 context, OASP_AI_OP_SUB,
                                                                 one, probability_target, error)
                                                           : NULL;
        oasp_ai_tensor *modulation = one_minus_target_probability
                                         ? power_with_scalar(one_minus_target_probability,
                                                             gamma, error)
                                         : NULL;
        oasp_ai_tensor *binary_cross_entropy = oasp_ai_bce_with_logits_loss_ex(
            context, input, target, NULL, OASP_AI_REDUCTION_NONE, error);
        elementwise = modulation && binary_cross_entropy
                          ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                           modulation, binary_cross_entropy, error)
                          : NULL;
        if (elementwise && alpha >= 0) {
            oasp_ai_tensor *alpha_tensor = constant_like(elementwise, alpha, error);
            oasp_ai_tensor *one_minus_alpha = alpha_tensor
                                                  ? oasp_ai_binary(context, OASP_AI_OP_SUB,
                                                                   one, alpha_tensor, error)
                                                  : NULL;
            oasp_ai_tensor *positive_alpha = alpha_tensor
                                                 ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                                  target, alpha_tensor, error)
                                                 : NULL;
            oasp_ai_tensor *negative_alpha = one_minus_target && one_minus_alpha
                                                 ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                                  one_minus_target,
                                                                  one_minus_alpha, error)
                                                 : NULL;
            oasp_ai_tensor *alpha_target = positive_alpha && negative_alpha
                                               ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                                positive_alpha,
                                                                negative_alpha, error)
                                               : NULL;
            oasp_ai_tensor *weighted = alpha_target
                                           ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                            elementwise, alpha_target, error)
                                           : NULL;
            oasp_ai_tensor_release(alpha_target);
            oasp_ai_tensor_release(negative_alpha);
            oasp_ai_tensor_release(positive_alpha);
            oasp_ai_tensor_release(one_minus_alpha);
            oasp_ai_tensor_release(alpha_tensor);
            oasp_ai_tensor_release(elementwise);
            elementwise = weighted;
        }
        oasp_ai_tensor_release(binary_cross_entropy);
        oasp_ai_tensor_release(modulation);
        oasp_ai_tensor_release(one_minus_target_probability);
        oasp_ai_tensor_release(probability_target);
        oasp_ai_tensor_release(negative);
        oasp_ai_tensor_release(positive);
        oasp_ai_tensor_release(one_minus_target);
        oasp_ai_tensor_release(one_minus_probability);
        oasp_ai_tensor_release(one);
        oasp_ai_tensor_release(probability);
    }
    return finish_loss(elementwise, reduction, error);
}

oasp_ai_tensor *oasp_ai_dice_loss(oasp_ai_context *context, oasp_ai_tensor *input,
                                  oasp_ai_tensor *target, double smooth, double epsilon,
                                  int from_logits, oasp_ai_reduction reduction,
                                  oasp_ai_error *error) {
    if (smooth < 0 || epsilon <= 0 ||
        validate_pair(context, input, target, reduction, 0, "DiceLoss", error)) {
        if (smooth < 0 || epsilon <= 0)
            oasp_ai_set_error(error, 5, "DiceLoss",
                              "smooth must be non-negative and epsilon must be positive",
                              "Use non-negative smoothing and a small positive epsilon.");
        return NULL;
    }
    int64_t batch = input->desc.rank > 1 ? input->desc.shape[0] : 1;
    int64_t flattened_shape[] = {batch, (int64_t)(input->element_count / (uint64_t)batch)};
    oasp_ai_tensor *probability = from_logits
                                      ? oasp_ai_unary(context, OASP_AI_OP_SIGMOID,
                                                      input, 0, error)
                                      : oasp_ai_tensor_clone(input, error);
    oasp_ai_tensor *flat_probability = probability
                                           ? oasp_ai_tensor_reshape(probability,
                                                                    flattened_shape, 2, error)
                                           : NULL;
    oasp_ai_tensor *flat_target = oasp_ai_tensor_reshape(target, flattened_shape, 2, error);
    oasp_ai_tensor *product = flat_probability && flat_target
                                  ? oasp_ai_binary(context, OASP_AI_OP_MUL, flat_probability,
                                                   flat_target, error)
                                  : NULL;
    oasp_ai_tensor *intersection = product
                                       ? oasp_ai_reduce(context, OASP_AI_OP_SUM, product, 1, 0, error)
                                       : NULL;
    oasp_ai_tensor *probability_sum = flat_probability
                                          ? oasp_ai_reduce(context, OASP_AI_OP_SUM,
                                                           flat_probability, 1, 0, error)
                                          : NULL;
    oasp_ai_tensor *target_sum = flat_target
                                     ? oasp_ai_reduce(context, OASP_AI_OP_SUM,
                                                      flat_target, 1, 0, error)
                                     : NULL;
    oasp_ai_tensor *two = intersection ? constant_like(intersection, 2, error) : NULL;
    oasp_ai_tensor *smooth_tensor = intersection ? constant_like(intersection, smooth, error) : NULL;
    oasp_ai_tensor *epsilon_tensor = intersection ? constant_like(intersection, epsilon, error) : NULL;
    oasp_ai_tensor *twice_intersection = two
                                             ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                              intersection, two, error)
                                             : NULL;
    oasp_ai_tensor *numerator = twice_intersection && smooth_tensor
                                    ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                     twice_intersection, smooth_tensor, error)
                                    : NULL;
    oasp_ai_tensor *sum = probability_sum && target_sum
                              ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                               probability_sum, target_sum, error)
                              : NULL;
    oasp_ai_tensor *smoothed_sum = sum && smooth_tensor
                                       ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                        sum, smooth_tensor, error)
                                       : NULL;
    oasp_ai_tensor *denominator = smoothed_sum && epsilon_tensor
                                      ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                       smoothed_sum, epsilon_tensor, error)
                                      : NULL;
    oasp_ai_tensor *score = numerator && denominator
                                ? oasp_ai_binary(context, OASP_AI_OP_DIV,
                                                 numerator, denominator, error)
                                : NULL;
    oasp_ai_tensor *one = score ? constant_like(score, 1, error) : NULL;
    oasp_ai_tensor *elementwise = one
                                      ? oasp_ai_binary(context, OASP_AI_OP_SUB, one, score, error)
                                      : NULL;
    oasp_ai_tensor_release(one);
    oasp_ai_tensor_release(score);
    oasp_ai_tensor_release(denominator);
    oasp_ai_tensor_release(smoothed_sum);
    oasp_ai_tensor_release(sum);
    oasp_ai_tensor_release(numerator);
    oasp_ai_tensor_release(twice_intersection);
    oasp_ai_tensor_release(epsilon_tensor);
    oasp_ai_tensor_release(smooth_tensor);
    oasp_ai_tensor_release(two);
    oasp_ai_tensor_release(target_sum);
    oasp_ai_tensor_release(probability_sum);
    oasp_ai_tensor_release(intersection);
    oasp_ai_tensor_release(product);
    oasp_ai_tensor_release(flat_target);
    oasp_ai_tensor_release(flat_probability);
    oasp_ai_tensor_release(probability);
    return finish_loss(elementwise, reduction, error);
}

static oasp_ai_tensor *paired_labels(oasp_ai_tensor *reference, int64_t count,
                                     oasp_ai_error *error) {
    int64_t *values = malloc((size_t)count * sizeof(*values));
    if (!values)
        return NULL;
    for (int64_t index = 0; index < count; index++)
        values[index] = index;
    oasp_ai_tensor_desc desc = {.dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {count}};
    oasp_ai_tensor *host = oasp_ai_tensor_create(reference->context, &desc, values,
                                                 (size_t)count * sizeof(*values), error);
    free(values);
    if (!host)
        return NULL;
    int32_t backend_index = oasp_ai_tensor_backend_index(reference);
    if (backend_index < 0)
        return host;
    char device[64];
    oasp_ai_tensor *result = !oasp_ai_tensor_device(reference, device, sizeof(device))
                                 ? oasp_ai_tensor_to_device(host, device, error)
                                 : NULL;
    oasp_ai_tensor_release(host);
    return result;
}

oasp_ai_tensor *oasp_ai_info_nce_loss(oasp_ai_context *context, oasp_ai_tensor *query,
                                      oasp_ai_tensor *key, double temperature, int symmetric,
                                      oasp_ai_reduction reduction, oasp_ai_error *error) {
    if (!context || !query || !key || query->desc.rank != 2 || key->desc.rank != 2 ||
        query->desc.shape[0] != key->desc.shape[0] ||
        query->desc.shape[1] != key->desc.shape[1] || temperature <= 0 ||
        reduction > OASP_AI_REDUCTION_SUM) {
        oasp_ai_set_error(error, 5, "InfoNCELoss",
                          "expected paired [batch, features] tensors, positive temperature, and none/mean/sum reduction",
                          "Align query/key shapes and use a positive temperature.");
        return NULL;
    }
    oasp_ai_tensor *tensors[] = {query, key};
    int32_t backend_index;
    if (oasp_ai_tensor_same_device(tensors, 2, &backend_index, error))
        return NULL;
    oasp_ai_tensor *query_norm = oasp_ai_reduce(context, OASP_AI_OP_NORM, query, 1, 1, error);
    oasp_ai_tensor *key_norm = oasp_ai_reduce(context, OASP_AI_OP_NORM, key, 1, 1, error);
    oasp_ai_tensor *normalized_query = query_norm
                                           ? oasp_ai_binary(context, OASP_AI_OP_DIV,
                                                            query, query_norm, error)
                                           : NULL;
    oasp_ai_tensor *normalized_key = key_norm
                                         ? oasp_ai_binary(context, OASP_AI_OP_DIV,
                                                          key, key_norm, error)
                                         : NULL;
    oasp_ai_tensor *key_transpose = normalized_key
                                        ? oasp_ai_tensor_transpose(normalized_key, 0, 1, error)
                                        : NULL;
    oasp_ai_tensor *similarity = normalized_query && key_transpose
                                     ? oasp_ai_matmul(context, normalized_query,
                                                      key_transpose, error)
                                     : NULL;
    oasp_ai_tensor *temperature_tensor = similarity
                                             ? constant_like(similarity, temperature, error)
                                             : NULL;
    oasp_ai_tensor *logits = temperature_tensor
                                 ? oasp_ai_binary(context, OASP_AI_OP_DIV,
                                                  similarity, temperature_tensor, error)
                                 : NULL;
    oasp_ai_tensor *labels = logits ? paired_labels(logits, query->desc.shape[0], error) : NULL;
    oasp_ai_tensor *loss = labels
                               ? oasp_ai_cross_entropy_ex(context, logits, labels, NULL, -100,
                                                          0, reduction, error)
                               : NULL;
    if (loss && symmetric) {
        oasp_ai_tensor *query_transpose =
            oasp_ai_tensor_transpose(normalized_query, 0, 1, error);
        oasp_ai_tensor *reverse_similarity = query_transpose
                                                 ? oasp_ai_matmul(context, normalized_key,
                                                                  query_transpose, error)
                                                 : NULL;
        oasp_ai_tensor *reverse_logits = reverse_similarity
                                             ? oasp_ai_binary(context, OASP_AI_OP_DIV,
                                                              reverse_similarity,
                                                              temperature_tensor, error)
                                             : NULL;
        oasp_ai_tensor *reverse_loss = reverse_logits
                                           ? oasp_ai_cross_entropy_ex(
                                                 context, reverse_logits, labels, NULL, -100,
                                                 0, reduction, error)
                                           : NULL;
        oasp_ai_tensor *sum = reverse_loss
                                  ? oasp_ai_binary(context, OASP_AI_OP_ADD,
                                                   loss, reverse_loss, error)
                                  : NULL;
        oasp_ai_tensor *half = sum ? constant_like(sum, 0.5, error) : NULL;
        oasp_ai_tensor *average = half
                                      ? oasp_ai_binary(context, OASP_AI_OP_MUL,
                                                       sum, half, error)
                                      : NULL;
        oasp_ai_tensor_release(half);
        oasp_ai_tensor_release(sum);
        oasp_ai_tensor_release(reverse_loss);
        oasp_ai_tensor_release(reverse_logits);
        oasp_ai_tensor_release(reverse_similarity);
        oasp_ai_tensor_release(query_transpose);
        oasp_ai_tensor_release(loss);
        loss = average;
    }
    oasp_ai_tensor_release(labels);
    oasp_ai_tensor_release(logits);
    oasp_ai_tensor_release(temperature_tensor);
    oasp_ai_tensor_release(similarity);
    oasp_ai_tensor_release(key_transpose);
    oasp_ai_tensor_release(normalized_key);
    oasp_ai_tensor_release(normalized_query);
    oasp_ai_tensor_release(key_norm);
    oasp_ai_tensor_release(query_norm);
    return loss;
}
