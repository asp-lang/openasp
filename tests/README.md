[English](README.md) | [简体中文](README.zh-CN.md)

# OpenASP Test Directory

| Directory | Responsibility | Default command |
|---|---|---|
| `unit/` | C-level compiler, runtime, component, and storage unit tests | `make test-unit` |
| `integration/` | CLI, FastCGI, debugger, extension, process, and KV integration tests | `make test-integration` |
| `conformance/` | ASP language and component compatibility cases | `make test-conformance` |
| `e2e/` | Complete ASP applications and end-to-end runners | `make test-e2e` |
| `stress/` | GC, fork, stability, and memory stress tests | `make test-stress` |
| `benchmarks/` | Performance runners and OpenASP/PHP comparison workloads | `make test-benchmark` |
| `fixtures/` | Small, deterministic test inputs | See [`fixtures/MANIFEST.md`](fixtures/MANIFEST.md) |
| `support/` | Shared Python paths, FastCGI, and process utilities | Not run separately |

`make test` runs only unit, integration, and conformance tests. `make test-all`
also runs e2e and stress tests. Benchmarks always require an explicit command
so generated performance data cannot contaminate correctness test results.

All temporary files, state databases, caches, sockets, and generated files must
be written under `build/tests/`.
