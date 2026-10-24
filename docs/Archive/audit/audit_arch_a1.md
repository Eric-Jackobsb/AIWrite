# audit_arch_a1：ARCH-A1 落地审计报告（AppContext 收口 · 六个应用级服务统一）

> 批次：`ARCH-A1` · 日期：2026-10-23 · 计划：docs/refactor/ARCHITECTURE.md §2.1 迁移路线阶段 3「应用级收口（ARCH-A）」A1
> 性质：**应用级收口第一批** —— Config / Clipboard / Diagnostics / RecentFiles / AutosaveService / WorkflowFile 六项服务从 `EditorState` 拆出为独立对象，并收口到统一容器 `AppContext`（`source/ui/include/app_context.h` + `source/ui/src/app_context.cpp`）。
> 判据：§4 验收判据 8（`AppContext` 运行时不可访问）为**目标态**（A3 后由 `rg` 守护）；本批判据 = 构建 0 error / 0 warning · `--run-selftest` PASS（F1 复制/粘贴 · 工作流往返 · PC-05 归档 · 编辑订阅 autosaveDirty 断言）· exec 713/0 · graph 151/0 · check_all 全 PASS · **零断言增减**。

## 1. 背景与目标（承接 ARCHITECTURE §1.8 对象定义总表）

架构 v1.1 定稿的 10 个对象中，`AppContext` 负责收口六个应用级服务。E 系列已把**文档级**对象（WorkflowModel / SelectionState / ReadOnlyView）拆出；A1 把仍留在 `EditorState` 的**应用级**服务（配置 / 剪贴板 / 诊断 / 最近列表 / 自动保存 / 工作流文件）拆为独立对象，让 `EditorState` 进一步瘦身为纯编排层。

## 2. 关键决策：AppContext 的构造方式（构造顺序鸡生蛋）

计划原设想「AppContext 由 app.cpp 外部组装，构造后传入 EditorState」。实施时发现**构造顺序鸡生蛋**：

- AutosaveService 与 WorkflowFile 依赖 `WorkflowModel&`（§1.8.11 依赖图第 8 / 9 位），
- 而 `WorkflowModel` 在 A1 阶段仍由 `EditorState` 持有（`model_`，§1.8.3 过渡登记 E3），
- 若先构造 AppContext 再传 EditorState，AppContext 无法在 `model_` 之后拿到其引用。

**决策**：本批 AppContext 作为 `EditorState` 的**成员**（`app_ctx_`，声明在 `model_` 之后），构造顺序 = `model_ → app_ctx_ → run → selection_`。对外 `EditorState` 新增 `app_ctx()` 访问器。随 ARCH-A3 `editor()` 退场后，AppContext 迁到 app/main 层外部注入（届时 UI 模块直接持有 WorkflowModel），即回到 §1.8.10 / §1.8.12 的目标形态。

## 3. 改动清单

### 3.1 新增 7 个文件（ui/include + ui/src）

| 文件 | 职责 | 拆出前位置 |
|---|---|---|
| `app_context.h/.cpp` | 六服务容器（成员 = config / clipboard / diagnostics / autosave / workflow_file） | —（新建） |
| `clipboard.h/.cpp` | 复制/粘贴（深拷贝 + 注入；`copy(选区, 源图)` / `paste(目标图)` → 新节点 + 新连线） | `EditorState::copySelection / pasteClipboard` 内的数据与注入逻辑 |
| `diagnostics_service.h/.cpp` | 异常计数 / 内存采样封装 + 工作线程结论互斥通道 | `background_mutex_ / background_status_ / background_pending_` + `utils::diagnostics` 直调 |
| `recent_files_service.h/.cpp` | 最近列表薄封装（load/push/clear/status/prune） | `engine::recentWithStatus()` 等直调 |
| `autosave_service.h/.cpp` | 订阅内容类 EditEvent → 节流写快照（tick/pause/baseline/flush/clear） | `EditorState` autosave 订阅循环 + 节流状态 + `onEditContent` |
| `workflow_file.h/.cpp` | 保存/打开/最近/归档 + 归口 `current_workflow_path` / `restored_from_autosave_path` / `last_archive_dir` | `saveWorkflowTo / openWorkflowFrom / recentWorkflows / workflowDisplayName / archiveRunOutputs` |

命名避让：`diagnostics.h` → `diagnostics_service.h`、`recent_files.h` → `recent_files_service.h`（ui 与 utils / engine 同名头冲突）。

### 3.2 `EditorState` 瘦身（编排层）

- **新增**：`AppContext app_ctx_`（成员，声明在 `model_` 之后）+ `app_ctx()` 访问器。
- **移除公有成员**：`current_workflow_path` / `restored_from_autosave_path` / `autosave_paused` / `last_archive_dir`（后三者分别经访问器 `last_archive_dir()` 或委托读取）。
- **移除私有成员**：`clipboard_nodes_ / clipboard_edges_`、`background_mutex_ / background_status_ / background_pending_`、`autosave_last_change_ / autosave_retry_after_ / autosave_change_seen_`、`autosave_sub_handles_`、`onEditContent`。
- **保留全部对外方法签名薄封装**（调用点零改动）：`saveWorkflowTo / openWorkflowFrom / recentWorkflows / tickAutosave / pauseAutosave / markAutosaveBaseline / autosaveDirty / flushAutosaveOnExit / restoreAutosave / copySelection / pasteClipboard / has_clipboard / postBackgroundStatus / takeBackgroundStatus / workflowDisplayName / archiveRunOutputs`。
- 构造器成员初始化列表：`EditorState::EditorState() : app_ctx_(model_)`（autosave 订阅在 `AutosaveService` 构造时注册、析构时注销，语义与拆出前一致）。

### 3.3 调用点收敛

- `app.cpp`：`Config& config = editor().app_ctx().config;`（`editor()` 首次调用提前到配置加载处——AppContext 构造不依赖 GLFW/窗口，可提前）；诊断四处改走 `app_ctx().diagnostics.*`；「最近打开」菜单改走 `app_ctx().workflow_file.recent_files().*`；恢复弹窗「忽略」改 `app_ctx().autosave.clear_snapshot()`。
- `cli_selftest.cpp` / `output_panel.cpp` / `property_panel.cpp`：`last_archive_dir` 公有成员 → `last_archive_dir()` 访问器。
- `connection_test.cpp`：`postBackgroundStatus` 经 EditorState 委托（零改动）。

### 3.4 行为等价要点

- `pasteClipboard` 的 EdgeAdded 事件负载由 `(from, port, to, port, edge_id)` 改为完整 `Edge` 对象（`EditEvent::edge(EdgeAdded, edge)`，语义一致）。
- autosave / 归档仍经进程缓存 `appConfig()` 读配置（与拆出前一致，保持「设置面板保存后下次启动生效」）。
- `tickAutosave` 由 `AutosaveService::tick` 返回「本次是否写盘」，`EditorState` 据此写状态栏（文案不变）。

## 4. 验证结果

| 验证 | 结果 |
|---|---|
| `source/build.ps1` | ✅ 构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ **PASS** —— F1 复制/粘贴 OK · 工作流文件往返 OK · PC-05 归档 OK · 编辑订阅（autosaveDirty 内容置位 / Selection 不置位）OK · ReadOnlyView 配置同源 OK |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `tools/check_all.ps1` | ✅ **19/19 全 PASS**（含 ce_repro 全帧 OK · package payload 19 文件 / 35.4 MB） |
| `check_links.py` | ✅ **1823 / 0**（本批新增文档链接经补跑后全绿） |

**零断言增减**：`--run-selftest` 全部既有断言（含 autosaveDirty / F1 复制粘贴 / 归档）原样通过，未新增未删除。

## 5. 过渡登记

- AppContext 本批为 **EditorState 成员**（§2 决策），尚未满足 §4 验收判据 8「运行时不可访问」——该判据由 `rg "AppContext"` / `rg "ctx\\."` 在 ARCH-A3 `editor()` 退场后守护（目标态见 ARCHITECTURE §1.8.10 / §1.8.13）。
- 六个服务为 **ui 层公共 API**（`ui/include/*.h` 头文件签名新增）—— 接口冻结 §1 例外登记（重构批次内新增，显式登记于 FREEZE §5 / ARCHITECTURE §8 v1.9）。
- `EditorState` 保留同签名薄封装仅为 A3 前的过渡接口；A3 后随 `editor()` 一并移除，调用方直接持有各服务对象。

## 6. 回滚点

`git checkout -- source/ui/include/app_context.h source/ui/src/app_context.cpp source/ui/include/clipboard.h source/ui/src/clipboard.cpp source/ui/include/diagnostics_service.h source/ui/src/diagnostics_service.cpp source/ui/include/recent_files_service.h source/ui/src/recent_files_service.cpp source/ui/include/autosave_service.h source/ui/src/autosave_service.cpp source/ui/include/workflow_file.h source/ui/src/workflow_file.cpp source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/ui/src/app.cpp source/app/src/cli_selftest.cpp source/ui/src/output_panel.cpp source/ui/src/property_panel.cpp source/CMakeLists.txt docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 E6 形态（保留 `docs/audit/audit_arch_a1.md` 仅作登记；删除即可）。
