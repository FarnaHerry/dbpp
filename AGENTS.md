# db++ AI 开发规范

阅读、修改或评审代码前先读本文件。db++ 是单页 HuxerUI 桌面应用（SQLite 优先、
ADBC 兼容其它数据库），规模不大，规则直接索引到源文件；项目级记忆与专项技能统一
存放在仓库根的 `project-memory/` 与 `project-skills/`（不写入外部或隐藏目录），
仅在任务命中对应领域时按需检索读取，避免无条件全量加载。

## 按任务必读

开始工作和任务范围变化时重新匹配；同时涉及多个领域则各条都读。路径均相对仓库根。

| 任务/触及范围 | 必须先读 |
| --- | --- |
| 任何修改后的构建/验证 | 本文件「最低验证命令」与 run.sh |
| 任何 UI 改动（壳、侧栏、对话框、表格、SQL 页） | src/ui/app_shell.cpp（及 src/ui/*.cpp）、src/ui/theme.h、project-skills/dbpp-ui-layout/SKILL.md（先用既有控件如 PasswordField，不重复造） |
| 数据库访问、分页、行编辑、SQL / Redis 执行 | src/types.cppm、src/db.cppm、src/db.cpp、src/adbc_session.cppm、src/redis_client.cppm |
| CLI 子命令、参数解析、JSON/表格输出 | src/cli.cppm、src/cli.cpp、platform/*/main.cpp、tests/test_cli.cpp |
| ADBC 管理器、dbpp.sqlite3 与在线安装 | src/config_db.*、src/db_profiles_limit.cpp、src/driver_installer.* 与 README |
| 托盘、单实例、关闭策略、轻量级模式、设置存储 | src/ui/tray.h 注释、src/ui/tray.cpp、src/instance.*、src/settings.*、src/config_db.* |
| 国际化（中/英、语言切换） | src/i18n.*、resources/strings/{default,zh}.properties、src/settings.*（GUI 走 ProvideEnvironment/UseString，Worker/后端走 Tr/TrFmt） |
| 主题/配色 | src/ui/theme.h、src/ui/theme_palette.h、src/ui/theme_palette.cpp、src/ui/theme.cpp（预设 + SemanticColors Token，禁止散落硬编码色值） |
| 图标与打包 | resources/（运行时资源包，app::images::* / app::strings::*）、assets/（平台二进制图标）、platform/ 与 CMakeLists.txt |
| CI、安装器与发版 | .github/workflows/build.yml、cmake/{CxxImportStdGate,FindSQLite3}.cmake、platform/{windows,macos}/ 与 tests/ |

## 全局底线

- 修改前检查 `git status`，保留已有改动；不擅自 reset、restore 或清理无关文件。未经用户明确要求，不提交、打标签或推送。
- 采用 **CMake >= 4.4 + C++23 (`cxx_std_23`) + C++ Modules (`src/*.cppm` + `import std;`)**：领域逻辑放 `dbpp_core` 模块，仅 UI 壳头文件（`src/ui/` 下 `ui_common.h` / `theme.h` / `tray.h` / `root.h`）保留传统 `#include`。每次修改后重新编译 dbpp 与测试并运行 `git diff --check`。
- 单个源文件最多 **1000 行**（新增代码按职责模块化拆分，存量超限文件随迭代逐步拆分），禁止为了凑行数强行合并无关的行或压缩代码可读性。
- 左侧栏 `TreeRow` 采用固定每级缩进且为不可展开叶子节点预留 `12px` 前导占位以保持纵向对齐；连接行关闭按钮放在条目内部末尾（`TreeRowOptions::trailing_action`），保持紧凑整行；右侧工作区贴边且与侧栏通过限宽 `SplitLayout` 拖拽分隔。
- ADBC 会话唯一访问路径是 `AdbcSession::sequence` 串行 Worker：阻塞查询/编辑一律放任务线程（`co_await *Async`），不得在组合回调里同步调用。
- 应用配置与连接统一持久化在 `dbpp.sqlite3`（使用 HuxerUI `Lib-SQLite` + WAL 模式，POSIX 下设为 0600 权限，自动迁移旧版 `connections.tsv` / `settings.tsv` 且保留原文件）。
- 托盘拥有应用生命周期：AppOptions 必须保持 `quit_on_last_window_closed = false`、`create_initial_window = false`；窗口一律经 `TrayRuntime::ShowWindow`（OpenWindow）创建，不要绕过托盘直接退出。
- 轻量级模式关闭窗口会销毁全部 UI 状态并断开所有 ADBC 会话，唤出后是干净状态——这是设计行为。托盘不可用时关闭必须走确认退出对话框，不得让窗口无入口可唤回。
- `MainWindowOptions`（src/ui/tray.cpp）与 AppOptions.window 必须一致；UI 根工厂唯一来源是 src/ui/root.h 的 AppRoot。
- UI 颜色、渐变与控件样式统一经 `src/ui/theme.*` 的预设与 `SemanticColors` Token 管理，禁止在业务页面散落硬编码色值；桌面端与后端文案经 `resources/strings/{default,zh}.properties` 与 `src/i18n.*` 维护中/英双语（默认跟随系统并可在设置即时切换，保持 CLI 输出与 JSON schema 稳定）。运行时资源放 resources/，Windows .ico 与 Linux 安装图标放 assets/，macOS .icns 放 platform/macos/，三者职责不混用。
- 平台入口只在 platform/<os>/main.cpp；Linux 桌面集成依赖二进制名与 dbpp.desktop 的 `Exec=dbpp`、`StartupWMClass=dbpp` 一致，改名需同步。

## 最低验证命令

```bash
cmake --build build --target dbpp
ctest --test-dir build --output-on-failure --no-tests=error
git diff --check
```

托盘、关闭与轻量级模式属于 GUI 行为，需要显示环境实机验证：`./run.sh`；
SQLite 驱动路径可用 `DBPP_SQLITE_DRIVER` 覆盖（run.sh 已自动指向构建产物）。
编译通过不等于托盘/窗口行为验收。

## 规范维护

- 本文件最多 **150 行、6,000 字符**，两项同时满足；行数含空行，字符按 LF 全文统计。
- 项目级记忆统一放入 `project-memory/*.md`，项目级技能统一放入 `project-skills/<name>/SKILL.md`（详见各自 `README.md`），不写入外部全局目录；所有 memory 与 skill 仅在当前任务明确命中对应领域时按需检索并读取，禁止每次会话无条件加载。
- 跨文件共享声明集中在 `src/types.cppm` 与对应 `src/*.cppm` 模块接口（强模块所有权）；新模块先写清 `.cppm` 职责注释再实现。
- 流程或目录变化时同步 README.md、本文件与 CMakeLists.txt 注释；删除约束需用户明确改变产品要求或有代码证据证明过时。
