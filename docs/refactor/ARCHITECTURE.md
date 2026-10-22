# 架构提案 · ARCHITECTURE —— AIwrite 目标架构设计（v1.1 定稿）

> 日期：2026-10-22 · 状态：🟢 **v1.1 定稿（用户拍板 + 对象定义总表）** · 编号空间：`ARCH-*`
> 前置：[FREEZE.md](FREEZE.md) 已生效 · 现状审计 = [../audit/audit_architecture_20261021.md](../audit/audit_architecture_20261021.md)
> 原则：**目标架构只解决结构混乱问题，不改变产品行为**；重构为增量式、每步可编译可自检。
> 核心决策：**事件通知（形态 1）+ 渲染直接读 + 先运行后编辑 + `editor()` 完全退场**。

---

## §1 目标架构总览

### 1.1 分层纪律

```
app/           进程入口 / 窗口 / 主循环 / AppContext（应用级聚合）
  │
ui/            编排层：只持有"界面状态 + 对 engine 的调用"，不含可验证业务逻辑
  │  (唯一合法方向：ui → engine；禁止 ui → ui 跨模块裸 extern)
engine/        领域层：Graph / 校验 / 执行 / 工作流 IO / 上级节点规格（唯一权威）
  │
ai/ web/ nodes/ utils/   能力层：无 UI 依赖，全部经 engine 间接暴露给 ui
```

**规则 1（依赖单向）**：`engine/*`、`nodes/*`、`ai/*`、`web/*`、`utils/*` 的头文件**禁止 include** `ui/*` 与 `app/*`；CI 用 include 扫描断言。

**规则 2（可验证逻辑归 engine）**：凡是"能被自检覆盖的判定"（校验、打包、边界识别、排序、序列化）**必须**在 engine 实现并被 `api_probe` 覆盖；ui 层只做编排与展示。

### 1.2 核心状态层（直接可读，只读零开销）

```
WorkflowModel（Graph + Undo）
SelectionState（选中）
RunSession（Executor + 快照）
```

**规则 3（核心状态直接可读）**：渲染**直接读**核心状态，不经过事件系统。事件只用于「变化通知」。

### 1.3 通信层：EventBus（形态 1：通知）

```
EditEvent  →  Undo / autosave / 诊断
RunEvent   →  输出面板 / 状态栏 / 诊断（复用现有队列）
FileEvent  →  最近文件 / 诊断
```

**规则 4（事件作为通知）**：生产者只发事件，不关心谁消费；消费者各自订阅处理。

### 1.4 ReadOnlyView（每帧构建，只存引用，零拷贝）

```cpp
struct ReadOnlyView {
    // 工作流数据：直接返回 graph 内部数组的 const 引用（零拷贝）
    const std::vector<Node>& nodes_for_render() const;
    const std::vector<Edge>& edges_for_render() const;

    const SelectionState& selection;
    const RunSnapshot& snapshot;   // 空对象 = 未运行（finished=false）
    const Config& config;

private:
    const Graph& graph_;           // 唯一持有的数据引用
};
```

**规则 5（渲染只读视图）**：渲染只读 `ReadOnlyView`，不散落各处直接访问三个对象。

### 1.5 生命周期分层

```
【应用级】  AppContext
              ├─ Config
              ├─ Clipboard
              ├─ Diagnostics
              ├─ RecentFiles（由 WorkflowFile 持有）
              └─ AutosaveService

【文档级】  WorkflowModel（Graph + Undo）
            SelectionState

【会话级】  RunSession（Executor + 快照）

【通信层】  EventBus
              ├─ EditEvent 队列
              ├─ RunEvent 队列（复用现有）
              └─ FileEvent 队列
```

**规则 6（跨帧可变状态显式化）**：任何跨帧存活的可变状态必须是某个对象成员；文件级 `static` 与跨 TU `extern` 全局在重构结束后**归零**。

### 1.6 状态生命周期表

| 状态 | 创建时机 | 销毁时机 | 变化原因 | 生命周期 |
|---|---|---|---|---|
| **Config** | 启动 | 退出 | 设置面板 | **应用级** |
| **Clipboard** | 启动 | 退出 | 复制/粘贴 | **应用级** |
| **Diagnostics** | 启动 | 退出 | 后台线程 | **应用级** |
| **RecentFiles** | 启动 | 退出 | 打开/保存 | **应用级** |
| **AutosaveService** | 启动 | 退出 | 定时 | **应用级** |
| **Graph** | 加载工作流 | 关闭工作流/退出 | 编辑 | **文档级** |
| **Undo 栈** | 加载工作流 | 关闭工作流 | 编辑 | **文档级** |
| **SelectionState** | 加载工作流 | 关闭工作流 | 点击 | **文档级** |
| **RunSession** | 启动（常驻） | 退出 | 每次运行重置 | **会话级** |
| **RunSnapshot** | 点击运行 | 下次运行 | 运行推进 | **会话级** |

### 1.7 模块依赖矩阵

| 模块 | 依赖 | 不依赖 |
|---|---|---|
| 画布 | `WorkflowModel` + `SelectionState` + `ReadOnlyView` | `RunSession` · `EventBus`（只发事件） |
| 参数面板 | `WorkflowModel` + `SelectionState` + `EventBus` | `RunSession` |
| 工具栏 | `WorkflowModel` + `RunSession` + `WorkflowFile` + `EventBus` | `SelectionState`（间接） |
| 菜单 | `AppContext`（仅启动注入）+ `WorkflowFile` + `RunSession` + `EventBus` | — |
| 输出面板 | `ReadOnlyView`（快照）+ `RunEvent` 订阅 | `WorkflowModel` |
| 状态栏 | `ReadOnlyView` + `RunEvent` 订阅 | — |
| 设置面板 | `Config`（启动注入） | — |
| Console | `Diagnostics` | — |

### 1.8 对象定义总表

> **每个对象 = 一句话定义 + 每个词有落点 + 无未解释术语。**
> 定义原则：先讲「做什么 / 有什么用 / 不做什么」，再展开接口与决策。

#### 1.8.1 `EventBus`（通信层）

**一句话定义**：`EventBus` 持有按事件类型分组的订阅者回调列表，提供订阅和发布接口；单线程 FIFO 分发，不跨线程，不存储事件历史。

| 维度 | 内容 |
|---|---|
| 标准写法 | 订阅者列表（`std::vector<std::function<void(const Event&)>>` 或按类型分组）+ `subscribe(kind, callback)` / `publish(event)` |
| 事件类型 | `EditEvent`（8 种）/ `RunEvent` / `FileEvent` |
| 线程模型 | 单线程（UI 线程）FIFO |
| 限制 | 不跨线程、不存储历史、不做事件合并 |

#### 1.8.2 `RunSession`（会话级）

**一句话定义**：`RunSession` 接收一份工作流的节点和连线数据，按「上游节点先跑完、下游节点才开始」的顺序逐个执行节点，记录每个节点的状态、输出、耗时和错误，供界面和其他模块读取；它不保存工作流文件、不检查工作流是否合法、不直接刷新界面。

| 维度 | 内容 |
|---|---|
| 持有执行器 | ✅ 创建 / 销毁执行器 |
| 常驻 / 新建 | **常驻**（启动创建，退出销毁；每次运行重置内部状态） |
| 运行中修改工作流 | **自己感知**（订阅编辑事件，收到就停） |
| 归档 | **不负责**（归 `WorkflowFile`，订阅「运行结束」事件后写） |
| 快照 | **全量**（每帧从执行器重新读一遍，重建快照） |
| 启动时数据形态 | 接收 `const Graph&`，**内部拷贝一份**（运行时改工作流不影响运行） |
| 限制 | 不持有 Graph、不校验、不写归档、不碰 UI |

#### 1.8.3 `WorkflowModel`（文档级）

**一句话定义**：`WorkflowModel` 持有当前工作流的全部节点、连线，以及一份独立的撤销栈（每次修改前保存完整快照），提供增删改节点、连线、参数的操作，修改后发出编辑事件；它不执行节点、不校验、不保存文件、不渲染界面、不持有选中状态。

| 维度 | 内容 |
|---|---|
| 撤销栈归属 | **独立 `UndoStack` 类，由 `WorkflowModel` 持有** |
| 运行前校验 | **不在 `WorkflowModel`**（归 `RunSession::start()`） |
| 选中状态 | **`SelectionState` 独立**（`WorkflowModel` 不持有） |
| 加载 / 关闭 | **`WorkflowModel::load(graph)`** 替换内部数据 + 清空撤销栈 |
| 限制 | 不执行、不校验、不保存文件、不渲染、不持有选中 |

#### 1.8.4 `SelectionState`（文档级）

**一句话定义**：`SelectionState` 持有当前选中的节点 id 集合，提供增删改查接口，变化时发出「选中变化」编辑事件；它不持有工作流数据、不渲染、不执行。

| 维度 | 内容 |
|---|---|
| 标准写法 | `std::unordered_set<std::string> selected_node_ids` + `add` / `remove` / `clear` / `is_selected` / `selected_ids` |
| 事件 | 变化时发 `EditEvent::SelectionChanged` |
| 生命周期 | 文档级（加载工作流时清空） |
| 限制 | 不持有 Graph、不渲染、不执行 |

#### 1.8.5 `ReadOnlyView`（渲染视图）

**一句话定义**：`ReadOnlyView` 是每帧临时构造的一个轻量结构，内部只存工作流数据、选中状态、运行快照和配置的引用，向渲染模块暴露只读访问器，渲染代码通过访问器读取数据而不直接碰底层对象；它不拷贝数据、不修改任何东西、不跨帧存活。

| 维度 | 内容 |
|---|---|
| 构建者 | **主循环统一构建一次**，传给所有渲染模块 |
| 字段 | 三个核心 + 配置（工作流数据 / 选中 / 运行快照 / 配置） |
| 快照为空 | **空对象**（`RunSnapshot` 内部 `finished = false`） |
| 渲染签名 | `render(const ReadOnlyView& view)` |
| 工作流数据形态 | **接口收敛（零拷贝）**：访问器返回 graph 内部数组的 const 引用 |
| 使用者 | **所有渲染模块**（画布、参数面板、输出面板、状态栏） |
| 限制 | 不拷贝、不修改、不跨帧、不直接暴露 Graph |

#### 1.8.6 `AutosaveService`（应用级）

**一句话定义**：`AutosaveService` 订阅编辑事件标记「有未保存修改」，按节流间隔检查标记，若有修改则把当前工作流写到临时文件；它不持有工作流数据、不做 UI 提示、不替代主动保存。

| 维度 | 内容 |
|---|---|
| 标准写法 | 订阅编辑事件 → 标记 dirty → 定时器检查 → 写临时文件 → 清除 dirty |
| 退出钩子 | `flush()` 供退出时强制写 |
| 数据来源 | 写时通过 `WorkflowModel::graph()` 读 |
| 限制 | 不持有 Graph、不做 UI、不替代主动保存 |

#### 1.8.7 `Diagnostics`（应用级）

**一句话定义**：`Diagnostics` 在后台线程周期性采集内存、帧率、异常计数等运行时指标，写入一个线程安全的环形缓冲，供 Console 面板读取展示；它不参与业务逻辑、不修改任何业务状态、不阻塞主线程。

| 维度 | 内容 |
|---|---|
| 标准写法 | 后台线程周期采集 → 环形缓冲 → `snapshot()` 加锁读取 |
| 线程安全 | ✅（生产者后台线程，消费者 UI 线程） |
| 限制 | 不参与业务、不修改状态、不阻塞主线程 |

#### 1.8.8 `Clipboard`（应用级）

**一句话定义**：`Clipboard` 持有用户复制的一组节点和连线（深拷贝），提供复制和粘贴接口；粘贴时把数据注入目标工作流并返回新建的节点 id 列表；它不持有工作流数据、不渲染、不执行。

| 维度 | 内容 |
|---|---|
| 标准写法 | 持有复制的节点/边深拷贝 + `copy(selection, graph)` / `paste(graph)` → 新节点 id 列表 + `has_content()` |
| 生命周期 | 应用级（跨工作流存活） |
| 限制 | 不持有 Graph、不渲染、不执行 |

#### 1.8.9 `WorkflowFile`（应用级）

**一句话定义**：`WorkflowFile` 负责把当前工作流的节点和连线数据写成 JSON 文件、从 JSON 文件读回来并替换当前工作流、维护最近打开的文件列表，以及在每次运行结束时把运行结果写进一个按时间命名的归档目录；它不持有工作流数据、不执行节点、不渲染界面、不负责定时自动保存。

| 维度 | 内容 |
|---|---|
| 持有 `WorkflowModel` | ✅（保存读 `graph()`，加载调 `load()`） |
| 运行归档触发 | **订阅「运行结束」事件**，收到后写归档 |
| 最近文件 | **独立 `RecentFiles` 对象**，`WorkflowFile` 持有它 |
| 失败处理 | **返回错误码 + 错误消息**，不碰 UI |
| 是否负责新建 | **不负责**（新建 = `model.load(空图)`） |
| 归档目录 | **沿用现有** `~/.brain-ai/outputs/<时间>-<工作流名>/` |
| 限制 | 不持有工作流数据、不执行、不渲染、不做定时自动保存 |

#### 1.8.10 `AppContext`（应用级聚合）

**一句话定义**：`AppContext` 在应用启动时按依赖顺序创建并持有所有长期存在的对象（配置、剪贴板、诊断、事件总线、工作流数据、选中、运行会话、文件操作、自动保存），在应用退出时按相反顺序销毁它们；它只在启动阶段把这些对象的引用交给各个界面模块，之后界面模块直接使用自己拿到的引用，不再回头向 `AppContext` 取任何东西；它本身不执行任何业务逻辑、不渲染、不处理事件、不持有界面对象。

| 维度 | 内容 |
|---|---|
| 形态 | **容器**（只在启动时创建对象、分发引用，之后无人访问） |
| 访问器 | **暴露 const 访问器，仅用于启动阶段注入**（运行时靠约定 + `rg` 验证） |
| 持有清单 | 完整（见 §1.5 生命周期分层） |
| 初始化顺序 | **构造函数内按固定顺序 `make_unique`** |
| 退出顺序 | **析构函数内按相反顺序显式处理**（`AutosaveService::flush()` → `RunSession` 停止 → 其余） |
| 是否持有 UI | **不持有**（UI 由 `AppFrame` 或 `app/` 自己管） |
| 限制 | 不执行逻辑、不渲染、不处理事件、不持有 UI、运行时禁止访问 |

#### 1.8.11 对象依赖图（初始化顺序）

```
1. EventBus              （无依赖）
2. Config                （无依赖）
3. Clipboard             （无依赖）
4. Diagnostics           （无依赖）
5. WorkflowModel         （依赖 EventBus）
6. SelectionState        （无依赖）
7. RunSession            （依赖 EventBus）
8. AutosaveService       （依赖 EventBus + WorkflowModel）
9. WorkflowFile          （依赖 EventBus + WorkflowModel，内含 RecentFiles）

退出顺序（相反）：
9. WorkflowFile → 8. AutosaveService(flush) → 7. RunSession(stop)
→ 6. SelectionState → 5. WorkflowModel → 4. Diagnostics → 3. Clipboard
→ 2. Config → 1. EventBus
```

#### 1.8.12 启动阶段「注入」示例

```cpp
// app/ 启动代码（唯一允许访问 AppContext 的地方）
AppContext ctx;

// 构造界面模块，直接传引用
NodeCanvas canvas(ctx.model(), ctx.selection(), ctx.bus());
PropertyPanel props(ctx.model(), ctx.selection(), ctx.bus());
Toolbar toolbar(ctx.model(), ctx.run_session(), ctx.file(), ctx.bus());
OutputPanel output(ctx.bus());
StatusBar status(ctx.bus());
SettingsPanel settings(ctx.config());

// 之后所有模块直接使用自己拿到的引用；不再访问 ctx
```

#### 1.8.13 `rg` 断言（§4 验收判据补充）

| 断言 | 命令 | 期望 |
|---|---|---|
| `AppContext` 只出现在启动代码 | `rg "AppContext" source/src/` | 结果只在 `app/` 下 |
| 运行时无 `ctx.` 访问 | `rg "ctx\." source/src/ui/` | 0 |
| 渲染函数只读 `ReadOnlyView` | `rg "render\(const ReadOnlyView"` | 覆盖所有渲染模块 |
| 无 `editor()` 引用 | `rg "editor\(\)" source/src/` | 0 |

---

## §2 问题与解法对应

### 2.1 六个结构问题

| 问题 | 现状 | 目标 | 解法 |
|---|---|---|---|
| 分层纪律 | engine 不依赖 ui（方向对，但未固定） | 编译期规则 + CI 扫描 | 规则 1 + ARCH-A4 |
| 单例总线 | `editor()` 上帝对象 + 12 个 `g_ce_*` | 状态对象化 + 显式作用域 | §1.5 生命周期分层 + §1.8 对象定义 |
| 双画布双轨 | 绘制层共享，交互层两套 | `CanvasInteractor` 参数化复用 | ARCH-E5 |
| 打包-校验不同源 | 两处约束不一致 | 规格生成下沉 engine + 生成后自检 | ARCH-E6 |
| 三入口行为漂移 | 菜单/工具栏/快捷键各自接线 | `dispatchCommand` 单一实现 | ARCH-R4 |
| 上帝对象 | `EditorState` 1181 行 + `app.cpp` 1371 行 | 拆 6 个对象 + `AppFrame` | ARCH-R1~R3 / E1~E4 / A1~A3 |

### 2.2 两个高频问题的解法

| 问题 | 表现 | 解法 |
|---|---|---|
| **甲：每帧跨对象访问** | 渲染时读 Graph、选中、快照，跨 3~4 个对象 | **ReadOnlyView**（每帧构建，只存引用，零拷贝） |
| **乙：一次操作触发多次同步** | 改参数 → Undo + autosave + UI 刷新 | **EditEvent**（一次操作 → 一个事件 → 消费者各自处理） |

---

## §3 迁移路线（增量式，每批可编译可自检）

> 批次间相互独立、可单独落地；每批结束跑回归基线（FREEZE §4）。
> **核心顺序：先运行后编辑**（RunEvent 已有基础，迁移成本低；验证事件系统可行后，再迁移编辑侧）。

### 阶段 1：运行侧事件化（ARCH-R）

| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-R1** | `RunSession` 对象化：Executor + 快照从 `editor()` 拆出 | `RunSession` 独立 | 构建 + `--exec-selftest` 713/0 |
| **ARCH-R2** | ~~RunEvent 订阅化：输出面板/状态栏改为订阅 `RunEvent`~~ ✅ **已落地（2026-10-22）** | UI 不再主动 `pumpEvents()`（`rg pumpRunEvents` = 0） | 构建 0/0 + `--exec-selftest` 713/0 + `--run-selftest`（新增订阅断言）+ check_links 1768/0 |
| **ARCH-R3** | `ReadOnlyView` 落地：渲染只读 `ReadOnlyView` | 渲染与状态解耦 | 构建 + 人工确认渲染正常 |
| **ARCH-R4** | 命令路由层：`ui/commands.h/.cpp`（AppCommand + 快捷键表 + dispatchCommand） | 三入口收敛 + 修 Ctrl+S | 构建 + 人工确认 Ctrl+S |

### 阶段 2：编辑侧事件化（ARCH-E）

| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-E1** | `EventBus` + `EditEvent` 落地：新增事件总线 + 8 种事件类型 | 事件系统就绪 | 构建 + 单元断言 |
| **ARCH-E2** | Undo/autosave 订阅化：改为订阅 `EditEvent` | 同步次数降低 | 构建 + `--graph-selftest` 151/0 |
| **ARCH-E3** | `WorkflowModel` 拆分：Graph + Undo 从 `editor()` 拆出 | 文档级独立 | 构建 + `--graph-selftest` |
| **ARCH-E4** | `SelectionState` 拆分 | 选中独立 | 构建 + `--graph-selftest` |
| **ARCH-E5** | `CanvasInteractor` 落地：双画布交互收敛为一套 | 双画布单套实现 | 构建 + `ce_repro` 0 + `check_all` |
| **ARCH-E6** | 打包下沉：`buildCompositeSpecFromSelection` 进 engine + 生成后严格校验 | 打包-校验同源 + 修暴露节点保存 | 构建 + `composite_selftest` + `check_all` |

### 阶段 3：应用级收口（ARCH-A）

| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-A1** | `AppContext` 收口：Config/Clipboard/Diagnostics/RecentFiles/AutosaveService/WorkflowFile | 应用级统一 | 构建 + `check_all` |
| **ARCH-A2** | `AutosaveService` 独立 | autosave 解耦 | 构建 + `check_all` |
| **ARCH-A3** | `editor()` 退场：只保留 `AppContext` 聚合 | 上帝对象消失 | 构建 + 全量自检 |
| **ARCH-A4** | 依赖单向 CI 断言：`tools/check_includes.ps1`（禁止 engine/nodes/ai/web/utils include ui/app），接入 `check_all` | 规则 1 工具化 | `check_all` 新项 PASS |
| **ARCH-A5** | 全局变量扫描：`tools/check_globals.ps1`（跨 TU extern + 文件级 static 可变状态 = 0），接入 `check_all` | 规则 6 工具化 | `check_all` 新项 PASS |

### 阶段 4：收尾（ARCH-Z）

| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-Z1** | 超长文件瘦身：`app.cpp` 主循环 → `AppFrame`；`node_canvas.cpp` 交互段移入 interactor 后瘦身 | 单文件 <800 行 | 构建 + `check_all` |
| **ARCH-Z2** | `CeSession` 对称拆分：`CeModel` / `CeFile` / `CeSelection` / `CeSpec` | 上级编辑器与主编辑器对称 | 构建 + 全局变量扫描 = 0 |

---

## §4 验收判据（重构完成 = 全部满足）

1. **行为等价**：全部自检断言数 ≥ 现状（`exec` 713 / `graph` 151 / `check_links` 1730），且 `--selftest` 0 失败；GUI 人工复核两个已知 bug 已修复（Ctrl+S 保存、暴露节点保存）。
2. **全局变量归零**：`check_globals.ps1` 扫描 `ui/` 跨 TU extern + 文件级 static 可变状态 = 0（`TextureCache` 作为**类成员**自然不计入，无例外条款）。
3. **双画布单套交互**：`ce_draw.cpp` 与 `node_canvas.cpp` 中不存在重复的"窗口判定 / Delete / 选中同步"实现（`rg` 验证关键谓词只出现在 `interactor.cpp`）。
4. **打包-校验同源**：`ce_pack.cpp` 不再直接写 `spec.internal_graph` / `spec.inputs` / `spec.exposed_params`（`rg` 验证为 0 次）。
5. **依赖单向**：`check_includes.ps1` 通过（engine/nodes/ai/web/utils 不含 ui/app 头）。
6. **`editor()` 退场**：全库 `editor()` 引用为 0（`rg` 验证），只保留 `AppContext`。
7. **ReadOnlyView 落地**：渲染路径只读 `ReadOnlyView`（`rg` 验证渲染函数不直接持有 `WorkflowModel&` / `SelectionState&` / `RunSession&` 的裸引用）。
8. **`AppContext` 运行时不可访问**：`rg "ctx\." source/src/ui/` 为 0；`rg "AppContext" source/src/` 只在 `app/` 下。

---

## §5 风险与对策

| 风险 | 对策 |
|---|---|
| 重构破坏现有行为（自检断言下降） | 每批回归基线"只升不降"；行为等价优先于结构美 |
| 拆分 `EditorState` 导致大范围调用点修改 | 按 ARCH-R → ARCH-E → ARCH-A 分批；每批只改一个对象 |
| `ce_draw` 与 `node_canvas` 交互差异比预想大 | `CanvasInteractor` 用"回调+参数"保留差异点；ARCH-E5 前先做差异盘点 |
| 全局变量扫描误报（constexpr/局部 static 非状态） | 扫描规则只匹配"可变静态存储期变量"，白名单登记合理项 |
| `ReadOnlyView` 每帧构建有开销 | 只存引用，O(1)；实测确认无性能回退 |
| 事件系统调试复杂度 | 事件加序号 + 日志；单线程 FIFO 保证顺序 |
| `AppContext` 重新长成上帝对象 | 运行时 `rg` 断言（`ctx.` 在 `ui/` 为 0）+ 代码评审 |

---

## §6 与冻结的关系

| 项 | 状态 |
|---|---|
| 功能冻结 | 🟢 生效（`FREEZE.md`） |
| 模块冻结 | 🟢 生效（新文件仅限 `ARCH-*` 批产出） |
| 接口冻结 | 🟢 生效（对外符号保持或显式登记变更） |
| 重构主线 | ✅ 本文（`ARCHITECTURE.md` v1.1） |

**冻结解除条件**：架构定稿 + 重构完成 + 回归基线全绿。

---

## §7 决策记录 `ARCH-D*`

| 编号 | 议题 | 结论 | 依据 |
|---|---|---|---|
| `ARCH-D1` | 重构方式 | **增量式**（每批可编译，保留全部自检） | 用户拍板（2026-10-22） |
| `ARCH-D2` | 拆分粒度 | **6 个对象**（WorkflowModel / RunSession / WorkflowFile / SelectionState / Clipboard / Diagnostics） | 用户拍板（2026-10-22） |
| `ARCH-D3` | `CeSession` 形态 | **对称拆分**（CeModel / CeFile / CeSelection / CeSpec） | 用户拍板（2026-10-22） |
| `ARCH-D4` | 拆分顺序 | **先运行后编辑**（ARCH-R → ARCH-E → ARCH-A） | 用户拍板（2026-10-22） |
| `ARCH-D5` | 调用方改造 | **每个 UI 模块直接持有需要的对象**，不持有 `editor()` | 用户拍板（2026-10-22） |
| `ARCH-D6` | 防止重新长回去 | **工具 + 流程双保险**（check_globals + 评审） | 用户拍板（2026-10-22） |
| `ARCH-D7` | 事件系统形态 | **形态 1：通知**（消费者各自处理） | 用户拍板（2026-10-22） |
| `ARCH-D8` | 渲染读取 | **直接访问核心状态**（通过 ReadOnlyView） | 用户拍板（2026-10-22） |
| `ARCH-D9` | 核心状态 | **保留 WorkflowModel**（Graph 是核心，事件是外围） | 用户拍板（2026-10-22） |
| `ARCH-D10` | RunEvent | **复用现有队列**，扩展为订阅模式 | 用户拍板（2026-10-22） |
| `ARCH-D11` | ReadOnlyView | **接受**（每帧构建，只存引用，零拷贝） | 用户拍板（2026-10-22） |
| `ARCH-D12` | EditEvent 类型 | **8 种**（NodeAdded / NodeRemoved / NodeMoved / EdgeAdded / EdgeRemoved / ParamChanged / TitleChanged / SelectionChanged） | 用户拍板（2026-10-22） |
| `ARCH-D13` | RunEvent 订阅化 | **接受**（UI 改为订阅，不再主动 pump） | 用户拍板（2026-10-22） |
| `ARCH-D14` | `editor()` 最终形态 | **完全退场**，只保留 `AppContext` 聚合 | 用户拍板（2026-10-22） |
| `ARCH-D15` | 验收判据例外 | **删除** `TextureCache` 例外（类成员自然不计入） | 用户拍板（2026-10-22） |
| `ARCH-D16` | ARCH-06 依赖图 | **拆分前补依赖图**（先出图，确认无环再拆） | 用户拍板（2026-10-22） |
| `ARCH-D17` | ARCH-02 差异盘点 | **收敛前补差异盘点**（列出所有交互差异） | 用户拍板（2026-10-22） |
| `ARCH-D18` | 行为等价量化 | **拆完后补对象交互断言** | 用户拍板（2026-10-22） |
| `ARCH-D19` | 回归基线 | **只升不降** | 用户拍板（2026-10-22） |

| `ARCH-D20` | `RunSession` 持有执行器 | **持有**（创建、销毁执行器） | 用户拍板（2026-10-22） |
| `ARCH-D21` | `RunSession` 常驻 | **常驻**（启动创建，退出销毁；每次运行重置） | 用户拍板（2026-10-22） |
| `ARCH-D22` | 运行中修改工作流 | **`RunSession` 自己感知**（订阅编辑事件） | 用户拍板（2026-10-22） |
| `ARCH-D23` | 运行归档归属 | **`WorkflowFile` 订阅「运行结束」事件后写** | 用户拍板（2026-10-22） |
| `ARCH-D24` | 快照全量/增量 | **全量**（每帧从执行器重新读一遍） | 用户拍板（2026-10-22） |
| `ARCH-D25` | `RunSession::start()` 数据形态 | **接收 `const Graph&`，内部拷贝一份** | 用户拍板（2026-10-22） |
| `ARCH-D26` | 撤销栈归属 | **独立 `UndoStack` 类，由 `WorkflowModel` 持有** | 用户拍板（2026-10-22） |
| `ARCH-D27` | 运行前校验归属 | **`RunSession::start()` 时校验** | 用户拍板（2026-10-22） |
| `ARCH-D28` | 选中状态归属 | **`SelectionState` 独立** | 用户拍板（2026-10-22） |
| `ARCH-D29` | 加载/关闭工作流 | **`WorkflowModel::load(graph)`** 替换数据 + 清空撤销栈 | 用户拍板（2026-10-22） |
| `ARCH-D30` | ReadOnlyView 构建者 | **主循环统一构建一次** | 用户拍板（2026-10-22） |
| `ARCH-D31` | ReadOnlyView 字段 | **三个核心 + 配置** | 用户拍板（2026-10-22） |
| `ARCH-D32` | 快照为空表示 | **空对象**（`finished = false`） | 用户拍板（2026-10-22） |
| `ARCH-D33` | 渲染函数签名 | **`render(const ReadOnlyView&)`** | 用户拍板（2026-10-22） |
| `ARCH-D34` | 工作流数据形态 | **接口收敛（零拷贝）** | 用户拍板（2026-10-22） |
| `ARCH-D35` | ReadOnlyView 使用者 | **所有渲染模块** | 用户拍板（2026-10-22） |
| `ARCH-D36` | `WorkflowFile` 持有 `WorkflowModel` | **持有** | 用户拍板（2026-10-22） |
| `ARCH-D37` | 最近文件 | **独立 `RecentFiles` 对象，`WorkflowFile` 持有** | 用户拍板（2026-10-22） |
| `ARCH-D38` | 失败处理 | **返回错误码 + 错误消息** | 用户拍板（2026-10-22） |
| `ARCH-D39` | 归档目录 | **沿用现有** | 用户拍板（2026-10-22） |
| `ARCH-D40` | `AppContext` 形态 | **容器**（启动时创建 + 分发，之后无人访问） | 用户拍板（2026-10-22） |
| `ARCH-D41` | `AppContext` 访问器 | **暴露 const 访问器，仅启动阶段注入** | 用户拍板（2026-10-22） |
| `ARCH-D42` | `AppContext` 初始化顺序 | **构造函数内按固定顺序** | 用户拍板（2026-10-22） |
| `ARCH-D43` | `AppContext` 退出顺序 | **析构函数内按相反顺序显式处理** | 用户拍板（2026-10-22） |
| `ARCH-D44` | `AppContext` 是否持有 UI | **不持有** | 用户拍板（2026-10-22） |

---

## §8 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-10-21 | v0.1 | **建立本文（讨论稿）**：分层纪律 + 状态管理 + 双画布收敛 + 打包-校验同源 + 命令路由 + 职责边界 + 8 批迁移路线 + 5 个待拍板事项 |
| 2026-10-22 | v1.0 | **用户拍板全部决策** —— ① 增量式重构；② 6 个对象拆分；③ CeSession 对称拆分；④ 先运行后编辑；⑤ 调用方直接持有对象；⑥ 工具+流程双保险；⑦ 事件形态 1（通知）；⑧ 渲染直接读；⑨ 保留 WorkflowModel；⑩ 复用 RunEvent；⑪ ReadOnlyView；⑫ 8 种 EditEvent；⑬ RunEvent 订阅化；⑭ `editor()` 完全退场；⑮ 删除 TextureCache 例外；⑯ 补依赖图；⑰ 补差异盘点；⑱ 补对象交互断言；⑲ 回归基线只升不降。**迁移路线重组为 ARCH-R / E / A / Z 四阶段。** |
| 2026-10-22 | **v1.1（当前）** | **补充 §1.8 对象定义总表** —— ① `EventBus` / `RunSession` / `WorkflowModel` / `SelectionState` / `ReadOnlyView` / `AutosaveService` / `Diagnostics` / `Clipboard` / `WorkflowFile` / `AppContext` **10 个对象全部定稿**（一句话定义 + 维度表 + 限制 + 依赖图 + 注入示例 + `rg` 断言）；② 新增 `ARCH-D20`~`ARCH-D44`（25 条对象级决策）；③ §4 验收判据新增第 8 条（`AppContext` 运行时不可访问）；④ §5 风险表新增「`AppContext` 重新长成上帝对象」对策；⑤ §1.6 状态生命周期表 `RunSession` 改为「启动（常驻）」。 |

---

**文档结束 · v1.1 定稿。**
**下一步：按 ARCH-R3 实施（ReadOnlyView 落地；等用户启动）。**
**已完成：ARCH-R1（RunSession 对象化）· ARCH-R2（RunEvent 订阅化）—— 审计见 docs/audit/audit_arch_r1.md / audit_arch_r2.md。**











