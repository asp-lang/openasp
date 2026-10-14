// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/engine/engine.c`): OpenASP implementation unit. Functions validate
 * caller-provided lengths and ownership before mutating state; local variables make
 * intermediate bounds and lifecycle transitions explicit.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700

#include "openasp.h"

#include "bytecode.h"
#include "component.h"
#include "debugger.h"
#include "internal.h"
#include "module_image.h"
#include "parser.h"
#include "preprocessor.h"
#include "runtime.h"
#include "shared_cache.h"
#include "state_store.h"
#include "vm.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    oasp_arena *arena;
    oasp_binding *bindings;
} oasp_persistent_store;

typedef struct oasp_session_state {
    struct oasp_session_state *next;
    char *id;
    oasp_persistent_store store;
    int timeout;
    time_t touched;
} oasp_session_state;

typedef struct oasp_compiled_page {
    struct oasp_compiled_page *next;
    char *path;
    void *image_data;
    size_t image_size;
    oasp_bytecode bytecode;
    oasp_preprocess_dependency *dependencies;
    size_t dependency_count;
    const void *encoded_modules;
    size_t module_count;
    uint64_t shared_version;
    uint64_t shared_generation;
} oasp_compiled_page;

struct oasp_engine {
    oasp_persistent_store application;
    oasp_session_state *sessions;
    oasp_compiled_page *pages;
    oasp_compiled_page *modules;
    oasp_shared_cache *shared_cache;
    oasp_state_store *state_store;
    oasp_component_context *component_context;
    uint64_t next_session_id;
    time_t global_checked_at;
    bool global_exists;
    bool gen_debug_info;
};

bool oasp_engine_load_external_components(oasp_engine *engine, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                          size_t error_size) {
    return engine && oasp_component_context_load_external(engine->component_context, modules, module_count,
                                                          require_sha256, error, error_size);
}

bool oasp_engine_external_worker_init(oasp_engine *engine, char *error, size_t error_size) {
    return engine && oasp_component_context_external_worker_init(engine->component_context, error, error_size);
}

void oasp_engine_external_worker_shutdown(oasp_engine *engine) {
    if (engine)
        oasp_component_context_external_worker_shutdown(engine->component_context);
}

typedef struct oasp_cache_lease {
    struct oasp_cache_lease *next;
    oasp_shared_cache_view view;
} oasp_cache_lease;

typedef struct {
    oasp_engine *engine;
    oasp_cache_lease *leases;
} oasp_execution_context;

static void oasp_execution_release(oasp_execution_context *context) {
    if (!context)
        return;
    while (context && context->leases) {
        oasp_cache_lease *lease = context->leases;
        context->leases = lease->next;
        oasp_shared_cache_release(context->engine->shared_cache, &lease->view);
        free(lease);
    }
}

static bool oasp_execution_hold(oasp_execution_context *context, const oasp_shared_cache_view *view) {
    oasp_cache_lease *lease = malloc(sizeof(*lease));
    if (!lease)
        return false;
    lease->view = *view;
    lease->next = context->leases;
    context->leases = lease;
    return true;
}

static bool oasp_execution_page(oasp_execution_context *context, const oasp_compiled_page *page, const oasp_bytecode **bytecode) {
    (void)context;
    if (!page || !bytecode)
        return false;
    *bytecode = &page->bytecode;
    return true;
}

static bool oasp_engine_cache_key(const char *path, bool independent_module, bool debug_info, char output[PATH_MAX + 5]) {
    int length = snprintf(output, PATH_MAX + 5, "%c:%c:%s", independent_module ? 'm' : 'p',
                          debug_info ? 'D' : 'N', path);
    return length > 0 && length < PATH_MAX + 5;
}

typedef struct {
    const oasp_oac_options *options;
    const oasp_preprocess_result *preprocessed;
} oasp_fragment_export;

static bool oasp_write_atomic(const char *path, const void *data, size_t length) {
    size_t path_length = strlen(path);
    static const char suffix[] = ".tmp.XXXXXX";
    if (path_length > SIZE_MAX - sizeof(suffix)) {
        errno = ENAMETOOLONG;
        return false;
    }
    char *temporary = malloc(path_length + sizeof(suffix));
    if (!temporary) {
        errno = ENOMEM;
        return false;
    }
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, suffix, sizeof(suffix));
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        free(temporary);
        return false;
    }
    const unsigned char *cursor = data;
    size_t remaining = length;
    while (remaining) {
        ssize_t written = write(descriptor, cursor, remaining);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        cursor += (size_t)written;
        remaining -= (size_t)written;
    }
    bool complete = remaining == 0 && fsync(descriptor) == 0;
    int saved_error = complete ? 0 : errno;
    if (close(descriptor) != 0 && complete) {
        complete = false;
        saved_error = errno;
    }
    if (complete && rename(temporary, path) != 0) {
        complete = false;
        saved_error = errno;
    }
    if (!complete)
        unlink(temporary);
    free(temporary);
    if (!complete)
        errno = saved_error;
    return complete;
}

static bool oasp_export_bytecode(const oasp_bytecode *bytecode, oasp_slice source, const char *source_name, const oasp_fragment_export *export) {
    const oasp_oac_options *options = export->options;
    const oasp_preprocess_result *preprocessed = export->preprocessed;
    oasp_module_image_metadata metadata = {
        .path = source_name,
        .dependencies = preprocessed->dependencies,
        .dependency_count = preprocessed->dependency_count,
        .modules = preprocessed->modules,
        .module_count = preprocessed->module_count,
        .debug_source = options->gen_debug_info ? &source : NULL,
    };
    oasp_buffer encoded;
    if (!oasp_module_image_encode(bytecode, &metadata, &encoded)) {
        fprintf(stderr, "openasp: cannot encode OAC for %s; reduce page metadata or source size and retry\n", source_name);
        return false;
    }
    bool written = oasp_write_atomic(options->output_path, encoded.data, encoded.length);
    if (!written)
        fprintf(stderr, "openasp: cannot write OAC to %s: %s; check the destination directory and permissions\n",
                options->output_path, strerror(errno));
    oasp_buffer_dispose(&encoded);
    return written;
}

static int oasp_execute_fragment(oasp_runtime *runtime, oasp_slice source, const char *source_name, size_t source_line, const oasp_fragment_export *export) {
    oasp_parser parser;
    if (!oasp_parser_init(&parser, runtime->arena, source.data, source.length)) {
        fprintf(stderr, "%s:%zu:%zu: syntax error: %s\n", source_name, source_line + parser.error_line - 1, parser.error_column,
                parser.error ? parser.error : "lexer failed");
        oasp_parser_dispose(&parser);
        return -1;
    }
    oasp_block program;
    if (!oasp_parse_program(&parser, &program)) {
        fprintf(stderr, "%s:%zu:%zu: syntax error: %s\n", source_name, source_line + parser.error_line - 1, parser.error_column,
                parser.error ? parser.error : "parser failed");
        oasp_parser_dispose(&parser);
        return -1;
    }
    oasp_parser_dispose(&parser);
    bool executed;
    if (export && export->options) {
        oasp_bytecode *bytecode = oasp_runtime_alloc(runtime, sizeof(*bytecode), _Alignof(oasp_bytecode));
        if (!bytecode || !oasp_bytecode_compile(runtime->arena, &program, bytecode)) {
            fprintf(stderr, "%s:%zu: compile error: cannot generate bytecode\n", source_name, source_line);
            return -1;
        }
        if (!oasp_export_bytecode(bytecode, source, source_name, export))
            return -1;
        executed = oasp_runtime_execute_bytecode(runtime, bytecode);
    } else {
        executed = oasp_runtime_execute(runtime, &program);
    }
    if (!executed && !runtime->debugger_aborted) {
        fprintf(stderr, "%s:%zu: runtime error %d: %s\n", source_name, source_line + runtime->error_line - 1, runtime->error_number,
                runtime->error_description ? runtime->error_description : "execution failed");
        return -1;
    }
    return executed ? 0 : -1;
}

static bool oasp_append_literal_raw(oasp_buffer *script, const char *literal, size_t length) {
    size_t cursor = 0;
    while (cursor < length) {
        size_t start = cursor;
        while (cursor < length && literal[cursor] != '\r' && literal[cursor] != '\n')
            cursor++;
        if (cursor > start) {
            if (!oasp_buffer_append(script, "Response.Write \"", 16))
                return false;
            for (size_t i = start; i < cursor; i++) {
                if (literal[i] == '"' && !oasp_buffer_append_byte(script, '"'))
                    return false;
                if (!oasp_buffer_append_byte(script, literal[i]))
                    return false;
            }
            if (!oasp_buffer_append(script, "\"\n", 2))
                return false;
        }
        if (cursor < length) {
            bool crlf = literal[cursor] == '\r' && cursor + 1 < length && literal[cursor + 1] == '\n';
            const char *statement = crlf                      ? "Response.Write Chr(13) & Chr(10)\n"
                                    : literal[cursor] == '\r' ? "Response.Write Chr(13)\n"
                                                              : "Response.Write Chr(10)\n";
            if (!oasp_buffer_append(script, statement, strlen(statement)))
                return false;
            cursor += crlf ? 2 : 1;
        }
    }
    return true;
}

static const char *oasp_find_case_range(const char *start, const char *end, const char *needle) {
    size_t length = strlen(needle);
    for (const char *cursor = start; cursor + length <= end; cursor++)
        if (!strncasecmp(cursor, needle, length))
            return cursor;
    return NULL;
}

static const char *oasp_skip_space_range(const char *cursor, const char *end) {
    while (cursor < end && isspace((unsigned char)*cursor))
        cursor++;
    return cursor;
}

static bool oasp_js_identifier_start(unsigned char ch) { return isalpha(ch) || ch == '_' || ch == '$'; }

static bool oasp_js_identifier_part(unsigned char ch) { return isalnum(ch) || ch == '_' || ch == '$'; }

static bool oasp_js_parse_identifier(const char **cursor, const char *end, const char **start, size_t *length) {
    const char *scan = oasp_skip_space_range(*cursor, end);
    if (scan >= end || !oasp_js_identifier_start((unsigned char)*scan))
        return false;
    *start = scan++;
    while (scan < end && oasp_js_identifier_part((unsigned char)*scan))
        scan++;
    *length = (size_t)(scan - *start);
    *cursor = scan;
    return true;
}

static bool oasp_js_find_string(const char *start, const char *end, const char *variable, const char **value, size_t *value_length) {
    size_t variable_length = strlen(variable);
    const char *cursor = start;
    while ((cursor = oasp_find_case_range(cursor, end, "var")) != NULL) {
        if ((cursor > start && oasp_js_identifier_part((unsigned char)cursor[-1])) || (cursor + 3 < end && oasp_js_identifier_part((unsigned char)cursor[3]))) {
            cursor += 3;
            continue;
        }
        const char *scan = oasp_skip_space_range(cursor + 3, end);
        if ((size_t)(end - scan) < variable_length || strncasecmp(scan, variable, variable_length) ||
            (scan + variable_length < end && oasp_js_identifier_part((unsigned char)scan[variable_length]))) {
            cursor += 3;
            continue;
        }
        scan = oasp_skip_space_range(scan + variable_length, end);
        if (scan >= end || *scan != '=') {
            cursor += 3;
            continue;
        }
        scan = oasp_skip_space_range(scan + 1, end);
        if (scan >= end || (*scan != '"' && *scan != '\''))
            return false;
        char quote = *scan++;
        const char *text = scan;
        while (scan < end && *scan != quote) {
            if (*scan == '\\')
                return false;
            scan++;
        }
        if (scan >= end)
            return false;
        *value = text;
        *value_length = (size_t)(scan - text);
        return true;
    }
    return false;
}

static bool oasp_append_vb_string(oasp_buffer *script, const char *value, size_t length) {
    if (!oasp_buffer_append_byte(script, '"'))
        return false;
    for (size_t i = 0; i < length; i++) {
        if (value[i] == '"' && !oasp_buffer_append_byte(script, '"'))
            return false;
        if (!oasp_buffer_append_byte(script, value[i]))
            return false;
    }
    return oasp_buffer_append_byte(script, '"');
}

static int oasp_append_js_character_mapper(oasp_buffer *script, const char *body, const char *end) {
    const char *function = oasp_find_case_range(body, end, "function");
    if (!function || (function > body && oasp_js_identifier_part((unsigned char)function[-1])) ||
        (function + 8 < end && oasp_js_identifier_part((unsigned char)function[8])) || !oasp_find_case_range(body, end, "t.indexOf") ||
        !oasp_find_case_range(body, end, "s.charAt") || !oasp_find_case_range(body, end, "return"))
        return 0;

    const char *cursor = function + 8;
    const char *name;
    const char *argument;
    size_t name_length;
    size_t argument_length;
    if (!oasp_js_parse_identifier(&cursor, end, &name, &name_length))
        return 0;
    cursor = oasp_skip_space_range(cursor, end);
    if (cursor >= end || *cursor++ != '(' || !oasp_js_parse_identifier(&cursor, end, &argument, &argument_length))
        return 0;
    cursor = oasp_skip_space_range(cursor, end);
    if (cursor >= end || *cursor != ')')
        return 0;

    const char *replacement;
    const char *characters;
    size_t replacement_length;
    size_t characters_length;
    if (!oasp_js_find_string(body, end, "s", &replacement, &replacement_length) || !oasp_js_find_string(body, end, "t", &characters, &characters_length))
        return 0;

    if (!oasp_buffer_append(script, "Function ", 9) || !oasp_buffer_append(script, name, name_length) || !oasp_buffer_append_byte(script, '(') ||
        !oasp_buffer_append(script, argument, argument_length) || !oasp_buffer_append(script, ")\n", 2) || !oasp_buffer_append(script, name, name_length) ||
        !oasp_buffer_append(script, " = __OpenASPMapCharacters(", 26) || !oasp_buffer_append(script, argument, argument_length) ||
        !oasp_buffer_append(script, ", ", 2) || !oasp_append_vb_string(script, replacement, replacement_length) || !oasp_buffer_append(script, ", ", 2) ||
        !oasp_append_vb_string(script, characters, characters_length) || !oasp_buffer_append(script, ")\nEnd Function\n", 15))
        return -1;
    return 1;
}

static bool oasp_append_literal_statement(oasp_buffer *script, const char *literal, size_t length) {
    const char *cursor = literal;
    const char *end = literal + length;
    while (cursor < end) {
        const char *opening = oasp_find_case_range(cursor, end, "<script");
        if (!opening)
            return oasp_append_literal_raw(script, cursor, (size_t)(end - cursor));
        const char *tag_end = memchr(opening, '>', (size_t)(end - opening));
        if (!tag_end || !oasp_find_case_range(opening, tag_end, "runat") || !oasp_find_case_range(opening, tag_end, "server")) {
            if (!oasp_append_literal_raw(script, cursor, (size_t)(tag_end ? tag_end + 1 - cursor : end - cursor)))
                return false;
            cursor = tag_end ? tag_end + 1 : end;
            continue;
        }
        const char *closing = oasp_find_case_range(tag_end + 1, end, "</script>");
        if (!closing)
            return oasp_append_literal_raw(script, cursor, (size_t)(opening - cursor));
        if (!oasp_append_literal_raw(script, cursor, (size_t)(opening - cursor)))
            return false;
        int translated = oasp_append_js_character_mapper(script, tag_end + 1, closing);
        if (translated < 0)
            return false;
        if (!translated && oasp_find_case_range(opening, tag_end, "vbscript") &&
            (!oasp_buffer_append(script, tag_end + 1, (size_t)(closing - tag_end - 1)) ||
             !oasp_buffer_append_byte(script, '\n')))
            return false;
        cursor = closing + 9;
    }
    return true;
}

static bool oasp_build_script(const char *source, size_t length, const char *source_name, oasp_buffer *script) {
    if (!oasp_buffer_init(script, length + 1024))
        return false;
    size_t cursor = 0;
    while (cursor < length) {
        const char *open = strstr(source + cursor, "<%");
        if (!open || (size_t)(open - source) >= length) {
            if (!oasp_append_literal_statement(script, source + cursor, length - cursor))
                goto out_of_memory;
            break;
        }
        size_t literal_length = (size_t)(open - (source + cursor));
        if (!oasp_append_literal_statement(script, source + cursor, literal_length))
            goto out_of_memory;

        const char *close = strstr(open + 2, "%>");
        if (!close || (size_t)(close - source) >= length) {
            fprintf(stderr, "%s: syntax error: unterminated ASP block\n", source_name);
            oasp_buffer_dispose(script);
            return false;
        }
        const char *block_source = open + 2;
        while (block_source < close && isspace((unsigned char)*block_source))
            block_source++;
        bool expression = block_source < close && *block_source == '=';
        bool directive = block_source < close && *block_source == '@';
        if (expression || directive)
            block_source++;
        if (!directive) {
            if (expression) {
                bool in_string = false;
                bool in_date = false;
                for (const char *scan = block_source; scan < close; scan++) {
                    if (in_string) {
                        if (*scan == '"' && scan + 1 < close && scan[1] == '"') {
                            scan++;
                        } else if (*scan == '"') {
                            in_string = false;
                        }
                        continue;
                    }
                    if (*scan == '"') {
                        in_string = true;
                        continue;
                    }
                    if (*scan == '#') {
                        in_date = !in_date;
                        continue;
                    }
                    if (!in_date && *scan == ':') {
                        fprintf(stderr,
                                "%s: syntax error: expression block contains "
                                "a statement separator\n",
                                source_name);
                        oasp_buffer_dispose(script);
                        return false;
                    }
                }
            }
            if (expression && !oasp_buffer_append(script, "Response.Write ", 15))
                goto out_of_memory;
            if (!oasp_buffer_append(script, block_source, (size_t)(close - block_source)) || !oasp_buffer_append_byte(script, '\n'))
                goto out_of_memory;
        }
        cursor = (size_t)(close - source) + 2;
    }
    const char *dump_path = getenv("OPENASP_DUMP_SCRIPT");
    if (dump_path && *dump_path) {
        FILE *dump = fopen(dump_path, "wb");
        if (dump) {
            (void)fwrite(script->data, 1, script->length, dump);
            fclose(dump);
        }
    }
    return true;
out_of_memory:
    oasp_buffer_dispose(script);
    return false;
}

static int oasp_render_with_runtime(oasp_runtime *runtime, const char *source, size_t length, const char *source_name, const oasp_fragment_export *export) {
    oasp_buffer script;
    if (!oasp_build_script(source, length, source_name, &script))
        return -1;
    oasp_slice normalized = {script.data, script.length};
    const char *previous_name = runtime->debug_source_name;
    oasp_slice previous_source = runtime->debug_source;
    runtime->debug_source_name = source_name;
    runtime->debug_source = normalized;
    int result = oasp_execute_fragment(runtime, normalized, source_name, 1, export);
    runtime->debug_source_name = previous_name;
    runtime->debug_source = previous_source;
    oasp_buffer_dispose(&script);
    return result;
}

static int oasp_page_codepage(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    oasp_buffer source;
    if (!oasp_buffer_init(&source, 4096)) {
        fclose(file);
        return 0;
    }
    char chunk[4096];
    size_t length;
    while ((length = fread(chunk, 1, sizeof(chunk), file)) != 0) {
        if (!oasp_buffer_append(&source, chunk, length)) {
            oasp_buffer_dispose(&source);
            fclose(file);
            return 0;
        }
    }
    bool read_ok = !ferror(file);
    fclose(file);
    if (!read_ok) {
        oasp_buffer_dispose(&source);
        return 0;
    }
    int inferred = oasp_text_is_utf8(source.data, source.length) ? 0 : 936;
    char *directive = strcasestr(source.data, "<%@");
    if (!directive) {
        oasp_buffer_dispose(&source);
        return inferred;
    }
    char *end = strcasestr(directive + 3, "%>");
    if (!end) {
        oasp_buffer_dispose(&source);
        return inferred;
    }
    *end = '\0';
    char *codepage = strcasestr(directive + 3, "codepage");
    if (!codepage) {
        oasp_buffer_dispose(&source);
        return inferred;
    }
    codepage += 8;
    while (*codepage && (isspace((unsigned char)*codepage) || *codepage == '='))
        codepage++;
    if (*codepage == '"' || *codepage == '\'')
        codepage++;
    char *value_end = NULL;
    errno = 0;
    long value = strtol(codepage, &value_end, 10);
    if (errno || value_end == codepage || value < 0 || value > INT_MAX) {
        oasp_buffer_dispose(&source);
        return inferred;
    }
    oasp_buffer_dispose(&source);
    return (int)value;
}

static const char *oasp_codepage_charset(int codepage) {
    switch (codepage) {
    case 0:
    case 65001:
        return "UTF-8";
    case 936:
        return "GBK";
    case 950:
        return "BIG5";
    case 932:
        return "SHIFT_JIS";
    case 949:
        return "EUC-KR";
    case 1252:
        return "WINDOWS-1252";
    default:
        return NULL;
    }
}

static void oasp_apply_page_codepage(oasp_request *request, const char *path) {
    int codepage = oasp_page_codepage(path);
    if (codepage)
        (void)oasp_request_set_codepage(request, codepage);
}

static bool oasp_preprocess_convert_to_utf8(oasp_preprocess_result *preprocessed, const char *path) {
    int codepage = oasp_page_codepage(path);
    const char *charset = oasp_codepage_charset(codepage);
    if (!codepage && oasp_text_is_utf8(preprocessed->output.data, preprocessed->output.length))
        return true;
    if (!codepage)
        charset = "GBK";
    if (!charset) {
        fprintf(stderr, "%s: unsupported source codepage %d; convert the page to UTF-8 or use a supported CODEPAGE\n", path, codepage);
        return false;
    }
    oasp_buffer converted;
    if (!oasp_text_convert(&converted, preprocessed->output.data, preprocessed->output.length, charset, "UTF-8")) {
        fprintf(stderr, "%s: cannot convert page source from %s to UTF-8: %s; verify the CODEPAGE directive and file encoding\n",
                path, charset, strerror(errno));
        return false;
    }
    oasp_buffer_dispose(&preprocessed->output);
    preprocessed->output = converted;
    return true;
}

int oasp_render_text(oasp_request *request, const char *source, size_t length, const char *source_name) {
    if (!request || !source)
        return -1;
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    return oasp_render_with_runtime(&runtime, source, length, source_name, NULL);
}

static int oasp_render_file_runtime(oasp_runtime *runtime, const char *path, const oasp_oac_options *options) {
    oasp_request *request = runtime->request;
    oasp_request_set_path(request, path);
    oasp_apply_page_codepage(request, path);
    oasp_preprocess_result preprocessed;
    if (!oasp_preprocess_file(oasp_request_root(request), path, &preprocessed)) {
        fprintf(stderr, "%s:%zu: preprocess error: %s\n", preprocessed.error_path[0] ? preprocessed.error_path : path, preprocessed.error_line,
                preprocessed.error ? preprocessed.error : "preprocessing failed");
        oasp_preprocess_dispose(&preprocessed);
        return -1;
    }
    if (!oasp_preprocess_convert_to_utf8(&preprocessed, path)) {
        oasp_preprocess_dispose(&preprocessed);
        return -1;
    }
    char global_path[PATH_MAX];
    int global_length = snprintf(global_path, sizeof(global_path), "%s/global.asa", oasp_request_root(request));
    if (global_length > 0 && (size_t)global_length < sizeof(global_path) && strcmp(path, global_path)) {
        FILE *global_file = fopen(global_path, "rb");
        if (global_file) {
            fclose(global_file);
            oasp_preprocess_result global;
            if (!oasp_preprocess_file(oasp_request_root(request), global_path, &global)) {
                fprintf(stderr, "%s:%zu: preprocess error: %s\n", global.error_path[0] ? global.error_path : global_path, global.error_line,
                        global.error ? global.error : "preprocessing failed");
                oasp_preprocess_dispose(&global);
                oasp_preprocess_dispose(&preprocessed);
                return -1;
            }
            if (!oasp_preprocess_convert_to_utf8(&global, global_path)) {
                oasp_preprocess_dispose(&global);
                oasp_preprocess_dispose(&preprocessed);
                return -1;
            }
            size_t output_length = 0;
            oasp_request_output(request, &output_length);
            int global_result = oasp_render_with_runtime(runtime, global.output.data, global.output.length, global_path, NULL);
            oasp_request_truncate_output(request, output_length);
            oasp_preprocess_dispose(&global);
            if (global_result != 0) {
                oasp_preprocess_dispose(&preprocessed);
                return global_result;
            }
        }
    }
    oasp_fragment_export export = {options, &preprocessed};
    int result = oasp_render_with_runtime(runtime, preprocessed.output.data, preprocessed.output.length, path,
                                          options ? &export : NULL);
    oasp_preprocess_dispose(&preprocessed);
    return result;
}

int oasp_render_file(oasp_request *request, const char *path) {
    if (!request || !path)
        return -1;
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    return oasp_render_file_runtime(&runtime, path, NULL);
}

int oasp_render_file_oac(oasp_request *request, const char *path, const oasp_oac_options *options) {
    if (!request || !path || !options || !options->output_path)
        return -1;
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    return oasp_render_file_runtime(&runtime, path, options);
}

static int oasp_engine_render_file_service_internal(oasp_engine *engine, oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats,
                                                    const oasp_oac_options *options) {
    if (!request || !path || (options && !options->output_path))
        return -1;
    oasp_runtime runtime;
    if (!oasp_runtime_init_service(&runtime, request, config))
        return -1;
    if (engine)
        runtime.component_context = engine->component_context;
    int result = oasp_render_file_runtime(&runtime, path, options);
    oasp_allocator_collect(runtime.allocator, true);
    oasp_runtime_gc_stats(&runtime, stats);
    if (config && config->diagnostics)
        oasp_allocator_dump(runtime.allocator, stderr);
    if (oasp_allocator_failed(runtime.allocator))
        result = -1;
    oasp_runtime_dispose(&runtime);
    return result;
}

int oasp_render_file_service(oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats) {
    return oasp_engine_render_file_service_internal(NULL, request, path, config, stats, NULL);
}

int oasp_render_file_service_oac(oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats, const oasp_oac_options *options) {
    if (!options)
        return -1;
    return oasp_engine_render_file_service_internal(NULL, request, path, config, stats, options);
}

int oasp_engine_render_file_service(oasp_engine *engine, oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats) {
    if (!engine)
        return -1;
    return oasp_engine_render_file_service_internal(engine, request, path, config, stats, NULL);
}

int oasp_engine_render_file_oac(oasp_engine *engine, oasp_request *request, const char *path, const oasp_oac_options *options) {
    if (!engine || !request || !path || !options || !options->output_path)
        return -1;
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    runtime.component_context = engine->component_context;
    return oasp_render_file_runtime(&runtime, path, options);
}

int oasp_engine_render_file_service_oac(oasp_engine *engine, oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats,
                                        const oasp_oac_options *options) {
    if (!engine || !options)
        return -1;
    return oasp_engine_render_file_service_internal(engine, request, path, config, stats, options);
}

int oasp_render_file_debug(oasp_request *request, const char *path, const oasp_debugger *debugger) {
    if (!request || !path || !debugger)
        return -1;
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    oasp_runtime_set_debugger(&runtime, debugger);
    int result = oasp_render_file_runtime(&runtime, path, NULL);
    oasp_runtime_dispose(&runtime);
    return result;
}

int oasp_render_file_service_debug(oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats, const oasp_debugger *debugger) {
    if (!request || !path || !debugger)
        return -1;
    oasp_runtime runtime;
    if (!oasp_runtime_init_service(&runtime, request, config))
        return -1;
    oasp_runtime_set_debugger(&runtime, debugger);
    int result = oasp_render_file_runtime(&runtime, path, NULL);
    oasp_allocator_collect(runtime.allocator, true);
    oasp_runtime_gc_stats(&runtime, stats);
    if (config && config->diagnostics)
        oasp_allocator_dump(runtime.allocator, stderr);
    if (oasp_allocator_failed(runtime.allocator))
        result = -1;
    oasp_runtime_dispose(&runtime);
    return result;
}

oasp_engine *oasp_engine_create(void) { return oasp_engine_create_with_cache(NULL); }

oasp_engine *oasp_engine_create_with_cache(oasp_shared_cache *cache) {
    oasp_process_configure();
    oasp_engine *engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    engine->component_context = oasp_component_context_create();
    if (!engine->component_context) {
        free(engine);
        return NULL;
    }
    engine->shared_cache = cache;
    engine->gen_debug_info = oasp_shared_cache_debug_info(cache);
    return engine;
}

int oasp_engine_enable_shared_state(oasp_engine *engine, const char *directory, const char *root) {
    if (!engine || !directory || !root)
        return -1;
    oasp_state_store *store = oasp_state_store_open(directory, root);
    if (!store)
        return -1;
    oasp_state_store_close(engine->state_store);
    engine->state_store = store;
    return 0;
}

void oasp_engine_release_memory(oasp_engine *engine) {
    if (engine)
        oasp_component_context_release_memory(engine->component_context);
}

static void oasp_persistent_store_dispose(oasp_persistent_store *store) {
    if (!store)
        return;
    oasp_arena_destroy(store->arena);
    memset(store, 0, sizeof(*store));
}

static void oasp_compiled_page_destroy(oasp_compiled_page *page) {
    if (!page)
        return;
    for (size_t i = 0; i < page->dependency_count; i++)
        free(page->dependencies[i].path);
    free(page->dependencies);
    free(page->image_data);
    free(page->path);
    free(page);
}

void oasp_engine_destroy(oasp_engine *engine) {
    if (!engine)
        return;
    oasp_state_store_close(engine->state_store);
    oasp_persistent_store_dispose(&engine->application);
    while (engine->sessions) {
        oasp_session_state *next = engine->sessions->next;
        oasp_persistent_store_dispose(&engine->sessions->store);
        free(engine->sessions->id);
        free(engine->sessions);
        engine->sessions = next;
    }
    while (engine->pages) {
        oasp_compiled_page *next = engine->pages->next;
        oasp_compiled_page_destroy(engine->pages);
        engine->pages = next;
    }
    while (engine->modules) {
        oasp_compiled_page *next = engine->modules->next;
        oasp_compiled_page_destroy(engine->modules);
        engine->modules = next;
    }
    oasp_component_context_destroy(engine->component_context);
    free(engine);
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

static bool oasp_compiled_page_valid(const oasp_compiled_page *page) {
    for (size_t i = 0; i < page->dependency_count; i++) {
        struct stat metadata;
        int64_t modified_nanoseconds;
        int64_t status_nanoseconds;
        if (stat(page->dependencies[i].path, &metadata) || (int64_t)metadata.st_mtime != page->dependencies[i].modified_seconds ||
#if defined(__APPLE__)
            (modified_nanoseconds = (int64_t)metadata.st_mtimespec.tv_nsec) !=
#else
            (modified_nanoseconds = (int64_t)metadata.st_mtim.tv_nsec) !=
#endif
                page->dependencies[i].modified_nanoseconds ||
            (int64_t)metadata.st_size != page->dependencies[i].file_size)
            return false;
        if ((int64_t)metadata.st_ctime != page->dependencies[i].status_seconds ||
#if defined(__APPLE__)
            (status_nanoseconds = (int64_t)metadata.st_ctimespec.tv_nsec) !=
#else
            (status_nanoseconds = (int64_t)metadata.st_ctim.tv_nsec) !=
#endif
                page->dependencies[i].status_nanoseconds) {
            uint64_t content_hash;
            if (oasp_file_content_hash(page->dependencies[i].path, &content_hash) &&
                content_hash != page->dependencies[i].content_hash)
                return false;
        }
    }
    return true;
}

typedef struct {
    oasp_engine *engine;
    const char *root;
} oasp_module_test_context;

static bool oasp_engine_module_parseable(void *context, const char *path) {
    oasp_module_test_context *test = context;
    if (test->engine->shared_cache) {
        char cache_key[PATH_MAX + 5];
        oasp_shared_cache_view view;
        if (!oasp_engine_cache_key(path, true, test->engine->gen_debug_info, cache_key))
            return false;
        if (oasp_shared_cache_acquire(test->engine->shared_cache, cache_key, &view, NULL)) {
            oasp_shared_cache_release(test->engine->shared_cache, &view);
            return true;
        }
        void *disk_image = NULL;
        size_t disk_size = 0;
        if (oasp_shared_cache_disk_load(test->engine->shared_cache, cache_key, &disk_image, &disk_size)) {
            free(disk_image);
            return true;
        }
    }
    oasp_preprocess_result preprocessed;
    if (!oasp_preprocess_file(test->root, path, &preprocessed)) {
        oasp_preprocess_dispose(&preprocessed);
        return false;
    }
    if (!oasp_preprocess_convert_to_utf8(&preprocessed, path)) {
        oasp_preprocess_dispose(&preprocessed);
        return false;
    }
    oasp_buffer script;
    if (!oasp_build_script(preprocessed.output.data, preprocessed.output.length, path, &script)) {
        oasp_preprocess_dispose(&preprocessed);
        return false;
    }
    oasp_arena *arena = oasp_arena_create(4096);
    oasp_parser parser;
    oasp_block program;
    bool initialized = arena && oasp_parser_init(&parser, arena, script.data, script.length);
    bool parseable = initialized && oasp_parse_program(&parser, &program);
    if (arena)
        oasp_parser_dispose(&parser);
    oasp_arena_destroy(arena);
    oasp_buffer_dispose(&script);
    oasp_preprocess_dispose(&preprocessed);
    return parseable;
}

static oasp_compiled_page *oasp_engine_compile_cached(oasp_engine *engine, oasp_compiled_page **cache, const char *root, const char *path, const char *image_path,
                                                      bool independent_module) {
    char canonical[PATH_MAX];
    const char *key = path[0] == '/' ? path : realpath(path, canonical) ? canonical : path;
    oasp_compiled_page **link = cache;
    while (*link) {
        oasp_compiled_page *page = *link;
        if (!strcmp(page->path, key)) {
            if (oasp_compiled_page_valid(page))
                return page;
            *link = page->next;
            oasp_compiled_page_destroy(page);
            break;
        }
        link = &page->next;
    }
    oasp_preprocess_result preprocessed;
    oasp_module_test_context module_test = {engine, root};
    if (!oasp_preprocess_file_hybrid(root, key, oasp_engine_module_parseable, &module_test, &preprocessed)) {
        fprintf(stderr, "%s:%zu: preprocess error: %s\n", preprocessed.error_path[0] ? preprocessed.error_path : key, preprocessed.error_line,
                preprocessed.error ? preprocessed.error : "preprocessing failed");
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    if (!oasp_preprocess_convert_to_utf8(&preprocessed, key)) {
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_buffer script;
    if (!oasp_build_script(preprocessed.output.data, preprocessed.output.length, key, &script)) {
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_arena *arena = oasp_arena_create(16384);
    if (!arena) {
        oasp_buffer_dispose(&script);
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_parser parser;
    oasp_block program;
    oasp_bytecode bytecode;
    if (!oasp_parser_init(&parser, arena, script.data, script.length) || !oasp_parse_program(&parser, &program)) {
        fprintf(stderr, "%s:%zu:%zu: syntax error: %s\n", key, parser.error_line, parser.error_column, parser.error ? parser.error : "parser failed");
        oasp_parser_dispose(&parser);
        oasp_arena_destroy(arena);
        oasp_buffer_dispose(&script);
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_parser_dispose(&parser);
    bool compiled = independent_module || preprocessed.module_count ? oasp_bytecode_compile_module(arena, &program, &bytecode)
                                                                    : oasp_bytecode_compile(arena, &program, &bytecode);
    if (!compiled) {
        oasp_arena_destroy(arena);
        oasp_buffer_dispose(&script);
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_slice debug_source = {script.data, script.length};
    oasp_module_image_metadata metadata = {
        .path = image_path ? image_path : key,
        .dependencies = preprocessed.dependencies,
        .dependency_count = preprocessed.dependency_count,
        .modules = preprocessed.modules,
        .module_count = preprocessed.module_count,
        .debug_source = engine->gen_debug_info ? &debug_source : NULL,
    };
    oasp_buffer encoded;
    if (!oasp_module_image_encode(&bytecode, &metadata, &encoded)) {
        fprintf(stderr, "OpenASP: module image encode failed for %s; bytecode or metadata exceeds the image format limits\n", key);
        oasp_arena_destroy(arena);
        oasp_buffer_dispose(&script);
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_module_image image;
    if (!oasp_module_image_decode(NULL, encoded.data, encoded.length, &image)) {
        fprintf(stderr, "OpenASP: compiled module image validation failed for %s\n", key);
        oasp_buffer_dispose(&encoded);
        oasp_arena_destroy(arena);
        oasp_buffer_dispose(&script);
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    oasp_compiled_page *page = calloc(1, sizeof(*page));
    if (page)
        page->path = strdup(key);
    if (!page || !page->path) {
        oasp_compiled_page_destroy(page);
        oasp_module_image_dispose(&image);
        oasp_buffer_dispose(&encoded);
        oasp_arena_destroy(arena);
        oasp_buffer_dispose(&script);
        oasp_preprocess_dispose(&preprocessed);
        return NULL;
    }
    page->image_data = encoded.data;
    page->image_size = encoded.length;
    page->bytecode = image.bytecode;
    page->encoded_modules = image.encoded_modules;
    page->module_count = image.module_count;
    page->dependencies = preprocessed.dependencies;
    page->dependency_count = preprocessed.dependency_count;
    preprocessed.dependencies = NULL;
    preprocessed.dependency_count = 0;
    preprocessed.dependency_capacity = 0;
    oasp_arena_destroy(arena);
    oasp_buffer_dispose(&script);
    oasp_preprocess_dispose(&preprocessed);
    page->next = *cache;
    *cache = page;
    return page;
}

static oasp_compiled_page *oasp_engine_find_page(oasp_compiled_page *pages, const char *path) {
    for (oasp_compiled_page *page = pages; page; page = page->next)
        if (!strcmp(page->path, path))
            return page;
    return NULL;
}

static void oasp_engine_discard_cached(oasp_compiled_page **pages, oasp_compiled_page *page) {
    oasp_compiled_page **link = pages;
    while (*link && *link != page)
        link = &(*link)->next;
    if (*link) {
        *link = page->next;
        oasp_compiled_page_destroy(page);
    }
}

static oasp_compiled_page *oasp_engine_decode_shared(oasp_compiled_page **cache, const char *path, const oasp_shared_cache_view *view) {
    oasp_compiled_page *page = calloc(1, sizeof(*page));
    if (page)
        page->path = strdup(path);
    oasp_module_image image;
    if (!page || !page->path || !oasp_module_image_decode(NULL, view->data, view->size, &image)) {
        fprintf(stderr, "OpenASP: shared module image decode failed for %s; remove stale opcache files and retry\n", path);
        oasp_compiled_page_destroy(page);
        return NULL;
    }
    page->bytecode = image.bytecode;
    page->encoded_modules = image.encoded_modules;
    page->module_count = image.module_count;
    page->shared_version = view->version;
    page->shared_generation = view->generation;
    oasp_compiled_page *old = oasp_engine_find_page(*cache, path);
    if (old)
        oasp_engine_discard_cached(cache, old);
    page->next = *cache;
    *cache = page;
    return page;
}

static oasp_compiled_page *oasp_engine_shared_compiled(oasp_execution_context *context, const char *root, const char *path, bool independent_module) {
    oasp_engine *engine = context->engine;
    char canonical[PATH_MAX];
    const char *key = path[0] == '/' ? path : realpath(path, canonical) ? canonical : path;
    oasp_compiled_page **cache = independent_module ? &engine->modules : &engine->pages;
    if (!engine->shared_cache)
        return oasp_engine_compile_cached(engine, cache, root, key, key, independent_module);

    char cache_key[PATH_MAX + 5];
    if (!oasp_engine_cache_key(key, independent_module, engine->gen_debug_info, cache_key))
        return NULL;

    uint64_t observed_version = 0;
    oasp_shared_cache_view view;
    bool acquired = oasp_shared_cache_acquire(engine->shared_cache, cache_key, &view, &observed_version);
    if (!acquired && observed_version == 0) {
        void *disk_image = NULL;
        size_t disk_size = 0;
        if (oasp_shared_cache_disk_load(engine->shared_cache, cache_key, &disk_image, &disk_size)) {
            (void)oasp_shared_cache_publish(engine->shared_cache, cache_key, disk_image, disk_size, 0, NULL);
            free(disk_image);
            acquired = oasp_shared_cache_acquire(engine->shared_cache, cache_key, &view, &observed_version);
        }
    }
    if (!acquired) {
        oasp_compiled_page *old = oasp_engine_find_page(*cache, key);
        if (old)
            oasp_engine_discard_cached(cache, old);
        oasp_compiled_page *compiled = oasp_engine_compile_cached(engine, cache, root, key, cache_key, independent_module);
        if (!compiled)
            return NULL;
        bool published = oasp_shared_cache_publish(engine->shared_cache, cache_key, compiled->image_data, compiled->image_size,
                                                   observed_version, NULL);
        if (!published || !oasp_shared_cache_acquire(engine->shared_cache, cache_key, &view, &observed_version))
            return compiled;
    }

    oasp_compiled_page *local = oasp_engine_find_page(*cache, key);
    if (!local || local->shared_version != view.version || local->shared_generation != view.generation)
        local = oasp_engine_decode_shared(cache, key, &view);
    if (!local || !oasp_execution_hold(context, &view)) {
        oasp_shared_cache_release(engine->shared_cache, &view);
        return NULL;
    }
    return local;
}

static bool oasp_engine_resolve_runtime_path(oasp_runtime *runtime, oasp_slice path, char canonical[PATH_MAX]) {
    if (!runtime || !path.length || path.length >= PATH_MAX)
        return false;
    char requested[PATH_MAX];
    for (size_t i = 0; i < path.length; i++)
        requested[i] = path.data[i] == '\\' ? '/' : path.data[i];
    requested[path.length] = '\0';

    const char *root = oasp_request_root(runtime->request);
    size_t root_length = strlen(root);
    bool absolute_under_root =
        requested[0] == '/' && !strncmp(requested, root, root_length) &&
        (requested[root_length] == '\0' || requested[root_length] == '/');
    char joined[PATH_MAX];
    const char *candidate = requested;
    if (!absolute_under_root) {
        const char *relative = requested[0] == '/' ? requested + 1 : requested;
        int length = snprintf(joined, sizeof(joined), "%s/%s", root, relative);
        if (length < 0 || (size_t)length >= sizeof(joined))
            return false;
        candidate = joined;
    }
    if (!realpath(candidate, canonical) || strncmp(canonical, root, root_length) ||
        (canonical[root_length] != '\0' && canonical[root_length] != '/'))
        return false;
    return true;
}

static bool oasp_engine_execute_include(void *context, oasp_runtime *runtime, oasp_slice path) {
    oasp_execution_context *execution = context;
    char canonical[PATH_MAX];
    if (!execution || !oasp_engine_resolve_runtime_path(runtime, path, canonical))
        return false;
    const char *root = oasp_request_root(runtime->request);
    oasp_compiled_page *module = oasp_engine_shared_compiled(execution, root, canonical, true);
    if (!module)
        return false;
    const oasp_bytecode *bytecode;
    if (!oasp_execution_page(execution, module, &bytecode))
        return false;
    if (!runtime->frame)
        return oasp_runtime_execute_bytecode(runtime, bytecode);
    const oasp_bytecode *previous = runtime->bytecode;
    runtime->bytecode = bytecode;
    bool ok = oasp_runtime_register_bytecode_definitions(runtime, bytecode) &&
              oasp_runtime_prepare_bytecode_globals(runtime, bytecode) && oasp_vm_execute(runtime, bytecode);
    runtime->bytecode = previous;
    return ok;
}

static bool oasp_engine_bind_modules(oasp_execution_context *context, oasp_runtime *runtime, const oasp_compiled_page *page, size_t depth) {
    if (depth > 64)
        return false;
    for (size_t i = 0; i < page->module_count; i++) {
        oasp_module_image image = {
            .bytecode = page->bytecode,
            .image_base = page->bytecode.image_base,
            .encoded_modules = page->encoded_modules,
            .module_count = page->module_count,
        };
        oasp_slice module_path;
        char path[PATH_MAX];
        if (!oasp_module_image_module_at(&image, i, &module_path) || module_path.length >= sizeof(path))
            return false;
        memcpy(path, module_path.data, module_path.length);
        path[module_path.length] = '\0';
        const char *root = oasp_request_root(runtime->request);
        oasp_compiled_page *module = oasp_engine_shared_compiled(context, root, path, true);
        const oasp_bytecode *bytecode;
        if (!module || !oasp_execution_page(context, module, &bytecode) ||
            !oasp_engine_bind_modules(context, runtime, module, depth + 1) ||
            !oasp_runtime_register_bytecode_definitions(runtime, bytecode) || !oasp_runtime_prepare_bytecode_globals(runtime, bytecode))
            return false;
    }
    return true;
}

static bool oasp_engine_collect_state_effects(oasp_execution_context *context, const char *root, const oasp_compiled_page *page, size_t depth, uint32_t *effects) {
    if (!context || !page || !effects || depth > 64)
        return false;
    *effects |= page->bytecode.state_effects;
    for (size_t i = 0; i < page->module_count; i++) {
        oasp_module_image image = {
            .bytecode = page->bytecode,
            .image_base = page->bytecode.image_base,
            .encoded_modules = page->encoded_modules,
            .module_count = page->module_count,
        };
        oasp_slice module_path;
        char path[PATH_MAX];
        if (!oasp_module_image_module_at(&image, i, &module_path) || module_path.length >= sizeof(path))
            return false;
        memcpy(path, module_path.data, module_path.length);
        path[module_path.length] = '\0';
        oasp_compiled_page *module = oasp_engine_shared_compiled(context, root, path, true);
        if (!module || !oasp_engine_collect_state_effects(context, root, module, depth + 1, effects))
            return false;
    }
    return true;
}

static bool oasp_engine_global_exists(oasp_engine *engine, const char *path) {
    long interval = engine->shared_cache ? oasp_shared_cache_validation_interval(engine->shared_cache) : 0;
    time_t now = time(NULL);
    bool check = !engine->global_checked_at || interval == 0 ||
                 (interval > 0 && now - engine->global_checked_at >= interval);
    if (check) {
        struct stat metadata;
        engine->global_exists = stat(path, &metadata) == 0;
        engine->global_checked_at = now;
    }
    return engine->global_exists;
}

static bool oasp_engine_page_state_effects(oasp_execution_context *context, oasp_request *request, const char *path, uint32_t *effects) {
    const char *root = oasp_request_root(request);
    oasp_compiled_page *page = oasp_engine_shared_compiled(context, root, path, false);
    if (!page || !oasp_engine_collect_state_effects(context, root, page, 0, effects))
        return false;
    char global_path[PATH_MAX];
    int length = snprintf(global_path, sizeof(global_path), "%s/global.asa", root);
    if (length > 0 && (size_t)length < sizeof(global_path) && strcmp(path, global_path) &&
        oasp_engine_global_exists(context->engine, global_path)) {
        oasp_compiled_page *global = oasp_engine_shared_compiled(context, root, global_path, false);
        if (!global || !oasp_engine_collect_state_effects(context, root, global, 0, effects))
            return false;
    }
    return true;
}

static void oasp_engine_discard_page(oasp_engine *engine, oasp_compiled_page *page) { oasp_engine_discard_cached(&engine->pages, page); }

static int oasp_engine_execute_page(oasp_execution_context *context, oasp_runtime *runtime, const char *path) {
    oasp_engine *engine = context->engine;
    oasp_compiled_page *page = oasp_engine_shared_compiled(context, oasp_request_root(runtime->request), path, false);
    if (!page)
        return -1;
    if (!oasp_engine_bind_modules(context, runtime, page, 0)) {
        oasp_engine_discard_page(engine, page);
        page = oasp_engine_shared_compiled(context, oasp_request_root(runtime->request), path, false);
        if (!page || !oasp_engine_bind_modules(context, runtime, page, 0))
            return -1;
    }
    const oasp_bytecode *bytecode;
    if (!oasp_execution_page(context, page, &bytecode))
        return -1;
    if (!oasp_runtime_execute_bytecode(runtime, bytecode)) {
        fprintf(stderr, "%s:%zu: runtime error %d: %s\n", page->path, runtime->error_line, runtime->error_number,
                runtime->error_description ? runtime->error_description : "execution failed");
        return -1;
    }
    return 0;
}

static bool oasp_engine_execute_transfer(void *context, oasp_runtime *runtime, oasp_slice path) {
    oasp_execution_context *execution = context;
    char canonical[PATH_MAX];
    if (!execution || !runtime || !oasp_engine_resolve_runtime_path(runtime, path, canonical))
        return false;

    oasp_runtime transferred;
    oasp_runtime_init_state(&transferred, runtime->request, runtime->session, runtime->application,
                            runtime->session_timeout);
    transferred.component_context = runtime->component_context;
    transferred.include_handler = oasp_engine_execute_include;
    transferred.transfer_handler = oasp_engine_execute_transfer;
    transferred.include_context = execution;
    bool ok = oasp_engine_execute_page(execution, &transferred, canonical) == 0;
    runtime->session = transferred.session;
    runtime->application = transferred.application;
    runtime->session_timeout = transferred.session_timeout;
    runtime->session_abandoned = transferred.session_abandoned;
    if (!ok && transferred.error_number) {
        runtime->error_number = transferred.error_number;
        runtime->error_description = transferred.error_description;
        runtime->error_source = transferred.error_source;
        runtime->error_line = transferred.error_line;
        runtime->error_generation++;
        runtime->error_pending = true;
    }
    return ok;
}

static int oasp_engine_render_cached(oasp_execution_context *context, oasp_runtime *runtime, const char *path) {
    oasp_request_set_path(runtime->request, path);
    char global_path[PATH_MAX];
    int length = snprintf(global_path, sizeof(global_path), "%s/global.asa", oasp_request_root(runtime->request));
    if (length > 0 && (size_t)length < sizeof(global_path) && strcmp(path, global_path)) {
        if (oasp_engine_global_exists(context->engine, global_path)) {
            size_t output_length = 0;
            oasp_request_output(runtime->request, &output_length);
            int result = oasp_engine_execute_page(context, runtime, global_path);
            oasp_request_truncate_output(runtime->request, output_length);
            if (result)
                return result;
        }
    }
    return oasp_engine_execute_page(context, runtime, path);
}

static bool oasp_persistent_store_replace(oasp_persistent_store *store, const oasp_binding *bindings) {
    oasp_arena *arena = oasp_arena_create(4096);
    if (!arena)
        return false;
    oasp_runtime destination;
    memset(&destination, 0, sizeof(destination));
    destination.arena = arena;
    oasp_binding *copy = oasp_runtime_clone_store(&destination, bindings);
    if (bindings && !copy) {
        oasp_arena_destroy(arena);
        return false;
    }
    oasp_arena_destroy(store->arena);
    store->arena = arena;
    store->bindings = copy;
    return true;
}

static void oasp_engine_new_session_id(oasp_engine *engine, char output[33]) {
    unsigned char random_bytes[16];
    FILE *random = fopen("/dev/urandom", "rb");
    bool generated = random && fread(random_bytes, 1, sizeof(random_bytes), random) == sizeof(random_bytes);
    if (random)
        fclose(random);
    if (!generated) {
        uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)(unsigned)getpid() << 32) ^ ++engine->next_session_id;
        for (size_t i = 0; i < sizeof(random_bytes); i++) {
            seed = seed * UINT64_C(6364136223846793005) + 1;
            random_bytes[i] = (unsigned char)(seed >> 56);
        }
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        output[i * 2] = digits[random_bytes[i] >> 4];
        output[i * 2 + 1] = digits[random_bytes[i] & 15];
    }
    output[32] = '\0';
}

static oasp_session_state *oasp_engine_session(oasp_engine *engine, oasp_request *request) {
    const char *selected = oasp_request_cookie_value(request, "OPENASPID");
    if (!*selected)
        selected = NULL;
    time_t now = time(NULL);
    oasp_session_state **link = &engine->sessions;
    while (*link) {
        oasp_session_state *session = *link;
        if (session->timeout > 0 && now - session->touched > (time_t)session->timeout * 60) {
            *link = session->next;
            oasp_persistent_store_dispose(&session->store);
            free(session->id);
            free(session);
            continue;
        }
        if (selected && !strcmp(session->id, selected)) {
            session->touched = now;
            return session;
        }
        link = &session->next;
    }
    oasp_session_state *session = calloc(1, sizeof(*session));
    if (!session)
        return NULL;
    char generated_id[33];
    if (!selected) {
        oasp_engine_new_session_id(engine, generated_id);
        selected = generated_id;
    }
    size_t id_length = strlen(selected);
    session->id = malloc(id_length + 1);
    if (!session->id) {
        free(session);
        return NULL;
    }
    memcpy(session->id, selected, id_length + 1);
    session->timeout = 20;
    session->touched = now;
    session->next = engine->sessions;
    engine->sessions = session;
    if (!*oasp_request_cookie_value(request, "OPENASPID")) {
        (void)oasp_request_set_cookie(request, "OPENASPID", session->id);
        (void)oasp_request_set_cookie_attribute(request, "OPENASPID", "Path", "/");
        (void)oasp_request_set_cookie_attribute(request, "OPENASPID", "HttpOnly", "True");
    }
    return session;
}

static int oasp_engine_render_file_with_debugger(oasp_engine *engine, oasp_request *request, const char *path, const oasp_debugger *debugger) {
    if (!engine || !request || !path)
        return -1;
    oasp_apply_page_codepage(request, path);
    oasp_execution_context execution = {.engine = engine};
    uint32_t state_effects = 0;
    if (!oasp_engine_page_state_effects(&execution, request, path, &state_effects)) {
        oasp_execution_release(&execution);
        return -1;
    }
    bool reads_session = (state_effects & (OASP_STATE_READS_SESSION | OASP_STATE_WRITES_SESSION)) != 0;
    bool writes_session = (state_effects & OASP_STATE_WRITES_SESSION) != 0;
    bool reads_application = (state_effects & (OASP_STATE_READS_APPLICATION | OASP_STATE_WRITES_APPLICATION)) != 0;
    bool writes_application = (state_effects & OASP_STATE_WRITES_APPLICATION) != 0;
    if (engine->state_store) {
        const char *id = reads_session ? oasp_request_cookie_value(request, "OPENASPID") : NULL;
        char generated_id[33];
        if (reads_session && !*id) {
            oasp_engine_new_session_id(engine, generated_id);
            id = generated_id;
            (void)oasp_request_set_cookie(request, "OPENASPID", id);
            (void)oasp_request_set_cookie_attribute(request, "OPENASPID", "Path", "/");
            (void)oasp_request_set_cookie_attribute(request, "OPENASPID", "HttpOnly", "True");
        }
        if (reads_session && !oasp_state_store_lock_session(engine->state_store, id)) {
            oasp_execution_release(&execution);
            return -1;
        }
        oasp_runtime runtime;
        oasp_runtime_init(&runtime, request);
        oasp_runtime_set_debugger(&runtime, debugger);
        runtime.component_context = engine->component_context;
        bool loaded = !reads_session ||
                      oasp_state_store_load_session(engine->state_store, id, &runtime, &runtime.session,
                                                    &runtime.session_timeout, time(NULL));
        if (loaded && reads_application)
            loaded = oasp_state_store_load_application(engine->state_store, &runtime, &runtime.application);
        oasp_binding *application_before =
            loaded && writes_application ? oasp_runtime_clone_store(&runtime, runtime.application) : NULL;
        if (loaded && writes_application && runtime.application && !application_before)
            loaded = false;
        runtime.include_handler = oasp_engine_execute_include;
        runtime.transfer_handler = oasp_engine_execute_transfer;
        runtime.include_context = &execution;
        int result = loaded ? oasp_engine_render_cached(&execution, &runtime, path) : -1;
        if (getenv("OPENASP_TRACE_ENGINE"))
            fprintf(stderr,
                    "OpenASP engine trace: rendered path=%s result=%d loaded=%d error=%d pending=%d line=%zu "
                    "request_failed=%d request_error=%s stopped=%d\n",
                    path, result, loaded ? 1 : 0, runtime.error_number, runtime.error_pending ? 1 : 0,
                    runtime.error_line, oasp_request_failed(request), oasp_request_error(request),
                    runtime.stopped ? 1 : 0);
        bool application_saved = true;
        bool session_saved = true;
        if (result == 0 && loaded && writes_application)
            application_saved = oasp_state_store_merge_application(engine->state_store, application_before, runtime.application);
        if (!application_saved)
            result = -1;
        if (result == 0 && loaded && writes_session) {
            session_saved = runtime.session_abandoned
                                ? oasp_state_store_delete_session(engine->state_store, id)
                                : oasp_state_store_save_session(engine->state_store, id, runtime.session,
                                                                runtime.session_timeout, time(NULL));
            if (!session_saved)
                result = -1;
        }
        if (getenv("OPENASP_TRACE_ENGINE"))
            fprintf(stderr,
                    "OpenASP engine trace: state effects=0x%x reads_session=%d writes_session=%d "
                    "reads_application=%d writes_application=%d application_saved=%d session_saved=%d result=%d\n",
                    state_effects, reads_session ? 1 : 0, writes_session ? 1 : 0,
                    reads_application ? 1 : 0, writes_application ? 1 : 0,
                    application_saved ? 1 : 0, session_saved ? 1 : 0, result);
        if (reads_session)
            oasp_state_store_unlock_session(engine->state_store, id);
        oasp_execution_release(&execution);
        return result;
    }
    oasp_session_state *session = reads_session ? oasp_engine_session(engine, request) : NULL;
    if (reads_session && !session) {
        oasp_execution_release(&execution);
        return -1;
    }
    oasp_runtime runtime;
    oasp_runtime_init_state(&runtime, request, session ? session->store.bindings : NULL,
                            reads_application ? engine->application.bindings : NULL,
                            session ? session->timeout : 20);
    oasp_runtime_set_debugger(&runtime, debugger);
    runtime.component_context = engine->component_context;
    runtime.include_handler = oasp_engine_execute_include;
    runtime.transfer_handler = oasp_engine_execute_transfer;
    runtime.include_context = &execution;
    int result = oasp_engine_render_cached(&execution, &runtime, path);
    if (getenv("OPENASP_TRACE_ENGINE"))
        fprintf(stderr,
                "OpenASP engine trace: rendered path=%s result=%d error=%d pending=%d line=%zu "
                "request_failed=%d request_error=%s stopped=%d\n",
                path, result, runtime.error_number, runtime.error_pending ? 1 : 0, runtime.error_line,
                oasp_request_failed(request), oasp_request_error(request), runtime.stopped ? 1 : 0);
    if (result == 0 && writes_application &&
        !oasp_persistent_store_replace(&engine->application, runtime.application))
        result = -1;
    if (result == 0 && writes_session && runtime.session_abandoned) {
        oasp_persistent_store_dispose(&session->store);
        session->timeout = 20;
    } else if (result == 0 && writes_session) {
        if (!oasp_persistent_store_replace(&session->store, runtime.session))
            result = -1;
        session->timeout = runtime.session_timeout;
    }
    if (reads_session)
        session->touched = time(NULL);
    oasp_execution_release(&execution);
    return result;
}

int oasp_engine_render_file(oasp_engine *engine, oasp_request *request, const char *path) {
    return oasp_engine_render_file_with_debugger(engine, request, path, NULL);
}

int oasp_engine_render_file_debug(oasp_engine *engine, oasp_request *request, const char *path, const oasp_debugger *debugger) {
    if (!debugger)
        return -1;
    return oasp_engine_render_file_with_debugger(engine, request, path, debugger);
}
