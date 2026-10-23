#pragma once

// ============================================================================
//  DOM 适配器脚本模板（一）—— **模块内**(ARCH-Z1b)
//
//  * 只允许 `ai/src/dom_web_client*.cpp` 自己 include；**不是**公开 API
//  * 逐字搬迁自原 `dom_web_client.cpp:21-448`（常量表 + kickoff/prepare/send-verify/click 脚本）
//  * `constexpr` ⇒ 内部链接，各 TU 各持一份（只读常量表，规则 6 合法形态）；
//    公开渲染入口仍是 `dom_web_client.h` 的 `domKickoffScript()` … `domClickSendScript()`
// ============================================================================

namespace aiwrite::ai {



// 轮询参数硬上限（R13：站点字段填错 / 打错也不至于无限等待）
constexpr int kMinPollMs    = 200;
constexpr int kMaxPollMs    = 2000;
constexpr int kMinPolls     = 10;
constexpr int kMaxPolls     = 600;
constexpr int kStableRounds = 3; // 未配 done_when 时：文本连续 N 轮不变即完成

// ---- 批 3 串行化（2026-10-17 R2-navfix；2026-10-18 `D-35④` 修订根因）----
//  * 9/10 站「控制器未就绪」的真正根因：**跨窗口线程复用同一 environment**（WebView2 线程
//    亲和违反 ⇒ `CreateCoreWebView2Controller` 同步 0x802A000C），已在 webview_host.cpp
//    改为每窗口线程自建环境。此处仍保留「每站关窗 + 避让」：同 profile 串行 + 给上一站
//    浏览器进程释放留时间（新环境创建前旧浏览器可能仍在退出，见 `D-35③` / `kReopenSettleMs`）。
constexpr int kProbeSiteSettleMs = 800;

// ---- M8-37（零开关）：运行期**自动识别**的站点无关口径 ----
//  * 缺 input_selector / answer_selector 时不再拦路：脚本先试声明值，未命中就自动识别
//  * 答案容器**不猜类名**：发送前记录候选池基线，发送后取「新出现 / 文本增长」者
//    （文档顺序**最后**且足够长 ⇒ 天然是回答，不是用户自己那条回声）
constexpr const char* kAnswerPoolSelector =
    "[class*=\"markdown\"], [class*=\"message\"], [class*=\"answer\"], [class*=\"reply\"], "
    "[class*=\"chat-content\"], [class*=\"response\"], [class*=\"bubble\"], "
    "[data-testid*=\"message\"], [data-message-author-role], article, [role=\"listitem\"]";
// PW-07j（2026-10-10 · 真机驱动 · `T-L12` gemini-web 实证）：**工具提示 / 屏读宿主**排除（**结构判据**）
//  * 真机反例：`gemini-web` 命中的是 `div.cdk-describedby-message-container` —— Angular CDK 的
//    **工具提示宿主**（`Dictate (^⇧D)` / `Close sidebar` / `Copy prompt` / `Stop response` 这些
//    `aria-describedby` 文案的容器），只因类名含 `message` 被池里 `[class*="message"]` **误吞**，
//    且它是文档顺序**最后**的候选 ⇒ 被当成答案（两轮 **77 / 92 字节**「假成功」，日志物证：
//    `答案容器**自动识别** div.cdk-describedby-message-container … 候选池 38 个`）。
//  * 判据 = **节点身份**（不是文本）：`describedby`（CDK 工具提示宿主）/ `tooltip` / `sr-only` /
//    `visually-hidden`（屏读专用文案）⇒ 这些节点**根本不算候选**（池内**剔除自己**，不看祖先）。
//  * 与 `PW-07l`「不做类名词表」的区别：那条禁的是**布局状态类**（`has-sidebar` 一类会误伤整页）；
//    这里是**工具提示 / 屏读专用**词，且只剔自己 ⇒ 不碰回答容器（钉子用例见夹具 ⑳）。
constexpr const char* kAnswerPoolExclude =
    "[class*=\"describedby\"], [id*=\"describedby\"], [class*=\"tooltip\"], [role=\"tooltip\"], "
    "[class*=\"sr-only\"], [class*=\"visually-hidden\"], "
    // PW-07k（2026-10-10 · 真机驱动 · `T-L13` gemini-web）：**本方消息**容器不算候选 ——
    //   剔掉 CDK 描述容器后，真机紧接着取到 `span.user-query-bubble-with-background`（**自己那条**
    //   `You said <提示词>`，172 字节）。对方回答的容器是 `model-response` / `message-content` 一族，
    //   与 `user-query` / `query-text` / `user-message` 这些**本方**专属词互斥 ⇒ 剔自己式排除安全。
    "[class*=\"user-query\"], [class*=\"query-text\"], [class*=\"user-message\"], [class*=\"query-bubble\"], "
    // PW-07o（2026-10-10 · 真机驱动 · `T-L18` ernie-web）：**站侧推荐追问**容器不算候选 ——
    //   真机反例：`div._answer-ask-wrapper_16z1u_8`（文心的 CSS-module 类）文本 = 4 条「用代码模拟… /
    //   把这段描述精简成… / 用幽默风格重新描述… / 能否再续写一段…」（183 字节）⇒ 被当答案；
    //   而真答案是它**内部**的 `div.cosd-markdown-content`（同池内、文档序在后）⇒ 剔掉 `ask` 追问外壳，
    //   内层真答案照常参与（夹具 ㉔ 钉子）。`ask-wrapper` 是**外壳身份**，回答容器不会叫这个名字。
    "[class*=\"ask-wrapper\"], [class*=\"ask_wrapper\"], "
    // PW-07r（2026-10-10 · 真机驱动 · `T-L24` yuanbao-web）：**行内元素**不算候选 ——
    //   真机反例：取到 `code.hyc-common-markdown__code__inline`（**13 字节** `mode=悬疑`，答案正文里的
    //   行内代码片段）—— 行内节点也带 markdown 字样类名 ⇒ 被池里的 markdown 类误吞，且它是
    //   文档序**最后**的候选 ⇒ 真答案（外层块级容器）反而落选。
    //   判据 = **元素形态**：答案容器是**块级**（`div` / `article` / `section`…），`code` / `span` 只承载
    //   **片段**（行内代码、气泡、标签）。注：`li[role=listitem]` 不在剔除之列（消息项仍照常参与）。
    "code, span";
constexpr const char* kInputPoolSelector =
    "textarea, input[type=\"text\"], input[type=\"search\"], input:not([type]), "
    "[contenteditable=\"true\"], [contenteditable=\"\"], [role=\"textbox\"]";
constexpr int kMinAutoAnswerChars = 8; // 自动识别的答案最小长度（更短的「新节点」多为包裹 / 装饰）

// ---- PW-07（真机驱动 · 精简版 · 2026-10-08）：答案**装饰 / 布局文本**排除（`AW-04` 反例的根因修）----
//  * 真机证据（同形两次）：差分判据（不在基线 + ≥ min_chars + 文档顺序最后）会命中**不是回答**的新节点：
//    ① 豆包：布局容器 `div.bp5-overflow-list`，整串文本 = 时间条「今天 13:31」⇒ 输出 **12 字节**「假成功」
//       （`docs/actionPlan/M8.md` §9.6 · `imgTest` 运行 1；2026-10-08 13:31 复现 —— 时间戳被当成答案写进
//       `n8(VLMGenerate)` 与 `n2(PromptTemplate)` ⇒ 下游 `n3` 只能回答「你只给了个时间点」）；
//    ② Kimi：用户消息操作条（整串 = `Copy Share`，**10 字节**）—— 同族。
//  * 判据 = **整串形态**（`^(…)$` 全匹配）：纯时间戳 / 纯日期 / 纯 UI 控件词 ⇒ 该候选**不算答案**
//    （继续轮询；超时则如实报错 —— `R13`：宁可失败，不许假成功）。
//  * **不**做子串匹配、**不**做类名词表（`sidebar` / `overflow` 一类词表已被真机证伪：Kimi 应用根带**状态类**
//    `has-sidebar` ⇒ 整页被判「侧栏内」⇒ 真答案全被误伤，见 `M_patchC` §6 `PW-07l`）—— 只有「整串就是装饰」
//    才拒，故真答案里出现「今天」/「复制」**不受影响**（`AW-04` 的「不得误伤」侧）。
constexpr const char* kAnswerChromeTextRe =
    "^(?:(?:今天|昨天|前天|刚刚|上午|中午|下午|晚上|凌晨)\\s*)?\\d{1,2}[:：]\\d{2}(?:[:：]\\d{2})?$" // 13:31 / 今天 13:31
    "|^(?:今天|昨天|前天|刚刚|上午|中午|下午|晚上|凌晨)$"                                            // 纯日期词
    "|^\\d{4}\\s*[-/年]\\s*\\d{1,2}\\s*[-/月]\\s*\\d{1,2}\\s*日?(?:\\s*\\d{1,2}[:：]\\d{2})?$"       // 2026-10-08 / 2026年10月8日 13:31
    "|^\\d{1,2}\\s*[-/月]\\s*\\d{1,2}\\s*日?(?:\\s*\\d{1,2}[:：]\\d{2})?$"                           // 10/8 / 10月8日
    "|^(?:Copy|Share|Retry|Regenerate|Edit|Delete|Like|Dislike|Stop|Continue)"
    "(?:\\s+(?:Copy|Share|Retry|Regenerate|Edit|Delete|Like|Dislike|Stop|Continue))*$"            // Copy Share
    "|^(?:复制|分享|重试|重新生成|重新回答|编辑|删除|赞同|反对|停止|继续|朗读)"
    "(?:\\s*(?:复制|分享|重试|重新生成|重新回答|编辑|删除|赞同|反对|停止|继续|朗读))*$";          // 复制 分享

// PW-07i（2026-10-10 · 真机驱动 · `T-L11` gemini-web 实证）：**chrome-only 门**（与上面的「整串门」互补）
//  * 真机反例：`gemini-web` 自动识别把**操作条 / 工具提示的拼接**当答案 ——
//    `Dictate (^⇧D)Close sidebarImagesSettingsEdit promptCopy promptCopy responseMoreRedo`（**77 字节**、
//    写进 `n3` 与 `n4`，被三口径判「可用」⇒ **假成功**）。整串门抓不住它：它**不等于**任一个控件词，
//    而是**多个**控件词的拼接（`Copy prompt` + `Copy response` 这类**每轮回答都有**的操作按钮文本）。
//  * 口径（**只拒、不猜**）：把已知控件词**逐词剥离**（大小写无关），再抹掉空白 / 标点 / 符号，
//    残余若 `< answer_min_chars` ⇒ 该候选**不算答案**（继续轮询；超时如实失败 —— `R13`）。
//    ⇒ 真答案**不可能**被误伤：残余必然 ≥ `answer_min_chars`（与上面的整串门同一「不得误伤」侧，`AW-04`）。
//  * 与 `PW-07l`「不做类名词表」的区别：那条禁的是**节点类名**词表（`sidebar` 一类状态类会误伤整页）；
//    这里剥的是**可见文本里的控件词**，且**只用于「残余近空」判否**，不做节点归属判断。
constexpr const char* kAnswerChromeWordsRe =
    "Dictate\\s*\\([^)]*\\)|Close sidebar|Open sidebar|Edit prompt|Copy prompt|Copy response|Copy code|"
    "Regenerate response|Stop response|Stop generating|See plans and pricing|Temporary chat|Voice mode|Attach files|"
    "New chat|Deep research|Sign up|Log in|Log out|Share|Redo|Undo|Retry|Regenerate|Delete|Dislike|Like|"
    "Download|Upload|Settings|Images|Sidebar|Search|Send|Microphone|More|Help|Copy|Stop|"
    "侧栏|设置|图片|复制|分享|重试|重新生成|重新回答|朗读|编辑|删除|赞同|反对|停止生成|停止|新建对话|"
    "登录|注册|退出|下载|上传|发送|麦克风|更多|帮助";

// 页面内「写入提示词 + 触发发送」（读 window.__aiwriteDom；返回诊断对象）
//  M8-37（零开关）：声明值未命中 → **自动识别**输入框 / 发送按钮；并在发送前记录答案基线
constexpr const char* kKickoffScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const out = { ok: false, error: '', input_selector: '', input_hits: 0, input_tag: '',
                input_auto: false, input_fingerprint: '', send_kind: '', send_value: '',
                send_hits: 0, send_auto: false, baseline: 0 };
  const norm = function (s) { return (s || '').replace(/\s+/g, ' ').trim(); };
  const visible = function (el) {
    try { const r = el.getBoundingClientRect(); return r.width > 1 && r.height > 1; } catch (e) { return false; }
  };
  const fingerprint = function (el) {
    const cls = (typeof el.className === 'string') ? el.className.replace(/\s+/g, ' ').trim() : '';
    const id = el.id || '';
    const first = cls.split(' ')[0] || '';
    return el.tagName.toLowerCase() + (id ? ('#' + id) : (first ? ('.' + first) : ''));
  };
  // 输入框打分（自动识别用）：可见 + 靠视口下半部 + 有 placeholder 者优先
  const score_input = function (el) {
    let score = 0;
    if (visible(el)) { score += 4; }
    const r = el.getBoundingClientRect();
    if (r.top > (window.innerHeight || 0) * 0.35) { score += 3; }
    if (el.getAttribute('placeholder')) { score += 1; }
    if (el.disabled === true || el.readOnly === true) { score -= 6; }
    return score;
  };
  try {
    // ---- 1) 输入框：**声明值优先**；未命中 → 自动识别（M8-37：缺字段不再拦路）----
    const declared = cfg.input_selector || '';
    let el = null;
    if (declared) {
      out.input_hits = document.querySelectorAll(declared).length;
      el = document.querySelector(declared);
    }
    const input_pool_sel = cfg.input_pool || 'textarea, [contenteditable="true"], [role="textbox"]';
    if (!el) {
      let best = null;
      let best_score = -99;
      document.querySelectorAll(input_pool_sel).forEach(function (cand) {
        const score = score_input(cand);
        if (score >= best_score) { best_score = score; best = cand; } // 同分取文档顺序靠后者
      });
      if (best) { el = best; out.input_auto = true; }
    }
    if (!el) {
      out.error = '输入框未命中（声明值：' + (declared || '（空）') + '；页面候选 ' +
                  document.querySelectorAll(input_pool_sel).length +
                  ' 个）且自动识别未取到候选；可用 --web-dom-dump --provider <id> 取建议选择器';
      return out;
    }
    out.input_tag = (el.tagName || '').toLowerCase();
    out.input_selector = out.input_auto ? fingerprint(el) : declared;
    out.input_fingerprint = fingerprint(el);
    if (out.input_hits === 0) { out.input_hits = 1; }

    // ---- 2) 答案基线（**发送前**快照）：自动识别靠「新增节点差分」，不猜类名 ----
    window.__aiwriteDomRuntime = { composer: el, baseline: [], at: Date.now() };
    try {
      const pool_sel = cfg.answer_pool || '';
      const base = [];
      if (pool_sel) {
        document.querySelectorAll(pool_sel).forEach(function (cand) {
          base.push(norm(cand.innerText || cand.textContent || ''));
        });
      }
      window.__aiwriteDomRuntime.baseline = base;
      out.baseline = base.length;
    } catch (e) { }

    // ---- 3) 写入提示词 ----
    el.focus();
    const text = cfg.prompt || '';
    if (el.isContentEditable) {
      let inserted = false;
      try { inserted = document.execCommand('insertText', false, text); } catch (e) { inserted = false; }
      if (!inserted || !el.textContent) { el.textContent = text; }
      el.dispatchEvent(new InputEvent('input', { bubbles: true, cancelable: true, data: text,
                                                 inputType: 'insertText' }));
    } else {
      const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
      const desc = Object.getOwnPropertyDescriptor(proto, 'value');
      if (desc && desc.set) { desc.set.call(el, text); } else { el.value = text; }
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      if (el.value !== text) { out.error = 'input 写入失败（站点可能拦截脚本输入）'; return out; }
    }

    // ---- 4) 触发发送：**声明值优先**；click 未命中 → 自动找一个可见发送按钮（M8-37）----
    const kind = cfg.send_kind || 'key';
    out.send_kind = kind;
    out.send_value = cfg.send_value || (kind === 'click' ? '' : 'Enter');
    if (kind === 'click') {
      let btn = out.send_value ? document.querySelector(out.send_value) : null;
      if (!btn) {
        const pool = document.querySelectorAll('button, [role="button"], [type="submit"], [class*="send"]');
        for (let i = 0; i < pool.length; i++) {
          if (visible(pool[i]) && pool[i].disabled !== true) {
            btn = pool[i]; out.send_auto = true; out.send_value = fingerprint(pool[i]); break;
          }
        }
      }
      if (!btn) { out.error = 'send.selector 未命中且未找到可点的发送按钮：' + (out.send_value || '（空）'); return out; }
      btn.click();
      out.send_hits = 1;
    } else {
      const key = out.send_value || 'Enter';
      const init = { key: key, code: key, keyCode: key === 'Enter' ? 13 : 0,
                     which: key === 'Enter' ? 13 : 0, bubbles: true, cancelable: true, composed: true };
      ['keydown', 'keypress', 'keyup'].forEach(function (t) { el.dispatchEvent(new KeyboardEvent(t, init)); });
      out.send_hits = 1;
    }
    out.ok = true;
  } catch (e) { out.error = 'DOM 执行异常：' + (e && e.message ? e.message : String(e)); }
  return out;
})();
)JS";

// ============================================================================
//  M_patchC（`PW-07f` · 2026-10-08 · **自 `wip/pw07n-20261008` 移植回主分支**）
//  真实发送链路 = 三步（`kKickoffScript` **降级**为合成事件回退）
//
//  真机证据（2026-10-08）：豆包纯文本路径连续三次 `候选池 0 个`，且运行后页面 URL 仍停在
//  `https://www.doubao.com/chat/`（**没有新会话**）⇒ 消息**根本没发出去**；同一站点图片路径
//  却「能用」（`候选池 39 → 47`）—— 因为图片走 CDP `DOM.setFileInputFiles`（**可信**）先落进
//  composer，合成回车才有内容可发。可见症结 = `kKickoffScript` 的**合成事件**写法：
//  `execCommand('insertText')` / `el.textContent =` 直接改 DOM，`new KeyboardEvent('keydown')`
//  的 `isTrusted=false` —— 豆包（tiptap / ProseMirror）用自己的 state 重渲染 ⇒ 文本被抹掉、
//  回车被忽略，而旧代码 `out.ok = true` **不做任何校验**（假阳性 ⇒ 白等 60 秒才报「未取到答案」）。
//  修正：① 页面脚本只做**发现 + 聚焦 + 清空 + 记基线**；② 文本与回车交给 C++
//  `web::cdp_input_text`（CDP `Input.insertText` / `Input.dispatchKeyEvent`，**isTrusted=true**）；
//  ③ 用回执脚本**验证确实发出去**（URL 变化 / 出现新消息节点），未生效则依次降级
//  （合成事件 → 点可见发送按钮）并**如实报错**（`R13`：宁可失败，不许假成功）。
// ============================================================================

// ① 准备：与 `kKickoffScript` 的「发现输入框 + 记基线」同源，另外**聚焦 + 清空**；**不写入、不发送**
constexpr const char* kPrepareScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const out = { ok: false, error: '', input_selector: '', input_hits: 0, input_tag: '',
                input_auto: false, input_fingerprint: '', baseline: 0, focused: false,
                contenteditable: false, cleared: false };
  const norm = function (s) { return (s || '').replace(/\s+/g, ' ').trim(); };
  const visible = function (el) {
    try { const r = el.getBoundingClientRect(); return r.width > 1 && r.height > 1; } catch (e) { return false; }
  };
  const fingerprint = function (el) {
    const cls = (typeof el.className === 'string') ? el.className.replace(/\s+/g, ' ').trim() : '';
    const id = el.id || '';
    const first = cls.split(' ')[0] || '';
    return el.tagName.toLowerCase() + (id ? ('#' + id) : (first ? ('.' + first) : ''));
  };
  const score_input = function (el) {
    let score = 0;
    if (visible(el)) { score += 4; }
    const r = el.getBoundingClientRect();
    if (r.top > (window.innerHeight || 0) * 0.35) { score += 3; }
    if (el.getAttribute('placeholder')) { score += 1; }
    if (el.disabled === true || el.readOnly === true) { score -= 6; }
    return score;
  };
  try {
    const declared = cfg.input_selector || '';
    let el = null;
    if (declared) {
      out.input_hits = document.querySelectorAll(declared).length;
      el = document.querySelector(declared);
    }
    const input_pool_sel = cfg.input_pool || 'textarea, [contenteditable="true"], [role="textbox"]';
    if (!el) {
      let best = null;
      let best_score = -99;
      document.querySelectorAll(input_pool_sel).forEach(function (cand) {
        const score = score_input(cand);
        if (score >= best_score) { best_score = score; best = cand; } // 同分取文档顺序靠后者
      });
      if (best) { el = best; out.input_auto = true; }
    }
    if (!el) {
      out.error = '输入框未命中（声明值：' + (declared || '（空）') + '；页面候选 ' +
                  document.querySelectorAll(input_pool_sel).length +
                  ' 个）且自动识别未取到候选；可用 --web-dom-dump --provider <id> 取建议选择器';
      return out;
    }
    out.input_tag = (el.tagName || '').toLowerCase();
    out.contenteditable = el.isContentEditable === true;
    out.input_selector = out.input_auto ? fingerprint(el) : declared;
    out.input_fingerprint = fingerprint(el);
    if (out.input_hits === 0) { out.input_hits = 1; }

    // 答案基线（**发送前**快照）+ 发送回执锚点（URL / 基线节点打标）
    window.__aiwriteDomRuntime = { composer: el, baseline: [], seen: {}, seq: 0,
                                   at: Date.now(), url_before: location.href };
    try {
      const pool_sel = cfg.answer_pool || '';
      const base = [];
      if (pool_sel) {
        document.querySelectorAll(pool_sel).forEach(function (cand) {
          try { cand.__aiwritePreSend = true; } catch (e) { }
          base.push(norm(cand.innerText || cand.textContent || ''));
        });
      }
      window.__aiwriteDomRuntime.baseline = base;
      out.baseline = base.length;
    } catch (e) { }

    // 聚焦（CDP 真实输入要求编辑区拿到焦点；**不写入**）
    try { el.focus(); } catch (e) { }
    try { out.focused = (document.activeElement === el); } catch (e) { out.focused = false; }
    // 清空残留（上一次失败尝试可能留下半截文本 ⇒ 不清会**重复发送**）
    //  * 只在**确实聚焦**时做：`execCommand('selectAll')` 选的是**当前焦点**范围，
    //    未聚焦时会选中整页 ⇒ 后续 delete 会破坏页面（保守做法：宁可不清晰，交给回执判定）
    if (out.focused) {
      if (out.contenteditable) {
        try {
          if ((el.innerText || el.textContent || '') !== '') {
            document.execCommand('selectAll', false, null);
            document.execCommand('delete', false, null);
          }
          out.cleared = ((el.innerText || el.textContent || '') === '');
        } catch (e) { out.cleared = false; }
      } else {
        try { if (typeof el.value === 'string' && el.value !== '') { el.value = ''; } } catch (e) { }
        out.cleared = true;
      }
    }
    out.ok = true;
  } catch (e) { out.error = 'DOM 执行异常：' + (e && e.message ? e.message : String(e)); }
  return out;
})();
)JS";
// ---- PW-07f① 准备脚本结束（下面接 ② 发送回执 / ③ 兜底点击）----

// ② 发送回执：URL 变化 / 出现新消息节点 ⇒ 判定「确实发出去了」（否则如实判「未生效」）
//  * 为什么需要它：豆包 / Kimi 这类站点**静默忽略**不可信输入 ⇒ 旧代码「写完就当成功」会白等 60 秒
//  * 锚点：`kPrepareScript` 写下的 `url_before` + 给基线候选打的 `__aiwritePreSend` 标
constexpr const char* kSendVerifyScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const rt = window.__aiwriteDomRuntime || {};
  const out = { sent: false, url_changed: false, new_nodes: 0, composer_len: -1,
                composer_has_prompt: false, url: '', note: '' };
  const norm = function (s) { return (s || '').replace(/\s+/g, ' ').trim(); };
  const squash = function (s) {
    return (s || '').replace(/\s+/g, '').replace(/[\u201c\u201d\u2018\u2019]/g, '"');
  };
  try {
    out.url = location.href;
    out.url_changed = !!rt.url_before && out.url !== rt.url_before;
    const composer = rt.composer ||
      (cfg.input_selector ? document.querySelector(cfg.input_selector) : null);
    if (composer) {
      const text = composer.isContentEditable
        ? (composer.innerText || composer.textContent || '')
        : (composer.value || '');
      out.composer_len = norm(text).length;
      const pk = squash(cfg.prompt || '');
      out.composer_has_prompt = pk !== '' && squash(text).indexOf(pk) === 0;
    }
    const pool_sel = cfg.answer_pool || '';
    if (pool_sel) {
      document.querySelectorAll(pool_sel).forEach(function (el) {
        if (el.__aiwritePreSend === true) { return; } // 发送前就有 ⇒ 不是本轮新消息
        const t = norm(el.innerText || el.textContent || '');
        if (t.length < 2) { return; }
        if (composer && (el === composer || el.contains(composer))) { return; } // 输入区不算
        out.new_nodes += 1;
      });
    }
    out.sent = out.url_changed || out.new_nodes > 0;
    out.note = 'URL' + (out.url_changed ? '已变化' : '未变化') + ' / 新节点 ' + out.new_nodes +
               ' / 输入框 ' + out.composer_len + ' 字' + (out.composer_has_prompt ? '（含提示词）' : '');
  } catch (e) { out.note = 'DOM 执行异常：' + (e && e.message ? e.message : String(e)); }
  return out;
})();
)JS";
// ---- PW-07f② 发送回执脚本结束（下面接 ③ 兜底点击）----

// ③ 兜底：找可见发送按钮并点击（CDP 回车无效时；优先「send / 发送」类名 / aria）
//  * 用途：`web.send.kind = click` 的站点；以及 key 模式下回执仍显示「未发出」时的最后一招
constexpr const char* kClickSendScript = R"JS(
(function () {
  const cfg = window.__aiwriteDom || {};
  const out = { clicked: false, selector: '', candidates: 0, declared: false, note: '' };
  const visible = function (el) {
    try { const r = el.getBoundingClientRect(); return r.width > 1 && r.height > 1; } catch (e) { return false; }
  };
  const fingerprint = function (el) {
    const cls = (typeof el.className === 'string') ? el.className.replace(/\s+/g, ' ').trim() : '';
    const id = el.id || '';
    const first = cls.split(' ')[0] || '';
    return el.tagName.toLowerCase() + (id ? ('#' + id) : (first ? ('.' + first) : ''));
  };
  const looks_send = function (el) {
    const blob = ((el.className || '') + ' ' + (el.getAttribute('aria-label') || '') + ' ' +
                  (el.getAttribute('title') || '') + ' ' + (el.getAttribute('data-testid') || '')).toLowerCase();
    return /send|submit|发送/.test(blob);
  };
  try {
    // 声明值优先（`web.send.value` 命中的站点：直接用声明选择器，避免猜错按钮）
    const declared = cfg.send_value || '';
    if (declared) {
      try {
        const btn = document.querySelector(declared);
        if (btn && visible(btn) && btn.disabled !== true) {
          btn.click();
          out.clicked = true;
          out.declared = true;
          out.selector = declared;
          return out;
        }
      } catch (e) { }
    }
    const pool = document.querySelectorAll('button, [role="button"], [type="submit"]');
    let chosen = null;
    for (let i = pool.length - 1; i >= 0; i--) { // 倒序：发送按钮通常在文档**靠后**的输入区
      if (!visible(pool[i]) || pool[i].disabled === true) { continue; }
      out.candidates += 1;
      if (looks_send(pool[i])) { chosen = pool[i]; break; }
    }
    if (!chosen) { out.note = '未找到「发送」按钮（候选 ' + out.candidates + ' 个）'; return out; }
    chosen.click();
    out.clicked = true;
    out.selector = fingerprint(chosen);
  } catch (e) { out.note = 'DOM 执行异常'; }
  return out;
})();
)JS";
// ---- PW-07f③ 兜底点击脚本结束 ----

} // namespace aiwrite::ai

