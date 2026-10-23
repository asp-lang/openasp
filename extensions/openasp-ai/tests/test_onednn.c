// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void report_error(const char *stage, const oasp_ai_error *error) {
    fprintf(stderr, "%s failed: operation=%s message=%s suggestion=%s\n", stage,
            error->operation ? error->operation : "", error->message ? error->message : "",
            error->suggestion ? error->suggestion : "");
}

static void copy_f32(oasp_ai_tensor *tensor, float *values, size_t count, oasp_ai_error *error) {
    if (oasp_ai_tensor_copy_to_host(tensor, values, count * sizeof(*values), error)) {
        report_error("copy", error);
        abort();
    }
}

static float maximum_difference(oasp_ai_tensor *expected, oasp_ai_tensor *actual, size_t count, oasp_ai_error *error) {
    float *left = malloc(count * sizeof(*left));
    float *right = malloc(count * sizeof(*right));
    assert(left && right);
    copy_f32(expected, left, count, error);
    copy_f32(actual, right, count, error);
    float maximum = 0;
    for (size_t index = 0; index < count; index++)
        maximum = fmaxf(maximum, fabsf(left[index] - right[index]));
    free(right);
    free(left);
    return maximum;
}

static oasp_ai_tensor *to_onednn(oasp_ai_tensor *tensor, oasp_ai_error *error) {
    oasp_ai_tensor *result = oasp_ai_tensor_to_device(tensor, "cpu-onednn:0", error);
    if (!result)
        report_error("to_onednn", error);
    assert(result);
    return result;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    oasp_ai_error error = {0};
    oasp_ai_context_options options = {
        .seed = 42,
        .maximum_tensor_bytes = UINT64_C(1) << 30,
        .maximum_model_bytes = UINT64_C(8) << 30,
        .strict_device = 1,
        .training_enabled = 1,
    };
    oasp_ai_context *context = oasp_ai_context_create(&options, &error);
    assert(context);
    if (oasp_ai_context_load_backend(context, argv[1], NULL, &error)) {
        report_error("load backend", &error);
        return 1;
    }

    oasp_ai_tensor_desc left_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {2, 3, 4}};
    oasp_ai_tensor_desc right_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {2, 4, 5}};
    float left_values[24];
    float right_values[40];
    for (size_t index = 0; index < 24; index++)
        left_values[index] = (float)((int)(index % 11) - 5) / 8.0f;
    for (size_t index = 0; index < 40; index++)
        right_values[index] = (float)((int)(index % 13) - 6) / 7.0f;
    oasp_ai_tensor *cpu_left = oasp_ai_tensor_create(context, &left_desc, left_values, sizeof(left_values), &error);
    oasp_ai_tensor *cpu_right = oasp_ai_tensor_create(context, &right_desc, right_values, sizeof(right_values), &error);
    oasp_ai_tensor *onednn_left = to_onednn(cpu_left, &error);
    oasp_ai_tensor *onednn_right = to_onednn(cpu_right, &error);
    oasp_ai_tensor *cpu_product = oasp_ai_matmul(context, cpu_left, cpu_right, &error);
    oasp_ai_tensor *onednn_product = oasp_ai_matmul(context, onednn_left, onednn_right, &error);
    if (!onednn_product)
        report_error("matmul", &error);
    assert(cpu_product && onednn_product);
    float matmul_difference = maximum_difference(cpu_product, onednn_product, 30, &error);
    assert(matmul_difference < 1e-5f);

    enum { batch = 2, tokens = 49, width = 32, elements = batch * tokens * width };
    oasp_ai_tensor_desc attention_desc = {
        .dtype = OASP_AI_DTYPE_F32,
        .rank = 3,
        .shape = {batch, tokens, width},
    };
    float *query_values = malloc(elements * sizeof(*query_values));
    float *key_values = malloc(elements * sizeof(*key_values));
    float *value_values = malloc(elements * sizeof(*value_values));
    assert(query_values && key_values && value_values);
    for (size_t index = 0; index < elements; index++) {
        query_values[index] = (float)((int)(index % 23) - 11) / 32.0f;
        key_values[index] = (float)((int)(index % 19) - 9) / 32.0f;
        value_values[index] = (float)((int)(index % 17) - 8) / 16.0f;
    }
    oasp_ai_tensor *cpu_query = oasp_ai_tensor_create(context, &attention_desc, query_values, elements * sizeof(float), &error);
    oasp_ai_tensor *cpu_key = oasp_ai_tensor_create(context, &attention_desc, key_values, elements * sizeof(float), &error);
    oasp_ai_tensor *cpu_value = oasp_ai_tensor_create(context, &attention_desc, value_values, elements * sizeof(float), &error);
    oasp_ai_tensor *onednn_query = to_onednn(cpu_query, &error);
    oasp_ai_tensor *onednn_key = to_onednn(cpu_key, &error);
    oasp_ai_tensor *onednn_value = to_onednn(cpu_value, &error);
    assert(oasp_ai_tensor_requires_grad(cpu_query, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cpu_key, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cpu_value, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_query, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_key, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_value, 1, &error) == 0);
    oasp_ai_tensor *cpu_attention = oasp_ai_attention(context, cpu_query, cpu_key, cpu_value, NULL, 0, &error);
    oasp_ai_tensor *cpu_attention_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, cpu_attention, -1, 0, &error);
    assert(cpu_attention && cpu_attention_loss && oasp_ai_tensor_backward(cpu_attention_loss, NULL, 0, &error) == 0);
    oasp_ai_tensor *onednn_attention = oasp_ai_attention(context, onednn_query, onednn_key, onednn_value, NULL, 0, &error);
    oasp_ai_tensor *onednn_attention_loss = onednn_attention
        ? oasp_ai_reduce(context, OASP_AI_OP_SUM, onednn_attention, -1, 0, &error) : NULL;
    if (!onednn_attention || !onednn_attention_loss ||
        oasp_ai_tensor_backward(onednn_attention_loss, NULL, 0, &error)) {
        report_error("attention", &error);
        abort();
    }
    float attention_differences[4] = {
        maximum_difference(cpu_attention, onednn_attention, elements, &error),
        maximum_difference(oasp_ai_tensor_gradient(cpu_query), oasp_ai_tensor_gradient(onednn_query), elements, &error),
        maximum_difference(oasp_ai_tensor_gradient(cpu_key), oasp_ai_tensor_gradient(onednn_key), elements, &error),
        maximum_difference(oasp_ai_tensor_gradient(cpu_value), oasp_ai_tensor_gradient(onednn_value), elements, &error),
    };
    for (size_t index = 0; index < 4; index++)
        assert(attention_differences[index] < 1e-4f);

    oasp_ai_tensor_desc norm_parameter_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {width}};
    float norm_weight_values[width];
    float norm_bias_values[width];
    float *norm_upstream_values = malloc(elements * sizeof(*norm_upstream_values));
    assert(norm_upstream_values);
    for (size_t index = 0; index < width; index++) {
        norm_weight_values[index] = 0.5f + (float)index / 64.0f;
        norm_bias_values[index] = (float)((int)(index % 7) - 3) / 32.0f;
    }
    for (size_t index = 0; index < elements; index++)
        norm_upstream_values[index] = (float)((int)(index % 13) - 6) / 16.0f;
    oasp_ai_tensor *cpu_norm_weight =
        oasp_ai_tensor_create(context, &norm_parameter_desc, norm_weight_values, sizeof(norm_weight_values), &error);
    oasp_ai_tensor *cpu_norm_bias =
        oasp_ai_tensor_create(context, &norm_parameter_desc, norm_bias_values, sizeof(norm_bias_values), &error);
    oasp_ai_tensor *cpu_norm_upstream =
        oasp_ai_tensor_create(context, &attention_desc, norm_upstream_values, elements * sizeof(float), &error);
    oasp_ai_tensor *onednn_norm_weight = to_onednn(cpu_norm_weight, &error);
    oasp_ai_tensor *onednn_norm_bias = to_onednn(cpu_norm_bias, &error);
    oasp_ai_tensor *onednn_norm_upstream = to_onednn(cpu_norm_upstream, &error);
    assert(oasp_ai_tensor_requires_grad(cpu_value, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cpu_norm_weight, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cpu_norm_bias, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_value, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_norm_weight, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_norm_bias, 1, &error) == 0);
    oasp_ai_tensor *cpu_norm =
        oasp_ai_layer_norm(context, cpu_value, cpu_norm_weight, cpu_norm_bias, 1e-5, 0, &error);
    oasp_ai_tensor *onednn_norm =
        oasp_ai_layer_norm(context, onednn_value, onednn_norm_weight, onednn_norm_bias, 1e-5, 0, &error);
    assert(cpu_norm && onednn_norm);
    assert(oasp_ai_tensor_backward(cpu_norm, cpu_norm_upstream, 0, &error) == 0);
    if (oasp_ai_tensor_backward(onednn_norm, onednn_norm_upstream, 0, &error)) {
        report_error("layer norm", &error);
        abort();
    }
    float norm_differences[4] = {
        maximum_difference(cpu_norm, onednn_norm, elements, &error),
        maximum_difference(oasp_ai_tensor_gradient(cpu_value), oasp_ai_tensor_gradient(onednn_value), elements, &error),
        maximum_difference(oasp_ai_tensor_gradient(cpu_norm_weight), oasp_ai_tensor_gradient(onednn_norm_weight), width, &error),
        maximum_difference(oasp_ai_tensor_gradient(cpu_norm_bias), oasp_ai_tensor_gradient(onednn_norm_bias), width, &error),
    };
    for (size_t index = 0; index < 4; index++)
        assert(norm_differences[index] < 1e-4f);

    oasp_ai_tensor_desc logits_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {4, 5}};
    oasp_ai_tensor_desc labels_desc = {.dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {4}};
    float logits_values[20];
    int64_t labels_values[] = {0, 3, 1, 4};
    for (size_t index = 0; index < 20; index++)
        logits_values[index] = (float)((int)(index % 9) - 4) / 4.0f;
    oasp_ai_tensor *cpu_logits = oasp_ai_tensor_create(context, &logits_desc, logits_values, sizeof(logits_values), &error);
    oasp_ai_tensor *cpu_labels = oasp_ai_tensor_create(context, &labels_desc, labels_values, sizeof(labels_values), &error);
    oasp_ai_tensor *onednn_logits = to_onednn(cpu_logits, &error);
    oasp_ai_tensor *onednn_labels = to_onednn(cpu_labels, &error);
    assert(oasp_ai_tensor_requires_grad(cpu_logits, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_logits, 1, &error) == 0);
    oasp_ai_tensor *cpu_loss = oasp_ai_cross_entropy(context, cpu_logits, cpu_labels, &error);
    oasp_ai_tensor *onednn_loss = oasp_ai_cross_entropy(context, onednn_logits, onednn_labels, &error);
    assert(cpu_loss && onednn_loss);
    assert(oasp_ai_tensor_backward(cpu_loss, NULL, 0, &error) == 0);
    if (oasp_ai_tensor_backward(onednn_loss, NULL, 0, &error)) {
        report_error("cross entropy", &error);
        abort();
    }
    float loss_difference = maximum_difference(cpu_loss, onednn_loss, 1, &error);
    float loss_gradient_difference =
        maximum_difference(oasp_ai_tensor_gradient(cpu_logits), oasp_ai_tensor_gradient(onednn_logits), 20, &error);
    assert(loss_difference < 1e-5f && loss_gradient_difference < 1e-5f);

    float class_weight_values[] = {1, 2, 0.5f, 1.5f, 3};
    int64_t extended_label_values[] = {0, -100, 1, 4};
    oasp_ai_tensor_desc class_weight_desc = {
        .dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {5},
    };
    oasp_ai_tensor *cpu_extended_logits =
        oasp_ai_tensor_create(context, &logits_desc, logits_values, sizeof(logits_values), &error);
    oasp_ai_tensor *cpu_extended_labels =
        oasp_ai_tensor_create(context, &labels_desc, extended_label_values,
                              sizeof(extended_label_values), &error);
    oasp_ai_tensor *cpu_class_weight =
        oasp_ai_tensor_create(context, &class_weight_desc, class_weight_values,
                              sizeof(class_weight_values), &error);
    oasp_ai_tensor *onednn_extended_logits = to_onednn(cpu_extended_logits, &error);
    oasp_ai_tensor *onednn_extended_labels = to_onednn(cpu_extended_labels, &error);
    oasp_ai_tensor *onednn_class_weight = to_onednn(cpu_class_weight, &error);
    assert(oasp_ai_tensor_requires_grad(cpu_extended_logits, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_extended_logits, 1, &error) == 0);
    oasp_ai_tensor *cpu_extended_loss = oasp_ai_cross_entropy_ex(
        context, cpu_extended_logits, cpu_extended_labels, cpu_class_weight, -100,
        0.2, OASP_AI_REDUCTION_MEAN, &error);
    oasp_ai_tensor *onednn_extended_loss = oasp_ai_cross_entropy_ex(
        context, onednn_extended_logits, onednn_extended_labels, onednn_class_weight,
        -100, 0.2, OASP_AI_REDUCTION_MEAN, &error);
    assert(cpu_extended_loss && onednn_extended_loss);
    assert(oasp_ai_tensor_backward(cpu_extended_loss, NULL, 0, &error) == 0);
    assert(oasp_ai_tensor_backward(onednn_extended_loss, NULL, 0, &error) == 0);
    float extended_loss_difference =
        maximum_difference(cpu_extended_loss, onednn_extended_loss, 1, &error);
    float extended_gradient_difference = maximum_difference(
        oasp_ai_tensor_gradient(cpu_extended_logits),
        oasp_ai_tensor_gradient(onednn_extended_logits), 20, &error);
    assert(extended_loss_difference < 1e-5f &&
           extended_gradient_difference < 1e-5f);

    oasp_ai_tensor_desc ctc_log_desc = {
        .dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {2, 1, 2},
    };
    oasp_ai_tensor_desc ctc_target_desc = {
        .dtype = OASP_AI_DTYPE_I64, .rank = 2, .shape = {1, 1},
    };
    oasp_ai_tensor_desc ctc_length_desc = {
        .dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {1},
    };
    float ctc_log_values[] = {
        (float)log(0.6), (float)log(0.4), (float)log(0.5), (float)log(0.5),
    };
    int64_t ctc_target_values[] = {1};
    int64_t ctc_input_length_values[] = {2};
    int64_t ctc_target_length_values[] = {1};
    oasp_ai_tensor *cpu_ctc_log = oasp_ai_tensor_create(
        context, &ctc_log_desc, ctc_log_values, sizeof(ctc_log_values), &error);
    oasp_ai_tensor *cpu_ctc_target = oasp_ai_tensor_create(
        context, &ctc_target_desc, ctc_target_values, sizeof(ctc_target_values), &error);
    oasp_ai_tensor *cpu_ctc_input_length = oasp_ai_tensor_create(
        context, &ctc_length_desc, ctc_input_length_values,
        sizeof(ctc_input_length_values), &error);
    oasp_ai_tensor *cpu_ctc_target_length = oasp_ai_tensor_create(
        context, &ctc_length_desc, ctc_target_length_values,
        sizeof(ctc_target_length_values), &error);
    oasp_ai_tensor *onednn_ctc_log = to_onednn(cpu_ctc_log, &error);
    oasp_ai_tensor *onednn_ctc_target = to_onednn(cpu_ctc_target, &error);
    oasp_ai_tensor *onednn_ctc_input_length = to_onednn(cpu_ctc_input_length, &error);
    oasp_ai_tensor *onednn_ctc_target_length = to_onednn(cpu_ctc_target_length, &error);
    assert(oasp_ai_tensor_requires_grad(cpu_ctc_log, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_ctc_log, 1, &error) == 0);
    oasp_ai_tensor *cpu_ctc = oasp_ai_ctc_loss(
        context, cpu_ctc_log, cpu_ctc_target, cpu_ctc_input_length,
        cpu_ctc_target_length, 0, 0, OASP_AI_REDUCTION_MEAN, &error);
    oasp_ai_tensor *onednn_ctc = oasp_ai_ctc_loss(
        context, onednn_ctc_log, onednn_ctc_target, onednn_ctc_input_length,
        onednn_ctc_target_length, 0, 0, OASP_AI_REDUCTION_MEAN, &error);
    assert(cpu_ctc && onednn_ctc);
    assert(oasp_ai_tensor_backward(cpu_ctc, NULL, 0, &error) == 0);
    assert(oasp_ai_tensor_backward(onednn_ctc, NULL, 0, &error) == 0);
    float ctc_loss_difference = maximum_difference(cpu_ctc, onednn_ctc, 1, &error);
    float ctc_gradient_difference = maximum_difference(
        oasp_ai_tensor_gradient(cpu_ctc_log),
        oasp_ai_tensor_gradient(onednn_ctc_log), 4, &error);
    assert(ctc_loss_difference < 1e-5f && ctc_gradient_difference < 1e-5f);

    oasp_ai_tensor *cpu_nll_input =
        oasp_ai_tensor_create(context, &logits_desc, logits_values, sizeof(logits_values), &error);
    oasp_ai_tensor *onednn_nll_input = to_onednn(cpu_nll_input, &error);
    assert(oasp_ai_tensor_requires_grad(cpu_nll_input, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(onednn_nll_input, 1, &error) == 0);
    oasp_ai_tensor *cpu_nll =
        oasp_ai_nll_loss(context, cpu_nll_input, cpu_labels, OASP_AI_REDUCTION_MEAN, &error);
    oasp_ai_tensor *onednn_nll =
        oasp_ai_nll_loss(context, onednn_nll_input, onednn_labels, OASP_AI_REDUCTION_MEAN, &error);
    assert(cpu_nll && onednn_nll && oasp_ai_tensor_backward(cpu_nll, NULL, 0, &error) == 0);
    if (oasp_ai_tensor_backward(onednn_nll, NULL, 0, &error)) {
        report_error("nll", &error);
        abort();
    }
    float nll_difference = maximum_difference(cpu_nll, onednn_nll, 1, &error);
    float nll_gradient_difference = maximum_difference(
        oasp_ai_tensor_gradient(cpu_nll_input), oasp_ai_tensor_gradient(onednn_nll_input), 20, &error);
    assert(nll_difference < 1e-5f && nll_gradient_difference < 1e-5f);

    printf("openasp-ai-cpu-onednn: matmul=%g attention=[%g,%g,%g,%g] "
           "layer_norm=[%g,%g,%g,%g] cross_entropy=[%g,%g] "
           "extended_cross_entropy=[%g,%g] ctc=[%g,%g] nll=[%g,%g] parity=passed\n",
           matmul_difference, attention_differences[0], attention_differences[1],
           attention_differences[2], attention_differences[3], norm_differences[0],
           norm_differences[1], norm_differences[2], norm_differences[3],
           loss_difference, loss_gradient_difference, extended_loss_difference,
           extended_gradient_difference, ctc_loss_difference, ctc_gradient_difference,
           nll_difference, nll_gradient_difference);

    oasp_ai_tensor *tensors[] = {
        onednn_ctc, cpu_ctc, onednn_ctc_target_length, onednn_ctc_input_length,
        onednn_ctc_target, onednn_ctc_log, cpu_ctc_target_length,
        cpu_ctc_input_length, cpu_ctc_target, cpu_ctc_log,
        onednn_extended_loss, cpu_extended_loss, onednn_class_weight,
        onednn_extended_labels, onednn_extended_logits, cpu_class_weight,
        cpu_extended_labels, cpu_extended_logits,
        onednn_nll, cpu_nll, onednn_nll_input, cpu_nll_input,
        onednn_loss, cpu_loss, onednn_labels, onednn_logits, cpu_labels, cpu_logits,
        onednn_norm, cpu_norm, onednn_norm_upstream, onednn_norm_bias, onednn_norm_weight,
        cpu_norm_upstream, cpu_norm_bias, cpu_norm_weight, onednn_attention_loss,
        onednn_attention, cpu_attention_loss, cpu_attention, onednn_value, onednn_key,
        onednn_query, cpu_value, cpu_key, cpu_query, onednn_product, cpu_product,
        onednn_right, onednn_left, cpu_right, cpu_left,
    };
    for (size_t index = 0; index < sizeof(tensors) / sizeof(tensors[0]); index++)
        oasp_ai_tensor_release(tensors[index]);
    oasp_ai_context_destroy(context);
    free(norm_upstream_values);
    free(value_values);
    free(key_values);
    free(query_values);
    return 0;
}
