"""step 14（词表 **v4**）端到端验证：内容返回三命令 —— `send_prompt` / `read_answer` / `upload_image`。

为什么单独有这一支（`docs/actionPlan/M7B.md` §6.3 step 14 · P6）
    * `send_prompt`（真打字注入 + 按 `send` 触发）/ `read_answer`（轮询取正文）**只有真发命令**
      才会暴露「选择器 / 键盘 / 传输层」层面的坑（对照 step 11 的 `logout_site` 教训）；
    * `upload_image` 的 **`I18` 协议级拦截**（`send_prompt{upload_evidence:true}` 无证据 → 拒绝）
      是**纯协议行为**，离线断言看不到端到端效果；
    * 全部**零外网零登录**：`open_tab` 只去本地 `file:///` 夹具。

夹具页（本探针生成）
    `<input id="box">` + `<button id="send">` + `<div id="ans">`
    * `input` 事件计数写 `window.__inputs`（证明是**逐字符真打字**，不是 JS 一次性灌值）
    * 点 `#send` → `#ans.innerText = '答：' + box.value`（模拟站点把回答渲染进容器）

用法（cwd = `source/python`）
    .venv/Scripts/python.exe _probe/m7b54_v4_content_probe.py

退出码：0 = 全通过；1 = 有失败。
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

PACKAGE_DIR = str(pathlib.Path(__file__).resolve().parents[1])
sys.path.insert(0, PACKAGE_DIR)

from brain_ai_browser import pipe, protocol as P  # noqa: E402

STATE = {"ok": True}
PROMPT = "v4-content-ok"

FIXTURE = """<!doctype html><html><head><title>v4-content-fixture</title></head><body>
<input id="box" type="text">
<button id="send">发送</button>
<div id="ans"></div>
<script>
window.__inputs = 0;
document.getElementById('box').addEventListener('input', function () {
  window.__inputs = (window.__inputs || 0) + 1;
});
document.getElementById('send').addEventListener('click', function () {
  var box = document.getElementById('box');
  document.getElementById('ans').innerText = '\u7b54\uff1a' + box.value;
});
</script>
</body></html>"""


def check(cond: object, label: str, detail: str = "") -> None:
    good = bool(cond)
    print(("   PASS  " if good else "   FAIL  ") + label +
          (("   -> " + detail) if (detail and not good) else ""), flush=True)
    STATE["ok"] = STATE["ok"] and good


def main() -> int:
    name = "aiwrite-browser-step14-content-" + str(os.getpid())
    fixture = os.path.join(tempfile.gettempdir(), "aiwrite-step14-content.html")
    with open(fixture, "w", encoding="utf-8") as handle:
        handle.write(FIXTURE)
    url = "file:///" + fixture.replace("\\", "/")

    print("[step14 内容返回验证] 管道 %s" % name, flush=True)
    proc = subprocess.Popen(
        [sys.executable, "-m", "brain_ai_browser", "--serve", "--pipe-name", name, "--once"],
        cwd=PACKAGE_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    client = None
    try:
        client = pipe.connect_pipe(name, 30_000)

        client.write_line(P.encode_command("hello", "1"))
        ready = P.parse_line(client.read_line(10_000) or "")
        check(isinstance(ready, P.Frame) and ready.name == "ready"
              and ready.field("proto") == 4,
              "hello → ready{proto=4}（v4 握手）", repr(ready))

        client.write_line(P.encode_command("open_tab", "2", provider="step14-content", url=url))
        opened = P.parse_line(client.read_line(30_000) or "")
        check(isinstance(opened, P.Frame) and opened.name == "stage"
              and opened.field("ok") is True,
              "open_tab（本地 file:/// 夹具）→ stage{open, ok=true}", repr(opened))

        # ---- ① `I18` 协议级拦截：`upload_evidence=true` 但本会话无上传证据 → 拒绝 ----
        client.write_line(P.encode_command(
            "send_prompt", "3a", provider="step14-content", prompt=PROMPT,
            input_selector=["#box"], send={"kind": "click", "value": "#send"},
            upload_evidence=True))
        blocked = P.parse_line(client.read_line(15_000) or "")
        check(isinstance(blocked, P.Frame) and blocked.is_error()
              and blocked.name == "no_upload_evidence",
              "send_prompt{upload_evidence=true}（无上传证据）→ err{no_upload_evidence}（I18 拦截）",
              repr(blocked))

        # ---- ② `upload_image` 缺 `attach_selector` → 可操作拒绝（不静默、不假装成功） ----
        client.write_line(P.encode_command(
            "upload_image", "3b", provider="step14-content", images=[fixture]))
        no_attach = P.parse_line(client.read_line(15_000) or "")
        check(isinstance(no_attach, P.Frame) and no_attach.is_error()
              and no_attach.name == "attach_unsupported"
              and "attach_selector" in str(no_attach.field("hint") or ""),
              "upload_image 缺 attach_selector → err{attach_unsupported} + 可操作 hint",
              repr(no_attach))

        # ---- ③ `send_prompt`（真打字 + 点击发送）→ stage{send, ok}（**带 id = 完成回包**） ----
        client.write_line(P.encode_command(
            "send_prompt", "4", provider="step14-content", prompt=PROMPT,
            input_selector=["#box"], send={"kind": "click", "value": "#send"}))
        sent = P.parse_line(client.read_line(30_000) or "")
        check(isinstance(sent, P.Frame) and sent.name == "stage"
              and sent.field("stage") == "send" and sent.field("ok") is True,
              "send_prompt → stage{send, ok=true}（真打字注入 + 点击发送）", repr(sent))

        # ---- ③′ 逐字符自证：`window.__inputs` > 0（真打字，不是 JS 一次性灌值） ----
        client.write_line(P.encode_command(
            "run_script", "4b", provider="step14-content",
            script=("return { inputs: window.__inputs || 0,"
                    " value: document.getElementById('box').value };")))
        probe = P.parse_line(client.read_line(20_000) or "")
        payload = probe.field("result") if isinstance(probe, P.Frame) else None
        check(isinstance(probe, P.Frame) and probe.name == "script_done"
              and isinstance(payload, dict) and int(payload.get("inputs") or 0) > 0
              and payload.get("value") == PROMPT,
              "逐字符自证：`input` 事件 > 0 且输入框值 == 提示词（`run_script` 只读复核）",
              repr(probe))

        # ---- ④ `read_answer` → `answer_done{text, text_bytes, truncated=false}` ----
        client.write_line(P.encode_command(
            "read_answer", "5", provider="step14-content", answer_selector=["#ans"],
            poll_ms=200, max_polls=20, timeout_ms=15_000))
        answer = P.parse_line(client.read_line(30_000) or "")
        expected = "\u7b54\uff1a" + PROMPT
        check(isinstance(answer, P.Frame) and answer.name == "answer_done"
              and str(answer.field("text") or "").strip() == expected
              and int(answer.field("text_bytes") or 0) == len(expected.encode("utf-8"))
              and answer.field("truncated") is False,
              "read_answer → answer_done{text=%r, text_bytes, truncated=false}" % expected,
              repr(answer))

        # ---- ⑤ 选择器全不中 → 如实报错（不假装空答案、不回落 · I14） ----
        client.write_line(P.encode_command(
            "read_answer", "6", provider="step14-content", answer_selector=["#nope"],
            poll_ms=200, max_polls=3, timeout_ms=5_000))
        missing_answer = P.parse_line(client.read_line(20_000) or "")
        check(isinstance(missing_answer, P.Frame) and missing_answer.is_error()
              and missing_answer.name == "send_timeout",
              "read_answer 选择器未命中 → err{send_timeout} + 可操作 hint（不假装空答案）",
              repr(missing_answer))

        # ---- ⑥ `shutdown` → `stage{close}`（`I23①` 收尾） ----
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

    print("[step14 内容返回验证] 结果：%s" % ("PASS" if STATE["ok"] else "FAIL"), flush=True)
    return 0 if STATE["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
