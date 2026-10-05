"""管道协议 v1 词表（`docs/actionPlan/M7B.md` §6.1 的**唯一实现**）。

帧格式（两侧一致）
    每帧 = **UTF-8 JSON 对象，一行一帧**（`\n` 结尾，帧内无裸换行）；必带
    `v`（整数，本文件 = 4）· `id`（字符串，请求-响应配对；事件帧用 `"-"`）· `kind`（cmd | evt | err）。

非法行（`VB2-29`）
    JSON 不合法 / 缺 `v` 或 `kind` / `v` 不识别 / 超过单帧上限
    → **丢弃 + 记日志 + 回 `err {bad_frame}`**，**不得**让守护进程退出。

容量与超时（写死，防膨胀）
    单帧 ≤ 64 KiB；`images[]` ≤ 8（对齐 P7-a 图片缓存上限）；
    **图片一律传路径、不传 base64**（会撞单帧上限且重复编码）。

本模块**只用标准库**（`json` / `dataclasses` / `typing`），不新增第三方依赖。
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple, Union

PROTO_VERSION = 4

# ---- 容量与超时上限（§6.1「容量与超时（写死，防膨胀）」）----
MAX_FRAME_BYTES = 64 * 1024
MAX_IMAGES = 8
TIMEOUT_INJECT_MS = 30_000    # 注入 / 上传
TIMEOUT_EVIDENCE_MS = 60_000  # 上传完成证据（P7b-11）
TIMEOUT_ANSWER_MS = 120_000   # 回答
SHUTDOWN_GRACE_MS = 5_000     # I23①：Browser.close → 等进程退出

# ---- 帧 ----
EVENT_ID = "-"
KIND_CMD = "cmd"
KIND_EVT = "evt"
KIND_ERR = "err"
KNOWN_KINDS: Tuple[str, ...] = (KIND_CMD, KIND_EVT, KIND_ERR)

# ---- 命令词表（§6.1 表 1；值 = **必需字段**）----
COMMAND_REQUIRED_FIELDS: Dict[str, Tuple[str, ...]] = {
    "hello": (),                                    # 握手；回 ready {proto, python, browser}
    "open_tab": ("provider",),                      # 可选 url（缺省取条目 web.login_url）
    "login_state": ("provider",),                   # 回 login {state, cookie_names, has_expires, http_only}
    "upload_image": ("provider", "images"),         # 可选 attach / attach_selector / timeout_ms；**只注入、不发提示词**（I18）
    "send_prompt": ("provider", "prompt", "input_selector", "send"),  # **v4**：真打字注入 + 按 send 触发
    "read_answer": ("provider", "answer_selector"),  # **v4**：轮询回答 → answer_done {text, text_bytes?, truncated?}
    "run_script": ("provider", "script"),           # **批 3 step 8**：页面内执行诊断 JS → 回 script_done {result}
    "logout_site": ("provider",),                   # **v3（批 3 step 11）**：按站点注销（Storage.clearDataForOrigin）
    "current_tab": (),                              # **v3（批 3 step 11）**：读当前 tab → 回 tab {url, site}
    "shutdown": (),                                 # 可选 grace_ms（默认 5000）；**禁止 close 后立刻 kill**
}
KNOWN_COMMANDS: Tuple[str, ...] = tuple(COMMAND_REQUIRED_FIELDS)

# ---- v4 站字段组（`send_prompt` / `read_answer` / `upload_image` 共用；§6.1 v4 设计定稿）----
#  * 选择器 / 发送方式一律**由调用方下发**（`I14`：Python 侧不读条目表、无回落）
#  * `send` = `{"kind": "key" | "click", "value": "<键名 / CSS 选择器>"}`
#  * `done_when` = `{"kind": "selector_present" | "selector_gone", "selector": "<可选>"}`
STATION_SELECTOR_FIELDS: Tuple[str, ...] = ("input_selector", "answer_selector")
SEND_KINDS: Tuple[str, ...] = ("key", "click")
DONE_KINDS: Tuple[str, ...] = ("selector_present", "selector_gone")

# ---- 事件词表（§6.1 表 2；值 = 字段）----
#  * ⚠️ `stage` 事件的**阶段标识**放在载荷键 `stage`（不是 `name`）—— 帧头 `name` 已被
#    「事件名」占用，两者在同层 JSON 里**无法共存**（2026-10-03 实现期暴露的 §6.1 歧义，
#    已同批回填文档）。
EVENT_FIELDS: Dict[str, Tuple[str, ...]] = {
    "stage": ("stage", "ok"),                       # stage ∈ open | login | inject | evidence | send | answer | script | close
    "evidence": ("page", "network", "both"),        # P7b-11 双证据载体（判据值由 P7b-05b B2 回填）
    "delta": ("seq", "text"),                       # CDP 增量帧（M7B-21）
    "answer_done": ("text",),                       # 可选 http_status
    "script_done": ("result", "truncated"),        # **批 3 step 8**：`run_script` 回包（result = 脚本返回值；超大 → 截断 + `truncated`）
    "tab": ("url", "site"),                        # **v3（批 3 step 11）**：`current_tab` 回包（当前 tab 的 url / 站点键）
    "error": ("code", "hint"),                      # 必须可操作（I21）
    "ready": ("proto", "python", "browser"),        # hello 的回包
}
KNOWN_EVENTS: Tuple[str, ...] = tuple(EVENT_FIELDS)

# ---- 错误码（→ I21 行为映射）----
BAD_FRAME = "bad_frame"
ERROR_CODES: Tuple[str, ...] = (
    BAD_FRAME,             # 非法帧（丢弃 + 记日志，**不退出**）
    "no_python",           # 依赖缺失（不换通道、不回落）
    "no_browser",          # 同上
    "daemon_down",         # 同上
    "not_logged_in",       # 引导登录（M7B-06b 口径）
    "no_upload_evidence",  # I18：无上传证据不得发送
    "attach_unsupported",  # P7b-12 错误条两个显式按钮之一
    "upload_timeout",      # 同上（超时 = 可操作错误，不假装完成）
    "send_timeout",        # 同上
    "script_error",        # **批 3 step 8**：脚本执行失败（语法 / 超时 / 页面未就绪 —— 一律带可操作 hint）
    "not_implemented",     # **v3（批 3 step 11）**：词表内但本步未实现的命令（**闭合开口项 MB-Q7**：
                           # 原借 `daemon_down`，语义不符，易被 UI 误读为「守护进程挂了」）
)

# ---- 上传入口形态（M7.md D11；只读自条目、**无回落**，I14）----
ATTACH_KINDS: Tuple[str, ...] = ("auto", "file_input", "drop_zone", "paste_only", "none")

@dataclass
class Frame:
    """合法帧。`payload` = 除 `v` / `kind` / `id` / `name` 之外的字段。"""

    v: int
    kind: str
    id: str
    name: str = ""
    payload: Dict[str, Any] = field(default_factory=dict)

    def is_command(self) -> bool:
        return self.kind == KIND_CMD

    def is_event(self) -> bool:
        return self.kind == KIND_EVT

    def is_error(self) -> bool:
        return self.kind == KIND_ERR

    def field(self, key: str, default: Any = None) -> Any:
        return self.payload.get(key, default)


@dataclass
class BadFrame:
    """非法行（须丢弃 + 记日志 + 回 `err {bad_frame}`，**不退出**）。"""

    reason: str
    raw: str = ""


ParsedLine = Union[Frame, BadFrame]


def _dump(obj: Dict[str, Any]) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def frame_within_limit(line: str) -> bool:
    """单帧字节上限（UTF-8 计）。"""
    return len(line.encode("utf-8")) <= MAX_FRAME_BYTES



def parse_line(line: str) -> ParsedLine:
    """一行 → `Frame`（合法）或 `BadFrame`（非法 + 可操作原因）。"""
    text = line.rstrip("\r\n")
    if text.strip() == "":
        return BadFrame("空行（帧内不得有裸换行）", line)
    if not frame_within_limit(text):
        return BadFrame("超过单帧上限 %d 字节" % MAX_FRAME_BYTES, text[:120])
    try:
        obj = json.loads(text)
    except ValueError as exc:
        return BadFrame("JSON 不合法：%s" % exc, text[:200])
    if not isinstance(obj, dict):
        return BadFrame("帧必须是 JSON 对象", text[:200])

    if "v" not in obj:
        return BadFrame("缺少字段 v", text[:200])
    version = obj["v"]
    if isinstance(version, bool) or not isinstance(version, int):
        return BadFrame("v 必须是整数", text[:200])
    if version != PROTO_VERSION:
        return BadFrame("v 不识别：%d（本机支持 %d）" % (version, PROTO_VERSION), text[:200])

    if "kind" not in obj:
        return BadFrame("缺少字段 kind", text[:200])
    kind = obj["kind"]
    if kind not in KNOWN_KINDS:
        return BadFrame("kind 非法：%s（应为 cmd | evt | err）" % kind, text[:200])

    frame_id = obj.get("id", "")
    if not isinstance(frame_id, str):
        return BadFrame("id 必须是字符串", text[:200])
    if kind != KIND_ERR and frame_id == "":
        return BadFrame("缺少字段 id（请求-响应配对；事件帧用 \"-\"）", text[:200])

    name = obj.get("name", "")
    if not isinstance(name, str):
        return BadFrame("name 必须是字符串", text[:200])

    payload = {key: value for key, value in obj.items()
               if key not in ("v", "kind", "id", "name")}
    return Frame(v=version, kind=kind, id=frame_id, name=name, payload=payload)


def encode_command(command: str, frame_id: str, **fields: Any) -> str:
    """构造命令帧（C++ → Python）。`fields` = §6.1 表 1 的命令字段。"""
    obj: Dict[str, Any] = {"v": PROTO_VERSION, "kind": KIND_CMD, "id": frame_id, "name": command}
    obj.update(fields)
    return _dump(obj)


def encode_event(event: str, frame_id: str = EVENT_ID, **fields: Any) -> str:
    """构造事件帧（Python → C++；`id` 缺省 `"-"`）。`fields` = §6.1 表 2 的事件字段。"""
    obj: Dict[str, Any] = {"v": PROTO_VERSION, "kind": KIND_EVT, "id": frame_id, "name": event}
    obj.update(fields)
    return _dump(obj)


def encode_error(code: str, frame_id: str = EVENT_ID, hint: str = "") -> str:
    """构造错误帧（**必须可操作**，`I21`）；`code` 取 `ERROR_CODES`。"""
    obj: Dict[str, Any] = {"v": PROTO_VERSION, "kind": KIND_ERR, "id": frame_id, "name": code}
    if hint:
        obj["hint"] = hint
    return _dump(obj)


def is_known_command(name: str) -> bool:
    return name in COMMAND_REQUIRED_FIELDS


def required_fields_of(command: str) -> Tuple[str, ...]:
    return COMMAND_REQUIRED_FIELDS.get(command, ())


def validate_command(frame: Frame) -> Optional[str]:
    """命令帧校验 → `None` = 通过；否则返回**可操作原因**（未知命令 / 缺少字段 / 取值非法）。"""
    if frame.kind != KIND_CMD:
        return "不是命令帧（kind=%s）" % frame.kind
    if not is_known_command(frame.name):
        return "未知命令 %s（已知：%s）" % (frame.name, " / ".join(KNOWN_COMMANDS))
    missing: List[str] = [name for name in required_fields_of(frame.name)
                          if name not in frame.payload]
    if missing:
        return "缺少字段 %s（命令 %s 必需）" % (" / ".join(missing), frame.name)
    if frame.name == "upload_image":
        images = frame.payload.get("images")
        if not isinstance(images, list) or not images:
            return "缺少字段 images（命令 upload_image 需要**本地绝对路径**数组）"
        if len(images) > MAX_IMAGES:
            return "images 超过上限 %d（收到 %d）" % (MAX_IMAGES, len(images))
        for item in images:
            if not isinstance(item, str) or not item:
                return "images 元素必须是**路径字符串**（不传 base64）"
    attach = frame.payload.get("attach")
    if attach is not None and attach not in ATTACH_KINDS:
        return "attach 取值非法：%s（应为 %s）" % (attach, " | ".join(ATTACH_KINDS))
    if frame.name in ("send_prompt", "read_answer", "upload_image"):
        return station_fields_reason(frame)
    return None


def selector_list_reason(value: Any, field_name: str) -> Optional[str]:
    """选择器字段（多候选）→ 可操作原因（`None` = 合法）。"""
    if not isinstance(value, list) or not value:
        return "字段 %s 必须是**非空字符串数组**（多候选逐个探测，首个可见且命中者胜）" % field_name
    for item in value:
        if not isinstance(item, str) or not item.strip():
            return "字段 %s 的元素必须是**非空字符串**（CSS 选择器）" % field_name
    return None


def station_fields_reason(frame: Frame) -> Optional[str]:
    """**v4** 站字段组校验（`send_prompt` / `read_answer` / `upload_image`）→ `None` = 通过。

    * 选择器（`input_selector[]` / `answer_selector[]`）必须是**非空字符串数组**
    * `send` = `{"kind": "key" | "click", "value": "<键名 / CSS 选择器>"}`（`send_prompt` 必需）
    * `done_when` = `{"kind": "selector_present" | "selector_gone", "selector": "<可选>"}`
    * `poll_ms` / `max_polls` = 整数；`upload_evidence` = 布尔（`I18` **按位开关**）
    """
    for field_name in STATION_SELECTOR_FIELDS:
        if field_name in frame.payload:
            reason = selector_list_reason(frame.payload.get(field_name), field_name)
            if reason is not None:
                return reason
    if frame.name == "send_prompt":
        send = frame.payload.get("send")
        if not isinstance(send, dict):
            return "字段 send 必须是对象 {kind, value}（kind = key | click）"
        if str(send.get("kind") or "") not in SEND_KINDS:
            return "字段 send.kind 取值非法：%r（应为 %s）" % (
                send.get("kind"), " | ".join(SEND_KINDS))
        value = send.get("value")
        if not isinstance(value, str) or not value.strip():
            return "字段 send.value 必须是**非空字符串**（按键名或 CSS 选择器）"
    done = frame.payload.get("done_when")
    if done is not None:
        if not isinstance(done, dict):
            return "字段 done_when 必须是对象 {kind, selector?}"
        if str(done.get("kind") or "") not in DONE_KINDS:
            return "字段 done_when.kind 取值非法：%r（应为 %s）" % (
                done.get("kind"), " | ".join(DONE_KINDS))
    for field_name in ("poll_ms", "max_polls"):
        value = frame.payload.get(field_name)
        if value is not None and (isinstance(value, bool) or not isinstance(value, int)):
            return "字段 %s 必须是整数（毫秒 / 轮数）" % field_name
    evidence = frame.payload.get("upload_evidence")
    if evidence is not None and not isinstance(evidence, bool):
        return "字段 upload_evidence 必须是**布尔**（I18 按位开关：仅本次运行**有图**才置位）"
    return None


def delta_text_of(payload: Dict[str, Any]) -> Tuple[str, bool]:
    """`delta` 事件载荷 → `(文本, 是否必须显式标注「非流式」)`。

    * 形态 A：`{"seq":N,"text":"…"}`
    * 形态 B：`{"delta":{"text":"…"}}`
    * 无可用增量 → `("", True)` ⇒ 调用方**必须显式标注「非流式（轮询）」**（`I22`）。
    """
    if not isinstance(payload, dict):
        return "", True
    text = payload.get("text")
    if isinstance(text, str) and text:
        return text, False
    nested = payload.get("delta")
    if isinstance(nested, dict):
        inner = nested.get("text")
        if isinstance(inner, str) and inner:
            return inner, False
    return "", True


def delta_text_of_frame(payload_json: str) -> Tuple[str, bool]:
    """同 `delta_text_of`，但输入为**载荷 JSON 文本**（供离线桩与 C++ 侧同构断言）。"""
    if not payload_json.strip():
        return "", True
    try:
        payload = json.loads(payload_json)
    except ValueError:
        return "", True
    return delta_text_of(payload if isinstance(payload, dict) else {})


