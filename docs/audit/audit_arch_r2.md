# audit_arch_r2：ARCH-R2 落地审计报告（RunEvent 订阅化）

> 批次：`ARCH-R2` · 日期：2026-10-22 · 计划：docs/refactor/ARCHITECTURE.md §6 阶段 1
> 性质：**运行侧事件化第二批** —— RunEvent 订阅化：输出面板 / 状态栏改为订阅 `RunEvent`，UI 不再主动 `pumpEvents()`。
> 判据：行为等价（`--exec-selftest` 713/0 只升不降）；构建 0 error / 0 warning。

## 1. 目标形态（ARCHITECTURE.md §1.3 / §1.8.2 / 决策）

| 决策 | 结论 | 本批落点 |
|---|---|---|
| `ARCH-D7` | 事件系统形态 1：**通知**（消费者各自处理） | 订阅回调只更新「变化通知」字段 |
| `ARCH-D10` | RunEvent **复用现有队列**，扩展为订阅模式 | `RunSession::subscribeRunEvents`（不新建队列） |
| `ARCH-D13` | RunEvent 订阅化：**接受**（UI 改为订阅，不再主动 pump） | 删 `EditorState::pumpRunEvents()`；输出面板/状态栏读订阅字段 |

## 2. 改动清单

### 2.1 `RunSession`（订阅 API —— 复用现有队列扩展订阅模式）

- `run_session.h`：新增 `using RunEventCallback` + `subscribeRunEvents(callback)` / `unsubscribeRunEvents(handle)`；私有 `run_subscribers_`（句柄→回调）+ `next_sub_id_`；私有 `dispatchRunEvent`。
- `run_session.cpp`：
  - `pumpEvents()` 在**每个事件应用后**（switch 之后、循环内）调用 `dispatchRunEvent(event)` —— 主线程 FIFO 分发（形态 1：通知）。
  - `dispatchRunEvent`：**拷贝回调列表再遍历**（回调内 subscribe/unsubscribe 不破坏迭代）。
  - `subscribeRunEvents` / `unsubscribeRunEvents` 实现。

### 2.2 `EditorState`（UI 改为订阅，不再主动 pump）

- `editor_state.h`：新增构造函数（订阅）/ 析构函数（注销）；公开订阅通知字段 `follow_output_` / `follow_node_id_` / `run_finished_` / `run_last_summary_`；私有 `onRunEvent` + `run_sub_handle_`；**删除 `pumpRunEvents()` 声明**。
- `editor_state.cpp`：
  - 构造：`run.subscribeRunEvents([this](.){ onRunEvent(event); })`。
  - 析构：`run.unsubscribeRunEvents(run_sub_handle_)`。
  - `onRunEvent`：`NodeOutput/Delta` → `follow_output_=true` + 记节点；`Finished` → 停跟随 + 缓存 `run_last_summary_`。
  - `startRunAsync` / `startRerunAsync`：新会话开始重置订阅通知。
  - **删除 `pumpRunEvents()` 定义**（全库引用归零）。

### 2.3 输出面板（订阅 RunEvent → 流式跟随滚动）

- `output_panel.cpp` `drawOutputPanel`：节点段渲染后，若 `follow_output_` 且 `follow_node_id_ == info.node_id` → `ImGui::SetScrollHereY(1.0f)` 并清除标记（PB-03 流式呈现：运行中自动跟随最新输出）。渲染仍直接读快照（规则 3）。

### 2.4 状态栏（订阅 RunEvent → 结束摘要缓存）

- `app.cpp` 状态栏：`runStatusText()` 为空（executor 已被 reset）时，若 `run_finished_` 则显示 `run_last_summary_` —— 切图/退出后「上次运行」摘要仍可见。

### 2.5 自检（只升不降）

- `cli_selftest.cpp` PB-01 第二步新增断言 `subscribed_notify`（startRunAsync 重置 → Finished 事件置位 `run_finished_`），打印「订阅通知=是/否」。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login） |
| `aiwrite.exe --run-selftest` | ✅ **PASS**（含新增「订阅通知=是」断言） |
| `api_probe.exe --exec-selftest` | ✅ **713 通过 / 0 失败**（基线持平，只升不降） |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `check_links.py` | ✅ **1768 / 0**（基线 1755 → 1768，新增文档链接） |
| `rg pumpRunEvents` | ✅ **0 处**（UI 不再主动 pumpEvents） |

## 4. 回滚点

`git checkout -- source/ui/include/run_session.h source/ui/src/run_session.cpp source/ui/include/editor_state.h source/ui/src/editor_state.cpp source/ui/src/output_panel.cpp source/ui/src/app.cpp source/app/src/cli_selftest.cpp docs/CHANGELOG.md docs/DocMAP.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 R1 形态（保留 `docs/audit/audit_arch_r2.md` 仅作登记；删除即可）。
