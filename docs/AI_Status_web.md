# AI_Status_web · 网页版通道状态（11 站）

> **用途**：只看**网页版**这一条线（DOM 自动化 10 站 + 协议型适配器 `deepseek-web` 1 站）——「哪站能匿名用 / 哪站要登录 / 哪站选择器要回填 / 图片上传接到哪了」。
> **配套**：[AI_Status.md](AI_Status.md)（项目总览：门禁基线 / 循环轮次 / 下一步）· [AI_Status_api.md](AI_Status_api.md)（官方 API 分片）· [loop_dev.md](loop_dev.md)（循环规则 · S2/S3 判据权威位）· [../source/assets/providers.json](../source/assets/providers.json)（声明与接线）。
> **维护**：**§二 两张表机器生成** —— `loop_verify.ps1` 每轮结束 / `-StatusOnly` 回写哨兵块，**人工只审阅**（改数据请改 `source/assets/providers.json` / `source/assets/site_capability_evidence.json`）；§三 / §四 由 AI 协作者在每轮收尾时滚动更新。
> **最近更新**：2026-10-10（**移除重复条目 `tongyi-web`**（与 `qwen-web` 重复）⇒ web **12 → 11 站** · 同日 `T-L26` 全门禁轮复核；此前：从 [AI_Status.md](AI_Status.md) §二按 `kind=web` 拆出 · `TST-L05`）

---

## 一、这一条线怎么走（先读这个）

1. **S2 匿名探测**（只读、不起生成）：探「输入框 / 发送键在不在」⇒ 记 `S2 探测` 与 `输入命中/候选`。
2. **登录态**：**匿名优先**（loop_dev §3）；探到需登录的站**不**在脚本里自动登录 ⇒ 人工在窗口内登录一次后重跑 S3。
3. **S3 端到端**（文本 → 取回文本）：绿判据 = **三口径**（`rc=0` + `n3=done` + 归档正文非站侧错误 / 忙碌串）。
4. **图片理解**（图片 → 取回文本）：需要 `web.upload_adapter`（图片上传适配器）+ `attach_selector` 侦察 ⇒ **未接线**的站在「VLM + 网页版」组合下**会明确报错**，不是静默失败。

> 判据的**权威位** = [loop_dev.md](loop_dev.md) §3（匿名优先 8 步）/ §3.2（超时）/ §5.1（失败四分类）；本文件只做**分站台账**（「哪站什么状态」）。
> `deepseek-web` 是**协议型适配器**（同源协议请求，不走 DOM 选择器）⇒ 它的「S2/S3 物证」与其余 10 站不同源，`接入` 列已区分。

## 二、网页版台账（11 站 · 机器生成 · 跨轮累积）

<!-- SITE-STATUS:BEGIN （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->
> 生成：`loop_verify.ps1`（TST-L05）· 时间 2026-10-16 · 覆盖轮次 0：
> **范围**：`kind=web` 的 **11 站**（DOM 自动化 10 站 + 协议型适配器 `deepseek-web` 1 站）· **文字理解**判据 = S3 **三口径**（`rc=0` + `n3=done` + 归档正文非站侧错误/忙碌串）；**图片理解**判据 = 站点上传双证据 + 取回正文。
> **图例**：``✅ 实测``（有真机物证）· ``🟡 就绪/声明·未实测`` · ``⬜ 未接/未测/未声明`` · ``⚠ 不可用或不一致``
> 来源 = ``providers.json``（声明/接线）× ``site_capability_evidence.json``（实测证据）× ``build/loop/*/report.json``（轮次物证），**冲突时实测优先并在列内标 ⚠**。

**汇总（web）**：共 11 站 · 文字理解 ✅ 实测 **7** · 图片理解 ✅ 实测 **1** / 🟡 就绪 1 / ⬜ 未接 9 · 归因口径：需登录 0 · 站侧忙碌 0

**表 A · 链路证据（S2 匿名探测 → S3 端到端）**

| # | id | 接入 | 最近轮次 | S2 探测 | 输入命中/候选 | 登录证据 | S3 rc | n3 | 正文码点 | 最终态 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `spark-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 2 | `gemini-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 3 | `doubao-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 4 | `deepseek-web` | web·deepseek-web | — | — | — | — | — | — | — | ✅ 已实测（协议型适配器） |
| 5 | `qwen-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 6 | `claude-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 7 | `yuanbao-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 8 | `chatgpt-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 9 | `ernie-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 10 | `kimi-web` | web·dom | — | — | — | — | — | — | — | 未测 |
| 11 | `chatglm-web` | web·dom | — | — | — | — | — | — | — | 未测 |

**表 B · 能力与接线（文字理解 / 图片理解 · 下一动作）**

| # | id | 文字理解 | 图片理解 | 上传适配器 | 选择器 in/ans | 依据（图片理解） | 下一动作 |
|---|---|---|---|---|---|---|---|
| 1 | `spark-web` | ⬜ 未测 | ⬜ 未接 | — | N/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 2 | `gemini-web` | ✅ 实测 | ⬜ 未接 | — | N/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 3 | `doubao-web` | ✅ 实测 | ✅ 实测 | `builtin:doubao` | Y/N | B3 真视觉（256×256 纯红 → 描述正确）+ upload_adapter=builtin:doubao 双证据 PASS（协议记录 §8.3/§9/§10.2） | S2 探到「可用」后跑 S3 |
| 4 | `deepseek-web` | ✅ 实测 | ⬜ 未接 | — | N/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | — |
| 5 | `qwen-web` | ⬜ 未测 | ⬜ 未接 | — | Y/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 6 | `claude-web` | ⬜ 未测 | ⬜ 未接 | — | N/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 7 | `yuanbao-web` | ✅ 实测 | ⬜ 未接 | — | Y/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 8 | `chatgpt-web` | ⬜ 未测 | ⬜ 未接 | — | Y/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 9 | `ernie-web` | ✅ 实测 | ⬜ 未接 | — | Y/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |
| 10 | `kimi-web` | ✅ 实测 | 🟡 就绪·未实测 | `builtin:kimi` | Y/N | 接线 builtin:kimi · attach_selector 待侦察/待真机 | 侦察 attach_selector → 真机一次 |
| 11 | `chatglm-web` | ✅ 实测 | ⬜ 未接 | — | N/N | 无 web.upload_adapter（VLM + 网页版组合会明确报错） | S2 探到「可用」后跑 S3 |

> 重建（零副作用，随时可跑）：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly`
<!-- SITE-STATUS:END -->

## 三、这一条线的常见卡点（人工维护）

1. **候选态假「可用」**：S2 报「可用」但 `输入命中/候选 = 0/1` ⇒ 大概率 `web.input_selector` 未回填（`chatglm-web` / `spark-web` / `chatgpt-web` / `claude-web` / `gemini-web` 均为 `N`）。
2. **站侧忙碌 ≠ 成功**：`n3=done` 但正文是站侧错误串（如 `System is currently busy`）⇒ 按 loop_dev §10.1 陷阱 6 判**失败**，复测 2~3 次定性（偶发 / 限流）。
3. **需登录**：`spark-web` 一类站只能人工在窗口内登录一次再重跑 S3（凭证只在进程内存流转，不入库、不进日志）。
4. **图片链路**：`doubao-web` 已实测（`builtin:doubao` 双证据）；`kimi-web` 已接线待真机（`attach_selector` 待侦察）；其余**未接** ⇒ 按站点上传模板（`actionPlan/M8.md` §13）补适配器。
5. **协议型 vs DOM**：改 `deepseek-web` 要动协议适配器；改其余 10 站要动选择器 / 等待策略 —— 别混改。

## 四、纪律（别踩）

1. **改数据只改两处**：`source/assets/providers.json`（声明 / 接线）× `source/assets/site_capability_evidence.json`（实测证据）⇒ 表格由脚本重建，**不要手改表格**。
2. **声明 ≠ 实测**：`web.input_selector` / `web.upload_adapter` 是**声明**；只有 S2 / S3 与上传双证据才算「实测」（loop_dev §10.1 陷阱 7）。
3. **看结论先看物证**：`build/loop/**` 随 `build/` 易失 ⇒ 结论要落到 [CHANGELOG.md](CHANGELOG.md) + 本文档 + [loop_dev.md](loop_dev.md) §11。
4. **⛔ 不擅自 `commit` / `tag` / `push` / 改版本号**（[../AI_README.md](../AI_README.md) §6）。
