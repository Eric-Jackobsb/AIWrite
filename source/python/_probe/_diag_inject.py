# -*- coding: utf-8 -*-
"""M7B-03 诊断：`Page.addScriptToEvaluateOnNewDocument` 注入是否真的生效。"""
import asyncio

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer


async def main() -> int:
    opts = ChromiumOptions()
    opts.headless = True
    with LocalServer() as server:
        async with Chrome(options=opts) as browser:
            tab = await browser.start()
            enable = await tab._execute_command({'method': 'Page.enable', 'params': {}})  # noqa: SLF001
            inject = await tab._execute_command({  # noqa: SLF001
                'method': 'Page.addScriptToEvaluateOnNewDocument',
                'params': {'source': 'window.__m7b_marker = 42;'}})
            print('Page.enable 返回:', enable)
            print('注入返回     :', inject)

            await tab.go_to(f'http://127.0.0.1:{server.port}/page?mode=fetch')
            marker = await tab.execute_script('return String(window.__m7b_marker);')
            fetch_src = await tab.execute_script('return String(window.fetch).slice(0, 60);')
            got = await tab.execute_script('return String(window.__got);')
            print('注入标记     :', getattr(marker, "result", marker))
            print('fetch 片段   :', getattr(fetch_src, "result", fetch_src))
            print('页面收到的块数:', getattr(got, "result", got))
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
