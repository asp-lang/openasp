// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float f16_to_f32(uint16_t value) {
    uint32_t sign = (uint32_t)(value & 0x8000) << 16;
    uint32_t exponent = (value >> 10) & 0x1f;
    uint32_t fraction = value & 0x3ff;
    uint32_t bits;
    if (!exponent) {
        if (!fraction)
            bits = sign;
        else {
            exponent = 127 - 15 + 1;
            while (!(fraction & 0x400)) {
                fraction <<= 1;
                exponent--;
            }
            bits = sign | (exponent << 23) | ((fraction & 0x3ff) << 13);
        }
    } else if (exponent == 0x1f)
        bits = sign | UINT32_C(0x7f800000) | (fraction << 13);
    else
        bits = sign | ((exponent + 127 - 15) << 23) | (fraction << 13);
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static uint16_t f32_to_f16(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = (int32_t)((bits >> 23) & 0xff) - 127 + 15;
    uint32_t fraction = bits & 0x7fffff;
    if (exponent <= 0) {
        if (exponent < -10)
            return (uint16_t)sign;
        fraction = (fraction | 0x800000) >> (uint32_t)(1 - exponent);
        return (uint16_t)(sign | ((fraction + 0x1000) >> 13));
    }
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | ((fraction + 0x1000) >> 13));
}

static uint64_t tensor_physical_index(const oasp_ai_tensor *tensor, uint64_t logical_index) {
    uint64_t physical = tensor->offset;
    for (uint32_t axis = tensor->desc.rank; axis; axis--) {
        uint64_t dimension = (uint64_t)tensor->desc.shape[axis - 1];
        physical += (logical_index % dimension) * (uint64_t)tensor->desc.strides[axis - 1];
        logical_index /= dimension;
    }
    return physical;
}

double oasp_ai_tensor_read(const oasp_ai_tensor *tensor, uint64_t logical_index) {
    if (tensor->storage->memory_kind == OASP_AI_MEMORY_DEVICE) {
        size_t element_size = oasp_ai_dtype_size(tensor->desc.dtype);
        unsigned char value[8] = {0};
        oasp_ai_registered_backend *backend = &tensor->context->backends[tensor->storage->backend_index];
        oasp_ai_copy_desc copy = {
            .struct_size = sizeof(copy),
            .source = tensor->storage->backend_storage,
            .source_offset = tensor_physical_index(tensor, logical_index) * element_size,
            .bytes = element_size,
            .destination_host = value,
            .source_kind = OASP_AI_MEMORY_DEVICE,
            .destination_kind = OASP_AI_MEMORY_HOST,
        };
        if (backend->descriptor->api->copy(backend->transfer_stream, &copy, NULL, NULL) ||
            backend->descriptor->api->synchronize(backend->transfer_stream, NULL))
            return NAN;
        oasp_ai_storage host_storage = {.data = value, .memory_kind = OASP_AI_MEMORY_HOST};
        oasp_ai_tensor host = *tensor;
        host.storage = &host_storage;
        host.offset = 0;
        host.desc.rank = 1;
        host.desc.shape[0] = 1;
        host.desc.strides[0] = 1;
        return oasp_ai_tensor_read(&host, 0);
    }
    uint64_t index = tensor_physical_index(tensor, logical_index);
    const unsigned char *data = tensor->storage->data;
    double value;
    switch (tensor->desc.dtype) {
    case OASP_AI_DTYPE_BOOL:
    case OASP_AI_DTYPE_U8:
        value = ((const uint8_t *)data)[index];
        break;
    case OASP_AI_DTYPE_I8:
        value = ((const int8_t *)data)[index];
        break;
    case OASP_AI_DTYPE_I16:
        value = ((const int16_t *)data)[index];
        break;
    case OASP_AI_DTYPE_I32:
        value = ((const int32_t *)data)[index];
        break;
    case OASP_AI_DTYPE_I64:
        value = (double)((const int64_t *)data)[index];
        break;
    case OASP_AI_DTYPE_F16:
        value = f16_to_f32(((const uint16_t *)data)[index]);
        break;
    case OASP_AI_DTYPE_BF16: {
        uint32_t bits = (uint32_t)((const uint16_t *)data)[index] << 16;
        float decoded;
        memcpy(&decoded, &bits, sizeof(decoded));
        value = decoded;
        break;
    }
    case OASP_AI_DTYPE_F32:
        value = ((const float *)data)[index];
        break;
    case OASP_AI_DTYPE_F64:
        value = ((const double *)data)[index];
        break;
    default:
        return 0;
    }
    if (tensor->quantized_bits) {
        double scale = tensor->quantized_scale;
        if (tensor->quantized_channel_count && tensor->quantized_axis >= 0) {
            uint64_t divisor = 1;
            for (uint32_t axis = tensor->desc.rank; axis > (uint32_t)tensor->quantized_axis + 1; axis--)
                divisor *= (uint64_t)tensor->desc.shape[axis - 1];
            uint64_t channel = logical_index / divisor % (uint64_t)tensor->desc.shape[tensor->quantized_axis];
            scale = tensor->quantized_channel_scales[channel];
        }
        value = (value - (double)tensor->quantized_zero_point) * scale;
    }
    return value;
}

void oasp_ai_tensor_write(oasp_ai_tensor *tensor, uint64_t logical_index, double value) {
    if (tensor->storage->memory_kind == OASP_AI_MEMORY_DEVICE) {
        size_t element_size = oasp_ai_dtype_size(tensor->desc.dtype);
        unsigned char encoded[8] = {0};
        oasp_ai_storage host_storage = {.data = encoded, .memory_kind = OASP_AI_MEMORY_HOST};
        oasp_ai_tensor host = *tensor;
        host.storage = &host_storage;
        host.offset = 0;
        host.desc.rank = 1;
        host.desc.shape[0] = 1;
        host.desc.strides[0] = 1;
        oasp_ai_tensor_write(&host, 0, value);
        oasp_ai_registered_backend *backend = &tensor->context->backends[tensor->storage->backend_index];
        oasp_ai_copy_desc copy = {
            .struct_size = sizeof(copy),
            .destination = tensor->storage->backend_storage,
            .destination_offset = tensor_physical_index(tensor, logical_index) * element_size,
            .bytes = element_size,
            .source_host = encoded,
            .source_kind = OASP_AI_MEMORY_HOST,
            .destination_kind = OASP_AI_MEMORY_DEVICE,
        };
        backend->descriptor->api->copy(backend->transfer_stream, &copy, NULL, NULL);
        backend->descriptor->api->synchronize(backend->transfer_stream, NULL);
        return;
    }
    uint64_t index = tensor_physical_index(tensor, logical_index);
    unsigned char *data = tensor->storage->data;
    if (tensor->quantized_bits) {
        double scale = tensor->quantized_scale;
        if (tensor->quantized_channel_count && tensor->quantized_axis >= 0) {
            uint64_t divisor = 1;
            for (uint32_t axis = tensor->desc.rank; axis > (uint32_t)tensor->quantized_axis + 1; axis--)
                divisor *= (uint64_t)tensor->desc.shape[axis - 1];
            uint64_t channel = logical_index / divisor % (uint64_t)tensor->desc.shape[tensor->quantized_axis];
            scale = tensor->quantized_channel_scales[channel];
        }
        double quantized = nearbyint(value / scale) + (double)tensor->quantized_zero_point;
        double minimum = tensor->quantized_bits == 4 ? -8 : -128;
        double maximum = tensor->quantized_bits == 4 ? 7 : 127;
        value = quantized < minimum ? minimum : quantized > maximum ? maximum : quantized;
    }
    switch (tensor->desc.dtype) {
    case OASP_AI_DTYPE_BOOL:
        ((uint8_t *)data)[index] = value != 0;
        break;
    case OASP_AI_DTYPE_U8:
        ((uint8_t *)data)[index] = (uint8_t)value;
        break;
    case OASP_AI_DTYPE_I8:
        ((int8_t *)data)[index] = (int8_t)value;
        break;
    case OASP_AI_DTYPE_I16:
        ((int16_t *)data)[index] = (int16_t)value;
        break;
    case OASP_AI_DTYPE_I32:
        ((int32_t *)data)[index] = (int32_t)value;
        break;
    case OASP_AI_DTYPE_I64:
        ((int64_t *)data)[index] = (int64_t)value;
        break;
    case OASP_AI_DTYPE_F16:
        ((uint16_t *)data)[index] = f32_to_f16((float)value);
        break;
    case OASP_AI_DTYPE_BF16: {
        float narrowed = (float)value;
        uint32_t bits;
        memcpy(&bits, &narrowed, sizeof(bits));
        ((uint16_t *)data)[index] = (uint16_t)(bits >> 16);
        break;
    }
    case OASP_AI_DTYPE_F32:
        ((float *)data)[index] = (float)value;
        break;
    case OASP_AI_DTYPE_F64:
        ((double *)data)[index] = value;
        break;
    }
}

static oasp_ai_storage *storage_create(oasp_ai_context *context, uint64_t bytes, oasp_ai_error *error) {
    if (!context || bytes > context->options.maximum_tensor_bytes || bytes > SIZE_MAX) {
        oasp_ai_set_error(error, 7, "Allocate", "tensor exceeds the configured memory limit", "Reduce the shape or raise max tensor bytes.");
        return NULL;
    }
    oasp_ai_storage *storage = calloc(1, sizeof(*storage));
    if (storage)
        storage->data = calloc(1, (size_t)bytes);
    if (!storage || !storage->data) {
        free(storage);
        oasp_ai_set_error(error, 7, "Allocate", "unable to allocate tensor storage", "Reduce tensor size or release unused tensors.");
        return NULL;
    }
    atomic_init(&storage->references, 1);
    storage->context = context;
    storage->bytes = bytes;
    storage->memory_kind = OASP_AI_MEMORY_HOST;
    storage->backend_index = -1;
    context->metrics.live_bytes += bytes;
    if (context->metrics.live_bytes > context->metrics.peak_bytes)
        context->metrics.peak_bytes = context->metrics.live_bytes;
    return storage;
}

static void storage_retain(oasp_ai_storage *storage) {
    atomic_fetch_add_explicit(&storage->references, 1, memory_order_relaxed);
}

static void storage_release(oasp_ai_storage *storage) {
    if (!storage || atomic_fetch_sub_explicit(&storage->references, 1, memory_order_acq_rel) != 1)
        return;
    storage->context->metrics.live_bytes -= storage->bytes;
    if (storage->memory_kind == OASP_AI_MEMORY_DEVICE) {
        oasp_ai_registered_backend *backend = &storage->context->backends[storage->backend_index];
        backend->descriptor->api->free(backend->device, storage->backend_storage);
    } else
        free(storage->data);
    free(storage);
}

static oasp_ai_tensor *tensor_allocate_metadata(oasp_ai_context *context, oasp_ai_error *error) {
    oasp_ai_tensor *tensor = calloc(1, sizeof(*tensor));
    if (!tensor) {
        oasp_ai_set_error(error, 7, "Tensor", "unable to allocate tensor metadata", "Reduce process memory pressure.");
        return NULL;
    }
    atomic_init(&tensor->references, 1);
    tensor->context = context;
    return tensor;
}

oasp_ai_tensor *oasp_ai_tensor_create(oasp_ai_context *context, const oasp_ai_tensor_desc *desc, const void *data, size_t data_size, oasp_ai_error *error) {
    uint64_t elements;
    if (!context || !desc || !oasp_ai_dtype_size(desc->dtype) || oasp_ai_checked_elements(desc->shape, desc->rank, &elements, error))
        return NULL;
    size_t element_size = oasp_ai_dtype_size(desc->dtype);
    if (elements > UINT64_MAX / element_size) {
        oasp_ai_set_error(error, 7, "Tensor", "tensor byte size overflows", "Reduce the tensor shape.");
        return NULL;
    }
    uint64_t bytes = elements * element_size;
    if (data && data_size != bytes) {
        oasp_ai_set_error(error, 5, "Tensor", "input byte length does not match shape and dtype", "Pass exactly Shape product times dtype size bytes.");
        return NULL;
    }
    oasp_ai_tensor *tensor = tensor_allocate_metadata(context, error);
    if (!tensor)
        return NULL;
    tensor->storage = storage_create(context, bytes, error);
    if (!tensor->storage) {
        free(tensor);
        return NULL;
    }
    tensor->desc = *desc;
    int64_t stride = 1;
    for (uint32_t axis = desc->rank; axis; axis--) {
        tensor->desc.strides[axis - 1] = stride;
        stride *= desc->shape[axis - 1];
    }
    tensor->desc.flags |= OASP_AI_TENSOR_CONTIGUOUS;
    tensor->element_count = elements;
    if (data)
        memcpy(tensor->storage->data, data, data_size);
    return tensor;
}

oasp_ai_tensor *oasp_ai_tensor_create_on_backend(oasp_ai_context *context, const oasp_ai_tensor_desc *desc, int32_t backend_index,
                                                 oasp_ai_error *error) {
    uint64_t elements;
    if (!context || backend_index < 0 || (size_t)backend_index >= context->backend_count || !desc ||
        !oasp_ai_dtype_size(desc->dtype) || oasp_ai_checked_elements(desc->shape, desc->rank, &elements, error))
        return NULL;
    size_t element_size = oasp_ai_dtype_size(desc->dtype);
    if (elements > UINT64_MAX / element_size)
        return NULL;
    uint64_t bytes = elements * element_size;
    if (bytes > context->options.maximum_tensor_bytes) {
        oasp_ai_set_error(error, 7, "Allocate", "tensor exceeds the configured memory limit", "Reduce the shape or raise max tensor bytes.");
        return NULL;
    }
    oasp_ai_tensor *tensor = tensor_allocate_metadata(context, error);
    oasp_ai_storage *storage = tensor ? calloc(1, sizeof(*storage)) : NULL;
    if (!tensor || !storage) {
        free(tensor);
        free(storage);
        return NULL;
    }
    oasp_ai_registered_backend *backend = &context->backends[backend_index];
    oasp_ai_alloc_desc allocation = {
        .struct_size = sizeof(allocation),
        .bytes = bytes,
        .alignment = 256,
        .memory_kind = OASP_AI_MEMORY_DEVICE,
    };
    if (!backend->descriptor->api->alloc ||
        backend->descriptor->api->alloc(backend->device, &allocation, &storage->backend_storage, error)) {
        free(storage);
        free(tensor);
        return NULL;
    }
    atomic_init(&storage->references, 1);
    storage->context = context;
    storage->bytes = bytes;
    storage->memory_kind = OASP_AI_MEMORY_DEVICE;
    storage->backend_index = backend_index;
    tensor->storage = storage;
    tensor->desc = *desc;
    int64_t stride = 1;
    for (uint32_t axis = desc->rank; axis; axis--) {
        tensor->desc.strides[axis - 1] = stride;
        stride *= desc->shape[axis - 1];
    }
    tensor->desc.flags |= OASP_AI_TENSOR_CONTIGUOUS;
    tensor->element_count = elements;
    context->metrics.live_bytes += bytes;
    if (context->metrics.live_bytes > context->metrics.peak_bytes)
        context->metrics.peak_bytes = context->metrics.live_bytes;
    return tensor;
}

oasp_ai_tensor *oasp_ai_tensor_full(oasp_ai_context *context, const int64_t *shape, uint32_t rank, oasp_ai_dtype dtype, double value,
                                    oasp_ai_error *error) {
    oasp_ai_tensor_desc desc = {.dtype = dtype, .rank = rank};
    if (rank <= OASP_AI_MAX_RANK)
        memcpy(desc.shape, shape, rank * sizeof(*shape));
    oasp_ai_tensor *tensor = oasp_ai_tensor_create(context, &desc, NULL, 0, error);
    if (!tensor)
        return NULL;
    for (uint64_t index = 0; index < tensor->element_count; index++)
        oasp_ai_tensor_write(tensor, index, value);
    return tensor;
}

static uint64_t random_next(oasp_ai_context *context) {
    uint64_t value = context->options.seed + UINT64_C(0x9e3779b97f4a7c15) * ++context->rng_counter;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

oasp_ai_tensor *oasp_ai_tensor_random(oasp_ai_context *context, const int64_t *shape, uint32_t rank, oasp_ai_dtype dtype, double first,
                                      double second, int normal, oasp_ai_error *error) {
    oasp_ai_tensor *tensor = oasp_ai_tensor_full(context, shape, rank, dtype, 0, error);
    if (!tensor)
        return NULL;
    for (uint64_t index = 0; index < tensor->element_count; index++) {
        double uniform = ((random_next(context) >> 11) + 1.0) / 9007199254740993.0;
        double value = first + (second - first) * uniform;
        if (normal) {
            double second_uniform = ((random_next(context) >> 11) + 1.0) / 9007199254740993.0;
            value = first + second * sqrt(-2 * log(uniform)) * cos(6.283185307179586 * second_uniform);
        }
        oasp_ai_tensor_write(tensor, index, value);
    }
    return tensor;
}

void oasp_ai_tensor_retain(oasp_ai_tensor *tensor) {
    if (tensor)
        atomic_fetch_add_explicit(&tensor->references, 1, memory_order_relaxed);
}

static void autograd_node_release(oasp_ai_autograd_node *node) {
    if (!node || atomic_fetch_sub_explicit(&node->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t index = 0; index < node->input_count; index++)
        oasp_ai_tensor_release(node->inputs[index]);
    free(node->inputs);
    free(node->versions);
    free(node);
}

void oasp_ai_tensor_release(oasp_ai_tensor *tensor) {
    if (!tensor || atomic_fetch_sub_explicit(&tensor->references, 1, memory_order_acq_rel) != 1)
        return;
    storage_release(tensor->storage);
    autograd_node_release(tensor->grad_fn);
    oasp_ai_tensor_release(tensor->grad);
    free(tensor->quantized_channel_scales);
    free(tensor);
}

const oasp_ai_tensor_desc *oasp_ai_tensor_descriptor(const oasp_ai_tensor *tensor) {
    return tensor ? &tensor->desc : NULL;
}

uint64_t oasp_ai_tensor_elements(const oasp_ai_tensor *tensor) {
    return tensor ? tensor->element_count : 0;
}

uint64_t oasp_ai_tensor_bytes(const oasp_ai_tensor *tensor) {
    return tensor ? tensor->element_count * oasp_ai_dtype_size(tensor->desc.dtype) : 0;
}

void *oasp_ai_tensor_data(oasp_ai_tensor *tensor) {
    if (!tensor || tensor->storage->memory_kind != OASP_AI_MEMORY_HOST ||
        !(tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS))
        return NULL;
    return (unsigned char *)tensor->storage->data + tensor->offset * oasp_ai_dtype_size(tensor->desc.dtype);
}

const void *oasp_ai_tensor_const_data(const oasp_ai_tensor *tensor) {
    return oasp_ai_tensor_data((oasp_ai_tensor *)tensor);
}

oasp_ai_tensor *oasp_ai_tensor_like(oasp_ai_tensor *tensor, int zero, oasp_ai_error *error) {
    int32_t backend_index = oasp_ai_tensor_backend_index(tensor);
    oasp_ai_tensor *result = backend_index >= 0
                                 ? oasp_ai_tensor_create_on_backend(tensor->context, &tensor->desc, backend_index, error)
                                 : oasp_ai_tensor_create(tensor->context, &tensor->desc, NULL, 0, error);
    if (result && tensor->quantized_bits) {
        result->quantized_bits = tensor->quantized_bits;
        result->quantized_axis = tensor->quantized_axis;
        result->quantized_scale = tensor->quantized_scale;
        result->quantized_zero_point = tensor->quantized_zero_point;
        result->quantized_channel_count = tensor->quantized_channel_count;
        memcpy(result->quantized_calibration, tensor->quantized_calibration, sizeof(result->quantized_calibration));
        if (tensor->quantized_channel_count) {
            result->quantized_channel_scales = malloc(tensor->quantized_channel_count * sizeof(*result->quantized_channel_scales));
            if (!result->quantized_channel_scales) {
                oasp_ai_tensor_release(result);
                return NULL;
            }
            memcpy(result->quantized_channel_scales, tensor->quantized_channel_scales,
                   tensor->quantized_channel_count * sizeof(*result->quantized_channel_scales));
        }
    }
    if (result && !zero) {
        size_t bytes = (size_t)oasp_ai_tensor_bytes(tensor);
        if (backend_index >= 0) {
            if (tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS) {
                oasp_ai_registered_backend *backend = &tensor->context->backends[backend_index];
                oasp_ai_event ready = 0;
                oasp_ai_copy_desc copy = {
                    .struct_size = sizeof(copy),
                    .source = tensor->storage->backend_storage,
                    .destination = result->storage->backend_storage,
                    .source_offset = tensor->offset * oasp_ai_dtype_size(tensor->desc.dtype),
                    .bytes = bytes,
                    .source_kind = OASP_AI_MEMORY_DEVICE,
                    .destination_kind = OASP_AI_MEMORY_DEVICE,
                };
                oasp_ai_event copied = 0;
                if (backend->descriptor->api->record_event(backend->compute_stream, &ready, error) ||
                    backend->descriptor->api->wait_event(backend->transfer_stream, ready, error) ||
                    backend->descriptor->api->copy(backend->transfer_stream, &copy, &copied, error) ||
                    backend->descriptor->api->wait_event(backend->compute_stream, copied, error)) {
                    oasp_ai_tensor_release(result);
                    return NULL;
                }
            } else {
                oasp_ai_tensor *inputs[] = {tensor};
                oasp_ai_tensor *outputs[] = {result};
                if (oasp_ai_backend_execute(tensor->context, backend_index, OASP_AI_OP_CONTIGUOUS,
                                            inputs, 1, outputs, 1, NULL, 0, error)) {
                    oasp_ai_tensor_release(result);
                    return NULL;
                }
            }
            tensor->context->metrics.device_to_device_bytes += bytes;
        } else
            for (uint64_t index = 0; index < tensor->element_count; index++)
                oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(tensor, index));
    }
    return result;
}

oasp_ai_tensor *oasp_ai_tensor_clone(oasp_ai_tensor *tensor, oasp_ai_error *error) {
    if (!tensor)
        return NULL;
    oasp_ai_tensor *result = oasp_ai_tensor_like(tensor, 0, error);
    oasp_ai_tensor *inputs[] = {tensor};
    if (result && oasp_ai_attach_grad(result, OASP_AI_OP_CONTIGUOUS, inputs, 1, 0, -1, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    return result;
}

oasp_ai_tensor *oasp_ai_tensor_contiguous(oasp_ai_tensor *tensor, oasp_ai_error *error) {
    if (!tensor)
        return NULL;
    if (tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS) {
        oasp_ai_tensor_retain(tensor);
        return tensor;
    }
    return oasp_ai_tensor_clone(tensor, error);
}

static oasp_ai_tensor *tensor_view(oasp_ai_tensor *tensor, oasp_ai_error *error) {
    oasp_ai_tensor *view = tensor_allocate_metadata(tensor->context, error);
    if (!view)
        return NULL;
    view->storage = tensor->storage;
    storage_retain(view->storage);
    view->desc = tensor->desc;
    view->offset = tensor->offset;
    view->element_count = tensor->element_count;
    view->quantized_bits = tensor->quantized_bits;
    view->quantized_axis = tensor->quantized_axis;
    view->quantized_scale = tensor->quantized_scale;
    view->quantized_zero_point = tensor->quantized_zero_point;
    view->quantized_channel_count = tensor->quantized_channel_count;
    memcpy(view->quantized_calibration, tensor->quantized_calibration, sizeof(view->quantized_calibration));
    if (tensor->quantized_channel_count) {
        view->quantized_channel_scales = malloc(tensor->quantized_channel_count * sizeof(*view->quantized_channel_scales));
        if (!view->quantized_channel_scales) {
            oasp_ai_tensor_release(view);
            return NULL;
        }
        memcpy(view->quantized_channel_scales, tensor->quantized_channel_scales,
               tensor->quantized_channel_count * sizeof(*view->quantized_channel_scales));
    }
    return view;
}

oasp_ai_tensor *oasp_ai_tensor_reshape(oasp_ai_tensor *tensor, const int64_t *shape, uint32_t rank, oasp_ai_error *error) {
    uint64_t elements;
    if (!tensor || oasp_ai_checked_elements(shape, rank, &elements, error))
        return NULL;
    if (elements != tensor->element_count) {
        oasp_ai_set_error(error, 5, "Reshape", "reshape changes the element count", "Use a shape with the same number of elements.");
        return NULL;
    }
    oasp_ai_tensor *source = tensor;
    oasp_ai_tensor *temporary = NULL;
    if (!(tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS)) {
        temporary = oasp_ai_tensor_clone(tensor, error);
        source = temporary;
    }
    if (!source)
        return NULL;
    oasp_ai_tensor *view = tensor_view(source, error);
    oasp_ai_tensor_release(temporary);
    if (!view)
        return NULL;
    view->desc.rank = rank;
    memcpy(view->desc.shape, shape, rank * sizeof(*shape));
    int64_t stride = 1;
    for (uint32_t axis = rank; axis; axis--) {
        view->desc.strides[axis - 1] = stride;
        stride *= shape[axis - 1];
    }
    oasp_ai_tensor *inputs[] = {tensor};
    if (oasp_ai_attach_grad(view, OASP_AI_OP_RESHAPE, inputs, 1, 0, -1, error)) {
        oasp_ai_tensor_release(view);
        return NULL;
    }
    return view;
}

oasp_ai_tensor *oasp_ai_tensor_transpose(oasp_ai_tensor *tensor, uint32_t first, uint32_t second, oasp_ai_error *error) {
    if (!tensor || first >= tensor->desc.rank || second >= tensor->desc.rank) {
        oasp_ai_set_error(error, 5, "Transpose", "transpose axis is out of range", "Use axes smaller than tensor rank.");
        return NULL;
    }
    oasp_ai_tensor *view = tensor_view(tensor, error);
    if (!view)
        return NULL;
    int64_t shape = view->desc.shape[first];
    view->desc.shape[first] = view->desc.shape[second];
    view->desc.shape[second] = shape;
    int64_t stride = view->desc.strides[first];
    view->desc.strides[first] = view->desc.strides[second];
    view->desc.strides[second] = stride;
    if (first != second)
        view->desc.flags &= ~OASP_AI_TENSOR_CONTIGUOUS;
    oasp_ai_tensor *inputs[] = {tensor};
    if (oasp_ai_attach_grad(view, OASP_AI_OP_TRANSPOSE, inputs, 1, (double)second, (int32_t)first, error)) {
        oasp_ai_tensor_release(view);
        return NULL;
    }
    return view;
}

oasp_ai_tensor *oasp_ai_tensor_slice(oasp_ai_tensor *tensor, uint32_t axis, int64_t start, int64_t stop, int64_t step,
                                     oasp_ai_error *error) {
    if (!tensor || axis >= tensor->desc.rank || step <= 0 || start < 0 || stop < start || stop > tensor->desc.shape[axis]) {
        oasp_ai_set_error(error, 5, "Slice", "slice bounds are invalid", "Use a positive step and bounds within the selected axis.");
        return NULL;
    }
    oasp_ai_tensor *view = tensor_view(tensor, error);
    if (!view)
        return NULL;
    view->offset += (uint64_t)(start * view->desc.strides[axis]);
    view->desc.shape[axis] = (stop - start + step - 1) / step;
    view->desc.strides[axis] *= step;
    if (start || stop != tensor->desc.shape[axis] || step != 1)
        view->desc.flags &= ~OASP_AI_TENSOR_CONTIGUOUS;
    oasp_ai_checked_elements(view->desc.shape, view->desc.rank, &view->element_count, NULL);
    oasp_ai_tensor *inputs[] = {tensor};
    if (oasp_ai_attach_grad(view, OASP_AI_OP_SLICE, inputs, 1, 0, (int32_t)axis, error)) {
        oasp_ai_tensor_release(view);
        return NULL;
    }
    if (view->grad_fn) {
        view->grad_fn->parameters[0] = start;
        view->grad_fn->parameters[1] = stop;
        view->grad_fn->parameters[2] = step;
    }
    return view;
}

oasp_ai_tensor *oasp_ai_tensor_cast(oasp_ai_tensor *tensor, oasp_ai_dtype dtype, oasp_ai_error *error) {
    if (!tensor || !oasp_ai_dtype_size(dtype)) {
        oasp_ai_set_error(error, 5, "Cast", "unsupported destination dtype", "Use a documented OpenASP.AI dtype.");
        return NULL;
    }
    oasp_ai_tensor_desc desc = tensor->desc;
    desc.dtype = dtype;
    oasp_ai_tensor *result = oasp_ai_tensor_create(tensor->context, &desc, NULL, 0, error);
    if (!result)
        return NULL;
    for (uint64_t index = 0; index < tensor->element_count; index++)
        oasp_ai_tensor_write(result, index, oasp_ai_tensor_read(tensor, index));
    oasp_ai_tensor *inputs[] = {tensor};
    if (oasp_ai_attach_grad(result, OASP_AI_OP_CAST, inputs, 1, 0, -1, error)) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    return result;
}

int oasp_ai_tensor_copy_to_host(const oasp_ai_tensor *tensor, void *data, size_t size, oasp_ai_error *error) {
    if (!tensor || !data || size != oasp_ai_tensor_bytes(tensor)) {
        oasp_ai_set_error(error, 5, "ToBytes", "destination byte length does not match tensor", "Allocate Tensor.Bytes bytes.");
        return -1;
    }
    if ((tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS) &&
        tensor->storage->memory_kind == OASP_AI_MEMORY_DEVICE) {
        oasp_ai_registered_backend *backend = &tensor->context->backends[tensor->storage->backend_index];
        oasp_ai_event ready = 0;
        if (backend->descriptor->api->record_event(backend->compute_stream, &ready, error) ||
            backend->descriptor->api->wait_event(backend->transfer_stream, ready, error))
            return -1;
        oasp_ai_copy_desc copy = {
            .struct_size = sizeof(copy),
            .source = tensor->storage->backend_storage,
            .source_offset = tensor->offset * oasp_ai_dtype_size(tensor->desc.dtype),
            .bytes = size,
            .destination_host = data,
            .source_kind = OASP_AI_MEMORY_DEVICE,
            .destination_kind = OASP_AI_MEMORY_HOST,
        };
        if (backend->descriptor->api->copy(backend->transfer_stream, &copy, NULL, error) ||
            backend->descriptor->api->synchronize(backend->transfer_stream, error))
            return -1;
        tensor->context->metrics.device_to_host_bytes += size;
        tensor->context->metrics.synchronization_count++;
    } else if (tensor->desc.flags & OASP_AI_TENSOR_CONTIGUOUS)
        memcpy(data, oasp_ai_tensor_const_data(tensor), size);
    else {
        size_t element_size = oasp_ai_dtype_size(tensor->desc.dtype);
        for (uint64_t index = 0; index < tensor->element_count; index++) {
            double value = oasp_ai_tensor_read(tensor, index);
            oasp_ai_tensor temporary = *tensor;
            oasp_ai_storage storage = {.data = (unsigned char *)data + index * element_size};
            temporary.storage = &storage;
            temporary.offset = 0;
            temporary.element_count = 1;
            temporary.desc.rank = 1;
            temporary.desc.shape[0] = 1;
            temporary.desc.strides[0] = 1;
            oasp_ai_tensor_write(&temporary, 0, value);
        }
    }
    return 0;
}

int oasp_ai_tensor_backend_index(const oasp_ai_tensor *tensor) {
    return tensor && tensor->storage->memory_kind == OASP_AI_MEMORY_DEVICE ? tensor->storage->backend_index : -1;
}

int oasp_ai_tensor_device(const oasp_ai_tensor *tensor, char *device, size_t capacity) {
    if (!tensor || !device || !capacity)
        return -1;
    int32_t backend_index = oasp_ai_tensor_backend_index(tensor);
    int written = backend_index < 0
                      ? snprintf(device, capacity, "cpu:0")
                      : snprintf(device, capacity, "%s:%u", tensor->context->backends[backend_index].descriptor->backend_name,
                                 tensor->context->backends[backend_index].ordinal);
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

oasp_ai_tensor *oasp_ai_tensor_to_device(oasp_ai_tensor *tensor, const char *device, oasp_ai_error *error) {
    if (!tensor || !device)
        return NULL;
    int32_t destination = oasp_ai_backend_find(tensor->context, device, error);
    int32_t source = oasp_ai_tensor_backend_index(tensor);
    if (destination == source) {
        oasp_ai_tensor_retain(tensor);
        return tensor;
    }
    oasp_ai_tensor *contiguous = oasp_ai_tensor_contiguous(tensor, error);
    if (!contiguous)
        return NULL;
    size_t bytes = (size_t)oasp_ai_tensor_bytes(contiguous);
    oasp_ai_tensor *result = destination < 0
                                 ? oasp_ai_tensor_create(tensor->context, &contiguous->desc, NULL, 0, error)
                                 : oasp_ai_tensor_create_on_backend(tensor->context, &contiguous->desc, destination, error);
    unsigned char *staging = NULL;
    int failed = !result;
    if (!failed && source < 0 && destination >= 0) {
        oasp_ai_registered_backend *backend = &tensor->context->backends[destination];
        oasp_ai_copy_desc copy = {
            .struct_size = sizeof(copy),
            .destination = result->storage->backend_storage,
            .bytes = bytes,
            .source_host = oasp_ai_tensor_const_data(contiguous),
            .source_kind = OASP_AI_MEMORY_HOST,
            .destination_kind = OASP_AI_MEMORY_DEVICE,
        };
        oasp_ai_event copied = 0;
        failed = backend->descriptor->api->copy(backend->transfer_stream, &copy, &copied, error) ||
                 backend->descriptor->api->wait_event(backend->compute_stream, copied, error);
        tensor->context->metrics.host_to_device_bytes += failed ? 0 : bytes;
    } else if (!failed && source >= 0 && destination < 0)
        failed = oasp_ai_tensor_copy_to_host(contiguous, oasp_ai_tensor_data(result), bytes, error);
    else if (!failed) {
        staging = malloc(bytes);
        failed = !staging || oasp_ai_tensor_copy_to_host(contiguous, staging, bytes, error);
        if (!failed) {
            oasp_ai_registered_backend *backend = &tensor->context->backends[destination];
            oasp_ai_copy_desc copy = {
                .struct_size = sizeof(copy),
                .destination = result->storage->backend_storage,
                .bytes = bytes,
                .source_host = staging,
                .source_kind = OASP_AI_MEMORY_HOST,
                .destination_kind = OASP_AI_MEMORY_DEVICE,
            };
            oasp_ai_event copied = 0;
            failed = backend->descriptor->api->copy(backend->transfer_stream, &copy, &copied, error) ||
                     backend->descriptor->api->wait_event(backend->compute_stream, copied, error);
            tensor->context->metrics.host_to_device_bytes += failed ? 0 : bytes;
        }
    }
    free(staging);
    oasp_ai_tensor_release(contiguous);
    if (failed) {
        oasp_ai_tensor_release(result);
        return NULL;
    }
    result->desc.flags = tensor->desc.flags | OASP_AI_TENSOR_CONTIGUOUS;
    return result;
}

int oasp_ai_tensor_view_build(const oasp_ai_tensor *tensor, oasp_ai_tensor_view *view) {
    if (!tensor || !view || tensor->storage->memory_kind != OASP_AI_MEMORY_DEVICE)
        return -1;
    *view = (oasp_ai_tensor_view){
        .struct_size = sizeof(*view),
        .storage = tensor->storage->backend_storage,
        .byte_offset = tensor->offset * oasp_ai_dtype_size(tensor->desc.dtype),
        .element_count = tensor->element_count,
        .dtype = tensor->desc.dtype,
        .rank = tensor->desc.rank,
        .flags = tensor->desc.flags,
    };
    memcpy(view->shape, tensor->desc.shape, tensor->desc.rank * sizeof(*view->shape));
    memcpy(view->strides, tensor->desc.strides, tensor->desc.rank * sizeof(*view->strides));
    return 0;
}

int oasp_ai_tensor_requires_grad(oasp_ai_tensor *tensor, int enabled, oasp_ai_error *error) {
    if (!tensor || tensor->desc.dtype < OASP_AI_DTYPE_F16) {
        oasp_ai_set_error(error, 5, "RequiresGrad", "gradients require a floating-point tensor", "Cast the tensor to float32 or float64.");
        return -1;
    }
    if (enabled)
        tensor->desc.flags |= OASP_AI_TENSOR_REQUIRES_GRAD;
    else
        tensor->desc.flags &= ~OASP_AI_TENSOR_REQUIRES_GRAD;
    return 0;
}

oasp_ai_tensor *oasp_ai_tensor_gradient(const oasp_ai_tensor *tensor) {
    if (!tensor || !tensor->grad)
        return NULL;
    oasp_ai_tensor_retain(tensor->grad);
    return tensor->grad;
}

int oasp_ai_tensor_zero_grad(oasp_ai_tensor *tensor, oasp_ai_error *error) {
    (void)error;
    if (!tensor)
        return -1;
    oasp_ai_tensor_release(tensor->grad);
    tensor->grad = NULL;
    return 0;
}

oasp_ai_tensor *oasp_ai_tensor_detach(oasp_ai_tensor *tensor, oasp_ai_error *error) {
    if (!tensor)
        return NULL;
    oasp_ai_tensor *result = tensor_view(tensor, error);
    if (result)
        result->desc.flags &= ~OASP_AI_TENSOR_REQUIRES_GRAD;
    return result;
}
