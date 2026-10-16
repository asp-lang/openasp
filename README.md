# OpenASP: High-performance, Low-memory Classic ASP for Linux, Unix and MacOS

<p align="center" id="openasp-top">
  <img src="assets/openasp-logo.png" alt="OpenASP" width="500">
</p>

<p align="center">
  <a href="#build-from-source"><img src="assets/badge-compiler.png" alt="Compiler: C11" height="20"></a>
  <a href="https://spdx.org/licenses/MIT.html"><img src="assets/badge-license.png" alt="License: MIT" height="20"></a>
  <a href="#why-openasp"><img src="assets/badge-platforms.png" alt="Platforms: Linux, MacOS, and Unix" height="20"></a>
</p>

<p align="center">
  <a href="#install-a-binary-package">Install</a> |
  <a href="#build-from-source">Build</a> |
  <a href="#hello-world">Hello World</a> |
  <a href="#command-line-tools">CLI Tools</a> |
  <a href="#deploy-with-nginx">Nginx</a> |
  <a href="#compatibility-and-components">Compatibility</a> |
  <a href="https://asp-lang.org">Website</a> |
  <a href="README.zh-CN.md">简体中文</a>
</p>

The official OpenASP website is **[https://asp-lang.org](https://asp-lang.org)**. Questions, suggestions, and ideas are always welcome. Please send feedback through the official website.

OpenASP is a high-performance, low-memory Classic ASP interpreter and resident runtime implemented in pure C11. It runs existing VBScript/ASP applications natively on Linux, Unix and MacOS without Windows Server or IIS. It provides standalone page rendering, a persistent interactive environment, a multi-process FastCGI service, ADO-compatible data access, native components, bytecode caching, and debugging tools. See the [OpenASP vs PHP performance and memory benchmark report](#openasp-vs-php-performance-and-memory) for detailed throughput and memory results.

## Why OpenASP

OpenASP is more than a compatibility layer. It preserves familiar Classic ASP and VBScript behavior while replacing the Windows-specific hosting stack with a portable runtime designed for current production environments. Existing applications can move to Linux, Unix, or MacOS without rewriting business code around a different language or web framework.

The runtime also extends Classic ASP with modern native capabilities. ASP code can use MySQL/MariaDB, SQLite, MDB, Redis, Memcache, HTTP/WebSocket clients, JSON, cryptography, and OpenAI-compatible APIs directly through `Server.CreateObject`, without a separate bridge service.

| Traditional Classic ASP | OpenASP |
| --- | --- |
| Requires Windows Server | Runs natively on Linux, Unix and MacOS |
| Hosted by IIS | Runs standalone or behind Nginx/FastCGI |
| Depends on Windows COM components | Provides portable native C components |
| Migration work leaks into ASP code | Compatibility work stays inside the engine |
| Access to modern services | Extends ASP with native Redis, MySQL/MariaDB, and OpenAI integrations |

## Install a Binary Package

OpenASP release archives use the following names:

| Platform | Archive |
| --- | --- |
| Linux x86-64 | [`openasp-0.1.0-linux-x86-64.tar.gz`](https://github.com/asp-lang/openasp/releases/download/v-0.1.0/openasp-0.1.0-linux-x86-64.tar.gz) |
| MacOS Apple Silicon | [`openasp-0.1.0-macos-arm64.zip`](https://github.com/asp-lang/openasp/releases/download/v-0.1.0/openasp-0.1.0-macos-arm64.zip) |

Download the matching archive from the project release page or [https://asp-lang.org](https://asp-lang.org), then extract it:

```sh
# Linux x86-64
tar -xzf openasp-0.1.0-linux-x86-64.tar.gz

# MacOS Apple Silicon
unzip openasp-0.1.0-macos-arm64.zip
```

Each package contains the five programs under `bin/`, the public extension headers under `include/`, and an example FPM configuration. Add `bin/` to `PATH` and verify the installation:

```sh
export PATH="/path/to/openasp-0.1.0-linux-x86-64/bin:$PATH"
openasp --help
```

Use the extracted MacOS directory name when installing on Apple Silicon.

## Build from Source

OpenASP requires a C11 compiler, `make`, `pkg-config`, PCRE2, libxml2, libcurl, SQLite, json-c, OpenSSL, a MySQL/MariaDB client library, and mdbtools.

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

Install the Xcode command-line tools and Homebrew dependencies:

```sh
xcode-select --install
brew install pkg-config pcre2 libxml2 curl sqlite json-c openssl@3 \
    mariadb-connector-c mdbtools
```

### Compile and Test

From the repository root:

```sh
make clean
make all
make test
```

The five executables are written to `build/bin/`. The full profile includes all native components. A smaller profile keeps the core runtime, persistent state, SQLite, json-c, and `Scripting.Dictionary`:

```sh
make minimal
make minimal-test
```

Additional verification targets:

```sh
make test-all
make external-test
make fixture-diff
make asan
```

Release archives can be built with:

```sh
scripts/make-release.sh macos
scripts/make-release.sh linux
scripts/make-release.sh all
```

Archives are written to `release/`. Linux x86-64 releases can be built from Apple Silicon through the local `ubuntu:24.04` Docker image and QEMU.

## Hello World

Create a site directory and save the following page as `/tmp/openasp-site/HelloWorld.asp`:

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

Render it directly without a web server:

```sh
mkdir -p /tmp/openasp-site
openasp /tmp/openasp-site/HelloWorld.asp \
    --root /tmp/openasp-site \
    --query "name=OpenASP"
```

When running a source build, replace `openasp` with
`./build/bin/openasp`. The output contains:

```html
<h1>Hello, OpenASP!</h1>
```

The standalone runner can also simulate a form request:

```sh
openasp /tmp/openasp-site/HelloWorld.asp \
    --root /tmp/openasp-site \
    --method POST \
    --body "name=OpenASP"
```

## Command-line Tools

| Program | Purpose |
| --- | --- |
| `openasp` | Render ASP pages, simulate requests, and export OAC bytecode |
| `openasp-fpm` | Serve ASP through FastCGI using synchronous worker processes |
| `openasp-cli` | Run a persistent interactive VBScript/OpenASP session |
| `openasp-dis` | Validate and disassemble `.oac` bytecode images |
| `openasp-dbg` | Debug ASP source or OAC images at source and opcode level |

### `openasp`

```sh
openasp page.asp --root /srv/www/site --query "id=42"
openasp page.asp --root /srv/www/site \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
openasp service.asp --root /srv/www/site \
    --runtime-mode=service --gc-heap-limit=256M --gc-diagnostics
```

The default `service` runtime uses the non-moving generational GC. Explicit `--runtime-mode=batch` uses a request arena. High-risk process capabilities remain disabled unless `--process-enable=on` or `--shell-enable=on` is passed.

### `openasp-cli`

```sh
openasp-cli --root /srv/www/site
```

Variables, functions, classes, `Session`, and `Application` remain available between inputs. Enter `? expression` to print a value, `:help` for commands, and `:quit` to exit. Arrow keys, Home/End, Backspace, and Delete are supported in an interactive terminal.

At the top-level prompt, input beginning with `!` executes the remaining text through `$SHELL` and streams its standard output and error directly to the terminal:

```text
OpenASP> !pwd
/srv/www/site
```

Shell commands are enabled by default in `openasp-cli` and can be disabled with `--shell-enable=off`; this does not change the shell policy of other OpenASP commands. Only the child command's standard output and error are displayed, including any colors it produces. Interactive terminals display the `OpenASP>` prompt in cyan. Redirected output, `TERM=dumb`, or `NO_COLOR=1` disables prompt formatting.

### `openasp-dis`

Generate an OAC image and inspect its metadata, source mapping, symbols, and 24-byte VM instructions:

```sh
openasp page.asp --root /srv/www/site \
    --emit-oac /tmp/page.oac --gen-dbg-info=on
openasp-dis /tmp/page.oac
```

See [`tools/openasp-dis/README.md`](tools/openasp-dis/README.md) for the output format and validation rules.

### `openasp-dbg`

Debug an ASP source file:

```sh
openasp-dbg page.asp --root /srv/www/site
```

Common commands include `break`, `run`, `continue`, `step`, `next`, `finish`, `backtrace`, `print`, `watch`, `list`, and `disassemble`. The debugger can also open OAC files created with `--gen-dbg-info=on`.

See [`tools/openasp-dbg/README.md`](tools/openasp-dbg/README.md) for local and remote FPM debugging.

### `openasp-fpm`

Start a development FastCGI service on loopback:

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

Each worker handles one FastCGI connection at a time. Set the required concurrency directly with `--workers`; the maximum is 256. Configuration can also be loaded from YAML:

```sh
openasp-fpm -c /etc/openasp-fpm.yaml
```

Command-line values override YAML, which overrides built-in defaults. A complete example is available at [`examples/openasp-fpm.yaml`](examples/openasp-fpm.yaml).

## Deploy with Nginx

### 1. Start OpenASP FastCGI

For a first deployment, bind FastCGI to loopback:
Create `/var/cache/openasp` first and grant the FPM service account write access to it.

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

The FPM `--root` and the Nginx `root` must identify the same physical directory.

### 2. Configure Nginx

Create `/etc/nginx/conf.d/openasp.conf`:

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

On Homebrew Nginx, `fastcgi_params` is normally located at `/opt/homebrew/etc/nginx/fastcgi_params`.

Validate the configuration, reload Nginx, and request the Hello World page:

```sh
sudo nginx -t
sudo nginx -s reload
curl 'http://127.0.0.1:8080/HelloWorld.asp?name=OpenASP'
```

For a Unix socket, start FPM with
`--unix-socket /run/openasp/openasp-fpm.sock`, replace `fastcgi_pass` with `unix:/run/openasp/openasp-fpm.sock`, and ensure the Nginx and OpenASP service accounts share access to the socket directory. OpenASP creates the socket with mode `0660`.

### Production Checklist

- Run Nginx and `openasp-fpm` as unprivileged service accounts.
- Keep TCP FastCGI bound to loopback, or use a protected Unix socket.
- Keep `shell-enable` and `process-enable` off unless a trusted application requires them.
- Make the site read-only except for explicit upload, state, cache, and log directories.
- Use systemd, launchd, or another service manager to supervise `openasp-fpm`.
- Keep timestamp validation on during development; disable it in production only when deployments restart workers.
- Set request, body, response, header, timeout, and memory limits for the application.

## Compatibility and Components

OpenASP implements the Classic ASP object model directly:

| Area | Supported surface |
| --- | --- |
| VBScript | Procedures, classes, arrays, `ByRef`, errors, `On Error Resume Next`, built-ins |
| ASP objects | `Request`, `Response`, `Server`, `Session`, `Application`, `Err` |
| Request data | Query strings, forms, cookies, server variables, headers, `BinaryRead` |
| Response | Buffering, headers, cookies, status, redirects, text and binary output |
| Includes | Classic file and virtual includes |
| Data access | SQLite, MySQL/MariaDB, Jet 4/MDB, Recordset, Command, parameters |

Portable native components include:

- `Scripting.Dictionary`, `Scripting.FileSystemObject`, and `VBScript.RegExp`
- `ADODB.Connection`, `ADODB.Recordset`, `ADODB.Command`, and `ADODB.Stream`
- XML DOM, XMLHTTP, HTTP, JSON, and Crypto
- Socket, WebSocket/WSS, Redis, and Memcache
- OpenAI-compatible APIs
- `OpenASP.Process` and `Shell.Application`, disabled by default

Concurrent outbound HTTP requests can be executed in one worker thread with the libcurl Multi-based `OpenASP.HttpMulti` component:

```asp
Set multi = Server.CreateObject("OpenASP.HttpMulti")
Set first = multi.CreateRequest("GET", "https://api-a.example/data")
Set second = multi.CreateRequest("POST", "https://api-b.example/query")
second.Body = "{""id"":123}"
second.SetHeader "Content-Type", "application/json"
multi.MaxConcurrency = 8
multi.Timeout = 10000
multi.Execute
If first.ErrorCode = 0 Then Response.Write first.ResponseText
```

Each request exposes `Status`, `Completed`, `ErrorCode`, `ErrorMessage`, `ResponseText`, `ContentType`, `Timeout`, `TLSVerify`, and `CAFile`. A failed transfer does not cancel successful requests in the same batch.

Multiple Redis endpoints can be accessed concurrently with `OpenASP.RedisMulti`. Commands for the same client are automatically pipelined, while different clients are driven concurrently by `epoll` on Linux and `kqueue` on macOS:

```asp
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
```

Command results expose `Value`, `Completed`, `ErrorCode`, `ErrorMessage`, and `MaybeExecuted`. Connection failures and Redis command errors remain isolated to the affected client or command. Stateful, subscription, and blocking commands are rejected because they cannot safely share a pipeline connection.

`OpenASP.MemcacheMulti` provides the same endpoint concurrency and per-client Pipeline model for the Memcache text protocol:

```asp
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
```

Supported commands are `get`, `gets`, `set`, `add`, `replace`, `append`, `prepend`, `cas`, `delete`, `touch`, `incr`, `decr`, `version`, and `flush_all`. Command results expose the common status properties plus `Flags` and `Cas` for retrieval commands. TLS and the Memcache binary protocol are not supported by this component.

External native extensions can be loaded through the stable C ABI. 

## Architecture and Documentation

ASP templates and includes are parsed and compiled into validated bytecode. Standalone and service runtimes execute that bytecode directly; FPM stores immutable compiled modules in a shared cross-process cache with an optional disk OAC cache. Request memory uses mmap-backed arenas, while persistent CLI service mode uses a non-moving generational GC.

| Path | Responsibility |
| --- | --- |
| `src/compiler/` | ASP preprocessing, lexer, parser, symbols, and bytecode |
| `src/runtime/` | Values, execution, VM dispatch, and debugging hooks |
| `src/engine/` | Engine lifecycle and standalone/service rendering |
| `src/web/` | FastCGI protocol, workers, requests, and limits |
| `src/components/` | Built-in and external component implementations |
| `src/storage/` | Shared bytecode cache and persistent state |
| `tools/` | Disassembler and debugger |
| `tests/` | Unit, integration, conformance, e2e, stress, and benchmarks |

## Security

`OpenASP.Process` and `Shell.Application` are disabled by default. Do not enable them for untrusted sites. External extensions are administrator allow-listed, and FastCGI applies request, body, response, header, timeout, and memory limits. API keys and database credentials should be supplied by the deployment environment, not committed to ASP source.

## Version

The current source version is `0.1.0`, defined by `OPENASP_VERSION` in `src/cmd/version.h`.

## More to Try

[uYasoo-MicroBlog](https://github.com/asp-lang/uyasoo-microblog) is a complete Classic ASP microblog built for OpenASP. It uses Redis for application data and runs behind Nginx through `openasp-fpm`, with support for user profiles, follows, timelines, topics, posts, comments, likes, search, image uploads, and more. Follow the project documentation to run it with OpenASP.

## OpenASP vs PHP Performance and Memory

The following benchmarks compare `openasp-fpm` with PHP-FPM 8.4.4 on an Apple M3 Pro with 12 CPU cores and 36 GiB of memory, running MacOS 14.8.1. Each configuration uses 1, 2, 4, or 8 static workers, executes three rounds of 2,000 requests at concurrency 16, and reports the median. Requests rotate evenly through the equivalent `default`, `search`, `tags`, `feed`, and `view` pages after their rendered output has been verified. The load generator connects directly through a Unix FastCGI socket, so Nginx and network overhead are excluded.

The benchmark runners are located in `tests/benchmarks/runners/`. The equivalent OpenASP and PHP workloads are stored in `tests/benchmarks/workloads/openasp/` and `tests/benchmarks/workloads/php/`.

Memory values are the hot-state totals for the complete FPM service, including its master and workers but excluding the load generator and external database/cache services. RSS and MacOS physical footprint are both shown in MiB; physical footprint is the more useful comparison on this platform, although the process totals are not equivalent to Linux PSS. Across all five workloads, the test comprises 120 runtime executions and 240,000 measured requests.

### 1. SQLite ZBlog

**Method.** `new-asp-zblog` and `php-zblog` render equivalent blog pages from the same SQLite dataset. Each load-generator thread uses its own session cookie, both runtimes use a 16 MiB bytecode/opcode cache, and timestamp validation is disabled.

| Workers | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP physical | PHP physical | Physical saved |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2,176 | 2,092 | 1.04x | 7.6 | 33.9 | 3.8 | 18.0 | 79.1% |
| 2 | 4,202 | 3,978 | 1.06x | 12.6 | 42.7 | 7.4 | 22.9 | 67.7% |
| 4 | 7,815 | 5,187 | 1.51x | 20.2 | 59.8 | 13.1 | 32.4 | 59.6% |
| 8 | 10,542 | 5,115 | 2.06x | 36.2 | 91.0 | 24.1 | 50.1 | 51.8% |

**Conclusion.** OpenASP leads at every worker count and scales from 2,176 to 10,542 requests per second, or 4.84x from one to eight workers. PHP-FPM stops scaling after four workers, while OpenASP reaches 2.06x its throughput at eight workers and uses 51.8% less physical memory.

### 2. MySQL ZBlog

**Method.** `new-asp-zblog-mysql` and `php-zblog-mysql` use the same schema and records on a local MySQL 5.7.24 server. This OpenASP variant uses the standard non-buffered Recordset result path, making the test sensitive to database result fetching and value conversion costs.

| Workers | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP physical | PHP physical | Physical saved |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 374 | 620 | 0.60x | 9.3 | 32.3 | 4.0 | 17.0 | 76.4% |
| 2 | 693 | 1,561 | 0.44x | 15.2 | 39.5 | 9.0 | 21.0 | 57.2% |
| 4 | 1,305 | 2,233 | 0.58x | 29.1 | 53.9 | 15.6 | 29.1 | 46.6% |
| 8 | 1,491 | 2,831 | 0.53x | 52.5 | 78.9 | 28.3 | 42.4 | 33.3% |

**Conclusion.** The standard MySQL result path is the clearest current OpenASP bottleneck: throughput is 44-60% of PHP-FPM despite using 33-76% less physical memory. Four workers are the practical efficiency point because doubling to eight workers adds only 14% throughput while increasing physical memory by about 81%.

### 3. MySQL Buffered Result ZBlog

**Method.** `new-asp-zblog-mysql-store` uses the buffered MySQL `store_result` path and is compared with the same `php-zblog-mysql` workload and database used above. The templates, page mix, records, worker settings, and validation remain unchanged, isolating the effect of OpenASP result buffering.

| Workers | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP physical | PHP physical | Physical saved |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 697 | 605 | 1.15x | 9.1 | 32.0 | 3.8 | 16.7 | 77.4% |
| 2 | 1,374 | 1,544 | 0.89x | 16.6 | 39.3 | 8.4 | 20.8 | 59.7% |
| 4 | 2,446 | 2,409 | 1.02x | 28.9 | 54.2 | 15.6 | 29.5 | 47.2% |
| 8 | 2,877 | 2,883 | 1.00x | 52.2 | 79.1 | 28.4 | 42.9 | 33.9% |

**Conclusion.** Buffering removes most of the gap: OpenASP is 15% faster with one worker and effectively tied with PHP-FPM at four and eight workers. Since this changes the OpenASP result mode while preserving the PHP control, the result points to non-buffered row fetching and associated conversion work as the primary optimization target.

### 4. MySQL Batched ZBlog

**Method.** `new-asp-zblog-mysql-batch` and `php-zblog-mysql-batch` batch post-tag retrieval so that a page requires fewer database round trips. Both sides use equivalent batched queries against the same MySQL data.

| Workers | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP physical | PHP physical | Physical saved |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 672 | 730 | 0.92x | 8.9 | 32.3 | 3.9 | 17.0 | 77.2% |
| 2 | 1,420 | 1,511 | 0.94x | 16.9 | 39.7 | 8.7 | 21.3 | 59.3% |
| 4 | 2,730 | 2,841 | 0.96x | 28.5 | 53.6 | 15.0 | 28.8 | 47.8% |
| 8 | 3,397 | 3,014 | 1.13x | 52.2 | 80.0 | 27.8 | 43.5 | 36.2% |

**Conclusion.** The runtimes remain within 4-8% through four workers, and OpenASP becomes 12.7% faster at eight workers. Reducing database round trips exposes stronger OpenASP worker scaling; four workers offer the best memory efficiency, while eight workers provide the highest throughput.

### 5. Redis and Memcache ZBlog

**Method.** `new-asp-zblog-redis-mc` and `php-zblog-redis-mc` use equivalent Redis and Memcache data access. Redis 8.10.1 and Memcached 1.6.45 run on isolated ports for each round, and both workloads use persistent client connections so that a previous round cannot warm or contaminate the next one.

| Workers | OpenASP RPS | PHP RPS | OpenASP / PHP | OpenASP RSS | PHP RSS | OpenASP physical | PHP physical | Physical saved |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1,959 | 1,655 | 1.18x | 7.2 | 32.5 | 3.5 | 17.1 | 79.9% |
| 2 | 3,595 | 3,549 | 1.01x | 11.9 | 39.8 | 6.8 | 21.3 | 67.9% |
| 4 | 6,167 | 5,294 | 1.16x | 18.0 | 53.5 | 11.3 | 28.8 | 60.9% |
| 8 | 7,999 | 4,177 | 1.92x | 32.6 | 78.7 | 21.9 | 43.1 | 49.1% |

**Conclusion.** OpenASP matches or exceeds PHP-FPM at every worker count. PHP-FPM peaks at four workers and then falls by 21%, whereas OpenASP continues to 7,999 requests per second, reaches 1.92x PHP throughput, and uses 49.1% less physical memory at eight workers.
