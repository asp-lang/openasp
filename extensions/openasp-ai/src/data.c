// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_be32(FILE *file, int *failed) {
    unsigned char data[4];
    if (fread(data, 1, sizeof(data), file) != sizeof(data)) {
        *failed = 1;
        return 0;
    }
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

static int copy_dataset_tensor(oasp_ai_tensor *tensor, unsigned char **data, size_t *sample_bytes, oasp_ai_error *error) {
    if (!tensor || tensor->desc.rank < 1 || tensor->desc.shape[0] <= 0) {
        oasp_ai_set_error(error, 5, "TensorDataset", "dataset tensors require a positive sample dimension",
                          "Use tensors shaped [samples, ...].");
        return -1;
    }
    uint64_t bytes = oasp_ai_tensor_bytes(tensor);
    if (bytes > SIZE_MAX) {
        oasp_ai_set_error(error, 7, "TensorDataset", "dataset tensor is too large", "Use a smaller or streamed dataset.");
        return -1;
    }
    *data = malloc((size_t)bytes);
    if (!*data || oasp_ai_tensor_copy_to_host(tensor, *data, (size_t)bytes, error)) {
        free(*data);
        *data = NULL;
        return -1;
    }
    *sample_bytes = (size_t)(bytes / (uint64_t)tensor->desc.shape[0]);
    return 0;
}

oasp_ai_dataset *oasp_ai_dataset_from_tensors(oasp_ai_tensor *inputs, oasp_ai_tensor *labels, oasp_ai_error *error) {
    if (!inputs || !labels || inputs->context != labels->context || inputs->desc.rank < 1 || labels->desc.rank < 1 ||
        inputs->desc.shape[0] != labels->desc.shape[0]) {
        oasp_ai_set_error(error, 5, "TensorDataset", "inputs and labels must have the same positive sample count",
                          "Use tensors shaped [samples, ...] from the same AI context.");
        return NULL;
    }
    oasp_ai_dataset *dataset = calloc(1, sizeof(*dataset));
    if (!dataset)
        return NULL;
    atomic_init(&dataset->references, 1);
    dataset->context = inputs->context;
    dataset->input_desc = inputs->desc;
    dataset->label_desc = labels->desc;
    dataset->sample_count = (uint64_t)inputs->desc.shape[0];
    if (copy_dataset_tensor(inputs, &dataset->inputs, &dataset->input_sample_bytes, error) ||
        copy_dataset_tensor(labels, &dataset->labels, &dataset->label_sample_bytes, error)) {
        oasp_ai_dataset_release(dataset);
        return NULL;
    }
    return dataset;
}

oasp_ai_dataset *oasp_ai_dataset_mnist(oasp_ai_context *context, const char *images_path, const char *labels_path, int normalize, oasp_ai_error *error) {
    FILE *images = context && images_path ? fopen(images_path, "rb") : NULL;
    FILE *labels = context && labels_path ? fopen(labels_path, "rb") : NULL;
    int failed = !images || !labels;
    uint32_t image_magic = failed ? 0 : read_be32(images, &failed);
    uint32_t image_count = failed ? 0 : read_be32(images, &failed);
    uint32_t rows = failed ? 0 : read_be32(images, &failed);
    uint32_t columns = failed ? 0 : read_be32(images, &failed);
    uint32_t label_magic = failed ? 0 : read_be32(labels, &failed);
    uint32_t label_count = failed ? 0 : read_be32(labels, &failed);
    uint64_t pixels = (uint64_t)rows * columns;
    if (failed || image_magic != 2051 || label_magic != 2049 || !image_count || image_count != label_count || !rows || !columns ||
        pixels > SIZE_MAX / image_count || pixels * image_count > context->options.maximum_model_bytes) {
        if (images)
            fclose(images);
        if (labels)
            fclose(labels);
        oasp_ai_set_error(error, 5, "MNIST", "invalid, mismatched, or oversized IDX files",
                          "Pass intact IDX3 image and IDX1 label files within the model byte limit.");
        return NULL;
    }
    size_t raw_size = (size_t)(pixels * image_count);
    unsigned char *raw = malloc(raw_size);
    unsigned char *label_data = malloc(image_count);
    oasp_ai_dataset *dataset = calloc(1, sizeof(*dataset));
    if (!raw || !label_data || !dataset || fread(raw, 1, raw_size, images) != raw_size ||
        fread(label_data, 1, image_count, labels) != image_count) {
        free(raw);
        free(label_data);
        free(dataset);
        fclose(images);
        fclose(labels);
        oasp_ai_set_error(error, errno ? errno : 5, "MNIST", "unable to read complete IDX data", "Check the dataset files and memory limit.");
        return NULL;
    }
    fclose(images);
    fclose(labels);
    atomic_init(&dataset->references, 1);
    dataset->context = context;
    dataset->sample_count = image_count;
    dataset->label_desc = (oasp_ai_tensor_desc){.dtype = OASP_AI_DTYPE_I64, .rank = 1, .shape = {(int64_t)image_count}};
    dataset->label_sample_bytes = sizeof(int64_t);
    dataset->labels = malloc((size_t)image_count * sizeof(int64_t));
    dataset->input_desc =
        (oasp_ai_tensor_desc){.dtype = normalize ? OASP_AI_DTYPE_F32 : OASP_AI_DTYPE_U8,
                              .rank = 3,
                              .shape = {(int64_t)image_count, (int64_t)rows, (int64_t)columns}};
    dataset->input_sample_bytes = normalize ? (size_t)pixels * sizeof(float) : (size_t)pixels;
    dataset->inputs = normalize ? malloc(raw_size * sizeof(float)) : raw;
    if (!dataset->labels || !dataset->inputs) {
        free(raw);
        free(label_data);
        oasp_ai_dataset_release(dataset);
        return NULL;
    }
    for (uint32_t index = 0; index < image_count; index++)
        ((int64_t *)dataset->labels)[index] = label_data[index];
    if (normalize) {
        for (size_t index = 0; index < raw_size; index++)
            ((float *)dataset->inputs)[index] = raw[index] / 255.0f;
        free(raw);
    }
    free(label_data);
    return dataset;
}

void oasp_ai_dataset_retain(oasp_ai_dataset *dataset) {
    if (dataset)
        atomic_fetch_add_explicit(&dataset->references, 1, memory_order_relaxed);
}

void oasp_ai_dataset_release(oasp_ai_dataset *dataset) {
    if (!dataset || atomic_fetch_sub_explicit(&dataset->references, 1, memory_order_acq_rel) != 1)
        return;
    free(dataset->inputs);
    free(dataset->labels);
    free(dataset);
}

uint64_t oasp_ai_dataset_size(const oasp_ai_dataset *dataset) {
    return dataset ? dataset->sample_count : 0;
}

oasp_ai_dataloader *oasp_ai_dataloader_create(oasp_ai_dataset *dataset, const oasp_ai_dataloader_options *options, oasp_ai_error *error) {
    if (!dataset || !options || !options->batch_size || options->batch_size > dataset->sample_count || options->workers > 64 ||
        options->prefetch_batches > 1024) {
        oasp_ai_set_error(error, 5, "DataLoader", "invalid DataLoader options",
                          "Use a positive batch size no larger than the dataset and bounded worker/prefetch counts.");
        return NULL;
    }
    oasp_ai_dataloader *loader = calloc(1, sizeof(*loader));
    if (!loader)
        return NULL;
    atomic_init(&loader->references, 1);
    loader->dataset = dataset;
    loader->options = *options;
    if (!loader->options.prefetch_batches)
        loader->options.prefetch_batches = 2;
    if (!loader->options.seed)
        loader->options.seed = dataset->context->options.seed;
    oasp_ai_dataset_retain(dataset);
    return loader;
}

void oasp_ai_dataloader_release(oasp_ai_dataloader *loader) {
    if (!loader || atomic_fetch_sub_explicit(&loader->references, 1, memory_order_acq_rel) != 1)
        return;
    oasp_ai_dataset_release(loader->dataset);
    free(loader);
}

static uint64_t shuffle_next(uint64_t *state) {
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void free_prefetched_batch(oasp_ai_prefetched_batch *batch) {
    if (!batch)
        return;
    free(batch->inputs);
    free(batch->labels);
    free(batch);
}

static oasp_ai_prefetched_batch *prepare_batch(oasp_ai_data_iterator *iterator, uint64_t start) {
    oasp_ai_dataset *dataset = iterator->loader->dataset;
    uint64_t remaining = dataset->sample_count - start;
    uint64_t count = remaining < iterator->loader->options.batch_size ? remaining : iterator->loader->options.batch_size;
    if (count < iterator->loader->options.batch_size && iterator->loader->options.drop_last)
        return NULL;
    if (count > SIZE_MAX / dataset->input_sample_bytes || count > SIZE_MAX / dataset->label_sample_bytes)
        return NULL;
    oasp_ai_prefetched_batch *batch = calloc(1, sizeof(*batch));
    if (batch) {
        batch->inputs = malloc((size_t)count * dataset->input_sample_bytes);
        batch->labels = malloc((size_t)count * dataset->label_sample_bytes);
    }
    if (!batch || !batch->inputs || !batch->labels) {
        free_prefetched_batch(batch);
        return NULL;
    }
    batch->start = start;
    batch->count = count;
    for (uint64_t index = 0; index < count; index++) {
        uint64_t source = iterator->order[start + index];
        memcpy(batch->inputs + index * dataset->input_sample_bytes, dataset->inputs + source * dataset->input_sample_bytes,
               dataset->input_sample_bytes);
        memcpy(batch->labels + index * dataset->label_sample_bytes, dataset->labels + source * dataset->label_sample_bytes,
               dataset->label_sample_bytes);
    }
    return batch;
}

static void *prefetch_worker(void *opaque) {
    oasp_ai_data_iterator *iterator = opaque;
    oasp_ai_dataset *dataset = iterator->loader->dataset;
    for (;;) {
        pthread_mutex_lock(&iterator->mutex);
        while (!iterator->stopping && iterator->produced_cursor < dataset->sample_count &&
               iterator->queued >= iterator->loader->options.prefetch_batches)
            pthread_cond_wait(&iterator->space, &iterator->mutex);
        if (iterator->stopping || iterator->produced_cursor >= dataset->sample_count) {
            pthread_mutex_unlock(&iterator->mutex);
            break;
        }
        uint64_t start = iterator->produced_cursor;
        uint64_t remaining = dataset->sample_count - start;
        uint64_t count = remaining < iterator->loader->options.batch_size ? remaining : iterator->loader->options.batch_size;
        if (count < iterator->loader->options.batch_size && iterator->loader->options.drop_last) {
            iterator->produced_cursor = dataset->sample_count;
            pthread_mutex_unlock(&iterator->mutex);
            break;
        }
        iterator->produced_cursor += count;
        iterator->queued++;
        pthread_mutex_unlock(&iterator->mutex);
        oasp_ai_prefetched_batch *batch = prepare_batch(iterator, start);
        pthread_mutex_lock(&iterator->mutex);
        if (iterator->stopping) {
            iterator->queued--;
            pthread_mutex_unlock(&iterator->mutex);
            free_prefetched_batch(batch);
            break;
        }
        if (!batch) {
            iterator->failed = 1;
            iterator->queued--;
            pthread_cond_broadcast(&iterator->ready);
            pthread_mutex_unlock(&iterator->mutex);
            break;
        }
        oasp_ai_prefetched_batch **position = &iterator->head;
        while (*position && (*position)->start < batch->start)
            position = &(*position)->next;
        batch->next = *position;
        *position = batch;
        if (!batch->next)
            iterator->tail = batch;
        if (!iterator->tail)
            iterator->tail = batch;
        if (iterator->head == batch && !batch->next)
            iterator->head = batch;
        pthread_cond_broadcast(&iterator->ready);
        pthread_mutex_unlock(&iterator->mutex);
    }
    pthread_mutex_lock(&iterator->mutex);
    if (iterator->live_workers)
        iterator->live_workers--;
    iterator->finished = iterator->live_workers == 0;
    pthread_cond_broadcast(&iterator->ready);
    pthread_mutex_unlock(&iterator->mutex);
    return NULL;
}

oasp_ai_data_iterator *oasp_ai_dataloader_iterator(oasp_ai_dataloader *loader, uint64_t epoch, oasp_ai_error *error) {
    if (!loader)
        return NULL;
    oasp_ai_data_iterator *iterator = calloc(1, sizeof(*iterator));
    uint64_t count = loader->dataset->sample_count;
    if (iterator)
        iterator->order = malloc((size_t)count * sizeof(*iterator->order));
    uint32_t worker_count = loader->options.workers ? loader->options.workers : 1;
    if (iterator)
        iterator->workers = calloc(worker_count, sizeof(*iterator->workers));
    if (!iterator || !iterator->order || !iterator->workers) {
        if (iterator)
            free(iterator->order);
        free(iterator);
        return NULL;
    }
    iterator->loader = loader;
    iterator->epoch = epoch;
    atomic_fetch_add_explicit(&loader->references, 1, memory_order_relaxed);
    for (uint64_t index = 0; index < count; index++)
        iterator->order[index] = index;
    if (loader->options.shuffle) {
        uint64_t state = loader->options.seed ^ (epoch * UINT64_C(0xd1342543de82ef95));
        for (uint64_t index = count; index > 1; index--) {
            uint64_t selected = shuffle_next(&state) % index;
            uint64_t temporary = iterator->order[index - 1];
            iterator->order[index - 1] = iterator->order[selected];
            iterator->order[selected] = temporary;
        }
    }
    if (!pthread_mutex_init(&iterator->mutex, NULL))
        iterator->mutex_initialized = 1;
    if (iterator->mutex_initialized && !pthread_cond_init(&iterator->ready, NULL))
        iterator->ready_initialized = 1;
    if (iterator->ready_initialized && !pthread_cond_init(&iterator->space, NULL))
        iterator->space_initialized = 1;
    if (!iterator->space_initialized) {
        oasp_ai_set_error(error, 7, "DataLoader.Iterator", "unable to start the prefetch worker", "Reduce worker pressure or process limits.");
        oasp_ai_data_iterator_release(iterator);
        return NULL;
    }
    iterator->live_workers = worker_count;
    for (uint32_t index = 0; index < worker_count; index++) {
        if (pthread_create(&iterator->workers[index], NULL, prefetch_worker, iterator)) {
            pthread_mutex_lock(&iterator->mutex);
            iterator->live_workers = iterator->worker_count;
            iterator->stopping = 1;
            pthread_cond_broadcast(&iterator->space);
            pthread_mutex_unlock(&iterator->mutex);
            oasp_ai_set_error(error, 7, "DataLoader.Iterator", "unable to start all prefetch workers",
                              "Reduce the DataLoader worker count or process thread pressure.");
            oasp_ai_data_iterator_release(iterator);
            return NULL;
        }
        iterator->worker_count++;
    }
    return iterator;
}

void oasp_ai_data_iterator_release(oasp_ai_data_iterator *iterator) {
    if (!iterator)
        return;
    if (iterator->worker_count) {
        pthread_mutex_lock(&iterator->mutex);
        iterator->stopping = 1;
        pthread_cond_broadcast(&iterator->space);
        pthread_mutex_unlock(&iterator->mutex);
        for (uint32_t index = 0; index < iterator->worker_count; index++)
            pthread_join(iterator->workers[index], NULL);
    }
    oasp_ai_prefetched_batch *batch = iterator->head;
    while (batch) {
        oasp_ai_prefetched_batch *next = batch->next;
        free_prefetched_batch(batch);
        batch = next;
    }
    if (iterator->space_initialized)
        pthread_cond_destroy(&iterator->space);
    if (iterator->ready_initialized)
        pthread_cond_destroy(&iterator->ready);
    if (iterator->mutex_initialized)
        pthread_mutex_destroy(&iterator->mutex);
    free(iterator->workers);
    free(iterator->order);
    oasp_ai_dataloader_release(iterator->loader);
    free(iterator);
}

int oasp_ai_data_iterator_next(oasp_ai_data_iterator *iterator, oasp_ai_batch **result, oasp_ai_error *error) {
    if (!iterator || !result)
        return -1;
    uint64_t started = oasp_ai_now_nanoseconds();
    pthread_mutex_lock(&iterator->mutex);
    while ((!iterator->head || iterator->head->start != iterator->cursor) && !iterator->finished && !iterator->failed)
        pthread_cond_wait(&iterator->ready, &iterator->mutex);
    oasp_ai_prefetched_batch *raw = iterator->head && iterator->head->start == iterator->cursor ? iterator->head : NULL;
    if (raw) {
        iterator->head = raw->next;
        if (!iterator->head)
            iterator->tail = NULL;
        iterator->queued--;
        pthread_cond_signal(&iterator->space);
    }
    int failed = iterator->failed;
    pthread_mutex_unlock(&iterator->mutex);
    iterator->loader->dataset->context->metrics.dataloader_wait_nanoseconds += oasp_ai_now_nanoseconds() - started;
    if (!raw) {
        *result = NULL;
        if (failed)
            oasp_ai_set_error(error, 7, "DataLoader.Next", "the prefetch worker could not allocate a batch", "Reduce batch or prefetch size.");
        return failed ? -1 : 0;
    }
    oasp_ai_dataset *dataset = iterator->loader->dataset;
    oasp_ai_tensor_desc input_desc = dataset->input_desc;
    oasp_ai_tensor_desc label_desc = dataset->label_desc;
    input_desc.shape[0] = (int64_t)raw->count;
    label_desc.shape[0] = (int64_t)raw->count;
    oasp_ai_batch *batch = calloc(1, sizeof(*batch));
    if (batch) {
        atomic_init(&batch->references, 1);
        batch->inputs = oasp_ai_tensor_create(dataset->context, &input_desc, raw->inputs,
                                              (size_t)raw->count * dataset->input_sample_bytes, error);
    }
    if (batch && batch->inputs)
        batch->labels = oasp_ai_tensor_create(dataset->context, &label_desc, raw->labels,
                                              (size_t)raw->count * dataset->label_sample_bytes, error);
    if (!batch || !batch->inputs || !batch->labels) {
        oasp_ai_batch_release(batch);
        free_prefetched_batch(raw);
        return -1;
    }
    batch->size = raw->count;
    iterator->cursor += raw->count;
    free_prefetched_batch(raw);
    *result = batch;
    return 1;
}

uint64_t oasp_ai_data_iterator_cursor(const oasp_ai_data_iterator *iterator) {
    return iterator ? iterator->cursor : 0;
}

void oasp_ai_batch_retain(oasp_ai_batch *batch) {
    if (batch)
        atomic_fetch_add_explicit(&batch->references, 1, memory_order_relaxed);
}

void oasp_ai_batch_release(oasp_ai_batch *batch) {
    if (!batch || atomic_fetch_sub_explicit(&batch->references, 1, memory_order_acq_rel) != 1)
        return;
    oasp_ai_tensor_release(batch->inputs);
    oasp_ai_tensor_release(batch->labels);
    free(batch);
}

oasp_ai_tensor *oasp_ai_batch_inputs(oasp_ai_batch *batch) {
    if (batch && batch->inputs)
        oasp_ai_tensor_retain(batch->inputs);
    return batch ? batch->inputs : NULL;
}

oasp_ai_tensor *oasp_ai_batch_labels(oasp_ai_batch *batch) {
    if (batch && batch->labels)
        oasp_ai_tensor_retain(batch->labels);
    return batch ? batch->labels : NULL;
}

uint64_t oasp_ai_batch_size(const oasp_ai_batch *batch) {
    return batch ? batch->size : 0;
}

oasp_ai_tokenizer *oasp_ai_tokenizer_load(const char *path, int add_unknown_token, oasp_ai_error *error) {
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
        oasp_ai_set_error(error, 2, "Tokenizer", "unable to open vocabulary", "Check the vocabulary path and permissions.");
        return NULL;
    }
    oasp_ai_tokenizer *tokenizer = calloc(1, sizeof(*tokenizer));
    char line[65536];
    size_t capacity = 0;
    if (add_unknown_token) {
        tokenizer->tokens = calloc(1, sizeof(*tokenizer->tokens));
        tokenizer->lengths = calloc(1, sizeof(*tokenizer->lengths));
        if (tokenizer->tokens)
            tokenizer->tokens[0] = oasp_ai_string_duplicate("<unk>");
        if (!tokenizer->tokens || !tokenizer->lengths || !tokenizer->tokens[0])
            goto failure;
        tokenizer->lengths[0] = 5;
        tokenizer->count = capacity = 1;
        tokenizer->unknown_token = 0;
    } else
        tokenizer->unknown_token = -1;
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (!length)
            continue;
        char **tokens = oasp_ai_grow_array(tokenizer->tokens, sizeof(*tokens), &capacity, tokenizer->count + 1);
        if (!tokens)
            goto failure;
        tokenizer->tokens = tokens;
        size_t lengths_capacity = capacity;
        size_t *lengths = realloc(tokenizer->lengths, lengths_capacity * sizeof(*lengths));
        if (!lengths)
            goto failure;
        tokenizer->lengths = lengths;
        tokenizer->tokens[tokenizer->count] = oasp_ai_string_duplicate(line);
        if (!tokenizer->tokens[tokenizer->count])
            goto failure;
        tokenizer->lengths[tokenizer->count] = length;
        tokenizer->count++;
    }
    fclose(file);
    if (!tokenizer->count) {
        oasp_ai_set_error(error, 5, "Tokenizer", "vocabulary is empty", "Provide one UTF-8 token per line.");
        oasp_ai_tokenizer_release(tokenizer);
        return NULL;
    }
    return tokenizer;

failure:
    fclose(file);
    oasp_ai_tokenizer_release(tokenizer);
    return NULL;
}

void oasp_ai_tokenizer_release(oasp_ai_tokenizer *tokenizer) {
    if (!tokenizer)
        return;
    for (size_t index = 0; index < tokenizer->count; index++)
        free(tokenizer->tokens[index]);
    free(tokenizer->tokens);
    free(tokenizer->lengths);
    free(tokenizer);
}

int oasp_ai_tokenizer_encode(oasp_ai_tokenizer *tokenizer, const void *text, size_t text_size, int64_t **tokens, size_t *token_count,
                             oasp_ai_error *error) {
    if (!tokenizer || (!text && text_size) || !tokens || !token_count)
        return -1;
    const unsigned char *bytes = text;
    int64_t *result = text_size ? malloc(text_size * sizeof(*result)) : NULL;
    size_t count = 0;
    for (size_t offset = 0; offset < text_size;) {
        size_t best = SIZE_MAX;
        size_t best_length = 0;
        for (size_t index = 0; index < tokenizer->count; index++)
            if (tokenizer->lengths[index] > best_length && tokenizer->lengths[index] <= text_size - offset &&
                !memcmp(bytes + offset, tokenizer->tokens[index], tokenizer->lengths[index])) {
                best = index;
                best_length = tokenizer->lengths[index];
            }
        if (best == SIZE_MAX) {
            if (tokenizer->unknown_token < 0) {
                free(result);
                oasp_ai_set_error(error, 5, "Tokenizer.Encode", "input contains bytes absent from the vocabulary",
                                  "Add matching tokens or enable the unknown token.");
                return -1;
            }
            result[count++] = tokenizer->unknown_token;
            offset++;
        } else {
            result[count++] = (int64_t)best;
            offset += best_length;
        }
    }
    *tokens = result;
    *token_count = count;
    return 0;
}

int oasp_ai_tokenizer_decode(oasp_ai_tokenizer *tokenizer, const int64_t *tokens, size_t token_count, char **text, size_t *text_size,
                             oasp_ai_error *error) {
    if (!tokenizer || (!tokens && token_count) || !text || !text_size)
        return -1;
    size_t total = 0;
    for (size_t index = 0; index < token_count; index++) {
        if (tokens[index] < 0 || (uint64_t)tokens[index] >= tokenizer->count || tokenizer->lengths[tokens[index]] > SIZE_MAX - total) {
            oasp_ai_set_error(error, 5, "Tokenizer.Decode", "token ID is outside the vocabulary", "Validate generated token IDs.");
            return -1;
        }
        total += tokenizer->lengths[tokens[index]];
    }
    char *result = malloc(total + 1);
    if (!result)
        return -1;
    size_t offset = 0;
    for (size_t index = 0; index < token_count; index++) {
        size_t length = tokenizer->lengths[tokens[index]];
        memcpy(result + offset, tokenizer->tokens[tokens[index]], length);
        offset += length;
    }
    result[total] = '\0';
    *text = result;
    *text_size = total;
    return 0;
}
