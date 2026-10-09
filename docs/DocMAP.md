# AIwrite 文档中心

> 项目：AI 小说/剧本创作软件（节点工作流 + DeepSeek 后端）
> 当前进度：**M1 ✅ / M2 ✅**；**M3 🟡**（节点编辑器）、**M4 ✅ 核心完成**（文本生成链路）、**M5 🟡 核心切片完成**（图片理解 + 图片显示）、**M7 ✅ 第一轮代码与手册落地 / ✅ 第二轮 P7-a 已落地（v0.5.2）/ 🟡 P7-b 计划中**（第一轮 = 图片输入收口 + 节点精简 9→8 + 图片格式按内容嗅探/双解码后端 + **一次 CRT 断言崩溃的根因修复与冻结区 `I17`**；**第二轮 P7-a ✅ 已落地（2026-09-28）**：图片**多变长**端口 + **多选图片** + **统一资源目录**（`~/.brain-ai/assets/images/` 内容寻址 + `aiwrite-asset:` 令牌 + 一键迁移）+ official 收口（编码缓存 / 体积与耗时 / 三类错误文案）+ **UI A 档**（默认布局常显 / 空状态 / 面板三组折叠 / 图片卡片 / 错误条居中 / 状态栏 / 缩略图放大）—— 见 [actionPlan/M7.md](actionPlan/M7.md) §11–§14；**P7-b v0.5.3**（待确认）：网页版图片上传 = Pydoll 独立浏览器 + Python 守护进程 + 命名管道，**先做前置验证，未过则回滚**；**方案 B′（2026-09-29）：网页版图片理解首个目标站 = 豆包 `doubao-web`，先做只读侦察（`P7b-05b`），结论审核通过后才启动 C++↔Python 管道本体；`deepseek-web` 保持文字主线不动**）；补丁系列：`Patch A` ✅ 全部完成、`M_patchB`（Provider 可插拔化）**L1 已收口 ✅ / L3 ✅ / L4 部分落地**，**前两期已完成部分已归档**（[Archive/actionPlan/](Archive/actionPlan/)）—— **补丁系列所有剩余项统一见 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md)**（要点：配置表 / 用户覆盖 / API 参数化 / **网页版去硬编码** / **多站点会话并存** / `config.toml` 多 provider / `--provider-selftest`；**L4（登录层去 DeepSeek 化）**、**L2 工厂**、**原 Patch C/D/PM** 均归入承接文档）；**M7 第三轮 `M7B` 已立项（计划中）**：网页通道**整体迁 Pydoll** —— WebView2 **退场**（嵌入控件易被站点识别为非真实浏览器）、`builtin:deepseek` 协议栈退役、**CDP `Network` 事件重建 SSE 增量**（保住逐字流式）、**只维护一套登录态**、**作废不变量 `I2`**（改由 `I20` CLI 契约替代：`--provider` 必填 / 缺参列候选 + 退出码 2）—— **前置验证 7/9 已过**（`M7B-01`/`02`/`03`/`04`/`05`/`08`/`09`；余 `M7B-06`/`07` 真站点登录 + `M7B-06b`）· **批 1 step 1~6 已落地**（协议词表 / 命名管道 / Pydoll 驱动 / 守护进程主循环 / **L2 加密快照 `session.py`（`VB2-39`）** / **C++ 新通道 `pydoll_channel`（`--pydoll-selftest` 已过）**；**生产路径未接线**）· **目录迁移完成（2026-10-03）**：Python **运行代码**归 `source/python/`（含 `_probe/` 与 `.venv`），**运行期数据仍留 `~/.brain-ai`**，并新增决议 **`MB-D0-8`**（登录态持久化四层：**L1 干净退出 / L2 DPAPI 加密快照 / L3 启动自愈 attach / L4 异常退出可见**）· **⚠️ 运行时通道现状（2026-10-03 实测）：主程序 GUI 与网页节点仍走 WebView2**（Pydoll 新通道已建成但**未接线**，仅 `--pydoll-selftest` / `--pydoll-login <id>` 可用；切换归批 3 `M7B-20`；逐入口归属见 §1.4）—— 📦 **`M7B` 已归档（2026-10-05 · 路线退场）** → [Archive/actionPlan/M7B.md](Archive/actionPlan/M7B.md)；**M7 第四轮 `M8`（方案 E）已立项**：网页通道**文字生成回退 WebView2** + **图片上传改走 C++ native HTTP**、Python / 命名管道 / 守护进程整体退场 —— 见 [actionPlan/M8.md](actionPlan/M8.md)；M6 未开始
> 最近更新：2026-10-09
> 文件名：**`DocMAP.md`**（2026-10-08 由 `README.md` 更名 · `git mv` 保留历史；历史文档（含 `docs/Archive/**`）里出现的 `docs/README.md` 均指本文）

---

## 一、文档地图

| 文档 | 内容 | 何时看 |
|---|---|---|
| [ai_writer_nodes.md](ai_writer_nodes.md) | **项目设计文档 v1.0**：需求、架构、数据结构、节点、执行引擎、UI、日志、配置、开发计划、附录 | 了解整体设计 / 写代码前对齐规范 |
| [actionPlan/milestone_plan.md](actionPlan/milestone_plan.md) | 里程碑总体计划（M1–M6 + **M7**）与节奏、总体验收对照 | 看整体排期与当前进度 |
| [roadmap.md](roadmap.md) | **长期路线图 v1.0（一页纸）**：定位（面向非技术用户 / 可能转商业）、阶段总览、P7-a → P7-b + 面向 C 前置项 → **M6 一键安装包** → 滚动发布 → 商业候选；含 **`RM-D1`~`RM-D6` 决策**、风险与回滚、不做清单、待确认项与"待同步差异" | 判断"这个改动该不该做 / 排在哪个阶段"时先看它 |
| [actionPlan/M7.md](actionPlan/M7.md) | **M7 计划（图片输入收口 + 节点精简）**：删除图片输出节点（9→8）、图片格式**按内容嗅探**、本地解码 **stb/WIC 双后端**、可操作报错、诊断入口 `--image-decode`；**§1 记录了 CRT 断言崩溃（退出码 3 / `0x80000003`）的根因与冻结区 `I17`** | 排查「某张图无法识别/预览」、改图片解码相关代码前**必读**；做 P7（图片内部运行 / 网页版图片上传 / UI A 档）前**必读** §8–§17 |
| [actionPlan/M8.md](actionPlan/M8.md) | **M8 计划（网页通道方案 E）**：文字生成**回退 WebView2** + 图片上传改走 **C++ native HTTP**（Pydoll / Python / 命名管道 / 守护进程**整体退场**）；Phase 0 归档 / Phase 1 回滚 / Phase 2 Gate 验证 / Phase 3 退场 / Phase 4 解禁 / Phase 5 可选 | 做**网页版图片上传**、或判断「网页通道走哪条路」时**先读** |
| [actionPlan/](actionPlan/)（M3–M6 + 补丁 A/B） | **进行中 / 待开发**的里程碑与补丁 Action Plan（**补丁系列剩余项 [M_patchAB_rest.md](actionPlan/M_patchAB_rest.md)**；已归档的 [Archive/actionPlan/M_patchB.md](Archive/actionPlan/M_patchB.md) Provider 可插拔化 / JSON 配置表：**L1 已收口**（含网页版去硬编码与多站点会话），第二轮修订 v6 的修复项 `PB2-20` **已落地（v7）**（`mode` 恒按 `D-21` 提供 `official`/`web` 两项、面板与校验按**节点自身条目**解析站点）；另有 v9 修复项 `PB2-22`/`PB2-23` **已落地**（**站点不回落** + 三条引导 + 按条目 `--web-probe --provider`）与 v10 的 `PB2-26`（**登录型站点条目**：内置 **11 个 AI 的网页版登录入口** —— Kimi / 通义 / Qwen / 智谱清言 / 豆包 / 元宝 / 文心 / 星火 / ChatGPT / Claude / Gemini；**附录 D/E**）；**L3 已落地（v11）**（通用 DOM 站点适配器：条目补齐选择器即**真正生成**；选择器诊断 `--web-adapter-selftest --provider <id>`）；**L4 立项（v12 · 登录层去 DeepSeek 化 + 站点数据落地 + 自助闭环 —— `PB2-27`…`PB2-30` / `B2-e` / `I15`·`I16` / `AB2-20`…`AB2-22`，见该文档 §9.7）**；剩余 L2、`PB2-25`（会话失效可诊断）与 `PB2-07` 界面按钮，见 [M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §1–§9（承接文档）与已归档的 `Archive/actionPlan/M_patchB.md` §9.x / 附录 D/E —— **现行规格以承接文档 §8 附录为准**） | 开发某个里程碑 / 补丁前 |
| [Archive/actionPlan/](Archive/actionPlan/)（M1 · M2 · M_rerun · M_textio · **M_patchA · M_patchB** · **M7B** · **M7_P7b_pydollRoute**） | **已完成**的里程碑 / 补丁计划（归档保留；补丁系列 2026-09-27 归档，剩余项见 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md)） | 追溯历史决策与验收依据 |
| [Archive/M1_技术验证报告.md](Archive/M1_技术验证报告.md) | M1 实测环境、分层验证结果、V/A 验收对照、问题与解决记录 | 想知道“为什么这么搭环境/踩过哪些坑” |
| [节点编辑器使用说明.md](节点编辑器使用说明.md) | **M2 节点系统 P1/P2** 操作手册（鼠标操作全表、**8** 节点说明、参数校验、撤销规则、自检命令、20 项人工验证清单、已知限制；§10.9 图片理解/显示 + **§10.10 P7 计划中的人工步骤**） | 上手操作节点画布 / 做人工验收 |
| [变量术语对照表.md](变量术语对照表.md) | **中文术语 ↔ 代码标识符对照表**：目录/命名空间、`ed::` 别名、图模型（`Graph`/`Node`/`Port`/`Param`/`Edge`）、节点类型与**端口/参数 id**、画布交互与 `Config` 按键、**鼠标按钮编号（中键 = 2）**、执行/运行、提供商、`config.toml` 键 ↔ 字段、`~/.brain-ai` 数据目录、自检开关、命名与补丁编号约定；**§12 已知文档≠代码**、**§13 新增功能检索入口** | **搜不到代码里的名字时先查这里** |
| [actionPlan/M_patchC_web_node_bind.md](actionPlan/M_patchC_web_node_bind.md) | **补丁系列第三期（🆕 2026-10-06 · 🟢 已拍板 2026-10-07）**：网页版「节点 ↔ 窗口」绑定与页面就绪判据 —— 真机归因（`imgTest` 三次运行）· 不变量 `I24` / `I25` / `I26` · 计划 `PW-01`…`PW-13` · **已拍板 `D-33`…`D-36`**（②独立窗口真并行 / ①`answer_selector`可选 / ③复位首次+失败 / ②`web.attach`诊断字段） | 网页版**多节点 / 多运行**异常时**先读** |
| [CHANGELOG.md](CHANGELOG.md) | 全部重要变更（新增/修复/变更/移除）与验证项结果 | 每次看进度、排查“什么时候改的” |
| [../source/README.md](../source/README.md) | 源码结构、构建、运行、调试（VS Code 任务/配置）、常见问题 | 动手构建或运行程序 |
| [../AI_README.md](../AI_README.md) | **AI / 协作者行为契约**：读取顺序 · 命令表与当前基线 · 取证纪律 · 改动伴随义务 · **提交纪律（⛔ 禁止擅自提交）** · 编号与登记约定 · **表达规范（写给人类的文字）** · 禁止项 · 路径速查 | **接手任何改动前先读**（AI 助手 / 新协作者必读） |

---

## 二、快速开始（人类最短路径）

```powershell
cd F:\GameDao\Tools\AIwrite\source
.\build.ps1                                   # 或：cmake --preset default && cmake --build --preset debug
F:\GameDao\Tools\AIwrite\build\bin\aiwrite.exe
```

- VS Code：`Ctrl+Shift+B` 构建，`F5` 调试（配置见 `.vscode/launch.json`），`Terminal → Run Task` 还有 release / 重装依赖 / 自检 / 看日志等任务。
- Visual Studio 2026：打开 `build\aiwrite.slnx`，选 `aiwrite` 后 F5。
- 验证工具：`api_probe.exe --selftest`（SHA3 + HTTP + 请求构造 + 配置往返）、`webview2_login.exe --selftest`（WebView2 Cookie 链路，退出码 0=PASS）。

---

## 三、当前状态（M1 验收对照）

| 项 | 状态 |
|---|---|
| 构建 / 运行 / 中文界面 / 日志 / 配置（A-01、A-02、A-07、A-08） | ✅ 已验证 |
| HTTP、SHA3（V-04、V-05） | ✅ 已验证 |
| 节点画布渲染与交互（V-02、A-03） | ✅ 已升级为真实节点系统（M2 P1/P2）：节点/连线/参数/撤销全部可鼠标操作，逻辑层自检 70 项全绿 |
| WebView2 登录 + Cookie（V-03 / A-04、A-05） | 🟡 自动化链路已通过（`webview2_login --selftest` → Cookie 5 条）；⏳ 运行 `webview2_login.exe` 手动登录一次 |
| DeepSeek API 生成（V-06 / A-06） | ⏳ 需设置 `DEEPSEEK_API_KEY` |
| M3 节点编辑器验收 | ⏳ 按 [节点编辑器使用说明.md](节点编辑器使用说明.md) §8 的 22 项清单人工确认 |

---

## 四、文档维护约定

1. **任何代码/构建/依赖变更** → 追加到 [CHANGELOG.md](CHANGELOG.md) 的 `[Unreleased]`（完成里程碑时归档为该版本号小节）。
2. **里程碑完成** → 更新 [actionPlan/M*](actionPlan/) 顶部状态行、[milestone_plan.md](actionPlan/milestone_plan.md) 的“状态”列，并产出该里程碑的验证报告（如 `M2_*报告.md`）。
3. **环境、构建方式、目录结构、依赖版本变化** → 同步设计文档 [ai_writer_nodes.md](ai_writer_nodes.md) 的 §2（技术栈/构建）、§18（里程碑状态）、§20（目录结构、配置示例、依赖清单）。
4. **构建/调试流程变化** → 同步 [../source/README.md](../source/README.md) 与 `.vscode/`（launch/tasks）。
5. 报告中记录的“待人工验证项”完成后，在本文件与 CHANGELOG 中把对应行改为 ✅。
6. **协作契约同步**：新增/更名文档、改命令、改基线数字、改提交纪律 → 同步 [../AI_README.md](../AI_README.md)（AI / 协作者行为契约）；其中**基线数字**以最近一次全量实测为准（[actionPlan/M_patchC_web_node_bind.md](actionPlan/M_patchC_web_node_bind.md) §7.1 记录的是各任务**落地当时**的数字，历史数字不追溯改写）。
7. **提交由开发者决定**：默认改完即停并汇报；未获明确指令不做 `commit` / `tag` / `push` / 改版本号（见 [../AI_README.md](../AI_README.md) §6）。
8. **状态标记口径**：新增 / 修订内容一律按 §五 的**三态标记**（✅ 已固定 / 📦 已失效 / ⬜ 待定）书写；`.md` 里的历史数字**不追溯改写**，只在新条目里覆盖。

---

## 五、文档状态标记口径（统一 · 2026-10-08 全量梳理）

> 起因：多轮路线变更（`M7B` 立项 → 退场 → **方案 E**）之后，活文档里**同时存在**「已落地」「已作废」「未拍板」三类内容，写法却不统一 ⇒ 本节定**唯一口径**，全库（`docs/**` 活文档 + [../AI_README.md](../AI_README.md)）照此执行。

### 5.1 三态标记（只用这三个）

| 标记 | 含义 | 标准写法 | 硬规则 |
|---|---|---|---|
| ✅ **已固定** | 需求 / 设计**已落地并视为契约**；再改必须新开条目 + 重跑回归 | `✅ 已固定（2026-10-08）` | 不得无条目地回改；**历史数字不追溯改写** |
| 📦 **已失效** | 设计**作废 / 被取代**，或整条路线退场 | `📦 **已失效（2026-10-05 · 方案 E）** → 现行 [actionPlan/M8.md](actionPlan/M8.md)` | **⛔ 只标记不删除**（原文照留，可加 `~~删除线~~`）；**必须**给「现行指向」 |
| ⬜ **待定** | **未拍板**的开口项 / 设计分叉 | `⬜ 待定（PQ-12）` | **必须**在 [DevPlan.todo](DevPlan.todo) 的「**待定 · 未拍板**」分类里有同名 `PQ-xx` 项 |

### 5.2 三条纪律

1. **不删除**：失效内容一律**保留原文** —— 它们是「为什么不做」与后续决策的证据（与 [Archive/README.md](Archive/README.md) §三 的归档规则同源）。
2. **单向**：活文档**可以**引用归档；归档文档与历史条目**不回改**。
3. **闭环**：任何 ⬜ 待定项**拍板后**必须同一批完成 —— ① 计划文档就地改 ✅/📦 ② 本文 §5.3 行更新 ③ `DevPlan.todo` 的 `PQ-xx` 置 done（或移出待定组）。

### 5.3 本次全量梳理结果（2026-10-08 · `docs/**` 活文档）

| 文档 | ✅ 已固定 | 📦 已失效（标记不删） | ⬜ 待定（→ 看板） |
|---|---|---|---|
| [roadmap.md](roadmap.md) | `RM-D1`~`RM-D4` · §十 不做清单 | 轨道 A / 轨道 C（Pydoll 路线）· `RQ-3` / `RQ-5` / `RQ-6` | `RQ-1` / `RQ-4` → `PQ-01` / `PQ-03`（`RQ-2` ✅ 已拍板 2026-10-09 · `PQ-02` 结案） |
| [ai_writer_nodes.md](ai_writer_nodes.md) | §4.4 八节点 · §6.7 纯鼠标 · §17 三批决议（T/C 全表） | `N-09` ImagePreview · §2.3 移除表 · §18.3 的 `M7B` / P7-b 行 | §19.1 延后功能去留 ✅ 已拍板（2026-10-09 · `PQ-16`） |
| [节点编辑器使用说明.md](节点编辑器使用说明.md) | §3 鼠标操作 · §4 八节点 · §6 撤销 · §10.9 / §10.10（P7-a） | §10.10 的 P7-b 表（Pydoll / 命名管道） | —（§8 清单为**待真机**，非设计待定） |
| [网页版协议实测记录.md](网页版协议实测记录.md) | §1 / §2 / §5 / §6 / §9.1 / §10.2（豆包取值） | §9.2 `s=` 算法 · 附录 A A′ 候选 · §8.3 的 `paste_only` 判定 | §10.3 Kimi 取值（待真机侦察 · 范围 = `PQ-12`）· `D-30` 判据 ✅ 已拍板（2026-10-09 · `PQ-10`） |
| [变量术语对照表.md](变量术语对照表.md) | §11 编号约定（+ `PQ-xx`）· §12 偏差表 | §12 逐条偏差（`ImagePreview` / `web.attach` / `[general]` 等 · 末项 2026-10-09 增 · `PQ-18②`） | — |
| [actionPlan/M4.md](actionPlan/M4.md) | `M4-01`~`M4-13` 核心链路 | — | —（残项归 `M_patchAB_rest`） |
| [actionPlan/M5.md](actionPlan/M5.md) | `M5-01`~`M5-08`（M5-C 核心切片） | —（`M5-03` 图片预览节点已由 M7 删除） | `M5-09` / `M5-10` 范围 ✅ 已拍板（2026-10-09 · `PQ-17`：留后 · 不排期） |
| [actionPlan/M6.md](actionPlan/M6.md) | —（未开工） | — | `A-03` 交付形态 ✅ 已拍板（2026-10-09 · `PQ-02` = Inno Setup）；`M6-02` 归 `PM-01`；i18n ✅ 已拍板（2026-10-09 · `PQ-18` = ②）—— `config` 侧 `[general]` **已移除**（代码已落地） |
| [actionPlan/M7.md](actionPlan/M7.md) | 第一轮 §1–§7 · P7-a §11 | §12 P7-b 依赖 Pydoll 的条目 · §16 旧实施序 · §17 `Q2`/`Q3`/`Q4`/`Q6`/`Q7` | §17 `Q1` / `Q5` / `Q8` 已按建议默认执行（视为 ✅）· `M7-10` → `PQ-08` |
| [actionPlan/M8.md](actionPlan/M8.md) | §13 站点上传模板（`M8-24`~`M8-37`）· `M8-Q1` 结案（`D-36②`） | §3.1 ①/③ 路线（原生复刻 · crawl `lHg`） | §7 `M8-Q2`~`M8-Q6` → `PQ-04`~`PQ-08`（✅ 4 项已拍板 2026-10-09 · 余 `M8-Q4` = `PQ-06` 待定） |
| [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) | §7 `PD-01` 不做 · §8 附录 B–E 现行规格 | §6 `PB-04`（已展开为 `PB2-*`） | §9 `D-30` / `D-31` ✅ 已拍板（2026-10-09）· `D-26` / `D-32` → `PQ-09` / `PQ-12` ⏳ 待定；`PD-08` ✅（`PQ-18` = ② 移除并记 M6；**代码已落地 2026-10-09**） |
| [actionPlan/M_patchC_web_node_bind.md](actionPlan/M_patchC_web_node_bind.md) | §9 `D-33`~`D-36` · `PW-01` / `PW-03` / `PW-07` / `PW-07g` · §5 `I24`~`I26` | §4.5 原计划「类名收窄 / 祖先排除」（未采用 · 被整串门取代） | ✅ 全部已拍板（2026-10-09）：`W-D8` → `PQ-13`（改为报错）· A2/A1 分叉 → `PQ-14`（不做）· `PW-14` → `PQ-15`（不排期） |

> **未归档的「未完工作」不算待定项**：它们已有任务号与验收（例：`PW-02` / `PW-04` / `PW-05` / `PW-06` / `PW-08` / `PW-09` / `PW-10` / `PW-11`、`M8-13`~`M8-23`、`PB2-25` / `PB2-29`、`PM-01` / `PM-03` / `PM-04`、`PC-0x` / `PD-03` / `PD-07`）⇒ 只按「计划中 / 待真机」记账，**不**进待定分类。
> **归档（只读）**：`docs/Archive/**` 按 [Archive/README.md](Archive/README.md) 冻结 —— **本次一行未改**。
