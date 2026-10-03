"""浏览器探测与环境物证（`M7B-14` · 批 1 step 3 落地）。

口径（`M7B.md` §6.2 step 4 / `M7B-14`）
    * **标准安装路径先 Chrome 后 Edge**（不查注册表、不猜商店版）；
    * 两者都无 → `detect_browser()` 返回 `("none", None)`：**驱动直接报 `no_browser` + 引导**，
      不静默回落、不自动下载浏览器（`I14` / `I21`）；
    * `browser_class()` 惰性 import pydoll（`Edge` / `Chrome`）⇒ 本模块**顶层不依赖 pydoll**，
      系统 Python（未装 pydoll）下仍可 import、可测。

本模块**零副作用**：只做「路径存在性判断」「`version` 查询」「无参构造选项」三类纯操作，
**不启动浏览器、不发任何 HTTP**（`VB2-30` 的「不开浏览器」前提）。
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
from typing import Any, Dict, Optional, Tuple

# ---- 跨语言常量（与 C++ `utils/paths.h::pydoll_profile()` / `M7B.md` §6.2 同值）----
#  * 改动此处必须同步 `source/src/utils/paths.cpp`（step 5 落地后在 C++ 侧自检里复验）
PROFILE = pathlib.Path.home() / ".brain-ai" / "pydoll-profile"
#  * `--window-size` 写死：视口尺寸决定 DOM 形态（`P4`，B1 教训；与 `M7B-28` 同源）
WINDOW_SIZE: Tuple[int, int] = (1440, 1000)

CHROME_PATHS: Tuple[pathlib.Path, ...] = (
    pathlib.Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
    pathlib.Path(r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"),
    pathlib.Path(os.path.expandvars(r"%LocalAppData%\Google\Chrome\Application\chrome.exe")),
)
EDGE_PATHS: Tuple[pathlib.Path, ...] = (
    pathlib.Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
    pathlib.Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"),
)


def profile_path(profile: Optional[pathlib.Path] = None) -> pathlib.Path:
    """本次启动使用的 profile 目录（缺省 = `~/.brain-ai/pydoll-profile`，单 profile）。"""
    return pathlib.Path(profile) if profile else PROFILE


def detect_chrome() -> Optional[pathlib.Path]:
    """标准安装路径里的 Chrome（无 → `None`）。"""
    for path in CHROME_PATHS:
        if path.is_file():
            return path
    return None


def detect_edge() -> Optional[pathlib.Path]:
    """标准安装路径里的 Edge（无 → `None`）。"""
    for path in EDGE_PATHS:
        if path.is_file():
            return path
    return None


def detect_browser() -> Tuple[str, Optional[pathlib.Path]]:
    """`("chrome" | "edge" | "none", exe)` —— **先 Chrome 后 Edge**（本机无 Chrome 时落到 Edge）。"""
    chrome = detect_chrome()
    if chrome:
        return "chrome", chrome
    edge = detect_edge()
    if edge:
        return "edge", edge
    return "none", None


def browser_kind() -> str:
    """`"chrome"` / `"edge"` / `"none"`。"""
    return detect_browser()[0]


def browser_class() -> Any:
    """返回 pydoll 浏览器类（`Edge` / `Chrome`）。

    ⚠️ **必须先过 `runtime.require_runtime()`**：pydoll 缺失时这里会抛 `ImportError`，
    而 `I21` 要求的形态是「可操作错误码 + 引导」⇒ 由调用方（`driver.start`）前置拦截。

    两者都无时仍返回 `Chrome`：让库自己报启动失败并把**真实错误**留证（探针 `M7B-09` 口径），
    不在这里假装成功、也不静默改成别的浏览器。
    """
    from pydoll.browser.chromium import Chrome, Edge  # 惰性：顶层不依赖 pydoll
    return Edge if browser_kind() == "edge" else Chrome


def env_proof() -> Dict[str, Any]:
    """环境物证（换机可追溯）：Python / pydoll / 浏览器类 / 浏览器 exe 及其版本。"""
    import importlib.metadata as metadata

    exe = detect_browser()[1]
    info: Dict[str, Any] = {
        "python": ".".join(str(part) for part in sys.version_info[:3]),
        "pydoll": "",
        "browser_kind": browser_kind(),
        "browser_exe": str(exe) if exe else "",
        "browser_version": "",
        "profile": str(PROFILE),
    }
    try:
        info["pydoll"] = metadata.version("pydoll-python")
    except Exception:  # noqa: BLE001 —— 未装 pydoll 时不让物证收集炸掉自检
        pass
    if exe:
        info["browser_version"] = _file_version(exe)
    return info


def _file_version(exe: pathlib.Path) -> str:
    """读 exe 的 `ProductVersion`（best-effort；失败返回空串，不留假数字）。"""
    try:
        done = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-Item '%s').VersionInfo.ProductVersion" % exe],
            capture_output=True, text=True, timeout=20, check=False)
    except Exception:  # noqa: BLE001
        return ""
    return done.stdout.strip()
