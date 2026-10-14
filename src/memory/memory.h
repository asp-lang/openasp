// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_MEMORY_H
#define OPENASP_MEMORY_H

#include "openasp.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct oasp_memory_manager {
    size_t limit;
    size_t current;
    size_t peak;
    size_t allocation_failures;
    size_t failed_requested;
    size_t failed_current;
    const char *failed_category;
    char error[192];
} oasp_memory_manager;

void oasp_memory_manager_init(oasp_memory_manager *manager, size_t limit);
bool oasp_memory_reserve(oasp_memory_manager *manager, size_t amount, const char *category);
void oasp_memory_release(oasp_memory_manager *manager, size_t amount);
const char *oasp_memory_error(const oasp_memory_manager *manager);

bool oasp_arena_bind_memory(oasp_arena *arena, oasp_memory_manager *manager);
void oasp_arena_unbind_memory(oasp_arena *arena);
oasp_memory_manager *oasp_request_memory_manager(oasp_request *request);

#endif
