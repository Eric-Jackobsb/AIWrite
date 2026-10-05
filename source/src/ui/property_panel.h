#pragma once

#include "engine/graph.h"

// ============================================================================
//  参数面板（设计文档 §6.3 / §14.5 / M3-06）
//
//  * 选中节点后显示其参数控件：String / Text / Int / Float / Bool / Enum /
//    File / Directory / Color，并给出校验提示（必填/范围/枚举/文件不存在）
//  * is_secret（如 api_key）使用密码框
//  * 编辑结束（控件失焦/回车/松开鼠标）才置 changed，避免拖滑块产生大量撤销快照
// ============================================================================

namespace aiwrite::ui {

struct PropertyEditResult {
    bool begin_edit = false; // 某个控件刚刚被激活（值尚未变化）→ 外层应在此刻压撤销快照
    bool changed    = false; // 参数被修改（编辑结束）
    bool renamed    = false; // 节点标题被改名
};

// node 为 nullptr 时显示"未选中节点"
void draw_property_panel(const char* window_title, bool* open, engine::Node* node,
                         PropertyEditResult& result);

} // namespace aiwrite::ui
