# -*- coding: utf-8 -*-
"""`_diag_attach.py` —— 查清 V-c 的一个异常：**attach 成功但 `get_cookies()` 返回空**。

假设：
  H1 attach 落在**不同的 browser context**（`Storage.getCookies()` 默认只回默认上下文）
  H2 直起的"残留实例"其实用的是**另一个 profile / 空 profile**
  H3 读时机太早（Cookie 库尚未加载）

做法：直起 Chrome（我们的 profile + 固定端口）→ attach → 依次取证：
  `/json/list`（tab 数 / URL）· `/json/version`（profile 路径?）· `tab.get_cookies()` ·
  服务端 `/eyes`（**真认账**证据）· `document.cookie`。
"""
import asyncio
import json
import subprocess
import time
import urllib.request

from m7b09_common import PROFILE, js_text, names, options, save
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

PORT = 9334


def http_json(path: str, timeout: float = 2.0):
    with urllib.request.urlopen(f'http://127.0.0.1:{PORT}{path}', timeout=timeout) as resp:
        return json.loads(resp.read().decode('utf-8'))


def wait_devtools(timeout: float = 25.0):
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        try:
            return http_json('/json/version')
        except Exception:  # noqa: BLE001
            time.sleep(0.4)
    return None


async def main() -> int:
    report: dict = {'profile': str(PROFILE)}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        chrome = Chrome(options=options(headless=True))._get_default_binary_location()  # noqa: SLF001
        proc = subprocess.Popen(
            [chrome, f'--user-data-dir={PROFILE}', f'--remote-debugging-port={PORT}',
             '--no-first-run', '--no-default-browser-check', f'{url}/setcookie?name=ATTACH_probe&value=1&max_age=86400'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        version = wait_devtools() or {}
        report['version'] = {k: version.get(k) for k in ('Browser', 'Protocol-Version', 'webSocketDebuggerUrl')}
        await asyncio.sleep(2.0)                     # H3：给它时间落盘
        report['json_list'] = [{'id': t.get('id'), 'type': t.get('type'), 'url': t.get('url')}
                               for t in (http_json('/json/list') or [])]
        print('  DevTools:', report['version'].get('Browser'), '｜ tabs:', len(report['json_list']))

        browser = Chrome(options=options(headless=True))
        tab = await browser.connect(version.get('webSocketDebuggerUrl'))
        cookies_first = await tab.get_cookies()
        report['attach_cookies_first'] = {'names': names(cookies_first)}
        await asyncio.sleep(1.0)
        cookies_second = await tab.get_cookies()
        await tab.go_to(f'{url}/eyes')
        await asyncio.sleep(0.5)
        eyes = js_text(await tab.execute_script('return document.body.innerText;'))
        doc_cookie = js_text(await tab.execute_script('return document.cookie;'))
        report['attach_cookies_second'] = {'names': names(cookies_second)}
        report['eyes'] = eyes
        report['document_cookie'] = doc_cookie
        print(f"  attach 读库：第1次={names(cookies_first)} · 第2次={names(cookies_second)}")
        print(f'  服务端 /eyes：{eyes[:140]}')
        print(f'  document.cookie：{doc_cookie[:100]}')

        # 对照：正常 Pydoll 启动（同 profile）读库
        b2 = Chrome(options=options(headless=True))
        tab2 = await b2.start()
        ref = await tab2.get_cookies()
        report['normal_launch_cookies'] = {'names': names(ref)}
        try:
            await b2._execute_command({'method': 'Browser.close', 'params': {}})  # noqa: SLF001
        except Exception:  # noqa: BLE001
            pass
        print(f'  对照（正常启动同 profile）：{names(ref)}')

        try:
            await browser._execute_command({'method': 'Browser.close', 'params': {}})  # noqa: SLF001
        except Exception:  # noqa: BLE001
            pass
        for _ in range(40):
            if proc.poll() is not None:
                break
            await asyncio.sleep(0.25)
        if proc.poll() is None:
            proc.kill()
        path = save('diag_attach.json', report)
        print(f'\n[_diag_attach] 证据：{path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
