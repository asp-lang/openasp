[English](README.md) | [Simplified Chinese](README.zh-CN.md)

# openasp-dbg

`openasp-dbg` is a source-level debugger for the OpenASP VM. The debugging hook is located before opcode dispatch
, so breakpoints, single steps, call stacks and variable views all correspond to the actual execution state of the VM.

## Build

```sh
make build/bin/openasp-dbg
make PROFILE=minimal build/minimal/bin/openasp-dbg
```

Both the full `make all` and minimal builds generate this tool.

## Start

Directly debug the ASP source file:

```sh
build/bin/openasp-dbg /srv/www/page.asp --root /srv/www
```

Debug OAC with source code snippet:

```sh
build/bin/openasp /srv/www/page.asp --root /srv/www \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
build/bin/openasp-dbg /tmp/page.oac
```

OAC must be generated with `--gen-dbg-info=on`; the debugger will refuse execution when the source code segment is missing and give
a rebuild recommendation. `--runtime-mode=service` can be used for debugging in the GC service runtime.

Use a command file to perform repeatable debugging:

```sh
build/bin/openasp-dbg page.asp --root /srv/www -x commands.dbg
```

Connect to an FPM worker with remote debugging enabled:

```sh
build/bin/openasp-dbg connect --unix-socket /run/openasp/debug.sock --target 12345
build/bin/openasp-dbg connect --tcp 127.0.0.1:19090 --token "$OPENASP_DEBUG_TOKEN" --target 12345
```

`target` is the worker PID. FPM registers the current worker in the same directory as the debug socket in the form of
`debug.sock.<pid>`; the old PID becomes invalid immediately after the worker restarts, and the new
PID is used to reconnect. Remote mode directly reuses the command set below and the same VM command/core implementation.

## Command

| Command | Function |
| --- | --- |
| `run`, `continue`, `c` | Continue from current stopping point |
| `step`, `s` | Enter the call and execute to the next source line |
| `next`, `n` | Skip the call and execute to the next source line of the current frame |
| `finish` | Execute to the current routine and return |
| `stepi`, `si` | Execute a VM opcode |
| `nexti`, `ni` | Execute a VM opcode beyond the call |
| `until` | Execute to the subsequent source line of the current frame or return |
| `break LOCATION`, `b` | Add a line, `file:line` or function breakpoint |
| `tbreak LOCATION` | Add a breakpoint that is automatically deleted when hit |
| `delete ID` | Delete a breakpoint or watchpoint |
| `enable ID`, `disable ID` | Enable or disable breakpoints |
| `condition ID EXPR` | Set or clear a conditional expression |
| `ignore ID COUNT` | Ignore a specified number of subsequent hits |
| `watch EXPR` | Stop when expression value changes |
| `catch error` | Stop before ASP error returns or resumes execution |
| `backtrace`, `bt` | Display dynamic call stack |
| `frame N`, `up`, `down` | Select call frame |
| `print EXPR`, `p` | Evaluate ASP expression in selected frame |
| `set variable NAME = EXPR` | Modify variables in the selected frame |
| `set follow-fork-mode parent\|child` | Select `Process.Fork` followed by the parent or child process |
| `set detach-on-fork on\|off` | fork Unbind unfollowed processes later, or keep two inferiors at the same time |
| `show follow-fork-mode`, `show detach-on-fork` | Display the current fork debugging strategy |
| `info inferiors` | Display the current process and the associated processes of the latest fork |
| `info locals`, `info args`, `info globals` | Display variables |
| `info breakpoints` | Display breakpoint status, hit and ignore times |
| `info line`, `info opcode` | Display current position or 24-byte opcode |
| `list [LINE]` | Display content near the current source code |
| `disassemble` | Disassemble the current bytecode |
| `source FILE` | Execute debug command file |
| `quit`, `q` | Terminate the debugged program |

Location example:

```text
break 42
break page.asp:42
break CalculateTotal
condition 1 total > 1000
ignore 1 3
```

An empty command will repeat the previous command. Non-interactive input does not print prompts, making testing and automation easier.

## Execution model

- The line breakpoint is bound to the first opcode of the line to avoid repeated hits of internal jumps in the same line.
- `step`, `next` and `finish` simultaneously compare bytecode, PC, source lines and dynamic frame depth.
- Conditional, `print` and watch expressions are evaluated using the formal ASP parser/runtime and do not use a separate
simplified expression language; temporary errors resulting from evaluation do not pollute the `Err` status of the debugged program.
- String assignments are copied to the target runtime lifetime and remain valid after the command buffer is destroyed.
- fork policy defaults to `follow-fork-mode parent` and `detach-on-fork on`. When child
is selected, the parent process is immediately unbound, and the child process inherits breakpoints, watchpoints, and the current execution mode.
- `detach-on-fork off` will preserve both parent and child processes. All inherited debuggers enter the command loop serially via the inter-process control
token, so the same terminal is not read concurrently by multiple processes; each stop displays the
actual PID. The inferior who reaches the stopping point first gets control first.
- `--process-enable=on` must still be passed explicitly when using `OpenASP.Process`.
- FPM remote debugging is turned off by default. When enabled,
`debug-enable: on` and `debug-unix-socket: /run/openasp/debug.sock` must be set.
Use `0600` permissions for the Unix broker and each worker agent respectively.
- TCP broker only allows `127.0.0.1` and a non-empty `debug-token` must be set. The protocol first requires
`auth TOKEN`, then `target PID`; tokens use a fixed time comparison. Do not expose TCP broker
to the network.
- A worker can only accept one debug console at a time. Control side disconnects will be recognized and cleaned up before the next request, and
will not cause normal FastCGI requests to fail with EOF.
- The debug source is the canonical source parsed by the compiler; the per-include raw file mapping for ABI 20 is not part of the
current OAC ABI 19 implementation.
