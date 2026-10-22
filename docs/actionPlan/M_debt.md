# M_debt：债务清零行动计划（当前主线 · 打磨四阶段前置）

> 类型：**跨里程碑债务清零专项**（不占用 M1–M8 / POL-* 编号；独立成文，与 [M_polish.md](M_polish.md) / [M_code_cleanup.md](M_code_cleanup.md) / [M_patchAB_rest.md](M_patchAB_rest.md) 平级）
> 动机：**2026-10-21 用户拍板** —— 「现在优先解决所有债务问题」：先把全部债务盘点、清零，再继续 M_polish 四阶段打磨。
> 编号空间：任务 **`DEBT-*`**（债务总账条目 · 权威位 = [../audit/DEBT.todo](../audit/DEBT.todo)）· 人工验证项承接位 = [../DevPlan.todo](../DevPlan.todo)（**四、债务清零人工验证**组）
> 状态：🟢 **已立项（2026-10-21）** · 债务总账已建立（6 类 / 26 项）· 批次见 §2
> 相关：打磨主线 [M_polish.md](M_polish.md)（四阶段 · 债务清零完成后回归）· 代码清理 [M_code_cleanup.md](M_code_cleanup.md)（CLN 主体已清零）· 补丁残项 [M_patchAB_rest.md](M_patchAB_rest.md) · M6 收口 [M6.md](M6.md) · 长期路线图 [../roadmap.md](../roadmap.md)

---

## §1 债务全景（2026-10-21 盘点）

> 全量清单与逐项状态见 [../audit/DEBT.todo](../audit/DEBT.todo)（与 DevPlan 同格式看板）。下表为摘要。

| 类别 | 编号空间 | 项目数 | 构成 | 可离线 | 需人工 |
|---|---|---|---|---|---|
| 一、代码结构债 | `DEBT-C*` | 5 | CLN 主体 **✅ 已清零**（C0 勾账）+ 小地图 mode 5 复核（C1）+ webview/dom_web 拆分（C2）+ ce_repro 并入自检（C3）+ 观察点（C4） | C2/C3/C4 | C1 |
| 二、逻辑/功能残项债 | `DEBT-L*` | 11 | PB2-29 逐站回填（L1）· 会话失效复核（L2）· 真 Key 生成（L3）· PB2-24 点选（L4）· PC-01/02 点选（L5）· 企业网络实测（L6）· VB-03 断言（L7）· M6-04 重试/失败汇总（L8）· M6-06 校验边界（L9）· M6-07 启动测量（L10）· M6-05 UI 细节（L11） | L7/L8/L9/L11 | L1/L2/L3/L4/L5/L6/L10 |
| 三、验证/人工验收债 | `DEBT-V*` | 6 | DevPlan.todo Test 〇/一/二组剩余 + DevPlan Human 未迁项 + M6-10 示例 + roadmap 人工项 | — | **全部**（承接 DevPlan.todo Test） |
| 四、打包/发布债 | `DEBT-P*` | 2 | 安装包冒烟（P1）· README 截图/视频（P2） | — | **全部** |
| 五、文档债 | `DEBT-D*` | 1 | DOC-M1-01 取证后更新（D1 · 依赖 Key） | — | **全部** |
| 六、已知 bug 债 | `DEBT-B*` | 1 | ⚠ Ctrl+S 保存失败（B1 · ★高优先级 · 真实 bug 记录） | 先排查 | 复核 |
| **合计** | — | **26** | 可离线 ≈ 8 · 需人工 ≈ 18 | 8 | 18 |

**关键结论**：
1. **代码债主体已清零**（CLN-01~15 + 驼峰化 8 批，2026-10-20 全部落地）—— 剩余代码债仅 4 项（1 项需人工复核，3 项低优先级）。
2. **债务大头是「人工验证」**：18/26 项需要真人 / 真机 / Key / 真实站点 —— 全部登记 [../DevPlan.todo](../DevPlan.todo)「四、债务清零人工验证」组。
3. **唯一已知真实 bug**：Ctrl+S 保存失败（DevPlan.todo Test #457 · 原 ToVerify #305 子项）—— 债务清零的第一优先。

---

## §2 执行批次

> 原则与 M_polish §4 一致：每批小步、每批验证、先立基线、只升不降、语义等价、人工项如实登记。

### 批 A · 离线可做（约 8 项 · 不需要真机）

> **🟢 批 A 状态（2026-10-21）**：8 项中的 **7 项已完成**（A-1 ✅ 先排查段 · A-2/3/4/5/7/8 ✅ 本批落地）· A-6 = 离线代码部分 ✅、视觉复核顺延 M_polish 阶段二（半人工）。构建 **0/0** · exec **83+91+691 / 0** · graph **151 / 0** · run-selftest PASS · check_all 全绿 · check_links **1717/0**。详单见 [CHANGELOG.md](../CHANGELOG.md) 2026-10-21 · 🧮。

| 序号 | 债务项 | 内容 | 验证 |
|---|---|---|---|
| A-1 | `DEBT-B1`（先排查段） | ✅ Ctrl+S 保存失败：快捷键链路修复（`compositeEditorSave`）→ GUI 复核登记 DevPlan.todo Test | 构建 0/0 + selftest 全绿 ✅ · GUI 复核走 DevPlan.todo Test（待做） |
| A-2 | `DEBT-L7` | ✅ VB-03「假增量」离线断言补上（4 断言 · exec 682→686） | `exec-selftest` 686/0 ✅ |
| A-3 | `DEBT-C3` | ✅ check_all 补 `aiwrite --run-selftest` + `ce_repro` mode 0 回归 | check_all 全绿 ✅ |
| A-4 | `DEBT-L8` | ✅ 失败汇总纯函数 + UI 接线（exec 686→691）；重试已有断言 | 错误路径断言 691/0 ✅ + GUI 复核顺延 |
| A-5 | `DEBT-L9` | ✅ 校验 5 边界补强（executor 73→83） | 边界用例断言 83/0 ✅ |
| A-6 | `DEBT-L11` | 🟡 离线代码部分 ✅（`io.FontDefault` 显式 + 主题审计）；视觉复核待做 | 视觉复核（半人工 → M_polish 阶段二） |
| A-7 | `DEBT-C2` | ✅ 拆分评估：两文件仍高耦合无接缝 ⇒ 维持不拆（行为等价） | 构建 0/0 + 行为等价 ✅ |
| A-8 | `DEBT-C4` | ✅ `##ce_inline_` → `##node_inline_` 去借名 | 构建 0/0 ✅ |

### 批 B · 人工验证（18 项 · 全部登记 DevPlan.todo Test 四组）

| 债务项 | 对应 DevPlan.todo Test 四组条目 | 前置 |
|---|---|---|
| `DEBT-C1` mode 5 小地图 GUI 复核 | 四组 · 小地图导航 | 构建 app |
| `DEBT-L1` PB2-29 逐站回填（6 站） | 四组 · 逐站回填（6 子项） | 真实登录态 |
| `DEBT-L2` 会话失效复核 | 四组 · 会话失效 | 一次真实失效响应 |
| `DEBT-L3` 真 Key 生成 | 四组 · Key 门 | Anthropic / Gemini Key |
| `DEBT-L4` PB2-24 点选回路 | 四组 · PB2-24 点选 | GUI |
| `DEBT-L5` PC-01/02 点选 | 四组 · 崩溃恢复点选 | GUI |
| `DEBT-L6` 企业网络实测 | 四组 · 企业网络 | 企业网络环境 |
| `DEBT-L10` 启动测量 | 四组 · 启动测量 | GUI + 计时 |
| `DEBT-V1`~`V6` | 四组 · 对应条目 | 见各条目 |
| `DEBT-P1`/`P2` · `DEBT-D1` | 四组 · 对应条目 | 见各条目 |
| `DEBT-B1`（复核段） | 四组 · Ctrl+S 复核 | 修复后 |

> 勾账口径：人工项在 DevPlan.todo Test 勾 done 后，回填 DEBT.todo 对应条目 done=true + CHANGELOG。

---

## §3 与既有主线衔接

| 文档 | 关系 |
|---|---|
| [M_polish.md](M_polish.md) | 债务清零 = **阶段 0（前置）**；完成后回归四阶段（POL-L-02 与 DEBT-C1 同源，一次 GUI 复核可同时勾两条） |
| [M_code_cleanup.md](M_code_cleanup.md) | CLN 主体已清零；§4.1 方向 4/5 = DEBT-C2/C3 承接 |
| [M_patchAB_rest.md](M_patchAB_rest.md) | 逻辑残项（PB-07 / PB2-10/11/24/25/29 / PC-01/02 / VB-03）= DEBT-L1~L7 |
| [M6.md](M6.md) | 打磨小项（M6-04/05/06/07/10）= DEBT-L8~L11 / V5 |
| [../roadmap.md](../roadmap.md) | 阶段总览加「债务清零」当前主线行 |
| [../DevPlan.todo](../DevPlan.todo) | 四、债务清零人工验证组 = 人工项唯一勾账位（DevPlan.todo Test） |
| [../audit/DEBT.todo](../audit/DEBT.todo) | 债务总账权威位 |

---

## §4 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-10-21 | v1 | **立项**：用户拍板「优先解决所有债务」→ 债务总账 DEBT.todo（6 类 / 26 项）+ 本计划（批 A 离线 8 项 / 批 B 人工 18 项）+ roadmap / DevPlan.todo Test / M_polish 同步。**零代码改动（纯文档立项）。** |
