"""日志脱敏与 L4 埋点（`M7B-11` · 批 1 step 4）。

口径（**机器可断言**，不靠自律）
    * 日志落 `~/.brain-ai/logs/browser.log`，**一行一条 JSON**（只追加）；
    * **Cookie 值 / 令牌绝不进日志**：`redact_cookies()` 只输出名字 + 长度 + 标志位；
      自由文本走 `redact_text()`（`token=…` / `Cookie: …` / 长随机串一律打码）；
    * `log_event()` 是**唯一**写日志入口 ⇒「无明文」可以被扫描断言
      （往日志里塞明文即失败，见 `--daemon-selftest` 组 A）；
    * 日志**只用于本机诊断**，不上报、不联网（`I21` 同族：出问题要看得见，但不得泄露凭据）。
"""

from __future__ import annotations

import json
import pathlib
import re
import time
from typing import Any, Dict, Iterable, List, Optional

__all__ = [
    "LOG_DIR",
    "LOG_PATH",
    "mask",
    "redact_text",
    "redact_cookies",
    "scrub",
    "log_event",
    "tail",
]

LOG_DIR = pathlib.Path.home() / ".brain-ai" / "logs"
LOG_PATH = LOG_DIR / "browser.log"

# 命中即打码的**键名**（不含 `name`/`domain` 这类元数据）
SECRET_KEYS = ("cookie", "token", "authorization", "auth", "password", "passwd",
               "secret", "api_key", "apikey", "session", "signature", "sig", "credential")

# 自由文本里的「键=值」泄露
_KV_PATTERN = re.compile(
    r"(?i)\b(cookie|set-cookie|token|access_token|refresh_token|authorization|password"
    r"|secret|api[-_]?key|session|signature)\b\s*[:=]\s*([^\s;,)\]}\"']+)")
# 长随机串（base64 / hex / JWT 片段）——**不含** `/` `:` `\`，避免把路径与 URL 打码
_BLOB_PATTERN = re.compile(r"[A-Za-z0-9_\-]{28,}")


def mask(value: Any, keep: int = 4) -> str:
    """保留头尾各 `keep` 字符，中间 `*`（短值全打码）。"""
    text = "" if value is None else str(value)
    if len(text) <= keep * 2:
        return "*" * len(text)
    return "%s…%s（%d 字符）" % (text[:keep], text[-keep:], len(text))


def redact_text(text: Any) -> str:
    """自由文本 → 脱敏文本（键值对 + 长随机串）。"""
    result = _KV_PATTERN.sub(lambda m: "%s=%s" % (m.group(1), mask(m.group(2))), str(text))
    return _BLOB_PATTERN.sub(lambda m: mask(m.group(0), keep=3), result)


def redact_cookies(cookies: Optional[Iterable[Any]]) -> List[Dict[str, Any]]:
    """Cookie 列表 → **只留名字 / 长度 / 标志位**（值永不出现在返回值里）。"""
    rows: List[Dict[str, Any]] = []
    for cookie in cookies or []:
        if not isinstance(cookie, dict):
            continue
        rows.append({
            "name": str(cookie.get("name", "")),
            "domain": cookie.get("domain"),
            "path": cookie.get("path"),
            "value_len": len(str(cookie.get("value", ""))),
            "http_only": bool(cookie.get("httpOnly")),
            "secure": bool(cookie.get("secure")),
            "session": bool(cookie.get("session")),
        })
    return sorted(rows, key=lambda row: (row["name"], str(row["domain"])))


def _scrub(value: Any, depth: int = 0) -> Any:
    """递归清洗任意结构：命中 `SECRET_KEYS` 的键整体打码，字符串走 `redact_text`。"""
    if depth > 6:
        return "…（超深）"
    if isinstance(value, dict):
        out: Dict[str, Any] = {}
        for key, item in value.items():
            name = str(key)
            if any(secret in name.lower() for secret in SECRET_KEYS):
                out[name] = mask(item)
            else:
                out[name] = _scrub(item, depth + 1)
        return out
    if isinstance(value, (list, tuple)):
        return [_scrub(item, depth + 1) for item in value[:32]]
    if isinstance(value, str):
        return redact_text(value)
    return value


def scrub(value: Any) -> Any:
    """公开的递归脱敏（`_scrub` 的对外单点 —— 其他模块写状态 / 日志时复用）。"""
    return _scrub(value)


def log_event(event: str, message: str = "", level: str = "info",
              path: Optional[pathlib.Path] = None, **fields: Any) -> Dict[str, Any]:
    """写一行日志（**唯一入口**）；返回实际落盘的记录（便于自检断言）。"""
    record: Dict[str, Any] = {
        "ts": round(time.time(), 3),
        "level": level,
        "event": str(event),
        "pid": _pid(),
    }
    if message:
        record["message"] = redact_text(message)
    for key, value in fields.items():
        record[key] = _scrub(value)
    target = pathlib.Path(path) if path else LOG_PATH
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    except OSError:
        # 日志不可写**不得**影响功能（不静默吞掉「有过错」这件事：退回 stderr 打印一行摘要）
        print("[日志] 无法写入 %s（%s）" % (target, record.get("event")), flush=True)
    return record


def tail(lines: int = 20, path: Optional[pathlib.Path] = None) -> List[str]:
    """读日志尾部（自检 / 诊断用）。"""
    target = pathlib.Path(path) if path else LOG_PATH
    if not target.is_file():
        return []
    try:
        content = target.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    return content[-max(1, lines):]


def _pid() -> int:
    import os
    return os.getpid()
