#include "ui/text_view.h"

#include "utils/log.h"

#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace aiwrite::ui {
namespace {

struct TextCache {
    std::string       text;
    std::vector<char> buffer;
};

} // namespace

void draw_readonly_text(const char* id, const std::string& text, float height_lines,
                        std::size_t display_limit)
{
    static std::unordered_map<std::string, TextCache> cache;
    if (id == nullptr) {
        return;
    }

    const bool        truncated = display_limit > 0 && text.size() > display_limit;
    const std::string shown     = truncated ? text.substr(0, display_limit) : text;

    TextCache& entry = cache[id];
    if (entry.text != shown) {
        entry.text = shown;
        entry.buffer.assign(shown.begin(), shown.end());
        entry.buffer.push_back('\0');
    }

    const float height =
        ImGui::GetTextLineHeight() * height_lines + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::InputTextMultiline(id, entry.buffer.data(), entry.buffer.size(), ImVec2(-FLT_MIN, height),
                              ImGuiInputTextFlags_ReadOnly);
    if (truncated) {
        ImGui::TextDisabled("（显示已截断：%zu / %zu 字符；复制与导出为完整内容）", shown.size(),
                            text.size());
    }
}

void copy_text(const std::string& text, const std::string& log_prefix)
{
    ImGui::SetClipboardText(text.c_str());
    log::info(log_prefix + " 已复制 " + std::to_string(text.size()) + " 字符到剪贴板");
}

} // namespace aiwrite::ui
