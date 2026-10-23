#include "canvas_view.h"

#include "asset_store.h"   // 图片卡片：资源令牌解析
#include "config.h"        // ui.running_animation（运行中脉冲动效）
#include "node_registry.h"
#include "output_panel.h"  // node_output_text（结果摘要与面板同源）
#include "paths.h"
#include "provider_resolve.h" // 生效提供商提示
#include "texture_cache.h"    // 图片卡片缩略图
#include "theme.h"            // category_color / port_color / state_color

#include <imgui_stdlib.h>     // 内联文本编辑：InputTextMultiline(std::string*)

#include <algorithm>
#include <cfloat> // FLT_MAX（小地图包围盒初始化）
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::ui {

// --------------------------------------------------------------- 手柄映射 ----
ed::NodeId nodeHandle(const std::string& node_id)
{
    return ed::NodeId(static_cast<uintptr_t>(engine::numericIdOf(node_id)));
}

ed::LinkId linkHandle(const std::string& edge_id)
{
    return ed::LinkId(static_cast<uintptr_t>(engine::numericIdOf(edge_id)));
}

ed::PinId pinHandle(const engine::Node& node, engine::PortDirection direction, int index)
{
    const uintptr_t base  = static_cast<uintptr_t>(engine::numericIdOf(node.id)) << 8;
    const uintptr_t flags = (direction == engine::PortDirection::Output) ? 0x80u : 0x00u;
    return ed::PinId(base | flags | static_cast<uintptr_t>(index & 0x7F));
}

PinRef resolvePin(ed::PinId id, const engine::Graph& graph)
{
    PinRef ref;
    const uintptr_t value = id.Get();
    if (value == 0) {
        return ref;
    }
    const int node_seq = static_cast<int>((value >> 8) & 0x7FFFFF);
    const engine::PortDirection direction =
        ((value & 0x80u) != 0) ? engine::PortDirection::Output : engine::PortDirection::Input;
    const int index = static_cast<int>(value & 0x7F);

    const engine::Node* node = graph.findNode("n" + std::to_string(node_seq));
    if (node == nullptr) {
        return ref;
    }
    const std::vector<engine::Port>& ports =
        (direction == engine::PortDirection::Input) ? node->inputs : node->outputs;
    if (index < 0 || index >= static_cast<int>(ports.size())) {
        return ref;
    }

    ref.node_id   = node->id;
    ref.port_id   = ports[static_cast<std::size_t>(index)].id;
    ref.direction = direction;
    ref.valid     = true;
    return ref;
}

// ------------------------------------------------------------- 文本收口（像素）---
std::string clipToWidth(const std::string& text, float max_width)
{
    if (text.empty()) {
        return std::string();
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }
    std::string out = text;
    while (!out.empty()) {
        std::size_t cut = out.size() - 1; // 回退一个 UTF-8 字符（绝不切断多字节序列）
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        out.erase(cut);
        if (ImGui::CalcTextSize(out.c_str()).x <= max_width) {
            return out;
        }
    }
    return std::string();
}

std::string elideToWidth(const std::string& text, float max_width)
{
    const std::string ellipsis   = "…";
    const float       ellipsis_w = ImGui::CalcTextSize(ellipsis.c_str()).x;
    if (max_width <= ellipsis_w) {
        return ellipsis; // 连省略号都放不下
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text; // 放得下 → 原样返回（零开销路径）
    }
    return clipToWidth(text, max_width - ellipsis_w) + ellipsis;
}

std::string wrapToWidth(const std::string& text, float max_width, int max_lines)
{
    constexpr std::size_t kCharBudget = 2000; // 性能兜底：超长正文只折前 2000 字节
    const std::string     source = (text.size() > kCharBudget) ? text.substr(0, kCharBudget) : text;
    const bool            budget_cut = source.size() != text.size();

    std::vector<std::string> lines;
    std::string              line;
    std::size_t              i = 0;
    while (i < source.size()) {
        std::size_t next = i + 1; // 取一个完整 UTF-8 字符（绝不切断多字节序列）
        while (next < source.size() &&
               (static_cast<unsigned char>(source[next]) & 0xC0u) == 0x80u) {
            ++next;
        }
        const std::string ch = source.substr(i, next - i);
        i = next;

        if (ch == "\n") { // 原文自带换行 → 直接断行
            lines.push_back(line);
            line.clear();
            continue;
        }
        if (!line.empty() && ImGui::CalcTextSize((line + ch).c_str()).x > max_width) {
            lines.push_back(line); // 放不下 → 在此字符前断行
            line = ch;
            continue;
        }
        line += ch;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }

    const bool line_cut = max_lines > 0 && static_cast<int>(lines.size()) > max_lines;
    if (line_cut) {
        lines.resize(static_cast<std::size_t>(max_lines));
    }

    // 收口保证①：任何一行都必须 ≤ max_width（修掉「单字符就超宽」「删行后末行过长」两种情形）
    for (std::string& item : lines) {
        if (ImGui::CalcTextSize(item.c_str()).x > max_width) {
            item = clipToWidth(item, max_width);
        }
    }

    // 收口保证②：要补 … 时，先给末行让出「省略号的宽度」（否则末行会超宽）
    const bool mark = line_cut || budget_cut;
    if (mark) {
        const float marker_w = ImGui::CalcTextSize("…").x;
        if (lines.empty()) {
            lines.emplace_back();
        }
        lines.back() = clipToWidth(lines.back(), max_width - marker_w);
    }

    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out += "\n";
        }
        out += lines[index];
    }
    if (mark) {
        out += "…";
    }
    return out;
}

float portRowWidth(const engine::Port& port)
{
    return kPinDotSize + kPinTextGap + ImGui::CalcTextSize(port.display_name.c_str()).x;
}

// ---------------------------------------------------------------- 主题（CEA-D5）---
namespace {

CanvasTheme make_main_theme()
{
    CanvasTheme t;
    t.bg                       = IM_COL32(30, 30, 30, 255);
    t.grid                     = IM_COL32(42, 42, 42, 255);
    t.node_bg                  = IM_COL32(45, 45, 45, 255);
    t.node_border              = IM_COL32(58, 58, 58, 255);
    t.sel_node_border          = IM_COL32(74, 144, 226, 255);
    t.hov_node_border          = IM_COL32(74, 144, 226, 200);
    t.composite_stripe_enabled = true;
    t.composite_stripe         = IM_COL32(150, 110, 220, 255);
    t.pin_outline              = IM_COL32(20, 20, 20, 255);
    t.minimap_bg               = IM_COL32(18, 20, 26, 215);
    t.minimap_border           = IM_COL32(130, 130, 145, 210);
    t.minimap_title            = IM_COL32(200, 200, 210, 235);
    t.minimap_close            = IM_COL32(225, 90, 90, 235);
    t.minimap_empty            = IM_COL32(120, 120, 135, 200);
    t.snap_line                = IM_COL32(0x9C, 0x6C, 0xFF, 230);
    return t;
}

CanvasTheme make_composite_theme()
{
    // CEA-D5：上级节点编辑器画布「差异主题」—— 仅换色、结构与交互与主画布一致。
    // 整体偏蓝（画布背景 / 网格 / 节点底 / 边框 / 小地图），与主画布中性灰一眼可辨；
    // 端口圆点 / 连线 / 状态色等「交互相关」颜色保持与主画布一致（连线体验对齐优先）。
    CanvasTheme t = make_main_theme();
    t.bg                 = IM_COL32(24, 27, 36, 255);   // 深蓝黑画布背景
    t.grid               = IM_COL32(36, 40, 52, 255);   // 蓝灰网格
    t.node_bg            = IM_COL32(41, 46, 60, 255);   // 蓝灰节点底
    t.node_border        = IM_COL32(70, 78, 100, 255);  // 偏蓝边框
    t.composite_stripe_enabled = false;                 // 内部图无嵌套 → 不画紫条，统一分类色
    t.minimap_bg         = IM_COL32(13, 15, 21, 220);   // 更深差异底
    t.minimap_border     = IM_COL32(92, 102, 132, 215); // 偏蓝边框
    t.minimap_title      = IM_COL32(206, 212, 232, 235);
    t.minimap_empty      = IM_COL32(110, 120, 145, 200);
    return t;
}

} // namespace

const CanvasTheme& canvasThemeMain()
{
    static const CanvasTheme theme = make_main_theme();
    return theme;
}

const CanvasTheme& canvasThemeComposite()
{
    static const CanvasTheme theme = make_composite_theme();
    return theme;
}

void canvasApplyEditorStyle(const CanvasTheme& theme)
{
    ed::Style& style              = ed::GetStyle();
    style.NodeRounding            = 4.0f;
    style.NodeBorderWidth         = 1.5f;
    style.SelectedNodeBorderWidth = 2.5f;
    style.NodePadding             = ImVec4(10.0f, 8.0f, 10.0f, 8.0f);
    style.Colors[ed::StyleColor_Bg]            = ImColor(theme.bg);
    style.Colors[ed::StyleColor_Grid]          = ImColor(theme.grid);
    style.Colors[ed::StyleColor_NodeBg]        = ImColor(theme.node_bg);
    style.Colors[ed::StyleColor_NodeBorder]    = ImColor(theme.node_border);
    style.Colors[ed::StyleColor_SelNodeBorder] = ImColor(theme.sel_node_border);
    style.Colors[ed::StyleColor_HovNodeBorder] = ImColor(theme.hov_node_border);
}

// ------------------------------------------------------------- 画布上下文 -----
namespace {

// ARCH-Z1：`effective_theme` 随渲染段（draw_pin / drawNodeBody）搬入 canvas_view_render.cpp，
// 本 TU 只留 `empty_snapshot`（结果摘要缓存用）。
const engine::RunSnapshot& empty_snapshot()
{
    static const engine::RunSnapshot empty;
    return empty;
}
} // namespace

void updateContentWidth(const engine::Graph& graph, CanvasContext& ctx)
{
    float width = kNodeContentWidthMin;
    for (const engine::Node& node : graph.nodes) {
        for (const engine::Port& port : node.inputs) {
            width = (std::max)(width, portRowWidth(port));
        }
        for (const engine::Port& port : node.outputs) {
            width = (std::max)(width, portRowWidth(port));
        }
    }
    ctx.content_width = width;
}

// --------------------------------------------------- 运行结果摘要（PA-03）----
namespace {

std::size_t result_signature(const engine::Node& node, const engine::RunSnapshot& snapshot)
{
    std::size_t signature = snapshot.nodes.size() * 1000003u;
    signature += static_cast<std::size_t>(node.state) * 10007u;
    signature += node.error_message.size() * 31u;
    if (const engine::RunNodeView* view = snapshot.find(node.id)) {
        signature += view->text.size() * 17u; // PB-01：读快照文本长度（含流式增长）
    }
    return signature;
}

} // namespace

NodeResultView resultViewOf(const CanvasContext& ctx, const engine::Node& node)
{
    struct Cached {
        std::size_t    signature = 0;
        NodeResultView view;
        bool           valid = false;
    };
    static std::unordered_map<std::string, Cached> cache;

    const engine::RunSnapshot& snapshot = ctx.run_snapshot ? ctx.run_snapshot() : empty_snapshot();
    const std::size_t          signature = result_signature(node, snapshot);
    Cached&                    entry     = cache[node.id];
    if (entry.valid && entry.signature == signature) {
        return entry.view;
    }

    entry.valid     = true;
    entry.signature = signature;
    entry.view      = NodeResultView{};

    if (node.state == engine::NodeState::Error) {
        std::string first = node.error_message;
        if (const std::size_t pos = first.find('\n'); pos != std::string::npos) {
            first.resize(pos);
        }
        if (first.size() > 120) {
            first.resize(117);
            first += "…";
        }
        entry.view.has_content = true;
        entry.view.is_error    = true;
        entry.view.text        = first.empty() ? std::string("执行失败") : first;
        return entry.view;
    }
    if (node.state == engine::NodeState::Skipped) {
        entry.view.has_content = true;
        entry.view.is_skipped  = true;
        entry.view.text        = "已跳过（上游失败）";
        return entry.view;
    }
    if (node.state == engine::NodeState::Running) {
        // PB-08：流式进度（增量字节数来自读模型；无增量时退回"运行中…"）
        const engine::RunNodeView* live  = snapshot.find(node.id);
        const std::size_t          bytes = (live != nullptr) ? live->delta_bytes : 0;
        entry.view.has_content = true;
        entry.view.text        = (bytes > 0) ? ("生成中…（" + std::to_string(bytes) + " 字）")
                                             : std::string("运行中…");
        return entry.view;
    }

    if (ctx.graph == nullptr) {
        return entry.view;
    }
    const std::string body = node_output_text(*ctx.graph, snapshot, node.id);
    if (!body.empty()) {
        std::string preview;
        for (const char ch : body) {
            if (preview.size() >= 160) {
                break;
            }
            preview += (ch == '\n' || ch == '\r') ? ' ' : ch;
        }
        std::string prefix = "结果： ";
        if (node.type == "TextOutput") { // M_textio P3：最终输出
            std::string label;
            if (const engine::Param* label_param = node.findParam("label")) {
                label = label_param->text();
            }
            prefix = label.empty() ? std::string("最终输出： ") : ("最终输出（" + label + "）： ");
        }
        entry.view.has_content = true;
        entry.view.text        = prefix + preview +
                          (body.size() > preview.size() ? "…" : "") +
                          "（" + std::to_string(body.size()) + " 字符）";
    }
    return entry.view;
}

// P7a-16：节点卡片要显示的图片
//  * 优先**本次运行结果**（RunNodeView.images，任意节点）
//  * 其次「图片输入」节点的参数（解析资源令牌 → 真实路径）
std::string nodeCardImage(const CanvasContext& ctx, const engine::Node& node)
{
    const engine::RunSnapshot& snapshot = ctx.run_snapshot ? ctx.run_snapshot() : empty_snapshot();
    if (const engine::RunNodeView* view = snapshot.find(node.id);
        view != nullptr && !view->images.empty()) {
        return view->images.front();
    }
    if (node.type != "ImageInput") {
        return {};
    }
    const engine::Param* path_param = node.findParam("path");
    if (path_param == nullptr) {
        return {};
    }
    const std::vector<std::string> entries = paths::splitPathList(path_param->text());
    if (entries.empty()) {
        return {};
    }
    bool              missing = false;
    std::string       error;
    const std::string local = asset::toLocalPath(entries.front(), &missing, &error);
    return missing ? std::string() : local;
}

// 节点内条件分隔线：clamp 到节点内容区宽度（ctx.content_width），不超出节点边框。
void drawNodeSeparator(const CanvasContext& ctx)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 p        = ImGui::GetCursorScreenPos();
    draw_list->AddLine(p, ImVec2(p.x + ctx.content_width, p.y),
                       ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(ctx.content_width, 0.0f)); // 占位推进一行；宽度顺带加固内容区宽度锚点
}
} // namespace aiwrite::ui
