# M_Preset_Library：官方预设节点库设计文档

> 类型：**架构设计文档**（草稿 v1.3 · 存放 `docs/draft/`，成熟后归位 `docs/actionPlan/` 并执行五处登记）
> 定位：基于现有 **14 个基础节点**构建上层**官方预设节点库（Composite Node 预设）**与**推荐工作流**的完整设计 —— 文档驱动，先成熟再实施。
> 上游依据：开发者 2026-10-16 ~ 2026-10-18 拍板（D1 / D3 / D4 / D7 / D8 / D10 + PSL-1~5 修订版 D）· [Composition_node.md](../Archive/actionPlan/Composition_node.md)（CN-D1~D20 全量采纳，含 `CN-D17` 禁嵌套）
> 编号空间：`PS-*`（任务 `PS-0x`）· `PS-D*`（决策）· `PSL-*`（预设设计约定）· `PS-P*`（预设）· `PS-WF*`（工作流）· `PS-V*`（验证断言）· `PS-AC*`（人工验收）· `PS-Q*`（待拍板）
> 状态：🟡 **草稿 v1.3（2026-10-18 · PS-D10：全预设零 AI 调用 · 输出提示词 · AI 由用户外部配置）· 待逐章过审**
> 相关：[Composition_node.md](../Archive/actionPlan/Composition_node.md) · [ai_writer_nodes.md](../ai_writer_nodes.md) · [Creation_Roadmap_Guideline.md](../actionPlan/Creation_Roadmap_Guideline.md) · [DocMAP.md](../DocMAP.md) · schema 附录 [PS_schema_reference.md](PS_schema_reference.md)
> 建立：**2026-10-18**（v1 草稿 · 零代码）

---

## 目录

- [§0 文档元信息与约定](#§0-文档元信息与约定)
- [§1 结论摘要（PS-D1~D8）](#§1-结论摘要)
- [§2 工作机制](#§2-工作机制)
- [§3 预设设计约定（PSL-1~5 修订版）](#§3-预设设计约定)
- [§4 P1~P10 预设逐项细节](#§4-p1p10-预设逐项细节)
- [§5 推荐工作流 WF-1/2/3](#§5-推荐工作流)
- [§6 验收框架](#§6-验收框架)
- [§7 UI 与上手体验](#§7-ui-与上手体验)
- [§8 待拍板清单](#§8-待拍板清单)
- [§9 实施拆解](#§9-实施拆解)
- [§10 变更记录](#§10-变更记录)

---

## §0 文档元信息与约定

### 0.1 定位

- **本文是官方预设节点库的设计依据**：定义「官方预设（Composite Node 预设）＝ 用 14 个基础节点组合出的、可开箱即用的迷你工作流，随应用内置、只读、可一键复制到用户节点库」。**统一形态（`PS-D10`）**：每个预设内部**零 AI 调用节点**，只把素材/输入**装配成一段可直接交给生成节点的提示词**（唯一 `out1` 文本输出）；AI 生成由用户在外部工作流放置 `LLMGenerate`/`VLMGenerate` + `ProviderConfig` 最终配置。
- **本文是 [Composition_node.md](../Archive/actionPlan/Composition_node.md) 的组合层落地承接**：CN 已交付「上级节点」的**机制**（规格 / 校验 / 注册表 / 展开执行 / UI），本文交付「机制之上**第一批官方内容**」—— 9 个预设（P1~P9 + 待拍板备选 P10）+ 3 个推荐工作流。
- **不改他文**：本文对 `Composition_node.md` / `ai_writer_nodes.md` / `Creation_Roadmap_Guideline.md` / `M_node_expansion*.md` 只**单向引用**。

### 0.2 本文要解决的那一个问题

> 「用户打开软件后，如何**零配置、三步内**跑通小说创作的一条典型链路？」—— 答案 = 官方预设节点库：把常见创作原子操作预制成「官方·」节点，拖入即用；把完整链路预制成推荐工作流，一键载入。

### 0.3 执行原则

1. **组合优于新增**：能靠 14 个基础节点组合出来的能力，**不新增任何基础节点**（本文零新基础节点）。
2. **文档驱动**：§4 细节可逐字实现、§6 断言编号完整、§8 归零或明确"下一批"之后才启动代码。
3. **内置只读 + 用户可复制**（`PS-D1` 方案 A）：官方资产永不写回，用户拥有全部控制权。
4. **统一装配原则（`PS-D10` · 2026-10-18 用户拍板）**：**所有预设内部一律不内置 LLM/VLM 调用与 Provider 配置**（零 AI 节点），统一输出「可直接交给生成节点的提示词文本」；AI 生成由用户在外部工作流中放置 `LLMGenerate`/`VLMGenerate` + `ProviderConfig` 并**最终配置**后执行。P1~P4/P8（及备选 P10）由原「生成类（内置 ProviderConfig）」全部转为「提示词装配类」。

### 0.4 本轮范围

| 做（设计文档） | 不做（本批） |
|---|---|
| 9 预设完整规格（P1~P9 + 待拍板备选 P10 · 内部图 / 端口 / 暴露参数 / 默认模板 / 验证断言）· 3 推荐工作流 · 验收框架 · UI 设计 · 实施拆解 | 任何 `source/**` 代码改动 · `source/workflows/compositeNodes/*.json` 实体文件 · 正式五处登记（待归位后） |

### 0.5 术语约定

- 基础节点统一「代号 + 说明」形式（如 `N-03 PromptTemplate 提示词模板`），权威 schema 见 [PS_schema_reference.md](PS_schema_reference.md)。
- **预设（Preset）** = 内置的、只读的 Composite Node 规格（资产 JSON）。
- **资产（Asset）** = `source/workflows/compositeNodes/*.json` 单文件。
- **实例（Instance）** = 用户把预设拖入画布后创建的 Composite 节点（规格值独立）。
- **暴露面（Exposed Surface）** = `inputs` + `outputs` + `exposed_params`。

### 0.6 已取证的事实基座（本稿全部细节的依据）

| 事实 | 出处 |
|---|---|
| 14 节点权威 schema（端口 / 参数 id / 默认值 / 枚举 / 范围） | `source/engine/src/node_registry.cpp`（行号见 [PS_schema_reference.md](PS_schema_reference.md)） |
| 资产 JSON schema = `composite_to_json` 输出：`schema_version=1` + `id/name/description/inputs/outputs/exposed_params/internal_graph` | `source/engine/src/composite.cpp:124-161` · `kCompositeSchemaVersion=1`（`composite.cpp:16`） |
| `internal_graph` = `graph_to_json` 输出（`version/name/nodes/edges/viewport`，节点含 `id/type/title/position/params`） | `source/engine/src/workflow_io.cpp:43` |
| 硬约束：`validate_composite_spec` 拒绝「标记输入端口已有内部入边」 | `composite_selftest.cpp:142-154` |
| `PromptTemplate.vars` 变长多值只有 `{1}/{2}…`，**单值才提供 `{vars}`**；未填充占位符报错（PW-10 / PQ-13） | `local_nodes.cpp:428-478` |
| `TextMerge.texts` 递归展平数组（TextSplit 直连友好） | `local_nodes.cpp:493-509` |
| `text_of`：非字符串（含数组）一律 JSON dump | `text_process_common.h:24-33` |
| TextSplit 输出 `texts` = JSON 数组；空输入 → 空数组 | `text_split.cpp:115-153` |
| TextReplace 空 pattern = 不改；TextClean 默认（trim/去控制字符开） | `text_replace.cpp:163-165` · `text_clean.cpp:120-126` |
| TextDiff 无差异 → `（无差异）`；TextStats `output_mode=number` 走 number 端口 | `text_diff.cpp:197` · `text_stats.cpp:170-172` |
| 双源现状（`PS-D9` 目标）：`CompositeRegistry` 现只扫 `~/.brain-ai/composites/`；实施后扫内置 `source/workflows/compositeNodes/` + 用户 `~/.brain-ai/workflows/compositeNodes/` | `composite.cpp:500-560` · `composite.h:81-114` · `paths.cpp:84-88` |

---

## §1 结论摘要

**官方预设节点库 = 随应用内置的 9 个只读 Composite 预设（P1~P9 + 待拍板备选 P10）+ 3 个推荐工作流（WF-1/2/3），基于 14 个基础节点组合而成，拖入即用、可一键复制到用户节点库。**

### 1.1 决策记录（PS-D1~D8）

| 编号 | 议题 | 结论 | 依据 |
|---|---|---|---|
| `PS-D1` | 预设形态 | **方案 A**：内置只读资产 `source/workflows/compositeNodes/*.json`（随仓库分发）+ registry **双源扫描** + 「复制到我的节点库」按钮 | 开发者拍板（2026-10-16 · 存储地点修订 `PS-D9`） |
| `PS-D2` | 推荐工作流 | 三件套草案成立：`WF-1` 续写入门 / `WF-2` 图片转小说 / `WF-3` 全链路演示（P1→P3→外部 LLM 续写）；落地为 `source/workflows/examples/` 示例 JSON（§5） | 开发者拍板（草案 · 范围细节 `PS-Q3`） |
| `PS-D3` | 预设数量与范围 | 9 个预设**全进文档**（P1~P9）+ 待拍板备选 P10，文档成熟度优先 | 开发者拍板（2026-10-16；2026-10-18 增 P9/P10） |
| `PS-D4` | 资产加载链 | 资产 JSON → `composite_from_json` → `validate_composite_spec`（严格）→ registry 加载；单文件损坏仅告警跳过（沿用现有行为） | 派生结论（2026-10-16） |
| `PS-D5` | 验收框架 | **不新建正式 `VCN-*`/`ACN-*` 框架**（CN §14 待细化项 8）；改为扩展 `composite_selftest` 的「预设库」子段（离线断言 `PS-V*`）+ 用户人工验收 `PS-AC*` 清单 | 派生结论（2026-10-16） |
| `PS-D6` | 首启引导 | 首次启动展示「官方预设」面板：9 预设 + 3 工作流卡片，分「提示词装配类 / 文本处理类 / 工作流」三组；范围与分组见 §7 | 草案（范围细节 `PS-Q3`） |
| `PS-D7` | 落档 | 新建**本文**（成熟后归位 `docs/actionPlan/M_preset_library.md`），编号空间 `PS-*`；五处登记（`AI_README §5` · `DocMAP` · `CHANGELOG` · `变量术语对照表 §11.2` · `DevPlan.todo`）待归位后执行 | 开发者拍板（2026-10-16） |
| `PS-D8` | 实操验收 | 并行实操验收：离线 `composite_selftest` 扩展（PS-V*）+ 用户人工验收（PS-AC*），顺带完成 CN 人工验收 | 开发者拍板（2026-10-16） |
| `PS-D9` | 存储地点（双源映射） | 内置源 → `source/workflows/compositeNodes/`（仓库内 · 只读）；用户源 → `~/.brain-ai/workflows/compositeNodes/`（用户工作流同级）；旧 `~/.brain-ai/composites/` 首次 reload 时并入用户源 | 开发者拍板（2026-10-18 · 与用户讨论） |
| `PS-D10` | **预设零 AI 调用（统一装配原则）** | **所有预设（上级节点）内部一律不含 `LLMGenerate`/`VLMGenerate`/`ProviderConfig` 调用节点**；预设只把素材/输入装配成「可直接交给生成节点的提示词」，唯一 `out1`(text) 输出；AI 生成由用户在**外部工作流**放置 `LLMGenerate`/`VLMGenerate` + `ProviderConfig` 并最终配置 | 开发者拍板（2026-10-18 · 用户：「所有上级节点都不默认配置 AI，都需要用户在外部最终配置」） |

### 1.2 预设设计约定（PSL-1~5 修订版 D，详见 §3）

| 编号 | 约定 | 要点 |
|---|---|---|
| `PSL-1` | 自包含（修订版 D） | 仅 `P3` / `P4` 带**文本输入端口** `in1`（接上游提示词/素材文本，如 P1/P2 输出）；其余预设**无输入端口、完全自包含**；**全部预设内部零 AI 调用节点**（`PS-D10`），输出 = 可直接交给生成节点的提示词 |
| `PSL-2` | 单输出 | 所有预设**强制单输出** `out1`（Text），符合 `CN-D7` |
| `PSL-3` | 最小暴露 | 只暴露用户**必须改**的参数（内容类 `TextInput.text` / `PromptTemplate.template` / `TextOutput.label` 必暴露；结构类分隔符按需暴露）；内部节点其余参数全部用内置默认值 |
| `PSL-4` | 零 Provider | **全部预设不内置 `ProviderConfig`、不调用任何 AI 节点**（`PS-D10`）；AI 由用户在外部工作流放置 `LLMGenerate`/`VLMGenerate` + `ProviderConfig` 并最终配置 |
| `PSL-5` | 命名 | 资产文件 `ps_p1.json`…`ps_p9.json`（+ 备选 `ps_p10.json`），规格 `id = "ps_p1"…`；显示名前缀「**官方·**」（如「官方·前提句生成」）；内部节点 id `n1`…`nN` |

### 1.3 架构位置

```
【官方预设层】  ← 本文：9 预设（只读资产）+ 备选 P10 + 3 推荐工作流
      ↑ 组合（Composite Node · CN-D20 L4）
【组合节点层】  ← Composition_node.md：上级节点机制（规格/校验/注册表/展开/UI）
      ↑ 组合
【基础节点层】  ← 现有 14（MVP 8 + L2 6）
```

### 1.4 与上游文档的关系

- 采 [Composition_node.md](../Archive/actionPlan/Composition_node.md) 全部 `CN-D1`~`D20`：禁嵌套（`CN-D16` 校验拒绝，预设资产天然满足）· 强制单输出（`CN-D7`）· 参数选择性暴露（`CN-D6`）· 展开执行（`CN-D9`）。
- 补 CN §14 待细化项 10「**程序化生成上级节点的规则**」：**本文 §4 即"程序化生成的权威内容源"**——资产 JSON 由文档规格生成（实施批 `PS-01`），`composite_from_json` 消费。
- 对应 [Creation_Roadmap_Guideline.md](../actionPlan/Creation_Roadmap_Guideline.md) 创作阶段：P1/P2/P3（构思 0-40）· P4/P8（草稿 50-75）· P5/P6/P7（修订 75-90）· **P9（装配期 · 任意阶段前，供 P1~P4/P8 喂料）** · P10（P2 图片版提示词 · 构思 20-30 · 待拍板）。

---

## §2 工作机制

### 2.1 数据流总览

```
source/workflows/compositeNodes/ps_p1.json … ps_p9.json（+ 备选 ps_p10.json · 待拍板）  （内置 · 只读 · 随仓库分发）
        │  CompositeRegistry::reload() 双源扫描
        ▼
composite_from_json → validate_composite_spec（严格）→ 注册表内存（id/name 排序）
        │                                                    ▲
        │ build_composite_node()                             │ 复制（save 到用户源）
        ▼                                                    │
画布实例（Composite 节点）              ~/.brain-ai/workflows/compositeNodes/ps_pX.json
        │                                                     （用户源 · 可读写）
        ▼
Executor: expand_composites() 工作副本就地展开 → 按帧推进
```

### 2.2 双源扫描规则（`PS-D1` 核心）

| 项 | 内置源（builtin） | 用户源（user） |
|---|---|---|
| 目录 | `source/workflows/compositeNodes/`（仓库内 · 只读 · 随分发） | `~/.brain-ai/workflows/compositeNodes/`（用户工作流同级） |
| 读写 | **只读**（`save/remove` 拒绝） | 可读写（现状不变） |
| 加载 | 首次 `list()/find()` 惰性扫描 + 显式 `reload()` | 同左（现有行为） |
| id 冲突 | **用户源覆盖内置源**（后扫覆盖） | — |
| 损坏文件 | 告警跳过（沿用现有 `composite.cpp:515` 行为） | 同左 |
| 来源标记 | registry 内部维护 `spec id → source` 映射（内存态，不写入 JSON） | 同左 |

- **来源映射不进规格 JSON**：`CompositeSpec` 结构不加字段（避免破坏 `composite_to_json/from_json` 往返兼容）；registry 在扫描时记录 `std::unordered_map<std::string, Source>` 供 UI 查询。
- **id 冲突语义**：复制动作以同 id 落盘用户源 ⇒ 用户「接管」该预设（官方徽标消失、可编辑）。删除用户副本 ⇒ 恢复内置源。**简单、可预期**。
- **复制动作用户源可覆盖官方资产**：这是有意为之（用户拥有最终控制权）；UI 上复制前弹确认文案「将复制一份到我的节点库，之后可编辑」。
- **旧目录迁移**：首次 `reload()` 把 `~/.brain-ai/composites/` 遗留文件并入用户源（复制后删除原文件），用户节点不丢失（`PS-D9`）。

### 2.3 资产 JSON 文件结构（`composite_to_json` schema，逐字段）

```jsonc
{
  "schema_version": 1,
  "id": "ps_p1",                       // 预设 id（PSL-5）
  "name": "官方·前提句生成",             // 显示名（新建实例默认标题）
  "description": "…",
  "inputs": [                          // CompositePortRef 数组（仅 P3/P4 有；internal_node = 内部 TextMerge）
    {
      "internal_node": "n1",           // 内部节点 id（internal_graph 内 · P3/P4 = n1 TextMerge）
      "internal_port": "texts",        // 内部端口 id（TextMerge.texts · 无内部入边）
      "display_name": "故事设定",       // 外部显示名
      "type": "text",                  // portTypeName 输出（小写）
      "is_optional": true
    }
  ],
  "outputs": [                         // 强制 1 项（CN-D7）
    { "internal_node": "n3", "internal_port": "text", "display_name": "结果", "type": "text" }
  ],
  "exposed_params": [                  // CompositeParamRef 数组
    { "internal_node": "n2", "param_id": "template", "display_name": "提示词模板" }
  ],
  "internal_graph": {                  // graph_to_json 输出
    "version": "1.0", "name": "", "nodes": [ … ], "edges": [ … ], "viewport": { … }
  }
}
```

> 完整示例见 §4.1（P1 全量 JSON 骨架，可逐字照抄）。`type` 字段取值：`text` / `image` / `provider` / `number` / `any`（`portTypeName` 小写）。

### 2.4 加载与实例化时序

1. **启动**：UI 首次打开节点库「上级节点」分类 → `CompositeRegistry::list()` 触发惰性扫描（双源）→ 内置 9 预设进列表（带官方徽标）。
2. **拖入**：`canvas_add_composite_at_center(spec->id)`（现有）→ `build_composite_node` 依据规格建实例（端口 in1…/out1、暴露参数取内部默认值，值独立）。
3. **运行**：`executor` 在**工作副本**上 `expand_composites()` 就地展开（`<外节点id>__<内部id>` 前缀）；P3/P4 的 `in1` 外部边重接到内部 `m1.texts`。
4. **复制**：点「复制到我的节点库」→ registry 以同 id `save` 到用户源 → 内存来源标记切为 user → 徽标消失、可编辑。
5. **卸载/删除**：用户源项走现有 `remove()`；内置源项无删除入口（只读）。

### 2.5 不改动的既有行为

- 内部图**禁止嵌套 Composite**（`CN-D16`）：预设资产由 14 基础节点组成，天然满足。
- `allow_incomplete_draft` 仅用于「新建上级节点」草稿；**预设资产加载走严格校验**（`PS-D4`），不允许空图/未标记输出。
## §3 预设设计约定

### 3.1 PSL-1~5 细则

| 约定 | 细则 |
|---|---|
| `PSL-1` 自包含（修订版 D） | ① **全部预设内部零 AI 调用节点**（`PS-D10`：无 `LLMGenerate`/`VLMGenerate`/`ProviderConfig` 接线），统一输出「可直接交给生成节点的提示词」文本。② 除 `P3`/`P4` 外，所有预设**无 `inputs`**（零输入端口，拖入即跑；`P9` 的素材输入 = 内部暴露 `TextInput`，见 §4.9）。③ `P3`/`P4` 各带 **1 个可选文本输入端口 `in1`**（`is_optional=true`），用于接上游提示词/素材文本（如 `P1.out1` → `P3.in1`）。④ ⚠️ **「可选输入未接」受引擎运行前校验约束**：展开后必填变长端口无入边 → 拦截「至少需要 1 条连线」（`executor.cpp:285-294` · `validate.cpp:127`）⇒ **当前引擎下 `P3`/`P4` 的 `in1` 必须接线**，「未接兜底」依赖引擎扩展 `PS-Q8`（详见 §3.3）。⑤ 输入端口上限 3（`P9` 未来增强预留）。 |
| `PSL-2` 单输出 | `outputs` 恰 1 项，`type = "text"`，外部端口 id 固定 `out1`；内部引用一律指向「最终文本承载节点」（TextOutput 或 PromptTemplate）的 `text` 端口。 |
| `PSL-3` 最小暴露 | 暴露参数 = 仅「用户必须改」；判据：① 内容类（TextInput.text / ImageInput.path / PromptTemplate.template / TextOutput.label）必暴露；② 结构类（分隔符等）仅当「用户大概率想改」才暴露（如 P3/P4 的 `n1.separator`、P6 的 `diff_mode`）；③ 其余一律内部默认。注：预设已无 AI 节点 ⇒ 采样参数（temperature 等）不再出现于预设内部。 |
| `PSL-4` 零 Provider（`PS-D10`） | **全部预设（P1~P9 + P10）不内置 `ProviderConfig`、不调用任何 AI 节点**；AI 由用户在外部工作流放置 `LLMGenerate`/`VLMGenerate` + `ProviderConfig` 并最终配置（provider / mode / model_custom / api_key 均跟随外部配置表 + 凭据管理器）。 |
| `PSL-5` 命名 | 文件 `ps_p{n}.json` · id `ps_p{n}` · `name = "官方·" + 中文名`。内部节点 id：`n1`…`nN`（P1/P2/P8/P10 = 3 节点；P3/P4 = 5 节点；P9 = 6 节点）。展示前缀「官方·」同时用于节点库列表与实例默认标题。 |

### 3.2 提示词装配类预设的通用内部拓扑（全部预设基线 · 零 AI 节点）

```
（P1/P2 基线 · 纯文本模板装配）
TextInput ─text→ PromptTemplate ─text→ TextOutput
                        vars          text
out1 ← TextOutput.text

（P3/P4 基线 · 带 in1，两级 TextMerge 合并素材 —— 设计依据见 §4.3「硬约束与绕行」）
in1 ──→ m1.TextMerge.texts（标记端口 · 无内部入边）
m1.text ─┐
         ├→ m2.TextMerge.texts → PromptTemplate.vars → TextOutput
n1.TextInput.text ─┘
out1 ← TextOutput.text

（P8/P10 基线 · 图片场景提示词装配 —— 预设零 AI，图片由外部 VLMGenerate 处理）
TextInput ─text→ PromptTemplate ─text→ TextOutput
                        vars          text
out1 ← TextOutput.text

（P9 基线 · 提示词整理器 —— 无输入端口 · 三素材框 + 合并 + 模板装配）
n1.TextInput ─text─┐
n2.TextInput ─text─┼→ m.TextMerge.texts → PromptTemplate.vars → TextOutput → out1
n3.TextInput ─text─┘    text → prompt      text → text
```

> **PS-D10 统一语义**：上述拓扑**均不含 AI 节点**。运行预设得到的是「可直接交给生成节点的提示词文本」；把它接入外部 `LLMGenerate.prompt`（文本场景）或 `VLMGenerate.prompt`（图片场景，图片另接 `ImageInput → VLMGenerate.image`）+ `ProviderConfig → VLM/LLMGenerate.provider` 即完成生成。

### 3.3 为什么 P3/P4 必须用「两级 TextMerge」（关键设计推导）

- **硬约束**：`validate_composite_spec` 拒绝「标记输入端口已有内部入边」（`composite_selftest.cpp:142-154`），防止展开后外部边 + 内部边重复占用同一输入端口。
- **单级方案不成立**：若 `in1 → TextMerge.texts`（标记），则内部 TextInput **不能**再连同一 `texts`（违反硬约束）；若 `in1 → PromptTemplate.vars`（标记），则内部 TextInput 也不能连 `vars`。任何「单个合并点同时接受外部标记 + 内部入边」都违反约束。
- **两级方案成立**：`in1 → m1.texts`（m1.texts 无内部入边 ✓）；`m1.text → m2.texts` 与 `n1.text → m2.texts` 两条内部边都落在 **m2**（m2 **不是**标记端口，变长端口多入边合法 ✓）；`m2.text → vars` 是**单值** → 模板可用 `{vars}`（而非被迫用 `{1}/{2}`）✓。
- **行为验证（执行层）**：in1 已接 → `m2 = [外部, 兜底]`（`ignore_empty=true` 下兜底空串被忽略）；`{vars}` 均有值，**不会触发 PW-10 未填充报错**。
- **⚠️ 运行前校验事实（2026-10-18 取证 · 关键认知修正）**：两级 TextMerge **只绕过了规格校验**（`validate_composite_spec` 的「标记端口无内部入边」），**没有绕过运行前校验** —— `Executor::start()` 先 `expand_composites()` 再 `validateBeforeRun()`（`executor.cpp:285-294`），`m1.texts` 是**必填变长**端口，`in1` 未接时无入边 → 报「`[x__m1]` 可变长输入「文本」至少需要 1 条连线」（`validate.cpp:127`）→ 运行**直接失败**；`composite.cpp:280` 注释明确「暴露输入恒可选：**是否必接由内部端口决定（展开后校验）**」；`composite_selftest.cpp:271-275` 展开用例仅覆盖 in1 已接。⇒ **当前引擎下 P3/P4 的 `in1` 必须接线**；「未接兜底」依赖引擎扩展 **`PS-Q8`**（可选标记端口未接 → 展开后豁免对应内部端口的必填校验）。
- **备选（待拍板 `PS-Q1`）**：若后续引擎放宽「变长端口允许外部标记 + 内部入边共存（合并语义）」，P3/P4 可简化为**单级** TextMerge（in1 与 n1 同一 merge）——本文按当前机制（两级）为**主案**，简化仅作记录。

### 3.4 文本处理与提示词装配类预设的拓扑（全部预设 · 零 AI）

```
P1/P2/P8/P10: TextInput → PromptTemplate → TextOutput        （装配提示词）
P3/P4:        in1(+TextInput) → 两级 TextMerge → PromptTemplate → TextOutput（装配提示词）
P5:           TextInput → TextClean → TextReplace → TextOutput（文本处理）
P6:           TextInput(a) ─┐
                             ├→ TextDiff → TextOutput        （文本处理）
              TextInput(b) ─┘
P7:           TextInput → TextStats → TextOutput              （文本处理）
P9:           3×TextInput → TextMerge → PromptTemplate → TextOutput（装配提示词）
—— 全部预设零 AI 节点（PS-D10），运行结果均为纯文本。
```

### 3.5 预设 → 资产 JSON 的字段映射约定（实施批 `PS-01` 用）

| 规格字段 | 来源 |
|---|---|
| `id/name/description` | §4 各预设表 |
| `inputs` | 仅 P3/P4（`internal_node="n1"`, `internal_port="texts"`）；其余空数组（P9 素材输入 = 内部暴露 TextInput，无端口；P8/P10 图片走外部 VLM，预设零 AI，见 §4.8/§4.10） |
| `outputs` | 1 项（内部末节点 `text` 端口；`display_name` = 「结果」） |
| `exposed_params` | §4 各预设「暴露参数」表（`internal_node` + `param_id`；`display_name` 空 = 用内部显示名） |
| `internal_graph.nodes` | `id/type/title/position/params`；params 只写**非默认**值（与 E-02 示例同风格），默认值不必写全 |
## §4 P1~P10 预设逐项细节

> **阅读约定**：内部节点 `title` 即默认显示名；参数表只列**非默认**值（资产 JSON 按 §3.5 只写非默认）；`position` 采用画布纵排布局（x 递增，y 错开），实施时可微调。每个预设给出：① 用途与端口 ② 内部图 ③ 内部参数默认值 ④ 边表 ⑤ 暴露参数 ⑥ 验证断言编号（§6 全表）。
> 模板字符串中的 `\n` 为 JSON 转义（`"\\n"` 写入文件）；本文用单行展示。

### 4.1 `PS-P1` 官方·前提句生成（logline）

- **定位**：构思阶段（CR 0-10）——把一句灵感**装配成**「扩写故事前提」的完整提示词。**`PS-D10`：预设零 AI 节点**，生成由用户在外部接入 `LLMGenerate` 完成。
- **端口**：无输入；`out1` ← `n3.text`（Text）。
- **内部图**（纯文本装配）：

```
n1.TextInput ─text→ n2.PromptTemplate ─text→ n3.TextOutput
                        vars              text → out1
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 灵感素材 | `text = "一个雨夜的便利店，收银员发现常客从未买过任何东西。"`（**暴露**） |
| n2 | PromptTemplate | 前提模板 | `template = "把以下灵感扩写成一句话故事前提（logline），必须包含：主角、目标、阻碍。只输出前提本身，不要解释或补充。\n\n灵感：\n{vars}"`（**暴露**） |
| n3 | TextOutput | 前提句 | `label = "前提句"`（**暴露**） |

- **边表**：

| 边 | from | to |
|---|---|---|
| e1 | n1.text | n2.vars |
| e2 | n2.text | n3.text |

- **暴露参数（3）**：`n1.text`（灵感素材）· `n2.template`（提示词模板）· `n3.label`（输出标签）。
- **验证断言**：`PS-V101`（暴露参数集合精确匹配）· `PS-V201`（装配链离线执行：`{vars}` 被输入替换，无 PW-10）。
- **外部接线（`PS-D10` 标准用法）**：`P1.out1` → 外部 `LLMGenerate.prompt`；外部 `ProviderConfig` → `LLMGenerate.provider`。运行整图即得前提句。

**P1 资产 JSON 全量骨架（可逐字照抄 · 其余预设按同构扩展）**：

```json
{
  "schema_version": 1,
  "id": "ps_p1",
  "name": "官方·前提句生成",
  "description": "把一句灵感装配成扩写前提句的提示词（零 AI 节点）。运行得到提示词文本；请接入外部 LLMGenerate.prompt（并配置 Provider）完成生成。",
  "inputs": [],
  "outputs": [
    { "internal_node": "n3", "internal_port": "text", "display_name": "结果", "type": "text" }
  ],
  "exposed_params": [
    { "internal_node": "n1", "param_id": "text", "display_name": "灵感素材" },
    { "internal_node": "n2", "param_id": "template", "display_name": "提示词模板" },
    { "internal_node": "n3", "param_id": "label", "display_name": "输出标签" }
  ],
  "internal_graph": {
    "version": "1.0",
    "name": "",
    "nodes": [
      { "id": "n1", "type": "TextInput", "title": "灵感素材",
        "position": { "x": -640.0, "y": -60.0 },
        "params": { "text": "一个雨夜的便利店，收银员发现常客从未买过任何东西。" } },
      { "id": "n2", "type": "PromptTemplate", "title": "前提模板",
        "position": { "x": -220.0, "y": -60.0 },
        "params": { "template": "把以下灵感扩写成一句话故事前提（logline），必须包含：主角、目标、阻碍。只输出前提本身，不要解释或补充。\n\n灵感：\n{vars}" } },
      { "id": "n3", "type": "TextOutput", "title": "前提句",
        "position": { "x": 200.0, "y": -60.0 },
        "params": { "label": "前提句" } }
    ],
    "edges": [
      { "id": "e1", "from": { "node": "n1", "port": "text" }, "to": { "node": "n2", "port": "vars" } },
      { "id": "e2", "from": { "node": "n2", "port": "text" }, "to": { "node": "n3", "port": "text" } }
    ],
    "viewport": { "x": 0.0, "y": 0.0, "zoom": 1.0 }
  }
}
```

---

### 4.2 `PS-P2` 官方·角色档案卡

- **定位**：构思阶段（CR 20-30）——由一段角色描述**装配成**「结构化角色档案卡」的提示词。**`PS-D10`：预设零 AI 节点**。
- **端口**：无输入；`out1` ← `n3.text`（Text）。
- **内部图**：与 P1 **同构**（`n1.TextInput → n2.PromptTemplate → n3.TextOutput`，零 AI 节点）。
- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 角色描述 | `text = "主角：林小满，一个在夜市摆糖画摊的手艺人。"`（**暴露**） |
| n2 | PromptTemplate | 档案模板 | `template = "根据以下角色描述，生成一份结构化角色档案卡。每项一行，使用「项目：内容」格式：\n姓名 / 年龄 / 性格（至少 3 条） / 目标 / 动机 / 恐惧 / 关键关系 / 人物弧光\n\n角色描述：\n{vars}"`（**暴露**） |
| n3 | TextOutput | 角色档案卡 | `label = "角色档案卡"`（**暴露**） |

- **边表**：同 P1（e1 n1.text→n2.vars；e2 n2.text→n3.text）。
- **暴露参数（3）**：`n1.text`（角色描述）· `n2.template`（提示词模板）· `n3.label`（输出标签）。
- **外部接线（`PS-D10` 标准用法）**：`P2.out1` → 外部 `LLMGenerate.prompt`（+ `ProviderConfig`）。运行整图即得档案卡。
- **v2 图片输入提案（§8 `PS-Q6` · 依 `PS-D10` 简化）**：用户提出「角色档案卡加**图片输入**并**暴露接口**」。`PS-D10` 后预设内部零 AI ⇒ **P2 不消费图片**；图片由用户在外部接 `ImageInput → VLMGenerate.image`，把 `P2.out1`（或 `P10.out1`）接入 `VLMGenerate.prompt` 即可「看图生成档案」。待拍板仅剩 **P10（角色图片档案提示词 · §4.10）是否并入批次**。
### 4.3 `PS-P3` 官方·章节大纲生成

- **定位**：构思阶段（CR 30-40）——由故事设定**装配成**「全书章节大纲」的提示词；支持**上游输入**（接 `P1.out1` 前提 / `P2.out1` 档案卡）。**`PS-D10`：预设零 AI 节点**。⚠️ **当前引擎下 `in1` 必须接线**（展开后运行前校验拦截「未接」场景，§3.3 · `PS-Q8`）。
- **端口**：`in1`（Text · 可选标记 · display_name「故事设定」；**当前引擎下未接 = 运行失败**，见 §3.3）→ `n1.texts`；`out1` ← `n5.text`（Text）。
- **内部图**（**两级 TextMerge**，设计依据 §3.3 · 零 AI 节点 · 无 TextSplit）：

```
in1 ──────→ n1.TextMerge.texts（标记端口 · 无内部入边）
n1.text ──┐
          ├→ n2.TextMerge.texts → n4.PromptTemplate.vars → n5.TextOutput
n3.TextInput.text ─┘                 text → n5.text → out1
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextMerge | 上游合并 | `separator = "\\n"`，`ignore_empty = true`（**暴露** `separator`；其余默认：sort=edge_order、dedup=false、empty_line=keep） |
| n2 | TextMerge | 设定合并 | `separator = "\\n\\n"`，`ignore_empty = true` |
| n3 | TextInput | 设定补充 | `text = ""`（**暴露**；设定补充。原「in1 未接兜底」依赖 `PS-Q8`，当前引擎下 in1 未接会被运行前校验拦截） |
| n4 | PromptTemplate | 大纲模板 | `template = "根据以下故事设定，生成全书章节大纲。每个章节一行，格式：「第X章 \| 章节名 \| 一句话剧情」。覆盖开端、发展、高潮、结局。\n\n故事设定：\n{vars}\n\n章节大纲："`（**暴露**） |
| n5 | TextOutput | 章节大纲 | `label = "章节大纲"`（**暴露**） |

- **边表**：

| 边 | from | to |
|---|---|---|
| e1 | n1.text | n2.texts |
| e2 | n3.text | n2.texts |
| e3 | n2.text | n4.vars |
| e4 | n4.text | n5.text |

- **暴露参数（4）**：`n1.separator`（合并分隔符）· `n3.text`（设定补充）· `n4.template`（提示词模板）· `n5.label`（输出标签）。
- **验证断言**：`PS-V103`（暴露集合）· `PS-V005`（n1.texts 无内部入边）· `PS-V007`（展开正确）。
- **外部接线（`PS-D10` 标准用法）**：`P3.out1` → 外部 `LLMGenerate.prompt`（+ `ProviderConfig`）→ 生成大纲文本；如需**逐章分块**，把 `LLMGenerate.text` 接入外部 `TextSplit`（按 `\n` 拆分）即可（原 P3 内置 TextSplit 已移除）。
- **备注**：原设计内置 `TextSplit` 用于把 LLM 输出拆行显示；`PS-D10` 后 P3 不再含 AI 节点，拆行由用户在外部按需自接。`PS-Q2`（`TextOutput` 数组 join 增强）仍为通用候选，不再由 P3 驱动。
### 4.4 `PS-P4` 官方·续写助手

- **定位**：草稿阶段（CR 50-75）——给定前文**装配成**「续写提示词」；支持**上游输入**（接任意文本，如 `P3.out1` 某章）。**`PS-D10`：预设零 AI 节点**。⚠️ **当前引擎下 `in1` 必须接线**（§3.3 · `PS-Q8`）。
- **端口**：`in1`（Text · 可选标记 · display_name「前文/上下文」；**当前引擎下未接 = 运行失败**，见 §3.3）→ `n1.texts`；`out1` ← `n5.text`（Text）。
- **内部图**（与 P3 同构，**零 AI 节点**）：

```
in1 ──────→ n1.TextMerge.texts（标记端口 · 无内部入边）
n1.text ──┐
          ├→ n2.TextMerge.texts → n4.PromptTemplate.vars → n5.TextOutput
n3.TextInput.text ─┘                 text → n5.text → out1
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextMerge | 上游合并 | `separator = "\\n\\n"`，`ignore_empty = true`（**暴露** `separator`） |
| n2 | TextMerge | 前文合并 | `separator = "\\n\\n"`，`ignore_empty = true` |
| n3 | TextInput | 前文 | `text = ""`（**暴露**；前文补充。原「in1 未接兜底」依赖 `PS-Q8`，当前引擎下 in1 未接会被运行前校验拦截） |
| n4 | PromptTemplate | 续写模板 | `template = "你是小说续写助手。请续写以下内容，保持已有的叙事风格、视角、人称与时态。不要复述原文，直接从下一句开始：\n\n原文：\n{vars}\n\n续写："`（**暴露**） |
| n5 | TextOutput | 续写 | `label = "续写"`（**暴露**） |

- **边表**：

| 边 | from | to |
|---|---|---|
| e1 | n1.text | n2.texts |
| e2 | n3.text | n2.texts |
| e3 | n2.text | n4.vars |
| e4 | n4.text | n5.text |

- **暴露参数（4）**：`n1.separator`（合并分隔符）· `n3.text`（前文补充）· `n4.template`（提示词模板）· `n5.label`（输出标签）。
- **验证断言**：`PS-V104`（暴露集合）· `PS-V005`（n1.texts 无内部入边）。
- **外部接线（`PS-D10` 标准用法）**：`P4.out1` → 外部 `LLMGenerate.prompt`（+ `ProviderConfig`）→ 续写文本；把 `LLMGenerate.text` 回接 `P4.in1` 即可**多轮续写**。
### 4.5 `PS-P5` 官方·文本清理管线

- **定位**：修订阶段（CR 75-90）——一键清理：去 AI 客套前缀、去首尾空白、压缩连续空白、去控制字符/空行，再按需做文本替换。**零配置即跑**（无 Provider）。
- **端口**：无输入；`out1` ← `n4.text`（Text）。
- **内部图**：

```
n1.TextInput ─text→ n2.TextClean ─text→ n3.TextReplace ─text→ n4.TextOutput
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 原文 | `text = ""`（**暴露**） |
| n2 | TextClean | 清理 | `trim = true`（默认）· `collapse_whitespace = true` · `remove_control_chars = true`（默认）· `remove_empty_lines = true` · `remove_ai_prefix = true`（`remove_html = false`、`dedup_lines = false`、`remove_prefix/suffix = ""` 默认） |
| n3 | TextReplace | 替换 | `pattern = ""`（空 = 不改，边界安全）· `replacement = ""` · `use_regex = false` · `case_sensitive = true`（默认）· `max_replacements = 0`（默认）· `whole_word = false`（默认） |
| n4 | TextOutput | 清理结果 | `label = "清理结果"` |

- **边表**：e1 n1.text→n2.text；e2 n2.text→n3.text；e3 n3.text→n4.text。
- **暴露参数（9）**：`n1.text`（原文）· `n2.trim` · `n2.collapse_whitespace` · `n2.remove_ai_prefix` · `n2.remove_empty_lines` · `n3.pattern`（查找）· `n3.replacement`（替换为）· `n3.use_regex`（正则模式）· `n4.label`。
- **验证断言**：`PS-V105`（暴露集合）· `PS-V202`（离线执行链）。
- **备注**：`remove_html`/`dedup_lines`/`remove_prefix`/`remove_suffix` 锁默认（不暴露），需要时用户可在「复制到我的节点库」后编辑内部节点。

### 4.6 `PS-P6` 官方·一致性检查

- **定位**：修订阶段（CR 75-90）——对比两段文本（如「设定基准」vs「新章节」）输出差异报告。**零配置即跑**。
- **端口**：无输入；`out1` ← `n4.text`（Text）。
- **内部图**：

```
n1.TextInput ─text→ n3.TextDiff.text_a
n2.TextInput ─text→ n3.TextDiff.text_b ─text→ n4.TextOutput
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 基准文本 | `text = ""`（**暴露**） |
| n2 | TextInput | 待查文本 | `text = ""`（**暴露**） |
| n3 | TextDiff | 对比 | `diff_mode = "line"`（默认）· `context_lines = 3`（默认）· `output_mode = "text"`（默认） |
| n4 | TextOutput | 差异报告 | `label = "差异报告"` |

- **边表**：e1 n1.text→n3.text_a；e2 n2.text→n3.text_b；e3 n3.text→n4.text。
- **暴露参数（6）**：`n1.text`（基准文本）· `n2.text`（待查文本）· `n3.diff_mode`（对比粒度）· `n3.context_lines`（上下文行数）· `n3.output_mode`（输出形态）· `n4.label`。
### 4.7 `PS-P7` 官方·词频统计

- **定位**：辅助——对文本做基础统计 + 词频 Top N（创作阶段自查用字/关键词分布）。**零配置即跑**。
- **端口**：无输入；`out1` ← `n3.text`（Text）。
- **内部图**：

```
n1.TextInput ─text→ n2.TextStats ─text→ n3.TextOutput
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 文本 | `text = ""`（**暴露**） |
| n2 | TextStats | 统计 | `stats_mode = "all"`（基础 + 词频）· `output_mode = "text"`（默认）· `top_n = 10`（默认） |
| n3 | TextOutput | 词频统计 | `label = "词频统计"` |

- **边表**：e1 n1.text→n2.text；e2 n2.text→n3.text。
- **暴露参数（5）**：`n1.text`（文本）· `n2.stats_mode`（统计项）· `n2.top_n`（Top N）· `n2.output_mode`（输出形态）· `n3.label`。
- **验证断言**：`PS-V107`（暴露集合）· `PS-V204`（离线执行：词频 Top N 行格式 `词  次数`）。
- **备注**：`output_mode` 切到 `number` 时走 `number` 端口（中文字数）——预设默认 `text`（报告可读）；number 场景用户可复制后改内部参数。

### 4.8 `PS-P8` 官方·图片转小说

- **定位**：草稿阶段（CR 50-75）——把「看图写小说」的指令**装配成**成品提示词。**`PS-D10`：预设零 AI 节点**（原内置 `ImageInput`/`VLMGenerate`/`ProviderConfig` 已移除；图片由用户在外部接 `ImageInput → VLMGenerate.image`）。
- **端口**：无输入；`out1` ← `n3.text`（Text）。
- **内部图**：

```
n1.TextInput ─text→ n2.PromptTemplate.vars → n3.TextOutput → out1
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 提示词 | `text = "把这张图写成一个小说开头：150 字以内，有画面感，写出时间、地点与氛围。"`（**暴露**） |
| n2 | PromptTemplate | 模板 | `template = "你是小说作家，擅长把画面转成克制的文字描写。\n\n看图写作指令：\n{vars}\n\n请直接输出小说开头。"`（**暴露**） |
| n3 | TextOutput | 输出 | `label = "图片转小说提示词"`（**暴露**） |

- **边表**：e1 n1.text→n2.vars；e2 n2.text→n3.text。
- **暴露参数（3）**：`n1.text`（提示词）· `n2.template`（提示词模板）· `n3.label`（输出标签）。
- **验证断言**：`PS-V108`（暴露集合）· `PS-V205`（离线执行：装配结果 = 模板骨架 + 提示词原文）。
- **外部接线（`PS-D10` 标准用法）**：`ImageInput → VLMGenerate.image`；`P8.out1 → VLMGenerate.prompt`（+ `ProviderConfig` 配 VLM 模型，如智谱 `glm-4v-flash`）。
- **备注**：原 v1.2 版与示例工作流 [E-02 图片转小说](../../source/workflows/examples/E-02_图片转小说.json) 同构；该工作流现作为 P8 的「完整图」外部接线示范保留。`PS-Q5`（VLM 模型预填）口径不变。

### 4.9 `PS-P9` 官方·提示词整理器

- **定位**：任意创作阶段前（CR 0-40 装配期）——把多段素材（世界观 / 人设 / 大纲 / 灵感等）**整理成一套统一格式的创作提示词**，作为**唯一输出**，供下游任意生成节点（如 `LLMGenerate.prompt`）直接使用。**零配置即跑**（无 Provider · 无输入端口）。
- **端口**：无输入（素材在参数面板填写）；`out1` ← `n6.text`（Text · display_name「整理后提示词」）。
- **为什么不用输入端口（引擎约束取证）**：`validateBeforeRun` 在**展开后**的图上执行（`executor.cpp:285-294`），必填变长端口无入边 → 报「至少需要 1 条连线」（`validate.cpp:127`）；暴露输入恒可选、「是否必接由内部端口决定」（`composite.cpp:280`）⇒ **「可选输入端口未接」在当前引擎下会被运行前校验拦截**（P3/P4 同受此约束，见 §3.3）。P9 的「用户配置整合什么信息」由 **3 个暴露素材框 + 暴露模板**承载，参数面板即可完成，无需输入端口。如需接上游（如 `P1.out1` → P9），待引擎扩展 `PS-Q8` 拍板后增设输入端口。
- **内部图**（无输入端口 · 三素材框 + 合并 + 模板装配）：

```
n1.TextInput ─text─┐
n2.TextInput ─text─┼→ n4.TextMerge.texts → n5.PromptTemplate.vars → n6.TextOutput
n3.TextInput ─text─┘      text → n5.prompt      text → n6.text
out1 ← n6.text
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 素材① | `text = ""`（**暴露**） |
| n2 | TextInput | 素材② | `text = ""`（**暴露**） |
| n3 | TextInput | 素材③ | `text = ""`（**暴露**） |
| n4 | TextMerge | 总合并 | `separator = "\\n\\n---\\n\\n"`，`ignore_empty = true`（空素材跳过；块间分隔线供模板识别素材边界） |
| n5 | PromptTemplate | 提示词模板 | `template =`（**暴露**；默认见下） |
| n6 | TextOutput | 整理结果 | `label = "整理后提示词"`（**暴露**） |

- **边表**：

| 边 | from | to |
|---|---|---|
| e1 | n1.text | n4.texts |
| e2 | n2.text | n4.texts |
| e3 | n3.text | n4.texts |
| e4 | n4.text | n5.vars |
| e5 | n5.text | n6.text |

- **默认模板**（`{vars}` = 非空素材按 n1→n2→n3 顺序、以 `---` 分隔合并；JSON 中换行为 `\\n`）：

```
你是一名提示词设计师。下面是用分隔线（---）隔开的多段创作素材。请把它们整理成一套完整、连贯、可直接使用的创作提示词：
1. 合并重复信息，保留全部有效细节；
2. 按【背景设定】【角色】【情节】【风格要求】【输出格式】分节组织；
3. 只输出整理后的提示词本身，不要任何解释或前言。

素材：
{vars}
```

- **暴露参数（5）**：`n1.text`（素材①）· `n2.text`（素材②）· `n3.text`（素材③）· `n5.template`（提示词模板 · 用户配置「整合什么」的主入口）· `n6.label`（输出标签）。
- **验证断言**：`PS-V109`（暴露集合）· `PS-V206`（离线执行：① 三素材全空 → 输出含模板骨架且 `{vars}` 替换为空串（PW-10 不触发）；② 填一段 → 输出含该段原文；③ 填两段 → 两段间含 `---`）。
- **备注**：
  - 素材框个数 3 = 默认上限（对应常见「世界观 / 人设 / 大纲」三源）；需要更多或更少 → 复制到我的节点库后编辑内部节点即可。
  - P9 默认**纯文本装配**（确定性 · 零 Provider）。若需要「LLM 智能整理」（压缩 / 结构化 / 提炼重点），复制后在 `n5` 与 `n6` 之间插入 `LLMGenerate` 即可（`PS-Q7` **已结案 = A：不默认 LLM**）。
  - 典型用法：`P9.out1` → `LLMGenerate.prompt`，实现「先装配提示词、再驱动创作」。`P9` 接上游输入端口 = 引擎扩展 `PS-Q8` 拍板后的增强。

### 4.10 `PS-P10` 官方·角色图片档案卡（**待拍板备选** · 依 §8 `PS-Q6` 结果决定是否并入批次）

- **定位**：P2 的**图片版提示词装配**——把「角色档案卡」指令**装配成**成品提示词，供用户在外部接 `VLMGenerate.prompt` 配合角色图片（`ImageInput → VLMGenerate.image`）生成档案。**`PS-D10`：预设零 AI 节点**（与 P8 同构；原 v1.2 的 `VLMGenerate`/`ProviderConfig`/`in1`(Image) 端口已全部移除）。
- **端口**：无输入；`out1` ← `n3.text`（Text）。
- **内部图**：

```
n1.TextInput ─text→ n2.PromptTemplate.vars → n3.TextOutput → out1
```

- **内部节点与参数（非默认）**：

| id | 类型 | title | 非默认参数 |
|---|---|---|---|
| n1 | TextInput | 档案提示词 | `text = "结合下面这张角色图片，生成一份结构化角色档案卡。每项一行，使用「项目：内容」格式：姓名（按图中特征推测） / 外貌 / 衣着 / 气质与神态 / 性格（至少 3 条） / 目标 / 动机 / 恐惧 / 关键关系 / 人物弧光。"`（**暴露**） |
| n2 | PromptTemplate | 档案模板 | `template = "你是角色设定师，擅长从角色图片提炼外貌、气质与性格线索。\n\n{vars}"`（**暴露**） |
| n3 | TextOutput | 输出 | `label = "角色档案提示词"`（**暴露**） |

- **边表**：e1 n1.text→n2.vars；e2 n2.text→n3.text。
- **暴露参数（3）**：`n1.text`（档案提示词）· `n2.template`（提示词模板）· `n3.label`（输出标签）。
- **验证断言**：`PS-V110`（暴露集合）· `PS-V207`（离线执行：装配结果 = 模板骨架 + 档案提示词原文）。
- **外部接线（`PS-D10` 标准用法）**：`ImageInput → VLMGenerate.image`；`P10.out1 → VLMGenerate.prompt`（+ `ProviderConfig` 配 VLM 模型）。
- **与 P8 的关系**：P8 = 「图片转小说」提示词，P10 = 「角色图片档案」提示词——两者同构（TextInput→PromptTemplate→TextOutput），仅默认文案不同。**是否并入批次：待拍板 §8 `PS-Q6`**。
- **引擎约束备注（历史）**：原 v1.2 版 P10 内置 `VLMGenerate` + `in1`(Image) **必填**端口（图必填由 `local_nodes.cpp:980-982` 与展开后校验强制，见 §3.3）；`PS-D10` 后图片统一由外部 `VLMGenerate` 消费，「图必填/可选」之争不再影响预设内部结构（§8 `PS-Q6` 已按此结案）。

### 4.11 预设一览表（汇总）

| 预设 | 文件/id | 名称 | 类型 | 输入 | 暴露参数数 | Provider |
|---|---|---|---|---|---|---|
| P1 | `ps_p1` | 官方·前提句生成 | 提示词装配 | 无 | 3 | — |
| P2 | `ps_p2` | 官方·角色档案卡 | 提示词装配 | 无 | 3 | — |
| P3 | `ps_p3` | 官方·章节大纲生成 | 提示词装配 | `in1`（可选） | 4 | — |
| P4 | `ps_p4` | 官方·续写助手 | 提示词装配 | `in1`（可选） | 4 | — |
| P5 | `ps_p5` | 官方·文本清理管线 | 纯文本 | 无 | 9 | — |
| P6 | `ps_p6` | 官方·一致性检查 | 纯文本 | 无 | 6 | — |
| P7 | `ps_p7` | 官方·词频统计 | 纯文本 | 无 | 5 | — |
| P8 | `ps_p8` | 官方·图片转小说 | 提示词装配 | 无 | 3 | — |
| P9 | `ps_p9` | 官方·提示词整理器 | 纯文本 | 无（3 内部素材框） | 5 | — |
| P10 | `ps_p10` | 官方·角色图片档案卡（待拍板 · §8 `PS-Q6`） | 提示词装配 | 无 | 3 | — |

> **类型说明**：`PS-D10` 后，P1/P2/P3/P4/P8/P10 均为**提示词装配类**（零 AI 节点，输出成品提示词，AI 由用户在外部配置）；P5/P6/P7/P9 为**纯文本处理类**。所有预设内部**无 Provider**（Provider 列全为 —）。

---

## §5 推荐工作流

> 三件套落地为 `source/workflows/examples/` 示例 JSON（实施批 `PS-06`）。工作流是**完整图**（非 Composite），可直接载入运行；其中 **WF-3 演示预设串联 + 外部 AI 接线**（P1→P3→外部 LLMGenerate→P4→外部 LLMGenerate）。

### 5.1 `PS-WF1` 续写入门（文件 `E-01_续写助手.json` · 新增）

- **定位**：首条创作链路——输入前文 → P4 装配续写提示词 → 外部 LLM 生成 → 导出。**零组装**，载入即跑。
- **图结构**：

```
TextInput（前文）→ P4「官方·续写助手」.in1
P4.out1 → LLMGenerate.prompt → TextOutput（续写结果）
ProviderConfig → LLMGenerate.provider
```

- **要点**：直接拖入 **P4 预设实例**（in1 接上游 TextInput）；P4.out1 接外部 `LLMGenerate.prompt`、`ProviderConfig` 接 `LLMGenerate.provider`（**`PS-D10` 标准用法**）。用户只需在「前文」节点粘贴文本 → 运行。展示：预设的 in1 用法 + 「预设装配提示词 → 外部 AI 生成」的标准链。
- **默认参数**：TextInput.text = 一段 3~5 行的故事开头示例；LLMGenerate 用默认参数；TextOutput.label = "续写结果"。
- **验证**：`PS-V301`（可加载 + 无断链）· `PS-V302`（展开后含 P4 内部节点前缀）。

### 5.2 `PS-WF2` 图片转小说（文件 `E-02_图片转小说.json` · 已有，保留为推荐入口）

- **定位**：图片 → 文字，已有示例即推荐工作流（= **P8 的「完整图」外部接线示范**）。
- **图结构**：`ImageInput → VLMGenerate.image`；`P8.out1 → VLMGenerate.prompt`；`ProviderConfig → VLMGenerate.provider`。
- **要点**：作为 **P8 的"完整工作流"版**保留；与 P8 的关系 = 工作流展示完整接线（图片、VLM、Provider 均在外部），预设只提供提示词。两者并存。
- **验证**：`PS-V301`。

### 5.3 `PS-WF3` 全链路演示（文件 `E-03_全链路演示.json` · 新增）

- **定位**：**核心演示**——一条从"灵感 → 前提 → 大纲 → 续写第一章"的完整小说创作链路，同时展示**预设串联**（P1.out1 → P3.in1）与 **`PS-D10` 外部 AI 接线**（P3.out1 / P4.out1 → 外部 `LLMGenerate.prompt`）。
- **图结构**：

```
TextInput（灵感 · 即 P1 内部 n1.text 参数）→ P1「官方·前提句生成」.out1 → P3「官方·章节大纲生成」.in1
P3.out1 → LLM1.prompt → LLM1.text → TextOutput（大纲）
LLM1.text → TextSplit("\n") → 第一章 → P4「官方·续写助手」.in1
P4.out1 → LLM2.prompt → LLM2.text → TextOutput（第一章续写）
ProviderConfig → LLM1.provider / LLM2.provider
```

- **简化版（v1 推荐，避免多实例复杂度）**：

```
TextInput（灵感）→ P1 → P3.in1
P3.out1 → LLM1.prompt → TextOutput（大纲）
LLM1.text → TextSplit("\n") → P4.in1 → P4.out1 → LLM2.prompt → TextOutput（第一章）
ProviderConfig → LLM1.provider / LLM2.provider
```

- **要点**：① 演示 `P1.out1 → P3.in1`（PSL-1 修订版 D ③ 的核心收益）；② **`PS-D10` 外部接线**：P3/P4 输出的是**提示词**，必须经外部 `LLMGenerate`（+ `ProviderConfig`）才生成文本；③ 演示「预设（提示词装配）+ 外部 AI 节点」混排；④ 逐章拆分用外部 `TextSplit`（原 P3 内置 TextSplit 已移除，§4.3）。
- **默认参数**：与各预设默认一致；TextInput.text = "一个在无人岛上醒来、却记得所有前世的女孩。"
- **验证**：`PS-V301` · `PS-V302`（展开后同时含 `ps_p1` 与 `ps_p3` 内部节点前缀）· `PS-V303`（断链 = 0）。

### 5.4 工作流与预设的覆盖关系

| 工作流 | 使用的预设/节点 | 覆盖创作阶段 |
|---|---|---|
| WF-1 | P4 + 外部 LLMGenerate + ProviderConfig | 草稿（续写） |
| WF-2 | 基础节点（ImageInput → VLMGenerate）+ P8.out1 + ProviderConfig | 草稿（图片→文字） |
| WF-3 | P1→P3 串联 + 外部 LLMGenerate×2 + TextSplit + ProviderConfig | 构思→草稿（全链路） |

## §6 验收框架

### 6.1 离线断言（扩展 `source/tools/composite_selftest.cpp` 新增「预设库」子段）

> 实施批 `PS-03` 落地；断言并入 `--exec-selftest` 汇总（沿用现机制）。全部断言**无网络依赖**（纯文本类与提示词装配类走真实执行器；AI 生成由用户在外部节点配置，预设断言**不触发 LLM**）。

**A. 资产加载与结构（PS-V001~V010）**

| 编号 | 断言 |
|---|---|
| `PS-V001` | `source/workflows/compositeNodes/` 下恰有 9 个 `ps_p*.json` 资产文件（P10 拍板并入后为 10） |
| `PS-V002` | 逐文件 `composite_from_json` 成功（schema 合法） |
| `PS-V003` | 逐文件 `validate_composite_spec` 通过（严格校验） |
| `PS-V004` | 逐文件 `outputs.size() == 1` 且 `type == "text"`（CN-D7） |
| `PS-V005` | P3/P4：`inputs[*].internal_node.internal_port` 指向的端口**无内部入边**（验证硬约束绕行设计） |
| `PS-V006` | 逐文件 `exposed_params` 内部引用端口/参数存在、id 无重复 |
| `PS-V007` | 逐文件 `build_composite_node` 成功；`expand_composites` 无错、内部节点 id 前缀 = `<外id>__<内id>` |
| `PS-V008` | 注册表内置源扫描：`list()` 含 9 个 `ps_p*` 且来源标记 = builtin |
| `PS-V009` | 用户源复制：以同 id `save` 到用户根 → 双源合并后该 id 来源 = user（覆盖语义） |
| `PS-V010` | 删除用户副本 → 来源回落 builtin；内置源 `save`/`remove` 被拒（只读） |

**B. 暴露面精确匹配（PS-V101~V110）**

| 编号 | 预设 | 期望 `exposed_params` 精确集合（id 有序） |
|---|---|---|
| `PS-V101` | P1 | `n1.text` `n2.template` `n3.label` |
| `PS-V102` | P2 | 同 P1 结构（n1/n2/n3） |
| `PS-V103` | P3 | `n1.separator` `n3.text` `n4.template` `n5.label` |
| `PS-V104` | P4 | `n1.separator` `n3.text` `n4.template` `n5.label` |
| `PS-V105` | P5 | `n1.text` `n2.trim` `n2.collapse_whitespace` `n2.remove_ai_prefix` `n2.remove_empty_lines` `n3.pattern` `n3.replacement` `n3.use_regex` `n4.label` |
| `PS-V106` | P6 | `n1.text` `n2.text` `n3.diff_mode` `n3.context_lines` `n3.output_mode` `n4.label` |
| `PS-V107` | P7 | `n1.text` `n2.stats_mode` `n2.top_n` `n2.output_mode` `n3.label` |
| `PS-V108` | P8 | `n1.text` `n2.template` `n3.label` |
| `PS-V109` | P9 | `n1.text` `n2.text` `n3.text` `n5.template` `n6.label` |
| `PS-V110` | P10 | `n1.text` `n2.template` `n3.label` |

**C. 离线执行链（PS-V201~V207）**

| 编号 | 预设 | 断言（直接调内部节点执行器，无网络） |
|---|---|---|
| `PS-V201` | P1 模板 | `execute_prompt_template`（P1 模板 + 单值 vars）输出含「灵感：」与输入文本；`{vars}` 被替换（PW-10 不触发） |
| `PS-V202` | P5 | 输入 `"  好的，以下是正文。\n\n"` → 清理后 = `"正文。"`；替换 `pattern="foo"→"bar"` 生效；`pattern=""` 原样返回 |
| `PS-V203` | P6 | 两段相同 → `（无差异）`；一段增行 → 输出含 `+` 行 |
| `PS-V204` | P7 | 输入 `"猫 猫 狗"` → 词频含 `猫  2`、`狗  1` |
| `PS-V205` | P8 | 装配链离线执行：P8 模板 + vars → 输出含「看图写作指令：」与提示词原文；`{vars}` 被替换（PW-10 不触发） |
| `PS-V206` | P9 | ① 三个素材框全空：整链执行 → 输出含模板骨架（「素材：」等）且 `{vars}` 替换为空串（PW-10 不触发）；② 填一段素材 → 输出含该段原文；③ 填两段 → 两段间含 `---` 分隔 |
| `PS-V207` | P10 | 装配链离线执行：输出 = 模板骨架（「你是角色设定师…」）+ 档案提示词原文（PW-10 不触发） |

**D. 工作流（PS-V301~V303）**

| 编号 | 断言 |
|---|---|
| `PS-V301` | `E-01`/`E-02`/`E-03` 示例 JSON 可加载（`graph_from_json`）+ 节点/边完整 |
| `PS-V302` | WF-1 展开后含 `ps_p4__` 前缀节点；WF-3 展开后同时含 `ps_p1__` 与 `ps_p3__` 前缀 |
| `PS-V303` | WF-1/2/3 断链检查 = 0（沿用断链工具） |

### 6.2 人工验收（用户 · `PS-AC*`）

| 编号 | 验收项 |
|---|---|
| `PS-AC01` | 节点库「上级节点」分类显示 9 个「官方·」预设（P10 并入后 10），带官方徽标 |
| `PS-AC02` | 拖入 P1 → 运行 → out1 输出前提提示词；把 `P1.out1` 接外部 `LLMGenerate.prompt`（已配置 Provider）→ 运行 → 得到前提句 |
| `PS-AC03` | P3 拖入 → **in1 接上游**（如 TextInput）→ 运行 → out1 输出大纲提示词（in1 未接在当前引擎会被运行前校验拦截，§3.3 / `PS-Q8`）；`P3.out1` → 外部 `LLMGenerate.prompt` → 生成大纲 |
| `PS-AC04` | `P1.out1 → P3.in1` 串联 → `P3.out1` → 外部 `LLMGenerate.prompt` → 大纲基于前提生成 |
| `PS-AC05` | P4 拖入 → **in1 接上游** + `P4.out1` → 外部 `LLMGenerate.prompt` → 续写成功 |
| `PS-AC06` | P5/P6/P7 零配置运行（P6 填两段文本 → 差异报告） |
| `PS-AC07` | P8 拖入 → 运行 → 得图片转小说提示词；外部接 `ImageInput → VLMGenerate.image`、`P8.out1 → VLMGenerate.prompt`（Provider 配 VLM 模型）→ 输出小说开头 |
| `PS-AC08` | 「复制到我的节点库」→ 徽标消失、可编辑、保存成功 |
| `PS-AC09` | 编辑内置源被拦截（提示先复制） |
| `PS-AC10` | 首启引导面板展示 9 预设 + 3 工作流，点击可创建 |
| `PS-AC11` | P9 拖入 → 在参数面板填三个素材框（可留空）→ 运行 → 输出一套完整提示词；再把 P9.out1 接到 `LLMGenerate.prompt` 运行 → 生成正常 |
| `PS-AC12` | P10（拍板后）拖入 → 运行 → 得角色图片档案提示词；外部接 `ImageInput → VLMGenerate.image`、`P10.out1 → VLMGenerate.prompt` → 输出档案卡 |
## §7 UI 与上手体验

### 7.1 节点库「上级节点」分类（改动 `source/ui/src/node_library.cpp`）

- **官方徽标**：内置源项在名称前画金色小圆点（`category_color` 风格，金色 `~0.85,0.62,0.18`）；hover tooltip 追加一行「**内置官方预设（只读）** · 复制到我的节点库后可编辑」。
- **复制按钮**：仅内置源项显示「⧉ 复制到我的节点库」小按钮（同行右侧）→ 弹确认「将复制一份到我的节点库，之后可编辑」→ `registry.save(spec)`（同 id）→ 该项来源切 user、徽标消失。
- **只读保护**：内置源项**不显示**「删除」入口；尝试从编辑器打开内置源 → 拦截提示「官方预设为只读，请先复制到我的节点库」。
- **搜索**：沿用现有 `find_ci`（L3-01）——搜索命中「官方·」名称与类型 `ps_p*` 均有效。

### 7.2 实例外观（`node_canvas` 现有 Composite 渲染）

- 预设实例与用户自定义 Composite **同渲染**（紫条 + 规格名），`title = spec.name`（含「官方·」前缀）。
- 参数面板：仅暴露参数可见（`exposed_params`），分组沿用参数组（输出/采样/高级折叠）。

### 7.3 首启引导面板（`PS-D6` 草案 · 实施批 `PS-05`）

- **触发**：首次启动（无 `~/.brain-ai` 用户态标记）→ 模态「从官方预设开始」。
- **布局**：三组卡片——
  - **提示词装配类**：P1 前提句 · P2 角色档案卡 · P3 章节大纲 · P4 续写助手 · P8 图片转小说（P10 待拍板）
  - **文本处理类**：P5 清理管线 · P6 一致性检查 · P7 词频统计 · P9 提示词整理器
  - **推荐工作流**：WF-1 续写入门 · WF-2 图片转小说 · WF-3 全链路演示
- **交互**：点击卡片 → 预设类（提示词装配 / 纯文本处理）= 画布创建实例；工作流 = 载入示例 JSON。底部「不再提示」写入标记；「关闭」本次不创建。
- **范围待拍板**：`PS-Q3`（是否全部展示，还是按「入门 5 项 + 更多」折叠）。

### 7.4 上手动线（文档用）

1. **首启**：引导面板点「官方·续写助手」→ 画布出现实例 → 填「前文」→ 接外部 `LLMGenerate` + `ProviderConfig` → 运行 → 得到续写。（30 秒）
2. **进阶**：节点库「上级节点」拖入 P1 → 运行得前提提示词 → 拖 P3 → 连线 `P1.out1 → P3.in1` → 接外部 `LLMGenerate` → 运行得大纲。
## §8 待拍板清单

> 目标：本文归位前**全部拍板或明确归属「下一批」**。当前 **6 项**待拍板（`PS-Q7` 已结案）。

| 编号 | 议题 | 候选 | 建议 |
|---|---|---|---|
| `PS-Q1` | 引擎扩展：变长输入端口允许「外部标记 + 内部入边」合并语义（放宽 `validate_composite_spec` + 展开逻辑） | A. 保持现状（P3/P4 两级 TextMerge 为永久方案）· B. 扩展引擎（P3/P4 可简化为单级） | **A（默认）**：当前机制成熟、无需动引擎；B 记入「下一批」 |
| `PS-Q2` | 增强候选：`TextOutput` 对数组输入按行 join 显示（消除 P3 大纲的 JSON dump 显示） | A. 做（TextOutput 数组 → `\n` join）· B. 不做（文档说明 + 建议下游接 TextMerge） | **B（默认）**：WF-3 已用 TextMerge 规避；A 记入「下一批」 |
| `PS-Q3` | 首启引导展示范围（`PS-D6` 细化） | A. 全量 9 预设 + 3 工作流 · B. 入门 5 项 +「更多」折叠 · C. 仅工作流 | 待用户拍板（影响 `PS-05`） |
| `PS-Q5` | P8 的 `model_custom` 是否预填智谱 VLM 模型（如 `glm-4v-flash`） | A. 预填（开箱即用指向智谱）· B. 留空（跟随配置表，描述文案提示） | **B（默认）**：避免 provider 与模型错配；A 记入「下一批」 |
| `PS-Q6` | **P2 角色档案卡「图片输入 + 暴露接口」落地方式**（2026-10-18 用户需求 · `PS-D10` 后简化）—— 预设内部零 AI ⇒ 图片由用户在外部接 `ImageInput → VLMGenerate.image`、P2.out1 / P10.out1 接 `VLMGenerate.prompt` 即可；引擎约束（`VLMGenerate` 空图必抛错 `local_nodes.cpp:981` + 展开后必填校验）不再影响预设内部结构 | A. **P10 提示词装配版并入本批**（§4.10：TextInput→PromptTemplate→TextOutput，与 P8 同构）· B. **暂不并入**（P2 纯文本已覆盖；图片场景用户自拼 `ImageInput→VLMGenerate` + 复用 P2/P9 提示词）· C. 删除 P10 方案 | **B（默认）**：P10 与 P2/P8 增量价值有限，留作备选；A 记入「下一批」 |
| `PS-Q7` ✅ 已结案 | P9 提示词整理器的「整理」是否默认调用 LLM | A. 保持纯文本装配（默认模板 + 用户配置，确定性 · 零 Provider）· B. 默认内置 LLMGenerate（智能整理，需 Provider） | **A 已拍板（2026-10-18）**：P9 **不默认 LLM**；且 `PS-D10` 后所有预设统一零 AI，B 永久记入「下一批」或用户复制后自插 LLM |
| `PS-Q8` | **引擎扩展：「可选输入端口未接」的语义**（2026-10-18 取证：`Executor::start()` 先展开后校验，必填变长端口无入边 → 拦截「至少需要 1 条连线」，`executor.cpp:285-294` · `validate.cpp:127` · `composite.cpp:280`） | A. 不改（P3/P4 的 `in1` 一律必须接线；P9 保持无输入端口、用内部素材框）· B. 扩展引擎（可选标记端口未接 → 展开后豁免对应内部端口必填校验；「未接兜底」与 P9 输入端口增强才可用） | **A（默认）**：预设已按引擎约束设计、不阻塞批次；B 与 `PS-Q1` 属同一引擎区域，合并评估后记「下一批」 |

> `D2` 工作流方案已并入 §5（三件套草案成立）；`D5` VCN/ACN 已由 `PS-D5` 结案（不建正式框架）；`D6` 首启引导已入 §7.3 + `PS-Q3`；`PS-Q6/Q7/Q8` 为 2026-10-18 新增（P2 图片 / P9 / 可选输入引擎约束）。**`PS-Q7` 已结案 = A**（P9 不默认 LLM，`PS-D10` 统一零 AI）；`PS-Q6` 依 `PS-D10` 简化为「P10 是否并入批次」；`PS-Q8` 建议按默认案。**§8 无阻塞项，实施可启动。**

## §9 实施拆解

> 文档成熟（§4 可逐字实现 + §6 断言编号完整 + §8 归零或明确"下一批"）后按序启动。任务编号 `PS-01`…；实施批登记规则沿用 `Composition_node.md` 实施批。

### 9.1 补充需求决定（2026-10-18 · 与用户讨论定稿）

> 三条新需求（存储地点 / 左列右键删除 / 上级节点编辑器补齐）已拍板，均并入本文范围，不另立文档。

**① 存储地点双源映射（`PS-D9`）** —— 已同步修订 §0.6 / §1.1 / §2.1 / §2.2 / §9 表。

| 源 | 目录 | 读写 |
|---|---|---|
| 内置官方预设（P1~P10） | `source/workflows/compositeNodes/`（仓库内 · 随分发） | 只读 |
| 用户节点 | `~/.brain-ai/workflows/compositeNodes/`（用户工作流同级） | 可读写 |

**② 左列右键删除（并入 `PS-04` / §7.1）**

- 「上级节点」分类列表项挂右键菜单 → 「删除上级节点」→ 确认弹窗（不可恢复；提示画布在用实例将失效）→ `CompositeRegistry::remove()`。
- 目标节点正被编辑器打开 → 删除前先 `composite_editor_force_close()`。
- 内置源项不显示删除入口（只读 · `PS-AC09` 语义）。

**③ 上级节点编辑器补齐（新增 `PS-09`）**

与主编辑器差距矩阵（讨论定稿 · 参数编辑按**方案乙**复用 `property_panel` 参数控件）：

| # | 功能 | 级别 | 实现要点 |
|---|---|---|---|
| G1 | 内部节点参数值编辑 | 🟥 P0 | 选中内部节点 → 参数编辑区（复用 `draw_param_widget`） |
| G2 | 节点右键菜单 | 🟥 P0 | 「删除内部节点」「改名」 |
| G3 | 添加节点搜索/分类 | 🟥 P0 | 背景右键菜单加搜索/分组 |
| G4 | 小地图 | 🟨 P1 | 内部图导航 |
| G5 | 对齐吸附 + 辅助线 | 🟨 P1 | 复用 L3 思路 |
| G6 | 双击改名 | 🟨 P1 | 与 G2 合并 |
| G7 | 网格 + 16px 吸附控制 | 🟨 P1 | — |
| G8 | 适配视图 / 缩放 | 🟨 P1 | 「适配视图」按钮（`NavigateToContent`） |
| G9 | 复制/粘贴 | ⬜ 不追 | 主编辑器本身暂停 |
| G10 | 运行/重跑/控制台/输出/缩略图 | ⬜ 不追 | 设计期规格不单独运行 |
| G11 | 卡片参数预览 | 🟨 可选 | 与 G1 联动 |

| 编号 | 任务 | 产物 | 依赖 |
|---|---|---|---|
| `PS-01` | 资产落地：`source/workflows/compositeNodes/` 目录 + 9 个 `ps_p*.json`（P10 依 §8 `PS-Q6` 拍板结果并入；按 §4 逐字实现；`schema_version=1`；**全部零 AI 节点 · 只输出提示词 · `PS-D10`**） | 资产文件 | 本文 §4 定稿 |
| `PS-02` | 引擎双源：`CompositeRegistry` 支持内置根（`source/workflows/compositeNodes/`）+ 用户根（`~/.brain-ai/workflows/compositeNodes/`）+ 来源映射 + 只读保护 + 冲突规则 + 旧目录迁移（§2.2 / `PS-D9`） | `composite.{h,cpp}` + `paths.cpp` | PS-01 |
| `PS-03` | 验收：`composite_selftest.cpp` 新增「预设库」子段（§6.1 A~D 全部 `PS-V*`） | selftest 断言 | PS-01/02 |
| `PS-04` | UI：节点库官方徽标 + 复制按钮 + 编辑拦截 + **列表项右键「删除上级节点」**（§7.1 / §9.1②） | `node_library.cpp`（+ 必要时 `node_canvas`/`composite_editor` 只读守卫） | PS-02 |
| `PS-05` | 首启引导面板（§7.3；范围按 `PS-Q3` 拍板结果） | `app.cpp` + 标记文件 | PS-01 |
| `PS-06` | 工作流：`E-01_续写助手.json` / `E-03_全链路演示.json` 新增 + `E-02` 保留（§5） | examples JSON | PS-01 |
| `PS-07` | 文档收口：本文归位 `docs/actionPlan/` + 五处登记 + `check_all` 全绿 + CHANGELOG | 文档批 | PS-01~06 落地 |
| `PS-08` | 人工验收：§6.2 `PS-AC01`~`PS-AC12` 用户走查 | 验收回填 | PS-04/05/06 |
| `PS-09` | 上级节点编辑器补齐 G1~G8（§9.1③）：内部参数编辑（方案乙）+ 节点右键菜单 + 添加节点搜索 + 小地图 + 对齐吸附 + 双击改名 + 网格/缩放控件 | `composite_editor.{h,cpp}` + `property_panel.cpp`（抽公共参数控件） | 独立批次（可与 PS-01~08 并行） |

### 9.2 本轮新增需求（2026-10-18 · 与用户讨论）

| # | 需求 | 落点 | 状态 |
|---|---|---|---|
| ④ | 新增**官方整理节点**：用户配置「整合什么信息」→ 整理输入 → **统一成一套提示词作为唯一输出** | 新增 **P9 官方·提示词整理器**（§4.9：**无输入端口** · 3 内部素材框 + 暴露模板 · 零 Provider；`PS-Q7` **已结案 = A：不默认 LLM**） | ✅ 已入本文 |
| ⑤ | **官方·角色档案卡（P2）加图片输入 + 暴露接口** | `PS-D10` 后预设内部零 AI ⇒ 图片走外部 `ImageInput → VLMGenerate.image`，P2.out1 / P10.out1 接 `VLMGenerate.prompt` 即可；**P10 改为提示词装配版**（§4.10 · 无 `in1`(Image) 端口） | ⏳ 待拍板（P10 是否并入批次 · `PS-Q6`） |

> 两需求均**不新增基础节点**：P9 = 3×TextInput + TextMerge + PromptTemplate + TextOutput（纯组合）；P10（若并入）= TextInput + PromptTemplate + TextOutput（与 P8 同构 · **零 AI**）。**P9 用「内部素材框」而非「输入端口」** = 引擎约束（可选输入未接 → 展开后运行前校验拦截，§3.3 / `PS-Q8`）；P10 并入批次后，`PS-01` 资产数、`PS-V001/V008`、`PS-AC01` 等计数同步 +1。

## §10 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-10-18 | v1（草稿） | **建立本文（`docs/draft/`，零代码）**：§0 事实基座（14 节点 schema + 资产 JSON schema + 硬约束 + 执行语义全部取证）· §1 决策 `PS-D1`~`D8` · §2 双源工作机制 · §3 约定 `PSL-1`~`5` + 两级 TextMerge 推导 · §4 P1~P8 逐项细节（P1 全量 JSON 骨架 + P2~P8 同构差异表 + 暴露面精确集合）· §5 WF-1/2/3 · §6 验收 `PS-V001`~`V303` + `PS-AC01`~`11` · §7 UI · §8 待拍板 `PS-Q1/Q2/Q3/Q5` · §9 实施 `PS-01`~`08`。**关键设计推导**：P3/P4 两级 TextMerge 绕行「标记端口无内部入边」硬约束；P3 TextSplit 数组显示的已知瑕疵 + WF-3 规避方案。 |
| 2026-10-18 | v1.1（草稿） | **三条补充需求定稿（与用户讨论）**：① 存储双源 `PS-D9`（内置→`source/workflows/compositeNodes/` · 用户→`~/.brain-ai/workflows/compositeNodes/`，修订 §0.6/§1.1/§2.1/§2.2/§9）；② 左列右键删除（并入 `PS-04`）；③ 上级节点编辑器补齐 G1~G8（新增 `PS-09` · 参数编辑方案乙）。 |
| 2026-10-18 | v1.3（草稿） | **`PS-D10` 全局定稿：所有预设内部零 AI 调用节点**（只装配提示词，AI 由用户在外部工作流放 `LLMGenerate`/`VLMGenerate` + `ProviderConfig` 最终配置）。影响面：① §1 摘要 / §1.2 PSL-1~5 修订版 D / §2.3 P1 骨架 / §3 约定与 §3.2 拓扑 / §3.4；② **§4 全量重写**：P1（3 节点装配 + 3 暴露参数）· P2（同构）· P3（去 LLM/TextSplit，5 节点）· P4（同构）· P8（去 ImageInput/VLM/Provider，改提示词装配）· P10（去 `in1`(Image) 必填端口，改提示词装配备选）；③ §4.11 一览表（类型改「提示词装配」· 暴露 3/3/4/4/3/3 · Provider 全 —）；④ §5 工作流加外部 LLM/VLM + ProviderConfig 接线；⑤ §6 断言 V101~V110 / V201~V207 / AC01~AC12 全量同步；⑥ §8 结案 **`PS-Q7`=A**（P9 不默认 LLM）· `PS-Q6` 简化为「P10 是否并入批次」；⑦ §9.1/§9.2 需求描述同步。**全批零代码。** |

---

**文档结束。**
**状态：🟡 草稿 v1.3（2026-10-18 · `docs/draft/`）· 待逐章过审。**












