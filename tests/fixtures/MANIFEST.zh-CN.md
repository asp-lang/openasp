[English](MANIFEST.md) | [简体中文](MANIFEST.zh-CN.md)

# Fixture 资源清单

测试资源与测试代码分离。运行时生成的数据库、锁、缓存和日志必须写入
`build/tests/`，不得回写本目录。

| 路径 | 来源与用途 | 使用者 |
|---|---|---|
| `common/*.asp` | OpenASP 自有的最小运行时、GC、FastCGI 和组件场景 | unit、integration、stress |
| `reference/` | 参考 ASP 行为页面集合，用于兼容性和差分验证 | integration、fixture-diff |

完整 ASP 应用及其 MDB 数据库不属于 fixture，统一存放于
`tests/e2e/apps/`。其中 MDB 是应用原始数据或兼容性验证资源，必须保留；
`.eglock`、缓存、日志、IDE 输出及编译生成的 DLL/PDB 不属于测试资源。
