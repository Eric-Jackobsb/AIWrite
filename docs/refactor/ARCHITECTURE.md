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

**规则 1（依赖单向）**：`engine/*`、`nodes/*`、`ai/*`、`web/*`、`utils/*` 的头文件**禁止 include** `ui/*` 与 `app/*`；CI 用 include 扫描断言。 ✅ **已工具化（2026-10-23 · ARCH-A4）**：断言 = [../../source/tools/check_includes.ps1](../../source/tools/check_includes.ps1)（R1 = 能力层不得依赖 `ui|app`；R2 = `ui` 不得依赖 `app`；跨层同名头 ⇒ 归属不明判失败），已接入 `check_all` 第 ⑥ 项。

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

> **对称视图（ARCH-Z2 · 2026-10-23）**：上级节点编辑器一侧的同角色结构 = `CeReadOnlyView`（`ui/ce_session.h`），
> 字段 = `CeModel` 的内部图（`nodes_for_render` / `edges_for_render` / `find_node` / `graph`）+ `const CeSelection&` +
> `const RunSnapshot&` + `const Config&` + 规格 id / 名称 / dirty；渲染签名 `drawCompositeEditor(name, open, const CeReadOnlyView&, editor)`，
> 每帧在 `app.cpp` 构建一次 —— §4 判据 7 两侧同时满足（[audit/audit_arch_z2.md](../audit/audit_arch_z2.md)）。

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

**规则 6（跨帧可变状态显式化）**：任何跨帧存活的可变状态必须是某个对象成员；文件级 `static` 与跨 TU `extern` 全局在重构结束后**归零**。（机器判据 = `source/tools/check_globals.ps1` · 已入 `check_all` 第 ⑦ 项 · 见 §4 判据 2）

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

> **过渡登记（A2 批）**：`AutosaveService` 完全独立 —— `EditorState` 不再持有任何 autosave 方法（`tickAutosave` / `pauseAutosave` / `markAutosaveBaseline` / `autosaveDirty` / `flushAutosaveOnExit` / `restoreAutosave` 全部移除，调用方直连 autosave 服务）；「恢复快照」编排（`autosaveLoad` → `model().load` → 路径置空 → baseline）落在 app 层恢复弹窗（唯一调用方）；`flush()` 退出钩子由 app.cpp 退出路径直连。**A3 已落地（2026-10-23）**：`AutosaveService` 依 §1.8.11 第 8 位由 `AppContext` 构造，UI 侧经 `EditorController::autosave()` 直接使用。

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

> **过渡登记（A1 批）**：`WorkflowFile` 已拆为独立对象（`ui/workflow_file.h/.cpp`），由 `AppContext.workflow_file` 持有；归口数据 `current_workflow_path` / `restored_from_autosave_path` / `last_archive_dir`（原 `EditorState` 公有成员）。**运行归档触发**：目标态为「订阅运行结束事件」，A1 批仍由 `EditorController::finishRunSession` 编排调用 `archive_run_outputs(executor)`（依赖 `RunSession`；订阅化留 ARCH-Z1 后）；`recent_workflows()` ≈ `engine::loadRecentFiles()`。**A3 已落地**：同签名薄封装 `recentWorkflows()` / `last_archive_dir()` 已移除，调用方直连 `EditorController::workflow_file()`。

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

> **过渡登记（A1 批 · 决策 ARCH-A1）**：因**构造顺序鸡生蛋**（AutosaveService / WorkflowFile 依赖 `WorkflowModel&`，而 WorkflowModel 在 A1 阶段仍由 `EditorState` 持有），A1 批的 `AppContext` 曾作为 **`EditorState` 成员**（`app_ctx_`）构造，经 `editor().app_ctx()` 访问。
> **A3 已落地（2026-10-23）**：`AppContext` 已回到目标形态 —— app/main 启动时按 §1.8.11 顺序组装，UI 模块只拿到各自需要的引用（`model()` / `selection()` / `autosave()` …），运行时无人再访问 `AppContext`（判据 8）；`app_ctx()` 访问器与 `editor()` 单例一并移除。

> **ARCH-A3 已落地（2026-10-23）**：`AppContext` 现为**独立容器**（启动时在 `app.cpp` / `cli_selftest` 按 §1.8.11 顺序组装），与原 `EditorState` 的编排逻辑分离为无所有权的 `EditorController`（§1.8.14）；全局单例 `editor()` 已删除。

#### 1.8.14 `EditorController`（UI 编排层 · ARCH-A3）

**一句话定义**：`EditorController` 在启动时接收 8 个长期对象的引用（`WorkflowModel` / `SelectionState` / `RunSession` / `Config` / `Clipboard` / `Diagnostics` / `AutosaveService` / `WorkflowFile`），把它们编排成界面能直接调用的一组编辑 / 运行 / 文件 / 状态栏操作；它**不拥有**任何对象、不存业务数据、不渲染、不订阅全局，退出时只注销自己注册的 `RunEvent` 订阅。

| 维度 | 内容 |
|---|---|
| 形态 | **无所有权编排层**（成员全部是引用；拷贝/赋值 `delete`） |
| 生命周期 | 与 `AppContext` 同寿（`AppContext::editor`，启动构造 / 退出析构） |
| 访问器 | `model()` / `selection()` / `run()` / `config()` / `clipboard()` / `diagnostics()` / `autosave()` / `workflow_file()` / `event_bus()` |
| 持有状态 | 仅 **UI 瞬时状态**：`status` · `last_error` · `request_focus_title` / `request_navigate_to_content` / `request_focus_selection` · `follow_output_` / `run_finished_` / `run_last_summary_` |
| 注入方式 | 启动时由 app/main 把引用传给各 UI 模块（构造参数 / `CommandContext.editor`）——**没有全局单例** |
| 限制 | 不拥有数据、不渲染、不落盘、不做校验、不替代 `WorkflowModel` / `RunSession` 的职责 |
| 后续迁移 | `status` / `last_error` / `request_*` 归 ARCH-Z1（`AppFrame` 落地后分别下沉到状态栏 / 错误条 / 画布）；~~`CeSession` 单例归 ARCH-Z2~~ ✅ **已落地（2026-10-23 · §1.8.15）** |

#### 1.8.15 ce 五对象 + `CeReadOnlyView`（上级节点编辑器 · ARCH-Z2）

**一句话定义**：上级节点编辑器（ce）按主编辑器一侧的既有对象对称拆成 5 个 —— `CeModel`（规格工作副本 + 撤销栈 + dirty）· `CeFile`（打开 / 保存 / 热重载）· `CeSelection`（内部图选中）· `CeSpec`（规格标记 + 显示名缓冲）· `CeSession`（会话门面，唯一实例 `ceSession()`）；渲染侧新增 `CeReadOnlyView`（对称 `ReadOnlyView`），绘制函数只经视图读取。

| 对象 | 对称对象 | 持有 | 不负责 |
|---|---|---|---|
| `CeModel` | `WorkflowModel` | `CompositeSpec` 工作副本（`spec()` / `graph()`）+ 撤销 / 重做栈（**整份快照 · 深 30**）+ dirty | 不落盘、不做标记编辑、不渲染 |
| `CeSelection` | `SelectionState` | 内部图选中 id（局部单选：`single()`）+ `sync()` 变化检测 | 不持有图、不渲染 |
| `CeSpec` | —（ce 独有域） | 对外输入 / 输出（唯一）/ 暴露参数三处标记 + 显示名输入缓冲（原 `ce_name_buffers()` 函数内 static） | 不压快照、不置脏（调用方先 `snapshot()`） |
| `CeFile` | `WorkflowFile` | 打开（注册表 → 工作副本）· 保存（校验 + 注册表 + 落盘 + 主画布同 id 热重载） | 不持会话开关、不渲染 |
| `CeSession` | `EditorController` + `RunSession`（会话部分） | `active` / `ask_close` / `error` / 名称·描述缓冲 / 试运行执行器与读模型 + 上面 4 个对象 | 不渲染、不直接读写盘 |

| 维度 | 内容 |
|---|---|
| 单例形态 | `ceSession()`（`ce_session.cpp` 唯一函数内 static 承载对象）· 原 `compositeEditorState()` 已删除 |
| 撤销粒度 | **整份 `CompositeSpec`**（含对外端口 / 暴露参数）· `kCeUndoDepth = 30`；主画布 `engine::UndoStack` 绑 `Graph` 深 50，机制相同、深度不同 |
| 批量删除语义 | `ceRemoveNode` **不压快照**，调用方按批次压一次（右键菜单 / Delete 键）—— 一次删除 = 一次撤销 |
| 渲染只读 | `drawCompositeEditor(name, open, const CeReadOnlyView&, EditorController&)`；`nodes_for_render()` / `edges_for_render()` / `find_node` / `graph()` 均为 const 引用零拷贝 |
| 写路径 | 顶栏按钮 / 标记 / 参数 / 连线 / 删除 / 试运行 start·stop 仍走 `ceSession()` 及转发函数（`compositeEditorOpen` / `Save` / `RequestClose` / `ForceClose` 对外签名不变） |
| 规则 6 | 全部跨帧状态都是对象成员 ⇒ 函数内 static 棘轮 **51 → 50**（`ce_name_buffers()` 消失） |

---



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

> **A3 已落地（2026-10-23）**：构造顺序回到上表目标顺序 —— `AppContext` 独立构造（`config → clipboard → diagnostics → model → selection → run → autosave → workflow_file`），编排层 `EditorController` 作为第 10 项（只持引用）；`app.cpp` / `cli_selftest` 是唯一组装点，UI 模块只接收引用。

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
| ce 渲染只读 `CeReadOnlyView` | `rg "const CeReadOnlyView&"` | 命中 `composite_editor.h` / `ce_draw.cpp` / `app.cpp` |
| 无 `editor()` 引用 | `rg "editor\(\)" source/src/` | 0 |

> **A3 实测（2026-10-23）**：`rg "\beditor\(\)" source/` = **0**（含注释）；`rg "AppContext" source/` 的**代码引用（非注释行）** 仅命中 `ui/include/app_context.h` · `ui/src/app_context.cpp` · `ui/src/app.cpp` · `app/src/cli_selftest.cpp`（启动组装点）—— 其余命中均为注释中的架构引用。

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
| **ARCH-R3** | ~~`ReadOnlyView` 落地：渲染只读 `ReadOnlyView`~~ ✅ **已落地（2026-10-22）** | 渲染与状态解耦（`render(const ReadOnlyView&)` 覆盖全部渲染模块） | 构建 0/0 + `--run-selftest`（新增 ReadOnlyView 零拷贝/同源断言）+ exec 713/0 + graph 151/0 + check_links 1768/0 |
| **ARCH-R4** | ~~命令路由层：`ui/commands.h/.cpp`（AppCommand + 快捷键表 + dispatchCommand）~~ ✅ **已落地（2026-10-22）** | 三入口收敛（菜单/工具栏/快捷键统一 `dispatchCommand` 单一实现）+ 修 Ctrl+S（主窗口 Ctrl+S = 保存工作流；ce 场景归保存上级节点） | 构建 0/0 + `--run-selftest`（新增命令路由断言）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links 1774/0 + check_all 19/19 + **人工确认 Ctrl+S**（GUI 真机） |

### 阶段 2：编辑侧事件化（ARCH-E）

| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-E1** | ~~`EventBus` + `EditEvent` 落地：新增事件总线 + 8 种事件类型~~ ✅ **已落地（2026-10-22）** | 事件系统就绪（8 种类型各有真实发布点 + 单元断言） | 构建 0/0 + `--run-selftest`（新增编辑事件断言）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links 1786/0 + check_all 19/19 |
| **ARCH-E2** | ~~Undo/autosave 订阅化：改为订阅 `EditEvent`~~ ✅ **已落地（2026-10-22）** | 同步次数降低（autosave 不再每帧算 content signature；undo 快照经 before-edit 通道压栈） | 构建 0/0 + `--run-selftest`（新增「编辑订阅」断言）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1792**/0 |
| **ARCH-E3** | ~~`WorkflowModel` 拆分：Graph + Undo + EventBus 从 `editor()` 拆出~~ ✅ **已落地（2026-10-22）** | 文档级独立（`editor().model()` 访问；`state.graph` → `state.model().graph` 全库替换） | 构建 0/0 + `--run-selftest`（编辑订阅断言照常）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1800**/0 |
| **ARCH-E4** | ~~`SelectionState` 拆分：选中独立~~ ✅ **已落地（2026-10-22）** | 选中独立（`editor().selection()` 访问；`selected_nodes` 等公有成员全库移除） | 构建 0/0 + `--run-selftest`（ReadOnlyView 选中同源照常）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1800**/0 |
| **ARCH-E5** | ~~`CanvasInteractor` 落地：双画布交互收敛为一套~~ ✅ **已落地（2026-10-22）** | 双画布单套实现（关键交互谓词只出现在 interactor.cpp） | 构建 0/0 + `ce_repro` 全帧 OK + check_links **1809**/0 + check_all **19/19** |
| **ARCH-E6** | ~~打包下沉：`buildCompositeSpecFromSelection` 进 engine + 生成后严格校验~~ ✅ **已落地（2026-10-23）** | 打包-校验同源 + 修暴露节点保存（ce_pack.cpp 不再直接写 `spec.internal_graph` / `spec.inputs` / `spec.exposed_params`） | 构建 0/0 + composite_selftest **112**/0 + exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1817**/0 + check_all **19/19** |

### 阶段 3：应用级收口（ARCH-A）

| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-A1** | ~~`AppContext` 收口：Config/Clipboard/Diagnostics/RecentFiles/AutosaveService/WorkflowFile~~ ✅ **已落地（2026-10-23）** | 应用级统一（六服务拆为独立对象 + `AppContext` 容器；`EditorState` 保留薄封装 + `app_ctx()` 访问器 · 过渡形态 = EditorState 成员，A3 迁 app/main 注入） | 构建 0/0 + `--run-selftest` PASS（零断言增减）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1823**/0 + check_all **19/19** |
| **ARCH-A2** | ~~`AutosaveService` 独立（A1 已拆出对象；本批 = EditorState autosave 方法随 `editor()` 一并移除）~~ ✅ **已落地（2026-10-23）** | autosave 解耦（`EditorState` 六方法移除：tick / pause / mark_baseline / dirty / flush / restore → 调用方直连 `app_ctx().autosave`；restore 编排落 app 层恢复弹窗） | 构建 0/0 + `--run-selftest` PASS（零断言增减）+ exec 713/0 + graph 151/0 + check_links **1839**/0 + check_all **19/19** |
| **ARCH-A3** | ~~`editor()` 退场：只保留 `AppContext` 聚合~~ ✅ **已落地（2026-10-23）** | 上帝对象消失（`EditorState` → 无所有权 `EditorController`；`AppContext` 启动组装 + 只注入一次） | 构建 0/0 + `--run-selftest` PASS（零断言增减）+ exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1845**/0 + check_all **19/19** + `rg "editor\(\)"` = 0 |
| **ARCH-A4** | ~~依赖单向 CI 断言：`tools/check_includes.ps1`（禁止 engine/nodes/ai/web/utils include ui/app），接入 `check_all`~~ ✅ **已落地（2026-10-23）** | 规则 1 工具化（`check_includes.ps1`：R1 能力层 ⊅ `ui|app` · R2 `ui` ⊅ `app` · 跨层同名头 = 归属不明 ⇒ 失败；裸文件名 → 所属层反查，覆盖 201 文件 / 656 include 行；`check_all` **19 → 20 项**，新项排第 ⑥） | `check_all` **20/20** + 新项 PASS（201 文件 · 违规 0）· 注入测试 3 种全部变红 · 构建 0/0 + `--run-selftest` PASS + exec 713/0 + graph 151/0 + check_links **1864**/0 |
| **ARCH-A5** | ~~全局变量扫描：`tools/check_globals.ps1`（跨 TU extern + 文件级 static 可变状态 = 0），接入 `check_all`~~ ✅ **已落地（2026-10-23）** | 规则 6 工具化（`check_globals.ps1`：R1 头文件 `extern` 变量 · R2 文件作用域 static（含匿名 namespace）· R3 外部链接全局；类成员 / `const` 常量表 / 函数内 static 不计入）· **跨 TU extern 清零**（`ce_internal.h` 12 条 + 同域 2 条收进 `CeUiState` 对象，`ceUiState()` 单一定义点）· 存量 **113 → 87**（ui 60 / web 15 / utils 12）登记**棘轮基线** `tools/check_globals_baseline.txt`（新增即失败 · 只减不增）· `check_all` **20 → 21 项**，新项排第 ⑦ | `check_all` **21/21** + 新项 PASS（201 文件 · 违规 87 · **R1 0** · new 0）· 注入测试 3 种全部变红 · 构建 0/0 + `--run-selftest` PASS + exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_links **1889**/0 |
| **ARCH-A6** | ~~ui 层 60 条文件级 static 可变状态**逐文件对象化**~~ ✅ **已落地（2026-10-23）** | **60 条 → 0**（`settings_panel` 19 → `SettingsPanelState` · `texture_cache` 12 → **`class TextureCache`** · `node_canvas` 11 → `NodeCanvasState` · `ce_draw` 7 → `CompositeDialogsState` · `node_library` 6 → `NodeLibraryState` · `console_panel` 3 → `ConsolePanelState` · `output_panel` 2 → `OutputPanelState`）；7 个状态对象由 `EditorController` **持有并注入**（菜单 / 命令 / 主循环三入口同源）· 顺带修三处隐式单实例（`CanvasContext::textures` 指针化 · `LevelFilter` 出头文件 · 三个入口补上下文参数）· 基线 **87 → 27**（ui 层 60 → **0**，余 27 条全是 **web/utils 的文件级 static**，属 A7） | `check_all` **21/21** + check_globals PASS（201 文件 · 27 违规 · **fixable 0** · new 0）· 构建 **0/0**（0 warning）+ `--run-selftest` PASS + exec 713/0 + graph 151/0 + ce_repro 全帧 OK + settings/site/official/autosave 断言全 PASS + check_includes 666/0 + check_links 1897/0 |
| **ARCH-A7** | ~~web/utils 27 条文件级 static 可变状态**逐文件对象化** + 删基线切严格零容忍~~ ✅ **已落地（2026-10-23）** | **27 条 → 0 · 文件级存量 87 → 0**（`webview_host` 8 → `WindowRegistry` + `EnvCache` + `tlsWindow()` · `diagnostics` 7 → `ExceptionCounter` · `pipe_client` 4 → `IpcCounters` + `IpcThreadCounters`（`thread_local` 语义保留）· `pydoll_channel` 3 → `SessionState` · `config` 2 → `ConfigStore` · `log` 2 → `Loggers` · `credential` 1 → `AvailabilityCache`）；**基线文件删除** + `check_all` 第 ⑦ 项改**裸调用**（严格 0）+ 工具新增**函数内 static 棘轮** `-MaxFunctionStatics`（41 → 51 一次性交换 · 只许减 · 超 1 条即失败） | `check_all` **21/21** + check_globals **201 文件 / 违规 0（R1 0 / R2 0 / R3 0）/ fn_static 51·51** · 构建 **0/0** + `--run-selftest` PASS + exec 713/0 + graph 151/0 + ce_repro 全帧 OK + check_includes 666/0 + check_links 1910/0 · 三种注入反向验证（文件级 static → exit 1 · 棘轮超限 → exit 1 · 复原 → exit 0） |


| 批 | 内容 | 产出 | 验证 |
|---|---|---|---|
| **ARCH-Z1** | ~~超长文件瘦身：`app.cpp` 主循环 → `AppFrame`；`node_canvas.cpp` 交互段移入 interactor 后瘦身~~ ✅ **已落地（2026-10-23 · 13 组提交覆盖 14 个文件）** | `provider_spec` **1743 → 5 TU**（第 1 组）· `webview_host` **3514 → 8** · `dom_web_client` **2397 → 4 + 2 脚本头** · `pydoll_channel` **1282 → 3**（第 2‑4 组）· `app` **1454 → 4** + `app_detail.h`（第 5 组）· `node_canvas` **→ 2** · `settings_panel` **→ 3** · `canvas_view` **→ 2** · `property_panel` **→ 2** · `ce_draw` **→ 2**（第 6‑10 组）· `executor` **→ 2** · `node_registry` **→ 2** · `composite` **→ 3** · `local_nodes` **→ 4**（第 11‑13 组）· **去重专项 6 步**（`check_counter.h` / `workflow_io` / `resolve_retry_plan` / `provider_common` / `site_upload_common`） | 单文件 <800 行达成（生产模块现存最大 **766** = `engine/src/graph.cpp`；唯一 ≥800 = 工具 `app/src/cli_selftest.cpp` 1429，口径不计）· 公共 API 头一字未改 | 构建 **0/0** + `check_all` **21/21** + `--provider-selftest` **52/0** + `--upload-selftest` **72/0** + 夹具 44/17/15 + exec 713/0 + graph 151/0 + `check_globals` **254 文件 / 违规 0 / fn_static 50·50** + `check_includes` **254/913/0** + `check_links` **1974/0** + 重复率 **3.8 % → 2.98 %**（[audit/audit_arch_z1.md](../audit/audit_arch_z1.md) §8 收口表 / §10 去重 / §11 8/8 复审 · [audit/audit_arch_z1_web.md](../audit/audit_arch_z1_web.md)） |
| **ARCH-Z2** | ~~`CeSession` 对称拆分：`CeModel` / `CeFile` / `CeSelection` / `CeSpec`~~ ✅ **已落地（2026-10-23）** | 上级编辑器与主编辑器对称 + **ce 渲染 `CeReadOnlyView` 化（判据 7 ✅）** | 构建 0/0 + `--run-selftest` PASS + exec 713/0 + graph 151/0 + ce_repro 全帧 OK + `check_globals` **206 文件 / 违规 0 / fn_static 50·50**（棘轮 51 → 50）+ check_includes 679/0 + check_links 1933/0（[audit/audit_arch_z2.md](../audit/audit_arch_z2.md)） |

---

## §4 验收判据（重构完成 = 全部满足）

1. **行为等价**：全部自检断言数 ≥ 现状（`exec` 713 / `graph` 151 / `check_links` 1730），且 `--selftest` 0 失败；GUI 人工复核两个已知 bug 已修复（Ctrl+S 保存、暴露节点保存）。
2. **全局变量归零**：`check_globals.ps1` 扫描全库跨 TU extern + 文件级 static 可变状态 = 0（`TextureCache` 作为**类成员**自然不计入，无例外条款）。 ✅ **已满足（2026-10-23 · ARCH-A7）**：**跨 TU `extern` = 0 · 文件级 static 可变状态 = 0 · R3 = 0**（A5 清 `extern` 12 条 · A6 清 ui 60 条 · A7 清 web 15 + utils 12 条 ⇒ 存量 **113 → 0**）；`check_all` 第 ⑦ 项改**严格零容忍**（基线文件 `tools/check_globals_baseline.txt` **已删除**），并新增**函数内 static 棘轮**（`$script:FnStaticCap = 51` · 只许减 · 超 1 条即 exit 1）堵住「文件级归零、函数内继续长」的口子；实测 `checked files 201 / violations 0 / fn_static 51·51`，反向注入两种形态均变红（[audit/audit_arch_a7.md](../audit/audit_arch_a7.md) · [audit/audit_arch_a6.md](../audit/audit_arch_a6.md) · [audit/audit_arch_a5.md](../audit/audit_arch_a5.md)）。**ARCH-Z2 补记（2026-10-23）**：`ce_name_buffers()` 函数内 static 收进 `CeSpec::name_bufs_` ⇒ 棘轮 **51 → 50**（`$script:FnStaticCap = 50`，实测 `206 文件 / 违规 0 / fn_static 50·50` · [audit/audit_arch_z2.md](../audit/audit_arch_z2.md)）。
3. **双画布单套交互**：`ce_draw.cpp` 与 `node_canvas.cpp` 中不存在重复的"窗口判定 / Delete / 选中同步"实现（`rg` 验证关键谓词只出现在 `interactor.cpp`）。 ✅ **已满足（2026-10-22 · ARCH-E5）**：`GetSelected* / BeginCreate/EndCreate / BeginDelete/EndDelete / Query* / Accept* / Reject* / ClearSelection` 只出现在 `interactor.cpp`；窗口判定只出现在 `interactorWindowActiveMain/Composite`。
4. **打包-校验同源**：`ce_pack.cpp` 不再直接写 `spec.internal_graph` / `spec.inputs` / `spec.exposed_params`（`rg` 验证为 0 次）。 ✅ **已满足（2026-10-23 · ARCH-E6）**：打包逻辑下沉 `engine::buildCompositeSpecFromSelection`（composite.h/.cpp），ce_pack 仅调用 + 生成后严格校验（`validate_composite_spec` 通过才落盘）；生成阶段自动修正「暴露输入不得有内部入边」（variadic 端口内外并存场景）。
5. **依赖单向**：`check_includes.ps1` 通过（engine/nodes/ai/web/utils 不含 ui/app 头）。 ✅ **已满足（2026-10-23 · ARCH-A4）**：`check_includes.ps1` = `checked files: 201 / include lines: 656 / violations: 0`（能力层 130 文件 276 行 include 中归属 `ui|app` 者 0；`ui` 292 行中归属 `app` 者 0）；接入 `check_all` 第 ⑥ 项，三个注入测试均能变红（[audit/audit_arch_a4.md](../audit/audit_arch_a4.md)）。
6. **`editor()` 退场**：全库 `editor()` 引用为 0（`rg` 验证），只保留 `AppContext`。 ✅ **已满足（2026-10-23 · ARCH-A3）**：`rg "\beditor\(\)" source/` = 0；`AppContext` 只在 `ui/app_context.*` + 启动组装点（`app.cpp` / `cli_selftest.cpp`）出现。
7. **ReadOnlyView 落地**：渲染路径只读 `ReadOnlyView`（`rg` 验证渲染函数不直接持有 `WorkflowModel&` / `SelectionState&` / `RunSession&` 的裸引用）。 ✅ **已满足（2026-10-23 · ARCH-Z2）**：**主画布侧**（`drawNodeCanvas` / `drawPropertyPanel` / `drawOutputPanel` + `app.cpp` 4 个文本函数 = `const ReadOnlyView&`）**+ ce 侧**（`drawCompositeEditor(name, open, const CeReadOnlyView&, editor)` · 每帧在 `app.cpp` 构建一次 · `nodes_for_render` / `edges_for_render` / `find_node` / `graph` 均 const 引用零拷贝 · 写路径仍走 `ceSession()`）—— 两侧渲染函数从签名上就拿不到可变对象；`CompositeEditorState` 单例已拆成 5 对象（§1.8.15），`rg "compositeEditorState\(" source/` 代码行 = **0**；等价证据 = `ce_repro` **ALL FRAMES OK（mode 0）** + 自检断言零升降（[audit/audit_arch_z2.md](../audit/audit_arch_z2.md)）。
8. **`AppContext` 运行时不可访问**：`rg "ctx\." source/src/ui/` 为 0；`rg "AppContext" source/src/` 只在 `app/` 下。 ✅ **已满足（2026-10-23 · ARCH-A3）**：UI 面板代码 **0** 处 `AppContext` 代码引用（仅注释提及）；各模块只持 `EditorController&`（内部转发所需引用），启动组装点 = `app.cpp` / `cli_selftest.cpp`。

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
| 2026-10-22 | **v1.1** | **补充 §1.8 对象定义总表** —— ① `EventBus` / `RunSession` / `WorkflowModel` / `SelectionState` / `ReadOnlyView` / `AutosaveService` / `Diagnostics` / `Clipboard` / `WorkflowFile` / `AppContext` **10 个对象全部定稿**（一句话定义 + 维度表 + 限制 + 依赖图 + 注入示例 + `rg` 断言）；② 新增 `ARCH-D20`~`ARCH-D44`（25 条对象级决策）；③ §4 验收判据新增第 8 条（`AppContext` 运行时不可访问）；④ §5 风险表新增「`AppContext` 重新长成上帝对象」对策；⑤ §1.6 状态生命周期表 `RunSession` 改为「启动（常驻）」。 |
| 2026-10-22 | **v1.2** | **ARCH-R3 落地** —— ① 新增 `ReadOnlyView`（`readonly_view.h`）：每帧主循环统一构建一次（`ARCH-D30`）、字段 = 三个核心 + 配置（`ARCH-D31`）、快照空对象 = 未运行（`ARCH-D32`）、渲染签名 `render(const ReadOnlyView&)`（`ARCH-D33`）、零拷贝访问器 `nodes_for_render` / `edges_for_render`（`ARCH-D34`）、使用者 = 画布/参数面板/输出面板/状态栏/信息面板（`ARCH-D35`）；② 画布 / 输出面板 / 参数面板 / 状态栏 / 信息面板渲染读取统一走 view，交互写仍走 `editor()`（E3/E4 拆分后收敛）；③ `--run-selftest` 新增 ReadOnlyView 零拷贝/同源断言（`ARCH-D18`）。验证：构建 0/0 · run-selftest PASS · exec 713/0 · graph 151/0 · check_links 1768/0。 |
| 2026-10-22 | **v1.3** | **ARCH-R4 落地（命令路由层）** —— ① 新增 `ui/commands.h/.cpp`：`AppCommand` 命令全集 + `dispatchCommand` **单一实现** + `ShortcutBinding` **快捷键表** + `handleShortcut` 表驱动分派；② **三入口收敛**：菜单（文件/编辑/上级节点/帮助）/ 工具栏（撤销/重做/删除/新建/示例/打开/保存）/ 快捷键（主画布 Delete · ce Delete/Ctrl+S）统一经 `dispatchCommand` —— §2.1「三入口行为漂移」关闭；③ **修 Ctrl+S**：主窗口 Ctrl+S = 保存工作流（此前主窗口无保存快捷键），作用域隔离经 `compositeEditorWindowFocused()`（ce 窗口聚焦/悬停 → Ctrl+S 归 ce 保存上级节点）；ce 顶栏「保存」按钮收敛到 `dispatchCommand(CompositeSave)`（与 Ctrl+S 同一路径 = DEBT-B1 语义）；④ 范围边界：视图菜单（面板可见性 + config 持久化）与运行/停止（P1-b 预检弹窗）不入命令层（单入口 / 无三入口漂移，见审计）。验证：构建 0/0 · run-selftest PASS（新增命令路由断言）· exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links 1774/0 · check_all 19/19。 |
| 2026-10-22 | **v1.4（当前）** | **ARCH-E1 落地（阶段 2 编辑侧事件化 · EventBus + EditEvent）** —— ① 新增 `ui/event_bus.h/.cpp`：`EventBus`（`subscribe` / `unsubscribe` / `publish` 单线程 FIFO 通知总线，§1.8.1 形态 1 = ARCH-D7）+ `EditEvent` 8 种类型（ARCH-D12）+ 静态工厂；② `EditorState` 持有 EventBus（`editor().event_bus()`；E3 拆 WorkflowModel 后迁到文档级对象）；③ **发布点覆盖 8 种事件**（§2.2 问题乙「一次操作触发多次同步」的事件基础就绪）：`createNode`→NodeAdded · `deleteNode`→NodeRemoved · 画布拖拽（sync_positions）→NodeMoved · 画布连线（handle_create_link / pasteClipboard / createSampleWorkflow）→EdgeAdded · 连线删除（handle_delete_items / deleteSelected）→EdgeRemoved · 参数面板（edit.changed）→ParamChanged · 标题（edit.renamed）→TitleChanged · 选中同步（sync_selection 变化检测）→SelectionChanged；④ `--run-selftest` 新增「编辑事件」断言（机制：kind 分组 / FIFO 顺序 / 注销 / 计数 + 8 种类型完整性 + 编辑操作→事件映射 = ARCH-D18）。范围边界：ce（上级节点编辑器）独立模型不接入（E3/E5 处理）；批量/重置操作（undo / clear / open）细粒度事件语义留 E2 定义。验证：构建 0/0 · run-selftest PASS · exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links 1786/0 · check_all 19/19。 |
| 2026-10-22 | **v1.5** | **ARCH-E2 落地（阶段 2 编辑侧事件化 · Undo / autosave 订阅化）** —— ① `EventBus` 新增「**修改前**」通道（`subscribe_before_edit` / `publish_before_edit` / `unsubscribe_before_edit` · 与 `publish` 同构 FIFO）：快照式撤销需要「修改前」状态而 EditEvent 在修改后发布，故 Undo 订阅此通道压快照；② `snapshot(label)` 改为只发「修改前」通知（对外签名不变，全部调用点零改动）—— `abortRunIfAny + undo.push` 移入 Undo 订阅回调（构造时注册）；③ **autosave 订阅化**：订阅 7 种内容类 EditEvent（NodeAdded / NodeRemoved / NodeMoved / EdgeAdded / EdgeRemoved / ParamChanged / TitleChanged；SelectionChanged 非内容不订阅）→ `onEditContent()` 标记 dirty，**删除每帧 1 Hz `graphContentSignature` 轮询**（`autosave_signature_` / `autosave_last_probe_` 成员删除）；静默 3 秒 / 失败退避 / 暂停闸门 / baseline 语义保留；④ **批量重置操作细粒度事件**（E1 范围边界承接）：`undoOnce` / `redoOnce` / `clearWorkflow` / `openWorkflowFrom` / `restoreAutosave` 整图替换后 `publishGraphDiff` 逐节点 diff 事件（NodeRemoved 隐含端点已删连线）；⑤ `--run-selftest` 新增「编辑订阅」断言（before 通道 FIFO / 通道隔离 / snapshot→undo 压栈 / undoOnce→NodeRemoved diff / autosaveDirty 内容置位 · Selection 不置位）。范围边界：RunSession 订阅编辑事件留 ARCH-D22 / E2 后；ce 独立模型仍不接入（E3/E5）。验证：构建 0/0 · run-selftest PASS · exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links **1792**/0。 |
| 2026-10-23 | **v1.9（当前）** | **ARCH-A1 落地（阶段 3 应用级收口 · AppContext）** —— ① 新增 `ui/app_context.h/.cpp` + 六个服务 h/cpp（`clipboard` / `diagnostics_service` / `recent_files_service` / `autosave_service` / `workflow_file`）：Config / Clipboard / Diagnostics / RecentFiles / AutosaveService / WorkflowFile 从 `EditorState` 拆出为独立对象并收口 `AppContext`（§1.8.10 · 文件命名避让同名头冲突）；② `EditorState` 瘦身为编排层（保留全部对外方法签名薄封装 + `app_ctx()` 访问器 · 移除 12 个已迁移成员与 autosave 订阅循环）；③ **过渡决策 ARCH-A1**：因构造顺序鸡生蛋（AutosaveService / WorkflowFile 依赖 `WorkflowModel&`，本批仍由 EditorState 持有），AppContext 作为 EditorState 成员（`app_ctx_`，声明在 `model_` 之后）构造，A3 `editor()` 退场后迁 app/main 注入（§1.8.10 过渡登记）；④ 调用点收敛（app.cpp config 引用 / 诊断 / 最近菜单 · cli_selftest / output_panel / property_panel `last_archive_dir()`）。验证：构建 0/0 · `--run-selftest` PASS（零断言增减）· exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links 1823/0 · check_all 19/19（[audit/audit_arch_a1.md](../audit/audit_arch_a1.md)）。 |
| 2026-10-23 | **v1.10** | **ARCH-A2 落地（阶段 3 应用级收口 · AutosaveService 独立收尾）** —— ① `EditorState` 六项 autosave 方法全部移除（`tickAutosave` / `pauseAutosave` / `markAutosaveBaseline` / `autosaveDirty` / `flushAutosaveOnExit` / `restoreAutosave`），调用方直连 `app_ctx().autosave`（AutosaveService）与 `app_ctx().workflow_file`（§1.8.6）：app.cpp 主循环 `tick`（tick 返回「本次是否写盘」据此写状态栏）· 启动放行 `mark_baseline` + `pause(false)` · 退出 `flush(current_workflow_path())`；cli_selftest 编辑订阅断言直连 `app_ctx().autosave.dirty()` / `mark_baseline()`（**零断言增减**）；② `restoreAutosave` 编排逻辑迁 app 层恢复弹窗（`do_restore` lambda：`engine::autosaveLoad` → `abortRunIfAny` → `model().load` → `selection().clear()` → `request_navigate` → 路径置空 + `restored_from` → `autosave.mark_baseline()` → 状态栏 · 复用启动时 probe 的 `autosave_info` —— 弹窗期间 autosave 暂停，快照不可变）；③ `editor_state.cpp` 移除 `autosave.h` include（本 TU 不再用 engine::autosave_*）· `clearWorkflow` 基线直连 `app_ctx_.autosave.mark_baseline()`。验证：构建 0/0 · `--run-selftest` PASS（零断言增减）· exec 713/0 · graph 151/0 · check_links **1839**/0 · check_all **19/19**（[audit/audit_arch_a2.md](../audit/audit_arch_a2.md)）。 |

| 2026-10-23 | **v1.8** | **ARCH-E6 落地（阶段 2 编辑侧事件化 · 打包下沉 + 打包-校验同源）** —— ① 新增 `engine::buildCompositeSpecFromSelection`（composite.h/.cpp）：主画布「打包为上级节点」的规格构建（内部图 / 输入 / 输出 / 暴露参数）从 ce_pack.cpp 迁入 engine 单一函数；② **生成阶段自动修正**（修暴露节点保存 · audit §4.2 问题 2）：暴露输入端口在内部图中有内部入边时（variadic 端口内外并存场景），该内部边不纳入内部图 —— 外部输入展开时接管，产物必然通过 `validate_composite_spec`；③ **生成后严格校验**：ce_pack.cpp 先 `validate_composite_spec` 再 `save`（打包-校验同源 = §4 判据 4）；④ `composite_selftest` 新增「打包」断言 21 条（边界输入/输出识别 · 暴露参数含密钥排除 · 运行态清零 · variadic 冲突自动修正 · 修正后可保存）。验证：构建 0/0 · composite_selftest **112**/0 · exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links **1817**/0 · check_all **19/19**（[audit/audit_arch_e6.md](../audit/audit_arch_e6.md)）。 |
| 2026-10-22 | **v1.7** | **ARCH-E5 落地（阶段 2 编辑侧事件化 · CanvasInteractor 双画布收敛）** —— 新增 `ui/interactor.h/.cpp`（`CanvasInteractor` 交互共享层，与 `canvas_view` 绘制层并列）：`CanvasInteract` 回调组（resolve_pin / can_connect（带 would_replace 差异）/ on_link_accepted / on_link_rejected / on_create_tip / on_delete_batch / delete_link / delete_node / on_selection）+ 阶段函数（`interactorCreateLink`（BeginCreate/EndCreate 成对契约内置）· `interactorDeleteOuter`（BeginDelete/EndDelete 契约内置）· `interactorDeleteSelection`（选中删除一次快照 + 清选中）· `interactorReadSelection` / `interactorClearSelection` / `interactorSyncSelection`（ed 选中 API 唯一封装）· `interactorPushNodePosition` / `interactorReadNodePosition`（位置原语：editor_knows + clamp_limit + epsilon）· `interactorWindowActiveMain/Composite`（窗口判定收敛））；主画布 `node_canvas.cpp`（`mainCanvasInteract` 静态缓存 + handle_create_link / handle_delete_items / sync_selection / handle_delete_key / sync_positions / 推位置改走 interactor · 局部 `clamp_position`+`kPositionLimit` 删除 → `kCanvasPositionLimit` 迁入共享层）与 ce `ce_draw.cpp`（`ceInteractCore` 回调组 + compositeEditorDeleteSelection 改 `interactorDeleteSelection` + 连线创建/拖出删除/位置写回/选中同步/快捷键窗口判定改走 interactor）各保留差异点（图源 / 替换语义 / 快照归属 / 提示落点 / clamp）；commands.cpp `ed::ClearSelection` → `interactorClearSelection`。**§4 验收判据 3 满足**：关键交互谓词（GetSelected* / BeginCreate/EndCreate / BeginDelete/EndDelete / Query* / Accept* / Reject* / ClearSelection）只出现在 `interactor.cpp`（rg 验证）。验证：构建 0/0 · run-selftest PASS · exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links **1809**/0 · check_all **19/19**（[audit/audit_arch_e5.md](../audit/audit_arch_e5.md)）。 |
| 2026-10-22 | **v1.6** | **ARCH-E3 + ARCH-E4 落地（阶段 2 编辑侧事件化 · 文档级对象拆分）** —— **E3 `WorkflowModel` 拆分**（§1.8.3）：Graph + Undo + EventBus 从 `editor()` 拆出为文档级对象（持有 `graph` + `undo`（ARCH-D26）+ 私有 `event_bus_` · Undo 订阅「修改前」通道随迁（回调 = 修改前钩子 → `undo.push`，钩子由 `EditorState` 注入 `abortRunIfAny`）· 编辑操作归文档级（`snapshot / undoOnce / redoOnce / clear / load（ARCH-D29）/ createNode / deleteNode` + `publishGraphDiff` 随迁）· `EditorState` 瘦身为编排层（`model()` 访问器 · 编辑方法保留同签名薄封装）· 调用点全库替换（`state.graph` → `state.model().graph` 等 10 文件））；**E4 `SelectionState` 拆分**（§1.8.4 / ARCH-D28）：选中集合从 `EditorState` 拆出为独立对象（新增 `selection_state.h/.cpp` · 持有 `nodes_`（节点 id）/ `links_`（连线 id）/ `single_`（单选节点 id）· 修改方法内置变化检测 → 回调注入发 `EditEvent::SelectionChanged`（依赖图第 6 位无依赖，不 include EventBus）· `ReadOnlyView::selection` 由过渡 `SelectionReadView` 替换为 `const SelectionState&`（渲染调用点不变）· 编辑方法选中操作改走 `selection()` · 调用点全库替换（node_canvas `sync_selection` 重写 + 6 处 `select_node` / toolbar / error_bar / ce_pack / ce_session / ce_draw / app / cli_selftest））。验证：构建 0/0 · run-selftest PASS（E3「编辑订阅」断言照常 + E4「ReadOnlyView 选中同源」升级照常）· exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links **1800**/0（[audit/audit_arch_e3.md](../audit/audit_arch_e3.md) · [audit/audit_arch_e4.md](../audit/audit_arch_e4.md)）。 |
| 2026-10-23 | **v1.11** | **ARCH-Z2 落地（阶段 4 收尾 · `CeSession` 对称拆分 + ce 渲染只读视图化）** —— ① 新增 `ui/ce_session.h` + 5 个 TU：**`CeModel`**（`CompositeSpec` 工作副本 + 撤销/重做栈 **整份快照深 30** + dirty · `ce_model.cpp`）· **`CeSelection`**（内部图选中 + `sync()` 变化检测 · `ce_selection.cpp`）· **`CeSpec`**（对外输入/输出/暴露参数三处标记 + 显示名缓冲——原 `ce_name_buffers()` 函数内 static 收进成员 · `ce_spec.cpp`）· **`CeFile`**（打开 / 保存 / 主画布同 id 热重载 · `ce_file.cpp`）· **`CeSession`**（会话门面：`active`/`ask_close`/`error`/名称·描述缓冲/试运行执行器与读模型 + 4 个对象 + 对外 API 转发 + 内部图编辑 API · `ce_session.cpp`）；② **`CompositeEditorState` 单例删除**（`ce_state.cpp` 只留 `ceUiState()` + 撤销薄转发），`ceSession()` 为单一定义点；③ **ce 渲染只读视图**：新增 `CeReadOnlyView`（对称 `ReadOnlyView` · 每帧在 `app.cpp` 构建一次 · `nodes_for_render`/`edges_for_render`/`find_node`/`graph` 全 const 引用零拷贝 · `drawCompositeEditor(name, open, const CeReadOnlyView&, editor)`），`ce_draw.cpp` 读走 `view`、写走 `session`，`ce_name_buffers()`/`ce_name_buffer()` 删除；④ 调用点收敛：`app.cpp` / `commands.cpp` / `node_library.cpp` 的 `compositeEditorState()` → `ceSession()`，`CMakeLists.txt` 新增 4 个 TU。**§4 验收判据 7 由 🟡 → ✅**；`check_globals` 函数内 static 棘轮 **51 → 50**。验证：构建 **0/0** · `--run-selftest` PASS（断言零增减）· exec **713/0** · graph **151/0** · `ce_repro` **ALL FRAMES OK（mode 0）** · `check_globals` **206 文件 / 违规 0 / fn_static 50·50** · `check_includes` **679/0** · `check_links` **1933/0**（[audit/audit_arch_z2.md](../audit/audit_arch_z2.md)）。 |
| 2026-10-23 | **v1.14（当前）** | **ARCH-Z1 收口 + 8/8 复审 + 冻结解除** —— ① **13 组提交覆盖 14 个 ≥800 行生产文件，全部 < 800**（`provider_spec` 1743→5 TU · `webview_host` 3514→8 · `dom_web_client` 2397→4+2 脚本头 · `pydoll_channel` 1282→3 · `app` 1454→4 · `node_canvas` 1125→2 · `settings_panel` 944→3 · `canvas_view` 938→2 · `property_panel` 855→2 · `ce_draw` 845→2 · `executor` 886→2 · `node_registry` 899→2 · `composite` 1171→3 · `local_nodes` 1242→4）；生产模块现存最大 **766**（`engine/src/graph.cpp`），唯一 ≥800 = 工具 `app/src/cli_selftest.cpp` 1429（口径不计）；② **去重专项 6 步**（`check_counter.h` → `make_selftest_check` · `workflow_io` 恢复 helper · `nodes_detail::resolve_retry_plan` · **新** `provider_common`（anthropic↔gemini）· **新** `site_upload_common`（doubao↔kimi））⇒ 重复行 **1112 → 876** · 重复率 **3.8 % → 2.98 %**（≤3.0 % ✅）；③ **§4 八条验收判据 8/8 ✅**（逐条当场复跑取证 · [audit/audit_arch_z1.md](../audit/audit_arch_z1.md) §11；判据 1 的「GUI 人工单击」标注为非阻塞建议）；④ **冻结按 [FREEZE.md](FREEZE.md) §3 解除**（🟢 已冻结 → 🔵 已解除 · `DevPlan.todo` 冻结组核对 = `📦 冻结待解冻` 从未建立 ⇒ 无组可迁）；⑤ 复审顺带修正文档偏差 3 处（「13 个文件」→ **14 个** · §8 残留「剩余 10 个」待拆表 → 收口实测表 · 三处行数口径 −1）。验证：构建 **0/0** · `check_all` **21/21** · exec **713/0** · graph **151/0** · `ce_repro` 全帧 OK · `check_globals` **254 文件 / 违规 0 / fn_static 50·50** · `check_includes` **254/913/0** · `check_links` **1974/0** · 重复率 **2.98 %**。 |
| 2026-10-23 | **v1.13** | **ARCH-Z1 第 2 组落地（阶段 4 第二批续 · web / DOM 三文件 → 15 TU）** —— **`web/src/webview_host.cpp` 3514 行 → 8 TU**（`webview_host.cpp` **364** = `WindowCtx` 访问器 / `WindowRegistry`+`EnvCache` 单一定义点 / 窗口表 `ctx_find`·`ctx_start`·`ctx_close` / Cookie 提取 · `webview_host_window.cpp` **480** = `ensure_environment`（每线程私有环境 `D-35④`）· 控制器回调 · `window_proc`（`WM_APP+1..+7`）· `run_login_window` · 网络回执挂钩 · `webview_host_probe.cpp` **607** = 探测三段脚本 + 两段式探测 + 就绪三级 `I25` + `peek_page_state`/`wait_ready` · `webview_host_actions.cpp` **399** = 脚本槽 / 复位导航 `PW-08` / `input[type=file]` / CDP 输入 · `webview_host_pow.cpp` **299** = 页面内 PoW + 按站点注销 · `webview_host_api.cpp` **337** = `LoginWindow` / 自检 / `prepare_window` / 归属守卫 · `webview_host_session.cpp` **518** = `ensure_session`×2 / `logout_site` / 回执 / 上传 / CDP 入口 · `webview_host_upload.cpp` **314** = M8-37 附件入口只读识别）；**`ai/src/dom_web_client.cpp` 2397 → 4 TU + 2 脚本模板头**（`dom_web_client.cpp` **583** 会话主体 / `_probe.cpp` **416** 单站探测 / `_probe_all.cpp` **232** 批量探测 / `_selftest.cpp` **391** 自检 · 模板头 `dom_web_client_scripts.h` **445** + `_scripts_poll.h` **373** 逐字搬迁 `constexpr` 常量表 ⇒ 各 TU 只读副本，规则 6 合法）；**`web/src/pydoll_channel.cpp` 1282 → 3 TU**（`pydoll_channel.cpp` **527** 基础设施 + 会话生命周期 / `_commands.cpp` **227** 内容返回 / `_selftest.cpp` **495** 三套自检 + `--pydoll-login`）；**内部契约** = 新增 3 个头（`webview_host_detail.h` **378** 承载 `WindowCtx` + 状态对象 + 消息号 + 30 个入口 · `dom_web_client_detail.h` **46** · `pydoll_channel_detail.h` **71**），**三个公共 API 头一字未改**（调用方零改动）；唯一非搬迁改动 = 3 个 node 夹具的**源码抽取口**改为「脚本头 + 主体 .cpp」拼接（拆分前它们直读 `dom_web_client.cpp`，拆分后抽取失败 ⇒ `check_all` 第 ③ 项报红，已修）；**逐字搬迁校验**（`build/tmp/z1b_verify*.py`）断言「原文件非空行恰好覆盖一次 + 每个区间块逐字出现」40 块 **0 差异**。验证：构建 **0/0** · `check_all` **21/21** · `--provider-selftest` **52/0** · `--upload-selftest` **72/0** · 夹具 **44/17/15** · exec **713/0** · graph **151/0** · `ce_repro` 全帧 OK · `check_globals` **228 文件 / 违规 0 / fn_static 50·50** · `check_includes` **228/725/0** · 重复率 **3.7 %**（[audit/audit_arch_z1_web.md](../audit/audit_arch_z1_web.md)）。 |
| 2026-10-23 | **v1.12** | **ARCH-Z1 第 1 组落地（阶段 4 第二批 · `provider_spec` 五 TU 拆分）** —— `ai/src/provider_spec.cpp` **1743 行 → 5 个 TU**：**加载层** `provider_spec.cpp` **256**（层优先级 `builtin → source → user.d/* → user` · `schema_version` / `replace_all` 规则 · `merge_provider_specs` · `loadProviderSpecs` · 快照与 `reload`）· **解析层** `provider_spec_parse.cpp` **679**（JSON 读取小工具 · 明文密钥双重检测 · 允许字段集 · `parse_models/endpoints/web` · `applyEntry` · `validateFinal` · `fallbackSpecs`）· **站点层** `provider_spec_web.cpp` **268**（`webSpecFor` 回落语义守 `I2` / `strictWebSpecFor` 不回落守 `I14` · 不可用文案与缺失清单 `D-26` · 登录态 `D-27` · 探测适用性 `I16`）· **查询层** `provider_spec_query.cpp` **219**（`findModel` / `find` `ids` `countKind` / 协议与 adapter 清单 / `default*` / `resolve_*`）· **断言层** `provider_spec_selftest.cpp` **384**（`CheckCounter` + 合成夹具 + `providerSpecSelftest`）；**内部契约** = 新增 `ai/include/provider_spec_detail.h` **63 行**（仅 3 个入口跨 TU + `startsWith` / `joinCn` 两个内联单一来源 + `kSchemaVersion`），**公共 API `provider_spec.h` 一字未改**（零签名变动 · 调用方无需改动）；顺带删掉 `joinNames` ↔ `join_list` 两份**逐字相同**的顿号拼接实现与分散的 `starts_with` ⇒ `dup_scan` 克隆块 **91 → 90** · 重复率 **3.8 % → 3.7 %**（ai 层 **5.8 % → 5.6 %**）；`CMakeLists.txt` 显式登记 4 个新 TU。验证：构建 **0/0** · `check_all` **20/20** · `--provider-selftest` **52/0** · `--run-selftest` PASS · exec **713/0** · graph **151/0** · `ce_repro` 全帧 OK · `check_globals` **211 文件 / 违规 0 / fn_static 50·50** · `check_includes` **211/689/0**（[audit/audit_arch_z1.md](../audit/audit_arch_z1.md)）。 |



---

## §9 冻结与解除（原 `FREEZE.md` 摘要 · 2026-10-24 合并）

- **状态**：🟢 已冻结（2026-10-22 启动）→ **🔵 已解除（2026-10-23）**。
- **解除依据**：[§4](#4-验收判据) 八条验收判据 **8/8 ✅**（逐条当场复跑取证）；`DevPlan.todo` 的「📦 冻结待解冻」组从未建立 ⇒ 无组可迁。
- **解除后纪律**：新功能 / 新模块可登记实施；每次改动回归基线 = 构建 0/0 · `check_all` 21/21 · exec 713/0 · graph 151/0 · 文档断链 0。
- **原文归档**（冻结范围 / 逐批记录 / 解除动作）→ [../Archive/refactor/FREEZE.md](../Archive/refactor/FREEZE.md)。

---

**文档结束 · v1.14 定稿。**
**状态：ARCH-Z1 已收口 · §4 验收判据 8/8 ✅ · 冻结已解除（🟢 → 🔵 · 2026-10-23 · 摘要见 §9）。**
**下一步（解冻后）：新功能 / 新模块可登记实施（回归基线 §4 仍每次改动必跑：构建 0/0 · `check_all` 21/21 · exec 713 · graph 151 · 断链 0）；若续做 Z1 遗留口径 = 工具类 `app/src/cli_selftest.cpp` 1429 行（另立批）· 去重余量 **2.98 %**（>3.0 % 才需再专项）。**
**已完成：ARCH-R1（RunSession 对象化）· ARCH-R2（RunEvent 订阅化）· ARCH-R3（ReadOnlyView 渲染视图）· ARCH-R4（命令路由层）· ARCH-E1（EventBus + EditEvent）· ARCH-E2（Undo/autosave 订阅化）· ARCH-E3（WorkflowModel 拆分：Graph + Undo + EventBus 拆出 · 文档级独立）· ARCH-E4（SelectionState 拆分：选中独立）· ARCH-E5（CanvasInteractor 落地：双画布交互收敛为一套）· ARCH-E6（打包下沉：buildCompositeSpecFromSelection 进 engine + 生成后严格校验 · 打包-校验同源）· ARCH-A1（AppContext 收口：Config/Clipboard/Diagnostics/RecentFiles/AutosaveService/WorkflowFile 六项应用级服务统一 · 过渡形态 = EditorState 成员）· ARCH-A2（AutosaveService 独立收尾：EditorState autosave 方法移除 · 调用方直连 app_ctx().autosave · restore 编排落 app 层）· ARCH-A3（`editor()` 退场：AppContext 启动组装 + EditorController 注入）· ARCH-A4（依赖单向断言工具化：`check_includes.ps1`）· ARCH-A5（全局变量断言工具化：`check_globals.ps1` + 棘轮基线 + 跨 TU `extern` 清零）· ARCH-A6（ui 层 60 条文件级 static 逐文件对象化 · 7 个状态对象归 `EditorController` · 基线 **87 → 27**）· ARCH-A7（web/utils 27 条文件级 static 对象化 · **存量 87 → 0** · 删基线切严格零容忍 + 函数内 static 棘轮 51）· ARCH-Z2（`CeSession` 五对象对称拆分：`CeModel` / `CeFile` / `CeSelection` / `CeSpec` + `CeSession` 门面 · ce 渲染 `CeReadOnlyView` 化 · **判据 7 ✅** · 棘轮 51 → 50）· ARCH-Z1（**超长文件瘦身收口**：13 组提交 / 14 个文件全部 < 800 行 + 去重专项 6 步 **3.8 % → 2.98 %** · **§4 判据 8/8 ✅ ⇒ 冻结解除**）—— 审计见 docs/Archive/audit/audit_arch_r1.md / audit_arch_r2.md / audit_arch_r3.md / audit_arch_r4.md / audit_arch_e1.md / audit_arch_e2.md / audit_arch_e3.md / audit_arch_e4.md / audit_arch_e5.md / audit_arch_e6.md / audit_arch_a1.md / audit_arch_a2.md / audit_arch_a3.md / audit_arch_a4.md / audit_arch_a5.md / audit_arch_a6.md / audit_arch_a7.md / audit_arch_z2.md / audit_arch_z1.md（§8 收口表 · §10 去重专项 · §11 8/8 复审）/ audit_arch_z1_web.md**（2026-10-24 已迁 `docs/Archive/audit/` · 索引见 [../audit/refactor_summary.md](../audit/refactor_summary.md)）。**











