#include "ce_session.h"

#include <algorithm>
#include <utility>

namespace aiwrite::ui {

// ============================================================== CeModel ====
// 文档级规格工作副本 + 撤销栈（对称 WorkflowModel：Graph + Undo · ARCH-Z2）。
// 撤销粒度 = 整份 CompositeSpec（含对外输入/输出/暴露参数），深度 30
//（engine::UndoStack 固定绑 Graph、深度 50，语义不同，故本对象自带同角色小栈）。
namespace {
constexpr std::size_t kCeUndoDepth = 30; // 会话内撤销深度（原 ce_state.cpp 常量，随实现迁移）
} // namespace

void CeModel::pushSnapshot(const std::string& label)
{
    Entry entry;
    entry.spec  = spec_;
    entry.label = label;
    undo_.push_back(std::move(entry));
    if (undo_.size() > kCeUndoDepth) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    dirty_ = true;
}

bool CeModel::undoOnce()
{
    if (undo_.empty()) {
        return false;
    }
    Entry entry = std::move(undo_.back());
    undo_.pop_back();

    Entry current;
    current.spec  = spec_;
    current.label = entry.label;
    redo_.push_back(std::move(current));

    spec_ = std::move(entry.spec);
    return true;
}

bool CeModel::redoOnce()
{
    if (redo_.empty()) {
        return false;
    }
    Entry entry = std::move(redo_.back());
    redo_.pop_back();

    Entry current;
    current.spec  = spec_;
    current.label = entry.label;
    undo_.push_back(std::move(current));

    spec_ = std::move(entry.spec);
    return true;
}

void CeModel::clearHistory()
{
    undo_.clear();
    redo_.clear();
}

void CeModel::load(engine::CompositeSpec spec)
{
    spec_ = std::move(spec);
}

} // namespace aiwrite::ui
