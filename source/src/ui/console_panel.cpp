#include "ui/console_panel.h"

#include "ui/text_view.h"
#include "utils/file_dialog.h"
#include "utils/log.h"

#include <imgui.h>

#include <cstring>
#include <fstream>
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

// 过滤后的日志纯文本（与面板渲染共用同一个 log::filter，保证"复制的内容 == 屏幕可见"）
std::string console_visible_text(int level_filter, const std::string& search)
{
    const LevelFilter             filter = static_cast<LevelFilter>(level_filter);
    const std::vector<log::Entry> rows =
        log::filter(min_level_of(filter), filter != LevelFilter::All, search);
    std::string text;
    text.reserve(rows.size() * 64);
    for (const log::Entry& entry : rows) {
        text += entry.text;
        text += '\n';
    }
    return text;
}

void draw_console_panel(const char* title, bool* open, float default_height)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, default_height), ImGuiCond_FirstUseEver);
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

    // ---- 过滤结果：工具条（复制 / 导出）与列表共用，保证「复制的内容 == 屏幕可见」----
    const bool                    use_level_filter = g_level_filter != LevelFilter::All;
    const std::vector<log::Entry> rows =
        log::filter(min_level_of(g_level_filter), use_level_filter, std::string(g_search));
    const std::string visible_text =
        console_visible_text(static_cast<int>(g_level_filter), std::string(g_search));

    ImGui::Separator();
    if (ImGui::Button("复制可见")) {
        copy_text(visible_text, "[Console] 可见");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("复制当前级别过滤 + 搜索后的 %zu 行（与屏幕上可见内容一致）", rows.size());
    }
    ImGui::SameLine();
    if (ImGui::Button("复制全部")) {
        std::string all;
        all.reserve(log::entries().size() * 64);
        for (const log::Entry& entry : log::entries()) {
            all += entry.text;
            all += '\n';
        }
        copy_text(all, "[Console] 全部");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("复制缓冲区内的全部 %zu 行（不受过滤/搜索影响）", log::entries().size());
    }
    ImGui::SameLine();
    if (ImGui::Button("导出可见到文件…")) {
        const std::string path = utils::save_file({{"文本", "txt"}, {"全部文件", "*"}}, {},
                                                 "aiwrite-console.txt");
        if (!path.empty()) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (stream) {
                stream << visible_text;
                stream.close();
                log::info("[Console] 已导出可见日志：" + path + "（" + std::to_string(rows.size()) +
                          " 行）");
            }
            else {
                log::error("[Console] 导出失败（无法写入）：" + path);
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("把当前过滤 + 搜索后的内容导出为 UTF-8 文本（行数与可见行一致）");
    }
    ImGui::Separator();

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
        // PA-05：用户手动上滚 → 自动暂停（不再强制贴底）
        const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        if (!at_bottom && ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0.0f) {
            g_auto_scroll = false;
        }
        if (stick_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    if (!g_auto_scroll) {
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.20f, 1.0f), "自动滚动已暂停（检测到手动上滚）");
        ImGui::SameLine();
        if (ImGui::Button("回到底部")) {
            g_auto_scroll = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("恢复自动滚动并贴到底部");
        }
    }

    ImGui::TextDisabled("显示 %zu / %zu 条  |  日志文件: %s", rows.size(), log::entries().size(),
                        log::file_path_string().c_str());

    ImGui::End();
}

} // namespace aiwrite::ui
