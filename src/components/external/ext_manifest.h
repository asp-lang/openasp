// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_EXT_MANIFEST_INTERNAL_H
#define OPENASP_EXT_MANIFEST_INTERNAL_H

#include "openasp/ext_manifest.h"

#include <stdbool.h>
#include <stddef.h>

bool oasp_external_manifest_validate_library(const oasp_external_module_config *module, bool require_sha256, char *error, size_t error_size);

#endif
