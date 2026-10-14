[English](ASP_TEST_MATRIX.md) | [简体中文](ASP_TEST_MATRIX.zh-CN.md)

# OpenASP ASP 测试矩阵

本文档是 ASP 语言与原生组件契约的可执行清单。`make test` 执行无外部
依赖的行；`make external-test` 执行需要本地服务或显式启用特权能力的行。

## 语言语法

| 解析节点或语法族 | 正常路径 | 边界路径 | 错误路径 |
| --- | --- | --- | --- |
| 空语句、HTML/脚本边界、续行 | `expressions.asp`、`vbscript_bnf_coverage.asp` | `response_html_doctype_prefix.asp` | `invalid_syntax.asp` |
| 字面量、标识符、成员访问、调用、`New` | `expressions.asp`、`routines_classes.asp` | `object_member_id_fast_path.asp` | `dynamic_error_paths.asp` |
| 一元 `+`、`-`、`Not` | `expressions.asp` | `float_literals.asp` | `test_core.c` |
| 算术、拼接、比较、`Like`、`Is` | `expressions.asp` | `reversed_comparison.asp`、`object_is.asp` | `test_core.c` |
| `And`、`Or`、`Xor`、`Eqv`、`Imp` | `expressions.asp` | `boolean_integer_comparison.asp` | `test_core.c` |
| `Option Explicit`、`Dim`、`Const`、`ReDim [Preserve]`、`Erase` | `syntax_statements.asp` | `multidimensional_array_bounds_byref.asp`、`redim_empty_array.asp` | `test_core.c` |
| 赋值、`Set`、`Call` | `routines_classes.asp`、`call_statement_concat.asp` | `array_assignment_isolation.asp` | `resume_next_failed_assignment.asp` |
| `If` / `ElseIf` / `Else` | `syntax_statements.asp` | `if_inline_end_if.asp`、`elseif_true_branch.asp` | `invalid_syntax.asp` |
| `While`、`Do [While/Until]`、`For`、`For Each`、`Exit` | `control_flow.asp`、`syntax_statements.asp`、`with_exit.asp` | `exit_do_nested.asp`、`nested_loop_exit_copyback.asp` | `test_core.c` |
| `Select Case` | `syntax_statements.asp` | `select_case_multi_value.asp` | `test_core.c` |
| `Sub`、`Function`、ByRef/ByVal、属性、类 | `routines_classes.asp`、`with_exit.asp` | `property_get_let.asp`、`class_readonly_property_byref.asp` | `exit_property.asp` |
| `With`、词法/全局作用域、include | `with_exit.asp`、`preprocessor.asp` | `with_load_from_file_function.asp`、`routine_lexical_scope.asp` | `preprocessor_missing_include.asp` |
| `On Error`、`Err`、`Eval`、`Execute`、`ExecuteGlobal` | `dynamic_error_paths.asp` | `execute_frame_bridge.asp`、`execute_global.asp` | `err_resume.asp`、`resume_next_missing_object_condition.asp` |
| 内置转换、字符串、数组、日期、数学、二进制 | `syntax_builtins.asp` | `math_trig_builtins.asp`、`binary_read_cursor.asp` | `test_core.c` |
| ASP `Request`、`Response`、`Server`、`Session`、`Application` | `request_response.asp`、`test_fastcgi.py` | `server_vars_full.asp`、`response_binary_prefix.asp`、`get_session_complex.asp` | `response_end_in_condition.asp` |

上表列出的参考 `*.asp` fixture 位于
`tests/fixtures/reference/`，由既有回归套件执行。
`tests/unit/test_components.c` 覆盖无法作为可渲染页面表示的解析器与 VM 拒绝路径。

## 原生组件

| 组件族与成员 | 正常路径 | 边界路径 | 错误/策略路径 |
| --- | --- | --- | --- |
| `Scripting.Dictionary`：add/item/keys/items/remove/count/compare mode | `component_local_members.asp` | `dictionary_nested_keys.asp` | `component_error_paths.asp` |
| `Scripting.FileSystemObject`、文本文件、文件/目录集合 | `component_local_members.asp` | `fso_date_last_modified.asp` | `component_error_paths.asp` |
| `VBScript.RegExp`、匹配、匹配/子匹配集合 | `component_local_members.asp` | `component_regexp_capture.asp`、`component_regexp_unicode_escape.asp` | `test_core.c` |
| `ADODB.Stream` | `component_local_members.asp` | `component_stream_copy.asp` | `component_error_paths.asp` |
| XML DOM、XML 节点、节点列表、解析错误 | `component_local_members.asp` | `component_dom.asp`、`component_xml_cdata.asp` | `test_core.c` |
| HTTP/XMLHTTP 与 OpenAI 客户端 | `component_http.asp`、`component_backend_services.asp` | `component_https.asp` | `test_external_components.py` mock 失败路径 |
| ADO Connection、Recordset、Command、Parameters | `component_mysql.asp`、`ado_sqlite.asp` | `ado_recordset_paging.asp`、`mysql_sliding_window.asp` | `component_mysql_empty_recordset.asp` |
| `OpenASP.Socket` TCP/Unix/UDP/listener | `component_socket_client.asp` | `component_socket_listener.asp`、`component_socket_udp.asp` | `component_socket_error.asp` |
| `OpenASP.Redis` | `component_redis.asp` | worker pool 检查 | `test_external_components.py` 的不可用端点检查 |
| `OpenASP.JSON`、`OpenASP.Crypto` | `component_local_members.asp` | `components_basic.asp` | `test_core.c` |
| `OpenASP.Process`、`Shell.Application` | `component_process.asp`、`component_shell_enabled.asp` | 字面参数注入防护 | 禁用策略与启动失败 |
| `OpenASP.WebSocket` | `component_websocket_client.asp` | server 与 WSS fixture | backend harness 协议失败断言 |
| `OpenASP.Memcache` | `component_memcache.asp` | CAS/多 key mock 覆盖 | 非法 key 与关闭状态检查 |

`component_factories.asp` 额外验证每个已注册 canonical ProgID 均可构造。
别名 ProgID 及依赖服务的成员语义由 `tests/integration/extensions/test_external_components.py` 覆盖。

## 覆盖率说明

当前仓库没有 coverage 构建目标；本机 Apple Clang 的常规构建未生成
`gcov` profile 数据，因此本次报告只给出可复现的行为矩阵执行结果，不能
将其表述为行覆盖率。后续应增加 Clang
`-fprofile-instr-generate -fcoverage-mapping` 与 `llvm-cov export` 目标。
