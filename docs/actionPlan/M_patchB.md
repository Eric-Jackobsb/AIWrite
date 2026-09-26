# M_patchB：Provider 可插拔化 —— 配置表（JSON）驱动 + 用户可自定义

> 类型：跨里程碑「地基补丁」第二期（**不占用 M1–M6 编号**，与 `M1.md … M6.md`、[M_patchA.md](M_patchA.md) 平级互链）
> 依据：2026-09-26 全库 provider / 推理链路审计（见 §1，逐条附**文件:行号**证据）
> 上游登记：本补丁是 [M_patchA.md](M_patchA.md) §4.1 **`PB-04` Provider 统一抽象**（`FEA-M4-04`）的**展开落地计划**；§12 决策 **D-06/D-07** 已确定「视觉模型走智谱、实现按 OpenAI 兼容规范」
> 用户目标（原话）：**「现在的 provider 是否已经模块化？我想设置任何 AI 的 web 或者 API 都能很容易实现？」**
> 用户补充指示（2026-09-26，**已确认，本文档已按此改写**）：
> ① **「使用配置表，而不是写进硬编码，并且允许用户自己配置。存储一个 json 用来管理配置表」**
> ② **「不仅仅是 api 驱动，也要对 web 模式进行同样方式处理」** —— 网页版（DeepSeek 网页版 / 任意站点）**与 API 共用同一张 JSON 配置表、同一套合并与覆盖规则、同一套校验/管理/自检入口**，且**站点 URL、探测路径、登录页、窗口标题、选择器全部去硬编码**
> 现状结论（一句话）：**OpenAI 兼容的 API 已经「能配出来」（零代码），但架构层未模块化；API 与网页版的厂商/站点元数据全部硬编码在 C++ 里，非兼容协议（Anthropic / Gemini / Azure）与「任意 AI 的网页版」都必须改源码。**
> 核心改造方向：**把「API 厂商」与「网页版站点」两种元数据统一搬进一份 JSON 配置表** —— 程序只负责**加载 / 合并 / 校验 / 分派 / 消费**；用户在自己的目录里加一份 JSON，即可接入新 API **或**新站点（**不改代码、不重编译**）。
> 状态：🟡 **待审核确认**（§6 为待确认决策清单；`D-11/D-12/D-15` 已按你的指示定稿，**web 与 API 同机制为硬性要求**）—— **补充（2026-09-26）**：`PB2-01…PB2-03`/`PB2-07`（离线部分）已落地，`PB2-04`/`PB2-05`/`PB2-06` 未开工；其中 `PB2-05` 的「网页版去硬编码」**部分未落地**、**多站点会话未开工**，见下方 **v4 复核块**
> 版本目标：v0.5.x（在已收口的 M5 核心切片之上补「推理后端可插拔」地基）
> 预计工期（估）：**L1 ≈ 2–2.5 天 · L2 ≈ 2–3 天 · L3 ≈ 5–8 天**（全职估算，含自检与文档；L1 含**网页版去硬编码**，故高于纯 API 方案）
>
> ✅ **已修复（2026-09-26 · L1 收口）**：下列 6 项缺口已全部落地 —— `PB2-17`（登录入口去硬编码）/ `PB2-18`（会话按站点键控）/
> `PB2-19`（窗口串行 + 按站点注销）/ `PB2-06`（`config.toml` 多 provider）＋ `PB2-05` 补完（`mode` 按 `kind` 过滤、运行前告警、
> `api_probe` 断言表驱动）。实测与基线见 §9「L1 收口」与 `docs/CHANGELOG.md`；**下方保留修复前的复核记录**供追溯。
>
> ⚠️ **2026-09-26 第二轮修订（v6 · 文档先行，零代码变更）**：上述收口里的「**`mode` 下拉按 `kind` 过滤**」与同批其它承诺冲突（`ai::web_spec_for()` 的「非网页版条目 → 回落内置默认站点」、`PB2-05` 的「缺省回落到内置默认 + 警告」），且把「官方条目 + `mode=web`」这条**合法路径变成不可达**（默认工作流的 `provider` = 表内第一项 = `deepseek`（official）→ 下拉里没有 `web`）。
> **已按用户指示定为回归并改规格**：**网页版与官方 API 同等优先级，`mode` 的 `official` / `web` 两项始终可选**；`kind` 只影响建议值与提示，**不裁剪候选、不静默改写**。
> 对应决策 **`D-21`**、不变量 **`I13`**、任务 **`PB2-20`**、批次 **`B2-a3`**、验收 **`AB2-15`**、验证 **`VB2-18`**、风险 **`R16`**；两处「§9 声明 ✅、代码未生效」见 §9.1「第二轮修订」。
> ✅ **2026-09-26 修订已落地（v7 · 代码批次）**：`PB2-20` 六项全做完（§9.1「落地实测」）—— `mode` 恒 `{official, web}`（不按 `kind` 裁剪、`kind` 与 `mode` 不一致**只提示不改写**）、面板与运行前校验按**节点自身条目**解析站点；新增断言 `VB2-18` **4/4 PASS**（`--exec-selftest` **204 / 0**）；`--run-selftest --web` **5/5（8.76s）**；构建 **0 error / 0 warning**（4 目标）
>
> ⚠️ **2026-09-26 第三轮复核（v8 · 文档先行，零代码变更）：站点恒 DeepSeek** —— 用户实测「不管选哪个 AI，网页版登录窗口都是 DeepSeek」。结论：**不是 v7 未修好**，而是「非网页版条目 → 回落表内第一个 web 条目（= `deepseek-web`）」这条**规格**（`ai/provider_spec.cpp:269-283`）＋「表里 `kind=web` 只有 1 条」＋「自建站点闭环缺失 / 模板与校验器 schema 不一致」三层叠加（逐条证据见 §9.3 与**附录 D**）。**本轮定稿（用户指示）**：`D-22` 选 **②**（**不回落**：明确报错 + 三条引导，运行期直接失败）、`D-23` 选 **A**（本轮只改文档：手写 `providers.d/*.json` + 重启）、`D-24` 本轮范围 = `PB2-22` + `PB2-23`；新增不变量 **`I14`**、任务 **`PB2-21`/`PB2-22`/`PB2-23`（+ `PB2-24` 后置）**、批次 **`B2-a4`**、验收 **`AB2-16`**、验证 **`VB2-19`**、风险 **`R17`/`R18`**；待确认 **`D-25`**（运行前是否阻断）/ **`D-26`**（`login_url` 缺失是否也禁止回落）
>
> ✅ **2026-09-26 修订已落地（v9 · 代码批次）**：`PB2-22`（**不回落**：严格站点解析 + 面板错误块/按钮禁用/一键改选 + 校验「本次运行必定失败」+ 运行期 `NodeError`）与 `PB2-23`（`solve_pow_via_page` 按站点 + `--web-probe --provider <id>`）全部完成；`PB2-21` 只做文档侧（**附录 D**），`PB2-24` 仍后置。实测：构建 **0 error / 0 warning**、`--exec-selftest` **209 / 0**（`VB2-19` 5 项全 PASS）、`--graph-selftest` 111/0、`--provider-selftest` 50/0、`--web-probe --provider zhipu` **exit 2 + 三条引导**；⚠️ `--run-selftest --web` 暂不能复测 5/5 —— **官网侧会话失效**（`userToken` 形状变化 + 端点 40003，**旧路径同样复现**，非本批引入）→ 见 **§9.4**，新任务 **`PB2-25`** / 风险 **`R19`·`R20`**
>
> ✅ **2026-09-26 内置站点入口已落地（v10 · 代码批次）**：`PB2-26`「**登录型站点条目**」（`adapter=dom` 缺生成字段 → **警告可加载**，生成未就绪**如实标记**；不填假选择器）+ **附录 E** 收录 **11 个 AI 的网页版登录入口**（Kimi / 通义 / Qwen / 智谱清言 / 豆包 / 元宝 / 文心 / 星火 / ChatGPT / Claude / Gemini；+ 已有 `deepseek-web` 共 12 条 web 条目）；实测 `--provider-dump` **21 条（official 9 / web 12）**、`--exec-selftest` **214 / 0**（`VB2-21` 5 项）、`--provider-selftest` 50/0、构建 **0 error / 0 warning**。**生成**仍需 L3（`PB2-13…16`）+ 选择器实测；`PB2-25` / `PB2-24` 未实施
>
> ✅ **2026-09-26 L3 已落地（v11 · 代码批次）：通用 DOM 站点适配器 + 选择器探测** —— 新增 `ai/dom_web_client.{h,cpp}`（**站点 = 纯数据**：`input_selector` / `send{kind,value|selector}` / `answer_selector` / `done_when` / `answer_poll_ms|max_polls`；轮询有硬上限，超时**如实返回已取文本 + 明确警告**，R13）+ `web::run_script_sync()`（窗口内同步执行脚本，**不把内存 `userToken` 当通用条件** —— DOM 站点登录态在浏览器 profile 里）+ `--web-adapter-selftest --provider <id>`（选择器探测/诊断：命中数 / `done_when` 现状 / `token_expr` 取值形状 / Cookie 可读性 + **可操作修复建议**）；`implemented_protocols`/`implemented_web_adapters` 增 **`dom`**。实测：构建 **0 error / 0 warning**、`--exec-selftest` **219 / 0**（`VB2-22` 5 项）、`--graph-selftest` 111/0、`--selftest` 七组 PASS、`--provider-selftest` 50/0、`--provider-dump` **21 条（official 9 / web 12）**、**`--web-adapter-selftest --provider kimi-web` 端到端跑通**（页面脚本真实执行：读出 `URL=https://www.kimi.com/` + 标题 + 4 条建议）；**发现**：`kimi.moonshot.cn` 会 301 到 **`www.kimi.com`**（条目已按最终域修正）。⚠️ **各站点选择器仍需逐站实测**（当前 11 条均为**登录型条目**：可登录/可探测，生成前需填选择器）
>
> ⚠️ **2026-09-26 第四轮复核（v12 · 文档先行，零代码变更）：登录层仍绑 DeepSeek → 用户实测「所有 AI 都无法登录，只能切回 DeepSeek」**
> 复核结论：**不是 L3 未完成** —— L3 只治了「**生成引擎**」（`ai/dom_web_client.cpp` + `web::run_script_sync()` 已做到站点=纯数据、**不要求内存 `userToken`**：`dom_chat()` 对「未取到内存凭证」**仅给警告、不阻断**，见 `ai/dom_web_client.cpp:194-202`），**没治「登录/会话判定层」与「站点数据（选择器）」**。三层叠加：
> ① **登录态判据是 DeepSeek 专有物**：`SessionStore::has_token()` = `user_token` 非空（`web/session_store.cpp:186-191`）→ 通用站点恒为 false → `ensure_session()` 走满 25s 超时（`web/webview_host.cpp:1301-1376`，文案「未取得网页版凭证（内存里没有 userToken）」）；面板恒显示「userToken：未获取（网页版接口需要它，请先登录）」（`ui/property_panel.cpp:237-239`）；Cookie 名回落 `ds_session_id`（`web/webview_host.h:32` / `ui/property_panel.cpp:126-130`）；状态栏只看内存槽（`ui/app.cpp:196-214`）。
> ② **探测脚本对非 DeepSeek 站点注入 DeepSeek 端点**：`probe_kickoff_script()` 在 `probe_paths` / `token_expr` 为空时**保持内置值**（`/api/v0/users/current`、`/api/v0/chat_session/fetch_page`、`localStorage.getItem('userToken')`，`web/webview_host.cpp:1060-1086`）→ 在 Kimi / 通义等站点**必然 404**，面板出现红色「探测错误」，被读作「登录失败」。
> ③ **11 条内置站点没有选择器**（`source/assets/providers.json:258-434` → `ai::web_login_only()`，`ai/provider_spec.cpp:404-411`）→ 运行期明确报「登录型条目（缺生成字段）」（`nodes/local_nodes.cpp:429-435`）；且**界面没有填选择器的入口**（`PB2-24` 未做；本机 `~/.brain-ai/providers.d` 为空目录）。
> **本轮定稿**：新增层 **L4**（登录/会话层去厂商化 + 站点数据落地 + 自助闭环）、任务 **`PB2-27`…`PB2-30`**（`PB2-24` 并入）、批次 **`B2-e`**、不变量 **`I15`/`I16`**、验收 **`AB2-20`…`AB2-22`**、验证 **`VB2-24`…`VB2-26`**、风险 **`R21`…`R23`**；待确认 **`D-27`**（登录态判据）/ **`D-28`**（`userToken` 显示策略）/ **`D-29`**（归口）
>
> 🟡 **2026-09-26 L4 立项（v12 · 文档先行，零代码变更）**：登录/会话层去 DeepSeek 化（站点无关「已登录」判据 + 探测「不适用」语义）+ 内置站点**逐站选择器实测回填** + 自建站点 UI 闭环（`PB2-24` 并入）。**未开工**；`B2-c`（L3）已 ✅，**代码与数据改动待 `B2-e`**（§9.7）
>
> ✅ **2026-09-27 `PB2-27` 已落地（v13 · 代码批次）**：登录/会话层的「判据 + 文案」去 DeepSeek 化 —— 新增纯函数 `ai::web_session_state(spec, evidence)`（判据 = 条目 `cookie_names` 命中 **∪** 该 origin Cookie 非空，**不看** `userToken`，`D-27`/`I15`）+ `ai::probe_is_applicable()`（DOM 站点**协议探测不适用**，`I16`）+ `ai::web_shows_user_token()`（`D-28`①）；`web::ensure_session()` 对 DOM 站点改用**站点无关**就绪判据（**不再空等 15/25 s 的 `userToken`**）；面板/状态栏/字段警告/加载警告文案全部站点无关；`ui/**` 与 `web/**` 已无任何厂商专有 Cookie 名（删除 `kDefaultCookieName`）。实测：构建 **0 error / 0 warning**、`--exec-selftest` **219 → 227 / 0**（`VB2-24` 5 项 + `VB2-26` 3 项全 PASS）、`--graph-selftest` **111 / 0**、`--provider-selftest` **50 / 0**（§9.8）。`PB2-28`（探测只读诊断分支 + CLI 判据）、`PB2-29`（逐站选择器）、`PB2-30` 待做
> ✅ **2026-09-27 `PB2-28` 已落地（v14 · 代码批次，①②③）**：**协议探测「不适用」语义** —— 新增只读诊断脚本 `kProbeKickoffScriptReadOnly`（读 URL / 标题 / localStorage 键名与个数 / Cookie 名与个数 / 输入框与按钮候选数；**脚本内既无 `/api/v0/` 也无 `localStorage.getItem('userToken')`**，输出结构与内置脚本同名 → 解析逻辑零改动）；`probe_kickoff_script()` 按 `probe_applicable` 分支（默认 `true` → 内置站点与 CLI/无参路径**逐字不变**，守 `I2`）；CLI `--web-probe --provider <id>` 与面板按钮对不适用站点改打「协议探测：**不适用**」+ 只读诊断 + **站点无关**登录态结论（退出码 0=已登录 / 2=未登录 / 1=诊断失败）。实测：构建 **0/0**、`--exec-selftest` **227 → 232 / 0**（`VB2-25` 5 项全 PASS）、`--graph-selftest` **111/0**、`--provider-selftest` **50/0**（§9.9）。④ 会话失效识别（`40002`/`401`/`40003`）仍归 `PB2-25` 待做；`PB2-29`（逐站选择器）、`PB2-30` 待做
> ✅ **2026-09-27 v15 收口（代码批次）**：① **`PB2-28`④ 会话失效识别落地**（`ai::web_session_failure_hint()` 纯函数 + `web_chat()` 命中 `401`/`40002` → 作废该站点内存会话并给重新登录指引；`40003` → 只提示）② **`PB2-30`① 落地**（`--run-selftest --web --provider <id>` 端到端断言，严格解析不回落）③ **新增只读工具 `--web-dom-dump`**（枚举页面候选 input / 发送 / 回答容器 + 建议选择器 → 让逐站填选择器不再依赖人肉 F12，`PB2-29` 的执行工具）④ **修复一处 CLI 崩溃**：`ExecuteScript` 返回值解包缺失 → `type_error.306` 未捕获 → `std::terminate`/`__fastfail`（0xC0000409，且 stdout 缓冲全丢）；已统一解包 + try/catch 兜底。实测：构建 **0/0**、`--exec-selftest` **232 → 237 / 0**（`VB2-27` 5 项全 PASS）、`--graph-selftest` 111/0、`--provider-selftest` 50/0、`--web-dom-dump --provider kimi-web` exit 0 并读出真实候选（输入框 = `div.chat-input-editor`）—— 但**发现**：未登录的 Kimi 也有 4 条**匿名 Cookie** → 仅按「Cookie 非空」判「已登录」会误报（见 §9.10「待拍板 `D-30`」）
>

> **⚠️ 2026-09-26 复核（v4 · 文档先行批次，当时只改文档、零代码变更）**：`PB2-05` 的「网页版去硬编码（逐处替换清单）」**第 1、2 行未落地**，且**「多站点（多份 Cookie）并存」尚不具备条件**。缺口已登记为新任务 **`PB2-17` / `PB2-18` / `PB2-19`**（§3，状态全部 ⬜ 未开工），并由新增不变量 **`I11`/`I12`**、验收 **`AB2-13`/`AB2-14`**、验证 **`VB2-16`/`VB2-17`**、决策 **`D-19`/`D-20`**、风险 **`R14`/`R15`** 约束（复核证据见 §9「B2-b 前置复核」）：
> ① **登录 URL / 窗口标题仍硬编码** —— `web/webview_host.h:68-75`（`interactive_login_request()` 返回固定 `https://chat.deepseek.com/` + 固定标题）；面板入口 `ui/property_panel.cpp:105`（`draw_web_session_section()` **无参**，调用点 `:644` 不传节点）→ `:155` / `:183-186`（探测按钮内联 DeepSeek URL）/ `:119`（固定查 `ds_session_id`）/ `:648`（文案写死）；
> ② **运行时自动引导走默认站点** —— `web/webview_host.cpp:1107-1110` 的 `ensure_session()` 直接 `LoginRequest request;`（默认值 = `webview_host.h:23` 的 DeepSeek URL）；
> ③ **会话是「单槽」而非「按站点」** —— `web/session_store.h:64-82`（一个 `Session`，`set()` 覆盖 → 第二站点会冲掉第一站点）、登录窗口**进程内单例**（`web/webview_host.h:129`、`webview_host.cpp:936-943`）、注销 `remove_all(~/.brain-ai/webview2)`（`ui/property_panel.cpp:88-102`，**一次注销清掉所有站点**）；
> ④ **`mode` 下拉未按 `kind` 过滤**（⚠️ **2026-09-26 按决策 `D-21` 改判**：「无条件给 `{official, web}`」**本身就是正确行为**（网页版与 API 同等优先级），本条真正要修的是「**静默**落回内置默认」→ 改为**显式橙色提示**；**不裁剪候选、不静默改写**） —— `engine/node_registry.cpp:319` 仍无条件给 `{official, web}`，选「official 条目 + `mode=web`」会静默落回 `ProviderWebEndpoints` 内置默认（= DeepSeek 端点，`ai/provider_spec.h:53-58`）；
> ⑤ **内置表 `kind=web` 仅 `deepseek-web` 1 条**（Kimi 只是顶层 `_example_web_dom` **模板**，`_` 前缀键不参与加载；本机 `~/.brain-ai/providers.d/` 为空、亦无 `~/.brain-ai/providers.json`）—— 即「切换提供商 + 选网页版仍打开 DeepSeek」既有硬编码原因，也有**表中确实只有一个站点条目**的原因；
> ⑥ **既有断言把硬编码钉死** —— `tools/api_probe.cpp:314-316` 断言 `interactive.url.find("deepseek.com")`，必须随 `PB2-17` 改为表驱动双向断言（见 `R15`）。

---

## §0 文档元信息与约定

### 0.1 目的（为什么做这件事）

现在「换一家 AI」只有两条路：① 改界面上的 `API 地址` + `模型（自定义）`（仅当对方兼容 OpenAI）；② 改源码。
本项目要长期演进（M6 之后还有更多节点与场景），若不把推理后端抽象出来，**每接一家厂商都会污染节点执行逻辑**，并且「网页版」永远是单点实现。

**本文档的目的**：把「接入一个新的 AI」从**改代码**降级为**改一份 JSON**，并把必须写代码的部分收敛到**一个明确的扩展点**（新增一个 Provider 类或一个站点适配器）。

**本补丁的第一性原则（用户指示）**

| 原则 | 含义 |
|---|---|
| **配置表优先（Data over Code）** | 厂商与站点元数据（地址 / 路径 / 认证 / 候选模型 / 能力 / 环境变量名 / 凭据引用名 / **网页版站点 URL、探测路径、选择器**）**一律放 JSON**；C++ 里**不再出现**具体厂商或站点的常量 |
| **API 与网页版同机制（同表 · 同规则 · 同入口）** | `kind: "official"` 与 `kind: "web"` 是**同一张表的两种条目**：同一份加载/合并/校验代码、同一套用户覆盖目录、同一个 UI 管理区（打开/重载）、同一个自检命令（`--provider-selftest` 覆盖两类） |
| **用户可配置** | 用户在自己的目录（`~/.brain-ai/providers.json`、`~/.brain-ai/providers.d/*.json`）新增或覆盖条目即可接入新 AI **或新网站**，**不需要改代码、不需要重编译、不需要管理员权限** |
| **单一数据源** | 随程序发布的 `assets/providers.json` 是**唯一权威配置表**；文档、UI 下拉、执行调用、站点探测都从它派生（不重复维护） |
| **失败不致命** | 表坏了 / 缺了 / 某条字段不合法 → 明确报错 + 用「上一份可用表 / 最小兜底」继续可用，绝不崩溃、绝不静默改变行为 |

### 0.2 编号规则（与 `M_patchA` 的 `PB-xx` 严格区分）

| 前缀 | 含义 | 示例 |
|---|---|---|
| `PB2-` | 本补丁任务（Patch B 第二期） | `PB2-03` |
| `VB2-` | 技术验证 / 离线断言项 | `VB2-01` |
| `AB2-` | 验收标准 | `AB2-02` |
| `D-xx` | 决策记录（与 `M_patchA` §12 的编号**连续**） | `D-15` |

> ⚠️ `M_patchA` 里的 `PB-01…PB-09` 是**第一期**（线程化 / 流式 / 凭据 / 官方 Provider），本文档**不重开**那些编号；`PB-04` 只在 §1.4 作为「上游登记项」被引用。

### 0.3 范围与非范围

**做（In scope）**

- **`assets/providers.json` 配置表**（随程序发布，唯一权威数据文件）+ 加载 / 合并 / 校验 / 兜底
- **两类条目同等对待**：`kind: "official"`（API）与 `kind: "web"`（网页版站点）走**同一套**机制
- **用户自定义层**：`~/.brain-ai/providers.json`（字段覆盖）+ `~/.brain-ai/providers.d/*.json`（新增条目，便于分享单文件）——**API 与站点都适用**
- UI 管理入口：**打开配置表 / 打开所在文件夹 / 重新加载配置表**（热重载）+ 条目来源标注（内置 / 用户覆盖）
- **API 去硬编码**：端点 / 认证 / env 名 / 超时全部按表取值
- **网页版去硬编码**：登录页 URL、登录窗口标题、站点 host、探测路径、PoW/会话端点、站点显示名全部按表取值（现有 DeepSeek 网页版行为不变）
- 「测试连接」+ `--provider-selftest`（**含配置表校验；API 与 web 两条路径都覆盖**）
- 推理后端**接口 + 工厂**（设计 §8.1；按表的 `protocol` / `web.adapter` 分派；把现有 API 与网页版两个实现收编）
- 至少再落地 **1 家非 OpenAI 协议**的 Provider 类（Anthropic 或 Gemini，见 §6）
- **通用网页版适配器（DOM 驱动）**：站点表 + 选择器探测/诊断工具（`--web-adapter-selftest`）
- **登录/会话层去厂商化（L4）**：「已登录 / 未登录 / 需重新登录」的判据与界面文案**站点无关**（不依赖 `userToken` / `ds_session_id` / `/api/v0/*`）；非 DeepSeek 站点不再被注入 DeepSeek 探测端点（如实表达「协议探测不适用」）；内置站点选择器**逐站实测回填**；自建站点「新建 → 重载 → 登录 → 生成」界面闭环

**不做（Out of scope，明确登记避免发散）**

| 不做 | 原因 |
|---|---|
| AutoProvider（自动选择后端）/ 多 Key 轮询 / 失败自动换厂商 | 设计 §8.2 明确「**无 AutoProvider**（T-07 已取消 auto）」 |
| 模型市场 / 计费统计 / 用量报表 | 与「接入容易」无关，另立项 |
| 把网页版做成「零维护」 | 站点改版与反爬必然发生，只能做到「**用户改 JSON 自助修复** + 可诊断 + 明确报错」 |
| 绕过站点验证 / 逆向反爬（验证码、风控） | 安全与合规红线；适配器只操作用户**已登录页面**的 DOM |
| 图形化 provider / 站点编辑器（点选生成 JSON） | 手改 JSON 已足够简单；图形化留给 M6-01 设置面板评估 |
| 表里存 API Key / Cookie / Token | **安全红线**：表只存「环境变量名 + 凭据引用名 + 取 token 的页面表达式」；明文密钥一律走 `utils/credential`（DPAPI），日志脱敏 |
| 引入新第三方依赖（HTTP/JSON/浏览器自动化库） | 沿用 httplib / nlohmann / OpenSSL / WebView2（`M_patchA` §0.3 原则 5；**nlohmann 已在用，读 JSON 表零新增依赖**） |
| 在线下载 / 远程同步配置表 | 先做本地文件；远程仓库属后期（未登记） |

### 0.4 执行原则（沿用 `M_patchA` §0.3，本补丁追加 3 条）

1. **运行态值 vs 文档值**：执行结果永不进 `Graph` / 撤销快照 / 工作流 JSON。
2. **只读暴露**：UI 只读执行产物；写入走 `EditorState` 快照事务。
3. **脱敏**：API Key / Cookie / userToken 只驻留内存，日志与归档一律脱敏。
4. **一次提交一个补丁**：独立可验证、可回滚；提交信息附实测数据。
5. **不引入新依赖**。
6. **（新）请求体逐字节兼容**：改造后**文本请求体必须与改造前完全一致**（字段集合一致、`stream=false` 不变），保证既有 `--exec-selftest` 请求体断言**不改一行仍然通过**。
7. **（新）离线可断言优先**：加载 / 合并 / 校验 / 端点拼接 / 认证头 / 能力门控 / 响应解析**全部是纯函数**，无网络、无 Key 即可断言。
8. **（新）表是数据，不是代码**：C++ 源码中**不允许**出现具体厂商的地址/模型/认证常量（唯一例外：§7 附录 B 的**最小兜底表**，仅 2 条，用于「配置表文件丢失」的降级）。

### 0.5 回归基线（每个提交必须重跑并贴结果）

| 命令 | 期望（本文档开工前基线，2026-09-26 实测） |
|---|---|
| `api_probe.exe --selftest` | 七组全 PASS |
| `api_probe.exe --graph-selftest` | 111 通过 / 0 失败 |
| `api_probe.exe --exec-selftest` | **120 通过 / 0 失败**（本补丁**只增不减**） |
| `aiwrite.exe --run-selftest` | PASS（离线 3/5 为预期） |
| `aiwrite.exe --run-selftest --web` | 5/5 |
| `aiwrite.exe --cred-selftest` / `--export-selftest` | 8/8 / 7/7 PASS |
| `aiwrite.exe --vlm-selftest` | 离线断言 PASS（无 Key 退出码 2） |
| `aiwrite.exe --web-probe` / `--web-chat "<提示词>"` | PASS（**本补丁不得改变其行为**） |
| `aiwrite.exe --web-session-selftest` | PASS |
| 构建 | 0 error / 0 warning |
| **新增（本补丁）** | `aiwrite.exe --provider-selftest`（见 §3 `PB2-07`） |

---

## §1 现状体检（审计，2026-09-26）

审计方式：全库检索 provider / 端点 / 认证 / 环境变量 / 站点常量，逐条追踪调用链（节点 → ai → web），并检查是否存在任何厂商元数据文件。

### 1.1 推理链路的实际结构（改造前）

```
ui/property_panel.cpp ──显示「生效提供商」──┐
ui/node_canvas.cpp ───────显示摘要──────────┤
ui/toolbar.cpp ───────────运行前预检────────┤→ engine/provider_resolve.cpp（仅解析 + 提示，不调用）
engine/validate.cpp ──────校验 Key──────────┘

engine/executor.cpp（工作线程）
   └─ nodes/local_nodes.cpp
        ├─ execute_llm_generate()   ← if (mode != "web") { ai::official_chat() } else { ai::web_chat() }
        └─ execute_vlm_generate()   ← if (mode == "web") throw ...; ai::official_chat()

ai/deepseek_official_provider.cpp   official_chat()  → httplib POST {api_base}/chat/completions（Bearer）
ai/deepseek_web_client.cpp          web_chat()       → chat.deepseek.com + PoW(SHA3) + SSE
ai/web_pow.cpp                      （DeepSeek 专有挑战求解）
web/webview_host.cpp                （DeepSeek 网页版登录 + 页面内协议探测）
```

**关键事实**：`ai/` 目录里**只有两个具体实现**，没有接口、没有工厂、没有注册表；分派逻辑写在**节点实现**里（`local_nodes.cpp`）；**厂商元数据 100% 硬编码在 .cpp/.h 中**。

### 1.2 七个硬编码点（"不模块化"的根因）

| # | 硬编码内容 | 证据（文件:行） | 后果 |
|---|---|---|---|
| 1 | **后端分派**写死在节点里 | `nodes/local_nodes.cpp:291`（`if (mode != "web")`）、`:416`（`if (mode == "web") throw`） | 新增第三种后端要改节点执行逻辑 |
| 2 | **端点路径**固定 `/chat/completions` | `ai/deepseek_official_provider.cpp:198-202`、`:232-234` | 接不了 Anthropic `/v1/messages`、Gemini `:generateContent`、Azure `?api-version=` |
| 3 | **认证头**固定 `Authorization: Bearer` | `ai/deepseek_official_provider.cpp:239-241` | 接不了 Azure（`api-key:`）、Anthropic（`x-api-key` + `anthropic-version`） |
| 4 | **响应解析**固定 `choices[0].message.content` | `ai/deepseek_official_provider.cpp:260-276` | Anthropic（`content[0].text`）/ Gemini（`candidates[0].content.parts[0].text`）解析不出文本 |
| 5 | **多模态格式**固定 OpenAI `image_url` + data URL | `ai/deepseek_official_provider.cpp:188-195` | Anthropic（`source.base64`）/ Gemini（`inline_data`）需另一套映射 |
| 6 | **环境变量名**固定 `DEEPSEEK_API_KEY`（全库 **15 处**：**4 处 `getenv` 取值 + 11 处文案/注释**） | 取值：`deepseek_official_provider.cpp:209`、`engine/provider_resolve.cpp:97`、`engine/validate.cpp:144`、`utils/credential.cpp:364`；文案：`main.cpp:813`、`deepseek_official_provider.cpp:33`、`provider_resolve.cpp:86,122,124`、`validate.cpp:148`、`local_nodes.cpp:57,310,453` 等 | 换厂商时 env 兜底失效；提示文案也误导 |
| 7 | **网页版站点**全写死（host / 路径 / Cookie / PoW） | `ai/deepseek_web_client.cpp:15-17`、`ai/web_pow.h:7-13`、`web/webview_host.h:22,62`、`web/webview_host.cpp:150-163`、`ui/property_panel.cpp:184` | 换一个网站 = 重写一整套逆向客户端 |

### 1.3 `provider` 字段的真相：**装饰性字段**

| 事实 | 证据 |
|---|---|
| 参数定义只有一个枚举值 | `engine/node_registry.cpp:303` `enum_param("provider","提供商",{"deepseek"},"deepseek")` |
| 全仓库**没有任何地方按它分派** | 检索 `effective.provider` / `.provider ==` → **零命中**；取值后仅用于界面显示（`ui/property_panel.cpp:482-529`、`ui/node_canvas.cpp`） |
| 真正生效的是 `mode` + `api_base` + `model_custom` | `nodes/local_nodes.cpp:272-277`（mode/model 从 provider 输入句柄取）、`:294`（api_base）、`:227-231`（model_custom 覆盖枚举） |

> 即：**「提供商」下拉今天选什么都不会影响调用结果**；换厂商靠手填 `API 地址` + `模型（自定义）`。

### 1.4 配置载体现状（本补丁的直接靶心）：**没有任何厂商元数据文件**

| 项 | 现状 | 证据 |
|---|---|---|
| 厂商元数据文件 | **不存在**（全库无 `providers.json` / 无 provider 相关 `*.json`）；厂商地址、模型名、认证方式、环境变量名**全部在 .cpp/.h 里** | 见 §1.2 七项；`source/assets/` 目前只有 `fonts/`、`icons/`、`images/`（`assets/images/sample.png` 为 M5 新增） |
| `config.toml` | 只有 `[providers.deepseek]` **单节**（存的是「实例参数」，不是「厂商元数据」）；`Config::Provider` 是普通结构体字段 `Config::deepseek`（**不是 map**） | `utils/config.h:66-80`、`utils/config.cpp:180-189`、`:217-219` |
| 官方说明 | 自陈「`providers.deepseek.*` **仅作默认值来源**，实际以 ProviderConfig 节点参数为准」 | `utils/config.h:96-105`（`unwired_config_fields`） |
| Key 引用名默认值 | 写死 `brain-ai/deepseek`（换厂商会串味 / 可能复用错条目） | `engine/node_registry.cpp:315` |
| 超时 / 重试 | 代码写死 15s / 180s；`config.timeout.*`、`config.error.*` **零消费** | `ai/deepseek_official_provider.cpp:236-238`、`utils/config.h:48-58`、审计项 `FEA-M4-13` |
| 资源目录约定（可复用） | `paths::assets_dir()` = `<exe_dir>/assets` **已存在**（当前未被 provider 使用） | `utils/paths.h:29` |
| 上游登记项 | `PB-04` Provider 统一抽象（`FEA-M4-04`）：`ai/inference_provider.h` + `ai/provider_factory.{h,cpp}` + **把 `web_chat()` 收编为 `DeepSeekWebProvider`**（保持 `--web-chat` 行为不变） | `M_patchA.md` §4.1（PB-04 行）、§2 剩余汇总行 |
| 设计文档 | §8.1 已给出 `InferenceProvider{generate, generateStream, generateWithImage, name, supportsVision}`；§8.2 后端类型；§8.3 后端选择（official / web） | `ai_writer_nodes.md:550-582`、`:136-153` |

> **结论**：今天既没有「配置表」，也谈不上「用户可配置」—— 用户能改的只有单个工作流节点上的 4 个字段（`mode/api_base/model_custom/api_key`）。
> 本补丁把**厂商元数据**抽到 JSON 表（可被用户覆盖/扩充），把**实例参数**留在 `config.toml` 与工作流节点里，两层职责分清。

### 1.5 三类「换个 AI」的真实成本（今天 vs 目标）

| 场景 | 今天 | 目标（本补丁后） |
|---|---|---|
| OpenAI 兼容 API（智谱 / 硅基流动 / Ollama / OpenRouter / vLLM） | **零代码**，但要在界面手抄 URL 与模型名；无候选提示、无连通性测试；接错只在运行时才知道 | **配置表里已内置**，下拉选厂商 → 默认地址 / 模型 / Key 引用名**自动带出** → 点「测试连接」即时验证 |
| 表里没有的新厂商（仍兼容 OpenAI） | 手抄配置 | **用户自己写 10 行 JSON**（`~/.brain-ai/providers.d/my-ai.json`）→ 重启/重载后出现在下拉里，**零代码、零重编译** |
| 非兼容 API（Anthropic / Gemini / Azure OpenAI） | **必须改源码**（端点 + 认证 + body + 响应 4 处硬编码） | 配置表声明 `protocol`；协议差异收敛到一个 Provider 类（约 120–180 行纯新增，**不改节点**） |
| 任意 AI 的**网页版**（Kimi / 通义 / 豆包 / ChatGPT …） | **必须新写一套逆向客户端**（登录 + 协议 + PoW/SSE + UI 入口，数百行） | 无 PoW 站点：**一份站点 JSON**（选择器 + 登录 URL）；有 PoW/反爬的站点：以 `DeepSeekWebProvider` 为模板抄一个类 |

### 1.6 现状评级（可扩展性）

| 维度 | 评级 | 说明 |
|---|---|---|
| 加一家 **OpenAI 兼容 API** | 🟢 可配 | 无需编译，但体验差（手抄 + 无测试） |
| 加一家 **非兼容 API** | 🔴 需改代码 | 4 处硬编码协议假设 |
| 加一个 **网页版站点** | 🔴 需重写 | 单点实现，无适配器概念 |
| **厂商元数据** 可配置性 | 🔴 **无** | 没有配置文件，全在 C++ 里 |
| **UI / 校验 / 提示** 随厂商扩展 | 🔴 硬编码 | 「提供商」枚举、env 名、错误文案均写死 DeepSeek |
| **配置 / 密钥** 随厂商扩展 | 🔴 单节 | `[providers.deepseek]` 固定；ref 默认写死 |

> **结论**：今天的状态是「**OpenAI 兼容 + 手抄配置**」能用；离「设置任何 AI 的 web 或 API 都很容易」还差三层 —— 这正是 §2/§3 要补的。

---

## §2 目标形态

### 2.1 架构对比

**改造前**（今天）

```
厂商元数据：写死在 C++ 里（地址/模型/认证/env 名）
ProviderConfig 节点参数（provider 枚举无用 / mode / api_base / model_custom / api_key / ref）
        │  provider 句柄（JSON）
        ▼
nodes/local_nodes.cpp ──if(mode=="web")──► ai::web_chat()      ← DeepSeek 专有，写死
                     └──else─────────────► ai::official_chat() ← OpenAI 兼容，写死
```

**改造后**（目标：`assets/providers.json` + 用户覆盖层 → 配置表 → 工厂 → 具体 Provider）

```
┌─ 配置表（JSON，唯一的厂商元数据 + 站点元数据来源）────────────────┐
│  ① <exe>/assets/providers.json      随程序发布：official + web 条目│
│  ② ~/.brain-ai/providers.d/*.json   用户新增条目（API / 站点）     │
│  ③ ~/.brain-ai/providers.json       用户字段级覆盖（API / 站点）   │
│  ④ C++ 最小兜底（仅 ① 缺失时：custom-official + deepseek）        │
└──────────────────────────┬───────────────────────────────────────┘
                           ▼  ai::load_provider_specs()  ← 纯函数：加载/合并/校验（两类同一套）
                std::vector<ProviderSpec>（kind=official / web + 来源标注）
                           ├───────────────────────────────────┐
        （official）▼                                        （web）▼
  ProviderConfig 节点参数 → provider_resolve → 工厂        登录窗口 / 协议探测 / 网页版会话
        │                                        │  login_url · window_title · 探测与 PoW 端点
        ▼                                        ▼  全部取自表（不再写死 chat.deepseek.com）
  nodes/local_nodes.cpp ──► ai::make_provider(spec, mode, options)  ← 唯一分派点（工厂）
                                │
                                ├─ OpenAICompatibleProvider  （protocol=openai：DeepSeek/智谱/硅基/Ollama/OpenRouter/Azure/任意用户条目）
                                ├─ AnthropicProvider         （protocol=anthropic）
                                ├─ GeminiProvider            （protocol=gemini）
                                ├─ DeepSeekWebProvider       （web.adapter=builtin:deepseek；现有 PoW + SSE 收编）
                                └─ WebDomAdapter             （web.adapter=dom；选择器驱动的通用站点适配器）
```

> 关键点：**新增一家厂商 = 新增一份 JSON**（不改 C++）；**换/修一个网页版站点 = 改一份 JSON**（内置适配器站点除 URL/路径外，只有出现**新协议形态**时才需要新类）。

### 2.2 三层能力模型（分层做，可独立交付）

| 层 | 名称 | 解决什么 | 加一家新 AI / 新站点的成本 | 估时 | 风险 |
|---|---|---|---|---|---|
| **L1** | **JSON 配置表（API 与网页版同表同机制）** | 两类条目的加载 / 合并 / 校验 / 用户覆盖 / 打包；**API 去硬编码**（端点·认证·env·超时）+ **网页版去硬编码**（登录页·窗口标题·站点 host·探测与 PoW 端点·显示名） | 内置条目：**下拉即用**；用户**写 JSON 即可接入新 OpenAI 兼容 API**；**改 JSON 即可换站点/改登录页/改探测路径**（DeepSeek 网页版） | 2–2.5 天 | 低–中（web 去硬编码需回归 `--web-chat` / `--web-session-selftest`） |
| **L2** | **协议 / 适配器实现层（工厂统一分派）** | 按表 `protocol` / `web.adapter` 造对象；把现有 API 与网页版实现收编；新增 Anthropic / Gemini | 非兼容协议：**+1 个类（~150 行），节点零改动**；新形态网页版站：**+1 个适配器类** | 2–3 天 | 低–中（`--web-chat` 行为必须不变） |
| **L3** | **通用 DOM 适配器（任意站点 → 纯 JSON）** | 选择器驱动的「填输入 → 触发发送 → 轮询答案」执行器 + 选择器探测/诊断工具（**表与覆盖机制已在 L1/L2 内**，L3 只补执行器与诊断） | **一份 JSON（~15 行）**接入无 PoW 站点；站点改版 → **用户自助改 JSON 修复** | 5–8 天 | 中–高（站点改版 / WebView2 现场手测） |
| **L4** | **登录/会话层去厂商化 + 站点数据落地 + 自助闭环** | 「已登录」判据与界面文案不再依赖 `userToken` / `ds_session_id` / `/api/v0/*`（`web_session_state()` 纯函数）；探测脚本对通用站点**不注入** DeepSeek 端点（如实说「不适用」）；内置站点**逐站实测选择器**；界面「新建站点条目 + 重新加载 + 测试选择器」 | **新站点 = 填一次选择器（实测）+ 一份 JSON**，之后**登录即用**；不改 C++、不重编译、不需要理解 DeepSeek 协议 | 3–5 天（站点越多越久） | 中（站点 DOM / 登录墙会变；收益 = 把「所有 AI 都无法登录」变成「登录即用」） |

> **四层互相不依赖但语义连续**：L1 交付后「API 与网页版都已由表驱动、都可用户自定义」；L2 把协议打开（含把 DeepSeek 网页版实现收编为按表工作）；L3 把「任意站点」降级为纯数据；**L4 把「登录即用」做成真**（登录态判据与文案站点无关 + 探测「不适用」语义 + 站点选择器数据 + 自助闭环）。
> **推荐**：L1+L2 一次做完（约 4–5.5 天）——此时**API 与网页版两条路都已完成「表驱动 + 可扩展」**；L3 单独立项、按需推进；**L4 紧随 L3（`B2-e`）** —— 否则「非 DeepSeek 站点看起来无法登录」（用户实测反馈）。

### 2.3 不变量（Invariants，三层共同遵守）

| # | 不变量 | 验证方式 |
|---|---|---|
| I1 | 改造后**文本请求体与改造前逐字节一致** | 复用现有 `--exec-selftest` 请求体断言（**不改一行**必须仍 PASS） |
| I2 | `--web-chat` / `--web-probe` / `--web-session-selftest` 行为不变 | 三个自检命令 |
| I3 | Key 三级优先级与「填一次自动入库」「日志脱敏」不变；**配置表内不含明文密钥** | `--cred-selftest` 8/8 + 表校验断言 |
| I4 | 新增的每一条数据/协议映射都有**离线断言** | `--exec-selftest`（api_probe）/ `--provider-selftest`（aiwrite） |
| I5 | 任何「未实现的组合」都必须**明确报错 + 给操作步骤**，绝不静默失败 | 既有 `unwired_reason` / VLM web 报错风格延续 |
| I6 | 不新增第三方依赖；不需要管理员权限；不写盘明文密钥 | 构建 + `--cred-selftest` |
| I7 | **用户新增/覆盖条目无需改代码**：只写 JSON，功能即可用（新协议、新适配器形态除外） | `AB2-09` 验收 |
| I8 | **表坏/缺失不致命**：报错 + 用上一份可用表或最小兜底继续运行 | `AB2-10` 验收 |
| I9 | **网页版同样无硬编码**：`web/**`、`ai/deepseek_web_client.cpp` 中**不出现**站点 URL / 探测路径 / 窗口标题常量（全部来自表）；只改表即可改登录页与探测目标 | `AB2-11` + `grep` 审查 |
| I10 | **两类条目同待遇**：web 条目与 official 条目一样支持「用户新增 / 字段级覆盖 / 重新加载 / 自检 / 来源标注」 | `AB2-12` + `--provider-selftest` |
| I11 | **网页版站点身份唯一来源 = 生效条目**：登录页 URL、登录窗口标题、页面内探测路径、站点端点、取 token 表达式（`token_expr`）、Cookie 名（`cookie_names`）**一律取自生效条目**；内置 `deepseek-web` 的字段值与改造前常量**逐字一致**（故 `--web-probe` / `--web-chat` / `--web-session-selftest` 结果不变） | `AB2-13` + `VB2-17` + `grep` 审查 |
| I12 | **会话按站点独立**：任一站点的登录 / 探测 / 注销 / 会话失效**不影响**其他站点的内存会话；同一站点的多个条目**共享**一份会话（键 = 站点 origin） | `AB2-14` + `VB2-16` |
| I13 | **`mode` 候选不裁剪、不静默改写**：`mode` 的可选项对**任何** `kind` 都是 `{official, web}`（**网页版与官方 API 同等优先级**，决策 `D-21`）；`kind` 只影响「切换提供商时的建议值」与「不一致时的提示」；任何条目/连线组合都不得让某个模式**不可选**，也不得在运行期覆盖用户的选择 | `AB2-15` + `VB2-18` + 人工验证 1c |
| **I14** | **站点身份不得静默替换**：网页版登录 / 生成所用的站点**只能是生效条目自己的 `web` 段**（`kind=web`）；条目不是网页版条目（`kind=official`）或表里没有该 id 时，**必须显式报错**并给可操作引导，**绝不**回落到「表内第一个 web 条目」或内置默认站点（决策 `D-22` ②） | `AB2-16` + `VB2-19` + 人工验证 `1d` |
| **I15** | **登录态判据不得依赖厂商专有物**：任何「已登录 / 未登录 / 需要重新登录」的结论只能来自**站点无关**证据（条目 `cookie_names`、该 origin 的 Cookie、页面可达性、用户确认），**不得**以 `userToken`、`ds_session_id`、`/api/v0/*` 端点成功与否作为通用判据（决策 `D-27`/`D-28`） | `AB2-20` + `VB2-24` + `VB2-26` + `grep` 审查（`ui/**`、`web/**` 不得出现 `ds_session_id`） |
| **I16** | **探测不适用的站点不得被注入 DeepSeek 协议细节**：条目未配 `probe_paths` / `token_expr` / `endpoints` 时，页面内探测**不得**请求 DeepSeek 端点、**不得**读取 `localStorage.userToken`；能力不可用时只能如实说「不适用」，**不得**伪装成「失败」 | `AB2-20`③ + `VB2-25`（内置 `deepseek-web` 与无参路径**逐字不变** → 守 `I2`） |

---

## §3 任务分解

### L1 —— JSON 配置表（**API 与网页版同表同机制** + 用户可自定义）

#### PB2-01 新增 `ai/provider_spec.{h,cpp}`：配置表加载 / 合并 / 校验（**纯函数**）

- **目标**：把「厂商元数据」从代码搬进 JSON，并提供可离线断言的加载器。
- **数据文件（本补丁已先落盘）**：`source/assets/providers.json`（见 §7 附录 B 的完整规范与示例；发布时随程序拷贝到 `<exe>/assets/providers.json`）。
- **接口（草案）**：

```cpp
namespace aiwrite::ai {
struct ProviderSpec { /* 见附录 B 字段表 */ std::string origin; };  // origin = "builtin" | "user.d/<file>" | "user" | "fallback"
enum class SpecLoadStatus { Ok, MissingBuiltin, BadJson, Partial };
struct SpecLoadReport {
    SpecLoadStatus         status = SpecLoadStatus::Ok;
    std::vector<std::string> errors;      // 可操作错误（含文件路径与原因）
    std::vector<std::string> warnings;    // 未知字段 / 覆盖条目 / 未验证条目
    std::vector<std::filesystem::path> files;   // 实际加载顺序（诊断用）
    std::string            origin_of(const std::string& id) const;
};
// 纯函数：给定「文件列表 + 各自内容」→ 合并后的表 + 报告（不碰文件系统 → 可离线断言）
std::vector<ProviderSpec> merge_provider_specs(
    const std::vector<std::pair<std::string, std::string>>& layered_json,  // 低 → 高优先级
    SpecLoadReport* report);
// 便捷：真实读盘（按 §7 附录 C 的查找顺序）后调用 merge_provider_specs
std::vector<ProviderSpec> load_provider_specs(SpecLoadReport* report);
const ProviderSpec*       find_provider_spec(const ProviderSpecs&, const std::string& id);
std::vector<std::string>  provider_ids(const ProviderSpecs&, const std::string& kind /*空=全部*/);

struct ProviderSpecs {                       // 进程内缓存（与 config 一致：显式 reload）
    std::vector<ProviderSpec> items;
    SpecLoadReport            report;
};
const ProviderSpecs& provider_specs();       // 首次访问自动 load
void                 reload_provider_specs(); // UI「重新加载配置表」/ --provider-selftest 用
}
```

- **校验规则（必须逐条实现 + 断言）**：
  1. `schema_version` 必须存在且等于支持版本（当前 `1`）；不支持 → **整体拒绝**并报错（避免误读新格式）
  2. 必填字段：`id` / `display` / `kind` / `protocol`（`models` 可空，表示「自由填写」）
  3. `id` 唯一：后者覆盖前者（记录来源链，如 `zhipu: user.d/10-zhipu.json 覆盖 builtin`）
  4. 字段类型不符 → 该条**跳过**并报错（其余条目照常可用）
  5. 未知字段 → **警告不失败**（向前兼容：未来版本新增字段时旧程序不炸）
  6. 表里出现疑似密钥（`api_key` / `auth.token` 等键名，或值形如 `sk-…`）→ **警告 + 拒绝该字段**（安全红线，见 I3）
  7. 非法 JSON（语法错误）→ 保留**上一份可用表**（进程内）+ 报错；首次加载失败 → 走**最小兜底表**
  8. **两类条目同等校验（official 与 web 共用同一套代码，仅字段子集不同）**：
     - `kind=official`：`api_base`（可空=手填）、`protocol` ∈ 本版本已实现集合（`openai`/`anthropic`/`gemini`）
     - `kind=web`：`web.login_url` 必填；再按 `web.adapter` 分支校验
       - `builtin:*`（内置适配器，如 `builtin:deepseek`）：`web.endpoints.*` 缺失 → 用内置默认值 + **警告**（保持今天行为）
       - `dom`（通用 DOM 适配器）：`input_selector` / `send` / `answer_selector` 必填，缺任一 → 该条跳过并报错
  9. **`_` 前缀键 = 纯文档字段**（`_doc` / `_user_override` / `_example_web_dom` / `_secrets_policy` …）：加载器**忽略且不产生警告**（方便在表里内嵌示例与说明）
  10. `web.adapter=dom` 的轮询常量必须有**上限**（`answer_poll_ms` 默认 500、`answer_max_polls` 默认 120；超出上限 → 警告并截断，避免用户写出「等一小时」的表）
  11. `kind` 与 `mode` 一致性：**只警告、不纠正**（决策 `D-21` / 不变量 `I13`）—— 两种组合都**允许**：official 条目 + `mode=web`（回落**内置默认站点**）、web 条目 + `mode=official`（该条目没有官方 API 通道：按表 `api_base` 解析，缺失则**明确报错**）；校验期只给提示，**绝不**改写参数、**绝不**按 `kind` 裁剪 `mode` 候选
- **最小兜底表（唯一允许的 C++ 内联数据，2 条）**：`custom-official`（空地址，必须手填）+ `deepseek`（今天的行为基线），仅当 ①② 都缺失时使用，并在 Console 明确提示「配置表缺失，已使用最小兜底」。
- **验收**：`VB2-01`（合并优先级 / 必填缺失 / 类型错误 / 未知字段警告 / dup id 覆盖 / 密钥拒绝 / 坏 JSON / schema_version 不匹配 / 兜底触发）

#### PB2-02 配置表落点与打包（"随程序发布"这条路打通）

- **路径**：`paths` 新增 `providers_asset_file()`（= `assets_dir()/providers.json`）、`user_providers_dir()`（`~/.brain-ai/providers/`）、`user_providers_file()`（`~/.brain-ai/providers.json`）、`user_provider_entries_dir()`（`~/.brain-ai/providers.d/`）；`ensure_data_dirs()` 顺带创建用户目录。
- **打包**：`CMakeLists.txt` 新增 `aiwrite_copy_assets(<target>)`（仿现有 `aiwrite_copy_runtime_dlls`，`cmake/copy_assets.cmake`），把 `assets/providers.json` 拷到 `$<TARGET_FILE_DIR>/assets/`；对 `aiwrite` 与 `api_probe` 都生效（自检也要读表）。
- **开发态兜底**：`<exe>/assets/providers.json` 不存在时，尝试 `AIWRITE_SOURCE_DIR/assets/providers.json`（该宏已用于自检，见 `CMakeLists.txt:281`），再退到最小兜底。
- **验收**：`VB2-02`（查找顺序：exe/assets → 源码目录（开发态）→ 兜底；拷贝任务在构建后确实产出文件）

#### PB2-03 用户自定义层与覆盖规则

- **用户可做的事**（三种，全部零代码）：
  1. **新增条目**：`~/.brain-ai/providers.d/<任意名>.json`（文件名升序生效）—— 单文件即可分享给他人
  2. **覆盖内置条目**：`~/.brain-ai/providers.json` 里给同 `id` 条目只写要改的字段（**字段级 merge**，未写字段沿用下层）
  3. **整体替换**：`providers.json` 里 `{"replace_all": true, "providers": […]}`（高级用法；会在 Console 明确提示「已忽略内置表」）
- **覆盖白名单**：允许覆盖 `display/api_base/chat_path/auth_style/auth_header/extra_headers/env_names/key_ref_default/models/capabilities/limits/notes/docs_url/web/*`；**禁止**覆盖 `id`（作为键）与其类型；`kind/protocol` 允许覆盖但会警告（可能导致不可用）
- **UI 呈现**：下拉里分组「内置 / 用户（N）」，条目后缀标注来源（如 `zhipu（用户覆盖）`、`my-ai（用户）`）；覆盖条目在参数面板显示「来源：user.d/10-zhipu.json」
- **对两类条目同样生效（web 同待遇）**：用户的新增/覆盖既能作用于 **API 条目**，也能作用于 **网页版站点条目** —— 例如：
  - 改站点登录页 / 窗口标题：`{"id":"deepseek-web","web":{"login_url":"…","window_title":"…"}}`
  - 改站点探测或 PoW 端点（站点换域名/换 API 版本时）：`{"id":"deepseek-web","web":{"endpoints":{"host":"…","completion_path":"…"}}}`
  - 新增一个 DOM 站点（选择器）或修正失效选择器：见 §7 附录 B 示例
  ⇒ **站点改版时用户自助修复，无需等程序更新**（这是「web 同机制」的核心收益）
- **验收**：`VB2-03`（字段级覆盖 / providers.d 多文件顺序 / 新条目可见 / replace_all / 白名单拒绝 + 报错文案 / **web 条目覆盖生效**）

#### PB2-04 API 请求参数化：端点 / 认证 / 超时全部按表取值

- **目标**：消除硬编码点 #2 #3 #6。
- **改动**：`ai/deepseek_official_provider.{h,cpp}` → 抽出 `ProviderOptions{api_base, chat_path, auth_style, auth_header, extra_headers, env_names, limits}`；`build_endpoint(base, path)`、`build_auth_headers(spec, key)`、`resolve_api_key(param_key, env_names)`（env 名**列表**按序尝试，默认仍含 `DEEPSEEK_API_KEY` 以兼容）。
- **兼容**：旧签名保留为重载（默认值 = 今天 DeepSeek 行为）→ 不变量 I1 自动成立。
- **验收**：`VB2-04`（端点拼接 5 例：默认 / 尾斜杠 / 路径前缀 / 带 query（Azure `?api-version=`）/ 空串回退；认证头 5 例：bearer / api-key / x-api-key+version / query 传参 / none；env 名列表按序命中）

#### PB2-05 节点 / UI / 校验 + **网页版去硬编码**，全部改「查表驱动」

> 本节是「**web 与 API 同机制**」在 **L1 阶段**的落地：**在不动网页版协议实现的前提下**，把站点相关的一切从 C++ 常量搬进表。

- **节点注册**（`engine/node_registry.cpp`）：
  - `provider` 参数：枚举值 **由配置表生成**（`provider_ids()`，按 `kind` 分组），默认项 = 表中第一项（内置顺序 `deepseek` 置前以保持老工作流默认观感）
  - `mode` 参数：**恒定提供 `{official, web}` 两项**（**网页版与官方 API 同等优先级**，决策 `D-21`）—— **不得**按条目 `kind` 裁剪候选；`kind` 只用于 ①「切换提供商」时带出**建议值**（web 条目建议 `web`、official 条目建议 `official`）② 两处不一致时的**提示/告警**，且**不得静默改写**用户选择
  - `api_base` 说明改为「**留空 = 用该提供商的默认地址**」；`model` 枚举 → 该条目的候选模型（首项为默认建议；`models` 为空则保持自由输入）
  - `api_key_ref` 默认值 → `key_ref_default`（换厂商不再串味）
- **生效解析**（`engine/provider_resolve.cpp`）：`EffectiveProvider` 增 `spec_id` / `kind` / `const ProviderSpec*`；空字段由表默认值补齐；「提供商」字段**真正参与解析**（不再是装饰性字段）；**`kind` 不锁定、不改写 `mode`**（`D-21`：用户选什么就按什么走；web 条目 + `official` 时按表 `api_base` 走官方通道，缺失则明确报错）
- **参数面板**（`ui/property_panel.cpp`）：
  - 显示「生效：<display> / <mode> / <模型>」+ 能力徽标（`视觉 ✅/❌ · seed ✅/❌ · 系统角色 ✅/❌`）
  - 切换提供商 → 自动带出默认地址 / Key 引用名 / 候选模型 / **`mode` 建议值**（web 条目建议 `web`、official 条目建议 `official`；**只带出一次建议，之后用户可任意改选、不再被改写**，`D-21`）（**先压快照**，可撤销）
  - **网页版会话区随条目变化**：站点名、登录按钮、登录窗口标题、探测按钮说明**全部取自该条目的 `web.*`**（不再写死 DeepSeek）
  - **配置表管理区**（本补丁新增）：显示表来源与条目数（如 `配置表：builtin + 2 个用户文件，共 12 条（其中网页版站点 2 个）`）+ 四个按钮：**「打开配置表」**、**「打开所在文件夹」**、**「重新加载配置表」**、**「打开用户目录」**
- **网页版去硬编码（逐处替换清单）**：

| 现状硬编码 | 位置 | 改为 |
|---|---|---|
| 登录 URL + 窗口标题 | `web/webview_host.h:22,62` | `web.login_url` / `web.window_title`（缺省回落到内置默认 + 警告） |
| 参数面板登录入口 URL / 标题 | `ui/property_panel.cpp:184` | 同上（按生效条目） |
| 页面内探测路径（`/api/v0/users/current`、`/api/v0/chat_session/fetch_page`、PoW challenge 路径） | `web/webview_host.cpp:150-163`（JS 模板） | `web.endpoints` + `web.probe_paths`（JS 模板参数化，**探测逻辑不变**） |
| 站点 host / completion / challenge 路径 | `ai/deepseek_web_client.cpp:15-17` | `web.endpoints.*`（PoW 算法与 SSE 解析**保持内置**：属「协议形态」而非站点数据） |
| 会话创建 / 拉取路径 | `ai/deepseek_web_client.cpp:122-152` | `web.endpoints.session_create_path` / `session_fetch_path` |

**落地情况（2026-09-26 复核；⬜ → ✅ 见「L1 收口」）**

| 逐处替换清单行 | 状态 | 证据 / 说明 |
|---|---|---|
| 登录 URL + 窗口标题（`web/webview_host.h:22,62`） | ✅ **已落地（PB2-17）** | 新增 `interactive_login_request(site,id)` / `probe_login_request` / `boot_login_request`（按条目，空字段回落内置默认）；无参重载保留 = 旧常量（`VB2-17` 断言） |
| 参数面板登录入口 URL / 标题（`ui/property_panel.cpp:184`） | ✅ **已落地（PB2-17）** | `draw_web_session_section(node, graph)` 按生效条目渲染（站点条目/来源、登录页、适配器、Cookie 名、登录/探测/注销） |
| 页面内探测路径（`web/webview_host.cpp:150-163`） | ✅ **已落地** | `probe_kickoff_script` 已按 `web.endpoints.probe_paths/challenge_path` 参数化（`webview_host.cpp:904-925`） |
| 站点 host / completion / challenge 路径（`ai/deepseek_web_client.cpp:15-17`） | ✅ **已落地** | `local_nodes.cpp` 把生效条目 `web.endpoints` 传入 `request.endpoints` |
| 会话创建 / 拉取路径（`ai/deepseek_web_client.cpp:122-152`） | ✅ **已落地** | 同上（取 `web.endpoints.session_create_path/session_fetch_path`） |
| 运行时「未登录自动开窗」的站点 | ✅ **已落地（PB2-17）** | `ensure_session(const LoginRequest&,…)` 按目标站点开窗；`local_nodes.cpp` 用 `ai::web_spec_for()` 构造请求 |
| `mode` 下拉候选（本节 `PB2-05` 计划项） | ✅ **已按 `D-21` 修订落地（v7）** | 收口时曾误按 `kind` 收窄为「official→仅 official / web→仅 web」，已判定为**回归**并**撤回**：现**恒为 `{official, web}` 两项**（`kind` 只影响建议值与提示）；同时撤回「web 条目自动锁 web」的**静默改写**（详见 §9.1 与任务 `PB2-20`） |

- **行为不变量**：内置条目的字段值与今天的常量**逐字一致** ⇒ `--web-probe` / `--web-chat` / `--web-session-selftest` 输出与结果不变（不变量 I2）
- **校验/提示**（`engine/validate.cpp`）：Key 校验的 env 名 / 引用名默认值改走表；**web 条目的「未登录 / 未探测」提示含站点显示名与登录页**；「未实现组合」提示由 `caps` + 表字段生成
- **验收**：`VB2-05`（表驱动下拉与 mode 过滤 / 默认值补齐 / 节点参数覆盖 / 连线优先 / **改表里的 `web.login_url` 后探测与登录目标随之变化（零改码）** / 提示文案含表内 env 名与站点名）

#### PB2-06 实例参数层（`config.toml` 多 provider + 旧配置迁移）

- **定位**：`config.toml` 只存**实例参数**（默认用哪个 provider、地址覆盖、默认模型、ref 名），厂商元数据仍以表为准。
- **改动**：`Config::Provider` 保留（兼容旧文件）；新增 `std::map<std::string, Provider> providers`；读写 `[providers.<id>]`；加载时**旧单节自动迁移**（`deepseek` 节原样搬家）；写盘前备份 `config.toml.bak`。
- **验收**：`VB2-06`（往返读写 / 旧配置迁移幂等 / 迁移失败不覆盖原文件）

#### PB2-07 「测试连接」+ `--provider-selftest`（表校验 + **API 与网页版两条路径**）

- **CLI**：`aiwrite.exe --provider-selftest [--provider <id>] [--api-base <url>] [--model <名>] [--key-ref <ref>] [--image <路径>] [--timeout N]`
  - **开关复用**：`--api-base / --model / --key-ref / --image / --timeout` **已存在**（`--vlm-selftest`、`--web-probe`、`--login-selftest` 在用）；本项**只新增** `--provider` 与 `--provider-selftest`
  - ① **表校验 + 离线断言（两类条目同一套）**：加载/合并/校验全流程 + API 端点 / 认证头 / 请求体（纯文本 + 多模态）+ 能力门控 → 输出 `[Provider 自检] 配置表：N 条（builtin + M 用户；其中网页版站点 W 个）；离线断言 K/K PASS`，并打印条目摘要（id / kind / protocol 或 web.adapter / 来源 / 地址或 login_url / 模型数）
  - ② **API 联网**（有 Key 时）：发一条 `ping`（提示词「请只回复 pong」）→ 打印 `HTTP / 模型 / 耗时`
  - ③ **网页版路径（`--provider <web 条目>`）**：检查「登录页可达 + 内存会话是否已有凭证 + 生效 endpoints/probe_paths 是否与表一致」→ 打印 `[Provider 自检] 网页版 <display>：登录态=有/无；endpoints=表内一致；probe 路径 N 条`；**不发送任何提示词**（避免误触发站点调用与风控）
  - 退出码：`0`=全通过 / `1`=失败（含表校验失败）/ `2`=无 Key（API）或未登录（web）—— 离线部分均已通过
- **可选加分项** `--provider-dump`：打印**生效表**（含来源与覆盖链），便于用户确认自己的 JSON 生效（低风险，建议做）
- **UI**：参数面板「测试连接」按钮（异步、不阻塞界面；结果进 Console + 状态栏）
- **验收**：`VB2-07`（无 Key 退出码 2 且离线断言全 PASS；坏表场景下报错文案可操作）

#### PB2-17 网页版登录入口去硬编码（补完 `PB2-05` 漏项）

> ⬜ **未开工**（2026-09-26 v4 复核时登记）。本节 = `PB2-05`「逐处替换清单」第 1、2 行 + `mode` 过滤的**补完**，是 `AB2-11` / `I9` / `I11` 真正落地的前提。
>
> ✅ **已完成（2026-09-26 · L1 收口）**：见 §9「L1 收口」；断言 `VB2-17`（`--exec-selftest` 4 项）。

- **现象（用户报告）**：把「模式」切到 `web`（或换「提供商」后再选网页版），点「打开登录窗口」**始终打开 DeepSeek**；即便把条目换成别的站点（自建 JSON），登录页仍是 DeepSeek。
- **复核证据**：见本文档顶部 v4 复核块 ① ② 与 `PB2-05` 节的「落地情况」表。
- **改动**：
  1. `web/webview_host.h`：`interactive_login_request()` 改为**收站点参数**（`const ai::ProviderSpec&` 或站点字段结构体：`login_url` / `window_title` / `probe_paths` / `challenge_path` / `completion_path` / `token_expr` / `cookie_names`）；条目缺 `web` 段或字段为空 → **回落内置默认（DeepSeek 值逐字一致）+ 警告**；旧无参重载保留（默认值 = 今日行为）以保 `I2`。
  2. `ui/property_panel.cpp`：`draw_web_session_section(const engine::Node&, const engine::Graph&)` —— 内部经 `engine::resolve_effective_provider(graph, node)` 取生效条目；站点名 / 登录页 / 登录窗口标题 / 探测按钮说明 / 状态区（含 `cookie_names` 取值，取代写死的 `ds_session_id`）/ 注销范围**全部按条目**；**非网页版条目或不含 `web` 段时**给可操作提示（「当前提供商没有网页版站点条目：请选 `deepseek-web`，或把站点 JSON 放进 `~/.brain-ai/providers.d/`」）。
  3. `web/webview_host.cpp`：`ensure_session()` 的 `LoginRequest` **由调用方传入站点参数**（`nodes/local_nodes.cpp:415-430` 已持有 `effective.spec`，顺带传入）。
  4. `engine/node_registry.cpp:319`：`mode` 下拉**按条目 `kind` 生成**（official → 仅 `official`；web → 仅 `web`），保留 `provider_resolve.cpp:124-126` 的「web 条目锁 `mode=web`」。
  5. `engine/validate.cpp` + `engine/provider_resolve.cpp`：对「**official 条目 + `mode=web`**」给出明确警告（文案含「当前会使用**内置默认站点（DeepSeek）**」+ 两条建议：改选 web 条目 / 改回 `official`），杜绝「以为在登录 A、其实在登录 DeepSeek」。
  6. `tools/api_probe.cpp:314-316`：断言改为**表驱动双向断言**（deepseek-web 条目 = 与旧常量逐字一致；自定义站点条目 = 按表取值）—— 见 `VB2-17` / `R15`。
- **不变量**：`I11`（站点身份唯一来源 = 生效条目）。**兼容**：内置条目行为与今天逐字一致（`I2`）。
- **验收**：`AB2-13`；**验证**：`VB2-17`。

#### PB2-18 多站点会话并存（按站点键控）

> ⬜ **未开工**（2026-09-26 v4 复核时登记）。**用户目标**：「结合两个 AI 的输出」时，两个网页版节点要**各用各的登录态**（互不覆盖）。
>
> ✅ **已完成（2026-09-26 · L1 收口）**：`SessionStore` 按站点 origin 键控；断言 `VB2-16`（`--exec-selftest` 6 项）。

- **现状（为什么今天做不到）**：`web/session_store.h:64-82` 只保存**一个** `Session`（一组 Cookie + 一个 `userToken` + 一次探测结果），`set()` 覆盖写 → 第二个站点登录会**冲掉**第一个；`nodes/local_nodes.cpp:415-422` 取的也是「那一个」会话；`ensure_session()` 只判断 `userToken` 非空，**与站点无关**。
- **改动**：
  1. `web/session_store.{h,cpp}`：内部改 `std::map<site_key, Session>`（`site_key` = **站点 origin**，见 `D-19`）；`Session` 增 `site` / `provider_id` 字段；新增 `set(key, session)` / `snapshot(key)` / `clear(key)` / `keys()` / `sites()`；**旧无键 API 保留为「当前站点」薄封装**（保证 `--web-probe` / `--web-chat` / `--web-session-selftest` 结果不变 → `I2`）。
  2. `web/webview_host.cpp`：写会话 / 探测结果**按站点归档**（键取当前窗口 URL 的 origin）；`ensure_session(站点参数, …)` 按站点判 `has_token`，缺失时**为该站点**起窗口。
  3. `nodes/local_nodes.cpp`：取会话改为「按生效条目的站点 key」→ **每个 `ProviderConfig` 节点的网页版调用各用各的凭证**。
  4. `ui/property_panel.cpp`：会话区增加「**已登录站点**」列表（多份 Cookie 并存的可视化）+ 每站点「打开 / 关闭 / 注销」。
- **不做（明确登记）**：凭证落盘 / 跨进程持久化 / 自动刷新会话 / 绕过站点验证（合规护栏不变，见 §0.3）。
- **不变量**：`I12`。**验收**：`AB2-14`；**验证**：`VB2-16`。

#### PB2-19 窗口策略与按站点注销

> ⬜ **未开工**（2026-09-26 v4 复核时登记）。
>
> ✅ **已完成（2026-09-26 · L1 收口）**：`ensure_session` 串行复用 + `logout_site`（按 origin 删 Cookie / 清 localStorage）+ 面板「已登录站点」列表与「高级：删除整个 profile（二次确认）」。

- **窗口策略**：先做**串行复用单窗口** —— 记住当前窗口所属站点；需要另一站点时 `request_close()` + 等线程 `join()` 后再以新站点 `login_url` 重开（多花数秒，但无需改动 `webview_host.cpp` 的全局状态）。理由：执行链是**单 worker 线程顺序执行**（`engine/executor.cpp:402`），串行足够支撑「多个网页版节点各自登录一次 → 运行时各用各的凭证」（决策 `D-20`）。
- **后置项（登记，不在本批）**：N 个站点窗口**并发** —— 需把 `g_request` / `g_window` / `g_webview` / `g_controller` / `g_probe_*` 等进程级全局**实例化**（`web/webview_host.cpp:196-200`、`:439`、`:946-958`），风险与工作量显著更大（隔离边界见 `R14`）。
- **注销语义**：`ui/property_panel.cpp:88-102` 现为 `remove_all(~/.brain-ai/webview2)`（**清掉所有站点**）→ 改为**按站点**：清该 origin 的内存会话 + 用 `ICoreWebView2CookieManager` 按 origin 清 Cookie（可选清该 origin 的 localStorage）；「删除整个 profile」保留为**带二次确认的高级操作**。
- **验收**：并入 `AB2-14`；**验证**：并入 `VB2-16`。

---

#### PB2-20 「`mode` 恒两项」修复 + 生效条目按**节点自身参数**解析（补完 `PB2-05`/`PB2-17` 声明未生效项）—— ✅ **已完成（v7 · 2026-09-26）**

> 触发：2026-09-26 用户实测报告「选中「提供商配置」后，参数面板的「模式」下拉**没有 `web` 选项**」。判定为**回归**（见 §9.1），按决策 `D-21` 修复。

- **语义（`D-21`）**：`mode` 的 `official` / `web` **始终可选**（网页版与官方 API 同等优先级）；`kind` 只影响建议值与提示。
  - `engine/provider_resolve.{h,cpp}`：`provider_mode_options()` 改为**恒返回 `{official, web}`**（保留单点开关，便于将来若产品需要再收窄）；`resolve_effective_provider()` 删除 `if (spec.kind == "web") result.mode = "web";` 的**静默改写**；
  - `ui/property_panel.cpp`：不再对 `mode` 传 `enum_override`；「web 条目自动锁 web」改为「切换提供商时带出建议值」；不一致时给**橙色提示**（不是告警）；
  - `engine/node_registry.cpp`：「提供商配置」的 `mode` 说明文案去掉「自动锁定为 web」，改为「两项都可选；网页版条目建议 web，官方条目建议 official」。
- **生效条目解析（补完 `PB2-17` 的面板/校验两半）**：新增「自参数解析」（只读**该节点自身参数** + 配置表，等价于「把 ProviderConfig 当作自己的 provider 来源」，**不设** `from_edge`），供三处使用：
  - `ui/property_panel.cpp:105`（`web_site_context()`：站点条目 / 登录页 / Cookie 名 / 适配器 / 站点键必须取自**该节点选中的条目**，选 Kimi 不能显示 DeepSeek）；
  - `ui/property_panel.cpp:724`（`provider_effective`：用于「站点条目/来源」显示）；
  - `engine/validate.cpp:138`（运行前提示按**该节点自己的条目**生成）。
  - 根因：`resolve_effective_provider()` 对非 `LLMGenerate`/`VLMGenerate` 节点（`uses_provider()`，`provider_resolve.cpp:72-75`）**直接返回默认构造**（`spec=nullptr` / `kind=""` / `provider="deepseek"`），故上述三处在「提供商配置」上恒等于「回落内置默认站点」。
- **状态**：✅ **已完成（v7）** —— 验收 `AB2-15`（GUI 人工点击项待用户确认；其代码路径已由 `VB2-18②③` 断言覆盖）、验证 `VB2-18`（**4/4 PASS**）；人工验证清单见 `docs/节点编辑器使用说明.md` §8 第 1c 项。

- **落地实测（v7 · 2026-09-26 · 代码批次）**

| # | 文件 | 实际改动 | 位置 |
|---|---|---|---|
| 1 | `engine/provider_resolve.{h,cpp}` | `provider_mode_options()` **恒** `{official, web}`（入参保留 = 单点开关）；**删除** `if (spec.kind == "web") result.mode = "web";` 的静默改写；抽出 `apply_spec_table()`；新增 `resolve_self_provider()` / `resolve_display_provider()` / `provider_mode_suggestion()` / `mode_kind_hint()` | `provider_resolve.cpp:52-78 / 134-160 / 169-207 / 241-245` |
| 2 | `ui/property_panel.cpp` | 去掉 `mode` 的 `enum_override`；`web_site_context()` 与「生效条目」改用 `resolve_display_provider()`；**删**「web 条目自动锁 web」→ 改为**切换提供商带出建议值**（只带一次）；不一致给**橙色提示**（`mode_kind_hint`） | `:105` / `:721-728` / `:760-785` / `:823` |
| 3 | `engine/validate.cpp` | 运行前提示改按**该节点自己的条目**解析；网页版条目 + `official` → **明确提示**（不再误导性地只提示缺 Key） | `:136-167` |
| 4 | `engine/node_registry.cpp` | 「模式」说明改为「两项都可选（网页版与官方 API 同等优先级）；切换提供商带出建议值；不一致只提示」 | `:319-322` |
| 5 | `tools/api_probe.cpp` | 「模式过滤 1 项」→ **`VB2-18` 4 项**（候选恒两项 / 自参数解析 / 不改写 `mode` / 表外 id） | `:2315-2398` |
| 6 | 文档 | 本节 + §4.1 / §9 基线 + `CHANGELOG.md` / `节点编辑器使用说明.md` / `网页版协议实测记录.md` / `docs/README.md` / `source/README.md` / `DevPlan.todo` 同步回填 | — |

- **实测（2026-09-26 · 全绿）**：`--exec-selftest` **204 / 0**（190 → +11（`VB2-17` 4 + `VB2-16` 6 + 模式过滤 1）→ −1（撤回「模式过滤」）+4（`VB2-18`））；`--graph-selftest` **111 / 0**；`--selftest` **七组 PASS**；`--provider-selftest` **50 / 0**；`--run-selftest` **PASS**（离线 3/5，预期）；`--run-selftest --web` **5/5（0 失败 0 跳过，8.76s）**；`--web-session-selftest` / `--web-probe` / `--web-chat` **PASS**；构建 **0 error / 0 warning**（4 目标）

#### PB2-21 用户自建网页版站点条目：**正确格式**与模板对齐（本轮 = 文档；模板 / UI 改造后置）

> 触发（v8 实测）：`~/.brain-ai/providers.d/*.json` **必须**带信封 `schema_version`（整数 1）+ `providers:[…]`，否则**整层被忽略**；`web` 段是**嵌套**写法（`send{kind,value}` / `done_when{kind,selector}`），写扁平名（`send_kind`…）会被判「未知字段」并因 `缺少 send` **跳过该条**；内置模板 `_example_web_dom` 是**顶层 `_` 键**（不加载）、**不能整份复制**当用户文件；错误只写 `app.log`，**界面不可见**（三次实测原始输出见**附录 D**）。

- **本轮（文档）**：附录 D + `docs/节点编辑器使用说明.md` + `docs/网页版协议实测记录.md` §7.5 给出**实测通过**的完整 JSON 与三步流程（写文件 → 重启 → 在「提供商」里选）。
- **后置（本轮不做，见 `PB2-24`）**：把 `assets/providers.json` 的 `_example_web_dom` 改成**可直接复制的整份样例**（含信封）/ 新增 `--provider-template <id>`；`provider_spec.h` 头注释改用**嵌套** schema 名（现状扁平名与实现不一致 → `R18`）。
- **验收 / 验证**：`AB2-16`（附录 D 的 JSON 逐字可用）、`VB2-19`。

#### PB2-22 「非网页版条目 + `mode=web`」**不再回落**：明确报错 + 三条引导（决策 `D-22` ②）—— ✅ **已完成（v9 · 2026-09-26）**

> **落地实测（v9）**：`ai::strict_web_spec_for()` / `web_site_error()` / `web_site_field_warnings()` / `web_adapter_implemented()` 已落地；面板（错误块 + 一键改选 + 两按钮禁用）、运行前校验（「本次运行必定失败」+ 三条引导，**不阻断**）、运行期（`NodeError` + 适配器门控）三处一致；`--exec-selftest` **209 / 0**（`VB2-19①…⑤` 全 PASS）；`--web-probe --provider zhipu` → **exit 2** 且打印「不是网页版条目…三条引导」（**不开窗**）；`--run-selftest --web` 因**官网侧会话失效**暂不能复测 5/5（§9.4，非本批引入）。

- **现状（v8 根因）**：`ai::web_spec_for()` / `web_provider_id_for()` 对**任何非 `kind=web` 条目**返回**表内第一个 web 条目**（= `deepseek-web`）→「选智谱 / OpenAI + 模式 web」时，面板站点区、登录按钮、运行前提示、运行时**全部指向 DeepSeek**（`ai/provider_spec.cpp:269-299`；调用点 `ui/property_panel.cpp:108-111` / `:239`、`engine/validate.cpp:144`、`nodes/local_nodes.cpp:418-420`）。
- **目标**：非网页版条目（或表外 id）→ **没有站点可用**：三处**统一明确报错** + 三条可操作引导（① 改选网页版条目（如 `deepseek-web`）② 新建网页版站点条目（附录 D）③ 把「模式」改回 `official`），**运行期直接失败**（`NodeError`），**绝不**打开或使用 DeepSeek。
- **改动清单（待审）**：
  1. `ai/provider_spec.{h,cpp}`：新增**严格**解析 `strict_web_spec_for(spec)`（或给 `web_spec_for` 增 `allow_fallback` 参数，**默认 false**）→ 非 web 条目**返回空**；`web_provider_id_for` 同步；旧回落行为**仅供 CLI / 自检**（守 `I2`）。
  2. `ui/property_panel.cpp`：站点区不再显示「将使用内置默认站点」，改为**错误块**（含三条引导）；**「打开登录窗口」/「探测网页版协议」按钮禁用**（绝不打开 DeepSeek）。
  3. `engine/validate.cpp`：`mode=web` + 非网页版条目 → 文案升级为「**本次运行必然失败**」+ 三条引导（是否**阻断运行**见 §6 `D-25`）。
  4. `nodes/local_nodes.cpp`：网页版分支取不到站点 → `NodeError`（含三条引导），不再回落。
  5. 自检与断言同步：`source/src/main.cpp` 的 `--run-selftest --web` 把「提供商配置」的 `provider` 设为 **`deepseek-web`**（现依赖回落，改后会失败）；`tools/api_probe.cpp` 的 web 模式用例同步。
- **迁移影响（`R17`）**：老工作流若为 `provider=deepseek` + `mode=web`（今天靠回落偷偷工作），升级后**会明确报错**；引导文案提供**一键把「提供商」改为 `deepseek-web`**（复用面板既有「把 X 改为 web」的交互模式）。
- **验收 / 验证**：`AB2-16`、`VB2-19`。

#### PB2-23 `solve_pow_via_page()` 站点参数化 + 「按条目」网页版自检入口 —— ✅ **已完成（v9 · 2026-09-26）**

> **落地实测（v9）**：`solve_pow_via_page(site_url, challenge_json, …)` 改为**按站点** `ensure_session`（不再用无参 → 内置默认站点）；新增 `web::protocol_probe_for_provider()` + CLI `--web-probe --provider <id>`（站点不可用 → 打印原因 + 退出码 **2**，**不开窗**）；`--web-probe --provider deepseek-web` 与旧 `--web-probe` 结果一致（守 `I2`）。

- **现状**：`web/webview_host.cpp:1409` 内部调**无参** `ensure_session(15000,…)` → 窗口未开 / 无凭证时按**内置默认站点**开窗（残留硬编码）；CLI / 自检（`main.cpp:756` 的 `--web-session-selftest`、`--web-probe`、`--web-chat`）走无参重载 = DeepSeek（守 `I2`，但易被误读为「写死了」）。
- **改动**：`solve_pow_via_page(const LoginRequest&, …)`（由调用方传生效条目站点）；新增**按条目**的自检入口（如 `--web-probe --provider <id>`），既有 DeepSeek 自检**保持不变**。
- **验收 / 验证**：`VB2-19` 末项 + 现有 web 自检全绿（守 `I2`）。

#### PB2-24 （**后置 · 本轮不做**，`D-23` A）自建站点闭环：新建站点条目 UI + 重新加载 + 下拉即时刷新 + 配置表错误可见

- 现状证据：`reload_provider_specs()` **全工程零调用点**；`src/ui` 无任何「配置表 / 重新加载 / 测试连接」入口（`PB2-07` 界面按钮仍 ⬜）；`registerAllNodes()` 幂等（`node_registry.cpp:225-230`）→ 「提供商」枚举在首次注册时**一次性**生成，运行中 reload 也**不刷新下拉**；配置表错误 / 警告只在 `app.log`（界面不可见）。
- 改动方向：①「新建网页版站点…」按钮（写入附录 D 模板 + 立即可用）② `reload_provider_specs()` 接线 + `provider` 参数改**动态枚举**（或提供重建接口）③ 面板 / Console 显示配置表**错误 / 警告**（含「已忽略该层 / 已跳过该条」）。
- **不进本轮**（用户指示 `D-23` A）；`PB2-07` 与 L3（`PB2-13…16`）仍按原批次。

#### PB2-25 （**v9 实测登记 · 未实施**）网页版会话失效的**可诊断性**

> 触发（v9 实测，2026-09-26 17:16）：`localStorage.userToken` 的值形状由「64 字符真 token」变为「30 字符 JSON 包裹值」（15:53 → 17:16 之间），服务端对**所有**端点返回 `{"code":40003,"msg":"Authorization Failed (invalid token)"}` → 网页版生成失败；**未改动的旧路径（`--web-probe` / `--web-session-selftest`）同样复现** → 与本批代码**无关**（证据见 §9.4）。

- **问题 1（判据偏松）**：`--web-probe` 的 PASS 判据只看 `probe.ok && has_user_token` → 端点 40003 时**仍打印 PASS（退出码 0）**。**改**：判据加「端点探测全部 `code=0`（至少 `/api/v0/users/current`）」。
- **问题 2（失效不可诊断）**：会话失效目前只在**运行期**报「挑战解析失败…invalid token」。**改**：把「端点 40003 / 无有效 userToken」识别为**会话失效** → 面板站点区 + 运行前校验给「**会话已失效：请重新登录**」+ 一键打开登录窗口（与 `PB-07` 合并）。
- **问题 3（token 取值形状）**：若是官网**形状变化**，属表驱动可覆盖项（`web.token_expr`）→ 需要① 诊断输出（受 `R20` 约束：只给前 4 + 长度 / 「是否 JSON 包裹」等不可复用特征）② 或把取值写成「解包 JSON / 容错」表达式。
- **验收 / 验证（建议 `AB2-17` / `VB2-20`）**：会话失效时 `--web-probe` 退出码 **1**（不再 PASS）、面板给「会话失效」+ 重登入口、运行前提示明确；重新登录后 `--run-selftest --web` **5/5**。

#### PB2-26 内置各 AI 网页版**登录入口**（登录型站点条目）—— ✅ **已完成（v10 · 2026-09-26）**

> 背景（用户指示 2026-09-26）：为每个 AI 的网页版配置**正确的网页入口**。**约束（已讨论并采纳方案 A）**：选择器 / 凭证取值**必须实测**才能写对（各站有 WAF / 反爬，且本机无浏览器交互能力）→ 采用**登录型条目**：只配「登录页 + 窗口标题」，**不填假选择器**。

- **改动**：
  1. `ai/provider_spec.{h,cpp}`：`adapter=dom` 缺生成字段时由 **error（静默跳过该条）→ warning（可加载）**，文案「**登录型站点条目**：web 缺 …（生成未就绪 —— 登录 / 协议探测可用）」；新增 `web_login_only()`（`adapter=dom` 且生成字段未就绪）；`web_site_field_warnings()` 文案细分（缺 `token_expr` / `cookie_names` → 「**该站点无法自动探测凭证**（登录仍可用）」，取代误导性的「已回落内置默认值」）
  2. `nodes/local_nodes.cpp`：运行期报错补充「该条为**登录型条目**（缺生成字段）」
  3. `source/src/main.cpp`：`--provider-selftest --provider <web 条目>` 对 DOM 条目如实显示「端点：**不适用**（DOM 适配器；无内置端点）」+「生成：未就绪（登录型…）」—— 此前会把**内置默认端点**显示成该条目的端点（误导）
  4. `assets/providers.json`：**内置 11 个站点入口**（清单与核实状态见**附录 E**）
  5. `tools/api_probe.cpp`：新增 **`VB2-21` 5 项**；`provider_spec_selftest` 中「dom 缺选择器 → error 且条目被丢弃」的旧期望改写为「→ **登录型条目**（警告，不丢弃）」
- **实测（2026-09-26）**：构建 **0 error / 0 warning**；`--provider-dump` **21 条（official 9 / web 12）**（每条登录型条目一行警告）；`--exec-selftest` **209 → 214 / 0**（`VB2-21①…⑤` 全 PASS）；`--graph-selftest` **111 / 0**；`--selftest` 七组 PASS；`--provider-selftest` **50 / 0**；`--provider-selftest --provider kimi-web` → `网页版 Kimi（Moonshot 网页版）：适配器=dom` / `登录页：https://kimi.moonshot.cn/` / `端点：不适用（DOM 适配器；无内置端点）` / `生成：未就绪（登录型站点条目）`，退出码 **2**（未登录）；`--provider-selftest --provider deepseek-web` → 端点仍按条目显示（守 `I2`）；GUI 冒烟：启动日志「条目 21 条」→ 正常退出。
- **不做（本轮）**：DOM 执行器（L3 `PB2-13…16`）与**选择器 / 凭证取值实测** → 生成仍会在运行时**明确报错**（不静默、不回落）；`PB2-25`（会话失效可诊断）与本条无关亦未实施。
- **验收 / 验证**：`AB2-18`、`VB2-21`。

### L2 —— 协议 / 适配器实现（工厂按表分派；落地设计 §8.1 / PB-04）

#### PB2-08 新增 `ai/inference_provider.h`（接口 + 值类型）

- **设计对齐**：命名与职责取自 `ai_writer_nodes.md` §8.1（`name` / `supportsVision` / `generate` / `generate_stream` / `generateWithImage`），并按现状（多图、seed、快照线程）做**最小必要扩展**：
  - `struct ProviderCaps { bool vision; bool stream; bool seed; bool system_role; }`（来自表的 `capabilities` —— 取代节点里的 `if (mode == "web") throw`）
  - `struct GenerateParams`（system_prompt / prompt / images / temperature / max_tokens / top_p / seed / on_delta）
  - `struct GenerateResult`（ok / http_status / text / error / raw_head / elapsed_ms）
  - `class InferenceProvider { name(); caps(); generate(params, options); }`
  - **取舍**：不新开 `generateStream`/`generateWithImage` 两个虚函数，保留 `on_delta` + `images`（与 PB-03「流式暂停」的现状一致，避免虚函数空转）—— 记 `D-09`，§6 待确认
- **验收**：编译期 + `VB2-08`（能力表来源正确：web 条目 `vision=false`；official 条目按表；节点报错文案由能力表生成而非硬编码）

#### PB2-09 新增 `ai/provider_factory.{h,cpp}` + 现有实现收编

- `make_provider(const ProviderSpec&, const std::string& mode, const ProviderOptions&) -> std::unique_ptr<InferenceProvider>`
  - 分派依据 = 表里的 `protocol` + `kind`（**不是** C++ 里的 `if (mode == "web")`）
- `OpenAICompatibleProvider`：搬迁现有 `ai::official_chat()` 逻辑（**行为不变**：错误分类、`stream=false`、超时默认值）
- `DeepSeekWebProvider`：搬迁现有 `ai::web_chat()` 逻辑（**行为不变**：`on_delta`、PoW、SSE）
- 未知 `protocol` → 返回空 + 可操作错误（「配置表里的 protocol=xyz 尚未实现，请改用 openai/anthropic/gemini/dom，或更新程序」）
- **验收**：`VB2-09`（工厂按 protocol 正确分派 / 未知 protocol 报错 / 旧 `official_chat()`/`web_chat()` 重载仍可用）；`--web-chat`、`--vlm-selftest`、`--exec-selftest` 全绿

#### PB2-10 `AnthropicProvider`（Messages API）

- 端点 `{api_base}{chat_path}`（默认 `/v1/messages`）；认证 `x-api-key`；`extra_headers` 带 `anthropic-version: 2023-06-01`；`max_tokens` 必填；`system` 为**顶层字段**而非消息角色；多模态 `content[] = [{type:"text"},{type:"image", source:{type:"base64", media_type, data}}]`；响应取 `content[0].text`
- **验收**：`VB2-10`（请求体结构 / 认证头 / 响应解析 / 错误映射）

#### PB2-11 `GeminiProvider`（generateContent）

- 端点 `{api_base}/v1beta/models/{model}:generateContent`（Key 走 `extra_headers` 的 `x-goog-api-key` 或 `auth_style=query`，二选一并在表里声明）；`contents[].parts[]`；`inline_data{mime_type,data}`；参数走 `generationConfig{temperature,topP,maxOutputTokens}`；响应取 `candidates[0].content.parts[0].text`
- **验收**：`VB2-11`（model 名进 URL 的转义 / `generationConfig` 结构 / 解析 / 认证两种风格）

#### PB2-12 节点接线改能力驱动 + 校验/提示同步

- `nodes/local_nodes.cpp`：`if (mode != "web") {...} else {...}` → `make_provider(spec, mode, options)->generate(...)`
- 删除硬编码 `if (mode == "web") throw 图片理解暂不支持网页版`，改为 **caps 检查（表驱动，web 与 API 同一判据）**：
  - 生效模型在表内且 `vision=false` → 明确报错：「<display> 的模型 <model> 不支持图片理解（表内视觉模型：<vision_model_default>）」
  - 生效条目 `kind=web`（`caps.vision=false`）→ 明确报错：「<display>（网页版）不支持图片理解：请把「提供商配置」改为视觉 API 条目（如 zhipu / siliconflow）」
  - 生效模型**不在表内**（用户手填的自定义模型）→ 允许调用 + Console 提示「表内未声明该模型的视觉能力，若失败请改用 …」
- `engine/provider_resolve.cpp` / `engine/validate.cpp`：缺 Key 文案按表生成（env 名列表 / ref 名 / 默认地址）
- **验收**：`VB2-12`（视觉门控 3 例：表内非视觉模型 / 表内视觉模型 / 表外自定义；缺 Key 文案含表内 env 名）

---

### L3 —— 通用网页版适配器（DOM 驱动）+ 用户自定义站点

> 前置事实：`web/webview_host.cpp` 已支持**在页面内执行任意 JS**（PoW 求解与协议探测就是这么做的，见 `webview_host.cpp:150-163`），因此「DOM 驱动型适配器」**不需要新的技术栈**。
> 范围说明：**站点表、用户覆盖、UI 管理、自检入口已在 L1/L2 内实现**；L3 只补「DOM 执行器」与「选择器探测/诊断」——即把「任意站点」真正降级为纯数据。

#### PB2-13 站点条目（`assets/providers.json` 内 `kind=web`）两种形态：`builtin:*` 与 `dom` —— ✅ **已完成（v11 · 2026-09-26）**

> **落地实测（v11）**：新增 `ai/dom_web_client.{h,cpp}`（DOM 执行器：注入提示词 → 触发发送 → 轮询答案）+ `web::run_script_sync()`（窗口内同步执行脚本；三级前置：窗口已在该站点 → `ensure_session` → **兜底离屏开窗 + 等 `readyState=complete`**，因此**不要求内存 `userToken`**）；`provider_spec.cpp` 的 `implemented_web_adapters()` 增 `dom`、`implemented_protocols()` 增 `dom`；`nodes/local_nodes.cpp` 增 DOM 分支（登录型条目 → 明确报错并给诊断入口；`dom` → `ai::dom_chat()`）。`--provider-dump` 现已显示「已实现协议：openai、deepseek-web、**dom**」「已实现网页版适配器：builtin:deepseek、**dom**」。

- **与 API 条目同一份 JSON、同一套加载/覆盖/校验**（这是「web 同机制」的数据基础）。`kind=web` 条目的 `web` 对象按 `adapter` 分两种形态：

**形态一：内置适配器站点（`web.adapter: "builtin:deepseek"`）** —— 站点参数化，协议逻辑保持内置（L1 已交付）

```json
{
  "id": "deepseek-web",
  "display": "DeepSeek（网页版）",
  "kind": "web",
  "protocol": "deepseek-web",
  "capabilities": { "vision": false, "seed": false, "system_role": false, "stream": true },
  "models": [ { "id": "default" }, { "id": "expert", "label": "专家模式" } ],
  "web": {
    "adapter": "builtin:deepseek",
    "login_url": "https://chat.deepseek.com/",
    "window_title": "AIwrite · DeepSeek 网页版登录（登录后关闭窗口）",
    "endpoints": {
      "host": "https://chat.deepseek.com",
      "completion_path": "/api/v0/chat/completion",
      "challenge_path": "/api/v0/chat/create_pow_challenge",
      "session_create_path": "/api/v0/chat_session/create",
      "session_fetch_path": "/api/v0/chat_session/fetch_page"
    },
    "probe_paths": ["/api/v0/users/current", "/api/v0/chat_session/fetch_page"],
    "cookie_names": ["ds_session_id"],
    "token_expr": "localStorage.getItem('userToken')"
  },
  "verified": true,
  "notes": "内置适配器（有头登录 + PoW + SSE）。站点换域名/换 API 版本时，只需在用户表里覆盖 web.endpoints，无需改程序"
}
```

**形态二：通用 DOM 站点（`web.adapter: "dom"`）** —— 纯选择器驱动（L3 交付）

```json
{
  "id": "kimi",
  "display": "Kimi（月之暗面）· 网页版",
  "kind": "web",
  "protocol": "dom",
  "capabilities": { "vision": false, "seed": false, "system_role": false, "stream": true },
  "web": {
    "adapter": "dom",
    "login_url": "https://kimi.moonshot.cn/",
    "window_title": "AIwrite · Kimi 网页版登录",
    "input_selector": "[contenteditable='true']",
    "send": { "kind": "key", "value": "Enter" },
    "answer_selector": ".markdown-body",
    "done_when": { "kind": "selector_gone", "selector": "button[aria-label*='停止']" },
    "cookie_names": ["kimi-auth"],
    "token_expr": "localStorage.getItem('token')",
    "answer_poll_ms": 500,
    "answer_max_polls": 120
  },
  "verified": false,
  "notes": "示例模板：选择器需按站点实际 DOM 调整（免手写 C++）"
}
```

- `WebDomAdapter`（实现 `InferenceProvider`，`caps.vision=false`）：
  1. `LoginWindow`（已有）打开 `login_url`，用户手动登录 → 提取 `cookie_names` / 求值 `token_expr`
  2. 注入 JS：写入 prompt → 触发 `send` → 按 `answer_poll_ms` 轮询 `answer_selector` 的 `innerText`
  3. 增量：用 `on_delta` 发增量（沿用 PB-03 的 `RunEvent::Delta`）
  4. 结束：`done_when` 命中或轮询用尽（**用尽时如实返回已取文本 + 明确警告**，不假装成功）

#### PB2-14 用户自定义站点（**与 API 完全同一套机制**）—— ✅ **已完成（v11 · 2026-09-26）**

> **落地实测（v11）**：用户把 `~/.brain-ai/providers.d/<id>.json`（信封 + `adapter=dom` + 选择器）放进去并**重启**即可被 DOM 执行器直接驱动 —— **无需改 C++**；缺少选择器时按**登录型条目**处理（可登录 / 可探测；生成前明确报错并指向 `--web-adapter-selftest` 诊断）。真实样例与格式见**附录 D**，内置清单见**附录 E**。

- 用户可做（零代码）：
  1. **新增站点**：`~/.brain-ai/providers.d/kimi.json`（DOM 站点，见 PB2-13 形态二）
  2. **修站点（改版应急）**：`providers.json` 覆盖 `web.login_url` / `web.window_title` / `web.endpoints.*` / `web.probe_paths` / 选择器 —— **站点改版时用户自助修复，无需等程序更新**
  3. **改站点模型/能力**：覆盖 `models`（如网页版模式名变化时）与 `capabilities`
- 与 API 条目**共用**：同一份用户目录、同一套字段级 merge、同一套白名单校验、同一个「重新加载配置表」按钮、同一个 `--provider-selftest`
- 安全/合规护栏（沿用既有策略）：**有头登录、用户手动操作、不代填密码、不自动刷新会话**；不注入脚本绕过验证（验证码/风控不碰）
- **验收**：`VB2-12`（web 条目覆盖生效 / 新增 DOM 站点可见 / 非法选择器字段报错）

#### PB2-15 站点改版诊断 + `--web-adapter-selftest` —— ✅ **已完成（v11 · 2026-09-26）**

> **落地实测（v11）**：`aiwrite.exe --web-adapter-selftest`（无 `--provider` → 列出表里全部 web 条目及其适配器/是否登录型，exit 2）；`--web-adapter-selftest --provider <id>` → **只读**探测：打印当前页面 `URL`/`标题`、`input_selector` 命中数与可见性、`send` 命中数、`answer_selector` 命中数与末条字数、`done_when` 现状、`token_expr` 取值形状（长度 / 是否 **JSON 包裹值** / 求值失败）、Cookie 可读数，并给**可操作修复建议**（F12 → Copy selector / 改 send 方式 / 修 `token_expr` / 补 `done_when`）。**实测输出**（`--provider kimi-web --timeout 20`，2026-09-26）：`URL=https://www.kimi.com/`、`标题=Kimi AI with K3 …`、4 条建议、exit **1**（选择器未填）—— 证明「窗口 + 页面脚本」链路真实可用。

- `aiwrite.exe --web-adapter-selftest [--site <id>] [--timeout N]`：离屏打开站点 → 逐条检查「输入框 / 发送方式 / 答案选择器 / 登录态」是否可达（**只检查、不发送内容**）
- 退出码 0=全部可达 / 1=有未命中 / 2=窗口或站点超时；未命中时打印**选择器 + 命中的候选元素摘要**（便于用户改 JSON）
- 失败路径：错误文案给出「选择器未命中，站点可能已改版；请更新 `~/.brain-ai/providers.d/<id>.json`」，并把 DOM 片段（脱敏、限长）写诊断日志（`utils/diagnostics.*` 已有落点）

#### PB2-16 文档与索引同步 —— ✅ **已完成（v11 · 2026-09-26）**

> **落地实测（v11）**：`M_patchB`（本批）+ `使用说明`（§9「怎么写/怎么调选择器」+ §8 `1g`）+ `网页版协议实测记录` §7.7 + `CHANGELOG` + `docs/README` + `source/README` + `DevPlan.todo` 已同步。

- `CHANGELOG`（新条目 + 索引）、`docs/节点编辑器使用说明.md`（新增「接入一个新 AI：3 步（写 JSON → 重载 → 测试连接）」）、`docs/README.md`、`actionPlan/M_patchA.md` §4.1 `PB-04` 状态 → ✅ 并指向本文档、`milestone_plan.md`、`DevPlan.todo`（登记/翻转条目）
- **`source/README.md`**：新增「配置文件位置」小节（`assets/providers.json` / `~/.brain-ai/providers.json` / `providers.d/`）

---

### L4 —— 登录/会话层去厂商化 + 站点数据落地 + 自助闭环

> 前置事实（为什么还要一层）：**生成引擎已在 L3 交付** —— `ai::dom_chat()` 对「未取到内存凭证」**只给警告、不阻断**（`ai/dom_web_client.cpp:194-202`），`web::run_script_sync()` 三级前置**不要求内存 `userToken`**（`web/webview_host.cpp:1519-1557`）。即「任意站点生成」缺的**不是引擎**，而是：① **登录态判据与文案**仍绑 DeepSeek（`has_token()` / `ds_session_id` / `userToken`）② **探测脚本**对通用站点注入 DeepSeek 端点 → 假「探测错误」③ **11 条内置站点没有选择器**（登录型条目）+ **界面没有填选择器的入口**。
> 用户语义里的「能不能登录、能不能用」在 L1–L3 都不成立，**L4 才把它做成真**。

#### PB2-27 「已登录」判定与界面文案去 DeepSeek 化（站点无关）—— ✅ **已完成（v13 · 2026-09-27）**

- **现状证据**：`web/session_store.cpp:186-191`（`has_token()` = `user_token` 非空 → 通用站点恒 false）、`ui/property_panel.cpp:237-239`（永久显示「userToken：未获取（网页版接口需要它，请先登录）」）、`ui/property_panel.cpp:126-130` + `web/webview_host.h:32`（Cookie 名回落 `ds_session_id`）、`ui/app.cpp:196-214`（状态栏只看内存槽）、`ai/provider_spec.cpp:388-391`（把「缺 `probe_paths`/`token_expr`/`cookie_names`」写成「该站点**无法自动探测凭证**」）。
- **改动（已按此落地）**：① 新增**纯函数** `ai::web_session_state(spec, evidence)` → `{unknown / logged_in / logged_out}` + 站点无关原因串（判据见 `D-27`：**并集**，**不看** `userToken`）；证据类型 `ai::WebSessionEvidence` 由 `web::web_session_evidence(session)` 从内存会话转换（ai 层不依赖 `web/**`，可离线断言）；② 面板：`userToken` 行**仅当条目配了 `token_expr`** 才显示（`D-28` ①，纯函数 `web_shows_user_token()`），否则显示「本条目为 DOM 站点：登录态由浏览器 profile 维持（生成不依赖 `userToken`）」；③ 状态栏 `provider_mode_text()` 改按**生效条目自己的站点键**判据（不再读默认槽）；④ `web_site_field_warnings()` 文案改「条目缺 `probe_paths`/`token_expr`/`cookie_names`：**协议探测不适用**（DOM 站点：登录态由浏览器 profile 维持…）」；⑤ 未登录态引导「登录后**无需**再点『探测网页版协议』」；⑥ **`web::ensure_session()` 对 DOM 站点改用站点无关就绪判据**（`LoginRequest.probe_applicable=false` → 只看该 origin 有没有 Cookie，**不再等 `userToken` 到超时**；内置站点路径逐字不变，守 `I2`）。
- **界面/协议的厂商专有物清零**：`ui/**`、`web/**` 已无 `ds_session_id`（删 `kDefaultCookieName`；`ui` 不再回落任何厂商 Cookie 名）。
- **实测（2026-09-27 · 全绿）**：构建 **0 error / 0 warning**；`api_probe --exec-selftest` **219 → 227 / 0**（`VB2-24` 5 项 + `VB2-26` 3 项全 PASS）；`--graph-selftest` **111 / 0**；`--provider-selftest` **50 / 0**；`grep ds_session_id` 在 `ui/**`、`web/**` **零命中**（详见 §9.8）。
- **验收**：`AB2-20` / `VB2-24` / `VB2-26`（`AB2-20` 的**手测**部分仍需现场：手动登录任一站点点「运行」看 Console）。

#### PB2-28 非 DeepSeek 站点「协议探测不适用」语义（脚本分支 + CLI 判据 + 会话失效识别）—— ✅ **已完成（v15 · 2026-09-27；①②③ v14 落地，④ v15 落地）**

- **现状证据**：`web/webview_host.cpp:1060-1086` —— `probe_paths` / `token_expr` 为空时**保持内置 DeepSeek 值**（`/api/v0/users/current`、`/api/v0/chat_session/fetch_page`、`localStorage.getItem('userToken')`）→ 在 Kimi / 通义等站点**必然 404** → 面板出现红字「探测错误」（`ui/property_panel.cpp:244-246`），用户读作「登录失败」。
- **改动（①②③ 已按此落地）**：① 新增 `ai::probe_is_applicable(spec)`（`PB2-27` 已提供）+ **只读诊断脚本**分支 `kProbeKickoffScriptReadOnly`：探测改读 `location.href` / `document.title` / `localStorage` 键名与个数 / `document.cookie` 名与个数 / 输入框与按钮候选数，**不注入任何 DeepSeek 端点、不读取 `localStorage` 的值**（脚本内既无 `/api/v0/` 也无 `localStorage.getItem('userToken')`）；输出结构与内置脚本**同名** → `poll_protocol_probe()` 解析逻辑零改动；② `deepseek-web`（`builtin:deepseek`）与**无站点参数**路径**逐字不变**（`probe_applicable` 默认 `true` → 守 `I2`）；③ CLI + 面板：`--web-probe --provider <id>` / 面板按钮对不适用站点打印「**协议探测：不适用（DOM 站点）**→ 只读诊断」+ **站点无关**登录态结论（`ai::web_session_state`；退出码 0 = 已登录 / 2 = 未登录或未确认 / 1 = 诊断本身失败），**不再打印「未取得凭证」**；面板按钮标题改为「只读诊断（该站点不适用协议探测）」。
- **仍待做**：④ 会话失效（`40002` / `401` / `40003`）识别与 `PB2-25` 合并（`D-29`）—— 需真实失效响应才能对齐（官网侧实测见 §9.4）。
- **实测（2026-09-27 · 全绿）**：`api_probe --exec-selftest` **227 → 232 / 0**（`VB2-25` 5 项全 PASS）；`--graph-selftest` **111 / 0**；`--provider-selftest` **50 / 0**；构建 **0 error / 0 warning**。⚠️ CLI 文案与退出码需 **GUI 现场实测**（`--web-probe --provider kimi-web`）。
- **④ 会话失效识别已落地（v15）**：新增 `ai::web_session_failure_hint()` / `web_session_failure_needs_relogin()`（**纯函数**，容错匹配 `code=40002` / `code= 40002` / `biz_code=40003` 与 HTTP `401` / `403`）；`web_chat()` 命中后：`401` / `40002` → 追加「请重新登录该站点」指引并**作废该站点的内存会话**（面板随即显示「未登录」，Cookie 仍留在 profile）；`40003` → 只给可操作提示（PoW / 频率 / 前端版本），**不**作废会话。实测 `VB2-27` **5 项全 PASS**，`--exec-selftest` 232 → **237 / 0**（§9.10）。
- **验收**：`AB2-20`②③ / `VB2-25`。

#### PB2-29 内置站点**选择器逐站实测与回填**（数据批次 · **逐站独立验收**）

- **现状证据**：`source/assets/providers.json:258-434`（11 条只有 `login_url` / `window_title`）→ `ai::web_login_only()`（`ai/provider_spec.cpp:404-411`）→ 运行期明确报「登录型条目（缺生成字段）」（`nodes/local_nodes.cpp:429-435`）。工具已就绪（v11 实测：`--web-adapter-selftest --provider kimi-web` 已能读出页面 URL / 标题 / 命中数 / 建议）。
- **单站标准流程**：① 界面「打开登录窗口」**手动登录**（不代填密码、不绕过验证）→ ② `aiwrite.exe --web-dom-dump --provider <id>` 读出页面**候选元素 + 建议选择器**（v15 新增；只读）→ ③ 按结果填 `input_selector` / `send` / `answer_selector`（+ 可选 `done_when` / `answer_poll_ms` / `answer_max_polls`）→ ④ 点「运行」或 `--run-selftest --web --provider <id>` 跑一次端到端生成 → ⑤ 条目置 `verified: true` + `notes` 记实测日期与站点版本 → ⑥ 回填**附录 E** 的「可登录 / 可生成」两列。
- **次序建议**：**Kimi 先行**（旧域 301 已修正、React + `contenteditable`），通过后逐站推进；ChatGPT / Claude / Gemini 需可访问网络环境（可选做）。
- **原则**：**不实测不填值**（沿用 `PB2-26`）；站点改版 → 用户改 JSON 自助修复（`R21`）。选择器写进**内置表**（团队默认）或**用户表**（个人）皆可 —— 两者机制相同。
- **验收**：`AB2-21` + 每站一行实测记录（`docs/网页版协议实测记录.md` §7.8 起逐站追加）。

#### PB2-30 端到端自检 + 面板「测试选择器」（并归入 `PB2-24`）—— 🟡 **部分完成（v15：① 已落地；② 面板按钮与 `PB2-24` 待做）**

- **① 已落地（v15）**：`--run-selftest --web --provider <id>` —— 任意网页版条目的端到端生成断言；严格解析（表外 id / 非 `kind=web` 条目 / 站点不可用 / **登录型条目** 都立即给出明确原因与退出码 `1`/`2`，**不**回落、不静默）。用法：
  `aiwrite.exe --run-selftest --web --provider kimi-web`（退出 0 = 生成成功；1 = 生成失败或条目未就绪；2 = 条目 / 站点不可用）。
- **② 待做**：面板「测试选择器」按钮（复用只读探测，把命中数与建议显示在界面）；`PB2-24`（新建站点条目 / 重新加载配置表 / 下拉即时刷新）—— 归 `B2-e` 末批。

- **改动**：① `--run-selftest --web --provider <id>`：对**任意** `dom` 条目跑真实生成断言（exit 0 = 命中 / 1 = 选择器缺项 / 2 = 站点不可用），纳入回归基线；② 面板「**测试选择器**」按钮 → 复用 `ai::dom_adapter_selftest()`（只读），把命中数与建议**显示在界面**（不必再看 Console）；③ **归口**：`PB2-24`（自建站点 UI 闭环：从内置条目复制模板 → 写 `~/.brain-ai/providers.d/<id>.json`（带 `schema_version` 信封）→ 「重新加载配置表」→ 下拉即时刷新 → 配置表错误界面可见）**并入本批**（`D-29`）。
- **验收**：`AB2-22`。

---

## §4 阶段计划与验收

### 4.1 分批提交（每批独立可验证 / 可回滚）

| 批次 | 内容 | 提交信息（约定） | 预估 |
|---|---|---|---|
| **B2-a** | `PB2-01` → `PB2-07`（L1：**JSON 配置表（official + web 两类同表）** + 加载/合并/校验 + 用户覆盖 + 打包 + API 参数化 + **网页版去硬编码** + 表驱动 UI + 测试连接） | `feat(ai+web): M_patchB L1 provider 配置表 JSON 化（API 与网页版同表 + 用户覆盖 + 校验/热重载）+ 网页版站点参数化 + --provider-selftest` | 2–2.5 天 |
| **B2-a2** | ✅ **已完成**（`PB2-17` → `PB2-19`：**登录入口去硬编码补完** + **多站点会话按站点键控** + 窗口串行 / 按站点注销） | `fix(web+ui): M_patchB L1 续 —— 网页版登录按生效条目（登录 URL / 窗口标题 / 面板入口 / mode 过滤）+ 多站点会话键控（SessionStore by site）+ 按站点注销` | 1.5–2 天 |
| **B2-a3** | ✅ **已完成（v7 · 2026-09-26）**（`PB2-20`：撤回「`mode` 按 `kind` 过滤」+ 修复面板/校验对「提供商配置」节点的生效条目解析） | `fix(ui+engine): M_patchB L1 修订 —— mode 恢复 official/web 两项可选（网页版与 API 同等优先级）+ 面板/校验按 ProviderConfig 自身条目解析站点` | 0.5 天 |
| **B2-a4** | ✅ **已完成（v9 · 2026-09-26）**（`PB2-22` 不回落 + `PB2-23` PoW 站点参数化；`PB2-21` 仅文档侧完成；`PB2-24` 后置） | `fix(ai+ui): M_patchB L1 修订 —— 网页版站点不再回落（非网页版条目明确报错 + 三条引导）+ PoW 站点参数化` | 1 天 |
| **B2-b** | `PB2-08` → `PB2-12`（L2：接口 + 工厂 + **DeepSeekWebProvider 收编** + Anthropic/Gemini + 能力驱动接线） | `feat(ai): M_patchB L2 InferenceProvider 接口与工厂（openai/anthropic/gemini/deepseek-web 按表分派）+ 能力驱动接线` | 2–3 天 |
| **B2-c** | ✅ **已完成（v11 · 2026-09-26）**（`PB2-13` → `PB2-16`：**DOM 站点执行器** + 选择器探测/诊断 + 用户自定义站点闭环 + 文档） | `feat(ai+web): M_patchB L3 通用 DOM 站点适配器（选择器 JSON 驱动）+ 选择器探测/诊断 + --web-adapter-selftest` | 5–8 天 |
| **B2-d** | 文档收尾（可并入各批） | `docs(patchB): …` | 0.5 天 |
| **B2-e** | 🟡 **进行中**（L4）：**`PB2-27` ✅（v13）+ `PB2-28` ①②③ ✅（v14）** → `PB2-28`④（会话失效识别，并入 `PB2-25`）→ `PB2-29`（逐站选择器）→ `PB2-30` + `PB2-24` 并入；不变量 `I15`/`I16`，验收 `AB2-20…22`，验证 `VB2-24…26`，风险 `R21…R23` | `fix(web+ui): M_patchB L4 登录层去 DeepSeek 化（站点无关登录态 + 探测不适用语义）+ 内置站点选择器实测回填 + 自建站点 UI 闭环` | 3–5 天 |

> `B2-a` 与 `B2-b` 可**完全离线自检**；`B2-c` 与 `B2-e`（L4）需现场手测（GUI + 真实站点 + **逐站人工登录**），建议**单独排期**。
> 配置表数据文件（`source/assets/providers.json`）**已经先于代码落盘**（见 §8 变更记录 v2），`B2-a` 要做的是「让它真的被读取」。

### 4.2 验收标准（AB2-*）

| 编号 | 标准 | 判定方式 |
|---|---|---|
| AB2-01 | **厂商元数据零硬编码**：C++ 源码中除最小兜底表（2 条）外，**不存在**具体厂商地址/模型/认证常量 | `grep` 审查 + 代码 diff |
| AB2-02 | 加一家新的 OpenAI 兼容服务：用户**只写 JSON**（`~/.brain-ai/providers.d/xxx.json`），完成后界面下拉出现该条目、默认地址/模型/ref 自动带出 | 手测 + `--provider-selftest` |
| AB2-03 | 「提供商」下拉**真正生效**（选 `zhipu` → 请求发往 `open.bigmodel.cn`，无需手填地址） | 离线端点断言 + 一次真实调用 |
| AB2-04 | 加一家非兼容 API：**只新增 1 个 Provider 类**（配置表里 `protocol: "anthropic"`），`nodes/**` 零改动 | 代码 diff |
| AB2-05 | 网页版新站接入 = **一份 JSON**（无 PoW 站点），选择器失效有明确报错与诊断日志 | `--web-adapter-selftest` + 手测 |
| AB2-06 | **不回退**：§0.5 全部基线保持（111/0、120/0（+新增）、七组 PASS、3-of-5、8/8、7/7、0 error 0 warning） | 逐条重跑并贴结果 |
| AB2-07 | **密钥零泄漏**：表内不含明文 Key（含疑似值检测）；`--cred-selftest` 8/8 不变 | 自检 + 代码审查 |
| AB2-08 | **请求体兼容**：既有 `--exec-selftest` 请求体断言**一行未改**仍 PASS | git diff + 自检 |
| AB2-09 | **用户可配置闭环**：不改代码、不改程序目录，仅新增/修改用户 JSON → 新条目可用（含覆盖内置条目字段）；`--provider-selftest` 打印生效表与来源 | 手测 + 自检输出 |
| AB2-10 | **表坏不致命**：语法错误 / schema 不匹配 / 必填缺失 → 明确报错 + 用上一份可用表或最小兜底继续运行，**不崩溃** | `--provider-selftest` 场景 + 手测 |
| AB2-11 | **网页版无硬编码**：`web/**` 与 `ai/deepseek_web_client.cpp` 中**不存在**站点 URL / 探测路径 / 窗口标题常量（全部来自表）；**只改表（`login_url` / `endpoints` / `probe_paths`）即可改登录页与探测目标** | `grep` 审查 + 改表实测（改后探测目标随之变化） |
| AB2-12 | **web 条目同待遇**：用户新增/覆盖站点条目后（含选择器修正），重启或「重新加载配置表」即生效；`--provider-selftest` 对 web 条目同样给出表校验与登录态结论 | 手测 + 自检输出 |
| AB2-13 | **站点身份随表变化（零改码）**：在 `~/.brain-ai/providers.json` 覆盖 `deepseek-web.web.login_url` / `window_title` → 登录窗口标题与页面、参数面板站点名/登录页、`--provider-selftest --provider deepseek-web` 输出**全部随之变化**；未选网页版条目（或条目无 `web` 段）时登录入口指向内置默认**并明确提示「当前不在网页版条目上」**（`PB2-17`） | 改表实测 + 自检输出 |
| AB2-14 | **多站点并存**：`deepseek-web` 与自建站点 A **各自完成一次登录后两者凭证同时存在**（面板「已登录站点」列出 2 项）；两个 `LLMGenerate`（`provider` 分别连线到两个 `ProviderConfig` 节点）运行时 Console 各打印自己的站点与端点；**注销 A → B 仍可用**；两站点输出分别接 `TextMerge`（变长 `texts`）与 `PromptTemplate`（变长 `vars`，`{1}/{2}`）完成汇聚（`PB2-18`/`PB2-19`） | 手测 + `VB2-16` |
| AB2-15 | **模式两项可选（决策 `D-21`）**：`提供商配置` 选任一 **official** 条目（含默认 `deepseek`）时「模式」下拉**同时列出 `official` 与 `web`**；切到 `web` 后节点预览/JSON 同步、面板站点区给「将使用内置默认站点（DeepSeek 网页版）」提示且登录入口可用；把「提供商」改成 `deepseek-web` → 站点区显示**该条目自己的**站点名/登录页/来源（不是回落 DeepSeek） | 手测（使用说明 §8 第 1c 项）+ `VB2-18` |
| AB2-16 | **站点不得静默替换（`I14` · 待实施）**：`provider=deepseek`（official）+ `mode=web` → 面板站点区 / 运行前提示 / 运行结果**三处都明确报错**并给三条引导，**绝不打开 DeepSeek 登录窗口**；把「提供商」改为 `deepseek-web` 后一切正常；自建条目（**附录 D** 的 `kimi`）在**重启后**出现在「提供商」下拉里，选它 → 登录窗口是 `https://kimi.moonshot.cn/`（生成会明确报「`adapter=dom` 未实现」） | 手测（§8 `1d`/`1e`）+ `VB2-19` |
| AB2-18 | **内置站点入口可用（登录型 · v10）**：选任一内置网页版条目（如 `Kimi（Moonshot 网页版）`）→「打开登录窗口」打开的**是该站点**（`kimi.moonshot.cn`，不是 DeepSeek）；面板站点区显示「登录型（生成未就绪）」；点运行 → **明确报错**（DOM 适配器未实现 / 登录型条目），不会静默、不会用 DeepSeek 端点；`--provider-selftest --provider kimi-web` 如实显示登录页 + 「端点：不适用 / 生成：未就绪」 | 手测（§8 `1f`）+ `VB2-21` |
| AB2-19 | **DOM 站点生成（L3 · v11）**：给任一条目补齐 `web.input_selector` / `send` / `answer_selector`（+ 可选 `done_when`）并重启 → 在界面**已登录**该站点后点「运行」→ 提示词被写入输入框、自动发送、轮询取回回答正文（超时/未稳定时**如实返回已取文本 + Console 警告**）；缺字段时 → 明确报「登录型条目」并指引 `--web-adapter-selftest`；选择器写错时 → 报可操作错误（不静默、不回落 DeepSeek） | 手测（§8 `1g`）+ `VB2-22` |
| AB2-20 | **登录可视与文案不再误导（L4）**：选任一非 DeepSeek 网页版条目（如 `kimi-web`）→ 面板**不再出现**「无法自动探测凭证 / userToken 未获取（网页版接口需要它）/ 无法登录」类文案；未登录 → 「未登录（该站点）」+ 可点「打开登录窗口」；**手动登录后**（该站点出现 Cookie）→ 「已登录（该站点，Cookie N 条）」，且**不需要**再点「探测网页版协议」；「探测协议」对不适用站点打印「不适用（DOM 站点）」而非报错；`deepseek-web` 与无参路径行为**逐字不变**（守 `I2`） | 手测 + `VB2-24`/`VB2-25` |
| AB2-21 | **任一 Web AI 可登录并生成（L4 核心）**：按 `PB2-29` 流程为某站点填好选择器 → **手动登录** → 点「运行」→ 提示词被写入输入框、自动发送、取回回答正文（Console 有「适配器 dom」/ `input_selector 命中 N` / 轮询次数）；该条目 `verified: true` 且附录 E 的「可生成」为 ✅ | 手测（使用说明 §8 新增 `1h`）+ `--web-adapter-selftest --provider <id>` exit 0 |
| AB2-22 | **零改码接入任意站点（含自助闭环 · `PB2-24` 并入）**：界面「新建站点条目（从内置条目复制模板）」→ 填 `login_url` + 选择器 → 「重新加载配置表」→ 下拉**即时**出现 → 登录 → 生成成功；全程**不改 C++、不重编译、不重启** | 手测 + `--provider-selftest` |

### 4.3 技术验证项（VB2-*）汇总

| 编号 | 内容 | 落在哪个自检 |
|---|---|---|
| VB2-01 | **配置表加载/合并/校验**：必填缺失 / 类型错误 / 未知字段警告 / dup id 覆盖 / 疑似密钥拒绝 / 坏 JSON / `schema_version` 不匹配 / 兜底触发 | `--provider-selftest`（+ api_probe 纯函数断言） |
| VB2-02 | 查找顺序：exe/assets → 源码目录（开发态）→ 兜底；CMake 拷贝产物存在 | `--provider-selftest` + 构建 |
| VB2-03 | 用户覆盖：字段级 merge / `providers.d` 多文件顺序 / 新条目可见 / `replace_all` / 白名单拒绝 / **web 条目覆盖生效** | `--provider-selftest` |
| VB2-04 | 端点拼接 5 例 + 认证头 5 例 + env 名列表按序命中 | `--exec-selftest`（纯函数） |
| VB2-05 | 表驱动下拉与 **`mode` 候选恒为 `{official, web}`（决策 `D-21`：不按 `kind` 裁剪）** / 默认值补齐 / 节点参数覆盖 / 连线优先 / **改表中 `web.login_url` 后探测目标随之变化** | `--exec-selftest` + `--graph-selftest` |
| VB2-06 | `config.toml` 往返 / 旧单节迁移幂等 / 失败不覆盖原文件 | `--exec-selftest` |
| VB2-07 | 测试连接：离线断言 + 无 Key 退出码 2 + 坏表报错文案 | `--provider-selftest` |
| VB2-08 | 能力表来源正确（web `vision=false` 等） | `--exec-selftest` |
| VB2-09 | 工厂按 `protocol` 分派 / 未知 protocol 报错 / 旧函数重载仍在 | `--exec-selftest` |
| VB2-10 | Anthropic：请求体 / 头 / 解析 / 错误映射 | `--exec-selftest` |
| VB2-11 | Gemini：URL 转义 / `generationConfig` / 解析 / 认证风格 | `--exec-selftest` |
| VB2-12 | 视觉门控 3 例 + 缺 Key 文案含表内 env 名 | `--exec-selftest` |
| VB2-13 | 站点条目 JSON（两种形态字段完整性 / 用户覆盖 / 缺字段报错 / `_` 前缀键被忽略且不警告） | `--provider-selftest` |
| VB2-14 | 站点选择器可达性（需 GUI + 网络） | `--web-adapter-selftest` |
| VB2-15 | **网页版去硬编码回归**：`--web-chat` / `--web-probe` / `--web-session-selftest` 结果与改造前一致（内置条目字段值=原常量） | 三个既有自检命令 |
| VB2-16 | **键控会话（纯函数）**：写站点 A → 写站点 B → **A 仍在**；`clear(A)` 不影响 B；`keys()`/`sites()` 顺序稳定；**无键（legacy）API 行为与今天一致**（同一内存槽）；`ensure_session` 按站点判 `has_token`（真值表：A 有凭证 / B 无凭证 → 只为 B 起窗口） | `--exec-selftest`（api_probe，纯函数）+ `--web-session-selftest` |
| VB2-17 | **登录请求按生效条目构造**：`interactive_login_request(deepseek-web)` 的 URL / 窗口标题 / `probe_after_load` / `offscreen` 与旧常量**逐字一致**；传入 DOM 站点样例（`_example_web_dom` 形态）→ `url` / `window_title` / `probe_paths` / `token_expr` / `cookie_names` **全按表**；条目缺 `web` 段 → 回落 + 警告 | `--exec-selftest`（纯函数）+ `--provider-selftest` |
| VB2-18 | **模式候选 + 自参数解析（纯函数）**：① `provider_mode_options(任意 provider)` **恒返回 2 项且含 `web`**（official 条目 / 表外 id 亦然）；② ProviderConfig（`provider=deepseek-web`）的自参数解析 → `kind=="web"`、`display` 非空、站点取自该条目；③ 同上但 `provider=deepseek` → `kind=="official"` 且 `mode` **不被改写**；④ `provider=表外 id` → `spec==nullptr`（**不**回落显示为 official） | `--exec-selftest`（api_probe，**4/4 PASS**）+ `--graph-selftest` |
| VB2-19 | **严格站点解析（纯函数 · 待实施）**：① `strict_web_spec_for(表内 official 条目)` **为空**（**不**回落）；② `strict_web_spec_for(deepseek-web)` 的 `login_url` / `window_title` 与条目一致；③ `strict_web_spec_for(nullptr / 表外 id)` **为空**；④ `web_provider_id_for(非 web 条目)` 为空（面板不再显示误导性站点）；⑤ 「按条目」自检 `--web-probe --provider deepseek-web` 与既有 `--web-probe` 一致（守 `I2`） | `--exec-selftest`（api_probe）+ `--provider-selftest` |
| VB2-21 | **登录型站点条目（纯函数 + 真实表）**：① `kimi-web` 可加载（不再因缺生成字段被**跳过**）② 严格解析给出该条目的登录页且 `web_site_error` 为空 ③ `web_login_only()==true` + `web_adapter_implemented("dom")==false`（→ 运行期明确报错）④ `deepseek-web` 不受影响（仍可生成）⑤ 加载报告把「缺生成字段」降级为**警告**（含「登录型站点条目」字样） | `--exec-selftest`（api_probe，**5/5 PASS**） |
| VB2-22 | **DOM 适配器纯函数（L3）**：① 轮询钳制（默认 500ms/120 次不变；极小 → 200ms/10 次；超大 → 2000ms/600 次）② 注入配置 `dom_cfg_json` 可解析且**转义安全**（提示词含引号/反斜杠/换行/制表）③ 脚本常量（kickoff/poll 读 `__aiwriteDom`；probe 读 `__aiwriteDomProbe`）④ 就绪度（生成字段齐 → 非登录型；缺 `answer_selector` → 登录型；补齐即就绪）⑤ `dom_chat` 前置校验（缺生成字段 → 立即报错，**不打开窗口、不发送**） | `--exec-selftest`（api_probe，**5/5 PASS**） |
| VB2-23 | **选择器探测 CLI（L3）**：`--web-adapter-selftest`（无 `--provider`）→ 列出全部 web 条目 + exit **2**；`--provider <id>` → 在**已开/自动打开**的站点页面上执行只读探测并打印命中数 + 建议（exit 0 = 全命中 / 1 = 有缺项；站点不可用 = 2） | 实测：`--provider kimi-web` → 读出 `URL=https://www.kimi.com/` + 标题 + 4 条建议，exit 1 |
| VB2-24 | **登录态判定（纯函数 · L4）** 5 例：① `cookie_names` 命中 → `logged_in` ② 未命中 → `logged_out` ③ **未配 `cookie_names` 但该 origin 有 Cookie → `logged_in`**（`D-27` 并集）④ 空会话 → `unknown/logged_out` ⑤ 配了 `token_expr` 但无 token → **仍按 Cookie 判**（不看 `userToken`） | `--exec-selftest` |
| VB2-25 | **探测适用性（纯函数 + 脚本不变性 · L4）**：① `probe_is_applicable(deepseek-web)` → 适用 ② `probe_is_applicable(kimi-web)` → **不适用** ③ 自建 `dom` 条目无 `probe_paths`/`token_expr`/`endpoints` → 不适用；④ **`probe_kickoff_script(默认参数)` 与改造前逐字一致**（守 `I2`）；⑤ 不适用分支的脚本**不含** `/api/v0/` 与 `localStorage.getItem('userToken')` | `--exec-selftest` |
| VB2-26 | **文案与渲染条件（离线可断言部分 · L4）**：① `web_site_field_warnings(dom 条目)` **不再包含**「无法自动探测凭证」② `userToken` 行的渲染条件 = `!token_expr.empty()`（`D-28` ①）③ 会话不可用文案只描述**站点无关**原因（Cookie / 页面），不出现 DeepSeek 专有名词 | `--exec-selftest` |

---

## §5 风险与对策

| # | 风险 | 影响 | 对策 |
|---|---|---|---|
| R1 | 改造打断现有请求体（回归） | 既有 `--exec-selftest` 120 项失败；线上行为变化 | 不变量 **I1**：旧签名保留为重载 + 默认值 = 今日 DeepSeek 行为；既有断言**一行不改**必须仍 PASS（AB2-08） |
| R2 | 工厂/接口改动波及网页版 | `--web-chat` 等自检失败、用户网页版不可用 | `web_chat` 只做**搬迁不做修改**（先加 wrapper 再切调用方，分两步提交）；不变量 **I2** |
| R3 | 「提供商」下拉生效后，老工作流语义变化 | 老 `.json` 工作流里 `provider="deepseek"` 语义从「无用」变「决定地址」 | **迁移规则**：`provider` 为空/未知 → 回退 `custom-official` + 用工作流里显式 `api_base`（老工作流行为**完全不变**）；E-01/E-02 等示例做回归加载断言 |
| R4 | 多 provider 配置迁移写坏用户 `config.toml` | 用户配置丢失 | 写盘前备份 `config.toml.bak`；迁移只在内存完成，落盘失败不覆盖原文件；`VB2-06` 断言迁移幂等 |
| R5 | 站点适配器让程序显得「时好时坏」 | 用户困惑 | 站点条目在表里标 `verified`/`notes`；失败给**可操作**文案 + `--web-adapter-selftest` 一键诊断（PB2-15）；不用适配器时完全不影响既有网页版路径 |
| R6 | 站点反爬 / 合规风险 | 账号与法律风险 | 沿用既有策略：**有头登录、用户手动操作、不代填密码、不自动刷新会话**；不注入脚本绕过验证 |
| R7 | 范围失控 | 拖住主线 | §0.3 非范围清单硬约束；L3 默认**不做**，需用户显式批准（§6） |
| R8 | 配置表改坏导致启动异常 | 用户无法使用程序 | **I8 + AB2-10**：坏表 → 报错 + 上一份可用表 / 最小兜底；`--provider-selftest` 是自助诊断入口 |
| R9 | **表与代码能力脱节**（表里写了 `protocol: "xxx"` 但程序没有对应类） | 用户以为能用的条目实际不可用 | 工厂对未知 `protocol` 明确报错；`--provider-selftest` 逐条检查「表条目 → 是否有实现」并列出「本版本支持的 protocol 清单」 |
| R10 | 表里出现明文密钥（用户图省事） | 安全泄漏 | 加载时**键名/值双重检测**（`api_key`、`"sk-"` 前缀等）→ 警告 + 拒绝该字段 + Console 给正确做法（填 key 引用名/环境变量） |
| R11 | 抽象过度 / 维护成本上升 | 长期负担 | 单文件职责清晰、纯函数优先；**新增代码预估 ≤ 1800 行**（L1 约 600–750、L2 约 500–700、L3 另计、加 JSON 数据） |
| R12 | **表里的 web 参数与内置适配器能力不同步**（用户改了 `endpoints` 里的某路径，但适配器并不使用它） | 用户以为改了就生效，实际无变化 | 校验期声明「adapter 支持的字段集」：用户写了**该 adapter 不支持**的字段 → 加载时**警告并忽略**；`--provider-selftest` 打印「本版本 `builtin:deepseek` 支持的字段清单」 |
| R13 | **站点字段填错 → 长时间等待**（选择器一直不命中） | 用户以为卡死 | `answer_poll_ms` / `answer_max_polls` 有**硬上限 + 截断警告**；超时后**如实返回已取文本 + 明确报错**，并提示「跑 `--web-adapter-selftest` 检查选择器」 |
| R14 | **同 profile 多站点共存的隔离边界被误解**（用户以为「各站点完全隔离」） | 误判为 bug（如「注销 A 把 B 也清了」「换站点后仍显示旧站点的 Cookie」） | 文档明确边界：Cookie / localStorage / 缓存**按 origin 隔离**，但**登录 profile 仍共享**（删除整个 `~/.brain-ai/webview2` = 清掉所有站点）；注销改为**按站点**、删 profile 降级为高级操作（`PB2-19`）；面板显示「已登录站点」列表让状态可见（`PB2-18`/`AB2-14`） |
| R15 | **既有断言把硬编码行为钉死**（`tools/api_probe.cpp:314-316` 断言 `interactive.url.find("deepseek.com")`） | 修复 `PB2-17` 后自检变红；或有人「顺手改断言掩盖问题」 | 断言改为**表驱动双向断言**：① 内置 `deepseek-web` 条目 → 与旧常量**逐字一致**（守住 `I2`）；② 自定义站点条目 → **按表取值**（证明去硬编码生效）；随 `PB2-17` 同步（`VB2-17`），**只允许增强、不允许放宽** |
| R16 | **「按 `kind` 收窄 `mode`」的回归**（把「两类同待遇」误做成「按条目类型砍掉一条通道」） | 老工作流（默认流 `provider=deepseek`）在界面里**选不到 `web`**，用户以为网页版被移除（本批实际发生，见 §9.1） | 语义钉死不变量 **`I13`**（候选恒两项、不静默改写）+ 决策 **`D-21`**；断言 **`VB2-18` ①** 强制「候选恒含 `web`」；人工验证 **1c 必做** |
| R17 | **老工作流依赖「回落」**（`provider=deepseek` + `mode=web` 今天靠回落偷偷用 DeepSeek） | 升级后**突然报错**，用户以为「功能被改坏了」 | 报错文案含三条引导 + **一键把「提供商」改为 `deepseek-web`**；`CHANGELOG` / 使用说明给**迁移说明**；`--run-selftest --web` 与 `api_probe` 用例同步改用网页版条目（`PB2-22` 改动 5） |
| R18 | **模板 / 文档与实现不一致 → 照抄也建不出站点**（`_example_web_dom` 不能整份复制；`provider_spec.h` 注释用扁平字段名、实际是嵌套 `send{kind,value}` / `done_when{kind,selector}`；错误只在 `app.log`） | 用户「照文档做」却静默失效，被判为 bug | 本轮：**附录 D** + 使用说明 + 实测记录给出**实测可用**的 JSON（含信封）；后置：模板改造与头注释对齐（`PB2-21` 后置项 / `PB2-24`） |
| R19 | **官网侧 token / 存储形状变化**（v9 实测：`localStorage.userToken` 从 64 字符真 token 变为 **30 字符 JSON 包裹值**；全部端点 `40003 invalid token`） | 网页版生成突然不可用；若判据偏松还会**误报 PASS**（`--web-probe` 今天在 40003 下仍打印 PASS） | `PB2-25`：① `--web-probe` 判据加「端点 `code=0`」② 会话失效**可诊断** + 「重新登录」引导（与 `PB-07` 合并）③ 形状变化走 `web.token_expr` **表驱动覆盖**（必要时解包/容错） |
| R20 | 为诊断形状变化而打印 `userToken` 原文 → 可能泄漏凭证 | 安全泄漏 | 只打印**前 4 + 长度**（沿用现有脱敏）；需要更多信息时只打印「是否 JSON 包裹 / 首字符类别」等**不可复用**特征，绝不打印可复用片段 |
| R21 | **站点 DOM / 登录墙变化** → 选择器失效（L4 引入"逐站实测选择器"后必然遇到） | 生成失败，用户判为 bug | 条目 `verified` + `notes`（记录实测日期与站点版本）；`--web-adapter-selftest --provider <id>` 一键诊断给**可操作建议**；失败文案指向「改 JSON 自助修复」（`PB2-29`/`PB2-30`） |
| R22 | **「已登录」判定收紧过严**（如强制某个 Cookie 名）→ 用户明明登录了却显示「未登录」 | 又一次"看起来不能登录" | 判据取**并集**（`D-27`：该 origin Cookie 非空 **或** `cookie_names` 命中）；且**任何**登录态都**不阻断**生成（DOM 生成只依赖页面，`dom_chat()` 对缺凭证仅给警告） |
| R23 | 把「登录」做成**自动填密码 / 绕过验证码与风控** | 合规红线、封号风险 | 只做**有头登录 + 用户手动 + 不代填密码 + 不绕过验证**（沿用 `R6`）；L4 **不实现**任何自动登录、不注入绕过脚本 |

---

## §6 待确认决策（请审核时逐条拍板）

| 编号 | 决策点 | 选项 | 状态 / 建议 |
|---|---|---|---|
| **D-08** | 本补丁做到哪一层？ | ① 只做 L1（**配置表 + 用户自定义 + API 与网页版双去硬编码**） ② L1+L2（再含工厂与各协议/适配器实现，含 `DeepSeekWebProvider` 收编） ③ L1+L2+L3（再含通用 DOM 站点） | 建议 **②**：此时「API 与网页版都已是表驱动 + 可扩展」；L3 单独立项 |
| **D-09** | `InferenceProvider` 接口形态 | ① 完全照设计 §8.1（三个虚函数） ② 合并为 `generate(params)+on_delta+images+caps()` | 建议 **②**（与流式暂停现状一致；文档注明与 §8.1 的差异） |
| **D-10** | 非兼容协议先做哪家 | ① Anthropic ② Gemini ③ 都做 | 建议 **① Anthropic**；Gemini 作为 `PB2-11` 可选 |
| **D-11** | 配置载体 | ① 只用 `config.toml` ② 只用 JSON ③ 两者分工：**厂商元数据 → JSON 表；实例参数 → config.toml** | ✅ **已定（用户指示）**：选 **③** —— 配置表以 **JSON** 存储，**允许用户自己配置**（覆盖 + 新增） |
| **D-12** | 内置条目范围 | 最小集（deepseek/zhipu/ollama/custom）或完整集（+siliconflow/openrouter/openai/anthropic/gemini，共 10 条） | ✅ **已定（用户指示）**：**完整集** —— 条目现在只是**数据**，增删成本≈0 |
| **D-13** | 「测试连接」是否发真实请求 | ① 只做离线断言 ② 发一条最小请求（消耗极小额度） | 建议 **②**（提示词极短 + 明确标注会消耗额度） |
| **D-14** | 文档命名与编号 | 保留 `M_patchB.md` + `PB2-` 前缀；或改名 `M_provider.md` | 建议**保留**（与 `M_patchA` 系列对齐） |
| **D-15** | 配置表存放位置与覆盖规则 | ① 只放 `~/.brain-ai/providers.json` ② 只放程序目录 ③ **三层：程序目录 `assets/providers.json`（权威）→ `~/.brain-ai/providers.d/*.json`（新增）→ `~/.brain-ai/providers.json`（覆盖）** | ✅ **已定（用户指示）**：选 **③**；另有「最小兜底表（2 条）」作为文件缺失时的降级 |
| **D-16** | 是否支持热重载 | ① 重启程序才生效 ② 按钮「重新加载配置表」（手动） ③ 监听文件变化（自动） | 建议 **②**（实现简单、行为可预期；③ 需文件监听，收益低风险高） |
| **D-17** | 是否提供 `--provider-dump`（打印生效表与来源） | ① 不提供 ② 提供 | 建议 **②**（用户自助排错的关键一步，成本≈20 行） |
| **D-18** | 首批内置**网页版站点**范围 | ① 仅收编 `deepseek-web`（= 现状能力，零新增站点） ② 再内置 1 个 DOM 站点并**实测通过**（如 Kimi / 通义） ③ 不内置站点，只提供 `_example_web_dom` 模板 | 建议 **①**（先保机制与回归）；DOM 站点在 L3 用模板 + 用户自助添加 |
| **D-19** | 多站点会话的**键**（`PB2-18`） | ① provider 条目 id ② **站点 origin**（如 `https://chat.deepseek.com`） ③ 表里新增自定义 `site_key` 字段 | 建议 **②**：Cookie / localStorage 天然按 origin 隔离；同一站点的多个条目可**共享**登录态；`Session` 另存 `provider_id` 仅供界面显示 |
| **D-20** | 多站点**窗口策略**（`PB2-19`） | ① **串行复用单窗口**（切站点先关旧窗再开新窗） ② N 个站点窗口**并发** | 建议 **①**：执行链是单 worker 线程顺序执行（`engine/executor.cpp:402`），串行即可满足「各站点各自登录一次 + 运行时各用各的凭证」；② 需把 `webview_host.cpp` 的进程级全局实例化，**后置**（`R14`） |
| **D-21** | `kind` 与 `mode` 的关系（**网页版 vs 官方 API 的优先级**） | ① 按 `kind` 收窄候选（official→仅 official；web→仅 web）＝ L1 收口时的实现 ② **候选恒为 `{official, web}`**，`kind` 只影响「切换提供商时的建议值」与「不一致时的提示」，且**不静默改写**用户选择 | ✅ **已定（用户指示 2026-09-26）**：选 **②** —— **「web 的优先级要和 api 同等」**，恢复到 `web` / `official` **都能选**；据此撤回 ① 的过滤与「web 条目自动锁 web」 |
| **D-22** | 「**非网页版条目 + `mode=web`**」的行为 | ① 回落表内第一个 web 条目（= `deepseek-web`，今日实现） ② **不回落**：明确报「该条目没有网页版站点」+ 三条引导（改选网页版条目 / 新建站点条目 / 改回 `official`），运行期**直接失败** | ✅ **已定（用户指示 2026-09-26）**：选 **②** —— **站点身份不得静默替换**（不变量 `I14`）；据此撤回 `web_spec_for()` 的「非 web 条目 → 回落」语义（旧行为仅供 CLI / 自检守 `I2`） |
| **D-23** | 用户如何获得「别的 AI 的网页版站点」（闭环做到哪一步） | ① A：只改文档（手写 `providers.d/*.json` + 重启） ② B：加「新建站点条目」UI + `reload_provider_specs()` + 下拉即时刷新 + 配置表错误界面可见 ③ C：提前做 L3（DOM 执行器，让第二站点真能生成） | ✅ **已定（用户指示 2026-09-26）**：本轮选 **① A**（文档路线，附录 D 给实测可用格式）；② B 登记为 **`PB2-24`（后置）**；③ C 仍按原批次 `B2-c` |
| **D-24** | 本轮（v8）范围 | ① 只做 `D-22②` + `PB2-23` ② 顺带做 `PB2-24` 的 UI 闭环 ③ 连 L3 一起 | ✅ **已定**：选 **①**（先文档 → 审核 → 再实现 `PB2-22`/`PB2-23`；`PB2-21` 只做文档侧；模板 / UI / L3 后置） |
| **D-27** | 「**已登录**」的判据（L4） | ① 该 origin Cookie 非空 ② 条目 `cookie_names` 命中 ③ 页面可达 / 用户确认 | 🟡 **待确认（建议 ①或②的并集）**：不配 `cookie_names` 的条目也要能判「已登录」；**绝不**以 `userToken` 为通用判据（不变量 `I15`、`R22`） |
| **D-28** | 通用站点是否显示 `userToken` | ① 保留，但**仅当条目配了 `token_expr`** 才显示 / 使用 ② 彻底移除该行 | 🟡 **待确认（建议 ①）**：既守 `I2`（`deepseek-web` 行为不变）又能诊断；未配 `token_expr` 的条目显示「DOM 站点：登录态由浏览器 profile 维持」 |
| **D-29** | L4 归口 | ① `PB2-24`（自建站点 UI 闭环）**并入 L4 本批**；`PB2-25`（会话失效可诊断）按 `AB2-20` 落地情况并入 ② 保持分散（`PB2-24`/`PB2-25` 仍后置） | 🟡 **待确认（建议 ①）**：用户诉求「任意 Web AI 都能登录并生成」的闭环里，**填选择器**这一步没有 UI 入口就不可用 |

### 审核确认清单（勾选后我开工）

- [ ] **§6 D-08**：确定范围（L1 / L1+L2 / 全量）
- [ ] **§6 D-09 / D-10 / D-13 / D-14 / D-16 / D-17 / D-18**：确认或修改（D-11 / D-12 / D-15 已按你的指示定稿；web 与 API 同机制为**硬性要求**）
- [ ] **§4.1**：批次划分与提交信息约定
- [ ] **§4.2 AB2-01…AB2-10**：验收标准是否够用
- [ ] **§7 附录 B 的 JSON 字段规范**：是否要增减字段（尤其 `capabilities` / `limits` / `web.*`）
- [ ] **§3 任务清单**：是否增删（如 `config.timeout/error` 接线默认**不纳入**，归 `FEA-M4-13`）
- [ ] **§6 `D-25`（新增待确认）**：`mode=web` + 非网页版条目时，**运行前校验是否阻断**（`validateBeforeRun` 返回 false）？—— 会牵动既有契约「运行前提示刻意不阻断」与 `api_probe` 的 web 用例；**我的建议：不阻断**，但文案升级为「**本次运行必然失败**」+ 三条引导，并在该节点上直接 `NodeError`
- [ ] **§6 `D-26`（新增待确认）**：站点条目的**字段级**回落是否一起收紧？—— 现状 `web.login_url` 为空会回落到**内置默认站点（DeepSeek）**的登录页（`webview_host.h:88-89`）；按 `I14` **建议：`login_url` 缺失 = 明确报错（不回落）**，`window_title` / `probe_paths` / `token_expr` 等可回落默认 + 警告
- [ ] **§3 `PB2-21` / `PB2-22` / `PB2-23` + 附录 D**：是否照此实施（附录 D 的 JSON 已实测可用）
- [ ] **§6 `D-27`（新增待确认）**：「**已登录**」的判据 —— 建议「该 origin Cookie 非空」**或**「条目 `cookie_names` 命中」的**并集**（不配 `cookie_names` 也能判），**绝不**使用 `userToken`（= 不变量 `I15`）
- [ ] **§6 `D-28`（新增待确认）**：通用站点是否还显示 `userToken` —— 建议**仅当条目配了 `token_expr`** 才显示/使用，否则显示「本条目为 DOM 站点：登录态由浏览器 profile 维持」
- [ ] **§6 `D-29`（新增待确认）**：归口 —— 建议把 `PB2-24`（自建站点 UI 闭环）**并入 L4 本批**；`PB2-25`（会话失效可诊断）按 `AB2-20` 落地情况决定是否一并
- [ ] **§3 `PB2-27`…`PB2-30` + §4.1 `B2-e`**：L4 任务与批次划分是否照此实施（先 `PB2-27`/`PB2-28` 代码 → 再逐站 `PB2-29` 数据 → `PB2-30` 自检与 UI）


---

## §7 附录

### 附录 A · 现状证据明细（可复核）

| 主题 | 文件:行 | 内容摘要 |
|---|---|---|
| 多模态请求体（唯一现成扩展点） | `ai/deepseek_official_provider.cpp:167-196` | `build_request_body(request, data_urls)`：无图 → 字符串 content；有图 → `[{type:text},{type:image_url}]` |
| 端点拼接 | `ai/deepseek_official_provider.cpp:198-202` | `trim_right_slashes(base) + "/chat/completions"` |
| Key 解析（env 名写死） | `ai/deepseek_official_provider.cpp:204-211` | `param → DEEPSEEK_API_KEY` |
| 认证头 | `ai/deepseek_official_provider.cpp:239-241` | `Authorization: Bearer` + `Accept` |
| 响应解析 | `ai/deepseek_official_provider.cpp:260-276` | `choices[0].message.content` |
| 错误分类 | `ai/deepseek_official_provider.cpp:26-46` | `classify_http_error()`：400/401/402/429/5xx → 可操作文案（401 文案里亦写死 env 名） |
| 超时写死 | `ai/deepseek_official_provider.cpp:236-238` | 连接 15s / 读 180s（未读 `config.timeout.*`） |
| 网页版端点常量 | `ai/deepseek_web_client.cpp:15-17` | host / completion / challenge 三常量 |
| 网页版请求头 | `ai/deepseek_web_client.cpp:18-30` | Bearer + Origin/Referer/UA/`x-client-platform` |
| PoW 流程 | `ai/web_pow.h:7-13`、`ai/web_pow.cpp` | 挑战 → SHA3 求解 → `x-ds-pow-response` |
| 登录窗口 URL | `web/webview_host.h:22,62`、`ui/property_panel.cpp:184` | `https://chat.deepseek.com/` 三处硬编码 |
| 页面内探测 JS | `web/webview_host.cpp:150-163` | `fetch('/api/v0/...')` —— **已具备页面内执行任意 JS 的能力**（L3 的技术基础） |
| 节点分派 | `nodes/local_nodes.cpp:291`、`:416` | `if (mode != "web")` / `if (mode == "web") throw` |
| Key 三级优先级（公共） | `nodes/local_nodes.cpp:56-113` | `resolve_official_key()`（PB-06 复用点，L1/L2 继续沿用） |
| provider 参数定义 | `engine/node_registry.cpp:303-316` | provider/model 枚举 + `model_custom` + `api_key_ref` 默认 `brain-ai/deepseek` |
| 生效解析 | `engine/provider_resolve.cpp:26-61` | 连线优先；`model_custom` 覆盖 |
| 未接线/缺 Key 文案 | `engine/provider_resolve.cpp:68-125` | 含 `DEEPSEEK_API_KEY` 文案（第 6 项硬编码之一） |
| 运行前校验 | `engine/validate.cpp:110-175` | Key / env 检查 + 「未接线分支」警告 |
| 配置单节 | `utils/config.h:66-80`、`utils/config.cpp:180-189,217-219` | `[providers.deepseek]` 单节 |
| 凭据 env 兜底 | `utils/credential.cpp:364` | `DEEPSEEK_API_KEY`（第 4 处代码硬编码） |
| 资源目录工具（可复用） | `utils/paths.h:29` | `assets_dir()` = `<exe_dir>/assets`（当前未被 provider 使用） |
| 现有 assets 内容 | `source/assets/` | 仅 `fonts/`、`icons/`、`images/sample.png`（**无任何 provider 元数据文件**） |
| 打包脚本先例（可仿写） | `CMakeLists.txt:139-167`、`cmake/copy_runtime_dlls.cmake` | `aiwrite_copy_runtime_dlls()` 的 POST_BUILD 拷贝模式 |
| 开发态宏（可复用） | `CMakeLists.txt:281` | `AIWRITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"` |
| 上期决策背景 | `docs/actionPlan/M_patchA.md` §4.1 / §12 | `PB-04` 登记项；**D-06/D-07**（优先 M5 / 视觉走智谱） |
| 设计依据 | `docs/ai_writer_nodes.md:550-582` | §8.1 抽象层 / §8.2 后端类型 / §8.3 后端选择 |
| 现有实测样例（视觉） | `source/workflows/examples/E-02_图片转小说.json`、`--vlm-selftest` | M5 已打通「OpenAI 兼容 + 智谱 `glm-4v-flash`」路径（本补丁把它从「手抄」变成「下拉」） |
| **网页版登录入口（未落地）** | `web/webview_host.h:68-75`、`ui/property_panel.cpp:105,155,183-186,119,648` | `interactive_login_request()` 固定 DeepSeek URL + 标题；`draw_web_session_section()` **无参**（`PB2-17` 待落地） |
| **运行时自动引导的站点（未落地）** | `web/webview_host.cpp:1107-1110` | `ensure_session()` 内 `LoginRequest request;` 用默认值（= DeepSeek，`webview_host.h:23`） |
| **会话存储（单槽）** | `web/session_store.h:64-82` | 一个 `Session`；`set()` 覆盖 → 多站点互斥（`PB2-18` 待落地） |
| **登录窗口（进程内单例）** | `web/webview_host.h:129`、`web/webview_host.cpp:936-943` | `login_window()` 单例，`start()` 见 `running_` 即拒绝（「已有登录窗口在运行」） |
| **注销范围（全清）** | `ui/property_panel.cpp:88-102` | `remove_all(~/.brain-ai/webview2)`：**一次注销清掉所有站点**（`PB2-19` 待落地） |
| **把硬编码钉死的断言** | `tools/api_probe.cpp:314-316` | 断言 `interactive.url.find("deepseek.com")`（`R15`；改断言须随 `PB2-17` 一起做 `VB2-17`） |
| **表字段无消费点** | `ai/provider_spec.cpp:542-546`（解析）/ `:158`、`:231`（校验白名单） | `web.cookie_names` / `web.token_expr` **全库无消费点**（尚未接线；`PB2-17`/`PB2-18` 才用得上） |
| **内置表 web 条目数** | `source/assets/providers.json` | 10 条中 `kind=web` **仅 1 条**（`deepseek-web`）；Kimi 仅顶层 `_example_web_dom` 模板（`_` 前缀，不加载） |

### 附录 B · 配置表 JSON 规范（`assets/providers.json`）

**顶层结构**

```json
{
  "schema_version": 1,
  "replace_all": false,
  "providers": [ { /* ProviderSpec */ } ]
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `schema_version` | int | ✅ | 规范版本；当前 **1**。不支持 → 整体拒绝并报错（避免误读新格式） |
| `replace_all` | bool | ❌ | **仅用户文件可用**：`true` = 忽略下层所有条目（高级用法，Console 明确提示） |
| `providers` | array | ✅ | 条目数组（可为空数组，表示「本层不贡献条目」） |

**条目字段（`ProviderSpec`）**

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `id` | string | ✅ | — | 唯一键（`provider` 参数取值）；**不可被用户覆盖**（作为合并键） |
| `display` | string | ✅ | — | 下拉显示名（可中文） |
| `kind` | string | ✅ | — | `official`（HTTP API）/ `web`（网页版）→ 决定 `mode` 可选项 |
| `protocol` | string | ✅ | — | `openai` / `anthropic` / `gemini` / `dom` / `deepseek-web`（与工厂分派一致） |
| `api_base` | string | 条件 | `""` | official 必填（用户层可省略 = 沿用下层值）；web 不用 |
| `chat_path` | string | ❌ | 按 `protocol` | `openai → /chat/completions`；`anthropic → /v1/messages`；可含 query（Azure `?api-version=`） |
| `auth_style` | string | ❌ | 按 `protocol` | `bearer` / `api-key` / `x-api-key` / `query` / `none` |
| `auth_header` | string | ❌ | 按 `auth_style` | 自定义头名（如 Azure 的 `api-key`） |
| `extra_headers` | object | ❌ | `{}` | 逐条附加（如 `anthropic-version`、`HTTP-Referer`） |
| `env_names` | array | ❌ | `[]` | 环境变量名**按序尝试**（如 `["ZHIPU_API_KEY","GLM_API_KEY"]`） |
| `key_ref_default` | string | ❌ | `brain-ai/<id>` | 凭据库默认引用名（换厂商不串味） |
| `models` | array | ❌ | `[]` | 候选模型：`[{"id":"glm-4-flash","label":"…","vision":false}]`；空数组 = 模型自由填写 |
| `vision_model_default` | string | ❌ | `""` | 视觉节点的建议模型（用于报错文案与自动带出） |
| `capabilities` | object | ❌ | 按 protocol | `{"vision":bool,"seed":bool,"system_role":bool,"stream":bool}` |
| `limits` | object | ❌ | 全局默认 | `{"image_max_bytes":8388608,"connect_timeout_s":15,"read_timeout_s":180}` |
| `web` | object | 条件 | — | `kind=web` 必填：站点字段（见下） |
| `verified` | bool | ❌ | `false` | **是否在本机实测可用**（UI 用它提示「未验证」；诚实标注，不吹可用性） |
| `notes` | string | ❌ | `""` | 显示在参数面板/悬停的帮助文本（可写「如何申请 Key」「限制」等） |
| `docs_url` | string | ❌ | `""` | 官方文档链接（悬停可点） |

**顶层与条目的通用说明**

- **`_` 前缀键 = 纯文档字段**（`_doc` / `_user_override` / `_example_web_dom` / `_secrets_policy` …）：加载器**忽略且不产生警告**（方便在表里内嵌示例与说明）
- `kind=official` 与 `kind=web` **共用同一套加载 / 合并 / 覆盖 / 校验 / UI / 自检**代码，仅字段子集与专用子对象不同

**`web` 子对象字段（`kind=web`）** —— 按 `adapter` 分两种形态

> ⚠️ `web.adapter` 取值：`builtin:<name>`（内置适配器，目前 `builtin:deepseek`）或 `dom`（通用 DOM 适配器）。
> 写了**该 adapter 不支持**的字段 → 加载时**警告并忽略**（R12），`--provider-selftest` 会打印该 adapter 支持的字段清单。

| 字段 | 类型 | builtin:deepseek | dom | 说明 |
|---|---|---|---|---|
| `adapter` | string | ✅ | ✅ | `builtin:deepseek` / `dom` |
| `login_url` | string | ✅ | ✅ | 登录页（有头登录，用户手动操作） |
| `window_title` | string | ❌ | ❌ | 登录窗口标题（缺省 = 内置默认模板） |
| `endpoints` | object | ✅（缺省用内置默认+警告） | — | 站点端点：`host` / `completion_path` / `challenge_path` / `session_create_path` / `session_fetch_path` |
| `probe_paths` | array | ❌ | — | 协议探测用路径清单（如 `["/api/v0/users/current"]`） |
| `cookie_names` | array | ❌ | ❌ | 需要的 Cookie 名（**最小必要**，不全取）。⚠️ **当前无消费点**（仅解析与白名单校验）；接线见 `PB2-17`/`PB2-18` |
| `token_expr` | string | ❌ | ❌ | 在页面里求值的取 token 表达式（如 `localStorage.getItem('userToken')`）。⚠️ **当前无消费点**；接线见 `PB2-17`/`PB2-18` |
| `input_selector` | string | — | ✅ | 输入框选择器 |
| `send` | object | — | ✅ | `{"kind":"key","value":"Enter"}` 或 `{"kind":"click","selector":"…"}` |
| `answer_selector` | string | — | ✅ | 答案容器选择器（读取 `innerText`） |
| `done_when` | object | — | ❌ | 结束判定：`{"kind":"selector_gone","selector":"…"}` / `{"kind":"selector_present",…}` |
| `answer_poll_ms` | int | — | ❌ | 轮询间隔（默认 500，**有上限**） |
| `answer_max_polls` | int | — | ❌ | 最大轮询次数（默认 120，**有上限**） |

> `kind=web` 也可带 `models`（网页版模式名，如 `default` / `expert` / `deepseek-reasoner`）与 `capabilities`（网页版一般 `seed=false`、`system_role=false`）——**与 API 条目同字段**。

**字段级合并规则**（用户层只写要改的字段）

```jsonc
// ① 新增一家 OpenAI 兼容服务（~/.brain-ai/providers.d/10-my-ai.json）
{
  "schema_version": 1,
  "providers": [{
    "id": "my-ai", "display": "我的自建服务", "kind": "official", "protocol": "openai",
    "api_base": "http://192.168.1.10:8000/v1",
    "env_names": ["MY_AI_KEY"], "key_ref_default": "brain-ai/my-ai",
    "models": [{"id": "my-llm-7b", "label": "my-llm-7b（本地）"}]
  }]
}

// ② 覆盖内置条目（~/.brain-ai/providers.json）—— 只写要改的字段
{
  "schema_version": 1,
  "providers": [{
    "id": "zhipu",
    "api_base": "https://open.bigmodel.cn/api/paas/v4",
    "models": [{"id": "glm-4v-flash", "label": "glm-4v-flash（视觉·实测）", "vision": true}],
    "notes": "我自己的备注"
  }]
}
```

**最小兜底表（C++ 内联，仅 2 条，文件全缺时使用）**

| id | display | kind | protocol | api_base | 说明 |
|---|---|---|---|---|---|
| `custom-official` | 自定义（OpenAI 兼容） | official | openai | 空（必须手填） | 保证「表丢了也能像今天一样手填」 |
| `deepseek` | DeepSeek（官方 API） | official | openai | `https://api.deepseek.com` | 保持今天的行为基线，避免默认值突变 |

> 完整内置表（10 条：`deepseek` / `deepseek-web` / `zhipu` / `siliconflow` / `ollama` / `openrouter` / `openai` / `anthropic` / `gemini` / `custom-official`）
> 已经落到 **`source/assets/providers.json`**（本补丁随文档一起提供，字段与上表一致；条目带 `verified`/`notes` 如实标注实测状态）。
> 模型名 / 地址以**实测可用**为准；未实测条目会被 UI 标为「未验证」，实现阶段逐个校准后改 `verified: true`。

### 附录 C · 加载顺序与生效规则（唯一权威定义）

| 顺序 | 来源 | 角色 | 缺失时 |
|---|---|---|---|
| ① | `<exe>/assets/providers.json` | **随程序发布的内置表（权威默认）** | 尝试 ②；② 也缺 → 用最小兜底表并 Console 警告 |
| ② | `$AIWRITE_SOURCE_DIR/assets/providers.json` | **开发态兜底**（源码树直跑 / 自检） | 同上 |
| ③ | `~/.brain-ai/providers.d/*.json` | 用户**新增/覆盖**条目（文件名升序，可分享单文件） | 跳过（不报错） |
| ④ | `~/.brain-ai/providers.json` | 用户**字段级覆盖**（最高优先；支持 `replace_all`） | 跳过（不报错） |
| ⑤ | 工作流节点参数（`provider/mode/api_base/model_custom/api_key/api_key_ref`） | **运行时实例参数（最终生效值）** | 表默认值补齐 |

**生效规则**
1. `provider` 为空或表里找不到该 id → 回退 `custom-official` + 使用节点自带 `api_base`（**老工作流行为不变**，记 R3）
2. `api_base` 为空 → 用表里该条目的 `api_base`；仍为空 → 报「未填写 API 地址」
3. `model`/`model_custom`：`model_custom` 非空优先（沿用 M5-02 规则）；为空则用表内 `models[0]`
4. Key：节点参数 → `env_names` 列表（按序）→ `api_key_ref`（凭据库）→ 都无则报可操作错误（文案含表内 env 名与 ref 名）
5. 视觉：`caps.vision==false` → 明确报错；模型不在表内 → 允许 + 提示（见 PB2-12）

---

### 附录 D · 用户自建网页版站点条目（**2026-09-26 实测可用**）

**三步**：

1. 新建文件 `~/.brain-ai/providers.d/<你的id>.json`（**必须**带信封 `schema_version` + `providers` 数组；文件名任意，按名升序合并）；
2. 保存后**重启 AIwrite**（当前版本没有「重新加载配置表」入口 —— 见 `PB2-24`）；
3. 「提供商配置」→「提供商」下拉里出现该条目 → 选中（模式会带出建议值 `web`）→「打开登录窗口」即打开**该站点**。

**最小可用样例（实测通过；以 Kimi 为例）**：

```json
{
  "schema_version": 1,
  "providers": [
    {
      "id": "kimi",
      "display": "Kimi（网页版）",
      "kind": "web",
      "protocol": "dom",
      "verified": false,
      "web": {
        "adapter": "dom",
        "login_url": "https://kimi.moonshot.cn/",
        "window_title": "AIwrite · Kimi 网页版登录（登录后关闭窗口）",
        "input_selector": "[contenteditable='true']",
        "send": { "kind": "key", "value": "Enter" },
        "answer_selector": ".markdown-body",
        "done_when": { "kind": "selector_gone", "selector": "button[aria-label*='停止']" },
        "cookie_names": ["kimi_session"],
        "token_expr": "localStorage.getItem('token')",
        "answer_poll_ms": 500,
        "answer_max_polls": 120
      }
    }
  ]
}
```

**实测结果（2026-09-26，`--provider-dump`）**：

| 写法 | 结果 |
|---|---|
| 缺 `schema_version`（直接照抄 `_example_web_dom`） | `[错误] 「user.d/…」缺少 schema_version（应为整数 1；已忽略该层）` → **整层失效**，条目数仍 10 条 |
| 用扁平字段名（`send_kind` / `send_value` / `done_kind` / `done_selector`） | `[警告] … 未知字段 send_kind（已忽略）` + `[错误] … web.adapter=dom 缺少 send（已跳过该条）` → 条目被跳过，条目数仍 10 条 |
| **上述嵌套写法（本附录）** | `[配置表] 2 层，警告 4；条目 11 条（official 9 / web 2）`、`kimi kind=web adapter=dom login=https://kimi.moonshot.cn/`；并告警 `kimi web.adapter=dom 本版本尚未实现（已实现：builtin:deepseek）——选中它会在运行时明确报错` |

**当前限制（如实登记）**：`kind=web` + `adapter=dom` 的站点**登录 / 探测可用**、**生成尚不可用**（DOM 执行器 = L3 `PB2-13…16`，未开工；`D-18` 当时选择先只收编 `deepseek-web`）。内置表中 `kind=web` 仍只有 `deepseek-web` 1 条；且加站点后**必须重启**（无 reload 入口）。

### 附录 E · 内置各 AI 网页版站点入口清单（**登录型** · 2026-09-26）

> 用法：「提供商配置 →『提供商』」下拉里直接选（official 条目在前、web 条目在后）→「打开登录窗口」登录一次 → 凭证按站点隔离（**仅内存**）。
> **生成**：DOM 适配器（L3 `PB2-13…16`）未实现 + 选择器未实测 → 点运行会**明确报错**（不静默、**不**用 DeepSeek 端点）。

| 条目 id | 显示名 | 登录页（`web.login_url`） | 入口核实（2026-09-26） | 可登录 | 可生成（选择器） | 备注 |
|---|---|---|---|---|---|---|
| `deepseek-web` | DeepSeek（网页版） | `https://chat.deepseek.com/` | ✅ 已实测**可生成** | ✅ | ✅ | 唯一 `builtin:deepseek` 适配器条目 |
| `kimi-web` | Kimi（Moonshot 网页版） | `https://www.kimi.com/` | ✅ 抓取 200 | ⬜ | ⬜ | 登录型；旧域 `kimi.moonshot.cn` **301** 到此（v11 实测） |
| `tongyi-web` | 通义千问（阿里网页版） | `https://www.tongyi.com/qianwen` | ✅ 200 | ⬜ | ⬜ | ⚠️ 旧域名 `tongyi.aliyun.com` 已迁移，勿用 |
| `qwen-web` | Qwen（国际站网页版） | `https://chat.qwen.ai/` | ⚠️ 未完全核实（抓取返回移动端提示） | ⬜ | ⬜ | 登录型 |
| `chatglm-web` | 智谱清言（ChatGLM 网页版） | `https://chatglm.cn/main/alltoolsdetail` | ⚠️ 200 但有 **WAF 混淆** | ⬜ | ⬜ | 选择器 / 凭证取值必须实测 |
| `doubao-web` | 豆包（字节网页版） | `https://www.doubao.com/chat/` | ✅ 200 | ⬜ | ⬜ | 登录型 |
| `yuanbao-web` | 腾讯元宝 | `https://yuanbao.tencent.com/` | ✅ 200 | ⬜ | ⬜ | 登录型 |
| `ernie-web` | 文心一言（百度文心助手） | `https://yiyan.baidu.com/` | ✅ 200 | ⬜ | ⬜ | 站点现名「百度文心助手」 |
| `spark-web` | 讯飞星火 | `https://xinghuo.xfyun.cn/` | ✅ 200 | ⬜ | ⬜ | 登录型 |
| `chatgpt-web` | ChatGPT（chatgpt.com） | `https://chatgpt.com/` | ⚠️ 未核实（403 反爬） | ⬜ | ⬜ | 境外站点：需可访问的网络环境 |
| `claude-web` | Claude（claude.ai） | `https://claude.ai/` | ⚠️ 未核实（403） | ⬜ | ⬜ | 境外站点 |
| `gemini-web` | Gemini（gemini.google.com） | `https://gemini.google.com/app` | ⚠️ 未核实（抓取失败） | ⬜ | ⬜ | 境外站点 |

- **新增两列（L4 起）**：**可登录** = 该条目的「打开登录窗口 → 手动登录 → 抓取 Cookie」链路已实测通过；**可生成（选择器）** = `web.input_selector` / `send` / `answer_selector` **已实测填好**且端到端生成通过（⬜ = 尚未实测，**不填假值** —— 沿用 `PB2-26` 原则）。**L4 的目标就是把 11 行的「可生成」从 ⬜ 逐站变为 ✅**（`PB2-29`；每站需一次人工登录 + 选择器实测）。


- **为什么没有选择器**：`adapter=dom` 的生成字段（`input_selector` / `send` / `answer_selector`）与 `cookie_names` / `token_expr` **必须实测**；本轮**不填假值**（避免 L3 落地后误判「已适配」）→ 采用**登录型条目**（`PB2-26`：可加载 + 生成未就绪如实标记）。
- **合规**：只做**有头登录 + 用户手动 + 不代填密码 + 不绕过验证**；自动化生成需 L3 落地，且由用户自担各站点 ToS 风险（与 `R19` 同族）。
- **自行新增站点**：照**附录 D** 格式放进 `~/.brain-ai/providers.d/` 并**重启**（无 reload 入口 —— `PB2-24`）。

## §8 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-09-26 | v1（草案） | 首版：现状审计（§1，逐条证据）+ 三层方案（§2）+ 任务分解 + 阶段与验收（§4）+ 风险（§5）+ 待确认决策（§6）+ 附录 A/B |
| 2026-09-26 | v2 | **按用户第一条指示改写**：① 配置载体由「C++ 内置描述表」改为 **JSON 配置表**（`assets/providers.json`，随程序发布）+ **用户可自定义层**（`~/.brain-ai/providers.json`、`~/.brain-ai/providers.d/*.json`）；② 决策 `D-11`（JSON 表 + 实例参数分工）、`D-12`（完整内置集）、`D-15`（三层覆盖规则）**定稿**；③ 任务重编号为 `PB2-01…PB2-16`（L1 增加「加载/合并/校验」「打包与路径」「用户覆盖」「表驱动 UI + 管理入口」）；④ 新增验收 `AB2-09`（用户可配置闭环）、`AB2-10`（表坏不致命）与验证项 `VB2-01…VB2-14`；⑤ 新增风险 `R8…R11`（坏表 / 表与代码脱节 / 明文密钥 / 维护成本）；⑥ 新增不变量 `I7`（用户 JSON 即插即用）、`I8`（表坏不致命）；⑦ 新增附录 B（JSON 字段规范 + 合并示例 + 最小兜底表）与附录 C（加载顺序与生效规则）；⑧ **新增数据文件 `source/assets/providers.json`**（内置 10 条，随文档先落盘，`PB2-01/02` 让它真正被读取） |
| 2026-09-26 | **v3（当前）** | **按用户第二条指示改写（web 与 API 同机制）**：① 第一性原则新增「**API 与网页版同机制（同表 · 同规则 · 同入口）**」；② 范围把**网页版去硬编码**与**通用 DOM 适配器**列入在范围内（不再把 web 当可选层）；③ §2.1/§2.2 架构与三层模型改为「配置表承载 official + web 两类条目」；④ **`PB2-05` 扩写为「节点/UI/校验 + 网页版去硬编码」并给出逐处替换清单**（`webview_host` 登录 URL/窗口标题/探测路径、`property_panel` 登录入口、`deepseek_web_client` host 与端点、会话创建/拉取路径）；⑤ `PB2-01` 增两类条目校验规则（`web.adapter` 分支校验、`_` 前缀键忽略、轮询上限、`kind`↔`mode` 一致性）；⑥ `PB2-03`/`PB2-13`/`PB2-14` 明确「web 条目同待遇」，站点条目分 `builtin:*` 与 `dom` 两种形态；⑦ `PB2-07` 自检增**网页版路径**（只查登录态与端点一致性，不发内容）；⑧ 新增不变量 `I9`（网页版无硬编码）、`I10`（两类同待遇）、验收 `AB2-11`/`AB2-12`、验证 `VB2-15`、风险 `R12`/`R13`、决策 `D-18`；⑨ 附录 B 的 `web` 字段表改为「两种形态」并说明 `_` 前缀键语义；⑩ **`source/assets/providers.json` 的 `deepseek-web` 条目补齐** `adapter` / `window_title` / `endpoints` / `probe_paths` / `models`，并新增顶层 `_example_web_dom` 模板 |
| 2026-09-26 | **v5** | **L1 收口（代码批次，非文档）**：① 新增任务 `PB2-17`/`PB2-18`/`PB2-19` 全部落地；② `PB2-05` 逐处替换清单**全部 ✅**（新增「落地情况」表状态列）＋ `mode` 按 `kind` 过滤 ＋ 运行前提示按生效条目；③ `PB2-06` `config.toml` 多 provider（旧单节迁移 / `.bak` / `.tmp` 原子替换）；④ 新增 §9「L1 收口」小节（任务状态 + 新增断言 + 实测基线 + `AB2-13`/`AB2-14` 结论）；⑤ 顶部 v4 复核块标注「已修复」并保留历史记录 |
| 2026-09-26 | **v4** | **文档先行批次（只改文档、零代码变更）**：按「先文档后代码」把这轮复核发现的缺口**钉成规格**——① 新增任务 **`PB2-17`（网页版登录入口去硬编码，补完 `PB2-05` 漏项：登录 URL/窗口标题/面板入口/运行时默认站点/`mode` 按 `kind` 过滤/`api_probe` 断言表驱动）**、**`PB2-18`（多站点会话并存：`SessionStore` 按站点 origin 键控）**、**`PB2-19`（窗口串行复用 + 按站点注销）**；② `PB2-05` 节新增「落地情况」对照表（3 项 ✅ / 4 项 ⬜，逐条附 `file:line`）；③ 新增不变量 **`I11`**（站点身份唯一来源 = 生效条目）、**`I12`**（会话按站点独立）；④ 新增验收 **`AB2-13`**（改表即换站点，零改码）、**`AB2-14`**（多站点并存 + `TextMerge`/`PromptTemplate` 汇聚）；⑤ 新增验证 **`VB2-16`**（键控会话纯函数断言）、**`VB2-17`**（登录请求按条目构造）；⑥ 新增决策 **`D-19`**（键 = 站点 origin）、**`D-20`**（窗口先串行，多窗并发后置）；⑦ 新增风险 **`R14`**（同 profile 多站点隔离边界）、**`R15`**（断言绑死硬编码）；⑧ §4.1 新增批次 **`B2-a2`**（`PB2-17` → `PB2-19`）；⑨ 附录 A 增 8 行未落地/无消费点证据，附录 B 标注 `cookie_names`/`token_expr` 无消费点；⑩ 新增 §9「B2-b 前置复核」小节，并同步 `CHANGELOG` / `网页版协议实测记录.md` / `节点编辑器使用说明.md` / `docs/README.md` / `source/README.md` / `DevPlan.todo` |
| 2026-09-26 | **v6** | **第二轮修订（文档先行 · 零代码变更）**：用户实测「「模式」下拉没有 `web` 选项」→ 判定为**回归**（属 `PB2-05`/L1 且已标 ✅；`B2-b`/`B2-c`/`PB2-07` 均不含此项）：收口的「`mode` 按 `kind` 过滤」与 `ai::web_spec_for()`「非网页版条目 → 回落内置默认站点」+「缺省回落 + 警告」冲突，且默认工作流 `provider` = 表内第一项（`deepseek`，official）→ `web` **不可达**。按用户指示定稿 **`D-21`**（**网页版与官方 API 同等优先级**：候选恒 `{official, web}`，`kind` 只影响建议/提示）；据此修订 `PB2-01` 第 11 条、`PB2-05`（`mode` 参数 / 生效解析 / 切换提供商带出项 / 落地情况表）、`VB2-05`；新增 **`I13`** / **`PB2-20`** / **`B2-a3`** / **`AB2-15`** / **`VB2-18`** / **`R16`**；新增 §9.1 登记两处「§9 声明 ✅ 但代码未生效」（面板/校验的生效条目解析）与「人工验证 1c 未执行」；同步 `docs/CHANGELOG.md` / `节点编辑器使用说明.md` / `网页版协议实测记录.md` / `docs/README.md` |
| 2026-09-26 | **v7** | **修订落地（代码批次）**：`PB2-20` 六项全部完成 —— ① `provider_mode_options()` 恒 `{official, web}`（撤回按 `kind` 裁剪，守 `I13`）② 删除 `resolve_effective_provider()` 对 web 条目的 `mode` **静默改写** ③ 新增 `resolve_self_provider()` / `resolve_display_provider()`，面板（`property_panel.cpp:105` / `:724`）与运行前校验（`validate.cpp:138`）改按**该节点自身条目**解析 ④ 「web 条目自动锁 web」→ **切换提供商带出建议值**（只带一次）+ 不一致时橙色提示（`mode_kind_hint`）⑤ `node_registry` 文案改「两项都可选」⑥ `api_probe` 新增 `VB2-18` 4 项；实测 `--exec-selftest` **204/0**、`--graph-selftest` 111/0、`--selftest` 七组 PASS、`--provider-selftest` 50/0、`--run-selftest` PASS、`--run-selftest --web` **5/5（8.76s）**、`--web-probe` / `--web-chat` / `--web-session-selftest` PASS、构建 0 error / 0 warning（4 目标）；文档已回填（§3 `PB2-20` 落地表 / §4.1 `B2-a3` / §4.3 / §8 / §9 / §9.1 / CHANGELOG / 使用说明 / 实测记录 / README×2 / DevPlan.todo） |
| 2026-09-26 | **v8（当前）** | **第三轮复核（文档先行 · 零代码变更）：站点恒 DeepSeek** —— 用户实测「不管选哪个 AI，登录窗口都是 DeepSeek」。复核：三层叠加（① `web_spec_for()` 对非 web 条目回落表内第一个 web 条目 ② 表里 `kind=web` 仅 1 条、Kimi 只是 `_example_web_dom` 模板 ③ 自建站点闭环缺失：`reload_provider_specs()` 零调用点 / 下拉枚举冻结 / 配置表错误只在 `app.log`）+ 实测发现④ 模板与校验器 schema 不一致（缺信封 → 整层忽略；扁平字段名 → `缺少 send` 跳过该条）⑤ `adapter=dom` 未实现（可登录不可生成）⑥ `solve_pow_via_page()` 残留默认站点。**定稿 `D-22②`（不回落 + 三条引导 + 运行期直接失败）与 `D-23 A`（本轮只改文档）**；新增 `I14` / `PB2-21`·`PB2-22`·`PB2-23`（+ `PB2-24` 后置）/ `B2-a4` / `AB2-16` / `VB2-19` / `R17`·`R18` / `D-22`·`D-23`·`D-24`；新增 §9.3 与**附录 D**（实测可用的用户站点条目格式）；待确认 `D-25` / `D-26` |
| 2026-09-26 | **v9（当前）** | **修订落地（代码批次）**：`PB2-22`（**不回落**：`strict_web_spec_for()` / `web_site_error()` / `web_site_field_warnings()` / `web_adapter_implemented()`；面板错误块 + 一键改选 + 按钮禁用；校验「本次运行必定失败」不阻断；运行期 `NodeError` + 适配器门控）+ `PB2-23`（`solve_pow_via_page(site_url,…)` 按站点 + `protocol_probe_for_provider()` / `--web-probe --provider <id>`）+ `main.cpp`（`--run-selftest --web` 显式选网页版条目）+ `api_probe`（`VB2-19` 5 项）；实测 `--exec-selftest` **209/0**、`--graph-selftest` 111/0、`--selftest` 七组 PASS、`--provider-selftest` 50/0、`--run-selftest` PASS、构建 0 error / 0 warning；⚠️ `--run-selftest --web` 暂不能复测 5/5（**官网侧会话失效**：`userToken` 形状变化 + 端点 40003，旧路径同样复现 → §9.4 / 新任务 `PB2-25` / 风险 `R19`·`R20`） |
| 2026-09-26 | **v10（当前）** | **内置各 AI 网页版登录入口（登录型站点条目）**：`PB2-26`（`adapter=dom` 缺生成字段 → **警告可加载** + `web_login_only()` + 字段警告文案细分 + 运行期/CLI 如实显示「端点不适用 / 生成未就绪」，并改写 `provider_spec_selftest` 的旧期望）+ `assets/providers.json` **新增 11 个站点入口**（附录 E）+ `VB2-21` 5 项 + `AB2-18`；实测 `--provider-dump` **21 条（official 9 / web 12）**、`--exec-selftest` **214/0**、`--graph-selftest` 111/0、`--selftest` 七组 PASS、`--provider-selftest` 50/0、构建 **0 error / 0 warning**；**未做**：L3 DOM 执行器与选择器实测（`PB2-13…16`）、`PB2-25`（会话失效可诊断）、`PB2-24`（自建站点 UI 闭环） |
| 2026-09-26 | **v11（当前）** | **L3 落地（代码批次）：通用 DOM 站点适配器 + 选择器探测** —— ① 新增 `ai/dom_web_client.{h,cpp}`：`clamp_poll_params` / `dom_cfg_json`（转义安全）/ `dom_kickoff_script`（contenteditable `insertText` + input/textarea 原生 setter + `send{kind=key|click}`）/ `dom_poll_script`（`answer_selector` 末节点 innerText + `done_when`）/ `dom_probe_script` / `dom_chat`（**超时如实返回 + 警告**，R13）/ `dom_adapter_selftest`（PB2-15 诊断 + 可操作建议）② `webview_host` 增 `run_script_sync`（窗口内同步执行脚本；三级前置，**不要求内存 userToken**）+ `run_script_now`/`wait_page_ready` ③ `local_nodes` DOM 分支 + 登录型条目拦截 ④ `main.cpp` `--web-adapter-selftest` ⑤ `api_probe` `VB2-22` 5 项 ⑥ `implemented_protocols`/`implemented_web_adapters` 增 `dom`；实测 `--exec-selftest` **219/0**、`--graph-selftest` 111/0、`--selftest` 七组 PASS、`--provider-selftest` 50/0、`--provider-dump` 21 条（web 12，含 `dom`）、`--web-adapter-selftest --provider kimi-web` 端到端跑通（页面脚本真实执行）→ **发现 `kimi.moonshot.cn` 301 到 `www.kimi.com`**（条目已修正）；构建 0 error / 0 warning；文档已回填（§3 `PB2-13…16` 落地实测 / §4.1 `B2-c` / §4.2 `AB2-19` / §4.3 `VB2-22`·`VB2-23` / §9.6 / 使用说明 §9+§8 `1g` / 实测记录 §7.7 / README×2 / DevPlan） |
| 2026-09-26 | **v12** | **第四轮复核（文档先行 · 零代码变更）：登录层仍绑 DeepSeek** —— 用户实测「所有 AI 都无法登录，只能切回 DeepSeek」。复核：L3 只治**生成引擎**（`dom_chat()` 对「未取到内存凭证」仅给警告、`run_script_sync()` 不要求 `userToken`），未治 ①**登录态判据**（`has_token()` = `user_token` 非空 → 通用站点恒 false；面板恒显「userToken：未获取」；Cookie 名回落 `ds_session_id`；状态栏只看内存槽）②**探测脚本**对非 DeepSeek 站点注入 DeepSeek 端点（`probe_kickoff_script()` 空字段保持内置值 → 必然 404 → 假「探测错误」）③**11 条内置站点无选择器**（登录型条目）+ **无界面入口**（`PB2-24`）。**新增 L4 层**、任务 **`PB2-27`…`PB2-30`**（`PB2-24` 并入）、批次 **`B2-e`**、不变量 **`I15`/`I16`**、验收 **`AB2-20`…`AB2-22`**、验证 **`VB2-24`…`VB2-26`**、风险 **`R21`…`R23`**、待确认 **`D-27`/`D-28`/`D-29`**；新增 §9.7（L4 立项：用户诉求 / 逐层证据 / 与 L3 的边界 / 下一步） |
| 2026-09-27 | **v13** | **`PB2-27` 落地（代码批次）：登录/会话层去 DeepSeek 化（判据 + 文案）** —— ① 新增纯函数 `ai::web_session_state(spec, evidence)`（判据 = 条目 `cookie_names` 命中 ∪ 该 origin Cookie 非空；**不看** `userToken`；`D-27` / `I15`）+ `ai::WebSessionEvidence`（`web::web_session_evidence(session)` 转换，ai 层不依赖 `web/**`）② `ai::probe_is_applicable()`（内置适配器适用 / `dom` 站点**不适用**；`I16`）③ `ai::web_shows_user_token()`（`D-28`①：仅配了 `token_expr` 才显示 `userToken` 行）④ `LoginRequest.probe_applicable`（默认 `true` → CLI/无参路径逐字不变，守 `I2`）⑤ **`web::ensure_session()` 站点无关就绪判据**（DOM 站点只看该 origin Cookie，**不再空等 `userToken` 15/25 s**）⑥ 面板状态（已登录/未登录/未确认 + 站点无关原因）/ 状态栏（按生效条目站点键）/ `web_site_field_warnings()`（「协议探测不适用」）/ 加载报告（「登录可用；协议探测：不适用（DOM 站点）」）文案去 DeepSeek 化；**删除 `kDefaultCookieName`** → `ui/**`、`web/**` 零 `ds_session_id`。实测：构建 **0 error / 0 warning**、`--exec-selftest` **219 → 227 / 0**（`VB2-24` 5 项 + `VB2-26` 3 项全 PASS）、`--graph-selftest` **111 / 0**、`--provider-selftest` **50 / 0**、`grep ds_session_id`（`ui/**`+`web/**`）零命中；文档已回填（§3 `PB2-27` ✅ + §4.1 `B2-e` 进行中 + §8 + §9.8） |
| 2026-09-27 | **v14** | **`PB2-28` 落地（代码批次，①②③）：协议探测「不适用」语义** —— ① 新增只读诊断脚本 `kProbeKickoffScriptReadOnly`（读 `location.href` / `document.title` / `localStorage` 键名与个数 / `document.cookie` 名与个数 / 输入框与按钮候选数；**脚本内既无 `/api/v0/` 也无 `localStorage.getItem('userToken')`**；输出字段与内置脚本同名 → `poll_protocol_probe()` 零改动）② `probe_kickoff_script()` 按 `probe_applicable` 分支（默认 `true` → 内置站点与 CLI/无参路径**逐字不变**，守 `I2`）③ CLI `--web-probe --provider <id>` + 面板按钮：不适用站点打印「**协议探测：不适用（DOM 站点）**→ 只读诊断」+ **站点无关**登录态结论（`ai::web_session_state`；退出码 0=已登录 / 2=未登录或未确认 / 1=诊断失败），按钮标题改「只读诊断（该站点不适用协议探测）」；④ 会话失效识别（`40002`/`401`/`40003`）**未做** → 仍归 `PB2-25`。实测：构建 **0 error / 0 warning**、`--exec-selftest` **227 → 232 / 0**（`VB2-25` 5 项全 PASS）、`--graph-selftest` **111 / 0**、`--provider-selftest` **50 / 0**；文档已回填（§3 `PB2-28` 🟡 + §4.1 + §8 + §9.9） |
| 2026-09-27 | **v15（当前）** | **收口批次：`PB2-28`④ + `PB2-30`① + 新工具 `--web-dom-dump` + 崩溃修复** —— ① `ai::web_session_failure_hint()` / `web_session_failure_needs_relogin()`（纯函数，容错匹配 `code=40002`/`40003` 与 HTTP `401`/`403`）：`web_chat()` 命中 `401`/`40002` → 报错文案追加「重新登录该站点」并**作废该站点内存会话**；`40003` → 只提示不作废 ② `--run-selftest --web --provider <id>`（表外 id / 非 web 条目 / 站点不可用 / 登录型条目 → 退出码 1/2，不回落）③ 新增 `--web-dom-dump --provider <id>`：只读枚举页面候选 input / 发送 / 回答容器并给**建议选择器**（`PB2-29` 执行工具）④ **崩溃修复**：`ExecuteScript` 返回「字符串」时被再包一层 JSON → `json::parse` 得 string → `value()` 抛 `type_error.306` → 未捕获 → `std::terminate`/`__fastfail`（0xC0000409，stdout 缓冲全丢）：已统一解包 + 整函数 try/catch（异常今后打印可读原因）⑤ 实测：构建 **0 error / 0 warning**、`--exec-selftest` **232 → 237 / 0**（`VB2-27` 5 项全 PASS）、`--graph-selftest` 111/0、`--provider-selftest` 50/0、`--web-dom-dump --provider kimi-web` exit 0（读出 `div.chat-input-editor` 等真实候选）⑥ **实测发现（`D-30` 待拍板）**：未登录的 Kimi 亦有 4 条匿名 Cookie → 仅按「该 origin Cookie 非空」会**误报已登录**；建议判据改为「`cookie_names` 命中优先，Cookie 非空降级为『未校验』」（§9.10） |

---

## §9 实施进度（B2-a 第一批 · 2026-09-26）

> 提交：`1b15474` 之后的 L1 第一批（详见 `docs/CHANGELOG.md` 同名条目）

| 任务 | 状态 | 说明 |
|---|---|---|
| `PB2-01` 配置表加载 / 合并 / 校验 | ✅ 完成 | `ai/provider_spec.{h,cpp}`：四层来源 + 字段级覆盖 + `replace_all` + 最小兜底；校验含「类型不符跳过该条 / 未知字段警告 / `_` 前缀忽略 / **明文密钥拒绝** / `web.adapter` 分支 / 未实现协议警告」；`provider_specs_snapshot()` 线程安全 |
| `PB2-02` 落点与打包 | ✅ 完成 | `paths::providers_asset_file/user_providers_dir/user_providers_file`；`CMakeLists` 新增 `aiwrite_copy_assets()` → `build/bin/assets/providers.json` 实测存在 |
| `PB2-03` 用户自定义与覆盖 | ✅ 完成 | `~/.brain-ai/providers.d/*.json`（文件名升序）+ `~/.brain-ai/providers.json`（字段级覆盖，可 `replace_all`）；origin 标注来源；**official 与 web 同一套** |
| `PB2-04` API 请求参数化 | ⬜ 未开始 | 端点 / 认证 / env / 超时仍走现有实现（`/chat/completions` + Bearer + 15s/180s） |
| `PB2-05` 节点 / UI 表驱动 + **网页版去硬编码** | ⬜ 未开始 | 节点枚举、`api_base` 默认值、生效展示、配置表管理区、`webview_host` / `deepseek_web_client` 端点参数化 —— **全部待做** |
| `PB2-06` `config.toml` 多 provider | ⬜ 未开始 | 仍为单节 `[providers.deepseek]` |
| `PB2-07` 测试连接 + `--provider-selftest` | 🟡 **离线部分完成** | 表校验 50 项断言全 PASS；`--provider <id>` 已能按表解析地址/模型/引用名并取 Key 发 ping；**web 路径只查登录态与端点（不发送内容）**；「界面测试连接按钮」未做 |

### 实测基线（全绿）

| 命令 | 结果 |
|---|---|
| `api_probe --graph-selftest` | **111 / 0** |
| `api_probe --exec-selftest` | **170 / 0**（120 → +50 配置表断言） |
| `api_probe --selftest` | 七组全 PASS |
| `aiwrite --provider-selftest` | **50 / 0 PASS**（exit 0） |
| `aiwrite --provider-selftest --provider zhipu` | 自动带出智谱地址 + `glm-4-flash` + `brain-ai/zhipu`；exit **2**（无 Key，离线部分已过） |
| `aiwrite --provider-dump` | 10 条（official 9 / web 1）+ 来源 + 覆盖链 |
| `aiwrite --run-selftest` | PASS（离线 3/5，预期） |
| `aiwrite --run-selftest --web` | **5 / 5**，0 失败 0 跳过（9.01s，真实网页版） |
| `--cred-selftest` / `--export-selftest` | PASS / PASS |
| `--web-session-selftest` / `--web-probe` / `--web-chat` | PASS / PASS / PASS |
| `--vlm-selftest` | 离线 PASS，exit 2（实测默认值仍为智谱 `glm-4v-flash`） |
| 构建 | 0 error / 0 warning |

> **行为不变性**：本批只新增「表的加载与自检」，**未接管执行链路**（节点仍走 `ai::official_chat` / `ai::web_chat`），因此 `--run-selftest --web` / `--web-chat` / `--vlm-selftest` 行为与改造前完全一致（不变量 I1/I2 保持）。
> **下一批（B2-a 续）**：`PB2-04` → `PB2-05`（含网页版去硬编码清单）→ `PB2-06`；随后 `B2-b`（`PB2-08…PB2-12` 工厂与协议收编）。

### B2-b 前置复核（2026-09-26 · 仅文档）

> 触发：用户实测「切换提供商 + 选网页版，登录窗口**仍是 DeepSeek**」，并追问「多个 provider 能否各自保存 Cookie、两个 AI 的输出能否汇聚成一个工作流」。
> 方式：逐处追踪 `property_panel → provider_resolve → local_nodes → deepseek_web_client → webview_host` 与 `web/session_store.h`（**只读审计，未改任何代码**）。

| # | 发现 | 证据（file:line） | 结论 | 登记任务 |
|---|---|---|---|---|
| 1 | 登录请求**硬编码 DeepSeek** | `web/webview_host.h:68-75` | 面板调它 → 永远登 DeepSeek | `PB2-17` |
| 2 | 面板**拿不到当前条目** | `ui/property_panel.cpp:105`（无参）、`:644`（调用点不传节点） | 会话区不能按条目渲染 | `PB2-17` |
| 3 | 面板另有 3 处站点固定值 | `property_panel.cpp:155`、`:183-186`、`:119`、`:648` | 探测按钮 / 状态徽标 / 文案写死 | `PB2-17` |
| 4 | 运行时自动引导走默认站点 | `web/webview_host.cpp:1107-1110`（`LoginRequest request;`） | 未登录时自动开 DeepSeek 登录页 | `PB2-17` |
| 5 | `mode` 下拉未按 `kind` 过滤 | `engine/node_registry.cpp:319` | 「official 条目 + `mode=web`」静默用内置默认端点（`ai/provider_spec.h:53-58`） | `PB2-17` |
| 6 | 会话**单槽** | `web/session_store.h:64-82` | 第二个站点登录会冲掉第一个 → **多站点不可能** | `PB2-18` |
| 7 | 节点取的是「那一个」会话 | `nodes/local_nodes.cpp:415-422`、`webview_host.cpp:1107+`（`ensure_session` 只判 `userToken`） | 两个网页版节点共用同一份凭证 | `PB2-18` |
| 8 | 登录窗口**进程内单例** | `web/webview_host.h:129`、`webview_host.cpp:936-943` | 同时只能开一个站点窗口 | `PB2-19` |
| 9 | 注销 = 删整个 profile | `ui/property_panel.cpp:88-102`（`remove_all`） | 注销一个站点会清掉所有站点 | `PB2-19` |
| 10 | 断言绑死 `deepseek.com` | `tools/api_probe.cpp:314-316` | 修完必须同步改断言（且要改强） | `R15` / `VB2-17` |
| 11 | 表里 web 条目只有 1 条 | `source/assets/providers.json`（10 条，`kind=web` 仅 `deepseek-web`；`_example_web_dom` 不加载） | 「换网页版站点」在数据层也**没有别的站点可选** | `D-18`（决策 ①，维持） |
| 12 | 表字段已备好但未接线 | `ai/provider_spec.cpp:542-546`（解析）、`:158`/`:231`（白名单）；全库无消费点 | `cookie_names` / `token_expr` 正是多站点要用的两个字段 | `PB2-17`/`PB2-18` |
| 13 | 汇聚/串联在图层**已支持** | `engine/node_registry.cpp:286-291`（`TextMerge` 变长 `texts`）、`:269-274`（`PromptTemplate` 变长 `vars`，`{1}/{2}`）、`engine/graph.cpp:498-500`（变长端口不替换旧连线） | 「两个 AI 输出 → 一个节点」今天即可搭；**唯一卡点是凭证（发现 6/7）** | `AB2-14` |
| 14 | 执行链是**单 worker 线程顺序** | `engine/executor.cpp:402` | 窗口串行策略（`D-20`）足够，无需多窗口并发 | `D-20` |

> **本批次交付**：以上 14 条已全部转为 `PB2-17/18/19` + `I11/I12` + `AB2-13/14` + `VB2-16/17` + `D-19/20` + `R14/15`。
> **代码批次**：`B2-a2`（`PB2-17` → `PB2-19`），预估 1.5–2 天；**在此之前不做**任何 `web/**` / `ui/**` / `nodes/**` 改动（保持基线：`--graph-selftest 111/0`、`--exec-selftest 190/0`、`--provider-selftest 50/0`、构建 0 error/0 warning）。

### L1 收口（B2-a 第二/三批 + B2-a2 · 2026-09-26）

> 提交：本条对应 `docs/CHANGELOG.md` 的「M_patchB L1 收口」条目。

| 任务 | 状态 | 说明 / 证据 |
|---|---|---|
| `PB2-04` API 请求参数化 | ✅ 完成（前批） | `ProviderOptions` / `build_endpoint(base,path)` / `build_auth_headers` / `resolve_chat_path` / `resolve_api_key(param,env_names)` / `provider_options_from(spec)` |
| `PB2-05` 节点/UI/校验 + 网页版去硬编码 | ✅ **完成（含 2 处修订）** | 逐处替换清单**全部落地**（见该节「落地情况」表）——「**`mode` 下拉按 `kind` 过滤**」与「web 条目自动锁 `web`」已按 `D-21` **撤回**；「面板按生效条目渲染」「运行前提示按生效条目」两处的**生效条目解析**已按 `PB2-20` 修正（v7） |
| `PB2-06` `config.toml` 多 provider | ✅ 完成 | `Config::providers` 映射 + `[providers.<id>]` 读写 + **旧单节迁移**（幂等）+ 保存前 `.bak` + `.tmp` 原子替换；断言在 `api_probe --selftest` V-08 |
| `PB2-17` 登录入口去硬编码 | ✅ 完成 | `web::interactive_login_request(site,id)` / `probe_login_request` / `boot_login_request`；`draw_web_session_section(node,graph)`；`ensure_session(site_request)`；`local_nodes` 用 `ai::web_spec_for()` |
| `PB2-18` 多站点会话并存 | ✅ 完成 | `web::site_key_of()`（origin）+ `SessionStore` 按站点 `map` 键控 + 旧 API = 默认槽薄封装（保 `I2`） |
| `PB2-19` 窗口串行 + 按站点注销 | ✅ 完成 | `ensure_session` 站点对齐（PoW 依赖该站点页面）+ `logout_site`（`ICoreWebView2CookieManager` 删该 origin + 清 localStorage）+ 面板「已登录站点」/「高级删除 profile」 |
| `PB2-20` `mode` 恒两项 + 自参数解析 | ✅ 完成（v7） | `provider_mode_options()` 恒 `{official, web}`；面板/校验改 `resolve_display_provider()`；**建议值 + 橙色提示**取代静默改写；断言 `VB2-18` **4/4** |
| `PB2-07`（界面「测试连接」按钮） | ⬜ 未做 | 离线部分（`--provider-selftest`）早已完成；仅界面按钮待做 |

**新增/增强断言（本批）**

| 编号 | 内容 | 落点 |
|---|---|---|
| `VB2-17` | 无参 `interactive_login_request()` 与旧常量逐字一致；按条目的登录页/窗口标题/探测路径/站点键；缺字段回落；探测窗口按条目 | `--exec-selftest`（4 项） |
| `VB2-16` | 站点键 = origin（路径/查询/大小写归一；非 URL 原样）；多站点互不覆盖；凭证按站点；按站点注销不影响他人；`sites()`；旧 API 默认槽行为不变 | `--exec-selftest`（6 项） |
| `VB2-18` | 「模式」候选恒 `{official, web}`（不按 `kind` 裁剪）；`ProviderConfig` **自参数解析**按自身条目（kind / 显示名 / 站点与登录页）；`kind` 与 `mode` 不一致时**不改写** mode；表外 id → 条目为空 | `--exec-selftest`（**4 项全 PASS**） |
| `PB2-06`（V-08 内） | 旧单节迁移幂等；多条目往返一致；保存前 `.bak` 生成 | `api_probe --selftest` |

**实测基线（全绿，2026-09-26）**

| 命令 | 结果 |
|---|---|
| `api_probe --graph-selftest` | **111 / 0** |
| `api_probe --exec-selftest` | **204 / 0**（190 → +11（`VB2-17` 4 + `VB2-16` 6 + 模式过滤 1）→ **−1**（按 `D-21` 撤回「模式过滤」）+ **+4**（`VB2-18`，全 PASS）） |
| `api_probe --selftest` | 七组 PASS（含 V-08 的 `PB2-06` 断言） |
| `aiwrite --provider-selftest` | **50 / 0**（exit 0） |
| `aiwrite --provider-selftest --provider deepseek-web` | exit **2**（未登录，端点取自表） |
| `aiwrite --cred-selftest` / `--export-selftest` | PASS / PASS |
| `aiwrite --vlm-selftest` | 离线 PASS（exit 2） |
| `aiwrite --run-selftest` | PASS |
| `aiwrite --web-session-selftest` | PASS（userToken 64 位） |
| `aiwrite --web-probe` | PASS（`/api/v0/users/current` 200） |
| `aiwrite --web-chat "…"` | PASS（HTTP 200 / PoW 1 次） |
| `aiwrite --run-selftest --web` | **5 / 5**，0 失败 0 跳过（8.76s，v7 复测） |
| 构建 | 0 error / 0 warning（4 目标） |
| **`AB2-13` 端到端**（改表即换站点） | 用户表覆盖 `deepseek-web.web.login_url = https://chat.deepseek.com/?fromTable=1` → 日志 `登录窗口已启动（…?fromTable=1）`、`开始导航: …?fromTable=1`，会话键归一为 `https://chat.deepseek.com`，`--run-selftest --web` 仍 5/5；删除临时表后 `--provider-dump` 回到内置值 |
| **`AB2-14` 多站点** | 机制与断言就绪（`VB2-16`）；**两个真实不同站点各自登录**需用户自建第二个站点条目并各登录一次（本机内置表仅 `deepseek-web`，见 `D-18`） |

> **不改动**：凭证**只存内存**（不落盘、日志脱敏）；有头登录、用户手动操作、不代填密码、不自动刷新会话、不绕过验证（合规护栏不变）。

### 第二轮修订（v6 · `D-21`「web 与 API 同等优先级」· 2026-09-26 · 文档先行）

> 触发：用户实测「选中「提供商配置」→ 参数面板**没有 `web` 选项**」（即 `节点编辑器使用说明.md` §8 第 1c 项）。
> 判定：**回归**（不是「未排期的后续阶段」）—— 该行为属 `PB2-05`（L1）并已在 §9「L1 收口」标 ✅；`B2-b` / `B2-c` / `PB2-07` 均不含此项，后续阶段**不会**实现它。

**1) 根因（逐处证据）**

| # | 现象 | 证据 | 说明 |
|---|---|---|---|
| 1 | **official 条目**下「模式」下拉**只有 `official`** | `engine/provider_resolve.cpp:151-153`（`spec->kind=="official"` → `{"official"}`）+ `ui/property_panel.cpp:764-770`（对 `mode` 传 `enum_override`） | 候选被裁掉 → `web` **不可达** |
| 2 | **默认工作流必然命中** | `engine/node_registry.cpp:299-307`（`provider` 默认值 = 表内第一项，**official 在前** → `deepseek`）、`:317`；用户 `~/.brain-ai/workflows/default.json` 的「提供商配置」= `provider=deepseek` / `mode=official` | 老工作流默认就在 official 条目上 |
| 3 | 枚举控件**不会**补回缺失的当前值 | `ui/property_panel.cpp:479-484`（仅当「当前值不在候选里」才插到首位） | 当前值 `official` 在候选里 → 不会补上 `web` |
| 4 | **与同批承诺冲突** | `ai/provider_spec.cpp:269-283`（`web_spec_for()`：非 web 条目 → 回落表内第一个 web 条目 = 内置默认站点）、`ai/provider_spec.h:151-155`、本文件 `PB2-05`「缺省回落到内置默认 + 警告」、`PB2-01` 第 11 条「不一致 → 警告并按 `kind` 纠正」 | 「official 条目 + `mode=web`」被文档承诺为**合法状态**，却在下拉里被删除 → **自相矛盾** |
| 5 | 自检**测不到** | `source/src/main.cpp:79-86`（`--run-selftest` 把两个节点的 `mode` **都写成 `web`**）→ 5/5 PASS | 只有人机交互路径才暴露 |

**2) 同时登记的「§9 声明 ✅、代码未生效」**（同因，转 `PB2-20`）

`resolve_effective_provider()` 对**非** `LLMGenerate`/`VLMGenerate` 节点直接返回默认构造（`provider_resolve.cpp:72-82`：`uses_provider()` 只认这两类 → `spec=nullptr` / `kind=""` / `display=""` / `provider="deepseek"`），而下列三处都把它用在**「提供商配置」节点**上：

| # | 位置 | 声明（§9 收口） | 实际后果 |
|---|---|---|---|
| 1 | `ui/property_panel.cpp:724` | 「web 条目自动锁 web」（§3 `PB2-05` 落地情况表末行） | `is_web()` 恒 false → **死代码**，从不触发 |
| 2 | `ui/property_panel.cpp:105-113`（`web_site_context`） | 「`draw_web_session_section` 按生效条目渲染（站点条目/来源、登录页、适配器、Cookie 名）」 | `web_spec_for(nullptr,…)` 恒回落 → 站点区**永远**显示「⚠ 当前「提供商」不是网页版条目 / 将使用内置默认站点」；**换成 Kimi 条目也会显示 DeepSeek 的站点名与登录页** → `AB2-13` 后半条**未达成** |
| 3 | `engine/validate.cpp:138-148` | 「运行前提示按生效条目生成」（`CHANGELOG.md` 同批条目） | `:143` 的 `spec != nullptr` 恒假 → **永不提示**「官方条目 + web 模式」 |
| ✅ **已修复（v7）** | 上述三处（`:105` / `:724` / `validate.cpp:138`） | — | 全部改用 `resolve_display_provider()`（**该节点自身条目**）+ `mode_kind_hint()`；站点条目 / 登录页 / 提示不再误回落 DeepSeek（证据：`VB2-18②`） |

> 运行期不受影响（`nodes/local_nodes.cpp:418` 在 `LLMGenerate` 上解析，路径正确）→ 这解释了「离线断言与 `--run-selftest --web` 全绿、界面却不对」。

**3) 本轮定稿语义（`D-21`）**

- **`mode` 的 `official` / `web` 始终可选**（**网页版与 API 同等优先级**）；候选**不得**按 `kind` 裁剪；
- `kind` 只影响两件事：①「切换提供商」时带出的**建议值**；② 不一致时的**提示**（official 条目 + `web` → 「将使用内置默认站点」；web 条目 + `official` → 「该条目没有官方 API 通道，按表 `api_base` 解析，缺失则明确报错」）；
- **不静默改写**用户选择（撤回 `provider_resolve.cpp:124-126` 的 `result.mode = "web"` 与面板的自动锁）。

**4) 待改清单 —— ✅ 全部落地（v7 · 2026-09-26 · 代码批次）**

| # | 文件 | 改动 | 结果 |
|---|---|---|---|
| 1 | `engine/provider_resolve.{h,cpp}` | `provider_mode_options()` → 恒 `{official, web}`；删除 web 条目对 `mode` 的静默改写；新增 `resolve_self_provider()` / `resolve_display_provider()` / `provider_mode_suggestion()` / `mode_kind_hint()` | ✅ |
| 2 | `ui/property_panel.cpp` | 去掉 `mode` 的 `enum_override`；`:105` / `:724` 改用 `resolve_display_provider()`；「自动锁 web」→「切换提供商带出建议值」；不一致文案改**橙色提示** | ✅ |
| 3 | `engine/validate.cpp:136` | 改用 `resolve_display_provider()`（该节点自己的条目）；网页版条目 + `official` → 明确提示 | ✅ |
| 4 | `engine/node_registry.cpp:319-322` | 「模式」说明改为「两项都可选（网页版与官方 API 同等优先级）…」 | ✅ |
| 5 | `tools/api_probe.cpp:2315-2398` | 「模式过滤 1 项」→ `VB2-18` **4 项**（候选 / 自参数解析 / 不改写 mode / 表外 id） | ✅ |
| 6 | 文档 | 本文件 §3 / §4 / §8 / §9 + `CHANGELOG.md` / `节点编辑器使用说明.md`（§8 1c、§9） / `网页版协议实测记录.md`（§7.1 / §7.4） / `docs/README.md` / `source/README.md` / `DevPlan.todo` | ✅ |

**5) 验证结果（已回填 · 2026-09-26）**

- 断言：`--exec-selftest` **204 / 0**（`VB2-18①②③④` 全 PASS）/ `--graph-selftest` **111 / 0** / `--selftest` **七组 PASS** / `--provider-selftest` **50 / 0**；
- 运行：`--run-selftest` **PASS**（离线 3/5，失败 1 预期）、`--run-selftest --web` **5/5（0 失败 0 跳过，8.76s）**、`--web-probe` / `--web-chat` / `--web-session-selftest` **PASS**（守 `I2`）；
- **人工验证 1c**：GUI 点击项由用户确认（`节点编辑器使用说明.md` §8）；其代码路径已由 `VB2-18②③` 覆盖（面板与断言调用同一 `resolve_display_provider()` / `mode_kind_hint()`）；
- 构建：**0 error / 0 warning**（4 目标）。

---

### 第三轮复核（v8 · 「站点恒 DeepSeek」· 2026-09-26 · 文档先行 · 零代码变更）

> 触发（用户实测）：「不管选哪个 AI（网页版），登录界面都是 DeepSeek」。

**1) 三层根因 + 三处残留（逐条带证据）**

| # | 层 | 证据 | 后果 |
|---|---|---|---|
| 1 | **回落规则** | `ai/provider_spec.cpp:269-283` `web_spec_for()`：非 `kind=web` / 表外 id → 返回**表内第一个 web 条目**；`web_provider_id_for()`（`:285-299`）同；调用点 `ui/property_panel.cpp:108-111` / `:239`、`engine/validate.cpp:144`、`nodes/local_nodes.cpp:418-420` | 选任何**非网页版条目** + `mode=web` → 站点恒为 DeepSeek（面板 / 提示 / 运行三处一致） |
| 2 | **表里只有一个站点** | `assets/providers.json`：10 条（official 9 / web 1）；`_example_web_dom` 是**顶层 `_` 键**（`provider_spec.cpp:251`：`_` 前缀 = 纯文档字段，不加载）；`src/tools` 无任何消费点 | 没有「第二个 AI 的网页版」可回落 |
| 3 | **自建站点闭环缺失** | `reload_provider_specs()` **零调用点**；`src/ui` 无「配置表 / 重新加载 / 测试连接」入口；`node_registry.cpp:225-230` 幂等 → 「提供商」枚举一次性生成；配置表错误只写 `app.log` | 加站点**必须重启**；错误**界面不可见** |
| 4 | **模板 / 校验器不一致**（本轮实测） | 见**附录 D** 三次实验输出：缺信封 → 整层忽略；扁平字段名 → `缺少 send` 跳过该条；嵌套正确 → 成功（条目 11 条 / web 2） | 「照文档抄」也建不出站点 |
| 5 | **`adapter=dom` 未实现** | `provider_spec.cpp:357-360` `implemented_web_adapters()` = `{"builtin:deepseek"}`；加载后启动即告警「选中它会在运行时明确报错」 | 第二站点：**登录可用、生成不可用**（L3 未开工） |
| 6 | **残留默认站点** | `web/webview_host.cpp:1409` `solve_pow_via_page()` 内部调**无参** `ensure_session(15000,…)`；CLI / 自检走无参重载（`main.cpp:756` 等） | 极端路径下按内置默认站点开窗；CLI 恒为 DeepSeek（守 `I2`） |

**2) 与 v6 / v7 的关系（为什么「v7 修好了却还是 DeepSeek」）**

`v7`（`PB2-20`）修的是**面板取不到生效条目**（`resolve_effective_provider()` 对 ProviderConfig 恒返回默认构造）。修好后面板**正确地**按规则显示「不是网页版条目 → 将使用内置默认站点」—— 即**第 1 层的规则被如实执行了**，而第 1 层本身就是「静默换站点」。故 v7 是**必要不充分**：第 1/2/3 层属**规格与资源缺口**，须由本轮（`D-22②`）与后置项（`PB2-24` / L3）解决。

**3) 本轮定稿（用户指示 2026-09-26）**

- `D-22` 选 **②**：**不回落**（非网页版条目 + `mode=web` → 明确报错 + 三条引导；运行期 `NodeError`）→ 新增不变量 `I14`。
- `D-23` 选 **A**：本轮**只改文档**（手写 `providers.d` + 重启）；UI 闭环 → `PB2-24`（后置）；L3 → `B2-c`。
- `D-24`：本轮范围 = `PB2-22` + `PB2-23`；`PB2-21` 只做文档侧。

**4) 待确认（审核清单已列）**

- `D-25`：`mode=web` + 非网页版条目时，运行前校验**是否阻断**（建议：不阻断，文案升级为「本次运行必然失败」+ 三条引导）。
- `D-26`：`web.login_url` **缺失**时是否也禁止回落（按 `I14` 建议：禁止，明确报错；其余字段可回落 + 警告）。

**5) 改动清单（待审后执行）与验证要求**

- 见 `PB2-22` / `PB2-23` 的「改动清单」；`main.cpp` 的 `--run-selftest --web` 与 `api_probe` web 用例须同步改用 `deepseek-web` 条目（否则会因「不回落」失败）。
- 验证：`--exec-selftest`（新增 `VB2-19` 5 项）/ `--graph-selftest` / `--selftest` / `--provider-selftest`；`--run-selftest`、**`--run-selftest --web` 必须仍 5/5**、`--web-probe` / `--web-chat` / `--web-session-selftest` PASS（守 `I2`）；人工项 `节点编辑器使用说明.md` §8 `1c`/`1d`/`1e`；构建 0 error / 0 warning。

---

### 第四批（v9 · 代码批次）：`PB2-22` + `PB2-23` 落地实测（2026-09-26）

**1) 落地内容**：见 §3 `PB2-22` / `PB2-23` 的「落地实测」；`PB2-21` 只做文档侧（附录 D）；`PB2-24` 仍后置。

**2) 离线实测（全绿）**：构建 **0 error / 0 warning**（4 目标）；`api_probe --exec-selftest` **209 / 0**（204 → +5：`VB2-19①…⑤` 全 PASS）；`--graph-selftest` **111 / 0**；`--selftest` 七组 PASS；`aiwrite --provider-selftest` **50 / 0**；`--run-selftest` PASS（离线 3/5，预期）；`--web-probe --provider zhipu` → **exit 2** 且打印「不是网页版条目…三条引导」（**不开窗**）；`--web-probe --provider deepseek-web` 与旧 `--web-probe` 结果一致（守 `I2`）；GUI 冒烟：启动 → 渲染循环 → 正常退出。

**3) 网页版生成：暂不能复测 5/5（**非本批引入**，如实登记）**

| 项 | 证据（`app.log` / 命令输出） |
|---|---|
| 现象 | `--run-selftest --web` → n3 失败：`文本生成（网页版）失败：挑战解析失败: 挑战接口返回失败：Authorization Failed (invalid token)`（`{"code":40003,…}`），完成 **3/5** |
| 时间点 | **15:53:32** 同一命令 **5/5（8.76s）**；**17:16** 起稳定失败（两次复跑一致） |
| 变化点 | `[网页版探测] userToken: {"va****"0"}(len=30)`（JSON 包裹值）—— 15:53 及以前为 `8Mgu****vZkr(len=64)`（真 token）；`localStorage` 仍**含 `userToken` 键** |
| 端点 | `/api/v0/users/current -> 200 {"code":40003,…}`、`/api/v0/chat_session/fetch_page -> 200 {"code":40003,…}`（**服务端拒绝**） |
| **旁证（关键）** | `--web-session-selftest`（**未改动的旧路径**）同样只取到 30 字符 token；`--web-probe`（旧路径）同样 40003 → **与 `PB2-22` / `PB2-23` 无关** |
| 处置 | ① 用户**重新登录**一次后复测 `--run-selftest --web`（期望 5/5）；② 若仍 40003 → 按 **`PB2-25` / `R19`** 实施（判据加强 + 会话失效可诊断 + `token_expr` 覆盖） |

**4) 本批未动**：会话「只存内存、按站点键控」（`I12`）、PoW 方案 A、`--web-*` 旧自检行为（守 `I2`）、L2 / L3、`PB2-07` / `PB2-24`。

---

### 第五批（v10 · 代码批次）：内置各 AI 网页版**登录入口**（2026-09-26）

**1) 用户诉求**：为每个 AI 的 web 版本配置**正确的网页入口**；（按要求）有疑问/风险先讨论 → 采纳**方案 A（推荐）**并实施（§3 `PB2-26`）。

**2) 讨论与结论（已定）**

| 议题 | 结论 |
|---|---|
| 选择器能不能写？ | **不能凭空写对**（各站有 WAF / 反爬；本机无浏览器交互能力）→ **不填假选择器**，改用**登录型条目** |
| 登录型条目是否合法？ | 原校验器会**静默跳过**缺生成字段的 `dom` 条目（= 「照文档抄也建不出站点」的同源问题）→ 需要**小改动**：降级为**警告**（`PB2-26`） |
| 本轮交付到哪一步？ | **登录 / 协议探测可用**（窗口开在正确站点、凭证按站点隔离）；**生成**明确报错，待 L3（`PB2-13…16`）+ 选择器实测 |
| 合规 | 只做**有头登录 + 用户手动 + 不代填密码 + 不绕过验证**；自动化生成需 L3 且用户自担站点 ToS 风险 |

**3) 落地与实测**：见 §3 `PB2-26`「实测」；内置清单见**附录 E**（12 条 `kind=web`：`deepseek-web` 可生成 + 11 条登录型）。

**4) 本批未动**：L3 DOM 执行器 / 选择器实测（`PB2-13…16`）、`PB2-25`（会话失效可诊断）、`PB2-24`（自建站点 UI 闭环）、`PB2-07`。**入口 URL 核实**：11 条中 7 条本机抓取 200（Kimi / 通义 / 豆包 / 元宝 / 文心 / 星火 + deepseek-web 已实测），1 条 WAF（智谱）、1 条待复核（Qwen 国际站）、3 条境外未核实（ChatGPT / Claude / Gemini，403 或抓取失败）。

---

### 第六批（v11 · 代码批次）：L3 —— 通用 DOM 站点适配器 + 选择器探测（2026-09-26）

**1) 交付内容**：见 §3 `PB2-13`…`PB2-16` 的「落地实测」；批次 `B2-c` ✅。

**2) 关键设计（为什么能"站点=纯数据"）**

| 要点 | 做法 |
|---|---|
| 页面内执行脚本 | 复用既有 **WebView2 窗口线程 + `ExecuteScript`**：新增 `web::run_script_sync()`（同步等待结果；结果原样 JSON 回传）—— 与 PoW/协议探测同源，无新依赖 |
| 不要求内存 `userToken` | DOM 站点的登录态在**浏览器 profile** 里；`run_script_sync` 三级前置：① 窗口已在该站点 → 直接执行 ② `ensure_session` 成功 → 执行 ③ 兜底：按站点**离屏开窗 + 等 `readyState=complete`** → 执行（实测 kimi：第 ③ 级生效并成功读到页面 URL/标题） |
| 提示词写入 | `contenteditable` → `insertText`（触发 `input` 事件）；`input`/`textarea` → 原型 setter + `input`/`change`（兼容 React/Vue 受控组件） |
| 触发发送 | `send.kind=key`（默认 Enter → `keydown/keypress/keyup`）/ `click`（点 `send.selector`） |
| 取答案 | 轮询 `answer_selector` **最后一个匹配节点** 的 `innerText`；`done_when=selector_gone|selector_present` 命中即完成；未配置时按「文本连续 3 轮不变」 |
| 上限与如实 | `answer_poll_ms` ∈ [200,2000]、`answer_max_polls` ∈ [10,600]（钳制）；到上限/总超时 → **已取文本照样返回 + Console 明确警告**（R13：不假装成功、不无限等待） |

**3) 自动化实测（全绿）**：构建 **0 error / 0 warning**；`api_probe --exec-selftest` **219 / 0**（214 → +5：`VB2-22①…⑤` 全 PASS）；`--graph-selftest` **111 / 0**；`--selftest` 七组 PASS；`aiwrite --provider-selftest` **50 / 0**；`--provider-dump` → **21 条（official 9 / web 12）** + 「已实现协议：openai、deepseek-web、**dom**」+「已实现网页版适配器：builtin:deepseek、**dom**」；`--run-selftest` PASS（离线 3/5，预期）；GUI 冒烟：启动 → 渲染循环 → 正常退出。

**4) 端到端实测（窗口 + 页面脚本）**：`aiwrite.exe --web-adapter-selftest --provider kimi-web --timeout 20` → 打印 `条目 kimi-web` / `适配器 dom` / `登录页 …` / 字段缺失与登录型提示 / `站点键 https://kimi.moonshot.cn`（旧域）→ **页面探测真实执行**：`URL = https://www.kimi.com/`、`标题 = Kimi AI with K3 | Built for Agentic Coding & Knowledge Work`、`input/send/answer 命中 0`（条目尚未填选择器）+ **4 条可操作建议**、exit **1**。
→ **由此发现**：`kimi.moonshot.cn` **301 到 `www.kimi.com`**（Kimi 条目 `login_url` 已按最终域修正为 `https://www.kimi.com/`，并在 `notes` 与附录 E 标注）。

**5) 本批未做 / 下一步**：① **逐站点选择器实测**（用 `--web-adapter-selftest --provider <id>` 取选择器 → 填进条目/用户表 → 重启 → `verified: true` + 回填**附录 E**）；② `PB2-25`（会话失效可诊断：`--web-probe` 判据加「端点 `code=0`」+ 面板重登入口）；③ `PB2-24`（自建站点 UI 闭环：新建条目 / 重新加载 / 下拉刷新 / 配置表错误可见）；④ `PB2-07`（界面「测试连接」按钮）。

---

### 第七批（v12 · 文档先行）：L4 立项 —— 登录层去 DeepSeek 化（2026-09-26）

**1) 用户诉求（原话）**：「现在所有的 AI 都显示无法进行登录，只能切换到 DeepSeek。这个问题不是应该在 L3 中解决吗？应该去掉原来 DeepSeek 的硬编码」

**2) 复核结论（逐条证据见横幅与 §3）**

| 层 | 是否已去硬编码 | 证据 |
|---|---|---|
| 生成引擎（DOM 执行器） | ✅ 已去（L3） | `ai/dom_web_client.cpp:171-240`（站点 = 纯数据；`dom_chat()` 对「未取到内存凭证」**仅给警告、不阻断**）；`web::run_script_sync()` 三级前置**不要求 `userToken`**（`web/webview_host.cpp:1519-1557`） |
| 登录 / 会话判定与界面文案 | ❌ 仍有 DeepSeek 语义 | `web/session_store.cpp:186-191`（`has_token()` = `user_token` 非空）、`ui/property_panel.cpp:237-239`（永久「userToken：未获取」）、`ui/property_panel.cpp:126-130` + `web/webview_host.h:32`（Cookie 名回落 `ds_session_id`）、`ui/app.cpp:196-214`（状态栏只看内存槽）、`ai/provider_spec.cpp:388-391`（把缺探测字段写成「该站点无法自动探测凭证」） |
| 协议探测脚本 | ❌ 对通用站点注入 DeepSeek 端点 | `web/webview_host.cpp:1060-1086`（`probe_paths` / `token_expr` 为空 → **保持内置 DeepSeek 值**，在 Kimi / 通义等站点必然 404） |
| 内置站点数据（选择器） | ❌ 11 条为空（登录型条目） | `source/assets/providers.json:258-434`、`ai/provider_spec.cpp:404-411`、`nodes/local_nodes.cpp:429-435` |
| 自建站点界面闭环 | ❌ 未做（`PB2-24`） | `~/.brain-ai/providers.d` 为空目录；`reload_provider_specs()` 在界面**零调用点**，无「重新加载配置表」入口 |

**3) 定稿**：新增层 **L4**（登录/会话层去厂商化 + 站点数据落地 + 自助闭环）、任务 **`PB2-27`…`PB2-30`**（`PB2-24` 并入）、批次 **`B2-e`**、不变量 **`I15`/`I16`**、验收 **`AB2-20`…`AB2-22`**、验证 **`VB2-24`…`VB2-26`**、风险 **`R21`…`R23`**；待确认 **`D-27`**（登录态判据）/ **`D-28`**（`userToken` 显示策略）/ **`D-29`**（归口）。**本批只改文档，零代码变更。**

**4) 与 L3 的边界（为什么会「看起来没法登录」）**：L3 把**生成引擎**做成了站点无关（`dom_chat()` 不因缺 `userToken` 失败，`run_script_sync()` 也不要求它）；但**登录态的判据与文案**、**探测脚本的目标端点**、**站点数据（选择器）**、**用户自助入口**这四件事仍属 L4 —— 缺了它们，界面就会对每个非 DeepSeek 站点显示「无法自动探测凭证 / userToken 未获取」，用户读到的是「无法登录」。

**5) 下一步**：① 拍板 `D-27`…`D-29` → ② 本批文档落盘（已完成：本文档 + `CHANGELOG` + 使用说明 + 实测记录 + `docs/source README` + `DevPlan.todo`）→ ③ 开代码批次 `B2-e`（先 `PB2-27`/`PB2-28`，再逐站 `PB2-29`；**每站都需要在界面手动登录一次**，程序侧只做只读诊断与选择器回填）。

---

### 第八批（v13 · 代码批次）：`PB2-27` 落地 —— 登录/会话层去 DeepSeek 化（2026-09-27）

**1) 用户诉求与范围定稿**：用户实测「所有 AI 都无法登录，只能切回 DeepSeek」（§9.7）。本轮先做 `PB2-27`；**用户 2026-09-27 定稿「同时存在」的含义 = 会话并存 + 窗口串行**（`D-20` ①，不扩并发窗口；`R14` 保持不变）。

**2) 改动（代码）**

| 位置 | 改动 | 对应 |
|---|---|---|
| `ai/provider_spec.h/.cpp` | 新增 `WebSessionState` / `WebSessionEvidence` / `WebSessionVerdict` + 纯函数 `web_session_state(spec, evidence)`：**cookie_names 命中** 或 **该 origin Cookie 非空** → `logged_in`；已读过 Cookie 但无证据 → `logged_out`；从未读过 → `unknown`（**不看** `userToken`） | `D-27` / `I15` / `VB2-24` |
| `ai/provider_spec.h/.cpp` | 新增 `probe_is_applicable(web/spec)`：内置适配器（空 / `builtin:*`）→ 适用；`dom` 站点仅当显式配了 `probe_paths` / `token_expr` 才适用，否则**不适用** | `I16` / `VB2-25`（脚本分支在 `PB2-28`） |
| `ai/provider_spec.h/.cpp` | 新增 `web_shows_user_token(spec)`（= `!token_expr.empty()`）；`web_site_field_warnings()` 对**不适用**站点改写为「**协议探测不适用**（DOM 站点…）」；加载报告改「登录可用；协议探测：不适用（DOM 站点）」 | `D-28`① / `VB2-26` |
| `web/session_store.h/.cpp` | 新增 `web_session_evidence(session)`（Cookie 维度证据；`cookies_known` = 该站点写过会话快照） | `PB2-27`① |
| `web/webview_host.h` | `LoginRequest` 增 `probe_applicable`（默认 `true` → CLI/无参路径**逐字不变**）；`login_request_of()` 按条目设置；**删除 `kDefaultCookieName`**（`web/**` 不再出现 `ds_session_id`） | `I2` / `I15` |
| `web/webview_host.cpp` | `ensure_session()`：DOM 站点 → 就绪判据 = 该 origin **有 Cookie**（不再等 `userToken`）；不触发协议探测；超时文案站点无关（不再提 `userToken`）。内置站点路径文案与行为**逐字不变** | `AB2-20`① / `I15` |
| `ui/property_panel.cpp` | 状态行 = `已登录（该站点，Cookie N 条）` / `未登录（该站点）` / `未确认（该站点）` + 站点无关原因 + 未登录引导；`userToken` 行**仅当配了 `token_expr`**；「探测错误」红字只对**适用**站点显示；界面不再回落厂商 Cookie 名 | `AB2-20`② / `D-28`① |
| `ui/app.cpp` | `provider_mode_text()` 按**生效条目自己的站点键**判状态（不再读默认槽） | `AB2-20`③ |
| `tools/api_probe.cpp` | 新增 `VB2-24`（5 项：命中 / 未命中 / 未配 cookie_names 但有 Cookie / 空会话 / 配了 token_expr 无 token）+ `VB2-26`（3 项：警告文案 / `userToken` 渲染条件 / 结论无 DeepSeek 专有名词） | `VB2-24` / `VB2-26` |

**3) 实测（全绿）**

| 项 | 结果 |
|---|---|
| 构建 | **0 error / 0 warning**（`cmake --build build --config RelWithDebInfo`，含 `aiwrite` / `api_probe` / `webview2_login`） |
| `api_probe --exec-selftest` | **227 通过 / 0 失败**（219 → +8：`VB2-24` 5 项 + `VB2-26` 3 项**全 PASS**） |
| `api_probe --graph-selftest` | **111 通过 / 0 失败**（不变） |
| `aiwrite --provider-selftest` | **50 项通过 / 0 失败**（exit 0） |
| `aiwrite --provider-dump` | 21 条（official 9 / web 12）；`kimi-web` 行 `adapter=dom login=https://www.kimi.com/`；加载警告为「登录型站点条目：web 缺 input_selector、send、answer_selector（生成未就绪 —— **登录可用；协议探测：不适用（DOM 站点）**）」 |
| `grep ds_session_id`（`ui/**` + `web/**`） | **零命中**（仅 `ai/**` 注释与 `tools/api_probe.cpp` 遗留单槽断言中保留） |

**4) 仍未做（后续）**：`PB2-28`（探测「不适用」**只读诊断脚本**分支 + CLI `--web-probe` / `--web-adapter-selftest` 判据 + 会话失效 40002/401/40003 识别并入 `PB2-25`）、`PB2-29`（11 站选择器**逐站实测回填**，Kimi 先行；每站需界面手动登录一次）、`PB2-30`（`--run-selftest --web --provider <id>` + 面板「测试选择器」+ `PB2-24` 自助闭环）。

**5) 待确认**：`D-27`（判据 = 并集，已按建议值实现）/ `D-28`①（`userToken` 行仅当配了 `token_expr`，已按建议值实现）/ `D-29`（归口：`PB2-24` 并入 L4，`PB2-25` 并入 `PB2-28`）—— 用户 2026-09-27 指示「直接按 `PB2-27`…`PB2-30` 开工」，故本批按**建议值**实现；若需改动，改判据即可（单点：`ai::web_session_state()`）。

---

### 第九批（v14 · 代码批次）：`PB2-28` 落地 —— 协议探测「不适用」只读诊断 + CLI 判据（2026-09-27）

**1) 目标（承接 §9.7 的 ②）**：非 DeepSeek 站点上「探测网页版协议」**必然 404** → 面板红字「探测错误」被用户读作「登录失败」。修法：**不适用**的站点改跑**只读诊断**，且**不注入任何 DeepSeek 协议细节**（不变量 `I16`）。

**2) 改动（代码）**

| 位置 | 改动 | 对应 |
|---|---|---|
| `web/webview_host.cpp` | 新增 `kProbeKickoffScriptReadOnly`（只读：`location.href` / `document.title` / `localStorage` **键名与个数** / `document.cookie` **名与个数** / 输入框与按钮候选数；**无 `/api/v0/`、无 `localStorage.getItem('userToken')`**；输出字段与内置脚本同名 → 解析零改动） | `PB2-28`① / `I16` |
| `web/webview_host.cpp` | `probe_kickoff_script()`：`probe_applicable == false` → 返回只读脚本；否则**逐字**走原逻辑（内置站点 / CLI / 无参路径不变） | `PB2-28`② / `I2` |
| `web/webview_host.cpp` | `protocol_probe_with(base, spec, label, timeout)`：不适用站点打印「协议探测：**不适用**（该站点不是内置协议站点）→ 只读诊断」，输出诊断摘要 + **站点无关**登录态结论（`ai::web_session_state`）与引导；退出码 **0 = 已登录 / 2 = 未登录或未确认 / 1 = 诊断本身失败** | `PB2-28`③ |
| `web/webview_host.cpp` | `protocol_probe_for_provider()` 传入该条目的 `spec`（`protocol_probe()` 传 `nullptr`，行为不变） | `PB2-28`③ |
| `ui/property_panel.cpp` | 按钮标题按适用性切换：适用 → 「探测网页版协议（dev）」；不适用 → 「**只读诊断（该站点不适用协议探测）**」+ 对应 tooltip（明示不请求端点、不读 `userToken`） | `PB2-28`③ |
| `tools/api_probe.cpp` | 新增 `VB2-25` 5 项：① `deepseek-web` 适用 ② `kimi-web` 不适用 ③ 自建 `dom` 条目（无字段 → 不适用；配 `probe_paths` / `token_expr` → 适用）④ 默认参数脚本仍是内置 DeepSeek 行为（`probe_applicable` 默认 true，守 `I2`）⑤ 只读脚本**不含** `/api/v0/` 与 `localStorage.getItem('userToken')` | `VB2-25` |

**3) 实测（全绿）**

| 项 | 结果 |
|---|---|
| 构建 | **0 error / 0 warning** |
| `api_probe --exec-selftest` | **232 通过 / 0 失败**（227 → +5：`VB2-25` ①②③④⑤ **全 PASS**） |
| `api_probe --graph-selftest` | **111 通过 / 0 失败** |
| `aiwrite --provider-selftest` | **50 项通过 / 0 失败** |
| ⚠️ 需 GUI 现场实测 | `--web-probe --provider kimi-web` 应打印「协议探测：**不适用**…」+ 只读诊断 + 登录态结论（退出码 0/2）；面板按钮应显示「只读诊断（该站点不适用协议探测）」 |

**4) 仍未做**：④ **会话失效识别**（`40002` / `401` / `40003`）—— 归 `PB2-25`，需真实失效响应对齐（§9.4 记录过官网侧 `40003` + `userToken` 形状变化）；`PB2-29`（11 站选择器逐站实测回填，Kimi 先行；每站需界面手动登录一次）；`PB2-30`（`--run-selftest --web --provider <id>` + 面板「测试选择器」+ `PB2-24` 自助闭环）。

---

### 第十批（v15 · 代码批次）：收口 —— `PB2-28`④ / `PB2-30`① / `--web-dom-dump` / 崩溃修复（2026-09-27）

**1) 修复的 CLI 崩溃（0xC0000409）—— 根因与修法**

- **现象**：`--web-dom-dump --provider kimi-web` 无任何输出且退出码 `0xC0000409`（`STATUS_STACK_BUFFER_OVERRUN`）；`& .\aiwrite.exe ... *>` 形式连 `--help`/`--provider-selftest` 也复现。
- **根因（两个，互相独立）**：
  ① **调用方式**：`aiwrite.exe` 是 **GUI（WIN32）子系统**程序，PowerShell 的 `*>`/管道重定向会与它内部的 `attach_parent_console()` + `freopen("CONOUT$", "w", stdout)` 冲突 → 进程被 `__fastfail` 终止。**正确调用**：`Start-Process -FilePath .\aiwrite.exe -ArgumentList ... -NoNewWindow -RedirectStandardOutput <file> -PassThru -Wait`。
  ② **真实代码缺陷**：`ICoreWebView2::ExecuteScript` 对「脚本 `return` 一个**字符串**」的返回值会**再包一层 JSON 字符串** → `nlohmann::json::parse(raw)` 得到 `string` → 后续 `value("url", …)` 抛 `nlohmann::detail::type_error.306` → 未被捕获 → `std::terminate` → `__fastfail`（**stdout 缓冲随进程一起丢**，所以看起来「什么都没打印」）。既有代码（`webview_host.cpp` 的 `poll_protocol_probe`）本来就做了解包，我这里漏了。
- **修法**：`ai::dom_selector_dump()` 内统一解包（`is_string()` → 再 `parse` 内层）+ 校验必须是**对象**（否则打印实际类型）+ 整个函数 `try/catch(const std::exception&)` / `catch(...)` 兜底并 `fflush(stdout)`；关键节点加 `fflush`，保证崩溃时**已产生的输出不丢**。今后同类异常一律打印可读原因，不再静默崩。

**2) 新增能力**

| 能力 | 说明 |
|---|---|
| `ai::web_session_failure_hint()` / `web_session_failure_needs_relogin()` | **纯函数**会话失效识别：`code=40002`（容错 `"code": 40002` / `"biz_code":40003` 等写法）/ `40003` / HTTP `401` / `403`；`web_chat()` 命中 `401`/`40002` → 报错文案追加「重新登录该站点」并**作废该站点内存会话**（面板随即显示「未登录」，Cookie 仍在 profile） |
| `--run-selftest --web --provider <id>` | 任意网页版条目的**端到端生成断言**；严格解析（表外 id / 非 `kind=web` / 站点不可用 / 登录型条目 → 退出码 1/2，不回落、不静默） |
| `--web-dom-dump --provider <id>`（**新工具**） | **只读**枚举当前页面候选：输入框（`input` / `textarea` / `contenteditable`）、发送候选、回答容器，逐个打印 `id` / `class` / `placeholder` / `aria-label` / `contenteditable` / 可见性 / 文本片段，并给出**建议选择器**（`#id` → `[placeholder]` → `[aria-label]` → `tag.class…`）。`PB2-29` 的逐站回填工具 |

**3) 实测（全绿）**

| 项 | 结果 |
|---|---|
| 构建 | **0 error / 0 warning** |
| `api_probe --exec-selftest` | **237 通过 / 0 失败**（232 → +5：`VB2-27` ①②③④⑤ 全 PASS） |
| `api_probe --graph-selftest` | **111 通过 / 0 失败** |
| `aiwrite --provider-selftest` | **50 项通过 / 0 失败** |
| `--web-dom-dump`（无 `--provider`） | exit **2** + 明确引导（参数路径验证） |
| `--web-dom-dump --provider kimi-web` | exit **0**，读出真实页面：URL `https://www.kimi.com/`、标题 `Kimi AI with K3 …`；**输入框候选 = `div.chat-input-editor`（contenteditable=true，可见）**；回答容器候选 = `div.message-list`（当前隐藏，尚无消息）；发送候选多为侧栏按钮（该站点按 **Enter 发送**，故用 `send=key/Enter` 最稳） |

**4) 实测发现 → 待拍板 `D-30`（登录态判据的**误报**）**

- 现象：**未登录**的 Kimi 页面在 profile 里也有 **4 条 Cookie**（匿名 Cookie）→ 按 `D-27` 的「该 origin Cookie 非空 → 已登录」会显示 **「已登录」**，而页面右上角仍是 `Log in`。
- 影响：面板/状态栏可能**假已登录**（`R22` 的镜像面）；但**不再空等**这一收益仍在（`ensure_session` 只看 Cookie 非空 → 立刻返回）。
- **建议 `D-30`（三选一，默认 ①）**：① **`cookie_names` 命中优先**：命中 → `logged_in`；未命中但 Cookie 非空 → 新增状态「**未校验**（有 Cookie，但站点的登录 Cookie 未出现）」并如实显示；完全无 Cookie → `logged_out`；② 维持现并集（把「未校验」并入「已登录」）；③ 收紧为「必须有 `cookie_names` 命中才算已登录」（对未配 `cookie_names` 的站点恒「未确认」）。
- 配套（无论选哪个）：**为每个站点补 `cookie_names`**（用 `--web-dom-dump` / 浏览器 F12 观测登录前后的 Cookie 名差集）→ 判据立即变准。

**5) `PB2-29` 逐站回填的可行性与剩余阻塞**

- 工具链已就绪（`--web-dom-dump` 枚举候选 → `--web-adapter-selftest` 复核命中 → `--run-selftest --web --provider <id>` 端到端）。
- **第一步（我已实测，Kimi）**：输入框选择器、发送方式可确定（Kimi：`div.chat-input-editor` + `send=key/Enter`）。
- **剩余必须由人在界面完成的动作**：① 在各站点**手动登录一次**；② 登录后**手动发一条消息**（让回答容器出现）→ 才能实测 `answer_selector` 与 `done_when`；③ 点「运行」确认生成闭环。
- 未登录时无法测得 `answer_selector`（回答容器在无消息时不存在 / 隐藏）——**这就是「每个站点的 web 版本都可生成」的最后一道人工关卡**，程序侧不做代登录、不绕过验证（合规边界，`R19` 同族）。




