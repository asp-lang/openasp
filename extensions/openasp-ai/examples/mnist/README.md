[English](README.md) | [Simplified Chinese](README.zh-CN.md)

# OpenASP.AI MNIST Model Examples

This directory covers major MNIST recognition models ranging from linear
baselines to convolutional, recurrent, and vision Transformer architectures.
Each model has an independent ASP file that can train, save weights, reload
them, and report Accuracy, Macro Precision, and Macro Recall.

## Model Catalog

| Model | ASP file | Architecture and purpose |
|---|---|---|
| Softmax Regression | [`mnist-logistic.asp`](mnist-logistic.asp) | `784 -> 10`, a linear classification baseline |
| Multilayer Perceptron | [`mnist-class.asp`](mnist-class.asp) | `784 -> 128 -> 64 -> 10`, the standard PyTorch-style Module example |
| Residual MLP | [`mnist-resmlp.asp`](mnist-resmlp.asp) | 128-dimensional features and two residual blocks for validating deeper fully connected networks |
| LeNet-style Network | [`mnist-lenet.asp`](mnist-lenet.asp) | Two learnable local patch convolutions and a `120 -> 84 -> 10` classifier |
| CNN | [`mnist-cnn.asp`](mnist-cnn.asp) | Two shared patch convolutions that exploit local spatial structure |
| Vanilla RNN | [`mnist-rnn.asp`](mnist-rnn.asp) | Combines two image rows per step and processes a sequence of length 14 |
| GRU | [`mnist-gru.asp`](mnist-gru.asp) | Update and reset gates mitigate long-term dependency limitations of a basic RNN |
| LSTM | [`mnist-lstm.asp`](mnist-lstm.asp) | Input, forget, and output gates with cell state |
| Transformer | [`mnist-transformer.asp`](mnist-transformer.asp) | Row-sequence embeddings, Self-Attention, and an Encoder Block |
| Vision Transformer | [`mnist-vit.asp`](mnist-vit.asp) | 4x4 image patches, positional embeddings, and a Transformer Encoder |
| MLP-Mixer | [`mnist-mixer.asp`](mnist-mixer.asp) | Alternating token-mixing and channel-mixing MLPs |

[`mnist.asp`](mnist.asp) retains the earlier procedural MLP example. New code
should use [`mnist-class.asp`](mnist-class.asp) as the primary reference. New
models share the data loading, AdamW training, weight reload, and evaluation
pipeline in [`mnist-common.inc`](mnist-common.inc).

Training passes final-layer logits directly to `CrossEntropy`; evaluation
applies Softmax afterward. This follows the numerically stable convention used
by mainstream deep learning frameworks.

## Measured Results

The following results use the fixed random seed `42`, train on the complete
MNIST training set, then evaluate all 10,000 test samples after saving weights,
creating a new model, and reloading the weights. Models use different default
epoch counts and backends, so this table demonstrates trainability and
recognition quality rather than a uniform hardware performance ranking.

| Model | Epochs | Backend | Accuracy | Macro Precision | Macro Recall |
|---|---:|---|---:|---:|---:|
| Softmax Regression | 10 | `cuda:0` | 92.32% | 92.37% | 92.19% |
| Multilayer Perceptron | 5 | `cpu-onednn:0` | 97.51% | 97.52% | 97.47% |
| Residual MLP | 5 | `cpu-onednn:0` | 97.39% | 97.43% | 97.33% |
| LeNet-style Network | 3 | `cpu-onednn:0` | 96.86% | 96.89% | 96.83% |
| CNN | 3 | `cuda:0` | 95.80% | 95.83% | 95.73% |
| Vanilla RNN | 3 | `cpu-onednn:0` | 94.19% | 94.13% | 94.13% |
| GRU | 3 | `cpu-onednn:0` | 96.74% | 96.71% | 96.72% |
| LSTM | 3 | `cpu-onednn:0` | 96.28% | 96.27% | 96.23% |
| Transformer | 1 | `cuda:0` | 72.19% | 71.91% | 71.74% |
| Vision Transformer | 1 | `cuda:0` | 59.04% | 59.79% | 58.23% |
| MLP-Mixer | 1 | `cuda:0` | 90.86% | 90.92% | 90.72% |

## Preparing the Data

Run this command in the current directory:

```sh
chmod +x download-data.sh
./download-data.sh
```

You can also place the data in a separate directory:

```sh
./download-data.sh /data00/openasp-data/mnist
```

The script downloads and extracts the four standard MNIST IDX files without
leaving compressed archives in the source tree.

## Building

Build OpenASP and OpenASP.AI from the repository root:

```sh
make AI=1
```

For the CUDA backend on V100, also run:

```sh
make -C extensions/openasp-ai/backends/cuda CUDA_HOME=/usr/local/cuda CUDA_ARCH=70
```

The OpenASP external-component configuration must load the generated
`extensions/openasp-ai/libopenasp_ai.so` and specify its SHA-256 digest. See
[`../../openasp-ai.yaml.example`](../../openasp-ai.yaml.example) for all
configuration fields.

## Running

CPU example:

```sh
build/bin/openasp \
  -c /absolute/path/openasp-ai.yaml \
  extensions/openasp-ai/examples/mnist/mnist-class.asp \
  --root extensions/openasp-ai/examples/mnist
```

All new models accept the same query parameters:

- `data`: directory containing the four IDX data files; defaults to `data` under the current root.
- `epochs`: number of training epochs; defaults to the model's `DefaultEpochCount`.
- `weights`: weight save and reload path; defaults to `<data>/<ModelSlug>.oaspweights`.

For example, train GRU for three epochs with a separate data directory:

```sh
build/bin/openasp \
  -c /absolute/path/openasp-ai.yaml \
  extensions/openasp-ai/examples/mnist/mnist-gru.asp \
  --root extensions/openasp-ai/examples/mnist \
  --query 'data=/data00/openasp-data/mnist&epochs=3&weights=/data00/openasp-data/mnist/mnist-gru.oaspweights'
```

The same ASP files run on both CPU and CUDA. To switch to CUDA, configure only
the deployment environment:

```sh
export OPENASP_AI_BACKEND=/absolute/path/libopenasp_ai_cuda.so
export OPENASP_AI_BACKEND_SHA256=<lowercase-sha256>
export OPENASP_AI_DEVICE=cuda:0
```

When cuDNN Graph is unsupported on V100, additionally set
`OPENASP_AI_CUDA_CUDNN=0` to use the stable fallback path, then run the same
`openasp` command.

## PyTorch-style ASP Class

[`mnist-class.asp`](mnist-class.asp) wraps its layers in `Class MnistMLP` and
registers `fc1`, `relu1`, `fc2`, `relu2`, and `fc3` through
`OpenASP.AI.Module`, giving it stable StateDict parameter paths.

After training, the example saves `.oaspweights`, creates a new `MnistMLP`,
loads the weights, and uses the new object for inference and precision/recall
statistics:

```sh
build/bin/openasp \
  -c /absolute/path/openasp-ai.yaml \
  extensions/openasp-ai/examples/mnist/mnist-class.asp \
  --root extensions/openasp-ai/examples/mnist \
  --query 'data=/data00/openasp-data/mnist&epochs=5&weights=/data00/openasp-data/mnist/mnist-class.oaspweights'
```

## Output Metrics

The scripts report:

- `accuracy`: correct predictions divided by the total number of test samples.
- Per-class `precision`: `TP / (TP + FP)`.
- Per-class `recall`: `TP / (TP + FN)`.
- `macro_precision`: arithmetic mean of precision for digits 0 through 9.
- `macro_recall`: arithmetic mean of recall for digits 0 through 9.
- Raw per-class `tp`, `fp`, and `fn` counts.

During evaluation, each tensor calls `ToArray()` only once. Probability tensors
are transferred as complete blocks while labels remain on the CPU, avoiding
per-element `Item()` calls and unnecessary label transfers that would cause
frequent GPU synchronization.
