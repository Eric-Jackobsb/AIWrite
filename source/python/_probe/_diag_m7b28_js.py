# -*- coding: utf-8 -*-
"""M7B-28 诊断：`execute_script` 的**真实返回形状**（决定 `js_text` 该怎么解）。"""
import asyncio

from pydoll.browser.chromium import Chrome

import m7b09_common as common

CASES = [
    ('str', 'return "probe-ok";'),
    ('jsonstr', 'return JSON.stringify({a:1});'),
    ('iife-expr', '(function (s) { return JSON.stringify({ "#file": document.querySelectorAll(s).length }); })("#file");'),
    ('obj', 'return {a:1};'),
    ('num', 'return document.querySelectorAll("body").length;'),
]


async def main() -> int:
    browser = Chrome(options=common.options(headless=True))
    try:
        tab = await browser.start()
        await tab.go_to('data:text/html,<html><body><input id="file" type="file"><textarea id="box"></textarea></body></html>')
        await asyncio.sleep(0.5)
        for label, script in CASES:
            reply = await tab.execute_script(script)
            print(f'--- {label} ---')
            print(f'  类型 : {type(reply).__name__}')
            print(f'  原始 : {repr(reply)[:400]}')
            print(f'  js_text(common) = {common.js_text(reply)!r}')
            body = getattr(reply, 'result', None)
            print(f'  getattr(.result) = {repr(body)[:200]}')
    finally:
        info = await common.shutdown(browser, 'close_wait')
        print(f'[收尾] {info.get("mode")} / 残留 {len(info.get("strays_after") or [])}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
