# -*- coding: utf-8 -*-
"""`M7B-09` / V-d（v2 · **每个 case 用全新 profile**）—— **会话 Cookie 能否跨重启**（`MB-Q6` 判据）。

⚠️ v1 的教训：v1 在**长期使用的 profile** 上测，结果**全部"存活"** —— 原因是该 profile
经历过多次强杀，`Preferences.profile.exit_type` 处于 **Crashed**，Chromium 把会期 Cookie
当作**崩溃恢复**保留了下来。⇒ **"会期 Cookie 能否跨重启"不仅取决于是否干净退出，
还取决于 profile 的崩溃历史**（这正是它不可依赖的原因）。
v2 因此**每个 case 一个全新 profile**（用完即删），并逐轮记录 `exit_type`。

要回答的问题：全新 profile + **干净退出**后，会期 Cookie 掉不掉？若不掉，是哪个手段起的作用？
  baseline / restore_switch（`--restore-last-session`）/ prefs_restore（`session.restore_on_startup=1`）
  / switch_and_prefs（两者同开）

证据：`out/m7b09_session_cookie.json`。
"""
import asyncio
import pathlib
import shutil
import tempfile

from m7b09_common import (PROFILE, assert_read_path, exit_state, names, norm,
                          options, save, shutdown)
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

CASES = (
    ('baseline', None, ()),
    ('restore_switch', None, ('--restore-last-session',)),
    ('prefs_restore', {'session': {'restore_on_startup': 1}}, ()),
    ('switch_and_prefs', {'session': {'restore_on_startup': 1}}, ('--restore-last-session',)),
)
ROOT = pathlib.Path(tempfile.gettempdir()) / 'm7b09_vd'


def fresh_profile(label: str) -> pathlib.Path:
    """每个 case 一个**全新** profile（消除崩溃历史对会期 Cookie 的影响）。"""
    path = ROOT / label
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)
    return path


async def one_case(label: str, prefs, args, url: str) -> dict:
    profile = fresh_profile(label)
    sess = f'SESS_{label}'
    pers = f'PERS_{label}'
    opts = dict(prefs=prefs, extra_args=args, profile=profile)

    # 1) 写：一条会期 Cookie（无 Max-Age）+ 一条持久 Cookie（对照）
    browser = Chrome(options=options(headless=True, **opts))
    tab = await browser.start()
    self_check = await assert_read_path(tab)
    await tab.go_to(f'{url}/setcookie?name={sess}&value=1&session=1')
    await asyncio.sleep(0.3)
    await tab.go_to(f'{url}/setcookie?name={pers}&value=1&max_age=86400')
    await asyncio.sleep(0.4)
    before = await tab.get_cookies()
    state_before = exit_state(profile)
    exit_info = await shutdown(browser, 'close_wait')      # ★ 干净退出（L1）
    state_after = exit_state(profile)

    # 2) 重启读（同 profile / 同 prefs / 同开关 —— 恢复语义依赖它们）
    browser2 = Chrome(options=options(headless=True, **opts))
    tab2 = await browser2.start()
    await tab2.go_to(f'{url}/page?mode=cookies')
    await asyncio.sleep(0.5)
    after = await tab2.get_cookies()
    state_restart = exit_state(profile)
    await shutdown(browser2, 'close_wait')

    restart_names = names(after)
    return {
        'label': label, 'profile': str(profile), 'prefs': prefs, 'extra_args': list(args),
        'self_check': self_check,
        'write_names': names(before),
        'session_flag_of_sess': next((c['session'] for c in norm(before) if c['name'] == sess), None),
        'exit': exit_info,
        'exit_state_before_close': state_before,
        'exit_state_after_close': state_after,
        'exit_state_after_restart': state_restart,
        'restart_names': restart_names,
        'session_survived': sess in restart_names,
        'persistent_survived': pers in restart_names,
    }


async def main() -> int:
    report: dict = {'template_profile': str(PROFILE), 'root': str(ROOT), 'cases': {}}
    shutil.rmtree(ROOT, ignore_errors=True)
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        print(f'[V-d v2] 本地桩：{url}（每 case 全新 profile：{ROOT}）')
        for label, prefs, args in CASES:
            case = await one_case(label, prefs, args, url)
            report['cases'][label] = case
            print(f'  [{label:<16}] exit_type={case["exit_state_after_close"].get("exit_type")} · '
                  f'会期Cookie {"存活" if case["session_survived"] else "掉了"} · '
                  f'持久Cookie {"存活" if case["persistent_survived"] else "掉了"} · '
                  f'重启后={case["restart_names"]}')

        base = report['cases']['baseline']
        report['findings'] = {
            'fresh_profile_baseline_session_dropped': not base['session_survived'],
            'fresh_profile_baseline_persistent_kept': base['persistent_survived'],
            'clean_exit_state': base['exit_state_after_close'],
            'levers_that_keep_session_cookie': [label for label, case in report['cases'].items()
                                                if case['session_survived']] or None,
        }
        print('\n=== 判定（`MB-Q6`）===')
        for key, value in report['findings'].items():
            print(f'  {key:<42} = {value}')
        path = save('m7b09_session_cookie.json', report)
        print(f'\n[V-d] 证据：{path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
