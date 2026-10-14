// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Versioned binary codec for Session/Application values. The format stores
 * explicit tags, lengths, and nesting bounds and rejects truncation, overflow,
 * unsupported native objects, or trailing bytes. Encoding returns heap-owned
 * bytes; decoding creates values owned by `runtime`.
 */

#ifndef OPENASP_STATE_BINARY_H
#define OPENASP_STATE_BINARY_H

#include "runtime.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Serializes a linked binding store into newly allocated `*data` and writes its
 * exact byte count to `*length`. Callers free the buffer. Failure leaves no
 * partially owned output.
 */
bool oasp_state_binary_encode_bindings(const oasp_binding *bindings, char **data, size_t *length);

/* Decodes one complete binding image into runtime-managed `*bindings`. */
bool oasp_state_binary_decode_bindings(oasp_runtime *runtime, const void *data, size_t length, oasp_binding **bindings);

/* Value equivalents used for nested persistent component payloads. */
bool oasp_state_binary_encode_value(const oasp_value *value, char **data, size_t *length);
bool oasp_state_binary_decode_value(oasp_runtime *runtime, const void *data, size_t length, oasp_value *value);

#endif
