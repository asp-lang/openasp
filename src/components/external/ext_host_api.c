// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ext_internal.h"

#include "log.h"

static void host_log(int level, const char *message) {
    oasp_log_write(level >= 0 && level <= 4 ? (oasp_log_level)level : OASP_LOG_ERROR, "%s", message ? message : "");
}

const oasp_host_api *oasp_external_host_api(void) {
    static oasp_host_api api = {
        sizeof(api), OASP_EXTENSION_ABI_MAJOR, OASP_EXTENSION_ABI_MINOR,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, oasp_external_host_account_external, host_log
    };
    static bool initialized;
    if (!initialized) {
        oasp_external_host_value_api(&api);
        initialized = true;
    }
    return &api;
}
