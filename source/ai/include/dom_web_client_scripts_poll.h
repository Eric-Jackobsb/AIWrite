#pragma once

// ============================================================================
//  DOM 适配器脚本模板（二）—— **模块内**(ARCH-Z1b)
//
//  * 只允许 `ai/src/dom_web_client*.cpp` 自己 include；**不是**公开 API
//  * 逐字搬迁自原 `dom_web_client.cpp:449-805`（poll / probe / discover 脚本）
//  * `constexpr` ⇒ 内部链接，各 TU 各持一份；公开渲染入口见 `dom_web_client.h`
// ============================================================================

namespace aiwrite::ai {



// 页面内「取答案 + 判结束」（读 window.__aiwriteDom / window.__aiwriteDomRuntime）
//  M8-37（零开关）：声明值未命中 → 自动识别（基线差分；站点无关）
//
//  ---- PW-07g（**文本保真** · 2026-10-08 真机驱动）：`norm` **只服务判据**，产物一律**原样**搬运 ----
//  * 真机反例：豆包回答 `div.container-qX9Csx`（**759 字节**、内容完整）经 `norm` 后写盘 ⇒
//    `4-TextOutput.txt` 只有 **1 行**（LF 全在文件头）—— 段落换行在**页面内**就被 `\s+ → ' '`
//    压成了空格（内容一个字没丢，**分段没了**；`3-LLMGenerate.txt` 里可见「…结果。 我试着…」）。
//  * 口径（用户拍板 · 方案 A）：页面只负责**逐字符搬出来**（保留 `\n` / `\r\n` / 缩进），
//    排版交给 C++ 纯函数 `format_answer_text()`（**离线可断言**：`VB2-22⑦`）。
//  * 判据（基线差分 / 装饰整串门 / 回声排除 / `min_chars`）继续吃 `norm` ⇒ PW-07 的
//    「12 字节假成功」不会因本次改动复活（行为锁：`source/tools/poll_answer_gate_test.js` ⓪①④）。
constexpr const char* kPollScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const rt = window.__aiwriteDomRuntime || {};
  // PW-07h（2026-10-10 · 真机驱动）：轮询回执**带上实时 URL** —— 「明明发出去了却取不到答案」
  //   这类归因全靠它：URL 跳到 `/auth/login` ⇒ 需登录（疑似 chatgpt-web 的形态），
  //   URL 仍是目标站点 ⇒ 站侧没渲染答案。`location` 在 Node 夹具里不存在 ⇒ 安全取空串。
  const href = function () {
    try { return (typeof location !== 'undefined' && location.href) ? location.href : ''; }
    catch (e) { return ''; }
  };
  const out = { found: false, count: 0, chars: 0, lf: 0, text: '', done: false, done_kind: cfg.done_kind || '',
                url: href(),
                ready: (typeof document !== 'undefined' && document.readyState) ? document.readyState : '',
                body_chars: (function () {
                  try { return (document.body && document.body.innerText) ? document.body.innerText.length : 0; }
                  catch (e) { return 0; }
                })(),
                // 「正文很长却 0 候选」时，光看长度分不清**登录墙 / 限额页**还是**正常外壳但没渲染消息**
                //   ⇒ 带回首 80 字（页面自己的可见文本，仅**失败路径**写进 error）—— 一眼定性，不靠猜
                body_head: (function () {
                  try {
                    // 内联归一（`norm` 在本对象**之后**才声明 ⇒ 此处不能引用，否则 TDZ 恒空）
                    const t = String(document.body ? (document.body.innerText || '') : '')
                                  .replace(/\s+/g, ' ').trim();
                    return t.length > 80 ? t.slice(0, 80) : t;
                  } catch (e) { return ''; }
                })(),
                auto: false, answer_selector: cfg.answer_selector || '', fingerprint: '' };
  // PW-07g：**判据专用**归一（`\s+` → 单空格）—— 绝不用于产物（否则段落换行被吃掉）
  const norm = function (s) { return (s || '').replace(/\s+/g, ' ').trim(); };
  // PW-07g：**产物取值** = 原样搬运（不清理）；`lf` = 页面到底给了几个换行（= 分段物证）
  const raw_text = function (el) { return el.innerText || el.textContent || ''; };
  const lf_count = function (s) { const m = String(s).match(/\n/g); return m ? m.length : 0; };
  const fingerprint = function (el) {
    const cls = (typeof el.className === 'string') ? el.className.replace(/\s+/g, ' ').trim() : '';
    const id = el.id || '';
    const first = cls.split(' ')[0] || '';
    return el.tagName.toLowerCase() + (id ? ('#' + id) : (first ? ('.' + first) : ''));
  };
)JS"
// ⚠ MSVC C2026（单字面量 ≤ 16380 字节）：本脚本按**顶层语句边界**切成多个相邻字面量拼接，
//   脚本内容**不变**（切点处正好一个 `\n`，拼接后与未切分时逐字符一致 —— 夹具的抽取器同样支持多段）。
R"JS(
  try {
    // ---- ① 声明值优先（语义不变：**最后**一个匹配节点 = 本轮回答）----
    const sel = cfg.answer_selector || '';
    const nodes = sel ? document.querySelectorAll(sel) : [];
    out.count = nodes.length;
    if (nodes.length > 0) {
      const last = nodes[nodes.length - 1];
      const raw = raw_text(last);                 // PW-07g：原样（含 `\n`）
      out.text = raw;                             //  * 产物**不**归一（段落换行不再被吃掉）
      out.chars = raw.length;
      out.lf = lf_count(raw);                     //  * 页面给的换行数（= 分段物证，进 steps / 日志）
      out.found = norm(raw).length > 0;           //  * 判据仍用归一（空 / 纯空白不算命中）
      out.fingerprint = fingerprint(last);
    }
    // ---- ② 未声明 / 未命中 → 自动识别（发送前基线 → 取「新出现」且足够长的**最后**候选）----
    if (!out.found && cfg.auto_answer !== false) {
      const composer = rt.composer || null;
      const baseline = rt.baseline || [];
      const prompt = norm(cfg.prompt || '');
      // PW-07：装饰 / 布局文本形态编译（**整串**匹配；编译失败 = 该门不生效，不阻断自动识别）
      let chrome_text = null;
      try { chrome_text = cfg.answer_chrome_text ? new RegExp(cfg.answer_chrome_text) : null; } catch (e) { chrome_text = null; }
      // PW-07i（真机驱动 · gemini-web 实证）：**控件词剥离门** —— 抓「**多个**控件词拼接」这一形态
      //   真机反例：`Dictate (^⇧D)Close sidebarImagesSettingsEdit promptCopy promptCopy responseMoreRedo`
      //   （77 字节，写进 n3/n4 ⇒ 假成功）。剥离控件词后残余近空 ⇒ 判**不算答案**（只拒，不猜）。
      let chrome_words = null;
      try { chrome_words = cfg.answer_chrome_words ? new RegExp(cfg.answer_chrome_words, 'gi') : null; } catch (e) { chrome_words = null; }
      const chrome_only = function (t) {
        if (!chrome_words) { return false; }
        const s = String(t);
        const stripped = s.replace(chrome_words, ' ');
        if (stripped === s) { return false; } // **一个控件词都没剥到** ⇒ 不是「控件词拼接」（纯短文本归 `min_chars` 管）
        const rest = stripped.replace(/[\s\p{P}\p{S}]+/gu, '');
        return rest.length < (cfg.answer_min_chars || 8); // 残余近空 ⇒ 拼的是控件词（真答案残余必然够长）
      };
      const refuse = { chrome_text: 0, chrome_only: 0, echo: 0, internal: 0, progress: 0, suggest: 0, shell: 0, short: 0 }; // PW-07：命中明细（计数，进 note ⇒ steps / 日志）
      // PW-07k（真机驱动 · T-L13 gemini-web）：**本方消息回声**门 —— 文本里含提示词、且**围绕它的残余很短**
      //   真机反例：`span.user-query-bubble-with-background` 的 `You said <提示词>`（172 字节，前缀
      //   `You said ` + 整条提示词）⇒ 被当成「新答案」（下游把**自己那条**当回答）。旧口径只拦「整串等于
      //   提示词 / 以提示词开头」⇒ 带前缀的回声漏网。残余 < `min_chars` 判「回声 / 夹带」；真答案残余必然够长。
      const echo_only = function (t) {
        if (!prompt) { return false; }
        const s = String(t);
        if (s.indexOf(prompt) < 0) { return false; }
        // 真机 T-L14 形态：提示词在**回声里出现两次**（`You said <提示词> <提示词>` = 172 字节）
        //   ⇒ 只剥一次会留下整条提示词、残余变长而漏网 ⇒ **剥掉全部出现**再量残余。
        const rest = s.split(prompt).join('').replace(/[\s\p{P}\p{S}]+/gu, '');
        return rest.length < (cfg.answer_min_chars || 8);
      };
      // PW-07m（真机驱动 · T-L17 回归扫描 yuanbao-web）：**站侧内部码**不算答案 ——
      //   真机反例：`CONFLICT_LOG`（**12 字节**）被当答案写进 `n3` / `n4`（元宝把内部诊断节点渲染进了
      //   DOM，且是文档序最后候选）。整串（去空白 / 标点 / 符号后）就是**一个** `SNAKE_CASE` 常量名
      //   ⇒ 判「站侧内部码」（真答案不会是单独一个大写下划线常量；纯代码答案通常多行 / 带围栏）。
      const internal_code_only = function (t) {
        // 注意：`_` 属 Unicode 标点类 `Pc`，用 `[\s\p{P}\p{S}]` 一并剥会把常量名的下划线也吃掉
        //   ⇒ 这里用**白名单**（只留字母 / 数字 / 下划线）再判。
        const s = String(t).replace(/[^\p{L}\p{N}_]+/gu, '');
        if (!s || s.length > 40) { return false; }
        return /^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$/.test(s);
      };
      // PW-07n（真机驱动 · T-L17 回归扫描 ernie-web）：**生成中占位**不算答案（继续轮询）——
      //   真机反例：`P1C参数驱动续写故事 / 正在编写，请稍候... / 0% / 改用对话直接回答`
      //   （站侧**自动标题** + 占位文案 + 进度条）。剥离占位词后残余 = 站侧自动标题（9 字）⇒ 单靠
      //   剥离门（PW-07i）拦不住 ⇒ 用「**两个独立信号同时出现**」判：占位 / 进度短语 **且** 裸百分比。
      //   判据刻意保守（真答案同时出现「请稍候」与「37%」的概率极低 ⇒ 只拒、不猜，超时如实失败）。
      const progress_words = /(正在编写|正在生成|正在思考|正在回答|生成中|思考中|回答中|请稍候|请稍等|改用对话直接回答|Generating|Thinking|Loading)/;
      const progress_only = function (t) {
        const s = String(t);
        if (!/\d{1,3}\s*%/.test(s)) { return false; }
        return progress_words.test(s);
      };
      // PW-07p（真机驱动 · T-L19 ernie-web）：**提示词派生短句列表**（站侧「推荐追问 / 猜你想问」）不算答案 ——
      //   真机两轮反例，且容器类名**换了**（`_answer-ask-wrapper_16z1u_8` → `ai-index-no-animation-icon`）
      //   ⇒ 类名词表**打不完**（站侧一改版本就是新类名）⇒ 改用「**文本形态**」判：
      //     ① `用代码模拟P1C参数驱动流程 / 把这段描述精简成一句话总结核心 / 用幽默风格重新描述P1C参数驱动过程 /
      //        能否再续写一段关于P1C参数驱动的描述`（183 字节）
      //     ② `把这段逻辑精简成一段话 / 换个风格重写这段P1C参数驱动描述 / 能否再详细描述一下P1C参数驱动的过程 /
      //        把P1C参数驱动逻辑改成可直接落地的操作说明`（192 字节）
      //   判据 = **四条同时**成立：① ≥3 行；② 每行 ≤ 24 字；③ 整串**无句末标点**（`。！？；`）；
      //          ④ **≥ 半数行**与提示词共享 ≥2 字片段（= 从提示词派生的短句）。四条**全部**成立才算
      //          ⇒ 正文只要出现一处句末标点即放行（夹具 ㉕b / ㉕c 钉「不得误伤」）。
      //   真机 T-L22 ernie-web 修正（㉕d）：④ 原为「**每行**都含提示词片段」，被站侧第 4 条
      //   `用日常产品视角写一段完整故事`（与提示词无共享片段）绕过 ⇒ 放宽为「**≥ 半数行**」。
      //   代价（如实记录）：极短的「三行无标点清单」式回答会被判失败 —— 取「宁可失败、不许假成功」（`R13`）。
      const prompt_frags = (function () {
        const s = prompt.replace(/[\s\p{P}\p{S}]+/gu, '');
        const out = [];
        for (let i = 0; i + 2 <= s.length; i++) { out.push(s.substr(i, 2)); }
        return out;
      })();
      const suggest_list = function (raw_t) {
        const s = String(raw_t || '');
        if (!s || /[。！？!?；;]/u.test(s)) { return false; } // ③ 有句末标点 ⇒ 正文
        const lines = s.split(/\n+/).map(function (x) { return x.replace(/\s+/g, ''); })
                                .filter(function (x) { return x.length > 0; });
        if (lines.length < 3) { return false; }              // ① ≥3 行
        let derived = 0;
        for (let i = 0; i < lines.length; i++) {
          if (lines[i].length > 24) { return false; }        // ② 每行 ≤ 24 字
          for (let j = 0; j < prompt_frags.length; j++) {    // ④ 行与提示词共享 ≥2 字片段（计「派生行」数）
            if (prompt_frags[j] && lines[i].indexOf(prompt_frags[j]) >= 0) { derived++; break; }
          }
        }
        return derived * 2 >= lines.length;                  // ④ ≥ 半数行是「提示词派生」
      };
)JS"
R"JS(
      const is_new = function (el) {
        const raw_el = raw_text(el);                            // PW-07p 要看**行结构** ⇒ 用原样文本
        const t = norm(raw_el);
        if (t.length < 2) { return false; }
        if (composer && (el === composer || el.contains(composer))) { return false; } // 排除输入区
        if (prompt && (t === prompt || t.indexOf(prompt) === 0)) { return false; }     // 排除自己那条回声
        // PW-07：**装饰 / 布局文本**（整串 = 时间戳 / 日期 / 纯 UI 控件词）不算答案 ⇒ 继续轮询
        //   真机证据：豆包 `div.bp5-overflow-list` 的「今天 13:31」（12 字节）与 Kimi 操作条的「Copy Share」（10 字节）
        if (chrome_text && chrome_text.test(t)) { refuse.chrome_text++; return false; }
        if (chrome_only(t)) { refuse.chrome_only++; return false; } // PW-07i：控件词拼接（gemini-web 反例）
        if (echo_only(t)) { refuse.echo++; return false; }         // PW-07k：本方消息回声（`You said <提示词>`）
        if (internal_code_only(t)) { refuse.internal++; return false; } // PW-07m：站侧内部码（`CONFLICT_LOG`）
        if (progress_only(t)) { refuse.progress++; return false; }     // PW-07n：生成中占位（`正在编写…0%`）
        if (suggest_list(raw_el)) { refuse.suggest++; return false; }   // PW-07p：推荐追问（提示词派生短句列表）
        return baseline.indexOf(t) < 0;
      };
      const pool_sel = cfg.answer_pool || '';
      const pool_excl = cfg.answer_pool_exclude || '';
      let pool = pool_sel ? Array.prototype.slice.call(document.querySelectorAll(pool_sel)) : [];
      // PW-07j（真机驱动 · T-L12 gemini-web）：**工具提示 / 屏读宿主**不算候选（**剔除自己**，不看祖先）
      //   真机反例：`div.cdk-describedby-message-container`（CDK 工具提示宿主）被 `[class*="message"]` 误吞，
      //   又是文档序**最后**的候选 ⇒ 77 / 92 字节「假成功」。剔除后**回答容器照常参与**（夹具 ⑳ 钉子）。
      let excl_n = 0;
      if (pool_excl) {
        pool = pool.filter(function (el) {
          try { if (el.matches && el.matches(pool_excl)) { excl_n++; return false; } } catch (e) { /* 选择器不支持 ⇒ 该节点不剔 */ }
          return true;
        });
      }
      out.count = pool.length; // PW-07：候选池规模**始终**回显（原来只在命中时回显）
      // PW-07q（真机驱动 · T-L22 gemini-web）：**外壳容器**不算候选 —— 「**内含其他候选**」者 = 聊天记录 /
      //   消息外壳（里面装着每条消息节点）。真机反例：取到 `infinite-scroller.chat-history`（436 字节 =
      //   历史各轮的 `You said … / Gemini said …` + 本轮），而本轮真答案是它内部的
      //   `#model-response-message-content*`（同池候选）—— 该次轮询只跑了 4 次 ⇒ 真答案节点当时还空 ⇒
      //   外壳（装着**上一轮**回答）抢先中选（下游拿到的是历史记录，不是本轮答案）。
      //   判据 = **池内包含关系**（看的是「有没有别的候选装在它里面」，与类名 / 文本无关）⇒ 外壳继续轮询，
      //   等内层真答案渲染出来；内层候选照常参与（夹具 ㉖ 钉子：外壳剔 + 内层取到）。
      const inner_of_some = function (el) {
        for (let i = 0; i < pool.length; i++) {
          if (pool[i] !== el && el.contains && el.contains(pool[i])) { return true; }
        }
        return false;
      };
      const min_chars = cfg.answer_min_chars || 8;
      for (let i = pool.length - 1; i >= 0; i--) { // 逆序 = 文档顺序**最后**者优先（回答在用户消息之后）
        if (inner_of_some(pool[i])) { refuse.shell++; continue; } // PW-07q：外壳（内含其他候选）不算候选
        if (!is_new(pool[i])) { continue; }
        const raw = raw_text(pool[i]);             // PW-07g：原样（含 `\n`）
        const t = norm(raw);                       //  * `t` **只判长度**（产物不吃归一）
        if (t.length >= min_chars) {
          out.text = raw; out.chars = raw.length;  //  * 产物 = 原样搬运（段落换行保住）
          out.lf = lf_count(raw);
          out.found = true; out.auto = true;
          out.fingerprint = fingerprint(pool[i]); break;
        }
        // PW-07：**取消**「短候选也算成功」（旧口径正是 12 / 10 字节类假成功的温床）——
        //   短候选只计数（`note` 里如实回显），不算 found ⇒ 继续轮询；超时由 C++ 侧如实报错（`R13`）
        refuse.short++;
      }
      out.note = '自动识别：候选池 ' + pool.length + ' 个'
               + (excl_n > 0 ? ('（另剔非候选宿主 ' + excl_n + ' 个：工具提示 / 屏读 / 本方消息 / 推荐追问 · PW-07j/k/o）') : '')
               + '；装饰文本拒 ' + refuse.chrome_text + '（整串 = 时间戳 / 控件词）'
               + '；控件词拼接拒 ' + refuse.chrome_only + '（PW-07i：剥离后残余近空）'
               + '；回声拒 ' + refuse.echo + '（PW-07k：含提示词且残余近空）'
               + '；内部码拒 ' + refuse.internal + '（PW-07m：整串 = SNAKE_CASE 常量）'
               + '；生成中拒 ' + refuse.progress + '（PW-07n：占位词 + 百分比进度）'
               + '；建议列表拒 ' + refuse.suggest + '（PW-07p：提示词派生短句）'
               + '；外壳剔 ' + refuse.shell + '（PW-07q：内含其他候选）'
               + '；过短拒 ' + refuse.short + '（< ' + min_chars + ' 字符）';
    }
    const dsel = cfg.done_selector || '';
    if (out.done_kind === 'selector_present') { out.done = dsel ? document.querySelectorAll(dsel).length > 0 : false; }
    else if (out.done_kind === 'selector_gone') { out.done = dsel ? document.querySelectorAll(dsel).length === 0 : false; }
  } catch (e) { out.text = ''; out.error = (e && e.message) ? e.message : String(e); }
  return out;
})();
)JS";

// PB2-15：选择器探测（读 window.__aiwriteDomProbe）—— **只读诊断，不发送任何内容**
constexpr const char* kProbeScript = R"JS(
(function () {
  const cfg = window.__aiwriteDomProbe || {};
  const out = { url: location.href, title: document.title || '',
                input_selector: cfg.input_selector || '', input_hits: 0, input_visible: false,
                send_kind: cfg.send_kind || 'key', send_value: cfg.send_value || '', send_hits: 0,
                answer_selector: cfg.answer_selector || '', answer_hits: 0, answer_chars: 0,
                done_kind: cfg.done_kind || '', done_now: null,
                token_expr: cfg.token_expr || '', token_len: -1, token_json: false,
                cookies_wanted: cfg.cookie_names || [], cookies_present: [],
                input_candidates: 0, login_controls: 0, login_evidence: '',
                needs_login: false, page_abnormal: false, error: '' };
  try {
    const q = function (s) { try { return s ? document.querySelectorAll(s) : []; } catch (e) { return []; } };
    const inputs = q(out.input_selector);
    out.input_hits = inputs.length;
    if (inputs.length > 0) {
      const r = inputs[0].getBoundingClientRect();
      out.input_visible = r.width > 0 && r.height > 0;
    }
    out.send_hits = (out.send_kind === 'click') ? q(out.send_value).length : out.input_hits;
    const answers = q(out.answer_selector);
    out.answer_hits = answers.length;
    if (answers.length > 0) { out.answer_chars = (answers[answers.length - 1].innerText || '').length; }
    const dsel = cfg.done_selector || '';
    if (out.done_kind === 'selector_present') { out.done_now = q(dsel).length > 0; }
    else if (out.done_kind === 'selector_gone') { out.done_now = q(dsel).length === 0; }
    if (out.token_expr) {
      try {
        const v = eval(out.token_expr);
        if (typeof v === 'string') {
          out.token_len = v.length;
          const t = v.trim();
          out.token_json = (t.charAt(0) === '{' || t.charAt(0) === '[' || t.charAt(0) === '"');
        } else if (v === null || v === undefined) { out.token_len = -1; }
      } catch (e) { out.token_len = -2; }
    }
    const jar = document.cookie || '';
    (out.cookies_wanted || []).forEach(function (n) {
      if (jar.indexOf(n + '=') >= 0) { out.cookies_present.push(n); }
    });
    // ---- Loop Dev（FEA-L01/L03）：**需登录判据** —— 只用页面事实（DOM），**不看 Cookie** ----
    //  §3.2：① 输入区候选 > 0（或声明值命中）→ 匿名可用；② 候选 = 0 且有登录迹象 → 需强制登录；
    //        ③ 候选 = 0 且无任何登录迹象 → 页面异常（归失败，**不**当成「需登录」）
    //  判据只在本脚本里算一次（C++ 侧只搬运结果）⇒ 夹具抽同一段脚本真跑（TST-L02）
    const cands = q(cfg.input_pool || 'textarea, input[type="text"], input[type="search"], input:not([type]),' +
                    ' [contenteditable="true"], [contenteditable=""], [role="textbox"]');
    out.input_candidates = cands.length;
    const pw = q('input[type="password"]');
    const lg = q('[class*="login"], [class*="signin"], [class*="sign-in"],' +
                 ' [id*="login"], [id*="signin"], [href*="login"], [href*="signin"]');
    out.login_controls = pw.length + lg.length;
    const ev = [];
    if (pw.length > 0) { ev.push('password-input x' + pw.length); }
    if (lg.length > 0) { ev.push('login-node x' + lg.length); }
    const lpath = ((typeof location !== 'undefined' && location)
                     ? ((location.pathname || '') + (location.search || '') + (location.hash || ''))
                     : '').toLowerCase();
    if (/(login|signin|sign-in|passport|signup|register|auth|sso)/.test(lpath)) {
      ev.push('login-url:' + (location.pathname || ''));
    }
    out.login_evidence = ev.join(' + ');
    if (out.input_hits > 0 || out.input_candidates > 0) { out.needs_login = false; }
    else if (ev.length > 0) { out.needs_login = true; }
    else { out.page_abnormal = true; }
  } catch (e) { out.error = (e && e.message) ? e.message : String(e); }
  return out;
})();
)JS";
// M_patchB L4（PB2-29 / v15）：**选择器候选枚举**（只读）—— 让「填选择器」不必靠人肉 F12
//  * 在**当前页面**里枚举候选输入框 / 发送按钮 / 回答容器，回传标签·id·class·placeholder·aria 等指纹
//  * C++ 侧再据此给出**建议选择器**（优先 id → placeholder/aria → tag.class…），配合命中数探测即可逐站回填
//  * 纯只读：不写入、不发送、不读取任何值（与 `kProbeScript` 同族）
constexpr const char* kDiscoverScript = R"JS(
(function () {
  const out = { url: location.href, title: document.title || '', inputs: [], sends: [], answers: [],
                cookies: [], keys: '', input_candidates: 0 };
  const visible = function (el) {
    try { const r = el.getBoundingClientRect(); return r.width > 1 && r.height > 1; } catch (e) { return false; }
  };
  const desc = function (el) {
    const cls = (typeof el.className === 'string') ? el.className : '';
    return {
      tag: el.tagName.toLowerCase(),
      id: el.id || '',
      cls: cls.replace(/\s+/g, ' ').trim().slice(0, 200),
      name: el.getAttribute('name') || '',
      ph: el.getAttribute('placeholder') || '',
      ce: el.getAttribute('contenteditable') || '',
      aria: el.getAttribute('aria-label') || '',
      role: el.getAttribute('role') || '',
      visible: visible(el),
      text: (el.innerText || '').replace(/\s+/g, ' ').trim().slice(0, 40)
    };
  };
  const push = function (list, el, limit) { if (list.length < limit) { list.push(desc(el)); } };
  try {
    const inputs = document.querySelectorAll('textarea, input[type="text"], input[type="search"], input:not([type]), [contenteditable="true"], [contenteditable=""], [role="textbox"]');
    out.input_candidates = inputs.length;
    inputs.forEach(function (el) { push(out.inputs, el, 14); });
    document.querySelectorAll('button, [role="button"], [type="submit"], a[class*="send"], [class*="send"]')
      .forEach(function (el) { push(out.sends, el, 14); });
    document.querySelectorAll('[class*="markdown"], [class*="message"], [class*="answer"], [class*="chat-content"], [class*="response"], article, [role="listitem"]')
      .forEach(function (el) { push(out.answers, el, 14); });
    out.cookies = (document.cookie || '').split(';')
      .map(function (p) { return p.split('=')[0].trim(); })
      .filter(function (n) { return n.length > 0; });
    try { out.keys = Object.keys(localStorage).slice(0, 40).join(','); } catch (e) { out.keys = ''; }
  } catch (e) { out.error = (e && e.message) ? e.message : String(e); }
  return JSON.stringify(out);
})();
)JS";




} // namespace aiwrite::ai

