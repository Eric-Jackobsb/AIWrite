"""Pydoll 驱动（`M7B.md` §6.2 step 3 的**最小集**）。

本步范围（只这几项，其余如实留给后续步）
    `start` · `tab_for` / `new_tab` · `cookies_all` · `cookies_for_domain` ·
    `set_cookies` / `delete_all_cookies`（step 5：L2 快照**回灌**用）·
    `execute_script` · `close_wait`。
    ✅ **批 3 step 14（P1）已落地：内容返回基建** —— `selector_facts` / `pick_visible` /
      `type_humanized` / `press_key` / `click_selector` / `set_file_input_files` /
      `inject_files_via_chooser` / `read_answer_text`（+ 纯函数 `pick_visible_index` /
      `answer_payload` / `done_hit`）= `send_prompt` / `read_answer` / `upload_image` 的地基。
    ⬜ 未含：`attach_if_running`（step 4）· `stream_deltas`（CDP 增量 · `M7B-21`）。

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
import json
import time
from typing import Any, Dict, Iterable, List, Optional, Tuple

from . import browsers
from . import runtime

__all__ = [
    "PROFILE",
    "WINDOW_SIZE",
    "DEFAULT_START_TIMEOUT_S",
    "DEFAULT_CLOSE_WAIT_S",
    "ANSWER_TEXT_LIMIT_BYTES",
    "DEFAULT_POLL_MS",
    "DEFAULT_MAX_POLLS",
    "DEFAULT_STABLE_ROUNDS",
    "DriverDependencyError",
    "DriverContentError",
    "BrowserDriver",
    "build_options",
    "cookies_for_domain",
    "unwrap_result",
    "pick_visible_index",
    "answer_payload",
    "done_hit",
    "start",
]

# 跨语言常量：与 `browsers.PROFILE` / `M7B.md` §6.2 同值（C++ 侧 `utils/paths.h::pydoll_profile()`）
PROFILE = browsers.PROFILE
WINDOW_SIZE = browsers.WINDOW_SIZE
DEFAULT_START_TIMEOUT_S = 60.0   # 新 profile 首次启动较慢（探针经验）
DEFAULT_CLOSE_WAIT_S = 5.0       # `MB-Q5`：L1 关闭等待阈值（超时**只 warn**）

# ---- 内容返回（批 3 step 14 · P1）----
#: 回答正文上限：与 `daemon.SCRIPT_RESULT_LIMIT_BYTES` **同值**（单帧 64 KiB 留余量 · `M7B-55`）
ANSWER_TEXT_LIMIT_BYTES = 48 * 1024
#: `read_answer` 缺省轮询参数（与 `dom_web_client.clamp_poll_params` 同口径）
DEFAULT_POLL_MS = 1000
DEFAULT_MAX_POLLS = 120
DEFAULT_STABLE_ROUNDS = 3        # 未配 `done_when` 时：文本连续 N 轮不变视为完成


class DriverContentError(RuntimeError):
    """内容返回失败（**可操作** · `I21`）：选择器全未命中 / 输入框不在 / 发送按钮缺失 / 页面未就绪。

    与 `DriverDependencyError` 分开：后者 = 依赖缺失（`no_python` / `no_browser`），
    前者 = **站点侧**（选择器 / 页面）⇒ 调用方文案不同（`M7B-25`：给出下一步）。
    """

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


def pick_visible_index(facts: Optional[Iterable[Any]]) -> int:
    """多候选选择器判定（**纯函数** · `M7B-24`）：首个「**命中且可见**」者胜。

    * `facts` = 每个候选的 `{"hits": int, "visible": bool}`（`BrowserDriver.selector_facts()` 采集）
    * 返回索引；**全不中 → `-1`** ⇒ 调用方**必须如实报错**（不得回落第一个候选 —— `I14` 无回落）
    """
    for index, fact in enumerate(facts or []):
        if not isinstance(fact, dict):
            continue
        try:
            hits = int(fact.get("hits") or 0)
        except (TypeError, ValueError):
            hits = 0
        if hits > 0 and bool(fact.get("visible")):
            return index
    return -1


def answer_payload(text: Any, limit_bytes: int = ANSWER_TEXT_LIMIT_BYTES) -> Tuple[str, int, bool]:
    """回答正文 → `(可入帧文本, 原始字节数, 是否截断)`（**纯函数** · `I21` 不假装完整）。

    * 未超限 → 原样 + `truncated=False`
    * 超限 → **UTF-8 安全前缀** + `truncated=True`（调用方**必须**在界面 / 日志标注「已截断」· `M7B-55`）
    """
    body = str(text or "")
    raw = body.encode("utf-8")
    if len(raw) <= int(limit_bytes):
        return body, len(raw), False
    prefix = raw[:int(limit_bytes)].decode("utf-8", errors="ignore")
    return prefix, len(raw), True


def done_hit(done_when: Optional[Dict[str, Any]], present: bool,
             stable_rounds: int, need_rounds: int = DEFAULT_STABLE_ROUNDS) -> bool:
    """`done_when` 判定（**纯函数** · 与 `dom_web_client.kPollScript` 同口径）。

    * `selector_present` → 命中即完成；`selector_gone` → 消失即完成
    * **缺省 / 未知取值** → 「文本连续 `need_rounds` 轮不变」（不假装有更好判据）
    """
    kind = ""
    if isinstance(done_when, dict):
        kind = str(done_when.get("kind") or "")
    if kind == "selector_present":
        return bool(present)
    if kind == "selector_gone":
        return not bool(present)
    return int(stable_rounds) >= max(1, int(need_rounds))


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

    async def current_tab_url(self) -> str:
        """当前标签页 URL（`current_tab` 命令的地基 · v3 批 3 step 11）。

        ⚠️ pydoll 的 `Tab.current_url` 在不同版本上有**属性 / 协程**两种形态 ⇒ 这里统一
        `await if coroutine`；**读不到一律回空串**（调用方按「不在任何站点」处置 —— 不假装）。
        """
        if self._tab is None:
            return ""
        try:
            value = getattr(self._tab, "current_url", "")
            if asyncio.iscoroutine(value):
                value = await value
            return str(value or "")
        except Exception:  # noqa: BLE001 —— 读不到不致命：如实回空串
            return ""

    # ---- 脚本 ----
    async def execute_script(self, script: str, *, return_by_value: bool = False,
                             user_gesture: bool = False) -> Any:
        """执行 JS 并返回**解包后的业务值**（两层 `result` 由 `unwrap_result` 收敛）。

        ⚠️ **默认（`return_by_value=False`）拿不到非原始值**：CDP 对**对象 / 数组**只回
        `objectId`（不带 `value`）⇒ 解包得 `None`（**静默 null，最易误判为「通道不通」**）。
        需要**结构化结果**（DOM 枚举 / 诊断脚本 / `run_script`）**必须**传 `return_by_value=True`。

        实测（2026-10-03 · 批 3 step 8）：`return [1,'two',true,null];` 与返回对象的脚本
        **一律回 `null`**，而 `return 6 * 7;`（数字）正常 ⇒ 与 `M7B-05` 的「两层 `result`」
        同族坑：**读回层级 / 序列化选项错，会把「机制可用」误记为「机制不可用」**。
        """
        if return_by_value:
            return unwrap_result(await self._tab.execute_script(
                script, return_by_value=True, user_gesture=user_gesture))
        return unwrap_result(await self._tab.execute_script(script, user_gesture=user_gesture))

    # ---- 内容返回（批 3 step 14 · P1：`send_prompt` / `read_answer` / `upload_image` 的地基）----
    #  * 站点知识**全在调用方**（`I14`）：选择器由命令下发，本层不认识任何站点
    #  * 注入用 pydoll **原生真打字**（`keyboard.type_text` · humanize），与「JS 一次性灌值」区分
    async def selector_facts(self, selectors: Iterable[Any]) -> List[Dict[str, Any]]:
        """每个候选的 `{selector, hits, visible}`（**只读**；多候选判定的输入 · `M7B-24`）。"""
        items = [str(item) for item in (selectors or []) if str(item or "")]
        if not items:
            return []
        script = (
            "const sels = %s; const out = [];"
            "for (const s of sels) { let hits = 0; let visible = false;"
            " try { const nodes = document.querySelectorAll(s); hits = nodes.length;"
            "  for (const node of nodes) {"
            "   const rects = node.getClientRects ? node.getClientRects() : null;"
            "   const shown = (node.offsetParent !== null) || (rects && rects.length > 0);"
            "   if (shown) { visible = true; break; } } } catch (e) { hits = 0; visible = false; }"
            " out.push({ hits: hits, visible: visible }); }"
            "return out;" % json.dumps(items)
        )
        value = await self.execute_script(script, return_by_value=True)
        rows = value if isinstance(value, list) else []
        facts: List[Dict[str, Any]] = []
        for index, item in enumerate(items):
            row = rows[index] if index < len(rows) and isinstance(rows[index], dict) else {}
            facts.append({"selector": item,
                          "hits": int(row.get("hits") or 0),
                          "visible": bool(row.get("visible"))})
        return facts

    async def pick_visible(self, selectors: Iterable[Any]) -> str:
        """多候选 → **首个「命中且可见」的选择器**；全不中 ⇒ `DriverContentError`（不回落、不猜 · `I14`）。"""
        items = [str(item) for item in (selectors or []) if str(item or "")]
        facts = await self.selector_facts(items)
        index = pick_visible_index(facts)
        if index < 0:
            raise DriverContentError(
                "选择器全部未命中或不可见：%s（可用 `--web-adapter-selftest --provider <id>` 复核）"
                % json.dumps(items, ensure_ascii=False))
        return items[index]

    async def type_humanized(self, selector: str, text: str) -> Dict[str, Any]:
        """聚焦 `selector` → **humanize 真打字**（`M7B-05` 实测逐字符、154 ms/字符）。"""
        if not selector:
            raise DriverContentError("type_humanized 需要具体选择器（I14：选择器由调用方下发）")
        focused = await self.execute_script(
            "const el = document.querySelector(%s);"
            "if (!el) { return false; } el.focus(); return true;" % json.dumps(selector))
        if not bool(focused):
            raise DriverContentError("输入框不存在：%s" % selector)
        await self._tab.keyboard.type_text(str(text))
        return {"selector": selector, "chars": len(str(text))}

    async def press_key(self, key_name: str) -> Dict[str, Any]:
        """按一次功能键（`send.kind=key` 的发送实现）。

        ⚠️ `pydoll 2.27.0` 的 `keyboard.press()` 要 **`Key` 枚举**（`pydoll.constants.Key`）；
        传字符串会按元组解包 ⇒ `ValueError: too many values to unpack`（2026-10-02 B3 实测踩到）。
        """
        from pydoll.constants import Key  # 惰性：离线自检不依赖 pydoll

        key = getattr(Key, str(key_name).upper(), None)
        if key is None:
            raise DriverContentError("pydoll Key 无成员：%s（可试 Enter / Tab / Escape）" % key_name)
        await self._tab.keyboard.press(key)
        return {"key": str(key_name)}

    async def click_selector(self, selector: str) -> Dict[str, Any]:
        """点击 `selector`（`send.kind=click` 的发送实现；带 `user_gesture` 以触发原生行为）。"""
        if not selector:
            raise DriverContentError("click_selector 需要具体选择器")
        clicked = await self.execute_script(
            "const el = document.querySelector(%s);"
            "if (!el) { return false; } el.click(); return true;" % json.dumps(selector),
            user_gesture=True)
        if not bool(clicked):
            raise DriverContentError("发送按钮不存在：%s" % selector)
        return {"selector": selector}

    async def set_file_input_files(self, selector: str, paths: Iterable[Any]) -> Dict[str, Any]:
        """文件注入（**主路线** · `P7b-10`）：`DOM.setFileInputFiles`（`M7B-05` 实测生效 + 触发 change）。

        * **必须走 tab 连接**（探针同款：`DOM.getDocument` → `DOM.querySelector` → `setFileInputFiles`）
        * 站点无 `<input type=file>` → `DriverContentError`（可操作：指向条目 `web.attach` 备选入口）
        """
        files = [str(item) for item in (paths or []) if str(item or "")]
        if not selector or not files:
            raise DriverContentError("set_file_input_files 需要文件输入框选择器 + 至少一个绝对路径")
        document = await self._tab._execute_command(  # noqa: SLF001 —— 探针 M7B-05 同款
            {"method": "DOM.getDocument", "params": {}})
        root = document.get("result", {}).get("root", {}).get("nodeId")
        node = await self._tab._execute_command(  # noqa: SLF001
            {"method": "DOM.querySelector", "params": {"nodeId": root, "selector": selector}})
        target = node.get("result", {}).get("nodeId")
        if not target:
            raise DriverContentError(
                "未找到文件输入框：%s（站点可能改用拖拽 / 粘贴入口 —— 见条目 `web.attach`）" % selector)
        await self._tab._execute_command(  # noqa: SLF001
            {"method": "DOM.setFileInputFiles", "params": {"files": files, "nodeId": target}})
        return {"selector": selector, "node_id": target, "count": len(files)}

    async def inject_files_via_chooser(self, trigger_selector: str, paths: Iterable[Any],
                                       *, wait_s: float = 0.6) -> Dict[str, Any]:
        """文件注入（**备选** · `expect_file_chooser`）：仅当站点**无** `input[type=file]` 时用。"""
        files = [str(item) for item in (paths or []) if str(item or "")]
        if not files:
            raise DriverContentError("inject_files_via_chooser 需要至少一个绝对路径")
        async with self._tab.expect_file_chooser(files):
            if trigger_selector:
                await self.click_selector(trigger_selector)
            await asyncio.sleep(max(0.0, float(wait_s)))
        return {"count": len(files), "trigger": trigger_selector}

    async def read_answer_text(self, selectors: Iterable[Any], *, done_when: Any = None,
                               poll_ms: int = DEFAULT_POLL_MS, max_polls: int = DEFAULT_MAX_POLLS,
                               timeout_s: float = 120.0,
                               stable_rounds: int = DEFAULT_STABLE_ROUNDS) -> Dict[str, Any]:
        """轮询 `answer_selector` 取回答正文（**最后一个**命中节点 = 本轮回答）。

        * 判据：`done_when.kind` = `selector_present` / `selector_gone`；缺省 = **文本连续 N 轮稳定**
        * 硬上限：`poll_ms` / `max_polls` / `timeout_s`（`R13`：有硬上限，不无限等）
        * **截断显式**（`I21` · `M7B-55`）：超 `ANSWER_TEXT_LIMIT_BYTES` → `truncated=true` + 前缀
        * 返回 `{text, text_bytes, truncated, polls, found, last_error}` —— **不抛**（如实回报，
          由调用方决定「未取到」文案；与 `dom_web_client` 的 R13 口径一致）
        """
        items = [str(item) for item in (selectors or []) if str(item or "")]
        poll_ms = max(200, min(int(poll_ms or DEFAULT_POLL_MS), 2000))
        max_polls = max(1, min(int(max_polls or DEFAULT_MAX_POLLS), 600))
        script = (
            "const sels = %s; let text = ''; let hits = 0;"
            "for (const s of sels) { try { const nodes = document.querySelectorAll(s);"
            " if (nodes.length > 0) { const last = nodes[nodes.length - 1];"
            "  text = (last.innerText || last.textContent || '').trim(); hits = nodes.length;"
            "  break; } } catch (e) {} }"
            "return { text: text, hits: hits };" % json.dumps(items)
        )
        started = time.monotonic()
        last = ""
        stable = 0
        polls = 0
        last_error = ""
        while polls < max_polls and (time.monotonic() - started) < float(timeout_s):
            polls += 1
            await asyncio.sleep(poll_ms / 1000.0)
            try:
                value = await self.execute_script(script, return_by_value=True)
            except Exception as exc:  # noqa: BLE001 —— 偶发失败下一轮重试（单次不致命）
                last_error = "%s: %s" % (type(exc).__name__, exc)
                continue
            row = value if isinstance(value, dict) else {}
            text = str(row.get("text") or "")
            if text and text != last:
                last = text
                stable = 0
            elif text:
                stable += 1
            if last and done_hit(done_when, int(row.get("hits") or 0) > 0, stable, stable_rounds):
                break
        body, raw_bytes, truncated = answer_payload(last)
        return {"text": body, "text_bytes": raw_bytes, "truncated": truncated,
                "polls": polls, "found": bool(last), "last_error": last_error}

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
        站点级登出（`logout_site`）见下 `clear_origin_data`。"""
        await self._browser.delete_all_cookies()

    async def clear_origin_data(self, origin: str) -> None:
        """按 origin 清站点数据（`Storage.clearDataForOrigin` · v3 批 3 step 11）—— `logout_site` 的地基。

        * **只碰该 origin**（Cookie / localStorage / IndexedDB / Cache / ServiceWorker …）
          ——「按站点注销」的语义；其他站点的登录态**不受影响**（与「删整个 profile」区分）
        * `origin` **必须由调用方给**（`I14`：Python 侧不读条目表、不自造站点）
        * ⚠️ **必须走 tab（页）连接**，**不能**走浏览器级连接 —— 实测（2026-10-04 · 本机）：
          浏览器级 `_execute_command` 对**任何** `storageTypes` 取值都回
          `Internal error (code -32603)`；tab 连接下 `all` / `cookies` / `local_storage`
          / `cookies,local_storage` **全部 OK**。
          （⚠️ 对照：`Browser.close` 与 `Storage.getCookies` 恰恰是**浏览器级**才对 ——
          **同一个 `Storage` 域名内，两条命令的传输层要求不同**，不能想当然照抄。）
        """
        if self._tab is None:
            raise RuntimeError("当前没有打开的标签页：请先 `open_tab` 到目标站点，再按站点注销")
        await self._tab._execute_command(  # noqa: SLF001 —— tab 级（浏览器级会 -32603，见上）
            {"method": "Storage.clearDataForOrigin",
             "params": {"origin": str(origin), "storageTypes": "all"}})

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
