// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/runtime/value.c`): Runtime core. This unit manages ASP values, frames,
 * bytecode execution, and allocator roots. State variables encode interpreter control flow and
 * errors; all allocation and reference updates must preserve arena or generational-GC ownership
 * rules.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "value.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

oasp_value oasp_value_empty(void) {
    oasp_value value = {0};
    value.kind = OASP_VALUE_EMPTY;
    return value;
}

oasp_value oasp_value_null(void) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_NULL;
    return value;
}

oasp_value oasp_value_bool(bool boolean) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_BOOL;
    value.as.boolean = boolean;
    return value;
}

oasp_value oasp_value_int(int64_t integer) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_INT;
    value.as.integer = integer;
    return value;
}

oasp_value oasp_value_float(double floating) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_FLOAT;
    value.as.floating = floating;
    return value;
}

oasp_value oasp_value_string(const char *data, size_t length) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_STRING;
    value.as.string.data = data;
    value.as.string.length = length;
    return value;
}

oasp_value oasp_value_nothing(void) {
    oasp_value value = oasp_value_empty();
    value.kind = OASP_VALUE_NOTHING;
    return value;
}

static bool oasp_slice_to_int(oasp_slice text, int64_t *result) {
    size_t offset = 0;
    while (offset < text.length && isspace((unsigned char)text.data[offset]))
        offset++;
    bool negative = false;
    if (offset < text.length && (text.data[offset] == '+' || text.data[offset] == '-')) {
        negative = text.data[offset] == '-';
        offset++;
    }
    size_t digit_start = offset;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t value = 0;
    while (offset < text.length && isdigit((unsigned char)text.data[offset])) {
        unsigned digit = (unsigned)(text.data[offset] - '0');
        if (value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
        offset++;
    }
    if (offset == digit_start || offset != text.length)
        return false;
    if (negative)
        *result = value == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)value;
    else
        *result = (int64_t)value;
    return true;
}

bool oasp_value_truthy(const oasp_value *value) {
    if (!value)
        return false;
    switch (value->kind) {
    case OASP_VALUE_BOOL:
        return value->as.boolean;
    case OASP_VALUE_INT:
        return value->as.integer != 0;
    case OASP_VALUE_FLOAT:
        return value->as.floating != 0.0;
    case OASP_VALUE_STRING:
        return value->as.string.length != 0;
    case OASP_VALUE_DATE:
        return true;
    default:
        return false;
    }
}

bool oasp_value_to_int(const oasp_value *value, int64_t *result) {
    if (!value || !result)
        return false;
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
        *result = 0;
        return true;
    case OASP_VALUE_BOOL:
        *result = value->as.boolean ? -1 : 0;
        return true;
    case OASP_VALUE_INT:
        *result = value->as.integer;
        return true;
    case OASP_VALUE_FLOAT:
        if (!isfinite(value->as.floating) || value->as.floating < -0x1p63 ||
            value->as.floating >= 0x1p63)
            return false;
        *result = (int64_t)value->as.floating;
        return true;
    case OASP_VALUE_STRING:
        return oasp_slice_to_int(value->as.string, result);
    default:
        return false;
    }
}

bool oasp_value_to_float(const oasp_value *value, double *result) {
    int64_t integer = 0;
    if (!value || !result)
        return false;
    if (value->kind == OASP_VALUE_FLOAT) {
        *result = value->as.floating;
        return true;
    }
    if (oasp_value_to_int(value, &integer)) {
        *result = (double)integer;
        return true;
    }
    return false;
}

bool oasp_value_text(const oasp_value *value, char *buffer, size_t capacity, oasp_slice *result) {
    if (!value || !buffer || !capacity || !result)
        return false;
    const char *data = "";
    size_t length = 0;
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        break;
    case OASP_VALUE_BOOL:
        data = value->as.boolean ? "True" : "False";
        length = value->as.boolean ? 4 : 5;
        break;
    case OASP_VALUE_INT:
        length = (size_t)snprintf(buffer, capacity, "%lld", (long long)value->as.integer);
        data = buffer;
        break;
    case OASP_VALUE_FLOAT:
        length = (size_t)snprintf(buffer, capacity, "%.15g", value->as.floating);
        data = buffer;
        break;
    case OASP_VALUE_STRING:
        data = value->as.string.data;
        length = value->as.string.length;
        break;
    case OASP_VALUE_OBJECT:
    case OASP_VALUE_ARRAY:
        data = "[object]";
        length = 8;
        break;
    case OASP_VALUE_DATE:
        data = value->as.date.data;
        length = value->as.date.length;
        break;
    }
    if (length >= capacity && data == buffer)
        return false;
    result->data = data;
    result->length = length;
    return true;
}

char *oasp_value_format(oasp_arena *arena, const oasp_value *value) {
    char buffer[64];
    oasp_slice text;
    if (!arena || !oasp_value_text(value, buffer, sizeof(buffer), &text))
        return NULL;
    return oasp_arena_copy(arena, text.data, text.length);
}

static oasp_value oasp_value_clone(oasp_arena *arena, const oasp_value *value, bool copy_text) {
    if (!value)
        return oasp_value_empty();
    oasp_value copy = *value;
    if (copy_text && (value->kind == OASP_VALUE_STRING || value->kind == OASP_VALUE_DATE)) {
        oasp_slice source = value->kind == OASP_VALUE_STRING ? value->as.string : value->as.date;
        char *text = oasp_arena_copy(arena, source.data, source.length);
        if (!text)
            return oasp_value_empty();
        if (value->kind == OASP_VALUE_STRING)
            copy.as.string.data = text;
        else
            copy.as.date.data = text;
    } else if (value->kind == OASP_VALUE_ARRAY) {
        const oasp_array *source = value->as.array;
        oasp_array *array = oasp_arena_alloc(arena, sizeof(*array), _Alignof(oasp_array));
        if (!source || !array)
            return oasp_value_empty();
        *array = *source;
        array->bounds = NULL;
        array->items = NULL;
        if (source->rank) {
            array->bounds = oasp_arena_alloc(arena, source->rank * sizeof(*array->bounds), _Alignof(int64_t));
            if (!array->bounds)
                return oasp_value_empty();
            memcpy(array->bounds, source->bounds, source->rank * sizeof(*array->bounds));
        }
        if (source->length) {
            array->items = oasp_arena_alloc(arena, source->length * sizeof(*array->items), _Alignof(oasp_value));
            if (!array->items)
                return oasp_value_empty();
            for (size_t i = 0; i < source->length; i++)
                array->items[i] = oasp_value_clone(arena, &source->items[i], copy_text);
        }
        copy.as.array = array;
    }
    return copy;
}

oasp_value oasp_value_copy(oasp_arena *arena, const oasp_value *value) { return oasp_value_clone(arena, value, true); }

oasp_value oasp_value_request_copy(oasp_arena *arena, const oasp_value *value) { return oasp_value_clone(arena, value, false); }
