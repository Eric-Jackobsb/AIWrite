# -*- coding: utf-8 -*-
"""M7B 前置验证 —— 本地测试服务器（一次性；**只为验证，不进产品**）。

用途：把「CDP 能否拿到增量」与「某站点是否已登录 / 是否改版」**解耦** ——
      M7B-03（流式）/ M7B-05（页面驱动）/ M7B-08（多站点）全部在 localhost 上自证。

端点：
  /sse?n=8&gap=0.35         → text/event-stream，逐条 flush（验 Network.eventSourceMessageReceived）
  /stream?n=8&gap=0.35&size=64 → 分块（chunked）二进制流（验 Fetch.takeResponseBodyAsStream + IO.read
                                与 Network.dataReceived + getResponseBody）
  /page?mode=sse|fetch|drive  → 测试页（自动触发 / 带输入框与文件框）
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import parse_qs, urlparse
import time


DRIVE_PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>M7B drive probe</title></head>
<body>
<h3>M7B-05 页面驱动测试页</h3>
<textarea id="box" rows="3" cols="60" placeholder="在这里打字"></textarea>
<input id="file" type="file" multiple>
<div id="log"></div>
<script>
  // 记录注入的文件名（供断言：DataTransfer / file chooser 注入是否真的落到 input.files）
  const fileEl = document.getElementById('file');
  fileEl.addEventListener('change', () => {
    document.getElementById('log').textContent =
      'FILES=' + Array.from(fileEl.files).map(f => f.name + ':' + f.size).join('|');
  });
  // 记录逐字输入的时间戳（供断言：是否「逐字符」而非一次性灌入）
  const box = document.getElementById('box');
  box.addEventListener('input', () => {
    window.__inputs = (window.__inputs || 0) + 1;
    window.__stamps = (window.__stamps || []).concat([Math.round(performance.now())]);
  });
</script>
</body></html>"""

COOKIE_PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>M7B cookie probe</title></head>
<body>
<h3>M7B-01 Cookie 可见性测试页</h3>
<div id="doc">DOC_COOKIE=<span id="dc"></span></div>
<script>
  // JS 可写的普通 Cookie（HttpOnly 的看不到 —— 这正是本项要证的差异）
  document.cookie = 'm7b_js=3; max-age=86400; path=/';
  document.getElementById('dc').textContent = document.cookie;
</script>
</body></html>"""


def _page(mode: str, query: dict) -> bytes:
    if mode == 'drive':
        return DRIVE_PAGE.encode('utf-8')
    if mode == 'cookies':
        return COOKIE_PAGE.encode('utf-8')
    count = query.get('n', ['8'])[0]
    gap = query.get('gap', ['0.35'])[0]
    size = query.get('size', ['64'])[0]
    starter = {
        'sse': f"const es = new EventSource('/sse?n={count}&gap={gap}');"
               "es.onmessage = e => { window.__got = (window.__got||0)+1; };",
        'fetch': f"fetch('/stream?n={count}&gap={gap}&size={size}').then(async r => {{"
                 "const rd = r.body.getReader();"
                 "for (;;) { const {done, value} = await rd.read(); if (done) break;"
                 "window.__got = (window.__got||0)+1; } });",
    }.get(mode, '')
    html = ("<!doctype html><html><head><meta charset='utf-8'><title>m7b probe</title></head>"
            f"<body><div id='m'>{mode}</div><script>{starter}</script></body></html>")
    return html.encode('utf-8')


class _Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):  # 静音（避免刷屏）
        pass

    def _chunk(self, payload: bytes) -> None:
        self.wfile.write(b'%X\r\n' % len(payload) + payload + b'\r\n')
        self.wfile.flush()

    def do_GET(self):  # noqa: N802（BaseHTTPRequestHandler 约定）
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        if parsed.path == '/page':
            body = _page(query.get('mode', ['fetch'])[0], query)
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if parsed.path == '/setcookies':
            # M7B-01：一条 HttpOnly（`document.cookie` 看不到）+ 一条普通 + 一条普通（持久）
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Set-Cookie', 'm7b_http=secret1; Max-Age=86400; HttpOnly; Path=/')
            self.send_header('Set-Cookie', 'm7b_plain=plain1; Max-Age=86400; Path=/')
            self.send_header('Set-Cookie', 'm7b_js_session=sess1; Path=/')  # 会话 Cookie（关窗即失效）
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if parsed.path == '/setcookie':
            # M7B-09：通用写 Cookie（名字唯一 → 同名 Cookie 不污染跨 case 判定）
            #   /setcookie?name=X&value=Y&max_age=86400&http_only=1&session=0
            name = query.get('name', ['c'])[0]
            value = query.get('value', ['v'])[0]
            parts = [f'{name}={value}', 'Path=/']
            if query.get('session', ['0'])[0] == '1':
                pass                                    # 会话 Cookie（不写 Max-Age）
            else:
                parts.append(f"Max-Age={query.get('max_age', ['86400'])[0]}")
            if query.get('http_only', ['0'])[0] == '1':
                parts.append('HttpOnly')
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Set-Cookie', '; '.join(parts))
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if parsed.path == '/eyes':
            # M7B-09：**服务端视角**回显请求带的 Cookie（证明"站点是否认账"，
            # 而不是只看浏览器本地库 —— 两者在回灌后可能不一致）
            raw = self.headers.get('Cookie', '')
            body = ('{"cookie_header":"' + raw.replace('"', '\\"') + '"}').encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        count = int(query.get('n', ['8'])[0])
        gap = float(query.get('gap', ['0.35'])[0])

        if parsed.path == '/sse':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for i in range(count):
                self._chunk(f'data: chunk-{i}\n\n'.encode('utf-8'))
                time.sleep(gap)
            self._chunk(b'')
            return

        if parsed.path == '/stream':
            size = int(query.get('size', ['64'])[0])
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for i in range(count):
                payload = (f'chunk-{i}-'.encode('ascii')).ljust(size, b'.')
                self._chunk(payload)
                time.sleep(gap)
            self._chunk(b'')
            return

        self.send_response(404)
        self.send_header('Content-Length', '0')
        self.end_headers()


class LocalServer:
    """一次性本地服务器（上下文管理器；端口 0 = 自动分配）。"""

    def __init__(self, port: int = 0):
        self._httpd = ThreadingHTTPServer(('127.0.0.1', port), _Handler)
        self._thread = Thread(target=self._httpd.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return self._httpd.server_address[1]

    def url(self, path: str) -> str:
        return f'http://127.0.0.1:{self.port}{path}'

    def url_host(self, host: str, path: str) -> str:
        """按指定 host 生成 URL（M7B-08 用：`localhost` 与 `127.0.0.1` 是**不同 Cookie 域**）。"""
        return f'http://{host}:{self.port}{path}'

    def __enter__(self) -> 'LocalServer':
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
