// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_AI_CUDA_CUDNN_GRAPH_CUH
#define OPENASP_AI_CUDA_CUDNN_GRAPH_CUH

struct cuda_cudnn_norm_plan {
    uint32_t rank;
    int64_t dimensions[OASP_AI_MAX_RANK];
    uint32_t rms_only;
    uint32_t epsilon_bits;
    bool supported;
    uint64_t last_used;
    cudnnBackendDescriptor_t x;
    cudnnBackendDescriptor_t scale;
    cudnnBackendDescriptor_t bias;
    cudnnBackendDescriptor_t epsilon;
    cudnnBackendDescriptor_t y;
    cudnnBackendDescriptor_t operation;
    cudnnBackendDescriptor_t operation_graph;
    cudnnBackendDescriptor_t engine_config;
    cudnnBackendDescriptor_t execution_plan;
    void *workspace;
    int64_t workspace_bytes;
    float *epsilon_device;
};

struct cuda_cudnn_state {
    cudnnHandle_t handle;
    std::vector<cuda_cudnn_norm_plan *> plans;
    uint64_t clock;
};

static void cuda_cudnn_destroy_descriptor(cudnnBackendDescriptor_t descriptor) {
    if (descriptor)
        cudnnBackendDestroyDescriptor(descriptor);
}

static void cuda_cudnn_destroy_norm_plan(cuda_cudnn_norm_plan *plan) {
    if (!plan)
        return;
    cudaFree(plan->epsilon_device);
    cudaFree(plan->workspace);
    cuda_cudnn_destroy_descriptor(plan->execution_plan);
    cuda_cudnn_destroy_descriptor(plan->engine_config);
    cuda_cudnn_destroy_descriptor(plan->operation_graph);
    cuda_cudnn_destroy_descriptor(plan->operation);
    cuda_cudnn_destroy_descriptor(plan->y);
    cuda_cudnn_destroy_descriptor(plan->epsilon);
    cuda_cudnn_destroy_descriptor(plan->bias);
    cuda_cudnn_destroy_descriptor(plan->scale);
    cuda_cudnn_destroy_descriptor(plan->x);
    delete plan;
}

static cuda_cudnn_state *cuda_cudnn_create(cudaStream_t stream) {
    cuda_cudnn_state *state = new (std::nothrow) cuda_cudnn_state{};
    if (!state)
        return nullptr;
    if (cudnnCreate(&state->handle) != CUDNN_STATUS_SUCCESS ||
        cudnnSetStream(state->handle, stream) != CUDNN_STATUS_SUCCESS) {
        if (state->handle)
            cudnnDestroy(state->handle);
        delete state;
        return nullptr;
    }
    return state;
}

static void cuda_cudnn_destroy(cuda_cudnn_state *state) {
    if (!state)
        return;
    for (cuda_cudnn_norm_plan *plan : state->plans)
        cuda_cudnn_destroy_norm_plan(plan);
    cudnnDestroy(state->handle);
    delete state;
}

static bool cuda_cudnn_set(cudnnBackendDescriptor_t descriptor, cudnnBackendAttributeName_t name,
                           cudnnBackendAttributeType_t type, int64_t count, const void *value) {
    return cudnnBackendSetAttribute(descriptor, name, type, count, value) == CUDNN_STATUS_SUCCESS;
}

static bool cuda_cudnn_make_tensor(cudnnBackendDescriptor_t *output, uint32_t rank,
                                   const int64_t *dimensions, const int64_t *strides, int64_t uid) {
    int64_t alignment = 16;
    cudnnDataType_t dtype = CUDNN_DATA_FLOAT;
    return cudnnBackendCreateDescriptor(CUDNN_BACKEND_TENSOR_DESCRIPTOR, output) == CUDNN_STATUS_SUCCESS &&
           cuda_cudnn_set(*output, CUDNN_ATTR_TENSOR_DATA_TYPE, CUDNN_TYPE_DATA_TYPE, 1, &dtype) &&
           cuda_cudnn_set(*output, CUDNN_ATTR_TENSOR_DIMENSIONS, CUDNN_TYPE_INT64, rank, dimensions) &&
           cuda_cudnn_set(*output, CUDNN_ATTR_TENSOR_STRIDES, CUDNN_TYPE_INT64, rank, strides) &&
           cuda_cudnn_set(*output, CUDNN_ATTR_TENSOR_UNIQUE_ID, CUDNN_TYPE_INT64, 1, &uid) &&
           cuda_cudnn_set(*output, CUDNN_ATTR_TENSOR_BYTE_ALIGNMENT, CUDNN_TYPE_INT64, 1, &alignment) &&
           cudnnBackendFinalize(*output) == CUDNN_STATUS_SUCCESS;
}

static bool cuda_cudnn_find_engine(cudnnBackendDescriptor_t operation_graph,
                                   cudnnBackendHeurMode_t mode,
                                   cudnnBackendDescriptor_t *engine_config) {
    cudnnBackendDescriptor_t heuristic = nullptr;
    int64_t returned = 0;
    bool ok = cudnnBackendCreateDescriptor(CUDNN_BACKEND_ENGINEHEUR_DESCRIPTOR,
                                           &heuristic) == CUDNN_STATUS_SUCCESS &&
              cuda_cudnn_set(heuristic, CUDNN_ATTR_ENGINEHEUR_OPERATION_GRAPH,
                             CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &operation_graph) &&
              cuda_cudnn_set(heuristic, CUDNN_ATTR_ENGINEHEUR_MODE,
                             CUDNN_TYPE_HEUR_MODE, 1, &mode) &&
              cudnnBackendFinalize(heuristic) == CUDNN_STATUS_SUCCESS &&
              cudnnBackendCreateDescriptor(CUDNN_BACKEND_ENGINECFG_DESCRIPTOR,
                                           engine_config) == CUDNN_STATUS_SUCCESS &&
              cudnnBackendGetAttribute(heuristic, CUDNN_ATTR_ENGINEHEUR_RESULTS,
                                       CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &returned,
                                       engine_config) == CUDNN_STATUS_SUCCESS &&
              returned == 1;
    cuda_cudnn_destroy_descriptor(heuristic);
    if (!ok) {
        cuda_cudnn_destroy_descriptor(*engine_config);
        *engine_config = nullptr;
    }
    return ok;
}

static cuda_cudnn_norm_plan *cuda_cudnn_build_norm_plan(cuda_cudnn_state *state,
                                                        const oasp_ai_tensor_view &input,
                                                        double epsilon, bool rms_only) {
    cuda_cudnn_norm_plan *plan = new (std::nothrow) cuda_cudnn_norm_plan{};
    if (!plan)
        return nullptr;
    plan->rank = std::max<uint32_t>(input.rank, 3);
    uint32_t padding = plan->rank - input.rank;
    for (uint32_t axis = 0; axis < plan->rank; axis++)
        plan->dimensions[axis] = axis < padding ? 1 : input.shape[axis - padding];
    plan->rms_only = rms_only;
    float epsilon_value = static_cast<float>(epsilon);
    std::memcpy(&plan->epsilon_bits, &epsilon_value, sizeof(epsilon_value));

    int64_t tensor_strides[OASP_AI_MAX_RANK] = {};
    int64_t parameter_dimensions[OASP_AI_MAX_RANK] = {};
    int64_t parameter_strides[OASP_AI_MAX_RANK] = {};
    int64_t scalar_dimensions[OASP_AI_MAX_RANK] = {};
    int64_t scalar_strides[OASP_AI_MAX_RANK] = {};
    int64_t stride = 1;
    for (uint32_t axis = plan->rank; axis; axis--) {
        tensor_strides[axis - 1] = stride;
        stride *= plan->dimensions[axis - 1];
        parameter_dimensions[axis - 1] = axis == plan->rank ? plan->dimensions[axis - 1] : 1;
        parameter_strides[axis - 1] = axis == plan->rank ? 1 : parameter_dimensions[axis] * parameter_strides[axis];
        scalar_dimensions[axis - 1] = 1;
        scalar_strides[axis - 1] = 1;
    }

    bool ok = cuda_cudnn_make_tensor(&plan->x, plan->rank, plan->dimensions, tensor_strides, 1) &&
              cuda_cudnn_make_tensor(&plan->scale, plan->rank, parameter_dimensions, parameter_strides, 2) &&
              cuda_cudnn_make_tensor(&plan->bias, plan->rank, parameter_dimensions, parameter_strides, 3) &&
              cuda_cudnn_make_tensor(&plan->epsilon, plan->rank, scalar_dimensions, scalar_strides, 4) &&
              cuda_cudnn_make_tensor(&plan->y, plan->rank, plan->dimensions, tensor_strides, 5);
    cudnnBackendNormMode_t mode = rms_only ? CUDNN_RMS_NORM : CUDNN_LAYER_NORM;
    cudnnBackendNormFwdPhase_t phase = CUDNN_NORM_FWD_INFERENCE;
    if (ok)
        ok = cudnnBackendCreateDescriptor(CUDNN_BACKEND_OPERATION_NORM_FORWARD_DESCRIPTOR,
                                          &plan->operation) == CUDNN_STATUS_SUCCESS &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_MODE,
                            CUDNN_TYPE_NORM_MODE, 1, &mode) &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_PHASE,
                            CUDNN_TYPE_NORM_FWD_PHASE, 1, &phase) &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_XDESC,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->x) &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_SCALE_DESC,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->scale) &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_BIAS_DESC,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->bias) &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_EPSILON_DESC,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->epsilon) &&
             cuda_cudnn_set(plan->operation, CUDNN_ATTR_OPERATION_NORM_FWD_YDESC,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->y) &&
             cudnnBackendFinalize(plan->operation) == CUDNN_STATUS_SUCCESS;
    if (ok)
        ok = cudnnBackendCreateDescriptor(CUDNN_BACKEND_OPERATIONGRAPH_DESCRIPTOR,
                                          &plan->operation_graph) == CUDNN_STATUS_SUCCESS &&
             cuda_cudnn_set(plan->operation_graph, CUDNN_ATTR_OPERATIONGRAPH_HANDLE,
                            CUDNN_TYPE_HANDLE, 1, &state->handle) &&
             cuda_cudnn_set(plan->operation_graph, CUDNN_ATTR_OPERATIONGRAPH_OPS,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->operation) &&
             cudnnBackendFinalize(plan->operation_graph) == CUDNN_STATUS_SUCCESS;

    if (ok && !cuda_cudnn_find_engine(plan->operation_graph, CUDNN_HEUR_MODE_A,
                                      &plan->engine_config))
        ok = cuda_cudnn_find_engine(plan->operation_graph, CUDNN_HEUR_MODE_FALLBACK,
                                    &plan->engine_config);
    if (ok)
        ok = cudnnBackendCreateDescriptor(CUDNN_BACKEND_EXECUTION_PLAN_DESCRIPTOR,
                                          &plan->execution_plan) == CUDNN_STATUS_SUCCESS &&
             cuda_cudnn_set(plan->execution_plan, CUDNN_ATTR_EXECUTION_PLAN_HANDLE,
                            CUDNN_TYPE_HANDLE, 1, &state->handle) &&
             cuda_cudnn_set(plan->execution_plan, CUDNN_ATTR_EXECUTION_PLAN_ENGINE_CONFIG,
                            CUDNN_TYPE_BACKEND_DESCRIPTOR, 1, &plan->engine_config) &&
             cudnnBackendFinalize(plan->execution_plan) == CUDNN_STATUS_SUCCESS &&
             cudnnBackendGetAttribute(plan->execution_plan, CUDNN_ATTR_EXECUTION_PLAN_WORKSPACE_SIZE,
                                      CUDNN_TYPE_INT64, 1, nullptr,
                                      &plan->workspace_bytes) == CUDNN_STATUS_SUCCESS;
    if (ok && plan->workspace_bytes)
        ok = cudaMalloc(&plan->workspace, static_cast<size_t>(plan->workspace_bytes)) == cudaSuccess;
    if (ok)
        ok = cudaMalloc(reinterpret_cast<void **>(&plan->epsilon_device), sizeof(float)) == cudaSuccess &&
             cudaMemcpy(plan->epsilon_device, &epsilon_value, sizeof(epsilon_value),
                        cudaMemcpyHostToDevice) == cudaSuccess;
    plan->supported = ok;
    return plan;
}

static bool cuda_cudnn_norm_plan_matches(const cuda_cudnn_norm_plan *plan,
                                         const oasp_ai_tensor_view &input,
                                         double epsilon, bool rms_only) {
    uint32_t rank = std::max<uint32_t>(input.rank, 3);
    if (plan->rank != rank || plan->rms_only != static_cast<uint32_t>(rms_only))
        return false;
    float epsilon_value = static_cast<float>(epsilon);
    uint32_t epsilon_bits;
    std::memcpy(&epsilon_bits, &epsilon_value, sizeof(epsilon_value));
    if (plan->epsilon_bits != epsilon_bits)
        return false;
    uint32_t padding = rank - input.rank;
    for (uint32_t axis = 0; axis < rank; axis++)
        if (plan->dimensions[axis] != (axis < padding ? 1 : input.shape[axis - padding]))
            return false;
    return true;
}

static bool cuda_cudnn_execute_norm(cuda_cudnn_state *state, const oasp_ai_tensor_view &input,
                                    const oasp_ai_tensor_view &scale, const oasp_ai_tensor_view &bias,
                                    oasp_ai_tensor_view &output, double epsilon, bool rms_only,
                                    bool allow_plan_build) {
    if (!state)
        return false;
    cuda_cudnn_norm_plan *plan = nullptr;
    for (cuda_cudnn_norm_plan *candidate : state->plans)
        if (cuda_cudnn_norm_plan_matches(candidate, input, epsilon, rms_only)) {
            plan = candidate;
            break;
        }
    if (!plan) {
        if (!allow_plan_build)
            return false;
        plan = cuda_cudnn_build_norm_plan(state, input, epsilon, rms_only);
        if (!plan)
            return false;
        if (state->plans.size() >= 32) {
            auto oldest = std::min_element(
                state->plans.begin(), state->plans.end(),
                [](const cuda_cudnn_norm_plan *left, const cuda_cudnn_norm_plan *right) {
                    return left->last_used < right->last_used;
                });
            cuda_cudnn_destroy_norm_plan(*oldest);
            state->plans.erase(oldest);
        }
        state->plans.push_back(plan);
    }
    plan->last_used = ++state->clock;
    if (!plan->supported)
        return false;

    int64_t uids[] = {1, 2, 3, 4, 5};
    void *pointers[] = {
        reinterpret_cast<unsigned char *>(input.storage) + input.byte_offset,
        reinterpret_cast<unsigned char *>(scale.storage) + scale.byte_offset,
        reinterpret_cast<unsigned char *>(bias.storage) + bias.byte_offset,
        plan->epsilon_device,
        reinterpret_cast<unsigned char *>(output.storage) + output.byte_offset,
    };
    cudnnBackendDescriptor_t variant = nullptr;
    bool ok = cudnnBackendCreateDescriptor(CUDNN_BACKEND_VARIANT_PACK_DESCRIPTOR,
                                           &variant) == CUDNN_STATUS_SUCCESS &&
              cuda_cudnn_set(variant, CUDNN_ATTR_VARIANT_PACK_UNIQUE_IDS,
                             CUDNN_TYPE_INT64, 5, uids) &&
              cuda_cudnn_set(variant, CUDNN_ATTR_VARIANT_PACK_DATA_POINTERS,
                             CUDNN_TYPE_VOID_PTR, 5, pointers) &&
              cuda_cudnn_set(variant, CUDNN_ATTR_VARIANT_PACK_WORKSPACE,
                             CUDNN_TYPE_VOID_PTR, 1, &plan->workspace) &&
              cudnnBackendFinalize(variant) == CUDNN_STATUS_SUCCESS &&
              cudnnBackendExecute(state->handle, plan->execution_plan, variant) == CUDNN_STATUS_SUCCESS;
    cuda_cudnn_destroy_descriptor(variant);
    if (!ok)
        plan->supported = false;
    return ok;
}

#endif
