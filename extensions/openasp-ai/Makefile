# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c11 -Wall -Wextra -Werror -Wpedantic
ROOT := ../..
CUDNN ?= 0
CUDNN_PREFIX ?= /usr
CPPFLAGS := -Iinclude -I$(ROOT)/include -Isrc
UNAME_S := $(shell uname -s)
SHARED_EXT := $(if $(filter Darwin,$(UNAME_S)),dylib,so)
DL_LIBS := $(if $(filter Linux,$(UNAME_S)),-ldl,)
THREAD_LIBS := -pthread
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null)
CPPFLAGS += $(OPENSSL_CFLAGS)
LDLIBS := -lm $(DL_LIBS) $(THREAD_LIBS) $(OPENSSL_LIBS)

CORE_SOURCES := \
	src/core.c \
	src/tensor.c \
	src/operators.c \
	src/losses.c \
	src/ctc.c \
	src/operators_extra.c \
	src/autograd.c \
	src/module.c \
	src/state.c \
	src/optimizer.c \
	src/training.c \
	src/graph.c \
	src/async.c \
	src/profiler.c \
	src/backend.c \
	src/transformer.c \
	src/data.c \
	src/quantization.c \
	src/safetensors.c \
	src/oair.c \
	src/checkpoint.c \
	src/bundle.c \
	src/onnx.c \
	src/persistence.c
COMPONENT_SOURCES := src/component.c src/component_metadata.c
TEST_SOURCE := tests/test_ai.c tests/mock_backend.c
OBJECTS := $(CORE_SOURCES:.c=.o)

.PHONY: all clean test asan cuda cuda-test cpu-onednn cpu-onednn-test

all: libopenasp_ai.$(SHARED_EXT) test-ai $(if $(filter 1,$(AI_CUDA)),cuda,)

cuda:
	$(MAKE) -C backends/cuda CUDA_HOME="$(CUDA_HOME)" CUDA_ARCH="$(CUDA_ARCH)" \
		CUDNN="$(CUDNN)" CUDNN_PREFIX="$(CUDNN_PREFIX)"

cuda-test: cuda test-cuda
	./test-cuda "$(CURDIR)/backends/cuda/libopenasp_ai_cuda.so"

cpu-onednn:
	$(MAKE) -C backends/cpu-onednn ONEDNN_PREFIX="$(ONEDNN_PREFIX)"

cpu-onednn-test: cpu-onednn test-onednn
	./test-onednn "$(CURDIR)/backends/cpu-onednn/libopenasp_ai_cpu_onednn.$(SHARED_EXT)"

test-cuda: $(CORE_SOURCES) tests/test_cuda.c src/ai_internal.h include/openasp/ai.h include/openasp/ai_backend.h
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(CORE_SOURCES) tests/test_cuda.c $(LDLIBS) -o $@

test-onednn: $(CORE_SOURCES) tests/test_onednn.c src/ai_internal.h include/openasp/ai.h include/openasp/ai_backend.h
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(CORE_SOURCES) tests/test_onednn.c $(LDLIBS) -o $@

libopenasp_ai.$(SHARED_EXT): $(CORE_SOURCES) $(COMPONENT_SOURCES) src/ai_internal.h src/component_metadata.h include/openasp/ai.h include/openasp/ai_backend.h
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) -fPIC -shared $(CORE_SOURCES) $(COMPONENT_SOURCES) $(LDLIBS) -o $@

test-ai: $(CORE_SOURCES) $(TEST_SOURCE) src/ai_internal.h include/openasp/ai.h include/openasp/ai_backend.h
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) $(CORE_SOURCES) $(TEST_SOURCE) $(LDLIBS) -o $@

test: test-ai
	./test-ai

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined" test

clean:
	$(MAKE) -C backends/cuda clean
	$(MAKE) -C backends/cpu-onednn clean
	rm -rf libopenasp_ai.so libopenasp_ai.dylib libopenasp_ai.dylib.dSYM test-ai test-ai.dSYM test-cuda test-onednn src/*.o
