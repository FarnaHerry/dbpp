# db++

SQLite 优先、通过 **Apache Arrow ADBC** 兼容其它数据库的桌面数据管理器。
UI 使用本地 [HuxerUI](../HuxerUI-fork)（`farna/main`）和
[HuxerUI/Lib-DataTable](https://github.com/HuxerUI/Lib-DataTable)；数据库访问统一走
[Apache Arrow ADBC](https://arrow.apache.org/adbc/) 驱动管理器。

## 功能

- 打开/连接数据库：SQLite 文件、任意 ADBC driver（PostgreSQL / MySQL / DuckDB …）
- ADBC 管理器：保存连接配置（不含密码）、发现本机驱动、从列表中选择驱动/连接并删除配置
- 表与视图列表、虚拟化数据表格（Lib-DataTable）、分页、排序、`WHERE` 筛选
- SQLite 使用 `rowid`，其它数据库使用主键约束做行标识；支持行内编辑、添加行、删除行
- SQL 控制台：结果表格、影响行数、执行后自动刷新对象列表
- 系统托盘：托盘图标与菜单（显示主窗口 / 启用托盘 / 轻量级模式 / 退出），
  托盘拥有应用生命周期；托盘不可用时关闭会弹确认退出
- 轻量级模式：关闭窗口时销毁整个界面（断开全部 ADBC 会话），从托盘唤出时重建，
  空闲内存占用更低；设置页可开关，持久化在 `settings.tsv`
- 单色黑白主题与应用图标（标题栏 logo / 托盘图标 / Windows .ico / macOS .icns /
  Linux hicolor 图标 + desktop 文件）
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

## ADBC 管理器

侧栏“ADBC 管理器…”用于管理连接和驱动：

- 连接配置保存在应用数据目录的 `connections.tsv`（表格格式，密码不落盘）。
- 驱动列表来自 `DBPP_ADBC_DRIVERS`、`ADBC_DRIVER_PATH`、构建目录以及系统库目录中的
  `libadbc_driver_*`；点击驱动填入路径，点击已保存连接回填表单。
- 头部按钮：关闭 / 删除 / 保存 / 连接。连接会先保存当前配置，再打开 ADBC 会话。

## 托盘与轻量级模式

- 托盘菜单：显示主窗口、启用托盘图标、轻量级模式、退出；两个开关与「设置」页
  （标题栏右上角齿轮）等价，改动即时持久化到应用数据目录的 `settings.tsv`（key/value TSV）。
- 托盘启用时，关闭窗口 = 隐藏到托盘（窗口保活）；轻量级模式开启时，关闭窗口 =
  销毁整个原生窗口与 UI 树，托盘保留运行时，唤出时经 `OpenWindow` 重建（数据库
  连接随窗口销毁，重新打开是干净状态）。
- 托盘不可用（宿主缺失/关闭托盘）时，关闭窗口会弹出确认退出对话框。
- Linux 托盘走 StatusNotifierItem（KDE/GNOME 插件等宿主），Windows 走
  `Shell_NotifyIcon`，macOS 走 `NSStatusBar`，均由 HuxerUI 引擎平台层提供。

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
src/app.cpp          HuxerUI 壳、侧栏、数据表、SQL 页、设置页、对话框
src/theme.*          黑白主题与共享控件
src/tray.*           托盘运行时：图标/菜单、关闭策略、轻量级模式
src/settings.*       settings.tsv 读写与应用级 SettingsModel
resources/           运行时资源包（应用内 logo、托盘图标）
assets/              平台二进制图标（Linux icon.png、Windows icon.ico）
third_party/         ADBC、Lib-DataTable 与 vendor 依赖
platform/            平台入口与打包文件（desktop、app.rc、icns）
```
