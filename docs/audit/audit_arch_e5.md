# audit_arch_e5：ARCH-E5 落地审计报告（CanvasInteractor 落地 · 双画布单套交互）

> 批次：`ARCH-E5` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §2.1「双画布双轨」/ §3 阶段 2 E5
> 性质：**编辑侧事件化第五批** —— 双画布（主画布 `node_canvas.cpp` + ce 上级节点编辑器 `ce_draw.cpp`）交互层从「两套平行实现」收敛为**一套** `CanvasInteractor`（新增 `source/ui/include/interactor.h` + `source/ui/src/interactor.cpp`），与只读绘制层 `canvas_view` 对应（绘制一处、交互也一处）。
> 判据：双画布单套实现 —— `rg` 验证关键交互谓词（`GetSelectedObjectCount / GetSelectedNodes / GetSelectedLinks / BeginCreate / EndCreate / BeginDelete / EndDelete / QueryNewLink / AcceptNewItem / RejectNewItem / QueryDeletedLink / QueryDeletedNode / AcceptDeletedItem / ClearSelection`）**只出现在 interactor.cpp**；构建 0 error / 0 warning；`ce_repro` 0；`check_all` 全 PASS。

## 1. 差异盘点（ARCHITECTURE.md §5 风险对策：ce_draw 与 node_canvas 交互差异比预想大）

实施前逐段盘点两画布交互实现，差异比 E5 立项预期更大 —— 统一用「**回调 + 参数**」保留差异点：

| 维度 | 主画布（node_canvas.cpp） | ce 画布（ce_draw.cpp） | 收敛方式 |
|---|---|---|---|
| 模型图源 | `editor().model().graph` | `compositeEditorState().working.internal_graph` | `resolve_pin` / `can_connect` 回调注入图源 |
| 连线替换语义 | `canConnect(..., &would_replace)` 支持替换原连线（label「替换连线/新建连线」） | `canConnect(...)` 无替换（would_replace 恒 false） | `can_connect` 回调带 `would_replace` 输出，ce 置 false |
| 连接受拒绝提示 | 拒绝 → `log::warn + setStatus` | 拒绝 → `state.error` | `on_link_rejected` 回调（IsMouseReleased 判定由交互层统一） |
| 拖拽连线鼠标提示 | 有（M_node_expansion L3-09：端口类型 + 兼容性） | 无 | `on_create_tip` 回调（ce 空） |
| 拖出删除快照 | 每条 `snapshot("删除连线"/"删除节点")` | 每条 `compositeEditorSnapshot("删除内部连线"/"删除内部节点")`；节点原经 `compositeEditorDeleteNode`（内部压「删除内部节点 n5」） | `on_delete_batch(label)` 回调 + `delete_link` / `delete_node` 回调（label 由调用方传入；ce 节点删除 label 从「删除内部节点 n5」→「删除内部节点」，撤销次数一致） |
| 选中删除（Delete 键） | 走 `state.deleteSelected()`（基于 SelectionState，不读 ed） | `compositeEditorDeleteSelection()` 读 ed 选中 | `interactorDeleteSelection`（读 ed 选中 + `batch_snapshot` 一次压栈 + 逐项删除 + 清选中） |
| 选中同步 | 全集合（nodes + links + single 保留逻辑）→ `SelectionState::set_selection` | 仅单选局部变量 `ce_selected_node`（参数编辑区用） | `on_selection` 回调（主画布含 single 保留；ce 取 `node_ids.size()==1`） |
| 位置写回 | 有限性检查 + 0.5px 容差 + **clamp**（kPositionLimit）+ 拖拽开始压一次快照 + NodeMoved 事件 | 直接 `p.x != n.x` 精确比较 + 写回 + `dirty`，**无 clamp / 无快照** | `interactorReadNodePosition(node, model_x, model_y, clamp_limit, epsilon)` 原语：clamp_limit 主画布传 `kCanvasPositionLimit` / ce 传 0（保持现状）；epsilon 主画布 0.5f / ce 0.0f |
| 模型→编辑器推位置 | 每帧（新建/粘贴落点）+ 未知手柄分支 | 每帧 | `interactorPushNodePosition` |
| 快捷键窗口判定 | `IsWindowFocused(ChildWindows) || IsWindowHovered()` | `IsWindowFocused/Hovered(RootAndChildWindows)`（DEBT-B1） | `interactorWindowActiveMain()` / `interactorWindowActiveComposite()` |
| 清 ed 内部选中 | commands.cpp（Delete 后 `ed::ClearSelection`） | 右键删除节点后 / 选中删除后 | `interactorClearSelection()`（commands.cpp 与 ce_draw.cpp 调用） |

## 2. 改动清单

### 2.1 新增 `CanvasInteractor`（source/ui/include/interactor.h + source/ui/src/interactor.cpp）

- **`CanvasInteract` 回调组**（仿 canvas_view `CanvasContext` 的 std::function 模式）：连线创建（`resolve_pin` / `can_connect` / `on_link_accepted` / `on_link_rejected` / `on_create_tip`）· 删除（`on_delete_batch` / `delete_link` / `delete_node`）· 选中同步（`on_selection`）。
- **阶段函数**（imgui-node-editor 交互 API 唯一封装点）：
  - `interactorCreateLink`：`BeginCreate..EndCreate` 无条件配对契约内置（v0.9.3 `m_InActive` 断言）；统一「输出→输入」标准化；拒绝/接受/替换语义走回调；拖拽提示 `on_create_tip`。
  - `interactorDeleteOuter`：`BeginDelete..EndDelete` 契约内置；每个对象 `on_delete_batch(label)` 一次。
  - `interactorDeleteSelection`：选中删除（读 ed 选中 → `batch_snapshot` 一次压栈 → 逐项 `delete_node` / `delete_link` → 清选中）。
  - `interactorReadSelection` / `interactorClearSelection` / `interactorSyncSelection`：ed 选中 API（`GetSelectedObjectCount / GetSelectedNodes / GetSelectedLinks / ClearSelection`）唯一封装。
  - `interactorPushNodePosition` / `interactorReadNodePosition`：位置同步原语（`editor_knows` 有限性检查 + 未知手柄推模型坐标 + `clamp_limit` 夹紧 + `epsilon` 容差）。
  - `interactorWindowActiveMain` / `interactorWindowActiveComposite`：ARCH-R4 两画布窗口判定收敛。
- **`kCanvasPositionLimit`**（= 100000.0f）：主画布原 `kPositionLimit` 迁入共享层（单一来源；`positions_sane` 检查改用）。
- 关键谓词 **只出现在 interactor.cpp**（§4 验收判据 3；`rg` 验证见 §3）。

### 2.2 node_canvas.cpp（主画布）

- 新增 `mainCanvasInteract()`：**静态缓存一次构造**（每帧被连线/删除/选中/位置同步调用 4 次），回调全部无捕获、每次从 `editor()` 读当前状态；`makeMainCanvasInteract()` 承载构造。
- `handle_create_link` → `interactorCreateLink(mainCanvasInteract())`（约 100 行收敛为 3 行）。
- `handle_delete_items` → `interactorDeleteOuter(mainCanvasInteract(), "删除连线", "删除节点")`。
- `sync_selection()` → `interactorSyncSelection(mainCanvasInteract())`（single 保留逻辑随迁 `on_selection` 回调；函数签名去掉未用的 `state` 参数）。
- `handle_delete_key` 窗口判定 → `interactorWindowActiveMain()`。
- `sync_positions` → 循环 `interactorReadNodePosition(node.id, node.x, node.y, kCanvasPositionLimit, 0.5f)`；拖拽快照 / NodeMoved 事件留在主画布（差异点）。
- 渲染前推位置（L924-926）→ `interactorPushNodePosition`；`clamp_position` + 局部 `kPositionLimit` 删除。

### 2.3 ce_draw.cpp（ce 上级节点编辑器）

- 新增 `ceInteractCore()`（匿名域）：连线创建/删除回调（图源 = `working.internal_graph`，`would_replace=false`，拒绝落 `state.error`，`on_create_tip` 空）；`on_delete_batch` = `compositeEditorSnapshot(label)`；`delete_node` = `ceRemoveNode`（不压快照，快照统一走 batch 通道）。
- `compositeEditorDeleteSelection`（公共 API，commands.cpp 调用）→ `interactorReadSelection` + `interactorDeleteSelection`（`batch_snapshot` = `cePushSnapshot("删除选中（Delete 键）")`）。
- `drawCompositeEditor` 内：推位置 → `interactorPushNodePosition`；连线创建（原约 40 行）→ `interactorCreateLink(ce_interact)`；拖出删除（原约 20 行）→ `interactorDeleteOuter(ce_interact, "删除内部连线", "删除内部节点")`；右键删除节点后 `interactorClearSelection`；位置写回 → `interactorReadNodePosition(n.id, n.x, n.y, 0.0f, 0.0f)`（clamp 0 / epsilon 0 = 保持 ce 原语义）；选中同步 → `interactorSyncSelection`（`on_selection` 捕获局部 `ce_selected_node`，`node_ids.size()==1` 单选）；快捷键窗口判定 → `interactorWindowActiveComposite()`（含 `g_ce_window_focused` 更新）。

### 2.4 commands.cpp

- `ed::ClearSelection`（Delete 后清内部选中）→ `interactorClearSelection()`；删 `#include <imgui_node_editor.h>` + `namespace ed`（仅此一处用途）。

### 2.5 构建接入

- `source/CMakeLists.txt`：`target_sources(aiwrite PRIVATE ui/src/interactor.cpp)`。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `cmake --build` | ✅ 构建 **0 error / 0 warning** |
| **关键谓词唯一性**（§4 验收判据 3） | ✅ `rg`（`GetSelectedObjectCount` / `GetSelectedNodes` / `GetSelectedLinks` / `ClearSelection` / `BeginCreate` / `EndCreate` / `BeginDelete` / `EndDelete` / `QueryNewLink` / `AcceptNewItem` / `RejectNewItem` / `QueryDeletedLink` / `QueryDeletedNode` / `AcceptDeletedItem`）→ **只出现在 interactor.cpp**（node_canvas.cpp / ce_draw.cpp / commands.cpp 为 0，仅注释与 `interactorClearSelection()` 调用点） |
| 窗口判定收敛 | ✅ `IsWindowFocused/Hovered` 画布窗口判定只出现在 interactor.cpp（`interactorWindowActiveMain` / `interactorWindowActiveComposite`；console_panel 滚轮判定无关） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（ReadOnlyView 选中同源照常 · 编辑订阅照常） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1809 / 0**（基线持平 + 新增 audit_arch_e5.md 引用） |
| `check_all.ps1` | ✅ **19/19 全 PASS**（构建 / api_probe 八项 / run-selftest / ce_repro / 三夹具 / upload-selftest / 台账 / check_links / package） |

## 4. 行为等价说明（关键差异点的等价性论证）

| 原实现 | 新实现 | 等价论证 |
|---|---|---|
| 主画布拖出删除：每条 `snapshot("删除连线")` + removeEdge + EdgeRemoved + log + status | `interactorDeleteOuter`：`on_delete_batch` = snapshot(label) + 同款 delete_link / delete_node | 撤销次数 / 事件 / 提示逐一对应 |
| ce 拖出删除节点：`compositeEditorDeleteNode`（内部压「删除内部节点 n5」） | `on_delete_batch("删除内部节点")` + `delete_node` = `ceRemoveNode`（不压） | 撤销**次数一致**（一次）；label 从带 id → 不带 id（撤销语义不变，可接受） |
| ce 选中删除：`cePushSnapshot("删除选中（Delete 键）")` + ceRemoveNode + removeEdge + ClearSelection | `interactorDeleteSelection`：`batch_snapshot` = 同款一次压栈 + 同款删除 + 清选中 | 逐项对应（ceRemoveNode / removeEdge 回调不含快照） |
| 主画布连线创建：BeginCreate → QueryNewLink → resolve → canConnect(would_replace) → Accept/Reject + tip | `interactorCreateLink`：契约 + 标准化内置；回调注入 | 拒绝提示统一在 `IsMouseReleased(0)` 时（两画布原一致）；tip 走 `on_create_tip` |
| 主画布位置写回：有限性检查 + 0.5px 容差 + clamp + 拖拽一次快照 + NodeMoved | `interactorReadNodePosition(..., kCanvasPositionLimit, 0.5f)` + 循环保留快照/事件 | 原语覆盖有限性/clamp/容差；快照与事件仍在主画布 |
| ce 位置写回：`p.x != n.x` 精确 + dirty（无 clamp / 无快照） | `interactorReadNodePosition(..., 0.0f, 0.0f)` | clamp_limit=0 / epsilon=0 保持 ce 原语义；新增 `editor_knows` 防御（ce 每帧先推位置，实际不受影响） |

## 5. 范围边界（本批**不**接入 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| 右键菜单内容差异（主画布复制/重跑/上级打开 vs ce 改名/搜索新建） | 菜单内容本身差异大，非「窗口判定/Delete/选中同步」类重复实现 | 保留两画布各自 popup（各自调用 `ed::Suspend/Resume`，非交互核心谓词） |
| `ed::GetDoubleClickedNode`（双击行为差异） | 两画布行为不同，无重复实现 | 保留在画布各自 TU |
| 对齐吸附（`apply_alignment_snap` 主画布 / `ceApplySnap` ce） | 已上移 `canvas_view` 共享核心（CLN-10），`GetNodePosition` 属吸附辅助而非交互核心谓词 | 保留（canvas_view 单一来源） |
| 主画布选中删除走 `state.deleteSelected()`（SelectionState 源） | E4 已收口为文档级对象；不读 ed 选中 | 保留（commands.cpp 经 `dispatchCommand` 分派，R4 已三入口收敛） |
| ce 独立选中（`ce_selected_node`）与主画布 `SelectionState` 不互通 | E4 边界延续；ce 无文档级选中对象 | ARCH-Z2（CeSession 对称拆分） |
| `RunSession` 感知编辑事件 | E4 边界延续 | ARCH-D22 后续 |

## 6. 过渡登记

- `CanvasInteractor` 是**交互层共享组件**（非 §1.8 对象定义总表的文档级对象）：与 `canvas_view`（绘制层）并列，两画布经「回调 + 参数」注入模型差异。
- 新增文件 `interactor.h/.cpp` 属 `ARCH-E5` 批产出（模块冻结 §2 例外登记）。
- 后续任何画布交互应复用 interactor 阶段函数，关键谓词（ed 选中/创建/删除 API）不得外溢到其它 TU。

## 7. 回滚点

`git checkout -- source/ui/include/interactor.h source/ui/src/interactor.cpp source/CMakeLists.txt source/ui/src/node_canvas.cpp source/ui/src/ce_draw.cpp source/ui/src/commands.cpp docs/CHANGELOG.md docs/DocMAP.md docs/变量术语对照表.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 E4 形态（保留 `docs/audit/audit_arch_e5.md` 仅作登记；删除即可）。


