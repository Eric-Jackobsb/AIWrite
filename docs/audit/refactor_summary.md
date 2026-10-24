# 重构审计摘要（`ARCH-R/E/A/Z` · 过程审计已归档）

> 类型：**摘要索引**（不是新审计）。原文 20 份 `audit_arch_*` 已于 2026-10-24 迁至
> [../Archive/audit/](../Archive/audit/)（`git mv` 保留历史）；当前结论以
> [audit_codebase_20261023.md](audit_codebase_20261023.md) 与 [../refactor/ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §4 为准。
> 状态：**重构 19/19 批全部落地 · 8/8 验收判据通过 · 重复率 2.98 % · 生产文件 0 个 > 800 行**（2026-10-23）。

## 一、批次一览

| 阶段 | 批次 | 结论 | 原始审计（已归档） |
|---|---|---|---|
| 1 运行侧事件化 | `ARCH-R1` ~ `ARCH-R4` | ✅ 4/4（RunSession / RunEvent 订阅 / ReadOnlyView / 命令路由） | [r1](../Archive/audit/audit_arch_r1.md) · [r2](../Archive/audit/audit_arch_r2.md) · [r3](../Archive/audit/audit_arch_r3.md) · [r4](../Archive/audit/audit_arch_r4.md) |
| 2 编辑侧事件化 | `ARCH-E1` ~ `ARCH-E6` | ✅ 6/6（EventBus / Undo+autosave 订阅 / WorkflowModel / SelectionState / CanvasInteractor / 打包下沉） | [e1](../Archive/audit/audit_arch_e1.md) · [e2](../Archive/audit/audit_arch_e2.md) · [e3](../Archive/audit/audit_arch_e3.md) · [e4](../Archive/audit/audit_arch_e4.md) · [e5](../Archive/audit/audit_arch_e5.md) · [e6](../Archive/audit/audit_arch_e6.md) |
| 3 应用级收口 | `ARCH-A1` ~ `ARCH-A7` | ✅ 7/7（AppContext / AutosaveService / `editor()` 退场 / 依赖单向 / 全局变量 / ui 状态对象化 / web+utils 状态归零） | [a1](../Archive/audit/audit_arch_a1.md) ~ [a7](../Archive/audit/audit_arch_a7.md) |
| 4 收尾 | `ARCH-Z1` · `ARCH-Z2` | ✅ 2/2（14 个超长文件拆分 + 去重 6 步 / `CeSession` 五对象 + `CeReadOnlyView`） | [z1](../Archive/audit/audit_arch_z1.md) · [z1_web](../Archive/audit/audit_arch_z1_web.md) · [z2](../Archive/audit/audit_arch_z2.md) |

## 二、验收判据（8/8）

行为等价 · 全局变量归零 · 双画布单套交互 · 打包-校验同源 · 依赖单向 · `editor()` 退场 ·
`ReadOnlyView` 落地 · `AppContext` 运行时不可访问 —— 逐条证据见
[audit_codebase_20261023.md](audit_codebase_20261023.md) §1 与
[../Archive/audit/audit_arch_z1.md](../Archive/audit/audit_arch_z1.md) §11。

## 三、当前门禁（权威位 = [../../AI_README.md](../../AI_README.md) §2）

构建 0 error / 0 warning · `check_all` 21/21 · exec 713/0 · graph 151/0 · upload 72/0 ·
`check_includes` / `check_globals` 违规 0 · 文档断链 0 · 重复率 2.98 %。
