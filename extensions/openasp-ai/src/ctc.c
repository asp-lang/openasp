// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

static double log_add(double left, double right) {
    if (left == -INFINITY)
        return right;
    if (right == -INFINITY)
        return left;
    double maximum = left > right ? left : right;
    return maximum + log1p(exp(-fabs(left - right)));
}

static int64_t target_value(const oasp_ai_tensor *targets, uint64_t batch,
                            uint64_t position, uint64_t maximum_target_length) {
    return (int64_t)oasp_ai_tensor_read(
        targets, batch * maximum_target_length + position);
}

static int validate_ctc(oasp_ai_context *context, oasp_ai_tensor *log_probabilities,
                        oasp_ai_tensor *targets, oasp_ai_tensor *input_lengths,
                        oasp_ai_tensor *target_lengths, int64_t blank,
                        oasp_ai_reduction reduction, int32_t *backend_index,
                        oasp_ai_error *error) {
    if (!context || !log_probabilities || !targets || !input_lengths || !target_lengths ||
        log_probabilities->desc.rank != 3 || targets->desc.rank != 2 ||
        input_lengths->desc.rank != 1 || target_lengths->desc.rank != 1 ||
        targets->desc.dtype != OASP_AI_DTYPE_I64 ||
        input_lengths->desc.dtype != OASP_AI_DTYPE_I64 ||
        target_lengths->desc.dtype != OASP_AI_DTYPE_I64 ||
        targets->desc.shape[0] != log_probabilities->desc.shape[1] ||
        input_lengths->desc.shape[0] != log_probabilities->desc.shape[1] ||
        target_lengths->desc.shape[0] != log_probabilities->desc.shape[1] ||
        blank < 0 || blank >= log_probabilities->desc.shape[2] ||
        reduction > OASP_AI_REDUCTION_SUM) {
        oasp_ai_set_error(
            error, 5, "CTCLoss",
            "expected log-probabilities [time,batch,classes], padded int64 targets [batch,max-target], int64 lengths [batch], and none/mean/sum reduction",
            "Check CTC tensor ranks, dtypes, batch dimensions, blank index, and reduction.");
        return -1;
    }
    oasp_ai_tensor *inputs[] = {
        log_probabilities, targets, input_lengths, target_lengths,
    };
    return oasp_ai_tensor_same_device(inputs, 4, backend_index, error);
}

static int ctc_forward_host(oasp_ai_tensor *log_probabilities,
                            oasp_ai_tensor *targets, oasp_ai_tensor *input_lengths,
                            oasp_ai_tensor *target_lengths, oasp_ai_tensor *loss,
                            oasp_ai_tensor *alpha, oasp_ai_tensor *per_sample,
                            int64_t blank, int zero_infinity,
                            oasp_ai_reduction reduction, oasp_ai_error *error) {
    uint64_t maximum_time = (uint64_t)log_probabilities->desc.shape[0];
    uint64_t batch_count = (uint64_t)log_probabilities->desc.shape[1];
    uint64_t classes = (uint64_t)log_probabilities->desc.shape[2];
    uint64_t maximum_target_length = (uint64_t)targets->desc.shape[1];
    uint64_t maximum_states = (uint64_t)alpha->desc.shape[2];
    for (uint64_t index = 0; index < alpha->element_count; index++)
        oasp_ai_tensor_write(alpha, index, -INFINITY);
    double aggregate = 0;
    for (uint64_t batch = 0; batch < batch_count; batch++) {
        int64_t input_length = (int64_t)oasp_ai_tensor_read(input_lengths, batch);
        int64_t target_length = (int64_t)oasp_ai_tensor_read(target_lengths, batch);
        if (input_length < 0 || (uint64_t)input_length > maximum_time ||
            target_length < 0 || (uint64_t)target_length > maximum_target_length) {
            oasp_ai_set_error(error, 5, "CTCLoss", "CTC sequence length is outside its padded dimension",
                              "Keep input and target lengths within their tensor dimensions.");
            return -1;
        }
        uint64_t repeats = 0;
        for (int64_t position = 0; position < target_length; position++) {
            int64_t label = target_value(targets, batch, (uint64_t)position,
                                         maximum_target_length);
            if (label < 0 || (uint64_t)label >= classes || label == blank) {
                oasp_ai_set_error(error, 5, "CTCLoss", "target contains an invalid or blank label",
                                  "Use labels in [0, classes) excluding the blank class.");
                return -1;
            }
            if (position && label == target_value(targets, batch, (uint64_t)position - 1,
                                                  maximum_target_length))
                repeats++;
        }
        double sample_loss = INFINITY;
        if (input_length > 0 && (uint64_t)input_length >= (uint64_t)target_length + repeats) {
            uint64_t states = (uint64_t)target_length * 2 + 1;
#define ALPHA_INDEX(t, s) ((batch * maximum_time + (uint64_t)(t)) * maximum_states + (uint64_t)(s))
#define LOGP_INDEX(t, c) (((uint64_t)(t) * batch_count + batch) * classes + (uint64_t)(c))
            oasp_ai_tensor_write(alpha, ALPHA_INDEX(0, 0),
                                 oasp_ai_tensor_read(log_probabilities, LOGP_INDEX(0, blank)));
            if (target_length)
                oasp_ai_tensor_write(
                    alpha, ALPHA_INDEX(0, 1),
                    oasp_ai_tensor_read(
                        log_probabilities,
                        LOGP_INDEX(0, target_value(targets, batch, 0, maximum_target_length))));
            for (int64_t time = 1; time < input_length; time++)
                for (uint64_t state = 0; state < states; state++) {
                    int64_t label = state % 2
                                        ? target_value(targets, batch, state / 2,
                                                       maximum_target_length)
                                        : blank;
                    double value = oasp_ai_tensor_read(alpha, ALPHA_INDEX(time - 1, state));
                    if (state)
                        value = log_add(value, oasp_ai_tensor_read(
                                                   alpha, ALPHA_INDEX(time - 1, state - 1)));
                    if (state > 1 && label != blank &&
                        label != (state > 2
                                      ? target_value(targets, batch, state / 2 - 1,
                                                     maximum_target_length)
                                      : blank))
                        value = log_add(value, oasp_ai_tensor_read(
                                                   alpha, ALPHA_INDEX(time - 1, state - 2)));
                    value += oasp_ai_tensor_read(log_probabilities,
                                                 LOGP_INDEX(time, label));
                    oasp_ai_tensor_write(alpha, ALPHA_INDEX(time, state), value);
                }
            double log_likelihood =
                oasp_ai_tensor_read(alpha, ALPHA_INDEX(input_length - 1, states - 1));
            if (states > 1)
                log_likelihood = log_add(
                    log_likelihood,
                    oasp_ai_tensor_read(alpha,
                                        ALPHA_INDEX(input_length - 1, states - 2)));
            sample_loss = -log_likelihood;
#undef LOGP_INDEX
#undef ALPHA_INDEX
        }
        oasp_ai_tensor_write(per_sample, batch, sample_loss);
        double reduced_loss = zero_infinity && !isfinite(sample_loss) ? 0 : sample_loss;
        if (reduction == OASP_AI_REDUCTION_NONE)
            oasp_ai_tensor_write(loss, batch, reduced_loss);
        else if (reduction == OASP_AI_REDUCTION_MEAN)
            aggregate += reduced_loss / (double)(target_length > 0 ? target_length : 1);
        else
            aggregate += reduced_loss;
    }
    if (reduction != OASP_AI_REDUCTION_NONE)
        oasp_ai_tensor_write(loss, 0, reduction == OASP_AI_REDUCTION_MEAN
                                         ? aggregate / (double)batch_count
                                         : aggregate);
    return 0;
}

oasp_ai_tensor *oasp_ai_ctc_loss(oasp_ai_context *context,
                                 oasp_ai_tensor *log_probabilities,
                                 oasp_ai_tensor *targets,
                                 oasp_ai_tensor *input_lengths,
                                 oasp_ai_tensor *target_lengths, int64_t blank,
                                 int zero_infinity, oasp_ai_reduction reduction,
                                 oasp_ai_error *error) {
    int32_t backend_index;
    if (validate_ctc(context, log_probabilities, targets, input_lengths,
                     target_lengths, blank, reduction, &backend_index, error))
        return NULL;
    uint64_t batch = (uint64_t)log_probabilities->desc.shape[1];
    uint64_t maximum_time = (uint64_t)log_probabilities->desc.shape[0];
    uint64_t maximum_states = (uint64_t)targets->desc.shape[1] * 2 + 1;
    oasp_ai_tensor_desc loss_desc = {
        .dtype = OASP_AI_DTYPE_F32,
        .rank = 1,
        .shape = {reduction == OASP_AI_REDUCTION_NONE ? (int64_t)batch : 1},
    };
    oasp_ai_tensor_desc alpha_desc = {
        .dtype = OASP_AI_DTYPE_F32,
        .rank = 3,
        .shape = {(int64_t)batch, (int64_t)maximum_time, (int64_t)maximum_states},
    };
    oasp_ai_tensor_desc sample_desc = {
        .dtype = OASP_AI_DTYPE_F32,
        .rank = 1,
        .shape = {(int64_t)batch},
    };
    oasp_ai_tensor *loss = backend_index >= 0
                               ? oasp_ai_tensor_create_on_backend(context, &loss_desc,
                                                                  backend_index, error)
                               : oasp_ai_tensor_create(context, &loss_desc, NULL, 0, error);
    oasp_ai_tensor *alpha = backend_index >= 0
                                ? oasp_ai_tensor_create_on_backend(context, &alpha_desc,
                                                                   backend_index, error)
                                : oasp_ai_tensor_create(context, &alpha_desc, NULL, 0, error);
    oasp_ai_tensor *per_sample = backend_index >= 0
                                     ? oasp_ai_tensor_create_on_backend(context, &sample_desc,
                                                                        backend_index, error)
                                     : oasp_ai_tensor_create(context, &sample_desc, NULL, 0, error);
    if (!loss || !alpha || !per_sample) {
        oasp_ai_tensor_release(per_sample);
        oasp_ai_tensor_release(alpha);
        oasp_ai_tensor_release(loss);
        return NULL;
    }
    oasp_ai_ctc_attributes attributes = {
        .reduction = reduction,
        .blank = blank,
        .zero_infinity = !!zero_infinity,
        .targets_are_padded = 1,
    };
    if (backend_index >= 0) {
        oasp_ai_tensor *inputs[] = {
            log_probabilities, targets, input_lengths, target_lengths,
        };
        oasp_ai_tensor *outputs[] = {loss, alpha, per_sample};
        if (oasp_ai_backend_execute(context, backend_index, OASP_AI_OP_CTC_LOSS,
                                    inputs, 4, outputs, 3, &attributes,
                                    sizeof(attributes), error)) {
            oasp_ai_tensor_release(per_sample);
            oasp_ai_tensor_release(alpha);
            oasp_ai_tensor_release(loss);
            return NULL;
        }
    } else if (ctc_forward_host(log_probabilities, targets, input_lengths,
                                target_lengths, loss, alpha, per_sample, blank,
                                zero_infinity, reduction, error)) {
        oasp_ai_tensor_release(per_sample);
        oasp_ai_tensor_release(alpha);
        oasp_ai_tensor_release(loss);
        return NULL;
    }
    oasp_ai_tensor *saved[] = {
        log_probabilities, targets, input_lengths, target_lengths, alpha, per_sample,
    };
    if (oasp_ai_attach_grad(loss, OASP_AI_OP_CTC_LOSS, saved, 6, 0,
                            (int32_t)reduction, error)) {
        oasp_ai_tensor_release(per_sample);
        oasp_ai_tensor_release(alpha);
        oasp_ai_tensor_release(loss);
        return NULL;
    }
    if (loss->grad_fn) {
        loss->grad_fn->parameters[0] = blank;
        loss->grad_fn->parameters[1] = !!zero_infinity;
    }
    oasp_ai_tensor_release(per_sample);
    oasp_ai_tensor_release(alpha);
    return loss;
}

int oasp_ai_ctc_loss_backward(oasp_ai_tensor *output, oasp_ai_tensor *upstream,
                              oasp_ai_error *error) {
    oasp_ai_tensor *log_probabilities = output->grad_fn->inputs[0];
    oasp_ai_tensor *targets = output->grad_fn->inputs[1];
    oasp_ai_tensor *input_lengths = output->grad_fn->inputs[2];
    oasp_ai_tensor *target_lengths = output->grad_fn->inputs[3];
    oasp_ai_tensor *alpha = output->grad_fn->inputs[4];
    oasp_ai_tensor *per_sample = output->grad_fn->inputs[5];
    int64_t blank = output->grad_fn->parameters[0];
    int zero_infinity = (int)output->grad_fn->parameters[1];
    oasp_ai_reduction reduction = (oasp_ai_reduction)output->grad_fn->axis;
    oasp_ai_tensor *gradient = oasp_ai_tensor_like(log_probabilities, 1, error);
    if (!gradient)
        return -1;
    int32_t backend_index = oasp_ai_tensor_backend_index(log_probabilities);
    int status = 0;
    if (backend_index >= 0) {
        oasp_ai_tensor *beta = oasp_ai_tensor_like(alpha, 1, error);
        oasp_ai_tensor *inputs[] = {
            log_probabilities, targets, input_lengths, target_lengths,
            alpha, per_sample, upstream,
        };
        oasp_ai_tensor *outputs[] = {gradient, beta};
        oasp_ai_ctc_attributes attributes = {
            .reduction = reduction,
            .blank = blank,
            .zero_infinity = zero_infinity,
            .targets_are_padded = 1,
        };
        status = !beta ||
                 oasp_ai_backend_execute(output->context, backend_index,
                                         OASP_AI_BACKEND_OP_CTC_LOSS_BACKWARD,
                                         inputs, 7, outputs, 2, &attributes,
                                         sizeof(attributes), error);
        oasp_ai_tensor_release(beta);
    } else {
        uint64_t maximum_time = (uint64_t)log_probabilities->desc.shape[0];
        uint64_t batch_count = (uint64_t)log_probabilities->desc.shape[1];
        uint64_t classes = (uint64_t)log_probabilities->desc.shape[2];
        uint64_t maximum_target_length = (uint64_t)targets->desc.shape[1];
        uint64_t maximum_states = (uint64_t)alpha->desc.shape[2];
        double *beta = malloc(maximum_time * maximum_states * sizeof(*beta));
        if (!beta)
            status = -1;
        for (uint64_t batch = 0; !status && batch < batch_count; batch++) {
            int64_t input_length = (int64_t)oasp_ai_tensor_read(input_lengths, batch);
            int64_t target_length = (int64_t)oasp_ai_tensor_read(target_lengths, batch);
            double sample_loss = oasp_ai_tensor_read(per_sample, batch);
            if (!isfinite(sample_loss) || input_length <= 0)
                continue;
            uint64_t states = (uint64_t)target_length * 2 + 1;
            for (uint64_t index = 0; index < maximum_time * maximum_states; index++)
                beta[index] = -INFINITY;
#define BETA_INDEX(t, s) ((uint64_t)(t) * maximum_states + (uint64_t)(s))
#define ALPHA_INDEX(t, s) ((batch * maximum_time + (uint64_t)(t)) * maximum_states + (uint64_t)(s))
#define LOGP_INDEX(t, c) (((uint64_t)(t) * batch_count + batch) * classes + (uint64_t)(c))
            beta[BETA_INDEX(input_length - 1, states - 1)] = 0;
            if (states > 1)
                beta[BETA_INDEX(input_length - 1, states - 2)] = 0;
            for (int64_t time = input_length - 2; time >= 0; time--)
                for (uint64_t state = 0; state < states; state++) {
                    double value = -INFINITY;
                    for (uint64_t advance = 0; advance < 3; advance++) {
                        uint64_t next = state + advance;
                        if (next >= states)
                            continue;
                        int64_t next_label = next % 2
                                                 ? target_value(targets, batch, next / 2,
                                                                maximum_target_length)
                                                 : blank;
                        if (advance == 2 &&
                            (next_label == blank ||
                             next_label == (state % 2
                                                ? target_value(targets, batch, state / 2,
                                                               maximum_target_length)
                                                : blank)))
                            continue;
                        value = log_add(
                            value,
                            beta[BETA_INDEX(time + 1, next)] +
                                oasp_ai_tensor_read(log_probabilities,
                                                    LOGP_INDEX(time + 1, next_label)));
                    }
                    beta[BETA_INDEX(time, state)] = value;
                }
            double scale = oasp_ai_tensor_read(
                upstream, reduction == OASP_AI_REDUCTION_NONE ? batch : 0);
            if (reduction == OASP_AI_REDUCTION_MEAN)
                scale /= (double)batch_count *
                         (double)(target_length > 0 ? target_length : 1);
            for (int64_t time = 0; time < input_length; time++) {
                for (uint64_t class_index = 0; class_index < classes; class_index++)
                    oasp_ai_tensor_write(
                        gradient, LOGP_INDEX(time, class_index),
                        exp(oasp_ai_tensor_read(log_probabilities,
                                                LOGP_INDEX(time, class_index))) *
                            scale);
                for (uint64_t state = 0; state < states; state++) {
                    int64_t label = state % 2
                                        ? target_value(targets, batch, state / 2,
                                                       maximum_target_length)
                                        : blank;
                    uint64_t index = LOGP_INDEX(time, label);
                    double posterior = exp(
                        oasp_ai_tensor_read(alpha, ALPHA_INDEX(time, state)) +
                        beta[BETA_INDEX(time, state)] + sample_loss);
                    oasp_ai_tensor_write(
                        gradient, index,
                        oasp_ai_tensor_read(gradient, index) - posterior * scale);
                }
            }
#undef LOGP_INDEX
#undef ALPHA_INDEX
#undef BETA_INDEX
        }
        free(beta);
    }
    if (!status && (log_probabilities->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD))
        status = oasp_ai_accumulate_gradient(log_probabilities, gradient, error);
    oasp_ai_tensor_release(gradient);
    return status;
}
