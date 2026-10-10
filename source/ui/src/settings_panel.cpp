#include "settings_panel.h"

#include "file_dialog.h"
#include "log.h"
#include "paths.h"
#include "provider_spec.h"
#include "settings_validation.h"

#include <imgui.h>

#include <cstdio>
#include <string>

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

    // ---- ③ 网络超时（暂不生效：归口 PB-08）----
    if (ImGui::CollapsingHeader("网络超时（暂不生效）")) {
        ImGui::TextDisabled("这些值已写入 config.toml，但当前版本的超时实际取自站点配置表（归口 PB-08）。");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("连接超时（毫秒）", &g_edit.timeout.connect_ms, 500, 5000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("首字节超时（毫秒）", &g_edit.timeout.first_byte_ms, 1000, 10000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("流空闲超时（毫秒）", &g_edit.timeout.stream_idle_ms, 5000, 30000);
    }

    // ---- ④ 错误与重试（暂不生效：归口 PB-08）----
    if (ImGui::CollapsingHeader("错误与重试（暂不生效）")) {
        ImGui::Checkbox("自动重试失败节点", &g_edit.error.retry_enabled);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("重试次数", &g_edit.error.retry_count, 1, 1);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("重试间隔（毫秒）", &g_edit.error.retry_interval_ms, 500, 1000);
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

    // ---- ⑥ 提供商与站点表（只读摘要 + 重新加载）----
    if (ImGui::CollapsingHeader("提供商与站点表")) {
        const ai::ProviderSpecs&  specs  = ai::provider_specs();
        const ai::SpecLoadReport& report = specs.report;
        ImGui::Text("条目：%zu（官方 API %zu / 网页版 %zu）", specs.items.size(),
                    specs.count_kind("official"), specs.count_kind("web"));
        ImGui::Text("加载：%s", report.summary().c_str());
        if (!report.errors.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "错误 %zu 条：%s",
                               report.errors.size(), report.errors.front().c_str());
        }
        if (ImGui::Button("重新加载配置表")) {
            ai::reload_provider_specs();
            const ai::SpecLoadReport& after = ai::provider_specs().report;
            g_notice = "配置表已重新加载：" + after.summary();
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
                g_error = "打开失败（providers.d 尚未创建；可先在参数面板里保存一次）";
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

    ImGui::End();
    return result;
}

bool settings_panel_visible()
{
    return g_open;
}

} // namespace aiwrite::ui
