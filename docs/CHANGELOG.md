# Changelog

本文件记录 AIwrite 的全部重要变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循语义化版本 `MAJOR.MINOR`（设计文档 T-15）。

**相关文档索引**

| 文档 | 说明 |
|---|---|
| [ai_writer_nodes.md](ai_writer_nodes.md) | 项目设计文档（v1.0） |
| [actionPlan/milestone_plan.md](actionPlan/milestone_plan.md) | 里程碑总体计划（M1–M6 + **M7**） |
| [roadmap.md](roadmap.md) | **长期路线图 v1.0（一页纸）**：定位 / 阶段总览 / 决策 `RM-D1`~`RM-D6` / 风险与不做清单 / 待确认项与"待同步差异" |
| [actionPlan/M7.md](actionPlan/M7.md) | **M7 现行计划（两轮）**：**第一轮**（已收口）= 图片输入收口 + 节点精简（删图片输出节点 9→8）+ 图片格式按内容嗅探/双解码后端 + **CRT 断言崩溃根因与冻结区 `I17`**；**第二轮 P7**（§8–§17，计划中）= **P7-a v0.5.2**（图片内部运行：`image` 端口 variadic 多图 + 统一资源目录；official 收口；UI **A 档**）+ **P7-b v0.5.3**（待确认；网页版图片上传 = Pydoll 独立浏览器 + Python 守护进程 + 命名管道 + Win32 事件，先做前置技术验证）+ 不变量 `I18`/`I19` |
| [actionPlan/](actionPlan/) · [Archive/actionPlan/](Archive/actionPlan/) | 各里程碑 Action Plan（进行中 / 已完成归档） |
| [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) | **补丁系列剩余工作总集**（Patch A/B 残项 + 数据安全 + 交互打磨 + 并行小项 + `B2` 残项）—— **后续唯一执行入口**；含现行规格（附录 B `JSON` 规范 / C 加载顺序 / D 自建站点 / E 站点清单两列）与待拍板决策 |
| [Archive/actionPlan/M_patchA.md](Archive/actionPlan/M_patchA.md) · [Archive/actionPlan/M_patchB.md](Archive/actionPlan/M_patchB.md) | **已完成部分已归档（2026-09-27）**：Patch A（结果回流与可观测性）· `M_patchB`（Provider 可插拔化：**JSON 配置表** + **API 与网页版同表同机制** + L1 收口 + L3 DOM 站点适配器 + L4 部分落地）—— 历史规格与各批次实测记录 |
| [DevPlan.todo](DevPlan.todo) | **开发计划看板**：TodoList 格式（根键 `todotree`），Debug / Feature / Test / Docs / Archive 五类 × M1–M7 分层，每条含一行描述与 `fileLink` 文档链接 |
| [Archive/M1_技术验证报告.md](Archive/M1_技术验证报告.md) | M1 实测环境、验证结果与问题记录（已归档） |
| [Archive/README.md](Archive/README.md) | 归档索引与归档规则 |
| [../source/README.md](../source/README.md) | 源码构建 / 运行 / 调试说明 |

---

## [Unreleased] — M7（图片收口 + 节点精简）+ M5 核心切片（M5-C）已落地；**M7 第二轮 P7-a 已落地（v0.5.2）**；M7 第三轮 `M7B` 立项（网页通道整体迁 Pydoll）

**🧭 M7B 批 3 · step 15 落地（`M7B-56` · P4）：生产内容路径切换 + 静默截断修复 + 新 CLI `--pydoll-chat-selftest`（2026-10-05）**

- **① 生产路径切换（`ai/dom_web_client.cpp::dom_chat`）**：注入 / 发送 / 取回答的**三段 `run_script`**（配置注入 + kickoff + 轮询 poll）**全部换成 v4 协议命令** —— `channel::send_prompt`（`input_selector` 单元素数组 · `send{kind,value}` 来自条目 `web` 段 · `upload_evidence=false` = 纯文本**不置** `I18` 位）+ `channel::read_answer`（`answer_selector` · `done_when` · `poll_ms`/`max_polls`（`clamp_poll_params` 钳制）· 总超时）⇒ **生产路径再无 `run_script`**（`VB2-22` 口径就地更新为「**纯函数不变 + 生产不再使用**」；DOM 脚本常量保留为**诊断资产**）。`DomChatResult.polls` 恒 0（轮询在 Python 侧 · 协议不回传次数）⇒ `local_nodes` 日志改「输出 N 字节 / X ms」。
- **② 静默截断修复（`I21` 同族）**：`run_script` **成功但截断**（> 48 KiB）时用 `*error` 给说明 —— 原实现**只在失败路径**使用它 ⇒ 截断被**静默吞掉**（`raw` 是前缀 → JSON 解析失败 → `catch/continue`）。修法：两处诊断调用点（`dom_selector_dump` / `dom_adapter_selftest`，后者把 `||` 短路**拆成两段**）改为**成功路径也提示**「警告：已截断…」；生产侧由 `read_answer.truncated` 出参 ⇒ `result.warning`。
- **③ 新 CLI `--pydoll-chat-selftest`（跨语言端到端 · 本地 `file:///` 夹具 · 零外网零登录）**：`main.cpp` CLI 分发 → `web::channel::chat_selftest()` —— 起守护进程（独立管道 · `--once`）→ `hello`（`proto=4`）→ `open_tab`（输入框 + 发送按钮 + 回答容器的夹具）→ **`send_prompt`**（真打字 + 点击发送）→ 逐字符自证（`run_script` 只读 `window.__inputs`）→ **`read_answer`**（`answer_done` 与夹具渲染**字节一致** · `truncated=false`）→ `I18` 拦截（`upload_evidence=true` 无证据 → `no_upload_evidence`）→ `upload_image` 缺 `attach_selector` → `attach_unsupported` → `shutdown`（**不立刻 close**）。实测 **6 / 0 · exit 0**（`proto=4` · 守护进程退出码 0）。
- **回归（本机 · 构建 0 error / 0 warning**，唯一告警 = **既有** `brotlienc.dll` copy**）**：`api_probe --selftest` **335 / 0** · `--graph-selftest` **110 / 0** · `--pipe-selftest` **PASS**（`proto=4`）· **`--pydoll-chat-selftest` 6 / 0 · exit 0** · `--pydoll-script-selftest` **4 / 0** · Python `--selftest` **16 / 0** · 探针 2/2 PASS · 残留进程 **0**。
- **边界（如实 · 下一步）**：**P5**（`P7b-16` 网页版图片理解解禁）**未做**；真站点**端到端生成**仍是人工关卡（需人工登录 + 选择器回填）；**`M7B-24`（多候选数组）未做** ⇒ 生产只下发**单元素**选择器；`stream_deltas`（CDP 增量 · `M7B-21`）未做 ⇒ 仍为**轮询式**（按 `I22` 标注「非流式」）。

**🧭 M7B 批 3 · step 14 落地：内容返回正式化（**词表 v4**）—— `send_prompt` / `read_answer` / `upload_image` 三命令 + 两侧校验 + 端到端探针（2026-10-05）**

- **起因（设计缺口，见 `M7B.md` §6.1 v4 设计定稿）**：批 3 的「网页内容返回」（注入 → 发送 → 取回答正文）此前**只走 `run_script`**（C++ 下发 3 段 DOM 脚本），而词表里的 `send_prompt` / `read_answer` / `upload_image` **一直是桩**（`err{not_implemented}`）⇒ ① 三命令**无主任务**（编号被 dom_chat 换代 / delta / 失效 / CLI 占用）；② 与 `I14`（选择器只在调用方）**字面冲突**（`send_prompt{provider,prompt}` 无选择器字段）；③ **长文本无分片 / 截断契约**（`answer_done{text}` 无上限而单帧 ≤ 64 KiB）。**用户 2026-10-05 拍板：正式化协议命令**（`run_script` 降级为诊断专用）。
- **词表 v4（两侧同批 · **只加字段、不加名字**）**：`PROTO_VERSION 3→4` ↔ `kProtoVersion 3→4`；**10 命令 / 8 事件 / 错误码均不变**；新增**站字段组**（`input_selector[]` · `send{kind,value}` · `answer_selector[]` · `done_when{kind,selector}` · `poll_ms` · `max_polls` · `attach_selector`）+ `send_prompt.upload_evidence`（**`I18` 按位开关**）+ `answer_done.{text_bytes, truncated}`（**显式截断**）；必需字段收紧（`send_prompt` = provider+prompt+input_selector+send、`read_answer` = provider+answer_selector）；站字段组校验 `station_fields_reason` **两侧逐条同构**；`not_implemented` 码保留但**当前无使用点**。
- **Python 侧**：`driver.py` 新增内容返回基建（**只新增**）—— `selector_facts` / `pick_visible`（多候选，首个「命中且可见」者胜 · `M7B-24`）/ `type_humanized`（**pydoll 原生真打字**）/ `press_key`（`Key` 枚举）/ `click_selector`（`user_gesture`）/ `set_file_input_files`（`DOM.setFileInputFiles`）/ `inject_files_via_chooser`（备选）/ `read_answer_text` + 纯函数 `pick_visible_index` / `answer_payload` / `done_hit`（+ `DriverContentError`）；`daemon.py` 新增 `_cmd_upload_image`（`attach=none` / 缺 `attach_selector` → 可操作拒绝；成功 → `evidence{page, network:[], both:false}` + **按 provider + 会话代数**记账上传证据）/ `_cmd_send_prompt`（**`upload_evidence=true` 且无证据 → `err{no_upload_evidence}`** = `I18` 协议级拦截；真打字 → 按 `send` 触发）/ `_cmd_read_answer`（`answer_done{...}`；未取到 → `err{send_timeout}`）；`logout_site` 成功后**清该 provider 证据**。
- **C++ 侧**：`pydoll_channel.{h,cpp}` 新增 `upload_image` / `send_prompt` / `read_answer`（**阻塞 · 仅后台线程 / CLI** · `Q4`；**不自动起浏览器**；缺参 → `false` + 可操作原因 · `I21`；截断时仍 `true` 但 `*error` 给说明）；`channel_frames.{h,cpp}` `kProtoVersion=4` + 站字段组校验。
- **断言 / 探针**：`api_probe --selftest` **332 → 335 / 0**（+`VB2-44⑤⑥⑦` 与 `VB2-43①` 就地改「≥v3」）；Python `--selftest` **12 → 16 / 0**（+`VB2-44①②③④`）；**新增探针 `source/python/_probe/m7b54_v4_content_probe.py`**（本地 `file:///` 夹具端到端 · 零外网零登录 ⇒ **PASS 11 / 0**：`I18` 拦截 / `attach_unsupported` / **真打字 `stage{send,ok}`** / **逐字符自证**（`input` 事件 > 0）/ `answer_done{text='答：v4-content-ok'}` / 选择器未命中 → `send_timeout`）；既有探针 `m7b20b_v3_commands_probe.py` **升 v4**（**PASS 8 / 0**）。
- **回归（本机 · 构建 0 error / 0 warning**，唯一告警 = **既有** `brotlienc.dll` copy**）**：`api_probe --selftest` **335 / 0** · `--graph-selftest` **110 / 0** · `[配置表自检]` **50 / 0** · Python `--selftest` **16 / 0** · `--pipe-selftest` **22 / 0** · `--daemon-selftest --headless` **34 / 0** · `aiwrite --pipe-selftest` **PASS**（`proto=4`）· `--pydoll-script-selftest` **4 / 0**（`proto=4`）· 探针 2/2 PASS · 残留进程 **0**。
- **边界（如实 · 下一步）**：**P4**（`dom_chat` 生产切换 + 修「`run_script` 成功但截断被静默」）与 **P5**（`P7b-16` 图片理解解禁）**未做**；`upload_image` 的**网络回执证据**（`both=true`）归 **`P7b-11`**（当前如实 `both=false`）；`stream_deltas`（CDP 增量 · `M7B-21`）未做 ⇒ 内容返回为**轮询式**（`I22` 标注「非流式」）。

**🧭 M7B 批 3 · step 13 落地：`M7B-45`~`M7B-47` 渲染路径去同步 IPC（修「登录后界面卡死」）+ 「渲染路径零 IPC」护栏 + 会话守护进程残留事实（2026-10-04）**

- **起因（用户实测报的真 bug）**：点「打开登录窗口」后**界面卡死**（帧率 2–10 fps）。
- **根因（物证链）**：`app.log` 的 `current_tab` 在 20:25–20:31 每分钟 **363 / 588 / 456 / 108 / 0 / 0 / 342**（全时段 **2597** 条），单次 `tab_on_site()` **30–140 ms**（最坏 **2.1 s**），守护进程 `served=343`。机制 = `ui/property_panel.cpp` 的 `draw_web_session_section()` 在**每帧渲染**里调 `web::channel::tab_on_site()` ⇒ `current_tab_site()` = **同步 IPC**（**新建管道连接** 2 s 超时 + `current_tab` 8 s 超时），被 `channel_alive = session_ready()` 门控 ⇒ 点「打开登录窗口」起**每帧真打 IPC**。
- **换代表错映射（教训）**：HEAD 的 `current_window_site()` 是**本地读**（`webview_host.cpp` 读本进程 `g_window`，纳秒级 / **非阻塞** / 任意位置可调），step 11 换成 `tab_on_site()`（**真远程阻塞读**）时只对齐了签名、没标「阻塞性 / 允许调用位置」⇒ 本次给 `M7B.md §6.2` 接口映射表**补附表**。
- **UI 侧（`M7B-45`）**：渲染路径**零 IPC** —— 「本节点站点 tab」改读**本节点缓存** `web_task_view(node.id)`（微秒级），未观测时**如实**显示「未观测（点右侧「刷新」）」（`I21`：不假装「不在当前 tab」）；新增 `WebTaskKind::Tab` + 「刷新」按钮，观测**只在后台线程**做（`Q4`），结果写 `web_task_set_tab()`（含时间戳）；`any_web_task_running()` **跳过 `Tab`**（只读观测不构成进程级禁用理由 · `M7B-44`）。
- **护栏（`M7B-46`）**：`web/pipe_client.{h,cpp}` 在**全仓 IPC 收口点**（`connect` / `call` / `send_command`）加 `ipc_connect_count()` / `ipc_command_count()`（全局 · 诊断）与 `ipc_thread_connect_count()` / `ipc_thread_command_count()`（**本线程 · 护栏判据** —— 后台线程并发发 IPC 会改动全局计数 ⇒ 用全局计数会**误报**）；`ui/app.cpp` 帧循环在 `draw_property_panel()` **前后**取**本线程**差值断言 ⇒ 非 0 即 `log::warn`（前 5 次）+ **状态栏红字**。渲染路径 IPC 清单已核对：只剩 `session_ready()`（纯本地查询），其余阻塞接口全在 `WebTask` 后台线程体内。
- **残留事实（`M7B-47`）**：会话守护进程以 `--serve --idle-timeout 600` 拉起 ⇒ App 强杀后孤儿（守护进程 + 浏览器）**最多再存活 10 分钟**（**不是** 30 s 默认值）；本机实测残留 **0**（上次的 `pid 15332` + Edge `20556` 已由 idle 自清）。「父进程存活检测」记为下一批候选。
- **回归（本机 · 构建 0 error / 0 warning）**：`api_probe --selftest` **332 / 0** · `--graph-selftest` **110 / 0** · Python `--selftest` **12 / 0** · `--daemon-selftest --headless` **30 / 0** · `--pipe-selftest` **PASS**（`proto=3`）· 残留进程 **0**。同一条自检新增一行实测输出 `[管道自检] IPC 计数：连接=1 命令=2`（证明护栏计数**在动**，不是死码）。**词表仍 v3**（本步不动命令 / 事件 / 错误码）。
- **未含**：事件驱动改造（宿主主动推事件 + 中枢常连接 + 订阅基线 + 面板事件化 + 事件洪水四层背压）留待下一批 S1–S4。
- **✅ 验收（用户实测 · 2026-10-05）**：**Gate-S0 通过** —— 登录后界面卡死已消除（上述 5 条验收全过：不卡 / 渲染路径零 IPC / 护栏自证 / 未观测如实显示 / 残留 0 + 构建 0/0）。**无代码改动**（纯留痕）；词表仍 v3；提交时机由用户决定。
- **📐 设计定稿（下一步 · 2026-10-05）**：批 3「**网页内容返回**」正式化 = 词表 **v4** —— `send_prompt` / `read_answer` 增**站字段组**（`input_selector[]` / `send{}` / `answer_selector[]` / `done_when{}` / `poll_ms` / `max_polls`）、`send_prompt.upload_evidence` **按位开关**（`I18` 只在有图时拦截）、`answer_done.{text_bytes,truncated}`（长文本显式截断 + `delta` 分片）、**`run_script` 降级为诊断专用**；含图片链路 `P7b-16` 解禁与 `dom_chat` **静默截断**修复。施工图（P1~P6）+ 任务 `M7B-54`~`M7B-56` 见 [actionPlan/M7B.md §6.3](actionPlan/M7B.md)（**待落地**）。


**🧭 M7B 批 3 · step 12 落地：`M7B-44` 登录轮询收口（`login_state` 纯观测 · 不自愈）+ 面板「按节点记账」（修用户实测的「一次点击弹多个窗口」）（2026-10-04）**

- **起因（用户实测报的真 bug）**：点一次「打开登录窗口」，Pydoll **弹出多个窗口**。物证（本机 `~/.brain-ai/logs/browser.log` + `daemon-state.json`）：**`open_tab` 只有 1 条**，但 `browser_start` **4 次** / `browser_selfheal` **4 次**（`session_starts=4 / self_heals=4`），末次还 `FailedToStartBrowser`。
- **根因**：`login_site` 每 1.5 s 发一条 `login_state`（上限 300 s），而守护进程侧 `_cmd_login_state` 走 `_ensure_browser_restored` ⇒ **每一轮轮询都「浏览器不在 → 自愈重启」** ⇒ 用户**关掉窗口后每一轮都重开一个新窗口**。放大器：`doubao-web` 条目缺 `web.cookie_names` ⇒ 判据恒 `unknown`（`I14` 的正确行为）⇒ 必然跑满 300 s。**修法不是调参，而是「观测命令不得有副作用」**。
- **Python 侧**：`login_state` 改**纯观测 · 不自愈**（新增 `_browser_no_restart()` / `_read_op()`：浏览器不在 → 如实回 `err{daemon_down}` + 可操作 hint，**绝不重启、也不做 L2 回灌**）；`M7B-11` 的自愈对**生产命令**（`open_tab` / `run_script` / `logout_site` / `current_tab`）**保留不变**；**词表仍 v3**（无命令/字段变更 ⇒ 不升版本、无两侧错配）。
- **C++ 通道（`web/pydoll_channel.{h,cpp}`）**：`login_site` **观测失败即结束**（旧实现无视错误继续空转 —— 正是多窗口来源）；新增 `request_cancel_session_ops()`（退出时由 `ui::wait_web_tasks()` 置位 ⇒ 消掉关窗后死等 3 分钟）；`cookie_names` 为空 → **立刻如实回报「无法自动判定」**（不再空转；窗口照旧打开）；面板登录上限 `300 s → 120 s`。
- **UI「按节点记账」（`ui/property_panel.cpp` · 本轮用户点名的第一要求）**：`WebTask` 由**进程级单例**改为**按 `node.id` 记账的注册表** —— 状态 / 去重 / 禁用**都只作用于本节点**（不再「A 节点点的登录显示在 B 节点上」、不再「A 在跑就静默吞掉 B 的点击」）；进程级事实（浏览器会话在不在跑 · 「关闭浏览器会话」）**如实标注归属**并写出「在等谁」；`wait_web_tasks()` 遍历所有节点 join。
- **断言（新增）**：`--daemon-selftest` **30 / 0**（+`M7B-44①②`：杀掉浏览器后 `login_state` 回 `err{daemon_down}` 且 `browser_start` **不增**；`open_tab` 的自愈仍生效）。
- **端到端探针（新增 · 回归防线）**：`source/python/_probe/m7b44_login_poll_no_restart_probe.py`（**零外网零登录**）—— 本机 **10 / 10 PASS**。⚠️ 附带发现：`subprocess.Popen(...).pid` **不是**守护进程的 pid（venv 的 `python.exe` 是转发器，实测 `21844` vs 子进程 `13492`）⇒ 判据只能取日志 `daemon_start.pid`。
- **回归（本机）**：构建 **0 error / 0 warning** · `api_probe --selftest` **332 / 0** · `--graph-selftest` **110 / 0** · Python `--selftest` **12 / 0** · `--pipe-selftest` **PASS**（`proto=3`）· 既有探针 `m7b20b_v3_commands_probe.py` **8 / 8** · 残留进程 0。
- **边界（下一步）**：批 4（`M7B-26`~`M7B-29`）的 `cookie_names` / `answer_selector` 回填**未做** ⇒ 多数登录型条目仍「能登录、**不能**自动判定」（本步已把这点变成**明说**而非空转）；GUI **真站点人工登录**仍是人工关卡（`M7B-19`）。

**🧭 M7B 批 3 · step 11 落地：`M7B-20b` 会话族换代 —— 词表 v3 + 面板登录走新通道（闭合 step 10 的「无法登录」断点）（2026-10-04）**

- **起因（本步驱动力）**：step 10 把网页版文字生成切到新通道后，**用户路径断了** —— 面板唯一的登录入口仍是 WebView2 内嵌窗口（登 `~/.brain-ai/webview2/`），而 `dom_chat` 读 `~/.brain-ai/pydoll-profile/` ⇒ **点面板登录也无效**，GUI 里跑生成必然「未登录」，唯一出路是命令行 `--pydoll-login`。本步闭合该断点。
- **词表 v3（两侧同步）**：+ 命令 `logout_site`（按站点注销 · `Storage.clearDataForOrigin`）/ `current_tab`、+ 事件 `tab`（`url` / `site`）、+ 错误码 `not_implemented`（**闭合开口项 `MB-Q7`** —— 不再借 `daemon_down`）。C++ `channel_frames.{h,cpp}`：`kProtoVersion 2→3`、命令 **8→10**、事件 **7→8**；Python `protocol.py` / `__init__.py`：`PROTO_VERSION=3`。**实测**：`--pipe-selftest` / `--pydoll-script-selftest` 的 `ready{proto}` = **3**（「不匹配即拒」自动生效）。
- **Python 侧**：`driver.py` + `clear_origin_data(origin)`（`Storage.clearDataForOrigin` · 浏览器级 CDP，与 `close_wait` 同款）+ `current_tab_url()`（**属性 / 协程两形态统一**，读不到回空串）；`daemon.py` + `_cmd_logout_site`（**`origin` 必须由调用方下发** · `I14` · 成功后**同步重写 L2 快照**，否则下次回灌会把登录态带回来）+ `_cmd_current_tab` + `site_key_of()`（与 C++ **逐字同构**）。
- **C++ 通道**：`logout_site` / `current_tab_site` / `tab_on_site` 由**如实占位换真实现**；**新增 `login_site(site, timeout_s)`**（确保会话 → `open_tab` → 轮询 `login_state` → 写内存会话 → **保持会话 · 不 `shutdown`**）—— 与 CLI `pydoll_login` 的**唯一差别**是登录后不关会话（浏览器继续给网页节点复用）；抽出 `ensure_daemon_session` / `remember_session` / `query_login_state` 三个 helper（**等价抽取**；`ensure_session` 改用它）。
- **UI 换代（`ui/property_panel.cpp` · 本步最大工程约束）**：新增 **`WebTask` 后台任务执行器** —— 登录 / 收尾 / 注销**一律后台跑**（`Q4`：新通道函数**全阻塞**，UI 线程调会卡死），UI 只读**加锁缓存**；同一时刻只跑一个任务（连点 → **明确忽略 + 记日志**）；任务体统一 `try/catch` 兜底；新增 `ui::wait_web_tasks()` 由 `main.cpp` **退出收尾前** join（同时避免与主线程收尾并发操作同一条管道）。按钮：「打开登录窗口（**Pydoll**）」/「关闭浏览器会话」；「注销该站点」→ `channel::logout_site`；「删除整个 profile」→ 路径改 **`paths::pydoll_profile()`**（**不再删 `webview2`**，旧目录退役归批 5 · 不自动删用户数据）；「探测网页版协议（dev）」**保留旧通道**（协议栈能力，批 5 随其退役）。
- **`dom_chat` 会话改有头**：新增 `web::visible_login_request()`（`offscreen=false`）—— 新通道浏览器是用户**唯一登录入口**，离屏则「看不见 ⇒ 无法登录」；`boot_login_request`（离屏）**保留**给旧协议栈。
- **⚠️ 实测踩坑（端到端验证抓到真 bug）**：`Storage.clearDataForOrigin` **只能经 tab（页）连接下发** —— 走**浏览器级**连接时 Chromium 对 `storageTypes` 的**任何取值**（`all` / `cookies` / `local_storage` / `cookies,local_storage`）一律回 `Internal error (code -32603)`；改走 tab 连接后**四种取值全部 OK**。**反直觉**：同一 `Storage` 域名内 `Storage.getCookies` / `Browser.close` 恰恰**必须**走浏览器级 ⇒ **传输层要求按命令而异，不能照抄同域邻居**。该 bug 只有真发一条 `logout_site` 才暴露（离线断言覆盖不到）⇒ 专门补了**经命名管道驱动真守护进程**的端到端验证：`hello{proto=3}` → `open_tab` → **`current_tab`** → **`logout_site`** → 缺 `origin` 可操作报错 → `send_prompt` 回 **`not_implemented`** → `shutdown` + 退出码 **0**，**8 / 8 全过**；脚本按探针约定落盘 **`source/python/_probe/m7b20b_v3_commands_probe.py`**（零外网零登录）。
- **断言**：`--exec-selftest` **328 → 332 / 0**（+`VB2-43①~④`：词表 v3 / `tab` 事件 / 会话族无会话如实报 / `visible_login_request` 有头；`VB2-29④`（**10 命令 / 8 事件**）、`VB2-40④`（`logout_site` 不再是「尚未实现」）、`VB2-41②` **就地更新**）；Python `--selftest` **9 → 12 / 0**（+`VB2-42①②③`：词表 v3 + `site_key_of` 跨语言同构）。
- **回归（本机）**：构建 **0 error / 0 warning** · `api_probe --selftest` **332 / 0 · exit 0** · `--graph-selftest` **110 / 0** · Python `--selftest` **12 / 0** · `--pydoll-script-selftest` **4 / 0 · exit 0**（`proto=3` · 守护进程退出码 0 · 未强杀）· `--pipe-selftest` **PASS** · 残留进程 **0**。
- **文档订正（D 部分）**：`local_nodes.cpp:539-548` 的 `web::ensure_session` 属 **`builtin:deepseek` → `web_chat()`** 协议栈（要 `userToken` + PoW）⇒ **不切、随批 5 删**（`M7B-42` 白名单收敛 `{dom}`）—— `M7B.md` §1.4 表 + `source/README.md` 归属表同步订正（原写「批 3 / 需单独切」有误）。
- **边界（下一步）**：`send_prompt` / `read_answer` / `upload_image` 仍回 `err{not_implemented}`；GUI **真站点人工登录 + 端到端生成**是**人工关卡**（`M7B-19`）—— 本步只保证**入口与机制可用**，**不代为宣称已冒烟**；流式增量仍为轮询（`M7B-21`）。

**🧭 M7B 批 3 · step 10 落地：`M7B-20` 生产路径切换 —— `dom_chat` 走新通道（批 3 首个切换点）（2026-10-04）**

- **做法**：`dom_chat` 的依赖面只有 **3 个旧通道函数**（`web::ensure_session` ×1 + `web::run_script_sync` ×3）⇒ 按 §6.2「同构函数集」**逐行等价替换**；**DOM 脚本常量一字未改**（`M7B-20` 的验收口径 / `VB2-22`「DOM 层零改动」的证据保持不变）。
- **`ai/dom_web_client.cpp`（4 处换代 + 1 处断依赖）**：`:255` `web::ensure_session` → `web::channel::ensure_session`；`:269` / `:275` / `:310` `web::run_script_sync` → `web::channel::run_script`（注入配置 / kickoff / 轮询答案）；`:12-17` **删除 `#include "web/webview_host.h"`** → 改显式 `web/pydoll_channel.h` + `web/session_store.h` + `web/site_ref.h`（本文件所用符号**全部**落在**无 Win32 依赖**的三个头里）。
- **`main.cpp`（新增退出收尾 · 本步差异点）**：新通道会话 = **常驻守护进程 + 独立有头浏览器**，**不随本进程消失**（旧内嵌 WebView2 窗口随进程退出）⇒ 在 `ui::run()` 返回后、`log::shutdown()` 之前加 `session_ready()` → `shutdown_session()`（**幂等** · 收尾结果进日志）。诊断侧仍用 `ChannelSessionGuard`（RAII · step 9），生产侧由 `main.cpp` **统一**收尾（避免两套机制打架）。
- **签名逐字等价（无需适配层）**：`ensure_session` 三参、`run_script_sync` → `run_script` 五参的**类型 / 顺序 / 含义完全一致**；且 `SiteRef` = `using SiteRef = LoginRequest;`（`web/site_ref.h:60` · **纯别名**）⇒ 调用方**零转换**。
- **真站点实测（`kimi-web` · 只读零登录 · 与 `dom_chat` 是**同一条调用面**）**：`--web-dom-dump --provider kimi-web` **成功**（`https://www.kimi.com/` · 输入框 `div.chat-input-editor` 可见 · 建议 `send.selector=button.next-sidebar-nav-item` · Cookie 名 1 · localStorage **24 键**）；`--web-adapter-selftest --provider kimi-web` **会话 + 脚本执行均通**（返回「有缺项」= **条目缺 `answer_selector` 的业务判定**，该函数本步**未改动**，与通道无关）。
- **订正（诚实留痕 · 推翻一处过强结论）**：step 8 记的「pydoll 把脚本文本按**函数体**执行 ⇒ 诊断脚本**必须**带顶层 `return`」**不成立** —— pydoll `browser/tab.py:1465` 为 `if has_return_outside_function(script): script = f'(function(){{ {script} }})()'`，且 `expression=script` **直传** CDP `Runtime.evaluate`（`:1905`）⇒ **IIFE 与顶层 `return` 两种形态都能取回值**（本项目 4 个 DOM 脚本常量**全是** IIFE ⇒ 无需改形态）；当时「IIFE 回 `null`」实为**同批 `return_by_value` 缺失**的叠加效应。
- **回归（本机）**：构建 **0 error / 0 warning** · `api_probe --selftest` **328 / 0** · `--graph-selftest` **110 / 0** · Python `--selftest` **9 / 0** · `--pydoll-script-selftest` **4 / 0 · exit 0**（守护进程退出码 **0** · 未强杀）· `--pipe-selftest` **PASS** · 残留进程 **0**。
- **边界（下一步）**：`channel::logout_site` / `current_tab_site` / `tab_on_site` 仍为如实占位 ⇒ `property_panel.cpp` 按站点注销与 `local_nodes.cpp:542`（`window_on_site`）需**扩词表 v3**（`Storage.clearDataForOrigin`）；`local_nodes.cpp:539-548` 的前置判定仍是 **WebView2 + `userToken` 语义** ⇒ 与 `dom_chat` **分开切**（否则两个语义变更互相掩盖）；**用户可见变化**：新通道为**独立有头浏览器**（profile `~/.brain-ai/pydoll-profile/` ≠ 旧 `~/.brain-ai/webview2/`）⇒ **首次需重新登录一次**，UI 文案换代归 `M7B-25` / `M7B-33`。


**🧭 M7B 批 3 · step 9 落地：`M7B-18` 诊断换代 —— 两个诊断工具走新通道（**真站点验证通过**）（2026-10-03）**

- **做法（= step 7 记录块的路径 (a)）**：step 8 的 `run_script` 已备 ⇒ 把 `--web-dom-dump` / `--web-adapter-selftest` 的**执行后端**从 WebView2 换成新通道。
- **`ensure_session` 换代（核心）**：从「**只读判定**（要求守护进程已在跑）」→「**确保有活会话且已导航到站点**」—— 无会话则**起守护进程（常驻 `once=false`）+ `open_tab`（站点 `url`）**，已有则**复用**；登录态仍按 `I15′` 判（未登录 → false + 可操作原因，但**会话可用**：只读诊断照跑）。
- **新增会话生命周期**：`session_ready()`（纯查询）· `shutdown_session()`（**幂等**收尾，返回「是否关过」）· **`ChannelSessionGuard`（RAII）** —— 保证诊断函数**任何 return / 抛异常**都收尾，**不留孤儿浏览器 / 守护进程**。
- **调用点换代**：`ai/dom_web_client.cpp` 的 `dom_selector_dump` / `dom_adapter_selftest` 各 4 处 —— `web::ensure_session` → `web::channel::ensure_session`、`web::run_script_sync` → `web::channel::run_script`；**`dom_chat`（生产路径）不动**（归 `M7B-20` · 守 §9.3「批 1–4 只新增不删改」）。
- **断言升级（`VB2-40⑤` 原地改，编号 / 总数不变）**：旧断言调 `ensure_session` 验「未接线」错误 —— 换代后它会**真起浏览器**（断言必须零副作用）⇒ 改为验 `session_ready()==false`（初始无会话）+ `shutdown_session()` **幂等**（返回 false · 不崩）。
- **真站点实测（`kimi-web` · 只读零登录 · 本机）**：`--web-dom-dump --provider kimi-web` **exit 0** —— 读到 `https://www.kimi.com/` · 标题「Kimi AI 官网 - K3 上线…」· Cookie 名 `theme`（**只读名**）· localStorage **24 键** · 输入框候选 **`div.chat-input-editor`（contenteditable · 可见）** · 回答容器 2 个（隐藏）；`--web-adapter-selftest --provider kimi-web` **exit 0** —— `input div.chat-input-editor → 命中 1 个（可见）` · `Cookie 期望 0 / 可读 0`（`I15` 只报数）· 给出可操作建议。⇒ **「用新通道读真站点 DOM」成立** —— 这是「web 版本验证」的实证，并**解锁批 4**（选择器回填）。
- **回归**：构建 **0 error / 0 warning** · `api_probe --exec-selftest` **328 / 0** · Python `--selftest` **9 / 0** · `--pydoll-script-selftest` **4 / 0 · exit 0** · `--pydoll-selftest` **exit 0** · `--pipe-selftest` **PASS** · 残留进程 **0**。
- **未含**：`--web-dom-dump` 的 `send` 建议仍为**启发式**（真值靠人工按 §10 回填，非本次范围）· `dom_chat`（网页版文字生成）仍走 WebView2（`M7B-20`）· `logout_site` / `current_tab_site` / `tab_on_site` 仍为如实占位。

**🧭 M7B 批 3 · step 8 落地：词表 v2 + `run_script` 全链路（新通道「页面内执行 JS」打通 · 诊断换代的地基）（2026-10-03）**

- **为什么是这一步**：`M7B-18`（诊断换代）受阻的**唯一原因** = 词表**没有「页面内执行 JS」命令**。补上它，`--web-dom-dump` / `--web-adapter-selftest` 才有换代地基 —— 这是「把 web 版本验证完」的第一块拼图。
- **词表 v1 → v2（两侧同升 · §6.1 冻结规则「改动 = 升 `v`」）**：+ 命令 `run_script {provider, script}` · + 事件 `script_done {result, truncated}` · + 错误码 `script_error` · `stage` 枚举 + `script`；`PROTO_VERSION` / `kProtoVersion` **1 → 2**（`protocol.py` · `__init__.py` · `channel_frames.h` · `channel_frames.cpp`）。
- **实现（三处）**：Python `daemon._cmd_run_script`（+ 分发；`NOT_IMPLEMENTED_HINT` 收窄为 3 条余项）· Python `daemon.script_payload()`（**截断保护**：超 48 KiB → 文本前缀 + `truncated:true`；**公开**以便离线断言）· C++ `channel::run_script()`（占位 → **真实实现**；无守护进程 ⇒ 可操作原因、**不自动起浏览器**）· `driver.execute_script(..., return_by_value=…)` 新参数。
- **新 CLI `--pydoll-script-selftest`（跨语言端到端 · `M7B-42①~④`）**：起守护进程 → `open_tab`（**本地临时 HTML** `file:///` · 零外网零登录）→ `run_script` 读 DOM → 类型保真 → 空脚本应回 `script_error` → `shutdown`。**实测 4 / 0 · exit 0 · 守护进程退出码 0**；脚本返回物证 = `{"count":2,"ids":["user","pass"],"title":"aiwrite-script-selftest"}` ⇒ **「DOM 枚举 + 结构化取回」能力成立**（`--web-dom-dump` 换代后正是依赖它）。
- **实现期订正（三个真因 · 形态都是「看起来像通道不通」）**：① **`return_by_value`** —— `execute_script` 默认**不传**该选项 ⇒ CDP 对**对象 / 数组**只回 `objectId`（不带 `value`）⇒ 解包得 `None`（**静默 `null`**）；数字（`6*7`）正常，故 `--driver-selftest` **从未暴露**它 —— 与 `M7B-05` 的「两层 `result`」**同族坑**。② **脚本形态** —— pydoll 把脚本文本按**函数体**执行 ⇒ 必须带**顶层 `return`**；`(function(){…})()` 这种「表达式」形态结果被丢弃（回 `null`）。③ **收尾顺序** —— 发完 `shutdown` **立刻 `close()` 句柄** → 守护进程读循环撞 `pipe_error` ⇒ **退出码 1**；改为正常收尾**不 close**、直接 `wait_daemon`（照 `--pydoll-selftest` 既有做法）。
- **实测（本机）**：构建 **0 error / 0 warning** · `api_probe --exec-selftest` **326 → 328 / 0**（+`VB2-41①②`；`VB2-40④` 拆分；`VB2-29③④` 升 **8 命令 / 7 事件**）· Python `--selftest` **7 → 9 / 0** · Python `--pipe-selftest` **15 / 0** · `--driver-selftest` **25 / 0**（+`step8-①`）· `aiwrite --pydoll-selftest` **exit 0 · `proto=2`** · `--pipe-selftest` **PASS** · **`--pydoll-script-selftest` 4 / 0 · exit 0** · 残留进程 **0**。
- **未含（批 3 余项）**：`send_prompt` / `read_answer` / `upload_image`（`M7B-20`~`M7B-23`）· `--web-dom-dump` / `--web-adapter-selftest` 的**换代接线**（下一小步 = 诊断脚本改由 `run_script` 下发 ⇒ 完成 `M7B-18`）。

**🧭 M7B 批 1–2 · step 7 开工：`M7B-19` 全基线（12 / 13 绿）+ `M7B-18` 受阻登记（2026-10-03 · **纯文档，零代码改动**）**

- **`M7B-19` 全基线（本机实测 · 逐条贴数字）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy）· `api_probe --selftest` 七组 PASS · `--exec-selftest` **326 / 0** · `--graph-selftest` **110 / 0** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--pipe-selftest` **PASS**（守护进程退出码 0 / 丢弃非法帧 0）· `--pydoll-selftest` **exit 0**（`proto=1` · `python=3.12.10` · `browser=edge`）· `--pydoll-login <未知 id>` **exit 2** + 可操作提示、不开窗 · `--run-selftest --web` **PASS**（**生产路径仍走 WebView2**）· `python -m brain_ai_browser --session-selftest` **22 / 0 · RC=0**（`VB2-39①~⑬`）· docs+source 相对链接 **320 / 0 broken**。
- **⚠️ 唯一偏差（如实登记 + 订正历史记录）**：`--run-selftest`（**official** 模式）**FAIL · exit 1** —— 本机未设 `DEEPSEEK_API_KEY`、且凭据库无 `config.toml` 的 `api_key_ref = brain-ai/deepseek` 条目 ⇒ `n3 LLMGenerate error（缺少 API Key）` → `n4 skipped` → `PC-05 归档：失败`。**判定 = 环境依赖，非代码回归**：报错路径在 `engine/provider_resolve.cpp:291-295`、**step 1~6 未触碰**；且 `--run-selftest --web`（不依赖 Key）**PASS** ⇒ 执行器链路完好。**并订正 v13 / v19 措辞**：§11.1 的「PASS / PASS（5-5）」是 **2026-09-28 旧机**记录（`F:\Python` + Chrome 156），v13 的「批 0 复测」把它当成换机后结论照抄 —— **本机从未真正复测该条** ⇒ **§9.3「换机后必须重取基线」在此抓到一处漏项**；§11.1 已改为「**该条依赖本机凭据，换机/重装后必须重配**」。
- **`M7B-18` 诊断换代 = 部分完成 + 受阻（未擅自扩契约）**：✅ `pydoll_login`（`aiwrite --pydoll-login <id>`）step 6 已落地；⬜ `--web-adapter-selftest` / `--web-dom-dump` 走新通道 —— 二者需**在页面内执行任意诊断 JS**（现走 `web::run_script_sync` → WebView2），而**词表 v1 只有 7 个命令、无此命令**（`protocol.py:42-50`），扩表按 §6.1 冻结规则 = **升 `v`**（牵动两侧 + `ready{proto}` 断言 + `VB2-40④⑤`）；且与 `B12-C1`「**批 2 只新增不替换**」**口径冲突** ⇒ 已记录三条路径待拍板：**(a)** 扩词表 `v2` + `M7B-18` 移批 3；**(b)** 诊断改走**不经管道**的 Python 侧 CLI；**(c)** 保留 WebView2 诊断至批 5。
- **明确未做**：生产路径切换（`M7B-20` · 归批 3；前置缺口实测仍在：`daemon.py` 只实现 `hello`/`shutdown`/`open_tab`/`login_state`，C++ `run_script`/`logout_site`/`current_tab_site`/`tab_on_site` 为如实占位）· 打标签 `m7b-batch2`（工作区 **118 文件未提交**，须先提交）· 可选改进（属新任务）：让 `--run-selftest` 缺 Key 时**回 2**（对齐 `main.cpp:74` 的「2 = 缺少 API Key，离线部分已通过」语义）。

**🧭 M7B 批 1 · step 6 落地：C++ 侧接线（站点描述搬迁 + 快照只读视图 + 新通道 `pydoll_channel` + `PipeClient` 判据同步）（2026-10-03 · **生产路径零改动**）**

- **新增（C++ 侧 · `src/web/`）**：`site_ref.h`（`SiteRef` = `LoginRequest` **纯别名**；全部站点纯函数（`login_request_of` / `interactive_login_request` / `probe_login_request` / `boot_login_request` / `plan_session_boot` / `login_request_site`）**整体搬迁**于此 —— `webview_host.h` 改为 include 它 ⇒ **新通道取站点描述不再拉入 WebView2 依赖**，且现有调用点**零改动**（守 `I2`））· `session_snapshot.{h,cpp}`（**L2 快照只读视图**：路径 / 触发时机 / 状态文案；**只碰元数据** `exists` / `file_size` / `last_write_time`，**永不读内容** ⇒「导出物零明文」在本层**天然成立** · `VB2-39⑥`）· `pydoll_channel.{h,cpp}`（`namespace web::channel`：**`selftest` / `pydoll_login` 落地**；`ensure_session` 只读判定（`I15′`）；`run_script` / `logout_site` / `current_tab_site` / `tab_on_site` **如实回「尚未实现（批 3）」** —— 不假装成功）。
- **`PipeClient` 判据同步（关闭 `MB-Q10`）**：`stage` 帧**带请求 id** ⇒ 该命令的**完成回包**；只有 `id == "-"` 才是纯进度事件（此前会把 `open_tab` 的回包当进度吞掉 ⇒ `call()` 等到超时）。
- **改动（只加不替换 · `B12-C1`）**：`paths.{h,cpp}` +`pydoll_profile()` / `session_snapshot_dir()`；`provider_spec.{h,cpp}`：`allowed_web_fields()` +`attach` + 解析（**只解析、不消费** · `B12-C2`）；`session_store.h` 注释（写入方 = CDP 快照 · 持久化归 L2 · `I15′`）；`CMakeLists.txt` 挂 3 组新源 + **`aiwrite_copy_python()`**（`brain_ai_browser/*.py` → `<exe>/python/…`，**`.venv` 排除**；实测 10 个 `.py` 就位）；`main.cpp` +`--pydoll-selftest` / `--pydoll-login <id>`；`tools/api_probe.cpp` **只新增**断言块。
- **实测（本机）**：构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy）· `api_probe --exec-selftest` **320 → 326 / 0**（**只升不降**：`VB2-40①~⑤` + `VB2-39⑥`）· `api_probe --selftest` 七组 PASS · `--graph-selftest` **110 / 0** · `aiwrite --pydoll-selftest` **exit 0**（守护进程 → `hello` → `ready{proto=1, python=3.12.10, browser=edge}` → `shutdown` → 退出码 **0**）· `aiwrite.exe --pipe-selftest` **PASS** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS**（**生产路径仍走 WebView2** ——「先建后拆」证据）· `--pydoll-login <未知 id>` **exit 2 + 可操作提示、不开窗**。
- **实现期订正（真因）**：`find_package_dir()` 起初只按「包目录存在」挑候选 ⇒ 选中 `<exe>/python` 的拷贝（旁边**没有** `.venv`）→ 退回 PATH 上的 `python.exe`（本机 **3.14.3 无 pydoll**）⇒ 生产登录必失败；改为**优先「包目录 + 同级 `.venv/Scripts/python.exe`」**那一份（实测后 `python=3.12.10`）。
- **明确未含（step 7 / 批 3）**：`--pydoll-login` 的**真站点人工登录冒烟**（`M7B-19` 门槛项）· 生产路径切换（`M7B-20`：`dom_web_client` 改走新通道）· `upload_image` / `send_prompt` / `read_answer` / CDP 增量流式 · 诊断换代（`--web-dom-dump` / `--web-adapter-selftest` · `M7B-18`）。

**🧭 M7B 批 1 · step 5 落地：L2 登录态加密快照 `session.py` + `VB2-39`（Python 侧生效）（2026-10-03 · C++ 侧零改动）**

- **新增（Python 侧）**：`source/python/brain_ai_browser/session.py` —— **DPAPI 加密快照**（`ctypes` 直调 `crypt32`，`CRYPTPROTECT_UI_FORBIDDEN` ⇒ 无 UI 提示、**不引第三方依赖**）；数据落 **`~/.brain-ai/session/cookies.dat`**（**代码入库 / 数据不入库**；`BRAIN_AI_SESSION_DIR` 仅作测试钩子 ⇒ 自检**绝不污染真实快照**）；纯函数 `host_of` / `belongs_to`（**父域方向**）/ `scoped` / `to_cdp_params`（**字段白名单** + 会期 Cookie **不带 `expires`**）/ `usable_cookies` / `scan_plaintext` / `summarize`；`SnapshotStore.save/load/clear`（**原子写** `.tmp` + `os.replace`、**明文摘要去重**、**损坏 / 篡改 / 版本不识 = 当「没有」**）。
- **`I23②③④` 落地**：**三触发点**（`open_tab` 登记作用域 → `login_state=logged_in` **登录即写** → `shutdown` 在 `Browser.close` **之前**再写）+ 空闲 / 命令间隙**每 5 s 刷新**（`wait_for(queue.get(), 5)`，与命令**同一任务** ⇒ **不并发访问 CDP**）；启动 / 自愈后**回灌一次**（`Storage.setCookies`），**失败发 `error{not_logged_in}` 显式提示**（`I23④`，不影响本次命令回包）；**无 DPAPI ⇒ 不快照、绝不写明文**。
- **订正（`M7B-17` 口径）**：`driver.cookies_all()` 由**页级** `Network.getCookies`（依赖当前页、**会漏父域登录 Cookie**）改为**浏览器级 `Storage.getCookies`**（全 origin · 含 HttpOnly）；新增 `set_cookies` / `delete_all_cookies`。
- **实现期订正三处**：① `gone_exceptions()` 纳入内建 **`ConnectionError`**（浏览器被杀后下一条命令先撞「连接被拒绝」`[WinError 1225]` ⇒ 漏掉会**丢自愈**）；② DPAPI blob **自带完整性校验**（截断 / 改头部 / 改中段 / 改末尾 / 明文冒充 → 错误码 13），**例外面 = 头部明文「描述区」**（改它不影响解密）⇒ 篡改断言取**中段 / 末尾**；③ **close 前刚写过 Cookie** 时 Chrome 退出可能 > 5 s（flush cookie 库）⇒ 判据用生产同款 `close_verdict`。
- **新开口项 `MB-Q11`**：作用域默认 = **本次会话导航过的域**（由 `open_tab` 的 url 派生），**空作用域 ⇒ 不写快照**（隐私最小、不发散）；接口留 `hosts=[...]` 覆盖口。
- **新增 `--session-selftest [--headless]`**：组 A 离线 8 条（DPAPI 往返 / 文件与日志**零明文** / **父域作用域** / 会期语义 / 过期过滤 / **6 例损坏不崩**）+ 组 B 真机 5 条（注入 → 存 → 清空 → **回灌：名字齐 + `httpOnly` 保持**）。
- **验收（step 5）**：`--session-selftest --headless` **22 / 0**；**系统 Python 3.14.3（无 pydoll）→ 15 / 0 + 组 B 显式 SKIP + exit 0**（`VB2-30` 同族）；`--daemon-selftest --headless` **23 / 0**（22 → **+`M7B-11⑭`**：`shutdown` 前落盘必经、非死码）；`--driver-selftest` **22 / 0** · `--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · `aiwrite.exe --pipe-selftest` **PASS** · `api_probe` 七组 / **320 / 0** / **110 / 0** · `aiwrite --provider-selftest` **50 / 0**；`~/.brain-ai/session/` **未被自检创建**；收尾**无残留进程**。
- **进度**：`M7B.md` §6.2 **step 5 记录块** + 批 1 文件表 `session.py` 标完成 + §9.2 `VB2-39` 标「Python 侧生效」+ §11.1 基线表（`--session-selftest` / `--daemon-selftest` 行）+ 开口项 **`MB-Q11`** + §15 **v18**。
- **明确未含（step 6~7）**：C++ 侧 `pydoll_channel` / `session_snapshot` / `SessionStore` 改造 + `PipeClient` 进度事件判据同步 + 诊断换代（`--pydoll-login` 等）+ `M7B-19` 中间检查点；`upload_image` / `send_prompt` / `read_answer` 仍未实现（批 3）—— **`webview_host` 原样在跑**。

**🧭 目录迁移：Python 运行代码 `python/` → `source/python/`（代码归源码树 · 数据仍归 `~/.brain-ai`）（2026-10-03 · 纯搬迁，零逻辑变更）**

- **动因（代码 / 数据分离）**：**运行代码**（守护进程 / 管道 / Pydoll 封装 / 探针 + `requirements.txt` + `.venv`）全部收进 `source/python/`，与 C++ 同树、随构建树管理；**运行期数据**一律留 `%USERPROFILE%\.brain-ai`（`logs` / `pydoll-profile` / …；step 5 的登录态快照将落 `.brain-ai\session\`）—— **代码入库、数据不入库**。
- **迁移范围**：`python/brain_ai_browser/`（10 个模块）· `python/_probe/`（**70 文件**，含 45 个物证 JSON；`git mv` 保历史）· `python/requirements.txt` · `python/.venv/`（**移动后实测可用**：`sys.prefix` 自动跟随、`-m pip` 正常 ⇒ **未重建**，省一次 ≈110 s 安装）。
- **C++ 同步（1 行逻辑 + 4 处注释）**：`src/main.cpp` 的 `--pipe-selftest` 包目录候选 `<仓库根>/python` → **`AIWRITE_SOURCE_DIR / "python"`**（= `<仓库根>/source/python`；`.venv` 候选随 `package_dir` 自动正确、`work_dir` 同步）；`web/channel_frames.h` / `web/pipe_client.h` 注释路径同步。
- **口径同步（58 处文本 + 4 处代码文案）**：`.gitignore`（`/python/.venv/` → `/source/python/.venv/` 等 4 条 + 段注释）· `runtime.py` 的 **`PYDOLL_HINT` 用户可见引导** · `driver.py` 文档串 · `requirements.txt` 头部（含新安装命令）· docs **8 文件**（`M7B.md` / `CHANGELOG.md` / `M7.md` / `M_patchAB_rest.md` / `网页版协议实测记录.md` / `source/README.md` / `DevPlan.todo` / `providers.json`）。**历史物证文件（`_probe/out/*.json|*.err`）按「证据不可篡改」原则保留原路径**（不改一行）。
- **验收（迁移后逐条与迁移前一致 · 本机 2026-10-03）**：`--selftest` **7 / 0** · `--pipe-selftest` **13 / 0** · `--driver-selftest --headless` **22 / 0** · `--daemon-selftest --headless` **22 / 0**（子进程 `cwd = parents[1]` 自动跟随新路径的实测）· `aiwrite.exe --pipe-selftest` **PASS**（C++ 按新候选找到包 + `.venv` 起守护进程，`hello→ready` 正常）· `api_probe --selftest` **七组 PASS** · `--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS** · 构建 **0 error / 0 warning** · docs 相对链接 **298 / 0** · 收尾**无残留进程**。
- **边界**：**零逻辑变更**（唯一 C++ 改动 = 路径常量 + 注释）；`webview_host` 原样在跑；`source/assets/providers.json` 仅注释内物证路径同步；**step 5（`session.py` · `VB2-39`）仍未开工**。

**🧭 M7B 批 1 · step 4 落地：守护进程主循环 + `VB2-38`（Python 侧生效）（2026-10-03 · 新增通道代码，生产路径零改动）**

- **新增（Python 侧）**：`source/python/brain_ai_browser/daemon.py` —— **唯一主循环**：管道**监听线程**（阻塞 `accept` / `read_line`）→ `call_soon_threadsafe` 投 `asyncio.Queue` → **主线程 asyncio** 跑 `handle_frame`（Pydoll 用同一循环）；**写帧一律 `asyncio.to_thread`**；收尾 = 关管道（放监听线程出来）→ `join` → 补关浏览器（异常路径）。浏览器**按需启动** ⇒ `hello` / `shutdown` 不需要浏览器（`aiwrite.exe --pipe-selftest` 仍秒级）。`--serve` 由「最小伺服」**整体迁入** `daemon.serve()`，`__main__.py` 只留 CLI 与三层自检（`--pipe-selftest` / `--driver-selftest` 也改为走**生产同一条帧处理路径**）。`redact.py` —— 日志脱敏（`~/.brain-ai/logs/browser.log`，一行一条 JSON；`log_event()` = **唯一写日志入口** ⇒「无明文」可被扫描断言；Cookie 值 / 令牌一律打码，名字保留可诊断）。
- **命令边界落定**：`hello` → `ready`（**依赖缺失时追加一条 `error` 事件**带可操作引导 —— `M7B-13` / `M7B-14`）；`open_tab` → 按需起浏览器 + `stage{open, ok}`（**带请求 id = 完成回包**）；`login_state` → `login{state, cookie_names, has_expires, http_only}`（**Cookie 值不进协议**；可选 `cookie_names` / `domain_suffix`，未给 → `state=unknown`，判定不越权自造 —— `I14`）；`shutdown` → `stage{close}` → `Browser.close` → **等进程退出**（5 s，超时才兜底强杀 + **必须留 warn**）；`upload_image` / `send_prompt` / `read_answer` 仍 `err{daemon_down, hint=尚未实现}`（`MB-Q7`）。
- **`M7B-11` 关键行为**：**用户手动关窗 = 可恢复状态** —— `BrowserNotRunning` / 浏览器进程已死**被吞**，L4 留痕（`browser_selfheal`）后**自愈重启**（实测：强杀浏览器 → 下一条命令仍成功、pid 换新、**stderr 零回溯**）；L4 心跳（10 s）/ 崩溃检测 / `daemon-state.json` 留痕（未干净退出 → 下次启动打印「⚠️ 上次未干净退出（登录态可能已回滚）」，`MB-D0-8` L4）。
- **`VB2-38`（`I23①`）Python 侧生效**：`daemon.close_verdict()` **7 条纯逻辑判据**（缺 `Browser.close` → 失败 / `BrowserNotRunning` → 归正常收尾 / 超时**无 warn** → 失败 / 超时 + warn（±兜底强杀）→ 通过 / 证据自相矛盾 → 失败）+ 端到端物证（`verdict_ok=true`、`waited_s=2.16 s`、**未强杀**）。
- **新增 `--daemon-selftest`**：组 A（`VB2-38` 离线 7 条判据 + L4 判定 + 日志脱敏零命中）+ 组 B（真机端到端 11 条：`hello` / 按需启动 / L4 埋点 / `login_state` 值不进协议 / **强杀后自愈** / `shutdown` 等进程退出 / 退出码 0 / 干净退出留痕 / 无残留 / stderr 无回溯）。
- **⚠️ 实现期口径（新开口项）**：① `stage` 帧**带请求 id = 该命令的完成回包**、`id="-"` 才是纯进度事件 ⇒ **C++ 侧 `PipeClient` 判据需同步**（否则 `open_tab` 的 `call()` 等不到回包）→ **`MB-Q10`**；② §6.1 表 2 缺 `login` 事件名（命令边界表已引用）→ **`MB-Q9`**；本批**均不动词表**，以文档口径先行。
- **验收（step 4）**：`--daemon-selftest` **有头窗口 22 / 0**（合法帧 4 · 丢弃非法帧 0 · 浏览器启动 **2 次**（含自愈 1 次）· 退出码 **0** · `close_wait` **2.16 s / 未强杀** · 残留 **0** · stderr 无回溯）；`--driver-selftest` **22 / 0**（回归）；`--selftest` **7 / 0** · `--pipe-selftest` **13 / 0**（系统 Python **3.14.3 无 pydoll → 14 / 0**，多出 1 条 = `ready` 后**追加 error 事件**的真机证据）；`aiwrite.exe --pipe-selftest` **PASS**（**新主循环**在 C++ 侧端到端可用、**未改一行 C++**）；`--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS**；docs 相对链接 **298 / 0**；收尾**无残留进程**。
- **进度**：`M7B.md` §6.2 **step 4 记录块** + 命令边界表落定 + §9.2 `VB2-38` 标「Python 侧生效」+ 开口项 **`MB-Q9`** / **`MB-Q10`** + §15 **v16**。
- **明确未含（step 5~7）**：L2 加密快照 `session.py`（`VB2-39`）· C++ `pydoll_channel` / `session_snapshot` / `site_ref` 接线与诊断换代（`--pydoll-login` 等）· 守护进程**自启动**与「一键重登」UI · `M7B-19` 中间检查点 —— **生产路径零改动**（`webview_host` 原样在跑）；`source/assets/providers.json` **未改**。

**🧭 M7B 批 1 · step 3 落地：Pydoll 驱动最小集 + `M7B-04` 合测通过（2026-10-03 · 新增通道代码，生产路径零改动）**

- **新增（Python 侧 · 3 个模块）**：`source/python/brain_ai_browser/browsers.py`（`M7B-14` 浏览器探测：标准安装路径**先 Chrome 后 Edge**、`detect_browser` / `browser_class` / `env_proof`；跨语言常量 `PROFILE` = `~/.brain-ai/pydoll-profile`、`WINDOW_SIZE` = `1440×1000`（`P4`））· `runtime.py`（`M7B-13` 运行时检测 `check_runtime() -> {python, pydoll, ok, hint}` + 可操作引导 + `DependencyError`；`load_pydoll()` = **包内唯一 pydoll 入口**，自检可打桩）· `driver.py`（**最小集**：`start` / `tab_for` / `new_tab` / `cookies_all` / `cookies_for_domain`（`P3` 按域过滤）/ `execute_script`（**单点解包两层 `result`** —— 解错层级会**静默拿空串**）/ `close_wait`（`Browser.close` → **等进程退出**；**超时只 warn、不提供强杀**，`MB-D0-8` L1/L3）；`_launch()` = 唯一启动点）。pydoll 一律**惰性 import** ⇒ 未装 pydoll 的系统 Python 下 `--selftest` / `--pipe-selftest` / `--serve` 照旧可跑。
- **新增（`__main__.py`）**：`--driver-selftest [--headless] [--timeout <秒>]` —— **组 A** 离线 `VB2-30`（打桩验「依赖缺失 → 报错 + 引导 + 零副作用」，**不需要真卸依赖**）+ **组 B** 真机（驱动断言 + `M7B-04` 合测，会**弹真实浏览器窗口**）；`--headless` / `--timeout` **仅在**该开关下合法，否则 **exit 2**（**不静默忽略**）。
- **`M7B-04` ✅ 已过**：管道伺服在**独立线程**（实例先由主线程 `open()`），asyncio 主循环里**并发**跑「浏览器命令 burst」与 `asyncio.to_thread(pipe 往返)` ⇒ ① 往返时刻落在命令窗口内**且返回时命令仍在跑**；② 窗口内心跳持续推进（**12 次 / ≈0.66 s**；headless 轮 14 次 / ≈0.78 s）⇒ 事件循环未被管道读阻塞（**真并发**判据，非「先后都成功」）。生产形态（`daemon.py` 主循环 + 命令分发）在 step 4 复验。
- **`VB2-30`（`I21`）双证据**：① **桩物证** = `no_python` / `no_browser` 两条路径 → 可操作引导 + **启动器调用数 0**（不开浏览器）+ **新建 socket 数 0**（不发 HTTP）；② **真机物证** = **系统 Python 3.14.3（本机未装 pydoll）** 实跑 `--driver-selftest` **11 / 0**（组 B **显式 SKIP** + exit 0 —— 不静默、不假装通过）。
- **⚠️ 实现期订正（新踩坑 · 与 step 2 的 `P7` 同类表象）**：`M7B-04` 合测初版把管道伺服线程放在组 B **开头**、`accept` 超时 15 s ⇒ 被「起浏览器 6.6 s + 4 次 PowerShell 取证」挤爆，`accept` 超时**作废实例** → 客户端 `CreateFile` 报 `FILE_NOT_FOUND` 重试到超时。**表象 = 双方都说「对方没听见」（极易误判为协议错）**，成因是**时序**而非句柄模式。修法：实例先由主线程 `open()` + 伺服线程紧贴合测窗口启动（`accept` 30 s 兜底），并把「**往返返回时命令仍在跑**」写进判据。
- **验收（step 3）**：`--driver-selftest` **有头窗口 22 / 0**（起 Edge pid=10448 · 启动 **6.56 s** · `execute_script` → **42** · `cookies_all` 为 list · 真实命令行含 `--window-size` / `--user-data-dir` · `close_wait` **3.83 s / exit 0 / 无强杀** · 残留 **0** · 管道 `served=1`）；`--driver-selftest --headless` **22 / 0**（判据不依赖 headless）；`--selftest` **7 / 0** · `--pipe-selftest` **13 / 0**（venv 3.12.10 与系统 3.14.3 均过）；参数错误路径 4 条均 **exit 2**；构建 **0 error / 0 warning**（唯一告警 = **既有** `brotlienc.dll` copy，与本轮无关）；全基线复测 `--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS** · `aiwrite.exe --pipe-selftest` **PASS**；docs 相对链接 **297 / 0**；收尾**无残留进程**。
- **进度**：`M7B.md` §5 **7/9 已过**（`M7B-04` 转 ✅，注明「生产形态 step 4 复验」）；§6.2 新增 step 3 记录块（含实现期订正）；新增开口项 **`MB-Q8`**（词表缺「依赖细分」码 —— pydoll 缺失沿用 `no_python` + `hint` 显式说明；**本批不动词表**，与 `MB-Q7` 同族）；§15 **v15**。
- **明确未含（step 4~7）**：守护进程主循环 `daemon.py`（含把 `--serve` 最小伺服整体迁走）· L2 快照 `session.py` · 日志脱敏 `redact.py` · C++ 通道接线与诊断换代（`--pydoll-login` 等）· `M7B-19` 中间检查点 —— **`VB2-38` / `VB2-39` 尚未生效**、`VB2-30` 仅 Python 侧（C++ 侧 `no_python` / `daemon_down` 文案归 step 5~6）；**生产路径零改动**（`webview_host` 原样在跑、新通道**未接线**）；`source/assets/providers.json` **未改**。

**🔌 M7B 批 1 · step 2 通过：C++ ↔ Python 命名管道双向 IPC 打通（`M7B-02`）（2026-10-03 · 新增通道代码，生产路径零改动）**

- **新增（Python 侧）**：`source/python/brain_ai_browser/pipe.py` —— `ctypes` 直调 kernel32（`CreateNamedPipeW` / `ConnectNamedPipe` / `ReadFile` / `WriteFile` / `CancelIoEx`），**overlapped + 精确超时**；`PipeServer`（**服务器 = 守护进程**，一实例一连接，accept 超时即作废实例）+ 连接对象（`read_line` / `write_line` / `write_bytes`；无换行的超长行**原样交出**，由 `protocol.parse_line` 判非法）。**纯标准库**，不引 pydoll（step 2 不碰浏览器）。
- **新增（`__main__.py`）**：`--serve`（**最小伺服**：`hello` → `ready{proto, python, browser}`；`shutdown` → `stage{stage=close}` 后退出；`--once` 单连接即退；`--idle-timeout` 超时退出**不留僵尸**；非法行**只**丢弃 + 回 `err{bad_frame}`，**不退出**）+ `--pipe-selftest`（进程内 loopback 6 条断言）。
- **新增（C++ 侧）**：`source/src/web/pipe_client.{h,cpp}`（挂 `aiwrite_core`）—— `connect` / `call` / `send_command` / `on_event` / `close`；**读线程 + `id` 配对**（`delta` / `stage` 为**进度事件**，不完成调用）；词表校验在**发送前**做（未知命令 / 缺字段 → 直接失败，**不发帧**）；收到非法行**回 `err{bad_frame}`**；`close()` = `CancelIoEx` → join → 关句柄。契约：**不得在 UI 线程调 `call()`**。
- **新增（`main.cpp`）**：`--pipe-selftest [--timeout <秒>]` —— 起 Python 守护进程（`brain_ai_browser --serve --once`，其 stdout/stderr 落 `~/.brain-ai/logs/pipe_selftest_daemon.log`）→ 连管道 → `hello` → 校验 `ready` → `shutdown` → 收 `stage{close}` → **等进程退出**（`I23①` 口径，不强杀）；失败时打印守护进程日志尾部。
- **⚠️ 实施要点（新风险行 `P7` · `M7B.md` §6.2）**：**同步句柄上的并发 I/O 会被 I/O 管理器串行化** —— C++ 侧若用非 overlapped 句柄，读线程的挂起 `ReadFile` 会把 `WriteFile` 排到死。踩坑表象 = **「写阻塞 20 s（＝服务端 idle 超时）后 `ERROR_NO_DATA`」而服务端同轮收到 0 帧**（两端互等；双向都像「对方没听见」，**极易误判为协议错**）。修法：句柄**必须** `FILE_FLAG_OVERLAPPED`（读/写各一事件）+ 写加 **30 s 上限**（对端不读**不无限等**）+ `close()` 用 `CancelIoEx`。
- **验收（step 2）**：`aiwrite.exe --pipe-selftest` → **PASS / exit 0**（`ready{proto=1, python=3.12.10, browser=edge@…}` + `stage{close}` + 守护进程退出码 **0** + 丢弃非法帧 **0**）；Python `--pipe-selftest`（loopback）**13 / 0**（venv 3.12.10 与系统 3.14.3 均过）；守护进程日志**逐帧物证 = 收到 2 帧**（`hello` 44 B / `shutdown` 62 B）；构建 **0 error / 0 warning**；全基线复测 `--exec-selftest` **320 / 0** · `--graph-selftest` **110 / 0** · `--provider-selftest` **50 / 0** · `--provider-dump` **21 条** · `--run-selftest` **PASS**（**生产路径未动** = `MB-D1` 先建后拆的证据）；收尾**无残留进程**。
- **进度**：`M7B.md` §5 **6/9 已过**（`M7B-02` 转 ✅）；`M7B-04` 细化（「管道监听线程 × 调用线程」并发已合测，**仅剩** asyncio × 管道监听线程 → 归 step 4 的 `daemon.py`）；新增开口项 **`MB-Q7`**（词表缺「命令未实现」错误码 —— 本批**不动词表**，动词表 = 升 `v`）；§15 **v14**；顺手修 §6.2 的重复标题（`### 批 1` 出现两次）。
- **明确未含（step 3~7）**：Pydoll 驱动（`driver.py`）· L2 快照（`session.py`）· 守护进程主循环（`daemon.py`）· 诊断换代（`--pydoll-login`）· `M7B-19` 中间检查点 —— `VB2-30` / `VB2-38` / `VB2-39` **尚未生效**；**生产路径零改动**（`webview_host` 原样在跑，新通道**未接线**）；`source/assets/providers.json` **未改**；`DevPlan.todo` id 200/201 仍 `done: false`。


**🧱 M7B 批 1–2 施工细化表冻结 + 批 0 基线复测（含换机基线修复）（2026-10-03 · 文档 + 3 处产品侧小改）**

- **冻结**：[actionPlan/M7B.md](actionPlan/M7B.md) 新增 **§6.2 批 1–2 施工细化表**（拆到**文件 / 函数 / 命令字段 / 断言编号**级：批 1 文件表 + 命令边界；批 2 接口映射表 + 逐文件改动清单 + 断言归属 + `M7B-19` 门槛 + 风险 + 开工 step 1~7）+ §15 **v13**。三处新口径：`B12-C1` **批 2 只新增不替换**（`pydoll_channel` 与 `webview_host` 并存，调用方零改动；切换归批 3 `M7B-20`）/ `B12-C2` `ai/provider_spec.cpp::allowed_web_fields()` **加 `attach`**（解析 + 缺省 `auto`，**运行期不消费**；`providers.json` 本批**不落值**）/ `B12-C3` `SessionStore::user_token` **留到批 5**（守 §9.3「批 1–4 只新增不删改」）。
- **批 0 基线复测（换机后必须重取，§9.3）**：构建 **0 error / 0 warning** · `api_probe --selftest` **七组 PASS** · `--exec-selftest` **311 / 0** · `--graph-selftest` **110 / 0** · `aiwrite --provider-selftest` **50 / 0** · `--provider-dump` **21 条**（official 9 / web 12）· `--run-selftest` / `--run-selftest --web` **PASS** · docs 相对链接 **296 / 0 broken**。
- **换机账（本轮唯一产品侧改动 · 4 处）**：开工时 `--exec-selftest` = **309 / 1**，唯一失败 `M5-04 示例：E-02 加载校验通过` —— 根因 = `source/workflows/examples/E-02_图片转小说.json` 写死**旧机绝对路径**（`F:/GameDao/Tools/AIwrite/source/assets/images/sample.png`），而 `engine/graph.cpp:584-587` 的 File 参数校验对非令牌值只做 `exists(原值)` ⇒ **换机必挂、且换机用户打开示例必报「文件不存在」**。修复：① 示例图片路径改**仓库相对路径** `assets/images/flamingo.png`（描述同步）；② `tools/api_probe.cpp` **新增**「示例图片路径**可移植**（不含盘符 / 反斜杠）」断言，并在校验前把路径**代入本机示例图**（相对路径 / 资源令牌无法跨机解析，不代入会把「示例可移植」误判成「示例损坏」）；③ `main.cpp` 的 `--vlm-selftest` 默认图 `sample.png` → `flamingo.png`（`sample.png` 已在工作区删除）；④ `docs/节点编辑器使用说明.md` §10 同步。→ 断言 **310 → 311**，**全绿**。
- **工具账（换机）**：`source\build.ps1` 在本机**被执行策略拦截**（`running scripts is disabled`）→ 复现改用 `cmake --build --preset debug`；旧机 `vcpkg-cache/check_links.py` 在本机**不存在** → 改用等价内联链接检查（**296** 相对链接 / **0 broken**）。
- **批 1 · step 1 已落地（协议词表 · 2026-10-03）**：新增 `source/python/brain_ai_browser/`（`__init__.py` / `protocol.py` / `__main__.py`，**纯标准库、不新增依赖**）—— §6.1 词表的**唯一实现**（帧格式 `v`/`id`/`kind` · 7 命令 · 6 事件 · 9 错误码 · 容量与超时上限 · `attach` 取值）；新增 C++ 侧**同构纯函数** `source/src/web/channel_frames.{h,cpp}`（无 IO、不抛异常、非法行只置 `bad`）+ 挂入 `aiwrite_core`；`tools/api_probe.cpp` 新增离线断言 **`VB2-29①~⑦` / `VB2-32①②`**（9 条；**不删任何旧断言**，守 `MB-D1`）。
- **验收（step 1）**：`source\python\.venv\Scripts\python.exe -m brain_ai_browser --selftest` → **7 通过 / 0 失败（exit 0）**；`py_compile` **退出码 0**；构建 **0 error / 0 warning**；`api_probe --exec-selftest` **311 → 320 通过 / 0 失败**（exit 0）。
- **实现期订正（`M7B.md` §6.1）**：`stage` 事件的**阶段标识键 = `stage`**（原表写 `name`）—— 帧头 `name` 已被「事件名」占用，两者在**同层 JSON 无法共存**（`VB2-29` 实现时暴露）；Python 与 C++ 两侧同改 + 文档注。
- **明确未含（step 2~7）**：管道（`pipe.py` / `pipe_client`）、Pydoll 驱动、L2 快照、守护进程主循环 —— `VB2-30` / `VB2-38` / `VB2-39` **尚未生效**；`--stub-selftest` 入口已**如实声明**该边界（不越权宣称）。
- **明确未含（口径不变）**：`source/assets/images/sample.png` 的删除与 `flamingo.png` 的入库取舍（属**上一批未提交变更**，提交时一并拍板）；`providers.json` 的 `web.attach` / `cookie_names` **仍不落值**（归 `P7b-10` / 批 4）；`DevPlan.todo` id **200 / 201** 保持 `done: false`；**本批不提交**变更集。



- **触发器（误报实证）**：换机后首次 B0（`source\python\_probe\m7b28_doubao_recon.py --login`）在**无人操作**窗口的 **9 s** 内自报「✅ 检测到登录成功」，**用户确认当时未登录** ⇒ 判为**误报**，按纪律**不采信**该 PASS（物证另存 `source/python/_probe/out/m7b28_doubao_recon.b0-false-positive-20261002.json`）。
- **两处根因（都在判据本身，不在站点）**：① **判据太松** = `Cookie 名差集非空` + `输入框出现` —— 命中的 `flow_cur_user_sec_id` / `flow_user_country` **匿名态也会被种下**，输入框**未登录就渲染**（B1 已证）⇒ 两个条件都不构成「已登录」；② **Cookie 未按域过滤** —— `Storage.getCookies` 返回**整个 profile**，Edge 首启在 `msn.cn` / `ntp.msn.cn` 种了 **10 条**自家 Cookie（`App-User` / `MUID` / `MUIDB` / `USRLOC` / `_C_Auth` / `_C_ETH` / `_EDGE_S` / `_EDGE_V` / `__rubyUX` / `MicrosoftApplicationsTelemetryDeviceId`）⇒ 差集被**站点无关名**污染，`D-30` 的「`cookie_names` 优先」会跟着错。
- **修复（`source/python/_probe/` 一次性资产）**：① 新增只读 **`LOGIN_JS`** 扫描**登录后才可能出现的 DOM 正证据**（头像 `img` + 近祖先命中 `avatar|头像` / 昵称 class 命中 `nickname|user-name|userName` / 用户菜单 `aria-label|title` 命中 `个人中心|账号|我的|profile|account` / 「退出登录」菜单项），**连续 `LOGIN_STABLE_POLLS=2` 次命中**才判定，「登录 / 注册」入口只作诊断；② 新增 **`SITE_COOKIE_DOMAIN='.doubao.com'`** + `cookie_snapshot(..., domain_suffix)` / `site_cookies()` ⇒ Cookie 快照 / `D-30` 差集 / `cookie_ttl` **只算站点域**（全库条目仍留证为 `cookie_rows_all_*`）；③ **重启复读的「仍登录」改用同一 DOM 判据**（原先看 Cookie 名 —— 匿名 Cookie 重启也在，**必假阳**）；④ verdict 增 `B0_login_dom_positive` / `B0_login_entry_present`，`d30_cookie_names_diff` 增 `domain_filter` 与「差集非空 ≠ 已登录」警示。
- **回归**：`--selftest` **PASS（exit 0 · 残留 0）** · `py_compile` 退出码 **0**（改动未触碰 B1 / B2 / B3 链路与 `source/`）。
- **文档**：[actionPlan/M7B.md](actionPlan/M7B.md) §10「登录 Cookie 存活口径」追加**误报纠错记录**（含「B1 已证未登录即渲染输入框」「匿名态也种 `flow_*`」两条直接反证）。
- **明确未含**：**B0 复证**（需人工登录一次）在本条写作时**仍在进行** —— 其结论**不在本条**内；`providers.json` 的 `web.attach` **仍不填**。
- **✅ 追加（同日实测收口 · B0 / B2 / B3 全部出结论）**：
  - **B0 复证 PASS**（exit 0 · **67 s**）：起点即已登录（**14 个鉴权名**）→ **6 s** 判定 → `close_wait` 优雅退出（残留 0）→ **重启复读**鉴权名仍在 → `仍登录 = True`。`B0_login_dom_positive` 仍 **FAIL** —— **如实留证**：豆包默认视口下**无可见头像节点**（这正是「鉴权 Cookie 白名单」这条腿存在的理由）。
  - **`D-30` 豆包证据**（**两次独立观测**的差集）：匿名态站点域 **8–10 条** → 登录态 **33–35 条**，净新增 **23 名**；其中 **14 个鉴权名**作 `cookie_names` 优先名单（其余随站点版本 / 账号形态变）。
  - **B2 PASS（`P7b-05` 双证据齐备）**：**把视口放大到 1440×1000 后**入口现身 = **`input.hidden`（隐藏 file input）** ⇒ `entry_kind=file_input` · `mode=dom` · `file_input_files=m7b28-doubao-256x256.png`；网络回执**收紧为 POST / postData 后仍 PASS**，回执链 = `POST /alice/resource/prepare_upload` → **`POST tos-hl-x.snssdk.com/upload/v1/…png`（TOS 上传本体）** → `POST /top/v1?Action=CommitImageUpload`。
  - **B3 PASS（真视觉）**：256×256 纯红方块 → 答「一整块均匀、饱和度很高的**正红色**…没有任何图案、文字、物体」⇒ **真·视觉理解**（非 OCR）；⚠️ 同一夹具在 **1×1** 尺寸下被判「**完全纯白色**」= **退化输入**（已换夹具，物证 `out/m7b28_doubao_recon.b3-1x1-white-20261002.json`）。
  - **另修 4 处探针缺陷**：① `classify()` 只认 `contenteditable` ⇒ 登录态 `textarea` 被误判 `none`（已认 textarea）；② `upload_requests` 只按 URL 子串 ⇒ 把 **CSS / JS / 图标 GET** 当上传回执（**假阳性**；现要求 **POST / 带 postData**，原启发式降级为诊断字段 `upload_urls_heuristic`）；③ `keyboard.press('Enter')` 传字符串 ⇒ pydoll 2.27.0 `ValueError: too many values to unpack`（新增 `common.press_key()` 转 **`Key` 枚举**）；④ 夹具 **1×1 → 256×256**。补充：**发送按钮在输入内容后才渲染** ⇒ 改为「输入完再扫一次」。
  - **新增工程约束**：**视口尺寸决定 UI 形态** —— 859×450 **无附件入口** / 1440×1000 有 ⇒ `M7B-28` 启动浏览器**必须显式设窗口尺寸**（探针 `SITE_WINDOW_ARGS`）；**2026-09-29 的 `paste_only` 结论作废**（小视口假象），`P7b-10` 注入路线定为 **`setFileInputFiles` / `expect_file_chooser`**。
  - **仍未含**：`providers.json` 的 `capabilities.vision` **保持 `false`**、`web.attach` **仍不填** —— 真视觉证据已到手，但改这两项属**产品资产 + `P7b-16` 三处落点**，需拍板后同批做。

**🐍 换机环境重建（路线 B）：Python 3.12.10 + `source/python/.venv` + `pydoll-python 2.27.0` 钉版 + Edge 兜底首次实跑（2026-10-02 · 零 C++ 改动）**

- **背景（换机账）**：国庆换机提交 `8e79254` 后 **`.venv` 未随仓库迁移**（`.gitignore` 覆盖），本机只剩 **Python 3.14.3**、**无 `pydoll`**、**无 Chrome**（仅 Edge **154.0.4258.48**）→ `M7B` 前置验证（`P7b-05b` 的 **B0 / B2 / B3**）**直接开跑不了**。本轮**只重建环境**，不动主管道、不改协议。
- **路线 B（最保守 · 与旧机 9 个探针物证同构）**：Python Install Manager 并行装 **3.12.10**（`PrependPath=0` → **不抢** 3.14 的 `python` / `py` 默认）→ `py -3.12 -m venv source\python\.venv` → 直连 PyPI 装 **`pydoll-python==2.27.0`**。**不升 `3.0.0`**（2026-09-29 发布的破坏性大版本，探针与主管道用到的 5 处私有 API 已变）。
- **新增入库 `source/python/requirements.txt`**（17 包 = `pip freeze` 原样；文件头写明**生成方式 / 解释器 / 安装命令 / 钉版理由**）。`.venv` 依旧由 `.gitignore:85 /python/.venv/` 忽略，**不入库**。
- **网络取证（换机后新事实）**：`py install --yes 3.12` 走 **BITS** 报 `NoInternetError`，但普通 HTTPS **通**（python.org HTTP 200）→ **真因是带宽**（≈**20–70 KB/s** 且中途多次 stall），非防火墙。处置：改「官方安装器直下 + **断点续传** `curl -C -` + **停滞自动重试**」；镜像实测 **`huaweicloud` 不通**、`tuna` ~8 KB/s，**`files.pythonhosted` ~70 KB/s** ⇒ pip **直连 PyPI**、不换源。
- **Chrome 缺失 → Edge 兜底（`M7B-14`）本机首次实跑**：探针 `m7b09_common.py` 增 `browser_kind()` / `browser_exe()` / `browser_class()` / `env_proof()`（**标准安装路径**先探 Chrome、再探 Edge，命中谁用谁；两者皆无仍返回 `Chrome`，让库自己报启动失败、探针留真实错误）；`m7b28_doubao_recon.py` 三处 `Chrome(...)` 改 `common.browser_class()(...)`，JSON 增 **`env`** 物证字段、verdict 增 `env_browser_kind`。
- **验收（本机实跑 · 本地桩 · 临时 profile · headless · 零副作用、不需登录）**：`source\python\.venv\Scripts\python.exe source\python\_probe\m7b28_doubao_recon.py --selftest` → **PASS**（**exit 0 · 47 s**）：读回自证 `probe-ok` · `entry_kind=file_input` · `file_inputs=['#file']` · `#file` 命中 **1** · 收尾 `close_wait` **残留 0**。**环境物证**：`{'python': '3.12.10', 'pydoll': '2.27.0', 'browser_kind': 'edge', 'browser_version': '154.0.4258.48'}`。
- **文档落点**：[actionPlan/M7B.md](actionPlan/M7B.md) §11.1 增「**换机环境重建**」登记（新旧基线对照表 + 命令实测 + 网络取证）；[actionPlan/M7.md](actionPlan/M7.md) §12.1 **`P7b-01`** 加「**换机复证（Edge）**」注（headful 登录窗口 + `~/.brain-ai/pydoll-profile` **仍待 B0**）；本条目。
- **明确未含（口径不变）**：**`B0` / `B2` / `B3` 仍待人工登录一次**；`source/assets/providers.json` 的 **`web.attach` 仍不填**（`D11` 显式确认口径）；`DevPlan.todo` id **200 / 201** 保持 `done: false`；**本批不提交**变更集。
- **验证**：`py_compile` 探针 **2 文件退出码 0** · `providers.json` **JSON 合法（21 条）** · `.venv` **未入库** · `source/` 改动 = **旧遗留 2 项**（`install-deps.cmd` / `node_canvas.cpp`）+ 本轮 **`providers.json`**（B1 回填）。

**🔌 文档补齐：管道协议 v1 词表 + `web.attach` 契约 + `P7b-16` 解禁分派（2026-10-02 · 零产品代码改动）**

- **背景**：「网页版图片上传」（方案 A · 目标站 = 豆包）现状核对发现 **3 处施工图缺口** —— ① 上传完成的**证据形态**未定（`P7b-05b` 的 **B2 段**未跑）；② 管道协议只有「JSON 行 + 版本号」，**没有命令 / 字段词表**（C++ 与 Python 会各自猜字段）；③ C++ 侧「拆 `M5-02` 闸门」**无任务编号**、配置表**无字段**承载「上传入口形态」。**本轮补 ②③ 与 C++ 侧契约**（① 属实测，留在 B2）。
- **`M7B.md`**：[§6.1 管道协议 v1 词表](actionPlan/M7B.md)（**新增**：帧格式 `v` / `id` / `kind` · **7 命令** · **6 事件** · 错误码 → `I21` 映射 · 容量与超时上限 · 非法行处置）；`M7B-12` / `VB2-29` 补「按 §6.1 词表校验」；§7 配置表迁移增 **`web.attach`** 行；§11.2 追加同步说明；§15 记 **`v12`**。
- **`M7.md`**：§9 新增 **`D11`**（`web.attach` 契约 + `capabilities.vision=false` 的**显式确认**口径 —— 与 `D7-b` / `I18` 同族，不静默、不假装）；§12.2 新增 **`P7b-16`**（解禁与分派，含 **3 处落点必须同批** + 5 条验收）；§13 增「配置表」行；§14 增解禁分派基线 + 协议词表指针；§15 增「`M5-02` 闸门 3 处落点」；§16 顺序图串入 **B0 / B2 / B3** 与 `M7B-28` 豆包行（两条线汇合）。
- **登记**：[DevPlan.todo](DevPlan.todo) 新增 id **200**（`FEA-M7-10`）/ **201**（`TST-M7-08`）；父组计数 `M7（9 项）→ M7（10 项）`、`M7（7 项）→ M7（8 项）`。
- **明确未含（留后续）**：`providers.json` 的 `web.attach` **落值** 与 `doubao-web.notes` 订正（属产品资产 → 随 `P7b-05b` B2 / B3）；**B3 负结果分支处置**（待拍板）；**环境修复**（本机实测 Python **3.14.3**、无 `pydoll`、无 Chrome → 仅 Edge **154.0.4258.48**）—— ✅ **已于同日完成**，见上条「🐍 换机环境重建（路线 B）」。
- **验证**：`DevPlan.todo` **JSON 合法（201 条 todo）** · 文档断链自检 **broken 0** · `source/` 与 `source/assets/providers.json` **零改动**。

**📄 文档订正：豆包 B1 侦察结果回填 + 新示例登记 + 目录清理（2026-10-02 · 零产品代码改动）**

- **背景**：`P7b-05b` 的 **B1 段**（豆包 `doubao-web` 上传入口只读侦察）**已于 2026-09-29 20:33 实跑**并留下物证 `source/python/_probe/out/m7b28_doubao_recon.json`，但结论未回填（文档仍写「🟡 计划中」）→ 本轮逐处补齐。
- **B1 关键结论**：① **上传入口 = `paste_only`**（`file_inputs: []` / `drop_zones: []`）⇒ **`P7b-10` 注入路线定为 `DataTransfer` 构造 File + 合成 `paste`**（主路线 `expect_file_chooser` 对豆包**不适用**）；② **composer = `div.tiptap.ProseMirror`**（**未登录即渲染**、命中 1 可见 —— 更正「未登录不渲染输入框」旧结论）；③ **未登录已有 10 条匿名 Cookie**（仅名字）⇒ 决策 `D-30`「Cookie 非空 = 已登录」误报风险再获旁证；④ **`answer_selector` 4 类候选全 0** ⇒ 须「登录 + 手动发一条」；⑤ **B0 / B2 / B3 未跑** ⇒ **不构成 `I18` 证据**，网页版条目 `capabilities.vision` **保持 `false`**。⚠️ **已更正（同日稍后）**：B0 / B2 / B3 **已全部跑完**，且本条 ①「入口 = `paste_only`」**是小视口假象**（真入口 = `file_input`）⇒ 见上「🔍 探针判据纠错」条的「✅ 追加」段。
- **回填落点（4 处）**：[actionPlan/M7.md](actionPlan/M7.md) §12.1（状态列 + 新增「B1 段实测结论」块 + `P7b-10` 补注）· [actionPlan/M7B.md](actionPlan/M7B.md) §10（`doubao-web` 单列行 `input_selector` 更正 + 「登录 Cookie 存活口径」豆包行补登录前 10 条 + 「网页版图片理解侦察记录」表填 `paste_only` / `DataTransfer`）+ §15 **`v11`** · [DevPlan.todo](DevPlan.todo) `TST-M7-07`（**`done: false` 不变**）· [网页版协议实测记录.md](网页版协议实测记录.md) §8.3（新增 B1 结果表）。
- **新示例登记**：`source/workflows/examples/ImgPlusText.json`（**8 节点 7 连线**，2026-09-30 入库；文本链 + 图片链并排，未编 E 号）→ 写入 [ai_writer_nodes.md](ai_writer_nodes.md) §20.4 与 [节点编辑器使用说明.md](节点编辑器使用说明.md) §10.9（同时说明「示例工作流」按钮载的是**内置**示例，本文件须经「打开工作流」载入；`api_probe` 对它**尚无断言**）。
- **清理**：删除仓库根 **0 字节**误提交文件 `m7b28_doubao_recon.py`（真脚本在 `source/python/_probe/`）。
- **验证**：文档断链自检 **docs/ 全量 289 checked / 0 broken**（**本机无仓库自带的 `check_links.py`**，改用一次性脚本在 `%TEMP%` 完成、**不入仓库**）；`DevPlan.todo` **JSON 合法（199 条 todo）**、`TST-M7-07` 仍 `done: false`；`source/` 与 `source/assets/providers.json` **零改动**。

**🖱 画布新增：Alt + 左键拖拽平移（适配无中键鼠标 / 笔记本）+ 新增《变量 · 术语对照表》（2026-10-02）**

- **新增功能（既有绑定零改动）**：`source/src/ui/node_canvas.cpp` 新增 `update_alt_left_pan()` —— 按住 **Alt** 期间把画布「平移键」临时换成**左键**，同时把「拖节点 / 框选」临时挪到中键位（否则 Alt+左键拖拽会同时把节点拖走或拉出框选矩形）；**Alt 一松开立即原样还原**。右键平移 / 右键菜单 / 左键拖节点 / 左键框选在 **Alt 未按下时行为与之前完全一致**。
- **实现方式（不改 `third_party`、不新增依赖）**：imgui-node-editor 的平移动作 **每帧**读取 `Config::NavigateButtonIndex`（`imgui_node_editor.cpp:3326` `ImGui::IsMouseDragging(...NavigateButtonIndex...)`），因此只要在 `ed::Begin()` **之前**改写该字段即当帧生效；改写入口用**公开 API** `ed::GetConfig(g_context)`（`imgui_node_editor.h:285` → `imgui_node_editor_api.cpp:77` 返回编辑器内部**真实** `Detail::Config` 对象，非临时副本），Alt 按下时改写、松开还原。
- **新增标识符**（便于检索）：常量 `kNodeEditorDragButton` / `kNodeEditorSelectButton` / `kNodeEditorNavButton`（= 各键原始默认值）与 `kAltPanParkedButton`；状态 `g_alt_pan_active`；函数 `update_alt_left_pan()`。
- **界面与日志**：画布提示栏新增一行「Alt + 左键拖拽：平移画布」；处于 Alt 平移模式时该行**变为浅蓝色**提示「按住左键拖拽即可平移画布（松开 Alt 即恢复拖动节点/框选）」；切换时 Console 记录 `[画布] 进入/退出 Alt 平移模式：…`。
- **新增文档**：[docs/变量术语对照表.md](变量术语对照表.md) —— 中文术语 ↔ 代码标识符对照表（目录与命名空间、`ed::` 别名、图模型 `Graph/Node/Port/Param/Edge`、节点类型与**端口/参数 id**、画布交互与 `Config` 按键、**鼠标按钮编号**、执行/运行、提供商、`config.toml` 键 ↔ 字段、`~/.brain-ai` 数据目录、自检开关、命名与补丁编号约定、文档索引）。其中 **§12 专列「已知文档 ≠ 代码」**（含「文档写中键平移、v0.9.3 代码默认是右键」这一不一致的取证与处置），**§13** 是本次新增功能的检索入口。目的：解决「知道中文说法却搜不到代码」。
- **文档同步**：[docs/README.md](README.md) 文档地图新增该表；[docs/节点编辑器使用说明.md](节点编辑器使用说明.md) §3 鼠标操作表新增「Alt + 左键拖拽平移」一行。
- **验证**：`source/build.ps1`（Debug · VS 2026 x64）编译通过、**0 告警**、`build/bin/aiwrite.exe` 已重新链接（未跑基线复测）。

**📄 文档同步：方案 B′ 定案 + 站点多模态（图片）能力外部核查（2026-09-29 · 零代码改动）**

- **方案 B′ 定案（用户拍板）**：网页版**图片理解**的**首个目标站 = `doubao-web`（豆包）** —— 理由：条目**已存在**（`source/assets/providers.json` 的 `doubao-web`，`adapter=dom`、直连已实测 ✅）→ 换站**零新增条目 / 零自检改动**；豆包属字节多模态系，「目的地具备视觉理解」这条前提**更可能成立**；**不动 `M7B` 批 4 主线**（`deepseek-web` 继续做文字链路迁移 `M7B-06`/`M7B-26`）。
- **顺序闸门（新）**：先做**只读侦察**（`P7b-05b` → 新增一次性探针 `source/python/_probe/m7b28_doubao_recon.py`，**不进主管道**）→ 结论**经用户审核通过后**才启动 **C++↔Python 命名管道本体**（`M7B-02`/`04` = 方案 A）。B 的结论同时定 `P7b-10` 的注入路线（**`expect_file_chooser` 优先**，仅当站点无 `file input` 时才退 `DataTransfer`）与 `P7b-05` 的证据形态。
- **外部事实核查（修正两条旧假设）**：① **DeepSeek 官方 API 已支持真视觉** —— 2026-08-21 实验模型 → **2026-09-10 `deepseek-flash`（DeepSeek-V4.1-Flash：native multimodal visual understanding，Pricing 页 Vision ✓；`deepseek-v4-pro` 仍 ✗）**；官方 Vision 指南 = 「describe pictures / read text from screenshots / analyze charts」→ **真视觉，非 OCR-only**。**仓库 `deepseek` 条目未跟上**（`vision=false`、无 `deepseek-flash`、`notes` 仍写「官方 API 无视觉模型」）→ 登记 **`M7-10`**（⬜ 待办；**本次未改 `providers.json`**）；临时**零代码通道** = GUI「模型（自定义）」填 `deepseek-flash`（表外模型放行 · **未实测**，故不写入手册）。② **DeepSeek 网页版**：官方最后一次点名 Web 是 2026-08-13 的 **V4-Pro GA（Vision ✗）**，且 V4.1-Flash 公告**只提 API** → **推断「无真视觉理解」**（推断，非结论）；是否有上传入口**待实测**。③ **豆包网页版**：公开资料**未声明**图片理解 → **待实测**；证据出来前**不得**改其 `capabilities.vision`（`I18`）。
- **判定方法写死**：上传入口 4 类（`file_input` / `drop_zone` / `paste_only` / `none`）；视觉性质用「**纯图无字**」判别（能描述图形 / 颜色 / 构图 = 真视觉；只复述文字或答「看不到图片」= OCR / 非视觉）。
- **文档落点**：[actionPlan/M7.md](actionPlan/M7.md)（决策 `D10` · 任务 `M7-10` · 前置验证 `P7b-05b` · §16 顺序 · §12.1 外部核查注）· [actionPlan/M7B.md](actionPlan/M7B.md)（§10 `doubao-web` 单列 + 新表 + `v10`）· [网页版协议实测记录.md](网页版协议实测记录.md)（**新增 §8**）· [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md)（`D-30` 证据计划 + 附录 E + `v2`）· [README.md](README.md)（进度行 + 最近更新）· `DevPlan.todo`（新增 2 条）· [roadmap.md](roadmap.md) · [ai_writer_nodes.md](ai_writer_nodes.md) · [actionPlan/milestone_plan.md](actionPlan/milestone_plan.md)。
- **验证**：文档断链自检 `python vcpkg-cache/check_links.py` → **broken 0**；`source/` 与 `source/assets/providers.json` **零改动**。
- **✅ 追加（同日 · 用户实测通过）**：**DeepSeek 官方 API 视觉能力已实测通过** —— 上述「零代码临时通道」不再是"未实测"而是**已验证可用**；据此 `M7.md` 的 `M7-09` 改 🟡 部分（DeepSeek 视觉路径已过）、`M7-10` 改 🟡 部分（**只剩 `providers.json` 能力表声明 + 文档同步**）。口径见 [actionPlan/M7.md](actionPlan/M7.md) §3 与 [网页版协议实测记录.md](网页版协议实测记录.md) §8.1/§8.5。**证据（日志物证）**：`~/.brain-ai/logs/app.log` **2026-09-29 01:03** —— `[图片理解] 模型=deepseek-flash / 图片 1 张` → `完成：HTTP 200，输出 759 字节（6256 ms）`；下游 `n3`（文本生成）同模型出 747 字节，正文与图中场景一致（**真视觉理解**，非 OCR 复述）。

**📐 UI 修复：节点宽度改为「按输入/输出标签实测最宽自适应」（全画布同值，不再随字符长度变化）（2026-09-29）**

- **问题**：节点宽度此前是**内容驱动** —— ① 标题行（`标题 + (id)`）、参数预览行、`生效 …` 行、图片解码失败文案是**无收口的单行文本**；② 运行结果 / 必失败原因走 `PushTextWrapPos`（**依赖 ImGui 自动换行**，对中文这类"无空格长词"不可靠，且**截断补 `…` 时末行会超出上限** —— 自检实测抓到 **220 > 215 px**）。两者都会把节点撑宽 → 同一画布上节点宽度不一致，输入/输出端口边界随之漂移。
- **改动**（`source/src/ui/node_canvas.cpp`）：① 新增 **`clip_to_width()` / `elide_to_width()`**（按**像素**而非字符裁剪，回退时保证不切断 UTF-8 多字节序列；`elide` = `clip` 到「上限 − 省略号宽」再补 `…`，**结果恒 ≤ 上限**）；② 新增 **`wrap_to_width()`**：**自绘折行**（不依赖 ImGui 自动换行），中文/英文/混排都按像素断行，最多 `kNodeSummaryLines = 6` 行、字符预算 2000 字节，超出在末行补 `…`；③ **所有文本项统一收口**到「全画布共用的内容宽度 `g_content_width`」（= 按输入/输出标签实测最宽的**同一个值**）：标题 + `(id)`（`kNodeTitleIndent = 15px` / `kLabelGap = 4px`）、输入端口名、输出端口名（先收口再算 `spacer` → **输出圆点恒贴右边界**，不再退化成左对齐）、参数预览整行、`生效 …` 行、图片预览失败文案、运行结果 / 必失败原因（折行 + 6 行上限）；④ **宽度自适应**：新增 `port_row_width()` / `update_content_width()`，**每帧按当前图**取「所有节点里最宽的一行端口标签」并施加下限 `kNodeContentWidthMin = 160px` → **全画布共用同一个值**；标题行下 `Dummy(g_content_width, 1.0f)` 是**唯一**的宽度来源；⑤ **新增两处自检**（写 `app.log`）：**折行自检**（首次绘制：超长无空格串 / 纯中文 / 中英混排 / 自带换行 四类样例，最长行必须 ≤ 上限）与**节点宽度自检**（`ed::GetNodeSize` 采样，尺寸一变就记录；不一致时限流 WARN）。
- **效果**：画布上**所有节点等宽**（实测 **180 px** = 内容区 **160 px** —— 当前各节点端口标签均短于下限、故下限生效 + 2×10 内边距）；宽度**只由端口标签决定**，与标题 / 参数 / 运行结果的文本长度无关。
- **验证（`~/.brain-ai/logs/app.log` · 2026-09-29 20:06）**：`[画布] 折行自检：4 个样例，最长行 157 px（上限 160 px）→ 全部收口 ✅` · `[画布] 节点宽度自检：5 个节点，宽 180 ~ 180 px（内容区 160 px · 按输入/输出标签自适应）→ 全部等宽 ✅`（同一进程退出码 0、无 ImGui 断言；此前一轮自检抓到过 **220 > 215 px** 的末行溢出 bug，已由 `clip_to_width` 收口修掉）。
- **文档同步**：[ai_writer_nodes.md](ai_writer_nodes.md) §14.2 节点尺寸（**最小 200 / 最大 400 → 按输入/输出标签自适应、全画布同值** + 文本收口 / 自检规则）· [节点编辑器使用说明.md](节点编辑器使用说明.md) §人工验证清单 2b · [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §5「已完成」行。

**✅ M7 第二轮 P7-a 落地（v0.5.2 · 2026-09-28）—— 图片多图 + 统一资源目录 + official 收口 + UI A 档（`P7a-01`~`P7a-19` 全部完成）**

- **图片内部运行**：`image` 端口改 **变长**（`P7a-01` —— 只改端点声明，变长收集 / 校验机制引擎早已通用）→ 多个「图片输入」
  可同时连入「图片理解」；「图片输入」支持 **多选**（`P7a-02`：新增 `file_dialog::open_files`（NFD 多选）+
  `paths::split_path_list / join_path_list`（**换行分隔**、去空行 / 去首尾空白 / 去重 / 保序）+ 参数面板改「2 行输入框 + 浏览… / 多选… / 清空」）；
  多图请求体断言（`P7a-03`：`content` 含 **3 个 `image_url`**）
- **统一资源目录（新增 `source/src/utils/asset_store.{h,cpp}`）**：资源根 `~/.brain-ai/assets/images/`
  （`paths::assets_images_dir`，随 `ensure_data_dirs` 创建）；**内容寻址** `<SHA3-256>.<ext>`（同内容只存一份 —— 复用仓库既有
  `utils/crypto.h`，**零新依赖**）；工作流存 **令牌** `aiwrite-asset:<64 位摘要>`（`P7a-04` / `P7a-05`：**换目录 / 换机仍能取到图片**，
  实测**无需改 `workflow_io`**）；**旧绝对路径照旧可用**（不变量 `I19`）+ Console 迁移提示 + 参数面板「**迁移到资源目录**」按钮
  （失败项保留原值，**不静默改写用户文件**，`P7a-06`）；资源缺失 → 文案含 **资源目录完整路径 + 预期文件 + 下一步**，
  且**重新归档同一内容即可自愈**（`P7a-07`）
- **official 收口**：编码缓存（`P7a-08`：键 = 路径 + 修改时间 + 大小，**加锁**（执行器已线程化），上限 32 条 → 同图重跑不重复编码）；
  超限 **直接拒绝 + 可复制诊断**（含实际值 / 上限 / 下一步 / 配置项名，`P7a-09`）；**编码后体积与耗时**（`P7a-10`：新增
  `OfficialChatResult.encoded_bytes / encode_ms` 与 `human_bytes()`（KB/MB 自适应）→ 执行器 Console 与控制台自检都打印）；
  错误分类带标签 **路径 / 格式 / 体积**，400 用关键词表区分 **「内容策略」** 与「请求不合法」（`P7a-11`：`classify_http_error` 提为公开纯函数）
- **UI A 档**：⚠️ **行为变化** —— 默认布局改为 **节点库 + 参数面板常显**（`P7a-12`；**显式 `false` 的老配置仍保持隐藏**，`P7a-13`）；
  空画布引导（`P7a-14`）；参数面板分「**输入 / 参数 / 运行状态**」三组可折叠（折叠态经 `imgui.ini` **跨运行持久**，`P7a-15`）；
  图片节点卡片 = 缩略图 + `W×H` + **格式徽标**（`P7a-16`，格式由 `TextureInfo.format` 内容嗅探提供，不额外读盘）；
  错误条点击 → 视图 **居中并放大**到失败节点（`P7a-17`，`ed::NavigateToSelection`）；状态栏增「**提供商（id）· 模型**」与
  「**耗时**（累计 / 节点数）」（`P7a-18`）；输出面板缩略图 **点击放大**（`P7a-19`，弹窗每帧只绘一次避免 ID 冲突）
- **基线（P7-a 收口实测 · 2026-09-28）**：构建 **0 error / 0 warning** · `--selftest` **七组 PASS** ·
  `--exec-selftest` **251 → 310 / 0**（**只升不降，0 条旧断言被删**）· `--graph-selftest` **110 / 0** ·
  `--provider-selftest` **50 / 0** · `--run-selftest` **PASS** · `--vlm-selftest` 离线 **PASS**（无 Key `exit 2`）·
  文档断链 **266 / 0**；GUI 冒烟：启动 8s 存活 / 首帧完成 / 无 ImGui 断言
- 文档同步：[actionPlan/M7.md](actionPlan/M7.md)（§11 全 ✅ + §14 基线回填）· `DevPlan.todo` ·
  [ai_writer_nodes.md](ai_writer_nodes.md)（§7.1 / §7.2 / §20.2）· [节点编辑器使用说明.md](节点编辑器使用说明.md)（§2 / §4 / §10）·
  [../source/README.md](../source/README.md)（源文件表 + 资源目录）

**立项（M7 第三轮 `M7B` · 2026-09-28 · 计划未开工）—— 网页通道整体迁 Pydoll（WebView2 退场）**

- 用户 2026-09-28 决议 **7 条**（见 [actionPlan/M7B.md](actionPlan/M7B.md) §2）：**WebView2 退场**（嵌入控件易被站点识别为非真实浏览器）·
  **全部网页版条目一律走 Pydoll**（`builtin:deepseek` 协议栈退役）· **用 CDP `Network` 事件重建 SSE 增量**（保住逐字流式）·
  **只维护一套登录态**（单 profile `~/.brain-ai/pydoll-profile`）· **作废不变量 `I2`**（不再承诺「无参 CLI 逐字不变」→ 改由
  **`I20` CLI 契约**替代）· 缺 `--provider` → **列候选 + 退出码 2** + 新增显式关键字 `--provider auto`
- ✅ **新增不变量**：`I20`（CLI 契约）/ `I21`（**不静默降级**：任一依赖缺失即报错 + 引导，绝不换通道 / 换身份）/
  `I22`（**流式降级必须显式标注「非流式」**）；`I15 → I15′`（证据来源改 **CDP Cookie**）；`I16` 升级为**全局**
  （程序不再注入任何站点内部端点）
- ⚠️ **破坏性变更（计划）**：`--web-chat` / `--login-selftest` 将**需要 `--provider <id>`**；
  `--web-probe` **废弃**（由 `--web-adapter-selftest` + `--web-chat` 取代）；
  **老用户需重新登录一次**（旧 `~/.brain-ai/webview2` 登录态格式不同、**无法迁移**）
- 影响面：`source/` 内 **17 文件 ≈184 处** WebView2 代码进入撤除清单（§1.3）；`vcpkg.json` / `CMakeLists.txt` /
  `paths` / UI / `webview2_login` 工具全清 → **`PM-01`（WebView2 Runtime 检测）作废**；
  Python 运行时**升级为 M6 硬门槛**（`Q7`）
- 任务：**6 批** `M7B-10`~`M7B-41` + 前置验证 `M7B-01`~`09` + **`M7B-06b`**（含 **CDP 增量三路线实测**、**DeepSeek 网页版选择器实测**、**关闭时序与会话持久化**）；
  **先建后拆**（批 1–4 保留 WebView2，批 5 才删）；每批结束跑基线
- ✅ **`MB-D1` 已定（2026-09-28）：先建后拆** —— 批 1–4 保留 WebView2（可切回），批 5 才删；由此外加两条纪律：
  **批 1–4 只新增不删改**（`--exec-selftest` **只升不降**，任何下降即真回归）、**配置表收敛与删除类断言统一归批 5**
  （新增 `M7B-42` / `M7B-43`）。**回滚锚点** = 提交 `a1a7e0c` / 标签 `pre-m7b` —— 见 [actionPlan/M7B.md](actionPlan/M7B.md) §12
- ⚠️ **基线提示**：`--exec-selftest` 会**先降后升**（`I2` 作废删 `VB2-17` 前半与 `VB2-25①③④`；新增 `VB2-28`~`VB2-36` 共 9 条）——
  预期路径见 [actionPlan/M7B.md](actionPlan/M7B.md) §9.3，**不得误判为回归**

**阶段 0 完成（`M7B` 开工前置 · 2026-09-28 · 未动一行产品代码）**

- **基线复测（锚点 `pre-m7b` / `a1a7e0c` · Debug · 本机）**：`build.ps1` 编译成功 · `api_probe --selftest` **七组 PASS** ·
  `--exec-selftest` **251 / 0** · `--graph-selftest` **110 / 0** · `aiwrite --provider-selftest` **50 / 0**（`--provider-dump` 21 条）·
  `aiwrite --run-selftest` **PASS** · `aiwrite --run-selftest --web` **PASS** · 文档断链 **265 / 0**
  → 批 1–4 守门数字 = **251 / 110 / 50**（[actionPlan/M7B.md](actionPlan/M7B.md) §11.1）
- 🐛 **纠正文档错误**：`--provider-selftest` / `--provider-dump` 属 **`aiwrite.exe`**，**不是** `api_probe.exe`（原表写错）
- **Pydoll 环境就绪**：Python **3.12.1** + 项目内 **`.venv`** + **pydoll-python 2.27.0**（Chrome **156.0.8072.0** / Edge 存在）；
  **冒烟通过**（自动探测 Chrome → CDP 读 Cookie → `execute_script` → 关窗）—— 据此**修正三处计划假设**：
  网络日志/响应体有库级 API（`get_network_logs` / `get_network_response_body`）、文件注入优先
  `expect_file_chooser`（`DataTransfer` 降为备选）、`Edge` 兜底在 API 层已确认 —— 见 [actionPlan/M7B.md](actionPlan/M7B.md) §5
- ⚠️ **新增合规禁用清单**（Pydoll 自带能力，必须代码零命中 · 断言 `VB2-37`）：
  `expect_and_bypass_cloudflare_captcha` / `enable_auto_solve_cloudflare_captcha`（验证码规避）、
  `apply_fingerprint` / `FingerprintApplier`（指纹伪造）—— 见 [actionPlan/M7B.md](actionPlan/M7B.md) §13
- **回滚锚点已建立**：标签 `pre-m7b` → 提交 `a1a7e0c`（已推送）；`.gitignore` 补 Python 运行时忽略项

**前置验证进行中（`M7B-01`~`M7B-09` · 2026-09-28 · 仍未动一行产品代码）**

- ✅ **已过 5 项**（脚本 + JSON 证据在 `source/python/_probe/`，一次性、不进主管道）：
  - **`M7B-01`**（headful + 单 profile + CDP 读 Cookie）：profile 落 `~/.brain-ai/pydoll-profile`（1064 文件，含 `Cookies` 库）；
    **CDP 读回含 `httpOnly` Cookie，而 `document.cookie` 看不到它**；**重启同 profile 后持久 Cookie 仍在**、会话 Cookie 正确消失
  - **`M7B-03`**（CDP 增量流式 · `R2` 主闸门）：主路线 = **`Fetch.takeResponseBodyAsStream` + `IO.read(size=128~256)`**（`size` 即推送粒度，实测 256 B/帧、41 帧 / 10 KB 流）
  - **`M7B-05`**（页面驱动）：**154 ms/字符**逐字输入（21 次 input 事件）+ 文件注入 **两条路径都成功**（`expect_file_chooser` / `DOM.setFileInputFiles`，后者触发 change）
  - **`M7B-08`**（单 profile 多站点 + 按 origin 注销）：两域并存 → `Storage.clearDataForOrigin(A)` 后 **A 空 / B 完整**；⚠️ Cookie **按域名隔离、端口不参与**
  - **`M7B-09`**（关闭时序 / 持久化 / 接管 · `MB-D0-8` L1~L4 闸门 · 四轮 V-a~V-d）：
    ① **干净退出保住持久 Cookie**，`Browser.close` → 等进程退出只需 **0.22 s**；强杀与库默认 `stop()` 会丢（且残留 `Cookies-journal`）；
    ② 写 Cookie 后 **≥30 s** 再强杀仍存活 ⇒ **10–30 s 延迟提交窗口**；
    ③ 外部关窗（等价用户点 X）**干净**且 Cookie 保住，随后 `stop()` 抛 **`BrowserNotRunning`（实现层必须吞）**；
    ④ **会期 Cookie 干净退出必掉**（每 case 全新 profile 对照实证），`--restore-last-session` 能保但**会重开上次标签页**（副作用）⇒
    **改用 L2 加密快照**：回灌后 `session`/`httpOnly` 属性保持且**服务端 `/eyes` 认账**（端到端已验）；
    ⑤ profile 被占时**同端口 = 静默附着到既有实例 / 随机端口 = `FailedToStartBrowser`**，`browser.connect(ws)` **attach 可行**
    ⇒ **启动前先探端口，禁止强杀**
- ⏳ **待过 4 项 + 1 项加强**：`M7B-02` / `M7B-04`（C++ ↔ Python **命名管道双向 IPC**；C++ 工具链冒烟已通
  ⇒ 剩管道本体）、`M7B-06` / `M7B-07`（DeepSeek 网页版选择器 / 会话失效证据来源，**需真站点人工登录**）、
  `M7B-06b`（真站点「重启仍登录」+ 回填 §10「登录 Cookie 存活口径」表）
- ⚠️ **一条计划级发现（已改进计划 · 决议 `MB-D0-8`）**：Pydoll 的 `browser.stop()` 是
  **「发 `Browser.close` 后立刻 `terminate()`（硬杀）」** → Chrome 来不及把 Cookie 库落盘 →
  **登录态每次退出即丢**（实测：强杀 = 丢 / `close` + 等 3 s = 保住；源码证据
  `pydoll/browser/chromium/base.py:223-243` + `browser/managers/browser_process_manager.py:73-89`）。
  用户拍板 **四层保障全做**：**L1 干净退出**（`Browser.close` → 等进程退出，默认 5 s，超时才强杀）·
  **L2 DPAPI 加密 Cookie 快照**（启动 `Network.setCookies` 回灌，抗崩溃/断电）· **L3 启动自愈**（profile 被占 →
  优先优雅接管，禁止为启动而强杀浏览器）· **L4 异常退出可见**（埋点 + 一键重登/恢复）
- ✅ 文档同步：新增不变量 **`I23`**、断言 **`VB2-38`/`VB2-39`**、前置验证 **`M7B-09`**（关闭时序三档 + 提交时机 + 快照往返）
  与 **`M7B-06b`**（真站点「重启仍登录」+ 记录登录 Cookie 的 `expires`）、开口项 `MB-Q4`/`MB-Q5`/`MB-Q6`、
  §13 登录态快照红线；`source/README.md` 新增 **§6.2 一次性探针脚本纪律**（读回必须自证）—— 见 [actionPlan/M7B.md](actionPlan/M7B.md) v5 / v6 变更记录
- ✅ 文档同步（v7 · `M7B-09` 收口）：不变量 **`I23⑤`**（保活手段不得改变用户可见行为）；§13 新增
  **启动与登录态两条"不做"**（禁止为启动而强杀既有实例 / 禁止默认使用会重开上次标签页的开关）；§5 实施要点 **+⑨⑩⑪**
  （登录态读数作用域 / profile 被占两形态 / `--restore-last-session` 默认禁用）与 **`M7B-09` 结论块**；
  §14.2 **`MB-Q6` 结案**（默认不写 `Preferences`，会期 Cookie 交 L2）；`source/README.md` §6.2 **+3 条探针纪律**
  （读数作用域 / `try/finally` 收尾 / 一因多果逐项排除）—— 见 [actionPlan/M7B.md](actionPlan/M7B.md) v7 变更记录
- ✅ 文档同步（v8）：**L2 快照节奏**由 V-a 的提交窗口数字收口 —— 刷新间隔 **< 10 s** + **登录成功即写** /
  **优雅退出前再写**（否则崩溃时快照旧一个窗口）；探针侧固化**残留自检**（`m7b09_common.shutdown()` 返回
  `strays_after` / `strays_killed`）—— 见 [actionPlan/M7B.md](actionPlan/M7B.md) v8 变更记录

**文档（新增）：`docs/roadmap.md` —— 长期路线图 v1.0（一页纸）**

- 确立定位与边界：**面向非技术用户**（后期可能转商业）· **滚动发布**（版本号内部保留）· **数据全本地**（仅 API 请求出网）·
  **Windows only 但预留跨平台**（IPC 抽象，不写死 Win32）
- 决策 `RM-D1`~`RM-D6`：`PB2-24` 自建站点 UI 闭环**提升为面向非技术用户必做**（用户不接触 JSON）·
  交付形态 = **一键安装包**（非绿色版 zip，形态待确认）· 滚动发布形态**待确认**
- 阶段与依赖：P7-a（当前）→ P7-b + **面向 C 前置项**（并行：`PD-03` / `PM-01` / `P7b-15` / `PB2-24`）→ **M6 一键安装包（v1.0）**
  → 滚动发布（v1.x）→ 商业候选（v2.0+，不排期）；P7-b 独立回滚
- 附：风险与回滚点 · 不做清单（硬约束）· 待确认 `RQ-1`~`RQ-4` · **"与既有文档的差异"待同步清单（5 条）**
- 索引：`docs/README.md` 文档地图 + `CHANGELOG.md` 相关文档索引 + `actionPlan/milestone_plan.md` 头部交叉引用已同步

**计划（M7 第二轮 P7 · 2026-09-27 会议决议 `D1`–`D9` · 未开工）—— 图片内部运行 / 网页版图片上传 / UI 优化一版**

- 分层：**P7-a = v0.5.2**（**图片内部运行**：`image` 端口改 **variadic 多图** + **统一资源目录** / 资源引用；
  **official 收口**：超限直接拒（不自动缩）、打印 base64 后体积与耗时、错误分类；**UI A 档**可用性）·
  **P7-b = v0.5.3（待确认）**（**网页版图片上传**：方案 A = **Pydoll 独立浏览器 + Python 守护进程 +
  命名管道 + Win32 事件**；行为模拟 `humanize`；失败报错 + 显式重试，不静默降级）
- 依据与细则：`docs/actionPlan/M7.md` **§8–§17**（决策 `D1`–`D9` / 任务表 `P7a-01~19`·`P7b-01~15` /
  影响面 / 验证基线 / 开口项 `Q1`–`Q8`）
- 新增不变量（**计划**）：**`I18`** 网页版图片「**无上传完成证据不得自动发送**」（仅用户本次运行显式
  选择「不含图片继续」可降级，且必须记录「本次未含图片」）· **`I19`** `image` 端口语义升级须**向后兼容**
  旧工作流 —— 已登记进 [../source/README.md](../source/README.md) §4.2
- 看板：`docs/DevPlan.todo` M7 组续号 **id 176–188**（`FEA-M7-04`…`ARC-M7-02`）
- 交付顺序：P7-a（三条线并行）→ 跑基线 → P7-b **前置技术验证**（未过即回滚）→ P7-b 实现 → 再次跑基线；
  **落地后先评审、不自动提交**
- **本次落档验证（2026-09-27，纯文档）**：`build.ps1` **0 error / 0 warning**（未触及 C++ 源码）·
  文档断链 **199 checked / broken 0**（旧 190 + 本次新增 9 条链接）· `docs/DevPlan.todo` 新增 **13 条**
  （id **176–188**，五类 M7 组同步计数；Python `json` 与 PowerShell `ConvertFrom-Json` **双解析器 PASS**，
  id 唯一、最大 188）· 全部改动文件 **CRLF + 无 BOM**

**新增（M7-04/05 · 图片格式嗅探与解码）：不再相信扩展名 + 本地支持 WebP + 可操作报错**

- `utils/image_decode.{h,cpp}`（新）：魔数嗅探 `sniffImage` / `sniffImageBytes`（PNG/JPEG/GIF/BMP/**WebP**/TIFF/ICO/JXR/HEIF/AVIF/PSD/HDR/PNM，纯函数版本供离线断言）+
  `decodeImageRgba8`（**stb 优先 / WIC 兜底**，统一 RGBA8、行序与 GL 上传一致）+ `readImageSize` +
  `wicInfo`（解码器**元数据枚举**）+ `decodeErrorMessage`（可操作文案：实际格式 + 装扩展/另存为建议 + 文件头 + 「本地预览失败不影响发给模型」）
- `ui/texture_cache.cpp`：`texture_for` / `image_size` 改调上述模块（**对外接口不变 ⇒ 输出面板 / 参数面板零改动**）+ GL 纹理上限检查（超限给明确提示）
- `ai/deepseek_official_provider.{h,cpp}`：新增 `image_mime_from_bytes`；`image_mime_from_path` 改「**内容优先** → 扩展名回退」；扩展名表补 `.tif/.tiff/.ico/.heic/.heif/.avif`
- 实测：`F:\ims.png`（扩展名 `.png`、内容实为 WebP，2048×2048）→ 嗅探 `WebP` / MIME **`image/webp`**（旧实现谎报 `image/png`）/ 本地解码 **`wic` OK**
- 断言：`api_probe --exec-selftest` **237 → 251 通过 / 0 失败**（新增 15 条：嗅探 / MIME / 解码 / 尺寸 / 文案）
- 诊断入口：`api_probe --image-decode <路径>`（8 行结论；exit 0 = 本地可预览 / 2 = 不可预览）

**修复（M7-D7 · 冻结区不变量 `I17`）：WIC「合成文件头探测」导致 CRT 断言崩溃（退出码 3 / `0x80000003`）**

- 症状：`api_probe --exec-selftest` 与「参数面板渲染 WebP 图片」进程**突然结束**（退出码 `3` / `-2147483645`），
  输出戛然而止、`app.log` 无"结束"行；`try/catch` 与 SEH 都抓不到（`_CrtDbgReport` 直接中断）
- 根因：初版用「30 字节**合成** WebP 头」交 WIC `CreateDecoderFromStream` 探测能力；本机装有
  `Microsoft.WebpImageExtension 1.2.31.0` → 解码器**接受**该假头后解析比特流 → 断言/中断
- 修复：能力探测改**解码器元数据枚举**（`CreateComponentEnumerator` + `IEnumUnknown` + `IWICComponentInfo::GetFriendlyName`，
  **不解析任何比特流**）；删除 `wic_accepts_bytes` / `synthetic_webp_head` / `synthetic_ftyp_head`；
  **解码分派收紧**（仅 WebP/TIFF/ICO/JXR/HEIF/AVIF 进 WIC；未知与 stb 覆盖的格式一律不进）；失败文案**不依赖**探测结果
- 冻结：`utils/image_decode.{h,cpp}` 的 WIC 相关代码冻结 + `#define AIWRITE_IMAGE_DECODE_FROZEN 1`
- 定位方法（已写进 [../source/README.md](../source/README.md) §6.1）：带 `fflush(stdout)` 的 `[TRACE]` 标记 + 二分，配合 `app.log` 最后一条

**变更（M7-01~03 · 节点精简）：删除图片输出节点 N-09（`ImagePreview`）—— 9 → 8 节点**

- 注册表块 / 执行器 `execute_image_preview` / `nodes.h` 声明 / `register_executors` 注册全部移除；
  图片结果仍由 `ImageInput` 的 `image` 端口经 `RunNodeView.images` 在输出面板与参数面板渲染缩略图（M5-03 通道未受影响）
- 连带：`api_probe` 计数与用例同步（注册表 9→8、类型数组、输出类 2→1、端口不兼容用例改用 `VLMGenerate.image`、
  快照用例改为 ImageInput 独立跑 + 纯文本节点 `images` 为空）；注释与文档计数（`source/README.md` §7 等）
- 回归：`--graph-selftest` **111 → 110 通过 / 0 失败**；`--selftest` 七组 PASS；`--run-selftest` PASS；
  `--vlm-selftest` 离线断言 PASS；构建 **0 error / 0 warning**

**文档（M7）**：新增 [actionPlan/M7.md](actionPlan/M7.md)（§0 结论 / §1 **事故记录** / §2 现状勘误 / §3 任务 / §4 决策 / §5 影响面 / §6 基线 / §7 遗留）；
[../source/README.md](../source/README.md) 新增 **§4.2 图片诊断与图片解码冻结区**、**§6.1 崩溃 / 断言错误排查（CRT assert · 退出码 3）** 与 `--image-decode` 命令，并刷新自检项数

**文档（补丁系列拆分与归档）：新增 `M_patchAB_rest` 承接剩余项 + `M_patchA` / `M_patchB` 归档**

- **拆分**：新建 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) —— 把 `M_patchA`（Patch B 残项 / 原 Patch C 数据安全 / 原 Patch D 交互打磨 / PM）与 `M_patchB`（L2 `PB2-04`…`PB2-12`、`PB2-07` 界面按钮、`PB2-25`、`PB2-29`/`PB2-30②`、`PB2-24`）的**全部未完成项**收编为**唯一执行入口**；**编号不改号**（`PA-`/`PB-`/`PC-`/`PD-`/`PM-`/`PB2-*` 沿用）；刷新回归基线（构建 0/0 · `--exec-selftest` **237/0** · `--provider-selftest` 50/0 · `--graph-selftest` 111/0 · 文档断链 0）；附录 B/C/D/E 复制为**现行版本**（附录 E 就地维护「窗口可达 / 可登录 / 可生成」三列）；承接决策 `D-25`…`D-30` 并新增 `D-31`（流式方案）/ `D-32`（逐站范围）。
- **归档**：`git mv` → [Archive/actionPlan/M_patchA.md](Archive/actionPlan/M_patchA.md) · [Archive/actionPlan/M_patchB.md](Archive/actionPlan/M_patchB.md)（顶部加 `📦 已归档（2026-09-27）` 横幅 + 「未完项承接」指引；相对链接层级修正；**正文规格与各批次实测记录原样保留**）。
- **同步**：本文索引、[README.md](README.md)（进度行 + 文档地图 + Archive 行）、[Archive/README.md](Archive/README.md)（归档索引 + 待归档候选）、`actionPlan/milestone_plan.md`、`actionPlan/M3.md`·`M4.md`、`ai_writer_nodes.md`、[节点编辑器使用说明.md](节点编辑器使用说明.md)、[网页版协议实测记录.md](网页版协议实测记录.md)、[../source/README.md](../source/README.md) 的引用改指**承接文档 / 归档路径**；`DevPlan.todo` 的 `fileLink` 按「**已完成 → 归档；未完成 → 承接文档**」更新，并登记 `ARC-M3-18`/`ARC-M3-19`。
- **验证**：文档断链自检 `python vcpkg-cache/check_links.py` → **broken 0**；`DevPlan.todo` JSON 可解析且全文件 CRLF；**未改任何代码**。

**修复（M_patchB L4 / `B2-e` 第一批 `PB2-27`）：登录/会话层去 DeepSeek 化 —— 站点无关判据 + 文案**

- **问题（用户实测）**：选任一非 DeepSeek 网页版条目 → 面板恒显示「该站点无法自动探测凭证」「userToken：未获取（请先登录）」，
  点运行空等 15/25 秒后报「未取得网页版凭证（内存里没有 userToken）」→ 用户读作「所有 AI 都无法登录」。
  根因：登录态判据是 DeepSeek 专有物（`SessionStore::has_token()` = `user_token` 非空）→ 通用站点恒 false。
- **新增纯函数（`ai/provider_spec.{h,cpp}`）**
  - `web_session_state(spec, evidence)` → `{unknown / logged_in / logged_out}` + **站点无关**原因串；
    判据 = 条目 `cookie_names` 命中 **∪** 该 origin Cookie 非空（决策 `D-27`）；**不看** `userToken` / `ds_session_id` / `/api/v0/*`（不变量 `I15`）
  - `probe_is_applicable(web/spec)`：内置适配器适用；`dom` 站点仅在显式配了 `probe_paths` / `token_expr` 时适用，否则**不适用**（不变量 `I16`）
  - `web_shows_user_token(spec)`：`userToken` 行**仅当**条目配了 `token_expr` 才显示（`D-28` ①）
- **`web/session_store.{h,cpp}`**：`web_session_evidence(session)`（Cookie 维度证据；`cookies_known` = 该站点写过会话快照）
- **`web/webview_host.{h,cpp}`**：`LoginRequest.probe_applicable`（默认 `true` → CLI / 无参路径**逐字不变**，守 `I2`）；
  `ensure_session()` 对 DOM 站点改判**该 origin 有没有 Cookie**（不再空等 `userToken`），且不触发协议探测，超时文案站点无关；
  **删除 `kDefaultCookieName`**（`web/**` 不再出现 `ds_session_id`）
- **界面（`ui/property_panel.cpp` / `ui/app.cpp`）**：状态行 → 「已登录（该站点，Cookie N 条）」/「未登录（该站点）」/「未确认（该站点）」+ 原因 + 引导；
  `userToken` 行按 `D-28` ① 渲染；「探测错误」红字只对**适用**站点显示；状态栏按**生效条目自己的站点键**判状态；`ui/**` 零 `ds_session_id`
- **文案（`ai/provider_spec.cpp`）**：字段警告「无法自动探测凭证」→「**协议探测不适用**（DOM 站点…）」；加载报告 →「登录可用；协议探测：不适用（DOM 站点）」
- **断言/回归**：`api_probe --exec-selftest` **219 → 227 通过 / 0 失败**（`VB2-24` 5 项 + `VB2-26` 3 项全 PASS）；
  `--graph-selftest` **111/0**；`--provider-selftest` **50/0**；构建 **0 error / 0 warning**；`grep ds_session_id`（`ui/**`+`web/**`）零命中
- **仍未做**：`PB2-28`④（会话失效识别，归 `PB2-25`）、`PB2-29`（11 站选择器逐站实测回填）、`PB2-30`（`--run-selftest --web --provider <id>` + 面板「测试选择器」+ `PB2-24` 自助闭环）

**修复（M_patchB L4 / `B2-e` 第二批 `PB2-28`）：协议探测「不适用」语义 —— 只读诊断 + CLI 判据**

- **问题**：`probe_paths` / `token_expr` 为空时探测脚本**保持内置 DeepSeek 端点与 `userToken` 读取** → 在 Kimi / 通义等站点必然 404
  → 面板红字「探测错误」，用户读作「登录失败」。
- **改动**
  - `web/webview_host.cpp`：新增 `kProbeKickoffScriptReadOnly` —— 只读 `location.href` / `document.title` / `localStorage` **键名与个数** /
    `document.cookie` **名与个数** / 输入框与按钮候选数；**脚本内既无 `/api/v0/` 也无 `localStorage.getItem('userToken')`**；
    输出字段与内置脚本同名 → `poll_protocol_probe()` 解析零改动
  - `probe_kickoff_script()`：按 `LoginRequest.probe_applicable` 分支（默认 `true` → 内置站点 / `--web-probe` / `--web-chat` / `--login-selftest` **逐字不变**，守 `I2`）
  - `protocol_probe_with(base, spec, label, timeout)`：不适用站点打印「协议探测：**不适用**（该站点不是内置协议站点）→ 只读诊断」+
    诊断摘要 + **站点无关**登录态结论（`ai::web_session_state`；退出码 0=已登录 / 2=未登录或未确认 / 1=诊断失败），**不再打印「未取得凭证」**
  - `ui/property_panel.cpp`：按钮标题按适用性切换为「探测网页版协议（dev）」或「**只读诊断（该站点不适用协议探测）**」+ 对应 tooltip
- **断言/回归**：`api_probe --exec-selftest` **227 → 232 通过 / 0 失败**（`VB2-25` 5 项全 PASS）；`--graph-selftest` **111/0**；`--provider-selftest` **50/0**；
  构建 **0 error / 0 warning**；⚠️ CLI 文案 / 退出码与面板按钮需 GUI 现场实测（`--web-probe --provider kimi-web`）
- **仍未做**：会话失效识别（`40002`/`401`/`40003`，归 `PB2-25`）；`PB2-29`（选择器逐站实测）；`PB2-30`（端到端自检 + 面板「测试选择器」+ `PB2-24`）

**修复 + 新增（M_patchB L4 / `B2-e` 第三批 · v15）：崩溃修复 + 会话失效识别 + `--web-dom-dump` 选择器枚举 + 逐站直连实测**

- **修复 CLI 崩溃 `0xC0000409`（`STATUS_STACK_BUFFER_OVERRUN` / `__fastfail`）** —— 两个独立原因：
  - **调用方式**：`aiwrite.exe` 是 GUI 子系统程序，PowerShell `*>`/管道重定向会与其 `attach_parent_console()`+`freopen("CONOUT$")` 冲突 → 用
    `Start-Process … -PassThru -Wait -RedirectStandardOutput <file>`
  - **真实缺陷**：`ExecuteScript` 对「脚本返回字符串」会**再包一层 JSON 字符串** → `json::parse` 得 `string` → `value()` 抛 `type_error.306` →
    未捕获 → `std::terminate`/`__fastfail`（stdout 缓冲随之丢失，表现为「无输出 + 崩溃码」）→ **已统一解包 + 整函数 try/catch 兜底 + 关键节点 `fflush`**
- **`PB2-28`④ 会话失效识别（并入 `PB2-25`）**：`ai::web_session_failure_hint()` / `web_session_failure_needs_relogin()`（纯函数；`code=40002`/`40003` 与 HTTP `401`/`403`，容错 `"code": 40002` / `"biz_code":40003` 写法）；
  `web_chat()` 命中 `401`/`40002` → 文案追加「重新登录该站点」并**作废该站点内存会话**；`40003` → 仅提示
- **`PB2-30`①**：`--run-selftest --web --provider <id>`（表外 id / 非 `kind=web` / 站点不可用 / 登录型条目 → 退出码 1/2，严格解析不回落）
- **新增 `--web-dom-dump --provider <id>`（只读）**：枚举页面候选输入框 / 发送 / 回答容器（`id`/`class`/`placeholder`/`aria-label`/`contenteditable`/可见性/文本）
  并给**建议选择器** —— 逐站填选择器不再依赖人肉 F12（`PB2-29` 执行工具）
- **断言/回归**：`api_probe --exec-selftest` **232 → 237 通过 / 0 失败**（`VB2-27` 5 项全 PASS）；`--graph-selftest` 111/0；`--provider-selftest` 50/0；构建 **0 error / 0 warning**；
  `--web-dom-dump --provider kimi-web` exit 0（实测读出 `div.chat-input-editor` 等真实候选）
- **实测发现（待拍板 `D-30`）**：未登录的 Kimi 也有匿名 Cookie → 仅按「该 origin Cookie 非空」会误报「已登录」；建议改「`cookie_names` 命中优先 + Cookie 非空降级为『未校验』」，并为每站补 `cookie_names`
- **逐站直连实测（12 条 web 条目，2026-09-27）**：用 `--web-dom-dump` 逐站跑（只读）
  - **12/12 条目的登录窗口都真的打开了各自站点**；3 条域名按实测修正为**最终域**：`kimi-web`→`www.kimi.com`、`tongyi-web`→**`www.qianwen.com`**、`ernie-web`→`wenxin.baidu.com`
  - **输入框选择器实测 4 条并写回 `assets/providers.json`**：`kimi-web`=`div.chat-input-editor`、`qwen-web`=`textarea[placeholder="Ask Qwen"]`（发送 `div.message-input-right-button-send`）、`yuanbao-web`=`div.ql-editor.ql-blank`、`ernie-web`=`#chat-textarea`（`--provider-dump` 确认这 4 条只剩 `answer_selector` 未就绪）
  - `chatgpt-web` / `claude-web` / `gemini-web` 本机网络**可直连**；`chatglm-web` 首屏为 **WAF 挑战页**；`doubao-web`/`spark-web` 未登录不渲染输入框
  - 完整逐站表格见 [网页版协议实测记录 §7.9](网页版协议实测记录.md)

**界面（仅 ImGui 层 · v16）：提供商下拉「合并显示」—— 去掉 `-web` 重复项**

- **问题**：下拉里同一家 AI 出现两项（`deepseek` 与 `deepseek-web`、`gemini` 与 `gemini-web` …），`-web` 后缀像内部 id，容易选错。
- **实现（全部在 `ui/property_panel.cpp`；`ai/**` / `engine/**` / `nodes/**` / `web/**` 与配置表零改动）**：
  - `build_provider_choices()`：① 同名后缀规则 `xxx ↔ xxx-web` ② 品牌别名 `openai↔chatgpt-web`、`anthropic↔claude-web`、`zhipu↔chatglm-web`
    ③ 无孪生的 web 条目独立成项但**显示名去掉 `-web`**（`kimi` / `tongyi` / `qwen` / `doubao` / `yuanbao` / `ernie` / `spark`）④ 官方条目原样
    → **下拉里不再出现 `-web` 字样**
  - 「提供商」参数改用合并列表渲染；选中后写入**当前模式对应的真实 id**（`web` → `xxx-web`，`official` → `xxx`）
  - `sync_provider_id_with_mode()`：切换「模式」时把 `provider` 配对到该条目的官方版 / 网页版 id（`deepseek ⇄ deepseek-web`、`openai ⇄ chatgpt-web` …），写日志、可撤销
  - 合并结果变化时打印一次诊断：`[提供商标] 下拉合并结果（界面层）：…`
- **关键性质**：写进节点的 id 永远是**配置表里真实存在的 id** → 生效解析 / 网页版会话键控 / `--provider-*` 自检 / 工作流 JSON / `--run-selftest --web --provider <id>` 全部无需改动。
- **结果**：21 条表项 → **16 个下拉项**；实测构建 **0 error / 0 warning**、`--provider-selftest` **50/0**、`--exec-selftest` 237/0、`--graph-selftest` 111/0。
- **✅ 完成（用户界面验证通过 · 2026-09-27）**：下拉无 `-web` 重复项、合并行为与上方表格一致 → 本项标记完成（DevPlan 任务 **160** `done: true`）。

**特性（M_patchB L1 收口）：网页版站点身份按「生效条目」+ 多站点会话并存 + `config.toml` 多 provider**

- **`PB2-05` 补完 = `PB2-17` 网页版登录入口去硬编码（不变量 I11）**
  - `web/webview_host.h`：`LoginRequest` 增 `provider_id/token_expr/cookie_names`；新增 `login_request_of()` 与
    `interactive_login_request(site,id)` / `probe_login_request(site,id)` / `boot_login_request(site,id)`
    （**空字段回落内置默认**）；无参 `interactive_login_request()` 与旧常量**逐字一致**（保 I2）
  - `ui/property_panel.cpp`：`draw_web_session_section(node, graph)` **按生效条目**渲染 —— 站点条目/来源、
    登录页、适配器、Cookie 名（取代写死的 `ds_session_id`）、登录/探测/注销全部针对**该站点**；非网页版条目给明确提示
  - `nodes/local_nodes.cpp`：网页版分支改用 `ai::web_spec_for()` 取站点参数（未选网页版条目 → 内置默认站点），
    并按站点 `ensure_session`；Console 打印生效站点与适配器
  - `engine/node_registry.cpp`/`provider_resolve.{h,cpp}`：「模式」下拉**恒提供 `official` / `web` 两项**（**网页版与官方 API 同等优先级**）
    —— ⚠️ 收口时曾按条目 `kind` 收窄候选（official→仅 official / web→仅 web）并「web 条目自动锁 web」，已判定为**回归**；
    按决策 `D-21` **已撤回并落地修订**（见下方 v7 条目 / `docs/actionPlan/M_patchB.md` §9.1 / 任务 `PB2-20`）
  - `engine/validate.cpp`：运行前提示按**生效条目**生成（含「该条目不是网页版条目 → 将使用内置默认站点」的明确告警）；`PB2-20` 起按**该节点自身条目**解析（`resolve_display_provider()`）
  - `tools/api_probe.cpp`：把绑死硬编码的断言 `interactive.url.find("deepseek.com")` 改为**表驱动双向断言**（见 VB2-17）
- **`PB2-18` 多站点会话并存（不变量 I12）**
  - `web/session_store.{h,cpp}`：新增 `site_key_of(url)`（**origin**，忽略路径/查询/大小写）+ `Session.site/provider_id`；
    存储改为 `std::map<站点键, Session>`；新增 `snapshot(site)` / `set_probe(probe,site)` / `has_token(site)` /
    `clear(site)` / `clear_all()` / `sites()` / `current_site()`；**旧无键 API = 「默认槽」薄封装**（改造前语义不变）
  - `web/webview_host.cpp`：`write_session()` 按站点归档；探测结果按站点写入；新增 `current_window_site()` / `window_on_site()`
  - `nodes/local_nodes.cpp`：按**站点**取会话 → 两个网页版条目**各用各的凭证**（互不覆盖）
- **`PB2-19` 窗口串行复用 + 按站点注销**
  - `ensure_session(site_request,…)`：窗口必须开在**目标站点**上（页面内 PoW 依赖该站点页面）；站点不一致时
    **先关旧窗再按目标站点开窗**（决策 D-20）；站点已就绪时幂等直返
  - 新增 `logout_site(site_request,…)`：清该站点内存会话 + 用 `ICoreWebView2CookieManager` 删**该 origin** 的 Cookie
    + 清同源 `localStorage/sessionStorage`（窗口线程内执行，`kLogoutMessage`）；**不动其他站点**
  - `ui/property_panel.cpp`：「注销该站点」按钮 + 「已登录站点」列表（逐站点注销）+ 「高级：删除整个 profile」（二次确认，`clear_all`）
- **`PB2-06` `config.toml` 多 provider 实例参数**
  - `utils/config.{h,cpp}`：`Config::providers`（`std::map<id, Provider>`）+ 读写 `[providers.<id>]`；
    **旧单节自动迁移**（幂等）；`deepseek` 旧成员与 `providers["deepseek"]` 互为镜像（保存时旧成员为权威）
  - 保存改为「先写 `.tmp` → 原子替换」，并**写盘前备份 `config.toml.bak`**（写失败不覆盖原配置）
- **断言/回归**：`api_probe --exec-selftest` **190 → 204 通过 / 0 失败**（新增 `VB2-17` 4 项 + `VB2-16` 6 项 + `VB2-18` 4 项；「模式过滤 1 项」已按 `D-21` 撤回并改写为「候选恒含 `web`」，见下方 v7 条目）；
  `api_probe --selftest` V-08 增 `PB2-06` 断言（旧单节迁移 / 新增条目往返 / `.bak` 生成）；`--graph-selftest` 111/0 不变；
  `--provider-selftest` **50/0**；构建 **0 error / 0 warning**（4 目标）
- **实测基线（全绿，2026-09-26）**：`--run-selftest` PASS · `--run-selftest --web` **5/5 / 0 失败 / 0 跳过（5.15s）** ·
  `--web-session-selftest` PASS（userToken 64 位）· `--web-probe` PASS（`/api/v0/users/current` 200）·
  `--web-chat` PASS（HTTP 200 / PoW 1 次）· `--cred-selftest` / `--export-selftest` PASS · `--vlm-selftest` 离线 PASS（exit 2）
- **「改表即换站点」端到端证据**（`AB2-13`，零改码）：把 `deepseek-web.web.login_url` 用
  `~/.brain-ai/providers.d/zz-selftest-site.json` 覆盖为 `https://chat.deepseek.com/?fromTable=1` 后运行 `--run-selftest --web`，
  `app.log` 显示 `[文本生成] 正在准备网页版会话（站点 https://chat.deepseek.com/?fromTable=1…）`、
  `[网页版登录] 登录窗口已启动（https://chat.deepseek.com/?fromTable=1…）`、`开始导航: …?fromTable=1`，
  且会话键归一为 `https://chat.deepseek.com`（**同 origin 复用同一登录态**）→ 5/5 PASS；随后删除临时表，`--provider-dump` 回到内置值
- **边界（如实登记）**：凭证仍**只存内存**；注销按站点、删除整个 profile 需二次确认（`R14`）；
  「两个**不同站点**（如 +Kimi）各自登录」需真实第二个站点条目（用户自建 + 手动各登录一次），机制与断言已就绪
- **本批未做**：`PB2-07` 界面「测试连接」按钮（离线部分早已完成）、`B2-b`（`PB2-08…PB2-12` 工厂 / Anthropic / Gemini / `DeepSeekWebProvider` 收编）

**文档（修订 v6 · 2026-09-26 · 零代码变更）：`mode` 恒两项 —— 网页版与官方 API 同等优先级**

- **触发**：用户实测「「提供商配置」的参数面板里**没有 `web` 选项**」。
- **判定**：**回归** —— 该行为属 `PB2-05`（L1）并已在上方标 ✅；`B2-b` / `B2-c` / `PB2-07` 均不含此项，**不会**在后续阶段被实现。
- **根因**：收口新增的 `engine::provider_mode_options()` 按条目 `kind` 收窄「模式」候选，与同批保留的 `ai::web_spec_for()`「非网页版条目 → **回落内置默认站点**」冲突；而「提供商配置」的 `provider` 默认值 = 表内第一项（`deepseek`，official）→ 默认工作流的 `web` **不可达**。
- **定稿（决策 `D-21`，用户指示「web 的优先级要和 api 同等」）**：`mode` 的 `official` / `web` **始终可选**；`kind` 只影响「切换提供商时的建议值」与「不一致时的提示」，**不裁剪候选、不静默改写**用户选择。
- **同时登记（转 `PB2-20`）**：面板/校验把**「提供商配置」节点**交给 `resolve_effective_provider()`（该函数只对 `LLMGenerate`/`VLMGenerate` 生效）→「面板按生效条目渲染站点」「web 条目自动锁 web」「运行前按条目提示」三处**声明 ✅ 但未生效** → ✅ **已于 v7 修复**（见下方 v7 条目）。
- **文档**：`docs/actionPlan/M_patchB.md`（`D-21`/`I13`/`PB2-20`/`B2-a3`/`AB2-15`/`VB2-18`/`R16` + §9.1）、`docs/节点编辑器使用说明.md`（§8 第 1c 项 + §9 语义）、`docs/网页版协议实测记录.md` §7.1/§7.4、`docs/README.md`

**修订落地（v7 · 2026-09-26 · 代码批次）：`mode` 恒两项 + 面板/校验按节点自身条目（`PB2-20`）**

- **改动（六项，逐条可核对；`M_patchB.md` §9.1「落地实测」有表）**：① `engine::provider_mode_options()` **恒** `{official, web}`（不按 `kind` 裁剪，守不变量 `I13`）② 删除 `resolve_effective_provider()` 里 `if (spec.kind == "web") result.mode = "web";` 的**静默改写**（抽出 `apply_spec_table()` 共用）
  ③ 新增 `resolve_self_provider()` / `resolve_display_provider()`：参数面板（`property_panel.cpp:105` / `:724`）与运行前校验（`validate.cpp:138`）改按**「提供商配置」节点自己的条目**解析站点条目 / 登录页 / 提示（此前恒回落内置默认站点）
  ④ 「web 条目自动锁 web」→ **切换提供商带出建议值**（只带一次；`provider_mode_suggestion()`）+ 不一致时**橙色提示**（`mode_kind_hint()`）⑤ `engine/node_registry.cpp` 的「模式」说明改「两项都可选（网页版与官方 API 同等优先级）」
  ⑥ `tools/api_probe.cpp` 新增 **`VB2-18` 4 项**（候选恒两项 / `ProviderConfig` 自参数解析 / 不改写 `mode` / 表外 id）
- **断言/回归**：`api_probe --exec-selftest` **204 / 0**（`201 → −1`（撤回「模式过滤」）`+4`（`VB2-18`））、`--graph-selftest` 111/0、`--selftest` 七组 PASS、`--provider-selftest` **50 / 0**
- **实测基线**：`--run-selftest` PASS（离线 3/5，预期）· `--run-selftest --web` **5/5（0 失败 0 跳过，8.76s）** · `--web-session-selftest` / `--web-probe` / `--web-chat` PASS（守 `I2`）· 构建 **0 error / 0 warning**（4 目标）
- **人工项**：`节点编辑器使用说明.md` §8 第 1c 项（GUI 点击）待用户确认；其代码路径已由 `VB2-18②③` 断言覆盖

**文档（修订 v8 · 2026-09-26 · 零代码变更）：「站点恒 DeepSeek」三轮复核 —— `D-22②` 站点不回落（待实施）**

- **触发**：用户实测「不管选哪个 AI，网页版登录窗口都是 DeepSeek」。
- **判定**：**不是 v7 未修好**，而是「非网页版条目 → 回落表内第一个 web 条目（= `deepseek-web`）」这条**规格**（`ai/provider_spec.cpp:269-283`）＋「内置表 `kind=web` 只有 1 条」＋「自建站点闭环缺失」三层叠加；并**实测发现**模板与校验器 schema 不一致（详见下条 ④）。
- **根因三层 + 残留三处**（完整证据与原始输出：`docs/actionPlan/M_patchB.md` §9.3 与**附录 D**）：① 回落规则（`web_spec_for()` 对非 web 条目返回表内第一个 web 条目；面板 / 校验 / 运行三处同规则）② 表里只有一个站点条目（Kimi 只是顶层 `_example_web_dom` 模板，`_` 前缀不加载）③ 自建站点闭环缺失（`reload_provider_specs()` **零调用点** / 「提供商」枚举一次性生成 / 配置表错误只在 `app.log`）④ 模板与校验器不一致（缺信封 → 整层忽略；扁平字段名 → `缺少 send` 跳过该条）⑤ `adapter=dom` 未实现（**可登录、不可生成**）⑥ `solve_pow_via_page()` 残留默认站点（`webview_host.cpp:1409`）。
- **定稿（用户指示 2026-09-26）**：`D-22` 选 **②**（**不回落**：明确报错 + 三条引导，运行期直接失败；新增不变量 `I14`）；`D-23` 选 **A**（本轮只改文档：手写 `providers.d` + 重启；UI 闭环 → `PB2-24` 后置）；`D-24` 本轮范围 = `PB2-22` + `PB2-23`。
- **文档**：`docs/actionPlan/M_patchB.md`（v8 复核块 / `I14` / `PB2-21`·`PB2-22`·`PB2-23`·`PB2-24` / `B2-a4` / `AB2-16` / `VB2-19` / `R17`·`R18` / `D-22`·`D-23`·`D-24` / §9.3 / **附录 D**）、`docs/节点编辑器使用说明.md`（§9 站点规则 + 自建站点 + §8 `1d`/`1e`）、`docs/网页版协议实测记录.md`（§7.1 / §7.5）、`docs/README.md`、`source/README.md`、`DevPlan.todo`。
- **待确认**：`D-25`（`mode=web` + 非网页版条目时运行前校验**是否阻断**）、`D-26`（`web.login_url` **缺失**是否也禁止回落）—— 见 `M_patchB.md` §6「审核确认清单」。
- **未做（本轮）**：**任何代码改动**（`PB2-22` / `PB2-23` / `PB2-24` 均待审核通过后实施）。

**修订落地（v9 · 2026-09-26 · 代码批次）：`PB2-22` 站点不回落 + `PB2-23` 按站点 PoW（用户拍板：**不阻断 + 收紧**）**

- **改动（9 文件）**：① `ai/provider_spec.{h,cpp}`：新增 **`strict_web_spec_for()`** / **`strict_web_provider_id_for()`**（非网页版条目 → **空**，不回落）、**`web_site_error()`**（统一「不可用原因 + 三条引导」，界面/校验/运行期**逐字一致**）、**`web_site_field_warnings()`**（`D-26`：可选字段回落 + 警告）、**`web_adapter_implemented()`**（`R12` 运行时门控）
  ② `engine/provider_resolve.cpp`：`mode_kind_hint()` 的「official + web」改为**没有网页版站点可用**文案；`unwired_reason()` 增「`mode=web` 必须有该条目自己的站点」闸门
  ③ `ui/property_panel.cpp`：站点区**错误块 + 一键改选网页版条目**、两个开窗按钮**禁用**（`site_usable`）、可选字段警告
  ④ `engine/validate.cpp`：`mode=web` 且站点不可用 → 提示改为「**本次运行必定失败** —— …」（**不阻断**，`D-25`）
  ⑤ `nodes/local_nodes.cpp`：站点不可用 → **`NodeError`**；适配器未实现 → **`NodeError`**（明示「可登录/可探测、生成待 L3」）
  ⑥ `web/webview_host.{h,cpp}`：`solve_pow_via_page(site_url, …)` **按站点**；新增 `protocol_probe_for_provider()`；探测主体抽出共用（旧入口行为逐字不变，守 `I2`）
  ⑦ `ai/deepseek_web_client.cpp`：PoW 按 `endpoints.host` 站点求解
  ⑧ `source/src/main.cpp`：`--run-selftest --web` 显式把「提供商」设为**网页版条目**（决策不回落所必需）、`--web-probe --provider <id>`
  ⑨ `tools/api_probe.cpp`：新增 **`VB2-19` 5 项**
- **断言/回归**：`api_probe --exec-selftest` **204 → 209 / 0**（`VB2-19①…⑤` 全 PASS）；`--graph-selftest` **111/0**；`--selftest` 七组 PASS；`--provider-selftest` **50 / 0**；`--run-selftest` PASS（离线 3/5，预期）；`--web-probe --provider zhipu` → **exit 2**（打印「不是网页版条目…三条引导」，**不开窗**）；`--web-probe --provider deepseek-web` 与旧 `--web-probe` 一致（守 `I2`）；构建 **0 error / 0 warning**（4 目标）
- ⚠️ **网页版生成暂不能复测 5/5（非本批引入，如实登记）**：`--run-selftest --web` 自 **17:16** 起稳定失败于 `Authorization Failed (invalid token)`（`{"code":40003}`），而 **15:53** 同一命令为 **5/5（8.76s）**；`[网页版探测] userToken` 由 `8Mgu****vZkr(len=64)` 变为 `{"va****"0"}(len=30)`（JSON 包裹值），`/api/v0/users/current` 亦 40003；**未改动的旧路径（`--web-probe` / `--web-session-selftest`）同样复现** → 判定为**官网侧会话/存储形状变化**，与本批无关。处置：用户**重新登录**后复测；诊断与判据加强登记为 **`PB2-25`** / 风险 **`R19`·`R20`**（见 `M_patchB.md` §9.4）
- **文档**：`M_patchB.md`（v9 横幅 / `PB2-22`·`PB2-23` 落地实测 / `B2-a4` / §5 `R19`·`R20` / §8 v9 行 / §9.4 / `PB2-25`）、`节点编辑器使用说明.md`（§8 `1c`/`1d`/`1e`）、`网页版协议实测记录.md` §7.5、`docs/README.md`、`source/README.md`、`DevPlan.todo`

**内置各 AI 网页版登录入口（v10 · 2026-09-26 · 代码批次 · 用户指示"为每个 AI 配正确的网页入口"）**

- **先讨论后实施**：按用户要求先讨论 → 采纳**方案 A**：① 选择器 / 凭证取值**必须实测**（各站有 WAF / 反爬），**不填假选择器**；② 需要**小改动**才允许「**登录型站点条目**」存在（原校验器会**静默跳过**缺生成字段的 `dom` 条目 —— 与"照文档抄也建不出站点"同源）；③ 本轮交付 = **登录 / 协议探测可用**，生成**明确报错**（待 L3）；④ 合规：有头登录 + 用户手动 + 不代填密码 + 不绕过验证。
- **改动（5 文件）**：① `ai/provider_spec.{h,cpp}`：`adapter=dom` 缺生成字段 **error（跳过）→ warning（可加载）**（文案含「登录型站点条目」）；新增 **`web_login_only()`**；`web_site_field_warnings()` 文案细分（缺 `token_expr`/`cookie_names` → 「该站点**无法自动探测凭证**（登录仍可用）」）；`provider_spec_selftest` 旧期望同步改写
  ② `nodes/local_nodes.cpp`：运行期报错补充「该条为**登录型条目**」
  ③ `source/src/main.cpp`：`--provider-selftest --provider <web 条目>` 对 DOM 条目如实显示「端点：**不适用**」+「生成：未就绪」（此前会把内置默认端点显示成该条目的端点）
  ④ `source/assets/providers.json`：**新增 11 个站点入口**（Kimi / 通义千问 / Qwen 国际站 / 智谱清言 / 豆包 / 腾讯元宝 / 文心一言 / 讯飞星火 / ChatGPT / Claude / Gemini）→ 共 **21 条（official 9 / web 12）**
  ⑤ `tools/api_probe.cpp`：新增 **`VB2-21` 5 项**
- **断言/回归**：`api_probe --exec-selftest` **209 → 214 / 0**（`VB2-21①…⑤` 全 PASS）；`--graph-selftest` **111/0**；`--selftest` 七组 PASS；`--provider-selftest` **50 / 0**；`--provider-dump` **21 条（official 9 / web 12）** + 11 条登录型警告；`--provider-selftest --provider kimi-web` → 打印真实登录页 + 「端点：不适用 / 生成：未就绪」；构建 **0 error / 0 warning**（4 目标）；GUI 冒烟：启动日志「条目 21 条」→ 正常退出
- **文档**：`M_patchB.md`（v10 横幅 / §3 `PB2-26` / §4.2 `AB2-18` / §4.3 `VB2-21` / §8 v10 行 / §9.5 / **附录 E** 站点清单）、`节点编辑器使用说明.md`（§9 内置清单表 + §8 `1f`）、`网页版协议实测记录.md` §7.6、`docs/README.md`、`source/README.md`、`DevPlan.todo`
- **未做**：L3 DOM 执行器与选择器实测（`PB2-13…16`）、`PB2-25`（会话失效可诊断）、`PB2-24`（自建站点 UI 闭环）、`PB2-07`

**L4 立项（v12 · 2026-09-26 · 文档先行，零代码变更）：登录层去 DeepSeek 化 + 站点数据落地 + 自助闭环**

- **背景（用户实测）**：「现在所有的 AI 都显示无法进行登录，只能切换到 DeepSeek」。复核结论：**L3 只治了「生成引擎」**（`ai/dom_web_client.cpp` 的 `dom_chat()` 对「未取到内存凭证」**仅给警告、不阻断**；`web::run_script_sync()` 三级前置**不要求内存 `userToken`**），**没治**：①**登录态判据**（`SessionStore::has_token()` = `user_token` 非空 → 通用站点恒 false；面板恒显「userToken：未获取（网页版接口需要它）」；Cookie 名回落 `ds_session_id`；状态栏只看内存槽）②**探测脚本**对非 DeepSeek 站点注入 DeepSeek 端点（`probe_kickoff_script()` 在 `probe_paths`/`token_expr` 为空时**保持内置值** → 必然 404 → 假「探测错误」）③**11 条内置站点无选择器**（如实标为登录型）+ **无界面入口**（`PB2-24`）。
- **新增层 L4（文档先行）**：任务 **`PB2-27`**（站点无关「已登录」判定与文案，纯函数 `web_session_state()`）/ **`PB2-28`**（探测「不适用」语义 + 只读诊断脚本分支 + 会话失效识别）/ **`PB2-29`**（内置站点选择器**逐站实测回填**，`verified:true` + 附录 E）/ **`PB2-30`**（`--run-selftest --web --provider <id>` + 面板「测试选择器」，并归入 `PB2-24`）；批次 **`B2-e`**；不变量 **`I15`/`I16`**；验收 **`AB2-20`…`AB2-22`**；验证 **`VB2-24`…`VB2-26`**；风险 **`R21`…`R23`**；待确认 **`D-27`/`D-28`/`D-29`**。
- **本批变更范围（只改文档、零代码）**：`M_patchB.md`（v12 横幅 / §0.3 / §2.2 / §2.3 / §3 L4 / §4.1–4.3 / §5 / §6 / 附录 E 两列 / §8 v12 / §9.7）、`CHANGELOG.md`（本条目）、`节点编辑器使用说明.md`（§9 新增「为什么『所有 AI 都无法登录』」+ §8 `1h`）、`网页版协议实测记录.md` §7.8、`docs/README.md`、`source/README.md`、`DevPlan.todo`。
- **未做 / 下一步**：`B2-e` 的**代码与数据**（先 `PB2-27`/`PB2-28` 去 DeepSeek 语义 → 再 `PB2-29` **逐站**实测选择器：**每站都需要在界面手动登录一次**）以及既有 `PB2-25`（会话失效可诊断）/ `PB2-07`（界面「测试连接」）。


**L3 落地（v11 · 2026-09-26 · 代码批次）：通用 DOM 站点适配器 + 选择器探测（`PB2-13`…`PB2-16` / 批次 `B2-c`）**

- **新增 `ai/dom_web_client.{h,cpp}`（站点 = 纯数据，程序里无站点专有常量）**：
  · `dom_chat()`：按站点确保窗口 → 注入提示词（`contenteditable` 用 `insertText`；`input`/`textarea` 用原型 setter + `input`/`change`，兼容 React/Vue 受控组件）→ 触发发送（`send.kind=key`（默认 Enter）/ `click` 点选择器）→ 轮询 `answer_selector`（**最后一个**匹配节点的 `innerText`），`done_when=selector_gone|selector_present` 命中即完成；未配置时按「文本连续 3 轮不变」；`answer_poll_ms ∈ [200,2000]`、`answer_max_polls ∈ [10,600]`（钳制）；**到上限/总超时 → 已取文本照样返回 + Console 明确警告**（R13：不假装成功、不无限等待）
  · `dom_adapter_selftest()`（PB2-15）：只读探测当前页面 `URL`/`标题`、各选择器命中数与可见性、`done_when` 现状、`token_expr` 取值形状（长度 / **JSON 包裹值** / 求值失败）、Cookie 可读性 + **可操作修复建议**
  · 纯函数（离线可断言）：`clamp_poll_params` / `dom_cfg_json`（**转义安全**）/ `dom_kickoff_script` / `dom_poll_script` / `dom_probe_script`
- **`web/webview_host.{h,cpp}`**：新增 `run_script_sync()`（窗口内**同步**执行脚本，结果原样 JSON 回传）+ `run_script_now()` + `wait_page_ready()`；三级前置（窗口已在该站点 → `ensure_session` → **兜底离屏开窗 + 等 `readyState=complete`**）→ **不把内存 `userToken` 当通用条件**（DOM 站点登录态在浏览器 profile 里）
- **接线**：`nodes/local_nodes.cpp` 增 DOM 分支（`adapter=="dom"` → `ai::dom_chat()`；登录型条目 → 明确报错 + 指向诊断命令）；`provider_spec.cpp` 的 `implemented_protocols()` / `implemented_web_adapters()` 增 **`dom`**；`main.cpp` 增 **`--web-adapter-selftest [--provider <id>]`**（无 `--provider` → 列出 web 条目 + exit 2）；`CMakeLists.txt` 增源文件
- **断言/回归**：`api_probe --exec-selftest` **214 → 219 / 0**（`VB2-22①…⑤` 全 PASS；`VB2-19⑤` / `VB2-21③` 按「dom 已实现」同步改写）；`--graph-selftest` **111/0**；`--selftest` 七组 PASS；`--provider-selftest` **50 / 0**；`--provider-dump` **21 条（official 9 / web 12）** + 「已实现协议：openai、deepseek-web、**dom**」+「已实现网页版适配器：builtin:deepseek、**dom**」；`--run-selftest` PASS（离线 3/5，预期）；构建 **0 error / 0 warning**（4 目标）
- **端到端实测（窗口 + 页面脚本）**：`--web-adapter-selftest --provider kimi-web --timeout 20` → 探测**真实执行**：`URL = https://www.kimi.com/`、`标题 = Kimi AI with K3 | Built for Agentic Coding & Knowledge Work`、`input/send/answer 命中 0`（该条尚未填选择器）+ **4 条可操作建议**、exit **1** → 由此**发现 `kimi.moonshot.cn` 301 到 `www.kimi.com`**（Kimi 条目 `login_url` 已修正）
- **文档**：`M_patchB.md`（v11 横幅 / §3 `PB2-13…16` 落地实测 / §4.1 `B2-c` / §4.2 `AB2-19` / §4.3 `VB2-22`·`VB2-23` / §8 v11 行 / §9.6）、`节点编辑器使用说明.md`（§9 选择器怎么填/怎么调 + §8 `1g`）、`网页版协议实测记录.md` §7.7、`docs/README.md`、`source/README.md`、`DevPlan.todo`
- **未做 / 下一步**：**逐站点选择器实测**（用 `--web-adapter-selftest` 取选择器 → 填进条目/用户表 → `verified:true` → 回填附录 E）；`PB2-25`（会话失效可诊断）、`PB2-24`（自建站点 UI 闭环）、`PB2-07`（界面「测试连接」按钮）

**文档（先行，2026-09-26）：网页版站点身份与多站点会话 —— 缺口复核（本批次无代码变更）**

- **背景（用户实测）**：切换「提供商」并选网页版后，点「打开登录窗口」**始终打开 DeepSeek**；且无法让两个网页版提供商各自保存 Cookie、各用各的登录态。
- **复核结论（只读审计，逐条附证据）**：
  - **站点硬编码 5 处**：`web/webview_host.h:68-75`（`interactive_login_request()` 固定 URL + 标题）、`ui/property_panel.cpp:105`（`draw_web_session_section()` **无参**，调用点 `:644` 不传节点）、`ui/property_panel.cpp:155`/`:183-186`/`:119`/`:648`（探测按钮 URL、状态徽标 `ds_session_id`、文案）、`web/webview_host.cpp:1107-1110`（`ensure_session()` 用 `LoginRequest` 默认值）
  - **`mode` 下拉未按条目 `kind` 过滤**：`engine/node_registry.cpp:319` 无条件给 `{official, web}` → 「official 条目 + `mode=web`」静默落回内置默认端点（`ai/provider_spec.h:53-58` = DeepSeek 端点）
  - **会话不能并存（4 处单槽/单例）**：`web/session_store.h:64-82`（只存一个 `Session`，`set()` 覆盖）、`web/webview_host.h:129` + `webview_host.cpp:936-943`（登录窗口进程内单例）、`nodes/local_nodes.cpp:415-422`（节点取「那一个」会话）、`ui/property_panel.cpp:88-102`（注销 = `remove_all(~/.brain-ai/webview2)`，**清掉所有站点**）
  - **数据层事实**：`source/assets/providers.json` 共 10 条，`kind=web` **仅 `deepseek-web` 1 条**（Kimi 仅顶层 `_example_web_dom` 模板，`_` 前缀键不参与加载）；本机 `~/.brain-ai/providers.d/` 为空、无 `~/.brain-ai/providers.json`
  - **断言绑死硬编码**：`tools/api_probe.cpp:314-316` 断言 `interactive.url.find("deepseek.com")`（修复时必须改为表驱动双向断言）
  - **表字段未接线**：`web.cookie_names` / `web.token_expr` 已被解析与白名单校验（`ai/provider_spec.cpp:542-546`、`:158`/`:231`），但**全库无消费点**
  - **已具备条件（好消息）**：`ai::web_chat(session, request)` 已按参数收会话；WebView2 profile 的 Cookie/localStorage 天然按 origin 隔离（单 profile 可同时保存多站点）；`TextMerge`（变长 `texts`）与 `PromptTemplate`（变长 `vars`）已支持多路文本汇聚，`graph.cpp:498-500` 变长端口不会被替换
- **本批次交付（仅文档）**：`docs/actionPlan/M_patchB.md` 新增任务 **`PB2-17`（登录入口去硬编码）/ `PB2-18`（多站点会话按站点键控）/ `PB2-19`（窗口串行 + 按站点注销）**，新增不变量 **`I11`/`I12`**、验收 **`AB2-13`/`AB2-14`**、验证 **`VB2-16`/`VB2-17`**、决策 **`D-19`/`D-20`**、风险 **`R14`/`R15`**、批次 **`B2-a2`**、§9「B2-b 前置复核」14 条发现；同步 `docs/网页版协议实测记录.md`（新增 §7 边界与实测计划）、`docs/节点编辑器使用说明.md`（§9 已知限制 + §10.2/§10.4 现状标注）、`docs/README.md`、`source/README.md`、`docs/DevPlan.todo`
- **基线不受影响（声明）**：本批次**零 `.cpp/.h` 变更** → `--graph-selftest 111/0`、`--exec-selftest 190/0`、`--provider-selftest 50/0`、构建 0 error / 0 warning **均未触碰**；代码改动归 **`B2-a2`**（`PB2-17…PB2-19`，预估 1.5–2 天）

**修复（构建）：CMake 4.4 配置输出被 vcpkg 工具链弃用警告刷屏 —— 已彻底清零**

- **现象**：CMake 4.4.3 + `C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake` 时，每次配置都会打印多条带调用栈的
  `CMake Warning (deprecated) at .../vcpkg.cmake:40 (cmake_policy): Compatibility with CMake < 3.10 will be removed from a future version of CMake.`
  —— 观感像 CMake 报错（Configure 实际仍能完成，产物正常）
- **根因**：vcpkg 自带工具链内部 `cmake_policy(VERSION 3.7.2)`（`vcpkg.cmake:40` 与 `:878`）在 CMake 3.31+/4.x 触发**策略版本弃用警告**；工具链在项目文件**之前**执行，因此项目里的 `cmake_minimum_required(VERSION 3.25...4.6)` 与 `CMAKE_POLICY_VERSION_MINIMUM=3.10` 都管不到它
- **修复**（`source/CMakePresets.json`）：
  - 增 `"CMAKE_WARN_DEPRECATED": "OFF"`（preset 缓存变量在工具链之前生效 → 警告消失）
  - 工具链改用 preset 的 **`"toolchainFile"`** 字段（原先写成 cache 变量 `CMAKE_TOOLCHAIN_FILE`，会被 CMake 反过来判为 `unused-cli` 警告）
  - `CMAKE_SUPPRESS_DEVELOPER_WARNINGS`（-Wno-dev）**特意不写进 preset**：它由 CMake 自身消费，作为手工变量会被 `unused-cli` 警告一次（已实测）
- **兜底**（`source/CMakeLists.txt`）：不用 preset 的 `cmake -S source -B build` 场景同样设 `CMAKE_WARN_DEPRECATED=OFF`；补注释说明「谁先执行、为什么只能在 preset 层解决」以及 `unused-cli` 的坑
- **文档**：`source/README.md` §6 常见问题第 3 条改写为完整修复说明
- **实测**：全新配置 `cmake --preset default -B <新目录>` 与增量配置 `cmake --preset default` **警告/错误均 0 条**；全量编译 **0 error / 0 warning**（5 个目标）；`--graph-selftest` **111/0**、`--exec-selftest` **190/0** 回归通过

**特性（M_patchB L1 第二批）：请求参数化 + 节点/UI/执行链路真正按表走 + 网页版站点参数化**

- **PB2-04 请求参数化**（`ai/deepseek_official_provider.{h,cpp}`）：新增 `ProviderOptions{api_base, chat_path, auth_style, auth_header, extra_headers, env_names, connect/read_timeout_s}` 与纯函数 `build_endpoint(base, path)` / `build_auth_headers(options, key)` / `resolve_api_key(param, env_names)` / `resolve_chat_path(path, model)`（`{model}` 占位，Gemini 风格）/ `provider_options_from(spec)`；`official_chat` 的端点·认证·超时**全部按 options**（默认值 = 改造前行为，旧签名保留重载）
- **PB2-05 表驱动（节点/解析/执行/UI）**
  - `engine/provider_resolve.{h,cpp}`：`EffectiveProvider` 增 `specs/spec/kind/display/api_base/key_ref/key_required/spec_origin`（快照保活，reload 后不悬垂）；空字段用表默认补齐；**web 条目自动锁 `mode=web`**；`unwired_reason` 全部按表生成（表里没有该 id / 网页版不支持视觉 / 缺 Key 文案含**表内 env 名与引用名**；`auth_style=none` 不再要求 Key）
  - `engine/node_registry.cpp`：**「提供商」下拉来自配置表**（official 在前、web 在后）+ 默认项取表内首项；`api_base` / `api_key_ref` 默认改为空（= 用表默认，换厂商自动带出正确的 ref）
  - `nodes/local_nodes.cpp`：`execute_llm_generate` / `execute_vlm_generate` 用生效条目构造请求（选项 / 地址 / 模型 / 限额 / env 名）；新增 Console 行「提供商=… / 模式=… / 地址=… / 模型=…（来源 builtin|user）」；`execute_provider_config` 的句柄输出**表解析后的生效值**（含 `kind`/`display`/`key_ref`/`site_login`）；VLM 的视觉门控改为**能力表**判据（表内 `vision=false` → 明确报错并给出建议模型；表外模型放行 + 提示），文案保留「暂不支持网页版」以免破坏既有断言/习惯
  - `nodes/local_nodes.cpp`：`resolve_official_key` 的 env 名与引用名默认值改为**按表**（节点参数 → 表内 env 列表按序 → 凭据库 ref）
- **PB2-05 网页版去硬编码**（`web/webview_host.{h,cpp}`、`ai/deepseek_web_client.{h,cpp}`）
  - `LoginRequest` 增 `probe_paths` / `challenge_path` / `completion_path`；新增纯函数 **`probe_kickoff_script()`**（把站点路径注入探测 JS 模板；**不传参数时渲染结果与改造前逐字一致**），探测脚本改为按 `g_request` 渲染
  - `WebChatRequest` 增 `endpoints`（默认 = 改造前写死的 DeepSeek 常量）；`web_chat` 的 host / completion / challenge / 会话创建·拉取路径**全部取自已传入的端点**；`base_headers` 的 Origin/Referer 也按站点 host 生成
- 断言：`api_probe --exec-selftest` **170 → 190 通过 / 0 失败**（新增：端点 5 例 / 认证 5 例 / env 3 例 / 协议默认值 4 例 / **探测脚本渲染 3 例**——含「无站点参数 = 内置默认」与「自定义站点后不残留内置路径」）
- **实测（改表即改行为，零代码）**：临时写入 `~/.brain-ai/providers.json` 覆盖 `deepseek-web.web.probe_paths=["/api/v0/users/current"]` → `--provider-selftest --provider deepseek-web` 立即显示「来源=user」「探测路径：/api/v0/users/current」（表为 2 层）；验证后已删除临时文件
- **基线（本批实测）**：`--graph-selftest` **111/0** · `--exec-selftest` **190/0** · `--selftest` 七组 PASS · `--run-selftest` PASS（离线 3/5）· `--cred-selftest` PASS · `--export-selftest` PASS · `--vlm-selftest` 离线 PASS（exit 2，默认值不变）· `--provider-selftest` **50/0**（exit 0）· `--provider-selftest --provider deepseek-web` exit 2（未登录，端点取自表）· 构建 **0 error / 0 warning**
- ⚠️ **未能复跑的基线（环境问题，非代码）**：`--login-selftest` / `--web-probe` / `--web-chat` / `--run-selftest --web` 在本机被**残留 WebView2 进程锁住用户数据目录**（`~/.brain-ai/webview2`；8 个 `msedgewebview2.exe` 无法终止）后，新进程在 WebView2 初始化/退出阶段挂起或 `0xC0000005`；同批次 `webview2_login.exe --selftest` **PASS（Cookie 5 条）**，且崩溃前的日志显示网页版流程本身正常（Cookie 5 条 / userToken 64 位 / `/api/v0/users/current` 200）。**待办**：关闭其它 AIwrite 实例或重启后重跑这四条基线并回填结果
- **本批未做**：`PB2-06`（`config.toml` 多 provider 节 + 旧配置迁移）、`B2-b`（`PB2-08…PB2-12` 工厂 / Anthropic / Gemini / `DeepSeekWebProvider` 收编）

**特性（M_patchB L1 第一批）：Provider 配置表真的被程序读取了 —— 加载 / 合并 / 校验 / 用户覆盖 / 自检**

- 新增 `ai/provider_spec.{h,cpp}`（PB2-01）：JSON 配置表加载器（纯函数 `merge_provider_specs` + 读盘 `load_provider_specs`）
  - **四层来源（低 → 高）**：`<exe>/assets/providers.json` → `$AIWRITE_SOURCE_DIR/assets/providers.json`（开发态兜底）→ `~/.brain-ai/providers.d/*.json`（文件名升序）→ `~/.brain-ai/providers.json`（字段级覆盖，支持 `replace_all`）；四层都缺失 → **最小兜底表**（custom-official + deepseek）
  - **校验（official 与 web 同一套）**：`schema_version` / 必填（id·display·kind·protocol）/ 类型不符 → 跳过该条 / 未知字段 → 警告 / **`_` 前缀键 → 忽略且不警告** / **明文密钥键名与值双重检测 → 警告 + 拒绝该字段** / 坏 JSON 只废该层 / **`web.adapter` 分支校验**（`builtin:*` 缺 `endpoints` → 沿用内置默认 + 警告；`dom` 缺选择器 → 报错并丢弃）/ `kind=web` 缺 `login_url` → 报错 / 「表里有、程序还没实现」→ 警告（R9）
  - 每条记录 `origin`（`builtin` / `source` / `user.d/<文件名>` / `user` / `fallback`）；提供线程安全快照 `provider_specs_snapshot()`（工作线程用）
- `utils/paths.{h,cpp}`（PB2-02）：新增 `providers_asset_file()` / `user_providers_dir()` / `user_providers_file()`；`ensure_data_dirs()` 顺带创建 `~/.brain-ai/providers.d`
- `CMakeLists.txt`（PB2-02）：新增 `aiwrite_copy_assets(<target>)`，把 `assets/providers.json` 拷到 `$<TARGET_FILE_DIR>/assets/`（`aiwrite` 与 `api_probe` 均生效）；实测产物 `build/bin/assets/providers.json`
- 新增 CLI（PB2-07 **离线部分**）：`--provider-selftest [--provider <id>] [--api-base <地址>] [--model <名>] [--key-ref <引用名>]`、`--provider-dump`
  - 表校验 + 离线断言 → `[配置表自检] 50 项通过 / 0 项失败`；`--provider <id>`：API 条目**按表解析**「生效地址 / 生效模型 / 引用名」→ 按 `env_names`（按序）→ 凭据库取 Key → 有 Key 发一条 ping（`请只回复：pong`）；**web 条目只检查登录态与端点一致性，不发送任何内容**；退出码 0=通过 / 1=失败 / 2=缺 Key（API）或未登录（web）
  - 实测：`--provider-selftest` → 50/0 PASS（exit 0）；`--provider-selftest --provider zhipu` → 自动带出 `https://open.bigmodel.cn/api/paas/v4` + `glm-4-flash` + `brain-ai/zhipu`（exit 2：无 Key）；`--provider-dump` → 打印 10 条（official 9 / web 1）+ 来源 + 已实现协议清单
  - `--vlm-selftest` 行为不变（默认值改在分发处补齐，仍是智谱 `glm-4v-flash` / `brain-ai/zhipu`；实测离线 PASS / exit 2）
- 断言：`api_probe --exec-selftest` **120 → 170 通过 / 0 失败**（新增配置表 50 项：合并优先级 / 字段级覆盖 / 新增条目 / 必填缺失 / 类型错误 / 未知字段 / 明文密钥 / 坏 JSON / schema_version / 兜底 / replace_all / web 两形态 / 未实现协议警告 / 解析辅助 / 视觉门控）
- `source/assets/providers.json`：补 `endpoints.users_path`（消除「缺 users_path」警告）
- **基线（全绿，2026-09-26 实测）**：`--graph-selftest` **111/0** · `--exec-selftest` **170/0** · `--selftest` 七组 PASS · `--run-selftest` PASS（离线 3/5）· `--run-selftest --web` **5/5 / 0 失败 / 0 跳过（9.01s）** · `--cred-selftest` PASS · `--export-selftest` PASS · `--web-session-selftest` PASS（userToken 64 位）· `--web-probe` PASS（`/api/v0/users/current` 200）· `--web-chat` PASS（HTTP 200 / PoW 1 次）· 构建 **0 error / 0 warning**
- **本批未做（下一批 L1 续）**：`PB2-04` 请求参数化（端点 / 认证 / 超时全部按表取值）、`PB2-05` 节点 / UI / 校验表驱动 + **网页版去硬编码**（`webview_host` 登录 URL·窗口标题·探测路径；`deepseek_web_client` host 与端点；`property_panel` 登录入口）、`PB2-06` `config.toml` 多 provider —— **表已在跑，但尚未接管执行链路**（节点仍走现有实现，故本轮行为零变化、基线不变）

**文档 + 数据（M_patchB 计划）：Provider 从「写死在 C++」改为「JSON 配置表 + 用户可覆盖」，且 **API 与网页版同表同机制****

- 新增 `docs/actionPlan/M_patchB.md`（**v3**）：现状审计（7 个硬编码点 / `provider` 字段零分派 / 全库无任何厂商元数据文件）+ 三层方案（**L1 JSON 配置表（official + web 两类同表）** · L2 协议/适配器工厂 · L3 通用 DOM 站点适配器）+ `PB2-01…PB2-16` 任务 + `AB2-01…AB2-12` 验收 + `VB2-01…VB2-15` 验证项 + 决策 `D-08…D-18` + 附录 B（**JSON 字段规范，含 web 两种形态**）与附录 C（**加载顺序与生效规则**）
- **web 与 API 同机制（v3 新增，硬性要求）**：`kind: "official"` 与 `kind: "web"` 共用同一张表、同一套合并/覆盖/校验、同一个 UI 管理区与同一个自检命令；**网页版去硬编码清单**（`PB2-05`）——`web/webview_host.h:22,62`（登录 URL/窗口标题）、`ui/property_panel.cpp:184`（登录入口）、`web/webview_host.cpp:150-163`（探测 JS 路径）、`ai/deepseek_web_client.cpp:15-17,122-152`（host / completion / challenge / 会话创建·拉取路径）全部改由表的 `web.login_url` / `web.window_title` / `web.endpoints` / `web.probe_paths` 驱动，**站点换域名或改版时用户改 JSON 即可**（PoW 与 SSE 解析仍属内置实现）
- 已定稿决策：`D-11`（厂商/站点元数据 → JSON 表；实例参数 → `config.toml`）· `D-12`（完整内置集）· `D-15`（三层覆盖规则）· **web 与 API 同机制**
- **新增配置表数据文件 `source/assets/providers.json`**：内置 10 条（`deepseek` / `deepseek-web` / `zhipu` / `siliconflow` / `ollama` / `openrouter` / `openai` / `anthropic` / `gemini` / `custom-official`），逐条带 `capabilities` / `models` / `limits` / `notes` / `docs_url`，`verified` 如实标注是否本机实测（当前 `deepseek` / `deepseek-web` / `zhipu` 为 true）；**不含任何明文密钥**（只有环境变量名与凭据引用名）
- **`deepseek-web` 条目已按 v3 规范补齐**：`web.adapter = builtin:deepseek` + `window_title` + `endpoints{host,completion_path,challenge_path,session_create_path,session_fetch_path}` + `probe_paths` + `models`（default/expert/deepseek-reasoner）；另加顶层 **`_example_web_dom`**（DOM 站点模板，`_` 前缀键为纯文档字段，加载器忽略不警告）与 `_keys_note`
- 用户自定义（零代码，**API 与站点都适用**）：`~/.brain-ai/providers.json`（字段级覆盖）+ `~/.brain-ai/providers.d/*.json`（新增条目，单文件可分享）
- **状态**：计划草案待审核；**尚未接线** —— 加载器 `ai/provider_spec.*`、打包拷贝、表驱动 UI（含网页版会话区/配置表管理区）与 `--provider-selftest` 属 `B2-a`；接口/工厂/`DeepSeekWebProvider` 收编/Anthropic/Gemini 属 `B2-b`；通用 DOM 适配器与 `--web-adapter-selftest` 属 `B2-c`（见文档 §4.1）
- 索引互链：`CHANGELOG` 索引、`docs/README.md`（进度行 + 文档地图）、`M_patchA` §4.1 `PB-04` 行、`milestone_plan.md`（补丁系列两处）；文档断链自检 **94 / 0**

**特性（M5-C）：读图生成 + 看得见图片 —— 代码落地（4 提交）**

> 决策：`actionPlan/M_patchA.md` §12 **D-06**（优先 M5 核心，Patch B 剩余项 PB-02/04/07/09 延后）· **D-07**（视觉模型走**智谱**，实现按 **OpenAI 兼容**规范）
> 基线：`--graph-selftest` 111/0 · `--exec-selftest` **72 → 120** · `--selftest` 七组全 PASS · `--run-selftest` 3/5（离线预期）· `--cred-selftest` 8/8 · `--export-selftest` 7/7 · 构建 0 error / 0 warning

- **AI 层（`779857d`）**：`ai/deepseek_official_provider.{h,cpp}` 支持**多模态消息体** ——
  `OfficialChatRequest.images`（本地路径，按序）非空时 `messages[].content` 变为数组
  （`[{type:"text",…}, {type:"image_url", image_url:{url:"data:<mime>;base64,…"}}]`；智谱 GLM / 硅基流动 / 本地 Ollama 通用）；
  新增纯函数 `image_mime_from_path` / `encode_image_data_url`（OpenSSL `EVP_EncodeBlock`，默认 8 MB 上限，超限报可操作错误）；
  **无图路径逐字节保持旧请求体**（回归安全）；离线断言 **+20**（`--exec-selftest` 72 → 92）
- **引擎层（`62139bf`）**：`VLMGenerate`（图片理解）接线 —— 图片（单张/多张）→ data URL → 生成文本，错误分类复用官方 API；
  网页版给出「暂不支持」可操作错误；`ProviderConfig` 新增 **`model_custom`（模型（自定义））** 覆盖枚举模型名
  （`engine::resolve_effective_provider` 与执行同规则，界面显示生效模型）；Key 解析抽为 `resolve_official_key`（复用 PB-06 三级优先级 + 首次自动入库）；断言 **+16**（→ 108）
- **UI 层（`6359aa9`）**：新增 `ui/texture_cache.{h,cpp}`（stb_image + **GL 1.1**，缓存键 = 路径 + mtime + 大小，LRU 8 张，退出前 `release_textures()`）；
  输出面板 / 参数面板渲染缩略图 + 「打开所在文件夹」（`ShellExecuteW`）；新增 `RunNodeView.images` 快照通道（ImageInput 输出 / ImagePreview 汇点虚拟端口）；
  Image Input 参数面板显示「图片尺寸」；断言 **+8**（→ 116）
- **示例与实测入口（`c5dddfe`）**：`source/workflows/examples/E-02_图片转小说.json`（5 节点 4 连线，智谱端点 + `glm-4v-flash` 预置）+
  `source/assets/images/sample.png`（640×360）；新增 `aiwrite.exe --vlm-selftest [--image <路径>] [--api-base <url>] [--model <名>] [--key-ref <引用>]`
  （① 离线请求体断言 ② 有 Key 时真实读图；退出码 0=通过 / 1=失败 / 2=缺少 Key）；断言 **+4**（→ 120）；
  新增编译宏 `AIWRITE_SOURCE_DIR`，自检可稳定定位仓库内示例
- **实测**：`--vlm-selftest` **离线部分 PASS**（示例图 → data URL 4826 字符，请求体 = text + image_url）；**联网读图待 API Key**
  （`build\bin\aiwrite.exe --vlm-selftest`，或界面加载 E-02 后运行）
- **未做（留后）**：M5-09 输出面板历史（=`PD-07`）、M5-10 图片复制/导出、归档按日期分文件夹、示例 E-03·E-05·E-06、网页版图片入口
- **文档**：`M5.md` 实施状态刷新 + 进度节；`milestone_plan.md` 节奏表 M5 行；`M_patchA.md` §0.4 基线 + §11.4 v4 + §1.7；`DevPlan.todo` 的 `FEA-M5-01/02` 置 done

**文档（actionPlan 全量状态同步）：M_patchA / M3 / M4 / M5 / M6 / milestone_plan 按实况更新**

- `M_patchA.md`：
  - 新增 **§1.0 处理状态总览**（A–G 七类逐类标注「已修 / 部分 / 未做」+ 对应编号）
  - §2 补丁总览新增**状态列**：A ✅ 全部完成 · B 🟡 主体完成 · C 🟡 部分 · D 🟡 部分 · 并行 🟡 部分
  - §4.1 `PB-01…PB-09`、§4.2 `VB-01…VB-09`、§5.1 `PC-01…PC-04`、§6.1 `PD-01…PD-08`、§7 `PM-01…PM-04` **全部加状态标记**；§5.1 / §6.1 增状态说明；§8 增「状态以实施进度表为准」注
  - §11.2 自检速查：`--exec-selftest` 更正为 **72/0**，并补 `--cred-selftest` 8/8、`--export-selftest` 7/7、`--web-session-selftest`
  - §11.4 变更记录补 **v2**（Patch A 完成 + F1/F2/F3）、**v3**（本次状态校正）
  - §12 增**决议状态**注（D-01…D-05 结论）
- **编号校正**：设置面板在 `M6.md` 中为 **M6-01**（M6-06 是「工作流校验完善」）—— 修正 `M_patchA` §6.1 / §10 / §11.3 与 `milestone_plan.md` 的表述
- `M3.md`：顶部增**实施状态行**（M3-01…M3-10 全部 ✅；唯一待办 `FEA-M3-07` 工作流变体保存）；M3-08 由 🟡 改为 ✅（连线右键菜单按需求移除，删除改走「左键选中 → 工具栏删除选中」）
- `M4.md`：新增**实施状态表**（M4-01…M4-13 逐条状态与承接方：官方 Provider ✅ PB-05、凭据改 DPAPI 文件库 ✅ PB-06、流式 UI 侧 ✅ / 数据源 🚫、会话失效引导 ⬜ PB-07）+ 验收对照（A-01…A-07）
- `M5.md` / `M6.md`：新增**实施状态表** —— 提前落地项（`PromptTemplate` / `TextMerge` / 输出归档 / 文本导出）与归口项（`PD-03` 设置面板、`PM-01` WebView2 引导）逐条标注
- `milestone_plan.md`：总体节奏表 M2–M6 与「补丁系列」行状态刷新（含当时/现基线数字 `95/0、56/0 → 111/0、72/0`）
- 验证：文档断链自检 `checked 84 / broken 0`；**未改动任何代码**

**文档（状态校正）：`M_patchA` 实施进度表按实况更新（PB-01 三步完成 / PB-05·PB-06 已完成 / 剩余项收敛）**

- 背景：表中「**PB-01 执行线程化（第一、二步：引擎核心 + 状态层）｜🟡 部分**」等行已过期 —— PB-01 **三步**（`4d9b9ef` 引擎核心 / `6255d93` 状态层 / `f0b34fc` GUI 收口）早已落地；PB-05 官方 Provider、PB-06 凭据库亦在后续批次完成
- 校正（`docs/actionPlan/M_patchA.md`）：
  - **PB-01** → ✅ 三步完成（含实跑断言：`PB-01 线程化 OK`（状态事件 9 / 输出事件 5 / Graph 未被工作线程写=是 / 快照一致=是）+ `PB-01 第二步 OK（快照·Graph == 权威）`）
  - **PB-05** → ✅ 已完成（**API Key 由「提供商配置」节点读取**，`c38b127` 修正「取错节点」；用户已实测真实生成）
  - **PB-06 拆为两行**：凭据库 ✅（DPAPI + `BRNC` 容器 + 三级优先级 + CLI + `--cred-selftest` **8/8**）／剩余「代理与自签证书策略 + `[credentials]` 配置项」⬜（`FEA-M4-13`）
  - **PB-03** → ✅ 已落地（最小化）/ 🚫 数据源暂停（httplib v0.15.3 无 POST 响应流式重载，见 [`网页版协议实测记录.md`](网页版协议实测记录.md) 附录 A）；**PB-08** → 🟡 UI 侧已落地
  - **剩余项收敛为一行**：PB-02 真取消 · PB-07 会话失效引导 · PB-09 自检扩展 · 流式数据源 · FEA-M4-13；另两项待办（离线「假增量」断言、PB-04 Provider 统一抽象）
  - **§0.4 回归基线表**同步：`--exec-selftest` 56 → **72**；新增 `--cred-selftest` 8/8、`--export-selftest` 7/7、`--web-session-selftest`；表头日期 → 2026-09-26；「PA-02 后」那行标注为**历史快照**
- 验证：文档断链自检 `checked 81 / broken 0`；**未改动任何代码**

**修复：网页版运行报「未取得网页版凭证」—— 登录窗口已开时不再干等，改为自动补探测**

- 现象：点「▶ 运行」或「重新生成（新 seed）」→ 节点失败 `文本生成（网页版）不可用：未取得网页版凭证…`，且要等约 25 秒才报错
- 根因：网页版凭证（`userToken`）**只存内存**，每次启动进程都要重新探测一次；而 `web::ensure_session()` 旧逻辑只判断「窗口句柄是否为空」——
  **窗口已打开就只“等”**，从不发起探测。而用户手动点「打开登录窗口」时用的 `probe_after_load=false`（不探测）→ 内存里永远没有 `userToken` → 白等 25 秒超时。
  **与 Cookie 过期无关**：实测 `--web-probe` 仍能取到 `userToken`（`/api/v0/users/current` → 200），profile 登录态有效 ⇒ 纯粹是引导逻辑 bug
- 修复（`web/webview_host.{h,cpp}`）：新增纯逻辑决策 `plan_session_boot(has_token, window_open)`（`HaveToken` / **`ReuseAndProbe`（窗口已开 → 补一次探测）** /
  `StartAndProbe`），`ensure_session()` 按它执行；等待期间**每 6 秒补探测一次（最多 3 次）**，覆盖「登录动作发生在页面加载之后 / 浏览器尚未就绪」；
  失败文案改为可操作指引（点「探测网页版协议（dev）」或跑 `--web-probe`）
- 修复（`ui/property_panel.cpp`）：手动「打开登录窗口（WebView2）」改用 `web::interactive_login_request()` —— 窗口页面加载完成后**自动探测一次**（按钮悬停有说明），
  之后点「运行 / 重新生成」无需再等
- 断言（离线）：`api_probe --selftest` V-09 新增「会话自动引导决策」真值表 4 项 + 「手动登录窗口自动探测」→ 实测 `OK`
- 新增回归（端到端，能直接复现本 bug）：`aiwrite.exe --web-session-selftest` —— 先按用户手动路径开窗口（刻意不探测，断言此刻 `userToken` 为空）→ 再调 `ensure_session()` 必须自己补探测取到凭证；
  实测 `步骤 1：窗口已开（Cookie 5 条，userToken 空）` / `步骤 2：ensure_session=OK（userToken 长度 64）` / `PASS`，退出码 0
- 回归：构建 0 error / 0 warning；`api_probe --selftest` exit 0；`--run-selftest` / `--cred-selftest` / `--export-selftest` exit 0；`--run-selftest --web` 完成 5/5 PASS（真实网页版生成不受影响）

**修复（崩溃）：参数滑块范围越界导致选中「文本生成」节点即 abort（ImGui SliderInt 断言）**

- 现象：`Assertion failed: *(const ImS32*)p_min >= IM_S32_MIN / 2 && *(const ImS32*)p_max <= IM_S32_MAX / 2`（imgui_widgets.cpp `SliderBehavior`）→ 选中节点后参数面板渲染即崩
- 根因：上批给 `seed` 写的范围 `0..2147483647` 超出 ImGui Slider 的 half-range 上限（`IM_S32_MAX/2 = 1073741823`）
- 面板层根治：`ui/property_panel.cpp` 检测范围越界时**自动降级为 `InputInt`/`InputFloat` 并把输入裁回参数范围**（Float 另加 `isfinite` 校验），杜绝同类崩溃
- 数据层：`seed` 范围改为 `0..1000000000`；「重新生成（新 seed）」派生值同步收窄到 `[0, 1e9]`
- 断言（防回归）：`--run-selftest` 新增「参数范围安全（ImGui Slider 上限）」——遍历注册表所有带范围参数校验落在 `±1073741823`（Int）/ `±1.7e38`（Float）内，且 `min<=max`

**M_rerun：文本生成「重新生成（新 seed）」—— 重跑等效“再次运行”，但换随机种子**

- 文档先行：`actionPlan/M_rerun.md`（计划）+ `M_patchA §6.1 PD-06` 扩写（区分「通用重跑 FEA-M4-11」与「新 seed 重跑 FEA-M4-17」）
- `engine/node_registry.cpp`：LLMGenerate 增 `seed` 参数（Int，默认 0 = 不指定；顺带修正 `mode` 的过期描述）
- `ai/deepseek_official_provider.{h,cpp}`：`OfficialChatRequest.seed`；`build_request_body` 在 `seed > 0` 时加入 `seed`
- `nodes/local_nodes.cpp`：official 传 seed；网页版在 `seed > 0` 时 Console 提示「网页版不支持 seed，已忽略」
- `ui/property_panel.cpp`：LLMGenerate 节点新增 **「重新生成（新 seed）」** 按钮（运行中禁用；先压快照→写新 seed→异步运行，**可撤销**）
- 断言（离线）：请求体含/不含 seed、节点含 seed 且默认 0 → `--run-selftest` 实测 `M_rerun 新 seed：OK`
- 回归：构建 0 error / 0 warning；6 套自检 exit 0；GUI 冒烟优雅退出

**M_textio P5（修复 + 路径可配置）：汇点节点结果入运行态 + TextOutput 导出参数/自动导出**

- **修复真 bug**：`nodeOutputText` 只遍历输出端口，而 `TextOutput`/`ImagePreview` 是**无输出端口的汇点** → 运行结果永远读不到（表现为：连接线“像没接上”、输出面板显示“无输出”、归档缺该节点文件、导出无正文）→ `Executor` 对无输出端口节点把返回值写入运行态值**虚拟端口 `__result`**；`nodeOutputText`/`makeSnapshot` 回退读取
- `TextOutput` 新增参数：**`export_dir`** / **`file_name`** / **`auto_export`**；导出对话框默认目录与默认名取自参数（自动导出不覆盖同名，自动 `-1/-2`）
- 断言：`--run-selftest` 新增「M_textio 汇点结果可读」（汇点文本 == 上游文本 且 快照可读）

**M_textio P2+P3：文本输出 = 最终输出（预览）+ 导出为文档**

- `utils/text_export.{h,cpp}`（新）：`sanitize_file_stem` / `render_document_name` /
  `build_document_body`（元信息头可选）/ `write_document`（**原子写**：临时文件+rename；同名自动 `-1/-2`）/
  `export_text_document` / `document_stamp_now` / `document_time_text`
- `--export-selftest`：**7 条离线断言 7/7 PASS**（文件名渲染+清洗 / 正文含与不含元信息头 / 写盘回读一致 /
  同名不覆盖自动改名 / 不可写路径明确错误 / 无 .tmp 残留 / label 进入标题）
- UI：输出面板 **最终输出段置顶 + 高亮**（`【最终输出】<标签>`）、该段与参数面板各加 **「导出为文档…」**、
  工具栏加「导出最终输出为文档…」；画布节点摘要前缀 `最终输出：`；**`label` 参数从此真正生效**
- 回归：构建 0/0；`api_probe` 三套 exit 0；`--run-selftest` / `--cred-selftest` / `--export-selftest` 均 exit 0；GUI 冒烟优雅退出
- 文档：`actionPlan/M_textio.md`（计划）、`ai_writer_nodes.md`（N-08 语义）、使用说明 §10.8（人工确认 6 项）；DevPlan `FEA-M4-14/15` ✅

**PB-05（落地「API 使用」）：官方 DeepSeek API Provider —— `mode=official` 真正可用**

- 新增 `ai/deepseek_official_provider.{h,cpp}`：`POST {api_base}/chat/completions`（OpenAI 兼容）
  - 纯函数（离线可断言）：`build_endpoint()` / `build_request_body()`（含 `model/stream/temperature/max_tokens/top_p`
    与 `messages[system?, user]`）/ `resolve_api_key()`（节点参数 → 环境变量 `DEEPSEEK_API_KEY`）
  - `official_chat()`：15s 连接超时 / 180s 读超时；**错误分类**（400 参数 / 401 Key 无效 / 402 余额不足 / 429 限流 /
    5xx 服务端 / 网络·超时·DNS），错误信息带可操作提示；解析 `choices[0].message.content`
- `nodes/local_nodes.cpp`：`mode=official` 分支**不再抛“尚未接线”**，改为真正调用官方 API；`api_base` 由
  provider 输入覆盖（节点参数为准）；缺 Key 时给出明确提示（凭据管理器见 PB-06）
- `engine/provider_resolve.cpp`：`unwired_reason()` 改为「**缺 API Key** 才必定失败」——配置了 Key（参数或环境变量）
  即不再出现运行前告警；`validateBeforeRun` 的 warning 与工具栏运行前弹窗同步更新
- 回归：构建 0 error / 0 warning；`api_probe` 七组 PASS / 111-0 / 73-0+72-0；`--run-selftest` PASS（离线为
  缺 Key 预期失败）；`--run-selftest --web` 5/5
- 人工验证（需 API Key）：① 在「提供商配置」填 API Key（或设 `DEEPSEEK_API_KEY`）→ 模式选 official → 运行 →
  应真实生成；② 故意填错 Key → 应提示 401 且建议检查 Key


**PB-03（最小化）+ PB-08：流式呈现 —— 网页版增量回调 → 逐字显示「生成中…（N 字）」**

- `ai/deepseek_web_client.{h,cpp}`：`WebChatRequest` 增 `on_delta`；SSE 解析循环**每解析到一段
  `delta_text_of` 就回调一次**（原先只累加、结束时才返回全文，故做不到逐字）
- `engine/executor.{h,cpp}`：`ExecutionContext` 增 `on_delta` + `delta()` 助手；`RunEvent::Kind` 增 `Delta`；
  构造时接线 `handleDelta()`（**异步模式才推事件**，同步/自检路径行为不变）
- `nodes/local_nodes.cpp`：网页版请求携带 `ctx.on_delta`（官方 Provider 的 `on_delta` 随 PB-05 延后）
- `ui/editor_state.cpp`：`pump_run_events()` 处理 `Delta`（读模型 `text` 逐段追加、状态置 Running、
  `delta_bytes` 同步）；`run_status_text()` 运行中显示 `当前 nX（生成中… N 字）`；
  **`abort_run_if_any()` 先 `stop_run_async()` 再 `cancel/reset`**（运行中切图/改图不再与工作线程竞争）
- `ui/node_canvas.cpp`：`Running` 状态显示 `生成中…（N 字）`（无增量回退「运行中…」）；缓存签名含文本长度
  → 逐字增长自然刷新
- `ui/output_panel.cpp`：`RunNodeView` 标题行运行中追加 `· 生成中（N 字）`；正文随增量逐段增长
- 回归：构建 **0 error / 0 warning**；`api_probe --selftest` 七组 PASS；`--graph-selftest` 111/0；
  `--exec-selftest` 73/0 + 72/0；`--run-selftest` PASS（PB-01 OK）；`--run-selftest --web` PASS 5/5（8.81s）
- 看板：DevPlan Archive/M3 追加 `ARC-M3-10`（PB-01 三步）/ `ARC-M3-11`（PB-03-min + PB-08）
- **待办**：① 离线"假增量"断言（把 VB-03 自动化：临时注册会 `ctx.delta()` 的执行器，断言增量拼接 ==
  最终文本 + `delta_bytes` 一致）；② **PB-07 会话失效引导**（40002/401 → 错误条「重新登录」→ WebView2 登录窗）

**PB-01（第一步）：执行线程化核心 —— 事件队列 + Graph 副本 + 只读快照（Patch B 开工）**

- `engine/executor.{h,cpp}`：
  - 新增 `RunEvent{Kind: NodeState/Console/NodeOutput/Finished, node_id, text, error, state, final_state, summary}`
  - 新增 **`startAsync()/pumpEvents()/requestStop()/stopAsync()/asyncMode()/eventsPushed()`**：工作线程在
    **`worker_graph_` 副本**上推进 `tick`（1ms 节拍），只把状态 / Console 文本 / 节点输出全文 / 结束统计
    推入 `mutex + condition_variable` 事件队列；`startAsync` 会拦截既有 console/state 回调改为事件，
    因此**工作线程不触碰主线程的 Graph、ImGui 与日志**
  - 新增 `RunSnapshot` + `makeSnapshot()`（UI 侧只读读模型；与 `runInfos()/outputs()` 同源）
  - `nodeOutputText()` 提升为**引擎侧唯一实现**，`ui::node_output_text` 委托到它（多线程只有一份规则）
  - 既有同步路径 `start()/tick()/runToCompletion()` **完全保留** → `api_probe`、`--run-selftest` 零改动
- `src/main.cpp`：`--run-selftest` 新增 **PB-01 断言**（离线）——异步会话跑通；事件完整性
  （状态 9 / 输出 5 / Console 11 / **Finished 1**）；**Graph 未被工作线程写**（主线程未应用事件时节点仍 Idle）；
  快照与权威数据一致（节点数/状态/类型/耗时）；节点输出文本一致
  - 断言过程中发现并固定了 API 契约：**必须先 `stopAsync()`（join）再 `pumpEvents()`**——否则会话结束帧
    `state_` 已翻转、`Finished` 事件可能尚未入队（首轮实测 `Finished 0` → 修正后通过）
- 回归：构建 **0 error / 0 warning**；`api_probe --selftest` 七组 PASS；`--graph-selftest` 111/0；
  `--exec-selftest` 73/0 + 72/0；`--run-selftest` PASS（3/5 + P1-c + **PB-01 OK**）；
  `--run-selftest --web` PASS（5/5）
- **第二步（状态层闭环）**：`ui/editor_state.{h,cpp}` 新增 `start_run_async()` / `pump_run_events()` /
  `stop_run_async()` / `session_active()`；`tick_run()` 异步分支只泵事件（**主线程写 Graph**）；收尾集中到
  `finish_run_session()`（**先 join → `makeSnapshot` 落定 → 状态栏/日志 → 归档 → 错误条**）；
  `cancel_run()` 改用 `requestStop()`；读模型定型为 **`RunNodeView`**（含 `text` / `delta_bytes`，为 PB-03/08 铺路），
  `makeSnapshot(graph, executor)` 用 `nodeOutputText` 落定全文
- `--run-selftest` 新增 **PB-01 第二步断言**：`OK（启动=是 / 会话结束=是 / 快照·Graph == 权威=是 / 节点 5 个 / 状态 finished）`
- 回归：`api_probe --selftest` 七组 PASS；`--graph-selftest` 111/0；`--exec-selftest` 73/0 + 72/0；
  `--run-selftest` PASS（3/5 + P1-c + PB-01 两步 OK）；`--run-selftest --web` PASS 5/5
- **待完成（PB-01 第三步，下一步做）**：GUI 三处运行入口切 `start_run_async()`、UI 面板改读 `run_snapshot`
  （输出面板 / 节点摘要 / 参数面板）、`abort_run_if_any` 与退出前 `stop_run_async()`；随后 PB-03-min →
  PB-08 流式呈现 → PB-07 会话失效引导

**Patch A2 收尾（PA-05 / PA-06 / PA-08 / PA-09）：Console 复制导出 · 错误条 · 配置治理 · 文档索引**

- **PA-05 Console 增强**（`ui/console_panel.{h,cpp}`）：工具条新增 `复制可见` / `复制全部` /
  `导出可见到文件…`（UTF-8，行数与可见行一致）；新增 `console_visible_text()`——
  与面板渲染**共用同一个 `log::filter`**，保证「复制的内容 == 屏幕可见」；**手动上滚自动暂停**
  （显示「自动滚动已暂停」+「回到底部」按钮恢复）；窗口初始高度接 `ui.console_height`
- **PA-06 轻量错误条**：`EditorState` 增 `last_error{node_id, message, at}` + `refresh_last_error()`
  （运行结束记录**首个失败节点**，成功运行自动清空）；新增 `ui/error_bar.{h,cpp}`：
  状态栏红条（单行省略 96 字符 + 悬停全文/时间 + `×` 关闭），**点击选中失败节点并让视图跟随**；
  连续失败只覆盖显示、不堆积
- **PA-08 未接配置字段治理**：`ui.console_height`（Console 初始高度）、`ui.running_animation`
  （运行中节点状态点脉冲光环）**接线生效**；新增 `unwired_config_fields()` 清单，
  启动日志分别输出**已接线**与**尚未生效**（含归口补丁）两行；
  `ui.grid_size` 如实登记为「vendored imgui-node-editor 的 Style 无网格间距字段」→ 归口 PD-03；
  `--selftest` 新增 **PA-08 断言**（清单含 `general.language` / `timeout.*` / `output.auto_open_on_complete`，
  且不含已接线的 `console_height` / `running_animation` / `window_*`）
- **PA-09 文档与索引同步**：`CHANGELOG`（本条）、`M_patchA.md`（实施进度 + §3.2 四小节 + §3.3 的
  VA-04/05/06 状态）、`节点编辑器使用说明.md` **§10.7**（Console / 错误条 / 配置未接线清单人工确认）、
  `milestone_plan.md` 补「补丁系列」行；`M4.md` 归口说明（M4-05→PB-04/05、M4-12→PA-05+PD-07、
  M4-13→PA-04）此前已就位
- 回归：构建 **0 error / 0 warning**；`api_probe --selftest` **七组 PASS**（含 PA-08 清单断言 PASS）；
  `--graph-selftest` 111/0；`--exec-selftest` 73/0 + 72/0（零失败）；`--run-selftest` PASS（3/5 + P1-c OK）；
  `--run-selftest --web` PASS（5/5 + P1-c OK）
- 附：Patch A **A1 + A2 全部完成**；下一步进入 Patch B（PB-01…09）

**F3（FEA-M3-04 / PD-04）：窗口几何持久化 —— 尺寸/位置/最大化写入 config.toml + 越屏矫正**

- `utils/config.{h,cpp}`：`[ui]` 新增 `window_width` / `window_height` / `window_pos_x` /
  `window_pos_y` / `window_maximized`（默认：尺寸 0 = 用内置默认；位置 -1 = 未记录）
- `utils/window_geometry.{h,cpp}`（新，纯函数、不依赖 GLFW）：
  - `fit_window_to_workarea(desired, workarea)`：先夹尺寸（≥ 最小 640x480、≤ 工作区），
    再平移使**整窗落在工作区内**（尽量保留原位置）；工作区无效（无显示器）时原样返回；
    返回 `changed` 便于调用方回写配置
  - `has_position()`：判断是否记录过位置（`-1` = 未记录）
- `ui/app.cpp`：
  - 配置加载**提前到窗口创建之前**（几何来自 `config.toml [ui]`）：记录值优先 → 越屏矫正 →
    `glfwCreateWindow` / `glfwSetWindowPos` / `glfwMaximizeWindow`；矫正结果落回内存配置并写日志
  - **节流保存**：每帧检测移动/缩放/最大化，变化后**静默 2 秒**才落盘一次（避免拖拽期间频繁写文件）；
    最大化时不覆盖"还原尺寸/位置"，下次启动先恢复几何再最大化
  - **退出兜底保存**：清理前再采样一次几何并写 `config.toml`（+ `set_app_config` 同步进程缓存）
- `tools/api_probe.cpp`：`--selftest` 的「配置往返」组新增窗口几何字段往返断言 +
  **6 项越屏矫正断言**（区内不变 / 越屏拉回 x=640 / 左上越界归零 / 3000x2000 → 1920x1040 /
  100x80 → 640x480 最小保护 / 无工作区原样 + `has_position`）
- 回归：构建 **0 error / 0 warning**；`api_probe --selftest` **七组 PASS**（含新增窗口几何 PASS）；
  `--graph-selftest` 111/0；`--exec-selftest` 72/0；`--run-selftest` PASS（3/5 + P1-c OK）；
  `--run-selftest --web` PASS（5/5 + P1-c OK）
- 文档/看板：M_patchA 实施进度（F3 ✅）；DevPlan 勾选 `FEA-M3-04`、Archive/M3 追加 `ARC-M3-08`
  （M3 剩余仅 `FEA-M3-07 工作流变体保存`，按你要求**延后**）

**F2（FEA-M3-03 / PD-05）：参数面板增强 —— 搜索过滤 + 批量应用同类型参数 + 二次确认**

- `engine/graph.{h,cpp}`（新能力）：`Graph::copyParamsToSameType(node_id, error)` ——
  按参数 id 把某节点的参数值批量应用到图中全部**同类型**节点：
  `is_secret`（如 API Key）**不复制**（避免密钥误扩散）；不改动标题 / 位置 / 连线；
  返回被更新的节点数，节点不存在时返回 0 并写 error。纯模型层，可被界面与自检共用
- `ui/property_panel.cpp`：
  - **参数搜索框**（`id` / 显示名 / 说明，ASCII 大小写不敏感）+ 「清除」按钮 + 命中统计
    `搜索「…」：命中 N 项，已过滤 M 项（共 K 项）`
  - **「把当前参数应用到全部同类节点」**：显示同类型其他节点数；点击后**二次确认**
    （「确认把 nX 的参数应用到 N 个同类节点？（密钥类参数不复制）」[确认应用]/[取消]）；
    执行前 `snapshot("批量应用参数到同类节点")` → 一次撤销即可回滚整批
  - **「重置为默认值」二次确认**（避免误触清空编辑），执行前仍先压快照（P1-c 已加）
  - 只读标记：`is_secret` 参数显示「（密钥：仅存内存、不写盘）」；与默认值不同的参数显示「已改」
- `tools/api_probe.cpp`：`--graph-selftest` 新增 **10 项**「参数批量应用」断言 —— 同类命中数、
  数值/文本/枚举同步、异类节点不受影响、标题与位置不被改动、**is_secret 不复制**、
  同类非密钥参数正常复制、未知节点返回 0 并写 error
- 回归：构建 **0 error / 0 warning**；`api_probe --selftest` 七组 PASS；
  **`--graph-selftest` 111 通过 / 0 失败（原 95）**；`--exec-selftest` 72/0；
  `--run-selftest` PASS（3/5 + P1-c OK）；`--run-selftest --web` PASS（5/5 + P1-c OK）
- 文档/看板：M_patchA 实施进度（F2 ✅）+ 当前基线更新为 111/0；DevPlan 勾选 `FEA-M3-03`、
  Archive/M3 追加 `ARC-M3-07`；新增（**延后**）`FEA-M3-07 工作流变体保存`（用户后续设计）

**P1-c：参数编辑即时写回（修复"改了输入框，运行结果还是旧值"）+ 重置可撤销 + 参数驱动断言**

> 排查结论：**不存在硬编码**。全仓搜索「从前有座山」只有 `ui/editor_state.cpp:146`（默认示例工作流里
> 「文本输入」的初值）；`TextInput` 执行器就是 `params.value("text")`（`nodes/local_nodes.cpp:59`）；
> 归档 `n1-TextInput.txt` 的内容 == 该参数值 == 示例初值。
> 真实原因：参数面板所有控件都写成 `Widget(...) && IsItemDeactivatedAfterEdit()`，而"值发生变化"与
> "控件失焦"常常**不在同一帧**（多行文本尤其明显）→ 打字后失焦/点运行**没有提交到模型** ✗
> （日志佐证：19:04 之后没有任何 `参数变更: n1.text`，运行仍发出 67 字节 = 示例默认提示词）。
> 代码里 Enum 分支的注释恰好记录过同类坑（Combo 必须"值变化即写回"），本轮把该结论推广到所有控件。

- `ui/property_panel.cpp`：
  - 新增 `write_param()`，**所有参数类型统一改为"值变化即写回模型"**（字符串 / 多行文本 / 整数 / 浮点 /
    布尔 / 文件·目录（含「浏览…」）/ 颜色；Enum 原本已是即时写回）；`edited` 仍只在**编辑结束**时通知
    外层（`result.changed` + 日志 `参数变更: nX.param` 每段编辑只记一次，不刷屏）
  - 节点标题输入同样改为"变化即通知"（`title_touched || IsItemDeactivatedAfterEdit()`）
  - **「重置为默认值」先压快照**（`editor().snapshot("重置参数为默认值")`）→ 误触可撤销（D-M3-5 前半；
    二次确认留待 F2）
- `src/main.cpp`：`--run-selftest` 新增 **P1-c 参数驱动（"无硬编码"）断言**：改「文本输入」的文本 →
  重新运行 → 断言该节点输出 == 新文本（且与上次不同）且下游「提示词模板」输出**包含**新文本
  （即真正送给 AI 的提示词来自输入框）
- 回归：构建 **0 error / 0 warning**；`api_probe --selftest` 七组 PASS、`--graph-selftest` **95/0**、
  `--exec-selftest` **72/0**；`--run-selftest` **PASS**（3/5 + `P1-c 参数驱动：OK`）；
  `--run-selftest --web` **PASS**（5/5 + `P1-c 参数驱动：OK`）
- DevPlan：新增并勾选 `DBG-M3-02 参数编辑写回不可靠`，Archive/M3 追加 `ARC-M3-06`

**P1-a / P1-b（M3 前插）：生效提供商可见性 + 运行前提示 —— 消除"改提示词/改模式都没反应"**

> 背景（真实用户困惑）：默认示例工作流把「提供商配置」（默认 `mode=official`）连到「文本生成」的
> provider 输入，而 `execute_llm_generate` 无条件用 provider 句柄的 mode **覆盖**节点自身参数
> （节点自身默认 `web`），于是 official（官方 API，PB-04/PB-05 未接线）分支必定抛错、下游跳过 →
> 全程没有一次 AI 调用；而输出面板里唯一有内容的「文本输入」节点输出被误当成"硬编码结果"。
> 网页版（`provider.mode = web`）**已真实接线**：`--run-selftest --web` 真联网生成 PASS。

- `engine/provider_resolve.{h,cpp}`（新）：provider 生效规则的**单点实现**（与 `nodes/local_nodes.cpp`
  的取值顺序逐字一致）——`resolve_effective_provider()`（provider 输入连线优先，覆盖节点自身参数）、
  `official_not_wired()`、`unwired_reason()`（official / M5-02 未接线）；纯模型层、不依赖 ImGui
- `ui/property_panel.cpp`：推理节点顶部显示 `生效：official（来自 提供商配置 n5）· 模型 deepseek-chat`
  （未连 provider 时显示"节点自身设置"）+ 红字 `官方 API 尚未接线（PB-04/PB-05）：本次运行该节点必定失败，
  下游会被跳过。` + 按钮 **「把 提供商配置 n5 改为 web」**（先压快照 → 改 mode，可撤销）
- `ui/node_canvas.cpp`：节点体显示 `生效 official ← n5` 与红字「官方 API 尚未接线…：必定失败」
- `engine/validate.cpp`：运行前校验**追加** warning（**不升级为 error**——自检与 api_probe 断言要求
  「不阻断」，见「未配置 Key：运行前校验仍通过」）：`[n3] 官方 API 尚未接线（PB-04/PB-05）（生效模式
  official 来自 n5）：本次运行该节点必定失败，下游会被跳过；网页版已接线，需先登录一次`
- `ui/toolbar.cpp`：**运行前预检 + 确认弹窗**（拦截只在这一层）——点「▶ 运行」时若存在未接线分支，
  弹窗列出清单 `n3 文本生成 ← 提供商配置 n5（官方 API 尚未接线（PB-04/PB-05））`，三个按钮：
  **「切换为网页版并运行」**（批量把来源节点 mode 改 web，一次快照可撤销，随后运行）/
  **「仍要运行」**（可复现当前失败行为）/ **「取消」**；无未接线分支时行为与之前完全一致
- 明确**不改动**：默认示例工作流（`editor_state.cpp` 的输入初值与 `ProviderConfig` 默认 official）、
  任何既有断言（本次未新增/未修改测试断言——按用户要求）
- 回归（期望值全部未变）：构建 **0 error / 0 warning**；`api_probe --selftest` 七组 PASS、
  `--graph-selftest` **95/0**、`--exec-selftest` **72/0**；`--run-selftest` **3/5 PASS**（official 分支
  预期失败，错误文案不变）；`--run-selftest --web` **5/5 PASS**（真实联网 6.60s，输出 1090 字符）
- DevPlan：新增并勾选 `FEA-M3-06 生效提供商可见性 + 运行前提示`，Archive/M3 追加 `ARC-M3-05`

**M3 / F4（PA-03）：结果呈现 —— 节点内摘要 + 参数面板「运行结果」区（三处同源）**

- `ui/text_view.{h,cpp}`（新）：只读多行文本 + 复制助手抽出为共享组件（`draw_readonly_text` /
  `copy_text`，缓冲按「控件 id + 内容」缓存）→ 输出面板与参数面板**共用同一实现**
- `ui/node_canvas.cpp`：节点体内追加**运行结果摘要**（PA-03）——完成显示 `结果： <首 160 字符>…（N 字符）`
  （绿）、失败显示错误首行（红）、跳过显示「已跳过（上游失败）」（黄）、运行中显示「运行中…」；
  用「运行信息条数 + 状态 + 错误长度 + 输出字节数」做**签名缓存**，避免每帧重算长文本
- `ui/property_panel.cpp`：新增「**运行结果**」折叠区（默认展开）——状态/耗时、
  失败错误（红）、只读全文（可选中）、「复制运行结果」按钮、最近归档路径 + 「复制归档路径」；
  未运行时提示「尚未运行」
- `ui/output_panel.cpp`：改用共享 `text_view`（删除本地重复实现）
- `src/main.cpp`：`--run-selftest` 新增 **VA-01 三处一致断言**（输出面板全文必须包含每个节点全文）
- 实测：`--run-selftest` → `VA-01 三处一致：OK（比对 3 个有输出的节点，全文 613 字符）`、PASS；
  `--run-selftest --web` → `VA-01：OK（4 节点 / 2397 字符）`、5/5 PASS；
  `--selftest` 七组 PASS；`--graph-selftest` 95/0；`--exec-selftest` 72/0；构建 0 error / 0 warning
- DevPlan：`FEA-M3-05` ✅、`TST-M3-02` 描述更新（仅剩人工确认），Archive/M3 追加 `ARC-M3-03/04`

**M3 / F1（M3-04）：复制/粘贴重做 —— 断言先行 + 设计 §6.1 入口接回**

- `src/main.cpp`：`--run-selftest` 新增 **7 项模型级断言**（先断言后接线）——
  新增节点数 / 连线期望与实际（含"选区内部连线被一并粘贴"路径）/ 位置偏移 ±40 且有限 /
  参数按值复制 / 选区与视图跟随标志 / 可撤销（撤销后节点与连线数复原）/ 空剪贴板粘贴被拒绝
  - 断言期间修掉测试自身的两处问题：`paste_clipboard()` 的 `snapshot()` 会让 `graph.nodes` 重新分配
    → 源节点改为**按值捕获**（原先持有 `Node*` 读到悬空内存导致误报）；边数在**撤销前**采样
- `src/ui/node_canvas.cpp`：画布右键菜单新增「**粘贴**」（剪贴板为空时禁用并提示）、
  节点右键菜单新增「**复制**」；移除两处"暂停"占位项
- `src/ui/app.cpp`：「编辑」菜单新增「**复制 / 粘贴**」（按可用性禁用）
- `src/ui/toolbar.cpp`：更新注释——复制/粘贴入口按设计放在右键与编辑菜单（工具栏不重复放置）
- 设计对照：§6.1「右键节点 → 复制；右键画布 → 粘贴」；§6.7「MVP 不保留快捷键」→ 快捷键任务**决定不做**
- 实测：`--run-selftest` → `F1 复制/粘贴：OK（新增 2 节点 / 连线 期望+1 实际+1 新增节点间 1 / 位置=OK /
  参数=OK / 选择与跟随=OK / 可撤销=OK / 空剪贴板=OK）`、**PASS**；`--run-selftest --web` 5/5 PASS；
  `--selftest` 七组 PASS；`--graph-selftest` 95/0；`--exec-selftest` 72/0；构建 0 error / 0 warning
- DevPlan 看板：`vcpkg-cache/update_devplan.py`（新增维护脚本，幂等 + 校验 expandKeys）→
  勾选 `FEA-M3-01`、`DBG-M3-01`（并校正描述）、`FEA-M3-02` 标记"决定不做"，Archive/M3 追加归档记录

**文档：`docs/DevPlan.todo` 改为 TodoList（saber2pr.todolist）原生格式**

- 根键 `todotree`（`tree` / `expandKeys`(全数字,33) / `add_mode:"bottom"` / `title` / `desc` / `lang` / `version` / `timelines`）
- 节点 = `{key:number, children:[], todo:{id, content, done, level, fileLink}}`；`key == todo.id` 且**按树序编号**（1…105）
- 层级：**分类（5）→ 里程碑（M1–M6）→ 任务（72）**；分类着色 `Debug=danger / Feature=default / Test=warning / Docs=secondary / Archive=success`
- **Archive 全类 `done:true`**（done-as-archive 约定，不删除已完成项）；每条任务一行描述 + `fileLink` 指向文档（本地路径；章节指针附在描述尾部 ` ｜ §…`）
- 文件级 `desc` = 当前回归基线 + 主要风险清单（结构化摘要）
- 转换脚本：`vcpkg-cache/gen_devplan.py`（含 Pre-push Checklist 校验：key 为数字、key==id、id 唯一、level 合法、expandKeys 与"有子节点的 key"一致）
- 校验：JSON 解析通过；105 节点 / 72 任务；`expandKeys` 33 个全为数字

**M_patchA / Patch C 提前落地（PC-05/PC-06）：运行结果自动归档到 outputs + 网页版 system_prompt 透传**

- `utils/output_archive.{h,cpp}`（新）：`archive_run()` 写出 `outputs/<yyyyMMdd-HHmmss>-<工作流名>/`
  - 每节点一份 `<node_id>-<Type>.txt`（元信息头：节点/类型/状态/耗时/错误/时间/统计 + 正文）；无输出的失败节点只进 run.json
  - `run.json`：归档时间、工作流名、统计、逐节点明细（状态/耗时/错误/文件名/字节数）
  - 同秒多次运行自动加序号；**本次归档恒排最前且永不删除**
  - 保留策略：`keep_history=false` → 只留最近 1 份；`true` → 保留 `max_history` 份；`ttl_days>0` → 清理超期目录
    （只清理符合命名规则的目录，先统计后删除并写日志）；工作流名非法字符自动清洗
  - 目录不可写等失败只返回 `error`，不抛异常、不影响运行流程
- `ui/editor_state.{h,cpp}`：运行结束自动归档（节点条目与输出面板同源 `node_output_text` → **归档内容与面板一致**）；
  新增 `last_archive_dir` 与 `workflow_display_name()`
- `ui/output_panel.cpp`：显示「已归档：<路径>」+「复制归档路径」按钮（悬停显示完整路径）
- `nodes/local_nodes.cpp`：**网页版 system_prompt 透传**（前置拼进 prompt，网页版无 system 角色槽位）；
  当 `temperature/max_tokens/top_p` 非默认时提示"网页版忽略、仅官方 API 生效"
- `utils/config.h`：`[output]` 四个字段标注为**已接线**（`auto_open_on_complete` 仍留 M5）
- 自检：`--exec-selftest` **72 通过 / 0 失败**（+10 项归档断言：run.json、节点 .txt、无输出不写 txt、
  名字清洗、元信息头与正文、run.json 明细、保留份数、不可写路径、ttl 清理）；
  `--run-selftest` 增加 **PC-05 归档断言**（临时目录，不污染用户 outputs）
- 实测：`--run-selftest --web` → 5/5、**PC-05 归档 OK**（`n1-TextInput.txt`/`n2-PromptTemplate.txt`/
  `n3-LLMGenerate.txt`（1063 B，即生成文档）/`n5-ProviderConfig.txt`/`run.json`）、VA-07 OK、PASS；
  离线模式 PC-05 OK、PASS；`--selftest` 七组 PASS；`--graph-selftest` 95/0；构建 0 error / 0 warning

**M_patchA / A2（PA-02）：输出面板 —— 运行结果可见 / 可复制 / 可导出**

- `ui/output_panel.{h,cpp}`（新）：可停靠输出面板（默认与 Console 同区）
  - 数据源**只读**：`Executor::runInfos()`（状态/耗时/错误）+ `Executor::outputs()`（运行态值）
  - 逐节点分段：标题 `[n1] TextInput · done · 0 ms`（按状态着色）、正文只读多行（可选中复制）、
    失败节点红色「错误：…」、跳过节点 `skipped` 无正文
  - 工具栏：**复制全文** / **复制该节点** / **导出到文件…**（复用 `utils::save_file()`，UTF-8）
  - 长文在面板内最多显示 4 万字符（缓存缓冲按内容变化重建）；复制/导出始终为完整内容
  - `node_output_text()` / `run_output_text()` 与自检**共用同一实现**（保证"看到的 = 导出的"）
- `ui/app.cpp`：`kWindowOutput` 窗口 + 默认停靠 Console 同区；「视图 → 输出」开关
  **首次接线 `config.ui.show_output_window`**（切换即 `save_config` + 同步 `app_config()`）；启动日志含「输出」可见性
- `src/main.cpp`：`--run-selftest` 增加面板数据通道验证 + **VA-07 快照纯净断言**
  （保存后的工作流 JSON 不得包含 LLM 运行结果）
- 实测：`--run-selftest --web` → 输出面板文本 **501 字符**、`VA-07 = OK`、**PASS**（5.51s）；
  离线模式 613 字符（含错误/跳过段）、`VA-07 = OK`、PASS；
  回归：`--selftest` 七组 PASS / `--graph-selftest` 95-0 / `--exec-selftest` 62-0；构建 0 error / 0 warning
- 文档：`M_patchA.md`（PA-02 ✅、VA-01/VA-02 部分、VA-07 ✅ + 实施进度）；`节点编辑器使用说明.md` 增 §10.5 人工确认清单

**M_patchA / A1：运行信息只读暴露 + 逐节点耗时入 workflow.log + 配置访问器（地基补丁第一步）**

- `engine/executor.{h,cpp}`（PA-01）：新增 `NodeRunInfo{node_id,type,state,duration_ms,error,delta_bytes}` 与只读 `Executor::runInfos()`；
  完成/失败/跳过三条路径均记录（跳过记 `0ms` + 上游错误），`reset()` 清空；**不进 Graph / 撤销快照 / 工作流 JSON**（运行态值原则）
- `engine/executor.cpp`（PA-04 前半）：节点开始计时，控制台/日志行带耗时 —— `[n3] 完成（7167 ms）`、`[n3] 失败（0.11 ms）：…`、`[跳过] n4（上游 n3 失败）`
- `ui/editor_state.cpp`（PA-04 后半）：运行结束向 `workflow.log` 追加**机器可读明细** ——
  `[运行明细] n1(TextInput)=done/0ms n3(LLMGenerate)=error/0ms/err:… n4(TextOutput)=skipped/0ms/err:上游 n3 执行失败，已跳过`
- `utils/config.{h,cpp}`（PA-07）：新增进程级只读配置缓存 `app_config() / set_app_config()`（`std::mutex` 保护）；
  `ui/app.cpp` 启动与保存 `config.toml` 后同步；`tools/api_probe.cpp` 启动载入真实配置（为 Patch B 的 provider 超时/重试铺路）
- 自检：`api_probe --selftest` 配置往返组新增 PA-07 断言（set→get 一致 + 默认值）；`--exec-selftest` 新增 6 项 PA-01 断言
- 实测（基线全绿）：`--selftest` 七组 PASS；`--graph-selftest` 95/0；`--exec-selftest` **62/0**（原 56）；
  `--run-selftest` PASS（3/5 预期）；`--run-selftest --web` **5/5（7.17s）**，`workflow.log` 明细可读；构建 0 error / 0 warning

**文档：建立「地基补丁系列」行动计划（M_patchA）**

- `docs/actionPlan/M_patchA.md`（新）：基于 2026-09-23 **全库底层逻辑审计**（A 结果回流 / B 执行层 / C 配置层 / D 会话凭据 / E 交互 / F 数据 / G 占位），
  把跨里程碑的地基缺口收敛为补丁系列 **A→B→C→D + 并行小项**，并给出 Patch A 的完整详述
  （PA-01…PA-09 任务、VA-01…VA-08 验证、风险与产出）、横向追溯矩阵（缺口→任务→验证→验收）与决策点 D-01…D-05
- 关键审计结论：`Executor::outputs()` 在 UI 侧**零引用**（结果无法回显/复制）、执行**同步阻塞且不可中断**、
  `ExecutionContext` 无流式回调、`config` 的多数字段（`output/timeout/error/general/advanced` 等）**未被读取**、
  无自动保存/版本迁移/输出归档、无快捷键、复制粘贴暂停、窗口几何不持久
- 与既有里程碑的归口：M4-05/M4-07/M4-10/M4-11 → **Patch B**；M4-12/M4-13 → **Patch A**；
  M6-02 → **PM-01**；M6-06 → **PD-03**；M5 §11 输出归档 → **Patch C** 打底
- 索引同步：`docs/CHANGELOG.md` 相关文档索引新增本文件；`docs/actionPlan/milestone_plan.md` 新增「补丁系列」行
  并修正 M2/M3/M4/M5/M6 过期状态（M2 已完成、M3/M4 进行中）

**网页版生成端到端打通（M4-06 / M4-09 方案 A 收口）：LLMGenerate 已能真实生成文本**

- PoW 方案 A：`web::solve_pow_via_page()` —— 在**隐藏登录窗口内调用官方 PoW worker** 求解
  （自动定位 worker chunk：读 `main.*.js` → `new Worker(n.u(<id>))` → chunk 表取 hash → 取源码包 Blob Worker，
  故前端发版自适应；实测返回 answer 后服务端接受）
- `web::ensure_session()`：内存会话无凭证时，自动离屏起登录窗口并等一次协议探测（Cookie + userToken），
  使「第一次点运行」也能直接工作（无需手动开登录窗口）
- `ai/deepseek_web_client.cpp`：SSE 解析修正为「增量（`{"p":"…/content","o":"APPEND","v":…}` 与紧凑 `{"v":…}`）
  + 完整快照兜底取更全者」，修复正文重复/截断
- `nodes/local_nodes.cpp`：`execute_llm_generate` 接线 —— `mode=web` 走网页版（Cookie+userToken → PoW → SSE），
  `mode=official` 保持 M4-05 待接线提示；`mode`/`model` 参数加入 LLMGenerate（默认 `web` + `deepseek-chat`）
- `src/CMakeLists.txt`：`web/webview_host.cpp` 移入 `aiwrite_core`（`ai/` 依赖 `web/`；api_probe 亦可链接）
- `src/main.cpp`：新增 `--run-selftest --web`（示例工作流走真实网页版生成，等价于界面「▶ 运行」）
- 实测：`--web-chat "用一句话介绍你自己"` → HTTP 200 / PoW 由页面内求解 / 正文 PASS；
  `--run-selftest --web` → **完成 5/5，失败 0，跳过 0，耗时 9.83s**，TextOutput 收到完整正文；
  `--selftest` 七组 PASS；`--graph-selftest` 95/0；`--exec-selftest` 56/0；`--run-selftest`（离线）PASS

**网页版（M4-06/M4-08/M4-09）第一阶段：协议探明 + 鉴权链路打通**

- `src/web/session_store.{h,cpp}`：`Session.user_token`（**仅内存**，日志/文件永不落）；`ProbeResult`（脱敏协议探测结果：localStorage 键名、Token/JWT 脱敏值、challenge 原文、端点报告）；`SessionStore::set_probe()`（真 Token 只写入 `user_token`，探测结构本身可安全打印）
- `src/web/webview_host.{h,cpp}`：新增**两段式协议探测**（`ExecuteScript` 注入异步脚本 → 写入 `window.__aiwriteProbe` → 宿主 500ms 轮询取回；规避部分运行时 `ExecuteScript` 不等待 Promise 的问题）；`LoginRequest` 新增 `probe_after_load` / `auto_close_after_probe`；`web::request_protocol_probe()`（在已打开窗口内探测）与 `web::protocol_probe()`（`--web-probe` 离屏自检）
- `src/ai/web_pow.{h,cpp}`（新）：PoW 挑战解析（`data.biz_data.challenge`）、标准 Base64、`x-ds-pow-response` 头构造（与官方一致）
- `src/ai/deepseek_web_client.{h,cpp}`（新）：`web_chat()` = 取挑战 → 求解 → `POST /api/v0/chat/completion` → SSE 解析（通用文本提取 + `raw_head` 原始行诊断）；会话「新建成功则用新建、否则复用最近会话」
- `src/main.cpp`：新增 `--web-probe`（协议探测自检）与 `--web-chat "<提示词>"`（端到端生成）；`src/ui/property_panel.cpp`：网页版会话区新增 userToken 状态与「探测网页版协议（dev）」按钮
- `docs/网页版协议实测记录.md`（新）：实测端点、鉴权头、PoW 挑战形态、官方 `x-ds-pow-response` 头格式、**PoW 哈希为自定义实现**（非 NIST SHA3-256 / 非 Keccak-256）的指纹证据，以及两条落地路线（A：隐藏窗口调用官方 worker，已实测可用；B：忠实移植其 sponge）
- 实测：`--web-probe` PASS（约 4.5s）；`GET /api/v0/users/current` 200 且 `code=0`；PoW 头被服务端接受（不再报 422/缺失），当前仅剩 `INVALID_POW_RESPONSE`（哈希实现差异）

**M2 P3-6：工作流打开 / 保存接线 + recent.json（M2-05 收尾）**

- `src/engine/recent_files.{h,cpp}`（新）：`~/.brain-ai/recent.json`（`load/push/clear`，去重置顶、上限 10 条、路径用 `weakly_canonical` 归一化比较、损坏文件按空列表处理）
- `src/utils/paths.{h,cpp}`：新增 `recent_file()`；`src/utils/file_dialog.{h,cpp}`：新增 `save_file()`（NFD `SaveDialog`，支持默认目录与默认文件名）
- `src/ui/editor_state.{h,cpp}`：`save_workflow_to()`（保存 + 记入最近 + 写 `workflow.log`）、`open_workflow_from()`（**加载 → 加载校验（不合法拒绝且保持原图）→ 清空撤销栈 → 视图跟随 → 记入最近**）、`recent_workflows()`、`abort_run_if_any()`（运行中切换工作流/修改工作流统一终止）；`open_workflow_dialog()` / `save_workflow_dialog()` 供菜单与工具栏共用
- `src/ui/toolbar.cpp` + `src/ui/app.cpp`：工具栏新增「打开工作流 / 保存工作流」；文件菜单「打开工作流…」「保存工作流…」「最近打开 ▸（含清空列表）」
- `tools/api_probe.cpp`：`--selftest` 新增 **V-10 工作流文件 / 最近列表**（保存→加载往返一致、加载校验、**密钥不落盘**、最近列表去重置顶/上限 10/清空、含未知节点类型的文件被拒绝；测试前后自动备份/还原用户 `recent.json`）
- `src/main.cpp`：`--run-selftest` 增加 app 侧「保存 → 打开」往返验证
- 实测：`--selftest` 七组全 PASS（exit 0）；`--run-selftest` PASS（`保存=OK / 打开=OK / 节点数一致=OK`）；`recent.json` 未被自检污染

- `src/utils/log.{h,cpp}`：新增 `log::workflow(message)` 与第二个日志器（`workflow` logger，`rotating_file_sink` 10MB × 5，同一格式化器，注册到 spdlog registry 以便 `flush_every` 覆盖）；`workflow_file_path_string()` 供 UI/自检展示
- `src/ui/editor_state.cpp`：执行事件同时写 `workflow.log`——运行前校验结果、`===== 运行开始 =====`、逐节点开始/完成/失败/跳过、`===== 运行结束：完成 … =====`；`app.log` 侧保持 `[执行] …` 行（Console 面板同源）
- 实测：`aiwrite.exe --run-selftest` → `~/.brain-ai/logs/workflow.log` 生成，内容为一次运行的完整事件流（14 行），与 `app.log` 分离

- `src/ui/editor_state.{h,cpp}`：`EditorState` 持有 `engine::Executor`；新增 `start_run()`（先运行前校验 → 失败逐条写 Console 并拒绝启动）、`cancel_run()`、`tick_run()`（主循环每帧推进一个节点）、`run_status_text()`；**守卫**：运行中修改工作流（`snapshot()` 入口）会自动终止本次运行并把节点状态复位
- `src/ui/toolbar.cpp`：工具栏右端由「占位按钮」升级为 **「▶ 运行」+「■ 停止」**（运行中禁用运行、空闲禁用停止；悬停说明运行前校验与节点配色含义）
- `src/ui/app.cpp`：主循环每帧调用 `editor().tick_run()`；状态栏新增运行进度（运行中 `运行中 | 3/5 | 0.1s | 当前 n3`，结束后显示 `完成 3/5，失败 1，跳过 1，耗时 …（finished）`）
- `src/main.cpp`：新增 **`aiwrite.exe --run-selftest`**（不开窗口，直接验证 app 侧运行接线：建示例工作流 → 运行到结束 → 打印各节点状态与统计）
- 实测：`--run-selftest` **PASS**（n1/n2/n5 done、n3 error「尚未接线」、n4 skipped、退出码 0）；控制台输出全部经 `log::info` 进入 Console 面板与 `app.log`

**M2 P3-1 / P3-2：拓扑排序 + 三级校验 + 执行器核心**

- `engine/graph.{h,cpp}`：`Graph::topologicalOrder()`（Kahn；零入度按插入序出队 → 稳定可测；只统计"连接两个存在节点"的边；有环时 error 列出环内节点）
- `engine/validate.{h,cpp}`（新）：`validateWorkflow()`（节点类型已注册 / id 唯一 / 参数合法 / 边两端节点与端口存在 / 端口类型兼容 / 非可变长输入不被重复占用 / 无环）与 `validateBeforeRun()`（必填输入已连接、可变长输入 ≥1 条边、Provider 可用性仅给 warning 不阻断）
- `engine/executor.{h,cpp}`（新）：`Executor`（`start()` 内强制执行运行前校验 + 拓扑 + 执行实现检查；`tick()` 每帧推进一个节点；`runToCompletion()` 供自检/将来命令行；`cancel()` 协作式取消；`summary()` 会话统计）、`NodeOutputs`（运行态值，**不进入 Graph** → 撤销快照与工作流 JSON 不受执行结果影响）、`NodeError`（设计 §9.3）、`ExecutionContext`（console/state 回调 + cancel 标志）、`NodeExecutorRegistry`
- `src/nodes/nodes.h` + `local_nodes.cpp` + `register_executors.cpp`（新）：7 个本地节点执行函数（文本输入 / 图片输入 / 提示词模板 / 文本合并 / 提供商配置 / 文本输出 / 图片预览）+ 2 个占位（LLMGenerate / VLMGenerate 抛 `NodeError`，正好用作失败传播用例）；`ProviderConfig` 输出只含 `has_api_key` 布尔值，**绝不输出 Key 明文**（设计 §8.4）
- 节点约定：单输出节点 → 返回值即该端口值；多输出节点 → `{"端口id": 值}`；变长输入 → 按边插入序收集为数组；`PromptTemplate` 变量规则 = `{端口id}` / `{1}{2}…` / 单值时 `{vars}`，未匹配占位符原样保留（注册表默认模板同步改为 `…：\n{vars}`）
- `tools/api_probe.cpp`：新增 **`--exec-selftest`**（**129 项断言**，全离线：拓扑 8 组 / 加载校验 8 组 / 运行前校验 9 组 / 执行器 15 组：值传递·状态流转·失败下游 Skipped·无关分支继续·取消·Provider 脱敏）；`--selftest` 增加「执行器」组
- 实测：全量构建 **0 error / 0 warning**；`--graph-selftest` **95/0**；`--exec-selftest` **129/0**；`--selftest` 六组全 PASS；GUI 冒烟正常（首帧 + 干净退出）

**M1 收尾（配置接线 + 遗留审计）**

- `src/ui/app.cpp`：面板可见性改为**读 `config.toml` 的 `[ui]` 段**——此前 `show_library` / `show_params` **硬编码 `true`（等于忽略配置）**，现改为 `config.ui.show_node_library` / `config.ui.show_property_panel`，落实设计 §7.2「节点库 / 参数面板默认隐藏、可切换」；启动日志新增 `界面可见性（config.toml [ui]）: 节点库=… 参数面板=… Console=… 网格=…`；「视图」菜单切换时**写回** `config.toml`（M6-06 设置面板落地前的过渡方案）
- 文档（M1 收尾审计）：`actionPlan/M1.md`（§三 目录树改为实际结构、偏差校正补「`.vscode` 8 配置 / 9 任务」、M1-04 Bootstrapper 对策注明顺延 M6、§八 验收表加状态列、§十一 4 项待确认标 ✅ + 遗留归属表）；`M1_技术验证报告.md` §4.3 增「配置字段应用」；遗留登记：`recent.json`→**M2-05**、`workflow.log` 独立 sink→**M4-13**、WebView2 Runtime 检测/安装引导→**M6-02/M6-08**
- 实测：全量构建 **0 error / 0 warning**；`config.toml` 置 `true/true` 与 `false/false` 各启动一次，日志分别输出「节点库=显示，参数面板=显示」与「节点库=隐藏，参数面板=隐藏」→ **配置确认生效**（两轮退出码均 0）

**交互补强：运行按钮 UI 占位 + 网页版登录（W1）**

- `src/ui/toolbar.cpp`：新增 **「▶ 运行」按钮**（右对齐 = 顶栏右上角，设计 §6.5 / §7.1）。**本步骤只渲染 UI、不接逻辑**（禁用态 + 悬停说明），执行引擎在 M2-03（拓扑排序）/ M2-04（单线程分帧执行）落地后接线，届时连同设计里的「停止」按钮与状态栏进度一起启用
- `src/web/session_store.{h,cpp}`：网页版会话存储（设计 §8.5：Cookie **只存内存、退出即销毁**；`mask_value()` 脱敏；线程安全快照 + `clear()` 注销）
- `src/web/webview_host.{h,cpp}`：**内嵌 WebView2 有头登录窗口**，跑在独立线程（WebView2 需要 STA + 自己的消息泵），登录期间主界面保持响应；页面加载完成与每 3 秒自动重取 Cookie → 写入内存会话；`Ctrl+Alt+C` 立即提取、`ESC` 关闭；退出前由 `stop_login_window()` 显式收尾（避免静态析构撞日志关闭）
- `src/engine/graph.{h,cpp}`：`Param` 新增**条件可见性** `visible_when_param` / `visible_when_value` + `engine::param_visible()`（未设置 = 始终可见，向后兼容）；`Graph::validateParams()` 跳过条件隐藏的参数
- `src/engine/node_registry.cpp`：ProviderConfig 的 `api_base / model / api_key / api_key_ref` 标记为仅 `mode == official` 可见 → **模式切到 web 时 official 专属参数自动隐藏**
- `src/ui/property_panel.cpp`：ProviderConfig 且 `mode == web` 时显示「网页版会话」区（状态 / 来源 / 更新时间 / `ds_session_id` 脱敏值 + 「打开登录窗口」「关闭登录窗口」「注销（清会话 + 删除登录 profile）」）；official 模式给切换提示；末尾提示被隐藏的参数条数
- `src/ui/node_canvas.cpp`：节点卡片参数预览同样按可见性过滤（与面板、校验共用 `param_visible()`）
- `src/ui/app.cpp`：状态栏新增 `推理模式: 官方 API / 网页版（已登录 N 条 Cookie）/ 网页版（未登录）/ 未配置`
- `src/main.cpp`：新增 `aiwrite.exe --login-selftest [--timeout N]`——GUI 程序自动挂父控制台（且尊重已有重定向），离屏跑「WebView2 环境 → 控制器 → 导航 → 提取 Cookie」，退出码 `0=通过 / 1=失败 / 2=超时`
- `tools/api_probe.cpp`：`--selftest` 新增 **V-09 会话存储 / 参数条件可见性**（写读 → 注销清空；official 显示 `api_key`、web 隐藏 `api_key`/`api_base`，两模式校验均通过）
- `CMakeLists.txt`：`aiwrite` 链接 `${AIWRITE_WEBVIEW2}`（此前仅 CLI 工具 `webview2_login` 链接），新增 `src/web/session_store.cpp`（core）与 `src/web/webview_host.cpp`（GUI）

**实测（2026-09-23）**：全量构建 **0 error / 0 warning**；`api_probe --selftest` → `SHA3 / HTTP / 请求构造 / 配置往返 / 会话与可见性` **全 PASS**（退出码 0）；`--graph-selftest` **95 通过 / 0 失败**；`aiwrite.exe --login-selftest --timeout 30` → **PASS，Cookie 5 条（脱敏），5~7 秒，退出码 0**；GUI 冒烟首帧正常、退出码 0。


### Added（本轮）

**P1 数据层（`api_probe --graph-selftest`：初版 76 通过 / 0 失败；加入序列化断言后 95 通过 / 0 失败）**

- `src/engine/graph.{h,cpp}`：Node / Port / Param / Edge / Graph 数据模型（设计 §5.1–5.6）；端口类型兼容矩阵（同类型 ✅ / `any` 双向 ✅ / 其他 ✗）、参数校验（必填为空 / 超范围 / 枚举非法 / 文件与目录存在）、节点与连线增删、节点复制、稳定 id（`n1` / `e1`）
  - 创建类接口统一**返回 id**（而不是 `Node*`）：自检时实测到「返回 `&nodes.back()` 后被 `push_back` 重分配打悬空」的缺陷，改为返回稳定 id + `findNode(id)` 访问
- `src/engine/node_registry.{h,cpp}`：9 个 MVP 节点注册表（分类 / 端口 / 参数 / 默认值 / 范围 / 枚举 + 说明文案）
- `src/engine/undo_stack.{h,cpp}`：快照式撤销/重做，深度上限 50，新操作丢弃重做分支
- `src/ui/property_panel.{h,cpp}`：参数面板（String / Text / Int / Float / Bool / Enum / File / Directory / Color 九类控件、校验提示、密码框、重置为默认值）；控件激活时上报 `begin_edit`，保证撤销快照落在值变化**之前**
- `src/utils/file_dialog.{h,cpp}`：nativefiledialog-extended 原生文件/目录对话框（`NFD::Init/Quit` 配对封装）
- `tools/api_probe.cpp`：新增 `--graph-selftest`（注册表 / 增删 / 兼容矩阵 / 端口替换 / 可变长 / 删除连带 / 复制 / 参数校验 / 撤销重做，共 76 项断言；后续加入序列化断言 → 95 项）

**P2 画布与编辑器**

- `src/ui/node_canvas.{h,cpp}`：真实 Graph 画布（替换 M1 演示）——节点卡片（分类色条 + 状态圆点 + 端口 + 参数预览 + 校验错误）、端口按类型着色、可变长端口双圈、右键画布分类新建、右键节点复制/删除/改名、右键连线删除、拖拽连线（类型不符变红拒绝 / 输入端口替换）、双击节点聚焦改名、Ctrl+单击与框选多选、中键平移、滚轮缩放 0.1x–4.0x、稳定手柄映射（节点 = 序号；端口 = `序号<<8 | 输出位 0x80 | 下标`）、`ed::EnableShortcuts(false)` 落实「无快捷键」
- `src/ui/editor_state.{h,cpp}`：Graph + UndoStack 唯一持有者，统一「修改前压快照」的编辑操作（创建 / 删除 / 复制 / 粘贴 / 清空 / 示例工作流）
- `src/ui/node_library.{h,cpp}`：左侧节点库（按分类，点击创建到画布中心，悬停显示端口与参数清单）
- `src/ui/toolbar.{h,cpp}`：工具栏（撤销 / 重做 / 复制 / 粘贴 / 删除选中 / 新建 / 示例 + 计数与撤销栈深度；全部鼠标操作）
- `src/ui/theme.h`：分类 / 端口 / 状态配色集中定义（设计 §14.3 / §4.5 / §4.6）
- `src/ui/app.cpp`：停靠布局改为「节点库 | 画布 | 参数 + 工作流信息 / Console」；新增工具栏行、菜单「编辑」（撤销/重做/复制/粘贴/删除选中）、状态栏显示节点/连线/选中数与操作提示；启动自动创建 4 节点示例工作流
- 删除 `src/ui/node_editor_demo.{h,cpp}`（M1 演示数据画布）

**依赖**

- 新增 overlay port `source/ports/nativefiledialog-extended`（v1.3.0）+ `source/vcpkg-configuration.json`（`overlay-ports: ["./ports"]`）+ `source/vcpkg.json` 依赖项；工程 `find_package(nfd CONFIG REQUIRED)` + `nfd::nfd`（nfd.dll 由既有运行时 DLL 同步逻辑拷贝）

**文档**

- 新增 `docs/节点编辑器使用说明.md`（操作手册 + 20 项人工验证清单 + 已知限制）
- 设计文档同步：§6.4（内置 32px 网格，20px 自绘作废）、§6.6 / §6.7 实现说明、§20.1 目录结构、§20.3 依赖状态
- `docs/actionPlan/M3.md` 增加「实施进度」对照表

### Added（本轮）

**M1-04 WebView2 自检能力（`tools/webview2_login.cpp`）**

- 新增命令行选项：`--selftest`（隐藏/离屏自动跑「导航 → 提取 Cookie」全链路，**退出码 0=通过 / 1=失败 / 2=超时**）、`--url <url>`（默认 `https://chat.deepseek.com/`）、`--timeout <秒>`（自检默认 30；交互模式也可用来自动关闭）、`--hidden`（离屏窗口，不抢焦点）
- 自检结果写入 `app.log`（`[V-03] WebView2 自检：Cookie N 条 → PASS/FAIL`），并在控制台输出脱敏 Cookie 清单（名称/前4后4/属性）
- 实测：`webview2_login.exe --selftest --timeout 30` → **PASS，Cookie 5 条，1.2~2.1 秒，退出码 0**（连续两次复现一致）

**M1-05/06/07 自检增强（`tools/api_probe.cpp`）**

- `--selftest` 现覆盖四组：`V-05 SHA3` / `V-04 HTTP` / **`V-06 请求构造`** / **`V-08 配置往返`**（后两组不需要 Key、不依赖用户配置）
- 新增 `--chat "<prompt>" --dry-run`：打印 `POST /chat/completions`、Authorization（已设置时显示 `Bearer ****`）、body JSON，并断言 `model 非空 / stream=false / messages[1]{role=user, content=prompt}`；不发起网络请求，无需 Key
- `test_http` 输出连接/读取超时与证书校验状态，并把 `HTTP 请求失败` 按错误类型区分 **SKIP（网络/代理不可达，不计失败，退出码 2）** 与 **FAIL（TLS/其它，退出码 1）**
- 新增配置往返自检：临时文件上 `load（生成默认）→ 改 language/show_grid/ttl_days/model → save → load` 断言相等
- 实测：`api_probe --selftest` → `SHA3 PASS / HTTP PASS / 请求构造 PASS / 配置往返 PASS`，退出码 0；`--graph-selftest` 95 通过 / 0 失败

**工作流 JSON 序列化（设计 §4.7 schema，M2-05 核心部分）**

- `src/engine/workflow_io.{h,cpp}`：`graph_to_json` / `graph_from_json` / `save_workflow` / `load_workflow`
  - schema：`version / name / description / nodes[{id,type,title,position,params}] / edges[{id,from,to}] / viewport`
  - 反序列化以**节点注册表为准**重建端口与参数定义，参数值取自文件，缺失参数保持默认值；未注册类型与非法连线**跳过并告警**；坐标/缩放做合法性夹紧（防御损坏文件）
  - **密钥不落盘**：`is_secret` 参数（如 `api_key`）不写入、也不从文件恢复
- **默认启动工作流落盘**：启动时创建默认工作流并保存到 `~/.brain-ai/workflows/default.json`（设计 §11.3），日志记录路径
- **默认工作流新增「提供商配置」节点**：`n5 ProviderConfig(220,260)` 的 `provider` 输出接入 `n3 LLMGenerate.provider` 输入（默认工作流 = 5 节点 4 连线）
- `api_probe --graph-selftest` 增加 17 项序列化断言（结构/数量/密钥不落盘/往返一致/文件往返/非法输入被拒）→ 合计 **95 项**

### Fixed（本轮修复）

| 问题 | 根因与处理 |
|---|---|
| 参数面板下拉框（Enum）选了不生效 —— 例如「提供商配置 → 模式」无法从 `official` 切到 `web` | 根因：判定条件写成 `ImGui::Combo(...) && ImGui::IsItemDeactivatedAfterEdit()`，但 Combo 在**点击下拉项的那一帧**就返回 true，而被点击的选项是弹出层里的另一个 item，该帧 `IsItemDeactivatedAfterEdit()` 并不成立 → 值永远写不回去（`model` / `mode` 等所有枚举参数都受影响）。处理：改为**值变化即写回**（`Combo` 返回 true 时立刻写入，索引做了 `std::clamp` 保护） |
| 链接 `aiwrite.exe` 报 `LNK1163: invalid selection for COMDAT section` | 头文件里的 `inline constexpr const char* kWorkflowVersion` 触发 MSVC 链接期 inline 变量的 COMDAT 合并问题。处理：改为普通 `constexpr`（内部链接），并在文件内注明原因 |


| 问题 | 根因与处理 |
|---|---|
| **启动后界面无响应（CPU 单核打满、内存涨到 ~216 MB 后不动）——曾误判为"内存泄漏"** | **真凶**：`~/.brain-ai/node_editor.json`（imgui-node-editor 自己持久化的**节点坐标 + 视图 scroll/visible_rect/zoom**）里残留了**损坏数值**——`"node:5": {"location": {"x": -2147483520, "y": -2147483648}}` 与 `"zoom": 2.16e-07`、`visible_rect` 跨度 ±3e9。这些值正是更早"复制/粘贴"脏值事故（`GetNodePosition → FLT_MAX`）留下的；编辑器启动即恢复该视图，导致**每帧网格与剔除计算爆量** → CPU 打满、界面冻结（用临时阶段追踪定位到卡在 `ed::End()` 内部，再用 VEH/内存采样确认"CPU=1.0 秒/秒、内存平稳"以排除泄漏）。**处理**：① 删除/备份损坏的 `node_editor.json`；② 代码加**三层防护**——启动前 `sanitize_editor_settings()`（校验坐标/缩放，异常则备份改名重建）、运行期**视图自愈**（`GetCurrentZoom`/视图原点异常即 `NavigateToContent` 重置）、模型侧 `clamp_position()` 夹紧 ±100000 |
| **诊断能力补齐（新增）** | `src/utils/diagnostics.{h,cpp}`：VEH **首异常计数器**（只做原子计数，汇总时输出**抛出模块 + 偏移**，限流）+ **内存采样**（工作集/私有/峰值 + 节点/连线/撤销栈）+ 渲染循环**心跳**；全部以 `[诊断]` 行写入 `app.log`（启动前几帧 + 之后约每 30/60 秒一条），用于今后此类"卡死 / 疑似泄漏 / 异常洪流"问题的快速定位 |
| **复制/粘贴导致界面无响应（卡死）** | 根因链：粘贴会在"本帧节点绘制之后"新建节点，而同帧末尾的 `sync_positions()` 会读 `ed::GetNodePosition(新句柄)`——该手柄编辑器还不认识，函数返回 **`FLT_MAX`**（`imgui_node_editor.cpp:1676`）→ 旧代码把这个值当位置**回写进 Graph** → 下一帧 `ed::SetNodePosition(FLT_MAX)` 经 `Floor()` 变成 ~`-2.1e9`，而粘贴又是**唯一同时请求视图跟随**的操作（`request_navigate_to_content = true`）→ `NavigateToContent` 在 ~1e9 坐标上计算视图 → 绘制/断言数学溢出 → 无响应（日志实测：卡死点精确定位在粘贴那一帧）。**处理**：① 复制/粘贴 **UI 入口下线**（右键菜单 / 工具栏 / 编辑菜单，逻辑与剪贴板代码保留，M2/M3 重做）；② 加**位置守卫**（`sync_positions` 双向同步：编辑器不认识该节点时改为"模型 → 编辑器"推送，绝不回写脏值；`clamp_position()` 夹紧到 ±100000；`NavigateToContent` 前校验坐标） |
| 连线右键菜单（「删除连线」「选中该连线」）点了无效 | 第三层根因：`context_link_handle` 是**普通局部变量，每帧被重置为 0**，而右键菜单在**后续帧**才被点击 → 算出 `edge_id = "e0"` → 找不到连线（对照：`context_node` 是 `static`，所以节点菜单一直正常）。**处理**：按需求**移除该菜单**（含 `ShowLinkContextMenu` 分支与悬停兜底），右键菜单链回到「节点 > 背景」；`context_node_handle` 一并改为 `static` 消除同类隐患；连线删除保留已验证可用的「左键单击连线选中 → 工具栏【删除选中】」 |
| 节点端口不区分输入/输出、连线方向看不清（端口圆点位置混乱） | `draw_pin` 原先把「圆点 + 名称」和「名称 + 圆点」都按同一列左对齐排布，输出端口圆点紧跟在文字后面而不是节点右边缘。处理：重写为**真正的左右两列**——输入行 `[圆点][名称]` 贴节点左边缘、输出行 `[名称][圆点]` 右对齐到 `kNodeContentWidth = 215px`（节点内容宽度统一由标题行的占位 Dummy 保证），连线锚点用 `PinPivotAlignment(0,0.5)` / `(1,0.5)` 分别取端口矩形左中 / 右中 |
| 连线无法删除（右键「删除连线」点了没反应） | **两个叠加原因**：① 右键菜单写成三个**并列的 `if`** 且 `ShowBackgroundContextMenu()` 在最前，背景菜单先消费掉右键事件 → 节点/连线右键菜单根本打不开；② 即便菜单打开，官方做法 `ed::DeleteLink()` 也会**静默失效**——其内部 `DeleteItemsAction::Add()` 开头就是 `if (Editor->GetCurrentAction() != nullptr) return false;`，而右键菜单打开期间"上下文菜单动作"正是 current action（官方 blueprints 的 Delete 菜单项同样有此问题）。**处理**：① 改为官方 `blueprints-example` 的 **else-if 链（节点 > 连线 > 背景）**；② 放弃 `ed::DeleteLink()`，统一以 **Graph 为唯一真相直接删边**（画布随后不再提交该连线，编辑器会在本帧 `End()` 因 `!m_IsLive` 自动回收内部连线对象，见 `imgui_node_editor.cpp:1307`）。现在连线有 3 条鼠标删除路径：右键连线 →「删除连线」、左键点连线选中 → 工具栏「删除选中」、画布右键菜单 →「删除选中（节点 / 连线）」，均在修改前压撤销快照 |
| 拖动端口连线时弹出 imgui-node-editor 断言 `IM_ASSERT(false == m_InActive)`（`CreateItemAction::Begin()`） | 根因：`ed::BeginCreate()` 内部会执行 `CreateItemAction::Begin()`（`m_InActive = true`），**只有 `ed::EndCreate()` 复位**；原实现写成 `if (!ed::BeginCreate(...)) return;`，未成对调用导致 `m_InActive` 恒为 true，下一帧再进 `Begin()` 即断言失败。处理：改为无条件调用 `EndCreate()`/`EndDelete()`（与官方 widgets/blueprints 示例一致），并把该契约写进 `node_canvas.cpp` 注释 |
| CMake 警告 `Compatibility with CMake < 3.10 will be removed` / `Update the VERSION argument <min> value. Or, use the <min>...<max> syntax` | 来源是 vcpkg 自带工具链 `scripts/buildsystems/vcpkg.cmake:40,878` 的 `cmake_policy(VERSION 3.7.2)`。处理：本项目 `cmake_minimum_required` 改用区间写法 **`3.25...4.6`**（策略版本取 4.6，新版 CMake 不再因策略未设置告警），并设置全局策略下限 **`CMAKE_POLICY_VERSION_MINIMUM=3.10`**（策略版本 = max(声明值, 下限)，既消除该 deprecation 警告，也容忍仍声明老版本号的历史代码/端口） |
| 在 VS Code「CMake Tools」里构建报 `Could not find a package configuration file provided by "imgui"` | 根因：vcpkg 的 `VCPKG_INSTALLED_DIR` 由 `dev.ps1` 环境变量提供，一旦构建树的缓存缺失/被重建，CMake 自行重新配置时取不到该路径回退到 `<build>/vcpkg_installed`，于是找不到 imgui。处理：把 vcpkg 相关路径**写进 `CMakePresets.json` 的 `cacheVariables`**（`VCPKG_INSTALLED_DIR`、`VCPKG_TARGET_TRIPLET`、`VCPKG_HOST_TRIPLET`、`VCPKG_MANIFEST_DIR`、`VCPKG_MANIFEST_INSTALL=OFF`、`CMAKE_PREFIX_PATH`、`CMAKE_POLICY_VERSION_MINIMUM`），使「仅 cmake --build / 无 dev.ps1 环境」的配置也能成功（已实测干净环境 configure + build 通过） |
| `install-deps.cmd` 无法确认端口构建使用的 CMake | 增加 `where cmake` + `cmake --version` 输出（实测为工程自带 3.31.6），并在 `third_party/cmake-3.31.6` 缺失且系统 CMake ≥ 4.0 时给出提示与解决办法（triplet 注入 `VCPKG_CMAKE_CONFIGURE_OPTIONS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5`） |

### Changed（本轮）

- **UI 入口下线（暂停，逻辑保留）**：复制/粘贴的 4 个入口（画布右键菜单 / 节点右键菜单 / 工具栏按钮 / 编辑菜单）改为禁用占位项 `复制/粘贴（暂停，M2/M3 重做）`；`EditorState::copy_selection/paste_clipboard/has_clipboard` 与剪贴板数据**原样保留**（含暂停原因与接回前置条件的注释）
- **连线右键菜单移除**：`ShowLinkContextMenu` 分支、「悬停命中」兜底与 `##link_menu` 弹窗全部删除；连线删除仅保留「左键单击选中 → 工具栏【删除选中】/ 画布右键【删除选中（节点 / 连线）】」
- CMake 工程版本 `0.1.0` → **`0.2.0`**（`project(VERSION)`、界面版本号与「关于」菜单同步）
- `source/install-deps.cmd`：CMake 版本策略注释与自检输出
- `source/CMakePresets.json`：新增 `VCPKG_HOST_TRIPLET`、`CMAKE_POLICY_VERSION_MINIMUM` 缓存项
- **M1 文档漂移校正**：`docs/M1_技术验证报告.md`（产物路径 `build/ninja-debug/bin/` → `build/bin/`、删除方式由 `Delete` 键改为鼠标菜单、launch 7 配置 / tasks 9 任务、产出物清单更新）、`docs/actionPlan/M1.md`（新增"实施偏差校正"）、`source/README.md`（7 配置 / 9 任务）

### 待完成（均需人工执行一次）

- ✅ **M1 实施项已全部完成**（2026-09-23 校正）：M1-01 ~ M1-07、V-01/V-02/V-04/V-05/V-07/V-08、A-01/A-02/A-03/A-07/A-08 均已落地或验证；文档漂移（产物路径 / 删除方式 / 任务与配置数量）已修正
- ⏳ **V-03**：运行 `build/bin/webview2_login.exe` 手动登录 DeepSeek → `Ctrl+Alt+C` 提取 Cookie（工具已就绪，已验证 WebView2 环境可创建）——**需您本人操作一次**
- ⏳ **V-06**：设置环境变量 `DEEPSEEK_API_KEY` 后执行 `api_probe.exe --chat "..."`，验证官方 API 生成文本——**需您的 Key**
- ⏳ **M1 收尾**：上述 V-03 / V-06 完成后，A-09（V-01 ~ V-08 全通过）即闭环
- ⏳ **M3 验收（原 A-03）**：按 `docs/节点编辑器使用说明.md` §8 的 **22 项**清单在界面中逐项确认（删除一律走鼠标菜单；复制/粘贴已暂停）
- ⏳ **M2 计划待审核**：`docs/actionPlan/M2.md` + 对话中整理的"剩余任务（M2-03 / M2-06 / M2-04 / M2-05）与 K1–K5 决策"

---

## [0.1.0] - 2026-09-22 — M1 技术验证 + 项目骨架

### Added

**环境与构建体系**

- 工具链：MSVC **14.50**（VS 2026 Insiders）+ 系统安装 **CMake 4.4.3** + `Visual Studio 18 2026` 生成器（无需 Developer Prompt / vcvars64）
- `source/build.ps1`：一键配置 + 编译；`source/dev.ps1`：会话环境（vcpkg 变量 + CMake 解析）；`source/install-deps.cmd`：vcpkg 依赖安装
- `source/CMakePresets.json`：配置预设 `default`，编译预设 `debug` / `release`
- vcpkg manifest 模式（`source/vcpkg.json`）：依赖安装树 `vcpkg-installed/`、下载与二进制缓存 `vcpkg-cache/`（均在 F: 盘，不占用 C 盘）
- 构建产物统一在 `build/bin/`（exe 同级自动拷贝所需 vcpkg 运行时 DLL）

**应用骨架（M1-02 / M1-07）**

- `src/main.cpp`：程序入口（`--console` 可选分配控制台）
- `src/ui/app.cpp`：GLFW + OpenGL3 + Dear ImGui **1.90.7 (docking)**；深色主题（设计文档 14.3/14.4）；微软雅黑 14px + Consolas 13px（运行时加载系统字体，不把字体文件放进仓库）；停靠布局（节点画布 / Console / 工作流信息）+ 菜单栏 + 状态栏
- `src/ui/console_panel.cpp`：Console 面板（级别过滤 + 搜索 + `ImGuiListClipper` 虚拟化 + 自动滚动，设计文档 12.4）
- `src/utils/paths.cpp`：`~/.brain-ai` 数据目录（outputs / workflows / snapshots / logs / webview2 / config.toml / imgui.ini）
- `src/utils/log.cpp`：spdlog 三级别 + 双 sink（`app.log` 10MB × 5 轮转 + 控制台）+ 内存环形缓冲 10000 条 + 2 秒周期落盘
- `src/utils/config.cpp`：toml++ 读写 `config.toml`（字段与设计文档 20.2 一致，缺省自动生成）
- `src/utils/crypto.cpp`：SHA3-256（OpenSSL EVP，M4 PoW 复用）

**节点画布（M1-03）**

- `src/ui/node_editor_demo.cpp` + `third_party/imgui-node-editor` **v0.9.3**（源码集成）：2 个演示节点 + 1 条连线，支持拖拽 / 缩放 / 平移 / 新建连线 / 删除

**M1 验证工具**

- `tools/api_probe.cpp`：`--selftest`（V-04 HTTP + V-05 SHA3）、`--sha3`、`--http`、`--chat`（V-06，Key 只从环境变量读取）
- `tools/webview2_login.cpp`：WebView2 登录 + Cookie 提取（`Ctrl+Alt+C`，脱敏打印、仅内存、不落盘）

**工程化与文档**

- `.vscode/launch.json`（6 个调试配置）、`.vscode/tasks.json`（8 个任务）、`.vscode/settings.json`
- `source/README.md`（构建 / 运行 / 调试 / 常见问题）
- `docs/M1_技术验证报告.md`（环境基线、验证结果、问题与解决记录）
- 本 CHANGELOG

### Fixed

| 问题 | 处理 |
|---|---|
| 节点画布首帧崩溃 `0xC0000005`（进程假死、并留下僵尸进程占用 exe 导致后续 `LNK1104/LNK1168`） | 根因：`ed::GetStyle()` 实现为 `s_Editor->GetStyle()`，依赖"当前编辑器"；改为先 `ed::SetCurrentEditor()` 再应用样式 |
| imgui-node-editor 与 imgui 1.90.7 重复定义 `ImVec2 operator==/!=`（C2084） | 对 vendored `imgui_extra_math.inl` 打最小补丁（`#ifndef IMGUI_DEFINE_MATH_OPERATORS` 包裹，文件内已标注） |
| `Microsoft::WRL::Callback` 未定义 | 该符号声明在 `<wrl/event.h>`（不是 `wrl/implements.h`） |
| `LoadCursorW(nullptr, IDC_ARROW)` 类型不匹配 | 编译选项补充 `UNICODE` / `_UNICODE` |
| 运行中的进程占用 exe / DLL 导致链接或拷贝失败 | 运行时 DLL 同步改为逐文件、失败仅告警 |
| 日志长时间不落盘 | 增加 `spdlog::flush_every(2s)` |
| 依赖发现失败 | Windows 下 config 模式不搜索 `<prefix>/share/<pkg>`（显式追加 `share` 到 `CMAKE_PREFIX_PATH`）；`tomlplusplus` 端口无 CMake config（手工建导入目标）；`webview2` 目标名为 `unofficial::webview2::webview2` |

### Changed

- 构建体系统一为 **系统 CMake 4.4.3 + `Visual Studio 18 2026` 生成器**，**单一构建目录 `build/`**（含可用 VS 2026 打开的 `aiwrite.slnx`），产物统一在 `build/bin/`
- vcpkg 端口构建固定使用工程自带的 CMake `third_party/cmake-3.31.6`（本快照 ports 与 CMake 4.x 不兼容），仅作用于依赖安装
- M1 Action Plan 与设计文档中的环境/目录描述按实测结果同步修正

### Removed

- 调试期临时的分阶段启动参数及相关代码（`--minimal` / `--imgui-only` / `--no-node-editor`）：**只保留单一 `aiwrite.exe`**（`--console` 保留用于调试日志）
- 原 Ninja 构建预设与 `build/ninja-debug` 目录

### Verified（M1 验证项）

| 编号 | 验证项 | 结果 |
|---|---|---|
| V-01 | ImGui 窗口（中文显示） | ✅ 通过 |
| V-02 | 节点编辑器（渲染） | ✅ 通过（交互待人工确认） |
| V-03 | WebView2（登录 / 取 Cookie） | ⚠️ 环境创建成功，登录与 Cookie 待人工 |
| V-04 | HTTP | ✅ `HTTP 401 / 190 ms` |
| V-05 | SHA3-256 | ✅ 与 NIST 向量一致 |
| V-06 | DeepSeek API | ⏳ 待 API Key |
| V-07 | spdlog 写文件 | ✅ 通过 |
| V-08 | toml++ 读配置 | ✅ 通过 |
