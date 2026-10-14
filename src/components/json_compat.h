// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/components/json_compat.h`): Native component boundary. It maps
 * ASP-compatible object operations onto C libraries while preserving runtime error semantics,
 * ownership, and garbage-collector tracing. Result and error variables are initialized before
 * crossing the boundary.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OASP_JSON_COMPAT_H
#define OASP_JSON_COMPAT_H

struct json_object;

void oasp_json_object_put(struct json_object *object);

#endif
