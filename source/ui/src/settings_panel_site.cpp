#include "settings_panel.h"

#include "provider_spec.h"  // ai::providerSpecs()（覆盖二次确认 / 生成报告）
#include "site_template.h"  // PB2-24：自建网页版站点模板（生成 / 校验 / 落盘）
#include "string_util.h"    // CLN-05：trim 收敛
#include "file_dialog.h"    // utils::openFolder（「打开目录」按钮）
#include "log.h"
#include "paths.h"          // paths::userProvidersDir
#include "settings_panel_detail.h"  // ARCH-Z1：模态名 + 跨 TU 内部入口

#include <imgui.h>

#include <string>
#include <vector>

namespace aiwrite::ui {
namespace settings_detail {

// std::string 单选（options 非空；值不在列表里 ⇒ 视为第一项，不静默改值）
bool combo_string(const char* label, std::string& value, const std::vector<std::string>& options)
{
    if (options.empty()) {
        return false;
    }
    int index = 0;
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i] == value) {
            index = static_cast<int>(i);
            break;
        }
    }
    std::vector<const char*> items;
    items.reserve(options.size());
    for (const std::string& option : options) {
        items.push_back(option.c_str());
    }
    if (ImGui::Combo(label, &index, items.data(), static_cast<int>(items.size()))) {
        value = options[static_cast<std::size_t>(index)];
        return true;
    }
    return false;
}


// 从加载报告里挑出**点名该 id** 的第一条错误 / 警告（生效失败时给用户看原文）
std::string site_report_line(const ai::SpecLoadReport& report, const std::string& id)
{
    for (const std::string& text : report.errors) {
        if (text.find(id) != std::string::npos) {
            return text;
        }
    }
    for (const std::string& text : report.warnings) {
        if (text.find(id) != std::string::npos) {
            return text;
        }
    }
    return "（加载报告未点名该 id —— 请确认文件是 UTF-8 JSON 且含 schema_version 信封）";
}
void draw_new_site_modal(SettingsPanelState& state, SettingsPanelResult& result)
{
    if (state.site_modal_open) {
        ImGui::OpenPopup(kNewSiteModal); // 必须在设置窗口的上下文内调用
        state.site_modal_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(780.0f, 560.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kNewSiteModal, nullptr)) {
        return;
    }

    const ai::ProviderSpecs& specs = ai::providerSpecs();
    ImGui::TextDisabled("三步：① 填 id + 登录页 → ②「创建并重新加载」→ ③「提供商」下拉立即出现（无需重启）");
    ImGui::TextDisabled("写到哪里：%s", paths::userProvidersDir().string().c_str());
    ImGui::Separator();

    // ---- 模板来源（照抄内置条目；R18）----
    const std::vector<std::string> web_ids = specs.ids("web");
    std::vector<std::string>       sources;
    sources.push_back("空白模板（只有 id + 登录页）");
    for (const std::string& id : web_ids) {
        sources.push_back("照抄 " + id);
    }
    int source_index = state.site_source_index;
    if (source_index < 0 || source_index >= static_cast<int>(sources.size())) {
        source_index = 0;
    }
    std::vector<const char*> source_items;
    source_items.reserve(sources.size());
    for (const std::string& label : sources) {
        source_items.push_back(label.c_str());
    }
    ImGui::SetNextItemWidth(380.0f);
    if (ImGui::Combo("模板来源", &source_index, source_items.data(),
                     static_cast<int>(source_items.size()))) {
        state.site_source_index = source_index;
        state.site_tpl          = (source_index == 0)
                                  ? ai::siteTemplateDefault()
                                  : ai::siteTemplateFromSpec(
                                        specs.find(web_ids[static_cast<std::size_t>(source_index) - 1]));
        state.site_error.clear();
        state.site_notice.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("「照抄」= 连站点身份一起带过来，改个 id 就能建（R18）");
    ImGui::Separator();

    ImGui::BeginChild("##site_fields", ImVec2(0.0f, 350.0f), true);
    ImGui::TextUnformatted("站点身份");
    ImGui::TextDisabled("站点 id（= 文件名，只能字母 / 数字 / `-` / `_` / `.`）");
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##site_id", state.site_tpl.id, 64);
    ImGui::TextDisabled("显示名（下拉里显示的名字）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_display", state.site_tpl.display, 120);
    ImGui::TextDisabled("登录页地址（**站点身份唯一来源**：必须 http:// 或 https://）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_login", state.site_tpl.login_url, 400);
    ImGui::TextDisabled("登录窗口标题（留空 = 自动「AIwrite · <显示名> 登录（登录后关闭窗口）」）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_window", state.site_tpl.window_title, 200);
    ImGui::Separator();
    ImGui::TextUnformatted("站点选择器（可后补：留空 = 登录型条目 —— 登录 / 登录态可用，生成会在运行期明确报错）");
    ImGui::TextDisabled("输入框选择器（例 div.chat-input-editor / textarea[placeholder=\"Ask …\"]）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_input", state.site_tpl.input_selector, 300);
    const std::vector<std::string> send_kinds = {"key", "click"};
    ImGui::SetNextItemWidth(160.0f);
    combo_string("发送方式", state.site_tpl.send_kind, send_kinds);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    input_text_string("##site_send_value", state.site_tpl.send_value, 200);
    ImGui::SameLine();
    ImGui::TextDisabled("键名（Enter）或发送按钮选择器");
    ImGui::TextDisabled("回答容器选择器（需要**先有一条回答**才存在 —— 唯一人工关卡，见 §3.4）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_answer", state.site_tpl.answer_selector, 300);
    const std::vector<std::string> done_kinds = {"（不设完成判据）", "selector_gone",
                                                 "selector_present"};
    std::string done_kind = state.site_tpl.done_kind.empty() ? done_kinds.front() : state.site_tpl.done_kind;
    ImGui::SetNextItemWidth(200.0f);
    if (combo_string("完成判据", done_kind, done_kinds)) {
        state.site_tpl.done_kind = (done_kind == done_kinds.front()) ? std::string() : done_kind;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##site_done_selector", state.site_tpl.done_selector, 300);
    ImGui::TextDisabled("登录 Cookie 名（逗号分隔；留空 = 该 origin 只要有 Cookie 即视为已登录）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_cookies", state.site_tpl.cookie_names, 300);
    ImGui::TextDisabled("页面内取 token 表达式（纯 DOM 站点一般留空；例 localStorage.getItem('token')）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_token", state.site_tpl.token_expr, 300);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("轮询间隔（毫秒）", &state.site_tpl.answer_poll_ms, 100, 500);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("最大轮询次数", &state.site_tpl.answer_max_polls, 10, 60);
    ImGui::TextDisabled("备注（写进条目 notes：如实标注是否实测）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_notes", state.site_tpl.notes, 400);
    ImGui::EndChild();
    ImGui::Separator();

    // ---- 现场提示：可写性 / 待实测 / 二次确认 ----
    const std::string pending = ai::siteTemplatePending(state.site_tpl);
    if (!pending.empty()) {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f),
                           "生成未就绪：缺 %s（**登录 / 登录态仍可用**；生成会在运行期明确报错）",
                           pending.c_str());
    }
    const std::string wrong = ai::siteTemplateError(state.site_tpl);
    if (!wrong.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "✗ %s", wrong.c_str());
    }
    else if (id_in_table(state.site_tpl.id)) {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f),
                           "表里已有条目「%s」（内置或此前新建）—— 创建将以**用户层条目**替换它",
                           state.site_tpl.id.c_str());
    }
    ImGui::Checkbox("覆盖表里已有条目 / 已存在同名文件（R8：默认不覆盖）", &state.site_overwrite);

    if (ImGui::Button("创建并重新加载", ImVec2(150.0f, 0.0f))) {
        std::string error = ai::siteTemplateError(state.site_tpl);
        if (error.empty() && id_in_table(state.site_tpl.id) && !state.site_overwrite) {
            error = "表里已有条目「" + state.site_tpl.id +
                    "」——请改 id，或勾选「覆盖表里已有条目」后重试";
        }
        if (!error.empty()) {
            state.site_error  = error;
            state.site_notice.clear();
        }
        else {
            std::filesystem::path written;
            if (!ai::write_user_site_file(state.site_tpl, paths::userProvidersDir(),
                                          state.site_overwrite, &error, &written)) {
                state.site_error = error;
                state.site_notice.clear();
                log::warn("[站点] 新建条目失败：" + error);
            }
            else {
                // 写完**立即**重载并复核该条目是否真的生效（R8：坏表不能只报成功）
                ai::reloadProviderSpecs();
                result.provider_table_reloaded = true;
                const ai::ProviderSpecs& after   = ai::providerSpecs();
                const ai::ProviderSpec*  created = after.find(state.site_tpl.id);
                if (created == nullptr || created->kind != "web") {
                    state.site_error = "已写文件，但重新加载后该条目**未生效**：\n" +
                                   site_report_line(after.report, state.site_tpl.id) +
                                   "\n（修好文件后点设置里的「重新加载配置表」）";
                    state.site_notice.clear();
                }
                else {
                    state.site_overwrite = true; // 再点即覆盖（文件已存在）
                    state.site_notice = "已创建 " + written.string() + "\n加载：" +
                                    after.report.summary() +
                                    "\n「提供商」下拉立即出现该条目（无需重启）；选择器实测见 §3.4";
                    state.site_error.clear();
                }
                log::info("[站点] 新建条目 " + written.string() + " → " + after.report.summary());
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("打开 providers.d 目录", ImVec2(170.0f, 0.0f))) {
        if (utils::openFolder(paths::userProvidersDir().string())) {
            state.site_notice = "已打开 ~/.brain-ai/providers.d（每站一个 JSON，可分享/备份）";
        }
        else {
            state.site_error = "打开失败（目录尚未创建：点「创建并重新加载」会先建目录）";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    if (!state.site_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", state.site_error.c_str());
    }
    else if (!state.site_notice.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%s", state.site_notice.c_str());
    }

    ImGui::EndPopup();
}

}  // namespace settings_detail
}  // namespace aiwrite::ui
