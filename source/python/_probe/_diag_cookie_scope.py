# -*- coding: utf-8 -*-
"""`_diag_cookie_scope.py` —— 证清 `tab.get_cookies()` 的**作用域**（`M7B-09` 关键实现注意点）。

源码事实（`.venv/.../pydoll/browser/tab.py:882-898`）：
    `get_cookies()` 若 `_browser_context_id` 有值 → CDP `Storage.getCookies`（**全库**）；
    否则 → `Network.getCookies()` **不带 urls** ⇒ 按 CDP 定义"**当前页面 URL** 的 Cookie"。

⇒ 推论：**登录态检测必须先导航到目标站点，否则会误报"未登录"**（V-c 的"attach 读到空"即此因）。

链条取证（一次性本地桩，全部自证）：
  ① 直起 Chrome（我们的 profile + 固定端口，落地页 `about:blank`）→ attach
  ② attach 后**不导航**读 `get_cookies()`            → 期望 `[]`（空读复现）
  ③ 导航到本地桩页面后再读                            → 期望拿到该域 Cookie（>0）
  ④ 同连接上发裸命令 `Storage.getCookies`（无 browserContextId） → 期望**全库**
  ⑤ `Network.getCookies(urls=[本地桩])`               → 期望只回该 URL 的
"""
import asyncio
import json
import subprocess
import time
import urllib.request

from m7b09_common import (PROFILE, kill_strays, names, options, save, stray_browsers)
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

PORT = 9336


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


async def main() -> int:
    if stray_browsers():
        kill_strays()
    report: dict = {'profile': str(PROFILE)}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        # 先用正常 Pydoll 写一条 Cookie 进库
        seed = Chrome(options=options(headless=True))
        st = await seed.start()
        await st.go_to(f'{url}/setcookie?name=SCOPE_probe&value=1&max_age=86400')
        await asyncio.sleep(0.4)
        try:
            await seed._execute_command({'method': 'Browser.close', 'params': {}})  # noqa: SLF001
        except Exception:  # noqa: BLE001
            pass
        for _ in range(40):
            if PROFILE.exists() and not stray_browsers():
                break
            await asyncio.sleep(0.25)
        await asyncio.sleep(2.0)

        chrome = Chrome(options=options(headless=True))._get_default_binary_location()  # noqa: SLF001
        proc = subprocess.Popen(
            [chrome, f'--user-data-dir={PROFILE}', f'--remote-debugging-port={PORT}',
             '--no-first-run', '--no-default-browser-check', 'about:blank'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            version = wait_devtools() or {}
            await asyncio.sleep(1.5)
            browser = Chrome(options=options(headless=True))
            tab = await browser.connect(version.get('webSocketDebuggerUrl'))
            report['url_before_nav'] = await tab.current_url
            report['step2_before_nav'] = names(await tab.get_cookies())
            storage = await tab._execute_command(  # noqa: SLF001
                {'method': 'Storage.getCookies', 'params': {}})
            report['step4_storage_all'] = names((storage.get('result') or {}).get('cookies') or [])
            net = await tab._execute_command(  # noqa: SLF001
                {'method': 'Network.getCookies', 'params': {'urls': [f'{url}/page']}})
            report['step5_network_with_url'] = names((net.get('result') or {}).get('cookies') or [])
            await tab.go_to(f'{url}/page?mode=cookies')
            await asyncio.sleep(0.5)
            report['url_after_nav'] = await tab.current_url
            report['step3_after_nav'] = names(await tab.get_cookies())
            print(f'  ①落地页={report["url_before_nav"]}')
            print(f'  ②不导航读 get_cookies()      = {report["step2_before_nav"]}  （期望空）')
            print(f'  ③导航后再读 get_cookies()    = {report["step3_after_nav"]}  （期望非空）')
            print(f'  ④Storage.getCookies（全库）  = {report["step4_storage_all"]}')
            print(f'  ⑤Network.getCookies(urls=…)  = {report["step5_network_with_url"]}')
            report['findings'] = {
                'empty_on_about_blank': report['step2_before_nav'] == [],
                'nonempty_after_nav': bool(report['step3_after_nav']),
                'storage_returns_whole_db': 'SCOPE_probe' in report['step4_storage_all'],
            }
            print('\n=== 判定（读取作用域）===')
            for key, value in report['findings'].items():
                print(f'  {key:<32} = {value}')
        finally:
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
            report['strays_after'] = stray_browsers()
            if report['strays_after']:
                report['strays_killed'] = kill_strays()
            print(f'\n[_diag_cookie_scope] 证据：{save("diag_cookie_scope.json", report)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
