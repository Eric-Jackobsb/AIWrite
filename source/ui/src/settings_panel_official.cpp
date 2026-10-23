#include "settings_panel.h"

#include "credential.h"         // PB-06：API Key 以 Windows DPAPI 入库（不落明文）
#include "official_template.h"  // 自定义 AI 收口：自建官方 API 提供商模板（生成 / 校验 / 落盘）
#include "provider_spec.h"      // ai::providerSpecs() / 加载报告
#include "settings_panel_detail.h"  // ARCH-Z1：模态名 + 跨 TU 内部入口
#include "string_util.h"        // CLN-05：trim 收敛

#include <imgui.h>

#include <cstring>
#include <string>
#include <vector>

#include "file_dialog.h"        // utils::openFolder（「打开目录」按钮）
#include "log.h"
#include "paths.h"              // paths::userProvidersDir

namespace aiwrite::ui {
namespace settings_detail {

void draw_new_official_modal(SettingsPanelState& state, SettingsPanelResult& result)
{
    if (state.off_modal_open) {
        ImGui::OpenPopup(kNewOfficialModal); // 必须在设置窗口的上下文内调用
        state.off_modal_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(660.0f, 600.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kNewOfficialModal, nullptr)) {
        return;
    }

    const ai::ProviderSpecs& specs = ai::providerSpecs();
    ImGui::TextDisabled("三步：① 填 id + API 地址 → ②「创建并重新加载」→ ③「提供商」下拉立即出现（无需重启）");
    ImGui::TextDisabled("写到哪里：%s", paths::userProvidersDir().string().c_str());
    ImGui::Separator();

    // ---- 模板来源（照抄内置官方条目；R18）----
    const std::vector<std::string> official_ids = specs.ids("official");
    std::vector<std::string>       sources;
    sources.push_back("空白模板（只有 id + API 地址）");
    for (const std::string& id : official_ids) {
        sources.push_back("照抄 " + id);
    }
    int source_index = state.off_source_index;
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
        state.off_source_index = source_index;
        state.off_tpl          = (source_index == 0)
                                 ? ai::officialTemplateDefault()
                                 : ai::officialTemplateFromSpec(
                                       specs.find(official_ids[static_cast<std::size_t>(
                                           source_index) - 1]));
        state.off_error.clear();
        state.off_notice.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("「照抄」= 地址 / 模型 / 能力一起带过来，改个 id 就能建（R18）");
    ImGui::Separator();

    ImGui::BeginChild("##official_fields", ImVec2(0.0f, 400.0f), true);
    ImGui::TextUnformatted("提供商身份");
    ImGui::TextDisabled("提供商 id（= 文件名，只能字母 / 数字 / `-` / `_` / `.`）");
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##off_id", state.off_tpl.id, 64);
    ImGui::TextDisabled("显示名（下拉里显示的名字）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##off_display", state.off_tpl.display, 120);

    const std::vector<std::string> proto_values = {"openai", "anthropic", "gemini"};
    const std::vector<std::string> proto_labels = {
        "openai（OpenAI 兼容，绝大多数服务）",
        "anthropic（Claude Messages 协议）",
        "gemini（Google generateContent 协议）"};
    int proto_index = 0;
    for (std::size_t i = 0; i < proto_values.size(); ++i) {
        if (proto_values[i] == state.off_tpl.protocol) {
            proto_index = static_cast<int>(i);
            break;
        }
    }
    std::vector<const char*> proto_items;
    proto_items.reserve(proto_labels.size());
    for (const std::string& label : proto_labels) {
        proto_items.push_back(label.c_str());
    }
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::Combo("协议", &proto_index, proto_items.data(),
                     static_cast<int>(proto_items.size()))) {
        state.off_tpl.protocol = proto_values[static_cast<std::size_t>(proto_index)];
    }
    ImGui::TextDisabled("API 地址（**完整前缀，含 /v1 等**；例 https://api.example.com/v1）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##off_base", state.off_tpl.api_base, 400);
    ImGui::TextDisabled("聊天路径（留空 = 按协议默认，如 /chat/completions）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##off_chat", state.off_tpl.chat_path, 300);
    ImGui::TextDisabled("模型清单（逗号分隔；**留空 = 模型自由填写**，最省事）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##off_models", state.off_tpl.models, 400);
    ImGui::TextDisabled("视觉默认模型（可留空；如 deepseek-flash / glm-4v-flash）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##off_vision", state.off_tpl.vision_model, 300);

    ImGui::Separator();
    ImGui::TextUnformatted("能力与凭据");
    ImGui::Checkbox("支持图片理解（vision）", &state.off_tpl.vision);
    ImGui::SameLine();
    ImGui::Checkbox("支持 seed 参数", &state.off_tpl.seed);
    ImGui::SameLine();
    ImGui::Checkbox("支持系统角色", &state.off_tpl.system_role);
    ImGui::TextDisabled("环境变量名（可留空；有则按此读 Key）");
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##off_env", state.off_tpl.env_name, 200);
    ImGui::TextDisabled("凭据库引用名（留空 = brain-ai/<id>；保存后 Key 归此处）");
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##off_keyref", state.off_tpl.key_ref, 200);
    ImGui::TextDisabled("API Key（可留空；之后在节点面板「API Key」填一次同样入库。以 Windows DPAPI "
                       "加密保存，不落明文）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##off_key", state.off_key, sizeof(state.off_key), ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("备注（写进条目 notes：如实标注是否实测）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##off_notes", state.off_tpl.notes, 400);
    ImGui::EndChild();
    ImGui::Separator();

    // ---- 现场提示：可写性 / 二次确认 ----
    const std::string wrong = ai::officialTemplateError(state.off_tpl);
    if (!wrong.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "✗ %s", wrong.c_str());
    }
    else if (id_in_table(state.off_tpl.id)) {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f),
                           "表里已有条目「%s」（内置或此前新建）—— 创建将以**用户层条目**替换它",
                           state.off_tpl.id.c_str());
    }
    ImGui::Checkbox("覆盖表里已有条目 / 已存在同名文件（R8：默认不覆盖）", &state.off_overwrite);

    if (ImGui::Button("创建并重新加载", ImVec2(150.0f, 0.0f))) {
        std::string error = ai::officialTemplateError(state.off_tpl);
        if (error.empty() && id_in_table(state.off_tpl.id) && !state.off_overwrite) {
            error = "表里已有条目「" + state.off_tpl.id +
                    "」——请改 id，或勾选「覆盖表里已有条目」后重试";
        }
        if (!error.empty()) {
            state.off_error  = error;
            state.off_notice.clear();
        }
        else {
            std::filesystem::path written;
            if (!ai::writeUserOfficialFile(state.off_tpl, paths::userProvidersDir(),
                                           state.off_overwrite, &error, &written)) {
                state.off_error = error;
                state.off_notice.clear();
                log::warn("[提供商] 新建条目失败：" + error);
            }
            else {
                // API Key 可选：填了即入凭据库（Windows DPAPI；不落明文、不进 JSON）
                //  * 入库失败**不阻断**创建，但提示必须保留（下面生效复核的 clear 不能盖掉它）
                const std::string key_ref = utils::trim(state.off_tpl.key_ref).empty()
                                                ? ("brain-ai/" + state.off_tpl.id)
                                                : utils::trim(state.off_tpl.key_ref);
                bool               key_was_filled = !std::string(state.off_key).empty();
                std::string        key_error_text;
                if (key_was_filled) {
                    if (utils::saveCredential(key_ref, state.off_key, &key_error_text)) {
                        std::memset(state.off_key, 0, sizeof(state.off_key));
                    }
                    else {
                        key_error_text = "条目已创建，但 API Key 入库失败：" + key_error_text +
                                         "（可在节点面板「API Key」再填一次）";
                    }
                }
                // 写完**立即**重载并复核该条目是否真的生效（R8：坏表不能只报成功）
                ai::reloadProviderSpecs();
                result.provider_table_reloaded = true;
                const ai::ProviderSpecs& after   = ai::providerSpecs();
                const ai::ProviderSpec*  created = after.find(state.off_tpl.id);
                if (created == nullptr || created->kind != "official") {
                    state.off_error = "已写文件，但重新加载后该条目**未生效**：\n" +
                                  site_report_line(after.report, state.off_tpl.id) +
                                  "\n（修好文件后点设置里的「重新加载配置表」）";
                    state.off_notice.clear();
                }
                else {
                    state.off_overwrite = true; // 再点即覆盖（文件已存在）
                    std::string key_line;
                    if (!key_error_text.empty()) {
                        key_line = "\n⚠ " + key_error_text;
                    }
                    else if (key_was_filled) {
                        key_line = "\nAPI Key 已入凭据库（ref=" + key_ref + "，Windows DPAPI，不落明文）";
                    }
                    state.off_notice = "已创建 " + written.string() + "\n加载：" +
                                    after.report.summary() +
                                    "\n「提供商」下拉立即出现该条目（无需重启）" + key_line;
                    state.off_error = key_error_text; // 空 = 清掉上一帧的红字；非空 = 保留 Key 失败提示
                }
                log::info("[提供商] 新建条目 " + written.string() + " → " + after.report.summary());
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("打开 providers.d 目录", ImVec2(170.0f, 0.0f))) {
        if (utils::openFolder(paths::userProvidersDir().string())) {
            state.off_notice = "已打开 ~/.brain-ai/providers.d（每个提供商一个 JSON，可分享/备份）";
        }
        else {
            state.off_error = "打开失败（目录尚未创建：点「创建并重新加载」会先建目录）";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    if (!state.off_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", state.off_error.c_str());
    }
    else if (!state.off_notice.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%s", state.off_notice.c_str());
    }

    ImGui::EndPopup();
}
}  // namespace settings_detail
}  // namespace aiwrite::ui
