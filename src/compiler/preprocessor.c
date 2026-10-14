// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/preprocessor.c`): Compiler pipeline. Arena-owned tokens, AST
 * nodes, symbols, bytecode, and immutable images move through this stage without per-node
 * frees. Length/count variables bound every indexed operation and malformed input must fail
 * before execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700

#include "preprocessor.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct oasp_include_frame {
    struct oasp_include_frame *parent;
    char path[PATH_MAX];
} oasp_include_frame;

static bool oasp_include_intent(const char *begin, const char *end) {
    while (begin < end && isspace((unsigned char)*begin))
        begin++;
    static const char keyword[] = "#include";
    size_t length = sizeof(keyword) - 1;
    return (size_t)(end - begin) >= length && !strncasecmp(begin, keyword, length) &&
           ((size_t)(end - begin) == length || isspace((unsigned char)begin[length]));
}

static bool oasp_record_module(oasp_preprocess_result *result, const char *path) {
    for (size_t i = 0; i < result->module_count; i++)
        if (!strcmp(result->modules[i], path))
            return true;
    if (result->module_count == result->module_capacity) {
        size_t capacity = result->module_capacity ? result->module_capacity * 2 : 8;
        char **grown = realloc(result->modules, capacity * sizeof(*grown));
        if (!grown)
            return false;
        result->modules = grown;
        result->module_capacity = capacity;
    }
    size_t length = strlen(path);
    char *copy = malloc(length + 1);
    if (!copy)
        return false;
    memcpy(copy, path, length + 1);
    result->modules[result->module_count++] = copy;
    return true;
}

static bool oasp_append_module_marker(oasp_preprocess_result *result, const char *path) {
    static const char prefix[] = "<% Call __OpenASP_Include(\"";
    static const char suffix[] = "\") %>";
    if (!oasp_buffer_append(&result->output, prefix, sizeof(prefix) - 1))
        return false;
    for (const char *cursor = path; *cursor; cursor++) {
        if (*cursor == '"' && !oasp_buffer_append_byte(&result->output, '"'))
            return false;
        if (!oasp_buffer_append_byte(&result->output, *cursor))
            return false;
    }
    return oasp_buffer_append(&result->output, suffix, sizeof(suffix) - 1);
}

static bool oasp_file_content_hash(const char *path, uint64_t *result) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned char buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        for (size_t i = 0; i < count; i++) {
            hash ^= buffer[i];
            hash *= UINT64_C(1099511628211);
        }
    bool ok = !ferror(file) && fclose(file) == 0;
    if (ok)
        *result = hash;
    return ok;
}

static bool oasp_record_dependency(oasp_preprocess_result *result, const char *path) {
    struct stat metadata;
    if (stat(path, &metadata))
        return false;
    if (result->dependency_count == result->dependency_capacity) {
        size_t capacity = result->dependency_capacity ? result->dependency_capacity * 2 : 8;
        oasp_preprocess_dependency *grown = realloc(result->dependencies, capacity * sizeof(*grown));
        if (!grown)
            return false;
        result->dependencies = grown;
        result->dependency_capacity = capacity;
    }
    oasp_preprocess_dependency *dependency = &result->dependencies[result->dependency_count++];
    size_t length = strlen(path);
    dependency->path = malloc(length + 1);
    if (!dependency->path) {
        result->dependency_count--;
        return false;
    }
    memcpy(dependency->path, path, length + 1);
    dependency->modified_seconds = (int64_t)metadata.st_mtime;
#if defined(__APPLE__)
    dependency->modified_nanoseconds = (int64_t)metadata.st_mtimespec.tv_nsec;
#else
    dependency->modified_nanoseconds = (int64_t)metadata.st_mtim.tv_nsec;
#endif
    dependency->status_seconds = (int64_t)metadata.st_ctime;
#if defined(__APPLE__)
    dependency->status_nanoseconds = (int64_t)metadata.st_ctimespec.tv_nsec;
#else
    dependency->status_nanoseconds = (int64_t)metadata.st_ctim.tv_nsec;
#endif
    dependency->file_size = (int64_t)metadata.st_size;
    if (!oasp_file_content_hash(path, &dependency->content_hash)) {
        free(dependency->path);
        result->dependency_count--;
        return false;
    }
    return true;
}

static bool oasp_read_file(const char *path, oasp_buffer *output) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    char block[16384];
    size_t read;
    while ((read = fread(block, 1, sizeof(block), file)) != 0) {
        if (!oasp_buffer_append(output, block, read)) {
            fclose(file);
            return false;
        }
    }
    bool ok = !ferror(file);
    fclose(file);
    return ok;
}

static bool oasp_path_under_root(const char *root, const char *path) {
    size_t length = strlen(root);
    return !strncmp(root, path, length) && (path[length] == '\0' || path[length] == '/');
}

static bool oasp_directory(const char *path, char output[PATH_MAX]) {
    size_t length = strlen(path);
    if (length >= PATH_MAX)
        return false;
    memcpy(output, path, length + 1);
    char *slash = strrchr(output, '/');
    if (!slash) {
        memcpy(output, ".", 2);
        return true;
    }
    if (slash == output)
        slash[1] = '\0';
    else
        *slash = '\0';
    return true;
}

static bool oasp_join_path(const char *base, const char *relative, char output[PATH_MAX]) {
    int length = snprintf(output, PATH_MAX, "%s/%s", base, relative);
    return length >= 0 && length < PATH_MAX;
}

static const char *oasp_find_case(const char *start, const char *end, const char *needle) {
    size_t length = strlen(needle);
    if (!length)
        return start;
    for (const char *cursor = start; cursor + length <= end; cursor++) {
        if (!strncasecmp(cursor, needle, length))
            return cursor;
    }
    return NULL;
}

static bool oasp_extract_include(const char *start, const char *end, bool *virtual_path, char path[PATH_MAX]) {
    const char *cursor = start;
    while (cursor < end && isspace((unsigned char)*cursor))
        cursor++;
    if (cursor >= end || *cursor != '#')
        return false;
    cursor++;
    while (cursor < end && isspace((unsigned char)*cursor))
        cursor++;
    if (end - cursor < 7 || strncasecmp(cursor, "include", 7))
        return false;
    cursor += 7;
    while (cursor < end && isspace((unsigned char)*cursor))
        cursor++;
    const char *name = cursor;
    while (cursor < end && isalpha((unsigned char)*cursor))
        cursor++;
    size_t name_length = (size_t)(cursor - name);
    if (oasp_ascii_equal_n(name, name_length, "file"))
        *virtual_path = false;
    else if (oasp_ascii_equal_n(name, name_length, "virtual"))
        *virtual_path = true;
    else
        return false;
    while (cursor < end && isspace((unsigned char)*cursor))
        cursor++;
    if (cursor >= end || *cursor++ != '=')
        return false;
    while (cursor < end && isspace((unsigned char)*cursor))
        cursor++;
    if (cursor >= end || (*cursor != '"' && *cursor != '\''))
        return false;
    char quote = *cursor++;
    const char *value = cursor;
    while (cursor < end && *cursor != quote)
        cursor++;
    size_t length = (size_t)(cursor - value);
    if (cursor >= end || length == 0 || length >= PATH_MAX)
        return false;
    for (size_t i = 0; i < length; i++)
        path[i] = value[i] == '\\' ? '/' : value[i];
    path[length] = '\0';
    return true;
}

static bool oasp_expand(const char *root, const char *path, oasp_preprocess_result *result, oasp_include_frame *parent, size_t depth,
                        oasp_preprocess_module_test module_test, void *module_context) {
    if (depth > 64) {
        result->error = "include nesting exceeds 64 levels";
        return false;
    }
    char canonical[PATH_MAX];
    if (!realpath(path, canonical)) {
        result->error = "include file does not exist";
        snprintf(result->error_path, sizeof(result->error_path), "%s", path);
        return false;
    }
    if (!oasp_path_under_root(root, canonical)) {
        result->error = "include path escapes site root";
        snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
        return false;
    }
    if (!oasp_record_dependency(result, canonical)) {
        result->error = "cannot record source dependency";
        snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
        return false;
    }
    for (oasp_include_frame *frame = parent; frame; frame = frame->parent) {
        if (!strcmp(frame->path, canonical)) {
            result->error = "recursive include detected";
            snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
            return false;
        }
    }
    oasp_include_frame frame = {.parent = parent};
    snprintf(frame.path, sizeof(frame.path), "%s", canonical);
    oasp_buffer source;
    if (!oasp_buffer_init(&source, 4096))
        return false;
    if (!oasp_read_file(canonical, &source)) {
        result->error = "cannot read source file";
        snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
        oasp_buffer_dispose(&source);
        return false;
    }
    const char *cursor = source.data;
    const char *end = source.data + source.length;
    if (source.length >= 3 && (unsigned char)source.data[0] == 0xef && (unsigned char)source.data[1] == 0xbb && (unsigned char)source.data[2] == 0xbf)
        cursor += 3;
    size_t line = 1;
    while (cursor < end) {
        const char *include = oasp_find_case(cursor, end, "<!--");
        const char *directive = oasp_find_case(cursor, end, "<%@");
        const char *next = include;
        bool page_directive = false;
        if (!next || (directive && directive < next)) {
            next = directive;
            page_directive = directive != NULL;
        }
        if (!next) {
            if (!oasp_buffer_append(&result->output, cursor, (size_t)(end - cursor)))
                goto oom;
            break;
        }
        if (!oasp_buffer_append(&result->output, cursor, (size_t)(next - cursor)))
            goto oom;
        for (const char *scan = cursor; scan < next; scan++)
            if (*scan == '\n')
                line++;
        if (page_directive) {
            const char *close = oasp_find_case(next + 3, end, "%>");
            if (!close) {
                result->error = "unterminated page directive";
                result->error_line = line;
                snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
                oasp_buffer_dispose(&source);
                return false;
            }
            for (const char *scan = next; scan < close + 2; scan++)
                if (*scan == '\n') {
                    if (!oasp_buffer_append_byte(&result->output, '\n'))
                        goto oom;
                    line++;
                }
            cursor = close + 2;
            if (cursor < end && *cursor == '\r') {
                cursor++;
                if (cursor < end && *cursor == '\n')
                    cursor++;
                line++;
            } else if (cursor < end && *cursor == '\n') {
                cursor++;
                line++;
            }
            continue;
        }
        const char *close = oasp_find_case(next + 4, end, "-->");
        if (!close) {
            if (oasp_include_intent(next + 4, end)) {
                result->error = "unterminated include directive";
                result->error_line = line;
                snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
                oasp_buffer_dispose(&source);
                return false;
            }
            if (!oasp_buffer_append(&result->output, next, (size_t)(end - next)))
                goto oom;
            break;
        }
        bool virtual_path = false;
        char include_path[PATH_MAX];
        if (!oasp_extract_include(next + 4, close, &virtual_path, include_path)) {
            if (oasp_include_intent(next + 4, close)) {
                result->error = "invalid include directive";
                result->error_line = line;
                snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
                oasp_buffer_dispose(&source);
                return false;
            }
            if (!oasp_buffer_append(&result->output, next, (size_t)(close + 3 - next)))
                goto oom;
            cursor = close + 3;
            continue;
        }
        char base[PATH_MAX];
        char joined[PATH_MAX];
        if (virtual_path) {
            snprintf(base, sizeof(base), "%s", root);
            const char *relative = include_path;
            while (*relative == '/' || *relative == '\\')
                relative++;
            if (!oasp_join_path(base, relative, joined))
                goto path_error;
        } else {
            if (!oasp_directory(canonical, base) || !oasp_join_path(base, include_path, joined))
                goto path_error;
        }
        char included_canonical[PATH_MAX];
        if (!realpath(joined, included_canonical)) {
            result->error = "include file does not exist";
            result->error_line = line;
            snprintf(result->error_path, sizeof(result->error_path), "%s", joined);
            oasp_buffer_dispose(&source);
            return false;
        }
        if (!oasp_path_under_root(root, included_canonical)) {
            result->error = "include path escapes site root";
            result->error_line = line;
            snprintf(result->error_path, sizeof(result->error_path), "%s", included_canonical);
            oasp_buffer_dispose(&source);
            return false;
        }
        bool use_module = module_test && module_test(module_context, included_canonical);
        if (use_module) {
            if (!oasp_record_module(result, included_canonical) || !oasp_append_module_marker(result, included_canonical))
                goto oom;
        } else if (!oasp_expand(root, included_canonical, result, &frame, depth + 1, module_test, module_context)) {
            if (!result->error_line)
                result->error_line = line;
            oasp_buffer_dispose(&source);
            return false;
        }
        for (const char *scan = next; scan < close + 3; scan++)
            if (*scan == '\n')
                line++;
        cursor = close + 3;
    }
    oasp_buffer_dispose(&source);
    return true;
path_error:
    result->error = "include path is too long";
    result->error_line = line;
    snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
    oasp_buffer_dispose(&source);
    return false;
oom:
    result->error = "out of memory while expanding includes";
    result->error_line = line;
    snprintf(result->error_path, sizeof(result->error_path), "%s", canonical);
    oasp_buffer_dispose(&source);
    return false;
}

bool oasp_preprocess_file(const char *root, const char *path, oasp_preprocess_result *result) {
    return oasp_preprocess_file_hybrid(root, path, NULL, NULL, result);
}

bool oasp_preprocess_file_hybrid(const char *root, const char *path, oasp_preprocess_module_test module_test, void *module_context, oasp_preprocess_result *result) {
    if (!root || !path || !result)
        return false;
    memset(result, 0, sizeof(*result));
    if (!oasp_buffer_init(&result->output, 16384))
        return false;
    char canonical_root[PATH_MAX];
    if (!realpath(root, canonical_root)) {
        result->error = "site root does not exist";
        snprintf(result->error_path, sizeof(result->error_path), "%s", root);
        return false;
    }
    return oasp_expand(canonical_root, path, result, NULL, 0, module_test, module_context);
}

void oasp_preprocess_dispose(oasp_preprocess_result *result) {
    if (!result)
        return;
    for (size_t i = 0; i < result->dependency_count; i++)
        free(result->dependencies[i].path);
    for (size_t i = 0; i < result->module_count; i++)
        free(result->modules[i]);
    free(result->dependencies);
    free(result->modules);
    oasp_buffer_dispose(&result->output);
    memset(result, 0, sizeof(*result));
}
