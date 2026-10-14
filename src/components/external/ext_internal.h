// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXTERNAL_INTERNAL_H
#define OPENASP_EXTERNAL_INTERNAL_H

#include "ext_loader.h"
#include "openasp/ext_api.h"

typedef const oasp_extension_descriptor *(*oasp_extension_entry)(void);

typedef struct {
    const oasp_external_module_config *config;
    void *library;
    const oasp_extension_descriptor *descriptor;
    void *module_state;
    void *worker_state;
} oasp_external_module;

typedef struct {
    oasp_external_module *module;
    const oasp_extension_class *class_definition;
} oasp_external_class;

struct oasp_external_registry {
    oasp_external_module *modules;
    size_t module_count;
    oasp_external_class *classes;
    size_t class_count;
};

typedef struct {
    oasp_value value;
    size_t references;
    bool released;
} oasp_value_handle_impl;

typedef struct {
    oasp_external_class *class_definition;
    void *instance;
} oasp_external_object;

bool oasp_external_ascii_equal(oasp_slice left, const char *right);
void oasp_external_error(oasp_runtime *runtime, const oasp_ext_error *error, const char *fallback, size_t line);

void oasp_external_call_enter(oasp_runtime *runtime, void *owner);
void oasp_external_call_leave(void);
oasp_value_handle *oasp_external_value_create(oasp_value value);
void oasp_external_host_value_api(oasp_host_api *api);
int oasp_external_host_account_external(ptrdiff_t delta);
const oasp_host_api *oasp_external_host_api(void);

#endif
