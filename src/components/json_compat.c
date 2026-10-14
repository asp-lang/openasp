// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/components/json_compat.c`): Native component boundary. It maps
 * ASP-compatible object operations onto C libraries while preserving runtime error semantics,
 * ownership, and garbage-collector tracing. Result and error variables are initialized before
 * crossing the boundary.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "json_compat.h"

#include <json-c/json.h>

static void oasp_json_clear(struct json_object *object) {
    if (!object)
        return;
    /* json-c 0.19 can retain nested containers on macOS; empty them bottom-up before the final put. */
    if (json_object_is_type(object, json_type_object)) {
        json_object_object_foreach(object, key, child) {
            oasp_json_clear(child);
            json_object_object_del(object, key);
        }
        return;
    }
    if (json_object_is_type(object, json_type_array)) {
        size_t count = json_object_array_length(object);
        while (count) {
            size_t index = --count;
            oasp_json_clear(json_object_array_get_idx(object, index));
            json_object_array_del_idx(object, index, 1);
        }
    }
}

void oasp_json_object_put(struct json_object *object) {
    if (!object)
        return;
    oasp_json_clear(object);
    (void)json_object_put(object);
}
