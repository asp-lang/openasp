// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static oasp_value stream_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static char *stream_host_path(oasp_runtime *runtime, const char *path) {
    size_t length = strlen(path);
    char *host = oasp_runtime_copy(runtime, path, length);
    if (!host)
        return NULL;
    for (size_t i = 0; i < length; i++)
        if (host[i] == '\\')
            host[i] = '/';
    return host;
}

static bool stream_read_file(oasp_runtime *runtime, const char *path, oasp_value *result) {
    char *host = stream_host_path(runtime, path);
    FILE *file = host ? fopen(host, "rb") : NULL;
    if (!file)
        return false;
    oasp_buffer buffer;
    if (!oasp_buffer_init(&buffer, 4096)) {
        fclose(file);
        return false;
    }
    char chunk[4096];
    size_t count;
    bool ok = true;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) != 0)
        if (!oasp_buffer_append(&buffer, chunk, count)) {
            ok = false;
            break;
        }
    if (ferror(file))
        ok = false;
    fclose(file);
    *result = ok ? stream_slice_value(runtime, buffer.data, buffer.length) : oasp_value_empty();
    oasp_buffer_dispose(&buffer);
    return ok;
}

static bool stream_write(oasp_stream *stream, const char *data, size_t length) {
    if (stream->position > stream->buffer.length) {
        size_t padding = stream->position - stream->buffer.length;
        while (padding--)
            if (!oasp_buffer_append_byte(&stream->buffer, '\0'))
                return false;
    }
    size_t available = stream->buffer.length - stream->position;
    size_t overwrite = length < available ? length : available;
    if (overwrite)
        memcpy(stream->buffer.data + stream->position, data, overwrite);
    if (length > overwrite && !oasp_buffer_append(&stream->buffer, data + overwrite, length - overwrite))
        return false;
    stream->position += length;
    return true;
}

void oasp_builtin_stream_finalize(void *context) {
    oasp_stream *stream = context;
    oasp_buffer_dispose(&stream->buffer);
}

void *oasp_builtin_stream_create(oasp_runtime *runtime) {
    oasp_stream *stream = oasp_runtime_alloc(runtime, sizeof(*stream), _Alignof(oasp_stream));
    if (!stream || !oasp_buffer_init(&stream->buffer, 4096))
        return NULL;
    stream->position = 0;
    stream->type = 2;
    stream->mode = 3;
    stream->state = 0;
    stream->charset.data = "utf-8";
    stream->charset.length = 5;
    if (!oasp_runtime_account_external(runtime, stream, stream->buffer.capacity) ||
        !oasp_runtime_defer(runtime, stream, oasp_builtin_stream_finalize, stream)) {
        oasp_buffer_dispose(&stream->buffer);
        return NULL;
    }
    return stream;
}

bool oasp_builtin_stream_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_STREAM)
        return false;
    oasp_stream *stream = object->native;
    if (oasp_ascii_equal(member, "type"))
        *result = oasp_value_int(stream->type);
    else if (oasp_ascii_equal(member, "position"))
        *result = oasp_value_int((int64_t)stream->position);
    else if (oasp_ascii_equal(member, "size"))
        *result = oasp_value_int((int64_t)stream->buffer.length);
    else if (oasp_ascii_equal(member, "eos"))
        *result = oasp_value_bool(stream->position >= stream->buffer.length);
    else if (oasp_ascii_equal(member, "state"))
        *result = oasp_value_int(stream->state);
    else if (oasp_ascii_equal(member, "mode"))
        *result = oasp_value_int(stream->mode);
    else if (oasp_ascii_equal(member, "charset"))
        *result = oasp_value_string(stream->charset.data, stream->charset.length);
    else
        return false;
    return true;
}

bool oasp_builtin_stream_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!object || object->component_kind != OASP_COMPONENT_STREAM)
        return false;
    oasp_stream *stream = object->native;
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "writetext") || oasp_ascii_equal(member, "write")) {
        char *text = oasp_value_format(runtime->arena, value);
        if (!stream->state)
            oasp_runtime_raise(runtime, 3704, "stream is closed", object->component_name, line);
        else if (!text || !stream_write(stream, text, strlen(text)))
            oasp_runtime_raise(runtime, 7, "out of memory", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "type")) {
        if (!oasp_value_to_int(value, &integer) || (integer != 1 && integer != 2))
            oasp_runtime_raise(runtime, 5, "invalid stream type", object->component_name, line);
        else
            stream->type = (int)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "position")) {
        if (!oasp_value_to_int(value, &integer) || integer < 0 || (uint64_t)integer > SIZE_MAX)
            oasp_runtime_raise(runtime, 5, "invalid stream position", object->component_name, line);
        else
            stream->position = (size_t)integer;
        return true;
    }
    if (oasp_ascii_equal(member, "charset")) {
        char *text = oasp_value_format(runtime->arena, value);
        if (text) {
            stream->charset.data = text;
            stream->charset.length = strlen(text);
        }
        return true;
    }
    if (oasp_ascii_equal(member, "mode")) {
        if (oasp_value_to_int(value, &integer))
            stream->mode = (int)integer;
        return true;
    }
    return false;
}

bool oasp_builtin_stream_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                              size_t line, oasp_value *result) {
    if (!object || object->component_kind != OASP_COMPONENT_STREAM)
        return false;
    oasp_stream *stream = object->native;
    if (oasp_ascii_equal(member, "open")) {
        if (stream->state)
            oasp_runtime_raise(runtime, 3705, "operation is not allowed when the object is open",
                               object->component_name, line);
        else {
            stream->buffer.length = 0;
            stream->buffer.data[0] = '\0';
            stream->position = 0;
            stream->state = 1;
        }
    } else if (oasp_ascii_equal(member, "close")) {
        stream->state = 0;
    } else if (oasp_ascii_equal(member, "flush")) {
        if (!stream->state)
            oasp_runtime_raise(runtime, 3704, "stream is closed", object->component_name, line);
    } else if (oasp_ascii_equal(member, "writetext") || oasp_ascii_equal(member, "write")) {
        char *text = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
        if (!stream->state)
            oasp_runtime_raise(runtime, 3704, "stream is closed", object->component_name, line);
        else if (!text || !stream_write(stream, text, strlen(text)))
            oasp_runtime_raise(runtime, 7, "out of memory", object->component_name, line);
    } else if (oasp_ascii_equal(member, "readtext") || oasp_ascii_equal(member, "read")) {
        if (!stream->state)
            oasp_runtime_raise(runtime, 3704, "stream is closed", object->component_name, line);
        else {
            size_t remaining = stream->position < stream->buffer.length ? stream->buffer.length - stream->position : 0;
            int64_t requested = -1;
            if (argument_count)
                (void)oasp_value_to_int(&arguments[0], &requested);
            size_t length = requested < 0 || (uint64_t)requested > remaining ? remaining : (size_t)requested;
            *result = stream_slice_value(runtime, stream->buffer.data + stream->position, length);
            stream->position += length;
            return true;
        }
    } else if (oasp_ascii_equal(member, "loadfromfile")) {
        char *path = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
        oasp_value contents;
        if (!path || !stream_read_file(runtime, path, &contents)) {
            oasp_runtime_raise(runtime, 53, "file not found", object->component_name, line);
        } else {
            stream->buffer.length = 0;
            stream->buffer.data[0] = '\0';
            stream->position = 0;
            (void)oasp_buffer_append(&stream->buffer, contents.as.string.data, contents.as.string.length);
            *result = contents;
            return true;
        }
    } else if (oasp_ascii_equal(member, "savetofile")) {
        char *path = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
        int64_t mode = 1;
        if (argument_count > 1)
            (void)oasp_value_to_int(&arguments[1], &mode);
        char *host = path ? stream_host_path(runtime, path) : NULL;
        if (!host || (mode != 2 && access(host, F_OK) == 0))
            oasp_runtime_raise(runtime, 58, "file already exists", object->component_name, line);
        else {
            FILE *file = fopen(host, "wb");
            size_t written = file ? fwrite(stream->buffer.data, 1, stream->buffer.length, file) : 0;
            if (!file || written != stream->buffer.length) {
                if (file)
                    fclose(file);
                oasp_runtime_raise(runtime, 70, "permission denied", object->component_name, line);
            } else
                fclose(file);
        }
    } else if (oasp_ascii_equal(member, "copyto")) {
        oasp_stream *target = argument_count && arguments[0].kind == OASP_VALUE_OBJECT &&
                                      arguments[0].as.object->component_kind == OASP_COMPONENT_STREAM
                                  ? arguments[0].as.object->native : NULL;
        size_t source_position = stream->position;
        if (target && stream->type == 2 && target->type == 1 && source_position == 3 &&
            stream->buffer.length >= 3 && memcmp(stream->buffer.data, "\xef\xbb\xbf", 3))
            source_position = 0;
        size_t remaining = source_position < stream->buffer.length ? stream->buffer.length - source_position : 0;
        int64_t requested = (int64_t)remaining;
        if (argument_count > 1)
            (void)oasp_value_to_int(&arguments[1], &requested);
        size_t length = requested < 0 || (uint64_t)requested > remaining ? remaining : (size_t)requested;
        if (!target || !stream_write(target, stream->buffer.data + source_position, length))
            oasp_runtime_raise(runtime, 13, "invalid target stream", object->component_name, line);
        else
            stream->position = source_position + length;
    } else if (oasp_ascii_equal(member, "seteos")) {
        if (stream->position < stream->buffer.length) {
            stream->buffer.length = stream->position;
            stream->buffer.data[stream->position] = '\0';
        }
    } else {
        return false;
    }
    *result = oasp_value_empty();
    return true;
}
