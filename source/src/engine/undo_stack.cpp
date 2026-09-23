#include "engine/undo_stack.h"

#include <utility>

namespace aiwrite::engine {

void UndoStack::clear()
{
    undo_.clear();
    redo_.clear();
}

void UndoStack::trim()
{
    while (undo_.size() > kMaxDepth) {
        undo_.erase(undo_.begin());
    }
}

void UndoStack::push(const Graph& snapshot, const std::string& label)
{
    undo_.push_back(Entry{snapshot, label});
    trim();
    redo_.clear(); // 新操作使重做分支失效
}

bool UndoStack::undo(const Graph& current, Graph& restored, std::string* label)
{
    if (undo_.empty()) {
        return false;
    }

    Entry entry = std::move(undo_.back());
    undo_.pop_back();

    const std::string action = entry.label;
    redo_.push_back(Entry{current, action});
    restored = std::move(entry.graph);

    if (label != nullptr) {
        *label = action;
    }
    return true;
}

bool UndoStack::redo(const Graph& current, Graph& restored, std::string* label)
{
    if (redo_.empty()) {
        return false;
    }

    Entry entry = std::move(redo_.back());
    redo_.pop_back();

    const std::string action = entry.label;
    undo_.push_back(Entry{current, action});
    trim();
    restored = std::move(entry.graph);

    if (label != nullptr) {
        *label = action;
    }
    return true;
}

std::string UndoStack::lastUndoLabel() const
{
    return undo_.empty() ? std::string() : undo_.back().label;
}

std::string UndoStack::lastRedoLabel() const
{
    return redo_.empty() ? std::string() : redo_.back().label;
}

} // namespace aiwrite::engine
