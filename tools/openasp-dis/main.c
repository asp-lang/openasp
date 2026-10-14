// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * OpenASP OAC image disassembler. The image remains immutable and alive while
 * module_image exposes zero-copy slices into it. All structural and bytecode
 * validation is delegated to the same decoder used by the runtime.
 */

#define _POSIX_C_SOURCE 200809L

#include "bytecode_format.h"
#include "file.h"
#include "module_image.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    oasp_slice text;
    size_t *offsets;
    size_t *lengths;
    size_t line_count;
} debug_source_index;

typedef enum {
    DISASSEMBLY_REGION_ROOT,
    DISASSEMBLY_REGION_CLASS,
    DISASSEMBLY_REGION_ROUTINE
} disassembly_region_kind;

typedef struct {
    disassembly_region_kind kind;
    size_t entry;
    size_t limit;
    oasp_slice owner;
    oasp_slice name;
} disassembly_region;

static const char *routine_kind_name(oasp_routine_kind kind) {
    static const char *const names[] = {"sub", "function", "property-get", "property-let", "property-set"};
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static bool debug_source_index_init(const oasp_module_image *image, debug_source_index *index) {
    memset(index, 0, sizeof(*index));
    if (!image->has_debug_info)
        return true;
    if (!oasp_module_image_debug_source(image, &index->text))
        return false;
    size_t count = 1;
    for (size_t i = 0; i < index->text.length; i++) {
        if (index->text.data[i] == '\r') {
            if (i + 1 < index->text.length && index->text.data[i + 1] == '\n')
                i++;
            count++;
        } else if (index->text.data[i] == '\n') {
            count++;
        }
    }
    if (count > SIZE_MAX / sizeof(*index->offsets))
        return false;
    index->offsets = malloc(count * sizeof(*index->offsets));
    index->lengths = malloc(count * sizeof(*index->lengths));
    if (!index->offsets || !index->lengths) {
        free(index->offsets);
        free(index->lengths);
        memset(index, 0, sizeof(*index));
        return false;
    }
    size_t line = 0;
    size_t start = 0;
    for (size_t i = 0; i < index->text.length; i++) {
        if (index->text.data[i] != '\r' && index->text.data[i] != '\n')
            continue;
        index->offsets[line] = start;
        index->lengths[line++] = i - start;
        if (index->text.data[i] == '\r' && i + 1 < index->text.length &&
            index->text.data[i + 1] == '\n')
            i++;
        start = i + 1;
    }
    index->offsets[line] = start;
    index->lengths[line++] = index->text.length - start;
    index->line_count = line;
    return true;
}

static void debug_source_index_dispose(debug_source_index *index) {
    if (!index)
        return;
    free(index->offsets);
    free(index->lengths);
    memset(index, 0, sizeof(*index));
}

static bool debug_source_line(const debug_source_index *index, uint32_t line, oasp_slice *result) {
    if (!index || !result || !line || line > index->line_count)
        return false;
    size_t position = (size_t)line - 1;
    result->data = index->text.data + index->offsets[position];
    result->length = index->lengths[position];
    return true;
}

static bool print_names(const oasp_bytecode *bytecode) {
    printf("\nNames (%zu)\n", bytecode->name_count);
    for (size_t i = 0; i < bytecode->name_count; i++) {
        oasp_slice name;
        if (!oasp_bytecode_name_at(bytecode, i, &name))
            return false;
        printf("  [%zu] ", i);
        oasp_tool_print_escaped(stdout, name);
        putchar('\n');
    }
    return true;
}

static bool print_constants(const oasp_bytecode *bytecode) {
    printf("\nConstants (%zu)\n", bytecode->constant_count);
    for (size_t i = 0; i < bytecode->constant_count; i++) {
        oasp_value value;
        if (!oasp_bytecode_constant_at(bytecode, i, &value))
            return false;
        printf("  [%zu] ", i);
        oasp_tool_print_value(stdout, &value, OASP_TOOL_VALUE_TYPED);
        putchar('\n');
    }
    return true;
}

static bool print_globals(const oasp_bytecode *bytecode) {
    printf("\nGlobal slots (%zu)\n", bytecode->global_slot_count);
    for (size_t i = 0; i < bytecode->global_slot_count; i++) {
        oasp_slice name;
        if (!oasp_bytecode_global_slot_at(bytecode, i, &name))
            return false;
        printf("  [%zu] ", i);
        oasp_tool_print_escaped(stdout, name);
        putchar('\n');
    }
    return true;
}

static bool print_dependencies(const oasp_module_image *image) {
    printf("\nDependencies (%zu)\n", image->dependency_count);
    for (size_t i = 0; i < image->dependency_count; i++) {
        oasp_module_image_dependency dependency;
        if (!oasp_module_image_dependency_at(image, i, &dependency))
            return false;
        printf("  [%zu] path=", i);
        oasp_tool_print_escaped(stdout, dependency.path);
        printf(" mtime=%" PRId64 ".%09" PRId64 " size=%" PRId64 "\n", dependency.modified_seconds,
               dependency.modified_nanoseconds, dependency.file_size);
    }
    return true;
}

static bool print_modules(const oasp_module_image *image) {
    printf("\nModules (%zu)\n", image->module_count);
    for (size_t i = 0; i < image->module_count; i++) {
        oasp_slice module;
        if (!oasp_module_image_module_at(image, i, &module))
            return false;
        printf("  [%zu] ", i);
        oasp_tool_print_escaped(stdout, module);
        putchar('\n');
    }
    return true;
}

static bool print_routines(const oasp_bytecode *bytecode) {
    printf("\nRoutines (%zu)\n", bytecode->routine_count);
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        oasp_routine_bytecode routine;
        if (!oasp_bytecode_routine_at(bytecode, i, &routine))
            return false;
        printf("  [%zu] name=", i);
        oasp_tool_print_escaped(stdout, routine.name);
        printf(" kind=%s range=[%zu,%zu) class=", routine_kind_name(routine.kind), routine.entry, routine.limit);
        if (routine.class_index == SIZE_MAX)
            fputs("none", stdout);
        else
            printf("%zu", routine.class_index);
        printf(" default=%s error-aware=%s\n", routine.is_default ? "yes" : "no", routine.error_aware ? "yes" : "no");
        for (size_t j = 0; j < routine.parameter_count; j++) {
            oasp_parameter_bytecode parameter;
            if (!oasp_bytecode_routine_parameter(bytecode, &routine, j, &parameter))
                return false;
            printf("    param[%zu] name=", j);
            oasp_tool_print_escaped(stdout, parameter.name);
            printf(" mode=%s array=%s\n", parameter.by_value ? "ByVal" : "ByRef", parameter.array ? "yes" : "no");
        }
        for (size_t j = 0; j < routine.slot_count; j++) {
            oasp_slice slot;
            if (!oasp_bytecode_routine_slot(bytecode, &routine, j, &slot))
                return false;
            printf("    slot[%zu] ", j);
            oasp_tool_print_escaped(stdout, slot);
            putchar('\n');
        }
    }
    return true;
}

static bool print_classes(const oasp_bytecode *bytecode) {
    printf("\nClasses (%zu)\n", bytecode->class_count);
    for (size_t i = 0; i < bytecode->class_count; i++) {
        oasp_class_bytecode class_def;
        if (!oasp_bytecode_class_at(bytecode, i, &class_def))
            return false;
        printf("  [%zu] name=", i);
        oasp_tool_print_escaped(stdout, class_def.name);
        printf(" range=[%zu,%zu) routines=[%zu,%zu) error-aware=%s\n", class_def.entry, class_def.limit,
               class_def.routine_offset, class_def.routine_offset + class_def.routine_count,
               class_def.error_aware ? "yes" : "no");
    }
    return true;
}

static void print_operand_annotation(const oasp_bytecode *bytecode, const oasp_instruction *instruction) {
    oasp_slice text;
    oasp_value value;
    switch ((oasp_opcode)instruction->opcode) {
    case OASP_BC_INCLUDE:
    case OASP_BC_PUSH_LITERAL:
    case OASP_BC_RECORDSET_FIELD:
    case OASP_BC_WRITE_RS_FIELD:
    case OASP_BC_WRITE_HTMLENCODE_RS_FIELD:
    case OASP_BC_WRITE_INT_RS_FIELD:
    case OASP_BC_WRITE_RENDER_RS_FIELD:
    case OASP_BC_WRITE_CONST:
        if (oasp_bytecode_constant_at(bytecode, instruction->operand, &value)) {
            fputs("  # ", stdout);
            oasp_tool_print_value(stdout, &value, OASP_TOOL_VALUE_TYPED);
        }
        break;
    case OASP_BC_PUSH_GLOBAL_SLOT:
    case OASP_BC_ASSIGN_GLOBAL_SLOT:
    case OASP_BC_APPEND_GLOBAL:
    case OASP_BC_APPEND_GLOBAL_N:
        if (oasp_bytecode_global_slot_at(bytecode, instruction->operand, &text)) {
            fputs("  # global ", stdout);
            oasp_tool_print_escaped(stdout, text);
        }
        break;
    case OASP_BC_PUSH_NAME:
    case OASP_BC_NEW:
    case OASP_BC_GET_MEMBER:
    case OASP_BC_CALL_NAME:
    case OASP_BC_CALL_MEMBER:
    case OASP_BC_ASSIGN_NAME:
    case OASP_BC_ASSIGN_MEMBER:
    case OASP_BC_ASSIGN_INDEX_NAME:
    case OASP_BC_ASSIGN_INDEX_MEMBER:
    case OASP_BC_ASSIGN_COOKIE_ATTRIBUTE:
    case OASP_BC_DECLARE:
    case OASP_BC_REDIM:
    case OASP_BC_DECLARE_CONST:
    case OASP_BC_ERASE:
    case OASP_BC_WRITE_MEMBER:
    case OASP_BC_LOAD_MEMBER_CALL_MEMBER:
        if (oasp_bytecode_name_at(bytecode, instruction->operand, &text)) {
            fputs("  # ", stdout);
            oasp_tool_print_escaped(stdout, text);
        }
        break;
    default:
        break;
    }
}

static void print_source_text(oasp_slice source) {
    for (size_t i = 0; i < source.length; i++) {
        unsigned char byte = (unsigned char)source.data[i];
        if (byte == '\t' || (byte >= 0x20 && byte <= 0x7e))
            putchar(byte);
        else
            printf("\\x%02x", byte);
    }
}

static void print_symbol_text(oasp_slice symbol) {
    for (size_t i = 0; i < symbol.length; i++) {
        unsigned char byte = (unsigned char)symbol.data[i];
        putchar(isalnum(byte) || byte == '_' || byte == '$' || byte == '.' ? byte : '_');
    }
}

static bool instruction_regions_init(const oasp_bytecode *bytecode, disassembly_region **result) {
    *result = NULL;
    if (!bytecode->length)
        return true;
    if (bytecode->length > SIZE_MAX / sizeof(**result))
        return false;
    disassembly_region *regions = calloc(bytecode->length, sizeof(*regions));
    if (!regions)
        return false;
    for (size_t pc = 0; pc < bytecode->length; pc++) {
        regions[pc].kind = DISASSEMBLY_REGION_ROOT;
        regions[pc].limit = bytecode->length;
    }
    for (size_t i = 0; i < bytecode->class_count; i++) {
        oasp_class_bytecode class_def;
        if (!oasp_bytecode_class_at(bytecode, i, &class_def) ||
            class_def.entry > class_def.limit || class_def.limit > bytecode->length)
            goto fail;
        for (size_t pc = class_def.entry; pc < class_def.limit; pc++) {
            regions[pc].kind = DISASSEMBLY_REGION_CLASS;
            regions[pc].entry = class_def.entry;
            regions[pc].limit = class_def.limit;
            regions[pc].name = class_def.name;
        }
    }
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        oasp_routine_bytecode routine;
        if (!oasp_bytecode_routine_at(bytecode, i, &routine) ||
            routine.entry > routine.limit || routine.limit > bytecode->length)
            goto fail;
        oasp_slice owner = {0};
        if (routine.class_index != SIZE_MAX) {
            oasp_class_bytecode class_def;
            if (!oasp_bytecode_class_at(bytecode, routine.class_index, &class_def))
                goto fail;
            owner = class_def.name;
        }
        for (size_t pc = routine.entry; pc < routine.limit; pc++) {
            regions[pc].kind = DISASSEMBLY_REGION_ROUTINE;
            regions[pc].entry = routine.entry;
            regions[pc].limit = routine.limit;
            regions[pc].owner = owner;
            regions[pc].name = routine.name;
        }
    }
    *result = regions;
    return true;
fail:
    free(regions);
    return false;
}

static bool same_instruction_region(const disassembly_region *left, const disassembly_region *right) {
    return left->kind == right->kind && left->entry == right->entry && left->limit == right->limit &&
           left->name.data == right->name.data && left->name.length == right->name.length;
}

static void print_region_symbol(const disassembly_region *region) {
    if (region->kind == DISASSEMBLY_REGION_ROOT) {
        fputs("__asp_page", stdout);
        return;
    }
    if (region->owner.length) {
        print_symbol_text(region->owner);
        fputs("::", stdout);
    }
    print_symbol_text(region->name);
    if (region->kind == DISASSEMBLY_REGION_CLASS)
        fputs("::__body", stdout);
}

static void print_mnemonic(const char *name) {
    for (; *name; name++)
        putchar(tolower((unsigned char)*name));
}

static bool opcode_has_constant_operand(oasp_opcode opcode) {
    return opcode == OASP_BC_INCLUDE || opcode == OASP_BC_PUSH_LITERAL ||
           opcode == OASP_BC_RECORDSET_FIELD || opcode == OASP_BC_WRITE_RS_FIELD ||
           opcode == OASP_BC_WRITE_HTMLENCODE_RS_FIELD || opcode == OASP_BC_WRITE_INT_RS_FIELD ||
           opcode == OASP_BC_WRITE_RENDER_RS_FIELD ||
           opcode == OASP_BC_WRITE_CONST;
}

static bool opcode_has_name_operand(oasp_opcode opcode) {
    return opcode == OASP_BC_PUSH_NAME || opcode == OASP_BC_NEW || opcode == OASP_BC_GET_MEMBER ||
           opcode == OASP_BC_CALL_NAME || opcode == OASP_BC_CALL_MEMBER ||
           opcode == OASP_BC_ASSIGN_NAME || opcode == OASP_BC_ASSIGN_MEMBER ||
           opcode == OASP_BC_ASSIGN_INDEX_NAME || opcode == OASP_BC_ASSIGN_INDEX_MEMBER ||
           opcode == OASP_BC_ASSIGN_COOKIE_ATTRIBUTE || opcode == OASP_BC_DECLARE ||
           opcode == OASP_BC_REDIM || opcode == OASP_BC_DECLARE_CONST || opcode == OASP_BC_ERASE ||
           opcode == OASP_BC_WRITE_MEMBER || opcode == OASP_BC_LOAD_MEMBER_CALL_MEMBER;
}

static bool opcode_has_global_operand(oasp_opcode opcode) {
    return opcode == OASP_BC_PUSH_GLOBAL_SLOT || opcode == OASP_BC_ASSIGN_GLOBAL_SLOT ||
           opcode == OASP_BC_APPEND_GLOBAL || opcode == OASP_BC_APPEND_GLOBAL_N;
}

static bool opcode_has_local_operand(oasp_opcode opcode) {
    return opcode == OASP_BC_PUSH_LOCAL_SLOT || opcode == OASP_BC_ASSIGN_LOCAL_SLOT ||
           opcode == OASP_BC_APPEND_LOCAL || opcode == OASP_BC_APPEND_LOCAL_N ||
           opcode == OASP_BC_WRITE_LOCAL || opcode == OASP_BC_LOAD_LOCAL_CALL_BUILTIN ||
           opcode == OASP_BC_LOCAL_CONST_COMPARE_JUMP || opcode == OASP_BC_LOCAL_ADD_CONST_STORE;
}

static bool opcode_has_jump_target(oasp_opcode opcode) {
    return opcode == OASP_BC_JUMP_IF_FALSE || opcode == OASP_BC_JUMP_IF_TRUE ||
           opcode == OASP_BC_JUMP || opcode == OASP_BC_LOOP_BACK ||
           opcode == OASP_BC_FOR_INIT || opcode == OASP_BC_FOR_NEXT ||
           opcode == OASP_BC_FOR_EACH_INIT || opcode == OASP_BC_FOR_EACH_NEXT ||
           opcode == OASP_BC_JUMP_IF_EMPTY || opcode == OASP_BC_LOCAL_CONST_COMPARE_JUMP;
}

static bool opcode_has_display_operand(oasp_opcode opcode) {
    return opcode_has_jump_target(opcode) || opcode_has_constant_operand(opcode) ||
           opcode_has_name_operand(opcode) || opcode_has_global_operand(opcode) ||
           opcode_has_local_operand(opcode);
}

static bool print_target_symbol(const oasp_bytecode *bytecode, const disassembly_region *regions, size_t instruction_offset, uint32_t target) {
    if (target >= bytecode->length)
        return false;
    const disassembly_region *region = &regions[target];
    size_t address = instruction_offset + (size_t)target * sizeof(oasp_instruction);
    size_t symbol_offset = ((size_t)target - region->entry) * sizeof(oasp_instruction);
    printf("0x%016" PRIxMAX " <", (uintmax_t)address);
    print_region_symbol(region);
    printf("+%zu>", symbol_offset);
    return true;
}

static bool print_instruction_operand(const oasp_bytecode *bytecode, const disassembly_region *regions, const oasp_instruction *instruction, size_t instruction_offset) {
    oasp_opcode opcode = (oasp_opcode)instruction->opcode;
    if (opcode_has_jump_target(opcode))
        return print_target_symbol(bytecode, regions, instruction_offset, instruction->target);
    if (opcode_has_constant_operand(opcode)) {
        printf("$const[%" PRIu32 "]", instruction->operand);
    } else if (opcode_has_name_operand(opcode)) {
        printf("$name[%" PRIu32 "]", instruction->operand);
    } else if (opcode_has_global_operand(opcode)) {
        printf("$global[%" PRIu32 "]", instruction->operand);
    } else if (opcode_has_local_operand(opcode)) {
        printf("%%local[%" PRIu32 "]", instruction->operand);
    }
    return true;
}

static bool print_instructions(const oasp_bytecode *bytecode, const debug_source_index *debug_source) {
    if (!bytecode->image_base || (const unsigned char *)(const void *)bytecode->instructions < bytecode->image_base)
        return false;
    disassembly_region *regions = NULL;
    if (!instruction_regions_init(bytecode, &regions))
        return false;
    size_t instruction_offset =
        (size_t)((const unsigned char *)(const void *)bytecode->instructions - bytecode->image_base);
    printf("\nDisassembly of section .oasp_bytecode:\n");
    disassembly_region previous_region;
    memset(&previous_region, 0, sizeof(previous_region));
    bool have_region = false;
    uint32_t previous_line = UINT32_MAX;
    for (size_t i = 0; i < bytecode->length; i++) {
        const oasp_instruction *instruction = oasp_bytecode_instruction_at(bytecode, i);
        if (!instruction || !oasp_tool_opcode_valid(instruction->opcode))
            goto fail;
        const disassembly_region *region = &regions[i];
        if (!have_region || !same_instruction_region(region, &previous_region)) {
            if (have_region)
                puts("End of assembler dump.\n");
            fputs("Dump of assembler code for function ", stdout);
            print_region_symbol(region);
            puts(":");
            previous_region = *region;
            have_region = true;
            previous_line = UINT32_MAX;
        }
        oasp_slice source_line;
        if (instruction->line != previous_line &&
            debug_source_line(debug_source, instruction->line, &source_line)) {
            printf("%5" PRIu32 "\t", instruction->line);
            print_source_text(source_line);
            putchar('\n');
            previous_line = instruction->line;
        }
        size_t address = instruction_offset + i * sizeof(*instruction);
        size_t symbol_offset = (i - region->entry) * sizeof(*instruction);
        printf("   0x%016" PRIxMAX " <", (uintmax_t)address);
        print_region_symbol(region);
        printf("+%zu>:\t", symbol_offset);
        oasp_tool_print_instruction_bytes(stdout, instruction);
        fputs("\t", stdout);
        print_mnemonic(oasp_tool_opcode_name(instruction->opcode));
        if (opcode_has_display_operand((oasp_opcode)instruction->opcode)) {
            putchar('\t');
            if (!print_instruction_operand(bytecode, regions, instruction, instruction_offset))
                goto fail;
        }
        print_operand_annotation(bytecode, instruction);
        fputs("\t; ", stdout);
        oasp_tool_print_instruction_fields(stdout, instruction);
        putchar('\n');
    }
    if (have_region)
        puts("End of assembler dump.");
    free(regions);
    return true;
fail:
    free(regions);
    return false;
}

static bool disassemble(const char *file_path, size_t file_size, const oasp_module_image *image) {
    const oasp_bytecode *bytecode = &image->bytecode;
    debug_source_index debug_source;
    if (!debug_source_index_init(image, &debug_source))
        return false;
    oasp_slice source_path;
    if (!oasp_module_image_path(image, &source_path)) {
        debug_source_index_dispose(&debug_source);
        return false;
    }
    puts("OpenASP OAC disassembly");
    printf("File: %s\nSize: %zu bytes\nSource: ", file_path, file_size);
    oasp_tool_print_escaped(stdout, source_path);
    printf("\nInstructions: %zu\nMax stack: %zu\nLoops: %zu\nDebug info: %s\nRoot error-aware: %s\nState effects: 0x%08" PRIx32 "\n",
           bytecode->length, bytecode->max_stack, bytecode->loop_count,
           image->has_debug_info ? "present" : "unavailable",
           bytecode->root_error_aware ? "yes" : "no", bytecode->state_effects);
    bool success = print_dependencies(image) && print_modules(image) && print_names(bytecode) &&
                   print_constants(bytecode) && print_globals(bytecode) && print_routines(bytecode) &&
                   print_classes(bytecode) && print_instructions(bytecode, &debug_source);
    debug_source_index_dispose(&debug_source);
    return success;
}

static void print_usage(FILE *output) {
    fputs("Usage: openasp-dis <file.oac>\n", output);
}

int main(int argc, char **argv) {
    oasp_print_version(stderr);
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        print_usage(stdout);
        return 0;
    }
    if (argc != 2) {
        print_usage(stderr);
        return 2;
    }

    unsigned char *data = NULL;
    size_t size = 0;
    char error[512];
    if (!oasp_tool_read_file(argv[1], &data, &size, error, sizeof(error))) {
        fprintf(stderr, "openasp-dis: %s\n", error);
        return 1;
    }
    oasp_module_image image;
    if (!oasp_module_image_decode(NULL, data, size, &image)) {
        fprintf(stderr, "openasp-dis: '%s' is not a valid OAC image (format, ABI, checksum, or bytecode validation failed)\n",
                argv[1]);
        free(data);
        return 1;
    }
    bool success = disassemble(argv[1], size, &image);
    if (!success)
        fprintf(stderr, "openasp-dis: validated image access failed for '%s'\n", argv[1]);
    oasp_module_image_dispose(&image);
    free(data);
    if (ferror(stdout)) {
        fprintf(stderr, "openasp-dis: cannot write output: %s\n", strerror(errno));
        return 1;
    }
    return success ? 0 : 1;
}
