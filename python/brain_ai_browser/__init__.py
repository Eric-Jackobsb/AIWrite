"""AIwrite · 浏览器守护进程包（M7B 批 1 · 网页通道唯一引擎 = Pydoll）。

* 通道：C++（`web/pipe_client`）↔ 本守护进程，命名管道 + Win32 事件，UTF-8 JSON 一行一帧；
* 词表：`protocol.py`（= `docs/actionPlan/M7B.md` §6.1，**唯一实现**）；
* 纪律：`I21` 不静默降级 · `I23` 退出/持久化 · 依赖只用标准库 + `pydoll`。

批 1 阶段（本包当前状态）：
    * step 1 ✅ `protocol.py`（§6.1 词表唯一实现）；
    * step 2 ✅ `pipe.py`（命名管道服务端 = 本守护进程；`Shell` 侧 `src/web/pipe_client.*`）；
    * step 3 ✅ `browsers.py`（浏览器探测 · `M7B-14`）/ `runtime.py`（运行时检测 · `M7B-13`）/
      `driver.py`（Pydoll 最小集：`start` / `tab_for` / `cookies_all` / `execute_script` / `close_wait`）；
    * step 4 ✅ `daemon.py`（**唯一主循环**：管道监听线程 × asyncio 事件循环；命令分发 / 按需启动 /
      **`BrowserNotRunning` 自愈** / 关闭协议 `close_verdict` · `VB2-38`）+ `redact.py`（日志脱敏，唯一写日志入口）；
      `--serve` 已从「最小伺服」迁入 `daemon.serve()`；
      ⬜ `upload_image` / `send_prompt` / `read_answer` 仍未实现（批 3 / step 5；`MB-Q7`）。
    * ⬜ 未落地：`session.py`（L2 快照 · `VB2-39`）；C++ 侧接线与诊断换代归 step 5~6。
"""

PROTO_VERSION = 1
PACKAGE_VERSION = "0.1.0"

__all__ = ["PROTO_VERSION", "PACKAGE_VERSION"]
