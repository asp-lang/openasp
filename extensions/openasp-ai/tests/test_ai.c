// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const oasp_ai_backend_descriptor *oasp_ai_test_mock_backend(void);

static void near(double actual, double expected) {
    assert(fabs(actual - expected) < 1e-5);
}

static void write_standard_safetensors(const char *path, int overlap) {
    char header[512];
    int length = snprintf(
        header, sizeof(header),
        "{\"__metadata__\":{\"format\":\"pt\",\"note\":\"external\\\\u0020file\"},"
        "\"weight\":{\"data_offsets\":[0,8],\"shape\":[2],\"dtype\":\"F32\"},"
        "\"running_mean\":{\"shape\":[2],\"dtype\":\"F32\",\"data_offsets\":[%d,%d]}}",
        overlap ? 4 : 8, overlap ? 12 : 16);
    assert(length > 0 && (size_t)length < sizeof(header));
    while (length % 8)
        header[length++] = ' ';
    unsigned char encoded_length[8];
    for (unsigned index = 0; index < sizeof(encoded_length); index++)
        encoded_length[index] = (unsigned char)((uint64_t)length >> (index * 8));
    const float values[] = {0.9f, 0.9f, 4, 4};
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(encoded_length, 1, sizeof(encoded_length), file) == sizeof(encoded_length) &&
           fwrite(header, 1, (size_t)length, file) == (size_t)length &&
           fwrite(values, 1, sizeof(values), file) == sizeof(values) && fclose(file) == 0);
}

static void test_tensor_and_autograd(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    int64_t x_shape[] = {2, 2};
    int64_t w_shape[] = {2, 1};
    float x_data[] = {1, 2, 3, 4};
    float w_data[] = {2, 3};
    oasp_ai_tensor_desc x_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    oasp_ai_tensor_desc w_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 1}};
    oasp_ai_tensor *x = oasp_ai_tensor_create(context, &x_desc, x_data, sizeof(x_data), &error);
    oasp_ai_tensor *w = oasp_ai_tensor_create(context, &w_desc, w_data, sizeof(w_data), &error);
    assert(x && w);
    assert(oasp_ai_tensor_requires_grad(w, 1, &error) == 0);
    oasp_ai_tensor *y = oasp_ai_matmul(context, x, w, &error);
    assert(y && oasp_ai_tensor_elements(y) == 2);
    near(((const float *)oasp_ai_tensor_const_data(y))[0], 8);
    near(((const float *)oasp_ai_tensor_const_data(y))[1], 18);
    oasp_ai_tensor *loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, y, -1, 0, &error);
    assert(loss && oasp_ai_tensor_backward(loss, NULL, 0, &error) == 0);
    oasp_ai_tensor *gradient = oasp_ai_tensor_gradient(w);
    assert(gradient);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[0], 4);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[1], 6);

    oasp_ai_tensor *transposed = oasp_ai_tensor_transpose(x, 0, 1, &error);
    assert(transposed && !oasp_ai_tensor_data(transposed));
    oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(transposed, &error);
    assert(contiguous && oasp_ai_tensor_data(contiguous));
    const float *values = oasp_ai_tensor_const_data(contiguous);
    near(values[0], 1);
    near(values[1], 3);
    near(values[2], 2);
    near(values[3], 4);

    oasp_ai_tensor_release(contiguous);
    oasp_ai_tensor_release(transposed);
    oasp_ai_tensor_release(gradient);
    oasp_ai_tensor_release(loss);
    oasp_ai_tensor_release(y);
    oasp_ai_tensor_release(w);
    oasp_ai_tensor_release(x);
    (void)x_shape;
    (void)w_shape;
}

static void test_module_optimizer_and_weights(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    int64_t shape[] = {2};
    oasp_ai_tensor *parameter = oasp_ai_tensor_full(context, shape, 1, OASP_AI_DTYPE_F32, 1, &error);
    oasp_ai_tensor *buffer = oasp_ai_tensor_full(context, shape, 1, OASP_AI_DTYPE_F32, 4, &error);
    oasp_ai_module *module = oasp_ai_module_create(context, "Linear", &error);
    assert(parameter && buffer && module);
    assert(oasp_ai_module_register_parameter(module, "weight", parameter, &error) == 0);
    assert(oasp_ai_module_register_buffer(module, "running_mean", buffer, 1, &error) == 0);
    oasp_ai_tensor *objective = oasp_ai_reduce(context, OASP_AI_OP_SUM, parameter, -1, 0, &error);
    assert(objective && oasp_ai_tensor_backward(objective, NULL, 0, &error) == 0);
    oasp_ai_optimizer *optimizer = oasp_ai_optimizer_create(module, OASP_AI_OPTIMIZER_SGD, 0.1, 0, &error);
    assert(optimizer && oasp_ai_optimizer_step(optimizer, &error) == 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 0.9);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[1], 0.9);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/openasp-ai-weights-%ld.oaspweights", (long)getpid());
    assert(oasp_ai_module_save_weights(module, path, &error) == 0);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 7;
    ((float *)oasp_ai_tensor_data(buffer))[0] = 8;
    assert(oasp_ai_module_load_weights(module, path, 1, &error) == 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 0.9);
    near(((const float *)oasp_ai_tensor_const_data(buffer))[0], 4);
    unlink(path);

    oasp_ai_state_dict *state = oasp_ai_module_state_dict(module, &error);
    assert(state && oasp_ai_state_dict_count(state) == 2);
    const char *state_name = NULL;
    oasp_ai_state_kind state_kind;
    oasp_ai_tensor *state_tensor = oasp_ai_state_dict_tensor(state, 1, &state_name, &state_kind);
    assert(state_tensor && !strcmp(state_name, "running_mean") && state_kind == OASP_AI_STATE_BUFFER);
    oasp_ai_tensor_release(state_tensor);
    oasp_ai_state_dict_release(state);

    snprintf(path, sizeof(path), "/tmp/openasp-ai-weights-%ld.safetensors", (long)getpid());
    assert(oasp_ai_module_save_safetensors(module, path, &error) == 0);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 11;
    ((float *)oasp_ai_tensor_data(buffer))[0] = 12;
    oasp_ai_state_diagnostics diagnostics = {0};
    assert(oasp_ai_module_load_safetensors(module, path, 1, &diagnostics, &error) == 0);
    assert(!diagnostics.missing_count && !diagnostics.unexpected_count && !diagnostics.shape_mismatch_count &&
           !diagnostics.dtype_mismatch_count);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 0.9);
    near(((const float *)oasp_ai_tensor_const_data(buffer))[0], 4);
    oasp_ai_state_diagnostics_free(&diagnostics);
    unlink(path);

    write_standard_safetensors(path, 0);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 31;
    assert(oasp_ai_module_load_safetensors(module, path, 1, &diagnostics, &error) == 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 0.9);
    near(((const float *)oasp_ai_tensor_const_data(buffer))[0], 4);
    oasp_ai_state_diagnostics_free(&diagnostics);
    write_standard_safetensors(path, 1);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 55;
    assert(oasp_ai_module_load_safetensors(module, path, 1, &diagnostics, &error) != 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 55);
    oasp_ai_state_diagnostics_free(&diagnostics);
    unlink(path);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 0.9f;

    const unsigned char bytecode[] = {0x4f, 0x41, 0x43, 0x00};
    const char configuration[] = "{\"features\":2}";
    const char dependencies[] = "[]";
    oasp_ai_bundle_security bundle_security = {.allowed_root = "/tmp", .allow_unsigned = 1};
    snprintf(path, sizeof(path), "/tmp/openasp-ai-bundle-%ld.oaspbundle", (long)getpid());
    assert(oasp_ai_save_bundle(path, module, "LinearModel", bytecode, sizeof(bytecode), configuration,
                               sizeof(configuration) - 1, dependencies, sizeof(dependencies) - 1, &bundle_security, &error) == 0);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 21;
    char *entry_class = NULL;
    void *loaded_bytecode = NULL;
    size_t loaded_bytecode_size = 0;
    void *loaded_configuration = NULL;
    size_t loaded_configuration_size = 0;
    void *loaded_dependencies = NULL;
    size_t loaded_dependencies_size = 0;
    assert(oasp_ai_load_bundle(context, path, module, &entry_class, &loaded_bytecode, &loaded_bytecode_size,
                               &loaded_configuration, &loaded_configuration_size, &loaded_dependencies,
                               &loaded_dependencies_size, &bundle_security, &error) == 0);
    assert(!strcmp(entry_class, "LinearModel") && loaded_bytecode_size == sizeof(bytecode) &&
           !memcmp(loaded_bytecode, bytecode, sizeof(bytecode)) &&
           loaded_configuration_size == sizeof(configuration) - 1 &&
           loaded_dependencies_size == sizeof(dependencies) - 1);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 0.9);
    free(entry_class);
    free(loaded_bytecode);
    free(loaded_configuration);
    free(loaded_dependencies);
    unlink(path);

    oasp_ai_optimizer_release(optimizer);
    oasp_ai_tensor_release(objective);
    oasp_ai_module_release(module);
    oasp_ai_tensor_release(buffer);
    oasp_ai_tensor_release(parameter);
}

static void test_view_softmax_and_broadcast_gradients(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    oasp_ai_tensor_desc vector_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {3}};
    float vector_data[] = {1, 2, 3};
    float weights_data[] = {1, 0, -1};
    oasp_ai_tensor *vector = oasp_ai_tensor_create(context, &vector_desc, vector_data, sizeof(vector_data), &error);
    oasp_ai_tensor *weights = oasp_ai_tensor_create(context, &vector_desc, weights_data, sizeof(weights_data), &error);
    assert(vector && weights && oasp_ai_tensor_requires_grad(vector, 1, &error) == 0);
    oasp_ai_tensor *probabilities = oasp_ai_softmax(context, vector, 0, &error);
    oasp_ai_tensor *weighted = oasp_ai_binary(context, OASP_AI_OP_MUL, probabilities, weights, &error);
    oasp_ai_tensor *loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, weighted, -1, 0, &error);
    assert(loss && oasp_ai_tensor_backward(loss, NULL, 0, &error) == 0);
    oasp_ai_tensor *gradient = oasp_ai_tensor_gradient(vector);
    assert(gradient);
    double exponential[] = {exp(1), exp(2), exp(3)};
    double total = exponential[0] + exponential[1] + exponential[2];
    double dot = exponential[0] / total - exponential[2] / total;
    for (size_t index = 0; index < 3; index++) {
        double probability = exponential[index] / total;
        near(((const float *)oasp_ai_tensor_const_data(gradient))[index], probability * (weights_data[index] - dot));
    }
    oasp_ai_tensor_release(gradient);
    oasp_ai_tensor_release(loss);
    oasp_ai_tensor_release(weighted);
    oasp_ai_tensor_release(probabilities);
    oasp_ai_tensor_release(weights);
    oasp_ai_tensor_release(vector);

    oasp_ai_tensor_desc matrix_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 3}};
    oasp_ai_tensor_desc bias_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 1}};
    float matrix_data[] = {1, 2, 3, 4, 5, 6};
    float bias_data[] = {2, 3};
    oasp_ai_tensor *matrix = oasp_ai_tensor_create(context, &matrix_desc, matrix_data, sizeof(matrix_data), &error);
    oasp_ai_tensor *bias = oasp_ai_tensor_create(context, &bias_desc, bias_data, sizeof(bias_data), &error);
    assert(matrix && bias && oasp_ai_tensor_requires_grad(bias, 1, &error) == 0);
    oasp_ai_tensor *product = oasp_ai_binary(context, OASP_AI_OP_MUL, matrix, bias, &error);
    loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, product, -1, 0, &error);
    assert(loss && oasp_ai_tensor_backward(loss, NULL, 0, &error) == 0);
    gradient = oasp_ai_tensor_gradient(bias);
    assert(gradient);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[0], 6);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[1], 15);
    oasp_ai_tensor_release(gradient);
    oasp_ai_tensor_release(loss);
    oasp_ai_tensor_release(product);
    oasp_ai_tensor_release(bias);
    oasp_ai_tensor_release(matrix);

    int64_t slice_shape[] = {4};
    oasp_ai_tensor *source = oasp_ai_tensor_full(context, slice_shape, 1, OASP_AI_DTYPE_F32, 1, &error);
    assert(source && oasp_ai_tensor_requires_grad(source, 1, &error) == 0);
    oasp_ai_tensor *slice = oasp_ai_tensor_slice(source, 0, 1, 4, 2, &error);
    loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, slice, -1, 0, &error);
    assert(loss && oasp_ai_tensor_backward(loss, NULL, 0, &error) == 0);
    gradient = oasp_ai_tensor_gradient(source);
    assert(gradient);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[0], 0);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[1], 1);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[2], 0);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[3], 1);
    oasp_ai_tensor_release(gradient);
    oasp_ai_tensor_release(loss);
    oasp_ai_tensor_release(slice);
    oasp_ai_tensor_release(source);
}

static void test_checkpoint_round_trip(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    int64_t shape[] = {2};
    oasp_ai_tensor *parameter = oasp_ai_tensor_full(context, shape, 1, OASP_AI_DTYPE_F32, 1, &error);
    oasp_ai_module *module = oasp_ai_module_create(context, "Checkpoint", &error);
    assert(parameter && module && oasp_ai_module_register_parameter(module, "weight", parameter, &error) == 0);
    oasp_ai_optimizer *optimizer = oasp_ai_optimizer_create(module, OASP_AI_OPTIMIZER_ADAMW, 0.01, 0.1, &error);
    oasp_ai_scheduler *scheduler = oasp_ai_scheduler_create(optimizer, OASP_AI_SCHEDULER_STEP, 100, 10, 0.5, &error);
    oasp_ai_loss_scaler *scaler = oasp_ai_loss_scaler_create(1024, 2, 0.5, 8, &error);
    oasp_ai_tensor *loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, parameter, -1, 0, &error);
    assert(optimizer && scheduler && scaler && loss && oasp_ai_tensor_backward(loss, NULL, 0, &error) == 0 &&
           oasp_ai_optimizer_step(optimizer, &error) == 0 && oasp_ai_scheduler_step(scheduler, &error) == 0);
    float saved = ((const float *)oasp_ai_tensor_const_data(parameter))[0];
    const char metadata[] = "epoch=7";
    char path[128];
    snprintf(path, sizeof(path), "/tmp/openasp-ai-checkpoint-%ld.oaspckpt", (long)getpid());
    assert(oasp_ai_save_checkpoint(path, module, optimizer, metadata, sizeof(metadata) - 1, &error) == 0);
    ((float *)oasp_ai_tensor_data(parameter))[0] = 99;
    void *loaded_metadata = NULL;
    size_t loaded_size = 0;
    assert(oasp_ai_load_checkpoint(path, module, optimizer, &loaded_metadata, &loaded_size, &error) == 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], saved);
    assert(loaded_size == sizeof(metadata) - 1 && !memcmp(loaded_metadata, metadata, loaded_size));
    free(loaded_metadata);

    oasp_ai_checkpoint_state full = {
        .scheduler = scheduler,
        .loss_scaler = scaler,
        .metadata = metadata,
        .metadata_size = sizeof(metadata) - 1,
        .epoch = 7,
        .global_step = 19,
    };
    assert(oasp_ai_save_checkpoint_full(path, module, optimizer, &full, &error) == 0);
    for (int index = 0; index < 4; index++)
        assert(oasp_ai_scheduler_step(scheduler, &error) == 0);
    oasp_ai_checkpoint_state restored = {.scheduler = scheduler, .loss_scaler = scaler};
    assert(oasp_ai_load_checkpoint_full(path, module, optimizer, &restored, 1, &error) == 0);
    assert(restored.epoch == 7 && restored.global_step == 19 && oasp_ai_scheduler_current_step(scheduler) == 1);
    assert(restored.metadata_size == sizeof(metadata) - 1 && !memcmp(restored.metadata, metadata, restored.metadata_size));
    oasp_ai_checkpoint_state_clear(&restored);

    FILE *checkpoint = fopen(path, "r+b");
    assert(checkpoint && fseek(checkpoint, 0, SEEK_END) == 0);
    long checkpoint_size = ftell(checkpoint);
    assert(checkpoint_size > 32 && fseek(checkpoint, 0, SEEK_SET) == 0);
    unsigned char *checkpoint_bytes = malloc((size_t)checkpoint_size);
    assert(checkpoint_bytes && fread(checkpoint_bytes, 1, (size_t)checkpoint_size, checkpoint) == (size_t)checkpoint_size);
    size_t weight_occurrences = 0;
    for (size_t index = 0; index + 6 <= (size_t)checkpoint_size - 32; index++)
        if (!memcmp(checkpoint_bytes + index, "weight", 6) && ++weight_occurrences == 2) {
            checkpoint_bytes[index] = 'x';
            break;
        }
    assert(weight_occurrences == 2);
    assert(oasp_ai_sha256(checkpoint_bytes, (size_t)checkpoint_size - 32,
                          checkpoint_bytes + checkpoint_size - 32) == 0);
    assert(fseek(checkpoint, 0, SEEK_SET) == 0 &&
           fwrite(checkpoint_bytes, 1, (size_t)checkpoint_size, checkpoint) == (size_t)checkpoint_size &&
           fclose(checkpoint) == 0);
    free(checkpoint_bytes);

    ((float *)oasp_ai_tensor_data(parameter))[0] = 77;
    optimizer->step = 123;
    optimizer->learning_rate = 0.321;
    scheduler->step = 9;
    scaler->scale = 333;
    context->rng_counter = 456;
    assert(oasp_ai_load_checkpoint_full(path, module, optimizer, &restored, 1, &error) != 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 77);
    near(optimizer->learning_rate, 0.321);
    assert(optimizer->step == 123 && scheduler->step == 9 && scaler->scale == 333 && context->rng_counter == 456);

    unlink(path);
    oasp_ai_tensor_release(loss);
    oasp_ai_loss_scaler_release(scaler);
    oasp_ai_scheduler_release(scheduler);
    oasp_ai_optimizer_release(optimizer);
    oasp_ai_module_release(module);
    oasp_ai_tensor_release(parameter);
}

static void test_graph_round_trip(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    oasp_ai_tensor_desc desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    oasp_ai_graph *graph = oasp_ai_graph_create(context, "AddGraph", &error);
    uint32_t left = oasp_ai_graph_input(graph, "left", &desc, &error);
    uint32_t right = oasp_ai_graph_input(graph, "right", &desc, &error);
    uint32_t inputs[] = {left, right};
    uint32_t output = oasp_ai_graph_operation(graph, OASP_AI_OP_ADD, inputs, 2, NULL, 0, &error);
    assert(output && oasp_ai_graph_set_outputs(graph, &output, 1, &error) == 0);
    oasp_ai_plan *plan = oasp_ai_graph_compile(graph, "cpu:0", &error);
    assert(plan);
    oasp_ai_plan *cached_plan = oasp_ai_graph_compile(graph, "cpu:0", &error);
    assert(cached_plan == plan && oasp_ai_context_metrics(context)->graph_cache_hits > 0);
    oasp_ai_plan_release(cached_plan);

    float left_data[] = {1, 2, 3, 4};
    float right_data[] = {4, 3, 2, 1};
    oasp_ai_tensor *left_tensor = oasp_ai_tensor_create(context, &desc, left_data, sizeof(left_data), &error);
    oasp_ai_tensor *right_tensor = oasp_ai_tensor_create(context, &desc, right_data, sizeof(right_data), &error);
    oasp_ai_tensor *plan_inputs[] = {left_tensor, right_tensor};
    oasp_ai_tensor **outputs = NULL;
    size_t output_count = 0;
    assert(oasp_ai_plan_execute(plan, plan_inputs, 2, &outputs, &output_count, &error) == 0);
    assert(output_count == 1);
    for (size_t index = 0; index < 4; index++)
        near(((const float *)oasp_ai_tensor_const_data(outputs[0]))[index], 5);
    oasp_ai_tensor_release(outputs[0]);
    free(outputs);

    assert(oasp_ai_context_register_backend(context, oasp_ai_test_mock_backend(), &error) == 0);
    oasp_ai_plan *mock_plan = oasp_ai_graph_compile(graph, "mock:0", &error);
    outputs = NULL;
    output_count = 0;
    assert(mock_plan && oasp_ai_plan_execute(mock_plan, plan_inputs, 2, &outputs, &output_count, &error) == 0);
    assert(output_count == 1);
    float mock_values[4];
    assert(oasp_ai_tensor_copy_to_host(outputs[0], mock_values, sizeof(mock_values), &error) == 0);
    for (size_t index = 0; index < 4; index++)
        near(mock_values[index], 5);
    oasp_ai_tensor_release(outputs[0]);
    free(outputs);
    oasp_ai_plan_release(mock_plan);

    oasp_ai_tensor *mock_left = oasp_ai_tensor_to_device(left_tensor, "mock:0", &error);
    oasp_ai_tensor *mock_right = oasp_ai_tensor_to_device(right_tensor, "mock:0", &error);
    oasp_ai_tensor *mock_sum = oasp_ai_binary(context, OASP_AI_OP_ADD, mock_left, mock_right, &error);
    char device[32];
    assert(mock_left && mock_right && mock_sum && oasp_ai_tensor_device(mock_sum, device, sizeof(device)) == 0);
    assert(!strcmp(device, "mock:0"));
    assert(oasp_ai_tensor_copy_to_host(mock_sum, mock_values, sizeof(mock_values), &error) == 0);
    for (size_t index = 0; index < 4; index++)
        near(mock_values[index], 5);
    oasp_ai_module *mock_module = oasp_ai_module_create(context, "DeviceWeights", &error);
    assert(mock_module && oasp_ai_module_register_parameter(mock_module, "weight", mock_left, &error) == 0);
    float gradient_data[] = {3, 4, 0, 0};
    oasp_ai_tensor *host_gradient = oasp_ai_tensor_create(context, &desc, gradient_data, sizeof(gradient_data), &error);
    mock_left->grad = oasp_ai_tensor_to_device(host_gradient, "mock:0", &error);
    oasp_ai_optimizer *mock_optimizer = oasp_ai_optimizer_create(mock_module, OASP_AI_OPTIMIZER_SGD, 0.1, 0, &error);
    assert(host_gradient && mock_left->grad && mock_optimizer &&
           oasp_ai_optimizer_clip_grad_norm(mock_optimizer, 1, &error) == 0);
    oasp_ai_tensor *clipped_gradient = oasp_ai_tensor_gradient(mock_left);
    assert(clipped_gradient &&
           oasp_ai_tensor_copy_to_host(clipped_gradient, mock_values, sizeof(mock_values), &error) == 0);
    near(mock_values[0], 0.6);
    near(mock_values[1], 0.8);
    near(mock_values[2], 0);
    near(mock_values[3], 0);
    oasp_ai_tensor_release(clipped_gradient);
    oasp_ai_optimizer_release(mock_optimizer);
    oasp_ai_tensor_release(host_gradient);

    gradient_data[0] = NAN;
    host_gradient = oasp_ai_tensor_create(context, &desc, gradient_data, sizeof(gradient_data), &error);
    oasp_ai_tensor_release(mock_left->grad);
    mock_left->grad = oasp_ai_tensor_to_device(host_gradient, "mock:0", &error);
    mock_optimizer = oasp_ai_optimizer_create(mock_module, OASP_AI_OPTIMIZER_SGD, 0.1, 0, &error);
    assert(host_gradient && mock_left->grad && mock_optimizer &&
           oasp_ai_optimizer_clip_grad_norm(mock_optimizer, 1, &error) != 0);
    assert(strstr(error.message, "not finite"));
    oasp_ai_optimizer_release(mock_optimizer);
    oasp_ai_tensor_release(host_gradient);

    char device_weights_path[128];
    snprintf(device_weights_path, sizeof(device_weights_path), "/tmp/openasp-ai-device-weights-%ld.oaspweights", (long)getpid());
    assert(oasp_ai_module_save_weights(mock_module, device_weights_path, &error) == 0);
    assert(oasp_ai_module_load_weights(mock_module, device_weights_path, 1, &error) == 0);
    unlink(device_weights_path);
    oasp_ai_module_release(mock_module);
    oasp_ai_tensor_release(mock_sum);
    oasp_ai_tensor_release(mock_right);
    oasp_ai_tensor_release(mock_left);

    assert(oasp_ai_context_profiler_start(context, 1, &error) == 0);
    oasp_ai_event_handle *event = oasp_ai_plan_execute_async(plan, plan_inputs, 2, &error);
    assert(event && oasp_ai_event_wait(event, &error) == 0 && oasp_ai_event_ready(event));
    outputs = NULL;
    output_count = 0;
    assert(oasp_ai_event_outputs(event, &outputs, &output_count, &error) == 0 && output_count == 1);
    for (size_t index = 0; index < 4; index++)
        near(((const float *)oasp_ai_tensor_const_data(outputs[0]))[index], 5);
    oasp_ai_tensor_release(outputs[0]);
    free(outputs);
    oasp_ai_event_release(event);
    assert(oasp_ai_context_profiler_stop(context, &error) == 0);
    char trace_path[128];
    snprintf(trace_path, sizeof(trace_path), "/tmp/openasp-ai-trace-%ld.json", (long)getpid());
    assert(oasp_ai_context_profiler_export_chrome_trace(context, trace_path, &error) == 0);
    FILE *trace_file = fopen(trace_path, "rb");
    char trace_header[16] = {0};
    assert(trace_file && fread(trace_header, 1, 15, trace_file) == 15 && fclose(trace_file) == 0);
    assert(!memcmp(trace_header, "{\"traceEvents\":", 15));
    unlink(trace_path);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/openasp-ai-model-%ld.oaspmodel", (long)getpid());
    assert(oasp_ai_graph_save(graph, path, &error) == 0);
    oasp_ai_graph *loaded = oasp_ai_graph_load(context, path, &error);
    assert(loaded && oasp_ai_graph_verify(loaded, &error) == 0);
    FILE *file = fopen(path, "r+b");
    assert(file && fseek(file, 80, SEEK_SET) == 0);
    int byte = fgetc(file);
    assert(byte != EOF && fseek(file, -1, SEEK_CUR) == 0 && fputc(byte ^ 1, file) != EOF && fclose(file) == 0);
    oasp_ai_graph *corrupted = oasp_ai_graph_load(context, path, &error);
    assert(!corrupted);
    unlink(path);

    snprintf(path, sizeof(path), "/tmp/openasp-ai-model-%ld.onnx", (long)getpid());
    assert(oasp_ai_graph_export_onnx(graph, path, &error) == 0);
    oasp_ai_graph *onnx = oasp_ai_graph_import_onnx(context, path, &error);
    assert(onnx && oasp_ai_graph_verify(onnx, &error) == 0);
    oasp_ai_plan *onnx_plan = oasp_ai_graph_compile(onnx, "cpu:0", &error);
    outputs = NULL;
    output_count = 0;
    assert(onnx_plan && oasp_ai_plan_execute(onnx_plan, plan_inputs, 2, &outputs, &output_count, &error) == 0);
    assert(output_count == 1);
    for (size_t index = 0; index < 4; index++)
        near(((const float *)oasp_ai_tensor_const_data(outputs[0]))[index], 5);
    oasp_ai_tensor_release(outputs[0]);
    free(outputs);
    oasp_ai_plan_release(onnx_plan);
    oasp_ai_graph_release(onnx);
    unlink(path);

    oasp_ai_graph_release(loaded);
    oasp_ai_tensor_release(right_tensor);
    oasp_ai_tensor_release(left_tensor);
    oasp_ai_plan_release(plan);
    oasp_ai_graph_release(graph);
}

static void test_attention_cache_and_sampling(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    oasp_ai_tensor_desc desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 4, .shape = {1, 1, 2, 2}};
    float data[] = {1, 0, 0, 1};
    oasp_ai_tensor *query = oasp_ai_tensor_create(context, &desc, data, sizeof(data), &error);
    oasp_ai_tensor *output = oasp_ai_attention(context, query, query, query, NULL, 1, &error);
    assert(output && oasp_ai_tensor_elements(output) == 4);
    oasp_ai_kv_cache *cache = oasp_ai_kv_cache_create(context, 2, 1, 1, 2, 16, OASP_AI_DTYPE_F32, &error);
    assert(cache && oasp_ai_kv_cache_append(cache, 0, query, query, &error) == 0);
    assert(oasp_ai_kv_cache_length(cache) == 2);

    oasp_ai_tensor_desc logits_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {3}};
    float logits_data[] = {1, 5, 2};
    oasp_ai_tensor *logits = oasp_ai_tensor_create(context, &logits_desc, logits_data, sizeof(logits_data), &error);
    int64_t token = -1;
    assert(oasp_ai_sample(context, logits, 0, 0, 1, NULL, 0, 1, &token, &error) == 0 && token == 1);
    oasp_ai_tensor_release(logits);
    oasp_ai_kv_cache_release(cache);
    oasp_ai_tensor_release(output);
    oasp_ai_tensor_release(query);
}

static void test_data_pipeline_and_tokenizer(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    oasp_ai_tensor_desc input_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {6, 2}};
    oasp_ai_tensor_desc label_desc = {.dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {6}};
    float input_data[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    int64_t label_data[] = {0, 1, 2, 3, 4, 5};
    oasp_ai_tensor *inputs = oasp_ai_tensor_create(context, &input_desc, input_data, sizeof(input_data), &error);
    oasp_ai_tensor *labels = oasp_ai_tensor_create(context, &label_desc, label_data, sizeof(label_data), &error);
    oasp_ai_dataset *dataset = oasp_ai_dataset_from_tensors(inputs, labels, &error);
    oasp_ai_dataloader_options options = {
        .batch_size = 2, .seed = 77, .workers = 3, .prefetch_batches = 2, .shuffle = 1, .drop_last = 0,
    };
    oasp_ai_dataloader *loader = oasp_ai_dataloader_create(dataset, &options, &error);
    oasp_ai_data_iterator *iterator = oasp_ai_dataloader_iterator(loader, 4, &error);
    assert(dataset && loader && iterator && oasp_ai_dataset_size(dataset) == 6);
    uint64_t samples = 0;
    for (;;) {
        oasp_ai_batch *batch = NULL;
        int status = oasp_ai_data_iterator_next(iterator, &batch, &error);
        assert(status >= 0);
        if (!status)
            break;
        assert(oasp_ai_batch_size(batch) == 2);
        oasp_ai_tensor *batch_inputs = oasp_ai_batch_inputs(batch);
        oasp_ai_tensor *batch_labels = oasp_ai_batch_labels(batch);
        assert(batch_inputs && batch_labels && oasp_ai_tensor_descriptor(batch_inputs)->shape[0] == 2 &&
               oasp_ai_tensor_descriptor(batch_labels)->shape[0] == 2);
        samples += oasp_ai_batch_size(batch);
        oasp_ai_tensor_release(batch_inputs);
        oasp_ai_tensor_release(batch_labels);
        oasp_ai_batch_release(batch);
    }
    assert(samples == 6 && oasp_ai_data_iterator_cursor(iterator) == 6);
    oasp_ai_data_iterator_release(iterator);
    oasp_ai_dataloader_release(loader);
    oasp_ai_dataset_release(dataset);
    oasp_ai_tensor_release(labels);
    oasp_ai_tensor_release(inputs);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/openasp-ai-vocab-%ld.txt", (long)getpid());
    FILE *file = fopen(path, "wb");
    assert(file && fwrite("hello\n \nworld\n", 1, 14, file) == 14 && fclose(file) == 0);
    oasp_ai_tokenizer *tokenizer = oasp_ai_tokenizer_load(path, 1, &error);
    int64_t *tokens = NULL;
    size_t token_count = 0;
    assert(tokenizer && oasp_ai_tokenizer_encode(tokenizer, "hello world", 11, &tokens, &token_count, &error) == 0);
    assert(token_count == 3);
    char *decoded = NULL;
    size_t decoded_size = 0;
    assert(oasp_ai_tokenizer_decode(tokenizer, tokens, token_count, &decoded, &decoded_size, &error) == 0);
    assert(decoded_size == 11 && !memcmp(decoded, "hello world", 11));
    free(decoded);
    free(tokens);
    oasp_ai_tokenizer_release(tokenizer);
    unlink(path);
}

static void test_quantization_scheduler_and_loss_scaler(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    oasp_ai_tensor_desc desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    float data[] = {-2, -1, 1, 2};
    oasp_ai_tensor *tensor = oasp_ai_tensor_create(context, &desc, data, sizeof(data), &error);
    oasp_ai_tensor *quantized = oasp_ai_tensor_quantize(tensor, 4, 1, "absmax", &error);
    oasp_ai_quantization_info info;
    assert(quantized && oasp_ai_tensor_quantization(quantized, &info) == 0);
    assert(info.enabled && info.bits == 4 && info.axis == 1 && info.channel_count == 2);

    oasp_ai_module *quantized_module = oasp_ai_module_create(context, "Quantized", &error);
    assert(quantized_module && oasp_ai_module_register_buffer(quantized_module, "weight", quantized, 1, &error) == 0);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/openasp-ai-quantized-%ld.oaspweights", (long)getpid());
    assert(oasp_ai_module_save_weights(quantized_module, path, &error) == 0);
    oasp_ai_tensor *replacement_source = oasp_ai_tensor_full(context, desc.shape, desc.rank, OASP_AI_DTYPE_F32, 1, &error);
    oasp_ai_tensor *replacement = oasp_ai_tensor_quantize(replacement_source, 8, -1, "minmax", &error);
    oasp_ai_module *replacement_module = oasp_ai_module_create(context, "Quantized", &error);
    assert(replacement_source && replacement && replacement_module &&
           oasp_ai_module_register_buffer(replacement_module, "weight", replacement, 1, &error) == 0);
    assert(oasp_ai_module_load_weights(replacement_module, path, 1, &error) == 0);
    assert(oasp_ai_tensor_quantization(replacement, &info) == 0 && info.enabled && info.bits == 4 &&
           info.axis == 1 && info.channel_count == 2 && !strcmp(info.calibration_method, "absmax"));
    unlink(path);

    oasp_ai_graph *quantized_graph = oasp_ai_graph_create(context, "QuantizedGraph", &error);
    oasp_ai_tensor_desc input_desc = {.dtype = OASP_AI_DTYPE_I8, .rank = 2, .shape = {2, 2}};
    uint32_t input = oasp_ai_graph_input(quantized_graph, "input", &input_desc, &error);
    uint32_t constant = oasp_ai_graph_constant(quantized_graph, "weight", quantized, &error);
    uint32_t add_inputs[] = {input, constant};
    uint32_t output = oasp_ai_graph_operation(quantized_graph, OASP_AI_OP_ADD, add_inputs, 2, NULL, 0, &error);
    assert(output && oasp_ai_graph_set_outputs(quantized_graph, &output, 1, &error) == 0);
    snprintf(path, sizeof(path), "/tmp/openasp-ai-quantized-%ld.oaspmodel", (long)getpid());
    assert(oasp_ai_graph_save(quantized_graph, path, &error) == 0);
    oasp_ai_graph *loaded_graph = oasp_ai_graph_load(context, path, &error);
    assert(loaded_graph);
    assert(loaded_graph->value_count == 3 && loaded_graph->values[1].constant);
    assert(oasp_ai_tensor_quantization(loaded_graph->values[1].constant, &info) == 0 &&
           info.enabled && info.bits == 4 && info.axis == 1 && info.channel_count == 2);
    oasp_ai_graph_release(loaded_graph);
    oasp_ai_graph_release(quantized_graph);
    unlink(path);

    oasp_ai_tensor *dequantized = oasp_ai_tensor_dequantize(quantized, OASP_AI_DTYPE_F32, &error);
    assert(dequantized);
    for (size_t index = 0; index < 4; index++)
        assert(fabs(((const float *)oasp_ai_tensor_const_data(dequantized))[index] - data[index]) < 0.3);

    int64_t shape[] = {1};
    oasp_ai_tensor *parameter = oasp_ai_tensor_full(context, shape, 1, OASP_AI_DTYPE_F32, 1, &error);
    oasp_ai_module *module = oasp_ai_module_create(context, "Scaled", &error);
    assert(parameter && module && oasp_ai_module_register_parameter(module, "weight", parameter, &error) == 0);
    oasp_ai_optimizer *optimizer = oasp_ai_optimizer_create(module, OASP_AI_OPTIMIZER_SGD, 0.1, 0, &error);
    oasp_ai_scheduler *scheduler = oasp_ai_scheduler_create(optimizer, OASP_AI_SCHEDULER_STEP, 10, 2, 0.5, &error);
    oasp_ai_loss_scaler *scaler = oasp_ai_loss_scaler_create(8, 2, 0.5, 2, &error);
    oasp_ai_tensor *loss = oasp_ai_reduce(context, OASP_AI_OP_SUM, parameter, -1, 0, &error);
    oasp_ai_tensor *scaled = oasp_ai_loss_scaler_scale_loss(scaler, loss, &error);
    assert(optimizer && scheduler && scaler && scaled && oasp_ai_tensor_backward(scaled, NULL, 0, &error) == 0);
    assert(oasp_ai_loss_scaler_step(scaler, optimizer, &error) == 0);
    near(((const float *)oasp_ai_tensor_const_data(parameter))[0], 0.9);
    assert(oasp_ai_scheduler_step(scheduler, &error) == 0 && oasp_ai_scheduler_step(scheduler, &error) == 0);
    near(oasp_ai_optimizer_learning_rate(optimizer), 0.05);

    oasp_ai_tensor_release(scaled);
    oasp_ai_tensor_release(loss);
    oasp_ai_loss_scaler_release(scaler);
    oasp_ai_scheduler_release(scheduler);
    oasp_ai_optimizer_release(optimizer);
    oasp_ai_module_release(module);
    oasp_ai_tensor_release(parameter);
    oasp_ai_tensor_release(dequantized);
    oasp_ai_module_release(replacement_module);
    oasp_ai_tensor_release(replacement);
    oasp_ai_tensor_release(replacement_source);
    oasp_ai_module_release(quantized_module);
    oasp_ai_tensor_release(quantized);
    oasp_ai_tensor_release(tensor);
}

static void test_loss_functions(oasp_ai_context *context) {
    oasp_ai_error error = {0};
    oasp_ai_tensor_desc vector_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {2}};
    float input_values[] = {0.2f, 0.8f};
    float target_values[] = {0, 1};
    oasp_ai_tensor *input = oasp_ai_tensor_create(context, &vector_desc, input_values,
                                                  sizeof(input_values), &error);
    oasp_ai_tensor *target = oasp_ai_tensor_create(context, &vector_desc, target_values,
                                                   sizeof(target_values), &error);
    assert(input && target && oasp_ai_tensor_requires_grad(input, 1, &error) == 0);

    oasp_ai_tensor *mse = oasp_ai_mse_loss(context, input, target, OASP_AI_REDUCTION_MEAN, &error);
    assert(mse);
    near(((const float *)oasp_ai_tensor_const_data(mse))[0], 0.04);
    assert(oasp_ai_tensor_backward(mse, NULL, 0, &error) == 0);
    oasp_ai_tensor *gradient = oasp_ai_tensor_gradient(input);
    assert(gradient);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[0], 0.2);
    near(((const float *)oasp_ai_tensor_const_data(gradient))[1], -0.2);
    oasp_ai_tensor_release(gradient);

    oasp_ai_tensor *l1 = oasp_ai_l1_loss(context, input, target, OASP_AI_REDUCTION_MEAN, &error);
    oasp_ai_tensor *bce = oasp_ai_bce_loss(context, input, target, OASP_AI_REDUCTION_MEAN, &error);
    assert(l1 && bce);
    near(((const float *)oasp_ai_tensor_const_data(l1))[0], 0.2);
    near(((const float *)oasp_ai_tensor_const_data(bce))[0], -log(0.8));

    float logits_values[] = {0, 2};
    oasp_ai_tensor *logits = oasp_ai_tensor_create(context, &vector_desc, logits_values,
                                                   sizeof(logits_values), &error);
    oasp_ai_tensor *bce_logits = oasp_ai_bce_with_logits_loss(
        context, logits, target, OASP_AI_REDUCTION_MEAN, &error);
    assert(bce_logits);
    near(((const float *)oasp_ai_tensor_const_data(bce_logits))[0],
         (log(2.0) + log1p(exp(-2.0))) / 2.0);

    float log_probability_values[] = {(float)log(0.4), (float)log(0.6)};
    float distribution_values[] = {0.5f, 0.5f};
    oasp_ai_tensor *log_probability = oasp_ai_tensor_create(
        context, &vector_desc, log_probability_values, sizeof(log_probability_values), &error);
    oasp_ai_tensor *distribution = oasp_ai_tensor_create(
        context, &vector_desc, distribution_values, sizeof(distribution_values), &error);
    oasp_ai_tensor *kl = oasp_ai_kl_div_loss(
        context, log_probability, distribution, OASP_AI_REDUCTION_SUM, 0, &error);
    assert(kl);
    near(((const float *)oasp_ai_tensor_const_data(kl))[0],
         0.5 * log(0.5 / 0.4) + 0.5 * log(0.5 / 0.6));

    float huber_input_values[] = {0, 3};
    float zero_values[] = {0, 0};
    oasp_ai_tensor *huber_input = oasp_ai_tensor_create(
        context, &vector_desc, huber_input_values, sizeof(huber_input_values), &error);
    oasp_ai_tensor *zeros = oasp_ai_tensor_create(
        context, &vector_desc, zero_values, sizeof(zero_values), &error);
    oasp_ai_tensor *huber = oasp_ai_huber_loss(
        context, huber_input, zeros, 1, OASP_AI_REDUCTION_MEAN, &error);
    assert(huber);
    near(((const float *)oasp_ai_tensor_const_data(huber))[0], 1.25);

    oasp_ai_tensor_desc matrix_desc = {.dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 2}};
    oasp_ai_tensor_desc label_desc = {.dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {2}};
    float nll_input_values[] = {-0.1f, -2, -3, -0.2f};
    int64_t label_values[] = {0, 1};
    oasp_ai_tensor *nll_input = oasp_ai_tensor_create(
        context, &matrix_desc, nll_input_values, sizeof(nll_input_values), &error);
    oasp_ai_tensor *labels = oasp_ai_tensor_create(
        context, &label_desc, label_values, sizeof(label_values), &error);
    assert(nll_input && labels && oasp_ai_tensor_requires_grad(nll_input, 1, &error) == 0);
    oasp_ai_tensor *nll = oasp_ai_nll_loss(
        context, nll_input, labels, OASP_AI_REDUCTION_MEAN, &error);
    assert(nll);
    near(((const float *)oasp_ai_tensor_const_data(nll))[0], 0.15);
    assert(oasp_ai_tensor_backward(nll, NULL, 0, &error) == 0);
    gradient = oasp_ai_tensor_gradient(nll_input);
    assert(gradient);
    const float *nll_gradient = oasp_ai_tensor_const_data(gradient);
    near(nll_gradient[0], -0.5);
    near(nll_gradient[1], 0);
    near(nll_gradient[2], 0);
    near(nll_gradient[3], -0.5);

    float positive_weight_values[] = {3, 3};
    oasp_ai_tensor *positive_weight = oasp_ai_tensor_create(
        context, &vector_desc, positive_weight_values, sizeof(positive_weight_values), &error);
    float binary_logits_values[] = {0, 0};
    oasp_ai_tensor *binary_logits = oasp_ai_tensor_create(
        context, &vector_desc, binary_logits_values, sizeof(binary_logits_values), &error);
    oasp_ai_tensor *weighted_bce = oasp_ai_bce_with_logits_loss_ex(
        context, binary_logits, target, positive_weight, OASP_AI_REDUCTION_MEAN, &error);
    assert(weighted_bce);
    near(((const float *)oasp_ai_tensor_const_data(weighted_bce))[0], 2 * log(2.0));

    oasp_ai_tensor *smooth_l1 = oasp_ai_smooth_l1_loss(
        context, huber_input, zeros, 1, OASP_AI_REDUCTION_MEAN, &error);
    assert(smooth_l1);
    near(((const float *)oasp_ai_tensor_const_data(smooth_l1))[0], 1.25);
    oasp_ai_tensor_release(smooth_l1);

    oasp_ai_tensor *focal = oasp_ai_focal_loss(
        context, binary_logits, target, 0.25, 2, OASP_AI_REDUCTION_MEAN, &error);
    assert(focal);
    near(((const float *)oasp_ai_tensor_const_data(focal))[0],
         log(2.0) * 0.125);
    oasp_ai_tensor_release(focal);
    oasp_ai_tensor_release(weighted_bce);
    oasp_ai_tensor_release(binary_logits);
    oasp_ai_tensor_release(positive_weight);

    float dice_input_values[] = {1, 0};
    float dice_target_values[] = {1, 1};
    oasp_ai_tensor *dice_input = oasp_ai_tensor_create(
        context, &vector_desc, dice_input_values, sizeof(dice_input_values), &error);
    oasp_ai_tensor *dice_target = oasp_ai_tensor_create(
        context, &vector_desc, dice_target_values, sizeof(dice_target_values), &error);
    oasp_ai_tensor *dice = oasp_ai_dice_loss(
        context, dice_input, dice_target, 1, 1e-7, 0, OASP_AI_REDUCTION_MEAN, &error);
    assert(dice);
    near(((const float *)oasp_ai_tensor_const_data(dice))[0], 0.25);
    oasp_ai_tensor_release(dice);
    oasp_ai_tensor_release(dice_target);
    oasp_ai_tensor_release(dice_input);

    float identity_values[] = {1, 0, 0, 1};
    oasp_ai_tensor *query = oasp_ai_tensor_create(
        context, &matrix_desc, identity_values, sizeof(identity_values), &error);
    oasp_ai_tensor *key = oasp_ai_tensor_create(
        context, &matrix_desc, identity_values, sizeof(identity_values), &error);
    oasp_ai_tensor *info_nce = oasp_ai_info_nce_loss(
        context, query, key, 1, 1, OASP_AI_REDUCTION_MEAN, &error);
    assert(info_nce);
    near(((const float *)oasp_ai_tensor_const_data(info_nce))[0], log1p(exp(-1.0)));
    oasp_ai_tensor_release(info_nce);
    oasp_ai_tensor_release(key);
    oasp_ai_tensor_release(query);

    oasp_ai_tensor_desc weighted_logits_desc = {
        .dtype = OASP_AI_DTYPE_F32, .rank = 2, .shape = {2, 3},
    };
    oasp_ai_tensor_desc class_weight_desc = {
        .dtype = OASP_AI_DTYPE_F32, .rank = 1, .shape = {3},
    };
    float weighted_logits_values[] = {2, 0, -1, 0, 1, 2};
    int64_t weighted_label_values[] = {0, -100};
    float class_weight_values[] = {1, 2, 3};
    oasp_ai_tensor *weighted_logits = oasp_ai_tensor_create(
        context, &weighted_logits_desc, weighted_logits_values,
        sizeof(weighted_logits_values), &error);
    oasp_ai_tensor *weighted_labels = oasp_ai_tensor_create(
        context, &label_desc, weighted_label_values, sizeof(weighted_label_values), &error);
    oasp_ai_tensor *class_weight = oasp_ai_tensor_create(
        context, &class_weight_desc, class_weight_values, sizeof(class_weight_values), &error);
    assert(weighted_logits && weighted_labels && class_weight &&
           oasp_ai_tensor_requires_grad(weighted_logits, 1, &error) == 0);
    oasp_ai_tensor *smoothed_cross_entropy = oasp_ai_cross_entropy_ex(
        context, weighted_logits, weighted_labels, class_weight, -100, 0.1,
        OASP_AI_REDUCTION_MEAN, &error);
    assert(smoothed_cross_entropy);
    near(((const float *)oasp_ai_tensor_const_data(smoothed_cross_entropy))[0], 0.6201636);
    assert(oasp_ai_tensor_backward(smoothed_cross_entropy, NULL, 0, &error) == 0);
    oasp_ai_tensor_release(smoothed_cross_entropy);
    oasp_ai_tensor_release(class_weight);
    oasp_ai_tensor_release(weighted_labels);
    oasp_ai_tensor_release(weighted_logits);

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
        (float)log(0.6), (float)log(0.4),
        (float)log(0.5), (float)log(0.5),
    };
    int64_t ctc_target_values[] = {1};
    int64_t ctc_input_length_values[] = {2};
    int64_t ctc_target_length_values[] = {1};
    oasp_ai_tensor *ctc_log = oasp_ai_tensor_create(
        context, &ctc_log_desc, ctc_log_values, sizeof(ctc_log_values), &error);
    oasp_ai_tensor *ctc_target = oasp_ai_tensor_create(
        context, &ctc_target_desc, ctc_target_values, sizeof(ctc_target_values), &error);
    oasp_ai_tensor *ctc_input_length = oasp_ai_tensor_create(
        context, &ctc_length_desc, ctc_input_length_values,
        sizeof(ctc_input_length_values), &error);
    oasp_ai_tensor *ctc_target_length = oasp_ai_tensor_create(
        context, &ctc_length_desc, ctc_target_length_values,
        sizeof(ctc_target_length_values), &error);
    assert(ctc_log && ctc_target && ctc_input_length && ctc_target_length &&
           oasp_ai_tensor_requires_grad(ctc_log, 1, &error) == 0);
    oasp_ai_tensor *ctc = oasp_ai_ctc_loss(
        context, ctc_log, ctc_target, ctc_input_length, ctc_target_length,
        0, 0, OASP_AI_REDUCTION_MEAN, &error);
    assert(ctc);
    near(((const float *)oasp_ai_tensor_const_data(ctc))[0], -log(0.7));
    assert(oasp_ai_tensor_backward(ctc, NULL, 0, &error) == 0);
    oasp_ai_tensor *ctc_gradient = oasp_ai_tensor_gradient(ctc_log);
    assert(ctc_gradient);
    const float *ctc_gradient_values = oasp_ai_tensor_const_data(ctc_gradient);
    near(ctc_gradient_values[0], 0.6 - 3.0 / 7.0);
    near(ctc_gradient_values[1], 0.4 - 4.0 / 7.0);
    near(ctc_gradient_values[2], 0.5 - 2.0 / 7.0);
    near(ctc_gradient_values[3], 0.5 - 5.0 / 7.0);

    oasp_ai_tensor_release(ctc_gradient);
    oasp_ai_tensor_release(ctc);
    oasp_ai_tensor_release(ctc_target_length);
    oasp_ai_tensor_release(ctc_input_length);
    oasp_ai_tensor_release(ctc_target);
    oasp_ai_tensor_release(ctc_log);
    oasp_ai_tensor_release(gradient);
    oasp_ai_tensor_release(nll);
    oasp_ai_tensor_release(labels);
    oasp_ai_tensor_release(nll_input);
    oasp_ai_tensor_release(huber);
    oasp_ai_tensor_release(zeros);
    oasp_ai_tensor_release(huber_input);
    oasp_ai_tensor_release(kl);
    oasp_ai_tensor_release(distribution);
    oasp_ai_tensor_release(log_probability);
    oasp_ai_tensor_release(bce_logits);
    oasp_ai_tensor_release(logits);
    oasp_ai_tensor_release(bce);
    oasp_ai_tensor_release(l1);
    oasp_ai_tensor_release(mse);
    oasp_ai_tensor_release(target);
    oasp_ai_tensor_release(input);
}

int main(void) {
    oasp_ai_error error = {0};
    oasp_ai_context_options options = {.seed = 42, .maximum_tensor_bytes = 64 * 1024 * 1024, .training_enabled = 1};
    oasp_ai_context *context = oasp_ai_context_create(&options, &error);
    assert(context);
    test_tensor_and_autograd(context);
    test_module_optimizer_and_weights(context);
    test_view_softmax_and_broadcast_gradients(context);
    test_checkpoint_round_trip(context);
    test_graph_round_trip(context);
    test_attention_cache_and_sampling(context);
    test_data_pipeline_and_tokenizer(context);
    test_quantization_scheduler_and_loss_scaler(context);
    test_loss_functions(context);
    assert(oasp_ai_context_metrics(context)->live_bytes == 0);
    oasp_ai_context_destroy(context);
    puts("openasp-ai: all tests passed");
    return 0;
}
