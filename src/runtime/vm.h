// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Validated-bytecode execution entry points. `runtime` owns all transient
 * values; `bytecode` is borrowed and must remain alive for the call. Range
 * limits are half-open instruction indexes and must describe a validated
 * routine or root program.
 */

#ifndef OPENASP_VM_H
#define OPENASP_VM_H

#include "bytecode.h"
#include "runtime.h"

/* Executes the complete root program with normal include/error semantics. */
bool oasp_vm_execute(oasp_runtime *runtime, const oasp_bytecode *bytecode);

/* Executes a complete module inline without replacing registered definitions. */
bool oasp_vm_execute_inline(oasp_runtime *runtime, const oasp_bytecode *bytecode);

/* Executes [entry, limit), deriving error-awareness from bytecode metadata. */
bool oasp_vm_execute_range(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit);
bool oasp_vm_execute_range_inline(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit);

/*
 * Explicit-mode variants are used by routine dispatch after it determines
 * whether each instruction needs a Resume Next recovery target.
 */
bool oasp_vm_execute_range_mode(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit, bool error_aware);
bool oasp_vm_execute_range_inline_mode(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t entry, size_t limit, bool error_aware);

#endif
