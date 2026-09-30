# M7B 详细 Action Plan：网页通道整体迁 Pydoll（WebView2 退场 · 文字与图片共用一条通道）

> 里程碑：M7 **第三轮 `M7B`**（与第一轮 M7、第二轮 P7 并列；排在 M6 之前 —— M6 是 v1.0 打包里程碑）
> 版本：**v0.5.4**（内部）
> 前置：M5 核心切片（M5-C）已落地；**P7-a（v0.5.2）不阻塞本文**；**P7-b（v0.5.3）与本文共用通道底座**（见 §6 批 1–2）
> 类型：**架构替换 + 运行时依赖退场**（删 WebView2；`builtin:deepseek` 协议栈退役；新增 Python + Pydoll 运行时）
> 依据：用户 2026-09-28 决议 `MB-D0-1`~`MB-D0-7`（见 §2）；承接 [M7.md](M7.md) §17 `Q2`/`Q3` —— **本文使 `Q2`/`Q3` 作废**（原「两套引擎可切换」不在本文范围）
> 状态：**计划（未开工）· 前置验证进行中：5/9 已过**（`M7B-01`/`03`/`05`/`08`/`09`；余 `M7B-02`/`04` 的
> C++↔Python 命名管道与 `M7B-06`/`07` 的真站点人工登录 + `M7B-06b`）· 新增决议 **`MB-D0-8`**（登录态持久化四层
> `L1`~`L4`，由 `M7B-09` 四轮实测支撑） —— §5 前置验证**全部通过前不进入实现**（同 P7-b 模式）
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
6. **基线提醒**：作废 `I2` 会**删掉**若干旧断言（`--exec-selftest` 总数会变化），属**预期变化**，不得误判为回归
   —— 但因 **`MB-D1` 已定 = 先建后拆**，**删除动作集中在批 5 一次完成**：**批 1–4 数字只升不降**
   （任何下降即**真回归**），批 5 内**先降后升闭合**。预期路径见 **§9.3**。

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
| `tools/api_probe.cpp`（**注意**：不在 `src/` 下） | 3 | 断言换代（§9；**删除类动作归批 5**，见 §9.3） |
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
| `MB-D0-8` | **登录态持久化四层保障**（2026-09-28 前置验证暴露「Pydoll 关窗即丢登录态」后**用户拍板：四层全做**） | **L1 干净退出**（`Browser.close` → **等进程退出**，默认 5 s，超时才强杀；`browser.stop()` 只作异常兜底）· **L2 加密快照**（`Storage.getCookies` → **DPAPI 加密**落 `~/.brain-ai/session/`，启动 `Network.setCookies` 回灌）· **L3 启动自愈**（profile 被占 → 优先优雅接管 / attach，**禁止**为"让程序起来"而强杀浏览器）· **L4 异常退出可见**（未干净退出埋点 + 提示 + 一键重登 / 从快照恢复）。**理由**：Pydoll `stop()` = 「close 后**立刻** kill」→ Cookie 库不落盘 → **登录态每次退出即丢**（证据见 §5 要点⑦）。纪律：快照**仅加密落盘、不外传**；`SessionStore` 仍只存内存 |

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
| 会话存储 | `SessionStore` 只做**内存快照**，不落盘（`I15′`）；**登录态落盘只经 L2 加密快照**（独立组件，**不喂** `SessionStore`） |
| 浏览器与标签 | **一个浏览器进程、多标签**；`D-20`（串行复用同一窗口）作废 → 每站点一个 tab（`MB-D2`） |
| **退出 / 关闭** | CDP `Browser.close` → **等进程退出**（默认 5 s）→ 超时才强杀；**禁止「close 后立刻 kill」**（Pydoll `stop()` 的顺序缺陷，见 §5 要点⑦）；**用户手动关窗 = 正常路径**（Chrome 自行落盘），守护进程须按**可恢复状态**处理 |
| **登录态持久化** | **L1 干净退出 + L2 DPAPI 加密快照 + L3 启动自愈 + L4 异常退出提示**（`MB-D0-8`）；判据仍是 `I15′`（站点无关 Cookie 名单） |
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

## 5. 前置技术验证（`M7B-01`~`M7B-09` + `M7B-06b` · 未通过 = 停留，不进入实现）

> **进度：5/9 已过**（`M7B-01` / `03` / `05` / `08` / `09` · 2026-09-28）—— 余 `M7B-02` / `M7B-04`
> （C++ ↔ Python 命名管道双向 IPC；**工具链已通**，见 `M7B-02` 行注）、`M7B-06` / `M7B-07`（**需真站点人工登录**）
> 与加强项 `M7B-06b`。其中 `M7B-09` 是决议 `MB-D0-8` 新增的闸门，已由 V-a~V-d 四轮收口（见下方结论块）。
>
> **与 `P7b-01`~`P7b-04` 合并执行**（一次验证两用；`M7B-05` 已覆盖 `P7b-04` 的图片注入前哨）——
> `P7b-01` / `P7b-04` **已随 `M7B-01` / `M7B-05` 一并通过**（状态已回填 [M7.md](M7.md) §12.1）。

| 编号 | 任务 | 验收 | 与 P7-b 关系 |
|---|---|---|---|
| `M7B-01` | Pydoll 起**独立 Edge / Chrome**（headful）、人工登录、profile 落 `~/.brain-ai/pydoll-profile`；**经 CDP 读 Cookie（含 HttpOnly）** | ✅ **已过（2026-09-28，机制层）**：窗口可见、profile 落盘、CDP 读回含 HttpOnly、重启后持久 Cookie 保留；**真·站点登录仍在 `M7B-06`** | `P7b-01` + 扩展（读 Cookie） |
| `M7B-02` | C++ ↔ Python **双向 IPC**（命名管道 + Win32 事件；JSON 行协议；非法行拒绝） | ⏳ **待过**：**C++ 工具链冒烟已通**（`python/_probe/cpp/hello.cpp` 经 VS18 `cl` 编译链接成功 ⇒ 环境就绪），**剩管道双向 IPC 本体**（`CreateNamedPipe` ↔ Python 端 = 1 命令 + 1 事件） | `P7b-02` / `P7b-07` |
| `M7B-03` | **CDP 增量流式三路线实测**：① `Fetch.takeResponseBodyAsStream` + `IO.read` ② `Network.eventSourceMessageReceived` ③ `Network.dataReceived` + `getResponseBody` | ✅ **已过（2026-09-28）**：主路线 = **①+`IO.read(size=128~256)`**（`size` 即推送粒度，实测 256 B/帧）；帧序样本见 §5 结论块与 `python/_probe/out/m7b03_cdp_stream.json` | 本文独有（`R2` 闸门） |
| `M7B-04` | asyncio 事件循环与管道监听共存 | 🟡 **部分**：**asyncio 侧已多轮实证无死锁**（本轮 9 个探针在同一事件循环里并行 `start()` / CDP 读库 / 本地 HTTP 桩，全部正常收尾）；**「管道监听线程」这一半待 `M7B-02` 接通后合测** | `P7b-03` |
| `M7B-05` | 页面驱动：真实打字 / 按键（`humanize`）+ `DataTransfer` 注入 File | ✅ **已过（2026-09-28）**：逐字符（154 ms/字符、21 次 input 事件）；文件注入 `expect_file_chooser` 与 `DOM.setFileInputFiles` **两条路径都成功** | `P7b-04` + `P7b-10` 前哨 |
| `M7B-06` | **DeepSeek 网页版选择器实测**（`input_selector` / `send` / `answer_selector` / `done_when`） | 四项命中且完成**一轮真实问答** | 本文独有（**最大人工关卡**，§10） |
| `M7B-07` | **会话失效证据来源实测**：CDP 状态码 / body vs 页面证据（登录墙 / 跳转） | 一次真实失效可判定，且纯函数输入契约确定 | 承接 `PB2-25` |
| `M7B-08` | 单 profile 多站点并存（多 tab）+ **按 origin 注销** | ✅ **已过（2026-09-28）**：两域并存（7 条 Cookie）→ `Storage.clearDataForOrigin(A)` 后 **A 空 / B 完整**；⚠️ Cookie 按**域名**隔离（端口不参与） | 承接 `PB2-19` |
| `M7B-09` | **关闭时序与持久化三档对照**（`MB-D0-8` L1/L2 闸门）：① 强杀 ② `Browser.close` 后立刻杀 ③ `close` + **等进程退出**；并测「写 Cookie 后等 0/10/30/60 s 再强杀」（判定 Chrome 是**延迟提交**还是**只在干净退出提交**）；快照往返（`Storage.getCookies` → `Network.setCookies` 回灌后站点是否认账）；手动关窗 + 残留实例接管 | ✅ **已过（2026-09-28 · V-a~V-d 四轮 + 4 项补充诊断）**：① 三档对照 → **干净退出保住持久 Cookie**（`close` + 等 **0.22 s** 即够）/ 强杀与库默认 `stop()` = 丢；② 写 Cookie 后 **≥30 s** 再强杀也保得住（**10–30 s 延迟提交窗口**，强杀残留 `Cookies-journal`）；③ 快照往返 PASS（`httpOnly` 保持、站点认账）；④ **会期 Cookie 干净退出必掉**，`--restore-last-session` 能保但**会重开上次标签页**（副作用）⇒ **L2 快照必需**（实测回灌后 `session`/`httpOnly` 保持 + 服务端认账）；⑤ profile 被占时**同端口会静默附着到既有实例 / 随机端口 `FailedToStartBrowser`** ⇒ **必须先探端口，禁止强杀** | 本文独有（`MB-D0-8` 新闸门） |
| `M7B-06b` | **真·站点登录后「重启仍登录」**（含逐个记录登录 Cookie 的 `expires`） | 同一站点：登录 → 干净退出 → 重启 → **仍为已登录**；无 `expires` 的会话 Cookie **如实记录**（决定"下次免登录"能否承诺，回填 §10 表） | `M7B-06` 的加强项（`MB-D0-8` L1 的端到端验收） |

> **Pydoll 2.27.0 能力实测（2026-09-28 · 环境探测 · 数字见 §11.1）**
> 本机链路**已通**（冒烟通过）：Pydoll 自动探测 Chrome → CDP 读 Cookie → `execute_script` → 正常关窗。
> 由此**修正三处计划假设**：
> ① **`M7B-03`（`R2` 的主闸门）不确定性下降**：Pydoll 内置 `Tab.get_network_logs()` /
>    `get_network_response_body()` / `enable_network_events()` —— 原三路线里的第 ③ 条
>    （`Network` 事件 + 取响应体）**有库级 API 支撑**，不必手写 CDP 帧；仍需实测的是
>    **能否拿到"增量/逐帧"而非一次性完整 body**（这才决定 `I22` 是否触发）。
> ② **P7-b 文件注入（`P7b-10` / `M7B-05`）有更稳的路子**：`Tab.expect_file_chooser()` +
>    `enable_intercept_file_chooser_dialog()` 是**一等公民 API**，应优先于 `DataTransfer` 注入（后者降为备选）。
> ③ **`Edge` 兜底（`M7B-14`）已在 API 层确认**：`pydoll.browser.chromium` 同模块导出 `Chrome` / `Edge`。
>
> **`M7B-03` 实测结论（2026-09-28 · 本地页面自证 · 脚本 `python/_probe/m7b03_cdp_stream.py`）**
> **主路线 = ① `Fetch.takeResponseBodyAsStream` + `IO.read`** —— 增量成立，且**粒度 = `IO.read` 的 `size`**：
>
> | 变体（本地流） | `read size` | 到达粒度 | 实测 |
> |---|---|---|---|
> | 512 B / 2.8 s | 65536 | 64 B / 0.35 s | 3 帧，pause@4888 ms（**流已结束**）→ 一次拿全 |
> | 1.5 MB / 12 s | 65536 | 64 KB / 0.5 s | **25 帧 / 跨度 12015 ms**，每帧正好 64 KB |
> | 960 B / 18 s（**≈真实回答形态**） | 65536 | 16 B / 0.3 s | 2 帧 → 阻塞到 EOF（**size 远大于总量**） |
> | 10 KB / 30 s | 4096 | 100 B / 0.3 s | 4 帧（4096/4096/1808/0）→ **攒批** |
> | **10 KB / 30 s** | **256** | 100 B / 0.3 s | **41 帧，每帧 256 B，间隔 0.8–1.0 s，尾部 16 B** ← **可逐字** |
>
> **结论**：① 是**真增量**；`IO.read` 阻塞到**攒够 `size` 或 EOF** ⇒ **`size` 即最小推送粒度**，
> 取 **128–256** 即可满足逐字呈现（回答通常 < 8 KB → 总往返 < 60 次，可接受）。
> ② `Network.eventSourceMessageReceived` 有效（**8 条 / 2446 ms**）但**仅当站点用 `EventSource`**。
> ③ `Network.dataReceived` 只给**进度**（末尾 `getResponseBody` 才拿到全文 332 B）→ **不能作内容增量**。
> ④ 页面 hook（`Page.addScriptToEvaluateOnNewDocument` + `body.tee()`）**可用**：**8 帧 / 跨度 5568 ms**。
>   ⚠️ **2026-09-28 更正**：首轮报「未打通」是**探针 bug**（`execute_script` 返回是**两层 `result`**，
>   只解一层 → 读回恒为空串）。修正后路线④ 增量成立 → 记为**备用路线**（见实施要点 ①）。
>
> **Pydoll 2.27 实施要点（2026-09-28 前置验证累计实测 · 必须带进实现）**
> ① **`execute_script` 返回是两层**：`{'id':N,'result':{'result':{'type','value'}}}` —— 解错层级会**静默拿到空串**
>    （本轮因此误判了两条结论：路线④ 与 M7B-05 的"全 FAIL"都是探针 bug，不是机制问题）。
> ② **退出必须优雅关闭**：Pydoll 默认 `stop()` = **强杀** → Cookie 库**不落盘**、**重启后登录态丢失**；
>    必须发 CDP **`Browser.close`**（实测：强杀 → 丢失 / 优雅 → 保留，脚本 `python/_probe/_diag_persist.py`）。
>    **这条直接决定「单 profile 登录态可复用」能否成立**，是 `M7B-11` 的硬要求。
> ③ **Pydoll 默认已加** `--no-first-run` / `--no-default-browser-check`（再 `add_argument` 会抛
>    `ArgumentAlreadyExistsInOptions`）；`--user-data-dir` 需自己加（**单 profile 靠它**）。
> ④ **Cookie 按域名隔离、端口不参与**：`127.0.0.1:portA` 与 `127.0.0.1:portB` 是**同一 Cookie 域**
>    → 「多站点」必须按**域名**设计（`M7B-08` 用 `127.0.0.1` vs `localhost` 才对）。
> ⑤ **`IO.read(size)` 阻塞到攒够 `size` 或 EOF** ⇒ `size` 即最小推送粒度（逐字取 **128~256**）；
>    取走 body 后 `Fetch.continueResponse` 返回 `-32602`，**必须容忍**。
> ⑥ **键盘事件发给焦点元素**：打字前必须 `focus()`（否则"打了 3 秒但输入框仍是空的"）；
>    文件注入**两条路径都可用**：`expect_file_chooser`（+`user_gesture` 触发 chooser）与
>    `DOM.setFileInputFiles`（后者更稳，且实测**会触发 change 事件**）。
> ⑦ **`browser.stop()` 不得作为退出路径**（源码级证据）：`browser/chromium/base.py:223-243` 的 `stop()`
>    先发 `Browser.close`，**紧接着**调 `stop_process()` → `browser/managers/browser_process_manager.py:73-89`
>    的 `self._process.terminate()`（Windows = `TerminateProcess` **硬杀**）——**不给 Chrome 落盘时间**
>    ⇒ 持久 Cookie 丢失（本轮实测：强杀=False / `close`+**等 3 s**=True）。**正确顺序**：
>    `Browser.close` → **等 CDP 端点消失 / 进程退出**（默认 5 s）→ 超时才强杀（`MB-D0-8` L1）。
>    附带：`_is_browser_running()` 是 **CDP ping**（不是进程检查）⇒ **用户手动关窗后调 `stop()` 会抛
>    `BrowserNotRunning`**，实现层**必须吞掉**并视作正常收尾。
> ⑧ **`browser_preferences` 是库级一等公民**：`ChromiumOptions.browser_preferences`（dict）由
>    `_set_browser_preferences_in_user_data_dir()` 写入 `<user-data-dir>/Default/Preferences`，
>    且实现为**先备份再读-合并-写**（`base.py:1026-1055`，`Preferences.backup`）—— 若将来需要
>    `Session.restore_on_startup` 之类的 prefs（会话 Cookie 保活，见 `MB-Q6`），**这是唯一正路**；
>    默认**不启用**（不写用户 profile 的 `Preferences`）。
> ⑨ **登录态读数有作用域**（`M7B-17` 证据采集按此写）：`tab.get_cookies()` 在无 `browser_context_id` 时
>    走 `Network.getCookies`（**不带 urls**）⇒ **只回当前页面 URL 的 Cookie**；停在 `about:blank` **必读空**。
>    **全库读取一律 `Storage.getCookies`**（诊断：`python/_probe/_diag_cookie_scope.py`）。
> ⑩ **profile 被占的两种失败形态都要识别**（`M7B-11` · L3）：**同端口**再启 → `start()` **假成功**
>    （实为**附着到既有实例**，此后 `stop()` 会**关掉用户浏览器**）；**随机端口** → `FailedToStartBrowser`（超时）。
>    ⇒ 启动前**先探调试端口 / profile 锁**再决策；**禁止以强杀解决"占着"**（`I21` 不静默）。
> ⑪ **`--restore-last-session` 默认禁用**：实测能保会期 Cookie，但**副作用 = 重开上次标签页**
>    （等于替用户发起请求 / 弹窗口）⇒ 会期 Cookie 交给 **L2 快照**（`_diag_snapshot_session.py` 已证可行）。
>
> **探针纪律（本轮教训 · 必须遵守）**：任何「机制不可用」的结论，**先排除探针自身**——
> 读数前先用**已知常量自证**（例：先 `execute_script('return "probe-ok"')` 再读目标）。
> 本轮 `M7B-05` 与路线④ 的"全 FAIL"均由此产生（两层 `result` 解包错误）。
>
> ⚠️ **两条实施要点（实测踩到，必须带进 `M7B-21`）**：
> ① 取走 body 后 `Fetch.continueResponse` 返回 **`-32602 Unable to continue request as is after body is taken`** → **必须容忍**（不影响响应交付）；
> ② 读到 0 字节时只在 **`eof=true`** 才结束循环（不要把"暂无数据"当结束）。

> **`M7B-01` / `M7B-05` / `M7B-08` 实测结论（2026-09-28 · 本地页面 + 自助脚本）**
>
> | 项 | 判定 | 关键证据 |
> |---|---|---|
> | `M7B-01` headful + 单 profile + **CDP 读 Cookie（含 HttpOnly）** | ✅ **全 PASS** | profile 落 `~/.brain-ai/pydoll-profile`（1064 文件，含 `Cookies` 库）；CDP 读回 4 条含 `m7b_http`（`httpOnly=true`），而 **`document.cookie` 看不到它**（=`['m7b_js','m7b_js_session','m7b_plain']`）；**重启同 profile 后持久 Cookie 仍在**、会话 Cookie 正确消失 |
> | `M7B-05` 页面驱动（humanize 打字 + 文件注入） | ✅ **全 PASS** | 打字 21 字符 / 3234 ms（**154 ms/字符**），**21 次 input 事件、间隔 144~156 ms** → 真**逐字符**；值读回逐字一致；**文件注入两条路径都成功**（`expect_file_chooser` → `FILES=m7b-a.png:120|m7b-b.txt:64`；`DOM.setFileInputFiles` → 同样结果且**触发 change 事件**） |
> | `M7B-08` 单 profile 多站点 + 按 origin 注销 | ✅ **全 PASS** | 同一浏览器/profile 下两域并存（全库 7 条）；`Storage.clearDataForOrigin(A)` 后 **A 视角 = []、B 视角 3 条完整**、B 页面 `document.cookie` 仍可用 → **注销 A 不动 B** |
>
> ⚠️ **本批暴露出「探针自身正确性」的教训**：首轮 M7B-05 与路线④ 双双"全 FAIL"，
> 根因是**读回层级解错**（两层 `result`）而非机制问题 —— 前置验证脚本**必须让读回自证**
> （例如先读一个已知常量），否则会把"探针 bug"误记为"机制不可用"。
> `expect_and_bypass_cloudflare_captcha()` / `enable_auto_solve_cloudflare_captcha()` / `apply_fingerprint()`
> —— 与「不规避验证码 / 不伪造身份」**直接冲突**，须显式禁用并加断言（`VB2-37`）。
>
> 附带印证：冒烟用 `headless=True` 时 UA 为 `…HeadlessChrome/156.0.0.0…` —— 这**正是** §13 要求
> 必须 **headful** 的现实理由（无头指纹本身就是"非真实浏览器"信号）。
>
> **`M7B-09` 关闭时序 / 持久化 / 接管 实测结论（2026-09-28 · `MB-D0-8` L1~L4 闸门 · `python/_probe/m7b09_*.py`）**
>
> | 轮次 | 问题 | 结果（数字） |
> |---|---|---|
> | **V-a** 三档退出 × 提交窗口 | 哪种退出保登录态？何时算写进库？ | ① `close_wait`（`Browser.close` → **等进程退出**）：**0.22 s** 退干净、Cookie **保住**；`kill_raw`（任务管理器强杀）与 `pydoll_stop`（库默认，close 后立刻 kill）= **丢**，且强杀残留 `Cookies-journal`；② 写 Cookie 后 `0/10/30/60 s` 再强杀：**≥30 s 存活**、短窗丢失 ⇒ **Chrome 是延迟提交（10–30 s 窗口）**，而非"只在干净退出提交" |
> | **V-b** 快照往返 | 属性是否保真、站点是否认账？ | **7/7 PASS**：`Storage.getCookies` → 文件 → 回灌后**名字齐全、`httpOnly` 保持、`document.cookie` 仍不可见、服务端重新认账** |
> | **V-c** 手动关窗 + 残留实例 | 用户点 X 算什么？有既有实例怎么办？ | ① 外部 `Browser.close`（等价点 X）：**0.2 s** 干净退出、**Cookie 保住**，随后 `stop()` 抛 **`BrowserNotRunning`（实现层必须吞）**；② 残留实例占 profile：**同 profile + 同端口**再启会"成功"（实为**静默附着到既有实例**，此后 `stop()` 会**关掉用户浏览器**）· **同 profile + 随机端口**则 **`FailedToStartBrowser`**（超时）⇒ 启动前**必须先探端口/锁**，**禁止以强杀当启动手段**；③ `browser.connect(ws_address)` **attach 可行**（读库 + 驱动页面都正常）⇒ **L3 走 attach** |
> | **V-d** 会期 Cookie（`MB-Q6`） | 无 `expires` 的登录 Cookie 能跨重启吗？ | **每 case 全新 profile**对照：干净退出后 **baseline 会期 Cookie 必掉**（持久 Cookie 保留）；**`--restore-last-session` 能使其存活** ✅，而 `session.restore_on_startup=1` 写 `Preferences` **实测无效**；⚠️ 且该开关**会把上次的标签页重新拉起来**（实测 2 个 marker 页被恢复）⇒ **不可作默认手段** |
>
> **由四轮收口的四条结论**（已落到 §13 / §14.2 / `I23⑤`）：
> ① **L1 保「持久」、L2 保「会期」** —— 真实站点常见的会期 Cookie 是 L1 救不回的；`_diag_snapshot_session.py`
>    端到端证明 **L2 回灌能救回且服务端认账**（`session` / `httpOnly` 属性保持）⇒ **L2 是必需项而非可选项**；
> ② **L3 = 善意 attach**（`connect(ws_address)`）**或如实告知用户**，不是"接管后强杀"；
> ③ **登录态读数有作用域**：`tab.get_cookies()` 在无 `browser_context_id` 时走 `Network.getCookies`（**不带 urls**）
>    ⇒ **只回当前页面的 Cookie**，停在 `about:blank` 必读空；**全库读取一律 `Storage.getCookies`**
>    （`_diag_cookie_scope.py` 实证：②空 / ③导航后 13 条 / ④全库 16 条 / ⑤按 URL 13 条）。
>    本轮 V-c 的"attach 读到 0 条"即此因（**不是** profile 被换 —— `chrome://version` 的 `Profile Path` 已证 profile 正确）。
> ④ **L2 快照节奏（`MB-Q5` 配套）**：Chrome 的提交窗口实测 **10–30 s** ⇒ 快照刷新间隔必须 **< 10 s**，
>    并叠加两个**事件触发点**（**登录成功即写** + **优雅退出前再写一次**）；否则崩溃/断电时，
>    快照会比真实登录态**旧一个窗口**（用户"刚登录就断电"的情形最吃亏）。
>
> ⚠️ **探针收尾纪律（本轮踩到）**：探针若崩在 `start()` 之前，会**遗留孤儿实例占住 profile**，
> 使**下一次 `start()` 直接 `FailedToStartBrowser`**（实测遇到两次）。⇒ 探针必须 `try/finally` 收尾，
> 并清点/清理归属该 profile 的残留进程（`m7b09_common.stray_browsers()/kill_strays()`）。

---

## 6. 实施任务（`M7B-10`~`M7B-41` · 6 批 · **每批结束跑 §11 基线并贴数字**）

### 批 1 —— 底座（Python 侧）

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-10` | Python 包骨架（daemon / pipe / pydoll wrapper） | `python/brain_ai_browser/`（新；**文档中只写反引号、不写链接**） |
| `M7B-11` | 守护进程：自启动、后台、心跳、崩溃检测、日志脱敏 **+ 退出协议与异常埋点**（`MB-D0-8` L1/L4） | **退出 = `Browser.close` → 等进程退出（默认 5 s）→ 超时才强杀**；**用户手动关窗 = 可恢复状态**（`BrowserNotRunning` 必须被吞，不得报错）；未干净退出 → 下次启动提示「上次异常退出，登录态可能已回滚」+ 一键重登 / 从快照恢复；不留僵尸进程 |
| `M7B-12` | 管道协议 v1（命令 / 事件 JSON 行 + 版本号 + 非法行拒绝） | 离线桩可断言（`VB2-29`） |
| `M7B-13` | Python 运行时检测与打包策略（`Q7` / `P7b-15` → **M6 硬门槛**） | 未安装 → 引导 + **不阻塞其他功能** |
| `M7B-14` | 浏览器检测与引导（Chrome 缺失 → Edge 兜底 → 可操作文案） | 两条路径都可起浏览器 |

### 批 2 —— C++ 通道（**中间检查点**）

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-15` | `web/pipe_client.{h,cpp}`（工作线程阻塞等待，`Q4`） | 不阻塞 UI 线程 |
| `M7B-16` | `web/pydoll_channel.{h,cpp}`：替代 `run_script_sync` / `ensure_session`（原 `webview_host.h:207/221`） | 接口收敛为「按站点 + 脚本 → JSON」 |
| `M7B-17` | `SessionStore` 改造：证据来源 = **CDP Cookie 快照**（`I15′`） | 仍是只读暴露 + 仅内存；证据取 **`Storage.getCookies`（浏览器级 · 全 origin · 含 HttpOnly）**；**加密快照落盘归 L2 独立组件**（`session_snapshot`，**不喂** `SessionStore`，`MB-D0-8`） |
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

### 批 5 —— 撤除 WebView2（**唯一不可逆点** · `MB-D1` = 先建后拆）

> **进入条件**（硬门槛）：批 1–4 全绿 **且** §10 的 12 条站点选择器**全部回填完毕**。
> **本批合并了全部「删除类」动作**（`MB-D1` 顺位修正 · 2026-09-28）：配置表收敛（§7）与删除类断言（§9.2）
> **一律在此批落地**（`M7B-42` / `M7B-43`）—— 批 1–4 **只新增、不删改**（详见 §9.3）。
> `M7B-42` / `M7B-43` 为本次顺位修正**追加**（编号续在批 6 之后，见 §15 v2）。

| 编号 | 任务 | 验收 |
|---|---|---|
| `M7B-30` | 删 `web/webview_host.{h,cpp}`、`tools/webview2_login.cpp` | 编译通过（无残留引用） |
| `M7B-31` | `CMakeLists.txt` / `vcpkg.json` / `build.ps1` 移除 WebView2 | 构建 0 error / 0 warning；产物不再含 WebView2 DLL |
| `M7B-32` | `paths::webview2_profile()` 退役 + 遗留目录提示（**不自动删用户数据**） | 启动时一次性提示「需重新登录」 |
| `M7B-33` | UI 文案换代：`property_panel.cpp:194/195/283/310` + 状态栏 | 无「WebView2」字样残留 |
| `M7B-34` | **CLI 契约重建**（§8）：`--provider` 必填 + 候选枚举 + `--provider auto` + `--web-probe` 废弃 | `VB2-34` |
| `M7B-35` | `I2` 解冻留痕 + `I20`/`I21`/`I22` 进冻结区 | `source/README.md` §4.2 |
| `M7B-42` | **配置表收敛与警告路径**（§7 全表）：白名单收敛 `{dom}`；`builtin:deepseek` / `web.protocol=deepseek-web` → **警告不报错**；`endpoints` / `probe_paths` / `challenge_path` / `completion_path` / `token_expr` → 保留解析 + 标注「已被 `M7B` 取代」；`--web-probe` 废弃（`--help` 标替代命令） | `VB2-28` / `VB2-33` / `VB2-34` |
| `M7B-43` | **删除类断言一次性落地**（§9.2）：删 `VB2-17`（前半）· `VB2-25①③④`；`VB2-25′` 转正 | 数字**在本批内**先降后升闭合（§9.3） |

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
| `I15 → I15′` | 登录态判据 = **该 origin 的 Cookie 名单（CDP 读取）+ 页面可交互证据**；**不得**依赖厂商专有物（原 `userToken` / `ds_session_id` 随协议栈退役） | 判据**站点无关**这一精神不变。**补注（`MB-D0-8`）**：证据来源扩展为「**CDP Cookie + L2 加密快照**」，但**判定逻辑不变**（仍是 Cookie 名单）；`SessionStore` 的「仅内存」纪律**不因 L2 松动**（快照是独立组件） |
| **`I23`（新）** | **登录态持久化**：① 退出必须**先 `Browser.close` 并等待进程退出**（超时才强杀），**禁止「close 后立刻 kill」**；② 持久 Cookie 的存续**不得依赖单一机制**（干净退出 **+** 加密快照双保险）；③ 快照**不得明文落盘、不得外传**，UI / 日志一律脱敏；④ 恢复失败必须**显式提示**并引导重新登录（`I21` 同族，不静默）；⑤ **保活手段不得改变用户可见行为** —— 会重开上次标签页的启动开关（`--restore-last-session` 等）**默认禁用**，**会期 Cookie 由 L2 承担**（实测：L1 只保持久 Cookie） | 由 `VB2-38`（关闭协议离线桩）/ `VB2-39`（快照无明文）断言 |
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
| **`VB2-37`（新）** | **合规禁用清单零命中**：源码内不出现 `expect_and_bypass_cloudflare_captcha` / `enable_auto_solve_cloudflare_captcha` / `apply_fingerprint` / `FingerprintApplier`（§13） | |
| **`VB2-38`（新）** | **关闭协议（`I23①`）离线桩**：退出路径缺「`Browser.close` + 等进程退出」即判失败（纯逻辑桩，不启浏览器）；`BrowserNotRunning` 必须被吞并归为**正常收尾**；超时兜底强杀必须**留 warn**（登录态可能回滚） | |
| **`VB2-39`（新）** | **快照无明文（`I23③`）**：快照文件字节内**不出现**任何 Cookie 名 / 值明文（DPAPI 加解密往返 + 明文扫描双重断言）；`SessionStore` 导出物同样零明文 | |

### 9.3 基线数字的预期路径（**先建后拆**：单调段 + 批 5 内闭合）

```
批 0（锚点 a1a7e0c）: --exec-selftest 251 / 0  ← 2026-09-27 记录；**开工前须复测取现值**
   │  仅新增：VB2-25′ · VB2-29~32 · VB2-36（旧断言**一条不删**）
   ▼
批 1–4: ≥ 251 / 0 —— **只升不降**（任何下降 = 真回归 ← 先建后拆的核心收益）
   │  批 5 同批内完成「删 + 加」：
   │    − 删 VB2-17（前半）· VB2-25①③④
   │    + VB2-28 / VB2-33 / VB2-34 / VB2-35（配置收敛 + CLI 契约 + 解冻守卫）
   ▼
批 5 结束: 新值 / 0 —— **先降后升在本批之内闭合**（无跨批过渡态）
   ▼
批 6 收口: 记录新值并**全绿**（数字回填 §11）
```

> **纪律（`MB-D1` = 先建后拆）**：批 5 之前**不得**删除任何旧断言、**不得**收敛配置表（§7）、**不得**废弃 `--web-probe`
> —— 它们**全部归批 5**（`M7B-42` / `M7B-43`）。理由：WebView2 通道在批 1–4 **仍在生产路径上**，删掉「守它的断言」
> 会制造一个**说不清是回归还是意图**的窗口；压缩到批 5 之后，批 1–4 内的任何数字下降都**唯一指向真回归**。
>
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
| `doubao-web` | `dom` | ❌ 空（未登录不渲染输入框） | ❌ | ❌ | **网页版图片理解首个目标站（方案 B′ · 2026-09-29 · `M7.md` `D10`）**：人工登录后一次取齐三项 **+ 上传入口侦察 + 视觉性质判别**（`python/_probe/m7b28_doubao_recon.py`，见 `M7.md` `P7b-05b`） |
| `tongyi-web` / `chatglm-web` / `spark-web` / `chatgpt-web` / `claude-web` / `gemini-web` | `dom` | ❌ 空（登录型条目） | ❌ | ❌ | 人工登录后 `--web-dom-dump --provider <id>` **一次取齐三项**（`M7B-28`） |
| **全部 11 条 dom** | — | — | — | — | `cookie_names` 回填（配合 `D-30`「`cookie_names` 优先」）+ `answer_selector` 回填（`M7B-29`，承接 `PB2-29`） |

**登录 Cookie 存活口径（`M7B-06b` 回填 · `MB-D0-8` L1 的端到端验收）**

> 「下次免登录」**不是无条件承诺** —— 语义取决于站点登录 Cookie **是否带 `expires`**：
> 无 `expires` 的**会话 Cookie**，即便干净退出也会掉（实测见 §5 结论块与 `M7B-09`）。
> 届时必须**如实告知**用户（`I21` 不静默）；如需保活，走 `MB-Q6` 的可选开关。

| 条目 | 登录 Cookie 名 | 有 `expires`? | `httpOnly`? | 干净退出 + 重启后仍登录? | 实测日期 |
|---|---|---|---|---|---|
| （12 条逐行回填） | | | | | |
| `doubao-web` | ⬜（`P7b-05b` B0 段） | ⬜ | ⬜ | ⬜ | ⬜ |

> **豆包一行（2026-09-29 计划）**：随 `M7.md` `P7b-05b` 的 **B0 段**（人工登录一次）一并回填 —— 登录前 / 后 Cookie 快照 + 登录 Cookie 名 / `expires` / `httpOnly` / 「干净退出 + 重启后仍登录」。其**「登录前后 Cookie 名差集」同时作为 `D-30`（`cookie_names` 优先）的豆包证据**。

**回填记录格式**（每站一行，写入 §15 变更记录）：

```
<id>：input=<命中选择器> · send=<键/点击> · answer=<选择器> · done_when=<判据> · cookie_names=<名单>
      实测日期 / 浏览器版本 / 备注（如「需先关掉新手引导」）
```

**网页版图片理解侦察记录**（`P7b-05b` 执行时逐行追加；判定口径见 `M7.md` §12.1 —— 上传入口 4 类 + **纯图无字**视觉性质判别）：

| 条目 | 上传入口形态 | 注入路线 | 网络回执 | 视觉性质 | 实测日期 |
|---|---|---|---|---|---|
| `doubao-web` | ⬜ `file_input` / `drop_zone` / `paste_only` / `none` | ⬜ `expect_file_chooser` / `DataTransfer` | ⬜ | ⬜ 真视觉 / OCR / 无入口 | ⬜ |
| （待填） | | | | | |

---

## 11. 验证基线与文档同步

### 11.1 基线

| 命令 | 当前记录（2026-09-27） | `M7B` 目标 |
|---|---|---|
| `source\build.ps1` | 0 error / 0 warning | **不变**（硬指标） |
| `api_probe --selftest` | 七组 PASS | 换代后全绿 |
| `api_probe --exec-selftest` | **251 / 0** | 批 1–4 **只升不降**；批 5 内**先降后升闭合**（§9.3）→ 记录新值并全绿 |
| `api_probe --graph-selftest` | 110 / 0 | 不变 |
| `aiwrite --provider-selftest` | 50 / 0（`--provider-dump` 21 条：official 9 / web 12） | 换代后全绿 |
| `aiwrite --provider-dump` | 21 条（official 9 / web 12） | 条目数不变 |
| `aiwrite --run-selftest` / `--run-selftest --web` | PASS / PASS（5-5） | 换通道后重测 |
| `--login-selftest` / `--web-chat` | 0 / 1 / 2 语义 | **调用形式与退出码不变**（`I20`） |
| `--web-probe` | 可用 | **废弃**（`--help` 标注替代命令） |
| **新增** `--web-stream-selftest --provider <id>` | — | CDP 增量逐帧 PASS（`I22` 断言） |
| 文档断链 | broken **0** | **不变**（新文件先只写反引号） |

> **批 0 实测（2026-09-28 复测 · 锚点 `pre-m7b` / `a1a7e0c` · Debug · 本机）**
> 环境登记：**Python 3.12.1**（`F:\Python`）+ 项目内 **`.venv`** · **pydoll-python 2.27.0** ·
> **Chrome 156.0.8072.0**（`C:\Program Files\Google\Chrome`）· **Edge 存在**（`C:\Program Files (x86)\Microsoft\Edge`）
>
> | 命令 | 实测 |
> |---|---|
> | `source\build.ps1` | **编译成功**（增量：无重编译单元 → 无警告输出；上次全量 = 0 error / 0 warning） |
> | `api_probe --selftest` | **七组 PASS**（exit 0） |
> | `api_probe --exec-selftest` | **251 / 0**（exit 0） |
> | `api_probe --graph-selftest` | **110 / 0**（exit 0） |
> | `aiwrite --provider-selftest` | **50 / 0**（exit 0）· `--provider-dump` **21 条** |
> | `aiwrite --run-selftest` | **PASS**（exit 0） |
> | `aiwrite --run-selftest --web` | **PASS**（exit 0 · 真实联网跑通） |
> | 文档断链 | **265 / 0** |
> | Pydoll 冒烟（一次性脚本） | **PASS**：自动探测 Chrome → CDP 读 Cookie → `execute_script` → 关窗 |
>
> → **批 1–4 的守门数字 = `251 / 110 / 50`**（`--exec-selftest` **只升不降**，§9.3）。
> ⚠️ 本次复测**纠正一处文档错误**：`--provider-selftest` / `--provider-dump` 属 **`aiwrite.exe`**，
> **不是** `api_probe.exe`（原表写错，已修并补 `--provider-dump` 行）。

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

> **2026-09-29 追加（方案 B′ · 只改文档）**：`M7.md` 另增 **`M7-10`**（DeepSeek 官方 API 视觉跟进）/ **`P7b-05b`**（网页版图片理解只读侦察，目标站 = 豆包）/ **`D10`**（首个目标站决议），以及 [网页版协议实测记录.md](../网页版协议实测记录.md) **§8**（站点多模态能力公开证据核查）与 `roadmap.md` / `ai_writer_nodes.md` / `milestone_plan.md` / `DevPlan.todo` 的口径同步 —— 随 `M7B-40` 一并复核。

---

## 12. 回滚点（**阶段级** —— 因为没有第二个引擎可切）

| 级别 | 手段 | 影响面 |
|---|---|---|
| 一级（零代码） | 条目改回 `adapter` 走旧通道 / 改用 `protocol=openai` 的 official 条目 | 单条目 |
| 二级（进程） | 不启用任何 Pydoll 条目 → **不启动** Python 守护进程、不装依赖、不占端口（对齐 `Q7` / `P7b-15`：**不阻塞其他功能**） | 全局 |
| 三级（阶段） | `M7B-01`~`09` + `M7B-06b` 未**全**过 → **整体停留**（同 P7-b 模式）；批 5 之前任一批未全绿 → 停在原批（**WebView2 仍在，可切回**） | 计划级 |
| 四级（代码） | 批 5 之后（WebView2 已删）**只能 `git revert`** —— 故本文强制「**先建后拆**」（`MB-D1`） | 变更集级 |

> **回滚锚点（`MB-D1` 的物理前提）**：标签 **`pre-m7b`** → 提交 **`a1a7e0c`**（2026-09-28：M7 第一轮代码 + 全部文档已入库并推送）。
> `git revert` / `git reset --hard` **必须**以该标签为界；**标签缺失则锚点不成立**（不得靠记哈希）。
>
> **关键纪律**：批 5 是**不可逆点**。进入批 5 的**前置条件** = 批 1–4 全绿 + §10 的 12 条站点选择器**全部回填完毕**。
>
> **`MB-D0-8` 四层保障对回滚的影响**：L1 / L3 / L4 是**行为约定**（无新增可回滚资产）；L2 新增
> `~/.brain-ai/session/`（**纯新增目录**，删掉即等于未启用）→ **不进回滚清单**；
> 快照仅供**本机同一用户**（DPAPI 用户态密钥）解密，**跨机无意义**，故**不属于用户数据迁移范围**。

---

## 13. 合规红线（**不因反检测诉求放宽**）

撤 WebView2 的**正当理由**：嵌入控件 ≠ 真实浏览器，会被站点**误判**为机器人。据此采取的 `humanize`
（Bezier 鼠标 / 随机打字延迟）**仅限「像正常浏览器一样操作」**；**仍然**：

- 有头窗口 + 用户**手动**登录；
- **不代填密码**、**不绕过验证**、**不伪造站点鉴权**；
- **不针对验证码做规避**（`M7b` 不引入任何打码 / 绕过能力）；
- 若站点要求人机验证，**流程停在那里等用户**（这是「像人一样」的自然结果，也是边界）。

（同族口径：[../节点编辑器使用说明.md](../节点编辑器使用说明.md) §10.3、[M7.md](M7.md) §12、`P7b-14`。）

- **库级禁用清单（Pydoll 2.27.0 实测 · 断言 `VB2-37`）**：不得调用库内自带的
  `expect_and_bypass_cloudflare_captcha()` / `enable_auto_solve_cloudflare_captcha()`（**验证码规避**）、
  `apply_fingerprint()` / `FingerprintApplier`（**指纹伪造**）—— 与「像真实浏览器一样操作」≠「伪装成别人」的
  边界冲突。这三条**不是"暂不使用"，而是代码中零命中**（grep 断言）。
  另：本库存在 `headless` 选项，但**本项目的 Pydoll 通道一律 headful**（无头指纹本身即风险信号，见 §5 实测）。

- **启动与登录态的两条"不做"（`MB-D0-8` L3/L4 · 实测代价见 §5 `M7B-09` V-c/V-d）**：
  ① **禁止**为"启动浏览器"而**强杀**既有实例 —— 占着 profile 的可能是**用户正在用的窗口**；
  profile 被占时只允许**善意 attach**（`browser.connect(ws_address)`）或**如实告知用户**（`I21` 不静默）。
  ② **禁止默认使用**会**重开用户上次标签页**的启动开关（`--restore-last-session` 等）—— 那等于**替用户发起请求**、
  弹出用户没要的窗口；会期 Cookie 的保活改由 **L2 加密快照**承担（仅供本站点 origin，见紧接着的一条红线）。

- **登录态快照红线（`MB-D0-8` L2 · 断言 `VB2-39`）**：Cookie 快照**只允许经 DPAPI 加密后落在
  `~/.brain-ai/session/`**；**禁止**明文落盘、**禁止**写入日志 / 界面 / 工作流文件、**禁止**任何形式外传
  —— 与 `SessionStore`「Cookie 不写盘」的既有纪律同族（`web/session_store.h:6`），L2 只是把
  「必须落盘的那一份」**限定为加密文件 + Cookie 子集**。

---

## 14. 开口项与待拍板

### 14.1 待拍板（`MB-D2`~`MB-D6` · `MB-D1` 已定）

| 编号 | 事项 | 建议 |
|---|---|---|
| ~~`MB-D1`~~ | **撤除时机**：一次性删 vs 先建后拆 | ✅ **已定（2026-09-28）：先建后拆** —— 批 1–4 保留 WebView2（可切回），批 5 才删；**全部删除类动作（配置收敛 / 断言删除 / CLI 废弃）归批 5** |
| `MB-D2` | 站点与标签：每站点一个 tab / 复用单 tab | 每站点一个 tab（登录态由单 profile 共享） |
| `MB-D3` | `adapter=builtin:deepseek` 的处理：警告降级 vs 直接拒绝 | **警告 + 按 `dom` 语义**（守「旧文件能加载」） |
| `MB-D4` | `DevPlan.todo` 编号：M7 组续号 vs 新 `M7B` 组 | 续号（`189+`），与 `Q8` 先例一致 |
| `MB-D5` | 版本与顺序：`M7B` = v0.5.4 排在 P7-a（v0.5.2）之后，与 P7-b（v0.5.3）**交织** | **P7-a → `M7B` 批 1–2（通道底座）→ P7-b 上传 + `M7B` 批 3+** |
| `MB-D6` | Python 内嵌与否（`Q7` 升级为 **M6 硬门槛**） | **内嵌**（否则「非技术用户独立安装」的退出条件不成立） |

### 14.2 开口项（`MB-Q1`~`MB-Q3`）

| 编号 | 开口项 | 建议默认 |
|---|---|---|
| `MB-Q1` | **CDP 增量主路线**（`M7B-03` 出结果后回填本文） | ✅ **已定（2026-09-28 实测，见 §5）**：主路线 = **`Fetch.takeResponseBodyAsStream` + `IO.read(size=128~256)`**；`size` 即最小推送粒度（实测 256 B/帧）。`EventSource` 站点走 `Network.eventSourceMessageReceived`；`dataReceived` 仅作进度；**页面 hook 可用（备用，8 帧/5568 ms）** |
| `MB-Q2` | 多候选选择器的语法（数组 vs `a\|b` 字符串） | **数组**（显式、可校验、可给「第 N 候选命中」诊断） |
| `MB-Q3` | 是否保留 official API 作为「无浏览器退路」 | **保留**（已是现有能力，`R1` 的主要缓解） |
| `MB-Q4` | **L2 快照范围**：全 origin vs 仅配置表内网页版条目 origin | **① 仅配置表内网页版条目 origin**（隐私面最小、够用；配合 `I14`「站点只来自条目」）；全 origin 作高级开关 |
| `MB-Q5` | **L1 关闭等待阈值**（等进程退出的上限，超时才强杀） | **5 s**（可配）；实测若普遍 < 2 s 可下调 |
| `MB-Q6` | 是否启用 `browser_preferences`（如 `Session.restore_on_startup`）以保活**会话 Cookie** | ✅ **已定（2026-09-28 实测 · §5 `M7B-09` V-d）：默认不启用**（不写用户 profile 的 `Preferences`）—— ① `session.restore_on_startup=1` 写 `Preferences` **实测无效**；② 唯一有效手段 `--restore-last-session` **会重开上次标签页**（副作用，见 §13）；③ 会期 Cookie 改由 **L2 加密快照**兜底（实测可救回且服务端认账）⇒ **无需为此写 prefs**；仅当 `M7B-06b` 发现某站点会期 Cookie 无法回灌且用户明确要求时才作高级开关 |

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
| 2026-09-28 | v2 | **`MB-D1` 已定 = 先建后拆**（用户拍板）→ ① §6 批 5 升格为「唯一不可逆点」并写明进入条件；② **顺位修正**：§7 配置收敛与 §9.2 删除类断言**全部压到批 5**（新增 `M7B-42` / `M7B-43`），**批 1–4 只新增不删改**；③ §9.3 数字路径改写为「批 1–4 只升不降 + 批 5 内闭合」（原「批 3–5 过渡态」取消）；④ §12 登记**回滚锚点**（提交 `a1a7e0c` / 标签 `pre-m7b`）；⑤ §1.3 修路径笔误（`tools/api_probe.cpp` 不在 `src/` 下）。**仍为计划、未开工** |
| 2026-09-28 | v3 | **阶段 0 完成（文档 + 环境，未动一行产品代码）**：① **批 0 基线实测回填**（§11.1：build OK · selftest 七组 · exec **251/0** · graph **110/0** · provider **50/0** · run-selftest **PASS** · run-selftest --web **PASS** · 断链 **265/0**）；② 纠正文档错误：`--provider-selftest` / `--provider-dump` 归 **`aiwrite.exe`**；③ **Pydoll 2.27.0 环境登记 + 冒烟通过**，并据此**修正三处计划假设**（§5：网络日志/响应体有库级 API；文件注入优先 `expect_file_chooser`；`Edge` 兜底 API 已确认）；④ **新增合规禁用清单**（§13）与断言 **`VB2-37`**（验证码规避 / 指纹伪造 API 零命中）；⑤ `.gitignore` 补 Python 运行时忽略项。**仍未开工** |
| 2026-09-28 | v4 | **`M7B-03` 前置验证通过（结论已回填）**：用**本地页面**（`python/_probe/local_server.py`）把「CDP 能否拿增量」与「站点登录 / 改版」解耦，5 个变体实测 ⇒ **主路线 = `Fetch.takeResponseBodyAsStream` + `IO.read(size=128~256)`**（`size` 即最小推送粒度；实测 256 B/帧、41 帧 / 10 KB 流）；② 仅 `EventSource` 站点可用；③ 只给进度；④ 页面 hook 未打通（备用）。**并新增两条实施要点**（`-32602` 必须容忍；0 字节只在 `eof=true` 结束）。证据：`python/_probe/out/m7b03_cdp_stream.json`。**其余 7 项前置验证未跑** |
| 2026-09-28 | v5 | **`M7B-01` / `M7B-05` / `M7B-08` 前置验证通过 + 一条**错误结论更正**：① `M7B-01` 全 PASS（headful、profile 落盘 1064 文件、**CDP 见 HttpOnly 而 `document.cookie` 看不到**、重启后持久 Cookie 保留）；② `M7B-05` 全 PASS（**154 ms/字符** 逐字输入 + 21 次 input 事件；文件注入**两条路径**都成功且触发 change）；③ `M7B-08` 全 PASS（两域并存 → 清 A 后 **A 空 / B 完整**）；④ **更正**：路线④（页面 hook）**其实可用**（8 帧 / 5568 ms），首轮"未打通"是**探针读回层级 bug**（`execute_script` 返回**两层 `result`**）；⑤ 新增 **6 条 Pydoll 实施要点**（含**退出必须 `Browser.close` 否则登录态不落盘**、`--no-first-run` 已由库添加、Cookie 按域名隔离、`IO.read` 攒批、打字需先 `focus()`）。证据：`python/_probe/out/m7b0{1,5,8}*.json`、`_diag_persist.py`。**仍剩 `M7B-02`/`M7B-04`（C++↔Python 管道）与 `M7B-06`/`M7B-07`（需人工登录）** |
| 2026-09-28 | v6 | **登录态持久化改进（用户拍板：四层全做 · `MB-D0-8`）** —— 触发：前置验证暴露 **Pydoll `stop()` = 「`Browser.close` 后立刻 `terminate()`（硬杀）」** → Cookie 库不落盘 → **登录态每次退出即丢**（源码证据 `base.py:223-243` + `browser_process_manager.py:73-89`；实测强杀=False / `close`+等 3 s=True）。落盘改动：① §2 新增决议 **`MB-D0-8`**（L1 干净退出 / L2 DPAPI 加密快照 / L3 启动自愈 / L4 异常退出可见）；② §3 架构表新增「**退出 / 关闭**」与「**登录态持久化**」两行，并收紧「会话存储」行（落盘只经 L2、**不喂** `SessionStore`）；③ §5 实施要点 **+⑦⑧**（`stop()` 禁用 + 源码行号；`browser_preferences` 为库级一等公民、读-合并-写带 backup）并新增**探针纪律**（「机制不可用」结论须先排除探针自身）；④ §5 前置验证 **+`M7B-09`**（关闭时序三档 + 提交时机 + 快照往返 + 接管）与 **+`M7B-06b`**（真站点「重启仍登录」+ 记录 `expires`）；⑤ §6 `M7B-11` 加**退出协议与异常埋点**、`M7B-17` 明确证据取 `Storage.getCookies`；⑥ §9.1 新增不变量 **`I23`** 并给 `I15′` 加补注；⑦ §9.2 新增断言 **`VB2-38`**（关闭协议离线桩）/ **`VB2-39`**（快照无明文）；⑧ §10 新增「**登录 Cookie 存活口径**」回填表（把"下次免登录"从口号变成可核对的数字）；⑨ §12 登记 L2 对新目录**不进回滚清单**；⑩ §13 新增**登录态快照红线**；⑪ §14.2 新增 `MB-Q4`/`MB-Q5`/`MB-Q6`（快照范围 / 关闭等待 5 s / prefs 默认不启用）。**仍只改文档：产品代码零改动** |
| 2026-09-28 | v7 | **`M7B-09` 前置验证通过（`MB-D0-8` L1~L4 四层全部得到实测支撑）** —— V-a~V-d 四轮 + 4 项补充诊断：① **V-a**：`close_wait`（`Browser.close` → 等进程退出）**0.22 s** 即保住 Cookie；强杀与库默认 `stop()` 丢，强杀残留 `Cookies-journal`；写 Cookie 后**≥30 s** 再强杀仍存活 ⇒ **10–30 s 延迟提交窗口**；② **V-b**：快照往返 7/7（`httpOnly` 保持、站点认账）；③ **V-c**：外部关窗（等价点 X）**0.2 s** 干净退出且 Cookie 保住、`stop()` 抛 **`BrowserNotRunning`（须吞）**；残留实例占 profile 时 **同端口 = 静默附着到既有实例 / 随机端口 = `FailedToStartBrowser`**，`browser.connect(ws)` **attach 可行**；④ **V-d**：**每 case 全新 profile** 对照 ⇒ 干净退出后**会期 Cookie 必掉**（持久 Cookie 保留），**`--restore-last-session` 是其唯一有效保活手段**但**会重开上次标签页**（副作用，实测 2 个 marker 页被恢复）、`session.restore_on_startup=1` 写 `Preferences` **无效**；⑤ **补充诊断**：`_diag_cookie_scope` 证 `tab.get_cookies()` **是当前页作用域**（停在 `about:blank` 必读空；全库须 `Storage.getCookies`）· `_diag_snapshot_session` **端到端证明 L2 回灌能救回会期 Cookie**（`session`/`httpOnly` 保持 + 服务端 `/eyes` 认账）· `_diag_restore_tabs` 证开关副作用 · `_diag_profile_identity` 用 `chrome://version` 排除"profile 被换"假设。落盘文档改动：§5 `M7B-09` 行改为**已过**并新增四轮结论块（含**探针收尾纪律**：崩溃会留孤儿占 profile → 下次 `start()` 直接失败）；§5 实施要点 **+⑨⑩⑪**（读数作用域 / profile 被占两形态 / `--restore-last-session` 默认禁用）；§9.1 **`I23⑤`**；§13 新增**启动与登录态两条"不做"**；§14.2 **`MB-Q6` 结案**（默认不启用 prefs，会期 Cookie 交 L2）；`source/README.md` §6.2 **+3 条探针纪律**；`docs/CHANGELOG.md` 前置验证进度更新。**仍只改文档 + 一次性探针：产品代码零改动** |
| 2026-09-28 | v8 | **`M7B-09` 补一条收口（L2 快照节奏）**：由 V-a 的提交窗口数字（**10–30 s**）推出实现要求 —— 快照刷新间隔 **< 10 s** + 两个事件触发点（**登录成功即写** / **优雅退出前再写**），否则崩溃/断电时快照会旧一个窗口。同时固化工程纪律：`m7b09_common.shutdown()` 增加**残留自检与清理**（`strays_after` / `strays_killed`），探针一律 `try/finally` 收尾 —— 本轮两次因孤儿实例占住 profile 导致下一次 `start()` 直接 `FailedToStartBrowser`。**仍只改文档 + 一次性探针** |
| 2026-09-28 | v9 | **文档进度同步（8 个文件 · 只改文档）** —— 前置验证已跑到 **5/9**，但「进度条类」文档仍停在「已立项 · 计划中未开工」，逐处对齐：① `M7B.md` 顶部状态行、§5 标题与进度摘要、`M7B-02` 补「**C++ 工具链冒烟已通**，剩管道双向 IPC 本体」、`M7B-04` 标 🟡 部分；② `CHANGELOG` 前置验证块新增「⏳ 待过 4 项 + 1 项加强」；③ `docs/README.md` 当前进度行、`milestone_plan.md` 状态列与前置口径（`M7B-01`–`08` → `M7B-01`–`09` + `M7B-06b`）；④ `roadmap.md` 轨道 C + **`RQ-6` 由「未实测」改为已实测收口**（`MB-Q1` 结案）；⑤ `ai_writer_nodes.md` §18 两行（**顺带修掉 P7-a 行仍写「计划中」的过期状态**）；⑥ `DevPlan.todo` `TST-M7B-01` 补进度 + **新增 `FEA-M7B-03`（`MB-D0-8` 四层登录态持久化）**；⑦ `M7.md` §12.1（`P7b-01`/`P7b-04` 标已过、`P7b-03` 标部分）+ §12 上方「外部调研」注更正；⑧ `节点编辑器使用说明.md` §10.2 前置验证补状态；⑨ **反向核对再补 3 处**（`M7B.md` §12 三级回滚口径、`CHANGELOG` 立项块任务口径、`ai_writer_nodes.md` §18 P7-b 行状态）。**验证**：文档断链 **276 checked / 0 broken**、`DevPlan.todo` JSON 合法且节点 **196 → 197**、过期措辞反向核对：`⬜ 计划中` 行 **0 命中**、旧「未过则停留」编号 **0 命中**（唯一保留命中 = 本行对旧措辞的引用）、`source/` **产品代码零改动**（其变更均属 P7-a 变更集） |
| 2026-09-29 | v10 | **方案 B′ 定案（网页版图片理解目标站改豆包）+ 站点多模态能力外部核查（只改文档 · 零产品代码）** —— ① **目标站**：网页版图片理解首个目标站 = **`doubao-web`**（`M7.md` 新增决策 `D10`；理由：条目已存在 + 字节多模态系 + 不动批 4 主线），**`deepseek-web` 保持文字链路主线**（`M7B-06`/`M7B-26` 不变）；② **顺序闸门**：先 `P7b-05b` 只读侦察（`python/_probe/m7b28_doubao_recon.py`）→ 结论**用户审核通过后**才启动 `M7B-02`/`04` 管道本体（`M7.md` §16 已插入该顺序）；③ `M7.md` 新增 **`P7b-05b`**（上传入口 4 类判定 + 三项选择器真实命中 + `D-30` 证据 + **纯图无字**视觉性质判别）与 **`M7-10`**（DeepSeek 官方 API 视觉跟进）；④ **外部核查**：DeepSeek **API 已原生多模态**（2026-09-10 `deepseek-flash` = V4.1-Flash，Vision ✓；`deepseek-v4-pro` ✗）→ 仓库表过时；DeepSeek **网页版**推断无视觉（Web 跑 V4-Pro GA 且 ✗ + 公告只提 API）；**豆包待实测** → 新增 [网页版协议实测记录.md](../网页版协议实测记录.md) **§8**；⑤ §10 表把 `doubao-web` **单列**（从合并行拆出）+ 新增「**网页版图片理解侦察记录**」表 + 「登录 Cookie 存活口径」表补豆包一行（`P7b-05b` B0 段回填）；⑥ `M7.md` `P7b-10` 主路线改写为 **`expect_file_chooser`**（`M7B-05` 已实测两条注入路径均成功）→ 仅当无 `file input` 时才退 `DataTransfer`；⑦ 同步 `M_patchAB_rest.md`（`D-30` 证据计划 + 附录 E 豆包行）/`docs/README.md`/`CHANGELOG.md`/`DevPlan.todo`/`roadmap.md`/`ai_writer_nodes.md`/`milestone_plan.md`。**验证**：文档断链自检 **broken 0**、`source/` **零改动**、`source/assets/providers.json` **未改** |

**站点选择器回填记录**（格式见 §10；`M7B-06` / `M7B-27` / `M7B-28` / `M7B-29` 执行时逐行追加）：

| 日期 | 条目 | input | send | answer | done_when | cookie_names | 备注 |
|---|---|---|---|---|---|---|---|
| （待填） | | | | | | | |







