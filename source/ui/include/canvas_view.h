#pragma once

// ============================================================================
//  画布视图共享层（docs/draft/CN_editor_refactor.md · 阶段 1 · CEA-01 / CEA-02）
//
//  主画布（node_canvas.cpp）与上级节点编辑器（composite_editor.cpp）共用的
//  **只读绘制层**：手柄映射 / 文本收口 / 端口绘制 / 节点卡片绘制 / 内容宽度 /
//  结果摘要 / 图片卡片，全部经 CanvasContext 取 graph 与运行读模型 —— 一处实现、
//  两画布各自取用，避免「简化平行副本」再次漂移（CEA-D1）。
//
//  配色（CEA-D5）：CanvasTheme 区分「主画布默认主题」与「上级节点编辑器差异主题」
//  （画布背景 / 节点背景 / 小地图底色等偏蓝），结构与交互两画布保持一致、仅换色。
//  共享绘制代码只从 theme 取色，一处定义、两画布各自取用。
// ============================================================================

#include <functional>
#include <string>
#include <unordered_map>

#include "executor.h" // engine::RunSnapshot
#include "graph.h"

#include <imgui.h>
#include <imgui_node_editor.h>

namespace aiwrite::ui {

// ARCH-A6：纹理缓存是显式对象（EditorController::textures()）——绘制层只拿指针，不取全局
class TextureCache;

namespace ed = ax::NodeEditor;

// ------------------------------------------------ 稳定手柄映射（id ↔ handle）---
// 节点 "n12" → 12；端口 = (节点序号 << 8) | (输出位 0x80) | 端口下标
// 主画布与上级编辑器内部图（同为 "n1" 风格 id）共用同一套整数手柄方案。
ed::NodeId nodeHandle(const std::string& node_id);
ed::LinkId linkHandle(const std::string& edge_id);
ed::PinId  pinHandle(const engine::Node& node, engine::PortDirection direction, int index);

struct PinRef {
    std::string           node_id;
    std::string           port_id;
    engine::PortDirection direction = engine::PortDirection::Input;
    bool                  valid     = false;
};

PinRef resolvePin(ed::PinId id, const engine::Graph& graph);

// -------------------------------------------------------------- 布局常量 -----
// 节点宽度由「输入/输出标签实测最宽」决定、全画布所有节点共用同一个值（update_content_width），
// 宽度 = max(所有节点里最宽的一行端口标签, 下限 kNodeContentWidthMin)。
constexpr float kNodeContentWidthMin = 160.0f; // 内容区下限（保证标题行 / 参数预览可读）
constexpr float kPinRadius           = 5.0f;
constexpr float kPinDotSize          = kPinRadius * 2.0f;
constexpr float kPinTextGap          = 6.0f;
constexpr float kNodeTitleIndent     = 15.0f; // 标题行左侧占位（分类色条 + 状态圆点）
constexpr float kLabelGap            = 4.0f;  // 同一行内文本片段之间的间隙
constexpr int   kNodeSummaryLines    = 6;     // 节点卡片上多行文本（运行结果 / 必失败原因）最多几行

// ------------------------------------------------------------ 文本收口（像素）---
// 只裁剪、**不加**省略号：返回不超过 max_width 像素的最长前缀（不切断 UTF-8 多字节序列）。
std::string clipToWidth(const std::string& text, float max_width);
// 单行省略 = clip 到「max_width − 省略号宽度」再补 …（结果恒 ≤ max_width）。
std::string elideToWidth(const std::string& text, float max_width);
// 自绘折行（不依赖 ImGui 自动换行；中文也可任意字符断行）；每行 ≤ max_width，
// 超过 max_lines 行或字符预算时在末行补 …。
std::string wrapToWidth(const std::string& text, float max_width, int max_lines);

// 一行端口标签占用的宽度（左列 = [圆点][间隙][名称]，右列同宽）
float portRowWidth(const engine::Port& port);

// ------------------------------------------------------------ 主题（CEA-D5）---
struct CanvasTheme {
    // imgui-node-editor 画布 / 节点配色（StyleColor_*）
    ImU32 bg               = IM_COL32(30, 30, 30, 255);
    ImU32 grid             = IM_COL32(42, 42, 42, 255);
    ImU32 node_bg          = IM_COL32(45, 45, 45, 255);
    ImU32 node_border      = IM_COL32(58, 58, 58, 255);
    ImU32 sel_node_border  = IM_COL32(74, 144, 226, 255);
    ImU32 hov_node_border  = IM_COL32(74, 144, 226, 200);
    // 上级节点紫条（仅主画布：composite_id 非空的节点用紫条区分；内部图无嵌套 → 关闭）
    bool  composite_stripe_enabled = false;
    ImU32 composite_stripe         = IM_COL32(150, 110, 220, 255);
    // 端口圆点描边
    ImU32 pin_outline = IM_COL32(20, 20, 20, 255);
    // 小地图（与所在画布一致观感）
    ImU32 minimap_bg     = IM_COL32(18, 20, 26, 215);
    ImU32 minimap_border = IM_COL32(130, 130, 145, 210);
    ImU32 minimap_title  = IM_COL32(200, 200, 210, 235);
    ImU32 minimap_close  = IM_COL32(225, 90, 90, 235);
    ImU32 minimap_empty  = IM_COL32(120, 120, 135, 200);
    // 对齐辅助线（紫色实线，仅拖动中显示）
    ImU32 snap_line = IM_COL32(0x9C, 0x6C, 0xFF, 230);
};

// 主画布默认主题（现状配色，零行为变更）
const CanvasTheme& canvasThemeMain();
// 上级节点编辑器差异主题（CEA-D5：画布背景 / 节点背景 / 小地图偏蓝，便于一眼区分）
const CanvasTheme& canvasThemeComposite();

// 应用主题到「当前编辑器上下文」（调用前须 ed::SetCurrentEditor；样式按上下文隔离）
void canvasApplyEditorStyle(const CanvasTheme& theme);

// ------------------------------------------------------------- 画布上下文 -----
struct CanvasContext {
    const engine::Graph* graph = nullptr;
    const CanvasTheme*   theme = nullptr;                 // nullptr → 主画布默认主题
    float                content_width = kNodeContentWidthMin; // 每帧由 update_content_width 重算
    // 运行读模型（PB-01 只读快照；未设置时回退空快照）
    std::function<const engine::RunSnapshot&()> run_snapshot;

    // ARCH-A6：图片缩略图用的纹理缓存（nullptr = 未接线 → 卡片不显示图片，绝不崩）
    TextureCache* textures = nullptr;

    // ---- 内联文本编辑（仅上级节点编辑器开启；主画布保持只读预览不变）----
    // 开启后：TextInput 节点的「文本内容」参数在节点卡片上直接渲染为可编辑多行输入框
    // （官方 widgets-example 同款：节点内直接放 InputText；本库编辑器快捷键由调用方关掉）。
    bool inline_edit_text = false;
    // 编辑开始（值尚未变化）→ 外层压撤销快照（保证快照是修改前的状态）
    std::function<void()> on_param_edit_begin;
    // 值变化即写回（P1-c 语义）：node_id / param_id / 编辑后的完整文本
    std::function<void(const std::string& node_id, const std::string& param_id,
                       const std::string& value)> on_param_edit_commit;
};

// 按当前图重算「全画布共用的内容区宽度」：取所有节点里**最宽的一行端口标签**
// （下限 kNodeContentWidthMin）。每帧重算 → 换工作流 / 增删节点即时生效。
void updateContentWidth(const engine::Graph& graph, CanvasContext& ctx);

// --------------------------------------------------- 运行结果摘要（PA-03）----
struct NodeResultView {
    std::string text;
    bool        has_content = false;
    bool        is_error    = false;
    bool        is_skipped  = false;
};
// 节点内只读展示运行结果（与输出面板 / 参数面板「运行结果」同源 ui::node_output_text；
// 用「运行信息条数 + 状态 + 错误长度 + 输出字节数」轻量签名缓存，避免每帧重算长文本）
NodeResultView resultViewOf(const CanvasContext& ctx, const engine::Node& node);

// 节点卡片要显示的图片（运行结果优先，其次「图片输入」节点的参数路径解析）
std::string nodeCardImage(const CanvasContext& ctx, const engine::Node& node);

// 节点内条件分隔线：clamp 到节点内容区宽度（ctx.content_width），不超出节点边框。
// 不能直接用 ImGui::Separator() —— 节点内容是「画布窗口里的一个 Group」，Separator 会
// 按**画布窗口**的 Pos/Size 横跨整个画布宽度（视觉上就是「超出节点框的长灰线」）。
void drawNodeSeparator(const CanvasContext& ctx);

// 端口行绘制（圆点 + 名称；输入贴左边缘 / 输出贴右边缘，颜色按端口类型，可变长端口多一圈描边）
void draw_pin(const CanvasContext& ctx, const engine::Node& node, const engine::Port& port,
              int index);

// 节点卡片整体绘制（只读绘制层；阶段 1 起两画布共用同一实现）
void drawNodeBody(const CanvasContext& ctx, const engine::Node& node);

// ------------------------------------------------------- 小地图（CLN-09）----
// 小地图输入屏蔽快照（鼠标进入小地图区域 → 事件只归小地图，画布不得参与）
struct MinimapState {
    bool   blocking = false; // 本帧鼠标位于小地图区域（已对 ed 屏蔽输入）
    bool   clicked  = false; // 屏蔽前快照：左键本帧点击
    bool   down     = false; // 屏蔽前快照：左键按住中
    ImVec2 mouse    = ImVec2(0.0f, 0.0f); // 屏蔽前快照：鼠标屏幕坐标
};

// 小地图矩形（左下角 190×130，margin 10；与 draw_minimap 共用同一套几何，单一来源）
void minimapRect(ImVec2& out_min, ImVec2& out_max);
bool minimapHit(const ImVec2& mouse, const ImVec2& min, const ImVec2& max);

// 在 ed::Begin 之前调用：小地图区域的鼠标事件只属于小地图，画布不得参与。
// show = 小地图开关（点击 ✕ 关闭时回写 false）；state = 本 TU 的输入屏蔽快照。
void blockMinimapInput(bool& show, MinimapState& state);

// 小地图（左下角缩略图；点击 / 按住拖动 → 视口中心平移到对应画布位置；右上角 ✕ 关闭）
// 纯导航：绝不选中 / 移动 / 框选节点 —— 输入已在 block_minimap_input 中与画布彻底隔离。
// 调用时机（CEA-D2）：在 ed::End() 之后、ed::SetCurrentEditor(nullptr) 之前调用。
// node_color：小地图节点块着色（主画布=按运行状态着色 / 上级编辑器=固定蓝紫色）。
void draw_minimap(const engine::Graph& graph, const CanvasTheme& theme, bool& show,
                  const MinimapState& state,
                  const std::function<ImU32(const engine::Node&)>& node_color);

// ---------------------------------------------------- 对齐吸附（CLN-10）----
// 对齐吸附结果：applied=本帧执行了吸附；line_v/line_h=辅助线（屏幕坐标，仅拖动中显示）
struct SnapResult {
    bool   applied = false;
    bool   line_v  = false;
    bool   line_h  = false;
    ImVec2 v_a = ImVec2(0.0f, 0.0f);
    ImVec2 v_b = ImVec2(0.0f, 0.0f);
    ImVec2 h_a = ImVec2(0.0f, 0.0f);
    ImVec2 h_b = ImVec2(0.0f, 0.0f);
};

// 拖拽中的节点 id：左键按住 + 位置相对上一帧移动 → 判定为拖动中。
// suppress = Alt 平移互斥（主画布传 g_alt_pan_active；上级编辑器无此概念传 false）。
// prev_positions = 上一帧节点位置表（本 TU 自持，每帧末尾全量更新）。
std::string find_dragged_node(const engine::Graph& graph, bool suppress,
                              std::unordered_map<std::string, ImVec2>& prev_positions);

// 对齐核心：把拖动中的节点吸附到其它节点的左/右/中心X、上/下/中心Y（三边对）。
// 命中即写回节点位置并产出辅助线端点（屏幕坐标）。不含开关判断 / 网格吸附 / 状态更新。
SnapResult apply_alignment_snap_core(const engine::Graph& graph, const std::string& dragged,
                                     const ImVec2& dpos, float dw, float dh, float snap_canvas);

// 对齐辅助线（仅拖动中显示；颜色 / 线宽由调用方传入以保留两画布差异）
void drawSnapLines(const SnapResult& snap, ImDrawList* draw_list, ImU32 color, float width);

} // namespace aiwrite::ui
