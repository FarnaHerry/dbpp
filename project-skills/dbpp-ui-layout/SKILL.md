---
name: dbpp-ui-layout
description: >-
  On-demand db++ desktop UI layout, sidebar tree alignment, Gemini 4-color icon
  tokens, and compact toolbar/status-bar conventions. Load ONLY when modifying
  db++ UI layout, sidebar tree rows, theme tokens, or pagination/toolbar UI.
---

# db++ UI 布局与组件规范（按需查阅）

> 仅在改动侧栏树、工作区布局、工具栏/状态栏或主题色板时读取本 skill，无需在无关任务中加载。

## 1. 左侧栏树形行（`TreeRow` — `src/ui/theme.h` / `src/ui/theme.cpp`）
- **固定缩进与统一折叠槽位**：
  - `kBaseLeftPadding = 6.0F`，每级固定步进 `kIndentStep = 14.0F`（`left = 6 + indent_level * 14`）。
  - 所有层级必须固定预留 `12.0F` 前导折叠图标槽位（`kChevronSlot = 12.0F`）：
    - 可展开节点绘制 `chevron_down` / `chevron_right`；
    - 不可展开的叶子节点（表、视图、字段提示等）同样填充 `12.0F × 12.0F` 透明 `Spacer` 占位，保证同级图标纵向对齐整齐。
- **紧凑行高与行内操作按钮**：
  - 单行最小高度 `24.0F`，圆角 `5.0F`，行内主区域内边距 `top/bottom = 3.0F`、`Spacing{4.0F}`。
  - 每个连接的关闭按钮通过 `TreeRowOptions::trailing_action` 嵌入 `TreeRow` 内部末尾，作为可点击 `main_area` 的兄弟节点放在同一行容器内，禁止将关闭按钮独立放在 `TreeRow` 外侧导致长度错落与悬浮/边框割裂。

## 2. Gemini 四色语义配色（`src/ui/theme_palette.*`）
- **基础语义与对象图标着色**：
  - **数据库图标（`accent_database`）**：Gemini 黄（深色 `#FBBC05` / 浅色 `#F4B400`）
  - **表图标（`accent_table`）**：Gemini 绿（`#34A853`）
  - **视图 / 物化视图图标（`accent_view`）**：Gemini 蓝（`#4285F4` / `#6AA6FF`）
  - **SQL 控制台（`accent_sql`）**：Gemini 红（`#EA4335` / `#F26C60`）
  - **连接状态指示点**：绿（`status_connected = #34A853`）、红（`status_failed = #EA4335` + Tooltip）、灰（未连接），不显示多余状态文字。
- `Monochrome` 预设保留纯中性灰阶覆盖；其余彩色预设统一复用四色对象语义；禁止在业务页面散落硬编码 RGB。

## 3. 工作区紧凑分栏与工具栏 / 状态栏（`src/ui/app_shell.cpp`、`sidebar.cpp`、`table_browser_page.cpp`、`sql_page.cpp`）
- **可拖拽分栏**：主工作区使用 HuxerUI `SplitLayout` 分隔左侧栏（默认 `240.0F`，限宽 `180.0F ~ 420.0F`）与右侧内容区（`min_width = 420.0F` + `Grow(1.0F)`）。
- **右侧贴边紧凑布局**：`main_content` 不设外层边距与额外间距；表格（`row_height = 26.0F`、`header_height = 28.0F`）及底部 `BottomStatusBar`（`32.0F` 高直角横条）直接贴边对齐。
- **单行等高工具栏**：`TableBrowserPage` 的筛选框（`CompactTextField`）与操作按钮（`CompactActionButton`）及排序下拉统一锁定 `28.0F` 高度位于同一行；SQL 页顶栏选择器与执行按钮同为 `28.0F`。
- **序号首列与无边框紧凑底栏**：表格与 SQL 结果第一列固定为不可排序/不可编辑的 `#` 序号列（按分页偏移计算 1-based 全局行号）；底栏从左到右为 `[耗时]` `[执行 SQL + 复制图标]` `Spacer()` `[总行数 · 共 N 页]` `[每页 X 行]` `[<< < [当前页] > >>]`（去掉斜杠 `1 / N`），且底栏内仅当前页号输入框保留描边边框，其余徽章与翻页按钮均无边框线。
