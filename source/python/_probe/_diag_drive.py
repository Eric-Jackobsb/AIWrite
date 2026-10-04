# -*- coding: utf-8 -*-
"""M7B-05 诊断：打字与文件注入为何"看似执行了但页面没变"（焦点？上下文？节点？）。"""
import asyncio
import pathlib

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer

OUT = pathlib.Path(__file__).parent / 'out'


def cmd(method: str, **params) -> dict:
    return {'method': method, 'params': params}


def js(result) -> str:
    payload = getattr(result, 'result', result)
    return str(payload.get('result', {}).get('value', '')) if isinstance(payload, dict) else str(payload)


async def main() -> int:
    target = OUT / 'drive_files' / 'm7b-a.png'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(b'x' * 120)
    opts = ChromiumOptions()
    opts.headless = True
    with LocalServer() as server:
        url = f'http://127.0.0.1:{server.port}/page?mode=drive'
        async with Chrome(options=opts) as browser:
            tab = await browser.start()
            await tab.go_to(url)
            await asyncio.sleep(0.5)
            print('URL          :', getattr(tab, 'current_url', '?'))
            print('readyState   :', js(await tab.execute_script('return document.readyState;')))
            print('inputs 数量  :', js(await tab.execute_script(
                "return String(document.querySelectorAll('input,textarea').length);")))
            print('activeElement:', js(await tab.execute_script(
                'return String(document.activeElement && document.activeElement.id);')))
            await tab.execute_script("document.getElementById('box').focus();")
            print('focus 后     :', js(await tab.execute_script(
                'return String(document.activeElement && document.activeElement.id);')))
            await tab.keyboard.type_text('AB')
            print('打字后 value :', repr(js(await tab.execute_script(
                "return document.getElementById('box').value;"))))
            print('input 事件数 :', js(await tab.execute_script('return String(window.__inputs||0);')))
            print('body 文本头  :', repr(js(await tab.execute_script(
                'return document.body.innerText.slice(0,60);'))))

            document = await tab._execute_command(cmd('DOM.getDocument', depth=-1))  # noqa: SLF001
            root = document.get('result', {}).get('root', {}).get('nodeId')
            node = await tab._execute_command(  # noqa: SLF001
                cmd('DOM.querySelector', nodeId=root, selector='#file'))
            node_id = node.get('result', {}).get('nodeId')
            response = await tab._execute_command(  # noqa: SLF001
                cmd('DOM.setFileInputFiles', files=[str(target)], nodeId=node_id))
            await asyncio.sleep(0.4)
            print('DOM nodeId   :', node_id, '｜set 返回:', response.get('result', response))
            print('files 读回   :', repr(js(await tab.execute_script(
                "return Array.from(document.getElementById('file').files).map(f=>f.name).join(',');"))))
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
