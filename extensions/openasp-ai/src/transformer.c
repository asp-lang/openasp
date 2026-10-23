// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

oasp_ai_kv_cache *oasp_ai_kv_cache_create(oasp_ai_context *context, uint32_t layers, uint32_t batch, uint32_t heads, uint32_t head_dimension,
                                          uint32_t page_tokens, oasp_ai_dtype dtype, oasp_ai_error *error) {
    if (!context || !layers || !batch || !heads || !head_dimension || !page_tokens || dtype < OASP_AI_DTYPE_F16) {
        oasp_ai_set_error(error, 5, "KVCache", "invalid KV cache configuration", "Use positive dimensions and a floating-point dtype.");
        return NULL;
    }
    oasp_ai_kv_cache *cache = calloc(1, sizeof(*cache));
    if (cache)
        cache->layers = calloc(layers, sizeof(*cache->layers));
    if (!cache || !cache->layers) {
        free(cache);
        return NULL;
    }
    atomic_init(&cache->references, 1);
    cache->context = context;
    cache->layer_count = layers;
    cache->batch = batch;
    cache->heads = heads;
    cache->head_dimension = head_dimension;
    cache->page_tokens = page_tokens;
    cache->dtype = dtype;
    return cache;
}

void oasp_ai_kv_cache_release(oasp_ai_kv_cache *cache) {
    if (!cache || atomic_fetch_sub_explicit(&cache->references, 1, memory_order_acq_rel) != 1)
        return;
    for (uint32_t layer = 0; layer < cache->layer_count; layer++) {
        oasp_ai_tensor_release(cache->layers[layer].keys);
        oasp_ai_tensor_release(cache->layers[layer].values);
    }
    free(cache->layers);
    free(cache);
}

static oasp_ai_tensor *append_sequence(oasp_ai_kv_cache *cache, oasp_ai_tensor *previous, oasp_ai_tensor *addition, oasp_ai_error *error) {
    const oasp_ai_tensor_desc *desc = &addition->desc;
    if (desc->rank != 4 || desc->shape[0] != cache->batch || desc->shape[1] != cache->heads || desc->shape[3] != cache->head_dimension ||
        desc->dtype != cache->dtype) {
        oasp_ai_set_error(error, 5, "KVCacheAppend", "KV tensor shape or dtype does not match the cache", "Use [batch, heads, tokens, head_dimension].");
        return NULL;
    }
    if (!previous)
        return oasp_ai_tensor_clone(addition, error);
    oasp_ai_tensor_desc combined = *desc;
    combined.shape[2] += previous->desc.shape[2];
    oasp_ai_tensor *result = oasp_ai_tensor_create(cache->context, &combined, NULL, 0, error);
    if (!result)
        return NULL;
    uint64_t old_tokens = (uint64_t)previous->desc.shape[2];
    uint64_t new_tokens = (uint64_t)addition->desc.shape[2];
    uint64_t width = cache->head_dimension;
    for (uint64_t batch = 0; batch < cache->batch; batch++)
        for (uint64_t head = 0; head < cache->heads; head++) {
            uint64_t output_base = (batch * cache->heads + head) * (old_tokens + new_tokens) * width;
            uint64_t old_base = (batch * cache->heads + head) * old_tokens * width;
            uint64_t new_base = (batch * cache->heads + head) * new_tokens * width;
            for (uint64_t index = 0; index < old_tokens * width; index++)
                oasp_ai_tensor_write(result, output_base + index, oasp_ai_tensor_read(previous, old_base + index));
            for (uint64_t index = 0; index < new_tokens * width; index++)
                oasp_ai_tensor_write(result, output_base + old_tokens * width + index, oasp_ai_tensor_read(addition, new_base + index));
        }
    return result;
}

int oasp_ai_kv_cache_append(oasp_ai_kv_cache *cache, uint32_t layer, oasp_ai_tensor *keys, oasp_ai_tensor *values, oasp_ai_error *error) {
    if (!cache || layer >= cache->layer_count || !keys || !values || keys->desc.shape[2] != values->desc.shape[2]) {
        oasp_ai_set_error(error, 5, "KVCacheAppend", "invalid layer or mismatched K/V tensors", "Use a valid layer and equal token counts.");
        return -1;
    }
    oasp_ai_tensor *new_keys = append_sequence(cache, cache->layers[layer].keys, keys, error);
    oasp_ai_tensor *new_values = append_sequence(cache, cache->layers[layer].values, values, error);
    if (!new_keys || !new_values) {
        oasp_ai_tensor_release(new_keys);
        oasp_ai_tensor_release(new_values);
        return -1;
    }
    oasp_ai_tensor_release(cache->layers[layer].keys);
    oasp_ai_tensor_release(cache->layers[layer].values);
    cache->layers[layer].keys = new_keys;
    cache->layers[layer].values = new_values;
    uint64_t length = (uint64_t)new_keys->desc.shape[2];
    if (length > cache->length)
        cache->length = length;
    return 0;
}

uint64_t oasp_ai_kv_cache_length(const oasp_ai_kv_cache *cache) {
    return cache ? cache->length : 0;
}

typedef struct {
    uint64_t index;
    double probability;
} sample_item;

static int compare_probability(const void *left, const void *right) {
    double first = ((const sample_item *)left)->probability;
    double second = ((const sample_item *)right)->probability;
    return first < second ? 1 : first > second ? -1 : 0;
}

static uint64_t sample_random(oasp_ai_context *context) {
    uint64_t value = context->options.seed + UINT64_C(0x9e3779b97f4a7c15) * ++context->rng_counter;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

int oasp_ai_sample(oasp_ai_context *context, oasp_ai_tensor *logits, double temperature, uint32_t top_k, double top_p,
                   const int64_t *history, size_t history_count, double repetition_penalty, int64_t *token, oasp_ai_error *error) {
    if (!context || !logits || !token || logits->desc.rank < 1 || temperature < 0 || top_p < 0 || top_p > 1 || repetition_penalty <= 0) {
        oasp_ai_set_error(error, 5, "Sample", "invalid sampling configuration", "Use temperature >= 0, top_p in [0,1], and positive repetition penalty.");
        return -1;
    }
    uint64_t vocabulary = (uint64_t)logits->desc.shape[logits->desc.rank - 1];
    uint64_t base = logits->element_count - vocabulary;
    sample_item *items = calloc((size_t)vocabulary, sizeof(*items));
    if (!items)
        return -1;
    double maximum = -INFINITY;
    for (uint64_t index = 0; index < vocabulary; index++) {
        double value = oasp_ai_tensor_read(logits, base + index);
        for (size_t seen = 0; seen < history_count; seen++)
            if (history[seen] == (int64_t)index) {
                value = value >= 0 ? value / repetition_penalty : value * repetition_penalty;
                break;
            }
        items[index] = (sample_item){index, value};
        if (value > maximum)
            maximum = value;
    }
    if (temperature == 0) {
        for (uint64_t index = 0; index < vocabulary; index++)
            if (items[index].probability == maximum) {
                *token = (int64_t)index;
                free(items);
                return 0;
            }
    }
    double total = 0;
    for (uint64_t index = 0; index < vocabulary; index++) {
        items[index].probability = exp((items[index].probability - maximum) / temperature);
        total += items[index].probability;
    }
    for (uint64_t index = 0; index < vocabulary; index++)
        items[index].probability /= total;
    qsort(items, (size_t)vocabulary, sizeof(*items), compare_probability);
    uint64_t retained = top_k && top_k < vocabulary ? top_k : vocabulary;
    if (top_p > 0 && top_p < 1) {
        double cumulative = 0;
        retained = 0;
        do {
            cumulative += items[retained++].probability;
        } while (retained < vocabulary && cumulative < top_p);
    }
    total = 0;
    for (uint64_t index = 0; index < retained; index++)
        total += items[index].probability;
    double draw = (double)(sample_random(context) >> 11) / 9007199254740992.0 * total;
    uint64_t selected = retained - 1;
    for (uint64_t index = 0; index < retained; index++) {
        draw -= items[index].probability;
        if (draw <= 0) {
            selected = index;
            break;
        }
    }
    *token = (int64_t)items[selected].index;
    free(items);
    return 0;
}
