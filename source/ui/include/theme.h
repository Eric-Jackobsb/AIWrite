#pragma once

// ============================================================================
//  UI 配色（设计文档 §14.3 分类配色 / §4.5 端口类型 / §4.6 节点状态 / §14.4 背景）
//  供画布与节点库共用
// ============================================================================

#include <imgui.h>

#include "engine/graph.h"

namespace aiwrite::ui {

inline ImU32 category_color(engine::NodeCategory category)
{
    switch (category) {
    case engine::NodeCategory::Input:     return IM_COL32(0x4A, 0x9E, 0x8F, 255); // 青绿
    case engine::NodeCategory::Process:   return IM_COL32(0x5A, 0x7A, 0x9E, 255); // 蓝灰
    case engine::NodeCategory::Config:    return IM_COL32(0x7A, 0x5A, 0x9E, 255); // 紫
    case engine::NodeCategory::Inference: return IM_COL32(0x3A, 0x6E, 0xA5, 255); // 深蓝
    case engine::NodeCategory::Output:    return IM_COL32(0xC4, 0x7A, 0x3A, 255); // 橙
    }
    return IM_COL32(0x4A, 0x9E, 0x8F, 255);
}

inline ImVec4 category_color_vec4(engine::NodeCategory category)
{
    const ImU32 color = category_color(category);
    return ImGui::ColorConvertU32ToFloat4(color);
}

inline ImU32 port_color(engine::PortType type)
{
    switch (type) {
    case engine::PortType::Text:     return IM_COL32(0xE0, 0xE0, 0xE0, 255); // 白
    case engine::PortType::Image:    return IM_COL32(0x64, 0xC8, 0x64, 255); // 绿
    case engine::PortType::Provider: return IM_COL32(0x64, 0x96, 0xFF, 255); // 蓝
    case engine::PortType::Number:   return IM_COL32(0xB4, 0xB4, 0xB4, 255); // 灰
    case engine::PortType::Any:      return IM_COL32(0xC8, 0x96, 0xFF, 255); // 紫
    }
    return IM_COL32(0xE0, 0xE0, 0xE0, 255);
}

inline ImU32 state_color(engine::NodeState state)
{
    switch (state) {
    case engine::NodeState::Idle:    return IM_COL32(0x66, 0x66, 0x66, 255);
    case engine::NodeState::Waiting: return IM_COL32(0xD4, 0xB8, 0x30, 255);
    case engine::NodeState::Running: return IM_COL32(0x4A, 0x90, 0xE2, 255);
    case engine::NodeState::Done:    return IM_COL32(0x50, 0xB0, 0x50, 255);
    case engine::NodeState::Error:   return IM_COL32(0xE2, 0x50, 0x50, 255);
    case engine::NodeState::Skipped: return IM_COL32(0x4A, 0x4A, 0x4A, 255);
    }
    return IM_COL32(0x66, 0x66, 0x66, 255);
}

} // namespace aiwrite::ui
