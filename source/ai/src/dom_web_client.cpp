#include "dom_web_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "log.h"
#include "upload_registry.h" // M8-14：站点上传模板（图片先上传再发送）
#include "webview_host.h"

namespace aiwrite::ai {
namespace {

// 轮询参数硬上限（R13：站点字段填错 / 打错也不至于无限等待）
constexpr int kMinPollMs    = 200;
constexpr int kMaxPollMs    = 2000;
constexpr int kMinPolls     = 10;
constexpr int kMaxPolls     = 600;
constexpr int kStableRounds = 3; // 未配 done_when 时：文本连续 N 轮不变即完成

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



} // namespace

void clamp_poll_params(const ProviderWebSpec& site, int* poll_ms, int* max_polls)
{
    const int interval = site.answer_poll_ms > 0 ? site.answer_poll_ms : 500;
    const int polls    = site.answer_max_polls > 0 ? site.answer_max_polls : 120;
    if (poll_ms != nullptr) {
        *poll_ms = std::clamp(interval, kMinPollMs, kMaxPollMs);
    }
    if (max_polls != nullptr) {
        *max_polls = std::clamp(polls, kMinPolls, kMaxPolls);
    }
}

std::string dom_cfg_json(const DomChatRequest& request)
{
    nlohmann::json cfg;
    cfg["prompt"]          = request.prompt;
    cfg["input_selector"]  = request.site.input_selector;
    cfg["send_kind"]       = request.site.send_kind.empty() ? std::string("key") : request.site.send_kind;
    cfg["send_value"]      = request.site.send_value.empty() ? std::string("Enter") : request.site.send_value;
    cfg["answer_selector"] = request.site.answer_selector;
    cfg["done_kind"]       = request.site.done_kind;
    cfg["done_selector"]   = request.site.done_selector;
    cfg["stable_rounds"]   = kStableRounds;
    // ---- M8-37（零开关）：自动识别（声明值命中时不生效，语义与改造前一致）----
    cfg["auto_input"]       = true;
    cfg["auto_answer"]      = true;
    cfg["input_pool"]       = kInputPoolSelector;
    cfg["answer_pool"]      = kAnswerPoolSelector;
    // PW-07j：**工具提示 / 屏读宿主**从候选池里**剔除自己**（结构判据，真机 gemini-web 命中物）
    cfg["answer_pool_exclude"] = kAnswerPoolExclude;
    cfg["answer_min_chars"] = kMinAutoAnswerChars;
    // PW-07：装饰 / 布局文本形态（**整串**匹配）—— 与候选池同族：仍是**纯数据**，不落盘
    cfg["answer_chrome_text"] = kAnswerChromeTextRe;
    // PW-07i：装饰**控件词**（**逐词剥离**后残余近空 ⇒ 拒）—— 抓「多个控件词拼接」这一形态
    cfg["answer_chrome_words"] = kAnswerChromeWordsRe;
    return cfg.dump();
}

std::string dom_kickoff_script() { return std::string(kKickoffScript); }
// PW-07f：真实发送链路的三段式脚本（与 dom_kickoff_script 同为**常量脚本**读取者）
std::string dom_prepare_script() { return std::string(kPrepareScript); }
std::string dom_send_verify_script() { return std::string(kSendVerifyScript); }
std::string dom_click_send_script() { return std::string(kClickSendScript); }
std::string dom_poll_script() { return std::string(kPollScript); }
std::string dom_probe_script() { return std::string(kProbeScript); }

// ---- PW-07g：答案文本排版（纯函数 · 离线断言 `VB2-22⑦`）--------------------------------
//  为什么放在 C++ 侧：页面脚本只做「**无损搬运**」，排版必须是**确定性 + 可离线断言**的一步
//  （仓库既有口径：纯函数进 `api_probe`）。历史上「判据需要归一 ⇒ 顺手把产物也归一」
//  正是本次真机事故（段落换行丢失）的成因，故这里把两步**显式分开**。
std::string format_answer_text(const std::string& raw)
{
    // ① `\r\n` / 单独 `\r` → `\n`（Windows 站点 innerText 常见）
    std::string uni;
    uni.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            uni.push_back('\n');
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++i; // CRLF 的 LF 一并吃掉
            }
        }
        else {
            uni.push_back(raw[i]);
        }
    }
    // ② 逐行：清行尾空白；行内连续空白（空格 / 制表 ≥2）→ 单空格；**行首缩进原样保留**
    std::string lines;
    lines.reserve(uni.size());
    std::size_t pos = 0;
    while (pos <= uni.size()) {
        const std::size_t nl   = uni.find('\n', pos);
        const std::size_t end  = (nl == std::string::npos) ? uni.size() : nl;
        std::size_t       last = end; // 行尾空白起点 = 最后一个非空白字符 + 1
        while (last > pos && (uni[last - 1] == ' ' || uni[last - 1] == '\t')) {
            --last;
        }
        std::size_t first = pos; // 行首缩进区间 [pos, first) 原样保留
        while (first < last && (uni[first] == ' ' || uni[first] == '\t')) {
            ++first;
        }
        lines.append(uni, pos, first - pos);
        bool prev_ws = false;
        for (std::size_t i = first; i < last; ++i) {
            const bool ws = (uni[i] == ' ' || uni[i] == '\t');
            if (ws) {
                if (prev_ws) { continue; } // 行内连续空白只留一个空格
                prev_ws = true;
                lines.push_back(' ');
            }
            else {
                prev_ws = false;
                lines.push_back(uni[i]);
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        lines.push_back('\n');
        pos = nl + 1;
    }
    // ④ 空行规范化（A2）：**≥1 个空行 → 恰好 1 个空行**（连续 `\n{3,}` → `\n\n`）
    //  * 语义：行内软换行保留为 `\n`（同段）；**空行 = 段界** ⇒ 产物是规范 Markdown 分段
    //    （下游若是 Markdown 预览 / 再喂 LLM，单 `\n` 只是软换行 ⇒ 段界必须靠空行表达）
    //  * 实现：数**已写入尾部**的连续 `\n`，已有 1 个空行（= 2 个 `\n`）就丢弃多余的
    std::string folded;
    folded.reserve(lines.size());
    std::size_t tail_nl = 0;
    for (const char c : lines) {
        if (c == '\n') {
            if (tail_nl >= 2) { continue; }
            ++tail_nl;
        }
        else {
            tail_nl = 0;
        }
        folded.push_back(c);
    }
    // ⑤ 首尾 trim（空格 / 制表 / 换行）；纯空白 → 空串
    const auto  is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    std::size_t b     = 0;
    std::size_t e     = folded.size();
    while (b < e && is_ws(folded[b])) { ++b; }
    while (e > b && is_ws(folded[e - 1])) { --e; }
    return folded.substr(b, e - b);
}

DomChatResult dom_chat(const DomChatRequest& request)
{
    DomChatResult result;
    const auto    started = std::chrono::steady_clock::now();
    const auto    finish  = [&result, started]() {
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        return result;
    };

    // ---- 前置：只留「真不可用」；生成字段缺失**不再拦路**（M8-37 零开关）----
    //  * `login_url` 缺 → 没有站点就无从开窗（真不可用）
    //  * `input_selector` / `send` / `answer_selector` 缺 → 运行期**自动识别**：
    //    输入框按「可见 + 靠视口下半部」打分，答案容器按「发送前基线 → 新增节点差分」，
    //    发送按钮 click 未命中时自动找可见按钮；都识别不到才报错（含页面候选数 + 下一步）
    if (request.site.login_url.empty()) {
        result.error = "条目缺少 web.login_url（站点登录页）";
        return finish();
    }

    // ---- 1) M8-14：图片**先上传，再发送**（不变量 `I18`：无证据不发送）----
    //  * 走站点上传模板：值解析 → 按站点白名单/限额转码 → CDP 交件 → 等**双证据**
    //  * 未通过 ⇒ 直接返回（**不注入提示词、不发送**）；提示词与发送在下方 3) 才发生
    //  * M8-37 顺序：上传**先于**会话引导 —— 值解析 / 转码是**纯本地**步骤，坏图 / 坏令牌
    //    在碰浏览器之前就失败（既不产生窗口副作用，也让离线自检可断言）
    std::string upload_steps;
    if (!request.image_values.empty()) {
        UploadPlan plan;
        plan.site         = request.site;
        plan.provider_id  = request.provider_id;
        plan.image_values = request.image_values;
        plan.max_bytes    = request.image_max_bytes;
        plan.timeout_ms   = request.timeout_ms;
        plan.owner        = request.owner; // M_patchC（PW-01）：上传证据 / 上传槽按 owner 归属
        const UploadResult upload = upload_images(plan);
        result.uploaded    = upload.ok;
        result.upload_page = upload.page_evidence;
        result.upload_net  = upload.net_evidence;
        result.conversions = upload.conversions;
        upload_steps       = upload.steps;
        if (!upload.ok) {
            result.error = "图片上传未通过（**未注入提示词、未发送**）：" + upload.error;
            return finish();
        }
    }

    // ---- 2) 按站点确保窗口（未登录不阻断：DOM 站点的登录态在浏览器 profile 里；
    //         页面自己会告诉我们「输入框在不在 / 能不能写入」——由脚本诊断给出可操作原因）----
    //  * 放在上传之后：上传若已成功，窗口已在站点上，这里基本是幂等确认
    const web::LoginRequest site_request =
        web::with_owner(web::boot_login_request(request.site, request.provider_id), request.owner);
    std::string boot_error;
    const int   boot_timeout = std::max(15000, std::min(request.timeout_ms, 60000));
    if (!web::ensure_session(site_request, boot_timeout, &boot_error)) {
        result.warning = "未取到内存凭证（该站点可能未登录，或该站点不产生 userToken）：" + boot_error;
    }

    // ---- 3) 页面内驱动：注入提示词 → 触发发送 → 轮询答案（全部走 web::run_script_sync）----
    int poll_ms   = 0;
    int max_polls = 0;
    clamp_poll_params(request.site, &poll_ms, &max_polls);

    // ---- M_patchC PW-03（不变量 I25）：等**交互态**（可见输入候选）----
    //  * 只作为**诊断 + 短等待**：`composer` 与站点强相关，真正的自动识别在 kickoff 脚本里
    //    （打分 + 明确报错）⇒ 未达 L3 **不阻断**（避免把「选择器形态差异」升级成硬失败）
    if (!probe_is_applicable(request.site)) { // DOM 站点才需要（内置协议站点走 web_chat）
        std::string ready_why;
        const int   reached = web::wait_ready(site_request, web::kReadyScript,
                                              std::min(request.timeout_ms, 10000), &ready_why);
        if (!web::ready_satisfied(reached, web::kReadyScript)) {
            result.steps += (result.steps.empty() ? std::string() : "｜") +
                            "就绪提示：" + ready_why;
            log::warn("[dom_chat] " + std::string(web::owner_tag(request.owner)) + " " + ready_why);
        }
    }

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(site_request,
                              "window.__aiwriteDom = " + dom_cfg_json(request) + "; 'ok';", 10000,
                              &raw, &script_error)) {
        result.error = "注入站点配置失败：" + script_error;
        return finish();
    }
    // ---- 3a) 页面准备（JS）：发现输入框 + 聚焦 + 清空 + 记答案基线（**不写入、不发送**）----
    //  * 旧写法（`kKickoffScript`）是「脚本自己写入 + 脚本自己发合成回车」⇒ 豆包 / Kimi 这类
    //    富文本编辑器**忽略**不可信输入（真机 2026-10-08：连续三次 `候选池 0`、页面 60 秒无新消息）
    if (!web::run_script_sync(site_request, dom_prepare_script(), 15000, &raw, &script_error)) {
        result.error = "站点页面交互失败（准备阶段）：" + script_error;
        return finish();
    }
    const bool send_by_click = (request.site.send_kind == "click");
    try {
        const nlohmann::json prep = nlohmann::json::parse(raw);
        if (!prep.value("ok", false)) {
            result.error = prep.value("error", std::string("页面准备失败")) +
                           "（用 --web-adapter-selftest --provider " + request.provider_id +
                           " 复核选择器）";
            return finish();
        }
        result.input_auto = prep.value("input_auto", false);
        result.input_used = prep.value("input_selector", std::string());
        result.steps      = (upload_steps.empty() ? std::string() : ("上传：" + upload_steps + "｜")) +
                       (result.input_auto
                            ? ("自动识别输入框 " + prep.value("input_fingerprint", std::string("?")))
                            : ("input_selector 命中 " + std::to_string(prep.value("input_hits", 0)) +
                               " 个（" + prep.value("input_tag", std::string("?")) + "）")) +
                       "；发送前基线 " + std::to_string(prep.value("baseline", 0)) + " 个候选" +
                       (prep.value("focused", false) ? "、已聚焦" : "、**未取得焦点**");
    }
    catch (const std::exception& ex) {
        result.error = std::string("解析准备结果失败：") + ex.what() + "；原始返回：" +
                       raw.substr(0, 200);
        return finish();
    }

    // ---- 3b) **真实输入**（CDP `Input` 域）：写入提示词（+ key 模式回车）----
    //  * 为什么不能用合成事件：`execCommand('insertText')` / `el.textContent =` + `new
    //    KeyboardEvent('keydown')` 的 `isTrusted=false` ⇒ 富文本编辑器用自己的 state 重渲染
    //    ⇒ 提示词被抹掉、回车被忽略 ⇒ **消息根本没发出去**（而旧代码写完就 `ok=true`：假阳性）
    //  * CDP `Input.insertText` / `Input.dispatchKeyEvent` = 浏览器**进程内**输入 ⇒ `isTrusted=true`
    std::string cdp_detail;
    std::string cdp_error;
    std::string send_mode;
    {
        bool cdp_ok = false;
        if (send_by_click) {
            cdp_ok = web::cdp_input_text(site_request, request.prompt, /*press_enter=*/false, 15000,
                                         &cdp_detail, &cdp_error);
            send_mode = "click";
        }
        else {
            cdp_ok = web::cdp_input_text(site_request, request.prompt, /*press_enter=*/true, 15000,
                                         &cdp_detail, &cdp_error);
            send_mode = cdp_ok ? "key(cdp)" : "key(synth-fallback)";
        }
        if (cdp_ok && send_by_click) {
            // click 模式：CDP 只负责写入文本，发送交给「点发送按钮」（声明值优先）
            std::string click_raw;
            std::string click_err;
            if (web::run_script_sync(site_request, dom_click_send_script(), 10000, &click_raw,
                                     &click_err)) {
                try {
                    const nlohmann::json cj = nlohmann::json::parse(click_raw);
                    cdp_detail += "｜" +
                                  (cj.value("clicked", false)
                                       ? ("点击发送按钮 " + cj.value("selector", std::string("?")))
                                       : cj.value("note", std::string("未点击")));
                }
                catch (const std::exception&) { cdp_detail += "｜点击结果解析失败"; }
            }
            else {
                cdp_detail += "｜点击脚本失败：" + click_err;
            }
        }
        else if (!cdp_ok) {
            // CDP 硬失败（窗口 / 焦点 / 页面就绪等）→ 回退**合成事件**（旧 kickoff：重新写入 + 发送）
            result.steps += "｜CDP 真输入失败（" + cdp_error + "）→ 回退合成事件路径";
            log::warn("[dom_chat] " + std::string(web::owner_tag(request.owner)) +
                      " CDP 真输入失败，回退合成事件：" + cdp_error);
            std::string fb_raw;
            std::string fb_err;
            if (!web::run_script_sync(site_request, dom_kickoff_script(), 15000, &fb_raw, &fb_err)) {
                result.error = "站点页面交互失败：" + fb_err;
                return finish();
            }
        }
    }
    result.steps += (cdp_detail.empty() ? std::string() : ("｜CDP：" + cdp_detail));

    // ---- 3c) 发送回执校验（PW-07f）：URL 变化 / 出现新消息节点 ⇒ 才算「发出去了」----
    //  * 不再「写完就当成功」——豆包 / Kimi 这类站点会**静默忽略**不可信输入；必须验证确实发出，
    //    否则会白等 60 秒（真机 2026-10-08 16:37 / 16:50 / 16:56 连续三次 `候选池 0`，页面
    //    60 秒内一条消息都没渲染，运行后 URL 仍停在 `https://www.doubao.com/chat/`）
    bool        sent             = false;
    bool        composer_has_txt = false;
    int         composer_len     = -1; // -1 = 取不到输入框（无法判断）；0 = 输入框已空
    std::string verify_note;
    const auto  sample_sent      = [&](int rounds) {
        for (int attempt = 0; attempt < rounds && !sent; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::string v_raw;
            std::string v_err;
            if (!web::run_script_sync(site_request, dom_send_verify_script(), 10000, &v_raw, &v_err)) {
                verify_note = "发送回执采样失败：" + v_err;
                continue;
            }
            try {
                const nlohmann::json v = nlohmann::json::parse(v_raw);
                sent                   = v.value("sent", false);
                composer_has_txt       = v.value("composer_has_prompt", false);
                composer_len           = v.value("composer_len", -1);
                verify_note            = v.value("note", std::string());
            }
            catch (const std::exception&) { /* 下一轮重试 */ }
        }
    };
    sample_sent(12);
    // 兜底 A：提示词**根本没写进输入框**（该站忽略 CDP 输入）→ 回退合成事件（旧 kickoff）
    if (!sent && !composer_has_txt) {
        result.steps += "｜CDP 输入未落进输入框 → 回退合成事件";
        std::string fb_raw;
        std::string fb_err;
        if (web::run_script_sync(site_request, dom_kickoff_script(), 15000, &fb_raw, &fb_err)) {
            send_mode += "+synth";
            sample_sent(12);
        }
        else {
            result.steps += "（合成事件回退也失败：" + fb_err + "）";
        }
    }
    // 兜底 B：文本已写但站点不发 → 点可见「发送」按钮（声明值优先）
    if (!sent) {
        std::string click_raw;
        std::string click_err;
        if (web::run_script_sync(site_request, dom_click_send_script(), 10000, &click_raw, &click_err)) {
            try {
                if (nlohmann::json::parse(click_raw).value("clicked", false)) {
                    send_mode += "+click";
                    sample_sent(6);
                }
            }
            catch (const std::exception&) { /* 忽略：下面按未发送处理 */ }
        }
    }
    // 兜底 C（2026-10-10 · 真机驱动 · 循环开发 `T-L06` · **qwen-web 实证**）：
    //  * 现象：CDP `Input.insertText` 成功（提示词**确实在输入框 DOM 里**：回执
    //    `composer_has_prompt=true`），但站点**没发** —— URL 未变化 / 新节点 0 / 提示词仍在框内。
    //    真机 `send=key` 与 `send=click` **两种都试过**仍不发（`CDP Input.dispatchKeyEvent Enter OK`
    //    + 点击 `div.message-input-right-button-send` 均无效果）⇒ 病根不是「哪种发送方式」，
    //    而是**受控组件（React state）没吃到「仅 DOM 值变化」的输入**：按钮 `disabled`、回车不提交。
    //  * 与兜底 A 的区别：A 只处理「文本**根本没进**框」，这里文本在框里但**站点不认**。
    //  * 做法：再走一次**合成事件**（原生 setter + input/change + 声明发送）把内部 state 同步过去，
    //    重新取回执；仍不发 ⇒ 按未发送**如实失败**（`R13`：宁可失败，不许假成功）。
    if (!sent && composer_has_txt) {
        result.steps += "｜CDP 输入已在框内但站点未发 → 合成事件同步内部 state 重试";
        std::string fb_raw;
        std::string fb_err;
        if (web::run_script_sync(site_request, dom_kickoff_script(), 15000, &fb_raw, &fb_err)) {
            send_mode += "+synth2";
            sample_sent(12);
            if (!sent) { // 同步后再点一次可见「发送」按钮（声明值优先）
                std::string click_raw;
                std::string click_err;
                if (web::run_script_sync(site_request, dom_click_send_script(), 10000, &click_raw,
                                         &click_err)) {
                    try {
                        if (nlohmann::json::parse(click_raw).value("clicked", false)) {
                            send_mode += "+click2";
                            sample_sent(6);
                        }
                    }
                    catch (const std::exception&) { /* 忽略：下面按未发送处理 */ }
                }
            }
        }
        else {
            result.steps += "（合成事件同步失败：" + fb_err + "）";
        }
    }
    result.steps += "；send=" + std::string(send_by_click ? "click" : "key") + "（" + send_mode +
                    "）｜发送回执：" + (verify_note.empty() ? std::string("未取到采样") : verify_note);
    if (!sent) {
        // 歧义分支（防**误判**）：回执未取到，但输入框**已空**（提示词不在框里）⇒ 站点多半已经
        //  收下并发出，只是「新消息节点」不在候选池里 / 是同文档跳转 ⇒ **不硬失败**：继续轮询，
        //  但把「回执未取到」如实写进 steps / warning（若最终取不到答案，仍是**如实失败**）
        if (composer_len == 0) {
            result.steps += "｜回执未取到但输入框已空 → 按「已发出」继续轮询";
            result.warning = "发送回执未取到（URL 未变化 / 无新消息节点），但输入框已空 ⇒ 按「已发出」"
                             "继续轮询；若最终未取到答案，请复核 input / send 配置";
            log::warn("[dom_chat] " + std::string(web::owner_tag(request.owner)) + " " + result.warning);
        }
        else {
            result.error =
                "**发送未生效**（提示词已按 CDP 真实输入写入页面，但站点未真正发出消息："
                "URL 未变化 / 无新消息节点 / 提示词仍留在输入框）"
                "—— 常见原因：站点忽略不可信输入、或编辑区未取得焦点。诊断：" +
                result.steps + "；下一步：--web-adapter-selftest --provider " + request.provider_id +
                " 复核 input / send 方式（click 模式的站点可回填发送按钮选择器）";
            log::warn("[dom_chat] " + std::string(web::owner_tag(request.owner)) + " " + result.error);
            return finish();
        }
    }

    // ---- PW-07f：发送后、轮询前**重注入配置**（幂等）----
    //  * 部分站点（真机 Kimi：首条消息整页导航到 `/chat/<id>`）会**换文档** ⇒ 清空页面注入的
    //    `window.__aiwriteDom` / `__aiwriteDomRuntime`；轮询脚本每次都读它 ⇒ 不重注入会得到
    //    「候选池 0 个」的**假象**（豆包等 SPA 站点是同文档跳转，此处为无副作用幂等调用）
    {
        std::string re_raw;
        std::string re_err;
        if (!web::run_script_sync(site_request, "window.__aiwriteDom = " + dom_cfg_json(request) + "; 'ok';",
                                  10000, &re_raw, &re_err)) {
            log::warn("[dom_chat] " + std::string(web::owner_tag(request.owner)) +
                      " 轮询前重注入配置失败（继续按已注入配置轮询）：" + re_err);
        }
    }

    // ---- 4) 轮询答案：done_when 命中 / 文本连续 N 轮稳定 / 到达上限（R13：如实返回 + 提示）----
    std::string last_text;
    std::string last_poll_error;
    // PW-07：最后一轮「自动识别」体检串（诊断：**为什么没取到** —— 装饰文本拒 / 过短拒 计数）
    std::string last_poll_note;
    // PW-07h：最后一轮**实时 URL**（诊断：「发出去了却没答案」到底是**跳走了**还是**站侧没答**）
    std::string last_poll_url;
    // PW-07h：最后一轮 `readyState` + 正文长度（区分「页面还在加载（正文 0 字）」与
    //   「页面已渲染但候选池不匹配（正文很长却 0 候选）」—— 前者等一会儿，后者要回填选择器）
    std::string last_poll_ready;
    int         last_poll_body = 0;
    std::string last_poll_head;
    // PW-07g：页面侧**原始换行数**（物证：`0` ⇒ 该站点本身只给一段，不是我们丢了换行）
    int         last_raw_lf = 0;
    int         stable = 0;
    for (int attempt = 1; attempt <= max_polls; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        result.polls = attempt;
        std::string poll_raw;
        std::string poll_error;
        if (!web::run_script_sync(site_request, dom_poll_script(), 15000, &poll_raw, &poll_error)) {
            last_poll_error = poll_error; // 单次失败不致命：下一轮重试
            continue;
        }
        try {
            const nlohmann::json poll   = nlohmann::json::parse(poll_raw);
            const bool           found  = poll.value("found", false);
            const bool           is_done = poll.value("done", false);
            const std::string    text   = found ? poll.value("text", std::string()) : std::string();
            // PW-07：体检串（`note`）每轮留存 —— 成功 / 失败两条路径都要能一眼看出「拒了几个装饰 / 过短」
            const std::string    note   = poll.value("note", std::string());
            if (!note.empty()) { last_poll_note = note; }
            // PW-07h：实时 URL（每轮留存，失败时写进 error ⇒ 一眼分清「跳走了」还是「站侧没答」）
            const std::string    poll_url = poll.value("url", std::string());
            if (!poll_url.empty()) { last_poll_url = poll_url; }
            // PW-07h：`readyState` + 正文长度（页面还在加载 vs 已渲染但池不匹配）
            const std::string    poll_ready = poll.value("ready", std::string());
            if (!poll_ready.empty()) { last_poll_ready = poll_ready; }
            if (poll.contains("body_chars")) { last_poll_body = poll.value("body_chars", 0); }
            const std::string    poll_head = poll.value("body_head", std::string());
            if (!poll_head.empty()) { last_poll_head = poll_head; }
            if (found) { last_raw_lf = poll.value("lf", 0); } // PW-07g：页面原始换行数（分段物证）
            if (found && !text.empty()) {
                if (result.text.empty()) {
                    // M8-37：声明值命中 → 原样；自动识别 → 如实标明（含页面指纹，供**可选**回填）
                    result.answer_auto        = poll.value("auto", false);
                    result.answer_fingerprint = poll.value("fingerprint", std::string());
                    result.steps += result.answer_auto
                                        ? ("；答案容器**自动识别** " +
                                           (result.answer_fingerprint.empty()
                                                ? std::string("（候选池 " + std::to_string(poll.value("count", 0)) + " 个）")
                                                : result.answer_fingerprint))
                                        : ("；answer_selector 命中 " +
                                           std::to_string(poll.value("count", 0)) + " 个");
                    if (!note.empty()) {
                        result.steps += "；" + note; // PW-07：命中明细（装饰文本拒 / 过短拒）
                    }
                }
                // PW-07g：**排版在 C++ 侧**（页面脚本只做无损搬运）—— 保留段落换行（`\n` 不丢）
                //  * A2 口径：**空行 = 段界**（连续 `\n{3,}` → `\n\n`）
                //  * 纯函数 `format_answer_text` ⇒ 可离线断言（`VB2-22⑦`）；每轮对**原始**文本重算（幂等）
                result.text = format_answer_text(text);
            }
            if (is_done) {
                break; // done_when 条件成立
            }
            if (!result.text.empty() && result.text == last_text) {
                if (++stable >= kStableRounds) {
                    break; // 未配 done_when：文本稳定即完成
                }
            }
            else {
                stable    = 0;
                last_text = result.text;
            }
        }
        catch (const std::exception&) {
            continue; // 页面切换 / 偶发解析失败：下一轮重试
        }
        if (std::chrono::steady_clock::now() - started >
            std::chrono::milliseconds(request.timeout_ms > 0 ? request.timeout_ms : 180000)) {
            result.warning = "达到总超时（" + std::to_string(request.timeout_ms) +
                             " ms）：已取回当前文本，可能仍在生成中";
            break;
        }
    }
    if (result.text.empty()) {
        // PW-07：失败路径也要能一眼看出「为什么没取到」（旧日志只说「未取到候选」，看不出装饰 / 过短被拒）
        // PW-07f：**并把发送链路的诊断（`result.steps`）带出来** —— 否则「发送未生效」与「站点没渲染」
        //   这两件完全不同的事在日志里长得一样（真机 2026-10-08：三次失败都只有「候选池 0」一行）
        log::warn("[DOM 适配器] 未取到答案：站点=" + request.site.login_url +
                  (last_poll_note.empty() ? std::string() : ("｜" + last_poll_note)) +
                  (result.steps.empty() ? std::string() : ("｜发送链路：" + result.steps)));
        result.error = "未取到答案文本（answer_selector 未命中、自动识别也未取到候选，或站点仍在生成）"
                       "；可用 --web-adapter-selftest --provider <id> 诊断选择器";
        if (!result.steps.empty()) {
            result.error += "｜发送链路：" + result.steps;
        }
        if (!last_poll_note.empty()) {
            result.error += "｜最后一轮自动识别体检：" + last_poll_note; // PW-07：命中明细（为什么没取到）
        }
        if (!last_poll_url.empty()) { // PW-07h：实时页面（URL 跳走 = 需登录 / 被重定向；仍在目标站 = 站侧没答）
            result.error += "｜实时页面：" + last_poll_url;
            if (!last_poll_ready.empty()) {
                result.error += "（readyState=" + last_poll_ready + " · 正文 " +
                                std::to_string(last_poll_body) + " 字）";
            }
            if (!last_poll_head.empty()) { // 页面自己的可见文本（首 80 字）—— 登录墙 / 限额页一眼可辨
                result.error += "｜页面正文开头：「" + last_poll_head + "」";
            }
        }
        if (!last_poll_error.empty()) {
            result.error += "（最后一次轮询错误：" + last_poll_error + "）";
        }
        return finish();
    }
    if (result.warning.empty() && result.polls >= max_polls) {
        result.warning = "轮询上限（" + std::to_string(max_polls) +
                         " 次 / 约 " + std::to_string((max_polls * poll_ms) / 1000) +
                         " 秒）到达：已取回文本，可能不完整";
    }
    result.ok = true;
    // PW-07g（A2）：日志用的**行数 / 折叠数** —— 折叠 R = 页面换行 k − 产物换行（**钳 0**）
    //  * 钳 0 的必要性：页面给孤立 `\r`（CR）时页面侧 `lf_count` **不数**它，而 `format_answer_text`
    //    会把它归一成 `\n` ⇒ 产物换行可能**多于** k（差为负），如实报负数会误导排查
    const long long text_lines =
        static_cast<long long>(std::count(result.text.begin(), result.text.end(), '\n')) + 1;
    const int folded_lf = std::max(0, last_raw_lf - static_cast<int>(text_lines - 1));
    log::info("[DOM 适配器] 站点=" + request.site.login_url +
              (request.image_values.empty()
                   ? std::string()
                   : ("，图片 " + std::to_string(request.image_values.size()) + " 张已上传")) +
              " 轮询=" + std::to_string(result.polls) + " 次 / " +
              std::to_string(result.elapsed_ms) + " ms，文本 " +
              std::to_string(result.text.size()) + " 字节" +
              // PW-07g：**行数**（`\n` + 1；A2 后含空行 ⇒ 不再等于「段数」）+ 页面换行数 + **折叠数**
              "；行 " + std::to_string(text_lines) + "（页面换行 " + std::to_string(last_raw_lf) +
              "，折叠 " + std::to_string(folded_lf) + "）" +
              (result.warning.empty() ? "" : ("；警告：" + result.warning)) +
              (result.error.empty() ? "" : ("；错误：" + result.error)) +
              (last_poll_note.empty() ? "" : ("；轮询判据：" + last_poll_note)));
    return finish();
}

// M_patchB L4（PB2-29 / v15）：**选择器候选枚举**（只读）—— 输出候选元素指纹 + 建议选择器
//  * 用途：`--web-dom-dump --provider <id>`；配合 `--web-adapter-selftest` 的命中数即可逐站回填选择器
//  * 只读：不写入、不发送、不读取任何值
std::string suggest_selector_of(const nlohmann::json& d)
{
    const std::string id   = d.value("id", std::string());
    const std::string ph   = d.value("ph", std::string());
    const std::string aria = d.value("aria", std::string());
    const std::string tag  = d.value("tag", std::string("div"));
    if (!id.empty()) {
        return "#" + id;
    }
    if (!ph.empty()) {
        return tag + "[placeholder=\"" + ph + "\"]";
    }
    if (!aria.empty()) {
        return "[aria-label=\"" + aria + "\"]";
    }
    // class 兜底：取前两个「不像哈希/状态」的类名（含数字且较长的一律跳过）
    const std::string cls = d.value("cls", std::string());
    std::vector<std::string> keep;
    std::size_t              begin = 0;
    while (begin < cls.size() && keep.size() < 2) {
        const std::size_t end  = cls.find(' ', begin);
        const std::string item = cls.substr(begin, end == std::string::npos ? std::string::npos
                                                                           : end - begin);
        if (!item.empty() && item.size() <= 24 &&
            !(item.find_first_of("0123456789") != std::string::npos && item.size() > 14)) {
            keep.push_back(item);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (keep.empty()) {
        return {};
    }
    std::string selector = tag;
    for (const std::string& item : keep) {
        selector += "." + item;
    }
    return selector;
}

int dom_selector_dump(const std::string& provider_id, int timeout_ms)
{
  try { // 兜底：把任何 C++ 异常转成可读错误（否则 std::terminate → __fastfail 0xC0000409，输出全丢）
    const ProviderSpec* spec       = provider_specs().find(provider_id);
    const std::string   site_error = web_site_error(spec);
    if (!site_error.empty()) {
        std::printf("[候选枚举] 站点不可用（条目 %s）：%s\n", provider_id.c_str(), site_error.c_str());
        return 2;
    }
    const ProviderWebSpec   site    = strict_web_spec_for(spec);
    const std::string       site_id = strict_web_provider_id_for(spec);
    const web::LoginRequest request = web::interactive_login_request(site, site_id);

    std::printf("\n===== 选择器候选枚举（只读；用于填 input_selector / send / answer_selector）=====\n");
    std::printf("条目     : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
    std::printf("登录页   : %s\n", site.login_url.c_str());
    std::fflush(stdout); // 排障：崩溃前已产生的输出先落盘（__fastfail 会丢缓冲）

    std::string boot_error;
    const int   boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    const bool        session_ok = web::ensure_session(request, boot_timeout, &boot_error);
    const std::string site_key   = web::login_request_site(request);
    const web::Session session   = web::SessionStore::instance().snapshot(site_key);
    const WebSessionVerdict verdict =
        web_session_state(spec, web::web_session_evidence(session));
    std::printf("站点键   : %s\n", site_key.c_str());
    std::printf("登录态   : %s（%s）\n", web_session_state_label(verdict.state).c_str(),
                verdict.reason.c_str());
    std::printf("说明     : %s\n",
                session_ok ? "页面已就绪（候选来自**当前页面**）"
                           : "未取到内存会话：候选来自**未登录页面**（登录后建议重跑）");
    if (!session_ok && !boot_error.empty()) {
        std::printf("会话提示 : %s\n", boot_error.c_str());
    }
    std::fflush(stdout); // 排障：确认 ensure_session 已返回

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(request, std::string(kDiscoverScript), 15000, &raw, &script_error)) {
        std::printf("枚举失败 : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
    }
    std::fflush(stdout); // 排障：确认发现脚本已执行
    nlohmann::json root;
    try {
        nlohmann::json parsed = nlohmann::json::parse(raw);
        // ExecuteScript 会把「脚本 return 的字符串」再包一层 JSON 字符串（既有做法见
        // webview_host.cpp 的 poll_protocol_probe）→ 这里统一解包，避免 type_error.306
        if (parsed.is_string()) {
            parsed = nlohmann::json::parse(parsed.get<std::string>());
        }
        if (!parsed.is_object()) {
            std::printf("枚举结果不是对象（实际类型 %s）：%s\n", parsed.type_name(),
                        raw.substr(0, 200).c_str());
            return 1;
        }
        root = std::move(parsed);
    }
    catch (const std::exception& ex) {
        std::printf("枚举结果解析失败：%s；原始：%s\n", ex.what(), raw.substr(0, 200).c_str());
        return 1;
    }
    std::printf("\nURL      : %s\n", root.value("url", std::string("?")).c_str());
    std::printf("标题     : %s\n", root.value("title", std::string("?")).c_str());
    {
        // 页面 Cookie 名（**只读名**，不含值）+ localStorage 键名（截断）—— 用于后续补 cookie_names
        const nlohmann::json cookies =
            root.contains("cookies") ? root["cookies"] : nlohmann::json::array();
        std::string names;
        for (const nlohmann::json& item : cookies) {
            if (!item.is_string()) {
                continue;
            }
            if (!names.empty()) {
                names += ",";
            }
            names += item.get<std::string>();
        }
        std::printf("页面 Cookie 名（%zu）: %s\n", cookies.size(),
                    names.empty() ? "(无 / HttpOnly 不可见)" : names.c_str());
        const std::string keys = root.value("keys", std::string());
        std::printf("localStorage 键: %s\n", keys.empty() ? "(空)" : keys.c_str());
    }
    std::fflush(stdout); // 排障：确认已解析并回显页面信息
    const auto dump_list = [&](const char* title, const char* key, const char* field) {
        const nlohmann::json list = root.contains(key) ? root[key] : nlohmann::json::array();
        std::printf("\n--- %s（%zu 个候选）---\n", title, list.size());
        int         shown = 0;
        std::string first_visible;
        for (const nlohmann::json& d : list) {
            const std::string selector = suggest_selector_of(d);
            const bool        visible  = d.value("visible", false);
            if (!visible && shown >= 8) {
                continue; // 隐藏元素只列前 8 个（多为模板 / 预渲染节点）
            }
            ++shown;
            std::printf("  %-2d %s %s [%s]\n", shown, visible ? "[可见]" : "[隐藏]",
                        selector.empty() ? "(无可用指纹)" : selector.c_str(),
                        d.value("tag", std::string("?")).c_str());
            if (!d.value("id", std::string()).empty()) {
                std::printf("      id=%s\n", d.value("id", std::string()).c_str());
            }
            if (!d.value("cls", std::string()).empty()) {
                std::printf("      class=%s\n", d.value("cls", std::string()).c_str());
            }
            if (!d.value("ph", std::string()).empty()) {
                std::printf("      placeholder=%s\n", d.value("ph", std::string()).c_str());
            }
            if (!d.value("aria", std::string()).empty()) {
                std::printf("      aria-label=%s\n", d.value("aria", std::string()).c_str());
            }
            if (!d.value("ce", std::string()).empty()) {
                std::printf("      contenteditable=%s\n", d.value("ce", std::string()).c_str());
            }
            if (!d.value("text", std::string()).empty()) {
                std::printf("      text=%s\n", d.value("text", std::string()).c_str());
            }
            if (visible && first_visible.empty() && !selector.empty()) {
                first_visible = selector;
            }
        }
        if (!first_visible.empty()) {
            std::printf("  ⇒ 建议 %s = %s\n", field, first_visible.c_str());
        }
        else {
            std::printf("  ⇒ 建议 %s = （未发现可见候选：可能需先登录 / 页面未渲染完）\n", field);
        }
    };

    dump_list("输入框候选（input / textarea / contenteditable）", "inputs", "input_selector");
    dump_list("发送候选（button / submit / send）", "sends", "send.selector");
    dump_list("回答容器候选（markdown / message / answer）", "answers", "answer_selector");

    std::printf("\n下一步：把建议填进该条目的 web 段 → 重跑 "
                "`aiwrite.exe --web-adapter-selftest --provider %s` 确认可达\n",
                provider_id.c_str());
    std::fflush(stdout);
    return 0;
  }
  catch (const std::exception& ex) {
    std::printf("枚举异常（已兜底，不再崩溃）：%s\n", ex.what());
    std::fflush(stdout);
    return 1;
  }
  catch (...) {
    std::printf("枚举异常（未知类型，已兜底）\n");
    std::fflush(stdout);
    return 1;
  }
}

namespace {

// 报告时间戳（"YYYY-MM-DD HH:MM:SS"，与其它落盘报告同口径）
std::string probe_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

// 站点状态（5 态中的 4 态；「新增 / 消失」由 diff 判定）—— 判据与 §3.2 一一对应
std::string probe_state_of(const DomSiteProbe& p)
{
    if (!p.site_error.empty()) {
        return "未测"; // 条目不可用：根本没测
    }
    if (!p.probe_ok || p.page_abnormal) {
        return "失败"; // 探测没跑通，或页面既无输入区候选也无登录迹象
    }
    return p.needs_login ? "需登录" : "可用";
}

// 状态文案（人类可读，附一句原因）
std::string probe_state_text(const DomSiteProbe& p)
{
    const std::string state = probe_state_of(p);
    if (state == "未测") {
        return "未测（" + p.site_error + "）";
    }
    if (state == "失败") {
        const std::string why =
            p.page_abnormal ? std::string("页面异常：既无输入区候选也无登录迹象")
                            : (p.error.empty() ? std::string("原因不明") : p.error);
        return "失败（" + why + "）";
    }
    if (state == "需登录") {
        return "需登录（" + p.login_evidence + "）";
    }
    return "可用（匿名）";
}

// 一行判据（机器可读版进 JSON；这里给人看）：输入区候选 / 登录控件 / 登录证据
std::string probe_criteria_text(const DomSiteProbe& p)
{
    return "输入区候选 " + std::to_string(p.input_candidates) + " · 登录控件 " +
           std::to_string(p.login_controls) + " · 登录证据 " +
           (p.login_evidence.empty() ? std::string("无") : p.login_evidence);
}

// 每站紧凑块（2–4 行；命令输出即报告素材）
void print_probe_block(int index, int total, const DomSiteProbe& p)
{
    std::printf("[%2d/%2d] %s（%s）\n", index, total, p.id.c_str(), p.display.c_str());
    std::printf("         状态    : %s\n", probe_state_text(p).c_str());
    std::printf("         判据    : %s\n", probe_criteria_text(p).c_str());
    if (p.probe_ok) {
        std::printf("         页面    : %s\n", p.url.empty() ? "?" : p.url.c_str());
        std::printf("         命中    : input %d（%s）· send %d · answer %d（末条 %d 字）"
                    "· token 长度 %d · Cookie %d/%d\n",
                    p.input_hits, p.input_visible ? "可见" : "不可见", p.send_hits, p.answer_hits,
                    p.answer_chars, p.token_len, p.cookies_present, p.cookies_wanted);
    }
    else if (!p.error.empty()) {
        std::printf("         原因    : %s\n", p.error.c_str());
    }
    if (p.needs_login) {
        std::printf("         → 人工介入：在该窗口内手动登录一次，然后重跑本命令\n");
    }
    if (p.attempts > 1) {
        std::printf("         重试    : 已按 loop_dev §3.2/§5.1③ 重试 1 次（本站本轮共 %d 次尝试）\n",
                    p.attempts);
    }
    std::fflush(stdout);
}

} // namespace

// 单站探测的共用实现（`dom_adapter_selftest` 与批量探测都走这里跑脚本）
//  * 只读：注入配置 + 执行探测脚本；**不写入、不发送、不读任何值**
static bool run_site_probe_script(const ProviderWebSpec& site, const std::string& site_id,
                                  int timeout_ms, DomSiteProbe* out)
{
    const web::LoginRequest request = web::interactive_login_request(site, site_id);
    std::string             boot_error;
    const int               boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    out->session_ok = web::ensure_session(request, boot_timeout, &boot_error);
    if (!out->session_ok) {
        out->session_hint = boot_error;
    }

    nlohmann::json probe_cfg;
    probe_cfg["input_selector"]  = site.input_selector;
    probe_cfg["send_kind"]       = site.send_kind;
    probe_cfg["send_value"]      = site.send_value;
    probe_cfg["answer_selector"] = site.answer_selector;
    probe_cfg["done_kind"]       = site.done_kind;
    probe_cfg["done_selector"]   = site.done_selector;
    probe_cfg["token_expr"]      = site.token_expr;
    probe_cfg["cookie_names"]    = site.cookie_names;
    // 输入区候选池：与运行期自动识别**同源**（`kInputPoolSelector`）—— 口径不复制
    probe_cfg["input_pool"]      = kInputPoolSelector;

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(request, "window.__aiwriteDomProbe = " + probe_cfg.dump() + "; 'ok';",
                              10000, &raw, &script_error) ||
        !web::run_script_sync(request, dom_probe_script(), 15000, &raw, &script_error)) {
        out->error = "探测脚本执行失败：" + script_error;
        return false;
    }
    if (!dom_probe_result_from_json(raw, out)) {
        out->error = "探测结果解析失败（前 200 字符）：" + raw.substr(0, 200);
        return false;
    }
    return true;
}

int dom_site_probe(const std::string& provider_id, int timeout_ms, DomSiteProbe* out)
{
    if (out == nullptr) {
        return 2;
    }
    *out    = DomSiteProbe();
    out->id = provider_id;

    const ProviderSpec* spec = provider_specs().find(provider_id);
    if (spec == nullptr) {
        out->site_error = "表里没有该条目（id 未知）";
        return 2;
    }
    out->display = spec->display;
    out->adapter = spec->web.adapter;

    const std::string site_error = web_site_error(spec);
    if (!site_error.empty()) {
        out->site_error = site_error;
        return 2;
    }
    if (spec->web.adapter != "dom") {
        out->site_error = "适配器是 " + spec->web.adapter + "（本探测只诊断 adapter=dom 的条目）";
        return 2;
    }

    const ProviderWebSpec site    = strict_web_spec_for(spec);
    const std::string     site_id = strict_web_provider_id_for(spec);
    if (!run_site_probe_script(site, site_id, timeout_ms, out)) {
        return 1;
    }
    return 0;
}

bool dom_probe_result_from_json(const std::string& raw, DomSiteProbe* out)
{
    if (out == nullptr) {
        return false;
    }
    nlohmann::json r;
    try {
        r = nlohmann::json::parse(raw);
    }
    catch (const std::exception&) {
        return false;
    }
    if (!r.is_object()) {
        return false;
    }
    const auto str_of = [&r](const char* key) -> std::string {
        return (r.contains(key) && r[key].is_string()) ? r[key].get<std::string>() : std::string();
    };
    // 数值缺失 → 回调默认值；**不把缺失当事实**（页面事实要么有要么没有）
    const auto num_of = [&r](const char* key, int fallback) -> int {
        return (r.contains(key) && r[key].is_number()) ? r[key].get<int>() : fallback;
    };
    const auto bool_of = [&r](const char* key, bool fallback) -> bool {
        return (r.contains(key) && r[key].is_boolean()) ? r[key].get<bool>() : fallback;
    };
    const auto size_of = [&r](const char* key) -> int {
        return (r.contains(key) && r[key].is_array()) ? static_cast<int>(r[key].size()) : 0;
    };

    DomSiteProbe p         = *out; // 保留调用方填好的入口字段（id / display / adapter）
    p.url                  = str_of("url");
    p.title                = str_of("title");
    p.input_hits           = num_of("input_hits", 0);
    p.input_visible        = bool_of("input_visible", false);
    p.send_hits            = num_of("send_hits", 0);
    p.answer_hits          = num_of("answer_hits", 0);
    p.answer_chars         = num_of("answer_chars", 0);
    p.token_len            = num_of("token_len", -1);
    p.token_json           = bool_of("token_json", false);
    p.cookies_wanted       = size_of("cookies_wanted");
    p.cookies_present      = size_of("cookies_present");
    p.input_candidates     = num_of("input_candidates", 0);
    p.login_controls       = num_of("login_controls", 0);
    p.login_evidence       = str_of("login_evidence");
    p.needs_login          = bool_of("needs_login", false);
    p.page_abnormal        = bool_of("page_abnormal", false);
    const std::string serr = str_of("error"); // 脚本自身抛错 ⇒ 事实可能不完整 ⇒ 不算探测成功
    if (!serr.empty()) {
        p.error = "页面脚本报错：" + serr;
    }
    p.probe_ok = serr.empty();
    *out       = p;
    return true;
}

int dom_probe_all(int timeout_ms, nlohmann::json* report, std::vector<DomSiteProbe>* probes)
{
    // 站点集合：`kind=web 且 web.adapter=dom` —— 表驱动，**不写死站点清单**
    std::vector<const ProviderSpec*> targets;
    for (const ProviderSpec& spec : provider_specs().items) {
        if (spec.kind == "web" && spec.web.adapter == "dom") {
            targets.push_back(&spec);
        }
    }

    std::printf("\n===== 网页版 DOM 站点批量只读探测（loop_dev 批 3 · FEA-L01）=====\n");
    std::printf("条目 %zu 个（kind=web 且 adapter=dom）· 超时 %d s/站"
                "· 只读：不写入、不发送、不读任何值\n",
                targets.size(), timeout_ms);
    if (targets.empty()) {
        std::printf("[批量探测] 表里没有可诊断的 DOM 站点（检查 providers.json / 覆盖链）\n");
        return 2;
    }

    nlohmann::json rep;
    rep["schema"]       = "aiwrite.web_adapter_report/1";
    rep["generated_at"] = probe_timestamp();
    rep["timeout_ms"]   = timeout_ms;
    rep["sites"]        = nlohmann::json::array();

    int                       usable = 0, need_login = 0, failed = 0, skipped = 0;
    std::vector<DomSiteProbe> all;
    all.reserve(targets.size());
    int index = 0;
    for (const ProviderSpec* spec : targets) {
        DomSiteProbe p;
        dom_site_probe(spec->id, timeout_ms, &p);
        // ---- 环境级失败 ⇒ **重试 1 次**（`loop_dev` §3.2 行 3 + §5.1③；§3.3：同轮同站上限 2 次）----
        //  * 只对「探测没跑通（窗口 / 页面未就绪）」与「页面异常」重试 —— 二者可能是重站首屏慢；
        //  * **不**对「需登录」重试（登录墙不会自己消失；重试只会白等一轮）
        if (p.site_error.empty() && (!p.probe_ok || p.page_abnormal)) {
            std::printf("[%2d/%2d] %s 环境级失败（%s）⇒ 按 §3.2 重试 1 次\n", index + 1,
                        static_cast<int>(targets.size()), p.id.c_str(),
                        p.page_abnormal ? "页面异常" : "探测未跑通");
            std::fflush(stdout);
            DomSiteProbe again;
            dom_site_probe(spec->id, timeout_ms, &again);
            again.attempts = 2;
            p              = again;
        }
        print_probe_block(++index, static_cast<int>(targets.size()), p);

        const std::string state = probe_state_of(p);
        if (state == "可用") {
            ++usable;
        }
        else if (state == "需登录") {
            ++need_login;
        }
        else if (state == "失败") {
            ++failed;
        }
        else {
            ++skipped;
        }

        nlohmann::json item;
        item["id"]               = p.id;
        item["display"]          = p.display;
        item["adapter"]          = p.adapter;
        item["state"]            = state;
        item["probe_ok"]         = p.probe_ok;
        item["session_ok"]       = p.session_ok;
        item["url"]              = p.url;
        item["title"]            = p.title;
        item["input_hits"]       = p.input_hits;
        item["input_visible"]    = p.input_visible;
        item["input_candidates"] = p.input_candidates;
        item["login_controls"]   = p.login_controls;
        item["login_evidence"]   = p.login_evidence;
        item["needs_login"]      = p.needs_login;
        item["page_abnormal"]    = p.page_abnormal;
        item["send_hits"]        = p.send_hits;
        item["answer_hits"]      = p.answer_hits;
        item["answer_chars"]     = p.answer_chars;
        item["token_len"]        = p.token_len;
        item["token_json"]       = p.token_json;
        item["cookies_wanted"]   = p.cookies_wanted;
        item["cookies_present"]  = p.cookies_present;
        item["error"]            = p.error;
        item["site_error"]       = p.site_error;
        item["attempts"]         = p.attempts;
        rep["sites"].push_back(item);
        all.push_back(p);
    }

    rep["counts"] = {{"total", static_cast<int>(targets.size())},
                     {"usable", usable},
                     {"needs_login", need_login},
                     {"failed", failed},
                     {"skipped", skipped}};

    std::printf("----- 汇总 -----\n");
    std::printf("可用 %d · 需登录 %d · 失败 %d · 未测 %d（共 %zu 站）\n", usable, need_login, failed,
                skipped, targets.size());
    if (need_login > 0) {
        std::printf("需人工介入（退出码 3）：上列「需登录」站点 —— 在窗口里手动登录一次后重跑本命令\n");
    }
    if (failed > 0) {
        std::printf("**真失败**（退出码 1）：上列「失败」站点 —— 待修清单，需人读原因后处置\n");
    }
    std::fflush(stdout);

    if (report != nullptr) {
        *report = rep;
    }
    if (probes != nullptr) {
        *probes = all;
    }
    if (failed > 0) {
        return 1; // 真失败优先（比「需登录」更需要人手）
    }
    return need_login > 0 ? 3 : 0;
}

bool dom_report_diff(const nlohmann::json& previous, const nlohmann::json& current,
                     std::vector<std::string>* lines)
{
    if (!previous.is_object() || !previous.contains("sites") || !previous["sites"].is_array() ||
        !current.is_object() || !current.contains("sites") || !current["sites"].is_array()) {
        return false;
    }
    // 状态口径与 `probe_state_of` 一一对应；旧版报告没有 `state` 字段时按事实字段回推
    const auto state_of = [](const nlohmann::json& item) -> std::string {
        if (item.contains("state") && item["state"].is_string()) {
            return item["state"].get<std::string>();
        }
        if (!item.value("site_error", std::string()).empty()) {
            return "未测";
        }
        if (item.contains("probe_ok") && !item.value("probe_ok", false)) {
            return "失败";
        }
        if (item.value("page_abnormal", false)) {
            return "失败";
        }
        if (item.value("needs_login", false)) {
            return "需登录";
        }
        // 与页面脚本共用同一条 §3.2 判据：既无输入区候选、也无登录迹象 ⇒ 页面异常（失败）
        if (item.value("input_hits", 0) == 0 && item.value("input_candidates", 0) == 0 &&
            item.value("login_evidence", std::string()).empty()) {
            return "失败";
        }
        return "可用";
    };

    std::map<std::string, const nlohmann::json*> before;
    for (const nlohmann::json& item : previous["sites"]) {
        if (item.is_object() && item.contains("id") && item["id"].is_string()) {
            before[item["id"].get<std::string>()] = &item;
        }
    }

    std::vector<std::string>    out;
    std::map<std::string, bool> seen;
    int                         unchanged = 0;
    for (const nlohmann::json& item : current["sites"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            continue;
        }
        const std::string id        = item["id"].get<std::string>();
        const std::string now_state = state_of(item);
        seen[id]                    = true;
        const auto it               = before.find(id);
        if (it == before.end()) {
            out.push_back("新增      " + id + "：" + now_state + "（上一轮报告里没有这个条目）");
            continue;
        }
        const nlohmann::json& old_item      = *it->second;
        const std::string     old_state     = state_of(old_item);
        const std::string     now_ev        = item.value("login_evidence", std::string());
        const std::string     old_ev        = old_item.value("login_evidence", std::string());
        const std::string     now_url       = item.value("url", std::string());
        const std::string     old_url       = old_item.value("url", std::string());
        const bool            state_changed = (old_state != now_state);
        const bool            ev_changed    = (now_ev != old_ev);
        const bool            url_changed   = (now_url != old_url);
        if (!state_changed && !ev_changed && !url_changed) {
            ++unchanged;
            continue;
        }
        if (state_changed) {
            out.push_back("状态变化  " + id + "：" + old_state + " → " + now_state);
        }
        if (ev_changed) {
            out.push_back("判据变化  " + id + "：登录证据「" + (old_ev.empty() ? "无" : old_ev) +
                          "」→「" + (now_ev.empty() ? "无" : now_ev) + "」");
        }
        if (url_changed) {
            out.push_back("页面变化  " + id + "：" + (old_url.empty() ? "?" : old_url) + " → " +
                          (now_url.empty() ? "?" : now_url));
        }
    }
    for (const auto& kv : before) {
        if (seen.count(kv.first) == 0) {
            out.push_back("消失      " + kv.first + "：上一轮有、本轮没有（条目被删 / 改名？）");
        }
    }
    if (out.empty()) {
        out.push_back("无变化（" + std::to_string(unchanged) + " 站逐项一致）");
    }
    if (lines != nullptr) {
        *lines = out;
    }
    return true;
}

int dom_adapter_selftest(const std::string& provider_id, int timeout_ms)
{
    const ProviderSpec* spec       = provider_specs().find(provider_id);
    const std::string   site_error = web_site_error(spec);
    if (!site_error.empty()) {
        std::printf("[选择器探测] 站点不可用（条目 %s）：%s\n", provider_id.c_str(),
                    site_error.c_str());
        return 2;
    }
    if (spec->web.adapter != "dom") {
        std::printf("[选择器探测] 条目 %s 的适配器是 %s —— 本自检只诊断 adapter=dom 的条目\n",
                    provider_id.c_str(), spec->web.adapter.c_str());
        return 2;
    }

    const ProviderWebSpec   site    = strict_web_spec_for(spec);
    const std::string       site_id = strict_web_provider_id_for(spec);
    const web::LoginRequest request = web::interactive_login_request(site, site_id);

    std::printf("\n===== 选择器探测 / 诊断（只读，不发送任何内容）=====\n");
    std::printf("条目        : %s（%s）\n", spec->id.c_str(), spec->display.c_str());
    std::printf("适配器      : %s\n", site.adapter.c_str());
    std::printf("登录页      : %s\n", site.login_url.c_str());
    if (!site.window_title.empty()) {
        std::printf("窗口标题    : %s\n", site.window_title.c_str());
    }
    const std::string field_warnings = web_site_field_warnings(spec);
    if (!field_warnings.empty()) {
        std::printf("字段缺失    : %s\n", field_warnings.c_str());
    }
    if (web_login_only(spec)) {
        std::printf("⚠️ 缺生成字段（选择器 / 发送 / 取答案）：**生成仍可用** —— 运行期会自动识别"
                    "（M8-37，结果只打印）；本自检仅用于只读复核命中数 + 固化建议值\n");
    }

    // 1) 按站点准备窗口（未登录 → 在该窗口手动登录一次；不代填密码、不绕过验证）
    std::string boot_error;
    const int   boot_timeout =
        std::max(10000, std::min(timeout_ms > 0 ? timeout_ms * 1000 : 30000, 60000));
    const bool        session_ok = web::ensure_session(request, boot_timeout, &boot_error);
    const std::string site_key   = web::login_request_site(request);
    const web::Session session   = web::SessionStore::instance().snapshot(site_key);
    std::printf("站点键      : %s\n", site_key.c_str());
    std::printf("登录态      : %s（仅内存；程序退出即销毁）\n",
                session.user_token.empty() ? "未取到（请在该窗口内手动登录一次后重跑）" : "已取得");
    if (!session_ok && !boot_error.empty()) {
        std::printf("会话提示    : %s\n", boot_error.c_str());
    }

    // 2) 注入探测配置 + 执行探测脚本（只读）
    nlohmann::json probe_cfg;
    probe_cfg["input_selector"]  = site.input_selector;
    probe_cfg["send_kind"]       = site.send_kind;
    probe_cfg["send_value"]      = site.send_value;
    probe_cfg["answer_selector"] = site.answer_selector;
    probe_cfg["done_kind"]       = site.done_kind;
    probe_cfg["done_selector"]   = site.done_selector;
    probe_cfg["token_expr"]      = site.token_expr;
    probe_cfg["cookie_names"]    = site.cookie_names;
    probe_cfg["input_pool"]      = kInputPoolSelector; // 候选池与运行期自动识别同源

    std::string raw;
    std::string script_error;
    if (!web::run_script_sync(request, "window.__aiwriteDomProbe = " + probe_cfg.dump() + "; 'ok';",
                              10000, &raw, &script_error) ||
        !web::run_script_sync(request, dom_probe_script(), 15000, &raw, &script_error)) {
        std::printf("探测失败    : %s\n", script_error.c_str());
        std::printf("（提示：窗口 / 页面未就绪，或站点拒绝脚本执行；可重跑本命令）\n");
        return 1;
    }
    nlohmann::json r;
    try {
        r = nlohmann::json::parse(raw);
    }
    catch (const std::exception& ex) {
        std::printf("探测结果解析失败：%s；原始：%s\n", ex.what(), raw.substr(0, 200).c_str());
        return 1;
    }

    const int         input_hits   = r.value("input_hits", 0);
    const int         send_hits    = r.value("send_hits", 0);
    const int         answer_hits  = r.value("answer_hits", 0);
    const int         answer_chars = r.value("answer_chars", 0);
    const int         token_len    = r.value("token_len", -1);
    const bool        token_json   = r.value("token_json", false);
    const std::size_t cookies_ok =
        r.value("cookies_present", std::vector<std::string>()).size();

    std::printf("\n--- 当前页面 ---\n");
    std::printf("URL         : %s\n", r.value("url", std::string("?")).c_str());
    std::printf("标题        : %s\n", r.value("title", std::string("?")).c_str());
    std::printf("input       : %s → 命中 %d 个%s\n", site.input_selector.c_str(), input_hits,
                input_hits == 0
                    ? ""
                    : (r.value("input_visible", false) ? "（可见）"
                                                       : "（不可见：可能在登录页 / 隐藏面板）"));
    std::printf("send        : kind=%s value=%s → 命中 %d\n", site.send_kind.c_str(),
                site.send_value.c_str(), send_hits);
    std::printf("answer      : %s → 命中 %d 个（末条 %d 字）\n", site.answer_selector.c_str(),
                answer_hits, answer_chars);
    if (site.done_kind.empty()) {
        std::printf("done_when   : 未配置（将用「文本稳定 %d 轮」判定）\n", kStableRounds);
    }
    else {
        std::printf("done_when   : kind=%s selector=%s（当前：%s）\n", site.done_kind.c_str(),
                    site.done_selector.c_str(),
                    r.contains("done_now") && r["done_now"].is_boolean()
                        ? (r["done_now"].get<bool>() ? "已满足" : "未满足")
                        : "未知");
    }
    if (site.token_expr.empty()) {
        std::printf("token_expr  : 未配置（该站点无法自动探测凭证；登录仍可用）\n");
    }
    else if (token_len == -2) {
        std::printf("token_expr  : %s → **求值失败**（CSP 拦截 eval 或表达式有误）\n",
                    site.token_expr.c_str());
    }
    else if (token_len < 0) {
        std::printf("token_expr  : %s → 取到 null / undefined\n", site.token_expr.c_str());
    }
    else {
        std::printf("token_expr  : %s → 长度 %d%s\n", site.token_expr.c_str(), token_len,
                    token_json ? "（**是 JSON 包裹值** → 建议改成解包表达式；与 R19 同源）" : "");
    }
    std::printf("Cookie      : 期望 %zu 个 / 可读 %zu 个（HttpOnly 不计入）\n",
                site.cookie_names.size(), cookies_ok);

    // 2b) 需登录判据（loop_dev §3.2）：只用页面事实 → 「可用 / 需登录 / 页面异常」三分类
    //     单一真相源：判据在页面脚本里算（此处只搬运结果）
    DomSiteProbe facts;
    facts.id      = spec->id;
    facts.display = spec->display;
    facts.adapter = site.adapter;
    if (!dom_probe_result_from_json(raw, &facts)) {
        std::printf("\n需登录判据  : **解析失败**（探测结果不是 JSON 对象）—— 归失败\n");
        return 1;
    }
    std::printf("\n需登录判据  : 输入区候选 %d · 登录控件 %d · 登录证据 %s\n",
                facts.input_candidates, facts.login_controls,
                facts.login_evidence.empty() ? "无" : facts.login_evidence.c_str());
    if (facts.needs_login) {
        std::printf("            → **需登录**（退出码 3）：在窗口里手动登录一次后重跑本命令\n");
    }
    else if (facts.page_abnormal) {
        std::printf("            → **页面异常**：既无输入区候选也无登录迹象（可能是加载中 / 反爬"
                    "拦截页）—— 归失败\n");
    }
    else {
        std::printf("            → 匿名可用（无需登录即可发送）\n");
    }

    // 3) 可操作建议
    bool                     ok = true;
    std::vector<std::string> tips;
    if (input_hits == 0) {
        ok = false;
        tips.emplace_back("input_selector 未命中：在窗口里按 F12 选中输入框 → 右键 Copy → Copy "
                          "selector，填进该条目的 web.input_selector");
    }
    if (send_hits == 0) {
        ok = false;
        tips.emplace_back(std::string("send 目标未命中：") +
                          (site.send_kind == "click"
                               ? "send.selector 不存在 → 修正选择器，或改 send.kind=key（Enter）"
                               : "输入框不可交互 → 确认页面已登录、输入框可见"));
    }
    if (answer_hits == 0) {
        ok = false;
        tips.emplace_back("answer_selector 未命中：先在窗口里手动发一条消息，再选中回答容器"
                          "（如 .markdown-body）取其 CSS 选择器");
    }
    if (site.done_kind.empty()) {
        tips.emplace_back("建议配 done_when（kind=selector_gone +「停止生成」按钮选择器）：否则只能靠"
                          "「文本稳定」判定，长回答可能被判早完成");
    }
    if (token_len == -2) {
        tips.emplace_back("token_expr 求值失败：改用可在页面直接求值的表达式（避免被 CSP 拦的写法）");
    }
    if (token_json) {
        tips.emplace_back("token_expr 取到 JSON 包裹值：改成解包（如 JSON.parse(...).token）");
    }
    if (!tips.empty()) {
        std::printf("\n--- 建议（按序修；改完重启程序再跑一次本命令）---\n");
        for (const std::string& tip : tips) {
            std::printf("  · %s\n", tip.c_str());
        }
    }
    std::printf("\n[选择器探测] 结果：%s\n", ok ? "必需选择器全部命中 —— 可在界面点「运行」验证生成"
                                                : "有缺项（见上「建议」）");
    if (ok) {
        std::printf("  提示：实测可用后请把 %s 的 verified 置 true，并回填 M_patchB 附录 E\n",
                    spec->id.c_str());
    }
    // 退出码（loop_dev §6.3）：3 = 需人工介入（需登录）**优先于** 1 —— 登录墙下选择器缺项是
    // 「没法测」而不是「真失败」，两者处置不同（前者人手登录，后者改选择器）
    if (facts.needs_login) {
        std::printf("\n[选择器探测] 需人工介入：先在该窗口内登录一次，再重跑本命令（退出码 3）\n");
        return 3;
    }
    if (facts.page_abnormal) {
        std::printf("\n[选择器探测] 页面异常：既无输入区候选也无登录迹象（反爬 / 加载中？）"
                    "—— 归失败（退出码 1）\n");
        return 1;
    }
    return ok ? 0 : 1;
}

// ----------------------------------------------------------------------------
//  Loop Dev（批 3）：**离线自检** —— 报告解析 / diff / 状态映射的纯函数断言
//  * 不开窗口、不联网、不写文件（`FEA-L01` 的回归资产；真机走 `--web-adapter-selftest`）
//  * 断言口径 = `docs/loop_dev.md` §3.2（判据）+ §6.2（五态）+ §6.3（退出码）
// ----------------------------------------------------------------------------
int dom_probe_selftest(int* passed_out)
{
    int passed = 0;
    int failed = 0;
    const auto check = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    // ---- ① `dom_probe_result_from_json`：事实搬运（不重算判据）----
    const std::string full = R"({"url":"https://x/login","title":"T","input_hits":0,)"
                             R"("input_visible":false,"send_hits":0,"answer_hits":2,"answer_chars":7,)"
                             R"("token_len":64,"token_json":true,"cookies_wanted":["a","b"],)"
                             R"("cookies_present":["a"],"input_candidates":0,"login_controls":3,)"
                             R"("login_evidence":"password-input x1 + login-node x2",)"
                             R"("needs_login":true,"page_abnormal":false,"error":""})";
    DomSiteProbe p;
    p.id      = "kimi-web";
    p.display = "Kimi";
    p.adapter = "dom";
    check(dom_probe_result_from_json(full, &p), "解析：合法 JSON 对象 ⇒ true");
    check(p.url == "https://x/login" && p.title == "T" && p.answer_hits == 2 &&
              p.answer_chars == 7,
          "解析：页面事实逐项搬运（url / title / answer）");
    check(p.token_len == 64 && p.token_json && p.cookies_present == 1 && p.cookies_wanted == 2,
          "解析：token / Cookie 计数搬运（长度而非值 —— 守 I15）");
    check(p.needs_login && !p.page_abnormal &&
              p.login_evidence == "password-input x1 + login-node x2" && p.login_controls == 3,
          "解析：**需登录判据来自页面**（needs_login / login_evidence 只搬运、不重算）");
    check(p.probe_ok && p.error.empty(), "解析：无 error ⇒ probe_ok（事实完整）");
    check(p.id == "kimi-web" && p.display == "Kimi" && p.adapter == "dom" && p.attempts == 1,
          "解析：入口字段（id / display / adapter）与 attempts 不被脚本结果覆盖");

    DomSiteProbe bare;
    check(dom_probe_result_from_json(R"({"url":"https://y/"})", &bare) &&
              bare.input_hits == 0 && bare.input_candidates == 0 && bare.token_len == -1 &&
              !bare.needs_login && !bare.page_abnormal && bare.login_evidence.empty(),
          "解析：字段缺失 = 「无事实」（不猜、不默认成功；token_len 回 -1）");

    DomSiteProbe bad;
    bad.input_hits = 9;
    check(!dom_probe_result_from_json("{not json", &bad) && bad.input_hits == 9,
          "解析：非法 JSON ⇒ false（**不改**调用方结构体）");
    check(!dom_probe_result_from_json("[1,2,3]", &bad), "解析：JSON 数组（非对象）⇒ false");
    check(!dom_probe_result_from_json(full, nullptr), "解析：空指针 ⇒ false（不崩）");

    DomSiteProbe err;
    check(dom_probe_result_from_json(R"({"url":"https://z/","error":"boom"})", &err) &&
              !err.probe_ok && err.error.find("boom") != std::string::npos,
          "解析：脚本自身报错 ⇒ probe_ok = false（事实可能不完整）");

    // ---- ② `probe_state_of`：§6.2 五态中的 4 态（第 5 态由 diff 判「新增 / 消失」）----
    {
        DomSiteProbe ok_p;
        ok_p.probe_ok = true;
        check(probe_state_of(ok_p) == "可用", "状态：探测跑通且无登录迹象 ⇒ 可用");
        ok_p.needs_login = true;
        check(probe_state_of(ok_p) == "需登录", "状态：needs_login ⇒ 需登录（退出码 3，**不是**失败）");
        ok_p.page_abnormal = true;
        check(probe_state_of(ok_p) == "失败", "状态：页面异常 ⇒ 失败（优先级高于需登录）");
        DomSiteProbe bad_p;
        check(probe_state_of(bad_p) == "失败", "状态：探测没跑通（probe_ok = false）⇒ 失败");
        bad_p.site_error = "表里没有该条目（id 未知）";
        check(probe_state_of(bad_p) == "未测", "状态：条目不可用 ⇒ 未测（根本没测，不算失败）");
    }

    // ---- ③ `dom_report_diff`：五态 + 旧报告回推（§6.2）----
    const auto site = [](const std::string& id, const std::string& state, bool ok = true,
                         bool need = false, bool abnormal = false,
                         const std::string& ev = "", const std::string& url = "",
                         int hits = 1, int cands = 1) {
        nlohmann::json item;
        item["id"]               = id;
        item["state"]            = state;
        item["probe_ok"]         = ok;
        item["needs_login"]      = need;
        item["page_abnormal"]    = abnormal;
        item["login_evidence"]   = ev;
        item["url"]              = url;
        item["input_hits"]       = hits;
        item["input_candidates"] = cands;
        return item;
    };
    const auto report_of = [](const std::vector<nlohmann::json>& sites) {
        nlohmann::json doc;
        doc["schema"] = "aiwrite.web_adapter_report/1";
        doc["sites"]  = nlohmann::json::array();
        for (const nlohmann::json& s : sites) {
            doc["sites"].push_back(s);
        }
        return doc;
    };
    const auto join = [](const std::vector<std::string>& lines) {
        std::string text;
        for (const std::string& line : lines) {
            text += line + "\n";
        }
        return text;
    };

    {
        const nlohmann::json prev = report_of({site("a", "可用", true, false, false, "", "u1"),
                                               site("b", "需登录", true, true, false, "login-node x1",
                                                    "u2")});
        std::vector<std::string> lines;
        check(dom_report_diff(prev, prev, &lines) && lines.size() == 1 &&
                  lines[0].find("无变化") != std::string::npos,
              "diff：两轮逐项一致 ⇒ 「无变化（2 站逐项一致）」");
        check(!dom_report_diff(nlohmann::json::object(), prev, &lines),
              "diff：上一轮缺 sites ⇒ false（如实报错，**不**假装无变化）");
        check(!dom_report_diff(nlohmann::json("[1]"), prev, &lines), "diff：非对象报告 ⇒ false");
    }
    {
        const nlohmann::json prev =
            report_of({site("gone", "可用", true, false, false, "", "u1"),
                       site("stay", "可用", true, false, false, "", "u1"),
                       site("shift", "需登录", true, true, false, "login-node x1", "u2")});
        const nlohmann::json now =
            report_of({site("stay", "可用", true, false, false, "", "u1"),
                       site("shift", "可用", true, false, false, "", "u3", 1, 2),
                       site("fresh", "失败", false)});
        std::vector<std::string> lines;
        const bool               ok = dom_report_diff(prev, now, &lines);
        const std::string        text = join(lines);
        check(ok && text.find("新增      fresh：失败") != std::string::npos,
              "diff：本轮新增条目 ⇒ 「新增」（含其当前状态）");
        check(text.find("消失      gone") != std::string::npos,
              "diff：上一轮有、本轮没有 ⇒ 「消失」（条目被删 / 改名）");
        check(text.find("状态变化  shift：需登录 → 可用") != std::string::npos,
              "diff：状态变化逐行给出「旧 → 新」");
        check(text.find("页面变化  shift：u2 → u3") != std::string::npos,
              "diff：页面 URL 变化 ⇒ 单独一行（不掩在状态里）");
        check(text.find("stay") == std::string::npos, "diff：逐项一致的站**不**出现在输出里");
    }
    {
        // 旧版报告（无 `state`）⇒ 按事实字段回推 §3.2 判据（含「候选 0 且无登录迹象 ⇒ 失败」）
        nlohmann::json old_abnormal = site("legacy1", "", true, false, false, "", "u", 0, 0);
        old_abnormal.erase("state");
        nlohmann::json old_untested = site("legacy2", "", false);
        old_untested.erase("state");
        old_untested["site_error"] = "表里没有该条目（id 未知）";
        nlohmann::json old_need = site("legacy3", "", true, true, false, "login-url:/login");
        old_need.erase("state");
        const nlohmann::json prev = report_of({old_abnormal, old_untested, old_need});
        const nlohmann::json now  = report_of({site("legacy1", "可用"),
                                               site("legacy2", "可用"),
                                               site("legacy3", "可用")});
        std::vector<std::string> lines;
        check(dom_report_diff(prev, now, &lines) &&
                  join(lines).find("状态变化  legacy1：失败 → 可用") != std::string::npos,
              "diff：旧报告无 state ⇒ 回推「候选 0 + 无 Login 迹象 = 失败」（§3.2 同一判据）");
        check(join(lines).find("状态变化  legacy2：未测 → 可用") != std::string::npos,
              "diff：旧报告 site_error ⇒ 回推「未测」");
        check(join(lines).find("状态变化  legacy3：需登录 → 可用") != std::string::npos,
              "diff：旧报告有登录证据 ⇒ 回推「需登录」");
    }

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    std::printf("  → DOM 探测报告自检：%d 通过 / %d 失败\n", passed, failed);
    return failed;
}

} // namespace aiwrite::ai


