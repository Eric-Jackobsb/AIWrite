// =============================================================================
//  dom_gate_test.js —— Loop Dev（`FEA-L01` / `FEA-L03`）「DOM 探针**需登录判据**」行为夹具
//
//  做什么：从源码 `source/ai/src/dom_web_client.cpp` **抽取真实的** `kProbeScript`
//          与输入区候选池常量 `kInputPoolSelector`，在 node 里用**迷你 DOM** 真跑该脚本，
//          断言 `docs/loop_dev.md` §3.2 的三条判据**逐条成立**，并且锁住
//          「**判据不看 Cookie**」（匿名临时会话通常也有 Cookie ⇒ 旧口径会误报「已登录」）。
//          不复制逻辑、不 mock 判据（源码一动，夹具立刻能看出行为变了）。
//
//  为什么需要它（§3.2 注 · 真机后果）：
//    · `web_session_state()` 的「已登录」判据是「该 origin 有 Cookie」—— 而匿名也能拿到
//      Cookie ⇒ 若拿它当「需登录」判据，会把**能用的站**记成「需登录」（白白等人工），
//      也会把**空白页**记成「已登录」（假成功）；本夹具的 ⑥ 就是这条反例；
//    · 行 3（候选 = 0 且无登录迹象 ⇒ **页面异常**，归失败）是最容易被写错的一条：
//      写成「⇒ 需登录」会让真失败被当成「等人」而**永远不被修复**（④ 锁死）；
//    · 行 1（`input_hits > 0` **或** 候选 > 0 ⇒ 可用）也不许被漏（⑤ 锁死声明值命中的分支）。
//
//  用法：node source\tools\dom_gate_test.js
//        node source\tools\dom_gate_test.js --old   （= DOM_GATE_OLD=1）
//          `--old`：把判据块换成**本批之前**的「只看 Cookie」写法
//                   ⇒ **⓪ ⓪b ④ ⑤ ⑥ ⑦ 应如实 FAIL**（**6 / 15**；①②③③b 与事实类
//                   ⑧⑨⑨b⑩⑩b **侥幸 PASS**，如实注明）—— 证明夹具能**捕获回归**
//                   （负向模式是故意注入回归，退出码非 0 属预期；注入**校验生效**，
//                   否则会「全绿而什么都没测到」）。
//
//  为什么放 `source/tools/`（**入库**）：同 `poll_answer_gate_test.js` —— `build/` 会被
//    重新配置清空，夹具是回归资产，必须与 `api_probe.cpp` 同级入库（`loop_dev` §9）。
//
//  依赖：仅 node 内置（无 npm 包）。不写任何文件、不联网、无副作用。
// =============================================================================
'use strict';

const fs = require('fs');
const path = require('path');

const CPP = path.join(__dirname, '..', 'ai', 'src', 'dom_web_client.cpp');
const SRC = fs.readFileSync(CPP, 'utf8');

// --------------------------------------------------------- 抽取（源码即真相）----
// C++ 字符串字面量 → 运行时值：相邻字面量拼接 + 转义（`\\` `\"` `\n` `\t` `\r`）
function decodeCppStringLit(block) {
  let out = '';
  const re = /"((?:[^"\\]|\\.)*)"/g;
  let m;
  while ((m = re.exec(block))) {
    const raw = m[1];
    let s = '';
    for (let i = 0; i < raw.length; i++) {
      const c = raw[i];
      if (c !== '\\') { s += c; continue; }
      const n = raw[++i];
      if (n === 'n') { s += '\n'; }
      else if (n === 't') { s += '\t'; }
      else if (n === 'r') { s += '\r'; }
      else { s += n; } // `\\` → `\` 、`\"` → `"` 、其余原样
    }
    out += s;
  }
  return out;
}
function extractConst(name) {
  const m = SRC.match(new RegExp('constexpr\\s+const\\s+char\\*\\s+' + name + '\\s*=([\\s\\S]*?);'));
  if (!m) { throw new Error('抽取失败：' + name); }
  return decodeCppStringLit(m[1]);
}
// `constexpr const char* kProbeScript = R"JS( … )JS";` ⇒ 脚本体
function extractRawScript(name) {
  const m = SRC.match(new RegExp('constexpr\\s+const\\s+char\\*\\s+' + name +
                                 '\\s*=\\s*([\\s\\S]*?)\\)JS";'));
  if (!m) { throw new Error('抽取失败：' + name); }
  return m[1].replace(/R"JS\(/g, '').replace(/\)JS"/g, '');
}
const POOL_SEL = extractConst('kInputPoolSelector');
let SCRIPT = extractRawScript('kProbeScript');

// `--old` 注入点：把 §3.2 的三行判据换成**旧口径（只看 Cookie）** ⇒ ⓪④⑤⑥⑦ 必须如实 FAIL
const OLD_MODE = process.argv.indexOf('--old') >= 0 || process.env.DOM_GATE_OLD === '1';
if (OLD_MODE) {
  if (SCRIPT.indexOf('out.input_candidates > 0') < 0 ||
      SCRIPT.indexOf('out.page_abnormal = true;') < 0) {
    throw new Error('--old 注入失败（§3.2 判据块字面量已变：源码里找不到候选判据 / 页面异常行）');
  }
  SCRIPT = SCRIPT.replace(
      /if \(out\.input_hits > 0 \|\| out\.input_candidates > 0\)[\s\S]*?else \{ out\.page_abnormal = true; \}/,
      "out.needs_login = !(document.cookie || '');");
  if (SCRIPT.indexOf('page_abnormal = true') >= 0) {
    throw new Error('--old 注入失败（替换后判据块仍在 ⇒ 正则没命中）');
  }
}

// ------------------------------------------------------------------ 迷你 DOM ----
// 支持的选择器：`*` / 标签 / `.class` / `#id` / `[attr]` / `[attr="v"]` / `[attr*="v"]`
// 与**逗号分组** —— 覆盖探针用到的全部口径（探针只查候选池 / 密码框 / 登录节点 / 声明值，
// 都是标签 + 类 + 属性 + 分组）。⚠️ 迷你 DOM **不支持** `:not()` 等伪类
// （`input:not([type])` 那条硬编码默认池**不参与**断言：夹具一律显式传 `input_pool`）。
function parseCompound(s) {
  const c = { tag: '', id: '', classes: [], attrs: [] };
  const re = /([a-zA-Z][\w-]*|\*)|#([\w-]+)|\.([\w-]+)|\[([^\]]+)\]/g;
  let m;
  while ((m = re.exec(s))) {
    if (m[1]) { c.tag = m[1].toLowerCase(); }
    else if (m[2]) { c.id = m[2]; }
    else if (m[3]) { c.classes.push(m[3]); }
    else if (m[4]) {
      const a = m[4].match(/^\s*([\w-]+)\s*(?:([*^$~]?=)\s*"?([^"\]]*)"?\s*)?$/);
      if (a) { c.attrs.push({ name: a[1], op: a[2] || '', val: a[3] || '' }); }
    }
  }
  return c;
}
const parseGroups = (sel) => sel.split(',').map((s) => s.trim()).filter((s) => s).map(parseCompound);
const attrOf = (el, name) => {
  if (name === 'class') { return el.className; }
  if (name === 'id') { return el.id; }
  return (el.attrs && el.attrs[name] !== undefined) ? String(el.attrs[name]) : null;
};
function matchCompound(el, c) {
  if (c.tag && c.tag !== '*' && el.tagName.toLowerCase() !== c.tag) { return false; }
  if (c.id && el.id !== c.id) { return false; }
  for (let i = 0; i < c.classes.length; i++) {
    if ((' ' + el.className + ' ').indexOf(' ' + c.classes[i] + ' ') < 0) { return false; }
  }
  for (let i = 0; i < c.attrs.length; i++) {
    const a = c.attrs[i];
    const v = attrOf(el, a.name);
    if (v === null) { return false; }
    if (a.op === '=' && v !== a.val) { return false; }
    if (a.op === '*=' && v.indexOf(a.val) < 0) { return false; }
  }
  return true;
}
const matchSel = (el, sel) => parseGroups(sel).some((c) => matchCompound(el, c));

// 按 spec 建树：`{ tag, cls, id, attrs, text, kids, rect }`
//   · `innerText` = 自身文本 + 子文本用 `\n` 拼接（与浏览器块级子元素同序）
//   · `rect: null` ⇒ `getBoundingClientRect()` 返回 **0×0**（用于 `input_visible` 用例）
//   · `spec.cookie` / `spec.title` / `spec.location` / `spec.children` 提供页面级事实
function build(spec) {
  const reg = [];
  function mk(s, parent) {
    const el = {
      tagName: (s.tag || 'DIV').toUpperCase(), className: s.cls || '', id: s.id || '',
      attrs: s.attrs || {}, children: [], __parent: parent || null, __own: s.text || '',
      textContent: '', innerText: '',
    };
    el.__rect = (s.rect === null)
        ? { width: 0, height: 0, top: 0, left: 0 }
        : Object.assign({ width: 120, height: 24, top: 0, left: 0 }, s.rect || {});
    el.getBoundingClientRect = function () { return this.__rect; };
    reg.push(el);
    (s.kids || []).forEach((k) => { el.children.push(mk(k, el)); });
    return el;
  }
  const root = mk(spec, null);
  const textOf = (el) => el.__own + el.children.map(textOf).join('\n');
  (function refresh(n) {
    n.children.forEach(refresh);
    n.textContent = textOf(n);
    n.innerText = n.textContent;
  })(root);
  reg.forEach((el) => {
    el.contains = function (o) {
      let p = o;
      while (p) { if (p === this) { return true; } p = p.__parent; }
      return false;
    };
  });
  const loc = spec.location || {};
  const doc = {
    body: root, title: spec.title || '', cookie: spec.cookie || '',
    querySelectorAll: (sel) => (sel === '*' ? reg.slice() : reg.filter((el) => matchSel(el, sel))),
    querySelector: (sel) => doc.querySelectorAll(sel)[0] || null,
    // 夹具内定位用（按 class 取节点）
    find: (cls) => reg.filter((el) => (' ' + el.className + ' ').indexOf(' ' + cls + ' ') >= 0)[0] || null,
    __reg: reg, __root: root,
    __location: { href: loc.href || 'https://example.com/', pathname: loc.pathname || '/',
                  search: loc.search || '', hash: loc.hash || '' },
  };
  return doc;
}

// ------------------------------------------------------------ 跑真实脚本一「次」----
function run(cfg, doc, extra) {
  const win = Object.assign({ __aiwriteDomProbe: cfg }, extra || {});
  // `SCRIPT` 以换行开头 ⇒ 先 trim（否则 `return\n(...)` 触发 ASI，返回值变 undefined）
  const fn = new Function('window', 'document', 'location',
                          'return ' + SCRIPT.trim() + '\n//# sourceURL=dom_probe.js');
  return fn(win, doc, doc.__location);
}
// 页面 + 配置的快照（只读性断言 ⑧ 用）
const snapshot = (doc) => JSON.stringify({
  title: doc.title, cookie: doc.cookie, loc: doc.__location,
  nodes: doc.__reg.map((el) => [el.tagName, el.className, el.id,
                                JSON.stringify(el.attrs), el.innerText.length]),
});
// 一次探测：`spec` = 页面事实；`cfg` = 探测配置（`input_pool` 默认取源码常量）；`extra` = 脚本可读的额外全局
function probe(spec, cfg, extra) {
  const doc = build(spec);
  const c = Object.assign({ input_pool: POOL_SEL }, cfg || {});
  const before = snapshot(doc);
  const cfg_before = JSON.stringify(c);
  const r = run(c, doc, extra);
  return { r, doc, before, after: snapshot(doc), cfg_before, cfg_after: JSON.stringify(c) };
}

// -------------------------------------------------------------------- 断言 ----
let pass = 0, fail = 0;
function check(name, ok) {
  if (ok) { pass++; console.log('  PASS ' + name); }
  else { fail++; console.log('  FAIL ' + name); }
}
const has = (s, sub) => String(s || '').indexOf(sub) >= 0;

console.log('[' + (OLD_MODE ? '旧口径 --old（判据 = 只看 Cookie）' : '当前源码') +
            '] 运行 Loop Dev 批 3 DOM 探针判据夹具 …');

// 复刻真机形态：`kimi-web` 的输入区 = `div.chat-input-editor[contenteditable="true"]`
//   （2026-10-09 真机实测：该选择器命中 1 且可见 ⇒ 匿名可用）
const KIMI_INPUT = { tag: 'DIV', cls: 'chat-input-editor', attrs: { contenteditable: 'true' } };
const EMPTY_BODY = { tag: 'BODY', kids: [] };

// ---- ⓪ §3.2 行 1：输入区候选 > 0 ⇒ **匿名可用**（不登录也能问）----
{
  const { r } = probe({ tag: 'BODY', kids: [KIMI_INPUT] }, {});
  check('⓪ 候选池命中（div.chat-input-editor[contenteditable]）⇒ 匿名可用：' +
        'needs_login=false / page_abnormal=false / 无 Login 证据',
        r.input_candidates === 1 && r.needs_login === false && r.page_abnormal === false &&
        r.login_evidence === '' && r.error === '');
}
{
  // 豆包形态：`textarea[placeholder="Ask Qwen"]`（qwen-web 的声明值；此处走候选池，不靠声明值也能中）
  const { r } = probe({ tag: 'BODY', kids: [
    { tag: 'TEXTAREA', cls: 'chat-input', attrs: { placeholder: 'Ask Qwen' } } ]}, {});
  check('⓪b textarea 命中候选池 ⇒ 同样匿名可用（候选池与运行期自动识别**同源**）',
        r.input_candidates === 1 && r.needs_login === false && r.page_abnormal === false);
}

// ---- ① §3.2 行 2：候选 = 0 **且** 有登录控件 ⇒ **需强制登录**（带证据）----
{
  const { r } = probe({ tag: 'BODY', kids: [{ tag: 'DIV', cls: 'login-panel' }] }, {});
  check('① 候选 0 + 登录节点（div.login-panel）⇒ needs_login=true + 证据 `login-node x1`' +
        ' + 退出码语义 3（等人，**不是**失败）',
        r.input_candidates === 0 && r.login_controls === 1 && r.needs_login === true &&
        r.page_abnormal === false && has(r.login_evidence, 'login-node x1'));
}
{
  const { r } = probe({ tag: 'BODY', kids: [
    { tag: 'INPUT', attrs: { type: 'password', name: 'pwd' } } ]}, {});
  check('② 候选 0 + 密码框 ⇒ needs_login=true + 证据 `password-input`',
        r.needs_login === true && r.page_abnormal === false &&
        has(r.login_evidence, 'password-input x1'));
}
{
  const { r } = probe(Object.assign({}, EMPTY_BODY, { location: { pathname: '/login' } }), {});
  check('③ 候选 0 且页面空白、但 URL 落在登录路径（/login）⇒ needs_login=true + 证据 `login-url:`',
        r.input_candidates === 0 && r.needs_login === true && r.page_abnormal === false &&
        has(r.login_evidence, 'login-url:/login'));
}
{
  // 登录路径也认 search / hash（`?next=/signin`）—— 只看 pathname 会漏判
  const { r } = probe(Object.assign({}, EMPTY_BODY, { location: { pathname: '/', search: '?next=/signin' } }), {});
  check('③b 登录路径同时看 search / hash（`?next=/signin`）⇒ 仍判需登录',
        r.needs_login === true && has(r.login_evidence, 'login-url:'));
}

// ---- ④ §3.2 行 3：候选 = 0 **但**无任何登录迹象 ⇒ **页面异常（归失败）** ----
//   最易写错的一条：写成「⇒ 需登录」会把**真失败**当成「等人」⇒ 永远不被修复（§5.1 ③）
{
  const { r } = probe({ tag: 'BODY', kids: [{ tag: 'DIV', cls: 'placeholder-skeleton' }] },
                      { location: { pathname: '/' } });
  check('④ 候选 0 + 无登录迹象 ⇒ page_abnormal=true（失败）· needs_login=false' +
        '（**不得**误归「需登录」，否则真失败会被当成等人）',
        r.input_candidates === 0 && r.login_controls === 0 && r.needs_login === false &&
        r.page_abnormal === true && r.login_evidence === '');
}

// ---- ⑤ §3.2 行 1 的**后半**：`input_hits > 0`（声明值命中）也算可用 ----
//   （候选池为 0 —— 例如站点把编辑器做成非 contenteditable 的自绘控件；
//     漏掉这个分支会把「声明值明明命中」的站误判成需登录 / 页面异常）
{
  const { r } = probe({ tag: 'BODY', kids: [{ tag: 'DIV', cls: 'declared-editor' }] },
                      { input_selector: 'div.declared-editor' });
  check('⑤ 声明值命中（input_hits=1）但候选池 0 ⇒ needs_login=false · page_abnormal=false',
        r.input_hits === 1 && r.input_candidates === 0 && r.needs_login === false &&
        r.page_abnormal === false);
}

// ---- ⑥ **判据不看 Cookie**（§3.2 注 · I15′ 反例）----
//   匿名临时会话也能拿到 Cookie ⇒ 「有 Cookie」**不得**被当成「已登录 / 可用」；
//   页面既无候选也无登录迹象时，唯一正确结论仍是**页面异常（失败）**。
{
  const { r } = probe({ tag: 'BODY', kids: [{ tag: 'DIV', cls: 'skeleton' }],
                        cookie: 'token=abc123; sessionid=xyz', title: 'T' },
                      { cookie_names: ['token'] });
  check('⑥ 有 Cookie（匿名临时会话）但页面无候选 / 无登录迹象 ⇒ 仍判**页面异常**；' +
        'cookies_present 只作事实、**不作判据**',
        r.cookies_present.length === 1 && r.needs_login === false && r.page_abnormal === true &&
        r.input_candidates === 0);
}

// ---- ⑦ 判据**优先级**：候选 / 声明值命中**优先于**登录迹象（行 1 在行 2 之前）----
//   登录页里常有可见账号输入框（`input[type="text"]` + 密码框 + 登录节点同时存在）——
//   此时判「可用」，由 S3 端到端（`--run-selftest`）兜底判「到底能不能问」。
{
  const { r } = probe({ tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'login-panel' },
    { tag: 'INPUT', cls: 'account', attrs: { type: 'text' } },
    { tag: 'INPUT', cls: 'pwd', attrs: { type: 'password' } } ]}, {});
  check('⑦ 候选 > 0 与登录迹象**并存** ⇒ 判可用（行 1 优先）；由 S3 端到端兜底',
        r.input_candidates === 1 && r.login_controls >= 1 && r.needs_login === false &&
        r.page_abnormal === false);
}

// ---- ⑧ **只读性**：探测脚本不写入页面、不改配置（只读 DOM 是`FEA-L01` 的硬约束）----
{
  const s = probe({ tag: 'BODY', kids: [KIMI_INPUT, { tag: 'DIV', cls: 'login-panel' }],
                    cookie: 'a=1', title: 'T', location: { pathname: '/x?y=1' } },
                  { input_selector: 'div.chat-input-editor', cookie_names: ['a'] });
  check('⑧ 跑完**页面逐字节未变**（DOM / Cookie / 标题 / URL）且**配置未被写入**（不落盘、不发送）',
        s.before === s.after && s.cfg_before === s.cfg_after);
}

// ---- ⑨ `token_expr` 只回传**形状**（长度 / 是否 JSON），不取内容（守 I15）----
{
  const { r } = probe(EMPTY_BODY, { token_expr: 'window.__tok' },
                      { __tok: 'x'.repeat(64) });
  const j = probe(EMPTY_BODY, { token_expr: 'window.__tok' }, { __tok: '{"a":1}' }).r;
  check('⑨ token 形状：长度回传（64）· 仅凭**首字符**判 JSON（`x…`=false / `{…}`=true）—— 不回传值本身',
        r.token_len === 64 && r.token_json === false && j.token_len === 7 && j.token_json === true);
}
{
  const { r } = probe(EMPTY_BODY, { token_expr: 'throw new Error("boom")' });
  check('⑨b token 表达式抛错 ⇒ token_len = **-2**（区分「空 = -1」，不吞成 0）',
        r.token_len === -2 && r.error === '');
}

// ---- ⑩ 声明值 / `done_when` / 可见性：只读事实如实回传 ----
const ANSWER_LAST = '春天的风把柳枝吹得很软。'; // 12 字（`answer_chars` = **末条** innerText 长度）
{
  const { r } = probe({ tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'answer', text: '第一段' },
    { tag: 'DIV', cls: 'answer', text: ANSWER_LAST },
    { tag: 'BUTTON', cls: 'send-btn' }, { tag: 'BUTTON', cls: 'send-btn' } ]},
    { answer_selector: 'div.answer', send_kind: 'click', send_value: 'button.send-btn',
      done_kind: 'selector_present', done_selector: 'div.answer' });
  check('⑩ 声明值：answer_hits=2 · answer_chars=' + ANSWER_LAST.length +
        '（**末条** innerText 长度）· send_hits=2（click）· done_now=true（selector_present）',
        r.answer_hits === 2 && r.answer_chars === ANSWER_LAST.length && r.send_hits === 2 &&
        r.done_now === true);
}
{
  const { r } = probe({ tag: 'BODY', kids: [{ tag: 'DIV', cls: 'hidden-box', rect: null }] },
                      { input_selector: 'div.hidden-box',
                        done_kind: 'selector_gone', done_selector: 'div.missing' });
  check('⑩b `input_visible` 是**可见性**（命中但 0×0 ⇒ false）· selector_gone 缺节点 ⇒ done_now=true',
        r.input_hits === 1 && r.input_visible === false && r.done_now === true);
}

console.log('Loop Dev 批 3 DOM 探针判据夹具：' + pass + ' / ' + fail +
            (OLD_MODE ? '（负向模式 --old：⓪⓪b④⑤⑥⑦ 失败即**预期**（6/15）—— 证明夹具能捕获回归）' : ''));
process.exit(fail === 0 ? 0 : 1);


