[English](README.md) | [简体中文](README.zh-CN.md)

# Source Modules

`src` is organized by responsibility. The public API remains under `include/`
at the repository root:

| Directory | Responsibility |
| --- | --- |
| `log/` | Process-level logging, level filtering, and daily file rotation |
| `memory/` | Arena and service-mode GC allocators |
| `compiler/` | Lexer, parser, symbol resolution, bytecode, module images, and preprocessing |
| `runtime/` | VBScript value semantics, runtime, VM, and shared internal facilities |
| `engine/` | Page compilation cache, include/transfer, Session/Application coordination, and interactive sessions |
| `web/` | Request/Response model and FastCGI host |
| `storage/` | Shared opcode cache and state persistence |
| `components/` | COM-compatible components, databases, networking, serialization, and MDB support |
| `cmd/` | Entry points for `openasp`, `openasp-fpm`, and `openasp-cli` |

## Large Implementation Files

`runtime/runtime.c` implements runtime state, call dispatch, built-in functions,
and AST execution. `components/component.c` implements native component types,
resource lifecycles, and call dispatch for the full profile. Both contain many
shared private types and `static` helpers, so each remains a single translation
unit instead of being assembled through textual `.inc.c` includes.

New independent features should use ordinary `.c/.h` files and be added to the
`Makefile`. Any future split of these two large implementations must first
define narrow internal module interfaces and ownership boundaries. Do not
reintroduce `.inc.c` files.
