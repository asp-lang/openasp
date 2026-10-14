[English](COMPONENT_CONFORMANCE_MATRIX.md) | [简体中文](COMPONENT_CONFORMANCE_MATRIX.zh-CN.md)

# OpenASP component semantic test matrix

This matrix defines the executable contract of the built-in component. Each row of fixtures must be separated from the business site;
defect fixes are also added to `tests/conformance/cases/*.asp`, and external dependencies are driven by
`tests/integration/extensions/test_external_components.py`.

| Components | Input and State Migration | Output and Error Contracts | Current Standalone Use Cases |
| --- | --- | --- | --- |
| `ADODB.Recordset` | Empty set, first line, `MoveNext`, `MoveFirst`, `Close`, forward window release | `BOF`/`EOF`, 3704/3021/3219, cursor position | `ado_recordset_lifetime.asp`, `ado_recordset_addnew.asp`, `tests/unit/test_components.c` |
| Recordset paging | `PageSize`, `AbsolutePage`, last page, out-of-bounds page | `PageCount` round up, return EOF out of page | `tests/fixtures/reference/ado_recordset_paging.asp`, `tests/unit/test_components.c` |
| Recordset field | ordinal, case-independent name, Fields enumeration, schema switching | Correct column value; unknown name is 3265 | `ado_fields_enumeration.asp`, `tests/unit/test_components.c` |
| Recordset `GetRows` | Empty set, partial read, streaming/buffered results, 2D dimension | `rows(field,row)`, column order, cursor advance after call | `ado_mysql_getrows_column_order.asp`, `tests/unit/test_components.c` |
| SQLite ADO | NULL, Unicode, long text, multiple active result sets, nested queries | Text is valid for the lifetime of Recordset; driver error 3000 | `ado_recordset_lifetime.asp`, `test_fastcgi.py` |
| MySQL ADO | streaming/buffered, nested query, connection pool, empty set, type conversion | streaming prohibits nesting with unconsumed connections; buffered allows | `mysql_sliding_window.asp`, `mysql_buffered_nested.asp`, `test_external_components.py` |
| MDB ADO | DDL/DML, transactions, updates, Memo, complex queries | Rollback on failure; compound updates keep field values | `ado_mdb_*.asp`, `test_external_components.py` |
| `Request` | Query/Form/Cookie subkeys, encoding, duplicate parameters | Parameter collection and binary body do not string requests | `request_response.asp`, `request_cookie_subkeys.asp`, `test_fastcgi.py` |
| `Response` | Buffer, Header, Cookie, Redirect, End, binary writing | Status/header must be submitted before body; End terminates the current request | `request_response.asp`, `test_fastcgi.py` |
| `Server` | CreateObject, MapPath, Execute, Include | Paths constrained by root; Include errors can be located | `component_factories.asp`, `preprocessor.asp` |
| `Session`/`Application` | Cross-request persistence, objects and arrays, concurrent workers | Same as session Visible, different session/request isolation | `set_session_complex.asp`, `get_session_complex.asp`, `test_fastcgi.py` |
| VBScript error model | `On Error Resume Next`, nested routines, `Err.Clear` | Local error status does not pollute the caller | `resume_next_conditions.asp`, `resume_next_call_scope.asp` |
| VBScript values and scope | ByRef/ByVal, default members, arrays, dates, Variants, class field masking | parameter writeback, array boundaries, scope priority | `routines_classes.asp`, `syntax_builtins.asp`, `tests/unit/test_components.c` |
| Dictionary/FSO/Stream/XML/RegExp/JSON | Creation, collection access, text/binary, parsing and exceptions | Standard member values and error codes such as 5/438 | `component_local_members.asp`, `component_error_paths.asp` |
| Network and Privileged Components | HTTP/Socket/Redis/Memcache/Process | Disable default reject; external protocol failure is recoverable | `test_external_components.py` |

## Layered execution

| Levels | Commands | Targets |
| --- | --- | --- |
| No external dependencies | `make test` | Parser, runtime, FastCGI, standalone conformance fixture |
| External components | `make external-test` | MySQL, Redis, MDB, network components and connection pools |
| Memory safety | `make asan` | ASan/UBSan and Darwin leak checking |
| Long stability and concurrency | `python3 tests/runtime_long_stress.py`, `python3 tests/stability_syntax_fuzz.py` | Multiple workers, timeouts, request interruptions and random syntax input |

Uncovered matrix rows must not be stated as "compatible". Minimal regressions for new engine defects must be entered into
`tests/conformance/cases/`, and then included in the default or external layer by the corresponding Python/C harness.
