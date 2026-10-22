# PS_Schema_Reference：14 基础节点权威 schema 参考（预设库设计依据）

> 类型：**参考附录**（草稿 v1.1 · 与 [M_preset_library.md](M_preset_library.md) 配套 · 随 `PS-D10` 同步）
> 定位：预设库设计所需的**字段级权威依据** —— 14 个基础节点的类型 / 分类 / 端口 / 参数（id · 类型 · 默认值 · 范围/枚举）+ 推理共享参数面 + 关键执行语义。
> 数据来源：**代码实证** `source/engine/src/node_registry.cpp`（行号逐条标注）· `source/nodes/src/*.cpp`（执行语义）。
> 编号空间：随 `PS-*`（本文不另设编号）。
> 状态：🟡 **草稿 v1.1（2026-10-18 · 随 `PS-D10` 同步）**
> 建立：**2026-10-18**

---

## 目录

- [§1 类型字符串](#§1-类型字符串)
- [§2 14 节点总览](#§2-14-节点总览)
- [§3 推理共享参数面 inference_common_params](#§3-推理共享参数面)
- [§4 关键执行语义（预设设计必须知道）](#§4-关键执行语义)
- [§5 资产 JSON schema 速查](#§5-资产-json-schema-速查)
- [§6 变更记录](#§6-变更记录)

---

## §1 类型字符串

| 枚举 | 字符串 | 说明 |
|---|---|---|
| `PortType::Text/Image/Provider/Number/Any` | `text` / `image` / `provider` / `number` / `any` | `portTypeName`（`graph.cpp:34-44`，小写） |
| `ParamType::String/Text/Int/Float/Bool/Enum/File/Directory/Color/StringList` | `string` / `text` / `int` / `float` / `bool` / `enum` / `file` / `directory` / `color` / `stringlist` | `paramTypeName`（`graph.cpp:46-60`） |
| 分类 `NodeCategory` | `输入` / `处理` / `配置` / `推理` / `输出` / `辅助功能` | `categoryName`（`node_registry.cpp:228-239`） |

---

## §2 14 节点总览

> 参数默认值 / 枚举 / 范围均按 `node_registry.cpp` 实证；「端口」列：`v`=变长 · `o`=可选。

### N-01 TextInput 文本输入（`node_registry.cpp:338-351` · Input）

- 输出：`text`(Text)｜参数：`text`(Text, **必填**, 默认空)。

### N-02 ImageInput 图片输入（`node_registry.cpp:353-382` · Input）

- 输出：`image`(Image)｜参数：
  - `path`(File, 可选) —— png/jpg/jpeg/bmp/webp，多张每行一个；`directory`(Directory, 可选) —— 批量取目录全部图片；**二者至少填一个**
  - `resize_mode`(enum `none`/`fit`/`fill` = `none`)；`resize_max_width`/`resize_max_height`(int, 0=不限制)
  - `crop_enabled`(bool=false)；`crop_x`/`crop_y`/`crop_w`/`crop_h`(int, 0)

### N-03 PromptTemplate 提示词模板（`node_registry.cpp:384-443` · Process）

- 输入：`vars`(Any, **v, o**)｜输出：`text`(Text)
- 参数：`template`(Text, **必填**, 默认 `请根据以下内容续写：\n{vars}`)
- **AI 生成提示词子面**（独立按钮触发，默认 manual 不参与）：`provider`(enum 动态) · `mode`(enum `official`/`web`=`official`) · `api_base` · `model` · `model_custom` · `api_key`(secret) · `api_key_ref` · `system_prompt` · `temperature`(0.7) · `max_tokens`(2048) · `top_p`(1.0) · `top_k`(0) · `stop`(StringList) · `seed`(0) · `stream`(true)
- **占位符规则**：`{端口名}`；变长多值只有 `{1}/{2}…`，**单值才有 `{vars}`**；未填充占位符报错（PW-10）。

### N-04 TextMerge 文本合并（`node_registry.cpp:445-472` · Process）

- 输入：`texts`(Text, **v**)｜输出：`text`(Text)
- 参数：`separator`(String, 默认 `\n`，支持 `\n` 转义) · `ignore_empty`(bool=true) · `sort_mode`(enum `edge_order`/`by_name`=`edge_order`) · `prefix`/`suffix`(String="") · `source_label`(enum `none`/`[nX]`=`none`) · `dedup`(bool=false) · `empty_line_mode`(enum `keep`/`collapse`/`remove`=`keep`)

### N-05 ProviderConfig 提供商配置（`node_registry.cpp:474-523` · Config）

- 输出：`provider`(Provider)｜参数：
  - `provider`(enum **动态** = 配置表 `provider_option_ids()` 首选；`provider_spec.cpp` 驱动，PB2-24 就地重建枚举)
  - `mode`(enum `official`/`web`=`official`)
  - `api_base` · `model`(enum 内置提示) · `model_custom`(覆盖) · `api_key`(secret) · `api_key_ref`
- 可见性：`mode=web` 时隐藏 official 专属 5 项（`node_registry.cpp:515-521`）。

### N-06 LLMGenerate 文本生成（`node_registry.cpp:525-569` · Inference）

- 输入：`prompt`(Text) · `provider`(Provider, **o**)｜输出：`text`(Text)
- 参数 = `inference_common_params("web")`（**mode 默认 web**，NE-D38）+ 特有：`model`(enum `deepseek-chat`/`deepseek-reasoner`/`expert`=`deepseek-chat`) · `response_format`(enum `text`/`json_object`=`text`) · `n`(int 1~8=1, `n>1` 时 text 输出第一个候选)

### N-07 VLMGenerate 图片理解（`node_registry.cpp:571-607` · Inference）

- 输入：`prompt`(Text) · `image`(Image, **v**) · `provider`(Provider, **o**)｜输出：`text`(Text)
- 参数 = `inference_common_params("official")`（**mode 默认 official**）+ 特有：`image_order`(enum `edge_order`/`by_name`=`edge_order`) · `resize_mode`/`resize_max_width`/`resize_max_height`/`crop_enabled`/`crop_x`/`crop_y`/`crop_w`/`crop_h`（与 N-02 共用预处理）

### N-08 TextOutput 文本输出（`node_registry.cpp:609-637` · Output）

- 输入：`text`(Text)｜参数：`label`(String="输出") · `export_dir`(Directory="") · `file_name`(String="") · `auto_export`(bool=false) · `export_template`(Text="") · `batch_export`(bool=false) · `archive_policy`(enum `inherit`=`inherit`)

### N-09 TextSplit 文本分块（`node_registry.cpp:644-` · Auxiliary）

- 输入：`text`(Text)｜输出：`texts`(Text) —— **JSON 数组**（`text_split.cpp:148-153`）
- 参数：`split_mode`(enum `by_separator`/`by_length`/`by_paragraph`/`by_regex`=`by_separator`) · `separator`(默认 `\n\n`) · `keep_separator`(bool=false) · `max_length`(int 2000) · `overlap`(int 0) · `regex_pattern`(String="")
- 边界：空输入 → 空数组；分隔符不存在 → 单段（原文本）；`max_length=0` → 校验层拦。

### N-10 TextExtract 文本提取（`node_registry.cpp:672-700` · Auxiliary）

- 输入：`text`(Text)｜输出：`texts`(Text) · `text`(Text)
- 参数：`extract_mode`(enum `regex`/`between`/`json_path`/`key_value`=`regex`) · `pattern` · `start_marker` · `end_marker` · `json_path` · `key_name` · `output_mode`(enum `texts`/`text`=`texts`)

### N-11 TextClean 文本清理（`node_registry.cpp:702-` · Auxiliary）

- 输入：`text`(Text)｜输出：`text`(Text)
- 参数：`trim`(bool=**true**) · `collapse_whitespace`(bool=false) · `remove_html`(bool=false) · `remove_control_chars`(bool=**true**) · `dedup_lines`(bool=false) · `remove_empty_lines`(bool=false) · `remove_prefix`/`remove_suffix`(String="") · `remove_ai_prefix`(bool=false)
- 边界：全部 false → 原文本不变；`remove_prefix/suffix` 只去开头/结尾一次。

### N-12 TextReplace 文本替换（`text_replace.cpp` · Auxiliary）

- 输入：`text`(Text)｜输出：`text`(Text)
- 参数：`pattern`(String="") · `replacement`(String="") · `use_regex`(bool=false) · `case_sensitive`(bool=**true**) · `max_replacements`(int 0) · `whole_word`(bool=false)
- 边界：**空 pattern = 不改**（`text_replace.cpp:163-165`）；替换为空 = 删除匹配；正则非法 → 报错。

### N-13 TextStats 文本统计（`text_stats.cpp` · Auxiliary）

- 输入：`text`(Text)｜输出：`text`(Text) · `number`(Number=中文字数)
- 参数：`stats_mode`(enum `basic`/`word_freq`/`all`=`basic`) · `output_mode`(enum `text`/`number`=`text`) · `top_n`(int 10)
- 边界：空输入 → 全 0。

### N-14 TextDiff 文本对比（`text_diff.cpp` · Auxiliary）

- 输入：`text_a`(Text) · `text_b`(Text)｜输出：`text`(Text)
- 参数：`diff_mode`(enum `line`/`word`/`char`=`line`) · `context_lines`(int 3) · `output_mode`(enum `text`/`json`=`text`)
- 边界：两文本相同 → `（无差异）`；一方为空 → 全部为增/删。

---

## §3 推理共享参数面

### 3.1 `inference_common_params`（N-06/N-07 共用 17 项 · `node_registry.cpp:165-223`）

| # | id | 类型 | 默认 | 范围/枚举 | 分组 |
|---|---|---|---|---|---|
| 1 | `system_prompt` | Text | `""` | — | 输出 |
| 2 | `mode` | enum | **调用方传入**（N-06=web / N-07=official，NE-D38） | `official` / `web` | 输出 |
| 3 | `max_tokens` | int | `2048` | 1~8192 | 输出 |
| 4 | `stop` | StringList | `[]` | 逗号分隔；网页版忽略 | 输出 |
| 5 | `stream` | bool | `true` | — | 输出 |
| 6 | `temperature` | float | `0.7` | 0.0~2.0（步 0.1） | 采样 |
| 7 | `top_p` | float | `1.0` | 0.0~1.0（步 0.05） | 采样 |
| 8 | `top_k` | int | `0` | 0~100；0=不限制；网页版忽略 | 采样 |
| 9 | `frequency_penalty` | float | `0.0` | -2.0~2.0（步 0.1）；网页版忽略 | 采样 |
| 10 | `presence_penalty` | float | `0.0` | -2.0~2.0（步 0.1）；网页版忽略 | 采样 |
| 11 | `seed` | int | `0` | 0~1e9；网页版忽略 | 采样 |
| 12 | `timeout_connect_ms` | int | `0` | 0=用全局 | 高级 |
| 13 | `timeout_first_byte_ms` | int | `0` | 0=用全局 | 高级 |
| 14 | `timeout_stream_idle_ms` | int | `0` | 0=用全局 | 高级 |
| 15 | `retry_enabled` | bool | 全局 `error.retry_enabled` | — | 高级 |
| 16 | `retry_count` | int | `0` | 0~5；0=用全局 | 高级 |
| 17 | `retry_interval_ms` | int | `0` | 0~30000；0=用全局 | 高级 |

### 3.2 特有增量

| 节点 | 特有参数 |
|---|---|
| N-06 LLMGenerate | `model`(enum `deepseek-chat`/`deepseek-reasoner`/`expert`=`deepseek-chat`，插在 `mode` 之后) · `response_format`(enum `text`/`json_object`=`text`) · `n`(int 1~8=1) |
| N-07 VLMGenerate | `image_order`(enum `edge_order`/`by_name`) · 图片预处理 8 项（resize/crop，与 N-02 同名同语义） |

### 3.3 对预设暴露面的含义（对应 M_preset_library `PSL-3/4` · `PS-D10`）

- **`PS-D10`（2026-10-18）：预设内部零 AI 调用节点** —— 预设内**不出现** `LLMGenerate` / `VLMGenerate` / `ProviderConfig`；本节的采样 / Provider 参数面**仅存在于用户外部节点**，预设既不暴露、也不写默认值。
- **提示词装配类预设**（P1/P2/P3/P4/P8/P10）只暴露内容类参数：`TextInput.text` / `PromptTemplate.template` / `TextOutput.label`（P3/P4 另暴露 `TextMerge.separator`）。
- **纯文本类预设**（P5/P6/P7/P9）暴露各自处理参数（与 AI 无关）。

---

## §4 关键执行语义（预设设计必须知道）

| # | 语义 | 实证 | 对预设的影响 |
|---|---|---|---|
| 1 | **`text_of`：非字符串一律 JSON dump**（含数组） | `text_process_common.h:24-33` | `PS-D10` 后 P3 不再内置 TextSplit（主文档 §4.3）；若用户在外部把 `TextSplit.texts` 接 `TextOutput`，仍会 JSON dump（`PS-Q2` 通用候选） |
| 2 | **`PromptTemplate.vars` 变长多值只有 `{1}/{2}…`；单值才有 `{vars}`** | `local_nodes.cpp:433-446` | P3/P4 必须保证 vars 收到**单值**（`n2.text`），模板才能用 `{vars}` |
| 3 | **PW-10：未填充占位符报错**（PQ-13 已拍板） | `local_nodes.cpp:462-468` | `{vars}` 必须恒有值（两级 TextMerge）；预设内不能出现用户无法填充的占位符。⚠️ `in1` 未接时**运行前校验**先拦截「至少需要 1 条连线」，走不到执行器（见第 13 条） |
| 4 | **TextMerge.texts 递归展平数组** | `local_nodes.cpp:493-509` | TextSplit → TextMerge 直连友好；用户在外部拆章时可用 TextMerge 承接数组 |
| 5 | **TextSplit.texts 输出 = JSON 数组** | `text_split.cpp:148-153` | 原 P3 内置 TextSplit 已随 `PS-D10` 移除（主文档 §4.3）；用户在外部拆章时输出为数组，可直接接 `TextMerge.texts`（展平） |
| 6 | **TextReplace 空 pattern = 不改** | `text_replace.cpp:163-165` | P5 的替换默认空 pattern 安全 |
| 7 | **TextClean 默认开 `trim` + `remove_control_chars`** | `text_clean.cpp:120-126` | P5 只需再开 `collapse_whitespace`/`remove_empty_lines`/`remove_ai_prefix` |
| 8 | **TextDiff 无差异 → `（无差异）`** | `text_diff.cpp:197` | P6 断言 `PS-V203` |
| 9 | **TextStats `output_mode=number` 只走 number 端口** | `text_stats.cpp:170-172` | P7 默认 `text` 输出报告；number 场景需改内部参数 |
| 10 | **LLMGenerate mode 默认 web / VLM 默认 official** | `node_registry.cpp:541/605`（NE-D38） | `PS-D10` 后预设内不再有 LLM/VLM/ProviderConfig；mode 由用户在**外部节点**配置（`LLMGenerate` 默认 web / `VLMGenerate` 默认 official，`ProviderConfig.mode` 可切） |
| 11 | **`n>1` 时 LLM text 端口输出第一个候选** | `node_registry.cpp:564-565` | 外部 `LLMGenerate` 锁 `n=1`（默认），预设内部不涉及 |
| 12 | **ImageInput path 为空 → 运行报错** | `local_nodes.cpp`（执行器校验） | `PS-D10` 后 `ImageInput` 只在**外部**出现（如 `P8.out1 → VLMGenerate.prompt` 配图场景）；用户运行前需填 path |
| 13 | **展开后运行前校验：必填变长端口无入边 → 拦截「至少需要 1 条连线」**（2026-10-18 取证） | `executor.cpp:285-294`（`start()` 先 `expand_composites()` 后 `validateBeforeRun()`）· `validate.cpp:127` · `composite.cpp:280`（「暴露输入恒可选：是否必接由内部端口决定」） | **「可选输入端口未接」当前引擎下不成立**：P3/P4 的 `in1` 必须接线，「未接兜底」待主文档 `PS-Q8`；P9 因此用**内部素材框**（无输入端口）；`PS-D10` 后 P10 已无 `in1`(Image) 端口，图必填约束只在用户外部接线时涉及 |

---

## §5 资产 JSON schema 速查

> 对应 `composite_to_json`（`composite.cpp:124-161`）+ `graph_to_json`（`workflow_io.cpp:43`）；完整示例见主文档 §4.1。

```jsonc
{
  "schema_version": 1,                       // kCompositeSchemaVersion（composite.cpp:16）
  "id": "ps_p1", "name": "官方·…", "description": "…",
  "inputs":  [{ "internal_node", "internal_port", "display_name", "type", "is_optional" }],
  "outputs": [{ "internal_node", "internal_port", "display_name", "type" }],   // 恰 1 项
  "exposed_params": [{ "internal_node", "param_id", "display_name" }],
  "internal_graph": {                        // graph_to_json 结构（与工作流 JSON 同构）
    "version": "1.0", "name": "",
    "nodes": [{ "id", "type", "title", "position": {"x","y"}, "params": {…} }],
    "edges": [{ "id", "from": {"node","port"}, "to": {"node","port"} }],
    "viewport": { "x", "y", "zoom" }
  }
}
```

**加载链（`PS-D4`）**：`composite_from_json` → `validate_composite_spec`（严格：禁嵌套 / 输出唯一 / 引用存在 / 暴露 id 唯一 / 标记输入端口无内部入边）→ `CompositeRegistry`（惰性扫描；损坏仅告警跳过）。

---

## §6 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-10-18 | v1（草稿） | **建立本文**：类型字符串 · 14 节点 schema 逐项（含代码行号）· 推理共享参数面 17 项 + 特有增量 · 12 条关键执行语义 · 资产 JSON schema 速查。全部数据来自代码实证。 |
| 2026-10-18 | v1.1（草稿） | **随 `PS-D10`（主文档 v1.3）同步**：§3.3 暴露面含义改为「预设零 AI · 采样/Provider 参数面只在外部节点」；§4 执行语义第 1/2/4/5/10/11/12/13 条更新（P3 去 TextSplit · P8/P10 去 ImageInput/VLM/Provider · `PS-V205` 语义变更等）。 |

---

**文档结束。**
**状态：🟡 草稿 v1.1（2026-10-18 · `docs/draft/`）· 与 [M_preset_library.md](M_preset_library.md) 配套。**


