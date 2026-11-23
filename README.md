# db++

SQLite 优先、通过 **Apache Arrow ADBC** 兼容其它数据库的桌面数据管理器。
UI 使用本地 [HuxerUI](../HuxerUI-fork)（`farna/main`）和
[HuxerUI/Lib-DataTable](https://github.com/HuxerUI/Lib-DataTable)；数据库访问统一走
[Apache Arrow ADBC](https://arrow.apache.org/adbc/) 驱动管理器。

## 功能

- 打开/连接数据库：SQLite 文件、任意 ADBC driver（PostgreSQL / MySQL / DuckDB …）
- ADBC 管理器：保存连接配置（不含密码）、发现本机驱动、在线安装/更新/卸载 ADBC 驱动、
  从列表中选择驱动/连接并删除配置
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

## 构建与测试

依赖：CMake ≥ 3.30、Ninja、GCC/Clang/MSVC（C++20）；Linux 需要 GTK4 ≥ 4.14、
libepoxy、libsoup3 开发包。ADBC 与 SQLite3（`DBPP_USE_VENDORED_SQLITE3=ON`）以源码形式
vendored 在 `third_party/adbc`，开箱即编。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/dbpp
```

`DBPP_HUXERUI_DIR` 自动优先检测 `third_party/huxerui`，否则回落到
`../HuxerUI-fork`；`DBPP_ADBC_DIR` 指向 `third_party/adbc`，`DBPP_DATATABLE_DIR`
指向 `third_party/Lib-DataTable`。

## ADBC 管理器

侧栏“ADBC 管理器…”用于管理连接和驱动：

- 连接配置保存在应用数据目录的 `connections.tsv`（表格格式，密码不落盘）。
- 驱动列表同时发现两类来源：
  1. 标准 ADBC driver manifest（`.toml`）：按 `ADBC_DRIVER_PATH`、`DBPP_ADBC_DRIVERS`、
     `$VIRTUAL_ENV/etc/adbc/drivers`、`$CONDA_PREFIX/etc/adbc/drivers`、用户配置目录
     （Linux `~/.config/adbc/drivers`、macOS `~/Library/Application Support/ADBC/Drivers`、
     Windows `%LOCALAPPDATA%\ADBC\Drivers`，兼容 `dbc install <slug>`）和系统目录
     （`/etc/adbc/drivers`）顺序扫描，点击即填入短名或 `.toml` 路径。
  2. 共享库（`libadbc_driver_*`）：扫描 `DBPP_ADBC_DRIVERS`、`ADBC_DRIVER_PATH`、
     构建目录以及系统库目录，点击填入绝对路径。
- 头部按钮：关闭 / 删除 / 保存 / 连接；可切换至「安装驱动」页签或点击「安装驱动…」。
- 内置「安装驱动」页签：直接浏览 ADBC Driver Registry（默认 `https://dbc-cdn.columnar.tech`，
  可用 `DBPP_ADBC_REGISTRY_URL` 覆盖），支持按关键字筛选、`HttpClient` 流式下载进度、
  SHA-256 校验、安全解压到用户驱动目录下的 `.packages/<slug>/<version>/`，并原子写入
  标准 ADBC v1 `<slug>.toml` manifest 到用户配置目录，同时支持更新、重装与卸载。

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

SQLite 驱动随构建一起产出并打包到可执行文件同级或 `lib/` / `Frameworks/` 目录，运行时按可执行文件相对路径自动定位，最后回落到编译期 `DBPP_SQLITE_DRIVER_PATH`；也可通过环境变量覆盖：

```sh
DBPP_SQLITE_DRIVER=/path/to/libadbc_driver_sqlite.so ./build/dbpp
```

“连接…”对话框接受任意 ADBC driver（短名如 `postgresql`、`.toml` manifest 路径或动态库完整路径）和 URI（可选用户名/密码），例如：

| 数据库 | driver 示例 | URI 示例 |
| --- | --- | --- |
| SQLite | `libadbc_driver_sqlite.so` | `/tmp/app.sqlite` |
| PostgreSQL | `postgresql` / `libadbc_driver_postgresql.so` | `postgresql://host/db` |
| MySQL | `mysql` / `libadbc_driver_mysql.so` | `mysql://user@host/db` |
| DuckDB | `duckdb` / `libadbc_driver_duckdb.so` | `/tmp/app.duckdb` |

ADBC 驱动管理器启用标准搜索标志（`ADBC_LOAD_FLAG_DEFAULT`），可通过 `dbc install <slug>`、系统包或自定义路径安装对应驱动后直接使用。
分页使用 `LIMIT … OFFSET …`，标识符按方言引用（MySQL 使用反引号，其余双引号）。

## 目录

```
src/adbc_session.*     ADBC 句柄、Arrow 流读取、对象/模式元数据、manifest 发现
src/driver_installer.* ADBC Driver Registry 目录拉取、SHA-256 校验与安全安装/卸载
src/db.cpp             表格分页、行编辑/删除/插入、SQL 异步封装
src/app.cpp            HuxerUI 壳、侧栏、数据表、SQL 页、设置页、对话框
src/theme.*            黑白主题与共享控件
src/tray.*             托盘运行时：图标/菜单、关闭策略、轻量级模式
src/settings.*         settings.tsv 读写与应用级 SettingsModel
resources/             运行时资源包（应用内 logo、托盘图标）
assets/                平台二进制图标（Linux icon.png、Windows icon.ico）
cmake/                 构建模块（如 vendored SQLite3 探测 FindSQLite3.cmake）
third_party/           ADBC、Lib-DataTable 与 vendor 依赖
platform/              平台入口与打包/安装器配置（desktop、WiX、plist、icns）
tests/                 无窗口核心单元测试（CTest）
.github/workflows/     多平台桌面 CI 与 Release 构建流水线（Linux/Windows/macOS）
docs/releases/         各版本 Release 说明
```
