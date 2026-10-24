# 审计报告 · CLN-09 + CLN-10（重复实现合并：小地图 / 对齐吸附共享化）+ CLN-11 复核

- **批次**：CLN-09（小地图共享化）+ CLN-10（对齐吸附 + 辅助线共享化）+ CLN-11（残留复核）
- **日期**：2026-10-20
- **性质**：⚠ 含删除 → 走 §3.0 删除前置审计闸门（本报告 = ①清点 + ②取证 + ③影响面 + ④行动计划，交人工审核；**⑤审核闸门通过前不动代码**）
- **关联**：CLN-05 同族（重复实现合并）；与 [../draft/CN_editor_refactor.md](../draft/CN_editor_refactor.md) 阶段 3 同向；共享层 `canvas_view.{h,cpp}` 已就绪（CEA-01/02/05）

---

## ① 对象清点（删除对象清单）

### CLN-09 · 小地图（`node_canvas.cpp` ↔ `composite_editor.cpp` 平行实现）

| # | 文件 : 行号 | 符号 | 删除理由 |
|---|---|---|---|
| D1 | `source/ui/src/composite_editor.cpp:106-114` | `ce_minimap_rect` | 重复（与主画布 `minimap_rect` **逐行一致**） |
| D2 | `source/ui/src/composite_editor.cpp:116-119` | `ce_minimap_hit` | 重复（与主画布 `minimap_hit` **逐行一致**） |
| D3 | `source/ui/src/composite_editor.cpp:122-149` | `ce_block_minimap_input` | 重复（与主画布 `block_minimap_input` **结构一致**，仅全局变量前缀 `g_ce_minimap_*`） |
| D4 | `source/ui/src/composite_editor.cpp:156-221` | `ce_draw_minimap` | 重复（与主画布 `draw_minimap` **几何/逻辑逐行一致**，仅主题取色差异） |
| D5 | `source/ui/src/composite_editor.cpp:101-104` | `g_ce_minimap_blocking/clicked/down/mouse` | 平行状态（与主画布 `g_minimap_*` 同构） |
| D6 | `source/ui/src/composite_editor.cpp:175, 130 等` | `g_ce_show_minimap` 读取点 | 平行开关（主画布 `g_show_minimap` 同义） |

### CLN-10 · 对齐吸附 + 辅助线

| # | 文件 : 行号 | 符号 | 删除理由 |
|---|---|---|---|
| D7 | `source/ui/src/composite_editor.cpp:230-247` | `ce_find_dragged_node` | 重复（与主画布 `find_dragged_node` 逻辑一致，差异见 §③） |
| D8 | `source/ui/src/composite_editor.cpp:258-347` | `ce_apply_alignment_snap` | 重复（核心循环与主画布 `apply_alignment_snap` **逐行一致**） |
| D9 | `source/ui/src/composite_editor.cpp:351-392` | `ce_apply_snap` | 重复（入口封装 = 主画布 `apply_alignment_snap` 前置逻辑） |
| D10 | `source/ui/src/composite_editor.cpp:395-403` | `ce_draw_snap_lines` | 重复（与主画布 L1330-1336 内联绘制同功能） |
| D11 | `source/ui/src/composite_editor.cpp:228` | `g_ce_prev_positions` | 平行状态（主画布 `g_prev_positions` 同义） |
| D12 | `source/ui/src/composite_editor.cpp:249-254` | `CeSnapResult` | 重复结构（= 主画布 `SnapResult`） |

### CLN-11 · 残留复核（无则勾账）

| 符号 | 全库出现 | 结论 |
|---|---|---|
| `ce_draw_node` | **0** | ✅ 已删无残留 |
| `ce_result_view` | **0** | ✅ 已删无残留 |
| `ce_node_handle` / `ce_link_handle` / `ce_pin_handle` | **0** | ✅ 已删无残留 |
---

## ② 引用取证（引用计数 + 证据）

### CLN-09 小地图 —— 定义/调用点全表

| 符号 | 定义处 | 调用处 |
|---|---|---|
| `minimap_rect` | `node_canvas.cpp:807` | `:833`（block_minimap_input 内）、`:1015`（draw_minimap 内） |
| `minimap_hit` | `node_canvas.cpp:817` | `:837`、`:1068` |
| `block_minimap_input` | `node_canvas.cpp:823` | `:1128`（draw_node_canvas 内，ed::Begin 之前） |
| `draw_minimap` | `node_canvas.cpp:1007` | `:1338`（ed::End 之后） |
| `ce_minimap_rect` | `composite_editor.cpp:106` | `:131`、`:166` |
| `ce_minimap_hit` | `composite_editor.cpp:116` | `:134`、`:216` |
| `ce_block_minimap_input` | `composite_editor.cpp:122` | `:1230`（ed::Begin 之前） |
| `ce_draw_minimap` | `composite_editor.cpp:156` | `:1499`（ed::End 之后） |

**调用拓扑**：两处小地图均为「ed::Begin 前 block 输入 → ed::End 后 draw」，时序一致（CEA-D2 已修复）。小地图状态 `g_*_minimap_*` 仅在各自 TU 内使用，**无跨 TU 引用**。

### CLN-10 对齐吸附 —— 定义/调用点全表

| 符号 | 定义处 | 调用处 |
|---|---|---|
| `find_dragged_node` | `node_canvas.cpp:861` | `:900`（apply_alignment_snap 内） |
| `apply_alignment_snap` | `node_canvas.cpp:893` | `:1327`（draw_node_canvas 内） |
| `SnapResult` | `node_canvas.cpp:884` | 同上 |
| 辅助线绘制 | — | `node_canvas.cpp:1331-1336`（内联 AddLine） |
| `ce_find_dragged_node` | `composite_editor.cpp:230` | `:358`（ce_apply_snap 内） |
| `ce_apply_alignment_snap` | `composite_editor.cpp:258` | `:376`（ce_apply_snap 内） |
| `ce_apply_snap` | `composite_editor.cpp:351` | `:1497`（ce_draw_composite_editor 内） |
| `ce_draw_snap_lines` | `composite_editor.cpp:395` | `:1498` |
| `CeSnapResult` | `composite_editor.cpp:249` | 上述 |
| `g_ce_prev_positions` | `composite_editor.cpp:228` | `:233,:237,:355,:389` |
| `g_prev_positions` | `node_canvas.cpp:858` | `:864,:870,:897,:1001` |

**跨 TU 引用检查**：所有上述符号均为**文件内匿名命名空间 / static 内部链接**，无任何跨 TU 引用；删除后无外部契约影响。`node_handle`/`link_handle`/`pin_handle` 等已在 `canvas_view` 共享（30 处引用），不在此批范围。
---

## ③ 影响面评估（差异点 → 保留策略）

### 逐行比对结论（核心逻辑全部一致）

| 组 | 比对结果 |
|---|---|
| `minimap_rect` vs `ce_minimap_rect` | **逐行一致**（几何 190×130、margin 10、锚定右下） |
| `minimap_hit` vs `ce_minimap_hit` | **逐行一致** |
| `block_minimap_input` vs `ce_block_minimap_input` | **结构逐行一致**，仅全局状态名前缀不同（`g_minimap_*` ↔ `g_ce_minimap_*`）+ 开关名（`g_show_minimap` ↔ `g_ce_show_minimap`） |
| `draw_minimap` vs `ce_draw_minimap` | **几何/遍历/导航逻辑逐行一致**；差异仅为取色（见下） |
| `apply_alignment_snap` 核心循环 vs `ce_apply_alignment_snap` | **逐行一致**（三边对 X/Y 吸附 + 辅助线端点换算） |

### 需保留的差异行为（⚠ 合并时不得抹平）

| 差异 | 主画布 | 上级编辑器 | 保留方式 |
|---|---|---|---|
| **主题取色** | `IM_COL32(18,20,26,215)` 等字面量 + `state_color(node.state)` | `canvas_theme_composite()`（`theme.minimap_*`） | 共享函数收 `const CanvasTheme&`；主画布传 `canvas_theme_main()`（字面量 = theme 默认值，见下核对） |
| **小地图节点块色** | `state_color(node.state)`（按状态着色） | 固定 `IM_COL32(190,195,220,200)` | 共享函数增参（如 `std::function<ImU32(const engine::Node&)>`） |
| **辅助线颜色/宽度** | `IM_COL32(0x9C,0x6C,0xFF,230)` · 线宽 `1.6f` | `IM_COL32(200,80,220,255)` · 线宽 `1.0f` | 共享函数收颜色 + 线宽参数；两调用点各传现值 |
| **Alt 平移互斥** | `find_dragged_node` 检查 `g_alt_pan_active` | 无此概念 | 共享函数收 `bool suppress` 参数；主画布传 `g_alt_pan_active`，上级传 `false` |
| **拖拽按键** | `kNodeEditorDragButton`(=Left) | `ImGui::IsMouseDown(0)` | 语义相同（均左键）；合并后统一用 `kNodeEditorDragButton` |
| **开关** | `g_snap_enabled` + `g_snap_distance`(可调 slider) | `g_ce_align_snap` + `g_ce_grid_snap` + 常量 `kCeSnapDistance=8` | 开关留在各自 TU（共享函数只收参数，不碰开关） |
| **网格吸附** | 无 | `kCeGridSize=16` + `g_ce_grid_snap`（G7 独有） | **保留在上级调用点**（共享函数不含网格吸附；上级 ce_apply_snap 保留此步） |
| **上级网格开关 UI** | — | `composite_editor.cpp:1159` | 保留 |

### 主画布主题色核对（关键前置确认）

主画布 `draw_minimap` 当前用字面量，`canvas_view.cpp` `make_main_theme()` 中**已有完全相同的默认值**：
- `minimap_bg=IM_COL32(18,20,26,215)` ↔ 字面量 L1017 相同 ✅
- `minimap_border=IM_COL32(130,130,145,210)` ↔ L1018 相同 ✅
- `minimap_title=IM_COL32(200,200,210,235)` ↔ L1019 相同 ✅
- `minimap_close=IM_COL32(225,90,90,235)` ↔ L1028 相同 ✅
- `minimap_empty=IM_COL32(120,120,135,200)` ↔ L1033 相同 ✅
- `snap_line=IM_COL32(0x9C,0x6C,0xFF,230)` ↔ L1332/1335 相同 ✅

⇒ **共享后主画布行为零变化**（字面量 → theme 默认值 = 同一像素值）。

### 回滚点 / 风险

- **回滚点**：当前 `git HEAD`。合并批提交后可 `git revert` 单批回滚。
- **行为面**：仅内部 TU 代码移动 + 主题参数化，**无功能增减**（上级网格吸附、Alt 互斥、两套配色全部保留）。
- **验证**：构建 0/0 + `--selftest` 全绿 + `--graph-selftest`（画布相关）+ `check_links` 0。

---

## ④ 行动计划（交人工审核）

### 合并方向（复用既有共享层 `canvas_view.{h,cpp}`）

**CLN-09**：把 4 个主画布实现**提升**为共享（`node_canvas.cpp` 与 `composite_editor.cpp` 都删本地版）：
1. `minimap_rect` / `minimap_hit` → 直接移入 `canvas_view`（无差异，0 参数）
2. `block_minimap_input` → 共享；把「开关指针 + 状态快照（4 个 bool/ImVec2 引用）」收为参数，两 TU 各传自己的 `g_*_minimap_*`
3. `draw_minimap` → 共享；收 `(const Graph&, const CanvasTheme&, 节点块色回调, 开关指针…)`，删除本地 `ce_draw_minimap`
4. 删除 `ce_minimap_rect/hit/block_minimap_input/draw_minimap` + `g_ce_minimap_*`（用共享层状态参数替代）

**CLN-10**：对齐核心共享化：
1. 新增共享 `SnapResult apply_alignment_snap_core(const Graph&, const std::string& dragged, const ImVec2& dpos, float dw, float dh, float snap_canvas)`（= `ce_apply_alignment_snap` 签名化 + 命名统一）
2. 主画布 `apply_alignment_snap` 改为「开关判断 + find_dragged_node + 调共享核心 + 状态更新」薄封装（保留 Alt 互斥、g_snap_distance）
3. 上级 `ce_apply_snap` 保留「开关 + find_dragged_node + 调共享核心 + **网格吸附** + 状态更新」，`ce_apply_alignment_snap`/`ce_find_dragged_node`/`CeSnapResult` 删除
4. 辅助线绘制收 `(const SnapResult&, ImDrawList*, ImU32 color, float width)` 共享函数；主画布传 `canvas_theme_main().snap_line, 1.6f`、上级传 `IM_COL32(200,80,220,255), 1.0f`
5. 删除 D7~D12

**CLN-11**：复核无残留 → **勾账（无删除）**

### 验证清单

| 项 | 预期 |
|---|---|
| 构建（MSVC /W4） | 0 error / 0 warning |
| `--selftest` | 13 组 PASS |
| `--exec-selftest` | 全绿 |
| `--graph-selftest` | 全绿（画布回归重点） |
| `check_links` | 0 broken |
| 无 `ce_minimap_*`/`ce_apply_*`/`ce_find_dragged_node`/`ce_draw_snap_lines`/`CeSnapResult` 残留 | 0 |

### 文档登记

- 本审计报告 §⑥ 补落地记录
- `M_code_cleanup.md` §3.3 `CLN-09`/`CLN-10`/`CLN-11` 勾账 + changelog 更新

---

## ⑤ 审核闸门

- [ ] **⛔ 待人工审核**（未批准前不动代码）
- [x] ✅ 通过 → 执行合并（落地后补 §⑥）
- [ ] 需修改 → 修改行动计划后重新提交

*审核意见：* **2026-10-20 用户批准「通过」，按 §④ 执行。**

---

*（本文档为 §3.0 删除前置审计流程产出物；批准后按④执行，落地后补⑥。）*

---

## ⑥ 落地记录（审核通过后执行，2026-10-20）

### 6.1 执行概况

按 §④ 行动计划完成合并，构建 **0 error / 0 warning**，selftest 全量回归绿，`check_links` 0 broken。

| 验证项 | 结果 |
|---|---|
| 构建（MSVC /W4） | 0 error / 0 warning |
| `--selftest` | 13 组 PASS |
| `--exec-selftest` | 682 / 0 |
| `--graph-selftest` | 151 / 0 |
| `check_links` | 1613 / 0 |
| 残差断言（`ce_minimap` / `ce_apply_alignment_snap` / `ce_find_dragged_node` / `ce_draw_snap_lines` / `ce_draw_minimap` / `CeSnapResult` / `g_*_minimap_blocking` 等） | 0 |

### 6.2 共享层新增（`canvas_view.{h,cpp}` 单一来源）

- `struct MinimapState`（blocking / clicked / down / mouse 输入屏蔽快照）
- `minimap_rect` / `minimap_hit`（几何无差异，0 参数）
- `block_minimap_input(bool& show, MinimapState& state)`（开关指针 + 状态快照收为参数）
- `draw_minimap(const Graph&, const CanvasTheme&, bool& show, const MinimapState&, 节点块色回调)`（主题取色走 `CanvasTheme`；节点块色走回调）
- `struct SnapResult`（共享吸附结果；`CeSnapResult` 废除）
- `find_dragged_node(const Graph&, bool suppress, PrevPositions&)`（`suppress` 承载 Alt 平移互斥）
- `apply_alignment_snap_core(const Graph&, dragged, dpos, dw, dh, snap_canvas)`（对齐核心 = 原 `ce_apply_alignment_snap` 签名化）
- `draw_snap_lines(const SnapResult&, ImDrawList*, ImU32 color, float width)`（颜色 / 线宽参数化）

### 6.3 删除对象执行情况（对照 §①）

| # | 原对象 | 执行结果 |
|---|---|---|
| D1 | `ce_minimap_rect` | ✅ 删除 |
| D2 | `ce_minimap_hit` | ✅ 删除 |
| D3 | `ce_block_minimap_input` | ✅ 删除 |
| D4 | `ce_draw_minimap` | ✅ 删除 |
| D5 | `g_ce_minimap_blocking/clicked/down/mouse` | ✅ 收敛为共享 `MinimapState g_ce_minimap;`（4 全局 → 1 结构） |
| D6 | `g_ce_show_minimap` | 保留为开关（共享 `draw_minimap` 收 `bool&`，点 ✕ 关闭回写） |
| D7 | `ce_find_dragged_node` | ✅ 删除（共享 `find_dragged_node` 替代） |
| D8 | `ce_apply_alignment_snap` | ✅ 删除（共享 `apply_alignment_snap_core` 替代） |
| D9 | `ce_apply_snap` | 改造为薄封装（开关 + `find_dragged_node` + 共享核心 + **网格吸附（G7 保留）** + 状态更新），返回共享 `SnapResult` |
| D10 | `ce_draw_snap_lines` | ✅ 删除（共享 `draw_snap_lines` 替代） |
| D11 | `g_ce_prev_positions` | 保留为共享 `find_dragged_node` 的 `prev_positions` 参数宿主（每帧末尾全量更新不变） |
| D12 | `CeSnapResult` | ✅ 删除（共享 `SnapResult` 替代） |

主画布侧同步删除本地重复实现（`node_canvas.cpp`）：`minimap_rect` / `minimap_hit` / `block_minimap_input` / `draw_minimap` / `find_dragged_node` / `SnapResult` / `apply_alignment_snap` 核心逻辑（改为薄封装，保留 `g_snap_enabled` / `g_snap_distance` / Alt 互斥 / `g_prev_positions`）。

### 6.4 差异行为保留核对（对照 §③ 表）

| 差异 | 保留方式 | 验证 |
|---|---|---|
| 主画布小地图底色 / 辅助线色 | `canvas_theme_main()`（字面量 = `make_main_theme()` 默认值 pixel-identical） | `theme.snap_line=IM_COL32(0x9C,0x6C,0xFF,230)` ✅ 各 minimap 色字面量一致 ✅ |
| 上级编辑器小地图差异主题 | `canvas_theme_composite()` 传入共享函数 | ✅ |
| 小地图节点块色 | 主画布 `state_color(node.state)` / 上级固定 `IM_COL32(190,195,220,200)`（回调参数） | ✅ |
| 辅助线颜色 / 宽度 | 主画布 `snap_line, 1.6f` / 上级 `IM_COL32(200,80,220,255), 1.0f` | ✅ |
| Alt 平移互斥 | 主画布 `find_dragged_node(graph, g_alt_pan_active, …)` / 上级 `false` | ✅ |
| 网格 16px 吸附（G7） | 保留在上级 `ce_apply_snap` 薄封装 | ✅ |
| 开关（`g_snap_enabled` / `g_ce_align_snap` / `g_ce_grid_snap` / `g_*_show_minimap`） | 全部留在各自 TU | ✅ |

### 6.5 CLN-11 复核

`ce_draw_node` / `ce_result_view` / `ce_*_handle` 全库 **0 残留** → **勾账（无删除）**。

### 6.6 文档登记

- 本报告 §⑤ 已勾选通过 · 本 §⑥ 为落地记录
- `M_code_cleanup.md` §3.3 已勾账（CLN-09 / CLN-10 / CLN-11）· §7 变更记录 v1.8

---

*（本文档为 §3.0 删除前置审计流程产出物；⑥ = 批准后落地记录。）*

