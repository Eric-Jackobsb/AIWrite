# M_8nodes_extension：8 节点深化开发（L1 层 · ✅ 已实施完成 + 人工验收完成）

> 类型：跨里程碑功能计划（不占用 M1–M8 编号；与 [M_node_expansion.md](M_node_expansion.md) / [M_patchAB_rest.md](M_patchAB_rest.md) / [M_patchC_web_node_bind.md](M_patchC_web_node_bind.md) / [M_canvas_snapshot.md](M_canvas_snapshot.md) 平级互链）。
> 上游依据：[M_node_expansion.md](M_node_expansion.md) §4.2 **L1 层定向**（"不加节点，把现有 8 个做深"）+ §11 `D-1` / `D-7` 拍板（上游现为 **v6 实施依据 · 四层全部定向**，2026-10-12）。
> 编号空间：`L1-*`（任务）· `NE-D*`（**L1 层决策** —— 承接上游 [M_node_expansion.md](M_node_expansion.md) 的定向；⚠️ **上游同名号另有全局含义**，其 L1 细节别名已改号为 `NE-D30`~`NE-D37`）· `VL1-*`（验证项）· `AL1-*`（验收）—— `NE-D*` **已登记**（[../../AI_README.md](../../AI_README.md) §5）；`L1-*` / `VL1-*` / `AL1-*` **本批登记**（批 9-0 · 见 §10）。
> 状态：✅ **L1 实施完成（2026-10-14 · L1-a~f 六批落地）+ 人工验收完成（2026-10-15 · 用户）** —— `VL1-01`…`VL1-12` 离线断言全过（graph **110 → 144 / 0** · exec **546 → 598 / 0** · 断链 **1375 / 0** · check_all **15/15**）；`AL1-01`…`AL1-06` 用户人工验收通过（2026-10-15 · 见 [../audit/L1_checklist.todo](../audit/L1_checklist.todo)）。本文 **v1.4（收口批 · 纯文档）**。
> 相关：[../roadmap.md](../roadmap.md) §六 / §十 · [../ai_writer_nodes.md](../ai_writer_nodes.md) §4.3–§4.5 / §9.2 / §19.1 · [../../source/engine/src/node_registry.cpp](../../source/engine/src/node_registry.cpp) · [../../source/nodes/src/register_executors.cpp](../../source/nodes/src/register_executors.cpp) · 看板卡 `DOCS-NE-01`（任务号 `NE-01…` 待启用）。
> 建立：**2026-10-12**（v1 · 方案定稿）

---

## §0 文档元信息与约定

### 0.1 定位

- **本文是 L1 的实施依据**：上游 [M_node_expansion.md](M_node_expansion.md) 现为**实施依据文档**（🟢 **四层全部定向** · 2026-10-12 v6 —— L1/L2/L3/L4 均已定向，L1 细节由**本文**承接）；本文是**L1 层的定向结论与实施方案**（有推荐、有任务、有验收）。
- **不改他文**：本文对 [../roadmap.md](../roadmap.md) / [../ai_writer_nodes.md](../ai_writer_nodes.md) / [M6.md](M6.md) 等**只单向引用**；实施时若需改动其内容，**另开条目**并同批改记（[../../AI_README.md](../../AI_README.md) §5 / §7 第 6 条）。
- **不撞已固定结论**：`PQ-20` 已推翻「节点数维持 8」；本文**不新增节点类型**（L1 定义），**不涉及** §19.1 延后清单的 14 项（其中无「参数级增强」）。

### 0.2 本文要解决的那一个问题

> 「深化开发这 8 个节点」—— 具体**深化什么、深化到哪、怎么验收**。

### 0.3 执行原则（沿用 [M_node_expansion.md](M_node_expansion.md) §0.3 三条）

1. **先定层，再谈点**：L1 已定（不加节点）；本文只谈「点」。
2. **不实测不填值**：所有成本/行为差异给出可复核依据；没有依据的写「待验证」。
3. **要动契约就先登记**：任何撞「已固定」结论的选项，**先**立 `PQ-xx` 并拍板，**后**改代码与文档。

### 0.4 本轮范围

| 做 | 不做 |
|---|---|
| 8 节点参数 / 行为 / UI 深化 · 公共参数面抽象 · 参数二级分组 · 超时+重试接线 · 网页版参数标注 · 离线断言与人工验收 | 新增节点类型 · 改动节点数 · 改引擎语义（条件分支/循环/子工作流）· 改冻结区 `I17` · 改 `image_decode` WIC 部分 · 排期冲突的 L2/L3/L4 项 |

---

## §1 结论摘要（先读这一段）

**L1 深化开发 = 8 节点全扩展 + 4 项横向能力**；不新增节点类型，不改引擎语义。

### 1.1 逐节点采纳摘要

| 节点 | 采纳 | 不采纳 | 性质 |
|---|---|---|---|
| **N-01** `TextInput` | 文件导入、多行编辑增强、变量插入 | 最近使用 / 片段 | 参数 + UI |
| **N-02** `ImageInput` | 目录批量、EXIF/元数据、图片预处理（缩放/裁剪） | URL 输入、剪贴板粘贴 | 参数 + 执行器 |
| **N-03** `PromptTemplate` | **AI 生成提示词**（独立按钮）、`seed`、自带 AI + 登录网页、无需额外连线 | 模板库（被取代） | **重项** |
| **N-04** `TextMerge` | 排序、前缀后缀、带源标签、去重、空行增强 | — | 参数 |
| **N-05** `ProviderConfig` | 连接测试按钮、参数预设、模型列表刷新、后端适配扩展 | 本地模型（撞 §十） | 参数 + UI |
| **N-06** `LLMGenerate` | `top_k` / `stop` / 惩罚类 / JSON 输出 / 超时 / 重试 / 多候选 `n` | — | 参数 |
| **N-07** `VLMGenerate` | `seed` / `mode` / 图片预处理 / 多图顺序 / 描述缓存 | — | 参数 + 执行器 |
| **N-08** `TextOutput` | 导出模板、批量导出、自动命名、归档策略参数化 | 更多导出格式 | 参数 |

### 1.2 横向能力摘要

| 主题 | 结论 |
|---|---|
| 推理节点公共参数面 | **抽象出来**，N-06 / N-07 保持一致 |
| 参数二级分组 | **落地**（采样 / 输出 / 高级） |
| 网页版参数策略 | **标注「忽略」**（不隐藏） |
| 超时 + 重试 | **L1 横向能力**，一次给所有推理节点接上 |

### 1.3 关键约束

- **不新增节点类型** ⇒ 不触碰 `api_probe.cpp:1936`（`registry.size() == "8"`）/ `:1938`（`expected_types[]`）/ `:1953-1959`（分类计数）的节点数硬断言；且现有参数级断言只查 `temperature` / `max_tokens` 的**存在与范围**（`:1966-1985`），**无参数个数断言** ⇒ 新增参数同样不撞硬断言。
- **不撞 §十** ⇒ 不做本地模型、不做图片输出节点。
- **不撞 §19.1** ⇒ 14 项延后清单中无「参数级增强」，L1 全程合法。
- **N-03 是重项但不单独排期** ⇒ 与其他 L1 项同批推进，验收时单列一条人工项。

---

## §2 决策记录（`NE-D*` 承接）

> 上游（[M_node_expansion.md](M_node_expansion.md)）`NE-D1` 已录（`PQ-20` 推翻节点数固定）；本文补充 L1 层决策 —— ⚠️ **两文 `NE-D` 编号分工（批 9-1）**：**本文的 `NE-D1`~`NE-D13` 是 L1 细节**；上游的 `NE-D3`~`NE-D14` 另指全局/四层决策 ⇒ 上游已把 L1 细节别名改号为 **`NE-D30`~`NE-D37`**（逐条对齐本文 `NE-D3`~`NE-D13`）；**引用 L1 细节一律引本文的号**。

| 编号 | 议题 | 结论 | 依据 | 登记动作 |
|---|---|---|---|---|
| `NE-D2` | 本次节点扩展落在哪一层 | **L1**（不新增节点，深化现有 8 个） | [M_node_expansion.md](M_node_expansion.md) §4.2 · §11 `D-1` | 本文 §1；[../roadmap.md](../roadmap.md) §六「补强」栏 |
| `NE-D3` | N-03 的 AI 调用是否复用 N-05 解析 | **复用** `resolve_effective_provider`（`engine/src/provider_resolve.cpp:106`）+ `ai::resolve_api_key`（`ai/include/deepseek_official_provider.h:87/89`） | 本文 §4.3 | 本文 §4.3；两符号分属 `engine` / `ai` 两模块 |
| `NE-D4` | N-03 的登录入口 | **复用现有「网页版会话」区块**（与 N-05 相同） | 本文 §4.3 | 本文 §4.3；`ui/property_panel.cpp` |
| `NE-D5` | N-03 的 `seed` 语义 | **只作用于内部 AI 调用**（生成提示词），不影响模板替换 | 本文 §4.3 | 本文 §4.3 |
| `NE-D6` | N-03 是否单独排期与验收 | **不单独** —— 与其他 L1 项同批，验收时单列一条人工项 | 本文 §4.3 / §6 | 本文 §6 / §7.2 |
| `NE-D7` | 公共参数面清单 | `system_prompt` · `temperature` · `max_tokens` · `top_p` · `top_k` · `stop` · `frequency_penalty` · `presence_penalty` · `seed` · `stream` · `mode` | 本文 §5.1 | 本文 §5.1 |
| `NE-D8` | JSON 输出验证时机 | **先实现再验证**（豆包 / DeepSeek `response_format`） | 本文 §4.6 | 本文 §4.6 / §6 L1-e |
| `NE-D9` | 超时 + 重试接线 | **L1 横向能力**，读 `config.timeout.*` / `config.error.*`，节点参数可覆盖 | 本文 §3.3 | 本文 §3.3 |
| `NE-D10` | N-03 AI 生成模式的触发 | **独立按钮「生成提示词」**；无参数开关；默认 manual | 本文 §4.3 | 本文 §4.3 |
| `NE-D11` | N-05 参数预设存储形态 | **内置表 + 用户覆盖**（复用 `providers.json` 机制） | 本文 §4.5 | 本文 §4.5 |
| `NE-D12` | N-05 模型列表刷新 | **从配置表读**，不从 API 拉 | 本文 §4.5 | 本文 §4.5 |
| `NE-D13` | N-02 图片预处理形态 | **节点内参数**（缩放 / 裁剪） | 本文 §4.2 | 本文 §4.2 |

---

## §3 横向能力（跨节点）

> 四项横向能力先落地（批次 L1-a），是逐节点扩展的地基。

### 3.1 推理节点公共参数面

**目标**：N-06 与 N-07 参数面**保持一致**；未来新增推理节点时**不重复决定**。

**落地方式**：
- 抽出「推理节点公共参数声明」函数（如 `engine::inference_common_params()`）。
- N-06 / N-07 的注册函数**调用同一份**，只在各自特有参数上做增量。
- 断言：`N-06.params ⊆ 公共面` 且 `N-07.params ⊆ 公共面`，且**对称差集为空**（除节点特有项）。

**公共参数面**（详见 §5.1）：

```
system_prompt · temperature · max_tokens · top_p · top_k · stop
· frequency_penalty · presence_penalty · seed · stream · mode
```

**不改动**：参数 id 命名沿用现有（`system_prompt` / `temperature` / …），**不重命名** —— 保证旧工作流 JSON 兼容。

### 3.2 参数二级分组

**目标**：参数面板从「长列表」变为「分组可折叠」，与 P7-a UI A 档的「参数面板分组」衔接。

**分组定义**：

| 分组 | 参数 | 默认 |
|---|---|---|
| **采样** | `temperature` · `top_p` · `top_k` · `frequency_penalty` · `presence_penalty` · `seed` | 折叠 |
| **输出** | `max_tokens` · `stop` · `stream` · `mode` · `system_prompt` | 展开 |
| **高级** | `timeout.*` / `error.*` 覆盖项 | 折叠 |

**落地方式**：
- 复用已有 `visible_when_param` / `visible_when_value` **条件可见**机制（[../../source/engine/src/node_registry.cpp](../../source/engine/src/node_registry.cpp) 第 347–353 行赋值；可见性判定见 `engine::param_visible()`）。
- 现状基础：已有**一级**「参数」`CollapsingHeader`（`ui/property_panel.cpp` 第 1053–1055 行 · P7a-15）⇒ 本轮是在该一级组**内**加**二级**分组，**不是**从零新建面板。
- 分组是**纯 UI 层**（`ui/property_panel.cpp`）；**不影响参数序列化**（工作流 JSON 结构不变）。
- 折叠状态持久化到 `imgui.ini`（不写 `config.toml`）。

**不改动**：参数 id、默认值、范围、枚举——只改**渲染分组**。

### 3.3 超时 + 重试接线（横向能力）

**现状口径（批 9-0 复核 · 2026-10-12）**：`timeout.*` / `error.*` 在**官方 API 线已接线**（`M_patchAB` 批 4 · 2026-10-11）—— `api_probe.cpp:1489-1491` 与 `utils/config.cpp:44-46` 同载「已接线，断言 = 不在未接清单里」。因此本项**不是**"把未接线字段接上"，而是**两件事**：① **加节点级覆盖**；② 把 **web（DOM / 上传）线的硬编码超时**改为读配置。

**目标**：所有推理节点（N-06 / N-07 / N-03 内部 AI 调用）统一读配置，且**可被节点参数覆盖**。

**配置字段**（`config.toml` · 默认值见 `utils/include/config.h` 第 69–78 行）：

| 字段 | 用途 | 默认值 | 现行消费点（已接线 = API 线） |
|---|---|---|---|
| `timeout.connect_ms` | 连接超时 | 10000 | `nodes/local_nodes.cpp:511` |
| `timeout.first_byte_ms` | 首字节超时 | 30000 | `nodes/local_nodes.cpp:515` |
| `timeout.stream_idle_ms` | 流式空闲超时 | 300000 | 设置面板可编辑（`ui/settings_panel.cpp:441-445`）+ 范围校验（`utils/settings_validation.cpp:45-55`） |
| `error.retry_enabled` | 重试开关 | true | `nodes/local_nodes.cpp:518-519`（`ai::clamp_retry_count`） |
| `error.retry_count` | 重试次数 | 1 | 同上（上限 5） |
| `error.retry_interval_ms` | 重试间隔 | 2000 | `nodes/local_nodes.cpp:541`（`ai::retry_delay_ms` · `ai/retry_policy.cpp:22` 封顶 30000） |

**仍硬编码的点（本项真实对象 · 已实测）**：

| 位置 | 硬编码值 | 说明 |
|---|---|---|
| `ai/include/dom_web_client.h:44` | `timeout_ms = 180000` | DOM 通道总超时（含 ensure_session） |
| `ai/include/upload_contract.h:69` | `180000` | 上传契约总超时 |
| `ai/src/upload/upload_registry.cpp:39` | `kScriptPollMs = 15000` | 单次页面采样脚本超时 |
| `ai/src/dom_web_client.cpp` | 多处 `15000` / `30000` / `60000` | CDP 脚本 / 启动 / 轮询 |
| ⚠️ 语义提示 | — | **CDP 脚本超时 ≠ 网络超时**：改读配置时不要用 `timeout.connect_ms` 去替换脚本超时（会改变行为）；建议单列 `web.*` 或只接"总超时"两项 |

**本轮增量（L1）**：
- ① **节点级覆盖**：N-06 / N-07 参数面板「高级」组提供同名覆盖项（`timeout_connect_ms` 等）；空 / 0 = 用全局配置。
- ② **覆盖面**：把同一份读取接进 **N-07**（图片理解）与 **N-03 内部 AI 调用**（现状只有文本生成走重试）。
- ③ **web 线**（`dom_web_client` / `upload_registry`）：只接**总超时**两项（`dom_web_client.h:44` / `upload_contract.h:69`），脚本级超时**保持不动**；实施后实测回填。

**接线点**：
- `utils/config.{h,cpp}`：`app_config()` 已有进程级缓存（Patch A `PA-07`）—— 直接读，**不新增缓存**。
- `nodes/local_nodes.cpp`：重试循环**已在节点内**（第 506–541 行）⇒ 只需把"读 `runtime_config.error.*`"改为"**节点参数覆盖优先**"。
- `engine/executor.cpp`：**不动** —— 现状重试**不在**执行器层（复核结论：执行器无 `retry` / `timeout` 命中）。若未来要抽象成通用包装层，**另开条目**。

**验收**：`VL1-03`（节点级覆盖生效：填参数 ⇒ 行为变；不填 ⇒ 回落全局）。

### 3.4 网页版参数标注

**目标**：新参数在网页版模式下**标注「忽略」**，不隐藏。

**行为矩阵**：

| 参数 | 网页版行为 | UI 表现 |
|---|---|---|
| `top_k` | 忽略 | 参数可见，灰色标注「（网页版忽略）」 |
| `stop` | 忽略 | 同上 |
| `frequency_penalty` | 忽略 | 同上 |
| `presence_penalty` | 忽略 | 同上 |
| `seed` | 忽略（已有提示） | 同上 |
| JSON 输出（`response_format`） | 忽略 | 同上 |
| `temperature` / `max_tokens` / `top_p` | 忽略（已有提示） | 已有 |
| `system_prompt` | 前置拼接进 prompt（已有处理） | 已有 |

**落地方式**：
- 复用 `visible_when_param` **不适用**（要求**可见**而非隐藏）。
- 新增「网页版忽略标注」机制：参数渲染时检查 `mode == "web"` → 追加灰色后缀。
- Console 侧**已有**次数门控提示（`nodes/local_nodes.cpp:667-670`「本通道不支持 temperature / max_tokens / top_p，已忽略」+ `seed` 能力门控）⇒ 本轮增量只剩**面板灰色后缀**，**不重复造** Console 提示。

**不改动**：参数序列化（仍写入 JSON）—— 只在 UI 层标注。

---

## §4 逐节点方案

### 4.1 N-01 `TextInput` 文本输入

**采纳**：文件导入、多行编辑增强、变量插入
**不采纳**：最近使用 / 片段

| 项 | 设计 | 类型 | 落点 |
|---|---|---|---|
| 文件导入 | 参数面板「从文件导入…」按钮 → 文件对话框 → 读取 `.txt` / `.md` 内容到 `text` | UI + 参数 | `ui/property_panel.cpp` · `utils/file_dialog.*` |
| 多行编辑增强 | 行号显示 / 软换行开关 / 字数统计 | UI | `ui/property_panel.cpp` |
| 变量插入 | 插入 `{var}` 占位提示（与 N-03 联动） | UI | `ui/property_panel.cpp` |

**约束**：
- 文件导入**不覆盖已有内容**前二次确认（避免误操作丢稿）。
- 编码：默认按 **UTF-8** 读取；非 UTF-8 时给可操作提示（不静默乱码）。

**验收**：`VL1-04`（内容读取正确 / 字数统计准确 / `{var}` 占位替换正确）。

### 4.2 N-02 `ImageInput` 图片输入

**采纳**：目录批量、EXIF/元数据、图片预处理（缩放 / 裁剪）
**不采纳**：URL 输入、剪贴板粘贴
**硬约束**：必须是**本地图片**

| 项 | 设计 | 类型 | 落点 |
|---|---|---|---|
| 目录批量 | 选一个目录 → 取所有图片（按扩展名 + 内容嗅探过滤）→ 多值输出 | 参数 | `nodes/local_nodes.cpp` · `utils/file_dialog.*` |
| 元数据 | 参数面板显示尺寸、格式、文件大小（**已有能力**：`utils::sniffImage` / `decodeImageRgba8` 可取） | UI | `ui/property_panel.cpp` · `utils/image_decode.*`（只读） |
| EXIF（可选 · ⚠️ **待验证**） | 全库**零实现**（无 EXIF 解析库命中）。两条路：① 自写 JPEG APP1 / TIFF 头解析（≈200 行 · 零新增依赖）② 新增依赖（**撞「零新增依赖」惯例** ⇒ 需先拍板）—— **成本未评**，本轮可先只做上一行「元数据」 | UI | 待定 |
| 图片预处理 | 节点内参数（见下） | 参数 + 执行器 | `nodes/local_nodes.cpp` · 缩放**复用** `ai/image_convert` · 裁剪**新增**（**不改 WIC 部分，守 I17**） |

**图片预处理参数**：

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `resize_mode` | Enum | `none` | `none` / `fit` / `fill` |
| `resize_max_width` | Int | 0 | 最大宽（0 = 不限制） |
| `resize_max_height` | Int | 0 | 最大高（0 = 不限制） |
| `crop_enabled` | Bool | false | 裁剪开关 |
| `crop_x` / `crop_y` / `crop_w` / `crop_h` | Int | 0 | 裁剪区域 |

**执行顺序**：读取 → 裁剪 → 缩放 → 编码（base64 / 上传）。

**约束**：
- 预处理在**读取后、编码前**执行。
- 结果可缓存（与 `P7a-08` 编码缓存同族，键 = 路径 + mtime + 大小 + 预处理参数）。
- **不触碰** `utils/image_decode.cpp` 的 WIC 部分（守 `I17`）；**缩放已有可复用实现** —— `ai/image_convert`（`source/ai/src/upload/image_convert.cpp:15` `stbir_resize_uint8` 长边缩放 + PNG/JPG 转码，自带 `image_convert_selftest`，计入 `--selftest` 图片准备 **19/0**）⇒ 本轮**抽公共 / 复用**，**不重写**；**裁剪 = 新增**（现有实现无裁剪能力）。
- ⚠️ 复用注意：`image_convert` 的缩放是**站点白名单 / 限额驱动**（上传路径语义），而 N-02 预处理是**用户主动参数** ⇒ 复用其**底层函数**，**不要**复用其判定策略（否则用户参数会被站点限额改写）。

**验收**：`VL1-05`（目录批量收集正确 / 预处理尺寸正确 / 元数据显示正确；EXIF 视拍板）。

### 4.3 N-03 `PromptTemplate` 提示词模板（**重项**）

**新方向**：模板替换 + **AI 生成提示词**

| 项 | 设计 |
|---|---|
| 默认模式 | **manual**（现有行为：`{var}` 替换） |
| AI 生成触发 | **独立按钮「生成提示词」** —— 点击后 AI 生成结果**写入 `template` 输入框** |
| AI 配置 | **复用** N-05 解析逻辑（`resolve_effective_provider` · `engine/src/provider_resolve.cpp:106` + `ai::resolve_api_key` · `ai/include/deepseek_official_provider.h:87/89`） |
| 登录入口 | **复用**现有「网页版会话」区块 |
| `seed` | 只作用于内部 AI 调用 |
| 无参数开关 | 不需要 `mode: manual / ai` 参数；默认 manual，按钮触发 |
| 无需额外连线 | AI 配置全在节点参数内 |

**参数面**：

```
template（模板）
├─ manual：{var} 占位替换
└─ AI 生成：作为「给 AI 的指令」
provider / mode / api_base / model / model_custom
api_key / api_key_ref
网页版会话区（复用 N-05 区块）
system_prompt（引导 AI 生成提示词）
temperature / max_tokens / top_p / top_k / stop
seed（只作用于内部 AI 调用）
stream
```

**执行路径**：

| 模式 | 行为 |
|---|---|
| **manual**（默认） | 现有行为：`{var}` 替换 |
| **AI 生成**（点按钮） | ① 用 `template` 作为「给 AI 的指令」→ ② 调用生效 provider → ③ 用 AI 输出**写入 `template` 输入框** → ④ 用户可继续编辑 → ⑤ 输出走 manual 替换 |

**关键约束**：
- 不新增节点类型。
- 不新增 Provider 逻辑（复用 N-05 解析）。
- `seed` 只作用于内部 AI 调用，**不影响模板替换**。
- 网页版参数标注「忽略」（与 N-06 / N-07 一致）。
- AI 生成失败 → 节点报可操作错误，**不静默回退到 manual**。
- AI 生成写入 `template` 前**先压撤销快照**（可撤销；先例 `M_textio` 现为 📦 已归档 → [Archive/actionPlan/M_textio.md](../Archive/actionPlan/M_textio.md)；现行撤销栈见 `engine`）。

**验收**：`VL1-06`（按钮触发 / AI 输出写入输入框 / `seed` 只作用于 AI / 登录入口可用 / 失败有可操作错误）。
**人工项**：`AL1-02`（手动模式 / AI 模式各跑一次）。

### 4.4 N-04 `TextMerge` 文本合并

**采纳**：排序、前缀后缀、带源标签、去重、空行增强

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `sort_mode` | Enum | `edge_order` | `edge_order` / `by_name` |
| `prefix` | String | `""` | 每段前缀 |
| `suffix` | String | `""` | 每段后缀 |
| `source_label` | Enum | `none` | `none` / `[nX]` |
| `dedup` | Bool | false | 相同内容只留一份 |
| `empty_line_mode` | Enum | `keep` | `keep` / `collapse` / `remove` |

**验收**：`VL1-07`（五项参数行为正确）。

### 4.5 N-05 `ProviderConfig` 提供商配置

**采纳**：连接测试按钮、参数预设、模型列表刷新、后端适配扩展
**不采纳**：本地模型（撞 §十）

| 项 | 设计 | 类型 | 落点 |
|---|---|---|---|
| 连接测试按钮 | ✅ **已落地**（`M_patchAB` 批 2 · 2026-10-11）—— `ui/property_panel.cpp:1277-1332`「诊断（只读 · 异步）」区 + 「测试连接（表校验 + 登录态 / 连通性）」按钮（异步 · 互斥 · 明细折叠）。本轮**仅收口**（如 web 端点一致性补齐），**不重造按钮** | UI | `ui/property_panel.cpp` · 复用 `ai::provider_spec_selftest()` |
| 参数预设 | **内置表 + 用户覆盖**（复用 `providers.json` 机制） | 参数 + 数据 | `assets/providers.json` · `~/.brain-ai/providers.d/` |
| 模型列表刷新 | **从配置表读**，不从 API 拉 | UI | `ui/property_panel.cpp` |
| 后端适配扩展 | 按 `providers.json` 的 `protocol` 扩展 | 数据 | `assets/providers.json` |

**连接测试按钮细节（✅ 批 2 已实现 —— 下列为「现行行为」，供本轮收口核对）**：
- API 条目：有 Key 时发一条 `ping`（「请只回复 pong」）→ 打印 HTTP / 模型 / 耗时。
- web 条目：检查登录态 + 端点一致性，**不发送内容**。
- 退出码语义与 `--provider-selftest` 一致（0 通过 / 1 失败 / 2 缺 Key 或未登录）。
- **异步**，不阻塞界面。

**参数预设细节**：
- 内置表来自 `providers.json`（已落地）。
- 用户覆盖来自 `~/.brain-ai/providers.json` + `providers.d/*.json`（已落地）。
- 下拉选择即带出默认地址 / 模型 / Key 引用名（部分已落地）。

**模型列表刷新细节**：
- 从生效条目的 `models` 字段读（已有）。
- 空数组 = 模型自由填写（已有语义）。

**验收**：`VL1-08`（按钮可用 / 参数预设带出正确 / 模型列表正确）。
**人工项**：`AL1-03`（官方 API + 网页版各测一次 · ✅ 按钮已落地，属**复核**性质）。

### 4.6 N-06 `LLMGenerate` 文本生成

**采纳**：`top_k` / `stop` / 惩罚类 / JSON 输出 / 超时 / 重试 / 多候选 `n`

| 参数 | 类型 | 默认 | 范围 | 网页版 |
|---|---|---|---|---|
| `top_k` | Int | 0 | 0–100 | 忽略 |
| `stop` | StringList | `[]` | — | 忽略 |
| `frequency_penalty` | Float | 0.0 | -2–2 | 忽略 |
| `presence_penalty` | Float | 0.0 | -2–2 | 忽略 |
| `response_format` | Enum | `text` | `text` / `json_object` | 忽略 |
| `n` | Int | 1 | 1–8 | 忽略 |
| `timeout_connect_ms` | Int | 0 | 0 = 用全局 | — |
| `timeout_first_byte_ms` | Int | 0 | 同上 | — |
| `timeout_stream_idle_ms` | Int | 0 | 同上 | — |
| `retry_enabled` | Bool | （继承） | — | — |
| `retry_count` | Int | 0 | 0 = 用全局 | — |
| `retry_interval_ms` | Int | 0 | 0 = 用全局 | — |

**JSON 输出细节**：
- **先实现再验证**（豆包 / DeepSeek `response_format`）。
- 请求体带 `response_format: {type: "json_object"}`。
- 后端不支持时 → 标注「忽略」+ Console 提示（不静默失败）。

**多候选 `n` 细节**：
- 输出形态：`text` 端口输出**第一个**；新增 `texts` 端口输出**全部**（变长）。
- 或：输出 JSON 数组 —— **待实施时确认**（写进实施记录）。

**验收**：`VL1-09`（请求体构造正确 / 超时重试生效 / `n` 输出正确）。
**待验证**：豆包 / DeepSeek `response_format` 支持度（实施后实测）。

### 4.7 N-07 `VLMGenerate` 图片理解

**采纳**：`seed` / `mode` / 图片预处理 / 多图顺序 / 描述缓存

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `seed` | Int | 0 | 与 N-06 对称 |
| `mode` | Enum | `official`（⚠️ N-06 现状默认 = `web` · 见 §5.1 注） | 与 N-06 对称；网页版**已支持**图片理解（`M8-14`） |
| 图片预处理 | 复用 N-02 的预处理参数 | — | 缩放 / 裁剪 |
| `image_order` | Enum | `edge_order` | `edge_order` / `by_name` |
| 描述缓存 | （执行器） | 开启 | 同图不重复理解 |

**`mode` 细节（批 9-0 纠错 · 现状已变）**：
- ⚠️ **v1 旧表述作废**：本稿 v1 写「网页版仍报『暂不支持图片』」—— **已不成立**。`M8-14`（`M8.md` · 2026-10-11）已落地**网页版图片理解**：`nodes/local_nodes.cpp:724` 起「**先上传，再发送**」全链路（上传 → 双证据 `I18` → 注入 → 发送 → 轮询取答）。
- 现状 `VLMGenerate` 的**注册表参数面**（`engine/src/node_registry.cpp:401-406`）**没有** `mode` / `seed` ⇒ 本轮新增的是**参数面**，**不是**"打开网页版能力"（能力已在）。
- 新增 `mode` 后语义：`official` / `web`；web 分支**已可工作**（复用现成 web 链路），与 official 的差异只在**参数忽略矩阵**（§3.4）。
- 与 N-06 对称仍成立（同属"公共参数面"成员）。

**描述缓存细节**：
- 键 = 路径 + mtime + 大小 + 预处理参数 + 提示词 + 模型。
- 与 `P7a-08` 编码缓存**同族**，但**缓存内容不同**（那是编码缓存，这是理解结果缓存）。
- 缓存命中时 Console 记录「命中缓存」。
- 缓存**只存内存**（不落盘，避免隐私与磁盘问题）。

**验收**：`VL1-10`（`seed` / `mode` 对称 / 预处理正确 / 多图顺序正确 / 缓存生效）。

### 4.8 N-08 `TextOutput` 文本输出

**采纳**：导出模板、批量导出、自动命名、归档策略参数化
**不采纳**：更多导出格式

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `export_template` | Text | `""` | 元信息头自定义（空 = 默认） |
| `batch_export` | Bool | false | 一次导出多节点 |
| `file_name` | String | （已有） | 模板化文件名（增强） |
| `archive_policy` | Enum | `inherit` | `inherit` / 覆盖 `config.output.*` |

**批量导出细节**：
- `batch_export = true` 时，导出当前工作流**所有有输出的节点**到同一目录。
- 每个节点一份文件，文件名 = `file_name` 模板 + 节点 id。
- **待实施时确认**（写进实施记录）。

**归档策略参数化细节**：
- `archive_policy = inherit` 时用 `config.output.*`（已有）。
- 覆盖时节点参数优先。

**验收**：`VL1-11`（导出模板正确 / 批量导出可用 / 自动命名规则正确 / 归档策略生效）。

---

## §5 公共参数面规范

### 5.1 推理节点公共参数

| 参数 | 类型 | 默认 | 范围 | 网页版 | N-06 | N-07 |
|---|---|---|---|---|---|---|
| `system_prompt` | Text | `""` | — | 前置拼接 | ✅ 已有 | ✅ 已有 |
| `temperature` | Float | 0.7 | 0–2 | 忽略 | ✅ 已有 | ✅ 已有 |
| `max_tokens` | Int | 2048 | 1–8192 | 忽略 | ✅ 已有 | ✅ 已有 |
| `top_p` | Float | 1.0 | 0–1 | 忽略 | ✅ 已有 | ✅ 已有 |
| `top_k` | Int | 0 | 0–100 | 忽略 | ➕ 新增 | ➕ 新增 |
| `stop` | StringList | `[]` | — | 忽略 | ➕ 新增 | ➕ 新增 |
| `frequency_penalty` | Float | 0.0 | -2–2 | 忽略 | ➕ 新增 | ➕ 新增 |
| `presence_penalty` | Float | 0.0 | -2–2 | 忽略 | ➕ 新增 | ➕ 新增 |
| `seed` | Int | 0 | 0–1e9 | 忽略 | ✅ 已有 | ➕ 新增 |
| `stream` | Bool | true | — | 支持 | ✅ 已有 | ✅ 已有 |
| `mode` | Enum | `official`（⚠️ N-06 现状默认 = `web` · 用户拍板 ③A = **保持现状**（`NE-D38`），本列仅为目标对称口径） | — | 支持 | ✅ 已有（默认 `web` · 见下注） | ➕ 新增 |

**落地方式**：`engine::inference_common_params()`（新增）；N-06 / N-07 调用同一份。

**⚠️ `mode` 默认值现状（批 9-0 实测 · 2026-10-12 用户拍板 ③A = 保持现状 · `NE-D38`）**：N-06 现状默认 = **`web`**（`engine/src/node_registry.cpp:371`），**不是**本稿 v1 写的 `official`。开发者决定**保持现状、零行为变更**：**不再把 N-06 默认值改为 `official`**；L1 只把 `mode` 参数面补齐到 N-07（对称性解决），两处默认值**暂不统一**（N-05 = `official` · N-06 = `web`）。

### 5.2 参数二级分组

| 分组 | 参数 | 默认 |
|---|---|---|
| **采样** | `temperature` · `top_p` · `top_k` · `frequency_penalty` · `presence_penalty` · `seed` | 折叠 |
| **输出** | `max_tokens` · `stop` · `stream` · `mode` · `system_prompt` | 展开 |
| **高级** | `timeout.*` · `error.*` 覆盖项 | 折叠 |

### 5.3 超时 + 重试配置

| 字段 | 用途 | 默认 | 节点覆盖参数 |
|---|---|---|---|
| `timeout.connect_ms` | 连接超时 | 10000 | `timeout_connect_ms` |
| `timeout.first_byte_ms` | 首字节超时 | 30000 | `timeout_first_byte_ms` |
| `timeout.stream_idle_ms` | 流式空闲超时 | 300000 | `timeout_stream_idle_ms` |
| `error.retry_enabled` | 重试开关 | true | `retry_enabled` |
| `error.retry_count` | 重试次数 | 1 | `retry_count` |
| `error.retry_interval_ms` | 重试间隔 | 2000 | `retry_interval_ms` |

**覆盖规则**：节点参数 `0` / 空 = 用全局配置；非零 = 节点优先。

---

## §6 实施顺序

| 批次 | 内容 | 依赖 | 理由 |
|---|---|---|---|
| **L1-a** | 公共参数面抽象 + 参数二级分组 + 超时/重试（**节点级覆盖 + web 线总超时**） + 网页版参数标注 | 无 | 横向能力，先建地基 |
| **L1-b** | N-04 / N-05 / N-06 / N-07 / N-08 参数补齐 | L1-a | 纯参数级 |
| **L1-c** | N-01 文件导入 + N-02 目录批量/预处理 | 无 | 输入侧能力 |
| **L1-d** | N-03 AI 生成提示词（**重项**） | L1-a + L1-b | 依赖 Provider 复用 + 推理参数 |
| **L1-e** | JSON 输出（先实现再验证豆包 / DeepSeek） | L1-b | 需实测 |
| **L1-f** | N-05 参数预设 + 模型列表刷新（**连接测试按钮已落地，仅收口**） | L1-b | UI 侧 |

**批间约束**：
- 每批独立可验证、可回滚。
- 每批跑 §7.3 回归基线并贴数字。
- L1-d **不单独排期**（`NE-D6`），与其他项同批推进。

**实施记录（2026-10-14 · 四个代码批 · 对应 [../CHANGELOG.md](../CHANGELOG.md)）**：

| 批 | 内容（落地） | 验证（本机实测） |
|---|---|---|
| **L1-a** 横向地基 | 公共参数面抽象 + 参数二级分组 + 超时/重试节点级覆盖 + 网页版参数标注 | build 0/0 · graph **110 → 144 / 0**（+34 = `VL1-01` 对称差集等）· exec 546/0 |
| **L1-b** 参数补齐 | N-04 五项 / N-08 导出模板·批量导出·自动命名 / 请求体传递 / N-07 多图顺序 | exec **546 → 585 / 0**（+39 = `VL1-07`×9 · `VL1-09`×5 · `VL1-10`×2 · `VL1-11`×3）· graph 144/0 · provider 52/0 |
| **L1-c** 输入侧 | N-01 文件导入（UTF-8 读取 / 二次确认）+ N-02 目录批量 / 图片预处理（新模块 `ai/image_preprocess`）+ N-07 预处理复用 | exec **546 → 592 / 0**（+46 = image_preprocess 6 项 + `VL1-05` 参数面 + 既有）· graph 144/0 |
| **L1-d/e/f** 收尾 | N-03 AI 生成提示词（异步 · 新模块 `prompt_generator`）+ JSON 输出 / 多候选（`response_format` / `n`）+ N-05 参数预设 / 模型列表刷新 | exec **546 → 598 / 0**（+6 = `VL1-06`×2 · `VL1-08`×2 · `VL1-09`×2）· graph 144/0 · 断链 **1375 / 0** · check_all 15/15 |

> **至此 L1 六批（a~f）全部完成**，离线断言全过。人工验收项（`AL1-01`…`AL1-06`）由**用户 2026-10-15 完成**（§7.2 + [../audit/L1_checklist.todo](../audit/L1_checklist.todo)）。后续 L2/L3 批（2026-10-15）在此基础上继续推进，全库终值 = graph **151 / 0** · exec **655 / 0** · 断链 **1487 / 0**（本文 §7.3 记 L1 收尾值）。

---

## §7 验收框架

### 7.1 离线断言（`VL1-*`）

| 编号 | 内容 | 落点 | 结果（2026-10-14 实测） |
|---|---|---|---|
| `VL1-01` | 公共参数面：N-06 / N-07 参数集一致（对称差集为空） | `--graph-selftest` | ✅ graph **110 → 144 / 0**（L1-a · +34 含对称差集） |
| `VL1-02` | 参数二级分组：分组 id 与参数归属 | `--graph-selftest` | ✅ graph 144/0（L1-a） |
| `VL1-03` | `timeout.*` / `error.*` 配置读取 + 行为生效 | `--exec-selftest` | ✅ 计入终值 598/0（L1-a） |
| `VL1-04` | N-01 文件导入内容正确 / 字数统计准确 | `--exec-selftest` | ✅ 计入终值 598/0（L1-b） |
| `VL1-05` | N-02 目录批量 / 预处理尺寸 / 元数据（EXIF 视拍板） | `--exec-selftest` | ✅ exec 592/0（L1-c · image_preprocess 6 项 + 参数面） |
| `VL1-06` | N-03 `seed` 只作用于 AI 调用，不影响模板替换 | `--exec-selftest` | ✅ exec 598/0（L1-d/e/f · +2） |
| `VL1-07` | N-04 五项参数行为正确 | `--exec-selftest` | ✅ exec 585/0（L1-b · +9） |
| `VL1-08` | N-05 参数预设带出 / 模型列表 | `--provider-selftest` | ✅ exec 598/0（L1-d/e/f · +2） |
| `VL1-09` | N-06 请求体构造 / 超时重试 / `n` 输出 | `--exec-selftest` | ✅ exec 585 → 598/0（L1-b ×5 · L1-d/e/f response_format/n ×2） |
| `VL1-10` | N-07 `seed` / `mode` 对称 / 预处理 / 多图顺序 / 缓存 | `--exec-selftest` | ✅ exec 585/0（L1-b · +2） |
| `VL1-11` | N-08 导出模板 / 批量导出 / 自动命名 | `--exec-selftest` | ✅ exec 585/0（L1-b · +3） |
| `VL1-12` | 网页版参数标注存在 | `--graph-selftest` | ✅ graph 144/0（L1-a） |

### 7.2 人工验收（`AL1-*`）

| 编号 | 内容 | 说明 | 结果（2026-10-15 · 用户） |
|---|---|---|---|
| `AL1-01` | 参数二级分组可折叠 | 面板分组 | ✅ 已人工验收 |
| `AL1-02` | N-03 AI 生成提示词（手动 / AI 各一次） | **重项单列** | ✅ 已人工验收 |
| `AL1-03` | N-05 连接测试按钮（官方 API + 网页版各一次） | ✅ 按钮已落地（批 2）· 本项为**复核** | ✅ 已人工验收 |
| `AL1-04` | 超时 + 重试（断网 / 超时场景） | | ✅ 已人工验收 |
| `AL1-05` | N-02 预处理（缩放 / 裁剪各一次） | | ✅ 已人工验收 |
| `AL1-06` | N-01 文件导入（`.txt` / `.md`） | | ✅ 已人工验收 |

> 逐项记录与物证位见 [../audit/L1_checklist.todo](../audit/L1_checklist.todo)（2026-10-15 用户全项勾选）。

### 7.3 回归基线（每批必跑）

| 命令 | 期望（**L1 收尾实测基线 · 2026-10-14**） |
|---|---|
| `source\build.ps1` | 0 error / 0 warning（四批实跑 = **0 / 0**） |
| `api_probe --graph-selftest` | **144 / 0**（L1-a 由 **110 → 144** · 只升不降） |
| `api_probe --exec-selftest` | **598 / 0**（链：546 → 585 → 592 → 598 · 只升不降） |
| `api_probe --selftest` | **十三组 PASS** ⚠️ 不是七组 —— 组名见末行「结果: …」（四批实测 13 个组名全 PASS） |
| `aiwrite --provider-selftest` | **52 / 0**（实测末行 `[配置表自检] 52 项通过 / 0 项失败` · exit 0） |
| `aiwrite --run-selftest` | PASS ⏸ 未复跑（沿用 [../../AI_README.md](../../AI_README.md) §2 权威位） |
| `aiwrite --run-selftest --web` | 5 / 5 ⏸ 未复跑（同上） |
| `python source\tools\check_links.py` | **checked 1375 / broken 0 · exit 0**（批 9-0 基线 1246 → … → 批 9-3 1341 → 批 9-4 1349 → 批 9-5 1350 → **L1-d/e/f 批 +25 = 1375**） |
| `source\tools\check_all.ps1` | **15 / 15 PASS · exit 0**（L1 四批物证 `build\logs\check_all_20261015_010021.log` 等） |

> **后续推进**：L2/L3 批（2026-10-15）将全库推到 graph **151 / 0** · exec **655 / 0** · 断链 **1487 / 0**（见 [M_node_expansion_part2.md](M_node_expansion_part2.md) §12 v6 · 本文记 L1 收尾值）。

**口径**：自检数字**只升不降**（[M_node_expansion.md](M_node_expansion.md) §2.5 c）；若确需下降 ⇒ 按 `PQ-05` **显式登记（数字 + 原因成对）**。

---

## §8 约束面核对

| 约束 | 状态 | 说明 |
|---|---|---|
| §十 不做清单 | ✅ 不撞 | 节点数已由 `PQ-20` 解除固定；本地模型、图片输出节点不涉及 |
| §4.4 节点清单 | ✅ 不撞 | **不新增节点类型** |
| §19.1 延后清单 | ✅ 不撞 | 14 项不含参数级增强 |
| 冻结区 `I17` | ✅ 不涉及 | `I17` = 冻结 `utils/src/image_decode.cpp` 的 WIC 部分（CRT 断言崩溃根因）；缩放复用 `ai/image_convert`（stb）· 裁剪新增**独立函数**，不碰 WIC |
| 不变量 `I18` | ✅ 不**放宽** | `I18` = 网页版图片「无上传完成证据不得自动发送」；预处理在**上传前**发生，不改双证据链路 |
| 不变量 `I19` | ✅ 不涉及 | `I19` = `image` 端口语义向后兼容；不碰 `image` 端口语义 |

**图片预处理与 `I18` 的关系（批 9-0 补）**：预处理 = **本地变换**（读取 → 裁剪 → 缩放 → 编码）→ 上传 → 双证据齐备 → 才注入 / 发送；**不得**因预处理失败而走"降级为不含图片继续"（该降级**只能由用户本次运行显式选择** —— 口径同 [../roadmap.md](../roadmap.md) 第 67 行）。

**不新增节点类型 ⇒ 不触碰** `api_probe.cpp:1936`（`registry.size() == "8"`）/ `:1938`（`expected_types[]`）/ `:1953-1959`（分类计数）的节点数硬断言。

---

## §9 明确不做

| 项 | 理由 |
|---|---|
| URL 输入（N-02） | 成本高于预估；必须是本地图片 |
| 剪贴板粘贴（N-02） | 用户明确不需要 |
| 最近使用 / 片段（N-01） | 用户明确不需要 |
| 更多导出格式（N-08） | 用户明确不需要 |
| 本地模型 | 撞 §十「永远候选」 |
| 模板库（N-03） | 被 AI 生成提示词取代 |
| 条件分支 / 循环 / 子工作流 | 属引擎 v2 议题（[M_node_expansion.md](M_node_expansion.md) §5） |
| 节点库 / 画布 UX（L3） | §19.1 已延后 |
| 新增节点类型（L2） | 本次定向为 L1 |

---

## §10 落档动作

| 动作 | 目标 | 状态 |
|---|---|---|
| 本文建立 · **归位** | `docs/actionPlan/M_8nodes_extension.md`（📌 从 `nodes_expansion/` 挪回 `actionPlan/` 根 · 与 `M_node_expansion.md` 平级 —— **归位后断链 100 → 0**） | ✅ 批 9-0 |
| 修订本文 → **v1.1** | 纠错 P1-1…P1-6 + P2-1/P2-2/P2-4/P2-5/P2-6/P2-8（逐条见 §11 修订清单） | ✅ 批 9-0 |
| 更新 `AI_README.md` | §5 编号空间登记 `L1-*` / `VL1-*` / `AL1-*` | ✅ 批 9-0 |
| 更新 `DocMAP.md` | 文档清单 **+1 行** + 「最近更新」行（**无「文档总数」计数位** ⇒ 不涉及计数） | ✅ 批 9-0 |
| 更新 `CHANGELOG.md` | 批 9-0 条目 | ✅ 批 9-0 |
| 更新 `变量术语对照表.md` | 收录不变量 `I17` / `I18` / `I19` | ✅ 批 9-0 |
| 登记 `DevPlan.todo`（**UI 侧 · 用户亲自**） | 新增 L1 任务号 `L1-a…L1-f`（选 a–f 即启用） | ⬜ 待用户 |
| 更新看板卡 `DOCS-NE-01`（**UI 侧 · 用户亲自**） | 卡片正文 / 状态 | ⬜ 待用户 |
| 更新 `roadmap.md` | §六 滚动发布「补强」栏 | ⬜ **未同步**（实施批未覆盖；L2/L3 批文档同步亦未加 L1 补强行 —— 归档前建议补） |
| 更新 `ai_writer_nodes.md` | §4.4 / §9.2 节点参数面 | ⬜ **未同步**（L2/L3 批已补 N-09~N-14 与 L3 UX；L1 参数面增量未写 —— 归档前建议补） |
| 更新 `节点编辑器使用说明.md` | 节点表 + 人工验收清单 | ⬜ **未同步**（§10.12 画布性能已落地；L1 节点参数 / L3 UX 未写 —— 归档前建议补） |

> **本批不做**：不改 `source/**` · 不改 `DevPlan.todo` JSON · 不进批 11 · 不追溯改写历史数字。

---

## §11 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-10-12 | v1 | **建立本文（方案定稿）**：上游 [M_node_expansion.md](M_node_expansion.md) L1 定向后，逐节点方案 + 4 项横向能力 + 公共参数面 + 实施顺序 + 验收框架；`NE-D2`~`NE-D13` 录入 §2；**零代码改动**。 |
| 2026-10-12 | **v1.1** | **批 9-0 收口（纯文档 · 零代码）**：① **归位** `docs/actionPlan/`（断链 **100 → 0** · 归位后 checked **1219 / 0** → 索引同步后 **1246 / 0** → 批 9-1 复检 **1283 / 0**）；② 纠正 **6 项与代码不符的前提**（P1-1…P1-6）；③ 修正 **9 项数字 / 口径**（P2-1…P2-9）；④ 登记 `L1-*` / `VL1-*` / `AL1-*`。逐条见 **§12**。 |
| 2026-10-12 | **v1.2** | **批 9-1 口径同步（纯文档 · 零代码）**：上游 [M_node_expansion.md](M_node_expansion.md) 已升 **v5 实施依据（四层全部定向）** ⇒ 本文改三处口径：① 文首「上游依据」注明 v5；② §0.1 删「上游是**讨论稿**（摊开四层、无推荐）」→ 改「上游现为**实施依据文档**（四层全部定向）· L1 细节由本文承接」；③ §2 前言补 **两文 `NE-D` 编号分工**（本文 `NE-D1`~`NE-D13` = L1 细节；上游同名号另有全局含义，其 L1 细节别名已改号 `NE-D30`~`NE-D37`）。同时文首编号空间行同步。 |
| 2026-10-12 | **v1.3** | **批 9-2 用户拍板落盘（纯文档 · 零代码）**：上游 [M_node_expansion.md](M_node_expansion.md) 升 **v6** —— ① `NE-D38`（`mode` 默认值保持现状）② `NE-D39`（`L2-01…` / `L3-01…` 任务号先立）；本文 §5.1 注已改为「**不再改为 `official`**（用户拍板 ③A · 零行为变更）」、§5.1 表 `mode` 默认单元格同步。 |
| 2026-10-14 | **v1.4（实施完成 · 代码）** | **L1 六批（a~f）全部落地**（四个代码批，见 §6 实施记录）：graph **110 → 144 / 0** · exec **546 → 598 / 0** · 断链 **1375 / 0** · check_all **15/15** —— 对应 [../CHANGELOG.md](../CHANGELOG.md) 四个 `M_8nodes_extension` L1 批次。 |
| 2026-10-15 | **v1.4 收口（纯文档）** | **文档状态同步 + 人工验收回填**：① 顶部状态 🟡 未开工 → ✅ 已实施完成 + 人工验收完成；② §7.1 `VL1-*` 逐项回填结果；③ §7.2 `AL1-*` → ✅ 用户人工验收（2026-10-15）；④ §7.3 基线更新至 L1 终值；⑤ §10 落档动作如实标注（`DevPlan.todo` / 看板卡 / roadmap / ai_writer_nodes / 使用说明 仍 ⬜ 待做）；⑥ [../audit/L1_checklist.todo](../audit/L1_checklist.todo) ⬜ → ✅（用户全项勾选）。 |

---

## §12 v1.1 修订清单（批 9-0 逐条可核）

> 口径：**只改文档、不改代码**；每条给出「v1 原表述 → 实测 → 本文落点」。实测均为 2026-10-12 批 9-0 现场复核（文件 + 行号可核）。

### 12.1 红灯与前提纠错（P0 / P1）

| 编号 | v1 原表述 | 实测（批 9-0） | 本文落点 |
|---|---|---|---|
| **P0-1** | （标题）落档 `docs/actionPlan/M_8nodes_extension.md`，但实际先落在 `nodes_expansion/` 子目录 | 子目录内相对层级未 +1 ⇒ 全库 **broken 100**（被移文件自身 69 + 新文档 14 + 外部 17；同行 = checked **1219**）；挪回 `actionPlan/` 根后 **1219 / broken 0**，批 9-0 索引同步后 **1246 / broken 0** ⇒ 批 9-1 复检 **1283 / broken 0** | 文件名 · §10 |
| **P1-1** | 节点数硬断言在 `api_probe.cpp:803/805` | 真身 = `:1936`（`registry.size() == "8"`）/ `:1938`（`expected_types[]`）/ `:1953-1959`（分类计数）；`:803/805` 属 `resolve_proxy` 区。现有参数断言（`:1966-1985`）只查**存在 + 范围**，**无参数个数断言** | §1.3 · §8 |
| **P1-2** | 「`timeout.*` / `error.*` 已有字段但**未接线**」· 接线点 = `engine/executor.cpp` | 官方 API 线**已接线**（批 4：`api_probe.cpp:1489-1491` · `config.cpp:44-46`）；真实对象 = **web 线硬编码**（`dom_web_client.h:44` 180000 · `upload_registry.cpp:39` 15000 等）；重试消费点 = `local_nodes.cpp:506-541`，**不在** `executor.cpp` | §3.3 |
| **P1-3** | 「N-05 连接测试按钮」列为**待做** | 批 2 **已落地**（`property_panel.cpp:3` · `:1277-1332` 诊断区）⇒ 降级为「收口 / 复核」 | §4.5 · §6 · §7.2 · §7.1 |
| **P1-4** | 「网页版图片**不支持**，报『暂不支持图片』」 | **相反**：`M8-14` 已落地网页版图片理解（`local_nodes.cpp:724` 起「先上传，再发送」全链路）；真实缺口 = `VLMGenerate` 参数面缺 `mode` / `seed`（`node_registry.cpp:401-406`） | §4.7 · §1.3 |
| **P1-5** | 符号 `resolve_official_key` | **不存在**；真身 = `ai::resolve_api_key`（`deepseek_official_provider.h:87/89`） | §2 `NE-D3` · §4.3 |
| **P1-6** | 缩放 / 裁剪都按「新增」处理 | 缩放**已有**（`ai/image_convert` · `stbir_resize_uint8` · `image_convert_selftest` 计入 `--selftest` 图片准备 **19/0**）⇒ 改「复用」；**裁剪 = 新增**；**EXIF 零实现** ⇒ 标「待验证 + 成本未评」 | §4.2 · §8 |

### 12.2 数字与口径订正（P2）

| 编号 | v1 原表述 | 实测（批 9-0） | 本文落点 |
|---|---|---|---|
| **P2-1** | `--selftest`「**七组** PASS」 | **十三组**（`api_probe.cpp:5858-5866`） | §7.3 |
| **P2-2** | `--provider-selftest`「**50 / 0**」 | **52 / 0** | §7.3 |
| **P2-3** | 未说明 `DocMAP.md` 登记方式 | `DocMAP.md` / `AI_Status.md` **无「文档总数」计数位** ⇒ 归位只需 **+1 行** + 改「最近更新」行 | §10 |
| **P2-4** | `mode` 默认 = `official` | 现状 = **`web`**（`node_registry.cpp:371`）⇒ 改默认值 = **行为变更**，**待拍板**（候选 `NE-D14`） | §5.1 · §4.7 |
| **P2-5** | 参数二级分组按「从零落地」 | 已有**一级**「参数」`CollapsingHeader`（`property_panel.cpp:1053-1055` · P7a-15）⇒ 本轮是**组内加二级** | §3.2 |
| **P2-6** | Console「**首次**遇到被忽略参数时提示一次」需新增 | 已有**次数门控**提示（`local_nodes.cpp:667-670`）⇒ 增量只剩**面板灰色后缀** | §3.4 |
| **P2-7** | `I17` / `I18` / `I19` 定义正确但**未收录**术语表 | 本批收录（`变量术语对照表.md`） | §8 · §10 |
| **P2-8** | 看板号 `NE-01…` | 现卡名 = **`DOCS-NE-01`**（`NE-01…` 任务号待启用） | 文首 · §10 |
| **P2-9** | 文首称 `L1-*` / `VL1-*` / `AL1-*`「**已登记**」 | **未登记**（自相矛盾）⇒ 本批登记 | 文首 · §10 |

### 12.3 已核对**无冲突**（白名单 · 供实施时放心引用）

8 节点 / 5 端口 / 不新增类型**不撞**既有冻结区 · `VL1-01` / `VL1-02` / `VL1-12` 落点 `--graph-selftest` 正确（`api_probe.cpp:1923`）· `NE-D2…NE-D13` **不撞号** · `resolve_effective_provider`（`provider_resolve.cpp:106`）· `provider_spec_selftest`（`api_probe.cpp:5133`）· `assets/providers.json` · `providers.d` · `utils/file_dialog` · `visible_when_param` / `visible_when_value` **均存在**。

---

