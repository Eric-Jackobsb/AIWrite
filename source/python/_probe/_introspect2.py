# -*- coding: utf-8 -*-
"""M7B 前置验证 —— 第二轮 API 探测：命令名 / 事件名 / IO.read 的确切写法。"""
import inspect

from pydoll.protocol.base import Command
from pydoll.protocol.fetch.methods import FetchCommands
from pydoll.protocol.network.methods import NetworkCommands

print('Command 签名 :', inspect.signature(Command.__init__))
print()
print('FetchCommands:', [x for x in dir(FetchCommands) if not x.startswith('_')])
print()
print('NetworkCommands 关键项:',
      [x for x in dir(NetworkCommands) if not x.startswith('_') and
       any(k in x.lower() for k in ('enable', 'data_received', 'event_source', 'response_body',
                                    'get_response', 'load', 'loading'))])
print()
try:
    from pydoll.protocol.network.events import NetworkEvents

    print('NetworkEvents:', [x for x in dir(NetworkEvents) if not x.startswith('_')])
except Exception as exc:  # noqa: BLE001
    print('NetworkEvents 导入失败：', exc)
try:
    from pydoll.protocol.fetch.events import FetchEvents

    print('FetchEvents  :', [x for x in dir(FetchEvents) if not x.startswith('_')])
except Exception as exc:  # noqa: BLE001
    print('FetchEvents 导入失败：', exc)
