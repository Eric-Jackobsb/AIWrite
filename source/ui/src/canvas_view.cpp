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

const CanvasTheme* effective_theme(const CanvasContext& ctx)
{
    return (ctx.theme != nullptr) ? ctx.theme : &canvasThemeMain();
}

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

// ------------------------------------------------------------- 端口绘制 -----
// 设计 §14.7：**输入端口在节点左侧、输出端口在节点右侧**
//   · 输入行：[圆点][名称]          —— 圆点贴左边缘，连线接在左侧
//   · 输出行：[        名称][圆点]  —— 名称与圆点右对齐，圆点贴右边缘，连线接在右侧
//   · 颜色按端口类型（theme.h）；可变长端口多一圈描边
void draw_pin(const CanvasContext& ctx, const engine::Node& node, const engine::Port& port,
              int index)
{
    const CanvasTheme& theme    = *effective_theme(ctx);
    const bool         is_input = (port.direction == engine::PortDirection::Input);

    ed::BeginPin(pinHandle(node, port.direction, index),
                 is_input ? ed::PinKind::Input : ed::PinKind::Output);
    // 连线锚点：输入取端口矩形左中，输出取右中（主画布契约：两画布一致）
    ed::PinPivotAlignment(is_input ? ImVec2(0.0f, 0.5f) : ImVec2(1.0f, 0.5f));

    ImDrawList* draw_list   = ImGui::GetWindowDrawList();
    const float line_height = ImGui::GetTextLineHeight();
    const ImU32 dot_color   = port_color(port.type);

    // 在"圆点占位 Dummy"的位置上画圆（占位 Dummy 同时保证端口矩形覆盖圆点）
    const auto draw_dot = [&](const ImVec2& dummy_top_left) {
        const ImVec2 center(dummy_top_left.x + kPinRadius, dummy_top_left.y + line_height * 0.5f);
        draw_list->AddCircleFilled(center, kPinRadius, dot_color, 16);
        draw_list->AddCircle(center, kPinRadius, theme.pin_outline, 16, 1.5f);
        if (port.is_variadic) {
            draw_list->AddCircle(center, kPinRadius + 2.5f, dot_color, 16, 1.0f);
        }
    };

    if (is_input) {
        // ---- 左列：圆点 + 名称 ----
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPinDotSize, line_height));
        draw_dot(dot_pos);

        ImGui::SameLine(0.0f, kPinTextGap);
        // 端口名按「内容宽度 − 圆点 − 间隙」收口（宽度由端口标签自适应，见 update_content_width）
        ImGui::TextUnformatted(
            elideToWidth(port.display_name, ctx.content_width - kPinDotSize - kPinTextGap)
                .c_str());
    }
    else {
        // ---- 右列：名称 + 圆点（名称先按可用宽度收口，再用占位 Dummy 推到右边界）----
        const std::string shown_name =
            elideToWidth(port.display_name, ctx.content_width - kPinDotSize - kPinTextGap);
        const float text_width = ImGui::CalcTextSize(shown_name.c_str()).x;
        const float spacer     = ctx.content_width - kPinDotSize - kPinTextGap - text_width;
        if (spacer > 0.0f) {
            ImGui::Dummy(ImVec2(spacer, line_height));
            ImGui::SameLine(0.0f, 0.0f);
        }

        ImGui::TextUnformatted(shown_name.c_str());

        ImGui::SameLine(0.0f, kPinTextGap);
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kPinDotSize, line_height));
        draw_dot(dot_pos);
    }

    ed::EndPin();
}

// ------------------------------------------------------------- 节点卡片 -----
// 设计 §14.1：标题栏（分类色 + 状态圆点）+ 端口 + 参数预览 + 图片卡片 +
// 提供商提示 + 参数校验 + 运行结果摘要。阶段 1 起主画布与上级编辑器共用此实现。
void drawNodeBody(const CanvasContext& ctx, const engine::Node& node)
{
    const CanvasTheme& theme = *effective_theme(ctx);
    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node.type);
    const engine::NodeCategory category =
        (definition != nullptr) ? definition->category : engine::NodeCategory::Process;

    ed::BeginNode(nodeHandle(node.id));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float  height = ImGui::GetTextLineHeight() + 8.0f;
        // 分类色条 + 状态圆点；CN：上级节点用紫色条区分（仅主画布主题开启）
        const ImU32 stripe = (!node.composite_id.empty() && theme.composite_stripe_enabled)
                                 ? theme.composite_stripe
                                 : category_color(category);
        draw_list->AddRectFilled(ImVec2(cursor.x - 4.0f, cursor.y),
                                 ImVec2(cursor.x, cursor.y + height), stripe, 2.0f);
        draw_list->AddCircleFilled(ImVec2(cursor.x + 7.0f, cursor.y + height * 0.5f), 4.0f,
                                   state_color(node.state), 12);
        // PA-08：ui.running_animation 开 → 运行中节点状态点加脉冲光环（关闭则静止）
        if (node.state == engine::NodeState::Running && aiwrite::appConfig().ui.running_animation) {
            const ImVec2 dot_center(cursor.x + 7.0f, cursor.y + height * 0.5f);
            const float  pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
            ImVec4       glow  = ImGui::ColorConvertU32ToFloat4(state_color(node.state));
            glow.w             = 0.30f + 0.45f * pulse;
            draw_list->AddCircle(dot_center, 6.0f + 3.0f * pulse,
                                 ImGui::ColorConvertFloat4ToU32(glow), 20, 1.5f);
        }
        ImGui::Dummy(ImVec2(13.0f, height));
        ImGui::SameLine(0.0f, 2.0f);
        // 宽度约束：标题 + (id) 合计必须落在内容宽度内（超出按像素省略，不撑宽节点）
        const float       title_avail = ctx.content_width - kNodeTitleIndent;
        const std::string id_text     = "(" + node.id + ")";
        const float       id_w        = ImGui::CalcTextSize(id_text.c_str()).x;
        const std::string shown_title =
            elideToWidth(node.title, title_avail - id_w - kLabelGap);
        ImGui::TextUnformatted(shown_title.c_str());
        ImGui::SameLine(0.0f, kLabelGap);
        const float id_room =
            title_avail - ImGui::CalcTextSize(shown_title.c_str()).x - kLabelGap;
        ImGui::TextDisabled("%s", elideToWidth(id_text, id_room).c_str());
    }

    // 宽度（**唯一**的宽度来源）：这一行占位把内容区宽度钉死为 ctx.content_width（全画布共用值），
    // 配合节点内所有文本的收口（elide / wrap）→ 画布上**所有节点等宽**。
    // 2026-10-15 确认：标题下方的 Separator 被用户视为「每个节点上面自带的长灰线」→ 移除（网格保留）。
    ImGui::Dummy(ImVec2(ctx.content_width, 1.0f));

    // ---- 输入端口（左列）----
    int index = 0;
    for (const engine::Port& port : node.inputs) {
        draw_pin(ctx, node, port, index++);
    }
    // ---- 输出端口（右列）----
    index = 0;
    for (const engine::Port& port : node.outputs) {
        draw_pin(ctx, node, port, index++);
    }

    if (!node.params.empty()) { // 参数预览
        drawNodeSeparator(ctx);
        int shown = 0;
        for (const engine::Param& param : node.params) {
            if (!engine::paramVisible(node, param)) {
                continue; // 条件隐藏的参数不预览（如 web 模式下的 API Key / API 地址）
            }
            // 内联编辑开启时，TextInput 的「文本内容」由下方编辑区展示，不再重复只读预览
            if (ctx.inline_edit_text && node.type == "TextInput" && param.id == "text") {
                continue;
            }
            if (shown >= 2) {
                break;
            }
            std::string value = param.text();
            if (param.is_secret && !value.empty()) {
                value = "******";
            }
            if (value.size() > 18) {
                value = value.substr(0, 18) + "…";
            }
            // 宽度约束：整行（参数名 + 值）按像素收口，避免长参数名撑宽节点
            const std::string line =
                param.display_name + ": " + (value.empty() ? std::string("(空)") : value);
            ImGui::TextDisabled("%s", elideToWidth(line, ctx.content_width).c_str());
            ++shown;
        }
    }

    // ---- 内联文本编辑（仅上级节点编辑器开启：TextInput 节点文本在卡片上直接编辑）----
    // 官方 widgets-example 同款模式：节点内直接放 InputText（本库版本无 BeginAttribute，
    // 该示例即为本版本的可用形态）。输入隔离由调用方保证（上级编辑器已 EnableShortcuts(false)）。
    if (ctx.inline_edit_text && node.type == "TextInput") {
        const engine::Param* text_param = node.findParam("text");
        if (text_param != nullptr && text_param->type == engine::ParamType::Text) {
            drawNodeSeparator(ctx);
            // 编辑缓冲按节点 id 缓存（跨帧存活）；失焦时与模型同步，撤销/外部编辑即时可见
            static std::unordered_map<std::string, std::string> inline_bufs;
            std::string& buf = inline_bufs[node.id];
            const std::string current = text_param->text();
            const std::string label   = "##node_inline_" + node.id; // 节点级唯一 ID（DEBT-C4：去 `ce_` 借名）
            ImGui::InputTextMultiline(label.c_str(), &buf,
                                      ImVec2(ctx.content_width,
                                             ImGui::GetTextLineHeight() * 3.0f));
            if (ImGui::IsItemActivated() && ctx.on_param_edit_begin) {
                ctx.on_param_edit_begin(); // 编辑开始 → 压「修改前」快照（可撤销）
            }
            if (ImGui::IsItemActive()) {
                if (buf != current && ctx.on_param_edit_commit) {
                    ctx.on_param_edit_commit(node.id, text_param->id, buf); // 值变化即写回（P1-c）
                }
            }
            else if (buf != current) {
                buf = current; // 未在编辑 → 以模型为准（覆盖撤销 / 右侧面板编辑 / 清空结果）
            }
        }
    }

    // ---- P7a-16：图片卡片（缩略图 + 尺寸 + 格式徽标；格式取内容嗅探结果）----
    if (const std::string card_image = nodeCardImage(ctx, node); !card_image.empty() && ctx.textures != nullptr) {
        const TextureInfo texture = ctx.textures->textureFor(card_image);
        drawNodeSeparator(ctx);
        if (texture.texture != 0) {
            float width  = static_cast<float>(texture.width);
            float height = static_cast<float>(texture.height);
            if (width > ctx.content_width && width > 0.0f) {
                const float shrink = ctx.content_width / width;
                width *= shrink;
                height *= shrink;
            }
            ImGui::Image(
                reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture.texture)),
                ImVec2(width, height));
            ImGui::TextDisabled("%d×%d · %s", texture.width, texture.height,
                                texture.format.empty() ? "未知格式" : texture.format.c_str());
        }
        else {
            // 宽度约束：解码失败文案可能很长（含魔数 / 扩展名建议）→ 按像素收口
            const std::string reason =
                "图片预览失败：" + (texture.error.empty() ? std::string("未知原因") : texture.error);
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               elideToWidth(reason, ctx.content_width).c_str());
        }
    }

    // ---- 生效提供商提示（P1-a：provider 输入优先；official 未接线 → 必定失败）----
    if (engine::usesProvider(node.type) && ctx.graph != nullptr) {
        const engine::EffectiveProvider effective =
            engine::resolveEffectiveProvider(*ctx.graph, node);
        const std::string reason = engine::unwiredReason(*ctx.graph, node);

        drawNodeSeparator(ctx);
        if (node.type == "LLMGenerate") {
            // 固定宽度约束：来源节点标题长度不可控 → 按像素收口
            const std::string line =
                effective.from_edge
                    ? ("生效 " + effective.mode + " ← " + effective.source_node)
                    : ("生效 " + effective.mode + "（节点自身）");
            ImGui::TextDisabled("%s", elideToWidth(line, ctx.content_width).c_str());
        }
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            // 自绘折行（每行 ≤ 内容宽度），不靠 ImGui 自动换行 → 长文案不撑宽节点
            ImGui::TextUnformatted(
                wrapToWidth(reason + "：必定失败", ctx.content_width, kNodeSummaryLines).c_str());
            ImGui::PopStyleColor();
        }
    }

    if (ctx.graph != nullptr) {
        std::vector<std::string> errors;
        ctx.graph->validateParams(node, &errors);
        if (!errors.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验 %d 项错误",
                               static_cast<int>(errors.size()));
        }
    }

    // ---- 运行结果摘要（PA-03：只读；与输出面板 / 参数面板同源）----
    const NodeResultView result_view = resultViewOf(ctx, node);
    if (result_view.has_content) {
        drawNodeSeparator(ctx);
        if (result_view.is_error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        }
        else if (result_view.is_skipped) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.70f, 0.30f, 1.0f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.75f, 0.42f, 1.0f));
        }
        // 自绘折行（每行 ≤ 内容宽度）+ 只显示前 kNodeSummaryLines 行 → 长正文不撑宽节点
        ImGui::TextUnformatted(
            wrapToWidth(result_view.text, ctx.content_width, kNodeSummaryLines).c_str());
        ImGui::PopStyleColor();
    }

    ed::EndNode();
}

// ------------------------------------------------------ 小地图（CLN-09）----
// 单一来源：两画布共用同一套几何 / 输入屏蔽 / 绘制。行为差异通过参数保留：
//  - show：本 TU 的小地图开关（点 ✕ 关闭回写）
//  - state：本 TU 的输入屏蔽快照
//  - node_color：节点块着色（主画布=按状态着色 / 上级编辑器=固定蓝紫色）
void minimapRect(ImVec2& out_min, ImVec2& out_max)
{
    const ImVec2 window_pos  = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float  margin      = 10.0f;
    out_min = ImVec2(window_pos.x + window_size.x - 190.0f - margin,
                     window_pos.y + window_size.y - 130.0f - margin);
    out_max = ImVec2(out_min.x + 190.0f, out_min.y + 130.0f);
}

bool minimapHit(const ImVec2& mouse, const ImVec2& min, const ImVec2& max)
{
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

void blockMinimapInput(bool& show, MinimapState& state)
{
    state.blocking = false;
    state.clicked  = false;
    state.down     = false;
    if (!show) {
        return;
    }

    ImVec2 map_min, map_max;
    minimapRect(map_min, map_max);

    ImGuiIO&    io    = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    if (!minimapHit(mouse, map_min, map_max)) {
        return;
    }

    // 快照原始输入（draw_minimap 用；io 随后被清空，画布拿不到这些事件）
    state.blocking = true;
    state.mouse    = mouse;
    state.clicked  = io.MouseClicked[0];
    state.down     = io.MouseDown[0];

    for (int i = 0; i < 5; ++i) {
        io.MouseClicked[i]  = false;
        io.MouseDown[i]     = false;
        io.MouseReleased[i] = false;
    }
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MouseDelta  = ImVec2(0.0f, 0.0f);
}

void draw_minimap(const engine::Graph& graph, const CanvasTheme& theme, bool& show,
                  const MinimapState& state,
                  const std::function<ImU32(const engine::Node&)>& node_color)
{
    if (!show) {
        return;
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImVec2 map_min, map_max;
    minimapRect(map_min, map_max);

    draw_list->AddRectFilled(map_min, map_max, theme.minimap_bg, 4.0f);
    draw_list->AddRect(map_min, map_max, theme.minimap_border, 4.0f, 0, 1.0f);
    draw_list->AddText(map_min + ImVec2(6.0f, 3.0f), theme.minimap_title, "小地图");

    const ImVec2 close_pos(map_max.x - 18.0f, map_min.y + 2.0f);
    if (state.blocking && state.clicked && state.mouse.x >= close_pos.x &&
        state.mouse.x <= close_pos.x + 14.0f && state.mouse.y >= close_pos.y &&
        state.mouse.y <= close_pos.y + 14.0f) {
        show = false;
        return;
    }
    draw_list->AddText(close_pos, theme.minimap_close, "✕");

    const ImVec2 area_min(map_min.x + 6.0f, map_min.y + 20.0f);
    const ImVec2 area_max(map_max.x - 6.0f, map_max.y - 6.0f);
    if (graph.nodes.empty()) {
        draw_list->AddText(area_min, theme.minimap_empty, "空画布");
        return;
    }

    ImVec2 bbox_min(FLT_MAX, FLT_MAX);
    ImVec2 bbox_max(-FLT_MAX, -FLT_MAX);
    for (const engine::Node& node : graph.nodes) {
        const ImVec2 sz = ed::GetNodeSize(nodeHandle(node.id));
        const float  w  = (sz.x > 1.0f) ? sz.x : 80.0f;
        const float  h  = (sz.y > 1.0f) ? sz.y : 40.0f;
        bbox_min.x = std::min(bbox_min.x, node.x);
        bbox_min.y = std::min(bbox_min.y, node.y);
        bbox_max.x = std::max(bbox_max.x, node.x + w);
        bbox_max.y = std::max(bbox_max.y, node.y + h);
    }
    const ImVec2 bbox_size(std::max(bbox_max.x - bbox_min.x, 1.0f),
                           std::max(bbox_max.y - bbox_min.y, 1.0f));
    const ImVec2 area_size(area_max.x - area_min.x, area_max.y - area_min.y);
    const float  scale = std::min(area_size.x / bbox_size.x, area_size.y / bbox_size.y);

    for (const engine::Node& node : graph.nodes) {
        const ImVec2 sz = ed::GetNodeSize(nodeHandle(node.id));
        const float  w  = std::max((sz.x > 1.0f ? sz.x : 80.0f) * scale, 3.0f);
        const float  h  = std::max((sz.y > 1.0f ? sz.y : 40.0f) * scale, 3.0f);
        const ImVec2 p(area_min.x + (node.x - bbox_min.x) * scale,
                       area_min.y + (node.y - bbox_min.y) * scale);
        draw_list->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                                 node_color(node), 1.0f);
        draw_list->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                           IM_COL32(0, 0, 0, 190), 1.0f);
    }

    if (state.blocking && (state.clicked || state.down) &&
        minimapHit(state.mouse, area_min, area_max)) {
        const float cx = (state.mouse.x - area_min.x) / scale + bbox_min.x;
        const float cy = (state.mouse.y - area_min.y) / scale + bbox_min.y;
        ed::NavigateToCenter(ImVec2(cx, cy), 0.0f);
    }
}


// ----------------------------------------------------- 对齐吸附（CLN-10）----
// 单一来源：两画布共用同一套「拖拽判定 + 对齐核心 + 辅助线」。行为差异通过参数保留：
//  - find_dragged_node：suppress=Alt 平移互斥（主画布传 g_alt_pan_active / 上级传 false）
//  - apply_alignment_snap_core：snap_canvas=距离阈值（主画布 g_snap_distance / 上级 kCeSnapDistance）
//  - draw_snap_lines：颜色 / 线宽由调用方传入
std::string find_dragged_node(const engine::Graph& graph, bool suppress,
                              std::unordered_map<std::string, ImVec2>& prev_positions)
{
    if (suppress || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        prev_positions.clear();
        return {};
    }
    for (const engine::Node& node : graph.nodes) {
        const ImVec2 current = ed::GetNodePosition(nodeHandle(node.id));
        const auto   it      = prev_positions.find(node.id);
        if (it == prev_positions.end()) {
            prev_positions[node.id] = current;
            continue;
        }
        if (std::fabs(current.x - it->second.x) > 0.5f ||
            std::fabs(current.y - it->second.y) > 0.5f) {
            return node.id;
        }
    }
    return {};
}

SnapResult apply_alignment_snap_core(const engine::Graph& graph, const std::string& dragged,
                                     const ImVec2& dpos, float dw, float dh, float snap_canvas)
{
    SnapResult result;
    const float dleft   = dpos.x;
    const float dright  = dpos.x + dw;
    const float dcx     = dpos.x + dw * 0.5f;
    const float dtop    = dpos.y;
    const float dbottom = dpos.y + dh;
    const float dcy     = dpos.y + dh * 0.5f;

    float  dx      = 0.0f;
    float  dy      = 0.0f;
    bool   have_x  = false;
    bool   have_y  = false;
    ImVec2 v_a, v_b, h_a, h_b;
    for (const engine::Node& other : graph.nodes) {
        if (other.id == dragged) {
            continue;
        }
        const ImVec2 pos  = ed::GetNodePosition(nodeHandle(other.id));
        const ImVec2 size = ed::GetNodeSize(nodeHandle(other.id));
        const float  w    = size.x > 1.0f ? size.x : 80.0f;
        const float  h    = size.y > 1.0f ? size.y : 40.0f;
        const float  left   = pos.x;
        const float  right  = pos.x + w;
        const float  cx     = pos.x + w * 0.5f;
        const float  top    = pos.y;
        const float  bottom = pos.y + h;
        const float  cy     = pos.y + h * 0.5f;

        // X：左 / 右 / 中心 三对边取 |delta| 最小者
        const float dx_left  = left - dleft;
        const float dx_right = right - dright;
        const float dx_cx    = cx - dcx;
        float       dx_best  = dx_left;
        if (std::fabs(dx_right) < std::fabs(dx_best)) {
            dx_best = dx_right;
        }
        if (std::fabs(dx_cx) < std::fabs(dx_best)) {
            dx_best = dx_cx;
        }
        if (std::fabs(dx_best) <= snap_canvas && (!have_x || std::fabs(dx_best) < std::fabs(dx))) {
            dx     = dx_best;
            have_x = true;
            const float align_x = dleft + dx_best;
            v_a = ed::CanvasToScreen(ImVec2(align_x, std::min(dtop, top)));
            v_b = ed::CanvasToScreen(ImVec2(align_x, std::max(dbottom, bottom)));
        }

        // Y：上 / 下 / 中心 三对边
        const float dy_top    = top - dtop;
        const float dy_bottom = bottom - dbottom;
        const float dy_cy     = cy - dcy;
        float       dy_best   = dy_top;
        if (std::fabs(dy_bottom) < std::fabs(dy_best)) {
            dy_best = dy_bottom;
        }
        if (std::fabs(dy_cy) < std::fabs(dy_best)) {
            dy_best = dy_cy;
        }
        if (std::fabs(dy_best) <= snap_canvas && (!have_y || std::fabs(dy_best) < std::fabs(dy))) {
            dy     = dy_best;
            have_y = true;
            const float align_y = dtop + dy_best;
            h_a = ed::CanvasToScreen(ImVec2(std::min(dleft, left), align_y));
            h_b = ed::CanvasToScreen(ImVec2(std::max(dright, right), align_y));
        }
    }

    if (have_x || have_y) {
        ImVec2 new_pos = dpos;
        if (have_x) {
            new_pos.x      = dpos.x + dx;
            result.line_v  = true;
            result.v_a     = v_a;
            result.v_b     = v_b;
        }
        if (have_y) {
            new_pos.y      = dpos.y + dy;
            result.line_h  = true;
            result.h_a     = h_a;
            result.h_b     = h_b;
        }
        ed::SetNodePosition(nodeHandle(dragged), new_pos);
        result.applied = true;
    }
    return result;
}

void drawSnapLines(const SnapResult& snap, ImDrawList* draw_list, ImU32 color, float width)
{
    if (snap.line_v) {
        draw_list->AddLine(snap.v_a, snap.v_b, color, width);
    }
    if (snap.line_h) {
        draw_list->AddLine(snap.h_a, snap.h_b, color, width);
    }
}


} // namespace aiwrite::ui





