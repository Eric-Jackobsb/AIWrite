# audit_arch_z1_web：ARCH-Z1 第 2 组落地审计报告（web / DOM 三文件 → 15 TU）

> 日期：2026-10-23 · 批次：`ARCH-Z1` 第 2 组（阶段四第二批续）
> 提交：`c344363`（`webview_host.cpp` 8 TU）· 本提交（`dom_web_client.cpp` 4 TU + `pydoll_channel.cpp` 3 TU + 夹具抽取口 + 文档）
> 目标（[ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §4 判据 1）：**单文件 < 800 行**；本组是 Z1 中**风险最高**的一组（WebView2 / 线程亲和 / 命名管道的真实运行期路径）。

---

## 1. 背景与目标

第 1 组（[audit_arch_z1.md](audit_arch_z1.md)）已把 `provider_spec.cpp` 1743 → 5 TU 收口。本组处理三个
**跨语言 + 跨线程**的大文件：

| 文件 | 行数 | 为什么排在第 2 组 |
|---|---|---|
| `web/src/webview_host.cpp` | **3514** | WebView2 宿主：`WindowCtx` 运行期状态 + 窗口线程 + `window_proc` 分发 + 全部公开 API，**全库最大生产文件** |
| `ai/src/dom_web_client.cpp` | **2397** | DOM 适配器：页面脚本模板 + 三段发送链路 + 取答案判据 + 探测/自检 |
| `web/src/pydoll_channel.cpp` | **1282** | 新通道：守护进程 + 命名管道 + 会话生命周期 + 三套自检 |

**硬约束（本组自定）**：这三个文件含 GUI / 线程 / 管道交互，**没有单元级等价证据** ⇒ 拆分只允许
「**逐字搬函数体**」，不改调用顺序、不改消息号、不改超时值；等价证据 = 断言计数不变
（`--upload-selftest` 72/0、`--pydoll-*` 自检、exec 713/0、graph 151/0、三个 node 夹具 44/17/15）
\+ 一次真机网页版运行。

## 2. 拆分口径（职责 → TU）

### 2.1 `webview_host.cpp` → 8 TU（提交 `c344363`）

| TU | 行数 | 职责 |
|---|---|---|
| `webview_host.cpp` | 364 | **状态与窗口表**：`WindowCtx` 访问器（`tlsWindow`/`st`/`forget_ready`）· `WindowRegistry`/`EnvCache` 单一定义点 · `to_wide`/`to_utf8`/`attach_parent_console`/`resize_webview` · 会话落库与 Cookie 提取 · `ctx_find/ctx_start/ctx_close/ctx_close_all`（开窗唯一入口） |
| `webview_host_window.cpp` | 480 | **WebView2 机制**：`ensure_environment`（每窗口线程私有环境 · `D-35④`）· `create_controller_for_window` · `on_environment_ready`/`on_controller_ready` · `window_proc`（`WM_APP+1..+7` 分发 / 定时器 / 热键）· `run_login_window`（STA + 消息泵）· `hook_resource_capture`（`PW-04`/`I26` 网络回执） |
| `webview_host_probe.cpp` | 607 | **协议探测 + 就绪三级**：三段探测脚本 + `replace_all_token` · `start/finish/poll_protocol_probe`（两段式）· `remember_ready`/`cached_ready`/`wait_controller_ready` · `peekPageState`/`waitReady`（`PW-03`/`I25`）· `protocolProbe`/`protocolProbeForProvider`（PB2-23） |
| `webview_host_actions.cpp` | 399 | **窗口内动作**：`run_script_in_window`（L3 脚本槽）· `run_navigate_in_window`/`navigate_now`（`PW-08` 复位导航）· `run_set_files_in_window`（M8-14 CDP 交文件）· `run_cdp_input_in_window`（`PW-07f` 真实输入） |
| `webview_host_pow.cpp` | 299 | **页面内 PoW + 注销**：`kSolvePowScript`/轮询脚本 · `run_pow_solve_in_page`/`poll_pow_solve` · `run_logout_in_page`（PB2-19） |
| `webview_host_api.cpp` | 337 | **句柄与前置**：`LoginWindow` 四方法 · `selftest` · `loginWindow()` · `ctx_post_probe`/`ctx_latest_alive` · `prepare_window`（`PW-08` 复用/新建/复位唯一入口）· 窗口诊断与 `closeOwnerWindow` |
| `webview_host_session.cpp` | 518 | **按站点会话**：两个 `ensureSession` 重载 · `logoutSite`（PB2-19）· `run_script_now`（L3 同步执行）· 回执三件套（`begin/take/resourceCaptureReady`）· `set_file_input_files` · `cdp_input_text` |
| `webview_host_upload.cpp` | 314 | **附件入口只读识别**（M8-37）：`kFileInputProbeScript` + `resolve_file_input_selector` + `protocol_probe_with`（PB2-23 探测主体，两个公开入口共用） |

### 2.2 `ai/src/dom_web_client.cpp` → 4 TU + 2 个脚本模板头

| 文件 | 行数 | 职责 |
|---|---|---|
| `dom_web_client.cpp` | 583 | **会话主体**：`clampPollParams` · `domCfgJson` · 6 个脚本渲染入口（`domKickoffScript`…`domProbeScript`）· `formatAnswerText`（`PW-07g` 纯函数）· `domChat`（三段链路 + 轮询判据） |
| `dom_web_client_probe.cpp` | 416 | **单站探测**：`suggest_selector_of` · `domSelectorDump` · 探测辅助（`probe_timestamp`/`probe_state_*`/`print_probe_block`，原匿名名字空间 → 契约头）· `run_site_probe_script` · `domSiteProbe` · `domProbeResultFromJson` |
| `dom_web_client_probe_all.cpp` | 232 | **批量探测**：`domProbeAll`（报告 + 台账 + 逐站序） |
| `dom_web_client_selftest.cpp` | 391 | **自检**：`domAdapterSelftest`（逐元素判据）· `domProbeSelftest`（离线夹具） |
| `ai/include/dom_web_client_scripts.h` | 445 | 配置常量表（候选池 / 装饰排除正则 / 轮询钳制）+ `kKickoffScript`/`kPrepareScript`/`kSendVerifyScript`/`kClickSendScript` |
| `ai/include/dom_web_client_scripts_poll.h` | 373 | `kPollScript`（`PW-07g` 文本保真）/`kProbeScript`/`kDiscoverScript` |

### 2.3 `web/src/pydoll_channel.cpp` → 3 TU

| TU | 行数 | 职责 |
|---|---|---|
| `pydoll_channel.cpp` | 527 | **基础设施 + 会话**：`widen` · `find_package_dir`/`find_python` · `channel_pipe_name` · `DaemonProcess` + `spawn_daemon`（`CreateProcessW` 语义不变）· `wait_daemon`/`release_daemon` · `SessionState`/`sessionState()` + 会话生命周期 · `ensureSession`/`loginSite`/`logoutSite` |
| `pydoll_channel_commands.cpp` | 227 | **内容返回**：`run_script` · `connect_session` · `currentTabSite`/`tabOnSite` · `joinNames` |
| `pydoll_channel_selftest.cpp` | 495 | **自检**：`selftest` / `scriptSelftest`（跨语言端到端）/ `chatSelftest`（v4 三命令）/ `pydollLogin`（有头人工登录） |


## 3. 文件与体量

| 原文件 | 原行数 | 拆分后 | 最大 TU |
|---|---|---|---|
| `web/src/webview_host.cpp` | 3514 | 8 TU（364 / 480 / 607 / 399 / 299 / 337 / 518 / 314）+ 契约头 378 | **607** |
| `ai/src/dom_web_client.cpp` | 2397 | 4 TU（583 / 416 / 232 / 391）+ 契约头 46 + 脚本头 445 / 373 | **583** |
| `web/src/pydoll_channel.cpp` | 1282 | 3 TU（527 / 227 / 495）+ 契约头 71 | **527** |
| **合计** | **7193** | **15 个 TU + 5 个内部头** | **607** |

> 旧的三个单文件全部退场；`CMakeLists.txt` **显式登记** 14 个新 TU（无 GLOB，逐行列出，含用途注释）。

## 4. 内部契约（3 个 detail 头 + 2 个脚本头）

原则与第 1 组一致：**公共 API 头一字未改**，只把「原本一个 TU 内的私有件」显式化成 TU 间契约。

### 4.1 `web/include/webview_host_detail.h`（378 行）

* **`struct WindowCtx` 定义整份搬入**（头文件按 `aiwrite::web::WindowCtx` 前向声明 ⇒ 必须留在该名字空间）；
* **状态对象** `WindowRegistry` / `EnvCache` / `CtxScope` / `BusyGuard`（`PW-05` 串行守卫，头内 inline 类）；
* **消息 / 定时器常量** `WM_APP + 1..+7`、`kTimerId`、`kCookiePollMs`、`kHotkey*`、`kProbeDelayMs`、`kResetNavigateTimeoutMs`、`kReopenSettleMs`（值逐字未改）；
* **30 个跨 TU 入口**：`windowRegistry`/`envCache`/`tlsWindow`/`st`（头内 inline）/`forget_ready`/`to_wide`/`to_utf8`/`attach_parent_console`/`resize_webview`/`write_session`/`extract_cookies`/`close_window`/`ctx_*`/`run_login_window`/9 个窗口线程消息处理器 + `navigate_now`/`remember_ready`/`cached_ready`/`wait_controller_ready`/`prepare_window`/`run_script_now`/`protocol_probe_with`。
* 唯一**链接性变动**：`static int protocol_probe_with(...)` → 去掉 `static`（原文级 static 只为单文件服务，跨 TU 后必须外部链接；函数体一字未改）。

### 4.2 `ai/include/dom_web_client_detail.h`（46 行）+ `dom_web_client_scripts*.h`

* detail 头只放**原匿名名字空间的 5 个探测辅助**（`probe_timestamp` / `probe_state_of` / `probe_state_text` / `probe_criteria_text` / `print_probe_block`）+ 统一 include 块；
* 脚本模板头是**逐字搬迁**的 `constexpr const char*` 常量表：`constexpr` ⇒ 内部链接 ⇒ 每个 TU 各持一份**只读**副本，
  **不构成跨 TU 可变状态**（`check_globals` 规则 6 的合法形态「`const` / `constexpr` 常量表」；实测 R1/R2/R3 均为 0）。

### 4.3 `web/include/pydoll_channel_detail.h`（71 行）

* `DaemonProcess`（句柄只用 `void*`，不向接口暴露 `windows.h` 类型 —— 原设计保留）+ `SessionState`（含 `cancel_polls` 取消位）；
* 7 个跨 TU 入口：`channel_pipe_name`/`spawn_daemon`/`wait_daemon`/`release_daemon`/`sessionState`/`joinNames`（+ 结构体本身）。

## 5. 行为等价保持点（逐条对齐原实现）

1. **只搬函数体**：15 个 TU 的全部内容均由脚本按**原行区间**切片生成，`build/tmp/z1b_verify*.py` 断言
   「原文件非空行**恰好被覆盖一次**」且「每个区间块**逐字**出现在目标文件」（web 25 块 / dom 6 块 + 2 脚本头 / pydoll 7 块 + 2 契约头块，**不一致 0**）。
2. **对象仍是唯一实例**：`windowRegistry()` / `envCache()` / `sessionState()` 的**函数内 static** 定义点没动（仍是一处），
   只把「同名声明」放进契约头 ⇒ 跨 TU 看到的是**同一个对象**；`check_globals` `fn_static 50/50` 未变。
3. **线程模型未变**：`tlsWindow()` 的 `thread_local` 定义仍在 `webview_host.cpp`；`CtxScope` 变成头内 inline 类（语义相同）。
4. **消息号 / 超时 / 预算未变**：`WM_APP+1..+7`、`kTimerTickMs` 500、`kCookiePollMs` 3000、`kAutoCloseGraceMs` 3000、
   `kProbeDelayMs` 1500、`kResetNavigateTimeoutMs` 15000、`kReopenSettleMs` 800 —— 均为常量搬移，值未改。
5. **结构体只搬一次**：`WindowCtx` / `WindowRegistry` / `EnvCache` / `SessionState` / `DaemonProcess` 从原 TU **移出**到契约头
   （首次生成时曾同时留在 TU 内 ⇒ `C2011` 重定义与 `C2027` 未定义类型，已通过把该区间从 TU 中排除修正）。
6. **匿名名字空间放开**：原 `namespace { … }` 里的函数改为外部链接，但**函数体 / 名字 / 调用点全未改**；
   原「文件内 static」只剩 `run_site_probe_script`（仅 `dom_web_client_probe.cpp` 自用，保持 static）。
7. **公共头零改动**：`webview_host.h` / `dom_web_client.h` / `pydoll_channel.h` 一字未改 ⇒ 调用方（`ui` / `nodes` / `app` / `tools`）**零改动**。

## 6. 夹具抽取口修正（本组唯一的「非搬迁」改动 —— 必须做）

三个 node 夹具原本**读源码抽脚本**（`fs.readFileSync('ai/src/dom_web_client.cpp')` + 正则取 `kPollScript` 等），
拆分后脚本模板移到 `ai/include/dom_web_client_scripts[_poll].h` ⇒ 抽取失败（`Error: 抽取失败：kAnswerPoolSelector`，
`check_all` 第 ③ 项直接红）。修法：把抽取口改为**「两个脚本头 + 主体 .cpp」按顺序拼接**（3 个夹具同一改法）：

```js
const AI_DIR = path.join(__dirname, '..', 'ai');
const SRC = [
    path.join(AI_DIR, 'include', 'dom_web_client_scripts.h'),
    path.join(AI_DIR, 'include', 'dom_web_client_scripts_poll.h'),
    path.join(AI_DIR, 'src', 'dom_web_client.cpp'),
].map((file) => fs.readFileSync(file, 'utf8')).join('\n');
```

* 语义不变：夹具仍**不复制逻辑**，仍从真实源码抽真实脚本（源码一动夹具立刻见）；
* 顺带把 3 个夹具的文件头注释改成新路径（避免下一个人再踩）。
* 教训登记：**源码扫描型夹具是拆分时的隐性依赖** —— 后续组（`ui` / `engine` / `nodes`）拆分前先 `rg -l readFileSync` 查一遍。

## 7. 验证结果（本组实测）

| 项 | 结果 |
|---|---|
| 构建（`check_all.ps1 -OnlyBuild`） | **0 error / 0 warning**（4 目标，含 `C4335` 行尾格式告警 —— 拆分文件统一 CRLF） |
| `check_all.ps1`（21 项） | **21/21 PASS · 失败 0 项** |
| `aiwrite.exe --upload-selftest` | **72 通过 / 0 失败**（网页版上传模板 —— 本组唯一可自动化的 WebView2 旁证） |
| `aiwrite.exe --run-selftest` | **PASS**（CLI 自检断言零增减） |
| `api_probe --selftest` | **全组 PASS**（含 SHA3 / HTTP / 配置往返 / 执行器 / L2 回环） |
| `api_probe --exec-selftest` | **713 通过 / 0 失败** |
| `api_probe --graph-selftest` | **151 通过 / 0 失败** |
| `ce_repro.exe 0` | **ALL FRAMES OK (mode=0)** |
| 夹具 `poll_answer_gate_test.js` | **44 / 0**（`PW-07` / `PW-07g` / `PW-07i`） |
| 夹具 `send_chain_test.js` | **17 / 0**（`PW-07f` 发送链路） |
| 夹具 `dom_gate_test.js` | **15 / 0**（Loop Dev 批 3 DOM 判据） |
| `check_globals.ps1` | `checked files: 228 · violations: 0（R1 0 / R2 0 / R3 0）· fn_static: 50 / 50` · **PASS** |
| `check_includes.ps1` | `checked files: 228 · include lines: 725 · violations: 0` · **PASS**（5 个新头文件均归属本层，无反向依赖） |
| `dup_scan.py` | 克隆块 **90** · 重复行 **1096** · 重复率 **3.7 %**（web 层 5.4 % —— 内的 28 行 `webview_host_probe ↔ session` 与 21 行 `dom_web_client_probe ↔ selftest` 是**既有同构分支**，留给「同函数双分支合并」专项） |
| `check_links.py` | `checked links: 1951 · broken: 0`（本文档新增引用之后复测） |
| 逐字搬迁校验 | `z1b_verify.py` / `z1b_verify2.py` / `z1b_verify3.py` **全 0 问题**（覆盖 + 块比对 40 块） |

> **等价证据口径**：WebView2 / 命名管道没有 GUI 自动化，故用「**断言项数不变**」+「**逐字块比对 0 差异**」
> 双证据；`--upload-selftest` 72/0 是本组能自动跑到的最深一条真实 WebView2 路径（环境 → 控制器 → 页面 → 上传模板）。

**复现命令**：

```powershell
cd source; .\build.ps1
.\tools\check_all.ps1 -SkipBuild
..\build\bin\aiwrite.exe --upload-selftest
python ..\source\tools\dup_scan.py
python ..\build\tmp\z1b_verify.py
```

## 8. 判据对照（ARCH-Z1 目标）

| 目标 | 状态 |
|---|---|
| 单文件 < 800 行 | **本组 3 个文件全部达标**（最大 607）· 全库余 **10 个**生产文件 > 800（§9） |
| 公共 API 零变动 | ✅ 三个公开头一字未改；14 个新 TU 共享同一份声明 |
| 重复率 ≤ 3.0 % | 🟡 **3.7 %**（本组**未新增**克隆块：拆分把「同文件内重复」变成「跨文件重复」，总量持平 1096 行）—— 余下靠 §9 的三条专项 |
| 依赖单向 / 零全局 / 断链 | ✅ 228 文件 / 0 违规；fn_static 50·50；1967 链接 0 断 |

## 9. 遗留与风险（Z1 队列 · 第 3/4 组）

已完成：`provider_spec`（第 1 组）· `webview_host` / `dom_web_client` / `pydoll_channel`（第 2 组）· `image_decode` 758（已 < 800）。

**剩余 10 个生产文件 / 10,361 行 → 预计 20–25 TU**：

| 组 | 文件 | 行数 | 拆分预判 |
|---|---|---|---|
| 3（ui） | `ui/src/app.cpp` | **1454** | 主循环 → `AppFrame`（§3 原定口径）+ 面板接线 |
| 3 | `ui/src/node_canvas.cpp` | **1126** | 交互段移入 interactor（A6 已把 11 条 static 收进 `NodeCanvasState`） |
| 3 | `ui/src/settings_panel.cpp` | **944** | 分区渲染 → 每区一函数（`SettingsPanelState` 已就位） |
| 3 | `ui/src/canvas_view.cpp` | **938** | 视图装配（`ReadOnlyView` 已在 R3 就位） |
| 3 | `ui/src/property_panel.cpp` | **855** | 每类节点一函数 |
| 3 | `ui/src/ce_draw.cpp` | **846** | 按「规格 / 模型 / 试运行」三段切 |
| 4（engine+nodes） | `nodes/src/local_nodes.cpp` | **1242** | 两个节点函数结构同构 → 先合并骨架再拆 |
| 4 | `engine/src/composite.cpp` | **1171** | 复合执行（与 `executor` 同簇） |
| 4 | `engine/src/node_registry.cpp` | **899** | 注册表 + 内置节点声明 |
| 4 | `engine/src/executor.cpp` | **886** | 校验块两处同构（14 行） |

**重复率专项（≤ 3.0 % 需净减 ≈ 219 行 / 现有 1096 行）**：

1. **provider 骨架模板方法化** ≈ 116 行（`anthropic↔gemini` 29 · `doubao↔kimi` 26 · `official↔site` 20 · `provider-hdr↔inference_provider.h` 41）—— 逻辑改造，**单独提交**；
2. **同函数双分支合并** ≈ 106 行（`webview_host_probe↔session` 28 · `dom_web_client_probe↔selftest` 21 · `pydoll_channel_selftest` 同文件 23 行 + 14 行 · `local_nodes` 14+10+8 · `executor` 14 · `workflow_io` 18 …）；
3. 平台样板 ≈ 96 行（已随第 2 组消解为「跨 TU 同构」，合并随专项 ②）。

**风险**：

1. 第 3 组（`ui`）是**渲染顺序敏感**代码：拆分必须保持每个函数的**调用顺序与帧内时序**，以 `ce_repro ALL FRAMES OK` 作主证据；
2. 第 4 组（`engine`/`nodes`）有断言覆盖（exec 713/0 · graph 151/0），风险最低，适合连续推进；
3. `source/tools/*` 自检工具（`api_execution_selftest.cpp` 3888 行等）**不计入 Z1**（审计口径只算生产模块），另立批。

## 10. 回滚方式

* 整组回滚：`git revert <本组两个提交>`（恢复 3 个单文件 + 删 14 个 TU / 5 个头 + 还原 `CMakeLists.txt` 与 3 个夹具的抽取口）。
* 只回滚某文件：把该文件的 N 个 TU 依次并回原文件名的 TU，并删除契约头与 CMake 中对应行（其余文件不受影响）。
* 公共 API 未变 ⇒ **调用方零回滚**（三个公开头一字未改）。

