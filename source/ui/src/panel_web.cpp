
#include "panel_internal.h"

#include "provider_resolve.h"
#include "provider_spec.h"
#include "log.h"
#include "paths.h"
#include "session_store.h"
#include "webview_host.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
//  参数面板 · 网页会话 + 提供商下拉域（CLN-12 拆分 · 2026-10-20）
//  自 property_panel.cpp 原样搬移（行为等价，仅分文件）
// ============================================================================


namespace aiwrite::ui {

using engine::Node;
using engine::Param;

namespace {
// ------------------------------------------------------------ 生效站点上下文 --
// M_patchB L1 续（PB2-17/18）：ProviderConfig 的「生效条目 → 站点参数 → 会话键」
//  * 生效条目 = 按**该节点自身参数**解析（PB2-20：ProviderConfig 自己就是配置来源，不看 provider 连线）
//  * 站点参数**只**来自条目自己的 `web` 段（PB2-22 / 决策 D-22② / `I14`：**不**回落内置默认站点）
//  * 会话键 = 站点 origin（SessionStore 按站点归档；决策 D-19）
struct WebSiteContext {
    engine::EffectiveProvider effective;
    ai::ProviderWebSpec       site;
    std::string               provider_id;    // 生效 web 条目 id（非网页版条目 = 空）
    bool                      on_web_entry = false; // 生效条目本身是不是网页版条目
    bool                      site_usable  = false; // 站点是否可用（false → 错误块，**不打开任何窗口**）
    std::string               site_error;     // 不可用原因（含三条引导；空 = 可用）
    std::string               field_warnings; // 可选字段缺失（回落 + 警告；决策 D-26）
    std::string               login_url;      // 生效登录页（仅 site_usable 时有意义）
    std::string               site_key;       // 站点键（origin）
    web::LoginRequest         manual;         // 「打开登录窗口」用（页面加载后自动探测）
    web::LoginRequest         probe;          // 「探测网页版协议」用
};

WebSiteContext web_site_context(const engine::Node& node, const engine::Graph& graph)
{
    WebSiteContext ctx;
    ctx.effective      = engine::resolveDisplayProvider(graph, node); // PB2-20：按自身条目解析
    ctx.on_web_entry   = ctx.effective.is_web();
    ctx.site           = ai::strictWebSpecFor(ctx.effective.spec);        // PB2-22：**不**回落
    ctx.provider_id    = ai::strictWebProviderIdFor(ctx.effective.spec);
    ctx.site_error     = ai::webSiteError(ctx.effective.spec);
    ctx.site_usable    = ctx.site_error.empty();
    ctx.field_warnings = ai::webSiteFieldWarnings(ctx.effective.spec);
    if (ctx.site_usable) {
        ctx.manual    = web::interactive_login_request(ctx.site, ctx.provider_id);
        ctx.probe     = web::probe_login_request(ctx.site, ctx.provider_id);
        ctx.login_url = ctx.manual.url;
        ctx.site_key  = web::login_request_site(ctx.manual);
    }
    return ctx;
}

// 该站点用于界面展示的 Cookie 名（**只**取条目自己的 `cookie_names`；空 = 该条目未声明 → 不显示具体 Cookie）
//  * L4（PB2-27 / 不变量 I15）：界面**不得**再回落到任何厂商专有 Cookie 名（一律以条目为准）
std::string site_cookie_name(const WebSiteContext& ctx)
{
    return ctx.site.cookie_names.empty() ? std::string() : ctx.site.cookie_names.front();
}

// 按**站点**注销（PB2-19）：清该站点的内存会话 + 删该 origin 的 Cookie / localStorage
// （**不影响**其他站点；不删除登录 profile）
void logout_web_session(const WebSiteContext& ctx)
{
    std::string error;
    if (!web::logoutSite(ctx.manual, 20000, &error)) {
        log::warn("网页版按站点注销失败: " + error);
    }
}

// 高级操作：删除**整个**登录 profile（= 清掉所有站点；PB2-19 / R14 边界说明）
void delete_web_profile()
{
    web::loginWindow().requestClose();
    web::loginWindow().join();
    web::SessionStore::instance().clearAll();

    std::error_code ec;
    const std::filesystem::path profile = paths::webview2_profile();
    std::filesystem::remove_all(profile, ec);
    if (ec) {
        log::warn("网页版注销：profile 目录删除失败 " + profile.string() + "（" + ec.message() + "）");
    }
    else {
        log::info("网页版注销：profile 已删除 " + profile.string() + "（**全部站点**，下次需重新登录）");
    }
}
// ============================================================================
//  M_patchB L4 / v16（**仅界面层**）：提供商下拉的「合并显示」
//
//  背景：配置表里同一家 AI 常有两条 —— 官方 API 条目（`deepseek`）与网页版条目（`deepseek-web`），
//        下拉里就出现两个名字（且 `-web` 后缀像内部 id，用户困惑）。
//  规则（**只改界面**：配置表 / `ai/**` / `engine/**` / `nodes/**` 一律不动）：
//    ① `xxx-web` 存在同名无后缀条目 `xxx`（如 `deepseek`+`deepseek-web`、`gemini`+`gemini-web`）
//       → **合并为一项**（显示 `xxx`，下拉里不再出现 `xxx-web`）：
//          · 「模式」= web      → 节点 `provider` 写入 `xxx-web`（运行期照常解析该网页站点）
//          · 「模式」= official → 写入 `xxx`
//    ② `xxx-web` 没有同名无后缀条目（如 `kimi-web` / `chatgpt-web`）→ 仍**独立一项**，
//       但显示名**去掉 `-web` 后缀**（即下拉里永远不会出现 `-web` 字样）
//    ③ 其它条目（官方 API / 自定义）原样显示
//  关键性质：合并只是「一个显示项 ↔ 两个真实 id」。写进节点的 id **始终是表内真实存在的 id**，
//            因此生效解析、`--provider-*` 自检、工作流 JSON、网页版会话键控全部无需改动。
// ============================================================================
// `base_id` 是否有网页版孪生（`base_id-web` 且 kind=web）
std::string web_twin_of(const ai::ProviderSpecs& table, const std::string& base_id)
{
    const std::string       candidate = base_id + "-web";
    const ai::ProviderSpec* spec      = table.find(candidate);
    return (spec != nullptr && spec->kind == "web") ? candidate : std::string();
}

// `id` 是否是「已被合并」的 web 条目（存在同名无后缀条目）
bool is_merged_web_id(const ai::ProviderSpecs& table, const std::string& id)
{
    const std::string suffix = "-web";
    if (id.size() <= suffix.size() ||
        id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    return table.find(id.substr(0, id.size() - suffix.size())) != nullptr;
}
} // namespace
// ---------------------------------------------------------------- 网页版会话 --
// 登录窗口（独立线程，不阻塞主界面）：进程内单例见 web::login_window()
bool isWebMode(const Node& node)
{
    const Param* mode = node.findParam("mode");
    return mode != nullptr && mode->text() == "web";
}
// ProviderConfig 节点在 web 模式下显示「生效站点 + 会话状态 + 登录入口」（PB2-17/18/19）
//  * 站点身份唯一来源 = **生效条目自己的 `web` 段**（不变量 `I11` / `I14`）：登录页 / 窗口标题 / 探测路径 / Cookie 名
//  * 站点不可用（非网页版条目 / 缺 `web.login_url`）→ **错误块 + 三条引导 + 一键改选**，
//    且**不打开任何窗口**（PB2-22 / 决策 D-22②）
//  * 返回 true = 就地改了节点参数（外层据此压快照 + 标记变更）
bool drawWebSessionSection(engine::Node& node, const engine::Graph& graph)
{
    const WebSiteContext ctx     = web_site_context(node, graph);
    bool                 changed = false;

    ImGui::Separator();
    ImGui::TextUnformatted("网页版会话（Cookie 只存内存，程序退出即销毁）");

    if (!ctx.site_usable) {
        // ---- 站点不可用：明确报错 + 一键改选网页版条目；两个会开窗的按钮**禁用** ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("✗ %s", ctx.site_error.c_str());
        ImGui::PopStyleColor();

        const std::vector<std::string> web_ids = ai::providerSpecs().ids("web");
        if (!web_ids.empty()) {
            if (ImGui::Button(("把「提供商」改为「" + web_ids.front() + "」").c_str())) {
                if (engine::Param* provider = node.findParam("provider")) {
                    provider->value = web_ids.front();
                    if (engine::Param* mode = node.findParam("mode")) {
                        mode->value = std::string("web"); // 建议值（可再改回）
                    }
                    changed = true;
                    log::info("参数变更: " + node.id + ".provider → " + web_ids.front() +
                              "（按 D-22② 引导改选网页版条目，可撤销）");
                }
            }
        }
        ImGui::BeginDisabled(true);
        ImGui::Button("打开登录窗口（WebView2）", ImVec2(-FLT_MIN, 0.0f));
        ImGui::Button("探测网页版协议（dev）", ImVec2(-FLT_MIN, 0.0f));
        ImGui::EndDisabled();
        ImGui::TextDisabled("（站点不可用：**不会**回落到内置默认站点 —— 请先选定网页版站点条目）");
        return changed;
    }

    const web::Session session = web::SessionStore::instance().snapshot(ctx.site_key);

    // ---- 生效站点（按条目）----
    ImGui::TextDisabled("站点条目：%s（来源 %s）",
                        ctx.effective.display.empty() ? ctx.effective.provider.c_str()
                                                      : ctx.effective.display.c_str(),
                        ctx.effective.spec_origin.empty() ? "未知" : ctx.effective.spec_origin.c_str());
    if (!ctx.field_warnings.empty()) {
        ImGui::TextDisabled("%s", ctx.field_warnings.c_str()); // 决策 D-26：回落 + 警告
    }
    ImGui::TextDisabled("登录页：%s", ctx.login_url.c_str());
    ImGui::TextDisabled("适配器：%s",
                        ctx.site.adapter.empty() ? "builtin" : ctx.site.adapter.c_str());

    // ---- 该站点会话状态（L4 / PB2-27：**站点无关**判据 —— 不看 userToken，不变量 I15）----
    const ai::WebSessionVerdict verdict =
        ai::webSessionState(ctx.effective.spec, web::webSessionEvidence(session));
    const std::string cookie_name = site_cookie_name(ctx);
    if (verdict.state == ai::WebSessionState::loggedIn) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "状态：已登录（该站点，Cookie %zu 条）",
                           session.cookieCount());
        ImGui::TextDisabled("来源：%s", session.url.c_str());
        ImGui::TextDisabled("更新：%s", session.updated_at.c_str());
        if (!cookie_name.empty()) {
            if (const web::Cookie* cookie = session.find(cookie_name)) {
                ImGui::TextDisabled("%s = %s", cookie_name.c_str(),
                                    web::maskValue(cookie->value).c_str());
            }
        }
    }
    else {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "状态：%s（该站点）",
                           ai::webSessionStateLabel(verdict.state).c_str());
    }
    ImGui::TextDisabled("%s", verdict.reason.c_str());
    if (verdict.state != ai::WebSessionState::loggedIn) {
        ImGui::TextDisabled("点「打开登录窗口」在该站点**手动登录**；登录后**无需**再点「探测网页版协议」。");
    }

    // 协议探测状态（M4-06/M4-08 逆向用）
    //  * L4（PB2-27 / D-28①）：`userToken` 行**仅当**条目配了 `token_expr` 才显示；
    //    否则说明「本条目为 DOM 站点」（生成不依赖 userToken）
    //  * L4（PB2-27 / 不变量 I16）：**探测不适用**的站点不再显示红字「探测错误」（那是 DeepSeek 端点 404）
    const web::ProbeResult& probe = session.probe;
    if (ai::webShowsUserToken(ctx.effective.spec)) {
        if (probe.has_user_token) {
            ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "userToken：%s",
                               probe.user_token_masked.c_str());
        }
        else {
            ImGui::TextDisabled("userToken：未获取（网页版接口需要它，请先登录）");
        }
    }
    else {
        ImGui::TextDisabled("本条目为 DOM 站点：登录态由浏览器 profile 维持（生成不依赖 userToken）");
    }
    if (!probe.challenge_json.empty()) {
        ImGui::TextDisabled("PoW 挑战：已获取（%zu 字节，见 Console / app.log）",
                            probe.challenge_json.size());
    }
    if (!probe.error.empty() && ai::probeIsApplicable(ctx.site)) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "探测错误：%s", probe.error.c_str());
    }

    web::LoginWindow& window      = web::loginWindow();
    // `PW-02`：界面窗口是**独立归属**（`界面#登录窗口`）—— 判定一律按该归属查窗口表
    const bool        window_here = web::windowOnSite(web::ui_window_owner(), ctx.site_key);
    if (window.running()) {
        if (window_here) {
            ImGui::TextDisabled("登录窗口已打开（该站点）：请在窗口中完成登录，然后关闭它。");
        }
        else {
            ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "登录窗口正开着其他站点：%s",
                               web::currentWindowSite(web::ui_window_owner(), ctx.site_key).c_str());
            ImGui::TextDisabled("点「打开登录窗口」会按串行策略切到本节点站点（先关旧窗）");
        }
        ImGui::TextDisabled("当前：%s", window.status().c_str());
        if (ImGui::Button("关闭登录窗口", ImVec2(-FLT_MIN, 0.0f))) {
            window.requestClose();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("打开登录窗口（WebView2）", ImVec2(-FLT_MIN, 0.0f))) {
        // 站点参数来自**生效条目**（PB2-17）；页面加载完成后自动探测一次（与改造前一致）
        if (window.running() && !window_here) {
            window.requestClose(); // 串行复用：先关旧站点窗口（PB2-19 / D-20）
            window.join();
        }
        std::string error;
        if (!window.running()) {
            if (window.start(ctx.manual, &error)) {
                log::info("网页版登录窗口已打开（站点 " + ctx.login_url +
                          "；独立线程，页面加载后自动探测凭证）");
            }
            else {
                log::warn("网页版登录窗口打开失败: " + error);
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("站点取自「生效条目」：%s\n"
                          "profile 会记住登录态（重启后仍在）；窗口打开后会复读该站点 Cookie，面板随即显示登录态。\n"
                          "切换网页版条目时会改用该条目的登录页（先关旧窗口）。",
                          ctx.login_url.c_str());
    }

    ImGui::Spacing();
    // L4（PB2-28 / I16）：协议探测只对**内置协议站点**适用；DOM 站点改跑**只读诊断**
    const bool probe_applicable = ai::probeIsApplicable(ctx.site);
    if (ImGui::Button(probe_applicable ? "探测网页版协议（dev）"
                                       : "只读诊断（该站点不适用协议探测）",
                      ImVec2(-FLT_MIN, 0.0f))) {
        if (web::windowOnSite(web::ui_window_owner(), ctx.site_key)) {
            web::requestProtocolProbe(); // 窗口已开在该站点：直接在当前页面里探测 / 诊断
        }
        else {
            if (window.running()) {
                window.requestClose();
                window.join();
            }
            std::string error;
            if (!window.start(ctx.probe, &error)) {
                log::warn("启动探测窗口失败: " + error);
            }
        }
        log::info(probe_applicable
                      ? ("已触发网页版协议探测（站点 " + ctx.login_url + "；结果写入 Console 与 app.log）")
                      : ("已触发只读诊断（站点 " + ctx.login_url +
                         "；该站点不适用协议探测：只读页面信息，不请求端点、不读 userToken）"));
    }
    if (ImGui::IsItemHovered()) {
        if (probe_applicable) {
            ImGui::SetTooltip("在已登录页面内读取 userToken 并请求 PoW 挑战（同源，绕过跨域与反爬）；\n"
                              "Token 只以脱敏形式记录");
        }
        else {
            ImGui::SetTooltip("该条目不是内置协议站点（无 PoW / 站点端点）：只读检查页面 URL / 标题 /\n"
                              "localStorage 键名 / Cookie 名 / 输入框候选数 —— **不**请求任何站点端点，\n"
                              "也**不**读取 userToken；登录态由该站点 Cookie 判定。");
        }
    }

    if (verdict.state == ai::WebSessionState::loggedIn) {
        ImGui::Spacing();
        if (ImGui::Button("注销该站点（清会话 + 删该站点 Cookie）", ImVec2(-FLT_MIN, 0.0f))) {
            logout_web_session(ctx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("只影响本站点：内存会话 + 该 origin 的 Cookie / localStorage；\n"
                              "其他站点（例如另一个网页版条目）的登录态不受影响。");
        }
    }

    // ---- 已登录站点列表（多站点并存的可视化 + 逐站点注销；PB2-18/19）----
    const std::vector<std::string> sites = web::SessionStore::instance().sites();
    if (!sites.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("已登录站点（%zu）", sites.size());
        for (const std::string& site : sites) {
            const web::Session item = web::SessionStore::instance().snapshot(site);
            ImGui::PushID(site.c_str());
            ImGui::TextDisabled("%s（Cookie %zu%s）", site.c_str(), item.cookieCount(),
                                item.user_token.empty() ? "" : "，有凭证");
            ImGui::SameLine();
            if (ImGui::SmallButton("注销")) {
                web::LoginRequest other;
                other.url = site; // 只需 origin：注销按站点
                std::string error;
                if (!web::logoutSite(other, 20000, &error)) {
                    log::warn("按站点注销失败（" + site + "）: " + error);
                }
            }
            ImGui::PopID();
        }
    }

    // ---- 高级：删除整个登录 profile（= 清掉**所有**站点；二次确认）----
    ImGui::Spacing();
    static bool confirm_delete_profile = false;
    if (!confirm_delete_profile) {
        if (ImGui::SmallButton("高级：删除整个登录 profile（所有站点）")) {
            confirm_delete_profile = true;
        }
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "将删除 ~/.brain-ai/webview2 —— 所有站点都要重新登录");
        if (ImGui::SmallButton("确认删除##profile")) {
            delete_web_profile();
            confirm_delete_profile = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("取消##profile")) {
            confirm_delete_profile = false;
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped("登录窗口内：Ctrl+Alt+C 立即重新提取 Cookie，ESC 关闭窗口。");

    return changed;
}
// 构建下拉项（每次绘制重建：条目很少，且「重新加载配置表」后立即生效）
//  配对来源两条（**只在界面层**）：
//   ① 同名后缀规则：`xxx` ↔ `xxx-web`（deepseek / gemini …）
//   ② 品牌别名表：id 不同名但同属一家（openai ↔ chatgpt-web、anthropic ↔ claude-web、zhipu ↔ chatglm-web）
std::vector<ProviderChoice> buildProviderChoices()
{
    const ai::ProviderSpecs& table = ai::providerSpecs();

    struct AliasPair {
        const char* official;
        const char* web;
    };
    static const AliasPair kAliases[] = {
        {"openai", "chatgpt-web"},     // OpenAI（API） ↔ ChatGPT 网页版
        {"anthropic", "claude-web"},   // Anthropic Claude（API） ↔ Claude 网页版
        {"zhipu", "chatglm-web"},      // 智谱 GLM（API） ↔ 智谱清言网页版
        {"deepseek", "deepseek-web"},  // 与后缀规则一致（冗余写明，便于阅读）
        {"gemini", "gemini-web"},
    };

    std::vector<ProviderChoice> out;
    out.reserve(table.items.size());
    std::vector<std::string> merged_web_ids; // 已被合并的 web id（不再单列）
    for (const ai::ProviderSpec& spec : table.items) {
        if (spec.kind != "web") {
            continue;
        }
        if (is_merged_web_id(table, spec.id)) {
            merged_web_ids.push_back(spec.id);
            continue;
        }
        for (const AliasPair& alias : kAliases) {
            if (spec.id == alias.web && table.find(alias.official) != nullptr) {
                merged_web_ids.push_back(spec.id);
                break;
            }
        }
    }
    const auto is_merged = [&merged_web_ids](const std::string& id) {
        return std::find(merged_web_ids.begin(), merged_web_ids.end(), id) != merged_web_ids.end();
    };

    for (const ai::ProviderSpec& spec : table.items) {
        if (spec.kind == "web" && is_merged(spec.id)) {
            continue; // → 合并进它的官方条目，不再单列
        }
        ProviderChoice item;
        if (spec.kind == "web") {
            item.id    = spec.id;
            item.label = spec.id.size() > 4 ? spec.id.substr(0, spec.id.size() - 4) : spec.id;
        }
        else {
            item.id     = spec.id;
            item.label  = spec.id;
            item.web_id = web_twin_of(table, spec.id); // ① 同名后缀
            if (item.web_id.empty()) {                 // ② 品牌别名
                for (const AliasPair& alias : kAliases) {
                    if (spec.id == alias.official && table.find(alias.web) != nullptr) {
                        item.web_id = alias.web;
                        break;
                    }
                }
            }
        }
        out.push_back(item);
    }

    // 诊断（仅在结果变化时打印一次）：便于核对「合并后下拉里到底有哪些项」
    {
        static std::string last_signature;
        std::string        signature;
        for (const ProviderChoice& choice : out) {
            signature += choice.label + "[" + choice.id +
                         (choice.web_id.empty() ? "" : ("/" + choice.web_id)) + "] ";
        }
        if (signature != last_signature) {
            last_signature = signature;
            log::info("[提供商标] 下拉合并结果（界面层）：" + signature);
        }
    }
    return out;
}

// 切换「模式」时让 `provider` 跟随其孪生 id（official ↔ web）——**只改节点的参数值**，
// 使「不带 -web 的条目」在 web 模式下也能正常工作（写进去的仍是表内真实 id）
void syncProviderIdWithMode(Node& node)
{
    const engine::Param* mode_param = node.findParam("mode");
    engine::Param*       provider   = node.findParam("provider");
    if (mode_param == nullptr || provider == nullptr) {
        return;
    }
    const std::string mode    = mode_param->text();
    const std::string current = provider->text();
    for (const ProviderChoice& choice : buildProviderChoices()) {
        if (choice.id != current && (choice.web_id.empty() || choice.web_id != current)) {
            continue;
        }
        const std::string wanted =
            (mode == "web" && !choice.web_id.empty()) ? choice.web_id : choice.id;
        if (wanted == current) {
            return;
        }
        provider->value = wanted;
        log::info("参数变更: " + node.id + ".provider → " + wanted +
                  "（随「模式」配对到该条目的网页版 / 官方版；可撤销）");
        return;
    }
}
} // namespace aiwrite::ui

