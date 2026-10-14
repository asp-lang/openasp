[English](README.md) | [简体中文](README.zh-CN.md)

# openasp-dbg

`openasp-dbg` 是 OpenASP VM 的源码级调试器。调试 hook 位于 opcode dispatch
之前，因此断点、单步、调用栈和变量视图均对应 VM 实际执行状态。

## 构建

```sh
make build/bin/openasp-dbg
make PROFILE=minimal build/minimal/bin/openasp-dbg
```

完整的 `make all` 和 minimal 构建都会生成该工具。

## 启动

直接调试 ASP 源文件：

```sh
build/bin/openasp-dbg /srv/www/page.asp --root /srv/www
```

调试带源码段的 OAC：

```sh
build/bin/openasp /srv/www/page.asp --root /srv/www \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
build/bin/openasp-dbg /tmp/page.oac
```

OAC 必须使用 `--gen-dbg-info=on` 生成；缺少源码段时调试器会拒绝执行并给出
重建建议。`--runtime-mode=service` 可用于在 GC service runtime 中调试。

使用命令文件执行可重复调试：

```sh
build/bin/openasp-dbg page.asp --root /srv/www -x commands.dbg
```

连接到启用远程调试的 FPM worker：

```sh
build/bin/openasp-dbg connect --unix-socket /run/openasp/debug.sock --target 12345
build/bin/openasp-dbg connect --tcp 127.0.0.1:19090 --token "$OPENASP_DEBUG_TOKEN" --target 12345
```

`target` 是 worker PID。FPM 在调试 socket 同目录以
`debug.sock.<pid>` 形式注册当前 worker；worker 重启后旧 PID 立即失效，使用新
PID 重连。远程模式直接复用下文的命令集和同一个 VM command/core 实现。

## 命令

| 命令 | 作用 |
| --- | --- |
| `run`, `continue`, `c` | 从当前停点继续 |
| `step`, `s` | 进入调用并执行到下一源码行 |
| `next`, `n` | 越过调用并执行到当前帧下一源码行 |
| `finish` | 执行到当前例程返回 |
| `stepi`, `si` | 执行一个 VM opcode |
| `nexti`, `ni` | 越过调用执行一个 VM opcode |
| `until` | 执行到当前帧后续源码行或返回 |
| `break LOCATION`, `b` | 添加行、`file:line` 或函数断点 |
| `tbreak LOCATION` | 添加命中后自动删除的断点 |
| `delete ID` | 删除断点或 watchpoint |
| `enable ID`, `disable ID` | 启用或禁用断点 |
| `condition ID EXPR` | 设置或清除条件表达式 |
| `ignore ID COUNT` | 忽略后续指定次数的命中 |
| `watch EXPR` | 表达式值变化时停止 |
| `catch error` | 在 ASP 错误返回或恢复执行前停止 |
| `backtrace`, `bt` | 显示动态调用栈 |
| `frame N`, `up`, `down` | 选择调用帧 |
| `print EXPR`, `p` | 在选中帧中计算 ASP 表达式 |
| `set variable NAME = EXPR` | 修改选中帧中的变量 |
| `set follow-fork-mode parent\|child` | 选择 `Process.Fork` 后跟随父进程或子进程 |
| `set detach-on-fork on\|off` | fork 后解绑未跟随进程，或同时保留两个 inferior |
| `show follow-fork-mode`, `show detach-on-fork` | 显示当前 fork 调试策略 |
| `info inferiors` | 显示当前进程和最近一次 fork 的关联进程 |
| `info locals`, `info args`, `info globals` | 显示变量 |
| `info breakpoints` | 显示断点状态、命中和忽略次数 |
| `info line`, `info opcode` | 显示当前位置或 24 字节 opcode |
| `list [LINE]` | 显示当前源码附近内容 |
| `disassemble` | 反汇编当前 bytecode |
| `source FILE` | 执行调试命令文件 |
| `quit`, `q` | 终止被调试程序 |

位置示例：

```text
break 42
break page.asp:42
break CalculateTotal
condition 1 total > 1000
ignore 1 3
```

空命令会重复上一条命令。非交互输入不会打印提示符，便于测试和自动化。

## 执行模型

- 行断点绑定到该行的第一个 opcode，避免同一行内部跳转重复命中。
- `step`、`next` 和 `finish` 同时比较 bytecode、PC、源码行与动态帧深度。
- 条件、`print` 和 watch 表达式使用正式 ASP parser/runtime 求值，不使用单独的
  简化表达式语言；求值产生的临时错误不会污染被调试程序的 `Err` 状态。
- 字符串赋值会复制到目标 runtime 生命周期，命令缓冲区销毁后仍然有效。
- fork 策略默认为 `follow-fork-mode parent` 和 `detach-on-fork on`。选择 child
  时父进程立即解绑，子进程继承断点、watchpoint 和当前执行模式。
- `detach-on-fork off` 会同时保留父子进程。所有继承的调试器通过进程间控制
  令牌串行进入命令循环，因此同一终端不会被多个进程并发读取；每个停点均显示
  实际 PID。先到达停点的 inferior 先获得控制权。
- 使用 `OpenASP.Process` 时仍必须显式传入 `--process-enable=on`。
- FPM 远程调试默认关闭。启用时必须设置
  `debug-enable: on` 与 `debug-unix-socket: /run/openasp/debug.sock`。
  Unix broker 和每个 worker agent 分别使用 `0600` 权限。
- TCP broker 仅允许 `127.0.0.1`，并且必须设置非空 `debug-token`。协议首先要求
  `auth TOKEN`，再要求 `target PID`；令牌使用固定时间比较。不要把 TCP broker
  暴露给网络。
- 一个 worker 同时只接受一个调试控制端。控制端断开会在下一次请求前被识别并清理，
  不会让正常 FastCGI 请求因 EOF 失败。
- 调试源码是编译器解析的规范化源码；ABI 20 的逐 include 原始文件映射不属于
  当前 OAC ABI 19 实现。
