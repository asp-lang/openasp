// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "ai_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} proto_buffer;

typedef struct {
    const unsigned char *cursor;
    const unsigned char *end;
} proto_reader;

static int reserve(proto_buffer *buffer, size_t size) {
    if (buffer->size > SIZE_MAX - size)
        return -1;
    size_t required = buffer->size + size;
    if (required <= buffer->capacity)
        return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }
    unsigned char *grown = realloc(buffer->data, capacity);
    if (!grown)
        return -1;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

static int raw(proto_buffer *buffer, const void *data, size_t size) {
    if (reserve(buffer, size))
        return -1;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int varint(proto_buffer *buffer, uint64_t value) {
    unsigned char bytes[10];
    size_t count = 0;
    do {
        bytes[count] = (unsigned char)(value & 0x7f);
        value >>= 7;
        if (value)
            bytes[count] |= 0x80;
        count++;
    } while (value);
    return raw(buffer, bytes, count);
}

static int field_varint(proto_buffer *buffer, uint32_t field, uint64_t value) {
    return varint(buffer, (uint64_t)field << 3) || varint(buffer, value);
}

static int field_bytes(proto_buffer *buffer, uint32_t field, const void *data, size_t size) {
    return varint(buffer, ((uint64_t)field << 3) | 2) || varint(buffer, size) || raw(buffer, data, size);
}

static int field_string(proto_buffer *buffer, uint32_t field, const char *value) {
    return field_bytes(buffer, field, value, strlen(value));
}

static int field_message(proto_buffer *buffer, uint32_t field, proto_buffer *message) {
    int status = field_bytes(buffer, field, message->data, message->size);
    free(message->data);
    *message = (proto_buffer){0};
    return status;
}

static int onnx_dtype(oasp_ai_dtype dtype) {
    static const int values[] = {9, 2, 3, 5, 6, 7, 10, 16, 1, 11};
    return dtype <= OASP_AI_DTYPE_F64 ? values[dtype] : 0;
}

static int internal_dtype(uint64_t onnx, oasp_ai_dtype *dtype) {
    for (int value = OASP_AI_DTYPE_BOOL; value <= OASP_AI_DTYPE_F64; value++)
        if ((uint64_t)onnx_dtype((oasp_ai_dtype)value) == onnx) {
            *dtype = (oasp_ai_dtype)value;
            return 0;
        }
    return -1;
}

typedef struct {
    oasp_ai_operator operation;
    const char *name;
    uint32_t minimum_inputs;
    uint32_t maximum_inputs;
} onnx_operator;

static const onnx_operator operators[] = {
    {OASP_AI_OP_ADD, "Add", 2, 2},       {OASP_AI_OP_SUB, "Sub", 2, 2},
    {OASP_AI_OP_MUL, "Mul", 2, 2},       {OASP_AI_OP_DIV, "Div", 2, 2},
    {OASP_AI_OP_POW, "Pow", 2, 2},       {OASP_AI_OP_EXP, "Exp", 1, 1},
    {OASP_AI_OP_LOG, "Log", 1, 1},       {OASP_AI_OP_SQRT, "Sqrt", 1, 1},
    {OASP_AI_OP_MAXIMUM, "Max", 2, 2},   {OASP_AI_OP_RELU, "Relu", 1, 1},
    {OASP_AI_OP_LEAKY_RELU, "LeakyRelu", 1, 1},
    {OASP_AI_OP_SIGMOID, "Sigmoid", 1, 1},
    {OASP_AI_OP_TANH, "Tanh", 1, 1},     {OASP_AI_OP_MATMUL, "MatMul", 2, 2},
    {OASP_AI_OP_SOFTMAX, "Softmax", 1, 1},
    {OASP_AI_OP_GATHER, "Gather", 2, 2},
};

static const onnx_operator *operator_by_id(uint32_t id) {
    for (size_t index = 0; index < sizeof(operators) / sizeof(operators[0]); index++)
        if (operators[index].operation == (oasp_ai_operator)id)
            return &operators[index];
    return NULL;
}

static const onnx_operator *operator_by_name(const char *name) {
    for (size_t index = 0; index < sizeof(operators) / sizeof(operators[0]); index++)
        if (!strcmp(operators[index].name, name))
            return &operators[index];
    return NULL;
}

static int graph_value_info(proto_buffer *graph, uint32_t field, const oasp_ai_ir_value *value) {
    proto_buffer info = {0};
    proto_buffer shape = {0};
    proto_buffer tensor_type = {0};
    proto_buffer type = {0};
    int failed = field_string(&info, 1, value->name);
    for (uint32_t axis = 0; axis < value->desc.rank && !failed; axis++) {
        proto_buffer dimension = {0};
        if (value->desc.shape[axis] == -1) {
            char symbolic[32];
            snprintf(symbolic, sizeof(symbolic), "dim_%u", axis);
            failed = field_string(&dimension, 2, symbolic);
        } else
            failed = field_varint(&dimension, 1, (uint64_t)value->desc.shape[axis]);
        if (!failed)
            failed = field_message(&shape, 1, &dimension);
    }
    if (!failed)
        failed = field_varint(&tensor_type, 1, (uint64_t)onnx_dtype(value->desc.dtype)) ||
                 field_message(&tensor_type, 2, &shape) || field_message(&type, 1, &tensor_type) ||
                 field_message(&info, 2, &type) || field_message(graph, field, &info);
    free(info.data);
    free(shape.data);
    free(tensor_type.data);
    free(type.data);
    return failed;
}

static int initializer(proto_buffer *graph, const oasp_ai_ir_value *value, oasp_ai_error *error) {
    oasp_ai_tensor *tensor = oasp_ai_tensor_contiguous(value->constant, error);
    proto_buffer message = {0};
    proto_buffer dimensions = {0};
    int failed = !tensor;
    for (uint32_t axis = 0; axis < value->desc.rank && !failed; axis++)
        failed = varint(&dimensions, (uint64_t)value->desc.shape[axis]);
    if (!failed)
        failed = field_bytes(&message, 1, dimensions.data, dimensions.size) ||
                 field_varint(&message, 2, (uint64_t)onnx_dtype(value->desc.dtype)) ||
                 field_string(&message, 8, value->name) ||
                 field_bytes(&message, 9, oasp_ai_tensor_const_data(tensor), (size_t)oasp_ai_tensor_bytes(tensor)) ||
                 field_message(graph, 5, &message);
    free(dimensions.data);
    free(message.data);
    oasp_ai_tensor_release(tensor);
    return failed;
}

typedef struct {
    oasp_ai_graph *graph;
} onnx_writer;

static int write_onnx(FILE *file, void *opaque, oasp_ai_error *error) {
    oasp_ai_graph *source = ((onnx_writer *)opaque)->graph;
    proto_buffer graph = {0};
    proto_buffer model = {0};
    int failed = field_string(&graph, 2, source->name);
    for (size_t index = 0; index < source->value_count && !failed; index++)
        if (source->values[index].constant)
            failed = initializer(&graph, &source->values[index], error);
    for (size_t index = 0; index < source->operation_count && !failed; index++) {
        oasp_ai_ir_operation *operation = &source->operations[index];
        const onnx_operator *mapping = operator_by_id(operation->op_id);
        proto_buffer node = {0};
        failed = !mapping || operation->input_count < mapping->minimum_inputs || operation->input_count > mapping->maximum_inputs;
        for (size_t input = 0; input < operation->input_count && !failed; input++)
            failed = field_string(&node, 1, source->values[operation->inputs[input] - 1].name);
        if (!failed)
            failed = field_string(&node, 2, source->values[operation->output - 1].name) ||
                     field_string(&node, 4, mapping->name);
        if (!failed && (operation->op_id == OASP_AI_OP_SOFTMAX || operation->op_id == OASP_AI_OP_GATHER)) {
            proto_buffer attribute = {0};
            int64_t axis = operation->attributes_size == sizeof(int32_t) ? *(int32_t *)operation->attributes :
                           operation->op_id == OASP_AI_OP_SOFTMAX ? -1 : 0;
            failed = field_string(&attribute, 1, "axis") || field_varint(&attribute, 3, (uint64_t)axis) ||
                     field_varint(&attribute, 20, 2) || field_message(&node, 5, &attribute);
        }
        if (!failed && operation->op_id == OASP_AI_OP_LEAKY_RELU) {
            proto_buffer attribute = {0};
            float alpha = operation->attributes_size == sizeof(double) ? (float)*(double *)operation->attributes : 0.01f;
            uint32_t bits;
            memcpy(&bits, &alpha, sizeof(bits));
            failed = field_string(&attribute, 1, "alpha") || varint(&attribute, (2u << 3) | 5u) ||
                     raw(&attribute, &bits, sizeof(bits)) || field_varint(&attribute, 20, 1) ||
                     field_message(&node, 5, &attribute);
        }
        if (!failed)
            failed = field_message(&graph, 1, &node);
        free(node.data);
    }
    for (size_t index = 0; index < source->input_count && !failed; index++)
        failed = graph_value_info(&graph, 11, &source->values[source->inputs[index] - 1]);
    for (size_t index = 0; index < source->output_count && !failed; index++)
        failed = graph_value_info(&graph, 12, &source->values[source->outputs[index] - 1]);
    proto_buffer opset = {0};
    if (!failed)
        failed = field_varint(&model, 1, 9) || field_varint(&opset, 2, 20) || field_message(&model, 8, &opset) ||
                 field_message(&model, 7, &graph);
    if (!failed)
        failed = fwrite(model.data, 1, model.size, file) != model.size;
    free(opset.data);
    free(graph.data);
    free(model.data);
    if (failed)
        oasp_ai_set_error(error, 5, "ExportONNX", "graph uses operators outside the supported ONNX subset",
                          "Decompose the graph into supported standard ONNX operators.");
    return failed ? -1 : 0;
}

int oasp_ai_graph_export_onnx(oasp_ai_graph *graph, const char *path, oasp_ai_error *error) {
    if (oasp_ai_graph_verify(graph, error))
        return -1;
    onnx_writer writer = {graph};
    return oasp_ai_write_atomic(path, write_onnx, &writer, error);
}

static int read_varint(proto_reader *reader, uint64_t *value) {
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (reader->cursor >= reader->end)
            return -1;
        unsigned char byte = *reader->cursor++;
        if (shift == 63 && (byte & 0xfe))
            return -1;
        *value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return 0;
    }
    return -1;
}

static int next_field(proto_reader *reader, uint32_t *field, uint32_t *wire, proto_reader *payload, uint64_t *integer) {
    uint64_t tag;
    if (read_varint(reader, &tag) || !(tag >> 3) || (tag & 7) > 5)
        return -1;
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7);
    if (*wire == 0)
        return read_varint(reader, integer);
    if (*wire == 2) {
        uint64_t size;
        if (read_varint(reader, &size) || size > (uint64_t)(reader->end - reader->cursor))
            return -1;
        *payload = (proto_reader){reader->cursor, reader->cursor + size};
        reader->cursor += size;
        return 0;
    }
    size_t size = *wire == 1 ? 8 : *wire == 5 ? 4 : 0;
    if (!size || (size_t)(reader->end - reader->cursor) < size)
        return -1;
    *payload = (proto_reader){reader->cursor, reader->cursor + size};
    reader->cursor += size;
    return 0;
}

static char *reader_text(proto_reader payload, size_t maximum) {
    size_t size = (size_t)(payload.end - payload.cursor);
    if (size > maximum || memchr(payload.cursor, '\0', size))
        return NULL;
    char *text = malloc(size + 1);
    if (text) {
        memcpy(text, payload.cursor, size);
        text[size] = '\0';
    }
    return text;
}

typedef struct {
    char *name;
    oasp_ai_tensor_desc desc;
    int has_type;
} onnx_value;

typedef struct {
    char *name;
    oasp_ai_tensor *tensor;
} onnx_initializer;

typedef struct {
    char **inputs;
    size_t input_count;
    char *output;
    char *operation;
    int has_axis;
    int64_t axis;
    int has_alpha;
    double alpha;
} onnx_node;

typedef struct {
    char *name;
    onnx_value *inputs;
    size_t input_count;
    onnx_value *outputs;
    size_t output_count;
    onnx_initializer *initializers;
    size_t initializer_count;
    onnx_node *nodes;
    size_t node_count;
} onnx_graph;

static int append_pointer(void **items, size_t element_size, size_t *count) {
    if (*count >= 10000000 || *count + 1 > SIZE_MAX / element_size)
        return -1;
    void *grown = realloc(*items, (*count + 1) * element_size);
    if (!grown)
        return -1;
    *items = grown;
    memset((unsigned char *)grown + *count * element_size, 0, element_size);
    (*count)++;
    return 0;
}

static int parse_dimension(proto_reader reader, int64_t *dimension) {
    uint32_t field;
    uint32_t wire;
    proto_reader payload = {0};
    uint64_t integer = 0;
    *dimension = -1;
    while (reader.cursor < reader.end) {
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field == 1 && wire == 0) {
            if (!integer || integer > INT64_MAX)
                return -1;
            *dimension = (int64_t)integer;
        }
    }
    return 0;
}

static int parse_shape(proto_reader reader, oasp_ai_tensor_desc *desc) {
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field == 1 && wire == 2) {
            if (desc->rank == OASP_AI_MAX_RANK || parse_dimension(payload, &desc->shape[desc->rank]))
                return -1;
            desc->rank++;
        }
    }
    return desc->rank ? 0 : -1;
}

static int parse_type(proto_reader reader, oasp_ai_tensor_desc *desc) {
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field != 1 || wire != 2)
            continue;
        while (payload.cursor < payload.end) {
            uint32_t tensor_field;
            uint32_t tensor_wire;
            proto_reader nested = {0};
            if (next_field(&payload, &tensor_field, &tensor_wire, &nested, &integer))
                return -1;
            if (tensor_field == 1 && tensor_wire == 0 && internal_dtype(integer, &desc->dtype))
                return -1;
            if (tensor_field == 2 && tensor_wire == 2 && parse_shape(nested, desc))
                return -1;
        }
    }
    return desc->rank ? 0 : -1;
}

static int parse_value_info(proto_reader reader, onnx_value *value) {
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field == 1 && wire == 2)
            value->name = reader_text(payload, 4096);
        else if (field == 2 && wire == 2)
            value->has_type = !parse_type(payload, &value->desc);
    }
    return !value->name || !value->has_type ? -1 : 0;
}

static int parse_tensor(proto_reader reader, oasp_ai_context *context, onnx_initializer *initializer, oasp_ai_error *error) {
    oasp_ai_tensor_desc desc = {0};
    const unsigned char *raw_data = NULL;
    size_t raw_size = 0;
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field == 1) {
            if (wire == 0) {
                if (!integer || integer > INT64_MAX || desc.rank == OASP_AI_MAX_RANK)
                    return -1;
                desc.shape[desc.rank++] = (int64_t)integer;
            } else if (wire == 2)
                while (payload.cursor < payload.end) {
                    if (read_varint(&payload, &integer) || !integer || integer > INT64_MAX || desc.rank == OASP_AI_MAX_RANK)
                        return -1;
                    desc.shape[desc.rank++] = (int64_t)integer;
                }
        } else if (field == 2 && wire == 0) {
            if (internal_dtype(integer, &desc.dtype))
                return -1;
        } else if (field == 8 && wire == 2)
            initializer->name = reader_text(payload, 4096);
        else if (field == 9 && wire == 2) {
            raw_data = payload.cursor;
            raw_size = (size_t)(payload.end - payload.cursor);
        }
    }
    if (!initializer->name || !desc.rank || !raw_data)
        return -1;
    initializer->tensor = oasp_ai_tensor_create(context, &desc, raw_data, raw_size, error);
    return initializer->tensor ? 0 : -1;
}

static int parse_attribute(proto_reader reader, onnx_node *node) {
    char *name = NULL;
    int64_t integer_value = 0;
    float float_value = 0;
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            goto failure;
        if (field == 1 && wire == 2)
            name = reader_text(payload, 128);
        else if (field == 2 && wire == 5)
            memcpy(&float_value, payload.cursor, sizeof(float_value));
        else if (field == 3 && wire == 0)
            integer_value = (int64_t)integer;
    }
    if (!name)
        return -1;
    if (!strcmp(name, "axis")) {
        node->has_axis = 1;
        node->axis = integer_value;
    } else if (!strcmp(name, "alpha")) {
        node->has_alpha = 1;
        node->alpha = float_value;
    } else
        goto failure;
    free(name);
    return 0;
failure:
    free(name);
    return -1;
}

static int parse_node(proto_reader reader, onnx_node *node) {
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field == 1 && wire == 2) {
            char *input = reader_text(payload, 4096);
            char **grown = input ? realloc(node->inputs, (node->input_count + 1) * sizeof(*grown)) : NULL;
            if (!grown) {
                free(input);
                return -1;
            }
            node->inputs = grown;
            node->inputs[node->input_count++] = input;
        } else if (field == 2 && wire == 2) {
            if (node->output)
                return -1;
            node->output = reader_text(payload, 4096);
        } else if (field == 4 && wire == 2)
            node->operation = reader_text(payload, 128);
        else if (field == 5 && wire == 2 && parse_attribute(payload, node))
            return -1;
        else if (field == 7 && wire == 2 && payload.cursor != payload.end)
            return -1;
    }
    return !node->input_count || !node->output || !node->operation ? -1 : 0;
}

static void free_onnx_graph(onnx_graph *graph) {
    free(graph->name);
    for (size_t index = 0; index < graph->input_count; index++)
        free(graph->inputs[index].name);
    for (size_t index = 0; index < graph->output_count; index++)
        free(graph->outputs[index].name);
    for (size_t index = 0; index < graph->initializer_count; index++) {
        free(graph->initializers[index].name);
        oasp_ai_tensor_release(graph->initializers[index].tensor);
    }
    for (size_t index = 0; index < graph->node_count; index++) {
        for (size_t input = 0; input < graph->nodes[index].input_count; input++)
            free(graph->nodes[index].inputs[input]);
        free(graph->nodes[index].inputs);
        free(graph->nodes[index].output);
        free(graph->nodes[index].operation);
    }
    free(graph->inputs);
    free(graph->outputs);
    free(graph->initializers);
    free(graph->nodes);
}

static int parse_graph(proto_reader reader, oasp_ai_context *context, onnx_graph *graph, oasp_ai_error *error) {
    while (reader.cursor < reader.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        if (next_field(&reader, &field, &wire, &payload, &integer))
            return -1;
        if (field == 1 && wire == 2) {
            if (append_pointer((void **)&graph->nodes, sizeof(*graph->nodes), &graph->node_count) ||
                parse_node(payload, &graph->nodes[graph->node_count - 1]))
                return -1;
        } else if (field == 2 && wire == 2)
            graph->name = reader_text(payload, 4096);
        else if (field == 5 && wire == 2) {
            if (append_pointer((void **)&graph->initializers, sizeof(*graph->initializers), &graph->initializer_count) ||
                parse_tensor(payload, context, &graph->initializers[graph->initializer_count - 1], error))
                return -1;
        } else if ((field == 11 || field == 12) && wire == 2) {
            onnx_value **items = field == 11 ? &graph->inputs : &graph->outputs;
            size_t *count = field == 11 ? &graph->input_count : &graph->output_count;
            if (append_pointer((void **)items, sizeof(**items), count) || parse_value_info(payload, &(*items)[*count - 1]))
                return -1;
        }
    }
    return !graph->name || !graph->input_count || !graph->output_count ? -1 : 0;
}

static ssize_t find_name(char **names, size_t count, const char *name) {
    for (size_t index = 0; index < count; index++)
        if (!strcmp(names[index], name))
            return (ssize_t)index;
    return -1;
}

oasp_ai_graph *oasp_ai_graph_import_onnx(oasp_ai_context *context, const char *path, oasp_ai_error *error) {
    FILE *file = context && path ? fopen(path, "rb") : NULL;
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file)
            fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length <= 0 || (uint64_t)length > context->options.maximum_model_bytes || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    unsigned char *data = malloc((size_t)length);
    int failed = !data || fread(data, 1, (size_t)length, file) != (size_t)length;
    fclose(file);
    proto_reader model = {data, data + (failed ? 0 : length)};
    proto_reader graph_payload = {0};
    uint64_t opset = 0;
    while (!failed && model.cursor < model.end) {
        uint32_t field;
        uint32_t wire;
        proto_reader payload = {0};
        uint64_t integer = 0;
        failed = next_field(&model, &field, &wire, &payload, &integer);
        if (!failed && field == 7 && wire == 2)
            graph_payload = payload;
        else if (!failed && field == 8 && wire == 2)
            while (payload.cursor < payload.end) {
                uint32_t opset_field;
                uint32_t opset_wire;
                proto_reader ignored = {0};
                if (next_field(&payload, &opset_field, &opset_wire, &ignored, &integer)) {
                    failed = 1;
                    break;
                }
                if (opset_field == 2 && opset_wire == 0)
                    opset = integer;
                if (opset_field == 1 && opset_wire == 2 && ignored.cursor != ignored.end)
                    failed = 1;
            }
    }
    onnx_graph parsed = {0};
    if (!failed)
        failed = !graph_payload.cursor || !opset || opset > 20 || parse_graph(graph_payload, context, &parsed, error);
    oasp_ai_graph *graph = failed ? NULL : oasp_ai_graph_create(context, parsed.name, error);
    size_t capacity = parsed.input_count + parsed.initializer_count + parsed.node_count;
    char **names = graph && capacity ? calloc(capacity, sizeof(*names)) : NULL;
    uint32_t *ids = graph && capacity ? calloc(capacity, sizeof(*ids)) : NULL;
    size_t count = 0;
    if (graph && capacity && (!names || !ids))
        failed = 1;
    for (size_t index = 0; index < parsed.initializer_count && !failed; index++) {
        ids[count] = oasp_ai_graph_constant(graph, parsed.initializers[index].name, parsed.initializers[index].tensor, error);
        names[count++] = parsed.initializers[index].name;
        failed = !ids[count - 1];
    }
    for (size_t index = 0; index < parsed.input_count && !failed; index++) {
        int initializer_input = 0;
        for (size_t item = 0; item < parsed.initializer_count; item++)
            initializer_input |= !strcmp(parsed.inputs[index].name, parsed.initializers[item].name);
        if (initializer_input)
            continue;
        ids[count] = oasp_ai_graph_input(graph, parsed.inputs[index].name, &parsed.inputs[index].desc, error);
        names[count++] = parsed.inputs[index].name;
        failed = !ids[count - 1];
    }
    for (size_t index = 0; index < parsed.node_count && !failed; index++) {
        onnx_node *node = &parsed.nodes[index];
        const onnx_operator *mapping = operator_by_name(node->operation);
        uint32_t *inputs = mapping ? calloc(node->input_count, sizeof(*inputs)) : NULL;
        failed = !mapping || node->input_count < mapping->minimum_inputs || node->input_count > mapping->maximum_inputs || !inputs;
        for (size_t input = 0; input < node->input_count && !failed; input++) {
            ssize_t found = find_name(names, count, node->inputs[input]);
            failed = found < 0;
            if (!failed)
                inputs[input] = ids[found];
        }
        int32_t axis = (int32_t)node->axis;
        double alpha = node->alpha;
        const void *attributes = NULL;
        size_t attributes_size = 0;
        if (mapping && (mapping->operation == OASP_AI_OP_SOFTMAX || mapping->operation == OASP_AI_OP_GATHER)) {
            attributes = &axis;
            attributes_size = sizeof(axis);
        } else if (mapping && mapping->operation == OASP_AI_OP_LEAKY_RELU) {
            attributes = &alpha;
            attributes_size = sizeof(alpha);
        }
        uint32_t output = !failed ? oasp_ai_graph_operation(graph, mapping->operation, inputs, node->input_count,
                                                             attributes, attributes_size, error) : 0;
        free(inputs);
        if (!output || count >= capacity)
            failed = 1;
        else {
            free(graph->values[output - 1].name);
            graph->values[output - 1].name = oasp_ai_string_duplicate(node->output);
            failed = !graph->values[output - 1].name;
            names[count] = node->output;
            ids[count++] = output;
        }
    }
    uint32_t *outputs = !failed ? calloc(parsed.output_count, sizeof(*outputs)) : NULL;
    if (!failed && !outputs)
        failed = 1;
    for (size_t index = 0; index < parsed.output_count && !failed; index++) {
        ssize_t found = find_name(names, count, parsed.outputs[index].name);
        failed = found < 0;
        if (!failed)
            outputs[index] = ids[found];
    }
    if (!failed)
        failed = oasp_ai_graph_set_outputs(graph, outputs, parsed.output_count, error) || oasp_ai_graph_verify(graph, error);
    free(outputs);
    free(names);
    free(ids);
    free_onnx_graph(&parsed);
    free(data);
    if (failed) {
        oasp_ai_graph_release(graph);
        oasp_ai_set_error(error, 5, "ImportONNX", "ONNX model is malformed or outside the supported standard subset",
                          "Use opset 20 or earlier with supported tensor types and operators.");
        return NULL;
    }
    return graph;
}
