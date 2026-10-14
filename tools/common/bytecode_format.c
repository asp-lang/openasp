// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "bytecode_format.h"

#include <inttypes.h>

/*
 * Instruction quick reference.
 *
 * S[a, b] means a is below b on the evaluation stack; "S[x] -> S[y]"
 * documents the number, order, and result of stack operands. C[n], N[n],
 * G[n], and L[n] refer to the constant pool, name pool, global-slot table,
 * and current frame's local slots. PC[n] is an instruction index.
 *
 * `line` always identifies the source line for diagnostics. Other instruction
 * fields are opcode-specific; omitted fields have no semantic meaning. `slot`
 * encodes a lexical slot as +L[n] (n + 1), -G[n] (-n - 1), or, for loop
 * variables only, INT32_MIN with the name index split across flags/reserved.
 */
static const char *const oasp_tool_opcode_names[OASP_BC_COUNT] = {
    /* Enable undeclared-name checks. S[] -> S[]; no opcode-specific operands. */
    [OASP_BC_OPTION_EXPLICIT] = "OPTION_EXPLICIT",
    /* Enable per-instruction error recovery. S[] -> S[]; no operands. */
    [OASP_BC_ON_ERROR_RESUME_NEXT] = "ON_ERROR_RESUME_NEXT",
    /* Disable Resume Next and clear the current runtime error. S[] -> S[]; no operands. */
    [OASP_BC_ON_ERROR_GOTO_0] = "ON_ERROR_GOTO_0",
    /* Execute an include file. S[] -> S[]; operand=C[path string]. */
    [OASP_BC_INCLUDE] = "INCLUDE",
    /* Push VBScript Empty. S[] -> S[Empty]; no operands. */
    [OASP_BC_PUSH_EMPTY] = "PUSH_EMPTY",
    /* Push a pooled literal. S[] -> S[value]; operand=C[value]. */
    [OASP_BC_PUSH_LITERAL] = "PUSH_LITERAL",
    /*
     * Resolve and push a dynamic/dotted name. S[] -> S[value]; operand=N[name],
     * reserved=static member id when known. ROOT_SLOT uses slot as the dotted
     * name's root lexical slot and resolves the remaining member from it.
     */
    [OASP_BC_PUSH_NAME] = "PUSH_NAME",
    /* Load a global and retain its assignment reference. S[] -> S[value]; operand=G[index]. */
    [OASP_BC_PUSH_GLOBAL_SLOT] = "PUSH_GLOBAL_SLOT",
    /* Load a local and retain its assignment reference. S[] -> S[value]; operand=L[index]. */
    [OASP_BC_PUSH_LOCAL_SLOT] = "PUSH_LOCAL_SLOT",
    /* Construct an object by ProgID/class name. S[] -> S[object]; operand=N[class name]. */
    [OASP_BC_NEW] = "NEW",
    /* Read a member from the receiver. S[receiver] -> S[value]; operand=N[member], reserved=member id. */
    [OASP_BC_GET_MEMBER] = "GET_MEMBER",
    /*
     * Invoke a routine or named callable. S[arg1, ..., argN] -> S[result];
     * target=N (argument count), operand=N[callable]. slot is a static routine
     * index when positive; flags/reserved select static builtin/member fast paths,
     * ROOT_SLOT supplies a dotted-name root, and SYMBOL_HINT uses error_target as
     * the resolved symbol id.
     */
    [OASP_BC_CALL_NAME] = "CALL_NAME",
    /* Invoke a member dynamically. S[receiver, arg1, ..., argN] -> S[result]; target=N, operand=N[member]. */
    [OASP_BC_CALL_MEMBER] = "CALL_MEMBER",
    /* Invoke an object's default member. S[receiver, arg1, ..., argN] -> S[result]; target=N. */
    [OASP_BC_CALL_DEFAULT] = "CALL_DEFAULT",
    /* Negate a numeric value. S[value] -> S[-value]; no opcode-specific operands. */
    [OASP_BC_NEGATE] = "NEGATE",
    /* Apply unary plus/coercion. S[value] -> S[+value]; no opcode-specific operands. */
    [OASP_BC_POSITIVE] = "POSITIVE",
    /* Apply VBScript logical Not. S[value] -> S[Not value]; no opcode-specific operands. */
    [OASP_BC_NOT] = "NOT",
    /* Concatenate two values. S[left, right] -> S[left & right]; no opcode-specific operands. */
    [OASP_BC_CONCAT] = "CONCAT",
    /* Concatenate N values in evaluation order. S[value1, ..., valueN] -> S[result]; operand=N (at least 2). */
    [OASP_BC_CONCAT_N] = "CONCAT_N",
    /* Compare equality. S[left, right] -> S[left = right]; no opcode-specific operands. */
    [OASP_BC_EQ] = "EQ",
    /* Compare inequality. S[left, right] -> S[left <> right]; no opcode-specific operands. */
    [OASP_BC_NE] = "NE",
    /* Compare less-than. S[left, right] -> S[left < right]; no opcode-specific operands. */
    [OASP_BC_LT] = "LT",
    /* Compare less-than-or-equal. S[left, right] -> S[left <= right]; no opcode-specific operands. */
    [OASP_BC_LE] = "LE",
    /* Compare greater-than. S[left, right] -> S[left > right]; no opcode-specific operands. */
    [OASP_BC_GT] = "GT",
    /* Compare greater-than-or-equal. S[left, right] -> S[left >= right]; no opcode-specific operands. */
    [OASP_BC_GE] = "GE",
    /* Add two values with normal VBScript coercion. S[left, right] -> S[left + right]. */
    [OASP_BC_ADD] = "ADD",
    /* Subtract two values. S[left, right] -> S[left - right]. */
    [OASP_BC_SUBTRACT] = "SUBTRACT",
    /* Multiply two values. S[left, right] -> S[left * right]. */
    [OASP_BC_MULTIPLY] = "MULTIPLY",
    /* Divide two values. S[left, right] -> S[left / right]. */
    [OASP_BC_DIVIDE] = "DIVIDE",
    /* Integer-divide two values. S[left, right] -> S[left \ right]. */
    [OASP_BC_INT_DIVIDE] = "INT_DIVIDE",
    /* Compute remainder. S[left, right] -> S[left Mod right]. */
    [OASP_BC_MOD] = "MOD",
    /* Raise to a power. S[left, right] -> S[left ^ right]. */
    [OASP_BC_POWER] = "POWER",
    /* Apply bitwise/logical And. S[left, right] -> S[left And right]. */
    [OASP_BC_AND] = "AND",
    /* Apply bitwise/logical Or. S[left, right] -> S[left Or right]. */
    [OASP_BC_OR] = "OR",
    /* Apply bitwise/logical Xor. S[left, right] -> S[left Xor right]. */
    [OASP_BC_XOR] = "XOR",
    /* Apply logical equivalence. S[left, right] -> S[left Eqv right]. */
    [OASP_BC_EQV] = "EQV",
    /* Apply logical implication. S[left, right] -> S[left Imp right]. */
    [OASP_BC_IMP] = "IMP",
    /* Compare object identity. S[left, right] -> S[left Is right]. */
    [OASP_BC_IS] = "IS",
    /* Compare object non-identity. S[left, right] -> S[left Is Not right]. */
    [OASP_BC_IS_NOT] = "IS_NOT",
    /* Store a value by dynamic name. S[value] -> S[]; operand=N[target name]. */
    [OASP_BC_ASSIGN_NAME] = "ASSIGN_NAME",
    /* Store a member. S[value, receiver] -> S[]; operand=N[member]. */
    [OASP_BC_ASSIGN_MEMBER] = "ASSIGN_MEMBER",
    /* Store an indexed named value. S[value, index1, ..., indexN] -> S[]; operand=N[name], target=N. */
    [OASP_BC_ASSIGN_INDEX_NAME] = "ASSIGN_INDEX_NAME",
    /* Store an indexed member. S[value, receiver, index1, ..., indexN] -> S[]; operand=N[member], target=N. */
    [OASP_BC_ASSIGN_INDEX_MEMBER] = "ASSIGN_INDEX_MEMBER",
    /* Store Response.Cookies(args...).attribute. S[value, arg1, ..., argN] -> S[]; operand=N[attribute], target=N. */
    [OASP_BC_ASSIGN_COOKIE_ATTRIBUTE] = "ASSIGN_COOKIE_ATTRIBUTE",
    /* Store a global slot. S[value] -> S[]; operand=G[index]. */
    [OASP_BC_ASSIGN_GLOBAL_SLOT] = "ASSIGN_GLOBAL_SLOT",
    /* Store a local slot. S[value] -> S[]; operand=L[index]. */
    [OASP_BC_ASSIGN_LOCAL_SLOT] = "ASSIGN_LOCAL_SLOT",
    /* Dim an array/name. S[bound1, ..., boundN] -> S[]; operand=N[name], target=N dimensions, flags bit 0=Preserve. */
    [OASP_BC_DECLARE] = "DECLARE",
    /* ReDim an existing array. S[bound1, ..., boundN] -> S[]; operand=N[name], target=N dimensions, flags bit 0=Preserve. */
    [OASP_BC_REDIM] = "REDIM",
    /* Declare an immutable constant. S[value] -> S[]; operand=N[constant name]. */
    [OASP_BC_DECLARE_CONST] = "DECLARE_CONST",
    /* Reserved legacy opcode: Erase currently compiles as PUSH_EMPTY plus an assignment; the VM has no ERASE handler. */
    [OASP_BC_ERASE] = "ERASE",
    /* Enter a With scope. S[object] -> S[]; subsequent implicit members use this object. */
    [OASP_BC_WITH_ENTER] = "WITH_ENTER",
    /* Leave the current With scope. S[] -> S[]; restores the previous implicit object. */
    [OASP_BC_WITH_LEAVE] = "WITH_LEAVE",
    /* Discard the top evaluation result. S[value] -> S[]. */
    [OASP_BC_DROP] = "DROP",
    /* Duplicate the top value and its assignment reference. S[value] -> S[value, value]. */
    [OASP_BC_DUP] = "DUP",
    /* Pop a condition and branch when false. S[condition] -> S[]; target=PC[false], error_target=PC[Resume Next error]. */
    [OASP_BC_JUMP_IF_FALSE] = "JUMP_IF_FALSE",
    /* Pop a condition and branch when true. S[condition] -> S[]; target=PC[true], error_target=PC[Resume Next error]. */
    [OASP_BC_JUMP_IF_TRUE] = "JUMP_IF_TRUE",
    /* Unconditional branch. S[] -> S[]; target=PC[destination]. */
    [OASP_BC_JUMP] = "JUMP",
    /* Back-edge for While/Do loops. S[] -> S[]; target=PC[loop head], operand=loop-state index. */
    [OASP_BC_LOOP_BACK] = "LOOP_BACK",
    /*
     * Initialize a numeric For loop and assign its control variable. S[start,
     * limit, step?] -> S[]; operand=loop-state index, error_target=2 or 3
     * input count, target=PC[loop end], slot=loop variable destination.
     */
    [OASP_BC_FOR_INIT] = "FOR_INIT",
    /* Advance a numeric For loop. S[] -> S[]; operand=loop-state index, target=PC[loop body], slot=control-variable destination. */
    [OASP_BC_FOR_NEXT] = "FOR_NEXT",
    /* Initialize a For Each loop. S[collection] -> S[]; operand=loop-state index, target=PC[loop end], slot=loop variable destination. */
    [OASP_BC_FOR_EACH_INIT] = "FOR_EACH_INIT",
    /* Advance a For Each loop. S[] -> S[]; operand=loop-state index, target=PC[loop body], slot=control-variable destination. */
    [OASP_BC_FOR_EACH_NEXT] = "FOR_EACH_NEXT",
    /* End the current routine range. S[] -> S[]; execution returns to the caller. */
    [OASP_BC_RETURN] = "RETURN",
    /* End the root program range. S[] -> S[]; execution terminates normally. */
    [OASP_BC_HALT] = "HALT",
    /* Fast-path signed 64-bit addition with overflow checking. S[left, right] -> S[sum]. */
    [OASP_BC_ADD_I64] = "ADD_I64",
    /* Fast-path string comparison. S[left, right] -> S[boolean]; flags=oasp_expr_kind comparison operator. */
    [OASP_BC_COMPARE_STRING] = "COMPARE_STRING",
    /* Pop a value and branch on Empty (or non-Empty when flags is nonzero). S[value] -> S[]; target=PC[destination]. */
    [OASP_BC_JUMP_IF_EMPTY] = "JUMP_IF_EMPTY",
    /* Fast-path string concatenation. S[left, right] -> S[result]. */
    [OASP_BC_CONCAT_STRING] = "CONCAT_STRING",
    /* Call a known builtin with one local by reference/value. S[] -> S[result]; operand=L[arg], error_target=N[builtin], reserved=builtin id. */
    [OASP_BC_LOAD_LOCAL_CALL_BUILTIN] = "LOAD_LOCAL_CALL_BUILTIN",
    /*
     * Compare a local slot with a pooled constant and conditionally branch.
     * S[] -> S[]; operand=L[left], reserved=C[right], slot=oasp_expr_kind,
     * flags=branch polarity, target=PC[branch], error_target=PC[Resume Next error].
     */
    [OASP_BC_LOCAL_CONST_COMPARE_JUMP] = "LOCAL_CONST_COMPARE_JUMP",
    /* Add a pooled constant to a local and store it. S[] -> S[]; operand=L[source], reserved=C[addend], target=L[destination]. */
    [OASP_BC_LOCAL_ADD_CONST_STORE] = "LOCAL_ADD_CONST_STORE",
    /* Load a member and immediately call its member. S[receiver, arg1, ..., argN] -> S[result]; operand=N[member], reserved=member id, target=N. */
    [OASP_BC_LOAD_MEMBER_CALL_MEMBER] = "LOAD_MEMBER_CALL_MEMBER",
    /* Append one suffix to a local string/value. S[suffix] -> S[]; operand=L[destination]. */
    [OASP_BC_APPEND_LOCAL] = "APPEND_LOCAL",
    /* Append one suffix to a global string/value. S[suffix] -> S[]; operand=G[destination]. */
    [OASP_BC_APPEND_GLOBAL] = "APPEND_GLOBAL",
    /* Write a pooled literal to the response. S[] -> S[]; operand=C[value]. */
    [OASP_BC_WRITE_CONST] = "WRITE_CONST",
    /* Write a lexical local/global slot to the response. S[] -> S[]; slot=encoded source slot. */
    [OASP_BC_WRITE_LOCAL] = "WRITE_LOCAL",
    /* Read and write a member to the response. S[receiver] -> S[] normally; operand=N[member], reserved=member id, ROOT_SLOT makes slot the receiver. */
    [OASP_BC_WRITE_MEMBER] = "WRITE_MEMBER",
    /* Pop and write a computed value to the response. S[value] -> S[]. */
    [OASP_BC_WRITE_VALUE] = "WRITE_VALUE",
    /* Append N suffixes to a local value. S[suffix1, ..., suffixN] -> S[]; operand=L[destination], target=N. */
    [OASP_BC_APPEND_LOCAL_N] = "APPEND_LOCAL_N",
    /* Append N suffixes to a global value. S[suffix1, ..., suffixN] -> S[]; operand=G[destination], target=N. */
    [OASP_BC_APPEND_GLOBAL_N] = "APPEND_GLOBAL_N",
    /* Pop and write N values in order. S[value1, ..., valueN] -> S[]; target=N (at least 2). */
    [OASP_BC_WRITE_N] = "WRITE_N",
    /* Read a Recordset field or fall back to Fields/Item. S[] -> S[value]; slot=recordset, operand=C[field name], ROOT_SLOT target=N[member], reserved=member id. */
    [OASP_BC_RECORDSET_FIELD] = "RECORDSET_FIELD",
    /* Write a Recordset field or fallback value. S[] -> S[]; slot=recordset, operand=C[field name], ROOT_SLOT target=N[member], reserved=member id. */
    [OASP_BC_WRITE_RS_FIELD] = "WRITE_RS_FIELD",
    /* Read then HTML-encode and write a Recordset field. S[] -> S[]; slot=recordset, operand=C[field name], ROOT_SLOT target=N[member], reserved=member id. */
    [OASP_BC_WRITE_HTMLENCODE_RS_FIELD] = "WRITE_HTMLENCODE_RS_FIELD",
    /* Read, coerce to integer, and write a Recordset field. S[] -> S[]; slot=recordset, operand=C[field name], ROOT_SLOT target=N[member], reserved=member id. */
    [OASP_BC_WRITE_INT_RS_FIELD] = "WRITE_INT_RS_FIELD",
    /* Write constant prefix and suffix around a directly rendered Recordset field. */
    [OASP_BC_WRITE_RENDER_RS_FIELD] = "WRITE_RENDER_RS_FIELD",
    /* Advance a Recordset cursor, with a dynamic MoveNext fallback. S[] -> S[]; slot=recordset, operand=N[dotted MoveNext name]. */
    [OASP_BC_RECORDSET_MOVE_NEXT] = "RECORDSET_MOVE_NEXT",
    /* Read a Recordset EOF flag, with a dynamic fallback. S[] -> S[boolean]; slot=recordset, operand=N[dotted EOF name]. */
    [OASP_BC_RECORDSET_EOF] = "RECORDSET_EOF",
    /* Store through an object's default indexed member. S[value, receiver, index1, ..., indexN] -> S[]; target=N. */
    [OASP_BC_ASSIGN_INDEX_DEFAULT] = "ASSIGN_INDEX_DEFAULT"
};

_Static_assert(sizeof(oasp_tool_opcode_names) / sizeof(oasp_tool_opcode_names[0]) == OASP_BC_COUNT,
               "opcode name table is stale");

bool oasp_tool_opcode_valid(unsigned opcode) {
    return opcode < OASP_BC_COUNT && oasp_tool_opcode_names[opcode];
}

const char *oasp_tool_opcode_name(unsigned opcode) {
    return oasp_tool_opcode_valid(opcode) ? oasp_tool_opcode_names[opcode] : "<invalid>";
}

void oasp_tool_print_escaped(FILE *output, oasp_slice value) {
    fputc('"', output);
    for (size_t i = 0; i < value.length; i++) {
        unsigned char byte = (unsigned char)value.data[i];
        if (byte == '\\' || byte == '"')
            fprintf(output, "\\%c", byte);
        else if (byte == '\n')
            fputs("\\n", output);
        else if (byte == '\r')
            fputs("\\r", output);
        else if (byte == '\t')
            fputs("\\t", output);
        else if (byte >= 0x20 && byte <= 0x7e)
            fputc(byte, output);
        else
            fprintf(output, "\\x%02x", byte);
    }
    fputc('"', output);
}

void oasp_tool_print_value(FILE *output, const oasp_value *value, oasp_tool_value_style style) {
    switch (value->kind) {
    case OASP_VALUE_EMPTY:
        fputs("Empty", output);
        break;
    case OASP_VALUE_NULL:
        fputs("Null", output);
        break;
    case OASP_VALUE_BOOL:
        fputs(value->as.boolean ? "True" : "False", output);
        break;
    case OASP_VALUE_INT:
        fprintf(output, "%" PRId64, value->as.integer);
        break;
    case OASP_VALUE_FLOAT:
        fprintf(output, "%.17g", value->as.floating);
        break;
    case OASP_VALUE_STRING:
        if (style == OASP_TOOL_VALUE_TYPED)
            fputs("String(", output);
        oasp_tool_print_escaped(output, value->as.string);
        if (style == OASP_TOOL_VALUE_TYPED)
            fputc(')', output);
        break;
    case OASP_VALUE_NOTHING:
        fputs("Nothing", output);
        break;
    case OASP_VALUE_DATE:
        fputs("Date(", output);
        oasp_tool_print_escaped(output, value->as.date);
        fputc(')', output);
        break;
    case OASP_VALUE_OBJECT:
        if (style == OASP_TOOL_VALUE_DEBUG)
            fprintf(output, "<object %p>", (void *)value->as.object);
        else
            fputs("<invalid>", output);
        break;
    case OASP_VALUE_ARRAY:
        if (style == OASP_TOOL_VALUE_DEBUG) {
            fprintf(output, "<array rank=%zu length=%zu>",
                    value->as.array ? value->as.array->rank : 0,
                    value->as.array ? value->as.array->length : 0);
        } else {
            fputs("<invalid>", output);
        }
        break;
    }
}

void oasp_tool_print_instruction_bytes(FILE *output, const oasp_instruction *instruction) {
    const unsigned char *bytes = (const unsigned char *)(const void *)instruction;
    for (size_t i = 0; i < sizeof(*instruction); i++)
        fprintf(output, "%02x%s", bytes[i], i + 1 == sizeof(*instruction) ? "" : " ");
}

void oasp_tool_print_instruction_fields(FILE *output, const oasp_instruction *instruction) {
    fprintf(output, "line=%" PRIu32 " flags=%" PRIu8 " reserved=%" PRIu16
                    " target=%" PRIu32 " error=%" PRIu32 " operand=%" PRIu32
                    " slot=%" PRId32,
            instruction->line, instruction->flags, instruction->reserved,
            instruction->target, instruction->error_target, instruction->operand,
            instruction->slot);
}
