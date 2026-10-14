// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "openasp/ext_manifest.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * Isolated mode has a distinct failure path so the in-process loader cannot
 * silently weaken a manifest's crash-isolation requirement. The component-host
 * transport will replace this gate without changing loader policy.
 */
bool oasp_external_isolated_proxy_load(const oasp_external_module_config *module, char *error, size_t error_size) {
    snprintf(error, error_size, "external component '%s' requests isolated mode, but openasp-component-host is unavailable",
             module && module->id ? module->id : "");
    return false;
}
