// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Build-only CPU sampler for environments where the operating system denies
 * process attachment. It is linked only with OASP_CPU_PROFILE=1 and records
 * periodic call stacks from compiler function instrumentation.
 */

#include <limits.h>
#include <mach/mach_time.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OASP_PROFILE_NOINSTRUMENT __attribute__((no_instrument_function))
#define OASP_PROFILE_MAX_DEPTH 64
#define OASP_PROFILE_MAX_SAMPLES 20000

typedef struct {
    uint8_t depth;
    void *frames[OASP_PROFILE_MAX_DEPTH];
} oasp_cpu_profile_sample;

static bool oasp_cpu_profile_initialized;
static bool oasp_cpu_profile_enabled;
static bool oasp_cpu_profile_dumped;
static uint64_t oasp_cpu_profile_period;
static char oasp_cpu_profile_path[PATH_MAX];
static oasp_cpu_profile_sample oasp_cpu_profile_samples[OASP_PROFILE_MAX_SAMPLES];
static size_t oasp_cpu_profile_sample_count;
static _Thread_local void *oasp_cpu_profile_stack[OASP_PROFILE_MAX_DEPTH];
static _Thread_local size_t oasp_cpu_profile_depth;
static _Thread_local uint64_t oasp_cpu_profile_next_sample;

static void oasp_cpu_profile_dump(void) OASP_PROFILE_NOINSTRUMENT;
static void oasp_cpu_profile_initialize(void) OASP_PROFILE_NOINSTRUMENT;
static void oasp_cpu_profile_maybe_sample(void) OASP_PROFILE_NOINSTRUMENT;

/* Darwin's always-inline signal helper needs a linkable form under full function instrumentation. */
int __sigbits(int signal_number) OASP_PROFILE_NOINSTRUMENT;

int __sigbits(int signal_number) {
    return signal_number > 32 ? 0 : (1 << (signal_number - 1));
}

static void oasp_cpu_profile_dump(void) {
    if (!oasp_cpu_profile_enabled || oasp_cpu_profile_dumped)
        return;
    oasp_cpu_profile_dumped = true;
    FILE *output = fopen(oasp_cpu_profile_path, "w");
    if (!output)
        return;
    extern const struct mach_header_64 _mh_execute_header;
    fprintf(output, "# image=%p\n", (const void *)&_mh_execute_header);
    for (size_t sample_index = 0; sample_index < oasp_cpu_profile_sample_count; sample_index++) {
        const oasp_cpu_profile_sample *sample = &oasp_cpu_profile_samples[sample_index];
        for (size_t frame_index = 0; frame_index < sample->depth; frame_index++) {
            if (frame_index)
                fputc(';', output);
            fprintf(output, "%p", sample->frames[frame_index]);
        }
        fputc('\n', output);
    }
    fclose(output);
}

static void oasp_cpu_profile_initialize(void) {
    if (oasp_cpu_profile_initialized)
        return;
    oasp_cpu_profile_initialized = true;
    const char *path = getenv("OPENASP_CPU_PROFILE");
    if (!path || !*path)
        return;
    size_t path_length = strlen(path);
    if (path_length >= sizeof(oasp_cpu_profile_path))
        return;
    memcpy(oasp_cpu_profile_path, path, path_length + 1);
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.numer || !timebase.denom)
        return;
    oasp_cpu_profile_period = (UINT64_C(1000000) * timebase.denom + timebase.numer - 1) / timebase.numer;
    if (!oasp_cpu_profile_period)
        oasp_cpu_profile_period = 1;
    oasp_cpu_profile_enabled = true;
    oasp_cpu_profile_next_sample = mach_absolute_time() + oasp_cpu_profile_period;
    (void)atexit(oasp_cpu_profile_dump);
}

static void oasp_cpu_profile_maybe_sample(void) {
    if (!oasp_cpu_profile_enabled || oasp_cpu_profile_sample_count == OASP_PROFILE_MAX_SAMPLES)
        return;
    uint64_t now = mach_absolute_time();
    if (now < oasp_cpu_profile_next_sample)
        return;
    oasp_cpu_profile_next_sample = now + oasp_cpu_profile_period;
    oasp_cpu_profile_sample *sample = &oasp_cpu_profile_samples[oasp_cpu_profile_sample_count++];
    sample->depth = (uint8_t)oasp_cpu_profile_depth;
    memcpy(sample->frames, oasp_cpu_profile_stack, sample->depth * sizeof(*sample->frames));
}

void __cyg_profile_func_enter(void *function, void *caller) OASP_PROFILE_NOINSTRUMENT;
void __cyg_profile_func_exit(void *function, void *caller) OASP_PROFILE_NOINSTRUMENT;

void __cyg_profile_func_enter(void *function, void *caller) {
    (void)caller;
    oasp_cpu_profile_initialize();
    if (!oasp_cpu_profile_enabled)
        return;
    oasp_cpu_profile_maybe_sample();
    if (oasp_cpu_profile_depth < OASP_PROFILE_MAX_DEPTH)
        oasp_cpu_profile_stack[oasp_cpu_profile_depth++] = function;
}

void __cyg_profile_func_exit(void *function, void *caller) {
    (void)caller;
    if (!oasp_cpu_profile_enabled)
        return;
    while (oasp_cpu_profile_depth && oasp_cpu_profile_stack[oasp_cpu_profile_depth - 1] != function)
        oasp_cpu_profile_depth--;
    if (oasp_cpu_profile_depth)
        oasp_cpu_profile_depth--;
}
