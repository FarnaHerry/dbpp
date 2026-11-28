# db++

SQLite 优先、通过 **Apache Arrow ADBC** 兼容其它数据库的桌面数据管理器。
UI 使用本地 [HuxerUI](../HuxerUI-fork)（`farna/main`）和
[HuxerUI/Lib-DataTable](https://github.com/HuxerUI/Lib-DataTable)；数据库访问统一走
[Apache Arrow ADBC](https://arrow.apache.org/adbc/) 驱动管理器。

## 功能

- 顶部工具栏与统一连接树：左上角提供「新建连接 / 新建查询 / 驱动管理」快捷按钮，左侧栏以单一可滚动树统一展示已连接会话与已保存连接；MySQL 等多库/多 Schema 连接自动按「连接 -> 数据库 -> 对象类型（表/视图/物化视图/外部表）-> 对象」四层分类展示并折叠系统库，支持拖拽连接行调整顺序并持久化到 `dbpp.sqlite3`，以及右键菜单编辑连接、刷新、新建查询、查看结构、复制名称/URI/SELECT、删除对象和断开/删除配置
- 打开/连接与编辑数据库：两步式「新建连接」向导与右键「编辑连接…」共用一致的按类型定制表单字段（文件浏览 / 主机+端口+库名与 URI 双向同步 / 原始 URI），所有密码框内置尾部小眼睛图标切换明/暗文；内置 15 种数据库品牌图标与一键安装 ADBC 驱动
- Redis 内置支持：零外部依赖 RESP2/RESP3 TCP 客户端，左侧按 `db0`..`db15` 展示逻辑库与键计数，右侧虚拟表格支持 `MATCH` 模式筛选、分页/排序、内联编辑（String 保留 TTL、Hash/List/Set/ZSet JSON 编辑、TTL `EXPIRE`/`PERSIST`、`RENAMENX` 改名、批量新建与删除键），并内置 Redis 命令控制台
- ADBC 管理器：保存连接配置（默认保留密码）、发现本机驱动、在线安装/更新/卸载 ADBC 驱动、
  从列表中选择驱动/连接并删除配置
- 单行紧凑表与视图列表、右侧多标签页工作区（同屏打开并独立保留多个表、视图与 SQL 查询页状态）
- 虚拟化数据表格（Lib-DataTable）：单元格与表头单行显示、超出宽度自动省略号（不换行），首次打开按采样内容自动调节初始列宽，后续保留用户手动拖拽拉伸的列宽；支持底部分页、排序、`WHERE` 筛选（SQLite 使用 `rowid`，其它数据库使用主键约束做行标识）
- 行内编辑、底部内联空白草稿行添加（未提交前不做入库校验，支持「提交」事务写入或「回滚」放弃）与删除行
- SQL 控制台：独立查询标签页、结果表格、影响行数、执行后自动刷新对象列表
- 系统托盘与深浅色同步：托盘图标与菜单（显示主窗口 / 启用托盘 / 轻量级模式 / 退出），
  托盘拥有应用生命周期，且原生托盘菜单（Windows / macOS）随设置/系统深浅色自动同步；托盘不可用时关闭会弹确认退出
- 桌面单实例：同一时间只允许运行一个 GUI 实例；重复启动或通过桌面打开文件会唤醒已有窗口并转发待打开数据库（CLI 子命令仍可并发无头运行）
- 无头 CLI：`dbpp <command> ...`，无需 GUI/显示服务即可直接查表、执行 SQL、管理 `dbpp.sqlite3` 连接配置与在线安装/卸载 ADBC 驱动，支持 `--json` 结构化输出供 AI Agent 调用
- 轻量级模式：关闭窗口时销毁整个界面（断开全部 ADBC 会话），从托盘唤出时重建，
  空闲内存占用更低；设置页可开关，持久化在 `dbpp.sqlite3`
- 多预设主题、双语界面与图标：默认采用「Gemini 星光」分层配色（蓝-紫-珊瑚星光渐变与四色语义高亮），支持切换翡翠青 / 暖沙琥珀 / 暮光紫 / 极简黑白及跟随系统/深/浅模式；内置简体中文 / English 双语界面（默认跟随系统语言，可在设置页即时切换）并统一持久化至 `dbpp.sqlite3`；附带应用图标（Windows .ico / macOS .icns / Linux hicolor + desktop 文件）与托盘图标
- 启动参数：无参或 `dbpp /path/to/file.sqlite`（以及 `dbpp gui` / `dbpp open`）启动 GUI；`dbpp <command>` 走无头 CLI；也可用 `DBPP_OPEN=uri DBPP_DRIVER=/path/to/driver.so dbpp`

## 安装

### 一键安装（推荐，免管理员权限）

**Linux (`x86_64` / `aarch64`) & macOS (`Apple Silicon`)：**

```sh
curl -fsSL https://github.com/FarnaHerry/dbpp/releases/latest/download/install.sh | bash
```

**Windows (`x86_64` PowerShell)：**

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://github.com/FarnaHerry/dbpp/releases/latest/download/install.ps1 | iex"
```

常用环境变量 / 参数：
- 指定版本：`DBPP_VERSION=0.4.9`（或传 `--version 0.4.9` / `-Version 0.4.9`）
- 镜像/代理前缀：`DBPP_DOWNLOAD_BASE=https://mirror.example.com/dbpp/v0.4.9`
- Windows 拉起图形安装器：`DBPP_USE_INSTALLER=1`（或 `-UseInstaller`）
- 卸载（保留用户数据 `dbpp.sqlite3` 与已安装的 ADBC 驱动）：
  - Linux / macOS：`curl -fsSL https://github.com/FarnaHerry/dbpp/releases/latest/download/install.sh | bash -s -- --uninstall`
  - Windows：`powershell -ExecutionPolicy ByPass -c "$env:DBPP_UNINSTALL='1'; irm https://github.com/FarnaHerry/dbpp/releases/latest/download/install.ps1 | iex"`

也可直接在 [GitHub Releases](https://github.com/FarnaHerry/dbpp/releases) 手动下载各平台安装包：Windows `.exe` 安装器 / `.zip`、Linux `.deb` / `.rpm` / `.tar.xz`、macOS `.dmg`（均附带 `SHA256SUMS.txt`）。

## 构建与测试

依赖：CMake ≥ 4.4、Ninja、支持 C++23 模块与 `import std` 的现代编译器（GCC ≥ 16 / Clang ≥ 21 + libc++ / MSVC）；Linux 源码构建需要 GTK4 ≥ 4.14、
libepoxy、libsoup3 开发包。SQLite3 统一使用 [HuxerUI/Lib-SQLite](../HuxerUI-libs/Lib-SQLite) 自带源码（`3.53.4`，无重复 vendored 副本），ADBC 与 Lib-DataTable 均源码构建，开箱即编。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/dbpp
```

`DBPP_HUXERUI_DIR` 自动优先检测 `third_party/huxerui`，否则回落到
`../HuxerUI-fork`；`DBPP_SQLITE_LIB_DIR` 自动优先检测 `third_party/Lib-SQLite`，否则回落到
`../HuxerUI-libs/Lib-SQLite`；`DBPP_ADBC_DIR` 指向 `third_party/adbc`，`DBPP_DATATABLE_DIR`
指向 `third_party/Lib-DataTable`。

## ADBC 管理器

侧栏“ADBC 管理器…”用于管理连接和驱动：

- 连接配置与偏好设置统一保存在应用数据目录的 `dbpp.sqlite3`（基于 `HuxerUI::SQLite` WAL 模式，默认保留可选密码，POSIX 下以 `0600` 权限保存；首次启动自动迁移旧版 `connections.tsv` 与 `settings.tsv` 并保留原文件；可在侧栏右键「编辑连接…」随时修改）。
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

## CLI（AI Agent / 终端操作）

同一份 `dbpp` 可执行文件在传入子命令时完全无头运行（不初始化 GTK/Win32/AppKit 窗口或托盘），与桌面端共用 `connections.tsv` 与用户 ADBC 驱动目录。全局支持 `--json`（等同 `--format json`，成功输出 `{"ok":true,...}`，错误向 stderr 输出 `{"ok":false,"error":"..."}`）、`--format table|tsv|csv|json`，退出码约定为 `0`（成功）/ `1`（运行时或数据库错误）/ `2`（用法或参数错误）。

常用命令：

```sh
# 诊断本机环境（版本、数据目录、SQLite 驱动、已发现驱动）
dbpp doctor --json

# 管理 ADBC 驱动（与 GUI「ADBC 管理器」共用目录）
dbpp driver list --json
dbpp driver catalog --filter postgres --json
dbpp driver install postgresql --json
dbpp driver uninstall postgresql --json

# 管理已保存连接（passwords 不落盘；查询时通过 --password-env 传入）
dbpp profile save pg --driver postgresql --uri postgresql://127.0.0.1:5432/myapp --user dev
dbpp profile list --json
dbpp profile show pg --json
dbpp profile remove pg --json

# 查看对象 / 表结构 / 分页数据 / 执行 SQL（支持 SQL 字面量、--file 或 stdin `-`）
dbpp tables --sqlite ./app.sqlite --json
dbpp schema --sqlite ./app.sqlite items --json
dbpp rows   --sqlite ./app.sqlite items --where "id >= 1" --order id:desc --limit 20 --json
dbpp query  --sqlite ./app.sqlite "SELECT id, name FROM items ORDER BY id" --read-only --json
dbpp exec   --sqlite ./app.sqlite --script --file ./init.sql --json
dbpp sql    --sqlite ./app.sqlite - --json <<'SQL'
SELECT COUNT(*) AS total FROM items;
SQL

# 复用已保存的 PostgreSQL 配置（通过环境变量传密码，不暴露到进程命令行或磁盘）
PGPASS=secret dbpp tables -p pg --password-env PGPASS --json

# Redis 无头操作（--redis，支持 db0..db15、MATCH 筛选、命令执行与 --read-only）
dbpp tables --redis redis://127.0.0.1:6379/0 --json
dbpp rows   --redis redis://127.0.0.1:6379/0 db0 --where "user:*" --json
dbpp query  --redis redis://127.0.0.1:6379/0 "INFO server" --read-only --json
```

完整说明可执行 `dbpp --help` 或 `dbpp <command> --help`。

## 目录

```
src/*.cppm             领域层 C++23 模块接口（dbpp.types / dbpp.settings / dbpp.adbc / dbpp.db / dbpp.cli 等）
src/cli.*              无头 CLI 解析、JSON/TSV/CSV/表格输出、Agent 子命令分发
src/adbc_session.*     ADBC 句柄、Arrow 流读取、URI 拆解/拼装、多库命名、manifest 发现
src/redis_client.*     内置零依赖 Redis RESP2/RESP3 客户端、键浏览器与命令控制台适配
src/driver_installer.* ADBC Driver Registry 目录拉取、SHA-256 校验与安全安装/卸载
src/db.*               表格分页、行编辑/删除/插入、SQL 异步封装
src/ui/                HuxerUI 壳、四层数据库侧栏、统一连接/编辑表单、数据表、SQL 页、设置页、主题色板与托盘运行时
src/i18n.*             中/英双语解析（跟随系统 / 手动覆盖）与后端消息翻译
src/instance.*         跨平台 GUI 单实例锁与唤出/文件转发通道
src/settings.*         dbpp.sqlite3 / 旧版 settings.tsv 读写与应用级 SettingsModel
resources/             运行时资源包（数据库/密码显隐 SVG、logo、托盘图标、中/英双语 strings/*.properties）
assets/                平台二进制图标（Linux icon.png、Windows icon.ico）
cmake/                 构建模块（CxxImportStdGate.cmake 与 vendored SQLite3 探测 FindSQLite3.cmake）
third_party/           ADBC、Lib-DataTable 与 vendor 依赖
platform/              平台入口与打包/安装器配置（desktop、WiX、plist、icns）
scripts/               跨平台一键安装与卸载脚本（install.sh / install.ps1）
tests/                 无窗口核心单元测试（CTest）
.github/workflows/     多平台桌面 CI 与 Release 构建流水线（Linux/Windows/macOS）
docs/releases/         各版本 Release 说明
```
