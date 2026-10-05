"""AIwrite · 浏览器守护进程包（M7B 批 1 · 网页通道唯一引擎 = Pydoll）。

* 通道：C++（`web/pipe_client`）↔ 本守护进程，命名管道 + Win32 事件，UTF-8 JSON 一行一帧；
* 词表：`protocol.py`（= `docs/actionPlan/M7B.md` §6.1，**唯一实现**）；
* 纪律：`I21` 不静默降级 · `I23` 退出/持久化 · 依赖只用标准库 + `pydoll`。

目录（2026-10-03 迁移）：本包 = `source/python/brain_ai_browser/`（**运行代码归源码树**，含同级 `_probe/` 探针与 `.venv/`）；
运行期数据一律落 `~/.brain-ai`（`logs` / `pydoll-profile` / L2 快照 `session/`）—— **代码入库、数据不入库**。
命令口径：`cd source/python` → `.venv/Scripts/python.exe -m brain_ai_browser …`。

批 1 阶段（本包当前状态）：
    * step 1 ✅ `protocol.py`（§6.1 词表唯一实现）；
    * step 2 ✅ `pipe.py`（命名管道服务端 = 本守护进程；`Shell` 侧 `src/web/pipe_client.*`）；
    * step 3 ✅ `browsers.py`（浏览器探测 · `M7B-14`）/ `runtime.py`（运行时检测 · `M7B-13`）/
      `driver.py`（Pydoll 最小集：`start` / `tab_for` / `cookies_all` / `execute_script` / `close_wait`）；
    * step 4 ✅ `daemon.py`（**唯一主循环**：管道监听线程 × asyncio 事件循环；命令分发 / 按需启动 /
      **`BrowserNotRunning` 自愈** / 关闭协议 `close_verdict` · `VB2-38`）+ `redact.py`（日志脱敏，唯一写日志入口）；
      `--serve` 已从「最小伺服」迁入 `daemon.serve()`；
    * step 5 ✅ `session.py`（**L2 DPAPI 加密快照** · `VB2-39`）：数据落 `~/.brain-ai/session/cookies.dat`
      （代码入库 / 数据不入库）；**三触发点**（定时 5 s / 登录即写 / `shutdown` **关闭前**再写）
      + 启动 / 自愈后**回灌**（失败发 `error{not_logged_in}` 显式提示 · `I23④`）；`--session-selftest` 覆盖。
    * step 6 ✅ C++ 侧接线（`web/pydoll_channel` / `web/session_snapshot` / `web/site_ref` + `PipeClient` 判据同步）；
    * step 7 ✅ `M7B-19` 全基线（`M7B-18` 诊断换代**受阻已登记**：词表当时无「执行 JS」命令）；
    * **step 8 ✅ 词表 v2 + `run_script` 全链路**（诊断换代的地基：`--web-dom-dump` / `--web-adapter-selftest` 可走新通道）；
    * step 9 ✅ `M7B-18` 诊断换代（两个诊断工具走新通道）；step 10 ✅ `M7B-20` 生产路径切换（`dom_chat` 走新通道）；
    * **step 11 ✅ 词表 v3 + 会话族落地**（`logout_site` / `current_tab` / 事件 `tab` / 错误码 `not_implemented`
      —— 闭合开口项 `MB-Q7`；配套 C++ `pydoll_channel` 的 `login_site` / `logout_site` / `tab_on_site` 换代）；
    * ⬜ 批 3 余项（`M7B-21`~`M7B-25`）：CDP 增量（`stream_deltas`）· 网络回执双证据（`P7b-11`）·
      C++ 侧 `channel::{send_prompt,read_answer,upload_image}` 接线（step 14 的 P3/P4）。
    * **step 14 ✅ 词表 v4 + 内容返回落地**（`M7B-54`/`M7B-55`）：`upload_image`
      （`DOM.setFileInputFiles` + **证据记账**）/ `send_prompt`（**真打字**注入 + 按 `send` 触发）/
      `read_answer`（轮询 + **显式截断**）；`run_script` **降级为诊断专用**。
"""
PROTO_VERSION = 4


PACKAGE_VERSION = "0.1.0"

__all__ = ["PROTO_VERSION", "PACKAGE_VERSION"]
