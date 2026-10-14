[English](README.md) | [简体中文](README.zh-CN.md)

# 源码模块

`src` 按职责划分，公共 API 保留在仓库根目录的 `include/`：

| 目录 | 职责 |
| --- | --- |
| `log/` | 进程级日志、级别过滤与按日文件轮转 |
| `memory/` | Arena 与 Service Mode GC allocator |
| `compiler/` | Lexer、Parser、符号解析、字节码、模块镜像与预处理 |
| `runtime/` | VBScript 值语义、运行时、VM 和内部通用设施 |
| `engine/` | 页面编译缓存、include/transfer、Session/Application 协调及交互会话 |
| `web/` | Request/Response 模型与 FastCGI 宿主 |
| `storage/` | 共享 opcode cache 和状态持久化 |
| `components/` | COM 兼容组件、数据库、网络、序列化和 MDB 支持 |
| `cmd/` | `openasp`、`openasp-fpm` 和 `openasp-cli` 命令入口 |

## 大型实现文件

`runtime/runtime.c` 负责运行时状态、调用分派、内建函数和 AST 执行；
`components/component.c` 负责完整模式下的原生组件类型、资源生命周期与调用
分派。二者内部存在大量共享私有类型和 `static` 辅助函数，因此保持为各自独立
的 translation unit，不再通过 `.inc.c` 文本包含拼装。

新增独立能力应使用普通 `.c/.h` 文件并加入 `Makefile`。若后续需要继续拆分这
两个大型实现，应先定义窄化的内部模块接口和所有权边界，禁止重新引入
`.inc.c`。
