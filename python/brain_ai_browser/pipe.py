"""命名管道传输（Windows 原生，`ctypes` 直调 kernel32）—— `M7B-02`。

方向与归属（`M7B.md` §3）
    **Python 守护进程 = 服务器**（`CreateNamedPipeW`），**C++ = 客户端**（`CreateFileW`，
    见 `source/src/web/pipe_client.cpp`）。守护进程持有浏览器与生命周期，故由它持有管道实例。
    管道名带 pid（`\\\\.\\pipe\\aiwrite-browser-<pid>`）⇒ 多实例互不干扰（`M7B.md` §6.2 `P1`）。

分帧
    字节流 + `\\n` 分帧（**帧内不得有裸换行**）；单帧 ≤ `protocol.MAX_FRAME_BYTES`。
    超长行（无换行且超上限）**原样交出**，由 `protocol.parse_line` 判非法 → 调用方回
    `err {bad_frame}`（**不得**让守护进程退出，`VB2-29`）。

超时与取消
    全部 I/O 走 **overlapped**（`FILE_FLAG_OVERLAPPED` + `OVERLAPPED.hEvent`）：
    超时 → `CancelIoEx` + `GetOverlappedResult(bWait=True)` 收尾 → 再抛 `PipeTimeout`
    （不留悬挂 I/O，句柄可安全关闭；避免"关句柄时还有 I/O 在飞"的未定义行为）。

依赖纪律
    本模块**只用标准库**（`ctypes` / `os` / `time`）；`pydoll` 一律不在此层出现
    （step 3 由 `driver.py` 引入）。
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import time
from typing import Optional

from . import protocol as P

__all__ = [
    "PIPE_PREFIX",
    "PipeError",
    "PipeTimeout",
    "PipeServer",
    "PipeConnection",
    "normalize_pipe_name",
    "daemon_pipe_name",
    "connect_pipe",
]

# ---- 管道命名（`P1`：多实例互不干扰）----
PIPE_PREFIX = "\\\\.\\pipe\\"
DAEMON_PIPE_STEM = "aiwrite-browser-"


def normalize_pipe_name(name: str) -> str:
    """短名 → 全名（`\\\\.\\pipe\\<name>`）；已是全名则原样返回。"""
    if name.startswith("\\\\"):
        return name
    return PIPE_PREFIX + name


def daemon_pipe_name(pid: Optional[int] = None) -> str:
    """守护进程管道全名：`\\\\.\\pipe\\aiwrite-browser-<pid>`（缺省 = 本进程 pid）。"""
    return PIPE_PREFIX + DAEMON_PIPE_STEM + str(os.getpid() if pid is None else pid)


# ---- Win32 常量 ----
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PIPE_ACCESS_DUPLEX = 0x00000003
FILE_FLAG_OVERLAPPED = 0x40000000
PIPE_TYPE_BYTE = 0x00000000
PIPE_READMODE_BYTE = 0x00000000
PIPE_WAIT = 0x00000000
PIPE_UNLIMITED_INSTANCES = 255
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INFINITE = 0xFFFFFFFF
WAIT_OBJECT_0 = 0x00000000

ERROR_FILE_NOT_FOUND = 2
ERROR_BROKEN_PIPE = 109
ERROR_PIPE_BUSY = 231
ERROR_NO_DATA = 232
ERROR_PIPE_NOT_CONNECTED = 233
ERROR_HANDLE_EOF = 38
ERROR_PIPE_CONNECTED = 535
ERROR_OPERATION_ABORTED = 995
ERROR_IO_PENDING = 997

# 对端已消失（读 = 正常 EOF；写 = 可操作错误）
_EOF_CODES = (ERROR_BROKEN_PIPE, ERROR_PIPE_NOT_CONNECTED, ERROR_NO_DATA, ERROR_HANDLE_EOF)

class _OVERLAPPED(ctypes.Structure):
    """`OVERLAPPED`（x64：Internal / InternalHigh 为 ULONG_PTR）。"""

    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wt.DWORD),
        ("OffsetHigh", wt.DWORD),
        ("hEvent", wt.HANDLE),
    ]


_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.CreateNamedPipeW.restype = wt.HANDLE
_kernel32.CreateNamedPipeW.argtypes = [
    wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p,
]
_kernel32.ConnectNamedPipe.restype = wt.BOOL
_kernel32.ConnectNamedPipe.argtypes = [wt.HANDLE, ctypes.POINTER(_OVERLAPPED)]
_kernel32.DisconnectNamedPipe.restype = wt.BOOL
_kernel32.DisconnectNamedPipe.argtypes = [wt.HANDLE]
_kernel32.CreateFileW.restype = wt.HANDLE
_kernel32.CreateFileW.argtypes = [
    wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE,
]
_kernel32.ReadFile.restype = wt.BOOL
_kernel32.ReadFile.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD),
    ctypes.POINTER(_OVERLAPPED),
]
_kernel32.WriteFile.restype = wt.BOOL
_kernel32.WriteFile.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD),
    ctypes.POINTER(_OVERLAPPED),
]
_kernel32.GetOverlappedResult.restype = wt.BOOL
_kernel32.GetOverlappedResult.argtypes = [
    wt.HANDLE, ctypes.POINTER(_OVERLAPPED), ctypes.POINTER(wt.DWORD), wt.BOOL,
]
_kernel32.CancelIoEx.restype = wt.BOOL
_kernel32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.POINTER(_OVERLAPPED)]
_kernel32.CreateEventW.restype = wt.HANDLE
_kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
_kernel32.WaitForSingleObject.restype = wt.DWORD
_kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
_kernel32.CloseHandle.restype = wt.BOOL
_kernel32.CloseHandle.argtypes = [wt.HANDLE]


def _invalid(handle: Optional[int]) -> bool:
    return handle is None or handle == INVALID_HANDLE_VALUE


def _error_text(code: int) -> str:
    try:
        return ctypes.FormatError(code).strip()
    except Exception:  # pragma: no cover - Windows 上 FormatError 不会失败
        return "Win32 错误 %d" % code


def _wait_handle(handle: int, timeout_ms: Optional[int]) -> bool:
    ms = INFINITE if timeout_ms is None else max(0, int(timeout_ms))
    return _kernel32.WaitForSingleObject(handle, ms) == WAIT_OBJECT_0


class PipeError(Exception):
    """管道层错误（**必须可操作**，`I21`）。`code` 取 `protocol.ERROR_CODES`。"""

    def __init__(self, message: str, code: str = "daemon_down", hint: str = "") -> None:
        super().__init__(message)
        self.code = code
        self.hint = hint or message


class PipeTimeout(PipeError):
    """超时（= 可操作错误，**不假装完成**；与 `P7b-11` 同口径）。"""

    def __init__(self, message: str, hint: str = "") -> None:
        super().__init__(message, "daemon_down", hint)


class _PipeHandleIO:
    """已握手句柄上的**行分帧**读写（overlapped + 精确超时）。"""

    def __init__(self, handle: int, *, chunk: int = 4096, write_timeout_ms: int = 30_000,
                 disconnect_on_close: bool = False) -> None:
        self._handle: Optional[int] = handle
        self._chunk = chunk
        self._write_timeout_ms = write_timeout_ms
        self._disconnect_on_close = disconnect_on_close
        self._read_event = _kernel32.CreateEventW(None, True, False, None)
        self._write_event = _kernel32.CreateEventW(None, True, False, None)
        self._buffer = bytearray()
        self._closed = False
        if _invalid(self._read_event) or _invalid(self._write_event):
            raise PipeError("CreateEventW 失败：" + _error_text(ctypes.get_last_error()),
                            "daemon_down")

    # ---- 生命周期 ----
    @property
    def closed(self) -> bool:
        return self._closed

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        handle, self._handle = self._handle, None
        if not _invalid(handle):
            if self._disconnect_on_close:
                # 有意不断言 FlushFileBuffers：对端不读时它会阻塞；直接断开实例
                _kernel32.DisconnectNamedPipe(handle)
            _kernel32.CloseHandle(handle)
        for event in (self._read_event, self._write_event):
            if not _invalid(event):
                _kernel32.CloseHandle(event)
        self._read_event = self._write_event = None

    def __enter__(self) -> "_PipeHandleIO":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    # ---- 底层读 ----
    def _read_chunk(self, timeout_ms: Optional[int]) -> Optional[bytes]:
        """读一段字节；`None` = 对端已关闭（EOF）。"""
        buf = ctypes.create_string_buffer(self._chunk)
        overlapped = _OVERLAPPED()
        overlapped.hEvent = self._read_event
        ctypes.set_last_error(0)
        ok = _kernel32.ReadFile(self._handle, buf, self._chunk, None, ctypes.byref(overlapped))
        if not ok:
            err = ctypes.get_last_error()
            if err in _EOF_CODES:
                return None
            if err != ERROR_IO_PENDING:
                raise PipeError("ReadFile 失败：" + _error_text(err), "daemon_down")
        if not _wait_handle(overlapped.hEvent, timeout_ms):
            _kernel32.CancelIoEx(self._handle, ctypes.byref(overlapped))
            transferred = wt.DWORD()
            _kernel32.GetOverlappedResult(self._handle, ctypes.byref(overlapped),
                                          ctypes.byref(transferred), True)
            raise PipeTimeout("读取超时（%s ms）" % timeout_ms,
                              "守护进程未在期限内回包（查 ~/.brain-ai/logs/ 下的守护进程日志）")
        transferred = wt.DWORD()
        if not _kernel32.GetOverlappedResult(self._handle, ctypes.byref(overlapped),
                                            ctypes.byref(transferred), False):
            err = ctypes.get_last_error()
            if err in _EOF_CODES or err == ERROR_OPERATION_ABORTED:
                return None
            raise PipeError("GetOverlappedResult 失败：" + _error_text(err), "daemon_down")
        if transferred.value == 0:
            return None  # 对端关闭（EOF）
        return buf.raw[: transferred.value]


    def read_line(self, timeout_ms: Optional[int] = None) -> Optional[str]:
        """读一帧（不含 `\\n`）。

        * 返回 `None` = 对端已关闭且**缓冲无残留**（EOF）
        * 抛 `PipeTimeout` = 超时（即使已有半行数据也照抛，由调用方决定收尾）
        * 无换行的超长行 → 原样交出（交给 `protocol.parse_line` 判非法，`VB2-29`）
        """
        deadline = None if timeout_ms is None else time.monotonic() + max(0, timeout_ms) / 1000.0
        while True:
            index = self._buffer.find(b"\n")
            if index >= 0:
                line = bytes(self._buffer[:index])
                del self._buffer[: index + 1]
                return line.decode("utf-8", "replace").rstrip("\r")
            if len(self._buffer) > P.MAX_FRAME_BYTES:
                oversize = bytes(self._buffer)
                self._buffer.clear()
                return oversize.decode("utf-8", "replace")
            remaining: Optional[int] = None
            if deadline is not None:
                remaining = int(max(0.0, deadline - time.monotonic()) * 1000)
                if remaining <= 0:
                    raise PipeTimeout("读取超时（%s ms）" % timeout_ms,
                                      "守护进程未在期限内回包（查守护进程日志）")
            chunk = self._read_chunk(remaining)
            if chunk is None:
                if self._buffer:  # 最后一行没有换行符
                    tail = bytes(self._buffer)
                    self._buffer.clear()
                    return tail.decode("utf-8", "replace")
                return None
            self._buffer.extend(chunk)

    def write_line(self, text: str) -> None:
        """写一帧（自动补 `\\n`；**拒绝**超过单帧上限的载荷）。"""
        data = (text.strip("\r\n") + "\n").encode("utf-8")
        if len(data) - 1 > P.MAX_FRAME_BYTES:
            raise PipeError("帧超过单帧上限 %d 字节（§6.1 容量写死）" % P.MAX_FRAME_BYTES,
                            "bad_frame")
        self.write_bytes(data)

    def write_bytes(self, data: bytes) -> None:
        """写原始字节（**不检查单帧上限** —— 供 `write_line` 与自检构造超限帧）。"""
        if self._closed or self._handle is None:
            raise PipeError("连接已关闭，无法写入", "daemon_down", "先 connect_pipe / accept")
        view = memoryview(data)
        while view:
            piece = bytes(view[: self._chunk])
            buf = ctypes.create_string_buffer(piece)
            overlapped = _OVERLAPPED()
            overlapped.hEvent = self._write_event
            ctypes.set_last_error(0)
            ok = _kernel32.WriteFile(self._handle, buf, len(piece), None,
                                     ctypes.byref(overlapped))
            if not ok:
                err = ctypes.get_last_error()
                if err in _EOF_CODES or err == ERROR_OPERATION_ABORTED:
                    raise PipeError("对端已关闭（写失败）", "daemon_down",
                                    "C++ 侧可能已退出；重连前先 hello 握手")
                if err != ERROR_IO_PENDING:
                    raise PipeError("WriteFile 失败：" + _error_text(err), "daemon_down")
            if not _wait_handle(overlapped.hEvent, self._write_timeout_ms):
                _kernel32.CancelIoEx(self._handle, ctypes.byref(overlapped))
                raise PipeTimeout("写入超时（%s ms）" % self._write_timeout_ms,
                                  "对端未读取（管道缓冲区满）")
            transferred = wt.DWORD()
            done = _kernel32.GetOverlappedResult(self._handle, ctypes.byref(overlapped),
                                                 ctypes.byref(transferred), False)
            if not done or transferred.value == 0:
                err = ctypes.get_last_error()
                if err in _EOF_CODES or transferred.value == 0:
                    raise PipeError("对端已关闭（写失败）", "daemon_down")
                raise PipeError("GetOverlappedResult（写）失败：" + _error_text(err),
                                "daemon_down")
            view = view[transferred.value:]


class PipeConnection(_PipeHandleIO):
    """服务端的一条已连接管道（`close()` 会 `DisconnectNamedPipe`）。"""

    def __init__(self, handle: int, *, chunk: int = 4096, write_timeout_ms: int = 30_000) -> None:
        super().__init__(handle, chunk=chunk, write_timeout_ms=write_timeout_ms,
                         disconnect_on_close=True)


class PipeServer:
    """命名管道服务器：一个实例承载**一条**连接（实例用完即移交连接）。"""

    def __init__(self, name: str, *, in_buffer: int = 64 * 1024, out_buffer: int = 64 * 1024,
                 instances: int = PIPE_UNLIMITED_INSTANCES) -> None:
        self.name = normalize_pipe_name(name)
        self._in_buffer = in_buffer
        self._out_buffer = out_buffer
        self._instances = instances
        self._handle: Optional[int] = None

    def open(self) -> None:
        """创建管道实例（幂等）。"""
        if not _invalid(self._handle):
            return
        ctypes.set_last_error(0)
        handle = _kernel32.CreateNamedPipeW(
            self.name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            self._instances,
            self._out_buffer,
            self._in_buffer,
            0,  # nDefaultTimeOut 仅对 WaitNamedPipeW 生效（客户端走 overlapped 精确超时）
            None,
        )
        if _invalid(handle):
            err = ctypes.get_last_error()
            raise PipeError("CreateNamedPipe 失败：" + _error_text(err), "daemon_down",
                            "管道名 %s（同名实例可能已在跑）" % self.name)
        self._handle = handle

    def accept(self, timeout_ms: Optional[int] = None) -> PipeConnection:
        """等待客户端连接并交出**连接对象**（超时 → `PipeTimeout`；实例作废待重建）。"""
        self.open()
        event = _kernel32.CreateEventW(None, True, False, None)
        if _invalid(event):
            raise PipeError("CreateEventW 失败：" + _error_text(ctypes.get_last_error()),
                            "daemon_down")
        overlapped = _OVERLAPPED()
        overlapped.hEvent = event
        ctypes.set_last_error(0)
        ok = _kernel32.ConnectNamedPipe(self._handle, ctypes.byref(overlapped))
        if not ok:
            err = ctypes.get_last_error()
            if err == ERROR_PIPE_CONNECTED:
                pass  # 客户端已先连上（正常竞态）
            elif err != ERROR_IO_PENDING:
                _kernel32.CloseHandle(event)
                raise PipeError("ConnectNamedPipe 失败：" + _error_text(err), "daemon_down")
            elif not _wait_handle(event, timeout_ms):
                _kernel32.CancelIoEx(self._handle, ctypes.byref(overlapped))
                transferred = wt.DWORD()
                _kernel32.GetOverlappedResult(self._handle, ctypes.byref(overlapped),
                                              ctypes.byref(transferred), True)
                _kernel32.CloseHandle(event)
                self.close()  # 实例状态不可复用 → 下次 accept 重建
                raise PipeTimeout("等待客户端连接超时（%s ms）" % timeout_ms,
                                  "检查 C++ 侧是否已启动并调用了 pipe_client.connect")
        _kernel32.CloseHandle(event)
        handle, self._handle = self._handle, None  # 所有权移交连接
        return PipeConnection(handle)

    def close(self) -> None:
        handle, self._handle = self._handle, None
        if not _invalid(handle):
            _kernel32.DisconnectNamedPipe(handle)
            _kernel32.CloseHandle(handle)

    def __enter__(self) -> "PipeServer":
        self.open()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def connect_pipe(name: str, timeout_ms: int = 10_000) -> _PipeHandleIO:
    """以**客户端**身份连接管道（离线 loopback 自检 / 后续 `driver.py` 复用）。

    重试到 `timeout_ms`：`ERROR_FILE_NOT_FOUND`（服务器尚未建）/ `ERROR_PIPE_BUSY`（实例忙）。
    """
    full = normalize_pipe_name(name)
    deadline = time.monotonic() + max(0, timeout_ms) / 1000.0
    while True:
        ctypes.set_last_error(0)
        handle = _kernel32.CreateFileW(full, GENERIC_READ | GENERIC_WRITE, 0, None,
                                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if not _invalid(handle):
            return _PipeHandleIO(handle)
        err = ctypes.get_last_error()
        if err not in (ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY):
            raise PipeError("CreateFile 失败：" + _error_text(err), "daemon_down",
                            "管道 %s" % full)
        if time.monotonic() >= deadline:
            raise PipeTimeout("等待管道 %s 超时（%s ms）" % (full, timeout_ms),
                              "守护进程未启动，或双方管道名不一致")
        time.sleep(0.05)
