#include "selection_state.h"

#include <algorithm>
#include <utility>

namespace aiwrite::ui {

bool SelectionState::is_selected(const std::string& node_id) const
{
    return std::find(nodes_.begin(), nodes_.end(), node_id) != nodes_.end();
}

// 统一模式：先快照三组集合 → 修改 → 任一变化才触发回调（每帧调用也安全：无变化 = 无事件）
void SelectionState::set_selection(const std::vector<std::string>& nodes,
                                   const std::vector<std::string>& links,
                                   const std::string& single)
{
    const auto        prev_nodes  = nodes_;
    const auto        prev_links  = links_;
    const std::string prev_single = single_;
    nodes_                        = nodes;
    links_                        = links;
    single_                       = single;
    if (nodes_ != prev_nodes || links_ != prev_links || single_ != prev_single) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::select_node(const std::string& node_id)
{
    const auto        prev_nodes  = nodes_;
    const std::string prev_single = single_;
    nodes_                        = {node_id};
    single_                       = node_id;
    if (nodes_ != prev_nodes || single_ != prev_single) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::set_nodes(const std::vector<std::string>& ids)
{
    const auto prev_nodes = nodes_;
    nodes_                = ids;
    if (nodes_ != prev_nodes) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::set_single(const std::string& id)
{
    const std::string prev_single = single_;
    single_                       = id;
    if (single_ != prev_single) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::remove_node(const std::string& node_id)
{
    const auto        prev_nodes  = nodes_;
    const std::string prev_single = single_;
    nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node_id), nodes_.end());
    if (single_ == node_id) {
        single_.clear();
    }
    if (nodes_ != prev_nodes || single_ != prev_single) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::clear_nodes()
{
    const auto        prev_nodes  = nodes_;
    const std::string prev_single = single_;
    nodes_.clear();
    single_.clear();
    if (nodes_ != prev_nodes || single_ != prev_single) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::clear_links()
{
    const auto prev_links = links_;
    links_.clear();
    if (links_ != prev_links) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::prune_links(std::function<bool(const std::string&)> keep)
{
    const auto prev_links = links_;
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const std::string& eid) { return !keep(eid); }),
                 links_.end());
    if (links_ != prev_links) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::clear()
{
    const auto        prev_nodes  = nodes_;
    const auto        prev_links  = links_;
    const std::string prev_single = single_;
    nodes_.clear();
    links_.clear();
    single_.clear();
    if (nodes_ != prev_nodes || links_ != prev_links || single_ != prev_single) {
        if (changed_callback_) {
            changed_callback_();
        }
    }
}

void SelectionState::set_changed_callback(std::function<void()> callback)
{
    changed_callback_ = std::move(callback);
}

} // namespace aiwrite::ui
