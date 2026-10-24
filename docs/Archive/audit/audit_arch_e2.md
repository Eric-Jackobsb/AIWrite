# audit_arch_e2：ARCH-E2 落地审计报告（Undo / autosave 订阅化 · 订阅 EditEvent）

> 批次：`ARCH-E2` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 2
> 性质：**编辑侧事件化第二批** —— Undo / autosave 改为订阅：Undo 订阅「修改前」通道压快照；autosave 订阅内容类 EditEvent 替代每帧轮询；批量/重置操作补逐节点 diff 事件。
> 判据：同步次数降低（`autosave_signature_` / `autosave_last_probe_` 成员删除 = 不再每帧算 content signature）；行为等价（`--exec-selftest` 713/0 · `--graph-selftest` 151/0 只升不降 · `--run-selftest` PASS 新增「编辑订阅」断言）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §1.3 通信层 / §2.2 问题乙 / §6 阶段 2 E2 / §1.8.1 EventBus）

| 问题 | 现状（本批前） | 目标 | 本批落点 |
|---|---|---|---|
| 一次操作触发多次同步 | 改参数 → Undo（编辑操作内联 snapshot）+ autosave（每帧 1 Hz content signature 轮询）各自直接感知 | **EditEvent**（一次操作 → 一个事件 → 消费者各自订阅处理） | Undo 订阅「修改前」通道（`publish_before_edit`）压快照；autosave 订阅内容类 EditEvent 标记变更 |
| 批量/重置操作无细粒度事件 | undo / clear / open 整图替换后无事件（E1 范围边界留 E2 定义） | 整图替换后发逐节点 diff 事件 | `publishGraphDiff`（undoOnce / redoOnce / clearWorkflow / openWorkflowFrom / restoreAutosave） |

## 2. 改动清单

### 2.1 `EventBus` 新增「修改前」通道（event_bus.h / event_bus.cpp）

- 快照式撤销需要「修改前」状态，而 EditEvent 在修改**后**发布 → 总线另提供「修改前」通道：
  `subscribe_before_edit(callback)` / `unsubscribe_before_edit(handle)` / `publish_before_edit(label)` / `before_subscriber_count()`。
- 与 `publish` 同构：单线程 FIFO（拷贝列表防回调内订阅/注销影响本次分发），不跨线程、不存历史、不做合并（§1.8.1 限制对齐）。

### 2.2 Undo 订阅化（editor_state.h / editor_state.cpp）

- `EditorState` 构造时 `event_bus_.subscribe_before_edit(...)` 订阅「修改前」通道；回调 = `abortRunIfAny("修改工作流（" + label + "）") + undo.push(graph, label)`（M2-04 守卫随迁）。
- `snapshot(label)` 改为只 `event_bus_.publish_before_edit(label)`（对外签名不变，**全部调用点零改动**：node_canvas / app.cpp / ce_session 等照旧）。
- 效果：编辑操作不再直接操作 undo 栈；压快照的时机统一由事件驱动。

### 2.3 autosave 订阅化（editor_state.h / editor_state.cpp）

- 构造时订阅 **7 种内容类** EditEvent：NodeAdded / NodeRemoved / NodeMoved / EdgeAdded / EdgeRemoved / ParamChanged / TitleChanged（`kAutosaveEditKinds`；SelectionChanged 非内容变化不订阅）。
- 回调 `onEditContent()`：`autosave_change_seen_ = true` + 刷新 `autosave_last_change_`（不检查 paused —— 暂停期变更解闸后补写，与 1 Hz 轮询语义等价）。
- `tickAutosave()` 删除每帧 1 Hz `graphContentSignature` 轮询 → 事件驱动：无事件 ⇒ 不写；静默 ≥ `autosave_idle_sec`（默认 3）才写；写失败退避 10 秒保留。
- 删除 `autosave_signature_` / `autosave_last_probe_` 成员；`markAutosaveBaseline()` / `flushAutosaveOnExit()` 改以 `autosave_change_seen_` 判据；`pauseAutosave(false)` 改重置 `autosave_last_change_`（解闸后重新计时静默）。
- 新增自检钩子 `autosaveDirty()`（= `autosave_change_seen_`）。

### 2.4 批量/重置操作逐节点 diff 事件（E1 范围边界承接）

- 新增 `publishGraphDiff(before, after)`：删除节点 → NodeRemoved（连带其连线隐含，端点已删连线不重复发 EdgeRemoved）· 新增节点 → NodeAdded · 删除连线 → EdgeRemoved · 新增连线 → EdgeAdded。
- 挂接点（整图替换/清空后、`markAutosaveBaseline()` **之前**发，随后基线重置防止立刻重写）：`undoOnce` / `redoOnce` / `clearWorkflow` / `openWorkflowFrom` / `restoreAutosave`。
- `createSampleWorkflow` 本已有 NodeAdded/EdgeAdded 发布点（5 节点 + 4 连线）不再重复。

### 2.5 自检（只升不降）

- `cli_selftest.cpp` 新增「编辑订阅」断言块（汇总进 `--run-selftest` PASS 判定）：
  - **before-edit 机制**（独立临时总线）：FIFO 顺序 + label 传递 · `unsubscribe_before_edit` 后不再收到 · `publish(EditEvent)` 不触发 before 订阅者（通道隔离）· `before_subscriber_count`。
  - **Undo 订阅化**（真实 editor()）：`snapshot("...")` → `undo.undoDepth() +1`（订阅回调压栈）· 发 NodeAdded 不额外压栈（通道隔离）。
  - **批量重置 diff**：`snapshot` + `createNode` → `undoOnce` → 收到 NodeRemoved（撤销恢复整图）。
  - **autosave 订阅化**（真实 editor()，只查 dirty 标志不写盘）：`markAutosaveBaseline` → dirty=false · 发 SelectionChanged → 仍 false（不订阅）· 发 NodeAdded → true。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `cmake --build build --config Debug` | ✅ 构建 **0 error / 0 warning**（顺带修掉 E1 块 `h2/h3` 与 E2 块 `bh2` 三个 C4189 未用句柄警告） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（新增「编辑订阅」：beforeFIFO / before注销 / before隔离 / snapshot→undo / NodeAdded不压栈 / undoOnce撤销 / undoOnce→NodeRemoved / 基线 / Selection不置位 / 内容置位 = 全「是」） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1792 / 0**（基线持平 + 新增 audit_arch_e2.md 引用） |

## 4. 范围边界（本批**不**接入 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| `RunSession` 订阅编辑事件（运行中修改工作流 → 自己感知） | 与 undo/autosave 无依赖；属 ARCH-D22 既定路线 | E2 后（随 E3 拆 WorkflowModel 或独立批次） |
| ce（上级节点编辑器）编辑事件 | ce 独立模型（CeModel），事件语义与主画布不同步 | E3（WorkflowModel 拆分）/ E5（CanvasInteractor 双画布收敛） |
| undo/redo 的重做分支相关事件 | `redo_` 分支随新编辑清空（`UndoStack::push` 语义），无需额外事件 | — |
| `FileEvent`（打开/保存/最近文件通知） | §1.3 三事件类型之一；本批聚焦编辑侧 | ARCH-A（应用级收口） |

## 5. 过渡登记

- `EventBus` 仍由 `EditorState` 持有（`editor().event_bus()`）；before 通道的 Undo 订阅与内容事件订阅都在 `EditorState` 构造时注册 —— **E3 拆 WorkflowModel 时随 Graph + Undo + EventBus 一起迁到文档级对象**（§1.8.11 依赖图：WorkflowModel 依赖 EventBus）。
- `snapshot()` 语义变为「发布修改前通知」；若未来有其他模块订阅 before 通道，Undo 必须保持**最先订阅**（FIFO 保证压快照先于其他修改前处理）。
- autosave 事件驱动后，**所有内容变更都必须有事件**：单点操作（8 种发布点，E1）+ 批量/重置（`publishGraphDiff`，本批）两条路径齐备；若未来新增绕过事件总线的直接 Graph 修改点，需同步补发事件。
- `NodeMoved`（拖拽）订阅后：拖拽期间每帧刷新 `last_change_` → 停止拖拽静默 3 秒才写（与 1 Hz 轮询时代行为一致）。
- `autosaveDirty()` 为自检钩子（读 `autosave_change_seen_`），E3 拆 AutosaveService 时随迁。

## 6. 回滚点

`git checkout -- source/ui/include/event_bus.h source/ui/src/event_bus.cpp source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/app/src/cli_selftest.cpp docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 E1 形态（保留 `docs/audit/audit_arch_e2.md` 仅作登记；删除即可）。
