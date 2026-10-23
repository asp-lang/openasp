// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXTERNAL_H
#define OPENASP_EXTERNAL_H

#include "component.h"
#include "openasp/ext_manifest.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct oasp_external_registry oasp_external_registry;
typedef struct oasp_external_module oasp_external_module;
typedef struct oasp_ext_call_context oasp_ext_call_context;
typedef struct oasp_ext_invocation oasp_ext_invocation;
typedef struct oasp_ext_error oasp_ext_error;
typedef struct oasp_extension_class oasp_extension_class;

bool oasp_external_registry_create(oasp_external_registry **result, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                   size_t error_size);
void oasp_external_registry_destroy(oasp_external_registry *registry);
bool oasp_external_registry_worker_init(oasp_external_registry *registry, char *error, size_t error_size);
void oasp_external_registry_worker_shutdown(oasp_external_registry *registry);
bool oasp_external_isolated_proxy_load(oasp_external_module *module, char *error, size_t error_size);
void oasp_external_isolated_proxy_shutdown(oasp_external_module *module);
int oasp_external_isolated_host_main(int argc, char **argv);
int oasp_external_isolated_request_init(oasp_ext_call_context *context, void **request_state, oasp_ext_error *error);
void oasp_external_isolated_request_shutdown(oasp_ext_call_context *context, void *request_state);
int oasp_external_isolated_create(oasp_ext_call_context *context, const oasp_extension_class *class_definition, void *request_state, void **instance, oasp_ext_error *error);
void oasp_external_isolated_destroy(oasp_ext_call_context *context, void *instance);
int oasp_external_isolated_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_value *result, oasp_ext_error *error);
int oasp_external_isolated_snapshot(oasp_ext_call_context *context, void *instance, oasp_value *state, oasp_ext_error *error);
int oasp_external_isolated_restore(oasp_ext_call_context *context, void *instance, const oasp_value *state, oasp_ext_error *error);

bool oasp_external_component_create(oasp_runtime *runtime, oasp_component_context *context, oasp_slice prog_id, size_t line, oasp_value *result);
bool oasp_external_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
bool oasp_external_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_external_component_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                    const oasp_value *value, size_t line);
bool oasp_external_component_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                  size_t line, oasp_value *result);
bool oasp_external_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                                     oasp_value *result);
bool oasp_external_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result);
bool oasp_external_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result);
void oasp_external_component_gc_trace(oasp_allocator *allocator, const oasp_object *object);

#endif
