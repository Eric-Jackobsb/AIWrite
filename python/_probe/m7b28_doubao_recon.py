# -*- coding: utf-8 -*-
"""M7B-28 / P7b-05b 前置侦察 —— **豆包（doubao-web）网页版：上传入口 + 视觉性质**（一次性探针）。

对应计划与验收：
  * `M7.md` §12.1 **`P7b-05b`**：上传入口只读侦察 + 三项选择器命中 + `D-30` 证据 + 纯图无字判别
  * `M7B.md` §10「**网页版图片理解侦察记录**」表 +「登录 Cookie 存活口径」豆包行
  * `M_patchAB_rest.md` §9 **`D-30`**（登录前后 Cookie 名差集 → `cookie_names` 优先）
  * 决策 `M7.md` §9 `D10`：首个目标站 = `doubao-web`；`deepseek-web` 保持文字主线

四段（可单独跑；**默认只跑 B1 只读侦察**）：
  B0 `--login`   开有头窗口 → **你手动登录** → **DOM 正证据**判定（头像 / 昵称 / 用户菜单 / 退出登录，连续 2 次命中）→ Cookie 前后快照（**站点域过滤**）→ 优雅退出 → 重启复读
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
  * **换机兜底**：无 Chrome 时用 **Edge**（`common.browser_class()`，对应 `M7B-14` 的
    「Chrome 缺失 → Edge 兜底」）；实际用的浏览器 / Python / pydoll 版本写进 JSON 的 `env` 字段
    （**环境物证**，换机可追溯）
"""
import argparse
import asyncio
import base64
import json
import pathlib
import sys
import tempfile
import time

import m7b09_common as common  # 浏览器类由 common.browser_class() 选（无 Chrome → Edge，`M7B-14`）

SITE_ID = 'doubao-web'
SITE_URL = 'https://www.doubao.com/chat/'
# 站点 Cookie 域过滤：`D-30` / 登录 Cookie 存活口径**只认站点自己的域**。
# ⚠️ 2026-10-02 实测：`Storage.getCookies` 返回**整个 profile** 的 Cookie，Edge 首启会在
#    `msn.cn` / `ntp.msn.cn` 种下 10 条（`MUID` / `_EDGE_S` / `__rubyUX` / `USRLOC` …）⇒ 不过滤
#    会让差集变成「一堆与站点无关的新名」，直接把登录判定带偏（首次 B0 误报的帮凶之一）。
SITE_COOKIE_DOMAIN = '.doubao.com'
# 真实站点阶段的窗口尺寸：**默认视口只有 859×450**，豆包在这种尺寸下**附件工具条不渲染**
# （2026-10-02 实测：登录态 `input` 总数 0、无附件按钮、`attach_hints` 空 ⇒ 注入无路可走；
# 不是站点没有入口，而是**我们没给它画出来的地方**）⇒ 显式放大窗口再侦察。
SITE_WINDOW_ARGS = ('--window-size=1440,1000',)
# DOM 正证据需**连续 2 次**命中才判定登录成功（避免站点重渲染瞬间的抖动）
LOGIN_STABLE_POLLS = 2
# **鉴权 Cookie 实测白名单**（2026-10-02 23:59 从已登录 profile 只读诊断读出）。
# 为什么需要它：豆包在默认窗口（实测 859×450）下**没有可见的头像 / 昵称节点** —— 只读诊断
# `aw_login_diag.json` 里可见 `img` 仅 **1 个**（16×16 图标）、`avatarish` 只命中一个底栏 `div`
# ⇒ **纯 DOM 判据会漏检**（B0 复证实测：用户已登录，DOM 仍报「无」）。
# 而登录态实测多出整套**鉴权名**，且**匿名态（8 条 / B1 的 10 条）一个都不含** —— 见 §10 误报纠错记录。
AUTH_COOKIE_NAMES = frozenset({
    'sessionid', 'sessionid_ss', 'sid_guard', 'sid_tt', 'sid_ucp_v1', 'ssid_ucp_v1',
    'uid_tt', 'uid_tt_ss', 'session_tlb_tag', 'odin_tt', 'x-tt-multi-sids',
    'passport_auth_status', 'passport_auth_status_ss', 'passport_mfa_token'})
# 至少 **2 个不同**鉴权名才算命中（单个可能是残留 / 站点预置）
AUTH_COOKIE_MIN = 2
EVIDENCE = common.OUT / 'm7b28_doubao_recon.json'
FIXTURE_DIR = common.OUT / 'drive_files'
IMAGE_FIXTURE = FIXTURE_DIR / 'm7b28-doubao-256x256.png'
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


# ------------------------------------------- 登录态 DOM 正证据 JS（B0 判据）----
# ⚠️ 为什么要有这段（2026-10-02 实测纠正）：B0 原判据 = 「Cookie 名差集非空」+「输入框出现」，
#    结果在**无人操作**的 9s 内自报「登录成功」—— 差集被 **Edge 自家的 `msn.cn` Cookie** 触发，
#    而 `flow_cur_user_sec_id` / `flow_user_country` 在**匿名态**也会被站点种下；输入框更是未登录就渲染
#    （B1 已证）。⇒ 判据改为「**登录后才可能出现**的 DOM 正证据」：头像 / 昵称 / 用户菜单 / 退出登录。
#    只读：只枚举可见元素与属性，**不写值 / 不点击 / 不发请求**。
LOGIN_JS = r"""
  const out = { ok: false, url: location.href, title: document.title || '',
                login_entry: [], logout_entry: [], avatar: [], nickname: [],
                user_menu: [], candidates: [] };
  const visible = (el) => {
    try {
      const r = el.getBoundingClientRect();
      const s = getComputedStyle(el);
      return r.width > 1 && r.height > 1 && s.visibility !== 'hidden' && s.display !== 'none';
    } catch (e) { return false; }
  };
  const cls = (el) => {
    try {
      const c = el.className;
      if (c === undefined || c === null) { return ''; }
      if (typeof c === 'string') { return c; }
      return String(c.baseVal || '');
    } catch (e) { return ''; }
  };
  const tag = (el) => {
    const t = el.tagName ? el.tagName.toLowerCase() : '?';
    const c = cls(el).split(/\s+/).filter(Boolean).slice(0, 3).join('.');
    return c ? t + '.' + c : t;
  };
  try {
    // 1) 「登录 / 注册」入口 —— **只作诊断**（登录态下可能仍存在，故不参与判定）
    document.querySelectorAll('button,a,[role="button"],[class*="login"],[class*="Login"]')
      .forEach(el => {
        if (!visible(el)) { return; }
        const t = String(el.innerText || '').trim();
        if (t && t.length <= 8 && /登录|登陆|注册/.test(t) && !/退出/.test(t)) {
          out.login_entry.push({ tag: tag(el), text: t });
        }
      });
    // 2) 「退出登录」—— 登录后才出现的正证据（限定容器，避免全页 innerText 的代价）
    document.querySelectorAll('[class*="menu"],[class*="dropdown"],[class*="popover"],[class*="tooltip"],[class*="setting"]')
      .forEach(el => {
        if (!visible(el) || out.logout_entry.length > 8) { return; }
        const t = String(el.textContent || '').trim();
        if (t && t.length <= 60 && /退出登录|退出账号|注销/.test(t)) {
          out.logout_entry.push({ tag: tag(el), text: t.slice(0, 20) });
        }
      });
    // 3) 头像：可见 img（src 非空）且自身 / 近祖先的 class|alt|aria-label 命中关键字
    document.querySelectorAll('img').forEach(el => {
      if (!visible(el)) { return; }
      const src = String(el.getAttribute('src') || '');
      if (!src) { return; }
      let node = el, hit = '', depth = 0;
      while (node && depth < 4) {
        let bag = cls(node) + ' ';
        try {
          bag += String(node.getAttribute('alt') || '') + ' '
               + String(node.getAttribute('aria-label') || '');
        } catch (e) { }
        if (/avatar|portrait|head-?img|头像/i.test(bag)) { hit = bag.slice(0, 60); break; }
        node = node.parentElement; depth++;
      }
      if (hit) { out.avatar.push({ tag: tag(el), src_len: src.length, hit: hit }); }
    });
    // 4) 昵称：可见、文本 1~24 字、class 命中 nickname / user-name / userName
    document.querySelectorAll('[class*="nickname"],[class*="nickName"],[class*="user-name"],[class*="userName"]')
      .forEach(el => {
        if (!visible(el)) { return; }
        const t = String(el.innerText || '').trim();
        if (t && t.length <= 24) { out.nickname.push({ tag: tag(el), text: t }); }
      });
    // 5) 用户 / 账号菜单入口：aria-label 或 title 命中关键字
    document.querySelectorAll('[aria-label],[title]').forEach(el => {
      if (!visible(el)) { return; }
      const bag = (String(el.getAttribute('aria-label') || '') + ' '
                   + String(el.getAttribute('title') || '')).trim();
      if (/个人中心|账号|我的|profile|account/i.test(bag)) {
        out.user_menu.push({ tag: tag(el), text: bag.slice(0, 40) });
      }
    });
    // 6) 兜底候选（供人工核；**只落 class 名单，不落文本**，避免误采用户隐私）
    const seen = {};
    document.querySelectorAll('[class]').forEach(el => {
      if (!visible(el) || Object.keys(seen).length >= 40) { return; }
      const c = cls(el).toLowerCase();
      if (/user|account|profile|avatar|login/.test(c) && !seen[c]) { seen[c] = 1; }
    });
    out.candidates = Object.keys(seen);
    out.ok = true;
  } catch (e) { out.error = String((e && e.message) || e); }
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


async def cookie_snapshot(tab, domain_suffix: str | None = None) -> list[dict]:
    """Cookie 快照（`Storage.getCookies`，不带 urls）。

    ⚠️ `tab.get_cookies()` 是**当前页作用域**（停在 about:blank 必读空）—— `M7B-09` V-d 实测结论。
    ⚠️ `domain_suffix` 用来**只取站点自己的域**：不传 = 整个 profile（含 Edge 的 `msn.cn` 噪声）。
    """
    reply = await tab._execute_command(  # noqa: SLF001
        {'method': 'Storage.getCookies', 'params': {}})
    cookies = reply.get('result', {}).get('cookies', []) if isinstance(reply, dict) else []
    rows = common.norm(cookies)
    if domain_suffix:
        rows = [c for c in rows if str(c.get('domain') or '').lower().endswith(domain_suffix)]
    return rows


async def scan(tab) -> dict:
    return js_json(await tab.execute_script(SCAN_JS))


async def login_scan(tab) -> dict:
    """登录态 **DOM 正证据**扫描（只读，见 `LOGIN_JS`）。"""
    return js_json(await tab.execute_script(LOGIN_JS))


def site_cookies(rows) -> list[dict]:
    """只保留**站点自己域**的 Cookie（`SITE_COOKIE_DOMAIN` 结尾）—— `D-30` 的干净输入。"""
    return [c for c in (rows or [])
            if str(c.get('domain') or '').lower().endswith(SITE_COOKIE_DOMAIN)]


def dom_login_state(dom: dict) -> dict:
    """DOM 登录判据（**正证据**）：头像 / 昵称 / 用户菜单 / 退出登录 任一命中 ⇒ 已登录。

    为什么不能用「Cookie 差集非空」：2026-10-02 首次 B0 在**无人操作**的 9s 内自报登录成功 ——
    差集命中的是 `flow_cur_user_sec_id` / `flow_user_country`（**匿名态**也会被站点种下），
    且其中还混着 Edge 自家的 `msn.cn` Cookie。⇒ 只认「**登录后才可能出现**」的 DOM 信号；
    「登录 / 注册」入口只作诊断字段（登录态下它可能仍存在，故不参与判定）。
    """
    positive = {'avatar': len(dom.get('avatar') or []),
                'nickname': len(dom.get('nickname') or []),
                'user_menu': len(dom.get('user_menu') or []),
                'logout': len(dom.get('logout_entry') or [])}
    return {'logged_in': any(positive.values()),
            'hits': {key: value for key, value in positive.items() if value},
            'login_entry_count': len(dom.get('login_entry') or []),
            'scan_ok': bool(dom.get('ok'))}


def auth_cookie_hits(rows) -> list[str]:
    """站点域 Cookie 名 ∩ `AUTH_COOKIE_NAMES`（**实测白名单**，见常量注释）。"""
    return sorted({str(c.get('name')) for c in (rows or [])} & AUTH_COOKIE_NAMES)


def login_state(rows, dom: dict) -> dict:
    """B0 登录判据（**两腿并联**，任一成立即算已登录）：

    ① **DOM 正证据**（`dom_login_state`）—— 语义最硬，但**豆包默认视口下不可见**（会漏检）；
    ② **鉴权 Cookie 白名单**（`auth_cookie_hits` ≥ `AUTH_COOKIE_MIN`）—— 2026-10-02 实测：
       登录后多出 `sessionid` / `sid_guard` / `uid_tt` / `passport_auth_status` … 整套鉴权名，
       而**匿名态一个都不含** ⇒ 比原来「差集非空」紧得多（差集对着的是 Edge 自家 Cookie 噪声）。
    ⚠️ 白名单是**站点版本相关**的实测归纳（非规范）：站点改版若失效，先看诊断脚本转储再改名单。
    """
    dom_state = dom_login_state(dom)
    hits = auth_cookie_hits(rows)
    auth_ok = len(hits) >= AUTH_COOKIE_MIN
    return {'logged_in': bool(dom_state['logged_in'] or auth_ok),
            'dom': dom_state, 'auth_cookie_hits': hits, 'auth_cookie_ok': auth_ok}


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
    # ⚠️ 2026-10-02 纠错：登录后 composer 是 **`textarea`**（未登录才是 ProseMirror `div`）——
    # 原来只认 `contenteditable`，撞上 textarea 会误判成 `none`（B2 实测）。
    if any(item.get('contenteditable') or str(item.get('tag') or '').lower() == 'textarea'
           for item in (scan_data.get('editors') or [])):
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
    """纯图无字夹具（B3 判别「真视觉 vs OCR」用）：**256×256 纯红方块** PNG，不含任何文字。

    ⚠️ 2026-10-02 纠错：原先用 **1×1** 纯红 PNG，豆包答「**完全纯白色**……没有任何图案」——
    **颜色判错**，怀疑 1×1 是**退化输入**（图像管线 / 视觉编码器把它当空白）⇒ 换成 256×256
    明确色块（同为空/无字，但尺寸与色值都能被明确判别），文件名带尺寸以免复用旧夹具。
    """
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    if not IMAGE_FIXTURE.exists():
        IMAGE_FIXTURE.write_bytes(base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAACAElEQVR42u3TQQ0AAAgDsSmZf1GI4Y0G'
            'mlTBJZdp4a1IgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAA'
            'MAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAADKACBgADgAHAAGAAMAAY'
            'AAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAY'
            'AAwABgADgAHAAGAAMAAYAAwABgADgAEwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAG'
            'AAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADIAB'
            'VMAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOA'
            'AcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwAAYAA4ABwABgADAAGAAMAAYAA4ABwABg'
            'ADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABg'
            'ADAAGAAMANcCsfoQaa+PEUQAAAAASUVORK5CYII='))
    return IMAGE_FIXTURE


# ------------------------------------------------------------------- B0 登录 --
async def phase_login(args, report: dict) -> None:
    """B0：headful 开窗 → **用户手动登录** → **DOM 正证据**判定 → 快照（`D-30` 证据）→ 优雅退出 → 重启复读。

    ⚠️ 判据是 **DOM 正证据**（头像 / 昵称 / 用户菜单 / 退出登录，需连续 `LOGIN_STABLE_POLLS` 次命中），
    **不是**「Cookie 差集非空」—— 后者在 2026-10-02 首次 B0 造成**误报**（见 `dom_login_state` 注释）。
    Cookie 只作 `D-30` 证据，且**按站点域过滤**（`SITE_COOKIE_DOMAIN`）。
    """
    for attempt in (1, 2):
        label = '登录' if attempt == 1 else '重启复读'
        browser = common.browser_class()(
            options=common.options(headless=False, extra_args=SITE_WINDOW_ARGS))
        try:
            tab = await open_site(browser)
            proof = await common.assert_read_path(tab)  # 读回自证（否则一切读数不可信）
            print(f'[B0/{label}] 读回自证 = {proof or "（失败）"}')
            if attempt == 1:
                scan_before = await scan(tab)
                all_before = await cookie_snapshot(tab)
                rows_before = site_cookies(all_before)
                dom_before = await login_scan(tab)
                state_before = login_state(rows_before, dom_before)
                report['login'] = {
                    'readback_self_proof': proof,
                    'url': scan_before.get('url'), 'title': scan_before.get('title'),
                    'cookie_domain_filter': SITE_COOKIE_DOMAIN,
                    'cookie_rows_before': rows_before,
                    'cookie_names_before': names_of(rows_before),
                    'cookie_rows_all_before': all_before,
                    'cookie_all_names_before': names_of(all_before),
                    'dom_before': dom_before, 'dom_state_before': state_before,
                    'auth_cookie_hits_before': state_before['auth_cookie_hits'],
                    'already_logged_in_at_start': state_before['logged_in'],
                    'composer_before': len(scan_before.get('editors') or []),
                    'login_timeout_s': args.login_timeout,
                    'criterion': ('两腿并联（任一成立）—— ① DOM 正证据 avatar/nickname/user_menu/logout；'
                                  f'② 鉴权 Cookie 白名单 ≥ {AUTH_COOKIE_MIN} 个；均需连续 '
                                  f'{LOGIN_STABLE_POLLS} 次命中'),
                }
                print(f'[B0/登录] 登录前：站点 Cookie {len(rows_before)} 条 '
                      f'{names_of(rows_before)}（全库 {len(all_before)} 条）；'
                      f'输入框候选 {report["login"]["composer_before"]} 个；'
                      f'DOM 正证据 {state_before["dom"]["hits"] or "无"} / '
                      f'鉴权 Cookie {state_before["auth_cookie_hits"]} / '
                      f'登录入口 {state_before["dom"]["login_entry_count"]} 个 → '
                      f'起始已登录 = {state_before["logged_in"]}')
                print('[B0/登录] 请在窗口里手动登录（不代填密码、不绕验证码）……')
                started = time.monotonic()
                logged = False
                stable = 0
                last: dict = {}
                while time.monotonic() - started < args.login_timeout:
                    await asyncio.sleep(3.0)
                    dom = await login_scan(tab)
                    rows = site_cookies(await cookie_snapshot(tab))
                    state = login_state(rows, dom)
                    stable = stable + 1 if state['logged_in'] else 0
                    last = {'dom': dom, 'state': state}
                    fresh = sorted(set(names_of(rows)) - set(names_of(rows_before)))
                    now = await scan(tab)
                    print(f'[B0/登录] {int(time.monotonic() - started):4d}s：DOM 正证据 '
                          f'{state["dom"]["hits"] or "无"} / 鉴权 Cookie '
                          f'{len(state["auth_cookie_hits"])} 个{state["auth_cookie_hits"][:3]}'
                          f'（连续命中 {stable}）/ 登录入口 {state["dom"]["login_entry_count"]} 个 / '
                          f'站点 Cookie {len(rows)} 条（新增 {len(fresh)}）/ '
                          f'输入框 {len(now.get("editors") or [])} 个')
                    if stable >= LOGIN_STABLE_POLLS:  # 正证据连续命中 → 判定已登录
                        logged = True
                        report['login'].update({
                            'dom_after': dom, 'dom_state_after': state,
                            'cookie_rows_after': rows, 'cookie_names_after': names_of(rows),
                            'new_cookie_names': fresh,
                            'composer_after': len(now.get('editors') or []),
                            'login_wait_s': int(time.monotonic() - started)})
                        print(f'[B0/登录] ✅ 登录判据连续 {stable} 次命中（DOM '
                              f'{state["dom"]["hits"] or "无"} / 鉴权 Cookie '
                              f'{state["auth_cookie_hits"]}）→ 判定已登录（等 '
                              f'{int(time.monotonic() - started)}s）')
                        break
                report['login']['logged_in_detected'] = logged
                if not logged:
                    report['login']['dom_last'] = last
                    rows_tail = site_cookies(await cookie_snapshot(tab))
                    report['login'].update({
                        'cookie_rows_after': rows_tail,
                        'cookie_names_after': names_of(rows_tail),
                        'new_cookie_names': sorted(set(names_of(rows_tail))
                                                   - set(names_of(rows_before)))})
                    report['login']['note'] = (
                        f'{args.login_timeout}s 内 DOM 未出现登录正证据（头像 / 昵称 / 用户菜单 / '
                        '退出登录）⇒ 判定为**未登录**（**不**采信 Cookie 差集）')
                    print(f'[B0/登录] ⚠️ {report["login"]["note"]}')
                    print(f'[B0/登录] （留证）此时站点 Cookie 新增 '
                          f'{report["login"]["new_cookie_names"]} —— 与登录无关，不计入判定')
            else:
                all_rows = await cookie_snapshot(tab)
                rows = site_cookies(all_rows)
                dom = await login_scan(tab)
                state = login_state(rows, dom)
                now = await scan(tab)
                fresh = set(report.get('login', {}).get('new_cookie_names') or [])
                still = sorted(fresh & set(names_of(rows)))
                report['relaunch'] = {
                    'readback_self_proof': proof, 'cookie_domain_filter': SITE_COOKIE_DOMAIN,
                    'cookie_count': len(rows), 'cookie_names': names_of(rows),
                    'cookie_count_all': len(all_rows), 'new_names_still_present': still,
                    'composer_count': len(now.get('editors') or []),
                    'dom': dom, 'dom_state': state['dom'],
                    'auth_cookie_hits': state['auth_cookie_hits'],
                    'still_logged_in': bool(state['logged_in']),
                    'storage_keys': (now.get('storage_keys') or [])[:20],
                }
                print(f'[B0/重启复读] DOM 正证据 {state["dom"]["hits"] or "无"} / 鉴权 Cookie '
                      f'{len(state["auth_cookie_hits"])} 个 / '
                      f'站点 Cookie {len(rows)} 条（全库 {len(all_rows)} / 新增名仍在 {still}）/ '
                      f'输入框 {report["relaunch"]["composer_count"]} 个 → '
                      f'仍登录 = {report["relaunch"]["still_logged_in"]}')
        finally:
            info = await common.shutdown(browser, 'close_wait')  # 优雅退出（保登录态）
            report['shutdown_login' if attempt == 1 else 'shutdown_relaunch'] = info
            print(f'[B0/{label}] 退出：模式 {info.get("mode")} / 等 {info.get("waited_s")}s / '
                  f'退出码 {info.get("exit_code")} / 残留 {len(info.get("strays_after") or [])}')

    # Cookie 存活口径（`M7B.md` §10 表用，**只算站点域**）+ `D-30` 差集（喂「cookie_names 优先」判据）
    login = report.get('login') or {}
    report['cookie_ttl'] = [
        {'name': c.get('name'), 'session': c.get('session'), 'expires': c.get('expires'),
         'http_only': c.get('http_only'), 'secure': c.get('secure'), 'domain': c.get('domain')}
        for c in (login.get('cookie_rows_after') or [])
    ]
    report['d30_cookie_names_diff'] = {
        'domain_filter': SITE_COOKIE_DOMAIN,
        'before': login.get('cookie_names_before') or [],
        'after': login.get('cookie_names_after') or [],
        'new': login.get('new_cookie_names') or [],
        'note': ('登录后新增的**站点域** Cookie 名 = 候选 cookie_names（D-30 的豆包证据）；'
                 '已按域过滤，Edge 自家的 `msn.cn` / `ntp.msn.cn` **不计入**；'
                 '⚠️ 差集非空**不等于**已登录（匿名态也会种 `flow_cur_user_sec_id` 等）'),
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
                # 先**聚焦** composer（只调 `focus()`：不点击、不写值）—— 部分站点要 focus 才渲染附件工具条
                await tab.execute_script(
                    'const el = document.querySelector(' + json.dumps(editor_pre) + ');'
                    'if (el && el.focus) { el.focus(); } return "focus-ok";')
                await asyncio.sleep(0.3)
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
    urls_heuristic = []          # 只按 URL 子串命中的**候选**（含静态资源，仅供人工核）
    for entry in new_logs:
        request = (entry.get('params') or {}).get('request') or {}
        url = str(request.get('url') or '')
        if any(hint in url.lower() for hint in UPLOAD_HINTS):
            row = {
                'url': url[:200], 'method': request.get('method') or '',
                'has_post_data': bool(request.get('postData')),
                'resource_type': (entry.get('params') or {}).get('type')}
            urls_heuristic.append(row)
            # ⚠️ 2026-10-02 纠错：原来只按 URL 子串就记成「上传请求」⇒ 把 `…attachment-action-host.css`
            # / 图标 `Image` 之类的 **GET 静态资源**当成上传回执（B2 实测假阳性）。
            # 现在**必须**是 POST 或带 postData 才算「真上传回执」。
            if row['method'].upper() == 'POST' or row['has_post_data']:
                uploads.append(row)
    after = await attach_probe(tab, image.name)
    if target_file:
        injected_files = common.js_text(await tab.execute_script(
            'return Array.from((document.querySelector(' + json.dumps(target_file) +
            ') || {files: []}).files).map(f => f.name).join(",");'))
    page_ok = (bool(after.get('name_hit')) or bool(injected_files) or
               (int(after.get('attach_nodes') or 0) - int(before.get('attach_nodes') or 0)) > 0)
    return {'entry_kind': kind, 'mode': mode, 'image': str(image), 'image_bytes': size,
            'steps': steps, 'network_new_total': len(new_logs), 'upload_requests': uploads,
            'upload_urls_heuristic': urls_heuristic[:20],
            'upload_evidence_note': ('`upload_requests` = **POST / 带 postData** 的真上传回执；'
                                     '`upload_urls_heuristic` = 仅 URL 子串命中的候选（含静态资源）'),
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
    # ⚠️ 2026-10-02 实测：**发送按钮常在输入框有内容之后才渲染**（侦察那一刻的 DOM 里没有）
    # ⇒ 输入完**再扫一次**，命中就点击，否则退到 Enter。
    if not send_target:
        await asyncio.sleep(0.8)
        after = await scan(tab)
        send_target = pick_send_target({'attach_hints': after.get('attach_hints') or []})
        out['send_target_after_typing'] = send_target
    if send_target:
        await tab.execute_script('document.querySelector(' + json.dumps(send_target) + ').click();',
                                 user_gesture=True)
    else:
        out['enter_press'] = await common.press_key(tab, 'Enter')  # 兜底：Enter 发送
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
        'B0_login_detected': login.get('logged_in_detected'),  # 判据 = DOM 正证据 或 鉴权 Cookie 白名单
        'B0_login_dom_positive': bool(((login.get('dom_state_after') or {}).get('dom') or {}).get('hits')),
        'B0_login_auth_cookie_ok': bool((login.get('dom_state_after') or {}).get('auth_cookie_ok')),
        'B0_login_entry_present': bool(((login.get('dom_state_after') or {}).get('dom')
                                        or {}).get('login_entry_count')),
        'B0_d30_diff_nonempty': bool((report.get('d30_cookie_names_diff') or {}).get('new')),
        'B0_relaunch_still_logged_in': (report.get('relaunch') or {}).get('still_logged_in'),
        'B2_injected_ok': inject.get('injected_ok'),
        'B2_upload_request_seen': bool(inject.get('upload_requests')) if inject else None,
        'B3_sent': send.get('sent') if send else None,
        'env_browser_kind': (report.get('env') or {}).get('browser_kind') in ('chrome', 'edge'),
    }


# ------------------------------------------------------------------- 主流程 --
async def run(args) -> dict:
    report: dict = {'site': SITE_ID, 'site_url': SITE_URL, 'argv': sys.argv[1:],
                    'env': common.env_proof(),  # 环境物证（换机可追溯）
                    'started': time.strftime('%Y-%m-%d %H:%M:%S')}
    print(f'[环境] {report["env"]}')
    if args.login:
        await phase_login(args, report)  # B0（自带开窗与收尾）
    if args.recon or args.inject or args.send:
        browser = common.browser_class()(
            options=common.options(headless=False, extra_args=SITE_WINDOW_ARGS))
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
    print(f'[自检] 环境：{common.env_proof()}')
    with LocalServer() as server:
        browser = common.browser_class()(options=common.options(headless=True, profile=temp_profile))
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


