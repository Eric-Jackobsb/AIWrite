# -*- coding: utf-8 -*-
"""`M7B-09` / V-c —— **手动关窗语义 + 残留实例接管 / attach**（决议 `MB-D0-8` L3 的可行性闸门）。

三件事（全本地自证）：

  A. **用户手动关窗**（= Chrome 自己退出，例如点窗口 X）：
     外部发 `Browser.close`（Pydoll 全程"不知情"）→ 等进程退出 → 再调 `stop()`。
     预期：`stop()` 抛 `BrowserNotRunning`（**必须可吞**）；且登录态**保住**（干净退出）。
  B. **残留实例占 profile**：直接 `subprocess` 起 Chrome（固定调试端口）→
     a) 再用 Pydoll「同 profile + 同端口」启动一次：看会发生什么（接管？失败？附着？）
        —— 这是实现层必须识别的场景（**禁止为启动而强杀**）。
     b) `browser.connect(ws_address)` **attach 到既有实例**是否可行（L3 的正路）。
  C. 清理：优雅关掉直起的实例（不发强杀，避免顺带毁掉登录态）。

证据：`out/m7b09_selfheal_attach.json`。
"""
import asyncio
import json
import subprocess
import time
import urllib.request

from m7b09_common import (PROFILE, _wait_exit, assert_read_path, names, norm,
                          options, proc_of, save)
from pydoll.browser.chromium import Chrome

from local_server import LocalServer

PORT = 9333


def http_json(path: str, timeout: float = 2.0):
    with urllib.request.urlopen(f'http://127.0.0.1:{PORT}{path}', timeout=timeout) as resp:
        return json.loads(resp.read().decode('utf-8'))


def wait_devtools(timeout: float = 25.0) -> dict | None:
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        try:
            return http_json('/json/version')
        except Exception:  # noqa: BLE001
            time.sleep(0.4)
    return None


def find_chrome() -> str:
    """借用 Pydoll 的浏览器解析器（一次性探针可用私有方法；失败则回退到已知路径）。"""
    try:
        return Chrome(options=options(headless=True))._get_default_binary_location()  # noqa: SLF001
    except Exception:  # noqa: BLE001
        return r'C:\Program Files\Google\Chrome\Application\chrome.exe'


async def read_back(url: str, cookie: str) -> dict:
    """重启同 profile 读 Cookie（判定存活）。"""
    browser = Chrome(options=options(headless=True))
    tab = await browser.start()
    await tab.go_to(f'{url}/page?mode=cookies')
    await asyncio.sleep(0.4)
    cookies = await tab.get_cookies()
    try:
        await browser._execute_command({'method': 'Browser.close', 'params': {}})  # noqa: SLF001
    except Exception:  # noqa: BLE001
        pass
    await _wait_exit(proc_of(browser), 10)
    return {'names': names(cookies), 'survived': cookie in names(cookies)}


async def main() -> int:
    report: dict = {'profile': str(PROFILE), 'steps': {}}
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}'
        print(f'[V-c] 本地桩：{url}')

        # ---------------- A. 用户手动关窗语义 ----------------
        browser = Chrome(options=options(headless=False))
        tab = await browser.start()
        self_check = await assert_read_path(tab)
        await tab.go_to(f'{url}/setcookie?name=HEAL_ext&value=1&max_age=86400')
        await asyncio.sleep(0.4)
        close_err = None
        try:
            # 外部关窗：等价"用户点了 X" / Chrome 自行退出（Pydoll 不知情）
            await browser._execute_command({'method': 'Browser.close', 'params': {}})  # noqa: SLF001
        except Exception as exc:  # noqa: BLE001
            close_err = type(exc).__name__
        waited = await _wait_exit(proc_of(browser), 15)
        stop_exc = None
        try:
            await browser.stop()                     # 期望：抛 BrowserNotRunning（实现层必须吞）
        except Exception as exc:  # noqa: BLE001
            stop_exc = type(exc).__name__
        after = await read_back(url, 'HEAL_ext')
        report['steps']['manual_close'] = {
            'self_check': self_check, 'close_reply_error': close_err,
            'process_exit_waited_s': waited, 'stop_exception': stop_exc,
            'cookie_survived': after['survived'], 'restart_names': after['names'],
        }
        print(f'  A) 外部关窗：等退出 {waited}s · stop() 异常={stop_exc} · '
              f'Cookie 存活={"是" if after["survived"] else "否"}')

        # ---------------- B. 残留实例 + 固定端口 + attach ----------------
        chrome = find_chrome()
        proc = subprocess.Popen(
            [chrome, f'--user-data-dir={PROFILE}', f'--remote-debugging-port={PORT}',
             '--no-first-run', '--no-default-browser-check', 'about:blank'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        version = wait_devtools()
        residual = {'chrome': chrome, 'pid': proc.pid, 'devtools_version_ok': bool(version),
                    'browser': (version or {}).get('Browser'),
                    'ws_address': (version or {}).get('webSocketDebuggerUrl')}
        report['steps']['residual_instance'] = residual
        print(f'  B) 直起残留实例 pid={proc.pid} · DevTools 就绪={bool(version)} · '
              f'{residual["browser"]}')

        # B-a：Pydoll「同 profile + 同端口」再启一次（模拟"不知情"的第二次启动）
        start_result = None
        try:
            b2 = Chrome(options=options(headless=False), connection_port=PORT)
            tab2 = await b2.start()
            start_result = {'ok': True, 'opened_tabs': len(await b2.get_opened_tabs()),
                            'current_url': await tab2.current_url}
            print(f'    B-a) 第二次启动 **成功**（tab 数={start_result["opened_tabs"]}）'
                  f' ⇒ ⚠️ 疑似附着到既有实例')
        except Exception as exc:  # noqa: BLE001
            start_result = {'ok': False, 'error': f'{type(exc).__name__}: {exc}'}
            print(f'    B-a) 第二次启动 **失败**：{start_result["error"]}')
        report['steps']['second_launch_same_profile'] = start_result

        # B-b：attach 到既有实例（L3 正路）
        attach_result = None
        try:
            b3 = Chrome(options=options(headless=True))
            tab3 = await b3.connect(residual['ws_address'])
            cookies = await tab3.get_cookies()
            await tab3.go_to(f'{url}/eyes')
            await asyncio.sleep(0.3)
            attach_result = {'ok': True, 'cookie_names': names(cookies), 'cookies': norm(cookies)}
            print(f'    B-b) attach **成功**：既有实例 Cookie={names(cookies)}')
            try:                                     # 清理：用 attach 通道优雅关闭（不强杀）
                await b3._execute_command({'method': 'Browser.close', 'params': {}})  # noqa: SLF001
            except Exception:  # noqa: BLE001
                pass
        except Exception as exc:  # noqa: BLE001
            attach_result = {'ok': False, 'error': f'{type(exc).__name__}: {exc}'}
            print(f'    B-b) attach **失败**：{attach_result["error"]}')
        report['steps']['attach'] = attach_result

        # C. 兜底清理（超时未退 → 最后才强杀）
        exited = await _wait_exit(proc, 8)
        if proc.poll() is None:
            proc.kill()
        report['steps']['cleanup'] = {'residual_exit_waited_s': exited,
                                      'exit_code': proc.poll()}

        report['findings'] = {
            'manual_close_keeps_login': report['steps']['manual_close']['cookie_survived'],
            'stop_exception_on_manual_close': report['steps']['manual_close']['stop_exception'],
            'second_launch_same_profile': ('ok（疑似附着）' if start_result and start_result.get('ok')
                                           else 'failed'),
            'attach_works': bool(attach_result and attach_result.get('ok')),
        }
        print('\n=== 判定（L3 可行性）===')
        for key, value in report['findings'].items():
            print(f'  {key:<34} = {value}')
        path = save('m7b09_selfheal_attach.json', report)
        print(f'\n[V-c] 证据：{path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
