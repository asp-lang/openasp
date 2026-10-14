[English](README.md) | [简体中文](README.zh-CN.md)

# OpenASP 测试目录

| 目录 | 职责 | 默认命令 |
|---|---|---|
| `unit/` | C 层编译器、运行时、组件和存储单元测试 | `make test-unit` |
| `integration/` | CLI、FastCGI、调试器、扩展、进程和 KV 集成 | `make test-integration` |
| `conformance/` | ASP 语言及组件兼容性用例 | `make test-conformance` |
| `e2e/` | 完整 ASP 应用与端到端 runner | `make test-e2e` |
| `stress/` | GC、fork、稳定性和内存压力测试 | `make test-stress` |
| `benchmarks/` | 性能 runner 及 OpenASP/PHP 对照 workload | `make test-benchmark` |
| `fixtures/` | 小型、确定性的测试输入 | 见 [`fixtures/MANIFEST.zh-CN.md`](fixtures/MANIFEST.zh-CN.md) |
| `support/` | Python runner 共用路径、FastCGI 和进程工具 | 不单独运行 |

`make test` 只执行 unit、integration、conformance。`make test-all` 在此基础上
增加 e2e 和 stress；benchmark 始终显式运行，避免污染正确性测试结果。

所有临时文件、状态库、缓存、socket 和生成文件必须写入 `build/tests/`。
