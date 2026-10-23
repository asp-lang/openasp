[English](README.md) | [简体中文](README.zh-CN.md)

# OpenASP.AI MNIST 模型示例

本目录覆盖从线性基线到卷积、循环网络和视觉 Transformer 的主要 MNIST 识别模型。
每种模型结构都有独立 ASP 文件，可以单独训练、保存权重、重新加载并输出 Accuracy、
Macro Precision 和 Macro Recall。

## 模型目录

| 模型 | ASP 文件 | 结构与用途 |
|---|---|---|
| Softmax 回归 | [`mnist-logistic.asp`](mnist-logistic.asp) | `784 -> 10`，用于建立线性分类基线 |
| 多层感知机 | [`mnist-class.asp`](mnist-class.asp) | `784 -> 128 -> 64 -> 10`，标准 PyTorch 风格 Module 示例 |
| 残差 MLP | [`mnist-resmlp.asp`](mnist-resmlp.asp) | 128 维特征和两个残差块，验证深层全连接网络 |
| LeNet 风格网络 | [`mnist-lenet.asp`](mnist-lenet.asp) | 两层可学习局部 patch 卷积和 `120 -> 84 -> 10` 分类头 |
| CNN | [`mnist-cnn.asp`](mnist-cnn.asp) | 两层共享 patch 卷积，适合利用数字的局部空间结构 |
| Vanilla RNN | [`mnist-rnn.asp`](mnist-rnn.asp) | 将两行像素合并为一步，按长度 14 的序列输入基础循环单元 |
| GRU | [`mnist-gru.asp`](mnist-gru.asp) | 更新门和重置门缓解普通 RNN 的长期依赖问题 |
| LSTM | [`mnist-lstm.asp`](mnist-lstm.asp) | 输入门、遗忘门、输出门和 cell state |
| Transformer | [`mnist-transformer.asp`](mnist-transformer.asp) | 行序列嵌入、Self-Attention 和 Encoder Block |
| Vision Transformer | [`mnist-vit.asp`](mnist-vit.asp) | 4x4 image patch、位置编码和 Transformer Encoder |
| MLP-Mixer | [`mnist-mixer.asp`](mnist-mixer.asp) | 交替执行 token-mixing 和 channel-mixing MLP |

[`mnist.asp`](mnist.asp) 保留早期过程式 MLP 示例；新代码优先参考
[`mnist-class.asp`](mnist-class.asp)。新增模型共用
[`mnist-common.inc`](mnist-common.inc) 中的数据加载、AdamW 训练、权重重载和评估流程。

训练时 `CrossEntropy` 直接接收最后一层 logits；评估时再应用 Softmax。这与主流深度学习
框架的数值稳定训练方式一致。

## 实测结果

以下结果使用固定随机种子 `42`，在完整 MNIST 训练集上训练，并在保存权重、重新创建模型和
加载权重后评估 10,000 个测试样本。不同模型的默认 epoch 和后端不同，因此该表用于确认
实现可训练及给出识别效果，不应视为统一硬件性能排名。

| 模型 | Epoch | 后端 | Accuracy | Macro Precision | Macro Recall |
|---|---:|---|---:|---:|---:|
| Softmax 回归 | 10 | `cuda:0` | 92.32% | 92.37% | 92.19% |
| 多层感知机 | 5 | `cpu-onednn:0` | 97.51% | 97.52% | 97.47% |
| 残差 MLP | 5 | `cpu-onednn:0` | 97.39% | 97.43% | 97.33% |
| LeNet 风格网络 | 3 | `cpu-onednn:0` | 96.86% | 96.89% | 96.83% |
| CNN | 3 | `cuda:0` | 95.80% | 95.83% | 95.73% |
| Vanilla RNN | 3 | `cpu-onednn:0` | 94.19% | 94.13% | 94.13% |
| GRU | 3 | `cpu-onednn:0` | 96.74% | 96.71% | 96.72% |
| LSTM | 3 | `cpu-onednn:0` | 96.28% | 96.27% | 96.23% |
| Transformer | 1 | `cuda:0` | 72.19% | 71.91% | 71.74% |
| Vision Transformer | 1 | `cuda:0` | 59.04% | 59.79% | 58.23% |
| MLP-Mixer | 1 | `cuda:0` | 90.86% | 90.92% | 90.72% |

## 准备数据

在本目录执行：

```sh
chmod +x download-data.sh
./download-data.sh
```

也可以把数据放到独立目录：

```sh
./download-data.sh /data00/openasp-data/mnist
```

脚本会下载并解压四个标准 MNIST IDX 文件，不会把压缩包留在源码树中。

## 构建

在仓库根目录构建 OpenASP 和 OpenASP.AI：

```sh
make AI=1
```

使用 V100 CUDA 后端时另外执行：

```sh
make -C extensions/openasp-ai/backends/cuda CUDA_HOME=/usr/local/cuda CUDA_ARCH=70
```

OpenASP 外部组件配置需要加载构建出的 `extensions/openasp-ai/libopenasp_ai.so`，并配置其
SHA-256。完整配置字段可参考
[`../../openasp-ai.yaml.example`](../../openasp-ai.yaml.example)。

## 运行

CPU 示例：

```sh
build/bin/openasp \
  -c /absolute/path/openasp-ai.yaml \
  extensions/openasp-ai/examples/mnist/mnist-class.asp \
  --root extensions/openasp-ai/examples/mnist
```

所有新增模型接受相同的 query 参数：

- `data`：四个 IDX 数据文件所在目录，默认是当前 root 下的 `data`；
- `epochs`：训练轮数，省略时使用模型文件中的 `DefaultEpochCount`；
- `weights`：权重保存和重载路径，默认是 `<data>/<ModelSlug>.oaspweights`。

例如使用独立数据目录训练 GRU 3 个 epoch：

```sh
build/bin/openasp \
  -c /absolute/path/openasp-ai.yaml \
  extensions/openasp-ai/examples/mnist/mnist-gru.asp \
  --root extensions/openasp-ai/examples/mnist \
  --query 'data=/data00/openasp-data/mnist&epochs=3&weights=/data00/openasp-data/mnist/mnist-gru.oaspweights'
```

CPU 和 CUDA 使用完全相同的 ASP 文件。切换 CUDA 时只需设置部署环境：

```sh
export OPENASP_AI_BACKEND=/absolute/path/libopenasp_ai_cuda.so
export OPENASP_AI_BACKEND_SHA256=<lowercase-sha256>
export OPENASP_AI_DEVICE=cuda:0
```

V100 上 cuDNN Graph 不受支持时，再设置 `OPENASP_AI_CUDA_CUDNN=0` 启用稳定回退路径，然后执行
同一条 `openasp` 命令。

## PyTorch 风格 ASP Class

[`mnist-class.asp`](mnist-class.asp) 使用 ASP `Class MnistMLP` 封装各层，并通过
`OpenASP.AI.Module` 注册 `fc1`、`relu1`、`fc2`、`relu2`、`fc3`，因此具备稳定的
StateDict 参数路径。

该示例训练结束后保存 `.oaspweights`，随后重新创建 `MnistMLP`、加载权重并使用新对象
完成推理和准召率统计：

```sh
build/bin/openasp \
  -c /absolute/path/openasp-ai.yaml \
  extensions/openasp-ai/examples/mnist/mnist-class.asp \
  --root extensions/openasp-ai/examples/mnist \
  --query 'data=/data00/openasp-data/mnist&epochs=5&weights=/data00/openasp-data/mnist/mnist-class.oaspweights'
```

## 输出指标

脚本输出：

- `accuracy`：正确预测数除以测试样本总数；
- 每类 `precision`：`TP / (TP + FP)`；
- 每类 `recall`：`TP / (TP + FN)`；
- `macro_precision`：数字 0-9 的 precision 算术平均；
- `macro_recall`：数字 0-9 的 recall 算术平均；
- 每类 `tp`、`fp`、`fn` 原始计数。

评估阶段每个张量只执行一次 `ToArray()`；概率张量整块回传，标签保留在 CPU，避免逐元素
`Item()` 或无意义的标签传输造成大量 GPU 同步。
