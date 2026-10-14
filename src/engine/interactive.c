// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/engine/interactive.c`): OpenASP implementation unit. Functions validate
 * caller-provided lengths and ownership before mutating state; local variables make
 * intermediate bounds and lifecycle transitions explicit.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "interactive.h"

#include "component.h"
#include "internal.h"
#include "parser.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct oasp_interactive {
    oasp_request *request;
    oasp_runtime runtime;
    oasp_component_context *component_context;
};

static bool oasp_interactive_incomplete(const oasp_parser *parser) {
    if (!parser || !parser->error || oasp_parser_current(parser)->kind != OASP_TOKEN_EOF)
        return false;
    static const char *const messages[] = {
        "expected matching End statement",
        "expected end of routine declaration",
        "expected end of class declaration",
        "expected End If",
        "expected Wend",
        "expected Loop",
        "expected Next",
        "expected End Select",
        "expected End With"
    };
    for (size_t i = 0; i < OASP_ARRAY_COUNT(messages); i++)
        if (!strcmp(parser->error, messages[i]))
            return true;
    return false;
}

oasp_interactive *oasp_interactive_create(const char *root, size_t memory_limit) {
    oasp_interactive *interactive = calloc(1, sizeof(*interactive));
    if (!interactive)
        return NULL;
    interactive->request = oasp_request_create(root ? root : ".", "", "", "CLI");
    interactive->component_context = oasp_component_context_create();
    oasp_gc_config config = {0};
    oasp_request_limits limits = {.memory_limit = memory_limit};
    if (!interactive->request || !interactive->component_context ||
        oasp_request_set_limits(interactive->request, &limits) ||
        !oasp_runtime_init_service(&interactive->runtime, interactive->request, &config)) {
        if (interactive->request && oasp_request_failed(interactive->request))
            fprintf(stderr, "openasp-cli: %s\n", oasp_request_error(interactive->request));
        oasp_component_context_destroy(interactive->component_context);
        oasp_request_destroy(interactive->request);
        free(interactive);
        return NULL;
    }
    interactive->runtime.component_context = interactive->component_context;
    return interactive;
}

void oasp_interactive_destroy(oasp_interactive *interactive) {
    if (!interactive)
        return;
    oasp_runtime_dispose(&interactive->runtime);
    oasp_component_context_destroy(interactive->component_context);
    oasp_request_destroy(interactive->request);
    free(interactive);
}

oasp_interactive_result oasp_interactive_execute(oasp_interactive *interactive, const char *source, size_t length, bool expression, const char **output,
                                                 size_t *output_length) {
    if (!interactive || !source || !output || !output_length || length > SIZE_MAX - 32)
        return OASP_INTERACTIVE_ERROR;
    oasp_buffer script;
    if (!oasp_buffer_init(&script, length + 32))
        return OASP_INTERACTIVE_ERROR;
    bool built = !expression ||
                 oasp_buffer_append(&script, "Response.Write (", 16);
    built = built && oasp_buffer_append(&script, source, length);
    if (expression)
        built = built && oasp_buffer_append_byte(&script, ')');
    if (!built) {
        oasp_buffer_dispose(&script);
        return OASP_INTERACTIVE_ERROR;
    }

    char *persistent_source = oasp_arena_copy(interactive->runtime.arena, script.data, script.length);
    oasp_buffer_dispose(&script);
    if (!persistent_source)
        return OASP_INTERACTIVE_ERROR;
    oasp_parser parser;
    if (!oasp_parser_init(&parser, interactive->runtime.arena, persistent_source,
                          strlen(persistent_source))) {
        fprintf(stderr, "<stdin>:%zu:%zu: syntax error: %s\n", parser.error_line,
                parser.error_column, parser.error ? parser.error : "lexer failed");
        oasp_parser_dispose(&parser);
        return OASP_INTERACTIVE_ERROR;
    }
    oasp_block program;
    if (!oasp_parse_program(&parser, &program)) {
        if (!expression && oasp_interactive_incomplete(&parser)) {
            oasp_parser_dispose(&parser);
            return OASP_INTERACTIVE_INCOMPLETE;
        }
        fprintf(stderr, "<stdin>:%zu:%zu: syntax error: %s\n", parser.error_line,
                parser.error_column, parser.error ? parser.error : "parser failed");
        oasp_parser_dispose(&parser);
        return OASP_INTERACTIVE_ERROR;
    }
    oasp_parser_dispose(&parser);

    oasp_request_truncate_output(interactive->request, 0);
    interactive->runtime.error_number = 0;
    interactive->runtime.error_description = NULL;
    interactive->runtime.error_source = NULL;
    interactive->runtime.error_line = 0;
    interactive->runtime.error_pending = false;
    if (!oasp_runtime_execute_dynamic(&interactive->runtime, &program)) {
        fprintf(stderr, "<stdin>:%zu: runtime error %d: %s\n",
                interactive->runtime.error_line, interactive->runtime.error_number,
                interactive->runtime.error_description
                    ? interactive->runtime.error_description
                    : "execution failed");
        return OASP_INTERACTIVE_ERROR;
    }
    *output = oasp_request_output(interactive->request, output_length);
    return OASP_INTERACTIVE_OK;
}
