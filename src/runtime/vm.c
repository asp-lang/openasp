// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/runtime/vm.c`): Runtime core. This unit manages ASP values, frames,
 * bytecode execution, and allocator roots. State variables encode interpreter control flow and
 * errors; all allocation and reference updates must preserve arena or generational-GC ownership
 * rules.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "vm.h"

#include "builtin_internal.h"
#include "component.h"
#include "debugger.h"
#include "internal.h"
#include "symbol.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    oasp_value value;
    uintptr_t reference;
    uint64_t error_generation;
} oasp_vm_operand;

#define OASP_VM_REFERENCE_MASK ((uintptr_t)3)
#define OASP_VM_REFERENCE_LOCAL ((uintptr_t)1)
#define OASP_VM_REFERENCE_GLOBAL ((uintptr_t)2)
#define OASP_VM_REFERENCE_NAME ((uintptr_t)3)

typedef struct {
    oasp_value *values;
    uintptr_t *references;
    uint64_t *error_generations;
    size_t depth;
    size_t capacity;
} oasp_vm_stack;

typedef struct {
    double current;
    double limit;
    double step;
    oasp_array *array;
    size_t index;
} oasp_vm_loop;

static _Thread_local size_t oasp_vm_trace_pc;
static _Thread_local uint8_t oasp_vm_trace_opcode;
static _Thread_local uint32_t oasp_vm_trace_line;

static oasp_expr *oasp_vm_expr(oasp_runtime *runtime, oasp_expr_kind kind, size_t line) {
    oasp_expr *expr = oasp_arena_alloc(runtime->scratch, sizeof(*expr), _Alignof(oasp_expr));
    if (!expr)
        return NULL;
    memset(expr, 0, sizeof(*expr));
    expr->kind = kind;
    expr->line = line;
    return expr;
}

static oasp_expr *oasp_vm_literal(oasp_runtime *runtime, const oasp_value *value, size_t line) {
    oasp_expr *expr = oasp_vm_expr(runtime, OASP_EXPR_LITERAL, line);
    if (expr)
        expr->literal = *value;
    return expr;
}

static uintptr_t oasp_vm_reference(uintptr_t kind, uint32_t index) {
    return ((uintptr_t)index << 2) | kind;
}

static oasp_expr *oasp_vm_materialize_reference(oasp_runtime *runtime, const oasp_bytecode *bytecode, uintptr_t reference, size_t line) {
    if (!reference)
        return NULL;
    uintptr_t kind = reference & OASP_VM_REFERENCE_MASK;
    if (!kind)
        return (oasp_expr *)(void *)reference;
    size_t index = reference >> 2;
    oasp_slice name;
    bool resolved = kind == OASP_VM_REFERENCE_LOCAL
                        ? runtime->frame && oasp_bytecode_routine_slot(runtime->frame->routine_bytecode,
                                                                      runtime->frame->routine, index, &name)
                    : kind == OASP_VM_REFERENCE_GLOBAL
                        ? oasp_bytecode_global_slot_at(bytecode, index, &name)
                        : oasp_bytecode_name_at(bytecode, index, &name);
    oasp_expr *expr = resolved ? oasp_vm_expr(runtime, OASP_EXPR_IDENTIFIER, line) : NULL;
    if (expr)
        expr->name = name;
    return expr;
}

static oasp_expr *oasp_vm_argument_expr(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_value *value, uintptr_t encoded_reference, size_t line) {
    oasp_expr *reference = oasp_vm_materialize_reference(runtime, bytecode, encoded_reference, line);
    return reference ? reference : oasp_vm_literal(runtime, value, line);
}

static bool oasp_vm_push(oasp_vm_stack *stack, size_t *index) {
    if (stack->depth >= stack->capacity)
        return false;
    *index = stack->depth++;
    stack->values[*index] = oasp_value_empty();
    stack->references[*index] = 0;
    if (stack->error_generations)
        stack->error_generations[*index] = 0;
    return true;
}

static void oasp_vm_set_error_generation(oasp_vm_stack *stack, size_t index, uint64_t generation) {
    if (stack->error_generations)
        stack->error_generations[index] = generation;
}

static bool oasp_vm_value_valid(const oasp_runtime *runtime, const oasp_vm_stack *stack, size_t index) {
    return !stack->error_generations || runtime->error_generation == stack->error_generations[index];
}

static void oasp_vm_merge_error_generation(oasp_vm_stack *stack, size_t destination, size_t source) {
    if (stack->error_generations && stack->error_generations[source] < stack->error_generations[destination])
        stack->error_generations[destination] = stack->error_generations[source];
}

static oasp_recordset_ordinal_cache *oasp_vm_recordset_ordinal(oasp_runtime *runtime, const oasp_object *recordset, oasp_slice name) {
    for (size_t i = 0; i < OASP_ARRAY_COUNT(runtime->recordset_ordinals); i++) {
        oasp_recordset_ordinal_cache *entry = &runtime->recordset_ordinals[i];
        if (entry->valid && entry->recordset == recordset && entry->name == name.data &&
            entry->name_length == name.length)
            return entry;
    }
    size_t index = runtime->recordset_ordinal_next++ %
                   OASP_ARRAY_COUNT(runtime->recordset_ordinals);
    oasp_recordset_ordinal_cache *entry = &runtime->recordset_ordinals[index];
    entry->recordset = recordset;
    entry->name = name.data;
    entry->name_length = name.length;
    entry->schema = NULL;
    entry->ordinal = -1;
    entry->valid = true;
    return entry;
}

static bool oasp_vm_recordset_field_view(oasp_runtime *runtime, oasp_object *object, oasp_slice column, int *ordinal, const void **schema,
                                         bool *copy_text, size_t line, const oasp_value **result) {
    if (object && object->component_kind == OASP_COMPONENT_ADO_RECORDSET)
        return oasp_builtin_ado_recordset_field_view(runtime, object, column, ordinal, schema, copy_text, line, result);
    return oasp_component_recordset_field_view(runtime, object, column, ordinal, schema, copy_text, line, result);
}

static bool oasp_vm_unary_kind(oasp_opcode opcode, oasp_expr_kind *kind) {
    switch (opcode) {
    case OASP_BC_NEGATE:
        *kind = OASP_EXPR_NEGATE;
        return true;
    case OASP_BC_POSITIVE:
        *kind = OASP_EXPR_POSITIVE;
        return true;
    case OASP_BC_NOT:
        *kind = OASP_EXPR_NOT;
        return true;
    default:
        return false;
    }
}

static bool oasp_vm_binary_kind(oasp_opcode opcode, oasp_expr_kind *kind) {
    if (opcode == OASP_BC_CONCAT) {
        *kind = OASP_EXPR_CONCAT;
        return true;
    }
    if (opcode >= OASP_BC_EQ && opcode <= OASP_BC_IS_NOT) {
        static const oasp_expr_kind kinds[] = {
            OASP_EXPR_EQ,        OASP_EXPR_NE,       OASP_EXPR_LT,       OASP_EXPR_LE,       OASP_EXPR_GT,
            OASP_EXPR_GE,        OASP_EXPR_ADD,      OASP_EXPR_SUBTRACT, OASP_EXPR_MULTIPLY, OASP_EXPR_DIVIDE,
            OASP_EXPR_INT_DIVIDE, OASP_EXPR_MOD,      OASP_EXPR_POWER,    OASP_EXPR_AND,      OASP_EXPR_OR,
            OASP_EXPR_XOR,       OASP_EXPR_EQV,      OASP_EXPR_IMP,      OASP_EXPR_IS,        OASP_EXPR_IS_NOT,
        };
        *kind = kinds[opcode - OASP_BC_EQ];
        return true;
    }
    return false;
}

static bool oasp_vm_name(const oasp_bytecode *bytecode, uint32_t index, oasp_slice *name) {
    return oasp_bytecode_name_at(bytecode, index, name);
}

static const oasp_value *oasp_vm_slot_value(oasp_runtime *runtime, const oasp_bytecode *bytecode, int32_t encoded_slot) {
    if (encoded_slot > 0)
        return oasp_runtime_get_local_slot(runtime, (size_t)(encoded_slot - 1));
    if (encoded_slot < 0)
        return oasp_runtime_get_global_slot(runtime, bytecode, (size_t)(-encoded_slot - 1));
    return NULL;
}

static bool oasp_vm_dotted_member(oasp_slice name, oasp_slice *member) {
    const char *dot = memchr(name.data, '.', name.length);
    if (!dot || dot == name.data + name.length - 1)
        return false;
    member->data = dot + 1;
    member->length = name.length - (size_t)(dot - name.data) - 1;
    return true;
}

static bool oasp_vm_set_loop_value(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_instruction *instruction, const oasp_value *value) {
    if (instruction->slot > 0)
        return oasp_runtime_set_local_slot(runtime, (size_t)(instruction->slot - 1), value, false);
    if (instruction->slot < 0 && instruction->slot != INT32_MIN)
        return oasp_runtime_set_global_slot(runtime, bytecode, (size_t)(-instruction->slot - 1), value, false);
    if (instruction->slot == INT32_MIN) {
        oasp_slice name;
        return oasp_vm_name(bytecode, instruction->flags | ((uint32_t)instruction->reserved << 8), &name) &&
               oasp_runtime_set(runtime, name, value, false);
    }
    return false;
}

static bool oasp_vm_call(oasp_runtime *runtime, const oasp_bytecode *bytecode, oasp_vm_stack *stack, const oasp_instruction *instruction) {
    if (instruction->flags & OASP_BC_FLAG_CALL_STATEMENT) {
        runtime->error_number = 0;
        runtime->error_description = NULL;
        runtime->error_source = NULL;
        runtime->error_line = 0;
        runtime->error_pending = false;
    }
    uint64_t error_generation = runtime->error_generation;
    size_t count = instruction->target;
    bool named = instruction->opcode == OASP_BC_CALL_NAME;
    bool member = instruction->opcode == OASP_BC_CALL_MEMBER ||
                  instruction->opcode == OASP_BC_LOAD_MEMBER_CALL_MEMBER;
    size_t receiver_count = named ? 0 : 1;
    if (stack->depth < count + receiver_count)
        return false;
    size_t start = stack->depth - count - receiver_count;
    oasp_vm_operand receiver;
    memset(&receiver, 0, sizeof(receiver));
    if (receiver_count) {
        receiver.value = stack->values[start];
        receiver.reference = stack->references[start];
        receiver.error_generation =
            stack->error_generations ? stack->error_generations[start] : error_generation;
    }
    size_t argument_start = start + receiver_count;
    if (stack->error_generations && receiver_count && receiver.error_generation < error_generation)
        error_generation = receiver.error_generation;
    for (size_t i = 0; stack->error_generations && i < count; i++)
        if (stack->error_generations[argument_start + i] < error_generation)
            error_generation = stack->error_generations[argument_start + i];
    const oasp_value *values = &stack->values[argument_start];
    const uintptr_t *references = &stack->references[argument_start];
    oasp_value result;
    if (instruction->opcode == OASP_BC_CALL_NAME &&
        !(instruction->flags & OASP_BC_FLAG_ROOT_SLOT) && instruction->slot > 0) {
        result = oasp_runtime_call_routine_view(runtime, bytecode, (size_t)(instruction->slot - 1), values,
                                                references, count);
    } else if (named) {
        oasp_slice name;
        if (!oasp_vm_name(bytecode, instruction->operand, &name))
            return false;
        if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
            const oasp_value *root = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            oasp_slice member_name;
            if (!root || !oasp_vm_dotted_member(name, &member_name))
                return false;
            if (root->kind != OASP_VALUE_OBJECT) {
                result = oasp_runtime_call_name_view(runtime, bytecode, name, 0, 0,
                                                     instruction->reserved, values, references,
                                                     count, instruction->line);
            } else {
                result = oasp_runtime_call_member_view(runtime, bytecode, root,
                                                       instruction->reserved, member_name,
                                                       values, references, count, instruction->line);
            }
        } else {
            uint32_t symbol_id = instruction->flags & OASP_BC_FLAG_SYMBOL_HINT ? instruction->error_target : 0;
            uint16_t builtin_id = instruction->flags & OASP_BC_FLAG_STATIC_BUILTIN ? instruction->reserved : 0;
            uint16_t member_id = instruction->flags & OASP_BC_FLAG_STATIC_MEMBER ? instruction->reserved : 0;
            result = builtin_id
                         ? oasp_runtime_call_builtin(runtime, builtin_id, name, values, count, instruction->line)
                         : oasp_runtime_call_name_view(runtime, bytecode, name, symbol_id, builtin_id,
                                                       member_id, values, references, count, instruction->line);
        }
    } else if (member) {
        oasp_slice name;
        if (!oasp_vm_name(bytecode, instruction->operand, &name))
            return false;
        uint16_t member_id =
            instruction->opcode == OASP_BC_LOAD_MEMBER_CALL_MEMBER ? instruction->reserved : 0;
        result = oasp_runtime_call_member_view(runtime, bytecode, &receiver.value, member_id, name,
                                               values, references, count, instruction->line);
    } else {
        result = oasp_runtime_call_default_view(runtime, bytecode, &receiver.value, values, references,
                                                count, instruction->line);
    }
    stack->depth = start;
    size_t destination;
    if (!oasp_vm_push(stack, &destination))
        return false;
    stack->values[destination] = result;
    oasp_vm_set_error_generation(stack, destination,
                                 runtime->error_pending ? error_generation : runtime->error_generation);
    return true;
}

static oasp_expr *oasp_vm_assignment_target(oasp_runtime *runtime, const oasp_bytecode *bytecode, oasp_vm_stack *stack, const oasp_instruction *instruction, size_t *consumed) {
    bool default_index = instruction->opcode == OASP_BC_ASSIGN_INDEX_DEFAULT;
    oasp_slice name = {0};
    if (!default_index && !oasp_vm_name(bytecode, instruction->operand, &name))
        return NULL;
    if (instruction->opcode == OASP_BC_ASSIGN_NAME) {
        oasp_expr *target = oasp_vm_expr(runtime, OASP_EXPR_IDENTIFIER, instruction->line);
        if (target)
            target->name = name;
        *consumed = 0;
        return target;
    }
    bool member = instruction->opcode == OASP_BC_ASSIGN_MEMBER || instruction->opcode == OASP_BC_ASSIGN_INDEX_MEMBER;
    bool indexed = instruction->opcode == OASP_BC_ASSIGN_INDEX_NAME ||
                   instruction->opcode == OASP_BC_ASSIGN_INDEX_MEMBER || default_index;
    size_t count = indexed ? instruction->target : 0;
    size_t need = count + (member || default_index ? 1 : 0);
    if (stack->depth < need + 1)
        return NULL;
    size_t start = stack->depth - need;
    oasp_expr *base;
    if (member || default_index) {
        oasp_expr *receiver = (member || default_index)
                                  ? oasp_vm_literal(runtime, &stack->values[start], instruction->line)
                                  : oasp_vm_argument_expr(runtime, bytecode, &stack->values[start],
                                                          stack->references[start], instruction->line);
        if (!receiver)
            return NULL;
        if (default_index) {
            base = receiver;
        } else {
            base = oasp_vm_expr(runtime, OASP_EXPR_MEMBER, instruction->line);
            if (!base)
                return NULL;
            base->left = receiver;
            base->name = name;
        }
        start++;
    } else {
        base = oasp_vm_expr(runtime, OASP_EXPR_IDENTIFIER, instruction->line);
        if (!base)
            return NULL;
        base->name = name;
    }
    if (indexed) {
        oasp_expr *call = oasp_vm_expr(runtime, OASP_EXPR_CALL, instruction->line);
        oasp_expr **arguments =
            count ? oasp_arena_alloc(runtime->scratch, count * sizeof(*arguments), _Alignof(oasp_expr *)) : NULL;
        if (!call || (count && !arguments))
            return NULL;
        for (size_t i = 0; i < count; i++) {
            arguments[i] = oasp_vm_argument_expr(runtime, bytecode, &stack->values[start + i],
                                                 stack->references[start + i], instruction->line);
            if (!arguments[i])
                return NULL;
        }
        call->left = base;
        call->args = arguments;
        call->arg_count = count;
        base = call;
    }
    *consumed = need;
    return base;
}

static bool oasp_vm_declare(oasp_runtime *runtime, const oasp_bytecode *bytecode, oasp_vm_stack *stack, const oasp_instruction *instruction) {
    oasp_slice name;
    size_t dimensions = instruction->target;
    if (!oasp_vm_name(bytecode, instruction->operand, &name) || stack->depth < dimensions)
        return false;
    oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
    size_t start = stack->depth - dimensions;
    oasp_value *bounds =
        dimensions ? oasp_arena_alloc(runtime->scratch, dimensions * sizeof(*bounds), _Alignof(oasp_value)) : NULL;
    if (dimensions && !bounds) {
        oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
        return false;
    }
    for (size_t i = 0; i < dimensions; i++)
        bounds[i] = stack->values[start + i];
    stack->depth = start;
    bool result = oasp_runtime_declare_array(runtime, name, bounds, dimensions, (instruction->flags & 1u) != 0,
                                             instruction->opcode == OASP_BC_DECLARE, instruction->line);
    oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
    return result;
}

#if (defined(__GNUC__) || defined(__clang__)) && !defined(OASP_VM_DISABLE_DIRECT_THREADED)
#define OASP_VM_DIRECT_THREADED 1
#define OASP_VM_DISPATCH_LABEL(name) dispatch_##name:;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#else
#define OASP_VM_DIRECT_THREADED 0
#define OASP_VM_DISPATCH_LABEL(name)
#endif

static bool oasp_vm_execute_range_inner(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit, bool error_aware) {
    if (!runtime || !bytecode || !bytecode->instructions || entry > limit || limit > bytecode->length)
        return false;
    oasp_vm_stack stack = {0};
    if (bytecode->max_stack) {
        stack.values = oasp_arena_alloc(runtime->scratch, bytecode->max_stack * sizeof(*stack.values),
                                        _Alignof(oasp_value));
        stack.references = oasp_arena_alloc(runtime->scratch, bytecode->max_stack * sizeof(*stack.references),
                                            _Alignof(uintptr_t));
        if (error_aware)
            stack.error_generations =
                oasp_arena_alloc(runtime->scratch, bytecode->max_stack * sizeof(*stack.error_generations),
                                 _Alignof(uint64_t));
        if (!stack.values || !stack.references || (error_aware && !stack.error_generations))
            return false;
        memset(stack.values, 0, bytecode->max_stack * sizeof(*stack.values));
        stack.capacity = bytecode->max_stack;
    }
    oasp_vm_loop *loops = bytecode->loop_count
                              ? oasp_arena_alloc(runtime->scratch, bytecode->loop_count * sizeof(*loops), _Alignof(oasp_vm_loop))
                              : NULL;
    if (bytecode->loop_count && !loops)
        return false;
    if (loops)
        memset(loops, 0, bytecode->loop_count * sizeof(*loops));
    oasp_array **loop_arrays =
        bytecode->loop_count
            ? oasp_arena_alloc(runtime->scratch, bytecode->loop_count * sizeof(*loop_arrays),
                               _Alignof(oasp_array *))
            : NULL;
    if (bytecode->loop_count && !loop_arrays)
        return false;
    if (loop_arrays)
        memset(loop_arrays, 0, bytecode->loop_count * sizeof(*loop_arrays));
    oasp_object **with_stack =
        bytecode->length ? oasp_arena_alloc(runtime->scratch, bytecode->length * sizeof(*with_stack), _Alignof(oasp_object *)) : NULL;
    if (bytecode->length && !with_stack)
        return false;
    if (with_stack)
        memset(with_stack, 0, bytecode->length * sizeof(*with_stack));
    oasp_gc_root gc_root;
    oasp_runtime_gc_root_enter(runtime, &gc_root, stack.values, stack.capacity, with_stack,
                               bytecode->length, loop_arrays, bytecode->loop_count);
    size_t with_depth = 0;
    size_t pc = entry;
#if OASP_VM_DIRECT_THREADED
    static const void *const dispatch_table[] = {
        &&dispatch_option_explicit,
        &&dispatch_on_error_resume_next,
        &&dispatch_on_error_goto_0,
        &&dispatch_include,
        &&dispatch_push,
        &&dispatch_push,
        &&dispatch_push,
        &&dispatch_push,
        &&dispatch_push,
        &&dispatch_push,
        &&dispatch_get_member,
        &&dispatch_call,
        &&dispatch_call,
        &&dispatch_call,
        &&dispatch_unary,
        &&dispatch_unary,
        &&dispatch_unary,
        &&dispatch_binary,
        &&dispatch_concat_n,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_binary,
        &&dispatch_assign,
        &&dispatch_assign,
        &&dispatch_assign,
        &&dispatch_assign,
        &&dispatch_assign_cookie,
        &&dispatch_assign_slot,
        &&dispatch_assign_slot,
        &&dispatch_declare,
        &&dispatch_declare,
        &&dispatch_declare_const,
        &&dispatch_invalid,
        &&dispatch_with_enter,
        &&dispatch_with_leave,
        &&dispatch_drop,
        &&dispatch_dup,
        &&dispatch_jump_condition,
        &&dispatch_jump_condition,
        &&dispatch_jump,
        &&dispatch_loop_back,
        &&dispatch_for_init,
        &&dispatch_for_next,
        &&dispatch_for_each_init,
        &&dispatch_for_each_next,
        &&dispatch_terminal,
        &&dispatch_terminal,
        &&dispatch_special_binary,
        &&dispatch_special_binary,
        &&dispatch_jump_if_empty,
        &&dispatch_special_binary,
        &&dispatch_load_local_call_builtin,
        &&dispatch_local_const_compare_jump,
        &&dispatch_local_add_const_store,
        &&dispatch_call,
        &&dispatch_append,
        &&dispatch_append,
        &&dispatch_write_const,
        &&dispatch_write_local,
        &&dispatch_write_member,
        &&dispatch_write_value,
        &&dispatch_append_n,
        &&dispatch_append_n,
        &&dispatch_write_n,
        &&dispatch_recordset_field,
        &&dispatch_write_recordset_field,
        &&dispatch_write_recordset_field,
        &&dispatch_write_recordset_field,
        &&dispatch_recordset_move_next,
        &&dispatch_recordset_eof,
        &&dispatch_assign,
        &&dispatch_write_render_recordset_field,
    };
    _Static_assert(sizeof(dispatch_table) / sizeof(dispatch_table[0]) == OASP_BC_COUNT,
                   "dispatch table must cover every opcode");
#endif
    while (pc < limit) {
        if (runtime->format_arena != runtime->scratch)
            oasp_arena_reset(runtime->format_arena);
        const oasp_instruction *instruction = oasp_bytecode_instruction_at(bytecode, pc);
        oasp_vm_trace_pc = pc;
        oasp_vm_trace_opcode = instruction ? instruction->opcode : UINT8_MAX;
        oasp_vm_trace_line = instruction ? instruction->line : 0;
        if (!instruction || !oasp_runtime_safepoint(runtime, instruction->line) ||
            !oasp_runtime_debug_instruction(runtime, bytecode, instruction, stack.values, stack.depth, pc))
            return false;
        oasp_expr_kind operation;
#if OASP_VM_DIRECT_THREADED
        goto *dispatch_table[instruction->opcode];
dispatch_unary:
        if (!oasp_vm_unary_kind((oasp_opcode)instruction->opcode, &operation))
            return false;
#else
        if (oasp_vm_unary_kind((oasp_opcode)instruction->opcode, &operation)) {
#endif
            if (!stack.depth)
                return false;
            size_t index = stack.depth - 1;
            stack.references[index] = 0;
            stack.values[index] = oasp_vm_value_valid(runtime, &stack, index)
                                      ? oasp_runtime_apply_unary(runtime, operation, &stack.values[index], instruction->line)
                                      : oasp_value_empty();
            pc++;
            if (!error_aware && runtime->error_pending)
                return false;
            continue;
#if !OASP_VM_DIRECT_THREADED
        }
#endif
#if OASP_VM_DIRECT_THREADED
dispatch_binary:
        if (!oasp_vm_binary_kind((oasp_opcode)instruction->opcode, &operation))
            return false;
#else
        if (oasp_vm_binary_kind((oasp_opcode)instruction->opcode, &operation)) {
#endif
            if (stack.depth < 2)
                return false;
            size_t right = --stack.depth;
            size_t left = stack.depth - 1;
            stack.values[left] = oasp_runtime_apply_binary(runtime, operation, &stack.values[left],
                                                           &stack.values[right], instruction->line);
            stack.references[left] = 0;
            oasp_vm_merge_error_generation(&stack, left, right);
            pc++;
            if (!error_aware && runtime->error_pending)
                return false;
            continue;
#if !OASP_VM_DIRECT_THREADED
        }
#endif
        switch ((oasp_opcode)instruction->opcode) {
        case OASP_BC_OPTION_EXPLICIT:
            OASP_VM_DISPATCH_LABEL(option_explicit)
            runtime->option_explicit = true;
            pc++;
            break;
        case OASP_BC_ON_ERROR_RESUME_NEXT:
            OASP_VM_DISPATCH_LABEL(on_error_resume_next)
            runtime->resume_next = true;
            pc++;
            break;
        case OASP_BC_ON_ERROR_GOTO_0:
            OASP_VM_DISPATCH_LABEL(on_error_goto_0)
            runtime->resume_next = false;
            runtime->error_number = 0;
            runtime->error_description = NULL;
            runtime->error_source = NULL;
            runtime->error_line = 0;
            runtime->error_pending = false;
            pc++;
            break;
        case OASP_BC_INCLUDE: {
            OASP_VM_DISPATCH_LABEL(include)
            oasp_value include_path;
            if (!oasp_bytecode_constant_at(bytecode, instruction->operand, &include_path) ||
                include_path.kind != OASP_VALUE_STRING ||
                !oasp_runtime_execute_include(runtime, include_path.as.string))
                return false;
            pc++;
            break;
        }
        case OASP_BC_PUSH_EMPTY:
        case OASP_BC_PUSH_LITERAL:
        case OASP_BC_PUSH_NAME:
        case OASP_BC_PUSH_GLOBAL_SLOT:
        case OASP_BC_PUSH_LOCAL_SLOT:
        case OASP_BC_NEW: {
            OASP_VM_DISPATCH_LABEL(push)
            size_t operand;
            if (!oasp_vm_push(&stack, &operand))
                return false;
            oasp_vm_set_error_generation(&stack, operand, runtime->error_generation);
            if (instruction->opcode == OASP_BC_PUSH_EMPTY) {
                stack.values[operand] = oasp_value_empty();
            } else if (instruction->opcode == OASP_BC_PUSH_LITERAL) {
                if (!oasp_bytecode_constant_at(bytecode, instruction->operand, &stack.values[operand]))
                    return false;
            } else if (instruction->opcode == OASP_BC_PUSH_GLOBAL_SLOT) {
                const oasp_value *value = oasp_runtime_get_global_slot(runtime, bytecode, instruction->operand);
                stack.values[operand] = value ? *value : oasp_value_empty();
                if (instruction->operand < bytecode->global_slot_count)
                    stack.references[operand] = oasp_vm_reference(OASP_VM_REFERENCE_GLOBAL, instruction->operand);
            } else if (instruction->opcode == OASP_BC_PUSH_LOCAL_SLOT) {
                const oasp_value *value = oasp_runtime_get_local_slot(runtime, instruction->operand);
                stack.values[operand] = value ? *value : oasp_value_empty();
                if (runtime->frame && instruction->operand < runtime->frame->slot_count)
                    stack.references[operand] = oasp_vm_reference(OASP_VM_REFERENCE_LOCAL, instruction->operand);
            } else {
                oasp_slice name;
                if (!oasp_vm_name(bytecode, instruction->operand, &name))
                    return false;
                if (instruction->opcode == OASP_BC_PUSH_NAME) {
                    if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
                        const oasp_value *root =
                            oasp_vm_slot_value(runtime, bytecode, instruction->slot);
                        oasp_slice member;
                        if (!root || !oasp_vm_dotted_member(name, &member))
                            return false;
                        stack.values[operand] = root->kind == OASP_VALUE_OBJECT
                                                    ? oasp_runtime_get_member_id(runtime, root,
                                                                                 instruction->reserved,
                                                                                 member, instruction->line)
                                                    : oasp_runtime_get_name_id(runtime, name,
                                                                               instruction->reserved,
                                                                               instruction->line);
                        stack.references[operand] =
                            oasp_vm_reference(OASP_VM_REFERENCE_NAME, instruction->operand);
                    } else {
                        stack.values[operand] =
                            oasp_runtime_get_name_id(runtime, name, instruction->reserved, instruction->line);
                        stack.references[operand] =
                            oasp_vm_reference(OASP_VM_REFERENCE_NAME, instruction->operand);
                    }
                } else {
                    oasp_expr expr;
                    memset(&expr, 0, sizeof(expr));
                    expr.kind = OASP_EXPR_NEW;
                    expr.line = instruction->line;
                    expr.name = name;
                    stack.values[operand] = oasp_runtime_eval(runtime, &expr);
                }
            }
            pc++;
            break;
        }
        case OASP_BC_RECORDSET_FIELD: {
            OASP_VM_DISPATCH_LABEL(recordset_field)
            const oasp_value *receiver = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            oasp_value selector;
            if (!receiver || !oasp_bytecode_constant_at(bytecode, instruction->operand, &selector) ||
                selector.kind != OASP_VALUE_STRING)
                return false;
            uint64_t error_generation = runtime->error_generation;
            oasp_value result = oasp_value_empty();
            const oasp_value *view = NULL;
            bool copy_text = true;
            oasp_recordset_ordinal_cache *cache =
                receiver->kind == OASP_VALUE_OBJECT && receiver->as.object
                    ? oasp_vm_recordset_ordinal(runtime, receiver->as.object, selector.as.string)
                    : NULL;
            bool handled = cache &&
                           oasp_vm_recordset_field_view(runtime, receiver->as.object,
                                                        selector.as.string, &cache->ordinal,
                                                        &cache->schema,
                                                        &copy_text,
                                                        instruction->line, &view);
            if (handled && view)
                result = oasp_runtime_value_copy(runtime, view, copy_text);
            if (!handled) {
                if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
                    oasp_slice member;
                    if (!oasp_vm_name(bytecode, instruction->target, &member))
                        return false;
                    result = oasp_runtime_call_member_view(runtime, bytecode, receiver,
                                                           instruction->reserved, member,
                                                           &selector, NULL, 1, instruction->line);
                } else {
                    result = oasp_runtime_call_default_view(runtime, bytecode, receiver,
                                                            &selector, NULL, 1, instruction->line);
                }
            }
            size_t destination;
            if (!oasp_vm_push(&stack, &destination))
                return false;
            stack.values[destination] = result;
            oasp_vm_set_error_generation(&stack, destination, error_generation);
            pc++;
            break;
        }
        case OASP_BC_WRITE_RS_FIELD:
        case OASP_BC_WRITE_HTMLENCODE_RS_FIELD:
        case OASP_BC_WRITE_INT_RS_FIELD: {
            OASP_VM_DISPATCH_LABEL(write_recordset_field)
            const oasp_value *receiver = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            oasp_value selector;
            if (!receiver || !oasp_bytecode_constant_at(bytecode, instruction->operand, &selector) ||
                selector.kind != OASP_VALUE_STRING)
                return false;
            const oasp_value *view = NULL;
            oasp_recordset_ordinal_cache *cache =
                receiver->kind == OASP_VALUE_OBJECT && receiver->as.object
                    ? oasp_vm_recordset_ordinal(runtime, receiver->as.object, selector.as.string)
                    : NULL;
            bool handled = cache &&
                           oasp_vm_recordset_field_view(runtime, receiver->as.object,
                                                        selector.as.string, &cache->ordinal,
                                                        &cache->schema,
                                                        NULL,
                                                        instruction->line, &view);
            oasp_value fallback = oasp_value_empty();
            if (!handled) {
                if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
                    oasp_slice member;
                    if (!oasp_vm_name(bytecode, instruction->target, &member))
                        return false;
                    fallback = oasp_runtime_call_member_view(runtime, bytecode, receiver,
                                                             instruction->reserved, member,
                                                             &selector, NULL, 1,
                                                             instruction->line);
                } else {
                    fallback = oasp_runtime_call_default_view(runtime, bytecode, receiver,
                                                              &selector, NULL, 1,
                                                              instruction->line);
                }
                view = &fallback;
            }
            if (!view)
                view = &fallback;
            if (instruction->opcode == OASP_BC_WRITE_HTMLENCODE_RS_FIELD) {
                if (!oasp_runtime_write_htmlencoded_value(runtime, view, instruction->line))
                    return false;
            } else if (instruction->opcode == OASP_BC_WRITE_INT_RS_FIELD) {
                int64_t integer;
                if (!oasp_value_to_int(view, &integer)) {
                    oasp_runtime_raise(runtime, 13, "type mismatch", "OpenASP",
                                       instruction->line);
                } else {
                    oasp_value converted = oasp_value_int(integer);
                    if (!oasp_runtime_write_value(runtime, &converted, instruction->line))
                        return false;
                }
            } else if (!oasp_runtime_write_value(runtime, view, instruction->line)) {
                return false;
            }
            pc++;
            break;
        }
        case OASP_BC_WRITE_RENDER_RS_FIELD: {
            OASP_VM_DISPATCH_LABEL(write_render_recordset_field)
            const oasp_value *receiver = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            oasp_value selector;
            oasp_value prefix;
            oasp_value suffix;
            if (!receiver || !oasp_bytecode_constant_at(bytecode, instruction->operand, &selector) ||
                !oasp_bytecode_constant_at(bytecode, instruction->target, &prefix) ||
                !oasp_bytecode_constant_at(bytecode, instruction->error_target, &suffix) ||
                selector.kind != OASP_VALUE_STRING || prefix.kind != OASP_VALUE_STRING ||
                suffix.kind != OASP_VALUE_STRING)
                return false;
            const oasp_value *view = NULL;
            oasp_recordset_ordinal_cache *cache =
                receiver->kind == OASP_VALUE_OBJECT && receiver->as.object
                    ? oasp_vm_recordset_ordinal(runtime, receiver->as.object, selector.as.string)
                    : NULL;
            bool handled = cache &&
                           oasp_vm_recordset_field_view(runtime, receiver->as.object, selector.as.string,
                                                        &cache->ordinal, &cache->schema, NULL,
                                                        instruction->line, &view);
            oasp_value fallback = oasp_value_empty();
            if (!handled) {
                if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
                    oasp_slice member = instruction->reserved == OASP_MEMBER_FIELDS
                                            ? (oasp_slice){"fields", 6}
                                            : (oasp_slice){"item", 4};
                    fallback = oasp_runtime_call_member_view(runtime, bytecode, receiver,
                                                             instruction->reserved, member,
                                                             &selector, NULL, 1, instruction->line);
                } else {
                    fallback = oasp_runtime_call_default_view(runtime, bytecode, receiver,
                                                              &selector, NULL, 1, instruction->line);
                }
                view = &fallback;
            }
            if (!view)
                view = &fallback;
            if (!oasp_runtime_write_value(runtime, &prefix, instruction->line))
                return false;
            if (instruction->flags & OASP_BC_FLAG_RENDER_HTMLENCODE) {
                if (!oasp_runtime_write_htmlencoded_value(runtime, view, instruction->line))
                    return false;
            } else if (!oasp_runtime_write_value(runtime, view, instruction->line)) {
                return false;
            }
            if (!oasp_runtime_write_value(runtime, &suffix, instruction->line))
                return false;
            pc++;
            break;
        }
        case OASP_BC_RECORDSET_MOVE_NEXT: {
            OASP_VM_DISPATCH_LABEL(recordset_move_next)
            const oasp_value *receiver = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            if (!receiver)
                return false;
            if (receiver->kind != OASP_VALUE_OBJECT || !receiver->as.object ||
                !oasp_component_recordset_move_next(runtime, receiver->as.object,
                                                    instruction->line)) {
                oasp_slice name;
                if (!oasp_vm_name(bytecode, instruction->operand, &name))
                    return false;
                (void)oasp_runtime_call_name_view(runtime, bytecode, name, 0, 0,
                                                  OASP_MEMBER_MOVENEXT, NULL, NULL, 0,
                                                  instruction->line);
            }
            pc++;
            break;
        }
        case OASP_BC_RECORDSET_EOF: {
            OASP_VM_DISPATCH_LABEL(recordset_eof)
            const oasp_value *receiver = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            if (!receiver)
                return false;
            uint64_t error_generation = runtime->error_generation;
            bool eof = false;
            oasp_value result;
            if (receiver->kind == OASP_VALUE_OBJECT && receiver->as.object &&
                oasp_component_recordset_eof(runtime, receiver->as.object,
                                             instruction->line, &eof)) {
                result = oasp_value_bool(eof);
            } else {
                oasp_slice name;
                if (!oasp_vm_name(bytecode, instruction->operand, &name))
                    return false;
                result = oasp_runtime_get_name_id(runtime, name, OASP_MEMBER_EOF,
                                                  instruction->line);
            }
            size_t destination;
            if (!oasp_vm_push(&stack, &destination))
                return false;
            stack.values[destination] = result;
            oasp_vm_set_error_generation(&stack, destination, error_generation);
            pc++;
            break;
        }
        case OASP_BC_GET_MEMBER: {
            OASP_VM_DISPATCH_LABEL(get_member)
            if (!stack.depth)
                return false;
            oasp_slice name;
            size_t operand = stack.depth - 1;
            if (!oasp_vm_name(bytecode, instruction->operand, &name))
                return false;
            stack.values[operand] =
                oasp_runtime_get_member_id(runtime, &stack.values[operand], instruction->reserved, name,
                                           instruction->line);
            stack.references[operand] = 0;
            pc++;
            break;
        }
        case OASP_BC_CALL_NAME:
        case OASP_BC_CALL_MEMBER:
        case OASP_BC_CALL_DEFAULT:
        case OASP_BC_LOAD_MEMBER_CALL_MEMBER:
            OASP_VM_DISPATCH_LABEL(call)
            if (!oasp_vm_call(runtime, bytecode, &stack, instruction))
                return false;
            pc++;
            break;
        case OASP_BC_LOAD_LOCAL_CALL_BUILTIN: {
            OASP_VM_DISPATCH_LABEL(load_local_call_builtin)
            const oasp_value *value = oasp_runtime_get_local_slot(runtime, instruction->operand);
            oasp_slice name;
            if (!value || !oasp_vm_name(bytecode, instruction->error_target, &name))
                return false;
            oasp_value result =
                oasp_runtime_call_builtin(runtime, instruction->reserved, name, value, 1, instruction->line);
            size_t destination;
            if (!oasp_vm_push(&stack, &destination))
                return false;
            stack.values[destination] = result;
            oasp_vm_set_error_generation(&stack, destination, runtime->error_generation);
            pc++;
            break;
        }
        case OASP_BC_ADD_I64:
        case OASP_BC_COMPARE_STRING:
        case OASP_BC_CONCAT_STRING: {
            OASP_VM_DISPATCH_LABEL(special_binary)
            if (stack.depth < 2)
                return false;
            size_t right = --stack.depth;
            size_t left = stack.depth - 1;
            oasp_expr_kind kind = instruction->opcode == OASP_BC_ADD_I64 ? OASP_EXPR_ADD
                                  : instruction->opcode == OASP_BC_CONCAT_STRING ? OASP_EXPR_CONCAT
                                                                                : (oasp_expr_kind)instruction->flags;
            if (instruction->opcode == OASP_BC_ADD_I64 && stack.values[left].kind == OASP_VALUE_INT &&
                stack.values[right].kind == OASP_VALUE_INT) {
                int64_t sum;
                if (__builtin_add_overflow(stack.values[left].as.integer, stack.values[right].as.integer, &sum)) {
                    oasp_runtime_raise(runtime, 6, "overflow", "OpenASP", instruction->line);
                    stack.values[left] = oasp_value_empty();
                } else {
                    stack.values[left] = oasp_value_int(sum);
                }
            } else {
                stack.values[left] = oasp_runtime_apply_binary(runtime, kind, &stack.values[left],
                                                               &stack.values[right], instruction->line);
            }
            stack.references[left] = 0;
            oasp_vm_merge_error_generation(&stack, left, right);
            pc++;
            break;
        }
        case OASP_BC_JUMP_IF_EMPTY: {
            OASP_VM_DISPATCH_LABEL(jump_if_empty)
            if (!stack.depth)
                return false;
            size_t condition = --stack.depth;
            bool empty = stack.values[condition].kind == OASP_VALUE_EMPTY;
            bool take = instruction->flags ? !empty : empty;
            pc = take ? instruction->target : pc + 1;
            break;
        }
        case OASP_BC_LOCAL_CONST_COMPARE_JUMP: {
            OASP_VM_DISPATCH_LABEL(local_const_compare_jump)
            const oasp_value *left = oasp_runtime_get_local_slot(runtime, instruction->operand);
            oasp_value right;
            if (!left || !oasp_bytecode_constant_at(bytecode, instruction->reserved, &right))
                return false;
            oasp_value compared = oasp_runtime_apply_binary(runtime, (oasp_expr_kind)instruction->slot, left, &right,
                                                            instruction->line);
            if (runtime->error_pending) {
                if (!runtime->resume_next)
                    return false;
                runtime->error_pending = false;
                pc = instruction->error_target;
            } else {
                bool truth = oasp_value_truthy(&compared);
                bool take = instruction->flags ? !truth : truth;
                pc = take ? instruction->target : pc + 1;
            }
            break;
        }
        case OASP_BC_LOCAL_ADD_CONST_STORE: {
            OASP_VM_DISPATCH_LABEL(local_add_const_store)
            const oasp_value *left = oasp_runtime_get_local_slot(runtime, instruction->operand);
            oasp_value right;
            if (!left || !oasp_bytecode_constant_at(bytecode, instruction->reserved, &right))
                return false;
            oasp_value sum;
            if (left->kind == OASP_VALUE_INT && right.kind == OASP_VALUE_INT) {
                int64_t integer;
                if (__builtin_add_overflow(left->as.integer, right.as.integer, &integer)) {
                    oasp_runtime_raise(runtime, 6, "overflow", "OpenASP", instruction->line);
                    sum = oasp_value_empty();
                } else {
                    sum = oasp_value_int(integer);
                }
            } else {
                sum = oasp_runtime_apply_binary(runtime, OASP_EXPR_ADD, left, &right, instruction->line);
            }
            if (!runtime->error_pending &&
                !oasp_runtime_set_local_slot(runtime, instruction->target, &sum, false))
                return false;
            pc++;
            break;
        }
        case OASP_BC_CONCAT_N: {
            OASP_VM_DISPATCH_LABEL(concat_n)
            size_t count = instruction->operand;
            if (count < 2 || stack.depth < count)
                return false;
            size_t start = stack.depth - count;
            stack.values[start] =
                oasp_runtime_concat_values(runtime, &stack.values[start], count, instruction->line);
            stack.references[start] = 0;
            stack.depth = start + 1;
            pc++;
            break;
        }
        case OASP_BC_APPEND_LOCAL:
        case OASP_BC_APPEND_GLOBAL: {
            OASP_VM_DISPATCH_LABEL(append)
            if (!stack.depth)
                return false;
            size_t suffix = --stack.depth;
            if (oasp_vm_value_valid(runtime, &stack, suffix)) {
                bool ok = instruction->opcode == OASP_BC_APPEND_LOCAL
                              ? oasp_runtime_append_local_slot(runtime, instruction->operand,
                                                               &stack.values[suffix], instruction->line)
                              : oasp_runtime_append_global_slot(runtime, bytecode, instruction->operand,
                                                                &stack.values[suffix], instruction->line);
                if (!ok && !runtime->error_pending)
                    return false;
            }
            pc++;
            break;
        }
        case OASP_BC_APPEND_LOCAL_N:
        case OASP_BC_APPEND_GLOBAL_N: {
            OASP_VM_DISPATCH_LABEL(append_n)
            size_t count = instruction->target;
            if (!count || stack.depth < count)
                return false;
            size_t start = stack.depth - count;
            bool valid = true;
            for (size_t i = 0; i < count; i++)
                valid &= oasp_vm_value_valid(runtime, &stack, start + i);
            if (valid) {
                bool ok = instruction->opcode == OASP_BC_APPEND_LOCAL_N
                              ? oasp_runtime_append_local_slot_values(runtime, instruction->operand,
                                                                      &stack.values[start], count,
                                                                      instruction->line)
                              : oasp_runtime_append_global_slot_values(runtime, bytecode,
                                                                       instruction->operand,
                                                                       &stack.values[start], count,
                                                                       instruction->line);
                if (!ok && !runtime->error_pending)
                    return false;
            }
            stack.depth = start;
            pc++;
            break;
        }
        case OASP_BC_WRITE_CONST: {
            OASP_VM_DISPATCH_LABEL(write_const)
            oasp_value value;
            if (!oasp_bytecode_constant_at(bytecode, instruction->operand, &value) ||
                !oasp_runtime_write_value(runtime, &value, instruction->line))
                return false;
            pc++;
            break;
        }
        case OASP_BC_WRITE_LOCAL: {
            OASP_VM_DISPATCH_LABEL(write_local)
            const oasp_value *value = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
            if (!value || !oasp_runtime_write_value(runtime, value, instruction->line))
                return false;
            pc++;
            break;
        }
        case OASP_BC_WRITE_MEMBER: {
            OASP_VM_DISPATCH_LABEL(write_member)
            oasp_value receiver;
            oasp_slice member;
            if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
                const oasp_value *root = oasp_vm_slot_value(runtime, bytecode, instruction->slot);
                oasp_slice dotted_name;
                if (!root || !oasp_vm_name(bytecode, instruction->operand, &dotted_name) ||
                    !oasp_vm_dotted_member(dotted_name, &member))
                    return false;
                receiver = *root;
                if (receiver.kind != OASP_VALUE_OBJECT) {
                    oasp_value value =
                        oasp_runtime_get_name_id(runtime, dotted_name, instruction->reserved,
                                                 instruction->line);
                    if (!runtime->error_pending &&
                        !oasp_runtime_write_value(runtime, &value, instruction->line))
                        return false;
                    pc++;
                    break;
                }
            } else {
                if (!stack.depth)
                    return false;
                receiver = stack.values[--stack.depth];
                if (!oasp_vm_name(bytecode, instruction->operand, &member))
                    return false;
            }
            oasp_value value =
                oasp_runtime_get_member_id(runtime, &receiver, instruction->reserved, member,
                                           instruction->line);
            if (!runtime->error_pending &&
                !oasp_runtime_write_value(runtime, &value, instruction->line))
                return false;
            pc++;
            break;
        }
        case OASP_BC_WRITE_VALUE: {
            OASP_VM_DISPATCH_LABEL(write_value)
            if (!stack.depth)
                return false;
            size_t value = --stack.depth;
            if (oasp_vm_value_valid(runtime, &stack, value) &&
                !oasp_runtime_write_value(runtime, &stack.values[value], instruction->line))
                return false;
            pc++;
            break;
        }
        case OASP_BC_WRITE_N: {
            OASP_VM_DISPATCH_LABEL(write_n)
            size_t count = instruction->target;
            if (count < 2 || stack.depth < count)
                return false;
            size_t start = stack.depth - count;
            bool valid = true;
            for (size_t i = 0; i < count; i++)
                valid &= oasp_vm_value_valid(runtime, &stack, start + i);
            if (valid && !oasp_runtime_write_values(runtime, &stack.values[start], count,
                                                    instruction->line))
                return false;
            stack.depth = start;
            pc++;
            break;
        }
        case OASP_BC_ASSIGN_NAME:
        case OASP_BC_ASSIGN_MEMBER:
        case OASP_BC_ASSIGN_INDEX_NAME:
        case OASP_BC_ASSIGN_INDEX_MEMBER:
        case OASP_BC_ASSIGN_INDEX_DEFAULT: {
            OASP_VM_DISPATCH_LABEL(assign)
            if (!stack.depth)
                return false;
            oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
            size_t consumed = 0;
            oasp_expr *target = oasp_vm_assignment_target(runtime, bytecode, &stack, instruction, &consumed);
            if (!target || stack.depth < consumed + 1) {
                oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
                return false;
            }
            size_t value = stack.depth - consumed - 1;
            if (oasp_vm_value_valid(runtime, &stack, value))
                (void)oasp_runtime_assign_value(runtime, target, &stack.values[value], instruction->line);
            stack.depth -= consumed + 1;
            oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
            pc++;
            break;
        }
        case OASP_BC_ASSIGN_COOKIE_ATTRIBUTE: {
            OASP_VM_DISPATCH_LABEL(assign_cookie)
            size_t count = instruction->target;
            if (stack.depth < count + 1 || instruction->operand >= bytecode->name_count)
                return false;
            oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
            size_t value_index = stack.depth - count - 1;
            oasp_expr *identifier = oasp_vm_expr(runtime, OASP_EXPR_IDENTIFIER, instruction->line);
            oasp_expr *call = oasp_vm_expr(runtime, OASP_EXPR_CALL, instruction->line);
            oasp_expr *member = oasp_vm_expr(runtime, OASP_EXPR_MEMBER, instruction->line);
            oasp_expr **arguments =
                count ? oasp_arena_alloc(runtime->scratch, count * sizeof(*arguments), _Alignof(oasp_expr *)) : NULL;
            if (!identifier || !call || !member || (count && !arguments)) {
                oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
                return false;
            }
            identifier->name = (oasp_slice){"response.cookies", 16};
            for (size_t i = 0; i < count; i++) {
                arguments[i] = oasp_vm_literal(runtime, &stack.values[value_index + 1 + i], instruction->line);
                if (!arguments[i]) {
                    oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
                    return false;
                }
            }
            call->left = identifier;
            call->args = arguments;
            call->arg_count = count;
            member->left = call;
            if (!oasp_bytecode_name_at(bytecode, instruction->operand, &member->name)) {
                oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
                return false;
            }
            (void)oasp_runtime_assign_value(runtime, member, &stack.values[value_index], instruction->line);
            stack.depth = value_index;
            oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
            pc++;
            break;
        }
        case OASP_BC_ASSIGN_GLOBAL_SLOT:
        case OASP_BC_ASSIGN_LOCAL_SLOT: {
            OASP_VM_DISPATCH_LABEL(assign_slot)
            if (!stack.depth)
                return false;
            size_t value = --stack.depth;
            if (oasp_vm_value_valid(runtime, &stack, value)) {
                bool ok = instruction->opcode == OASP_BC_ASSIGN_GLOBAL_SLOT
                              ? oasp_runtime_set_global_slot(runtime, bytecode, instruction->operand,
                                                             &stack.values[value], false)
                              : oasp_runtime_set_local_slot(runtime, instruction->operand, &stack.values[value], false);
                if (!ok && !runtime->error_pending)
                    return false;
            }
            pc++;
            break;
        }
        case OASP_BC_DECLARE:
        case OASP_BC_REDIM:
            OASP_VM_DISPATCH_LABEL(declare)
            if (!oasp_vm_declare(runtime, bytecode, &stack, instruction))
                return false;
            pc++;
            break;
        case OASP_BC_DECLARE_CONST: {
            OASP_VM_DISPATCH_LABEL(declare_const)
            if (!stack.depth || instruction->operand >= bytecode->name_count)
                return false;
            size_t value = --stack.depth;
            oasp_slice name;
            if (!oasp_bytecode_name_at(bytecode, instruction->operand, &name))
                return false;
            size_t slot = 0;
            while (slot < bytecode->global_slot_count) {
                oasp_slice global_name;
                if (!oasp_bytecode_global_slot_at(bytecode, slot, &global_name))
                    return false;
                if (global_name.length == name.length &&
                    !strncasecmp(global_name.data, name.data, global_name.length))
                    break;
                slot++;
            }
            if (!(slot < bytecode->global_slot_count
                      ? oasp_runtime_set_global_slot(runtime, bytecode, slot, &stack.values[value], true)
                      : oasp_runtime_set(runtime, name, &stack.values[value], true)))
                return false;
            pc++;
            break;
        }
        case OASP_BC_WITH_ENTER:
            OASP_VM_DISPATCH_LABEL(with_enter)
            if (!stack.depth || stack.values[stack.depth - 1].kind != OASP_VALUE_OBJECT)
                return false;
            with_stack[with_depth++] = runtime->with_object;
            runtime->with_object = stack.values[--stack.depth].as.object;
            pc++;
            break;
        case OASP_BC_WITH_LEAVE:
            OASP_VM_DISPATCH_LABEL(with_leave)
            if (!with_depth)
                return false;
            runtime->with_object = with_stack[--with_depth];
            pc++;
            break;
        case OASP_BC_DROP:
            OASP_VM_DISPATCH_LABEL(drop)
            if (!stack.depth)
                return false;
            stack.depth--;
            pc++;
            break;
        case OASP_BC_DUP: {
            OASP_VM_DISPATCH_LABEL(dup)
            if (!stack.depth)
                return false;
            size_t source = stack.depth - 1;
            oasp_value value = stack.values[source];
            uintptr_t reference = stack.references[source];
            uint64_t error_generation =
                stack.error_generations ? stack.error_generations[source] : runtime->error_generation;
            size_t destination;
            if (!oasp_vm_push(&stack, &destination))
                return false;
            stack.values[destination] = value;
            stack.references[destination] = reference;
            oasp_vm_set_error_generation(&stack, destination, error_generation);
            pc++;
            break;
        }
        case OASP_BC_JUMP_IF_FALSE:
        case OASP_BC_JUMP_IF_TRUE: {
            OASP_VM_DISPATCH_LABEL(jump_condition)
            if (!stack.depth)
                return false;
            size_t condition = --stack.depth;
            if (!oasp_vm_value_valid(runtime, &stack, condition)) {
                if (!runtime->resume_next)
                    return false;
                runtime->error_pending = false;
                pc = instruction->error_target;
            } else {
                bool truth = oasp_value_truthy(&stack.values[condition]);
                pc = ((instruction->opcode == OASP_BC_JUMP_IF_TRUE) == truth) ? instruction->target : pc + 1;
            }
            break;
        }
        case OASP_BC_JUMP:
            OASP_VM_DISPATCH_LABEL(jump)
            pc = instruction->target;
            break;
        case OASP_BC_LOOP_BACK:
            OASP_VM_DISPATCH_LABEL(loop_back)
            if (instruction->operand >= bytecode->loop_count)
                return false;
            pc = instruction->target;
            break;
        case OASP_BC_FOR_INIT: {
            OASP_VM_DISPATCH_LABEL(for_init)
            size_t count = instruction->error_target;
            if (instruction->operand >= bytecode->loop_count || stack.depth < count || (count != 2 && count != 3))
                return false;
            oasp_value step = count == 3 ? stack.values[--stack.depth] : oasp_value_int(1);
            oasp_value upper = stack.values[--stack.depth];
            oasp_value lower = stack.values[--stack.depth];
            oasp_vm_loop *loop = &loops[instruction->operand];
            if (!oasp_value_to_float(&lower, &loop->current) || !oasp_value_to_float(&upper, &loop->limit) ||
                !oasp_value_to_float(&step, &loop->step) || loop->step == 0.0) {
                oasp_runtime_raise(runtime, 13, "type mismatch", "OpenASP", instruction->line);
                if (!runtime->resume_next)
                    return false;
                runtime->error_pending = false;
                pc = instruction->target;
                break;
            }
            if (!(loop->step > 0.0 ? loop->current <= loop->limit : loop->current >= loop->limit))
                pc = instruction->target;
            else {
                oasp_value value = loop->current == (double)(int64_t)loop->current ? oasp_value_int((int64_t)loop->current)
                                                                                  : oasp_value_float(loop->current);
                if (!oasp_vm_set_loop_value(runtime, bytecode, instruction, &value))
                    return false;
                pc++;
            }
            break;
        }
        case OASP_BC_FOR_NEXT: {
            OASP_VM_DISPATCH_LABEL(for_next)
            if (instruction->operand >= bytecode->loop_count)
                return false;
            oasp_vm_loop *loop = &loops[instruction->operand];
            loop->current += loop->step;
            if (!(loop->step > 0.0 ? loop->current <= loop->limit : loop->current >= loop->limit))
                pc++;
            else {
                oasp_value value = loop->current == (double)(int64_t)loop->current ? oasp_value_int((int64_t)loop->current)
                                                                                  : oasp_value_float(loop->current);
                if (!oasp_vm_set_loop_value(runtime, bytecode, instruction, &value))
                    return false;
                pc = instruction->target;
            }
            break;
        }
        case OASP_BC_FOR_EACH_INIT: {
            OASP_VM_DISPATCH_LABEL(for_each_init)
            if (instruction->operand >= bytecode->loop_count || !stack.depth)
                return false;
            oasp_value collection = stack.values[--stack.depth];
            if (collection.kind == OASP_VALUE_OBJECT) {
                oasp_value enumerated = oasp_value_empty();
                if (oasp_component_enumerate(runtime, collection.as.object, &enumerated))
                    collection = enumerated;
            }
            if (collection.kind != OASP_VALUE_ARRAY) {
                oasp_runtime_raise(runtime, 13, "type mismatch", "OpenASP", instruction->line);
                if (!runtime->resume_next)
                    return false;
                runtime->error_pending = false;
                pc = instruction->target;
                break;
            }
            oasp_vm_loop *loop = &loops[instruction->operand];
            loop->array = collection.as.array;
            loop_arrays[instruction->operand] = collection.as.array;
            loop->index = 0;
            if (!loop->array || !loop->array->length)
                pc = instruction->target;
            else if (!oasp_vm_set_loop_value(runtime, bytecode, instruction, &loop->array->items[0]))
                return false;
            else
                pc++;
            break;
        }
        case OASP_BC_FOR_EACH_NEXT: {
            OASP_VM_DISPATCH_LABEL(for_each_next)
            if (instruction->operand >= bytecode->loop_count)
                return false;
            oasp_vm_loop *loop = &loops[instruction->operand];
            loop->index++;
            if (!loop->array || loop->index >= loop->array->length)
                pc++;
            else if (!oasp_vm_set_loop_value(runtime, bytecode, instruction, &loop->array->items[loop->index]))
                return false;
            else
                pc = instruction->target;
            break;
        }
        case OASP_BC_RETURN:
        case OASP_BC_HALT:
            OASP_VM_DISPATCH_LABEL(terminal)
            if (stack.depth != 0 && oasp_process_options.trace_vm)
                fprintf(stderr,
                        "OpenASP VM terminal imbalance: pc=%zu opcode=%u line=%zu stack=%zu "
                        "error=%d pending=%d stopped=%d\n",
                        pc, instruction->opcode, (size_t)instruction->line, stack.depth, runtime->error_number,
                        runtime->error_pending ? 1 : 0, runtime->stopped ? 1 : 0);
            return stack.depth == 0;
        default:
            OASP_VM_DISPATCH_LABEL(invalid)
            return false;
        }
        if (runtime->error_pending) {
            if (!runtime->resume_next)
                return false;
            runtime->error_pending = false;
        }
        if (runtime->stopped)
            return true;
    }
    if (stack.depth != 0 && oasp_process_options.trace_vm)
        fprintf(stderr,
                "OpenASP VM range imbalance: pc=%zu limit=%zu stack=%zu error=%d pending=%d stopped=%d\n",
                pc, limit, stack.depth, runtime->error_number, runtime->error_pending ? 1 : 0,
                runtime->stopped ? 1 : 0);
    return stack.depth == 0;
}

#if OASP_VM_DIRECT_THREADED
#if defined(__clang__)
#pragma clang diagnostic pop
#else
#pragma GCC diagnostic pop
#endif
#endif
#undef OASP_VM_DISPATCH_LABEL

bool oasp_vm_execute_range_mode(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit, bool error_aware) {
    if (!runtime || !runtime->scratch)
        return false;
    oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(runtime->scratch);
    oasp_object *previous_with = runtime->with_object;
    oasp_gc_root *previous_roots = runtime->gc_roots;
    bool result = oasp_vm_execute_range_inner(runtime, bytecode, entry, limit, error_aware);
    if (!result && oasp_process_options.trace_vm)
        fprintf(stderr,
                "OpenASP VM failure: entry=%zu limit=%zu last_pc=%zu opcode=%u line=%u "
                "error=%d pending=%d stopped=%d\n",
                entry, limit, oasp_vm_trace_pc, oasp_vm_trace_opcode, oasp_vm_trace_line,
                runtime->error_number, runtime->error_pending ? 1 : 0, runtime->stopped ? 1 : 0);
    runtime->gc_roots = previous_roots;
    runtime->with_object = previous_with;
    oasp_arena_checkpoint_restore(runtime->scratch, checkpoint);
    return result;
}

bool oasp_vm_execute_range(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit) {
    return oasp_vm_execute_range_mode(runtime, bytecode, entry, limit, true);
}

bool oasp_vm_execute_range_inline_mode(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit, bool error_aware) {
    if (!runtime)
        return false;
    oasp_object *previous_with = runtime->with_object;
    oasp_gc_root *previous_roots = runtime->gc_roots;
    bool result = oasp_vm_execute_range_inner(runtime, bytecode, entry, limit, error_aware);
    runtime->gc_roots = previous_roots;
    runtime->with_object = previous_with;
    return result;
}

bool oasp_vm_execute_range_inline(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit) {
    return oasp_vm_execute_range_inline_mode(runtime, bytecode, entry, limit, true);
}

static size_t oasp_vm_root_limit(const oasp_bytecode *bytecode) {
    if (!bytecode)
        return 0;
    size_t limit = bytecode->length;
    oasp_class_bytecode class_def;
    oasp_routine_bytecode routine;
    if (bytecode->class_count && oasp_bytecode_class_at(bytecode, 0, &class_def) && class_def.entry < limit)
        limit = class_def.entry;
    if (bytecode->routine_count && oasp_bytecode_routine_at(bytecode, 0, &routine) && routine.entry < limit)
        limit = routine.entry;
    return limit;
}

bool oasp_vm_execute(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    return bytecode && oasp_vm_execute_range_mode(runtime, bytecode, 0, oasp_vm_root_limit(bytecode),
                                                  bytecode->root_error_aware);
}

bool oasp_vm_execute_inline(oasp_runtime *runtime, const oasp_bytecode *bytecode) {
    return bytecode && oasp_vm_execute_range_inline_mode(runtime, bytecode, 0, oasp_vm_root_limit(bytecode),
                                                         bytecode->root_error_aware);
}
