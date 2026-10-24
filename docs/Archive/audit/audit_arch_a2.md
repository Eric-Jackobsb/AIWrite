# audit_arch_a2：ARCH-A2 落地审计报告（AutosaveService 独立收尾 · EditorState autosave 方法移除）

> 批次：`ARCH-A2` · 日期：2026-10-23 · 计划：docs/refactor/ARCHITECTURE.md §2.1 迁移路线阶段 3「应用级收口（ARCH-A）」A2
> 性质：**autosave 解耦收尾** —— `EditorState` 六项 autosave 方法（`tickAutosave` / `pauseAutosave` / `markAutosaveBaseline` / `autosaveDirty` / `flushAutosaveOnExit` / `restoreAutosave`）随 `editor()` 退场（预期 A3）一并移除，调用方直连 `app_ctx().autosave`（AutosaveService，A1 已拆出对象）与 `app_ctx().workflow_file`。
> 判据：构建 0 error / 0 warning · `--run-selftest` PASS（编辑订阅 autosaveDirty 断言照常 · **零断言增减**）· exec 713/0 · graph 151/0 · check_links 1839/0 · check_all 19/19。

## 1. 背景与目标（承接 ARCHITECTURE §1.8.6 / §2.1 A2 行）

A1 已把 AutosaveService 拆为独立对象收口 `AppContext`，但 `EditorState` 仍保留六个同签名薄封装（`tickAutosave` / `pauseAutosave` / `markAutosaveBaseline` / `autosaveDirty` / `flushAutosaveOnExit` / `restoreAutosave`）。A2 的目标 = **移除这些过渡薄封装**，autosave 调用点全部直连 `app_ctx().autosave` —— 这是 `editor()` 退场（A3）前的最后一个 autosave 耦合面。

## 2. 改动清单

### 2.1 `EditorState` 移除六项 autosave 方法

| 方法 | 落点（移除后） |
|---|---|
| `tickAutosave()` | app.cpp 主循环直连 `app_ctx().autosave.tick(current_workflow_path())`，`tick` 返回「本次是否写盘」→ 写状态栏（文案不变） |
| `pauseAutosave(bool)` | app.cpp 启动放行 / 恢复弹窗两处直连 `app_ctx().autosave.pause(...)` |
| `markAutosaveBaseline()` | app.cpp 两处 + cli_selftest 两处 + `clearWorkflow` 内部（`app_ctx_.autosave.mark_baseline()`） |
| `autosaveDirty()` | cli_selftest 编辑订阅断言直连 `app_ctx().autosave.dirty()` |
| `flushAutosaveOnExit()` | app.cpp 退出路径直连 `app_ctx().autosave.flush(current_workflow_path())` |
| `restoreAutosave(std::string*)` | **编排逻辑迁 app 层恢复弹窗**（`do_restore` lambda，见 §2.2） |

- `editor_state.h`：删除上述声明与 PC-01 注释块；文件头编排清单移除 autosave。
- `editor_state.cpp`：删除上述定义；移除 `#include "autosave.h"`（本 TU 不再用 `engine::autosave_*`）；`clearWorkflow` 基线直连。

### 2.2 `restoreAutosave` 编排迁 app 层（行为等价）

恢复弹窗（app.cpp，唯一调用方）新增 `do_restore` lambda，逐字承接原逻辑：

1. `engine::autosaveLoad(loaded, &load_error)` 失败 → `log::error` + `setStatus("恢复失败：" + ...)` + 返回 false（原签名经 `error` 出参，弹窗分支 `setStatus` 语义不变）。
2. `abortRunIfAny("恢复自动保存")` → `model().load(std::move(loaded))`（整图替换 + 清撤销栈 + 逐节点 diff 事件，ARCH-E3）→ `selection().clear()`（ARCH-E4）→ `request_navigate_to_content = true`。
3. PC-02：`workflow_file.set_restored_from_autosave_path(autosave_info.workflow_path)` + `set_current_workflow_path("")`（恢复后必走另存为）。
4. `app_ctx().autosave.mark_baseline()`（恢复内容 = 当前状态，避免立刻再写同一快照）→ 日志 + `setStatus` 文案不变。
5. 成功后 `autosave.pause(false)` + 关弹窗。

**与拆出前唯一实现差异**：快照关联路径改取启动时 probe 的 `autosave_info.workflow_path`（原方法内部重新 `engine::autosaveProbe()`）。弹窗期间 autosave 处于暂停态，快照文件不可变 ⇒ 两次 probe 结果必然一致，行为等价。

### 2.3 调用点汇总

- `app.cpp`：启动放行（`mark_baseline` + `pause(false)`）· 主循环 tick · 恢复弹窗（`do_restore` + 两个拒绝分支）· 退出 flush。
- `cli_selftest.cpp`：编辑订阅断言直连 `app_ctx().autosave.dirty()` / `mark_baseline()`（断言逻辑与打印文案不变）。
- `editor_state.cpp`：`clearWorkflow` 内部基线直连。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `source/build.ps1` | ✅ 构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ **PASS** —— 编辑订阅断言照常（基线不置位 / Selection 不置位 / 内容置位），其余全部既有断言原样通过 |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `tools/check_all.ps1` | ✅ **19/19 全 PASS** |
| `check_links.py` | ✅ **1839 / 0** |

**零断言增减**：`--run-selftest` 全部既有断言（含 autosaveDirty）原样通过，未新增未删除 —— 仅断言访问路径从 `state.autosaveDirty()` 改为 `state.app_ctx().autosave.dirty()`。

**残留清扫**：`rg tickAutosave|pauseAutosave|markAutosaveBaseline|autosaveDirty|flushAutosaveOnExit|restoreAutosave`（source/）仅命中 app.cpp 一处注释（说明移除），无代码引用。

## 4. 过渡登记

- 本批移除的是 **A1 登记的「EditorState 保留同签名薄封装」中的 autosave 部分**；工作流文件薄封装（`saveWorkflowTo` / `openWorkflowFrom` / `recentWorkflows` / `archiveRunOutputs`）按计划保留至 A3。
- 「恢复快照」编排落 app 层恢复弹窗 —— 与目标态（app/main 持有各服务对象并编排）一致；A3 后 `AutosaveService` 依 §1.8.11 第 8 位由 AppContext 构造。
- 六服务仍为 **ui 层公共 API**（A1 已登记 FREEZE §5 / ARCHITECTURE §8 接口冻结例外，本批未新增签名）。

## 5. 回滚点

`git checkout -- source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/ui/src/app.cpp source/app/src/cli_selftest.cpp docs/refactor/ARCHITECTURE.md docs/refactor/FREEZE.md docs/CHANGELOG.md docs/DocMAP.md` 即回滚到 A1 形态（保留 `docs/audit/audit_arch_a2.md` 仅作登记；删除即可）。
