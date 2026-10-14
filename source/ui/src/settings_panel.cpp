#include "settings_panel.h"

#include "file_dialog.h"
#include "log.h"
#include "paths.h"
#include "provider_spec.h"
#include "settings_validation.h"
#include "site_template.h"   // PB2-24：自建网页版站点模板（生成 / 校验 / 落盘）

#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace aiwrite::ui {
namespace {

constexpr const char* kSettingsWindow = "设置";

bool        g_open   = false;
bool        g_loaded = false;
Config      g_edit;      // 工作副本（只有「保存」才写回磁盘与调用方）
Config      g_baseline;  // 打开时的快照（判断宿主是否需要重取本地 UI 状态）
std::string g_error;     // 最近一次「保存」被拒的原因
std::string g_notice;    // 最近一次动作的结果（绿字）

// 是否需要宿主重取本地 UI 状态（面板可见性 / 画布选项 / Console 高度）
//  * 这些值在 app.cpp 里另有 local 副本（菜单勾选项、CanvasOptions），保存后必须同步
bool needs_resync(const Config& before, const Config& after)
{
    return before.ui.show_node_library != after.ui.show_node_library ||
           before.ui.show_property_panel != after.ui.show_property_panel ||
           before.ui.show_console != after.ui.show_console ||
           before.ui.show_output_window != after.ui.show_output_window ||
           before.ui.show_grid != after.ui.show_grid ||
           before.ui.running_animation != after.ui.running_animation ||
           before.ui.console_height != after.ui.console_height;
}

// std::string ↔ ImGui InputText 的桥（每帧同步；文本变即回写）
bool input_text_string(const char* id, std::string& value, std::size_t capacity)
{
    constexpr std::size_t kBuffer = 1024;
    char                  buffer[kBuffer] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
    if (capacity > 0 && capacity < kBuffer) {
        buffer[capacity] = '\0';
    }
    if (ImGui::InputText(id, buffer, sizeof(buffer))) {
        value = buffer;
        return true;
    }
    return false;
}

void load_copy(const Config& config)
{
    g_edit = config;
    // 机器状态（窗口几何）保持原样：不在面板里编辑，也不因面板保存而被改
    g_edit.ui.window_width  = config.ui.window_width;
    g_edit.ui.window_height = config.ui.window_height;
    g_edit.ui.window_pos_x  = config.ui.window_pos_x;
    g_edit.ui.window_pos_y  = config.ui.window_pos_y;
    g_baseline              = config;
    g_error.clear();
    g_notice.clear();
    g_loaded = true;
}

// ============================================================================
//  PB2-24 要点①：「新建网页版站点…」模态（写 `~/.brain-ai/providers.d/<id>.json`）
//   * 面板只收集字符串；生成 / 校验 / 落盘全部走 `ai::site_template`（纯函数 ⇒ 可离线断言）
//   * 成功后**立即** `ai::reload_provider_specs()` + 置 `result.provider_table_reloaded`
//     ⇒ 下拉即时刷新（宿主 `app.cpp` 据此重建 `provider` 枚举，无需重启）
// ============================================================================

constexpr const char* kNewSiteModal = "新建网页版站点";

bool                g_site_modal_open   = false; // 本帧请求 OpenPopup（需在同一窗口上下文内调用）
ai::WebSiteTemplate g_site_tpl;                  // 工作副本
int                 g_site_source_index = 0;     // 0 = 空白模板；>0 = 表内 web 条目（照抄）
bool                g_site_overwrite    = false; // 覆盖表里已有条目 / 已存在同名文件
std::string         g_site_error;                // 红字（校验 / 落盘 / 生效失败）
std::string         g_site_notice;               // 绿字（已创建 + 新的加载摘要）

void open_new_site_modal()
{
    const std::vector<std::string> web_ids = ai::provider_specs().ids("web");
    g_site_source_index                    = 0;
    g_site_tpl                             = ai::site_template_default();
    if (!web_ids.empty()) {
        // 默认「照抄表里第一个 web 条目」（R18）：站点身份 / 选择器一起带过来，改 id 即可
        g_site_source_index = 1;
        g_site_tpl = ai::site_template_from_spec(ai::provider_specs().find(web_ids.front()));
    }
    g_site_overwrite = false;
    g_site_error.clear();
    g_site_notice.clear();
    g_site_modal_open = true;
}

// 表里是否已有该 id（内置或用户层）——用于「覆盖」二次确认（守 R8：不静默替换）
bool site_id_in_table(const std::string& id)
{
    return !id.empty() && ai::provider_specs().find(id) != nullptr;
}

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

void draw_new_site_modal(SettingsPanelResult& result);

} // namespace

// ---------------------------------------------------------------------------
//  「新建网页版站点…」模态实现（续上文匿名命名空间；状态见 g_site_*）
// ---------------------------------------------------------------------------
namespace {

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

void draw_new_site_modal(SettingsPanelResult& result)
{
    if (g_site_modal_open) {
        ImGui::OpenPopup(kNewSiteModal); // 必须在设置窗口的上下文内调用
        g_site_modal_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(780.0f, 560.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kNewSiteModal, nullptr)) {
        return;
    }

    const ai::ProviderSpecs& specs = ai::provider_specs();
    ImGui::TextDisabled("三步：① 填 id + 登录页 → ②「创建并重新加载」→ ③「提供商」下拉立即出现（无需重启）");
    ImGui::TextDisabled("写到哪里：%s", paths::user_providers_dir().string().c_str());
    ImGui::Separator();

    // ---- 模板来源（照抄内置条目；R18）----
    const std::vector<std::string> web_ids = specs.ids("web");
    std::vector<std::string>       sources;
    sources.push_back("空白模板（只有 id + 登录页）");
    for (const std::string& id : web_ids) {
        sources.push_back("照抄 " + id);
    }
    int source_index = g_site_source_index;
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
        g_site_source_index = source_index;
        g_site_tpl          = (source_index == 0)
                                  ? ai::site_template_default()
                                  : ai::site_template_from_spec(
                                        specs.find(web_ids[static_cast<std::size_t>(source_index) - 1]));
        g_site_error.clear();
        g_site_notice.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("「照抄」= 连站点身份一起带过来，改个 id 就能建（R18）");
    ImGui::Separator();

    ImGui::BeginChild("##site_fields", ImVec2(0.0f, 350.0f), true);
    ImGui::TextUnformatted("站点身份");
    ImGui::TextDisabled("站点 id（= 文件名，只能字母 / 数字 / `-` / `_` / `.`）");
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##site_id", g_site_tpl.id, 64);
    ImGui::TextDisabled("显示名（下拉里显示的名字）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_display", g_site_tpl.display, 120);
    ImGui::TextDisabled("登录页地址（**站点身份唯一来源**：必须 http:// 或 https://）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_login", g_site_tpl.login_url, 400);
    ImGui::TextDisabled("登录窗口标题（留空 = 自动「AIwrite · <显示名> 登录（登录后关闭窗口）」）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_window", g_site_tpl.window_title, 200);
    ImGui::Separator();
    ImGui::TextUnformatted("站点选择器（可后补：留空 = 登录型条目 —— 登录 / 登录态可用，生成会在运行期明确报错）");
    ImGui::TextDisabled("输入框选择器（例 div.chat-input-editor / textarea[placeholder=\"Ask …\"]）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_input", g_site_tpl.input_selector, 300);
    const std::vector<std::string> send_kinds = {"key", "click"};
    ImGui::SetNextItemWidth(160.0f);
    combo_string("发送方式", g_site_tpl.send_kind, send_kinds);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    input_text_string("##site_send_value", g_site_tpl.send_value, 200);
    ImGui::SameLine();
    ImGui::TextDisabled("键名（Enter）或发送按钮选择器");
    ImGui::TextDisabled("回答容器选择器（需要**先有一条回答**才存在 —— 唯一人工关卡，见 §3.4）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_answer", g_site_tpl.answer_selector, 300);
    const std::vector<std::string> done_kinds = {"（不设完成判据）", "selector_gone",
                                                 "selector_present"};
    std::string done_kind = g_site_tpl.done_kind.empty() ? done_kinds.front() : g_site_tpl.done_kind;
    ImGui::SetNextItemWidth(200.0f);
    if (combo_string("完成判据", done_kind, done_kinds)) {
        g_site_tpl.done_kind = (done_kind == done_kinds.front()) ? std::string() : done_kind;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300.0f);
    input_text_string("##site_done_selector", g_site_tpl.done_selector, 300);
    ImGui::TextDisabled("登录 Cookie 名（逗号分隔；留空 = 该 origin 只要有 Cookie 即视为已登录）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_cookies", g_site_tpl.cookie_names, 300);
    ImGui::TextDisabled("页面内取 token 表达式（纯 DOM 站点一般留空；例 localStorage.getItem('token')）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_token", g_site_tpl.token_expr, 300);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("轮询间隔（毫秒）", &g_site_tpl.answer_poll_ms, 100, 500);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("最大轮询次数", &g_site_tpl.answer_max_polls, 10, 60);
    ImGui::TextDisabled("备注（写进条目 notes：如实标注是否实测）");
    ImGui::SetNextItemWidth(-FLT_MIN);
    input_text_string("##site_notes", g_site_tpl.notes, 400);
    ImGui::EndChild();
    ImGui::Separator();

    // ---- 现场提示：可写性 / 待实测 / 二次确认 ----
    const std::string pending = ai::site_template_pending(g_site_tpl);
    if (!pending.empty()) {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f),
                           "生成未就绪：缺 %s（**登录 / 登录态仍可用**；生成会在运行期明确报错）",
                           pending.c_str());
    }
    const std::string wrong = ai::site_template_error(g_site_tpl);
    if (!wrong.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "✗ %s", wrong.c_str());
    }
    else if (site_id_in_table(g_site_tpl.id)) {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f),
                           "表里已有条目「%s」（内置或此前新建）—— 创建将以**用户层条目**替换它",
                           g_site_tpl.id.c_str());
    }
    ImGui::Checkbox("覆盖表里已有条目 / 已存在同名文件（R8：默认不覆盖）", &g_site_overwrite);

    if (ImGui::Button("创建并重新加载", ImVec2(150.0f, 0.0f))) {
        std::string error = ai::site_template_error(g_site_tpl);
        if (error.empty() && site_id_in_table(g_site_tpl.id) && !g_site_overwrite) {
            error = "表里已有条目「" + g_site_tpl.id +
                    "」——请改 id，或勾选「覆盖表里已有条目」后重试";
        }
        if (!error.empty()) {
            g_site_error  = error;
            g_site_notice.clear();
        }
        else {
            std::filesystem::path written;
            if (!ai::write_user_site_file(g_site_tpl, paths::user_providers_dir(),
                                          g_site_overwrite, &error, &written)) {
                g_site_error = error;
                g_site_notice.clear();
                log::warn("[站点] 新建条目失败：" + error);
            }
            else {
                // 写完**立即**重载并复核该条目是否真的生效（R8：坏表不能只报成功）
                ai::reload_provider_specs();
                result.provider_table_reloaded = true;
                const ai::ProviderSpecs& after   = ai::provider_specs();
                const ai::ProviderSpec*  created = after.find(g_site_tpl.id);
                if (created == nullptr || created->kind != "web") {
                    g_site_error = "已写文件，但重新加载后该条目**未生效**：\n" +
                                   site_report_line(after.report, g_site_tpl.id) +
                                   "\n（修好文件后点设置里的「重新加载配置表」）";
                    g_site_notice.clear();
                }
                else {
                    g_site_overwrite = true; // 再点即覆盖（文件已存在）
                    g_site_notice = "已创建 " + written.string() + "\n加载：" +
                                    after.report.summary() +
                                    "\n「提供商」下拉立即出现该条目（无需重启）；选择器实测见 §3.4";
                    g_site_error.clear();
                }
                log::info("[站点] 新建条目 " + written.string() + " → " + after.report.summary());
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("打开 providers.d 目录", ImVec2(170.0f, 0.0f))) {
        if (utils::open_folder(paths::user_providers_dir().string())) {
            g_site_notice = "已打开 ~/.brain-ai/providers.d（每站一个 JSON，可分享/备份）";
        }
        else {
            g_site_error = "打开失败（目录尚未创建：点「创建并重新加载」会先建目录）";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    if (!g_site_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", g_site_error.c_str());
    }
    else if (!g_site_notice.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%s", g_site_notice.c_str());
    }

    ImGui::EndPopup();
}

} // namespace

void open_settings_panel()
{
    g_loaded = false; // 每次打开都从当前配置重取（避免显示过期值）
    g_open   = true;
}

void close_settings_panel()
{
    g_open   = false;
    g_loaded = false;
}


SettingsPanelResult draw_settings_panel(Config& config)
{
    SettingsPanelResult result;
    if (!g_open) {
        return result; // 未打开：零副作用
    }
    if (!g_loaded) {
        load_copy(config);
    }

    ImGui::SetNextWindowSize(ImVec2(780.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kSettingsWindow, &g_open)) {
        ImGui::End();
        if (!g_open) {
            g_loaded = false;
        }
        return result;
    }

    ImGui::TextDisabled("文件：%s", paths::config_file().string().c_str());
    ImGui::TextDisabled("「保存」写完即生效；标「暂不生效」的项当前版本未接线（归口见使用说明）。");
    ImGui::Separator();

    // ---- ① 界面 ----
    if (ImGui::CollapsingHeader("界面", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("显示节点库", &g_edit.ui.show_node_library);
        ImGui::SameLine();
        ImGui::Checkbox("显示参数面板", &g_edit.ui.show_property_panel);
        ImGui::SameLine();
        ImGui::Checkbox("显示 Console", &g_edit.ui.show_console);
        ImGui::Checkbox("显示输出面板", &g_edit.ui.show_output_window);
        ImGui::Separator();
        ImGui::Checkbox("画布显示网格", &g_edit.ui.show_grid);
        ImGui::SameLine();
        ImGui::Checkbox("运行中动效", &g_edit.ui.running_animation);
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("Console 高度（像素）", &g_edit.ui.console_height, 60, 400);
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("网格间距", &g_edit.ui.grid_size, 4, 200);
        ImGui::SameLine();
        ImGui::TextDisabled("（暂不生效：归口 PD-03）");
    }

    // ---- ② 输出与归档 ----
    if (ImGui::CollapsingHeader("输出与归档", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted("归档目录");
        ImGui::SetNextItemWidth(520.0f);
        input_text_string("##archive_dir", g_edit.output.archive_dir, 500);
        ImGui::SameLine();
        if (ImGui::Button("选择目录…##archive")) {
            const std::string picked = utils::pick_folder(g_edit.output.archive_dir);
            if (!picked.empty()) {
                g_edit.output.archive_dir = picked;
            }
        }
        ImGui::TextDisabled("实际位置：%s",
                            paths::expand(g_edit.output.archive_dir).string().c_str());
        ImGui::Checkbox("运行完成后自动打开归档目录",
                        &g_edit.output.auto_open_on_complete);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("运行结束且归档成功后，用资源管理器打开本次归档目录（默认关）");
        }
        ImGui::Checkbox("保留多份历史归档", &g_edit.output.keep_history);
        if (g_edit.output.keep_history) {
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderInt("保留份数", &g_edit.output.max_history, 1, 100);
        }
        else {
            ImGui::TextDisabled("不保留多份时：只留最近 1 份归档");
        }
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("归档清理阈值（天，0 = 不清理）", &g_edit.output.ttl_days, 0, 3650);
    }

    // ---- ③ 网络超时（**已接线**：官方 API 的默认超时；站点表未声明 limits.* 时生效）----
    if (ImGui::CollapsingHeader("网络超时")) {
        ImGui::TextDisabled("官方 API 的**默认**连接 / 读超时（M_patchAB 批 4 起生效）；\n"
                            "站点表里声明了 limits.* 时**以表为准**（表优先）。");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("连接超时（毫秒）", &g_edit.timeout.connect_ms, 500, 5000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("首字节超时（毫秒）", &g_edit.timeout.first_byte_ms, 1000, 10000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("流空闲超时（毫秒）", &g_edit.timeout.stream_idle_ms, 5000, 30000);
    }

    // ---- ③′ 网络与证书（M_patchAB §2.3 · `PB-06` 剩余）----
    if (ImGui::CollapsingHeader("网络与证书（代理 / 自签）")) {
        ImGui::TextDisabled("出不去（企业网络）：填代理，或让程序读环境变量 HTTPS_PROXY / HTTP_PROXY；\n"
                            "证书不受信（内网自签）：打开下面的「自签证书兼容」（默认**严格校验**）。");
        ImGui::SetNextItemWidth(380.0f);
        input_text_string("##proxy", g_edit.network.proxy, 300);
        ImGui::TextDisabled("代理地址（例 http://127.0.0.1:7890；留空 = 不显式使用代理）");
        ImGui::Checkbox("自动读取环境变量（HTTPS_PROXY / HTTP_PROXY / ALL_PROXY）",
                        &g_edit.network.proxy_from_env);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("优先顺序：NO_PROXY 命中 ⇒ 直达；否则「上面手填的代理」优先，\n"
                              "再退回环境变量（按目标 https / http 各取其一）");
        }
        ImGui::SetNextItemWidth(380.0f);
        input_text_string("##no_proxy", g_edit.network.no_proxy, 400);
        ImGui::TextDisabled("不走代理的地址（逗号分隔；支持 .corp.com 后缀与 host:port；空 = 读 NO_PROXY）");
        ImGui::Checkbox("自签证书兼容（关闭证书校验）", &g_edit.network.insecure_tls);
        if (g_edit.network.insecure_tls) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "⚠ 危险：仅建议内网/离线环境使用");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("关闭后不再校验站点证书链 ⇒ 会被中间人替换；\n"
                              "普通外网请保持关闭状态（默认严格校验）");
        }
    }

    // ---- ④ 失败重试（**已接线**：官方 API 节点）----
    if (ImGui::CollapsingHeader("错误与重试")) {
        ImGui::TextDisabled("只对**可重试**失败重试：429 / 5xx / 网络超时；\n"
                            "凭证 / 余额 / 请求不合法**不重试**（避免加剧风控）。");
        ImGui::Checkbox("自动重试失败节点", &g_edit.error.retry_enabled);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("重试次数", &g_edit.error.retry_count, 1, 1);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("重试间隔（毫秒）", &g_edit.error.retry_interval_ms, 500, 1000);
    }

    // ---- ④′ 凭据库（M_patchAB §2.3 · `PB-06` 剩余）----
    if (ImGui::CollapsingHeader("凭据库（保存位置 / 保留天数）")) {
        ImGui::TextDisabled("API Key 以 Windows DPAPI 加密保存（**不落明文**）；\n"
                            "「保留天数」只影响「即将过期」提示与启动惰性清理（0 = 不清理）。");
        ImGui::SetNextItemWidth(300.0f);
        input_text_string("##cred_dir", g_edit.credentials.dir, 400);
        ImGui::TextDisabled("保存目录（空 = ~/.brain-ai/credentials）");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("保留天数（0 = 不清理）", &g_edit.credentials.ttl_days, 1, 10);
        ImGui::TextDisabled("后端：%s（本版本只支持 dpapi）", g_edit.credentials.backend.c_str());
    }

    // ---- ⑤ 高级 ----
    if (ImGui::CollapsingHeader("高级")) {
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("logs 保留天数（0 = 不清理）", &g_edit.advanced.log_ttl_days, 0, 3650);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("启动时删除 logs/ 里超过该天数的旧日志\n"
                              "（永远不动正在写的 app.log / workflow.log）");
        }
        ImGui::TextDisabled("配置：%s", g_edit.advanced.config_path.c_str());
        ImGui::TextDisabled("日志目录：%s", g_edit.advanced.log_dir.c_str());
        if (ImGui::Button("打开日志目录")) {
            if (utils::open_folder(paths::logs_dir().string())) {
                g_notice = "已在资源管理器中打开日志目录";
            }
            else {
                g_error = "打开日志目录失败（目录可能尚未创建）";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("打开数据目录")) {
            if (utils::open_folder(paths::data_root().string())) {
                g_notice = "已在资源管理器中打开数据目录";
            }
            else {
                g_error = "打开数据目录失败（目录可能尚未创建）";
            }
        }
    }

    // ---- ⑥ 提供商与站点表（摘要 + 错误/警告明细 + 新建站点 + 重新加载）----
    if (ImGui::CollapsingHeader("提供商与站点表")) {
        const ai::ProviderSpecs&  specs  = ai::provider_specs();
        const ai::SpecLoadReport& report = specs.report;
        ImGui::Text("条目：%zu（官方 API %zu / 网页版 %zu）", specs.items.size(),
                    specs.count_kind("official"), specs.count_kind("web"));
        ImGui::Text("加载：%s", report.summary().c_str());

        // PB2-24 要点③：配置表错误 / 警告**界面可见**（原文照登，含「已忽略该层 / 已跳过该条 /
        // 未知字段」；改完点「重新加载配置表」刷新本清单）
        const std::size_t problems = report.errors.size() + report.warnings.size();
        if (problems == 0) {
            ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "配置表：无错误 / 无警告");
        }
        else {
            ImGui::TextColored(
                report.errors.empty() ? ImVec4(0.85f, 0.70f, 0.30f, 1.0f)
                                      : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                "配置表问题：错误 %zu 条 / 警告 %zu 条（下列为原文）", report.errors.size(),
                report.warnings.size());
            ImGui::BeginChild("##spec_problems", ImVec2(0.0f, 92.0f), true);
            for (const std::string& text : report.errors) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "✗ %s", text.c_str());
            }
            for (const std::string& text : report.warnings) {
                ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "! %s", text.c_str());
            }
            ImGui::EndChild();
        }

        if (ImGui::Button("新建网页版站点…")) { // PB2-24 要点①（写 providers.d/<id>.json）
            open_new_site_modal();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("三步：填 id + 登录页 → 「创建并重新加载」→「提供商」下拉立即出现\n"
                              "（无需重启；文件落在 ~/.brain-ai/providers.d/，每站一份可分享）\n"
                              "选择器可后补：留空 = 登录型条目，登录/探测可用、生成会明确报错");
        }
        ImGui::SameLine();
        if (ImGui::Button("重新加载配置表")) {
            ai::reload_provider_specs();
            const ai::SpecLoadReport& after = ai::provider_specs().report;
            result.provider_table_reloaded  = true; // 宿主据此重建 provider 参数枚举
            g_notice = "配置表已重新加载：" + after.summary();
            g_error.clear();
            // 原文也进 Console / app.log（面板长清单 + 日志留痕，双通道）
            for (const std::string& text : after.errors) {
                log::warn("[站点表] " + text);
            }
            for (const std::string& text : after.warnings) {
                log::warn("[站点表] " + text);
            }
            log::info("[设置] 已重新加载 provider 配置表：" + after.summary());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("改完 providers.json / providers.d 后点这里，无需重启\n"
                              "（配置表位置：程序目录 assets\\providers.json 与 ~/.brain-ai/providers.d）");
        }
        ImGui::SameLine();
        if (ImGui::Button("打开用户配置表目录")) {
            if (utils::open_folder(paths::user_providers_dir().string())) {
                g_notice = "已打开 ~/.brain-ai/providers.d";
            }
            else {
                g_error = "打开失败（providers.d 尚未创建；可用「新建网页版站点…」先建一条）";
            }
        }
    }

    // ---- ⑦ 数据安全（自动保存与恢复；PC-01 / PC-02 · M_patchAB §4）----
    if (ImGui::CollapsingHeader("数据安全（自动保存）")) {
        ImGui::Checkbox("启用自动保存", &g_edit.ui.autosave_enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("工作流有改动、且静默超过下面的秒数后，自动写入快照\n"
                              "（崩溃 / 未保存退出后，下次启动可恢复）");
        }
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("静默多少秒后写快照", &g_edit.ui.autosave_idle_sec, 1, 60);
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("保留历史快照份数（0 = 不留）", &g_edit.ui.autosave_keep, 0, 20);
        ImGui::Checkbox("启动时提示恢复未保存的更改", &g_edit.ui.autosave_restore_prompt);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("恢复窗口里的「忽略，且不再提示」会把这个开关关掉（可在此重新打开）");
        }
        ImGui::TextDisabled("位置：%s", paths::snapshots_dir().string().c_str());
        ImGui::TextDisabled("autosave.json = 最新；autosave_1..N = 历史；仅在「有改动」时写盘");
        ImGui::SameLine();
        if (ImGui::Button("打开快照目录")) {
            if (utils::open_folder(paths::snapshots_dir().string())) {
                g_notice = "已在资源管理器中打开快照目录";
            }
            else {
                g_error = "打开快照目录失败（目录可能尚未创建）";
            }
        }
    }

    // ---- 底部：错误 / 提示 / 动作 ----
    ImGui::Separator();
    if (!g_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "无法保存：%s", g_error.c_str());
    }
    else if (!g_notice.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%s", g_notice.c_str());
    }

    if (ImGui::Button("保存", ImVec2(110.0f, 0.0f))) {
        const std::string error = utils::validate_settings(g_edit);
        if (!error.empty()) {
            g_error  = error;
            g_notice.clear();
        }
        else if (save_config(paths::config_file(), g_edit)) {
            config                    = g_edit;
            result.saved              = true;
            result.visibility_changed = needs_resync(g_baseline, config);
            result.message            = "设置已保存";
            g_baseline                = config;
            g_error.clear();
            g_notice = "已保存到 " + paths::config_file().string() + "（原文件已备份为 config.toml.bak）";
            log::info("[设置] 已保存 config.toml；界面需同步=" +
                      std::string(result.visibility_changed ? "是" : "否"));
        }
        else {
            g_error = "写入 config.toml 失败（目录不可写或磁盘已满）：" +
                      paths::config_file().string();
            g_notice.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(110.0f, 0.0f))) {
        g_loaded = false;
        g_open   = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("恢复默认值", ImVec2(120.0f, 0.0f))) {
        Config fresh;
        // 机器状态与用户数据不参与「恢复默认」：窗口几何、首启标记、多 provider 实例参数
        fresh.ui.window_width  = g_edit.ui.window_width;
        fresh.ui.window_height = g_edit.ui.window_height;
        fresh.ui.window_pos_x  = g_edit.ui.window_pos_x;
        fresh.ui.window_pos_y  = g_edit.ui.window_pos_y;
        fresh.ui.first_run_done = g_edit.ui.first_run_done;
        fresh.providers         = g_edit.providers;
        g_edit                  = fresh;
        g_notice                = "已恢复默认值（**尚未保存**：确认无误后点「保存」）";
        g_error.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("改完点「保存」才生效");

    // PB2-24 要点①「新建网页版站点…」模态（必须在设置窗口上下文内绘制；未打开时零副作用）
    draw_new_site_modal(result);

    ImGui::End();
    return result;
}

bool settings_panel_visible()
{
    return g_open;
}

} // namespace aiwrite::ui
