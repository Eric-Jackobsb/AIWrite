# M7B 详细 Action Plan：网页通道整体迁 Pydoll（WebView2 退场 · 文字与图片共用一条通道）

> 里程碑：M7 **第三轮 `M7B`**（与第一轮 M7、第二轮 P7 并列；排在 M6 之前 —— M6 是 v1.0 打包里程碑）
> 版本：**v0.5.4**（内部）
> 前置：M5 核心切片（M5-C）已落地；**P7-a（v0.5.2）不阻塞本文**；**P7-b（v0.5.3）与本文共用通道底座**（见 §6 批 1–2）
> 类型：**架构替换 + 运行时依赖退场**（删 WebView2；`builtin:deepseek` 协议栈退役；新增 Python + Pydoll 运行时）
> 依据：用户 2026-09-28 决议 `MB-D0-1`~`MB-D0-7`（见 §2）；承接 [M7.md](M7.md) §17 `Q2`/`Q3` —— **本文使 `Q2`/`Q3` 作废**（原「两套引擎可切换」不在本文范围）
> 状态：**计划（未开工）** —— §5 前置验证未通过前**不进入实现**（同 P7-b 模式）
> 结构：`§0` 摘要 ｜ `§1` 现状证据 ｜ `§2` 决议登记册 ｜ `§3` 目标架构 ｜ `§4` 风险与缓解 ｜ `§5` 前置技术验证 ｜ `§6` 实施任务 ｜ `§7` 配置表迁移 ｜ `§8` `I2` 解冻与 CLI 契约重建 ｜ `§9` 断言与不变量 ｜ `§10` 站点重测清单 ｜ `§11` 验证基线与文档同步 ｜ `§12` 回滚点 ｜ `§13` 合规红线 ｜ `§14` 开口项与待拍板
> 相关：设计 [../ai_writer_nodes.md](../ai_writer_nodes.md)｜第二轮 [M7.md](M7.md)｜路线图 [../roadmap.md](../roadmap.md)｜开发手册 [../../source/README.md](../../source/README.md)｜补丁残项 [M_patchAB_rest.md](M_patchAB_rest.md)｜操作手册 [../节点编辑器使用说明.md](../节点编辑器使用说明.md)

---

## 0. 结论摘要（先读这一段）

1. **触发**：2026-09-28 用户提问「文字版输入/输出是否已经用 Pydoll」。全库取证的答案是 **没有** ——
   `pydoll` 在代码里**零命中**，只出现在 5 个文档中；文字链路当前是
   **WebView2（页面脚本）+ C++ `httplib`（协议栈）** 两条老路。
2. **决议**：用户同日拍板 **7 条**（§2）—— 不再保留 WebView2（**嵌入控件容易被站点识别为非真实浏览器**），
   **全部网页版条目一律走 Pydoll**，流式用 **CDP `Network` 事件**重建，**只维护一套登录态**，
   并**直接作废不变量 `I2`**（不再承诺「无参 CLI 逐字不变」）。
3. **范围**：本文 = **通道底座 + 文字链路迁移 + WebView2 退场**；P7-b 的**图片上传专有部分**仍留在
   [M7.md](M7.md) §12，两者**共用同一个 Python 守护进程与同一条管道**（避免实现两遍）。
4. **最大代价（必须正视）**：撤掉 `builtin:deepseek` 协议栈 = 丢掉唯一「抗前端改版」的资产 ——
   今后 DeepSeek 网页版前端一改版，**文字生成会全断**；且 `deepseek-web` 从「协议驱动」变成
   「选择器驱动」，而它的页面选择器**从未实测过**（§10）。缓解见 §4 `R1`。
5. **实施纪律**：**先建后拆** —— 批 1–4 期间 WebView2 通道**保留**，批 5 才删；每批结束跑 §11 基线并贴数字。
6. **基线提醒**：作废 `I2` 会**删掉**若干旧断言（`--exec-selftest` 总数**先降后升**），
   属**预期变化**，不得误判为回归 —— 预期路径见 §11。

---

## 1. 现状证据（实测，非推断）

### 1.1 文字链路三条现存路径

| 路径 | 实现 | 证据 |
|---|---|---|
| `adapter=builtin:deepseek`（默认） | C++ `httplib` 直连：`create_pow_challenge` → 求解 → `/api/v0/chat/completion` SSE 逐行解析 | `ai/deepseek_web_client.cpp:9,15-17,19-44` |
| 同上（PoW 求解） | 借 WebView2 页面内 JS 求解 | `web/webview_host.h:216`（`solve_pow_via_page`） |
| `adapter=dom` | WebView2 页面内 `ExecuteScript`（注入提示词 / 触发发送 / 轮询答案） | `ai/dom_web_client.cpp:256,262,297` → `web/webview_host.h:207`（`run_script_sync`） |
| `protocol=openai` | `httplib` 直连官方 API（**不依赖浏览器**） | `ai/deepseek_official_provider.*` |

- 分派点：`nodes/local_nodes.cpp:415`（`adapter=="dom"`）→ `:433 dom_chat()`；否则 `:510 web_chat()`。
- 已实现白名单：适配器 `{builtin:deepseek, dom}`（`ai/provider_spec.cpp:578-582`）、协议 `{openai, deepseek-web, dom}`（`:570-576`）。
- 网页版条目共 **12 条**（`source/assets/providers.json`）：`deepseek-web` + `kimi-web` / `tongyi-web` / `qwen-web` /
  `chatglm-web` / `doubao-web` / `yuanbao-web` / `ernie-web` / `spark-web` / `chatgpt-web` / `claude-web` / `gemini-web`。

### 1.2 `pydoll` 使用情况

| 项 | 实测 |
|---|---|
| 代码命中 | **0**（`source/` 全库检索） |
| 文档命中 | 5 个：`M7.md` / `roadmap.md` / `CHANGELOG.md` / `ai_writer_nodes.md` / `节点编辑器使用说明.md` |
| `python/` 目录 | **不存在**（仓库无任何自研 `.py`） |
| `web.engine` 字段 | **不存在**（字段与代码均零命中；仅存在于 `M7.md` §17 `Q2` 的选项描述里） |

### 1.3 WebView2 接触面（撤除工单的事实基础）

`source/` 内**逐文件实测计数**（17 个文件、**约 184 处**）：

| 文件 | 处数 | 处置 |
|---|---|---|
| `tools/webview2_login.cpp` | 60 | **删除** → 新工具 `pydoll_login`（M1 的 `V-03` 记录标注「已被 `M7B` 取代」） |
| `src/web/webview_host.cpp` | 58 | **删除** → 逻辑迁 `src/web/pydoll_channel.cpp` |
| `CMakeLists.txt` | 21 | 删 `find_package(unofficial-webview2)`、`AIWRITE_WEBVIEW2` 导入目标、`webview2_login` 可执行与 link 行；改 Python 侧资源拷贝 |
| `source/README.md` | 16 | 文档改写（依赖 / 冻结区 / 排查） |
| `src/web/webview_host.h` | 6 | **删除** → 新接口头 |
| `src/ui/property_panel.cpp` | 5 | 「打开登录窗口（WebView2）」「探测网页版协议（dev）」、注销时删 profile 路径 → 改 Pydoll 文案 |
| `src/ai/deepseek_web_client.cpp` | 4 | 协议栈退役（`delta_text_of` / `web_session_failure_hint` **保留复用**） |
| `src/tools/api_probe.cpp` | 3 | 断言换代（§9） |
| `src/web/session_store.h` | 3 | 注释 + 证据来源改 CDP Cookie |
| `src/ai/dom_web_client.cpp` | 2 | 改调 Pydoll 通道 |
| `src/utils/paths.cpp` | 2 | `webview2_profile()` → `pydoll_profile()` |
| `src/utils/paths.h` | 1 | 同上（声明） |
| `src/main.cpp` | 2 | CLI 处置（§8） |
| `src/ai/dom_web_client.h` | 1 | 注释 |
| `src/nodes/local_nodes.cpp` | 1 | 注释 / 分派点 |
| `vcpkg.json` | 1 | 移除 `webview2` 依赖 |
| `build.ps1` | 1 | 构建链同步 |

---

## 2. 决议登记册（用户 2026-09-28 · 不可再议的前提）

| 编号 | 决议 | 落地含义 |
|---|---|---|
| `MB-D0-1` | **新建本文**（`docs/actionPlan/M7B.md`） | 与 [M7.md](M7.md) 并列；`M7.md` §17 `Q2`/`Q3` **作废**并回填指向本文 |
| `MB-D0-2` | **WebView2 退场**（嵌入控件容易被识别为非真实浏览器） | §1.3 的 17 文件 ≈184 处撤除；`vcpkg` / `CMakeLists` / `paths` / UI / 工具全清 |
| `MB-D0-3` | **全部网页版条目一律走 Pydoll** | `builtin:deepseek` 协议栈（PoW + `/api/v0/*` + SSE）**退役**；不再有「内置适配器」二等公民 |
| `MB-D0-4` | **用 Pydoll 的 CDP `Network` 事件重建 SSE 增量** | 逐字流式**保留**（不退化为轮询）；`delta_text_of()` 的既有双形态解析**复用**为帧解析器 |
| `MB-D0-5` | **只维护一套登录态** | 单一 profile `~/.brain-ai/pydoll-profile`；`M7.md` `Q3`（两套并存）作废；旧 `~/.brain-ai/webview2` 的登录态**格式不同、无法迁移** → 老用户需重新登录一次 |
| `MB-D0-6` | **`I2` 直接作废**，不做「无参 CLI 逐字不变」的兼容承诺 | 由 `I20`（CLI 契约）替代；**`D-22②`（站点不回落）的最后豁免区消失** → 全库口径统一（§8） |
| `MB-D0-7` | 缺 `--provider` 时 → **列候选 + 退出码 2**（严格）；另设**显式关键字** `--provider auto` 作一行式入口 | 见 §8 的 `I20` 措辞与对照表 |

> 与既有决策的关系：`MB-D0-2` / `MB-D0-3` **推翻** `M7.md` §13 表末那行「本部分**不改**网页版协议栈
> `ai/deepseek_web_client.cpp`」；`MB-D0-5` **推翻** `M7.md` `Q3`；`MB-D0-6` **解冻** `I2`（登记见 §9）。
> `RM-D3`（IPC 抽象层）/ `D7-b`（失败不静默降级）/ `P7b-14`（`humanize` 合规边界）**继续有效**。

---

## 3. 目标架构（单引擎）

```
ImGui 主线程 ── 执行器工作线程（不得阻塞 UI；M7.md Q4 = ①）
                  │  ① run_script(kickoff / poll / probe)  ② ensure_session  ③ watch_stream
                  ▼
        web::PydollChannel（C++；替代原三个自由函数）
                  │  命名管道 + Win32 事件（JSON 行协议，带版本号；阻塞式，仅在工作线程内）
                  ▼
   Python 守护进程 brain_ai_browser（asyncio）
        ├─ Pydoll 驱动真实 Edge / Chrome（headful；profile = ~/.brain-ai/pydoll-profile）
        ├─ 页面脚本：复用既有 dom_kickoff_script / dom_poll_script / 只读诊断脚本
        └─ CDP Network 订阅 → 增量帧 → 事件回传 C++ → on_delta 逐字呈现
```

| 维度 | 决策 |
|---|---|
| 引擎数量 | **1**（Pydoll）—— 无 `web.engine` 字段（`M7.md` `Q2` 作废） |
| 登录态 | **1 套**（单 profile）；Cookie 经 **CDP** 读取（含 HttpOnly，强于 `document.cookie`） |
| 会话存储 | `SessionStore` 只做**内存快照**，不落盘（`I15′`） |
| 浏览器与标签 | **一个浏览器进程、多标签**；`D-20`（串行复用同一窗口）作废 → 每站点一个 tab（`MB-D2`） |
| 站点来源 | **只来自条目** `web.*` 字段（`I14` / `D-22②`，**无回落**） |
| 流式 | 主路线 = CDP 增量；兜底 = DOM 轮询（**必须显式标注**，`I22`） |
| 退出/降级 | 任一依赖缺失 → 报错 + 引导，**绝不静默降级**（`I21`） |

---

## 4. 风险与缓解（评审重点）

| 编号 | 风险 | 缓解 |
|---|---|---|
| `R1` | **撤掉协议栈 = 丢掉唯一「抗前端改版」资产**：DeepSeek 前端一改版，文字生成全断（旧世界有 HTTP + PoW 兜底） | ① 选择器支持**多候选数组**（逐个探测，首个「可见且命中」者胜；`M7B-24` / `MB-Q2`）② 保留 `--web-dom-dump` / `--web-adapter-selftest` 作自愈工具 ③ **official API 路线（无浏览器依赖）作为用户可选退路**（`MB-Q3`） |
| `R2` | **CDP 增量流式存在技术不确定性**：`Network.eventSourceMessageReceived` 只对 `EventSource` 触发；若站点用 `fetch` + `ReadableStream`，需 `Fetch.takeResponseBodyAsStream` + `IO.read` 轮读 | `M7B-03` 列为**前置验证**，三路线实测后定主路线；**兜底 = DOM 轮询伪流**（但必须显式标注，`I22`） |
| `R3` | **`deepseek-web` 变「登录型条目」**：选择器未实测前，DeepSeek 文字生成**不可用** | **先建后拆**（`MB-D1`）：批 1–4 期间 WebView2 通道保留，批 5 才删；每批结束跑 §11 基线 |
| `R4` | **单点依赖**：Python / 浏览器 / 守护进程任一缺失 → 网页通道整体不可用 | `I21` + 打包内嵌 Python（`Q7` 升级为 **M6 硬门槛**）+ Edge 兜底（Windows 自带） |
| `R5` | **老用户登录态丢失**（`MB-D0-5`） | `~/.brain-ai/webview2/` 保留为遗留目录（**不自动删用户数据**）+ 一次性提示「需重新登录」；`CHANGELOG` 明写为**破坏性变更** |
| `R6` | 单 profile 多站点 → 注销粒度变粗 | 按 origin 注销（`Storage.clearDataForOrigin` + cookie 定向清理）；`M7B-08` 实测 |

---

## 5. 前置技术验证（`M7B-01`~`M7B-08` · 未通过 = 停留，不进入实现）

> **与 `P7b-01`~`P7b-04` 合并执行**（一次验证两用；`M7B-05` 已覆盖 `P7b-04` 的图片注入前哨）。

| 编号 | 任务 | 验收 | 与 P7-b 关系 |
|---|---|---|---|
| `M7B-01` | Pydoll 起**独立 Edge / Chrome**（headful）、人工登录、profile 落 `~/.brain-ai/pydoll-profile`；**经 CDP 读 Cookie（含 HttpOnly）** | 窗口可见、可登录、读回 Cookie 名单（脱敏打印） | `P7b-01` + 扩展（读 Cookie） |
| `M7B-02` | C++ ↔ Python **双向 IPC**（命名管道 + Win32 事件；JSON 行协议；非法行拒绝） | 命令 1 次 + 事件 1 次 PASS | `P7b-02` / `P7b-07` |
| `M7B-03` | **CDP 增量流式三路线实测**：① `Fetch.takeResponseBodyAsStream` + `IO.read` ② `Network.eventSourceMessageReceived` ③ `Network.dataReceived` + `getResponseBody` | 出结论：**主路线 + 实测帧序样本**（写入本文 §14 `MB-Q1`） | 本文独有（`R2` 闸门） |
| `M7B-04` | asyncio 事件循环与管道监听共存 | 无死锁；驱动与监听并行可用 | `P7b-03` |
| `M7B-05` | 页面驱动：真实打字 / 按键（`humanize`）+ `DataTransfer` 注入 File | 站点附件区出现文件；输入框内容正确 | `P7b-04` + `P7b-10` 前哨 |
| `M7B-06` | **DeepSeek 网页版选择器实测**（`input_selector` / `send` / `answer_selector` / `done_when`） | 四项命中且完成**一轮真实问答** | 本文独有（**最大人工关卡**，§10） |
| `M7B-07` | **会话失效证据来源实测**：CDP 状态码 / body vs 页面证据（登录墙 / 跳转） | 一次真实失效可判定，且纯函数输入契约确定 | 承接 `PB2-25` |
| `M7B-08` | 单 profile 多站点并存（多 tab）+ **按 origin 注销** | 两站独立登录；注销 A 不动 B | 承接 `PB2-19` |

---

## 6. 实施任务（`M7B-10`~`M7B-41` · 6 批 · **每批结束跑 §11 基线并贴数字**）

### 批 1 —— 底座（Python 侧）

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-10` | Python 包骨架（daemon / pipe / pydoll wrapper） | `python/brain_ai_browser/`（新；**文档中只写反引号、不写链接**） |
| `M7B-11` | 守护进程：自启动、后台、心跳、崩溃检测、日志脱敏 | 管道断开即可感知；不留僵尸进程 |
| `M7B-12` | 管道协议 v1（命令 / 事件 JSON 行 + 版本号 + 非法行拒绝） | 离线桩可断言（`VB2-29`） |
| `M7B-13` | Python 运行时检测与打包策略（`Q7` / `P7b-15` → **M6 硬门槛**） | 未安装 → 引导 + **不阻塞其他功能** |
| `M7B-14` | 浏览器检测与引导（Chrome 缺失 → Edge 兜底 → 可操作文案） | 两条路径都可起浏览器 |

### 批 2 —— C++ 通道（**中间检查点**）

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-15` | `web/pipe_client.{h,cpp}`（工作线程阻塞等待，`Q4`） | 不阻塞 UI 线程 |
| `M7B-16` | `web/pydoll_channel.{h,cpp}`：替代 `run_script_sync` / `ensure_session`（原 `webview_host.h:207/221`） | 接口收敛为「按站点 + 脚本 → JSON」 |
| `M7B-17` | `SessionStore` 改造：证据来源 = **CDP Cookie 快照**（`I15′`） | 仍是只读暴露 + 仅内存 |
| `M7B-18` | 诊断工具换代：`--web-adapter-selftest` / `--web-dom-dump` 走新通道；新增 `pydoll_login` | 与旧输出**同构**（便于对照） |
| `M7B-19` | **中间检查点**：本批结束**必须全基线绿**（此时 WebView2 仍在，可对照） | 硬门槛 |

### 批 3 —— 文字链路迁移

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-20` | `dom_chat` 改走 Pydoll 通道（**DOM 脚本常量不改**） | `VB2-22①~④` 作为「DOM 层零改动」的证据保持不变 |
| `M7B-21` | **CDP 增量 → `on_delta` 逐字呈现**；`delta_text_of()` 迁至独立模块并复用 | `VB2-32`（离线帧解析断言） |
| `M7B-22` | 会话失效识别：`web_session_failure_hint()` 复用，**数据源换 CDP** | `VB2-27` 重写输入源 |
| `M7B-23` | `--web-chat` / `--run-selftest --web` 走新通道 | 退出码语义按 `I20` |
| `M7B-24` | **多候选选择器**（`input_selector` / `answer_selector` 支持数组） | 逐个探测，首个「可见且命中」者胜 |
| `M7B-25` | 失败 / 超时 / 未就绪的**可操作文案**（含 `I21` 的主动报错路径） | 文案里给出下一步 |

### 批 4 —— 站点选择器回填（人工关卡，§10）

| 编号 | 任务 | 验收 |
|---|---|---|
| `M7B-26` | `deepseek-web` 改 `adapter=dom` + 选择器回填 | 依赖 `M7B-06`；**四种字段齐全** |
| `M7B-27` | 已实测 4 条（`kimi-web` / `qwen-web` / `yuanbao-web` / `ernie-web`）**在真 Chrome 复测**输入框 | 复测记录进 §10 |
| `M7B-28` | 7 条登录型站点（`tongyi-web` / `chatglm-web` / `doubao-web` / `spark-web` / `chatgpt-web` / `claude-web` / `gemini-web`）人工登录后**一次取齐三项** | 一次性完成，避免二次登录 |
| `M7B-29` | 11 条 `answer_selector` + `cookie_names` 回填（承接 `PB2-29` / `D-30`） | 完成后**不再有登录型条目**（除未支持站点） |

### 批 5 —— 撤除 WebView2

| 编号 | 任务 | 验收 |
|---|---|---|
| `M7B-30` | 删 `web/webview_host.{h,cpp}`、`tools/webview2_login.cpp` | 编译通过（无残留引用） |
| `M7B-31` | `CMakeLists.txt` / `vcpkg.json` / `build.ps1` 移除 WebView2 | 构建 0 error / 0 warning；产物不再含 WebView2 DLL |
| `M7B-32` | `paths::webview2_profile()` 退役 + 遗留目录提示（**不自动删用户数据**） | 启动时一次性提示「需重新登录」 |
| `M7B-33` | UI 文案换代：`property_panel.cpp:194/195/283/310` + 状态栏 | 无「WebView2」字样残留 |
| `M7B-34` | **CLI 契约重建**（§8）：`--provider` 必填 + 候选枚举 + `--provider auto` + `--web-probe` 废弃 | `VB2-34` |
| `M7B-35` | `I2` 解冻留痕 + `I20`/`I21`/`I22` 进冻结区 | `source/README.md` §4.2 |

### 批 6 —— 收口

| 编号 | 任务 | 验收 |
|---|---|---|
| `M7B-36` | 断言体系换代（§9 全表） | 全绿 + 贴数字 |
| `M7B-37` | 死码清理：`ai/web_pow.*`、`deepseek_web_client` 协议栈部分（**保留** `delta_text_of` / `web_session_failure_hint`） | 无孤立引用 |
| `M7B-38` | 打包：Python 内嵌（`MB-D6`）+ 移除 WebView2 Bootstrapper | M6 前置就绪 |
| `M7B-39` | 合规留痕（§13） | 文档 + 变更说明 |
| `M7B-40` | 文档同步（§11.2 的 11 项） | 断链 0 |
| `M7B-41` | `I2` 解冻的**登记与基线数字**回填（`source/README.md` §4.2 四件套） | 见 §9 |

---

## 7. 配置表迁移（`source/assets/providers.json`）

| 项 | 处置 |
|---|---|
| `web.adapter` | 白名单**收敛为 `{dom}`**（`ai/provider_spec.cpp:578-582`）；`builtin:deepseek` → **加载警告 + 按 `dom` 语义处理**（`MB-D3`；守「旧文件能加载」） |
| `web.protocol` | `{openai, dom}`（`deepseek-web` 值退役 → 警告） |
| `web.endpoints` / `probe_paths` / `challenge_path` / `completion_path` / `token_expr` | **保留解析（不报错）+ 警告「已被 `M7B` 取代」**；不再参与运行（`VB2-33`） |
| `web.input_selector` / `send` / `answer_selector` / `done_when` | **语义升级**：支持**多候选数组**（`M7B-24`；`MB-Q2`） |
| `web.cookie_names` | 从「可选」升级为**建议必填**（登录态判据来源，`I15′`）；未配时用「该 origin Cookie 非空」兜底（沿用 `D-27`） |
| `web.login_url` / `window_title` | 语义保留（Pydoll 由**独立浏览器窗口**承载，标题仍可用） |
| `--web-probe` | **废弃**（不再有内部端点可探）；`--help` 标注替代命令（§8） |

---

## 8. `I2` 解冻与 CLI 契约重建

### 8.1 `I2` 原文与射程（证据）

| 项 | 内容 | 位置 |
|---|---|---|
| **原文** | 「`--web-chat` / `--web-probe` / `--web-session-selftest` **行为不变** \| 三个自检命令」 | `M_patchB.md:294`（已归档） |
| 实际扩展 | 连 `--login-selftest` 与全部「无参重载」一起护住 | `web/webview_host.h:26,51,156,212` |
| **被 `I2` 扶起来的兼容装置** | 三常量 `kDefaultSite*`（只为「内置条目字段值 == 旧常量」而存在） | `web/webview_host.h:25-31` |
| | `probe_applicable = true` 默认值（注释明写「守 I2」） | `web/webview_host.h:50-54` |
| | 无参 `interactive_login_request()` / 无参 `ensure_session()` / `solve_pow_via_page("")` | `web/webview_host.h:156,212,221` |
| | `web_spec_for()` 的**回落分支**（注释：「保留旧回落语义，**仅供 CLI / 自检**守 `I2`」） | `ai/provider_spec.h:180-187,191` |
| | `web_shows_user_token()` 的「内置 `deepseek-web` 配了 `token_expr` → 行为不变（守 `I2`）」 | `ai/provider_spec.h:221-224` |
| | 内置 DeepSeek 探测脚本分支 / 「默认站点」入口 | `web/webview_host.cpp:1096-1097,1345,1776` |
| **钉住 `I2` 的断言** | `VB2-17`「无参 `interactive_login_request` 与旧常量**逐字一致**（不变量 I2）」 | `tools/api_probe.cpp:2341-2345` |
| | `VB2-25④`「默认参数 → 脚本仍是内置 DeepSeek 行为（守 I2）」 | `tools/api_probe.cpp:2745-2751` |
| **真实 CLI 行为** | `--login-selftest` 无站点概念 | `src/main.cpp:1102` |
| | `--web-probe` 支持 `--provider`；**无参 → 回落内置 DeepSeek** | `src/main.cpp:1297-1302` |
| | `--web-chat` **完全不支持 `--provider`**（写死内置 DeepSeek） | `src/main.cpp:1156-1162,1309-1310` |
| | `--web-session-selftest` 无参，模拟「窗口已开」 | `src/main.cpp:1153,778-800` |

### 8.2 判断：`I2` 护住的 9 项里，**真能力只有 4 项**，其余 5 项是纯兼容装置

| # | 被护住的东西 | 性质 | 处置 |
|---|---|---|---|
| 1 | `--login-selftest`：能起浏览器 + 导航 + 取 Cookie | **真能力**（健康检查） | 替代（§8.4 C-1） |
| 2 | `--web-probe`：登录态**真可用** + 端点一致 | **能力本体随协议栈消失** | 拆成两条更强的替代（C-3） |
| 3 | `--web-chat`：一行式真实生成 | **真能力**（端到端冒烟） | 替代（C-2） |
| 4 | `--web-session-selftest`：窗口已开 → 自动补凭证 | **真能力**（守护一个真 bug） | **语义升级**替代（C-4） |
| 5 | `LoginRequest` 默认构造 = DeepSeek | 纯兼容装置 | **删** → 默认空 + 必填校验（与 `D-22②` / `I14` 同向） |
| 6 | `web_spec_for()` 回落分支 | 纯兼容装置 | **删**，统一 `strict_web_spec_for()` |
| 7 | `probe_applicable` 默认 true + 内置探测脚本 | 纯兼容装置 | **删**（连带 `VB2-25①③④`） |
| 8 | 三常量 `kDefaultSite*` | 纯兼容装置 | **删** → **结清** `M_patchB.md:51/962` 记的「登录 URL / 窗口标题仍硬编码」欠账 |
| 9 | 无参重载 3 个 | 纯兼容装置 | **删**（编译期消失） |

> **结论**：`I2` 是 `D-22②`（站点不回落）**唯一的豁免区** —— 界面 / 运行前校验 / 运行期都已「不回落」，
> 只有 CLI 自检路径靠 `I2` 豁免回落到内置 DeepSeek。**作废 `I2` = 补上「站点不回落」的最后一块缺口**。
> 这不是「丢掉一个功能」，而是**删掉一套双轨兼容层**。

### 8.3 `I20`（新不变量 · 替代 `I2`）

> **`I20`（CLI 契约）**：所有网页版 CLI 子命令的**命令名 / 参数形式 / 退出码语义**，必须在
> **`--help` 输出、文档、自动断言**三处一致；且：
> ① **不允许任何隐式站点默认** —— 需要站点的子命令缺 `--provider` 时必须**打印候选条目清单并返回退出码 2**，
> **不得**静默取表内第一个；
> ② 唯一例外是**显式关键字** `--provider auto`（**保留字**，非条目 id；经检索确认表内 40 个 id 无冲突），
> 语义 = 表内第一个 `kind=web` 条目，且必须**打印实际选中的 id**；
> ③ 退出码三档固定：**0** = 通过 / **1** = 执行失败（未登录、超时、生成失败）/ **2** = 用法或配置问题
> （缺参、条目不存在、非 web 条目、站点不可用）；
> ④ 自检命令**不得残留状态**（会话清空、不写用户数据）。

### 8.4 CLI 替代对照表（逐命令 · 含退出码）

| 旧 | 新 | 无 `--provider` 时 | 说明 |
|---|---|---|---|
| `--login-selftest` | `--login-selftest --provider <id>` | 列候选 + **2** | 起 Pydoll 浏览器 → 导航该条目 `web.login_url` → CDP 取 Cookie |
| `--web-chat "<p>"` | `--web-chat --provider <id> "<p>"` | 列候选 + **2** | DOM 驱动发送 + CDP 增量流式取答 |
| `--web-probe` | **废弃** | — | 由 `--web-adapter-selftest --provider <id>`（静态就绪度）+ `--web-chat --provider <id> "ping"`（动态端到端）取代 |
| `--web-session-selftest` | 同名 · **语义升级** | 不需要（全局） | 新语义：**杀守护进程 / 关浏览器 → 重启 → 登录态免重登自动恢复** |
| `--run-selftest --web` | 不变（已有 `--provider`） | 沿用：取第一个并**打印** | 仅把「表中无 web 条目 → 1」统一为 **2**（`VB2-34④`） |
| 任意命令 + `--provider auto` | **新增** | — | 打印实际选中 id 后再执行 |
| `webview2_login` 工具 | `pydoll_login --provider <id>` | 列候选 + **2** | M1 的 `V-03` 记录标注「已被 `M7B` 取代」 |

**无参报错文案模板**（替代 `I2` 的用户体验补偿 —— 直接给出可复制的新命令）：

```
✗ --web-chat 需要 --provider <id>（例：--web-chat --provider deepseek-web "你好"）
  当前表里的 web 条目（12 条）：
    deepseek-web    DeepSeek 网页版      dom
    kimi-web        Kimi 网页版          dom
    …（复用 src/main.cpp:1260-1271 既有的候选枚举代码）
  提示：--provider auto = 用表内第一个 web 条目
```

### 8.5 代码级删除清单（`I2` 的兼容装置）

| # | 删除对象 | 位置 |
|---|---|---|
| 1 | `kDefaultSiteLoginUrl` / `kDefaultSiteWindowTitle` / `kDefaultSiteProbeTitle` | `web/webview_host.h:29-31` |
| 2 | `LoginRequest` 的 DeepSeek 默认 url / 标题 → 改空 + 必填校验 | `web/webview_host.h:34,36` |
| 3 | `interactive_login_request()` 无参重载 | `web/webview_host.h:156` |
| 4 | `ensure_session(int, std::string*)` 无参重载（改为必带 `LoginRequest`） | `web/webview_host.h:221` |
| 5 | `LoginRequest.probe_applicable` | `web/webview_host.h:50-54` |
| 6 | `solve_pow_via_page()` 整函数（随协议栈退役） | `web/webview_host.h:210-217` |
| 7 | `web_spec_for()` / `web_provider_id_for()` 回落重载 | `ai/provider_spec.h:180-187` |
| 8 | `web_shows_user_token()`（`token_expr` 退役） | `ai/provider_spec.h:221-224` |
| 9 | `probe_is_applicable()` 全套 + 内置探测脚本分支 | `ai/provider_spec.h:226-233`、`web/webview_host.cpp:1096-1097` |
| 10 | 「默认站点」入口 | `web/webview_host.cpp:1776` |
| 11 | `webview2_login.cpp`（`--url` 默认 DeepSeek） | `tools/webview2_login.cpp:19` |

---

## 9. 不变量与断言体系

### 9.1 不变量

| 编号 | 措辞 | 说明 |
|---|---|---|
| `I2` | **作废（解冻）** | 按 [../../source/README.md](../../source/README.md) §4.2 冻结区纪律登记：**解冻原因**（WebView2 退场 + 站点不回落贯彻到底）+ **替代物**（`I20`）+ **基线数字**（§11） |
| **`I20`（新）** | **CLI 契约**（措辞见 §8.3） | **替代 `I2`，但只锁「形状」（命令名 / 参数形式 / 退出码），不锁「字面」** —— 可自动断言 |
| **`I21`（新）** | **不静默降级**：守护进程 / Python / 浏览器 / 登录态任一不可用 → **立即报错 + 可操作引导 + 退出码 1/2**；**绝不换通道、绝不换身份、绝不假装成功**（含「回落 WebView2」「改用另一套登录态」两种禁止） | 与 `D7-b` 同族，覆盖到全部依赖 |
| **`I22`（新）** | **流式降级必须显式**：CDP 增量不可用 → 退回 DOM 轮询，但 UI 与日志**必须标注「非流式（轮询）」** | 不许安静地改变行为 |
| `I15 → I15′` | 登录态判据 = **该 origin 的 Cookie 名单（CDP 读取）+ 页面可交互证据**；**不得**依赖厂商专有物（原 `userToken` / `ds_session_id` 随协议栈退役） | 判据**站点无关**这一精神不变 |
| `I16` | **升级为全局**：程序**不再注入任何站点内部端点**（原「DOM 站点 vs 内置协议站点」的白名单制度取消 → **所有站点一视同仁**） | 由 `VB2-25′` 断言 |
| `I14` / `D-22②` | 继承且**豁免区消失**：站点只能来自条目 `web.*`，**无回落** —— 现在连 CLI 路径也如此（§8） | |
| `I17`（继承） | 图片解码冻结区不受影响 | 本文不动 `utils/image_decode.cpp` |
| `I18` / `I19`（继承） | 网页版图片上传证据 / `image` 端口向后兼容 | 仍属 P7-b，本文只提供通道底座 |
| `I13`（继承） | 候选恒两项、`kind` 与 `mode` 不一致只提示不改写 | 不受影响 |

### 9.2 断言处置全表

| 断言 | 现值口径 | 处置 |
|---|---|---|
| `VB2-17` | 无参 `interactive_login_request()` 与旧常量**逐字一致**（守 `I2`）+ 按条目构造 | **前半删**；**后半保留为 `VB2-17′`**（按条目构造站点参数） |
| `VB2-18` | 模式候选（official / web） | **保留**（不变） |
| `VB2-19①~⑤` | 严格站点解析 + 适配器门控 | **改**：白名单收敛 `{dom}`；`builtin:deepseek` → **警告路径** |
| `VB2-21①~⑤` | 登录型条目拦截 | **保留且更重要**（回填前 `deepseek-web` 也属此类） |
| `VB2-22①~④` | DOM 纯函数（钳制 / 转义 / 脚本常量 / 就绪度） | **不变** —— 作为「DOM 层零改动」的证据 |
| `VB2-22⑤` | `dom_chat` 前置校验：缺字段立即报错、不开窗 | **加强**：守护进程未就绪 → 立即报错，**不静默回落**（`I21`） |
| `VB2-24①~⑤` | 登录态判定（`I15`） | **改**：证据来源 = **CDP Cookie**（无 `userToken` 分支） |
| `VB2-25①~⑤` | 协议探测适用性（`I16`） | **删**（①③④）；②⑤ 精神保留 → **`VB2-25′`**：程序**不注入任何站点内部端点**（全局断言） |
| `VB2-26①~③` | 文案 / 渲染条件（`token_expr` 行、站点无关措辞） | **改**：删 `token_expr` 行；显示「浏览器通道：Pydoll」；加「本机需 Python + 浏览器」 |
| `VB2-27①~⑤` | 会话失效识别（`401` / `code=40002` / `40003`） | **重写输入源**（HTTP → CDP），**纯函数逻辑复用** |
| **`VB2-28`（新）** | 单引擎门控：无 `web.engine`；遗留 `builtin:*` 走**警告路径**（`MB-D3`） | |
| **`VB2-29`（新）** | 管道协议**离线桩**：JSON 行解析 / 非法行拒绝 / 心跳超时文案（不依赖真实 Python） | |
| **`VB2-30`（新）** | `I21`：Python / 浏览器 / 守护进程 / 登录态缺失 → 报错 + 引导；**不发起任何 HTTP、不开任何浏览器** | |
| **`VB2-31`（新）** | `I22`：CDP 增量不可用 → 轮询并**显式标注「非流式」** | |
| **`VB2-32`（新）** | CDP 帧 → 增量文本（**复用 `delta_text_of` 既有形态 A/B 用例**；纯函数离线断言） | |
| **`VB2-33`（新）** | 遗留配置字段（`endpoints` / `probe_paths` / `token_expr` / `builtin:*`）→ **警告不报错**（旧文件可加载） | |
| **`VB2-34`（新）** | `I20` CLI 契约：① `--help` 含全部子命令名与退出码 ② 缺 `--provider` 的自检命令 → **候选枚举 + 码 2**（不得静默取第一个）③ `--provider auto` → **打印实际选中 id** ④ 退出码三档**跨命令一致** | |
| **`VB2-35`（新）** | `I2` 解冻**回归守卫**（反向断言）：旧「无参回落内置 DeepSeek」行为**已不存在** —— 默认 `LoginRequest.url` 为空、无参重载已删、内置 DeepSeek 探测脚本分支已删 | |
| **`VB2-36`（新）** | **复用资产回归**：`web_session_failure_hint()` 在**新数据源**（CDP 状态码 / body）下判定与旧断言**一致** | |

### 9.3 基线数字的预期路径（**先降后升**，不得误判为回归）

```
当前（2026-09-27 记录）: --exec-selftest 251 / 0
   │  删 VB2-17（前半）· VB2-25①③④ → 预估计数下降
   ▼
批 3–5 期间（过渡态）: 约 24x / 0  ← 两套通道并存，断言处于新旧交替
   │  加 VB2-25′ · VB2-28~36（9 条）
   ▼
收口: 记录新值并**全绿**（数字回填 §11）
```

> `source/README.md` §4.2 的示例数字「`--exec-selftest` 237/0 → 251/0」是 **M7 第一轮 → P7** 的历史口径；
> 本文的实际数字**必须实测回填**，不得沿用。

---

## 10. 站点重测清单（12 条网页版条目 · 人工关卡）

> `answer_selector` 目前**全部为空**（11 条 dom 站点），`cookie_names` / `token_expr` 仅 `deepseek-web` 配过。
> **不要先在 WebView2 里测一遍** —— 直接在 **Pydoll 真实浏览器**里一次性测完（避免二次重测）。

| 条目 | 现 `adapter` | `input_selector` | `send` | `answer_selector` | 迁 Pydoll 后要做 |
|---|---|---|---|---|---|
| `deepseek-web` | `builtin:deepseek` | — | — | — | **全新实测三项**（`M7B-06` → `M7B-26`）；**本项最大人工关卡** |
| `kimi-web` | `dom` | ✅ `div.chat-input-editor` | ✅ `key:Enter` | ❌ 空 | **真 Chrome 复测**输入框 + **补测** answer |
| `qwen-web` | `dom` | ✅ `textarea[placeholder="Ask Qwen"]` | ✅ `click` | ❌ 空 | 同上 |
| `yuanbao-web` | `dom` | ✅ `div.ql-editor.ql-blank` | ✅ `key:Enter` | ❌ 空 | 同上 |
| `ernie-web` | `dom` | ✅ `#chat-textarea` | ✅ `key:Enter` | ❌ 空 | 同上（**建议首个验证站**：`M7B-06`） |
| `tongyi-web` / `chatglm-web` / `doubao-web` / `spark-web` / `chatgpt-web` / `claude-web` / `gemini-web` | `dom` | ❌ 空（登录型条目） | ❌ | ❌ | 人工登录后 `--web-dom-dump --provider <id>` **一次取齐三项**（`M7B-28`） |
| **全部 11 条 dom** | — | — | — | — | `cookie_names` 回填（配合 `D-30`「`cookie_names` 优先」）+ `answer_selector` 回填（`M7B-29`，承接 `PB2-29`） |

**回填记录格式**（每站一行，写入 §15 变更记录）：

```
<id>：input=<命中选择器> · send=<键/点击> · answer=<选择器> · done_when=<判据> · cookie_names=<名单>
      实测日期 / 浏览器版本 / 备注（如「需先关掉新手引导」）
```

---

## 11. 验证基线与文档同步

### 11.1 基线

| 命令 | 当前记录（2026-09-27） | `M7B` 目标 |
|---|---|---|
| `source\build.ps1` | 0 error / 0 warning | **不变**（硬指标） |
| `api_probe --selftest` | 七组 PASS | 换代后全绿 |
| `api_probe --exec-selftest` | **251 / 0** | **先降后升**（§9.3）→ 记录新值并全绿 |
| `api_probe --graph-selftest` | 110 / 0 | 不变 |
| `api_probe --provider-selftest` | 50 / 0（`--provider-dump` 21 条：official 9 / web 12） | 换代后全绿 |
| `aiwrite --run-selftest` / `--run-selftest --web` | PASS / PASS（5-5） | 换通道后重测 |
| `--login-selftest` / `--web-chat` | 0 / 1 / 2 语义 | **调用形式与退出码不变**（`I20`） |
| `--web-probe` | 可用 | **废弃**（`--help` 标注替代命令） |
| **新增** `--web-stream-selftest --provider <id>` | — | CDP 增量逐帧 PASS（`I22` 断言） |
| 文档断链 | broken **0** | **不变**（新文件先只写反引号） |

### 11.2 文档同步清单（11 项 · `M7B-40`）

| # | 文档 | 动作 |
|---|---|---|
| 1 | `docs/actionPlan/M7B.md` | **新建**（本文） |
| 2 | `docs/DevPlan.todo` | M7 组**续号**（当前最大 id 188 → 新增 **189+**） |
| 3 | `docs/actionPlan/M7.md` | §13 表末「不改协议栈」行**标记作废**；§17 `Q2`/`Q3` 标记**作废 + 指向本文**；§14 增「`M7B` 基线」行 |
| 4 | `docs/roadmap.md` | §四 轨道 A 补「通道唯一化归 `M7B`」；**`PM-01`（WebView2 Runtime 检测）作废**；§十一 补 `MB-Q*` / `MB-D*` |
| 5 | `docs/CHANGELOG.md` | `[Unreleased]` 增 `M7B` 立项 + **破坏性变更**（老用户需重新登录一次） |
| 6 | `docs/actionPlan/milestone_plan.md` | M7 行补 `M7B`（v0.5.4） |
| 7 | `docs/README.md` | 进度行补 `M7B 已立项` |
| 8 | `docs/ai_writer_nodes.md` | §2 依赖表：WebView2 行**移除**、`Python + Pydoll` 由「计划」改「**必需**」、命名管道行同；§18.3 表格补 `M7B` |
| 9 | `docs/节点编辑器使用说明.md` | WebView2 → Pydoll 章节改写 + 命令表按 §8.4 更新 |
| 10 | `source/README.md` | WebView2 说明改写 + §4.2 登记 `I2` 解冻与 `I20` / `I21` / `I22` |
| 11 | `docs/actionPlan/M_patchAB_rest.md` | `PB2-25` / `PB2-28` / `PB2-29` 与本文的归口交叉引用 |

---

## 12. 回滚点（**阶段级** —— 因为没有第二个引擎可切）

| 级别 | 手段 | 影响面 |
|---|---|---|
| 一级（零代码） | 条目改回 `adapter` 走旧通道 / 改用 `protocol=openai` 的 official 条目 | 单条目 |
| 二级（进程） | 不启用任何 Pydoll 条目 → **不启动** Python 守护进程、不装依赖、不占端口（对齐 `Q7` / `P7b-15`：**不阻塞其他功能**） | 全局 |
| 三级（阶段） | `M7B-01`~`08` 未过 → **整体停留**（同 P7-b 模式）；批 5 之前任一批未全绿 → 停在原批（**WebView2 仍在，可切回**） | 计划级 |
| 四级（代码） | 批 5 之后（WebView2 已删）**只能 `git revert`** —— 故本文强制「**先建后拆**」（`MB-D1`） | 变更集级 |

> **关键纪律**：批 5 是**不可逆点**。进入批 5 的**前置条件** = 批 1–4 全绿 + §10 的 12 条站点选择器**全部回填完毕**。

---

## 13. 合规红线（**不因反检测诉求放宽**）

撤 WebView2 的**正当理由**：嵌入控件 ≠ 真实浏览器，会被站点**误判**为机器人。据此采取的 `humanize`
（Bezier 鼠标 / 随机打字延迟）**仅限「像正常浏览器一样操作」**；**仍然**：

- 有头窗口 + 用户**手动**登录；
- **不代填密码**、**不绕过验证**、**不伪造站点鉴权**；
- **不针对验证码做规避**（`M7b` 不引入任何打码 / 绕过能力）；
- 若站点要求人机验证，**流程停在那里等用户**（这是「像人一样」的自然结果，也是边界）。

（同族口径：[../节点编辑器使用说明.md](../节点编辑器使用说明.md) §10.3、[M7.md](M7.md) §12、`P7b-14`。）

---

## 14. 开口项与待拍板

### 14.1 待拍板（`MB-D1`~`MB-D6`）

| 编号 | 事项 | 建议 |
|---|---|---|
| `MB-D1` | **撤除时机**：一次性删 vs **先建后拆**（批 5 才删） | **先建后拆**（否则 DeepSeek 选择器未回填期间文字生成中断；`R3`） |
| `MB-D2` | 站点与标签：每站点一个 tab / 复用单 tab | 每站点一个 tab（登录态由单 profile 共享） |
| `MB-D3` | `adapter=builtin:deepseek` 的处理：警告降级 vs 直接拒绝 | **警告 + 按 `dom` 语义**（守「旧文件能加载」） |
| `MB-D4` | `DevPlan.todo` 编号：M7 组续号 vs 新 `M7B` 组 | 续号（`189+`），与 `Q8` 先例一致 |
| `MB-D5` | 版本与顺序：`M7B` = v0.5.4 排在 P7-a（v0.5.2）之后，与 P7-b（v0.5.3）**交织** | **P7-a → `M7B` 批 1–2（通道底座）→ P7-b 上传 + `M7B` 批 3+** |
| `MB-D6` | Python 内嵌与否（`Q7` 升级为 **M6 硬门槛**） | **内嵌**（否则「非技术用户独立安装」的退出条件不成立） |

### 14.2 开口项（`MB-Q1`~`MB-Q3`）

| 编号 | 开口项 | 建议默认 |
|---|---|---|
| `MB-Q1` | **CDP 增量主路线**（`M7B-03` 出结果后回填本文） | 待实测；优先 `Fetch.takeResponseBodyAsStream` + `IO.read` |
| `MB-Q2` | 多候选选择器的语法（数组 vs `a\|b` 字符串） | **数组**（显式、可校验、可给「第 N 候选命中」诊断） |
| `MB-Q3` | 是否保留 official API 作为「无浏览器退路」 | **保留**（已是现有能力，`R1` 的主要缓解） |

### 14.3 明确不做（硬边界）

- 不改 `utils/image_decode.cpp`（`I17` 冻结区）；
- 不做「两套引擎可切换」（`M7.md` `Q2` 的方案 ③ **作废**）；
- 不做 i18n / 主题 / 快捷键 / 插件 / 多标签工作流（沿用 `M7.md` §15「明确不做」）；
- **不做**任何用于绕过站点风控 / 验证码的能力（§13）。

---

## 15. 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-09-28 | v1 | **`M7B` 立项**：用户决议 7 条（§2）；完成现状取证（§1）、风险（§4）、前置验证（§5）、6 批任务（§6）、配置迁移（§7）、**`I2` 解冻与 CLI 契约重建**（§8）、不变量与断言全表（§9）、站点重测清单（§10）、基线（§11）、回滚（§12）。**未开工** —— 待 §14.1 待拍板确认后进入 `M7B-01`~`08` |

**站点选择器回填记录**（格式见 §10；`M7B-06` / `M7B-27` / `M7B-28` / `M7B-29` 执行时逐行追加）：

| 日期 | 条目 | input | send | answer | done_when | cookie_names | 备注 |
|---|---|---|---|---|---|---|---|
| （待填） | | | | | | | |







