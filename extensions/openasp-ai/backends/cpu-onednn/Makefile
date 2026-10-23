# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

CXX ?= c++
ROOT := ../../../..
AI_ROOT := ../..
UNAME_S := $(shell uname -s)
SHARED_EXT := $(if $(filter Darwin,$(UNAME_S)),dylib,so)
TARGET := libopenasp_ai_cpu_onednn.$(SHARED_EXT)

ONEDNN_PREFIX ?= /usr/local
ONEDNN_CFLAGS ?= $(shell PKG_CONFIG_PATH="$(ONEDNN_PREFIX)/lib/pkgconfig:$(ONEDNN_PREFIX)/lib64/pkgconfig:$$PKG_CONFIG_PATH" pkg-config --cflags dnnl 2>/dev/null)
ONEDNN_LIBS ?= $(shell PKG_CONFIG_PATH="$(ONEDNN_PREFIX)/lib/pkgconfig:$(ONEDNN_PREFIX)/lib64/pkgconfig:$$PKG_CONFIG_PATH" pkg-config --libs dnnl 2>/dev/null)
ONEDNN_LIBDIR ?= $(firstword $(wildcard $(ONEDNN_PREFIX)/lib64 $(ONEDNN_PREFIX)/lib))
CXXFLAGS ?= -O3
CXXFLAGS += -std=c++14 -fPIC -Wall -Wextra -Werror -Wpedantic -fopenmp
CPPFLAGS := -I$(AI_ROOT)/include -I$(ROOT)/include -I$(ONEDNN_PREFIX)/include $(ONEDNN_CFLAGS)
LDFLAGS += -shared
LDLIBS := $(if $(strip $(ONEDNN_LIBS)),$(ONEDNN_LIBS),-L$(ONEDNN_LIBDIR) -Wl,-rpath,$(ONEDNN_LIBDIR) -ldnnl) -fopenmp

.PHONY: all clean

all: $(TARGET)

$(TARGET): onednn_backend.cpp $(AI_ROOT)/include/openasp/ai_backend.h $(AI_ROOT)/include/openasp/ai.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) onednn_backend.cpp $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
