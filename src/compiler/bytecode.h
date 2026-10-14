// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Compiler-to-VM contract. Instructions use fixed-width fields so they can be
 * validated, serialized, memory-mapped, and executed without pointer fixups.
 * Every table reference is an index checked by oasp_bytecode_validate(); the VM
 * must never execute externally loaded bytecode before that validation passes.
 */

#ifndef OPENASP_BYTECODE_H
#define OPENASP_BYTECODE_H

#include "parser.h"

typedef enum {
    OASP_BC_OPTION_EXPLICIT,
    OASP_BC_ON_ERROR_RESUME_NEXT,
    OASP_BC_ON_ERROR_GOTO_0,
    OASP_BC_INCLUDE,
    OASP_BC_PUSH_EMPTY,
    OASP_BC_PUSH_LITERAL,
    OASP_BC_PUSH_NAME,
    OASP_BC_PUSH_GLOBAL_SLOT,
    OASP_BC_PUSH_LOCAL_SLOT,
    OASP_BC_NEW,
    OASP_BC_GET_MEMBER,
    OASP_BC_CALL_NAME,
    OASP_BC_CALL_MEMBER,
    OASP_BC_CALL_DEFAULT,
    OASP_BC_NEGATE,
    OASP_BC_POSITIVE,
    OASP_BC_NOT,
    OASP_BC_CONCAT,
    OASP_BC_CONCAT_N,
    OASP_BC_EQ,
    OASP_BC_NE,
    OASP_BC_LT,
    OASP_BC_LE,
    OASP_BC_GT,
    OASP_BC_GE,
    OASP_BC_ADD,
    OASP_BC_SUBTRACT,
    OASP_BC_MULTIPLY,
    OASP_BC_DIVIDE,
    OASP_BC_INT_DIVIDE,
    OASP_BC_MOD,
    OASP_BC_POWER,
    OASP_BC_AND,
    OASP_BC_OR,
    OASP_BC_XOR,
    OASP_BC_EQV,
    OASP_BC_IMP,
    OASP_BC_IS,
    OASP_BC_IS_NOT,
    OASP_BC_ASSIGN_NAME,
    OASP_BC_ASSIGN_MEMBER,
    OASP_BC_ASSIGN_INDEX_NAME,
    OASP_BC_ASSIGN_INDEX_MEMBER,
    OASP_BC_ASSIGN_COOKIE_ATTRIBUTE,
    OASP_BC_ASSIGN_GLOBAL_SLOT,
    OASP_BC_ASSIGN_LOCAL_SLOT,
    OASP_BC_DECLARE,
    OASP_BC_REDIM,
    OASP_BC_DECLARE_CONST,
    OASP_BC_ERASE,
    OASP_BC_WITH_ENTER,
    OASP_BC_WITH_LEAVE,
    OASP_BC_DROP,
    OASP_BC_DUP,
    OASP_BC_JUMP_IF_FALSE,
    OASP_BC_JUMP_IF_TRUE,
    OASP_BC_JUMP,
    OASP_BC_LOOP_BACK,
    OASP_BC_FOR_INIT,
    OASP_BC_FOR_NEXT,
    OASP_BC_FOR_EACH_INIT,
    OASP_BC_FOR_EACH_NEXT,
    OASP_BC_RETURN,
    OASP_BC_HALT,
    OASP_BC_ADD_I64,
    OASP_BC_COMPARE_STRING,
    OASP_BC_JUMP_IF_EMPTY,
    OASP_BC_CONCAT_STRING,
    OASP_BC_LOAD_LOCAL_CALL_BUILTIN,
    OASP_BC_LOCAL_CONST_COMPARE_JUMP,
    OASP_BC_LOCAL_ADD_CONST_STORE,
    OASP_BC_LOAD_MEMBER_CALL_MEMBER,
    OASP_BC_APPEND_LOCAL,
    OASP_BC_APPEND_GLOBAL,
    OASP_BC_WRITE_CONST,
    OASP_BC_WRITE_LOCAL,
    OASP_BC_WRITE_MEMBER,
    OASP_BC_WRITE_VALUE,
    OASP_BC_APPEND_LOCAL_N,
    OASP_BC_APPEND_GLOBAL_N,
    OASP_BC_WRITE_N,
    OASP_BC_RECORDSET_FIELD,
    OASP_BC_WRITE_RS_FIELD,
    OASP_BC_WRITE_HTMLENCODE_RS_FIELD,
    OASP_BC_WRITE_INT_RS_FIELD,
    OASP_BC_RECORDSET_MOVE_NEXT,
    OASP_BC_RECORDSET_EOF,
    OASP_BC_ASSIGN_INDEX_DEFAULT,
    OASP_BC_WRITE_RENDER_RS_FIELD,
    OASP_BC_COUNT
} oasp_opcode;

#define OASP_BC_FLAG_STATIC_BUILTIN UINT8_C(0x01)
#define OASP_BC_FLAG_SYMBOL_HINT UINT8_C(0x02)
#define OASP_BC_FLAG_CALL_STATEMENT UINT8_C(0x20)
#define OASP_BC_FLAG_STATIC_MEMBER UINT8_C(0x40)
#define OASP_BC_FLAG_COMPARE_SHIFT 2
#define OASP_BC_FLAG_COMPARE_MASK UINT8_C(0x3c)
#define OASP_BC_FLAG_ROOT_SLOT UINT8_C(0x80)
#define OASP_BC_FLAG_RENDER_CSTR UINT8_C(0x01)
#define OASP_BC_FLAG_RENDER_HTMLENCODE UINT8_C(0x02)

#define OASP_STATE_READS_SESSION UINT32_C(0x01)
#define OASP_STATE_WRITES_SESSION UINT32_C(0x02)
#define OASP_STATE_READS_APPLICATION UINT32_C(0x04)
#define OASP_STATE_WRITES_APPLICATION UINT32_C(0x08)

typedef struct oasp_instruction {
    /* Operation and optimizer-produced metadata interpreted by the VM. */
    uint8_t opcode;
    uint8_t flags;
    /* Kept zero to make the serialized instruction format forward-compatible. */
    uint16_t reserved;
    /* Source line used for runtime errors and control-flow destinations. */
    uint32_t line;
    uint32_t target;
    uint32_t error_target;
    /* Opcode-specific table index/immediate and resolved lexical slot. */
    uint32_t operand;
    int32_t slot;
} oasp_instruction;

typedef oasp_instruction oasp_image_instruction;

typedef struct oasp_parameter_bytecode {
    oasp_slice name;
    bool by_value;
    bool array;
} oasp_parameter_bytecode;

typedef struct oasp_routine_bytecode {
    /* Name, slots, and parameters are pointers in compiled form and offsets in images. */
    oasp_slice name;
    union {
        oasp_slice *slot_names;
        size_t slot_offset;
    };
    size_t slot_count;
    union {
        oasp_parameter_bytecode *parameters;
        size_t parameter_offset;
    };
    size_t parameter_count;
    /* Half-open instruction range [entry, limit) and owning class, if any. */
    size_t entry;
    size_t limit;
    size_t class_index;
    oasp_routine_kind kind;
    bool is_default;
    bool error_aware;
} oasp_routine_bytecode;

typedef struct oasp_class_bytecode {
    oasp_slice name;
    size_t entry;
    size_t limit;
    size_t routine_offset;
    size_t routine_count;
    bool error_aware;
} oasp_class_bytecode;

typedef struct oasp_bytecode {
    /* Instruction storage is arena-owned or decoded from `image_base`. */
    oasp_instruction *instructions;
    void *image_decode_storage;
    const unsigned char *image_base;
    size_t image_size;
    /* Opaque encoded table bases; access only through the *_at helpers. */
    const void *image_names;
    const void *image_constants;
    const void *image_global_slots;
    const void *image_routines;
    const void *image_parameters;
    const void *image_routine_slots;
    const void *image_classes;
    /* Logical table sizes bound all VM operands and public indexed access. */
    size_t length;
    size_t loop_count;
    oasp_slice *names;
    size_t name_count;
    oasp_value *constants;
    size_t constant_count;
    oasp_slice *global_slot_names;
    size_t global_slot_count;
    oasp_routine_bytecode *routines;
    size_t routine_count;
    oasp_class_bytecode *classes;
    size_t class_count;
    /* Validation/optimization metadata consumed by runtime fast paths. */
    size_t max_stack;
    uint32_t state_effects;
    bool root_error_aware;
    size_t opcode_counts[OASP_BC_COUNT];
} oasp_bytecode;

/*
 * Compiles a parsed block into arena-owned bytecode. Module compilation keeps
 * definitions suitable for registration by another root program. On failure
 * the arena remains the sole owner of any partial allocations.
 */
bool oasp_bytecode_compile(oasp_arena *arena, const oasp_block *block, oasp_bytecode *bytecode);
bool oasp_bytecode_compile_module(oasp_arena *arena, const oasp_block *block, oasp_bytecode *bytecode);

/*
 * Proves opcode, stack-depth, jump-range, slot, and table-index invariants.
 * Successful validation also computes metadata used by execution fast paths.
 */
bool oasp_bytecode_validate(oasp_bytecode *bytecode);

/* Checked table accessors support both arena-compiled and image-backed layouts. */
const oasp_instruction *oasp_bytecode_instruction_at(const oasp_bytecode *bytecode, size_t index);
bool oasp_bytecode_name_at(const oasp_bytecode *bytecode, size_t index, oasp_slice *result);
bool oasp_bytecode_constant_at(const oasp_bytecode *bytecode, size_t index, oasp_value *result);
bool oasp_bytecode_global_slot_at(const oasp_bytecode *bytecode, size_t index, oasp_slice *result);
bool oasp_bytecode_routine_name_at(const oasp_bytecode *bytecode, size_t index, oasp_slice *result);
bool oasp_bytecode_routine_at(const oasp_bytecode *bytecode, size_t index, oasp_routine_bytecode *result);
bool oasp_bytecode_routine_parameter(const oasp_bytecode *bytecode, const oasp_routine_bytecode *routine, size_t index, oasp_parameter_bytecode *result);
bool oasp_bytecode_routine_parameter_at(const oasp_bytecode *bytecode, size_t routine_index, size_t index, oasp_parameter_bytecode *result);
bool oasp_bytecode_routine_slot(const oasp_bytecode *bytecode, const oasp_routine_bytecode *routine, size_t index, oasp_slice *result);
bool oasp_bytecode_routine_slot_at(const oasp_bytecode *bytecode, size_t routine_index, size_t index, oasp_slice *result);
bool oasp_bytecode_class_at(const oasp_bytecode *bytecode, size_t index, oasp_class_bytecode *result);

/*
 * Finds a routine by case-insensitive `name`, optional class, accepted kinds,
 * and default-member requirement; writes its table index to `result`.
 */
bool oasp_bytecode_find_routine(const oasp_bytecode *bytecode, oasp_slice name, size_t class_index, oasp_routine_kind first, oasp_routine_kind second,
                                bool default_only, size_t *result);

#endif
