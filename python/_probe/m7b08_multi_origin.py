# -*- coding: utf-8 -*-
"""M7B-08 前置验证 —— **单 profile 多站点并存**（多 tab）+ **按 origin 注销**。

验收（M7B.md §5）：两站独立登录；注销 A 不动 B。
用两个 localhost **不同端口**（= 两个 origin）自证，**单浏览器 + 单 profile**（`~/.brain-ai/pydoll-profile`）。
"""
import asyncio
import json
import pathlib

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer

PROFILE = pathlib.Path.home() / '.brain-ai' / 'pydoll-profile'
OUT = pathlib.Path(__file__).parent / 'out'


def cmd(method: str, **params) -> dict:
    return {'method': method, 'params': params}


def names(cookies: list[dict]) -> list[str]:
    return sorted(str(c.get('name', '')) for c in cookies if str(c.get('name', '')).startswith('m7b'))


def js_text(raw) -> str:
    """从 `execute_script` 原始返回取字符串（返回形状为两层 `result`）。"""
    node = getattr(raw, 'result', raw)
    for _ in range(3):
        if isinstance(node, dict) and isinstance(node.get('result'), dict):
            node = node['result']
        else:
            break
    return str(node.get('value', '')) if isinstance(node, dict) else str(node)


async def main() -> int:
    OUT.mkdir(exist_ok=True)
    opts = ChromiumOptions()
    opts.headless = True
    opts.add_argument(f'--user-data-dir={PROFILE}')
    report: dict = {'profile': str(PROFILE)}

    with LocalServer() as server_a, LocalServer() as server_b:
        # ⚠️ Cookie 按**域名**隔离（端口不参与）：两个 `127.0.0.1:不同端口` 仍是**同一个 Cookie 域**！
        #    → 必须用两个不同**主机名**才能演示「两站独立」：`127.0.0.1`（A）与 `localhost`（B）。
        origin_a = server_a.url_host('127.0.0.1', '')
        origin_b = server_b.url_host('localhost', '')
        report['origins'] = {'a': origin_a, 'b': origin_b,
                             'note': 'Cookie 按域名隔离（端口不参与）→ 用 127.0.0.1 与 localhost'}

        async with Chrome(options=opts) as browser:
            tab_a = await browser.start()
            await tab_a.go_to(f'{origin_a}/setcookies')
            tab_b = await browser.new_tab(f'{origin_b}/setcookies')
            await asyncio.sleep(0.8)

            report['before'] = {'tab_a_view': names(await tab_a.get_cookies()),
                                'tab_b_view': names(await tab_b.get_cookies()),
                                'browser_view': names(await browser.get_cookies())}

            # 注销 A：按 **origin** 清数据（cookies + 同源存储）——不动 B
            response = await tab_a._execute_command(  # noqa: SLF001
                cmd('Storage.clearDataForOrigin', origin=origin_a,
                    storageTypes='cookies,local_storage,session_storage,indexeddb,cache_storage'))
            await asyncio.sleep(0.6)

            report['after'] = {'tab_a_view': names(await tab_a.get_cookies()),
                               'tab_b_view': names(await tab_b.get_cookies()),
                               'browser_view': names(await browser.get_cookies()),
                               'clear_response': response.get('result', response)}
            report['b_page_cookie'] = js_text(await tab_b.execute_script('return document.cookie;'))

    before = report['before']
    after = report['after']
    # ⚠️ 判定要点：Cookie **按域名**隔离，A/B 两域同名 → 不能靠名字比对，
    #    必须看**各自视角**：A 视角清空 + B 视角完整。
    verdict = {
        'single_profile_both_origins': len(before['browser_view']) >= 4,
        'clear_a_removes_a_cookies': after['tab_a_view'] == [],
        'clear_a_keeps_b_cookies': sorted(after['tab_b_view']) == sorted(before['tab_b_view']) and
        len(after['tab_b_view']) > 0,
        'b_page_still_has_cookie': 'm7b_plain' in str(report['b_page_cookie']),
    }
    report['verdict'] = verdict
    (OUT / 'm7b08_multi_origin.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    print(f'[M7B-08] profile = {PROFILE}')
    print(f'[M7B-08] A（{origin_a}）视角 Cookie：{before["tab_a_view"]}')
    print(f'[M7B-08] B（{origin_b}）视角 Cookie：{before["tab_b_view"]}')
    print(f'[M7B-08] 全库视角（两站并存）：{before["browser_view"]}')
    print(f'[M7B-08] 注销 A 后 —— A 视角：{after["tab_a_view"]}｜B 视角：{after["tab_b_view"]}')
    print(f'[M7B-08] 注销 A 后 —— 全库：{after["browser_view"]}')
    print(f'[M7B-08] B 页面 document.cookie：{report["b_page_cookie"]}')
    print('=== 判定 ===')
    for key, value in verdict.items():
        print(f'  {key:32} {"PASS" if value else "FAIL"}')
    print(f'[M7B-08] 证据：{OUT / "m7b08_multi_origin.json"}')
    return 0 if all(verdict.values()) else 1


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
