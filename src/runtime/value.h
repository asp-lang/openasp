// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Tagged ASP value representation and coercion API. String/date slices and
 * object/array pointers are borrowed from their owning arena or runtime
 * allocator. Constructors do not copy payloads; explicit copy helpers are
 * required when a value crosses an ownership or request-lifetime boundary.
 */

#ifndef OPENASP_VALUE_H
#define OPENASP_VALUE_H

#include "internal.h"

typedef enum {
    OASP_VALUE_EMPTY,
    OASP_VALUE_NULL,
    OASP_VALUE_BOOL,
    OASP_VALUE_INT,
    OASP_VALUE_FLOAT,
    OASP_VALUE_STRING,
    OASP_VALUE_OBJECT,
    OASP_VALUE_ARRAY,
    OASP_VALUE_NOTHING,
    OASP_VALUE_DATE
} oasp_value_kind;

typedef struct oasp_object oasp_object;
typedef struct oasp_array oasp_array;

typedef struct {
    /* `kind` selects exactly one member of the payload union. */
    oasp_value_kind kind;
    union {
        bool boolean;
        int64_t integer;
        double floating;
        oasp_slice string;
        oasp_object *object;
        oasp_array *array;
        oasp_slice date;
    } as;
} oasp_value;

struct oasp_array {
    /* Row-major dimensions; bounds contains one inclusive upper bound per rank. */
    size_t rank;
    size_t length;
    int64_t *bounds;
    oasp_value *items;
};

/* Constructs scalar values without allocation; string data remains borrowed. */
oasp_value oasp_value_empty(void);
oasp_value oasp_value_null(void);
oasp_value oasp_value_bool(bool value);
oasp_value oasp_value_int(int64_t value);
oasp_value oasp_value_float(double value);
oasp_value oasp_value_string(const char *data, size_t length);
oasp_value oasp_value_nothing(void);

/*
 * Applies ASP truth/coercion rules. Numeric conversions write `result` only on
 * success. Text conversion may use caller scratch `buffer` and returns a slice
 * whose lifetime follows either that buffer or the original value.
 */
bool oasp_value_truthy(const oasp_value *value);
bool oasp_value_to_int(const oasp_value *value, int64_t *result);
bool oasp_value_to_float(const oasp_value *value, double *result);
bool oasp_value_text(const oasp_value *value, char *buffer, size_t capacity, oasp_slice *result);

/*
 * Formatting and copy helpers allocate in `arena`. The request-copy variant
 * recursively detaches mutable values that must survive runtime teardown.
 */
char *oasp_value_format(oasp_arena *arena, const oasp_value *value);
oasp_value oasp_value_copy(oasp_arena *arena, const oasp_value *value);
oasp_value oasp_value_request_copy(oasp_arena *arena, const oasp_value *value);

#endif
