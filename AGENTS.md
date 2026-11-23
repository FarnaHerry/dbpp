# db++ AI 开发规范

阅读、修改或评审代码前先读本文件。db++ 是单页 HuxerUI 桌面应用（SQLite 优先、
ADBC 兼容其它数据库），规模不大，规则直接索引到源文件，不设独立技能目录。

## 按任务必读

开始工作和任务范围变化时重新匹配；同时涉及多个领域则各条都读。路径均相对仓库根。

| 任务/触及范围 | 必须先读 |
| --- | --- |
| 任何修改后的构建/验证 | 本文件「最低验证命令」与 run.sh |
| 任何 UI 改动（壳、侧栏、对话框、表格、SQL 页） | src/app.cpp、src/theme.h（先用既有控件，不重复造） |
| 数据库访问、分页、行编辑、SQL 执行 | src/model.h（跨文件声明的唯一头）、src/db.cpp、src/adbc_session.h |
| ADBC 管理器、connections.tsv 与在线安装 | src/db.cpp 的 Load/SaveConnectionProfiles、src/driver_installer.* 与 README「ADBC 管理器」 |
| 托盘、关闭策略、轻量级模式、settings.tsv | src/tray.h 注释、src/tray.cpp、src/settings.cpp |
| 主题/配色 | src/theme.*：只有单色黑白，禁止引入新色相 |
| 图标与打包 | resources/（运行时资源包，app::images::*）、assets/（平台二进制图标）、platform/ 与 CMakeLists.txt |

## 全局底线

- 修改前检查 `git status`，保留已有改动；不擅自 reset、restore 或清理无关文件。未经用户明确要求，不提交、打标签或推送。
- 每次修改任何文件后都重新编译 dbpp 目标并运行 `git diff --check`；构建失败不得宣称完成，最终说明实际命令、结果与未覆盖范围。
- ADBC 会话唯一访问路径是 `AdbcSession::sequence` 串行 Worker：阻塞查询/编辑一律放任务线程（`co_await *Async`），不得在组合回调里同步调用。
- connections.tsv 不落密码，密码只随 ConnectionRequest 在内存传递。settings.tsv 同为 TSV：先写临时文件再原子替换，失败保留原文件。
- 托盘拥有应用生命周期：AppOptions 必须保持 `quit_on_last_window_closed = false`、`create_initial_window = false`；窗口一律经 `TrayRuntime::ShowWindow`（OpenWindow）创建，不要绕过托盘直接退出。
- 轻量级模式关闭窗口会销毁全部 UI 状态并断开所有 ADBC 会话，唤出后是干净状态——这是设计行为。托盘不可用时关闭必须走确认退出对话框，不得让窗口无入口可唤回。
- `MainWindowOptions`（src/tray.cpp）与 AppOptions.window 必须一致；UI 根工厂唯一来源是 src/root.h 的 AppRoot。
- UI 只用 theme.* 共享控件与单色黑白主题，文案用中文。运行时资源放 resources/，Windows .ico 与 Linux 安装图标放 assets/，macOS .icns 放 platform/macos/，三者职责不混用。
- 平台入口只在 platform/<os>/main.cpp；Linux 桌面集成依赖二进制名与 dbpp.desktop 的 `Exec=dbpp`、`StartupWMClass=dbpp` 一致，改名需同步。

## 最低验证命令

```bash
cmake --build build --target dbpp
git diff --check
```

托盘、关闭与轻量级模式属于 GUI 行为，需要显示环境实机验证：`./run.sh`；
SQLite 驱动路径可用 `DBPP_SQLITE_DRIVER` 覆盖（run.sh 已自动指向构建产物）。
编译通过不等于托盘/窗口行为验收。

## 规范维护

- 本文件最多 **150 行、6,000 字符**，两项同时满足；行数含空行，字符按 LF 全文统计。
- 跨文件共享声明集中在 src/model.h；新模块先在头文件写清职责注释再实现。
- 流程或目录变化时同步 README.md、本文件与 CMakeLists.txt 注释；删除约束需用户明确改变产品要求或有代码证据证明过时。
