// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXT_API_H
#define OPENASP_EXT_API_H

#include <stddef.h>
#include <stdint.h>

#define OASP_EXTENSION_ABI_MAJOR 1u
#define OASP_EXTENSION_ABI_MINOR 1u

typedef struct oasp_host_api oasp_host_api;
typedef struct oasp_extension_descriptor oasp_extension_descriptor;
typedef struct oasp_extension_class oasp_extension_class;
typedef struct oasp_extension_member oasp_extension_member;
typedef struct oasp_value_handle oasp_value_handle;

typedef struct {
    const void *data;
    size_t length;
} oasp_ext_bytes;

typedef enum {
    OASP_EXT_VALUE_EMPTY,
    OASP_EXT_VALUE_NULL,
    OASP_EXT_VALUE_BOOL,
    OASP_EXT_VALUE_I64,
    OASP_EXT_VALUE_F64,
    OASP_EXT_VALUE_DATE,
    OASP_EXT_VALUE_STRING,
    OASP_EXT_VALUE_BYTES,
    OASP_EXT_VALUE_ARRAY,
    OASP_EXT_VALUE_OBJECT
} oasp_ext_value_kind;

typedef struct {
    uint32_t struct_size;
    int32_t code;
    const char *message;
    const char *source;
} oasp_ext_error;

typedef void (*oasp_trace_visitor)(void *context, const oasp_value_handle *value);

struct oasp_extension_member {
    uint32_t struct_size;
    const char *name;
    uint32_t member_id;
};

struct oasp_extension_class {
    uint32_t struct_size;
    const char *prog_id;
    const char *canonical_name;
    const oasp_extension_member *members;
    size_t member_count;
    int (*create)(void *worker_state, void **instance, oasp_ext_error *error);
    void (*destroy)(void *instance);
    int (*get)(void *instance, uint32_t member_id, oasp_value_handle **result, oasp_ext_error *error);
    int (*set)(void *instance, uint32_t member_id, const oasp_value_handle *value, oasp_ext_error *error);
    int (*call)(void *instance, uint32_t member_id, const oasp_value_handle *const *args, size_t argc, oasp_value_handle **result,
                oasp_ext_error *error);
    void (*trace)(void *instance, oasp_trace_visitor visit, void *context);
};

struct oasp_extension_descriptor {
    uint32_t struct_size;
    uint32_t abi_major_required;
    uint32_t abi_minor_required;
    const char *module_name;
    const char *module_version;
    uint64_t capabilities;
    int (*module_init)(const oasp_host_api *host, void **module_state);
    int (*worker_init)(void *module_state, void **worker_state);
    void (*worker_shutdown)(void *worker_state);
    void (*module_shutdown)(void *module_state);
    const oasp_extension_class *classes;
    size_t class_count;
};

struct oasp_host_api {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    oasp_ext_value_kind (*value_kind)(const oasp_value_handle *value);
    int (*value_get_bool)(const oasp_value_handle *value, int *result);
    int (*value_get_i64)(const oasp_value_handle *value, int64_t *result);
    int (*value_get_f64)(const oasp_value_handle *value, double *result);
    int (*value_get_bytes)(const oasp_value_handle *value, oasp_ext_bytes *result);
    oasp_value_handle *(*value_new_empty)(void);
    oasp_value_handle *(*value_new_null)(void);
    oasp_value_handle *(*value_new_bool)(int value);
    oasp_value_handle *(*value_new_i64)(int64_t value);
    oasp_value_handle *(*value_new_f64)(double value);
    oasp_value_handle *(*value_new_string)(const void *data, size_t length);
    oasp_value_handle *(*value_new_array)(size_t length);
    int (*value_array_get)(const oasp_value_handle *value, size_t index, oasp_value_handle **result);
    int (*value_array_set)(oasp_value_handle *value, size_t index, const oasp_value_handle *item);
    int (*value_object_get)(const oasp_value_handle *value, const char *member, size_t member_length, oasp_value_handle **result, oasp_ext_error *error);
    int (*value_object_call)(const oasp_value_handle *value, const char *member, size_t member_length, const oasp_value_handle *const *args, size_t argc,
                             oasp_value_handle **result, oasp_ext_error *error);
    int (*value_retain)(const oasp_value_handle *value);
    void (*value_release)(const oasp_value_handle *value);
    int (*account_external)(ptrdiff_t delta);
    void (*log)(int level, const char *message);
};

#endif
