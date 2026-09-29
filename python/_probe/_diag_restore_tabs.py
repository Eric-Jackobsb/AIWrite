# -*- coding: utf-8 -*-
"""`_diag_restore_tabs.py` —— `--restore-last-session` 的**副作用**：会重开上次的标签页吗？

V-d v2 已证：`--restore-last-session` 是让**会期 Cookie**跨重启存活的**唯一有效手段**
（`session.restore_on_startup=1` 写 Preferences 无效）。但它按语义还有"恢复上次会话"的作用 ——
若成立，我们每次启动工具都会**把用户上次的页面重新拉起来**（意外弹窗 / 意外请求 / 意外副作用）。

做法（每轮全新 profile，干净退出 `close_wait`）：
  A. 不带开关：开两个页（本地桩 marker 页 + about:blank）→ 干净退出 → 重启 → 看 `get_opened_tabs()`
  B. 带开关：同上
判定：B 的重启结果里出现 marker URL ⇒ **确有副作用**（须记入 `M7B-09`，并作为 L2 快照的必要性论据）。

证据：`out/diag_restore_tabs.json`。
"""
import asyncio
import pathlib
import shutil
import tempfile

from m7b09_common import kill_strays, options, save, shutdown, stray_browsers
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

ROOT = pathlib.Path(tempfile.gettempdir()) / 'm7b09_restore'
MARK = 'm7b_restore_marker'


async def one_round(label: str, args: tuple[str, ...], url: str) -> dict:
    profile = ROOT / label
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir(parents=True, exist_ok=True)
    opts = dict(extra_args=args, profile=profile)

    browser = Chrome(options=options(headless=True, **opts))
    tab = await browser.start()
    await tab.go_to(f'{url}/page?mode={MARK}')
    marker_tab = tab
    try:
        await browser.new_tab(f'{url}/page?mode={MARK}_2')
        opened_before = [t.target_id for t in await browser.get_opened_tabs()]
    except Exception as exc:  # noqa: BLE001
        opened_before = [f'new_tab 不可用：{type(exc).__name__}']
    await shutdown(browser, 'close_wait')

    browser2 = Chrome(options=options(headless=True, **opts))
    await browser2.start()
    await asyncio.sleep(1.5)
    tabs = await browser2.get_opened_tabs()
    urls = []
    for item in tabs:
        try:
            urls.append(await item.current_url)
        except Exception:  # noqa: BLE001
            urls.append('<读不到>')
    await shutdown(browser2, 'close_wait')
    result = {'label': label, 'extra_args': list(args), 'tabs_before': len(opened_before),
              'tabs_after': len(tabs), 'urls_after': urls,
              'marker_restored': any(MARK in (u or '') for u in urls)}
    print(f'  [{label:<18}] 重启后 {len(tabs)} 个标签页 · marker 恢复='
          f'{"是" if result["marker_restored"] else "否"} · {urls}')
    return result


async def main() -> int:
    if stray_browsers():
        kill_strays()
    shutil.rmtree(ROOT, ignore_errors=True)
    report: dict = {'root': str(ROOT), 'marker': MARK}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        report['without_switch'] = await one_round('without_switch', (), url)
        report['with_switch'] = await one_round('with_switch', ('--restore-last-session',), url)
    report['findings'] = {
        'baseline_restores_tabs': report['without_switch']['marker_restored'],
        'switch_restores_tabs': report['with_switch']['marker_restored'],
        'switch_has_tab_side_effect': (report['with_switch']['marker_restored']
                                       and not report['without_switch']['marker_restored']),
    }
    print('\n=== 判定（副作用）===')
    for key, value in report['findings'].items():
        print(f'  {key:<30} = {value}')
    print(f"\n[_diag_restore_tabs] 证据：{save('diag_restore_tabs.json', report)}")
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
