#pragma once

// ============================================================================
//  快照式撤销/重做（设计文档 §6.6 / T-14）
//
//  * 快照方案：每次修改前把整张 Graph 压入撤销栈
//  * 栈深上限 50；产生新操作时丢弃重做分支；加载/新建工作流时清空
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "engine/graph.h"

namespace aiwrite::engine {

class UndoStack {
public:
    static constexpr std::size_t kMaxDepth = 50; // 设计 §6.6

    void clear();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // 在修改 Graph **之前** 调用：记录当前状态与操作名称
    void push(const Graph& snapshot, const std::string& label);

    // 撤销/重做：当前状态参与交换，恢复结果写入 restored
    bool undo(const Graph& current, Graph& restored, std::string* label = nullptr);
    bool redo(const Graph& current, Graph& restored, std::string* label = nullptr);

    std::size_t undoDepth() const { return undo_.size(); }
    std::size_t redoDepth() const { return redo_.size(); }

    std::string lastUndoLabel() const;
    std::string lastRedoLabel() const;

private:
    struct Entry {
        Graph       graph;
        std::string label;
    };

    std::vector<Entry> undo_;
    std::vector<Entry> redo_;

    void trim();
};

} // namespace aiwrite::engine
