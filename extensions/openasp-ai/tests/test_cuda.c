// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void near_value(float actual, float expected) {
    if (!(fabsf(actual - expected) < 1e-4f))
        fprintf(stderr, "CUDA result mismatch: actual=%g expected=%g\n", actual, expected);
    assert(fabsf(actual - expected) < 1e-4f);
}

static void copy_f32(oasp_ai_tensor *tensor, float *values, size_t count, oasp_ai_error *error) {
    int status = oasp_ai_tensor_copy_to_host(tensor, values, count * sizeof(*values), error);
    if (status)
        fprintf(stderr, "CUDA copy failed: operation=%s message=%s suggestion=%s\n",
                error->operation ? error->operation : "", error->message ? error->message : "",
                error->suggestion ? error->suggestion : "");
    assert(status == 0);
}

static void test_losses(oasp_ai_context *context, oasp_ai_error *error) {
    oasp_ai_tensor_desc vector_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {2}};
    float input_values[] = {0.2f, 0.8f};
    float target_values[] = {0, 1};
    oasp_ai_tensor *host_input = oasp_ai_tensor_create(
        context, &vector_desc, input_values, sizeof(input_values), error);
    oasp_ai_tensor *host_target = oasp_ai_tensor_create(
        context, &vector_desc, target_values, sizeof(target_values), error);
    oasp_ai_tensor *input = oasp_ai_tensor_to_device(host_input, "cuda:0", error);
    oasp_ai_tensor *target = oasp_ai_tensor_to_device(host_target, "cuda:0", error);
    assert(input && target);
    oasp_ai_tensor *losses[] = {
        oasp_ai_mse_loss(context, input, target, OASP_AI_REDUCTION_MEAN, error),
        oasp_ai_l1_loss(context, input, target, OASP_AI_REDUCTION_MEAN, error),
        oasp_ai_bce_loss(context, input, target, OASP_AI_REDUCTION_MEAN, error),
    };
    float value;
    assert(losses[0] && losses[1] && losses[2]);
    copy_f32(losses[0], &value, 1, error);
    near_value(value, 0.04f);
    copy_f32(losses[1], &value, 1, error);
    near_value(value, 0.2f);
    copy_f32(losses[2], &value, 1, error);
    near_value(value, (float)-log(0.8));

    float logits_values[] = {0, 2};
    oasp_ai_tensor *host_logits = oasp_ai_tensor_create(
        context, &vector_desc, logits_values, sizeof(logits_values), error);
    oasp_ai_tensor *logits = oasp_ai_tensor_to_device(host_logits, "cuda:0", error);
    oasp_ai_tensor *bce_logits = oasp_ai_bce_with_logits_loss(
        context, logits, target, OASP_AI_REDUCTION_MEAN, error);
    assert(bce_logits);
    copy_f32(bce_logits, &value, 1, error);
    near_value(value, (float)((log(2.0) + log1p(exp(-2.0))) / 2.0));

    float log_probability_values[] = {(float)log(0.4), (float)log(0.6)};
    float distribution_values[] = {0.5f, 0.5f};
    oasp_ai_tensor *host_log_probability = oasp_ai_tensor_create(
        context, &vector_desc, log_probability_values, sizeof(log_probability_values), error);
    oasp_ai_tensor *host_distribution = oasp_ai_tensor_create(
        context, &vector_desc, distribution_values, sizeof(distribution_values), error);
    oasp_ai_tensor *log_probability = oasp_ai_tensor_to_device(
        host_log_probability, "cuda:0", error);
    oasp_ai_tensor *distribution = oasp_ai_tensor_to_device(
        host_distribution, "cuda:0", error);
    oasp_ai_tensor *kl = oasp_ai_kl_div_loss(
        context, log_probability, distribution, OASP_AI_REDUCTION_SUM, 0, error);
    assert(kl);
    copy_f32(kl, &value, 1, error);
    near_value(value, (float)(0.5 * log(0.5 / 0.4) + 0.5 * log(0.5 / 0.6)));

    float huber_values[] = {0, 3};
    float zero_values[] = {0, 0};
    oasp_ai_tensor *host_huber_input = oasp_ai_tensor_create(
        context, &vector_desc, huber_values, sizeof(huber_values), error);
    oasp_ai_tensor *host_zeros = oasp_ai_tensor_create(
        context, &vector_desc, zero_values, sizeof(zero_values), error);
    oasp_ai_tensor *huber_input = oasp_ai_tensor_to_device(
        host_huber_input, "cuda:0", error);
    oasp_ai_tensor *zeros = oasp_ai_tensor_to_device(host_zeros, "cuda:0", error);
    oasp_ai_tensor *huber = oasp_ai_huber_loss(
        context, huber_input, zeros, 1, OASP_AI_REDUCTION_MEAN, error);
    assert(huber);
    copy_f32(huber, &value, 1, error);
    near_value(value, 1.25f);

    oasp_ai_tensor_desc matrix_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    oasp_ai_tensor_desc label_desc = {.dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {2}};
    float nll_values[] = {-0.1f, -2, -3, -0.2f};
    int64_t label_values[] = {0, 1};
    oasp_ai_tensor *host_nll_input = oasp_ai_tensor_create(
        context, &matrix_desc, nll_values, sizeof(nll_values), error);
    oasp_ai_tensor *host_labels = oasp_ai_tensor_create(
        context, &label_desc, label_values, sizeof(label_values), error);
    oasp_ai_tensor *nll_input = oasp_ai_tensor_to_device(host_nll_input, "cuda:0", error);
    oasp_ai_tensor *labels = oasp_ai_tensor_to_device(host_labels, "cuda:0", error);
    assert(nll_input && labels && oasp_ai_tensor_requires_grad(nll_input, 1, error) == 0);
    oasp_ai_tensor *nll = oasp_ai_nll_loss(
        context, nll_input, labels, OASP_AI_REDUCTION_MEAN, error);
    assert(nll);
    copy_f32(nll, &value, 1, error);
    near_value(value, 0.15f);
    assert(oasp_ai_tensor_backward(nll, NULL, 0, error) == 0);
    oasp_ai_tensor *gradient = oasp_ai_tensor_gradient(nll_input);
    float gradient_values[4];
    assert(gradient);
    copy_f32(gradient, gradient_values, 4, error);
    near_value(gradient_values[0], -0.5f);
    near_value(gradient_values[1], 0);
    near_value(gradient_values[2], 0);
    near_value(gradient_values[3], -0.5f);

    float positive_weight_values[] = {1, 3};
    oasp_ai_tensor *host_positive_weight = oasp_ai_tensor_create(
        context, &vector_desc, positive_weight_values, sizeof(positive_weight_values), error);
    oasp_ai_tensor *positive_weight =
        oasp_ai_tensor_to_device(host_positive_weight, "cuda:0", error);
    oasp_ai_tensor *weighted_bce = oasp_ai_bce_with_logits_loss_ex(
        context, logits, target, positive_weight, OASP_AI_REDUCTION_MEAN, error);
    assert(weighted_bce);
    copy_f32(weighted_bce, &value, 1, error);
    near_value(value, (float)((log(2.0) + 3.0 * log1p(exp(-2.0))) / 2.0));

    oasp_ai_tensor *smooth_l1 = oasp_ai_smooth_l1_loss(
        context, huber_input, zeros, 1, OASP_AI_REDUCTION_MEAN, error);
    oasp_ai_tensor *focal = oasp_ai_focal_loss(
        context, logits, target, 0.25, 2, OASP_AI_REDUCTION_MEAN, error);
    oasp_ai_tensor *dice = oasp_ai_dice_loss(
        context, input, target, 1, 1e-7, 0, OASP_AI_REDUCTION_MEAN, error);
    assert(smooth_l1 && focal && dice);
    copy_f32(smooth_l1, &value, 1, error);
    near_value(value, 1.25f);
    copy_f32(dice, &value, 1, error);
    near_value(value, (float)(1.0 - 2.6 / 3.0000001));

    float feature_values[] = {1, 0, 0, 1};
    oasp_ai_tensor *host_features = oasp_ai_tensor_create(
        context, &matrix_desc, feature_values, sizeof(feature_values), error);
    oasp_ai_tensor *features = oasp_ai_tensor_to_device(host_features, "cuda:0", error);
    oasp_ai_tensor *info_nce = oasp_ai_info_nce_loss(
        context, features, features, 1, 1, OASP_AI_REDUCTION_MEAN, error);
    assert(info_nce);
    copy_f32(info_nce, &value, 1, error);
    near_value(value, (float)log1p(exp(-1.0)));

    float ce_logits_values[] = {1, 0, -1, 0.5f, 1.5f, -0.5f};
    int64_t ce_label_values[] = {0, -100};
    float ce_weight_values[] = {1, 2, 3};
    oasp_ai_tensor_desc ce_logits_desc = {
        .dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 3},
    };
    oasp_ai_tensor_desc ce_label_desc = {
        .dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {2},
    };
    oasp_ai_tensor_desc ce_weight_desc = {
        .dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {3},
    };
    oasp_ai_tensor *host_ce_logits = oasp_ai_tensor_create(
        context, &ce_logits_desc, ce_logits_values, sizeof(ce_logits_values), error);
    oasp_ai_tensor *host_ce_labels = oasp_ai_tensor_create(
        context, &ce_label_desc, ce_label_values, sizeof(ce_label_values), error);
    oasp_ai_tensor *host_ce_weight = oasp_ai_tensor_create(
        context, &ce_weight_desc, ce_weight_values, sizeof(ce_weight_values), error);
    oasp_ai_tensor *ce_logits = oasp_ai_tensor_to_device(host_ce_logits, "cuda:0", error);
    oasp_ai_tensor *ce_labels = oasp_ai_tensor_to_device(host_ce_labels, "cuda:0", error);
    oasp_ai_tensor *ce_weight = oasp_ai_tensor_to_device(host_ce_weight, "cuda:0", error);
    assert(ce_logits && ce_labels && ce_weight &&
           oasp_ai_tensor_requires_grad(ce_logits, 1, error) == 0);
    oasp_ai_tensor *extended_ce = oasp_ai_cross_entropy_ex(
        context, ce_logits, ce_labels, ce_weight, -100, 0.1,
        OASP_AI_REDUCTION_MEAN, error);
    assert(extended_ce && oasp_ai_tensor_backward(extended_ce, NULL, 0, error) == 0);

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
    oasp_ai_tensor *host_ctc_log = oasp_ai_tensor_create(
        context, &ctc_log_desc, ctc_log_values, sizeof(ctc_log_values), error);
    oasp_ai_tensor *host_ctc_target = oasp_ai_tensor_create(
        context, &ctc_target_desc, ctc_target_values, sizeof(ctc_target_values), error);
    oasp_ai_tensor *host_ctc_input_length = oasp_ai_tensor_create(
        context, &ctc_length_desc, ctc_input_length_values,
        sizeof(ctc_input_length_values), error);
    oasp_ai_tensor *host_ctc_target_length = oasp_ai_tensor_create(
        context, &ctc_length_desc, ctc_target_length_values,
        sizeof(ctc_target_length_values), error);
    oasp_ai_tensor *ctc_log = oasp_ai_tensor_to_device(host_ctc_log, "cuda:0", error);
    oasp_ai_tensor *ctc_target = oasp_ai_tensor_to_device(host_ctc_target, "cuda:0", error);
    oasp_ai_tensor *ctc_input_length =
        oasp_ai_tensor_to_device(host_ctc_input_length, "cuda:0", error);
    oasp_ai_tensor *ctc_target_length =
        oasp_ai_tensor_to_device(host_ctc_target_length, "cuda:0", error);
    assert(ctc_log && ctc_target && ctc_input_length && ctc_target_length &&
           oasp_ai_tensor_requires_grad(ctc_log, 1, error) == 0);
    oasp_ai_tensor *ctc = oasp_ai_ctc_loss(
        context, ctc_log, ctc_target, ctc_input_length, ctc_target_length, 0, 0,
        OASP_AI_REDUCTION_MEAN, error);
    assert(ctc);
    copy_f32(ctc, &value, 1, error);
    near_value(value, (float)-log(0.7));
    assert(oasp_ai_tensor_backward(ctc, NULL, 0, error) == 0);
    float ctc_gradient_values[4];
    oasp_ai_tensor *ctc_gradient = oasp_ai_tensor_gradient(ctc_log);
    assert(ctc_gradient);
    copy_f32(ctc_gradient, ctc_gradient_values, 4, error);
    near_value(ctc_gradient_values[0], 0.6f - 3.0f / 7.0f);
    near_value(ctc_gradient_values[1], 0.4f - 4.0f / 7.0f);
    near_value(ctc_gradient_values[2], 0.5f - 2.0f / 7.0f);
    near_value(ctc_gradient_values[3], 0.5f - 5.0f / 7.0f);

    oasp_ai_tensor_release(ctc_gradient);
    oasp_ai_tensor_release(ctc);
    oasp_ai_tensor_release(ctc_target_length);
    oasp_ai_tensor_release(ctc_input_length);
    oasp_ai_tensor_release(ctc_target);
    oasp_ai_tensor_release(ctc_log);
    oasp_ai_tensor_release(host_ctc_target_length);
    oasp_ai_tensor_release(host_ctc_input_length);
    oasp_ai_tensor_release(host_ctc_target);
    oasp_ai_tensor_release(host_ctc_log);
    oasp_ai_tensor_release(extended_ce);
    oasp_ai_tensor_release(ce_weight);
    oasp_ai_tensor_release(ce_labels);
    oasp_ai_tensor_release(ce_logits);
    oasp_ai_tensor_release(host_ce_weight);
    oasp_ai_tensor_release(host_ce_labels);
    oasp_ai_tensor_release(host_ce_logits);
    oasp_ai_tensor_release(info_nce);
    oasp_ai_tensor_release(features);
    oasp_ai_tensor_release(host_features);
    oasp_ai_tensor_release(dice);
    oasp_ai_tensor_release(focal);
    oasp_ai_tensor_release(smooth_l1);
    oasp_ai_tensor_release(weighted_bce);
    oasp_ai_tensor_release(positive_weight);
    oasp_ai_tensor_release(host_positive_weight);
    oasp_ai_tensor_release(gradient);
    oasp_ai_tensor_release(nll);
    oasp_ai_tensor_release(labels);
    oasp_ai_tensor_release(nll_input);
    oasp_ai_tensor_release(host_labels);
    oasp_ai_tensor_release(host_nll_input);
    oasp_ai_tensor_release(huber);
    oasp_ai_tensor_release(zeros);
    oasp_ai_tensor_release(huber_input);
    oasp_ai_tensor_release(host_zeros);
    oasp_ai_tensor_release(host_huber_input);
    oasp_ai_tensor_release(kl);
    oasp_ai_tensor_release(distribution);
    oasp_ai_tensor_release(log_probability);
    oasp_ai_tensor_release(host_distribution);
    oasp_ai_tensor_release(host_log_probability);
    oasp_ai_tensor_release(bce_logits);
    oasp_ai_tensor_release(logits);
    oasp_ai_tensor_release(host_logits);
    for (size_t index = 0; index < sizeof(losses) / sizeof(losses[0]); index++)
        oasp_ai_tensor_release(losses[index]);
    oasp_ai_tensor_release(target);
    oasp_ai_tensor_release(input);
    oasp_ai_tensor_release(host_target);
    oasp_ai_tensor_release(host_input);
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
    assert(oasp_ai_context_load_backend(context, argv[1], NULL, &error) == 0);
    assert(oasp_ai_context_device_count(context) >= 2);
    test_losses(context, &error);

    oasp_ai_tensor_desc matrix = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    float left_values[] = {1, 2, 3, 4};
    float right_values[] = {4, 3, 2, 1};
    oasp_ai_tensor *host_left = oasp_ai_tensor_create(context, &matrix, left_values, sizeof(left_values), &error);
    oasp_ai_tensor *host_right = oasp_ai_tensor_create(context, &matrix, right_values, sizeof(right_values), &error);
    oasp_ai_tensor *left = oasp_ai_tensor_to_device(host_left, "cuda:0", &error);
    oasp_ai_tensor *right = oasp_ai_tensor_to_device(host_right, "cuda:0", &error);
    oasp_ai_tensor *sum = oasp_ai_binary(context, OASP_AI_OP_ADD, left, right, &error);
    float result_values[4];
    assert(left && right && sum);
    copy_f32(sum, result_values, 4, &error);
    fprintf(stderr, "eager add: %g %g %g %g\n", result_values[0], result_values[1], result_values[2], result_values[3]);
    for (size_t index = 0; index < 4; index++)
        near_value(result_values[index], 5);

    oasp_ai_tensor_desc fp16_left_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {1, 2}};
    oasp_ai_tensor_desc fp16_weight_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {1, 2}};
    float fp16_left_values[] = {0.1f, 0.2f};
    float fp16_weight_values[] = {0.3f, 0.4f};
    oasp_ai_tensor *fp16_host_left =
        oasp_ai_tensor_create(context, &fp16_left_desc, fp16_left_values, sizeof(fp16_left_values), &error);
    oasp_ai_tensor *fp16_host_weight =
        oasp_ai_tensor_create(context, &fp16_weight_desc, fp16_weight_values, sizeof(fp16_weight_values), &error);
    oasp_ai_tensor *fp16_left = oasp_ai_tensor_to_device(fp16_host_left, "cuda:0", &error);
    oasp_ai_tensor *fp16_weight = oasp_ai_tensor_to_device(fp16_host_weight, "cuda:0", &error);
    oasp_ai_tensor *fp16_weight_transposed = oasp_ai_tensor_transpose(fp16_weight, 0, 1, &error);
    oasp_ai_tensor *fp16_product = oasp_ai_matmul(context, fp16_left, fp16_weight_transposed, &error);
    float fp16_product_value = 0;
    assert(fp16_product);
    copy_f32(fp16_product, &fp16_product_value, 1, &error);
    near_value(fp16_product_value, 0.11f);
    if (getenv("OPENASP_AI_CUDA_FP16"))
        assert(fabsf(fp16_product_value - 0.11f) > 1e-6f);
    oasp_ai_tensor_release(fp16_product);
    oasp_ai_tensor_release(fp16_weight_transposed);
    oasp_ai_tensor_release(fp16_weight);
    oasp_ai_tensor_release(fp16_left);
    oasp_ai_tensor_release(fp16_host_weight);
    oasp_ai_tensor_release(fp16_host_left);

    float reuse_values[] = {-1, 2, -3, 4};
    float overwrite_values[] = {9, 9, 9, 9};
    oasp_ai_tensor *reuse_host = oasp_ai_tensor_create(context, &matrix, reuse_values, sizeof(reuse_values), &error);
    oasp_ai_tensor *overwrite_host =
        oasp_ai_tensor_create(context, &matrix, overwrite_values, sizeof(overwrite_values), &error);
    assert(reuse_host && overwrite_host);
    for (size_t iteration = 0; iteration < 128; iteration++) {
        oasp_ai_tensor *reuse_input = oasp_ai_tensor_to_device(reuse_host, "cuda:0", &error);
        oasp_ai_tensor *reuse_output = oasp_ai_unary(context, OASP_AI_OP_RELU, reuse_input, 0, &error);
        assert(reuse_input && reuse_output);
        oasp_ai_tensor_release(reuse_input);
        oasp_ai_tensor *overwrite = oasp_ai_tensor_to_device(overwrite_host, "cuda:0", &error);
        assert(overwrite);
        copy_f32(reuse_output, result_values, 4, &error);
        near_value(result_values[0], 0);
        near_value(result_values[1], 2);
        near_value(result_values[2], 0);
        near_value(result_values[3], 4);
        oasp_ai_tensor_release(overwrite);
        oasp_ai_tensor_release(reuse_output);
    }
    oasp_ai_tensor_release(overwrite_host);
    oasp_ai_tensor_release(reuse_host);

    oasp_ai_graph *graph = oasp_ai_graph_create(context, "cuda-add", &error);
    uint32_t first = oasp_ai_graph_input(graph, "first", &matrix, &error);
    uint32_t second = oasp_ai_graph_input(graph, "second", &matrix, &error);
    uint32_t graph_inputs[] = {first, second};
    uint32_t graph_output = oasp_ai_graph_operation(graph, OASP_AI_OP_ADD, graph_inputs, 2, NULL, 0, &error);
    assert(graph_output && oasp_ai_graph_set_outputs(graph, &graph_output, 1, &error) == 0);
    oasp_ai_plan *plan = oasp_ai_graph_compile(graph, "cuda:0", &error);
    oasp_ai_tensor *plan_inputs[] = {left, right};
    oasp_ai_tensor **plan_outputs = NULL;
    size_t output_count = 0;
    assert(plan && oasp_ai_plan_execute(plan, plan_inputs, 2, &plan_outputs, &output_count, &error) == 0);
    assert(output_count == 1 && oasp_ai_tensor_const_data(plan_outputs[0]) == NULL);
    copy_f32(plan_outputs[0], result_values, 4, &error);
    fprintf(stderr, "graph add: %g %g %g %g\n", result_values[0], result_values[1], result_values[2], result_values[3]);
    for (size_t index = 0; index < 4; index++)
        near_value(result_values[index], 5);
    oasp_ai_tensor_release(plan_outputs[0]);
    free(plan_outputs);
    plan_outputs = NULL;
    output_count = 0;
    assert(oasp_ai_plan_execute(plan, plan_inputs, 2, &plan_outputs, &output_count, &error) == 0);
    assert(output_count == 1);
    copy_f32(plan_outputs[0], result_values, 4, &error);
    for (size_t index = 0; index < 4; index++)
        near_value(result_values[index], 5);

    oasp_ai_graph *matmul_graph = oasp_ai_graph_create(context, "cuda-matmul", &error);
    uint32_t matmul_left = oasp_ai_graph_input(matmul_graph, "left", &matrix, &error);
    uint32_t matmul_right = oasp_ai_graph_input(matmul_graph, "right", &matrix, &error);
    uint32_t matmul_inputs[] = {matmul_left, matmul_right};
    uint32_t matmul_output =
        oasp_ai_graph_operation(matmul_graph, OASP_AI_OP_MATMUL, matmul_inputs, 2, NULL, 0, &error);
    assert(matmul_output && oasp_ai_graph_set_outputs(matmul_graph, &matmul_output, 1, &error) == 0);
    oasp_ai_plan *matmul_plan = oasp_ai_graph_compile(matmul_graph, "cuda:0", &error);
    float matmul_expected[] = {8, 5, 20, 13};
    for (unsigned iteration = 0; iteration < 2; iteration++) {
        oasp_ai_tensor **matmul_outputs = NULL;
        size_t matmul_output_count = 0;
        assert(matmul_plan &&
               oasp_ai_plan_execute(matmul_plan, plan_inputs, 2, &matmul_outputs,
                                    &matmul_output_count, &error) == 0);
        assert(matmul_output_count == 1);
        copy_f32(matmul_outputs[0], result_values, 4, &error);
        for (size_t index = 0; index < 4; index++)
            near_value(result_values[index], matmul_expected[index]);
        oasp_ai_tensor_release(matmul_outputs[0]);
        free(matmul_outputs);
    }
    oasp_ai_plan_release(matmul_plan);
    oasp_ai_graph_release(matmul_graph);

    oasp_ai_tensor_desc input_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    oasp_ai_tensor_desc weight_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 1}};
    float input_values[] = {1, 2, 3, 4};
    float weight_values[] = {1, 2};
    oasp_ai_tensor *host_input = oasp_ai_tensor_create(context, &input_desc, input_values, sizeof(input_values), &error);
    oasp_ai_tensor *host_weight = oasp_ai_tensor_create(context, &weight_desc, weight_values, sizeof(weight_values), &error);
    oasp_ai_tensor *input = oasp_ai_tensor_to_device(host_input, "cuda:0", &error);
    oasp_ai_tensor *weight = oasp_ai_tensor_to_device(host_weight, "cuda:0", &error);
    assert(input && weight && oasp_ai_tensor_requires_grad(weight, 1, &error) == 0);
    oasp_ai_module *module = oasp_ai_module_create(context, "CudaLinear", &error);
    assert(module && oasp_ai_module_register_parameter(module, "weight", weight, &error) == 0);
    oasp_ai_optimizer *optimizer = oasp_ai_optimizer_create(module, OASP_AI_OPTIMIZER_SGD, 0.1, 0, &error);
    assert(optimizer);
    uint64_t d2h_before_training = oasp_ai_context_metrics(context)->device_to_host_bytes;
    uint64_t sync_before_training = oasp_ai_context_metrics(context)->synchronization_count;
    for (unsigned step = 0; step < 8; step++) {
        oasp_ai_tensor *prediction = oasp_ai_matmul(context, input, weight, &error);
        oasp_ai_tensor *loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, prediction, -1, 0, &error);
        assert(prediction && loss && oasp_ai_tensor_backward(loss, NULL, 0, &error) == 0);
        assert(oasp_ai_optimizer_step(optimizer, &error) == 0);
        oasp_ai_tensor_release(loss);
        oasp_ai_tensor_release(prediction);
        assert(oasp_ai_optimizer_zero_grad(optimizer, &error) == 0);
    }
    assert(oasp_ai_context_metrics(context)->device_to_host_bytes == d2h_before_training);
    assert(oasp_ai_context_metrics(context)->synchronization_count == sync_before_training);
    float trained[2];
    copy_f32(weight, trained, 2, &error);
    near_value(trained[0], -2.2f);
    near_value(trained[1], -2.8f);
    oasp_ai_tensor *single_rank[] = {weight};
    assert(oasp_ai_tensor_all_reduce(single_rank, 1, OASP_AI_COLLECTIVE_MEAN, &error) == 0);

    assert(oasp_ai_tensor_requires_grad(weight, 1, &error) == 0);
    oasp_ai_optimizer *adamw = oasp_ai_optimizer_create(module, OASP_AI_OPTIMIZER_ADAMW, 0.01, 0.01, &error);
    uint64_t adamw_d2h = oasp_ai_context_metrics(context)->device_to_host_bytes;
    oasp_ai_tensor *adam_prediction = oasp_ai_matmul(context, input, weight, &error);
    oasp_ai_tensor *adam_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, adam_prediction, -1, 0, &error);
    assert(adamw && adam_prediction && adam_loss && oasp_ai_tensor_backward(adam_loss, NULL, 0, &error) == 0);
    assert(oasp_ai_optimizer_step(adamw, &error) == 0);
    assert(oasp_ai_context_metrics(context)->device_to_host_bytes == adamw_d2h);
    oasp_ai_optimizer_zero_grad(adamw, &error);
    oasp_ai_tensor_release(adam_loss);
    oasp_ai_tensor_release(adam_prediction);

    oasp_ai_tensor_desc scalar_matrix = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {1, 1}};
    float scaler_input_value = 2;
    float scaler_weight_value = 1;
    oasp_ai_tensor *scaler_host_input =
        oasp_ai_tensor_create(context, &scalar_matrix, &scaler_input_value, sizeof(scaler_input_value), &error);
    oasp_ai_tensor *scaler_host_weight =
        oasp_ai_tensor_create(context, &scalar_matrix, &scaler_weight_value, sizeof(scaler_weight_value), &error);
    oasp_ai_tensor *scaler_input = oasp_ai_tensor_to_device(scaler_host_input, "cuda:0", &error);
    oasp_ai_tensor *scaler_weight = oasp_ai_tensor_to_device(scaler_host_weight, "cuda:0", &error);
    oasp_ai_module *scaler_module = oasp_ai_module_create(context, "CudaLossScaler", &error);
    assert(scaler_input && scaler_weight && scaler_module);
    assert(oasp_ai_tensor_requires_grad(scaler_weight, 1, &error) == 0);
    assert(oasp_ai_module_register_parameter(scaler_module, "weight", scaler_weight, &error) == 0);
    oasp_ai_optimizer *scaler_optimizer =
        oasp_ai_optimizer_create(scaler_module, OASP_AI_OPTIMIZER_SGD, 0.1, 0, &error);
    oasp_ai_loss_scaler *loss_scaler = oasp_ai_loss_scaler_create(8, 2, 0.5, 100, &error);
    oasp_ai_tensor *scaler_prediction = oasp_ai_matmul(context, scaler_input, scaler_weight, &error);
    oasp_ai_tensor *scaler_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, scaler_prediction, -1, 0, &error);
    oasp_ai_tensor *scaled_loss = oasp_ai_loss_scaler_scale_loss(loss_scaler, scaler_loss, &error);
    assert(scaler_optimizer && loss_scaler && scaler_prediction && scaler_loss && scaled_loss);
    assert(oasp_ai_tensor_backward(scaled_loss, NULL, 0, &error) == 0);
    uint64_t scaler_d2h = oasp_ai_context_metrics(context)->device_to_host_bytes;
    assert(oasp_ai_loss_scaler_step(loss_scaler, scaler_optimizer, &error) == 0);
    assert(oasp_ai_context_metrics(context)->device_to_host_bytes == scaler_d2h + sizeof(float));
    float scaled_weight_result = 0;
    copy_f32(scaler_weight, &scaled_weight_result, 1, &error);
    near_value(scaled_weight_result, 0.8f);
    oasp_ai_tensor_release(scaled_loss);
    oasp_ai_tensor_release(scaler_loss);
    oasp_ai_tensor_release(scaler_prediction);
    assert(oasp_ai_optimizer_zero_grad(scaler_optimizer, &error) == 0);

    loss_scaler->scale = FLT_MAX;
    scaler_prediction = oasp_ai_matmul(context, scaler_input, scaler_weight, &error);
    scaler_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, scaler_prediction, -1, 0, &error);
    scaled_loss = oasp_ai_loss_scaler_scale_loss(loss_scaler, scaler_loss, &error);
    assert(scaler_prediction && scaler_loss && scaled_loss);
    assert(oasp_ai_tensor_backward(scaled_loss, NULL, 0, &error) == 0);
    assert(oasp_ai_loss_scaler_step(loss_scaler, scaler_optimizer, &error) == 0);
    near_value((float)oasp_ai_loss_scaler_scale(loss_scaler), FLT_MAX * 0.5f);
    copy_f32(scaler_weight, &scaled_weight_result, 1, &error);
    near_value(scaled_weight_result, 0.8f);
    oasp_ai_tensor_release(scaled_loss);
    oasp_ai_tensor_release(scaler_loss);
    oasp_ai_tensor_release(scaler_prediction);
    oasp_ai_loss_scaler_release(loss_scaler);
    oasp_ai_optimizer_release(scaler_optimizer);
    oasp_ai_module_release(scaler_module);
    oasp_ai_tensor_release(scaler_weight);
    oasp_ai_tensor_release(scaler_input);
    oasp_ai_tensor_release(scaler_host_weight);
    oasp_ai_tensor_release(scaler_host_input);

    oasp_ai_tensor_desc attention_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {1, 2, 1}};
    float query_values[] = {0, 0};
    float key_values[] = {0, 0};
    float value_values[] = {1, 3};
    oasp_ai_tensor *host_query = oasp_ai_tensor_create(context, &attention_desc, query_values, sizeof(query_values), &error);
    oasp_ai_tensor *host_key = oasp_ai_tensor_create(context, &attention_desc, key_values, sizeof(key_values), &error);
    oasp_ai_tensor *host_value = oasp_ai_tensor_create(context, &attention_desc, value_values, sizeof(value_values), &error);
    oasp_ai_tensor *query = oasp_ai_tensor_to_device(host_query, "cuda:0", &error);
    oasp_ai_tensor *key = oasp_ai_tensor_to_device(host_key, "cuda:0", &error);
    oasp_ai_tensor *value = oasp_ai_tensor_to_device(host_value, "cuda:0", &error);
    assert(query && key && value);
    assert(oasp_ai_tensor_requires_grad(query, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(key, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(value, 1, &error) == 0);
    uint64_t attention_d2h = oasp_ai_context_metrics(context)->device_to_host_bytes;
    oasp_ai_tensor *attention = oasp_ai_attention(context, query, key, value, NULL, 0, &error);
    oasp_ai_tensor *attention_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, attention, -1, 0, &error);
    assert(attention && attention_loss && oasp_ai_tensor_backward(attention_loss, NULL, 0, &error) == 0);
    assert(oasp_ai_context_metrics(context)->device_to_host_bytes == attention_d2h);
    float attention_values[2];
    copy_f32(attention, attention_values, 2, &error);
    near_value(attention_values[0], 2);
    near_value(attention_values[1], 2);
    oasp_ai_tensor *value_gradient = oasp_ai_tensor_gradient(value);
    float value_gradient_values[2];
    assert(value_gradient);
    copy_f32(value_gradient, value_gradient_values, 2, &error);
    near_value(value_gradient_values[0], 1);
    near_value(value_gradient_values[1], 1);

    oasp_ai_tensor_desc gradient_attention_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {1, 2, 2}};
    float gradient_query_values[] = {0.2f, -0.3f, 0.7f, 0.1f};
    float gradient_key_values[] = {-0.4f, 0.6f, 0.5f, -0.2f};
    float gradient_value_values[] = {1.0f, -2.0f, 0.5f, 3.0f};
    oasp_ai_tensor *cpu_query =
        oasp_ai_tensor_create(context, &gradient_attention_desc, gradient_query_values, sizeof(gradient_query_values), &error);
    oasp_ai_tensor *cpu_key =
        oasp_ai_tensor_create(context, &gradient_attention_desc, gradient_key_values, sizeof(gradient_key_values), &error);
    oasp_ai_tensor *cpu_value =
        oasp_ai_tensor_create(context, &gradient_attention_desc, gradient_value_values, sizeof(gradient_value_values), &error);
    assert(cpu_query && cpu_key && cpu_value);
    assert(oasp_ai_tensor_requires_grad(cpu_query, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cpu_key, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cpu_value, 1, &error) == 0);
    oasp_ai_tensor *cpu_attention = oasp_ai_attention(context, cpu_query, cpu_key, cpu_value, NULL, 0, &error);
    oasp_ai_tensor *cpu_attention_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, cpu_attention, -1, 0, &error);
    assert(cpu_attention && cpu_attention_loss && oasp_ai_tensor_backward(cpu_attention_loss, NULL, 0, &error) == 0);

    oasp_ai_tensor *cuda_query = oasp_ai_tensor_to_device(cpu_query, "cuda:0", &error);
    oasp_ai_tensor *cuda_key = oasp_ai_tensor_to_device(cpu_key, "cuda:0", &error);
    oasp_ai_tensor *cuda_value = oasp_ai_tensor_to_device(cpu_value, "cuda:0", &error);
    assert(cuda_query && cuda_key && cuda_value);
    assert(oasp_ai_tensor_requires_grad(cuda_query, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cuda_key, 1, &error) == 0);
    assert(oasp_ai_tensor_requires_grad(cuda_value, 1, &error) == 0);
    oasp_ai_tensor *cuda_attention = oasp_ai_attention(context, cuda_query, cuda_key, cuda_value, NULL, 0, &error);
    oasp_ai_tensor *cuda_attention_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, cuda_attention, -1, 0, &error);
    assert(cuda_attention && cuda_attention_loss && oasp_ai_tensor_backward(cuda_attention_loss, NULL, 0, &error) == 0);

    float cpu_attention_values[4];
    float cuda_attention_values[4];
    assert(oasp_ai_tensor_copy_to_host(cpu_attention, cpu_attention_values, sizeof(cpu_attention_values), &error) == 0);
    copy_f32(cuda_attention, cuda_attention_values, 4, &error);
    for (size_t index = 0; index < 4; index++)
        near_value(cuda_attention_values[index], cpu_attention_values[index]);
    oasp_ai_tensor *cpu_gradients[] = {
        oasp_ai_tensor_gradient(cpu_query),
        oasp_ai_tensor_gradient(cpu_key),
        oasp_ai_tensor_gradient(cpu_value),
    };
    oasp_ai_tensor *cuda_gradients[] = {
        oasp_ai_tensor_gradient(cuda_query),
        oasp_ai_tensor_gradient(cuda_key),
        oasp_ai_tensor_gradient(cuda_value),
    };
    for (size_t tensor_index = 0; tensor_index < 3; tensor_index++) {
        float expected[4];
        float actual[4];
        assert(cpu_gradients[tensor_index] && cuda_gradients[tensor_index]);
        assert(oasp_ai_tensor_copy_to_host(cpu_gradients[tensor_index], expected, sizeof(expected), &error) == 0);
        copy_f32(cuda_gradients[tensor_index], actual, 4, &error);
        for (size_t value_index = 0; value_index < 4; value_index++)
            near_value(actual[value_index], expected[value_index]);
        oasp_ai_tensor_release(cuda_gradients[tensor_index]);
        oasp_ai_tensor_release(cpu_gradients[tensor_index]);
    }

    // #region debug-point A:norm-backward-device-scalar-access
    if (getenv("OPENASP_AI_DEBUG_NORM")) {
        oasp_ai_tensor_desc norm_input_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {2, 28, 32}};
        oasp_ai_tensor_desc norm_parameter_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {32}};
        float norm_input_values[2 * 28 * 32];
        float norm_weight_values[32];
        float norm_bias_values[32] = {0};
        for (size_t index = 0; index < sizeof(norm_input_values) / sizeof(norm_input_values[0]); index++)
            norm_input_values[index] = (float)((int)(index % 17) - 8) / 8.0f;
        for (size_t index = 0; index < 32; index++)
            norm_weight_values[index] = 1;
        oasp_ai_tensor *cpu_norm_input =
            oasp_ai_tensor_create(context, &norm_input_desc, norm_input_values, sizeof(norm_input_values), &error);
        oasp_ai_tensor *cpu_norm_weight =
            oasp_ai_tensor_create(context, &norm_parameter_desc, norm_weight_values, sizeof(norm_weight_values), &error);
        oasp_ai_tensor *cpu_norm_bias =
            oasp_ai_tensor_create(context, &norm_parameter_desc, norm_bias_values, sizeof(norm_bias_values), &error);
        oasp_ai_tensor *cuda_norm_input = oasp_ai_tensor_to_device(cpu_norm_input, "cuda:0", &error);
        oasp_ai_tensor *cuda_norm_weight = oasp_ai_tensor_to_device(cpu_norm_weight, "cuda:0", &error);
        oasp_ai_tensor *cuda_norm_bias = oasp_ai_tensor_to_device(cpu_norm_bias, "cuda:0", &error);
        assert(cpu_norm_input && cpu_norm_weight && cpu_norm_bias && cuda_norm_input && cuda_norm_weight && cuda_norm_bias);
        assert(oasp_ai_tensor_requires_grad(cpu_norm_input, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(cpu_norm_weight, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(cpu_norm_bias, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(cuda_norm_input, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(cuda_norm_weight, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(cuda_norm_bias, 1, &error) == 0);

        uint64_t cpu_started = oasp_ai_now_nanoseconds();
        oasp_ai_tensor *cpu_norm =
            oasp_ai_layer_norm(context, cpu_norm_input, cpu_norm_weight, cpu_norm_bias, 1e-5, 0, &error);
        oasp_ai_tensor *cpu_norm_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, cpu_norm, -1, 0, &error);
        assert(cpu_norm && cpu_norm_loss && oasp_ai_tensor_backward(cpu_norm_loss, NULL, 0, &error) == 0);
        uint64_t cpu_elapsed = oasp_ai_now_nanoseconds() - cpu_started;

        uint64_t cuda_started = oasp_ai_now_nanoseconds();
        oasp_ai_tensor *cuda_norm =
            oasp_ai_layer_norm(context, cuda_norm_input, cuda_norm_weight, cuda_norm_bias, 1e-5, 0, &error);
        oasp_ai_tensor *cuda_norm_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, cuda_norm, -1, 0, &error);
        assert(cuda_norm && cuda_norm_loss && oasp_ai_tensor_backward(cuda_norm_loss, NULL, 0, &error) == 0);
        assert(oasp_ai_context_synchronize(context, &error) == 0);
        uint64_t cuda_elapsed = oasp_ai_now_nanoseconds() - cuda_started;

        const char *debug_url = getenv("DEBUG_SERVER_URL");
        const char *debug_run = getenv("DEBUG_RUN_ID");
        if (!debug_run || !*debug_run)
            debug_run = "pre-fix";
        if (debug_url && *debug_url) {
            char command[2048];
            snprintf(command, sizeof(command),
                     "curl -sS -X POST '%s' -H 'Content-Type: application/json' --data "
                     "'{\"sessionId\":\"transformer-gpu-underutilization\",\"runId\":\"%s\","
                     "\"hypothesisId\":\"A\",\"location\":\"tests/test_cuda.c:norm-backward-device-scalar-access\","
                     "\"msg\":\"[DEBUG] LayerNorm forward-backward timing\","
                     "\"data\":{\"elements\":1792,\"cpu_ms\":%.3f,\"cuda_ms\":%.3f,\"cuda_to_cpu_ratio\":%.1f}}'",
                     debug_url, debug_run, (double)cpu_elapsed / 1e6, (double)cuda_elapsed / 1e6,
                     cpu_elapsed ? (double)cuda_elapsed / (double)cpu_elapsed : 0);
            assert(system(command) == 0);
        }

        oasp_ai_tensor_release(cuda_norm_loss);
        oasp_ai_tensor_release(cuda_norm);
        oasp_ai_tensor_release(cuda_norm_bias);
        oasp_ai_tensor_release(cuda_norm_weight);
        oasp_ai_tensor_release(cuda_norm_input);
        oasp_ai_tensor_release(cpu_norm_loss);
        oasp_ai_tensor_release(cpu_norm);
        oasp_ai_tensor_release(cpu_norm_bias);
        oasp_ai_tensor_release(cpu_norm_weight);
        oasp_ai_tensor_release(cpu_norm_input);
    }
    // #endregion

    // #region debug-point A:strided-contiguous-device-fallback
    if (getenv("OPENASP_AI_DEBUG_CONTIGUOUS")) {
        enum { patch_batch = 16, patch_elements = patch_batch * 28 * 28 };
        oasp_ai_tensor_desc image_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {patch_batch, 28, 28}};
        int64_t patch_view_shape[] = {patch_batch, 7, 4, 7, 4};
        int64_t patch_matrix_shape[] = {patch_batch * 49, 16};
        float image_values[patch_elements];
        for (size_t index = 0; index < patch_elements; index++)
            image_values[index] = (float)(index % 256) / 255.0f;

        oasp_ai_tensor *cpu_images = oasp_ai_tensor_create(context, &image_desc, image_values, sizeof(image_values), &error);
        oasp_ai_tensor *cuda_images = oasp_ai_tensor_to_device(cpu_images, "cuda:0", &error);
        assert(cpu_images && cuda_images);

        uint64_t cpu_started = oasp_ai_now_nanoseconds();
        oasp_ai_tensor *cpu_patch_view = oasp_ai_tensor_reshape(cpu_images, patch_view_shape, 5, &error);
        oasp_ai_tensor *cpu_patch_transposed = oasp_ai_tensor_transpose(cpu_patch_view, 2, 3, &error);
        oasp_ai_tensor *cpu_patches = oasp_ai_tensor_reshape(cpu_patch_transposed, patch_matrix_shape, 2, &error);
        assert(cpu_patch_view && cpu_patch_transposed && cpu_patches);
        uint64_t cpu_elapsed = oasp_ai_now_nanoseconds() - cpu_started;

        uint64_t cuda_started = oasp_ai_now_nanoseconds();
        oasp_ai_tensor *cuda_patch_view = oasp_ai_tensor_reshape(cuda_images, patch_view_shape, 5, &error);
        oasp_ai_tensor *cuda_patch_transposed = oasp_ai_tensor_transpose(cuda_patch_view, 2, 3, &error);
        oasp_ai_tensor *cuda_patches = oasp_ai_tensor_reshape(cuda_patch_transposed, patch_matrix_shape, 2, &error);
        assert(cuda_patch_view && cuda_patch_transposed && cuda_patches);
        assert(oasp_ai_context_synchronize(context, &error) == 0);
        uint64_t cuda_elapsed = oasp_ai_now_nanoseconds() - cuda_started;
        float cpu_patch_values[patch_elements];
        float cuda_patch_values[patch_elements];
        assert(oasp_ai_tensor_copy_to_host(cpu_patches, cpu_patch_values, sizeof(cpu_patch_values), &error) == 0);
        copy_f32(cuda_patches, cuda_patch_values, patch_elements, &error);
        for (size_t index = 0; index < patch_elements; index++)
            near_value(cuda_patch_values[index], cpu_patch_values[index]);

        const char *debug_url = getenv("DEBUG_SERVER_URL");
        const char *debug_run = getenv("DEBUG_RUN_ID");
        if (!debug_run || !*debug_run)
            debug_run = "pre-fix";
        if (debug_url && *debug_url) {
            char command[2048];
            snprintf(command, sizeof(command),
                     "curl -sS -X POST '%s' -H 'Content-Type: application/json' --data "
                     "'{\"sessionId\":\"cuda-strided-contiguous\",\"runId\":\"%s\","
                     "\"hypothesisId\":\"A\",\"location\":\"tests/test_cuda.c:strided-contiguous-device-fallback\","
                     "\"msg\":\"[DEBUG] Patch transpose contiguous timing\","
                     "\"data\":{\"elements\":%d,\"cpu_ms\":%.3f,\"cuda_ms\":%.3f,\"cuda_to_cpu_ratio\":%.1f}}'",
                     debug_url, debug_run, patch_elements, (double)cpu_elapsed / 1e6, (double)cuda_elapsed / 1e6,
                     cpu_elapsed ? (double)cuda_elapsed / (double)cpu_elapsed : 0);
            assert(system(command) == 0);
        }

        oasp_ai_tensor_release(cuda_patches);
        oasp_ai_tensor_release(cuda_patch_transposed);
        oasp_ai_tensor_release(cuda_patch_view);
        oasp_ai_tensor_release(cuda_images);
        oasp_ai_tensor_release(cpu_patches);
        oasp_ai_tensor_release(cpu_patch_transposed);
        oasp_ai_tensor_release(cpu_patch_view);
        oasp_ai_tensor_release(cpu_images);
    }
    // #endregion

    // #region debug-point A-B:vit-patch-attention-parity
    if (getenv("OPENASP_AI_DEBUG_VIT_ACCURACY")) {
        enum { vit_batch = 2, vit_tokens = 49, vit_width = 32, vit_elements = vit_batch * vit_tokens * vit_width };
        oasp_ai_tensor_desc vit_image_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {1, 28, 28}};
        int64_t vit_patch_view_shape[] = {1, 7, 4, 7, 4};
        int64_t vit_patch_matrix_shape[] = {49, 16};
        float vit_image_values[28 * 28];
        for (size_t index = 0; index < 28 * 28; index++)
            vit_image_values[index] = (float)index;
        oasp_ai_tensor *vit_images =
            oasp_ai_tensor_create(context, &vit_image_desc, vit_image_values, sizeof(vit_image_values), &error);
        oasp_ai_tensor *vit_patch_view = oasp_ai_tensor_reshape(vit_images, vit_patch_view_shape, 5, &error);
        oasp_ai_tensor *vit_patch_transposed = oasp_ai_tensor_transpose(vit_patch_view, 2, 3, &error);
        oasp_ai_tensor *vit_patches = oasp_ai_tensor_reshape(vit_patch_transposed, vit_patch_matrix_shape, 2, &error);
        assert(vit_images && vit_patch_view && vit_patch_transposed && vit_patches);
        float vit_patch_values[28 * 28];
        assert(oasp_ai_tensor_copy_to_host(vit_patches, vit_patch_values, sizeof(vit_patch_values), &error) == 0);
        size_t patch_mismatches = 0;
        for (size_t patch_row = 0; patch_row < 7; patch_row++)
            for (size_t patch_column = 0; patch_column < 7; patch_column++)
                for (size_t inner_row = 0; inner_row < 4; inner_row++)
                    for (size_t inner_column = 0; inner_column < 4; inner_column++) {
                        size_t token = patch_row * 7 + patch_column;
                        size_t channel = inner_row * 4 + inner_column;
                        size_t source = (patch_row * 4 + inner_row) * 28 + patch_column * 4 + inner_column;
                        if (vit_patch_values[token * 16 + channel] != (float)source)
                            patch_mismatches++;
                    }

        oasp_ai_tensor_desc vit_attention_desc = {
            .dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {vit_batch, vit_tokens, vit_width}};
        float *vit_query_values = malloc(vit_elements * sizeof(*vit_query_values));
        float *vit_key_values = malloc(vit_elements * sizeof(*vit_key_values));
        float *vit_value_values = malloc(vit_elements * sizeof(*vit_value_values));
        assert(vit_query_values && vit_key_values && vit_value_values);
        for (size_t index = 0; index < vit_elements; index++) {
            vit_query_values[index] = (float)((int)(index % 23) - 11) / 32.0f;
            vit_key_values[index] = (float)((int)(index % 19) - 9) / 32.0f;
            vit_value_values[index] = (float)((int)(index % 17) - 8) / 16.0f;
        }
        oasp_ai_tensor *vit_cpu_query =
            oasp_ai_tensor_create(context, &vit_attention_desc, vit_query_values, vit_elements * sizeof(float), &error);
        oasp_ai_tensor *vit_cpu_key =
            oasp_ai_tensor_create(context, &vit_attention_desc, vit_key_values, vit_elements * sizeof(float), &error);
        oasp_ai_tensor *vit_cpu_value =
            oasp_ai_tensor_create(context, &vit_attention_desc, vit_value_values, vit_elements * sizeof(float), &error);
        assert(vit_cpu_query && vit_cpu_key && vit_cpu_value);
        assert(oasp_ai_tensor_requires_grad(vit_cpu_query, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(vit_cpu_key, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(vit_cpu_value, 1, &error) == 0);
        oasp_ai_tensor *vit_cpu_output =
            oasp_ai_attention(context, vit_cpu_query, vit_cpu_key, vit_cpu_value, NULL, 0, &error);
        oasp_ai_tensor *vit_cpu_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, vit_cpu_output, -1, 0, &error);
        assert(vit_cpu_output && vit_cpu_loss && oasp_ai_tensor_backward(vit_cpu_loss, NULL, 0, &error) == 0);

        oasp_ai_tensor *vit_cuda_query = oasp_ai_tensor_to_device(vit_cpu_query, "cuda:0", &error);
        oasp_ai_tensor *vit_cuda_key = oasp_ai_tensor_to_device(vit_cpu_key, "cuda:0", &error);
        oasp_ai_tensor *vit_cuda_value = oasp_ai_tensor_to_device(vit_cpu_value, "cuda:0", &error);
        assert(vit_cuda_query && vit_cuda_key && vit_cuda_value);
        assert(oasp_ai_tensor_requires_grad(vit_cuda_query, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(vit_cuda_key, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(vit_cuda_value, 1, &error) == 0);
        oasp_ai_tensor *vit_cuda_output =
            oasp_ai_attention(context, vit_cuda_query, vit_cuda_key, vit_cuda_value, NULL, 0, &error);
        oasp_ai_tensor *vit_cuda_loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, vit_cuda_output, -1, 0, &error);
        assert(vit_cuda_output && vit_cuda_loss && oasp_ai_tensor_backward(vit_cuda_loss, NULL, 0, &error) == 0);

        float *vit_expected = malloc(vit_elements * sizeof(*vit_expected));
        float *vit_actual = malloc(vit_elements * sizeof(*vit_actual));
        assert(vit_expected && vit_actual);
        float maximum_differences[4] = {0};
        oasp_ai_tensor *vit_cpu_results[] = {
            vit_cpu_output,
            oasp_ai_tensor_gradient(vit_cpu_query),
            oasp_ai_tensor_gradient(vit_cpu_key),
            oasp_ai_tensor_gradient(vit_cpu_value),
        };
        oasp_ai_tensor *vit_cuda_results[] = {
            vit_cuda_output,
            oasp_ai_tensor_gradient(vit_cuda_query),
            oasp_ai_tensor_gradient(vit_cuda_key),
            oasp_ai_tensor_gradient(vit_cuda_value),
        };
        for (size_t result_index = 0; result_index < 4; result_index++) {
            assert(vit_cpu_results[result_index] && vit_cuda_results[result_index]);
            assert(oasp_ai_tensor_copy_to_host(vit_cpu_results[result_index], vit_expected,
                                               vit_elements * sizeof(float), &error) == 0);
            copy_f32(vit_cuda_results[result_index], vit_actual, vit_elements, &error);
            for (size_t index = 0; index < vit_elements; index++)
                maximum_differences[result_index] =
                    fmaxf(maximum_differences[result_index], fabsf(vit_actual[index] - vit_expected[index]));
        }

        const char *vit_debug_url = getenv("DEBUG_SERVER_URL");
        const char *vit_debug_run = getenv("DEBUG_RUN_ID");
        if (!vit_debug_run || !*vit_debug_run)
            vit_debug_run = "pre-fix";
        if (vit_debug_url && *vit_debug_url) {
            char command[2048];
            snprintf(command, sizeof(command),
                     "curl -sS -X POST '%s' -H 'Content-Type: application/json' --data "
                     "'{\"sessionId\":\"vit-low-accuracy\",\"runId\":\"%s\","
                     "\"hypothesisId\":\"A\",\"location\":\"tests/test_cuda.c:vit-patch-semantic-order\","
                     "\"msg\":\"[DEBUG] ViT patch semantic ordering\","
                     "\"data\":{\"elements\":784,\"mismatches\":%zu}}'",
                     vit_debug_url, vit_debug_run, patch_mismatches);
            assert(system(command) == 0);
            snprintf(command, sizeof(command),
                     "curl -sS -X POST '%s' -H 'Content-Type: application/json' --data "
                     "'{\"sessionId\":\"vit-low-accuracy\",\"runId\":\"%s\","
                     "\"hypothesisId\":\"B\",\"location\":\"tests/test_cuda.c:vit-attention-parity\","
                     "\"msg\":\"[DEBUG] ViT-shape CPU CUDA attention parity\","
                     "\"data\":{\"shape\":\"2x49x32\",\"output_max_abs\":%.9g,\"query_grad_max_abs\":%.9g,"
                     "\"key_grad_max_abs\":%.9g,\"value_grad_max_abs\":%.9g}}'",
                     vit_debug_url, vit_debug_run, maximum_differences[0], maximum_differences[1],
                     maximum_differences[2], maximum_differences[3]);
            assert(system(command) == 0);
        }

        for (size_t index = 1; index < 4; index++) {
            oasp_ai_tensor_release(vit_cuda_results[index]);
            oasp_ai_tensor_release(vit_cpu_results[index]);
        }
        free(vit_actual);
        free(vit_expected);
        oasp_ai_tensor_release(vit_cuda_loss);
        oasp_ai_tensor_release(vit_cuda_output);
        oasp_ai_tensor_release(vit_cuda_value);
        oasp_ai_tensor_release(vit_cuda_key);
        oasp_ai_tensor_release(vit_cuda_query);
        oasp_ai_tensor_release(vit_cpu_loss);
        oasp_ai_tensor_release(vit_cpu_output);
        oasp_ai_tensor_release(vit_cpu_value);
        oasp_ai_tensor_release(vit_cpu_key);
        oasp_ai_tensor_release(vit_cpu_query);
        free(vit_value_values);
        free(vit_key_values);
        free(vit_query_values);
        oasp_ai_tensor_release(vit_patches);
        oasp_ai_tensor_release(vit_patch_transposed);
        oasp_ai_tensor_release(vit_patch_view);
        oasp_ai_tensor_release(vit_images);
    }
    // #endregion

    // #region debug-point B:vit-layer-norm-parity
    if (getenv("OPENASP_AI_DEBUG_VIT_ACCURACY")) {
        enum { norm_batch = 2, norm_tokens = 49, norm_width = 32, norm_elements = norm_batch * norm_tokens * norm_width };
        oasp_ai_tensor_desc norm_input_desc = {
            .dtype = OASP_AI_DTYPE_F32, .rank = 3, .shape = {norm_batch, norm_tokens, norm_width}};
        oasp_ai_tensor_desc norm_parameter_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {norm_width}};
        float *norm_input_values = malloc(norm_elements * sizeof(*norm_input_values));
        float *norm_upstream_values = malloc(norm_elements * sizeof(*norm_upstream_values));
        float norm_weight_values[norm_width];
        float norm_bias_values[norm_width];
        assert(norm_input_values && norm_upstream_values);
        for (size_t index = 0; index < norm_elements; index++) {
            norm_input_values[index] = (float)((int)(index % 29) - 14) / 8.0f;
            norm_upstream_values[index] = (float)((int)(index % 13) - 6) / 16.0f;
        }
        for (size_t index = 0; index < norm_width; index++) {
            norm_weight_values[index] = 0.5f + (float)index / 64.0f;
            norm_bias_values[index] = (float)((int)(index % 7) - 3) / 32.0f;
        }

        oasp_ai_tensor *norm_cpu_input =
            oasp_ai_tensor_create(context, &norm_input_desc, norm_input_values, norm_elements * sizeof(float), &error);
        oasp_ai_tensor *norm_cpu_upstream =
            oasp_ai_tensor_create(context, &norm_input_desc, norm_upstream_values, norm_elements * sizeof(float), &error);
        oasp_ai_tensor *norm_cpu_weight =
            oasp_ai_tensor_create(context, &norm_parameter_desc, norm_weight_values, sizeof(norm_weight_values), &error);
        oasp_ai_tensor *norm_cpu_bias =
            oasp_ai_tensor_create(context, &norm_parameter_desc, norm_bias_values, sizeof(norm_bias_values), &error);
        assert(norm_cpu_input && norm_cpu_upstream && norm_cpu_weight && norm_cpu_bias);
        assert(oasp_ai_tensor_requires_grad(norm_cpu_input, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(norm_cpu_weight, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(norm_cpu_bias, 1, &error) == 0);
        oasp_ai_tensor *norm_cpu_output =
            oasp_ai_layer_norm(context, norm_cpu_input, norm_cpu_weight, norm_cpu_bias, 1e-5, 0, &error);
        assert(norm_cpu_output && oasp_ai_tensor_backward(norm_cpu_output, norm_cpu_upstream, 0, &error) == 0);

        oasp_ai_tensor *norm_cuda_input = oasp_ai_tensor_to_device(norm_cpu_input, "cuda:0", &error);
        oasp_ai_tensor *norm_cuda_upstream = oasp_ai_tensor_to_device(norm_cpu_upstream, "cuda:0", &error);
        oasp_ai_tensor *norm_cuda_weight = oasp_ai_tensor_to_device(norm_cpu_weight, "cuda:0", &error);
        oasp_ai_tensor *norm_cuda_bias = oasp_ai_tensor_to_device(norm_cpu_bias, "cuda:0", &error);
        assert(norm_cuda_input && norm_cuda_upstream && norm_cuda_weight && norm_cuda_bias);
        assert(oasp_ai_tensor_requires_grad(norm_cuda_input, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(norm_cuda_weight, 1, &error) == 0);
        assert(oasp_ai_tensor_requires_grad(norm_cuda_bias, 1, &error) == 0);
        oasp_ai_tensor *norm_cuda_output =
            oasp_ai_layer_norm(context, norm_cuda_input, norm_cuda_weight, norm_cuda_bias, 1e-5, 0, &error);
        assert(norm_cuda_output && oasp_ai_tensor_backward(norm_cuda_output, norm_cuda_upstream, 0, &error) == 0);

        float *norm_expected = malloc(norm_elements * sizeof(*norm_expected));
        float *norm_actual = malloc(norm_elements * sizeof(*norm_actual));
        assert(norm_expected && norm_actual);
        float norm_maximum_differences[4] = {0};
        oasp_ai_tensor *norm_cpu_results[] = {
            norm_cpu_output,
            oasp_ai_tensor_gradient(norm_cpu_input),
            oasp_ai_tensor_gradient(norm_cpu_weight),
            oasp_ai_tensor_gradient(norm_cpu_bias),
        };
        oasp_ai_tensor *norm_cuda_results[] = {
            norm_cuda_output,
            oasp_ai_tensor_gradient(norm_cuda_input),
            oasp_ai_tensor_gradient(norm_cuda_weight),
            oasp_ai_tensor_gradient(norm_cuda_bias),
        };
        for (size_t result_index = 0; result_index < 4; result_index++) {
            size_t elements = result_index < 2 ? norm_elements : norm_width;
            assert(norm_cpu_results[result_index] && norm_cuda_results[result_index]);
            assert(oasp_ai_tensor_copy_to_host(norm_cpu_results[result_index], norm_expected,
                                               elements * sizeof(float), &error) == 0);
            copy_f32(norm_cuda_results[result_index], norm_actual, elements, &error);
            for (size_t index = 0; index < elements; index++)
                norm_maximum_differences[result_index] =
                    fmaxf(norm_maximum_differences[result_index], fabsf(norm_actual[index] - norm_expected[index]));
        }

        const char *norm_debug_url = getenv("DEBUG_SERVER_URL");
        const char *norm_debug_run = getenv("DEBUG_RUN_ID");
        if (!norm_debug_run || !*norm_debug_run)
            norm_debug_run = "pre-fix";
        if (norm_debug_url && *norm_debug_url) {
            char command[2048];
            snprintf(command, sizeof(command),
                     "curl -sS -X POST '%s' -H 'Content-Type: application/json' --data "
                     "'{\"sessionId\":\"vit-low-accuracy\",\"runId\":\"%s\","
                     "\"hypothesisId\":\"B\",\"location\":\"tests/test_cuda.c:vit-layer-norm-parity\","
                     "\"msg\":\"[DEBUG] ViT-shape CPU CUDA LayerNorm parity\","
                     "\"data\":{\"shape\":\"2x49x32\",\"output_max_abs\":%.9g,\"input_grad_max_abs\":%.9g,"
                     "\"weight_grad_max_abs\":%.9g,\"bias_grad_max_abs\":%.9g}}'",
                     norm_debug_url, norm_debug_run, norm_maximum_differences[0], norm_maximum_differences[1],
                     norm_maximum_differences[2], norm_maximum_differences[3]);
            assert(system(command) == 0);
        }

        for (size_t index = 1; index < 4; index++) {
            oasp_ai_tensor_release(norm_cuda_results[index]);
            oasp_ai_tensor_release(norm_cpu_results[index]);
        }
        free(norm_actual);
        free(norm_expected);
        oasp_ai_tensor_release(norm_cuda_output);
        oasp_ai_tensor_release(norm_cuda_bias);
        oasp_ai_tensor_release(norm_cuda_weight);
        oasp_ai_tensor_release(norm_cuda_upstream);
        oasp_ai_tensor_release(norm_cuda_input);
        oasp_ai_tensor_release(norm_cpu_output);
        oasp_ai_tensor_release(norm_cpu_bias);
        oasp_ai_tensor_release(norm_cpu_weight);
        oasp_ai_tensor_release(norm_cpu_upstream);
        oasp_ai_tensor_release(norm_cpu_input);
        free(norm_upstream_values);
        free(norm_input_values);
    }
    // #endregion

    oasp_ai_registered_backend *cuda_backend = &context->backends[oasp_ai_tensor_backend_index(weight)];
    oasp_ai_memory_stats stats = {.struct_size = sizeof(stats)};
    assert(cuda_backend->descriptor->api->memory_stats(cuda_backend->device, &stats, &error) == 0);
    assert(stats.live_bytes > 0 && stats.reserved_bytes >= stats.live_bytes);
    printf("openasp-ai-cuda: device=cuda:0 live=%llu reserved=%llu steps=8 d2h_during_training=0 training=passed graph=passed\n",
           (unsigned long long)stats.live_bytes, (unsigned long long)stats.reserved_bytes);

    oasp_ai_tensor_release(cuda_attention_loss);
    oasp_ai_tensor_release(cuda_attention);
    oasp_ai_tensor_release(cuda_value);
    oasp_ai_tensor_release(cuda_key);
    oasp_ai_tensor_release(cuda_query);
    oasp_ai_tensor_release(cpu_attention_loss);
    oasp_ai_tensor_release(cpu_attention);
    oasp_ai_tensor_release(cpu_value);
    oasp_ai_tensor_release(cpu_key);
    oasp_ai_tensor_release(cpu_query);
    oasp_ai_tensor_release(value_gradient);
    oasp_ai_tensor_release(attention_loss);
    oasp_ai_tensor_release(attention);
    oasp_ai_tensor_release(value);
    oasp_ai_tensor_release(key);
    oasp_ai_tensor_release(query);
    oasp_ai_tensor_release(host_value);
    oasp_ai_tensor_release(host_key);
    oasp_ai_tensor_release(host_query);
    oasp_ai_optimizer_release(adamw);
    oasp_ai_optimizer_release(optimizer);
    oasp_ai_module_release(module);
    oasp_ai_tensor_release(weight);
    oasp_ai_tensor_release(input);
    oasp_ai_tensor_release(host_weight);
    oasp_ai_tensor_release(host_input);
    oasp_ai_tensor_release(plan_outputs[0]);
    free(plan_outputs);
    oasp_ai_plan_release(plan);
    oasp_ai_graph_release(graph);
    oasp_ai_tensor_release(sum);
    oasp_ai_tensor_release(right);
    oasp_ai_tensor_release(left);
    oasp_ai_tensor_release(host_right);
    oasp_ai_tensor_release(host_left);
    assert(oasp_ai_context_metrics(context)->live_bytes == 0);
    oasp_ai_context_destroy(context);
    return 0;
}
