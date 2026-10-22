#pragma once

// ============================================================================
//  画布交互共享层（ARCH-E5 · 双画布单套交互）
//
//  主画布（node_canvas.cpp）与上级节点编辑器（ce_draw.cpp）共用同一套**交互层**
//  实现：连线创建 / 拖出删除 / 选中删除 / 选中同步 / 位置同步 / 快捷键窗口判定
//  —— 一处实现、两画布各自取用（与 canvas_view 只读绘制层对应）。
//
//  模型差异经「回调 + 参数」保留（ARCHITECTURE §5 风险对策：ce_draw 与
//  node_canvas 交互差异比预想大 —— 主画布 = editor().model().graph +
//  SelectionState（含替换连线语义）；ce = compositeEditorState().working.
//  internal_graph + 局部单选，无替换）。
//
//  imgui-node-editor 的交互 API 谓词（GetSelected* / BeginCreate / BeginDelete /
//  QueryNewLink / AcceptNewItem / RejectNewItem / QueryDeleted* / AcceptDeletedItem /
//  ClearSelection …）**只允许出现在本文件**（ARCHITECTURE §4 验收判据 3：
//  关键谓词只出现在 interactor.cpp —— `rg` 验证）。
// ============================================================================

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_node_editor.h>

#include "canvas_view.h" // PinRef / nodeHandle / resolvePin（手柄映射共享层）

namespace aiwrite::ui {

namespace ed = ax::NodeEditor;

// 位置安全上限：任何异常数值（FLT_MAX / NaN / 溢出）都不得进入绘图与视图计算
//（历史「复制粘贴卡死」根因 —— 主画布原 kPositionLimit 迁入共享层）
inline constexpr float kCanvasPositionLimit = 100000.0f;

// ------------------------------------------------------ 交互回调组 --------
// 两画布的模型差异（图源 / 快照归属 / 选中落点 / 提示）经回调注入。
struct CanvasInteract {
    // ---- 连线创建（差异：resolvePin 的图源 / canConnect 替换语义 / 写模型 / 提示）----
    // 解析两端 pin；返回 true = 两端都有效（统一成「输出 → 输入」由交互层内部完成）
    std::function<bool(ed::PinId start_pin, ed::PinId end_pin, PinRef& from, PinRef& to)> resolve_pin;
    // 校验可连接；返回 false → reason 按拒绝处理（主画布支持替换 would_replace；ce 无替换）
    std::function<bool(const PinRef& from, const PinRef& to, std::string& reason,
                       bool& would_replace)> can_connect;
    // 接受连线（写模型 + 压快照，画布自处理；would_replace = 是否替换原连线）
    std::function<void(const PinRef& from, const PinRef& to, bool would_replace)> on_link_accepted;
    // 拒绝提示（画布自定落点：主画布 log + status / ce error）
    std::function<void(const std::string& reason)> on_link_rejected;
    // 拖拽连线时的鼠标旁提示（主画布 M_node_expansion L3-09；ce 无 → 空）
    std::function<void(ed::PinId start_pin, ed::PinId end_pin)> on_create_tip;

    // ---- 删除（差异：模型 / 快照归属）----
    // 删除批次开始（interactorDeleteOuter 对每个对象调用一次，label 由调用方按画布语义传入）
    std::function<void(const std::string& label)> on_delete_batch;
    std::function<void(const std::string& link_id)> delete_link;
    std::function<void(const std::string& node_id)> delete_node;

    // ---- 选中同步（差异：主画布 SelectionState（含 single 保留）/ ce 局部单选）----
    std::function<void(const std::vector<std::string>& node_ids,
                       const std::vector<std::string>& link_ids)> on_selection;
};

// 读 ed 当前选中 → 节点/连线 id 列表（含 "n"/"e" 前缀）—— ed 选中 API 唯一封装
struct SelectionRead {
    std::vector<std::string> node_ids;
    std::vector<std::string> link_ids;
};
SelectionRead interactorReadSelection();

// 清 ed 内部选中（删除后 / 右键删除节点后）
void interactorClearSelection();

// 连线创建：BeginCreate..EndCreate 无条件配对契约内置（v0.9.3 m_InActive 断言）
void interactorCreateLink(const CanvasInteract& interact);

// 拖出删除（把节点/连线拖出画布）：BeginDelete..EndDelete 契约内置；每个对象
// on_delete_batch(label) 一次（label 由调用方按画布语义传入）
void interactorDeleteOuter(const CanvasInteract& interact, const std::string& link_label,
                           const std::string& node_label);

// 选中删除（ce Delete 键 / 工具栏「删除选中」）：batch_snapshot 压一次撤销快照
//（可为空）；逐项 delete_node / delete_link → 清 ed 选中
void interactorDeleteSelection(const CanvasInteract& interact, const SelectionRead& selection,
                               const std::function<void()>& batch_snapshot);

// 选中同步：读 ed 选中 → on_selection 回调（每帧在 ed::Begin..End 内调用）
void interactorSyncSelection(const CanvasInteract& interact);

// 模型 → 编辑器：推节点位置（新建/粘贴落点；ed::Begin 后每帧调用，幂等）
void interactorPushNodePosition(const std::string& node_id, float x, float y);

// 位置读回原语（编辑器 → 模型）：
//   * editor_knows = 编辑器是否认识该节点；不认识 → 内部把模型坐标推给编辑器（绝不回写脏值）
//   * clamp_limit > 0 → 夹紧到 ±clamp_limit（主画布 kCanvasPositionLimit；ce 传 0 保持现状）
//   * changed = 读回坐标与模型差超过 epsilon（主画布 0.5px 容差；ce 传 0 = 任何变化）
struct PositionRead {
    bool  editor_knows = false;
    bool  changed      = false;
    float x            = 0.0f;
    float y            = 0.0f;
};
PositionRead interactorReadNodePosition(const std::string& node_id, float model_x, float model_y,
                                        float clamp_limit, float epsilon);

// 快捷键窗口判定（ARCH-R4：两画布窗口条件收敛）
// 主画布：画布窗口聚焦（子窗口）或悬停（历史 Delete 键条件）
bool interactorWindowActiveMain();
// ce：根窗口（上级节点编辑器主窗口）聚焦/悬停（DEBT-B1 修复语义）
bool interactorWindowActiveComposite();

} // namespace aiwrite::ui
