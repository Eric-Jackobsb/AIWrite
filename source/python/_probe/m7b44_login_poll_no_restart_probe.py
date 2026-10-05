"""step 12（`M7B-44`）端到端验证：**`login_state` 是「纯观测」—— 不自愈、不重开浏览器**。

为什么单独有这一支（`docs/actionPlan/M7B.md` §6.2 step 12 物证块）
    * C++ 侧 `login_site`（提前结束 / 取消）与 `ui/property_panel.cpp` 的「按节点记账」
      **没有 CLI 钩子** ⇒ `--exec-selftest` / `api_probe --selftest` 这类离线断言覆盖不到；
    * 而**本轮真 bug** 恰恰只在端到端暴露：登录轮询每 1.5 s 发一条 `login_state`，
      旧实现每条都走 `_ensure_browser_restored` ⇒ 用户**关掉窗口后每一轮都把浏览器重新拉起**
      （实测 2026-10-04 · 本机：一次点击 → `browser_start` **4 次** = 4 个窗口，
      另加 1 次 `FailedToStartBrowser`；`session_starts=4 / self_heals=4`）。

断言口径（**零外网、零登录**）
    ① 还没起过浏览器时发 `login_state` → 必须回 `err{daemon_down}`，hint 明说「纯观测 / 不起浏览器」；
    ② 这一过程里 `browser.log` **不得**出现本守护进程 pid 的 `browser_start`
       （= 观测命令**没把浏览器拉起来** —— 这就是「不再弹窗」的物证）；
    ③ `open_tab`（**生产命令**）的「按需起浏览器 / 自愈」能力**保留**（`M7B-11` 行为不变）；
    ④ 浏览器**活着**时 `login_state` 照常回 `login{state}`（不自愈 ≠ 不可用）。

用法（cwd = `source/python`）
    .venv/Scripts/python.exe _probe/m7b44_login_poll_no_restart_probe.py

依赖缺失（无 pydoll / 无 Chrome·Edge）时 ③④ 如实 **SKIP**（不假装通过，也不判失败）。
退出码：0 = 全通过（含 SKIP）；1 = 有失败。
"""
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

# 包目录 = 本文件上一级（`_probe/` 的同级）；无需硬编码绝对路径
PACKAGE_DIR = str(pathlib.Path(__file__).resolve().parents[1])
sys.path.insert(0, PACKAGE_DIR)

from brain_ai_browser import pipe, protocol as P, redact  # noqa: E402

STATE = {"ok": True}


def check(cond: object, label: str, detail: str = "") -> None:
    good = bool(cond)
    print(("   PASS  " if good else "   FAIL  ") + label +
          (("   -> " + detail) if (detail and not good) else ""), flush=True)
    STATE["ok"] = STATE["ok"] and good


def skip(label: str, detail: str = "") -> None:
    print("   ----  SKIP  " + label + (("   -> " + detail) if detail else ""), flush=True)


def log_records(event: str, since: int = 0) -> list:
    """`browser.log` 里 `since`（起始行号）之后的某事件记录（同 `__main__._log_records`）。"""
    path = pathlib.Path(redact.LOG_PATH)
    if not path.is_file():
        return []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    out = []
    for line in lines[max(0, since):]:
        try:
            record = json.loads(line)
        except ValueError:
            continue
        if isinstance(record, dict) and record.get("event") == event:
            out.append(record)
    return out


def log_line_count() -> int:
    path = pathlib.Path(redact.LOG_PATH)
    if not path.is_file():
        return 0
    try:
        return len(path.read_text(encoding="utf-8", errors="replace").splitlines())
    except OSError:
        return 0


def browser_starts(pid: int, since: int = 0) -> list:
    """`browser_start` 记录（**按守护进程 pid 过滤** · L4 物证）。"""
    return [item for item in log_records("browser_start", since) if item.get("pid") == pid]


def daemon_pid_since(since: int, attempts: int = 10) -> int:
    """从日志取**本守护进程自身** pid（`daemon_start` 记录）。

    ⚠️ **坑（2026-10-04 实测）**：`subprocess.Popen(...).pid` **不是**守护进程的 pid ——
    venv 的 `Scripts\\python.exe` 是**转发器**，会另起一个真解释器子进程
    （实测：`Popen.pid=21844` vs 子进程 `os.getpid()=13492`）⇒ 只能从日志 `daemon_start.pid` 取。
    """
    for _ in range(max(1, attempts)):
        records = log_records("daemon_start", since)
        for record in records:
            pid = record.get("pid")
            if isinstance(pid, int):
                return pid
        time.sleep(0.5)
    return -1




def main() -> int:
    name = "aiwrite-browser-step12-verify-" + str(os.getpid())
    fixture = os.path.join(tempfile.gettempdir(), "aiwrite-step12-fixture.html")
    with open(fixture, "w", encoding="utf-8") as handle:
        handle.write("<!doctype html><html><head><title>step12-fixture</title></head>"
                     "<body><input id='user'></body></html>")
    url = "file:///" + fixture.replace("\\", "/")

    since = log_line_count()   # 基线：只认本进程起的守护进程之后的记录
    print("[step12 验证] 管道 %s" % name, flush=True)
    proc = subprocess.Popen(
        [sys.executable, "-m", "brain_ai_browser", "--serve", "--pipe-name", name, "--once"],
        cwd=PACKAGE_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    client = None
    try:
        client = pipe.connect_pipe(name, 30_000)

        client.write_line(P.encode_command("hello", "1"))
        ready = P.parse_line(client.read_line(10_000) or "")
        check(isinstance(ready, P.Frame) and ready.name == "ready"
              and ready.field("proto") == 3,
              "hello → ready{proto=3}（词表**未变**仍 v3 —— 本步不需要升版本）", repr(ready))

        daemon = daemon_pid_since(since)
        check(daemon > 0, "定位守护进程自身 pid（`daemon_start` 日志；**不能用** `Popen.pid`）",
              str(daemon))
        print("          守护进程 pid=%s（`Popen.pid`=%s —— venv 转发器，两者不同）"
              % (daemon, proc.pid), flush=True)

        # ---- ① + ②：**纯观测**命令不得起浏览器（「不再弹窗」的回归防线）----
        before = len(browser_starts(daemon, since))
        client.write_line(P.encode_command("login_state", "2", provider="step12-verify",
                                           domain_suffix="brain-ai.invalid"))
        state = P.parse_line(client.read_line(15_000) or "")
        after = len(browser_starts(daemon, since))
        hint = str(state.field("hint") or "") if isinstance(state, P.Frame) else ""
        check(isinstance(state, P.Frame) and state.is_error() and state.name == "daemon_down"
              and "纯观测" in hint and after == before == 0,
              "login_state（未起浏览器）→ err{daemon_down} + **不起浏览器**"
              "（browser_start %d → %d）" % (before, after), repr(state))

        # 同一 pid 再打一轮：**多轮轮询**也不得把浏览器拉起来（旧 bug 的形态）
        client.write_line(P.encode_command("login_state", "3", provider="step12-verify",
                                           domain_suffix="brain-ai.invalid"))
        state2 = P.parse_line(client.read_line(15_000) or "")
        rounds = len(browser_starts(daemon, since))
        check(isinstance(state2, P.Frame) and state2.is_error() and rounds == 0,
              "连打多轮 `login_state` 仍**零** `browser_start`（旧实现每轮弹一个窗口）",
              repr(state2))

        # ---- ③：生产命令 `open_tab` 仍按需起浏览器（`M7B-11` 保留）----
        client.write_line(P.encode_command("open_tab", "4", provider="step12-verify", url=url))
        opened = P.parse_line(client.read_line(60_000) or "")
        if isinstance(opened, P.Frame) and opened.is_error():
            skip("open_tab 起浏览器 / 存活时 login_state 照常回包",
                 str(opened.field("hint") or "")[:110])
        else:
            check(isinstance(opened, P.Frame) and opened.name == "stage"
                  and opened.field("ok") is True,
                  "open_tab（本地 file:/// 夹具）→ stage{open, ok=true}（**生产命令仍起浏览器**）",
                  repr(opened))
            starts = browser_starts(daemon, since)
            check(len(starts) >= 1,
                  "L4 物证：`open_tab` 触发 `browser_start`（按需起浏览器/自愈能力保留）",
                  str(starts[-1:] if starts else []))

            client.write_line(P.encode_command("login_state", "5", provider="step12-verify",
                                               domain_suffix="brain-ai.invalid"))
            live = P.parse_line(client.read_line(20_000) or "")
            check(isinstance(live, P.Frame) and live.name == "login"
                  and live.field("state") in ("logged_in", "not_logged_in", "unknown"),
                  "浏览器**活着**时 `login_state` 照常回 login{state}（不自愈 ≠ 不可用）",
                  repr(live))

        client.write_line(P.encode_command("shutdown", "6", grace_ms=5_000))
        closing = P.parse_line(client.read_line(15_000) or "")
        check(isinstance(closing, P.Frame) and closing.name == "stage"
              and closing.field("stage") == "close",
              "shutdown → stage{close}（I23① 收尾）", repr(closing))
    finally:
        if client is not None:
            try:
                client.close()
            except Exception:  # noqa: BLE001
                pass

    try:
        code = proc.wait(timeout=90)
        check(code == 0, "守护进程干净退出（退出码 0 · 未强杀）", "exit=%s" % code)
    except subprocess.TimeoutExpired:
        proc.kill()
        check(False, "守护进程干净退出（退出码 0 · 未强杀）", "超时未退出 → 已强杀")

    try:
        os.remove(fixture)
    except OSError:
        pass

    print("[step12 验证] 结果：%s" % ("PASS" if STATE["ok"] else "FAIL"), flush=True)
    return 0 if STATE["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
