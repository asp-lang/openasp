// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_AI_COMPONENT_METADATA_H
#define OPENASP_AI_COMPONENT_METADATA_H

#include <openasp/ext_api.h>

int oasp_ai_metadata_encode(oasp_ext_call_context *context, const oasp_ext_value_api *values, const oasp_ext_object_api *objects, const oasp_ext_value *value, void **data,
                            size_t *size, oasp_ext_error *error);
oasp_ext_value *oasp_ai_metadata_decode(oasp_ext_call_context *context, const oasp_ext_value_api *values, const void *data, size_t size, oasp_ext_error *error);
int oasp_ai_metadata_is_encoded(const void *data, size_t size);

#endif
