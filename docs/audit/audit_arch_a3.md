# audit_arch_a3：ARCH-A3 落地审计报告（`editor()` 退场 · AppContext 启动组装 + EditorController 注入）

> 批次：`ARCH-A3` · 日期：2026-10-23 · 计划：docs/refactor/ARCHITECTURE.md §2.1 迁移路线阶段 3「应用级收口（ARCH-A）」A3
> 性质：**上帝对象入口退场** —— 删除全局单例 `editor()`（连同 `EditorState` 类名），`AppContext` 回到 §1.8.10 目标形态（启动时按 §1.8.11 依赖顺序组装、只注入一次、运行时不可访问）；UI 编排层改为**无所有权**的 `EditorController`（只持 8 个服务引用），由 app/main 与各 UI 模块**经参数 / 命令上下文**传递。
> 判据：构建 0 error / 0 warning · `--run-selftest` PASS（**零断言增减**）· exec 713/0 · graph 151/0 · check_links 1845/0 · check_all 19/19 · `rg "editor\(\)" source/` = **0**（含注释）。

## 1. 背景与目标（承接 ARCHITECTURE §4 判据 6 / 8）

A1 收口了六个应用级服务、A2 移除了 autosave 薄封装，但 `EditorState` 仍是**全局单例**（`editor()`）且 `AppContext` 是其成员（`app_ctx_`）—— 违反 §1.8.10「启动时组装 + 只注入一次 + 运行时不可访问」，也违反 §4 判据 6「全库 `editor()` 引用为 0」。

A3 的目标 = **入口消失，依赖显式**：

1. `AppContext` 独立成容器：按 §1.8.11 顺序构造 Config → Clipboard → Diagnostics → WorkflowModel（含 EventBus）→ SelectionState → RunSession → AutosaveService → WorkflowFile。
2. `EditorState` 改名 `EditorController` 并**去掉所有权**：8 个服务改为引用成员；删除 `app_ctx()` 访问器（各服务各取所需）。
3. 删除 `editor()` 全局单例；所有 UI 模块改为接收 `EditorController&`（或经 `CommandContext.editor`）—— 运行时不再回取 `AppContext`（判据 8）。

## 2. 改动清单

### 2.1 `AppContext`（目标形态：纯容器 + 启动组装）

`source/ui/include/app_context.h` / `source/ui/src/app_context.cpp` 重写：

| 位次（§1.8.11） | 成员 | 依赖 |
|---|---|---|
| 1 | `EventBus`（WorkflowModel 成员） | 无 |
| 2 | `Config config` | 无 |
| 3 | `Clipboard clipboard` | 无 |
| 4 | `Diagnostics diagnostics` | 无 |
| 5 | `WorkflowModel model` | EventBus |
| 6 | `SelectionState selection` | 无 |
| 7 | `RunSession run` | EventBus（经方法参数取图） |
| 8 | `AutosaveService autosave` | EventBus + WorkflowModel |
| 9 | `WorkflowFile workflow_file` | EventBus + WorkflowModel（内含 RecentFiles） |
| — | `EditorController editor` | 上列 8 个引用（编排层，不拥有数据） |

- 构造顺序 = **成员声明顺序**；`AppContext::AppContext()` 初始化列表只写 `autosave(model) / workflow_file(model) / editor(...)`，顺序与声明一致（无 `-Wreorder`）。
- 退出顺序天然相反（§1.8.11）：`WorkflowFile` → `AutosaveService` → `RunSession` → `SelectionState` → `WorkflowModel` → `Diagnostics` → `Clipboard` → `Config`；`autosave.flush()` 由 app 退出路径显式调用（A2）。
- **不执行逻辑 / 不渲染 / 不处理事件 / 不持有 UI**（§1.8.10 限制）；只在启动阶段被 app/main 读取成员。

### 2.2 `EditorState` → `EditorController`（无所有权编排层）

新文件 `source/ui/include/editor_controller.h` + `source/ui/src/editor_controller.cpp`（原 `editor_state.*` 删除）：

| 维度 | A2 形态 | A3 形态 |
|---|---|---|
| 成员 | 拥有 `WorkflowModel model_` / `AppContext app_ctx_` / `SelectionState selection_` / `RunSession run` | **只持 8 个引用**（`model_` / `selection_` / `run_` / `config_` / `clipboard_` / `diagnostics_` / `autosave_` / `workflow_file_`） |

### 2.3 注入改造（UI 模块直接持引用，不再回头取全局）

| 模块 | 新签名（ARCH-A3） |
|---|---|
| 画布 | `drawNodeCanvas(title, CanvasOptions&, const ReadOnlyView&, EditorController&)` · `canvasAddNodeAtCenter(type, EditorController&)` · `canvasAddCompositeAtCenter(id, EditorController&)` |
| 参数面板 | `draw_property_panel(title, bool*, Node*, const ReadOnlyView&, PropertyEditResult&, EditorController&)` |
| 工具栏 | `drawToolbarButtons(EditorController&)`（内部 `dispatchCommand` 传 `CommandContext{nullptr, nullptr, &editor}`） |
| 节点库 | `drawNodeLibrary(title, bool*, EditorController&)` |
| 命令层 | `CommandContext` 新增 `EditorController* editor`；`dispatchCommand` 入口守卫（nullptr → `log::error` + 忽略，不崩） |
| 错误条 / 输出面板 | `drawErrorBar(EditorController&)` · `drawOutputPanel(..., EditorController&)`（类型改名，签名形状不变） |
| 上级节点编辑器 | `drawCompositeEditor(title, bool*, EditorController&)` · `compositeEditorSave(std::string*, EditorController&)` · `compositeEditorStartRun(std::string*, EditorController&)` · `compositePackSelection(name, std::string*, EditorController&)` · `compositePackDialog(EditorController&)` · `drawCompositeDialogs(EditorController&)` |
| 面板诊断 | `requestConnectionCheck(id, EditorController&)` · `requestSelectorCheck(id, EditorController&)` · `openWebLoginWindow(id, EditorController&)` |

主画布交互回调组（`makeMainCanvasInteract`）由「无捕获 lambda」改为「捕获注入的 `editor` 引用」；同时**去掉文件级静态缓存**（`static const CanvasInteract`）→ 每次按需构造（`std::function` 构造廉价），为 ARCH-A5「文件级可变静态状态归零」提前清账。

### 2.4 启动组装点（唯一）

- `app.cpp`：`run()` 开头 `AppContext app_context;` + `EditorController& state = app_context.editor;`，`Config& config = app_context.config;`；主循环与所有面板调用统一传 `state`（原 `editor().` 全部替换为 `state.`）。
- `cli_selftest.cpp`：`--run-selftest` 同样 `AppContext app_context;` + `EditorController& state = app_context.editor;`（自检与产品同一条组装路径）。

### 2.5 构建清单

- `source/CMakeLists.txt`：`ui/src/editor_state.cpp` → `ui/src/editor_controller.cpp`（`app_context.cpp` 原位保留）。
- 删除 `source/ui/include/editor_state.h` · `source/ui/src/editor_state.cpp`。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `source/build.ps1` | ✅ 构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ **PASS** —— 全部既有断言原样通过（F1 复制/粘贴 · 工作流往返 · PC-05 归档 · ReadOnlyView · 命令路由 · 编辑事件 · 编辑订阅） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `tools/check_all.ps1` | ✅ **19/19 全 PASS** |
| `check_links.py` | ✅ **1845 / 0** |
| `ce_repro`（mode 0 窗口栈回归） | ✅ ALL FRAMES OK |

**零断言增减**：未新增 / 未删除任何断言；自检访问路径由 `editor()` 改为局部 `AppContext.editor`，语义逐字一致。

## 4. 验收判据对照（§4）

| 判据 | 状态 | 取证 |
|---|---|---|
| **6. `editor()` 退场** | ✅ **满足** | `rg "\beditor\(\)" source/` = **0**（代码 + 注释均无；历史沿革改写为「全局编辑器单例」措辞） |
| **8. `AppContext` 运行时不可访问** | ✅ **满足（本批范围）** | `rg "AppContext" source/` 的**代码引用（非注释行）** 仅命中 `ui/include/app_context.h` · `ui/src/app_context.cpp` · `ui/src/app.cpp`（启动组装）· `app/src/cli_selftest.cpp`（自检组装）；UI 面板代码 0 处 |

## 5. 过渡登记（本批不做，留给后续批次）

- `compositeEditorState()`（上级节点编辑器会话单例）**仍在** —— 归 **ARCH-Z2**（`CeSession` 对称拆分：`CeModel` / `CeFile` / `CeSelection` / `CeSpec`）。
- UI 瞬时状态（`status` / `last_error` / `request_*` / `follow_*`）仍集中在 `EditorController` —— §1.5 未把它列为长生命周期对象；目标态由 **ARCH-Z1**（`app.cpp` 主循环 → `AppFrame`）把状态分别下沉到状态栏 / 错误条 / 画布。
- `EditorController` 仍是编排汇聚点（不是数据/服务所有者）；它不进入 `AppContext` 的 §1.5 生命周期分层（不持有资源），仅作为启动注入口存在。

## 6. 回滚点

`git checkout -- source/CMakeLists.txt source/ui source/app docs/refactor/ARCHITECTURE.md docs/refactor/FREEZE.md docs/CHANGELOG.md docs/DocMAP.md` 即回滚到 A2 形态（保留 `docs/audit/audit_arch_a3.md` 仅作登记；删除即可）。

| 构造 | 默认构造（成员自建） | `EditorController(8 个引用)`；拷贝/赋值 = `delete` |
| 访问器 | `model()` / `selection()` / `run_snapshot_view()` / `executor()` / `app_ctx()` | 同上 **去掉 `app_ctx()`**，新增 `run()` / `config()` / `clipboard()` / `diagnostics()` / `autosave()` / `workflow_file()` / `event_bus()` |
| 全局入口 | `EditorState& editor()` 单例 | **删除**（`rg "editor\(\)" source/` = 0） |
| 薄封装 | `recentWorkflows()` / `last_archive_dir()` | **移除**（调用方直连 `workflow_file().recent_workflows()` / `.last_archive_dir()`） |
| 编排保留 | 状态栏 / 选中清空 / autosave 基线 / 运行收尾 / 错误条 / 文件对话框 | 不变（`saveWorkflowTo` / `openWorkflowFrom` / `snapshot` / `undoOnce` / `start*Run*` / `refreshLastError` …） |

- `openWorkflowDialog()` / `saveWorkflowDialog()` 改为 `openWorkflowDialog(EditorController&)` / `saveWorkflowDialog(EditorController&)`（菜单与命令层共用，不再取全局）。
- `archiveRunOutputs()` 收为 private（唯一调用方 = `finishRunSession`），行为不变。
