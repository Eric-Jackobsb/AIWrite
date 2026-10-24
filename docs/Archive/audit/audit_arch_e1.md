# audit_arch_e1：ARCH-E1 落地审计报告（事件总线 EventBus + EditEvent · 8 种编辑事件）

> 批次：`ARCH-E1` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 2
> 性质：**编辑侧事件化第一批** —— `EventBus` + `EditEvent`（8 种）落地，事件系统就绪：订阅/发布机制 + 8 种类型各有真实发布点 + 单元断言。
> 判据：行为等价（`--exec-selftest` 713/0 只升不降 · `--run-selftest` PASS 新增编辑事件断言）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §1.3 通信层 / §1.8.1 EventBus / §2.2 问题乙 / §6 阶段 2 E1）

| 问题 | 现状（本批前） | 目标 | 本批落点 |
|---|---|---|---|
| 一次操作触发多次同步 | 改参数 → Undo + autosave + UI 刷新各自直接感知 | **EditEvent**（一次操作 → 一个事件 → 消费者各自处理） | `EventBus` 单线程 FIFO 通知总线 + `EditEvent` **8 种类型**（ARCH-D12）就绪 |
| 编辑侧无统一变化通知 | 编辑操作直接改 `Graph`，无变化通知载体 | 事件系统就绪 | 8 种事件**各有真实发布点** + `--run-selftest` 单元断言（机制 / 类型完整性 / 操作→事件映射） |

## 2. 改动清单

### 2.1 新增 `source/ui/include/event_bus.h` + `source/ui/src/event_bus.cpp`

- `EditEventKind` **8 种**（ARCH-D12）+ 哨兵 `Count`：NodeAdded / NodeRemoved / NodeMoved / EdgeAdded / EdgeRemoved / ParamChanged / TitleChanged / SelectionChanged。
- `EditEvent`：最小标识负载（`node_id` / `param_key` / `edge_id` + 两端端口）—— 通知形态（规则 4）只发通知，具体数据消费者**直接读核心状态**（规则 3）；静态工厂 `node()` / `param()` / `edge()` 免发布点手动填字段。
- `EventBus`：`subscribe(kind, callback)` → 句柄（>0）· `unsubscribe(handle)` · `publish(event)` 单线程 FIFO **同步分发**（按 kind 分组，按订阅顺序；拷贝列表防回调改容器）· `subscriberCount(kind)`（自检辅助）。
  - §1.8.1 限制对齐：不跨线程（编辑操作全在 UI 线程）、不存储事件历史、不做事件合并。
  - RunEvent 仍走 `RunSession` 现有队列（ARCH-D10，复用）；FileEvent 后续批次。

### 2.2 `EditorState` 持有 EventBus（过渡）

- `editor_state.h`：`EventBus event_bus_` 私有成员 + `event_bus()` const/非 const 访问器。
- 过渡登记：目标态 EventBus 为独立通信层对象（§1.8.11 依赖图：`WorkflowModel` 依赖 EventBus）；**E3 拆出 WorkflowModel 后迁出**。

### 2.3 发布点（8 种事件各有真实发布点 · 编辑操作 → 事件映射）

| 事件 | 发布点 | 负载 |
|---|---|---|
| `NodeAdded` | `editor_state.cpp` `createNode()` 成功后 · `pasteClipboard()` 每节点 | node_id |
| `NodeRemoved` | `editor_state.cpp` `deleteNode()` 成功后（连带其连线由 NodeRemoved 隐含） | node_id |
| `NodeMoved` | `node_canvas.cpp` `sync_positions()` 拖拽位置写回模型时 | node_id |
| `EdgeAdded` | `node_canvas.cpp` `handle_create_link()` 画布连线成功 · `editor_state.cpp` `pasteClipboard()` / `createSampleWorkflow()` | 两端端口 + edge_id |
| `EdgeRemoved` | `node_canvas.cpp` `handle_delete_items()` 画布删连线 · `editor_state.cpp` `deleteSelected()` 删选中连线（删除前取边数据） | 两端端口 + edge_id |
| `ParamChanged` | `app.cpp` `edit.changed`（参数编辑结束） | node_id（未跟踪具体 key → 通知「有参数变了」） |
| `TitleChanged` | `app.cpp` `edit.renamed` | node_id |
| `SelectionChanged` | `node_canvas.cpp` `sync_selection()` **变化检测**后（选中真正变化才发，避免每帧高频） | — |

- `createSampleWorkflow` / `pasteClipboard` 内的 `graph.addEdge` 直接调用点补发 EdgeAdded（与画布连线同构，保证一致性）。

### 2.4 自检（只升不降）

- `cli_selftest.cpp` 新增「编辑事件」断言块（汇总进 `--run-selftest` PASS 判定）：
  - **机制**（独立临时总线，不污染 editor() 总线）：kind 分组（发 NodeAdded 不触发 NodeRemoved 订阅者）· 同 kind 按订阅顺序 FIFO · 重复订阅追加 · `unsubscribe` 后不再收到 · `subscriberCount`。
  - **8 种类型完整性**：`EditEventKind::Count == 8`（ARCH-D12）。
  - **操作→事件映射**（真实 `editor()` 操作，断言后删除恢复原图）：`createNode` → NodeAdded · `deleteNode` → NodeRemoved。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `check_all.ps1`（含 build.ps1） | ✅ **19/19**：构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（含新增「编辑事件：kind分组=是 / FIFO顺序=是 / 注销=是 / NodeRemoved=是 / 计数=是 / 8种=是 / createNode→NodeAdded=是 / deleteNode→NodeRemoved=是」） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1786 / 0**（基线持平 +6：新增 event_bus 文档引用与 audit_arch_e1.md） |

## 4. 范围边界（本批**不**接入 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| ce（上级节点编辑器）编辑事件 | ce 是独立模型（CeModel），事件语义与主画布不同步；接入会扩大本批改动面 | E3（WorkflowModel 拆分）后随文档级对象统一 / E5（CanvasInteractor 双画布收敛） |
| undo / redo / clearWorkflow / openWorkflowFrom 批量与重置操作的细粒度事件 | 恢复/替换整图后事件语义需定义（逐节点 diff 或整图重置事件）；本批聚焦 8 种类型各就绪 | **E2**（Undo 订阅化时定义） |
| 消费方（Undo / autosave / RunSession 订阅 EditEvent） | E1 只做「事件系统就绪」；消费方订阅化 = ARCH-E2（Undo / autosave）· ARCH-D22（RunSession 感知） | E2 / E2 后 |
| `FileEvent`（打开/保存/最近文件通知） | §1.3 三事件类型之一；本批只落地 EditEvent | ARCH-A（应用级收口） |

## 5. 过渡登记

- `EventBus` 由 `EditorState` 持有（`editor().event_bus()`）——目标态是独立通信层对象；**E3 拆 WorkflowModel 时迁出**（WorkflowModel 依赖 EventBus，§1.8.11）。
- `ParamChanged` 负载暂不带 `param_key`（app.cpp 消费 `PropertyEditResult` 只知「有参数变了」）；通知形态下消费者直接读 `node->params` 即可，需要 key 时后续批次补。
- `NodeMoved` 在拖拽期间每帧位置变化都发布（通知形态，重复无害）；`SelectionChanged` 已做变化检测防高频。
- `EditEvent::edge(EditEventKind, const engine::Edge&)` 的重载供「删除前取边数据」场景；`edge(kind, from, from_port, to, to_port, id)` 供 addEdge 返回值场景。

## 6. 回滚点

`git checkout -- source/ui/include/event_bus.h source/ui/src/event_bus.cpp source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/ui/src/node_canvas.cpp source/ui/src/app.cpp source/app/src/cli_selftest.cpp source/CMakeLists.txt docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 R4 形态（保留 `docs/audit/audit_arch_e1.md` 仅作登记；删除即可）。
