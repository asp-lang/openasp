// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_COMPONENT_OBJECT_INTERNAL_H
#define OPENASP_COMPONENT_OBJECT_INTERNAL_H

#include "component.h"

oasp_value oasp_component_object_value(oasp_object *object);
oasp_object *oasp_component_object(oasp_runtime *runtime, oasp_component_kind kind, const char *name, void *native);
void oasp_component_missing(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line);

#endif
