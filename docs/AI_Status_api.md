# AI_Status_api · 官方 API 通道状态（9 条）

> **用途**：只看**官方 API**这一条线（`openai` / `anthropic` / `gemini` 协议直连：不起浏览器、不走 DOM、不依赖登录态）——「哪条能跑 / 卡在协议未实现 / 卡在没 `api_base` / 卡在没 Key / 图片（vision）声明与实测差在哪」。
> **配套**：[AI_Status.md](AI_Status.md)（项目总览）· [AI_Status_web.md](AI_Status_web.md)（网页版分片）· [../source/assets/providers.json](../source/assets/providers.json)（协议 / `api_base` / `capabilities.vision` 声明）· [Archive/actionPlan/M7.md](Archive/actionPlan/M7.md)（`M7-10` vision 声明回填）。
> **维护**：**§二 两张表机器生成** —— `loop_verify.ps1` 每轮结束 / `-StatusOnly` 回写哨兵块，**人工只审阅**（改数据请改 `source/assets/providers.json` / `source/assets/site_capability_evidence.json`）；§三 / §四 由 AI 协作者滚动更新。
> **最近更新**：2026-10-10（从 [AI_Status.md](AI_Status.md) §二按 `kind=official` 拆出 · `TST-L05`）

---

## 一、这一条线怎么走（先读这个）

1. **协议已实现才谈得上「测」**：本版本实现 `openai` / `deepseek-web` / `dom` ⇒ `anthropic` / `gemini` **未实现**是该链路不可用（**不是** Key 配错）。
2. **就绪三件套**：`api_base` 已填 + Key 已配置（凭证库 `~/.brain-ai/**`，**不入库 / 不进日志 / 不进文档**）+ `capabilities.vision` 声明与实测一致。
3. **文字理解**：真机一次（HTTP 200 + 取回正文）；`providers.json` 的 `verified=true` 只算**声明级**佐证。
4. **图片理解**：多模态真机一次才算实测 —— `capabilities.vision=true` 只是**声明**（loop_dev §10.1 陷阱 7）。`deepseek` 就是「实测已过、声明未回填」的样本（待 `M7-10`）。

## 二、官方 API 台账（9 条 · 机器生成 · 跨轮累积）

<!-- SITE-STATUS:BEGIN （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->
> 生成：`loop_verify.ps1`（TST-L05）· 时间 2026-10-11 · 覆盖轮次 0：
> **范围**：`kind=official` 的 **9 条**（`openai` / `anthropic` / `gemini` 协议直连：不起浏览器、不走 DOM、不依赖登录态）· **文字理解**判据 = 真机 HTTP 200 且取回正文（或 `providers.json` `verified=true`）；**图片理解**判据 = 多模态真机一次 —— `capabilities.vision=true` 只是**声明**（§10.1 陷阱 7）。
> **图例**：``✅ 实测``（有真机物证）· ``🟡 就绪/声明·未实测`` · ``⬜ 未接/未测/未声明`` · ``⚠ 不可用或不一致``
> 来源 = ``providers.json``（声明/接线）× ``site_capability_evidence.json``（实测证据）× ``build/loop/*/report.json``（轮次物证），**冲突时实测优先并在列内标 ⚠**。

**汇总（official）**：共 9 条 · 文字理解 ✅ 实测 **2** · 图片理解 ✅ 实测 **2** · ⚠ 协议未实现 2 · ⚠ 未填 `api_base` 1 · ⚠ 声明未回填 1

**表 A · 协议与就绪**

| # | id | 协议 | 本版本已实现 | api_base | capabilities.vision（声明） | verified（声明） | 最终态 |
|---|---|---|---|---|---|---|---|
| 1 | `openrouter` | openai | ✅ 已实现 | `https://openrouter.ai/api/v1` | true | false | 🟡 声明·未实测 |
| 2 | `zhipu` | openai | ✅ 已实现 | `https://open.bigmodel.cn/api/paas/v4` | true | true | ✅ 已实测 |
| 3 | `openai` | openai | ✅ 已实现 | `https://api.openai.com/v1` | true | false | 🟡 声明·未实测 |
| 4 | `ollama` | openai | ✅ 已实现 | `http://localhost:11434/v1` | true | false | 🟡 声明·未实测 |
| 5 | `gemini` | gemini | ⚠ 未实现 | `https://generativelanguage.googleapis.com` | true | false | ⚠ 协议未实现 |
| 6 | `custom-official` | openai | ✅ 已实现 | **—（未填）** | true | false | ⚠ 未填 api_base |
| 7 | `deepseek` | openai | ✅ 已实现 | `https://api.deepseek.com` | false | true | ✅ 已实测 |
| 8 | `anthropic` | anthropic | ⚠ 未实现 | `https://api.anthropic.com` | true | false | ⚠ 协议未实现 |
| 9 | `siliconflow` | openai | ✅ 已实现 | `https://api.siliconflow.cn/v1` | true | false | 🟡 声明·未实测 |

**表 B · 实测证据与动作**

| # | id | 文字理解 | 文字理解依据 | 图片理解 | 依据（图片理解） | ⚠ 冲突 | 下一动作 |
|---|---|---|---|---|---|---|---|
| 1 | `openrouter` | 🟡 声明·未实测 | — | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） | — | 有 Key / 环境就绪时真机一次 |
| 2 | `zhipu` | ✅ 实测 | providers.json verified=true · 官方 API 文本链路已实测 | ✅ 实测 | E-02 真跑（智谱 glm-4v-flash · 图片理解节点） | — | — |
| 3 | `openai` | 🟡 声明·未实测 | — | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） | — | 有 Key / 环境就绪时真机一次 |
| 4 | `ollama` | 🟡 声明·未实测 | — | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） | — | 有 Key / 环境就绪时真机一次 |
| 5 | `gemini` | ⚠ 协议未实现 | — | ⚠ 协议未实现 | 协议实现前该链路不可用 | — | 协议实现后重测 |
| 6 | `custom-official` | ⚠ 未填 api_base | — | 🟡 待 base+Key | 未填 api_base | — | 填 providers.json api_base + Key 后真机一次 |
| 7 | `deepseek` | ✅ 实测 | M7-09 实测（用户验证）+ providers.json verified=true | ✅ 实测 ⚠声明未回填 | app.log 2026-09-29 01:03 · 模型 deepseek-flash · HTTP 200（759 字节）· 官方 API 真视觉（M7-10 剩声明回填） | vision 声明未回填 | 回填 providers.json capabilities.vision（M7-10） |
| 8 | `anthropic` | ⚠ 协议未实现 | — | ⚠ 协议未实现 | 协议实现前该链路不可用 | — | 协议实现后重测 |
| 9 | `siliconflow` | 🟡 声明·未实测 | — | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） | — | 有 Key / 环境就绪时真机一次 |

> 重建（零副作用，随时可跑）：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly`
<!-- SITE-STATUS:END -->

## 三、这一条线的常见卡点（人工维护）

1. **`custom-official`**：`api_base` 未填 ⇒ 表 A 该格显示 **—（未填）**；先配 base + Key 再谈真机。
2. **`anthropic` / `gemini`**：协议未实现 ⇒ 先在 `actionPlan` 排期实现协议（不是配置问题）。
3. **`openai` / `openrouter` / `ollama` / `siliconflow`**：声明 `vision=true` 但**无真机物证** ⇒ 有 Key / 环境就绪时真机一次，并把证据写进 `source/assets/site_capability_evidence.json`。
4. **`zhipu` / `deepseek`**：已有真机物证（`E-02` / `M7-09`）⇒ `deepseek` 只差声明回填（`M7-10`），回填后即「声明 = 实测」的一致性样本。
5. **「⚠ 声明未回填」怎么读**：`site_capability_evidence.json` 记了 `tested=true`，而 `providers.json` 同名声明仍为假 ⇒ **以实测为准**，动作 = 回填声明（不要改证据去迁就声明）。

## 四、纪律（别踩）

1. **Key 只在凭证库**：`providers.json` / 文档 / 日志 / `CHANGELOG` 一律**不得**出现 Key 明文。
2. **改数据只改两处**：`source/assets/providers.json`（协议 / `api_base` / `capabilities.vision` 声明）× `source/assets/site_capability_evidence.json`（实测证据）⇒ 表格由脚本重建，**不要手改表格**。
3. **看结论先看物证**：`build/loop/**` 随 `build/` 易失 ⇒ 结论要落到 [CHANGELOG.md](CHANGELOG.md) + 本文档 + [loop_dev.md](loop_dev.md) §11。
4. **⛔ 不擅自 `commit` / `tag` / `push` / 改版本号**（[../AI_README.md](../AI_README.md) §6）。
