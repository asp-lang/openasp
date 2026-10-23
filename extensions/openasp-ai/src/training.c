// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdlib.h>

oasp_ai_scheduler *oasp_ai_scheduler_create(oasp_ai_optimizer *optimizer, oasp_ai_scheduler_kind kind, uint64_t total_steps, uint64_t warmup_or_step_size, double factor_or_minimum,
                                            oasp_ai_error *error) {
    if (!optimizer || kind > OASP_AI_SCHEDULER_LINEAR_WARMUP || !total_steps || !warmup_or_step_size ||
        warmup_or_step_size > total_steps || factor_or_minimum < 0 ||
        (kind == OASP_AI_SCHEDULER_STEP && (factor_or_minimum <= 0 || factor_or_minimum > 1))) {
        oasp_ai_set_error(error, 5, "Scheduler", "invalid learning-rate scheduler configuration",
                          "Use positive step counts and a valid decay factor or minimum learning rate.");
        return NULL;
    }
    oasp_ai_scheduler *scheduler = calloc(1, sizeof(*scheduler));
    if (!scheduler)
        return NULL;
    scheduler->optimizer = optimizer;
    scheduler->kind = kind;
    scheduler->total_steps = total_steps;
    scheduler->warmup_or_step_size = warmup_or_step_size;
    scheduler->initial_learning_rate = optimizer->learning_rate;
    scheduler->factor_or_minimum = factor_or_minimum;
    atomic_fetch_add_explicit(&optimizer->references, 1, memory_order_relaxed);
    return scheduler;
}

void oasp_ai_scheduler_release(oasp_ai_scheduler *scheduler) {
    if (!scheduler)
        return;
    oasp_ai_optimizer_release(scheduler->optimizer);
    free(scheduler);
}

int oasp_ai_scheduler_step(oasp_ai_scheduler *scheduler, oasp_ai_error *error) {
    if (!scheduler) {
        oasp_ai_set_error(error, 5, "Scheduler.Step", "scheduler is required", "Create a scheduler from an Optimizer.");
        return -1;
    }
    scheduler->step++;
    double learning_rate;
    if (scheduler->kind == OASP_AI_SCHEDULER_STEP) {
        uint64_t periods = scheduler->step / scheduler->warmup_or_step_size;
        learning_rate = scheduler->initial_learning_rate * pow(scheduler->factor_or_minimum, (double)periods);
    } else if (scheduler->kind == OASP_AI_SCHEDULER_COSINE) {
        double progress = (double)(scheduler->step > scheduler->total_steps ? scheduler->total_steps : scheduler->step) /
                          (double)scheduler->total_steps;
        learning_rate = scheduler->factor_or_minimum +
                        0.5 * (scheduler->initial_learning_rate - scheduler->factor_or_minimum) * (1 + cos(3.141592653589793 * progress));
    } else if (scheduler->step <= scheduler->warmup_or_step_size)
        learning_rate = scheduler->initial_learning_rate * (double)scheduler->step / (double)scheduler->warmup_or_step_size;
    else {
        uint64_t decay_steps = scheduler->total_steps - scheduler->warmup_or_step_size;
        uint64_t elapsed = scheduler->step - scheduler->warmup_or_step_size;
        double progress = decay_steps ? (double)(elapsed > decay_steps ? decay_steps : elapsed) / (double)decay_steps : 1;
        learning_rate = scheduler->initial_learning_rate +
                        (scheduler->factor_or_minimum - scheduler->initial_learning_rate) * progress;
    }
    scheduler->optimizer->learning_rate = learning_rate;
    return 0;
}

uint64_t oasp_ai_scheduler_current_step(const oasp_ai_scheduler *scheduler) {
    return scheduler ? scheduler->step : 0;
}

oasp_ai_loss_scaler *oasp_ai_loss_scaler_create(double initial_scale, double growth_factor, double backoff_factor, uint32_t growth_interval, oasp_ai_error *error) {
    if (!(initial_scale > 0) || !(growth_factor > 1) || !(backoff_factor > 0 && backoff_factor < 1) || !growth_interval) {
        oasp_ai_set_error(error, 5, "LossScaler", "invalid dynamic loss scaling configuration",
                          "Use a positive scale, growth factor above one, backoff in (0,1), and a positive growth interval.");
        return NULL;
    }
    oasp_ai_loss_scaler *scaler = calloc(1, sizeof(*scaler));
    if (!scaler)
        return NULL;
    scaler->scale = initial_scale;
    scaler->growth_factor = growth_factor;
    scaler->backoff_factor = backoff_factor;
    scaler->growth_interval = growth_interval;
    return scaler;
}

void oasp_ai_loss_scaler_release(oasp_ai_loss_scaler *scaler) {
    free(scaler);
}

double oasp_ai_loss_scaler_scale(const oasp_ai_loss_scaler *scaler) {
    return scaler ? scaler->scale : 0;
}

oasp_ai_tensor *oasp_ai_loss_scaler_scale_loss(oasp_ai_loss_scaler *scaler, oasp_ai_tensor *loss, oasp_ai_error *error) {
    if (!scaler || !loss)
        return NULL;
    oasp_ai_tensor *factor = NULL;
    int32_t backend_index = oasp_ai_tensor_backend_index(loss);
    if (backend_index >= 0) {
        factor = oasp_ai_tensor_like(loss, 1, error);
        if (factor) {
            oasp_ai_fill_attributes attributes = {.value = scaler->scale};
            oasp_ai_tensor *outputs[] = {factor};
            if (oasp_ai_backend_execute(loss->context, backend_index, OASP_AI_BACKEND_OP_FILL,
                                        NULL, 0, outputs, 1, &attributes, sizeof(attributes), error)) {
                oasp_ai_tensor_release(factor);
                factor = NULL;
            }
        }
    } else {
        int64_t shape[] = {1};
        factor = oasp_ai_tensor_full(loss->context, shape, 1, loss->desc.dtype, scaler->scale, error);
    }
    oasp_ai_tensor *scaled = factor ? oasp_ai_binary(loss->context, OASP_AI_OP_MUL, loss, factor, error) : NULL;
    oasp_ai_tensor_release(factor);
    return scaled;
}

static int backend_unscale_and_check(oasp_ai_optimizer *optimizer, double inverse_scale, int *finite,
                                     oasp_ai_error *error) {
    int32_t backend_index = -1;
    oasp_ai_tensor *status = NULL;
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
        if (!parameter)
            goto failed;
        if (parameter->grad) {
            int32_t current = oasp_ai_tensor_backend_index(parameter->grad);
            if (current < 0 || (backend_index >= 0 && current != backend_index)) {
                oasp_ai_tensor_release(parameter);
                goto unsupported;
            }
            if (backend_index < 0) {
                backend_index = current;
                oasp_ai_registered_backend *backend = &parameter->context->backends[current];
                oasp_ai_capability_query query = {
                    .struct_size = sizeof(query),
                    .op_id = OASP_AI_BACKEND_OP_UNSCALE_AND_CHECK,
                    .op_version = 1,
                    .input_dtype = parameter->grad->desc.dtype,
                    .output_dtype = parameter->grad->desc.dtype,
                };
                if (!backend->descriptor->api->query_capabilities ||
                    backend->descriptor->api->query_capabilities(backend->device, &query, NULL) ||
                    !query.supported) {
                    oasp_ai_tensor_release(parameter);
                    goto unsupported;
                }
                status = oasp_ai_tensor_like(parameter->grad, 1, error);
                if (!status) {
                    oasp_ai_tensor_release(parameter);
                    goto failed;
                }
                oasp_ai_fill_attributes fill = {.value = 1};
                oasp_ai_tensor *fill_outputs[] = {status};
                if (oasp_ai_backend_execute(parameter->context, backend_index, OASP_AI_BACKEND_OP_FILL,
                                            NULL, 0, fill_outputs, 1, &fill, sizeof(fill), error)) {
                    oasp_ai_tensor_release(parameter);
                    goto failed;
                }
            }
        }
        oasp_ai_tensor_release(parameter);
    }
    if (backend_index < 0) {
        *finite = 1;
        return 1;
    }
    for (size_t index = 0; index < optimizer->parameter_count; index++) {
        oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
        if (!parameter)
            goto failed;
        if (parameter->grad) {
            oasp_ai_loss_scale_attributes attributes = {.inverse_scale = inverse_scale};
            oasp_ai_tensor *inputs[] = {parameter->grad};
            oasp_ai_tensor *outputs[] = {parameter->grad, status};
            if (oasp_ai_backend_execute(parameter->context, backend_index,
                                        OASP_AI_BACKEND_OP_UNSCALE_AND_CHECK, inputs, 1, outputs, 2,
                                        &attributes, sizeof(attributes), error)) {
                oasp_ai_tensor_release(parameter);
                goto failed;
            }
        }
        oasp_ai_tensor_release(parameter);
    }
    float result = 0;
    if (oasp_ai_tensor_copy_to_host(status, &result, sizeof(result), error))
        goto failed;
    oasp_ai_tensor_release(status);
    *finite = result != 0;
    return 1;

unsupported:
    oasp_ai_tensor_release(status);
    return 0;
failed:
    oasp_ai_tensor_release(status);
    return -1;
}

int oasp_ai_loss_scaler_step(oasp_ai_loss_scaler *scaler, oasp_ai_optimizer *optimizer, oasp_ai_error *error) {
    if (!scaler || !optimizer)
        return -1;
    int finite = 1;
    int backend_status = backend_unscale_and_check(optimizer, 1.0 / scaler->scale, &finite, error);
    if (backend_status < 0)
        return -1;
    if (!backend_status)
        for (size_t index = 0; index < optimizer->parameter_count; index++) {
            oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
            if (parameter && parameter->grad)
                for (uint64_t item = 0; item < parameter->grad->element_count; item++)
                    if (!isfinite(oasp_ai_tensor_read(parameter->grad, item))) {
                        finite = 0;
                        break;
                    }
            oasp_ai_tensor_release(parameter);
            if (!finite)
                break;
        }
    if (!finite) {
        scaler->scale *= scaler->backoff_factor;
        scaler->finite_steps = 0;
        optimizer->module->context->metrics.nan_inf_count++;
        optimizer->module->context->metrics.loss_scale = scaler->scale;
        return oasp_ai_optimizer_zero_grad(optimizer, error);
    }
    if (!backend_status)
        for (size_t index = 0; index < optimizer->parameter_count; index++) {
            oasp_ai_tensor *parameter = oasp_ai_module_parameter(optimizer->module, index, NULL);
            if (parameter && parameter->grad)
                for (uint64_t item = 0; item < parameter->grad->element_count; item++)
                    oasp_ai_tensor_write(parameter->grad, item, oasp_ai_tensor_read(parameter->grad, item) / scaler->scale);
            oasp_ai_tensor_release(parameter);
        }
    int status = oasp_ai_optimizer_step(optimizer, error);
    if (!status && ++scaler->finite_steps >= scaler->growth_interval) {
        scaler->scale *= scaler->growth_factor;
        scaler->finite_steps = 0;
    }
    optimizer->module->context->metrics.loss_scale = scaler->scale;
    return status;
}
