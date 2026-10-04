# -*- coding: utf-8 -*-
"""`M7B-09` / V-a —— **关闭时序三档对照 + 提交时机**（决议 `MB-D0-8` L1 的闸门）。

回答两个问题（都不依赖任何真实站点，全部在本地页面自证）：

  A. 三种退出方式，重启后**持久 Cookie 还在吗**？
     ① `kill_raw`        —— 直接 TerminateProcess（等价任务管理器 / 断电）
     ② `pydoll_stop`     —— Pydoll 默认 `stop()`：发 `Browser.close` 后**立刻** kill
     ③ `close_wait`      —— **计划要求的 L1**：`Browser.close` → **等进程真正退出**

  B. Chrome 是**延迟批量提交** Cookie 库，还是**只在干净退出时提交**？
     写一条持久 Cookie → 等 W ∈ {0,10,30,60} s → **强杀** → 重启读。
     若大 W 仍丢 ⇒ 崩溃窗口 = 无限（L2 加密快照是唯一兜底）；
     若某 W 起能保住 ⇒ 崩溃窗口有限，可量化（并据此定"快照间隔"）。

证据：`out/m7b09_shutdown_timing.json`（含 journal 物证与三档耗时数字）。
"""
import asyncio

from m7b09_common import (PROFILE, assert_read_path, names, norm, options,
                          profile_artifacts, save, shutdown)
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

MODES = ('kill_raw', 'pydoll_stop', 'close_wait')
WAITS = (0, 10, 30, 60)


async def write_cookie(browser, url: str, cookie_name: str) -> dict:
    """起浏览器写一条持久 Cookie（名字唯一，避免跨 case 污染判定）。"""
    tab = await browser.start()
    self_check = await assert_read_path(tab)
    await tab.go_to(f'{url}/setcookie?name={cookie_name}&value=1&max_age=86400')
    await asyncio.sleep(0.4)
    cookies = await tab.get_cookies()
    return {'self_check': self_check, 'written': names(cookies), 'cookies': norm(cookies)}


async def read_back(url: str, label: str) -> dict:
    """重启同 profile → 读 Cookie（判定用）。"""
    browser = Chrome(options=options(headless=True))
    tab = await browser.start()
    self_check = await assert_read_path(tab)
    tab = await browser.new_tab(f'{url}/page?mode=cookies')
    await asyncio.sleep(0.3)
    cookies = await tab.get_cookies()
    info = await shutdown(browser, 'close_wait')
    return {'label': label, 'self_check': self_check,
            'names': names(cookies), 'cookies': norm(cookies), 'shutdown': info}


async def main() -> int:
    report: dict = {'profile': str(PROFILE), 'cases': {}, 'timing': {}, 'findings': {}}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        print(f'[V-a] 本地桩：{url}')

        # ---------------- A. 三档关闭 ----------------
        for mode in MODES:
            cookie = f'S_{mode}'
            browser = Chrome(options=options(headless=False))   # 关窗语义用 headful 更贴近生产
            try:
                write = await write_cookie(browser, url, cookie)
                exit_info = await shutdown(browser, mode)
            finally:
                pass
            after = await read_back(url, f'after_{mode}')
            survived = cookie in after['names']
            report['cases'][mode] = {
                'cookie': cookie, 'write': write, 'shutdown': exit_info,
                'restart_names': after['names'], 'restart_cookies': after['cookies'],
                'survived': survived,
                'artifacts_after_exit': profile_artifacts(),
            }
            print(f'  [{mode}] 写入={cookie} · 关闭={exit_info} · 重启后={after["names"]} '
                  f'⇒ 存活={"是" if survived else "否"}')

        # ---------------- B. 提交时机（延迟提交 vs 干净退出提交） ----------------
        for wait in WAITS:
            cookie = f'W{wait}'
            browser = Chrome(options=options(headless=True))
            write = await write_cookie(browser, url, cookie)
            await asyncio.sleep(wait)                      # 写完之后等 W 秒再强杀
            exit_info = await shutdown(browser, 'kill_raw')
            artifacts = profile_artifacts()
            after = await read_back(url, f'after_wait_{wait}')
            survived = cookie in after['names']
            report['timing'][f'{wait}s'] = {
                'cookie': cookie, 'write': write, 'shutdown': exit_info,
                'artifacts_after_kill': artifacts,
                'restart_names': after['names'], 'survived': survived,
            }
            print(f'  [等 {wait:>2}s 后强杀] 写入={cookie} · journal={artifacts["cookies_journal"]} · '
                  f'重启后={after["names"]} ⇒ 存活={"是" if survived else "否"}')

        # ---------------- 判定 ----------------
        cases = report['cases']
        report['findings'] = {
            'close_wait_keeps_persistent': cases['close_wait']['survived'],
            'pydoll_stop_drops_persistent': not cases['pydoll_stop']['survived'],
            'kill_raw_drops_persistent': not cases['kill_raw']['survived'],
            'delayed_commit_window_s': [int(k[:-1]) for k, v in report['timing'].items()
                                        if v['survived']] or None,
            'close_wait_elapsed_s': cases['close_wait']['shutdown'].get('waited_s'),
        }
        print('\n=== 判定 ===')
        for key, value in report['findings'].items():
            print(f'  {key:<34} = {value}')
        print('  （delayed_commit_window_s = 能"侥幸保住"的最短等待秒数；None = 强杀必丢）')
        path = save('m7b09_shutdown_timing.json', report)
        print(f'\n[V-a] 证据：{path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
