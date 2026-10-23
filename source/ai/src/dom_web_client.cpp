// ============================================================================//  dom_web_client.cpp — 会话聊天主体：clamp/cfg/脚本渲染/formatAnswerText/domChat//  * ARCH-Z1b：自 2398 行单文件 `dom_web_client.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `dom_web_client_detail.h`；公开 API 见 `dom_web_client.h`// ============================================================================
#include "dom_web_client_detail.h"
#include "dom_web_client_scripts.h"
#include "dom_web_client_scripts_poll.h"

namespace aiwrite::ai {

void clampPollParams(const ProviderWebSpec& site, int* poll_ms, int* max_polls)
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

std::string domCfgJson(const DomChatRequest& request)
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

std::string domKickoffScript() { return std::string(kKickoffScript); }
// PW-07f：真实发送链路的三段式脚本（与 dom_kickoff_script 同为**常量脚本**读取者）
std::string domPrepareScript() { return std::string(kPrepareScript); }
std::string domSendVerifyScript() { return std::string(kSendVerifyScript); }
std::string domClickSendScript() { return std::string(kClickSendScript); }
std::string domPollScript() { return std::string(kPollScript); }
std::string domProbeScript() { return std::string(kProbeScript); }

// ---- PW-07g：答案文本排版（纯函数 · 离线断言 `VB2-22⑦`）--------------------------------
//  为什么放在 C++ 侧：页面脚本只做「**无损搬运**」，排版必须是**确定性 + 可离线断言**的一步
//  （仓库既有口径：纯函数进 `api_probe`）。历史上「判据需要归一 ⇒ 顺手把产物也归一」
//  正是本次真机事故（段落换行丢失）的成因，故这里把两步**显式分开**。
std::string formatAnswerText(const std::string& raw)
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

DomChatResult domChat(const DomChatRequest& request)
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
        const UploadResult upload = uploadImages(plan);
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
    if (!web::ensureSession(site_request, boot_timeout, &boot_error)) {
        result.warning = "未取到内存凭证（该站点可能未登录，或该站点不产生 userToken）：" + boot_error;
    }

    // ---- 3) 页面内驱动：注入提示词 → 触发发送 → 轮询答案（全部走 web::run_script_sync）----
    int poll_ms   = 0;
    int max_polls = 0;
    clampPollParams(request.site, &poll_ms, &max_polls);

    // ---- M_patchC PW-03（不变量 I25）：等**交互态**（可见输入候选）----
    //  * 只作为**诊断 + 短等待**：`composer` 与站点强相关，真正的自动识别在 kickoff 脚本里
    //    （打分 + 明确报错）⇒ 未达 L3 **不阻断**（避免把「选择器形态差异」升级成硬失败）
    if (!probeIsApplicable(request.site)) { // DOM 站点才需要（内置协议站点走 web_chat）
        std::string ready_why;
        const int   reached = web::waitReady(site_request, web::kReadyScript,
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
                              "window.__aiwriteDom = " + domCfgJson(request) + "; 'ok';", 10000,
                              &raw, &script_error)) {
        result.error = "注入站点配置失败：" + script_error;
        return finish();
    }
    // ---- 3a) 页面准备（JS）：发现输入框 + 聚焦 + 清空 + 记答案基线（**不写入、不发送**）----
    //  * 旧写法（`kKickoffScript`）是「脚本自己写入 + 脚本自己发合成回车」⇒ 豆包 / Kimi 这类
    //    富文本编辑器**忽略**不可信输入（真机 2026-10-08：连续三次 `候选池 0`、页面 60 秒无新消息）
    if (!web::run_script_sync(site_request, domPrepareScript(), 15000, &raw, &script_error)) {
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
            if (web::run_script_sync(site_request, domClickSendScript(), 10000, &click_raw,
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
            if (!web::run_script_sync(site_request, domKickoffScript(), 15000, &fb_raw, &fb_err)) {
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
            if (!web::run_script_sync(site_request, domSendVerifyScript(), 10000, &v_raw, &v_err)) {
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
        if (web::run_script_sync(site_request, domKickoffScript(), 15000, &fb_raw, &fb_err)) {
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
        if (web::run_script_sync(site_request, domClickSendScript(), 10000, &click_raw, &click_err)) {
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
        if (web::run_script_sync(site_request, domKickoffScript(), 15000, &fb_raw, &fb_err)) {
            send_mode += "+synth2";
            sample_sent(12);
            if (!sent) { // 同步后再点一次可见「发送」按钮（声明值优先）
                std::string click_raw;
                std::string click_err;
                if (web::run_script_sync(site_request, domClickSendScript(), 10000, &click_raw,
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
        if (!web::run_script_sync(site_request, "window.__aiwriteDom = " + domCfgJson(request) + "; 'ok';",
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
        if (!web::run_script_sync(site_request, domPollScript(), 15000, &poll_raw, &poll_error)) {
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
                result.text = formatAnswerText(text);
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

} // namespace aiwrite::ai

