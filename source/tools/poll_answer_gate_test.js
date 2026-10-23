// =============================================================================
//  poll_answer_gate_test.js —— PW-07（精简版）「答案**归属**门」行为夹具
//
//  做什么：从源码 `source/ai/include/dom_web_client_scripts*.h`（ARCH-Z1b 拆分后脚本模板所在）
//          与 `source/ai/src/dom_web_client.cpp` **抽取真实的** `kPollScript`
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
//  PW-07g（2026-10-08 真机驱动 · **文本保真**）追加断言：
//    · ⑯ **段落保真**：容器内多段（`\n` 分隔）⇒ 产物**逐字符原样**（含 2 个 `\n`）、`lf` = 2
//      —— 修的就是「`norm` 顺手把产物也归一 ⇒ 段落换行被压成空格、输出整篇 1 行」；
//    · ⑰ **页面侧原样搬运**：缩进 / 空行 / 行尾空格一律**不动**（排版交 C++ 纯函数
//      `format_answer_text`，其逐例断言在 `api_probe --selftest` 的 `VB2-22⑧`）。
//    判据侧（基线差分 / 装饰整串门 / 回声排除 / `min_chars`）**继续吃 `norm`** ⇒ ⓪①④ 不变。
//
//  PW-07i（2026-10-10 真机驱动 · `T-L11` gemini-web）追加断言：
//    · ⑲a/⑲b **控件词拼接**不算答案 —— `gemini-web` 自动识别把操作条 / 工具提示的拼接
//      （`Dictate (^⇧D)Close sidebarImagesSettingsEdit promptCopy promptCopy responseMoreRedo`，**85 字节**）
//      当答案写进 `n3`/`n4`（三口径全判「可用」= **假成功**）；整串门抓不住它（**多个**控件词拼接）
//      ⇒ C++ 侧加常量 `kAnswerChromeWordsRe` + 「剥离后残余近空 ⇒ 拒」（**只拒、不猜**）；
//    · ⑲c **不得误伤**：真答案里出现控件词（`Search` / `More` / `Redo` / `复制`）照常通过、文本逐字未动。
//    · `--old` 注入点③：删掉该门 ⇒ ⑲a/⑲b 必须 FAIL（证明夹具能捕获该类回归）。
//
//  用法：node source\tools\poll_answer_gate_test.js
//        node source\tools\poll_answer_gate_test.js --old   （= POLL_GATE_OLD=1）
//          `--old`：把脚本还原成**本批之前**的写法（去掉装饰文本门 + 产物换回归一）
//                   ⇒ **①②③b⑯⑰ 应如实 FAIL** —— 证明夹具能**捕获回归**（负向模式是故意注入回归，
//                   退出码非 0 属预期；替换**校验生效**，否则会「全绿而什么都没测到」）。
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

// ARCH-Z1b（2026-10-23）：脚本模板与判据常量已拆到 `ai/include/dom_web_client_scripts*.h`，
//  运行期 .cpp 只保留调用方 —— 抽取口 = 「两个模板头 + 主体 .cpp」按顺序拼接（源码一动夹具即见）。
const AI_DIR = path.join(__dirname, '..', 'ai');
const SRC = [
    path.join(AI_DIR, 'include', 'dom_web_client_scripts.h'),
    path.join(AI_DIR, 'include', 'dom_web_client_scripts_poll.h'),
    path.join(AI_DIR, 'src', 'dom_web_client.cpp'),
].map((file) => fs.readFileSync(file, 'utf8')).join('\n');

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
const POOL_EXCL = extractConst('kAnswerPoolExclude'); // PW-07j：工具提示 / 屏读宿主（结构排除）
const CHROME_TEXT_RE = extractConst('kAnswerChromeTextRe');
const CHROME_WORDS_RE = extractConst('kAnswerChromeWordsRe'); // PW-07i：控件词剥离门
const m_min = SRC.match(/constexpr\s+int\s+kMinAutoAnswerChars\s*=\s*(\d+)/);
if (!m_min) { throw new Error('抽取失败：kMinAutoAnswerChars'); }
const MIN_CHARS = parseInt(m_min[1], 10);

// 取真实脚本体；`--old` = 还原**本批之前**的写法（装饰文本门 + 产物归一）⇒ ①②⑯ 必须如实 FAIL
let SCRIPT = extractPollScript();
const GATE_LINE = 'if (chrome_text && chrome_text.test(t)) { refuse.chrome_text++; return false; }';
// PW-07g：产物行（原样搬运）—— `--old` 注入点②：换回 `norm` ⇒ ⑯（多段保真）必须 FAIL
const PROD_RAW = 'out.text = raw;';
const OLD_MODE = process.argv.indexOf('--old') >= 0 || process.env.POLL_GATE_OLD === '1';
if (OLD_MODE) {
  if (SCRIPT.indexOf(GATE_LINE) < 0) {
    throw new Error('--old 注入失败（源码字面量已变）：' + GATE_LINE);
  }
  SCRIPT = SCRIPT.replace(GATE_LINE, '');
  if (SCRIPT.indexOf(PROD_RAW) < 0) {
    throw new Error('--old 注入失败（PW-07g 产物行字面量已变）：' + PROD_RAW);
  }
  SCRIPT = SCRIPT.split(PROD_RAW).join('out.text = norm(raw);'); // 旧口径：归一值当产物
  // PW-07i：`--old` 注入点③ —— 去掉控件词剥离门 ⇒ ⑲a 必须 FAIL（证明夹具能捕获该类回归）
  const GATE_LINE2 = 'if (chrome_only(t)) { refuse.chrome_only++; return false; }';
  if (SCRIPT.indexOf(GATE_LINE2) < 0) {
    throw new Error('--old 注入失败（PW-07i 门字面量已变）：' + GATE_LINE2);
  }
  SCRIPT = SCRIPT.replace(GATE_LINE2, '');
  // PW-07j：`--old` 注入点④ —— 关掉「工具提示宿主剔除」⇒ ⑳ 必须 FAIL
  const EXCL_LINE = "const pool_excl = cfg.answer_pool_exclude || '';";
  if (SCRIPT.indexOf(EXCL_LINE) < 0) {
    throw new Error('--old 注入失败（PW-07j 排除行字面量已变）：' + EXCL_LINE);
  }
  SCRIPT = SCRIPT.replace(EXCL_LINE, "const pool_excl = '';");
  // PW-07k：`--old` 注入点⑤ —— 去掉「本方消息回声」门 ⇒ ㉑a 必须 FAIL
  const ECHO_LINE = 'if (echo_only(t)) { refuse.echo++; return false; }';
  if (SCRIPT.indexOf(ECHO_LINE) < 0) {
    throw new Error('--old 注入失败（PW-07k 回声门字面量已变）：' + ECHO_LINE);
  }
  SCRIPT = SCRIPT.replace(ECHO_LINE, '');
  // PW-07m：`--old` 注入点⑥ —— 去掉「站侧内部码」门 ⇒ ㉒a 必须 FAIL
  const CODE_LINE = 'if (internal_code_only(t)) { refuse.internal++; return false; }';
  if (SCRIPT.indexOf(CODE_LINE) < 0) {
    throw new Error('--old 注入失败（PW-07m 内部码门字面量已变）：' + CODE_LINE);
  }
  SCRIPT = SCRIPT.replace(CODE_LINE, '');
  // PW-07n：`--old` 注入点⑦ —— 去掉「生成中占位」门 ⇒ ㉓a 必须 FAIL
  const PROG_LINE = 'if (progress_only(t)) { refuse.progress++; return false; }';
  if (SCRIPT.indexOf(PROG_LINE) < 0) {
    throw new Error('--old 注入失败（PW-07n 生成中门字面量已变）：' + PROG_LINE);
  }
  SCRIPT = SCRIPT.replace(PROG_LINE, '');
  // PW-07p：`--old` 注入点⑧ —— 去掉「提示词派生短句列表」门 ⇒ ㉕a 必须 FAIL
  const SUG_LINE = 'if (suggest_list(raw_el)) { refuse.suggest++; return false; }';
  if (SCRIPT.indexOf(SUG_LINE) < 0) {
    throw new Error('--old 注入失败（PW-07p 建议列表门字面量已变）：' + SUG_LINE);
  }
  SCRIPT = SCRIPT.replace(SUG_LINE, '');
  // PW-07q：`--old` 注入点⑨ —— 去掉「外壳（内含其他候选）剔除」⇒ ㉖a 必须 FAIL
  const SHELL_LINE = 'if (inner_of_some(pool[i])) { refuse.shell++; continue; }';
  if (SCRIPT.indexOf(SHELL_LINE) < 0) {
    throw new Error('--old 注入失败（PW-07q 外壳剔除字面量已变）：' + SHELL_LINE);
  }
  SCRIPT = SCRIPT.replace(SHELL_LINE, '');
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
    // PW-07j：真机脚本用 `el.matches(pool_excl)` 剔「工具提示 / 屏读宿主」⇒ 迷你 DOM 同口径提供
    el.matches = function (sel) { return matchSel(this, sel); };
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
    answer_chrome_words: CHROME_WORDS_RE, answer_pool_exclude: POOL_EXCL,
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

// ------------------------------------------------------------------ 断言 ----
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

console.log('[' + (OLD_MODE ? '旧口径 --old（装饰文本门 + 产物归一）' : '当前源码') +
            '] 运行 PW-07 / PW-07g 答案归属与文本保真夹具 …');

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

// ---- ⑯ PW-07g：**段落保真** —— 多段答案 ⇒ 产物逐字符原样（含 `\n`）、`lf` 回显段数 ----
//   真机形态：豆包回答容器（`div.container-qX9Csx`）**内部**多段 ⇒ `innerText` 段间为 `\n`；
//   旧口径 `norm`（`\s+` → `' '`）把它压成空格 ⇒ 输出文件整篇 **1 行**（LF 全在文件头）。
//   本用例锁「页面侧**不动**产物」：内容是完整的、**分段也在**。
{
  const p1 = '我不知道我到底错哪里了。';
  const p2 = '我试着回想整件事的始末，可最后还是落得这样的结果。';
  const p3 = '你想要偏伤感一点，还是克制平静一点的版本？';
  const answer = p1 + '\n' + p2 + '\n' + p3; // 迷你 DOM：块级子节点各占一行 ⇒ 以 `\n` 分隔
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'bp5-overflow-list message-list', kids: [
        // 单个**答案容器**内多段（段落节点自身不匹配候选池 ⇒ 取到的就是容器）
        { tag: 'DIV', cls: 'markdown-body', kids: [
          { tag: 'DIV', cls: 'md-line', text: p1 },
          { tag: 'DIV', cls: 'md-line', text: p2 },
          { tag: 'DIV', cls: 'md-line', text: p3 },
        ]},
      ]},
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(PRE_DOUBAO, post, 'composer-box', { prompt: PROMPT });
  check('⑯ 多段答案 ⇒ 产物**逐字符原样**（含 2 个 \\n）· `lf` = 2 · 无「句号+空格」压平指纹',
        r.found === true && r.auto === true && r.text === answer && r.lf === 2 &&
        r.fingerprint === 'div.markdown-body' &&
        (r.text.match(/\n/g) || []).length === 2 && r.text.indexOf(p1 + ' ' + p2) < 0);
}

// ---- ⑰ PW-07g：**页面侧原样搬运** —— 缩进 / 空行 / 行尾空格一律**不动** ----
//   （排版是 C++ 纯函数 `format_answer_text` 的职责：缩进保留、空行**规范化**（A2：空行 = 段界）、行尾空白清除）
{
  const messy = '  首段缩进保留\n\n\n第二段末尾有空格   ';
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'message-list' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'message-list', kids: [
      { tag: 'DIV', cls: 'markdown-block', text: messy },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('⑰ 页面侧原样搬运：缩进 / 空行 / 行尾空格**不动**（规整交 C++ 纯函数 · `VB2-22⑧`）',
        r.found === true && r.text === messy && r.lf === 3);
}

// ---- ⑱ PW-07h：轮询回执**带实时 URL**（「发出去了却没答案」的归因现场）----
//   * 真机驱动：chatgpt-web 第 2 次尝试「发送已生效（URL 已变化 / 输入框 0 字）」却「候选池 0 个」
//     ⇒ 光看旧回执分不清「跳去登录页」还是「站侧没渲染答案」；把 `location.href` 带回来即可定性。
//   * Node 夹具里 `location` **不存在** ⇒ 脚本不许抛（安全取空串），C++ 侧见空串就不写这一段。
{
  const s0 = scenario(doubaoPost(true, SPRING), doubaoPost(true, SPRING), 'composer-box', {});
  check('⑱a 无 `location`（Node 夹具）⇒ 不抛异常且 `url` 为空串（C++ 侧据此省略该诊断段）',
        typeof s0.r === 'object' && s0.r !== null && s0.r.url === '');
  // ⑱c：`ready` / `body_chars` 一并带回（区分「页面还在加载：正文 0 字」vs「已渲染但池不匹配」）
  check('⑱c 回执带 `ready` + `body_chars`（正文 = body 的 innerText 长度；无 `document.readyState` ⇒ 空串）',
        s0.r.ready === '' && s0.r.body_chars === s0.doc.body.innerText.length &&
        Object.prototype.hasOwnProperty.call(s0.r, 'body_chars'));
  // ⑱d：`body_head` = 页面可见文本**首 80 字**（空白压平；> 80 截断 ⇒ 登录墙一眼可辨）
  {
    const long = 'A'.repeat(200);
    const sLong = scenario(doubaoPost(null, long), doubaoPost(null, long), 'composer-box', {});
    const sTrim = scenario(doubaoPost(null, '  登 录 墙\n\nSign up  '), doubaoPost(null, '  登 录 墙\n\nSign up  '),
                           'composer-box', {});
    check('⑱d `body_head` 首 80 字（超长截断）+ 空白压平（登录墙 / 限额页定性用）',
          sLong.r.body_head.length === 80 && sTrim.r.body_head.indexOf('\n') < 0 &&
          sTrim.r.body_head.indexOf('登 录 墙 Sign up') >= 0);
  }
  const post = { tag: 'BODY', kids: [ { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } } ]};
  const cfg = baseCfg({});
  const doc = build(post);
  const fn = new Function('window', 'document', 'Node', 'location',
                          'return ' + SCRIPT.trim() + '\n//# sourceURL=poll.js');
  const out = fn({ __aiwriteDom: cfg, __aiwriteDomRuntime: { composer: doc.find('composer-box'), baseline: [], seen: {} } },
                 doc, { DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_PRECEDING: 2 },
                 { href: 'https://chatgpt.com/auth/login' });
  check('⑱b 有 `location` ⇒ 逐字带回实时 URL（登录墙形态一眼可辨：…/auth/login）',
        out.url === 'https://chatgpt.com/auth/login' && out.found === false);
}

// ---- ⑲ PW-07i：**控件词拼接**不算答案（真机反例 · `T-L11` gemini-web）----
//   * 现场：`gemini-web` 自动识别把**操作条 / 工具提示的拼接**当答案 ⇒ `n3`/`n4` = 77~85 字节的
//     `Dictate (^⇧D)Close sidebarImagesSettingsEdit promptCopy promptCopy responseMoreRedo`
//     （**整串门抓不住**：它不等于任一个控件词，而是**多个**控件词拼接；节点是池内命中的 `div.message-content`）。
//   * 口径：剥离控件词后残余近空 ⇒ 拒（`只拒、不猜`；真答案残余必然 ≥ `min_chars` ⇒ **不得误伤**）。
{
  // 真机原文（**逐字**取自 `build/loop/T-L11-20261010-1257/raw/gemini-web.run1.archive/n3-LLMGenerate.txt`；
  // 行内标签无分隔空格 ⇒ `innerText` 归一后正是下面这一串）
  const GEMINI_CHROME =
    'Dictate (^⇧D)Close sidebarImagesSettingsEdit promptCopy promptCopy responseMoreRedo';
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'message-content', text: GEMINI_CHROME },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('⑲a gemini-web 真机反例（操作条拼接 ' + Buffer.byteLength(GEMINI_CHROME, 'utf8') +
        ' 字节）不 found ⇒ 由「假成功」改判「如实失败」',
        r.found === false && r.text === '');
  check('⑲b 体检串回显「控件词拼接拒 ≥1」（不是因为池空 / 过短而没 found）',
        count(r.note, '控件词拼接拒') >= 1 && r.count >= 1);
}
{
  // ⑲c 不得误伤（`AW-04` 的反面）：真答案里**出现**控件词（甚至多个）⇒ 照常 found、文本逐字未动
  const ANSWER = 'Search 之后的结果如下：More 是最常用的按钮，Redo 用来重做，复制这段即可。';
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'message-content', text: ANSWER },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('⑲c 真答案含控件词（Search / More / Redo / 复制）**不被误伤**：found 且文本逐字一致',
        r.found === true && r.text === ANSWER && count(r.note, '控件词拼接拒') === 0);
}

// ---- ⑳ PW-07j：**工具提示 / 屏读宿主**不算候选（真机命中物 · `T-L12` gemini-web · 结构判据）----
//   * 现场：命中物 = `div.cdk-describedby-message-container`（Angular CDK 工具提示宿主，`aria-describedby`
//     文案的容器）—— 类名含 `message` ⇒ 被池里 `[class*="message"]` 误吞；又是文档序**最后**的候选
//     ⇒ 直接当答案（77 / 92 字节）。本题把它排在**真答案之后**，正是真机的文档序。
//   * 判据：池内**剔除自己**（`describedby` / `tooltip` / `sr-only` / `visually-hidden`）⇒ 轮到真答案；
//     同时 ⑳b 反向钉住「**回答容器不被误伤**」（剔除只看节点身份，不看祖先 / 不看文本）。
{
  const CHROME = 'Dictate (^⇧D)ImagesSettingsClose sidebarEdit promptCopy promptCopy responseMoreStop response';
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'message-content', text: SPRING },          // 真答案
    ]},
    { tag: 'DIV', cls: 'response-container' },                        // 池内（含 response），无文本
    { tag: 'DIV', cls: 'cdk-describedby-message-container', text: CHROME }, // 工具提示宿主（真机命中物）
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const { r } = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('⑳a 工具提示宿主（`cdk-describedby-message-container`）被剔 ⇒ 取到**真答案**（不再 92 字节假成功）',
        r.found === true && r.text === SPRING && r.fingerprint === 'div.message-content');
  check('⑳b 体检串回显「另剔非候选宿主 1 个」',
        has(r.note, '另剔非候选宿主 1 个'));
}

// ---- ㉑ PW-07k：**本方消息回声**不算答案（真机 · `T-L13` gemini-web）----
//   * 现场：剔掉 CDK 描述容器后，紧接着取到 `span.user-query-bubble-with-background` = `You said <提示词>`
//     （**172 字节**）⇒ `n3`/`n4` = **自己那条**（下游拿本方消息当回答）。旧口径只拦「整串等于提示词 /
//     以提示词开头」，带前缀（`You said `）的回声漏网。
//   * 判据：**含**提示词且**围绕它的残余**（去空白 / 标点后）< `min_chars` ⇒ 判回声（真答案残余必然够长）。
//   * 同批结构侧（PW-07k 的容器排除）：`user-query` / `query-text` / `user-message` 类 ⇒ 池内剔自己。
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  // ㉑a：回声挂在与装饰门无关的池内节点（`div.markdown`）上 ⇒ 只有回声门能拦住（真机形态逐字）
  const echoPost = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: 'You said ' + PROMPT },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, echoPost, 'composer-box', { prompt: PROMPT });
  check('㉑a `You said <提示词>`（172 字节形态）不 found ⇒ 不再拿**自己那条**当答案',
        s.r.found === false && s.r.text === '');
  check('㉑b 体检串回显「回声拒 ≥1」', count(s.r.note, '回声拒') >= 1);
  // ㉑e 真机 T-L14 形态：提示词在回声里**出现两次**（`You said <提示词> <提示词>` = 172 字节）
  //     ⇒ 回声门必须**剥掉全部出现**再量残余（只剥一次会漏网 —— 真机就是这么漏的）
  const echo2Post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: 'You said ' + PROMPT + ' ' + PROMPT },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s4 = scenario(pre, echo2Post, 'composer-box', { prompt: PROMPT });
  check('㉑e 提示词出现两次的回声（T-L14 真机 172 字节形态）也不 found（剥离全部出现）',
        s4.r.found === false && count(s4.r.note, '回声拒') >= 1);
  // ㉑c 不得误伤：真答案**包含**提示词（不在开头）+ 正文足够长 ⇒ 照常 found、文本逐字未动
  const ANSWER = '好的，关于「' + PROMPT + '」我的回答如下：图片里是一棵树，树下有人在走。';
  const ansPost = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: ANSWER },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, ansPost, 'composer-box', { prompt: PROMPT });
  check('㉑c 真答案含提示词（非开头）**不被误伤**：found 且文本逐字一致、回声拒 0',
        s2.r.found === true && s2.r.text === ANSWER && count(s2.r.note, '回声拒') === 0);
  // ㉑d 结构侧：本方消息容器（`user-query-*`）在**真答案之后**（真机文档序）⇒ 剔除后仍取真答案
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: SPRING },
    ]},
    { tag: 'SPAN', cls: 'user-query-bubble-with-background', text: 'You said ' + PROMPT },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s3 = scenario(pre, post2, 'composer-box', { prompt: PROMPT });
  check('㉑d 本方消息容器（`user-query-bubble-*`）池内剔除 ⇒ 取到真答案（真机文档序钉子）',
        s3.r.found === true && s3.r.text === SPRING && s3.r.fingerprint === 'div.markdown' &&
        has(s3.r.note, '另剔非候选宿主 1 个'));
}

// ---- ㉒ PW-07m：**站侧内部码**不算答案（真机 · `T-L17` 回归扫描 yuanbao-web）----
//   * 现场：`n3` / `n4` = `CONFLICT_LOG`（**12 字节**）—— 元宝把内部诊断节点渲染进了 DOM，且它是
//     文档序**最后**的候选 ⇒ 被三口径判「可用」⇒ 假成功（台账差点记成 yuanbao ✅）。
//   * 判据：整串（去空白 / 标点 / 符号后）就是**一个** `SNAKE_CASE` 常量名 ⇒ 不算答案（继续轮询）。
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  // ㉒a：真答案在前、内部码节点在**最后**（真机文档序）⇒ 门生效时取真答案
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: SPRING },
      { tag: 'DIV', cls: 'markdown', text: 'CONFLICT_LOG' },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('㉒a `CONFLICT_LOG`（12 字节内部码，文档序最后）不算答案 ⇒ 取到真答案',
        s.r.found === true && s.r.text === SPRING && count(s.r.note, '内部码拒') >= 1);
  // ㉒b 不得误伤：真答案里出现大写常量名（在句子里）照常 found、文本逐字未动
  const ANSWER = '建议把重试上限抽成常量 MAX_RETRY_COUNT，这样参数驱动才有一处可改。';
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: ANSWER },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, post2, 'composer-box', { prompt: PROMPT });
  check('㉒b 真答案里含 `MAX_RETRY_COUNT`（在句子里）**不被误伤**：found 且文本逐字一致、内部码拒 0',
        s2.r.found === true && s2.r.text === ANSWER && count(s2.r.note, '内部码拒') === 0);
}

// ---- ㉓ PW-07n：**生成中占位**不算答案（真机 · `T-L17` 回归扫描 ernie-web）----
//   * 现场：`n3` / `n4` = `P1C参数驱动续写故事 / 正在编写，请稍候... / 0% / 改用对话直接回答`
//     （站侧**自动标题** + 占位文案 + 进度条）⇒ 19 字节也被判「可用」⇒ 假成功。
//   * 判据：占位 / 进度短语 **且** 裸百分比（两个独立信号同时出现）⇒ 不算答案（继续轮询等真答案）。
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const placeholder = 'P1C参数驱动续写故事\n正在编写，请稍候...\n0%\n改用对话直接回答';
  // ㉓a：占位节点在**最后**（真机文档序）⇒ 门生效时取真答案
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: SPRING },
      { tag: 'DIV', cls: 'markdown', text: placeholder },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('㉓a `正在编写，请稍候...0%`（19 字节生成中占位，文档序最后）不算答案 ⇒ 取到真答案',
        s.r.found === true && s.r.text === SPRING && count(s.r.note, '生成中拒') >= 1);
  // ㉓b 不得误伤①：真答案里出现百分比但**没有**占位词 ⇒ found（单信号不触发）
  const ANSWER_PCT = '样本分布是这样的：37% 落在第一档，其余落在第二档，参数驱动下可以随时调。';
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: ANSWER_PCT },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, post2, 'composer-box', { prompt: PROMPT });
  check('㉓b 真答案含 `37%` 但无占位词 ⇒ 照常 found、生成中拒 0（单信号不误伤）',
        s2.r.found === true && s2.r.text === ANSWER_PCT && count(s2.r.note, '生成中拒') === 0);
  // ㉓c 不得误伤②：真答案里出现「请稍候」但**没有**百分比 ⇒ found（单信号不触发）
  const ANSWER_WAIT = '请稍候，我先把这段续写补完：山里的老和尚每天扫落叶，扫得很慢，却从不落下一片。';
  const post3 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: ANSWER_WAIT },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s3 = scenario(pre, post3, 'composer-box', { prompt: PROMPT });
  check('㉓c 真答案含「请稍候」但无百分比 ⇒ 照常 found、生成中拒 0（单信号不误伤）',
        s3.r.found === true && s3.r.text === ANSWER_WAIT && count(s3.r.note, '生成中拒') === 0);
}

// ---- ㉔ PW-07o：**站侧推荐追问**外壳不算候选（真机 · `T-L18` ernie-web）----
//   * 现场：取到 `div._answer-ask-wrapper_16z1u_8`（文心 CSS-module 外壳），文本 = 4 条追问
//     （`用代码模拟… / 把这段描述精简成… / 用幽默风格重新描述… / 能否再续写一段…`，183 字节）
//     ⇒ 被三口径判「可用」；真答案是它**内部**的 `div.cosd-markdown-content`。
//   * 判据 = **外壳身份**（`ask-wrapper`，节点自己匹配即剔，不看祖先）⇒ 内层真答案照常参与。
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const asks = '用代码模拟P1C参数驱动流程\n把这段描述精简成一句话总结核心\n用幽默风格重新描述P1C参数驱动过程\n能否再续写一段关于P1C参数驱动的描述';
  // ㉔a：真机结构 —— 真答案在前、追问外壳在**后**（文档序最后 ⇒ 反转扫描先撞上它）
  //   注：真机里外壳文本**只有**追问（153 / 183 字节），不含答案 ⇒ 建模为「答案节点之后的独立外壳」。
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'cosd-markdown-content', text: SPRING },
    ]},
    { tag: 'DIV', cls: '_answer-ask-wrapper_16z1u_8', text: asks },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('㉔a `_answer-ask-wrapper_*`（推荐追问外壳，文档序最后）剔除 ⇒ 取到真答案（cosd-markdown-content）',
        s.r.found === true && s.r.text === SPRING && s.r.fingerprint === 'div.cosd-markdown-content' &&
        has(s.r.note, '另剔非候选宿主 1 个'));
  check('㉔b 体检串回显「推荐追问」（PW-07o 口径可见）', has(s.r.note, '推荐追问'));
  // ㉔c 不得误伤：真答案容器类名含 `answer`（`answer-markdown`）⇒ 照常 found、无剔除
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'answer-markdown', text: SPRING },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, post2, 'composer-box', { prompt: PROMPT });
  check('㉔c 真答案容器类名含 `answer`（`answer-markdown`）**不被误伤**：found 且无剔除',
        s2.r.found === true && s2.r.text === SPRING && !has(s2.r.note, '另剔非候选宿主'));
}

// ---- ㉕ PW-07p：**提示词派生短句列表**（站侧推荐追问）不算答案（真机 · `T-L19` ernie-web）----
//   * 现场：容器类名**换了**（`_answer-ask-wrapper_16z1u_8` → `ai-index-no-animation-icon`）⇒ 类名词表打不完
//     ⇒ 改用文本形态判：≥3 行 · 每行 ≤ 24 字 · 整串无句末标点 · **每行都与提示词共享 ≥2 字片段**。
//   * 真机提示词 = `P1C 参数驱动：这段文本来自输入框。`（本组用它做提示词，否则四条派生短句无从判定）。
{
  const PROMPT2 = 'P1C 参数驱动：这段文本来自输入框。';
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  // ㉕a：真机 · `T-L19` 形态 —— 追问节点（新类名）在答案**之后**（文档序最后）
  const asks2 = '把这段逻辑精简成一段话\n换个风格重写这段P1C参数驱动描述\n能否再详细描述一下P1C参数驱动的过程\n把P1C参数驱动逻辑改成可直接落地的操作说明';
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'cosd-markdown-content', text: SPRING },
    ]},
    { tag: 'DIV', cls: 'ai-index-no-animation-icon answer-suggest-box', text: asks2 },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, post, 'composer-box', { prompt: PROMPT2 });
  check('㉕a 「提示词派生短句列表」（新类名 `ai-index-no-animation-icon`）不算答案 ⇒ 取到真答案',
        s.r.found === true && s.r.text === SPRING && count(s.r.note, '建议列表拒') >= 1);
  // ㉕b 不得误伤①：同样是多行短句，但**含一个句末标点** ⇒ 判正文（照常 found、建议列表拒 0）
  const ANS_PUNCT = 'P1C 参数驱动\n这段文本来自输入框\n它驱动着整条渲染链路，直到最后一帧。';
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'cosd-markdown-content', text: ANS_PUNCT },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, post2, 'composer-box', { prompt: PROMPT2 });
  check('㉕b 多行短句里只要有**一个句末标点** ⇒ 判正文（found 且建议列表拒 0）',
        s2.r.found === true && s2.r.text === ANS_PUNCT && count(s2.r.note, '建议列表拒') === 0);
  // ㉕c 不得误伤②：多行短句、无标点，但**与提示词无共享片段**（如诗行）⇒ 照常 found
  const ANS_POEM = '山里有座庙\n庙里有个老和尚\n老和尚在扫落叶';
  const post3 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'cosd-markdown-content', text: ANS_POEM },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s3 = scenario(pre, post3, 'composer-box', { prompt: PROMPT2 });
  check('㉕c 多行短句无标点但与提示词**无共享片段**（诗行）⇒ 照常 found、建议列表拒 0',
        s3.r.found === true && s3.r.text === ANS_POEM && count(s3.r.note, '建议列表拒') === 0);
  // ㉕d 真机 · `T-L22` 形态：第 4 条 `用日常产品视角写一段完整故事` **不含**提示词片段
  //   ⇒ ④ 若仍要求「每行」，此形态会漏网（真机就是这样又漏了一次）⇒ 放宽为「≥ 半数行」后必须拒。
  //   用**真机整条提示词**（含模板 `请根据以下内容续写：`）—— 站侧追问正是围着「续写」派生的。
  const PROMPT3 = '请根据以下内容续写： P1C 参数驱动：这段文本来自输入框。';
  const asks22 = '能否再续写一段\n换个风格续写这段\n用轻科幻风格续写一段完整场景\n用日常产品视角写一段完整故事';
  const post4 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'cosd-markdown-content', text: SPRING },
    ]},
    { tag: 'DIV', cls: 'ai-index-no-animation-icon answer-suggest-box', text: asks22 },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s4 = scenario(pre, post4, 'composer-box', { prompt: PROMPT3 });
  check('㉕d `T-L22` 追问形态（4 行里 3 行含提示词片段）也不 found（④ 放宽为「≥ 半数行」）',
        s4.r.found === true && s4.r.text === SPRING && count(s4.r.note, '建议列表拒') >= 1);
}

// ---- ㉖ PW-07q：**外壳容器**（内含其他候选）不算候选（真机 · `T-L22` gemini-web）----
//   * 现场：取到 `infinite-scroller.chat-history`（436 字节 = 历史各轮 `You said … / Gemini said …` + 本轮），
//     而本轮真答案是它内部的 `#model-response-message-content*`。外壳之所以中选：内层**上一轮**答案
//     文本早已在基线里（`is_new` 为假 ⇒ 跳过）、内层**本轮**答案节点当时还空 ⇒ 只有外壳的文本是「新」的。
//   * 判据 = 池内**包含关系**（`el.contains(other候选)`）⇒ 外壳不算候选 ⇒ 继续轮询等内层真答案渲染。
{
  const SHELL = 'infinite-scroller chat-history chat-content-container';
  const PREV = '庙里有个老和尚，正在给小和尚讲故事：';
  const NEW_USER = 'You said 请根据以下内容续写： P1C 参数驱动：这段文本来自输入框。';
  // 发送前：外壳里只有**上一轮**答案（⇒ 其文本进基线）；本轮答案节点已存在但为空
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: SHELL, kids: [
      { tag: 'DIV', cls: 'markdown', text: PREV },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  // ㉖a 真机形态：本轮答案**尚未渲染**（空节点）+ 外壳里新长出「本轮用户消息」⇒ 旧口径会把外壳当答案
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: SHELL, kids: [
      { tag: 'DIV', cls: 'markdown', text: PREV },
      { tag: 'DIV', cls: 'user-query-text', text: NEW_USER },
      { tag: 'DIV', cls: 'model-response-message-content', text: '' },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('㉖a `chat-history` 外壳（内含其他候选；本轮答案未渲染）不算答案 ⇒ 继续轮询（found=false）',
        s.r.found === false && count(s.r.note, '外壳剔') >= 1);
  // ㉖b 内层本轮答案已渲染 ⇒ 取内层真答案（外壳不参与）
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: SHELL, kids: [
      { tag: 'DIV', cls: 'markdown', text: PREV },
      { tag: 'DIV', cls: 'user-query-text', text: NEW_USER },
      { tag: 'DIV', cls: 'model-response-message-content', text: SPRING },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, post2, 'composer-box', { prompt: PROMPT });
  check('㉖b 内层本轮答案已渲染 ⇒ 取内层真答案（产物 = 内层文本，外壳不参与）',
        s2.r.found === true && s2.r.text === SPRING && !has(s2.r.text, 'You said'));
  // ㉖c 不得误伤：**平铺**（无包含关系）的多候选布局照常按「文档序最后」取（规则只管外壳）
  const post3 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: PREV },
    ]},
    { tag: 'DIV', cls: 'ai-index-answer-body', text: SPRING },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s3 = scenario(pre, post3, 'composer-box', { prompt: PROMPT });
  check('㉖c 平铺布局（无包含关系）**不被误伤**：照常取文档序最后的新候选',
        s3.r.found === true && s3.r.text === SPRING);
}

// ---- ㉗ PW-07r：**行内元素**（`code` / `span`）不算候选（真机 · `T-L24` yuanbao-web）----
//   * 现场：取到 `code.hyc-common-markdown__code__inline`（**13 字节** `mode="悬疑"`）—— 行内代码片段也带
//     `markdown` 字样类名 ⇒ 被池误吞，且它是文档序**最后**候选 ⇒ 外层真答案落选（三口径判「可用」）。
//   * 判据 = 元素形态（块级 vs 行内）：`code` / `span` 只承载片段。
{
  const pre = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  // ㉗a：真机形态 —— 块级真答案在前，行内 `code` 片段在**后**（文档序最后）
  const post = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'hyc-common-markdown', text: SPRING, kids: [
        { tag: 'CODE', cls: 'hyc-common-markdown__code__inline', text: 'mode="悬疑"' },
      ]},
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s = scenario(pre, post, 'composer-box', { prompt: PROMPT });
  check('㉗a 行内 `code.*__inline`（13 字节片段，文档序最后）不算候选 ⇒ 取到**块级真答案全文**',
        s.r.found === true && s.r.fingerprint === 'div.hyc-common-markdown' &&
        s.r.text.indexOf(SPRING) === 0 && has(s.r.text, 'mode="悬疑"') && has(s.r.note, '另剔非候选宿主 1 个'));
  // ㉗b 行内 `span` 同理（气泡 / 标签族）
  const post2 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container', kids: [
      { tag: 'DIV', cls: 'markdown', text: SPRING },
      { tag: 'SPAN', cls: 'message-badge', text: '刚刚 · 共 3 条消息' },
    ]},
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s2 = scenario(pre, post2, 'composer-box', { prompt: PROMPT });
  check('㉗b 行内 `span`（`message-badge`）不算候选 ⇒ 取到块级真答案',
        s2.r.found === true && s2.r.text === SPRING);
  // ㉗c 不得误伤：`li[role="listitem"]`（消息项）仍在池内 ⇒ 照常 found
  const post3 = { tag: 'BODY', kids: [
    { tag: 'DIV', cls: 'chat-content-container' },
    { tag: 'LI', cls: 'message-item', attrs: { role: 'listitem' }, text: SPRING },
    { tag: 'DIV', cls: 'composer-box', attrs: { contenteditable: 'true' } },
  ]};
  const s3 = scenario(pre, post3, 'composer-box', { prompt: PROMPT });
  check('㉗c `li[role="listitem"]`（消息项）**不被误伤**：照常 found',
        s3.r.found === true && s3.r.text === SPRING);
}

console.log('PW-07 / PW-07g / PW-07i 答案归属与文本保真夹具：' + pass + ' / ' + fail +
            (OLD_MODE ? '（负向模式 --old：①②③b⑯⑰⑲a/b⑳a/b㉑a/b/d/e㉒a㉓a㉔a㉕a㉕d㉖a㉗a㉗b 失败即**预期**；㉕b㉕c㉖b㉖c 亦可能失败 —— ② 产物归一 + 各门移除注入，多行文本必然对不上 —— 证明夹具能捕获回归）' : ''));
process.exit(fail === 0 ? 0 : 1);


