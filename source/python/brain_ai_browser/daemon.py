"""守护进程主循环（`M7B-11` 收口 · `M7B.md` §6.2 step 4 · `VB2-38`）。

结构（线程 × 事件循环，`M7B-04` 的生产形态）
    * **主线程 = asyncio 事件循环**：所有浏览器操作（Pydoll）在这里跑；
    * **管道监听线程**：`PipeServer.accept` + `read_line`（阻塞式），把**合法帧**投进队列
      （`call_soon_threadsafe`）⇒ 读管道**不占用**事件循环；
    * **写帧**一律 `asyncio.to_thread`（阻塞式 `WriteFile` 不卡循环）；
    * 浏览器**按需启动**（`hello` / `shutdown` 不需要浏览器 ⇒ `aiwrite.exe --pipe-selftest` 依然秒级）。

行为口径
    * `hello` → `ready{proto, python, browser}`（+`pid`，`P1`）；**依赖缺失**时**追加一条
      `error` 事件帧**（`id="-"`）带**可操作引导**（`M7B-13`/`M7B-14` 的「缺失 → 引导」，`I21`）；
    * 浏览器命令在依赖缺失 / 无浏览器时 → `err{no_python|no_browser}` + 引导，
      **不静默降级、不换通道**（`I21`）；
    * **用户手动关窗 = 可恢复状态**（`M7B-11`）：`BrowserNotRunning` 被**吞掉** → 记 L4 + **自愈重启**
      一次（日志留痕、`session_starts` 递增），不把异常抛给调用方；
    * `shutdown` → `stage{close}` → `Browser.close` → **等进程退出**（默认 5 s，`MB-Q5`）
      → **超时才兜底强杀，且必须留 warn**（`VB2-38` / `I23①`）；判定走 `close_verdict()` 纯函数；
    * 词表内**尚未实现**的命令（`upload_image` / `send_prompt` / `read_answer`）→ 如实回
      `err{daemon_down, hint=尚未实现}` 并**保持存活**（词表缺「未实现」码 = 开口项 **`MB-Q7`**）；
    * **L2 登录态快照**（`MB-D0-8` L2 · step 5 · `VB2-39`）：`open_tab` / `login_state` 起浏览器后
      **回灌一次**（每浏览器会话一次；失败 → 追加 `error{not_logged_in}` 事件**显式提示**，`I23④`，
      但**不影响本次命令回包**）；`login_state` 判到 `logged_in` → **登录即写**；
      `shutdown` 在 `Browser.close` **之前**再写一次；空闲 / 命令间隙每 5 s **定时刷新**
      （`_snapshot_tick` —— 与命令跑在**同一任务**里 ⇒ 不存在并发访问 CDP 的时序问题）；
    * ⚠️ **`login_state` 的回包名是 `login`** —— 该事件名**不在** §6.1 表 2 的事件词表里（当步迭代仅校验
      命令词表）；已登记为开口项 **`MB-Q9`**，本批**不动词表**，按 §6.2 命令边界表实现。
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import sys
import threading
import time
from typing import Any, Awaitable, Callable, Dict, Optional, Tuple

from . import PACKAGE_VERSION, PROTO_VERSION
from . import browsers
from . import driver
from . import pipe
from . import protocol as P
from . import redact
from . import runtime
from . import session

__all__ = ["DEFAULT_IDLE_TIMEOUT_S", "ready_payload", "dependency_problem",
           "close_verdict", "BrowserAccess", "Daemon", "serve",
           "serve_offline_connection", "handle_frame"]

DEFAULT_IDLE_TIMEOUT_S = 30.0     # 连接空闲 / 等待连接的超时（不留僵尸）
HEARTBEAT_INTERVAL_S = 10.0       # L4 心跳埋点间隔（`M7B-11`：心跳可检测）

WriteLine = Callable[[str], Awaitable[None]]


# ============================================================================
#  ready 载荷与依赖判定（`M7B-13` / `M7B-14`）
# ============================================================================

def ready_payload() -> Dict[str, Any]:
    """`ready` 事件载荷（字段与 step 2 一致，**不新增词表字段**）。"""
    kind, exe = browsers.detect_browser()
    return {
        "proto": PROTO_VERSION,
        "python": "%d.%d.%d" % sys.version_info[:3],
        "browser": ("%s@%s" % (kind, exe)) if exe else "none",
        "pid": os.getpid(),
    }


def dependency_problem() -> Optional[Tuple[str, str]]:
    """依赖缺失 → `(错误码, 可操作引导)`；齐全 → `None`。"""
    if runtime.load_pydoll() is None:
        return "no_python", runtime.PYDOLL_HINT
    if browsers.detect_browser()[1] is None:
        return "no_browser", runtime.NO_BROWSER_HINT
    return None


# ============================================================================
#  `VB2-38`：关闭协议判定（**纯逻辑**，不启浏览器）
# ============================================================================

def close_verdict(evidence: Dict[str, Any]) -> Tuple[bool, str]:
    """`I23①` / `MB-D0-8` L1 的关闭协议判定。

    证据字段（`BrowserAccess.close()` 产出）：
      `close_requested`（是否走过 `Browser.close`）· `browser_not_running`（`BrowserNotRunning`）
      · `waited_s`（等进程退出耗时，`<0` = 超时）· `exit_code` · `warning` · `fallback_kill`

    判据
      ① 缺 `Browser.close` ⇒ **失败**（「等进程退出」这条不成立）；
      ② `browser_not_running` ⇒ **通过**（用户手动关窗 = 可恢复状态，归正常收尾）；
      ③ 期限内退出 ⇒ 通过（且此时**不得**出现兜底强杀 —— 证据自相矛盾即失败）；
      ④ 超时却**没有 warn** ⇒ **失败**（`I23①`：异常必须可见）；
      ⑤ 超时 + warn（无论是否兜底强杀）⇒ 通过（强杀必须留痕，登录态可能回滚需如实标注）。
    """
    if evidence.get("browser_not_running"):
        return True, "浏览器本已不在运行（BrowserNotRunning 被吞）→ 归正常收尾"
    if not evidence.get("close_requested"):
        return False, "关闭路径未调用 Browser.close（缺「请浏览器自己关」这一步，I23①）"
    waited = evidence.get("waited_s")
    if isinstance(waited, (int, float)) and waited >= 0:
        if evidence.get("fallback_kill"):
            return False, "进程已退出却仍记录兜底强杀（证据自相矛盾：强杀只在超时后允许）"
        return True, "Browser.close → 等进程退出 %.2fs（未强杀）" % waited
    if not evidence.get("warning"):
        return False, "未在期限内退出却**没有 warn**（I23①：异常必须可见）"
    if not evidence.get("fallback_kill"):
        return True, "未在期限内退出：已留 warn，**未强杀**（可恢复状态）"
    return True, "未在期限内退出：已留 warn + 兜底强杀（登录态可能回滚，已如实标注）"


# ============================================================================
#  浏览器会话（按需启动 + 自愈 + 关闭协议）
# ============================================================================

class BrowserAccess:
    """一次守护进程生命周期内的浏览器会话。

    * **按需启动**：`ensure()` 才真起浏览器（`hello` / `shutdown` 不需要它）；
    * **自愈**：`BrowserNotRunning`（用户手动关窗 / 崩溃）→ 吞掉 + 记 L4 + 重启一次（`M7B-11`）；
    * **关闭**：`close()` 产出 `close_verdict()` 需要的证据（超时才兜底强杀 + warn，`MB-Q5`）。
    """

    def __init__(self, *, headless: bool = False,
                 start_timeout: float = driver.DEFAULT_START_TIMEOUT_S,
                 snapshot: Optional["session.SnapshotStore"] = None) -> None:
        self.headless = bool(headless)
        self.start_timeout = float(start_timeout)
        self.session_starts = 0     # 启动次数（自愈 = 第 2 次起）
        self.self_heals = 0         # 吞掉「浏览器已死」并重启的次数
        self.snapshot = snapshot    # L2 快照（`None` = 关闭该功能：不读也不写）
        self.snapshot_saves = 0     # L2 实际写盘次数（内容有变化才算）
        self.snapshot_restores = 0  # L2 成功回灌次数（每个浏览器会话一次）
        self._restored_at = 0       # 回灌时的 `session_starts`（幂等记账）
        self._driver: Optional[driver.BrowserDriver] = None

    # ---- 状态 ----
    @property
    def driver(self) -> Optional[driver.BrowserDriver]:
        return self._driver

    @property
    def alive(self) -> bool:
        return self._driver is not None and self._driver.alive()

    def browser_pid(self) -> Optional[int]:
        return self._driver.pid() if self._driver is not None else None

    # ---- 启动 / 自愈 ----
    async def ensure(self) -> driver.BrowserDriver:
        """确保有**存活**的浏览器；没有（或已死）→ 起一个（依赖缺失原样抛 `DependencyError`）。

        「上一会话已死」（用户关窗 / 崩溃 / 被强杀）⇒ **自愈重启 + L4 留痕**（`M7B-11`：
        用户手动关窗属**可恢复状态**，不得报错、不得静默）。
        """
        if self.alive:
            return self._driver  # type: ignore[return-value]
        if self._driver is not None:
            self.self_heals += 1
            redact.log_event("browser_selfheal", "浏览器已不在运行 → 自愈重启", level="warn",
                             reason="process_gone", previous_pid=self._driver.pid())
        return await self._start()

    async def _start(self) -> driver.BrowserDriver:
        """真启动（自愈记账由调用方做，避免重复计数）。"""
        self._driver = await driver.start(headless=self.headless, start_timeout=self.start_timeout)
        self.session_starts += 1
        redact.log_event("browser_start", "浏览器已启动", session_starts=self.session_starts,
                         browser=self._driver.kind, browser_pid=self._driver.pid(),
                         profile=str(self._driver.profile), headless=self.headless)
        return self._driver

    async def restart_after_gone(self, reason: str) -> driver.BrowserDriver:
        """命令执行期间发现「浏览器已死」（连接类异常）→ 吞掉 + 记 L4 + 重启一次。"""
        self.self_heals += 1
        redact.log_event("browser_selfheal", "命令执行期间发现浏览器已退出 → 重启后重试",
                         level="warn", reason=reason,
                         previous_pid=self._driver.pid() if self._driver else None)
        self._driver = None
        return await self._start()

    def note_command_error(self, code: str, hint: str) -> None:
        redact.log_event("command_error", "命令失败（可操作错误已回给调用方）",
                         level="warn", code=code, hint=hint)

    # ---- L2 快照（`MB-D0-8` L2 · step 5 · `VB2-39`）----
    async def save_snapshot(self, *, reason: str, provider: str = "") -> Dict[str, Any]:
        """读**全库** Cookie（浏览器级）→ 作用域过滤 → DPAPI 加密落盘；失败**只记日志**。

        三个触发点口径一致：**定时刷新**（`refresh`）/ **登录即写**（`login`）/
        **`shutdown` 关闭前再写**（`shutdown`）——「关闭前」必须在 `Browser.close` **之前**，
        否则 CDP 已断、读不到 Cookie。
        """
        store = self.snapshot
        if store is None:
            return {"ok": False, "reason": "disabled"}
        if self._driver is None or not self._driver.alive():
            return {"ok": False, "reason": "no_session"}
        try:
            cookies = await self._driver.cookies_all()
        except Exception as exc:  # noqa: BLE001 —— 读 Cookie 失败**不得**阻断命令 / 收尾
            redact.log_event("snapshot_read_failed", "L2 快照读取 Cookie 失败：%s" % exc,
                             level="warn", reason=reason)
            return {"ok": False, "reason": "read_failed"}
        result = store.save(cookies, reason=reason, provider=provider)
        if result.get("changed"):
            self.snapshot_saves += 1
        return result

    async def restore_snapshot(self) -> Optional[Tuple[int, str]]:
        """把 L2 快照**回灌**进当前浏览器会话；返回 `(写入条数, reason)` 或 `None`。

        * **每个浏览器会话只回灌一次**（自愈重启后会再灌 —— 新进程的 Cookie 库是空的）；
        * **不抛异常**：失败原因如实交给调用方，由它**显式提示**（`I23④`）；
        * `reason="absent"` = 首次运行（**正常**，不是故障）；`None` = 本次无需处理（已灌过 / 功能关闭）。
        """
        if self.snapshot is None or self._driver is None:
            return None
        if self._restored_at >= self.session_starts:
            return None                                   # 本会话已回灌（幂等）
        self._restored_at = self.session_starts
        loaded = self.snapshot.load()
        if not loaded["ok"]:
            return 0, str(loaded.get("reason") or "unknown")
        params = session.to_cdp_params(loaded["cookies"])
        try:
            written = await self._driver.set_cookies(params)
        except Exception as exc:  # noqa: BLE001 —— 回灌失败不能阻断启动
            redact.log_event("snapshot_restore_failed", "L2 快照回灌失败：%s" % exc,
                             level="warn", count=len(params))
            return 0, "set_cookies_failed"
        self.snapshot_restores += 1
        redact.log_event("snapshot_restored", "L2 快照已回灌（值不入日志）",
                         restored=written, **session.summarize(loaded["payload"]))
        return written, "restored"

    # ---- 关闭（`VB2-38`）----
    async def close(self, grace_ms: int = P.SHUTDOWN_GRACE_MS) -> Dict[str, Any]:
        """`Browser.close` → 等退出 → 超时兜底强杀（留 warn）；返回 `close_verdict()` 的证据。"""
        if self._driver is None:
            return {"close_requested": False, "browser_not_running": True, "waited_s": None,
                    "exit_code": None, "warning": "", "fallback_kill": False,
                    "note": "本次会话未启动浏览器 → 无需关闭"}
        info = await self._driver.close_wait(max(0.1, grace_ms / 1000.0))
        evidence: Dict[str, Any] = {
            "close_requested": True,
            "browser_not_running": info.get("close_reply_error") == "BrowserNotRunning",
            "waited_s": info.get("waited_s"),
            "exit_code": info.get("exit_code"),
            "warning": info.get("warning", ""),
            "fallback_kill": False,
        }
        waited = evidence["waited_s"]
        if isinstance(waited, (int, float)) and waited < 0:      # L1 超时 → 兜底强杀（必须留 warn）
            evidence["fallback_kill"] = self._force_kill()
            prefix = (evidence["warning"] + "；") if evidence["warning"] else ""
            evidence["warning"] = prefix + ("已兜底强杀（MB-Q5）—— 登录态可能未落盘，下次启动需探端口"
                                            if evidence["fallback_kill"] else
                                            "强杀未成功（进程可能已自行退出）")
        ok, reason = close_verdict(evidence)
        redact.log_event("shutdown", "关闭协议：%s" % ("通过" if ok else "**失败**"),
                         level="info" if ok else "error", verdict_ok=ok, verdict_reason=reason,
                         **{key: value for key, value in evidence.items() if key != "warning"})
        self._driver = None
        return evidence

    def _force_kill(self) -> bool:
        """**兜底强杀**（仅在 L1 超时后；探针 `kill_strays()` 的生产对应物，必须留 warn）。"""
        process = self._driver.process() if self._driver is not None else None
        if process is None or process.poll() is not None:
            return False
        try:
            process.kill()
        except Exception:  # noqa: BLE001 —— 强杀失败也算「如实回报」
            return False
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.1)
        return process.poll() is not None


# ============================================================================
#  帧处理（**生产与自检共用同一条路径**）
# ============================================================================

OFFLINE_HINT = ("本次伺服未接浏览器会话（离线 / 自检模式）：请由守护进程主循环处理浏览器命令"
                "（`M7B.md` §6.2 step 4）")
MISSING_URL_HINT = ("缺少 url：条目 `web.login_url` 由 C++ 侧解析后下发"
                    "（`I14`：站点只来自条目，Python 侧无回落）")
NOT_IMPLEMENTED_HINT = ("命令 %s 尚未实现（批 3 / step 5 落地）；词表暂缺「命令未实现」错误码 →"
                        " 现借 `daemon_down`（开口项 `MB-Q7`）")

# `I23④`：快照存在却恢复不了 → **必须显式提示**（不得静默降级成「没登录过」）
RESTORE_FAILED_HINT = ("登录态快照无法恢复（%s）→ 请重新登录一次；"
                       "下次干净退出后会重新写入快照")

_GONE_EXCEPTIONS: Optional[Tuple[type, ...]] = None


def gone_exceptions() -> Tuple[type, ...]:
    """「浏览器已不在运行 / 连接已断」类异常（惰性取自 pydoll；取不到 → 只剩内建那条）。

    ⚠️ **必含内建 `ConnectionError`**（`ConnectionRefusedError` / `ConnectionResetError` /
    `ConnectionAbortedError`）：浏览器被杀后，下一次 CDP 命令可能先撞到「连接被拒绝」而不是
    pydoll 的 `BrowserNotRunning`（2026-10-03 step 5 实测：`[WinError 1225] 远程计算机拒绝网络连接`）
    ⇒ 漏掉这条就会**丢失自愈**、命令直接失败（`M7B-11` 行为被破坏）。
    """
    global _GONE_EXCEPTIONS
    if _GONE_EXCEPTIONS is None:
        names = ("BrowserNotRunning", "ConnectionException", "ConnectionFailed",
                 "ReconnectionFailed", "WebSocketConnectionClosed")
        found: list = [ConnectionError]            # 内建（不依赖 pydoll）
        try:
            import pydoll.exceptions as exceptions  # 惰性：离线环境也能 import 本模块
            for name in names:
                item = getattr(exceptions, name, None)
                if isinstance(item, type) and issubclass(item, BaseException):
                    found.append(item)
        except Exception:  # noqa: BLE001 —— 未装 pydoll：离线路径不需要它
            pass
        _GONE_EXCEPTIONS = tuple(found)
    return _GONE_EXCEPTIONS


async def _error(write: WriteLine, frame: P.Frame, code: str, hint: str) -> None:
    """回错误帧（`I21`：必须可操作）。"""
    await write(P.encode_error(code, frame.id, hint))


async def handle_frame(frame: P.Frame, *, write: WriteLine,
                       browser: Optional["BrowserAccess"] = None) -> bool:
    """处理**一条合法帧**；返回 `False` = 伺服终止（`shutdown`）。

    * `write` —— 异步写帧（生产侧包一层 `asyncio.to_thread`，见 `Daemon._write`）；
    * `browser` —— `BrowserAccess`；`None` = 离线伺服（浏览器命令一律回**可操作错误**，不假装成功）。
    """
    if frame.kind != P.KIND_CMD:
        await _error(write, frame, P.BAD_FRAME, "该方向只接受命令帧（kind=cmd）")
        return True
    reason = P.validate_command(frame)
    if reason is not None:
        # 词表外命令 / 缺必需字段 / 取值非法 → 按「帧不合词表」处理（回可操作原因，不执行）
        await _error(write, frame, P.BAD_FRAME, reason)
        return True

    if frame.name == "hello":
        await write(P.encode_event("ready", frame.id, **ready_payload()))
        problem = dependency_problem()
        if problem is not None:
            code, hint = problem
            redact.log_event("dependency_missing", "依赖缺失（已回引导）", level="warn",
                             code=code, hint=hint)
            # 事件帧（id="-"）：**不静默**——缺失必须让调用方看得见（I21 / M7B-13 / M7B-14）
            await write(P.encode_error(code, P.EVENT_ID, hint))
        return True

    if frame.name == "shutdown":
        grace_ms = frame.field("grace_ms")
        grace_ms = int(grace_ms) if isinstance(grace_ms, (int, float)) else P.SHUTDOWN_GRACE_MS
        await write(P.encode_event("stage", P.EVENT_ID, stage="close", ok=True))
        if browser is None:
            redact.log_event("shutdown", "本次连接未启动浏览器 → 直接收尾（无需关闭协议）")
        else:
            # L2（`I23②`）：**关闭前再写一次快照** —— 必须早于 `Browser.close`
            #（close 之后 CDP 已断，读不到 Cookie）。失败只记日志，不改变关闭协议结果。
            snapshot = await browser.save_snapshot(reason="shutdown")
            evidence = await browser.close(grace_ms)
            redact.log_event("shutdown_snapshot", "关闭前 L2 快照落盘结果",
                             snapshot_ok=bool(snapshot.get("ok")),
                             snapshot_reason=snapshot.get("reason"),
                             snapshot_count=snapshot.get("count"),
                             close_requested=bool(evidence.get("close_requested")),
                             waited_s=evidence.get("waited_s"))
        return False

    if frame.name == "open_tab":
        return await _cmd_open_tab(frame, write, browser)
    if frame.name == "login_state":
        return await _cmd_login_state(frame, write, browser)

    # 词表内、但尚未实现 → 如实报错 + 保持存活（不静默装作成功；MB-Q7）
    hint = NOT_IMPLEMENTED_HINT % frame.name
    if browser is not None:
        browser.note_command_error("daemon_down", hint)
    await _error(write, frame, "daemon_down", hint)
    return True


async def _ensure_browser(browser: Optional["BrowserAccess"], write: WriteLine,
                          frame: P.Frame) -> Optional[driver.BrowserDriver]:
    """取存活浏览器；失败 → **回可操作错误**并返回 `None`（调用方直接收尾）。"""
    if browser is None:
        await _error(write, frame, "daemon_down", OFFLINE_HINT)
        return None
    try:
        return await browser.ensure()
    except runtime.DependencyError as exc:
        browser.note_command_error(exc.code, exc.hint)
        await _error(write, frame, exc.code, exc.hint)
        return None
    except Exception as exc:  # noqa: BLE001 —— 启动失败同样要给可操作文案
        hint = ("启动浏览器失败：%s（检查 profile 是否被占用 / 浏览器是否被移除；"
                "详见 ~/.brain-ai/logs/browser.log）" % exc)
        browser.note_command_error("no_browser", hint)
        await _error(write, frame, "no_browser", hint)
        return None


async def _ensure_browser_restored(browser: Optional["BrowserAccess"], write: WriteLine,
                                   frame: P.Frame) -> Optional[driver.BrowserDriver]:
    """`_ensure_browser` + **L2 回灌**（每个浏览器会话一次）+ 恢复失败的**显式提示**（`I23④`）。

    恢复失败**不影响本次命令的成败**：额外发一条 `error` 事件帧（`id="-"`）携带可操作引导，
    命令本身照常执行（用户可能手动重登）。
    """
    driver_now = await _ensure_browser(browser, write, frame)
    if driver_now is None or browser is None:
        return None
    outcome = await browser.restore_snapshot()
    if outcome is None:
        return driver_now                       # 已灌过 / 功能关闭
    written, reason = outcome
    if written > 0 or reason == "absent":
        return driver_now                       # 成功，或首次运行（**正常**，不吓用户）
    hint = RESTORE_FAILED_HINT % reason
    browser.note_command_error("not_logged_in", hint)
    await write(P.encode_error("not_logged_in", P.EVENT_ID, hint))
    return driver_now


async def _run_browser_op(browser: "BrowserAccess", op: Callable[[], Any]) -> Any:
    """跑一次浏览器操作；命中「浏览器已不在运行」→ **吞掉 + 自愈 + 重试一次**（`M7B-11`）。

    `op` 必须是**可重复调用**的（自愈后驱动对象已换 ⇒ 闭包里要现取 `browser.driver`）。
    """
    gone = gone_exceptions()
    try:
        return await op()
    except gone as exc:  # type: ignore[misc]  # 空元组 = 永不匹配（离线环境）
        await browser.restart_after_gone("%s: %s" % (type(exc).__name__, exc))
        return await op()


async def _cmd_open_tab(frame: P.Frame, write: WriteLine,
                        browser: Optional["BrowserAccess"]) -> bool:
    """`open_tab`：导航到调用方给的 url（**无站点回落**，`I14`）+ `stage{open}`。"""
    if await _ensure_browser_restored(browser, write, frame) is None:
        return True
    assert browser is not None                       # `_ensure_browser` 已保证
    url = str(frame.field("url") or "")
    provider = str(frame.field("provider") or "")
    if not url:
        await _error(write, frame, "daemon_down", MISSING_URL_HINT)
        browser.note_command_error("daemon_down", MISSING_URL_HINT)
        return True
    ok, detail = True, url
    try:
        await _run_browser_op(browser, lambda: browser.driver.tab_for(url))
    except Exception as exc:  # noqa: BLE001 —— 失败要可操作，不静默
        ok, detail = False, "%s: %s" % (type(exc).__name__, exc)
    # ⚠️ 口径：`stage` 帧**带请求 id** = 该命令的**完成回包**（调用方 `call()` 在此完成）；
    #    `id="-"` 的 `stage` 才是纯进度事件（如 `shutdown` 的 `stage{close}`）。
    #    该规则需 C++ 侧 `PipeClient` 同步（step 5~6）—— 已登记在 §6.2 step 4 记录里。
    await write(P.encode_event("stage", frame.id, stage="open", ok=ok))
    if ok:
        redact.log_event("open_tab", "已导航到目标页", provider=provider, url=url)
        if browser.snapshot is not None:
            # L2 作用域：只记**调用方给的 url 的 host**（`I14`：Python 侧不读条目表、不自造站点）
            browser.snapshot.add_scope(url)
        return True
    hint = "打开标签页失败：%s（可重试；若浏览器已被关，命令会自动重启浏览器）" % detail
    browser.note_command_error("no_browser", hint)
    await _error(write, frame, "no_browser", hint)
    return True


async def _cmd_login_state(frame: P.Frame, write: WriteLine,
                           browser: Optional["BrowserAccess"]) -> bool:
    """`login_state` → `login{state, cookie_names, has_expires, http_only}`（**值不进协议**）。

    `state` 判定：给了期望名单（**可选**字段 `cookie_names`，来源 = 条目 `web.cookie_names`）→
    `logged_in` / `not_logged_in`；**未给 → `unknown`**（观测值照回，判定交调用方 —— `I14`：
    站点知识只在条目，Python 侧不自造）。
    """
    if await _ensure_browser_restored(browser, write, frame) is None:
        return True
    assert browser is not None
    provider = str(frame.field("provider") or "")
    raw_expected = frame.field("cookie_names")
    expected = [str(item) for item in raw_expected] if isinstance(raw_expected, list) else []
    suffix = str(frame.field("domain_suffix") or "")
    try:
        cookies = await _run_browser_op(browser, lambda: browser.driver.cookies_for_domain(suffix))
    except Exception as exc:  # noqa: BLE001
        hint = "读取 Cookie 失败：%s（可重试）" % exc
        browser.note_command_error("daemon_down", hint)
        await _error(write, frame, "daemon_down", hint)
        return True
    rows = redact.redact_cookies(cookies)          # 只留名字 / 长度 / 标志位
    names = [row["name"] for row in rows]
    if expected:
        state = "logged_in" if all(name in names for name in expected) else "not_logged_in"
    else:
        state = "unknown"
    if state == "logged_in":
        # L2（`MB-Q5` 第二个触发点）：**登录即写** —— 刚判到登录就落盘，避免「登完就断电」
        await browser.save_snapshot(reason="login", provider=provider)
    await write(P.encode_event("login", frame.id, state=state, cookie_names=names,
                               has_expires=any(not row.get("session") for row in rows),
                               http_only=any(bool(row.get("http_only")) for row in rows)))
    redact.log_event("login_state", "登录态观测（值不进协议、日志已脱敏）", provider=provider,
                     state=state, expected=expected, cookies=rows)
    return True


# ============================================================================
#  离线伺服（自检用；**与生产共用 `handle_frame`**）
# ============================================================================

def _thread_writer(connection: Any) -> WriteLine:
    """把阻塞式 `connection.write_line` 包成异步写（**不卡事件循环**）。"""
    async def write(line: str) -> None:
        await asyncio.to_thread(connection.write_line, line)
    return write


async def _serve_offline_async(connection: Any, idle_timeout_s: float) -> int:
    """离线读循环：返回处理的**合法帧数**。

    非法行**只**丢弃 + 记日志 + 回 `err{bad_frame}`，**不退出**（`VB2-29`）；
    读走 `asyncio.to_thread`（阻塞读不占事件循环 = `M7B-04` 的生产形态）。
    """
    timeout_ms = int(max(0.1, idle_timeout_s) * 1000)
    served = 0
    while True:
        try:
            line = await asyncio.to_thread(connection.read_line, timeout_ms)
        except pipe.PipeTimeout:
            return served                      # 空闲超时 → 正常收尾（不留僵尸）
        except pipe.PipeError as exc:
            print("[伺服] 管道错误：%s（%s）" % (exc, exc.hint), flush=True)
            return served
        if line is None:
            return served                      # 对端断开（C++ 侧退出 / 关句柄）
        parsed = P.parse_line(line)
        if isinstance(parsed, P.BadFrame):
            print("[伺服] 丢弃非法帧：%s" % parsed.reason, flush=True)
            redact.log_event("bad_frame", "丢弃非法帧：%s" % parsed.reason, level="warn")
            try:
                await asyncio.to_thread(connection.write_line,
                                        P.encode_error(P.BAD_FRAME, P.EVENT_ID, parsed.reason))
            except pipe.PipeError:
                return served                  # 对端已走 → 回不了就算了（不假装成功）
            continue
        print("[伺服] 收到 %d 字节 · %s(id=%s)" % (len(line), parsed.name, parsed.id), flush=True)
        served += 1
        if not await handle_frame(parsed, write=_thread_writer(connection), browser=None):
            return served


def serve_offline_connection(connection: Any, idle_timeout_s: float = 5.0) -> int:
    """离线伺服的**同步**入口（自检 / `--pipe-selftest` 用；内部自建事件循环）。"""
    return asyncio.run(_serve_offline_async(connection, idle_timeout_s))


# ============================================================================
#  L4 状态留痕（`M7B-11`：未干净退出 → 下次启动可见）
# ============================================================================

STATE_PATH = redact.LOG_DIR / "daemon-state.json"


def write_state(phase: str, path: Optional[Any] = None, **extra: Any) -> Dict[str, Any]:
    """写守护进程状态（诊断用，**不是** L2 凭据快照 —— 后者归 `session.py` / step 5）。"""
    target = pathlib.Path(path) if path else STATE_PATH
    record: Dict[str, Any] = {"phase": str(phase), "pid": os.getpid(),
                              "ts": round(time.time(), 3), "version": PACKAGE_VERSION}
    for key, value in extra.items():
        record[key] = redact.scrub(value)           # 同一包内的脱敏单点
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
    except OSError:
        pass
    return record


def read_state(path: Optional[Any] = None) -> Dict[str, Any]:
    """读上一次的守护进程状态（不存在 / 坏 → 空字典）。"""
    target = pathlib.Path(path) if path else STATE_PATH
    try:
        data = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def previous_abnormal(previous: Optional[Dict[str, Any]]) -> bool:
    """上次状态**不是** `exited_clean`（但确实跑过）→ 异常退出（`M7B-11`：登录态可能已回滚）。"""
    if not previous:
        return False
    phase = str(previous.get("phase") or "")
    return bool(phase) and phase != "exited_clean"


# ============================================================================
#  守护进程（`--serve` 的真身：管道监听线程 × asyncio 主循环）
# ============================================================================

class Daemon:
    """把 `--serve` 从「最小伺服」升为**主循环**（`M7B-11` / §6.2 step 4）。

    线程模型（`M7B-04` 生产形态）
      * **监听线程**：`accept` + `read_line`（阻塞）→ 帧投进 `asyncio.Queue`（`call_soon_threadsafe`）；
        非法行的丢弃与回帧**就地**在这个线程做（小帧、快路径）；
      * **主线程**：asyncio 事件循环 —— 跑 `handle_frame`（Pydoll 操作用同一循环）；
        写帧一律 `asyncio.to_thread`；
      * **收尾**：关管道（把监听线程从阻塞读里放出来）→ `join` → 补关浏览器（异常路径）。
    """

    def __init__(self, *, pipe_name: str, idle_timeout_s: float = DEFAULT_IDLE_TIMEOUT_S,
                 once: bool = False, headless: bool = False,
                 start_timeout: float = driver.DEFAULT_START_TIMEOUT_S) -> None:
        self.pipe_name = pipe.normalize_pipe_name(pipe_name)
        self.idle_timeout_s = float(idle_timeout_s)
        self.once = bool(once)
        # L2 快照（`MB-D0-8` L2）：缺省落 `~/.brain-ai/session/cookies.dat`；
        # `BRAIN_AI_SESSION_DIR` 可重定向 ⇒ 自检**绝不污染真实快照**（见 `session.session_dir()`）
        self.browser = BrowserAccess(headless=headless, start_timeout=start_timeout,
                                     snapshot=session.SnapshotStore())
        self._last_snapshot_at = 0.0    # 定时刷新节流（与命令**同一任务**，不并发访问 CDP）
        self.served = 0
        self.frame_errors = 0                     # 丢弃的非法帧数（诊断）
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._queue: Optional["asyncio.Queue"] = None
        self._stop = False
        self._listener: Optional[threading.Thread] = None
        self._server: Optional[pipe.PipeServer] = None
        self._connection: Any = None
        self.abnormal_previous = False            # 上次未干净退出（`_serve_async` 里填充）
        self._exit_reason = "shutdown"            # shutdown | closed | idle | pipe_error | fatal

    # ---- 线程侧 ----
    def _post(self, item: Any) -> None:
        """把线程侧事件投进事件循环（线程安全；循环已关 → 丢弃）。"""
        loop, queue = self._loop, self._queue
        if loop is None or queue is None:
            return
        try:
            loop.call_soon_threadsafe(queue.put_nowait, item)
        except RuntimeError:
            pass

    def _listen(self) -> None:
        """监听线程：建管道 → 逐个连接读帧 → 投队列。"""
        server = pipe.PipeServer(self.pipe_name)
        self._server = server
        try:
            server.open()
        except pipe.PipeError as exc:
            self._post(("fatal", None, "%s（%s）" % (exc, exc.hint)))
            return
        print("[守护进程] 管道 = %s（pid=%d%s）"
              % (server.name, os.getpid(), " · --once" if self.once else ""), flush=True)
        redact.log_event("daemon_start", "守护进程已就绪", pipe=server.name, once=self.once,
                         version=PACKAGE_VERSION, proto=PROTO_VERSION)
        print("[守护进程] 上次异常退出 = %s" % ("是（登录态可能已回滚）" if self.abnormal_previous else "否"),
              flush=True)
        while not self._stop:
            try:
                connection = server.accept(int(self.idle_timeout_s * 1000))
            except pipe.PipeTimeout:
                self._post(("idle", None, ""))
                return
            except pipe.PipeError as exc:
                self._post(("fatal", None, "%s（%s）" % (exc, exc.hint)))
                return
            self._connection = connection
            print("[守护进程] 客户端已连接", flush=True)
            redact.log_event("client_connected", "客户端已连接")
            self._post(("connected", connection, ""))
            self._read_connection(connection)
            if self.once or self._stop:
                return

    def _read_connection(self, connection: Any) -> None:
        """读一个连接上的帧（阻塞；非法帧就地处理）。"""
        timeout_ms = int(max(0.1, self.idle_timeout_s) * 1000)
        while not self._stop:
            try:
                line = connection.read_line(timeout_ms)
            except pipe.PipeTimeout:
                self._post(("idle_timeout", connection, ""))
                return
            except pipe.PipeError as exc:
                self._post(("pipe_error", connection, "%s（%s）" % (exc, exc.hint)))
                return
            if line is None:
                self._post(("closed", connection, ""))
                return
            parsed = P.parse_line(line)
            if isinstance(parsed, P.BadFrame):
                self.frame_errors += 1
                print("[伺服] 丢弃非法帧：%s" % parsed.reason, flush=True)
                redact.log_event("bad_frame", "丢弃非法帧：%s" % parsed.reason,
                                 level="warn", dropped=self.frame_errors)
                try:
                    connection.write_line(P.encode_error(P.BAD_FRAME, P.EVENT_ID, parsed.reason))
                except pipe.PipeError:
                    self._post(("closed", connection, ""))
                    return
                continue
            print("[伺服] 收到 %d 字节 · %s(id=%s)" % (len(line), parsed.name, parsed.id), flush=True)
            self._post(("frame", connection, parsed))

    # ---- 事件循环侧 ----
    def _write(self, connection: Any) -> WriteLine:
        """异步写帧（阻塞式 `WriteFile` 交给线程池，不卡循环）。"""
        async def write(line: str) -> None:
            await asyncio.to_thread(connection.write_line, line)
        return write

    async def _snapshot_tick(self) -> None:
        """L2 定时刷新（`MB-Q5`：5 s < 10 s 上限）；节流 → 浏览器活着 → 有作用域，才真读。

        * **空作用域**（本次会话还没 `open_tab` 过）⇒ **不写**（隐私最小 · `MB-Q11`）；
        * 读 Cookie / 写盘失败**只记日志**，绝不打断主循环（`I21` 同族：不静默，但也不致命）。
        """
        now = time.monotonic()
        if now - self._last_snapshot_at < session.REFRESH_INTERVAL_S:
            return
        self._last_snapshot_at = now
        store = self.browser.snapshot
        if store is None or not self.browser.alive or not store.scope:
            return
        await self.browser.save_snapshot(reason="refresh")

    async def _heartbeat_loop(self) -> None:
        """L4 心跳 + 崩溃检测（`M7B-11`：心跳可检测、浏览器死了看得见）。"""
        while True:
            await asyncio.sleep(HEARTBEAT_INTERVAL_S)
            driver_now = self.browser.driver
            redact.log_event("heartbeat", "存活", alive=self.browser.alive,
                             browser_pid=self.browser.browser_pid(),
                             session_starts=self.browser.session_starts,
                             self_heals=self.browser.self_heals, served=self.served,
                             frame_errors=self.frame_errors)
            if driver_now is not None and not driver_now.alive():
                redact.log_event("browser_crashed", "浏览器进程已退出（下条命令触发自愈）",
                                 level="warn", browser_pid=driver_now.pid())

    async def _serve_async(self) -> int:
        """主循环：等帧 → 分发；`shutdown` / 空闲 / 致命 → 收尾（返回退出码）。"""
        self._loop = asyncio.get_running_loop()
        self._queue = asyncio.Queue()
        previous = read_state()
        self.abnormal_previous = previous_abnormal(previous)
        write_state("running", previous_phase=previous.get("phase"),
                    abnormal_previous=self.abnormal_previous)
        if self.abnormal_previous:
            redact.log_event("previous_abnormal_exit",
                             "上次守护进程未干净退出 → 登录态可能已回滚（L4 可见）", level="warn",
                             previous_phase=previous.get("phase"), previous_pid=previous.get("pid"))
            print("[守护进程] ⚠️ 上次未干净退出（登录态可能已回滚）"
                  "—— 详见 ~/.brain-ai/logs/browser.log", flush=True)
        self._listener = threading.Thread(target=self._listen, daemon=True, name="pipe-listener")
        self._listener.start()
        heartbeat = asyncio.create_task(self._heartbeat_loop())
        exit_code = 0
        try:
            while True:
                try:
                    # `MB-Q5`：L2 定时刷新走「队列空闲超时」—— 刷新在**主循环同一任务**里做，
                    # 不起后台 CDP 任务 ⇒ 不存在「命令与快照并发访问 CDP」的时序问题。
                    kind, connection, payload = await asyncio.wait_for(
                        self._queue.get(), timeout=session.REFRESH_INTERVAL_S)
                except asyncio.TimeoutError:
                    await self._snapshot_tick()
                    continue
                if kind == "frame":
                    if not await handle_frame(payload, write=self._write(connection),
                                              browser=self.browser):
                        self._exit_reason = "shutdown"
                        break
                    self.served += 1
                    await self._snapshot_tick()
                    continue
                if kind == "connected":
                    continue
                if kind in ("closed", "idle_timeout", "pipe_error"):
                    redact.log_event("connection_ended", "客户端连接结束（%s）" % kind,
                                     level="warn" if kind == "pipe_error" else "info")
                    print("[守护进程] 客户端连接结束（%s）" % kind, flush=True)
                    if kind == "pipe_error":
                        self._exit_reason, exit_code = "pipe_error", 1
                        break
                    self._exit_reason = "closed"
                    if self.once:
                        break
                    continue
                if kind == "idle":
                    print("[守护进程] 等待客户端连接超时 → 退出（不留僵尸）", flush=True)
                    self._exit_reason = "idle"
                    break
                print("[守护进程] %s" % payload, file=sys.stderr, flush=True)
                redact.log_event("daemon_fatal", payload, level="error")
                self._exit_reason, exit_code = "fatal", 1
                break
        finally:
            await self._shutdown(heartbeat)
        return exit_code

    async def _shutdown(self, heartbeat: asyncio.Task) -> None:
        """收尾：停心跳 → 关管道（放监听线程出来）→ join → 补关浏览器（异常路径）→ 落状态。"""
        self._stop = True
        heartbeat.cancel()
        try:
            await heartbeat
        except asyncio.CancelledError:
            pass
        except Exception:  # noqa: BLE001
            pass
        await asyncio.to_thread(self._close_pipe)
        if self._listener is not None:
            self._listener.join(5)
        if self.browser.driver is not None:          # 只在异常路径走到（正常 shutdown 已关过）
            await self.browser.close()
        clean = self._exit_reason in ("shutdown", "closed", "idle")
        write_state("exited_clean" if clean else "exited_abnormal", reason=self._exit_reason,
                    served=self.served, frame_errors=self.frame_errors,
                    session_starts=self.browser.session_starts, self_heals=self.browser.self_heals,
                    snapshot_saves=self.browser.snapshot_saves,
                    snapshot_restores=self.browser.snapshot_restores,
                    snapshot_reason=(self.browser.snapshot.last_reason
                                     if self.browser.snapshot is not None else "disabled"))
        redact.log_event("daemon_exit", "守护进程退出（%s）" % self._exit_reason,
                         level="info" if clean else "warn", reason=self._exit_reason,
                         served=self.served, frame_errors=self.frame_errors,
                         session_starts=self.browser.session_starts,
                         self_heals=self.browser.self_heals,
                         snapshot_saves=self.browser.snapshot_saves,
                         snapshot_restores=self.browser.snapshot_restores)
        print("[守护进程] 退出（合法帧 %d · 丢弃非法帧 %d · 浏览器启动 %d 次 · 自愈 %d 次 · "
              "L2 快照写 %d / 灌 %d）"
              % (self.served, self.frame_errors, self.browser.session_starts,
                 self.browser.self_heals, self.browser.snapshot_saves,
                 self.browser.snapshot_restores), flush=True)

    def _close_pipe(self) -> None:
        """关管道与连接（让监听线程从阻塞读里出来；幂等）。"""
        connection, server = self._connection, self._server
        self._connection = None
        for handle in (connection, server):
            if handle is None:
                continue
            try:
                handle.close()
            except Exception:  # noqa: BLE001 —— 收尾失败不改变退出码（日志已留痕）
                pass


def serve(*, pipe_name: str, idle_timeout_s: float = DEFAULT_IDLE_TIMEOUT_S,
          once: bool = False, headless: bool = False,
          start_timeout: float = driver.DEFAULT_START_TIMEOUT_S) -> int:
    """`--serve` 的同步入口（内部跑 asyncio 主循环）；返回退出码（0 = 干净收尾）。"""
    instance = Daemon(pipe_name=pipe_name, idle_timeout_s=idle_timeout_s, once=once,
                      headless=headless, start_timeout=start_timeout)
    try:
        return asyncio.run(instance._serve_async())
    except KeyboardInterrupt:                        # 人来 Ctrl+C：如实记 L4 后正常退出
        redact.log_event("daemon_interrupt", "收到 Ctrl+C → 收尾", level="warn")
        return 130
