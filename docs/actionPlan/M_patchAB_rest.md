# M_patchAB_rest：补丁系列剩余工作总集（A/B 残项 + C 数据安全 + D 交互打磨 + PM + B2 残项）

> 类型：跨里程碑「地基补丁」**剩余项承接文档**（不占用 M1–M6 编号；与前两期计划平级互链）
> **由来（2026-09-27 文档拆分）**：`M_patchA` 的 **Patch A 全部完成 ✅**、`M_patchB` 的 **L1 收口 + L3 + L4 部分落地** —— 两份计划里的**已完成部分已归档**：
> [../Archive/actionPlan/M_patchA.md](../Archive/actionPlan/M_patchA.md) · [../Archive/actionPlan/M_patchB.md](../Archive/actionPlan/M_patchB.md)。
> **两个文件里所有未完成项**收编到本文件 ⇒ **本文件是补丁系列后续唯一执行入口**（任务看板仍是 [../DevPlan.todo](../DevPlan.todo)）。
> **编号不改号**：`PA-` / `PB-` / `PC-` / `PD-` / `PM-`（第一期）与 `PB2-*`（第二期）沿用原编号，验证（`VA-/VB-/VC-/VD-/VM-/VB2-*`）与验收（`AA-/AB-/AC-/AD-/AM-/AB2-*`）同理 —— **只换归口文档，不重新编号**，历史引用与源码注释继续有效。
> **原规格逐字可查**：逐条证据、历史决策、实测记录都在**归档文档**里；本文只写「**要点摘要 + 依据指针 + 验证/验收 + DevPlan 归口**」，避免双份维护（附录除外，见 §8）。
> 状态：🟡 **进行中** —— 待你拍板：`D-25` / `D-26` / `D-27` / `D-28` / `D-29` / **`D-30`**（§9）
> 回归基线：见 **§0.4**（2026-09-27 实测刷新）

---

## §0 文档元信息与约定

### 0.1 定位

补丁系列（A–D + PM）与 `PB2-*` 展开计划都走到了「**主体已完成、剩尾项**」的状态：

- **收编范围**：`M_patchA` §4.1（B 残项）/ §5.1（C）/ §6.1（D）/ §7（PM）中所有非 ✅ 条目 + `M_patchB` §3（`PB2-04`…`PB2-12` 的 L2、`PB2-07` 界面按钮、`PB2-25`、`PB2-29`/`PB2-30②`、`PB2-24`）；
- **不在范围内**（已完成，归档留存）：Patch A1/A2 全部、`PB-01`/`PB-03`/`PB-05`/`PB-06`(凭据库)/`PB-08`、`PC-05`/`PC-06`、`PD-02`/`PD-04`/`PD-05`/`PD-06②`、`PM-02`、`PB2-01`…`PB2-03`/`PB2-06`、`PB2-17`…`PB2-20`、`PB2-21`(文档侧)/`PB2-22`/`PB2-23`、`PB2-26`、`PB2-27`/`PB2-28`、`PB2-30①`、**`PB2-31`（v16 提供商下拉合并显示，经用户界面验证通过）**。

### 0.2 归口映射（旧文档 ⇒ 本文）

| 原位置 | 本文位置 | 说明 |
|---|---|---|
| `Archive/actionPlan/M_patchA.md` §4.1（PB-xx 残项） | **§2** | Patch B 残项 |
| `Archive/actionPlan/M_patchA.md` §5.1 / §5.2 / §5.3（PC-xx） | **§4** | 原 Patch C · 数据安全 |
| `Archive/actionPlan/M_patchA.md` §6.1 / §6.2 / §6.3（PD-xx） | **§5** | 原 Patch D · 交互打磨 |
| `Archive/actionPlan/M_patchA.md` §7（PM-xx） | **§6** | 并行小项 |
| `Archive/actionPlan/M_patchA.md` §1.8 / §12（不做与决策） | **§7 / §9** | 明确不做 + 决策点 |
| `Archive/actionPlan/M_patchB.md` §3 L2（PB2-08…12）+ `PB2-04`/`PB2-05` 残余 | **§3.1** | 协议 / 适配器实现（工厂） |
| `Archive/actionPlan/M_patchB.md` §3 `PB2-07` / `PB2-25` / `PB2-29` / `PB2-30` / `PB2-24` | **§3.2 – §3.5** | L1 尾项 + L4（**网页版会话失效可诊断 / 逐站选择器回填**）—— ⚠️ `PB2-25`、`PB2-28`（探测适用性）、`PB2-29`（逐站回填）**将并入 [M7B.md](../Archive/actionPlan/M7B.md)**（2026-09-28）：`PB2-25` → `M7B-07`/`M7B-22`（证据来源改 CDP）；`PB2-28` → **作废**（`I16` 升级为全局「不注入任何站点内部端点」）；`PB2-29` → `M7B-29`（在 **Pydoll 通道上一次测完**，不在 WebView2 里重复测） |
| `Archive/actionPlan/M_patchB.md` §7 附录 B/C/D/E | **§8**（**现行版本**） | JSON 规范 / 加载顺序 / 自建站点 / 站点清单 |

### 0.3 执行原则（承接，不改语义）

沿用 `M_patchA` §0.3 五条（**运行态值不进文档** / **只读暴露** / **脱敏** / **一次提交一个补丁** / **不引入新依赖**）+ `M_patchB` §0.4 追加三条（**表数据优先于 C++ 常量** / **不静默回落** / **不实测不填值**），并守全部不变量 **`I1`…`I16`**（其中 `I2`「`deepseek-web` 与无参路径行为逐字不变」、`I15`「登录态判据不得依赖 `userToken`」、`I16`「探测不适用站点不得注入 DeepSeek 端点」在 L4 残项里最关键）。

### 0.4 回归基线（**2026-10-09 实测 · 2026-10-10 复测订正** · 每次提交必须重跑并贴结果）

| 命令 | 期望（当前实测） |
|---|---|
| 构建（MSVC /W4，4 目标） | **0 error / 0 warning** |
| `api_probe.exe --selftest` | 七组全 PASS · exit 0（含 **DOM 探测 27 / 0** · 执行器 **490 / 0**） |
| `api_probe.exe --graph-selftest` | **110 通过 / 0 失败** |
| `api_probe.exe --exec-selftest` | **490 通过 / 0 失败**（**2026-10-11 复测**；2026-10-10 曾为 **464**，**+26 = M_patchC 批 1**：窗口表 / 复位计划 / 归属准入 / 三类归因文案的纯函数断言） |
| `aiwrite.exe --provider-selftest` | **50 通过 / 0 失败** |
| `aiwrite.exe --provider-dump` | **20 条**（official 9 / web 11） |
| `aiwrite.exe --cred-selftest` | **8/8 PASS**（本轮未复跑） |
| `aiwrite.exe --export-selftest` | **7/7 PASS**（本轮未复跑） |
| `aiwrite.exe --web-session-selftest` | PASS（会话自动引导：窗口已开 → 补探测） |
| `aiwrite.exe --web-adapter-selftest --provider <id>` | 逐站命中数可达（诊断；有缺项时退出码 1） |
| `aiwrite.exe --web-adapter-selftest --provider all --timeout 30 --json <报告>` | 10 站一次跑完 · 退出码 **0 / 1 / 3 / 2**（`FEA-L01`/`FEA-L03` · 2026-10-09 实测：可用 **8** · 需登录 **2** · 失败 **1** · 未测 0 · **exit 1**） |
| `aiwrite.exe --web-session-list [--json <路径>]` | 内存会话 / L2 快照元数据 / 网页版条目 **11 条** · **exit 0**（有没有会话都算成功 · `FEA-L02`） |
| `aiwrite.exe --web-dom-dump --provider <id>` | 只读枚举候选元素（v15 新增；**11/11 条目直连各自站点**） |
| `aiwrite.exe --run-selftest` | PASS（离线无 Key：**3/5 为预期**） |
| `aiwrite.exe --run-selftest --web --provider <id>` | **exit 0 = 生成成功**（需该条目选择器就绪 + 会话有效） |
| `aiwrite.exe --run-selftest --web`（内置 DeepSeek 路径） | 5/5 —— ⚠️ 受官网侧会话失效（`R19`）影响时需先解决 `PB2-25`/重登 |
| 离线夹具 `node source/tools/*_test.js` | `poll_answer_gate_test` **44 / 0** · `send_chain_test` **17 / 0** · `dom_gate_test` **15 / 0**（各加 `--old` 负向**必须 FAIL**：**18 / 26** · **13 / 4** · **9 / 6** · 退出码 1） |
| 文档断链自检 `python vcpkg-cache/check_links.py` | **全库 0** —— **2026-10-11 实测 checked 1097 / broken 0**（同日四批：批 0 = 995 → `M8` Phase 3 改判批 = 1022 → 待拍板项清零批 = 1077 → **`M_patchC` 批 1 = 1097**（+17）；存量 8 条已清零：7 条真断链 = `actionPlan/M7B.md` 未指向 `Archive/`，1 条 = 检查器跨反引号误判；判据由「本轮不得新增」**升级为「全库 0」**） |

### 0.5 旧路径映射（归档后如何查历史）

| 旧路径 / 旧写法 | 现在在哪 |
|---|---|
| `docs/actionPlan/M_patchA.md` | → [../Archive/actionPlan/M_patchA.md](../Archive/actionPlan/M_patchA.md)（📦 已归档 2026-09-27） |
| `docs/actionPlan/M_patchB.md` | → [../Archive/actionPlan/M_patchB.md](../Archive/actionPlan/M_patchB.md)（📦 已归档 2026-09-27；§9.x 各批次实测记录与附录 B–E 的**快照**） |
| 本文档里 `M_patchA §x` / `M_patchB §x` 的写法 | 指**归档文档**内的同名章节（**规格与实测原文**）；**现行版本**以本文对应章节与 §8 附录为准 |
| 源码 / 数据里的历史引用（**本轮未改**） | `source/assets/providers.json` 的 `notes`、`source/include/ai/provider_spec.h` 注释里的 `docs/actionPlan/M_patchB.md`；`engine/executor.h`、`ui/output_panel.h`、`ai/deepseek/deepseek_official_provider.h` 注释里的 `M_patchA §0.3` / `M_patchA PB-03/PB-08` —— 均指**归档文档**；如需路径精确，可另行批量修正（属代码注释/数据文本，需一次重建验证） |


---

## §1 剩余项总表

> **§0.6 全量判定（2026-10-11 · 核对后落盘 · 判据 = 真要做 / 只勾账 / 已失效 / 不排期）**：
> * 🟢 **要做（保留）**：`PB-02` 真取消 · `PB-06` 剩余（代理 / 自签证书 / `[credentials]`）· `PB-07`（与 `PB2-25` 合并）· `PB-09` 自检扩展 · `PD-06①` 通用重跑 · **L2 整批**（`PB2-04` / `PB2-05` 残余 / `PB2-08` / `PB2-09` / `PB2-10` / `PB2-11` / `PB2-12` —— **2026-10-11 拍板：保留「做」**，理由 = 可插拔底座）· `PB2-07` 测试连接按钮 · `PB2-24` / `PB2-29` / `PB2-30②` 站点数据 · `PC-01`~`PC-04` · `PM-03`。
> * 📦 **不执行 / 不排期**：**`PD-07` 输出面板进阶**（历史列表 / 节点内搜索 / Markdown·JSON 导出 —— 2026-10-11 拍板**不执行**）· `PB-03` / `PB-08` 流式**数据源**（`PQ-11` = ③ 已拍板「不做」）· `PD-01` 快捷键（⛔ 早定不做）· `PW-09` 回填（见 [M_patchC_web_node_bind.md](M_patchC_web_node_bind.md) §0.6）。
> * 🔵 **只勾账**：`PM-01`（`--dep-check` 已落地）· `PM-04`（诊断复制已落地）· `PB2-31`（v16 已落地）· `PD-03`（= `M6-01` 已落地）· `PD-08`（`PQ-18②` 已结案且已落地）。
> * 🔁 **合并（不新建实施项）**：`PM-03` ⇒ 只在 `source/build.ps1` 加**一行**调用 `source/tools/check_all.ps1` + 文档提及。
> * ✅ **待拍板项已于 2026-10-11 全部结案（⛔ 零代码）**：`PQ-06` 版本号 = **沿用 `v0.5.4`**（见 [M8.md](M8.md) §7 `M8-Q4`）· `PQ-09` 字段回落口径 = **② `login_url` 缺失 = 明确报错（不回落）**（§9 `D-26`，复核 = **与现行实现一致**）· `PQ-12` 逐站回填范围 = **① 只做国内站点（6 站）**（§9 `D-32`）；**唯一落地项 = `PB2-29` 逐站实测回填**（归真机批）。

> 批次命名：**`B2-b`**（`PB2-*` L2）· **`B2-e2`**（L4 尾项）· **`B2-f`**（L1 尾项）· **`B3-*`**（Patch B 残项）· **`C1`**（数据安全）· **`D1`**（交互打磨）· **`PM1`**（并行小项）。
> 「归口」= `DevPlan.todo` 里已存在的任务 id（**不新开重复条目**，只把 `fileLink` 指到本文）。

| # | 组 | 条目 | 批次 | 验证 / 验收 | 归口（DevPlan） | 人工关卡 |
|---|---|---|---|---|---|---|
| 1 | B 残 | `PB-02` 真取消（HTTP 可中断 + 协作检查点） | `B3-a` | `VB-02` / `AB-01` | `FEA-M4-02`、`DBG-M4-02` | — |
| 2 | B 残 | `PB-07` 会话失效引导（**并入** `PB2-25`） | `B3-a` | `VB-06` / `AB-05` | `FEA-M4-07`、`DBG-M4-05` | 需一次真实失效响应 |
| 3 | B 残 | `PB-06` 剩余：代理 + 自签证书 + `[credentials]` 配置项 | `B3-b` | `VB-05` | `FEA-M4-13` | 企业网络环境下手测 |
| 4 | B 残 | `PB-09` 自检扩展（V-11 / V-12 + `--run-selftest --official`） | `B3-b` | `VB-04` | `FEA-M4-12` | 有 Key 才能跑 official |
| 5 | B 残 | 流式**数据源**（`PB-03`/`PB-08` 暂停项）+ `VB-03` 离线「假增量」断言 | `B3-c` | `VB-03` | `FEA-M4-03`、`FEA-M4-08` | — |
| 6 | B 残 | `PD-06①` 通用重跑（该节点 / 该节点及下游） | `B3-c` | `VD-05` | `FEA-M4-11` | — |
| 7 | L2 | `PB2-04` API 请求参数化（端点 / 认证 / 超时按表） | `B2-b` | `VB2-04` | `FEA-M_patchB-04` | — |
| 8 | L2 | `PB2-05` 残余（按归档 §3「落地情况」表逐行核对，只收非 ✅ 行） | `B2-b` | `VB2-05` | `FEA-M_patchB-05` | — |
| 9 | L2 | `PB2-08` `ai/inference_provider.h`（接口 + 值类型 + 能力表） | `B2-b` | `VB2-08` | `FEA-M_patchB-08` | — |
| 10 | L2 | `PB2-09` `ai/provider_factory` + 收编 `DeepSeekWebProvider` | `B2-b` | `VB2-09` | `FEA-M_patchB-09` | — |
| 11 | L2 | `PB2-10` `AnthropicProvider`（Messages API） | `B2-b` | `VB2-10` | `FEA-M_patchB-10` | 需 Anthropic Key |
| 12 | L2 | `PB2-11` `GeminiProvider`（generateContent） | `B2-b` | `VB2-11` | `FEA-M_patchB-11` | 需 Gemini Key |
| 13 | L2 | `PB2-12` 节点接线改**能力驱动** + 校验/提示同步 | `B2-b` | `VB2-12` | `FEA-M_patchB-12` | — |
| 14 | L1 尾 | `PB2-07` 界面「测试连接」按钮（异步、结果进 Console + 状态栏） | `B2-f` | `VB2-07` | `FEA-M_patchB-07` | — |
| 15 | L4 | `PB2-25` 网页版会话失效**可诊断**（`--web-probe` 判据加「端点 `code=0`」+ 面板/校验「重新登录」入口 + `token_expr` 表驱动容错） | `B2-e2` | `AB2-17` / `VB2-20` | `FEA-M_patchB-25` | 需真实失效响应 |
| 16 | L4 | `PB2-29` 内置站点 **`answer_selector` 逐站实测回填**（+ `verified:true` + §8 附录 E 两列） | `B2-e2` | `AB2-21` | `FEA-M_patchB-29…30` | **必须人工登录 + 手动发一条消息** |
| 17 | L4 | `PB2-30②` 面板「测试选择器」按钮（复用只读探测，把命中数/建议显示在界面） | `B2-e2` | `AB2-22` | `FEA-M_patchB-29…30` | — |
| 18 | L4 | `PB2-24` 自助闭环：新建站点条目 UI + **重新加载配置表**（下拉即时刷新）+ 配置表错误/警告界面可见（下拉「合并显示」＝ `PB2-31` **已 ✅ v16**） | `B2-e2` | `AB2-22` | `FEA-M_patchB-29…30` | ✅ **已落地（2026-10-10）** —— 代码 + 离线断言（`api_probe --site-assert`）见 §3.5「✅ 已落地」；**界面点选回路待真人点一次**；新建后需一次真实运行 |
| 19 | C | `PC-01` autosave（节流 + 原子写）· `PC-02` 恢复流程 —— ✅ **已落地（2026-10-10）**：纯逻辑落 `engine/autosave.*`（**落点调整**：原写 `ui/editor_state.*`，因 GUI 目标不可离线断言而改）+ `ui/editor_state.*` 节流编排 + `ui/app.cpp` 恢复询问；`api_probe --autosave-assert` **28 / 0** · `check_all.ps1` **14 / 14**；⛔ 真人点选（改图 → 写快照 → 点「恢复」）未做 | `C1` | `VC-01` / `VC-02` | `FEA-M2-01` | — |
| 20 | C | `PC-03` 最近列表治理（存在性校验 + 灰显 + 清理失效） | `C1` | `VC-03` | `FEA-M2-02` | — |
| 21 | C | `PC-04` 版本迁移框架（workflow `version` + `config_version` + 迁移自检） | `C1` | `VC-03` | `FEA-M2-03`、`DBG-M6-02` | — |
| 22 | D | `PD-03` 设置面板（`ui/settings_panel`；全量配置接线 + 落盘 + 范围/路径校验） | `D1` | `VD-03` | `FEA-M6-01` | — |
| 23 | D | `PD-07` 输出面板进阶（历史运行列表 / 节点内搜索 / 复制为 MD·JSON） | `D1` | `VD-06` | `FEA-M5-03` | ✅ **已拍板（2026-10-09 · `PQ-17`）：留后 · 不排期**（原文保留：范围 ⬜ 待定） |
| 24 | D | `PD-08` i18n 决策（最小字符串表 **或** 从 config 移除并记入 M6） | `D1` | `VD-07` | `FEA-M6-05` | ✅ **已拍板（2026-10-09 · `PQ-18`）= ② 从 `config.toml` 移除并记入 M6**；**代码已落地（2026-10-09）**（原文保留：需你选方案（`D-04`）） |
| 25 | D | 归档完整化：`output.auto_open_on_complete` 接线 + 历史/预览 | `D1` | `VC-04` | `FEA-M5-04` | — |
| 26 | PM | `PM-01` WebView2 Runtime 检测与安装引导（清零 M1 唯一遗留） | `PM1` | `VM-01` | `FEA-M6-02`、`DBG-M1-01` | 需模拟缺失环境 |
| 27 | PM | `PM-03` 构建脚本一键化（构建 + 全部自检摘要，失败给文件+行号） | `PM1` | `VM-03` | `FEA-M6-03` | — |
| 28 | PM | `PM-04` 诊断信息一键复制（版本/依赖/数据目录/日志路径/GPU/DPI） | `PM1` | `VM-04` | `FEA-M6-04` | — |
| 29 | C 类配置残项 | `advanced.log_ttl_days` / `log_dir` 语义实现 + `timeout.*` / `error.*` 接线（`grid_size` 归口 `PD-03`；`general.*` 已按 `PQ-18②` **移除**、不再接线） | `D1` | `VD-03` | `FEA-M6-07`、`DBG-M6-01` | — |
| 30 | 任务看板 | `FEA-M3-07` 工作流变体保存（**延后 · 由你后续设计**） | — | — | `FEA-M3-07` | ✅ **已拍板（2026-10-09 · `PQ-19`）：维持「延后 · 待设计」· 不排期** |

**顺序建议**：`B2-e2`（L4 尾项，含**唯一人工关卡** `PB2-29`）→ `B2-f` + `B3-a`（会话失效引导与 `PB2-25` 同批，一次真实失效响应可同时验两条）→ `B2-b`（L2 工厂，纯离线自检）→ `C1` → `D1` → `PM1`。
**可并行**：`B2-b`（离线）与 `B2-e2`（需现场手测）互不阻塞。

---

## §2 Patch B 残项（`B3-*`）

> 依据：[Archive/actionPlan/M_patchA.md](../Archive/actionPlan/M_patchA.md) §4.1（任务表）/ §4.2（`VB-01…VB-09`）/ §4.3（风险）。

### 2.1 `PB-02` 真取消（`B3-a`）

- **要点**：`cancel()` 置位并**唤醒**等待；HTTP 层可中断（客户端取消 / 缩短读超时 + 分块读 / 取消时主动关 socket）；节点**协作检查点**（`ctx.is_cancelled()` 在重试之间与流式块之间检查）。
- **落点**：`ai/*provider*`（官方与网页版）、`engine/executor.cpp`、`ui/editor_state.*`。
- **验证 / 验收**：`VB-02`（运行中点「■ 停止」**1 秒内**进入 Cancelled，日志有取消记录）/ `AB-01`。
- **风险对策**（归档 §4.3）：取消语义不彻底（阻塞在读）→ 缩短读超时并分块读；取消时主动关闭 socket。
- **决策依据**：`D-02 ①`（接受线程化）；`D-06 ①`（当时延后，现回到待办）。

### 2.2 `PB-07` 会话失效引导（`B3-a`；**与 §3.3 `PB2-25` 合并**）

- **要点**：识别网页版 `40002 / 401` 与官方 `401` → 状态栏 / 错误条显示「**会话已失效，点此重新登录**」+ 按钮直达登录窗口（WebView2）。
- **已有基础**（v15）：`ai::web_session_failure_hint()` / `web_session_failure_needs_relogin()` 已落地（命中 `40002`/`401` 作废该站点内存会话）；本项**只差界面入口 + `--web-probe` 判据收紧**。
- **落点**：`ai/deepseek_web_client.cpp`、`ui/editor_state.*`、`ui/error_bar.*`、`ui/property_panel.cpp`、`source/src/main.cpp`（`--web-probe`）。
- **验证 / 验收**：`VB-06`（伪造失效 Token → 出现「重新登录」入口且按钮可打开登录窗口）/ `AB-05`。
- **实测背景**：归档 `M_patchB.md` §9.4（v9：`userToken` 形状变化 + 全端点 `40003`）、§9.10（v15 `PB2-28④`）。

### 2.3 `PB-06` 剩余：代理 + 自签证书 + `[credentials]`（`B3-b`）

- **要点**：支持 `HTTPS_PROXY` / `HTTP_PROXY`；自签证书开关（**默认严格校验**）；`config.toml [credentials]`（路径 / TTL / backend）—— 当前使用代码默认值（`~/.brain-ai/credentials`、TTL 30 天、backend `dpapi`）。
- **落点**：`ai/*provider*`、`utils/config.*`、`utils/credential.*`。
- **验证**：`VB-05`；归口 `FEA-M4-13`。
- **风险对策**：凭据/代理失败要**回退到「输入框 + 说明」**，不阻断主流程。

### 2.4 `PB-09` 自检扩展（`B3-b`）

- **要点**：`api_probe --selftest` 增 **V-11 无 Key 请求构造**、**V-12 取消/超时语义**（七组 → 九组）；新增 `aiwrite.exe --run-selftest --official`（有 Key 时真实生成）；`--run-selftest --web` 保持 5/5。
- **落点**：`source/tools/api_probe.cpp`、`source/src/main.cpp`。
- **验证 / 验收**：`VB-04` / `AB-03`；**依赖 §2.1**（V-12 需取消语义落地）。

### 2.5 流式**数据源**（`B3-c` · 暂停项，需你定方案）· ✅ **已拍板（2026-10-09 · `PQ-11` = 决策 `D-31` 结案 = 选 ③）**：**明确不做**（网页版文案改「分段落定」）+ 补 `VB-03` 离线断言 —— 原文（拍板依据）保留：⬜ **待定（`PQ-11` = 决策 `D-31`）**

- **现状**：链路已通（`on_delta` → `RunEvent::Delta` → 读模型逐段追加），但 **httplib v0.15.3 无「POST 响应体流式接收」重载** → 增量一次性到达，表现为「最后一下全出来」；官方 SSE 同理（归档 `M_patchA §4.1 PB-03/PB-08`、[网页版协议实测记录](../Archive/网页版协议实测记录.md) 附录 A）。
- **三选一（待你拍板）**：① 升级/替换 httplib（风险中，需回归全部联网路径）；② 网页版改「POST 生成 + GET 轮询正文」（**需先探到 A′ 端点**，候选清单见实测记录附录 A.3）；③ 明确不做，把 UI 文案从「流式」改为「**分段落定**」并登记。
- **验证**：`VB-03` —— 把「离线假增量断言」自动化（构造多段增量 → 断言**拼接结果 == 非流式结果**，`delta_bytes` 与正文长度一致）。
- **注意**：`VB-03` 是**唯一被标 🚫** 的验证项，建议即使选 ③ 也把这条离线断言补上。

### 2.6 `PD-06①` 通用重跑（`B3-c`）

- **要点**：「重跑该节点」「重跑该节点及下游」；复用上游结果需执行器支持「从某节点开始」；重跑前**清除受影响节点的结果与状态**（运行态值，**不进**撤销快照 / 工作流 JSON）。
- **落点**：`engine/executor.*`、`ui/editor_state.*`、`ui/property_panel.cpp`、`ui/node_canvas.cpp`、`ui/toolbar.cpp`。
- **验证**：`VD-05`（只重跑目标节点时上游结果复用；重跑下游时受影响节点结果刷新）；归口 `FEA-M4-11`。
- **区分**：`PD-06②`「重新生成（新 seed）」= `FEA-M4-17` **已完成 ✅**（见 [Archive/actionPlan/M_rerun.md](../Archive/actionPlan/M_rerun.md)）。

---

## §3 M_patchB 残项（`B2-*`）

> 依据：[Archive/actionPlan/M_patchB.md](../Archive/actionPlan/M_patchB.md) §0.3（范围）/ §3（任务分解）/ §4.2（`AB2-*`）/ §5（风险 `R1…R23`）/ §9（实测进度）。

### 3.1 L2 —— 协议 / 适配器实现（`B2-b`）

> 整批**未开工** → ✅ **2026-10-11 拍板：整批保留「做」**（可插拔底座）。提交信息约定（沿用归档 §4.1）：`feat(ai): M_patchB L2 InferenceProvider 接口与工厂（openai/anthropic/gemini/deepseek-web 按表分派）+ 能力驱动接线`

| 编号 | 要点 | 验证 |
|---|---|---|
| `PB2-04` 🟢 **要做（L2 底座 · 2026-10-11 保留）** | 抽出 `ProviderOptions{api_base, chat_path, auth_style, auth_header, extra_headers, env_names, limits}`；`build_endpoint(base,path)` / `build_auth_headers(spec,key)` / `resolve_api_key(param_key, env_names)`（env 名**按序尝试**，默认仍含 `DEEPSEEK_API_KEY`）；**旧签名保留为重载**（默认值 = 今天 DeepSeek 行为 → `I1` 自动成立） | `VB2-04`（端点拼接 5 例 / 认证头 5 例 / env 顺序命中） |
| `PB2-05` 残余 🟢 **要做（小）** | 按归档 §3「落地情况」表**逐行核对**（第 1–3 行均已 ✅）；主要残项 = 面板「配置表管理区」四按钮（并入 §3.5）与 `provider_spec.h` 头注释改用**嵌套** schema 名（`R18`） | `VB2-05` |
| `PB2-08` 🟢 **要做** | `ai/inference_provider.h`：`ProviderCaps{vision,stream,seed,system_role}`（来自表 `capabilities`，**取代**节点里 `if (mode == "web") throw`）、`GenerateParams`（system_prompt/prompt/images/temperature/max_tokens/top_p/seed/on_delta）、`GenerateResult`（ok/http_status/text/error/raw_head/elapsed_ms）、`class InferenceProvider{name,caps,generate}`；**不**新开 `generateStream`/`generateWithImage` 虚函数（保留 `on_delta` + `images`，与「流式暂停」现状一致，避免虚函数空转）—— 记 `D-09` | `VB2-08` |
| `PB2-09` 🟢 **要做** | `ai/provider_factory.{h,cpp}`：**按表 `protocol` 分派**（`openai` / `anthropic` / `gemini` / `dom` / `deepseek-web`）；把现有 `web_chat()` 收编为 `DeepSeekWebProvider`（`--web-chat` 行为**逐字不变** → `I2`）；**未知 `protocol` 明确报错**（`R9`） | `VB2-09` |
| `PB2-10` 🟢 **要做** | `AnthropicProvider`（Messages API：`/v1/messages` + `x-api-key` + `anthropic-version`） | `VB2-10` |
| `PB2-11` 🟢 **要做** | `GeminiProvider`（`generateContent`） | `VB2-11` |
| `PB2-12` 🟢 **要做** | 节点接线改**能力驱动**（视觉 / seed / 系统角色门控来自表，不再硬编码 `mode == "web"` 判断）；校验与提示文案同步由能力表生成 | `VB2-12` |

**风险对策（归档 §5）**：`R9` 表与代码能力脱节 → 工厂对未知 `protocol` **明确报错**，`--provider-selftest` 逐条检查「表条目 → 是否有实现」；`R11` 抽象过度 → **新增代码预估 ≤ 1800 行**；`R12` adapter 不支持字段 → 加载时**警告并忽略**。

### 3.2 `PB2-07` 界面「测试连接」按钮（`B2-f`）

- **现状**：CLI 侧早已 ✅（`--provider-selftest` **50/0**、`--provider-dump` 20 条）；**只差界面按钮**。
- **要点**：参数面板加「测试连接」（**异步、不阻塞界面**；结果进 Console + 状态栏）：
  - API 条目：① 表校验 + 离线断言 ② 有 Key 时发一条 `ping`（「请只回复 pong」）→ 打印 `HTTP / 模型 / 耗时`；
  - web 条目：③ 登录态 + 生效 `endpoints`/`probe_paths` 与表是否一致；**不发送任何提示词**（避免误触发站点调用与风控）；
  - 退出码语义与 CLI 一致：`0` 全通过 / `1` 失败 / `2` 无 Key 或未登录。
- **落点**：`ui/property_panel.cpp`（按钮 + 结果区）、复用 `ai::provider_spec_selftest()` 的实现（**不要再写一套**）。
- **验证**：`VB2-07`；日志/Console 输出守脱敏（`R20`）。

### 3.3 `PB2-25` 网页版会话失效**可诊断**（`B2-e2`；与 §2.2 合并）

- **三个问题（v9 实测登记）**：
  1. **判据偏松**：`--web-probe` 的 PASS 只看 `probe.ok && has_user_token` → 端点 `40003` 时**仍打印 PASS**。**改**：判据加「端点探测全部 `code=0`（至少 `/api/v0/users/current`）」。
  2. **失效不可诊断**：会话失效只在**运行期**报「挑战解析失败… invalid token」。**改**：识别「端点 `40003` / 无有效凭证」→ 面板站点区 + 运行前校验给「**会话已失效：请重新登录**」+ 一键打开登录窗口（= §2.2）。
  3. **取值形状**：若属官网形状变化 → 走 `web.token_expr` **表驱动**容错（必要时解包 JSON）；诊断输出受 `R20` 约束（只给**前 4 字符 + 长度** / 「是否 JSON 包裹」等不可复用特征）。
- **已有基础**（v15，`PB2-28④`）：`ai::web_session_failure_hint()` / `web_session_failure_needs_relogin()`（容错匹配 `code=40002` / `code= 40002` / `biz_code=40003` 与 HTTP `401`/`403`；`401`/`40002` → 追加「请重新登录该站点」并**作废该站点内存会话**，`40003` → 只给可操作提示、**不**作废）。
- **落点**：`source/src/main.cpp`（`--web-probe` 判据）、`ui/property_panel.cpp`、`engine/validate.cpp`、`ui/app.cpp`（状态栏）。
- **验证 / 验收**：建议 `AB2-17` / `VB2-20` —— 会话失效时 `--web-probe` 退出码 **1**（不再 PASS）、面板给「会话失效」+ 重登入口、运行前提示明确；**重新登录后** `--run-selftest --web` **5/5**。

### 3.4 `PB2-29` / `PB2-30②` 逐站选择器（`B2-e2`）

- **现状（v15/v16 实测）**：**12/12** 条目的「打开登录窗口」都真的打开**各自的站点** ✅（v15 当时的清单含 `tongyi-web` —— 该条目 2026-10-10 因与 `qwen-web` 重复而移除 ⇒ 现行 **11/11**，见下表）；**4 条输入框选择器已实测回填**（`kimi-web` = `div.chat-input-editor`、`qwen-web` = `textarea[placeholder="Ask Qwen"]` + `div.message-input-right-button-send`、`yuanbao-web` = `div.ql-editor.ql-blank`、`ernie-web` = `#chat-textarea`，已进 `source/assets/providers.json`）；**10 条的 `answer_selector` 仍空**（回答容器必须先有一条回答才存在 —— **唯一人工关卡**）。
- **单站标准流程**：① 界面「打开登录窗口」**手动登录**（不代填密码、不绕过验证）→ ② **手动发一条消息** → ③ `aiwrite.exe --web-dom-dump --provider <id>` 读出候选元素 + 建议 → ④ 填 `answer_selector`（必要时 `done_when` / `answer_poll_ms` / `answer_max_polls`）→ ⑤ `--web-adapter-selftest --provider <id>` 复核命中数（**可见**）→ ⑥ `aiwrite.exe --run-selftest --web --provider <id>` 端到端 → ⑦ 条目置 `verified: true` + `notes` 记实测日期与站点版本 → ⑧ 回填 **§8 附录 E** 两列 + [网页版协议实测记录](../Archive/网页版协议实测记录.md) 逐站追加。
- **次序建议**：Kimi 先行（最终域 `www.kimi.com` 已修正、`contenteditable` 输入框已就绪）→ Qwen / 元宝 / 文心（输入框已就绪）→ 豆包 / 星火 → `chatglm-web`（需人工过 WAF）→ 境外三站（本机可直连，可选做）。
- **`PB2-30②`（面板「测试选择器」）**：复用 `ai::dom_adapter_selftest()`（**只读**），把**命中数 + 建议**显示在界面（不必再看 Console）→ 这是 `PB2-29` 的**配套工具**，建议先做它再逐站取选择器。
- **原则**：**不实测不填值**（沿用 `PB2-26`）；站点改版 → 用户改 JSON 自助修复（`R21`）。
- **验收**：`AB2-21`（逐站独立验收）/ `AB2-22`。

### 3.5 `PB2-24` 自建站点闭环（`B2-e2`）

- **初查现状（2026-10-10 之前 · 下面三条**已全部消除** —— 见本小节末「✅ 已落地」）**：`reload_provider_specs()` **全工程零调用点**；`src/ui` 无任何「配置表 / 重新加载 / 测试连接」入口；`registerAllNodes()` 幂等 → 「提供商」枚举在首次注册时**一次性**生成，运行中 reload **不刷新下拉**；配置表错误 / 警告**只在 `app.log`**（界面不可见）。
- **要点**：
  1. 「**新建网页版站点…**」按钮 → 从内置条目复制模板 → 写 `~/.brain-ai/providers.d/<id>.json`（**必须带 `schema_version` 信封**，见 §8 附录 C）→ 立即可用；
  2. `reload_provider_specs()` **接线** + `provider` 参数改**动态枚举**（或提供重建接口）→ 下拉即时刷新；
  3. 面板 / Console 显示配置表**错误 / 警告**（含「已忽略该层 / 已跳过该条」）。
- **已 ✅ 部分**：`PB2-31` 提供商下拉「合并显示」（v16 · 仅 ImGui 层 · **经用户界面验证通过**）。
- **依赖**：与 §3.4 的 `PB2-30②` 同批实现最省事（两者都读只读诊断结果）。
- **验收**：`AB2-22`；风险 `R8`（坏表 → 报错 + 上一份可用表/最小兜底）、`R18`（照抄也能建出站点）。

**✅ 已落地（2026-10-10 · 代码 + 离线断言；作者 = 本批）**：三条要点全部实现 ——

1. **新建站点**：设置面板分区⑥「**新建网页版站点…**」模态（模板来源下拉 = 空白 / 从内置条目复制 —— **只抄身份**（`login_url` / `window_title` / `cookie_names` / `token_expr`），**内置 `builtin:*` 的 `web.*` 选择器不抄**）→ 写 `~/.brain-ai/providers.d/<id>.json`：**带 `schema_version` 信封**（§8 附录 C）、**强制 `web.adapter=dom`**、**`verify=false`**（`PB2-26` 口径：无实测不写 `verified`）、**原子写**（`.tmp` → 改名）、**默认拒绝覆盖**（需勾选）。落点 `source/ai/{include,src}/site_template.*`（纯函数 ⇒ 可离线断言）。
2. **重载 → 下拉 / 校验刷新**：`reload_provider_specs_and_refresh()` = **重载 + 刷新单入口**；`refresh_provider_options(Graph&)` 会**连已有节点实例的 `provider` 枚举一起重建并保留当前值**（`registerAllNodes()` 幂等导致的「枚举只在首次注册时生成」不再是坑）。落点 `source/engine/{include,src}/node_registry.*`。
3. **错误 / 警告界面可见**：面板 §⑥ 列出配置表**逐条中文错误 / 警告**（含未知字段、缺 `answer_selector`）+ 「**重新加载配置表**」+ 「打开站点目录」按钮 ⇒ 手改 JSON 后**点一下即生效**，不必重启。
- **验证**：`api_probe --site-assert` **PASS · exit 0** —— 纯函数 **47 / 0** + 端到端：写盘 → 重载 → 进表 / 进网页版下拉 / 进 `provider` 枚举（**20 → 21**）→ 新建节点运行前校验通过 → **旧实例刷新回归**（打回旧枚举 ⇒ 先被判越界、刷新后通过，证明护栏真实）→ 清理（条目数回落 **20 → 20** · 磁盘零残留）；`check_all.ps1` **13/13 PASS**；构建 **0 error / 0 warning**。物证 `build/logs/`。
- **⛔ 未人工实测（如实登记）**：界面**点选**回路「新建 → 下拉出现 → 选中 → 打开登录窗口」需**真人点一次鼠标**（本机无 GUI 自动化）；同一条代码路径已由 `--site-assert` 覆盖。
- **同批**：唯一不顺带做的是 `PB2-30②` 面板「测试选择器」按钮（**仍 ⬜**，与 §3.4 逐站取选择器同批）。

---

## §4 数据安全（原 Patch C · `C1`）

> 依据：[Archive/actionPlan/M_patchA.md](../Archive/actionPlan/M_patchA.md) §5.1（任务）/ §5.2（`VC-01…VC-06`）/ §5.3（风险）。
> 已完成 ✅（归档留存）：`PC-05` 输出归档最小集（每节点 `.txt` + `run.json`）、`PC-06` TTL 与上限（`keep_history` / `max_history` / `ttl_days`）。

| 编号 | 要点 | 落点 | 验证 |
|---|---|---|---|
| `PC-01` | **autosave**：工作流变更后**节流**（≥3 秒且空闲）写 `~/.brain-ai/snapshots/autosave.json`；**原子写**（临时文件 + rename）；保留最近 N 份（`autosave_1..N`）　✅ **已落地（2026-10-10）· 落点调整**：纯逻辑 = `source/engine/{include,src}/autosave.*`（**原计划 `ui/editor_state.*`** —— 但 `ui/**` 属 GUI 可执行目标、`api_probe` 链接不到，放那里**无法离线断言** `VC-01`），`ui/editor_state.*` 只做**时机编排**（1 Hz 内容指纹探测 + 静默 `ui.autosave_idle_sec` 秒 + 无变更不写 + 失败退避 10 秒）；默认目录仍复用 `paths::snapshots_dir()`。历史轮转 = **先复制旧最新成 `autosave_1` 再原子替换最新** ⇒ 不存在「`autosave.json` 缺失窗口」 | `engine/autosave.{h,cpp}`（新）、`ui/editor_state.{h,cpp}`、`utils/paths`（复用 `snapshots_dir()`） | `VC-01`（✅ `--autosave-assert`：半截 `.tmp` 不参与恢复 / 可清理 / 清理后仍可加载 + 写后无 `.tmp` 残留 + `keep=2/0` 轮转） |
| `PC-02` | **恢复流程**：启动时若 autosave 比最近打开的工作流新 → 弹一次「恢复未保存的更改？」（可禁用该提示）；恢复后**不覆盖原文件**，另存需确认　✅ **已落地（2026-10-10）**：判定 = 快照比工作流 mtime 新 / 原文件不存在 / 未命名 ⇒ 问一次；模态三按钮「恢复 / 忽略（保留当前内容）/ 忽略，且不再提示（写 `ui.autosave_restore_prompt=false`）」；**恢复后 `current_workflow_path` 留空 ⇒ 保存必走另存**（原路径记 `restored_from_autosave_path`） | `ui/app.cpp`、`ui/editor_state.*` | `VC-02`（✅ `--autosave-assert` 4 场景 + 恢复后**内容指纹逐字相同**；GUI 实测：启动判定日志 + 12 秒后快照哈希不变 = 启动不冲掉真快照；⛔ 真人点「恢复」未做） |
| `PC-03` 🟢 **要做** | **最近列表治理**：打开前校验存在性；失效条目**灰显**并标注「（文件不存在）」；提供「清理失效条目」 | `engine/recent_files.{h,cpp}`、`ui/app.cpp` | `VC-03` |
| `PC-04` 🟢 **要做（与 `DBG-M6-02` 同批）** | **版本迁移框架**：`workflow_io` 的 `version` 与 `config_version` 建迁移表（当前只 warn）；**未知高版本明确拒绝**并提示；新增迁移自检 | `engine/workflow_io.cpp`、`utils/config.cpp` | `VC-03`（构造 v1.0 旧文件 → 加载并写回当前版本；未来版本 → 明确拒绝） |

**风险对策**：误删用户输出 → TTL 清理**先列出再删 + 写日志 + 默认保守**；autosave 频繁写盘 → 节流 + 仅变更时写 + 原子写；恢复提示打扰 → 仅当 autosave 更新时提示且可「不再提示」（归档 §5.3）。

---

## §5 交互打磨与配置接线（原 Patch D · `D1`）

> 依据：[Archive/actionPlan/M_patchA.md](../Archive/actionPlan/M_patchA.md) §6.1（任务）/ §6.2（`VD-01…VD-08`）/ §6.3（风险）。
> 已完成 ✅（归档留存）：`PD-02` 复制/粘贴重做、`PD-04` 窗口几何持久化、`PD-05` 参数面板增强、`PD-06②` 重新生成（新 seed）；**节点宽度按「输入/输出标签实测最宽」自适应（全画布同值 · 长文本按像素省略/折行）2026-09-29**（`ui/node_canvas.cpp`，见 `CHANGELOG` 与 `ai_writer_nodes.md` §14.2）。

| 编号 | 要点 | 落点 | 验证 |
|---|---|---|---|
| `PD-03` | **设置面板**（`ui/settings_panel.{h,cpp}`）：分区编辑 `ui/output/timeout/error/advanced`（**「通用」分区已无字段** —— `general.language` / `general.startup` 已按 `PQ-18②` 移除）；含**校验**（范围 / 路径存在性）+ 落盘 + 「需重启生效」标注；`deepseek` 段说明「**节点参数为准**」；本项同时承接 C 类配置残项接线（`timeout.*` / `error.*` / `advanced.*`，`grid_size` 受库限制归口于此） | `ui/settings_panel.{h,cpp}`、`ui/app.cpp`、`utils/config.*` | `VD-03`（面板 ↔ `config.toml` 双向一致；非法值被拒绝并提示） |
| `PD-07` ⏸ **不执行（2026-10-11）** | **输出面板进阶**：历史运行列表（内存 + 最近 N 次）、节点内搜索、复制为 Markdown / JSON | `ui/output_panel.cpp` | `VD-06` |
| `PD-08` ✅ **已落地（`PQ-18②` 结案 + 代码已落地）** | **i18n 决策**：二选一 —— ① 最小 i18n（字符串表 + `general.language` 切换，重启生效）② 从 `config.toml` 移除 `general.language/startup` 并记入 M6（`D-04` 默认 ②）　✅ **已拍板（2026-10-09 · `PQ-18` 结案）= 选 ②（从 `config.toml` 移除 `general.language` / `general.startup` 并记入 M6）**：与 [../roadmap.md](../roadmap.md) §十「i18n …**不做**」口径**一致** ⇒ 触发 §十「显式推翻」条款、**记入 M6**；✅ **代码已落地（2026-10-09）** = `utils/config.{h,cpp}` 删除 `General`（`language` / `startup`）+ `source/tools/api_probe.cpp` 的 `PA-08` 断言改判「**不含** `general.*`」；**实测数字无变动**（`--exec-selftest` 仍 **425 / 0**、`--graph-selftest` **110 / 0**、`--selftest` 七组全 PASS）—— 该断言是**单行 PASS/FAIL**、不进数字计数 ⇒ 原写的「425 → 424」是**误判**（订正见 §10 v4）。原文（拍板依据）：⬜ **待定（`PQ-18`）** —— 与 [../roadmap.md](../roadmap.md) §十「i18n **不做**」并列时**需一次显式确认** | `utils/config.*`、文档 | `VD-07` |
| 归档完整化 | `output.auto_open_on_complete` 接线 + 历史/预览（`FEA-M5-04`） | `ui/output_panel.cpp`、`utils/output_archive.*` | `VC-04` |

**明确不做**（归档 §6.1 / §1.8）：`PD-01` 快捷键表（设计 §6.7 定为**纯鼠标**，`⛔ 决定不做`）；`PD-08` i18n 界面语言（`PQ-18` = ② ⇒ **不做**；`config` 侧 `[general]` 已移除 · 2026-10-09）；多工作流标签页 / 协同编辑 / 云同步 / 自动更新 / 插件系统（超出 MVP 定位）。

**风险对策**：粘贴坐标修复回归 → **先写复现自检再改代码**；快捷键与 ImGui 文本输入冲突 → `WantTextInput` 时跳过全局快捷键；设置面板范围膨胀 → **只接现有 `Config` 字段，不新增语义**；重跑与撤销栈交互 → 重跑只改运行态与节点状态（不进快照）（归档 §6.3）。

---

## §6 并行小项（`PM1`）

> 依据：[Archive/actionPlan/M_patchA.md](../Archive/actionPlan/M_patchA.md) §7。
> 已完成 ✅：`PM-02` 文档索引与状态治理。

| 编号 | 要点 | 落点 | 验证 |
|---|---|---|---|
| `PM-01` 🔵 **只勾账（已落地 2026-10-10）** | **WebView2 Runtime 检测与安装引导**（清零 M1 唯一遗留 → 落地 M6-02）：启动自检 Runtime 版本；缺失时给**官方 Evergreen Bootstrapper** 链接与说明；**不阻塞主界面**（仅网页版相关功能提示） | `web/webview_host.*`、`ui/app.cpp`、文档 | `VM-01`（模拟缺失 → 出现引导且非网页版功能可正常使用） |
| `PM-03` 🔁 **合并（不新建实施项）** | **构建脚本一键化**：`build.ps1` 增加「跑全部自检并输出摘要」（失败给**文件名 + 行号**） | `source/build.ps1`、`source/README.md` | `VM-03`（干净环境一条命令完成 + 可读摘要） |
| `PM-04` 🔵 **只勾账（已落地）** | **诊断信息一键复制**：版本 / 依赖版本 / 数据目录 / 日志路径 / GPU / DPI 缩放 | `ui/app.cpp`（关于 / 诊断区） | `VM-04`（复制内容可直接用于问题反馈） |

---

## §7 不做与已登记 · ✅ **已固定（2026-10-08）**

> ✅ **本节视为契约**：「决定不做」的条目**不再被重新提出**，除非新开条目并说明「外部事实变化」；⛔ 原文不删。

| 项 | 状态 | 说明 |
|---|---|---|
| `PD-01` 快捷键表（设计 §6.7） | ⛔ **决定不做** | 纯鼠标交互；`ed::EnableShortcuts(false)` 保持 |
| `FEA-M3-07` 工作流变体保存 | ⬜ **延后 · 待你设计** → ✅ **已拍板（2026-10-09 · `PQ-19` 结案）：维持「延后 · 待设计」· 不排期** | 把面板保存为独立 workflow / 当前 workflow 变体；「启动恢复上次工作流」一并归入（**仅登记**） |
| `DBG-M5-01` 网页版会话副作用 | 🟡 **登记（不修）** | 每次运行会在账号内新建/追加会话，正文进入 `chat.deepseek.com` 历史 —— 属站点行为，不做自动化规避 |
| `PB-04` Provider 统一抽象（设计 §8.1） | ✅ **有承接** | 已展开为 `PB2-08`…`PB2-12`（见 §3.1），此处不再单列 |
| `PB2-31` 提供商下拉「合并显示」 | ✅ **已完成（v16）** | 仅 ImGui 层；21 条表项 → 16 个下拉项；**经用户界面验证通过（2026-09-27）** |
| 归档文档的**附录 A**（`M_patchB` 现状证据明细） | 📦 **留归档** | 历史审计证据；本文不复制（执行不再依赖） |

---

## §8 附录（**现行规范** · 执行期就地维护）

> **为什么在这里**：`M_patchB` 已归档，而归档文档按规则**只增不改**（[../Archive/README.md](../Archive/README.md) §三.4）；但这几份附录是**执行期必须改**的东西（用户照着 JSON 建站点、逐站回填实测状态）。
> 故：**本节的附录 B/C/D/E 是现行版本**（2026-09-27 从 `Archive/actionPlan/M_patchB.md` §7 复制并更新；附录字母**保持不变**以免打断交叉引用）；归档版为**同一日的快照**，两者若冲突**以本节为准**。

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
| `cookie_names` | array | ❌ | ❌ | 需要的 Cookie 名（**最小必要**，不全取）。**已有消费点（v13 `PB2-27`）**：`ai::web_session_state()` 用它判「已登录」；逐站补法见 `D-30` |
| `token_expr` | string | ❌ | ❌ | 在页面里求值的取 token 表达式（如 `localStorage.getItem('userToken')`）。**已有消费点（v13 `PB2-27`）**：**仅当配了它**，面板才显示/使用该站点的 `userToken` 行（`D-28`） |
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

### 附录 D · 用户自建网页版站点条目（**2026-09-26 实测可用** · 2026-09-27 现行版本）

**三步**：

1. 新建文件 `~/.brain-ai/providers.d/<你的id>.json`（**必须**带信封 `schema_version` + `providers` 数组；文件名任意，按名升序合并）；
2. 保存后**点设置面板的「重新加载配置表」**即生效（**不必重启**；2026-10-10 `PB2-24` 之前需要重启）；
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

### 附录 E · 内置各 AI 网页版站点入口清单（**登录型** · 更新 2026-09-27）

> 用法：「提供商配置 →『提供商』」下拉里直接选（official 条目在前、web 条目在后）→「打开登录窗口」登录一次 → 凭证按站点隔离（**仅内存**）。
> **生成**：DOM 适配器（L3 `PB2-13…16`）**已落地**；下列 11 条里 **4 条输入框选择器已实测回填**，其余仍缺 `answer_selector` → 运行前用 `--web-adapter-selftest --provider <id>` 确认（缺项会**明确报错**，不静默、**不**用 DeepSeek 端点）。

| 条目 id | 显示名 | 登录页（`web.login_url`） | 窗口可达（v15 实测） | 可登录（人工登录 · 待做） | 可生成（选择器） | 备注 |
|---|---|---|---|---|---|---|
| `deepseek-web` | DeepSeek（网页版） | `https://chat.deepseek.com/` | ✅（内置协议站点） | ✅ | ✅ | 唯一 `builtin:deepseek` 适配器条目 |
| `kimi-web` | Kimi（Moonshot 网页版） | `https://www.kimi.com/` | ✅ | ⬜ | 🟡 **输入框 ✅** / 回答 ⬜ | 旧域 `kimi.moonshot.cn` **301** 到此（v11 实测）；`div.chat-input-editor` 已回填 |
| `qwen-web` | Qwen（国际站网页版） | `https://chat.qwen.ai/` | ✅ | ⬜ | 🟡 **输入框 ✅** / 回答 ⬜ | `textarea[placeholder="Ask Qwen"]` + 发送按钮已回填 |
| `chatglm-web` | 智谱清言（ChatGLM 网页版） | `https://chatglm.cn/main/alltoolsdetail` | ⚠️ 可达但被 **WAF** 挡 | ⬜ | ⬜ | 需人工过验证 + 登录后才能取选择器 |
| `doubao-web` | 豆包（字节网页版） | `https://www.doubao.com/chat/` | ✅ | ⬜ | ⬜ | **未登录不渲染输入框**；**网页版图片理解首个目标站（方案 B′ · 2026-09-29）** —— 侦察先行（`P7b-05b`：人工登录一次取齐三项 + 上传入口侦察 + 视觉性质判别），「可生成」列保持 ⬜（**不填假值**） |
| `yuanbao-web` | 腾讯元宝 | `https://yuanbao.tencent.com/` | ✅ | ⬜ | 🟡 **输入框 ✅** / 回答 ⬜ | `div.ql-editor.ql-blank`（Quill）已回填 |
| `ernie-web` | 文心一言 → **百度文心助手** | ⚠️ `https://wenxin.baidu.com/` | ✅（**已修正重定向**） | ⬜ | 🟡 **输入框 ✅** / 回答 ⬜ | `#chat-textarea` 已回填；站点现名「百度文心助手」 |
| `spark-web` | 讯飞星火 | `https://xinghuo.xfyun.cn/` | ✅ | ⬜ | ⬜ | **未登录不渲染输入框** |
| `chatgpt-web` | ChatGPT（chatgpt.com） | `https://chatgpt.com/` | ✅（本机网络可直连） | ⬜ | ⬜ | 窄窗渲染**移动版** `#mobile-composer-prompt`（桌面版预期 `#prompt-textarea`） |
| `claude-web` | Claude（claude.ai） | `https://claude.ai/` → 跳 `/login` | ✅（可直连） | ⬜ | ⬜ | 登录/注册前无 composer |
| `gemini-web` | Gemini（gemini.google.com） | `https://gemini.google.com/app` | ✅（可直连） | ⬜ | ⬜ | 需 Google 账号登录后取选择器 |

- **四列口径（L4 起）**：**窗口可达** = 「打开登录窗口」真的打开了**该 AI 自己的站点**（v15 实测 **12/12** ✅）；**可登录** = 「人工登录 → 抓取 Cookie」链路已实测通过（**待做**）；**可生成（选择器）** = `web.input_selector` / `send` / `answer_selector` **已实测填好**且端到端生成通过（⬜ = 尚未实测，**不填假值** —— 沿用 `PB2-26` 原则）。**L4 的目标就是把 10 行的「可生成」从 ⬜ 逐站变为 ✅**（`PB2-29`；每站需一次人工登录 + 选择器实测）—— 🟡 = **输入框已就绪、只差回答容器**。
- ⚠ **2026-10-10 清单变更（去重）**：`tongyi-web`（`https://www.qianwen.com/` · 通义千问/千问）与 `qwen-web`（`https://chat.qwen.ai/` · Qwen 国际站）是**同一家族的重复入口** ⇒ 按开发者指示**移除 `tongyi-web`、只保留 `qwen-web`**：现行 web 清单由 12 条（DOM 11 + `deepseek-web`）变为 **11 条**（DOM 10 + `deepseek-web`）、配置表 21 条 → **20 条**；下表已删行，`source/workflows/testAllAI.json` 随之 85 节点 / 84 连线 → **82 / 81**。上文 v15 的 `12/12` / `12 条` 等数字是**当时**的实测记录，**保持不变**（历史不改）；证据台账同步删块（`source/assets/site_capability_evidence.json`）。
- **v15 逐站直连实测结果（2026-09-27，详见 [网页版协议实测记录 §7.9](../Archive/网页版协议实测记录.md)）**：
  - **「打开登录窗口」= 12/12 全部真的打开各自的站点** ✅（不再出现「不管选哪个都是 DeepSeek」）；其中 **3 条域名按实测修正为最终域**：`kimi-web`（→ `www.kimi.com`）、`tongyi-web`（→ **`www.qianwen.com`**）、`ernie-web`（→ `wenxin.baidu.com`）。
  - **输入框选择器已实测 4 条**（`kimi-web` = `div.chat-input-editor`；`qwen-web` = `textarea[placeholder="Ask Qwen"]` + 发送 `div.message-input-right-button-send`；`yuanbao-web` = `div.ql-editor.ql-blank`；`ernie-web` = `#chat-textarea`）→ 已写回 `source/assets/providers.json`，`--provider-dump` 确认这 4 条**只剩 `answer_selector` 未就绪**。
  - `chatgpt-web` / `claude-web` / `gemini-web` 在本机网络下**均可直连**（境外三站不再是「未核实」）；`chatglm-web` 首屏为 **WAF 挑战页**（需人工过验证）。
  - **仍未就绪**：10 条的 `answer_selector`（回答容器必须**先有一条回答**才存在 → 需人工登录 + 手动发一条消息后再跑 `--web-dom-dump` 取回）。


- **为什么没有选择器**：`adapter=dom` 的生成字段（`input_selector` / `send` / `answer_selector`）与 `cookie_names` / `token_expr` **必须实测**；本轮**不填假值**（避免 L3 落地后误判「已适配」）→ 采用**登录型条目**（`PB2-26`：可加载 + 生成未就绪如实标记）。
- **合规**：只做**有头登录 + 用户手动 + 不代填密码 + 不绕过验证**；自动化生成需 L3 落地，且由用户自担各站点 ToS 风险（与 `R19` 同族）。
- **自行新增站点**：① **界面**（2026-10-10 起）= 「设置 → 提供商与站点表 → **新建网页版站点…**」（`PB2-24` ✅）；② **手工**照**附录 D** 格式放进 `~/.brain-ai/providers.d/` 后点面板「**重新加载配置表**」即生效（**不必重启** —— `PB2-24` ✅）。

---

## §9 决策点

> ✅ **2026-10-08 全量梳理（口径见 [../DocMAP.md](../DocMAP.md) §五）**：本节的**未拍板项已登记到看板** [../DevPlan.todo](../DevPlan.todo) 的「**待定 · 未拍板**」分类 ——
> ✅ **2026-10-09 拍板落盘**：`D-26` → **`PQ-09`** ✅（② · 2026-10-11）· `D-30` → **`PQ-10`** ✅（①）· `D-31` → **`PQ-11`** ✅（③）· `D-32` → **`PQ-12`** ✅（① · 2026-10-11）；`D-25` / `D-27` / `D-28` / `D-29` **✅ 已按实现收口（原文保留，只补拍板留痕）**。
> ⛔ **原文不删**：下表是拍板依据；任何一项拍板后，同批改本表 + `DocMAP` §5.3 + 看板 `PQ-xx`。
> ✅ **2026-10-09 拍板落盘**：`D-30`（`PQ-10` = ①）· `D-31`（`PQ-11` = ③）**已拍板**并就地改 ✅（原文保留作拍板依据）；✅ **2026-10-11：`D-26`（`PQ-09` = ②）· `D-32`（`PQ-12` = ①）已拍板** ⇒ **本节决策点全部结案**（`D-25`~`D-32` 共 8 条：**已定 8 / 待定 0**）；**零代码改动**（`D-26` 与现行实现一致 ⇒ 只勾账；`D-32` 的落地 = 真机批逐站回填）。

> 承接 `M_patchB` §6 与 `M_patchA` §12 的**未决项**（已定项见归档 `D-21`/`D-22`/`D-23`/`D-24`，`D-01…D-07` 见归档 `M_patchA` §12）。

| 编号 | 议题 | 选项 | 状态 / 建议 |
|---|---|---|---|
| **`D-25`** | `mode=web` + **非网页版条目**时，运行前校验是否**阻断** | ① 阻断（`validateBeforeRun` 返回 false）② **不阻断**，但文案「**本次运行必然失败**」+ 三条引导 + 节点 `NodeError` | ✅ **实际已按 ② 落地**（v9 `PB2-22`）—— 仅补拍板留痕 |
| **`D-26`** | 站点条目的**字段级**回落是否收紧 | ① 维持（`login_url` 空 → 回落内置默认站点）② **`login_url` 缺失 = 明确报错（不回落）**；`window_title` / `probe_paths` / `token_expr` 可回落 + 警告 | ✅ **已拍板（2026-10-11 · `PQ-09` 结案 = 选 ②）**：**`login_url` 缺失 = 明确报错（不回落内置默认站点）**；`window_title` / `probe_paths` / `token_expr` 可回落 + **警告**（与 `I14` 一致）。**复核结论（2026-10-11）= 与现行实现一致 ⇒ 拍板 + 勾账 · ⛔ 无代码项**：`source/ai/src/provider_spec.cpp:346` 的 `web_site_error()`、`source/ai/src/dom_web_client.cpp:944` 同判、`provider_spec.cpp:365` 的 `web_site_field_warnings()` 已给警告。原文（拍板依据）：⬜ 待定（`PQ-09`）（建议 ②） |
| **`D-27`** | 「**已登录**」的判据（L4） | ① 该 origin Cookie 非空 ② 条目 `cookie_names` 命中 ③ 页面可达 / 用户确认 | ✅ **已按 ①∪② 实现**（v13 `PB2-27`：`web_session_state()` 纯函数，**不看** `userToken`）—— ✅ **已固定（2026-10-08）** |
| **`D-28`** | 通用站点是否显示 `userToken` | ① 保留，但**仅当条目配了 `token_expr`** 才显示 ② 彻底移除该行 | ✅ **已按 ① 实现**（v13）—— ✅ **已固定（2026-10-08）** |
| **`D-29`** | L4 归口 | ① `PB2-24` 并入 L4 本批；`PB2-25` 一并 ② 保持分散 | ✅ **已按 ① 收编**到本文（`B2-e2`）—— ✅ **已固定（2026-10-08）** |
| **`D-30`** | **登录态判据收紧**（v15 实测：**未登录页面已有匿名 Cookie** → 「Cookie 非空 = 已登录」会**误报**） | ① **`cookie_names` 命中优先**，Cookie 非空降级为「**未校验**」 ② 保持现状（Cookie 非空 = 已登录，接受误报） | ✅ **已拍板（2026-10-09 · `PQ-10` 结案 = 选 ①）**：**`cookie_names` 命中优先**，Cookie 非空降级为「**未校验**」；**⚠️ 代码落地项（非待定 · 本轮未动代码）** = `web_session_state()` 判据收紧 + 逐站补 `cookie_names`（数据回填归 `D-32` / `PQ-12` 范围）。原文（拍板依据）：⬜ **待定（`PQ-10`）**（建议 ①）；**证据已到手（2026-10-02）**：豆包登录前后 Cookie 名差集 = 净新增 **23 名**，其中 **14 个鉴权名**（`M7.md` §12.1 ⑤ / [Archive/actionPlan/M7B.md](../Archive/actionPlan/M7B.md) §10）⇒ **只差拍板 + 逐站补 `cookie_names`** |
| **`D-31`** | **流式方案**（§2.5） | ① 升级/替换 httplib ② 网页版改「POST + GET 轮询正文」（需先探 A′ 端点）③ 明确不做、文案改「分段落定」 | ✅ **已拍板（2026-10-09 · `PQ-11` 结案 = 选 ③）**：**明确不做**（网页版文案改「分段落定」）+ 补 `VB-03` 离线断言；A′ 端点探查**确定不启动**（原文保留：⬜ 待定（`PQ-11`）（建议 ③ + 补 `VB-03` 离线断言）） |
| **`D-32`** | `PB2-29` 逐站回填的**范围** | ① 只做国内 6 站（Kimi / Qwen / 豆包 / 元宝 / 文心 / 星火）② 连境外 3 站（ChatGPT / Claude / Gemini，本机可直连） | ✅ **已拍板（2026-10-11 · `PQ-12` 结案 = 选 ①）**：**只做国内站点 6 站**（`Kimi` / `Qwen` / `豆包` / `元宝` / `文心` / `星火`）；**境外 3 站（`ChatGPT` / `Claude` / `Gemini`）不在本批**（本机可直连，留作后续可选）；看板原文「国内 7 站」= `T-L26` 去重前口径 ⇒ **本批订正为 6 站**。**⚠️ 落地项（非待定 · 本轮未动代码）** = `PB2-29` 逐站实测回填（`verified:true` + 附录 E 两列 · 归**真机批**）。原文（拍板依据）：⬜ 待定（`PQ-12`）（建议 ① 先做，② 可选） |

**已定但需留痕**：`D-04` i18n 默认 ②（移除并记 M6；**`config` 侧已于 2026-10-09 移除 · `PQ-18②`**）；`D-08` 范围 = L1+L2+L3（L4 追加立项）；`D-09` `InferenceProvider` 不新开 `generateStream`/`generateWithImage` 虚函数；`D-19` 多站点会话键 = **站点 origin**；`D-20` 多站点窗口策略 = **串行复用单窗口**（并发后置）。

---

## §10 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-09-27 | **v1** | **建立本文（文档拆分）**：① 收编 `M_patchA`（Patch B 残项 / 原 Patch C / 原 Patch D / PM）与 `M_patchB`（L2 的 `PB2-04…12`、`PB2-07` 界面按钮、`PB2-25`、`PB2-29`/`PB2-30②`、`PB2-24`）的**全部未完成项**；② 刷新回归基线（构建 0/0 · `--exec-selftest` **237/0** · `--provider-selftest` 50/0 · `--graph-selftest` 111/0 · 文档断链 0）；③ 附录 B/C/D/E 复制为**现行版本**并在附录 E 就地维护「可登录 / 可生成」两列；④ 承接决策 `D-25`…`D-30` 并新增 `D-31`（流式方案）/ `D-32`（逐站范围）；⑤ 同批把 `M_patchA.md` / `M_patchB.md` **归档**到 [../Archive/actionPlan/](../Archive/actionPlan/)（已归档部分见其顶部横幅）。 |
| 2026-09-29 | **v2** | **方案 B′ 口径同步（只改文档 · 零代码改动）**：① §9 **`D-30`** 补**证据计划** —— 豆包「登录前后 Cookie 名差集」随 `M7.md` 新增的 `P7b-05b`（`source/python/_probe/m7b28_doubao_recon.py` B0 段）一并取证；② **附录 E** `doubao-web` 行补注「**网页版图片理解首个目标站（方案 B′ · 2026-09-29）**、`P7b-05b` 侦察先行」，**「可生成」列保持 ⬜**（不填假值）；③ 背景：`M7.md` 新增决策 `D10`（首个目标站 = `doubao-web`；`deepseek-web` 保持文字主线）+ 前置验证 `P7b-05b` + 任务 `M7-10`（DeepSeek 官方 API 视觉跟进），站点多模态能力公开证据核查见 [../网页版协议实测记录.md](../Archive/网页版协议实测记录.md) §8。**未改任何产品代码与配置表** |
| 2026-10-09 | **v3** | **待定项拍板落盘（只改文档 · 零代码改动）**：`D-30`（`PQ-10` = ① `cookie_names` 命中优先）· `D-31`（`PQ-11` = ③ 明确不做 + 文案改「分段落定」）就地改 ✅ 并留「原文（拍板依据）」；`D-26`（`PQ-09`）· `D-32`（`PQ-12`）**维持待定**。同批：§1 第 23/30 行、§2.5、§5 第 24/25 行、§7 `FEA-M3-07` 行同步改 ✅；[../DocMAP.md](../DocMAP.md) §5.3 · [../DevPlan.todo](../DevPlan.todo) · [../CHANGELOG.md](../CHANGELOG.md)。**⚠️ 已登记但未落地的代码项**：`D-30` ① 的判据收紧（`web_session_state()`）、`PQ-18` ② 的配置字段移除与断言登记。**（订正（2026-10-09 · v4）：其中 `PQ-18②` 已同日落地 ⇒ 见 v4 行；`D-30` ① 仍待。）** |
| 2026-10-09 | **v4** | **`PQ-18②` 代码落地（`config` 字段清理 · 代码 + 文档；含 §5「明确不做」补 `PD-08`/i18n）**：① `utils/config.h` 删 `General` 结构体与 `Config::general` 成员（并注明「旧 `[general]` 节静默忽略、保存不再写回」）；② `utils/config.cpp` 删 `read_general()`、写入端 `[general]` 节、加载端读取，未接清单 **8 → 6 项**；③ `tools/api_probe.cpp` 配置往返不再拿 `general.language` 当探针字段、`PA-08` 断言由「清单须含 `general.language`」改为「**不含** `general.*`」并同步打印文案。**实测（2026-10-09 本机）**：构建 **0 error / 0 warning**；`--selftest` 七组全 PASS · exit **0**（配置表 50/0 · 图片准备 19/0 · 上传证据 32/0 · 上传模板 21/0 · 执行器 **425 / 0**）；`--exec-selftest` **425 / 0** · `--graph-selftest` **110 / 0** · `--upload-selftest` **72 / 0** —— **数字全无变动**（`PA-08` 为单行 PASS/FAIL，非计数断言 ⇒ `PQ-05` 契约下无需新增登记行）。运行时物证（临时 `USERPROFILE`，未碰用户 `~/.brain-ai`）：新生成 `config.toml` **无 `[general]` 节**；含 `[general]` 的旧配置**加载正常**（`aiwrite.exe --upload-selftest` exit 0）。同批：[../CHANGELOG.md](../CHANGELOG.md) · [../DevPlan.todo](../DevPlan.todo) · [M6.md](M6.md) · [../DocMAP.md](../DocMAP.md)（§5.3 表 3 行）· [../ai_writer_nodes.md](../ai_writer_nodes.md)（§19.1 多语言行）。**⛔ 未提交**（按 [../../AI_README.md](../../AI_README.md) §6）。 |
| 2026-10-09 | **v5** | **批 3 · 网页站批量探测报告链路落地（代码 + 测试 + 文档）· 本文只动 §0.4 基线**：刷新为 2026-10-09 实测 —— `--exec-selftest` **425 → 452**（新增 27 项 DOM 探测报告自检）· `--graph-selftest` **110 / 0**（未变）· 新增三行（批量探测 `--provider all`（`FEA-L01`/`FEA-L03`：11 站 = 可用 8 / 需登录 2 / 失败 1 · exit 1）· 会话清单 `--web-session-list`（`FEA-L02`）· 离线夹具含 `dom_gate_test`（15/0；`--old` 6/15））；断链口径由「broken 0」改为「**本轮不得新增**」（实测 checked 610 / broken 8，8 条全部为存量）。逐项背景 / 物证见 [../CHANGELOG.md](../CHANGELOG.md) 批 3 条目与 [../loop_dev.md](../loop_dev.md) §3.1 · §4 · §6.2 · §6.3 · §10。**⛔ 未提交**（按 [../../AI_README.md](../../AI_README.md) §6「改完即停并汇报」）。
| 2026-10-10 | **v6** | **`PB2-24` 自建站点闭环落地（代码 + 离线断言 · 本文 §1 第 18 行 / §3.5 / 附录 D 就地改 ✅）**：① 新增 `source/ai/{include,src}/site_template.*`（模板纯函数 + **原子写** + 默认拒覆盖；**强制信封 / `adapter=dom` / `verify=false`**）；② `source/engine/{include,src}/node_registry.*` 新增 `provider_option_ids()` / `findMutable()` / `provider_param_options()` / `refresh_provider_options()`（2 重载，带 `Graph&` 时**保留已选值**）/ `reload_provider_specs_and_refresh()`（单入口）；③ 设置面板 §⑥ 新增「**新建网页版站点…**」模态 + 「**重新加载配置表**」+ 错误/警告明细 + 打开站点目录，`SettingsPanelResult.provider_table_reloaded` ⇒ `app.cpp` 刷新画布内已有节点实例枚举；④ 新开关 `api_probe --site-assert`（纯函数 **47 / 0** + 端到端：写盘 → 进表 → 进下拉 → 进枚举 **20 → 21** → 新节点校验 → **旧实例刷新回归** → 清理零残留；**模拟应用启动次序**）并接入 `check_all.ps1`（**12 → 13 项**）。**验证（2026-10-10 本机）**：构建 **0 / 0**；`--site-assert` **PASS · exit 0**；`check_all.ps1` **13/13 PASS**；`--selftest` / `--exec-selftest` **464 / 0** / `--graph-selftest` **110 / 0** 无回归；GUI 冒烟无崩溃。**⛔ 界面点选回路未人工实测**（需真人点一次）· **`PB2-30②` 仍未做**。同批：[../CHANGELOG.md](../CHANGELOG.md) `[Unreleased]` · [../变量术语对照表.md](../变量术语对照表.md)（§10.2 · §14 附表 · §15）· [../roadmap.md](../roadmap.md) §十二 第 2 条 · [../DevPlan.todo](../DevPlan.todo) `FEA-M_patchB-29…30` · [../AI_Status.md](../AI_Status.md) §一/§四 · [M6.md](M6.md) `M6-10`。**⛔ 未提交**（按 [../../AI_README.md](../../AI_README.md) §6「改完即停并汇报」）。 |
| 2026-10-10 | **v7** | **`PC-01` / `PC-02` 自动保存 + 崩溃恢复落地（代码 + 离线断言 · 本文 §1 第 19 行 / §4 两行就地改 ✅）**：① 新增 `source/engine/{include,src}/autosave.*`（`autosave_paths` / `autosave_probe` / `graph_content_signature`（不含 viewport）/ `autosave_write`（**原子写 + 先复制旧最新成 `autosave_1` ⇒ 无缺失窗口**）/ `autosave_load` / `autosave_clear` / `autosave_cleanup_temp` / `autosave_should_offer_restore` / `autosave_selftest`）；**落点调整（如实登记）**：原计划 `ui/editor_state.*`，因 `ui/**` 属 GUI 可执行目标、`api_probe` 链接不到 ⇒ 纯逻辑下沉 `engine/` 才能离线断言 `VC-01`/`VC-02`，`ui/` 只保留**时机编排**（`tick_autosave` 1 Hz 指纹探测 + 静默 `ui.autosave_idle_sec` 秒 + 无变更不写 + 失败退避 10 秒 + `flush_autosave_on_exit`）与**启动闸门** `autosave_paused`（防启动示例工作流冲掉真快照）；② `ui/app.cpp` 启动序 = 清残留 `.tmp` → 判定 → 模态（恢复 / 忽略 / 忽略且不再提示），**恢复后 `current_workflow_path` 留空 ⇒ 保存必走另存（不覆盖原文件）**；③ 配置新增 `ui.autosave_enabled` / `autosave_idle_sec` / `autosave_keep` / `autosave_restore_prompt`（设置面板 **§⑦ 数据安全（自动保存）**；`--settings-assert` 越界例 **8 → 11**）；④ 新开关 `api_probe --autosave-assert`（**28 / 0**）并接入 `check_all.ps1`（**13 → 14 项**）。**验证（2026-10-10 本机）**：构建 **0 / 0**；`--autosave-assert` **PASS · exit 0**（28/28）；`check_all.ps1` **14 / 14 PASS · exit 0**（`check_all_20261010_202628.log`）；GUI 实测（真机）：启动判定日志出现 + **12 秒后快照哈希不变**（种子事后删除，零残留）。**⚠ 同批踩坑 → 陷阱 10**：`Param::value` 是 `nlohmann::json`，**`value += "文本"` 语义 = `push_back`**（对字符串抛 `type_error` → `terminate` → `0x80000003`，stdout 缓冲丢失 ⇒ 看不出跑了），已改显式赋值 + 自检逐项 `fflush`。**⛔ 未验证（如实登记）**：GUI「改图 → 静默 3 秒 → 写快照」与「点『恢复』」需真人点一次鼠标（同一代码路径已由 `--autosave-assert` 覆盖）。同批：[../CHANGELOG.md](../CHANGELOG.md) `[Unreleased]` · [../DevPlan.todo](../DevPlan.todo) `FEA-M2-01` / `TST-M2-03` · [../AI_Status.md](../AI_Status.md) §一/§四 · [../变量术语对照表.md](../变量术语对照表.md) §6/§9/§10/§14/§15 · [../节点编辑器使用说明.md](../节点编辑器使用说明.md) §10.13 · [../loop_dev.md](../loop_dev.md) §10.1 陷阱 10 · [../../source/README.md](../../source/README.md) · [../../AI_README.md](../../AI_README.md) §2。**⛔ 未提交**（按 [../../AI_README.md](../../AI_README.md) §6「改完即停并汇报」）。 |
| 2026-10-11 | **v8** | **待拍板项清零（`PQ-09` / `PQ-12` · 只改文档 · ⛔ 零代码改动）**：`D-26`（`PQ-09`）✅ **= ②**：`login_url` 缺失 = **明确报错（不回落内置默认站点）**，`window_title` / `probe_paths` / `token_expr` 可回落 + 警告 —— **复核结论 = 与现行实现一致**（`source/ai/src/provider_spec.cpp:346 web_site_error()` 已报「**不会**回落到内置默认站点」；`web_site_field_warnings()` 对其余字段给警告；`source/ai/src/dom_web_client.cpp:944` 侧同判）⇒ **拍板 + 勾账 · 无代码项**；`D-32`（`PQ-12`）✅ **= ① 只做国内站点（6 站）**：名单 = `Kimi` / `Qwen` / `豆包` / `元宝` / `文心` / `星火`（看板原文「国内 7 站」= `T-L26` 去重前口径 ⇒ **本批订正为 6 站**），境外 3 站（`ChatGPT` / `Claude` / `Gemini`）**不在本批**；`M8-Q4`（`PQ-06`）✅ **= 沿用 `v0.5.4`**（见 [M8.md](M8.md) §7）。同批：[../roadmap.md](../roadmap.md)（`RM-D6` · §六注 · §十改判 · `RQ-1`/`RQ-4`/`RQ-5` · 头部 2 行）· [M8.md](M8.md) §7 · [../ai_writer_nodes.md](../ai_writer_nodes.md)（§4.4 标题 · P7-b 行）· [M_node_expansion.md](M_node_expansion.md)（v2：`NE-D1` + `NE-Q2` 已答）· [../DocMAP.md](../DocMAP.md) §5.3 · [../DevPlan.todo](../DevPlan.todo)（六项置 done + 分组计数 + 待定组清零）· [../CHANGELOG.md](../CHANGELOG.md)。**验证**：文档断链 **0**（见 §0.4）；**⛔ 未提交**（按 [../../AI_README.md](../../AI_README.md) §6「改完即停并汇报」）。 |




