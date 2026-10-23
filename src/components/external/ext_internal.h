// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXTERNAL_INTERNAL_H
#define OPENASP_EXTERNAL_INTERNAL_H

#include "ext_loader.h"
#include "openasp/ext_api.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct oasp_ext_map_entry {
    char *key;
    size_t key_length;
    oasp_value value;
} oasp_ext_map_entry;

typedef struct oasp_ext_handle_table oasp_ext_handle_table;

struct oasp_ext_value {
    oasp_value value;
    oasp_ext_value_kind public_kind;
    size_t references;
    void *owned_data;
    size_t owned_length;
    oasp_ext_map_entry *map_entries;
    size_t map_count;
    size_t map_capacity;
    oasp_ext_handle_table *table;
    struct oasp_ext_value *previous;
    struct oasp_ext_value *next;
};

struct oasp_ext_future {
    const oasp_ext_future_vtable *vtable;
    void *state;
    bool completed;
};

struct oasp_ext_stream {
    const oasp_ext_stream_vtable *vtable;
    void *state;
    bool completed;
};

struct oasp_ext_handle_table {
    oasp_ext_value *first;
    oasp_runtime *runtime;
    void *owner;
    bool closing;
};

typedef struct oasp_external_module {
    const oasp_external_module_config *config;
    void *library;
    const oasp_extension_descriptor *descriptor;
    void *module_state;
    void *worker_state;
    bool worker_started;
    bool isolated;
    void *isolated_state;
    atomic_size_t worker_external_bytes;
    atomic_size_t model_external_bytes;
    atomic_size_t device_external_bytes;
} oasp_external_module;

typedef struct {
    uint64_t hash;
    const oasp_extension_member *member;
} oasp_external_member_slot;

typedef struct {
    oasp_external_module *module;
    const oasp_extension_class *class_definition;
    oasp_external_member_slot *member_slots;
    size_t member_slot_count;
} oasp_external_class;

typedef struct {
    uint64_t hash;
    oasp_external_class *class_definition;
} oasp_external_class_slot;

struct oasp_external_registry {
    oasp_external_module *modules;
    size_t module_count;
    oasp_external_class *classes;
    size_t class_count;
    oasp_external_class_slot *class_slots;
    size_t class_slot_count;
};

typedef struct {
    oasp_external_class *class_definition;
    void *instance;
    void *request_state;
    oasp_ext_handle_table handles;
    uint64_t remote_id;
    bool remote_proxy;
    bool destroyed;
} oasp_external_object;

typedef struct oasp_external_request_context {
    struct oasp_external_request_context *next;
    oasp_external_module *module;
    oasp_runtime *runtime;
    void *state;
} oasp_external_request_context;

struct oasp_ext_call_context {
    oasp_runtime *runtime;
    oasp_external_module *module;
    oasp_external_object *object;
    void *resource_owner;
    oasp_ext_handle_table *handles;
    size_t line;
    int64_t deadline_milliseconds;
    bool cancelled;
    int (*wrap_owned)(struct oasp_ext_call_context *context, uint32_t class_id, void *instance, oasp_ext_value **result, oasp_ext_error *error);
    void *wrap_owned_data;
};

bool oasp_external_ascii_equal(oasp_slice left, const char *right);
uint64_t oasp_external_ascii_hash(const char *data, size_t length);
void oasp_external_error(oasp_runtime *runtime, const oasp_ext_error *error, const char *fallback, size_t line);

void oasp_external_context_init(oasp_ext_call_context *context, oasp_runtime *runtime, oasp_external_module *module, oasp_external_object *object, oasp_ext_handle_table *handles,
                                size_t line);
void oasp_external_context_set_timeout(oasp_ext_call_context *context, uint64_t timeout_milliseconds);
bool oasp_external_context_cancelled(oasp_ext_call_context *context);
int64_t oasp_external_context_deadline(oasp_ext_call_context *context);
oasp_ext_value *oasp_external_value_create(oasp_ext_call_context *context, oasp_value value, oasp_ext_value_kind kind);
bool oasp_external_value_unwrap(oasp_ext_call_context *context, const oasp_ext_value *value, oasp_value *result);
void oasp_external_value_table_dispose(oasp_ext_handle_table *table);
void oasp_external_value_table_trace(oasp_allocator *allocator, const oasp_ext_handle_table *table);

const oasp_ext_host *oasp_external_host_api(void);
const oasp_ext_value_api *oasp_external_value_api(void);
const oasp_ext_object_api *oasp_external_object_api(void);
const oasp_ext_async_api *oasp_external_async_api(void);
const oasp_ext_runtime_api *oasp_external_runtime_api(void);

bool oasp_external_descriptor_validate(const oasp_external_module_config *config, const oasp_extension_descriptor *descriptor, char *error, size_t error_size);
bool oasp_external_class_prepare(oasp_external_class *external_class, char *error, size_t error_size);
void oasp_external_class_dispose(oasp_external_class *external_class);
const oasp_extension_member *oasp_external_member_find(const oasp_external_class *external_class, oasp_slice member);
oasp_external_class *oasp_external_class_find(const oasp_external_registry *registry, oasp_slice prog_id);
oasp_external_class *oasp_external_class_find_id(const oasp_external_registry *registry, oasp_external_module *module, uint32_t class_id);
const oasp_extension_class *oasp_external_module_class_find(const oasp_external_module *module, uint32_t class_id);
int oasp_external_object_wrap_owned(oasp_ext_call_context *context, uint32_t class_id, void *instance, oasp_ext_value **result, oasp_ext_error *error);
int oasp_external_object_get_native(oasp_ext_call_context *context, const oasp_ext_value *value, uint32_t class_id, void **instance, oasp_ext_error *error);
bool oasp_external_object_remote_reference(const oasp_value *value, const oasp_external_module *module, uint32_t *class_id, uint64_t *remote_id);

int oasp_external_await_result(oasp_ext_call_context *context, oasp_ext_result *extension_result, oasp_value *result, oasp_ext_error *error);
bool oasp_external_request_state(oasp_runtime *runtime, oasp_external_module *module, size_t line, void **state, oasp_ext_error *error);

#endif
