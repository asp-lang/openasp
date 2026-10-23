// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <stdlib.h>

static void *execute_async(void *opaque) {
    oasp_ai_event_handle *event = opaque;
    uint64_t started = oasp_ai_now_nanoseconds();
    event->status = oasp_ai_plan_execute(event->plan, event->inputs, event->input_count, &event->outputs, &event->output_count, &event->error);
    uint64_t finished = oasp_ai_now_nanoseconds();
    pthread_mutex_lock(&event->mutex);
    event->completed = 1;
    pthread_cond_broadcast(&event->condition);
    pthread_mutex_unlock(&event->mutex);
    oasp_ai_trace_record(event->plan->graph->context, "async_execute", "schedule", started, finished - started);
    return NULL;
}

oasp_ai_event_handle *oasp_ai_plan_execute_async(oasp_ai_plan *plan, oasp_ai_tensor *const *inputs, size_t input_count, oasp_ai_error *error) {
    if (!plan || !inputs || input_count != plan->graph->input_count) {
        oasp_ai_set_error(error, 5, "ExecuteAsync", "input count does not match the plan", "Pass one Tensor for every graph input.");
        return NULL;
    }
    oasp_ai_event_handle *event = calloc(1, sizeof(*event));
    if (event)
        event->inputs = calloc(input_count, sizeof(*event->inputs));
    int mutex_initialized = event && event->inputs && !pthread_mutex_init(&event->mutex, NULL);
    int condition_initialized = mutex_initialized && !pthread_cond_init(&event->condition, NULL);
    if (!condition_initialized) {
        if (mutex_initialized)
            pthread_mutex_destroy(&event->mutex);
        free(event ? event->inputs : NULL);
        free(event);
        oasp_ai_set_error(error, 7, "ExecuteAsync", "unable to initialize Event synchronization",
                          "Reduce process resource pressure.");
        return NULL;
    }
    atomic_init(&event->references, 1);
    event->plan = plan;
    atomic_fetch_add_explicit(&plan->references, 1, memory_order_relaxed);
    event->input_count = input_count;
    for (size_t index = 0; index < input_count; index++) {
        event->inputs[index] = inputs[index];
        oasp_ai_tensor_retain(inputs[index]);
    }
    if (pthread_create(&event->thread, NULL, execute_async, event)) {
        for (size_t index = 0; index < input_count; index++)
            oasp_ai_tensor_release(event->inputs[index]);
        oasp_ai_plan_release(plan);
        pthread_cond_destroy(&event->condition);
        pthread_mutex_destroy(&event->mutex);
        free(event->inputs);
        free(event);
        oasp_ai_set_error(error, 7, "ExecuteAsync", "unable to start CPU execution task", "Reduce worker thread pressure.");
        return NULL;
    }
    return event;
}

void oasp_ai_event_retain(oasp_ai_event_handle *event) {
    if (event)
        atomic_fetch_add_explicit(&event->references, 1, memory_order_relaxed);
}

int oasp_ai_event_ready(oasp_ai_event_handle *event) {
    if (!event)
        return 0;
    pthread_mutex_lock(&event->mutex);
    int completed = event->completed;
    pthread_mutex_unlock(&event->mutex);
    return completed;
}

int oasp_ai_event_wait(oasp_ai_event_handle *event, oasp_ai_error *error) {
    if (!event)
        return -1;
    pthread_mutex_lock(&event->mutex);
    while (!event->completed)
        pthread_cond_wait(&event->condition, &event->mutex);
    int should_join = !event->joined;
    event->joined = 1;
    int status = event->status;
    oasp_ai_error asynchronous_error = event->error;
    pthread_mutex_unlock(&event->mutex);
    if (should_join)
        pthread_join(event->thread, NULL);
    if (status && error)
        *error = asynchronous_error;
    return status;
}

int oasp_ai_event_outputs(oasp_ai_event_handle *event, oasp_ai_tensor ***outputs, size_t *output_count, oasp_ai_error *error) {
    if (!outputs || !output_count || oasp_ai_event_wait(event, error))
        return -1;
    oasp_ai_tensor **result = event->output_count ? calloc(event->output_count, sizeof(*result)) : NULL;
    if (event->output_count && !result)
        return -1;
    for (size_t index = 0; index < event->output_count; index++) {
        result[index] = event->outputs[index];
        oasp_ai_tensor_retain(result[index]);
    }
    *outputs = result;
    *output_count = event->output_count;
    return 0;
}

void oasp_ai_event_release(oasp_ai_event_handle *event) {
    if (!event || atomic_fetch_sub_explicit(&event->references, 1, memory_order_acq_rel) != 1)
        return;
    oasp_ai_event_wait(event, NULL);
    for (size_t index = 0; index < event->input_count; index++)
        oasp_ai_tensor_release(event->inputs[index]);
    for (size_t index = 0; index < event->output_count; index++)
        oasp_ai_tensor_release(event->outputs[index]);
    free(event->inputs);
    free(event->outputs);
    oasp_ai_plan_release(event->plan);
    pthread_cond_destroy(&event->condition);
    pthread_mutex_destroy(&event->mutex);
    free(event);
}
