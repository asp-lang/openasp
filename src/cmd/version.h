// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/cmd/version.h`): Command and configuration layer. It translates
 * operator input into validated engine/server state, owns copied option strings, and reports
 * startup failures with actionable diagnostics before transferring control to the runtime.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_VERSION_H
#define OPENASP_VERSION_H

#include <stdio.h>

#define OPENASP_VERSION_MAJOR 0
#define OPENASP_VERSION_MINOR 2
#define OPENASP_VERSION_PATCH 0
#define OPENASP_VERSION "0.2.0"

#if defined(__APPLE__)
#define OPENASP_PLATFORM_OS "MacOS"
#elif defined(__linux__)
#define OPENASP_PLATFORM_OS "Linux"
#elif defined(_WIN32)
#define OPENASP_PLATFORM_OS "Windows"
#else
#define OPENASP_PLATFORM_OS "Unknown-OS"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define OPENASP_PLATFORM_ARCH "ARM64"
#elif defined(__x86_64__) || defined(_M_X64)
#define OPENASP_PLATFORM_ARCH "X86-64"
#elif defined(__i386__) || defined(_M_IX86)
#define OPENASP_PLATFORM_ARCH "X86-32"
#else
#define OPENASP_PLATFORM_ARCH "Unknown-Arch"
#endif

#define OPENASP_PLATFORM OPENASP_PLATFORM_OS " " OPENASP_PLATFORM_ARCH
#define OPENASP_BUILD_DATE __DATE__ " " __TIME__
#define OPENASP_COPYRIGHT "Copyright (C) 2026 https://asp-lang.org"

void oasp_print_version(FILE *stream);

#endif
