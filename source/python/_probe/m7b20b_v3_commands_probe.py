"""step 11（词表 **v3**）端到端验证：`current_tab` / `logout_site` / 词表校验。

⚠️ **step 14 起本探针随词表升 v4**：① 握手断言改 `proto == 4`；② 末条断言由
「`send_prompt` → `err{not_implemented}`」（v3 的「未实现」码）改为「缺 `input_selector` / `send`
⇒ `err{bad_frame}` + 可操作原因」（v4 起三命令**已落地**，校验走站字段组）；
③ v4 内容返回（`send_prompt` / `read_answer` / `upload_image`）的端到端验证见
**`source/python/_probe/m7b54_v4_content_probe.py`**（新）。

为什么单独有这一支（`docs/actionPlan/M7B.md` §6.2 step 11 ⑧）
    * C++ 侧 `logout_site` / `current_tab_site` / `tab_on_site` **没有 CLI 钩子**（只有参数面板在用）
      ⇒ `--exec-selftest` / `python -m brain_ai_browser --selftest` 这类**离线断言覆盖不到**它们；
    * 而 `driver.clear_origin_data` 的**传输层要求反直觉**，只有「真发一条 `logout_site`」才会暴露 ——
      实测（2026-10-04 · 本机）：`Storage.clearDataForOrigin` **必须走 tab（页）连接**；
      走**浏览器级**连接时 Chromium 对 `storageTypes` 的**任何取值**（`all` / `cookies` /
      `local_storage` / `cookies,local_storage`）一律回 `Internal error (code -32603)`。
      ⚠️ 同一 `Storage` 域名内 `Storage.getCookies` 与 `Browser.close` 恰恰**必须**走浏览器级
      （见 `driver.cookies_all` / `driver.close_wait`）⇒ **传输层要求按命令而异，不能照抄同域邻居**。

用法（cwd = `source/python`）
    .venv/Scripts/python.exe _probe/m7b20b_v3_commands_probe.py

零副作用：`open_tab` 只去**本地 `file:///` 夹具**（零外网、零登录）；`logout_site` 清的是一个
**空 origin**（`https://example.com`）—— **不碰任何真实站点**的登录态。
退出码：0 = 全通过；1 = 有失败。
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

# 包目录 = 本文件上一级（`_probe/` 的同级）；无需硬编码绝对路径
PACKAGE_DIR = str(pathlib.Path(__file__).resolve().parents[1])
sys.path.insert(0, PACKAGE_DIR)

from brain_ai_browser import pipe, protocol as P  # noqa: E402

STATE = {"ok": True}


def check(cond: object, label: str, detail: str = "") -> None:
    good = bool(cond)
    print(("   PASS  " if good else "   FAIL  ") + label +
          (("   -> " + detail) if (detail and not good) else ""), flush=True)
    STATE["ok"] = STATE["ok"] and good


def main() -> int:
    name = "aiwrite-browser-step11-verify-" + str(os.getpid())
    fixture = os.path.join(tempfile.gettempdir(), "aiwrite-step11-fixture.html")
    with open(fixture, "w", encoding="utf-8") as handle:
        handle.write("<!doctype html><html><head><title>step11-fixture</title></head>"
                     "<body><input id='user'></body></html>")
    url = "file:///" + fixture.replace("\\", "/")

    print("[step11 验证] 管道 %s" % name, flush=True)
    proc = subprocess.Popen(
        [sys.executable, "-m", "brain_ai_browser", "--serve", "--pipe-name", name, "--once"],
        cwd=PACKAGE_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    client = None
    try:
        client = pipe.connect_pipe(name, 30_000)

        client.write_line(P.encode_command("hello", "1"))
        ready = P.parse_line(client.read_line(10_000) or "")
        check(isinstance(ready, P.Frame) and ready.name == "ready" and ready.field("proto") == 4,
              "hello → ready{proto=4}（v4 握手）", repr(ready))

        client.write_line(P.encode_command("open_tab", "2", provider="step11-verify", url=url))
        opened = P.parse_line(client.read_line(30_000) or "")
        check(isinstance(opened, P.Frame) and opened.name == "stage" and opened.field("ok") is True,
              "open_tab（本地 file:/// 夹具）→ stage{open, ok=true}", repr(opened))

        # ---- 新命令 ①：`current_tab` → `tab{url, site}`（v3）----
        client.write_line(P.encode_command("current_tab", "3"))
        tab = P.parse_line(client.read_line(15_000) or "")
        check(isinstance(tab, P.Frame) and tab.name == "tab"
              and "step11-fixture" in str(tab.field("url") or "")
              and tab.field("site") == "file://",   # site_key_of("file:///C:/…") = "file://"
              "current_tab → tab{url, site}（`site` 与 C++ site_key_of 同构）", repr(tab))

        # ---- 新命令 ②：`logout_site` → `stage{close, ok}`（v3 · 走 tab 连接）----
        client.write_line(P.encode_command("logout_site", "4", provider="step11-verify",
                                           origin="https://example.com"))
        logout = P.parse_line(client.read_line(30_000) or "")
        check(isinstance(logout, P.Frame) and logout.name == "stage"
              and logout.field("stage") == "close" and logout.field("ok") is True,
              "logout_site（Storage.clearDataForOrigin · tab 级）→ stage{close, ok=true}",
              repr(logout))

        # 缺 `origin`（运行期必给）→ 可操作错误（不静默、不假装成功）
        client.write_line(P.encode_command("logout_site", "5", provider="step11-verify"))
        missing = P.parse_line(client.read_line(15_000) or "")
        check(isinstance(missing, P.Frame) and missing.is_error() and missing.name == "daemon_down"
              and "origin" in str(missing.field("hint") or ""),
              "logout_site 缺 origin → err{daemon_down} + 可操作 hint", repr(missing))

        # ---- v4：`send_prompt` 的站字段组是**必需**（选择器 / 发送方式由调用方下发 · I14）----
        #   （v3 时这条命令回 `not_implemented`；v4 起**已落地** ⇒ 校验走 §6.1 v4 词表）
        client.write_line(P.encode_command("send_prompt", "6", provider="p", prompt="hi"))
        missing_fields = P.parse_line(client.read_line(15_000) or "")
        check(isinstance(missing_fields, P.Frame) and missing_fields.is_error()
              and missing_fields.name == "bad_frame"
              and "input_selector" in str(missing_fields.field("hint") or ""),
              "send_prompt 缺 input_selector/send → err{bad_frame} + 可操作原因（v4 词表）",
              repr(missing_fields))

        client.write_line(P.encode_command("shutdown", "7", grace_ms=5_000))
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

    print("[step11 验证] 结果：%s" % ("PASS" if STATE["ok"] else "FAIL"), flush=True)
    return 0 if STATE["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
