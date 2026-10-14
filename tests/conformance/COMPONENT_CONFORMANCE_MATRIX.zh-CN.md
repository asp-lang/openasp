[English](COMPONENT_CONFORMANCE_MATRIX.md) | [简体中文](COMPONENT_CONFORMANCE_MATRIX.zh-CN.md)

# OpenASP 组件语义测试矩阵

本矩阵定义内建组件的可执行契约。每一行的 fixture 必须脱离业务站点；
缺陷修复同时补充 `tests/conformance/cases/*.asp`，外部依赖由
`tests/integration/extensions/test_external_components.py` 驱动。

| 组件 | 输入与状态迁移 | 输出与错误契约 | 当前独立用例 |
| --- | --- | --- | --- |
| `ADODB.Recordset` | 空集、首行、`MoveNext`、`MoveFirst`、`Close`、前向窗口释放 | `BOF`/`EOF`、3704/3021/3219、游标位置 | `ado_recordset_lifetime.asp`、`ado_recordset_addnew.asp`、`tests/unit/test_components.c` |
| Recordset 分页 | `PageSize`、`AbsolutePage`、尾页、越界页 | `PageCount` 向上取整，页外返回 EOF | `tests/fixtures/reference/ado_recordset_paging.asp`、`tests/unit/test_components.c` |
| Recordset 字段 | ordinal、大小写无关名称、Fields 枚举、schema 切换 | 正确列值；未知名称为 3265 | `ado_fields_enumeration.asp`、`tests/unit/test_components.c` |
| Recordset `GetRows` | 空集、部分读取、流式/缓冲结果、二维维度 | `rows(field,row)`、列序、调用后游标推进 | `ado_mysql_getrows_column_order.asp`、`tests/unit/test_components.c` |
| SQLite ADO | NULL、Unicode、长文本、多活动结果集、嵌套查询 | 文本在 Recordset 生命周期内有效；驱动错误为 3000 | `ado_recordset_lifetime.asp`、`test_fastcgi.py` |
| MySQL ADO | streaming/buffered、嵌套查询、连接池、空集、类型转换 | streaming 禁止同连接未消费嵌套；buffered 允许 | `mysql_sliding_window.asp`、`mysql_buffered_nested.asp`、`test_external_components.py` |
| MDB ADO | DDL/DML、事务、更新、Memo、复杂查询 | 失败回滚；复合更新保持字段值 | `ado_mdb_*.asp`、`test_external_components.py` |
| `Request` | Query/Form/Cookie 子键、编码、重复参数 | 参数集合和二进制 body 不串请求 | `request_response.asp`、`request_cookie_subkeys.asp`、`test_fastcgi.py` |
| `Response` | Buffer、Header、Cookie、Redirect、End、二进制写入 | 状态/头必须在 body 前提交；End 终止当前请求 | `request_response.asp`、`test_fastcgi.py` |
| `Server` | CreateObject、MapPath、Execute、Include | 路径受 root 约束；Include 错误可定位 | `component_factories.asp`、`preprocessor.asp` |
| `Session`/`Application` | 跨请求持久化、对象与数组、并发 worker | 同 session 可见，不同 session/request 隔离 | `set_session_complex.asp`、`get_session_complex.asp`、`test_fastcgi.py` |
| VBScript 错误模型 | `On Error Resume Next`、嵌套例程、`Err.Clear` | 局部错误状态不污染调用方 | `resume_next_conditions.asp`、`resume_next_call_scope.asp` |
| VBScript 值与作用域 | ByRef/ByVal、默认成员、数组、日期、Variant、类字段遮蔽 | 参数回写、数组边界、作用域优先级 | `routines_classes.asp`、`syntax_builtins.asp`、`tests/unit/test_components.c` |
| Dictionary/FSO/Stream/XML/RegExp/JSON | 创建、集合访问、文本/二进制、解析和异常 | 标准成员值和 5/438 等错误码 | `component_local_members.asp`、`component_error_paths.asp` |
| 网络与特权组件 | HTTP/Socket/Redis/Memcache/Process | 禁用默认拒绝；外部协议失败可恢复 | `test_external_components.py` |

## 分层执行

| 层级 | 命令 | 目标 |
| --- | --- | --- |
| 无外部依赖 | `make test` | 解析器、运行时、FastCGI、独立 conformance fixture |
| 外部组件 | `make external-test` | MySQL、Redis、MDB、网络组件和连接池 |
| 内存安全 | `make asan` | ASan/UBSan 与 Darwin 泄漏检查 |
| 长稳与并发 | `python3 tests/runtime_long_stress.py`、`python3 tests/stability_syntax_fuzz.py` | 多 worker、超时、请求中断和随机语法输入 |

未覆盖的矩阵行不得以“已兼容”表述。新增引擎缺陷的最小回归必须进入
`tests/conformance/cases/`，再由对应 Python/C harness 纳入默认或外部层。
