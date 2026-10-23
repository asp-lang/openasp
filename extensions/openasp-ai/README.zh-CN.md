[English](README.md) | [简体中文](README.zh-CN.md)

# OpenASP.AI 扩展

`openasp-ai` 是 `docs/openasp.ai-design.md` 的 C11 外部扩展实现。组件通过
Extension ABI v2.1 暴露唯一可创建 ProgID `OpenASP.AI`；Tensor、Module、Model、
Device 等子对象均为绑定请求级 AI context 的 internal class。

## 构建

在仓库根目录执行：

```sh
make AI=1
make ai-test
```

也可以只构建该扩展：

```sh
make -C extensions/openasp-ai
```

macOS 产物为 `libopenasp_ai.dylib`，Linux 产物为 `libopenasp_ai.so`。构建需要 C11
编译器、pthread 和 OpenSSL。`make clean` 会删除全部本地产物。

## 部署

1. 将动态库安装到只允许管理员写入的绝对路径。
2. 计算 `shasum -a 256 <library>`。
3. 复制并修改 `openasp-ai.yaml.example` 中的 library、sha256 和资源预算。
4. 生产环境优先使用 `mode: isolated`；可信、低延迟部署可选择 `in-process`。

配置白名单只包含 `OpenASP.AI`。不得把 internal class 加入 `prog-ids`。

## 能力

- 连续 Tensor、view、dtype、量化、广播、归约、卷积、归一化、Attention 和采样算子；
- 动态 Autograd、Module/ParameterCollection/StateDict、SGD/Adam/AdamW、scheduler 和 loss scaler；
- `MSELoss`、`L1Loss`、`BCELoss`、`BCEWithLogitsLoss`、`NLLLoss`、`KLDivLoss`、
  `HuberLoss`、`SmoothL1Loss`、`FocalLoss`、`DiceLoss`、`InfoNCELoss`、
  `CTCLoss` 和融合 `CrossEntropy`；
- Dataset、并行预取 DataLoader、Tokenizer、KV Cache；
- Core IR、Schedule IR、Execution Plan、trace、异步 Event、执行计划缓存和 profiler；
- `.oaspweights`、`.oaspckpt`、`.oaspbundle`、`.oaspmodel`、ONNX 和 SafeTensors；
- Backend ABI 1.4 的设备枚举、device-resident Tensor、stream/event、动态图训练、
  整图执行和 collective 接口。

内置 reference CPU 是正确性基线。CUDA 后端独立位于 `backends/cuda`，AI Core 不包含
CUDA 头文件或厂商类型。Tensor storage 可常驻设备；eager、autograd、SGD/Adam/AdamW、
Attention 和 Execution Plan 均直接提交到设备 stream。跨设备 AllReduce 使用通用
collective ABI；CUDA 后端以可选 NCCL 实现。

## oneDNN CPU

`backends/cpu-onednn` 是可选的高性能 CPU 后端，保留内置 reference CPU 作为数值基线。
后端通过通用 Backend ABI 1.4 加载，ASP 模型无需修改。构建需要 oneDNN、C++14 编译器
和 OpenMP；oneDNN 安装在非标准路径时传入 `ONEDNN_PREFIX`：

```sh
make -C extensions/openasp-ai cpu-onednn-test ONEDNN_PREFIX=/opt/oneDNN
```

部署时选择 `cpu-onednn:0`，并按目标 CPU 核数设置 OpenMP：

```sh
export OPENASP_AI_BACKEND=/absolute/path/libopenasp_ai_cpu_onednn.so
export OPENASP_AI_BACKEND_SHA256=<lowercase-sha256>
export OPENASP_AI_DEVICE=cpu-onednn:0
export OMP_NUM_THREADS=8
```

动态库包含指向 `ONEDNN_PREFIX` library 目录的 rpath。移动或删除 oneDNN runtime 后，
后端将无法加载；生产部署应把 oneDNN 安装到稳定、只允许管理员写入的路径。

## CUDA

CUDA 不属于默认构建依赖。在 CUDA 主机上执行：

```sh
make -C extensions/openasp-ai cuda-test CUDA_HOME=/usr/local/cuda CUDA_ARCH=70
make -C extensions/openasp-ai/backends/cuda NCCL=1
make -C extensions/openasp-ai cuda-test CUDA_ARCH=70 CUDNN=1 CUDNN_PREFIX=/opt/cudnn
```

`NCCL=1` 需要系统提供 NCCL headers 和 library；不启用时单 GPU 功能不受影响，多设备
AllReduce 会返回带修复建议的“不支持”错误。`CUDNN=1` 启用可选 cuDNN Graph
LayerNorm/RMSNorm forward execution plan；不支持的 GPU、形状或 plan 会缓存为不支持并
回退内置 CUDA kernel。`CUDNN_PREFIX` 下应包含 `include/x86_64-linux-gnu` 和
`lib/x86_64-linux-gnu`；布局不同可分别传入 `CUDNN_INCLUDE`、`CUDNN_LIBDIR`。
V100 使用 `CUDA_ARCH=70`。

连续 FP32 MatMul 默认使用 cuBLASLt 并缓存 shape 对应的 descriptor 和算法；全量
Sum/Mean/Min/Max/Norm reduction 使用 CUB。启用 Tensor Core 混合精度：

```sh
export OPENASP_AI_CUDA_FP16=1
export OPENASP_AI_CUDA_FP16_WORKSPACE_MB=32
```

该模式在 GEMM 边界把 FP32 输入转换为 FP16，使用 FP32 accumulate/output，并保留 FP32
主权重和梯度。超过转换 workspace 的 GEMM 自动走 FP32 cuBLASLt。`LossScaler` 在 CUDA
上使用设备 kernel 完成 finite check 和 unscale，每步只回传一个 4-byte 状态值。

编译 cuDNN 后默认尝试其 execution plan；设置 `OPENASP_AI_CUDA_CUDNN=0` 可禁用并用于
对照基准。Execution Plan 首次执行通过 stream capture 建立 CUDA Graph，后续执行直接
launch；捕获期间未预热的 cuDNN shape 会使用内置 kernel，避免在 capture 内创建
workspace。

cuDNN 9.1 的 LayerNorm/RMSNorm Graph engine 在 V100（SM70）上实测返回
`CUDNN_STATUS_NOT_SUPPORTED_ARCH_MISMATCH`，因此 V100 部署应设置
`OPENASP_AI_CUDA_CUDNN=0`，使用内置 CUDA norm kernel。A100（SM80）及更新架构可保留
默认值，让 backend 按 shape 探测并缓存可用 plan。cuDNN 安装在非系统路径时，其 engine
插件由内部 `dlopen` 加载，还需把 `CUDNN_LIBDIR` 加入 `LD_LIBRARY_PATH`。

部署时由进程环境选择 backend 和默认设备，无需修改 ASP 模型：

```sh
export OPENASP_AI_BACKEND=/absolute/path/libopenasp_ai_cuda.so
export OPENASP_AI_BACKEND_SHA256=<lowercase-sha256>
export OPENASP_AI_DEVICE=cuda:0
```

backend 路径必须是非 group/world writable 的绝对普通文件。生产配置仍需对白名单中的
`libopenasp_ai.so` 启用 SHA-256 校验。

## 损失函数

AI 对象提供以下 PyTorch 风格方法：

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

reduction 可取 `none`、`mean` 或 `sum`；`KLDivLoss` 额外支持 `batchmean`。
`NLLLoss` 输入必须是 `[batch, classes]` 的 log-probabilities，标签必须是 `[batch]`
的 Int64。`BCELoss` 接收概率并执行数值稳定夹紧；模型直接输出 logits 时应优先使用
`BCEWithLogitsLoss`。其第三参数既兼容旧版 reduction 字符串，也可传入广播兼容的
`pos_weight` Tensor，再由第四参数指定 reduction。

`CrossEntropy` 的 `classWeight` 可传 `Nothing`，`ignoreIndex` 默认为 `-100`，
`labelSmoothing` 默认为 `0`。`FocalLoss` 对 Int64 `[batch]` 标签执行多分类 softmax
版本，对与输入同形的浮点标签执行二分类 sigmoid 版本。`DiceLoss` 按第一维划分 batch。
`InfoNCELoss` 接收配对的 `[N, D]` Tensor，并对余弦相似度矩阵执行对比学习。
`CTCLoss` 接收 `[T, N, C]` log-probabilities、`[N, S]` 的 padded Int64 targets，
以及两个 `[N]` Int64 length Tensor；targets 不得包含 blank 类。

## ASP 示例

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

完整 API 和格式约束见 `../../docs/openasp.ai-design.md`，扩展加载与隔离配置见
`../../docs/extensions.zh-CN.md`。
