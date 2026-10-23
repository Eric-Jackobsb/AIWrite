#include "composite_editor.h"
#include "ce_internal.h"
#include "ce_session.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::ui {

// ======================================================= 画布域状态容器 ====
// ARCH-A5：原 12 条跨 TU extern（g_ce_*）收进本对象；ARCH-Z2：撤销栈已迁至 CeModel
//（对称 WorkflowModel.undo），本对象只剩**画布域**状态。
// 规则 6：跨帧可变状态必须是对象成员 —— 这里的函数级 static 就是承载它的对象；
// 命名空间作用域不再有可变变量。
CeUiState& ceUiState()
{
    static CeUiState state;
    return state;
}

// ------------------------------------------------------------ 撤销域 ----
// 快照栈本体在 CeModel（ce_model.cpp）；本文件只保留跨 TU / 对外 API 的薄转发。
void cePushSnapshot(const std::string& label)
{
    ceSession().snapshot(label);
}

// 编辑器内撤销/重做（窗口按钮 / 命令路由层调用；error 随撤销清空，与原语义一致）
bool compositeEditorCanUndo()
{
    return ceSession().model().canUndo();
}

bool compositeEditorCanRedo()
{
    return ceSession().model().canRedo();
}

void compositeEditorUndoOnce()
{
    CeSession& session = ceSession();
    if (session.model().undoOnce()) {
        session.error.clear();
    }
}

void compositeEditorRedoOnce()
{
    CeSession& session = ceSession();
    if (session.model().redoOnce()) {
        session.error.clear();
    }
}

} // namespace aiwrite::ui
