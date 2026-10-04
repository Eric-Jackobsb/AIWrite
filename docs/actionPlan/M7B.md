# M7B 详细 Action Plan：网页通道整体迁 Pydoll（WebView2 退场 · 文字与图片共用一条通道）

> 里程碑：M7 **第三轮 `M7B`**（与第一轮 M7、第二轮 P7 并列；排在 M6 之前 —— M6 是 v1.0 打包里程碑）
> 版本：**v0.5.4**（内部）
> 前置：M5 核心切片（M5-C）已落地；**P7-a（v0.5.2）不阻塞本文**；**P7-b（v0.5.3）与本文共用通道底座**（见 §6 批 1–2）
> 类型：**架构替换 + 运行时依赖退场**（删 WebView2；`builtin:deepseek` 协议栈退役；新增 Python + Pydoll 运行时）
> 依据：用户 2026-09-28 决议 `MB-D0-1`~`MB-D0-7`（见 §2）；承接 [M7.md](M7.md) §17 `Q2`/`Q3` —— **本文使 `Q2`/`Q3` 作废**（原「两套引擎可切换」不在本文范围）
> 状态：**进行中 —— 前置验证 7/9 已过**（`M7B-01`/`02`/`03`/`04`/`05`/`08`/`09`；余 `M7B-06`/`07` 真站点人工登录 +
> `M7B-06b`）· **批 1 step 1~3 已落地**（协议词表 §6.1 / 命名管道 / Pydoll 驱动最小集，**生产路径未接线**）·
> 新增决议 **`MB-D0-8`**（登录态持久化四层 `L1`~`L4`，由 `M7B-09` 四轮实测支撑） —— §5 前置验证
> **全部通过前不进入实现**（同 P7-b 模式）；**执行偏离如实登记**：step 1~3 是**与站点无关的地基**
> （协议 / 管道 / 驱动），经用户审核后先行落地，`M7B-06`/`07` 的真站点人工登录仍未过
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
7. **⚠️ 防误判（2026-10-03 实测新增）**：**当前运行时仍是 WebView2** —— 批 1 step 1~6 交付的是**旁路新通道**
   （`web/pydoll_channel`），生产路径按 `MB-D1` / `B12-C1` **刻意零改动**；Pydoll 通道目前**只挂在
   `--pydoll-selftest` / `--pydoll-login` 两条 CLI 命令**上，**主程序 GUI / 网页节点未接线**
   （切换 = 批 3 `M7B-20`）。**逐入口实测归属表 + 10 秒分辨法见 §1.4**。

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
| `python/` 目录 | **不存在**（仓库无任何自研 `.py`）—— ⚠️ **已过期（2026-10-03）**：Python 包已落地并迁至 **`source/python/`**（见 §6.2「目录迁移」） |
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

### 1.4 运行时通道归属盘点（**2026-10-03 实测** · 防误判：现在跑的**还是 WebView2**）

> **一句话**：批 1 step 1~6 交付的是**旁路新通道**；生产路径按 `MB-D1` / `B12-C1` **刻意零改动** ——
> 主程序（GUI）里所有网页功能**仍然 100% 走 WebView2**。**看到 WebView2 不是 step 6 没生效**，这就是设计。

**实测方法**：`git grep -n -E 'pydoll_channel|web::channel' -- source/`（命中仅 `main.cpp` CLI 分发 + `api_probe.cpp` 断言）
＋ 逐入口查 `#include` 归属。

| 用户可见入口 | 现在**实际走** | 证据（实测） | 切换任务 |
|---|---|---|---|
| 参数面板「打开登录窗口（WebView2）」 | **WebView2**（独立线程 + 内嵌窗口 · profile `~/.brain-ai/webview2/`） | `ui/property_panel.cpp:14`（include）+ `:116`（独立线程）+ `:230` / `:319`（**按钮文案仍写死 WebView2**） | 批 5（§1.3 `property_panel.cpp` 5 处） |
| 「关闭登录窗口」/「按站点注销」 | **WebView2** | `property_panel.cpp:170-173`（`web::logout_site` —— ⚠️ 与 `web::channel::logout_site` **同名**，靠 namespace 区分） | 批 3 / 批 5 |
| 登录型节点 `ensure_session` | **WebView2** | `nodes/local_nodes.cpp:14`（include）+ `:545`（`web::ensure_session`） | 批 3（`M7B-20`） |
| 网页版**文字生成**（`adapter=dom`） | **WebView2 `ExecuteScript`** 注入 → 触发 → 轮询 | `ai/dom_web_client.cpp:12` + `:245`（`ensure_session`）/ `:256,262,297`（`web::run_script_sync`） | 批 3（`M7B-20`~`M7B-23`） |
| 网页版文字（`adapter=builtin:deepseek`） | **C++ `httplib` 直连**（PoW 借 WebView2 页面求解） | `ai/deepseek_web_client.cpp` + `web/webview_host.h:216` | 批 5（协议栈退役） |
| `--login-selftest` / `--web-probe` / `--web-chat` / `--run-selftest --web` | **WebView2** | `main.cpp` CLI 分发 | 批 3 / 批 5 |
| **`--pydoll-selftest`（新 · step 6）** | **Pydoll**（守护进程起 → `hello` → `ready{proto=1}` → `shutdown`；**不开浏览器**） | `main.cpp:1523-1524` → `web::channel::selftest()` | ✅ **已可用** |
| **`--pydoll-login <id>`（新 · step 6）** | **Pydoll**（**独立** Edge 窗口 + `~/.brain-ai/pydoll-profile/`） | `main.cpp:1529-1530` → `web::channel::pydoll_login()` | ✅ **已可用**（真站点人工冒烟 ⬜ `M7B-19`） |

**10 秒分辨「我现在跑的是哪条通道」**（⚠️ **Pydoll 底层也是 Edge/Chromium，窗口长相不可靠**）：

| 判别项 | **WebView2**（现状 · 生产） | **Pydoll**（新通道 · 仅 CLI） |
|---|---|---|
| 触发方式 | 主程序面板按钮 / 网页节点 / 旧 CLI | **只有** `--pydoll-selftest` / `--pydoll-login <id>` |
| 窗口形态 | **内嵌**在主程序窗口内的子控件 | **独立** Edge 窗口（独立任务栏图标） |
| profile 目录 | `~/.brain-ai/webview2/`（`paths.h:26`，批 5 退役） | `~/.brain-ai/pydoll-profile/`（`paths.h:28`） |
| 守护进程日志 | **无** | `~/.brain-ai/logs/pydoll_channel_daemon.log` |
| 面板按钮文案 | 写死「打开登录窗口（**WebView2**）」 | **尚无**对应按钮 |

**三条常见误判（均可排除）**：

1. 「跑主程序看到 WebView2 ⇒ step 6 白做了」——**错**：step 6 交付 = `--exec-selftest` **326 / 0** + 新通道 CLI 可用
   + 构建 0/0；生产路径按 `B12-C1`（§6.2）**只新增不替换**，**切换归批 3 `M7B-20`**（`M7B.md` §6.2 / §6 批 3 表）。
2. 「Pydoll 也是 Edge，看起来一样」——**比 profile 目录 / 独立任务栏窗口 / daemon 日志**，别比窗口长相。
3. 「改了 `site_ref.h` 就该少掉 WebView2 依赖」——**错**：搬迁只让**新通道**取站点描述时不必 include
   `webview_host.h`；生产调用方**仍需** `run_script_sync` 等真 WebView2 函数 ⇒ **仍 include**（`webview_host.h:29` 注释即此意）。

**为什么此刻不能直接切（硬缺口 · 实测）**：

| 缺口 | 实测位置 | 状态 |
|---|---|---|
| Python 侧只实现 **4 个**命令：`hello` / `shutdown` / `open_tab` / `login_state` | `python/brain_ai_browser/daemon.py:363,382,384` | `upload_image` / `send_prompt` / `read_answer` → **如实回「尚未实现（批 3）」**（`daemon.py:19-20,296`） |
| C++ 侧 `logout_site` / `run_script` / `current_tab_site` / `tab_on_site` 为**如实占位** | `web/pydoll_channel.h:39,42,46,47` | ⬜ 批 3 |
| CDP 增量 → **逐字流式** | `M7B-21` | 未做 ⇒ 现在切会丢逐字流式（违反 `I22` 流式降级必须显式） |
| 真站点人工登录门槛 | `M7B-19` | ⬜ 未执行（人工） |

⇒ **`M7B-20`（生产路径切换）的充分条件 = 批 3 的 `run_script` + `send_prompt` / `read_answer` + CDP 增量先落地。**
本盘点结论：**观察到的 WebView2 = 符合设计的当前状态，非缺陷、非回归。**

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

> **进度：7/9 已过**（`M7B-01` / `02` / `03` / `04` / `05` / `08` / `09`）—— 余
> `M7B-06` / `M7B-07`（**需真站点人工登录**）与加强项 `M7B-06b`。其中 `M7B-09` 是决议 `MB-D0-8` 新增的闸门，已由 V-a~V-d 四轮收口（见下方结论块）；
> `M7B-04` 由 step 3 合测收口（`--driver-selftest`）；其**生产形态已在 step 4 的 `daemon.py` 复验**
> （`--daemon-selftest` **22 / 0**，记录见 §6.2 step 4 块）。
>
> **与 `P7b-01`~`P7b-04` 合并执行**（一次验证两用；`M7B-05` 已覆盖 `P7b-04` 的图片注入前哨）——
> `P7b-01` / `P7b-04` **已随 `M7B-01` / `M7B-05` 一并通过**（状态已回填 [M7.md](M7.md) §12.1）。

| 编号 | 任务 | 验收 | 与 P7-b 关系 |
|---|---|---|---|
| `M7B-01` | Pydoll 起**独立 Edge / Chrome**（headful）、人工登录、profile 落 `~/.brain-ai/pydoll-profile`；**经 CDP 读 Cookie（含 HttpOnly）** | ✅ **已过（2026-09-28，机制层）**：窗口可见、profile 落盘、CDP 读回含 HttpOnly、重启后持久 Cookie 保留；**真·站点登录仍在 `M7B-06`** | `P7b-01` + 扩展（读 Cookie） |
| `M7B-02` | C++ ↔ Python **双向 IPC**（命名管道 + Win32 事件；JSON 行协议；非法行拒绝） | ✅ **已过（2026-10-03）**：`source/python/brain_ai_browser/pipe.py`（`ctypes` → `CreateNamedPipeW` / `ConnectNamedPipe` / `ReadFile` / `WriteFile`，overlapped + 精确超时）↔ `src/web/pipe_client.{h,cpp}`（读线程 + `id` 配对 + `CancelIoEx` 收尾）。**1 命令 + 1 事件**实测：`aiwrite.exe --pipe-selftest` **PASS / exit 0**（`hello` → `ready{proto=1, python=3.12.10, browser=edge@…}`；`shutdown` → `stage{close}`；守护进程退出码 **0**；丢弃非法帧 **0**）。⚠️ 实施要点 `P7`：C++ 句柄**必须** `FILE_FLAG_OVERLAPPED` | `P7b-02` / `P7b-07` |
| `M7B-03` | **CDP 增量流式三路线实测**：① `Fetch.takeResponseBodyAsStream` + `IO.read` ② `Network.eventSourceMessageReceived` ③ `Network.dataReceived` + `getResponseBody` | ✅ **已过（2026-09-28）**：主路线 = **①+`IO.read(size=128~256)`**（`size` 即推送粒度，实测 256 B/帧）；帧序样本见 §5 结论块与 `source/python/_probe/out/m7b03_cdp_stream.json` | 本文独有（`R2` 闸门） |
| `M7B-04` | asyncio 事件循环与管道监听共存 | ✅ **已过（2026-10-03 · step 3 合测）**：`python -m brain_ai_browser --driver-selftest` 组 B —— 管道伺服在**独立线程**（`PipeServer.accept`，实例先由主线程 `open()`），asyncio 主循环里**并发**跑「浏览器命令 burst」与 `asyncio.to_thread(pipe 往返)`：① 往返时刻落在命令窗口内**且返回时命令仍在跑**；② 窗口内心跳持续推进（**12 次 / ≈0.66 s**；headless 轮 14 次 / ≈0.78 s）⇒ 事件循环未被管道读阻塞（**真并发**判据，非「先后都成功」）。**生产形态（`daemon.py` 主循环 + 命令分发）在 §6.2 step 4 复验** | `P7b-03` |
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
> **`M7B-03` 实测结论（2026-09-28 · 本地页面自证 · 脚本 `source/python/_probe/m7b03_cdp_stream.py`）**
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
>    必须发 CDP **`Browser.close`**（实测：强杀 → 丢失 / 优雅 → 保留，脚本 `source/python/_probe/_diag_persist.py`）。
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
>    **全库读取一律 `Storage.getCookies`**（诊断：`source/python/_probe/_diag_cookie_scope.py`）。
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
> **`M7B-09` 关闭时序 / 持久化 / 接管 实测结论（2026-09-28 · `MB-D0-8` L1~L4 闸门 · `source/python/_probe/m7b09_*.py`）**
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

### 6.1 管道协议 v1 词表（`M7B-12` / `P7b-07` 施工图 · **批 1 开工前冻结**）

> **为什么先写词表**：`M7B-12` 的验收只有「离线桩可断言（`VB2-29`）」，而 **P7-b 的图片上传链路要靠本协议承载**
> （`P7b-10` 注入 / `P7b-11` 证据 / `P7b-12` 重试 / `P7b-16` 分派）。词表缺失 → C++ 与 Python **各自猜字段**，
> 且**无法先写离线断言**。故本表在批 1 前冻结；改动 = **升 `v`** + §15 变更记录（`I20` 只锁 **CLI** 形状，**不锁本文**）。

**帧格式（两侧一致）**：**UTF-8 JSON 一行一帧**（`\n` 结尾，帧内无裸换行）；每帧必带
`v`（整数，本文 = `1`）、`id`（字符串，**请求-响应配对**；事件帧用 `"-"`）、`kind`（`cmd` \| `evt` \| `err`）。
**非法行**（JSON 不合法 / 缺 `v` 或 `kind` / `v` 不识别）→ **丢弃 + 记日志 + 回 `err {bad_frame}`**，
**不得**让守护进程退出（`VB2-29`）。

```json
{"v":1,"kind":"cmd","id":"7","name":"upload_image","provider":"doubao-web","images":["C:\\Users\\me\\.brain-ai\\assets\\images\\ab12c3.png"],"timeout_ms":30000}
{"v":1,"kind":"evt","id":"7","name":"evidence","page":{"attach_nodes":1},"network":[{"method":"POST","url":"…","status":200}],"both":true}
```

**命令（C++ → Python）**

| 命令 | 必需字段 | 可选 | 语义 / 验收要点 |
|---|---|---|---|
| `hello` | — | — | 握手；回 `ready {proto, python, browser}` —— `M7B-13` / `M7B-14` 的「缺失 → 引导」由回包判定 |
| `open_tab` | `provider`（条目 id） | `url`（缺省取条目 `web.login_url`） | 复用已有 tab（`MB-D2`：每站点一个 tab）；**无站点回落**（`I14`） |
| `login_state` | `provider` | — | 回 `login {state, cookie_names, has_expires, http_only}`（判据 `I15′`）；**Cookie 值一律不进协议** |
| `upload_image` | `provider` · `images[]`（**本地绝对路径**） | `attach`（`auto` \| `file_input` \| `drop_zone` \| `paste_only` \| `none`；缺省取条目 `web.attach`）· `timeout_ms` | **只注入、不发提示词**（`I18` 的物理分离）；`aiwrite-asset:<sha1>` 令牌 → 路径的解析**在 C++ 侧**（`utils/asset_store`），Python 不实现令牌语义 |
| `send_prompt` | `provider` · `prompt` | `timeout_ms` | **前置条件**：本会话已有 `upload_image` 的**成功证据**，否则回 `err {no_upload_evidence}`（`I18` 的协议级拦截） |
| `read_answer` | `provider` | `timeout_ms` | 回 `answer_done {text, http_status?}`；增量走 `delta`（`M7B-21`） |
| `shutdown` | — | `grace_ms`（默认 **5000**） | `Browser.close` → **等进程退出**；**禁止 close 后立刻 kill**（`I23①`） |

**事件（Python → C++）**

| 事件 | 字段 | 语义 |
|---|---|---|
| `stage` | **`stage`**（`open` \| `login` \| `inject` \| `evidence` \| `send` \| `answer` \| `close`）· `ok` | 阶段推进（状态栏 / Console 日志用） |
| `evidence` | `page`（附件节点数 / 就绪态）· `network[]`（`method` / `url` / `status`）· `both` | **`P7b-11` 双证据的载体**；**具体判据值由 `P7b-05b` B2 段实测回填（当前未定）** |
| `delta` | `seq` · `text` | CDP 增量帧（`M7B-21`）；不可用 → `stage {answer}` + **显式标注「非流式（轮询）」**（`I22`） |
| `answer_done` | `text` · `http_status?` | 一轮完成 |
| `error` | `code` · `hint` | **必须可操作**（`I21`）：`no_python` / `no_browser` / `daemon_down` / `not_logged_in` / `no_upload_evidence` / `attach_unsupported` / `upload_timeout` / `send_timeout` / `bad_frame` |
| `ready` | `proto` · `python` · `browser` | `hello` 的回包 |

> ⚠️ **实现期订正（2026-10-03 · 批 1 `VB2-29` 暴露）**：`stage` 事件的**阶段标识键 = `stage`**
> （本表原写 `name`）—— 帧头 `name` 已被「事件名」占用，两者在**同层 JSON 里无法共存**。
> 两侧同改：Python `source/python/brain_ai_browser/protocol.py`（`EVENT_FIELDS`）与
> C++ `src/web/channel_frames.*`。（只改**载荷键名**，帧格式与其余词表不变。）

**错误码 → 行为映射**：`no_python` / `no_browser` / `daemon_down` = 依赖缺失（`I21`：**不换通道、不回落**）；
`not_logged_in` = 引导登录（`M7B-06b` 口径）；`attach_unsupported` / `upload_timeout` = 错误条两个**显式**按钮（`P7b-12`）。

**容量与超时（写死，防膨胀）**：单帧 ≤ **64 KiB**；`images[]` ≤ **8**（对齐 P7-a 图片缓存上限）；
**图片一律传路径、不传 base64**（会撞单帧上限且重复编码）。默认超时 = 注入 / 上传 **30 s** · 证据 **60 s** · 回答 **120 s**
（`timeout_ms` 可覆盖）；**超时 = 可操作错误，不假装完成**（`P7b-11`）。

> **与 `M7B-02` 的关系**：前置验证只测「**1 命令 + 1 事件**」（`hello` → `ready`）即算通道成立；
> 本词表是**施工图**，其余命令在批 1 / 批 2 内按表实现，逐条加离线断言（`VB2-29`）。

### 6.2 批 1–2 施工细化表（**冻结 · 2026-10-03** · 文件 / 函数 / 命令字段 / 断言编号级）

> **性质**：§6 回答「做什么」，本表回答「**拆到哪个文件 / 哪个函数 / 哪条断言**」。
> 与 §6 冲突时**以 §6 为准**；本表新增的「冻结口径」为**已定约束**，不再逐次讨论。
> 纪律不变：批 1–4 **只新增、不删改**（`MB-D1` 先建后拆；§9.3）。

**冻结口径（2026-10-03 · 用户确认）**

| 编号 | 口径 | 理由 |
|---|---|---|
| `B12-C1` | **批 2 只新增不替换**：`pydoll_channel` 与 `webview_host` **并存**；调用方（`ai/dom_web_client.cpp` / `nodes/local_nodes.cpp` / `ui/property_panel.cpp` / `main.cpp`）本批**零改动** | 新通道失败即弃、不触碰生产路径；**切换归批 3**（`M7B-20`） |
| `B12-C2` | `ai/provider_spec.cpp::allowed_web_fields()` **新增 `attach`**（+ 解析 + 缺省 `auto`），**运行期不消费**（消费归 `P7b-10`）；`source/assets/providers.json` 本批**不落值** | 让 `M7.md` `D11` 契约**可见可断言**；避免「填了值被判未知字段」 |
| `B12-C3` | `web::SessionStore` 的 `user_token` 字段**不删**（删除类动作 → 批 5）；批 2 只把**证据来源**换成 CDP Cookie 名单（`I15′`） | 守 §9.3「批 1–4 只新增不删改」 |

**批 0 基线（2026-10-03 本机复测 · 开工前置 · §9.3 要求「必须实测回填」）**

| 命令 | 实测 | 备注 |
|---|---|---|
| 构建 | **0 error / 0 warning** | ⚠️ `source\build.ps1` 在本机被执行策略拦截 → 复现用 `cmake --build --preset debug` |
| `api_probe --selftest` | **七组 PASS**（exit 0） | |
| `api_probe --exec-selftest` | **311 / 0**（exit 0） | **开工时为 309 / 1**（既有失败）→ 修复见下行「换机账」 |
| `api_probe --graph-selftest` | **110 / 0** | |
| `aiwrite --provider-selftest` / `--provider-dump` | **50 / 0** · **21 条**（official 9 / web 12） | |
| `aiwrite --run-selftest` | **PASS** ⚠️ **误记 —— step 7（2026-10-03）订正：本机实为 FAIL · exit 1**（缺本机凭据）｜当时跑的很可能是 `--run-selftest --web`（该变体确 PASS） | 环境依赖条（`api_key_ref = brain-ai/deepseek`）：**换机 / 重装后必须重配凭据** |
| docs 相对链接 | **296 / 0 broken** | 旧机的 `check_links.py` 在本机**不存在** → 用等价内联检查取证 |
| 环境 | Python **3.12.10** + `source/python/.venv` + `pydoll-python 2.27.0`（17 包）· **Edge 154.0.4258.48**（无 Chrome） | 见 §11.1 换机登记 |

> **换机账（本批修复，2026-10-03）**：开工时 `--exec-selftest` 为 **309 / 1**，唯一失败 =
> `M5-04 示例：E-02 加载校验通过`，根因是**示例文件写死旧机绝对路径**
> （`source/workflows/examples/E-02_图片转小说.json` 的 `path = F:/GameDao/Tools/AIwrite/source/assets/images/sample.png`）
> —— 而 `graph.cpp` 的 File 参数校验对非令牌值只做 `exists(原值)` ⇒ 换机必挂、且换机用户打开示例必报
> 「文件不存在」。**修复（4 处）**：① 示例图片路径改**仓库相对路径** `assets/images/flamingo.png`（+ 描述同步）；
> ② `api_probe.cpp` **新增**「示例图片路径**可移植**（不含盘符 / 反斜杠）」断言，并在校验前把路径**代入本机示例图**
> （相对路径 / 令牌无法跨机解析，不代入会把「示例可移植」误判成「示例损坏」）；
> ③ `main.cpp` 的 `--vlm-selftest` 默认图 `sample.png` → `flamingo.png`（`sample.png` 已在工作区删除）；
> ④ `docs/节点编辑器使用说明.md` §10 同步。→ 断言总数 **310 → 311**，**全绿**。
> ⚠️ **`source/assets/images/sample.png` 的删除**（工作区未暂存）与 `flamingo.png`（未入库）属**上一批未提交变更**，
> 本批**只做上述兼容性修复，不改其取舍**；两者最终去留待提交时一并拍板。

> **目录迁移（2026-10-03 · step 4 之后 · 纯搬迁 · 零逻辑变更）**：Python **运行代码**由仓库根 `python/` 迁到 **`source/python/`**
> （包 `brain_ai_browser/` + 探针 `_probe/`（70 文件 / 45 个物证 JSON，`git mv` 保历史）+ `requirements.txt` + `.venv/`）；
> **运行期数据仍留 `%USERPROFILE%\.brain-ai`**（`logs` / `pydoll-profile` / step 5 的 `session/`）—— **代码入库、数据不入库**。
>
> * **C++ 侧同步 1 行**（`src/main.cpp`）：`--pipe-selftest` 的包目录候选 `AIWRITE_SOURCE_DIR.parent_path() / "python"` → **`AIWRITE_SOURCE_DIR / "python"`**
>   （= `<仓库根>/source/python`）；`.venv` 候选随 `package_dir` 自动正确，子进程 `work_dir` 同步。注释 3 处（`main.cpp` / `web/channel_frames.h` / `web/pipe_client.h`）同改。
> * **口径同步**：`.gitignore`（`/python/.venv/` → `/source/python/.venv/` 等 4 条 + 段注释）· `runtime.py` 的 `PYDOLL_HINT`（**用户可见引导**）·
>   `driver.py` 文档串 · `requirements.txt` 头部 · docs **8 文件**（本文件 / `CHANGELOG.md` / `M7.md` / `M_patchAB_rest.md` / `网页版协议实测记录.md` /
>   `source/README.md` / `DevPlan.todo` / `providers.json`）。**历史物证文件（`_probe/out/*.json|*.err`）保留原路径**（证据不可篡改）。
> * **验证（逐条与迁移前一致 · 本机）**：`--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · `--driver-selftest --headless` **22 / 0** ·
>   `--daemon-selftest --headless` **22 / 0**（子进程 `cwd = parents[1]` 自动跟随的实测）· `aiwrite.exe --pipe-selftest` **PASS** ·
>   `api_probe` **七组 / 320 / 110** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS** ·
>   构建 **0 error / 0 warning** · docs 断链 **298 / 0**。
>
> **以后所有命令口径**：自检/探针一律 `cd source\python` → `.venv\Scripts\python.exe -m brain_ai_browser …`；数据一律在 `~/.brain-ai/…`。


**批 1 文件表（`source/python/brain_ai_browser/` · 新包 · 不新增第三方依赖）**

| 文件 | 职责 | 关键 API | 验收 |
|---|---|---|---|
| `__init__.py` | 版本常量 | `PROTO_VERSION = 1` · `PACKAGE_VERSION` | import 零副作用 |
| `__main__.py` | CLI 入口 | `main(argv)` → `daemon.serve()` / `--selftest` / `--stub-selftest` | 两开关 exit 0 |
| `protocol.py` | **§6.1 词表唯一实现** | `parse_line(str) -> Frame \| BadFrame` · `encode_cmd/evt/err` · `CMD_SPEC`（7 命令）· `EVT_SPEC`（6 事件）· `ErrorCode`（含 `bad_frame` / `no_upload_evidence`）· 上限（`MAX_FRAME=64KiB` · `MAX_IMAGES=8` · 超时 30/60/120 s） | **`VB2-29`**（纯函数、离线） |
| `pipe.py` | 命名管道服务端（`ctypes` → `CreateNamedPipe` / `ConnectNamedPipe` / `ReadFile` / `WriteFile`） | `PipeServer(pipe_name)`: `accept` / `read_frame` / `write_frame` / `close` | **`M7B-02`** 冒烟 |
| `driver.py` | Pydoll 封装（复用探针已验证机制） | `start(profile, window_size)` · `attach_if_running` · `tab_for(provider, url)` · `cookies_all()`（= `Storage.getCookies`）· `cookies_for_domain(suffix)` · `execute_script` · `type_humanized` · `press_key`（`Key` 枚举）· `set_file_input_files` / `expect_file_chooser` · `stream_deltas()` · `close_wait(5s)` | **`M7B-04`** 合测 |
| `session.py` | ✅ **已落地（step 5 · 2026-10-03）**：**L2 DPAPI 加密快照**（落 `~/.brain-ai/session/cookies.dat`；定时刷新 5 s + 登录即写 + `shutdown` **关闭前**再写；启动**回灌** `Storage.setCookies`；**无 DPAPI ⇒ 不快照、绝不写明文**） | `SnapshotStore.save()` / `.load()` / `.clear()` · `to_cdp_params()`（白名单 + 会期语义）· `scoped()` / `belongs_to()`（**父域方向**）· `scan_plaintext()` · `dpapi_available()` | ✅ **`VB2-39①~⑬`**（`--session-selftest` **22 / 0**：离线 + 真机「存 → 弃 → 回灌」） |
| `daemon.py` | 守护进程主循环（命令分发 · 心跳 · 崩溃检测 · L4 埋点 · **吞 `BrowserNotRunning`**） | `Daemon.serve()` | **`VB2-38`** 关闭协议桩 |
| `browsers.py` | `M7B-14` 浏览器检测（**标准安装路径先 Chrome 后 Edge**；皆无仍返回 Chrome 让库报真错） | `detect_chrome` / `detect_edge` / `browser_class` / `env_proof` | 两条路径均可起浏览器 |
| `runtime.py` | `M7B-13` 运行时检测 | `check_runtime() -> {python, pydoll, ok, hint}` | 缺失 → 引导，**不阻塞其他功能** |
| `redact.py` | 日志脱敏（`~/.brain-ai/logs/browser.log`） | `mask` / `redact_cookies` | 无 Cookie 明文 |

**批 1 命令边界（§6.1 词表逐条）**

| 命令 | 处理入口 | 批 1 状态 |
|---|---|---|
| `hello` | `daemon.handle_frame` → `ready {proto, python, browser}`（**缺失时追加一条 `error` 事件**带引导） | ✅ 实现（**`M7B-02` 验收点**；step 4 加引导） |
| `open_tab` | `daemon._cmd_open_tab`：**按需** `BrowserAccess.ensure()` → `stage{open, ok}`（**带请求 id = 完成回包**，`MB-Q10`）；缺 `url` → 可操作错误（`I14`：条目 `web.login_url` 由 C++ 解析） | ✅ 实现（step 4） |
| `login_state` | `daemon._cmd_login_state` → `login {state, cookie_names, has_expires, http_only}`（**Cookie 值不进协议**；可选 `cookie_names` / `domain_suffix`，未给 → `state=unknown`，判定不越权自造 → `MB-Q9`/`MB-Q10`） | ✅ 实现（step 4） |
| `shutdown` | `daemon.BrowserAccess.close(grace_ms)` → `Browser.close` → 等进程退出 → **超时兜底强杀 + 必留 warn** → `close_verdict()`（`I23①`/`VB2-38`） | ✅ 实现（step 4） |
| `upload_image` | `on_upload_image` | ⬜ 桩（`attach_unsupported`；令牌解析**在 C++**） |
| `send_prompt` | `on_send_prompt`（前置 `no_upload_evidence`；`I18`） | ⬜ 桩 |
| `read_answer` | `on_read_answer` → `delta` / `answer_done` | ⬜ 桩（CDP 增量归批 3 `M7B-21`） |

**批 2 新增文件（`source/src/web/` · 与 `webview_host` **并存**，见 `B12-C1`）**

| 文件 | 对外接口 | 顶替 / 新增 |
|---|---|---|
| `pipe_client.{h,cpp}` | `class PipeClient { bool connect(std::string pipe_name, int timeout_ms, std::string* err); bool call(std::string name, const json& req, json* resp, int timeout_ms); void on_event(EventCallback); void close(); }` —— 阻塞仅在**执行器工作线程**（`M7.md` `Q4`） | ✅ **已落地**（`M7B-15` · step 2 + **step 6 判据同步**：`stage` 带 id = 完成回包，关闭 `MB-Q10`） |
| `pydoll_channel.{h,cpp}` | `namespace web::channel` —— 与 `webview_host` **同构的自由函数集**（下方接口映射表） | ✅ **已落地**（`M7B-16` · step 6）：`selftest` / `pydoll_login` 可用；其余如实回「批 3」；**本批不接线** |
| `channel_frames.{h,cpp}` | 纯函数 `parse_frame(string_view)` · `make_cmd(...)` · `delta_text_of_frame(...)` | ✅ **已落地**（`M7B-12` 的 C++ 侧；`VB2-29` / `VB2-32`） |
| `session_snapshot.{h,cpp}` | 快照路径 / 触发时机 / 状态展示（快照本体在 Python 侧） | ✅ **已落地**（`MB-D0-8` L2 · step 6）：**只读元数据**（永不读内容 ⇒「零明文」天然成立） |
| `site_ref.h` | `SiteRef` 值类型 + 既有**纯函数整体搬迁**（`login_request_of` / `interactive_login_request` / `probe_login_request` / `boot_login_request` / `plan_session_boot` / `login_request_site`） | ✅ **已搬迁**（step 6）· **零语义变化**（`webview_host.h` 改为 include 它；`--web-probe` / UI / `dom_web_client` 调用点不动，守 `I2`） |

**批 2 接口映射表（调用方零改动的依据）**

| 现有声明（`webview_host.h`） | 现有调用方 | 新接口（`pydoll_channel.h`） | 语义变化 |
|---|---|---|---|
| `run_script_sync(...)` **:207** | `ai/dom_web_client.cpp` ×13 | `channel::run_script(const SiteRef&, const std::string& js, int, std::string*, std::string*)` | 传输换命名管道；页面上下文由守护进程持有 |
| `ensure_session(...)` **:230** | `dom_web_client` · `local_nodes` · `main.cpp` | `channel::ensure_session(const SiteRef&, int, std::string*)` | 判据改 **`I15′`：CDP Cookie 名单**（去 `userToken`） |
| `logout_site(...)` **:233** | `ui/property_panel.cpp` | `channel::logout_site(const SiteRef&, int, std::string*)` | 实现换 `Storage.clearDataForOrigin` |
| `current_window_site()` **:225** / `window_on_site()` **:227** | UI | `channel::current_tab_site()` / `channel::tab_on_site()` | tab 归属（浏览器在守护进程内） |
| `LoginWindow` 类 / `login_window()` / `stop_login_window()` | `property_panel.cpp` ×8 · `main.cpp` | `channel::LoginWindow`（**同名同形**，内部改管道 + Pydoll 真浏览器） | 承载方式变化 |
| `solve_pow_via_page()` **:216** | `ai/deepseek_web_client.cpp` | **本批原地保留** | 批 3/5 才退役 |
| `selftest()` / `protocol_probe()` / `protocol_probe_for_provider()` | `main.cpp` | `channel::selftest()` + 新 `channel::pydoll_login(id, timeout)` | `--web-probe` 废弃属批 5 |

**`SiteRef` 字段**（由 `ai::ProviderWebSpec` + `web::login_request_of()` 派生）：
`provider_id` · `site`(origin) · `login_url` · `cookie_names[]` · `input_selector[]` · `send{kind,value}` ·
`answer_selector[]` · `done_when{}` · `poll_ms` / `max_polls` · **`attach`**（本批新增，缺省 `auto`，见 `B12-C2`）。

**批 2 改动清单（逐文件）**

| 文件 | 动作 | 影响生产路径？ |
|---|---|---|
| `src/web/pipe_client.{h,cpp}` · `pydoll_channel.{h,cpp}` · `channel_frames.{h,cpp}` · `session_snapshot.{h,cpp}` · `site_ref.h` | ✅ **已新增**（step 6 全部落地） | ❌ |
| `src/web/session_store.{h,cpp}` | ✅ 注释 + **证据来源**改 CDP（API 全保留；`user_token` 字段不动，`B12-C3`） | ❌ |
| `src/utils/paths.{h,cpp}` | ✅ **只新增** `pydoll_profile()` / `session_snapshot_dir()`；`webview2_profile()` 保留到批 5 | ❌ |
| `src/ai/provider_spec.{h,cpp}` | ✅ `allowed_web_fields()` +`attach` + 解析（**不消费**） | ❌ |
| `CMakeLists.txt` | ✅ `aiwrite_core` 增 3 组源文件（`pydoll_channel` / `session_snapshot`；`site_ref.h` 仅头文件）；**不改** WebView2 的 21 处；新增 `aiwrite_copy_python()`（`copy_if_different` 风格 + `file(GLOB CONFIGURE_DEPENDS)`，**`.venv` 排除**） | ❌ |
| `tools/api_probe.cpp` | ✅ **只新增**断言块（`VB2-40①~⑤` + `VB2-39⑥`），**不删任何旧断言**（320 → **326**） | ❌ |
| `source/python/brain_ai_browser/*` | ✅ 批 1 产物纳入 `aiwrite_copy_python()`（实测 `<exe>/python/brain_ai_browser/` 10 个 `.py`）+ `VB2-30` 离线桩（守护进程未起 → 可操作文案） | ❌ |

**批 2 断言归属（§9.2 对照）**

| 断言 | 落点 |
|---|---|
| `VB2-29` 协议离线桩（`v` / `kind` / `id` / 未知命令 / 非法行 → `err {bad_frame}` 不退出） | Python `protocol.py` + C++ `channel_frames.*` |
| `VB2-32` CDP 帧 → 增量文本（复用 `delta_text_of` 形态 A/B） | `channel_frames.*`（纯函数） |
| `VB2-31` `I22` 非流式**显式标注** | 帧解析纯函数 |
| `VB2-25′` **不注入任何站点内部端点**（全局） | `api_probe --exec-selftest` 新增块 |
| `VB2-30` `I21` 依赖缺失 → 报错 + 引导，**不发起 HTTP / 不开浏览器** | 纯逻辑 |
| `VB2-36` `web_session_failure_hint()` 在**新数据源**下判定一致 | 纯函数 |
| `VB2-38` 关闭协议桩（缺「`Browser.close` + 等进程退出」即失败；`BrowserNotRunning` 归正常收尾） | Python `daemon.on_shutdown` 契约 |
| `VB2-39` 快照无明文（DPAPI 往返 + 明文扫描） | Python `session.py` ✅ **已生效（step 5 · 2026-10-03）**：`--session-selftest` **22 / 0**（离线 8 条 + 真机 5 条：`VB2-39①~⑬`）；**C++ 半（`VB2-39⑥`）亦已生效（step 6）**：`session_snapshot.inspect()` / `summary()` **只碰元数据**（存在 / 字节 / 时间），永不读内容 |
| `VB2-40`（新 · step 6）**站点描述搬迁零语义 / `attach` 携带 / 快照常量同值 / 新通道如实报错** | C++ 纯逻辑（`api_probe --exec-selftest` · `site_ref.h` + `session_snapshot` + `pydoll_channel`）—— 不启进程、不读密文 |

**`M7B-19` 中间检查点（硬门槛 · 批 2 出口）**

| 命令 | 期望 |
|---|---|
| 构建 | **0 error / 0 warning** |
| `api_probe --selftest` | 七组 PASS |
| `api_probe --exec-selftest` | **≥ 311 / 0**（本批只加不删 → **只升不降**） |
| `api_probe --graph-selftest` | **110 / 0** |
| `aiwrite --provider-selftest` / `--provider-dump` | **50 / 0** · **21 条** |
| `aiwrite --run-selftest` / `--run-selftest --web` | PASS / PASS —— **此时仍走 WebView2**，数字须与批 0 一致（「先建后拆」未动生产路径的证据）。**step 7 实测**：official **FAIL · exit 1**（缺本机凭据 · **环境依赖**）· `--web` **PASS** ✅ |
| docs 相对链接 | **0 broken** |
| 新通道冒烟（人工） | `aiwrite --pydoll-login <id>` 起 Edge 并把 profile 落 `~/.brain-ai/pydoll-profile` |

**回滚点**：批 2 结束打标签 **`m7b-batch2`**（与 `pre-m7b` 并列，§12）；批 2 新通道**不进入生产路径**，冒烟失败即弃、零影响。

**批 1–2 特有风险**

| # | 风险 | 缓解 |
|---|---|---|
| `P1` | 管道名冲突 / 多实例 | `\\.\pipe\aiwrite-browser-<pid>`；`hello` 回包带 `pid` |
| `P2` | 守护进程僵尸 / profile 被占 | `MB-D0-8` L1/L4 + 启动前探端口/锁（`M7B-09` V-c 教训：**禁止以强杀当启动手段**） |
| `P3` | `Storage.getCookies` 读全库 → 跨域污染判据 | **一律按 `domain_suffix` 过滤**（`P7b-05b` B0 误报的直接教训） |
| `P4` | 视口尺寸决定 DOM 形态 | 启动参数写死 `--window-size=1440,1000`（B1 教训；与 `M7B-28` 同源） |
| `P5` | 新断言把旧数字压低 | `MB-D1`：批 1–4 只加不删；**任何下降 = 真回归** |
| `P6` | `source/python/` 打包路径 / `.venv` 误入库 | 沿用 `aiwrite_copy_assets` 的 `copy_if_different` 模式；`.venv` 由 `.gitignore` 排除 |
| `P7` | **同步句柄上的并发 I/O 被 I/O 管理器串行化**（step 2 实测踩到） | C++ 侧句柄**必须** `FILE_FLAG_OVERLAPPED`；读/写各一事件；写加 **30 s 上限**（对端不读**不无限等**）；`close()` 用 `CancelIoEx`。表象与误判路径见上方「实现期订正 ①」 |

**批 1–2 开工顺序（最小可验证切片）**

```
step 1  protocol.py + channel_frames.{h,cpp} + 两侧离线桩        → VB2-29 绿
step 2  pipe.py + pipe_client.{h,cpp} → hello → ready            → M7B-02 冒烟
step 3  driver.py 最小集（start / cookies_all / execute_script / close_wait） → M7B-04 合测
step 4  daemon.py + browsers.py + runtime.py + redact.py          → M7B-10~14 收口
step 5  session.py（L2 DPAPI 快照）+ daemon 三触发点（定时 / 登录即写 / 关闭前）
        + --session-selftest                                        → VB2-39 绿（✅ 已过）
step 6  C++ 侧接线：site_ref.h 搬迁 + session_snapshot + pydoll_channel
        + PipeClient 判据同步（stage 带 id）+ aiwrite_copy_python     → M7B-15~17（✅ 已过）
step 7  生产路径切换（dom_web_client → 新通道）+ 诊断换代（--web-dom-dump / --pydoll-login）
        + 跑 M7B-19 全基线 + 贴数字 + 打标签 m7b-batch2              → M7B-18 / M7B-19   ◀ 已开（基线已跑 · 标签待提交）
```

> **进度（2026-10-03）**：**step 1 / 2 / 3 / 4 / 5 / 6 ✅ 完成**（step 2 = `M7B-02` 冒烟；step 3 = 驱动最小集 + `M7B-04` 合测；step 4 = 守护进程主循环 + `VB2-38`；**step 5 = L2 加密快照 `session.py` + `VB2-39`（`--session-selftest` **22 / 0**）**；**step 6 = C++ 侧接线（`site_ref.h` / `session_snapshot` / `pydoll_channel` + `PipeClient` 判据同步）→ `M7B-15~17`（`--exec-selftest` **320 → 326 / 0**）**）；**step 7 = 已开** —— `M7B-19` 全基线**已跑（12 / 13 绿 · 1 项环境依赖偏差）**、`M7B-18` **受阻已登记**（见下方记录块）。余：`M7B-18` 三路径待拍板；生产路径切换归批 3（`M7B-20`）。
>
> * **step 1** —— 新增 `source/python/brain_ai_browser/`（`__init__.py` / `protocol.py` / `__main__.py`，
>   **纯标准库**）+ `src/web/channel_frames.{h,cpp}`（纯函数、无 IO、不抛异常；已挂 `aiwrite_core`）
>   + `tools/api_probe.cpp` 新增离线断言 **`VB2-29①~⑦` / `VB2-32①②`**（9 条；**不删任何旧断言**）。
> * **step 2（`M7B-02` 冒烟：1 命令 + 1 事件）** —— 新增 `source/python/brain_ai_browser/pipe.py`
>   （`ctypes` 直调 `CreateNamedPipeW` / `ConnectNamedPipe` / `ReadFile` / `WriteFile`，
>   **overlapped + 精确超时 + `CancelIoEx` 收尾**）+ `src/web/pipe_client.{h,cpp}`
>   （读线程 + `id` 配对 + 事件回调；**不得在 UI 线程调 `call()`**）+ `__main__.py` 的
>   `--serve`（最小伺服）/ `--pipe-selftest`（进程内 loopback）+ `main.cpp` 的 **`--pipe-selftest`**
>   （起守护进程 → `hello` → 校验 `ready` → `shutdown` → **等进程退出**）。
>
> **实测数字（本机 · 2026-10-03）**：构建 **0 error / 0 warning** · Python `--selftest` **7 / 0** ·
> `--pipe-selftest`（loopback）**13 / 0**（venv 3.12.10 与系统 3.14.3 均过）·
> `aiwrite.exe --pipe-selftest` **PASS / exit 0**（`ready{proto=1, python=3.12.10, browser=edge@…}` +
> `stage{close}` + 守护进程退出码 **0** + 丢弃非法帧 **0**）· 守护进程日志逐帧物证
> （**收到 2 帧**：`hello` 44 B / `shutdown` 62 B）· `api_probe --exec-selftest` **320 / 0** ·
> `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** ·
> `--run-selftest` **PASS**（生产路径未动，`MB-D1` 先建后拆的证据）· 收尾**无残留进程**。
>
> **实现期订正 ①（step 2 踩到 · 已落代码注释）**：C++ 侧管道句柄**必须**以 `FILE_FLAG_OVERLAPPED`
> 打开 —— 同步（非 overlapped）句柄上的**并发 I/O 会被 I/O 管理器串行化**：读线程一挂起（阻塞
> `ReadFile`），写操作就被排到后面干等。实测症状 = **「客户端写阻塞 20 s（＝服务端 idle 超时）后
> `ERROR_NO_DATA`」而服务端同轮收到 0 帧**（两端互等）。改 overlapped（读/写各一事件）后立刻互通；
> 并给**写加 30 s 上限**（对端不读**不无限等**）。
> **实现期订正 ②**：`main.cpp` 需显式 `#include <windows.h>`（此前只有 WebView2 相关文件引它）；
> `--pipe-selftest` 的 `--timeout` 口径是**秒**（与 `--login-selftest` 一致），函数内部换算毫秒。
> **实现期订正 ③（词表边界）**：`--serve` 是**最小伺服**，对「词表内但未实现」的命令回
> `err{daemon_down, hint=…尚未实现}` 并保持存活 —— 词表**缺「命令未实现」错误码**，已登记为
> **`MB-Q7`**（§14.2）待拍板，**本批不动词表**（动词表 = 升 `v`）。
>
> **step 3 ✅ 完成（2026-10-03）—— Pydoll 驱动最小集 + `M7B-04` 合测 + `VB2-30`**
>
> * **新增（Python 侧；标准库 + pydoll **惰性 import**）**：
>   ① `browsers.py`（`M7B-14`：标准安装路径**先 Chrome 后 Edge**、`detect_browser` / `browser_class` /
>   `env_proof`；跨语言常量 `PROFILE` = `~/.brain-ai/pydoll-profile`、`WINDOW_SIZE` = `1440×1000`）；
>   ② `runtime.py`（`M7B-13`：`check_runtime() -> {python, pydoll, ok, hint}` + 可操作引导 +
>   `DependencyError`；`load_pydoll()` = **包内唯一 pydoll 入口** ⇒ 自检可打桩）；
>   ③ `driver.py`（**最小集**：`start` / `tab_for` / `new_tab` / `cookies_all` /
>   `cookies_for_domain`（`P3` 按域过滤）/ `execute_script`（**单点解包两层 `result`**）/
>   `close_wait`（`Browser.close` → 等退出；**超时只 warn、不提供强杀**，`MB-D0-8` L1/L3）；
>   `_launch()` = **唯一启动点** ⇒ 「依赖缺失时一次都没调启动器」可断言）。
> * **新增（`__main__.py`）**：`--driver-selftest [--headless] [--timeout <秒>]` = **组 A**（离线 `VB2-30`）
>   + **组 B**（真机 + `M7B-04`）；`--headless` / `--timeout` **仅在** `--driver-selftest` 下合法，
>   否则 **exit 2**（**不静默忽略**）。
> * **`VB2-30`（`I21`）双证据**：① **桩物证** = 组 A 打桩两条路径（`no_python` / `no_browser`）→ 可操作引导 +
>   **启动器调用数 = 0** + **新建 socket 数 = 0**（「不开浏览器 / 不发 HTTP」的机器判据）；
>   ② **真机物证** = **系统 Python 3.14.3（无 pydoll）实跑**同两条断言全绿 + 组 B **显式 SKIP** + exit 0。
> * **`M7B-04` 判据（真并发）**：管道伺服在**独立线程**（实例先由主线程 `open()`），asyncio 主循环里并发跑
>   「浏览器命令 burst」与 `asyncio.to_thread(pipe 往返)` ⇒ ① 往返时刻落在命令窗口内**且返回时命令仍在跑**；
>   ② 窗口内心跳持续推进（**12 次 / ≈0.66 s**；headless 轮 14 次 / ≈0.78 s）⇒ 事件循环未被管道读阻塞。
> * **实现期订正 ①（`M7B-04` 合测）**：初版把管道伺服线程放在组 B **开头**、`accept` 超时 15 s ⇒ 被
>   「起浏览器 6.6 s + 4 次 PowerShell 取证」挤爆，`accept` 超时**作废实例** → 客户端 `CreateFile` 报
>   `FILE_NOT_FOUND` 重试到超时。**表象 = 双方都说「对方没听见」**（与 step 2 的 `P7` 同类误判陷阱，
>   只是成因是**时序**而非句柄模式）。修法 = 实例先由主线程 `open()` + 伺服线程紧贴合测窗口启动
>   （`accept` 30 s 兜底），并把「**往返返回时命令仍在跑**」写进判据。
> * **实测数字（本机 · 2026-10-03）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll`
>   copy，与本轮无关）· `--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · **`--driver-selftest`
>   （有头窗口 · 正式轮）22 / 0**（起 Edge pid=10448 · 启动 6.56 s · `execute_script` → **42** ·
>   `cookies_all` 为 list · 真实命令行含 `--window-size` / `--user-data-dir` · `close_wait` **3.83 s /
>   exit 0 / 无强杀** · 残留 **0** · 管道 `served=1`）· `--driver-selftest --headless` **22 / 0**
>   （判据不依赖 headless）· **系统 Python 3.14.3（无 pydoll）`--driver-selftest` 11 / 0** ·
>   参数错误路径 4 条均 **exit 2** · `--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** ·
>   `--provider-selftest` **50 / 0** · `--provider-dump` **21 条**（official 9 / web 12）·
>   `--run-selftest` **PASS** · `aiwrite.exe --pipe-selftest` **PASS**（step 2 回归）·
>   docs 相对链接 **297 / 0** · 收尾**无残留进程**。
> * **新开口项 `MB-Q8`**（§14.2）：词表缺「依赖细分」码 ——「pydoll 缺失」现沿用 `no_python`
>   （语义偏「Python 运行时缺失」）+ `hint` 显式说明**缺的是哪个依赖**；**本批不动词表**。
> * **边界**：**生产路径零改动**（`webview_host` 原样在跑、新通道**未接线**）· `source/assets/providers.json`
>   未改 · `session.py` / `daemon.py` / `redact.py` 未落地 ⇒ **`VB2-38` / `VB2-39` 尚未生效**（**后续**：
>   `daemon.py` + `redact.py` 于 **step 4** 落地、`session.py` 于 **step 5** 落地）·
>   `attach_if_running` / 上传 / 流式 / 打字留 step 4 / 批 3。

> **step 4 ✅ 完成（2026-10-03）—— 守护进程主循环（`M7B-10`/`M7B-11`/`M7B-13`/`M7B-14` 收口 + `VB2-38`）**
>
> * **新增 `daemon.py`（唯一主循环）**：**监听线程**（`accept` + `read_line`，阻塞 I/O 全在此）→
>   `call_soon_threadsafe` 投 `asyncio.Queue` → **主线程 asyncio** 跑 `handle_frame`（Pydoll 用同一循环）；
>   写帧一律 `asyncio.to_thread`；收尾 = 关管道（放监听线程出来）→ `join` → 补关浏览器（异常路径）。
>   浏览器**按需启动** ⇒ `hello` / `shutdown` 不需要浏览器（`aiwrite.exe --pipe-selftest` 仍秒级）。
>   `--serve` 由「最小伺服」**整体迁入** `daemon.serve()`（`__main__` 只留 CLI 与三层自检）。
> * **新增 `redact.py`**：日志脱敏（`~/.brain-ai/logs/browser.log`，一行一条 JSON）；`log_event()` =
>   **唯一写日志入口** ⇒「无明文」可被扫描断言（Cookie 值 / 令牌一律打码，名字保留可诊断）。
> * **命令边界落定**（§6.2 表）：`hello` → `ready`（依赖缺失时**追加一条 `error` 事件**带引导）；
>   `open_tab` → 按需起浏览器 + `stage{open, ok}`；`login_state` → `login{state, cookie_names,
>   has_expires, http_only}`（**值不进协议**；`state` 需调用方给可选 `cookie_names`，未给 = `unknown`，
>   判定不越权自造 —— `I14`）；`shutdown` → `stage{close}` → `Browser.close` → **等进程退出**
>   （5 s，超时才兜底强杀 + **必须留 warn**）；`upload_image` / `send_prompt` / `read_answer` 仍
>   `err{daemon_down, hint=尚未实现}`（`MB-Q7` 口径不变）。
> * **`M7B-11` 关键行为**：**用户手动关窗 = 可恢复状态** —— `BrowserNotRunning` / 进程已死**被吞**，
>   记 L4（`browser_selfheal`）后**自愈重启**（实测：强杀浏览器 → 下一条命令仍成功、pid 换新、
>   stderr **零回溯**）；L4 心跳（10 s）、崩溃检测、`daemon-state.json` 留痕（未干净退出 → 下次启动
>   打印 `⚠️ 上次未干净退出（登录态可能已回滚）`，`MB-D0-8` L4）。
> * **`VB2-38`（`I23①`）落地**：`daemon.close_verdict()` **纯逻辑 7 条判据**（缺 `Browser.close` → 失败 /
>   `BrowserNotRunning` → 归正常收尾 / 超时**无 warn** → 失败 / 超时 + warn（±兜底强杀）→ 通过 /
>   证据自相矛盾 → 失败）+ 端到端物证（`verdict_ok=true`、`waited_s=2.16 s`、**未强杀**）。
> * **实现期订正 ①（口径）**：`stage` 帧**带请求 id** = 该命令的**完成回包**；`id="-"` 的 `stage` 才是
>   纯进度事件 ⇒ **C++ 侧 `PipeClient` 的进度事件判据需同步**（**step 6**；否则 `open_tab` 的 `call()`
>   等不到回包）。已登记为 `MB-Q10`。
> * **实测数字（本机 · 2026-10-03）**：`--daemon-selftest`（**有头窗口 · 正式轮**）**22 / 0**
>   （合法帧 4 · 丢弃非法帧 0 · 浏览器启动 **2 次**（含自愈 **1 次**）· 守护进程退出码 **0** ·
>   `close_wait` **2.16 s / 未强杀** · 残留 **0** · stderr 无回溯）；`--driver-selftest` **22 / 0**（回归）·
>   `--selftest` **7 / 0** · `--pipe-selftest` **13 / 0**（**系统 Python 3.14.3 无 pydoll → 14 / 0**，
>   多出的 1 条 = `ready` 后**追加 error 事件**的真机证据）· `aiwrite.exe --pipe-selftest` **PASS**
>   （**新主循环**在 C++ 侧端到端可用；`--serve` 换真身**未改一行 C++**）· `--exec-selftest` **320 / 0** ·
>   `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** ·
>   `--run-selftest` **PASS** · 收尾**无残留进程**。
> * **新开口项**：**`MB-Q9`**（词表缺 `login` 事件名 —— §6.1 表 2 未列，命令边界表已引用）·
>   **`MB-Q10`**（`login_state` 的可选字段 `cookie_names` / `domain_suffix` + 「`stage` 带 id = 完成回包」
>   的口径未入表；本批**不动词表**，先按代码注释与本块口径执行）。
> * **边界**：**生产路径零改动** · C++ 侧关闭协议同步 / 诊断换代（`--pydoll-login`）/ 自启动与
>   「一键重登」UI 归属 step 6；`session.py`（L2 快照）在 **step 5 已落地** ⇒ **`VB2-39` Python 侧生效**。

> **批 1 step 5 记录块 —— L2 加密快照 `session.py` + `VB2-39` Python 侧生效（2026-10-03）**
>
> * **新增 `session.py`**（`source/python/brain_ai_browser/`）：**DPAPI 加密快照**（`ctypes` 直调
>   `crypt32`，`CRYPTPROTECT_UI_FORBIDDEN` ⇒ 不弹 UI、**不引第三方依赖**）· 纯函数 `host_of()` /
>   `belongs_to()`（**父域方向**）/ `scoped()` / `to_cdp_params()`（字段白名单 + 会期语义）/
>   `usable_cookies()` / `scan_plaintext()` / `summarize()` · `SnapshotStore.save/load/clear`
>   （**原子写** `.tmp` + `os.replace`；**内容没变不重写**，比较**明文摘要** —— DPAPI 密文每次都不同）；
>   数据落 **`~/.brain-ai/session/cookies.dat`**（**代码入库、数据不入库**；`BRAIN_AI_SESSION_DIR`
>   仅作测试钩子 ⇒ 自检**绝不污染真实快照**）。
> * **daemon 接线（三触发点 + 回灌）**：① `open_tab` 成功即登记作用域（**调用方给的 url**，`I14` 不读条目）；
>   ② `login_state` 判到 `logged_in` → **登录即写**；③ `shutdown` 在 `Browser.close` **之前**再写；
>   ④ 空闲 / 命令间隙 **每 5 s 定时刷新**（`_snapshot_tick` 走 `asyncio.wait_for(queue.get(), 5)` ⇒
>   刷新与命令跑在**同一任务**里，**不存在并发访问 CDP** 的时序问题）；⑤ 启动 / 自愈后
>   **回灌一次**（`Storage.setCookies`，每浏览器会话一次）——**失败发 `error{not_logged_in}`（`id="-"`）
>   显式提示**（`I23④`），但**不影响本次命令回包**；`daemon-state.json` 增 `snapshot_saves` /
>   `snapshot_restores` / `snapshot_reason`（L4）。
> * **`driver.py` 修正（`M7B-17` 口径）**：`cookies_all()` 由 `Tab.get_cookies()`（**页级**
>   `Network.getCookies`，依赖当前页、**会漏父域登录 Cookie**）改为 **`Browser.get_cookies()`
>   （浏览器级 `Storage.getCookies` · 全 origin · 含 HttpOnly）**；新增 `set_cookies()` /
>   `delete_all_cookies()`（回灌 / 自检注销用）。
> * **作用域口径（开口项 `MB-Q11` 采纳默认值）**：`scope` = 本次会话**导航过的域**（显式传参，可覆盖）；
>   **空作用域 ⇒ 不写快照**（隐私最小、宁缺勿滥，不擅自扩散到全库）。
> * **实现期订正 ①（自愈判据）**：把**内建 `ConnectionError`** 纳入 `gone_exceptions()` ——
>   浏览器被杀后，下一次 CDP 命令可能先撞「连接被拒绝」（实测 `[WinError 1225]`）而不是
>   `BrowserNotRunning`；漏掉它会**丢失自愈**（回归实测抓到的真因）。
> * **实现期订正 ②（DPAPI 完整性）**：DPAPI blob **自带完整性校验**（截断 / 改头部 / 改中段 / 改末尾 /
>   明文冒充密文 → 一律解密失败，错误码 13）；⚠️ 例外 = blob 头部一小段**明文「描述区」**
>   （`szDataDescr`）—— 改它不影响解密 ⇒ 自检的「篡改 1 字节」取**中段 / 末尾**（稳定判据）。
> * **实现期订正 ③（关闭耗时）**：**close 之前刚写过 Cookie** 时 Chrome 退出可能 > 5 s（要 flush
>   cookie 库）⇒ 自检判据改用**生产同款** `daemon.close_verdict`（超时 + warn + 未强杀 = 可恢复状态
>   ⇒ 通过），而非「必须 5 s 内退出」。
> * **实测数字（本机 · 2026-10-03）**：`--session-selftest --headless` **22 / 0**（7 协议 + **`VB2-39①~⑬`**：
>   DPAPI 往返 · 文件与日志**零明文** · 父域作用域 · 会期语义 · 过期过滤 · **6 例损坏不崩** ·
>   真机「注入 → 存 → 清空 → 回灌（名字齐 + `httpOnly` 保持）」· 关闭协议 · 快照留盘 · 残留 0 ·
>   **不污染真实快照**）；**系统 Python 3.14.3（无 pydoll）** 同命令 **15 / 0 + 组 B 显式 SKIP + exit 0**
>   （`VB2-30` 同族证据）；`--daemon-selftest --headless` **23 / 0**（22 → **+`M7B-11⑭`**：`shutdown`
>   前落盘必经、非死码；自检子进程快照**强制落临时目录**）；`--driver-selftest` **22 / 0**（回归）·
>   `--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · `aiwrite.exe --pipe-selftest` **PASS** ·
>   `api_probe --selftest` 七组 / `--exec-selftest` **320 / 0** / `--graph-selftest` **110 / 0** ·
>   `aiwrite --provider-selftest` **50 / 0** · 收尾**无残留进程** · `~/.brain-ai/session/` **未被自检创建**。
> * **边界**：**C++ 侧零改动**（`pydoll_channel` / `session_snapshot` / `SessionStore` 改造 + `PipeClient`
>   进度事件判据同步 + 诊断换代归 **step 6**）；`upload_image` / `send_prompt` / `read_answer` 仍未实现（批 3）。

> **批 1 step 6 记录块 —— C++ 侧接线（站点描述搬迁 + 快照只读视图 + 新通道 + `PipeClient` 判据同步）（2026-10-03）**
>
> * **新增文件（`src/web/`）**：`site_ref.h`（`SiteRef` = `LoginRequest` **纯别名** + 全部站点纯函数
>   **搬迁**；`webview_host.h` 改为 include 它 ⇒ 新通道取站点描述**不再拉入 WebView2 依赖**）·
>   `session_snapshot.{h,cpp}`（**L2 快照只读视图**：路径 / 触发时机 / 状态文案 —— **只碰元数据**
>   `exists` / `file_size` / `last_write_time`，**永不读内容** ⇒「导出物零明文」在本层**天然成立**）·
>   `pydoll_channel.{h,cpp}`（`namespace web::channel` 自由函数集：**`selftest` / `pydoll_login` 落地**；
>   `ensure_session` 只读判定（`I15′`）；`run_script` / `logout_site` / `current_tab_site` / `tab_on_site`
>   **如实回「尚未实现（批 3）」** —— 不假装成功）。
> * **`PipeClient` 判据同步（关闭 `MB-Q10`）**：`stage` 帧**带请求 id** ⇒ 视为该命令的**完成回包**；
>   只有 `id == "-"` 的才算纯进度事件（否则 `call("open_tab", …)` 会一直等到超时）。
> * **改动（零语义 / 只加不替换 · `B12-C1`）**：`paths.{h,cpp}` +`pydoll_profile()` / `session_snapshot_dir()`；
>   `provider_spec.{h,cpp}`：`allowed_web_fields()` +`attach` + 解析（**只解析、不消费** · `B12-C2`）；
>   `session_store.h` 注释（写入方 = CDP 快照 · 持久化归 L2 · `I15′`）；`CMakeLists.txt` 挂 3 组新源
>   + **`aiwrite_copy_python()`**（`brain_ai_browser/*.py` → `<exe>/python/…`；**`.venv` 排除**）；
>   `main.cpp` +`--pydoll-selftest` / `--pydoll-login <id>`；`tools/api_probe.cpp` **只新增**断言块。
> * **实测（本机 · 2026-10-03）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy）·
>   `api_probe --exec-selftest` **320 → 326 / 0**（**只升不降**：`VB2-40①~⑤` + `VB2-39⑥`）·
>   `api_probe --selftest` 七组 PASS · `--graph-selftest` **110 / 0** · `aiwrite --pydoll-selftest` **exit 0**
>   （守护进程起 → `hello` → `ready{proto=1, python=3.12.10, browser=edge}` → `shutdown` → 退出码 **0**；
>   解释器正确取到 `source/python/.venv`）· `aiwrite.exe --pipe-selftest` **PASS**（回归）·
>   `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS**
>   （**生产路径仍走 WebView2** ——「先建后拆」证据）· `--pydoll-login <未知 id>` **exit 2 + 可操作提示、不开窗**。
> * **实现期订正（真因）**：`find_package_dir()` 起初**只按「包目录存在」**挑候选 ⇒ 选中 `<exe>/python`
>   的拷贝（它旁边**没有** `.venv`）→ 退回 PATH 上的 `python.exe`（本机 **3.14.3 无 pydoll**）⇒ 生产登录必失败。
>   修法：**优先选「包目录 + 同级 `.venv/Scripts/python.exe`」**那一份（实测后 `python=3.12.10`）。
> * **边界 / 未含**：`--pydoll-login <id>` 的**真站点人工登录冒烟**待人工执行（`M7B-19` 门槛项）；
>   生产路径切换（`M7B-20`）/ `upload_image` / `send_prompt` / `read_answer` / 诊断换代（`M7B-18`）
>   归 **step 7（批 3）**。

> **step 7 开工（2026-10-03）** —— 三项里**两项受阻**（已**如实定位**：不是「做不动」，而是「契约未开」）；
> **唯一可当场交付 = `M7B-19` 全基线**（**已完成**）。
>
> **① `M7B-19` 全基线（硬门槛 · 本机 2026-10-03 实测 · 逐条贴数字）**
>
> | 命令 | 实测 | 判定 |
> |---|---|---|
> | 构建（`cmake --build --preset debug`） | **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy） | ✅ |
> | `api_probe --selftest` | 七组 PASS · exit 0 | ✅ |
> | `api_probe --exec-selftest` | **326 / 0** · exit 0 | ✅（只升不降） |
> | `api_probe --graph-selftest` | **110 / 0** · exit 0 | ✅ |
> | `aiwrite --provider-selftest` | **50 / 0** · exit 0 | ✅ |
> | `aiwrite --provider-dump` | **21 条** · exit 0 | ✅ |
> | `aiwrite --pipe-selftest` | **PASS** · exit 0（守护进程退出码 0 · 丢弃非法帧 0） | ✅ |
> | `aiwrite --pydoll-selftest` | **exit 0**（`proto=1` · `python=3.12.10` · `browser=edge`） | ✅ |
> | `aiwrite --pydoll-login <未知 id>` | **exit 2** + 可操作提示 · **不开窗** | ✅ |
> | `aiwrite --run-selftest --web` | **PASS** · exit 0 | ✅（**生产路径仍走 WebView2** ——「先建后拆」证据） |
> | `python -m brain_ai_browser --session-selftest` | **22 / 0** · RC=0（`VB2-39①~⑬` 全 PASS） | ✅ |
> | docs 相对链接（`docs` + `source`） | **320 / 0 broken** | ✅ |
> | `aiwrite --run-selftest`（**official** 模式） | **FAIL · exit 1** | ⚠️ **环境依赖偏差**（见下） |
>
> **⚠️ 唯一偏差 · 如实登记（并订正 v13 / v19 的措辞）**：`--run-selftest`（official）在本机
> **未设 `DEEPSEEK_API_KEY`**、且本机凭据库**无** `config.toml` 的 `api_key_ref = brain-ai/deepseek` 条目 ⇒
> `n3 LLMGenerate error（缺少 API Key）` → `n4 skipped` → `PC-05 归档：失败` ⇒ **exit 1**。
> **判定 = 环境依赖，非代码回归**，三条证据：① 报错路径在 `engine/provider_resolve.cpp:291-295`，
> **step 1~6 未触碰**；② `--run-selftest --web`（**不依赖** API Key）**PASS** ⇒ 执行器链路完好；
> ③ §11.1 的「PASS / PASS（5-5）」是 **2026-09-28 旧机**记录（`F:\Python` + Chrome 156），
> **v13「批 0 复测」把它当成换机后结论照抄** ⇒ **本次订正**：本机**从未真正复测**该条
> —— **§9.3「换机后必须重取基线」在此抓到一处漏项**。
> **待办**：用户填 Key（「提供商配置」或环境变量）后**复测该条**，才能在本表记 PASS；
> 可选改进（**未做**，属新任务）：让 `--run-selftest` 在缺 Key 时**回 2**（对齐 `--web-chat` 的
> 「2 = 缺少 API Key，离线部分已通过」语义，`main.cpp:74`）。
>
> **② `M7B-18` 诊断换代 —— 受阻（如实定位，未擅自扩契约）**
>
> * ✅ **已完成部分**：`pydoll_login`（= `aiwrite --pydoll-login <id>`）**step 6 已落地并实测**（未知 id → exit 2）。
> * ❌ **受阻部分**：`--web-adapter-selftest` / `--web-dom-dump` 走新通道 —— 二者需**在页面内执行诊断 JS**
>   （现走 `web::run_script_sync` → WebView2：`ai/dom_web_client.cpp:442,619`），而**词表 v1 只有 7 个命令**
>   （`protocol.py:42-50`：`hello` / `open_tab` / `login_state` / `upload_image` / `send_prompt` /
>   `read_answer` / `shutdown`）——**没有「执行脚本」这条命令**。⇒ 要落地**必须扩词表**，而 §6.1 冻结规则
>   明写「**改动 = 升 `v`** + §15 变更记录」（`v1 → v2`；牵动两侧 + `ready{proto}` 断言 + `VB2-40④⑤` 的
>   「如实报未实现」断言）。
> * ⚠️ **另有口径冲突，需拍板**：`M7B-18` 列在**批 2** 表内，但 `B12-C1`（§6.2）冻结「**批 2 只新增不替换、
>   调用方零改动**」—— 而「`--web-dom-dump` 改走新通道」= **替换** `main.cpp` 现有分发 ⇒ **两者不能同真**。
>   三条可选路径：**(a)** 扩词表 `v2` + 把 `M7B-18` 移到**批 3**（与 `run_script` 同批）；
>   **(b)** 诊断改走**不经管道**的 Python 侧 CLI（`python -m brain_ai_browser --dom-dump`），词表不动；
>   **(c)** 保留 WebView2 诊断至批 5。
>
> **③ 生产路径切换（`dom_web_client` → 新通道）—— 本 step 明确不做**
>
> `B12-C1` 已定「**切换归批 3**（`M7B-20`）」；且前置缺口**实测仍在**：Python 侧 `daemon.py` 只实现
> `hello` / `shutdown` / `open_tab` / `login_state`（`:363,382,384`），`send_prompt` / `read_answer` /
> `upload_image` **如实回「尚未实现（批 3）」**；C++ 侧 `run_script` / `logout_site` / `current_tab_site` /
> `tab_on_site` 仍为如实占位（`web/pydoll_channel.h:39,42,46,47`）⇒ **切换的充分条件 = 批 3 能力先落地**。
>
> **④ 打标签 `m7b-batch2` —— 未执行**：工作区**尚有 118 文件未提交**（含 step 1~6 与目录迁移），
> 打标签须先提交 ⇒ **待用户决定「提交切分 / 标签时机」**。

### 批 1 —— 底座（Python 侧）

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-10` | Python 包骨架（daemon / pipe / pydoll wrapper） | `source/python/brain_ai_browser/`（新；**文档中只写反引号、不写链接**） |
| `M7B-11` | 守护进程：自启动、后台、心跳、崩溃检测、日志脱敏 **+ 退出协议与异常埋点**（`MB-D0-8` L1/L4） | **退出 = `Browser.close` → 等进程退出（默认 5 s）→ 超时才强杀**；**用户手动关窗 = 可恢复状态**（`BrowserNotRunning` 必须被吞，不得报错）；未干净退出 → 下次启动提示「上次异常退出，登录态可能已回滚」+ 一键重登 / 从快照恢复；不留僵尸进程 |
| `M7B-12` | 管道协议 v1（命令 / 事件 JSON 行 + 版本号 + 非法行拒绝） | 离线桩可断言（`VB2-29`）；**词表见 §6.1（批 1 开工前冻结）** |
| `M7B-13` | Python 运行时检测与打包策略（`Q7` / `P7b-15` → **M6 硬门槛**） | 未安装 → 引导 + **不阻塞其他功能** |
| `M7B-14` | 浏览器检测与引导（Chrome 缺失 → Edge 兜底 → 可操作文案） | 两条路径都可起浏览器 |

### 批 2 —— C++ 通道（**中间检查点**）

| 编号 | 任务 | 落点 / 验收 |
|---|---|---|
| `M7B-15` | `web/pipe_client.{h,cpp}`（工作线程阻塞等待，`Q4`） | 不阻塞 UI 线程 |
| `M7B-16` | `web/pydoll_channel.{h,cpp}`：替代 `run_script_sync` / `ensure_session`（原 `webview_host.h:207/221`） | 接口收敛为「按站点 + 脚本 → JSON」 |
| `M7B-17` | `SessionStore` 改造：证据来源 = **CDP Cookie 快照**（`I15′`） | 仍是只读暴露 + 仅内存；证据取 **`Storage.getCookies`（浏览器级 · 全 origin · 含 HttpOnly）**；**加密快照落盘归 L2 独立组件**（`session_snapshot`，**不喂** `SessionStore`，`MB-D0-8`） |
| `M7B-18` | 诊断工具换代：`--web-adapter-selftest` / `--web-dom-dump` 走新通道；新增 `pydoll_login` | 与旧输出**同构**（便于对照）。**step 7 部分完成**：✅ `pydoll_login` 已落地（step 6）；⬜ 另两者**受阻** —— 需「页面内执行任意 JS」命令，而**词表 v1 无此命令**（扩表 = 升 `v`），且与 `B12-C1`「批 2 不替换」**冲突** ⇒ 见 **step 7 记录块的路径 (a)/(b)/(c)** |
| `M7B-19` | **中间检查点**：本批结束**必须全基线绿**（此时 WebView2 仍在，可对照） | 硬门槛 —— **step 7 实测 12 / 13 绿**；唯一偏差 = `--run-selftest`（official）缺本机凭据 → **exit 1**（**环境依赖，非回归**）；**门槛未闭合**（待用户配 Key 后复测该条） |

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
| `web.attach`（**新增** · `M7.md` `D11`） | 上传入口形态：`auto`（缺省 = 运行期探测）\| `file_input` \| `drop_zone` \| `paste_only` \| `none`；**只读自条目、无回落**（`I14`）；赋值来自 `P7b-05b` 实测（豆包 = **`paste_only`**）。**加载期不校验**（旧文件无此字段 = `auto`）→ 断言 = 表加载 + `auto` 回退探测 |
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
| **`VB2-29`（新）** | 管道协议**离线桩**：JSON 行解析 / 非法行拒绝 / 心跳超时文案（不依赖真实 Python）；**并按 §6.1 词表校验 `v` / `kind` / `id` 与未知命令** | |
| **`VB2-30`（新）** | `I21`：Python / 浏览器 / 守护进程 / 登录态缺失 → 报错 + 引导；**不发起任何 HTTP、不开任何浏览器** | |
| **`VB2-31`（新）** | `I22`：CDP 增量不可用 → 轮询并**显式标注「非流式」** | |
| **`VB2-32`（新）** | CDP 帧 → 增量文本（**复用 `delta_text_of` 既有形态 A/B 用例**；纯函数离线断言） | |
| **`VB2-33`（新）** | 遗留配置字段（`endpoints` / `probe_paths` / `token_expr` / `builtin:*`）→ **警告不报错**（旧文件可加载） | |
| **`VB2-34`（新）** | `I20` CLI 契约：① `--help` 含全部子命令名与退出码 ② 缺 `--provider` 的自检命令 → **候选枚举 + 码 2**（不得静默取第一个）③ `--provider auto` → **打印实际选中 id** ④ 退出码三档**跨命令一致** | |
| **`VB2-35`（新）** | `I2` 解冻**回归守卫**（反向断言）：旧「无参回落内置 DeepSeek」行为**已不存在** —— 默认 `LoginRequest.url` 为空、无参重载已删、内置 DeepSeek 探测脚本分支已删 | |
| **`VB2-36`（新）** | **复用资产回归**：`web_session_failure_hint()` 在**新数据源**（CDP 状态码 / body）下判定与旧断言**一致** | |
| **`VB2-37`（新）** | **合规禁用清单零命中**：源码内不出现 `expect_and_bypass_cloudflare_captcha` / `enable_auto_solve_cloudflare_captcha` / `apply_fingerprint` / `FingerprintApplier`（§13） | |
| **`VB2-38`（新）** | **关闭协议（`I23①`）离线桩**：退出路径缺「`Browser.close` + 等进程退出」即判失败（纯逻辑桩，不启浏览器）；`BrowserNotRunning` 必须被吞并归为**正常收尾**；超时兜底强杀必须**留 warn**（登录态可能回滚） | ✅ **Python 侧已生效（2026-10-03 · step 4）**：`daemon.close_verdict()` **7 条判据** + `--daemon-selftest` 端到端（`VB2-38①②`）；**C++ 侧关闭协议同步归 step 5~6** |
| **`VB2-39`（新）** | **快照无明文（`I23③`）**：快照文件字节内**不出现**任何 Cookie 名 / 值明文（DPAPI 加解密往返 + 明文扫描双重断言）；`SessionStore` 导出物同样零明文 | ✅ **Python 侧已生效（2026-10-03 · step 5）**：`session.py` 的 DPAPI 往返 / 明文扫描 / **6 例损坏不崩** / 真机回灌（名字齐 + `httpOnly` 保持）由 **`--session-selftest` 22 / 0**（`VB2-39①~⑬`）覆盖；**`SessionStore` 导出物零明文归 step 6**（C++） |

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
| `doubao-web` | `dom` | ✅ `div.tiptap.ProseMirror`（**B1 实测 2026-09-29**：未登录即渲染、命中 1 可见） | ⬜（待登录态） | ❌ 空（未登录无回答） | **网页版图片理解首个目标站（方案 B′ · 2026-09-29 · `M7.md` `D10`）**：上传入口 **B1 已判 = `paste_only`**（无 `file input` → 注入走 `DataTransfer`）；三项选择器待人工登录后取齐（`send` / `answer_selector`）+ **纯图无字**视觉性质判别（`source/python/_probe/m7b28_doubao_recon.py`，见 `M7.md` `P7b-05b`） |
| `tongyi-web` / `chatglm-web` / `spark-web` / `chatgpt-web` / `claude-web` / `gemini-web` | `dom` | ❌ 空（登录型条目） | ❌ | ❌ | 人工登录后 `--web-dom-dump --provider <id>` **一次取齐三项**（`M7B-28`） |
| **全部 11 条 dom** | — | — | — | — | `cookie_names` 回填（配合 `D-30`「`cookie_names` 优先」）+ `answer_selector` 回填（`M7B-29`，承接 `PB2-29`） |

**登录 Cookie 存活口径（`M7B-06b` 回填 · `MB-D0-8` L1 的端到端验收）**

> 「下次免登录」**不是无条件承诺** —— 语义取决于站点登录 Cookie **是否带 `expires`**：
> 无 `expires` 的**会话 Cookie**，即便干净退出也会掉（实测见 §5 结论块与 `M7B-09`）。
> 届时必须**如实告知**用户（`I21` 不静默）；如需保活，走 `MB-Q6` 的可选开关。

| 条目 | 登录 Cookie 名 | 有 `expires`? | `httpOnly`? | 干净退出 + 重启后仍登录? | 实测日期 |
|---|---|---|---|---|---|
| （12 条逐行回填） | | | | | |
| `doubao-web` | **14 个鉴权名**：`sessionid` / `sessionid_ss` / `sid_guard` / `sid_tt` / `sid_ucp_v1` / `ssid_ucp_v1` / `uid_tt` / `uid_tt_ss` / `session_tlb_tag` / `odin_tt` / `x-tt-multi-sids` / `passport_auth_status` / `passport_auth_status_ss` / `passport_mfa_token`（登录态站点域共 **33–35 条**） | ✅ **有**（实测 `session: false` ⇒ 全部带 `expires`） | 混合（`sessionid` / `sid_guard` / `uid_tt` 等 = `true`；`passport_csrf_token` / `x-tt-multi-sids` = `false`） | ✅ **是**（`close_wait` 优雅退出 → 重启复读：鉴权名仍在、`仍登录 = True`；另在**异常中断**后复读也仍在） | **2026-10-02** |

> **豆包一行（2026-09-29 计划）**：随 `M7.md` `P7b-05b` 的 **B0 段**（人工登录一次）一并回填 —— 登录前 / 后 Cookie 快照 + 登录 Cookie 名 / `expires` / `httpOnly` / 「干净退出 + 重启后仍登录」。其**「登录前后 Cookie 名差集」同时作为 `D-30`（`cookie_names` 优先）的豆包证据**。
> **2026-09-29 B1 已取（登录前）**：未登录态就有 **10 条**匿名 Cookie —— `hook_slardar_session_id` / `i18next` / `dbx-web-theme` / `conversation_list_v2_group_mode` / `flow_cur_user_sec_id` / `flow_user_country` / `s_v_web_id` / `passport_csrf_token` / `passport_csrf_token_default` / `biz_trace_id`（**仅名字，值不落盘**）→ 「Cookie 非空 = 已登录」的误报风险**再次得证**；**差集仍待 B0**（登录后快照未取）。物证 `source/python/_probe/out/m7b28_doubao_recon.json`。

> **2026-10-02 误报纠错（B0 判据 · 换机后首次实跑暴露）**：首次 B0 在**无人操作**窗口的 **9 s** 内自报
> 「✅ 检测到登录成功」——**用户确认当时未做任何登录操作** ⇒ 判为**误报**（**不予采信**；物证另存
> `source/python/_probe/out/m7b28_doubao_recon.b0-false-positive-20261002.json`）。根因两条，**都在判据本身**：
> ① **判据太松**：`Cookie 名差集非空` + `输入框出现` —— 前者命中的 `flow_cur_user_sec_id` /
> `flow_user_country` **匿名态也会被站点种下**，后者**未登录就渲染**（本页上一行的 B1 结论）；
> ② **Cookie 未按域过滤**：`Storage.getCookies` 返回**整个 profile** —— Edge 首启在 `msn.cn` /
> `ntp.msn.cn` 种了 **10 条**自家 Cookie（`App-User` / `MUID` / `MUIDB` / `USRLOC` / `_C_Auth` /
> `_C_ETH` / `_EDGE_S` / `_EDGE_V` / `__rubyUX` / `MicrosoftApplicationsTelemetryDeviceId`），
> 把差集**污染成与站点无关的名字**（`D-30` 的「`cookie_names` 优先」也会跟着错）。
> ⇒ 判据改为 **DOM 正证据**（头像 / 昵称 / 用户菜单 / 退出登录，**连续 2 次**命中）+ Cookie
> **只算站点域 `.doubao.com`**；「登录 / 注册」入口仅作诊断。**B0 复证结论**见下行豆包行。

> **2026-10-02 B0 复证（判据修正后 · exit 0 · 67 s）**：起点即**已登录**（**14 个鉴权名** / 站点域 35 条）
> → **6 s** 判定「已登录」（命中来自**鉴权 Cookie 白名单腿**；**DOM 腿仍报「无」**，**如实留证** —— 豆包在
> 859×450 视口下没有可见头像 / 昵称节点）→ `close_wait` **优雅退出**（残留 0）→ **重启复读**：鉴权名
> 14 个仍在、站点域 33 条 → `仍登录 = True`。物证 `out/m7b28_doubao_recon.b0-logged-in-20261002.json`
> （**误报**那轮另存 `out/m7b28_doubao_recon.b0-false-positive-20261002.json`）。
>
> **`D-30` 豆包证据（两次独立观测的差集，不是同一次运行的前后）**：
> * **匿名态** 站点域 **8–10 条**：`bd_sso_hi3jfd` / `ttwid` / `i18next` / `dbx-web-theme` /
>   `hook_slardar_session_id` / `conversation_list_v2_group_mode` / `flow_cur_user_sec_id` /
>   `flow_user_country` / `s_v_web_id` / `passport_csrf_token(_default)` / `biz_trace_id`
> * **登录态** 站点域 **33–35 条** ⇒ **净新增 23 名** = **14 个鉴权名**（见下行表格首列）+
>   `d_ticket` / `multi_sids` / `n_mh` / `has_biz_token` / `is_staff_user` / `flow_account_sync_event` /
>   `flow_multi_user_sec_info` / `LARK_SUITE_ACCESS_TOKEN` / `feishu_dpop_keypair`
> * ⇒ **`cookie_names` 优先名单取那 14 个鉴权名**（其余随站点版本 / 账号形态变化，不宜写死）
> * ⚠️ 复证轮的 `B0_d30_diff_nonempty` 为 `false`（起点已登录 ⇒ 本次无差集），差集由上述**两次运行**给出。

**回填记录格式**（每站一行，写入 §15 变更记录）：

```
<id>：input=<命中选择器> · send=<键/点击> · answer=<选择器> · done_when=<判据> · cookie_names=<名单>
      实测日期 / 浏览器版本 / 备注（如「需先关掉新手引导」）
```

**网页版图片理解侦察记录**（`P7b-05b` 执行时逐行追加；判定口径见 `M7.md` §12.1 —— 上传入口 4 类 + **纯图无字**视觉性质判别）：

| 条目 | 上传入口形态 | 注入路线 | 网络回执 | 视觉性质 | 实测日期 |
|---|---|---|---|---|---|
| `doubao-web` | ✅ **`file_input`**（登录态 · 大视口：`input.hidden` = **隐藏 file input**）；⚠️ **默认小视口（859×450）下该 input 根本不在 DOM** ⇒ 会误判成 `none` / `paste_only`（**2026-09-29 B1 的 `paste_only` 是小视口假象**） | ✅ **`DOM.setFileInputFiles`**（`--inject-mode dom`）：文件真进 `input.files`（`file_input_files = m7b28-doubao-256x256.png`） | ✅ **4 条 POST 链**：`POST /alice/profile/self` → `POST /alice/resource/prepare_upload` → `POST tos-hl-x.snssdk.com/upload/v1/…png`（TOS 上传本体）→ `POST /top/v1?Action=CommitImageUpload`（提交确认） | ✅ **真视觉**（256×256 纯红方块 → 答「一整块均匀、饱和度很高的**正红色**…没有任何图案、文字、物体」；⚠️ 同一夹具用 **1×1** 尺寸时被判「**完全纯白色**」= 退化输入） | **2026-10-02** |
| （待填） | | | | | |

> ⚠️ **视口尺寸是硬前提（2026-10-02 实测 · 影响 `P7b-10` / `M7B-28`）**：默认视口 **859×450** 时豆包**不渲染附件入口**
> —— DOM 里 `<input>` 总数 **0**、附件按钮为空、composer 退化成 `textarea[data-testid="chat_input_input"]`；
> 换成 **1440×1000** 才出现隐藏 `input.hidden`，composer 回到 `div.tiptap.ProseMirror`。⇒ `M7B-28` 启动浏览器
> **必须显式给窗口尺寸**（探针已加 `SITE_WINDOW_ARGS = ('--window-size=1440,1000',)`）。
> ⚠️ **2026-09-29 的 `paste_only` 结论作废**：那是**小视口**下观察到的形态；登录态 + 大视口下入口是
> `file_input`（隐藏），**注入路线随之由 `DataTransfer` 改为 `setFileInputFiles` / `expect_file_chooser`**。

---

## 11. 验证基线与文档同步

### 11.1 基线

| 命令 | 当前记录（2026-09-27） | `M7B` 目标 |
|---|---|---|
| `source\build.ps1` | 0 error / 0 warning | **不变**（硬指标） |
| `api_probe --selftest` | 七组 PASS | 换代后全绿 |
| `api_probe --exec-selftest` | **251 / 0** → **326 / 0（2026-10-03 · step 6）** | 批 1–4 **只升不降**（现状 ✅）；批 5 内删类断言**先降后升闭合**（§9.3） |
| `api_probe --graph-selftest` | 110 / 0 | 不变 |
| `aiwrite --provider-selftest` | 50 / 0（`--provider-dump` 21 条：official 9 / web 12） | 换代后全绿 |
| `aiwrite --provider-dump` | 21 条（official 9 / web 12） | 条目数不变 |
| `aiwrite --run-selftest` / `--run-selftest --web` | PASS / PASS（5-5）**（⚠️ 2026-09-28 旧机记录）** → **本机 2026-10-03 复测**：official **FAIL · exit 1**（缺本机凭据）· `--web` **PASS** | 换通道后重测；⚠️ **该条依赖本机凭据**（`api_key_ref = brain-ai/deepseek`）—— 换机 / 重装后**必须重配**，否则必红 |
| `--login-selftest` / `--web-chat` | 0 / 1 / 2 语义 | **调用形式与退出码不变**（`I20`） |
| `--web-probe` | 可用 | **废弃**（`--help` 标注替代命令） |
| **新增** `--web-stream-selftest --provider <id>` | — | CDP 增量逐帧 PASS（`I22` 断言） |
| **新增** `--session-selftest [--headless]` | — | **L2 快照**：`VB2-39①~⑬` 全绿（离线 + 真机「注入 → 存 → 清空 → 回灌」；`VB2-30` 同族：无 pydoll ⇒ 组 B **显式 SKIP + exit 0**） |
| **新增** `aiwrite --pydoll-selftest` | — | **新通道**（step 6）：起守护进程 → `hello` → `ready{proto=1}` → `shutdown` → 退出码 **0**（实测 exit 0；**不开浏览器**） |
| **新增** `aiwrite --pydoll-login <id> [--timeout]` | — | **新通道冒烟**（step 6）：起**有头**浏览器 + 人工登录 → 判据 `cookie_names` 命中 → 关窗；未知 id → **exit 2 + 可操作提示、不开窗**（实测）；**真站点登录待人工执行**（`M7B-19` 门槛项） |
| **新增** `--daemon-selftest [--headless]` | — | 守护进程：`VB2-38` 离线 + `M7B-11` 端到端（**22 → 23 条**：`+M7B-11⑭` L2 触发点；自检子进程快照强制落临时目录） |
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

> **换机环境重建（2026-10-02 · 路线 B · 探针 + `requirements.txt` + 文档，零 C++ 改动）**
> **背景**：国庆换机提交 `8e79254` 后 **`.venv` 未随仓库迁移**（`.gitignore` 覆盖）⇒ 本机只剩
> Python **3.14.3**、**无 `pydoll`**、**无 Chrome**（仅 Edge **154.0.4258.48**）→ `P7b-05b` 的
> **B0 / B2 / B3** 开跑不了。本轮**只重建环境**，**路线 B（最保守）= 与旧机 9 个探针物证同构**：
>
> | 项 | 旧机（2026-09-28 批 0） | 本机（2026-10-02 重建后） |
> |---|---|---|
> | Python | 3.12.1（`F:\Python`）+ 项目内 `.venv` | **3.12.10**（Install Manager 并行装 · **`PrependPath=0`** 不抢 3.14）+ `source\python\.venv` |
> | pydoll | `pydoll-python 2.27.0` | **`pydoll-python 2.27.0`**（**钉版**，理由与安装命令写在 **`source/python/requirements.txt`** 头部；该文件**入库**，`.venv` 仍忽略） |
> | 浏览器 | **Chrome 156.0.8072.0** | **无 Chrome → Edge 154.0.4258.48**（`M7B-14` 兜底**首次实跑**） |
>
> **环境物证**（探针 JSON 的 `env` 字段实读）：`{'python': '3.12.10', 'pydoll': '2.27.0',
> 'browser_kind': 'edge', 'browser_exe': 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
> 'browser_version': '154.0.4258.48'}` —— 换机可追溯，不再靠人记。
>
> | 命令 | 实测 |
> |---|---|
> | `py -3.12 -V` | **Python 3.12.10** |
> | `source\python\.venv\Scripts\python.exe -V` / `-m pip -V` | **3.12.10** / **pip 25.0.1** |
> | `source\python\.venv\Scripts\python.exe -m pip install pydoll-python==2.27.0` | **`pip_rc=0`**（**110 s** · 17 包 · **直连 PyPI 不换源**） |
> | `source\python\_probe\m7b28_doubao_recon.py --selftest`（本地桩 · 临时 profile · headless · **零副作用、不需登录**） | **PASS**（**exit 0 · 47 s**）：读回自证 `probe-ok` · `entry_kind=file_input` · `file_inputs=['#file']` · `#file` 命中 **1** · 收尾 `close_wait` **残留 0** |
>
> **换机兜底口径（本轮落到代码）**：`m7b09_common.py` 增 `browser_kind()` / `browser_exe()` /
> `browser_class()` / `env_proof()` —— **只按标准安装路径**先探 Chrome、再探 Edge，**命中谁用谁**；
> 两者皆无**仍返回 `Chrome`**（让库自己报启动失败，探针把**真实错误**留证，不做静默兜底）；
> `m7b28_doubao_recon.py` 三处 `Chrome(...)` → `common.browser_class()(...)`，JSON 增 **`env`**、
> verdict 增 **`env_browser_kind`**。
> **网络取证（换机后新事实）**：`py install --yes 3.12` 走 **BITS** 报 `NoInternetError`，但普通 HTTPS
> **通**（python.org HTTP 200）⇒ **真因是带宽**（≈**20–70 KB/s**、多次 stall），非防火墙 ⇒ 改「官方安装器
> 直下 + **断点续传** + **停滞自动重试**」；镜像实测 **`huaweicloud` 不通** / `tuna` ~8 KB/s。
> **未含（口径不变）**：**B0 / B2 / B3 仍待人工登录一次**；`providers.json` 的 **`web.attach` 仍不填**；
> `DevPlan.todo` id **200 / 201** 保持 `done: false`。

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
>
> **2026-10-02 追加（只改文档 · 零产品代码）**：`M7.md` 新增 **`D11`**（`web.attach` 契约 + `capabilities.vision=false` 的**显式确认**口径）/
> **`P7b-16`**（拆 `M5-02` 闸门 + 分派）并同步 §13 / §14 / §15 / §16；本文新增 **§6.1 协议 v1 词表** 与 §7 的 **`web.attach`** 行，
> `M7B-12` / `VB2-29` 补「按 §6.1 词表校验」；`DevPlan.todo` 登记 id **200**（`FEA-M7-10`）/ **201**（`TST-M7-08`）—— 随 `M7B-40` 一并复核。

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
| `MB-Q7` | **词表缺「命令未实现」错误码**（`M7B-02` 实现期暴露）：`--serve` 收到「词表内但本步尚未实现」的命令时，现有 9 个码里只能借 `daemon_down`（语义不符，易被 UI 误读为「守护进程挂了」） | **本批不动词表**（动词表 = 升 `v` + §15 登记）。**建议**（批 2 收口时一并定）：新增 `not_implemented`（`v=2`），或把「未实现」并入 `bad_frame`（`hint` 说明）。**当前口径已在代码注释与守护进程日志里如实标注**，不静默、不假装成功 |

| `MB-Q8` | **词表缺「依赖细分」错误码**（`M7B-04` / step 3 实现期暴露）：§6.1 只有 `no_python`（依赖缺失）与 `no_browser`；**「pydoll 未安装」** 只能沿用 `no_python`（语义偏「Python 运行时缺失」，易被 UI 误读为「请装 Python」） | **本批不动词表**（动词表 = 升 `v` + §15 登记）。现状：`runtime.PYDOLL_HINT` 在 `hint` 里**显式写明**「本机 Python 缺少依赖 pydoll」+ 安装命令 ⇒ **不静默、不含糊**。**建议**（批 2 收口与 `MB-Q7` 一并定）：新增 `no_pydoll`（`v=2`），或让 `no_python` 的 `hint` 承担细分职责（当前做法） |

| `MB-Q9` | **词表缺 `login` 事件名**（step 4 实现期暴露）：§6.1 表 2 只列 6 个事件（`stage` / `evidence` / `delta` / `answer_done` / `error` / `ready`），但 §6.2 命令边界表要求 `login_state` 回 `login {state, cookie_names, has_expires, http_only}` | **本批不动词表**（事件词表变更 = 升 `v`）。现状：Python 按 **§6.2 命令边界表**实现 `login`（当步迭代只校验命令词表，不影响运行，且已端到端实测）。**建议**（批 2 收口与 `MB-Q7` / `MB-Q8` 一并定）：把 `login` 补进 §6.1 表 2，或改为 `stage{stage="login"}` |
| `MB-Q10` | **两处口径未入表**（step 4 实现期暴露）：① `stage` 帧**带请求 id** = 该命令的**完成回包**（`id="-"` 才是纯进度事件）—— C++ 侧 `PipeClient` 的进度事件判据需同步，否则 `open_tab` 的 `call()` 等不到回包；② `login_state` 的两个**可选**字段 `cookie_names` / `domain_suffix`（来源 = 条目 `web.cookie_names` / profile 域过滤；未给 → `state=unknown`） | **本批不动词表**；现状已在代码注释与 §6.2 step 4 块写明（以文档口径先行）。**建议**：① 作为**协议口径澄清**正式写进 §6.1（无需升 `v`）；② 可选字段补进 §6.1 命令表「可选字段」列 |

| `MB-Q11` | **L2 快照作用域默认值**（step 5 实现期定案）：`MB-Q4` 建议「仅配置表内网页版条目 origin」，但 Python 侧按 `I14` **不读 `providers.json`** ⇒ 需要「作用域从哪来」的明确口径 | ✅ **已采纳默认值（step 5）**：`scope` = 本次会话**导航过的域**（由 `open_tab` 的 url 派生、经 `session.SnapshotStore.add_scope()` 登记）；**空作用域 ⇒ 不写快照**（隐私最小、宁缺勿滥）。**接口留口**：`save(hosts=[...])` 可显式覆盖（含"全库"）。**待批 2 收口拍板**：是否给部署期提供「全库快照」开关（默认关） |

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
| 2026-09-28 | v4 | **`M7B-03` 前置验证通过（结论已回填）**：用**本地页面**（`source/python/_probe/local_server.py`）把「CDP 能否拿增量」与「站点登录 / 改版」解耦，5 个变体实测 ⇒ **主路线 = `Fetch.takeResponseBodyAsStream` + `IO.read(size=128~256)`**（`size` 即最小推送粒度；实测 256 B/帧、41 帧 / 10 KB 流）；② 仅 `EventSource` 站点可用；③ 只给进度；④ 页面 hook 未打通（备用）。**并新增两条实施要点**（`-32602` 必须容忍；0 字节只在 `eof=true` 结束）。证据：`source/python/_probe/out/m7b03_cdp_stream.json`。**其余 7 项前置验证未跑** |
| 2026-09-28 | v5 | **`M7B-01` / `M7B-05` / `M7B-08` 前置验证通过 + 一条**错误结论更正**：① `M7B-01` 全 PASS（headful、profile 落盘 1064 文件、**CDP 见 HttpOnly 而 `document.cookie` 看不到**、重启后持久 Cookie 保留）；② `M7B-05` 全 PASS（**154 ms/字符** 逐字输入 + 21 次 input 事件；文件注入**两条路径**都成功且触发 change）；③ `M7B-08` 全 PASS（两域并存 → 清 A 后 **A 空 / B 完整**）；④ **更正**：路线④（页面 hook）**其实可用**（8 帧 / 5568 ms），首轮"未打通"是**探针读回层级 bug**（`execute_script` 返回**两层 `result`**）；⑤ 新增 **6 条 Pydoll 实施要点**（含**退出必须 `Browser.close` 否则登录态不落盘**、`--no-first-run` 已由库添加、Cookie 按域名隔离、`IO.read` 攒批、打字需先 `focus()`）。证据：`source/python/_probe/out/m7b0{1,5,8}*.json`、`_diag_persist.py`。**仍剩 `M7B-02`/`M7B-04`（C++↔Python 管道）与 `M7B-06`/`M7B-07`（需人工登录）** |
| 2026-09-28 | v6 | **登录态持久化改进（用户拍板：四层全做 · `MB-D0-8`）** —— 触发：前置验证暴露 **Pydoll `stop()` = 「`Browser.close` 后立刻 `terminate()`（硬杀）」** → Cookie 库不落盘 → **登录态每次退出即丢**（源码证据 `base.py:223-243` + `browser_process_manager.py:73-89`；实测强杀=False / `close`+等 3 s=True）。落盘改动：① §2 新增决议 **`MB-D0-8`**（L1 干净退出 / L2 DPAPI 加密快照 / L3 启动自愈 / L4 异常退出可见）；② §3 架构表新增「**退出 / 关闭**」与「**登录态持久化**」两行，并收紧「会话存储」行（落盘只经 L2、**不喂** `SessionStore`）；③ §5 实施要点 **+⑦⑧**（`stop()` 禁用 + 源码行号；`browser_preferences` 为库级一等公民、读-合并-写带 backup）并新增**探针纪律**（「机制不可用」结论须先排除探针自身）；④ §5 前置验证 **+`M7B-09`**（关闭时序三档 + 提交时机 + 快照往返 + 接管）与 **+`M7B-06b`**（真站点「重启仍登录」+ 记录 `expires`）；⑤ §6 `M7B-11` 加**退出协议与异常埋点**、`M7B-17` 明确证据取 `Storage.getCookies`；⑥ §9.1 新增不变量 **`I23`** 并给 `I15′` 加补注；⑦ §9.2 新增断言 **`VB2-38`**（关闭协议离线桩）/ **`VB2-39`**（快照无明文）；⑧ §10 新增「**登录 Cookie 存活口径**」回填表（把"下次免登录"从口号变成可核对的数字）；⑨ §12 登记 L2 对新目录**不进回滚清单**；⑩ §13 新增**登录态快照红线**；⑪ §14.2 新增 `MB-Q4`/`MB-Q5`/`MB-Q6`（快照范围 / 关闭等待 5 s / prefs 默认不启用）。**仍只改文档：产品代码零改动** |
| 2026-09-28 | v7 | **`M7B-09` 前置验证通过（`MB-D0-8` L1~L4 四层全部得到实测支撑）** —— V-a~V-d 四轮 + 4 项补充诊断：① **V-a**：`close_wait`（`Browser.close` → 等进程退出）**0.22 s** 即保住 Cookie；强杀与库默认 `stop()` 丢，强杀残留 `Cookies-journal`；写 Cookie 后**≥30 s** 再强杀仍存活 ⇒ **10–30 s 延迟提交窗口**；② **V-b**：快照往返 7/7（`httpOnly` 保持、站点认账）；③ **V-c**：外部关窗（等价点 X）**0.2 s** 干净退出且 Cookie 保住、`stop()` 抛 **`BrowserNotRunning`（须吞）**；残留实例占 profile 时 **同端口 = 静默附着到既有实例 / 随机端口 = `FailedToStartBrowser`**，`browser.connect(ws)` **attach 可行**；④ **V-d**：**每 case 全新 profile** 对照 ⇒ 干净退出后**会期 Cookie 必掉**（持久 Cookie 保留），**`--restore-last-session` 是其唯一有效保活手段**但**会重开上次标签页**（副作用，实测 2 个 marker 页被恢复）、`session.restore_on_startup=1` 写 `Preferences` **无效**；⑤ **补充诊断**：`_diag_cookie_scope` 证 `tab.get_cookies()` **是当前页作用域**（停在 `about:blank` 必读空；全库须 `Storage.getCookies`）· `_diag_snapshot_session` **端到端证明 L2 回灌能救回会期 Cookie**（`session`/`httpOnly` 保持 + 服务端 `/eyes` 认账）· `_diag_restore_tabs` 证开关副作用 · `_diag_profile_identity` 用 `chrome://version` 排除"profile 被换"假设。落盘文档改动：§5 `M7B-09` 行改为**已过**并新增四轮结论块（含**探针收尾纪律**：崩溃会留孤儿占 profile → 下次 `start()` 直接失败）；§5 实施要点 **+⑨⑩⑪**（读数作用域 / profile 被占两形态 / `--restore-last-session` 默认禁用）；§9.1 **`I23⑤`**；§13 新增**启动与登录态两条"不做"**；§14.2 **`MB-Q6` 结案**（默认不启用 prefs，会期 Cookie 交 L2）；`source/README.md` §6.2 **+3 条探针纪律**；`docs/CHANGELOG.md` 前置验证进度更新。**仍只改文档 + 一次性探针：产品代码零改动** |
| 2026-09-28 | v8 | **`M7B-09` 补一条收口（L2 快照节奏）**：由 V-a 的提交窗口数字（**10–30 s**）推出实现要求 —— 快照刷新间隔 **< 10 s** + 两个事件触发点（**登录成功即写** / **优雅退出前再写**），否则崩溃/断电时快照会旧一个窗口。同时固化工程纪律：`m7b09_common.shutdown()` 增加**残留自检与清理**（`strays_after` / `strays_killed`），探针一律 `try/finally` 收尾 —— 本轮两次因孤儿实例占住 profile 导致下一次 `start()` 直接 `FailedToStartBrowser`。**仍只改文档 + 一次性探针** |
| 2026-09-28 | v9 | **文档进度同步（8 个文件 · 只改文档）** —— 前置验证已跑到 **5/9**，但「进度条类」文档仍停在「已立项 · 计划中未开工」，逐处对齐：① `M7B.md` 顶部状态行、§5 标题与进度摘要、`M7B-02` 补「**C++ 工具链冒烟已通**，剩管道双向 IPC 本体」、`M7B-04` 标 🟡 部分；② `CHANGELOG` 前置验证块新增「⏳ 待过 4 项 + 1 项加强」；③ `docs/README.md` 当前进度行、`milestone_plan.md` 状态列与前置口径（`M7B-01`–`08` → `M7B-01`–`09` + `M7B-06b`）；④ `roadmap.md` 轨道 C + **`RQ-6` 由「未实测」改为已实测收口**（`MB-Q1` 结案）；⑤ `ai_writer_nodes.md` §18 两行（**顺带修掉 P7-a 行仍写「计划中」的过期状态**）；⑥ `DevPlan.todo` `TST-M7B-01` 补进度 + **新增 `FEA-M7B-03`（`MB-D0-8` 四层登录态持久化）**；⑦ `M7.md` §12.1（`P7b-01`/`P7b-04` 标已过、`P7b-03` 标部分）+ §12 上方「外部调研」注更正；⑧ `节点编辑器使用说明.md` §10.2 前置验证补状态；⑨ **反向核对再补 3 处**（`M7B.md` §12 三级回滚口径、`CHANGELOG` 立项块任务口径、`ai_writer_nodes.md` §18 P7-b 行状态）。**验证**：文档断链 **276 checked / 0 broken**、`DevPlan.todo` JSON 合法且节点 **196 → 197**、过期措辞反向核对：`⬜ 计划中` 行 **0 命中**、旧「未过则停留」编号 **0 命中**（唯一保留命中 = 本行对旧措辞的引用）、`source/` **产品代码零改动**（其变更均属 P7-a 变更集） |
| 2026-09-29 | v10 | **方案 B′ 定案（网页版图片理解目标站改豆包）+ 站点多模态能力外部核查（只改文档 · 零产品代码）** —— ① **目标站**：网页版图片理解首个目标站 = **`doubao-web`**（`M7.md` 新增决策 `D10`；理由：条目已存在 + 字节多模态系 + 不动批 4 主线），**`deepseek-web` 保持文字链路主线**（`M7B-06`/`M7B-26` 不变）；② **顺序闸门**：先 `P7b-05b` 只读侦察（`source/python/_probe/m7b28_doubao_recon.py`）→ 结论**用户审核通过后**才启动 `M7B-02`/`04` 管道本体（`M7.md` §16 已插入该顺序）；③ `M7.md` 新增 **`P7b-05b`**（上传入口 4 类判定 + 三项选择器真实命中 + `D-30` 证据 + **纯图无字**视觉性质判别）与 **`M7-10`**（DeepSeek 官方 API 视觉跟进）；④ **外部核查**：DeepSeek **API 已原生多模态**（2026-09-10 `deepseek-flash` = V4.1-Flash，Vision ✓；`deepseek-v4-pro` ✗）→ 仓库表过时；DeepSeek **网页版**推断无视觉（Web 跑 V4-Pro GA 且 ✗ + 公告只提 API）；**豆包待实测** → 新增 [网页版协议实测记录.md](../网页版协议实测记录.md) **§8**；⑤ §10 表把 `doubao-web` **单列**（从合并行拆出）+ 新增「**网页版图片理解侦察记录**」表 + 「登录 Cookie 存活口径」表补豆包一行（`P7b-05b` B0 段回填）；⑥ `M7.md` `P7b-10` 主路线改写为 **`expect_file_chooser`**（`M7B-05` 已实测两条注入路径均成功）→ 仅当无 `file input` 时才退 `DataTransfer`；⑦ 同步 `M_patchAB_rest.md`（`D-30` 证据计划 + 附录 E 豆包行）/`docs/README.md`/`CHANGELOG.md`/`DevPlan.todo`/`roadmap.md`/`ai_writer_nodes.md`/`milestone_plan.md`。**验证**：文档断链自检 **broken 0**、`source/` **零改动**、`source/assets/providers.json` **未改** |
| 2026-09-29 | v11 | **豆包 B1 只读侦察结果回填（只改文档 · 零产品代码）** —— 物证 `source/python/_probe/out/m7b28_doubao_recon.json`（`started 2026-09-29 20:33:03`、`readback_self_proof = "probe-ok"`、`close_wait` 退出码 **0**、`strays_after: []`）。① **入口形态 = `paste_only`**（`file_inputs: []` / `drop_zones: []`）⇒ **`P7b-10` 注入路线判定 = `DataTransfer`**（主路线 `expect_file_chooser` 对豆包**不适用**；`M7.md` §12.1 + §12.2 已补注）；② **composer = `div.tiptap.ProseMirror`**（**未登录即渲染**、命中 1 可见）→ §10 表该行 `input_selector` 由「❌ 空（未登录不渲染输入框）」**更正**为实测量；③ **未登录 10 条匿名 Cookie**（仅名字）→ `D-30`「Cookie 非空 = 已登录」误报风险再获旁证；④ **`answer_selector` 4 类候选全 0**（`message`/`answer`/`reply`/`markdown`）→ 仍需「登录 + 手动发一条」（= `M7B-06` / `M7B-28` 同一人工关卡）；⑤ **B0 / B2 / B3 未跑**（登录 Cookie 差集 / 注入 + 网络回执 / 纯图无字判别）⇒ **本次结果不构成 `I18` 证据**，网页版条目 `capabilities.vision` **保持 `false`**。同步：`M7.md` §12.1（状态列 + B1 结论块）/ `DevPlan.todo` `TST-M7-07`（仍 `done: false`）/ `网页版协议实测记录.md` §8.3 / `CHANGELOG.md`。**验证**：文档断链自检 **docs/ 全量 289 / 0 broken**、`DevPlan.todo` JSON 合法（199 条）、`source/` 与 `source/assets/providers.json` **零改动** |
| 2026-10-02 | v12 | **管道协议 v1 词表 + `web.attach` 契约补齐（只改文档 · 零产品代码）** —— 现状核对发现「网页版图片上传」施工图 3 处缺口，本轮补 ②③ 与 C++ 侧契约（① 属实测，留 `P7b-05b` B2）：① **新增 §6.1 协议 v1 词表**（帧格式 / `v`·`id`·`kind` / 7 命令 / 6 事件 / 错误码 → `I21` 映射 / 容量与超时上限），`M7B-12` 与 `P7b-07` 按此施工；② **`web.attach`** 入 §7 配置表迁移（`auto`\|`file_input`\|`drop_zone`\|`paste_only`\|`none`；豆包 = `paste_only`，**赋值随 B2/B3 回填**）；③ `M7B-12` / `VB2-29` 行补「按 §6.1 词表校验」；④ `M7.md` 同批：`D11` / `P7b-16` / §13 / §14 / §15 / §16 + `DevPlan.todo` id 200/201 + `CHANGELOG.md`。**验证**：`DevPlan.todo` JSON 合法（**201 条**）· docs 断链 **0** · `source/` 与 `providers.json` **零改动** |
| 2026-10-03 | v13 | **批 1–2 施工细化表冻结（`B12-C1`~`C3`）+ 批 0 基线复测（含换机修复）** —— ① 新增 **§6.2**：把批 1（`M7B-10`~`14`）/ 批 2（`M7B-15`~`19`）拆到**文件 / 函数 / 命令字段 / 断言编号**级（批 1 文件表 + 命令边界；批 2 接口映射表 + 逐文件改动清单 + 断言归属 + `M7B-19` 门槛 + 风险 `P1`~`P6` + 开工 step 1~7）；② 三处**冻结口径**：`B12-C1` 批 2 **只新增不替换**（切换归批 3 `M7B-20`）/ `B12-C2` `allowed_web_fields()` 加 `attach`（**运行期不消费**、`providers.json` 不落值）/ `B12-C3` `SessionStore::user_token` **留到批 5**；③ **批 0 基线复测**（换机后必须重取）：构建 **0/0** · `--selftest` 七组 PASS · `--exec-selftest` **311 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS** · docs 相对链接 **296 / 0**；④ **换机账修复（本批唯一产品侧改动 · 4 处）**：开工时 `--exec-selftest` **309 / 1**，唯一失败 = `M5-04 示例：E-02 加载校验通过`（示例写死**旧机绝对路径** `F:/GameDao/.../sample.png`，而 `engine/graph.cpp` 的 File 参数校验对非令牌值只做 `exists(原值)`）⇒ ① 示例改**仓库相对路径** `assets/images/flamingo.png`；② `tools/api_probe.cpp` **新增**「示例图片路径**可移植**（不含盘符 / 反斜杠）」断言 + 校验前把路径**代入本机示例图**；③ `main.cpp` 默认图 `sample.png` → `flamingo.png`；④ `docs/节点编辑器使用说明.md` 同步（断言 **310 → 311**，全绿）；⑤ **工具账**：`source\build.ps1` 在本机**被执行策略拦截**（复现改 `cmake --build --preset debug`）、旧机 `check_links.py` 本机**不存在**（改等价内联检查）。**明确未含**：`sample.png` 删除与 `flamingo.png` 入库的取舍（属上一批未提交变更）；`providers.json` 的 `attach` / `cookie_names` **仍不落值**；`DevPlan.todo` id **200 / 201** 保持 `done: false` |
| 2026-10-03 | v14 | **step 1 收口 + step 2（`M7B-02`）通过：C++ ↔ Python 命名管道双向 IPC 打通** —— ① **step 1**（§6.2 记录回填）：`source/python/brain_ai_browser/{__init__,protocol,__main__}.py`（纯标准库）+ `src/web/channel_frames.{h,cpp}` + `api_probe` 离线断言 **`VB2-29①~⑦` / `VB2-32①②`**（**不删旧断言**；`--exec-selftest` **311 → 320 / 0**）；实现期订正：`stage` 事件的阶段键 `name` → **`stage`**（帧头 `name` 已被事件名占用）；② **step 2 新增**：`source/python/brain_ai_browser/pipe.py`（`ctypes` → `CreateNamedPipeW`/`ConnectNamedPipe`/`ReadFile`/`WriteFile`，**overlapped + 精确超时**、`read_line`/`write_line`/`write_bytes`、`PipeServer.accept` 超时作废实例）+ `src/web/pipe_client.{h,cpp}`（`connect`/`call`/`send_command`/`on_event`/`close`；读线程 + `id` 配对 + **进度事件不完成调用** + 非法帧回 `err{bad_frame}`）+ `main.cpp` 的 **`--pipe-selftest`**（起守护进程 `--serve --once` → `hello` → 校 `ready` → `shutdown` → **等进程退出**；守护进程 stdout/stderr 落 `~/.brain-ai/logs/pipe_selftest_daemon.log`，失败时打印日志尾部）+ `CMakeLists.txt` 挂 `src/web/pipe_client.cpp`；③ **`M7B-02` 判定 ✅ 已过**（§5 行 + §6.2 进度块贴数字）：`--pipe-selftest` **PASS / exit 0**、loopback **13 / 0**、守护进程逐帧物证 **2 帧**、`--exec-selftest` **320 / 0**、`--graph-selftest` **110 / 0**、`--provider-selftest` **50 / 0**、`--provider-dump` **21 条**、`--run-selftest` **PASS**、无残留进程；④ **实施要点 `P7`（新风险行）**：**同步句柄上的并发 I/O 被 I/O 管理器串行化** ⇒ C++ 句柄**必须** `FILE_FLAG_OVERLAPPED`（读/写各一事件）+ 写 30 s 上限 + `close()` 用 `CancelIoEx`；踩坑表象 = 「写阻塞 20 s（＝服务端 idle 超时）后 `ERROR_NO_DATA`」而服务端同轮 0 帧（**双向都像「对方没听见」**，易误判为协议错）；⑤ **`M7B-04` 进度细化**（管道监听线程与调用线程并发已合测；**仅剩** asyncio × 管道监听线程，归 step 4 `daemon.py`）；⑥ **新增开口项 `MB-Q7`**（词表缺「命令未实现」码；本批**不动词表**）；⑦ 顺手修 §6.2 重复标题（`### 批 1` 出现两次）。**边界**：**生产路径零改动**（未接线；`webview_host` 原样在跑），`providers.json` 未改，`DevPlan.todo` id 200/201 仍 `done: false`；step 3~7 未开工 |
| 2026-10-03 | v15 | **step 3 落地：Pydoll 驱动最小集 + `M7B-04` 合测（§5 **7/9**）+ `VB2-30` 双证据** —— ① 新增 `source/python/brain_ai_browser/{browsers,runtime,driver}.py`：`M7B-14` 浏览器探测（标准路径**先 Chrome 后 Edge** / `env_proof`）· `M7B-13` 运行时检测（`check_runtime()` + `DependencyError` + 可操作引导）· **Pydoll 最小集**（`start` / `tab_for` / `new_tab` / `cookies_all` / `cookies_for_domain`（`P3` 按域过滤）/ `execute_script`（**单点解包两层 `result`**）/ `close_wait`（**超时只 warn、生产禁强杀**，`MB-D0-8` L1/L3））；pydoll **惰性 import**（`runtime.load_pydoll()` = 唯一入口）⇒ 无 pydoll 的系统 Python 下 `--selftest` / `--pipe-selftest` / `--serve` 照旧可跑（**7 / 0 · 13 / 0** 复核）；② `__main__.py` 新增 **`--driver-selftest [--headless] [--timeout <秒>]`**（组 A = 离线 `VB2-30`；组 B = 真机驱动断言 + `M7B-04`；`--headless` / `--timeout` **仅在**该开关下合法，否则 **exit 2**，**不静默忽略**）；③ **`M7B-04` ✅ 已过**（2026-10-03）：合测 = 管道伺服**独立线程**（实例先由主线程 `open()`）× asyncio 主循环并发跑「浏览器命令 burst」与 `asyncio.to_thread(pipe 往返)` ⇒ 往返落在命令窗口内**且返回时命令仍在跑** + 心跳 **12 次 / ≈0.66 s**（headless 轮 14 次 / ≈0.78 s）⇒ 事件循环未被管道读阻塞（**真并发**判据）；**生产形态（`daemon.py` 主循环 + 命令分发）在 step 4 复验**；④ **`VB2-30` 双证据**（`I21`）：**桩物证** = 组 A 打桩 `no_python` / `no_browser` 两条路径 → 可操作引导 + **启动器调用数 0** + **新建 socket 数 0**（「不开浏览器 / 不发 HTTP」机器判据）+ **真机物证** = **系统 Python 3.14.3（无 pydoll）** 实跑 `--driver-selftest` **11 / 0**（组 B **显式 SKIP** + exit 0，不静默、不假装通过）；⑤ **实现期订正**：初版把伺服线程放在组 B 开头 + `accept` 15 s ⇒ 被「起浏览器 6.6 s + 4 次 PowerShell 取证」挤爆、`accept` 超时**作废实例** → 客户端 `CreateFile` 报 `FILE_NOT_FOUND` 重试到超时（**表象与 step 2 的 `P7` 同类：双方都说「对方没听见」**，成因是**时序**而非句柄模式）；修法 = 实例先由主线程 `open()` + 伺服线程紧贴合测窗口启动（`accept` 30 s 兜底）+ 判据加「往返返回时命令仍在跑」；⑥ **新开口项 `MB-Q8`**（§14.2：词表缺「依赖细分」码 —— pydoll 缺失沿用 `no_python` + `hint` 显式说明；**本批不动词表**）。**实测（本机 · 2026-10-03）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy，与本轮无关）· `--driver-selftest` **有头窗口 22 / 0**（起 Edge pid=10448 · 启动 **6.56 s** · `execute_script` → **42** · `cookies_all` list · 真实命令行含 `--window-size` / `--user-data-dir` · `close_wait` **3.83 s / exit 0 / 无强杀** · 残留 **0** · 管道 `served=1`）· `--driver-selftest --headless` **22 / 0**（判据不依赖 headless）· 参数错误路径 4 条均 **exit 2** · `--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条**（official 9 / web 12）· `--run-selftest` **PASS** · `aiwrite.exe --pipe-selftest` **PASS**（step 2 回归）· docs 相对链接 **297 / 0** · 收尾**无残留进程**。**边界**：**生产路径零改动**（新通道未接线）· `source/assets/providers.json` 未改 · `session.py` / `daemon.py` / `redact.py` 未落地 ⇒ **`VB2-38` / `VB2-39` 尚未生效**；**step 4~7 未开工** |
| 2026-10-03 | v16 | **step 4 落地：守护进程主循环（`M7B-10`/`M7B-11`/`M7B-13`/`M7B-14` 收口）+ `VB2-38` Python 侧生效** —— ① 新增 `source/python/brain_ai_browser/daemon.py`（**唯一主循环**：管道**监听线程**（阻塞 `accept`/`read_line`）→ `call_soon_threadsafe` 投 `asyncio.Queue` → **主线程 asyncio** 跑 `handle_frame`（Pydoll 同循环）；**写帧一律 `asyncio.to_thread`**；收尾 = 关管道放监听线程出来 → `join` → 补关浏览器；浏览器**按需启动** ⇒ `hello`/`shutdown` 不需要浏览器，`aiwrite.exe --pipe-selftest` 仍秒级）+ `redact.py`（日志脱敏 `~/.brain-ai/logs/browser.log`，`log_event()` = **唯一写日志入口** ⇒「无明文」可被扫描断言）；`--serve` 从「最小伺服」**整体迁入** `daemon.serve()`（`__main__` 只留 CLI 与三层自检）；② **命令边界落定**：`hello` → `ready`（**依赖缺失时追加 `error` 事件**带可操作引导）/ `open_tab`（按需起浏览器 + `stage{open, ok}`）/ `login_state` → `login{state, cookie_names, has_expires, http_only}`（**值不进协议**；可选 `cookie_names`/`domain_suffix`，未给 = `unknown`）/ `shutdown`（`stage{close}` → `Browser.close` → 等进程退出 → **超时兜底强杀 + 必留 warn**）；`upload_image`/`send_prompt`/`read_answer` 仍 `err{daemon_down, hint=尚未实现}`；③ **`M7B-11` 关键行为**：**用户手动关窗 = 可恢复状态** —— `BrowserNotRunning` / 进程已死**被吞** + L4 留痕（`browser_selfheal`）+ **自愈重启**（实测：强杀浏览器后命令仍成功、pid 换新、**stderr 零回溯**）；L4 心跳（10 s）/ 崩溃检测 / `daemon-state.json` 留痕（未干净退出 → 下次启动打印 `⚠️ 上次未干净退出（登录态可能已回滚）`）；④ **`VB2-38`（`I23①`）Python 侧生效**：`daemon.close_verdict()` **7 条纯逻辑判据**（缺 `Browser.close` → 失败 / `BrowserNotRunning` → 正常收尾 / 超时无 warn → 失败 / 超时 + warn（±兜底强杀）→ 通过 / 证据自相矛盾 → 失败）+ 端到端物证（`verdict_ok=true`、`waited_s=2.16 s`、未强杀）；⑤ **实现期订正（口径）**：`stage` 帧**带请求 id = 完成回包**、`id="-"` 才是纯进度 ⇒ **C++ 侧 `PipeClient` 判据需同步**（否则 `open_tab` 的 `call()` 等不到回包），登记 **`MB-Q10`**；另暴露 **`MB-Q9`**（词表缺 `login` 事件名）；⑥ **新增 `--daemon-selftest`**（组 A = `VB2-38` 离线 7 条判据 + L4 判定 + 日志脱敏零命中；组 B = 真机端到端 11 条）。**实测（本机）**：`--daemon-selftest` **有头窗口 22 / 0**（合法帧 4 · 丢弃非法帧 0 · 浏览器启动 **2 次**（含自愈 **1 次**）· 退出码 **0** · `close_wait` **2.16 s / 未强杀** · 残留 **0** · stderr 无回溯）· `--daemon-selftest --headless` **22 / 0** · `--driver-selftest` **22 / 0**（回归）· `--selftest` **7 / 0** · `--pipe-selftest` **13 / 0**（系统 Python **3.14.3 无 pydoll → 14 / 0**，多出 1 条 = `ready` 后**追加 error 事件**的真机证据）· `aiwrite.exe --pipe-selftest` **PASS**（**新主循环**在 C++ 侧端到端可用、**未改一行 C++**）· `--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS** · docs 相对链接 **298 / 0** · 收尾**无残留进程**。**边界**：生产路径零改动 · C++ 侧关闭协议同步 / 诊断换代（`--pydoll-login`）/ 自启动与「一键重登」UI 归 step 5~6 · `session.py`（L2 快照）未落地 ⇒ **`VB2-39` 尚未生效**；**step 5~7 未开工** |
| 2026-10-03 | v17 | **目录迁移：Python 运行代码 `python/` → `source/python/`（代码归源码树 · 运行期数据仍归 `~/.brain-ai`；纯搬迁 · 零逻辑变更）** —— ① **迁移范围**：`brain_ai_browser/`（10 个模块）· `_probe/`（**70 文件**、含 45 个物证 JSON，`git mv` 保历史）· `requirements.txt` · `.venv/`（**移动后实测可用**：`sys.prefix` 自动跟随、`-m pip` 正常 ⇒ **未重建**）；② **C++ 同步（1 行逻辑 + 4 处注释）**：`src/main.cpp` 的 `--pipe-selftest` 包目录候选 `<仓库根>/python` → **`AIWRITE_SOURCE_DIR / "python"`**（`.venv` 候选随 `package_dir` 自动正确、`work_dir` 同步）+ `web/channel_frames.h` / `web/pipe_client.h` 注释；③ **口径同步（58 处文本 + 4 处代码文案）**：`.gitignore` 4 条规则 + 段注释 · `runtime.py` 的 **`PYDOLL_HINT`（用户可见引导）** · `driver.py` 文档串 · `requirements.txt` 头部 · docs **8 文件**（`M7B.md` / `CHANGELOG.md` / `M7.md` / `M_patchAB_rest.md` / `网页版协议实测记录.md` / `source/README.md` / `DevPlan.todo` / `providers.json`）；④ **历史物证文件（`_probe/out/*.json|*.err`）按「证据不可篡改」保留原路径**；⑤ **验收（逐条与迁移前一致）**：`--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · `--driver-selftest --headless` **22 / 0** · `--daemon-selftest --headless` **22 / 0** · `aiwrite.exe --pipe-selftest` **PASS** · `api_probe --selftest` **七组 PASS** / `--exec-selftest` **320 / 0** / `--graph-selftest` **110 / 0** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS** · 构建 **0 error / 0 warning** · docs 断链 **298 / 0**。**边界**：零逻辑变更 · `webview_host` 原样在跑 · **`session.py`（step 5 · `VB2-39`）未开工** |
| 2026-10-03 | v18 | **step 5 落地：L2 加密快照 `session.py` + `VB2-39` Python 侧生效** —— ① 新增 `source/python/brain_ai_browser/session.py`：**DPAPI** 经 `ctypes` 直调 `crypt32`（`CRYPTPROTECT_UI_FORBIDDEN`，**不引第三方依赖**）；数据落 **`~/.brain-ai/session/cookies.dat`**（**代码入库 / 数据不入库**；`BRAIN_AI_SESSION_DIR` 仅作测试钩子 ⇒ 自检不污染真实快照）；纯函数 `host_of` / `belongs_to`（**父域方向**）/ `scoped` / `to_cdp_params`（白名单 + 会期语义）/ `usable_cookies` / `scan_plaintext` / `summarize`；`SnapshotStore.save/load/clear`（**原子写** `.tmp` + `os.replace` + **明文摘要去重**）；② **daemon 三触发点 + 回灌**：`open_tab` 登记作用域 → `login_state=logged_in` **登录即写** → `shutdown` 在 `Browser.close` **之前**再写 → 空闲 / 命令间隙每 **5 s** 刷新（`wait_for(queue.get(), 5)`，与命令**同一任务** ⇒ 不并发访问 CDP）→ 启动 / 自愈后**回灌一次**（失败发 `error{not_logged_in}` **显式提示**，`I23④`，**不影响命令回包**）；`daemon-state.json` 增 `snapshot_saves` / `snapshot_restores` / `snapshot_reason`；③ **订正（`M7B-17` 口径）**：`driver.cookies_all()` 由**页级** `Network.getCookies` 改为**浏览器级 `Storage.getCookies`**（页级依赖当前页、**会漏父域登录 Cookie**），新增 `set_cookies` / `delete_all_cookies`；④ **实现期订正三处**：a) `gone_exceptions()` 纳入内建 **`ConnectionError`**（浏览器被杀后下一条命令先撞「连接被拒绝」`[WinError 1225]` ⇒ 漏掉它**丢失自愈**）；b) DPAPI blob **自带完整性校验**（截断 / 改头部 / 改中段 / 改末尾 / 明文冒充 → 错误码 13），**例外面 = 头部明文「描述区」**（改它不影响解密）⇒ 篡改断言取**中段 / 末尾**；c) **close 前刚写过 Cookie** 时 Chrome 退出可能 > 5 s（flush cookie 库）⇒ 自检改用生产同款 `close_verdict`；⑤ **新开口项 `MB-Q11`**（作用域默认 = **本次会话导航过的域** + **空作用域不写快照**，隐私最小）；⑥ **新增 `--session-selftest [--headless]`**（组 A 离线 8 条 + 组 B 真机 5 条），并把 `--daemon-selftest` 补到 **23 条**（`+M7B-11⑭` L2 触发点非死码）。**实测（本机 · 2026-10-03）**：`--session-selftest --headless` **22 / 0**；**系统 Python 3.14.3（无 pydoll）→ 15 / 0 + 组 B 显式 SKIP + exit 0**；`--daemon-selftest --headless` **23 / 0**；`--driver-selftest` **22 / 0** · `--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · `aiwrite.exe --pipe-selftest` **PASS** · `api_probe` 七组 / **320 / 0** / **110 / 0** · `aiwrite --provider-selftest` **50 / 0** · `~/.brain-ai/session/` **未被自检创建** · 收尾**无残留进程**。**边界**：**C++ 侧零改动**（`pydoll_channel` / `session_snapshot` / `SessionStore` 改造 + `PipeClient` 进度事件判据同步 + 诊断换代归 **step 6**）；`upload_image` / `send_prompt` / `read_answer` 仍未实现（批 3） |
| 2026-10-03 | v19 | **step 6 落地：C++ 侧接线（站点描述搬迁 + 快照只读视图 + 新通道 `pydoll_channel` + `PipeClient` 判据同步）** —— ① **新增 `src/web/site_ref.h`**：`SiteRef` = `LoginRequest` **纯别名**，全部站点纯函数（`login_request_of` / `interactive_login_request` / `probe_login_request` / `boot_login_request` / `plan_session_boot` / `login_request_site`）**整体搬迁**；`webview_host.h` 改为 include 它 ⇒ 新通道取站点描述**不再拉入 WebView2 依赖**、现有调用点**零改动**（守 `I2`）；② **新增 `src/web/session_snapshot.{h,cpp}`**（`MB-D0-8` L2 的 C++ **只读视图**：路径 / 触发时机 / 状态文案；**只碰元数据** `exists` / `file_size` / `last_write_time`，**永不读内容** ⇒「导出物零明文」在本层天然成立 · `VB2-39⑥`）；③ **新增 `src/web/pydoll_channel.{h,cpp}`**（`namespace web::channel`：**`selftest` / `pydoll_login` 落地**；`ensure_session` 只读判定（`I15′`）；`run_script` / `logout_site` / `current_tab_site` / `tab_on_site` **如实回「尚未实现（批 3）」**）；④ **`PipeClient` 判据同步（关闭 `MB-Q10`）**：`stage` 帧**带请求 id** = 该命令的完成回包，只有 `id="-"` 才是纯进度事件；⑤ **改动（只加不替换 · `B12-C1`）**：`paths` +`pydoll_profile` / `session_snapshot_dir`；`provider_spec`：`allowed_web_fields()` +`attach` + 解析（**只解析、不消费** · `B12-C2`）；`session_store.h` 注释（写入方 = CDP 快照 · 持久化归 L2）；`CMakeLists.txt` +3 组源 + **`aiwrite_copy_python()`**（**`.venv` 排除**，实测 10 个 `.py` 就位）；`main.cpp` +`--pydoll-selftest` / `--pydoll-login <id>`；`tools/api_probe.cpp` **只新增**断言块；⑥ **实现期订正（真因）**：`find_package_dir()` 必须**优先「包目录 + 同级 `.venv/Scripts/python.exe`」** —— 否则选中 `<exe>/python` 的拷贝、退回 PATH 上的 `python.exe`（本机 3.14.3 **无 pydoll**）⇒ 生产登录必失败。**实测（本机）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy）· `api_probe --exec-selftest` **320 → 326 / 0**（`VB2-40①~⑤` + `VB2-39⑥`，**只升不降**）· `--selftest` 七组 PASS · `--graph-selftest` **110 / 0** · `aiwrite --pydoll-selftest` **exit 0**（`ready{proto=1, python=3.12.10, browser=edge}` → 守护进程退出码 **0**）· `aiwrite.exe --pipe-selftest` **PASS** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS**（**生产路径仍走 WebView2**）· `--pydoll-login <未知 id>` **exit 2 + 可操作提示、不开窗**。**边界**：`--pydoll-login` 的**真站点人工登录冒烟**待人工执行（`M7B-19` 门槛项）；生产路径切换（`M7B-20`）/ 诊断换代（`M7B-18`）/ 批 3 命令归 **step 7** |
| 2026-10-03 | v20 | **§1.4 运行时通道归属盘点（防误判 · 纯文档，无代码改动）** —— 起因：用户运行主程序发现「还是 WebView」，怀疑 step 6 未生效。逐入口 `git grep` 实测确认：**生产路径 100% 仍是 WebView2**（`property_panel.cpp:14/116/230/319` · `local_nodes.cpp:14/545` · `dom_web_client.cpp:12/245/256/262/297`）；`pydoll_channel` 仅被 `main.cpp:1523-1530`（CLI 分发）与 `api_probe.cpp:3285+`（断言）引用 ⇒ **符合 `MB-D1` / `B12-C1` 设计，非缺陷、非回归**。新增 **§1.4**：逐入口归属表（含「切换任务」列）+ **10 秒分辨法**（profile 目录 / 独立任务栏窗口 / `pydoll_channel_daemon.log`；⚠️ **Pydoll 底层同为 Edge，窗口长相不可靠**）+ 三条常见误判 + 硬缺口表（Python 仅 4 命令 `daemon.py:363,382,384`；C++ `logout_site`/`run_script`/`current_tab_site`/`tab_on_site` 占位 `pydoll_channel.h:39,42,46,47`；CDP 增量未做 `M7B-21`；`M7B-19` 未过）⇒ **`M7B-20` 切换的充分条件 = 批 3 能力先落地**；§0 加防误判第 **7** 条。**`source/README.md`**：§4.1 / §4.3 标题口径更新（「计划未落地」→「**批 1 step 1~6 已落地 / 生产路径未切换**」）+ 新增「⚠️ 当前运行时通道归属」小节 + 命令清单补 `--pydoll-selftest` / `--pydoll-login` 两行 + `--exec-selftest` **251 → 326 项**。**`docs/README.md`** 进度段加运行时通道警示。校验：docs+source 相对链接 **320 / 0 broken** |
| 2026-10-03 | v21 | **step 7 开工：`M7B-19` 全基线（12 / 13 绿）+ `M7B-18` 受阻登记 + 订正一处换机漏项（纯文档，零代码改动）** —— ① **`M7B-19` 全基线实测（本机）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy）· `--selftest` 七组 PASS · `--exec-selftest` **326 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--pipe-selftest` **PASS** · `--pydoll-selftest` **exit 0**（`proto=1` / `python=3.12.10` / `browser=edge`）· `--pydoll-login <未知 id>` **exit 2** · `--run-selftest --web` **PASS** · `--session-selftest` **22 / 0 · RC=0** · docs+source 相对链接 **320 / 0 broken**；② **唯一偏差（如实登记）**：`--run-selftest`（**official**）**FAIL · exit 1** = 本机**缺凭据**（未设 `DEEPSEEK_API_KEY`、凭据库无 `config.toml` 的 `api_key_ref = brain-ai/deepseek` 条目）⇒ `n3 LLMGenerate error → n4 skipped → PC-05 归档失败`；**判定 = 环境依赖、非代码回归**（报错路径 `engine/provider_resolve.cpp:291-295` **未被 step 1~6 触碰**；`--run-selftest --web` **PASS** ⇒ 执行器链路完好）；③ **订正 v13 的一处漏项**：§11.1 的「`--run-selftest` PASS / PASS（5-5）」是 **2026-09-28 旧机**记录（`F:\Python` + Chrome 156），v13 的「批 0 复测」**误当换机后结论照抄** ⇒ §11.1 已改标「**该条依赖本机凭据，换机 / 重装后必须重配，否则必红**」（§9.3「换机后必须重取基线」在此**抓到一处漏项**）；④ **`M7B-18` 受阻如实登记（未擅自扩契约）**：`--web-adapter-selftest` / `--web-dom-dump` 走新通道需「**页面内执行任意 JS**」，而**词表 v1（`protocol.py:42-50`）只有 7 个命令、无此命令**；扩表按 §6.1 冻结规则 = **升 `v`**（牵动两侧 + `ready{proto}` + `VB2-40④⑤`）；且与 `B12-C1`「**批 2 只新增不替换**」**口径冲突** ⇒ 三路径待拍板：**(a)** 扩 `v2` + `M7B-18` 移批 3 / **(b)** 诊断改走**不经管道**的 Python CLI / **(c)** 保留 WebView2 诊断至批 5；⑤ **生产路径切换（`M7B-20`）明确不做** —— 归批 3，前置缺口实测仍在（`daemon.py` 仅 4 命令 · C++ 4 个占位）；⑥ **打标签 `m7b-batch2` 未执行** —— 工作区 **118 文件未提交**，须先提交。**文档落点**：`M7B.md` §6.2 step 7 记录块 + `M7B-18` / `M7B-19` 状态行 + 开工顺序进度段 + §11.1 + 本表；`CHANGELOG.md` step 7 条目；`source/README.md` 命令清单加「`--run-selftest` 依赖本机凭据」警示 |






**站点选择器回填记录**（格式见 §10；`M7B-06` / `M7B-27` / `M7B-28` / `M7B-29` 执行时逐行追加）：

| 日期 | 条目 | input | send | answer | done_when | cookie_names | 备注 |
|---|---|---|---|---|---|---|---|
| （待填） | | | | | | | |







