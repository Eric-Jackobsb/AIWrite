# audit_arch_r3：ARCH-R3 落地审计报告（ReadOnlyView 渲染视图）

> 批次：`ARCH-R3` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 1
> 性质：**运行侧事件化第三批** —— `ReadOnlyView` 落地：渲染只读 `ReadOnlyView`，渲染与状态解耦。
> 判据：行为等价（`--exec-selftest` 713/0 只升不降）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §1.4 / §1.8.5 / 决策）

| 决策 | 结论 | 本批落点 |
|---|---|---|
| `ARCH-D30` | ReadOnlyView 构建者 = **主循环统一构建一次** | app.cpp 每帧构建一次 `ReadOnlyView`，传给所有渲染模块 |
| `ARCH-D31` | 字段 = **三个核心 + 配置**（工作流数据 / 选中 / 运行快照 / 配置） | `selection` + `snapshot` + `config` 引用字段 + graph 零拷贝访问器 |
| `ARCH-D32` | 快照为空表示 = **空对象**（未运行 `finished=false`） | 直接引用 `RunSnapshot`；空对象即未运行（复用现状语义） |
| `ARCH-D33` | 渲染函数签名 = **`render(const ReadOnlyView&)`** | 画布 / 输出面板 / 参数面板 / 信息面板 / 状态栏读取路径全部接收 `const ReadOnlyView&` |
| `ARCH-D34` | 工作流数据形态 = **接口收敛（零拷贝）** | `nodes_for_render()` / `edges_for_render()` 返回 graph 内部数组 const 引用 |
| `ARCH-D35` | 使用者 = **所有渲染模块** | 画布、参数面板、输出面板、状态栏、信息面板 |

## 2. 改动清单

### 2.1 新增 `source/ui/include/readonly_view.h`

- `SelectionReadView`（过渡）：引用 `selected_nodes` / `selected_links` / `selected_node` 三个 const 引用 —— `ARCH-E4` 拆出 `SelectionState` 后替换为该对象引用（调用点不变）。
- `ReadOnlyView`：
  - 私有 `const engine::Graph& graph_`（唯一持有的数据引用，零拷贝）。
  - 访问器 `nodes_for_render()` / `edges_for_render()`（返回 graph 内部数组 const 引用）、`find_node()` / `find_edge()`。
  - **过渡访问器 `graph()`**：画布绘制层（canvas_view）仍需整图只读查询（findNode / canConnect 等），`E5` 收敛交互后逐步收窄（登记为过渡，不视为最终接口）。
  - 字段 `selection` / `snapshot` / `config`。

### 2.2 主循环统一构建一次（`app.cpp` · ARCH-D30）

- 每帧在状态获取后构建：`const SelectionReadView sel{state.selected_nodes, state.selected_links, state.selected_node}; const ReadOnlyView view{state.graph, sel, state.run_snapshot_view(), config};`
- 传给：`drawNodeCanvas` / `draw_property_panel` / `drawOutputPanel` / `draw_info_panel` / 状态栏读取。

### 2.3 画布（`node_canvas.h/.cpp`）

- `drawNodeCanvas(const char*, CanvasOptions&, const ReadOnlyView& view)`：
  - 位置同步 / 节点卡片绘制 / 连线绘制 / 小地图 / 空画布引导 / 节点·连线计数：**全部改从 view 读取**（`nodes_for_render` / `edges_for_render` / `find_node` / `snapshot` / `graph()`）。
  - `g_canvas.run_snapshot` 改为 `[&view]{ return view.snapshot; }`。
- 交互写（右键菜单 / 连线创建·删除 / 删除选中 / 选中同步 / 拖拽 / Alt 平移 / 对齐吸附）仍走 `EditorState& state = editor()` —— 符合「渲染只读 view，交互写走 editor()（E3/E4 拆分后收敛）」。

### 2.4 输出面板（`output_panel.h/.cpp`）

- `drawOutputPanel(const char*, bool*, const ReadOnlyView& view, EditorState& state)`：
  - 渲染读取（graph / snapshot / findNode / runOutputText / node_output_text）改从 view。
  - 交互（导出文档 / 复制 / 归档路径 / follow_output_ 滚动清除）仍用 `state`。

### 2.5 参数面板（`property_panel.h/.cpp`）

- `draw_property_panel(const char*, bool*, engine::Node*, const ReadOnlyView& view, PropertyEditResult&)`：
  - 渲染读取（端口连通清单 / 生效提供商解析 / 未接线原因 / 运行状态快照 / 运行结果 / 同类节点统计）改从 view。
  - 编辑写（压快照 / 改参数 / setStatus / findNode 后改值 / 运行判断 `executor().running()` / `session_active()`）仍用 `editor()`（运行判断无 view 等价物，登记为交互）。

### 2.6 状态栏（`app.cpp`）

- 节点 / 连线 / 选中计数：改从 view（`nodes_for_render` / `edges_for_render` / `selection.nodes`）。
- `provider_summary_text` / `provider_mode_text` / `last_run_duration_text`：签名改收 `const ReadOnlyView&`，graph 遍历 / 快照从 view。

### 2.7 信息面板（`app.cpp`）

- `draw_info_panel(const char*, bool*, const ReadOnlyView& view)`：节点 / 连线 / 选中统计从 view；撤销栈深度仍读 `state.undo`（undo 不在 ReadOnlyView 字段内，E3 拆出 WorkflowModel 后归入文档级对象）。

### 2.8 自检（只升不降 · ARCH-D18 对象交互断言）

- `cli_selftest.cpp` 新增「ReadOnlyView」断言块：
  - 零拷贝：`&view.nodes_for_render() == &state.graph.nodes`、`&view.edges_for_render() == &state.graph.edges`。
  - `find_node` 与 `graph.findNode` 同源命中 + 缺失节点返回 nullptr。
  - 快照 / 配置 / 选中三字段同源（引用同一底层对象）。
  - 汇总进 `--run-selftest` 最终 PASS 判定。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（含新增「ReadOnlyView：节点零拷贝=是 / 连线零拷贝=是 / find_node=是 / 快照同源=是 / 配置同源=是 / 选中同源=是」） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1768 / 0**（基线持平；新增 audit_arch_r3.md 链接） |
| `rg "render(const ReadOnlyView"`（目标形态） | ✅ 画布 / 输出面板 / 参数面板 / 信息面板 全部渲染函数接收 `const ReadOnlyView&`（状态栏读取点直接经 view 变量） |

## 4. 过渡登记（非最终接口）

- `ReadOnlyView::graph()` 为**过渡访问器**：画布绘制层（canvas_view）仍需整图只读查询；`ARCH-E5` 收敛交互后逐步收窄为 nodes/edges + 查询方法（§1.8.5「不直接暴露 Graph」最终态）。
- `SelectionReadView` 为 `SelectionState` 拆分前的只读聚合（`ARCH-E4` 替换）。
- 参数面板运行判断 `editor().executor().running()` / `editor().session_active()` 仍走 editor()（无 view 等价物；运行会话侧语义，E 阶段收敛）。

## 5. 回滚点

`git checkout -- source/ui/include/readonly_view.h source/ui/include/node_canvas.h source/ui/src/node_canvas.cpp source/ui/include/output_panel.h source/ui/src/output_panel.cpp source/ui/include/property_panel.h source/ui/src/property_panel.cpp source/ui/src/app.cpp source/app/src/cli_selftest.cpp docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 R2 形态（保留 `docs/audit/audit_arch_r3.md` 仅作登记；删除即可）。
