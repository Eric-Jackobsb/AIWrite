# audit_arch_e4：ARCH-E4 落地审计报告（SelectionState 拆分 · 选中独立）

> 批次：`ARCH-E4` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 2 E4
> 性质：**编辑侧事件化第四批** —— 选中集合（节点 / 连线 / 单选节点）从 `EditorState` 拆出为独立对象 `SelectionState`（§1.8.4 / ARCH-D28）；`ReadOnlyView` 的 `SelectionReadView` 过渡结构替换为该对象 const 引用（渲染调用点不变）。
> 判据：选中独立（`editor().selection()` 访问；`selected_nodes` 等公有成员全库移除）；行为等价（`--run-selftest` PASS 含「ReadOnlyView 选中同源」断言照常 · `--exec-selftest` 713/0 · `--graph-selftest` 151/0 只升不降）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §1.8.4 SelectionState / §1.8.11 依赖图 / 决策 ARCH-D28）

| 维度 | 本批前（EditorState 公有成员） | 目标（§1.8.4） | 本批落点 |
|---|---|---|---|
| 数据归属 | `EditorState.selected_nodes` / `selected_links` / `selected_node` | 独立对象持有选中集合 | `SelectionState::nodes() / links() / single()` |
| 标准写法 | 三个散落公有成员（无封装） | 集合 + 增删改查接口 | `set_selection / select_node / set_nodes / set_single / remove_node / clear_nodes / clear_links / prune_links / clear` |
| 事件 | `sync_selection` 变化检测后手动 publish | 变化时发 `EditEvent::SelectionChanged` | 对象内部变化检测 → 回调注入（EditorState 构造时注入 → publish） |
| 只读视图 | `SelectionReadView`（过渡聚合引用） | `const SelectionState&` | `ReadOnlyView::selection` 改 `const SelectionState&`（渲染调用点不变） |

## 2. 改动清单

### 2.1 新增 `SelectionState`（source/ui/include/selection_state.h + source/ui/src/selection_state.cpp）

- **文档级对象**（§1.8.4）：持有选中节点 id 集合 `nodes_` + 连线 id 集合 `links_` + 单选节点 id `single_`（参数面板用），提供增删改查接口。
- **变化检测内置**：每个修改方法先快照三组集合 → 修改 → **任一变化才触发回调**（避免每帧高频通知，与 E1 `sync_selection` 语义一致）。
- **无依赖注入**（§1.8.11 第 6 位）：不 include EventBus；`set_changed_callback` 由 `EditorState` 构造时注入 → 发 `EditEvent::SelectionChanged` 到 `model().event_bus()`。SelectionChanged 为通知形态（无内容消费者，autosave 明确不订阅）⇒ 事件发布点从 1 处（sync_selection）扩展到全部选中修改点，行为影响 ≈ 0。
- **保留现有粒度语义**（行为等价关键）：
  - `clear_nodes()` 只清 nodes + single、**保留 links**（与 undoOnce / redoOnce / clearWorkflow / createSampleWorkflow 现状一致）；
  - `clear()` 全清（restoreAutosave / openWorkflowFrom 现状一致）；
  - `remove_node(id)` 移除节点 + 若 single 命中清 single（deleteNode 现状一致）；
  - `prune_links(keep)` 保留满足 keep 的连线（ce 热重载失效引用清理现状语义一致）。
- **不碰 UI 编排**：不 setStatus / 不碰视图跟随 / 不碰剪贴板（仍归 `EditorState`）。

### 2.2 `EditorState` 瘦身（editor_state.h / editor_state.cpp）

- 移除公有成员 `selected_nodes` / `selected_links` / `selected_node`（全库 `rg` 验证 0 残留，仅注释与 ce 独立变量 `ce_selected_node` 除外——ce 属 E5/E3 范围边界）。
- 新增持有 `SelectionState selection_` + `selection()` const/非 const 访问器（`editor().selection()`）。
- 构造：`selection_.set_changed_callback(发 SelectionChanged 到 model().event_bus())`。
- 编辑方法选中操作改走 `selection_`：`undoOnce / redoOnce / clearWorkflow / createSampleWorkflow` → `clear_nodes()`；`deleteNode` → `remove_node()`；`deleteSelected` → `empty() / has_nodes() / has_links() / nodes() / links() / clear_nodes() / clear_links()`；`copySelection` → `nodes()`；`pasteClipboard` → `set_nodes(created) + set_single(front)`；`restoreAutosave / openWorkflowFrom` → `clear()`。
- `has_selection()` 保留薄封装（`selection_.has_nodes() || selection_.has_links()`），全部调用点零改动。

### 2.3 `ReadOnlyView` 替换（readonly_view.h）

- 删除过渡结构 `SelectionReadView`；`selection` 字段改 `const SelectionState&`；构造签名 `(const engine::Graph&, const SelectionState&, const engine::RunSnapshot&, const Config&)`。
- 渲染调用点不变（访问器名与过渡字段一致）：`view.selection.nodes().size()` 等（app.cpp 信息面板 L366 / 状态栏 L1289）。
- 主循环构建（app.cpp）：删 `selection_view` 局部聚合 → `ReadOnlyView view{state.model().graph, state.selection(), ...}`（直接引用独立对象）。

### 2.4 调用点改造（全库替换）

- **node_canvas.cpp**：`sync_selection` 重写 —— 从画布读回 node_ids / link_ids / single 后一次 `state.selection().set_selection(...)`（变化检测 + SelectionChanged 发布由对象内部完成）；6 处「创建 / 右键 / 双击」选中赋值 → `state.selection().select_node(id)`。
- **toolbar.cpp**：计数 / 重跑目标 → `state.selection().nodes() / links()`；`has_selection()` 不变。
- **error_bar.cpp**：定位失败节点 → `state.selection().select_node(node_id)`。
- **ce_pack.cpp**：打包选区读取 → `state.selection().nodes()`。
- **ce_session.cpp**：热重载失效连线清理 → `main_state.selection().prune_links(findEdge != nullptr)`。
- **ce_draw.cpp**：打包对话框计数 / 默认名判断 → `editor().selection().nodes().size()`。
- **app.cpp**：菜单「打包选区」可用性 / 参数面板 `single()` 读取 / 信息面板 / 状态栏。
- **cli_selftest.cpp**：复制/粘贴前置选中（`set_nodes`）、选中断言（`nodes().size()`）、ReadOnlyView 断言升级为「对象同源 + 三访问器同源」（`&rv.selection == &state.selection()`）。
- **commands.cpp**：仅经 `has_selection() / copySelection()`，无直接成员引用（零改动）。

### 2.5 构建接入

- `source/CMakeLists.txt`：`target_sources(aiwrite PRIVATE ui/src/selection_state.cpp)`。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `cmake --build` | ✅ 构建 **0 error / 0 warning**（新增 selection_state.cpp 自动识别 + 全量增量编译） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（ReadOnlyView 断言升级后「选中同源 = 是」；F1 复制/粘贴「选择与跟随 = OK」；「编辑订阅」断言照常） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1800 / 0**（基线持平 + 新增 audit_arch_e4.md 引用） |

## 4. 范围边界（本批**不**接入 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| `RunSession` 订阅编辑事件（运行中修改工作流 → 自己感知） | 当前仍由修改前钩子（abortRunIfAny）兜底；属 ARCH-D22 既定路线 | E4 后（独立批次） |
| ce（上级节点编辑器）编辑事件 / ce 选中（`ce_selected_node`） | ce 独立模型（CeModel / CeSelection），事件语义与主画布不同步 | E5（CanvasInteractor 双画布收敛）/ ARCH-Z2（CeSession 对称拆分） |
| 剪贴板独立对象 | `clipboard_nodes_` / `clipboard_edges_` 仍留 `EditorState` | ARCH-A1（Clipboard 应用级收口） |
| `sync_selection` 读画布选中（ed::GetSelectedNodes） | 画布双向同步机制；待 E5 `CanvasInteractor` 参数化复用后收敛 | E5 |

## 5. 过渡登记

- `EditorState` 持有 `SelectionState selection_`（`editor().selection()`）；`editor()` 退场（ARCH-A3）后 UI 模块直接持有 `SelectionState`。
- SelectionChanged 事件发布点从 1 处（sync_selection）扩展为「全部选中修改方法」（对象内部变化检测后回调）—— **无内容消费者**（autosave 明确不订阅；E2 断言「Selection 不置位」照常），行为影响 ≈ 0；后续批次若接入消费者（诊断 / 画布同步）以事件为准。
- `ReadOnlyView::selection` = `const SelectionState&`（零拷贝同源，`&view.selection == &state.selection()`）；`graph()` 过渡访问器说明继续生效（E5 收敛）。
- ce 独立选中（`ce_selected_node`）与本对象**不互通**（ce 内部画布选中，E5/Z2 收敛前保持现状）。
- 若未来新增选中修改点，应走 `SelectionState` 修改方法（内置变化检测 + 事件），避免直接绕回公有集合。

## 6. 回滚点

`git checkout -- source/ui/include/selection_state.h source/ui/src/selection_state.cpp source/ui/include/readonly_view.h source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/CMakeLists.txt source/ui/src/app.cpp source/ui/src/node_canvas.cpp source/ui/src/toolbar.cpp source/ui/src/error_bar.cpp source/ui/src/ce_pack.cpp source/ui/src/ce_session.cpp source/ui/src/ce_draw.cpp source/app/src/cli_selftest.cpp docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 E3 形态（保留 `docs/audit/audit_arch_e4.md` 仅作登记；删除即可）。

