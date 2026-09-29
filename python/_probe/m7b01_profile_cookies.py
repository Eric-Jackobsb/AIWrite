# -*- coding: utf-8 -*-
"""M7B-01 前置验证 —— **headful 起浏览器 + 独立 profile + 经 CDP 读 Cookie（含 HttpOnly）**。

验收（M7B.md §5）：窗口可见 · profile 落 `~/.brain-ai/pydoll-profile` · CDP 读回 Cookie 名单（脱敏）
**含 HttpOnly**（= 与 `document.cookie` 的关键差异）· 同 profile 重启后**持久 Cookie 仍在**。

⚠️ 本脚本会**弹出可见浏览器窗口**（headful 是要求，不是缺陷），每次约 8 秒后自动关闭。
真·站点登录态不在本项范围（`M7B-06` 需要人工登录一次）。
"""
import asyncio
import json
import pathlib

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer

PROFILE = pathlib.Path.home() / '.brain-ai' / 'pydoll-profile'
OUT = pathlib.Path(__file__).parent / 'out'
WANT = ('m7b_http', 'm7b_plain', 'm7b_js_session', 'm7b_js')


def options() -> ChromiumOptions:
    opts = ChromiumOptions()
    opts.headless = False                                # M7B-01：**必须 headful**
    # ⚠️ Pydoll 默认已加 `--no-first-run` / `--no-default-browser-check`（再加会抛
    #    ArgumentAlreadyExistsInOptions —— 实测踩到）；`--user-data-dir` 需我们自己加。
    opts.add_argument(f'--user-data-dir={PROFILE}')      # 单 profile（计划指定路径）
    opts.start_timeout = 60                              # 新 profile 首次启动较慢
    return opts


def redact(cookies: list[dict]) -> list[dict]:
    """脱敏：只留名字 / 长度 / 标志位（不落盘明文值）。"""
    out = []
    for cookie in cookies:
        name = str(cookie.get('name', ''))
        if name not in WANT:
            continue
        out.append({'name': name, 'value_len': len(str(cookie.get('value', ''))),
                    'httpOnly': bool(cookie.get('httpOnly')),
                    'secure': bool(cookie.get('secure')),
                    'session': bool(cookie.get('session')),
                    'domain': cookie.get('domain'), 'path': cookie.get('path')})
    return sorted(out, key=lambda item: item['name'])


def js_cookie_names(text: str) -> list[str]:
    return sorted(part.split('=')[0].strip() for part in str(text).split(';') if '=' in part)


def js_text(raw) -> str:
    """从 `execute_script` 原始返回取字符串（返回形状为两层 `result`）。"""
    node = getattr(raw, 'result', raw)
    for _ in range(3):
        if isinstance(node, dict) and isinstance(node.get('result'), dict):
            node = node['result']
        else:
            break
    return str(node.get('value', '')) if isinstance(node, dict) else str(node)


async def run_pass(label: str, base: str, mode: str) -> dict:
    """mode = 'write'（写登录态）｜ 'read'（重启后读，需先导航到同源页面才有页面上下文）。

    ⚠️ 关键：**必须优雅关闭**（CDP `Browser.close`）—— Pydoll 默认的 `stop()` 是强杀，
    Cookie 库不落盘，重启后登录态丢失（`_diag_persist.py` 实测：强杀=False / 优雅=True）。
    """
    try:
        browser = Chrome(options=options())
        try:
            tab = await browser.start()
            await asyncio.sleep(1.5)  # 让 headful 首次运行稳定（避免初始 target 被换掉）
            if mode == 'write':
                tab = await browser.new_tab(f'{base}/setcookies')   # ★ new_tab 拿新目标更稳
                await asyncio.sleep(0.4)
            # 同源页面（'read' 也要，否则没有页面上下文 → 读库返回空）
            await tab.go_to(f'{base}/page?mode=cookies')
            await asyncio.sleep(0.6)
            cookies = await tab.get_cookies()
            doc = await tab.execute_script('return document.cookie;')
            doc_text = js_text(doc)
            result = {'label': label, 'error': '', 'mode': mode,
                      'cdp': redact(cookies),
                      'cdp_count': len(cookies),
                      'document_cookie_names': js_cookie_names(doc_text),
                      'document_cookie_raw_len': len(doc_text)}
            # ---- 优雅关闭（关键）----
            try:
                await browser._execute_command(  # noqa: SLF001
                    {'method': 'Browser.close', 'params': {}})
            except Exception:  # noqa: BLE001 —— 关窗瞬间连接断开属正常
                pass
            await asyncio.sleep(2.5)
        finally:
            try:
                await browser.stop()
            except Exception:  # noqa: BLE001
                pass
        return result
    except Exception as exc:  # noqa: BLE001 —— 前置验证要把失败模式也留证
        return {'label': label, 'error': f'{type(exc).__name__}: {exc}', 'mode': mode,
                'cdp': [], 'cdp_count': 0, 'document_cookie_names': [],
                'document_cookie_raw_len': 0}

async def main() -> int:
    OUT.mkdir(exist_ok=True)
    report: dict = {'profile': str(PROFILE)}
    with LocalServer() as server:
        base = f'http://127.0.0.1:{server.port}'

        # ---- 第 1 轮：新建登录态 ----
        report['pass1'] = await run_pass('pass1-写入', base, mode='write')
        # ---- profile 落盘检查 ----
        files = [p for p in PROFILE.rglob('*') if p.is_file()] if PROFILE.exists() else []
        cookies_db = [p for p in files if p.name.lower() in ('cookies', 'cookies-journal')]
        report['profile'] = {'path': str(PROFILE), 'exists': PROFILE.exists(),
                             'file_count': len(files),
                             'cookie_db': [str(p.name) for p in cookies_db]}
        # ---- 第 2 轮：同 profile 重启（不导航，直接读库）----
        report['pass2'] = await run_pass('pass2-重启', base, mode='read')

    p1 = {c['name']: c for c in report['pass1']['cdp']}
    p2 = {c['name']: c for c in report['pass2']['cdp']}
    http_only_seen = bool(p1.get('m7b_http', {}).get('httpOnly'))
    js_missing_http = 'm7b_http' not in report['pass1']['document_cookie_names']
    persisted = 'm7b_plain' in p2 and 'm7b_http' in p2
    session_gone = 'm7b_js_session' not in p2
    report['verdict'] = {
        'cdp_sees_httponly': http_only_seen,
        'document_cookie_hides_httponly': js_missing_http,
        'persistent_survives_restart': persisted,
        'session_cookie_dropped_on_restart': session_gone,
    }
    (OUT / 'm7b01_profile_cookies.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    print(f'[M7B-01] profile = {PROFILE}（存在={report["profile"]["exists"]} '
          f'文件数={report["profile"]["file_count"]} Cookie库={report["profile"]["cookie_db"]}）')
    for label in ('pass1', 'pass2'):
        item = report[label]
        print(f'[M7B-01] {label} 错误={item["error"] or "（无）"}｜CDP 读回 {item["cdp_count"]} 条｜'
              f'本条相关={[c["name"] for c in item["cdp"]]}')
    print(f'[M7B-01] 第1轮 document.cookie 可见：{report["pass1"]["document_cookie_names"]}')
    print(f'[M7B-01] 第2轮（重启同 profile）CDP 读回：'
          f'{[c["name"] for c in report["pass2"]["cdp"]]}')
    print('=== 判定 ===')
    for key, value in report['verdict'].items():
        print(f'  {key:38} {"PASS" if value else "FAIL"}')
    print(f'[M7B-01] 证据：{OUT / "m7b01_profile_cookies.json"}')
    return 0 if all(report['verdict'].values()) else 1


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
