# audit_arch_e3：ARCH-E3 落地审计报告（WorkflowModel 拆分 · 文档级独立）

> 批次：`ARCH-E3` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 2 E3
> 性质：**编辑侧事件化第三批** —— Graph + Undo + EventBus 从 `editor()` 拆出为文档级对象 `WorkflowModel`；Undo 订阅（before 通道）与编辑操作随迁，autosave 内容事件订阅挂在文档级总线。
> 判据：文档级独立（`editor().model()` 访问；`state.graph` → `state.model().graph` 全库替换）；行为等价（`--exec-selftest` 713/0 · `--graph-selftest` 151/0 只升不降 · `--run-selftest` PASS 含「编辑订阅」断言照常）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §1.8.3 WorkflowModel / §1.8.11 依赖图 / §1.2 核心状态层）

| 维度 | 本批前（EditorState 直接持有） | 目标（§1.8.3） | 本批落点 |
|---|---|---|---|
| 数据归属 | `EditorState.graph` / `EditorState.undo` 公有成员 | `WorkflowModel` 持有 Graph + 独立 `UndoStack`（ARCH-D26） | `WorkflowModel::graph` / `WorkflowModel::undo` |
| 事件总线 | `EditorState.event_bus_`（`editor().event_bus()`） | `WorkflowModel` 依赖 EventBus（§1.8.11 第 5 位） | `WorkflowModel::event_bus_`（`editor().model().event_bus()`） |
| 编辑操作 | `EditorState` 内联（snapshot / undo / redo / clear / createNode / deleteNode） | 修改后发编辑事件；`WorkflowModel::load(graph)` 替换 + 清撤销栈（ARCH-D29） | `WorkflowModel::snapshot / undoOnce / redoOnce / clear / load / createNode / deleteNode` |
| 撤销订阅 | `EditorState` 构造时注册 before 通道（回调 = abortRunIfAny + undo.push） | Undo 订阅随迁文档级对象 | `WorkflowModel` 构造时注册（回调 = 修改前钩子 → undo.push） |

## 2. 改动清单

### 2.1 新增 `WorkflowModel`（source/ui/include/workflow_model.h + source/ui/src/workflow_model.cpp）

- **文档级对象**（§1.8.3）：持有 `engine::Graph graph`（唯一真相）+ `engine::UndoStack undo`（ARCH-D26）+ 私有 `EventBus event_bus_`。
- **Undo 订阅「修改前」通道**：`WorkflowModel` 构造时 `subscribe_before_edit`，回调 = **修改前钩子（可注入）→ `undo.push(graph, label)`**。
  - 修改前钩子 `setBeforeEditHook(callback)` 由 `EditorState` 注入 `abortRunIfAny`（M2-04 守卫：运行中改图先终止运行）。
  - **顺序保持与 E2 一致**：钩子（abort）先执行、`undo.push` 后执行（同一回调内先 hook 后 push），保证压入的是「已终止运行、无运行状态」的快照。
  - RunSession 自己感知编辑事件（ARCH-D22）后钩子移除。
- **编辑操作**（修改后发事件）：`snapshot(label)` = 只发 before 通知；`undoOnce / redoOnce` = 整图替换 + 逐节点 diff；`clear()` = 清空 + 清撤销栈 + diff；`load(graph)` = 替换 + 清撤销栈 + diff（ARCH-D29）；`createNode` = addNode + NodeAdded；`deleteNode` = removeNode + NodeRemoved。
- **`publishGraphDiff` 随迁**：整图重置（undo / redo / clear / load）后的逐节点 diff 事件（E2 定义，NodeRemoved 隐含连带连线）。
- **不碰 UI 编排**：不 setStatus / 不碰选中 / 不写盘（这些仍归 `EditorState`）。

### 2.2 `EditorState` 瘦身（editor_state.h / editor_state.cpp）

- 移除公有成员 `graph` / `undo`、私有成员 `event_bus_` / `undo_sub_handle_`、`publishGraphDiff`。
- 新增持有 `WorkflowModel model_` + `model()` const/非 const 访问器（`editor().model()`）。
- 编辑操作方法**保留同签名**（snapshot / undoOnce / redoOnce / clearWorkflow / createSampleWorkflow / createNode / deleteSelected / deleteNode / copySelection / pasteClipboard），内部委托 `model_` + 做 UI 编排（状态栏 / 选中清空 / 剪贴板 / `markAutosaveBaseline`）—— **对外调用点签名零改动**。
- 构造：`model_.setBeforeEditHook(abortRunIfAny)` + autosave 内容事件订阅改挂 `model_.event_bus()`（随迁到文档级总线）。
- `restoreAutosave` / `openWorkflowFrom` 的整图替换段改 `model_.load(std::move(loaded))`（替换 + 清栈 + diff 一步完成，语义 = ARCH-D29）。
- `createSampleWorkflow` 保留「开头清图不发 diff」语义：直接 `model_.graph.clear()` + `model_.undo.clear()`（不调 `model_.clear()`，避免多发一组 NodeRemoved → autosave 误置位）。

### 2.3 调用点改造（`state.graph` → `state.model().graph` 全库替换）

- 机械替换：`state.graph` / `editor().graph` → `state.model().graph`；`state.undo` → `state.model().undo`；`state.event_bus()` → `state.model().event_bus()`。
- 涉及文件：`app.cpp`（ReadOnlyView 构造 / 信息面板 undoDepth / ParamChanged / TitleChanged 发布 / diagnostic）、`node_canvas.cpp`（连线增删 / NodeMoved / SelectionChanged）、`toolbar.cpp`、`error_bar.cpp`、`output_panel.cpp`、`property_panel.cpp`、`ce_pack.cpp`、`ce_session.cpp`（`main_state.graph`）、`cli_selftest.cpp`（48 处 graph + 7 undo + 9 event_bus）、`commands.cpp`（无直接成员引用）。
- ce 独立模型（`state.working.internal_graph` / `ce_ctx.graph`）**不动**（E3/E5 范围边界）。

### 2.4 构建接入

- `source/CMakeLists.txt`：`target_sources(aiwrite PRIVATE ui/src/workflow_model.cpp)`。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `cmake --build --preset debug` | ✅ 构建 **0 error / 0 warning**（新增 workflow_model.cpp 自动识别 + 全量增量编译） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（含「编辑订阅」：beforeFIFO / snapshot→undo / undoOnce撤销 / undoOnce→NodeRemoved / autosaveDirty 全「是」—— WorkflowModel 拆分后行为等价） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1800 / 0**（基线持平 + 新增 audit_arch_e3.md 引用） |

## 4. 范围边界（本批**不**接入 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| `RunSession` 订阅编辑事件（运行中修改工作流 → 自己感知） | 当前仍由修改前钩子（abortRunIfAny）兜底；属 ARCH-D22 既定路线 | E3 后（独立批次） |
| ce（上级节点编辑器）编辑事件 | ce 独立模型（CeModel），事件语义与主画布不同步 | E5（CanvasInteractor 双画布收敛） |
| `SelectionState` 拆分（选中独立） | E3 聚焦 WorkflowModel；选中集合仍留 `EditorState`（`selected_nodes` 等） | **ARCH-E4** |
| 剪贴板独立对象 | `clipboard_nodes_` / `clipboard_edges_` 仍留 `EditorState` | ARCH-A1（Clipboard 应用级收口） |

## 5. 过渡登记

- `EditorState` 持有 `WorkflowModel model_`（`editor().model()`）；`editor()` 退场（ARCH-A3）后 UI 模块直接持有 `WorkflowModel`。
- Undo 订阅已迁 `WorkflowModel` 构造；**修改前钩子（abortRunIfAny）为过渡注入**，RunSession 自己感知（ARCH-D22）后移除，届时 before 通道只剩 `undo.push`。
- autosave 内容事件订阅挂在文档级总线（`model().event_bus()`），回调（dirty 标记）仍属 `EditorState`；E3 后拆 `AutosaveService`（ARCH-A2）时订阅注册随迁。
- 若未来新增绕过事件总线的直接 Graph 修改点，需同步补发事件（E2 过渡登记继续生效）。
- 调用点统一 `state.model().graph`：ReadOnlyView 零拷贝断言（`&view.nodes_for_render() == &state.model().graph.nodes`）继续成立（同一底层对象）。

## 6. 回滚点

`git checkout -- source/ui/include/workflow_model.h source/ui/src/workflow_model.cpp source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/ui/include/event_bus.h source/CMakeLists.txt source/ui/src/app.cpp source/ui/src/node_canvas.cpp source/ui/src/toolbar.cpp source/ui/src/error_bar.cpp source/ui/src/output_panel.cpp source/ui/src/property_panel.cpp source/ui/src/ce_pack.cpp source/ui/src/ce_session.cpp source/app/src/cli_selftest.cpp docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 E2 形态（保留 `docs/audit/audit_arch_e3.md` 仅作登记；删除即可）。

