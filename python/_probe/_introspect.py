# -*- coding: utf-8 -*-
"""M7B 前置验证 —— Pydoll 2.27 API 面探测（一次性脚本；查清 CDP 命令 / 事件注册的确切写法）。

为什么先做这一步：M7B-03 要实测三条 CDP 增量路线，必须知道
  ① 如何发原始 CDP 命令（Fetch.takeResponseBodyAsStream / IO.read）
  ② 如何订阅 Network 事件（eventSourceMessageReceived / dataReceived）
"""
import inspect
import pkgutil

import pydoll.protocol.fetch as fet
import pydoll.protocol.network as net


def dump(mod, label):
    print('=' * 10, label)
    print('file    :', mod.__file__)
    print('members :', [x for x in dir(mod) if not x.startswith('_')])
    if hasattr(mod, '__path__'):
        print('submods :', [m.name for m in pkgutil.iter_modules(mod.__path__)])


dump(net, 'protocol.network')
dump(fet, 'protocol.fetch')

try:
    import pydoll.protocol.io as io_mod

    dump(io_mod, 'protocol.io')
except Exception as exc:  # noqa: BLE001
    print('protocol.io 不可直接导入：', exc)

import pydoll.browser.tab as tab_mod  # noqa: E402

print('=' * 10, 'Tab 关键方法签名')
for name in ('on', 'remove_callback', '_execute_command', 'enable_network_events',
             'enable_fetch_events', 'get_network_logs', 'get_network_response_body'):
    func = getattr(tab_mod.Tab, name, None)
    print(f'{name:28}', inspect.signature(func) if func is not None else '(缺失)')

print('=' * 10, 'Tab 模块内与命令/事件相关的类')
print([n for n in dir(tab_mod) if n.endswith('Commands') or n.endswith('Events')])
