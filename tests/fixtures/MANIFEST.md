[English](MANIFEST.md) | [简体中文](MANIFEST.zh-CN.md)

# Fixture Resource Manifest

Test resources are kept separate from test code. Databases, locks, caches, and
logs generated at runtime must be written under `build/tests/` and must not be
written back into this directory.

| Path | Source and purpose | Consumers |
|---|---|---|
| `common/*.asp` | OpenASP-owned minimal runtime, GC, FastCGI, and component scenarios | unit, integration, stress |
| `reference/` | Reference ASP behavior pages for compatibility and differential validation | integration, fixture-diff |

Complete ASP applications and their MDB databases are not fixtures. They live
under `tests/e2e/apps/`. Those MDB files are original application data or
compatibility resources and must be preserved. `.eglock` files, caches, logs,
IDE output, and generated DLL/PDB files are not test resources.
