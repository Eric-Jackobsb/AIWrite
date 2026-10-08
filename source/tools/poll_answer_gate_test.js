// =============================================================================
//  poll_answer_gate_test.js —— PW-07（精简版）「答案**归属**门」行为夹具
//
//  做什么：从源码 `source/src/ai/dom_web_client.cpp` **抽取真实的** `kPollScript`
//          与三个判据常量（`kAnswerPoolSelector` / `kAnswerChromeTextRe` /
//          `kMinAutoAnswerChars`），在 node 里用**迷你 DOM** 真跑该脚本，
//          断言「装饰 / 布局文本不会被当成答案」且「真答案**不被误伤**」——
//          不复制逻辑、不 mock 判据（源码一动，夹具立刻能看出行为变了）。
//
//  为什么需要它（真机证据 · 2026-10-06 / 2026-10-08）：
//    · 豆包：布局容器 `div.bp5-overflow-list`（首个类名）整串文本 = 时间条「今天 13:31」
//      ⇒ 旧判据（不在基线 + ≥ min_chars + 文档顺序最后）把**时间戳**判定成「新答案」
//      ⇒ 输出 **12 字节**「假成功」（`AW-04` 反例：时间戳被喂给下游节点 `n8` / `n2`）；
//    · Kimi：用户消息操作条整串 = 「Copy Share」（**10 字节**）—— 同族。
//    上列两条形态在夹具里**逐字复刻**：`今天` + `13:31` 两个子节点 ⇒ `innerText` 归一后
//    = `今天 13:31`（**12 字节**）、`fingerprint` = `div.bp5-overflow-list`。
//
//  用法：node source\tools\poll_answer_gate_test.js
//        node source\tools\poll_answer_gate_test.js --old   （= POLL_GATE_OLD=1）
//          `--old`：把脚本还原成**本批之前**的写法（去掉装饰文本门）⇒ **①② 应如实 FAIL**
//                   —— 证明夹具能**捕获回归**（负向模式是故意注入回归，退出码非 0 属预期；
//                   替换**校验生效**，否则会「全绿而什么都没测到」）。
//
//  为什么放 `source/tools/`（**入库**）而不是 `build/`：2026-10-08 实测教训 ——
//    `build/` 被 `cmake` **重新配置**（干净重建）时整个目录被覆盖清空，放在那里的
//    临时夹具曾**丢失**。夹具是回归资产 ⇒ 与 `api_probe.cpp` 同级入库。
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
      else { s += n; } // \\ → \ 、\" → " 、其余原样
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
// `constexpr const char* kPollScript = R"JS( … )JS";` ⇒ 脚本体（支持多段相邻 raw string）
function extractPollScript() {
  const m = SRC.match(/constexpr\s+const\s+char\*\s+kPollScript\s*=\s*([\s\S]*?)\)JS";/);
  if (!m) { throw new Error('抽取失败：kPollScript'); }
  return m[1].replace(/R"JS\(/g, '').replace(/\)JS"/g, '');
}
const POOL_SEL = extractConst('kAnswerPoolSelector');
const CHROME_TEXT_RE = extractConst('kAnswerChromeTextRe');
const m_min = SRC.match(/constexpr\s+int\s+kMinAutoAnswerChars\s*=\s*(\d+)/);
if (!m_min) { throw new Error('抽取失败：kMinAutoAnswerChars'); }
const MIN_CHARS = parseInt(m_min[1], 10);

// 取真实脚本体；`--old` = 去掉**装饰文本门**（本批的根因修）⇒ ①② 必须如实 FAIL
let SCRIPT = extractPollScript();
const GATE_LINE = 'if (chrome_text && chrome_text.test(t)) { refuse.chrome_text++; return false; }';
const OLD_MODE = process.argv.indexOf('--old') >= 0 || process.env.POLL_GATE_OLD === '1';
if (OLD_MODE) {
  if (SCRIPT.indexOf(GATE_LINE) < 0) {
    throw new Error('--old 注入失败（源码字面量已变）：' + GATE_LINE);
  }
  SCRIPT = SCRIPT.replace(GATE_LINE, '');
}

// ------------------------------------------------------------------ 迷你 DOM ----
// 支持的选择器：`*` / 标签 / `.class` / `#id` / `[attr]` / `[attr="v"]` / `[attr*="v"]`
// 与**逗号分组** —— 覆盖候选池用到的全部口径（池里只有标签与属性，无伪类 / 后代选择器）。
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
const norm = (s) => String(s || '').replace(/\s+/g, ' ').trim();

// 按 spec 建树：`{ tag, cls, id, attrs, text, kids }`；`innerText` = 自身文本 + 子文本用 `\n`
// 拼接后自底向上刷新（与浏览器「块级子元素各占一行」同序 ⇒ `norm` 后即真机形态，
// 例：`昨天`… 此处 `今天` + `13:31` ⇒ `今天 13:31` = 12 字节 —— 与豆包真机日志**逐字一致**）
function build(spec) {
  const reg = [];
  function mk(s, parent) {
    const el = {
      tagName: (s.tag || 'DIV').toUpperCase(), className: s.cls || '', id: s.id || '',
      attrs: s.attrs || {}, children: [], __parent: parent || null, __own: s.text || '',
      textContent: '', innerText: '',
    };
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
  const doc = {
    body: root,
    querySelectorAll: (sel) => (sel === '*' ? reg.slice() : reg.filter((el) => matchSel(el, sel))),
    querySelector: (sel) => doc.querySelectorAll(sel)[0] || null,
    // 夹具内定位用（按 class 取节点）
    find: (cls) => reg.filter((el) => (' ' + el.className + ' ').indexOf(' ' + cls + ' ') >= 0)[0] || null,
    __reg: reg, __root: root,
  };
  return doc;
}

// ------------------------------------------------------------ 跑真实脚本一「轮」----
function baseCfg(o) {
  return Object.assign({
    answer_selector: '', answer_pool: POOL_SEL, answer_min_chars: MIN_CHARS,
    answer_chrome_text: CHROME_TEXT_RE, prompt: '', input_selector: '',
    auto_answer: true, done_kind: '', done_selector: '',
  }, o || {});
}
function run(cfg, rt, doc) {
  const win = { __aiwriteDom: cfg, __aiwriteDomRuntime: rt };
  const NodeConst = { DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_PRECEDING: 2 };
  // `SCRIPT` 以换行开头 ⇒ 先 trim（否则 `return\n(...)` 触发 ASI，返回值变 undefined）
  const fn = new Function('window', 'document', 'Node',
                          'return ' + SCRIPT.trim() + '\n//# sourceURL=poll.js');
  return fn(win, doc, NodeConst);
}
// 发送前基线 = 池内节点的归一文本（与脚本内 `norm` 同口径；发送前的空文本节点自然不匹配）
const baselineOf = (preDoc) => preDoc.querySelectorAll(POOL_SEL).map((el) => norm(el.innerText));
// 一次完整场景：发送前 DOM（取基线）→ 发送后 DOM → 跑一轮 poll
function scenario(pre, post, composer_cls, cfg) {
  const preDoc = build(pre);
  const postDoc = build(post);
  const rt = { composer: composer_cls ? (postDoc.find(composer_cls) || null) : null,
               baseline: baselineOf(preDoc), seen: {} };
  return { r: run(baseCfg(cfg), rt, postDoc), doc: postDoc };
}

// -------------------------------------------------------------------- 断言 ----
let pass = 0, fail = 0;
function check(name, ok) {
  if (ok) { pass++; console.log('  PASS ' + name); }
  else { fail++; console.log('  FAIL ' + name); }
}
const has = (s, sub) => String(s || '').indexOf(sub) >= 0;
// 从体检串取某个计数（`标签 N`）—— 只断言「**拒过**（≥1）」，不断言精确值（计数属实现细节）
const count = (note, label) => {
  const m = new RegExp(label + ' (\\d+)').exec(String(note || ''));
  return m ? parseInt(m[1], 10) : -1;
};

const SPRING = '春天的风把柳枝吹得很软，燕子从南边回来，落在屋檐下。';
const PROMPT = '描述这张图片';
// 豆包形态的**发送前**空壳：对话容器 + 消息列表都在，但**文本为空**（真机 13:31 的那一页）
const PRE_DOUBAO = { tag: 'BODY', kids: [
  { tag: 'DIV', cls: 'chat-content-container', kids: [
    { tag: 'DIV', cls: 'bp5-overflow-list message-list' },
  ]},
  { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
]};
// 发送后：布局容器里长出「时间条」（首个子节点）与（可选）真答案
const doubaoPost = (tip, answer) => {
  const list = { tag: 'DIV', cls: 'bp5-overflow-list message-list', kids: [] };
  if (tip) {
    list.kids.push({ tag: 'DIV', cls: 'message-tip', kids: [
      { tag: 'SPAN', text: '今天' }, { tag: 'SPAN', text: '13:31' } ] });
  }
  if (answer) { list.kids.push({ tag: 'DIV', cls: 'markdown', text: answer }); }
  return { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [list] },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
};

console.log('[' + (OLD_MODE ? '旧口径 --old（去掉装饰文本门）' : '当前源码') +
            '] 运行 PW-07 答案归属门夹具 …');

// ---- ⓪ 夹具**保真度**：锁死「与真机日志逐字一致」的形态（否则用例会悄悄漂走）----
{
  const doc = build(doubaoPost(true, null));
  const node = doc.find('bp5-overflow-list');
  const t = norm(node.innerText);
  check('⓪ 夹具保真度：布局容器文本 = 「今天 13:31」= **12 字节**、指纹 = div.bp5-overflow-list',
        t === '今天 13:31' && Buffer.byteLength(t, 'utf8') === 12 &&
        node.tagName.toLowerCase() + '.' + node.className.split(' ')[0] === 'div.bp5-overflow-list');
}

// ---- ① 豆包真机形态：布局容器 `div.bp5-overflow-list`（池内命中）+ 时间条「今天 13:31」----
{
  const { r } = scenario(PRE_DOUBAO, doubaoPost(true, null), 'composer-box', { prompt: PROMPT });
  check('① 时间条「今天 13:31」不 found（不再 12 字节假成功）', r.found === false && r.text === '');
  check('① 体检串回显「候选池 N 个 · 装饰文本拒 ≥1」（不是因为池空而没 found）',
        count(r.note, '装饰文本拒') >= 1 && r.count >= 2 && has(r.note, '候选池 ' + r.count));
}

// ---- ② Kimi 真机形态：用户消息操作条（池内命中 = `[role="listitem"]`）+「Copy Share」----
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-item' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-item', kids: [
      { tag: 'DIV', cls: 'segment-user-action-row', attrs: { role: 'listitem' }, kids: [
        { tag: 'SPAN', text: 'Copy' }, { tag: 'SPAN', text: 'Share' } ] },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('② 操作条「Copy Share」不 found（不再 10 字节假成功）', r.found === false && r.text === '');
  check('② 体检串回显「装饰文本拒 ≥1」', count(r.note, '装饰文本拒') >= 1);
}

// ---- ③ 正解：装饰条与真答案并存的**两种文档序**都要取到真答案 ----
//   （夹具自证：③a 里逆序先命中真答案就 `break` ⇒ 「装饰拒」计数天然为 0 —— 故**不**断言计数；
//     真正的**承重**用例是 ③b：装饰条排在真答案**之后** ⇒ 逆序必先遇到它 ⇒ 没有这道门就会选错）
{
  const { r } = scenario(PRE_DOUBAO, doubaoPost(true, SPRING), 'composer-box', { prompt: PROMPT });
  check('③a 时间条在前 + 真答案在后（真机同序）⇒ found 且取真答案（非时间条）',
        r.found === true && r.text === SPRING && r.auto === true &&
        r.fingerprint === 'div.markdown');
}
{
  // 发送后：真答案在前、装饰条在后（例如回答下方的时间戳 / 状态行）
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'bp5-overflow-list message-list', kids: [
        { tag: 'DIV', cls: 'markdown', text: SPRING },
        { tag: 'DIV', cls: 'message-tip', kids: [
          { tag: 'SPAN', text: '今天' }, { tag: 'SPAN', text: '13:31' } ] },
      ]},
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(PRE_DOUBAO, post, 'composer-box', { prompt: PROMPT });
  check('③b 装饰条在真答案**之后**（逆序先遇到它）⇒ 仍取真答案，且体检串「装饰文本拒 ≥1」',
        r.found === true && r.text === SPRING && r.fingerprint === 'div.markdown' &&
        count(r.note, '装饰文本拒') >= 1);
}

// ---- ④ 短候选**不**回落（PW-07 显式项：取消「短候选也算成功」）----
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'message-list' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'message-list', kids: [
      { tag: 'DIV', cls: 'message-body', text: '好的' } ] },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('④ 仅短候选（「好的」< min_chars）⇒ 不 found（继续轮询 / 由 C++ 侧如实报错）',
        r.found === false && r.text === '' && count(r.note, '过短拒') >= 1);
}

// ---- ⑤ 声明值优先**不受影响**（装饰门只管自动识别；用户显式指定 = 语义不变）----
{
  const pre = { tag: 'BODY', kids: [{ tag: 'DIV', cls: 'declared-tip' }] };
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'declared-tip', text: '今天 13:31' } ] };
  const { r } = scenario(pre, post, null, { prompt: PROMPT, answer_selector: 'div.declared-tip' });
  check('⑤ answer_selector 命中即取（声明值不被装饰门拦截）',
        r.found === true && r.text === '今天 13:31' && r.auto === false);
}

// ---- ⑥ **不误伤**：真答案里出现「今天」/「复制」照样取（整串才拒，非子串）----
{
  const answer = '今天 13:31 我把图片里的石碑拓下来，复制了拓片上的九个字。';
  const { r } = scenario(PRE_DOUBAO, doubaoPost(false, answer), 'composer-box', { prompt: PROMPT });
  check('⑥ 真答案含「今天」/「复制」⇒ 仍 found，且**零**装饰拒（整串匹配，不做子串排除）',
        r.found === true && r.text === answer && count(r.note, '装饰文本拒') === 0);
}

console.log('PW-07 答案归属门夹具：' + pass + ' / ' + fail +
            (OLD_MODE ? '（负向模式 --old：①② 失败即**预期** —— 证明夹具能捕获回归）' : ''));
process.exit(fail === 0 ? 0 : 1);


