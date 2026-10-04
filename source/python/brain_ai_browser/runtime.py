"""运行时依赖检测（`M7B-13` · 批 1 step 3 落地 · `VB2-30` 的 Python 侧一半）。

口径（`I21`）
    依赖缺失 → **明确错误码 + 可操作引导**，**不发起任何 HTTP、不开任何浏览器**、
    不静默降级、不自动安装（`M7B-13` 的「未安装 → 引导 + 不阻塞其他功能」）。

设计要点
    * `load_pydoll()` 是**包内唯一的 pydoll 惰性入口** ⇒ 自检可**打桩**：
      「pydoll 缺失」这条路径**不需要真卸依赖**就能验（`VB2-30` 组 A）。
    * 本模块顶层**不 import pydoll**（`importlib` 惰性），模块导入本身零副作用。
    * ⚠️ 依赖细分码登记为开口项 **`MB-Q8`**：词表（§6.1）只有 `no_python`（依赖缺失）与
      `no_browser`，**没有 `no_pydoll`** ⇒ 「pydoll 缺失」沿用 `no_python`，
      并在 `hint` 里**显式说明缺的是哪个依赖**（不静默、不含糊）。本批**不动词表**。
"""

from __future__ import annotations

import importlib
import sys
from typing import Any, Dict, Optional

__all__ = [
    "DependencyError",
    "PYTHON_HINT",
    "PYDOLL_HINT",
    "NO_BROWSER_HINT",
    "load_pydoll",
    "pydoll_version",
    "check_runtime",
    "require_runtime",
    "dependency_hint",
]

# ---- 可操作引导（含动作 + 安装物；`M7B-14` 的最终文案归 step 4 定稿）----
PYTHON_HINT = ("未检测到可用的 Python 运行时：请安装 Python 3.12（推荐）后重开 AIwrite；"
               "网页通道依赖本机 Python，不会自动下载安装")
PYDOLL_HINT = ("本机 Python 缺少依赖 pydoll：请在 source\\python\\.venv 下执行 "
               "`python -m pip install -r source\\python\\requirements.txt`（钉版 pydoll-python==2.27.0）")
NO_BROWSER_HINT = ("未检测到本机浏览器：请安装 Chrome（推荐）或 Edge 后重开 AIwrite；"
                   "AIwrite 不会自动下载浏览器，也不会改用官方 API 顶替网页版")


class DependencyError(Exception):
    """依赖缺失（`I21`）：`code` = 词表错误码，`hint` = **可操作**引导。"""

    def __init__(self, code: str, hint: str = "") -> None:
        super().__init__("%s：%s" % (code, hint) if hint else code)
        self.code = code
        self.hint = hint


def load_pydoll() -> Optional[Any]:
    """惰性 import pydoll（**本包唯一入口**）；不可用 → `None`（不抛）。"""
    try:
        return importlib.import_module("pydoll")
    except Exception:  # noqa: BLE001 —— 依赖缺失是**预期情形**，不是崩溃
        return None


def pydoll_version() -> str:
    """已安装 pydoll 的版本（未装 / 元数据缺失 → 空串，不留假数字）。"""
    try:
        import importlib.metadata as metadata
        return metadata.version("pydoll-python")
    except Exception:  # noqa: BLE001
        return ""


def check_runtime() -> Dict[str, Any]:
    """`M7B-13` 运行时检测：`{python, pydoll, ok, hint}`。

    * `python` = 解释器版本字符串（本机事实）
    * `pydoll` = pydoll 版本（不可用 → 空串）
    * `ok`     = pydoll 可用（Python 侧依赖就绪）
    * `hint`   = 不可用时的**可操作**引导（可用 → 空串）
    """
    available = load_pydoll() is not None
    return {
        "python": "%d.%d.%d" % sys.version_info[:3],
        "pydoll": pydoll_version() if available else "",
        "ok": available,
        "hint": "" if available else PYDOLL_HINT,
    }


def require_runtime() -> Any:
    """返回 pydoll 模块；缺失 → `DependencyError("no_python", PYDOLL_HINT)`（**不启动浏览器**）。"""
    module = load_pydoll()
    if module is None:
        raise DependencyError("no_python", PYDOLL_HINT)
    return module


def dependency_hint(code: str) -> str:
    """词表错误码 → 可操作引导（`I21`）。"""
    if code == "no_browser":
        return NO_BROWSER_HINT
    if code == "no_python":
        return PYDOLL_HINT
    return PYTHON_HINT
