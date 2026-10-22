// ce_repro.cpp —— 复现「新建上级节点 → 编辑器首帧」的 ImGui 窗口栈断言
//   IM_ASSERT_USER_ERROR(g.CurrentWindowStack.Size == 1, "Mismatched Begin/BeginChild vs End/EndChild calls...")
//
// 用法：ce_repro [mode]
//   mode 0: 忠实模拟 draw_composite_editor（BeginChild host + Suspend/Resume + 规格面板）
//   mode 1: 无规格面板（仅 canvas host）
//   mode 2: 无 BeginChild host（ed::Begin 直接放在编辑器窗口里）
//   mode 3: pack 风格（复合图里有内部节点/连线）→ 应成功
//   mode 4: 主画布未正确结束（漏 SetCurrentEditor(nullptr)）
//   mode 5: 小地图导航机制验证（headless）：复刻上级编辑器 ce_block_minimap_input +
//           ce_draw_minimap 完整链路，模拟点击小地图 → 断言下一帧画布视图中心移动到目标点。
#include <imgui.h>
#include <imgui_node_editor.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ed = ax::NodeEditor;

static ed::EditorContext* g_main_ctx = nullptr;
static ed::EditorContext* g_ce_ctx   = nullptr;

static void ensure_main()
{
    if (g_main_ctx != nullptr) {
        return;
    }
    ed::Config config;
    config.SettingsFile = nullptr;
    g_main_ctx          = ed::CreateEditor(&config);
    ed::SetCurrentEditor(g_main_ctx);
    ed::EnableShortcuts(false);
}

static void ensure_ce()
{
    if (g_ce_ctx != nullptr) {
        return;
    }
    ed::Config config;
    config.SettingsFile = nullptr; // 内部图编辑器不落盘设置
    g_ce_ctx            = ed::CreateEditor(&config);
    ed::SetCurrentEditor(g_ce_ctx);
    ed::EnableShortcuts(false);
}

static void draw_main_canvas(bool end_properly)
{
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
    if (!ImGui::Begin("##MainCanvas")) {
        ImGui::End();
        return;
    }
    ed::SetCurrentEditor(g_main_ctx);
    ed::Begin("##NodeCanvas", ImVec2(0.0f, 0.0f));
    ed::BeginNode(ed::NodeId(1));
    ImGui::Text("A");
    ImGui::Text("B");
    ed::EndNode();
    ed::End();
    ed::SetCurrentEditor(nullptr);
    if (!end_properly) {
        // mode 4：模拟「没有正确结束当前 active 画布」——不还原 current editor
        ed::SetCurrentEditor(g_main_ctx);
    }
    ImGui::End();
}

static void drawCompositeEditor(int mode, bool empty)
{
    ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 640.0f));
    bool open = true;
    if (!ImGui::Begin("##CompositeEditor", &open)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("id: ce_1");
    ImGui::Separator();

    const float spec_panel_w = 320.0f;
    if (mode != 2) {
        ImGui::BeginChild("##ce_canvas_host",
                          ImVec2(ImGui::GetContentRegionAvail().x - spec_panel_w, 0.0f));
    }
    {
        ensure_ce();
        ed::SetCurrentEditor(g_ce_ctx);

        ed::Begin("##CeCanvas", ImVec2(0.0f, 0.0f));

        if (!empty) {
            // mode 3：pack 风格——内部图有节点
            ed::BeginNode(ed::NodeId(11));
            ImGui::Text("inner A");
            ed::EndNode();
        }

        // 拖出连线（与 draw_composite_editor 相同）
        const bool creating = ed::BeginCreate(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), 2.0f);
        if (creating) {
            ed::PinId start_pin;
            ed::PinId end_pin;
            if (ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
                ed::RejectNewItem(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);
            }
        }
        ed::EndCreate();

        ed::BeginDelete();
        {
            ed::LinkId link_id;
            ed::NodeId node_id;
            if (ed::QueryDeletedLink(&link_id)) {
                ed::RejectDeletedItem();
            }
            if (ed::QueryDeletedNode(&node_id)) {
                ed::RejectDeletedItem();
            }
        }
        ed::EndDelete();

        ed::Suspend();
        if (ed::ShowBackgroundContextMenu()) {
            ImGui::OpenPopup("##ce_menu");
        }
        if (ImGui::BeginPopup("##ce_menu")) {
            ImGui::TextDisabled("新建节点");
            ImGui::Separator();
            if (ImGui::MenuItem("加一个内部节点")) {
            }
            ImGui::EndPopup();
        }
        ed::Resume();

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }
    if (mode != 2) {
        ImGui::EndChild();

        if (mode != 1) {
            ImGui::SameLine();
            ImGui::BeginChild("##ce_spec_panel", ImVec2(spec_panel_w, 0.0f));
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 1.0f, 1.0f), "规格标记");
            if (empty) {
                ImGui::TextWrapped("内部图为空：右键画布添加内部节点。");
            }
            ImGui::EndChild();
        }
    }

    ImGui::End();
}

// ----------------------------------------------------------------------------
// mode 5：小地图导航机制验证（headless）
// 复刻 composite_editor.cpp 的 ce_minimap_rect / ce_minimap_hit /
// ce_block_minimap_input / ce_draw_minimap（逻辑逐行一致，主题色简化为灰）。
// 目标：验证「ed::End() 之后 ed::NavigateToCenter」在第二个编辑器上下文里
// 是否真的能在下一帧移动画布视图（隔离「导航机制」vs「输入命中」两类故障）。
// ----------------------------------------------------------------------------
namespace {

bool   g_m5_blocking = false;
bool   g_m5_clicked  = false;
bool   g_m5_down     = false;
ImVec2 g_m5_mouse    = ImVec2(0.0f, 0.0f);
bool   g_m5_simulate = false; // 某帧模拟一次点击
ImVec2 g_m5_target   = ImVec2(-9999.0f, -9999.0f); // 小地图点击换算的画布目标
bool   g_m5_target_set = false;
ImVec2 g_m5_view_center = ImVec2(-9999.0f, -9999.0f); // 本帧画布中心对应画布坐标

void m5_minimap_rect(ImVec2& out_min, ImVec2& out_max)
{
    const ImVec2 window_pos  = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float  margin      = 10.0f;
    out_min = ImVec2(window_pos.x + window_size.x - 190.0f - margin,
                     window_pos.y + window_size.y - 130.0f - margin);
    out_max = ImVec2(out_min.x + 190.0f, out_min.y + 130.0f);
}

bool m5_minimap_hit(const ImVec2& mouse, const ImVec2& min, const ImVec2& max)
{
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

void m5_block_minimap_input()
{
    g_m5_blocking = false;
    g_m5_clicked  = false;
    g_m5_down     = false;

    ImVec2 map_min, map_max;
    m5_minimap_rect(map_min, map_max);
    ImGuiIO&     io    = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    if (!m5_minimap_hit(mouse, map_min, map_max)) {
        return;
    }
    g_m5_blocking = true;
    g_m5_mouse    = mouse;
    g_m5_clicked  = io.MouseClicked[0];
    g_m5_down     = io.MouseDown[0];
    for (int i = 0; i < 5; ++i) {
        io.MouseClicked[i]  = false;
        io.MouseDown[i]     = false;
        io.MouseReleased[i] = false;
    }
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MouseDelta  = ImVec2(0.0f, 0.0f);
}

// 内部图单节点（固定坐标，与 mode 3 同款布局）
struct M5Node {
    ImVec2 pos;
    ImVec2 size;
};

void m5_draw_minimap(const M5Node& node)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImVec2 map_min, map_max;
    m5_minimap_rect(map_min, map_max);
    draw_list->AddRectFilled(map_min, map_max, IM_COL32(18, 20, 26, 215), 4.0f);
    draw_list->AddRect(map_min, map_max, IM_COL32(130, 130, 145, 210), 4.0f, 0, 1.0f);
    draw_list->AddText(map_min + ImVec2(6.0f, 3.0f), IM_COL32(200, 200, 210, 235), "小地图");

    const ImVec2 area_min(map_min.x + 6.0f, map_min.y + 20.0f);
    const ImVec2 area_max(map_max.x - 6.0f, map_max.y - 6.0f);

    ImVec2 bbox_min = node.pos;
    ImVec2 bbox_max = node.pos + node.size;
    const ImVec2 bbox_size(std::max(bbox_max.x - bbox_min.x, 1.0f),
                           std::max(bbox_max.y - bbox_min.y, 1.0f));
    const ImVec2 area_size(area_max.x - area_min.x, area_max.y - area_min.y);
    const float  scale = std::min(area_size.x / bbox_size.x, area_size.y / bbox_size.y);

    const float  w = std::max(node.size.x * scale, 3.0f);
    const float  h = std::max(node.size.y * scale, 3.0f);
    const ImVec2 p(area_min.x + (node.pos.x - bbox_min.x) * scale,
                   area_min.y + (node.pos.y - bbox_min.y) * scale);
    draw_list->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h),
                             IM_COL32(190, 195, 220, 200), 1.0f);

    // 命中区域 → NavigateToCenter（与上级编辑器一致；目标点供断言）
    if (g_m5_blocking && (g_m5_clicked || g_m5_down) &&
        m5_minimap_hit(g_m5_mouse, area_min, area_max)) {
        const float cx = (g_m5_mouse.x - area_min.x) / scale + bbox_min.x;
        const float cy = (g_m5_mouse.y - area_min.y) / scale + bbox_min.y;
        ed::NavigateToCenter(ImVec2(cx, cy), 0.0f);
        g_m5_target     = ImVec2(cx, cy);
        g_m5_target_set = true;
        std::printf("[m5] NavigateToCenter target=canvas(%.1f, %.1f)\n", cx, cy);
    }
}

void m5_draw_composite_editor(int frame)
{
    ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 640.0f));
    bool open = true;
    if (!ImGui::Begin("##CompositeEditor", &open)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("id: ce_1");
    ImGui::Separator();

    const float spec_panel_w = 320.0f;
    ImGui::BeginChild("##ce_canvas_host",
                      ImVec2(ImGui::GetContentRegionAvail().x - spec_panel_w, 0.0f));
    {
        ensure_ce();
        ed::SetCurrentEditor(g_ce_ctx);

        // 模拟一次点击：鼠标放小地图中心，左键本帧按下
        if (g_m5_simulate) {
            g_m5_simulate = false;
            ImVec2 map_min, map_max;
            m5_minimap_rect(map_min, map_max);
            ImGuiIO& io          = ImGui::GetIO();
            io.MousePos          = ImVec2(0.5f * (map_min.x + map_max.x),
                                          0.5f * (map_min.y + map_max.y));
            io.MouseClicked[0]   = true;
            io.MouseDown[0]      = true;
            std::printf("[m5] 模拟点击屏幕(%.1f, %.1f)（小地图中心）\n", io.MousePos.x, io.MousePos.y);
        }

        m5_block_minimap_input(); // 与上级编辑器一致：ed::Begin 之前

        ed::Begin("##CeCanvas", ImVec2(0.0f, 0.0f));

        // 内部图一个节点
        ed::BeginNode(ed::NodeId(11));
        ImGui::Text("inner A");
        ImGui::Text("B");
        ed::EndNode();
        const ImVec2 node_size = ed::GetNodeSize(ed::NodeId(11));

        // 画布中心当前对应的画布坐标
        const ImVec2 child_center = ImGui::GetWindowPos() + ImGui::GetWindowSize() * 0.5f;
        const ImVec2 view_center  = ed::ScreenToCanvas(child_center);
        g_m5_view_center = view_center;
        if (frame >= 2) {
            std::printf("[m5] frame %d view_center=canvas(%.1f, %.1f)\n", frame, view_center.x,
                        view_center.y);
        }

        ed::End();

        M5Node node;
        node.pos  = ImVec2(100.0f, 100.0f);
        node.size = ImVec2(std::max(node_size.x, 80.0f), std::max(node_size.y, 40.0f));
        // 与上级编辑器一致：ed::End() 之后、SetCurrentEditor(nullptr) 之前
        m5_draw_minimap(node);

        ed::SetCurrentEditor(nullptr);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##ce_spec_panel", ImVec2(spec_panel_w, 0.0f));
    ImGui::TextColored(ImVec4(0.8f, 0.7f, 1.0f, 1.0f), "规格标记");
    ImGui::EndChild();

    ImGui::End();
}

} // namespace

int main(int argc, char** argv)
{
    int mode = 0;
    if (argc > 1) {
        mode = std::atoi(argv[1]);
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    // headless：无 GLFW/OpenGL 后端，须手动构建字体图集，否则 Render() 断言
    // "Font Atlas not built!"（与窗口栈问题无关，属正常 ImGui 前置条件）
    io.Fonts->AddFontDefault();
    {
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ensure_main(); // 主画布上下文在启动时创建（同 app）

    bool ce_active = false;
    int  new_frame = 3;

    if (mode == 5) {
        // mode 5：小地图导航机制验证——上级编辑器从首帧就打开
        ce_active = true;
        g_m5_simulate = true; // 首帧就模拟点击（稳定布局下隔离「输入命中」）
        // 点击目标点与下一帧视图中心（跨帧断言）
        ImVec2 nav_target = ImVec2(-9999.0f, -9999.0f);
        ImVec2 view_after = ImVec2(-9999.0f, -9999.0f);

        for (int f = 0; f < 10; ++f) {
            ImGui::NewFrame();

            draw_main_canvas(/*end_properly=*/true);

            g_m5_view_center = ImVec2(-9999.0f, -9999.0f);
            m5_draw_composite_editor(f);
            if (g_m5_target_set) {
                nav_target = g_m5_target;
                g_m5_target_set = false;
            }
            if (g_m5_view_center.x > -9000.0f) {
                view_after = g_m5_view_center;
            }

            ImGui::EndFrame();
            ImGui::Render();
        }

        const bool ok = nav_target.x > -9000.0f && view_after.x > -9000.0f &&
                        std::abs(view_after.x - nav_target.x) < 1.0f &&
                        std::abs(view_after.y - nav_target.y) < 1.0f;
        std::printf("mode5: nav_target=canvas(%.1f, %.1f) view_after=canvas(%.1f, %.1f)\n",
                    nav_target.x, nav_target.y, view_after.x, view_after.y);
        std::printf("mode5: %s\n", ok ? "PASS —— NavigateToCenter(after ed::End) 能移动视图"
                                      : "FAIL —— 导航未生效");
        ImGui::DestroyContext();
        return ok ? 0 : 1;
    }

    for (int f = 0; f < 12; ++f) {
        ImGui::NewFrame();

        draw_main_canvas(/*end_properly=*/mode != 4);

        // 第 new_frame 帧：模拟「新建对话框 → 点击创建并打开编辑器」→ active = true
        if (f == new_frame) {
            ce_active = true;
        }

        if (ce_active) {
            drawCompositeEditor(mode, /*empty=*/mode != 3);
        }

        ImGui::EndFrame();
        ImGui::Render();

        std::printf("frame %d ok\n", f);
    }

    ImGui::DestroyContext();
    std::printf("ALL FRAMES OK (mode=%d)\n", mode);
    return 0;
}
