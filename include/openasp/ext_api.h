// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXT_API_H
#define OPENASP_EXT_API_H

#include <stddef.h>
#include <stdint.h>

#define OASP_EXTENSION_ABI_MAJOR 2u
#define OASP_EXTENSION_ABI_MINOR 1u

#define OASP_EXT_API_VALUE "openasp.value"
#define OASP_EXT_API_OBJECT "openasp.object"
#define OASP_EXT_API_ASYNC "openasp.async"
#define OASP_EXT_API_RUNTIME "openasp.runtime"

#define OASP_EXT_API_VALUE_VERSION 1u
#define OASP_EXT_API_OBJECT_VERSION 2u
#define OASP_EXT_API_ASYNC_VERSION 1u
#define OASP_EXT_API_RUNTIME_VERSION 2u

typedef struct oasp_ext_call_context oasp_ext_call_context;
typedef struct oasp_ext_value oasp_ext_value;
typedef struct oasp_ext_future oasp_ext_future;
typedef struct oasp_ext_stream oasp_ext_stream;
typedef struct oasp_ext_host oasp_ext_host;
typedef struct oasp_ext_value_api oasp_ext_value_api;
typedef struct oasp_ext_object_api oasp_ext_object_api;
typedef struct oasp_ext_async_api oasp_ext_async_api;
typedef struct oasp_ext_runtime_api oasp_ext_runtime_api;
typedef struct oasp_extension_descriptor oasp_extension_descriptor;
typedef struct oasp_extension_class oasp_extension_class;
typedef struct oasp_extension_member oasp_extension_member;

typedef struct {
    const void *data;
    size_t length;
} oasp_ext_bytes;

typedef struct oasp_ext_error {
    uint32_t struct_size;
    int32_t code;
    int32_t category;
    const char *message;
    size_t message_length;
    const char *source;
    size_t source_length;
    const char *detail;
    size_t detail_length;
} oasp_ext_error;

typedef enum {
    OASP_EXT_OK = 0,
    OASP_EXT_ERROR = -1,
    OASP_EXT_PENDING = 1,
    OASP_EXT_END = 2,
    OASP_EXT_CANCELLED = 3
} oasp_ext_status;

typedef enum {
    OASP_EXT_ERROR_RUNTIME = 1,
    OASP_EXT_ERROR_ARGUMENT,
    OASP_EXT_ERROR_PERMISSION,
    OASP_EXT_ERROR_TIMEOUT,
    OASP_EXT_ERROR_CANCELLED,
    OASP_EXT_ERROR_TRANSPORT,
    OASP_EXT_ERROR_RESOURCE
} oasp_ext_error_category;

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
    OASP_EXT_VALUE_MAP,
    OASP_EXT_VALUE_OBJECT
} oasp_ext_value_kind;

typedef enum {
    OASP_EXT_OP_GET,
    OASP_EXT_OP_SET,
    OASP_EXT_OP_CALL,
    OASP_EXT_OP_DEFAULT_GET,
    OASP_EXT_OP_DEFAULT_SET,
    OASP_EXT_OP_INDEX_GET,
    OASP_EXT_OP_INDEX_SET,
    OASP_EXT_OP_ENUMERATE
} oasp_ext_operation;

typedef enum {
    OASP_EXT_ASYNC_READY,
    OASP_EXT_ASYNC_PENDING,
    OASP_EXT_ASYNC_FAILED,
    OASP_EXT_ASYNC_CANCELLED
} oasp_ext_async_state;

typedef enum {
    OASP_EXT_RESOURCE_REQUEST,
    OASP_EXT_RESOURCE_WORKER,
    OASP_EXT_RESOURCE_MODEL,
    OASP_EXT_RESOURCE_DEVICE
} oasp_ext_resource_scope;

enum {
    OASP_EXT_MEMBER_GET = UINT32_C(1) << 0,
    OASP_EXT_MEMBER_SET = UINT32_C(1) << 1,
    OASP_EXT_MEMBER_CALL = UINT32_C(1) << 2,
    OASP_EXT_MEMBER_DEFAULT = UINT32_C(1) << 3,
    OASP_EXT_MEMBER_INDEXED = UINT32_C(1) << 4
};

enum {
    OASP_EXT_CLASS_ENUMERABLE = UINT32_C(1) << 0,
    OASP_EXT_CLASS_PERSISTABLE = UINT32_C(1) << 1,
    OASP_EXT_CLASS_ASYNC = UINT32_C(1) << 2,
    OASP_EXT_CLASS_STREAMING = UINT32_C(1) << 3,
    OASP_EXT_CLASS_INTERNAL = UINT32_C(1) << 4
};

enum {
    OASP_EXT_CAP_CRYPTO = UINT64_C(1) << 0,
    OASP_EXT_CAP_FILESYSTEM = UINT64_C(1) << 1,
    OASP_EXT_CAP_NETWORK = UINT64_C(1) << 2,
    OASP_EXT_CAP_DATABASE = UINT64_C(1) << 3,
    OASP_EXT_CAP_PROCESS = UINT64_C(1) << 4,
    OASP_EXT_CAP_THREADS = UINT64_C(1) << 5
};

typedef struct oasp_ext_invocation {
    uint32_t struct_size;
    oasp_ext_operation operation;
    uint32_t member_id;
    const oasp_ext_value *value;
    const oasp_ext_value *const *arguments;
    size_t argument_count;
} oasp_ext_invocation;

typedef struct oasp_ext_result {
    uint32_t struct_size;
    oasp_ext_value *value;
    oasp_ext_future *future;
    oasp_ext_stream *stream;
} oasp_ext_result;

typedef struct {
    uint32_t struct_size;
    oasp_ext_async_state (*poll)(void *state, oasp_ext_call_context *context, oasp_ext_value **result, oasp_ext_error *error);
    void (*cancel)(void *state);
    void (*destroy)(void *state);
} oasp_ext_future_vtable;

typedef struct {
    uint32_t struct_size;
    oasp_ext_status (*next)(void *state, oasp_ext_call_context *context, oasp_ext_value **result, oasp_ext_error *error);
    void (*cancel)(void *state);
    void (*destroy)(void *state);
} oasp_ext_stream_vtable;

struct oasp_ext_host {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    const void *(*query_api)(const char *name, uint32_t minimum_version, uint32_t *actual_version);
};

struct oasp_ext_value_api {
    uint32_t struct_size;
    uint32_t version;
    oasp_ext_value_kind (*kind)(oasp_ext_call_context *context, const oasp_ext_value *value);
    int (*get_bool)(oasp_ext_call_context *context, const oasp_ext_value *value, int *result);
    int (*get_i64)(oasp_ext_call_context *context, const oasp_ext_value *value, int64_t *result);
    int (*get_f64)(oasp_ext_call_context *context, const oasp_ext_value *value, double *result);
    int (*get_data)(oasp_ext_call_context *context, const oasp_ext_value *value, oasp_ext_bytes *result);
    oasp_ext_value *(*new_empty)(oasp_ext_call_context *context);
    oasp_ext_value *(*new_null)(oasp_ext_call_context *context);
    oasp_ext_value *(*new_bool)(oasp_ext_call_context *context, int value);
    oasp_ext_value *(*new_i64)(oasp_ext_call_context *context, int64_t value);
    oasp_ext_value *(*new_f64)(oasp_ext_call_context *context, double value);
    oasp_ext_value *(*new_date)(oasp_ext_call_context *context, const void *data, size_t length);
    oasp_ext_value *(*new_string)(oasp_ext_call_context *context, const void *data, size_t length);
    oasp_ext_value *(*new_bytes)(oasp_ext_call_context *context, const void *data, size_t length);
    oasp_ext_value *(*new_array)(oasp_ext_call_context *context, const int64_t *upper_bounds, size_t rank);
    size_t (*array_rank)(oasp_ext_call_context *context, const oasp_ext_value *value);
    int (*array_bounds)(oasp_ext_call_context *context, const oasp_ext_value *value, int64_t *upper_bounds, size_t rank);
    int (*array_get)(oasp_ext_call_context *context, const oasp_ext_value *value, const int64_t *indices, size_t rank, oasp_ext_value **result);
    int (*array_set)(oasp_ext_call_context *context, oasp_ext_value *value, const int64_t *indices, size_t rank, const oasp_ext_value *item);
    oasp_ext_value *(*new_map)(oasp_ext_call_context *context);
    size_t (*map_size)(oasp_ext_call_context *context, const oasp_ext_value *value);
    int (*map_get)(oasp_ext_call_context *context, const oasp_ext_value *value, const void *key, size_t key_length, oasp_ext_value **result);
    int (*map_set)(oasp_ext_call_context *context, oasp_ext_value *value, const void *key, size_t key_length, const oasp_ext_value *item);
    int (*retain)(oasp_ext_call_context *context, const oasp_ext_value *value);
    void (*release)(oasp_ext_call_context *context, const oasp_ext_value *value);
};

struct oasp_ext_object_api {
    uint32_t struct_size;
    uint32_t version;
    int (*get)(oasp_ext_call_context *context, const oasp_ext_value *object, const char *member, size_t member_length, oasp_ext_value **result,
               oasp_ext_error *error);
    int (*set)(oasp_ext_call_context *context, const oasp_ext_value *object, const char *member, size_t member_length, const oasp_ext_value *value,
               oasp_ext_error *error);
    int (*call)(oasp_ext_call_context *context, const oasp_ext_value *object, const char *member, size_t member_length, const oasp_ext_value *const *arguments,
                size_t argument_count, oasp_ext_value **result, oasp_ext_error *error);
    int (*invoke_default)(oasp_ext_call_context *context, const oasp_ext_value *object, const oasp_ext_value *const *arguments, size_t argument_count, oasp_ext_value **result,
                          oasp_ext_error *error);
    int (*new_owned)(oasp_ext_call_context *context, uint32_t class_id, void *instance, oasp_ext_value **result, oasp_ext_error *error);
    int (*get_native)(oasp_ext_call_context *context, const oasp_ext_value *object, uint32_t class_id, void **instance, oasp_ext_error *error);
};

struct oasp_ext_async_api {
    uint32_t struct_size;
    uint32_t version;
    oasp_ext_future *(*future_create)(oasp_ext_call_context *context, const oasp_ext_future_vtable *vtable, void *state);
    oasp_ext_stream *(*stream_create)(oasp_ext_call_context *context, const oasp_ext_stream_vtable *vtable, void *state);
    int (*cancelled)(oasp_ext_call_context *context);
    int64_t (*deadline_milliseconds)(oasp_ext_call_context *context);
    void (*yield)(oasp_ext_call_context *context, uint32_t maximum_wait_milliseconds);
};

struct oasp_ext_runtime_api {
    uint32_t struct_size;
    uint32_t version;
    uint64_t (*granted_capabilities)(oasp_ext_call_context *context);
    int (*require_capability)(oasp_ext_call_context *context, uint64_t capability, oasp_ext_error *error);
    int (*account_external)(oasp_ext_call_context *context, ptrdiff_t delta);
    void (*log)(oasp_ext_call_context *context, int level, const char *message, size_t message_length);
    int (*account_resource)(oasp_ext_call_context *context, oasp_ext_resource_scope scope, const char *resource_id, size_t resource_id_length, ptrdiff_t delta,
                            oasp_ext_error *error);
};

struct oasp_extension_member {
    uint32_t struct_size;
    const char *name;
    uint32_t member_id;
    uint32_t flags;
};

struct oasp_extension_class {
    uint32_t struct_size;
    const char *prog_id;
    const char *canonical_name;
    uint32_t class_id;
    uint32_t flags;
    const oasp_extension_member *members;
    size_t member_count;
    int (*create)(oasp_ext_call_context *context, void *worker_state, void *request_state, void **instance, oasp_ext_error *error);
    void (*destroy)(oasp_ext_call_context *context, void *instance);
    int (*invoke)(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error);
    int (*snapshot)(oasp_ext_call_context *context, void *instance, oasp_ext_value **state, oasp_ext_error *error);
    int (*restore)(oasp_ext_call_context *context, void *worker_state, void *request_state, const oasp_ext_value *state, void **instance,
                   oasp_ext_error *error);
};

struct oasp_extension_descriptor {
    uint32_t struct_size;
    uint32_t abi_major_required;
    uint32_t abi_minor_required;
    const char *module_name;
    const char *module_version;
    uint64_t required_capabilities;
    int (*module_init)(const oasp_ext_host *host, uint64_t granted_capabilities, void **module_state, oasp_ext_error *error);
    void (*module_shutdown)(void *module_state);
    int (*worker_init)(void *module_state, void **worker_state, oasp_ext_error *error);
    void (*worker_shutdown)(void *worker_state);
    int (*request_init)(oasp_ext_call_context *context, void *worker_state, void **request_state, oasp_ext_error *error);
    void (*request_shutdown)(oasp_ext_call_context *context, void *request_state);
    const oasp_extension_class *classes;
    size_t class_count;
};

#if defined(_WIN32)
#define OASP_EXTENSION_EXPORT __declspec(dllexport)
#else
#define OASP_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

typedef const oasp_extension_descriptor *(*oasp_extension_entry)(void);
OASP_EXTENSION_EXPORT const oasp_extension_descriptor *openasp_extension(void);

#endif
