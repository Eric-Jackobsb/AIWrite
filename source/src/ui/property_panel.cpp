#include "ui/property_panel.h"

#include "engine/node_registry.h"
#include "engine/provider_resolve.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ui/text_view.h"
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

// 注销：清内存会话 + 删除 WebView2 profile（强制下次重新登录）
void logout_web_session()
{
    web::login_window().request_close();
    web::SessionStore::instance().clear();

    std::error_code ec;
    const std::filesystem::path profile = paths::webview2_profile();
    std::filesystem::remove_all(profile, ec);
    if (ec) {
        log::warn("网页版注销：profile 目录删除失败 " + profile.string() + "（" + ec.message() + "）");
    }
    else {
        log::info("网页版注销：profile 已删除 " + profile.string());
    }
}

// ProviderConfig 节点在 web 模式下显示「会话状态 + 登录入口」（设计 §8.5）
void draw_web_session_section()
{
    ImGui::Separator();
    ImGui::TextUnformatted("网页版会话（Cookie 只存内存，程序退出即销毁）");

    web::LoginWindow&  window  = web::login_window();
    const web::Session session = web::SessionStore::instance().snapshot();
    const bool         running = window.running();

    if (session.logged_in) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "状态：已登录（Cookie %zu 条）",
                           session.cookie_count());
        ImGui::TextDisabled("来源：%s", session.url.c_str());
        ImGui::TextDisabled("更新：%s", session.updated_at.c_str());
        if (const web::Cookie* cookie = session.find("ds_session_id")) {
            ImGui::TextDisabled("ds_session_id = %s", web::mask_value(cookie->value).c_str());
        }
    }
    else {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "状态：未登录");
    }

    // 协议探测状态（M4-06/M4-08 逆向用）：网页版接口真正需要的是 userToken（不是 Cookie）
    const web::ProbeResult& probe = session.probe;
    if (probe.has_user_token) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "userToken：%s",
                           probe.user_token_masked.c_str());
    }
    else {
        ImGui::TextDisabled("userToken：未获取（网页版接口需要它，请先登录）");
    }
    if (!probe.challenge_json.empty()) {
        ImGui::TextDisabled("PoW 挑战：已获取（%zu 字节，见 Console / app.log）",
                            probe.challenge_json.size());
    }
    if (!probe.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "探测错误：%s", probe.error.c_str());
    }

    if (running) {
        ImGui::TextDisabled("登录窗口已打开：请在窗口中完成登录，然后关闭它。");
        ImGui::TextDisabled("当前：%s", window.status().c_str());
        if (ImGui::Button("关闭登录窗口", ImVec2(-FLT_MIN, 0.0f))) {
            window.request_close();
        }
    }
    else {
        if (ImGui::Button("打开登录窗口（WebView2）", ImVec2(-FLT_MIN, 0.0f))) {
            // 页面加载完成后**自动探测一次**：手动开的窗口同样要拿到内存 userToken，
            // 否则运行时 ensure_session 只能等到超时（2026-09-26 修复）
            const web::LoginRequest request = web::interactive_login_request();
            std::string             error;
            if (window.start(request, &error)) {
                log::info("网页版登录窗口已打开（独立线程，主界面不受影响；页面加载后自动探测凭证）");
            }
            else {
                log::warn("网页版登录窗口打开失败: " + error);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("profile 会记住登录态；窗口页面加载完成后会自动探测一次（读取内存 userToken），\n"
                              "之后点「运行 / 重新生成」就无需再等。若提示未取得凭证：\n"
                              "先确认窗口内已登录，再点下方「探测网页版协议（dev）」后重试。");
        }
        if (session.logged_in) {
            ImGui::Spacing();
            if (ImGui::Button("注销（清会话 + 删除登录 profile）", ImVec2(-FLT_MIN, 0.0f))) {
                logout_web_session();
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("探测网页版协议（dev）", ImVec2(-FLT_MIN, 0.0f))) {
        if (window.running()) {
            web::request_protocol_probe(); // 窗口已开：直接在当前页面里探测
        }
        else {
            web::LoginRequest request;
            request.url              = "https://chat.deepseek.com/";
            request.window_title     = "AIwrite · 网页版协议探测（登录后自动探测）";
            request.probe_after_load = true; // 页面加载完成 → 自动探测
            std::string error;
            if (!window.start(request, &error)) {
                log::warn("启动探测窗口失败: " + error);
            }
        }
        log::info("已触发网页版协议探测（结果写入 Console 与 app.log）");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("在已登录页面内读取 userToken 并请求 PoW 挑战（同源，绕过跨域与反爬），\n"
                          "结果用于 M4-06 网页版 Provider 的实现；Token 只以脱敏形式记录");
    }

    ImGui::Spacing();
    ImGui::TextWrapped("登录窗口内：Ctrl+Alt+C 立即重新提取 Cookie，ESC 关闭窗口。"
                       "网页版推理（PoW 求解 / 定制 SSE）在 M4-06 / M4-08 落地；"
                       "当前已完成「登录 + 会话 + 协议探测」。");
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

// 绘制单个参数控件
//  返回 edited = 本次编辑结束（失焦/回车）；begin_edit = 控件刚被激活（值尚未变化，
//  外层在此刻压撤销快照）；changed_now = 值已写回模型（编辑过程中每帧都可能为 true）
bool draw_param_widget(Node& node, Param& param, bool& begin_edit, bool& changed_now)
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
        int index                 = 0;
        for (std::size_t i = 0; i < param.enum_options.size(); ++i) {
            if (param.enum_options[i] == current) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::vector<const char*> items;
        items.reserve(param.enum_options.size());
        for (const std::string& option : param.enum_options) {
            items.push_back(option.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        // 注意：Combo 在"点击下拉项"的当帧就返回 true，而该帧的 IsItemDeactivatedAfterEdit()
        // 并不成立（被点击的是弹出层里的选项，属于另一个 item），
        // 因此这里必须"值变化即写回"，否则会出现「选了 Web 但模式没变」的现象。
        if (!items.empty() &&
            ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
            const int safe_index =
                std::clamp(index, 0, static_cast<int>(param.enum_options.size()) - 1);
            param.value = param.enum_options[static_cast<std::size_t>(safe_index)];
            edited      = true;
            changed_now = true;
        }
        note_activation();
        break;
    }
    case ParamType::File:
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
        if (ImGui::Button(param.type == ParamType::File ? "浏览…" : "选择…", ImVec2(80.0f, 0.0f))) {
            const std::string picked = (param.type == ParamType::File)
                                           ? utils::open_file(filters_for(node), param.text())
                                           : utils::pick_folder(param.text());
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

    // ---- 生效提供商（P1-a）：provider 输入优先，覆盖节点自身参数 ----
    // 解决"改了节点「模式」却不生效"的困惑；official（官方 API，PB-04/PB-05 未接线）给出红字与一键切换
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

        const std::string reason = engine::unwired_reason(editor().graph, *node);
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s：本次运行该节点必定失败，下游会被跳过。", reason.c_str());
            ImGui::PopStyleColor();

            if (engine::official_not_wired(effective)) {
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
                                      "官方 API（API Key）待 PB-04/PB-05 接线后可用");
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

        bool param_begin_edit  = false;
        bool param_changed_now = false;
        const bool param_edit_ended =
            draw_param_widget(*node, param, param_begin_edit, param_changed_now);
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

    // ---- 网页版（web 模式）：会话状态 + 登录入口（设计 §8.5）----
    if (node->type == "ProviderConfig") {
        if (is_web_mode(*node)) {
            draw_web_session_section();
        }
        else {
            ImGui::Spacing();
            ImGui::TextDisabled("提示：把「模式」切到 web 可在此处打开 DeepSeek 网页版登录窗口"
                                "（Cookie 只存内存）。");
        }
    }

    // ---- 运行结果（PA-03）：只读展示 + 复制（与输出面板 / 画布节点摘要同源）----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("运行结果", ImGuiTreeNodeFlags_DefaultOpen)) {
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
            else if (run->error.empty()) {
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
