#include "ce_session.h"

#include <algorithm>

namespace aiwrite::ui {

// ========================================================== CeSelection ====
// 文档级内部图选中（对称 SelectionState · ARCH-Z2）。
// ce 画布为「局部单选」语义：只有恰好选中一个节点时 single() 才非空
//（右侧「参数编辑」区据此显示该节点参数）；多选 / 无选 → 空。
// 每帧由 ce_draw 的 ce_interact.on_selection 同步，内置变化检测（无变化不写）。
bool CeSelection::is_selected(const std::string& node_id) const
{
    return std::find(nodes_.begin(), nodes_.end(), node_id) != nodes_.end();
}

void CeSelection::sync(const std::vector<std::string>& node_ids)
{
    const std::string single = (node_ids.size() == 1) ? node_ids.front() : std::string();
    if (nodes_ == node_ids && single_ == single) {
        return; // 无变化：不写（每帧调用安全）
    }
    nodes_  = node_ids;
    single_ = single;
}

void CeSelection::clear()
{
    nodes_.clear();
    single_.clear();
}

} // namespace aiwrite::ui
