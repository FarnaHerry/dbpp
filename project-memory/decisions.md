# 项目关键决策与避坑记录

## 1. 打包与 CI
- **Fedora/RPM `/usr/lib/.build-id` 冲突**：`HuxerUI` 运行时包含 `libc++.so.1.0` / `libc++abi.so.1.0`，若打包为 RPM 时启用 build-id 软链或自动 Req/Prov，会与其他使用同版 HuxerUI 的包（如 `clash-flux`）冲突。`CMakeLists.txt` 必须保留 `CPACK_RPM_PACKAGE_AUTOREQPROV "no"` 与 `CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none"`，`.github/workflows/build.yml` 中的 RPM 校验禁止包含 `/usr/lib/.build-id/`。
- **macOS DMG CI 路径**：`huxerui package --prefix` 在 CI 中必须传入绝对路径（如 `"$PWD/.huxerui/package/macos/release"`），不可传相对路径。

## 2. 查询分页与 SQL 自动 LIMIT
- **页大小限制**：取消硬上限限制，允许任何 `>= 1` 的整数（上限按 `INT64_MAX` 防溢出，偏移乘法使用安全防溢出函数 `SafePageOffset`）。
- **SQL 自动分页**：仅当 SQL 为读语句且未显式写 `LIMIT`/`OFFSET` 时，在执行期隐式附加 `LIMIT`/`OFFSET` 并传入底栏「完整执行 SQL」展示；编辑器文本保持原样不改写。
- **即改即生效**：设置页或页大小弹窗修改每页条数后，立即同步更新全局默认值与各打开标签页（无未提交草稿行时立刻刷新）。

## 3. HuxerUI / C++23 编写注意项
- **`[[huxerui::composable]]` 参数只读性**：组合体生成 lambda 默认非 `mutable`，对 `View` / `StringVariant` 参数调用右值修饰器（如 `.With(...)`）前需先拷贝到局部变量。
- **指定初始化字段顺序**：`Frame` 顺序为 `{width, height, min_width, max_width, min_height, max_height}`；`EdgeInsets` 顺序为 `{top, right, bottom, left}`。
- **`SplitLayout` 状态回写**：子面板必须带稳定 `.Key(...)`，`OnChanged` 中必须同时回写 `Frame` 与 `Grow` 并再次钳制最小/最大宽度约束。
- **应用图标裁切留白**：生成平台图标与托盘/Logo 时以高清母版裁掉透明边缘并保留约 `90%–92%` 画布占比。
