# -*- coding: utf-8 -*-
"""M7B 前置验证 —— 第三轮 API 探测：键盘 / 文件选择器 / Cookie / Storage 的确切写法。"""
import inspect

from pydoll.browser.tab import Tab
from pydoll.commands.storage_commands import StorageCommands

print('StorageCommands:', [x for x in dir(StorageCommands) if not x.startswith('_')][:30])
try:
    from pydoll.commands.network_commands import NetworkCommands

    print('NetworkCommands(cookie 相关):',
          [x for x in dir(NetworkCommands) if 'ookie' in x or 'COOKIE' in x])
except Exception as exc:  # noqa: BLE001
    print('NetworkCommands 导入失败：', exc)

for name in ('get_cookies', 'set_cookies', 'delete_all_cookies', 'expect_file_chooser',
             'enable_intercept_file_chooser_dialog', 'disable_intercept_file_chooser_dialog',
             'keyboard', 'mouse', 'scroll'):
    member = getattr(Tab, name, None)
    print(f'{name:38}', inspect.signature(member) if callable(member) else type(member).__name__)

try:
    import pydoll.browser.page as _page  # noqa: F401
except Exception:  # noqa: BLE001
    pass
try:
    from pydoll.browser.tab import KeyboardAPI

    print('KeyboardAPI:', [x for x in dir(KeyboardAPI) if not x.startswith('_')])
except Exception as exc:  # noqa: BLE001
    print('KeyboardAPI 导入失败：', exc)
try:
    from pydoll.browser.tab import FileChooser  # 可能不存在
    print('FileChooser:', FileChooser)
except Exception:  # noqa: BLE001
    import pydoll.browser.tab as t

    print('tab 里与文件/键盘相关的类:',
          [n for n in dir(t) if any(k in n.lower() for k in ('file', 'keyboard', 'mouse', 'scroll'))])
