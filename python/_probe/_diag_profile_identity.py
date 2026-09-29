# -*- coding: utf-8 -*-
"""`_diag_profile_identity.py` —— 查清 V-c 的第二个异常：**直起 Chrome 时 profile 会被悄悄换掉**？

V-c 里：刚用 Pydoll 关掉 profile（`close_wait`，仅 0.2 s）→ 立刻直起 Chrome（同 `--user-data-dir`，
固定端口）→ DevTools 就绪、能 attach、**但 `get_cookies()` 返回空**。
`_diag_attach.py`（隔了很久再起）同一套流程却能读到 12 条 ⇒ 疑点指向**profile 锁竞争**：
上一个实例尚未完全释放，Chrome 的 ProcessSingleton 走了"已在使用"分支，结果这个"残留实例"
跑的不是我们的 profile（于是 Cookie 库为空）。

决定性取证：attach 后打开 `chrome://version`，直接读它的 **Profile Path / Command Line**。
两轮对照：`cold`（隔 6 s 再起）= 期望正常；`race`（关掉后立刻起）= 期望复现异常。

证据：`out/diag_profile_identity.json`。
"""
import asyncio
import json
import subprocess
import time
import urllib.request

from m7b09_common import (PROFILE, js_text, kill_strays, names, options, save,
                          shutdown, stray_browsers)
from pydoll.browser.chromium import Chrome

PORT = 9335


def http_json(path: str, timeout: float = 2.0):
    with urllib.request.urlopen(f'http://127.0.0.1:{PORT}{path}', timeout=timeout) as resp:
        return json.loads(resp.read().decode('utf-8'))


def wait_devtools(timeout: float = 20.0):
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        try:
            return http_json('/json/version')
        except Exception:  # noqa: BLE001
            time.sleep(0.4)
    return None


def pick(text: str, key: str) -> str:
    for line in text.splitlines():
        if line.strip().startswith(key):
            return line.strip()
    return ''


async def round_trip(label: str, delay: float) -> dict:
    """Pydoll 启停一次 profile →（等 delay）直起 Chrome → attach → 读身份 + Cookie。"""
    browser = Chrome(options=options(headless=False))
    tab = await browser.start()
    await tab.go_to('about:blank')
    await shutdown(browser, 'close_wait')
    await asyncio.sleep(delay)

    chrome = Chrome(options=options(headless=True))._get_default_binary_location()  # noqa: SLF001
    proc = subprocess.Popen(
        [chrome, f'--user-data-dir={PROFILE}', f'--remote-debugging-port={PORT}',
         '--no-first-run', '--no-default-browser-check', 'about:blank'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result: dict = {'label': label, 'delay_s': delay, 'direct_pid': proc.pid}
    try:
        version = wait_devtools() or {}
        await asyncio.sleep(1.5)
        result.update({'devtools_ok': bool(version), 'browser': version.get('Browser')})
        attached = Chrome(options=options(headless=True))
        atab = await attached.connect(version.get('webSocketDebuggerUrl'))
        result['attach_cookie_names'] = names(await atab.get_cookies())
        await atab.go_to('chrome://version')
        await asyncio.sleep(0.6)
        text = js_text(await atab.execute_script('return document.body.innerText;'))
        result['profile_path'] = pick(text, 'Profile Path')
        result['command_line'] = pick(text, 'Command Line')
        result['command_line_has_our_profile'] = str(PROFILE) in result['command_line']
        print(f'  [{label}] attach ✓ · Cookie={len(result["attach_cookie_names"])} 条 · '
              f'{result["profile_path"] or "（chrome://version 未取到）"}')
        print(f'         {result["command_line"][:150]}')
    except Exception as exc:  # noqa: BLE001
        result['attach_error'] = f'{type(exc).__name__}: {exc}'
        print(f'  [{label}] 失败/异常：{result["attach_error"]}')
    finally:                                    # ★ 任何分支都必须收尾（否则留下孤儿占住 profile）
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json/close/{proc.pid}', timeout=1.0):
                pass
        except Exception:  # noqa: BLE001
            pass
        for _ in range(24):
            if proc.poll() is not None:
                break
            await asyncio.sleep(0.25)
        if proc.poll() is None:
            proc.kill()
        result['cleanup_exit_code'] = proc.poll()
        result['strays_after'] = stray_browsers()
        if result['strays_after']:              # 兜底：真的退不掉才强杀
            result['strays_killed'] = kill_strays()
    return result


async def main() -> int:
    report: dict = {'profile': str(PROFILE)}
    report['strays_before'] = stray_browsers()
    if report['strays_before']:
        report['strays_killed_at_start'] = kill_strays()
        print(f'  前置清理：杀掉残留 {len(report["strays_killed_at_start"])} 个进程')
    report['cold'] = await round_trip('cold', 6.0)      # 上一实例完全释放
    report['race'] = await round_trip('race', 0.0)      # 关掉后立刻起（复现 V-c）
    cold, race = report['cold'], report['race']
    report['findings'] = {
        'cold_attach_uses_our_profile': cold.get('command_line_has_our_profile'),
        'cold_attach_cookie_count': len(cold.get('attach_cookie_names') or []),
        'race_attach_uses_our_profile': race.get('command_line_has_our_profile'),
        'race_attach_cookie_count': len(race.get('attach_cookie_names') or []),
        'profile_swap_reproduced': (race.get('attach_cookie_names') == []
                                    and (cold.get('attach_cookie_names') or []) != []),
    }
    print('\n=== 判定（profile 锁竞争）===')
    for key, value in report['findings'].items():
        print(f'  {key:<34} = {value}')
    print(f'\n[_diag_profile_identity] 证据：{save("diag_profile_identity.json", report)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
