#include "ui/property_panel.h"

#include "engine/node_registry.h"
#include "engine/provider_resolve.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ui/text_view.h"
#include "ui/texture_cache.h"
#include "utils/asset_store.h"
#include "utils/file_dialog.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/session_store.h"
#include "web/webview_host.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace aiwrite::ui {
namespace {

// ImGui 的 Slider* 只接受 half-range 内的数值（imgui_widgets.cpp SliderBehavior 的
//  IM_ASSERT(p_max <= T_MAX / 2)）——参数定义一旦越界，控件一渲染就 abort。
//  这里给出安全上限；越界范围自动降级为 Input* + 手动裁剪（见 draw_param_widget）。
constexpr double kSliderLimitInt   = 1073741823.0;  // IM_S32_MAX / 2
constexpr double kSliderLimitFloat = 1.7e38;         // ≈ FLT_MAX / 2

using engine::Node;
using engine::Param;
using engine::ParamType;

std::vector<utils::FileFilter> filters_for(const Node& node)
{
    if (node.type == "ImageInput") {
        return {{"图片文件", "png,jpg,jpeg,bmp,webp"}, {"所有文件", "*"}};
    }
    return {{"所有文件", "*"}};
}

// 校验当前参数，返回错误文案（空 = 通过）
std::string param_error(const Param& param)
{
    std::string error;
    engine::Graph::validateParam(param, &error);
    return error;
}

// P7a-15：「输入」分组的端口连通清单
//  * 判据与运行前校验一致：非 optional 且连接数 0 → 必填未连（红字）
//  * 变长端口显示已连条数（P7a-01 起「图片理解」的 image 为变长）
void draw_input_port_summary(const Node& node)
{
    if (node.inputs.empty()) {
        ImGui::TextDisabled("本节点没有输入端口");
        return;
    }
    ImGui::Separator();
    ImGui::TextDisabled("输入端口（%d）", static_cast<int>(node.inputs.size()));
    for (const engine::Port& port : node.inputs) {
        const int   connections = editor().graph.inputConnectionCount(node.id, port.id);
        std::string text        = port.display_name;
        if (port.is_variadic) {
            text += "（变长）";
        }
        if (connections > 0) {
            text += "：已连 " + std::to_string(connections) + " 条";
            if (port.is_variadic && connections > 1) {
                text += "（按顺序全部送入）";
            }
            ImGui::BulletText("%s", text.c_str());
        }
        else if (port.is_optional) {
            ImGui::TextDisabled("  · %s：未连接（可选）", text.c_str());
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::BulletText("%s：未连接（必填）", text.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void help_marker(const std::string& description)
{
    if (description.empty()) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string param_label(const Param& param)
{
    return param.display_name + (param.is_required ? " *" : "");
}

// ---------------------------------------------------------------- 网页版会话 --
// 登录窗口（独立线程，不阻塞主界面）：进程内单例见 web::login_window()
bool is_web_mode(const Node& node)
{
    const Param* mode = node.findParam("mode");
    return mode != nullptr && mode->text() == "web";
}

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
    ctx.effective      = engine::resolve_display_provider(graph, node); // PB2-20：按自身条目解析
    ctx.on_web_entry   = ctx.effective.is_web();
    ctx.site           = ai::strict_web_spec_for(ctx.effective.spec);        // PB2-22：**不**回落
    ctx.provider_id    = ai::strict_web_provider_id_for(ctx.effective.spec);
    ctx.site_error     = ai::web_site_error(ctx.effective.spec);
    ctx.site_usable    = ctx.site_error.empty();
    ctx.field_warnings = ai::web_site_field_warnings(ctx.effective.spec);
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
    if (!web::logout_site(ctx.manual, 20000, &error)) {
        log::warn("网页版按站点注销失败: " + error);
    }
}

// 高级操作：删除**整个**登录 profile（= 清掉所有站点；PB2-19 / R14 边界说明）
void delete_web_profile()
{
    web::login_window().request_close();
    web::login_window().join();
    web::SessionStore::instance().clear_all();

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

// ProviderConfig 节点在 web 模式下显示「生效站点 + 会话状态 + 登录入口」（PB2-17/18/19）
//  * 站点身份唯一来源 = **生效条目自己的 `web` 段**（不变量 `I11` / `I14`）：登录页 / 窗口标题 / 探测路径 / Cookie 名
//  * 站点不可用（非网页版条目 / 缺 `web.login_url`）→ **错误块 + 三条引导 + 一键改选**，
//    且**不打开任何窗口**（PB2-22 / 决策 D-22②）
//  * 返回 true = 就地改了节点参数（外层据此压快照 + 标记变更）
bool draw_web_session_section(engine::Node& node, const engine::Graph& graph)
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

        const std::vector<std::string> web_ids = ai::provider_specs().ids("web");
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
        ai::web_session_state(ctx.effective.spec, web::web_session_evidence(session));
    const std::string cookie_name = site_cookie_name(ctx);
    if (verdict.state == ai::WebSessionState::logged_in) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "状态：已登录（该站点，Cookie %zu 条）",
                           session.cookie_count());
        ImGui::TextDisabled("来源：%s", session.url.c_str());
        ImGui::TextDisabled("更新：%s", session.updated_at.c_str());
        if (!cookie_name.empty()) {
            if (const web::Cookie* cookie = session.find(cookie_name)) {
                ImGui::TextDisabled("%s = %s", cookie_name.c_str(),
                                    web::mask_value(cookie->value).c_str());
            }
        }
    }
    else {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "状态：%s（该站点）",
                           ai::web_session_state_label(verdict.state).c_str());
    }
    ImGui::TextDisabled("%s", verdict.reason.c_str());
    if (verdict.state != ai::WebSessionState::logged_in) {
        ImGui::TextDisabled("点「打开登录窗口」在该站点**手动登录**；登录后**无需**再点「探测网页版协议」。");
    }

    // 协议探测状态（M4-06/M4-08 逆向用）
    //  * L4（PB2-27 / D-28①）：`userToken` 行**仅当**条目配了 `token_expr` 才显示；
    //    否则说明「本条目为 DOM 站点」（生成不依赖 userToken）
    //  * L4（PB2-27 / 不变量 I16）：**探测不适用**的站点不再显示红字「探测错误」（那是 DeepSeek 端点 404）
    const web::ProbeResult& probe = session.probe;
    if (ai::web_shows_user_token(ctx.effective.spec)) {
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
    if (!probe.error.empty() && ai::probe_is_applicable(ctx.site)) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "探测错误：%s", probe.error.c_str());
    }

    web::LoginWindow& window      = web::login_window();
    const bool        window_here = web::window_on_site(ctx.site_key);
    if (window.running()) {
        if (window_here) {
            ImGui::TextDisabled("登录窗口已打开（该站点）：请在窗口中完成登录，然后关闭它。");
        }
        else {
            ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "登录窗口正开着其他站点：%s",
                               web::current_window_site().c_str());
            ImGui::TextDisabled("点「打开登录窗口」会按串行策略切到本节点站点（先关旧窗）");
        }
        ImGui::TextDisabled("当前：%s", window.status().c_str());
        if (ImGui::Button("关闭登录窗口", ImVec2(-FLT_MIN, 0.0f))) {
            window.request_close();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("打开登录窗口（WebView2）", ImVec2(-FLT_MIN, 0.0f))) {
        // 站点参数来自**生效条目**（PB2-17）；页面加载完成后自动探测一次（与改造前一致）
        if (window.running() && !window_here) {
            window.request_close(); // 串行复用：先关旧站点窗口（PB2-19 / D-20）
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
    const bool probe_applicable = ai::probe_is_applicable(ctx.site);
    if (ImGui::Button(probe_applicable ? "探测网页版协议（dev）"
                                       : "只读诊断（该站点不适用协议探测）",
                      ImVec2(-FLT_MIN, 0.0f))) {
        if (web::window_on_site(ctx.site_key)) {
            web::request_protocol_probe(); // 窗口已开在该站点：直接在当前页面里探测 / 诊断
        }
        else {
            if (window.running()) {
                window.request_close();
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

    if (verdict.state == ai::WebSessionState::logged_in) {
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
            ImGui::TextDisabled("%s（Cookie %zu%s）", site.c_str(), item.cookie_count(),
                                item.user_token.empty() ? "" : "，有凭证");
            ImGui::SameLine();
            if (ImGui::SmallButton("注销")) {
                web::LoginRequest other;
                other.url = site; // 只需 origin：注销按站点
                std::string error;
                if (!web::logout_site(other, 20000, &error)) {
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

// 小写副本（参数搜索过滤用：ASCII 大小写不敏感，中文按字节比较）
std::string lower_copy(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

// 立即写回参数值（**不依赖"失焦判定"**）；返回是否发生变化
//  * 背景：原先统一写 `Widget(...) && IsItemDeactivatedAfterEdit()`，而"值发生变化"与"控件失焦"
//    往往不在同一帧（多行文本尤其明显）→ 会出现「改了文本内容，运行结果还是旧值」。
//    现改为：值变化即写回模型；`edited` 只在编辑结束时通知外层（日志/状态栏不刷屏）。
bool write_param(Param& param, const nlohmann::json& value)
{
    if (param.value == value) {
        return false;
    }
    param.value = value;
    return true;
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
struct ProviderChoice {
    std::string id;     // 主 id（无后缀条目；无后缀可用时 = 该 web 条目自身）
    std::string web_id; // 网页版孪生 id（空 = 无孪生）
    std::string label;  // 下拉显示名（永不含 `-web`）
};

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

// 构建下拉项（每次绘制重建：条目很少，且「重新加载配置表」后立即生效）
//  配对来源两条（**只在界面层**）：
//   ① 同名后缀规则：`xxx` ↔ `xxx-web`（deepseek / gemini …）
//   ② 品牌别名表：id 不同名但同属一家（openai ↔ chatgpt-web、anthropic ↔ claude-web、zhipu ↔ chatglm-web）
std::vector<ProviderChoice> build_provider_choices()
{
    const ai::ProviderSpecs& table = ai::provider_specs();

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
void sync_provider_id_with_mode(Node& node)
{
    const engine::Param* mode_param = node.findParam("mode");
    engine::Param*       provider   = node.findParam("provider");
    if (mode_param == nullptr || provider == nullptr) {
        return;
    }
    const std::string mode    = mode_param->text();
    const std::string current = provider->text();
    for (const ProviderChoice& choice : build_provider_choices()) {
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

// 绘制单个参数控件
//  返回 edited = 本次编辑结束（失焦/回车）；begin_edit = 控件刚被激活（值尚未变化，
//  外层在此刻压撤销快照）；changed_now = 值已写回模型（编辑过程中每帧都可能为 true）
bool draw_param_widget(Node& node, Param& param, bool& begin_edit, bool& changed_now,
                       const std::vector<std::string>* enum_override = nullptr)
{
    bool edited             = false;
    const std::string label = param_label(param);
    (void)label;

    const auto note_activation = [&begin_edit]() {
        if (ImGui::IsItemActivated()) {
            begin_edit = true;
        }
    };

    ImGui::PushID(param.id.c_str());

    switch (param.type) {
    case ParamType::String: {
        std::string value         = param.text();
        ImGuiInputTextFlags flags = 0;
        if (param.is_secret) {
            flags |= ImGuiInputTextFlags_Password;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool touched =
            ImGui::InputTextWithHint("##value", param.display_name.c_str(), &value, flags);
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Text: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool touched =
            ImGui::InputTextMultiline("##value", &value,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f));
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Int: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        int value = static_cast<int>(std::lround(param.number(fallback)));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        // 修复（崩溃）：SliderInt 只支持 ±IM_S32_MAX/2；范围越界（如 0..2147483647）时
        //  改用 InputInt，并把输入裁回参数声明的范围，避免 ImGui 断言 abort。
        const bool in_slider_range =
            has_range && (*param.min_value >= -kSliderLimitInt) && (*param.max_value <= kSliderLimitInt);
        const bool touched =
            in_slider_range ? ImGui::SliderInt("##value", &value, static_cast<int>(*param.min_value),
                                              static_cast<int>(*param.max_value))
                            : ImGui::InputInt("##value", &value);
        if (touched) {
            if (has_range) {
                const double clamped = std::clamp(static_cast<double>(value), *param.min_value, *param.max_value);
                value                = static_cast<int>(std::lround(clamped));
            }
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Float: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        float value = static_cast<float>(param.number(fallback));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        // 与 Int 同理：SliderFloat 只支持 ±FLT_MAX/2；越界时降级为 InputFloat + 裁剪。
        const bool in_slider_range = has_range && std::isfinite(*param.min_value) &&
                                     std::isfinite(*param.max_value) &&
                                     (*param.min_value >= -kSliderLimitFloat) &&
                                     (*param.max_value <= kSliderLimitFloat);
        const bool touched =
            in_slider_range ? ImGui::SliderFloat("##value", &value, static_cast<float>(*param.min_value),
                                                static_cast<float>(*param.max_value), "%.2f")
                            : ImGui::InputFloat("##value", &value, 0.1f, 1.0f, "%.2f");
        if (touched) {
            if (has_range) {
                value = static_cast<float>(
                    std::clamp(static_cast<double>(value), *param.min_value, *param.max_value));
            }
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Bool: {
        bool value = param.flag();
        if (ImGui::Checkbox("##value", &value)) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Enum: {
        const std::string current = param.text();
        // ---- v16（仅界面）：提供商下拉「合并显示」（`-web` 项合并 / 去后缀；见上方说明）----
        if (param.id == "provider") {
            const std::vector<ProviderChoice> choices    = build_provider_choices();
            const engine::Param*              mode_param = node.findParam("mode");
            const std::string mode = mode_param != nullptr ? mode_param->text() : std::string();
            std::vector<std::string> labels;
            std::vector<std::string> values; // 与 labels 对齐：选中后写入 `provider` 的真实 id
            labels.reserve(choices.size() + 1);
            values.reserve(choices.size() + 1);
            int index = -1;
            for (const ProviderChoice& choice : choices) {
                labels.push_back(choice.label);
                values.push_back((mode == "web" && !choice.web_id.empty()) ? choice.web_id
                                                                          : choice.id);
                if (choice.id == current ||
                    (!choice.web_id.empty() && choice.web_id == current)) {
                    index = static_cast<int>(labels.size()) - 1;
                }
            }
            if (index < 0) {
                // 表外值（手改 JSON / 旧工作流里的其它 id）：**如实显示**，不静默替换
                labels.insert(labels.begin(), current);
                values.insert(values.begin(), current);
                index = 0;
            }
            std::vector<const char*> items;
            items.reserve(labels.size());
            for (const std::string& choice_label : labels) {
                items.push_back(choice_label.c_str());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!items.empty() &&
                ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
                const int safe_index = std::clamp(index, 0, static_cast<int>(values.size()) - 1);
                const std::string wanted = values[static_cast<std::size_t>(safe_index)];
                if (wanted != current) {
                    write_param(param, wanted); // 写入的始终是表内真实 id（运行期照常解析）
                }
                edited      = true;
                changed_now = true;
            }
            note_activation();
            break;
        }
        // 允许调用方按上下文覆盖可选项（扩展位）。
        //  * ⚠️ M_patchB L2（PB2-20 / 决策 D-21）：**不得**用它裁剪 ProviderConfig 的「模式」
        //    （恒 {official, web}，不变量 I13）；目前无调用方传入
        //  * 若当前值不在可选项里，**如实显示当前值**（调用方给出提示）
        std::vector<std::string> options =
            enum_override != nullptr ? *enum_override : param.enum_options;
        if (!current.empty() &&
            std::find(options.begin(), options.end(), current) == options.end()) {
            options.insert(options.begin(), current);
        }
        int index = 0;
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (options[i] == current) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::vector<const char*> items;
        items.reserve(options.size());
        for (const std::string& option : options) {
            items.push_back(option.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        // 注意：Combo 在"点击下拉项"的当帧就返回 true，而该帧的 IsItemDeactivatedAfterEdit()
        // 并不成立（被点击的是弹出层里的选项，属于另一个 item），
        // 因此这里必须"值变化即写回"，否则会出现「选了 Web 但模式没变」的现象。
        if (!items.empty() &&
            ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
            const int safe_index = std::clamp(index, 0, static_cast<int>(options.size()) - 1);
            param.value          = options[static_cast<std::size_t>(safe_index)];
            edited               = true;
            changed_now          = true;
        }
        // ---- v16（仅界面）：切换「模式」→ 让 `provider` 配对到该条目的网页版 / 官方版 ----
        //  * 使「不带 `-web` 的条目」（如 `deepseek` / `gemini`）在 web 模式下直接可用
        //  * 写入的仍是表内真实 id → 运行期与自检无需任何改动；可撤销
        if (param.id == "mode" && changed_now) {
            sync_provider_id_with_mode(node);
        }
        note_activation();
        break;
    }
    case ParamType::File: {
        // P7a-02：File 参数支持**多值**（每行一个路径；换行为分隔符，见 utils/paths.h）
        //  * 2 行输入框 + 右侧按钮组（浏览…=替换 / 多选…=追加去重 / 清空）
        //  * 多行控件的 SameLine 落点不可靠 → 用 SetCursorScreenPos 定位按钮组
        std::string value = param.text();

        const ImVec2 start    = ImGui::GetCursorScreenPos();
        const float  spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float  button_w = 84.0f;
        const float  field_w  = ImGui::GetContentRegionAvail().x - button_w - spacing;
        const float  field_h  = ImGui::GetTextLineHeight() * 2.0f +
                               ImGui::GetStyle().FramePadding.y * 2.0f;

        const bool touched =
            ImGui::InputTextMultiline("##value", &value, ImVec2(field_w, field_h));
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();

        const ImVec2 after_field = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x + field_w + spacing, start.y));
        ImGui::BeginGroup();
        if (ImGui::Button("浏览…", ImVec2(button_w, 0.0f))) {
            const std::string picked = utils::open_file(filters_for(node), param.text());
            if (!picked.empty()) {
                write_param(param, picked);
                edited      = true;
                changed_now = true;
            }
        }
        if (ImGui::Button("多选…", ImVec2(button_w, 0.0f))) {
            const std::vector<std::string> picked =
                utils::open_files(filters_for(node), param.text());
            if (!picked.empty()) {
                std::vector<std::string> merged = paths::split_path_list(param.text());
                for (const std::string& item : picked) {
                    if (std::find(merged.begin(), merged.end(), item) == merged.end()) {
                        merged.push_back(item);
                    }
                }
                write_param(param, paths::join_path_list(merged));
                edited      = true;
                changed_now = true;
            }
        }
        if (ImGui::Button("清空", ImVec2(button_w, 0.0f))) {
            write_param(param, std::string());
            edited      = true;
            changed_now = true;
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(after_field);

        const std::vector<std::string> entries = paths::split_path_list(param.text());
        const int count = static_cast<int>(entries.size());
        if (count > 1) {
            ImGui::TextDisabled("共 %d 张图片（每行一个路径）", count);
        }

        // P7a-04/07：令牌 → 资源目录里的真实路径（可见完整路径；缺失时红字告警）
        bool has_legacy = false;
        for (const std::string& entry : entries) {
            if (asset::needs_migration(entry)) {
                has_legacy = true;
                continue;
            }
            bool              missing = false;
            std::string       resolve_error;
            const std::string local = asset::to_local_path(entry, &missing, &resolve_error);
            if (missing || local.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                                   resolve_error.empty() ? "图片资源缺失（可能已被删除）"
                                                         : resolve_error.c_str());
                ImGui::PopStyleColor();
            }
            else {
                ImGui::TextDisabled("→ %s", local.c_str());
            }
        }

        // P7a-06：外部路径 → 一键迁进资源目录（失败项**保持原值**，不丢用户数据）
        if (has_legacy) {
            ImGui::TextDisabled("其中含**外部路径**：迁移进资源目录后，换目录 / 换机仍能取到图片");
            if (ImGui::Button("迁移到资源目录", ImVec2(150.0f, 0.0f))) {
                std::vector<std::string> migrated;
                std::vector<std::string> failures;
                for (const std::string& entry : entries) {
                    if (!asset::needs_migration(entry)) {
                        migrated.push_back(entry);
                        continue;
                    }
                    std::string       import_error;
                    const std::string token = asset::import_file(entry, &import_error);
                    if (token.empty()) {
                        if (!import_error.empty()) {
                            failures.push_back(import_error);
                        }
                        migrated.push_back(entry);
                        continue;
                    }
                    migrated.push_back(token);
                }
                write_param(param, paths::join_path_list(migrated));
                edited      = true;
                changed_now = true;
                log::info("[资源目录] 迁移完成：" + std::to_string(entries.size()) + " 项，失败 " +
                          std::to_string(failures.size()) + " 项");
                if (!failures.empty()) {
                    log::error("[资源目录] 迁移失败：" + failures.front());
                }
            }
        }
        break;
    }
    case ParamType::Directory: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN - 90.0f);
        const bool touched = ImGui::InputTextWithHint("##value", "(未选择)", &value);
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        ImGui::SameLine();
        if (ImGui::Button("选择…", ImVec2(80.0f, 0.0f))) {
            const std::string picked = utils::pick_folder(param.text());
            if (!picked.empty()) {
                write_param(param, picked);
                edited      = true;
                changed_now = true;
            }
        }
        break;
    }
    case ParamType::Color: {
        // 以 "#RRGGBB" 字符串存储
        const std::string hex = param.text();
        float rgb[3]          = {1.0f, 1.0f, 1.0f};
        if (hex.size() == 7 && hex[0] == '#') {
            for (int i = 0; i < 3; ++i) {
                rgb[i] = static_cast<float>(
                    std::strtol(hex.substr(static_cast<std::size_t>(1 + i * 2), 2).c_str(), nullptr, 16) /
                    255.0);
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::ColorEdit3("##value", rgb)) {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
                          static_cast<int>(rgb[0] * 255.0f), static_cast<int>(rgb[1] * 255.0f),
                          static_cast<int>(rgb[2] * 255.0f));
            changed_now = write_param(param, std::string(buffer));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    }

    ImGui::PopID();
    return edited;
}

} // namespace

void draw_property_panel(const char* window_title, bool* open, Node* node,
                         PropertyEditResult& result)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(window_title, open)) {
        ImGui::End();
        return;
    }

    if (node == nullptr) {
        ImGui::TextDisabled("未选中节点");
        ImGui::Separator();
        ImGui::TextWrapped("在画布中单击一个节点即可编辑其参数；"
                           "Ctrl+单击 或框选可多选，右键菜单可复制 / 删除。");
        ImGui::End();
        return;
    }

    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node->type);

    // ---- P7a-15：三组可折叠（输入 / 参数 / 运行状态）----
    //  * 折叠状态由 ImGui 持久化到 `io.IniFilename`（app.cpp 已指向数据目录）→ **跨运行保留**
    if (ImGui::CollapsingHeader("输入", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ---- 概览 ----
        ImGui::Text("节点 %s", node->id.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", node->type.c_str());
        ImGui::TextDisabled("分类: %s    状态: %s",
                            definition != nullptr ? engine::categoryName(definition->category) : "未知",
                            engine::nodeStateName(node->state));

        ImGui::TextUnformatted("标题");
        if (editor().request_focus_title) { // 双击节点 / 右键"改名" → 自动聚焦
            ImGui::SetKeyboardFocusHere();
            editor().request_focus_title = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool title_touched = ImGui::InputTextWithHint("##title", "节点标题", &node->title);
        if (title_touched || ImGui::IsItemDeactivatedAfterEdit()) {
            result.renamed = true;
        }
        if (ImGui::IsItemActivated()) {
            result.begin_edit = true; // 改名前的快照
        }

        if (definition != nullptr && !definition->description.empty()) {
            ImGui::TextWrapped("%s", definition->description.c_str());
        }

        // P7a-15：输入端口连通清单（必填未连 → 红字）
        draw_input_port_summary(*node);
    }

    // ---- P7a-15：「参数」分组（可折叠）----
    //  * 分组体为既有代码，**保持原缩进**以便本次改动可审（纯格式差异，功能不受影响）
    if (ImGui::CollapsingHeader("参数", ImGuiTreeNodeFlags_DefaultOpen)) {
    // ---- 生效提供商（P1-a）：provider 输入优先，覆盖节点自身参数 ----
    // 解决"改了节点「模式」却不生效"的困惑；official（官方 API）与图片理解（M5-02）给出红字与提示
    if (engine::uses_provider(node->type)) {
        const engine::EffectiveProvider effective =
            engine::resolve_effective_provider(editor().graph, *node);
        if (node->type == "LLMGenerate") {
            if (effective.from_edge) {
                ImGui::TextDisabled("生效：%s（来自 提供商配置 %s）· 模型 %s", effective.mode.c_str(),
                                    effective.source_node.c_str(), effective.model.c_str());
            }
            else {
                ImGui::TextDisabled("生效：%s（节点自身设置；provider 输入未连接）",
                                    effective.mode.c_str());
            }
        }
        else if (node->type == "VLMGenerate") {
            // M5-02：图片理解只走 official（OpenAI 兼容多模态）；模型名来自「模型（自定义）」
            ImGui::TextDisabled("生效：图片理解 · 模式 %s · 模型 %s%s", effective.mode.c_str(),
                                effective.model.empty() ? "（未设置）" : effective.model.c_str(),
                                effective.from_edge ? "" : "（provider 输入未连接）");
        }

        const std::string reason = engine::unwired_reason(editor().graph, *node);
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s：本次运行该节点必定失败，下游会被跳过。", reason.c_str());
            ImGui::PopStyleColor();

            // 「已接线」提示与一键切换只针对文本生成（图片理解没有网页版入口，见 M5-02）
            if (node->type == "LLMGenerate" && engine::official_not_wired(effective)) {
                ImGui::TextDisabled(
                    "网页版已接线（provider.mode = web）：切换后即可真实生成，需先登录一次。");

                const std::string target_id = effective.from_edge ? effective.source_node : node->id;
                const std::string target_label =
                    effective.from_edge ? "提供商配置 " + target_id : std::string("本节点");
                if (ImGui::Button(("把 " + target_label + " 改为 web").c_str())) {
                    if (engine::Node* target = editor().graph.findNode(target_id)) {
                        if (engine::Param* mode = target->findParam("mode")) {
                            editor().snapshot("切换提供商为网页版"); // 先压快照：可撤销
                            mode->value = std::string("web");
                            editor().set_status(target_label + " 已切换为网页版（可撤销）");
                            log::info("[参数面板] " + target_label + " mode → web");
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("网页版走 DeepSeek 网页版会话（需已登录一次）；\n"
                                      "官方 API（API Key）已接线（PB-05）：填 Key 即可用");
                }
            }
        }
    }

    // ---- 校验汇总（设计 §6.3；条件隐藏的参数不参与校验）----
    std::vector<std::string> errors;
    for (const Param& param : node->params) {
        if (!engine::param_visible(*node, param)) {
            continue;
        }
        const std::string error = param_error(param);
        if (!error.empty()) {
            errors.push_back(param.display_name + ": " + error);
        }
    }
    if (errors.empty()) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "参数校验通过");
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验：%d 项错误",
                           static_cast<int>(errors.size()));
        for (const std::string& error : errors) {
            ImGui::BulletText("%s", error.c_str());
        }
    }

    ImGui::Separator();

    // ---- 参数控件（F2：搜索过滤 + 只读标记；条件隐藏的参数不显示）----
    static char param_filter[64] = {};
    ImGui::SetNextItemWidth(-72.0f);
    ImGui::InputTextWithHint("##param_filter", "搜索参数（id / 名称 / 说明）", param_filter,
                             sizeof(param_filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("清除##param_filter")) {
        param_filter[0] = '\0';
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清空搜索框（重新显示全部参数）");
    }
    const std::string filter_text = lower_copy(param_filter);
    const auto        matches_filter = [&filter_text](const Param& param) {
        if (filter_text.empty()) {
            return true;
        }
        return lower_copy(param.id).find(filter_text) != std::string::npos ||
               lower_copy(param.display_name).find(filter_text) != std::string::npos ||
               lower_copy(param.description).find(filter_text) != std::string::npos;
    };

    // ---- M_patchB L1 续（PB2-17）+ L2 修订（PB2-20）：ProviderConfig 自己的生效条目 ----
    //  * 按**该节点自身参数**解析（此前误用 resolve_effective_provider()：它对 ProviderConfig
    //    恒返回默认构造 → 站点区/提示永远回落内置默认，见 M_patchB §9.1）
    //  * **撤回**「web 条目自动锁 mode=web」（决策 D-21）：只提示、不静默改写
    const bool                is_provider_node = node->type == "ProviderConfig";
    engine::EffectiveProvider provider_effective;
    if (is_provider_node) {
        provider_effective = engine::resolve_display_provider(editor().graph, *node);
    }

    if (node->params.empty()) {
        ImGui::TextDisabled("该节点没有参数");
    }
    int hidden_params   = 0;
    int filtered_params = 0;
    int matched_params  = 0;
    for (Param& param : node->params) {
        if (!engine::param_visible(*node, param)) {
            ++hidden_params;
            continue;
        }
        if (!matches_filter(param)) {
            ++filtered_params;
            continue;
        }
        ++matched_params;
        ImGui::PushID(param.id.c_str());
        ImGui::TextUnformatted(param_label(param).c_str());
        if (param.is_secret) {
            ImGui::SameLine();
            ImGui::TextDisabled("（密钥：仅存内存、不写盘）");
        }
        if (param.value != param.default_value) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "已改");
        }
        help_marker(param.description);
        ImGui::PopID();

        // M_patchB L2 修订（PB2-20 / 决策 D-21）：ProviderConfig 的「模式」**恒为 {official, web}**
        //  * **不再**按条目 kind 裁剪候选 / 覆盖下拉项（不变量 I13：两项始终可选）
        //  * 与条目类型不一致时只给**橙色提示**（mode_kind_hint），绝不静默改写用户选择
        bool param_begin_edit  = false;
        bool param_changed_now = false;
        const bool param_edit_ended =
            draw_param_widget(*node, param, param_begin_edit, param_changed_now);
        // 只在「模式 = official」时提示（web 模式下的站点问题由站点区的**错误块**给出，避免重复；PB2-22）
        if (is_provider_node && param.id == "mode" && param.text() != "web") {
            const std::string hint = engine::mode_kind_hint(provider_effective);
            if (!hint.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f), "  ⚠ %s", hint.c_str());
            }
        }
        // 「切换提供商」→ 带出一次「模式」建议值（web 条目建议 web / official 条目建议 official）
        //  * 只带出一次建议：之后用户可任意改选，**不再**被改写（D-21）
        if (is_provider_node && param.id == "provider" && (param_changed_now || param_edit_ended)) {
            if (engine::Param* mode_param = node->findParam("mode")) {
                const std::string suggestion = engine::provider_mode_suggestion(*node);
                if (!suggestion.empty() && mode_param->text() != suggestion) {
                    result.begin_edit = true; // 外层据此压一次撤销快照（可撤销）
                    mode_param->value = suggestion;
                    result.changed    = true;
                    log::info("参数变更: " + node->id + ".mode → " + suggestion +
                              "（切换提供商带出的建议值，可再改回）");
                }
            }
        }
        if (param_changed_now || param_edit_ended) {
            result.changed = true; // 值已写回模型（编辑过程中即时生效）
        }
        if (param_edit_ended) {
            log::info("参数变更: " + node->id + "." + param.id); // 每段编辑只记一次
        }
        if (param_begin_edit) {
            result.begin_edit = true;
        }

        const std::string error = param_error(param);
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  %s", error.c_str());
        }
        ImGui::Spacing();
    }

    if (hidden_params > 0) {
        ImGui::TextDisabled("（%d 个 %s 专属参数已按当前模式隐藏）", hidden_params,
                            node->type == "ProviderConfig" ? "官方 API" : "条件");
    }
    if (!filter_text.empty()) {
        ImGui::TextDisabled("搜索「%s」：命中 %d 项，已过滤 %d 项（共 %d 项）", param_filter,
                            matched_params, filtered_params, matched_params + filtered_params);
    }

    // ---- 网页版（web 模式）：会话状态 + 登录入口（PB2-17/18/19）----
    if (node->type == "ProviderConfig") {
        if (is_web_mode(*node)) {
            if (draw_web_session_section(*node, editor().graph)) {
                result.begin_edit = true; // 先压快照（可撤销）
                result.changed    = true;
            }
        }
        else {
            ImGui::Spacing();
            if (provider_effective.is_web()) {
                ImGui::TextDisabled("该提供商是网页版条目（建议「模式」= web）：把「模式」切到 web "
                                    "即可在此处打开该站点的登录窗口。");
            }
            else {
                ImGui::TextDisabled("提示：把「模式」切到 web 需要**网页版站点条目**"
                                    "（如 deepseek-web，或自建站点条目 —— 见使用说明 §9）；"
                                    "非网页版条目**不会**回落到内置默认站点（决策 D-22②）。");
            }
        }
    }
    } // P7a-15：「参数」分组结束

    // M5-01 收尾：图片输入的尺寸摘要（只读文件头，不需要 GL 上下文）
    // P7a-04/07：先解析资源令牌 → 真实路径；缺失时给可操作告警（不再只说「读不到文件头」）
    if (node->type == "ImageInput") {
        const engine::Param*           path_param = node->findParam("path");
        const std::vector<std::string> entries =
            path_param != nullptr ? paths::split_path_list(path_param->text())
                                  : std::vector<std::string>();
        if (!entries.empty()) {
            bool              missing = false;
            std::string       resolve_error;
            const std::string image_path = asset::to_local_path(entries.front(), &missing, &resolve_error);
            if (missing || image_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                                   resolve_error.empty() ? "图片资源缺失（可能已被删除）"
                                                         : resolve_error.c_str());
                ImGui::PopStyleColor();
            }
            else {
                int         width  = 0;
                int         height = 0;
                std::string error;
                if (image_size(image_path, &width, &height, &error)) {
                    if (entries.size() > 1) {
                        ImGui::TextDisabled("图片尺寸：%d×%d（共 %d 张，此处显示第 1 张）", width,
                                            height, static_cast<int>(entries.size()));
                    }
                    else {
                        ImGui::TextDisabled("图片尺寸：%d×%d", width, height);
                    }
                }
                else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                    ImGui::TextWrapped("%s", error.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    }

    // ---- 运行状态（PA-03）：只读展示 + 复制（与输出面板 / 画布节点摘要同源）----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("运行状态", ImGuiTreeNodeFlags_DefaultOpen)) {
        const engine::RunSnapshot& snapshot = editor().run_snapshot_view();
        const engine::RunNodeView* run      = snapshot.find(node->id);

        if (run == nullptr) {
            ImGui::TextDisabled("尚未运行（点工具栏「▶ 运行」；首次网页版会话约数秒）");
        }
        else {
            ImGui::TextDisabled("状态: %s    耗时: %.2f ms", engine::nodeStateName(run->state),
                                run->duration_ms);
            if (!run->error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("错误：%s", run->error.c_str());
                ImGui::PopStyleColor();
            }

            // M5-03：图片结果预览（与输出面板共用纹理缓存）
            if (!run->images.empty()) {
                for (std::size_t index = 0; index < run->images.size(); ++index) {
                    ImGui::PushID(static_cast<int>(3000 + index));
                    const TextureInfo texture = texture_for(run->images[index]);
                    if (texture.texture != 0) {
                        const float limit  = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
                        float       width  = static_cast<float>(texture.width);
                        float       height = static_cast<float>(texture.height);
                        if (width > limit && width > 0.0f) {
                            const float shrink = limit / width;
                            width *= shrink;
                            height *= shrink;
                        }
                        ImGui::Image(
                            reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture.texture)),
                            ImVec2(width, height));
                        ImGui::TextDisabled("预览 %d×%d", texture.width, texture.height);
                    }
                    else {
                        ImGui::TextDisabled("图片不可用：%s", texture.error.c_str());
                    }
                    ImGui::PopID();
                }
            }

            const std::string body = node_output_text(editor().graph, snapshot, node->id);
            if (!body.empty()) {
                draw_readonly_text("##prop_run_result", body, 8.0f);
                if (ImGui::Button("复制运行结果")) {
                    copy_text(body, "[参数面板] " + node->id);
                }
            }
            if (node->type == "TextOutput") { // M_textio P3：最终输出
                std::string label = "输出";
                if (const engine::Param* label_param = node->findParam("label")) {
                    const std::string text = label_param->text();
                    if (!text.empty()) {
                        label = text;
                    }
                }
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "【最终输出】%s", label.c_str());
                ImGui::BeginDisabled(body.empty());
                if (ImGui::Button("导出为文档…")) {
                    export_node_document(editor(), node->id);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("导出为 .md/.txt（与预览同源，含元信息头）");
                }
            }
            else if (run->error.empty() && run->images.empty()) {
                ImGui::TextDisabled("（该节点无输出）");
            }

            if (!editor().last_archive_dir.empty()) {
                ImGui::TextDisabled("已归档：%s", editor().last_archive_dir.c_str());
                if (ImGui::SmallButton("复制归档路径")) {
                    copy_text(editor().last_archive_dir, "[参数面板] 归档路径");
                }
            }
        }
    }

    // ---- M_rerun：「重新生成（新 seed）」—— 等效“再次运行”，但使用新的随机种子 ----
    if (node->type == "LLMGenerate") {
        ImGui::Separator();
        if (const engine::Param* seed_param = node->findParam("seed")) {
            ImGui::TextDisabled("随机种子：%d（0 = 不指定；「重新生成」会写入新值）",
                                static_cast<int>(seed_param->number(0.0)));
        }
        const bool rerunning = editor().executor.running() || editor().session_active();
        ImGui::BeginDisabled(rerunning);
        if (ImGui::Button("重新生成（新 seed）", ImVec2(-FLT_MIN, 0.0f))) {
            if (engine::Param* seed_param = node->findParam("seed")) {
                editor().snapshot("重新生成（新 seed）"); // 先压快照：可撤销
                const long long stamp = static_cast<long long>(std::time(nullptr));
                const int       value = static_cast<int>(stamp % 1000000000LL); // ∈ 参数范围 [0, 1e9]
                seed_param->value     = (value > 0) ? value : 1;
                const int applied     = static_cast<int>(seed_param->number(0.0));
                log::info("[重跑] " + node->id + " 新 seed=" + std::to_string(applied));
                editor().set_status("已为 " + node->id + " 设置新 seed=" + std::to_string(applied) +
                                    "，开始重新生成…");
                editor().start_run_async();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("等效于「再次运行」，但使用新的随机种子：\n"
                              "· 官方 API：请求体带 seed\n"
                              "· 网页版：协议无 seed 槽位（忽略，但重跑结果也会不同）\n"
                              "· 会先记录一步撤销，可回退到旧 seed");
        }
    }

    // ---- 底部操作（F2：批量应用 / 重置 均需二次确认）----
    static std::string confirm_apply_node; // 待确认"应用到同类节点"的节点 id
    static std::string confirm_reset_node; // 待确认"重置为默认值"的节点 id

    ImGui::Separator();
    int same_type_others = 0;
    for (const engine::Node& other : editor().graph.nodes) {
        if (other.id != node->id && other.type == node->type) {
            ++same_type_others;
        }
    }
    ImGui::TextDisabled("同类型（%s）其他节点：%d 个", node->type.c_str(), same_type_others);

    if (confirm_apply_node == node->id) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "确认把 %s 的参数应用到 %d 个同类节点？（密钥类参数不复制）",
                           node->id.c_str(), same_type_others);
        if (ImGui::Button("确认应用", ImVec2(120.0f, 0.0f))) {
            editor().snapshot("批量应用参数到同类节点"); // 先压快照：整批应用可一次撤销
            std::string error;
            const int   affected = editor().graph.copyParamsToSameType(node->id, &error);
            if (affected > 0) {
                result.changed = true;
                editor().set_status("已把 " + node->id + " 的参数应用到 " +
                                    std::to_string(affected) + " 个同类节点（可撤销）");
                log::info("[参数面板] 批量应用参数: " + node->id + " → " +
                          std::to_string(affected) + " 个同类节点");
            }
            else {
                editor().set_status("批量应用未生效：" +
                                    (error.empty() ? std::string("没有同类节点") : error));
            }
            confirm_apply_node.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##apply", ImVec2(80.0f, 0.0f))) {
            confirm_apply_node.clear();
        }
    }
    else {
        ImGui::BeginDisabled(same_type_others == 0);
        if (ImGui::Button("把当前参数应用到全部同类节点", ImVec2(-FLT_MIN, 0.0f))) {
            confirm_apply_node = node->id; // 二次确认（批量改动多节点）
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("按参数 id 复制到同类型节点（不改动标题 / 位置 / 连线）；\n"
                              "密钥类参数（API Key）不会被复制");
        }
    }

    ImGui::Separator();
    if (confirm_reset_node == node->id) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "确认把 %s 的全部参数重置为默认值？",
                           node->id.c_str());
        if (ImGui::Button("确认重置", ImVec2(120.0f, 0.0f))) {
            editor().snapshot("重置参数为默认值"); // 先压快照：可撤销（D-M3-5）
            for (Param& param : node->params) {
                param.reset_to_default();
            }
            result.changed = true;
            log::info("参数重置为默认值: " + node->id);
            editor().set_status("已把 " + node->id + " 的参数重置为默认值（可撤销）");
            confirm_reset_node.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##reset", ImVec2(80.0f, 0.0f))) {
            confirm_reset_node.clear();
        }
    }
    else {
        if (ImGui::Button("重置为默认值", ImVec2(-FLT_MIN, 0.0f))) {
            confirm_reset_node = node->id; // 二次确认（避免误触清空编辑）
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("把该节点全部参数恢复为定义中的默认值（需再确认一次；\n"
                              "执行前会自动记录一步撤销，可回退）");
        }
    }

    ImGui::End();
}

} // namespace aiwrite::ui
