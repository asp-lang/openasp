// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/engine/interactive.h`): OpenASP implementation unit. Functions validate
 * caller-provided lengths and ownership before mutating state; local variables make
 * intermediate bounds and lifecycle transitions explicit.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_INTERACTIVE_H
#define OPENASP_INTERACTIVE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct oasp_interactive oasp_interactive;

typedef enum {
    OASP_INTERACTIVE_ERROR = -1,
    OASP_INTERACTIVE_OK = 0,
    OASP_INTERACTIVE_INCOMPLETE = 1
} oasp_interactive_result;

oasp_interactive *oasp_interactive_create(const char *root, size_t memory_limit);
void oasp_interactive_destroy(oasp_interactive *interactive);
oasp_interactive_result oasp_interactive_execute(oasp_interactive *interactive, const char *source, size_t length, bool expression, const char **output,
                                                 size_t *output_length);

#endif
