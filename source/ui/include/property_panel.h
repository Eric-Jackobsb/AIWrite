#pragma once

#include <string>
#include <vector>

#include "graph.h"

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

// 单个参数控件（PS-09-G1 方案乙：上级节点编辑器复用；返回 edited = 本次编辑结束）
//  * begin_edit = 控件刚被激活（值尚未变化）→ 外层在此刻压撤销快照
//  * changed_now = 值已写回模型（编辑过程中每帧都可能为 true）
//  * enum_override：可选 —— 覆盖枚举参数的候选列表（None/为空数组时用参数自带 enum_options）
bool draw_param_widget(engine::Node& node, engine::Param& param, bool& begin_edit,
                       bool& changed_now, const std::vector<std::string>* enum_override = nullptr);

} // namespace aiwrite::ui
