// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`tests/test_core.c`): Native regression test. Local fixtures construct the
 * smallest state needed to exercise ownership, limits, persistence, and failure paths;
 * assertions document the externally observable contract and guard against silent regressions.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "ast_image.h"
#include "bytecode.h"
#include "lexer.h"
#include "openasp.h"
#include "parser.h"
#include "runtime.h"
#include "symbol.h"
#include "value.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    oasp_arena *arena = oasp_arena_create(64);
    assert(arena);
    const char *lex_source = "[spaced name] = +5\r\n"
                             "x = &H10& + &10 + .25E2 _\r\n"
                             "    + 1 ' ignored\r\n"
                             "d = #12/31/2020# : s = \"a\"\"b\"\r\n";
    oasp_token_list tokens;
    size_t lexer_arena_used = oasp_arena_used(arena);
    assert(oasp_lex(arena, lex_source, strlen(lex_source), &tokens));
    assert(tokens.length > 20);
    assert(tokens.items[0].kind == OASP_TOKEN_IDENTIFIER);
    assert(oasp_ascii_equal(tokens.items[0].text, "spaced name"));
    assert(tokens.items[3].kind == OASP_TOKEN_INTEGER);
    assert(oasp_ascii_equal(tokens.items[3].text, "5"));
    bool found_float = false;
    bool found_date = false;
    bool found_escaped_string = false;
    for (size_t i = 0; i < tokens.length; i++) {
        found_float |= tokens.items[i].kind == OASP_TOKEN_FLOAT && oasp_ascii_equal(tokens.items[i].text, ".25E2");
        found_date |= tokens.items[i].kind == OASP_TOKEN_DATE && oasp_ascii_equal(tokens.items[i].text, "12/31/2020");
        found_escaped_string |= tokens.items[i].kind == OASP_TOKEN_STRING && oasp_ascii_equal(tokens.items[i].text, "a\"b");
    }
    assert(found_float && found_date && found_escaped_string);
    assert(oasp_arena_used(arena) - lexer_arena_used == 10);
    oasp_token_list_dispose(&tokens);

    oasp_parser parser;
    const char *expression = "2 ^ 10 & \"|\" & (8 \\ 3) & \"|\" & "
                             "(True Eqv False) & \"|\" & "
                             "(True Imp False)";
    assert(oasp_parser_init(&parser, arena, expression, strlen(expression)));
    oasp_expr *tree = oasp_parse_expression(&parser);
    assert(tree && !parser.error);
    assert(tree->kind == OASP_EXPR_CONCAT);
    assert(tree->right->kind == OASP_EXPR_IMP);
    assert(tree->right->left->literal.kind == OASP_VALUE_BOOL);
    assert(tree->right->right->literal.kind == OASP_VALUE_BOOL);
    oasp_parser_dispose(&parser);

    const char *program_source = "Option Explicit\n"
                                 "Dim x, items(2)\n"
                                 "x = 40 + 2\n"
                                 "Response.Write(x)\n";
    oasp_block program;
    assert(oasp_parser_init(&parser, arena, program_source, strlen(program_source)));
    assert(oasp_parse_program(&parser, &program));
    assert(program.first->kind == OASP_STMT_OPTION_EXPLICIT);
    assert(program.first->next->kind == OASP_STMT_DIM);
    assert(program.first->next->declaration_count == 2);
    assert(program.first->next->declarations[1].dimension_count == 1);
    assert(program.last->kind == OASP_STMT_CALL);
    oasp_parser_dispose(&parser);

    oasp_buffer ast_bytes;
    size_t ast_statement_count = 0;
    size_t ast_expression_count = 0;
    assert(oasp_ast_image_encode(&program, &ast_bytes, &ast_statement_count, &ast_expression_count));
    oasp_ast_image ast_image;
    assert(oasp_ast_image_open(ast_bytes.data, ast_bytes.length, ast_statement_count, ast_expression_count, &ast_image));
    oasp_block materialized_program;
    assert(oasp_ast_image_materialize(&ast_image, arena, &materialized_program, NULL, NULL));
    assert(materialized_program.first && materialized_program.last);
    unsigned char *unaligned_ast = malloc(ast_bytes.length + 1);
    assert(unaligned_ast);
    memcpy(unaligned_ast + 1, ast_bytes.data, ast_bytes.length);
    assert(!oasp_ast_image_open(unaligned_ast + 1, ast_bytes.length, ast_statement_count, ast_expression_count, &ast_image));
    free(unaligned_ast);
    oasp_buffer_dispose(&ast_bytes);

    const char *branch_source = "If True Then\n"
                                "  x = 1\n"
                                "Else\n"
                                "  x = 2\n"
                                "End If\n";
    assert(oasp_parser_init(&parser, arena, branch_source, strlen(branch_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    oasp_bytecode bytecode;
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.length == 3);
    assert(bytecode.instructions[0].opcode == OASP_BC_PUSH_LITERAL);
    assert(bytecode.instructions[1].opcode == OASP_BC_ASSIGN_GLOBAL_SLOT);
    assert(bytecode.global_slot_count == 1);
    assert(bytecode.instructions[2].opcode == OASP_BC_HALT);
    assert(bytecode.max_stack == 1);
    assert(bytecode.opcode_counts[OASP_BC_ASSIGN_GLOBAL_SLOT] == 1);

    const char *stack_source = "x = 1 + 2 * 3\n";
    assert(oasp_parser_init(&parser, arena, stack_source, strlen(stack_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.length == 3);
    assert(bytecode.instructions[0].opcode == OASP_BC_PUSH_LITERAL);
    assert(bytecode.constants[bytecode.instructions[0].operand].kind == OASP_VALUE_INT);
    assert(bytecode.constants[bytecode.instructions[0].operand].as.integer == 7);
    assert(bytecode.instructions[1].opcode == OASP_BC_ASSIGN_GLOBAL_SLOT);
    assert(bytecode.instructions[2].opcode == OASP_BC_HALT);

    const char *concat_source = "result = \"a\" & 1 & \"b\" & 2\n";
    assert(oasp_parser_init(&parser, arena, concat_source, strlen(concat_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.length == 7);
    assert(bytecode.instructions[4].opcode == OASP_BC_CONCAT_N);
    assert(bytecode.instructions[4].operand == 4);
    assert(bytecode.max_stack == 4);

    const char *routine_source = "Function AddOne(ByRef value)\n"
                                 "  Dim local\n"
                                 "  local = value + 1\n"
                                 "  value = local\n"
                                 "  AddOne = local\n"
                                 "End Function\n"
                                 "x = 4\n"
                                 "y = AddOne(x)\n";
    assert(oasp_parser_init(&parser, arena, routine_source, strlen(routine_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.routine_count == 1);
    assert(bytecode.routines[0].slot_count == 3);
    assert(bytecode.routines[0].entry < bytecode.routines[0].limit);
    assert(bytecode.instructions[bytecode.routines[0].limit - 1].opcode == OASP_BC_RETURN);
    assert(bytecode.opcode_counts[OASP_BC_PUSH_LOCAL_SLOT] >= 3);
    bool has_static_call = false;
    for (size_t i = 0; i < bytecode.length; i++)
        has_static_call |= bytecode.instructions[i].opcode == OASP_BC_CALL_NAME && bytecode.instructions[i].slot == 1;
    assert(has_static_call);
    assert(oasp_bytecode_validate(&bytecode));
    for (size_t i = 0; i < bytecode.length; i++) {
        if (bytecode.instructions[i].opcode != OASP_BC_CALL_NAME)
            continue;
        int32_t slot = bytecode.instructions[i].slot;
        bytecode.instructions[i].flags |= OASP_BC_FLAG_ROOT_SLOT;
        bytecode.instructions[i].slot = INT32_MIN;
        assert(!oasp_bytecode_validate(&bytecode));
        bytecode.instructions[i].slot = slot;
        break;
    }

    const char *conditional_routine_source = "If True Then\n"
                                             "  Function NestedValue()\n"
                                             "    NestedValue = 42\n"
                                             "  End Function\n"
                                             "End If\n"
                                             "answer = NestedValue()\n";
    assert(oasp_parser_init(&parser, arena, conditional_routine_source, strlen(conditional_routine_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.routine_count == 1);
    assert(oasp_bytecode_validate(&bytecode));

    int64_t converted_integer = 0;
    const char adjacent_digits[] = {'1', '2'};
    oasp_value bounded_integer = oasp_value_string(adjacent_digits, 1);
    assert(oasp_value_to_int(&bounded_integer, &converted_integer) && converted_integer == 1);
    oasp_value huge_float = oasp_value_float(0x1p63);
    assert(!oasp_value_to_int(&huge_float, &converted_integer));
    oasp_value not_a_number = oasp_value_float(NAN);
    assert(!oasp_value_to_int(&not_a_number, &converted_integer));

    const char *static_binding_source = "Dim sharedValue\n"
                                        "Function ReadShared()\n"
                                        "  ReadShared = sharedValue\n"
                                        "End Function\n"
                                        "sharedValue = 7\n"
                                        "result = ReadShared()\n";
    assert(oasp_parser_init(&parser, arena, static_binding_source, strlen(static_binding_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.opcode_counts[OASP_BC_CALL_NAME] == 1);
    has_static_call = false;
    for (size_t i = 0; i < bytecode.length; i++)
        has_static_call |= bytecode.instructions[i].opcode == OASP_BC_CALL_NAME && bytecode.instructions[i].slot == 1;
    assert(has_static_call);
    assert(bytecode.opcode_counts[OASP_BC_PUSH_NAME] >= 1);
    assert(oasp_bytecode_compile_module(arena, &program, &bytecode));
    assert(bytecode.opcode_counts[OASP_BC_CALL_NAME] == 1);
    has_static_call = false;
    for (size_t i = 0; i < bytecode.length; i++)
        has_static_call |= bytecode.instructions[i].opcode == OASP_BC_CALL_NAME && bytecode.instructions[i].slot == 1;
    assert(has_static_call);

    const char *optimized_source = "Function Optimized(value)\n"
                                   "  Dim text, total\n"
                                   "  text = CStr(value)\n"
                                   "  total = Len(text) + Len(text)\n"
                                   "  If text = \"x\" Then total = total + 1\n"
                                   "  If IsEmpty(total) Then total = 0\n"
                                   "  Optimized = Trim(text)\n"
                                   "End Function\n";
    assert(oasp_parser_init(&parser, arena, optimized_source, strlen(optimized_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.opcode_counts[OASP_BC_LOAD_LOCAL_CALL_BUILTIN] >= 4);
    assert(bytecode.opcode_counts[OASP_BC_ADD_I64] == 1);
    assert(bytecode.opcode_counts[OASP_BC_LOCAL_CONST_COMPARE_JUMP] == 1);
    assert(bytecode.opcode_counts[OASP_BC_JUMP_IF_EMPTY] == 1);
    assert(bytecode.opcode_counts[OASP_BC_LOCAL_ADD_CONST_STORE] == 1);
    assert(sizeof(oasp_instruction) == 24);

    const char *write_source = "Dim text, probe\n"
                               "text = \"\"\n"
                               "text = text & \"x\"\n"
                               "text = text & \"y\" & \"z\"\n"
                               "Response.Write \"constant\"\n"
                               "Response.Write text\n"
                               "Response.Write probe.Value\n"
                               "Response.Write text & \"!\"\n";
    assert(oasp_parser_init(&parser, arena, write_source, strlen(write_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.opcode_counts[OASP_BC_APPEND_GLOBAL] == 1);
    assert(bytecode.opcode_counts[OASP_BC_APPEND_GLOBAL_N] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_CONST] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_LOCAL] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_MEMBER] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_N] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_VALUE] == 0);
    assert(!bytecode.root_error_aware);
    assert(oasp_bytecode_validate(&bytecode));

    const char *dynamic_call_source = "Execute \"x = 1\"\n"
                                      "result = Eval(\"1 + 1\")\n"
                                      "ExecuteGlobal \"y = 2\"\n"
                                      "Call ExternalRoutine(1)\n";
    assert(oasp_parser_init(&parser, arena, dynamic_call_source, strlen(dynamic_call_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    bool has_external_hint = false;
    size_t dynamic_calls = 0;
    for (size_t i = 0; i < bytecode.length; i++) {
        if (bytecode.instructions[i].opcode != OASP_BC_CALL_NAME)
            continue;
        oasp_slice call_name;
        assert(oasp_bytecode_name_at(&bytecode, bytecode.instructions[i].operand, &call_name));
        if (oasp_ascii_equal(call_name, "execute") || oasp_ascii_equal(call_name, "eval") ||
            oasp_ascii_equal(call_name, "executeglobal")) {
            assert(!(bytecode.instructions[i].flags & OASP_BC_FLAG_SYMBOL_HINT));
            dynamic_calls++;
        } else if (oasp_ascii_equal(call_name, "externalroutine")) {
            assert(bytecode.instructions[i].flags & OASP_BC_FLAG_SYMBOL_HINT);
            assert(bytecode.instructions[i].error_target == oasp_symbol_id(call_name));
            has_external_hint = true;
        }
    }
    assert(dynamic_calls == 3);
    assert(has_external_hint);

    const char *member_id_source = "Dim recordset\n"
                                   "eofValue = recordset.EOF\n"
                                   "fieldValue = recordset.Fields(\"title\")\n"
                                   "Call recordset.MoveNext()\n";
    assert(oasp_parser_init(&parser, arena, member_id_source, strlen(member_id_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    bool has_eof_id = false;
    bool has_fields_id = false;
    bool has_movenext_id = false;
    for (size_t i = 0; i < bytecode.length; i++) {
        has_eof_id |= bytecode.instructions[i].opcode == OASP_BC_RECORDSET_EOF;
        has_fields_id |= bytecode.instructions[i].opcode == OASP_BC_RECORDSET_FIELD &&
                         (bytecode.instructions[i].flags & OASP_BC_FLAG_ROOT_SLOT) &&
                         bytecode.instructions[i].reserved == OASP_MEMBER_FIELDS;
        has_movenext_id |= bytecode.instructions[i].opcode == OASP_BC_RECORDSET_MOVE_NEXT;
    }
    assert(has_eof_id);
    assert(has_fields_id);
    assert(has_movenext_id);

    const char *state_effect_source = "x = Session(\"x\")\n"
                                      "Session(\"x\") = 1\n"
                                      "y = Application(\"y\")\n"
                                      "Application(\"y\") = 2\n";
    assert(oasp_parser_init(&parser, arena, state_effect_source, strlen(state_effect_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert((bytecode.state_effects & (OASP_STATE_READS_SESSION | OASP_STATE_WRITES_SESSION |
                                     OASP_STATE_READS_APPLICATION | OASP_STATE_WRITES_APPLICATION)) ==
           (OASP_STATE_READS_SESSION | OASP_STATE_WRITES_SESSION |
            OASP_STATE_READS_APPLICATION | OASP_STATE_WRITES_APPLICATION));

    const char *recordset_write_source = "Dim rs\n"
                                         "Response.Write rs(\"title\")\n"
                                         "Response.Write Server.HTMLEncode(rs(\"title\"))\n"
                                         "Response.Write CLng(rs(\"id\"))\n";
    assert(oasp_parser_init(&parser, arena, recordset_write_source, strlen(recordset_write_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.opcode_counts[OASP_BC_WRITE_RS_FIELD] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_HTMLENCODE_RS_FIELD] == 1);
    assert(bytecode.opcode_counts[OASP_BC_WRITE_INT_RS_FIELD] == 1);

    const char *render_recordset_source = "Function Escape(value)\n"
                                          "Escape = Server.HTMLEncode(CStr(value))\n"
                                          "End Function\n"
                                          "Dim rs\n"
                                          "Response.Write \"<i>\" & Escape(rs(\"name\")) & \"</i>\"\n";
    assert(oasp_parser_init(&parser, arena, render_recordset_source, strlen(render_recordset_source)));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    assert(bytecode.opcode_counts[OASP_BC_WRITE_RENDER_RS_FIELD] == 1);

    oasp_value boolean = oasp_value_bool(true);
    oasp_value integer = oasp_value_int(42);
    oasp_value floating = oasp_value_float(2.5);
    char value_buffer[64];
    oasp_slice value_text;
    size_t value_text_arena_used = oasp_arena_used(arena);
    assert(oasp_value_text(&integer, value_buffer, sizeof(value_buffer), &value_text));
    assert(value_text.length == 2 && !memcmp(value_text.data, "42", 2));
    assert(oasp_arena_used(arena) == value_text_arena_used);
    assert(!strcmp(oasp_value_format(arena, &boolean), "True"));
    assert(!strcmp(oasp_value_format(arena, &integer), "42"));
    assert(!strcmp(oasp_value_format(arena, &floating), "2.5"));

    oasp_arena_reset(arena);
    assert(oasp_arena_used(arena) == 0);
    oasp_arena_destroy(arena);
    return 0;
}
