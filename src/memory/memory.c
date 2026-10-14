// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "memory.h"

#include <stdio.h>
#include <string.h>

void oasp_memory_manager_init(oasp_memory_manager *manager, size_t limit) {
    if (!manager)
        return;
    memset(manager, 0, sizeof(*manager));
    manager->limit = limit;
}

bool oasp_memory_reserve(oasp_memory_manager *manager, size_t amount, const char *category) {
    if (!manager || !amount)
        return true;
    if (amount > manager->limit || manager->current > manager->limit - amount) {
        manager->allocation_failures++;
        manager->failed_requested = amount;
        manager->failed_current = manager->current;
        manager->failed_category = category ? category : "memory";
        (void)snprintf(manager->error, sizeof(manager->error),
                       "allowed memory size of %zu bytes exhausted "
                       "(current usage %zu bytes, tried to allocate %zu bytes for %s)",
                       manager->limit, manager->failed_current, manager->failed_requested,
                       manager->failed_category);
        return false;
    }
    manager->current += amount;
    if (manager->current > manager->peak)
        manager->peak = manager->current;
    return true;
}

void oasp_memory_release(oasp_memory_manager *manager, size_t amount) {
    if (!manager)
        return;
    manager->current = amount < manager->current ? manager->current - amount : 0;
}

const char *oasp_memory_error(const oasp_memory_manager *manager) {
    if (!manager || !manager->allocation_failures)
        return NULL;
    return manager->error;
}
