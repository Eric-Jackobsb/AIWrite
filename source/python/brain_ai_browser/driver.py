"""Pydoll 驱动（`M7B.md` §6.2 step 3 的**最小集**）。

本步范围（只这几项，其余如实留给后续步）
    `start` · `tab_for` / `new_tab` · `cookies_all` · `cookies_for_domain` ·
    `set_cookies` / `delete_all_cookies`（step 5：L2 快照**回灌**用）·
    `execute_script` · `close_wait`。
    ⬜ 未含：`attach_if_running`（step 4）· `type_humanized` / `press_key` /
    `set_file_input_files` / `expect_file_chooser` / `stream_deltas`（批 3）。

复用的已实测机制（`source/python/_probe/m7b01..m7b09`，**不再重新试错**）
    * headful 起真浏览器 + 单 profile `~/.brain-ai/pydoll-profile`；
    * `--no-first-run` / `--no-default-browser-check` **由库自己加**（重加会抛
      `ArgumentAlreadyExistsInOptions`）⇒ 我们只加 `--user-data-dir` 与 `--window-size`；
    * `execute_script` 返回**两层 `result`** ⇒ `unwrap_result()` 单点解包（解错层级会静默拿空串）；
    * 退出必须 `Browser.close` → **等进程真正退出**（强杀 = 登录态不落盘，`MB-D0-8` L1）。

硬边界
    * **不得在 UI 线程调用**（`M7.md` `Q4`）：本模块全部是 async API，由守护进程的
      asyncio 循环驱动；C++ 侧 `PipeClient.call()` 只在执行器工作线程跑。
    * **生产禁止强杀**（`MB-D0-8` L3）：`close_wait()` 超时**只留 warn**，
      不提供 `kill` 路径（探针里的 `kill_strays()` **不搬进生产**）。
    * pydoll 一律**惰性 import**（`runtime.load_pydoll()` / 函数内 import）⇒
      未装 pydoll 的系统 Python 下，`--selftest` / `--pipe-selftest` / `--serve` 照旧可跑。
    * 站点/URL **只来自调用方**（`I14`：无回落）；本模块**不读 `providers.json`**。
"""

from __future__ import annotations

import asyncio
import time
from typing import Any, Dict, Iterable, List, Optional, Tuple

from . import browsers
from . import runtime

__all__ = [
    "PROFILE",
    "WINDOW_SIZE",
    "DEFAULT_START_TIMEOUT_S",
    "DEFAULT_CLOSE_WAIT_S",
    "DriverDependencyError",
    "BrowserDriver",
    "build_options",
    "cookies_for_domain",
    "unwrap_result",
    "start",
]

# 跨语言常量：与 `browsers.PROFILE` / `M7B.md` §6.2 同值（C++ 侧 `utils/paths.h::pydoll_profile()`）
PROFILE = browsers.PROFILE
WINDOW_SIZE = browsers.WINDOW_SIZE
DEFAULT_START_TIMEOUT_S = 60.0   # 新 profile 首次启动较慢（探针经验）
DEFAULT_CLOSE_WAIT_S = 5.0       # `MB-Q5`：L1 关闭等待阈值（超时**只 warn**）

#: `I21` 依赖缺失错误（`driver` 层沿用 `runtime` 的实现，便于调用方一处 except）
DriverDependencyError = runtime.DependencyError


# ============================================================================
#  纯函数（离线可断言）
# ============================================================================

def unwrap_result(raw: Any) -> Any:
    """`execute_script` 原始返回 → **业务值**。

    ⚠️ pydoll 返回**两层 `result`**（`{'id':N,'result':{'result':{'type','value'}}}`），
    解错层级会**静默拿到空串**（`M7B-05` 首轮「未打通」的根因）⇒ 单点解包在这里，
    驱动对外**只暴露业务值**。
    """
    node = getattr(raw, "result", raw)
    for _ in range(4):
        if isinstance(node, dict) and isinstance(node.get("result"), dict):
            node = node["result"]
        else:
            break
    if isinstance(node, dict):
        return node.get("value") if "value" in node else None
    return node


def cookies_for_domain(cookies: Optional[Iterable[Any]], suffix: str) -> List[Dict[str, Any]]:
    """按 `domain_suffix` 过滤（`P3`：读全库 → **一律按域过滤**，否则跨域污染判据）。

    域名比对前**去掉前导点**（`.example.com` → `example.com`）并小写化；空后缀 = 原样返回。
    """
    rows = [dict(cookie) for cookie in (cookies or []) if isinstance(cookie, dict)]
    if not suffix:
        return rows
    want = str(suffix).strip().lower().lstrip(".")
    return [row for row in rows
            if str(row.get("domain") or "").strip().lower().lstrip(".").endswith(want)]


def build_options(*, profile: Optional[Any] = None, window_size: Tuple[int, int] = WINDOW_SIZE,
                  headless: bool = False, start_timeout: float = DEFAULT_START_TIMEOUT_S) -> Tuple[Any, List[str]]:
    """构造 `ChromiumOptions`（**只加我们自己的两个参数**）。

    返回 `(options, arguments)`：`arguments` = 本驱动**显式添加**的参数列表，
    供自检回证（`P4`：视口尺寸写死 `--window-size=1440,1000`）。
    """
    from pydoll.browser.options import ChromiumOptions  # 惰性

    options = ChromiumOptions()
    options.headless = bool(headless)
    arguments = ["--user-data-dir=%s" % browsers.profile_path(profile),
                 "--window-size=%d,%d" % (int(window_size[0]), int(window_size[1]))]
    for argument in arguments:
        options.add_argument(argument)
    options.start_timeout = int(start_timeout)
    return options, arguments


# ============================================================================
#  启动（单一入口；自检可打桩 ⇒ `VB2-30` 离线可验）
# ============================================================================

def _launch(browser_cls: Any, options: Any) -> Any:
    """**唯一**的浏览器构造点（自检打桩点：证明「依赖缺失时一次都没调」）。"""
    return browser_cls(options=options)


async def start(*, profile: Optional[Any] = None, window_size: Tuple[int, int] = WINDOW_SIZE,
                headless: bool = False,
                start_timeout: float = DEFAULT_START_TIMEOUT_S) -> "BrowserDriver":
    """起浏览器并交出驱动。

    顺序（**依赖缺失先报错，绝不先开浏览器**，`VB2-30` / `I21`）：
      ① `runtime.require_runtime()` → pydoll 缺失 ⇒ `DependencyError(no_python)`
      ② `browsers.detect_browser()` → 无浏览器 ⇒ `DependencyError(no_browser)`
      ③ 构造选项 → `_launch` → `browser.start()`
    """
    runtime.require_runtime()
    kind, exe = browsers.detect_browser()
    if exe is None:
        raise DriverDependencyError("no_browser", runtime.dependency_hint("no_browser"))
    browser_cls = browsers.browser_class()
    options, arguments = build_options(profile=profile, window_size=window_size,
                                       headless=headless, start_timeout=start_timeout)
    browser = _launch(browser_cls, options)
    tab = await browser.start()
    return BrowserDriver(browser, tab, kind=kind, exe=str(exe),
                         profile=browsers.profile_path(profile),
                         window_size=(int(window_size[0]), int(window_size[1])),
                         headless=headless, arguments=arguments)


# ============================================================================
#  驱动
# ============================================================================

class BrowserDriver:
    """一个浏览器进程 + 当前标签页（批 1 step 3 最小集）。"""

    def __init__(self, browser: Any, tab: Any, *, kind: str, exe: str, profile: Any,
                 window_size: Tuple[int, int], headless: bool, arguments: List[str]) -> None:
        self._browser = browser
        self._tab = tab
        self.kind = kind
        self.exe = exe
        self.profile = profile
        self.window_size = window_size
        self.headless = headless
        self.arguments = list(arguments)   # 我们**显式**加的参数（自检回证用）
        self.started_at = time.monotonic()

    # ---- 基本 ----
    @property
    def tab(self) -> Any:
        return self._tab

    def process(self) -> Any:
        """浏览器进程（pydoll 私有链；探针同样依赖，缺失 → `None`）。"""
        manager = getattr(self._browser, "_browser_process_manager", None)
        return getattr(manager, "_process", None)

    def pid(self) -> Optional[int]:
        process = self.process()
        return getattr(process, "pid", None)

    def alive(self) -> bool:
        process = self.process()
        return process is not None and process.poll() is None

    def __repr__(self) -> str:
        return ("BrowserDriver(kind=%s, pid=%s, profile=%s, headless=%s)"
                % (self.kind, self.pid(), self.profile, self.headless))

    # ---- 标签页 ----
    async def new_tab(self, url: str = "") -> Any:
        """新开标签页（`MB-D2` 每站点一个 tab；**url 由调用方给**，`I14` 无回落）。"""
        self._tab = await self._browser.new_tab(url) if url else await self._browser.new_tab()
        return self._tab

    async def tab_for(self, url: str) -> Any:
        """导航当前标签页到 `url` 并返回它（**只认调用方给的 url**）。"""
        await self._tab.go_to(url)
        return self._tab

    # ---- 脚本 ----
    async def execute_script(self, script: str) -> Any:
        """执行 JS 并返回**解包后的业务值**（两层 `result` 由 `unwrap_result` 收敛）。"""
        return unwrap_result(await self._tab.execute_script(script))

    # ---- Cookie（**浏览器级** `Storage.getCookies` · 全 origin · 含 HttpOnly）----
    async def cookies_all(self) -> List[Dict[str, Any]]:
        """**全库** Cookie（浏览器级 `Storage.getCookies`）。

        ⚠️ 必须走 `self._browser.get_cookies()`（`M7B-17` 口径）：`Tab.get_cookies()` 在无
        `browser_context_id` 时走 **`Network.getCookies`（页级）** —— 只返回**当前页可见**的
        Cookie、依赖当前页停在哪（`about:blank` 上读不到站点 Cookie）。页级读会**漏掉父域
        登录 Cookie**（最典型的登录态形态）。原始字段照回；值**不进协议**（§6.1 只传名单 / 标志位）。
        """
        cookies = await self._browser.get_cookies()
        return [dict(cookie) for cookie in (cookies or []) if isinstance(cookie, dict)]

    async def cookies_for_domain(self, suffix: str) -> List[Dict[str, Any]]:
        """按域过滤（`P3`）。"""
        return cookies_for_domain(await self.cookies_all(), suffix)

    async def set_cookies(self, params: Optional[Iterable[Dict[str, Any]]]) -> int:
        """回灌 Cookie（浏览器级 `Storage.setCookies`）；返回**尝试写入**的条数。

        `params` 由 `session.to_cdp_params()` 生成（**字段白名单** + 会期语义）；
        空列表 → 直接返回 0（不发命令）。
        """
        rows = [dict(item) for item in (params or []) if isinstance(item, dict)]
        if not rows:
            return 0
        await self._browser.set_cookies(rows)
        return len(rows)

    async def delete_all_cookies(self) -> None:
        """清空本 profile 的 Cookie（浏览器级 `Storage.clearCookies`）—— 自检「注销→回灌」用；
        站点级登出（`logout_site`）归 step 6 接线。"""
        await self._browser.delete_all_cookies()

    # ---- 关闭（`I23①` / `MB-D0-8` L1）----
    async def close_wait(self, timeout_s: float = DEFAULT_CLOSE_WAIT_S) -> Dict[str, Any]:
        """`Browser.close` → **等进程真正退出**（超时**只 warn，不强杀**）。

        返回证据：`{exit_code, waited_s, close_reply_error?, warning?}`。
        `waited_s < 0` ⇒ 未在期限内退出（**不 kill**：强杀会让登录态回滚 + 残留占用
        profile）；如实回报，由上层决定提示 / 下次启动探端口。
        """
        info: Dict[str, Any] = {"exit_code": None, "waited_s": None, "warning": ""}
        process = self.process()
        try:
            await self._browser._execute_command(  # noqa: SLF001 —— 探针 M7B-01/09 同款
                {"method": "Browser.close", "params": {}})
        except Exception as exc:  # noqa: BLE001 —— 关窗瞬间连接断开属正常
            info["close_reply_error"] = type(exc).__name__
        if process is not None:
            info["waited_s"] = await self._wait_exit(process, timeout_s)
            if info["waited_s"] < 0:
                info["warning"] = ("浏览器 %.1fs 内未退出（L1：不强杀）—— 登录态可能未落盘，"
                                   "下次启动前需探端口 / 提示用户" % timeout_s)
            info["exit_code"] = process.poll()
        else:
            info["warning"] = "拿不到浏览器进程句柄 → 无法等待退出（如实回报，不假装完成）"
        try:
            await self._browser._connection_handler.close()  # noqa: SLF001
        except Exception:  # noqa: BLE001
            pass
        return info

    @staticmethod
    async def _wait_exit(process: Any, timeout_s: float) -> float:
        """等进程退出；返回实际等待秒数（未退出 → `-1.0`）。"""
        started = time.monotonic()
        while time.monotonic() - started < timeout_s:
            if process.poll() is not None:
                return round(time.monotonic() - started, 2)
            await asyncio.sleep(0.1)
        return -1.0

    # ---- 上下文管理 ----
    async def __aenter__(self) -> "BrowserDriver":
        return self

    async def __aexit__(self, *_exc: object) -> None:
        await self.close_wait()
