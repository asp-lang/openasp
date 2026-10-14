// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define PCRE2_CODE_UNIT_WIDTH 8

#include "builtin_internal.h"

#include <pcre2.h>
#include <string.h>

static void trace_value(oasp_allocator *allocator, const oasp_value *value) {
    if (value->kind == OASP_VALUE_STRING)
        oasp_allocator_trace(allocator, value->as.string.data);
    else if (value->kind == OASP_VALUE_DATE)
        oasp_allocator_trace(allocator, value->as.date.data);
    else if (value->kind == OASP_VALUE_ARRAY)
        oasp_allocator_trace(allocator, value->as.array);
    else if (value->kind == OASP_VALUE_OBJECT)
        oasp_allocator_trace(allocator, value->as.object);
}

static oasp_value slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static pcre2_code *compile_pattern(oasp_runtime *runtime, const oasp_regexp *expression, size_t line) {
    oasp_buffer translated;
    if (!oasp_buffer_init(&translated, expression->pattern.length + 16))
        return NULL;
    for (size_t i = 0; i < expression->pattern.length; i++) {
        const char *pattern = expression->pattern.data;
        if (pattern[i] == '\\' && i + 5 < expression->pattern.length && pattern[i + 1] == 'u') {
            (void)oasp_buffer_append(&translated, "\\x{", 3);
            (void)oasp_buffer_append(&translated, pattern + i + 2, 4);
            (void)oasp_buffer_append_byte(&translated, '}');
            i += 5;
        } else if (pattern[i] == '\\' && i + 1 < expression->pattern.length && pattern[i + 1] == '_') {
            (void)oasp_buffer_append_byte(&translated, '_');
            i++;
        } else {
            (void)oasp_buffer_append_byte(&translated, pattern[i]);
        }
    }
    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    uint32_t flags = PCRE2_UTF | PCRE2_UCP;
    if (expression->ignore_case)
        flags |= PCRE2_CASELESS;
    if (expression->multiline)
        flags |= PCRE2_MULTILINE;
    pcre2_code *compiled = pcre2_compile((PCRE2_SPTR)translated.data, translated.length, flags,
                                         &error_code, &error_offset, NULL);
    oasp_buffer_dispose(&translated);
    if (compiled)
        return compiled;
    PCRE2_UCHAR message[160];
    int length = pcre2_get_error_message(error_code, message, sizeof(message));
    oasp_runtime_raise(runtime, 5017, oasp_runtime_copy(runtime, (const char *)message,
                                                         length > 0 ? (size_t)length : 0),
                       "VBScript.RegExp", line);
    return NULL;
}

static oasp_value execute(oasp_runtime *runtime, oasp_regexp *expression, const oasp_value *input_value, size_t line) {
    char *input = oasp_value_format(runtime->arena, input_value);
    pcre2_code *compiled = input ? compile_pattern(runtime, expression, line) : NULL;
    if (!compiled)
        return oasp_value_empty();
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(compiled, NULL);
    size_t input_length = strlen(input);
    oasp_component_collection *collection = match_data
        ? oasp_runtime_alloc(runtime, sizeof(*collection), _Alignof(oasp_component_collection)) : NULL;
    if (!collection) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(compiled);
        return oasp_value_empty();
    }
    collection->items = oasp_runtime_alloc_values(runtime, input_length + 1);
    collection->count = 0;
    if (!collection->items) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(compiled);
        return oasp_value_empty();
    }
    uint32_t capture_count = 0;
    (void)pcre2_pattern_info(compiled, PCRE2_INFO_CAPTURECOUNT, &capture_count);
    for (size_t offset = 0; offset <= input_length;) {
        int status = pcre2_match(compiled, (PCRE2_SPTR)input, input_length, offset, 0, match_data, NULL);
        if (status < 0)
            break;
        PCRE2_SIZE *captures = pcre2_get_ovector_pointer(match_data);
        size_t start = (size_t)captures[0];
        size_t end = (size_t)captures[1];
        oasp_regexp_match *match = oasp_runtime_alloc(runtime, sizeof(*match), _Alignof(oasp_regexp_match));
        if (!match)
            break;
        memset(match, 0, sizeof(*match));
        oasp_value value = slice_value(runtime, input + start, end - start);
        match->value = value.as.string;
        match->first_index = start;
        if (capture_count) {
            match->submatches.items = oasp_runtime_alloc_values(runtime, capture_count);
            if (!match->submatches.items)
                break;
            match->submatches.count = capture_count;
            for (size_t i = 0; i < capture_count; i++) {
                PCRE2_SIZE group_start = captures[(i + 1) * 2];
                PCRE2_SIZE group_end = captures[(i + 1) * 2 + 1];
                match->submatches.items[i] = group_start == PCRE2_UNSET
                    ? oasp_value_string("", 0) : slice_value(runtime, input + group_start, (size_t)(group_end - group_start));
            }
        }
        oasp_object *match_object = oasp_component_object(runtime, OASP_COMPONENT_MATCH, "VBScript.RegExp.Match", match);
        if (!match_object)
            break;
        collection->items[collection->count++] = oasp_component_object_value(match_object);
        if (!expression->global)
            break;
        offset = end > offset ? end : offset + 1;
    }
    pcre2_match_data_free(match_data);
    pcre2_code_free(compiled);
    oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_MATCH_COLLECTION,
                                                 "VBScript.RegExp.MatchCollection", collection);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

static oasp_value replace(oasp_runtime *runtime, oasp_regexp *expression, const oasp_value *input_value, const oasp_value *replacement_value, size_t line) {
    char *input = oasp_value_format(runtime->arena, input_value);
    char *replacement = oasp_value_format(runtime->arena, replacement_value);
    pcre2_code *compiled = input && replacement ? compile_pattern(runtime, expression, line) : NULL;
    if (!compiled)
        return oasp_value_empty();
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(compiled, NULL);
    oasp_buffer output;
    if (!match_data || !oasp_buffer_init(&output, strlen(input) + 32)) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(compiled);
        return oasp_value_empty();
    }
    size_t input_length = strlen(input);
    for (size_t offset = 0; offset <= input_length;) {
        int status = pcre2_match(compiled, (PCRE2_SPTR)input, input_length, offset, 0, match_data, NULL);
        if (status < 0) {
            (void)oasp_buffer_append(&output, input + offset, input_length - offset);
            break;
        }
        PCRE2_SIZE *captures = pcre2_get_ovector_pointer(match_data);
        size_t start = (size_t)captures[0];
        size_t end = (size_t)captures[1];
        (void)oasp_buffer_append(&output, input + offset, start - offset);
        for (size_t i = 0; replacement[i]; i++) {
            if (replacement[i] == '$' && replacement[i + 1] >= '0' && replacement[i + 1] <= '9') {
                size_t group = (size_t)(replacement[++i] - '0');
                if ((int)group < status && captures[group * 2] != PCRE2_UNSET)
                    (void)oasp_buffer_append(&output, input + captures[group * 2],
                                             captures[group * 2 + 1] - captures[group * 2]);
            } else {
                (void)oasp_buffer_append_byte(&output, replacement[i]);
            }
        }
        offset = end > offset ? end : offset + 1;
        if (!expression->global) {
            (void)oasp_buffer_append(&output, input + offset, input_length - offset);
            break;
        }
    }
    pcre2_match_data_free(match_data);
    pcre2_code_free(compiled);
    oasp_value result = slice_value(runtime, output.data ? output.data : "", output.length);
    oasp_buffer_dispose(&output);
    return result;
}

oasp_regexp *oasp_builtin_regexp_create(oasp_runtime *runtime) {
    oasp_regexp *expression = oasp_runtime_alloc(runtime, sizeof(*expression), _Alignof(oasp_regexp));
    if (expression)
        memset(expression, 0, sizeof(*expression));
    return expression;
}

bool oasp_builtin_regexp_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    oasp_regexp *expression = object->native;
    if (oasp_ascii_equal(member, "pattern"))
        *result = oasp_value_string(expression->pattern.data, expression->pattern.length);
    else if (oasp_ascii_equal(member, "global"))
        *result = oasp_value_bool(expression->global);
    else if (oasp_ascii_equal(member, "ignorecase"))
        *result = oasp_value_bool(expression->ignore_case);
    else if (oasp_ascii_equal(member, "multiline"))
        *result = oasp_value_bool(expression->multiline);
    else
        return false;
    return true;
}

bool oasp_builtin_regexp_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value) {
    oasp_regexp *expression = object->native;
    if (oasp_ascii_equal(member, "pattern")) {
        char *text = oasp_value_format(runtime->arena, value);
        if (text)
            expression->pattern = (oasp_slice){text, strlen(text)};
    } else if (oasp_ascii_equal(member, "global"))
        expression->global = oasp_value_truthy(value);
    else if (oasp_ascii_equal(member, "ignorecase"))
        expression->ignore_case = oasp_value_truthy(value);
    else if (oasp_ascii_equal(member, "multiline"))
        expression->multiline = oasp_value_truthy(value);
    else
        return false;
    return true;
}

bool oasp_builtin_regexp_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                              size_t line, oasp_value *result) {
    oasp_regexp *expression = object->native;
    if (oasp_ascii_equal(member, "test") && argument_count) {
        oasp_value matches = execute(runtime, expression, &arguments[0], line);
        oasp_component_collection *collection = matches.kind == OASP_VALUE_OBJECT ? matches.as.object->native : NULL;
        *result = oasp_value_bool(collection && collection->count);
    } else if (oasp_ascii_equal(member, "execute") && argument_count) {
        *result = execute(runtime, expression, &arguments[0], line);
    } else if (oasp_ascii_equal(member, "replace") && argument_count >= 2) {
        *result = replace(runtime, expression, &arguments[0], &arguments[1], line);
    } else {
        return false;
    }
    return true;
}

bool oasp_builtin_regexp_match_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *result) {
    oasp_regexp_match *match = object->native;
    if (oasp_ascii_equal(member, "value"))
        *result = oasp_value_string(match->value.data, match->value.length);
    else if (oasp_ascii_equal(member, "firstindex"))
        *result = oasp_value_int((int64_t)match->first_index);
    else if (oasp_ascii_equal(member, "length"))
        *result = oasp_value_int((int64_t)match->value.length);
    else if (oasp_ascii_equal(member, "submatches")) {
        oasp_object *submatches = oasp_component_object(runtime, OASP_COMPONENT_SUBMATCH_COLLECTION,
                                                         "VBScript.RegExp.SubMatches", &match->submatches);
        *result = submatches ? oasp_component_object_value(submatches) : oasp_value_empty();
    } else {
        return false;
    }
    return true;
}

bool oasp_builtin_regexp_match_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                    size_t line, oasp_value *result) {
    if (!oasp_ascii_equal(member, "submatches"))
        return false;
    oasp_regexp_match *match = object->native;
    int64_t index = -1;
    if (!argument_count || !oasp_value_to_int(&arguments[0], &index) || index < 0 || (size_t)index >= match->submatches.count) {
        oasp_runtime_raise(runtime, 9, "subscript out of range", object->component_name, line);
        *result = oasp_value_empty();
    } else {
        *result = match->submatches.items[index];
    }
    return true;
}

void oasp_builtin_regexp_gc_trace(oasp_allocator *allocator, const oasp_object *object) {
    if (object->component_kind == OASP_COMPONENT_REGEXP) {
        const oasp_regexp *expression = object->native;
        oasp_allocator_trace(allocator, expression->pattern.data);
    } else if (object->component_kind == OASP_COMPONENT_MATCH) {
        const oasp_regexp_match *match = object->native;
        oasp_allocator_trace(allocator, match->value.data);
        oasp_allocator_trace(allocator, match->submatches.items);
        for (size_t i = 0; i < match->submatches.count; i++)
            trace_value(allocator, &match->submatches.items[i]);
    }
}
