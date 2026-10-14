# OpenASP：面向 Linux、Unix 和 MacOS 的经典 ASP

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

OpenASP 是一个使用纯 C11 实现的经典 ASP 解释器和常驻运行时。它让现有的 VBScript/ASP 应用无需 Windows Server 和 IIS，即可原生运行在 Linux、Unix 和 MacOS 上。OpenASP 提供独立页面渲染、持久交互环境、多进程 FastCGI 服务、兼容 ADO 的数据访问、原生组件、字节码缓存和调试工具。

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
| Linux x86-64 | [`openasp-0.1.0-linux-x86-64.tar.gz`](https://github.com/asp-lang/openasp/releases/download/v-0.1.0/openasp-0.1.0-linux-x86-64.tar.gz) |
| MacOS Apple Silicon | [`openasp-0.1.0-macos-arm64.zip`](https://github.com/asp-lang/openasp/releases/download/v-0.1.0/openasp-0.1.0-macos-arm64.zip) |

从项目发布页面或 [https://asp-lang.org](https://asp-lang.org) 下载对应平台的压缩包，然后解压：

```sh
# Linux x86-64
tar -xzf openasp-0.1.0-linux-x86-64.tar.gz

# MacOS Apple Silicon
unzip openasp-0.1.0-macos-arm64.zip
```

每个发行包都在 `bin/` 中提供五个程序，在 `include/` 中提供公开扩展头文件，并附带一份 FPM 配置示例。将 `bin/` 加入 `PATH` 并验证安装：

```sh
export PATH="/path/to/openasp-0.1.0-linux-x86-64/bin:$PATH"
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

### macOS

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

可通过稳定 C ABI 加载外部原生扩展。

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

当前源码版本为 `0.1.0`，由 `src/cmd/version.h` 中的 `OPENASP_VERSION` 定义。

## 更多尝试

[uYasoo-MicroBlog](https://github.com/asp-lang/uyasoo-microblog) 是一个为 OpenASP 构建的完整经典 ASP 微博客。它使用 Redis 存储应用数据，通过 Nginx 和 `openasp-fpm` 运行，支持用户资料、关注、时间线、话题、动态、评论、点赞、搜索和图片上传等功能。按照项目文档即可使用 OpenASP 运行。
