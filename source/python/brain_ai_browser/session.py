"""L2 登录态加密快照（`MB-D0-8` L2 · `M7B.md` §6.2 step 5 · `VB2-39`）。

职责（**只做一件事**）
    把「当前登录态」= CDP 浏览器级 Cookie（**全 origin · 含 HttpOnly**）存成一份
    **DPAPI 加密**的快照，下次启动时**回灌**——用户不必每次重登（`I23②`）。

硬口径（机器可断言，不靠自律）
    * **代码 / 数据分离**：本模块是**运行代码**（在 `source/python/brain_ai_browser/`）；
      数据一律落 `%USERPROFILE%\\.brain-ai\\session\\cookies.dat`（**仓库外 · 不入库**）；
    * **只许 DPAPI 加密落盘**（`I23③`）：`CryptProtectData`（`CRYPTPROTECT_UI_FORBIDDEN`，
      不弹 UI）；**DPAPI 不可用 ⇒ 不快照**（宁可让用户重登，也**绝不写明文 / 不写别处 / 不外传**）；
    * **值不进日志 / 不进协议 / 不进状态文件**：日志走 `redact.log_event()`，只记「名字 / 计数 / 域」；
    * **恢复失败必须显式提示**（`I23④`）：快照损坏 / 过期 / 解不开 → **当「没有」处理 + 如实回报原因**
      （由 `daemon.py` 追加一条 `error{id="-"}` 事件），不崩、不静默、不回退明文；
    * **不读 `providers.json`**（`I14`）：作用域（要快照哪些域）**由调用方给**。

作用域（开口项 `MB-Q11`）
    `scope` = 本次会话**导航过的域**（由 `open_tab` 的 url 派生）⇒ **空作用域不写快照**
    （隐私最小：宁缺勿滥）。需要「全库」时由调用方显式传 `scope=[]` 之外的策略，本模块不擅自扩大。

依赖：**只用标准库**（`ctypes` 调 DPAPI）。探针依据：`_probe/m7b09_snapshot_roundtrip.py`（V-b 可行性）。
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import pathlib
import sys
import time
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from . import redact

__all__ = [
    "SESSION_DIR",
    "SNAPSHOT_NAME",
    "SNAPSHOT_VERSION",
    "REFRESH_INTERVAL_S",
    "MAX_SNAPSHOT_BYTES",
    "ENTROPY",
    "SETTABLE_FIELDS",
    "ENV_SESSION_DIR",
    "SnapshotError",
    "SnapshotUnavailable",
    "session_dir",
    "snapshot_path",
    "dpapi_available",
    "protect",
    "unprotect",
    "host_of",
    "belongs_to",
    "scoped",
    "to_cdp_params",
    "usable_cookies",
    "scan_plaintext",
    "snapshot_payload",
    "summarize",
    "SnapshotStore",
]

# ---- 跨语言常量（与 C++ 侧 `session_snapshot.*` 对齐；step 6 落地时同值）----
SESSION_DIR_NAME = "session"                 # `~/.brain-ai/session`（**≠** `snapshots/`：那是工作流快照）
SNAPSHOT_NAME = "cookies.dat"                # 文件名**不含站点名**（不泄露访问过谁）
SNAPSHOT_VERSION = 1
REFRESH_INTERVAL_S = 5.0                     # `MB-Q5`：刷新周期 < 10 s（`M7B-09` 结论④）
MAX_SNAPSHOT_BYTES = 1024 * 1024             # 1 MiB 上限（防膨胀；超限当「损坏」）
ENTROPY = b"brain-ai-session-v1"             # DPAPI 附加熵（同机不同用途不可混解）
ENV_SESSION_DIR = "BRAIN_AI_SESSION_DIR"     # 测试 / 高级用途：重定向快照目录（默认不设）

# CDP `Cookie` → `SetCookieParams` 的安全白名单（探针 V-b 实测：多带只读字段会报错）
SETTABLE_FIELDS: Tuple[str, ...] = (
    "name", "value", "domain", "path", "secure", "httpOnly",
    "sameSite", "priority", "sourceScheme", "sourcePort",
)


class SnapshotError(Exception):
    """L2 快照读写失败（**必须如实回报**，绝不静默假装成功）。"""


class SnapshotUnavailable(SnapshotError):
    """DPAPI / 平台不可用 ⇒ **不快照**（绝不回退明文）。"""


def session_dir() -> pathlib.Path:
    """L2 快照目录（`BRAIN_AI_SESSION_DIR` 覆盖仅用于自检 / 高级用途；缺省 `~/.brain-ai/session`）。"""
    value = os.environ.get(ENV_SESSION_DIR, "").strip()
    return pathlib.Path(value) if value else SESSION_DIR


def snapshot_path() -> pathlib.Path:
    """快照文件路径（固定名 `cookies.dat` —— 不随站点变化）。"""
    return session_dir() / SNAPSHOT_NAME


#: 缺省目录（模块级常量；`session_dir()` 允许环境变量覆盖 ⇒ 自检**不污染真实快照**）
SESSION_DIR = pathlib.Path.home() / ".brain-ai" / SESSION_DIR_NAME


# ============================================================================
#  DPAPI（**唯一**加密入口；`I23③` —— 不引第三方依赖，`ctypes` 直调 `crypt32`）
# ============================================================================

CRYPTPROTECT_UI_FORBIDDEN = 0x01


class _DataBlob(ctypes.Structure):
    """`DATA_BLOB`（`win32crypt` 内部同结构；自声明以免引第三方依赖）。"""

    _fields_ = [("cbData", ctypes.c_uint32), ("pbData", ctypes.POINTER(ctypes.c_char))]


_API: Optional[Tuple[Any, Any]] = None


def _api() -> Tuple[Any, Any]:
    """惰性绑定 `crypt32` / `kernel32`（非 Windows / 缺函数 ⇒ `SnapshotUnavailable`）。"""
    global _API
    if _API is None:
        win_dll = getattr(ctypes, "WinDLL", None)
        if sys.platform != "win32" or win_dll is None:
            raise SnapshotUnavailable("非 Windows 平台无 DPAPI（L2 快照不可用；**绝不回退明文**）")
        try:
            crypt32 = win_dll("crypt32", use_last_error=True)
            kernel32 = win_dll("kernel32", use_last_error=True)
            crypt32.CryptProtectData.argtypes = [
                ctypes.POINTER(_DataBlob), ctypes.c_wchar_p, ctypes.POINTER(_DataBlob),
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                ctypes.POINTER(_DataBlob)]
            crypt32.CryptProtectData.restype = ctypes.c_int
            crypt32.CryptUnprotectData.argtypes = [
                ctypes.POINTER(_DataBlob), ctypes.POINTER(ctypes.c_wchar_p),
                ctypes.POINTER(_DataBlob), ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_uint32, ctypes.POINTER(_DataBlob)]
            crypt32.CryptUnprotectData.restype = ctypes.c_int
            kernel32.LocalFree.argtypes = [ctypes.c_void_p]
            kernel32.LocalFree.restype = ctypes.c_void_p
        except (OSError, AttributeError) as exc:      # pragma: no cover —— 依赖 OS
            raise SnapshotUnavailable("DPAPI 不可用：%s（L2 快照不可用；**绝不回退明文**）" % exc)
        _API = (crypt32, kernel32)
    return _API


def dpapi_available() -> bool:
    """DPAPI 是否可用（自检据此**显式 SKIP**，而不是 FAIL —— 不假装通过）。"""
    try:
        _api()
        return True
    except SnapshotUnavailable:
        return False


def _blob(data: bytes) -> Tuple[_DataBlob, Any]:
    """bytes → `DATA_BLOB`（连同**保活缓冲区**一起返回：被回收会导致野指针）。"""
    payload = bytes(data)
    buffer = ctypes.create_string_buffer(payload, max(1, len(payload)))
    return _DataBlob(len(payload), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_char))), buffer


def _entropy_blob(entropy: bytes) -> Tuple[Optional[_DataBlob], Any]:
    if not entropy:
        return None, None
    return _blob(entropy)


def _local_free(blob: _DataBlob) -> None:
    """释放 DPAPI 分配的内存（`LocalFree`；失败不改变主流程结果）。"""
    if not blob.pbData:
        return
    try:
        _api()[1].LocalFree(ctypes.cast(blob.pbData, ctypes.c_void_p))
    except Exception:  # noqa: BLE001 —— 收尾路径，不能因释放失败改变结论
        pass


def protect(plaintext: bytes, *, entropy: bytes = ENTROPY) -> bytes:
    """DPAPI 加密（`CRYPTPROTECT_UI_FORBIDDEN` ⇒ 无 UI 提示，可无人值守）。

    同一明文两次加密**结果不同**（DPAPI 带随机 salt）⇒ 「内容变了才写盘」的判断
    必须比较**明文摘要**（见 `SnapshotStore.save`），不能比较密文。
    """
    crypt32, _kernel32 = _api()
    data_in, _keep_in = _blob(plaintext)
    entropy_in, _keep_entropy = _entropy_blob(entropy)
    data_out = _DataBlob()
    ok = crypt32.CryptProtectData(ctypes.byref(data_in), "brain-ai L2 session",
                                  ctypes.byref(entropy_in) if entropy_in else None,
                                  None, None, CRYPTPROTECT_UI_FORBIDDEN,
                                  ctypes.byref(data_out))
    if not ok:
        raise SnapshotError("DPAPI 加密失败（错误码 %d）" % ctypes.get_last_error())
    try:
        return ctypes.string_at(data_out.pbData, data_out.cbData)
    finally:
        _local_free(data_out)


def unprotect(blob: bytes, *, entropy: bytes = ENTROPY) -> bytes:
    """DPAPI 解密（失败 ⇒ `SnapshotError`；调用方**当「损坏」处理**，不崩、不静默）。

    实测（2026-10-03 · 本机）：DPAPI blob **自带完整性校验** —— 截断 / 改头部 / 改中段 /
    改末尾 / 把明文 JSON 当密文喂进来，**一律解密失败**（错误码 13）。
    ⚠️ 唯一例外：blob 头部有一小段**明文「描述区」**（`CryptProtectData` 的 `szDataDescr`，
    本模块传 "brain-ai L2 session"）—— 改它**不影响解密**、也**不影响载荷内容**
    ⇒ 自检的「篡改 1 字节」断言取**中段 / 末尾**（稳定判据），这一点已写进 `--session-selftest`。
    """
    crypt32, _kernel32 = _api()
    data_in, _keep_in = _blob(blob)
    entropy_in, _keep_entropy = _entropy_blob(entropy)
    data_out = _DataBlob()
    ok = crypt32.CryptUnprotectData(ctypes.byref(data_in), None,
                                    ctypes.byref(entropy_in) if entropy_in else None,
                                    None, None, CRYPTPROTECT_UI_FORBIDDEN,
                                    ctypes.byref(data_out))
    if not ok:
        raise SnapshotError("DPAPI 解密失败（错误码 %d：快照损坏 / 换了用户账户 / 附加熵不符）"
                            % ctypes.get_last_error())
    try:
        return ctypes.string_at(data_out.pbData, data_out.cbData)
    finally:
        _local_free(data_out)


# ============================================================================
#  纯函数（离线可断言 —— `VB2-39` 的判据全落在这里）
# ============================================================================

def host_of(value: str) -> str:
    """url / origin / host → **小写 host**（去协议、端口、路径、userinfo；空 → 空串）。"""
    text = str(value or "").strip()
    if not text:
        return ""
    if "//" in text:
        text = text.split("//", 1)[1]
    text = text.split("/", 1)[0].split("?", 1)[0].split("#", 1)[0]
    if "@" in text:
        text = text.rsplit("@", 1)[1]
    if text.startswith("["):                       # IPv6 字面量
        return text.split("]", 1)[0].lstrip("[").lower()
    return text.split(":", 1)[0].lower()


def belongs_to(cookie_domain: str, host: str) -> bool:
    """Cookie 域是否属于该 host（`host == domain` 或 `host` 以 `.domain` 结尾）。

    ⚠️ 与 `driver.cookies_for_domain()` 的**后缀语义方向相反**：那个回答「条目给的可注册域 →
    全库里哪些属于它」；这个回答「**这个页面的登录 Cookie 有哪些**」—— 必须包含**父域 Cookie**
    （`.example.com` 对 `chat.example.com` 成立）。方向搞反就会漏掉最常见的登录 Cookie。
    """
    domain = str(cookie_domain or "").strip().lower().lstrip(".")
    site = host_of(host)
    if not domain or not site:
        return False
    return site == domain or site.endswith("." + domain)


def scoped(cookies: Optional[Iterable[Any]], hosts: Optional[Iterable[str]]) -> List[Dict[str, Any]]:
    """按作用域（域白名单）取 Cookie；**空作用域 ⇒ 空列表**（`MB-Q11`：不发散、隐私最小）。"""
    wanted = [host_of(item) for item in (hosts or [])]
    wanted = [item for item in wanted if item]
    rows = [dict(cookie) for cookie in (cookies or []) if isinstance(cookie, dict)]
    if not wanted:
        return []
    return [row for row in rows
            if any(belongs_to(row.get("domain") or "", host) for host in wanted)]


def to_cdp_params(cookies: Optional[Iterable[Any]]) -> List[Dict[str, Any]]:
    """Cookie → `Storage.setCookies` 参数（**字段白名单** + **会期语义**）。

    * 只带 `SETTABLE_FIELDS`（多带 `size` / `session` 等只读字段会被 CDP 拒绝 —— 探针 V-b 实测）；
    * `expires <= 0` ⇒ **不带** `expires`（会期 Cookie 不偷偷升级成持久）；
    * 无 `name` / 无 `domain` 的条目**丢弃**（CDP 需要域；丢弃数由调用方按长度差得出）。
    """
    params: List[Dict[str, Any]] = []
    for cookie in cookies or []:
        if not isinstance(cookie, dict):
            continue
        name = str(cookie.get("name") or "")
        domain = str(cookie.get("domain") or "")
        if not name or not domain:
            continue
        param: Dict[str, Any] = {key: cookie[key] for key in SETTABLE_FIELDS if key in cookie}
        param["name"] = name
        param["domain"] = domain
        expires = cookie.get("expires")
        if isinstance(expires, (int, float)) and expires > 0:
            param["expires"] = float(expires)
        params.append(param)
    return params


def usable_cookies(payload: Optional[Dict[str, Any]], *, now: Optional[float] = None,
                   skew_s: float = 0.0) -> List[Dict[str, Any]]:
    """载荷里**未过期**的 Cookie（会期 Cookie 一律保留；`now` 可注入 ⇒ 离线可断言）。"""
    when = time.time() if now is None else float(now)
    rows = payload.get("cookies") if isinstance(payload, dict) else None
    out: List[Dict[str, Any]] = []
    for cookie in rows or []:
        if not isinstance(cookie, dict):
            continue
        expires = cookie.get("expires")
        if isinstance(expires, (int, float)) and expires > 0 and expires <= when + float(skew_s):
            continue
        out.append(cookie)
    return out


def scan_plaintext(blob: bytes, secrets: Optional[Iterable[str]] = None,
                   *, min_len: int = 3) -> List[str]:
    """在快照**原始字节**里扫描明文（`VB2-39②` 的机器判据）。

    * `secrets` = 本应**不可能出现**的串（Cookie 名 / 值 / 域 / 令牌）；
    * 短于 `min_len` 的串跳过（如 `"1"` 会在 1 MiB 密文里偶然命中 ⇒ 误报）；
    * 返回命中的串**本身**（调用方只记个数，不把内容落盘）。
    """
    data = bytes(blob or b"")
    hits: List[str] = []
    for secret in secrets or []:
        text = str(secret or "")
        if len(text) < max(1, int(min_len)):
            continue
        try:
            needle = text.encode("utf-8")
        except UnicodeEncodeError:                 # pragma: no cover —— 极端输入
            continue
        if needle in data:
            hits.append(text)
    return hits


def snapshot_payload(cookies: Optional[Iterable[Any]], *, hosts: Optional[Iterable[str]] = None,
                     provider: str = "", reason: str = "",
                     now: Optional[float] = None) -> Dict[str, Any]:
    """构造快照载荷（**纯数据**；加密与落盘在 `SnapshotStore`）。"""
    when = time.time() if now is None else float(now)
    rows = [dict(cookie) for cookie in (cookies or []) if isinstance(cookie, dict)]
    return {
        "v": SNAPSHOT_VERSION,
        "saved_at": round(when, 3),
        "provider": str(provider or ""),
        "hosts": [host_of(item) for item in (hosts or []) if host_of(item)],
        "reason": str(reason or ""),
        "cookies": rows,
    }


def summarize(payload: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """载荷 → **可进日志**的摘要（只有名字 / 计数 / 域；**值不出现在返回值里**）。"""
    data = payload if isinstance(payload, dict) else {}
    rows = [row for row in (data.get("cookies") or []) if isinstance(row, dict)]
    names = sorted({str(row.get("name") or "") for row in rows if row.get("name")})
    domains = sorted({str(row.get("domain") or "") for row in rows if row.get("domain")})
    return {
        "version": data.get("v"),
        "saved_at": data.get("saved_at"),
        "count": len(rows),
        "names": names,
        "domains": domains,
        "http_only": sum(1 for row in rows if row.get("httpOnly")),
        "session": sum(1 for row in rows
                       if not (isinstance(row.get("expires"), (int, float))
                               and row.get("expires") > 0)),
    }


# ============================================================================
#  快照读写（DPAPI 加密 + 原子写；**唯一**落盘点）
# ============================================================================

class SnapshotStore:
    """L2 快照的读写单点（`MB-D0-8` L2）。

    * `save()` —— 作用域过滤 → 明文 JSON → DPAPI → **原子写**（`.tmp` + `os.replace`）；
      **空作用域 / 无 Cookie / DPAPI 不可用 / 写盘失败 ⇒ 一律不写**（返回 `ok=False` + `reason`，
      **绝不写明文**、绝不换路径）；
    * `load()` —— 读 → 解密 → 校验版本 → 过滤过期；**失败 = 当「没有」**（返回 `ok=False` + `reason`，
      由调用方**显式提示**重新登录 —— `I23④`）；**不抛异常、不崩**；
    * 计数（`saves` / `restores` / `last_reason`）进日志与 `daemon-state.json`（L4 留痕）。
    """

    def __init__(self, path: Optional[Any] = None, *, entropy: bytes = ENTROPY,
                 scope: Optional[Iterable[str]] = None) -> None:
        self.path = pathlib.Path(path) if path else snapshot_path()
        self.entropy = bytes(entropy)
        self.scope: List[str] = [host_of(item) for item in (scope or []) if host_of(item)]
        self.saves = 0
        self.restores = 0
        self.last_reason = ""
        self._plain_digest = ""            # 明文摘要（**不进日志**）：内容没变就不重复写盘

    # ---- 状态 ----
    def exists(self) -> bool:
        """快照文件是否存在（**只看文件**，不解密）。"""
        return self.path.is_file()

    def size(self) -> int:
        """文件字节数（取不到 → 0）。"""
        try:
            return self.path.stat().st_size
        except OSError:
            return 0

    def add_scope(self, *hosts: str) -> List[str]:
        """登记作用域（`open_tab` 成功后调用）；返回去重后的域列表。"""
        for item in hosts:
            host = host_of(item)
            if host and host not in self.scope:
                self.scope.append(host)
        return list(self.scope)

    # ---- 读（回灌用）----
    def load(self) -> Dict[str, Any]:
        """读快照 → `{ok, payload, cookies, reason, bytes}`；**任何失败都不抛**。"""
        result: Dict[str, Any] = {"ok": False, "payload": None, "cookies": [],
                                  "reason": "", "bytes": 0}
        if not self.path.is_file():
            result["reason"] = "absent"                 # 首次运行：**正常**，不是故障
            self.last_reason = "absent"
            return result
        try:
            blob = self.path.read_bytes()
        except OSError as exc:
            result["reason"] = "unreadable"
            self.last_reason = "unreadable: %s" % exc
            return result
        result["bytes"] = len(blob)
        if len(blob) > MAX_SNAPSHOT_BYTES:
            result["reason"] = "too_large"              # 被塞爆 / 损坏 → 当「没有」
            self.last_reason = "too_large"
            return result
        try:
            plain = unprotect(blob, entropy=self.entropy)
        except SnapshotUnavailable as exc:
            result["reason"] = "dpapi_unavailable"
            self.last_reason = str(exc)
            return result
        except SnapshotError as exc:
            result["reason"] = "corrupt"
            self.last_reason = str(exc)
            return result
        try:
            payload = json.loads(plain.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            result["reason"] = "corrupt"
            self.last_reason = "载荷不是合法 JSON：%s" % exc
            return result
        if not isinstance(payload, dict) or payload.get("v") != SNAPSHOT_VERSION:
            result["reason"] = "version_unknown"
            self.last_reason = "版本不识别：%r" % (
                payload.get("v") if isinstance(payload, dict) else payload)
            return result
        cookies = usable_cookies(payload)
        if not cookies:
            result["reason"] = "expired"                # 全部过期 → 当「没有」（引导重登）
            self.last_reason = "expired"
            return result
        self.restores += 1
        self.last_reason = "restored"
        result.update({"ok": True, "payload": payload, "cookies": cookies, "reason": ""})
        redact.log_event("snapshot_loaded", "L2 快照已读出（待回灌）",
                         bytes=len(blob), **summarize(payload))
        return result

    # ---- 写（定时刷新 / 登录即写 / 退出前再写）----
    def save(self, cookies: Optional[Iterable[Any]], *, hosts: Optional[Iterable[str]] = None,
             provider: str = "", reason: str = "", force: bool = False) -> Dict[str, Any]:
        """存快照；返回证据 `{ok, reason, count, bytes, changed}`（**失败不影响调用方结果**）。"""
        scope = list(hosts) if hosts is not None else list(self.scope)
        rows = scoped(cookies, scope)
        if not rows:
            wanted = [host_of(item) for item in scope if host_of(item)]
            self.last_reason = "no_scope" if not wanted else "empty"
            redact.log_event("snapshot_skipped",
                             "L2 快照未写：%s（不写明文、不发散）" % self.last_reason,
                             level="info", reason=self.last_reason, scope=wanted)
            return {"ok": False, "reason": self.last_reason, "count": 0, "bytes": 0,
                    "changed": False}
        payload = snapshot_payload(rows, hosts=scope, provider=provider, reason=reason)
        plain = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        digest = hashlib.sha256(plain).hexdigest()
        if not force and digest == self._plain_digest:
            self.last_reason = "unchanged"       # 内容没变 → 不重复写盘（省 IO、省磨损）
            return {"ok": True, "reason": "unchanged", "count": len(rows),
                    "bytes": self.size(), "changed": False}
        try:
            blob = protect(plain, entropy=self.entropy)
        except SnapshotUnavailable as exc:
            self.last_reason = "dpapi_unavailable"
            redact.log_event("snapshot_skipped", "L2 快照未写：DPAPI 不可用（**不回退明文**）",
                             level="warn", reason=self.last_reason, hint=str(exc))
            return {"ok": False, "reason": self.last_reason, "count": 0, "bytes": 0,
                    "changed": False}
        except SnapshotError as exc:
            self.last_reason = "encrypt_failed"
            redact.log_event("snapshot_failed", "L2 快照加密失败：%s" % exc, level="warn",
                             reason=self.last_reason)
            return {"ok": False, "reason": self.last_reason, "count": 0, "bytes": 0,
                    "changed": False}
        if len(blob) > MAX_SNAPSHOT_BYTES:
            self.last_reason = "too_large"
            redact.log_event("snapshot_failed", "L2 快照超上限（%d > %d）→ 不写"
                             % (len(blob), MAX_SNAPSHOT_BYTES), level="warn",
                             reason=self.last_reason)
            return {"ok": False, "reason": self.last_reason, "count": 0,
                    "bytes": len(blob), "changed": False}
        tmp = self.path.with_name(self.path.name + ".tmp")
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_bytes(blob)
            os.replace(tmp, self.path)           # 原子替换（同卷 rename）
        except OSError as exc:
            self.last_reason = "write_failed"
            redact.log_event("snapshot_failed", "L2 快照写盘失败：%s" % exc, level="warn",
                             reason=self.last_reason, path=str(self.path))
            try:
                tmp.unlink()
            except OSError:
                pass
            return {"ok": False, "reason": self.last_reason, "count": 0, "bytes": 0,
                    "changed": False}
        self._plain_digest = digest
        self.saves += 1
        self.last_reason = str(reason or "saved")
        redact.log_event("snapshot_saved", "L2 快照已保存（DPAPI 加密）",
                         reason=reason or "saved", provider=provider, bytes=len(blob),
                         **summarize(payload))
        return {"ok": True, "reason": self.last_reason, "count": len(rows),
                "bytes": len(blob), "changed": True}

    def clear(self) -> bool:
        """删除快照（**幂等**）；返回是否真的删了。"""
        try:
            self.path.unlink()
        except FileNotFoundError:
            return False
        except OSError as exc:
            redact.log_event("snapshot_failed", "L2 快照删除失败：%s" % exc, level="warn")
            return False
        self._plain_digest = ""
        self.last_reason = "cleared"
        redact.log_event("snapshot_cleared", "L2 快照已删除（下次启动需重新登录）")
        return True
