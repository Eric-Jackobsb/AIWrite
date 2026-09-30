#include "ui/node_canvas.h"

#include "engine/node_registry.h"
#include "engine/provider_resolve.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ui/theme.h"
#include "utils/config.h"
#include "utils/asset_store.h" // P7a-16：图片卡片解析资源令牌
#include "utils/log.h"
#include "utils/paths.h"
#include "ui/texture_cache.h"  // P7a-16：节点卡片缩略图 + 格式徽标

#include <imgui.h>
#include <imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::ui {
namespace {

namespace ed = ax::NodeEditor;

using engine::Edge;
using engine::Graph;
using engine::Node;
using engine::NodeCategory;
using engine::NodeState;
using engine::Port;
using engine::PortDirection;
using engine::PortType;

ed::EditorContext* g_context             = nullptr;
bool               g_first_frame         = true;
bool               g_drag_in_progress    = false; // 一次拖拽只压一次快照
bool               g_delete_in_progress  = false; // 一次删除批次只压一次快照

// 颜色（分类 / 端口类型 / 节点状态）统一由 ui/theme.h 提供（设计 §14.3 / §4.5 / §4.6）

// ------------------------------------------------- 稳定手柄映射（id ↔ handle）---
// 节点 "n12" → 12；端口 = (节点序号 << 8) | (输出位 0x80) | 端口下标
ed::NodeId node_handle(const std::string& node_id)
{
    return ed::NodeId(static_cast<uintptr_t>(engine::numericIdOf(node_id)));
}

ed::LinkId link_handle(const std::string& edge_id)
{
    return ed::LinkId(static_cast<uintptr_t>(engine::numericIdOf(edge_id)));
}

ed::PinId pin_handle(const Node& node, PortDirection direction, int index)
{
    const uintptr_t base  = static_cast<uintptr_t>(engine::numericIdOf(node.id)) << 8;
    const uintptr_t flags = (direction == PortDirection::Output) ? 0x80u : 0x00u;
    return ed::PinId(base | flags | static_cast<uintptr_t>(index & 0x7F));
}

struct PinRef {
    std::string   node_id;
    std::string   port_id;
    PortDirection direction = PortDirection::Input;
    bool          valid     = false;
};

PinRef resolve_pin(ed::PinId id, const Graph& graph)
{
    PinRef ref;
    const uintptr_t value = id.Get();
    if (value == 0) {
        return ref;
    }
    const int node_seq = static_cast<int>((value >> 8) & 0x7FFFFF);
    const PortDirection direction =
        ((value & 0x80u) != 0) ? PortDirection::Output : PortDirection::Input;
    const int index = static_cast<int>(value & 0x7F);

    const Node* node = graph.findNode("n" + std::to_string(node_seq));
    if (node == nullptr) {
        return ref;
    }
    const std::vector<Port>& ports = (direction == PortDirection::Input) ? node->inputs : node->outputs;
    if (index < 0 || index >= static_cast<int>(ports.size())) {
        return ref;
    }

    ref.node_id   = node->id;
    ref.port_id   = ports[static_cast<std::size_t>(index)].id;
    ref.direction = direction;
    ref.valid     = true;
    return ref;
}

// ------------------------------------------------------------- 初始化 -------
// 设计 §14.4：画布/节点/边框/选中 配色
void apply_editor_style()
{
    ed::Style& style              = ed::GetStyle();
    style.NodeRounding            = 4.0f;
    style.NodeBorderWidth         = 1.5f;
    style.SelectedNodeBorderWidth = 2.5f;
    style.NodePadding             = ImVec4(10.0f, 8.0f, 10.0f, 8.0f);
    // 说明（PA-08 治理）：vendored imgui-node-editor 的 Style **没有网格间距字段**，
    // 因此 ui.grid_size 目前无法生效 —— 已在 unwired_config_fields() 中如实登记并在启动日志提示
    style.Colors[ed::StyleColor_Bg]            = ImColor(30, 30, 30, 255);
    style.Colors[ed::StyleColor_Grid]          = ImColor(42, 42, 42, 255);
    style.Colors[ed::StyleColor_NodeBg]        = ImColor(45, 45, 45, 255);
    style.Colors[ed::StyleColor_NodeBorder]    = ImColor(58, 58, 58, 255);
    style.Colors[ed::StyleColor_SelNodeBorder] = ImColor(74, 144, 226, 255);
    style.Colors[ed::StyleColor_HovNodeBorder] = ImColor(74, 144, 226, 200);
}

// ---------------------------------------------------------- 设置文件净化 -----
// node-editor 会把节点位置与视图（scroll / visible_rect / zoom）持久化到
// ~/.brain-ai/node_editor.json 并在启动时恢复。若其中残留损坏数值
// （例如 ±2^31 的坐标、2e-7 的缩放 —— 历史上"复制粘贴"脏值事故写过一次），
// 编辑器每帧的网格与剔除计算会爆量，表现为 **CPU 单核打满 + 界面无响应**。
// 这里在创建编辑器之前校验一次：异常则备份改名，让编辑器以默认视图重建。
constexpr double kSettingsNumberLimit = 1000000.0;

bool settings_numbers_sane(const nlohmann::json& value)
{
    if (value.is_number()) {
        const double number = value.is_number_float() ? value.get<double>()
                                                      : static_cast<double>(value.get<long long>());
        return std::isfinite(number) && std::fabs(number) <= kSettingsNumberLimit;
    }
    if (value.is_object() || value.is_array()) {
        for (const auto& item : value) {
            if (!settings_numbers_sane(item)) {
                return false;
            }
        }
    }
    return true;
}

bool editor_settings_sane(const nlohmann::json& settings)
{
    if (!settings.is_object() || !settings_numbers_sane(settings)) {
        return false;
    }
    // 缩放必须落在编辑器允许的包络内（本项目配置 0.1x ~ 4.0x，留出余量）
    const auto view = settings.find("view");
    if (view != settings.end() && view->is_object()) {
        const auto zoom = view->find("zoom");
        if (zoom != view->end()) {
            if (!zoom->is_number()) {
                return false;
            }
            const double value = zoom->get<double>();
            if (!std::isfinite(value) || value < 0.05 || value > 10.0) {
                return false;
            }
        }
    }
    return true;
}

void sanitize_editor_settings(const std::filesystem::path& file)
{
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return;
    }

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return;
    }
    const std::string raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();

    bool sane = false;
    try {
        sane = editor_settings_sane(nlohmann::json::parse(raw));
    }
    catch (const std::exception& ex) {
        log::warn(std::string("[画布] 编辑器设置文件解析失败: ") + ex.what());
        sane = false;
    }

    if (sane) {
        return;
    }

    char stamp[32] = {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);

    const std::filesystem::path backup =
        file.parent_path() / ("node_editor_bad_" + std::string(stamp) + ".json");
    std::filesystem::rename(file, backup, error);
    if (error) {
        std::filesystem::remove(file, error);
        log::warn("[画布] 检测到损坏的编辑器设置（坐标/缩放异常），已删除并重建默认视图");
    }
    else {
        log::warn("[画布] 检测到损坏的编辑器设置（坐标/缩放异常），已备份为 " +
                  backup.filename().string() + " 并重建默认视图");
    }
}

void ensure_context()
{
    if (g_context != nullptr) {
        return;
    }

    static std::string settings_file = (paths::data_root() / "node_editor.json").string();

    // 启动前净化：损坏的坐标/视图会让 node-editor 的网格计算爆量（CPU 打满、界面无响应）
    sanitize_editor_settings(settings_file);

    ed::Config config;
    config.SettingsFile = settings_file.c_str();
    // 设计 §6.4：缩放范围 0.1x ~ 4.0x
    const float levels[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    config.CustomZoomLevels.resize(IM_ARRAYSIZE(levels));
    for (int i = 0; i < IM_ARRAYSIZE(levels); ++i) {
        config.CustomZoomLevels[i] = levels[i];
    }

    g_context = ed::CreateEditor(&config);
    log::info("[画布] 编辑器上下文已创建");

    // 注意：GetStyle/PushStyleColor 作用于"当前编辑器"，必须先 SetCurrentEditor
    ed::SetCurrentEditor(g_context);
    apply_editor_style();
    log::info("[画布] 编辑器样式已应用（缩放范围 0.1x ~ 4.0x）");

    // 设计 §6.7：MVP 不保留任何快捷键，全部改为鼠标操作（右键菜单 / 工具栏 / 菜单栏）
    ed::EnableShortcuts(false);
    log::info("[画布] 已关闭画布键盘快捷键（仅保留鼠标操作）");
}

// 节点宽度：**由「输入/输出标签实测最宽」决定，且全画布所有节点共用同一个值**
//   · 宽度 = max(所有节点里最宽的一行端口标签, 下限 `kNodeContentWidthMin`) —— 见 `update_content_width()`
//   · 因此节点的左边界（输入圆点）与右边界（输出圆点）在**所有节点上对齐**，且**与文本长度无关**
//   · 配套约束：节点内**任何文本都必须收口**（单行 `elide_to_width` / 多行 `wrap_to_width`），
//     否则长标题 / 长参数 / 长错误文案会把节点撑宽，端口边界随之漂移（同一画布上节点宽度不一致）
constexpr float kNodeContentWidthMin = 160.0f;              // 内容区下限（保证标题行 / 参数预览可读）
float           g_content_width      = kNodeContentWidthMin; // 每帧由 update_content_width() 重算
constexpr float kPinRadius        = 5.0f;
constexpr float kPinDotSize       = kPinRadius * 2.0f;
constexpr float kPinTextGap       = 6.0f;
constexpr float kNodeTitleIndent  = 15.0f; // 标题行左侧占位（分类色条 + 状态圆点）
constexpr float kLabelGap         = 4.0f;  // 同一行内文本片段之间的间隙
constexpr int   kNodeSummaryLines = 6;     // 节点卡片上多行文本（运行结果 / 必失败原因）最多几行

// 只裁剪、**不加**省略号：返回不超过 max_width 像素的最长前缀（不切断 UTF-8 多字节序列）。
std::string clip_to_width(const std::string& text, float max_width)
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

// 单行省略 = clip 到「max_width − 省略号宽度」再补 …（**保证结果 ≤ max_width**）。
// 按**像素**而非字符数裁剪 → 中英混排 / 全角半角 / 数字串都不会把节点撑宽。
std::string elide_to_width(const std::string& text, float max_width)
{
    const std::string ellipsis   = "…";
    const float       ellipsis_w = ImGui::CalcTextSize(ellipsis.c_str()).x;
    if (max_width <= ellipsis_w) {
        return ellipsis; // 连省略号都放不下
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text; // 放得下 → 原样返回（零开销路径）
    }
    return clip_to_width(text, max_width - ellipsis_w) + ellipsis;
}

// ------------------------------------------------------------- 端口绘制 -----
// 设计 §14.7：**输入端口在节点左侧、输出端口在节点右侧**
//   · 输入行：[圆点][名称]          —— 圆点贴左边缘，连线接在左侧
//   · 输出行：[        名称][圆点]  —— 名称与圆点右对齐，圆点贴右边缘，连线接在右侧
//   · 颜色按端口类型（§4.5）；可变长端口多一圈描边
void draw_pin(const Node& node, const Port& port, int index)
{
    const bool is_input = (port.direction == PortDirection::Input);

    ed::BeginPin(pin_handle(node, port.direction, index),
                 is_input ? ed::PinKind::Input : ed::PinKind::Output);
    // 连线锚点：输入取端口矩形左中，输出取右中
    ed::PinPivotAlignment(is_input ? ImVec2(0.0f, 0.5f) : ImVec2(1.0f, 0.5f));

    ImDrawList* draw_list   = ImGui::GetWindowDrawList();
    const float line_height = ImGui::GetTextLineHeight();
    const ImU32 dot_color   = port_color(port.type);

    // 在"圆点占位 Dummy"的位置上画圆（占位 Dummy 同时保证端口矩形覆盖圆点）
    const auto draw_dot = [&](const ImVec2& dummy_top_left) {
        const ImVec2 center(dummy_top_left.x + kPinRadius, dummy_top_left.y + line_height * 0.5f);
        draw_list->AddCircleFilled(center, kPinRadius, dot_color, 16);
        draw_list->AddCircle(center, kPinRadius, IM_COL32(20, 20, 20, 255), 16, 1.5f);
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
            elide_to_width(port.display_name, g_content_width - kPinDotSize - kPinTextGap)
                .c_str());
    }
    else {
        // ---- 右列：名称 + 圆点（名称先按可用宽度收口，再用占位 Dummy 推到右边界）----
        const std::string shown_name =
            elide_to_width(port.display_name, g_content_width - kPinDotSize - kPinTextGap);
        const float text_width = ImGui::CalcTextSize(shown_name.c_str()).x;
        const float spacer     = g_content_width - kPinDotSize - kPinTextGap - text_width;
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

// 折行（多行文本用）：按**像素**把文本切成若干行 —— **不依赖 ImGui 的自动换行**（它对中文这种
// 「无空格长词」的行为不可靠），所以中文也能在任意字符间断行；每行 ≤ max_width，
// 超过 max_lines 行或字符预算时在末行补 …（节点卡片是摘要，完整正文在输出面板）。
std::string wrap_to_width(const std::string& text, float max_width, int max_lines)
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
            item = clip_to_width(item, max_width);
        }
    }

    // 收口保证②：要补 … 时，先给末行让出「省略号的宽度」（否则末行会超宽 —— 自检抓到过 220 > 215）
    const bool mark = line_cut || budget_cut;
    if (mark) {
        const float marker_w = ImGui::CalcTextSize("…").x;
        if (lines.empty()) {
            lines.emplace_back();
        }
        lines.back() = clip_to_width(lines.back(), max_width - marker_w);
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

// 一行端口标签占用的宽度（左列 = [圆点][间隙][名称]，右列同宽）
float port_row_width(const Port& port)
{
    return kPinDotSize + kPinTextGap + ImGui::CalcTextSize(port.display_name.c_str()).x;
}

// 按当前图重算「全画布共用的内容区宽度」：取所有节点里**最宽的一行端口标签**（下限 kNodeContentWidthMin）。
// 每帧重算（代价 = 节点数 × 端口数 次 CalcTextSize，可忽略）→ 换工作流 / 增删节点即时生效。
void update_content_width(const Graph& graph)
{
    float width = kNodeContentWidthMin;
    for (const Node& node : graph.nodes) {
        for (const Port& port : node.inputs) {
            width = (std::max)(width, port_row_width(port));
        }
        for (const Port& port : node.outputs) {
            width = (std::max)(width, port_row_width(port));
        }
    }
    g_content_width = width;
}

// ------------------------------------------------------------- 节点绘制 -----
// 设计 §14.1：标题栏（分类色 + 状态圆点）+ 端口 + 参数预览 + 错误提示
// ------------------------------------------------- 运行结果摘要（PA-03）----
// 节点内只读展示运行结果（与输出面板 / 参数面板「运行结果」同源 ui::node_output_text）。
// 用「运行信息条数 + 状态 + 错误长度 + 输出字节数」做轻量签名缓存，避免每帧重算长文本。
struct NodeResultView {
    std::string text;
    bool        has_content = false;
    bool        is_error    = false;
    bool        is_skipped  = false;
};

std::size_t result_signature(const Node& node, const engine::RunSnapshot& snapshot)
{
    std::size_t signature = snapshot.nodes.size() * 1000003u;
    signature += static_cast<std::size_t>(node.state) * 10007u;
    signature += node.error_message.size() * 31u;
    if (const engine::RunNodeView* view = snapshot.find(node.id)) {
        signature += view->text.size() * 17u; // PB-01：读快照文本长度（含流式增长）
    }
    return signature;
}

NodeResultView result_view_of(const Node& node)
{
    struct Cached {
        std::size_t    signature = 0;
        NodeResultView view;
        bool           valid = false;
    };
    static std::unordered_map<std::string, Cached> cache;

    const engine::RunSnapshot& snapshot = editor().run_snapshot_view();
    const std::size_t       signature = result_signature(node, snapshot);
    Cached&                 entry     = cache[node.id];
    if (entry.valid && entry.signature == signature) {
        return entry.view;
    }

    entry.valid     = true;
    entry.signature = signature;
    entry.view      = NodeResultView{};

    if (node.state == NodeState::Error) {
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
    if (node.state == NodeState::Skipped) {
        entry.view.has_content = true;
        entry.view.is_skipped  = true;
        entry.view.text        = "已跳过（上游失败）";
        return entry.view;
    }
    if (node.state == NodeState::Running) {
        // PB-08：流式进度（增量字节数来自读模型；无增量时退回"运行中…"）
        const engine::RunNodeView* live  = snapshot.find(node.id);
        const std::size_t          bytes = (live != nullptr) ? live->delta_bytes : 0;
        entry.view.has_content = true;
        entry.view.text        = (bytes > 0) ? ("生成中…（" + std::to_string(bytes) + " 字）")
                                            : std::string("运行中…");
        return entry.view;
    }

    const std::string body = node_output_text(editor().graph, editor().run_snapshot_view(), node.id);
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
std::string node_card_image(const Node& node)
{
    const engine::RunSnapshot& snapshot = editor().run_snapshot_view();
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
    const std::vector<std::string> entries = paths::split_path_list(path_param->text());
    if (entries.empty()) {
        return {};
    }
    bool              missing = false;
    std::string       error;
    const std::string local = asset::to_local_path(entries.front(), &missing, &error);
    return missing ? std::string() : local;
}

void draw_node_body(const Node& node)
{
    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node.type);
    const NodeCategory category =
        (definition != nullptr) ? definition->category : NodeCategory::Process;

    ed::BeginNode(node_handle(node.id));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float  height = ImGui::GetTextLineHeight() + 8.0f;
        // 分类色条 + 状态圆点
        draw_list->AddRectFilled(ImVec2(cursor.x - 4.0f, cursor.y),
                                 ImVec2(cursor.x, cursor.y + height), category_color(category), 2.0f);
        draw_list->AddCircleFilled(ImVec2(cursor.x + 7.0f, cursor.y + height * 0.5f), 4.0f,
                                   state_color(node.state), 12);
        // PA-08：ui.running_animation 开 → 运行中节点状态点加脉冲光环（关闭则静止）
        if (node.state == NodeState::Running && aiwrite::app_config().ui.running_animation) {
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
        const float       title_avail = g_content_width - kNodeTitleIndent;
        const std::string id_text     = "(" + node.id + ")";
        const float       id_w        = ImGui::CalcTextSize(id_text.c_str()).x;
        const std::string shown_title =
            elide_to_width(node.title, title_avail - id_w - kLabelGap);
        ImGui::TextUnformatted(shown_title.c_str());
        ImGui::SameLine(0.0f, kLabelGap);
        const float id_room =
            title_avail - ImGui::CalcTextSize(shown_title.c_str()).x - kLabelGap;
        ImGui::TextDisabled("%s", elide_to_width(id_text, id_room).c_str());
    }

    // 宽度（**唯一**的宽度来源）：这一行占位把内容区宽度钉死为 g_content_width（全画布共用值），
    // 配合节点内所有文本的收口（elide / wrap）→ 画布上**所有节点等宽**。
    ImGui::Dummy(ImVec2(g_content_width, 1.0f));
    ImGui::Separator();

    // ---- 输入端口（左列）----
    int index = 0;
    for (const Port& port : node.inputs) {
        draw_pin(node, port, index++);
    }
    // ---- 输出端口（右列）----
    index = 0;
    for (const Port& port : node.outputs) {
        draw_pin(node, port, index++);
    }

    if (!node.params.empty()) { // 参数预览
        ImGui::Separator();
        int shown = 0;
        for (const engine::Param& param : node.params) {
            if (!engine::param_visible(node, param)) {
                continue; // 条件隐藏的参数不预览（如 web 模式下的 API Key / API 地址）
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
            ImGui::TextDisabled("%s", elide_to_width(line, g_content_width).c_str());
            ++shown;
        }
    }

    // ---- P7a-16：图片卡片（缩略图 + 尺寸 + 格式徽标；格式取内容嗅探结果）----
    if (const std::string card_image = node_card_image(node); !card_image.empty()) {
        const TextureInfo texture = texture_for(card_image);
        ImGui::Separator();
        if (texture.texture != 0) {
            float width  = static_cast<float>(texture.width);
            float height = static_cast<float>(texture.height);
            if (width > g_content_width && width > 0.0f) {
                const float shrink = g_content_width / width;
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
                               elide_to_width(reason, g_content_width).c_str());
        }
    }

    // ---- 生效提供商提示（P1-a：provider 输入优先；official 未接线 → 必定失败）----
    if (engine::uses_provider(node.type)) {
        const engine::EffectiveProvider effective =
            engine::resolve_effective_provider(editor().graph, node);
        const std::string reason = engine::unwired_reason(editor().graph, node);

        ImGui::Separator();
        if (node.type == "LLMGenerate") {
            // 固定宽度约束：来源节点标题长度不可控 → 按像素收口
            const std::string line =
                effective.from_edge
                    ? ("生效 " + effective.mode + " ← " + effective.source_node)
                    : ("生效 " + effective.mode + "（节点自身）");
            ImGui::TextDisabled("%s", elide_to_width(line, g_content_width).c_str());
        }
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            // 自绘折行（每行 ≤ 内容宽度），不靠 ImGui 自动换行 → 长文案不撑宽节点
            ImGui::TextUnformatted(
                wrap_to_width(reason + "：必定失败", g_content_width, kNodeSummaryLines).c_str());
            ImGui::PopStyleColor();
        }
    }

    std::vector<std::string> errors;
    editor().graph.validateParams(node, &errors);
    if (!errors.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验 %d 项错误",
                           static_cast<int>(errors.size()));
    }

    // ---- 运行结果摘要（PA-03：只读；与输出面板 / 参数面板同源）----
    const NodeResultView result_view = result_view_of(node);
    if (result_view.has_content) {
        ImGui::Separator();
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
            wrap_to_width(result_view.text, g_content_width, kNodeSummaryLines).c_str());
        ImGui::PopStyleColor();
    }

    ed::EndNode();
}

// ------------------------------------------------------------- 连线创建 -----
// 设计 §6.2：拖拽创建连接；类型不兼容变红并拒绝；输入端口已连接则替换
//
// 【重要契约】imgui-node-editor v0.9.3：ed::BeginCreate() 内部会调用
//   CreateItemAction::Begin()（其中 IM_ASSERT(false == m_InActive); m_InActive = true;），
//   只有 ed::EndCreate() 会把它复位为 false。因此 Begin/End **必须成对调用**，
//   不能用返回值提前 return（否则下一帧 Begin 时断言 IM_ASSERT(false == m_InActive) 失败）。
//   官方示例（widgets-example / blueprints-example）同样是无条件调用 EndCreate()。
void handle_create_link()
{
    EditorState& state = editor();

    const bool creating = ed::BeginCreate(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f);

    ed::PinId start_pin;
    ed::PinId end_pin;
    if (creating && ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
        PinRef from = resolve_pin(start_pin, state.graph);
        PinRef to   = resolve_pin(end_pin, state.graph);

        // 统一成"输出 → 输入"
        if (from.valid && to.valid && from.direction == PortDirection::Input &&
            to.direction == PortDirection::Output) {
            std::swap(from, to);
        }

        if (!from.valid || !to.valid) {
            ed::RejectNewItem(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), 2.0f);
        }
        else {
            bool        would_replace = false;
            std::string reason;
            const bool  ok = state.graph.canConnect(from.node_id, from.port_id, to.node_id,
                                                    to.port_id, &reason, &would_replace);
            if (!ok) {
                ed::RejectNewItem(ImVec4(1.0f, 0.30f, 0.30f, 1.0f), 2.0f);
                if (ImGui::IsMouseReleased(0)) {
                    log::warn("[连线] 被拒绝：" + reason);
                    state.set_status("连线被拒绝：" + reason);
                }
            }
            else {
                if (ed::AcceptNewItem(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f)) {
                    state.snapshot(would_replace ? "替换连线" : "新建连线");
                    std::string error;
                    const std::string edge_id = state.graph.addEdge(
                        from.node_id, from.port_id, to.node_id, to.port_id, &error);
                    if (edge_id.empty()) {
                        log::error("[连线] 创建失败：" + error);
                        state.set_status("连线失败：" + error);
                    }
                    else {
                        state.log_summary("[连线] " + from.node_id + "." + from.port_id + " → " +
                                          to.node_id + "." + to.port_id);
                        state.set_status("已连线 " + from.node_id + " → " + to.node_id +
                                         (would_replace ? "（替换了原连线）" : ""));
                    }
                }
            }
        }
    }

    ed::EndCreate();
}

// ------------------------------------------------------------- 删除处理 -----
// 设计 §6.1：删除节点（连带连线）；§6.2：删除连线。每个对象一次撤销快照
// 【重要契约】同 handle_create_link：BeginDelete()/EndDelete() 必须成对调用
void handle_delete_items()
{
    EditorState& state = editor();

    const bool deleting = ed::BeginDelete();

    if (deleting) {
        ed::LinkId deleted_link;
        while (ed::QueryDeletedLink(&deleted_link)) {
            if (ed::AcceptDeletedItem()) {
                const std::string edge_id = "e" + std::to_string(deleted_link.Get());
                state.snapshot("删除连线");
                if (state.graph.removeEdge(edge_id)) {
                    state.log_summary("[连线] 删除 " + edge_id);
                    state.set_status("已删除连线 " + edge_id);
                }
            }
        }

        ed::NodeId deleted_node;
        while (ed::QueryDeletedNode(&deleted_node)) {
            if (ed::AcceptDeletedItem()) {
                const std::string node_id = "n" + std::to_string(deleted_node.Get());
                state.snapshot("删除节点");
                if (state.delete_node(node_id)) {
                    state.log_summary("[节点] 删除 " + node_id);
                    state.set_status("已删除节点 " + node_id + "（含其连线）");
                }
            }
        }
    }

    ed::EndDelete();
}

// 位置安全上限：任何异常数值（FLT_MAX / NaN / 溢出）都不得进入绘图与视图计算
constexpr float kPositionLimit = 100000.0f;

float clamp_position(float value)
{
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -kPositionLimit, kPositionLimit);
}

// ------------------------------------------------------------- 状态同步 -----
void sync_selection(EditorState& state)
{
    const int count = ed::GetSelectedObjectCount();
    const std::size_t capacity = static_cast<std::size_t>(std::max(count, 0));

    // 选中的节点
    std::vector<ed::NodeId> node_handles(capacity);
    const int node_count = (count > 0) ? ed::GetSelectedNodes(node_handles.data(), count) : 0;
    state.selected_nodes.clear();
    for (int i = 0; i < node_count; ++i) {
        state.selected_nodes.push_back("n" +
                                       std::to_string(node_handles[static_cast<std::size_t>(i)].Get()));
    }

    // 选中的连线（左键点连线 / 框选都会选中连线 → 可与工具栏「删除选中」联动）
    std::vector<ed::LinkId> link_handles(capacity);
    const int link_count = (count > 0) ? ed::GetSelectedLinks(link_handles.data(), count) : 0;
    state.selected_links.clear();
    for (int i = 0; i < link_count; ++i) {
        state.selected_links.push_back("e" +
                                       std::to_string(link_handles[static_cast<std::size_t>(i)].Get()));
    }

    if (state.selected_nodes.size() == 1) {
        state.selected_node = state.selected_nodes.front();
    }
    else if (state.selected_node.empty() ||
             std::find(state.selected_nodes.begin(), state.selected_nodes.end(),
                       state.selected_node) == state.selected_nodes.end()) {
        state.selected_node.clear();
    }
}

// 位置同步（双向）：
//   · 编辑器已认识该节点  → 读回位置写进 Graph（对应拖拽移动）
//   · 编辑器还不认识（ed::GetNodePosition 对未知手柄返回 FLT_MAX，见 imgui_node_editor.cpp:1676）
//     → 用 ed::SetNodePosition 把 Graph 的位置推给编辑器，**绝不把脏值回写模型**
//   · 非有限值 / 超上限一律夹紧，避免溢出进入绘图与视图导航（历史"复制粘贴卡死"根因，见 CHANGELOG）
// 一次拖拽只压一次撤销快照
void sync_positions(EditorState& state)
{
    bool moved = false;
    for (Node& node : state.graph.nodes) {
        const ImVec2 position = ed::GetNodePosition(node_handle(node.id));

        const bool editor_knows =
            std::isfinite(position.x) && std::isfinite(position.y) &&
            std::fabs(position.x) < kPositionLimit && std::fabs(position.y) < kPositionLimit;

        if (!editor_knows) {
            ed::SetNodePosition(node_handle(node.id), ImVec2(node.x, node.y)); // 模型 → 编辑器
            continue;
        }

        if (std::fabs(position.x - node.x) > 0.5f || std::fabs(position.y - node.y) > 0.5f) {
            if (!g_drag_in_progress) {
                g_drag_in_progress = true;
                state.snapshot("移动节点");
            }
            node.x = clamp_position(position.x);
            node.y = clamp_position(position.y);
            moved  = true;
        }
    }
    if (!moved) {
        g_drag_in_progress = false; // 拖拽结束
    }
}

// --------------------------------------------------- 右键菜单：新建节点 ------
void show_add_node_menu(EditorState& state, const ImVec2& spawn)
{
    engine::registerAllNodes();
    engine::NodeRegistry& registry = engine::NodeRegistry::instance();

    const NodeCategory categories[] = {NodeCategory::Input,  NodeCategory::Process,
                                       NodeCategory::Config, NodeCategory::Inference,
                                       NodeCategory::Output};
    for (NodeCategory category : categories) {
        if (!ImGui::BeginMenu(engine::categoryName(category))) {
            continue;
        }
        for (const engine::Definition* definition : registry.listByCategory(category)) {
            if (ImGui::MenuItem(definition->display_name.c_str())) {
                state.snapshot(std::string("创建节点 ") + definition->display_name);
                const std::string id = state.create_node(definition->type, spawn.x, spawn.y);
                if (!id.empty()) {
                    state.selected_nodes = {id};
                    state.selected_node  = id;
                }
            }
            if (ImGui::IsItemHovered() && !definition->description.empty()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(definition->description.c_str());
                ImGui::TextDisabled("输入 %d 个 / 输出 %d 个 / 参数 %d 项",
                                    static_cast<int>(definition->inputs.size()),
                                    static_cast<int>(definition->outputs.size()),
                                    static_cast<int>(definition->params.size()));
                ImGui::EndTooltip();
            }
        }
        ImGui::EndMenu();
    }
}

// ------------------------------------------------- 宽度自检（不变量）----
// 画布上**所有节点必须等宽**：宽度 = g_content_width + 节点内边距（g_content_width 由「输入/输出
// 标签实测最宽」算出、全画布共用）。任何文本项漏了收口、或新加了无界控件，都会在这里被抓到。
// 尺寸 / 节点数一变就写一条 INFO（可直接拿去核对），破坏不变量时写 WARN（限流 2 秒一条）。
std::string px_text(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(value));
    return std::string(buffer);
}

void check_uniform_node_width(const Graph& graph)
{
    static int         frame          = 0;
    static std::string last_signature; // 上一次采样到的「尺寸指纹」
    static double      last_warn_time = 0.0;
    if (++frame < 3) {
        return; // 前两帧节点尺寸尚未结算（新建 / 刚加载图）
    }

    float       min_w = 0.0f;
    float       max_w = 0.0f;
    std::string min_id;
    std::string max_id;
    int         counted = 0;
    for (const Node& node : graph.nodes) {
        const ImVec2 size = ed::GetNodeSize(node_handle(node.id));
        if (size.x <= 0.0f) {
            continue; // 编辑器本帧才认识该节点 → 尺寸还没结算
        }
        ++counted;
        if (min_w == 0.0f || size.x < min_w) {
            min_w  = size.x;
            min_id = node.id;
        }
        if (size.x > max_w) {
            max_w  = size.x;
            max_id = node.id;
        }
    }
    if (counted < 2) {
        return;
    }

    const bool        uniform = (max_w - min_w) < 0.5f;
    const std::string signature =
        std::to_string(counted) + "/" + px_text(min_w) + "/" + px_text(max_w);
    if (signature != last_signature) {
        last_signature = signature; // 尺寸/节点数一变就重新采样并留证（换工作流也能被记录）
        log::info("[画布] 节点宽度自检：" + std::to_string(counted) + " 个节点，宽 " + px_text(min_w) +
                  " ~ " + px_text(max_w) + " px（内容区 " + px_text(g_content_width) +
                  " px · 按输入/输出标签自适应；最窄 " + min_id + " / 最宽 " + max_id + "）→ " +
                  (uniform ? "全部等宽 ✅" : "宽度不一致 ❌"));
    }
    if (!uniform) { // 破坏不变量：限流告警（最多 2 秒一条）
        const double now = ImGui::GetTime();
        if (now - last_warn_time > 2.0) {
            last_warn_time = now;
            log::warn("[画布] 节点宽度不一致（「固定宽度」不变量被破坏）：最窄 " + min_id + " = " +
                      px_text(min_w) + " px，最宽 " + max_id + " = " + px_text(max_w) +
                      " px —— 检查是否有文本未走 elide_to_width()/wrap_to_width() 收口");
        }
    }
}

// 折行自检（一次性 · 首次绘制时跑一遍）：用「超长无空格串 / 纯中文 / 中英混排 / 自带换行」四类样例
// 验证 wrap_to_width 的每条输出行都放得下 —— 这是「节点宽度固定」的构造性证据（不依赖人工造数据）。
void log_wrap_selftest()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    const std::vector<std::string> samples = {
        std::string(400, 'x'), // 超长无空格拉丁串（英文单词不会自动断行的经典反例）
        "这是一段没有任何空格的超长中文文本，用来验证中文也能在任意字符处断行，而不会把节点撑宽。",
        "中文English混排12345，测试mixed-width文本的折行边界是否正确。",
        "第一行\n第二行很长很长很长很长很长很长很长很长很长很长很长很长很长很长\n第三行",
    };

    float worst = 0.0f;
    for (const std::string& sample : samples) {
        const std::string wrapped = wrap_to_width(sample, g_content_width, kNodeSummaryLines);
        std::size_t       begin   = 0;
        while (true) {
            const std::size_t end  = wrapped.find('\n', begin);
            const std::string line = (end == std::string::npos)
                                         ? wrapped.substr(begin)
                                         : wrapped.substr(begin, end - begin);
            worst                  = (std::max)(worst, ImGui::CalcTextSize(line.c_str()).x);
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    log::info("[画布] 折行自检：" + std::to_string(samples.size()) + " 个样例，最长行 " + px_text(worst) +
              " px（上限 " + px_text(g_content_width) + " px）→ " +
              (worst <= g_content_width + 0.5f ? "全部收口 ✅" : "有行超宽 ❌"));
}

} // namespace

void draw_node_canvas(const char* title, CanvasOptions& options)
{
    ensure_context();
    EditorState& state = editor();

    static ImVec2      spawn_position = ImVec2(0.0f, 0.0f);
    static std::string context_node;

    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("输入端口在节点左侧、输出端口在右侧 ｜ 拖左侧圆点连线，类型不符会变红被拒");
    ImGui::SameLine();
    ImGui::TextDisabled("｜ 右键：画布新建 / 节点复制·删除 / 连线删除 ｜ 左键点连线可选中，再点工具栏「删除选中」");
    ImGui::Separator();

    ed::SetCurrentEditor(g_context);
    ed::Begin("##NodeCanvas", ImVec2(0.0f, 0.0f));

    // 视图自愈：损坏的设置文件可能残留 ±3e9 视图 / 2e-7 缩放，会让网格与剔除计算爆量
    // （表现为 CPU 单核打满、界面无响应）。检测到异常就重置为"跟随内容"。
    {
        const float  zoom   = ed::GetCurrentZoom();
        const ImVec2 origin = ed::ScreenToCanvas(ImVec2(0.0f, 0.0f));
        const bool zoom_bad = !std::isfinite(zoom) || zoom < 0.05f || zoom > 10.0f;
        const bool origin_bad = !std::isfinite(origin.x) || !std::isfinite(origin.y) ||
                                std::fabs(origin.x) > 1.0e6f || std::fabs(origin.y) > 1.0e6f;
        if (zoom_bad || origin_bad) {
            ed::NavigateToContent(0.0f);
            log::warn("[画布] 检测到异常视图（zoom=" + std::to_string(zoom) +
                      "，原点 x=" + std::to_string(origin.x) + "），已重置为跟随内容");
            state.set_status("已重置画布视图（检测到异常缩放/原点）");
        }
    }

    if (!options.show_grid) {
        ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    // 位置同步：新建 / 粘贴的节点在这里落到 Graph 记录的位置
    for (const Node& node : state.graph.nodes) {
        ed::SetNodePosition(node_handle(node.id), ImVec2(node.x, node.y));
    }
    // 宽度自适应：按当前图的「输入/输出标签实测最宽」算出**全画布共用的内容区宽度**（每帧重算，代价可忽略）
    update_content_width(state.graph);
    for (const Node& node : state.graph.nodes) {
        draw_node_body(node);
    }
    log_wrap_selftest();                   // 折行自检（一次性：每条输出行都必须放得下）
    check_uniform_node_width(state.graph); // 固定宽度自检（不变量：全部节点等宽）
    for (const Edge& edge : state.graph.edges) {
        const Node* from = state.graph.findNode(edge.from_node);
        const Node* to   = state.graph.findNode(edge.to_node);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        const int from_index = from->portIndex(edge.from_port, PortDirection::Output);
        const int to_index   = to->portIndex(edge.to_port, PortDirection::Input);
        if (from_index < 0 || to_index < 0) {
            continue;
        }
        ed::Link(link_handle(edge.id), pin_handle(*from, PortDirection::Output, from_index),
                 pin_handle(*to, PortDirection::Input, to_index));
    }

    handle_create_link();
    handle_delete_items();

    // ------------------------------------------------- 右键菜单（需挂起画布）----
    static ed::NodeId context_node_handle; // static：避免每帧重置（历史 bug 根因，见 CHANGELOG）

    ed::Suspend();

    // 右键菜单按「最具体优先」串联（官方 blueprints-example 写法）：节点 > 背景。
    // 说明：连线右键菜单（「删除连线」「选中该连线」）已按需求下线，见 CHANGELOG「连线右键菜单移除」；
    //       连线删除请用「左键单击连线选中 → 工具栏【删除选中】」。
    if (ed::ShowNodeContextMenu(&context_node_handle)) {
        context_node         = "n" + std::to_string(context_node_handle.Get());
        state.selected_nodes = {context_node};
        state.selected_node  = context_node;
        ImGui::OpenPopup("##node_menu");
    }
    else if (ed::ShowBackgroundContextMenu()) {
        spawn_position = ed::ScreenToCanvas(ImGui::GetMousePos());
        ImGui::OpenPopup("##canvas_menu");
    }

    if (ImGui::BeginPopup("##canvas_menu")) {
        ImGui::TextDisabled("新建节点");
        ImGui::Separator();
        show_add_node_menu(state, spawn_position);
        ImGui::Separator();
        // 设计 §6.1：右键画布 → 粘贴
        if (ImGui::MenuItem("粘贴", nullptr, false, state.has_clipboard())) {
            state.paste_clipboard();
        }
        if (ImGui::IsItemHovered() && !state.has_clipboard()) {
            ImGui::SetTooltip("剪贴板为空：先在节点右键菜单里「复制」");
        }
        if (ImGui::MenuItem("删除选中（节点 / 连线）", nullptr, false, state.has_selection())) {
            state.delete_selected();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##node_menu")) {
        ImGui::TextDisabled("节点 %s", context_node.c_str());
        ImGui::Separator();
        // 设计 §6.1：右键节点 → 复制（粘贴在画布右键菜单）
        if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
            state.copy_selection();
        }
        if (ImGui::MenuItem("改名（右侧参数面板）")) {
            state.request_focus_title = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("删除该节点")) {
            state.delete_selected();
        }
        ImGui::EndPopup();
    }

    ed::Resume();
    // 双击节点标题 → 参数面板聚焦标题（设计 §6.1 重命名）
    const ed::NodeId double_clicked = ed::GetDoubleClickedNode();
    if (double_clicked.Get() != 0) {
        state.selected_nodes      = {"n" + std::to_string(double_clicked.Get())};
        state.selected_node       = state.selected_nodes.front();
        state.request_focus_title = true;
        state.set_status("可在右侧参数面板修改标题与参数：" + state.selected_node);
    }

    sync_selection(state);

    // 视图跟随前先确认坐标正常（历史卡死根因：异常坐标 + NavigateToContent 会让视图落到天文数字区间）
    const bool positions_sane =
        state.graph.nodes.empty() ||
        std::all_of(state.graph.nodes.begin(), state.graph.nodes.end(), [](const Node& node) {
            return std::isfinite(node.x) && std::isfinite(node.y) &&
                   std::fabs(node.x) < kPositionLimit && std::fabs(node.y) < kPositionLimit;
        });

    if (g_first_frame || state.request_navigate_to_content) {
        if (positions_sane) {
            ed::NavigateToContent(0.0f);
        }
        else {
            log::warn("[画布] 跳过视图跟随：存在异常节点坐标（已按上限夹紧）");
            state.set_status("已跳过视图跟随（检测到异常节点坐标）");
        }
        g_first_frame                     = false;
        state.request_navigate_to_content = false;
    }

    // P7a-17：错误条点击「定位」→ 视图**居中并放大**到选中节点（不是仅跟随全部内容）
    if (state.request_focus_selection) {
        if (positions_sane) {
            ed::NavigateToSelection(true);
        }
        state.request_focus_selection = false;
    }

    ed::End();

    // P7a-14：空画布引导（新用户第一眼就知道下一步做什么）
    if (state.graph.nodes.empty()) {
        ImDrawList*  draw_list   = ImGui::GetWindowDrawList();
        const ImVec2 window_pos  = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const char*  line1 = "画布是空的：从左侧「节点库」单击一个节点，或直接拖到画布";
        const char*  line2 = "也可以右键画布 → 新建节点；菜单「文件 → 示例工作流」可载入示例";
        const ImVec2 size1 = ImGui::CalcTextSize(line1);
        const ImVec2 size2 = ImGui::CalcTextSize(line2);
        const float  center_x = window_pos.x + window_size.x * 0.5f;
        const float  base_y   = window_pos.y + window_size.y * 0.42f;
        draw_list->AddText(ImVec2(center_x - size1.x * 0.5f, base_y),
                           IM_COL32(205, 205, 205, 220), line1);
        draw_list->AddText(ImVec2(center_x - size2.x * 0.5f, base_y + size1.y + 6.0f),
                           IM_COL32(150, 150, 150, 200), line2);
    }

    if (!options.show_grid) {
        ed::PopStyleColor();
    }

    sync_positions(state);
    ed::SetCurrentEditor(nullptr);

    options.node_count = static_cast<int>(state.graph.nodes.size());
    options.link_count = static_cast<int>(state.graph.edges.size());

    ImGui::End();
}

std::string canvas_add_node_at_center(const std::string& type)
{
    ensure_context();
    EditorState& state = editor();

    ed::SetCurrentEditor(g_context);
    const ImVec2 center_canvas = ed::ScreenToCanvas(ImGui::GetMainViewport()->GetCenter());
    ed::SetCurrentEditor(nullptr);

    state.snapshot("创建节点");
    const std::string id = state.create_node(type, center_canvas.x, center_canvas.y);
    if (!id.empty()) {
        state.selected_nodes = {id};
        state.selected_node  = id;
    }
    return id;
}

} // namespace aiwrite::ui
