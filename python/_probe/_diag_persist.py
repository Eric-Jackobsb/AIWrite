# -*- coding: utf-8 -*-
"""M7B-01 关键诊断：**登录态为什么没在重启后保留** —— 关窗方式（强杀 vs 优雅）假设验证。"""
import asyncio
import pathlib

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer

PROFILE = pathlib.Path.home() / '.brain-ai' / 'pydoll-profile'


def options() -> ChromiumOptions:
    opts = ChromiumOptions()
    opts.headless = True
    opts.add_argument(f'--user-data-dir={PROFILE}')
    opts.start_timeout = 60
    return opts


async def pass_set(base: str, graceful: bool) -> list[str]:
    browser = Chrome(options=options())
    try:
        tab = await browser.start()
        await asyncio.sleep(1.0)
        tab = await browser.new_tab(f'{base}/setcookies')
        await asyncio.sleep(0.8)
        names = [c.get('name') for c in await tab.get_cookies()]
        if graceful:
            try:
                await browser._execute_command(  # noqa: SLF001
                    {'method': 'Browser.close', 'params': {}})
            except Exception as exc:  # noqa: BLE001 —— 关窗瞬间连接断开是正常的
                print('  Browser.close 返回异常（正常）：', type(exc).__name__)
            await asyncio.sleep(3.0)
        return names
    finally:
        if not graceful:
            await browser.stop()
        else:
            try:
                await browser.stop()
            except Exception:  # noqa: BLE001
                pass


async def pass_read(base: str) -> list[str]:
    browser = Chrome(options=options())
    try:
        tab = await browser.start()
        await asyncio.sleep(1.0)
        tab = await browser.new_tab(f'{base}/page?mode=cookies')
        await asyncio.sleep(0.8)
        return [c.get('name') for c in await tab.get_cookies()]
    finally:
        await browser.stop()


async def main() -> int:
    with LocalServer() as server:
        base = f'http://127.0.0.1:{server.port}'

        print('--- 场景 A：强杀（现状，`browser.stop()`）---')
        set_a = await pass_set(base, graceful=False)
        read_a = await pass_read(base)
        print('  写入后:', set_a, '｜重启后:', read_a)

        print('--- 场景 B：优雅关闭（CDP `Browser.close`）---')
        set_b = await pass_set(base, graceful=True)
        read_b = await pass_read(base)
        print('  写入后:', set_b, '｜重启后:', read_b)

        print('\n=== 判定 ===')
        print('  强杀后持久 Cookie 保留:', 'm7b_plain' in read_a or 'm7b_http' in read_a)
        print('  优雅关闭后持久 Cookie 保留:', 'm7b_plain' in read_b or 'm7b_http' in read_b)
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
