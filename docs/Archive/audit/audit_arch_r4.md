# audit_arch_r4：ARCH-R4 落地审计报告（命令路由层 · 三入口收敛 + 修 Ctrl+S）

> 批次：`ARCH-R4` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 1
> 性质：**运行侧事件化第四批** —— 命令路由层：菜单 / 工具栏 / 快捷键三入口收敛到 `dispatchCommand` 单一实现 + 补主窗口 Ctrl+S。
> 判据：行为等价（`--exec-selftest` 713/0 只升不降 · `--run-selftest` PASS 新增命令路由断言）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §2.1 三入口行为漂移 / §6 阶段 1 R4）

| 问题 | 现状（本批前） | 目标 | 本批落点 |
|---|---|---|---|
| 三入口行为漂移 | 菜单 / 工具栏 / 快捷键各自接线（同一动作多处实现，易不一致） | `dispatchCommand` **单一实现** | 菜单（文件/编辑/上级节点/帮助）· 工具栏（撤销/重做/删除/新建/示例/打开/保存）· 快捷键（主画布 Delete · ce Delete/Ctrl+S）统一经 `dispatchCommand` |
| 主窗口无 Ctrl+S | 主窗口只有菜单「保存工作流…」无快捷键（快捷键缺失） | 主窗口 Ctrl+S = 保存工作流 | 快捷键表 `{SaveWorkflow, Ctrl+S, Main}` + 主循环 `handleShortcut(Main, …, only=SaveWorkflow)` |

## 2. 改动清单

### 2.1 新增 `source/ui/include/commands.h` + `source/ui/src/commands.cpp`

- `AppCommand` 命令全集（22 个）：文件（NewWorkflow / SampleWorkflow / OpenWorkflow / SaveWorkflow / Settings / Exit）· 编辑（Undo / Redo / Copy / Paste / DeleteSelection）· 运行（Run / Stop）· 上级节点（CompositeNew / CompositePack / CompositeOpenEditor / CompositeSave / CompositeDeleteSelection）· 帮助（CopyDiagnostics / OpenLogDir / OpenDataDir / FirstRunGuide）+ 哨兵 `Count`。
- `dispatchCommand(AppCommand, const CommandContext&)`：**单一实现** —— 全部命令的动作集中于此（调用 `editor()` / 对话框函数 / ce 会话接口）。无可用性保护的命令（如 `DeleteSelection`）在 dispatch 内做前置条件兜底（无选中静默返回，与 Delete 键原行为一致），菜单侧仍用 `enabled` 灰显。
- `CommandContext`：承载 app.cpp 主循环**局部**状态（`GLFWwindow* window` = Exit 用；`bool* request_first_run_guide` = 首启引导用）。对话框函数 / `editor()` / `compositeEditorState()` 等 ui 层接口命令层可直接调用，不进 ctx。
- 快捷键表 `kShortcutTable`（表驱动）+ `handleShortcut`：
  - `CommandScope::Main`：`{SaveWorkflow, Ctrl+S}` · `{DeleteSelection, Delete}`
  - `CommandScope::CompositeEditor`：`{CompositeSave, Ctrl+S}` · `{CompositeDeleteSelection, Delete}`
  - 通用隔离（文本输入 `WantTextInput` / 激活控件 / 弹出层 / 左键按住）内部统一判定；`window_active` 由调用方在窗口上下文内判定传入（聚焦/悬停）。
  - `only` 参数：主循环只检测 SaveWorkflow 用（避免 Delete 在主循环误触发，保持 Delete 画布窗口语义不变）。

### 2.2 菜单收敛（`app.cpp`）

- 文件：新建 / 示例 / 打开 / 保存 / 设置 / 退出 → `dispatchCommand`（退出经 `cmd_ctx.window`）。「最近打开」子菜单保留（动态列表 + 失效校验 + 清理，非命令动作）。
- 编辑：撤销 / 重做 / 复制 / 粘贴 / 删除选中 → `dispatchCommand`（enabled 保留）。
- 上级节点：新建 / 打包 / 打开编辑器 → `dispatchCommand`（打开编辑器 = `CompositeOpenEditor`，dispatch 内保留「非活跃才新建」语义）。
- 帮助：复制诊断 / 首启引导 / 打开日志目录 / 打开数据目录 → `dispatchCommand`（原内联实现移入 dispatch）。
- 视图菜单（面板可见性 + config 持久化）**保留原样** —— 仅菜单一个入口（无三入口漂移），且强耦合 app.cpp 局部 config 持久化；见 §4 范围边界。
- 新增 `const CommandContext cmd_ctx{window, &request_first_run_guide};`（菜单使用）。

### 2.3 工具栏收敛（`toolbar.cpp`）

- 撤销 / 重做 / 删除选中 / 新建（清空）/ 示例 / 打开 / 保存 → `dispatchCommand`（可用性 `BeginDisabled` 保留）。
- 运行 / 停止 **保留原实现**：运行前预检（P1-b：未接线分支弹窗确认）为工具栏特有流程、且运行/停止无第二入口（无三入口漂移）；`AppCommand::Run/Stop` 枚举保留（未来入口用）。

### 2.4 快捷键收敛 + 修 Ctrl+S

- **主画布 Delete**（`node_canvas.cpp handle_delete_key`）：窗口条件（画布聚焦/悬停）+ 通用隔离 + 按键检测统一改走 `handleShortcut(Main, window_active, ctx, only=DeleteSelection)`；删除执行经 `dispatchCommand(DeleteSelection)`（`deleteSelected()` + `ed::ClearSelection()`，与菜单/工具栏同一实现）。
- **上级节点编辑器**（`ce_draw.cpp`）：
  - 删除 ce 选中逻辑从 `ce_handle_delete_key` 迁为公共接口 `compositeEditorDeleteSelection()`（composite_editor.h 声明，ce_draw.cpp 实现）—— 无窗口/按键检测，只做删除（含 cePushSnapshot 一次批次一次快照）。
  - ce Delete / Ctrl+S 检测统一改走 `handleShortcut(CompositeEditor, window_active=根窗口聚焦/悬停, ctx)`（原 `ce_handle_delete_key` / `ce_handle_save_key` 移除；DEBT-B1 历史注释保留，语义不变：Ctrl+S 保存与「保存」按钮同一路径）。
  - ce 顶栏「保存」按钮 → `dispatchCommand(CompositeSave)`（与 Ctrl+S 完全同一实现）。
- **主窗口 Ctrl+S（修）**（`app.cpp` 主循环）：`handleShortcut(Main, window_active = !(ce 活跃 && ce 窗口聚焦/悬停), cmd_ctx, only=SaveWorkflow)`。
  - 作用域隔离：`compositeEditorWindowFocused()`（ce_draw 每帧在 ce 窗口上下文更新 g_ce_window_focused；RootAndChildWindows）—— ce 窗口聚焦/悬停时 Ctrl+S 归 ce（保存上级节点），否则归主窗口（保存工作流）。
  - `IsKeyChordPressed` 同帧只触发一次：主循环先检测消费，画布/ce 窗口内检测不会重复。

### 2.5 其他收敛

- `node_library.cpp`：节点库「新建上级节点…」「打包选区为上级节点…」按钮 → `dispatchCommand(CompositeNew / CompositePack)`（与菜单同一实现）。
- `composite_editor.h`：新增 `compositeEditorDeleteSelection()` 与 `compositeEditorWindowFocused()` 声明。

### 2.6 自检（只升不降）

- `cli_selftest.cpp` 新增「命令路由」断言块：
  - 快捷键表完整性：Main 含 Delete / Ctrl+S · CompositeEditor 含 Delete / Ctrl+S（**修 Ctrl+S 的静态证据**）。
  - 表内唯一性：同一作用域 × 命令不重复（避免二义触发）。
  - 汇总进 `--run-selftest` 最终 PASS 判定。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `check_all.ps1`（含 build.ps1） | ✅ **19/19**：构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（含新增「命令路由：Main.Delete=有 / Main.Ctrl+S=有 / ce.Delete=有 / ce.Ctrl+S=有 / 表内唯一=是」） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK**（画布窗口栈回归） |
| `check_links.py` | ✅ **1774 / 0**（基线持平；新增 audit_arch_r4.md 链接） |
| `rg "dispatchCommand"` | ✅ 三入口全部收敛：app.cpp（菜单 + 主循环 Ctrl+S）· toolbar.cpp · node_canvas.cpp（Delete）· ce_draw.cpp（Delete/Ctrl+S/保存按钮）· node_library.cpp |
| 人工确认 Ctrl+S（GUI 真机） | ⏳ **待用户真机复核**（本批验证矩阵按 ARCHITECTURE §6 R4 = 「构建 + 人工确认 Ctrl+S」；快捷键表静态证据已入 `--run-selftest`，触发链路与菜单「保存工作流…」完全同一实现） |

## 4. 范围边界（本批**不**收敛 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| 视图菜单（节点库/参数面板/Console/输出/信息 可见性 + config 持久化 + 重置布局） | 仅菜单一个入口（无三入口漂移）；强耦合 app.cpp 局部 config 持久化（写 config.toml + `setAppConfig`） | 后续批次 / ARCH-A（应用级收口） |
| 运行 / 停止（工具栏） | 运行前预检（P1-b：未接线分支弹窗确认）为工具栏特有流程；运行/停止无第二入口 | 若未来加快捷键，经 `dispatchCommand(Run/Stop)`（枚举已就位） |
| 最近打开子菜单（动态列表 + 失效校验 + 清理失效条目） | 动态数据驱动（非固定命令动作） | — |
| 对话框内部按钮（文件对话框等） | 非三入口（对话框独有交互） | — |

## 5. 过渡登记

- `compositeEditorWindowFocused()` 为**跨帧窗口状态查询**（ce_draw 每帧更新 g_ce_window_focused；主循环用于 Ctrl+S 作用域归属判断）。ce 会话关闭时 `active=false` 使主循环条件短路，不依赖残留值。
- `AppCommand::Run / Stop` 枚举 + dispatch 实现已就位但**本批无调用方**（运行按钮保留 P1-b 预检）；非未使用 —— 供未来入口（快捷键/菜单）直接接入。

## 6. 回滚点

`git checkout -- source/ui/include/commands.h source/ui/src/commands.cpp source/ui/include/composite_editor.h source/ui/src/ce_draw.cpp source/ui/src/app.cpp source/ui/src/toolbar.cpp source/ui/src/node_canvas.cpp source/ui/src/node_library.cpp source/app/src/cli_selftest.cpp source/CMakeLists.txt docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 R3 形态（保留 `docs/audit/audit_arch_r4.md` 仅作登记；删除即可）。

