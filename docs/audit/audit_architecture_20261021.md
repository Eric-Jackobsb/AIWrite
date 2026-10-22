# 审计报告 · audit_architecture —— AIwrite 代码库逻辑结构与混乱度全量审计

> 日期：2026-10-21 · 性质：**逻辑拆解与混乱度评估**（用户指示「暂停修复，先做整体逻辑分析与拆解，评估混乱程度」）
> 范围：`source/` 全部自有代码（`ui/` `engine/` `nodes/` `ai/` `web/` `utils/` `tools/` `app/`），排除 `third_party/` 与 `source/python/`
> 方法：全量文件清单 + 关键文件逐段精读 + 快捷键/保存/校验/打包四条链路的调用链取证 + 行号级证据
> 目的：为「后续修复为何反复失败」提供结构性解释；为架构重构优先级提供依据。

---

## §1 代码库规模基线（自有代码）

| 层 | 文件数 | 总行数 | 典型文件（行数） |
|---|---|---|---|
| ui | 23 .cpp + 20 .h | ≈ 11,600 | app.cpp **1371** · editor_state.cpp **1181** · node_canvas.cpp **1180** · ce_draw.cpp 972 · canvas_view.cpp 938 |
| engine | 12 .cpp + 17 .h | ≈ 6,200 | composite.cpp **1014** · node_registry.cpp 899 · executor.cpp 886 · graph.cpp 766 |
| ai | ≈ 30 .cpp/.h | ≈ 7,300 | dom_web_client.cpp **2397** · provider_spec.cpp 1743 |
| web | 6 .cpp + 8 .h | ≈ 6,000 | webview_host.cpp **3489** · pydoll_channel.cpp 1273 |
| nodes | 2 .cpp | ≈ 1,270 | local_nodes.cpp 1242 |
| utils | ≈ 20 .cpp/.h | ≈ 2,300 | image_decode.cpp 758 |
| tools（自检） | 9 .cpp | ≈ 9,500 | api_execution_selftest.cpp **3888** · api_probe_selftest.cpp 1869 |
| app | 2 .cpp | ≈ 1,900 | main.cpp 661 · cli_selftest.cpp 1201 |

**规模定性**：自有代码合计约 **4.6 万行 C++**（不含 third_party 与 python）。单文件 >1000 行的有 **13 个**；其中 webview_host(3489) 与 dom_web_client(2397) 已被 `DEBT-C2` 评估为「高耦合无接缝，维持不拆」。这是一个**功能密度很高的单进程 GUI 应用**，但复杂度已超过「一个人凭记忆能可靠维护」的规模。

---

## §2 架构图景（设计意图 vs 实际）

### 2.1 设计意图（来自文档 / 头文件注释）

- **分层**：`ui`（编排/绘制）→ `engine`（Graph/校验/执行，可验证逻辑的唯一权威）→ `ai`/`web`（网络通道）→ `nodes`（节点执行器注册）。
- **关键原则**（editor_state.h 头注释）："所有可验证的逻辑都在 engine::Graph 内，本层只做编排与日志"。
- **单例**：`editor()` 全局唯一 EditorState（持有 Graph + UndoStack + Executor + autosave 状态）；`compositeEditorState()` 全局唯一（上级节点编辑会话）；`CompositeRegistry::instance()` 全局唯一（规格注册表）。
- **设计 §6.7**："MVP 不保留任何快捷键，全部改为鼠标操作"（node_canvas.cpp L337 注释）—— 但后来逐步打破了该决策（Delete 键、Ctrl+S 陆续补回）。

### 2.2 实际呈现（取证结果）

1. **全局单例 + 跨 TU 全局变量并存**：
   - `editor()` 单例持有 **Graph + UndoStack + Executor + RunSnapshot + 剪贴板 + autosave 8 个字段 + 最近文件 + 选中信息 + 一次性请求标志 + 后台诊断 mutex** —— 已是一个"上帝对象"。
   - 上级编辑器拆分后（CLN-13）仍用 `ce_internal.h` 暴露 **12 个跨 TU 全局变量**（g_ce_context / g_ce_rename_id / g_ce_rename_buf / g_ce_add_filter / g_ce_show_minimap / g_ce_align_snap / g_ce_grid_snap / g_ce_show_grid / g_ce_request_fit / g_ce_minimap / g_ce_undo / g_ce_redo），画布域/撤销域/会话域靠 extern 互相穿透。
   - node_canvas.cpp 自身另有 g_context / g_first_frame / g_drag_in_progress / g_delete_in_progress / g_alt_pan_active 5 个文件级静态。

2. **双层画布（主画布 vs 上级编辑器）逻辑高度重复**：
   - 两者各自实现 Delete 键处理（node_canvas.cpp L417 `handle_delete_key` vs ce_draw.cpp L58 `ce_handle_delete_key`）——**结构几乎一致**（WantTextInput / IsAnyItemActive / popup / 左键按住 / 窗口聚焦-悬停 / IsKeyPressed(Delete)），只是操作对象不同（Graph vs working.internal_graph）。
   - 两者各自实现选中同步 / 位置同步 / 上下文菜单 / 对齐吸附 / 小地图（后两者已部分上移共享 canvas_view，但交互层仍是两套）。
   - 两者各自持有一套 UndoStack（editor().undo vs compositeEditorState().undo），机制相同、互不相通。

3. **快捷键处理分散、无统一路由**：
   - 主画布：只有 Delete 键（node_canvas.cpp L417）。
   - 上级编辑器：Delete 键（ce_draw.cpp L58）+ Ctrl+S（ce_draw.cpp L118 `ce_handle_save_key`）。
   - **主画布至今没有 Ctrl+S**（问题 1 的根因）；菜单栏"保存工作流…"也没有绑定快捷键（app.cpp L904 的 MenuItem 无快捷字符串）。
   - imgui-node-editor 自带快捷键已全部关闭（两处 `ed::EnableShortcuts(false)`）。
   - 快捷键判定分散在各自窗口的每帧绘制函数里，**没有集中的输入/命令层**。

4. **保存链路双轨**：
   - 主画布：`saveWorkflowDialog()`（editor_state.cpp L1162）→ `saveWorkflowTo()`（L1037）→ `engine::saveWorkflow()`（workflow_io.cpp）。
   - 上级编辑器：`compositeEditorSave()`（ce_session.cpp L78）→ `CompositeRegistry::save()`（composite.cpp L760）→ `validate_composite_spec()`（L57）。
   - **两套保存各自独立**，dirty 标记、autosave 基线、状态栏提示各自维护；主画布保存完全不感知上级节点规格的存在。

5. **打包逻辑与校验规则存在结构性冲突**（问题 2 根因方向）：
   - `composite_pack_selection`（ce_pack.cpp L17）：暴露输入 = "有来自选区外的入边"的端口；**同时把选区内部连线整体拷贝**进 internal_graph（L53-57）。
   - `validate_composite_spec`（composite.cpp L165）：**"暴露输入对应的内部端口已有内部连线" → 直接报错拒绝**。
   - ⇒ 若某端口既被选区外节点连入（→ 被识别为暴露输入）、又被选区内节点连入（→ 内部连线被拷贝保留），打包产物**自身就无法通过保存校验**。两个判断分布在 UI 层（打包）与 engine 层（校验），对"暴露输入"的定义不一致。

---

## §3 混乱度评估（分维度）

> 评分：1（清晰）~ 5（严重混乱）。依据均为上述取证。

| 维度 | 评分 | 关键证据 |
|---|---|---|
| **模块边界 / 分层** | 4/5 | engine 层 composite.cpp 依赖 utils(log/paths)；ui 层直接裸操作 engine 数据结构字段（ce_pack 直改 spec.internal_graph / spec.inputs / exposed_params）；ui 层 editor_state 又反过来被 engine 无关逻辑（autosave/剪贴板/最近文件）撑爆 |
| **状态管理** | 5/5 | 三个全局单例 + 12 个跨 TU extern + 各文件级 static；状态散落且部分重复（selected 在主画布 vs 上级编辑器两套） |
| **职责单一** | 4/5 | EditorState 一对象兼 8 类职责；CompositeEditorState 兼会话+试运行+读模型；app.cpp 主循环函数承载菜单/工具栏/面板/状态栏/autosave 弹窗/看门狗/几何保存 |
| **重复代码** | 4/5 | Delete 键处理两套；画布交互层两套；UndoStack 两套；保存链路两套；对话框两套 |
| **数据流可追踪性** | 4/5 | 全局单例使"谁修改了什么"难以追踪；跨 TU 全局变量让 ce_draw 能写 g_ce_* 被 ce_session 读，无编译期约束 |
| **一致性（同因同果）** | 4/5 | 打包对"暴露输入"的定义 ≠ 校验对"暴露输入"的定义（问题 2）；主画布 Delete 用 `IsWindowFocused(ChildWindows)||IsWindowHovered()`（node_canvas.cpp L428），上级编辑器同样条件（ce_draw.cpp L68）—— 而 Ctrl+S 二修时把上级编辑器放宽为 `RootAndChildWindows`（ce_draw.cpp L229），**两处窗口判定口径从此分叉** |
| **文档 vs 代码** | 3/5 | 文档体系极完备（DocMAP/CHANGELOG/DevPlan/计划文档），但**注释里塞满里程碑编号**（每函数头一大段"为什么" + M_patchB/CN/CLN/DEBT 引用），代码即文档的噪音度偏高；文档数量 60+ 份，维护成本已超过代码本身 |

**总体混乱度：4/5 —— 功能可用、覆盖率高（自检 713 断言全绿），但结构上呈"单例总线 + 双画布双轨"形态，跨模块心智负担重，是多次修复反复失败的结构性原因。**

---

## §4 与本轮两个待修复问题的因果关系

### 4.1 问题 1：主画布 Ctrl+S 保存

- **根因**：主画布压根没有 Ctrl+S 处理。菜单（app.cpp L904）、工具栏（toolbar.cpp L85）只有"保存工作流…"按钮；快捷键只在上级编辑器实现了（ce_draw.cpp L118）。imgui-node-editor 快捷键已全关（L338）。
- **修复必须跨越的混乱点**：要加 Ctrl+S，需要决定"画布聚焦/悬停判定口径"（照抄 node_canvas Delete 的 `ChildWindows||Hovered`，还是对齐上级编辑器二修后的 `RootAndChildWindows`）。**两处口径现在不一致**，直接照抄某一处都会留下不一致。

### 4.2 问题 2：上级节点存在暴露节点时无法保存

- **根因方向**：`composite_pack_selection` 打包时的暴露输入识别（ce_pack.cpp L60-80，判据 = 有选区外入边）与 `validate_composite_spec` 的校验（composite.cpp L165，判据 = 暴露输入端口不得有内部入边）**对同一概念给出不同约束**。打包可生成"暴露输入同时带内部入边"的规格；保存时该规格被自己的校验拒绝。
- **修复必须跨越的混乱点**：打包（UI 层）与校验（engine 层）之间没有共享的"规格合法性"定义；改校验会破坏已有合法规格，改打包要动 UI 层逻辑。**正确解法应是把"暴露输入不得同时有内部入边"这条规则同时落实在打包生成阶段（不生成非法规格）与校验阶段（拒绝非法规格），两处同源。**

---


## §5 建议的演进方向（供拍板，本轮不执行）

按"风险从低到高 / 收益从高到低"排序：

| 优先级 | 动作 | 范围 | 收益 | 风险 |
|---|---|---|---|---|
| P0 | **键盘处理收敛**：把"画布窗口判定 + 快捷键判定"抽成共享函数（如 `canvas_view::key_guard()`），主画布与上级编辑器共用同一口径；Delete/Ctrl+S 走同一条守卫 | ui/canvas_view + 两个画布 | 消除口径分叉（问题 1/2 的共同土壤） | 低 |
| P0 | **打包-校验同源**：把"暴露输入不得有内部入边"等规格约束放进 engine 单一函数（如打包后立即校验并自动修正），打包不再手工拼装 | ui/ce_pack + engine/composite | 根治问题 2 及同类"自产非法规格" | 中 |
| P1 | **命令/快捷键路由层**：新增 `ui/commands.h`（Save/New/Open/Delete/Rerun…），菜单/工具栏/快捷键统一发命令，状态对象只响应命令 | ui | 消除三入口（菜单/工具栏/快捷键）行为漂移 | 中 |
| P1 | **画布交互层再上移**：Delete/选择同步/位置同步的"判定 + 操作"模板化，主画布与上级编辑器差参数化调用 | ui | 重复代码减半 | 中 |
| P1 | **EditorState 拆分**：把 autosave / 剪贴板 / 最近文件 / 后台诊断拆成独立小状态对象，EditorState 只留 Graph+Undo+Executor+选择 | ui | 职责单一化 | 中 |
| P2 | **全局变量收敛**：ce_internal.h 的 12 个 g_ce_* 收进一个 `CeCanvasState` 结构体，跨 TU 传引用 | ui | 减少隐式耦合 | 低 |
| P2 | **注释去噪音**：把函数头"为什么"注释中纯里程碑编号引用降噪为一句人话 | 全库 | 可读性 | 低 |

---

## §6 结论

- 代码库**功能成熟度高、自检覆盖好**，但**结构混乱度为 4/5**，核心病灶是三个：
  1. **单例总线 + 跨 TU 全局变量**使状态流不可追踪；
  2. **双画布双轨**导致同一概念（快捷键口径、暴露输入定义、删除/保存语义）在两处各自实现并逐渐分叉；
  3. **打包(UI)与校验(engine)对"合法规格"定义不一致**，是问题 2 的直接根因。

- 这与用户观察一致：**架构复杂 + 定义分散 → 单点修复容易撞上另一处的隐藏约束 → 反复失败**。
- 建议：**先做 P0 两项（键盘口径收敛 + 打包-校验同源）再修这两个 bug**，可显著降低修复回退概率；P1/P2 按批推进，每批小步验证（回归基线 = 构建 0/0 + exec 713/0 + graph 151/0 + check_links 1730/0）。

**文档结束。**
