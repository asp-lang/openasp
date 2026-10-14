[English](README.md) | [简体中文](README.zh-CN.md)

# openasp-dis

`openasp-dis` 用于校验并反汇编 `openasp` 或 OpenASP FPM 生成的 `.oac`
字节码镜像。
它复用运行时的镜像解码和字节码验证逻辑，损坏、截断、ABI 不匹配、校验和错误
或指令不合法的文件会被拒绝。

## 构建

```sh
make build/bin/openasp-dis
```

完整构建和 minimal profile 也会分别生成：

```text
build/bin/openasp-dis
build/minimal/bin/openasp-dis
```

## 使用

直接编译、执行一个页面并保存 VM 实际执行的同一份 bytecode：

```sh
build/bin/openasp /srv/www/page.asp --root /srv/www \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
build/bin/openasp-dis /tmp/page.oac
```

`--emit-oac FILE` 和 `--emit-oac=FILE` 等价，目标文件必须以 `.oac` 结尾。
写入使用同目录临时文件和原子替换，不会留下可被误读的半截镜像。
`--gen-dbg-info` 默认为 `off`；设为 `on` 时 OAC 才包含规范化源码。

FPM 的磁盘二级缓存也会生成 OAC。首先在 `openasp-fpm` 配置文件中启用源码
调试信息：

```yaml
gen-dbg-info: on
opcache-dir: /tmp/openasp-opcache
```

也可以通过命令行启用：

```sh
build/bin/openasp-fpm --gen-dbg-info=on --opcache-dir /tmp/openasp-opcache
```

页面至少被请求一次后，FPM 会在 `opcache-dir` 中生成以 16 位缓存键哈希命名的
`.oac` 文件。使用以下命令反汇编：

```sh
build/bin/openasp-dis /path/to/cache/0123456789abcdef.oac
```

不带参数或使用 `--help` 可以查看命令格式：

```text
Usage: openasp-dis <file.oac>
```

## 输出示例

以下示例来自真实编译的 ASP 页面：

```asp
<%
Function Classify(value)
    Select Case value
        Case "alpha", "beta", "gamma"
            Classify = "first"
        Case "delta", "epsilon", "zeta"
            Classify = "second"
        Case "eta", "theta", "iota"
            Classify = "third"
        Case Else
            Classify = "other"
    End Select
End Function

Response.Write Classify("theta")
%>
```

反汇编结果：

```text
OpenASP OAC disassembly
File: /tmp/openasp-opcache/6d30fb6d901ff936.oac
Size: 3528 bytes
Source: "P:D:/srv/www/select_case_metadata.asp"
Instructions: 66
Max stack: 3
Loops: 0
Debug info: present

Disassembly of section .oasp_bytecode:
Dump of assembler code for function __asp_page:
   15	Response.Write Classify("theta")
   0x00000000000005f0 <__asp_page+0>:	05 00 00 00 0f 00 00 00 ff ff ff ff ff ff ff ff 00 00 00 00 00 00 00 00	push_literal	$const[0]  # String("theta")
   0x0000000000000608 <__asp_page+24>:	0b 00 00 00 0f 00 00 00 01 00 00 00 ff ff ff ff 00 00 00 00 01 00 00 00	call_name	$name[0]  # "Classify"
   0x0000000000000620 <__asp_page+48>:	4d 00 00 00 0f 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff 00 00 00 00	write_value
   0x0000000000000680 <__asp_page+144>:	3f 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff 00 00 00 00	halt
End of assembler dump.

Dump of assembler code for function Classify:
    3	    Select Case value
   0x0000000000000698 <Classify+0>:	08 00 00 00 03 00 00 00 ff ff ff ff 40 00 00 00 01 00 00 00 00 00 00 00	push_local_slot	%local[1]
    4	        Case "alpha", "beta", "gamma"
   0x00000000000006c8 <Classify+48>:	05 00 00 00 04 00 00 00 ff ff ff ff 40 00 00 00 02 00 00 00 00 00 00 00	push_literal	$const[2]  # String("alpha")
   0x00000000000006f8 <Classify+96>:	37 00 00 00 03 00 00 00 15 00 00 00 40 00 00 00 ff ff ff ff 00 00 00 00	jump_if_true	0x00000000000007e8 <Classify+336>
    5	            Classify = "first"
   0x0000000000000800 <Classify+360>:	05 00 00 00 05 00 00 00 ff ff ff ff 40 00 00 00 05 00 00 00 00 00 00 00	push_literal	$const[5]  # String("first")
   0x0000000000000818 <Classify+384>:	2d 00 00 00 05 00 00 00 ff ff ff ff 40 00 00 00 00 00 00 00 00 00 00 00	assign_local_slot	%local[0]
End of assembler dump.
```

## 输出格式

文件头提供 OAC 的整体信息：

- `File`：正在读取的 OAC 文件。
- `Size`：OAC 文件总字节数。
- `Source`：缓存类型、调试模式和原始 ASP 路径。`P:D:` 表示页面缓存且包含调试信息。
- `Instructions`：指令总数。
- `Max stack`：验证后的 VM 最大操作数栈深度。
- `Loops`：编译器识别的循环数量。
- `Debug info`：是否保存了规范化 ASP 源码。

文件头之后依次显示依赖、模块、名称、常量、全局槽、例程和类元数据，最后是
`.oasp_bytecode` 指令区。

每条指令采用 GDB `disassemble /m` 与 `disassemble /r` 风格：

```text
0x00000000000006f8 <Classify+96>:  37 ... 00  jump_if_true  0x00000000000007e8 <Classify+336>
```

- `0x00000000000006f8`：指令在 OAC 文件中的真实字节偏移，不是受 ASLR 影响的进程地址。
- `<Classify+96>`：当前符号以及相对例程入口的字节偏移。
- `37 ... 00`：OAC 中原样保存的完整 24 字节指令编码，以十六进制显示。
- `jump_if_true`：OpenASP VM opcode 助记符。
- `0x... <Classify+336>`：跳转目标的 OAC 偏移及符号化位置。
- `# String("alpha")`：常量、名称或全局槽 operand 的解析结果。

启用调试信息后，源码行号和规范化 ASP 源码显示在对应指令组之前：

```text
    4        Case "alpha", "beta", "gamma"
```

`__asp_page` 表示页面顶层代码。函数、Sub、Property 和类代码使用自身符号名称
分块，并以 `Dump of assembler code` 和 `End of assembler dump.` 标记边界。

## 指令编码

这里的“机器码”是 OpenASP VM 实际执行的固定 24 字节 opcode 编码，不是
ARM64 或 x86-64 CPU 原生指令。字节布局为：

| 字节偏移 | 宽度 | 字段 |
| --- | ---: | --- |
| `0x00` | 1 | `opcode` |
| `0x01` | 1 | `flags` |
| `0x02` | 2 | `reserved` |
| `0x04` | 4 | `line` |
| `0x08` | 4 | `target` |
| `0x0c` | 4 | `error_target` |
| `0x10` | 4 | `operand` |
| `0x14` | 4 | `slot` |

十六进制字节与 OAC 文件内容逐字节一致。地址列可以直接用作文件偏移，配合
`xxd` 等工具核对：

```sh
xxd -g 1 -s 0x6f8 -l 24 /tmp/openasp-opcache/6d30fb6d901ff936.oac
```

## 无调试信息

`gen-dbg-info` 默认为 `off`。此时 OAC 不包含源码段，反汇编器仍会显示完整的
符号、十六进制编码、opcode 和操作数，但文件头显示：

```text
Debug info: unavailable
```

指令区不会显示 ASP 源码行。

## 校验与错误

反汇编器在输出前复用运行时解码器检查 magic、ABI、范围、对齐、checksum 和
字节码合法性。损坏、截断或版本不匹配的文件会返回非零状态；当前错误会列出
可能失败的校验类别，但解码器尚不区分具体失败阶段。

FPM 二级 opcode 缓存中的 `.oac` 文件名是缓存键的哈希值；
`openasp --emit-oac` 则使用用户明确指定的文件名。
缓存目录由 `opcache-dir` 配置，默认位于系统临时目录。
