# -*- coding: utf-8 -*-
"""`M7B-09` / V-b —— **Cookie 快照往返**（决议 `MB-D0-8` L2 的可行性闸门）。

问题：把「登录态」用 CDP 取出来存成一份快照，**注销后再回灌**，站点还认不认账？

步骤（全本地自证）：
  1. 建登录态（`/setcookies` 写 HttpOnly + 持久 + 会话；`/page?mode=cookies` 的 JS 再写一条）
  2. `Storage.getCookies` 取快照（**浏览器级 · 全 origin · 含 HttpOnly**）
  3. `/eyes` 记录**服务端视角**（站点是否认账的基线）
  4. **注销**：`Storage.clearCookies` → 断言浏览器库空 + 服务端看不到
  5. **回灌**：`Storage.setCookies(快照)` → 断言 ① 名字齐 ② `httpOnly` 保持
     ③ `document.cookie` **仍看不到** HttpOnly ④ **服务端 `/eyes` 重新看得到**（真认账）
  6. 干净退出（`close_wait`）

判定：`roundtrip_ok` = 名字齐 + httpOnly 保持 + 服务端认账（三者同时成立 ⇒ L2 技术可行）。
证据：`out/m7b09_snapshot_roundtrip.json`（含快照原文 —— 仅探针用；真实现必须 DPAPI 加密）。
"""
import asyncio

from m7b09_common import (PROFILE, assert_read_path, js_text, names, norm,
                          options, save, shutdown)
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

# CDP `Cookie` → `SetCookieParams` 的安全白名单（不白名单会把 `size` / `session` 等只读字段回传报错）
SETTABLE = ('name', 'value', 'domain', 'path', 'secure', 'httpOnly',
            'sameSite', 'priority', 'sourceScheme', 'sourcePort')


def to_params(cookies) -> list[dict]:
    params = []
    for cookie in cookies or []:
        if not isinstance(cookie, dict) or not cookie.get('name'):
            continue
        param = {key: cookie[key] for key in SETTABLE if key in cookie}
        expires = cookie.get('expires')
        # 会期 Cookie（expires<=0）回灌时**不带** expires（保持"会期"语义，不偷偷升级为持久）
        if isinstance(expires, (int, float)) and expires > 0:
            param['expires'] = float(expires)
        params.append(param)
    return params


async def eyes(tab, url: str) -> str:
    """服务端视角：请求带的 Cookie 头（真"站点认账"证据）。"""
    await tab.go_to(f'{url}/eyes')
    await asyncio.sleep(0.2)
    return js_text(await tab.execute_script('return document.body.innerText;'))


async def main() -> int:
    report: dict = {'profile': str(PROFILE), 'steps': {}}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        print(f'[V-b] 本地桩：{url}')

        browser = Chrome(options=options(headless=True))
        try:
            tab = await browser.start()
            report['self_check'] = await assert_read_path(tab)     # 读回自证

            # 1) 建登录态
            await tab.go_to(f'{url}/setcookies')
            await asyncio.sleep(0.4)
            await tab.go_to(f'{url}/page?mode=cookies')            # JS 写 m7b_js
            await asyncio.sleep(0.4)

            # 2) 取快照
            snapshot = await tab.get_cookies()
            params = to_params(snapshot)
            report['steps']['snapshot'] = {'names': names(snapshot), 'cookies': norm(snapshot),
                                           'param_count': len(params), 'params': params}
            print(f'  1-2) 快照 {len(snapshot)} 条：{names(snapshot)}')

            # 3) 服务端基线
            eyes_before = await eyes(tab, url)
            report['steps']['eyes_before'] = eyes_before
            print(f'  3) 服务端基线：{eyes_before[:120]}')

            # 4) 注销
            await tab.delete_all_cookies()
            await asyncio.sleep(0.4)
            after_clear = await tab.get_cookies()
            eyes_after_clear = await eyes(tab, url)
            report['steps']['after_clear'] = {'names': names(after_clear),
                                              'eyes': eyes_after_clear}
            print(f'  4) 注销后：浏览器库={names(after_clear)} · 服务端={eyes_after_clear[:80]}')

            # 5) 回灌
            await tab.set_cookies(params)
            await asyncio.sleep(0.5)
            restored = await tab.get_cookies()
            doc_cookie = js_text(await tab.execute_script('return document.cookie;'))
            eyes_after = await eyes(tab, url)
            restored_http = {c['name']: c['http_only'] for c in norm(restored)}
            report['steps']['restore'] = {
                'names': names(restored), 'cookies': norm(restored),
                'document_cookie': doc_cookie, 'eyes': eyes_after,
                'http_only_map': restored_http,
            }
            print(f'  5) 回灌后：浏览器库={names(restored)}')
            print(f'     document.cookie={doc_cookie[:80]}')
            print(f'     服务端={eyes_after[:100]}')

            # 6) 干净退出
            report['shutdown'] = await shutdown(browser, 'close_wait')
        finally:
            pass

        before = set(report['steps']['snapshot']['names'])
        after = set(report['steps']['restore']['names'])
        eyes_after = report['steps']['restore']['eyes']
        report['findings'] = {
            'names_restored': before == after,
            'missing_after_restore': sorted(before - after),
            'http_only_preserved': report['steps']['restore']['http_only_map'].get('m7b_http') is True,
            'document_cookie_still_hides_httponly': 'm7b_http' not in report['steps']['restore']['document_cookie'],
            'server_still_recognizes': 'm7b_http' in eyes_after and 'm7b_plain' in eyes_after,
            'roundtrip_ok': (before == after
                             and report['steps']['restore']['http_only_map'].get('m7b_http') is True
                             and 'm7b_http' in eyes_after),
        }
        print('\n=== 判定（L2 可行性）===')
        for key, value in report['findings'].items():
            print(f'  {key:<38} = {value}')
        path = save('m7b09_snapshot_roundtrip.json', report)
        print(f'\n[V-b] 证据：{path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
