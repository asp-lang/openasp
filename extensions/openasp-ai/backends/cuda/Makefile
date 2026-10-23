# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

CUDA_HOME ?= /usr/local/cuda
NVCC ?= $(CUDA_HOME)/bin/nvcc
ROOT := ../../../..
AI_ROOT := ../..
CUDA_ARCH ?= 70
NCCL ?= 0
CUDNN ?= 0
CUDNN_PREFIX ?= /usr
CUDNN_INCLUDE ?= $(CUDNN_PREFIX)/include/x86_64-linux-gnu
CUDNN_LIBDIR ?= $(CUDNN_PREFIX)/lib/x86_64-linux-gnu
TARGET := libopenasp_ai_cuda.so

NVCCFLAGS ?= -O3
NVCCFLAGS += -std=c++14 -Xcompiler=-fPIC,-Wall,-Wextra -gencode arch=compute_$(CUDA_ARCH),code=sm_$(CUDA_ARCH)
CPPFLAGS := -I$(AI_ROOT)/include -I$(ROOT)/include
LDLIBS := -lcudart -lcublasLt -lcublas
ifeq ($(NCCL),1)
CPPFLAGS += -DOASP_AI_WITH_NCCL
LDLIBS += -lnccl
endif
ifeq ($(CUDNN),1)
CPPFLAGS += -DOASP_AI_WITH_CUDNN -I$(CUDNN_INCLUDE)
LDLIBS += -L$(CUDNN_LIBDIR) -Xlinker=-rpath -Xlinker=$(CUDNN_LIBDIR) -lcudnn
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): cuda_backend.cu cuda_cudnn_graph.cuh $(AI_ROOT)/include/openasp/ai_backend.h $(AI_ROOT)/include/openasp/ai.h
	$(NVCC) $(NVCCFLAGS) $(CPPFLAGS) -shared cuda_backend.cu $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
