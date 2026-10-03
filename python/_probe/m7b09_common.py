# -*- coding: utf-8 -*-
"""M7B-09 公共工具（一次性探针 · `MB-D0-8` L1/L2 闸门共用）。

集中三件事，避免每个脚本各写一遍：
  1. **读回自证**（`source/README.md` §6.2 纪律）：`execute_script` 的返回是**两层 `result`**
     （`{'id':N,'result':{'result':{'type','value'}}}`），解错层级会**静默拿到空串**。
  2. **三种退出方式**：`kill_raw`（等价任务管理器强杀）/ `pydoll_stop`（= 库默认，close 后立刻 kill）
     / `close_wait`（**计划要求的 L1**：`Browser.close` → 等进程真正退出）。
  3. Cookie 归一化 + profile 落盘物证（`Cookies` / `Cookies-journal`）。
"""
import asyncio
import json
import pathlib
import subprocess
import sys
import time

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

PROFILE = pathlib.Path.home() / '.brain-ai' / 'pydoll-profile'
OUT = pathlib.Path(__file__).parent / 'out'


def options(headless: bool = True, prefs: dict | None = None,
            extra_args: tuple[str, ...] = (), profile: pathlib.Path | None = None) -> ChromiumOptions:
    """统一构造选项（单 profile；⚠️ `--no-first-run` 由库自己加，重加会抛）。"""
    opts = ChromiumOptions()
    opts.headless = headless
    opts.add_argument(f'--user-data-dir={profile or PROFILE}')
    for arg in extra_args:
        opts.add_argument(arg)
    if prefs:
        opts.browser_preferences = prefs
    opts.start_timeout = 60
    return opts


# ------------------------------------------------------- 浏览器选择（换机兜底） --
# 本机（2026-10-02 换机）**无 Chrome，只有 Edge** ⇒ `M7B-14` 的「Chrome 缺失 → Edge 兜底」
# 首次实跑。口径：**先探测标准安装路径**（不查注册表 / 不猜商店版），命中谁用谁；
# 两者都无 → 仍返回 `Chrome`（让库自己报启动失败，探针把真实错误留证）。
CHROME_PATHS = (pathlib.Path(r'C:\Program Files\Google\Chrome\Application\chrome.exe'),
                pathlib.Path(r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe'))
EDGE_PATHS = (pathlib.Path(r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'),
              pathlib.Path(r'C:\Program Files\Microsoft\Edge\Application\msedge.exe'))


def browser_kind() -> str:
    """`'chrome'` / `'edge'` / `'unknown'`（只按标准安装路径判定）。"""
    if any(path.exists() for path in CHROME_PATHS):
        return 'chrome'
    if any(path.exists() for path in EDGE_PATHS):
        return 'edge'
    return 'unknown'


def browser_exe() -> pathlib.Path | None:
    for path in CHROME_PATHS + EDGE_PATHS:
        if path.exists():
            return path
    return None


def browser_class():
    """返回 pydoll 浏览器类（`pydoll.browser.chromium` 同模块导出 `Chrome` / `Edge`）。"""
    from pydoll.browser.chromium import Chrome, Edge
    return Edge if browser_kind() == 'edge' else Chrome


def env_proof() -> dict:
    """**环境物证**（换机可追溯）：Python / pydoll / 浏览器类 / 浏览器 exe 及其版本。"""
    import importlib.metadata as metadata

    exe = browser_exe()
    info = {'python': '.'.join(str(part) for part in sys.version_info[:3]),
            'pydoll': 'unknown', 'browser_kind': browser_kind(),
            'browser_exe': str(exe) if exe else '', 'browser_version': ''}
    try:
        info['pydoll'] = metadata.version('pydoll-python')
    except Exception:  # noqa: BLE001 —— 未装 pydoll 时不让物证收集炸掉自检
        pass
    if exe:
        try:
            info['browser_version'] = subprocess.run(
                ['powershell', '-NoProfile', '-Command',
                 f"(Get-Item '{exe}').VersionInfo.ProductVersion"],
                capture_output=True, text=True, timeout=20, check=False).stdout.strip()
        except Exception:  # noqa: BLE001
            pass
    return info


def exit_state(profile: pathlib.Path | None = None) -> dict:
    """读 profile 的 `Default/Preferences` 里的**退出状态**（会期 Cookie 是否被恢复的关键）。

    Chromium 语义（实测推断）：上一次退出若被判为 **Crashed**，启动时会**保留会期 Cookie**
    （属崩溃恢复）；若是 **Normal**，会期 Cookie 正常清除。因此"会期 Cookie 能否跨重启"
    **不仅取决于是否干净退出，还取决于 profile 的崩溃历史** —— 这正是它不可依赖的原因。
    """
    path = (profile or PROFILE) / 'Default' / 'Preferences'
    if not path.exists():
        return {'preferences_exists': False}
    try:
        data = json.loads(path.read_text(encoding='utf-8', errors='ignore'))
    except Exception as exc:  # noqa: BLE001
        return {'preferences_exists': True, 'parse_error': type(exc).__name__}
    prof = data.get('profile', {}) if isinstance(data, dict) else {}
    return {'preferences_exists': True,
            'exit_type': prof.get('exit_type'),
            'exited_cleanly': prof.get('exited_cleanly')}


def stray_browsers(profile: pathlib.Path | None = None) -> list[dict]:
    """列出**属于该 profile** 的浏览器进程（Windows/WMI；失败返回 `[]`）。

    ⚠️ 探针崩在 `start()` 上会**遗留孤儿实例**占住 profile，导致下一次 `start()`
    直接 `FailedToStartBrowser`（实测）。`M7B-09` 的清理逻辑必须包含这一项。
    """
    target = str(profile or PROFILE).lower().replace("'", "")
    script = ("Get-CimInstance Win32_Process -Filter \"Name='chrome.exe' or Name='msedge.exe'\" "
              f"| Where-Object {{ $_.CommandLine -and $_.CommandLine.ToLower().Contains('{target}') }} "
              "| Select-Object ProcessId, CommandLine | ConvertTo-Json -Compress")
    try:
        done = subprocess.run(['powershell', '-NoProfile', '-Command', script],
                              capture_output=True, text=True, timeout=40, check=False)
        data = json.loads(done.stdout) if done.stdout.strip() else []
    except Exception:  # noqa: BLE001
        return []
    if isinstance(data, dict):
        data = [data]
    rows = []
    for item in data:
        cmd = str(item.get('CommandLine') or '')
        kind = next((tok.split('=', 1)[1] for tok in cmd.split() if tok.startswith('--type=')),
                    'root')
        rows.append({'pid': item.get('ProcessId'), 'kind': kind})
    return sorted(rows, key=lambda r: (r['kind'] != 'root', r['pid'] or 0))


def kill_strays(profile: pathlib.Path | None = None) -> list[int]:
    """强杀该 profile 的残留实例（**只在探针收尾/前置清理时用**；生产逻辑禁止拿它当启动手段）。"""
    killed = []
    for row in stray_browsers(profile):
        pid = row.get('pid')
        if not isinstance(pid, int):
            continue
        try:
            subprocess.run(['taskkill', '/PID', str(pid), '/F', '/T'],
                           capture_output=True, text=True, timeout=20, check=False)
            killed.append(pid)
        except Exception:  # noqa: BLE001
            pass
    return killed


def js_text(reply) -> str:
    """★ 读回自证的关键：Pydoll `execute_script` 返回**两层** `result`。"""
    if not isinstance(reply, dict):
        return ''
    inner = reply.get('result')
    if isinstance(inner, dict):
        inner = inner.get('result')
    if isinstance(inner, dict):
        value = inner.get('value')
        return value if isinstance(value, str) else ''
    return ''


async def assert_read_path(tab) -> str:
    """读回自证：返回 'probe-ok' 才算"通道能读回"（否则一切读数都不可信）。"""
    reply = await tab.execute_script('return "probe-ok";')
    return js_text(reply)


async def press_key(tab, key_name: str) -> dict:
    """按一次功能键（如 `Enter`）。

    ⚠️ `pydoll 2.27.0` 的 `keyboard.press()` 要 **`Key` 枚举**（`pydoll.constants.Key`）；
    传字符串会按元组解包 ⇒ `ValueError: too many values to unpack (expected 2)`
    （2026-10-02 B3 实测踩到）。此处统一转换，并把解析结果留证。
    """
    from pydoll.constants import Key

    key = getattr(Key, str(key_name).upper(), None)
    if key is None:
        raise ValueError(f'pydoll Key 无成员：{key_name!r}')
    await tab.keyboard.press(key)
    return {'key': str(key_name), 'resolved': str(key)}


def norm(cookies) -> list[dict]:
    """CDP Cookie → 精简结构（判定只看名字 + 关键属性）。"""
    out = []
    for cookie in cookies or []:
        if not isinstance(cookie, dict):
            continue
        out.append({
            'name': cookie.get('name'),
            'domain': cookie.get('domain'),
            'path': cookie.get('path'),
            'http_only': bool(cookie.get('httpOnly')),
            'secure': bool(cookie.get('secure')),
            'session': bool(cookie.get('session')),
            'expires': cookie.get('expires'),
            'same_site': cookie.get('sameSite'),
        })
    return sorted(out, key=lambda c: (str(c['name']), str(c['domain'])))


def names(cookies) -> list[str]:
    return sorted({str(c.get('name')) for c in (cookies or []) if isinstance(c, dict)})


def proc_of(browser):
    return browser._browser_process_manager._process  # noqa: SLF001


async def _wait_exit(proc, timeout: float) -> float:
    """等进程退出；返回实际等待秒数（未退出返回 -1）。"""
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        if proc.poll() is not None:
            return round(time.monotonic() - started, 2)
        await asyncio.sleep(0.1)
    return -1.0


async def shutdown(browser, mode: str, wait_s: float = 10.0,
                   cleanup_strays: bool = True) -> dict:
    """按三种方式之一退出浏览器；返回证据字典（含"等退实际耗时"与**残留自检**）。"""
    proc = proc_of(browser)
    info = {'mode': mode, 'exit_code': None, 'waited_s': None}
    if mode == 'close_wait':
        # ★ L1：先请 Chrome 优雅关，再**等它自己退出**（不 kill）
        try:
            await browser._execute_command(  # noqa: SLF001
                {'method': 'Browser.close', 'params': {}})
        except Exception as exc:  # noqa: BLE001 —— 关窗瞬间连接断开属正常
            info['close_reply_error'] = type(exc).__name__
        info['waited_s'] = await _wait_exit(proc, wait_s)
        if info['waited_s'] < 0:                    # 超时才兜底强杀（并留痕）
            proc.kill()
            await asyncio.sleep(1.0)
            info['fallback_kill'] = True
    elif mode == 'pydoll_stop':
        # = 库默认行为：发 Browser.close 后**立刻** terminate（不给落盘时间）
        try:
            await browser.stop()
        except Exception as exc:  # noqa: BLE001
            info['stop_error'] = type(exc).__name__
        if proc.poll() is None:
            await _wait_exit(proc, 5.0)
    elif mode == 'kill_raw':
        proc.kill()                                 # 等价"任务管理器 / 断电"
        await asyncio.sleep(0.5)
    else:
        raise ValueError(mode)
    await _wait_exit(proc, 10.0)
    info['exit_code'] = proc.poll()
    try:
        await browser._connection_handler.close()   # noqa: SLF001
    except Exception:  # noqa: BLE001
        pass
    # ★ 收尾自检（本轮教训）：进程退掉不等于**没有孤儿**（`--type=renderer` / crashpad / 上一次的残骸）。
    #   残留会占住 profile，使下一次 `start()` 直接 `FailedToStartBrowser`（实测）。
    #   ⚠️ 这里强杀只服务探针；生产逻辑**禁止**以强杀解决"占着"（`MB-D0-8` L3）。
    info['strays_after'] = stray_browsers()
    if info['strays_after'] and cleanup_strays:
        info['strays_killed'] = kill_strays()
        info['strays_after2'] = stray_browsers()
    return info


def profile_artifacts() -> dict:
    """profile 落盘物证：Cookies 库 / journal（未提交的 journal = 重启回滚的物证）。"""
    root = PROFILE / 'Default' / 'Network'
    return {
        'cookies_db': (root / 'Cookies').exists(),
        'cookies_journal': (root / 'Cookies-journal').exists(),
        'cookies_db_size': (root / 'Cookies').stat().st_size if (root / 'Cookies').exists() else 0,
    }


def save(name: str, payload: dict) -> pathlib.Path:
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / name
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')
    return path
