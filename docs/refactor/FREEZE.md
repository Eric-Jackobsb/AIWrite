# 冻结声明 · FREEZE —— 功能 / 模块冻结（重构前置）

> 日期：2026-10-21 · 状态：🟢 **已冻结（用户拍板）** · 编号空间：`FREEZE-*`
> 权威位：本文 + [DocMAP.md](../DocMAP.md) §一 冻结行
> 背景：审计 `audit/audit_architecture_20261021.md`（混乱度 4/5）认定 —— **功能可用但结构呈「单例总线 + 双画布双轨」形态，继续叠功能风险持续累积**。用户拍板：**先冻结功能与模块，重新设计清晰架构，再执行重构**。

---

## §1 冻结范围

| 维度 | 冻结内容 | 例外 |
|---|---|---|
| **功能** | 不再新增节点类型 / 不再新增面板 / 不再新增快捷键 / 不再新增配置项 / 不再扩展协议 | 仅修 bug（含两个已知 bug：Ctrl+S、暴露节点保存） |
| **模块** | 不再新增源文件 / 不再新增目录 / 不改变现有模块边界 | 重构批本身产出的新文件（走 `ARCH-*` 批） |
| **接口** | 不新增公共 API（engine/include、ui/include 头文件签名） | 重构批次内的内部重构允许，但对外符号须保持或显式登记变更 |
| **文档** | 不再新增里程碑 / 功能计划文档 | 重构专项文档（`docs/refactor/`）与登记性更新 |

## §2 冻结规则

1. **一切功能开发暂停**：任何「想做新功能」的诉求先登记到 `DevPlan.todo` 的 `📦 冻结待解冻` 组，不进入实施。
2. **bug 修复走轻量通道**：允许修，但每次修复后须跑回归基线（见 §4），且**不得扩大**到顺手重构。
3. **重构是本阶段唯一主线**：所有开发力量投入 `ARCHITECTURE.md` 定义的目标架构落地。

## §3 解除条件

冻结在**架构定稿 + 重构完成 + 回归基线全绿**后解除；解除动作 = 本文状态改为 🔵 已解除 + DocMAP 更新 + DevPlan.todo 冻结组转入可实施。

## §4 回归基线（每次改动后必跑）

- 构建：`source/build.ps1` → 0 error / 0 warning
- 自检：`api_probe --selftest`、`--exec-selftest`（713/0）、`--graph-selftest`（151/0）
- 链路：`tools/check_all.ps1`（含 `--run-selftest`、`ce_repro`、文档断链 1768/0）
- 原则：**只升不降** —— 每批落地后断言数只增不减

**冻结生效。**

---

## §5 进度记录

| 日期 | 事件 | 说明 |
|---|---|---|
| 2026-10-21 | 冻结生效 | 本文建立 · 用户拍板「先冻结功能与模块，重新设计清晰架构，再执行重构」 |
| 2026-10-22 | **架构定稿（v1.0）** | [ARCHITECTURE.md](ARCHITECTURE.md) **v1.0 定稿**（用户拍板 19 项决策 `ARCH-D1`~`D19` · 迁移路线 = ARCH-R/E/A/Z 四阶段）· **冻结继续生效** · 解除条件 = 重构完成（四阶段全落地）+ 回归基线全绿（§3） |
| 2026-10-22 | **架构定稿（v1.1 · 当前）** | [ARCHITECTURE.md](ARCHITECTURE.md) **v1.1 定稿**（补充 §1.8 对象定义总表 —— 10 个对象一句话定义 + 依赖图 + 注入示例 + `rg` 断言 · 新增 25 条决策 `ARCH-D20`~`D44` · 验收判据 +1 = 第 8 条 `AppContext` 运行时不可访问）· **冻结继续生效** |
| 2026-10-22 | **重构批 `ARCH-R1` 落地** | [ARCHITECTURE.md](ARCHITECTURE.md) 迁移路线阶段一第一批 —— **`RunSession` 对象化**：Executor + 快照从 `editor()` 拆出（新增 `run_session.h/.cpp` · 运行域委托 + UI 收尾留在 EditorState · 过渡期访问器 `executor()`）· **ARCH-D16 依赖图取证**先出图确认无环（[审计](../audit/audit_arch_r1.md)）· 验证 = 构建 0/0 · `--run-selftest` PASS · exec **713/0** · graph **151/0** · selftest 十三组 PASS · check_links **1755/0** · **冻结继续生效** |
| 2026-10-22 | **重构批 `ARCH-R2` 落地** | [ARCHITECTURE.md](ARCHITECTURE.md) 迁移路线阶段一第二批 —— **RunEvent 订阅化**：`RunSession` 事件队列扩展为订阅模式（`subscribeRunEvents` / `unsubscribeRunEvents` · pumpEvents 主线程 FIFO 分发 · **复用现有队列**）· `EditorState` 构造订阅 + **删除 `pumpRunEvents()`**（UI 不再主动 pump）· 输出面板订阅 → 流式跟随滚动（PB-03）· 状态栏订阅 → 结束摘要缓存（[审计](../audit/audit_arch_r2.md)）· 验证 = 构建 0/0 · `--run-selftest` PASS（新增「订阅通知」断言）· exec **713/0** · graph **151/0** · check_links **1768/0** · `rg pumpRunEvents` **0 处** · **冻结继续生效** |
| 2026-10-22 | **重构批 `ARCH-R3` 落地** | [ARCHITECTURE.md](ARCHITECTURE.md) 迁移路线阶段一第三批 —— **`ReadOnlyView` 渲染视图落地**：新增 `source/ui/include/readonly_view.h`（每帧主循环**统一构建一次** = `ARCH-D30` · 字段 = 三个核心 + 配置 = `ARCH-D31` · 快照空对象 = 未运行 = `ARCH-D32` · 渲染签名 `render(const ReadOnlyView&)` = `ARCH-D33` · 零拷贝访问器 `nodes_for_render`/`edges_for_render` = `ARCH-D34` · 使用者 = 画布/参数面板/输出面板/状态栏/信息面板 = `ARCH-D35`）· 渲染读取统一走 view，交互写仍走 editor()（E3/E4 拆分后收敛）（[审计](../audit/audit_arch_r3.md)）· 验证 = 构建 0/0 · `--run-selftest` PASS（新增「ReadOnlyView」零拷贝/同源断言 = ARCH-D18）· exec **713/0** · graph **151/0** · ce_repro **全帧 OK** · check_links **1768/0** · `render(const ReadOnlyView&)` **覆盖全部渲染模块** · **冻结继续生效** |
| 2026-10-22 | **重构批 `ARCH-R4` 落地** | [ARCHITECTURE.md](ARCHITECTURE.md) 迁移路线阶段一第四批 —— **命令路由层**：新增 `source/ui/include/commands.h` + `source/ui/src/commands.cpp`（`AppCommand` 命令全集 + `dispatchCommand` **单一实现** + `ShortcutBinding` **快捷键表** + `handleShortcut` 表驱动分派）；**三入口收敛**（§2.1 三入口行为漂移关闭）：菜单（文件/编辑/上级节点/帮助）/ 工具栏（撤销/重做/删除/新建/示例/打开/保存）/ 快捷键（主画布 Delete · ce Delete/Ctrl+S）统一经 `dispatchCommand`；**修 Ctrl+S**（主窗口 Ctrl+S = 保存工作流；ce 场景归保存上级节点 = `compositeEditorWindowFocused`）；ce 顶栏「保存」按钮收敛到 `dispatchCommand(CompositeSave)`（与 Ctrl+S 同一路径 = DEBT-B1 语义）；范围边界：视图菜单与运行/停止不入命令层（[审计](../audit/audit_arch_r4.md)）· 验证 = 构建 0/0 · `--run-selftest` PASS（新增「命令路由」断言）· exec **713/0** · graph **151/0** · ce_repro **全帧 OK** · check_links **1774/0** · check_all **19/19** · **冻结继续生效** |

