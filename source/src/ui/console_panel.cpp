#include "ui/console_panel.h"

#include "utils/log.h"

#include <imgui.h>

#include <cstring>
#include <string>
#include <vector>

namespace aiwrite::ui {
namespace {

enum class LevelFilter : int { All = 0, Info = 1, Warn = 2, Error = 3 };

LevelFilter g_level_filter = LevelFilter::All;
char        g_search[128]  = {};
bool        g_auto_scroll  = true;

// 设计文档 12.1：INFO 灰白 / WARN 黄 / ERROR 红
ImVec4 level_color(log::Level level)
{
    switch (level) {
    case log::Level::Warn:
        return ImVec4(1.00f, 0.80f, 0.20f, 1.0f);
    case log::Level::Error:
        return ImVec4(1.00f, 0.30f, 0.30f, 1.0f);
    case log::Level::Info:
    default:
        return ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    }
}

log::Level min_level_of(LevelFilter filter)
{
    switch (filter) {
    case LevelFilter::Info:
        return log::Level::Info;
    case LevelFilter::Warn:
        return log::Level::Warn;
    case LevelFilter::Error:
        return log::Level::Error;
    case LevelFilter::All:
    default:
        return log::Level::Info;
    }
}

} // namespace

void draw_console_panel(const char* title, bool* open)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 220.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    // ---- 过滤工具条 ----
    const char* level_items[] = {"全部", "INFO", "WARN", "ERROR"};
    int filter_index = static_cast<int>(g_level_filter);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##ConsoleLevel", &filter_index, level_items, IM_ARRAYSIZE(level_items))) {
        g_level_filter = static_cast<LevelFilter>(filter_index);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##ConsoleSearch", "搜索日志...", g_search, IM_ARRAYSIZE(g_search));
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &g_auto_scroll);
    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        log::clear_entries();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("上限 %zu 条", log::kMaxEntries);
    ImGui::Separator();

    // ---- 日志列表（ImGuiListClipper 虚拟化）----
    const bool use_level_filter = g_level_filter != LevelFilter::All;
    const std::vector<log::Entry> rows = log::filter(min_level_of(g_level_filter), use_level_filter,
                                                     std::string(g_search));

    const bool stick_to_bottom = g_auto_scroll;

    if (ImGui::BeginChild("##ConsoleScroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const log::Entry& entry = rows[static_cast<std::size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, level_color(entry.level));
                ImGui::TextUnformatted(entry.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (stick_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::TextDisabled("显示 %zu / %zu 条  |  日志文件: %s", rows.size(), log::entries().size(),
                        log::file_path_string().c_str());

    ImGui::End();
}

} // namespace aiwrite::ui
