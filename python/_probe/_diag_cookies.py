# -*- coding: utf-8 -*-
"""M7B-01 诊断：Cookie 为何读不到 —— 服务器侧 vs 浏览器侧。"""
import asyncio
import urllib.request

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer


async def main() -> int:
    with LocalServer() as server:
        base = f'http://127.0.0.1:{server.port}'

        # ---- ① 服务器侧：Set-Cookie 头是否真的发出 ----
        with urllib.request.urlopen(f'{base}/setcookies') as response:
            print('HTTP', response.status, '→ Set-Cookie:',
                  response.headers.get_all('Set-Cookie'))

        # ---- ② 浏览器侧：new_tab 是否真的导航 + Cookie 是否落库 ----
        opts = ChromiumOptions()
        opts.headless = True
        opts.add_argument('--no-first-run')
        async with Chrome(options=opts) as browser:
            tab = await browser.start()
            await asyncio.sleep(1.0)
            tab2 = await browser.new_tab(f'{base}/setcookies')
            await asyncio.sleep(1.2)
            print('new_tab 后 URL :', getattr(tab2, 'current_url', 'n/a'))
            print('tab2 cookies   :', [c.get('name') for c in await tab2.get_cookies()])
            await tab2.go_to(f'{base}/page?mode=cookies')
            await asyncio.sleep(0.8)
            print('go_to 后 URL   :', getattr(tab2, 'current_url', 'n/a'))
            doc = await tab2.execute_script('return document.cookie;')
            payload = getattr(doc, 'result', doc)
            print('document.cookie:', payload.get('result', {}).get('value', '') if isinstance(payload, dict) else payload)
            print('tab2 cookies#2 :', [c.get('name') for c in await tab2.get_cookies()])
            print('browser cookies:', [c.get('name') for c in await browser.get_cookies()])
            print('opened tabs    :', len(await browser.get_opened_tabs()))
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
