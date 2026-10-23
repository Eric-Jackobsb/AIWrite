# CN_Editor_Refactor：上级节点编辑器画布对齐主画布（重构草案）

> 类型：**重构设计文档**（v0.2 · 已评审通过 · 存放 `docs/draft/`，实施完成后归位 `docs/actionPlan/`）
> 定位：解决「上级节点编辑器内部图画布的节点+连线体验与主画布漂移 + 小地图锚定错误」，目标是把上级编辑器画布**完全对齐主画布**，最佳路径是**直接沿用/共享主画布的『节点+连线』实现**，不再维护简化平行副本。
> 上游依据：用户 2026-10-19 反馈（上级节点中连线体验不行 · 小地图应固定在上级编辑器**渲染视图左下角**而非**画布**上 · 可进行重构，先出草案评审）
> 编号空间：`CEA-*`（任务 `CEA-0x`）· `CEA-D*`（决策）· `CEA-Q*`（待拍板）· `CEA-V*`（验证）
> 状态：🟢 **已评审通过（2026-10-19）· 阶段 1（CEA-01/CEA-02）代码完成 · 构建 0/0 + check_all 16/16 · 待 GUI 目测（CEA-V3/V8）**
> 相关：[Composition_node.md](../Archive/actionPlan/Composition_node.md)（上级节点机制）· `source/ui/src/node_canvas.cpp`（主画布）· `source/ui/src/composite_editor.cpp`（上级编辑器）· [M_node_expansion 系列](../Archive/actionPlan)（小地图 L3/对齐辅助）· [CHANGELOG.md](../CHANGELOG.md)
> 建立：**2026-10-19**（v0.1 · 零代码）

---

## 目录

- [§0 文档元信息与约定](#§0-文档元信息与约定)
- [§1 结论摘要（CEA-D1~D5）](#§1-结论摘要)
- [§2 现状诊断（带代码证据）](#§2-现状诊断)
- [§3 重构目标与架构](#§3-重构目标与架构)
- [§4 方案比选（CEA-Q1）](#§4-方案比选)
- [§5 改造明细（三阶段）](#§5-改造明细)
- [§6 对齐核对表](#§6-对齐核对表)
- [§7 验证计划（CEA-V1~V8）](#§7-验证计划)
- [§8 拍板结论（CEA-Q1~Q7）](#§8-拍板结论)
- [§9 实施拆解（评审后细化）](#§9-实施拆解)
- [§10 变更记录](#§10-变更记录)

---

## §0 文档元信息与约定

### 0.1 定位

- **本文是「上级节点编辑器画布对齐主画布」重构的设计依据**：把上级编辑器内部图的画布渲染与交互从「简化平行副本」改为「共享主画布实现」，并修复小地图锚定。
- **本文只动 UI 层**：不涉及引擎层（`Graph` / `Node` / 执行器 / 规格校验均不变）；不改变上级节点机制本体（归档 [Composition_node.md](../Archive/actionPlan/Composition_node.md) 的规格 / 展开 / 试运行设计）。
- **不改他文**：对 `Composition_node.md` / `M_node_expansion*.md` / `M_canvas_snapshot.md` 只单向引用。

### 0.2 本文要解决的那一个问题

> 「上级节点编辑器内部图画布的节点+连线，和主画布长得不一样、连起来也不顺手，小地图还钉在画布上乱跑。」—— 答案 = 上级编辑器直接**共享主画布的『节点+连线』实现**（单一事实来源，杜绝再漂移），小地图改为**视图左下角固定**（屏幕空间）。

### 0.3 执行原则

1. **共享优先于复制**：能共用主画布实现就共用；复制到第二份 = 再次埋下本次问题的根因。
2. **主画布行为基线不变**：共享化是「抽公共、参数化差异」，不是重写主画布；每阶段构建 + 自检 + GUI 冒烟为门禁。
3. **文档驱动**：§5 明细可逐字实现、§6 核对表实现时逐项勾、§8 待拍板归零后才动代码。
4. **小步落地**：三阶段（绘制层 → 交互层 → 收尾），每阶段可独立验证、可回滚。

---


## §1 结论摘要（CEA-D1~D5）

| 编号 | 决策 | 一句话 |
|---|---|---|
| `CEA-D1` | **共享实现** | 上级编辑器画布改为**共享主画布『节点+连线』实现**（节点卡片 / 端口 / 连线创建 / 删除 / 对齐吸附 / 小地图），删除简化平行副本。 |
| `CEA-D2` | **小地图视图锚定** | 小地图移到 `ed::End()` **之后**、屏幕空间绘制（与主画布同款时序），固定上级编辑器**渲染视图左下角**；缩略节点改用 `state_color` 运行状态着色。 |
| `CEA-D3` | **画布上下文** | 通过 `CanvasContext` 参数化两画布差异（graph 引用 / 运行读模型 / 撤销 / 状态反馈 / 双击与右键菜单策略 / 内容宽度 / 小地图角落），主画布与上级编辑器共用同一套画布实现。 |
| `CEA-D4` | **分三阶段落地** | 阶段 1 只读绘制层共享 → 阶段 2 交互层共享 → 阶段 3 删除残留平行实现 + 文档收尾；每阶段独立构建/自检/GUI 门禁。 |
| `CEA-D5` | **配色区分** | 主画布与上级编辑器画布通过**差异主题**区分（画布背景 / 节点背景 / 标题区等），仅换色、结构与布局不变；颜色经 `CanvasContext.theme` 统一取用。 |

> 方案比选见 §4：**方案 A 共享模块**已拍板采纳；`CEA-Q1~Q7` 全部拍板，见 §8。

---

## §2 现状诊断（带代码证据）

### 2.1 连线「不行」：上级编辑器是主画布的「简化平行副本」

上级编辑器**底层已经使用 imgui-node-editor 的节点+连线模型**（`ed::BeginNode` / `ed::Link` / `ed::BeginCreate`，见 `composite_editor.cpp:1444~1522`），但节点卡片与连线交互是**主画布的简化版**，两套实现各自演化导致体验漂移：

| # | 主画布（`node_canvas.cpp`） | 上级编辑器（`composite_editor.cpp`） | 影响 |
|---|---|---|---|
| 1 | **节点卡片** `draw_node_body`（L614）+ `draw_pin`（L323）：分类色条 + 状态圆点 + **真正左右两列端口**（`PinPivotAlignment` 输入取矩形左中、输出取右中）+ 参数预览 + 提供商提示 + 图片卡片 + **动态内容宽度** `update_content_width`（L460，每帧按最宽端口标签自适应）+ 结果 `wrap_to_width` 折行 | **简化卡片** `ce_draw_node`（L161）：无分类色条；输入端口 = `Text + 6px Dummy`、输出 = `6px Dummy + Text`，**未调 `PinPivotAlignment`**；内容区固定 160px；结果固定 64 字符截断 | 端口热点小、**连线锚点落在卡片内部而非左右边缘**、视觉与主画布明显不一致 |
| 2 | **连线创建** `handle_create_link`（L788）：`canConnect(..., &would_replace)` → **支持『替换原连线』**；被拒时 `log::warn` + `set_status`；拖拽时**鼠标旁类型/兼容提示**（M_node_expansion L3-09，L842~883） | 连线创建块（L1464~1503）：`canConnect` **不传 `would_replace`** → 目标端口已连线时不能替换；被拒只写 `state.error`（无 log/status）；**无拖拽提示** | 连不上已占用端口、失败原因不可见、无连线类型提示 |
| 3 | **删除** `handle_delete_items`（L891）：`snapshot` + `removeEdge` + `log_summary` + `set_status` | 删除块（L1506~1522）：`snapshot` + `removeEdge`，**无状态/日志反馈** | 删除后无确认反馈 |
| 4 | **节点双击**（L1835）：聚焦标题参数 / **打开上级编辑器** | 双击（L1632）：改名弹窗 | 合理差异，保留（见 `CEA-Q6`） |
| 5 | **节点右键菜单**（L1760~）：重跑 / 删除 | 右键菜单（L1529~）：改名 / 删除 | 合理差异，保留（见 `CEA-Q4`） |

> 「连线不行」= ① 连线锚点/端口热点弱（简化 `ce_draw_node` 未对齐主画布 `draw_pin`）；② 不支持替换已占用输入端口；③ 无拖拽类型提示与被拒反馈。具体观感待 GUI 复现时逐项确认（`CEA-V3/V4`）。

### 2.2 小地图锚定错误（CEA-D2 依据）

| 项 | 主画布 | 上级编辑器 | 判定 |
|---|---|---|---|
| **绘制时机** | `draw_minimap` 在 **`ed::End()` 之后**（`node_canvas.cpp:1898`，屏幕空间）→ 平移/缩放**不移动**，锚定视图 | `ce_draw_minimap` 原在 **`ed::Begin` 之内、`ed::End` 前**（`composite_editor.cpp`）→ 处于画布渲染管线内部，**观察表现为随画布跑** | ✅ **已修复（v0.4）**：移到 `ed::End()` 之后、`SetCurrentEditor(nullptr)` 之前，与主画布同时序 |
| **位置几何** | `minimap_rect`（L1371）| `ce_minimap_rect`（L314）两者公式相同：`x = 右缘 - 190 - margin` → 实际落在视图**右下**（但开关文案写「左下角」） | 需求明确**左下角**（`CEA-Q2`：主画布是否一并统一待拍板） |
| **缩略节点色** | `state_color(node.state)`（运行状态着色） | 固定 `IM_COL32(190,195,220,200)` 灰蓝 | 改为 `state_color` |
| **输入隔离** | `block_minimap_input()` 在 `ed::Begin` **前**摘除该区域鼠标事件（L1387~1412） | `ce_block_minimap_input()` 同款（L330） | ✅ 已一致，保留 |

### 2.3 根因

> 上级编辑器画布是主画布的**简化平行副本**（`ce_draw_node` / `ce_resolve_pin` / `ce_minimap_*` / `ce_apply_snap` 各自独立演化）。只要存在两份实现，视觉与交互就必然漂移——本次「连线不行 + 小地图不对」正是漂移结果。**重构 = 消灭第二份实现。**

---

## §3 重构目标与架构（CEA-D1 / CEA-D3）

### 3.1 目标

1. 上级编辑器内部图画布 = 主画布同款「节点+连线」渲染与交互（像素级一致、行为一致）。
2. 小地图 = 渲染视图**左下角**固定（屏幕空间，平移/缩放不动）。
3. **单一事实来源**：主画布与上级编辑器共用一套画布实现，杜绝再次漂移。

### 3.2 画布上下文（CanvasContext）

两画布差异点全部收敛进一个上下文对象，共享画布实现只读它、不碰任何全局状态：

| 槽位 | 主画布填充 | 上级编辑器填充 |
|---|---|---|
| graph | `state.graph` | `state.working.internal_graph` |
| 运行读模型 | `editor().run_snapshot_view()` | `composite_editor_run_snapshot()` |
| 撤销快照 | `state.snapshot(label)` | `composite_editor_snapshot(label)` |
| 状态栏 / 日志 | `state.set_status` / `log_summary` / `log::*` | `state.error` / `state.run_status` / `log::*` |
| 内容宽度 | `update_content_width(state.graph)` | 同机制，喂 `internal_graph` |
| 节点双击 | 聚焦标题 / 打开上级编辑器 | 改名弹窗（`CEA-Q6`） |
| 节点右键菜单 | 重跑 / 删除 | 改名 / 删除（`CEA-Q4`） |
| 小地图角落 | 现状右下（`CEA-Q2`） | **左下**（`CEA-D2`） |
| 上级节点紫条 / 打开嵌套 | 需要（`composite_id` 非空） | 不需要（内部图无嵌套）→ 统一分类色 |
| 主题配色（`CEA-D5`） | 默认主题（现状配色） | 差异主题：画布背景 / 节点背景 / 标题区底色，端口与连线**与主画布一致** |

> 注意：共享实现的**内部数据结构**（node/pin/link 手柄方案 `n<seq>` / `e<seq>` / `pin = node<<8|dir|idx`）两画布**已一致**（`ce_node_handle` 与主画布 `node_handle` 同方案），无需处理。

### 3.3 共享实现承载位置（`CEA-Q1` 子项）

见 §4 方案比选。推荐**方案 A**：新增 `ui/include/canvas_view.h` + `ui/src/canvas_view.cpp`，把主画布的画布渲染/交互迁入；`node_canvas.cpp` 与 `composite_editor.cpp` 都变成「薄壳 + 上下文」。

---

## §4 方案比选（CEA-Q1）

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A 共享模块**（推荐） | 新增 `canvas_view.{h,cpp}`，把 `draw_node_body` / `draw_pin` / `update_content_width` / `handle_create_link` / `handle_delete_items` / `sync_selection` / `handle_delete_key` / 右键菜单 / 双击 / 对齐吸附 / 小地图迁入，全部走 `CanvasContext`；两画布改调它 | 单一事实来源、**长期不再漂移**；上级编辑器能力自动追平主画布（替换连线 / 拖拽提示 / 状态反馈） | 动主画布 → 回归风险；需分阶段 + 自检门禁 |
| **B 最小参数化** | 不新建文件：把 `node_canvas.cpp` 里这些函数参数化（接受 graph / snapshot / 回调），由 `composite_editor.cpp` 直接调用 | 改动面小、无新文件 | `node_canvas.cpp` 继续膨胀；共享逻辑仍寄生在「主画布文件」里，概念上易再漂移 |
| **C 逐字移植** | 把主画布 `draw_node_body`/`draw_pin`/`handle_create_link` 等**复制**进 `composite_editor.cpp`（含小地图修复） | 完全不动主画布、风险最小、见效最快 | **又是第二份拷贝** → 本次问题的根因原样复现，日后仍会漂移 |

> **推荐 A（若嫌重可退 B 作为第一阶段过渡，最终收敛到 A）**。C 被明确否决——它只是把「简化副本」换成「完整副本」，治标不治本。
>
> **A 的风险缓解**：主画布行为基线不变（只搬家不重写）；阶段 1 只动只读绘制、阶段 2 才动交互；每阶段跑 `build.ps1 0/0` + `api_probe --graph-selftest / --exec-selftest` + `check_all.ps1` + GUI 冒烟，任何回归立即回退该阶段。

---

## §5 改造明细（三阶段，CEA-D4 / CEA-D5）

### 配色区分（CEA-D5）

主画布**保持现状配色不变**；上级编辑器画布采用**差异主题**，仅改颜色、不改结构/布局/交互：

| 元素 | 主画布（默认） | 上级编辑器（差异） |
|---|---|---|
| 画布背景 | 现状 | 差异深色（偏蓝，数值实现时 GUI 目测后记录） |
| 节点背景 | 现状 | 差异节点底色（同族色阶） |
| 节点标题区 / 分类色条 | 现状 | 分类色条**沿用**（结构一致），标题区底色差异 |
| 端口圆点 / 连线 | 现状 | **与主画布一致**（连线体验对齐优先，不做色差干扰） |
| 小地图 | 现状 | 同差异画布底色（保持与所在画布一致观感） |

实现方式：`CanvasContext` 增加 `theme` 槽位（主画布=默认 / 上级编辑器=差异），共享绘制代码**只从 `ctx.theme` 取色**，一处定义两画布各自取用；颜色值定为常量放 `canvas_view.h`。

### 阶段 1：只读绘制层共享（零行为变更）

- 抽出：`draw_node_body` / `draw_pin` / `update_content_width` / `port_row_width` / `result_view_of` / `node_card_image` / `wrap_to_width` / `elide_to_width` / `draw_node_separator` / 标题与端口常量 → `canvas_view.{h,cpp}`，全部经 `CanvasContext` 取 graph 与运行读模型。
- `composite_editor.cpp` 画布节点循环（L1444）改调共享 `draw_node_body`；**删除 `ce_draw_node`**（含结果摘要简化版，改用共享 `result_view_of`）。
- 内容宽度：上级编辑器从「固定 160px」改为共享 `update_content_width(internal_graph)` 动态宽度。
- 门禁：构建 0/0 + 自检全绿 + GUI 两画布视觉比对（`CEA-V3`）。

### 阶段 2：交互层共享

- 抽出：`handle_create_link`（含替换连线 / 拖拽类型提示 L3-09 / 被拒 log+status）/ `handle_delete_items`（含状态反馈）/ `sync_selection` / `handle_delete_key` / 右键菜单框架 / 双击处理框架 / 对齐吸附 / 小地图 → `canvas_view`，经 `CanvasContext` 分发。
- 上级编辑器获得：**替换已占用端口**、**拖拽类型/兼容提示**、**删除与被拒的状态反馈**（写入 `state.error` / `state.run_status`，日志走 `log::*`）。
- **小地图**（`CEA-D2`）：
  - 绘制时机移到 `ed::End()` **之后**（与主画布 `node_canvas.cpp:1898` 同款）；
  - `minimap_rect` 增加角落参数（`CanvasContext.minimap_corner`），上级编辑器传**左下**；
  - 缩略节点色改 `state_color(node.state)`；
  - 输入隔离 `block_minimap_input()`（`ed::Begin` 前）保持不变。
- 门禁：构建 0/0 + 自检全绿 + GUI 连线/小地图逐项（`CEA-V4/V5`）。

### 阶段 3：收尾

- 删除被共享替代的上级编辑器残留：`ce_draw_node` / `ce_resolve_pin` / `ce_minimap_*` / `ce_block_minimap_input` / `ce_apply_snap` / `ce_handle_delete_key` 等（以共享版本替换）。
- `composite_editor.cpp` 画布区瘦身为：建上下文 → `ce_block_minimap_input`（或共享入口）→ `ed::Begin` → 共享绘制/交互 → `ed::End` → 共享小地图 → 右侧规格面板。
- 文档：CHANGELOG / 使用说明 / `docs/DevPlan.todo` Test / `docs/DocMAP.md` 同步；`docs/draft/CN_editor_refactor.md` 归位 `docs/actionPlan/`。

---


## §6 对齐核对表（实现时逐项勾）

| 能力 | 主画布现状 | 上级编辑器现状 | 对齐动作 | 勾 |
|---|---|---|---|---|
| 节点卡片（分类色条/状态点/双列端口/参数预览/提供商提示/图片卡片/结果折行） | ✅ 完整 | ✅ 共享（阶段 1 完成） | 共享 `draw_node_body` | ✅ |
| 端口圆点 + 连线锚点（`PinPivotAlignment` 左中/右中） | ✅ | ✅ 共享 | 共享 `draw_pin` | ✅ |
| 动态内容宽度（最宽端口标签自适应） | ✅ | ✅ 共享 | 共享 `update_content_width` | ✅ |
| 拖拽连线 + 类型不符变红 | ✅ | ✅ | 共享 `handle_create_link` | ☐ |
| 替换已占用输入端口（`would_replace`） | ✅ | ❌ | 共享 | ☐ |
| 拖拽类型/兼容提示（L3-09） | ✅ | ❌ | 共享 | ☐ |
| 被拒原因反馈（log + status） | ✅ | ⚠️ 仅 error | 共享 + Context 分发 | ☐ |
| 删除节点/连线（撤销 + 状态反馈） | ✅ | ⚠️ 无反馈 | 共享 | ☐ |
| 对齐吸附 + 网格吸附 + 辅助线 | ✅ | ✅（平行实现） | 共享，删平行 | ☐ |
| 小地图：绘制时机 | `ed::End` 后 | ❌ `ed::Begin` 内 | 统一 `ed::End` 后 | ☐ |
| 小地图：位置 | 右下（文案左下） | 右下 | 上级=**左下**（`CEA-Q2`） | ☐ |
| 小地图：节点着色 | `state_color` | ❌ 固定灰蓝 | 统一 `state_color` | ☐ |
| 小地图：输入隔离 | ✅ | ✅ | 保留 | ☐ |
| 节点双击 | 聚焦标题/打开上级 | 改名弹窗 | Context 策略（`CEA-Q6`） | ☐ |
| 节点右键菜单 | 重跑/删除 | 改名/删除 | Context 策略（`CEA-Q4`） | ☐ |
| 配色区分（画布/节点背景差异主题） | 主画布=默认 | 上级=差异 | `ctx.theme` 统一取色（`CEA-D5`） | ✅ |

---

## §7 验证计划（CEA-V1~V8）

| 编号 | 验证项 | 门禁 |
|---|---|---|
| `CEA-V1` | 编译 | `build.ps1` **0 error / 0 warning** |
| `CEA-V2` | 引擎自检回归 | `api_probe --graph-selftest` 151/0 · `--exec-selftest` 682/0（上级 91/0）· `--selftest` 全 PASS · `check_all.ps1` 全 PASS |
| `CEA-V3` | GUI：两画布节点卡片逐项视觉比对 | 标题/色条/状态点/端口圆点/参数预览/结果摘要一致 |
| `CEA-V4` | GUI：连线交互 | 拖拽锚点在端口圆点、类型提示、替换已占用端口、被拒原因可见 |
| `CEA-V5` | GUI：小地图 | 固定渲染视图**左下角**，平移/缩放不动；点击跳转正确；✕ 关闭/开关恢复；`state_color` 着色 |
| `CEA-V6` | GUI：试运行回归 | 「执行连线」状态圆点/结果摘要照常；与主画布运行互斥照常；退出无残留线程（`composite_editor_stop_run`） |
| `CEA-V7` | 文档 | CHANGELOG / DevPlan.todo Test / DocMAP 更新；核对表全勾 |
| `CEA-V8` | GUI：配色区分目测 | 两画布背景/节点底色**一眼可辨**；端口/连线/结构与主画布仍一致；小地图同差异色 |

---

## §8 拍板结论（CEA-Q1~Q7 · 已全部拍板）

| 编号 | 问题 | 拍板 |
|---|---|---|
| `CEA-Q1` | 方案 A / B / C | **方案 A（共享模块）** |
| `CEA-Q2` | 主画布小地图是否统一为左下角 | **一并统一为左下角** |
| `CEA-Q3` | 上级编辑器卡片复用区块范围 | **完整复用**（含图片卡片只读展示） |
| `CEA-Q4` | 上级编辑器右键菜单 | **保持 改名/删除** |
| `CEA-Q5` | 一次性 vs 分三阶段 | **分三阶段** |
| `CEA-Q6` | 上级编辑器双击行为 | **保持改名弹窗** |
| `CEA-Q7` | 画布配色区分（新增，用户拍板） | **差异主题**（`CEA-D5`：画布背景 / 节点背景等） |

---

## §9 实施拆解（评审后细化）

> 待 §8 待拍板归零后展开为任务 `CEA-01`~`CEA-0n`，逐任务标注文件、函数、验证项。占位如下：

- ✅ `CEA-01`：新建 `canvas_view.{h,cpp}` + `CanvasContext`（含 `theme` 槽位），迁移只读绘制层（阶段 1）。**2026-10-19 完成**：构建 0/0 + check_all 16/16；待 GUI 目测（`CEA-V3/V8`）。
- ✅ `CEA-02`：`composite_editor.cpp` 画布节点循环改走共享绘制，删除 `ce_draw_node`，切换差异主题（`CEA-D5`）。**2026-10-19 完成**（同步删除 `ce_result_view` / `CeResultView` / `ce_*_handle` / `ce_resolve_pin`，统一走 `canvas_view` 手柄与绘制）。
- `CEA-03`：迁移交互层（连线/删除/选中/右键/双击/吸附/小地图）至共享（阶段 2）。
- `CEA-04`：小地图时序 + 左下角 + `state_color` 改造（`CEA-D2`）。
- `CEA-05`：主画布小地图统一左下角（`CEA-Q2`）。
- `CEA-06`：删除上级编辑器残留平行实现（阶段 3）。
- `CEA-07`：文档收尾 + 核对表勾选 + CHANGELOG。

---

## §10 变更记录

| 版本 | 日期 | 内容 |
|---|---|---|
| v0.1 | 2026-10-19 | 初稿：诊断 + 方案 A 推荐 + 三阶段拆分 + 待拍板 Q1~Q6（零代码） |
| v0.2 | 2026-10-19 | 评审通过：Q1~Q6 全部拍板（方案 A / 主画布小地图统一左下角 / 完整复用 / 保持右键与双击策略 / 三阶段）+ 新增 `CEA-D5` 配色区分（`CEA-Q7`）；进入实施 |
| v0.3 | 2026-10-19 | 阶段 1 落地（CEA-01/CEA-02）：新增 `canvas_view.{h,cpp}` 共享只读绘制层（`CanvasContext` + `CanvasTheme`）；`node_canvas.cpp` / `composite_editor.cpp` 改走共享绘制；上级编辑器切换**差异主题**（画布/节点背景/小地图偏蓝）；删除 `ce_draw_node` / `ce_result_view` / `ce_*_handle` 平行实现。验证：构建 0/0 · check_all **16/16 PASS** · V-12 取消与超时单跑 PASS（check_all 首跑瞬时抖动，重跑复绿）。待 GUI 目测（`CEA-V3` / `CEA-V8`） |
| v0.4 | 2026-10-19 | 阶段 2 跟进两项修复：**(1) `CEA-D2` 小地图时序最终落地** —— 上级编辑器 `ce_draw_minimap` + `ce_draw_snap_lines` 从 `ed::Begin`…`ed::End` **内部**移到 `ed::End()` **之后**（`SetCurrentEditor(nullptr)` 之前），与主画布时序一致（`node_canvas.cpp`：`ed::End()` → 辅助线/小地图 → `SetCurrentEditor(nullptr)`）。根因：画布内绘制坐标叠加画布平移/缩放变换 ⇒ 观察表现为「小地图嵌在画布内、随画布跑」；移出后固定锚定视图左下角。**(2) 新增上级编辑器内联文本编辑** —— `TextInput` 节点「文本内容」参数在卡片上直接渲染为**可编辑多行输入框**（官方 widgets-example 同款模式；`CanvasContext.inline_edit_text` 仅上级画布开启，主画布保持只读预览零变更）。值变化即写回工作副本 + 标脏（`on_param_edit_commit`），编辑开始压「修改前」快照（`on_param_edit_begin` → 可撤销），失焦自动回滚到模型值。验证：构建 0/0 · check_all **14/14 PASS**（graph-selftest 151/0 · exec-selftest 682/0 · 断链 0）。待 GUI 目测（`CEA-V3` / `CEA-V5` 小地图 / 内联文本编辑） |
| v0.5 | 2026-10-19 | 上级编辑器「添加内部节点」落点明确为**背景右键那一刻的画布坐标**：右键画布任意位置 → 菜单中选节点 → 新节点初始位置即右键点（画布坐标，`ce_spawn_position` 于 `ShowBackgroundContextMenu()` 时经 `ed::ScreenToCanvas(ImGui::GetMousePos())` 捕获；弹窗内两路调用统一经 `ce_spawn` 传入 `composite_editor_add_node`）。验证：构建 0/0 · graph-selftest 151/0。待 GUI 目测：上级编辑器右键添加任意节点 → 新节点落在右键点 |

