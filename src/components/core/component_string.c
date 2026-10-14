// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component_internal.h"

#include <string.h>

oasp_slice oasp_component_string(oasp_runtime *runtime, const char *text) {
    size_t length = text ? strlen(text) : 0;
    char *copy = oasp_runtime_copy(runtime, text ? text : "", length);
    return (oasp_slice){copy ? copy : "", copy ? length : 0};
}
