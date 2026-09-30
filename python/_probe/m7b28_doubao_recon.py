# -*- coding: utf-8 -*-
"""M7B-28 / P7b-05b 前置侦察 —— **豆包（doubao-web）网页版：上传入口 + 视觉性质**（一次性探针）。

对应计划与验收：
  * `M7.md` §12.1 **`P7b-05b`**：上传入口只读侦察 + 三项选择器命中 + `D-30` 证据 + 纯图无字判别
  * `M7B.md` §10「**网页版图片理解侦察记录**」表 +「登录 Cookie 存活口径」豆包行
  * `M_patchAB_rest.md` §9 **`D-30`**（登录前后 Cookie 名差集 → `cookie_names` 优先）
  * 决策 `M7.md` §9 `D10`：首个目标站 = `doubao-web`；`deepseek-web` 保持文字主线

四段（可单独跑；**默认只跑 B1 只读侦察**）：
  B0 `--login`   开有头窗口 → **你手动登录** → 自动检测登录成功 → Cookie 前后快照 → 优雅退出 → 重启复读
  B1 `--recon`   只读侦察：上传入口 4 类判定（`file_input`/`drop_zone`/`paste_only`/`none`）+ 选择器候选
  B2 `--inject`  注入 1 张图（**不发送**）+ CDP `Network` 网络回执取证
  B3 `--send`    发送一轮问答（**默认关**；`I18`：无「注入成功」证据不得发送）→ 判别 **真视觉 / OCR**

纪律（`M7B.md` §5 / `source/README.md` §6.2）：
  * **headful**（headless 的 UA 含 `HeadlessChrome`，本身就是"非真实浏览器"信号）
  * `execute_script` 返回**两层 `result`** → 一律用 `common.js_text()` 解包
  * **读回先自证**（`common.assert_read_path`）才算"通道能读回"
  * **只读 JS**：不写值、不点击、不发请求（口径对齐 `dom_web_client.cpp` 的 `kProbeScript`）
  * **不绕过风控 / 验证码 / 指纹**（`VB2-37`）；**不注入站点内部端点**（`I16`）
  * 优雅退出保登录态；`try/finally` + 残留自检（`M7B-09`）；Cookie 只落名字/标志位
"""
import argparse
import asyncio
import base64
import json
import pathlib
import sys
import tempfile
import time

from pydoll.browser.chromium import Chrome

import m7b09_common as common

SITE_ID = 'doubao-web'
SITE_URL = 'https://www.doubao.com/chat/'
EVIDENCE = common.OUT / 'm7b28_doubao_recon.json'
FIXTURE_DIR = common.OUT / 'drive_files'
IMAGE_FIXTURE = FIXTURE_DIR / 'm7b28-doubao.png'
# 网络回执里判定「这一条是上传请求」的 URL 特征（只用于筛选，不改请求）
UPLOAD_HINTS = ('upload', 'file', 'attach', 'image', 'media', 'photo', 'album', 'tos-', 'vod')


# --------------------------------------------------------------- 只读侦察 JS --
# 只读：只枚举可见元素与属性，**不写值 / 不点击 / 不发请求**；返回 JSON 字符串。
# ⚠️ 关键约束（`_diag_m7b28_js.py` 实证）：pydoll 的 `execute_script` 把脚本当**函数体**执行，
#    **裸表达式**（如 `(function(){...})();`）的返回值会被丢弃（实测 `type: 'undefined'`）
#    → 脚本必须写成「函数体 + 显式 `return`」。下面三段 JS 都是这种形态。
SCAN_JS = r"""
  const out = { ok: false, url: location.href, title: document.title || '', error: '',
                file_inputs: [], drop_zones: [], editors: [], attach_hints: [],
                cookie_names: [], storage_keys: [], body_len: 0 };
  const visible = (el) => {
    try {
      const r = el.getBoundingClientRect();
      const s = getComputedStyle(el);
      return r.width > 1 && r.height > 1 && s.visibility !== 'hidden' &&
             s.display !== 'none' && s.opacity !== '0';
    } catch (e) { return false; }
  };
  const q = (v) => String(v == null ? '' : v).replace(/["\\]/g, '\\$&');
  const stableClasses = (el) => {
    const raw = (el.className && typeof el.className === 'string') ? el.className.split(/\s+/) : [];
    return raw.filter(c => c && c.length <= 24 && !(/\d/.test(c) && c.length > 14)).slice(0, 2);
  };
  const selectorOf = (el) => {
    if (!el || !el.tagName) return '';
    const tag = String(el.tagName).toLowerCase();
    if (el.id) return '#' + el.id;
    const testid = el.getAttribute('data-testid') || el.getAttribute('data-test-id');
    if (testid) return tag + '[data-testid="' + q(testid) + '"]';
    const aria = el.getAttribute('aria-label');
    if (aria) return tag + '[aria-label="' + q(aria) + '"]';
    const ph = el.getAttribute('placeholder');
    if (ph) return tag + '[placeholder="' + q(ph) + '"]';
    const cls = stableClasses(el);
    return cls.length ? tag + '.' + cls.join('.') : tag;
  };
  const row = (el) => ({
    selector: selectorOf(el), tag: String(el.tagName || '').toLowerCase(), visible: visible(el),
    cls: String(el.className || '').slice(0, 100), aria: el.getAttribute('aria-label') || '',
    title: el.getAttribute('title') || '', ph: el.getAttribute('placeholder') || '',
    text: String(el.innerText || '').trim().slice(0, 20)
  });
  try {
    // ① 文件输入（**含 hidden**：站点常见做法是隐藏 input + 自绘按钮）
    document.querySelectorAll('input[type=file]').forEach(el => {
      out.file_inputs.push(Object.assign(row(el), {
        accept: el.accept || '', multiple: !!el.multiple, disabled: !!el.disabled,
        parent: el.parentElement ? selectorOf(el.parentElement) : ''
      }));
    });
    // ② 拖拽 / 附件区（class 或 aria 命中关键词；**排除 dropdown** —— 实测被「下拉菜单」误报过）
    const zoneSel = '[class*="dropzone"],[class*="drag"],[class*="upload"],[class*="attach"],' +
                    '[class*="file-upload"],[aria-label*="上传"],[aria-label*="附件"]';
    document.querySelectorAll(zoneSel).forEach(el => {
      const cls = String(el.className || '');
      if (cls.indexOf('dropdown') >= 0) { return; }   // 「下拉菜单」不是拖拽区
      const r = el.getBoundingClientRect();
      if (out.drop_zones.length < 12 && visible(el) && r.width >= 80 && r.height >= 30) {
        out.drop_zones.push(row(el));
      }
    });
    // ③ 可输入目标（composer）
    const isEditor = (el) => {
      const tag = String(el.tagName || '').toLowerCase();
      if (el.isContentEditable === true) return true;
      if (tag === 'textarea') return true;
      if (tag === 'input') return ['text', 'search', ''].indexOf(String(el.type || '').toLowerCase()) >= 0;
      return false;
    };
    document.querySelectorAll('textarea,input,[contenteditable="true"],[contenteditable=""]').forEach(el => {
      if (out.editors.length < 12 && visible(el) && isEditor(el)) {
        out.editors.push(Object.assign(row(el), { contenteditable: el.isContentEditable === true }));
      }
    });
    // ④ 附件 / 上传 / 发送按钮线索（aria-label / title / 文本）
    const keys = ['上传', '附件', '图片', '文件', '相册', '拍照', '发送',
                  'upload', 'attach', 'image', 'file', 'paperclip', 'plus', 'send'];
    document.querySelectorAll('button,[role="button"],[aria-label],[title],div[class*="icon"],span[class*="icon"]')
      .forEach(el => {
        const hint = ((el.getAttribute('aria-label') || '') + ' ' + (el.getAttribute('title') || '') +
                      ' ' + String(el.innerText || '').slice(0, 12)).trim();
        const low = hint.toLowerCase();
        if (hint && keys.some(k => low.indexOf(k) >= 0) &&
            out.attach_hints.length < 16 && visible(el)) {
          out.attach_hints.push(Object.assign(row(el), { hint: hint.slice(0, 40) }));
        }
      });
    // ⑤ Cookie 名（JS 可见者，不含 HttpOnly）+ localStorage 键名
    (document.cookie || '').split(';').forEach(p => {
      const i = p.indexOf('=');
      if (i > 0) out.cookie_names.push(p.slice(0, i).trim());
    });
    try { out.storage_keys = Object.keys(localStorage); } catch (e) { }
    out.body_len = String(document.body ? document.body.innerText || '' : '').length;
    out.ok = true;
  } catch (e) { out.error = String((e && e.message) || e); }
  return JSON.stringify(out);
"""


# ---------------------------------------- B2 兜底注入 JS（DataTransfer 合成）----
# 站点没有 input[type=file] 时才用：合成 paste / drop。
# ⚠️ 合成事件 isTrusted=false，真站点可能忽略 → **失败也如实登记**（这正是侦察要回答的）。
INJECT_DATATRANSFER_JS = r"""
  const out = { ok: false, error: '', target: selector, kind: kind };
  try {
    const el = document.querySelector(selector);
    if (!el) { out.error = 'target not found: ' + selector; return JSON.stringify(out); }
    const bin = atob(b64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    const file = new File([bytes], name, { type: mime });
    const dt = new DataTransfer();
    dt.items.add(file);
    el.focus();
    const types = kind === 'paste' ? ['paste'] : ['dragenter', 'dragover', 'drop'];
    types.forEach(t => {
      let ev = null;
      try {
        if (t === 'paste') {
          ev = new ClipboardEvent(t, { bubbles: true, cancelable: true, clipboardData: dt });
        } else {
          ev = new DragEvent(t, { bubbles: true, cancelable: true, dataTransfer: dt });
        }
      } catch (e) { ev = null; }
      if (ev) el.dispatchEvent(ev);
    });
    out.ok = true;
    out.files_len = dt.files.length;
  } catch (e) { out.error = String((e && e.message) || e); }
  return JSON.stringify(out);
"""


# ------------------------------------------------ 附件区取证 JS（B2 双证据之一）----
ATTACH_PROBE_JS = r"""
  const out = { name_hit: false, attach_nodes: 0, img_nodes: 0, editor_text_len: -1 };
  try {
    const text = String(document.body ? document.body.innerText || '' : '');
    out.name_hit = name && text.indexOf(name) >= 0;
    document.querySelectorAll('[class*="attach"],[class*="file"],[class*="upload"],[class*="preview"]')
      .forEach(el => {
        if (String(el.className).indexOf('attach') >= 0 || el.querySelectorAll('img').length > 0) {
          out.attach_nodes++;
        }
      });
    out.img_nodes = document.querySelectorAll('img').length;
    const ed = document.querySelector('[contenteditable="true"],textarea');
    if (ed) out.editor_text_len = String(ed.innerText || ed.value || '').length;
  } catch (e) { }
  return JSON.stringify(out);
"""


# ------------------------------------------------------------------- 工具函数 --
def js_json(reply) -> dict:
    """两层 `result` 解包（复用 `common.js_text`）→ JSON 对象；解析失败返回空字典。"""
    raw = common.js_text(reply)
    if not raw:
        return {}
    try:
        data = json.loads(raw)
    except Exception:  # noqa: BLE001
        return {}
    return data if isinstance(data, dict) else {}


async def cookie_snapshot(tab) -> list[dict]:
    """**全库** Cookie（`Storage.getCookies`，不带 urls）。

    ⚠️ `tab.get_cookies()` 是**当前页作用域**（停在 about:blank 必读空）—— `M7B-09` V-d 实测结论。
    """
    reply = await tab._execute_command(  # noqa: SLF001
        {'method': 'Storage.getCookies', 'params': {}})
    cookies = reply.get('result', {}).get('cookies', []) if isinstance(reply, dict) else []
    return common.norm(cookies)


async def scan(tab) -> dict:
    return js_json(await tab.execute_script(SCAN_JS))


async def attach_probe(tab, name: str) -> dict:
    """附件区取证：页面上是否出现文件名 / 附件卡片 / img 数（B2 的「页面状态」证据）。

    参数用 `json.dumps` 注入（避免手写转义），并把 IIFE 形参换成局部常量。
    """
    script = 'const name = ' + json.dumps(name, ensure_ascii=False) + ';\n' + ATTACH_PROBE_JS
    return js_json(await tab.execute_script(script))


def classify(scan_data: dict) -> str:
    """上传入口 4 类判定（顺序即优先级，口径写死于 `M7.md` §12.1）。

    ⚠️ `drop_zone` 只认「真拖拽区」（已排除 `dropdown` 下拉菜单误报，并要求最小尺寸）——
    doubao 实测：未登录态**没有** file input / 真拖拽区，只有 ProseMirror 富文本 composer。
    """
    if scan_data.get('file_inputs'):
        return 'file_input'
    if scan_data.get('drop_zones'):
        return 'drop_zone'
    if any(item.get('contenteditable') for item in (scan_data.get('editors') or [])):
        return 'paste_only'
    return 'none'


def composer_center_js(selector: str) -> str:
    """取 composer 中心坐标（供 `--hover-composer` 把鼠标移上去触发工具条；**不点击**）。"""
    return ('const el = document.querySelector(' + json.dumps(selector) + ');\n'
            'if (!el) { return JSON.stringify({ ok: false }); }\n'
            'const r = el.getBoundingClientRect();\n'
            'return JSON.stringify({ ok: true, x: Math.round(r.left + r.width / 2),'
            ' y: Math.round(r.top + r.height / 2), w: Math.round(r.width),'
            ' h: Math.round(r.height) });')


def names_of(cookies) -> list[str]:
    return common.names(cookies)


# --------------------------------------------------------------- 浏览器动作 --
async def open_site(browser):
    """起 tab → 等首屏稳定（headful 首次运行会换初始 target）→ 导航站点。"""
    tab = await browser.start()
    await asyncio.sleep(1.5)
    await tab.go_to(SITE_URL)
    await asyncio.sleep(3.0)  # SPA 首屏渲染（含登录态判定所需的 composer）
    return tab


def make_fixture() -> pathlib.Path:
    """纯图无字夹具（B3 判别「真视觉 vs OCR」用）：1×1 纯红 PNG，**不含任何文字**。"""
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    if not IMAGE_FIXTURE.exists():
        IMAGE_FIXTURE.write_bytes(base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=='))
    return IMAGE_FIXTURE


# ------------------------------------------------------------------- B0 登录 --
async def phase_login(args, report: dict) -> None:
    """B0：headful 开窗 → **用户手动登录** → 前后快照（`D-30` 证据）→ 优雅退出 → 重启复读。"""
    for attempt in (1, 2):
        label = '登录' if attempt == 1 else '重启复读'
        browser = Chrome(options=common.options(headless=False))
        try:
            tab = await open_site(browser)
            proof = await common.assert_read_path(tab)  # 读回自证（否则一切读数不可信）
            print(f'[B0/{label}] 读回自证 = {proof or "（失败）"}')
            if attempt == 1:
                scan_before = await scan(tab)
                rows_before = await cookie_snapshot(tab)
                report['login'] = {
                    'readback_self_proof': proof,
                    'url': scan_before.get('url'), 'title': scan_before.get('title'),
                    'cookie_rows_before': rows_before,
                    'cookie_names_before': names_of(rows_before),
                    'composer_before': len(scan_before.get('editors') or []),
                    'login_timeout_s': args.login_timeout,
                }
                print(f'[B0/登录] 登录前：Cookie {len(rows_before)} 条 {names_of(rows_before)}；'
                      f'输入框候选 {report["login"]["composer_before"]} 个')
                print('[B0/登录] 请在窗口里手动登录（不代填密码、不绕验证码）……')
                started = time.monotonic()
                logged = False
                while time.monotonic() - started < args.login_timeout:
                    await asyncio.sleep(3.0)
                    now = await scan(tab)
                    rows = await cookie_snapshot(tab)
                    fresh = sorted(set(names_of(rows)) - set(names_of(rows_before)))
                    composer = len(now.get('editors') or [])
                    print(f'[B0/登录] {int(time.monotonic() - started):4d}s：Cookie {len(rows)} 条'
                          f'（新增 {fresh}）/ 输入框 {composer} 个')
                    if fresh and composer > 0:
                        logged = True
                        report['login'].update({
                            'cookie_rows_after': rows, 'cookie_names_after': names_of(rows),
                            'new_cookie_names': fresh, 'composer_after': composer,
                            'login_wait_s': int(time.monotonic() - started)})
                        print(f'[B0/登录] ✅ 检测到登录成功（新增 Cookie {fresh}）')
                        break
                report['login']['logged_in_detected'] = logged
                if not logged:
                    report['login']['note'] = (f'{args.login_timeout}s 内未检测到登录成功：'
                                               'Cookie 名未新增或输入框未出现')
                    print(f'[B0/登录] ⚠️ {report["login"]["note"]}')
            else:
                rows = await cookie_snapshot(tab)
                now = await scan(tab)
                fresh = set(report.get('login', {}).get('new_cookie_names') or [])
                still = sorted(fresh & set(names_of(rows)))
                report['relaunch'] = {
                    'readback_self_proof': proof, 'cookie_count': len(rows),
                    'cookie_names': names_of(rows), 'new_names_still_present': still,
                    'composer_count': len(now.get('editors') or []),
                    'still_logged_in': bool(still) and bool(now.get('editors')),
                    'storage_keys': (now.get('storage_keys') or [])[:20],
                }
                print(f'[B0/重启复读] Cookie {len(rows)} 条 / 新增名仍在 {still} / '
                      f'输入框 {report["relaunch"]["composer_count"]} 个 → '
                      f'仍登录 = {report["relaunch"]["still_logged_in"]}')
        finally:
            info = await common.shutdown(browser, 'close_wait')  # 优雅退出（保登录态）
            report['shutdown_login' if attempt == 1 else 'shutdown_relaunch'] = info
            print(f'[B0/{label}] 退出：模式 {info.get("mode")} / 等 {info.get("waited_s")}s / '
                  f'退出码 {info.get("exit_code")} / 残留 {len(info.get("strays_after") or [])}')

    # 登录 Cookie 存活口径（`M7B.md` §10 表用）+ `D-30` 差集（喂「cookie_names 优先」判据）
    login = report.get('login') or {}
    report['cookie_ttl'] = [
        {'name': c.get('name'), 'session': c.get('session'), 'expires': c.get('expires'),
         'http_only': c.get('http_only'), 'secure': c.get('secure'), 'domain': c.get('domain')}
        for c in (login.get('cookie_rows_after') or [])
    ]
    report['d30_cookie_names_diff'] = {
        'before': login.get('cookie_names_before') or [],
        'after': login.get('cookie_names_after') or [],
        'new': login.get('new_cookie_names') or [],
        'note': '登录后新增的 Cookie 名 = 候选 cookie_names（D-30 的豆包证据）',
    }


# ------------------------------------------------------------------- B1 侦察 --
async def phase_recon(tab, hover: bool = False) -> dict:
    """B1：**只读**侦察 —— 上传入口 4 类判定 + 选择器候选与真实命中数（不注入、不发送）。

    `hover=True`（`--hover-composer`）会先把鼠标**移到 composer 上（只移动、不点击）**再扫描 ——
    很多站点的附件按钮只在 hover 态渲染，这是拿到「上传入口」的必要动作。
    """
    proof = await common.assert_read_path(tab)  # 读回自证
    hover_info: dict = {}
    if hover:
        pre = await scan(tab)
        editor_pre = ((pre.get('editors') or [{}])[0].get('selector') or '')
        if editor_pre:
            center = js_json(await tab.execute_script(composer_center_js(editor_pre)))
            if center.get('ok'):
                await tab.mouse.move(int(center.get('x', 0)), int(center.get('y', 0)))
                await asyncio.sleep(0.5)
                await tab.mouse.move(int(center.get('x', 0)) + 10, int(center.get('y', 0)) - 8)
                await asyncio.sleep(1.2)
            hover_info = {'selector': editor_pre, 'center': center,
                          'note': '仅移动鼠标（不点击）以触发工具条渲染'}
        else:
            hover_info = {'note': '未找到 composer，跳过悬停'}
    data = await scan(tab)
    result = {
        'readback_self_proof': proof,
        'url': data.get('url'), 'title': data.get('title'),
        'body_len': data.get('body_len'), 'scan_ok': data.get('ok'), 'scan_error': data.get('error'),
        'entry_kind': classify(data),
        'file_inputs': data.get('file_inputs') or [], 'drop_zones': data.get('drop_zones') or [],
        'editors': data.get('editors') or [], 'attach_hints': data.get('attach_hints') or [],
        'cookie_names_js_visible': data.get('cookie_names') or [],
        'storage_keys': (data.get('storage_keys') or [])[:30],
        'hover': hover_info,
    }
    # 选择器候选 → **真实命中数**（对齐 `--web-adapter-selftest`：命中 0 不算就绪）
    candidates = []
    candidates += [e.get('selector') for e in result['editors'][:4]]
    candidates += [h.get('selector') for h in result['attach_hints'][:6]]
    candidates += [z.get('selector') for z in result['drop_zones'][:4]]
    candidates += [f.get('selector') for f in result['file_inputs'][:3]]
    candidates += ['[class*="message"]', '[class*="answer"]', '[class*="reply"]', '[class*="markdown"]']
    candidates = [c for c in dict.fromkeys(candidates) if c]
    if candidates:
        verify_js = ('const sels = ' + json.dumps(candidates, ensure_ascii=False) + ';\n'
                     'const hits = {};\n'
                     'sels.forEach(function (s) { try { hits[s] = document.querySelectorAll(s).length; }'
                     ' catch (e) { hits[s] = -1; } });\n'
                     'return JSON.stringify(hits);')
        result['selector_hits'] = js_json(await tab.execute_script(verify_js))
    return result


def pick_send_target(recon: dict) -> str:
    """从 `attach_hints` 里挑「发送」按钮选择器（B3 用；B2 不发送）。"""
    for hint in (recon.get('attach_hints') or []):
        low = (hint.get('hint') or '').lower()
        if '发送' in low or 'send' in low:
            return hint.get('selector') or ''
    return ''


# ------------------------------------------------------------------- B2 注入 --
async def phase_inject(tab, recon: dict, args) -> dict:
    """B2：注入 1 张图（**不发送**）+ CDP `Network` 回执 + 附件区页面证据。

    双证据口径（`P7b-05`）：**页面状态**（附件节点 / 文件名命中 / `input.files` 读回）
    **且** **网络回执**（命中上传特征的请求）—— 两者齐备才叫「上传完成」。
    """
    image = make_fixture()
    size = len(image.read_bytes())
    b64 = base64.b64encode(image.read_bytes()).decode('ascii')
    kind = recon.get('entry_kind') or 'none'
    mode = args.inject_mode
    if mode == 'auto':
        mode = 'dom' if recon.get('file_inputs') else ('chooser' if recon.get('attach_hints')
                                                       else 'datatransfer')
    target_file = ((recon.get('file_inputs') or [{}])[0].get('selector') or '')
    target_editor = ((recon.get('editors') or [{}])[0].get('selector') or '')
    before = await attach_probe(tab, image.name)

    await tab.enable_network_events()  # 网络回执（`P7b-05` 双证据的第二半）
    mark = len(await tab.get_network_logs())
    steps = []
    injected_files = ''

    if mode == 'dom' and target_file:
        document = await tab._execute_command(  # noqa: SLF001
            {'method': 'DOM.getDocument', 'params': {}})
        root = document.get('result', {}).get('root', {}).get('nodeId')
        node = await tab._execute_command(  # noqa: SLF001
            {'method': 'DOM.querySelector', 'params': {'nodeId': root, 'selector': target_file}})
        node_id = node.get('result', {}).get('nodeId')
        response = await tab._execute_command(  # noqa: SLF001
            {'method': 'DOM.setFileInputFiles',
             'params': {'files': [str(image)], 'nodeId': node_id}})
        steps.append({'mode': 'dom', 'selector': target_file, 'node_id': node_id,
                      'response': response.get('result', response)})
    elif mode == 'chooser':
        click_selector = ''
        for hint in (recon.get('attach_hints') or []):
            low = (hint.get('hint') or '').lower()
            if any(k in low for k in ('上传', '图片', '附件', '相册', 'upload', 'image',
                                      'attach', 'file', 'paperclip', 'plus')):
                click_selector = hint.get('selector') or ''
                break
        click_selector = click_selector or target_file
        try:
            async with tab.expect_file_chooser([str(image)]):
                await tab.execute_script(
                    'document.querySelector(' + json.dumps(click_selector) + ').click();',
                    user_gesture=True)
                await asyncio.sleep(1.0)
            steps.append({'mode': 'chooser', 'clicked': click_selector, 'ok': True})
        except Exception as exc:  # noqa: BLE001
            steps.append({'mode': 'chooser', 'clicked': click_selector, 'ok': False,
                          'error': f'{type(exc).__name__}: {exc}'})
    else:  # datatransfer：站点没有 file input 时的兜底（合成 paste / drop）
        target = target_editor or ((recon.get('drop_zones') or [{}])[0].get('selector') or '')
        sub_kind = 'paste' if target == target_editor else 'drop'
        script = ('const selector = ' + json.dumps(target) + ';\n'
                  'const b64 = ' + json.dumps(b64) + ';\n'
                  'const name = ' + json.dumps(image.name) + ';\n'
                  'const mime = "image/png";\n'
                  'const kind = ' + json.dumps(sub_kind) + ';\n' + INJECT_DATATRANSFER_JS)
        steps.append({'mode': 'datatransfer', 'target': target, 'kind': sub_kind,
                      'result': js_json(await tab.execute_script(script))})

    await asyncio.sleep(args.capture_s)  # 给站点时间发上传请求 / 渲染附件卡片
    logs = await tab.get_network_logs()
    new_logs = logs[mark:]
    uploads = []
    for entry in new_logs:
        request = (entry.get('params') or {}).get('request') or {}
        url = str(request.get('url') or '')
        if any(hint in url.lower() for hint in UPLOAD_HINTS):
            uploads.append({
                'url': url[:200], 'method': request.get('method') or '',
                'has_post_data': bool(request.get('postData')),
                'resource_type': (entry.get('params') or {}).get('type')})
    after = await attach_probe(tab, image.name)
    if target_file:
        injected_files = common.js_text(await tab.execute_script(
            'return Array.from((document.querySelector(' + json.dumps(target_file) +
            ') || {files: []}).files).map(f => f.name).join(",");'))
    page_ok = (bool(after.get('name_hit')) or bool(injected_files) or
               (int(after.get('attach_nodes') or 0) - int(before.get('attach_nodes') or 0)) > 0)
    return {'entry_kind': kind, 'mode': mode, 'image': str(image), 'image_bytes': size,
            'steps': steps, 'network_new_total': len(new_logs), 'upload_requests': uploads,
            'attach_before': before, 'attach_after': after, 'file_input_files': injected_files,
            'injected_ok': page_ok, 'capture_s': args.capture_s}


# ------------------------------------------------------------------- B3 发送 --
async def phase_send(tab, recon: dict, inject: dict, args) -> dict:
    """B3：发送一轮问答（**默认关**）。

    `I18`：**无「注入成功」证据不得发送** —— 这里做成硬闸门（拒绝并留证），
    发送内容固定为「纯图无字」判别提示词，用于区分 **真视觉 / OCR**。
    """
    out = {'enabled': True, 'sent': False}
    if not inject.get('injected_ok'):
        out['refused_reason'] = ('I18：未取得「注入成功」证据（页面状态 / 网络回执），拒绝发送'
                                 ' —— 请先修好注入路线（P7b-10）')
        print(f'[B3] ⛔ {out["refused_reason"]}')
        return out

    editor = ((recon.get('editors') or [{}])[0].get('selector') or '')
    if not editor:
        out['refused_reason'] = '未找到输入框选择器（composer）'
        print(f'[B3] ⛔ {out["refused_reason"]}')
        return out

    send_target = pick_send_target(recon)
    await tab.execute_script('document.querySelector(' + json.dumps(editor) + ').focus();')
    await tab.keyboard.type_text(args.prompt)  # humanize 逐字（M7B-05 实测 154 ms/字符）
    baseline = common.js_text(await tab.execute_script('return String(document.body.innerText.length);'))
    if send_target:
        await tab.execute_script('document.querySelector(' + json.dumps(send_target) + ').click();',
                                 user_gesture=True)
    else:
        await tab.keyboard.press('Enter')  # 兜底：Enter 发送
    out.update({'editor': editor, 'send_target': send_target or 'Enter(键盘)',
                'prompt': args.prompt, 'baseline_len': baseline})

    started = time.monotonic()
    best = ''
    stable = 0
    while time.monotonic() - started < args.answer_timeout:
        await asyncio.sleep(3.0)
        text = common.js_text(await tab.execute_script('return document.body.innerText;'))
        print(f'[B3] {int(time.monotonic() - started):4d}s：正文 {len(text)} 字符'
              f'（基线 {baseline}）')
        if len(text) > len(best):
            best = text
            stable = 0
        else:
            stable += 1
        if best and len(best) > int(baseline or 0) + 10 and stable >= 3:
            break  # 已增长且连续 3 次不变 → 视为回答完毕
    out['sent'] = True
    out['answer_tail'] = best[-1500:]
    out['answer_len'] = len(best)
    print(f'[B3] 回答尾部（供人工判定「真视觉 vs OCR」）：\n{out["answer_tail"][-600:]}')
    return out


# ------------------------------------------------------------------- 判定 ----
def verdict_of(report: dict) -> dict:
    recon = report.get('recon') or {}
    inject = report.get('inject') or {}
    login = report.get('login') or {}
    send = report.get('send') or {}
    return {
        'B1_readback_self_proof_ok': recon.get('readback_self_proof') == 'probe-ok',
        'B1_scan_ok': bool(recon.get('scan_ok')),
        'B1_entry_kind_determined': recon.get('entry_kind') in
                                    ('file_input', 'drop_zone', 'paste_only', 'none'),
        'B1_has_file_input': recon.get('entry_kind') == 'file_input',
        'B1_composer_found': bool(recon.get('editors')),
        'B0_login_detected': login.get('logged_in_detected'),
        'B0_d30_diff_nonempty': bool((report.get('d30_cookie_names_diff') or {}).get('new')),
        'B0_relaunch_still_logged_in': (report.get('relaunch') or {}).get('still_logged_in'),
        'B2_injected_ok': inject.get('injected_ok'),
        'B2_upload_request_seen': bool(inject.get('upload_requests')) if inject else None,
        'B3_sent': send.get('sent') if send else None,
    }


# ------------------------------------------------------------------- 主流程 --
async def run(args) -> dict:
    report: dict = {'site': SITE_ID, 'site_url': SITE_URL, 'argv': sys.argv[1:],
                    'started': time.strftime('%Y-%m-%d %H:%M:%S')}
    if args.login:
        await phase_login(args, report)  # B0（自带开窗与收尾）
    if args.recon or args.inject or args.send:
        browser = Chrome(options=common.options(headless=False))
        try:
            tab = await open_site(browser)
            report['recon'] = await phase_recon(tab, hover=args.hover_composer)  # B1
            if args.inject or args.send:
                report['inject'] = await phase_inject(tab, report['recon'], args)  # B2
            if args.send:
                report['send'] = await phase_send(  # B3（I18 闸门）
                    tab, report['recon'], report.get('inject') or {}, args)
        finally:
            info = await common.shutdown(browser, 'close_wait')
            report['shutdown'] = info
            print(f'[收尾] 退出：模式 {info.get("mode")} / 等 {info.get("waited_s")}s / '
                  f'退出码 {info.get("exit_code")} / 残留 {len(info.get("strays_after") or [])}')
    report['verdict'] = verdict_of(report)
    return report


async def selftest() -> int:
    """本地自证（**零副作用**）：用 `local_server` 的 `/drive` 页验证「扫描 JS + 分类 + 命中数」链路。"""
    from local_server import LocalServer  # 同目录一次性桩页

    # 自检用**独立临时 profile**：避免与真站点侦察/主程序共用一个 profile（单 profile 不能并发，
    # 并发时后启动者会 `Browser failed to start within timeout` —— 本探针实测踩到过）。
    temp_profile = pathlib.Path(tempfile.gettempdir()) / 'm7b28-selftest-profile'
    with LocalServer() as server:
        browser = Chrome(options=common.options(headless=True, profile=temp_profile))
        try:
            tab = await browser.start()
            await tab.go_to(f'http://127.0.0.1:{server.port}/page?mode=drive')
            await asyncio.sleep(0.8)
            proof = await common.assert_read_path(tab)
            data = await scan(tab)
            hits = js_json(await tab.execute_script(
                'return JSON.stringify({ "#file": document.querySelectorAll("#file").length });'))
            kind = classify(data)
            print(f'[自检] 读回自证 = {proof}')
            print(f'[自检] entry_kind = {kind}')
            print(f'[自检] file_inputs = {[f.get("selector") for f in (data.get("file_inputs") or [])]}')
            print(f'[自检] editors = {[e.get("selector") for e in (data.get("editors") or [])]}')
            print(f'[自检] 命中数验证 = {hits}')
            ok = (proof == 'probe-ok' and kind == 'file_input' and
                  bool(data.get('file_inputs')) and hits.get('#file') == 1)
            print(f'=== 自检判定：{"PASS" if ok else "FAIL"} ===')
            return 0 if ok else 1
        finally:
            info = await common.shutdown(browser, 'close_wait')
            print(f'[自检] 收尾：{info.get("mode")} / 残留 {len(info.get("strays_after") or [])}')


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='M7B-28 / P7b-05b：豆包（doubao-web）上传入口 + 视觉性质 侦察（一次性探针）')
    parser.add_argument('--selftest', action='store_true', help='本地桩页自证（零副作用）')
    parser.add_argument('--login', action='store_true', help='B0：开窗等你手动登录 + 重启复读')
    parser.add_argument('--recon', action='store_true', help='B1：只读侦察（默认动作）')
    parser.add_argument('--inject', action='store_true', help='B2：注入 1 张图（不发送）')
    parser.add_argument('--send', action='store_true', help='B3：发送一轮问答（需 B2 证据）')
    parser.add_argument('--hover-composer', action='store_true',
                        help='B1：先把鼠标移到 composer 上（只移动不点击）再扫描，用于触发附件按钮')
    parser.add_argument('--login-timeout', type=int, default=600, help='登录等待上限（秒）')
    parser.add_argument('--capture-s', type=float, default=6.0, help='注入后等网络回执的秒数')
    parser.add_argument('--answer-timeout', type=int, default=90, help='等待回答的上限（秒）')
    parser.add_argument('--inject-mode', choices=('auto', 'dom', 'chooser', 'datatransfer'),
                        default='auto', help='注入路线（auto = 按入口形态自动选）')
    parser.add_argument('--prompt', default='这张图里有什么？请描述你看到的内容（不要只念文字）。',
                        help='B3 发送的提示词（「纯图无字」判别用）')
    args = parser.parse_args(argv)
    if not (args.selftest or args.login or args.recon or args.inject or args.send):
        args.recon = True  # 默认：只读侦察
    return args


def main() -> int:
    args = parse_args(sys.argv[1:])
    if args.selftest:
        return asyncio.run(selftest())

    print(f'[运行] 站点={SITE_ID} 目标={SITE_URL}')
    print(f'[运行] 动作：login={args.login} recon={args.recon} inject={args.inject} '
          f'send={args.send}（注入路线={args.inject_mode}）')
    if args.login:
        print('[运行] ⚠️ B0 会打开可见窗口：请在其中**手动登录**；遇验证码请自行完成（探针不绕验证）')
    report = asyncio.run(run(args))
    path = common.save(EVIDENCE.name, report)

    print('=== 判定 ===')
    for key, value in (report.get('verdict') or {}).items():
        shown = '—（未跑）' if value is None else ('PASS' if value else 'FAIL')
        print(f'  {key:32} {shown}')
    print(f'[运行] 证据：{path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())


