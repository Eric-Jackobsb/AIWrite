// ============================================================================
//  webview_host_upload.cpp — 附件入口只读自动识别（M8-37 resolve_file_input_selector）
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "ready_level.h"
#include "probe_verdict.h"

namespace aiwrite::web {


// M8-37：**只读**枚举页面里的 `input[type=file]` —— 供 attach_selector 未回填 / 未命中时兜底
//  * 与 `--upload-selftest` 的「选择器未命中」诊断同源：枚举 → 挑最优 → 给出可回填清单
//  * 只读：不点击、不写入、不读取任何值；失败一律如实报错（绝不静默换选择器）
constexpr const char* kFileInputProbeScript = R"JS(
(function () {
  const out = { count: 0, items: [] };
  try {
    const nodes = document.querySelectorAll('input[type=file]');
    out.count = nodes.length;
    for (let i = 0; i < nodes.length && i < 8; i++) {
      const el = nodes[i];
      const cls = (typeof el.className === 'string') ? el.className : '';
      const r = el.getBoundingClientRect();
      const first = cls.replace(/\s+/g, ' ').trim().split(' ')[0];
      out.items.push({ accept: (el.getAttribute('accept') || '').slice(0, 120),
                       multiple: el.multiple === true,
                       visible: r.width > 1 && r.height > 1,
                       selector: el.id ? ('#' + el.id) : (first ? ('.' + first) : 'input[type=file]') });
    }
  } catch (e) { out.count = -1; }
  return out;
})();
)JS";


bool resolve_file_input_selector(const LoginRequest& site_request, int timeout_ms,
                                 std::string* selector, std::string* detail, std::string* error)
{
    if (selector != nullptr) {
        selector->clear();
    }
    if (detail != nullptr) {
        detail->clear();
    }

    // ---- M_patchC PW-03（不变量 I25）：先等**附件入口级**就绪 ----
    //  * 真机 41 ms 假失败 = 附件区懒加载未挂载时枚举到 0 个，却被归因为「形态不支持」（`G6`）
    //  * 这里先等到 L3（含附件入口）；未就绪 → 如实报「页面未就绪」，**不**报形态结论
    const int   need_ready = kReadyUpload;
    std::string ready_error;
    const int   reached = waitReady(site_request, need_ready,
                                     std::min(timeout_ms > 0 ? timeout_ms : 10000, 15000),
                                     &ready_error);
    if (!ready_satisfied(reached, need_ready)) {
        if (error != nullptr) {
            *error = ready_error.empty()
                         ? web_error_text(WebErrorKind::NotReady, site_request.owner,
                                          siteKeyOf(site_request.url), "未枚举附件入口")
                         : ready_error;
        }
        return false;
    }

    std::string raw;
    std::string why;
    if (!run_script_sync(site_request, kFileInputProbeScript, timeout_ms > 0 ? timeout_ms : 10000, &raw,
                         &why)) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner,
                                    siteKeyOf(site_request.url),
                                    "读取页面 `input[type=file]` 失败：" + why);
        }
        return false;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(raw);
        const int            count  = parsed.value("count", 0);
        if (count < 0) {
            if (error != nullptr) {
                *error = "页面脚本异常：无法枚举 `input[type=file]`";
            }
            return false;
        }
        const nlohmann::json items = parsed.value("items", nlohmann::json::array());
        std::string list = "页面里 `input[type=file]` 共 " + std::to_string(count) + " 个";
        if (count == 0) {
            // 归因分离（`G6`）：就绪三级**已通过**（上面刚等到附件入口）⇒ 这里是**竞态**
            // （懒加载卸载 / 换帧），不再归因为「该站点用拖拽 / 粘贴入口」
            list += "（就绪检查已通过，但枚举瞬间没有 `input[type=file]` —— 多为懒加载竞态；请重试）";
        }
        std::string with_image;   // accept 含 image（最可能是图片入口）
        std::string hidden_input; // 隐藏且带 accept（附件入口通常隐藏）
        std::string first_input;  // 文档顺序首个
        for (const auto& item : items) {
            const std::string sel    = item.value("selector", std::string("?"));
            const std::string accept = item.value("accept", std::string());
            const bool        visible = item.value("visible", false);
            list += "\n    · " + sel + "（accept=" + (accept.empty() ? std::string("(无)") : accept) +
                    "，multiple=" + (item.value("multiple", false) ? "true" : "false") + "，可见=" +
                    (visible ? "是" : "否") + "）";
            if (first_input.empty()) {
                first_input = sel;
            }
            if (with_image.empty() && accept.find("image") != std::string::npos) {
                with_image = sel;
            }
            if (hidden_input.empty() && !visible && !accept.empty()) {
                hidden_input = sel;
            }
        }
        // 取值策略（确定性，口径写在这里）：accept 含 image → 隐藏且带 accept → 文档顺序首个
        const std::string picked = !with_image.empty()    ? with_image
                                   : !hidden_input.empty() ? hidden_input
                                                           : first_input;
        if (picked.empty()) {
            if (error != nullptr) {
                // **归因分离**（`PW-05` · `W-D6`）：L3（含附件入口）**已通过**却枚举不到 ⇒
                // 这是「无入口 / 竞态」，**不得**再写成「页面未就绪」，也不得写「形态不支持」以外的猜测
                *error = web_error_text(
                    WebErrorKind::NoEntry, site_request.owner, siteKeyOf(site_request.url),
                    "枚举到 " + std::to_string(count) + " 个 `input[type=file]`（就绪已达：" +
                        ready_reached_text(reached) + "；就绪检查此前已通过 ⇒ 多为懒加载竞态）");
            }
            if (detail != nullptr) {
                *detail = list;
            }
            return false;
        }
        if (selector != nullptr) {
            *selector = picked;
        }
        if (detail != nullptr) {
            *detail = list + "\n    ⇒ 自动识别选择器：" + picked;
        }
        return true;
    }
    catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("枚举结果解析失败：") + ex.what() + "；原始结果：" + raw.substr(0, 160);
        }
        return false;
    }
}

long long solve_pow_via_page(const std::string& site_url, const std::string& challenge_json,
                             int timeout_ms, std::string* error, const WebOwner& owner)
{
    if (challenge_json.empty()) {
        if (error != nullptr) {
            *error = "挑战为空";
        }
        return -1;
    }

    // 确保有可用登录窗口（**按目标站点**；PB2-23：不再用无参 ensure_session → 内置默认站点）
    //  * `PW-02`：归属显式 —— 默认为**未标注**（内置协议路径 `web_chat` 尚未把节点 owner 穿下来，
    //    故它使用「未标注」归属在该站点的那一个窗口；节点级 DOM 路径不受影响）
    LoginRequest site_request;
    if (!site_url.empty()) {
        site_request.url = site_url;
    }
    site_request.owner            = owner;
    site_request.offscreen        = true;
    site_request.probe_after_load = true;
    std::string boot_error;
    if (!ensureSession(site_request, 15000, &boot_error)) {
        if (error != nullptr) {
            *error = boot_error;
        }
        return -1;
    }

    const std::string          site = siteKeyOf(site_request.url);
    std::shared_ptr<WindowCtx> ctx  = ctx_find(site_request.owner, site);
    if (!ctx_alive(ctx) || ctx->webview == nullptr) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site,
                                    "窗口 / 页面尚未创建完成（PoW 求解需要该站点页面）");
        }
        return -1;
    }
    CtxScope scope(ctx.get());

    const HWND window = st().window.load();
    {
        std::lock_guard<std::mutex> lock(st().pow_mutex);
        st().pow_challenge_json = challenge_json;
        st().pow_done           = false;
        st().pow_answer         = -1;
        st().pow_error.clear();
    }
    ::PostMessageW(window, kSolvePowMessage, 0, 0);

    std::unique_lock<std::mutex> lock(st().pow_mutex);
    const auto                   wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 60000);
    if (!st().pow_cv.wait_for(lock, wait_ms, [] { return st().pow_done; })) {
        if (error != nullptr) {
            *error = web_error_text(WebErrorKind::NotReady, site_request.owner, site,
                                    "页面内 PoW 求解超时");
        }
        return -1;
    }
    if (error != nullptr) {
        *error = st().pow_error;
    }
    return st().pow_answer;
}

// PB2-23：探测主体抽出，供「默认站点」与「按条目」两个入口共用（行为逐字一致）
int protocol_probe_with(const LoginRequest& base_request, const ai::ProviderSpec* spec,
                               const std::string& site_label, int timeout_seconds)
{
    attach_parent_console();

    const int timeout = timeout_seconds > 0 ? timeout_seconds : 30;
    log::info("[网页版探测] 自检开始（离屏窗口，超时 " + std::to_string(timeout) + " 秒）");
    if (!site_label.empty()) {
        std::printf("[网页版探测] 站点：%s\n", site_label.c_str());
    }

    LoginRequest request            = base_request;
    request.offscreen              = true;
    request.timeout_seconds        = timeout;
    request.probe_after_load       = true; // 页面加载完成后自动执行（适用 → 协议探测；不适用 → 只读诊断）
    request.auto_close_after_probe = true; // 探测完成即关窗退出

    // L4（PB2-28 / 不变量 I16）：**不适用**协议探测的站点（如 DOM）→ 说明清楚，不再打印「未取得凭证」
    if (!request.probe_applicable) {
        std::printf("[网页版探测] 协议探测：**不适用**（该站点不是内置协议站点）—— 改跑**只读诊断**：\n"
                    "             URL / 标题 / localStorage 键名与个数 / Cookie 名与个数 / 输入框与按钮候选数；\n"
                    "             **不**请求任何站点端点，**不**读取 localStorage 的值（含 userToken）\n");
    }

    LoginWindow window(selftest_owner()); // `PW-02`：探测自检用独立归属（不与节点 / 界面窗口互抢）
    std::string error;
    if (!window.start(request, &error)) {
        std::printf("[网页版探测] 启动失败: %s\n", error.c_str());
        return 1;
    }
    window.join();

    const Session      session = SessionStore::instance().snapshot();
    const Session      site_session = SessionStore::instance().snapshot(siteKeyOf(request.url));
    const ProbeResult& probe   = session.probe;

    // ---- L4（PB2-28 / I16）：不适用站点 → 只读诊断输出 + **站点无关**登录态结论 ----
    if (!request.probe_applicable) {
        std::printf("\n===== 网页版只读诊断（脱敏；该站点不适用协议探测）=====\n");
        std::printf("诊断            : %s\n", probe.endpoints_report.c_str());
        std::printf("localStorage 键 : %s\n", probe.local_storage_keys.c_str());
        std::printf("错误            : %s\n", probe.error.c_str());

        const ai::WebSessionVerdict verdict =
            ai::webSessionState(spec, webSessionEvidence(site_session));
        std::printf("站点            : %s\n", request.url.c_str());
        std::printf("登录态          : %s（Cookie %zu 条）\n",
                    ai::webSessionStateLabel(verdict.state).c_str(), site_session.cookies.size());
        std::printf("判据            : %s\n", verdict.reason.c_str());
        std::printf("================================================\n");
        if (verdict.state != ai::WebSessionState::loggedIn) {
            std::printf("[网页版探测] 提示：该站点未登录 → 请点界面参数面板的「打开登录窗口」**手动登录**；\n"
                        "             该站点不适用协议探测，登录后**无需**再点「探测网页版协议」\n");
        }
        log::info("[网页版探测] 只读诊断结束（不适用协议探测）：" + verdict.reason);

        SessionStore::instance().clear(siteKeyOf(request.url)); // 自检不留会话
        if (!probe.ok) {
            return 1; // 诊断脚本本身失败（页面没加载 / WebView2 异常）
        }
        return verdict.state == ai::WebSessionState::loggedIn ? 0 : 2; // 2 = 未登录 / 未确认
    }

    std::printf("\n===== 网页版协议探测（脱敏）=====\n");
    std::printf("localStorage 键 : %s\n", probe.local_storage_keys.c_str());
    std::printf("userToken      : %s\n",
                probe.has_user_token ? probe.user_token_masked.c_str() : "未获取（可能未登录）");
    std::printf("settingsJwt    : %s\n", probe.settings_jwt_masked.c_str());
    std::printf("challenge      : %s\n", probe.challenge_json.c_str());
    std::printf("端点探测       : %s\n", probe.endpoints_report.c_str());
    std::printf("错误           : %s\n", probe.error.c_str());
    std::printf("================================\n");

    // M_patchAB §3.3（PB2-25）：判据**收紧** —— 「有凭证」不再等于 PASS：
    //  * 还需**判据端点** HTTP 200 且 `code=0`（旧判据在端点 `code=40003` 时会**假 PASS**）
    //  * 结论四分类与可操作下一步由纯函数 `judge_probe_result()` 给出（离线可断言）
    const std::string required_path = request.probe_paths.empty()
                                          ? std::string("/api/v0/users/current")
                                          : request.probe_paths.front();
    const ProbeJudgement judgement =
        judge_probe_result(probe, required_path, /*probe_applicable=*/true);
    std::printf("结论           : %s\n", probeVerdictName(judgement.verdict).c_str());
    std::printf("判据           : %s\n", judgement.reason.c_str());
    std::printf("下一步         : %s\n", judgement.next_step.c_str());
    std::printf("================================\n");
    if (judgement.needs_relogin) {
        std::printf("[网页版探测] 提示：**会话已失效** → 请在界面参数面板点「打开登录窗口」重新登录；\n"
                    "             界面错误条同样会给出「重新登录」入口（PB-07）\n");
    }
    std::printf("[网页版探测] 结果：%s（退出码 %d）\n", judgement.pass ? "PASS" : "FAIL",
                judgement.pass ? 0 : 1);
    log::info("[网页版探测] 自检结束：" + probe.summary() + " ｜ 结论=" +
              probeVerdictName(judgement.verdict) + "：" + judgement.reason);

    SessionStore::instance().clear(); // 自检不留会话
    return judgement.pass ? 0 : 1;
}

// PB2-23：「默认站点」入口（旧行为，逐字不变 —— 守 I2）

} // namespace aiwrite::web
