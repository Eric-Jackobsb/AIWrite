// ce_repro.cpp —— 复现「新建上级节点 → 编辑器首帧」的 ImGui 窗口栈断言
//   IM_ASSERT_USER_ERROR(g.CurrentWindowStack.Size == 1, "Mismatched Begin/BeginChild vs End/EndChild calls...")
//
// 用法：ce_repro [mode]
//   mode 0: 忠实模拟 draw_composite_editor（BeginChild host + Suspend/Resume + 规格面板）
//   mode 1: 无规格面板（仅 canvas host）
//   mode 2: 无 BeginChild host（ed::Begin 直接放在编辑器窗口里）
//   mode 3: pack 风格（复合图里有内部节点/连线）→ 应成功
//   mode 4: 主画布未正确结束（漏 SetCurrentEditor(nullptr)）
#include <imgui.h>
#include <imgui_node_editor.h>

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

static void draw_composite_editor(int mode, bool empty)
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

    for (int f = 0; f < 12; ++f) {
        ImGui::NewFrame();

        draw_main_canvas(/*end_properly=*/mode != 4);

        // 第 new_frame 帧：模拟「新建对话框 → 点击创建并打开编辑器」→ active = true
        if (f == new_frame) {
            ce_active = true;
        }

        if (ce_active) {
            draw_composite_editor(mode, /*empty=*/mode != 3);
        }

        ImGui::EndFrame();
        ImGui::Render();

        std::printf("frame %d ok\n", f);
    }

    ImGui::DestroyContext();
    std::printf("ALL FRAMES OK (mode=%d)\n", mode);
    return 0;
}
