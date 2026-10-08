// =============================================================================
//  send_chain_test.js —— PW-07f「真实发送链路」行为夹具（prepare → CDP 输入 → 回执 → 兜底点击）
//
//  做什么：从源码 `source/src/ai/dom_web_client.cpp` **抽取真实的**三个页面脚本
//          （`kPrepareScript` / `kSendVerifyScript` / `kClickSendScript`）+ 两个真判据常量
//          （`kAnswerPoolSelector` / `kInputPoolSelector`），在 node 里用**迷你 DOM** 真跑，
//          断言：
//            ① 准备阶段**只聚焦 / 清残留 / 记基线，不写入**（写入只能由 CDP 真实输入完成）；
//            ② 回执阶段**如实区分「发出去了」与「被静默忽略」**（URL 变化 / 新消息节点 / 输入框残留）；
//            ③ 兜底点击**只点可见且像「发送」的按钮**，找不到时如实报「未找到」。
//
//  为什么需要它（真机证据 · 2026-10-08）：
//    · 旧路径（`kKickoffScript`）= 脚本自己写 DOM（`textContent` / `execCommand`）+ 自己派发
//      **不可信** KeyboardEvent ⇒ 豆包（ProseMirror/tiptap）**忽略**合成输入 ⇒ 输入框被清空、
//      页面 60 秒一条消息都不渲染、URL 仍停在 `https://www.doubao.com/chat/`（16:37 / 16:50 /
//      16:56 连续三次 `候选池 0`）—— 而旧代码**写完就当成功** ⇒ 白等 60 秒才报「未取到答案」，
//      日志里看不出「根本没发出去」。
//    · 本批改为 CDP 真实输入（`Input.insertText` / `dispatchKeyEvent`，`isTrusted=true`）
//      ⇒ 必须先用 ① 把编辑区**聚焦**并**清掉上一次的残留**（否则会重复发送半截文本）；
//      再用 ② **验证**确实发出（否则如实报「发送未生效」，不再假装成功）。
//
//  用法：node source\tools\send_chain_test.js
//        node source\tools\send_chain_test.js --old   （= SEND_CHAIN_OLD=1）
//        node source\tools\send_chain_test.js --debug （= SEND_CHAIN_DEBUG=1）逐个脚本转储返回值
//          `--old`：注入**本批之前**的两处写法 ⇒ 相关断言应如实 FAIL —— 证明夹具能**捕获回归**
//                  （负向模式是故意注入回归，退出码非 0 属预期）：
//                    ① 准备脚本去掉「仅在确实聚焦时才清空」守卫 ⇒ **⑥** FAIL（未聚焦也会清掉整页）
//                    ② 回执写成**恒为「已发送」**（= 旧代码「写完就当成功」的口径）
//                       ⇒ **⑦ / ⑧ / ⑪** FAIL（静默忽略 / 提示词仍在输入框 / 新容器包着输入区，
//                          全都会被打成「已发送」⇒ 白等 60 秒）
//
//  为什么放 `source/tools/`（**入库**）：与 `poll_answer_gate_test.js` 同源理由 ——
//    `build/` 会被 `cmake` 重新配置覆盖，夹具是回归资产，必须与源码同级入库。
//
//  依赖：仅 node 内置（无 npm 包）。不写任何文件、不联网、无副作用。
// =============================================================================
'use strict';

const fs = require('fs');
const path = require('path');

const CPP = path.join(__dirname, '..', 'src', 'ai', 'dom_web_client.cpp');
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
// `constexpr const char* kXxxScript = R"JS( … )JS";` ⇒ 脚本体
function extractRaw(name) {
  const m = SRC.match(new RegExp('constexpr\\s+const\\s+char\\*\\s+' + name + '\\s*=\\s*([\\s\\S]*?)\\)JS";'));
  if (!m) { throw new Error('抽取失败：' + name); }
  return m[1].replace(/R"JS\(/g, '').replace(/\)JS"/g, '');
}

const POOL_SEL = extractConst('kAnswerPoolSelector');
const INPUT_POOL_SEL = extractConst('kInputPoolSelector');
const MIN_CHARS_M = SRC.match(/constexpr\s+int\s+kMinAutoAnswerChars\s*=\s*(\d+)/);
if (!MIN_CHARS_M) { throw new Error('抽取失败：kMinAutoAnswerChars'); }
const MIN_CHARS = parseInt(MIN_CHARS_M[1], 10);

let PREPARE = extractRaw('kPrepareScript');
let VERIFY = extractRaw('kSendVerifyScript');
const CLICK = extractRaw('kClickSendScript');

const OLD_MODE = process.argv.indexOf('--old') >= 0 || process.env.SEND_CHAIN_OLD === '1';
if (OLD_MODE) {
  // 注入 ①：去掉「仅在确实聚焦时才清空」守卫（守卫在 = 未聚焦时**不**动页面）
  const GUARD_LINE = 'if (out.focused) {';
  if (PREPARE.indexOf(GUARD_LINE) < 0) {
    throw new Error('--old 注入①失败（源码字面量已变）：' + GUARD_LINE);
  }
  PREPARE = PREPARE.replace(GUARD_LINE, 'if (true) {');
  // 注入 ②：回执**恒判「已发送」**（= 本批之前「写完就当成功」的口径 ⇒ 静默忽略也报成功）
  const SENT_LINE = 'out.sent = out.url_changed || out.new_nodes > 0;';
  if (VERIFY.indexOf(SENT_LINE) < 0) {
    throw new Error('--old 注入②失败（源码字面量已变）：' + SENT_LINE);
  }
  VERIFY = VERIFY.replace(SENT_LINE, 'out.sent = true;');
}

// 调试转储（`--debug` / `SEND_CHAIN_DEBUG=1`）：断言挂了时一眼看出判据返回值，不必改夹具再加打印
const DBG = process.env.SEND_CHAIN_DEBUG === '1' || process.argv.indexOf('--debug') >= 0;

// ---- 抽取保真度：抽错常量 / 抽到别的脚本 ⇒ 立即大声失败，不静默跑空 ----
function assertExtracted(name, body, needles) {
  if (!body || body.length < 300) { throw new Error('抽取内容过短（可疑）：' + name); }
  needles.forEach(function (n) {
    if (body.indexOf(n) < 0) { throw new Error('抽取内容缺关键片段：' + name + ' 缺 ' + n); }
  });
}
assertExtracted('kPrepareScript', PREPARE, ['__aiwritePreSend', 'url_before', 'input_auto', 'focus']);
assertExtracted('kSendVerifyScript', VERIFY, ['url_changed', 'new_nodes', 'composer_len', 'composer_has_prompt']);
assertExtracted('kClickSendScript', CLICK, ['looks_send', 'clicked', 'candidates']);


// ------------------------------------------------------------------ 迷你 DOM ----
// 与 `poll_answer_gate_test.js` **同构**的选择器口径（`*` / 标签 / `.class` / `#id` /
// `[attr]` / `[attr="v"]` / `[attr*="v"]` + 逗号分组）—— 覆盖发送链路用到的全部写法。
// 本夹具**额外**建模了真实浏览器语义：`focus` / `activeElement` / `getBoundingClientRect` /
// `click` / `execCommand(selectAll|delete)`（后者是「未聚焦时清空会**清掉整页**」这一
// 破坏性语义的显式建模，见用例 ⑥）。
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
const parseGroups = (sel) => String(sel || '').split(',').map((s) => s.trim()).filter((s) => s).map(parseCompound);
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

// 按 spec 建树：`{ tag, cls, id, attrs, text, value, disabled, readOnly, top, rect, kids, focus_fails }`
//  * `innerText` = 自身文本 + 子文本用 `\n` 拼接后自底向上刷新（与浏览器「块级子元素各占一行」同序）
//  * **同一棵树上后续追加**（`doc.append`）= 真机 SPA 形态：DOM 节点**复用** ⇒ `kPrepareScript`
//    打下的 `__aiwritePreSend` 标与 `url_before` 都还在（这正是「回执」判据的锚点）
function build(spec) {
  const reg = [];
  let doc = null;
  const refreshAll = () => {
    const textOf = (n) => n.__own + n.children.map(textOf).join('\n');
    (function refresh(n) {
      n.children.forEach(refresh);
      n.textContent = textOf(n);
      n.innerText = n.textContent;
    })(doc.__root);
  };
  function mk(s, parent) {
    const el = {
      tagName: (s.tag || 'DIV').toUpperCase(), className: s.cls || '', id: s.id || '',
      attrs: Object.assign({}, s.attrs || {}), children: [], __parent: parent || null,
      __own: s.text || '', textContent: '', innerText: '',
      value: (s.value === undefined ? '' : s.value),
      disabled: s.disabled === true, readOnly: s.readOnly === true,
      __clicked: 0, __focused: false,
      __rect: Object.assign({ width: 200, height: 40, top: (s.top === undefined ? 10 : s.top), left: 0 },
                            s.rect || {}),
    };
    el.isContentEditable = (el.attrs.contenteditable === 'true');
    el.getAttribute = function (n) {
      if (n === 'class') { return el.className; }
      if (n === 'id') { return el.id ? el.id : null; }
      return (el.attrs[n] === undefined) ? null : String(el.attrs[n]);
    };
    el.getBoundingClientRect = function () { return el.__rect; };
    el.focus = function () {
      if (s.focus_fails === true) { return; } // 建模「站点不接受聚焦 / 被别的元素抢焦点」
      el.__focused = true;
      doc.activeElement = el;
    };
    el.click = function () { el.__clicked += 1; };
    // 与浏览器同语义：`a.contains(b)` = b 是 a 自身或 a 的后代。
    // **必须在 `mk` 里定义**（而不是建树结束后统一补）—— 真机是 SPA，验证/点击阶段会出现
    // `doc.append` 出来的**新节点**，它们同样要能参与 `contains(输入区)` 判断（用例 ⑨/⑪）。
    el.contains = function (o) {
      let p = o;
      while (p) { if (p === el) { return true; } p = p.__parent; }
      return false;
    };
    reg.push(el);
    (s.kids || []).forEach((k) => { el.children.push(mk(k, el)); });
    return el;
  }
  const root = mk(spec, null);
  const execCommand = function (cmd) {
    if (cmd === 'selectAll') { doc.__sel = doc.activeElement; return true; }
    if (cmd === 'delete') {
      const sel = doc.__sel;
      if (!sel) { return false; }
      if (sel === doc.activeElement && sel !== doc.body) {
        sel.__own = ''; sel.children.length = 0; refreshAll();
      } else {
        // 未聚焦时 `selectAll` 选的是**整页** ⇒ `delete` 会把页面上所有内容清掉（破坏性）
        doc.body_wiped = true;
        root.__own = ''; root.children.length = 0; refreshAll();
      }
      return true;
    }
    return false;
  };
  doc = {
    body: root, activeElement: root, body_wiped: false, __sel: null,
    querySelectorAll: (sel) => {
      if (!sel) { return []; }
      if (sel === '*') { return reg.slice(); }
      return reg.filter((el) => matchSel(el, sel));
    },
    find: (cls) => reg.filter((el) => (' ' + el.className + ' ').indexOf(' ' + cls + ' ') >= 0)[0] || null,
    append: (parent, s) => { const el = mk(s, parent); parent.children.push(el); refreshAll(); return el; },
    // 搬迁子节点（真机：站点换壳 / 重挂编辑区 —— 用例 ⑩ 用它造「新池容器**包着**输入区」）
    move: (child, parent) => {
      const p = child.__parent;
      if (p) { const i = p.children.indexOf(child); if (i >= 0) { p.children.splice(i, 1); } }
      child.__parent = parent;
      parent.children.push(child);
      refreshAll();
      return child;
    },
    execCommand: execCommand,
    __reg: reg, __root: root,
  };
  doc.querySelector = (sel) => doc.querySelectorAll(sel)[0] || null;
  refreshAll();
  return doc;
}

// --------------------------------------------------------------- 跑真实脚本 ----
const PROMPT = '把这张图里的石碑拓下来，用一句话说明它写了什么。';
const URL0 = 'https://www.doubao.com/chat/';
const URL1 = 'https://www.doubao.com/chat/7593170531'; // 豆包首条消息后 SPA pushState 的形态

function baseCfg(o) {
  return Object.assign({
    input_selector: '', input_pool: INPUT_POOL_SEL, answer_pool: POOL_SEL,
    answer_selector: '', answer_min_chars: MIN_CHARS, answer_chrome_text: '', prompt: PROMPT,
    auto_input: true, auto_answer: true, send_kind: 'key', send_value: 'Enter',
    done_kind: '', done_selector: '',
  }, o || {});
}
// 与 C++ 侧同构：先跑 `kPrepareScript`（1 次），再由 CDP 写文本 / 回车（本夹具不模拟 CDP 本身，
// 而是**直接建模站点对它的反应** —— 那才是回执判据要看清的东西），最后跑 `kSendVerifyScript`
function runScript(script, cfg, rt, doc, url) {
  const win = { __aiwriteDom: cfg, __aiwriteDomRuntime: rt, innerHeight: 800 };
  const loc = { href: url || URL0 };
  // 脚本以换行开头 ⇒ 先 trim（否则 `return\n(...)` 触发 ASI，返回值变 undefined）
  const fn = new Function('window', 'document', 'location',
                          'return ' + String(script).trim() + '\n//# sourceURL=send_chain.js');
  const out = { r: fn(win, doc, loc), win: win, loc: loc };
  if (DBG) { console.log('  [debug] 脚本返回 ' + JSON.stringify(out.r)); } // `--debug`
  return out;
}
function sendChain(preSpec, cfg0, react, url_after) {
  const doc = build(preSpec);
  const cfg = baseCfg(cfg0);
  const p = runScript(PREPARE, cfg, {}, doc, URL0);
  const rt = p.win.__aiwriteDomRuntime || null;
  if (typeof react === 'function') { react(doc); }
  const v = runScript(VERIFY, cfg, rt || {}, doc, url_after || URL0);
  return { p: p.r, v: v.r, doc: doc, rt: rt, cfg: cfg };
}
const setComposer = (doc, text) => { doc.find('chat-input-editor').innerText = text; };

// 豆包真机形态（与 `poll_answer_gate_test.js` 的 PRE_DOUBAO 同族）：
//  · `div.bp5-overflow-list message-list`（池内）+ `div.message-tip`（池内，整串 = 「今天 13:31」）
//  · 输入区 `div.chat-input-editor`（contenteditable，**在下半屏** ⇒ 自动识别应选它）
//  · 发送按钮 `button.send-btn`
const DOUBAO_PRE = () => ({
  tag: 'BODY',
  kids: [
    { tag: 'DIV', cls: 'bp5-overflow-list message-list', kids: [
      { tag: 'DIV', cls: 'message-tip', kids: [
        { tag: 'SPAN', text: '今天' }, { tag: 'SPAN', text: '13:31' }] },
    ]},
    { tag: 'DIV', cls: 'chat-input-editor', attrs: { contenteditable: 'true' }, top: 700 },
    { tag: 'BUTTON', cls: 'send-btn', attrs: { type: 'button' }, top: 720 },
  ],
});

// -------------------------------------------------------------------- 断言 ----
let pass = 0, fail = 0;
function check(name, ok) {
  if (ok) { pass++; console.log('  PASS ' + name); }
  else { fail++; console.log('  FAIL ' + name); }
}
const has = (s, sub) => String(s || '').indexOf(sub) >= 0;
const marked = (doc) => doc.__reg.filter((el) => el.__aiwritePreSend === true).length;

console.log('[' + (OLD_MODE ? '旧口径 --old（注入：无聚焦守卫 + 回执恒判已发送）' : '当前源码') +
            '] 运行 PW-07f 发送链路夹具 …');

// ---- 用例（⓪ … ⑦）：准备阶段 + 回执（如实判定）----

// ---- ⓪ 准备阶段**不写入**：提示词只能由 CDP 真实输入落进页面（脚本自己写 = 不可信 ⇒ 被站点忽略）----
{
  const { p, doc } = sendChain(DOUBAO_PRE(), {}, null);
  check('⓪ 准备阶段**不写入**：脚本跑完后整页文本里**没有**提示词（写入只归 CDP 真实输入）',
        p.ok === true && doc.__root.innerText.indexOf(PROMPT) < 0);
}

// ---- ① 声明值命中：聚焦 + 命中数 + 指纹（CDP 输入要求**编辑区拿焦点**）----
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'TEXTAREA', cls: 'chat-input', top: 700 },
    { tag: 'DIV', cls: 'composer-wrap' },
  ]};
  const { p } = sendChain(pre, { input_selector: 'textarea.chat-input' }, null);
  check('① 声明输入框命中 ⇒ ok / input_auto=false / input_hits=1 / input_selector=声明值 / **focus 已拿到**',
        p.ok === true && p.input_auto === false && p.input_hits === 1 &&
        p.input_selector === 'textarea.chat-input' && p.focused === true && p.contenteditable === false);
}

// ---- ② 清残留：上一次失败尝试留下的半截文本必须清掉（否则会**重复发送**半截）----
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'TEXTAREA', cls: 'chat-input', top: 700, value: '上一次的半截提示词' },
  ]};
  const { p, doc } = sendChain(pre, { input_selector: 'textarea.chat-input' }, null);
  check('② 输入框残留被清空（cleared=true / value 为空）且**整页没被动**（未清整页）',
        p.ok === true && p.cleared === true && doc.find('chat-input').value === '' &&
        doc.body_wiped === false);
}

// ---- ③ 未声明 ⇒ 自动识别：可见 + 下半屏 + 有 placeholder 的编辑区胜出（隐藏 / 上半屏候选落选）----
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'TEXTAREA', cls: 'hidden-box', top: 10, rect: { width: 0, height: 0 } },
    { tag: 'TEXTAREA', cls: 'top-box', attrs: { placeholder: '搜索' }, top: 10 },
    { tag: 'DIV', cls: 'chat-input-editor', attrs: { contenteditable: 'true' }, top: 700 },
  ]};
  const { p } = sendChain(pre, { input_selector: '' }, null);
  check('③ 自动识别选中下半屏可见编辑区（input_auto=true / 指纹=div.chat-input-editor / contenteditable）',
        p.ok === true && p.input_auto === true && p.input_fingerprint === 'div.chat-input-editor' &&
        p.contenteditable === true && p.focused === true);
}

// ---- ④ 输入框一个都取不到 ⇒ 如实失败 + 可执行的诊断（不静默、不瞎写）----
{
  const pre = { tag: 'BODY', kids: [{ tag: 'DIV', cls: 'static-footer', text: '仅供测试' }] };
  const { p } = sendChain(pre, { input_selector: 'textarea.chat-input' }, null);
  check('④ 输入框未命中 ⇒ ok=false + error 含「输入框未命中」与「--web-dom-dump」诊断出口',
        p.ok === false && has(p.error, '输入框未命中') && has(p.error, '--web-dom-dump'));
}

// ---- ⑤ 基线 + 回执锚点：发送**前**给池内候选打标（`__aiwritePreSend`）+ 记 `url_before` ----
{
  const { p, rt, doc } = sendChain(DOUBAO_PRE(), {}, null);
  check('⑤ 记基线 / 打锚点：baseline=2 · 池内 2 个候选全打上 __aiwritePreSend · url_before 已记',
        p.ok === true && p.baseline === 2 && marked(doc) === 2 &&
        !!rt && rt.url_before === URL0 &&
        rt.composer === doc.find('chat-input-editor'));
}

// ---- ⑥ **安全**：聚焦失败时**不清空、不动页面**（未聚焦的 selectAll 选的是整页 ⇒ 会清掉页面）----
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-input-editor', attrs: { contenteditable: 'true' }, top: 700,
      text: '上一次的残留', focus_fails: true },
  ]};
  const { p, doc } = sendChain(pre, {}, null);
  check('⑥ 聚焦失败 ⇒ focused=false·cleared=false·**整页未被清**（body_wiped=false）·残留文本仍在',
        p.ok === true && p.focused === false && p.cleared === false &&
        doc.body_wiped === false && doc.find('chat-input-editor').innerText === '上一次的残留');
}

// ---- ⑦ 真机反例（本批根因）：被**静默忽略** ⇒ 回执如实说「没发出去」（URL 未变 + 无新节点）----
{
  const { v, doc } = sendChain(DOUBAO_PRE(), {}, null); // 站点对输入**毫无反应**（旧合成事件的真机形态）
  check('⑦ 静默忽略 ⇒ sent=false（URL未变化 / 新节点 0 / 输入框 0 字）—— 不再「写完就当成功」',
        v.sent === false && v.url_changed === false && v.new_nodes === 0 &&
        v.composer_len === 0 && v.composer_has_prompt === false &&
        has(v.note, 'URL未变化') && has(v.note, '新节点 0'));
  check('⑦b 打标锚点生效：池内「今天 13:31」（12 字节 ≥ min_chars）**没有**被算成新消息',
        marked(doc) === 2 && v.new_nodes === 0);
}

// ---- 用例续（⑧ … ⑮）：CDP 成功 / 兜底点击 ----

// ---- ⑧ 提示词**还留在输入框**（CDP 写进去了但站点没消费 / 回车没生效）⇒ 如实判「未发出」----
{
  const { v } = sendChain(DOUBAO_PRE(), {}, (doc) => setComposer(doc, PROMPT));
  check('⑧ 提示词仍在输入框 ⇒ sent=false 且 composer_has_prompt=true（C++ 侧据此**硬失败**）',
        v.sent === false && v.composer_has_prompt === true && v.composer_len === PROMPT.length &&
        has(v.note, '含提示词'));
}

// ---- ⑨ CDP 成功（豆包同 URL SPA 形态）：输入框被消费 + 出现新消息节点 ⇒ sent=true ----
{
  const { v } = sendChain(DOUBAO_PRE(), {}, (doc) => {
    setComposer(doc, ''); // 站点收下文本（编辑器被清空）
    doc.append(doc.find('message-list'),
               { tag: 'DIV', cls: 'message-body', text: '这块石碑写着「九字真言」。' });
  });
  check('⑨ 真发出（输入框清空 + 新消息节点）⇒ sent=true / new_nodes=1 / note 含「新节点 1」',
        v.sent === true && v.new_nodes === 1 && v.composer_len === 0 && has(v.note, '新节点 1'));
}

// ---- ⑩ 首条消息 SPA 换 URL ⇒ 也算发出（豆包首条消息必然换 URL）----
{
  const { v } = sendChain(DOUBAO_PRE(), {}, (doc) => setComposer(doc, ''), URL1);
  check('⑩ URL 变化（/chat/ → /chat/<id>）⇒ sent=true / url_changed=true / note 含「URL已变化」',
        v.sent === true && v.url_changed === true && has(v.note, 'URL已变化'));
}

// ---- ⑪ 新建的**池容器包着输入区** ⇒ 不算「新消息」（否则会把自己的输入区当成回答）----
{
  const { v } = sendChain(DOUBAO_PRE(), {}, (doc) => {
    setComposer(doc, PROMPT); // 文本仍在输入区（站点尚未消费）
    const wrap = doc.append(doc.__root, { tag: 'DIV', cls: 'chat-content', kids: [] });
    doc.move(doc.find('chat-input-editor'), wrap); // 站点换壳：编辑区被挂到新容器里
  });
  check('⑪ 「新池容器 + 内含输入区」⇒ 不算新消息（new_nodes=0 / sent=false）',
        v.sent === false && v.new_nodes === 0);
}

// ---- ⑫ 兜底点击：**声明值优先**（`web.send.value` 命中即点它，不猜）----
{
  const doc = build({ tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'send-btn' },
    { tag: 'BUTTON', cls: 'other-send', attrs: { 'aria-label': '发送' } },
  ]});
  const r = runScript(CLICK, baseCfg({ send_value: 'div.send-btn' }), {}, doc).r;
  check('⑫ 声明 send_value 命中 ⇒ clicked=true / declared=true / selector=声明值（且只点它）',
        r.clicked === true && r.declared === true && r.selector === 'div.send-btn' &&
        doc.find('send-btn').__clicked === 1 && doc.find('other-send').__clicked === 0);
}

// ---- ⑬ 兜底点击：无声明 ⇒ 类名 / aria 像「send / 发送」的**可见**按钮（倒序：靠后者优先）----
{
  const doc = build({ tag: 'BODY', kids: [
    { tag: 'BUTTON', cls: 'attach-btn' },
    { tag: 'BUTTON', cls: 'real-send-btn' },
    { tag: 'BUTTON', cls: 'ghost-send', attrs: { 'aria-label': '发送' },
      rect: { width: 0, height: 0 } }, // 不可见（隐藏的发送按钮不点）
  ]});
  const r = runScript(CLICK, baseCfg({}), {}, doc).r;
  // 倒序扫描（靠后的按钮更可能是当前会话的发送键）⇒ **命中即停**，`candidates` 只数到命中为止：
  // 隐藏的 `ghost-send` 不可点（不计入）、`attach-btn` 在命中点之后（未扫到）⇒ 计数 1
  check('⑬ 无声明 ⇒ 点可见的 send 类按钮（命中 button.real-send-btn · 只点 1 次）；不可见 / 无关按钮**未**被点',
        r.clicked === true && r.declared === false && r.selector === 'button.real-send-btn' &&
        r.candidates === 1 &&
        doc.find('real-send-btn').__clicked === 1 &&
        doc.find('attach-btn').__clicked === 0 && doc.find('ghost-send').__clicked === 0);
}

// ---- ⑭ 兜底点击：发送按钮**被置灰**（disabled）⇒ 不点它，如实报「未找到」----
{
  const doc = build({ tag: 'BODY', kids: [
    { tag: 'BUTTON', cls: 'attach-btn' },
    { tag: 'BUTTON', cls: 'send-btn', disabled: true },
  ]});
  const r = runScript(CLICK, baseCfg({}), {}, doc).r;
  check('⑭ 发送按钮 disabled ⇒ clicked=false（不点）+ note 含「未找到」+ 候选计数 1（只算可点的）',
        r.clicked !== true && has(r.note, '未找到') && r.candidates === 1 &&
        doc.find('send-btn').__clicked === 0);
}

// ---- ⑮ 兜底点击：一个按钮都没有 ⇒ 如实失败（可诊断，不抛异常）----
{
  const doc = build({ tag: 'BODY', kids: [{ tag: 'DIV', cls: 'static-footer', text: '无按钮' }] });
  const r = runScript(CLICK, baseCfg({}), {}, doc).r;
  check('⑮ 页面无按钮 ⇒ clicked=false + note 含「未找到」+ 候选 0（如实失败，不抛）',
        r.clicked === false && has(r.note, '未找到') && r.candidates === 0);
}

console.log('PW-07f 发送链路夹具：' + pass + ' / ' + fail +
            (OLD_MODE ? '（--old 负向模式：⑥⑦⑧⑪ 如实 FAIL 属**预期** ⇒ 退出码非 0）' : ''));
process.exit(fail ? 1 : 0);
