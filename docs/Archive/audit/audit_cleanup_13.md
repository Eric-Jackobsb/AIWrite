# audit_cleanup_13：CLN-13 落地审计报告（composite_editor 拆分）

> 批次：`CLN-13` · 日期：2026-10-20 · 计划：docs/actionPlan/M_code_cleanup.md §3.4
> 性质：**超大文件拆分（可读性）** —— 不改变任何行为，仅分文件。
> 判据：不改变行为；单文件 < 60 KB；构建 0/0 + selftest 全绿。

## 1. 拆分对象与方案

| 项 | 值 |
|---|---|
| 源文件 | `source/ui/src/composite_editor.cpp`（1687 行 / ≈71 KB） |
| 拆分方向 | 按「功能域」拆（画布/上下文 / 状态+撤销 / 会话操作 / 试运行 / 打包 / 绘制+对话框），跨 TU 共享经内部头提升 |
| 落地文件 | `ce_state.cpp` · `ce_session.cpp` · `ce_run.cpp` · `ce_pack.cpp` · `ce_draw.cpp` · `ce_internal.h` |

### 拆分后文件

| 文件 | 行数 | 内容 |
|---|---|---|
| `source/ui/src/composite_editor.cpp` | 172 | 画布域：`g_ce_*` 画布全局（上下文 / 改名 / 画布辅助开关 / 小地图状态）+ `ensure_ce_context` / `ce_open_rename` / `ce_apply_rename` / `ce_apply_snap`（含网格 16px 吸附）+ `kCeSnap*` / `g_ce_prev_positions` |
| `source/ui/src/ce_state.cpp` | 86 | 会话状态 + 撤销域：`composite_editor_state` + 撤销栈（`g_ce_undo`/`g_ce_redo`/`CeUndoEntry`/`ce_push_snapshot`/`ce_undo`/`ce_redo`）+ `composite_editor_can_undo/can_redo/undo_once/redo_once` |
| `source/ui/src/ce_session.cpp` | 210 | 会话操作域：`composite_editor_open` / `request_close` / `force_close` / `save` / `snapshot` / `add_node` / `ce_remove_node` / `delete_node` / `add_edge` |
| `source/ui/src/ce_run.cpp` | 222 | 试运行「执行连线」域：`start_run` / `stop_run` / `clear_run` / `run_active` / `run_snapshot` / `tick_run` + `ce_is_test_source` / `ce_snapshot_entry`（匿名） |
| `source/ui/src/ce_pack.cpp` | 156 | 打包选区域：`composite_pack_selection` |
| `source/ui/src/ce_draw.cpp` | 924 | 编辑器窗口 + 对话框域：`draw_composite_editor` + `ce_handle_delete_key` + `ce_name_buffers`/`ce_name_buffer`（匿名）+ 新建/打包对话框（`composite_new_dialog` / `composite_pack_dialog` / `composite_editor_test_drive_new_dialog` / `draw_composite_dialogs` + 匿名 `run_new_dialog` / `run_pack_dialog` / `create_and_open_new_composite`） |
| `source/ui/include/ce_internal.h` | 78 | 六 TU 内部共享声明：`g_ce_*` 画布全局 extern + `CeUndoEntry`/`g_ce_undo`/`g_ce_redo` + `ensure_ce_context` / `ce_open_rename` / `ce_apply_rename` / `ce_apply_snap` / `ce_push_snapshot` / `ce_remove_node` |

## 2. 依赖分析与共享接口

- **使用矩阵**（grep 统计 draw_composite_editor / 各域对跨 TU 符号的引用）：
  - `ce_draw.cpp`（draw_composite_editor + ce_handle_delete_key）跨 TU 引用：`g_ce_context`(L982) / `ensure_ce_context`(L981) / `ce_open_rename`(L1122/1214) / `ce_apply_rename`(L1199/1202) / `ce_apply_snap`(L1219) / `g_ce_request_fit` / `g_ce_show_minimap` / `g_ce_align_snap` / `g_ce_grid_snap` / `g_ce_show_grid` / `g_ce_minimap` / `g_ce_add_filter` / `g_ce_rename_id` / `g_ce_rename_buf` / `ce_push_snapshot`(L851) / `ce_remove_node`(L853)。
  - `ce_session.cpp` 跨 TU 引用：`ce_push_snapshot`（snapshot / add_node / delete_node / add_edge）+ `g_ce_undo` / `g_ce_redo`（open / force_close 清栈）。
  - 撤销栈（`g_ce_undo`/`g_ce_redo` + `CeUndoEntry`）由 ce_state.cpp 定义，ce_session.cpp 经内部头 extern 访问。
- **不跨 TU（保持匿名）**：`kCeSnapDistance` / `kCeGridSize` / `g_ce_prev_positions`（仅 composite_editor.cpp 内 `ce_apply_snap` 用）· `ce_name_buffers`/`ce_name_buffer` / `ce_is_test_source`/`ce_snapshot_entry`（仅本域用）· `g_new_*` / `g_pack_*` 对话框状态（仅 ce_draw.cpp 用）。

## 3. 落地方式与等价性保证

- **纯物理搬移**：各新文件内容由原 composite_editor.cpp 按行区间提取（PowerShell 数组切片，逐字节保留），未改动任何函数体 / 注释。
- **最小结构调整**：① 跨 TU 成员由匿名提升为 `auiwrite::ui` 直接可见 —— 画布域全局（`g_ce_context` / `g_ce_rename_*` / `g_ce_add_filter` / `g_ce_show_minimap` / `g_ce_align_snap` / `g_ce_grid_snap` / `g_ce_show_grid` / `g_ce_request_fit` / `g_ce_minimap`）与函数（`ensure_ce_context` / `ce_open_rename` / `ce_apply_rename` / `ce_apply_snap`）、撤销栈（`CeUndoEntry` 结构上移 `ce_internal.h` + `g_ce_undo`/`g_ce_redo`/`ce_push_snapshot`）→ 全部经 `ce_internal.h` extern 声明共享；② **不进公共头 composite_editor.h**（对外 API 与链接符号不变，`ce_*` 短前缀仍模块私有）；③ 各新文件按域补齐 include。
- **CMakeLists**：`aiwrite` 目标新增 `ui/src/ce_state.cpp` / `ce_session.cpp` / `ce_run.cpp` / `ce_pack.cpp` / `ce_draw.cpp`。

## 4. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login 4 目标） |
| `api_probe --exec-selftest` | ✅ **682 通过 / 0 失败**（与基准一致） |
| `api_probe --graph-selftest` | ✅ **151 通过 / 0 失败**（与基准一致） |
| `api_probe --selftest` | ✅ **十三组全 PASS**（SHA3 / HTTP / 请求构造 / 配置往返 / 会话与可见性 / 执行器 / 工作流文件 / 无 Key / 取消与超时 / L2×3 / 网络与凭据） |
| 单文件体积 | composite_editor.cpp ≈71 KB → 172 行；最大新文件 ce_draw.cpp 924 行（draw_composite_editor 660 行主体，审计已预判「可细化、低优先」） |

## 5. 回滚点

- 新文件均可删除 + `git checkout -- source/ui/src/composite_editor.cpp source/CMakeLists.txt` 即回滚到拆分前单文件形态；函数名 / 语义零变化，无数据风险。
- ⚠️ 本次为纯拆分，**未改任何功能行为**；后续批（CLN-14/15 + 驼峰化）同法推进。

## 6. 备注

- 首轮构建失败（`ce_state.cpp` 的 `ce_redo` 函数体缺闭合 `}` —— 行提取时 `Slice(217,231)` 未含 L232 的收尾括号）→ 补 `}` 后 0/0；此为搬移类拆分的典型修补，非语义问题。
- `ce_draw.cpp`（924 行）仍偏大：`draw_composite_editor`（660 行）为单一绘制函数，审计已标「收益递减·低优先」；如后续打磨期需要，可再按「画布循环 / 规格面板 / 对话框」细化。
- 下一步：`CLN-14`（main.cpp，**先重写方向描述为「按 CLI 功能区拆」**）→ `CLN-15`（api_probe.cpp，先跑基线存档）→ 驼峰化（`CLN-06` 扩项）。
