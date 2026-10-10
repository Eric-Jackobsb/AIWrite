# 循环开发规则（Loop Dev）—— 无人监管下的「网页站逐站转可用」

> **定位**：本文 = **循环开发的唯一规则** —— 什么时候才能开跑、一轮怎么跑、什么算失败、结果记在哪、什么时候必须停。
> **适用对象**：`source/assets/providers.json` 里 `kind = "web"` 且 `web.adapter = "dom"` 的 **10 条网页版站点**（清单见 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §8 附录 E）。
> **权威边界**：站点数据（选择器 / 登录页 / `verified`）以 `source/assets/providers.json` 为准；命令开关以 [../source/README.md](../source/README.md) §4 为准；回归基线数字以 [../AI_README.md](../AI_README.md) §2 为准；**本文只管循环纪律**（怎么跑、记什么、何时停）。
> **状态**：✅ **规则已落盘（2026-10-09）**；前置模块 `FEA-L01`~`FEA-L05` / `TST-L01` / `TST-L02` **已落地**，`TST-L03`（报告模板 + 首份基线）**部分落地**（模板由 `TST-L01` 生成，首份基线 = R1）⇒ **现在是「可自动跑一轮」**（`.\source\tools\loop_verify.ps1`），不再是「只能手工跑单站」；一轮的**结论人审**仍是硬要求（§6.1 三块 + §7 权威位）。

---

## §1 启动闸：**必须由开发者明确授权**

**只有开发者说出「开始循环开发」（或同等明确的指令）才允许进入自动循环。** 没听到这句话时，AI 只允许：

- **只读侦察**：读代码 / 读配置表 / 读文档 / `git status` 等只读命令；
- **单站一次性命令**：例如某一站的 `--web-adapter-selftest --provider <id>`，跑完如实汇报；
- ❌ 不允许：连续多站自动跑、自动写回配置表、自动改文档结论。

**授权 = 一次「轮次预算」**（默认值如下，开发者可当场改）：

| 参数 | 默认值 | 说明 |
|---|---|---|
| 轮次上限 | **3 轮** | 达到上限即停（§8） |
| 单轮站点上限 | **10 站**（全部 DOM 站） | 逐站独立，一站失败不影响其他站 |
| 每站人工介入次数 | **最多 1 次** | 超过即停该站、记入 `needs_login[]` 或「待人工」，继续下一站 |
| 轮间允许的动作 | 只写报告 + `CHANGELOG` + 看板 | **不 commit / 不 tag / 不 push**（[../AI_README.md](../AI_README.md) §6） |

**授权自动失效** —— 出现下列任一情况**立刻停**（走 §8 的停止动作）：

1. 必须**改产品代码**才能继续（循环只做「跑 + 记录」，**不夹带开发**）；
2. 必须**改断言 / 改期望**才能变绿（红线，[../AI_README.md](../AI_README.md) §7 第 3 条）；
3. 站点**改版迹象**且影响不止一站（先汇报、再定）；
4. 需要**清理 / 迁移用户数据**（`~/.brain-ai/**`，红线，[../AI_README.md](../AI_README.md) §7 第 5 条）。

---

## §2 三层结构：轮（Round）/ 站（Site）/ 步（Step）

### 2.1 轮（Round）

- **一轮 = 把 §2.2 的站点清单完整扫一遍**（不是「跑到第一站失败就结束」）。
- **编号**：`L1` / `L2` / `L3`（第 1 / 2 / 3 轮）；轮次编号只出现在报告、`CHANGELOG` 与本文 §11 轮次表里，**不进代码**。
- **物证目录**：`<仓库根>/build/loop/<轮次>-<时间戳>/`，例：`build/loop/L1-20261009-1530/`
  - 该目录已被 `.gitignore` 的 `/build/` 覆盖（**无需改 `.gitignore`**）；
  - ⚠️ **它是易失区**：清 `build/` 即消失 ⇒ 每轮结论必须**同步进 `CHANGELOG` 与附录 E**（见 §7）。

### 2.2 站（Site）：**20 条条目**（official 9 + 网页版 11）· **站点状态台账（`TST-L05` · 机器生成）**

> 📄 **台账四处同源**：本节哨兵块 = **20 条合并一览**；同一次调用还写 [AI_Status.md](AI_Status.md) §二（**总览**：汇总 + 链接）· [AI_Status_web.md](AI_Status_web.md) §二（**网页版分片** 11 站）· [AI_Status_api.md](AI_Status_api.md) §二（**官方 API 分片** 9 条）⇒ 内容必然一致、不会漂移；开关 `-StatusDoc` / `-StatusDocWeb` / `-StatusDocApi`（各自 `''` = 关闭该落点）。

> 本小节哨兵块内的两张表由 `source/tools/loop_verify.ps1`（`TST-L05`）**自动生成** ⇒ **人工只审阅、不手改**：
> 改**数字**请改数据源 —— 声明与接线改 [`source/assets/providers.json`](../source/assets/providers.json)，
> 实测证据改 [`source/assets/site_capability_evidence.json`](../source/assets/site_capability_evidence.json)（**唯一人工维护点**：每站 `text` / `image` 的 `tested` + `evidence` + `date`），
> 轮次结果由循环自动累积（`build/loop/*/report.json`）。重建（**零副作用**：不跑门禁/探测/浏览器）：
> `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly`
> 完整台账（含每站 `history[]` 跨轮轨迹）：`build/loop/SITE_STATUS.json`（机器读）· `build/loop/SITE_STATUS.md`（人读，随 `build/` 易失）。
> **长期保留的人读位** = [AI_Status.md](AI_Status.md) §二（总览）+ [AI_Status_web.md](AI_Status_web.md) §二 / [AI_Status_api.md](AI_Status_api.md) §二（**两份分通道台账**）—— 都在 `docs/` 里、随版本库走，不会被清 `build/` 带走。
<!-- SITE-STATUS:BEGIN （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->
> 生成：`loop_verify.ps1`（TST-L05）· 时间 2026-10-10 · 覆盖轮次 39：R1 · SMOKE · SMOKE2 · SMOKE3 · SMOKE4 · SMOKE5 · SMOKE6 · SMOKE6 · SMOKE7 · SMOKE8 · SMOKE9 · SMOKE10 · SMOKE11 · SMOKE12 · SMOKE13 · T-L06 · T-L06b · T-L06c · T-L06d · T-L07 · T-L08 · T-L09 · T-L10 · T-L11 · T-L12 · T-L13 · T-L14 · T-L15 · T-L16 · T-L17 · T-L18 · T-L19 · T-L20 · T-L21 · T-L22 · T-L23 · T-L24 · T-L25 · T-L26
> **图例**：``✅ 实测``（有真机物证）· ``🟡 就绪/声明·未实测`` · ``⬜ 未接/未测/未声明`` · ``⚠ 不可用或不一致``
> **判据**：**文字理解** = 「文本 → 取回文本」S3 三口径通过（``rc=0`` + ``n3=done`` + 正文非站侧错误串）；**图片理解** = 「图片 → 取回文本」链路（站点上传双证据 + 取回正文）。
> 两列是**链路**、不是模型强弱；来源 = ``providers.json``（声明/接线）× ``site_capability_evidence.json``（实测证据）× ``build/loop/*/report.json``（轮次物证），**冲突时实测优先并在列内标 ⚠**。

**汇总**：共 20 站（web 11 / official 9）· 文字理解 ✅ 实测 **10** · 图片理解 ✅ 实测 **3** · 🟡 就绪/声明 6 · ⬜ 未接/未声明 9 · ⚠ 声明不一致 1

**主表（状态 · 跨轮）**

| # | id | 类型 | 最近轮次 | S2 探测 | 输入命中/候选 | 登录证据 | S3 rc | n3 | 正文码点 | 最终态 | 下一动作 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `spark-web` | web·dom | T-L16 | 失败 | 0/0 | — | — | — | — | 失败（探测级 · §5.1③） | 冷启动后重测；仍失败要有 DOM 物证才可分②改版 |
| 2 | `gemini-web` | web·dom | T-L25 | 未测 | / | — | 0 | done | 22 | 可用（匿名） | — |
| 3 | `doubao-web` | web·dom | T-L21 | 未测 | / | — | 0 | done | 41 | 可用（匿名） | — |
| 4 | `deepseek-web` | web·deepseek-web | — | — | — | — | — | — | — | ✅ 已实测（协议型适配器） | — |
| 5 | `qwen-web` | web·dom | T-L16 | 可用 | 1/1 | — | — | — | — | 未测（S3 未跑） | S2 探到「可用」后跑 S3 |
| 6 | `claude-web` | web·dom | T-L16 | 需登录 | 0/0 | login-url:/login | — | — | — | 需登录 | 人工在窗口内登录一次 → 重跑 S3 |
| 7 | `yuanbao-web` | web·dom | T-L25 | 未测 | / | — | 0 | done | 672 | 可用（匿名） | — |
| 8 | `chatgpt-web` | web·dom | T-L16 | 可用 | 1/1 | login-node x8 | — | — | — | 未测（S3 未跑） | S2 探到「可用」后跑 S3 |
| 9 | `ernie-web` | web·dom | T-L25 | 未测 | / | — | 0 | done | 697 | 可用（匿名） | — |
| 10 | `kimi-web` | web·dom | T-L21 | 未测 | / | — | 0 | done | 70 | 可用（匿名） | 侦察 attach_selector → 真机一次 |
| 11 | `chatglm-web` | web·dom | T-L21 | 未测 | / | — | 0 | done | 10044 | 可用（匿名） | — |
| 12 | `openrouter` | official·openai | — | — | — | — | — | — | — | 🟡 声明·未实测 | 有 Key / 环境就绪时真机一次 |
| 13 | `zhipu` | official·openai | — | — | — | — | — | — | — | ✅ 已实测 | — |
| 14 | `openai` | official·openai | — | — | — | — | — | — | — | 🟡 声明·未实测 | 有 Key / 环境就绪时真机一次 |
| 15 | `ollama` | official·openai | — | — | — | — | — | — | — | 🟡 声明·未实测 | 有 Key / 环境就绪时真机一次 |
| 16 | `gemini` | official·gemini | — | — | — | — | — | — | — | ⚠ 协议未实现 | 协议实现后重测 |
| 17 | `custom-official` | official·openai | — | — | — | — | — | — | — | ⚠ 未填 api_base | 填 providers.json api_base + Key 后真机一次 |
| 18 | `deepseek` | official·openai | — | — | — | — | — | — | — | ✅ 已实测 | 回填 providers.json capabilities.vision（M7-10） |
| 19 | `anthropic` | official·anthropic | — | — | — | — | — | — | — | ⚠ 协议未实现 | 协议实现后重测 |
| 20 | `siliconflow` | official·openai | — | — | — | — | — | — | — | 🟡 声明·未实测 | 有 Key / 环境就绪时真机一次 |

**能力表（文字理解 / 图片理解 · 两列）**

| id | 文字理解 | 图片理解 | 依据（图片理解） |
|---|---|---|---|
| `spark-web` | ✅ 实测（SMOKE2） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `gemini-web` | ✅ 实测（T-L25） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `doubao-web` | ✅ 实测（T-L21） | ✅ 实测 | B3 真视觉（256×256 纯红 → 描述正确）+ upload_adapter=builtin:doubao 双证据 PASS（协议记录 §8.3/§9/§10.2） |
| `deepseek-web` | ✅ 实测 | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `qwen-web` | ⬜ 未测（S3 未通过） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `claude-web` | ⬜ 未测（S3 未通过） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `yuanbao-web` | ✅ 实测（T-L25） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `chatgpt-web` | ⬜ 未测（S3 未通过） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `ernie-web` | ✅ 实测（T-L25） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `kimi-web` | ✅ 实测（T-L21） | 🟡 就绪·未实测 | 接线 builtin:kimi · attach_selector 待侦察/待真机 |
| `chatglm-web` | ✅ 实测（T-L21） | ⬜ 未接 | 无 web.upload_adapter（VLM + 网页版组合会明确报错） |
| `openrouter` | 🟡 声明·未实测 | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） |
| `zhipu` | ✅ 实测 | ✅ 实测 | E-02 真跑（智谱 glm-4v-flash · 图片理解节点） |
| `openai` | 🟡 声明·未实测 | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） |
| `ollama` | 🟡 声明·未实测 | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） |
| `gemini` | ⚠ 协议未实现 | ⚠ 协议未实现 | 协议实现前该链路不可用 |
| `custom-official` | ⚠ 未填 api_base | 🟡 待 base+Key | 未填 api_base |
| `deepseek` | ✅ 实测 | ✅ 实测 ⚠声明未回填 | app.log 2026-09-29 01:03 · 模型 deepseek-flash · HTTP 200（759 字节）· 官方 API 真视觉（M7-10 剩声明回填） |
| `anthropic` | ⚠ 协议未实现 | ⚠ 协议未实现 | 协议实现前该链路不可用 |
| `siliconflow` | 🟡 声明·未实测 | 🟡 声明·未实测 | capabilities.vision=true（无真机物证） |

> 重建（零副作用，随时可跑）：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly`
<!-- SITE-STATUS:END -->

> **20 条共性（2026-10-09 核实 · 人工补充区，哨兵块外、脚本不动）**：网页版 11 条的 `answer_selector` **全空**（11/11）、`done_when` / `answer_poll_ms` / `answer_max_polls` **全空**、`verified` **全 false**、`cookie_names` **仅 `deepseek-web` 配了**；official 9 条里 `protocol=anthropic` / `gemini` **本版本未实现**（选中即明确报错）、`custom-official` 未填 `api_base`。
> **缺 `answer_selector` 不拦路**：`M8-37`（零开关自动识别）会让运行期自己找输入框 / 发送按钮 / 答案容器 ⇒ **本循环不要求回填选择器**（§3 的 S6 可整步跳过）。
> **两列能力是「链路」不是「模型强弱」**：文字理解 = 「文本 → 取回文本」；图片理解 = 「图片 → 取回文本」（网页版还须站点上传模板 `web.upload_adapter` 接线 + 真机双证据）。
> **声明 ≠ 实测（§10.1 陷阱 7）**：`capabilities.vision` / `verified` 都是**声明**字段；例：`deepseek` 条目声明 `vision=false`，而官方 API 图片理解**已实测通过**（`M7-09` / `M7-10`）⇒ 台账能力列打 `⚠ 声明未回填`；回填 `providers.json` 属**产品侧动作**、不在循环里做。

### 2.3 步（Step）

站级 8 步见 §3；**每一步都要留下可复核的物证**（命令 + 退出码 + 输出 txt），否则该步不算完成（[../AI_README.md](../AI_README.md) §3 取证纪律）。

---

## §3 站级步骤：**匿名优先**（免登录站直接测；强制登录的记入报告并跳过）

> **核心口径（2026-10-09 修订）**：大部分站点**不登录、用临时会话也能输入输出** ⇒ 先匿名直跑，**不要一上来就要求人工登录**。
> 只有判定「**需强制登录**」的站，才把证据写进报告、交人工处理，**并立刻跳到下一个 AI**（不阻塞循环）。

| 步 | 动作 | 谁做 | 是否可跳过 |
|---|---|---|---|
| **S1** | 只读确认该条现状：`--provider-dump`（或读 `providers.json` 该条 + 会话清单 `--web-session-list`，见 `FEA-L02`） | 自动 | ❌ |
| **S2** | **匿名直跑**：`--web-adapter-selftest --provider <id>`（只读探针：输入框 / 发送 / 答案容器命中数） | 自动 | ❌ |
| **S3** | 生成测试：`--run-selftest --web --provider <id>`（**绿判据 = 三口径**：**退出码 0** **且** `n3 LLMGenerate = done` **且**归档正文**不是站侧自己的错误/忙碌串** —— 只看退出码会把「需登录 / 发送未生效」的站误判为可用（§10.1 陷阱 3）；只看 `n3=done` 会把「站点繁忙 / 被限流」的站误判为可用（§10.1 陷阱 6）） | 自动 | ❌ |
| **S4** | **需登录判定**（仅当 S2/S3 未通过）：按 §3.2 判据判定 ⇒ 写入报告 `needs_login[]` ⇒ **跳过，进入下一站** | 自动 | ❌（有需登录站时必做） |
| **S5** | 人工登录（**只在人工处理窗口做**：有头窗口手动登录一次，**不代填密码、不绕过验证**） | 人工 | ✅ 循环内不做，留给人工 |
| **S6** | 选择器回填（`answer_selector` / `done_when` / 轮询参数）：**默认整步跳过** —— 靠 `M8-37` 自动识别即可；仅当要"固化"某站时才做（需 S5 后有一条真实回答） | 人工 | ✅ **可完全跳过** |
| **S7** | 条目回填：`verified: true` + `notes` 记实测日期（**不填假值**，`PB2-26` 原则） | 人工确认后写 | ✅ 未过就不写 |
| **S8** | 结果入报告（§6.1）+ 同步 `CHANGELOG` / 附录 E（§7） | 自动 | ❌ |

**一轮里人只需要做两件事**（且都只在"该站确实需要"时才做）：① S5 登录一次；② S7 界面实测确认。**S6 可以整步不做** —— 这是 `M8-37` 带来的省时点。

### 3.1 命令清单（照抄即可，全部在仓库根执行）

```powershell
# S1 只读确认（20 条：official 9 / web 11；web 里 dom 适配器 10 条）
.\build\bin\aiwrite.exe --provider-dump > $env:TEMP\dump.txt 2>&1; Get-Content $env:TEMP\dump.txt

# S2 匿名直跑探针（**单站**；批量版见下 —— `FEA-L01` 已落地）
Start-Process -FilePath .\build\bin\aiwrite.exe -ArgumentList '--web-adapter-selftest','--provider','<id>' `
  -NoNewWindow -Wait -PassThru -RedirectStandardOutput $env:TEMP\adapter.txt; Get-Content $env:TEMP\adapter.txt

# S2 批量（FEA-L01 · 已落地）：10 站一次跑完 + 落盘报告 + 与上一轮对比
Start-Process -FilePath .\build\bin\aiwrite.exe `
  -ArgumentList '--web-adapter-selftest','--provider','all','--timeout','30','--json','build\b3_probe\probe_all2.json','--json-diff','build\b3_probe\probe_all.json' `
  -NoNewWindow -Wait -PassThru -RedirectStandardOutput $env:TEMP\probe_all.txt; Get-Content $env:TEMP\probe_all.txt
# 退出码：0=全部可用 / 3=需登录（等人，不算失败）/ 1=有真失败 / 2=用法配置错；逐站结论看报告 sites[].state

# 会话 / 登录态清单（FEA-L02 · 已落地）：有没有会话都算成功（GUI 子系统 ⇒ 要读 stdout 请照上面用 Start-Process 重定向）
.\build\bin\aiwrite.exe --web-session-list --json build\b3_probe\session.json

# S3 端到端生成（**一键跑一轮**时由 source\tools\loop_verify.ps1 自动做；下面是手工复核时照抄的版本）
Start-Process -FilePath .\build\bin\aiwrite.exe -ArgumentList '--run-selftest','--web','--provider','<id>' `
  -NoNewWindow -Wait -PassThru -RedirectStandardOutput $env:TEMP\run.txt; Get-Content $env:TEMP\run.txt; $LASTEXITCODE

# S6 仅在需要固化选择器时（人工先发一条问答后）
Start-Process -FilePath .\build\bin\aiwrite.exe -ArgumentList '--web-dom-dump','--provider','<id>' `
  -NoNewWindow -Wait -PassThru -RedirectStandardOutput $env:TEMP\dom.txt; Get-Content $env:TEMP\dom.txt
```

> ⚠️ `aiwrite.exe` 是 **GUI 子系统**：`>` 捕不到 stdout，**必须**用 `Start-Process … -RedirectStandardOutput`（[../AI_README.md](../AI_README.md) §3）；而且 stdout **偶发整轮丢失** ⇒ **S3 的判据不许只依赖 stdout**（§10.1 陷阱 2）。
>
> **S3 判据 = 三口径**（§10.1 陷阱 3 + 陷阱 6）：**`rc=0`** **且** 归档 `n3-LLMGenerate.txt` 的「状态:」= `done` **且** 该归档正文**不是站侧错误/忙碌串**（判据：正文 **≤ 120 码点** 且命中已知错误短语 ⇒ 判「未真生成」）。归档由 PC-05 写磁盘：`%TEMP%\aiwrite_run_selftest_outputs\<时间戳>-未命名\n3-LLMGenerate.txt`（前 3 行 = 节点 / 状态 / 耗时）；`loop_verify.ps1` 会把它复制到 `<轮次目录>\raw\<id>.run<N>.archive\`，报告即从那里取判据。


### 3.2 「需强制登录」的自动判据（**零人工标注**）

判据只用**页面侧事实**（DOM），**不看 Cookie** —— 原因见下方注。

| 事实（来自 S2 探针） | 结论 | 处置 |
|---|---|---|
| 输入区候选 `> 0`（或 `input_hits > 0`）、发送可达 | **匿名可用** | 直接进 S3；成功即记 `可用（匿名）` |
| 输入区候选 `= 0` **且** 页面出现登录控件 / URL 落在登录路径 | **需强制登录** | 写 `needs_login[]`（带证据）→ **跳过，下一站** |
| 输入区候选 `= 0` **但** 无任何登录迹象 | **页面异常** | 归失败（未渲染完 / 脚本被拦），可选重试 1 次；再失败则记入报告 |
| 输入区候选 `> 0`、S3 取答案超时 | **站点改版 / 轮询参数缺失** | 归失败，记入报告（§5 分类②） |

> **为什么不看 Cookie**：`web_session_state()`（`source/ai/src/provider_spec.cpp`）的「已登录」判据是「该 origin 有 Cookie」—— 而**匿名临时会话通常也有 Cookie** ⇒ 会误报「已登录」⇒ **不能**当「需登录」的判据。本判据不读 `userToken`（守不变量 `I15`），也不改 `D-27` 的既有判据。

### 3.3 跳过规则（本规则的核心行为）

- 「需强制登录」**不是失败**：退出码记 **3**（§6.3），循环**不停**，直接进入下一站。
- 需登录站由人工在**循环之外**处理：登录一次 → 重跑该站（`--run-selftest --web --provider <id>`）→ 结果回填报告与 `CHANGELOG`。
- **同站不重复试**：一轮内同一站最多跑 **2 次**（原始 + 1 次重试）；仍不过就记录结论、进下一站。
- **诚实失败**：站点不可用时**如实报错并给可操作原因**，不假装成功、不无限等待（沿用 `R13`）。

---

## §4 轮次门禁（每轮开跑前后各跑一次）

**开轮前**（证明起点干净）：

| 检查 | 命令 | 期望 |
|---|---|---|
| 构建 | `cd source; .\build.ps1` | **0 error / 0 warning** |
| 工具 / 契约自检 | `api_probe.exe --selftest` | 七组全 PASS · exit **0**（含执行器 **452 / 0** —— 2026-10-09 批 3 新增 27 项 DOM 自检后 **425 → 452**） |
| 图模型 / 执行器自检 | `--graph-selftest` / `--exec-selftest` | **110 / 0** · **452 / 0** |
| 提供商标记 | `aiwrite.exe --provider-selftest` / `--provider-dump` | **50 / 0** · **20 条**（official 9 / web 11）（`TST-L01` 门禁已把 `--provider-dump` 作为「条数快照」自动跑，见 `gate/dump.txt`） |
| **站点上传模板**（离线 · 不起浏览器） | `aiwrite.exe --upload-selftest` | **72 / 0** · exit **0**（2026-10-10 实测；`M8-24`~`M8-27` 上传模板契约，站点侧接线出问题时先跑这条；门禁 `gate/upload.txt`） |
| 离线夹具 | `node source\tools\poll_answer_gate_test.js` · `send_chain_test.js` · **`dom_gate_test.js`** | **12 / 0** · **17 / 0** · **15 / 0**（各自 `--old` 负向必 FAIL；`dom_gate_test.js --old` = **6 / 15** · exit 1） |
| 文档断链 | `python vcpkg-cache\check_links.py` | **新增断链 0** —— 2026-10-09 批 3 实测 = 检查 **610** 链接 / broken **8**；**R1 收轮复跑 = 检查 669** · **`TST-L05` 落地复跑 = 检查 683**（同一 broken 8，新增 14 条链接全有效）（数字涨 = 本文档批量新增 ⇒ broken 集合**不变**）⇒ 门禁判据是「**本轮不得新增**」，**不是**「全库 0」；8 条全部来自未改动的旧文档，已登记 `DBG-L01` |

> 上表是 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §0.4 回归基线的**循环化用法**（原表的数字以该处与 [../AI_README.md](../AI_README.md) §2 为准；数字变动时先改那两处，再同步本文）。
> **门禁的意义**：站点循环的数字只在"起点是绿的"前提下才有可比性；起点不绿 ⇒ **先停下汇报，不进循环**。

**收轮后**：重跑同一张表，并要求**与开轮前的数字逐项一致**（本轮只跑网页站，不应影响任何离线断言）。若不一致 ⇒ 停止循环、进入排查（可能是站点外因素 —— 如我方的配置表 / 会话落盘被动过）。

**轮次纪律**：

1. **一轮一报告**（§6.1），落在 `build/loop/<轮次>-<时间戳>/`；
2. **结论必须进 `CHANGELOG` 与附录 E**（物证目录易失，§7）；
3. **不夹带代码改动**：循环内只允许改 `providers.json` 的站点数据（`verified` / `notes` / 选择器）与文档；**产品代码改动一律出循环**（走正常计划流程）；
4. **不 commit / 不 tag / 不 push**（[../AI_README.md](../AI_README.md) §6）。

---

## §5 失败分类与处置三分法

### 5.1 四分类（每一类必须先归类，再决定动作）

| 类 | 名称 | 判据（客观事实） | 处置 |
|---|---|---|---|
| ① | **需人工（needs_login / WAF）** | §3.2 判据成立；或首屏为验证挑战 | **跳过**，记报告 `needs_login[]` / `需人工`；退出码 **3** |
| ② | **站点改版（数据过期）** | 原来能跑、本轮命中数变 0 / 取答案超时；页面结构明显变化 | **跳过 + 登记**（选择器 / 轮询参数待人工重取，走 S6）；退出码 **1** |
| ③ | **环境问题** | 网络不可达 / 会话失效（`R19`）/ 窗口被占用 / 磁盘或权限 | **重试 1 次**；仍失败则记环境失败（**不出循环**），退出码 **1** |
| ④ | **我方缺陷** | 探针脚本/报告本身报错；同一现象在多个站复现且与站点无关 | **停下来**（授权失效 §1 第 1 条）⇒ 出循环、按正常流程修 + 回归；不许在循环里直接改产品代码 |

### 5.1.1 归因台账（按轮追加 → 该轮同一现象在 **≥ 3 站**出现时**必查** ④）

> 本表是**台账索引**（便于跨轮对照）；**结论权威位**仍是 §7 的 `CHANGELOG` + 该轮报告。

| 轮次 | 站 | 现象（客观事实） | 归因 | 证据 |
|---|---|---|---|---|
| **R1** | `kimi-web` · `doubao-web` · `tongyi-web` | S2 探测 **可用（匿名）**，但 S3 `--run-selftest --web` **退出码 1**；`VA-07` 行判**失败**；保存 JSON 与运行前**逐字节相同**；三站 `n3=done`（生成其实成功） | **④ 我方缺陷**（VA-07 取样按**字节** ⇒ CJK 答案被误判「运行结果泄漏」） | `raw/s3_summary.txt` rc=1 ×3 · `raw/<id>.run.txt`（`n3 LLMGenerate done` + `VA-07 …：失败`）· `raw/kimi-web.p11_3.txt` 命中＝**947** · `va07_probe14.txt` / `va07_probe16.txt` 离线复算（同一物证上**旧判据命中 / 新判据未命中**）· 修复后 rc=0（`va07_probe12.txt` · `va07_probe13.txt`） |
| **R1** | `yuanbao-web` · `spark-web` | S3 **`rc=0` 且断言全 PASS**，但 `n3=error`（`yuanbao`：**发送未生效**；`spark`：需登录）⇒ 若只看退出码会被记成「可用」 | **④ 我方缺陷（判据层）**：`--run-selftest` 的**退出码不区分生成失败** ⇒ 报告口径会**假阳性** | `raw/yuanbao-web.run.txt`（`n3 LLMGenerate error` + `VA-07 …：OK` + `PASS` + rc=0）· `SMOKE6-20261009-2149/report.md`（`spark-web` rc=0 / n3=error）⇒ 已由 `TST-L01` 先收为**双口径**、后由 `SMOKE7` 再收为**三口径**（§10.1 陷阱 3 / 陷阱 6） |
| **R1** | `ernie-web` · `yuanbao-web` · `chatglm-web` | S3 **rc=0** | 无缺陷（**对照组**：防「真失败被改成假通过」） | `raw/s3_summary.txt` · `va07_probe12.txt` · `va07_probe15.txt` |
| **R1** | `qwen-web` · `chatgpt-web` · `claude-web` · `gemini-web` | S2 探测**未跑通**（2 次尝试同结果）：`页面未就绪（缺 页面级；已达：窗口级）`；输入区候选 0 **且**无登录迹象 | **③ 环境问题**（§3.2 行 3「页面异常 ⇒ 未渲染完 / 脚本被拦」）—— 本条**只能到③**：**未定**是否兼有②（站点改版），因**本轮无页面结构证据**（探针未取到 DOM） | `probe.txt` 行 14–19 · 46–63 · `probe.json`（4 条 `failure_reason` 同文案） |
| **R1** | `spark-web` | S2：输入区候选 **0** + 登录控件 **2**（`login-node x2`） | **① 需人工（需登录）** | `probe.txt` 行 40–45 · `probe.json` `needs_login[]` |

> **R1 的 ④ 触发准则**：同一现象 **3 站**（kimi / doubao / tongyi）连续出现 ⇒ 命中 §8「立即停止」第 1 条 ⇒ **出循环修缺陷**（已按 §5.2 第 3 条执行）。
> **R1 的第二个 ④（判据层）**：`yuanbao-web` / `spark-web` 的 `rc=0` 与「生成失败」同时成立 ⇒ 说明**退出码本身不是充分判据**（同现象的站数 ≥ 3 时按上一条处置；本例 2 站但性质明确 ⇒ 一并修，落 `TST-L01` 的双口径）。
> **`SMOKE7`~`SMOKE9`（`kimi-web`）⇒ 判据再加一档（第三口径）**：`rc=0` **且** `n3=done` 同时成立，但归档正文就是**站点自己的错误串**「System is currently busy. Please try again later.」（**49 码点**；stdout 整轮丢失 ⇒ 归档判据照常生效）⇒ 归 **③ 站侧繁忙 / 被限流（站点侧，非我方缺陷）**，动作 = 重试 1 次 + 判失败（`SMOKE8` / `SMOKE9` 均如实 exit 1）；`SMOKE10`（`ernie-web`）作**反面校准**：74 码点合法短答案未被误伤（`可用（匿名）` · exit 0）—— 详见 §10.1 陷阱 6。**同类站在报告里的位置**：`失败`（不是「可用」）。

### 5.2 处置三分法

1. **跳过**（①②③且无我方责任）：记录 + 进下一站，**不阻塞**；
2. **登记待人工**（①，以及②的"待重取选择器"）：写进报告 + 看板（`DevPlan.todo`）**待人工**项，供人工批量处理；
3. **出循环修缺陷**（④）：停循环 → 汇报 → 走正常改动流程（登记 → 改 → 回归 → 文档）。

> **判据顺序**：先「站点事实」再「我方责任」。**任何"可能是我们坏"的结论，都必须能给出代码级或物证级依据**，否则归类为 ①②③ 并如实标注「未定」。

---

## §6 观测物与退出码

### 6.1 每轮物证目录（唯一落点）

```
build/loop/<轮次>-<时间戳>/         例：build/loop/R1-20261009-2055/
├── report.md        人读版（**四块**：① 本轮数字表 ② 站点矩阵 ③ needs_login[] + 待人工清单 ④ 站点台账摘要（TST-L05））
├── site_status.md   站点状态台账人读快照（TST-L05 · 与 build/loop/SITE_STATUS.md 同源 · 跨轮两张表）
├── report.json      机器读版（供与上一轮 diff）
├── raw/             原始输出：每站 `<id>.run<N>.txt`（stdout + rc）+ `<id>.run<N>.err.txt`
│                                + `<id>.run<N>.workflow.json`（**运行前** JSON 基线，S3 判据用）
│                                + `<id>.run<N>.archive/`（PC-05 板上归档：`n3-LLMGenerate.txt` 的「状态:」= **n3 判据权威来源**，stdout 丢失时仍可得）
│                    + 探测/专项物证：`probe.txt` · `probe.json` · `<id>.adapter.txt` / `<id>.dom.txt` · `va07_probe*.txt`
├── gate/            门禁输出：build.txt · selftest.txt · graph.txt · exec.txt · provider.txt · upload.txt · dump.txt · links.txt
└── sandbox/         运行自检的临时工作目录（PC-05 归档落点；不影响用户数据）
```

- 目录由 `TST-L01`（`source/tools/loop_verify.ps1`）自动创建（✅ **已落地 2026-10-09**）；手工跑时**也要建同名目录**（口径一致，避免以后搬家）。
- 报告**四块**是固定契约（`TST-L03`）：少了哪一块，该轮视为**报告不合格**。模板**已由 `TST-L01` 生成**（`report.md` / `report.json` 同一份数据）；`TST-L03` 剩「首份基线 = R1」的人审与固化。
  - ① 数字表固定行（判据自检行，便于跨轮比趋势）：门禁 PASS/FAIL · S2 探测 · S3 端到端 · ⚠ `rc=0` 但 `n3≠done`（陷阱 3）· ⚠ `n3=done` 但正文＝站侧错误串（**陷阱 6**）· stdout 丢失站数（陷阱 2）。
  - ② 矩阵每站额外字段：`n3` / `n3_source`（`archive` / `stdout`）· **`busy`（命中的站侧错误短语，空=未命中）** · **`body_cps`（归档正文码点数）** · `stdout_lost`。
  - ③ 需人工清单（`needs_login[]` + 待人工项 + 每站「下一动作」备注）。
  - ④ **站点台账**（`TST-L05`）：20 条 × 两列能力（文字理解 / 图片理解）+ 下一动作（跨轮累积）；**四个落点同源**（同一次调用）—— 本文 §2.2 哨兵块（**合并一览** 20 条）· [AI_Status.md](AI_Status.md) §二（**总览**：汇总 + 链接）· [AI_Status_web.md](AI_Status_web.md) §二（**web 分片** 12 站）· [AI_Status_api.md](AI_Status_api.md) §二（**official 分片** 9 条）；快照 `build\loop\SITE_STATUS.md`（及 `<轮次目录>\site_status.md`）、机器读 `build\loop\SITE_STATUS.json`。少了 ④ ⇒ 该轮台账视为**未更新**。


### 6.2 站点矩阵（第二块）的五态口径

| 态 | 含义 | 依据 |
|---|---|---|
| `可用（匿名）` | 不登录即端到端成功 | S3 **三口径**：**退出码 0** **且** `n3 = done` **且** 归档正文**非站侧错误/忙碌串**（§10.1 陷阱 3 / 陷阱 6） |
| `可用（登录后）` | 人工登录后成功（该站已在循环外补测） | S3 退出码 **0** + `n3 = done` + 有登录动作记录 |
| `需登录` | 判据成立，未测生成 | §3.2 + 报告 `needs_login[]`；或 S3 `n3 = error` 且原因文案指向登录 |
| `失败` | ②③④ 之一 | §5.1 归类 + 证据（**`rc=0` 但 `n3 ≠ done` 也记这里** —— 退出码没区分；**`n3=done` 但归档正文＝站侧错误/忙碌串同样记这里** —— 见 §10.1 陷阱 6；两者都**不许**记「可用」） |
| `未测` | 本轮未覆盖（如时间不足） | 明确写出**未测原因**，不许留空 |

> 会话清单：`--web-session-list [--json <路径>]`（`FEA-L02` · ✅ **已落地 2026-10-09**，复用 `source/web/include/session_store.h` 的落盘数据）—— 把「登录态清单只在界面上能看」变成**报告可读**：① 本进程内存会话 ② L2 登录态快照**只读元数据**（不解密）③ 11 条网页版条目的登录判据；**有没有会话都算成功**（退出码 0）。

### 6.3 退出码契约（循环的"语言"）

| 码 | 含义 | 循环行为 |
|---|---|---|
| **0** | 成功 | 记 `可用`，进下一站 |
| **1** | 真失败（站点改版 / 环境 / 我方缺陷） | 归类（§5.1）后进下一站；④ 类则**停循环** |
| **2** | 用法 / 配置错误（缺参、未知 `--provider`） | **停循环**，不许带着错误参数继续（守 `I20` 既有 CLI 契约） |
| **3** | **需人工介入**（需登录 / 需过 WAF） | 记 `needs_login[]` → **跳过，进下一站**（不算失败） |

> **`3` 是循环能在无人监管下安全停、又不把"等人"当 bug 的前提。** ✅ **`FEA-L03` 已落地（2026-10-09）**：批量（`--provider all`）与单站（`--provider <id>`）两条路径都按本表返回退出码。
> **聚合口径（批 3 真机实测订正 · 以代码为准）**：**11 站混合**时 = **有真失败 ⇒ `1`；否则有 `需登录` ⇒ `3`；否则 `0`** —— 即 **`1` 优先于 `3`**（真失败比「等人」更需要人手）；**单站**场景里二者互斥（§3.2 行 2 与行 3 不会同时成立）⇒ 无冲突。逐站结论**不因聚合码丢失**：报告 `sites[].state` 与 §6.2 五态一一对应，需登录站也在 `needs_login[]` / stdout 里单独列出。
> ⚠️ 此前口头简报里的「优先级 **3 > 1**」是**单站**口径的简写，**已订正**为上面两句（2026-10-09 实测：11 站里 2 站需登录 + 1 站失败 ⇒ 返回 **1**）。
> **两点实测补充（R1 · 2026-10-09）**：
> ① 本表的码是**站点侧**（`--provider` 路径）的语言；`loop_verify.ps1`（TST-L01）的**轮次**退出码另加一条：**有「未测」也 ⇒ 1**（一轮没跑全 ⇒ 不许当成功）。
> ② **`0` 不等于「生成成功」**：`--run-selftest --web` 里「需登录 / 发送未生效」也会 PASS + `rc=0` ⇒ 站点态必须叠加 `n3`（§10.1 陷阱 3、§6.2）。

---

## §7 记录位（**唯一权威表** —— 结论不许只写在报告里）

| 内容 | 唯一权威位 |
|---|---|
| 本轮结论数字 + 11 行站点矩阵 | [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`（本轮小节） |
| 站点「可生成」列 + 登录态列 | [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §8 **附录 E** 两列 |
| 选择器 / `verified` / `notes` | `source/assets/providers.json`（**不填假值**） |
| 逐站实测细节（协议 / 元素 / 时间戳） | 📦 [Archive/网页版协议实测记录.md](Archive/网页版协议实测记录.md)（**已归档 2026-10-10 · 只追溯 · 不再维护**）⇒ **新证据改记** `source/assets/site_capability_evidence.json`（`TST-L05` 唯一人工维护点）+ §2.2 台账 |
| 循环规则本身 | **本文**（`docs/loop_dev.md`） |
| 任务与待人工项 | [DevPlan.todo](DevPlan.todo) |
| 物证原始输出 | `build/loop/<轮次>-<时间戳>/`（**易失**，仅作过程物证） |

> **硬约束**：`build/` 随时可被清空 ⇒ **只写报告不算完成**；每轮必须完成上表前两行的同步（[../AI_README.md](../AI_README.md) §4 改动伴随义务）。
> 看板登记参考：`source/python/_probe/out/**` 与 `build/**` 都是**过程产物**，不进版本库、不做权威位。

---

## §8 停止条件（无人监管下的"安全闸"）

**正常结束**：

1. 10 站全部有结论（含 `需登录` / `未测` 也要写明原因）；
2. 或到达**轮次上限**（默认 3 轮）。

**立即停止**（不等本轮跑完）：

| 触发 | 理由 |
|---|---|
| 同一类失败在 **≥ 3 站**连续出现 | 更像我方缺陷（§5.1 ④），继续跑只会刷垃圾数据 |
| 需要**改产品代码** | 授权自动失效（§1 第 1 条） |
| 需要**改断言 / 期望**才变绿 | 红线（[../AI_README.md](../AI_README.md) §7 第 3 条） |
| 需要**清理 / 迁移用户数据** | 红线（同前 §7 第 5 条） |
| 门禁数字与开轮前**不一致** | 起点/终点不可比（§4） |
| 单站人工介入次数**超上限** | 该站交人工，循环本身继续（除非连续发生） |

**停止后必做**（按序）：

1. 写报告（即使只有半轮，也要写"跑到哪、为什么停"）；
2. 同步 `CHANGELOG` + 附录 E（§7）；
3. 在 `DevPlan.todo` 登记**待人工项**；
4. **汇报 + 停下等指令**（不改代码、不提交）。

---

## §9 与代码组织规范的接口

循环只允许**按现有规范**加文件；新增任何代码（例如 `FEA-L01` 的批量入口、`TST-L01` 的驱动脚本）必须遵守：

- **每个模块自带 `src/` + `include/`** —— `.cpp` 在 `source/<模块>/src/**`、头文件在 `source/<模块>/include/**`（模块内**拍平**；引用写**裸名**，例 `#include "log.h"`）—— 见 [../AI_README.md](../AI_README.md) §11「代码组织规范」；
- **`source/ai/src/` 下按 AI 分目录**（`ai/deepseek/`、`ai/doubao/`、`ai/kimi/`…），**通用件不动**（`provider_spec` / `dom_web_client` / `web_pow` / `upload_supply_probe` / `upload/**` 的框架四件 `image_convert` · `upload_contract` · `upload_evidence` · `upload_registry`）—— **站点上传单元** `site_upload_<站>.cpp` 是站点知识 ⇒ 归 `ai/<站>/`；
- 循环脚本放 `source/tools/**`（与 `poll_answer_gate_test.js` 同处）。

---

## §10 前置模块（循环"能自动跑"还差什么）

| 编号 | 内容 | 归属 | 状态 |
|---|---|---|---|
| `FEA-L01` | 批量 + 结构化报告：`--web-adapter-selftest --provider all --json <路径>` + 与上轮 diff（含 `needs_login` / `login_evidence` 字段） | Feature | ✅ 已落地（2026-10-09 · 真机 11 站实测：可用 8 / 需登录 2 / 失败 1） |
| `FEA-L02` | `--web-session-list`：会话清单机器可读（复用 `source/web/include/session_store.h`） | Feature | ✅ 已落地（2026-10-09） |
| `FEA-L03` | 退出码 **3**（需人工介入）贯通（不破坏 `I20`） | Feature | ✅ 已落地（2026-10-09 · 聚合口径见 §6.3） |
| `FEA-L04` | 目录规范化重构：header → `source/include/**`（镜像 `src/**`）；`source/src/ai/` 下按 AI 分目录；CMake 与连带文档同步 | Feature | ✅ 已落地（2026-10-09） |
| `FEA-L05` | **目录再规范化（取代 `FEA-L04` 的两棵镜像树形态）**：每个模块自带 `src/` + `include/`（头文件模块内**拍平**；引用改**裸名** `#include "graph.h"`）；入口 → `source/app/src/main.cpp`；CMake 头文件根 **1 → 6** | Feature | ✅ 已落地（2026-10-09 · 104 文件搬迁 · 回归数字逐项不变） |
| `TST-L01` | `source/tools/loop_verify.ps1` 总驱动（门禁 + S2 探测 + S3 逐站 + 报告落盘） | Test | ✅ 已落地（2026-10-09 · R1 + SMOKE1~SMOKE9 实测；**绿判据 = 三口径 `rc=0` 且 `n3=done` 且归档正文非站侧错误串**，见 §10.1 陷阱 3 / 陷阱 6） |
| `TST-L02` | `source/tools/dom_gate_test.js` 夹具 + `--old` 负向打假 | Test | ✅ 已落地（2026-10-09 · **15 / 0**；`--old` **6 / 15** 如实 FAIL · exit 1） |
| `TST-L03` | 物证目录 + 报告三块模板 + 首份循环基线报告 | Test | 🟡 部分落地（2026-10-09 · 目录 + 三块模板**已由 `TST-L01` 生成**；首份基线 = **R1-20261009-2055**，人工固化中） |
| `TST-L05` | **站点状态台账**：`source/assets/site_capability_evidence.json`（实测证据唯一人工维护点）+ `providers.json`（声明）× `build/loop/*/report.json`（轮次物证）⇒ 生成「20 站 × 文字理解 / 图片理解」两列表 + `history[]`，回写 §2.2 哨兵块；`-StatusOnly` 零副作用重建 | Test | ✅ 已落地（2026-10-10 · 覆盖 12 轮累积 · 21 站 · 文字理解 ✅ 9 / 图片理解 ✅ 3 · ⚠ 声明不一致 1） |

| `DOC-L01` | 本文（循环规则）+ `AI_README` §6/§7 + `DocMAP` 登记 | Docs | ✅ 已落地（2026-10-09） |
| `DOC-L02` | 代码组织规范条文 + 重构连带文档同步 | Docs | ⬜ 计划中 |

> **现状诚实说明（2026-10-09 更新 · 取代旧口径「还没有一键一轮」）**：`FEA-L01` / `FEA-L02` / `FEA-L03` / `TST-L01` / `TST-L02` **已落地** ⇒ **可以一键跑一轮**：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1`（门禁 → S2 探测 → S3 逐站 → 报告四段落 `build\loop\<轮次>-<时间戳>\`）。
> 仍未闭环的部分（如实登记）：① `TST-L03` 的**首份基线**要人工把 R1 数字同步进 `CHANGELOG` / 附录 E（§7）；② **站点侧退出码不改**（`--provider` 路径的 `rc=0` 仍不区分生成失败）⇒ 判据的「三口径」目前固定在**驱动 + 报告**这一层（§10.1 陷阱 3 / 陷阱 6），若将来要产品侧也区分，属新计划项、**不在循环内做**。
> **`TST-L05` 站点状态台账（2026-10-10 已落地）**：每轮结束由 `loop_verify.ps1` 自动把「**20 站 × 两列能力**（文字理解 / 图片理解）」写进 ① `build\loop\SITE_STATUS.json`（机器读 · 含每站 `history[]` 跨轮轨迹）· ② `build\loop\SITE_STATUS.md` + `<轮次目录>\site_status.md`（人读快照）· ③ **`docs\loop_dev.md` §2.2 哨兵块**（唯一人读薄表，**机器回写 · 人工只审阅**）；③ 的**零副作用**重建：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly`（不建轮次目录 / 不跑门禁 / 不探测 / 不起浏览器）。

> **驱动唯一性**：R1 期间的**手工驱动** `s3_driver.ps1`（当时落在 `build\loop\R1-…\` 内，随 `build` 清理作废）**已被 `TST-L01` 取代** ⇒ **不要再造第二个驱动**；`build\**` 里的脚本一律视为**过程物证**，不进版本库。

### 10.1 已知陷阱（实测踩坑 · 逐条带物证 · 判据都按此收紧）

| # | 陷阱 | 实证 | 对策（已落地） |
|---|---|---|---|
| 1 | **GUI 子系统无 stdout** | `>` / `\|` 捕不到 `aiwrite.exe` 的输出（[../AI_README.md](../AI_README.md) §3） | 一律 `Start-Process … -RedirectStandardOutput`；`loop_verify.ps1` 统一走 `Invoke-Capture` |
| 2 | **stdout 会整轮丢失**（非「没输出」） | 多轮实测：同样的命令、同样的退出码，`*.run.txt` 有时 0 字节（VS Code shell 集成吞管道 + GUI 子系统双重因素） | **判据不依赖 stdout**：状态优先取**磁盘 PC-05 归档**（§3.1 末注）；stdout 只作辅助列（报告里 `n3_source` 注明来源）；报告不因 stdout 为空就判「未测」 |
| 3 | **`--run-selftest --web` 的 `rc=0` ≠ 该站可用** | `R1`：`yuanbao-web` **`n3=error`（发送未生效）** 却 `VA-07=OK` + `PASS` + `rc=0`；`SMOKE6`：`spark-web`（需登录）同样 `rc=0` / `n3=error` | S3 绿判据先改**双口径**：`rc=0` **且** `n3=done`（§3 表 · §6.2 · §6.3 注②）；报告单列 `n3` / `n3_source` 两列。**后由陷阱 6 再收为三口径**（+ 正文自检） |
| 4 | **VA-07 取样按「字节」会误伤 CJK** | `R1`：3 站（kimi / doubao / tongyi）`rc=1`；`raw/kimi-web.p11_3.txt` 旧判据命中位置 = **947**（24 字节只等于 8 个汉字） | 产品侧已修（`main.cpp`：按 **UTF-8 码点**取样 + **运行前 JSON 基线**判据）→ 离线复算锁 `va07_probe14/16.txt`（同一物证：旧判据命中 / 新判据未命中）；驱动侧保留**离线复算**列作交叉核对 |
| 5 | **`--provider a,b` 在 `-File` 调用下是「一个字符串」** | `SMOKE6-20261009-2148`：`-Providers spark-web,kimi-web` 生成的物证文件名叫 `spark-web,kimi-web.run1.txt`（没拆开） | 驱动内部**按逗号再拆**（`-File` 不做数组解析）⇒ `SMOKE6-20261009-2149` 起物证恢复为 `spark-web.run1.*` / `kimi-web.run1.*` |
| 6 | **`n3=done` ≠ 真生成了答案**（站点自己的错误串被当成答案） | `SMOKE7-20261009-2200`（`kimi-web`）：`rc=0` + `n3=done` + **stdout 整轮丢失**，但归档正文 **49 码点** = 站点侧「System is currently busy. Please try again later.」⇒ 按旧口径会被记「可用」（假绿）；`SMOKE8` / `SMOKE9` 复现同一现象（**均如实 exit 1**）。**源码反证**：全仓 `grep`「currently busy / try again later」= 0 命中 ⇒ 该串**不是我们写的**，是站点返回的。**反面校准**：`SMOKE10-20261009-2208`（`ernie-web`）归档正文 **74 码点**的**合法答案** + `busy=""` ⇒ 判 `可用（匿名）` · **exit 0**（判据未误伤合法短答案）。**复现命令**：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -Round SMOKE9 -Providers kimi-web -SkipGates -SkipProbe`（物证见 §11 烟测行） | S3 判据加**第三口径**：归档正文 **≤ 120 码点 且命中已知错误短语**（`system is currently busy` / `please try again later` / `rate limit` / `too many requests` / `系统繁忙` / `服务繁忙` / `访问过于频繁` / `请求过于频繁` / `请稍后再试` / `稍后再试` / `服务暂不可用` / `服务异常`）⇒ 判「未真生成」：状态＝`失败（站侧忙碌/拒绝…）`、**重试 1 次照旧**、轮次 **exit 1**；报告 ① 新增一行「n3=done 但正文＝站侧错误串」+ 站点对象加 `busy` / `body_cps` 两字段 + 附录就地给处置（重跑 / 复核限流） |
| 7 | **「声明」被当成「实测」**（`capabilities.vision` / `verified` / `web.upload_adapter`） | 20 条条目里 `deepseek` 声明 `vision=false`，而其**官方 API 图片理解已真机实测通过**（`M7-09` / `M7-10`）；另有 6 条网页版条目「接线就绪但无真机物证」——只看 `providers.json` 会把这两种都写错 | 台账（`TST-L05` · §2.2）把**声明**与**实测**分两处：实测只认 `source/assets/site_capability_evidence.json`（+ `build/loop/*/report.json` 轮次物证）；冲突时实测优先、列内打 `⚠`、摘要计数（`-StatusOnly` 可复现）。**回填 `providers.json` 属产品侧动作，不在循环内做** |
| 8 | **门禁正则与真实输出不符 ⇒ 假 `FAIL`**（判据层缺陷：假绿会放过坏数据，假红会腐蚀「门禁全绿」的信任） | `SMOKE11-20261010-0943`：`provider-selftest` **`rc=0`** 却判 `FAIL` —— 旧正则 `(?m)^.*\d+\s*/\s*\d+.*$` 要求「数字–斜杠–数字」**紧邻**，而真实末行是 **`[配置表自检] 50 项通过 / 0 项失败`**（数字与 `/` 之间夹汉字）⇒ **永不匹配**。**复现命令**：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -Round X -SkipProbe -Providers ernie-web`（物证 `build\loop\X-*\gate\provider.txt` 末行 + `report.json` 的 `gates[]`） | 判据逐字取自真实输出：`(?m)^\[配置表自检\]\s*\d+\s*项通过\s*/\s*\d+\s*项失败\s*$`（源码 `loop_verify.ps1` ① 门禁段）；**同一物证**上「旧正则 0 命中 / 新正则命中 `[配置表自检] 50 项通过 / 0 项失败`」= 反向可打假；夹具三行（`12 / 0` 形态）不受影响 |



> **陷阱 2/3/6 的关系**：陷阱 2 让「只看 stdout」不可靠，陷阱 3 让「只看退出码」不可靠，陷阱 6 让「只看 `n3=done`」不可靠 ⇒ **唯一可靠组合 = 退出码 + 磁盘归档 n3 状态 + 归档正文自检**。这就是 §6.2 `可用（匿名）` 的判据来源。

> **陷阱 7 与「声明 ≠ 实测」的关系**：`providers.json` 里的 `capabilities.vision` / `verified` / `web.upload_adapter` 都是**声明**字段（产品侧接线用），**不能当实测结论**；`TST-L05` 台账把「实测证据」单独放 `source/assets/site_capability_evidence.json`，两者冲突时以**实测**为准并在能力列打 `⚠ 声明未回填`（例：`deepseek` 声明 `vision=false` 而图片理解已实测通过）。

> **陷阱 6 的保守性（如实登记局限）**：正文阈值取 **≤ 120 码点** ⇒ 若某站返回**合法答案**但整篇 ≤ 120 码点、又恰含上述短语（例如用户就是问「这个报错什么意思」），会被判成**假失败**。**当前校准**：`SMOKE10` 的 **74 码点合法答案未命中短语 ⇒ 判「可用」（无误伤）**；「合法答案恰含短语」的样本**仍无**，届时按「① 改用站点级更精确判据（前缀 / 容器选择器）② 阈值再降」修正。
> **新增陷阱时必须同时给**：① 复现命令 ② 物证路径 ③ 对策落点（本条即此格式）。


---

## §11 轮次表（每轮一行 · 结论权威位之一）

| 轮次 | 日期 | 可用（匿名） | 可用（登录后） | 需登录 | 失败 | 未测 | 报告物证 | 备注 |
|---|---|---|---|---|---|---|---|---|
| **R1** | 2026-10-09 | **5** | — | 1 | 5 | 0 | `build\loop\R1-20261009-2055\`（`report.md` · `probe.json` · `s3_summary.txt` · `raw\*.run.txt` · `va07_probe12`~`16.txt` · `gate\`） | 详见 [CHANGELOG](CHANGELOG.md)「R1 首轮」。S3 实测 **6 站**：`ernie-web` / `chatglm-web` 开轮即 `rc=0`；`kimi-web` / `doubao-web` / `tongyi-web` 修复 **VA-07 假阴性**后 `rc=0`（`va07_probe12/13`）；`失败` = `yuanbao-web`（站侧「发送未生效」，重跑成功 ⇒ 偶发）+ `qwen` / `chatgpt` / `claude` / `gemini`（环境级页面未就绪）；`需登录` = `spark-web`。⚠️ 本轮暴露**两个我方缺陷**（§5.1.1 两条 ④）：VA-07 按**字节**取样 + **退出码不区分生成失败**（陷阱 3/4）。门禁为**收轮后**复跑（R1 是手工轮，开轮未跑；自 R2 起由 `TST-L01` 开/收轮各跑一次） |
| 批 3 探针（**非轮次**） | 2026-10-09 | 8（探测级） | — | 2 | 1 | 0 | `build\b3_probe\`（`probe_all2.json` · `run4.txt`） | ⚠️ **不是** §11 定义的「一轮」：无 §1 授权、只做 S2 探测（**未做 S3 端到端**），故不计入循环轮次；`失败` = `gemini-web` 环境级（页面未就绪）· `需登录` = `spark-web` / `claude-web` |
| SMOKE7~10 烟测（**非轮次**） | 2026-10-09 | 1（`ernie-web` · 见 `SMOKE10`） | — | 0 | 1（`kimi-web`） | 0 | `build\loop\SMOKE7-20261009-2200\`（**修前**：`rc=0` + `n3=done` 被旧口径记「可用」）· `SMOKE8-20261009-2202\` / `SMOKE9-20261009-2203\`（**修后**：`失败（站侧忙碌/拒绝）` · exit 1）· `SMOKE10-20261009-2208\`（`ernie-web` 反面校准：正文 74 码点合法答案 ⇒ `可用` · exit 0） | ⚠️ **非轮次**：`-SkipGates -SkipProbe` 单站烟测（无门禁、无 S2）。**用途** = 打假第三口径（§10.1 陷阱 6）：`kimi-web` 归档正文 49 码点＝站侧「System is currently busy. Please try again later.」（重试 1 次仍复现 ⇒ 该站本轮**被限流**，属 §5.1 ③ 站侧）；`SMOKE7` 的假绿是真缺陷（已修），`SMOKE8/9` 的 `失败` 是**正确**结论，`SMOKE10` 证明判据**未误伤**合法短答案（本站计入「可用（匿名）」列仅示对照，**不计入轮次统计**） |
| SMOKE11~13 烟测（**非轮次** · `TST-L05` 全链路） | 2026-10-10 | 1（`ernie-web` × 3） | — | 0 | 0 | 0 | `build\loop\SMOKE11-20261010-0943\` · `SMOKE12-20261010-0945\` · `SMOKE13-20261010-0946\`（各自 `report.md`（含 **④ 站点台账**块）· `site_status.md` · `gate\`（`upload.txt` / `dump.txt` / `provider.txt`）） | ⚠️ **非轮次**：`-SkipProbe` 单站烟测，**用途 = 验 `TST-L05` 台账全链路 + 新门禁**。`SMOKE11`（门禁全开）**暴露门禁假 `FAIL`**（`provider-selftest` `rc=0` 却 FAIL ⇒ **陷阱 8**，物证 `SMOKE11-…\gate\provider.txt` 末行）；`SMOKE12`（修后**同一命令行**）= **门禁 FAIL 0** · `可用（匿名）` · **exit 0** · 台账 **14 轮**；`SMOKE13`（`-SkipGates`）复验幂等边界：轮末写出的 §2.2 哨兵块与随后一次 `-StatusOnly` 回写 **逐字一致**（`docs\loop_dev.md` SHA256 **不变**） |
| **T-L26**（**去重轮** · 只跑门禁 · `-SkipProbe`） | 2026-10-10 | — | — | 0 | 0 | 0 | `build\loop\T-L26-20261010-1523\`（`report.md` **四块** · `report.json` · `site_status.md` · `gate\`（`dump.txt` / `upload.txt` / `links.txt` …）） | 本轮**不探测**（站点列记「未测」）：用途 = **站点条目去重**（移除与 `qwen-web` 重复的 `tongyi-web`）后的**全门禁复核** —— 11 项门禁 **FAIL 0** · `provider-dump` = `条目 20 条（official 9 / web 11）` · 轮次 **exit 0**；台账 20 站（文字理解 ✅ 10 / 图片理解 ✅ 3）· 已移除条目只打印「忽略历史条目 id」告警 |


---

## §12 本文的维护

- 本文只放**循环纪律**（何时能开跑 / 怎么跑 / 什么算失败 / 记在哪 / 何时停），**技术细节一律外链**：站点数据 → `providers.json`；命令 → [../source/README.md](../source/README.md) §4；基线 → [../AI_README.md](../AI_README.md) §2 / [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §0.4。
- **改本文的口径要写清"为什么换"**（例：2026-10-09 从"必经登录"改为"匿名优先"）—— 与 [../AI_README.md](../AI_README.md) §3「口径变更四处同步」一致。
- 每轮结束**回填 §11 一行**；规则本身与站点结论**分开记**（规则进本文，结论进 `CHANGELOG` + 附录 E）。
