# db++

SQLite 优先、通过 **Apache Arrow ADBC** 兼容其它数据库的桌面数据管理器。
UI 使用本地 [HuxerUI](../HuxerUI-fork)（`farna/main`）和
[HuxerUI/Lib-DataTable](https://github.com/HuxerUI/Lib-DataTable)；数据库访问统一走
[Apache Arrow ADBC](https://arrow.apache.org/adbc/) 驱动管理器。

## 功能

- 打开/连接数据库：SQLite 文件、任意 ADBC driver（PostgreSQL / MySQL / DuckDB …）
- 表与视图列表、虚拟化数据表格（Lib-DataTable）、分页、排序、`WHERE` 筛选
- SQLite 使用 `rowid`，其它数据库使用主键约束做行标识；支持行内编辑、添加行、删除行
- SQL 控制台：结果表格、影响行数、执行后自动刷新对象列表
- 单色黑白主题，参考 `llm-switch`：侧栏、卡片、描边和等宽数据单元
- 启动参数：`dbpp /path/to/file.sqlite`，或 `DBPP_OPEN=uri DBPP_DRIVER=/path/to/driver.so dbpp`

## 构建

依赖：CMake ≥ 3.30、Ninja、GCC/Clang（C++20）、GTK4 ≥ 4.14、libepoxy、libsoup3、
SQLite3 开发包。ADBC 以源码形式 vendored 在 `third_party/adbc`，内置 fmt/nanoarrow。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/dbpp
```

`DBPP_HUXERUI_DIR` 默认指向 `../HuxerUI-fork`，`DBPP_ADBC_DIR` 指向
`third_party/adbc`，`DBPP_DATATABLE_DIR` 指向 `third_party/Lib-DataTable`。

## ADBC 多数据库

SQLite 驱动由构建一起产出，并通过编译期 `DBPP_SQLITE_DRIVER_PATH` 指向。
运行时可覆盖：

```sh
DBPP_SQLITE_DRIVER=/path/to/libadbc_driver_sqlite.so ./build/dbpp
```

“连接…”对话框接受任意 ADBC driver 的完整路径和 URI（可选用户名/密码），例如：

| 数据库 | driver | URI 示例 |
| --- | --- | --- |
| SQLite | `libadbc_driver_sqlite.so` | `/tmp/app.sqlite` |
| PostgreSQL | `libadbc_driver_postgresql.so` | `postgresql://host/db` |
| MySQL | `libadbc_driver_mysql.so` | `mysql://user@host/db` |
| DuckDB | `libadbc_driver_duckdb.so` | `/tmp/app.duckdb` |

默认只从绝对路径加载 driver；需要把对应 ADBC 驱动编译/安装到本机。
分页使用 `LIMIT … OFFSET …`，标识符按方言引用（MySQL 使用反引号，其余双引号）。

## 目录

```
src/adbc_session.*   ADBC 句柄、Arrow 流读取、对象/模式元数据
src/db.cpp           表格分页、行编辑/删除/插入、SQL 异步封装
src/app.cpp          HuxerUI 壳、侧栏、数据表、SQL 页、对话框
src/theme.*          黑白主题与共享控件
third_party/         ADBC、Lib-DataTable 与 vendor 依赖
platform/            平台入口
```
