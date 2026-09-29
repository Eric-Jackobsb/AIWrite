# -*- coding: utf-8 -*-
"""M7B-03 前置验证 —— **CDP 增量流式三路线实测**（一次性脚本；不进产品）。

问题（`R2` / `MB-Q1`）：DeepSeek 网页版是「逐字流式」，撤掉协议栈后必须能用 CDP 拿到**增量**。
本脚本用**本地页面**把这个问题与「站点是否登录 / 是否改版」解耦：

  路线 ① `Fetch.takeResponseBodyAsStream` + `IO.read`（拦响应阶段 → 边到边读）
  路线 ② `Network.eventSourceMessageReceived`（站点用 EventSource 时才有）
  路线 ③ `Network.dataReceived` + `Network.getResponseBody`（只能看进度 / 末尾才拿到全文）

判定「增量」的标准：**首帧明显早于末帧**且**帧数 > 1**（否则等于一次性拿到全部）。
结论写入 `out/m7b03_cdp_stream.json`，供回填 M7B §14 `MB-Q1`。
"""
import asyncio
import base64
import json
import os
import pathlib
import time

from pydoll.browser.chromium import Chrome
from pydoll.browser.options import ChromiumOptions

from local_server import LocalServer  # 同目录一次性测试服务器

COUNT = 8      # 流内块数
GAP = 0.35     # 每块间隔（秒）→ 总时长 ≈ 2.8s，足以区分「增量」与「一次性」
OUT_DIR = pathlib.Path(__file__).parent / 'out'


def js_text(raw) -> str:
    """从 `execute_script` 的原始返回取字符串值。

    ⚠️ 实测返回形状：`{'id': N, 'result': {'result': {'type': 'string', 'value': '...'}}}` —— 要解两层。
    """
    node = getattr(raw, 'result', raw)
    for _ in range(3):
        if isinstance(node, dict) and isinstance(node.get('result'), dict):
            node = node['result']
        else:
            break
    return str(node.get('value', '')) if isinstance(node, dict) else str(node)


def cmd(method: str, **params) -> dict:
    """手写 CDP 命令（`Command` 是 TypedDict；`pydoll.commands.*` 里也有同名类型化 helper）。"""
    return {'method': method, 'params': params}


def headless_options() -> ChromiumOptions:
    opts = ChromiumOptions()
    opts.headless = True  # 本项只验 CDP 机制；headful 由 M7B-01 单独验
    return opts


def _frame(t0: float, size: int, extra: dict | None = None) -> dict:
    frame = {'ms': round((time.perf_counter() - t0) * 1000.0, 1), 'bytes': size}
    if extra:
        frame.update(extra)
    return frame


async def probe_fetch_stream(base_url: str) -> dict:
    """路线 ①：Fetch 拦响应 + takeResponseBodyAsStream + IO.read 轮读。"""
    frames: list[dict] = []
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()

        async def on_paused(event: dict) -> None:
            request_id = event.get('params', {}).get('requestId')
            if not request_id:
                return
            stream_resp = await tab._execute_command(  # noqa: SLF001（前置验证：探明 API 可用性）
                cmd('Fetch.takeResponseBodyAsStream', requestId=request_id))
            handle = stream_resp.get('result', {}).get('stream')
            if not handle:
                return
            total = 0
            while True:
                read_resp = await tab._execute_command(  # noqa: SLF001
                    cmd('IO.read', handle=handle, size=65536))
                result = read_resp.get('result', {})
                raw = result.get('data', '')
                data = base64.b64decode(raw) if result.get('base64Encoded') else raw.encode('utf-8')
                total += len(data)
                frames.append(_frame(t0, len(data), {'cum': total, 'eof': bool(result.get('eof'))}))
                if result.get('eof') or not data:
                    break
            try:
                await tab._execute_command(  # noqa: SLF001
                    cmd('Fetch.continueResponse', requestId=request_id))
            except Exception as exc:  # noqa: BLE001
                frames.append(_frame(t0, 0, {'note': f'continueResponse: {exc}'}))

        await tab.on('Fetch.requestPaused', on_paused)
        await tab._execute_command(  # noqa: SLF001
            cmd('Fetch.enable', patterns=[{'urlPattern': '*stream*', 'requestStage': 'Response'}]))
        await tab.go_to(f'{base_url}/page?mode=fetch')
        await asyncio.sleep(GAP * COUNT + 3.0)
    return {'frames': frames}


async def probe_network_data_received(base_url: str) -> dict:
    """路线 ③：Network.dataReceived 看进度 + 结束后 getResponseBody 拿全文。"""
    events: list[dict] = []
    state: dict = {'request_id': None}
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()

        async def on_data(event: dict) -> None:
            params = event.get('params', {})
            events.append(_frame(t0, int(params.get('encodedDataLength') or 0),
                                 {'dataLength': params.get('dataLength')}))

        async def on_finished(event: dict) -> None:
            params = event.get('params', {})
            if state['request_id'] is None:
                state['request_id'] = params.get('requestId')

        await tab.on('Network.dataReceived', on_data)
        await tab.on('Network.loadingFinished', on_finished)
        await tab.enable_network_events()
        await tab.go_to(f'{base_url}/page?mode=fetch')
        await asyncio.sleep(GAP * COUNT + 3.0)

        body_text = None
        if state['request_id']:
            try:
                body_text = await tab.get_network_response_body(state['request_id'])
            except Exception as exc:  # noqa: BLE001
                body_text = f'ERR: {exc}'
    return {'events': events, 'final_body_len': len(body_text or ''),
            'final_body_head': (body_text or '')[:40]}


async def probe_sse(base_url: str) -> dict:
    """路线 ②：Network.eventSourceMessageReceived（仅站点用 EventSource 时有值）。"""
    messages: list[dict] = []
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()

        async def on_message(event: dict) -> None:
            params = event.get('params', {})
            data = params.get('data', '')
            messages.append(_frame(t0, len(data), {'data': data[:24]}))

        await tab.on('Network.eventSourceMessageReceived', on_message)
        await tab.enable_network_events()
        await tab.go_to(f'{base_url}/page?mode=sse')
        await asyncio.sleep(GAP * COUNT + 3.0)
    return {'messages': messages}


async def probe_fetch_stream_big(base_url: str, chunks: int = 24, size_kb: int = 64) -> dict:
    """路线① 加严版：**大而慢**的流（1.5 MB / 12s），并记录 pause 时刻。

    为什么要加严：第一版只有 512 字节，被浏览器一次性缓冲 → 无法区分「本来就不增量」与「太小看不出」。
    """
    frames: list[dict] = []
    state: dict = {'paused_ms': None}
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()

        async def on_paused(event: dict) -> None:
            request_id = event.get('params', {}).get('requestId')
            state['paused_ms'] = round((time.perf_counter() - t0) * 1000.0, 1)
            if not request_id:
                return
            stream_resp = await tab._execute_command(  # noqa: SLF001
                cmd('Fetch.takeResponseBodyAsStream', requestId=request_id))
            handle = stream_resp.get('result', {}).get('stream')
            if not handle:
                return
            total = 0
            while True:
                read_resp = await tab._execute_command(  # noqa: SLF001
                    cmd('IO.read', handle=handle, size=65536))
                result = read_resp.get('result', {})
                raw = result.get('data', '')
                data = base64.b64decode(raw) if result.get('base64Encoded') else raw.encode('utf-8')
                total += len(data)
                frames.append(_frame(t0, len(data), {'cum': total, 'eof': bool(result.get('eof'))}))
                if result.get('eof') or not data:
                    break

        await tab.on('Fetch.requestPaused', on_paused)
        await tab._execute_command(  # noqa: SLF001
            cmd('Fetch.enable', patterns=[{'urlPattern': '*stream*', 'requestStage': 'Response'}]))
        await tab.go_to(f'{base_url}/page?mode=fetch&n={chunks}&size={size_kb * 1024}&gap=0.5')
        await asyncio.sleep(chunks * 0.5 + 6.0)
    return {'paused_ms': state['paused_ms'], 'frames': frames}


async def probe_fetch_stream_tiny_slow(base_url: str, chunks: int = 60, size: int = 16,
                                       gap: float = 0.3) -> dict:
    """路线① 判决性变体：**小体量 + 长时长**（960 B / 18 s）—— 真实聊天回答正是这个形态。

    背景：小负载（512B/2.8s）时 pause 落在**流结束之后**（拿不到增量），大负载（1.5MB/12s）时 pause 很早。
    真实回答通常「几 KB 但流 10 秒以上」→ 到底是哪一种，直接决定主路线，必须实测。
    """
    frames: list[dict] = []
    state: dict = {'paused_ms': None}
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()

        async def on_paused(event: dict) -> None:
            request_id = event.get('params', {}).get('requestId')
            state['paused_ms'] = round((time.perf_counter() - t0) * 1000.0, 1)
            if not request_id:
                return
            stream_resp = await tab._execute_command(  # noqa: SLF001
                cmd('Fetch.takeResponseBodyAsStream', requestId=request_id))
            handle = stream_resp.get('result', {}).get('stream')
            if not handle:
                return
            total = 0
            while True:
                read_resp = await tab._execute_command(  # noqa: SLF001
                    cmd('IO.read', handle=handle, size=65536))
                result = read_resp.get('result', {})
                raw = result.get('data', '')
                data = base64.b64decode(raw) if result.get('base64Encoded') else raw.encode('utf-8')
                total += len(data)
                frames.append(_frame(t0, len(data), {'cum': total, 'eof': bool(result.get('eof'))}))
                if result.get('eof') or not data:
                    break

        await tab.on('Fetch.requestPaused', on_paused)
        await tab._execute_command(  # noqa: SLF001
            cmd('Fetch.enable', patterns=[{'urlPattern': '*stream*', 'requestStage': 'Response'}]))
        await tab.go_to(f'{base_url}/page?mode=fetch&n={chunks}&size={size}&gap={gap}')
        await asyncio.sleep(chunks * gap + 6.0)
    return {'paused_ms': state['paused_ms'], 'frames': frames,
            'stream_total_s': round(chunks * gap, 1)}


async def probe_fetch_read_granularity(base_url: str, chunks: int = 100, chunk_size: int = 100,
                                       gap: float = 0.3, read_size: int = 4096) -> dict:
    """路线① 粒度判决：**到达块 100B / read size 4096B**。

    要回答的问题：`IO.read` 是「有多少给多少」（真增量、可逐字）还是「攒够 size 才给」？
      * 若每读 ~100B（= 到达粒度）→ 自然增量 → **逐字可行**（read size 取 1KB 上下即可）
      * 若每读 4096B（= 请求量）→ 攒批 → 逐字需要极小 read size（CDP 往返开销上升）
    """
    frames: list[dict] = []
    state: dict = {'paused_ms': None}
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()

        async def on_paused(event: dict) -> None:
            request_id = event.get('params', {}).get('requestId')
            state['paused_ms'] = round((time.perf_counter() - t0) * 1000.0, 1)
            if not request_id:
                return
            stream_resp = await tab._execute_command(  # noqa: SLF001
                cmd('Fetch.takeResponseBodyAsStream', requestId=request_id))
            handle = stream_resp.get('result', {}).get('stream')
            if not handle:
                return
            total = 0
            while True:
                read_resp = await tab._execute_command(  # noqa: SLF001
                    cmd('IO.read', handle=handle, size=read_size))
                result = read_resp.get('result', {})
                raw = result.get('data', '')
                data = base64.b64decode(raw) if result.get('base64Encoded') else raw.encode('utf-8')
                total += len(data)
                frames.append(_frame(t0, len(data), {'cum': total, 'eof': bool(result.get('eof'))}))
                if result.get('eof') or not data:
                    break

        await tab.on('Fetch.requestPaused', on_paused)
        await tab._execute_command(  # noqa: SLF001
            cmd('Fetch.enable', patterns=[{'urlPattern': '*stream*', 'requestStage': 'Response'}]))
        await tab.go_to(f'{base_url}/page?mode=fetch&n={chunks}&size={chunk_size}&gap={gap}')
        await asyncio.sleep(chunks * gap + 6.0)
    sizes = sorted({f['bytes'] for f in frames if f['bytes'] > 0})
    return {'paused_ms': state['paused_ms'], 'read_size': read_size, 'chunk_size': chunk_size,
            'frames': frames, 'distinct_sizes': sizes,
            'verdict': {'incremental': len(frames) > 2, 'frames': len(frames),
                        'span_ms': round(frames[-1]['ms'] - frames[0]['ms'], 1) if frames else 0.0}}


async def probe_page_hook(base_url: str) -> dict:
    """路线④（本文补充）：**页面侧 hook** —— 文档级预注入脚本 tee 住 fetch 的 ReadableStream。

    原理：不依赖 Chrome 的网络缓冲行为（页面自己就是最早的观察点）。
    实现：`Page.addScriptToEvaluateOnNewDocument` 注入 → 包装 `window.fetch` → clone 响应体逐块收进
          `window.__m7b_chunks`；探针每 300ms 轮询一次长度 → 看它是否**边到边长**。
    """
    hook = r"""
(() => {
  window.__m7b_chunks = [];
  window.__m7b_err = '';
  const origFetch = window.fetch;
  window.fetch = function (...args) {
    return origFetch.apply(this, args).then((resp) => {
      try {
        if (resp.body) {
          // 关键：流式响应不能 clone()（会抛），必须用 tee() 分流
          const [mine, theirs] = resp.body.tee();
          const reader = mine.getReader();
          const dec = new TextDecoder();
          (async () => {
            for (;;) {
              const { done, value } = await reader.read();
              if (done) break;
              window.__m7b_chunks.push({ t: Math.round(performance.now()),
                                        s: dec.decode(value, { stream: true }) });
            }
          })();
          return new Response(theirs, resp);
        }
      } catch (e) { window.__m7b_err = String(e); }
      return resp;
    });
  };
})();
"""
    samples: list[dict] = []
    async with Chrome(options=headless_options()) as browser:
        tab = await browser.start()
        t0 = time.perf_counter()
        await tab._execute_command(  # noqa: SLF001
            cmd('Page.enable'))
        await tab._execute_command(  # noqa: SLF001
            cmd('Page.addScriptToEvaluateOnNewDocument', source=hook))
        await tab.go_to(f'{base_url}/page?mode=fetch')
        for _ in range(int((COUNT * GAP + 3.0) / 0.3)):
            got = await tab.execute_script(
                'return JSON.stringify({n: (window.__m7b_chunks || []).length,'
                ' err: window.__m7b_err || "", t: (window.__m7b_chunks || []).map(c => c.t)});')
            value = js_text(got)
            try:
                info = json.loads(str(value))
                count, err = int(info.get('n', 0)), info.get('err', '')
                times = info.get('t', [])
            except Exception:  # noqa: BLE001
                count, err, times = 0, str(value)[:80], []
            samples.append({'ms': round((time.perf_counter() - t0) * 1000.0, 1),
                            'count': count, 'err': err, 'chunk_ms': times[:12]})
            await asyncio.sleep(0.3)
    return {'samples': samples, 'final_count': samples[-1]['count'] if samples else 0}


def verdict(frames: list[dict]) -> dict:
    if len(frames) < 2:
        return {'incremental': False, 'frames': len(frames), 'span_ms': 0.0}
    first, last = frames[0]['ms'], frames[-1]['ms']
    return {'incremental': (last - first) > 500.0, 'frames': len(frames),
            'span_ms': round(last - first, 1), 'first_ms': first, 'last_ms': last}


async def main() -> int:
    import sys

    only = {int(a) for a in sys.argv[1:] if a.isdigit()}  # 可只跑指定路线：python m7b03_cdp_stream.py 4
    OUT_DIR.mkdir(exist_ok=True)

    def wanted(route: int) -> bool:
        return not only or route in only

    with LocalServer() as server:
        base = f'http://127.0.0.1:{server.port}'
        print(f'[M7B-03] 本地服务器 {base}（/sse 与 /stream 各 {COUNT} 块、间隔 {GAP}s）')
        route1 = await probe_fetch_stream(base) if wanted(1) else {'frames': []}
        if wanted(1):
            print(f'[M7B-03] 路线①(小负载) 完成：{len(route1["frames"])} 帧')
        route1b = await probe_fetch_stream_big(base) if wanted(1) else {
            'paused_ms': None, 'frames': []}
        if wanted(1):
            print(f'[M7B-03] 路线①(1.5MB/12s) 完成：pause@{route1b["paused_ms"]}ms，'
                  f'{len(route1b["frames"])} 帧')
        route1c = await probe_fetch_stream_tiny_slow(base) if wanted(1) else {
            'paused_ms': None, 'frames': [], 'stream_total_s': 0.0}
        if wanted(1):
            print(f'[M7B-03] 路线①(小体量 960B/18s) 完成：pause@{route1c["paused_ms"]}ms，'
                  f'{len(route1c["frames"])} 帧')
        route1d = await probe_fetch_read_granularity(
            base, read_size=int(os.environ.get('M7B_READ_SIZE', '4096'))) if wanted(5) else {
            'paused_ms': None, 'frames': [], 'distinct_sizes': [], 'read_size': 0,
            'chunk_size': 0, 'verdict': {'incremental': False, 'frames': 0, 'span_ms': 0.0}}
        if wanted(5):
            print(f'[M7B-03] 路线①(粒度判决 100B×100/read={route1d["read_size"]}) 完成：'
                  f'{len(route1d["frames"])} 帧，出现的读取长度={route1d["distinct_sizes"]}')
        route4 = await probe_page_hook(base) if wanted(4) else {'samples': [], 'final_count': 0}
        if wanted(4):
            print(f'[M7B-03] 路线④(页面 hook) 完成：终值 {route4["final_count"]} 块'
                  f'（err={route4["samples"][-1]["err"] if route4["samples"] else ""}）')
        route3 = await probe_network_data_received(base) if wanted(3) else {
            'events': [], 'final_body_len': 0, 'final_body_head': ''}
        if wanted(3):
            print(f'[M7B-03] 路线③ 完成：{len(route3["events"])} 个事件')
        route2 = await probe_sse(base) if wanted(2) else {'messages': []}
        if wanted(2):
            print(f'[M7B-03] 路线② 完成：{len(route2["messages"])} 条消息')

    report = {
        'gap_s': GAP, 'count': COUNT,
        'route1_fetch_stream_small': {'frames': route1['frames'],
                                      'verdict': verdict(route1['frames'])},
        'route1_fetch_stream_big': {'paused_ms': route1b['paused_ms'],
                                    'frames': route1b['frames'],
                                    'verdict': verdict(route1b['frames'])},
        'route1_fetch_stream_tiny_slow': {
            'paused_ms': route1c['paused_ms'], 'stream_total_s': route1c['stream_total_s'],
            'frames': route1c['frames'], 'verdict': verdict(route1c['frames'])},
        'route2_sse_event_source': {'messages': route2['messages'],
                                    'verdict': verdict(route2['messages'])},
        'route3_network_data_received': {
            'events': route3['events'], 'verdict': verdict(route3['events']),
            'final_body_len': route3['final_body_len'],
            'final_body_head': route3['final_body_head']},
        'route1_fetch_read_granularity': route1d,
        'route4_page_hook': {'samples': route4['samples'], 'final_count': route4['final_count'],
                             'verdict': {'incremental': bool(route4['samples']) and
                                         route4['final_count'] > 1 and
                                         route4['samples'][0]['count'] < route4['final_count'],
                                         'frames': route4['final_count'],
                                         'span_ms': (route4['samples'][-1]['ms'] -
                                                     route4['samples'][0]['ms'])
                                         if route4['samples'] else 0.0}},
    }
    (OUT_DIR / 'm7b03_cdp_stream.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    print('\n=== 结论 ===')
    for key, label in (('route1_fetch_stream_small', '① Fetch 流（小负载 512B）'),
                       ('route1_fetch_stream_big', '① Fetch 流（大负载 1.5MB/12s）'),
                       ('route1_fetch_stream_tiny_slow', '① Fetch 流（小体量 960B/18s ← 真实形态）'),
                       ('route2_sse_event_source', '② Network.eventSourceMessageReceived'),
                       ('route3_network_data_received', '③ Network.dataReceived + getResponseBody'),
                       ('route4_page_hook', '④ 页面 hook（tee ReadableStream）')):
        data = report[key]
        v = data['verdict']
        extra = ''
        if key.endswith('data_received'):
            extra = f" · 末尾全文 {data['final_body_len']} 字节"
        if data.get('paused_ms') is not None:
            extra = f" · pause@{data['paused_ms']} ms / 流时长 {data.get('stream_total_s', '')}s"
        print(f'{label:44} 增量={str(v["incremental"]):5} 帧={v["frames"]:2} '
              f'跨度={v["span_ms"]:>7.1f} ms{extra}')
    print(f'\n证据已写入 {OUT_DIR / "m7b03_cdp_stream.json"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(asyncio.run(main()))
