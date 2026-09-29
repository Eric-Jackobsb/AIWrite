# -*- coding: utf-8 -*-
"""`_diag_snapshot_session.py` —— **L2 快照能否救回 L1 救不了的会期 Cookie**（`MB-Q6` 收口）。

已知（本轮实测）：
  · 干净退出（L1）→ **持久** Cookie 存活、**会期** Cookie 掉（fresh profile 实证）；
  · `--restore-last-session` 能让会期 Cookie 存活，但**副作用**=重开上次标签页（自动化不可接受）。

本项验 L2 端到端：会期 Cookie 掉了之后，用**快照回灌**能不能复活，且**服务端认账**。
  1) 全新 profile 播三条：`SS_sess`（会期）/ `SS_persist`（持久）/ `SS_http_sess`（会期 + HttpOnly）
  2) 快照 = 裸 `Storage.getCookies`（**全库**；注意 `tab.get_cookies()` 是当前页作用域的，见 `_diag_cookie_scope`）
  3) 干净退出 → 重启（**不带**任何恢复开关）→ 记录谁掉了
  4) **回灌快照**：`tab.set_cookies()`（底层 `Storage.setCookies`；`session:true` 者**不带 expires**）
  5) 服务端 `/eyes` 看 `cookie_header`（证明"认账"，而不是只看浏览器本地库）

证据：`out/diag_snapshot_session.json`。
"""
import asyncio
import json
import pathlib
import shutil
import tempfile

from m7b09_common import js_text, kill_strays, options, save, shutdown, stray_browsers
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

ROOT = pathlib.Path(tempfile.gettempdir()) / 'm7b09_snap'
OURS = ('SS_sess', 'SS_persist', 'SS_http_sess')
KEEP = ('name', 'value', 'domain', 'path', 'secure', 'httpOnly', 'sameSite')


async def dump(tab) -> list[dict]:
    reply = await tab._execute_command({'method': 'Storage.getCookies', 'params': {}})  # noqa: SLF001
    return ((reply.get('result') or {}).get('cookies') or [])


def brief(cookies: list[dict]) -> list[dict]:
    return [{'name': c.get('name'), 'session': bool(c.get('session')),
             'http_only': bool(c.get('httpOnly')), 'expires': c.get('expires')}
            for c in cookies if c.get('name') in OURS]


def restore_param(cookie: dict) -> dict:
    """快照 → 回灌参数：**会期 Cookie 不能带 `expires`**（带了就变成持久 Cookie 了）。"""
    param = {k: cookie[k] for k in KEEP if k in cookie}
    if not cookie.get('session') and cookie.get('expires') is not None:
        param['expires'] = cookie['expires']
    if param.get('sameSite') == 'None':
        param['secure'] = True
    return param


async def main() -> int:
    if stray_browsers():
        kill_strays()
    shutil.rmtree(ROOT, ignore_errors=True)
    ROOT.mkdir(parents=True, exist_ok=True)
    report: dict = {'profile': str(ROOT)}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        opts = dict(profile=ROOT)
        browser = Chrome(options=options(headless=True, **opts))
        tab = await browser.start()
        for name, extra in (('SS_sess', 'session=1'),
                            ('SS_persist', 'max_age=86400'),
                            ('SS_http_sess', 'session=1&http_only=1')):
            await tab.go_to(f'{url}/setcookie?name={name}&value=1&{extra}')
            await asyncio.sleep(0.25)
        snapshot = await dump(tab)
        report['step1_written'] = brief(snapshot)
        report['step2_snapshot_count'] = len(snapshot)
        report['exit'] = await shutdown(browser, 'close_wait')          # ★ L1

        # ---- 重启（无任何恢复开关）：会期 Cookie 应掉 ----
        browser2 = Chrome(options=options(headless=True, **opts))
        tab2 = await browser2.start()
        await tab2.go_to(f'{url}/page?mode=cookies')
        await asyncio.sleep(0.4)
        before_restore = await dump(tab2)
        report['step3_after_restart'] = brief(before_restore)
        report['step3_survivors'] = [c['name'] for c in before_restore if c.get('name') in OURS]

        # ---- L2 回灌 ----
        restore = [restore_param(c) for c in snapshot if c.get('name') in OURS]
        restore_error = None
        try:
            await tab2.set_cookies(restore)         # 底层 = Storage.setCookies（浏览器级）
        except Exception as exc:  # noqa: BLE001
            restore_error = f'{type(exc).__name__}: {exc}'
        await asyncio.sleep(0.5)
        after_restore = await dump(tab2)
        report['step4_restore_params'] = restore
        report['step4_restore_error'] = restore_error
        report['step4_after_restore'] = brief(after_restore)
        await tab2.go_to(f'{url}/eyes')
        await asyncio.sleep(0.4)
        report['step5_server_eyes'] = js_text(await tab2.execute_script(
            'return document.body.innerText;'))
        report['exit2'] = await shutdown(browser2, 'close_wait')

        names_after = [c['name'] for c in after_restore if c.get('name') in OURS]
        report['findings'] = {
            'session_dropped_after_clean_exit': 'SS_sess' not in report['step3_survivors'],
            'persistent_survived_clean_exit': 'SS_persist' in report['step3_survivors'],
            'restore_brought_back_all': sorted(names_after) == sorted(OURS),
            'session_flag_preserved': all(c['session'] for c in report['step4_after_restore']
                                          if c['name'] in ('SS_sess', 'SS_http_sess')),
            'http_only_preserved': next((c['http_only'] for c in report['step4_after_restore']
                                         if c['name'] == 'SS_http_sess'), None),
            'server_recognizes_restored': 'SS_sess=1' in report['step5_server_eyes'],
        }
        print('\n=== 判定（L2 快照 vs 会期 Cookie）===')
        for key, value in report['findings'].items():
            print(f'  {key:<36} = {value}')
        print(f'  step5 服务端 /eyes：{report["step5_server_eyes"][:160]}')
        print(f'\n[_diag_snapshot_session] 证据：{save("diag_snapshot_session.json", report)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
