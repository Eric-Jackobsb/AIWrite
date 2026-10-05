# M7 摘录归档：P7-b 的 **Pydoll 通道底座**（路线退场的部分）

> 📦 **摘录归档（2026-10-05）** —— 本件是 [../../actionPlan/M7.md](../../actionPlan/M7.md) 中**随路线 E 退场**的章节**原文照录**；`M7.md` 正本**原位保留**（另加 📦 指针，见其 §9 / §12 / §16 / §17）。
> **为何退场**：`M7B` 的「WebView2 退场 → 全部网页版走 Pydoll · Python 守护进程 + 命名管道」路线于 **2026-10-05** 被 **方案 E** 取代（文字生成**回退 WebView2** · 图片上传改走 **C++ native HTTP** · Python / 管道 / 守护进程**整体退场**）。
> **全量通道方案书** → [M7B.md](M7B.md)（同目录归档件）　·　**现行计划** → [../../actionPlan/M8.md](../../actionPlan/M8.md)
> **仍有效、未随本件退场**的条目：`P7b-12`（上传失败重试 UI）· `P7b-13`（`I18` 断言）· `P7b-14`（合规口径 · 待拍板）· `P7b-16`（解禁分派 → 转 `M8-21`）· 决策 `D7` / `D10` / `D11`。
> 归档原则：**只增不改** —— 摘录与 `M7.md` **逐字一致**，每块标注对应原文行号。

---

## 摘录 A：§9 决策清单 `D5` / `D6`（原文 = `M7.md` 第 **200–201** 行）

| `D5` | 网页版路线 | **方案 A：Pydoll + 独立浏览器**；不使用 WebView2 CDP 端点 | P7-b | 已定（配套 `Q2` / `Q3`） |
| `D6` | 进程架构 | **Python 守护进程 + 命名管道 + Win32 事件**；阻塞型事件驱动通信 | P7-b | 已定（配套 `Q4`） |

---

## 摘录 B：§12.1 前置技术验证（**未通过不进入实现**）· 含 B0/B1/B2/B3 实测结论（原文 = `M7.md` 第 **291–323** 行）

### 12.1 前置技术验证（**未通过不进入实现**）

> **外部事实核查（2026-09-29 · 只读核查，不改任何代码）**：
> ① **DeepSeek 官方 API 已可视觉** —— 2026-08-21 实验模型 → **2026-09-10 `deepseek-flash`（V4.1-Flash 原生多模态）**，Vision **✓**；`deepseek-v4-pro` Vision **✗** → 跟进项 `M7-10`，**与 P7-b 无关**（属 official 路线）。
> ② **DeepSeek 网页版（chat.deepseek.com）**：官方 changelog 最后一次点名 Web 是 **2026-08-13 的 V4-Pro GA（Vision ✗）**；V4.1-Flash 公告**只写「available on the DeepSeek API」**；且该站点是 SPA（未登录抓取无可判内容）→ **推断「网页版无真视觉理解」**（**推断，非结论**）。**是否有图片上传入口 = 待实测**。
> ③ **豆包网页版（doubao.com）**：公开资料**未声明**图片理解（第三方站只提「图像 / 视频生成」）→ **待 `P7b-05b` 实测**。**在实测出证据前，不得把任何网页版条目的 `capabilities.vision` 改成 `true`**（`I18`：无证据不宣称）。
> 详见 [网页版协议实测记录.md §8](../../网页版协议实测记录.md)。
> ④ **判定方法（写死，避免以后各说不一）**：上传入口按 4 类判定 —— `file_input`（有 `input[type=file]`，含 hidden）/ `drop_zone`（有拖拽区）/ `paste_only`（只有 contenteditable 粘贴）/ `none`；**视觉性质**用「**纯图无字**」判别 —— 能描述图形 / 颜色 / 构图 = **真视觉**；只复述文字或答「看不到图片」= **OCR / 非视觉**。

| 编号 | 任务 | 状态 | 落点 | 验收 |
|---|---|---|---|---|
| `P7b-01` | Pydoll 启动独立 Chrome / Edge、headful、用户手动登录 | ✅ **已过（随 `M7B-01` · 2026-09-28）**；🟡 **换机复证（2026-10-02 · 本机无 Chrome → Edge 154.0.4258.48 兜底）**：`--selftest`（headless + 临时 profile）**PASS 47 s** ⇒「Chrome / Edge 启动」这条在 **Edge** 上同样成立（`M7B-14` 兜底**首次实跑**）；**headful 登录窗口 + `~/.brain-ai/pydoll-profile` 仍待 B0**（需人工登录一次） | `source/python/` 试验脚本 | 窗口可见、可登录；profile 落在 `~/.brain-ai/pydoll-profile` |
| `P7b-02` | C++ 命名管道 + Win32 事件与 Python **双向**通信（= **方案 A** 的地基） | ✅ **已过（随 `M7B-02` · 2026-10-03）**：`\\.\pipe\aiwrite-browser-<pid>`（C++ = 客户端 / Python 守护进程 = 服务器）+ UTF-8 JSON 一行一帧；`aiwrite.exe --pipe-selftest` **PASS / exit 0**（`hello` → `ready` → `shutdown` → `stage{close}` → **等进程退出**）。**顺序闸门已由用户放行**（`P7b-05b` 侦察 → 审核 → `M7B` step 2/3 实施），本条不再属「未启动」 | C++ 试验 + Python 试验 | 命令下发 1 次 + 事件回传 1 次 PASS |
| `P7b-03` | Python asyncio 事件循环与管道监听线程共存 | ✅ **已过（随 `M7B-04` · 2026-10-03）**：管道伺服**独立线程**（实例先由主线程 `open()`）× asyncio 主循环**并发**跑「浏览器命令 burst」与 `asyncio.to_thread(pipe 往返)` ⇒ 往返落在命令窗口内**且返回时命令仍在跑** + 心跳 **12 次 / ≈0.66 s**（`--driver-selftest` **22 / 0**）；**生产形态（`daemon.py` 主循环）在 `M7B` §6.2 step 4 复验** | Python 侧 | 无死锁；Pydoll 操作与监听并行可用 |
| `P7b-04` | 行为模拟：Bezier 鼠标 + 随机打字延迟 | ✅ **已过（随 `M7B-05` · 2026-09-28）**：真实打字 **154 ms/字符** + 21 次 `input` 事件（`humanize`）；Bezier 鼠标轨迹仍留**人工观察** | Python 侧 | 人工观察轨迹非直线、节奏非机械 |
| `P7b-05` | 上传完成证据链：页面状态 + 网络回执**双证据** | ⬜ | Python 侧 | 双证据齐备才置「上传完成」（**证据形态由 `P7b-05b` 侦察确定**：附件节点数 / 进度态 / 网络回执特征） |
| `P7b-05b` | **上传入口只读侦察 + 视觉性质判别（方案 B′ · 先做）** | ✅ **B0 / B1 / B2 / B3 四段全部实跑**（2026-10-02 · 换机环境重建后）：**B0 PASS**（判据修正为「鉴权 Cookie 白名单 + DOM 正证据」并联；`close_wait` 优雅退出 → 重启复读仍登录）；**B1 = `file_input`**（登录态 · 大视口 `input.hidden`；⚠️ 2026-09-29 的 `paste_only` 是**小视口假象**，已作废）；**B2 PASS**（`DOM.setFileInputFiles` + 4 条 POST 回执链，`P7b-05` 双证据齐备）；**B3 = 真视觉**（256×256 纯红 → 答「均匀的正红色…无图案/文字/物体」；1×1 夹具会退化判错）。物证 `source/python/_probe/out/m7b28_doubao_recon.{b0-logged-in,b2,b3-1x1-white}-20261002.json` | **新增** `source/python/_probe/m7b28_doubao_recon.py`（一次性探针，**不进主管道**；站点 = `doubao-web`；`--inject` / `--send` 默认关） | ① 上传入口落 `file_input`/`drop_zone`/`paste_only`/`none` → **定 `P7b-10` 主路线 = `setFileInputFiles` / `expect_file_chooser`**；② `input_selector`/`send` 命中数真实（供回填 `providers.json`）；③ **登录前后 Cookie 名差集**（喂 `D-30`：14 个鉴权名）；④ **纯图无字判别 = 真视觉**；⑤ 干净退出 + 重启**仍登录** ✅ |

> **B1 段实测结论（2026-09-29 20:33 · 物证 `source/python/_probe/out/m7b28_doubao_recon.json`）**：
> ① **上传入口形态 = `paste_only`** —— `file_inputs: []`、`drop_zones: []`（既无 `input[type=file]` 也无拖拽区）⇒ **`P7b-10` 的主路线 `expect_file_chooser` 在豆包上不可用**，豆包**已判定走 `DataTransfer` 构造 File + 合成 `paste`**（「仅当站点无 `input[type=file]` 时才退」不再是待定项 —— 豆包就是这个例外）。—— ⚠️ **本条已于 2026-10-02 被推翻**（**小视口假象**）：登录态 + **大视口（1440×1000）**实测入口 = **`file_input`**（`input.hidden` = 隐藏 file input）⇒ 主路线回到 **`setFileInputFiles` / `expect_file_chooser`**，`DataTransfer` **降为备选**（见下方复测块与 `P7b-10` 行）。
> ② **composer 候选 = `div.tiptap.ProseMirror`**（`contenteditable: true`、`visible: true`、命中 **1**；tiptap/ProseMirror 族，与 `yuanbao-web` 同族）→ 可作 `input_selector` 候选；**未登录态即渲染**（与「登录型条目未登录不渲染输入框」的旧结论不同，该结论对豆包按此处更正）。
> ③ **未登录已有 10 条匿名 Cookie**（`hook_slardar_session_id` / `i18next` / `dbx-web-theme` / `conversation_list_v2_group_mode` / `flow_cur_user_sec_id` / `flow_user_country` / `s_v_web_id` / `passport_csrf_token` / `passport_csrf_token_default` / `biz_trace_id`；**仅名字，值不落盘**）→ 再次印证决策 `D-30`「Cookie 非空 = 已登录」必然误报；**登录前后 Cookie 名差集仍待 B0 段**（登录前快照本次已取）。
> ④ **`answer_selector` 四类候选全 0**（`[class*="message"]` / `[class*="answer"]` / `[class*="reply"]` / `[class*="markdown"]`）⇒ 必须「登录 + 手动发一条」才存在 —— 与 [网页版协议实测记录.md](../../网页版协议实测记录.md) §7.9 结论一致，属 `M7B-06` / `M7B-28` 的同一人工关卡。
> ⑤ **探针纪律执行到位**：`readback_self_proof = "probe-ok"`（读回先自证）、`close_wait` 退出码 **0**、`strays_after: []`（无残留实例）—— 口径对齐 `M7B.md` §5。
> ~~**未跑**：B0（登录 Cookie 差集 / 重启仍登录）、B2（注入 1 张图 + CDP 网络回执）、B3（纯图无字视觉性质判别）~~ ⇒ ⚠️ **四段已于 2026-10-02 全部跑完**（换机环境重建后）。

> **2026-10-02 复测结论（B0 / B1 / B2 / B3 · 物证 `source/python/_probe/out/m7b28_doubao_recon.{b0-logged-in,b2,b3-1x1-white}-20261002.json`）**：
> ① **B0 PASS**：登录判据修正为「**鉴权 Cookie 白名单**（14 名）+ **DOM 正证据**」**并联** —— 原「Cookie 差集非空」判据在**无人操作**的 9 s 内**误报**过（差集被 Edge 自家 `msn.cn` Cookie 与匿名态 `flow_*` 污染，物证 `…b0-false-positive-20261002.json`）；复证：起点已登录 → 6 s 判定 → `close_wait` 优雅退出（残留 0）→ **重启复读仍登录** ✅。
> ② **B1 更正 = `file_input`**（登录态 · 大视口 `input.hidden`）⇒ ① 条作废；**视口尺寸是硬前提**（859×450 下 `<input>` 总数 **0**、composer 退化成 `textarea`；1440×1000 下才有隐藏 input 且 composer 为 `div.tiptap.ProseMirror`）。
> ③ **B2 PASS（`P7b-05` 双证据齐备）**：`DOM.setFileInputFiles` 直设 —— 文件真进 `input.files`；网络回执（**收紧为 POST / postData 后仍 PASS**）= `prepare_upload` → **TOS `upload/v1/…png`** → `CommitImageUpload`。
> ④ **B3 = 真视觉**（256×256 纯红方块 → 答「一整块均匀、饱和度很高的**正红色**…没有任何图案、文字、物体」）；⚠️ 同一夹具 **1×1** 尺寸会被判「完全纯白色」= **退化输入**（夹具已换 256×256）。
> ⑤ **`D-30` 证据**：匿名态站点域 8–10 条 → 登录态 33–35 条，净新增 **23 名**，其中 **14 个鉴权名**入 `cookie_names` 优先名单（`M7B.md` §10）。
> ⑥ **`capabilities.vision` 仍保持 `false`**：真视觉证据**已到手**，但「能力声明」属**产品资产**且牵连 `P7b-16` 三处落点 + `D11②` 显式确认口径 ⇒ **待拍板后同批落**（本轮不擅自改，`I18` 也不无证据地宣称）。

---

## 摘录 C：§12.2 实现表中的**通道类**条目（原文 = `M7.md` 第 **325–334** 行）

### 12.2 实现

| 编号 | 任务 | 状态 | 落点 | 验收 |
|---|---|---|---|---|
| `P7b-06` | Python 守护进程框架：自启动、后台、心跳 | 🟡 **部分（2026-10-03 · `M7B-11`）**：**主循环 / 心跳 / 崩溃检测 / L4 留痕**已落地（`daemon.py`：心跳每 10 s、进程消失或连接类异常**被吞 + 自愈**、`browser.log` + `daemon-state.json`；`--daemon-selftest` **22 / 0**）；**自启动 / 后台常驻 / 「上次异常退出」UI 提示**归 step 6（需 C++ 侧接线）；**L2 登录态加密快照已落地（step 5 · `session.py` + `VB2-39`：三触发点 + 启动回灌）** | `source/python/`、C++ 启动逻辑 | 进程存活；心跳可检测；登录态跨重启可回灌 |
| `P7b-07` | 命名管道协议（C++→Python 命令 / Python→C++ 事件，JSON 行） | ✅ **已过（随 `M7B-12` + `M7B-02` · 2026-10-03）**：词表 = [M7B.md](M7B.md) §6.1（帧头 `v` / `id` / `kind` + 7 命令 + 6 事件 + 错误码 → `I21`）；双侧同构实现（`source/python/brain_ai_browser/protocol.py` ↔ `src/web/channel_frames.*`）+ 离线断言 **`VB2-29①~⑦`**（`--exec-selftest` **320 / 0**）；非法行（JSON 坏 / 缺 `v`/`kind` / `v` 不识别 / 超上限）→ **丢弃 + 记日志 + 回 `err{bad_frame}`** 且**不退出**（`--pipe-selftest` loopback **13 / 0**） | 双侧 | 协议可解析；非法行被拒绝并记录 |
| `P7b-08` | 事件句柄传递与阻塞等待（**在工作线程**，不得阻塞 UI 线程） | ⬜ | C++ 侧 | `WaitForSingleObject` 正确唤醒；UI 不卡 |
| `P7b-09` | 浏览器生命周期：启动 / 关闭 / 崩溃检测 | ✅ **已过（随 `M7B-11` · 2026-10-03）**：**启动 = 按需**（`open_tab` 才起；`hello` / `shutdown` 不起）+ **关闭 = `Browser.close` → 等进程退出**（默认 5 s，超时才兜底强杀且**必留 warn**）+ **崩溃检测** = 浏览器进程消失 / 连接类异常**被吞 + 自愈重启**（实测：强杀后命令仍成功、pid 换新、**stderr 零回溯**）；「管道断开即感知」= 监听线程 EOF → 记 L4 + 结束该连接（下一连接可复用浏览器） | 双侧 | 管道断开即感知，上报可操作错误 |
| `P7b-10` | 文件注入：**主路线 = `expect_file_chooser`**（`M7B-05` 已实测两条注入路径均成功）；**仅当站点无 `input[type=file]` 时**退 `DataTransfer` 构造 File + 合成 `paste`/`drop` + 行为模拟 | ⬜ | Python 侧 | 站点附件区出现文件（**路线由 `P7b-05b` 判定，不预设**）；⚠️ **已判定（2026-10-02）**：豆包 = **`file_input`**（登录态 · **大视口** `input.hidden`）⇒ 走 **`DOM.setFileInputFiles` / `expect_file_chooser`**，**`DataTransfer` 降为备选**（2026-09-29 的 `paste_only` 系**小视口假象**，已作废）；**启动浏览器必须显式设窗口尺寸** |
| `P7b-11` | 上传完成等待：双证据 + 超时 | ⬜ | Python 侧 | 超时给可操作错误（不假装完成） |

> （此处略去 §12.2 中**仍然有效**的三条：`P7b-12` / `P7b-13` / `P7b-14`，及其后的 `P7b-16` —— 它们不随路线退场，正本见 `M7.md`。）

（原文 = `M7.md` 第 **338** 行）

| `P7b-15` | Python 运行时检测与打包策略 | 🟡 **部分（2026-10-03 · `M7B-13`）**：**运行时检测已落地**（`runtime.check_runtime()` + 可操作引导 + `VB2-30` 桩/真机双证据；`hello` 在缺失时**追加 `error` 事件**让调用方看得见）；**打包策略（内嵌 Python · M6 硬门槛）**与「无 Python 时」的 C++ 侧引导未开工 | 打包脚本、`PM-03` | 无 Python / 无 Chrome 时给引导；不阻塞其他功能（关系见 `Q7`） |

---

## 摘录 D：§16 实施顺序（**旧链条** · 含方案 A = `M7B-02`/`M7B-04` 管道本体）（原文 = `M7.md` 第 **407–432** 行）

```
P7-a（三条线可并行）：
  ① P7a-01~03 端口 variadic + 多图（引擎侧，先写断言再改代码）
  ② P7a-04~08 统一资源目录 + 编码缓存（资源侧）
  ③ P7a-12~19 UI A 档（UI 侧）
        ↓ 跑 §14 基线（贴数字）
P7-b 前置验证 · 第 0 段（方案 B′ · 2026-09-29）：P7b-05b 只读侦察（豆包 doubao-web，零产品代码）
  ├─ B1 ✅ 已实测（2026-09-29）：入口 = paste_only（无 file input / 无拖拽区）⇒ 注入路线 = DataTransfer
  │        ⚠️ **已更正（2026-10-02）**：paste_only 是**小视口（859×450）假象**；登录态 + **大视口（1440×1000）** = `file_input`（隐藏 `input.hidden`）⇒ 路线 = `setFileInputFiles` / `expect_file_chooser`
  ├─ B0 ✅ 已实测（2026-10-02 · 67 s PASS）：判据修正为「鉴权 Cookie 白名单（14 名）+ DOM 正证据」并联；clean 退出 → 重启复读仍登录；`D-30` 差集 = 净新增 23 名
  ├─ B2 ✅ 已实测（2026-10-02 PASS）：注入 1 张图（不发送）→ **双证据齐备**（`input.files` + 4 条 POST：`prepare_upload` → TOS `upload/v1/…png` → `CommitImageUpload`）
  └─ B3 ✅ 已实测（2026-10-02）= **真视觉**：256×256 纯红方块 → 答「一整块均匀、饱和度很高的正红色…无图案/文字/物体」（⚠️ 1×1 夹具会退化成「纯白色」—— 负/灰结果处置：**先修实验材料**，不是换站）
        ↓ 结论经用户审核通过后，才启动
方案 A = M7B-02 / M7B-04（C++↔Python 命名管道本体；与站点无关 —— 是全部网页版链路的地基）
        协议 v1 词表 = M7B.md §6.1（批 1 开工前冻结）
        ↓ 接通后再回到
P7-b 前置验证：P7b-01~04 已过 / 在跑（随 M7B-01/03/05/08/09）；P7b-05 与 P7b-10 依赖方案 A
        → 未通过 = 回滚，不进入实现
        ↓ 通过后（两条线在此汇合）
  ├─ 线 1 站点选择器回填：M7B-24（多候选）→ M7B-28 的 doubao-web 行（send / answer_selector，人工登录一次）
  └─ 线 2 上传链路实现：P7b-06~15（管道 / 注入 / 证据 / 重试 / I18）+ P7b-16（解禁分派）
        ↓ 两条线都齐
P7-b 实现：E-02 端到端（豆包）→ 跑 §14 基线（贴数字）
        ↓
评审 → 按用户指示决定是否提交（当前变更集未提交）
```

---

## 摘录 E：§17 开口项 `Q2` / `Q3`（**已作废 · 随 `M7B` 一并退场**）（原文 = `M7.md` 第 **441–442** 行）

| `Q2` | Pydoll 引入后**既有 `adapter=dom`（WebView2 选择器驱动）怎么办** | ① 一律改走 Pydoll ② 一律 WebView2、Pydoll 仅实验 ③ 新增 `web.engine: webview2\|pydoll` | ⚠️ **已作废（2026-09-28）** —— 改由 [M7B.md](M7B.md) `MB-D0-3` 决定「**全部网页版条目一律走 Pydoll**」（WebView2 退场，不再有 `web.engine` 字段）；原建议默认「③ 默认 `webview2`」不再执行 |
| `Q3` | 两套浏览器 = **两套登录态**（WebView2 profile vs Pydoll profile） | ① 只维护一套 ② 两套并存、分别提示登录 ③ 把 Pydoll 的 Cookie 回灌 WebView2 | ⚠️ **已作废（2026-09-28）** —— 改由 [M7B.md](M7B.md) `MB-D0-5` 决定「**只维护一套登录态**」（单 profile `~/.brain-ai/pydoll-profile`；旧 `webview2` 登录态无法迁移） |
