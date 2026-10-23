// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "openai_stream_internal.h"

#include <string.h>

bool oasp_openai_sse_init(oasp_sse_parser *parser) {
    if (!parser || !oasp_buffer_init(&parser->input, 4096))
        return false;
    if (!oasp_buffer_init(&parser->data, 1024)) {
        oasp_buffer_dispose(&parser->input);
        return false;
    }
    if (!oasp_buffer_init(&parser->event_type, 64)) {
        oasp_buffer_dispose(&parser->data);
        oasp_buffer_dispose(&parser->input);
        return false;
    }
    return true;
}

void oasp_openai_sse_dispose(oasp_sse_parser *parser) {
    if (!parser)
        return;
    oasp_buffer_dispose(&parser->input);
    oasp_buffer_dispose(&parser->data);
    oasp_buffer_dispose(&parser->event_type);
}

static bool sse_set_buffer(oasp_buffer *buffer, const char *data, size_t length) {
    buffer->length = 0;
    buffer->data[0] = '\0';
    return oasp_buffer_append(buffer, data, length);
}

static bool sse_parse_line(oasp_sse_parser *parser, const char *line, size_t length, bool *has_data) {
    if (!length || line[0] == ':')
        return true;
    const char *colon = memchr(line, ':', length);
    size_t name_length = colon ? (size_t)(colon - line) : length;
    const char *value = colon ? colon + 1 : line + length;
    size_t value_length = colon ? length - name_length - 1 : 0;
    if (value_length && *value == ' ') {
        value++;
        value_length--;
    }
    if (name_length == 4 && !memcmp(line, "data", 4)) {
        if (*has_data && !oasp_buffer_append_byte(&parser->data, '\n'))
            return false;
        if (!oasp_buffer_append(&parser->data, value, value_length))
            return false;
        *has_data = true;
    } else if (name_length == 5 && !memcmp(line, "event", 5)) {
        if (!sse_set_buffer(&parser->event_type, value, value_length))
            return false;
    }
    return true;
}

oasp_sse_status oasp_openai_sse_next(oasp_sse_parser *parser, size_t max_event_bytes) {
    size_t cursor = 0;
    size_t consumed = 0;
    while (cursor < parser->input.length) {
        const char *newline = memchr(parser->input.data + cursor, '\n', parser->input.length - cursor);
        if (!newline)
            break;
        size_t line_end = (size_t)(newline - parser->input.data);
        size_t content_end = line_end;
        if (content_end > cursor && parser->input.data[content_end - 1] == '\r')
            content_end--;
        if (content_end == cursor) {
            consumed = line_end + 1;
            break;
        }
        cursor = line_end + 1;
        if (cursor > max_event_bytes)
            return OASP_SSE_LIMIT;
    }
    if (!consumed)
        return parser->input.length > max_event_bytes ? OASP_SSE_LIMIT : OASP_SSE_INCOMPLETE;
    if (consumed > max_event_bytes)
        return OASP_SSE_LIMIT;

    parser->data.length = 0;
    parser->data.data[0] = '\0';
    parser->event_type.length = 0;
    parser->event_type.data[0] = '\0';
    bool has_data = false;
    cursor = 0;
    while (cursor < consumed) {
        const char *newline = memchr(parser->input.data + cursor, '\n', consumed - cursor);
        if (!newline)
            return OASP_SSE_INVALID;
        size_t line_end = (size_t)(newline - parser->input.data);
        size_t content_end = line_end;
        if (content_end > cursor && parser->input.data[content_end - 1] == '\r')
            content_end--;
        if (content_end == cursor)
            break;
        if (!sse_parse_line(parser, parser->input.data + cursor, content_end - cursor, &has_data))
            return OASP_SSE_INVALID;
        cursor = line_end + 1;
    }

    size_t remaining = parser->input.length - consumed;
    if (remaining)
        memmove(parser->input.data, parser->input.data + consumed, remaining);
    parser->input.length = remaining;
    parser->input.data[remaining] = '\0';
    return has_data ? OASP_SSE_EVENT : OASP_SSE_SKIPPED;
}
