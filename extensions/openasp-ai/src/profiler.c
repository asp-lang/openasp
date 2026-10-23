// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <stdlib.h>
#include <string.h>

void oasp_ai_trace_record(oasp_ai_context *context, const char *name, const char *category, uint64_t started, uint64_t duration) {
    if (!context)
        return;
    pthread_mutex_lock(&context->mutex);
    if (!context->profiler_enabled || context->trace_event_count >= context->options.maximum_trace_events) {
        pthread_mutex_unlock(&context->mutex);
        return;
    }
    if (!context->trace_events)
        context->trace_events = calloc(context->options.maximum_trace_events, sizeof(*context->trace_events));
    if (context->trace_events) {
        oasp_ai_trace_event *event = &context->trace_events[context->trace_event_count++];
        snprintf(event->name, sizeof(event->name), "%s", name ? name : "operation");
        snprintf(event->category, sizeof(event->category), "%s", category ? category : "openasp.ai");
        event->started = started;
        event->duration = duration;
        event->thread_id = (uint64_t)(uintptr_t)pthread_self();
    }
    pthread_mutex_unlock(&context->mutex);
}

int oasp_ai_context_profiler_start(oasp_ai_context *context, int detailed, oasp_ai_error *error) {
    if (!context) {
        oasp_ai_set_error(error, 5, "Profiler.Start", "AI context is required", "Use an object created by OpenASP.AI.");
        return -1;
    }
    pthread_mutex_lock(&context->mutex);
    context->trace_event_count = 0;
    context->profiler_detailed = !!detailed;
    context->profiler_enabled = 1;
    pthread_mutex_unlock(&context->mutex);
    return 0;
}

int oasp_ai_context_profiler_stop(oasp_ai_context *context, oasp_ai_error *error) {
    if (!context) {
        oasp_ai_set_error(error, 5, "Profiler.Stop", "AI context is required", "Use an object created by OpenASP.AI.");
        return -1;
    }
    pthread_mutex_lock(&context->mutex);
    context->profiler_enabled = 0;
    pthread_mutex_unlock(&context->mutex);
    return 0;
}

typedef struct {
    oasp_ai_trace_event *events;
    size_t count;
} trace_writer;

static int write_trace(FILE *file, void *opaque, oasp_ai_error *error) {
    trace_writer *writer = opaque;
    (void)error;
    if (fputs("{\"traceEvents\":[", file) == EOF)
        return -1;
    for (size_t index = 0; index < writer->count; index++) {
        oasp_ai_trace_event *event = &writer->events[index];
        if ((index && fputc(',', file) == EOF) ||
            fprintf(file,
                    "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%llu,"
                    "\"ts\":%.3f,\"dur\":%.3f}",
                    event->name, event->category, (unsigned long long)event->thread_id,
                    (double)event->started / 1000.0, (double)event->duration / 1000.0) < 0)
            return -1;
    }
    return fputs("],\"displayTimeUnit\":\"ns\"}", file) == EOF ? -1 : 0;
}

int oasp_ai_context_profiler_export_chrome_trace(oasp_ai_context *context, const char *path, oasp_ai_error *error) {
    if (!context || !path)
        return -1;
    pthread_mutex_lock(&context->mutex);
    size_t count = context->trace_event_count;
    oasp_ai_trace_event *events = count ? malloc(count * sizeof(*events)) : NULL;
    if (count && events)
        memcpy(events, context->trace_events, count * sizeof(*events));
    pthread_mutex_unlock(&context->mutex);
    if (count && !events)
        return -1;
    trace_writer writer = {events, count};
    int status = oasp_ai_write_atomic(path, write_trace, &writer, error);
    free(events);
    return status;
}
