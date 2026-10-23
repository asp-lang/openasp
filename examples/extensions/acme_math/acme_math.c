// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include <openasp/ext_api.h>

#include <signal.h>
#include <stdlib.h>

#ifndef ACME_ABI_MAJOR
#define ACME_ABI_MAJOR OASP_EXTENSION_ABI_MAJOR
#endif

#ifndef ACME_REQUIRED_CAPABILITIES
#define ACME_REQUIRED_CAPABILITIES 0
#endif

enum {
    MEMBER_VERSION = 1,
    MEMBER_SCALE = 2,
    MEMBER_ADD = 3,
    MEMBER_CAPTURE = 4,
    MEMBER_CAPTURED_COUNT = 5,
    MEMBER_ASYNC_ADD = 6,
    MEMBER_SEQUENCE = 7,
    MEMBER_BYTES = 8,
    MEMBER_MATRIX = 9,
    MEMBER_MAP = 10,
    MEMBER_DEADLINE = 11,
    MEMBER_BLOCK = 12,
    MEMBER_CRASH = 13
};

typedef struct {
    int64_t scale;
    const oasp_ext_value *captured;
} acme_math;

static const oasp_ext_value_api *values;
static const oasp_ext_object_api *objects;
static const oasp_ext_async_api *async;

typedef struct {
    int polls;
    int64_t value;
} acme_future;

typedef struct {
    int64_t next;
    int64_t end;
} acme_stream;

static int module_init(const oasp_ext_host *host, uint64_t capabilities, void **state, oasp_ext_error *error) {
    uint32_t version = 0;
    (void)capabilities;
    (void)error;
    values = host->query_api(OASP_EXT_API_VALUE, OASP_EXT_API_VALUE_VERSION, &version);
    objects = host->query_api(OASP_EXT_API_OBJECT, OASP_EXT_API_OBJECT_VERSION, &version);
    async = host->query_api(OASP_EXT_API_ASYNC, OASP_EXT_API_ASYNC_VERSION, &version);
    *state = NULL;
    return values && objects && async ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int create(oasp_ext_call_context *context, void *worker_state, void *request_state, void **instance, oasp_ext_error *error) {
    (void)context;
    (void)worker_state;
    (void)request_state;
    (void)error;
    acme_math *math = calloc(1, sizeof(*math));
    if (!math)
        return OASP_EXT_ERROR;
    math->scale = 1;
    *instance = math;
    return OASP_EXT_OK;
}

static void destroy(oasp_ext_call_context *context, void *instance) {
    acme_math *math = instance;
    if (math && math->captured)
        values->release(context, math->captured);
    free(instance);
}

static int get(oasp_ext_call_context *context, acme_math *math, uint32_t member_id, oasp_ext_result *result, oasp_ext_error *error) {
    (void)error;
    if (member_id == MEMBER_VERSION)
        result->value = values->new_string(context, "1.0.0", 5);
    else if (member_id == MEMBER_SCALE)
        result->value = values->new_i64(context, math->scale);
    else if (member_id == MEMBER_CAPTURED_COUNT && math->captured)
        return objects->get(context, math->captured, "Count", 5, &result->value, error);
    else
        return OASP_EXT_ERROR;
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int set(oasp_ext_call_context *context, acme_math *math, uint32_t member_id, const oasp_ext_value *value, oasp_ext_error *error) {
    if (member_id != MEMBER_SCALE || values->get_i64(context, value, &math->scale) != OASP_EXT_OK) {
        error->code = 5;
        error->category = OASP_EXT_ERROR_ARGUMENT;
        error->message = "Scale requires an integer";
        error->message_length = 25;
        error->source = "Acme.Math";
        error->source_length = 9;
        return OASP_EXT_ERROR;
    }
    return OASP_EXT_OK;
}

static oasp_ext_async_state future_poll(void *state, oasp_ext_call_context *context, oasp_ext_value **result, oasp_ext_error *error) {
    acme_future *future = state;
    (void)error;
    if (!future->polls++)
        return OASP_EXT_ASYNC_PENDING;
    *result = values->new_i64(context, future->value);
    return *result ? OASP_EXT_ASYNC_READY : OASP_EXT_ASYNC_FAILED;
}

static void future_cancel(void *state) {
    (void)state;
}

static void future_destroy(void *state) {
    free(state);
}

static const oasp_ext_future_vtable future_vtable = {
    sizeof(future_vtable), future_poll, future_cancel, future_destroy
};

static oasp_ext_status stream_next(void *state, oasp_ext_call_context *context, oasp_ext_value **result, oasp_ext_error *error) {
    acme_stream *stream = state;
    (void)error;
    if (stream->next > stream->end)
        return OASP_EXT_END;
    *result = values->new_i64(context, stream->next++);
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static void stream_cancel(void *state) {
    (void)state;
}

static void stream_destroy(void *state) {
    free(state);
}

static const oasp_ext_stream_vtable stream_vtable = {
    sizeof(stream_vtable), stream_next, stream_cancel, stream_destroy
};

static int call(oasp_ext_call_context *context, acme_math *math, uint32_t member_id, const oasp_ext_value *const *arguments, size_t argument_count,
                oasp_ext_result *result, oasp_ext_error *error) {
    int64_t left;
    int64_t right;
    if (member_id == MEMBER_ADD && argument_count == 2 &&
        values->get_i64(context, arguments[0], &left) == OASP_EXT_OK &&
        values->get_i64(context, arguments[1], &right) == OASP_EXT_OK) {
        result->value = values->new_i64(context, (left + right) * math->scale);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member_id == MEMBER_CAPTURE && argument_count == 1) {
        if (math->captured)
            values->release(context, math->captured);
        if (values->retain(context, arguments[0]) != OASP_EXT_OK) {
            math->captured = NULL;
            return OASP_EXT_ERROR;
        }
        math->captured = arguments[0];
        result->value = values->new_bool(context, 1);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member_id == MEMBER_ASYNC_ADD && argument_count == 2 &&
        values->get_i64(context, arguments[0], &left) == OASP_EXT_OK &&
        values->get_i64(context, arguments[1], &right) == OASP_EXT_OK) {
        acme_future *future = calloc(1, sizeof(*future));
        if (!future)
            return OASP_EXT_ERROR;
        future->value = (left + right) * math->scale;
        result->future = async->future_create(context, &future_vtable, future);
        if (!result->future)
            free(future);
        return result->future ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member_id == MEMBER_SEQUENCE && argument_count == 1 &&
        values->get_i64(context, arguments[0], &right) == OASP_EXT_OK) {
        acme_stream *stream = calloc(1, sizeof(*stream));
        if (!stream)
            return OASP_EXT_ERROR;
        stream->next = 1;
        stream->end = right;
        result->stream = async->stream_create(context, &stream_vtable, stream);
        if (!result->stream)
            free(stream);
        return result->stream ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member_id == MEMBER_BYTES && argument_count == 1) {
        oasp_ext_bytes bytes;
        if (values->get_data(context, arguments[0], &bytes) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        result->value = values->new_bytes(context, bytes.data, bytes.length);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member_id == MEMBER_MATRIX && !argument_count) {
        const int64_t bounds[] = {1, 1};
        int64_t indices[] = {0, 0};
        result->value = values->new_array(context, bounds, 2);
        if (!result->value)
            return OASP_EXT_ERROR;
        for (int64_t row = 0; row < 2; row++)
            for (int64_t column = 0; column < 2; column++) {
                indices[0] = row;
                indices[1] = column;
                oasp_ext_value *item = values->new_i64(context, row * 2 + column + 1);
                if (!item || values->array_set(context, result->value, indices, 2, item) != OASP_EXT_OK)
                    return OASP_EXT_ERROR;
                values->release(context, item);
            }
        return OASP_EXT_OK;
    }
    if (member_id == MEMBER_MAP && !argument_count) {
        result->value = values->new_map(context);
        oasp_ext_value *answer = values->new_i64(context, 42);
        int status = result->value && answer
                         ? values->map_set(context, result->value, "answer", 6, answer) : OASP_EXT_ERROR;
        if (answer)
            values->release(context, answer);
        return status;
    }
    if (member_id == MEMBER_DEADLINE && !argument_count) {
        result->value = values->new_i64(context, async->deadline_milliseconds(context));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member_id == MEMBER_BLOCK && !argument_count) {
        for (;;)
            ;
    }
    if (member_id == MEMBER_CRASH && !argument_count)
        raise(SIGKILL);
    error->code = 5;
    error->category = OASP_EXT_ERROR_ARGUMENT;
    error->message = "Add requires two integers";
    error->message_length = 25;
    error->source = "Acme.Math";
    error->source_length = 9;
    return OASP_EXT_ERROR;
}

static int invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    acme_math *math = instance;
    if (invocation->operation == OASP_EXT_OP_GET)
        return get(context, math, invocation->member_id, result, error);
    if (invocation->operation == OASP_EXT_OP_SET)
        return set(context, math, invocation->member_id, invocation->value, error);
    if (invocation->operation == OASP_EXT_OP_CALL)
        return call(context, math, invocation->member_id, invocation->arguments, invocation->argument_count,
                    result, error);
    if (invocation->operation == OASP_EXT_OP_DEFAULT_GET && invocation->argument_count == 1) {
        int64_t value;
        if (values->get_i64(context, invocation->arguments[0], &value) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        result->value = values->new_i64(context, value * math->scale);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation == OASP_EXT_OP_ENUMERATE) {
        const int64_t bounds[] = {0};
        const int64_t index[] = {0};
        result->value = values->new_array(context, bounds, 1);
        oasp_ext_value *scale = values->new_i64(context, math->scale);
        int status = result->value && scale
                         ? values->array_set(context, result->value, index, 1, scale) : OASP_EXT_ERROR;
        if (scale)
            values->release(context, scale);
        return status;
    }
    return OASP_EXT_ERROR;
}

static int snapshot(oasp_ext_call_context *context, void *instance, oasp_ext_value **state, oasp_ext_error *error) {
    (void)error;
    *state = values->new_i64(context, ((acme_math *)instance)->scale);
    return *state ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int restore(oasp_ext_call_context *context, void *worker_state, void *request_state, const oasp_ext_value *state, void **instance,
                   oasp_ext_error *error) {
    if (create(context, worker_state, request_state, instance, error) != OASP_EXT_OK)
        return OASP_EXT_ERROR;
    if (values->get_i64(context, state, &((acme_math *)*instance)->scale) != OASP_EXT_OK) {
        destroy(context, *instance);
        *instance = NULL;
        return OASP_EXT_ERROR;
    }
    return OASP_EXT_OK;
}

static const oasp_extension_member members[] = {
    {sizeof(oasp_extension_member), "Version", MEMBER_VERSION, OASP_EXT_MEMBER_GET},
    {sizeof(oasp_extension_member), "Scale", MEMBER_SCALE, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET},
    {sizeof(oasp_extension_member), "Add", MEMBER_ADD, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Capture", MEMBER_CAPTURE, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "CapturedCount", MEMBER_CAPTURED_COUNT, OASP_EXT_MEMBER_GET},
    {sizeof(oasp_extension_member), "AsyncAdd", MEMBER_ASYNC_ADD, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Sequence", MEMBER_SEQUENCE, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Bytes", MEMBER_BYTES, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Matrix", MEMBER_MATRIX, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Map", MEMBER_MAP, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Deadline", MEMBER_DEADLINE, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Block", MEMBER_BLOCK, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Crash", MEMBER_CRASH, OASP_EXT_MEMBER_CALL}
};

static const oasp_extension_class classes[] = {{
    sizeof(oasp_extension_class), "Acme.Math", "Acme.Math", 1,
    OASP_EXT_CLASS_ENUMERABLE | OASP_EXT_CLASS_PERSISTABLE | OASP_EXT_CLASS_ASYNC | OASP_EXT_CLASS_STREAMING,
    members, sizeof(members) / sizeof(members[0]), create, destroy, invoke, snapshot, restore
}};

static const oasp_extension_descriptor descriptor = {
    sizeof(oasp_extension_descriptor), ACME_ABI_MAJOR, OASP_EXTENSION_ABI_MINOR,
    "acme-math", "2.0.0", ACME_REQUIRED_CAPABILITIES, module_init, NULL, NULL, NULL, NULL, NULL,
    classes, sizeof(classes) / sizeof(classes[0])
};

OASP_EXTENSION_EXPORT const oasp_extension_descriptor *openasp_extension(void) {
    return &descriptor;
}
