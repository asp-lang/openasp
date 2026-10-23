// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdlib.h>

oasp_ai_optimizer *oasp_ai_optimizer_create(oasp_ai_module *module, oasp_ai_optimizer_kind kind, double learning_rate, double weight_decay, oasp_ai_error *error) {
    if (!module || kind > OASP_AI_OPTIMIZER_ADAMW || !(learning_rate > 0) || weight_decay < 0) {
        oasp_ai_set_error(error, 5, "Optimizer", "invalid optimizer configuration", "Pass a module, positive learning rate, and non-negative weight decay.");
        return NULL;
    }
    oasp_ai_optimizer *optimizer = calloc(1, sizeof(*optimizer));
    if (!optimizer)
        return NULL;
    atomic_init(&optimizer->references, 1);
    optimizer->module = module;
    optimizer->kind = kind;
    optimizer->learning_rate = learning_rate;
    optimizer->weight_decay = weight_decay;
    optimizer->momentum = kind == OASP_AI_OPTIMIZER_SGD ? 0 : 0.9;
    optimizer->beta1 = 0.9;
    optimizer->beta2 = 0.999;
    optimizer->epsilon = 1e-8;
    optimizer->parameter_count = oasp_ai_module_parameter_count(module);
    optimizer->first_moment = calloc(optimizer->parameter_count, sizeof(*optimizer->first_moment));
    optimizer->second_moment = calloc(optimizer->parameter_count, sizeof(*optimizer->second_moment));
    if ((optimizer->parameter_count && !optimizer->first_moment) || (optimizer->parameter_count && !optimizer->second_moment)) {
        free(optimizer->first_moment);
        free(optimizer->second_moment);
        free(optimizer);
        return NULL;
    }
    oasp_ai_module_retain(module);
    return optimizer;
}

void oasp_ai_optimizer_release(oasp_ai_optimizer *optimizer) {
    if (!optimizer || atomic_fetch_sub_explicit(&optimizer->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor_release(optimizer->first_moment[index]);
        oasp_ai_tensor_release(optimizer->second_moment[index]);
    }
    free(optimizer->first_moment);
    free(optimizer->second_moment);
    oasp_ai_module_release(optimizer->module);
    free(optimizer);
}

int oasp_ai_optimizer_zero_grad(oasp_ai_optimizer *optimizer, oasp_ai_error *error) {
    if (!optimizer)
        return -1;
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
        if (!parameter)
            return -1;
        int status = oasp_ai_tensor_zero_grad(parameter, error);
        oasp_ai_tensor_release(parameter);
        if (status)
            return status;
    }
    return 0;
}

void oasp_ai_optimizer_set_momentum(oasp_ai_optimizer *optimizer, double momentum) {
    if (optimizer && momentum >= 0 && momentum < 1)
        optimizer->momentum = momentum;
}

void oasp_ai_optimizer_set_betas(oasp_ai_optimizer *optimizer, double beta1, double beta2, double epsilon) {
    if (!optimizer || beta1 < 0 || beta1 >= 1 || beta2 < 0 || beta2 >= 1 || epsilon <= 0)
        return;
    optimizer->beta1 = beta1;
    optimizer->beta2 = beta2;
    optimizer->epsilon = epsilon;
}

int oasp_ai_optimizer_clip_grad_norm(oasp_ai_optimizer *optimizer, double maximum_norm, oasp_ai_error *error) {
    if (!optimizer || maximum_norm <= 0) {
        oasp_ai_set_error(error, 5, "ClipGradNorm", "maximum norm must be positive", "Pass a positive clipping threshold.");
        return -1;
    }
    oasp_ai_tensor **host_gradients = calloc(optimizer->parameter_count, sizeof(*host_gradients));
    oasp_ai_tensor **clipped_gradients = calloc(optimizer->parameter_count, sizeof(*clipped_gradients));
    char (*devices)[64] = calloc(optimizer->parameter_count, sizeof(*devices));
    if (optimizer->parameter_count && (!host_gradients || !clipped_gradients || !devices)) {
        oasp_ai_set_error(error, 7, "ClipGradNorm", "unable to allocate gradient staging metadata", "Reduce process memory pressure.");
        free(host_gradients);
        free(clipped_gradients);
        free(devices);
        return -1;
    }

    double squared = 0;
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
        if (parameter && parameter->grad) {
            if (oasp_ai_tensor_device(parameter->grad, devices[index], sizeof(devices[index]))) {
                oasp_ai_tensor_release(parameter);
                goto failure;
            }
            host_gradients[index] = oasp_ai_tensor_to_device(parameter->grad, "cpu:0", error);
            if (!host_gradients[index]) {
                oasp_ai_tensor_release(parameter);
                goto failure;
            }
            for (uint64_t item = 0; item < host_gradients[index]->element_count; item++) {
                double value = oasp_ai_tensor_read(host_gradients[index], item);
                if (!isfinite(value)) {
                    oasp_ai_set_error(error, 5, "ClipGradNorm", "gradient norm is not finite",
                                      "Reduce the learning rate, inspect the loss, or enable loss scaling.");
                    oasp_ai_tensor_release(parameter);
                    goto failure;
                }
                squared += value * value;
            }
        }
        oasp_ai_tensor_release(parameter);
    }
    double norm = sqrt(squared);
    optimizer->module->context->metrics.gradient_norm = norm;
    if (norm > maximum_norm) {
        double scale = maximum_norm / norm;
        for (size_t index = 0; index < optimizer->parameter_count; index++) {
            oasp_ai_tensor *host = host_gradients[index];
            if (!host)
                continue;
            for (uint64_t item = 0; item < host->element_count; item++)
                oasp_ai_tensor_write(host, item, oasp_ai_tensor_read(host, item) * scale);
            clipped_gradients[index] = oasp_ai_tensor_to_device(host, devices[index], error);
            if (!clipped_gradients[index])
                goto failure;
        }
        for (size_t index = 0; index < optimizer->parameter_count; index++) {
            if (!clipped_gradients[index])
                continue;
            oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
            oasp_ai_tensor_release(parameter->grad);
            parameter->grad = clipped_gradients[index];
            clipped_gradients[index] = NULL;
            oasp_ai_tensor_release(parameter);
        }
    }

    for (size_t index = 0; index < optimizer->parameter_count; index++)
        oasp_ai_tensor_release(host_gradients[index]);
    free(host_gradients);
    free(clipped_gradients);
    free(devices);
    return 0;

failure:
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor_release(host_gradients[index]);
        oasp_ai_tensor_release(clipped_gradients[index]);
    }
    free(host_gradients);
    free(clipped_gradients);
    free(devices);
    return -1;
}

static int ensure_moments(oasp_ai_optimizer *optimizer, size_t index, oasp_ai_tensor *parameter, oasp_ai_error *error) {
    if (!optimizer->first_moment[index])
        optimizer->first_moment[index] = oasp_ai_tensor_like(parameter, 1, error);
    if (optimizer->kind != OASP_AI_OPTIMIZER_SGD && !optimizer->second_moment[index])
        optimizer->second_moment[index] = oasp_ai_tensor_like(parameter, 1, error);
    return !optimizer->first_moment[index] || (optimizer->kind != OASP_AI_OPTIMIZER_SGD && !optimizer->second_moment[index]);
}

int oasp_ai_optimizer_step(oasp_ai_optimizer *optimizer, oasp_ai_error *error) {
    if (!optimizer)
        return -1;
    uint64_t started = oasp_ai_now_nanoseconds();
    optimizer->step++;
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
        if (!parameter)
            return -1;
        if (!parameter->grad) {
            oasp_ai_tensor_release(parameter);
            continue;
        }
        if (ensure_moments(optimizer, index, parameter, error)) {
            oasp_ai_tensor_release(parameter);
            return -1;
        }
        int32_t backend_index = oasp_ai_tensor_backend_index(parameter);
        if (backend_index >= 0) {
            oasp_ai_tensor *second = optimizer->second_moment[index] ? optimizer->second_moment[index] : optimizer->first_moment[index];
            oasp_ai_tensor *inputs[] = {parameter, parameter->grad, optimizer->first_moment[index], second};
            oasp_ai_tensor *outputs[] = {parameter};
            oasp_ai_optimizer_attributes attributes = {
                .optimizer = optimizer->kind,
                .step = optimizer->step,
                .learning_rate = optimizer->learning_rate,
                .weight_decay = optimizer->weight_decay,
                .momentum = optimizer->momentum,
                .beta1 = optimizer->beta1,
                .beta2 = optimizer->beta2,
                .epsilon = optimizer->epsilon,
            };
            if (oasp_ai_backend_execute(parameter->context, backend_index, OASP_AI_BACKEND_OP_OPTIMIZER_STEP,
                                        inputs, 4, outputs, 1, &attributes, sizeof(attributes), error)) {
                oasp_ai_tensor_release(parameter);
                return -1;
            }
        } else for (uint64_t item = 0; item < parameter->element_count; item++) {
            double value = oasp_ai_tensor_read(parameter, item);
            double gradient = oasp_ai_tensor_read(parameter->grad, item);
            double update;
            if (optimizer->kind == OASP_AI_OPTIMIZER_SGD) {
                double velocity = optimizer->momentum * oasp_ai_tensor_read(optimizer->first_moment[index], item) + gradient;
                oasp_ai_tensor_write(optimizer->first_moment[index], item, velocity);
                update = velocity + optimizer->weight_decay * value;
            } else {
                double first = optimizer->beta1 * oasp_ai_tensor_read(optimizer->first_moment[index], item) + (1 - optimizer->beta1) * gradient;
                double second =
                    optimizer->beta2 * oasp_ai_tensor_read(optimizer->second_moment[index], item) + (1 - optimizer->beta2) * gradient * gradient;
                oasp_ai_tensor_write(optimizer->first_moment[index], item, first);
                oasp_ai_tensor_write(optimizer->second_moment[index], item, second);
                double corrected_first = first / (1 - pow(optimizer->beta1, (double)optimizer->step));
                double corrected_second = second / (1 - pow(optimizer->beta2, (double)optimizer->step));
                update = corrected_first / (sqrt(corrected_second) + optimizer->epsilon);
                if (optimizer->kind == OASP_AI_OPTIMIZER_ADAMW)
                    update += optimizer->weight_decay * value;
            }
            oasp_ai_tensor_write(parameter, item, value - optimizer->learning_rate * update);
        }
        parameter->storage->version++;
        parameter->version++;
        oasp_ai_tensor_release(parameter);
    }
    optimizer->module->context->metrics.optimizer_nanoseconds += oasp_ai_now_nanoseconds() - started;
    return 0;
}

double oasp_ai_optimizer_learning_rate(const oasp_ai_optimizer *optimizer) {
    return optimizer ? optimizer->learning_rate : 0;
}

void oasp_ai_optimizer_set_learning_rate(oasp_ai_optimizer *optimizer, double learning_rate) {
    if (optimizer && learning_rate > 0)
        optimizer->learning_rate = learning_rate;
}
