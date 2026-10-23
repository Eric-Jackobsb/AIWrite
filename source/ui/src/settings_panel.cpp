#include "settings_panel.h"

#include "file_dialog.h"
#include "log.h"
#include "paths.h"
#include "provider_spec.h"
#include "settings_validation.h"
#include "output_archive.h"   // PW-11：kHistoryFloor（keep_history=false 时的归档保留下限）
#include "site_template.h"   // PB2-24：自建网页版站点模板（生成 / 校验 / 落盘）
#include "official_template.h" // 自定义 AI 收口：自建官方 API 提供商模板（生成 / 校验 / 落盘）
#include "credential.h"       // PB-06：API Key 以 Windows DPAPI 入库（不落明文）
#include "string_util.h"      // CLN-05：trim 收敛

#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "settings_panel_detail.h"  // ARCH-Z1：模块内契约（模态名 + 5 个内部入口）

namespace aiwrite::ui {

using namespace settings_detail;  // 原匿名命名空间函数（同 TU 直接调用，名字查找不变）

namespace settings_detail {

constexpr const char* kSettingsWindow = "设置";

// 面板状态见 `SettingsPanelState`（settings_panel.h · ARCH-A6）：原先的 6 条文件级
// 可变变量（open / loaded / edit / baseline / error / notice）已迁入该对象，
// 由 `EditorController` 持有并逐帧传入 —— 本文件不再持有任何跨帧可变状态。

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

void load_copy(SettingsPanelState& state, const Config& config)
{
    state.edit = config;
    // 机器状态（窗口几何）保持原样：不在面板里编辑，也不因面板保存而被改
    state.edit.ui.window_width  = config.ui.window_width;
    state.edit.ui.window_height = config.ui.window_height;
    state.edit.ui.window_pos_x  = config.ui.window_pos_x;
    state.edit.ui.window_pos_y  = config.ui.window_pos_y;
    state.baseline              = config;
    state.error.clear();
    state.notice.clear();
    state.loaded = true;
}
// ============================================================================
//  PB2-24 要点①：「新建网页版站点…」模态（写 `~/.brain-ai/providers.d/<id>.json`）
//   * 面板只收集字符串；生成 / 校验 / 落盘全部走 `ai::site_template`（纯函数 ⇒ 可离线断言）
//   * 成功后**立即** `ai::reload_provider_specs()` + 置 `result.provider_table_reloaded`
//     ⇒ 下拉即时刷新（宿主 `app.cpp` 据此重建 `provider` 枚举，无需重启）
// ============================================================================

// 模态草稿（g_site_* → `SettingsPanelState::site_*`）：面板只收集字符串；
// 生成 / 校验 / 落盘全部走 `ai::site_template`（纯函数 ⇒ 可离线断言）。

void open_new_site_modal(SettingsPanelState& state)
{
    const std::vector<std::string> web_ids = ai::providerSpecs().ids("web");
    state.site_source_index                    = 0;
    state.site_tpl                             = ai::siteTemplateDefault();
    if (!web_ids.empty()) {
        // 默认「照抄表里第一个 web 条目」（R18）：站点身份 / 选择器一起带过来，改 id 即可
        state.site_source_index = 1;
        state.site_tpl = ai::siteTemplateFromSpec(ai::providerSpecs().find(web_ids.front()));
    }
    state.site_overwrite = false;
    state.site_error.clear();
    state.site_notice.clear();
    state.site_modal_open = true;
}

// 表里是否已有该 id（内置或用户层）——用于「覆盖」二次确认（守 R8：不静默替换）
bool id_in_table(const std::string& id)
{
    return !id.empty() && ai::providerSpecs().find(id) != nullptr;
}
// ============================================================================
//  自定义 AI 收口：「新建 API 提供商…」模态（写 `~/.brain-ai/providers.d/<id>.json`）
//   * 与「新建网页版站点…」同一机制（生成 / 校验 / 落盘走 `ai::official_template` 纯函数）
//   * 成功后**立即** `ai::reloadProviderSpecs()` + 置 `result.provider_table_reloaded`
//   * 填了 API Key ⇒ 写盘后**立即**入凭据库（Windows DPAPI；不落明文、不进 JSON）
// ============================================================================


void open_new_official_modal(SettingsPanelState& state)
{
    const std::vector<std::string> official_ids = ai::providerSpecs().ids("official");
    state.off_source_index                          = 0;
    state.off_tpl                                   = ai::officialTemplateDefault();
    if (!official_ids.empty()) {
        // 默认「照抄表里第一个 official 条目」（R18）：地址 / 模型 / 能力一起带过来，改 id 即可
        state.off_source_index = 1;
        state.off_tpl = ai::officialTemplateFromSpec(ai::providerSpecs().find(official_ids.front()));
    }
    state.off_overwrite = false;
    state.off_error.clear();
    state.off_notice.clear();
    std::memset(state.off_key, 0, sizeof(state.off_key));
    state.off_modal_open = true;
}
}  // namespace settings_detail

void openSettingsPanel(SettingsPanelState& state)
{
    state.loaded = false; // 每次打开都从当前配置重取（避免显示过期值）
    state.open   = true;
}

void closeSettingsPanel(SettingsPanelState& state)
{
    state.open   = false;
    state.loaded = false;
}

SettingsPanelResult drawSettingsPanel(SettingsPanelState& state, Config& config)
{
    SettingsPanelResult result;
    if (!state.open) {
        return result; // 未打开：零副作用
    }
    if (!state.loaded) {
        load_copy(state, config);
    }

    ImGui::SetNextWindowSize(ImVec2(780.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kSettingsWindow, &state.open)) {
        ImGui::End();
        if (!state.open) {
            state.loaded = false;
        }
        return result;
    }

    ImGui::TextDisabled("文件：%s", paths::configFile().string().c_str());
    ImGui::TextDisabled("「保存」写完即生效；标「暂不生效」的项当前版本未接线（归口见使用说明）。");
    ImGui::Separator();

    // ---- ① 界面 ----
    if (ImGui::CollapsingHeader("界面", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("显示节点库", &state.edit.ui.show_node_library);
        ImGui::SameLine();
        ImGui::Checkbox("显示参数面板", &state.edit.ui.show_property_panel);
        ImGui::SameLine();
        ImGui::Checkbox("显示 Console", &state.edit.ui.show_console);
        ImGui::Checkbox("显示输出面板", &state.edit.ui.show_output_window);
        ImGui::Separator();
        ImGui::Checkbox("画布显示网格", &state.edit.ui.show_grid);
        ImGui::SameLine();
        ImGui::Checkbox("运行中动效", &state.edit.ui.running_animation);
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("Console 高度（像素）", &state.edit.ui.console_height, 60, 400);
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("网格间距", &state.edit.ui.grid_size, 4, 200);
        ImGui::SameLine();
        ImGui::TextDisabled("（暂不生效：归口 PD-03）");
    }

    // ---- ② 输出与归档 ----
    if (ImGui::CollapsingHeader("输出与归档", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted("归档目录");
        ImGui::SetNextItemWidth(520.0f);
        input_text_string("##archive_dir", state.edit.output.archive_dir, 500);
        ImGui::SameLine();
        if (ImGui::Button("选择目录…##archive")) {
            const std::string picked = utils::pickFolder(state.edit.output.archive_dir);
            if (!picked.empty()) {
                state.edit.output.archive_dir = picked;
            }
        }
        ImGui::TextDisabled("实际位置：%s",
                            paths::expand(state.edit.output.archive_dir).string().c_str());
        ImGui::Checkbox("运行完成后自动打开归档目录",
                        &state.edit.output.auto_open_on_complete);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("运行结束且归档成功后，用资源管理器打开本次归档目录（默认关）");
        }
        ImGui::Checkbox("保留多份历史归档", &state.edit.output.keep_history);
        if (state.edit.output.keep_history) {
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderInt("保留份数", &state.edit.output.max_history, 1, 100);
        }
        else {
            ImGui::TextDisabled("不保留多份时：仍保留最近 %d 份归档（PW-11：避免复盘证据被清）",
                                aiwrite::utils::kHistoryFloor);
        }
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("归档清理阈值（天，0 = 不清理）", &state.edit.output.ttl_days, 0, 3650);
    }

    // ---- ③ 网络超时（**已接线**：官方 API 的默认超时；站点表未声明 limits.* 时生效）----
    if (ImGui::CollapsingHeader("网络超时")) {
        ImGui::TextDisabled("官方 API 的**默认**连接 / 读超时（M_patchAB 批 4 起生效）；\n"
                            "站点表里声明了 limits.* 时**以表为准**（表优先）。");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("连接超时（毫秒）", &state.edit.timeout.connect_ms, 500, 5000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("首字节超时（毫秒）", &state.edit.timeout.first_byte_ms, 1000, 10000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("流空闲超时（毫秒）", &state.edit.timeout.stream_idle_ms, 5000, 30000);
    }

    // ---- ③′ 网络与证书（M_patchAB §2.3 · `PB-06` 剩余）----
    if (ImGui::CollapsingHeader("网络与证书（代理 / 自签）")) {
        ImGui::TextDisabled("出不去（企业网络）：填代理，或让程序读环境变量 HTTPS_PROXY / HTTP_PROXY；\n"
                            "证书不受信（内网自签）：打开下面的「自签证书兼容」（默认**严格校验**）。");
        ImGui::SetNextItemWidth(380.0f);
        input_text_string("##proxy", state.edit.network.proxy, 300);
        ImGui::TextDisabled("代理地址（例 http://127.0.0.1:7890；留空 = 不显式使用代理）");
        ImGui::Checkbox("自动读取环境变量（HTTPS_PROXY / HTTP_PROXY / ALL_PROXY）",
                        &state.edit.network.proxy_from_env);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("优先顺序：NO_PROXY 命中 ⇒ 直达；否则「上面手填的代理」优先，\n"
                              "再退回环境变量（按目标 https / http 各取其一）");
        }
        ImGui::SetNextItemWidth(380.0f);
        input_text_string("##no_proxy", state.edit.network.no_proxy, 400);
        ImGui::TextDisabled("不走代理的地址（逗号分隔；支持 .corp.com 后缀与 host:port；空 = 读 NO_PROXY）");
        ImGui::Checkbox("自签证书兼容（关闭证书校验）", &state.edit.network.insecure_tls);
        if (state.edit.network.insecure_tls) {
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
        ImGui::Checkbox("自动重试失败节点", &state.edit.error.retry_enabled);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("重试次数", &state.edit.error.retry_count, 1, 1);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("重试间隔（毫秒）", &state.edit.error.retry_interval_ms, 500, 1000);
    }

    // ---- ④′ 凭据库（M_patchAB §2.3 · `PB-06` 剩余）----
    if (ImGui::CollapsingHeader("凭据库（保存位置 / 保留天数）")) {
        ImGui::TextDisabled("API Key 以 Windows DPAPI 加密保存（**不落明文**）；\n"
                            "「保留天数」只影响「即将过期」提示与启动惰性清理（0 = 不清理）。");
        ImGui::SetNextItemWidth(300.0f);
        input_text_string("##cred_dir", state.edit.credentials.dir, 400);
        ImGui::TextDisabled("保存目录（空 = ~/.brain-ai/credentials）");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("保留天数（0 = 不清理）", &state.edit.credentials.ttl_days, 1, 10);
        ImGui::TextDisabled("后端：%s（本版本只支持 dpapi）", state.edit.credentials.backend.c_str());
    }

    // ---- ⑤ 高级 ----
    if (ImGui::CollapsingHeader("高级")) {
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("logs 保留天数（0 = 不清理）", &state.edit.advanced.log_ttl_days, 0, 3650);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("启动时删除 logs/ 里超过该天数的旧日志\n"
                              "（永远不动正在写的 app.log / workflow.log）");
        }
        ImGui::TextDisabled("配置：%s", state.edit.advanced.config_path.c_str());
        ImGui::TextDisabled("日志目录：%s", state.edit.advanced.log_dir.c_str());
        if (ImGui::Button("打开日志目录")) {
            if (utils::openFolder(paths::logsDir().string())) {
                state.notice = "已在资源管理器中打开日志目录";
            }
            else {
                state.error = "打开日志目录失败（目录可能尚未创建）";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("打开数据目录")) {
            if (utils::openFolder(paths::dataRoot().string())) {
                state.notice = "已在资源管理器中打开数据目录";
            }
            else {
                state.error = "打开数据目录失败（目录可能尚未创建）";
            }
        }
    }

    // ---- ⑥ 提供商与站点表（摘要 + 错误/警告明细 + 新建站点 + 重新加载）----
    if (ImGui::CollapsingHeader("提供商与站点表")) {
        const ai::ProviderSpecs&  specs  = ai::providerSpecs();
        const ai::SpecLoadReport& report = specs.report;
        ImGui::Text("条目：%zu（官方 API %zu / 网页版 %zu）", specs.items.size(),
                    specs.countKind("official"), specs.countKind("web"));
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

        if (ImGui::Button("新建 API 提供商…")) { // 自定义 AI 收口（写 providers.d/<id>.json）
            open_new_official_modal(state);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("三步：填 id + API 地址 → 「创建并重新加载」→「提供商」下拉立即出现\n"
                              "（无需重启；文件落在 ~/.brain-ai/providers.d/，每份可分享）\n"
                              "支持 openai / anthropic / gemini 协议；模型留空 = 自由填写；\n"
                              "API Key 可在此直接填（Windows DPAPI 入库，不落明文）");
        }
        ImGui::SameLine();
        if (ImGui::Button("新建网页版站点…")) { // PB2-24 要点①（写 providers.d/<id>.json）
            open_new_site_modal(state);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("三步：填 id + 登录页 → 「创建并重新加载」→「提供商」下拉立即出现\n"
                              "（无需重启；文件落在 ~/.brain-ai/providers.d/，每站一份可分享）\n"
                              "选择器可后补：留空 = 登录型条目，登录/探测可用、生成会明确报错");
        }
        ImGui::SameLine();
        if (ImGui::Button("重新加载配置表")) {
            ai::reloadProviderSpecs();
            const ai::SpecLoadReport& after = ai::providerSpecs().report;
            result.provider_table_reloaded  = true; // 宿主据此重建 provider 参数枚举
            state.notice = "配置表已重新加载：" + after.summary();
            state.error.clear();
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
            if (utils::openFolder(paths::userProvidersDir().string())) {
                state.notice = "已打开 ~/.brain-ai/providers.d";
            }
            else {
                state.error = "打开失败（providers.d 尚未创建；可用「新建网页版站点…」先建一条）";
            }
        }
    }

    // ---- ⑦ 数据安全（自动保存与恢复；PC-01 / PC-02 · M_patchAB §4）----
    if (ImGui::CollapsingHeader("数据安全（自动保存）")) {
        ImGui::Checkbox("启用自动保存", &state.edit.ui.autosave_enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("工作流有改动、且静默超过下面的秒数后，自动写入快照\n"
                              "（崩溃 / 未保存退出后，下次启动可恢复）");
        }
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("静默多少秒后写快照", &state.edit.ui.autosave_idle_sec, 1, 60);
        ImGui::SetNextItemWidth(240.0f);
        ImGui::SliderInt("保留历史快照份数（0 = 不留）", &state.edit.ui.autosave_keep, 0, 20);
        ImGui::Checkbox("启动时提示恢复未保存的更改", &state.edit.ui.autosave_restore_prompt);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("恢复窗口里的「忽略，且不再提示」会把这个开关关掉（可在此重新打开）");
        }
        ImGui::TextDisabled("位置：%s", paths::snapshotsDir().string().c_str());
        ImGui::TextDisabled("autosave.json = 最新；autosave_1..N = 历史；仅在「有改动」时写盘");
        ImGui::SameLine();
        if (ImGui::Button("打开快照目录")) {
            if (utils::openFolder(paths::snapshotsDir().string())) {
                state.notice = "已在资源管理器中打开快照目录";
            }
            else {
                state.error = "打开快照目录失败（目录可能尚未创建）";
            }
        }
    }

    // ---- 底部：错误 / 提示 / 动作 ----
    ImGui::Separator();
    if (!state.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "无法保存：%s", state.error.c_str());
    }
    else if (!state.notice.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%s", state.notice.c_str());
    }

    if (ImGui::Button("保存", ImVec2(110.0f, 0.0f))) {
        const std::string error = utils::validateSettings(state.edit);
        if (!error.empty()) {
            state.error  = error;
            state.notice.clear();
        }
        else if (saveConfig(paths::configFile(), state.edit)) {
            config                    = state.edit;
            result.saved              = true;
            result.visibility_changed = needs_resync(state.baseline, config);
            result.message            = "设置已保存";
            state.baseline                = config;
            state.error.clear();
            state.notice = "已保存到 " + paths::configFile().string() + "（原文件已备份为 config.toml.bak）";
            log::info("[设置] 已保存 config.toml；界面需同步=" +
                      std::string(result.visibility_changed ? "是" : "否"));
        }
        else {
            state.error = "写入 config.toml 失败（目录不可写或磁盘已满）：" +
                      paths::configFile().string();
            state.notice.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(110.0f, 0.0f))) {
        state.loaded = false;
        state.open   = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("恢复默认值", ImVec2(120.0f, 0.0f))) {
        Config fresh;
        // 机器状态与用户数据不参与「恢复默认」：窗口几何、首启标记、多 provider 实例参数
        fresh.ui.window_width  = state.edit.ui.window_width;
        fresh.ui.window_height = state.edit.ui.window_height;
        fresh.ui.window_pos_x  = state.edit.ui.window_pos_x;
        fresh.ui.window_pos_y  = state.edit.ui.window_pos_y;
        fresh.ui.first_run_done = state.edit.ui.first_run_done;
        fresh.providers         = state.edit.providers;
        state.edit                  = fresh;
        state.notice                = "已恢复默认值（**尚未保存**：确认无误后点「保存」）";
        state.error.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("改完点「保存」才生效");

    // PB2-24 要点①「新建网页版站点…」模态（必须在设置窗口上下文内绘制；未打开时零副作用）
    draw_new_site_modal(state, result);
    draw_new_official_modal(state, result);

    ImGui::End();
    return result;
}

bool settingsPanelVisible(const SettingsPanelState& state)
{
    return state.open;
}

}  // namespace aiwrite::ui
