// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"
#include "component_metadata.h"

#include <openasp/ext_api.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    CLASS_AI = 1,
    CLASS_TENSOR,
    CLASS_MODULE,
    CLASS_OPTIMIZER,
    CLASS_GRAPH,
    CLASS_MODEL,
    CLASS_KV_CACHE,
    CLASS_TRACE,
    CLASS_DATASET,
    CLASS_DATALOADER,
    CLASS_DATA_ITERATOR,
    CLASS_BATCH,
    CLASS_TOKENIZER,
    CLASS_SCHEDULER,
    CLASS_LOSS_SCALER,
    CLASS_STATE_DICT,
    CLASS_EVENT,
    CLASS_PARAMETER_COLLECTION,
    CLASS_CHECKPOINT_STATE,
    CLASS_DEVICE
};

enum {
    AI_DEVICE = 1,
    AI_DTYPE,
    AI_SEED,
    AI_DETERMINISTIC,
    AI_STRICT_DEVICE,
    AI_ZEROS,
    AI_ONES,
    AI_FULL,
    AI_RANDOM_UNIFORM,
    AI_RANDOM_NORMAL,
    AI_TENSOR,
    AI_ADD,
    AI_SUB,
    AI_MUL,
    AI_DIV,
    AI_MATMUL,
    AI_RELU,
    AI_SIGMOID,
    AI_TANH,
    AI_GELU,
    AI_SILU,
    AI_SOFTMAX,
    AI_LAYER_NORM,
    AI_RMS_NORM,
    AI_CROSS_ENTROPY,
    AI_MSE_LOSS,
    AI_L1_LOSS,
    AI_BCE_LOSS,
    AI_BCE_WITH_LOGITS_LOSS,
    AI_NLL_LOSS,
    AI_KL_DIV_LOSS,
    AI_HUBER_LOSS,
    AI_ATTENTION,
    AI_MODULE,
    AI_SGD,
    AI_ADAM,
    AI_ADAMW,
    AI_GRAPH,
    AI_KV_CACHE,
    AI_SAMPLE,
    AI_MEMORY_STATS,
    AI_BACKEND_INFO,
    AI_SYNCHRONIZE,
    AI_LOAD_MODEL,
    AI_SAVE_CHECKPOINT,
    AI_LOAD_CHECKPOINT,
    AI_SEQUENTIAL,
    AI_LINEAR,
    AI_RELU_MODULE,
    AI_DROPOUT,
    AI_LAYER_NORM_MODULE,
    AI_RMS_NORM_MODULE,
    AI_EMBEDDING,
    AI_BEGIN_TRACE,
    AI_TENSOR_DATASET,
    AI_MNIST,
    AI_TOKENIZER,
    AI_QUANTIZE,
    AI_DEQUANTIZE,
    AI_STEP_SCHEDULER,
    AI_COSINE_SCHEDULER,
    AI_LINEAR_SCHEDULER,
    AI_LOSS_SCALER,
    AI_LOAD_ONNX,
    AI_PROFILER_START,
    AI_PROFILER_STOP,
    AI_PROFILER_EXPORT,
    AI_DEVICES,
    AI_GET_DEVICE,
    AI_SAVE_BUNDLE,
    AI_SUM,
    AI_MEAN,
    AI_SMOOTH_L1_LOSS,
    AI_FOCAL_LOSS,
    AI_DICE_LOSS,
    AI_INFO_NCE_LOSS,
    AI_CTC_LOSS
};

enum {
    TENSOR_SHAPE = 1,
    TENSOR_STRIDES,
    TENSOR_RANK,
    TENSOR_DTYPE,
    TENSOR_DEVICE,
    TENSOR_BYTES,
    TENSOR_REQUIRES_GRAD,
    TENSOR_GRAD,
    TENSOR_CONTIGUOUS_PROPERTY,
    TENSOR_RESHAPE,
    TENSOR_TRANSPOSE,
    TENSOR_SLICE,
    TENSOR_CONTIGUOUS,
    TENSOR_CLONE,
    TENSOR_DETACH,
    TENSOR_TO_DTYPE,
    TENSOR_TO_ARRAY,
    TENSOR_TO_BYTES,
    TENSOR_ITEM,
    TENSOR_BACKWARD,
    TENSOR_ZERO_GRAD,
    TENSOR_ADD,
    TENSOR_SUB,
    TENSOR_MUL,
    TENSOR_DIV,
    TENSOR_MATMUL,
    TENSOR_SIZE,
    TENSOR_TO_DEVICE
};

enum {
    MODULE_REGISTER_MODULE = 1,
    MODULE_REGISTER_PARAMETER,
    MODULE_REGISTER_BUFFER,
    MODULE_PARAMETER_COUNT,
    MODULE_PARAMETER,
    MODULE_TRAIN,
    MODULE_EVAL,
    MODULE_SAVE_WEIGHTS,
    MODULE_LOAD_WEIGHTS,
    MODULE_ADD,
    MODULE_FORWARD,
    MODULE_STATE_DICT,
    MODULE_LOAD_STATE_DICT,
    MODULE_TO_DEVICE,
    MODULE_TO_DTYPE,
    MODULE_SAVE_SAFETENSORS,
    MODULE_LOAD_SAFETENSORS,
    MODULE_PARAMETERS,
    MODULE_NAMED_PARAMETERS,
    MODULE_BUFFERS
};

enum {
    OPTIMIZER_ZERO_GRAD = 1,
    OPTIMIZER_STEP,
    OPTIMIZER_CLIP_GRAD_NORM
};

enum {
    GRAPH_INPUT = 1,
    GRAPH_OPERATION,
    GRAPH_BUILD,
    GRAPH_SAVE
};

enum {
    MODEL_FORWARD = 1,
    MODEL_SAVE,
    MODEL_FORWARD_ASYNC,
    MODEL_EXPORT_ONNX
};

enum {
    CACHE_LENGTH = 1,
    CACHE_APPEND
};

enum {
    DATASET_SIZE = 1,
    DATASET_DATALOADER
};

enum {
    DATALOADER_ITERATOR = 1
};

enum {
    ITERATOR_MOVE_NEXT = 1,
    ITERATOR_CURRENT,
    ITERATOR_CURSOR
};

enum {
    BATCH_INPUTS = 1,
    BATCH_LABELS,
    BATCH_SIZE
};

enum {
    TOKENIZER_ENCODE = 1,
    TOKENIZER_DECODE
};

enum {
    SCHEDULER_STEP = 1,
    SCHEDULER_CURRENT_STEP
};

enum {
    LOSS_SCALER_SCALE = 1,
    LOSS_SCALER_SCALE_LOSS,
    LOSS_SCALER_STEP
};

enum {
    STATE_DICT_COUNT = 1,
    STATE_DICT_NAME,
    STATE_DICT_KIND,
    STATE_DICT_TENSOR
};

enum {
    EVENT_READY = 1,
    EVENT_WAIT,
    EVENT_OUTPUT_COUNT,
    EVENT_OUTPUT
};

enum {
    COLLECTION_COUNT = 1,
    COLLECTION_NAME,
    COLLECTION_TENSOR,
    COLLECTION_KIND
};

enum {
    CHECKPOINT_METADATA = 1,
    CHECKPOINT_EPOCH,
    CHECKPOINT_GLOBAL_STEP
};

enum {
    DEVICE_NAME = 1,
    DEVICE_BACKEND,
    DEVICE_ORDINAL,
    DEVICE_TOTAL_MEMORY,
    DEVICE_FREE_MEMORY,
    DEVICE_SUPPORTS,
    DEVICE_SUPPORTS_OPERATOR
};

typedef struct {
    atomic_uint references;
    oasp_ai_context *core;
} ai_request;

typedef struct {
    ai_request *request;
    oasp_ai_dtype dtype;
    char device[64];
} ai_factory;

typedef struct {
    ai_request *request;
    oasp_ai_tensor *tensor;
    oasp_ai_graph *graph;
    uint32_t value_id;
    uint64_t accounted_bytes;
} ai_tensor_object;

typedef struct {
    ai_request *request;
    oasp_ai_module *module;
} ai_module_object;

typedef struct {
    ai_request *request;
    oasp_ai_optimizer *optimizer;
} ai_optimizer_object;

typedef struct {
    ai_request *request;
    oasp_ai_graph *graph;
    char device[64];
} ai_graph_object;

typedef struct {
    ai_request *request;
    oasp_ai_graph *graph;
    oasp_ai_plan *plan;
} ai_model_object;

typedef struct {
    ai_request *request;
    oasp_ai_kv_cache *cache;
} ai_cache_object;

typedef struct {
    ai_request *request;
    oasp_ai_dataset *dataset;
} ai_dataset_object;

typedef struct {
    ai_request *request;
    oasp_ai_dataloader *loader;
} ai_dataloader_object;

typedef struct {
    ai_request *request;
    oasp_ai_data_iterator *iterator;
    oasp_ai_batch *current;
} ai_data_iterator_object;

typedef struct {
    ai_request *request;
    oasp_ai_batch *batch;
} ai_batch_object;

typedef struct {
    ai_request *request;
    oasp_ai_tokenizer *tokenizer;
} ai_tokenizer_object;

typedef struct {
    ai_request *request;
    oasp_ai_scheduler *scheduler;
} ai_scheduler_object;

typedef struct {
    ai_request *request;
    oasp_ai_loss_scaler *scaler;
} ai_loss_scaler_object;

typedef struct {
    ai_request *request;
    oasp_ai_state_dict *state;
} ai_state_dict_object;

typedef struct {
    ai_request *request;
    oasp_ai_event_handle *event;
} ai_event_object;

typedef struct {
    ai_request *request;
    oasp_ai_state_dict *state;
    oasp_ai_state_kind kind;
} ai_parameter_collection_object;

typedef struct {
    ai_request *request;
    oasp_ai_checkpoint_state state;
} ai_checkpoint_state_object;

typedef struct {
    ai_request *request;
    size_t backend_index;
    oasp_ai_device_info info;
    char name[128];
    char backend[64];
} ai_device_object;

static const oasp_ext_value_api *values;
static const oasp_ext_object_api *objects;
static const oasp_ext_runtime_api *runtime_api;

static void request_retain(ai_request *request) {
    atomic_fetch_add_explicit(&request->references, 1, memory_order_relaxed);
}

static void request_release(ai_request *request) {
    if (!request || atomic_fetch_sub_explicit(&request->references, 1, memory_order_acq_rel) != 1)
        return;
    oasp_ai_context_destroy(request->core);
    free(request);
}

static int fail(oasp_ext_error *error, int code, oasp_ext_error_category category, const char *message) {
    if (error)
        *error = (oasp_ext_error){
            .struct_size = sizeof(*error),
            .code = code,
            .category = category,
            .message = message,
            .message_length = strlen(message),
            .source = "OpenASP.AI",
            .source_length = 10,
        };
    return OASP_EXT_ERROR;
}

static int core_fail(oasp_ext_error *target, const oasp_ai_error *source) {
    if (!source || !source->message)
        return fail(target, 5, OASP_EXT_ERROR_RUNTIME, "OpenASP.AI operation failed");
    if (target)
        *target = (oasp_ext_error){
            .struct_size = sizeof(*target),
            .code = source->code,
            .category = source->code == 7 ? OASP_EXT_ERROR_RESOURCE : OASP_EXT_ERROR_ARGUMENT,
            .message = source->message,
            .message_length = strlen(source->message),
            .source = source->operation ? source->operation : "OpenASP.AI",
            .source_length = strlen(source->operation ? source->operation : "OpenASP.AI"),
            .detail = source->suggestion,
            .detail_length = source->suggestion ? strlen(source->suggestion) : 0,
        };
    return OASP_EXT_ERROR;
}

static int get_string(oasp_ext_call_context *context, const oasp_ext_value *value, char *output, size_t capacity) {
    oasp_ext_bytes data;
    if (values->kind(context, value) != OASP_EXT_VALUE_STRING || values->get_data(context, value, &data) != OASP_EXT_OK || data.length >= capacity)
        return -1;
    memcpy(output, data.data, data.length);
    output[data.length] = '\0';
    return 0;
}

static int get_reduction(oasp_ext_call_context *context, const oasp_ext_value *value,
                         int allow_batch_mean, oasp_ai_reduction *reduction) {
    char name[16];
    if (get_string(context, value, name, sizeof(name)))
        return -1;
    if (!strcasecmp(name, "none"))
        *reduction = OASP_AI_REDUCTION_NONE;
    else if (!strcasecmp(name, "mean"))
        *reduction = OASP_AI_REDUCTION_MEAN;
    else if (!strcasecmp(name, "sum"))
        *reduction = OASP_AI_REDUCTION_SUM;
    else if (allow_batch_mean && !strcasecmp(name, "batchmean"))
        *reduction = OASP_AI_REDUCTION_BATCH_MEAN;
    else
        return -1;
    return 0;
}

static int is_missing_value(oasp_ext_call_context *context, const oasp_ext_value *value) {
    oasp_ext_value_kind kind = values->kind(context, value);
    return kind == OASP_EXT_VALUE_EMPTY || kind == OASP_EXT_VALUE_NULL;
}

static int read_shape_mode(oasp_ext_call_context *context, const oasp_ext_value *value, int64_t shape[OASP_AI_MAX_RANK], uint32_t *rank, int allow_dynamic,
                           oasp_ext_error *error) {
    int64_t bound;
    if (values->kind(context, value) != OASP_EXT_VALUE_ARRAY || values->array_rank(context, value) != 1 ||
        values->array_bounds(context, value, &bound, 1) != OASP_EXT_OK || bound < 0 || bound >= OASP_AI_MAX_RANK)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "shape must be a one-dimensional array with rank 1 through 8");
    *rank = (uint32_t)bound + 1;
    for (uint32_t axis = 0; axis < *rank; axis++) {
        int64_t index = axis;
        oasp_ext_value *item = NULL;
        int status = values->array_get(context, value, &index, 1, &item);
        if (status == OASP_EXT_OK)
            status = values->get_i64(context, item, &shape[axis]);
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK || (shape[axis] <= 0 && !(allow_dynamic && shape[axis] == -1)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        allow_dynamic ? "graph shape dimensions must be positive integers or -1" : "shape dimensions must be positive integers");
    }
    return OASP_EXT_OK;
}

static int read_shape(oasp_ext_call_context *context, const oasp_ext_value *value, int64_t shape[OASP_AI_MAX_RANK], uint32_t *rank, oasp_ext_error *error) {
    return read_shape_mode(context, value, shape, rank, 0, error);
}

static int new_tensor_object(oasp_ext_call_context *context, ai_request *request, oasp_ai_tensor *tensor, oasp_ai_graph *graph, uint32_t value_id,
                             oasp_ext_result *result, oasp_ext_error *error) {
    if (!tensor && !graph)
        return OASP_EXT_ERROR;
    ai_tensor_object *object = calloc(1, sizeof(*object));
    if (!object) {
        oasp_ai_tensor_release(tensor);
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate Tensor object");
    }
    object->request = request;
    object->tensor = tensor;
    object->graph = graph;
    if (graph)
        atomic_fetch_add_explicit(&graph->references, 1, memory_order_relaxed);
    object->value_id = value_id;
    object->accounted_bytes = tensor ? oasp_ai_tensor_bytes(tensor) : 0;
    if (object->accounted_bytes > PTRDIFF_MAX ||
        (object->accounted_bytes && runtime_api->account_resource(context, OASP_EXT_RESOURCE_MODEL, "openasp-ai", 10,
                                                                  (ptrdiff_t)object->accounted_bytes, error) != OASP_EXT_OK)) {
        oasp_ai_tensor_release(tensor);
        oasp_ai_graph_release(graph);
        free(object);
        return OASP_EXT_ERROR;
    }
    request_retain(request);
    if (objects->new_owned(context, CLASS_TENSOR, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    if (object->accounted_bytes)
        (void)runtime_api->account_resource(context, OASP_EXT_RESOURCE_MODEL, "openasp-ai", 10, -(ptrdiff_t)object->accounted_bytes, NULL);
    oasp_ai_tensor_release(tensor);
    oasp_ai_graph_release(graph);
    request_release(request);
    free(object);
    return OASP_EXT_ERROR;
}

static int get_native(oasp_ext_call_context *context, const oasp_ext_value *value, uint32_t class_id, void **result, oasp_ext_error *error) {
    return objects->get_native(context, value, class_id, result, error);
}

static int try_get_native(oasp_ext_call_context *context, const oasp_ext_value *value, uint32_t class_id, void **result) {
    oasp_ext_error ignored = {.struct_size = sizeof(ignored)};
    *result = NULL;
    return values->kind(context, value) == OASP_EXT_VALUE_OBJECT &&
           get_native(context, value, class_id, result, &ignored) == OASP_EXT_OK;
}

static int get_checkpoint_module(oasp_ext_call_context *context, const oasp_ext_value *value, ai_module_object **result, oasp_ext_error *error) {
    if (try_get_native(context, value, CLASS_MODULE, (void **)result))
        return OASP_EXT_OK;
    if (values->kind(context, value) != OASP_EXT_VALUE_OBJECT)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "checkpoint model must be a Module or expose NativeModule");
    oasp_ext_value *native = NULL;
    oasp_ext_error object_error = {.struct_size = sizeof(object_error)};
    if (objects->get(context, value, "NativeModule", 12, &native, &object_error) != OASP_EXT_OK || !native)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "checkpoint model must be a Module or expose NativeModule");
    int status = get_native(context, native, CLASS_MODULE, (void **)result, error);
    values->release(context, native);
    return status;
}

static int get_tensor(oasp_ext_call_context *context, const oasp_ext_value *value, ai_tensor_object **result, oasp_ext_error *error) {
    return get_native(context, value, CLASS_TENSOR, (void **)result, error);
}

static int shape_value(oasp_ext_call_context *context, const int64_t *items, uint32_t count, oasp_ext_value **result) {
    int64_t bound = (int64_t)count - 1;
    *result = values->new_array(context, &bound, 1);
    for (uint32_t index = 0; *result && index < count; index++) {
        int64_t element_index = index;
        oasp_ext_value *item = values->new_i64(context, items[index]);
        int status = item ? values->array_set(context, *result, &element_index, 1, item) : OASP_EXT_ERROR;
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK)
            return OASP_EXT_ERROR;
    }
    return *result ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int read_i64_array(oasp_ext_call_context *context, const oasp_ext_value *value, int64_t **items, size_t *count, oasp_ext_error *error) {
    int64_t bound;
    if (!items || !count || values->kind(context, value) != OASP_EXT_VALUE_ARRAY || values->array_rank(context, value) != 1 ||
        values->array_bounds(context, value, &bound, 1) != OASP_EXT_OK || bound < -1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "expected a one-dimensional integer array");
    *count = (size_t)(bound + 1);
    *items = *count ? calloc(*count, sizeof(**items)) : NULL;
    if (*count && !*items)
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate integer array");
    for (size_t index = 0; index < *count; index++) {
        int64_t position = (int64_t)index;
        oasp_ext_value *item = NULL;
        int status = values->array_get(context, value, &position, 1, &item);
        if (status == OASP_EXT_OK)
            status = values->get_i64(context, item, &(*items)[index]);
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK) {
            free(*items);
            *items = NULL;
            *count = 0;
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "array must contain only integers");
        }
    }
    return OASP_EXT_OK;
}

static oasp_ai_tensor *tensor_from_array(oasp_ext_call_context *context, ai_factory *factory, const oasp_ext_value *value, oasp_ext_error *error) {
    size_t rank = values->array_rank(context, value);
    if (values->kind(context, value) != OASP_EXT_VALUE_ARRAY || !rank || rank > OASP_AI_MAX_RANK) {
        fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tensor requires a numeric ASP array");
        return NULL;
    }
    int64_t bounds[OASP_AI_MAX_RANK];
    oasp_ai_tensor_desc desc = {.dtype = factory->dtype, .rank = (uint32_t)rank};
    if (values->array_bounds(context, value, bounds, rank) != OASP_EXT_OK)
        return NULL;
    uint64_t count = 1;
    for (size_t axis = 0; axis < rank; axis++) {
        if (bounds[axis] < 0 || (uint64_t)bounds[axis] + 1 > UINT64_MAX / count)
            return NULL;
        desc.shape[axis] = bounds[axis] + 1;
        count *= (uint64_t)desc.shape[axis];
    }
    oasp_ai_error core_error = {0};
    oasp_ai_tensor *tensor = oasp_ai_tensor_create(factory->request->core, &desc, NULL, 0, &core_error);
    if (!tensor) {
        core_fail(error, &core_error);
        return NULL;
    }
    for (uint64_t flat = 0; flat < count; flat++) {
        uint64_t remaining = flat;
        int64_t indices[OASP_AI_MAX_RANK];
        for (size_t reverse = 0; reverse < rank; reverse++) {
            size_t axis = rank - reverse - 1;
            indices[axis] = (int64_t)(remaining % (uint64_t)desc.shape[axis]);
            remaining /= (uint64_t)desc.shape[axis];
        }
        oasp_ext_value *item = NULL;
        double number;
        int status = values->array_get(context, value, indices, rank, &item);
        if (status == OASP_EXT_OK)
            status = values->get_f64(context, item, &number);
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK) {
            oasp_ai_tensor_release(tensor);
            fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tensor input must contain only numeric values");
            return NULL;
        }
        oasp_ai_tensor_write(tensor, flat, number);
    }
    return tensor;
}

static int request_init(oasp_ext_call_context *context, void *worker_state, void **request_state, oasp_ext_error *error) {
    (void)context;
    (void)worker_state;
    ai_request *request = calloc(1, sizeof(*request));
    oasp_ai_context_options options = {
        .seed = 1,
        .maximum_tensor_bytes = UINT64_C(1) << 30,
        .maximum_model_bytes = UINT64_C(2) << 30,
        .maximum_array_elements = UINT64_C(1) << 20,
        .training_enabled = 1,
    };
    oasp_ai_error core_error = {0};
    if (request)
        request->core = oasp_ai_context_create(&options, &core_error);
    const char *backend_path = getenv("OPENASP_AI_BACKEND");
    const char *backend_sha256 = getenv("OPENASP_AI_BACKEND_SHA256");
    if (request && request->core && backend_path && *backend_path &&
        oasp_ai_context_load_backend(request->core, backend_path, backend_sha256, &core_error)) {
        oasp_ai_context_destroy(request->core);
        request->core = NULL;
    }
    if (!request || !request->core) {
        free(request);
        return core_fail(error, &core_error);
    }
    atomic_init(&request->references, 1);
    *request_state = request;
    return OASP_EXT_OK;
}

static void request_shutdown(oasp_ext_call_context *context, void *request_state) {
    (void)context;
    request_release(request_state);
}

static int factory_create(oasp_ext_call_context *context, void *worker_state, void *request_state, void **instance, oasp_ext_error *error) {
    (void)context;
    (void)worker_state;
    ai_factory *factory = calloc(1, sizeof(*factory));
    if (!factory)
        return OASP_EXT_ERROR;
    factory->request = request_state;
    request_retain(factory->request);
    factory->dtype = OASP_AI_DTYPE_F32;
    const char *configured_device = getenv("OPENASP_AI_DEVICE");
    if (!configured_device || !*configured_device)
        configured_device = "cpu:0";
    oasp_ai_error core_error = {0};
    if (strlen(configured_device) >= sizeof(factory->device) ||
        oasp_ai_backend_find(factory->request->core, configured_device, &core_error) == -2) {
        request_release(factory->request);
        free(factory);
        return core_fail(error, &core_error);
    }
    memcpy(factory->device, configured_device, strlen(configured_device) + 1);
    *instance = factory;
    return OASP_EXT_OK;
}

static void factory_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_factory *factory = instance;
    request_release(factory->request);
    free(factory);
}

static void tensor_destroy(oasp_ext_call_context *context, void *instance) {
    ai_tensor_object *object = instance;
    if (object->accounted_bytes)
        (void)runtime_api->account_resource(context, OASP_EXT_RESOURCE_MODEL, "openasp-ai", 10, -(ptrdiff_t)object->accounted_bytes, NULL);
    oasp_ai_tensor_release(object->tensor);
    oasp_ai_graph_release(object->graph);
    request_release(object->request);
    free(object);
}

static void module_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_module_object *object = instance;
    oasp_ai_module_release(object->module);
    request_release(object->request);
    free(object);
}

static void optimizer_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_optimizer_object *object = instance;
    oasp_ai_optimizer_release(object->optimizer);
    request_release(object->request);
    free(object);
}

static void graph_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_graph_object *object = instance;
    oasp_ai_graph_release(object->graph);
    request_release(object->request);
    free(object);
}

static void model_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_model_object *object = instance;
    oasp_ai_plan_release(object->plan);
    oasp_ai_graph_release(object->graph);
    request_release(object->request);
    free(object);
}

static void cache_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_cache_object *object = instance;
    oasp_ai_kv_cache_release(object->cache);
    request_release(object->request);
    free(object);
}

static void dataset_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_dataset_object *object = instance;
    oasp_ai_dataset_release(object->dataset);
    request_release(object->request);
    free(object);
}

static void dataloader_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_dataloader_object *object = instance;
    oasp_ai_dataloader_release(object->loader);
    request_release(object->request);
    free(object);
}

static void data_iterator_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_data_iterator_object *object = instance;
    oasp_ai_batch_release(object->current);
    oasp_ai_data_iterator_release(object->iterator);
    request_release(object->request);
    free(object);
}

static void batch_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_batch_object *object = instance;
    oasp_ai_batch_release(object->batch);
    request_release(object->request);
    free(object);
}

static void tokenizer_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_tokenizer_object *object = instance;
    oasp_ai_tokenizer_release(object->tokenizer);
    request_release(object->request);
    free(object);
}

static void scheduler_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_scheduler_object *object = instance;
    oasp_ai_scheduler_release(object->scheduler);
    request_release(object->request);
    free(object);
}

static void loss_scaler_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_loss_scaler_object *object = instance;
    oasp_ai_loss_scaler_release(object->scaler);
    request_release(object->request);
    free(object);
}

static void state_dict_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_state_dict_object *object = instance;
    oasp_ai_state_dict_release(object->state);
    request_release(object->request);
    free(object);
}

static void event_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_event_object *object = instance;
    oasp_ai_event_release(object->event);
    request_release(object->request);
    free(object);
}

static void parameter_collection_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_parameter_collection_object *object = instance;
    oasp_ai_state_dict_release(object->state);
    request_release(object->request);
    free(object);
}

static void checkpoint_state_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_checkpoint_state_object *object = instance;
    oasp_ai_checkpoint_state_clear(&object->state);
    request_release(object->request);
    free(object);
}

static void device_destroy(oasp_ext_call_context *context, void *instance) {
    (void)context;
    ai_device_object *object = instance;
    request_release(object->request);
    free(object);
}

static int factory_get(oasp_ext_call_context *context, ai_factory *factory, uint32_t member, oasp_ext_result *result, oasp_ext_error *error) {
    (void)error;
    if (member == AI_DEVICE)
        result->value = values->new_string(context, factory->device, strlen(factory->device));
    else if (member == AI_DTYPE) {
        const char *name = oasp_ai_dtype_name(factory->dtype);
        result->value = values->new_string(context, name, strlen(name));
    } else if (member == AI_SEED)
        result->value = values->new_i64(context, (int64_t)factory->request->core->options.seed);
    else if (member == AI_DETERMINISTIC)
        result->value = values->new_bool(context, factory->request->core->options.deterministic);
    else if (member == AI_STRICT_DEVICE)
        result->value = values->new_bool(context, factory->request->core->options.strict_device);
    else
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown OpenASP.AI property");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int factory_set(oasp_ext_call_context *context, ai_factory *factory, uint32_t member, const oasp_ext_value *value, oasp_ext_error *error) {
    if (member == AI_DEVICE) {
        char device[sizeof(factory->device)];
        oasp_ai_error core_error = {0};
        if (get_string(context, value, device, sizeof(device)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Device requires a string");
        if (oasp_ai_backend_find(factory->request->core, device, &core_error) == -2)
            return core_fail(error, &core_error);
        memcpy(factory->device, device, strlen(device) + 1);
        return OASP_EXT_OK;
    }
    if (member == AI_DTYPE) {
        char name[32];
        if (get_string(context, value, name, sizeof(name)) || oasp_ai_dtype_parse(name, &factory->dtype))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "DType is not supported");
        return OASP_EXT_OK;
    }
    int64_t integer;
    if (member == AI_SEED && values->get_i64(context, value, &integer) == OASP_EXT_OK) {
        factory->request->core->options.seed = (uint64_t)integer;
        factory->request->core->rng_counter = 0;
        return OASP_EXT_OK;
    }
    int boolean;
    if ((member == AI_DETERMINISTIC || member == AI_STRICT_DEVICE) && values->get_bool(context, value, &boolean) == OASP_EXT_OK) {
        if (member == AI_DETERMINISTIC)
            factory->request->core->options.deterministic = !!boolean;
        else
            factory->request->core->options.strict_device = !!boolean;
        return OASP_EXT_OK;
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid OpenASP.AI property value");
}

static int factory_tensor_create(oasp_ext_call_context *context, ai_factory *factory, uint32_t member, const oasp_ext_invocation *invocation, oasp_ext_result *result,
                                 oasp_ext_error *error) {
    oasp_ai_error core_error = {0};
    oasp_ai_tensor *tensor = NULL;
    if (member == AI_TENSOR && invocation->argument_count >= 1)
        tensor = tensor_from_array(context, factory, invocation->arguments[0], error);
    else {
        int64_t shape[OASP_AI_MAX_RANK];
        uint32_t rank;
        if (!invocation->argument_count || read_shape(context, invocation->arguments[0], shape, &rank, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        double first = member == AI_ONES ? 1 : 0;
        double second = 1;
        if ((member == AI_FULL || member == AI_RANDOM_UNIFORM || member == AI_RANDOM_NORMAL) && invocation->argument_count > 1 &&
            values->get_f64(context, invocation->arguments[1], &first) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "numeric fill or distribution argument is required");
        if ((member == AI_RANDOM_UNIFORM || member == AI_RANDOM_NORMAL) && invocation->argument_count > 2 &&
            values->get_f64(context, invocation->arguments[2], &second) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "numeric distribution argument is required");
        if (member == AI_RANDOM_UNIFORM || member == AI_RANDOM_NORMAL)
            tensor = oasp_ai_tensor_random(factory->request->core, shape, rank, factory->dtype, first, second, member == AI_RANDOM_NORMAL, &core_error);
        else
            tensor = oasp_ai_tensor_full(factory->request->core, shape, rank, factory->dtype, first, &core_error);
    }
    if (!tensor)
        return error && error->message ? OASP_EXT_ERROR : core_fail(error, &core_error);
    if (strcmp(factory->device, "cpu") && strcmp(factory->device, "cpu:0")) {
        oasp_ai_tensor *migrated = oasp_ai_tensor_to_device(tensor, factory->device, &core_error);
        oasp_ai_tensor_release(tensor);
        tensor = migrated;
        if (!tensor)
            return core_fail(error, &core_error);
    }
    return new_tensor_object(context, factory->request, tensor, NULL, 0, result, error);
}

static int binary_result(oasp_ext_call_context *context, ai_request *request, uint32_t member, ai_tensor_object *left, ai_tensor_object *right,
                         oasp_ext_result *result, oasp_ext_error *error) {
    oasp_ai_error core_error = {0};
    oasp_ai_operator operation = member == AI_ADD || member == TENSOR_ADD   ? OASP_AI_OP_ADD
                                 : member == AI_SUB || member == TENSOR_SUB ? OASP_AI_OP_SUB
                                 : member == AI_MUL || member == TENSOR_MUL ? OASP_AI_OP_MUL
                                 : member == AI_DIV || member == TENSOR_DIV ? OASP_AI_OP_DIV
                                                                           : OASP_AI_OP_MATMUL;
    if (left->graph || right->graph) {
        if (!left->graph || left->graph != right->graph)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "symbolic tensors must belong to the same Graph");
        uint32_t inputs[] = {left->value_id, right->value_id};
        uint32_t output = oasp_ai_graph_operation(left->graph, operation, inputs, 2, NULL, 0, &core_error);
        if (!output)
            return core_fail(error, &core_error);
        return new_tensor_object(context, request, NULL, left->graph, output, result, error);
    }
    oasp_ai_tensor *tensor = operation == OASP_AI_OP_MATMUL ? oasp_ai_matmul(request->core, left->tensor, right->tensor, &core_error)
                                                            : oasp_ai_binary(request->core, operation, left->tensor, right->tensor, &core_error);
    if (!tensor)
        return core_fail(error, &core_error);
    return new_tensor_object(context, request, tensor, NULL, 0, result, error);
}

static int new_module_object(oasp_ext_call_context *context, ai_request *request, oasp_ai_module *module, oasp_ext_result *result, oasp_ext_error *error) {
    if (!module)
        return OASP_EXT_ERROR;
    ai_module_object *object = calloc(1, sizeof(*object));
    if (!object) {
        oasp_ai_module_release(module);
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate Module object");
    }
    object->request = request;
    object->module = module;
    request_retain(request);
    if (objects->new_owned(context, CLASS_MODULE, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    module_destroy(context, object);
    return OASP_EXT_ERROR;
}

static int new_factory_module_object(oasp_ext_call_context *context, ai_factory *factory, oasp_ai_module *module,
                                     oasp_ext_result *result, oasp_ext_error *error, oasp_ai_error *core_error) {
    if (module && oasp_ai_module_to_device(module, factory->device, core_error)) {
        oasp_ai_module_release(module);
        module = NULL;
    }
    return module ? new_module_object(context, factory->request, module, result, error) : core_fail(error, core_error);
}

static int new_state_dict_object(oasp_ext_call_context *context, ai_request *request, oasp_ai_state_dict *state, oasp_ext_result *result, oasp_ext_error *error) {
    ai_state_dict_object *object = state ? calloc(1, sizeof(*object)) : NULL;
    if (!object) {
        oasp_ai_state_dict_release(state);
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate StateDict object");
    }
    object->request = request;
    object->state = state;
    request_retain(request);
    if (objects->new_owned(context, CLASS_STATE_DICT, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    state_dict_destroy(context, object);
    return OASP_EXT_ERROR;
}

static int new_parameter_collection_object(oasp_ext_call_context *context, ai_request *request, oasp_ai_state_dict *state, oasp_ai_state_kind kind, oasp_ext_result *result,
                                           oasp_ext_error *error) {
    ai_parameter_collection_object *object = state ? calloc(1, sizeof(*object)) : NULL;
    if (!object) {
        oasp_ai_state_dict_release(state);
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate ParameterCollection object");
    }
    object->request = request;
    object->state = state;
    object->kind = kind;
    request_retain(request);
    if (objects->new_owned(context, CLASS_PARAMETER_COLLECTION, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    parameter_collection_destroy(context, object);
    return OASP_EXT_ERROR;
}

static int new_checkpoint_state_object(oasp_ext_call_context *context, ai_request *request, oasp_ai_checkpoint_state *state, oasp_ext_result *result, oasp_ext_error *error) {
    ai_checkpoint_state_object *object = calloc(1, sizeof(*object));
    if (!object) {
        oasp_ai_checkpoint_state_clear(state);
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate CheckpointState object");
    }
    object->request = request;
    object->state = *state;
    memset(state, 0, sizeof(*state));
    request_retain(request);
    if (objects->new_owned(context, CLASS_CHECKPOINT_STATE, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    checkpoint_state_destroy(context, object);
    return OASP_EXT_ERROR;
}

static int new_device_object(oasp_ext_call_context *context, ai_request *request, size_t backend_index, const oasp_ai_device_info *info, oasp_ext_result *result,
                             oasp_ext_error *error) {
    oasp_ai_registered_backend *backend = &request->core->backends[backend_index];
    ai_device_object *object = calloc(1, sizeof(*object));
    if (!object)
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate Device object");
    object->request = request;
    object->backend_index = backend_index;
    object->info = *info;
    size_t backend_length = strlen(backend->descriptor->backend_name);
    size_t name_length = info->name.length;
    if (backend_length >= sizeof(object->backend) || name_length >= sizeof(object->name)) {
        free(object);
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "backend device name exceeds the supported length");
    }
    memcpy(object->backend, backend->descriptor->backend_name, backend_length + 1);
    memcpy(object->name, info->name.data, name_length);
    object->name[name_length] = '\0';
    object->info.name = (oasp_ai_string){object->name, name_length};
    request_retain(request);
    if (objects->new_owned(context, CLASS_DEVICE, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    device_destroy(context, object);
    return OASP_EXT_ERROR;
}

static int new_model_object(oasp_ext_call_context *context, ai_request *request, oasp_ai_graph *graph, const char *device, oasp_ext_result *result,
                            oasp_ext_error *error) {
    oasp_ai_error core_error = {0};
    ai_model_object *object = calloc(1, sizeof(*object));
    if (object) {
        object->graph = graph;
        object->plan = oasp_ai_graph_compile(graph, device, &core_error);
    }
    if (!object || !object->plan) {
        free(object);
        oasp_ai_graph_release(graph);
        return core_fail(error, &core_error);
    }
    object->request = request;
    request_retain(request);
    if (objects->new_owned(context, CLASS_MODEL, object, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    model_destroy(context, object);
    return OASP_EXT_ERROR;
}

static oasp_ext_value *diagnostic_array(oasp_ext_call_context *context, char **items, size_t count) {
    int64_t bound = (int64_t)count - 1;
    oasp_ext_value *array = values->new_array(context, &bound, 1);
    for (size_t index = 0; array && index < count; index++) {
        int64_t position = (int64_t)index;
        oasp_ext_value *item = values->new_string(context, items[index], strlen(items[index]));
        int status = item ? values->array_set(context, array, &position, 1, item) : OASP_EXT_ERROR;
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK) {
            values->release(context, array);
            return NULL;
        }
    }
    return array;
}

static int diagnostics_result(oasp_ext_call_context *context, oasp_ai_state_diagnostics *diagnostics, oasp_ext_result *result) {
    static const char *const names[] = {"Missing", "Unexpected", "ShapeMismatch", "DTypeMismatch"};
    char **groups[] = {diagnostics->missing, diagnostics->unexpected, diagnostics->shape_mismatch, diagnostics->dtype_mismatch};
    size_t counts[] = {diagnostics->missing_count, diagnostics->unexpected_count, diagnostics->shape_mismatch_count,
                       diagnostics->dtype_mismatch_count};
    result->value = values->new_map(context);
    for (size_t index = 0; result->value && index < 4; index++) {
        oasp_ext_value *array = diagnostic_array(context, groups[index], counts[index]);
        int status = array ? values->map_set(context, result->value, names[index], strlen(names[index]), array) : OASP_EXT_ERROR;
        if (array)
            values->release(context, array);
        if (status != OASP_EXT_OK)
            return OASP_EXT_ERROR;
    }
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int enumerate_backend_devices(oasp_ext_call_context *context, ai_request *request, const char *selected, oasp_ext_result *result, oasp_ext_error *error) {
    size_t total = request->core->backend_count;
    if (!selected) {
        int64_t bound = (int64_t)total - 1;
        result->value = values->new_array(context, &bound, 1);
        if (!result->value)
            return OASP_EXT_ERROR;
    }
    size_t output_index = 0;
    for (size_t backend_index = 0; backend_index < request->core->backend_count; backend_index++) {
        oasp_ai_registered_backend *backend = &request->core->backends[backend_index];
        oasp_ai_error core_error = {0};
        oasp_ai_device_info item = {.struct_size = sizeof(item)};
        if (oasp_ai_context_device_info(request->core, backend_index, &item, &core_error))
            return core_fail(error, &core_error);
        char canonical[160];
        int length = snprintf(canonical, sizeof(canonical), "%s:%u", backend->descriptor->backend_name, item.ordinal);
        int matches = selected && length > 0 && (size_t)length < sizeof(canonical) &&
                      (!strcmp(selected, canonical) ||
                       (!strcmp(backend->descriptor->backend_name, "cpu") && !item.ordinal && !strcmp(selected, "cpu")));
        if (selected && !matches)
            continue;
        oasp_ext_result device = {0};
        int status = new_device_object(context, request, backend_index, &item, &device, error);
        if (status != OASP_EXT_OK)
            return status;
        if (selected) {
            result->value = device.value;
            return OASP_EXT_OK;
        }
        int64_t position = (int64_t)output_index++;
        status = values->array_set(context, result->value, &position, 1, device.value);
        values->release(context, device.value);
        if (status != OASP_EXT_OK)
            return status;
    }
    if (selected)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "requested AI device is not available");
    return OASP_EXT_OK;
}

static int factory_call(oasp_ext_call_context *context, ai_factory *factory, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    uint32_t member = invocation->member_id;
    oasp_ai_error core_error = {0};
    if (member == AI_DEVICES && !invocation->argument_count)
        return enumerate_backend_devices(context, factory->request, NULL, result, error);
    if (member == AI_GET_DEVICE && invocation->argument_count == 1) {
        char device[128];
        if (get_string(context, invocation->arguments[0], device, sizeof(device)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "GetDevice requires a device name");
        return enumerate_backend_devices(context, factory->request, device, result, error);
    }
    if (member >= AI_ZEROS && member <= AI_TENSOR)
        return factory_tensor_create(context, factory, member, invocation, result, error);
    if (member >= AI_ADD && member <= AI_MATMUL && invocation->argument_count == 2) {
        ai_tensor_object *left;
        ai_tensor_object *right;
        if (get_tensor(context, invocation->arguments[0], &left, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &right, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        return binary_result(context, factory->request, member, left, right, result, error);
    }
    if (member >= AI_RELU && member <= AI_SILU && invocation->argument_count == 1) {
        ai_tensor_object *input;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        oasp_ai_operator operation = member == AI_RELU      ? OASP_AI_OP_RELU
                                     : member == AI_SIGMOID ? OASP_AI_OP_SIGMOID
                                     : member == AI_TANH    ? OASP_AI_OP_TANH
                                     : member == AI_GELU    ? OASP_AI_OP_GELU
                                                           : OASP_AI_OP_SILU;
        if (input->graph) {
            uint32_t output = oasp_ai_graph_operation(input->graph, operation, &input->value_id, 1, NULL, 0, &core_error);
            return output ? new_tensor_object(context, factory->request, NULL, input->graph, output, result, error) : core_fail(error, &core_error);
        }
        oasp_ai_tensor *tensor = oasp_ai_unary(factory->request->core, operation, input->tensor, 0.01, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if (member == AI_SOFTMAX && invocation->argument_count >= 1) {
        ai_tensor_object *input;
        int64_t axis = -1;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if (invocation->argument_count > 1 && values->get_i64(context, invocation->arguments[1], &axis) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if (axis < 0)
            axis += input->tensor ? input->tensor->desc.rank : input->graph->values[input->value_id - 1].desc.rank;
        if (input->graph) {
            int32_t attribute = (int32_t)axis;
            uint32_t output =
                oasp_ai_graph_operation(input->graph, OASP_AI_OP_SOFTMAX, &input->value_id, 1, &attribute, sizeof(attribute), &core_error);
            return output ? new_tensor_object(context, factory->request, NULL, input->graph, output, result, error) : core_fail(error, &core_error);
        }
        oasp_ai_tensor *tensor = oasp_ai_softmax(factory->request->core, input->tensor, (int32_t)axis, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if ((member == AI_SUM || member == AI_MEAN) && invocation->argument_count >= 1) {
        ai_tensor_object *input;
        int64_t axis = -1;
        int keep_dimensions = 0;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if ((invocation->argument_count > 1 && values->get_i64(context, invocation->arguments[1], &axis) != OASP_EXT_OK) ||
            (invocation->argument_count > 2 && values->get_bool(context, invocation->arguments[2], &keep_dimensions) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Sum/Mean requires a Tensor, optional axis, and optional keep-dimensions Boolean");
        uint32_t rank = input->tensor ? input->tensor->desc.rank : input->graph->values[input->value_id - 1].desc.rank;
        if (axis < 0 && axis != -1)
            axis += rank;
        if (axis < -1 || axis >= rank)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Sum/Mean axis is out of range");
        oasp_ai_operator operation = member == AI_SUM ? OASP_AI_OP_SUM : OASP_AI_OP_MEAN;
        if (input->graph)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Sum/Mean graph capture is not implemented; use eager Tensor execution");
        oasp_ai_tensor *tensor = oasp_ai_reduce(factory->request->core, operation, input->tensor, (int32_t)axis,
                                                keep_dimensions, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if (member == AI_CROSS_ENTROPY && invocation->argument_count >= 2 &&
        invocation->argument_count <= 6) {
        ai_tensor_object *logits;
        ai_tensor_object *labels;
        ai_tensor_object *weight = NULL;
        int64_t ignore_index = -100;
        double label_smoothing = 0;
        oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
        if (get_tensor(context, invocation->arguments[0], &logits, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &labels, error) != OASP_EXT_OK ||
            !logits->tensor || !labels->tensor)
            return OASP_EXT_ERROR;
        if (invocation->argument_count > 2 &&
            !is_missing_value(context, invocation->arguments[2]) &&
            (get_tensor(context, invocation->arguments[2], &weight, error) != OASP_EXT_OK ||
             !weight->tensor))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "CrossEntropy weight must be a Tensor or Nothing");
        if ((invocation->argument_count > 3 &&
             values->get_i64(context, invocation->arguments[3], &ignore_index) != OASP_EXT_OK) ||
            (invocation->argument_count > 4 &&
             values->get_f64(context, invocation->arguments[4], &label_smoothing) != OASP_EXT_OK) ||
            (invocation->argument_count > 5 &&
             get_reduction(context, invocation->arguments[5], 0, &reduction)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "CrossEntropy expects optional weight, ignoreIndex, labelSmoothing, and reduction");
        oasp_ai_error core_error = {0};
        oasp_ai_tensor *tensor = oasp_ai_cross_entropy_ex(
            factory->request->core, logits->tensor, labels->tensor,
            weight ? weight->tensor : NULL, ignore_index, label_smoothing,
            reduction, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if ((member >= AI_MSE_LOSS && member <= AI_HUBER_LOSS) && invocation->argument_count >= 2) {
        ai_tensor_object *input;
        ai_tensor_object *target;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &target, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if (!input->tensor || !target->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "loss functions require eager Tensor arguments");
        oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
        int reduction_index = member == AI_HUBER_LOSS ? 3 : 2;
        ai_tensor_object *positive_weight = NULL;
        if (member == AI_BCE_WITH_LOGITS_LOSS && invocation->argument_count > 2 &&
            values->kind(context, invocation->arguments[2]) != OASP_EXT_VALUE_STRING) {
            reduction_index = 3;
            if (!is_missing_value(context, invocation->arguments[2]) &&
                (get_tensor(context, invocation->arguments[2], &positive_weight, error) !=
                     OASP_EXT_OK ||
                 !positive_weight->tensor))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "BCEWithLogitsLoss posWeight must be a Tensor or Nothing");
        }
        if (invocation->argument_count > (size_t)reduction_index &&
            get_reduction(context, invocation->arguments[reduction_index],
                          member == AI_KL_DIV_LOSS, &reduction))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "reduction must be none, mean, sum, or batchmean for KLDivLoss");
        oasp_ai_error core_error = {0};
        oasp_ai_tensor *tensor = NULL;
        if (member == AI_MSE_LOSS)
            tensor = oasp_ai_mse_loss(factory->request->core, input->tensor, target->tensor,
                                      reduction, &core_error);
        else if (member == AI_L1_LOSS)
            tensor = oasp_ai_l1_loss(factory->request->core, input->tensor, target->tensor,
                                     reduction, &core_error);
        else if (member == AI_BCE_LOSS)
            tensor = oasp_ai_bce_loss(factory->request->core, input->tensor, target->tensor,
                                      reduction, &core_error);
        else if (member == AI_BCE_WITH_LOGITS_LOSS)
            tensor = oasp_ai_bce_with_logits_loss_ex(
                factory->request->core, input->tensor, target->tensor,
                positive_weight ? positive_weight->tensor : NULL, reduction, &core_error);
        else if (member == AI_NLL_LOSS)
            tensor = oasp_ai_nll_loss(factory->request->core, input->tensor, target->tensor,
                                      reduction, &core_error);
        else if (member == AI_KL_DIV_LOSS) {
            int log_target = 0;
            if (invocation->argument_count > 3 &&
                values->get_bool(context, invocation->arguments[3], &log_target) != OASP_EXT_OK)
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "KLDivLoss logTarget must be Boolean");
            tensor = oasp_ai_kl_div_loss(factory->request->core, input->tensor, target->tensor,
                                         reduction, log_target, &core_error);
        } else {
            double delta = 1;
            if (invocation->argument_count > 2 &&
                values->get_f64(context, invocation->arguments[2], &delta) != OASP_EXT_OK)
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "HuberLoss delta must be numeric");
            tensor = oasp_ai_huber_loss(factory->request->core, input->tensor, target->tensor,
                                        delta, reduction, &core_error);
        }
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error)
                      : core_fail(error, &core_error);
    }
    if ((member == AI_SMOOTH_L1_LOSS || member == AI_FOCAL_LOSS ||
         member == AI_DICE_LOSS || member == AI_INFO_NCE_LOSS) &&
        invocation->argument_count >= 2) {
        ai_tensor_object *input;
        ai_tensor_object *target;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &target, error) != OASP_EXT_OK ||
            !input->tensor || !target->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "loss functions require two eager Tensor arguments");
        oasp_ai_tensor *tensor = NULL;
        if (member == AI_SMOOTH_L1_LOSS) {
            double beta = 1;
            oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
            if ((invocation->argument_count > 2 &&
                 values->get_f64(context, invocation->arguments[2], &beta) != OASP_EXT_OK) ||
                (invocation->argument_count > 3 &&
                 get_reduction(context, invocation->arguments[3], 0, &reduction)))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "SmoothL1Loss expects input, target, optional beta, and reduction");
            tensor = oasp_ai_smooth_l1_loss(factory->request->core, input->tensor,
                                            target->tensor, beta, reduction, &core_error);
        } else if (member == AI_FOCAL_LOSS) {
            double alpha = 0.25;
            double gamma = 2;
            oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
            if ((invocation->argument_count > 2 &&
                 values->get_f64(context, invocation->arguments[2], &alpha) != OASP_EXT_OK) ||
                (invocation->argument_count > 3 &&
                 values->get_f64(context, invocation->arguments[3], &gamma) != OASP_EXT_OK) ||
                (invocation->argument_count > 4 &&
                 get_reduction(context, invocation->arguments[4], 0, &reduction)))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "FocalLoss expects input, target, optional alpha, gamma, and reduction");
            tensor = oasp_ai_focal_loss(factory->request->core, input->tensor,
                                        target->tensor, alpha, gamma, reduction, &core_error);
        } else if (member == AI_DICE_LOSS) {
            double smooth = 1;
            double epsilon = 1e-7;
            int from_logits = 1;
            oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
            if ((invocation->argument_count > 2 &&
                 values->get_f64(context, invocation->arguments[2], &smooth) != OASP_EXT_OK) ||
                (invocation->argument_count > 3 &&
                 values->get_f64(context, invocation->arguments[3], &epsilon) != OASP_EXT_OK) ||
                (invocation->argument_count > 4 &&
                 values->get_bool(context, invocation->arguments[4], &from_logits) != OASP_EXT_OK) ||
                (invocation->argument_count > 5 &&
                 get_reduction(context, invocation->arguments[5], 0, &reduction)))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "DiceLoss expects input, target, optional smooth, epsilon, fromLogits, and reduction");
            tensor = oasp_ai_dice_loss(factory->request->core, input->tensor,
                                       target->tensor, smooth, epsilon, from_logits,
                                       reduction, &core_error);
        } else {
            double temperature = 0.07;
            int symmetric = 0;
            oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
            if ((invocation->argument_count > 2 &&
                 values->get_f64(context, invocation->arguments[2], &temperature) != OASP_EXT_OK) ||
                (invocation->argument_count > 3 &&
                 values->get_bool(context, invocation->arguments[3], &symmetric) != OASP_EXT_OK) ||
                (invocation->argument_count > 4 &&
                 get_reduction(context, invocation->arguments[4], 0, &reduction)))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                            "InfoNCELoss expects query, key, optional temperature, symmetric, and reduction");
            tensor = oasp_ai_info_nce_loss(factory->request->core, input->tensor,
                                           target->tensor, temperature, symmetric,
                                           reduction, &core_error);
        }
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0,
                                          result, error)
                      : core_fail(error, &core_error);
    }
    if (member == AI_CTC_LOSS && invocation->argument_count >= 4 &&
        invocation->argument_count <= 7) {
        ai_tensor_object *log_probabilities;
        ai_tensor_object *targets;
        ai_tensor_object *input_lengths;
        ai_tensor_object *target_lengths;
        int64_t blank = 0;
        int zero_infinity = 0;
        oasp_ai_reduction reduction = OASP_AI_REDUCTION_MEAN;
        if (get_tensor(context, invocation->arguments[0], &log_probabilities, error) !=
                OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &targets, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[2], &input_lengths, error) !=
                OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[3], &target_lengths, error) !=
                OASP_EXT_OK ||
            !log_probabilities->tensor || !targets->tensor || !input_lengths->tensor ||
            !target_lengths->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "CTCLoss requires four eager Tensor arguments");
        if ((invocation->argument_count > 4 &&
             values->get_i64(context, invocation->arguments[4], &blank) != OASP_EXT_OK) ||
            (invocation->argument_count > 5 &&
             values->get_bool(context, invocation->arguments[5], &zero_infinity) !=
                 OASP_EXT_OK) ||
            (invocation->argument_count > 6 &&
             get_reduction(context, invocation->arguments[6], 0, &reduction)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "CTCLoss expects optional blank, zeroInfinity, and reduction");
        oasp_ai_tensor *tensor = oasp_ai_ctc_loss(
            factory->request->core, log_probabilities->tensor, targets->tensor,
            input_lengths->tensor, target_lengths->tensor, blank, zero_infinity,
            reduction, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0,
                                          result, error)
                      : core_fail(error, &core_error);
    }
    if ((member == AI_LAYER_NORM || member == AI_RMS_NORM) && invocation->argument_count >= 1 &&
        values->kind(context, invocation->arguments[0]) == OASP_EXT_VALUE_OBJECT) {
        ai_tensor_object *input;
        ai_tensor_object *weight = NULL;
        ai_tensor_object *bias = NULL;
        double epsilon = 1e-5;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK || !input->tensor)
            return OASP_EXT_ERROR;
        if (invocation->argument_count > 1 &&
            (get_tensor(context, invocation->arguments[1], &weight, error) != OASP_EXT_OK || !weight->tensor))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "normalization weight must be a Tensor");
        if (member == AI_LAYER_NORM && invocation->argument_count > 2 &&
            (get_tensor(context, invocation->arguments[2], &bias, error) != OASP_EXT_OK || !bias->tensor))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "LayerNorm bias must be a Tensor");
        size_t epsilon_index = member == AI_LAYER_NORM ? 3 : 2;
        if (invocation->argument_count > epsilon_index &&
            values->get_f64(context, invocation->arguments[epsilon_index], &epsilon) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "normalization epsilon must be numeric");
        oasp_ai_tensor *tensor = oasp_ai_layer_norm(factory->request->core, input->tensor, weight ? weight->tensor : NULL,
                                                    bias ? bias->tensor : NULL, epsilon, member == AI_RMS_NORM, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if (member == AI_ATTENTION && invocation->argument_count >= 3) {
        ai_tensor_object *query;
        ai_tensor_object *key;
        ai_tensor_object *value;
        ai_tensor_object *mask = NULL;
        int causal = 0;
        if (get_tensor(context, invocation->arguments[0], &query, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &key, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[2], &value, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if (invocation->argument_count > 3 &&
            (get_tensor(context, invocation->arguments[3], &mask, error) != OASP_EXT_OK || !mask->tensor))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "attention mask must be a Tensor");
        if (invocation->argument_count > 4 && values->get_bool(context, invocation->arguments[4], &causal) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "attention causal flag must be Boolean");
        oasp_ai_tensor *tensor = oasp_ai_attention(factory->request->core, query->tensor, key->tensor, value->tensor,
                                                   mask ? mask->tensor : NULL, causal, &core_error);
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if ((member == AI_SEQUENTIAL || member == AI_RELU) && !invocation->argument_count) {
        oasp_ai_error core_error = {0};
        oasp_ai_module *module = oasp_ai_module_activation(
            factory->request->core, member == AI_SEQUENTIAL ? OASP_AI_MODULE_SEQUENTIAL : OASP_AI_MODULE_RELU, 0, &core_error);
        return new_factory_module_object(context, factory, module, result, error, &core_error);
    }
    if (member == AI_LINEAR && invocation->argument_count >= 2) {
        int64_t input_features;
        int64_t output_features;
        int with_bias = 1;
        if (values->get_i64(context, invocation->arguments[0], &input_features) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[1], &output_features) != OASP_EXT_OK ||
            (invocation->argument_count > 2 && values->get_bool(context, invocation->arguments[2], &with_bias) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Linear requires input features, output features, and optional bias");
        oasp_ai_error core_error = {0};
        oasp_ai_module *module =
            oasp_ai_module_linear(factory->request->core, (uint64_t)input_features, (uint64_t)output_features, with_bias, &core_error);
        return new_factory_module_object(context, factory, module, result, error, &core_error);
    }
    if ((member == AI_RELU_MODULE || member == AI_DROPOUT) && invocation->argument_count <= 1) {
        double argument = 0;
        if (member == AI_DROPOUT &&
            (!invocation->argument_count || values->get_f64(context, invocation->arguments[0], &argument) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Dropout requires a probability");
        oasp_ai_error core_error = {0};
        oasp_ai_module *module = oasp_ai_module_activation(
            factory->request->core, member == AI_RELU_MODULE ? OASP_AI_MODULE_RELU : OASP_AI_MODULE_DROPOUT, argument, &core_error);
        return new_factory_module_object(context, factory, module, result, error, &core_error);
    }
    if ((member == AI_LAYER_NORM_MODULE || member == AI_RMS_NORM_MODULE || member == AI_LAYER_NORM || member == AI_RMS_NORM) &&
        invocation->argument_count >= 1 && values->kind(context, invocation->arguments[0]) == OASP_EXT_VALUE_I64) {
        int64_t dimensions;
        double epsilon = 1e-5;
        if (values->get_i64(context, invocation->arguments[0], &dimensions) != OASP_EXT_OK ||
            (invocation->argument_count > 1 && values->get_f64(context, invocation->arguments[1], &epsilon) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "normalization requires dimensions and optional epsilon");
        oasp_ai_error core_error = {0};
        int rms_only = member == AI_RMS_NORM_MODULE || member == AI_RMS_NORM;
        oasp_ai_module *module = oasp_ai_module_normalization(factory->request->core, (uint64_t)dimensions, epsilon, rms_only, &core_error);
        return new_factory_module_object(context, factory, module, result, error, &core_error);
    }
    if (member == AI_EMBEDDING && invocation->argument_count == 2) {
        int64_t vocabulary;
        int64_t dimensions;
        if (values->get_i64(context, invocation->arguments[0], &vocabulary) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[1], &dimensions) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Embedding requires vocabulary and dimensions");
        oasp_ai_error core_error = {0};
        oasp_ai_module *module = oasp_ai_module_embedding(factory->request->core, (uint64_t)vocabulary, (uint64_t)dimensions, &core_error);
        return new_factory_module_object(context, factory, module, result, error, &core_error);
    }
    if (member == AI_MODULE && invocation->argument_count == 1) {
        char name[256];
        if (get_string(context, invocation->arguments[0], name, sizeof(name)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Module requires a name");
        oasp_ai_error core_error = {0};
        oasp_ai_module *module = oasp_ai_module_create(factory->request->core, name, &core_error);
        return new_factory_module_object(context, factory, module, result, error, &core_error);
    }
    if ((member == AI_SGD || member == AI_ADAM || member == AI_ADAMW) && invocation->argument_count >= 2) {
        ai_module_object *module;
        double learning_rate;
        double weight_decay = 0;
        if (get_native(context, invocation->arguments[0], CLASS_MODULE, (void **)&module, error) != OASP_EXT_OK ||
            values->get_f64(context, invocation->arguments[1], &learning_rate) != OASP_EXT_OK ||
            (invocation->argument_count > 2 && values->get_f64(context, invocation->arguments[2], &weight_decay) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "optimizer requires Module, learning rate, and optional weight decay");
        oasp_ai_optimizer_kind kind = member == AI_SGD ? OASP_AI_OPTIMIZER_SGD
                                      : member == AI_ADAM ? OASP_AI_OPTIMIZER_ADAM
                                                          : OASP_AI_OPTIMIZER_ADAMW;
        oasp_ai_error core_error = {0};
        ai_optimizer_object *object = calloc(1, sizeof(*object));
        if (object)
            object->optimizer = oasp_ai_optimizer_create(module->module, kind, learning_rate, weight_decay, &core_error);
        if (!object || !object->optimizer) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_OPTIMIZER, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        optimizer_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_GRAPH && invocation->argument_count == 1) {
        char name[256];
        if (get_string(context, invocation->arguments[0], name, sizeof(name)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Graph requires a name");
        oasp_ai_error core_error = {0};
        ai_graph_object *object = calloc(1, sizeof(*object));
        if (object)
            object->graph = oasp_ai_graph_create(factory->request->core, name, &core_error);
        if (!object || !object->graph) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        memcpy(object->device, factory->device, strlen(factory->device) + 1);
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_GRAPH, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        graph_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_BEGIN_TRACE && invocation->argument_count <= 1) {
        char name[256] = "Trace";
        if (invocation->argument_count && get_string(context, invocation->arguments[0], name, sizeof(name)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "BeginTrace name must be a string");
        ai_graph_object *object = calloc(1, sizeof(*object));
        if (object)
            object->graph = oasp_ai_graph_create(factory->request->core, name, &core_error);
        if (!object || !object->graph) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        memcpy(object->device, factory->device, strlen(factory->device) + 1);
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_TRACE, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        graph_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_KV_CACHE && invocation->argument_count >= 5) {
        int64_t layers;
        int64_t batch;
        int64_t heads;
        int64_t head_dimension;
        int64_t page_tokens;
        oasp_ai_dtype dtype = factory->dtype;
        char dtype_name[32];
        if (values->get_i64(context, invocation->arguments[0], &layers) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[1], &batch) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[2], &heads) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[3], &head_dimension) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[4], &page_tokens) != OASP_EXT_OK || layers <= 0 || batch <= 0 || heads <= 0 ||
            head_dimension <= 0 || page_tokens <= 0 || layers > UINT32_MAX || batch > UINT32_MAX || heads > UINT32_MAX ||
            head_dimension > UINT32_MAX || page_tokens > UINT32_MAX)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "KVCache requires positive layer, batch, head, dimension, and page sizes");
        if (invocation->argument_count > 5 &&
            (get_string(context, invocation->arguments[5], dtype_name, sizeof(dtype_name)) || oasp_ai_dtype_parse(dtype_name, &dtype)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "KVCache dtype is unsupported");
        ai_cache_object *object = calloc(1, sizeof(*object));
        if (object)
            object->cache = oasp_ai_kv_cache_create(factory->request->core, (uint32_t)layers, (uint32_t)batch, (uint32_t)heads,
                                                    (uint32_t)head_dimension, (uint32_t)page_tokens, dtype, &core_error);
        if (!object || !object->cache) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_KV_CACHE, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        cache_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_SAMPLE && invocation->argument_count >= 1) {
        ai_tensor_object *logits;
        double temperature = 1;
        int64_t top_k = 0;
        double top_p = 1;
        double repetition_penalty = 1;
        int64_t *history = NULL;
        size_t history_count = 0;
        if (get_tensor(context, invocation->arguments[0], &logits, error) != OASP_EXT_OK || !logits->tensor)
            return OASP_EXT_ERROR;
        if ((invocation->argument_count > 1 && values->get_f64(context, invocation->arguments[1], &temperature) != OASP_EXT_OK) ||
            (invocation->argument_count > 2 && values->get_i64(context, invocation->arguments[2], &top_k) != OASP_EXT_OK) ||
            (invocation->argument_count > 3 && values->get_f64(context, invocation->arguments[3], &top_p) != OASP_EXT_OK) || top_k < 0 ||
            top_k > UINT32_MAX)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Sample has invalid temperature, top-k, or top-p arguments");
        if (invocation->argument_count > 4 &&
            read_i64_array(context, invocation->arguments[4], &history, &history_count, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if (invocation->argument_count > 5 &&
            values->get_f64(context, invocation->arguments[5], &repetition_penalty) != OASP_EXT_OK) {
            free(history);
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "repetition penalty must be numeric");
        }
        int64_t token;
        int status = oasp_ai_sample(factory->request->core, logits->tensor, temperature, (uint32_t)top_k, top_p, history, history_count,
                                    repetition_penalty, &token, &core_error);
        free(history);
        if (status)
            return core_fail(error, &core_error);
        result->value = values->new_i64(context, token);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if ((member == AI_SAVE_CHECKPOINT || member == AI_LOAD_CHECKPOINT) && invocation->argument_count >= 3) {
        char path[4096];
        ai_module_object *module;
        ai_optimizer_object *optimizer;
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) ||
            get_checkpoint_module(context, invocation->arguments[1], &module, error) != OASP_EXT_OK ||
            get_native(context, invocation->arguments[2], CLASS_OPTIMIZER, (void **)&optimizer, error) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "checkpoint requires path, Module, and Optimizer");
        oasp_ai_checkpoint_state state = {0};
        if (member == AI_SAVE_CHECKPOINT) {
            void *encoded_metadata = NULL;
            size_t metadata_index = 3;
            ai_scheduler_object *scheduler;
            if (invocation->argument_count > metadata_index &&
                try_get_native(context, invocation->arguments[metadata_index], CLASS_SCHEDULER, (void **)&scheduler)) {
                state.scheduler = scheduler->scheduler;
                metadata_index++;
            }
            ai_loss_scaler_object *scaler;
            if (invocation->argument_count > metadata_index &&
                try_get_native(context, invocation->arguments[metadata_index], CLASS_LOSS_SCALER, (void **)&scaler)) {
                state.loss_scaler = scaler->scaler;
                metadata_index++;
            }
            ai_data_iterator_object *iterator;
            if (invocation->argument_count > metadata_index &&
                try_get_native(context, invocation->arguments[metadata_index], CLASS_DATA_ITERATOR, (void **)&iterator)) {
                state.data_iterator = iterator->iterator;
                metadata_index++;
            }
            if (invocation->argument_count > metadata_index) {
                if (oasp_ai_metadata_encode(context, values, objects, invocation->arguments[metadata_index],
                                            &encoded_metadata, &state.metadata_size, error) != OASP_EXT_OK)
                    return OASP_EXT_ERROR;
                state.metadata = encoded_metadata;
            }
            int64_t epoch = 0;
            int64_t global_step = 0;
            if (invocation->argument_count > metadata_index + 1 &&
                (values->get_i64(context, invocation->arguments[metadata_index + 1], &epoch) != OASP_EXT_OK || epoch < 0)) {
                free(encoded_metadata);
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "checkpoint epoch must be a non-negative integer");
            }
            if (invocation->argument_count > metadata_index + 2 &&
                (values->get_i64(context, invocation->arguments[metadata_index + 2], &global_step) != OASP_EXT_OK || global_step < 0)) {
                free(encoded_metadata);
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "checkpoint global step must be a non-negative integer");
            }
            state.epoch = (uint64_t)epoch;
            state.global_step = (uint64_t)global_step;
            int status = oasp_ai_save_checkpoint_full(path, module->module, optimizer->optimizer, &state, &core_error);
            free(encoded_metadata);
            if (status)
                return core_fail(error, &core_error);
            result->value = values->new_empty(context);
            return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
        }
        size_t argument = 3;
        ai_scheduler_object *scheduler;
        if (invocation->argument_count > argument &&
            try_get_native(context, invocation->arguments[argument], CLASS_SCHEDULER, (void **)&scheduler)) {
            state.scheduler = scheduler->scheduler;
            argument++;
        }
        ai_loss_scaler_object *scaler;
        if (invocation->argument_count > argument &&
            try_get_native(context, invocation->arguments[argument], CLASS_LOSS_SCALER, (void **)&scaler)) {
            state.loss_scaler = scaler->scaler;
            argument++;
        }
        ai_data_iterator_object *iterator;
        if (invocation->argument_count > argument &&
            try_get_native(context, invocation->arguments[argument], CLASS_DATA_ITERATOR, (void **)&iterator)) {
            state.data_iterator = iterator->iterator;
            argument++;
        }
        int strict = 1;
        if (invocation->argument_count > argument &&
            values->get_bool(context, invocation->arguments[argument], &strict) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "checkpoint strict flag must be Boolean");
        if (oasp_ai_load_checkpoint_full(path, module->module, optimizer->optimizer, &state, strict, &core_error))
            return core_fail(error, &core_error);
        return new_checkpoint_state_object(context, factory->request, &state, result, error);
    }
    if (member == AI_SAVE_BUNDLE && invocation->argument_count >= 6) {
        char path[4096];
        char entry_class[512];
        ai_module_object *module;
        oasp_ext_bytes bytecode;
        oasp_ext_bytes configuration;
        oasp_ext_bytes dependencies;
        oasp_ext_bytes signing_key = {0};
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) ||
            get_native(context, invocation->arguments[1], CLASS_MODULE, (void **)&module, error) != OASP_EXT_OK ||
            get_string(context, invocation->arguments[2], entry_class, sizeof(entry_class)) ||
            values->get_data(context, invocation->arguments[3], &bytecode) != OASP_EXT_OK ||
            values->get_data(context, invocation->arguments[4], &configuration) != OASP_EXT_OK ||
            values->get_data(context, invocation->arguments[5], &dependencies) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT,
                        "SaveBundle requires path, Module, entry class, bytecode, configuration, and dependency manifest");
        if (invocation->argument_count > 6 && values->get_data(context, invocation->arguments[6], &signing_key) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "SaveBundle signing key must be a PEM string");
        char *key = NULL;
        if (signing_key.length) {
            key = malloc(signing_key.length + 1);
            if (!key)
                return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate signing key");
            memcpy(key, signing_key.data, signing_key.length);
            key[signing_key.length] = '\0';
        }
        oasp_ai_bundle_security security = {.signing_private_key_pem = key, .allow_unsigned = key == NULL};
        int status = oasp_ai_save_bundle(path, module->module, entry_class, bytecode.data, bytecode.length,
                                         configuration.data, configuration.length, dependencies.data, dependencies.length,
                                         &security, &core_error);
        free(key);
        if (status)
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member == AI_TENSOR_DATASET && invocation->argument_count == 2) {
        ai_tensor_object *inputs;
        ai_tensor_object *labels;
        if (get_tensor(context, invocation->arguments[0], &inputs, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &labels, error) != OASP_EXT_OK || !inputs->tensor || !labels->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "TensorDataset requires input and label Tensors");
        ai_dataset_object *object = calloc(1, sizeof(*object));
        if (object)
            object->dataset = oasp_ai_dataset_from_tensors(inputs->tensor, labels->tensor, &core_error);
        if (!object || !object->dataset) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_DATASET, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        dataset_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_MNIST && invocation->argument_count >= 2) {
        char images_path[4096];
        char labels_path[4096];
        int normalize = 1;
        if (get_string(context, invocation->arguments[0], images_path, sizeof(images_path)) ||
            get_string(context, invocation->arguments[1], labels_path, sizeof(labels_path)) ||
            (invocation->argument_count > 2 && values->get_bool(context, invocation->arguments[2], &normalize) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "MNIST requires image path, label path, and optional normalize flag");
        ai_dataset_object *object = calloc(1, sizeof(*object));
        if (object)
            object->dataset = oasp_ai_dataset_mnist(factory->request->core, images_path, labels_path, normalize, &core_error);
        if (!object || !object->dataset) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_DATASET, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        dataset_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_TOKENIZER && invocation->argument_count >= 1) {
        char path[4096];
        int unknown = 1;
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) ||
            (invocation->argument_count > 1 && values->get_bool(context, invocation->arguments[1], &unknown) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tokenizer requires a vocabulary path and optional unknown-token flag");
        ai_tokenizer_object *object = calloc(1, sizeof(*object));
        if (object)
            object->tokenizer = oasp_ai_tokenizer_load(path, unknown, &core_error);
        if (!object || !object->tokenizer) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_TOKENIZER, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        tokenizer_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if ((member == AI_QUANTIZE || member == AI_DEQUANTIZE) && invocation->argument_count >= 1) {
        ai_tensor_object *input;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK || !input->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Quantize/Dequantize requires a concrete Tensor");
        oasp_ai_tensor *tensor = NULL;
        if (member == AI_QUANTIZE) {
            int64_t bits = 8;
            int64_t axis = -1;
            char method[32] = "absmax";
            if ((invocation->argument_count > 1 && values->get_i64(context, invocation->arguments[1], &bits) != OASP_EXT_OK) ||
                (invocation->argument_count > 2 && values->get_i64(context, invocation->arguments[2], &axis) != OASP_EXT_OK) ||
                (invocation->argument_count > 3 && get_string(context, invocation->arguments[3], method, sizeof(method))))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Quantize expects Tensor, bits, axis, and calibration method");
            tensor = oasp_ai_tensor_quantize(input->tensor, (uint32_t)bits, (int32_t)axis, method, &core_error);
        } else {
            char dtype_name[32] = "float32";
            oasp_ai_dtype dtype;
            if ((invocation->argument_count > 1 && get_string(context, invocation->arguments[1], dtype_name, sizeof(dtype_name))) ||
                oasp_ai_dtype_parse(dtype_name, &dtype))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Dequantize destination dtype is unsupported");
            tensor = oasp_ai_tensor_dequantize(input->tensor, dtype, &core_error);
        }
        return tensor ? new_tensor_object(context, factory->request, tensor, NULL, 0, result, error) : core_fail(error, &core_error);
    }
    if ((member == AI_STEP_SCHEDULER || member == AI_COSINE_SCHEDULER || member == AI_LINEAR_SCHEDULER) &&
        invocation->argument_count >= 4) {
        ai_optimizer_object *optimizer;
        int64_t total_steps;
        int64_t interval;
        double factor;
        if (get_native(context, invocation->arguments[0], CLASS_OPTIMIZER, (void **)&optimizer, error) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[1], &total_steps) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[2], &interval) != OASP_EXT_OK ||
            values->get_f64(context, invocation->arguments[3], &factor) != OASP_EXT_OK || total_steps <= 0 || interval <= 0)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "scheduler requires Optimizer, total steps, interval/warmup, and factor/minimum");
        oasp_ai_scheduler_kind kind = member == AI_STEP_SCHEDULER     ? OASP_AI_SCHEDULER_STEP
                                      : member == AI_COSINE_SCHEDULER ? OASP_AI_SCHEDULER_COSINE
                                                                      : OASP_AI_SCHEDULER_LINEAR_WARMUP;
        ai_scheduler_object *object = calloc(1, sizeof(*object));
        if (object)
            object->scheduler =
                oasp_ai_scheduler_create(optimizer->optimizer, kind, (uint64_t)total_steps, (uint64_t)interval, factor, &core_error);
        if (!object || !object->scheduler) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_SCHEDULER, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        scheduler_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_LOSS_SCALER && invocation->argument_count <= 4) {
        double initial = 65536;
        double growth = 2;
        double backoff = 0.5;
        int64_t interval = 2000;
        if ((invocation->argument_count > 0 && values->get_f64(context, invocation->arguments[0], &initial) != OASP_EXT_OK) ||
            (invocation->argument_count > 1 && values->get_f64(context, invocation->arguments[1], &growth) != OASP_EXT_OK) ||
            (invocation->argument_count > 2 && values->get_f64(context, invocation->arguments[2], &backoff) != OASP_EXT_OK) ||
            (invocation->argument_count > 3 && values->get_i64(context, invocation->arguments[3], &interval) != OASP_EXT_OK) || interval <= 0)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "LossScaler arguments are invalid");
        ai_loss_scaler_object *object = calloc(1, sizeof(*object));
        if (object)
            object->scaler = oasp_ai_loss_scaler_create(initial, growth, backoff, (uint32_t)interval, &core_error);
        if (!object || !object->scaler) {
            free(object);
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_LOSS_SCALER, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        loss_scaler_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_MEMORY_STATS) {
        const oasp_ai_metrics *metrics = oasp_ai_context_metrics(factory->request->core);
        result->value = values->new_map(context);
        oasp_ext_value *live = values->new_i64(context, (int64_t)metrics->live_bytes);
        oasp_ext_value *peak = values->new_i64(context, (int64_t)metrics->peak_bytes);
        int status = result->value && live && peak ? values->map_set(context, result->value, "LiveBytes", 9, live) : OASP_EXT_ERROR;
        if (status == OASP_EXT_OK)
            status = values->map_set(context, result->value, "PeakBytes", 9, peak);
        if (live)
            values->release(context, live);
        if (peak)
            values->release(context, peak);
        return status;
    }
    if (member == AI_BACKEND_INFO) {
        result->value = values->new_string(context, "reference-cpu/1.0.0", 19);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member == AI_SYNCHRONIZE) {
        oasp_ai_error core_error = {0};
        if (oasp_ai_context_synchronize(factory->request->core, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_bool(context, 1);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member == AI_LOAD_MODEL && invocation->argument_count >= 1) {
        char path[4096];
        if (get_string(context, invocation->arguments[0], path, sizeof(path)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "LoadModel requires a path");
        oasp_ai_error core_error = {0};
        ai_model_object *object = calloc(1, sizeof(*object));
        if (object)
            object->graph = oasp_ai_graph_load(factory->request->core, path, &core_error);
        if (object && object->graph)
            object->plan = oasp_ai_graph_compile(object->graph, factory->device, &core_error);
        if (!object || !object->plan) {
            if (object) {
                oasp_ai_graph_release(object->graph);
                free(object);
            }
            return core_fail(error, &core_error);
        }
        object->request = factory->request;
        request_retain(object->request);
        if (objects->new_owned(context, CLASS_MODEL, object, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        model_destroy(context, object);
        return OASP_EXT_ERROR;
    }
    if (member == AI_LOAD_ONNX && invocation->argument_count == 1) {
        char path[4096];
        if (get_string(context, invocation->arguments[0], path, sizeof(path)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "LoadONNX requires a path");
        oasp_ai_graph *graph = oasp_ai_graph_import_onnx(factory->request->core, path, &core_error);
        return graph ? new_model_object(context, factory->request, graph, factory->device, result, error)
                     : core_fail(error, &core_error);
    }
    if (member == AI_PROFILER_START) {
        int detailed = 0;
        if (invocation->argument_count &&
            values->get_bool(context, invocation->arguments[0], &detailed) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ProfilerStart accepts an optional detailed Boolean");
        if (oasp_ai_context_profiler_start(factory->request->core, detailed, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member == AI_PROFILER_STOP) {
        if (oasp_ai_context_profiler_stop(factory->request->core, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (member == AI_PROFILER_EXPORT && invocation->argument_count == 1) {
        char path[4096];
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) ||
            oasp_ai_context_profiler_export_chrome_trace(factory->request->core, path, &core_error))
            return core_error.message ? core_fail(error, &core_error)
                                      : fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ProfilerExport requires a path");
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid OpenASP.AI method arguments");
}

static int factory_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    if (invocation->operation == OASP_EXT_OP_GET)
        return factory_get(context, instance, invocation->member_id, result, error);
    if (invocation->operation == OASP_EXT_OP_SET)
        return factory_set(context, instance, invocation->member_id, invocation->value, error);
    if (invocation->operation == OASP_EXT_OP_CALL)
        return factory_call(context, instance, invocation, result, error);
    return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unsupported OpenASP.AI operation");
}

static int tensor_to_array(oasp_ext_call_context *context, ai_tensor_object *object, oasp_ext_result *result, oasp_ext_error *error) {
    if (!object->tensor || object->tensor->element_count > object->request->core->options.maximum_array_elements)
        return fail(error, 7, OASP_EXT_ERROR_RESOURCE, "Tensor is too large for ToArray");
    oasp_ai_error core_error = {0};
    oasp_ai_tensor *host = oasp_ai_tensor_to_device(object->tensor, "cpu:0", &core_error);
    if (!host)
        return core_fail(error, &core_error);
    int64_t bound = (int64_t)object->tensor->element_count - 1;
    result->value = values->new_array(context, &bound, 1);
    for (uint64_t index = 0; result->value && index < host->element_count; index++) {
        int64_t array_index = (int64_t)index;
        oasp_ext_value *item = values->new_f64(context, oasp_ai_tensor_read(host, index));
        int status = item ? values->array_set(context, result->value, &array_index, 1, item) : OASP_EXT_ERROR;
        if (item)
            values->release(context, item);
        if (status != OASP_EXT_OK) {
            oasp_ai_tensor_release(host);
            return OASP_EXT_ERROR;
        }
    }
    oasp_ai_tensor_release(host);
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int tensor_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_tensor_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET) {
        if (invocation->member_id == TENSOR_RANK)
            result->value = values->new_i64(context, object->tensor ? object->tensor->desc.rank : object->graph->values[object->value_id - 1].desc.rank);
        else if (invocation->member_id == TENSOR_SIZE)
            result->value = values->new_i64(context, object->tensor ? (int64_t)object->tensor->element_count : 0);
        else if (invocation->member_id == TENSOR_SHAPE) {
            const oasp_ai_tensor_desc *desc = object->tensor ? &object->tensor->desc : &object->graph->values[object->value_id - 1].desc;
            return shape_value(context, desc->shape, desc->rank, &result->value);
        } else if (invocation->member_id == TENSOR_STRIDES && object->tensor)
            return shape_value(context, object->tensor->desc.strides, object->tensor->desc.rank, &result->value);
        else if (invocation->member_id == TENSOR_DTYPE) {
            const oasp_ai_tensor_desc *desc = object->tensor ? &object->tensor->desc : &object->graph->values[object->value_id - 1].desc;
            const char *name = oasp_ai_dtype_name(desc->dtype);
            result->value = values->new_string(context, name, strlen(name));
        } else if (invocation->member_id == TENSOR_DEVICE && object->tensor) {
            char device[96];
            if (oasp_ai_tensor_device(object->tensor, device, sizeof(device)))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "unable to identify Tensor device");
            result->value = values->new_string(context, device, strlen(device));
        }
        else if (invocation->member_id == TENSOR_BYTES)
            result->value = values->new_i64(context, object->tensor ? (int64_t)oasp_ai_tensor_bytes(object->tensor) : 0);
        else if (invocation->member_id == TENSOR_REQUIRES_GRAD && object->tensor)
            result->value = values->new_bool(context, !!(object->tensor->desc.flags & OASP_AI_TENSOR_REQUIRES_GRAD));
        else if (invocation->member_id == TENSOR_CONTIGUOUS_PROPERTY && object->tensor)
            result->value = values->new_bool(context, !!(object->tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS));
        else if (invocation->member_id == TENSOR_GRAD && object->tensor) {
            oasp_ai_tensor *gradient = oasp_ai_tensor_gradient(object->tensor);
            if (!gradient)
                result->value = values->new_empty(context);
            else
                return new_tensor_object(context, object->request, gradient, NULL, 0, result, error);
        } else
            return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown or unavailable Tensor property");
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation == OASP_EXT_OP_SET && invocation->member_id == TENSOR_REQUIRES_GRAD && object->tensor) {
        int enabled;
        oasp_ai_error core_error = {0};
        if (values->get_bool(context, invocation->value, &enabled) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "RequiresGrad requires a Boolean");
        return oasp_ai_tensor_requires_grad(object->tensor, enabled, &core_error) ? core_fail(error, &core_error) : OASP_EXT_OK;
    }
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Tensor member is not callable");
    if (invocation->member_id >= TENSOR_ADD && invocation->member_id <= TENSOR_MATMUL && invocation->argument_count == 1) {
        ai_tensor_object *right;
        if (get_tensor(context, invocation->arguments[0], &right, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        return binary_result(context, object->request, invocation->member_id, object, right, result, error);
    }
    if (invocation->member_id == TENSOR_TO_ARRAY && !invocation->argument_count)
        return tensor_to_array(context, object, result, error);
    if (invocation->member_id == TENSOR_TO_BYTES && !invocation->argument_count && object->tensor) {
        size_t size = (size_t)oasp_ai_tensor_bytes(object->tensor);
        void *data = size ? malloc(size) : NULL;
        oasp_ai_error core_error = {0};
        if ((size && !data) || oasp_ai_tensor_copy_to_host(object->tensor, data, size, &core_error)) {
            free(data);
            return core_fail(error, &core_error);
        }
        result->value = values->new_bytes(context, data, size);
        free(data);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->member_id == TENSOR_TO_DEVICE && invocation->argument_count == 1 && object->tensor) {
        char device[64];
        if (get_string(context, invocation->arguments[0], device, sizeof(device)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ToDevice requires a device name");
        oasp_ai_error migration_error = {0};
        oasp_ai_tensor *copy = oasp_ai_tensor_to_device(object->tensor, device, &migration_error);
        return copy ? new_tensor_object(context, object->request, copy, NULL, 0, result, error) : core_fail(error, &migration_error);
    }
    if (invocation->member_id == TENSOR_ITEM && object->tensor) {
        uint64_t flat = 0;
        if (!invocation->argument_count && object->tensor->element_count == 1)
            flat = 0;
        else if (invocation->argument_count == object->tensor->desc.rank)
            for (size_t axis = 0; axis < invocation->argument_count; axis++) {
                int64_t index;
                if (values->get_i64(context, invocation->arguments[axis], &index) != OASP_EXT_OK || index < 0 ||
                    index >= object->tensor->desc.shape[axis])
                    return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "Tensor index is out of range");
                flat = flat * (uint64_t)object->tensor->desc.shape[axis] + (uint64_t)index;
            }
        else
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Item requires one index per dimension");
        result->value = values->new_f64(context, oasp_ai_tensor_read(object->tensor, flat));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    oasp_ai_error core_error = {0};
    oasp_ai_tensor *tensor = NULL;
    if (invocation->member_id == TENSOR_RESHAPE && invocation->argument_count == 1) {
        int64_t shape[OASP_AI_MAX_RANK];
        uint32_t rank;
        if (read_shape(context, invocation->arguments[0], shape, &rank, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        tensor = oasp_ai_tensor_reshape(object->tensor, shape, rank, &core_error);
    } else if (invocation->member_id == TENSOR_SLICE && invocation->argument_count >= 3) {
        int64_t axis;
        int64_t start;
        int64_t stop;
        int64_t step = 1;
        if (values->get_i64(context, invocation->arguments[0], &axis) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[1], &start) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[2], &stop) != OASP_EXT_OK ||
            (invocation->argument_count > 3 && values->get_i64(context, invocation->arguments[3], &step) != OASP_EXT_OK) || axis < 0 ||
            axis > UINT32_MAX)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Slice requires axis, start, stop, and optional positive step");
        tensor = oasp_ai_tensor_slice(object->tensor, (uint32_t)axis, start, stop, step, &core_error);
    } else if (invocation->member_id == TENSOR_TRANSPOSE && invocation->argument_count == 2) {
        int64_t first;
        int64_t second;
        if (values->get_i64(context, invocation->arguments[0], &first) != OASP_EXT_OK ||
            values->get_i64(context, invocation->arguments[1], &second) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Transpose requires two axes");
        tensor = oasp_ai_tensor_transpose(object->tensor, (uint32_t)first, (uint32_t)second, &core_error);
    } else if (invocation->member_id == TENSOR_CONTIGUOUS)
        tensor = oasp_ai_tensor_contiguous(object->tensor, &core_error);
    else if (invocation->member_id == TENSOR_CLONE)
        tensor = oasp_ai_tensor_clone(object->tensor, &core_error);
    else if (invocation->member_id == TENSOR_DETACH)
        tensor = oasp_ai_tensor_detach(object->tensor, &core_error);
    else if (invocation->member_id == TENSOR_TO_DTYPE && invocation->argument_count == 1) {
        char name[32];
        oasp_ai_dtype dtype;
        if (get_string(context, invocation->arguments[0], name, sizeof(name)) || oasp_ai_dtype_parse(name, &dtype))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "unsupported dtype");
        tensor = oasp_ai_tensor_cast(object->tensor, dtype, &core_error);
    } else if (invocation->member_id == TENSOR_BACKWARD) {
        oasp_ai_tensor *gradient = NULL;
        ai_tensor_object *gradient_object = NULL;
        if (invocation->argument_count && get_tensor(context, invocation->arguments[0], &gradient_object, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        if (gradient_object)
            gradient = gradient_object->tensor;
        if (oasp_ai_tensor_backward(object->tensor, gradient, 0, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    } else if (invocation->member_id == TENSOR_ZERO_GRAD) {
        if (oasp_ai_tensor_zero_grad(object->tensor, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (!tensor)
        return core_error.message ? core_fail(error, &core_error) : fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Tensor method arguments");
    return new_tensor_object(context, object->request, tensor, NULL, 0, result, error);
}

static uint32_t symbolic_constant(oasp_ai_graph *graph, oasp_ai_tensor *tensor, oasp_ai_error *error) {
    char name[64];
    snprintf(name, sizeof(name), "parameter.%zu", graph->value_count + 1);
    return oasp_ai_graph_constant(graph, name, tensor, error);
}

static uint32_t symbolic_module_forward(oasp_ai_module *module, oasp_ai_graph *graph, uint32_t input, oasp_ai_error *error) {
    if (module->kind == OASP_AI_MODULE_SEQUENTIAL) {
        uint32_t current = input;
        for (size_t index = 0; index < module->child_count && current; index++)
            current = symbolic_module_forward(module->children[index].module, graph, current, error);
        return current;
    }
    if (module->kind == OASP_AI_MODULE_LINEAR) {
        uint32_t weight = symbolic_constant(graph, module->parameters[0].tensor, error);
        uint32_t inputs[] = {input, weight};
        uint32_t output = weight ? oasp_ai_graph_operation(graph, OASP_AI_OP_MATMUL, inputs, 2, NULL, 0, error) : 0;
        if (output && module->parameter_count > 1) {
            uint32_t bias = symbolic_constant(graph, module->parameters[1].tensor, error);
            uint32_t add_inputs[] = {output, bias};
            output = bias ? oasp_ai_graph_operation(graph, OASP_AI_OP_ADD, add_inputs, 2, NULL, 0, error) : 0;
        }
        return output;
    }
    if (module->kind == OASP_AI_MODULE_RELU)
        return oasp_ai_graph_operation(graph, OASP_AI_OP_RELU, &input, 1, NULL, 0, error);
    if (module->kind == OASP_AI_MODULE_DROPOUT)
        return oasp_ai_graph_operation(graph, OASP_AI_OP_CONTIGUOUS, &input, 1, NULL, 0, error);
    if (module->kind == OASP_AI_MODULE_LAYER_NORM || module->kind == OASP_AI_MODULE_RMS_NORM) {
        uint32_t inputs[3] = {input, symbolic_constant(graph, module->parameters[0].tensor, error), 0};
        size_t count = 2;
        if (module->kind == OASP_AI_MODULE_LAYER_NORM) {
            inputs[2] = symbolic_constant(graph, module->parameters[1].tensor, error);
            count = 3;
        }
        if (!inputs[1] || (count == 3 && !inputs[2]))
            return 0;
        return oasp_ai_graph_operation(graph,
                                       module->kind == OASP_AI_MODULE_LAYER_NORM ? OASP_AI_OP_LAYER_NORM : OASP_AI_OP_RMS_NORM, inputs, count,
                                       &module->argument, sizeof(module->argument), error);
    }
    if (module->kind == OASP_AI_MODULE_EMBEDDING) {
        uint32_t axis = 0;
        uint32_t inputs[] = {symbolic_constant(graph, module->parameters[0].tensor, error), input};
        return inputs[0] ? oasp_ai_graph_operation(graph, OASP_AI_OP_GATHER, inputs, 2, &axis, sizeof(axis), error) : 0;
    }
    oasp_ai_set_error(error, 5, "Trace", "module has no traceable native Forward", "Use a built-in layer or express Forward with OpenASP.AI operators.");
    return 0;
}

static int module_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_module_object *object = instance;
    oasp_ai_error core_error = {0};
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == MODULE_PARAMETER_COUNT) {
        result->value = values->new_i64(context, (int64_t)oasp_ai_module_parameter_count(object->module));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Module member is not callable");
    if ((invocation->member_id == MODULE_REGISTER_PARAMETER || invocation->member_id == MODULE_REGISTER_BUFFER) &&
        invocation->argument_count >= 2) {
        char name[256];
        ai_tensor_object *tensor;
        if (get_string(context, invocation->arguments[0], name, sizeof(name)) ||
            get_tensor(context, invocation->arguments[1], &tensor, error) != OASP_EXT_OK || !tensor->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "RegisterParameter/Buffer requires a name and Tensor");
        int status = invocation->member_id == MODULE_REGISTER_PARAMETER
                         ? oasp_ai_module_register_parameter(object->module, name, tensor->tensor, &core_error)
                         : oasp_ai_module_register_buffer(object->module, name, tensor->tensor, 1, &core_error);
        if (status)
            return core_fail(error, &core_error);
    } else if (invocation->member_id == MODULE_REGISTER_MODULE && invocation->argument_count == 2) {
        char name[256];
        ai_module_object *child;
        if (get_string(context, invocation->arguments[0], name, sizeof(name)) ||
            get_native(context, invocation->arguments[1], CLASS_MODULE, (void **)&child, error) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "RegisterModule requires a name and Module");
        if (oasp_ai_module_register_module(object->module, name, child->module, &core_error))
            return core_fail(error, &core_error);
    } else if (invocation->member_id == MODULE_PARAMETER && invocation->argument_count == 1) {
        int64_t index;
        if (values->get_i64(context, invocation->arguments[0], &index) != OASP_EXT_OK || index < 0)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Parameter requires a zero-based index");
        oasp_ai_tensor *tensor = oasp_ai_module_parameter(object->module, (size_t)index, NULL);
        return tensor ? new_tensor_object(context, object->request, tensor, NULL, 0, result, error)
                      : fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "parameter index is out of range");
    } else if (invocation->member_id == MODULE_ADD && invocation->argument_count == 1) {
        ai_module_object *child;
        if (get_native(context, invocation->arguments[0], CLASS_MODULE, (void **)&child, error) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Sequential Add requires a Module");
        if (oasp_ai_module_add(object->module, child->module, &core_error))
            return core_fail(error, &core_error);
    } else if (invocation->member_id == MODULE_FORWARD && invocation->argument_count == 1) {
        ai_tensor_object *input;
        if (get_tensor(context, invocation->arguments[0], &input, error) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Forward requires a Tensor");
        if (input->graph) {
            uint32_t output = symbolic_module_forward(object->module, input->graph, input->value_id, &core_error);
            return output ? new_tensor_object(context, object->request, NULL, input->graph, output, result, error) : core_fail(error, &core_error);
        }
        oasp_ai_tensor *output = oasp_ai_module_forward(object->module, input->tensor, &core_error);
        return output ? new_tensor_object(context, object->request, output, NULL, 0, result, error) : core_fail(error, &core_error);
    } else if ((invocation->member_id == MODULE_PARAMETERS || invocation->member_id == MODULE_NAMED_PARAMETERS ||
                invocation->member_id == MODULE_BUFFERS) &&
               !invocation->argument_count) {
        oasp_ai_state_dict *state = oasp_ai_module_state_dict(object->module, &core_error);
        oasp_ai_state_kind kind = invocation->member_id == MODULE_BUFFERS ? OASP_AI_STATE_BUFFER : OASP_AI_STATE_PARAMETER;
        return state ? new_parameter_collection_object(context, object->request, state, kind, result, error)
                     : core_fail(error, &core_error);
    } else if (invocation->member_id == MODULE_STATE_DICT && !invocation->argument_count) {
        oasp_ai_state_dict *state = oasp_ai_module_state_dict(object->module, &core_error);
        return state ? new_state_dict_object(context, object->request, state, result, error) : core_fail(error, &core_error);
    } else if (invocation->member_id == MODULE_LOAD_STATE_DICT && invocation->argument_count >= 1) {
        ai_state_dict_object *state;
        int strict = 1;
        if (get_native(context, invocation->arguments[0], CLASS_STATE_DICT, (void **)&state, error) != OASP_EXT_OK ||
            (invocation->argument_count > 1 && values->get_bool(context, invocation->arguments[1], &strict) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "LoadStateDict requires a StateDict and optional strict Boolean");
        oasp_ai_state_diagnostics diagnostics = {0};
        int status = oasp_ai_module_load_state_dict(object->module, state->state, strict, &diagnostics, &core_error);
        int result_status = diagnostics_result(context, &diagnostics, result);
        oasp_ai_state_diagnostics_free(&diagnostics);
        if (result_status != OASP_EXT_OK)
            return result_status;
        return status && !core_error.message ? core_fail(error, &core_error) : OASP_EXT_OK;
    } else if ((invocation->member_id == MODULE_TO_DEVICE || invocation->member_id == MODULE_TO_DTYPE) &&
               invocation->argument_count == 1) {
        char name[64];
        if (get_string(context, invocation->arguments[0], name, sizeof(name)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ToDevice/ToDType requires a string");
        int status;
        if (invocation->member_id == MODULE_TO_DEVICE)
            status = oasp_ai_module_to_device(object->module, name, &core_error);
        else {
            oasp_ai_dtype dtype;
            status = oasp_ai_dtype_parse(name, &dtype) || oasp_ai_module_to_dtype(object->module, dtype, &core_error);
        }
        if (status)
            return core_fail(error, &core_error);
    } else if (invocation->member_id == MODULE_TRAIN)
        oasp_ai_module_train(object->module, 1);
    else if (invocation->member_id == MODULE_EVAL)
        oasp_ai_module_train(object->module, 0);
    else if ((invocation->member_id == MODULE_SAVE_WEIGHTS || invocation->member_id == MODULE_LOAD_WEIGHTS) && invocation->argument_count >= 1) {
        char path[4096];
        if (get_string(context, invocation->arguments[0], path, sizeof(path)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "weights operation requires a path");
        int status = invocation->member_id == MODULE_SAVE_WEIGHTS ? oasp_ai_module_save_weights(object->module, path, &core_error)
                                                                  : oasp_ai_module_load_weights(object->module, path, 1, &core_error);
        if (status)
            return core_fail(error, &core_error);
    } else if ((invocation->member_id == MODULE_SAVE_SAFETENSORS || invocation->member_id == MODULE_LOAD_SAFETENSORS) &&
               invocation->argument_count >= 1) {
        char path[4096];
        int strict = 1;
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) ||
            (invocation->argument_count > 1 && values->get_bool(context, invocation->arguments[1], &strict) != OASP_EXT_OK))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "SafeTensors operation requires a path and optional strict Boolean");
        if (invocation->member_id == MODULE_SAVE_SAFETENSORS) {
            if (oasp_ai_module_save_safetensors(object->module, path, &core_error))
                return core_fail(error, &core_error);
        } else {
            oasp_ai_state_diagnostics diagnostics = {0};
            int status = oasp_ai_module_load_safetensors(object->module, path, strict, &diagnostics, &core_error);
            int result_status = diagnostics_result(context, &diagnostics, result);
            oasp_ai_state_diagnostics_free(&diagnostics);
            if (result_status != OASP_EXT_OK)
                return result_status;
            return status && !core_error.message ? core_fail(error, &core_error) : OASP_EXT_OK;
        }
    } else
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Module method arguments");
    result->value = values->new_empty(context);
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int optimizer_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_optimizer_object *object = instance;
    oasp_ai_error core_error = {0};
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Optimizer member is not callable");
    int status;
    if (invocation->member_id == OPTIMIZER_ZERO_GRAD)
        status = oasp_ai_optimizer_zero_grad(object->optimizer, &core_error);
    else if (invocation->member_id == OPTIMIZER_STEP)
        status = oasp_ai_optimizer_step(object->optimizer, &core_error);
    else if (invocation->member_id == OPTIMIZER_CLIP_GRAD_NORM && invocation->argument_count == 1) {
        double maximum;
        if (values->get_f64(context, invocation->arguments[0], &maximum) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ClipGradNorm requires a numeric maximum");
        status = oasp_ai_optimizer_clip_grad_norm(object->optimizer, maximum, &core_error);
    } else
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Optimizer method arguments");
    if (status)
        return core_fail(error, &core_error);
    result->value = values->new_empty(context);
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int graph_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_graph_object *object = instance;
    oasp_ai_error core_error = {0};
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Graph member is not callable");
    if (invocation->member_id == GRAPH_INPUT && invocation->argument_count >= 2) {
        char name[256];
        char dtype_name[32] = "float32";
        int64_t shape[OASP_AI_MAX_RANK];
        uint32_t rank;
        if (get_string(context, invocation->arguments[0], name, sizeof(name)) ||
            read_shape_mode(context, invocation->arguments[1], shape, &rank, 1, error) != OASP_EXT_OK ||
            (invocation->argument_count > 2 && get_string(context, invocation->arguments[2], dtype_name, sizeof(dtype_name))))
            return OASP_EXT_ERROR;
        oasp_ai_tensor_desc desc = {.rank = rank};
        memcpy(desc.shape, shape, rank * sizeof(*shape));
        if (oasp_ai_dtype_parse(dtype_name, &desc.dtype))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Graph Input dtype is unsupported");
        uint32_t value_id = oasp_ai_graph_input(object->graph, name, &desc, &core_error);
        return value_id ? new_tensor_object(context, object->request, NULL, object->graph, value_id, result, error) : core_fail(error, &core_error);
    }
    if (invocation->member_id == GRAPH_OPERATION && invocation->argument_count >= 2) {
        int64_t operation;
        ai_tensor_object *input;
        if (values->get_i64(context, invocation->arguments[0], &operation) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[1], &input, error) != OASP_EXT_OK || input->graph != object->graph)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Graph Operation requires an operator ID and symbolic Tensor");
        uint32_t inputs[8];
        size_t input_count = invocation->argument_count - 1;
        if (input_count > 8)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Graph Operation has too many inputs");
        for (size_t index = 0; index < input_count; index++) {
            if (get_tensor(context, invocation->arguments[index + 1], &input, error) != OASP_EXT_OK || input->graph != object->graph)
                return OASP_EXT_ERROR;
            inputs[index] = input->value_id;
        }
        uint32_t value_id = oasp_ai_graph_operation(object->graph, (oasp_ai_operator)operation, inputs, input_count, NULL, 0, &core_error);
        return value_id ? new_tensor_object(context, object->request, NULL, object->graph, value_id, result, error) : core_fail(error, &core_error);
    }
    if ((invocation->member_id == GRAPH_BUILD || invocation->member_id == GRAPH_SAVE) && invocation->argument_count >= 1) {
        ai_tensor_object *output;
        if (get_tensor(context, invocation->arguments[0], &output, error) != OASP_EXT_OK || output->graph != object->graph)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Graph output must be a symbolic Tensor from this Graph");
        if (oasp_ai_graph_set_outputs(object->graph, &output->value_id, 1, &core_error))
            return core_fail(error, &core_error);
        if (invocation->member_id == GRAPH_SAVE) {
            char path[4096];
            if (invocation->argument_count < 2 || get_string(context, invocation->arguments[1], path, sizeof(path)))
                return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Graph Save requires output and path");
            if (oasp_ai_graph_save(object->graph, path, &core_error))
                return core_fail(error, &core_error);
            result->value = values->new_empty(context);
            return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
        }
        ai_model_object *model = calloc(1, sizeof(*model));
        if (model)
            model->plan = oasp_ai_graph_compile(object->graph, object->device, &core_error);
        if (!model || !model->plan) {
            free(model);
            return core_fail(error, &core_error);
        }
        model->request = object->request;
        request_retain(model->request);
        model->graph = object->graph;
        atomic_fetch_add_explicit(&model->graph->references, 1, memory_order_relaxed);
        if (objects->new_owned(context, CLASS_MODEL, model, &result->value, error) == OASP_EXT_OK)
            return OASP_EXT_OK;
        model_destroy(context, model);
        return OASP_EXT_ERROR;
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Graph method arguments");
}

static int model_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_model_object *object = instance;
    oasp_ai_error core_error = {0};
    if (invocation->operation != OASP_EXT_OP_CALL)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Model member is not callable");
    if (invocation->member_id == MODEL_SAVE && invocation->argument_count == 1) {
        char path[4096];
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) || oasp_ai_graph_save(object->graph, path, &core_error))
            return core_error.message ? core_fail(error, &core_error) : fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Model Save requires a path");
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->member_id == MODEL_EXPORT_ONNX && invocation->argument_count == 1) {
        char path[4096];
        if (get_string(context, invocation->arguments[0], path, sizeof(path)) ||
            oasp_ai_graph_export_onnx(object->graph, path, &core_error))
            return core_error.message ? core_fail(error, &core_error)
                                      : fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ExportONNX requires a path");
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if ((invocation->member_id == MODEL_FORWARD || invocation->member_id == MODEL_FORWARD_ASYNC) && invocation->argument_count) {
        oasp_ai_tensor **inputs = calloc(invocation->argument_count, sizeof(*inputs));
        if (!inputs)
            return OASP_EXT_ERROR;
        for (size_t index = 0; index < invocation->argument_count; index++) {
            ai_tensor_object *input;
            if (get_tensor(context, invocation->arguments[index], &input, error) != OASP_EXT_OK || !input->tensor) {
                free(inputs);
                return OASP_EXT_ERROR;
            }
            inputs[index] = input->tensor;
        }
        if (invocation->member_id == MODEL_FORWARD_ASYNC) {
            oasp_ai_event_handle *event = oasp_ai_plan_execute_async(object->plan, inputs, invocation->argument_count, &core_error);
            free(inputs);
            ai_event_object *event_object = event ? calloc(1, sizeof(*event_object)) : NULL;
            if (!event_object) {
                oasp_ai_event_release(event);
                return event ? fail(error, 7, OASP_EXT_ERROR_RESOURCE, "unable to allocate Event object")
                             : core_fail(error, &core_error);
            }
            event_object->request = object->request;
            event_object->event = event;
            request_retain(event_object->request);
            if (objects->new_owned(context, CLASS_EVENT, event_object, &result->value, error) == OASP_EXT_OK)
                return OASP_EXT_OK;
            event_destroy(context, event_object);
            return OASP_EXT_ERROR;
        }
        oasp_ai_tensor **outputs = NULL;
        size_t output_count = 0;
        int status = oasp_ai_plan_execute(object->plan, inputs, invocation->argument_count, &outputs, &output_count, &core_error);
        free(inputs);
        if (status || !output_count) {
            free(outputs);
            return core_fail(error, &core_error);
        }
        oasp_ai_tensor *first = outputs[0];
        for (size_t index = 1; index < output_count; index++)
            oasp_ai_tensor_release(outputs[index]);
        free(outputs);
        return new_tensor_object(context, object->request, first, NULL, 0, result, error);
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Model method arguments");
}

static int cache_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_cache_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == CACHE_LENGTH) {
        result->value = values->new_i64(context, (int64_t)oasp_ai_kv_cache_length(object->cache));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == CACHE_APPEND && invocation->argument_count == 3) {
        int64_t layer;
        ai_tensor_object *keys;
        ai_tensor_object *cache_values;
        if (values->get_i64(context, invocation->arguments[0], &layer) != OASP_EXT_OK || layer < 0 || layer > UINT32_MAX ||
            get_tensor(context, invocation->arguments[1], &keys, error) != OASP_EXT_OK ||
            get_tensor(context, invocation->arguments[2], &cache_values, error) != OASP_EXT_OK || !keys->tensor || !cache_values->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "KVCache.Append requires layer, key Tensor, and value Tensor");
        oasp_ai_error core_error = {0};
        if (oasp_ai_kv_cache_append(object->cache, (uint32_t)layer, keys->tensor, cache_values->tensor, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid KVCache method arguments");
}

static int dataset_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_dataset_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == DATASET_SIZE) {
        result->value = values->new_i64(context, (int64_t)oasp_ai_dataset_size(object->dataset));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation != OASP_EXT_OP_CALL || invocation->member_id != DATASET_DATALOADER || !invocation->argument_count)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "DataLoader requires a batch size and optional shuffle/worker settings");
    int64_t batch_size;
    int64_t workers = 0;
    int64_t prefetch = 2;
    int shuffle = 0;
    int drop_last = 0;
    int pinned = 0;
    if (values->get_i64(context, invocation->arguments[0], &batch_size) != OASP_EXT_OK || batch_size <= 0 ||
        (invocation->argument_count > 1 && values->get_bool(context, invocation->arguments[1], &shuffle) != OASP_EXT_OK) ||
        (invocation->argument_count > 2 && values->get_i64(context, invocation->arguments[2], &workers) != OASP_EXT_OK) ||
        (invocation->argument_count > 3 && values->get_i64(context, invocation->arguments[3], &prefetch) != OASP_EXT_OK) ||
        (invocation->argument_count > 4 && values->get_bool(context, invocation->arguments[4], &drop_last) != OASP_EXT_OK) ||
        (invocation->argument_count > 5 && values->get_bool(context, invocation->arguments[5], &pinned) != OASP_EXT_OK) || workers < 0 ||
        prefetch < 0 || workers > UINT32_MAX || prefetch > UINT32_MAX)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "DataLoader settings are invalid");
    oasp_ai_dataloader_options options = {
        .batch_size = (uint64_t)batch_size,
        .seed = object->request->core->options.seed,
        .workers = (uint32_t)workers,
        .prefetch_batches = (uint32_t)prefetch,
        .shuffle = shuffle,
        .drop_last = drop_last,
        .pinned_memory = pinned,
    };
    oasp_ai_error core_error = {0};
    ai_dataloader_object *loader = calloc(1, sizeof(*loader));
    if (loader)
        loader->loader = oasp_ai_dataloader_create(object->dataset, &options, &core_error);
    if (!loader || !loader->loader) {
        free(loader);
        return core_fail(error, &core_error);
    }
    loader->request = object->request;
    request_retain(loader->request);
    if (objects->new_owned(context, CLASS_DATALOADER, loader, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    dataloader_destroy(context, loader);
    return OASP_EXT_ERROR;
}

static int dataloader_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_dataloader_object *object = instance;
    if (invocation->operation != OASP_EXT_OP_CALL || invocation->member_id != DATALOADER_ITERATOR || invocation->argument_count > 1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Iterator accepts an optional epoch");
    int64_t epoch = 0;
    if (invocation->argument_count && (values->get_i64(context, invocation->arguments[0], &epoch) != OASP_EXT_OK || epoch < 0))
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Iterator epoch must be non-negative");
    oasp_ai_error core_error = {0};
    ai_data_iterator_object *iterator = calloc(1, sizeof(*iterator));
    if (iterator)
        iterator->iterator = oasp_ai_dataloader_iterator(object->loader, (uint64_t)epoch, &core_error);
    if (!iterator || !iterator->iterator) {
        free(iterator);
        return core_fail(error, &core_error);
    }
    iterator->request = object->request;
    request_retain(iterator->request);
    if (objects->new_owned(context, CLASS_DATA_ITERATOR, iterator, &result->value, error) == OASP_EXT_OK)
        return OASP_EXT_OK;
    data_iterator_destroy(context, iterator);
    return OASP_EXT_ERROR;
}

static int data_iterator_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_data_iterator_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == ITERATOR_CURSOR)
        result->value = values->new_i64(context, (int64_t)oasp_ai_data_iterator_cursor(object->iterator));
    else if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == ITERATOR_CURRENT) {
        if (!object->current)
            result->value = values->new_empty(context);
        else {
            ai_batch_object *batch = calloc(1, sizeof(*batch));
            if (!batch)
                return OASP_EXT_ERROR;
            batch->request = object->request;
            batch->batch = object->current;
            request_retain(batch->request);
            oasp_ai_batch_retain(batch->batch);
            if (objects->new_owned(context, CLASS_BATCH, batch, &result->value, error) != OASP_EXT_OK) {
                batch_destroy(context, batch);
                return OASP_EXT_ERROR;
            }
        }
    } else if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == ITERATOR_MOVE_NEXT && !invocation->argument_count) {
        oasp_ai_batch_release(object->current);
        object->current = NULL;
        oasp_ai_error core_error = {0};
        int status = oasp_ai_data_iterator_next(object->iterator, &object->current, &core_error);
        if (status < 0)
            return core_fail(error, &core_error);
        result->value = values->new_bool(context, status > 0);
    } else
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid DataIterator member or arguments");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int batch_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_batch_object *object = instance;
    if (invocation->operation != OASP_EXT_OP_GET)
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "Batch properties are read-only");
    if (invocation->member_id == BATCH_SIZE)
        result->value = values->new_i64(context, (int64_t)oasp_ai_batch_size(object->batch));
    else if (invocation->member_id == BATCH_INPUTS || invocation->member_id == BATCH_LABELS) {
        oasp_ai_tensor *tensor = invocation->member_id == BATCH_INPUTS ? oasp_ai_batch_inputs(object->batch) : oasp_ai_batch_labels(object->batch);
        return tensor ? new_tensor_object(context, object->request, tensor, NULL, 0, result, error) : OASP_EXT_ERROR;
    } else
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown Batch property");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int tokenizer_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_tokenizer_object *object = instance;
    if (invocation->operation != OASP_EXT_OP_CALL || invocation->argument_count != 1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Tokenizer Encode/Decode requires one argument");
    oasp_ai_error core_error = {0};
    if (invocation->member_id == TOKENIZER_ENCODE) {
        oasp_ext_bytes text;
        int64_t *tokens = NULL;
        size_t count = 0;
        if (values->kind(context, invocation->arguments[0]) != OASP_EXT_VALUE_STRING ||
            values->get_data(context, invocation->arguments[0], &text) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Encode requires a string");
        if (oasp_ai_tokenizer_encode(object->tokenizer, text.data, text.length, &tokens, &count, &core_error))
            return core_fail(error, &core_error);
        int status = count > UINT32_MAX ? OASP_EXT_ERROR : shape_value(context, tokens, (uint32_t)count, &result->value);
        free(tokens);
        return status;
    }
    if (invocation->member_id == TOKENIZER_DECODE) {
        int64_t *tokens = NULL;
        size_t count = 0;
        char *text = NULL;
        size_t text_size = 0;
        if (read_i64_array(context, invocation->arguments[0], &tokens, &count, error) != OASP_EXT_OK)
            return OASP_EXT_ERROR;
        int status = oasp_ai_tokenizer_decode(object->tokenizer, tokens, count, &text, &text_size, &core_error);
        free(tokens);
        if (status)
            return core_fail(error, &core_error);
        result->value = values->new_string(context, text, text_size);
        free(text);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown Tokenizer method");
}

static int scheduler_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_scheduler_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == SCHEDULER_CURRENT_STEP)
        result->value = values->new_i64(context, (int64_t)oasp_ai_scheduler_current_step(object->scheduler));
    else if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == SCHEDULER_STEP && !invocation->argument_count) {
        oasp_ai_error core_error = {0};
        if (oasp_ai_scheduler_step(object->scheduler, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
    } else
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Scheduler member or arguments");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int loss_scaler_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_loss_scaler_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == LOSS_SCALER_SCALE)
        result->value = values->new_f64(context, oasp_ai_loss_scaler_scale(object->scaler));
    else if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == LOSS_SCALER_SCALE_LOSS &&
             invocation->argument_count == 1) {
        ai_tensor_object *loss;
        if (get_tensor(context, invocation->arguments[0], &loss, error) != OASP_EXT_OK || !loss->tensor)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ScaleLoss requires a concrete Tensor");
        oasp_ai_error core_error = {0};
        oasp_ai_tensor *scaled = oasp_ai_loss_scaler_scale_loss(object->scaler, loss->tensor, &core_error);
        return scaled ? new_tensor_object(context, object->request, scaled, NULL, 0, result, error) : core_fail(error, &core_error);
    } else if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == LOSS_SCALER_STEP &&
               invocation->argument_count == 1) {
        ai_optimizer_object *optimizer;
        if (get_native(context, invocation->arguments[0], CLASS_OPTIMIZER, (void **)&optimizer, error) != OASP_EXT_OK)
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "LossScaler.Step requires an Optimizer");
        oasp_ai_error core_error = {0};
        if (oasp_ai_loss_scaler_step(object->scaler, optimizer->optimizer, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
    } else
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid LossScaler member or arguments");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int state_dict_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_state_dict_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == STATE_DICT_COUNT) {
        result->value = values->new_i64(context, (int64_t)oasp_ai_state_dict_count(object->state));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation != OASP_EXT_OP_CALL || invocation->argument_count != 1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "StateDict member requires a zero-based index");
    int64_t index;
    if (values->get_i64(context, invocation->arguments[0], &index) != OASP_EXT_OK || index < 0 ||
        (uint64_t)index >= oasp_ai_state_dict_count(object->state))
        return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "StateDict index is out of range");
    const char *name = NULL;
    oasp_ai_state_kind kind;
    oasp_ai_tensor *tensor = oasp_ai_state_dict_tensor(object->state, (size_t)index, &name, &kind);
    if (!tensor)
        return OASP_EXT_ERROR;
    if (invocation->member_id == STATE_DICT_NAME)
        result->value = values->new_string(context, name, strlen(name));
    else if (invocation->member_id == STATE_DICT_KIND) {
        const char *kind_name = kind == OASP_AI_STATE_PARAMETER ? "parameter" : "buffer";
        result->value = values->new_string(context, kind_name, strlen(kind_name));
    } else if (invocation->member_id == STATE_DICT_TENSOR)
        return new_tensor_object(context, object->request, tensor, NULL, 0, result, error);
    else {
        oasp_ai_tensor_release(tensor);
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "unknown StateDict member");
    }
    oasp_ai_tensor_release(tensor);
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int event_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_event_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == EVENT_READY) {
        result->value = values->new_bool(context, oasp_ai_event_ready(object->event));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    oasp_ai_error core_error = {0};
    if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == EVENT_WAIT) {
        if (oasp_ai_event_wait(object->event, &core_error))
            return core_fail(error, &core_error);
        result->value = values->new_empty(context);
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    oasp_ai_tensor **outputs = NULL;
    size_t output_count = 0;
    if (oasp_ai_event_outputs(object->event, &outputs, &output_count, &core_error))
        return core_fail(error, &core_error);
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == EVENT_OUTPUT_COUNT)
        result->value = values->new_i64(context, (int64_t)output_count);
    else if (invocation->operation == OASP_EXT_OP_CALL && invocation->member_id == EVENT_OUTPUT &&
             invocation->argument_count == 1) {
        int64_t index;
        if (values->get_i64(context, invocation->arguments[0], &index) != OASP_EXT_OK || index < 0 ||
            (uint64_t)index >= output_count) {
            for (size_t item = 0; item < output_count; item++)
                oasp_ai_tensor_release(outputs[item]);
            free(outputs);
            return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "Event output index is out of range");
        }
        oasp_ai_tensor *selected = outputs[index];
        for (size_t item = 0; item < output_count; item++)
            if ((int64_t)item != index)
                oasp_ai_tensor_release(outputs[item]);
        free(outputs);
        return new_tensor_object(context, object->request, selected, NULL, 0, result, error);
    } else {
        for (size_t item = 0; item < output_count; item++)
            oasp_ai_tensor_release(outputs[item]);
        free(outputs);
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "invalid Event member or arguments");
    }
    for (size_t item = 0; item < output_count; item++)
        oasp_ai_tensor_release(outputs[item]);
    free(outputs);
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static size_t parameter_collection_count(const ai_parameter_collection_object *object) {
    size_t count = 0;
    for (size_t index = 0; index < object->state->count; index++)
        count += object->state->entries[index].kind == object->kind;
    return count;
}

static oasp_ai_state_entry *parameter_collection_entry(ai_parameter_collection_object *object, size_t position) {
    for (size_t index = 0; index < object->state->count; index++) {
        if (object->state->entries[index].kind != object->kind)
            continue;
        if (!position--)
            return &object->state->entries[index];
    }
    return NULL;
}

static int parameter_collection_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_parameter_collection_object *object = instance;
    if (invocation->operation == OASP_EXT_OP_GET && invocation->member_id == COLLECTION_COUNT) {
        result->value = values->new_i64(context, (int64_t)parameter_collection_count(object));
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation != OASP_EXT_OP_CALL || invocation->argument_count != 1)
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "ParameterCollection member requires a zero-based index");
    int64_t index;
    if (values->get_i64(context, invocation->arguments[0], &index) != OASP_EXT_OK || index < 0)
        return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "ParameterCollection index is out of range");
    oasp_ai_state_entry *entry = parameter_collection_entry(object, (size_t)index);
    if (!entry)
        return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "ParameterCollection index is out of range");
    if (invocation->member_id == COLLECTION_NAME)
        result->value = values->new_string(context, entry->name, strlen(entry->name));
    else if (invocation->member_id == COLLECTION_KIND) {
        const char *kind = entry->kind == OASP_AI_STATE_PARAMETER ? "parameter" : "buffer";
        result->value = values->new_string(context, kind, strlen(kind));
    } else if (invocation->member_id == COLLECTION_TENSOR) {
        oasp_ai_tensor_retain(entry->tensor);
        return new_tensor_object(context, object->request, entry->tensor, NULL, 0, result, error);
    } else
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown ParameterCollection member");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int checkpoint_state_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_checkpoint_state_object *object = instance;
    uint32_t member = invocation->member_id;
    if (invocation->operation == OASP_EXT_OP_DEFAULT_GET && invocation->argument_count == 1) {
        char key[32];
        if (get_string(context, invocation->arguments[0], key, sizeof(key)))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "CheckpointState key must be a string");
        if (!strcasecmp(key, "epoch"))
            member = CHECKPOINT_EPOCH;
        else if (!strcasecmp(key, "step") || !strcasecmp(key, "globalstep"))
            member = CHECKPOINT_GLOBAL_STEP;
        else if (!strcasecmp(key, "metadata"))
            member = CHECKPOINT_METADATA;
        else
            return fail(error, 9, OASP_EXT_ERROR_ARGUMENT, "CheckpointState key is not available");
    } else if (invocation->operation != OASP_EXT_OP_GET) {
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "CheckpointState properties are read-only");
    }
    if (member == CHECKPOINT_METADATA) {
        if (oasp_ai_metadata_is_encoded(object->state.metadata, object->state.metadata_size))
            result->value = oasp_ai_metadata_decode(context, values, object->state.metadata, object->state.metadata_size, error);
        else
            result->value = values->new_bytes(context, object->state.metadata, object->state.metadata_size);
    }
    else if (member == CHECKPOINT_EPOCH)
        result->value = values->new_i64(context, (int64_t)object->state.epoch);
    else if (member == CHECKPOINT_GLOBAL_STEP)
        result->value = values->new_i64(context, (int64_t)object->state.global_step);
    else
        return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown CheckpointState property");
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static int operator_from_value(oasp_ext_call_context *context, const oasp_ext_value *value, uint32_t *operation) {
    int64_t numeric;
    if (values->get_i64(context, value, &numeric) == OASP_EXT_OK && numeric >= OASP_AI_OP_ZEROS && numeric <= OASP_AI_OP_SOFTMAX) {
        *operation = (uint32_t)numeric;
        return 0;
    }
    char name[64];
    if (get_string(context, value, name, sizeof(name)))
        return -1;
    static const struct {
        const char *name;
        uint32_t operation;
    } names[] = {
        {"zeros", OASP_AI_OP_ZEROS}, {"add", OASP_AI_OP_ADD}, {"relu", OASP_AI_OP_RELU},
        {"matmul", OASP_AI_OP_MATMUL}, {"conv2d", OASP_AI_OP_CONV2D}, {"layernorm", OASP_AI_OP_LAYER_NORM},
        {"rmsnorm", OASP_AI_OP_RMS_NORM}, {"attention", OASP_AI_OP_ATTENTION}, {"embedding", OASP_AI_OP_EMBEDDING},
        {"softmax", OASP_AI_OP_SOFTMAX},
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++)
        if (!strcasecmp(name, names[index].name)) {
            *operation = names[index].operation;
            return 0;
        }
    return -1;
}

static int device_invoke(oasp_ext_call_context *context, void *instance, const oasp_ext_invocation *invocation, oasp_ext_result *result, oasp_ext_error *error) {
    ai_device_object *object = instance;
    oasp_ai_registered_backend *backend = &object->request->core->backends[object->backend_index];
    if (invocation->operation == OASP_EXT_OP_GET) {
        if (invocation->member_id == DEVICE_NAME)
            result->value = values->new_string(context, object->name, strlen(object->name));
        else if (invocation->member_id == DEVICE_BACKEND)
            result->value = values->new_string(context, object->backend, strlen(object->backend));
        else if (invocation->member_id == DEVICE_ORDINAL)
            result->value = values->new_i64(context, object->info.ordinal);
        else if (invocation->member_id == DEVICE_TOTAL_MEMORY)
            result->value = values->new_i64(context, (int64_t)object->info.total_memory);
        else if (invocation->member_id == DEVICE_FREE_MEMORY) {
            oasp_ai_memory_stats stats = {.struct_size = sizeof(stats)};
            oasp_ai_error core_error = {0};
            uint64_t free_memory = object->info.free_memory;
            if (!object->info.ordinal && backend->descriptor->api->memory_stats &&
                !backend->descriptor->api->memory_stats(backend->device, &stats, &core_error) &&
                object->info.total_memory != UINT64_MAX)
                free_memory = stats.reserved_bytes < object->info.total_memory ? object->info.total_memory - stats.reserved_bytes : 0;
            result->value = values->new_i64(context, (int64_t)free_memory);
        } else
            return fail(error, 438, OASP_EXT_ERROR_ARGUMENT, "unknown Device property");
        return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
    }
    if (invocation->operation != OASP_EXT_OP_CALL || invocation->argument_count != 1 ||
        (invocation->member_id != DEVICE_SUPPORTS && invocation->member_id != DEVICE_SUPPORTS_OPERATOR))
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Device capability query requires one dtype or operator");
    oasp_ai_capability_query query = {
        .struct_size = sizeof(query),
        .op_id = OASP_AI_OP_MATMUL,
        .op_version = 1,
        .input_dtype = OASP_AI_DTYPE_F32,
        .output_dtype = OASP_AI_DTYPE_F32,
    };
    if (invocation->member_id == DEVICE_SUPPORTS) {
        char dtype_name[32];
        if (get_string(context, invocation->arguments[0], dtype_name, sizeof(dtype_name)) ||
            oasp_ai_dtype_parse(dtype_name, &query.input_dtype))
            return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "Supports requires a valid dtype name");
        query.output_dtype = query.input_dtype;
    } else if (operator_from_value(context, invocation->arguments[0], &query.op_id))
        return fail(error, 5, OASP_EXT_ERROR_ARGUMENT, "SupportsOperator requires a known operator name or ID");
    oasp_ai_error core_error = {0};
    if (object->info.ordinal || !backend->descriptor->api->query_capabilities ||
        backend->descriptor->api->query_capabilities(backend->device, &query, &core_error))
        result->value = values->new_bool(context, 0);
    else
        result->value = values->new_bool(context, query.supported);
    return result->value ? OASP_EXT_OK : OASP_EXT_ERROR;
}

#define MEMBER(name, id, flags) {sizeof(oasp_extension_member), name, id, flags}

static const oasp_extension_member factory_members[] = {
    MEMBER("Device", AI_DEVICE, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET),
    MEMBER("DType", AI_DTYPE, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET),
    MEMBER("Seed", AI_SEED, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET),
    MEMBER("Deterministic", AI_DETERMINISTIC, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET),
    MEMBER("StrictDevice", AI_STRICT_DEVICE, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET),
    MEMBER("Zeros", AI_ZEROS, OASP_EXT_MEMBER_CALL),
    MEMBER("Ones", AI_ONES, OASP_EXT_MEMBER_CALL),
    MEMBER("Full", AI_FULL, OASP_EXT_MEMBER_CALL),
    MEMBER("RandomUniform", AI_RANDOM_UNIFORM, OASP_EXT_MEMBER_CALL),
    MEMBER("RandomNormal", AI_RANDOM_NORMAL, OASP_EXT_MEMBER_CALL),
    MEMBER("Tensor", AI_TENSOR, OASP_EXT_MEMBER_CALL),
    MEMBER("Add", AI_ADD, OASP_EXT_MEMBER_CALL),
    MEMBER("Sub", AI_SUB, OASP_EXT_MEMBER_CALL),
    MEMBER("Mul", AI_MUL, OASP_EXT_MEMBER_CALL),
    MEMBER("Div", AI_DIV, OASP_EXT_MEMBER_CALL),
    MEMBER("MatMul", AI_MATMUL, OASP_EXT_MEMBER_CALL),
    MEMBER("ReLU", AI_RELU, OASP_EXT_MEMBER_CALL),
    MEMBER("Sigmoid", AI_SIGMOID, OASP_EXT_MEMBER_CALL),
    MEMBER("Tanh", AI_TANH, OASP_EXT_MEMBER_CALL),
    MEMBER("GELU", AI_GELU, OASP_EXT_MEMBER_CALL),
    MEMBER("SiLU", AI_SILU, OASP_EXT_MEMBER_CALL),
    MEMBER("Softmax", AI_SOFTMAX, OASP_EXT_MEMBER_CALL),
    MEMBER("LayerNorm", AI_LAYER_NORM, OASP_EXT_MEMBER_CALL),
    MEMBER("RMSNorm", AI_RMS_NORM, OASP_EXT_MEMBER_CALL),
    MEMBER("CrossEntropy", AI_CROSS_ENTROPY, OASP_EXT_MEMBER_CALL),
    MEMBER("MSELoss", AI_MSE_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("L1Loss", AI_L1_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("BCELoss", AI_BCE_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("BCEWithLogitsLoss", AI_BCE_WITH_LOGITS_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("NLLLoss", AI_NLL_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("KLDivLoss", AI_KL_DIV_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("HuberLoss", AI_HUBER_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("SmoothL1Loss", AI_SMOOTH_L1_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("FocalLoss", AI_FOCAL_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("DiceLoss", AI_DICE_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("InfoNCELoss", AI_INFO_NCE_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("CTCLoss", AI_CTC_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("ScaledDotProductAttention", AI_ATTENTION, OASP_EXT_MEMBER_CALL),
    MEMBER("Module", AI_MODULE, OASP_EXT_MEMBER_CALL),
    MEMBER("SGD", AI_SGD, OASP_EXT_MEMBER_CALL),
    MEMBER("Adam", AI_ADAM, OASP_EXT_MEMBER_CALL),
    MEMBER("AdamW", AI_ADAMW, OASP_EXT_MEMBER_CALL),
    MEMBER("Graph", AI_GRAPH, OASP_EXT_MEMBER_CALL),
    MEMBER("KVCache", AI_KV_CACHE, OASP_EXT_MEMBER_CALL),
    MEMBER("Sample", AI_SAMPLE, OASP_EXT_MEMBER_CALL),
    MEMBER("MemoryStats", AI_MEMORY_STATS, OASP_EXT_MEMBER_CALL),
    MEMBER("BackendInfo", AI_BACKEND_INFO, OASP_EXT_MEMBER_CALL),
    MEMBER("Synchronize", AI_SYNCHRONIZE, OASP_EXT_MEMBER_CALL),
    MEMBER("LoadModel", AI_LOAD_MODEL, OASP_EXT_MEMBER_CALL),
    MEMBER("SaveCheckpoint", AI_SAVE_CHECKPOINT, OASP_EXT_MEMBER_CALL),
    MEMBER("LoadCheckpoint", AI_LOAD_CHECKPOINT, OASP_EXT_MEMBER_CALL),
    MEMBER("Sequential", AI_SEQUENTIAL, OASP_EXT_MEMBER_CALL),
    MEMBER("Linear", AI_LINEAR, OASP_EXT_MEMBER_CALL),
    MEMBER("ReLULayer", AI_RELU_MODULE, OASP_EXT_MEMBER_CALL),
    MEMBER("Dropout", AI_DROPOUT, OASP_EXT_MEMBER_CALL),
    MEMBER("LayerNormLayer", AI_LAYER_NORM_MODULE, OASP_EXT_MEMBER_CALL),
    MEMBER("RMSNormLayer", AI_RMS_NORM_MODULE, OASP_EXT_MEMBER_CALL),
    MEMBER("Embedding", AI_EMBEDDING, OASP_EXT_MEMBER_CALL),
    MEMBER("BeginTrace", AI_BEGIN_TRACE, OASP_EXT_MEMBER_CALL),
    MEMBER("TensorDataset", AI_TENSOR_DATASET, OASP_EXT_MEMBER_CALL),
    MEMBER("MNIST", AI_MNIST, OASP_EXT_MEMBER_CALL),
    MEMBER("Tokenizer", AI_TOKENIZER, OASP_EXT_MEMBER_CALL),
    MEMBER("Quantize", AI_QUANTIZE, OASP_EXT_MEMBER_CALL),
    MEMBER("Dequantize", AI_DEQUANTIZE, OASP_EXT_MEMBER_CALL),
    MEMBER("StepScheduler", AI_STEP_SCHEDULER, OASP_EXT_MEMBER_CALL),
    MEMBER("CosineScheduler", AI_COSINE_SCHEDULER, OASP_EXT_MEMBER_CALL),
    MEMBER("LinearWarmupScheduler", AI_LINEAR_SCHEDULER, OASP_EXT_MEMBER_CALL),
    MEMBER("LossScaler", AI_LOSS_SCALER, OASP_EXT_MEMBER_CALL),
    MEMBER("LoadONNX", AI_LOAD_ONNX, OASP_EXT_MEMBER_CALL),
    MEMBER("ProfilerStart", AI_PROFILER_START, OASP_EXT_MEMBER_CALL),
    MEMBER("ProfilerStop", AI_PROFILER_STOP, OASP_EXT_MEMBER_CALL),
    MEMBER("ProfilerExport", AI_PROFILER_EXPORT, OASP_EXT_MEMBER_CALL),
    MEMBER("Devices", AI_DEVICES, OASP_EXT_MEMBER_CALL),
    MEMBER("GetDevice", AI_GET_DEVICE, OASP_EXT_MEMBER_CALL),
    MEMBER("SaveBundle", AI_SAVE_BUNDLE, OASP_EXT_MEMBER_CALL),
    MEMBER("Sum", AI_SUM, OASP_EXT_MEMBER_CALL),
    MEMBER("Mean", AI_MEAN, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member tensor_members[] = {
    MEMBER("Shape", TENSOR_SHAPE, OASP_EXT_MEMBER_GET),
    MEMBER("Strides", TENSOR_STRIDES, OASP_EXT_MEMBER_GET),
    MEMBER("Rank", TENSOR_RANK, OASP_EXT_MEMBER_GET),
    MEMBER("Size", TENSOR_SIZE, OASP_EXT_MEMBER_GET),
    MEMBER("DType", TENSOR_DTYPE, OASP_EXT_MEMBER_GET),
    MEMBER("Device", TENSOR_DEVICE, OASP_EXT_MEMBER_GET),
    MEMBER("Bytes", TENSOR_BYTES, OASP_EXT_MEMBER_GET),
    MEMBER("RequiresGrad", TENSOR_REQUIRES_GRAD, OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET),
    MEMBER("Grad", TENSOR_GRAD, OASP_EXT_MEMBER_GET),
    MEMBER("IsContiguous", TENSOR_CONTIGUOUS_PROPERTY, OASP_EXT_MEMBER_GET),
    MEMBER("Reshape", TENSOR_RESHAPE, OASP_EXT_MEMBER_CALL),
    MEMBER("Transpose", TENSOR_TRANSPOSE, OASP_EXT_MEMBER_CALL),
    MEMBER("Slice", TENSOR_SLICE, OASP_EXT_MEMBER_CALL),
    MEMBER("Contiguous", TENSOR_CONTIGUOUS, OASP_EXT_MEMBER_CALL),
    MEMBER("Clone", TENSOR_CLONE, OASP_EXT_MEMBER_CALL),
    MEMBER("Detach", TENSOR_DETACH, OASP_EXT_MEMBER_CALL),
    MEMBER("ToDType", TENSOR_TO_DTYPE, OASP_EXT_MEMBER_CALL),
    MEMBER("ToDevice", TENSOR_TO_DEVICE, OASP_EXT_MEMBER_CALL),
    MEMBER("ToArray", TENSOR_TO_ARRAY, OASP_EXT_MEMBER_CALL),
    MEMBER("ToBytes", TENSOR_TO_BYTES, OASP_EXT_MEMBER_CALL),
    MEMBER("Item", TENSOR_ITEM, OASP_EXT_MEMBER_CALL),
    MEMBER("Backward", TENSOR_BACKWARD, OASP_EXT_MEMBER_CALL),
    MEMBER("ZeroGrad", TENSOR_ZERO_GRAD, OASP_EXT_MEMBER_CALL),
    MEMBER("Add", TENSOR_ADD, OASP_EXT_MEMBER_CALL),
    MEMBER("Sub", TENSOR_SUB, OASP_EXT_MEMBER_CALL),
    MEMBER("Mul", TENSOR_MUL, OASP_EXT_MEMBER_CALL),
    MEMBER("Div", TENSOR_DIV, OASP_EXT_MEMBER_CALL),
    MEMBER("MatMul", TENSOR_MATMUL, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member module_members[] = {
    MEMBER("RegisterModule", MODULE_REGISTER_MODULE, OASP_EXT_MEMBER_CALL),
    MEMBER("RegisterParameter", MODULE_REGISTER_PARAMETER, OASP_EXT_MEMBER_CALL),
    MEMBER("RegisterBuffer", MODULE_REGISTER_BUFFER, OASP_EXT_MEMBER_CALL),
    MEMBER("ParameterCount", MODULE_PARAMETER_COUNT, OASP_EXT_MEMBER_GET),
    MEMBER("Parameter", MODULE_PARAMETER, OASP_EXT_MEMBER_CALL),
    MEMBER("Train", MODULE_TRAIN, OASP_EXT_MEMBER_CALL),
    MEMBER("Eval", MODULE_EVAL, OASP_EXT_MEMBER_CALL),
    MEMBER("SaveWeights", MODULE_SAVE_WEIGHTS, OASP_EXT_MEMBER_CALL),
    MEMBER("LoadWeights", MODULE_LOAD_WEIGHTS, OASP_EXT_MEMBER_CALL),
    MEMBER("Add", MODULE_ADD, OASP_EXT_MEMBER_CALL),
    MEMBER("Forward", MODULE_FORWARD, OASP_EXT_MEMBER_CALL),
    MEMBER("StateDict", MODULE_STATE_DICT, OASP_EXT_MEMBER_CALL),
    MEMBER("LoadStateDict", MODULE_LOAD_STATE_DICT, OASP_EXT_MEMBER_CALL),
    MEMBER("ToDevice", MODULE_TO_DEVICE, OASP_EXT_MEMBER_CALL),
    MEMBER("ToDType", MODULE_TO_DTYPE, OASP_EXT_MEMBER_CALL),
    MEMBER("SaveSafeTensors", MODULE_SAVE_SAFETENSORS, OASP_EXT_MEMBER_CALL),
    MEMBER("LoadSafeTensors", MODULE_LOAD_SAFETENSORS, OASP_EXT_MEMBER_CALL),
    MEMBER("Parameters", MODULE_PARAMETERS, OASP_EXT_MEMBER_CALL),
    MEMBER("NamedParameters", MODULE_NAMED_PARAMETERS, OASP_EXT_MEMBER_CALL),
    MEMBER("Buffers", MODULE_BUFFERS, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member optimizer_members[] = {
    MEMBER("ZeroGrad", OPTIMIZER_ZERO_GRAD, OASP_EXT_MEMBER_CALL),
    MEMBER("Step", OPTIMIZER_STEP, OASP_EXT_MEMBER_CALL),
    MEMBER("ClipGradNorm", OPTIMIZER_CLIP_GRAD_NORM, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member graph_members[] = {
    MEMBER("Input", GRAPH_INPUT, OASP_EXT_MEMBER_CALL),
    MEMBER("Operation", GRAPH_OPERATION, OASP_EXT_MEMBER_CALL),
    MEMBER("Build", GRAPH_BUILD, OASP_EXT_MEMBER_CALL),
    MEMBER("Save", GRAPH_SAVE, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member trace_members[] = {
    MEMBER("Input", GRAPH_INPUT, OASP_EXT_MEMBER_CALL),
    MEMBER("EndModel", GRAPH_BUILD, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member model_members[] = {
    MEMBER("Forward", MODEL_FORWARD, OASP_EXT_MEMBER_CALL),
    MEMBER("Save", MODEL_SAVE, OASP_EXT_MEMBER_CALL),
    MEMBER("ForwardAsync", MODEL_FORWARD_ASYNC, OASP_EXT_MEMBER_CALL),
    MEMBER("ExportONNX", MODEL_EXPORT_ONNX, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member cache_members[] = {
    MEMBER("Length", CACHE_LENGTH, OASP_EXT_MEMBER_GET),
    MEMBER("Append", CACHE_APPEND, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member dataset_members[] = {
    MEMBER("Size", DATASET_SIZE, OASP_EXT_MEMBER_GET),
    MEMBER("DataLoader", DATASET_DATALOADER, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member dataloader_members[] = {
    MEMBER("Iterator", DATALOADER_ITERATOR, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member data_iterator_members[] = {
    MEMBER("MoveNext", ITERATOR_MOVE_NEXT, OASP_EXT_MEMBER_CALL),
    MEMBER("Current", ITERATOR_CURRENT, OASP_EXT_MEMBER_GET),
    MEMBER("Cursor", ITERATOR_CURSOR, OASP_EXT_MEMBER_GET),
};

static const oasp_extension_member batch_members[] = {
    MEMBER("Inputs", BATCH_INPUTS, OASP_EXT_MEMBER_GET),
    MEMBER("Labels", BATCH_LABELS, OASP_EXT_MEMBER_GET),
    MEMBER("Size", BATCH_SIZE, OASP_EXT_MEMBER_GET),
};

static const oasp_extension_member tokenizer_members[] = {
    MEMBER("Encode", TOKENIZER_ENCODE, OASP_EXT_MEMBER_CALL),
    MEMBER("Decode", TOKENIZER_DECODE, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member scheduler_members[] = {
    MEMBER("Step", SCHEDULER_STEP, OASP_EXT_MEMBER_CALL),
    MEMBER("CurrentStep", SCHEDULER_CURRENT_STEP, OASP_EXT_MEMBER_GET),
};

static const oasp_extension_member loss_scaler_members[] = {
    MEMBER("Scale", LOSS_SCALER_SCALE, OASP_EXT_MEMBER_GET),
    MEMBER("ScaleLoss", LOSS_SCALER_SCALE_LOSS, OASP_EXT_MEMBER_CALL),
    MEMBER("Step", LOSS_SCALER_STEP, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member state_dict_members[] = {
    MEMBER("Count", STATE_DICT_COUNT, OASP_EXT_MEMBER_GET),
    MEMBER("Name", STATE_DICT_NAME, OASP_EXT_MEMBER_CALL),
    MEMBER("Kind", STATE_DICT_KIND, OASP_EXT_MEMBER_CALL),
    MEMBER("Tensor", STATE_DICT_TENSOR, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member event_members[] = {
    MEMBER("Ready", EVENT_READY, OASP_EXT_MEMBER_GET),
    MEMBER("Wait", EVENT_WAIT, OASP_EXT_MEMBER_CALL),
    MEMBER("OutputCount", EVENT_OUTPUT_COUNT, OASP_EXT_MEMBER_GET),
    MEMBER("Output", EVENT_OUTPUT, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member parameter_collection_members[] = {
    MEMBER("Count", COLLECTION_COUNT, OASP_EXT_MEMBER_GET),
    MEMBER("Name", COLLECTION_NAME, OASP_EXT_MEMBER_CALL),
    MEMBER("Tensor", COLLECTION_TENSOR, OASP_EXT_MEMBER_CALL),
    MEMBER("Kind", COLLECTION_KIND, OASP_EXT_MEMBER_CALL),
};

static const oasp_extension_member checkpoint_state_members[] = {
    MEMBER("Metadata", CHECKPOINT_METADATA, OASP_EXT_MEMBER_GET),
    MEMBER("Epoch", CHECKPOINT_EPOCH, OASP_EXT_MEMBER_GET),
    MEMBER("GlobalStep", CHECKPOINT_GLOBAL_STEP, OASP_EXT_MEMBER_GET),
};

static const oasp_extension_member device_members[] = {
    MEMBER("Name", DEVICE_NAME, OASP_EXT_MEMBER_GET),
    MEMBER("Backend", DEVICE_BACKEND, OASP_EXT_MEMBER_GET),
    MEMBER("Ordinal", DEVICE_ORDINAL, OASP_EXT_MEMBER_GET),
    MEMBER("TotalMemory", DEVICE_TOTAL_MEMORY, OASP_EXT_MEMBER_GET),
    MEMBER("FreeMemory", DEVICE_FREE_MEMORY, OASP_EXT_MEMBER_GET),
    MEMBER("Supports", DEVICE_SUPPORTS, OASP_EXT_MEMBER_CALL),
    MEMBER("SupportsOperator", DEVICE_SUPPORTS_OPERATOR, OASP_EXT_MEMBER_CALL),
};

static int module_initialize(const oasp_ext_host *host, uint64_t capabilities, void **state, oasp_ext_error *error) {
    uint32_t version = 0;
    (void)capabilities;
    (void)state;
    (void)error;
    values = host->query_api(OASP_EXT_API_VALUE, OASP_EXT_API_VALUE_VERSION, &version);
    objects = host->query_api(OASP_EXT_API_OBJECT, OASP_EXT_API_OBJECT_VERSION, &version);
    runtime_api = host->query_api(OASP_EXT_API_RUNTIME, OASP_EXT_API_RUNTIME_VERSION, &version);
    return values && objects && runtime_api ? OASP_EXT_OK : OASP_EXT_ERROR;
}

static const oasp_extension_class classes[] = {
    {sizeof(oasp_extension_class), "OpenASP.AI", "OpenASP.AI", CLASS_AI, 0, factory_members, sizeof(factory_members) / sizeof(factory_members[0]),
     factory_create, factory_destroy, factory_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Tensor", "OpenASP.AI.Tensor", CLASS_TENSOR, OASP_EXT_CLASS_INTERNAL, tensor_members,
     sizeof(tensor_members) / sizeof(tensor_members[0]), NULL, tensor_destroy, tensor_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Module", "OpenASP.AI.Module", CLASS_MODULE, OASP_EXT_CLASS_INTERNAL, module_members,
     sizeof(module_members) / sizeof(module_members[0]), NULL, module_destroy, module_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Optimizer", "OpenASP.AI.Optimizer", CLASS_OPTIMIZER, OASP_EXT_CLASS_INTERNAL, optimizer_members,
     sizeof(optimizer_members) / sizeof(optimizer_members[0]), NULL, optimizer_destroy, optimizer_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Graph", "OpenASP.AI.Graph", CLASS_GRAPH, OASP_EXT_CLASS_INTERNAL, graph_members,
     sizeof(graph_members) / sizeof(graph_members[0]), NULL, graph_destroy, graph_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Model", "OpenASP.AI.Model", CLASS_MODEL, OASP_EXT_CLASS_INTERNAL, model_members,
     sizeof(model_members) / sizeof(model_members[0]), NULL, model_destroy, model_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.KVCache", "OpenASP.AI.KVCache", CLASS_KV_CACHE, OASP_EXT_CLASS_INTERNAL, cache_members,
     sizeof(cache_members) / sizeof(cache_members[0]), NULL, cache_destroy, cache_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Trace", "OpenASP.AI.Trace", CLASS_TRACE, OASP_EXT_CLASS_INTERNAL, trace_members,
     sizeof(trace_members) / sizeof(trace_members[0]), NULL, graph_destroy, graph_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Dataset", "OpenASP.AI.Dataset", CLASS_DATASET, OASP_EXT_CLASS_INTERNAL, dataset_members,
     sizeof(dataset_members) / sizeof(dataset_members[0]), NULL, dataset_destroy, dataset_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.DataLoader", "OpenASP.AI.DataLoader", CLASS_DATALOADER, OASP_EXT_CLASS_INTERNAL,
     dataloader_members, sizeof(dataloader_members) / sizeof(dataloader_members[0]), NULL, dataloader_destroy, dataloader_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.DataIterator", "OpenASP.AI.DataIterator", CLASS_DATA_ITERATOR, OASP_EXT_CLASS_INTERNAL,
     data_iterator_members, sizeof(data_iterator_members) / sizeof(data_iterator_members[0]), NULL, data_iterator_destroy, data_iterator_invoke,
     NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Batch", "OpenASP.AI.Batch", CLASS_BATCH, OASP_EXT_CLASS_INTERNAL, batch_members,
     sizeof(batch_members) / sizeof(batch_members[0]), NULL, batch_destroy, batch_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Tokenizer", "OpenASP.AI.Tokenizer", CLASS_TOKENIZER, OASP_EXT_CLASS_INTERNAL,
     tokenizer_members, sizeof(tokenizer_members) / sizeof(tokenizer_members[0]), NULL, tokenizer_destroy, tokenizer_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Scheduler", "OpenASP.AI.Scheduler", CLASS_SCHEDULER, OASP_EXT_CLASS_INTERNAL,
     scheduler_members, sizeof(scheduler_members) / sizeof(scheduler_members[0]), NULL, scheduler_destroy, scheduler_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.LossScaler", "OpenASP.AI.LossScaler", CLASS_LOSS_SCALER, OASP_EXT_CLASS_INTERNAL,
     loss_scaler_members, sizeof(loss_scaler_members) / sizeof(loss_scaler_members[0]), NULL, loss_scaler_destroy, loss_scaler_invoke, NULL,
     NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.StateDict", "OpenASP.AI.StateDict", CLASS_STATE_DICT, OASP_EXT_CLASS_INTERNAL,
     state_dict_members, sizeof(state_dict_members) / sizeof(state_dict_members[0]), NULL, state_dict_destroy, state_dict_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Event", "OpenASP.AI.Event", CLASS_EVENT, OASP_EXT_CLASS_INTERNAL,
     event_members, sizeof(event_members) / sizeof(event_members[0]), NULL, event_destroy, event_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.ParameterCollection", "OpenASP.AI.ParameterCollection", CLASS_PARAMETER_COLLECTION,
     OASP_EXT_CLASS_INTERNAL, parameter_collection_members,
     sizeof(parameter_collection_members) / sizeof(parameter_collection_members[0]), NULL, parameter_collection_destroy,
     parameter_collection_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.CheckpointState", "OpenASP.AI.CheckpointState", CLASS_CHECKPOINT_STATE,
     OASP_EXT_CLASS_INTERNAL, checkpoint_state_members, sizeof(checkpoint_state_members) / sizeof(checkpoint_state_members[0]), NULL,
     checkpoint_state_destroy, checkpoint_state_invoke, NULL, NULL},
    {sizeof(oasp_extension_class), "OpenASP.AI.Device", "OpenASP.AI.Device", CLASS_DEVICE, OASP_EXT_CLASS_INTERNAL,
     device_members, sizeof(device_members) / sizeof(device_members[0]), NULL, device_destroy, device_invoke, NULL, NULL},
};

static const oasp_extension_descriptor descriptor = {
    sizeof(descriptor),
    OASP_EXTENSION_ABI_MAJOR,
    OASP_EXTENSION_ABI_MINOR,
    "openasp-ai",
    "1.0.0",
    0,
    module_initialize,
    NULL,
    NULL,
    NULL,
    request_init,
    request_shutdown,
    classes,
    sizeof(classes) / sizeof(classes[0]),
};

OASP_EXTENSION_EXPORT const oasp_extension_descriptor *openasp_extension(void) {
    return &descriptor;
}
