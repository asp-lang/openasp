# OpenASP：高性能低内存的面向 Linux、Unix 和 MacOS 的经典 ASP

<p align="center" id="openasp-top">
  <img src="assets/openasp-logo.png" alt="OpenASP" width="500">
</p>

<p align="center">
  <a href="#从源码编译"><img src="assets/badge-compiler.png" alt="编译器：C11" height="20"></a>
  <a href="https://spdx.org/licenses/MIT.html"><img src="assets/badge-license.png" alt="许可证：MIT" height="20"></a>
  <a href="#为什么选择-openasp"><img src="assets/badge-platforms.png" alt="平台：Linux、MacOS 和 Unix" height="20"></a>
</p>

<p align="center">
  <a href="#安装二进制发行包">安装</a> |
  <a href="#从源码编译">编译</a> |
  <a href="#hello-world">Hello World</a> |
  <a href="#命令行工具">命令行工具</a> |
  <a href="#使用-nginx-部署">Nginx</a> |
  <a href="#兼容性与组件">兼容性</a> |
  <a href="https://asp-lang.org">官方网站</a> |
  <a href="README.md">English</a>
</p>

OpenASP 的官方主页是 **[https://asp-lang.org](https://asp-lang.org)**。有任何问题、建议或想法，请随时通过官方网站反馈。

OpenASP 是一个使用纯 C11 实现的高性能、低内存经典 ASP 解释器和常驻运行时。它让现有的 VBScript/ASP 应用无需 Windows Server 和 IIS，即可原生运行在 Linux、Unix 和 MacOS 上。OpenASP 提供独立页面渲染、持久交互环境、多进程 FastCGI 服务、兼容 ADO 的数据访问、原生组件、字节码缓存和调试工具。详细吞吐和内存数据请参阅 [OpenASP 和 PHP 的性能和内存压测报告](#openasp和php的性能和内存对比)。

## 为什么选择 OpenASP

OpenASP 不只是一个兼容层。它在保留经典 ASP 与 VBScript 使用习惯和语义的同时，将依赖 Windows 的托管环境替换为面向现代生产环境的可移植运行时。现有应用可以迁移到 Linux、Unix 或 MacOS，而不必为了更换操作系统去重写业务语言、Web 框架或在 ASP 代码中加入平台适配逻辑。

OpenASP 还为经典 ASP 扩展了更多现代原生能力。ASP 代码可以通过 `Server.CreateObject` 直接使用 MySQL/MariaDB、SQLite、MDB、Redis、Memcache、HTTP/WebSocket、JSON、密码学和 OpenAI 兼容 API，无需额外部署其他语言编写的桥接服务。

| 传统经典 ASP | OpenASP |
| --- | --- |
| 依赖 Windows Server | 原生运行于 Linux、Unix 和 MacOS |
| 由 IIS 托管 | 可独立运行或接入 Nginx/FastCGI |
| 依赖 Windows COM 组件 | 提供可移植的原生 C 组件 |
| 平台迁移逻辑侵入 ASP 业务代码 | 兼容性适配集中在引擎内部 |
| 接入现代服务 | 原生扩展 Redis、MySQL/MariaDB 和 OpenAI 等现代功能 |

## 安装二进制发行包

OpenASP 发行包使用以下命名：

| 平台 | 压缩包 |
| --- | --- |
| Linux x86-64 | [`openasp-0.2.0-linux-x86-64.tar.gz`](https://github.com/asp-lang/openasp/releases/download/v-0.2.0/openasp-0.2.0-linux-x86-64.tar.gz) |
| MacOS Apple Silicon | [`openasp-0.2.0-macos-arm64.zip`](https://github.com/asp-lang/openasp/releases/download/v-0.2.0/openasp-0.2.0-macos-arm64.zip) |

从项目发布页面或 [https://asp-lang.org](https://asp-lang.org) 下载对应平台的压缩包，然后解压：

```sh
# Linux x86-64
tar -xzf openasp-0.2.0-linux-x86-64.tar.gz

# MacOS Apple Silicon
unzip openasp-0.2.0-macos-arm64.zip
```

每个发行包都在 `bin/` 中提供六个程序，包括隔离扩展使用的 `openasp-component-host`。核心扩展 SDK 位于 `include/`；OpenASP.AI 动态库、配置示例、文档及带命名空间的头文件统一位于 `extensions/openasp-ai/`。将 `bin/` 加入 `PATH` 并验证安装：

```sh
export PATH="/path/to/openasp-0.2.0-linux-x86-64/bin:$PATH"
openasp --help
```

在 Apple Silicon 上安装时，请将路径替换为实际解压出的 MacOS 目录。

## 从源码编译

OpenASP 需要 C11 编译器、`make`、`pkg-config`、PCRE2、libxml2、libcurl、SQLite、json-c、OpenSSL、MySQL/MariaDB 客户端库和 mdbtools。

### Ubuntu 24.04

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential pkg-config \
    libpcre2-dev libxml2-dev libcurl4-openssl-dev libsqlite3-dev \
    libjson-c-dev libssl-dev libmariadb-dev mdbtools-dev \
    libglib2.0-dev gettext
```

### MacOS

安装 Xcode 命令行工具和 Homebrew 依赖：

```sh
xcode-select --install
brew install pkg-config pcre2 libxml2 curl sqlite json-c openssl@3 \
    mariadb-connector-c mdbtools
```

### 编译与测试

在仓库根目录执行：

```sh
make clean
make all
make test
```

五个可执行程序会写入 `build/bin/`。完整构建包含所有原生组件；精简构建只保留核心运行时、持久状态、SQLite、json-c 和 `Scripting.Dictionary`：

```sh
make minimal
make minimal-test
```

其他验证命令：

```sh
make test-all
make external-test
make fixture-diff
make asan
```

生成发行包：

```sh
scripts/make-release.sh macos
scripts/make-release.sh linux
scripts/make-release.sh all
```

压缩包会写入 `release/`。在 Apple Silicon 上可通过本地 `ubuntu:24.04` Docker 镜像和 QEMU 构建 Linux x86-64 发行包。

## Hello World

创建站点目录，将以下页面保存为 `/tmp/openasp-site/HelloWorld.asp`：

```asp
<%@ Language=VBScript %>
<%
Response.ContentType = "text/html; charset=utf-8"

Dim name
name = Request.QueryString("name")
If name = "" Then name = Request.Form("name")
If name = "" Then name = "World"
%>
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>OpenASP Hello World</title>
</head>
<body>
  <h1>Hello, <%= Server.HTMLEncode(name) %>!</h1>
</body>
</html>
```

无需 Web 服务器即可直接渲染：

```sh
mkdir -p /tmp/openasp-site
openasp /tmp/openasp-site/HelloWorld.asp \
    --root /tmp/openasp-site \
    --query "name=OpenASP"
```

如果使用源码构建，请将 `openasp` 替换为 `./build/bin/openasp`。输出中包含：

```html
<h1>Hello, OpenASP!</h1>
```

独立运行器也可以模拟表单请求：

```sh
openasp /tmp/openasp-site/HelloWorld.asp \
    --root /tmp/openasp-site \
    --method POST \
    --body "name=OpenASP"
```

## 命令行工具

| 程序 | 用途 |
| --- | --- |
| `openasp` | 渲染 ASP 页面、模拟请求并导出 OAC 字节码 |
| `openasp-fpm` | 通过同步 worker 进程提供 FastCGI 服务 |
| `openasp-cli` | 启动持久的 VBScript/OpenASP 交互会话 |
| `openasp-dis` | 校验并反汇编 `.oac` 字节码镜像 |
| `openasp-dbg` | 在源码和 opcode 层面调试 ASP 或 OAC |

### `openasp`

```sh
openasp page.asp --root /srv/www/site --query "id=42"
openasp page.asp --root /srv/www/site \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
openasp service.asp --root /srv/www/site \
    --runtime-mode=service --gc-heap-limit=256M --gc-diagnostics
```

默认的 `service` runtime 使用非移动分代 GC。显式指定 `--runtime-mode=batch` 会使用请求 Arena。高风险进程能力默认关闭，只有传入 `--process-enable=on` 或 `--shell-enable=on` 才会启用。

### `openasp-cli`

```sh
openasp-cli --root /srv/www/site
```

变量、函数、类、`Session` 和 `Application` 会在多次输入之间保留。输入 `? expression` 打印表达式结果，`:help` 查看命令，`:quit` 退出。交互终端支持方向键、Home/End、Backspace 和 Delete。

在顶层提示符中，以 `!` 开头的输入会将其余文本交给 `$SHELL` 执行，并将标准输出和标准错误实时显示在终端：

```text
OpenASP> !pwd
/srv/www/site
```

`openasp-cli` 默认启用 Shell 命令，可通过 `--shell-enable=off` 关闭；这一默认值不会改变其他 OpenASP 命令的 Shell 安全策略。执行时只显示子命令的标准输出和标准错误，并保留子命令自身产生的颜色。交互终端使用青色显示 `OpenASP>` 提示符。重定向输出、`TERM=dumb` 或设置 `NO_COLOR=1` 时禁用提示符格式。

### `openasp-dis`

生成 OAC 镜像，并查看其元数据、源码映射、符号和 24 字节 VM 指令：

```sh
openasp page.asp --root /srv/www/site \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
openasp-dis /tmp/page.oac
```

输出格式和校验规则见 [`tools/openasp-dis/README.zh-CN.md`](tools/openasp-dis/README.zh-CN.md)。

### `openasp-dbg`

调试 ASP 源文件：

```sh
openasp-dbg page.asp --root /srv/www/site
```

常用命令包括 `break`、`run`、`continue`、`step`、`next`、`finish`、`backtrace`、`print`、`watch`、`list` 和 `disassemble`。调试器也可直接打开通过 `--gen-dbg-info=on` 创建的 OAC 文件。

本地调试和 FPM 远程调试说明见 [`tools/openasp-dbg/README.zh-CN.md`](tools/openasp-dbg/README.zh-CN.md)。

### `openasp-fpm`

在 loopback 地址启动开发用 FastCGI 服务：

```sh
openasp-fpm \
    --root /srv/www/example \
    --host 127.0.0.1 \
    --port 9000 \
    --workers 4 \
    --max-requests 10000 \
    --opcache-memory 64 \
    --log-file stderr \
    --log-level info
```

每个 worker 同一时刻处理一个 FastCGI 连接。所需并发数直接通过 `--workers` 配置，最大值为 256。也可以从 YAML 读取配置：

```sh
openasp-fpm -c /etc/openasp-fpm.yaml
```

配置优先级为：命令行 > YAML > 内置默认值。完整示例见 [`examples/openasp-fpm.yaml`](examples/openasp-fpm.yaml)。

## 使用 Nginx 部署

### 1. 启动 OpenASP FastCGI

首次部署时，请先创建 `/var/cache/openasp` 并授予 FPM 服务账户写权限，然后将 FastCGI 绑定到 loopback：

```sh
openasp-fpm \
    --root /srv/www/example \
    --host 127.0.0.1 \
    --port 9000 \
    --workers 4 \
    --opcache-memory 64 \
    --opcache-dir /var/cache/openasp \
    --opcache-validate-timestamps=off \
    --log-file stderr
```

FPM 的 `--root` 与 Nginx 的 `root` 必须指向同一个物理目录。

### 2. 配置 Nginx

创建 `/etc/nginx/conf.d/openasp.conf`：

```nginx
server {
    listen 8080;
    server_name _;

    root /srv/www/example;
    index default.asp;
    client_max_body_size 32m;

    location / {
        try_files $uri $uri/ /default.asp?$query_string;
    }

    location ~ \.asp$ {
        try_files $uri =404;
        include /etc/nginx/fastcgi_params;

        fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
        fastcgi_param SCRIPT_NAME     $fastcgi_script_name;
        fastcgi_param DOCUMENT_ROOT   $document_root;
        fastcgi_param PATH_INFO       "";
        fastcgi_param PATH_TRANSLATED $document_root$fastcgi_script_name;

        fastcgi_pass 127.0.0.1:9000;
        fastcgi_read_timeout 120s;
    }

    location ~* \.(asa|inc)$ {
        deny all;
    }
}
```

Homebrew Nginx 的 `fastcgi_params` 通常位于 `/opt/homebrew/etc/nginx/fastcgi_params`。

校验配置、重载 Nginx，并请求 Hello World 页面：

```sh
sudo nginx -t
sudo nginx -s reload
curl 'http://127.0.0.1:8080/HelloWorld.asp?name=OpenASP'
```

如果使用 Unix socket，请通过 `--unix-socket /run/openasp/openasp-fpm.sock` 启动 FPM，将 `fastcgi_pass` 改为 `unix:/run/openasp/openasp-fpm.sock`，并确保 Nginx 与 OpenASP 服务账户都能访问 socket 目录。OpenASP 创建的 socket 权限为 `0660`。

### 生产环境检查清单

- 使用非特权账户运行 Nginx 和 `openasp-fpm`。
- TCP FastCGI 只监听 loopback，或者使用受权限保护的 Unix socket。
- 除非可信应用确实需要，否则保持 `shell-enable` 和 `process-enable` 关闭。
- 除明确的上传、状态、缓存和日志目录外，站点目录应保持只读。
- 使用 systemd、launchd 或其他服务管理器守护 `openasp-fpm`。
- 开发环境保持时间戳校验；生产环境关闭后，部署新代码时必须重启 worker。
- 根据业务设置请求体、响应、Header、超时和内存限制。

## 兼容性与组件

OpenASP 直接实现经典 ASP 对象模型：

| 领域 | 支持范围 |
| --- | --- |
| VBScript | 过程、类、数组、`ByRef`、错误处理、`On Error Resume Next`、内置函数 |
| ASP 对象 | `Request`、`Response`、`Server`、`Session`、`Application`、`Err` |
| 请求数据 | QueryString、Form、Cookie、ServerVariables、Header、`BinaryRead` |
| 响应 | 缓冲、Header、Cookie、状态码、重定向、文本和二进制输出 |
| Include | 经典 file include 和 virtual include |
| 数据访问 | SQLite、MySQL/MariaDB、Jet 4/MDB、Recordset、Command、参数化查询 |

可移植的原生组件包括：

- `Scripting.Dictionary`、`Scripting.FileSystemObject` 和 `VBScript.RegExp`
- `ADODB.Connection`、`ADODB.Recordset`、`ADODB.Command` 和 `ADODB.Stream`
- XML DOM、XMLHTTP、HTTP、JSON 和 Crypto
- Socket、WebSocket/WSS、Redis 和 Memcache
- OpenAI 兼容 API
- 默认关闭的 `OpenASP.Process` 和 `Shell.Application`

`OpenASP.OpenAI` 支持基于拉取模式的 SSE 流式请求，可用于 Chat Completions、Responses 和自定义 OpenAI 兼容 endpoint：

```asp
<%
Response.Buffer = False
Set ai = Server.CreateObject("OpenASP.OpenAI")
ai.APIKey = Application("OPENAI_API_KEY")
ai.Model = "gpt-4o-mini"
ai.IdleTimeout = 30000
Set stream = ai.OpenChatStream("解释原生 ASP 运行时")
Do While stream.Read
    If Len(stream.Delta) > 0 Then
        Response.Write stream.Delta
        Response.Flush
    End If
Loop
If stream.ErrorCode <> 0 Then Response.Write stream.ErrorMessage
stream.Close
%>
```

Responses API 使用 `OpenResponsesStream(input)`，自定义 endpoint 使用 `OpenStream(endpoint, payload)`。每次成功的 `Read()` 只发布一个完整 SSE 事件，并通过 `Delta`、`Data`、`EventType`、`FinishReason` 和 `Usage` 暴露数据。遇到 `[DONE]`、EOF、`Close()` 或错误时，`Read()` 返回 `False`；可通过 `Completed`、`ErrorCode`、`ErrorMessage`、`Status` 和 `ResponseBytes` 区分结果。`Timeout`、`IdleTimeout`、`MaxEventBytes`、`MaxBufferedBytes` 和 `MaxResponseBytes` 分别限制总耗时、空闲时间、单事件大小、待处理输入和完整响应大小。待处理输入达到上限时，拉取模型会暂停 libcurl，因此 ASP 消费较慢也不会导致无界缓冲。

可以使用基于 libcurl Multi 的 `OpenASP.HttpMulti`，在单个 worker 线程内并发执行多个出站 HTTP 请求：

```asp
<%
Set multi = Server.CreateObject("OpenASP.HttpMulti")
Set first = multi.CreateRequest("GET", "https://api-a.example/data")
Set second = multi.CreateRequest("POST", "https://api-b.example/query")
second.Body = "{""id"":123}"
second.SetHeader "Content-Type", "application/json"
multi.MaxConcurrency = 8
multi.Timeout = 10000
multi.Execute
If first.ErrorCode = 0 Then Response.Write first.ResponseText
%>
```

每个请求都提供 `Status`、`Completed`、`ErrorCode`、`ErrorMessage`、`ResponseText`、`ContentType`、`Timeout`、`TLSVerify` 和 `CAFile`。单个传输失败不会取消同一批次中已经成功的请求。

可以使用 `OpenASP.RedisMulti` 并发访问多个 Redis endpoint。同一客户端的命令会自动组成 Pipeline，不同客户端在 Linux 上由 `epoll`、在 macOS 上由 `kqueue` 并发驱动：

```asp
<%
Set multi = Server.CreateObject("OpenASP.RedisMulti")
Set redisA = multi.CreateClient("Host=10.0.0.11;Port=6379;Database=0;Timeout=2000")
Set redisB = multi.CreateClient("Host=10.0.0.12;Port=6379;Database=2;Timeout=2000")
Set user = redisA.CreateCommand("HGETALL", "user:123")
Set orders = redisA.CreateCommand("LRANGE", "user:123:orders", 0, 19)
Set stock = redisB.CreateCommand("HGET", "product:456", "stock")
multi.MaxConcurrency = 8
multi.Timeout = 5000
multi.MaxRequestBytes = 4194304
multi.MaxResponseBytes = 16777216
multi.Execute
If user.ErrorCode = 0 Then values = user.Value
%>
```

命令结果提供 `Value`、`Completed`、`ErrorCode`、`ErrorMessage` 和 `MaybeExecuted`。连接失败与 Redis 命令错误只影响对应客户端或命令。事务、订阅和阻塞命令无法安全复用 Pipeline 连接，因此组件会直接拒绝这些命令。

`OpenASP.MemcacheMulti` 为 Memcache 文本协议提供相同的多 endpoint 并发与单客户端 Pipeline 模型：

```asp
<%
Set multi = Server.CreateObject("OpenASP.MemcacheMulti")
Set cacheA = multi.CreateClient("Host=10.0.0.21;Port=11211;Timeout=2000")
Set cacheB = multi.CreateClient("Host=10.0.0.22;Port=11211;Timeout=2000")
Set profile = cacheA.CreateCommand("gets", "user:123")
Set stored = cacheA.CreateCommand("set", "session:123", "active", 0, 300)
Set stock = cacheB.CreateCommand("get", "product:456:stock")
multi.MaxConcurrency = 8
multi.Timeout = 5000
multi.Execute
If profile.ErrorCode = 0 Then Response.Write profile.Value
%>
```

当前支持 `get`、`gets`、`set`、`add`、`replace`、`append`、`prepend`、`cas`、`delete`、`touch`、`incr`、`decr`、`version` 和 `flush_all`。命令结果除通用状态属性外，还通过 `Flags` 与 `Cas` 暴露读取结果的 Memcache 元数据。该组件暂不支持 TLS 和 Memcache 二进制协议。

外部原生扩展可通过 Extension ABI v2 以进程内或隔离模式加载，详见
[SDK 指南](docs/extensions.zh-CN.md)与[实现设计](docs/ext-design.zh-CN.md)。

OpenASP.AI 以 C11 外部扩展形式位于
[`extensions/openasp-ai`](extensions/openasp-ai/README.zh-CN.md)。执行 `make AI=1`
可同时构建引擎和 reference CPU AI runtime，`make ai-test` 运行专项测试。该扩展包含
Tensor 与训练 API、计算图编译、Transformer/KV Cache、异步执行、Profiler 及设计文档
规定的模型持久化格式；加速器实现通过独立的 Backend ABI 1.1 插件提供。

## 架构与文档

ASP 模板和 include 会被解析并编译为经过校验的字节码。独立运行器与 service runtime 直接执行字节码；FPM 将不可变编译模块存入跨进程共享缓存，并支持磁盘 OAC 二级缓存。请求内存使用 mmap Arena，持久 CLI service 模式使用非移动分代 GC。

| 路径 | 职责 |
| --- | --- |
| `src/compiler/` | ASP 预处理、lexer、parser、符号和字节码 |
| `src/runtime/` | 值、执行、VM 分派和调试 hook |
| `src/engine/` | 引擎生命周期和 standalone/service 渲染 |
| `src/web/` | FastCGI 协议、worker、请求和资源限制 |
| `src/components/` | 内建组件和外部组件实现 |
| `src/storage/` | 共享字节码缓存和持久状态 |
| `tools/` | 反汇编器和调试器 |
| `tests/` | 单测、集成、兼容、端到端、压力和基准测试 |

## 安全

`OpenASP.Process` 和 `Shell.Application` 默认关闭，不应为不可信站点启用。外部扩展必须由管理员加入 allow-list；FastCGI 默认限制请求参数、请求体、响应、Header、超时和内存。API Key 与数据库凭据应通过部署环境提供，不应提交到 ASP 源码中。

## 版本

当前源码版本为 `0.2.0`，由 `src/cmd/version.h` 中的 `OPENASP_VERSION` 定义。

## 更多尝试

[uYasoo-MicroBlog](https://github.com/asp-lang/uyasoo-microblog) 是一个为 OpenASP 构建的完整经典 ASP 微博客。它使用 Redis 存储应用数据，通过 Nginx 和 `openasp-fpm` 运行，支持用户资料、关注、时间线、话题、动态、评论、点赞、搜索和图片上传等功能。按照项目文档即可使用 OpenASP 运行。

## OpenASP和PHP的性能和内存对比

以下基准测试在配备 12 核 CPU 和 36 GiB 内存的 Apple M3 Pro、MacOS 14.8.1 上对比 `openasp-fpm` 与 PHP-FPM 8.4.4。每个配置使用 1、2、4 或 8 个静态 worker，以并发 16 分别执行三轮 2,000 次请求，最终取中位数。确认两端渲染输出一致后，请求会均匀轮询等价的 `default`、`search`、`tags`、`feed` 和 `view` 页面。负载生成器通过 Unix FastCGI socket 直连，因此数据不包含 Nginx 和网络开销。

压测 runner 位于 `tests/benchmarks/runners/`，OpenASP 与 PHP 的对等 workload 分别位于 `tests/benchmarks/workloads/openasp/` 和 `tests/benchmarks/workloads/php/`。

内存数据是整个 FPM 服务在热请求结束时的总和，包括 master 和 worker，但不包括负载生成器及外部数据库、缓存服务。表中的 RSS 和 MacOS Physical footprint 单位均为 MiB；在该平台上 Physical footprint 更适合实际内存对比，但各进程数值相加并不等价于 Linux PSS。五组测试共执行 120 个 runtime 实例和 240,000 次正式请求。

### 1. SQLite ZBlog

**方法。** `new-asp-zblog` 与 `php-zblog` 使用同一份 SQLite 数据渲染等价的博客页面。每个负载线程使用独立 Session Cookie，两端均使用 16 MiB 字节码或 Opcode 缓存，并关闭时间戳校验。

| Worker | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP Physical | PHP Physical | 物理内存节省 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2,176 | 2,092 | 1.04x | 7.6 | 33.9 | 3.8 | 18.0 | 79.1% |
| 2 | 4,202 | 3,978 | 1.06x | 12.6 | 42.7 | 7.4 | 22.9 | 67.7% |
| 4 | 7,815 | 5,187 | 1.51x | 20.2 | 59.8 | 13.1 | 32.4 | 59.6% |
| 8 | 10,542 | 5,115 | 2.06x | 36.2 | 91.0 | 24.1 | 50.1 | 51.8% |

**结论。** OpenASP 在所有 worker 数量下均领先，吞吐从单 worker 的 2,176 RPS 扩展到八 worker 的 10,542 RPS，提升 4.84 倍。PHP-FPM 在四 worker 后停止增长；八 worker 时 OpenASP 吞吐达到 PHP 的 2.06 倍，同时少用 51.8% 的物理内存。

### 2. MySQL ZBlog

**方法。** `new-asp-zblog-mysql` 与 `php-zblog-mysql` 使用本机 MySQL 5.7.24 中相同的表结构和数据。该 OpenASP 版本使用标准非缓冲 Recordset 结果路径，因此测试会直接反映数据库结果获取与值转换成本。

| Worker | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP Physical | PHP Physical | 物理内存节省 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 374 | 620 | 0.60x | 9.3 | 32.3 | 4.0 | 17.0 | 76.4% |
| 2 | 693 | 1,561 | 0.44x | 15.2 | 39.5 | 9.0 | 21.0 | 57.2% |
| 4 | 1,305 | 2,233 | 0.58x | 29.1 | 53.9 | 15.6 | 29.1 | 46.6% |
| 8 | 1,491 | 2,831 | 0.53x | 52.5 | 78.9 | 28.3 | 42.4 | 33.3% |

**结论。** 标准 MySQL 结果路径是 OpenASP 当前最明显的性能瓶颈：吞吐仅为 PHP-FPM 的 44% 至 60%，但物理内存仍少用 33% 至 76%。四 worker 是更实际的性价比点，因为继续翻倍至八 worker 仅增加 14% 吞吐，物理内存则增加约 81%。

### 3. MySQL 缓冲结果集 ZBlog

**方法。** `new-asp-zblog-mysql-store` 使用 MySQL `store_result` 缓冲结果路径，并与上一组相同的 `php-zblog-mysql` 和数据库对比。模板、页面比例、数据、worker 配置和输出校验都保持不变，从而单独观察 OpenASP 结果缓冲模式的影响。

| Worker | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP Physical | PHP Physical | 物理内存节省 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 697 | 605 | 1.15x | 9.1 | 32.0 | 3.8 | 16.7 | 77.4% |
| 2 | 1,374 | 1,544 | 0.89x | 16.6 | 39.3 | 8.4 | 20.8 | 59.7% |
| 4 | 2,446 | 2,409 | 1.02x | 28.9 | 54.2 | 15.6 | 29.5 | 47.2% |
| 8 | 2,877 | 2,883 | 1.00x | 52.2 | 79.1 | 28.4 | 42.9 | 33.9% |

**结论。** 缓冲结果集消除了大部分差距：单 worker 时 OpenASP 快 15%，四和八 worker 时与 PHP-FPM 基本持平。由于 PHP 对照保持不变而只切换了 OpenASP 结果模式，因此非缓冲逐行读取及其相关转换工作是后续最主要的优化目标。

### 4. MySQL 批量查询 ZBlog

**方法。** `new-asp-zblog-mysql-batch` 与 `php-zblog-mysql-batch` 批量读取文章标签，减少每次页面请求所需的数据库往返次数。两端针对同一份 MySQL 数据执行等价的批量查询。

| Worker | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP Physical | PHP Physical | 物理内存节省 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 672 | 730 | 0.92x | 8.9 | 32.3 | 3.9 | 17.0 | 77.2% |
| 2 | 1,420 | 1,511 | 0.94x | 16.9 | 39.7 | 8.7 | 21.3 | 59.3% |
| 4 | 2,730 | 2,841 | 0.96x | 28.5 | 53.6 | 15.0 | 28.8 | 47.8% |
| 8 | 3,397 | 3,014 | 1.13x | 52.2 | 80.0 | 27.8 | 43.5 | 36.2% |

**结论。** 1 至 4 worker 时两端差距保持在约 4% 至 8%，八 worker 时 OpenASP 反超 12.7%。减少数据库往返后，OpenASP 的 worker 扩展能力更加明显；四 worker 的内存效率最好，八 worker 的绝对吞吐最高。

### 5. Redis 和 Memcache ZBlog

**方法。** `new-asp-zblog-redis-mc` 与 `php-zblog-redis-mc` 使用等价的 Redis 和 Memcache 数据访问。Redis 8.10.1 与 Memcached 1.6.45 在每轮测试中使用隔离端口，两端均使用持久客户端连接，避免上一轮缓存状态污染下一轮。

| Worker | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP Physical | PHP Physical | 物理内存节省 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1,959 | 1,655 | 1.18x | 7.2 | 32.5 | 3.5 | 17.1 | 79.9% |
| 2 | 3,595 | 3,549 | 1.01x | 11.9 | 39.8 | 6.8 | 21.3 | 67.9% |
| 4 | 6,167 | 5,294 | 1.16x | 18.0 | 53.5 | 11.3 | 28.8 | 60.9% |
| 8 | 7,999 | 4,177 | 1.92x | 32.6 | 78.7 | 21.9 | 43.1 | 49.1% |

**结论。** OpenASP 在所有 worker 数量下均不弱于 PHP-FPM。PHP-FPM 在四 worker 达到峰值后下降 21%，OpenASP 则继续提升到 7,999 RPS；八 worker 时吞吐为 PHP 的 1.92 倍，物理内存少用 49.1%。
