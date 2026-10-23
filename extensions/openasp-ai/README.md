[English](README.md) | [Simplified Chinese](README.zh-CN.md)

# OpenASP.AI Extension

`openasp-ai` is the C11 external extension implementation of
`docs/openasp.ai-design.md`. The component exposes the single creatable ProgID
`OpenASP.AI` through Extension ABI v2.1. Tensor, Module, Model, Device, and the
other child objects are internal classes bound to a request-scoped AI context.

## Building

Run these commands from the repository root:

```sh
make AI=1
make ai-test
```

To build only the extension:

```sh
make -C extensions/openasp-ai
```

The output is `libopenasp_ai.dylib` on macOS and `libopenasp_ai.so` on Linux.
The build requires a C11 compiler, pthread, and OpenSSL. `make clean` removes
all local build artifacts.

## Deployment

1. Install the shared library at an absolute path writable only by administrators.
2. Calculate its digest with `shasum -a 256 <library>`.
3. Copy `openasp-ai.yaml.example` and configure its library path, SHA-256, and resource budgets.
4. Prefer `mode: isolated` in production. Trusted, latency-sensitive deployments may use `in-process`.

The configuration allowlist contains only `OpenASP.AI`. Do not add internal
classes to `prog-ids`.

## Capabilities

- Contiguous tensors, views, dtypes, quantization, broadcasting, reductions, convolution, normalization, Attention, and sampling operators.
- Dynamic Autograd, Module/ParameterCollection/StateDict, SGD/Adam/AdamW, schedulers, and loss scaling.
- `MSELoss`, `L1Loss`, `BCELoss`, `BCEWithLogitsLoss`, `NLLLoss`, `KLDivLoss`,
  `HuberLoss`, `SmoothL1Loss`, `FocalLoss`, `DiceLoss`, `InfoNCELoss`,
  `CTCLoss`, and fused `CrossEntropy`.
- Dataset, parallel-prefetch DataLoader, Tokenizer, and KV Cache.
- Core IR, Schedule IR, Execution Plan, tracing, asynchronous Event, execution-plan caching, and profiler.
- `.oaspweights`, `.oaspckpt`, `.oaspbundle`, `.oaspmodel`, ONNX, and SafeTensors.
- Backend ABI 1.4 device enumeration, device-resident tensors, streams/events,
  eager training, whole-graph execution, and collective interfaces.

The built-in reference CPU backend is the correctness baseline. The CUDA
backend lives independently under `backends/cuda`; AI Core contains no CUDA
headers or vendor types. Tensor storage can remain device-resident, and eager
execution, autograd, SGD/Adam/AdamW, Attention, and Execution Plan submit work
directly to device streams. Cross-device AllReduce uses the generic collective
ABI, with an optional NCCL implementation in the CUDA backend.

## oneDNN CPU

`backends/cpu-onednn` is the optional high-performance CPU backend. The
built-in reference CPU implementation remains available as a numerical
baseline. The backend loads through the generic Backend ABI 1.4, so ASP models
do not need modification. Building requires oneDNN, a C++14 compiler, and
OpenMP. Pass `ONEDNN_PREFIX` when oneDNN is installed at a nonstandard path:

```sh
make -C extensions/openasp-ai cpu-onednn-test ONEDNN_PREFIX=/opt/oneDNN
```

Select `cpu-onednn:0` at deployment time and configure OpenMP for the target
CPU core count:

```sh
export OPENASP_AI_BACKEND=/absolute/path/libopenasp_ai_cpu_onednn.so
export OPENASP_AI_BACKEND_SHA256=<lowercase-sha256>
export OPENASP_AI_DEVICE=cpu-onednn:0
export OMP_NUM_THREADS=8
```

The shared library contains an rpath to the library directory under
`ONEDNN_PREFIX`. The backend will fail to load if the oneDNN runtime is moved
or removed. Production deployments should install oneDNN at a stable path
writable only by administrators.

## CUDA

CUDA is not a default build dependency. Run these commands on a CUDA host:

```sh
make -C extensions/openasp-ai cuda-test CUDA_HOME=/usr/local/cuda CUDA_ARCH=70
make -C extensions/openasp-ai/backends/cuda NCCL=1
make -C extensions/openasp-ai cuda-test CUDA_ARCH=70 CUDNN=1 CUDNN_PREFIX=/opt/cudnn
```

`NCCL=1` requires NCCL headers and libraries. Single-GPU functionality is
unaffected when NCCL is disabled; multi-device AllReduce returns an unsupported
error with a concrete remediation suggestion. `CUDNN=1` enables the optional
cuDNN Graph LayerNorm/RMSNorm forward execution plan. Unsupported GPUs, shapes,
or plans are cached as unsupported and fall back to the built-in CUDA kernel.
`CUDNN_PREFIX` should contain `include/x86_64-linux-gnu` and
`lib/x86_64-linux-gnu`; use `CUDNN_INCLUDE` and `CUDNN_LIBDIR` when the layout
differs. Use `CUDA_ARCH=70` for V100.

Contiguous FP32 MatMul uses cuBLASLt by default and caches descriptors and
algorithms by shape. Full Sum/Mean/Min/Max/Norm reductions use CUB. Enable
Tensor Core mixed precision with:

```sh
export OPENASP_AI_CUDA_FP16=1
export OPENASP_AI_CUDA_FP16_WORKSPACE_MB=32
```

At GEMM boundaries, this mode converts FP32 inputs to FP16 while retaining FP32
accumulation, output, master weights, and gradients. GEMMs that exceed the
conversion workspace automatically use FP32 cuBLASLt. On CUDA, `LossScaler`
performs finite checks and unscaling in a device kernel and transfers only one
4-byte status value per step.

A cuDNN build attempts its execution plan by default. Set
`OPENASP_AI_CUDA_CUDNN=0` to disable it for a comparison baseline. Execution
Plan uses stream capture on its first execution to construct a CUDA Graph and
launches the graph directly afterward. A cuDNN shape that was not warmed before
capture uses the built-in kernel to avoid creating a workspace during capture.

On V100 (SM70), the cuDNN 9.1 LayerNorm/RMSNorm Graph engine returns
`CUDNN_STATUS_NOT_SUPPORTED_ARCH_MISMATCH`. V100 deployments should therefore
set `OPENASP_AI_CUDA_CUDNN=0` and use the built-in CUDA normalization kernel.
A100 (SM80) and newer architectures can retain the default and let the backend
probe and cache plans by shape. When cuDNN is installed outside a system path,
its engine plugins are loaded internally with `dlopen`; add `CUDNN_LIBDIR` to
`LD_LIBRARY_PATH` as well.

Select the backend and default device through the process environment without
modifying ASP models:

```sh
export OPENASP_AI_BACKEND=/absolute/path/libopenasp_ai_cuda.so
export OPENASP_AI_BACKEND_SHA256=<lowercase-sha256>
export OPENASP_AI_DEVICE=cuda:0
```

The backend path must be an absolute path to a regular file that is not
group/world writable. Production configuration must still enable SHA-256
verification for the allowlisted `libopenasp_ai.so`.

## Loss Functions

The AI object provides the following PyTorch-style methods:

```asp
Set ce = ai.CrossEntropy(logits, labels, classWeight, -100, 0.1, "mean")
Set mse = ai.MSELoss(input, target, "mean")
Set l1 = ai.L1Loss(input, target, "mean")
Set bce = ai.BCELoss(probability, target, "mean")
Set stableBce = ai.BCEWithLogitsLoss(logits, target, positiveWeight, "mean")
Set nll = ai.NLLLoss(logProbabilities, labels, "mean")
Set kl = ai.KLDivLoss(logProbabilities, targetProbabilities, "batchmean", False)
Set huber = ai.HuberLoss(input, target, 1.0, "mean")
Set smoothL1 = ai.SmoothL1Loss(input, target, 1.0, "mean")
Set focal = ai.FocalLoss(logits, target, 0.25, 2.0, "mean")
Set dice = ai.DiceLoss(logits, target, 1.0, 0.0000001, True, "mean")
Set infoNce = ai.InfoNCELoss(query, key, 0.07, True, "mean")
Set ctc = ai.CTCLoss(logProbabilities, targets, inputLengths, targetLengths, 0, True, "mean")
```

`reduction` accepts `none`, `mean`, or `sum`; `KLDivLoss` additionally supports
`batchmean`. `NLLLoss` requires `[batch, classes]` log probabilities and
`[batch]` Int64 labels. `BCELoss` accepts probabilities and applies numerically
stable clamping; prefer `BCEWithLogitsLoss` when the model emits logits. Its
third argument accepts either the legacy reduction string or a broadcastable
`pos_weight` tensor, with the fourth argument specifying reduction.

For `CrossEntropy`, `classWeight` may be `Nothing`, `ignoreIndex` defaults to
`-100`, and `labelSmoothing` defaults to `0`. `FocalLoss` uses multiclass
softmax for Int64 `[batch]` labels and binary sigmoid for floating-point labels
with the same shape as the input. `DiceLoss` treats the first dimension as the
batch. `InfoNCELoss` accepts paired `[N, D]` tensors and applies contrastive
learning to the cosine-similarity matrix. `CTCLoss` accepts `[T, N, C]` log
probabilities, `[N, S]` padded Int64 targets, and two `[N]` Int64 length
tensors; targets must not contain the blank class.

## ASP Example

```asp
<%
Set ai = Server.CreateObject("OpenASP.AI")
devices = ai.Devices()
Set cpu = ai.GetDevice("cpu:0")

Set model = ai.Sequential()
Call model.Add(ai.Linear(2, 4))
Call model.Add(ai.ReLULayer())
Call model.Add(ai.Linear(4, 1))

Set parameters = model.NamedParameters()
Set output = model.Forward(ai.Tensor(Array(1, 2)).Reshape(Array(1, 2)))
Response.Write cpu.Backend & "|" & parameters.Count & "|" & output.Item(0, 0)
%>
```
