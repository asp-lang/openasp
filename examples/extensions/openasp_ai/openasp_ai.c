// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include <openasp/ext_api.h>

#include <stdlib.h>
#include <string.h>

enum {
    CLASS_AI = 1,
    CLASS_TENSOR = 2
};

enum {
    AI_ZEROS = 1,
    AI_TENSOR,
    AI_ADD,
    AI_MATMUL
};

enum {
    TENSOR_RANK = 1,
    TENSOR_SIZE,
    TENSOR_SHAPE,
    TENSOR_ADD,
    TENSOR_MATMUL,
    TENSOR_RESHAPE,
    TENSOR_TO_ARRAY,
    TENSOR_ITEM
};

typedef struct {
    size_t rank;
    size_t dimensions[8];
    size_t count;
    size_t bytes;
    double *data;
} ai_tensor;

static const oasp_ext_value_api *values;
static const oasp_ext_object_api *objects;
static const oasp_ext_runtime_api *runtime_api;

static int tensor_shape(oasp_ext_call_context *context, const ai_tensor *tensor, oasp_ext_value **result);
static int tensor_to_array(oasp_ext_call_context *context, const ai_tensor *tensor, oasp_ext_value **result);

static int fail(oasp_ext_error *error, int code, oasp_ext_error_category category, const char *message) {
    if (error)
        *error = (oasp_ext_error){
            .struct_size = sizeof(*error),
            .code = code,
            .category = category,
            .message = message,
            .message_length = strlen(message),
            .source = "OpenASP.AI",
            .source_length = 10
        };
    return OASP_EXT_ERROR;
}

static int account_tensor(oasp_ext_call_context *context, ptrdiff_t delta, oasp_ext_error *error) {
    return runtime_api->account_resource(
        context, OASP_EXT_RESOURCE_MODEL, "reference-cpu", 13, delta, error);
}

static ai_tensor *tensor_allocate(oasp_ext_call_context *context, const size_t *dimensions, size_t rank, oasp_ext_error *error) {
    if (!dimensions || !rank || rank > 8) {
        fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "tensor rank must be between 1 and 8");
        return NULL;
    }
    size_t count = 1;
    for (size_t i = 0; i < rank; i++) {
        if (!dimensions[i] || count > SIZE_MAX / dimensions[i]) {
            fail(error, 6, OASP_EXT_ERROR_ARGUMENT, "tensor shape is empty or too large");
            return NULL;
        }
        count *= dimensions[i];
    }
    if (count > SIZE_MAX / sizeof(double) || count * sizeof(double) > PTRDIFF_MAX) {
        fail(error, 7, OASP_EXT_ERROR_RESOURCE, "tensor storage is too large");
        return NULL;
    }
    size_t bytes = count * sizeof(double);
    if (account_tensor(context, (ptrdiff_t)bytes, error) != OASP_EXT_OK)
        return NULL;
    ai_tensor *tensor = calloc(1, sizeof(*tensor));
    if (tensor)
        tensor->data = calloc(count, sizeof(*tensor->data));
    if (!tensor || !tensor->data) {
        free(tensor);
        (void)account_tensor(context, -(ptrdiff_t)bytes, NULL);
        fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate tensor storage");
        return NULL;
    }
    tensor->rank = rank;
    memcpy(tensor->dimensions, dimensions, rank * sizeof(*dimensions));
    tensor->count = count;
    tensor->bytes = bytes;
    return tensor;
}

static void tensor_release(oasp_ext_call_context *context, ai_tensor *tensor) {
    if (!tensor)
        return;
    free(tensor->data);
    if (tensor->bytes)
        (void)account_tensor(context, -(ptrdiff_t)tensor->bytes, NULL);
    free(tensor);
}

static int tensor_result(oasp_ext_call_context *context, ai_tensor *tensor, oasp_ext_result *result, oasp_ext_error *error) {
    if (!tensor)
        return OASP_EXT_ERROR;
    if (objects->new_owned(context, CLASS_TENSOR, tensor, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    tensor_release(context, tensor);
    return OASP_EXT_ERROR;
}

static int read_shape(oasp_ext_call_context *context, const oasp_ext_value *value, size_t *dimensions, size_t *rank, oasp_ext_error *error) {
    if (values->kind(context, value) != OASP_EXT_VALUE_ARRAY ||
        values->array_rank(context, value) != 1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "shape must be a one-dimensional ASP array");
    int64_t bound;
    if (values->array_bounds(context, value, &bound, 1) != OASP_EXT_OK ||
        bound < 0 || bound >= 8)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "shape rank must be between 1 and 8");
    *rank = (size_t)bound + 1;
    for (size_t i = 0; i < *rank; i++) {
        int64_t index = (int64_t)i;
        int64_t dimension;
        oasp_ext_value *item = NULL;
        int status = values->array_get(context, value, &index, 1, &item);
        if (status == OASP_EXT_OK)
            status = values->get_i64(context, item, &dimension);
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK || dimension <= 0 || (uint64_t)dimension > SIZE_MAX)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "shape dimensions must be positive integers");
        dimensions[i] = (size_t)dimension;
    }
    return OASP_EXT_OK;
}

static int tensor_from_array(oasp_ext_call_context *context, const oasp_ext_value *value, ai_tensor **result, oasp_ext_error *error) {
    if (values->kind(context, value) != OASP_EXT_VALUE_ARRAY ||
        values->array_rank(context, value) != 1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tensor requires a one-dimensional numeric ASP array");
    int64_t bound;
    if (values->array_bounds(context, value, &bound, 1) != OASP_EXT_OK || bound < 0)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tensor input array must not be empty");
    size_t dimensions[] = {(size_t)bound + 1};
    ai_tensor *tensor = tensor_allocate(context, dimensions, 1, error);
    if (!tensor)
        return OASP_EXT_ERROR;
    for (size_t i = 0; i < tensor->count; i++) {
        int64_t index = (int64_t)i;
        oasp_ext_value *item = NULL;
        int status = values->array_get(context, value, &index, 1, &item);
        if (status == OASP_EXT_OK)
            status = values->get_f64(context, item, &tensor->data[i]);
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK) {
            tensor_release(context, tensor);
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tensor input must contain only numeric values");
        }
    }
    *result = tensor;
    return OASP_EXT_OK;
}

static int get_tensor(oasp_ext_call_context *context, const oasp_ext_value *value, ai_tensor **tensor, oasp_ext_error *error) {
    void *native = NULL;
    if (objects->get_native(context, value, CLASS_TENSOR, &native, error) != OASP_EXT_OK)
        return OASP_EXT_ERROR;
    *tensor = native;
    return OASP_EXT_OK;
}

static ai_tensor *tensor_binary(oasp_ext_call_context *context, const ai_tensor *left, const ai_tensor *right, int multiply, oasp_ext_error *error) {
    if (!multiply) {
        if (left->rank != right->rank || left->count != right->count ||
            memcmp(left->dimensions, right->dimensions, left->rank * sizeof(*left->dimensions))) {
            fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Add requires tensors with identical shapes");
            return NULL;
        }
        ai_tensor *output = tensor_allocate(context, left->dimensions, left->rank, error);
        if (output)
            for (size_t i = 0; i < output->count; i++)
                output->data[i] = left->data[i] + right->data[i];
        return output;
    }
    if (left->rank != 2 || right->rank != 2 || left->dimensions[1] != right->dimensions[0]) {
        fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "MatMul requires compatible rank-2 tensors");
        return NULL;
    }
    size_t shape[] = {left->dimensions[0], right->dimensions[1]};
    ai_tensor *output = tensor_allocate(context, shape, 2, error);
    if (!output)
        return NULL;
    for (size_t row = 0; row < shape[0]; row++)
        for (size_t column = 0; column < shape[1]; column++)
            for (size_t inner = 0; inner < left->dimensions[1]; inner++)
                output->data[row * shape[1] + column] +=
                    left->data[row * left->dimensions[1] + inner] *
                    right->data[inner * right->dimensions[1] + column];
    return output;
}

static int factory_create(oasp_ext_call_context *context, void *worker_state, void *request_state, void **instance, oasp_ext_error *error) {
    (void)context;
    (void)worker_state;
    (void)request_state;
    (void)error;
    *instance = calloc(1, 1);
    return *instance ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static void factory_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    free(instance);
}

static void tensor_destroy(oasp_ext_call_context *context, void *instance) {
    tensor_release(context, instance);
}

static int tensor_create(oasp_ext_call_context *context, void *worker_state, void *request_state, void **instance, oasp_ext_error *error) {
    const size_t shape[] = {1};
    (void)worker_state;
    (void)request_state;
    *instance = tensor_allocate(context, shape, 1, error);
    return *instance ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int tensor_snapshot(oasp_ext_call_context *context, void *instance, oasp_ext_value **state, oasp_ext_error *error) {
    ai_tensor *tensor = instance;
    oasp_ext_value *shape = NULL;
    oasp_ext_value *data = NULL;
    *state = values->new_map(context);
    int status = *state ? tensor_shape(context, tensor, &shape) : OASP_EXT_ERROR;
    if (status == OASP_EXT_OK)
        status = tensor_to_array(context, tensor, &data);
    if (status == OASP_EXT_OK)
        status = values->map_set(context, *state, "shape", 5, shape);
    if (status == OASP_EXT_OK)
        status = values->map_set(context, *state, "data", 4, data);
    if (shape)
        values->release(context, shape);
    if (data)
        values->release(context, data);
    if (status != OASP_EXT_OK)
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to snapshot Tensor");
    return OASP_EXT_OK;
}

static int tensor_restore(oasp_ext_call_context *context, void *worker_state, void *request_state, const oasp_ext_value *state, void **instance,
                          oasp_ext_error *error) {
    oasp_ext_value *shape_value = NULL;
    oasp_ext_value *data_value = NULL;
    size_t shape[8];
    size_t rank;
    (void)worker_state;
    (void)request_state;
    int status = values->map_get(context, state, "shape", 5, &shape_value);
    if (status == OASP_EXT_OK)
        status = values->map_get(context, state, "data", 4, &data_value);
    if (status == OASP_EXT_OK)
        status = read_shape(context, shape_value, shape, &rank, error);
    ai_tensor *tensor = status == OASP_EXT_OK
                            ? tensor_allocate(context, shape, rank, error) : NULL;
    if (tensor) {
        int64_t bound;
        if (values->array_rank(context, data_value) != 1 ||
            values->array_bounds(context, data_value, &bound, 1) != OASP_EXT_OK ||
            bound < 0 || (size_t)bound + 1 != tensor->count)
            status = OASP_EXT_ERROR;
        for (size_t i = 0; status == OASP_EXT_OK && i < tensor->count; i++) {
            int64_t index = (int64_t)i;
            oasp_ext_value *item = NULL;
            status = values->array_get(context, data_value, &index, 1, &item);
            if (status == OASP_EXT_OK)
                status = values->get_f64(context, item, &tensor->data[i]);
            if (item)
                values->release(context, item);
        }
    }
    if (shape_value)
        values->release(context, shape_value);
    if (data_value)
        values->release(context, data_value);
    if (status != OASP_EXT_OK || !tensor) {
        tensor_release(context, tensor);
        return fail(error, 13, OASP_EXT_ERROR_ARGUMENT, "invalid persisted Tensor state");
    }
    *instance = tensor;
    return OASP_EXT_OK;
}

static int factory_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    (void)instance;
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "OpenASP.AI member is not callable");
    if (invocation->member_id == AI_ZEROS && invocation->argument_count >= 1) {
        size_t shape[8];
        size_t rank;
        if (read_shape(context, invocation->arguments[0], shape, &rank, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        return tensor_result(context, tensor_allocate(context, shape, rank, error), result, error);
    }
    if (invocation->member_id == AI_TENSOR && invocation->argument_count == 1) {
        ai_tensor *tensor = NULL;
        if (tensor_from_array(context, invocation->arguments[0], &tensor, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        return tensor_result(context, tensor, result, error);
    }
    if ((invocation->member_id == AI_ADD || invocation->member_id == AI_MATMUL) &&
        invocation->argument_count == 2) {
        ai_tensor *left;
        ai_tensor *right;
        if (get_tensor(context, invocation->arguments[0], &left, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &right, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        return tensor_result(
            context, tensor_binary(context, left, right, invocation->member_id == AI_MATMUL, error),
            result, error);
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid OpenASP.AI factory arguments");
}

static int tensor_shape(oasp_ext_call_context *context, const ai_tensor *tensor, oasp_ext_value **result) {
    int64_t bound[] = {(int64_t)tensor->rank - 1};
    *result = values->new_array(context, bound, 1);
    if (!*result)
        return OASP_EXT_ERROR;
    for (size_t i = 0; i < tensor->rank; i++) {
        int64_t index = (int64_t)i;
        oasp_ext_value *item = values->new_i64(context, (int64_t)tensor->dimensions[i]);
        int status = item ? values->array_set(context, *result, &index, 1, item) : OASP_EXT_ERROR;
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK)
            return OASP_EXT_ERROR;
    }
    return OASP_EXT_OK;
}

static int tensor_to_array(oasp_ext_call_context *context, const ai_tensor *tensor, oasp_ext_value **result) {
    int64_t bound[] = {(int64_t)tensor->count - 1};
    *result = values->new_array(context, bound, 1);
    if (!*result)
        return OASP_EXT_ERROR;
    for (size_t i = 0; i < tensor->count; i++) {
        int64_t index = (int64_t)i;
        oasp_ext_value *item = values->new_f64(context, tensor->data[i]);
        int status = item ? values->array_set(context, *result, &index, 1, item) : OASP_EXT_ERROR;
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK)
            return OASP_EXT_ERROR;
    }
    return OASP_EXT_OK;
}

static int tensor_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_tensor *tensor = instance;
    if (invocation->operation == OASP_EXT_OP_GET) {
        if (invocation->member_id == TENSOR_RANK)
            result->value = values->new_i64(context, (int64_t)tensor->rank);
        else if (invocation->member_id == TENSOR_SIZE)
            result->value = values->new_i64(context, (int64_t)tensor->count);
        else if (invocation->member_id == TENSOR_SHAPE)
            return tensor_shape(context, tensor, &result->value);
        else
            return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown Tensor property");
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Tensor member is not callable");
    if ((invocation->member_id == TENSOR_ADD || invocation->member_id == TENSOR_MATMUL) &&
        invocation->argument_count == 1) {
        ai_tensor *right;
        if (get_tensor(context, invocation->arguments[0], &right, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        return tensor_result(
            context,
            tensor_binary(context, tensor, right, invocation->member_id == TENSOR_MATMUL, error),
            result, error);
    }
    if (invocation->member_id == TENSOR_RESHAPE && invocation->argument_count == 1) {
        size_t shape[8];
        size_t rank;
        if (read_shape(context, invocation->arguments[0], shape, &rank, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        size_t count = 1;
        for (size_t i = 0; i < rank; i++)
            if (count > SIZE_MAX / shape[i])
                return fail(error, 6, OASP_EXT_ERROR_ARGUMENT, "reshape dimensions overflow");
            else
                count *= shape[i];
        if (count != tensor->count)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "reshape must preserve tensor element count");
        ai_tensor *output = tensor_allocate(context, shape, rank, error);
        if (output)
            memcpy(output->data, tensor->data, tensor->bytes);
        return tensor_result(context, output, result, error);
    }
    if (invocation->member_id == TENSOR_TO_ARRAY && !invocation->argument_count)
        return tensor_to_array(context, tensor, &result->value);
    if (invocation->member_id == TENSOR_ITEM &&
        invocation->argument_count >= 1 && invocation->argument_count <= tensor->rank) {
        size_t offset = 0;
        for (size_t i = 0; i < invocation->argument_count; i++) {
            int64_t index;
            if (values->get_i64(context, invocation->arguments[i], &index) != OASP_EXT_OK ||
                index < 0 || (size_t)index >= tensor->dimensions[i])
                return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "Tensor index is out of range");
            offset = offset * tensor->dimensions[i] + (size_t)index;
        }
        if (invocation->argument_count != tensor->rank)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Item requires one index per Tensor dimension");
        result->value = values->new_f64(context, tensor->data[offset]);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Tensor method arguments");
}

static int module_init(const oasp_ext_host *host, uint64_t capabilities, void **state, oasp_ext_error *error) {
    uint32_t version;
    (void)capabilities;
    (void)state;
    (void)error;
    values = host->query_api(OASP_EXT_API_VALUE, OASP_EXT_API_VALUE_VERSION, &version);
    objects = host->query_api(OASP_EXT_API_OBJECT, OASP_EXT_API_OBJECT_VERSION, &version);
    runtime_api = host->query_api(OASP_EXT_API_RUNTIME, OASP_EXT_API_RUNTIME_VERSION, &version);
    return values && objects && runtime_api ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static const oasp_extension_member factory_members[] = {
    {sizeof(oasp_extension_member), "Zeros", AI_ZEROS, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Tensor", AI_TENSOR, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Add", AI_ADD, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "MatMul", AI_MATMUL, OASP_EXT_MEMBER_CALL}
};

static const oasp_extension_member tensor_members[] = {
    {sizeof(oasp_extension_member), "Rank", TENSOR_RANK, OASP_EXT_MEMBER_GET},
    {sizeof(oasp_extension_member), "Size", TENSOR_SIZE, OASP_EXT_MEMBER_GET},
    {sizeof(oasp_extension_member), "Shape", TENSOR_SHAPE, OASP_EXT_MEMBER_GET},
    {sizeof(oasp_extension_member), "Add", TENSOR_ADD, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "MatMul", TENSOR_MATMUL, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Reshape", TENSOR_RESHAPE, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "ToArray", TENSOR_TO_ARRAY, OASP_EXT_MEMBER_CALL},
    {sizeof(oasp_extension_member), "Item", TENSOR_ITEM, OASP_EXT_MEMBER_CALL}
};

static const oasp_extension_class classes[] = {
    {
        sizeof(oasp_extension_class), "OpenASP.AI", "OpenASP.AI", CLASS_AI, 0,
        factory_members, sizeof(factory_members) / sizeof(factory_members[0]),
        factory_create, factory_destroy, factory_invoke, NULL, NULL
    },
    {
        sizeof(oasp_extension_class), "OpenASP.AI.Tensor", "OpenASP.AI.Tensor",
        CLASS_TENSOR, OASP_EXT_CLASS_INTERNAL | OASP_EXT_CLASS_PERSISTABLE,
        tensor_members, sizeof(tensor_members) / sizeof(tensor_members[0]),
        tensor_create, tensor_destroy, tensor_invoke, tensor_snapshot, tensor_restore
    }
};

static const oasp_extension_descriptor descriptor = {
    sizeof(oasp_extension_descriptor), OASP_EXTENSION_ABI_MAJOR, OASP_EXTENSION_ABI_MINOR,
    "openasp-ai-reference", "0.1.0", 0, module_init, NULL, NULL, NULL, NULL, NULL,
    classes, sizeof(classes) / sizeof(classes[0])
};

OASP_EXTENSION_EXPORT const oasp_extension_descriptor *openasp_extension(void) {
    return &descriptor;
}
