# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c11 -Wall -Wextra -Werror -Wpedantic
PROFILE ?= full
AI ?= 0
UNAME_S := $(shell uname -s)
PLATFORM_CPPFLAGS := $(if $(filter Linux,$(UNAME_S)),-D_GNU_SOURCE,)
DARWIN_SDK_PATH := $(if $(filter Darwin,$(UNAME_S)),$(shell xcrun --show-sdk-path 2>/dev/null),)
ICONV_LIBS := $(if $(DARWIN_SDK_PATH),$(DARWIN_SDK_PATH)/usr/lib/libiconv.tbd,)
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8 2>/dev/null)
PCRE2_LIBS := $(shell pkg-config --libs libpcre2-8 2>/dev/null)
XML_CFLAGS := $(shell pkg-config --cflags libxml-2.0 2>/dev/null)
XML_LIBS := $(shell pkg-config --libs libxml-2.0 2>/dev/null)
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS := $(shell pkg-config --libs sqlite3 2>/dev/null)
JSON_CFLAGS := $(shell pkg-config --cflags json-c 2>/dev/null)
JSON_LIBS := $(shell pkg-config --libs json-c 2>/dev/null)
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null)
ASAN_LEAKS := $(if $(filter Darwin,$(UNAME_S)),0,1)
DEPENDENCY_INSPECTOR := $(if $(filter Darwin,$(UNAME_S)),otool -L,ldd)
SOURCE_INCLUDE_DIRS = src src/log src/memory src/compiler src/runtime src/engine src/web src/storage src/components \
	src/components/core src/components/external src/components/builtin src/components/builtin/ado src/components/mdb
SOURCE_CPPFLAGS = $(PLATFORM_CPPFLAGS) $(addprefix -I,$(SOURCE_INCLUDE_DIRS))
TOOL_CPPFLAGS = -Itools/common
TOOL_COMMON_SOURCES = tools/common/bytecode_format.c tools/common/file.c
TOOL_COMMON_HEADERS = $(wildcard tools/common/*.h)
COMMAND_SOURCE = src/cmd/version.c
DIS_SOURCE = tools/openasp-dis/main.c
DBG_SOURCES = tools/openasp-dbg/main.c tools/openasp-dbg/debugger.c tools/openasp-dbg/command.c \
	tools/openasp-dbg/breakpoint.c tools/openasp-dbg/source.c tools/openasp-dbg/format.c
MARIADB_CFLAGS := $(shell pkg-config --cflags libmariadb 2>/dev/null)
MARIADB_LIBS := $(shell pkg-config --libs libmariadb 2>/dev/null)
MYSQL_CONFIG_LIBDIR := $(shell mysql_config --variable=pkglibdir 2>/dev/null)
MYSQL_CONFIG_LIBRARY := $(firstword $(wildcard $(MYSQL_CONFIG_LIBDIR)/libmysqlclient.dylib $(MYSQL_CONFIG_LIBDIR)/libmysqlclient.so))
MYSQL_CFLAGS ?= $(if $(MARIADB_LIBS),$(MARIADB_CFLAGS),$(if $(MYSQL_CONFIG_LIBRARY),$(shell mysql_config --cflags 2>/dev/null),$(shell pkg-config --cflags mysqlclient 2>/dev/null)))
MYSQL_LIBS ?= $(if $(MARIADB_LIBS),$(MARIADB_LIBS),$(if $(MYSQL_CONFIG_LIBRARY),$(MYSQL_CONFIG_LIBRARY) -Xlinker -rpath -Xlinker $(MYSQL_CONFIG_LIBDIR),$(shell pkg-config --libs mysqlclient 2>/dev/null)))
MDB_CFLAGS := $(shell pkg-config --cflags libmdb 2>/dev/null)
MDB_LIBS := $(shell pkg-config --libs libmdb 2>/dev/null) -lmdbsql
CORE_SOURCES = src/log/log.c src/memory/memory.c src/memory/arena.c src/memory/allocator.c \
	src/runtime/internal.c src/runtime/value.c src/runtime/runtime.c src/runtime/vm.c src/runtime/debugger.c \
	src/compiler/lexer.c src/compiler/parser.c src/compiler/symbol.c src/compiler/bytecode.c \
	src/compiler/ast_image.c src/compiler/module_image.c src/compiler/preprocessor.c \
	src/storage/shared_cache.c src/storage/state_binary.c src/storage/state_store.c \
	src/components/json_compat.c src/web/request.c src/engine/engine.c src/engine/interactive.c

ifeq ($(OASP_CPU_PROFILE),1)
CORE_SOURCES += src/profile/cpu_profile.c
endif

ifeq ($(PROFILE),full)
CPPFLAGS = -Iinclude $(SOURCE_CPPFLAGS) $(PCRE2_CFLAGS) $(XML_CFLAGS) $(CURL_CFLAGS) $(SQLITE_CFLAGS) $(JSON_CFLAGS) $(OPENSSL_CFLAGS) $(MYSQL_CFLAGS) $(MDB_CFLAGS)
LDLIBS = -lm $(ICONV_LIBS) $(PCRE2_LIBS) $(XML_LIBS) $(CURL_LIBS) $(SQLITE_LIBS) $(JSON_LIBS) $(OPENSSL_LIBS) $(MYSQL_LIBS) $(MDB_LIBS) $(if $(filter Linux,$(UNAME_S)),-ldl)
COMPONENT_SOURCES = src/components/mdb/mdb.c src/components/mdb/mdb_write_lock.c src/components/mdb/mdb_write_ddl.c \
	src/components/mdb/mdb_write.c src/components/external/ext_loader.c src/components/external/ext_manifest.c \
	src/components/external/ext_isolated_proxy.c src/components/external/ext_host_api.c \
	src/components/external/ext_value.c src/components/external/ext_object.c src/components/core/component_dispatch.c \
	src/components/core/component_registry.c src/components/core/component_object.c src/components/core/component_context.c \
src/components/core/component_cleanup.c src/components/core/component_string.c src/components/core/component_factory.c \
src/components/core/component_adapter.c \
src/components/builtin/dictionary.c src/components/builtin/regexp.c src/components/builtin/stream.c src/components/builtin/fso.c \
src/components/builtin/xml.c src/components/builtin/json.c src/components/builtin/http.c src/components/builtin/http_multi.c \
	src/components/builtin/openai_stream.c src/components/builtin/openai_stream_sse.c src/components/builtin/process.c \
	src/components/builtin/socket.c src/components/builtin/websocket.c src/components/builtin/redis.c src/components/builtin/redis_multi.c \
	src/components/builtin/redis_multi_event.c src/components/builtin/memcache.c src/components/builtin/memcache_multi.c \
	src/components/builtin/memcache_multi_event.c \
	src/components/builtin/ado/ado.c src/components/component.c
BUILD ?= build
else ifeq ($(PROFILE),minimal)
CPPFLAGS = -Iinclude $(SOURCE_CPPFLAGS) -DOASP_PROFILE_MINIMAL=1 $(SQLITE_CFLAGS) $(JSON_CFLAGS)
LDLIBS = -lm $(ICONV_LIBS) $(SQLITE_LIBS) $(JSON_LIBS)
COMPONENT_SOURCES = src/components/component_minimal.c
BUILD ?= build/minimal
else
$(error unsupported PROFILE '$(PROFILE)'; expected full or minimal)
endif

SOURCES = $(CORE_SOURCES) $(COMPONENT_SOURCES)
HEADERS = $(wildcard include/*.h src/cmd/*.h $(addsuffix /*.h,$(SOURCE_INCLUDE_DIRS)))
BIN_DIR = $(BUILD)/bin
TEST_BUILD = $(BUILD)/tests
TEST_BIN_DIR = $(TEST_BUILD)/bin
TEST_HARNESS = tests/unit/test_harness.c
export PYTHONPYCACHEPREFIX := $(abspath $(TEST_BUILD)/pycache)

.PHONY: all clean format-check test test-all test-unit test-integration test-conformance test-e2e test-stress \
	test-benchmark external-test full-conformance asan fixture-diff minimal minimal-test profile-info \
	zblog-benchmark fastcgi-load ai ai-test

all: $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm $(BIN_DIR)/openasp-cli $(BIN_DIR)/openasp-dis $(BIN_DIR)/openasp-dbg \
	$(if $(filter full,$(PROFILE)),$(BIN_DIR)/openasp-component-host) $(if $(filter 1,$(AI)),ai)

ai:
	$(MAKE) -C extensions/openasp-ai all

ai-test:
	$(MAKE) -C extensions/openasp-ai test

format-check:
	python3 tests/unit/test_format_c_parameters.py
	python3 scripts/format_c_parameters.py .

minimal:
	$(MAKE) PROFILE=minimal all

minimal-test:
	$(MAKE) PROFILE=minimal all
	test "$$(build/minimal/bin/openasp tests/fixtures/common/minimal_profile.asp --root tests/fixtures/common)" = "42"
	python3 tests/integration/cli/test_openasp_oac.py build/minimal/bin/openasp build/minimal/bin/openasp-dis
	python3 tests/integration/debugger/test_openasp_dbg.py build/minimal/bin/openasp build/minimal/bin/openasp-dbg
	! $(DEPENDENCY_INSPECTOR) build/minimal/bin/openasp-fpm | grep -Eq 'pcre2|xml2|curl|ssl|crypto|mysql|mariadb|mdb'

profile-info:
	@printf 'PROFILE=%s\nBUILD=%s\nBIN_DIR=%s\nCOMPONENT_SOURCES=%s\n' "$(PROFILE)" "$(BUILD)" "$(BIN_DIR)" "$(COMPONENT_SOURCES)"

$(BUILD):
	mkdir -p $(BUILD)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(TEST_BUILD):
	mkdir -p $(TEST_BUILD)

$(TEST_BIN_DIR):
	mkdir -p $(TEST_BIN_DIR)

$(BIN_DIR)/openasp: $(SOURCES) $(COMMAND_SOURCE) src/cmd/fpm_config.c src/cmd/openasp.c $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(SOURCES) $(COMMAND_SOURCE) src/cmd/fpm_config.c src/cmd/openasp.c $(LDLIBS) -o $@

$(BIN_DIR)/openasp-fpm: $(SOURCES) $(COMMAND_SOURCE) src/web/fastcgi.c src/cmd/fpm_config.c src/cmd/openasp-fpm.c $(DBG_SOURCES) $(TOOL_COMMON_SOURCES) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(TOOL_CPPFLAGS) -Itools/openasp-dbg $(SOURCES) $(COMMAND_SOURCE) src/web/fastcgi.c src/cmd/fpm_config.c src/cmd/openasp-fpm.c $(TOOL_COMMON_SOURCES) tools/openasp-dbg/debugger.c tools/openasp-dbg/command.c tools/openasp-dbg/breakpoint.c tools/openasp-dbg/source.c tools/openasp-dbg/format.c $(LDLIBS) -o $@

$(BIN_DIR)/openasp-cli: $(SOURCES) $(COMMAND_SOURCE) src/cmd/openasp-cli.c src/cmd/line_editor.c src/cmd/line_editor.h src/cmd/cli_shell.c src/cmd/cli_shell.h $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(SOURCES) $(COMMAND_SOURCE) src/cmd/openasp-cli.c src/cmd/line_editor.c src/cmd/cli_shell.c $(LDLIBS) -o $@

$(BIN_DIR)/openasp-dis: $(SOURCES) $(COMMAND_SOURCE) $(TOOL_COMMON_SOURCES) $(DIS_SOURCE) $(HEADERS) $(TOOL_COMMON_HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) -Isrc/cmd $(TOOL_CPPFLAGS) $(SOURCES) $(COMMAND_SOURCE) $(TOOL_COMMON_SOURCES) $(DIS_SOURCE) $(LDLIBS) -o $@

$(BIN_DIR)/openasp-dbg: $(SOURCES) $(COMMAND_SOURCE) $(TOOL_COMMON_SOURCES) $(DBG_SOURCES) $(HEADERS) $(TOOL_COMMON_HEADERS) tools/openasp-dbg/dbg.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) -Isrc/cmd $(TOOL_CPPFLAGS) -Itools/openasp-dbg $(SOURCES) $(COMMAND_SOURCE) $(TOOL_COMMON_SOURCES) $(DBG_SOURCES) $(LDLIBS) -o $@

$(BIN_DIR)/openasp-component-host: $(SOURCES) src/cmd/openasp-component-host.c $(HEADERS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(SOURCES) src/cmd/openasp-component-host.c $(LDLIBS) -o $@

$(TEST_BIN_DIR)/fastcgi-load: tests/integration/fastcgi/fastcgi_load.c | $(TEST_BIN_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) tests/integration/fastcgi/fastcgi_load.c -pthread -o $@

fastcgi-load: $(TEST_BIN_DIR)/fastcgi-load

$(TEST_BUILD)/test-allocator: tests/unit/test_allocator.c src/memory/memory.c src/memory/arena.c src/memory/allocator.c | $(TEST_BUILD)
	$(CC) $(WARNINGS) $(CPPFLAGS) tests/unit/test_allocator.c src/memory/memory.c src/memory/arena.c src/memory/allocator.c $(LDLIBS) -o $@

$(TEST_BUILD)/test-%: tests/unit/test_%.c $(TEST_HARNESS) $(SOURCES) | $(TEST_BUILD)
	$(CC) $(WARNINGS) $(CPPFLAGS) -Itests/unit $< $(TEST_HARNESS) $(SOURCES) $(LDLIBS) -o $@

$(TEST_BUILD)/test-shared-cache: tests/unit/test_shared_cache.c $(TEST_HARNESS) $(SOURCES) | $(TEST_BUILD)
	$(CC) $(WARNINGS) $(CPPFLAGS) -Itests/unit $< $(TEST_HARNESS) $(SOURCES) $(LDLIBS) -o $@

$(TEST_BUILD)/test-state-store: tests/unit/test_state_store.c $(TEST_HARNESS) $(SOURCES) | $(TEST_BUILD)
	$(CC) $(WARNINGS) $(CPPFLAGS) -Itests/unit $< $(TEST_HARNESS) $(SOURCES) $(LDLIBS) -o $@

test-unit: $(TEST_BUILD)/test-allocator $(TEST_BUILD)/test-compiler $(TEST_BUILD)/test-runtime \
	$(TEST_BUILD)/test-components $(TEST_BUILD)/test-web $(TEST_BUILD)/test-shared-cache \
	$(TEST_BUILD)/test-state-store
	$(TEST_BUILD)/test-allocator
	$(TEST_BUILD)/test-compiler
	$(TEST_BUILD)/test-runtime
	$(TEST_BUILD)/test-components
	$(TEST_BUILD)/test-web
	$(TEST_BUILD)/test-shared-cache
	$(TEST_BUILD)/test-state-store
	python3 tests/unit/test_differential_fixtures.py

test-integration: $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm $(BIN_DIR)/openasp-cli $(BIN_DIR)/openasp-dis $(BIN_DIR)/openasp-dbg $(BIN_DIR)/openasp-component-host | $(TEST_BUILD)
	test "$$($(BIN_DIR)/openasp-dis --help)" = "Usage: openasp-dis <file.oac>"
	! $(BIN_DIR)/openasp-dis
	printf 'not an oac image' > $(TEST_BUILD)/invalid.oac
	! $(BIN_DIR)/openasp-dis $(TEST_BUILD)/invalid.oac
	rm -f $(TEST_BUILD)/invalid.oac
	python3 tests/integration/cli/test_command_logos.py $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm $(BIN_DIR)/openasp-cli $(BIN_DIR)/openasp-dis $(BIN_DIR)/openasp-dbg
	$(BIN_DIR)/openasp tests/fixtures/common/service_gc.asp --root tests/fixtures/common --gc-young-size=16K --gc-old-threshold=32K --gc-heap-limit=8M --gc-diagnostics > $(TEST_BUILD)/default-service.out 2> $(TEST_BUILD)/default-service.err
	test "$$(cat $(TEST_BUILD)/default-service.out)" = "service-gc-ok"
	grep -q '^OpenASP GC:' $(TEST_BUILD)/default-service.err
	rm -f $(TEST_BUILD)/default-service.out $(TEST_BUILD)/default-service.err
	test "$$($(BIN_DIR)/openasp tests/fixtures/common/loop_unlimited.asp --root tests/fixtures/common)" = "loop-unlimited"
	python3 tests/integration/cli/test_openasp_oac.py $(BIN_DIR)/openasp $(BIN_DIR)/openasp-dis
	python3 tests/integration/cli/test_openasp_cli_editor.py $(BIN_DIR)/openasp-cli
	python3 tests/integration/cli/test_openasp_cli_shell.py $(BIN_DIR)/openasp-cli
	python3 tests/integration/debugger/test_openasp_dbg.py $(BIN_DIR)/openasp $(BIN_DIR)/openasp-dbg
	test "$$(printf 'value = 40\n? value + 2\nIf value = 40 Then\nResponse.Write 6 * 7\nEnd If\n:quit\n' | $(BIN_DIR)/openasp-cli | tr -d '\n')" = "4242"
	./scripts/smoke.sh $(BIN_DIR)/openasp
	python3 tests/integration/fastcgi/test_fpm_config.py $(BIN_DIR)/openasp-fpm
	python3 tests/integration/extensions/test_extension_sdk.py $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm
	python3 tests/integration/fastcgi/test_fastcgi.py $(BIN_DIR)/openasp-fpm tests/fixtures/reference
	python3 tests/integration/fastcgi/test_worker_supervisor.py $(BIN_DIR)/openasp-fpm tests/fixtures/reference
	python3 tests/integration/debugger/test_fpm_debug.py $(BIN_DIR)/openasp-fpm tests/fixtures/common
	python3 tests/integration/kv/test_kv_pool.py
	python3 tests/integration/process/test_process_cache.py $(BIN_DIR)/openasp-fpm $(BIN_DIR)/openasp-dis

test-conformance: $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm
	python3 tests/conformance/runner.py $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm

test-e2e: $(BIN_DIR)/openasp
	test -f tests/e2e/apps/backlighting-cms/Index.asp
	test -f tests/e2e/apps/chatroom-online/chatbord.asp
	test -f tests/e2e/apps/asp-device/index.asp
	test -f tests/e2e/apps/sblog/Default.asp
	test -f tests/e2e/apps/zblog/default.asp
	python3 -m compileall -q tests/e2e/runners

test-stress: $(BIN_DIR)/openasp
	test "$$($(BIN_DIR)/openasp tests/fixtures/common/runtime_long_stress.asp --root tests/fixtures/common --query iterations=4000 --runtime-mode=service --gc-young-size=16K --gc-old-threshold=32K --gc-heap-limit=8M)" = "stress-ok|4000|456719|4000|127"

test: format-check test-unit test-integration test-conformance

test-all: test test-e2e test-stress

test-benchmark: zblog-benchmark

fixture-diff: $(BIN_DIR)/openasp
	python3 scripts/differential_fixtures.py --candidate $(BIN_DIR)/openasp

zblog-benchmark: $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm
	python3 tests/benchmarks/runners/benchmark_zblog.py --openasp-cli $(BIN_DIR)/openasp --openasp-fpm $(BIN_DIR)/openasp-fpm

external-test: $(BIN_DIR)/openasp $(BIN_DIR)/openasp-fpm
	python3 tests/integration/extensions/test_external_components.py

full-conformance: test external-test fixture-diff

asan: CFLAGS = -O1 -g -fsanitize=address,undefined
asan: clean
	$(MAKE) CFLAGS="$(CFLAGS)" all
	$(MAKE) CFLAGS="$(CFLAGS)" test-unit
	ASAN_OPTIONS=detect_leaks=$(ASAN_LEAKS) $(BIN_DIR)/openasp tests/fixtures/common/service_gc.asp --root tests/fixtures/common --runtime-mode=service --gc-young-size=16K --gc-old-threshold=32K --gc-heap-limit=8M
	if [ "$(UNAME_S)" = Darwin ]; then python3 tests/support/run_darwin_leaks.py -- $(TEST_BUILD)/test-runtime; fi
	if [ "$(UNAME_S)" = Darwin ]; then python3 tests/support/run_darwin_leaks.py -- $(TEST_BUILD)/test-state-store; fi

clean:
	rm -rf $(BUILD)
	$(MAKE) -C extensions/openasp-ai clean
