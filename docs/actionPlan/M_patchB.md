# M_patchB：Provider 可插拔化 —— 让「任何 AI 的 API / 网页版」都能低成本接入

> 类型：跨里程碑「地基补丁」第二期（**不占用 M1–M6 编号**，与 `M1.md … M6.md`、[M_patchA.md](M_patchA.md) 平级互链）
> 依据：2026-09-26 全库 provider / 推理链路审计（见 §1，逐条附**文件:行号**证据）
> 上游登记：本补丁是 [M_patchA.md](M_patchA.md) §4.1 **`PB-04` Provider 统一抽象**（`FEA-M4-04`）的**展开落地计划**；§12 决策 **D-06/D-07** 已确定「视觉模型走智谱、实现按 OpenAI 兼容规范」
> 用户目标（原话）：**「现在的 provider 是否已经模块化？我想设置任何 AI 的 web 或者 API 都能很容易实现？」**
> 现状结论（一句话）：**OpenAI 兼容的 API 已经「能配出来」（零代码），但架构层未模块化；非兼容协议（Anthropic / Gemini / Azure）与「任意 AI 的网页版」都必须改 C++ 源码。**
> 状态：🟡 **待审核确认** —— 本文档是行动计划草案，**用户确认范围后再开工**（§6 为待确认决策清单）
> 版本目标：v0.5.x（在已收口的 M5 核心切片之上补「推理后端可插拔」地基）
> 预计工期（估）：**L1 ≈ 1–1.5 天 · L2 ≈ 2–3 天 · L3 ≈ 5–10 天**（全职估算，含自检与文档）

---

## §0 文档元信息与约定

### 0.1 目的（为什么做这件事）

现在「换一家 AI」只有两条路：① 改界面上的 `API 地址` + `模型（自定义）`（仅当对方兼容 OpenAI）；② 改源码。
本项目要长期演进（M6 之后还有更多节点与场景），若不把推理后端抽象出来，**每接一家厂商都会污染节点执行逻辑**，并且「网页版」永远是单点实现。

**本文档的目的**：把「接入一个新的 AI」从**改代码**降级为**改数据（配置 / 描述表）**，并把必须写代码的部分收敛到**一个明确的扩展点**（新增一个 Provider 类或一个站点适配器）。

### 0.2 编号规则（与 `M_patchA` 的 `PB-xx` 严格区分）

| 前缀 | 含义 | 示例 |
|---|---|---|
| `PB2-` | 本补丁任务（Patch B 第二期） | `PB2-03` |
| `VB2-` | 技术验证 / 离线断言项 | `VB2-01` |
| `AB2-` | 验收标准 | `AB2-02` |
| `D-xx` | 决策记录（与 `M_patchA` §12 的编号**连续**） | `D-08` |

> ⚠️ `M_patchA` 里的 `PB-01…PB-09` 是**第一期**（线程化 / 流式 / 凭据 / 官方 Provider），本文档**不重开**那些编号；`PB-04` 只在 §1.4 作为「上游登记项」被引用。

### 0.3 范围与非范围

**做（In scope）**

- 推理后端的**描述表 + 接口 + 工厂**（数据驱动接入）
- `ProviderConfig` 节点 / 参数面板 / 配置文件的**多 provider 化**
- 「测试连接」入口与离线断言（`--provider-selftest`）
- 至少再落地 **1 家非 OpenAI 协议**的 Provider 类（Anthropic 或 Gemini，二选一或都做，见 §6）
- 网页版**站点描述 + 通用 DOM 适配器**（可选层，见 §6 决策）

**不做（Out of scope，明确登记避免发散）**

| 不做 | 原因 |
|---|---|
| AutoProvider（自动选择后端）/ 多 Key 轮询 / 失败自动换厂商 | 设计 §8.2 明确「**无 AutoProvider**（T-07 已取消 auto）」 |
| 模型市场 / 计费统计 / 用量报表 | 与「接入容易」无关，另立项 |
| 把网页版做成「零维护」 | 站点改版与反爬必然发生，只能做到「可声明 + 可诊断 + 明确报错」 |
| 图形化 provider 编辑器（点选生成 JSON） | 先保证手写 JSON 足够简单；图形化留给 M6-01 设置面板评估 |
| 引入新第三方依赖（HTTP/JSON 库） | 沿用 httplib / nlohmann / OpenSSL / WebView2（`M_patchA` §0.3 原则 5） |

### 0.4 执行原则（沿用 `M_patchA` §0.3，本补丁追加 2 条）

1. **运行态值 vs 文档值**：执行结果永不进 `Graph` / 撤销快照 / 工作流 JSON。
2. **只读暴露**：UI 只读执行产物；写入走 `EditorState` 快照事务。
3. **脱敏**：API Key / Cookie / userToken 只驻留内存，日志与归档一律脱敏。
4. **一次提交一个补丁**：独立可验证、可回滚；提交信息附实测数据。
5. **不引入新依赖**。
6. **（新）请求体逐字节兼容**：改造后**文本请求体必须与改造前完全一致**（字段集合一致、`stream=false` 不变），以保证既有 `--exec-selftest` 的请求体断言**不改一行仍然通过**。
7. **（新）离线可断言优先**：所有「数据驱动」的部分（描述表 / 端点拼接 / 认证头 / 能力门控 / 响应解析）都写成纯函数，能在**无网络、无 Key** 的情况下断言。

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
| **新增（本补丁）** | `aiwrite.exe --provider-selftest`（见 §3 `PB2-05`） |

---

## §1 现状体检（审计，2026-09-26）

审计方式：全库检索 provider / 端点 / 认证 / 环境变量 / 站点常量，逐条追踪调用链（节点 → ai → web）。

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

**关键事实**：`ai/` 目录里**只有两个具体实现**，没有接口、没有工厂、没有注册表；分派逻辑写在**节点实现**里（`local_nodes.cpp`）。

### 1.2 七个硬编码点（"不模块化"的根因）

| # | 硬编码内容 | 证据（文件:行） | 后果 |
|---|---|---|---|
| 1 | **后端分派**写死在节点里 | `nodes/local_nodes.cpp:291`（`if (mode != "web")`）、`:416`（`if (mode == "web") throw`） | 新增第三种后端要改节点执行逻辑 |
| 2 | **端点路径**固定 `/chat/completions` | `ai/deepseek_official_provider.cpp:198-202`、`:232-234` | 接不了 Anthropic `/v1/messages`、Gemini `:generateContent`、Azure `?api-version=` |
| 3 | **认证头**固定 `Authorization: Bearer` | `ai/deepseek_official_provider.cpp:239-241` | 接不了 Azure（`api-key:`）、Anthropic（`x-api-key` + `anthropic-version`） |
| 4 | **响应解析**固定 `choices[0].message.content` | `ai/deepseek_official_provider.cpp:260-276` | Anthropic（`content[0].text`）/ Gemini（`candidates[0].content.parts[0].text`）解析不出文本 |
| 5 | **多模态格式**固定 OpenAI `image_url` + data URL | `ai/deepseek_official_provider.cpp:188-195` | Anthropic（`source.base64`）/ Gemini（`inline_data`）需另一套映射 |
| 6 | **环境变量名**固定 `DEEPSEEK_API_KEY`（全库 **15 处**：**4 处 `getenv` 取值 + 11 处文案/注释**，含用户提示与自检输出） | 取值：`deepseek_official_provider.cpp:209`、`engine/provider_resolve.cpp:97`、`engine/validate.cpp:144`、`utils/credential.cpp:364`；文案：`main.cpp:813`、`deepseek_official_provider.cpp:33`、`provider_resolve.cpp:86,122,124`、`validate.cpp:148`、`local_nodes.cpp:57,310,453` 等 | 换厂商时 env 兜底失效；提示文案也误导（会让用户以为只能配 DeepSeek） |
| 7 | **网页版站点**全写死（host / 路径 / Cookie / PoW） | `ai/deepseek_web_client.cpp:15-17`、`ai/web_pow.h:7-13`、`web/webview_host.h:22,62`、`web/webview_host.cpp:150-163`、`ui/property_panel.cpp:184` | 换一个网站 = 重写一整套逆向客户端 |

### 1.3 `provider` 字段的真相：**装饰性字段**

| 事实 | 证据 |
|---|---|
| 参数定义只有一个枚举值 | `engine/node_registry.cpp:303` `enum_param("provider","提供商",{"deepseek"},"deepseek")` |
| 全仓库**没有任何地方按它分派** | 检索 `effective.provider` / `.provider ==` → **零命中**；取值后仅用于界面显示（`ui/property_panel.cpp:482-529`、`ui/node_canvas.cpp`） |
| 真正生效的是 `mode` + `api_base` + `model_custom` | `nodes/local_nodes.cpp:272-277`（mode/model 从 provider 输入句柄取）、`:294`（api_base）、`:227-231`（model_custom 覆盖枚举） |

> 即：**「提供商」下拉今天选什么都不会影响调用结果**；换厂商靠手填 `API 地址` + `模型（自定义）`。

### 1.4 配置层与上游登记

| 项 | 现状 | 证据 |
|---|---|---|
| `config.toml` | 只有 `[providers.deepseek]` **单节**；`Config::Provider` 是普通结构体字段 `Config::deepseek`（**不是 map**） | `utils/config.h:66-80`、`utils/config.cpp:180-189`、`:217-219` |
| 官方说明 | 自陈「`providers.deepseek.*` **仅作默认值来源**，实际以 ProviderConfig 节点参数为准」 | `utils/config.h:96-105`（`unwired_config_fields`） |
| Key 引用名默认值 | 写死 `brain-ai/deepseek`（换厂商会串味 / 可能复用错条目） | `engine/node_registry.cpp:315` |
| 超时 / 重试 | 代码写死 15s / 180s；`config.timeout.*`、`config.error.*` **零消费** | `ai/deepseek_official_provider.cpp:236-238`、`utils/config.h:48-58`、审计项 `FEA-M4-13` |
| 上游登记项 | `PB-04` Provider 统一抽象（`FEA-M4-04`）：`ai/inference_provider.h` + `ai/provider_factory.{h,cpp}` + **把 `web_chat()` 收编为 `DeepSeekWebProvider`**（保持 `--web-chat` 行为不变） | `M_patchA.md` §4.1（PB-04 行）、§2 剩余汇总行 |
| 设计文档 | §8.1 已给出 `InferenceProvider{generate, generateStream, generateWithImage, name, supportsVision}`；§8.2 后端类型；§8.3 后端选择（official / web） | `ai_writer_nodes.md:550-582`、`:136-153` |

### 1.5 三类「换个 AI」的真实成本（今天 vs 目标）

| 场景 | 今天 | 目标（本补丁后） |
|---|---|---|
| OpenAI 兼容 API（智谱 / 硅基流动 / Ollama / OpenRouter / vLLM） | **零代码**，但要在界面手抄 URL 与模型名；无候选提示、无连通性测试；接错只在运行时才知道 | 下拉选厂商 → 默认地址 / 模型 / Key 引用名**自动带出** → 点「测试连接」即时验证 |
| 非兼容 API（Anthropic / Gemini / Azure OpenAI） | **必须改源码**（端点 + 认证 + body + 响应 4 处硬编码） | 新增一个 Provider 类（约 120–180 行，纯新增，**不改节点**）或在描述表切换 |
| 任意 AI 的**网页版**（Kimi / 通义 / 豆包 / ChatGPT …） | **必须新写一套逆向客户端**（登录 + 协议 + PoW/SSE + UI 入口，数百行） | 无 PoW 站点：**一份站点 JSON**（选择器 + 登录 URL）；有 PoW/反爬的站点：以 `DeepSeekWebProvider` 为模板抄一个类 |

### 1.6 现状评级（可扩展性）

| 维度 | 评级 | 说明 |
|---|---|---|
| 加一家 **OpenAI 兼容 API** | 🟢 可配 | 无需编译，但体验差（手抄 + 无测试） |
| 加一家 **非兼容 API** | 🔴 需改代码 | 4 处硬编码协议假设 |
| 加一个 **网页版站点** | 🔴 需重写 | 单点实现，无适配器概念 |
| **UI / 校验 / 提示** 随厂商扩展 | 🔴 硬编码 | 「提供商」枚举、env 名、错误文案均写死 DeepSeek |
| **配置 / 密钥** 随厂商扩展 | 🔴 单节 | `[providers.deepseek]` 固定；ref 默认写死 |

> **结论**：今天的状态是「**OpenAI 兼容 + 手抄配置**」能用；离「设置任何 AI 的 web 或 API 都很容易」还有三层距离 —— 这正是 §2/§3 要补的。

---

## §2 目标形态

### 2.1 架构对比

**改造前**（今天）

```
ProviderConfig 节点参数（provider 枚举无用 / mode / api_base / model_custom / api_key / ref）
        │  provider 句柄（JSON）
        ▼
nodes/local_nodes.cpp ──if(mode=="web")──► ai::web_chat()      ← DeepSeek 专有，写死
                     └──else─────────────► ai::official_chat() ← OpenAI 兼容，写死
```

**改造后**（目标）

```
ProviderConfig 节点参数（provider=spec id / mode / api_base(可空→用默认) / model_custom / api_key / ref）
        │  provider 句柄（JSON：含 spec id + 生效地址 + 生效模型）
        ▼
engine/provider_resolve.cpp ──► EffectiveProvider + const ProviderSpec*（选择表 + 默认值 + 能力）
        ▼
nodes/local_nodes.cpp ──► ai::make_provider(id, mode, options)   ← 唯一分派点（工厂）
                                │
                                ├─ OpenAICompatibleProvider   （DeepSeek 官方 / 智谱 / 硅基流动 / Ollama / OpenRouter / Azure）
                                ├─ AnthropicProvider          （/v1/messages）
                                ├─ GeminiProvider             （:generateContent）
                                ├─ DeepSeekWebProvider        （现有 web_chat 收编；PoW + SSE）
                                └─ WebDomProvider             （L3：站点 JSON 驱动的通用 DOM 适配器）
                                        ▲
                    ai/web/site_spec + ~/.brain-ai/web_providers/*.json
```

### 2.2 三层能力模型（分层做，可独立交付）

| 层 | 名称 | 解决什么 | 加一家新 AI 的成本 | 估时 | 风险 |
|---|---|---|---|---|---|
| **L1** | **provider 描述表**（数据驱动） | 端点 / 认证 / 默认地址 / 候选模型 / Key 引用名 / env 名 从「写死」变「查表」 | OpenAI 兼容：**加 8 行数据，零逻辑代码** | 1–1.5 天 | 低（纯增量） |
| **L2** | **接口 + 工厂**（设计 §8.1 / PB-04） | 协议差异（Anthropic / Gemini / 未来新协议）收敛到一个 Provider 类；节点不再 if/else | 非兼容 API：**新增 1 个类（~150 行），节点零改动** | 2–3 天 | 低–中（需回归 `--web-chat`） |
| **L3** | **网页版适配器**（站点描述 + DOM 驱动） | 「任何 AI 的网页版」= 一份站点 JSON；有 PoW 的站点仍写类 | 无 PoW 站点：**一份 JSON（~15 行）**；有 PoW：抄 `DeepSeekWebProvider` | 5–10 天 | 中–高（站点改版 / WebView2 现场手测） |

> 三层**互相不依赖**：L1 可单独交付（立刻改善「OpenAI 兼容」体验）；L2 在 L1 之上把协议打开；L3 独立于 L1/L2（可最后做，或不做）。
> **推荐**：L1 → L2 一次做完（约 3–4.5 天，风险可控、收益完整）；L3 单独立项、按需推进。

### 2.3 不变量（Invariants，三层共同遵守）

| # | 不变量 | 验证方式 |
|---|---|---|
| I1 | 改造后**文本请求体与改造前逐字节一致** | 复用现有 `--exec-selftest` 请求体断言（**不改一行**必须仍 PASS） |
| I2 | `--web-chat` / `--web-probe` / `--web-session-selftest` 行为不变 | 三个自检命令 |
| I3 | Key 三级优先级与「填一次自动入库」「日志脱敏」不变 | `--cred-selftest` 8/8 + 既有断言 |
| I4 | 新增的每一条数据/协议映射都有**离线断言** | `--exec-selftest`（api_probe）/ `--provider-selftest`（aiwrite） |
| I5 | 任何「未实现的组合」都必须**明确报错 + 给操作步骤**，绝不静默失败 | 既有的 `unwired_reason` / VLM web 报错风格延续 |
| I6 | 不新增第三方依赖；不需要管理员权限；不写盘明文密钥 | 构建 + `--cred-selftest` |

---

## §3 任务分解

### L1 —— provider 描述表（数据驱动）

#### PB2-01 新增 `ai/provider_spec.{h,cpp}`（描述表 + 查询 + 默认值解析）

- **目标**：把「厂商元数据」集中到一张表，支持内置 + 用户覆盖。
- **内容**：
  - 内置条目（建议首版 8 条）：`deepseek`（official+web）、`zhipu`（智谱 GLM，含视觉）、`siliconflow`、`ollama`（本地，无 Key）、`openrouter`、`openai`、`custom-official`（自由填）、`deepseek-web`（kind=web）
  - 查询：`provider_specs()` / `find_provider_spec(id)` / `provider_ids(kind)`
  - 默认值解析：`resolve_api_base(spec, params)`（节点参数非空优先，否则 spec 默认）、`resolve_key_ref(spec, params)`、`resolve_env_name(spec)`
- **验收**：`VB2-01`（离线断言：内置条目数量/必填字段/查找命中与未命中/默认值优先级/`provider_ids` 过滤）

#### PB2-02 请求参数化：端点 / 认证 / 超时按 spec 取值

- **目标**：消除硬编码点 #2 #3 #6。
- **改动**：`ai/deepseek_official_provider.{h,cpp}` 增加 `ProviderOptions{api_base, chat_path, auth_style, env_name, extra_headers, timeout_connect_s, timeout_read_s}`；`build_endpoint(base, path)`；`build_auth_headers(auth_style, key)`；`resolve_api_key(param_key, env_name)`（env 名参数化，默认仍 `DEEPSEEK_API_KEY`）。
- **兼容**：旧签名保留为重载（默认值 = 今天的 DeepSeek 行为）→ I1 自动成立。
- **验收**：`VB2-02`（端点拼接 5 例：默认 / 带尾斜杠 / 带路径前缀 / Azure 风格 query / 空串回退；认证头 4 例：bearer / api-key / x-api-key+version / none；env 名参数化）

#### PB2-03 节点与 UI 改 spec 驱动

- **目标**：消除硬编码点 #1 #7 的「UI 侧」与 §1.3 的「装饰性 provider 字段」。
- **改动**：
  - `engine/node_registry.cpp`：`provider` 枚举 → 由 `provider_specs()` 生成；`api_base` 说明改为「留空 = 用该提供商的默认地址」；`model` 枚举 → spec 的候选模型（首项为默认）；`api_key_ref` 默认 → `spec.key_ref_default`
  - `engine/provider_resolve.cpp`：`EffectiveProvider` 增 `spec_id`（并让 `provider` 字段真正参与解析：`mode` 与 capability 由 spec 决定），`resolve_effective_provider` 用 spec 默认值补齐空字段
  - `ui/property_panel.cpp`：显示「生效：<spec 显示名> / <mode> / <模型>」+ 「该提供商：视觉 ✅/❌ · 原生 seed ✅/❌」能力徽标；切换 provider 时**自动带出**默认地址与 Key 引用名（**先压快照**，可撤销）
- **验收**：`VB2-03`（解析断言：spec 默认值补齐 / 节点参数覆盖 / 连线优先；UI 侧断言放在既有 `--graph-selftest`）

#### PB2-04 配置层多 provider

- **目标**：`config.toml` 由单节变多节 + 支持用户自定义 provider 文件。
- **改动**：`utils/config.h/cpp`：`Config::Provider` 保留（兼容旧文件），新增 `std::map<std::string, Provider> providers`；读写 `[providers.<id>]`；加载时**旧单节自动迁移**为新 map（`deepseek` 节原样搬家）；新增用户目录 `~/.brain-ai/providers/*.json`（可覆盖内置条目字段，只允许覆盖白名单字段）。
- **验收**：`VB2-04`（往返读写 / 旧配置迁移 / 用户覆盖生效 / 非法 JSON 报错不崩）

#### PB2-05 「测试连接」+ `--provider-selftest`

- **目标**：配置对不对，**一键可知**，不用跑整个工作流。
- **改动**：
  - CLI：`aiwrite.exe --provider-selftest [--provider <id>] [--api-base <url>] [--model <名>] [--key-ref <ref>] [--image <路径>] [--timeout N]`
    - **开关复用**：`--api-base / --model / --key-ref / --image / --timeout` **已存在**（`--vlm-selftest`、`--web-probe`、`--login-selftest` 在用），本项**只新增** `--provider` 与 `--provider-selftest` 两个开关，沿用现有解析风格（`src/main.cpp` 裸 `std::string` 比较）
    - ① 离线：spec 查找 / 端点 / 认证头 / 请求体（纯文本 + 多模态）断言 → 输出 `[Provider 自检] 离线断言 N/N PASS`
    - ② 联网（有 Key 时）：发一条 `ping`（提示词「请只回复 pong」）→ 打印 `HTTP / 模型 / 耗时`；退出码 0=全通过 / 1=失败 / 2=无 Key（仅离线部分通过）
  - UI：`ProviderConfig` 参数面板加「测试连接」按钮（异步、不阻塞界面；结果进 Console + 状态栏）
- **验收**：`VB2-05`（无 Key 环境下退出码 2 且离线断言全 PASS —— 与 `--vlm-selftest` 同风格）

### L2 —— 接口 + 工厂（落地设计 §8.1 / PB-04）

#### PB2-06 新增 `ai/inference_provider.h`（接口 + 值类型）

- **设计对齐**：命名与职责取自 `ai_writer_nodes.md` §8.1（`name` / `supportsVision` / `generate` / `generate_stream` / `generateWithImage`），并按现状（多图、seed、快照线程）做**最小必要扩展**：
  - `struct ProviderCaps { bool vision; bool stream; bool seed; bool system_role; }`（能力表 —— 取代节点里的 `if (mode == "web") throw`）
  - `struct GenerateParams`（system_prompt / prompt / images / temperature / max_tokens / top_p / seed / on_delta）
  - `struct GenerateResult`（ok / http_status / text / error / raw_head / elapsed_ms）
  - `class InferenceProvider { name(); caps(); generate(params, options); }`
  - **`generateStream`/`generateWithImage` 的取舍**：现状是「`on_delta` 回调 + `images` 字段」（PB-03 已暂停数据源）；本文档**不新开两个虚函数**，而是保留 `on_delta` 与 `images`，避免与 `M_patchA` 的流式计划冲突（记 `D-09`，见 §6）
- **验收**：编译期 + `VB2-06`（能力表：web 后端 `vision=false`；official `vision=true`；节点报错文案由能力表生成而非硬编码）

#### PB2-07 新增 `ai/provider_factory.{h,cpp}` + 现有实现收编

- `make_provider(const std::string& spec_id, const std::string& mode, const ProviderOptions&) -> std::unique_ptr<InferenceProvider>`
- `OpenAICompatibleProvider`：由现有 `ai::official_chat()` 逻辑搬迁（**行为不变**，含 error 分类、`stream=false`、超时默认值）
- `DeepSeekWebProvider`：由现有 `ai::web_chat()` 逻辑搬迁（**行为不变**，含 `on_delta`、PoW、SSE 解析）
- `main.cpp` 的 `--vlm-selftest` 继续走纯函数路径（不依赖工厂），保证离线自检不引入新耦合
- **验收**：`VB2-07`（工厂返回类型正确 / 未知 id 报可操作错误 / 旧 `official_chat()`/`web_chat()` 重载仍可用）；`--web-chat`、`--vlm-selftest`、`--exec-selftest` 全绿

#### PB2-08 `AnthropicProvider`（Messages API）

- 端点 `{api_base}/v1/messages`；认证 `x-api-key` + `anthropic-version: 2023-06-01`；`max_tokens` 必填；`system` 为**顶层字段**而非消息角色；多模态 `content[] = [{type:"text"},{type:"image", source:{type:"base64", media_type, data}}]`；响应取 `content[0].text`；`stop_reason`/`error.type` 映射到既有错误分类风格
- **验收**：`VB2-08`（离线：请求体结构 / 认证头 / 响应解析 / 错误映射；联网可选）

#### PB2-09 `GeminiProvider`（generateContent）

- 端点 `{api_base}/v1beta/models/{model}:generateContent?key=...`（或 `x-goog-api-key` 头，二选一并记录）；`contents[].parts[]`；`inline_data{mime_type,data}`；参数走 `generationConfig{temperature,topP,maxOutputTokens}`；响应取 `candidates[0].content.parts[0].text`
- **验收**：`VB2-09`（同上；含「model 名进 URL」的转义断言）

#### PB2-10 节点接线改能力驱动 + 校验/提示同步

- `nodes/local_nodes.cpp`：`if (mode != "web") {...} else {...}` → `make_provider(...)->generate(...)`；删除硬编码 `if (mode == "web") throw 图片理解暂不支持网页版`，改为 **caps 检查**：`if (!caps.vision) throw "<spec 显示名>（<mode>）不支持图片理解：请改用 <建议的视觉提供商>"`
- `engine/provider_resolve.cpp`：`unwired_reason` 的「缺 Key」文案改为**按 spec 生成**（env 名 / 引用名 / 默认地址都来自 spec）
- `engine/validate.cpp`：Key 校验的 env 名与引用名默认值改走 spec（消除第 6 项硬编码）
- **验收**：`VB2-10`（缺 Key 文案含正确 env 名 / VLM+web 文案改为能力驱动 / 既有 16 项 VLM 断言意图不丢）

### L3 —— 网页版适配器（站点描述 + DOM 驱动；**可选层**）

> 前置事实：`web/webview_host.cpp` 已经支持**在页面内执行任意 JS**（PoW 求解与协议探测就是这么做的，见 `webview_host.cpp:150-163`），因此「DOM 驱动型适配器」**不需要新的技术栈**。

#### PB2-11 站点描述 `ai/web_adapter.{h,cpp}` + `assets/web_sites/*.json`

- 数据结构（`site spec`，JSON；字段与含义逐条文档化）：

```json
{
  "id": "kimi",
  "display": "Kimi（月之暗面）",
  "login_url": "https://kimi.moonshot.cn/",
  "mode": "dom",
  "input_selector": "[contenteditable='true']",
  "send": { "kind": "key", "value": "Enter" },
  "answer_selector": ".markdown-body",
  "done_hint": { "kind": "button_state", "selector": "button[aria-label*='停止']" },
  "cookie_names": ["kimi-auth"],
  "token_expr": "localStorage.getItem('token')",
  "notes": "无 PoW；登录态来自 cookie"
}
```

- 实现：`WebDomAdapter`（实现 `InferenceProvider` 接口，`caps.vision=false`）
  1. 用 `LoginWindow`（已有）打开 `login_url`，用户在页面里登录 → 提取 `cookie_names` / `token_expr`
  2. 打开（或复用）会话页 → 注入 JS：写入 prompt → 触发 `send` → 轮询 `answer_selector` 的 `innerText`
  3. **增量策略**：轮询 N 次（默认 10 次 × 500 ms，全常量集中）→ 用 `on_delta` 发增量（沿用 PB-03 的 `RunEvent::Delta`）
  4. 结束判定：`done_hint` 命中或轮询次数用尽（**用尽时如实返回已取到的文本 + 明确警告**，不假装成功）
- **风险与护栏**：选择器失效 → 错误文案给出「选择器未命中，站点可能已改版；请更新 `<site>.json>`」，并把 **DOM 片段（脱敏、限长）**写入诊断日志（`utils/diagnostics.*` 已有落点）

#### PB2-12 用户自定义 provider 目录

- `~/.brain-ai/providers/<id>.json`（API 类覆盖：地址/路径/认证/模型候选）
- `~/.brain-ai/web_providers/<id>.json`（网页版站点：整份站点描述）
- 合并规则：**内置 → 用户覆盖**（字段级覆盖，只允许白名单字段）；冲突与非法 JSON 在 Console 给可操作错误，**不影响内置条目可用**
- UI：`ProviderConfig` 的「提供商」下拉分组显示「内置 / 用户自定义（N）」；用户目录不存在时给一行提示（文件路径可复制）

#### PB2-13 站点改版诊断 + `--web-adapter-selftest`

- 新增 `aiwrite.exe --web-adapter-selftest [--site <id>] [--timeout N]`：离屏登录窗口 → 打开站点 → 逐条检查「输入框选择器 / 发送方式 / 答案选择器」是否存在（**只做选择器可达性检查 + 打印命中元素摘要，不发送内容**）
- 退出码 0=全部可达 / 1=有选择器未命中 / 2=窗口或站点超时
- 用途：站点改版后**先跑这个**定位问题，而不是跑整个工作流

#### PB2-14 文档与索引同步

- `CHANGELOG`（新条目 + 索引）、`节点编辑器使用说明.md`（新增「接入一个新 AI：3 步」小节 + 测试连接用法）、`M_patchA.md` §4.1 `PB-04` 行状态改为 ✅ 并指向本文档、`DevPlan.todo`（新增/翻转对应条目）、`milestone_plan.md`（补丁系列行补 M_patchB）

---

## §4 阶段计划与验收

### 4.1 三批提交（每批独立可验证 / 可回滚）

| 批次 | 内容 | 提交信息（约定） | 预估 |
|---|---|---|---|
| **B2-a** | `PB2-01` → `PB2-05`（L1：描述表 + 参数化 + UI/配置 + 测试连接） | `feat(ai): M_patchB L1 provider 描述表（端点/认证/默认值数据驱动）+ 多 provider 配置 + --provider-selftest` | 1–1.5 天 |
| **B2-b** | `PB2-06` → `PB2-10`（L2：接口 + 工厂 + Anthropic/Gemini + 能力驱动接线） | `feat(ai): M_patchB L2 InferenceProvider 接口与工厂（OpenAI 兼容/Anthropic/Gemini/网页版收编）+ 能力驱动接线` | 2–3 天 |
| **B2-c** | `PB2-11` → `PB2-14`（L3：网页版适配器 + 用户目录 + 诊断 + 文档） | `feat(ai+web): M_patchB L3 网页版站点适配器（JSON 驱动 DOM 接入）+ --web-adapter-selftest` | 5–10 天 |
| **B2-d** | 文档收尾（可并入各批） | `docs(patchB): …` | 0.5 天 |

> `B2-c` 需现场手测（GUI + 真实站点），建议**单独排期**；`B2-a`/`B2-b` 可完全离线自检。

### 4.2 验收标准（AB2-*）

| 编号 | 标准 | 判定方式 |
|---|---|---|
| AB2-01 | **加一家新的 OpenAI 兼容服务**：只改 `ai/provider_spec.cpp` 里 ≤10 行数据，**不改任何逻辑代码**，界面下拉即可见、默认地址/模型/引用名自动带出 | 代码 diff + 手工确认 |
| AB2-02 | 「提供商」下拉**真正生效**（选 `zhipu` → 请求发往 `open.bigmodel.cn`，无需手填地址） | 离线请求体/端点断言 + 一次真实调用 |
| AB2-03 | 加一家非兼容 API：**只新增 1 个 Provider 类**，`nodes/**` 零改动 | 代码 diff |
| AB2-04 | 网页版新站接入 = **一份 JSON**（无 PoW 站点），选择器失效有明确报错与诊断日志 | `--web-adapter-selftest` + 手测 |
| AB2-05 | **不回退**：§0.5 全部基线保持（111/0、120/0（+新增）、七组 PASS、3-of-5、8/8、7/7、0 error 0 warning） | 逐条重跑并贴结果 |
| AB2-06 | **密钥零泄漏**：新代码不落盘明文、日志脱敏、`--cred-selftest` 8/8 不变 | 自检 + 代码审查 |
| AB2-07 | **请求体兼容**：既有 `--exec-selftest` 请求体断言**一行未改**仍 PASS | git diff + 自检 |
| AB2-08 | 每批提交前：`--provider-selftest` 离线断言全 PASS（无 Key → 退出码 2） | 自检输出 |

### 4.3 技术验证项（VB2-*）汇总

| 编号 | 内容 | 落在哪个自检 |
|---|---|---|
| VB2-01 | 描述表：条目完整性 / 查找 / 默认值优先级 / kind 过滤 | `--provider-selftest` |
| VB2-02 | 端点拼接 5 例 + 认证头 4 例 + env 名参数化 | `--exec-selftest`（api_probe，纯函数） |
| VB2-03 | spec 默认值补齐 / 节点参数覆盖 / 连线优先 | `--exec-selftest` + `--graph-selftest` |
| VB2-04 | 配置往返 / 旧单节迁移 / 用户覆盖 / 非法 JSON | `--exec-selftest` |
| VB2-05 | 测试连接（离线断言 + 无 Key 退出码 2） | `--provider-selftest` |
| VB2-06 | 能力表：`vision` 门控产生正确错误 | `--exec-selftest` |
| VB2-07 | 工厂：返回类型 / 未知 id 报错 / 旧函数重载仍在 | `--exec-selftest` |
| VB2-08 | Anthropic：请求体 / 头 / 解析 / 错误映射 | `--exec-selftest` |
| VB2-09 | Gemini：URL 转义 / `generationConfig` / 解析 | `--exec-selftest` |
| VB2-10 | 缺 Key 文案含 spec 的 env 名；VLM 文案能力驱动 | `--exec-selftest` |
| VB2-11 | 站点 JSON：解析 / 缺字段报错 / 用户覆盖合并 | `--provider-selftest` |
| VB2-12 | 站点选择器可达性（需 GUI/网络） | `--web-adapter-selftest` |

---

## §5 风险与对策

| # | 风险 | 影响 | 对策 |
|---|---|---|---|
| R1 | 改造打断现有请求体（回归） | 既有 `--exec-selftest` 120 项失败；线上行为变化 | 不变量 **I1**：旧签名保留为重载 + 默认值 = 今日 DeepSeek 行为；既有断言**一行不改**必须仍 PASS（AB2-07） |
| R2 | 工厂/接口改动波及网页版 | `--web-chat` 等自检失败、用户网页版不可用 | `web_chat` 只做**搬迁不做修改**（先加 wrapper 再切调用方，分两步提交）；不变量 **I2** |
| R3 | 「提供商」下拉生效后，老工作流语义变化 | 老 `.json` 工作流里 `provider="deepseek"` 语义从「无用」变「决定地址」 | **迁移规则**：`provider` 为空或未知 → 回退 `custom-official` + 用工作流里显式 `api_base`（老工作流行为**完全不变**）；E-01/E-02 等示例做回归加载断言 |
| R4 | 多 provider 配置迁移写坏用户 `config.toml` | 用户配置丢失 | 写盘前**备份 `config.toml.bak`**；迁移只在内存完成，落盘失败不覆盖原文件；`VB2-04` 断言迁移幂等 |
| R5 | 站点适配器让程序显得「时好时坏」 | 用户困惑 | 站点 JSON 标注「可能失效」；失败给**可操作**文案 + `--web-adapter-selftest` 一键诊断（PB2-13）；不用适配器时完全不影响既有网页版路径 |
| R6 | 站点反爬 / 合规风险 | 账号与法律风险 | 沿用既有策略：**有头登录、用户手动操作、不代填密码、不自动刷新会话**；适配器只做「用户已登录页面的 DOM 操作」，不注入脚本绕过验证 |
| R7 | 范围失控 | 拖住主线 | §0.3 非范围清单硬约束；L3 默认**不做**，需用户显式批准（§6） |
| R8 | 抽象过度 / 维护成本 | 长期负担 | 单文件职责清晰、纯函数优先；**新增代码预估 ≤ 1500 行**（L1+L2 约 700–900 行，L3 另计） |

---

## §6 待确认决策（请审核时逐条拍板）

| 编号 | 决策点 | 选项 | 我的建议 |
|---|---|---|---|
| **D-08** | 本补丁做到哪一层？ | ① 只做 L1 ② L1+L2 ③ L1+L2+L3 | **②（L1+L2）**：一次把「API 侧任意接入」打通；L3 单独立项 |
| **D-09** | `InferenceProvider` 接口形态 | ① 完全照设计 §8.1（`generate`/`generateStream`/`generateWithImage` 三虚函数） ② 按现状合并为 `generate(params)` + `on_delta` + `images` + `caps()` | **②**：与 PB-03 暂停流式的现状一致，避免虚函数空转；文档注明与 §8.1 的差异原因 |
| **D-10** | 非兼容协议先做哪家 | ① Anthropic ② Gemini ③ 都做 | **① Anthropic**（协议稳定、需求多）；Gemini 作为 `PB2-09` 可选 |
| **D-11** | 配置载体 | ① 只用 `config.toml [providers.*]` ② 只用 `~/.brain-ai/providers/*.json` ③ 两者都支持 | **③**（toml 便于手改、json 便于覆盖/分享） |
| **D-12** | 内置条目首版范围 | 最小集（deepseek/zhipu/ollama/custom）或完整集（+siliconflow/openrouter/openai/anthropic/gemini） | **完整集**（成本≈0，收益是可发现性） |
| **D-13** | 「测试连接」是否发真实请求 | ① 只做离线断言 ② 发一条最小请求（消耗极小额度） | **②**（提示词极短 + 明确标注会消耗额度） |
| **D-14** | 文档命名与编号 | 本文档叫 `M_patchB.md`，与 `M_patchA` 的 `PB-xx` 并存 | 保留 `M_patchB.md` + `PB2-` 前缀区分（§0.2）；若你更想叫 `M_provider.md`，我改文件名与索引 |

### 审核确认清单（勾选后我开工）

- [ ] **§6 D-08**：确定范围（L1 / L1+L2 / 全量）
- [ ] **§6 D-09…D-14**：确认或修改
- [ ] **§4.1**：批次划分与提交信息约定
- [ ] **§4.2 AB2-01…AB2-08**：验收标准是否够用
- [ ] **§3 任务清单**：是否增删（例如是否把 `config.timeout/error` 接线纳入本补丁 —— 默认**不纳入**，归 `FEA-M4-13`）

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
| 上期决策背景 | `docs/actionPlan/M_patchA.md` §4.1 / §12 | `PB-04` 登记项；**D-06/D-07**（优先 M5 / 视觉走智谱） |
| 设计依据 | `docs/ai_writer_nodes.md:550-582` | §8.1 抽象层 / §8.2 后端类型 / §8.3 后端选择 |
| 现有实测样例（视觉） | `source/workflows/examples/E-02_图片转小说.json`、`--vlm-selftest` | M5 已打通的「OpenAI 兼容 + 智谱 `glm-4v-flash`」路径（本补丁把它从「手抄」变成「下拉」） |

### 附录 B · 接口与数据草案（供审核）

```cpp
// ---- ai/provider_spec.h ----
namespace aiwrite::ai {
struct ProviderSpec {
    std::string id;                       // "zhipu"
    std::string display;                  // "智谱 GLM"
    std::string kind;                     // "official" | "web"
    std::string protocol;                 // "openai" | "anthropic" | "gemini" | "dom"
    std::string api_base;                 // 默认地址（空 = 必须手填）
    std::string chat_path;                // "/chat/completions" / "/v1/messages" / 站内路径
    std::string auth_style;               // "bearer" | "api-key" | "x-api-key" | "none"
    std::vector<std::pair<std::string, std::string>> extra_headers;   // 如 anthropic-version
    std::string env_name;                 // "ZHIPU_API_KEY"（可空）
    std::string key_ref_default;          // "brain-ai/zhipu"
    std::vector<std::string> models;      // 候选模型（首项 = 默认建议值）
    bool        vision      = false;
    bool        native_seed = false;
    bool        system_role = true;
    std::string docs_url;
};
const std::vector<ProviderSpec>& provider_specs();                // 内置 + 用户覆盖后的结果
const ProviderSpec*              find_provider_spec(const std::string& id);
std::vector<std::string>         provider_ids(const std::string& kind /*空 = 全部*/);
} // namespace aiwrite::ai

// ---- ai/inference_provider.h（D-09 建议形态）----
namespace aiwrite::ai {
struct ProviderCaps { bool vision = false; bool stream = false; bool seed = false; bool system_role = true; };
struct GenerateParams {
    std::string system_prompt, prompt;
    std::vector<std::string> images;                    // 本地路径（按序）
    double temperature = 0.7; int max_tokens = 2048; double top_p = 1.0; int seed = 0;
    std::function<void(const std::string&)> on_delta;   // 空 = 不需要增量
};
struct GenerateResult {
    bool ok = false; int http_status = 0;
    std::string text, error, raw_head;                  // raw_head = 诊断用前 N 字节（脱敏）
    double elapsed_ms = 0.0;
};
class InferenceProvider {
public:
    virtual ~InferenceProvider() = default;
    virtual std::string  name() const = 0;
    virtual ProviderCaps caps() const = 0;
    virtual GenerateResult generate(const GenerateParams& params, const ProviderOptions& options) = 0;
};
} // namespace aiwrite::ai

// ---- ai/provider_factory.h ----
namespace aiwrite::ai {
std::unique_ptr<InferenceProvider> make_provider(const std::string& spec_id,
                                                 const std::string& mode,   // official | web
                                                 const ProviderOptions& options,
                                                 std::string* error);       // 未知组合 → error 含操作步骤
} // namespace aiwrite::ai
```

**内置条目草案（10 条）**

| id | 显示名 | kind | protocol | api_base | 默认模型 | 视觉 | 备注 |
|---|---|---|---|---|---|---|---|
| `deepseek` | DeepSeek（官方 API） | official | openai | `https://api.deepseek.com` | `deepseek-chat` | ❌ | 无视觉模型（现状已提示） |
| `deepseek-web` | DeepSeek（网页版） | web | dom+pow | — | `default` | ❌ | 现有 PoW+SSE 路径收编 |
| `zhipu` | 智谱 GLM | official | openai | `https://open.bigmodel.cn/api/paas/v4` | `glm-4-flash` | ✅ | 视觉：`glm-4v-flash`（M5 实测后端） |
| `siliconflow` | 硅基流动 | official | openai | `https://api.siliconflow.cn/v1` | `Qwen/Qwen2.5-7B-Instruct` | ✅ | 视觉：`Qwen/Qwen2.5-VL-72B-Instruct` |
| `ollama` | 本地 Ollama | official | openai | `http://localhost:11434/v1` | `qwen2.5:7b` | ✅ | `auth_style = none` |
| `openrouter` | OpenRouter | official | openai | `https://openrouter.ai/api/v1` | `openai/gpt-4o-mini` | ✅ | — |
| `openai` | OpenAI | official | openai | `https://api.openai.com/v1` | `gpt-4o-mini` | ✅ | — |
| `anthropic` | Anthropic Claude | official | anthropic | `https://api.anthropic.com` | `claude-sonnet-4-5` | ✅ | `PB2-08` |
| `gemini` | Google Gemini | official | gemini | `https://generativelanguage.googleapis.com` | `gemini-2.0-flash` | ✅ | `PB2-09`（可选） |
| `custom-official` | 自定义（OpenAI 兼容） | official | openai | 空（必须手填） | 空 | ✅ | **老工作流迁移落点**（R3） |

> 表内模型名 / 地址以**实测可用**为准；若某条目在本机不可用，实现时降级为「仅作候选提示、可自由覆盖」，并在文档里如实标注（**不声称未验证的可用性**）。

---

## §8 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-09-26 | v1（草案） | 首版：现状审计（§1，逐条证据）+ 三层方案（§2）+ 任务分解 `PB2-01…PB2-14`（§3）+ 阶段与验收（§4）+ 风险（§5）+ 待确认决策 `D-08…D-14`（§6）+ 附录 A/B。**状态：待用户审核确认** |







