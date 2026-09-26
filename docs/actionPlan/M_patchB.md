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
> 状态：🟡 **待审核确认**（§6 为待确认决策清单；`D-11/D-12/D-15` 已按你的指示定稿，**web 与 API 同机制为硬性要求**）
> 版本目标：v0.5.x（在已收口的 M5 核心切片之上补「推理后端可插拔」地基）
> 预计工期（估）：**L1 ≈ 2–2.5 天 · L2 ≈ 2–3 天 · L3 ≈ 5–8 天**（全职估算，含自检与文档；L1 含**网页版去硬编码**，故高于纯 API 方案）

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

> **三层互相不依赖但语义连续**：L1 交付后「API 与网页版都已由表驱动、都可用户自定义」；L2 把协议打开（含把 DeepSeek 网页版实现收编为按表工作）；L3 把「任意站点」降级为纯数据。
> **推荐**：L1+L2 一次做完（约 4–5.5 天）——此时**API 与网页版两条路都已完成「表驱动 + 可扩展」**；L3 单独立项、按需推进。

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
  11. `kind` 与 `mode` 一致性：official 条目只能配 `mode=official`、web 条目只能配 `mode=web`（不一致 → 警告并按 `kind` 纠正）
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
  - `mode` 参数：可选项由**条目的 `kind`** 决定（official 条目 → 仅 `official`；web 条目 → 仅 `web`），不再无条件给 `{official, web}`
  - `api_base` 说明改为「**留空 = 用该提供商的默认地址**」；`model` 枚举 → 该条目的候选模型（首项为默认建议；`models` 为空则保持自由输入）
  - `api_key_ref` 默认值 → `key_ref_default`（换厂商不再串味）
- **生效解析**（`engine/provider_resolve.cpp`）：`EffectiveProvider` 增 `spec_id` / `kind` / `const ProviderSpec*`；空字段由表默认值补齐；「提供商」字段**真正参与解析**（不再是装饰性字段）；**web 条目自动把 `mode` 锁为 `web`**（避免表与节点参数冲突）
- **参数面板**（`ui/property_panel.cpp`）：
  - 显示「生效：<display> / <mode> / <模型>」+ 能力徽标（`视觉 ✅/❌ · seed ✅/❌ · 系统角色 ✅/❌`）
  - 切换提供商 → 自动带出默认地址 / Key 引用名 / 候选模型（**先压快照**，可撤销）
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

#### PB2-13 站点条目（`assets/providers.json` 内 `kind=web`）两种形态：`builtin:*` 与 `dom`

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

#### PB2-14 用户自定义站点（**与 API 完全同一套机制**）

- 用户可做（零代码）：
  1. **新增站点**：`~/.brain-ai/providers.d/kimi.json`（DOM 站点，见 PB2-13 形态二）
  2. **修站点（改版应急）**：`providers.json` 覆盖 `web.login_url` / `web.window_title` / `web.endpoints.*` / `web.probe_paths` / 选择器 —— **站点改版时用户自助修复，无需等程序更新**
  3. **改站点模型/能力**：覆盖 `models`（如网页版模式名变化时）与 `capabilities`
- 与 API 条目**共用**：同一份用户目录、同一套字段级 merge、同一套白名单校验、同一个「重新加载配置表」按钮、同一个 `--provider-selftest`
- 安全/合规护栏（沿用既有策略）：**有头登录、用户手动操作、不代填密码、不自动刷新会话**；不注入脚本绕过验证（验证码/风控不碰）
- **验收**：`VB2-12`（web 条目覆盖生效 / 新增 DOM 站点可见 / 非法选择器字段报错）

#### PB2-15 站点改版诊断 + `--web-adapter-selftest`

- `aiwrite.exe --web-adapter-selftest [--site <id>] [--timeout N]`：离屏打开站点 → 逐条检查「输入框 / 发送方式 / 答案选择器 / 登录态」是否可达（**只检查、不发送内容**）
- 退出码 0=全部可达 / 1=有未命中 / 2=窗口或站点超时；未命中时打印**选择器 + 命中的候选元素摘要**（便于用户改 JSON）
- 失败路径：错误文案给出「选择器未命中，站点可能已改版；请更新 `~/.brain-ai/providers.d/<id>.json`」，并把 DOM 片段（脱敏、限长）写诊断日志（`utils/diagnostics.*` 已有落点）

#### PB2-16 文档与索引同步

- `CHANGELOG`（新条目 + 索引）、`docs/节点编辑器使用说明.md`（新增「接入一个新 AI：3 步（写 JSON → 重载 → 测试连接）」）、`docs/README.md`、`actionPlan/M_patchA.md` §4.1 `PB-04` 状态 → ✅ 并指向本文档、`milestone_plan.md`、`DevPlan.todo`（登记/翻转条目）
- **`source/README.md`**：新增「配置文件位置」小节（`assets/providers.json` / `~/.brain-ai/providers.json` / `providers.d/`）

---

## §4 阶段计划与验收

### 4.1 三批提交（每批独立可验证 / 可回滚）

| 批次 | 内容 | 提交信息（约定） | 预估 |
|---|---|---|---|
| **B2-a** | `PB2-01` → `PB2-07`（L1：**JSON 配置表（official + web 两类同表）** + 加载/合并/校验 + 用户覆盖 + 打包 + API 参数化 + **网页版去硬编码** + 表驱动 UI + 测试连接） | `feat(ai+web): M_patchB L1 provider 配置表 JSON 化（API 与网页版同表 + 用户覆盖 + 校验/热重载）+ 网页版站点参数化 + --provider-selftest` | 2–2.5 天 |
| **B2-b** | `PB2-08` → `PB2-12`（L2：接口 + 工厂 + **DeepSeekWebProvider 收编** + Anthropic/Gemini + 能力驱动接线） | `feat(ai): M_patchB L2 InferenceProvider 接口与工厂（openai/anthropic/gemini/deepseek-web 按表分派）+ 能力驱动接线` | 2–3 天 |
| **B2-c** | `PB2-13` → `PB2-16`（L3：**DOM 站点执行器** + 选择器探测/诊断 + 用户自定义站点闭环 + 文档） | `feat(ai+web): M_patchB L3 通用 DOM 站点适配器（选择器 JSON 驱动）+ 选择器探测/诊断 + --web-adapter-selftest` | 5–8 天 |
| **B2-d** | 文档收尾（可并入各批） | `docs(patchB): …` | 0.5 天 |

> `B2-a` 与 `B2-b` 可**完全离线自检**；`B2-c` 需现场手测（GUI + 真实站点），建议**单独排期**。
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

### 4.3 技术验证项（VB2-*）汇总

| 编号 | 内容 | 落在哪个自检 |
|---|---|---|
| VB2-01 | **配置表加载/合并/校验**：必填缺失 / 类型错误 / 未知字段警告 / dup id 覆盖 / 疑似密钥拒绝 / 坏 JSON / `schema_version` 不匹配 / 兜底触发 | `--provider-selftest`（+ api_probe 纯函数断言） |
| VB2-02 | 查找顺序：exe/assets → 源码目录（开发态）→ 兜底；CMake 拷贝产物存在 | `--provider-selftest` + 构建 |
| VB2-03 | 用户覆盖：字段级 merge / `providers.d` 多文件顺序 / 新条目可见 / `replace_all` / 白名单拒绝 / **web 条目覆盖生效** | `--provider-selftest` |
| VB2-04 | 端点拼接 5 例 + 认证头 5 例 + env 名列表按序命中 | `--exec-selftest`（纯函数） |
| VB2-05 | 表驱动下拉与 **`mode` 由 `kind` 过滤** / 默认值补齐 / 节点参数覆盖 / 连线优先 / **改表中 `web.login_url` 后探测目标随之变化** | `--exec-selftest` + `--graph-selftest` |
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

### 审核确认清单（勾选后我开工）

- [ ] **§6 D-08**：确定范围（L1 / L1+L2 / 全量）
- [ ] **§6 D-09 / D-10 / D-13 / D-14 / D-16 / D-17 / D-18**：确认或修改（D-11 / D-12 / D-15 已按你的指示定稿；web 与 API 同机制为**硬性要求**）
- [ ] **§4.1**：批次划分与提交信息约定
- [ ] **§4.2 AB2-01…AB2-10**：验收标准是否够用
- [ ] **§7 附录 B 的 JSON 字段规范**：是否要增减字段（尤其 `capabilities` / `limits` / `web.*`）
- [ ] **§3 任务清单**：是否增删（如 `config.timeout/error` 接线默认**不纳入**，归 `FEA-M4-13`）

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
| `cookie_names` | array | ❌ | ❌ | 需要的 Cookie 名（**最小必要**，不全取） |
| `token_expr` | string | ❌ | ❌ | 在页面里求值的取 token 表达式（如 `localStorage.getItem('userToken')`） |
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

## §8 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-09-26 | v1（草案） | 首版：现状审计（§1，逐条证据）+ 三层方案（§2）+ 任务分解 + 阶段与验收（§4）+ 风险（§5）+ 待确认决策（§6）+ 附录 A/B |
| 2026-09-26 | v2 | **按用户第一条指示改写**：① 配置载体由「C++ 内置描述表」改为 **JSON 配置表**（`assets/providers.json`，随程序发布）+ **用户可自定义层**（`~/.brain-ai/providers.json`、`~/.brain-ai/providers.d/*.json`）；② 决策 `D-11`（JSON 表 + 实例参数分工）、`D-12`（完整内置集）、`D-15`（三层覆盖规则）**定稿**；③ 任务重编号为 `PB2-01…PB2-16`（L1 增加「加载/合并/校验」「打包与路径」「用户覆盖」「表驱动 UI + 管理入口」）；④ 新增验收 `AB2-09`（用户可配置闭环）、`AB2-10`（表坏不致命）与验证项 `VB2-01…VB2-14`；⑤ 新增风险 `R8…R11`（坏表 / 表与代码脱节 / 明文密钥 / 维护成本）；⑥ 新增不变量 `I7`（用户 JSON 即插即用）、`I8`（表坏不致命）；⑦ 新增附录 B（JSON 字段规范 + 合并示例 + 最小兜底表）与附录 C（加载顺序与生效规则）；⑧ **新增数据文件 `source/assets/providers.json`**（内置 10 条，随文档先落盘，`PB2-01/02` 让它真正被读取） |
| 2026-09-26 | **v3（当前）** | **按用户第二条指示改写（web 与 API 同机制）**：① 第一性原则新增「**API 与网页版同机制（同表 · 同规则 · 同入口）**」；② 范围把**网页版去硬编码**与**通用 DOM 适配器**列入在范围内（不再把 web 当可选层）；③ §2.1/§2.2 架构与三层模型改为「配置表承载 official + web 两类条目」；④ **`PB2-05` 扩写为「节点/UI/校验 + 网页版去硬编码」并给出逐处替换清单**（`webview_host` 登录 URL/窗口标题/探测路径、`property_panel` 登录入口、`deepseek_web_client` host 与端点、会话创建/拉取路径）；⑤ `PB2-01` 增两类条目校验规则（`web.adapter` 分支校验、`_` 前缀键忽略、轮询上限、`kind`↔`mode` 一致性）；⑥ `PB2-03`/`PB2-13`/`PB2-14` 明确「web 条目同待遇」，站点条目分 `builtin:*` 与 `dom` 两种形态；⑦ `PB2-07` 自检增**网页版路径**（只查登录态与端点一致性，不发内容）；⑧ 新增不变量 `I9`（网页版无硬编码）、`I10`（两类同待遇）、验收 `AB2-11`/`AB2-12`、验证 `VB2-15`、风险 `R12`/`R13`、决策 `D-18`；⑨ 附录 B 的 `web` 字段表改为「两种形态」并说明 `_` 前缀键语义；⑩ **`source/assets/providers.json` 的 `deepseek-web` 条目补齐** `adapter` / `window_title` / `endpoints` / `probe_paths` / `models`，并新增顶层 `_example_web_dom` 模板 |

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
