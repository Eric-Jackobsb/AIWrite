# audit_arch_r1：ARCH-R1 落地审计报告（RunSession 对象化）

> 批次：`ARCH-R1` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 1
> 性质：**运行域对象化** —— `Executor` + 快照从 `editor()` 拆出为独立 `RunSession`。
> 判据：行为等价（`--exec-selftest` 713/0 只升不降）；构建 0 error / 0 warning。

## 1. ARCH-D16 依赖图取证（拆分前）

> 依 `ARCH-D16`：拆分前补依赖图，先出图、确认无环再拆。以下取证完成于拆分动工前。

### 1.1 拆分对象与边界

| 项 | 值 |
|---|---|
| 拆分对象 | `EditorState` 运行域（`source/ui/src/editor_state.cpp` / `editor_state.h`） |
| 搬出成员 | `engine::Executor executor` · `engine::RunSnapshot run_snapshot` · `bool session_active_` · `snapshotEntry` |
| 搬出方法 | `startRun` / `startRunAsync` / `startRerunAsync` / `pumpRunEvents` / `stopRunAsync` / `cancelRun` / `tickRun` / `runStatusText` / `abortRunIfAny` / `finishRunSession`（快照落定段） |
| 产出 | `source/ui/include/run_session.h` + `source/ui/src/run_session.cpp`（新对象，会话级） |
| 保留（UI 收尾，归 EditorState） | `setStatus` 文案 · `archiveRunOutputs` · `refreshLastError` · TextOutput 自动导出 · 运行明细日志 |

### 1.2 运行域引用点清单（全量）

| 调用方 | 引用 | 改造 |
|---|---|---|
| `app.cpp:796,1273,1332` | `tickRun()` / `runStatusText()` / `stopRunAsync()` | 保留 EditorState 委托方法 |
| `toolbar.cpp:101,153,173,189,192,257,264,277` | `executor.running()` / `startRunAsync()` / `startRerunAsync()` / `cancelRun()` | `executor` → `executor()` 访问器 |
| `property_panel.cpp:639,725,737,752,755,758` | `run_snapshot_view()` / `executor.running()` / `session_active()` / `startRunAsync()` / `startRerunAsync()` | `executor` → `executor()` 访问器 |
| `node_canvas.cpp:926,1018,1020,1023` | `run_snapshot_view()` / `session_active()` / `startRerunAsync()` | 保留访问器 / 委托方法 |
| `error_bar.cpp:65,73` | `session_active()` / `startRerunAsync()` | 保留 |
| `output_panel.cpp:216,279,280` | `run_snapshot_view()` / `executor` | `executor` → `executor()` |
| `cli_selftest.cpp:268~648`（30 处） | `startRun()` / `tickRun()` / `runStatusText()` / `startRunAsync()` / `session_active()` / `run_snapshot_view()` / `stopRunAsync()` / `executor`（直接全套） | `state.executor` → `state.executor()` |
| `ce_run.cpp:61` | `editor().session_active()` | 保留（复合编辑器只读会话态） |

### 1.3 依赖图（确认无环）

```
外部调用方（10 个 TU）
  └─▶ EditorState（编排 + UI 收尾）
        ├─ 持有 RunSession（成员 run）
        │     ├─ 持有 engine::Executor（ARCH-D20）＋ engine::RunSnapshot
        │     ├─ 依赖 engine::validateBeforeRun（ARCH-D27）＋ engine::makeSnapshot
        │     ├─ 依赖 nodes::registerAllExecutors / log（全局，无环）
        │     └─ 方法参数收 Graph&（校验 / 状态写回 / 权威快照）——不持有 Graph（ARCH-D25 内拷贝为后续批）
        └─ 读 run.executor() / run.snapshot() 做归档 / 错误条 / 自动导出
              └─▶ engine（executor / graph / validate）——engine 不依赖 ui ⇒ 无环 ✓
```

**无环结论**：`RunSession` 不引用 `EditorState`（Graph 走方法参数）；`EditorState → RunSession → engine` 单向；engine 不依赖 ui。**确认无环，可拆。**

## 2. 拆分方案

- **`RunSession`（ui 层，会话级）**：持有执行器 + 读模型快照 + 会话生命周期 + 事件泵 + 状态栏文本。
- **EditorState 保留对外签名**（`startRunAsync` / `tickRun` / `runStatusText` 等），内部委托 `run`；新增过渡期访问器 `executor()`（`cli_selftest` 等直接读运行态）。
- **UI 收尾留在 EditorState**：状态栏文案、运行明细日志、归档、错误条、TextOutput 自动导出（后续批 `ARCH-R2/3` 再按事件/ReadOnlyView 迁移）。
- **零行为变更**：搬移逐字保留函数体；仅 `run_snapshot` 读写改经 `run.snapshot()` / `run.executor()`。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（CLI 自检全绿） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `api_probe.exe --selftest` | ✅ 十三组全 PASS（SHA3 / HTTP / 请求构造 / 配置往返 / 会话与可见性 / 执行器 / 工作流文件 / 无 Key / 取消与超时 / L2 请求构造 / L2 工厂分派 / L2 回环 / 网络与凭据） |
| `check_links.py` | ✅ **1755 / 0**（基线持平） |

## 4. 回滚点

删除 `run_session.h` / `run_session.cpp`，`git checkout -- source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/CMakeLists.txt source/app/src/cli_selftest.cpp source/ui/src/output_panel.cpp source/ui/src/property_panel.cpp source/ui/src/toolbar.cpp` 即回滚到拆分前形态。
