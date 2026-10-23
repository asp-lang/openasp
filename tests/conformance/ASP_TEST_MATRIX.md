[English](ASP_TEST_MATRIX.md) | [简体中文](ASP_TEST_MATRIX.zh-CN.md)

# OpenASP ASP test matrix

This document is an executable manifest of the contract between the ASP language and native components. `make test` executes lines without external
dependencies; `make external-test` executes lines that require local services or explicitly enabled privileged capabilities.

## Language syntax

| Parse node or syntax family | Normal path | Boundary path | Error path |
| --- | --- | --- | --- |
| Empty statements, HTML/script boundaries, line continuations | `expressions.asp`, `vbscript_bnf_coverage.asp` | `response_html_doctype_prefix.asp` | `invalid_syntax.asp` |
| Literal, identifier, member access, call, `New` | `expressions.asp`, `routines_classes.asp` | `object_member_id_fast_path.asp` | `dynamic_error_paths.asp` |
| Unary `+`, `-`, `Not` | `expressions.asp` | `float_literals.asp` | `test_core.c` |
| Arithmetic, concatenation, comparison, `Like`, `Is` | `expressions.asp` | `reversed_comparison.asp`, `object_is.asp` | `test_core.c` |
| `And`, `Or`, `Xor`, `Eqv`, `Imp` | `expressions.asp` | `boolean_integer_comparison.asp` | `test_core.c` |
| `Option Explicit`, `Dim`, `Const`, `ReDim [Preserve]`, `Erase` | `syntax_statements.asp` | `multidimensional_array_bounds_byref.asp`, `redim_empty_array.asp` | `test_core.c` |
| Assignment, `Set`, `Call` | `routines_classes.asp`, `call_statement_concat.asp` | `array_assignment_isolation.asp` | `resume_next_failed_assignment.asp` |
| `If` / `ElseIf` / `Else` | `syntax_statements.asp` | `if_inline_end_if.asp`, `elseif_true_branch.asp` | `invalid_syntax.asp` |
| `While`, `Do [While/Until]`, `For`, `For Each`, `Exit` | `control_flow.asp`, `syntax_statements.asp`, `with_exit.asp` | `exit_do_nested.asp`, `nested_loop_exit_copyback.asp` | `test_core.c` |
| `Select Case` | `syntax_statements.asp` | `select_case_multi_value.asp` | `test_core.c` |
| `Sub`, `Function`, ByRef/ByVal, properties, classes | `routines_classes.asp`, `with_exit.asp` | `property_get_let.asp`, `class_readonly_property_byref.asp` | `exit_property.asp` |
| `With`, lexical/global scope, include | `with_exit.asp`, `preprocessor.asp` | `with_load_from_file_function.asp`, `routine_lexical_scope.asp` | `preprocessor_missing_include.asp` |
| `On Error`, `Err`, `Eval`, `Execute`, `ExecuteGlobal` | `dynamic_error_paths.asp` | `execute_frame_bridge.asp`, `execute_global.asp` | `err_resume.asp`, `resume_next_missing_object_condition.asp` |
| Built-in conversions, string, array, date, math, binary | `syntax_builtins.asp` | `math_trig_builtins.asp`, `binary_read_cursor.asp` | `test_core.c` |
| ASP `Request`, `Response`, `Server`, `Session`, `Application` | `request_response.asp`, `test_fastcgi.py` | `server_vars_full.asp`, `response_binary_prefix.asp`, `get_session_complex.asp` | `response_end_in_condition.asp` |

The reference `*.asp` fixtures listed in the above table are located in
`tests/fixtures/reference/`, executed by the existing regression suite.
`tests/unit/test_components.c` overrides parser and VM rejection paths that cannot be represented as renderable pages.

## Native component

| Component Families and Members | Normal Paths | Boundary Paths | Error/Policy Paths |
| --- | --- | --- | --- |
| `Scripting.Dictionary`: add/item/keys/items/remove/count/compare mode | `component_local_members.asp` | `dictionary_nested_keys.asp` | `component_error_paths.asp` |
| `Scripting.FileSystemObject`, text file, file/directory collection | `component_local_members.asp` | `fso_date_last_modified.asp` | `component_error_paths.asp` |
| `VBScript.RegExp`, match, match/submatch collection | `component_local_members.asp` | `component_regexp_capture.asp`, `component_regexp_unicode_escape.asp` | `test_core.c` |
| `ADODB.Stream` | `component_local_members.asp` | `component_stream_copy.asp` | `component_error_paths.asp` |
| XML DOM, XML nodes, node lists, parsing errors | `component_local_members.asp` | `component_dom.asp`, `component_xml_cdata.asp` | `test_core.c` |
| HTTP/XMLHTTP and OpenAI Client | `component_http.asp`, `component_backend_services.asp` | `component_https.asp`, `component_openai_stream.asp` | `test_external_components.py` mock failure path |
| ADO Connection, Recordset, Command, Parameters | `component_mysql.asp`, `ado_sqlite.asp` | `ado_recordset_paging.asp`, `mysql_sliding_window.asp` | `component_mysql_empty_recordset.asp` |
| `OpenASP.Socket` TCP/Unix/UDP/listener | `component_socket_client.asp` | `component_socket_listener.asp`, `component_socket_udp.asp` | `component_socket_error.asp` |
| `OpenASP.Redis` | `component_redis.asp` | worker pool check | `test_external_components.py` Unavailable endpoint check |
| `OpenASP.JSON`, `OpenASP.Crypto` | `component_local_members.asp` | `components_basic.asp` | `test_core.c` |
| `OpenASP.Process`, `Shell.Application` | `component_process.asp`, `component_shell_enabled.asp` | Literal parameter injection protection | Disable policy and startup failure |
| `OpenASP.WebSocket` | `component_websocket_client.asp` | server and WSS fixtures | backend harness protocol failure assertion |
| `OpenASP.Memcache` | `component_memcache.asp` | CAS/multi-key mock override | Illegal key and shutdown status checks |

`component_factories.asp` Additional validation that each registered canonical ProgID can be constructed.
Alias ​​ProgID and member semantics of dependent services are overridden by `tests/integration/extensions/test_external_components.py`.

## Coverage description

The current warehouse has no coverage build target; the regular build of native Apple Clang does not generate
`gcov` profile data, so this report only gives reproducible behavior matrix execution results and cannot
Express this as line coverage. Clang
`-fprofile-instr-generate -fcoverage-mapping` and `llvm-cov export` targets should be added later.
