// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/components/component.h`): Native component boundary. It maps
 * ASP-compatible object operations onto C libraries while preserving runtime error semantics,
 * ownership, and garbage-collector tracing. Result and error variables are initialized before
 * crossing the boundary.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_COMPONENT_H
#define OPENASP_COMPONENT_H

#include "runtime.h"

typedef struct oasp_component_context oasp_component_context;
typedef struct oasp_external_module_config oasp_external_module_config;
typedef struct oasp_engine oasp_engine;

typedef struct {
    const char *name;
    oasp_component_kind kind;
    const char *canonical_name;
} oasp_component_registration;

const oasp_component_registration *oasp_builtin_component_registration(oasp_slice prog_id);

/*
 * Every native component, including legacy built-ins, is invoked through this
 * table. A NULL optional hook means that operation is not supported.
 */
struct oasp_component_vtable {
    bool (*get)(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
    bool (*get_id)(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                   oasp_value *result);
    bool (*set)(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
    bool (*call)(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                 size_t argument_count, size_t line, oasp_value *result);
    bool (*default_member)(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                           oasp_value *result);
    bool (*enumerate)(oasp_runtime *runtime, oasp_object *object, oasp_value *result);
    bool (*persist_copy)(oasp_runtime *runtime, const oasp_object *source, oasp_object **result);
    void (*gc_trace)(oasp_allocator *allocator, const oasp_object *object);
};

const oasp_component_vtable *oasp_builtin_component_vtable(void);
const oasp_component_vtable *oasp_external_component_vtable(void);

oasp_component_context *oasp_component_context_create(void);
void oasp_component_context_destroy(oasp_component_context *context);
void oasp_component_context_release_memory(oasp_component_context *context);
bool oasp_component_context_load_external(oasp_component_context *context, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                          size_t error_size);
bool oasp_component_context_external_worker_init(oasp_component_context *context, char *error, size_t error_size);
void oasp_component_context_external_worker_shutdown(oasp_component_context *context);
void *oasp_component_context_external_registry(const oasp_component_context *context);
bool oasp_engine_load_external_components(oasp_engine *engine, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                          size_t error_size);
bool oasp_engine_external_worker_init(oasp_engine *engine, char *error, size_t error_size);
void oasp_engine_external_worker_shutdown(oasp_engine *engine);
oasp_value oasp_component_create(oasp_runtime *runtime, oasp_slice prog_id, size_t line);
bool oasp_component_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
bool oasp_component_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                           oasp_value *result);
bool oasp_component_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_component_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                         size_t line, oasp_value *result);
bool oasp_component_call_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                            size_t argument_count, size_t line, oasp_value *result);
bool oasp_component_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                            oasp_value *result);
bool oasp_component_recordset_field(oasp_runtime *runtime, oasp_object *object, oasp_slice column, size_t line, oasp_value *result);
bool oasp_component_recordset_field_view(oasp_runtime *runtime, oasp_object *object, oasp_slice column, int *ordinal, const void **schema,
                                         bool *copy_text, size_t line, const oasp_value **result);
bool oasp_component_recordset_move_next(oasp_runtime *runtime, oasp_object *object, size_t line);
bool oasp_component_recordset_eof(oasp_runtime *runtime, oasp_object *object, size_t line, bool *result);
bool oasp_component_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           const oasp_value *value, size_t line);
bool oasp_component_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result);
bool oasp_component_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result);
bool oasp_component_dictionary_info(const oasp_object *object, int *compare_mode, size_t *count);
bool oasp_component_dictionary_item(const oasp_object *object, size_t index, const oasp_value **key, const oasp_value **value);
oasp_object *oasp_component_dictionary_restore(oasp_runtime *runtime, int compare_mode, const oasp_value *keys, const oasp_value *values, size_t count);
bool oasp_component_xml_snapshot(const oasp_object *object, char **data, size_t *length);
bool oasp_component_xml_restore(oasp_runtime *runtime, oasp_slice data, oasp_object **result);
void oasp_component_gc_trace(oasp_allocator *allocator, const oasp_object *object);

#endif
