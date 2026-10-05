"""`python -m brain_ai_browser`（M7B 批 1：协议词表 + 命名管道 + Pydoll 驱动 + 守护进程）。

用法
    python -m brain_ai_browser --selftest            # 协议词表离线自检（`VB2-29` 等价）
    python -m brain_ai_browser --stub-selftest       # 同上 + 明示「桩」边界
    python -m brain_ai_browser --pipe-selftest       # 命名管道 loopback 自检（`M7B-02`）
    python -m brain_ai_browser --driver-selftest [--headless] [--timeout <秒>]
                                                     # Pydoll 驱动 + `M7B-04` 合测（**会弹真实浏览器窗口**）
    python -m brain_ai_browser --daemon-selftest [--headless] [--timeout <秒>]
                                                     # 守护进程：`VB2-38` 离线 + `M7B-11` 端到端（**会弹窗**）
    python -m brain_ai_browser --session-selftest [--headless] [--timeout <秒>]
                                                     # L2 快照：`VB2-39` 离线 + 真机「存 → 弃 → 回灌」（**会弹窗**）
    python -m brain_ai_browser --serve [--pipe-name <名>] [--once] [--idle-timeout <秒>] [--headless]
                                                     # 守护进程主循环（`daemon.py`；C++ 的 `--pipe-selftest` 起它）

退出码
    0 = 通过 · 1 = 有失败 · 2 = 参数错误

✅ 批 3 step 14 起（**词表 v4**）**全部 10 条命令均已实现**：
    * `--serve` = **守护进程主循环**（`daemon.py`：管道监听线程 × asyncio 事件循环）：
      `hello` / `open_tab` / `login_state` / `upload_image` / `send_prompt` / `read_answer` /
      `run_script` / `logout_site` / `current_tab` / `shutdown`（浏览器**按需启动**）；
      `not_implemented` 码**保留但当前无使用点**（留给「先入表、后实现」的未来命令）。
    * **内容返回**（注入 → 发送 → 取回答）= `send_prompt` + `read_answer`
      （选择器**由调用方下发** · `I14`；注入 = pydoll **真打字**）；`run_script` **降级为诊断专用**。
    * `--driver-selftest` / `--daemon-selftest` 均含**离线组**（不碰浏览器）与**真机组**；
      依赖缺失时真机组**显式 SKIP**（不静默、不假装通过 —— `I21` 同族）。
    * **`VB2-38` 已在 Python 侧生效**（`daemon.close_verdict` 纯逻辑 + 端到端关闭协议）；
    * **`VB2-39`（L2 快照 · `session.py`）已在 Python 侧生效**：DPAPI 加密落
      `~/.brain-ai/session/cookies.dat`（**无 DPAPI ⇒ 不快照、绝不写明文**），三个触发点
      （定时 5 s / 登录即写 / `shutdown` **关闭前**再写）+ 启动**回灌**（失败发
      `error{not_logged_in}` **显式提示**，`I23④`）；**C++ 侧 `pydoll_channel` / `session_snapshot` /
      `PipeClient` 判据同步与诊断换代（`--pydoll-login`）待 step 6**。
    * `ready.browser` 由 `browsers.detect_browser()` 判定（先 Chrome 后 Edge）；依赖缺失时
      `hello` 会**追加一条 `error` 事件帧**携带可操作引导（`M7B-13` / `M7B-14`）。
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import types
from typing import Any, Dict, List, Optional, Tuple

from . import PACKAGE_VERSION, PROTO_VERSION
from . import browsers
from . import daemon
from . import driver
from . import pipe
from . import protocol as P
from . import redact
from . import runtime
from . import session



class _Counter:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, ok: bool, label: str, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print("   PASS  " + label)
        else:
            self.failed += 1
            print("   FAIL  " + label + (("  ->  " + detail) if detail else ""))
        return ok


def _protocol_selftest(counter: _Counter) -> None:
    """`VB2-29` / `VB2-32` 的 Python 侧等价断言（纯函数、离线）。"""
    print("[协议词表] 管道协议 v3（§6.1）离线自检")

    # ---- VB2-29① 合法命令帧：v / kind / id / name 解析正确 ----
    hello = P.parse_line(P.encode_command("hello", "7"))
    counter.check(
        isinstance(hello, P.Frame) and hello.v == PROTO_VERSION and hello.kind == P.KIND_CMD
        and hello.id == "7" and hello.name == "hello",
        "VB2-29① 合法命令帧：v / kind=cmd / id / name 解析正确（§6.1）",
        repr(hello))

    # ---- VB2-29② 非法行：JSON 坏 / 缺 v / 缺 kind / v 不识别 / 超上限 → 判非法 ----
    cases = {
        "JSON 坏": "{not json",
        "缺 v": '{"kind":"cmd","id":"1","name":"hello"}',
        "缺 kind": '{"v":1,"id":"1","name":"hello"}',
        "v 不识别": '{"v":99,"kind":"cmd","id":"1","name":"hello"}',
        "上限": '{"v":1,"kind":"cmd","id":"1","name":"hello","pad":"' + "x" * P.MAX_FRAME_BYTES + '"}',
    }
    bad_ok = True
    detail = ""
    for label, line in cases.items():
        parsed = P.parse_line(line)
        if not isinstance(parsed, P.BadFrame) or not parsed.reason:
            bad_ok = False
            detail = "%s 未被判非法" % label
            break
    counter.check(bad_ok,
                  "VB2-29② 非法行（JSON 坏 / 缺 v / 缺 kind / v 不识别 / 超上限）→ 判非法并要求回 err{bad_frame}",
                  detail)

    # ---- VB2-29③ 未知命令 / 缺必需字段 → 可操作原因 ----
    unknown = P.parse_line(P.encode_command("fly_to_moon", "8"))
    missing = P.parse_line(P.encode_command("open_tab", "9"))
    unknown_reason = P.validate_command(unknown) if isinstance(unknown, P.Frame) else "非 Frame"
    missing_reason = P.validate_command(missing) if isinstance(missing, P.Frame) else "非 Frame"
    counter.check(
        isinstance(unknown_reason, str) and "未知命令" in unknown_reason
        and isinstance(missing_reason, str) and "缺少字段" in missing_reason,
        "VB2-29③ 未知命令 / 缺必需字段 → 明确原因（词表 10 命令 + 必需字段）",
        "%r / %r" % (unknown_reason, missing_reason))

    # ---- VB2-29④ err{bad_frame} 回帧可生成且 id 与请求配对 ----
    err_line = P.encode_error(P.BAD_FRAME, "7", "非法帧已丢弃")
    err_frame = P.parse_line(err_line)
    counter.check(
        isinstance(err_frame, P.Frame) and err_frame.is_error()
        and err_frame.id == "7" and err_frame.name == P.BAD_FRAME
        and err_frame.field("hint") == "非法帧已丢弃",
        "VB2-29④ 非法帧回包 err{bad_frame} 且 id 与请求配对",
        err_line)

    # ---- VB2-29⑤ 上限写死：单帧 ≤ 64 KiB / images ≤ 8 ----
    limit_ok = (P.MAX_FRAME_BYTES == 64 * 1024 and P.MAX_IMAGES == 8
                and P.frame_within_limit("x" * P.MAX_FRAME_BYTES)
                and not P.frame_within_limit("x" * (P.MAX_FRAME_BYTES + 1)))
    too_many = P.parse_line(P.encode_command(
        "upload_image", "10", provider="doubao-web",
        images=["C:/a.png"] * (P.MAX_IMAGES + 1)))
    over_reason = P.validate_command(too_many) if isinstance(too_many, P.Frame) else "非 Frame"
    counter.check(limit_ok and isinstance(over_reason, str) and "上限" in over_reason,
                  "VB2-29⑤ 容量上限：单帧 ≤ 64 KiB / images ≤ 8",
                  "%r" % (over_reason,))

    # ---- VB2-32① CDP 增量帧 → 文本（形态 A / 形态 B）----
    a_text, a_flag = P.delta_text_of_frame('{"seq":1,"text":"你"}')
    b_text, b_flag = P.delta_text_of_frame('{"delta":{"text":"好"}}')
    none_text, _ = P.delta_text_of_frame('{"foo":1}')
    counter.check(a_text == "你" and b_text == "好" and none_text == ""
                  and not a_flag and not b_flag,
                  "VB2-32① CDP 增量帧 → 文本（`text` 直取 / `delta.text` 嵌套）",
                  "%r / %r" % (a_text, b_text))

    # ---- VB2-32② / I22：无增量 → 必须显式标注「非流式（轮询）」 ----
    _, flagged = P.delta_text_of_frame("")
    stage = P.parse_line(P.encode_event("stage", P.EVENT_ID, stage="answer", ok=True, stream=False))
    counter.check(flagged and isinstance(stage, P.Frame) and stage.is_event()
                  and stage.name == "stage" and stage.field("stage") == "answer",
                  "VB2-32②/I22 无增量 → 置「非流式（轮询）」标记（不静默改变行为）",
                  "")

    # ---- VB2-41①②（批 3 step 8 · 词表 v2）：`run_script` 入词表 + 结果截断保护 ----
    run_required = P.COMMAND_REQUIRED_FIELDS.get("run_script", ())
    counter.check(
        "run_script" in P.KNOWN_COMMANDS and "provider" in run_required and "script" in run_required
        and "script_done" in P.KNOWN_EVENTS and "script_error" in P.ERROR_CODES
        and PROTO_VERSION >= 2,
        "VB2-41① 词表 ≥v2：`run_script`（必需 provider+script）+ 事件 `script_done` + 错误码 `script_error`",
        "v=%d cmd=%d evt=%d" % (PROTO_VERSION, len(P.KNOWN_COMMANDS), len(P.KNOWN_EVENTS)))

    small_value, small_truncated = daemon.script_payload({"ok": 1})
    big_value, big_truncated = daemon.script_payload(
        {"pad": "x" * (daemon.SCRIPT_RESULT_LIMIT_BYTES + 10)})
    counter.check(
        small_value == {"ok": 1} and not small_truncated
        and big_truncated and isinstance(big_value, str)
        and len(big_value) <= daemon.SCRIPT_RESULT_LIMIT_BYTES,
        "VB2-41② `run_script` 结果截断保护：小结果原样回；超 48 KiB → 截断 + `truncated=true`（不假装完整）",
        "small_truncated=%s big_truncated=%s" % (small_truncated, big_truncated))

    # ---- VB2-42①②③（批 3 step 11 · 词表 v3）：会话族命令 + 站点键跨语言同构 ----
    logout_required = P.COMMAND_REQUIRED_FIELDS.get("logout_site", ())
    counter.check(
        PROTO_VERSION >= 3 and "logout_site" in P.KNOWN_COMMANDS
        and "provider" in logout_required and "current_tab" in P.KNOWN_COMMANDS
        and P.COMMAND_REQUIRED_FIELDS.get("current_tab") == ()
        and "tab" in P.KNOWN_EVENTS and "not_implemented" in P.ERROR_CODES
        and len(P.KNOWN_COMMANDS) == 10 and len(P.KNOWN_EVENTS) == 8,
        "VB2-42① 词表 ≥v3：`logout_site`（必需 provider）+ `current_tab`（无必需字段）+ 事件 `tab`"
        " + 错误码 `not_implemented`（闭合 MB-Q7）；词表 10 命令 / 8 事件",
        "v=%d cmd=%d evt=%d" % (PROTO_VERSION, len(P.KNOWN_COMMANDS), len(P.KNOWN_EVENTS)))

    tab_line = P.encode_event("tab", "21", url="https://example.com/x",
                              site="https://example.com")
    tab_frame = P.parse_line(tab_line)
    counter.check(
        isinstance(tab_frame, P.Frame) and tab_frame.is_event() and tab_frame.name == "tab"
        and tab_frame.field("site") == "https://example.com",
        "VB2-42② `tab` 事件入词表：`current_tab` 回包（url / site）可生成且可解析",
        tab_line)

    counter.check(
        daemon.site_key_of("https://Example.COM/a/b?q=1#f") == "https://example.com"
        and daemon.site_key_of("about:blank") == "about:blank"
        and daemon.site_key_of("") == "",
        "VB2-42③ `site_key_of` 与 C++ **逐字同构**（去路径/查询/片段 + 全小写；无 `://` 原样）",
        "%r / %r" % (daemon.site_key_of("https://Example.COM/a/b?q=1#f"),
                     daemon.site_key_of("about:blank")))

    # ---- VB2-44①~⑤（批 3 step 14 · 词表 v4）：站字段组 + 内容返回纯函数 ----
    send_required = P.COMMAND_REQUIRED_FIELDS.get("send_prompt", ())
    answer_required = P.COMMAND_REQUIRED_FIELDS.get("read_answer", ())
    bad_send = P.parse_line(P.encode_command(
        "send_prompt", "9", provider="p", prompt="x", input_selector="input",
        send={"kind": "key", "value": "Enter"}))
    no_selector = P.parse_line(P.encode_command(
        "send_prompt", "9", provider="p", prompt="x", send={"kind": "key", "value": "Enter"}))
    bad_kind = P.parse_line(P.encode_command(
        "send_prompt", "9", provider="p", prompt="x", input_selector=["input"],
        send={"kind": "clickk", "value": "b"}))
    good_send = P.parse_line(P.encode_command(
        "send_prompt", "9", provider="p", prompt="x", input_selector=["a", "b"],
        send={"kind": "key", "value": "Enter"}, upload_evidence=True))
    bad_evidence = P.parse_line(P.encode_command(
        "send_prompt", "9", provider="p", prompt="x", input_selector=["a"],
        send={"kind": "key", "value": "Enter"}, upload_evidence="yes"))
    counter.check(
        PROTO_VERSION == 4 and "input_selector" in send_required and "send" in send_required
        and "answer_selector" in answer_required
        and P.validate_command(bad_send) is not None       # 选择器给成字符串 → 非法
        and P.validate_command(no_selector) is not None    # 缺必需字段
        and P.validate_command(bad_kind) is not None       # send.kind 取值非法
        and P.validate_command(bad_evidence) is not None   # upload_evidence 非布尔
        and P.validate_command(good_send) is None
        and len(P.KNOWN_COMMANDS) == 10 and len(P.KNOWN_EVENTS) == 8,
        "VB2-44④ 词表 v4（站字段组）：`send_prompt` 必需 provider+prompt+input_selector+send、"
        "`read_answer` 必需 answer_selector；字段校验一律回**可操作原因**；命令 / 事件计数**不变**",
        "v=%d cmd=%d evt=%d reason=%s" % (PROTO_VERSION, len(P.KNOWN_COMMANDS),
                                          len(P.KNOWN_EVENTS), P.validate_command(bad_kind)))

    picked = driver.pick_visible_index(
        [{"hits": 0, "visible": False}, {"hits": 2, "visible": False},
         {"hits": 1, "visible": True}, {"hits": 3, "visible": True}])
    counter.check(
        picked == 2 and driver.pick_visible_index([]) == -1
        and driver.pick_visible_index([{"hits": 5, "visible": False}]) == -1,
        "VB2-44① 多候选判定（M7B-24）：首个「命中且可见」者胜；**全不中 → -1**（I14：不回落、不猜）",
        "picked=%s" % picked)

    counter.check(
        driver.done_hit({"kind": "selector_present"}, True, 0)
        and not driver.done_hit({"kind": "selector_present"}, False, 9)
        and driver.done_hit({"kind": "selector_gone"}, False, 0)
        and not driver.done_hit({"kind": "selector_gone"}, True, 9)
        and driver.done_hit({}, True, driver.DEFAULT_STABLE_ROUNDS)
        and not driver.done_hit({}, True, driver.DEFAULT_STABLE_ROUNDS - 1),
        "VB2-44② `done_when` 判定：`selector_present` / `selector_gone` / 缺省 = 文本稳定 N 轮"
        "（与 `dom_web_client.kPollScript` 同口径）",
        "stable_rounds=%d" % driver.DEFAULT_STABLE_ROUNDS)

    short_text, short_bytes, short_cut = driver.answer_payload("你好")
    long_body = "汉" * (driver.ANSWER_TEXT_LIMIT_BYTES // 3 + 100)
    long_text, long_bytes, long_cut = driver.answer_payload(long_body)
    counter.check(
        short_text == "你好" and short_bytes == 6 and not short_cut
        and long_cut and long_bytes == len(long_body.encode("utf-8"))
        and len(long_text.encode("utf-8")) <= driver.ANSWER_TEXT_LIMIT_BYTES
        and long_text == "汉" * len(long_text),
        "VB2-44③ 截断口径（I21 · M7B-55）：未超限原样回；超 48 KiB → **UTF-8 安全前缀** + "
        "`truncated=true` + 原始字节数（不假装完整）",
        "short=%d/%s long=%d/%s" % (short_bytes, short_cut, long_bytes, long_cut))


# ============================================================================
#  命名管道 / 守护进程（主循环已整体迁往 `daemon.py` —— `M7B.md` §6.2 step 4）
# ============================================================================

# `ready` 载荷与依赖引导（`M7B-13` / `M7B-14`）、帧处理（`daemon.handle_frame`）、
# 主循环（`daemon.serve`，监听线程 × asyncio）均在 `daemon.py`；
# 本文件只保留：协议离线自检 · loopback 自检 · 驱动自检 · **守护进程自检** · CLI 解析。


def _serve(pipe_name: str, once: bool, idle_timeout_s: float, headless: bool = False) -> int:
    """`--serve`：**委托 `daemon.serve`**（主循环 = 监听线程 × asyncio，`M7B-04` 生产形态）。"""
    return daemon.serve(pipe_name=pipe_name or pipe.daemon_pipe_name(),
                        idle_timeout_s=idle_timeout_s, once=once, headless=headless)


def _pipe_selftest(counter: "_Counter") -> None:
    """`M7B-02` 冒烟（**进程内 loopback**）：`PipeServer` ↔ `connect_pipe`。

    只证明**管道与协议帧**成立；跨语言（C++ ↔ Python）由 `aiwrite.exe --pipe-selftest` 覆盖。
    """
    print("[命名管道] loopback 自检（PipeServer ↔ connect_pipe；M7B-02）")
    name = pipe.DAEMON_PIPE_STEM + "selftest-" + str(os.getpid())
    outcome: Dict[str, Any] = {}

    def server_thread() -> None:
        server = pipe.PipeServer(name)
        try:
            connection = server.accept(10_000)
        except pipe.PipeError as exc:
            outcome["error"] = "%s（%s）" % (exc, exc.hint)
            return
        try:
            # 走**生产同一条帧处理路径**（`daemon.handle_frame`；离线 = 无浏览器会话）
            outcome["served"] = daemon.serve_offline_connection(connection, 5.0)
        finally:
            connection.close()
        server.close()

    thread = threading.Thread(target=server_thread, daemon=True)
    thread.start()
    client = None
    try:
        client = pipe.connect_pipe(name, 10_000)

        # ① 1 命令 + 1 事件：hello → ready{proto, python, browser}
        client.write_line(P.encode_command("hello", "1"))
        ready = P.parse_line(client.read_line(5_000) or "")
        if isinstance(ready, P.Frame) and daemon.dependency_problem() is not None:
            # 依赖缺失时守护进程会**追加**一条 error 事件帧（`M7B-13`/`14` 的「缺失 → 引导」）→ 先取走，
            # 免得被下一项断言误读（本机有 Edge + pydoll → 正常不触发）
            extra = P.parse_line(client.read_line(2_000) or "")
            counter.check(isinstance(extra, P.Frame) and extra.is_error(),
                          "M7B-13/14 依赖缺失 → ready 后**追加 error 事件**（可操作引导，不静默）",
                          repr(extra))
        counter.check(
            isinstance(ready, P.Frame) and ready.is_event() and ready.name == "ready"
            and ready.id == "1" and ready.field("proto") == PROTO_VERSION
            and isinstance(ready.field("python"), str) and bool(ready.field("python"))
            and isinstance(ready.field("browser"), str) and bool(ready.field("browser")),
            "M7B-02① hello → ready{proto, python, browser}（1 命令 + 1 事件）",
            repr(ready))

        # ② 非法行（JSON 坏）→ 回 err{bad_frame}，且伺服**不退出**（VB2-29）
        client.write_line("{not json")
        bad = P.parse_line(client.read_line(5_000) or "")
        counter.check(
            isinstance(bad, P.Frame) and bad.is_error() and bad.name == P.BAD_FRAME,
            "M7B-02② 非法行 → err{bad_frame}（丢弃不退出）",
            repr(bad))

        # ③ 未知命令 → err{bad_frame} + id 配对 + 可操作原因
        client.write_line(P.encode_command("fly_to_moon", "2"))
        unknown = P.parse_line(client.read_line(5_000) or "")
        counter.check(
            isinstance(unknown, P.Frame) and unknown.is_error() and unknown.id == "2"
            and "未知命令" in str(unknown.field("hint")),
            "M7B-02③ 未知命令 → err{id 配对 + 可操作原因}（不静默执行）",
            repr(unknown))

        # ④ 无换行超长帧（> 单帧上限）→ 判非法，伺服仍存活（缓冲不无限增长）
        client.write_bytes(
            P.encode_command("hello", "3").encode("utf-8")[:-1]
            + b'{"pad":"' + b"x" * (P.MAX_FRAME_BYTES + 10) + b'"}\n')
        oversize = P.parse_line(client.read_line(5_000) or "")
        counter.check(
            isinstance(oversize, P.Frame) and oversize.is_error(),
            "M7B-02④ 超单帧上限 → err{bad_frame}（伺服未退出）",
            repr(oversize))

        # ⑤ shutdown → stage{stage=close}（I23① 关闭阶段可见）
        client.write_line(P.encode_command("shutdown", "4", grace_ms=500))
        closing = P.parse_line(client.read_line(5_000) or "")
        counter.check(
            isinstance(closing, P.Frame) and closing.is_event() and closing.name == "stage"
            and closing.field("stage") == "close",
            "M7B-02⑤ shutdown → stage{stage=close}（关闭阶段可观测）",
            repr(closing))

        # ⑥ 伺服线程干净收尾（不留僵尸线程 / 帧序完整）
        #    served 只计**合法帧** → 3 = hello / 未知命令 / shutdown（两条非法行不计，VB2-29）
        thread.join(5)
        counter.check(
            not thread.is_alive() and outcome.get("served", 0) == 3 and "error" not in outcome,
            "M7B-02⑥ 伺服收尾：线程结束 + 合法帧序完整（served=%s）" % outcome.get("served"),
            str(outcome))
    except pipe.PipeError as exc:
        counter.check(False, "M7B-02 loopback 通道异常", "%s（%s）" % (exc, exc.hint))
    finally:
        if client is not None:
            client.close()
# ============================================================================
#  Pydoll 驱动自检（`M7B-04` 合测 + `VB2-30`；批 1 step 3）
# ============================================================================

#: 「可操作文案」判据（不冻结具体措辞 —— `M7B-13`/`14` 文案定稿归 step 4）
_ACTION_WORDS: Tuple[str, ...] = ("安装", "下载", "执行", "pip", "路径", "重开")


def _actionable(hint: Any) -> bool:
    """`I21`：引导文案非空 + 含**动作**（不是「出错了」这种没法操作的废话）。"""
    text = str(hint or "")
    return len(text) >= 8 and any(word in text for word in _ACTION_WORDS)


class _SocketWatch:
    """统计期间**新建的 socket 数**（`VB2-30` 的「不发起任何 HTTP」机器判据）。

    只包一层计数、不改变行为；`socket.create_connection` 等内部同样走 `socket.socket` 全局名。
    """

    def __init__(self) -> None:
        self.created = 0
        self._original = socket.socket

    def __enter__(self) -> "_SocketWatch":
        watch = self
        original = self._original

        class _CountingSocket(original):  # type: ignore[misc, valid-type]
            def __init__(self, *args: Any, **kwargs: Any) -> None:
                watch.created += 1
                super().__init__(*args, **kwargs)

        socket.socket = _CountingSocket
        return self

    def __exit__(self, *_exc: object) -> None:
        socket.socket = self._original


def _capture_dependency_error(loop: asyncio.AbstractEventLoop, coro: Any) -> Tuple[str, str]:
    """跑一个 `driver.start()` 协程，返回 `(错误码, 引导)`；非预期异常**如实回报**。"""
    try:
        loop.run_until_complete(coro)
    except runtime.DependencyError as exc:
        return exc.code, exc.hint
    except Exception as exc:  # noqa: BLE001 —— 意外异常也要留证，不吞
        return "意外异常", "%s: %s" % (type(exc).__name__, exc)
    return "", "start() 未报错（预期：依赖缺失 → 报错 + 引导）"


def _driver_offline_checks(counter: "_Counter") -> None:
    """`VB2-30` 组 A：依赖缺失 → 报错 + 引导 + **零副作用**（不开浏览器、不发 HTTP）。"""
    print("[驱动] 组 A：依赖缺失 → 报错 + 引导 + 零副作用（VB2-30 · 离线，不碰浏览器）")

    info = runtime.check_runtime()
    counter.check(
        all(key in info for key in ("python", "pydoll", "ok", "hint")) and isinstance(info["ok"], bool)
        and isinstance(info["python"], str) and bool(info["python"]),
        "VB2-30① check_runtime() → {python, pydoll, ok, hint}（`M7B-13` 契约）",
        repr(info))

    launches: List[str] = []
    original_launch = driver._launch
    original_load = runtime.load_pydoll
    original_detect = browsers.detect_browser
    loop = asyncio.new_event_loop()
    try:
        with _SocketWatch() as watch:
            # ② pydoll 缺失（打桩；无需真卸依赖）
            runtime.load_pydoll = lambda: None
            driver._launch = lambda *args, **kwargs: launches.append("launch")
            code_python, hint_python = _capture_dependency_error(loop, driver.start())
            # ③ 本机无浏览器（pydoll 视为已装）
            runtime.load_pydoll = lambda: types.SimpleNamespace()
            browsers.detect_browser = lambda: ("none", None)
            code_browser, hint_browser = _capture_dependency_error(loop, driver.start())
            sockets_created = watch.created
    finally:
        driver._launch = original_launch
        runtime.load_pydoll = original_load
        browsers.detect_browser = original_detect
        loop.close()

    counter.check(code_python == "no_python" and _actionable(hint_python) and not launches,
                  "VB2-30② pydoll 缺失 → err{no_python} + 可操作引导，且**一次都没调启动器**（不开浏览器）",
                  "code=%s · hint=%s · launches=%s" % (code_python, hint_python, launches))
    counter.check(code_browser == "no_browser" and _actionable(hint_browser) and not launches,
                  "VB2-30③ 本机无浏览器 → err{no_browser} + 可操作引导，且**一次都没调启动器**",
                  "code=%s · hint=%s" % (code_browser, hint_browser))
    counter.check(sockets_created == 0,
                  "VB2-30④ 组 A 全程**新建 socket 数 = 0**（不发起任何 HTTP；机器判据）",
                  "created=%d" % sockets_created)


def _stray_browsers() -> List[Dict[str, Any]]:
    """**只读**列出用本 profile 起的浏览器进程（`--driver-selftest` 收尾自检）。

    ⚠️ 探针里的 `kill_strays()` **不搬进生产**（`MB-D0-8` L3：禁止以强杀当手段）；
    本函数只列出，判据交调用方。取证失败 → 空表（并**打印**说明，不假装）。
    """
    script = ("Get-CimInstance Win32_Process -Filter \"Name='msedge.exe' or Name='chrome.exe'\" | "
              "Where-Object { $_.CommandLine -like '*pydoll-profile*' } | "
              "Select-Object ProcessId,Name | ConvertTo-Json -Compress")
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                              capture_output=True, text=True, timeout=30, check=False)
    except Exception:  # noqa: BLE001
        return []
    return _json_rows(done.stdout)


def _process_command_line(pid: Optional[int]) -> str:
    """进程真实命令行（物证：证明 `--window-size` / `--user-data-dir` 真传下去了）。"""
    if not pid:
        return ""
    script = "(Get-CimInstance Win32_Process -Filter \"ProcessId=%d\").CommandLine" % int(pid)
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                              capture_output=True, text=True, timeout=30, check=False)
    except Exception:  # noqa: BLE001
        return ""
    return (done.stdout or "").strip()


def _json_rows(text: Any) -> List[Dict[str, Any]]:
    """`ConvertTo-Json` 输出 → 字典列表（空 / 坏 → 空表）。"""
    raw = str(text or "").strip()
    if not raw:
        return []
    try:
        data = json.loads(raw)
    except ValueError:
        return []
    rows = data if isinstance(data, list) else [data]
    return [row for row in rows if isinstance(row, dict)]


async def _heartbeat(state: Dict[str, Any], interval_s: float = 0.05) -> None:
    """asyncio 侧心跳：命令执行期间**持续推进** ⇒ 事件循环未被管道读阻塞（`M7B-04`）。"""
    while not state.get("stop"):
        state["ticks"] = int(state.get("ticks", 0)) + 1
        await asyncio.sleep(interval_s)


async def _browser_burst(drv: "driver.BrowserDriver", rounds: int = 4) -> None:
    """浏览器侧连续命令（含短 sleep）—— 给并发的管道往返一个**时间窗口**。"""
    for _ in range(rounds):
        await drv.execute_script("return 1 + 1;")
        await asyncio.sleep(0.15)


def _pipe_roundtrip(name: str, timeout_ms: int = 5_000) -> Dict[str, Any]:
    """在**工作线程**里走一次管道往返（`asyncio.to_thread` 形态 = `M7B-04` 的合测形态）。"""
    out: Dict[str, Any] = {"sent_ts": 0.0, "recv_ts": 0.0, "ok": False, "detail": ""}
    try:
        client = pipe.connect_pipe(name, timeout_ms)
    except pipe.PipeError as exc:
        out["detail"] = "%s（%s）" % (exc, exc.hint)
        return out
    try:
        out["sent_ts"] = time.monotonic()
        client.write_line(P.encode_command("hello", "drv1"))
        frame = P.parse_line(client.read_line(timeout_ms) or "")
        out["recv_ts"] = time.monotonic()
        out["ok"] = isinstance(frame, P.Frame) and frame.is_event() and frame.name == "ready"
        out["detail"] = repr(frame)
    except pipe.PipeError as exc:
        out["detail"] = "%s（%s）" % (exc, exc.hint)
    finally:
        client.close()
    return out


def _driver_pipe_server(server: "pipe.PipeServer", outcome: Dict[str, Any]) -> None:
    """`M7B-04` 合测用：**独立线程**里的最小伺服（复用 `_serve_connection`，与生产同形）。

    实例由**主线程**先 `open()`（避免「线程还没建实例、客户端已重试到超时」的偶发失败）。
    """
    try:
        connection = server.accept(30_000)
    except pipe.PipeError as exc:
        outcome["error"] = "%s（%s）" % (exc, exc.hint)
        return
    try:
        # 与生产同一条帧处理路径（离线：无浏览器会话；`M7B-04` 合测只关心帧往返）
        outcome["served"] = daemon.serve_offline_connection(connection, 5.0)
    finally:
        connection.close()
        server.close()


async def _driver_online(counter: "_Counter", *, headless: bool, timeout_s: float) -> None:
    """组 B：真起浏览器（step 3 驱动断言 + `M7B-04` 合测）。"""
    name = pipe.DAEMON_PIPE_STEM + "driver-" + str(os.getpid())
    outcome: Dict[str, Any] = {}
    thread: Optional[threading.Thread] = None
    state: Dict[str, Any] = {"ticks": 0, "stop": False}
    heartbeat = asyncio.create_task(_heartbeat(state))
    before = _stray_browsers()
    drv: Optional["driver.BrowserDriver"] = None
    try:
        started = time.monotonic()
        try:
            drv = await driver.start(headless=headless, start_timeout=timeout_s)
        except runtime.DependencyError as exc:
            counter.check(False, "step3-① 驱动 start() 依赖错误（组 B 前置已查，不该走到这里）",
                          "%s（%s）" % (exc.code, exc.hint))
            return
        start_s = round(time.monotonic() - started, 2)
        counter.check(drv.alive() and drv.pid() is not None,
                      "step3-① 浏览器已起（pid=%s · 启动 %.2fs · %s）"
                      % (drv.pid(), start_s, drv.kind), repr(drv))
        counter.check(str(drv.profile).lower() == str(browsers.PROFILE).lower(),
                      "step3-② profile = ~/.brain-ai/pydoll-profile（单 profile）", str(drv.profile))
        counter.check("--window-size=%d,%d" % driver.WINDOW_SIZE in drv.arguments,
                      "step3-③ 视口尺寸写死 --window-size=1440,1000（P4：尺寸决定 DOM 形态）",
                      str(drv.arguments))

        script_value = await drv.execute_script("return 6 * 7;")
        counter.check(script_value == 42,
                      "step3-④ execute_script 两层 result 解包 → 42", repr(script_value))

        # 批 3 step 8：**结构化结果必须 `return_by_value=True`**
        #  （默认 False 时 CDP 对对象 / 数组只回 `objectId` ⇒ 解包得 `None` ⇒ 静默 null）
        script_obj = await drv.execute_script("return {a:1};", return_by_value=True)
        counter.check(script_obj == {"a": 1},
                      "step8-① `return_by_value=True` → 结构化对象可用"
                      "（默认 False 只回 objectId ⇒ null · 2026-10-03 实测教训）",
                      repr(script_obj))

        cookies = await drv.cookies_all()
        filtered = await drv.cookies_for_domain("example.invalid")
        counter.check(isinstance(cookies, list) and filtered == [],
                      "step3-⑤ cookies_all() 是 list（浏览器级 Storage.getCookies，未导航也成立）"
                      "+ 按域过滤生效（P3）",
                      "cookies=%d · filtered=%d" % (len(cookies), len(filtered)))

        command_line = _process_command_line(drv.pid())
        evidence_ok = (not command_line) or (
            "--window-size=%d,%d" % driver.WINDOW_SIZE in command_line
            and str(drv.profile) in command_line)
        counter.check(evidence_ok,
                      "step3-⑥ 进程真实命令行含 --window-size / --user-data-dir（物证；取不到则如实记「无法取证」）",
                      command_line[:160] or "（命令行取证为空 → 按「无法取证」记，不假装有证据）")

        # ---- M7B-04：管道监听线程 × asyncio 事件循环 共存 ----
        #  * 管道伺服在**独立线程**里跑（`accept` 的实例先由主线程 `open()`，客户端立即可连）
        #  * 判据是「**并发**」而不是「先后都成功」：往返时刻落在命令窗口内 +
        #    往返返回时浏览器侧命令**仍在跑** + asyncio 心跳持续推进
        server = pipe.PipeServer(name)
        server.open()
        thread = threading.Thread(target=_driver_pipe_server, args=(server, outcome), daemon=True)
        thread.start()
        ticks_before = int(state["ticks"])
        window_start = time.monotonic()
        burst = asyncio.create_task(_browser_burst(drv))
        trip = await asyncio.to_thread(_pipe_roundtrip, name, 5_000)
        burst_active = not burst.done()
        await burst
        window_end = time.monotonic()
        ticks = int(state["ticks"]) - ticks_before
        inside = (bool(trip["ok"]) and window_start <= float(trip["sent_ts"]) <= window_end
                  and window_start <= float(trip["recv_ts"]) <= window_end and burst_active)
        counter.check(inside,
                      "M7B-04① 浏览器命令执行**窗口内**管道往返成功且**命令仍在跑**"
                      "（管道监听线程 × asyncio 并发）",
                      "trip=%s · burst_active=%s · window=[%.2f, %.2f]"
                      % (trip["detail"], burst_active, window_start, window_end))
        counter.check(ticks >= 3,
                      "M7B-04② 窗口内心跳持续推进 %d 次（≈%.2fs）→ 事件循环未被管道读阻塞"
                      "（真并发，非「先管道后浏览器」）"
                      % (ticks, window_end - window_start),
                      "ticks=%d" % ticks)

        closed = await drv.close_wait(driver.DEFAULT_CLOSE_WAIT_S)
        counter.check(closed.get("exit_code") is not None and (closed.get("waited_s") or -1) >= 0
                      and not closed.get("fallback_kill"),
                      "step3-⑦ close_wait：Browser.close → 进程真退出（waited_s=%s · exit_code=%s · **未强杀**）"
                      % (closed.get("waited_s"), closed.get("exit_code")), str(closed))
        counter.check(not closed.get("warning"),
                      "step3-⑧ close_wait 无 warn（L1 期限内退出；有 warn 即如实报「登录态可能未落盘」）",
                      str(closed.get("warning")))
        after = _stray_browsers()
        counter.check(len(after) <= len(before),
                      "step3-⑨ 收尾无残留（本 profile 进程：前 %d → 后 %d；只读探测，**不杀**）"
                      % (len(before), len(after)), str(after)[:200])
        print("[驱动] 关闭证据：%s" % (closed,))
    finally:
        if drv is not None and drv.alive():
            leftover = await drv.close_wait(driver.DEFAULT_CLOSE_WAIT_S)
            print("[驱动] 兜底关闭（异常路径）：%s" % (leftover,))
        state["stop"] = True
        heartbeat.cancel()
        try:
            await heartbeat
        except asyncio.CancelledError:
            pass
        except Exception:  # noqa: BLE001
            pass
        if thread is not None:
            thread.join(5)
            print("[驱动] 管道伺服（独立线程）：served=%s · error=%s · 线程仍在跑=%s"
                  % (outcome.get("served"), outcome.get("error", ""), thread.is_alive()))
        else:
            print("[驱动] 管道伺服（独立线程）：未启动（异常早退）")


def _driver_selftest(counter: "_Counter", *, headless: bool, timeout_s: float) -> None:
    """step 3 自检：组 A（`VB2-30` 离线）+ 组 B（真机 + `M7B-04` 合测）。"""
    print("[驱动] 环境物证：%s" % (browsers.env_proof(),))
    _driver_offline_checks(counter)

    info = runtime.check_runtime()
    if not info["ok"]:
        print("   ----  组 B（真机）：SKIP —— 运行时依赖缺失：%s" % (info["hint"] or "pydoll 不可用"))
        print("   ----  说明：组 B 需要 pydoll；**这不是失败**，但也**不假装通过**（`I21` 同族）")
        return
    if browsers.detect_browser()[1] is None:
        print("   ----  组 B（真机）：SKIP —— 本机无 Chrome / Edge：%s" % runtime.NO_BROWSER_HINT)
        return

    print("[驱动] 组 B：真机合测（**会弹真实浏览器窗口**；headless=%s；启动超时 %.0fs；预计 ~10 s）"
          % (headless, timeout_s))
    if headless:
        print("   ----  ⚠️ headless 仅自检提速；生产口径是 **有头窗口**（合规 §13 / `M7B-14`）")
    asyncio.run(_driver_online(counter, headless=headless, timeout_s=timeout_s))


# ============================================================================
#  守护进程自检（`VB2-38` 离线 + `M7B-11` 端到端；批 1 step 4）
# ============================================================================

def _daemon_offline_checks(counter: "_Counter") -> None:
    """`VB2-38`：关闭协议**纯逻辑桩**（不启浏览器）+ `M7B-11` 的 L4 判定 / 日志脱敏。"""
    print("[守护进程] 组 A：关闭协议纯逻辑 / L4 判定 / 日志脱敏（VB2-38 · 离线，不碰浏览器）")

    cases: Tuple[Tuple[str, Dict[str, Any], bool], ...] = (
        ("缺 Browser.close → 失败", {"close_requested": False}, False),
        ("BrowserNotRunning（用户手动关窗）→ 归正常收尾", {"browser_not_running": True}, True),
        ("超时但**无 warn** → 失败", {"close_requested": True, "waited_s": -1.0, "warning": ""}, False),
        ("超时 + warn + 兜底强杀 → 通过（留痕）",
         {"close_requested": True, "waited_s": -1.0, "warning": "已兜底强杀", "fallback_kill": True}, True),
        ("超时 + warn 未强杀 → 通过（可恢复状态）",
         {"close_requested": True, "waited_s": -1.0, "warning": "未在期限内退出"}, True),
        ("等进程退出 → 通过", {"close_requested": True, "waited_s": 0.42}, True),
        ("矛盾证据（已退出却记强杀）→ 失败",
         {"close_requested": True, "waited_s": 0.42, "fallback_kill": True}, False),
    )
    failures: List[str] = []
    for label, evidence, expect_ok in cases:
        ok, reason = daemon.close_verdict(evidence)
        if ok != expect_ok:
            failures.append("%s ⇒ ok=%s（%s）" % (label, ok, reason))
    counter.check(not failures,
                  "VB2-38① close_verdict：%d 条判据全对（缺 close / 手动关窗 / 超时无 warn / 兜底强杀留痕 / 矛盾证据）"
                  % len(cases), "；".join(failures))

    states: Tuple[Tuple[Optional[Dict[str, Any]], bool], ...] = (
        (None, False), ({}, False), ({"phase": "running"}, True),
        ({"phase": "exited_abnormal", "reason": "pipe_error"}, True),
        ({"phase": "exited_clean"}, False),
    )
    state_bad = ["%r ⇒ %s" % (previous, daemon.previous_abnormal(previous))
                 for previous, expect in states if daemon.previous_abnormal(previous) != expect]
    counter.check(not state_bad,
                  "M7B-11① L4 状态判定：未干净退出（running / exited_abnormal）→ 下次启动可见",
                  "；".join(state_bad))

    # 日志脱敏：植入明文 → 落盘 → 文件内**零命中**（`M7B-11` / 值不进日志）
    planted = "SECRET_cookie_value_9f3a" + str(os.getpid())
    probe = redact.LOG_DIR / ("redact-selftest-%d.log" % os.getpid())
    try:
        redact.log_event("selftest", "值不应落盘：Cookie=%s" % planted, path=probe,
                         cookies=[{"name": "SID", "value": planted, "domain": ".example.com"}],
                         token=planted, note={"Authorization": planted})
        blob = probe.read_text(encoding="utf-8") if probe.is_file() else ""
    finally:
        try:
            probe.unlink()
        except OSError:
            pass
    counter.check(planted not in blob and "SID" in blob,
                  "M7B-11② 日志脱敏：植入明文 → 落盘文件内**零命中**（值不进日志；名字保留可诊断）",
                  blob.strip()[:160] or "（日志未落盘）")


def _daemon_call(client: Any, name: str, frame_id: str, timeout_ms: int = 10_000,
                 **fields: Any) -> Any:
    """发一条命令并读回包（同步；自检用）。"""
    client.write_line(P.encode_command(name, frame_id, **fields))
    return P.parse_line(client.read_line(timeout_ms) or "")


def _log_records(event: str, since: int = 0, path: Optional[Any] = None) -> List[Dict[str, Any]]:
    """取日志里某事件的记录（`since` = 起始行号，避免读到历史运行）。"""
    target = pathlib.Path(path) if path else redact.LOG_PATH
    if not target.is_file():
        return []
    try:
        lines = target.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    out: List[Dict[str, Any]] = []
    for line in lines[max(0, since):]:
        try:
            record = json.loads(line)
        except ValueError:
            continue
        if isinstance(record, dict) and record.get("event") == event:
            out.append(record)
    return out


def _log_line_count(path: Optional[Any] = None) -> int:
    target = pathlib.Path(path) if path else redact.LOG_PATH
    if not target.is_file():
        return 0
    try:
        return len(target.read_text(encoding="utf-8", errors="replace").splitlines())
    except OSError:
        return 0


def _tail_file(path: Any, lines: int = 6) -> str:
    target = pathlib.Path(path)
    if not target.is_file():
        return "（无）"
    try:
        return " | ".join(target.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])
    except OSError:
        return "（读不了）"


def _daemon_online(counter: "_Counter", *, headless: bool, timeout_s: float) -> None:
    """`M7B-11` 端到端：子进程 `--serve --once` + 真浏览器 + **自愈** + 关闭协议。"""
    name = pipe.DAEMON_PIPE_STEM + "daemon-selftest-" + str(os.getpid())
    out_log = redact.LOG_DIR / ("daemon-selftest-%d.out.log" % os.getpid())
    err_log = redact.LOG_DIR / ("daemon-selftest-%d.err.log" % os.getpid())
    args = [sys.executable, "-u", "-m", "brain_ai_browser", "--serve", "--once",
            "--pipe-name", name, "--idle-timeout", str(max(60.0, timeout_s))]
    if headless:
        args.append("--headless")
    since = _log_line_count()
    before_strays = _stray_browsers()
    cwd = str(pathlib.Path(__file__).resolve().parents[1])
    # L2（step 5）：子进程的快照**强制落临时目录** ⇒ 自检绝不碰真实 `~/.brain-ai/session/`
    session_temp = tempfile.mkdtemp(prefix="m7b-daemon-session-")
    child_env = dict(os.environ, **{session.ENV_SESSION_DIR: session_temp})
    client = None
    process = None
    try:
        with out_log.open("w", encoding="utf-8") as out_handle, \
                err_log.open("w", encoding="utf-8") as err_handle:
            process = subprocess.Popen(args, cwd=cwd, stdout=out_handle, stderr=err_handle,
                                       env=child_env)
            client = pipe.connect_pipe(name, 30_000)              # 等守护进程建管道
            print("[守护进程] 子进程已起（pid=%d）· 管道 %s" % (process.pid, name), flush=True)

            # ① hello → ready（id 配对；**不启动浏览器**）
            reply = _daemon_call(client, "hello", "d1")
            counter.check(isinstance(reply, P.Frame) and reply.is_event() and reply.name == "ready"
                          and reply.id == "d1" and reply.field("proto") == PROTO_VERSION,
                          "M7B-11③ 守护进程 `hello` → ready{proto, python, browser}（id 配对）",
                          repr(reply))
            counter.check(len(_log_records("browser_start", since)) == 0,
                          "M7B-11④ `hello` **不启动浏览器**（按需启动；冒烟仍秒级）",
                          str(_log_records("browser_start", since)))

            # ② open_tab → 按需起浏览器 → stage{open, ok=true} **带请求 id = 完成回包**
            reply = _daemon_call(client, "open_tab", "d2", provider="selftest", url="about:blank",
                                 timeout_ms=int(timeout_s * 1000))
            counter.check(isinstance(reply, P.Frame) and reply.is_event() and reply.name == "stage"
                          and reply.id == "d2" and reply.field("stage") == "open"
                          and reply.field("ok") is True,
                          "M7B-11⑤ open_tab → stage{open, ok=true}（浏览器按需启动；stage 带请求 id）",
                          repr(reply))
            starts = _log_records("browser_start", since)
            first_pid = starts[-1].get("browser_pid") if starts else None
            counter.check(bool(starts) and isinstance(first_pid, int),
                          "M7B-11⑥ L4 埋点：`browser_start` 落日志（browser_pid=%s）" % first_pid,
                          str(starts[-1] if starts else None))

            # ③ login_state → login{state, cookie_names, has_expires, http_only}（**值不进协议**）
            reply = _daemon_call(client, "login_state", "d3", provider="selftest",
                                 domain_suffix="brain-ai.invalid", timeout_ms=20_000)
            keys = set(reply.payload) if isinstance(reply, P.Frame) else set()
            counter.check(isinstance(reply, P.Frame) and reply.is_event() and reply.name == "login"
                          and reply.id == "d3"
                          and reply.field("state") in ("logged_in", "not_logged_in", "unknown")
                          and isinstance(reply.field("cookie_names"), list)
                          and "value" not in keys,
                          "M7B-11⑦ login_state → login{state, cookie_names, …}（**Cookie 值不进协议**）",
                          repr(reply))

            # ④ 自愈：强杀浏览器（**测试手段**，非生产路径）→ 再发命令 → 仍成功且 pid 换新（M7B-11）
            if isinstance(first_pid, int):
                subprocess.run(["taskkill", "/PID", str(first_pid), "/F"],
                               capture_output=True, check=False)
                time.sleep(1.5)
                reply = _daemon_call(client, "open_tab", "d4", provider="selftest",
                                     url="about:blank", timeout_ms=int(timeout_s * 1000))
                heals = _log_records("browser_selfheal", since)
                starts2 = _log_records("browser_start", since)
                second_pid = starts2[-1].get("browser_pid") if starts2 else None
                counter.check(isinstance(reply, P.Frame) and reply.field("ok") is True and bool(heals)
                              and second_pid != first_pid,
                              "M7B-11⑧ BrowserNotRunning 自愈：强杀浏览器后命令**仍成功**"
                              "（吞异常 + 重启；pid %s → %s）" % (first_pid, second_pid),
                              "%r · heals=%d" % (reply, len(heals)))
            else:
                counter.check(False, "M7B-11⑧ 自愈前置条件：拿不到 browser_pid（L4 缺口）", "")

            # ④′ **M7B-44（step 12）**：观测命令**不自愈** —— 关掉浏览器后发 `login_state`，
            #     必须回**可操作错误**，且**不得**冒出新的 `browser_start`（否则 = 又弹一个窗口）。
            #     ⚠️ 这一步是「一次点击弹出 4 个窗口」的**回归防线**（旧实现每轮轮询都重开浏览器）
            starts_now = _log_records("browser_start", since)
            live_pid = starts_now[-1].get("browser_pid") if starts_now else None
            if isinstance(live_pid, int):
                subprocess.run(["taskkill", "/PID", str(live_pid), "/F"],
                               capture_output=True, check=False)
                time.sleep(1.5)
                before_starts = len(_log_records("browser_start", since))
                reply = _daemon_call(client, "login_state", "d4b", provider="selftest",
                                     domain_suffix="brain-ai.invalid", timeout_ms=20_000)
                after_starts = len(_log_records("browser_start", since))
                hint = str(reply.field("hint") or "") if isinstance(reply, P.Frame) else ""
                counter.check(isinstance(reply, P.Frame) and reply.is_error()
                              and reply.name == "daemon_down"
                              and after_starts == before_starts and "不会" in hint,
                              "M7B-44① 观测命令 `login_state` **不自愈**：浏览器被关后回"
                              "err{daemon_down} + **不再重开**（browser_start %d → %d）"
                              % (before_starts, after_starts), repr(reply))
                # 生产命令的自愈**保留**（`M7B-11` 行为不变）—— 顺便把会话复原给 ⑤ 收尾
                reply = _daemon_call(client, "open_tab", "d4c", provider="selftest",
                                     url="about:blank", timeout_ms=int(timeout_s * 1000))
                counter.check(isinstance(reply, P.Frame) and reply.field("ok") is True,
                              "M7B-44② `open_tab` 的自愈**保留**（生产命令不受本步影响）",
                              repr(reply))
            else:
                counter.check(False, "M7B-44① 前置条件：拿不到 browser_pid（L4 缺口）", "")

            # ⑤ shutdown → stage{close} → 守护进程**等浏览器进程退出后**自行退出（I23①）
            client.write_line(P.encode_command("shutdown", "d5", grace_ms=5_000))
            closing = P.parse_line(client.read_line(10_000) or "")
            counter.check(isinstance(closing, P.Frame) and closing.is_event()
                          and closing.name == "stage" and closing.field("stage") == "close",
                          "M7B-11⑨ shutdown → stage{close}（关闭阶段可见）", repr(closing))
            code = process.wait(timeout=60)
            counter.check(code == 0,
                          "M7B-11⑩ 守护进程自行退出、退出码 0（未强杀；等浏览器退出后才收尾）", str(code))
            verdicts = _log_records("shutdown", since)
            counter.check(bool(verdicts) and verdicts[-1].get("verdict_ok") is True,
                          "VB2-38② 端到端关闭协议判定通过（L4 物证：%s）"
                          % (verdicts[-1].get("verdict_reason") if verdicts else "无记录"),
                          str(verdicts[-1] if verdicts else None))
            exits = _log_records("daemon_exit", since)
            counter.check(bool(exits) and exits[-1].get("reason") == "shutdown"
                          and not daemon.previous_abnormal(daemon.read_state()),
                          "M7B-11⑪ 干净退出留痕：`daemon_exit{reason=shutdown}` + 状态 `exited_clean`",
                          str(exits[-1] if exits else None))
            after_strays = _stray_browsers()
            counter.check(len(after_strays) <= len(before_strays),
                          "M7B-11⑫ 收尾无残留（本 profile 进程：前 %d → 后 %d；只读探测，**不杀**）"
                          % (len(before_strays), len(after_strays)), str(after_strays)[:200])
            print("[守护进程] 子进程 stdout 尾部：%s" % _tail_file(out_log, 5))
            err_text = _tail_file(err_log, 3)
            counter.check("Traceback" not in err_text,
                          "M7B-11⑬ 守护进程 stderr **无回溯**（BrowserNotRunning 被吞，不外泄异常）",
                          err_text)
            snapshot_events = (_log_records("snapshot_skipped", since)
                               + _log_records("snapshot_saved", since))
            counter.check(bool(snapshot_events),
                          "M7B-11⑭ L2 快照触发点端到端走通：`shutdown` **关闭前**落盘必经"
                          "（末条 = %s；`about:blank` 无该域 Cookie ⇒ 如实 `snapshot_skipped`，"
                          "证明这段**不是死码**）"
                          % (snapshot_events[-1].get("event") if snapshot_events else "无记录"),
                          str(snapshot_events[-1] if snapshot_events else None))
    finally:
        if client is not None:
            try:
                client.close()
            except Exception:  # noqa: BLE001
                pass
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)


def _daemon_selftest(counter: "_Counter", *, headless: bool, timeout_s: float) -> None:
    """step 4 自检：组 A（`VB2-38` 离线）+ 组 B（`M7B-11` 真机端到端）。"""
    _daemon_offline_checks(counter)
    if not runtime.check_runtime()["ok"]:
        print("   ----  组 B（真机）：SKIP —— 运行时依赖缺失：%s" % runtime.PYDOLL_HINT)
        print("   ----  说明：组 B 需要 pydoll；**这不是失败**，但也**不假装通过**")
        return
    if browsers.detect_browser()[1] is None:
        print("   ----  组 B（真机）：SKIP —— 本机无 Chrome / Edge：%s" % runtime.NO_BROWSER_HINT)
        return
    print("[守护进程] 组 B：端到端（**会弹真实浏览器窗口**；子进程 `--serve --once`；预计 ~25 s）")
    if headless:
        print("   ----  ⚠️ headless 仅自检提速；生产口径是 **有头窗口**（合规 §13）")
    _daemon_online(counter, headless=headless, timeout_s=timeout_s)


# ============================================================================
#  L2 快照自检（`VB2-39`；批 1 step 5 —— `MB-D0-8` L2）
# ============================================================================

def _session_probe_store(temp: Any, host: str) -> "session.SnapshotStore":
    """造一个**隔离**的快照存储（临时目录 ⇒ 绝不碰真实 `~/.brain-ai/session/`）。"""
    store = session.SnapshotStore(pathlib.Path(temp) / session.SNAPSHOT_NAME)
    store.add_scope(host)
    return store


def _session_seed_cookies(host: str) -> List[Dict[str, Any]]:
    """合成「登录态」（名 / 值都带**哨兵串** ⇒ 明文扫描有稳定判据）；含会期 + 持久各一条。"""
    stamp = str(os.getpid())
    return [
        {"name": "aiwrite_selftest_sid", "value": "SIDVALUE" + stamp, "domain": host,
         "path": "/", "secure": True, "httpOnly": True, "sameSite": "Lax", "size": 21},
        {"name": "aiwrite_selftest_doc", "value": "DOCVALUE" + stamp, "domain": "." + host,
         "path": "/", "expires": time.time() + 3600},
    ]


def _session_offline_checks(counter: "_Counter", temp: Any) -> None:
    """组 A（离线）：`VB2-39①②④⑤` + 纯函数判据；**DPAPI 不可用 → 显式 SKIP**（不是 FAIL）。"""
    print("[L2 快照] 组 A：DPAPI / 明文扫描 / 损坏不崩 / 纯函数（离线，不碰浏览器）")
    if not session.dpapi_available():
        print("   ----  组 A：SKIP —— 本机无 DPAPI（非 Windows / crypt32 不可用）")
        print("   ----  说明：L2 快照**不降级为明文**；这不是失败，但也**不假装通过**（I23③）")
        return

    host = "session-selftest.invalid"
    store = _session_probe_store(temp, host)
    seeded = _session_seed_cookies(host)

    # ---- ① DPAPI 往返 ----
    probe_plain = ("probe-" + os.urandom(8).hex()).encode("utf-8")
    blob = session.protect(probe_plain)
    counter.check(blob != probe_plain and session.unprotect(blob) == probe_plain,
                  "VB2-39① DPAPI 往返：CryptProtectData → CryptUnprotectData 字节一致"
                  "（CRYPTPROTECT_UI_FORBIDDEN ⇒ 无 UI 提示、可无人值守）",
                  "密文 %d 字节" % len(blob))

    # ---- ② 快照文件字节零明文（名 + 值 + 域）----
    saved = store.save(seeded, reason="selftest")
    blob_file = store.path.read_bytes() if store.exists() else b""
    secrets = ([row["name"] for row in seeded] + [row["value"] for row in seeded]
               + [host, "." + host])
    hits = session.scan_plaintext(blob_file, secrets)
    counter.check(bool(saved.get("ok")) and saved.get("changed") and not hits,
                  "VB2-39② 快照落盘 = DPAPI 密文：文件字节内 **Cookie 名 / 值 / 域零明文**"
                  "（扫 %d 个哨兵串：" % len(secrets) + "命中 %d）" % len(hits),
                  "命中：%s · 文件 %d 字节" % (hits, len(blob_file)))
    print("[L2 快照] 快照文件：%s（%d 字节 · 载荷 %d 条）"
          % (store.path, len(blob_file), saved.get("count")))
    print("[L2 快照] 明文扫描哨兵：%s" % ", ".join(secrets[:4]))

    # ---- ③ 纯函数：作用域（**父域方向**）/ 会期语义 / 白名单 / 过期 / 空作用域 ----
    parent_cookie = {"name": "p1", "value": "v1", "domain": ".example.com", "path": "/"}
    child_cookie = {"name": "c1", "value": "v2", "domain": "chat.example.com", "path": "/"}
    other_cookie = {"name": "o1", "value": "v3", "domain": "other.example.org", "path": "/"}
    domain_ok = (session.belongs_to(".example.com", "chat.example.com")
                 and not session.belongs_to("chat.example.com", "other.example.com")
                 and session.host_of("https://chat.example.com:443/a?b=1") == "chat.example.com")
    picked = session.scoped([parent_cookie, child_cookie, other_cookie], ["chat.example.com"])
    counter.check(domain_ok and [row["name"] for row in picked] == ["p1", "c1"],
                  "VB2-39③ 作用域判定：**父域 Cookie 必须命中**（`.example.com` ⊃ `chat.example.com`）"
                  "+ 邻居域不串味（`driver.cookies_for_domain` 的后缀语义方向与之相反，别混用）",
                  "picked=%s" % [row["name"] for row in picked])

    session_only = {"name": "s1", "value": "v", "domain": "a.example", "path": "/", "expires": -1}
    persist_only = {"name": "s2", "value": "v", "domain": "a.example", "path": "/",
                    "expires": time.time() + 3600, "size": 7}
    params = {row["name"]: row for row in session.to_cdp_params([session_only, persist_only])}
    counter.check("expires" not in params["s1"] and "expires" in params["s2"]
                  and not ({"size", "session"} & set(params["s1"])),
                  "VB2-39③ 回灌参数：会期 Cookie **不带 expires**（不偷偷升级为持久）"
                  "+ 只读字段（size / session）被白名单剔除（探针 V-b 实测：多带会被 CDP 拒）",
                  "params=%s" % sorted(params["s1"]))
    counter.check(session.usable_cookies({"cookies": [session_only, persist_only]},
                                        now=time.time() + 7200)[0]["name"] == "s1"
                  and session.scoped(seeded, []) == [],
                  "VB2-39③ 过期条目被过滤（`now` 推到 +2 h：只剩**会期**的 `s1`，"
                  "未过期的 `s2` 在这时点已过期）+ **空作用域 ⇒ 不写快照**"
                  "（`MB-Q11`：隐私最小、宁缺勿滥）", "")

    # ---- ④ 日志零明文（**值**不进日志；名字 / 域保留可诊断 —— step 4 口径）----
    since = _log_line_count()
    store.save(seeded, reason="log-probe", force=True)
    added = max(1, _log_line_count() - since)
    tail_text = "\n".join(redact.tail(added))
    values_only = [row["value"] for row in seeded]
    log_hits = session.scan_plaintext(tail_text.encode("utf-8"), values_only)
    counter.check(not log_hits,
                  "VB2-39④ 日志零明文：`snapshot_saved` 只记名字 / 计数 / 域 ⇒ **Cookie 值不进日志**",
                  "命中：%s · 尾部 %d 行" % (log_hits, added))

    # ---- ⑤ 损坏 / 篡改 / 版本不识 / 明文冒充 → 不崩 + 如实 reason（不回退明文）----
    raw = store.path.read_bytes()
    mid = bytearray(raw)
    mid[len(raw) // 2] ^= 0xFF
    tail = bytearray(raw)
    tail[-1] ^= 0xFF
    future = session.protect(json.dumps(
        dict(session.snapshot_payload(seeded, hosts=[host]), v=99)).encode("utf-8"))
    cases: Tuple[Tuple[str, bytes, str], ...] = (
        ("篡改中段", bytes(mid), "corrupt"),
        ("篡改末尾", bytes(tail), "corrupt"),
        ("截断一半", raw[:max(1, len(raw) // 2)], "corrupt"),
        ("明文 JSON 冒充密文", b'{"v":1,"cookies":[]}', "corrupt"),
        ("空文件", b"", "corrupt"),
        ("版本不识（v=99）", future, "version_unknown"),
    )
    bad: List[str] = []
    for index, (label, payload, want) in enumerate(cases):
        probe = session.SnapshotStore(pathlib.Path(temp) / ("broken-%d.dat" % index))
        probe.path.write_bytes(payload)
        loaded = probe.load()
        if loaded["ok"] or loaded["reason"] != want:
            bad.append("%s ⇒ ok=%s reason=%s（期望 %s）"
                       % (label, loaded["ok"], loaded["reason"], want))
    counter.check(not bad,
                  "VB2-39⑤ 损坏 / 篡改 / 截断 / 版本不识 / 明文冒充 / 空文件 → **不崩 + 如实 reason**"
                  "（一律当「没有」处理，**绝不回退明文**；%d 例）" % len(cases),
                  "；".join(bad))


async def _session_online(counter: "_Counter", *, headless: bool, timeout_s: float) -> None:
    """组 B（真机）：注入合成登录态 → **存 → 清空 → 回灌** → 干净退出 + 残留 0。

    目标域用 **RFC 2606 保留域名**（`session-selftest.invalid`）⇒ **零外网请求、零副作用**；
    回灌只进浏览器 Cookie 库（`Storage.setCookies`），不发起任何网络请求。
    """
    temp = pathlib.Path(tempfile.mkdtemp(prefix="m7b-session-online-"))
    host = "session-selftest.invalid"
    store = _session_probe_store(temp, host)
    seeded = _session_seed_cookies(host)
    before = _stray_browsers()
    drv: Optional["driver.BrowserDriver"] = None
    print("[L2 快照] 组 B：真机往返（临时目录 %s · 域 %s）" % (temp, host))
    try:
        drv = await driver.start(headless=headless, start_timeout=timeout_s)

        written = await drv.set_cookies(session.to_cdp_params(seeded))
        in_library = await drv.cookies_all()
        names_now = sorted({str(row.get("name")) for row in in_library})
        counter.check(written == len(seeded) and "aiwrite_selftest_sid" in names_now,
                      "VB2-39⑥ 注入合成登录态：`Storage.setCookies` 写 %d 条 → **浏览器级** "
                      "`Storage.getCookies` 读回（全 origin；未导航也成立）" % written,
                      "库内名字=%s" % names_now)

        saved = store.save(in_library, reason="online")
        blob = store.path.read_bytes() if store.exists() else b""
        secrets = [row["name"] for row in seeded] + [row["value"] for row in seeded]
        hits = session.scan_plaintext(blob, secrets)
        counter.check(bool(saved.get("ok")) and saved.get("count") == len(seeded)
                      and store.exists() and not hits,
                      "VB2-39⑦ 存快照：%s 条 → DPAPI 密文落盘（%d 字节）+ 文件内**零明文**"
                      % (saved.get("count"), len(blob)),
                      "命中=%s · 证据=%s" % (hits, saved))

        await drv.delete_all_cookies()
        cleared = await drv.cookies_for_domain(host)
        counter.check(cleared == [],
                      "VB2-39⑧ 注销：`Storage.clearCookies` → 库内该域 Cookie **清零**"
                      "（先清空，「回灌」才有意义）", "剩余=%d" % len(cleared))

        loaded = store.load()
        restored = await drv.set_cookies(session.to_cdp_params(loaded["cookies"]))
        after = await drv.cookies_for_domain(host)
        after_names = sorted({str(row.get("name")) for row in after})
        http_only = {str(row.get("name")): bool(row.get("httpOnly")) for row in after}
        counter.check(bool(loaded.get("ok")) and restored == len(seeded)
                      and after_names == sorted({str(row["name"]) for row in seeded})
                      and http_only.get("aiwrite_selftest_sid") is True,
                      "VB2-39⑨ 回灌：解密 → `Storage.setCookies` 写回 %d 条；名字齐 + "
                      "**httpOnly 保持**（真·登录态，不是降级副本）" % restored,
                      "库内=%s · http_only=%s" % (after_names, http_only))

        # ⚠️ 实测（2026-10-03）：**close 之前刚写过 Cookie** 时，Chrome 退出可能 > 5 s（要 flush
        # cookie 库）⇒ 判据改用**生产同款** `daemon.close_verdict`（超时 + warn + 未强杀 =
        # 可恢复状态 ⇒ 通过），而不是「必须 5 s 内退出」。等待放宽到 10 s 只为减少噪音，判据不放水。
        closed = await drv.close_wait(max(10.0, driver.DEFAULT_CLOSE_WAIT_S))
        verdict_ok, verdict_reason = daemon.close_verdict({
            "close_requested": True,
            "browser_not_running": False,
            "waited_s": closed.get("waited_s"),
            "warning": closed.get("warning", ""),
            "fallback_kill": bool(closed.get("fallback_kill")),
        })
        counter.check(verdict_ok and closed.get("exit_code") in (0, None),
                      "VB2-39⑩ 关闭协议（`I23①` / `VB2-38` **同款判据** `close_verdict`）：%s"
                      "（waited_s=%s · exit_code=%s · warn=%s）"
                      % (verdict_reason, closed.get("waited_s"), closed.get("exit_code"),
                         "有（>5 s，如实留痕）" if closed.get("warning") else "无"),
                      str(closed))
        counter.check(store.exists(),
                      "VB2-39⑪ 快照留盘：退出后文件仍在（`%s`）⇒ 「下次启动可回灌」成立"
                      % store.path.name, str(store.path))
        after_strays = _stray_browsers()
        counter.check(len(after_strays) <= len(before),
                      "VB2-39⑫ 收尾无残留（本 profile 进程：前 %d → 后 %d；只读探测，**不杀**）"
                      % (len(before), len(after_strays)), str(after_strays)[:200])
    finally:
        if drv is not None and drv.alive():
            leftover = await drv.close_wait(driver.DEFAULT_CLOSE_WAIT_S)
            print("[L2 快照] 兜底关闭（异常路径）：%s" % (leftover,))


def _session_selftest(counter: "_Counter", *, headless: bool, timeout_s: float) -> None:
    """step 5 自检：组 A（`VB2-39①~⑤` · 离线）+ 组 B（真机「存 → 弃 → 回灌」· 需 pydoll + 浏览器）。"""
    real_file = session.SESSION_DIR / session.SNAPSHOT_NAME
    real_before = real_file.read_bytes() if real_file.is_file() else None
    temp = pathlib.Path(tempfile.mkdtemp(prefix="m7b-session-selftest-"))
    os.environ[session.ENV_SESSION_DIR] = str(temp)     # 双保险：任何「默认路径」都落临时目录
    try:
        _session_offline_checks(counter, temp)

        info = runtime.check_runtime()
        if not info["ok"]:
            print("   ----  组 B（真机）：SKIP —— 运行时依赖缺失：%s"
                  % (info["hint"] or "pydoll 不可用"))
            print("   ----  说明：组 B 需要 pydoll；**这不是失败**，但也**不假装通过**（`I21` 同族）")
        elif browsers.detect_browser()[1] is None:
            print("   ----  组 B（真机）：SKIP —— 本机无 Chrome / Edge：%s"
                  % runtime.NO_BROWSER_HINT)
        else:
            print("[L2 快照] 组 B：真机（**会弹真实浏览器窗口**；headless=%s；预计 ~15 s）"
                  % headless)
            if headless:
                print("   ----  ⚠️ headless 仅自检提速；生产口径是 **有头窗口**（合规 §13）")
            asyncio.run(_session_online(counter, headless=headless, timeout_s=timeout_s))
    finally:
        os.environ.pop(session.ENV_SESSION_DIR, None)
    real_after = real_file.read_bytes() if real_file.is_file() else None
    counter.check(real_before == real_after,
                  "VB2-39⑬ 自检**不污染真实快照**：`" + str(real_file)
                  + "` 自检前后字节一致（组 A / 组 B 全在临时目录里跑）", "")


_SELFTEST_FLAGS: Tuple[str, ...] = ("--selftest", "--stub-selftest", "--pipe-selftest",
                                    "--driver-selftest", "--daemon-selftest",
                                    "--session-selftest")


def main(argv: List[str]) -> int:
    args = [a for a in argv[1:] if a]
    if not args or any(a in ("-h", "--help") for a in args):
        print(__doc__)
        return 0

    flags: List[str] = []
    pipe_name = ""
    once = False
    idle_timeout_s = 30.0
    headless = False
    timeout_given = False
    driver_timeout_s = driver.DEFAULT_START_TIMEOUT_S
    index = 0
    while index < len(args):
        arg = args[index]
        index += 1
        if arg in _SELFTEST_FLAGS or arg == "--serve":
            flags.append(arg)
        elif arg == "--once":
            once = True
        elif arg == "--headless":
            headless = True
        elif arg in ("--pipe-name", "--idle-timeout", "--timeout"):
            if index >= len(args):
                print("参数 %s 缺少取值" % arg, file=sys.stderr)
                return 2
            value = args[index]
            index += 1
            if arg == "--pipe-name":
                pipe_name = value
            else:
                try:
                    seconds = float(value)
                except ValueError:
                    print("%s 取值非法：%s（应为秒数）" % (arg, value), file=sys.stderr)
                    return 2
                if seconds <= 0:
                    print("%s 必须 > 0" % arg, file=sys.stderr)
                    return 2
                if arg == "--timeout":
                    driver_timeout_s = seconds
                    timeout_given = True
                else:
                    idle_timeout_s = seconds
        else:
            print("未知参数：%s" % arg, file=sys.stderr)
            print(__doc__, file=sys.stderr)
            return 2

    if "--serve" in flags:
        if len(flags) > 1:
            print("--serve 不能与其他自检开关并用", file=sys.stderr)
            return 2
        if timeout_given:
            print("--timeout 仅用于 --driver-selftest / --daemon-selftest（不静默忽略）",
                  file=sys.stderr)
            return 2
        return _serve(pipe_name or pipe.daemon_pipe_name(), once, idle_timeout_s, headless)

    browser_selftests = ("--driver-selftest", "--daemon-selftest", "--session-selftest")
    if not any(flag in flags for flag in browser_selftests) and (headless or timeout_given):
        print("--headless / --timeout 仅用于 --driver-selftest / --daemon-selftest / "
              "--session-selftest（不静默忽略）", file=sys.stderr)
        return 2

    if not flags:
        print("未指定模式：需 --selftest / --stub-selftest / --pipe-selftest / "
              "--driver-selftest / --daemon-selftest / --session-selftest / --serve",
              file=sys.stderr)
        print(__doc__, file=sys.stderr)
        return 2

    stub = "--stub-selftest" in flags
    print("brain_ai_browser %s（协议 v%d）%s"
          % (PACKAGE_VERSION, PROTO_VERSION, " · 桩模式" if stub else ""))
    counter = _Counter()
    _protocol_selftest(counter)
    if "--pipe-selftest" in flags:
        _pipe_selftest(counter)
    if "--driver-selftest" in flags:
        _driver_selftest(counter, headless=headless, timeout_s=driver_timeout_s)
    if "--daemon-selftest" in flags:
        _daemon_selftest(counter, headless=headless, timeout_s=driver_timeout_s)
    if "--session-selftest" in flags:
        _session_selftest(counter, headless=headless, timeout_s=driver_timeout_s)
    if stub:
        print("   ----  桩边界（**v4 起**）：全部 10 条命令均已实现；`upload_image` 的**网络回执证据**"
              "（`evidence.both=true`）归 `P7b-11`（当前**如实**回 `both=false`）；"
              "`stream_deltas`（CDP 增量 · `M7B-21`）与 `dom_chat` 生产切换（**P4**）"
              "（含 C++ 侧接线已落地的 `channel::{send_prompt,read_answer,upload_image}`）")
    print("=== 自检结果: %d 通过 / %d 失败 ===" % (counter.passed, counter.failed))
    return 0 if counter.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
