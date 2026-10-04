# -*- coding: utf-8 -*-
"""M7B-05 前置验证 —— 页面驱动：**humanize 真实打字** + **文件注入两条路径对比**。

验收（M7B.md §5）：站点附件区出现文件；输入框内容正确。
用本地 `/drive` 页自证（输入框 + `<input type=file>`），并把两条证据做成可读回：
  * 每次 `input` 事件的时间戳 → 证明**逐字符**输入（非一次性灌入）
  * `change` 事件把 `input.files` 的「名字:大小」写进 `#log` → 证明注入**真的生效**
"""
import asyncio
import json
import pathlib

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer

OUT = pathlib.Path(__file__).parent / 'out'
TEXT = '真实打字测试：中文 abc 123，标点。'


def cmd(method: str, **params) -> dict:
    return {'method': method, 'params': params}


def make_files() -> list[pathlib.Path]:
    folder = OUT / 'drive_files'
    folder.mkdir(parents=True, exist_ok=True)
    made = []
    for name, size in (('m7b-a.png', 120), ('m7b-b.txt', 64)):
        path = folder / name
        path.write_bytes((bytes(range(78)) * 4)[:size])
        made.append(path)
    return made


def js_text(result) -> str:
    payload = getattr(result, 'result', result)
    return payload.get('result', {}).get('value', '') if isinstance(payload, dict) else str(payload)


async def probe_typing(tab, loop) -> dict:
    """① humanize 真实打字 → 读回值 + 逐字符时间戳。

    关键：CDP 键盘事件发给**当前焦点元素** → 必须先聚焦（JS `focus()` 即可，无需真实点击）。
    """
    await tab.execute_script("document.getElementById('box').focus();")  # ★ 先聚焦
    started = loop.time()
    await tab.keyboard.type_text(TEXT)
    elapsed_ms = round((loop.time() - started) * 1000.0, 1)
    value = js_text(await tab.execute_script("return document.getElementById('box').value;"))
    stamps = js_text(await tab.execute_script('return JSON.stringify(window.__stamps || []);'))
    events = js_text(await tab.execute_script('return String(window.__inputs || 0);'))
    try:
        times = json.loads(stamps) if stamps else []
    except Exception:  # noqa: BLE001
        times = []
    gaps = [times[i + 1] - times[i] for i in range(len(times) - 1)]
    return {'elapsed_ms': elapsed_ms, 'value_ok': value == TEXT, 'value': value,
            'input_events': int(events or 0), 'char_count': len(TEXT),
            'interval_min_ms': min(gaps) if gaps else None,
            'interval_max_ms': max(gaps) if gaps else None,
            'per_char_ms': round(elapsed_ms / max(1, len(TEXT)), 1)}


def js_text(raw) -> str:
    """从 `execute_script` 的原始返回里取字符串值。

    ⚠️ 实测返回形状：`{'id': N, 'result': {'result': {'type': 'string', 'value': '...'}}}` —— 要解两层。
    """
    node = getattr(raw, 'result', raw)
    for _ in range(3):
        if isinstance(node, dict) and isinstance(node.get('result'), dict):
            node = node['result']
        else:
            break
    return str(node.get('value', '')) if isinstance(node, dict) else str(node)


async def probe_file_chooser(tab, files: list[pathlib.Path]) -> dict:
    """② 文件注入（a）：`expect_file_chooser` + 触发原生 chooser（计划优先方案）。"""
    before = js_text(await tab.execute_script(
        "return document.getElementById('log').textContent;"))
    try:
        async with tab.expect_file_chooser([str(p) for p in files]):
            await tab.execute_script("document.getElementById('file').click();",
                                     user_gesture=True)
            await asyncio.sleep(0.6)
        after = js_text(await tab.execute_script(
            "return document.getElementById('log').textContent;"))
    except Exception as exc:  # noqa: BLE001
        after = f'ERR: {type(exc).__name__}: {exc}'
    return {'before': before, 'after': after,
            'expected': [f'{p.name}:{p.stat().st_size}' for p in files]}


async def probe_dom_set_files(tab, files: list[pathlib.Path]) -> dict:
    """③ 文件注入（b）：`DOM.setFileInputFiles`（不依赖 chooser；headless 更稳）。

    证据取法：直接读 `input.files`（**不依赖 change 事件** —— 实测 setFileInputFiles 成功但
    change 事件未必触发；读 `files` 才是"附件区真的有文件"的硬证据）。
    """
    await tab.execute_script("document.getElementById('file').value = '';")
    document = await tab._execute_command(cmd('DOM.getDocument'))  # noqa: SLF001
    root_id = document.get('result', {}).get('root', {}).get('nodeId')
    node = await tab._execute_command(  # noqa: SLF001
        cmd('DOM.querySelector', nodeId=root_id, selector='#file'))
    target = node.get('result', {}).get('nodeId')
    response = await tab._execute_command(  # noqa: SLF001
        cmd('DOM.setFileInputFiles', files=[str(p) for p in files], nodeId=target))
    await asyncio.sleep(0.4)
    files_text = js_text(await tab.execute_script(
        "return Array.from(document.getElementById('file').files)"
        ".map(f => f.name + ':' + f.size).join('|');"))
    log = js_text(await tab.execute_script(
        "return document.getElementById('log').textContent;"))
    return {'node_id': target, 'response': response.get('result', response),
            'files': files_text, 'change_event_log': log}


async def main() -> int:
    files = make_files()
    OUT.mkdir(exist_ok=True)
    opts = ChromiumOptions()
    opts.headless = True
    report: dict = {'text': TEXT, 'files': [str(p) for p in files]}

    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}/page?mode=drive'
        async with Chrome(options=opts) as browser:
            tab = await browser.start()
            await tab.go_to(url)
            loop = asyncio.get_running_loop()
            report['typing'] = await probe_typing(tab, loop)
            report['file_chooser'] = await probe_file_chooser(tab, files)
            report['dom_set_files'] = await probe_dom_set_files(tab, files)

    typing = report['typing']
    verdict = {
        'typing_text_correct': typing['value_ok'],
        'typing_is_per_char': typing['input_events'] >= max(1, len(TEXT) // 2),
        'file_chooser_injected': 'm7b-a.png' in str(report['file_chooser']['after']),
        'dom_set_files_injected': 'm7b-a.png' in str(report['dom_set_files']['files']),
        'dom_set_files_second_file_ok': 'm7b-b.txt' in str(report['dom_set_files']['files']),
        'dom_set_files_fires_change': 'm7b-a.png' in str(report['dom_set_files']['change_event_log']),
    }
    report['verdict'] = verdict
    (OUT / 'm7b05_page_drive.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    print(f'[M7B-05] 打字：{typing["char_count"]} 字符 / {typing["elapsed_ms"]} ms '
          f'（{typing["per_char_ms"]} ms per char），input 事件 {typing["input_events"]} 次，'
          f'间隔 {typing["interval_min_ms"]}~{typing["interval_max_ms"]} ms')
    print(f'[M7B-05] 值一致 = {typing["value_ok"]}｜读回：{typing["value"]}')
    print(f'[M7B-05] 期望注入：{report["file_chooser"]["expected"]}')
    print(f'[M7B-05] 路径(a) 文件选择器 → {report["file_chooser"]["after"]}')
    print(f'[M7B-05] 路径(b) DOM.setFileInputFiles → files=[{report["dom_set_files"]["files"]}] '
          f'change 事件日志=[{report["dom_set_files"]["change_event_log"]}]')
    print('=== 判定 ===')
    for key, value in verdict.items():
        print(f'  {key:32} {"PASS" if value else "FAIL"}')
    print(f'[M7B-05] 证据：{OUT / "m7b05_page_drive.json"}')
    return 0 if all(verdict.values()) else 1


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
