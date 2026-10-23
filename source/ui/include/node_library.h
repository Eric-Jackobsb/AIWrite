#pragma once

#include <string>

namespace aiwrite::ui {

class EditorController;

// ---------------------------------------------------------------------------
// ARCH-A6：节点库的跨帧状态（原 `node_library.cpp` 6 条文件级 static）
//  * 归属 = `EditorController::node_library()`；只放「右键菜单 / 二次确认」的临时状态
//  * 生命周期：点击右键 → 置 pending + *_open；弹窗消费后清空（守 R8：不静默删 / 覆盖）
// ---------------------------------------------------------------------------
struct NodeLibraryState {
    std::string context_id;        // 右键命中的上级节点 id
    bool        context_open = false;   // 右键命中后待打开菜单（须在 PushID 作用域外 OpenPopup）
    std::string delete_pending;         // 待删除确认的 id
    bool        delete_confirm_open = false;
    std::string copy_pending;           // 待复制确认的 id
    bool        copy_confirm_open = false;
};

// 节点库（设计 §7.2 / M3-09）：按分类列出 8 个节点，点击即在画布中心添加
// ARCH-A3：状态栏 / 节点创建走启动注入的 EditorController（不再有全局编辑器单例）
void drawNodeLibrary(const char* title, bool* open, EditorController& editor);

} // namespace aiwrite::ui
