#include "composite_selftest.h"

#include "composite.h"
#include "executor.h"
#include "nodes.h"
#include "node_registry.h"
#include "validate.h"
#include "workflow_io.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct Check {
    int passed = 0;
    int failed = 0;
};

void expect(Check& check, bool ok, const std::string& name, const std::string& detail = {})
{
    if (ok) {
        ++check.passed;
        std::printf("   PASS  %s\n", name.c_str());
    }
    else {
        ++check.failed;
        std::printf("   FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ->  ",
                    detail.c_str());
    }
}

std::string add_node(Check& check, aiwrite::engine::Graph& graph, const std::string& type,
                     const std::string& name)
{
    std::string error;
    const std::string id = graph.addNode(type, 0.0f, 0.0f, &error);
    expect(check, !id.empty(), name, error);
    return id;
}

// 构造一个可运行的上级节点规格：单个 PromptTemplate
//  * 暴露输入：vars（变量，variadic+optional）
//  * 暴露输出：text（结果）
//  * 暴露参数：template（模板）
aiwrite::engine::CompositeSpec make_selftest_spec()
{
    aiwrite::engine::Graph internal;
    const std::string pt_id = internal.addNode("PromptTemplate", 0.0f, 0.0f, nullptr);

    aiwrite::engine::CompositeSpec spec;
    spec.id          = "c1";
    spec.name        = "自检上级";
    spec.description = "composite_selftest 专用";

    aiwrite::engine::CompositePortRef in_ref;
    in_ref.internal_node = pt_id;
    in_ref.internal_port = "vars";
    in_ref.display_name  = "变量";
    in_ref.type          = aiwrite::engine::PortType::Any;
    spec.inputs.push_back(in_ref);

    aiwrite::engine::CompositePortRef out_ref;
    out_ref.internal_node = pt_id;
    out_ref.internal_port = "text";
    out_ref.display_name  = "结果";
    out_ref.type          = aiwrite::engine::PortType::Text;
    spec.outputs.push_back(out_ref);

    aiwrite::engine::CompositeParamRef p_ref;
    p_ref.internal_node = pt_id;
    p_ref.param_id      = "template";
    p_ref.display_name  = "模板";
    spec.exposed_params.push_back(p_ref);

    spec.internal_graph = std::move(internal);
    return spec;
}

} // namespace

int composite_selftest()
{
    namespace eng = aiwrite::engine;

    Check check;
    std::printf("[CN] 上级节点（Composition_node）自检\n");
    eng::registerAllNodes();
    aiwrite::nodes::registerAllExecutors();

    // 独立临时根目录（不触碰用户数据）
    std::error_code ec;
    const auto temp_root =
        std::filesystem::temp_directory_path(ec) / "aiwrite_composite_selftest";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    eng::CompositeRegistry::instance().setRoot(temp_root);

    // ---------------------------------------------------- 1. 规格校验 ----
    {
        const eng::CompositeSpec spec = make_selftest_spec();
        std::string error;
        expect(check, eng::validate_composite_spec(spec, &error), "规格校验：合法规格通过", error);

        // 嵌套拒绝
        eng::CompositeSpec nested = spec;
        eng::Node cn;
        cn.id = "n9";
        cn.type = "Composite";
        cn.composite_id = "c1";
        nested.internal_graph.nodes.push_back(cn);
        std::string nest_error;
        expect(check, !eng::validate_composite_spec(nested, &nest_error) &&
                          nest_error.find("嵌套") != std::string::npos,
               "规格校验：嵌套 Composite 被拒绝", nest_error);

        // 输出必须唯一（强制单输出）
        eng::CompositeSpec no_out = spec;
        no_out.outputs.clear();
        expect(check, !eng::validate_composite_spec(no_out, nullptr), "规格校验：无输出被拒绝");
        eng::CompositeSpec two_out = spec;
        two_out.outputs.push_back(two_out.outputs.front());
        expect(check, !eng::validate_composite_spec(two_out, nullptr), "规格校验：双输出被拒绝");

        // 引用端口 / 参数不存在
        eng::CompositeSpec bad_ref = spec;
        bad_ref.outputs[0].internal_port = "no_such_port";
        expect(check, !eng::validate_composite_spec(bad_ref, nullptr),
               "规格校验：引用端口不存在被拒绝");
        eng::CompositeSpec bad_param = spec;
        bad_param.exposed_params[0].param_id = "no_such_param";
        expect(check, !eng::validate_composite_spec(bad_param, nullptr),
               "规格校验：引用参数不存在被拒绝");

        // 暴露参数 id 重复
        eng::CompositeSpec dup_param = spec;
        dup_param.exposed_params.push_back(dup_param.exposed_params.front());
        expect(check, !eng::validate_composite_spec(dup_param, nullptr),
               "规格校验：暴露参数 id 重复被拒绝");

        // 标记输入端口已有内部入边（展开后重复占用）
        eng::CompositeSpec in_edge = make_selftest_spec();
        const std::string  src_id =
            in_edge.internal_graph.addNode("TextInput", 0, 0, nullptr);
        if (eng::Param* p = in_edge.internal_graph.findNode(src_id)->findParam("text")) {
            p->value = "内部文本";
        }
        const std::string pt_id = in_edge.inputs[0].internal_node;
        in_edge.internal_graph.addEdge(src_id, "text", pt_id, "vars", nullptr);
        std::string ie_error;
        expect(check, !eng::validate_composite_spec(in_edge, &ie_error) &&
                          ie_error.find("内部连线") != std::string::npos,
               "规格校验：标记输入端口已有内部入边被拒绝", ie_error);
    }

    // ---------------------------------------------------- 2. JSON 往返 ----
    {
        const eng::CompositeSpec spec = make_selftest_spec();
        const nlohmann::json     json = eng::compositeToJson(spec);
        eng::CompositeSpec       round;
        std::string              error;
        expect(check, eng::compositeFromJson(json, round, &error), "JSON 往返：反序列化成功",
               error);
        expect(check, round.id == "c1" && round.name == "自检上级" && round.inputs.size() == 1 &&
                          round.outputs.size() == 1 && round.exposed_params.size() == 1 &&
                          round.internal_graph.nodes.size() == 1,
               "JSON 往返：字段一致");
        expect(check, round.inputs[0].display_name == "变量" &&
                          round.outputs[0].internal_port == "text" &&
                          round.exposed_params[0].param_id == "template",
               "JSON 往返：引用与显示名一致");
    }

    // ---------------------------------------------------- 3. 注册表 ----
    {
        const eng::CompositeSpec spec = make_selftest_spec();
        std::string              error;
        expect(check, eng::CompositeRegistry::instance().save(spec, &error),
               "注册表：保存规格成功", error);
        expect(check, eng::CompositeRegistry::instance().find("c1") != nullptr,
               "注册表：find 命中");
        expect(check, eng::CompositeRegistry::instance().find("c_missing") == nullptr,
               "注册表：find 未命中返回空");
        expect(check, !eng::CompositeRegistry::instance().list().empty(),
               "注册表：list 非空");
        // 重新扫描目录后仍能读到（落盘成功）
        eng::CompositeRegistry::instance().reload();
        expect(check, eng::CompositeRegistry::instance().find("c1") != nullptr,
               "注册表：reload 后仍命中（文件已落盘）");
        const std::filesystem::path file = temp_root / "c1.json";
        expect(check, std::filesystem::exists(file), "注册表：独立文件已落盘 c1.json");
        expect(check, eng::CompositeRegistry::instance().remove("c1", &error),
               "注册表：删除成功", error);
        expect(check, eng::CompositeRegistry::instance().find("c1") == nullptr,
               "注册表：删除后 find 为空");
    }


    // ---------------------------------------------------- 4. 图集成 ----
    std::string cn1_id;
    {
        const eng::CompositeSpec spec = make_selftest_spec();
        std::string              error;
        expect(check, eng::CompositeRegistry::instance().save(spec, &error),
               "图集成：保存规格成功", error);

        eng::Graph graph;
        std::string add_error;
        cn1_id = graph.addCompositeNode("c1", 0.0f, 0.0f, &add_error);
        expect(check, !cn1_id.empty(), "图集成：addCompositeNode 成功", add_error);
        const eng::Node* cn = graph.findNode(cn1_id);
        expect(check, cn != nullptr && cn->type == "Composite" && cn->composite_id == "c1",
               "图集成：节点类型/规格引用正确");
        expect(check, cn->inputs.size() == 1 && cn->inputs[0].id == "in1" &&
                          cn->inputs[0].is_optional,
               "图集成：暴露输入端口 in1（可选）");
        expect(check, cn->outputs.size() == 1 && cn->outputs[0].id == "out1" &&
                          cn->outputs[0].type == eng::PortType::Text,
               "图集成：暴露输出端口 out1");
        expect(check, cn->params.size() == 1 && cn->params[0].id == "template" &&
                          cn->params[0].value == "请根据以下内容续写：\n{vars}",
               "图集成：暴露参数 template（值取内部默认）");

        // 工作流 JSON 往返：composite_id 被序列化与恢复
        const nlohmann::json doc = eng::graphToJson(graph);
        expect(check, !doc["nodes"][0].value("composite_id", std::string()).empty(),
               "图集成：工作流 JSON 含 composite_id");
        eng::Graph loaded;
        std::string load_error;
        expect(check, eng::graphFromJson(doc, loaded, &load_error),
               "图集成：工作流 JSON 往返成功", load_error);
        const eng::Node* cn2 = loaded.findNode(cn1_id);
        expect(check, cn2 != nullptr && cn2->composite_id == "c1" &&
                          cn2->outputs.size() == 1 && cn2->inputs.size() == 1,
               "图集成：往返后 Composite 节点端口/引用正确");

        // 运行前校验（UI 图含 Composite 节点）
        eng::ValidationMessages messages;
        expect(check, eng::validateWorkflow(graph, &messages) && messages.empty(),
               "图集成：含 Composite 的工作流校验通过");

        // 缺规格：直接实例化失败
        eng::Graph bad;
        std::string bad_error;
        expect(check, bad.addCompositeNode("c_missing", 0, 0, &bad_error).empty() &&
                          bad_error.find("规格不存在") != std::string::npos,
               "图集成：缺规格实例化失败并报错", bad_error);

        // 2026-10-18 回归：空草稿（内部图为空）也能拖入画布实例化 —— 实例化只做结构守卫，
        // 运行态校验在**展开后**由 validateBeforeRun 拦截（此前 build_composite_node 走严格
        // validate_composite_spec ⇒ 「新建上级节点」的空白草稿永远无法添加到画布）。
        {
            eng::CompositeSpec draft;
            draft.id   = "c_draft";
            draft.name = "草稿节点";
            std::string draft_save_error;
            expect(check, eng::CompositeRegistry::instance().save(draft, &draft_save_error,
                                                                  /*allow_incomplete_draft=*/true),
                   "图集成：空草稿保存成功", draft_save_error);
            eng::Graph draft_graph;
            std::string draft_add_error;
            expect(check,
                   !draft_graph.addCompositeNode("c_draft", 0, 0, &draft_add_error).empty(),
                   "图集成：空草稿可拖入画布实例化", draft_add_error);
        }
    }

    // ---------------------------------------------------- 5. 展开执行 ----
    {
        eng::Graph graph;
        const std::string text_id = add_node(check, graph, "TextInput", "展开执行：TextInput");
        std::string error;
        const std::string comp_id = graph.addCompositeNode("c1", 0.0f, 0.0f, &error);
        expect(check, !comp_id.empty(), "展开执行：addCompositeNode 成功", error);
        const std::string out_id = add_node(check, graph, "TextOutput", "展开执行：TextOutput");

        eng::Node* text = graph.findNode(text_id);
        if (eng::Param* p = text->findParam("text")) {
            p->value = "从前有座山";
        }
        // 外部实例参数覆盖：模板改写
        eng::Node* comp = graph.findNode(comp_id);
        if (eng::Param* p = comp->findParam("template")) {
            p->value = std::string("自检模板：") + "{vars}";
        }

        graph.addEdge(text_id, "text", comp_id, "in1", &error);
        graph.addEdge(comp_id, "out1", out_id, "text", &error);

        eng::Executor executor;
        expect(check, executor.start(graph, &error), "展开执行：start 成功（展开后校验）", error);
        expect(check, executor.runToCompletion(&graph, 64), "展开执行：runToCompletion 完成");

        // 展开结果：Composite 节点被移除、内部节点进入图
        expect(check, graph.findNode(comp_id) == nullptr,
               "展开执行：Composite 占位节点已被移除");
        bool internal_found = false;
        for (const eng::Node& node : graph.nodes) {
            if (node.type == "PromptTemplate") {
                internal_found = true;
            }
        }
        expect(check, internal_found, "展开执行：内部节点已进入图");

        // 输出 = 内部模板渲染结果（参数覆盖生效）
        const std::string out_text = eng::nodeOutputText(graph, executor.outputs(), out_id);
        expect(check, out_text == "自检模板：从前有座山",
               "展开执行：输出 = 模板渲染 + 参数覆盖", out_text);
        // 执行计划含内部节点（TextInput + 内部 PromptTemplate + TextOutput）
        expect(check, executor.totalCount() == 3, "展开执行：执行计划含内部节点（3 个）",
               std::to_string(executor.totalCount()));
    }

    // ---------------------------------------------------- 6. 重跑 -------
    {
        eng::Graph graph;
        const std::string text_id = add_node(check, graph, "TextInput", "重跑：TextInput");
        std::string error;
        const std::string comp_id = graph.addCompositeNode("c1", 0.0f, 0.0f, &error);
        const std::string out_id = add_node(check, graph, "TextOutput", "重跑：TextOutput");
        graph.addEdge(text_id, "text", comp_id, "in1", &error);
        graph.addEdge(comp_id, "out1", out_id, "text", &error);
        if (eng::Param* p = graph.findNode(text_id)->findParam("text")) {
            p->value = "旧版文本";
        }

        eng::Executor executor;
        expect(check, executor.start(graph, &error), "重跑：首次 start 成功", error);
        expect(check, executor.runToCompletion(&graph, 64), "重跑：首次 runToCompletion 完成");
        const std::string first_text = eng::nodeOutputText(graph, executor.outputs(), out_id);
        expect(check, first_text == "请根据以下内容续写：\n旧版文本",
               "重跑：首次输出 = 默认模板渲染", first_text);

        // 模拟 UI 的 startAsyncFrom：用**全新图**（仍含 Composite）发起重跑
        //  * 上游 TextInput 不在计划内 ⇒ 复用上次运行值（旧版文本）
        //  * 修改实例参数 template ⇒ 内部 PromptTemplate 重跑后按新模板渲染
        eng::Graph graph2;
        const std::string t2 = graph2.addNode("TextInput", 0, 0, nullptr);
        const std::string c2 = graph2.addCompositeNode("c1", 0, 0, &error);
        const std::string o2 = graph2.addNode("TextOutput", 0, 0, nullptr);
        graph2.addEdge(t2, "text", c2, "in1", &error);
        graph2.addEdge(c2, "out1", o2, "text", &error);
        if (eng::Param* p = graph2.findNode(t2)->findParam("text")) {
            p->value = "旧版文本"; // 与首次一致：上游不重跑，值复用
        }
        if (eng::Param* p = graph2.findNode(c2)->findParam("template")) {
            p->value = std::string("重跑模板：") + "{vars}";
        }

        eng::RerunSpec rerun;
        rerun.target_node = c2;
        rerun.scope       = eng::RerunScope::Downstream;
        expect(check, executor.startFrom(graph2, rerun, &error),
               "重跑：startFrom(Composite, Downstream) 成功", error);
        expect(check, executor.runToCompletion(&graph2, 64), "重跑：runToCompletion 完成");
        const std::string out_text = eng::nodeOutputText(graph2, executor.outputs(), o2);
        expect(check, out_text == "重跑模板：旧版文本",
               "重跑：内部节点已重跑（新模板生效、上游值复用）", out_text);
        // 计划 = 内部节点 + 下游（上游 TextInput 不在内）
        expect(check, executor.totalCount() == 2, "重跑：计划 = 内部节点 + 下游（2 个）",
               std::to_string(executor.totalCount()));
    }

    // ----------------------------------------- 7. 缺规格 → 运行失败 ----
    {
        eng::Graph graph;
        const std::string text_id = add_node(check, graph, "TextInput", "缺规格：TextInput");
        std::string error;
        const std::string comp_id = graph.addCompositeNode("c1", 0.0f, 0.0f, &error);
        const std::string out_id = add_node(check, graph, "TextOutput", "缺规格：TextOutput");
        graph.addEdge(text_id, "text", comp_id, "in1", &error);
        graph.addEdge(comp_id, "out1", out_id, "text", &error);

        eng::CompositeRegistry::instance().remove("c1", nullptr);
        eng::Executor executor;
        expect(check, !executor.start(graph, &error) &&
                          error.find("规格不存在") != std::string::npos,
               "缺规格：start 失败并报规格不存在", error);
    }

    // ----------------------------------------- 8. 双源（PS-D9）--------
    {
        std::error_code ec8;
        const auto builtin_root8 =
            std::filesystem::temp_directory_path(ec8) / "aiwrite_dual_builtin";
        const auto user_root8 = std::filesystem::temp_directory_path(ec8) / "aiwrite_dual_user";
        std::filesystem::remove_all(builtin_root8, ec8);
        std::filesystem::remove_all(user_root8, ec8);
        std::filesystem::create_directories(builtin_root8, ec8);
        std::filesystem::create_directories(user_root8, ec8);

        // 先放好内置资产，再 set_roots（其内部 reload 会扫到）
        const eng::CompositeSpec builtin_spec = make_selftest_spec();
        {
            std::ofstream out(builtin_root8 / "c1.json", std::ios::binary | std::ios::trunc);
            out << eng::compositeToJson(builtin_spec).dump(4);
        }
        eng::CompositeRegistry::instance().setRoots(builtin_root8, user_root8);

        std::string error8;
        expect(check, eng::CompositeRegistry::instance().find("c1") != nullptr,
               "双源：内置源扫描命中");
        expect(check, eng::CompositeRegistry::instance().sourceOf("c1") ==
                          eng::CompositeSource::Builtin,
               "双源：内置来源标记 = Builtin");

        // 内置只读：直接 save / remove 被拒
        eng::CompositeSpec modified = builtin_spec;
        modified.name              = "改名后的官方预设";
        expect(check, !eng::CompositeRegistry::instance().save(modified, &error8) &&
                          error8.find("只读") != std::string::npos,
               "双源：内置源 save 被拒（只读）", error8);
        expect(check, !eng::CompositeRegistry::instance().remove("c1", &error8) &&
                          error8.find("只读") != std::string::npos,
               "双源：内置源 remove 被拒（只读）", error8);

        // 「复制到我的节点库」→ 用户源落盘 + 来源切 User + 可编辑
        expect(check, eng::CompositeRegistry::instance().copyToUser("c1", &error8),
               "双源：copy_to_user 成功", error8);
        expect(check, std::filesystem::exists(user_root8 / "c1.json"), "双源：用户源文件已落盘");
        expect(check, eng::CompositeRegistry::instance().sourceOf("c1") ==
                          eng::CompositeSource::User,
               "双源：复制后来源 = User");
        expect(check, eng::CompositeRegistry::instance().save(modified, &error8),
               "双源：用户源 save 成功（可编辑）", error8);
        expect(check, eng::CompositeRegistry::instance().find("c1")->name == "改名后的官方预设",
               "双源：用户源覆盖生效（find 取用户内容）");

        // 删除用户副本 → 来源回落 Builtin（官方预设恢复，只读回归）
        expect(check, eng::CompositeRegistry::instance().remove("c1", &error8),
               "双源：删除用户副本成功", error8);
        expect(check, eng::CompositeRegistry::instance().sourceOf("c1") ==
                          eng::CompositeSource::Builtin,
               "双源：删除后来源回落 Builtin");
        expect(check, eng::CompositeRegistry::instance().find("c1")->name == "自检上级",
               "双源：回落的内置规格恢复（名称还原）");
        expect(check, !eng::CompositeRegistry::instance().save(modified, &error8) &&
                          error8.find("只读") != std::string::npos,
               "双源：回落后 save 再次被拒（只读回归）", error8);

        // 回到单源临时根（后续清理按 temp_root 进行）
        eng::CompositeRegistry::instance().setRoot(temp_root);
        std::filesystem::remove_all(builtin_root8, ec8);
        std::filesystem::remove_all(user_root8, ec8);
    }

    // ----------------------------------------- 9. 热重载（保存后同步实例）--------
    // 上级节点编辑器「保存」→ 主画布已放置的旧实例按新规格就地重建
    {
        std::string error9;
        const eng::CompositeSpec base9 = make_selftest_spec(); // id=c1 · 暴露 template
        expect(check, eng::CompositeRegistry::instance().save(base9, &error9),
               "热重载：初始规格保存成功", error9);

        // 画布放置两个同规格实例并接边（TextOutput 单输入非变长 ⇒ 每个输出节点只接一条）
        eng::Graph graph9;
        const std::string text9  = graph9.addNode("TextInput", 0.0f, 0.0f, nullptr);
        const std::string comp9  = graph9.addCompositeNode("c1", 0.0f, 0.0f, &error9);
        const std::string comp9b = graph9.addCompositeNode("c1", 80.0f, 0.0f, &error9);
        const std::string out9   = graph9.addNode("TextOutput", 0.0f, 0.0f, nullptr);
        const std::string out9b  = graph9.addNode("TextOutput", 0.0f, 80.0f, nullptr);
        expect(check, !comp9.empty() && !comp9b.empty(), "热重载：两个实例创建成功", error9);
        graph9.addEdge(text9, "text", comp9, "in1", &error9);
        graph9.addEdge(comp9, "out1", out9, "text", &error9);
        graph9.addEdge(comp9b, "out1", out9b, "text", &error9);
        expect(check, graph9.edges.size() == 3, "热重载：初始 3 条边",
               std::to_string(graph9.edges.size()));

        // 实例 A：默认标题（= 旧规格名）+ 已填模板参数值
        if (eng::Node* n = graph9.findNode(comp9)) {
            if (eng::Param* p = n->findParam("template")) {
                p->value = std::string("旧模板{vars}");
            }
        }
        // 实例 B：自定义标题（不应被新规格名覆盖）
        if (eng::Node* n = graph9.findNode(comp9b)) {
            n->title = "我的自定义标题";
        }

        // 新规格：改名 + 新增暴露参数 temperature + 移除对外输入（触发失效边清理）
        eng::CompositeSpec updated9 = base9;
        updated9.name              = "自检上级-新名";
        updated9.inputs.clear(); // 不再暴露输入 → 连向 in1 的边应被清理
        eng::CompositeParamRef temp_ref;
        temp_ref.internal_node = updated9.exposed_params.front().internal_node;
        temp_ref.param_id      = "temperature";
        temp_ref.display_name  = "温度";
        updated9.exposed_params.push_back(temp_ref);

        expect(check, eng::CompositeRegistry::instance().save(updated9, &error9),
               "热重载：新规格保存成功", error9);
        const int refreshed = eng::refresh_composite_instances(graph9, "c1", base9.name, &error9);
        expect(check, refreshed == 2, "热重载：返回 2（两个实例被同步）", error9);

        // 实例 A：标题跟随新规格名、模板值保留、temperature 新参数出现、输入端口清空
        const eng::Node* a = graph9.findNode(comp9);
        expect(check, a != nullptr && a->title == "自检上级-新名", "热重载：默认标题跟随新规格名",
               a != nullptr ? a->title : "");
        if (a != nullptr) {
            const eng::Param* tpl = a->findParam("template");
            expect(check, tpl != nullptr && tpl->text() == "旧模板{vars}",
                   "热重载：已填模板参数值保留");
            const eng::Param* temperature = a->findParam("temperature");
            expect(check, temperature != nullptr && temperature->number() == 0.7,
                   "热重载：新增暴露参数 temperature 出现（取默认值）");
            expect(check, a->inputs.empty(), "热重载：实例 A 输入端口已按新规格清空");
        }
        // 实例 B：自定义标题保留、位置保留、实例 id 不变
        const eng::Node* b = graph9.findNode(comp9b);
        expect(check, b != nullptr && b->title == "我的自定义标题", "热重载：自定义标题保留",
               b != nullptr ? b->title : "");
        expect(check, b != nullptr && b->x == 80.0f && b->y == 0.0f, "热重载：实例位置保留");
        expect(check, b != nullptr && b->id == comp9b, "热重载：实例 id 不变");

        // 边：连向已移除 in1 的边被清理；仍有效的 out1 边保留
        expect(check, graph9.edges.size() == 2, "热重载：失效输入边被清理（3 → 2）",
               std::to_string(graph9.edges.size()));
        expect(check, graph9.findEdgeIntoInput(comp9, "in1") == nullptr,
               "热重载：in1 已无入边");

        // 清理临时注册表
        eng::CompositeRegistry::instance().remove("c1", nullptr);
    }

    // -------------------------------------------------- 10. 试运行沙箱 ----
    // 「执行连线」：build_composite_test_graph 为暴露输入接测试源，让内部图脱离父工作流也能跑
    {
        // 内部图：TextSplit（必填输入 text）→ TextOutput；暴露输入 = TextSplit.text（必填）
        eng::Graph internal10;
        const std::string tsp10  = internal10.addNode("TextSplit", 0.0f, 0.0f, nullptr);
        const std::string tout10 = internal10.addNode("TextOutput", 0.0f, 80.0f, nullptr);
        internal10.addEdge(tsp10, "texts", tout10, "text", nullptr);

        eng::CompositeSpec spec10;
        spec10.id   = "c10";
        spec10.name = "试运行自检";
        eng::CompositePortRef in10;
        in10.internal_node = tsp10;
        in10.internal_port = "text";
        in10.display_name  = "文本";
        in10.type          = eng::PortType::Text;
        spec10.inputs.push_back(in10);
        eng::CompositePortRef out10;
        out10.internal_node = tsp10;
        out10.internal_port = "texts";
        out10.display_name  = "分块";
        out10.type          = eng::PortType::Text;
        spec10.outputs.push_back(out10);
        spec10.internal_graph = std::move(internal10);

        // 无沙箱时：必填输入 text 未连接 ⇒ 运行前校验必须拒绝
        {
            eng::Graph raw = spec10.internal_graph;
            eng::ValidationMessages raw_err;
            eng::ValidationMessages raw_warn;
            expect(check, !eng::validateBeforeRun(raw, &raw_err, &raw_warn),
                   "试运行：无沙箱时必填输入未连接 → 运行前校验拒绝");
        }

        // 构建沙箱：暴露输入自动接测试源
        eng::Graph sandbox10;
        std::string err10;
        expect(check, eng::buildCompositeTestGraph(spec10, &sandbox10, &err10),
               "试运行：沙箱图构建成功", err10);

        int source_count10 = 0;
        for (const eng::Node& n : sandbox10.nodes) {
            if (n.id.rfind("__ce_src_", 0) == 0) {
                ++source_count10;
            }
        }
        expect(check, source_count10 == 1, "试运行：暴露输入生成了 1 个测试源节点",
               std::to_string(source_count10));
        expect(check, sandbox10.nodes.size() == 3, "试运行：内部 2 节点 + 1 测试源",
               std::to_string(sandbox10.nodes.size()));
        expect(check, sandbox10.findEdgeIntoInput(tsp10, "text") != nullptr,
               "试运行：暴露输入端口已接测试源");

        // 规格本身不被沙箱构建改动（内部图原样保留）
        expect(check, spec10.internal_graph.nodes.size() == 2 &&
                          spec10.internal_graph.edges.size() == 1,
               "试运行：构建沙箱不改动规格内部图");

        // 带沙箱：运行前校验通过；给测试源填值 → 整图跑通
        if (eng::Node* src10 = sandbox10.findNode("__ce_src_1")) {
            if (eng::Param* p = src10->findParam("text")) {
                p->value = std::string("试运行你好");
            }
        }
        {
            eng::ValidationMessages verr10;
            eng::ValidationMessages vwarn10;
            expect(check, eng::validateBeforeRun(sandbox10, &verr10, &vwarn10),
                   "试运行：运行前校验通过（测试源已喂必填输入）",
                   verr10.empty() ? "" : verr10.front());
        }

        eng::Executor ex10;
        ex10.setConsoleHandler([](const std::string&) {}); // 静音执行器
        expect(check, ex10.start(sandbox10, &err10), "试运行：执行器启动", err10);
        const bool completed10 = ex10.runToCompletion(&sandbox10, 64);
        expect(check, completed10, "试运行：整图执行完成");

        const eng::Node* done_tsp10 = sandbox10.findNode(tsp10);
        expect(check, done_tsp10 != nullptr && done_tsp10->state == eng::NodeState::Done,
               "试运行：TextSplit 执行成功（Done）",
               done_tsp10 != nullptr ? eng::nodeStateName(done_tsp10->state) : "missing");
        expect(check, ex10.outputs().find(tsp10, "texts") != nullptr,
               "试运行：TextSplit 已产出输出值 texts");
    }

    // ------------------------------- 11. 打包（buildCompositeSpecFromSelection）----
    // ARCH-E6：选区 → 上级节点规格（自动识别边界输入/输出/暴露参数 + variadic 冲突自动修正）
    {
        // 11.1 基础打包：TextInput(选区外) → PromptTemplate(选区内) → TextOutput(选区外)
        eng::Graph pg;
        const std::string ext_in  = pg.addNode("TextInput", 0.0f, 0.0f, nullptr);
        const std::string pt      = pg.addNode("PromptTemplate", 0.0f, 0.0f, nullptr);
        const std::string ext_out = pg.addNode("TextOutput", 0.0f, 0.0f, nullptr);
        std::string       perr;
        pg.addEdge(ext_in, "text", pt, "vars", &perr);
        pg.addEdge(pt, "text", ext_out, "text", &perr);
        // TextInput.text 是必填参数：打包产物要过严格校验，源图需为合法工作流
        if (eng::Param* p = pg.findNode(ext_in)->findParam("text")) {
            p->value = "打包输入";
        }

        // 少于 2 个节点被拒绝
        eng::CompositeSpec p1;
        const std::vector<std::string> sel1 = {pt};
        expect(check, !eng::buildCompositeSpecFromSelection(pg, sel1, "打包自检", &p1, &perr) &&
                          perr.find("2 个节点") != std::string::npos,
               "打包：少于 2 个节点被拒绝", perr);

        // 选区 {ext_in, pt}：内部边 ext_in→pt.vars；无暴露输入（vars 入边来自选区内）；
        // 边界输出 = pt.text（到选区外 TextOutput）；暴露参数 = pt 非密钥参数
        const std::vector<std::string> sel_ab = {ext_in, pt};
        eng::CompositeSpec pack_ab;
        expect(check, eng::buildCompositeSpecFromSelection(pg, sel_ab, "打包自检", &pack_ab, &perr),
               "打包：基础打包成功", perr);
        expect(check, pack_ab.internal_graph.nodes.size() == 2, "打包：内部图 = 选中 2 节点",
               std::to_string(pack_ab.internal_graph.nodes.size()));
        expect(check, pack_ab.internal_graph.edges.size() == 1, "打包：内部边 = 选区内 1 条",
               std::to_string(pack_ab.internal_graph.edges.size()));
        expect(check, pack_ab.inputs.empty(), "打包：无暴露输入（vars 入边来自选区内）");
        expect(check, pack_ab.outputs.size() == 1 && pack_ab.outputs[0].internal_node == pt &&
                          pack_ab.outputs[0].internal_port == "text",
               "打包：边界输出 = pt.text（到选区外 TextOutput）");
        expect(check, !pack_ab.exposed_params.empty(), "打包：暴露参数非空");
        // 密钥参数不暴露（PromptTemplate 有 api_key is_secret=true）
        const bool has_secret =
            std::any_of(pack_ab.exposed_params.begin(), pack_ab.exposed_params.end(),
                        [](const eng::CompositeParamRef& r) { return r.param_id == "api_key"; });
        expect(check, !has_secret, "打包：密钥参数（api_key）不暴露");
        pack_ab.id = "c_pack1";
        std::string pab_err;
        expect(check, eng::validate_composite_spec(pack_ab, &pab_err), "打包：基础产物通过严格校验",
               pab_err);

        // 运行态清零：源节点 Running + 错误 → 打包产物应为 Idle + 无错误
        {
            eng::Node* pt_node = pg.findNode(pt);
            pt_node->state     = eng::NodeState::Running;
            pt_node->error_message = "打包测试错误";
            eng::CompositeSpec pack_run;
            expect(check, eng::buildCompositeSpecFromSelection(pg, sel_ab, "打包自检", &pack_run, &perr),
                   "打包：运行态节点重打包成功", perr);
            const eng::Node* inner = pack_run.internal_graph.findNode(pt);
            expect(check, inner != nullptr && inner->state == eng::NodeState::Idle &&
                              inner->error_message.empty(),
                   "打包：内部节点运行态清零（Idle + 无错误）");
            pt_node->state         = eng::NodeState::Idle;
            pt_node->error_message.clear();
        }

        // 11.2 暴露输入识别：选区 {pt, ext_out}（ext_in 在选区外）→ pt.vars 成为暴露输入
        const std::vector<std::string> sel_bc = {pt, ext_out};
        eng::CompositeSpec pack_bc;
        expect(check, eng::buildCompositeSpecFromSelection(pg, sel_bc, "打包自检", &pack_bc, &perr),
               "打包：选区含输出节点成功", perr);
        expect(check, pack_bc.inputs.size() == 1 && pack_bc.inputs[0].internal_node == pt &&
                          pack_bc.inputs[0].internal_port == "vars",
               "打包：暴露输入 = pt.vars（ext_in 在选区外）");
        expect(check, pack_bc.internal_graph.findEdgeIntoInput(pt, "vars") == nullptr,
               "打包：暴露输入端口无内部入边（ext_in 在选区外）");
        pack_bc.id = "c_pack2";
        std::string pbc_err;
        expect(check, eng::validate_composite_spec(pack_bc, &pbc_err), "打包：暴露输入产物通过严格校验",
               pbc_err);
    }


    // ---- 11.3 variadic 冲突自动修正（修暴露节点保存 · audit §4.2 问题 2）----
    // VLMGenerate.image（variadic）同时有选区内入边（img_in）与选区外入边（img_ext）：
    // 打包旧逻辑会生成「暴露输入带内部入边」的非法规格 → 保存被拒；ARCH-E6 在生成阶段
    // 剔除指向暴露输入的内边（外部输入接管）→ 产物必然通过严格校验、可直接保存。
    {
        eng::Graph vg;
        const std::string img_in  = vg.addNode("ImageInput", 0.0f, 0.0f, nullptr);
        const std::string vlm     = vg.addNode("VLMGenerate", 0.0f, 0.0f, nullptr);
        const std::string img_ext = vg.addNode("ImageInput", 0.0f, 0.0f, nullptr);
        const std::string out_ext = vg.addNode("TextOutput", 0.0f, 0.0f, nullptr);
        std::string       verr;
        vg.addEdge(img_in, "image", vlm, "image", &verr);   // 选区内
        vg.addEdge(img_ext, "image", vlm, "image", &verr);  // 选区外
        vg.addEdge(vlm, "text", out_ext, "text", &verr);    // 选区外

        const std::vector<std::string> sel_vlm = {img_in, vlm};
        eng::CompositeSpec pv;
        expect(check, eng::buildCompositeSpecFromSelection(vg, sel_vlm, "打包图片", &pv, &verr),
               "打包：variadic 冲突场景打包成功", verr);
        expect(check, pv.inputs.size() == 1 && pv.inputs[0].internal_node == vlm &&
                          pv.inputs[0].internal_port == "image",
               "打包：vlm.image 有选区外入边 → 暴露输入");
        expect(check, pv.internal_graph.findEdgeIntoInput(vlm, "image") == nullptr,
               "打包：暴露输入端口内部入边被自动剔除（外部输入接管）");
        expect(check, pv.outputs.size() == 1 && pv.outputs[0].internal_node == vlm &&
                          pv.outputs[0].internal_port == "text",
               "打包：边界输出 = vlm.text（到选区外 TextOutput）");
        pv.id = "c_pack_vlm";
        std::string pv_err;
        expect(check, eng::validate_composite_spec(pv, &pv_err), "打包：冲突修正后通过严格校验", pv_err);
        // 问题 2 修复：修正后的规格可直接保存（不再被「暴露输入已有内部连线」拒绝）
        std::string save_err;
        expect(check, eng::CompositeRegistry::instance().save(pv, &save_err),
               "打包：修正后可直接保存（修暴露节点保存）", save_err);
        eng::CompositeRegistry::instance().remove(pv.id, nullptr);
    }

    // ---- 12. DBG-CN-01 回归：暴露输入为空 = 合法（可校验 / 可保存 / 可回读 / 可实例化）----
    // 现象登记：上级节点规格的暴露输入端口为空时，保存被校验拦截（期望可保存）。
    // 本块按断言逐层验证「空输入」规格的完整链路，任一层被拦即 FAIL：
    //  * 严格校验：只强制 1 个对外输出，**不**要求暴露输入非空（composite.cpp 校验段）
    //  * 保存：注册表 save（allow_incomplete_draft=false）落盘 JSON（inputs = 空数组）
    //  * 回读：compositeFromJson 反序列化后 inputs 仍为空（没有隐式补端口）
    //  * 实例化：画布实例 0 输入 / 1 输出；暴露输入恒 is_optional ⇒ 父图校验通过
    {
        eng::CompositeSpec no_inputs = make_selftest_spec();
        no_inputs.id   = "c_no_inputs";
        no_inputs.name = "空输入上级";
        no_inputs.inputs.clear(); // 只保留内部图 + 1 个对外输出 + 暴露参数

        std::string ni_error;
        expect(check, eng::validate_composite_spec(no_inputs, &ni_error),
               "空输入：严格校验通过（暴露输入可为 0）", ni_error);

        // 保存（严格模式）：空输入不应被拦
        expect(check, eng::CompositeRegistry::instance().save(no_inputs, &ni_error),
               "空输入：注册表保存成功（严格校验放行）", ni_error);
        const std::filesystem::path ni_file = temp_root / "c_no_inputs.json";
        expect(check, std::filesystem::exists(ni_file), "空输入：JSON 已落盘 c_no_inputs.json");

        // 落盘回读：inputs 为空数组，不得被补出端口
        {
            std::ifstream  in(ni_file);
            nlohmann::json doc;
            in >> doc;
            expect(check, doc.contains("inputs") && doc["inputs"].is_array() &&
                              doc["inputs"].empty(),
                   "空输入：落盘 JSON 的 inputs 为空数组");
            eng::CompositeSpec round;
            std::string        parse_error;
            expect(check, eng::compositeFromJson(doc, round, &parse_error) && round.inputs.empty() &&
                              round.outputs.size() == 1,
                   "空输入：反序列化后 inputs 仍为空 / outputs = 1", parse_error);
        }

        // 实例化到父图 + 父图校验（暴露输入恒可选 ⇒ 0 输入不会被「必填输入未连接」拦）
        eng::Graph  parent;
        std::string add_error;
        const std::string inst = parent.addCompositeNode("c_no_inputs", 0.0f, 0.0f, &add_error);
        expect(check, !inst.empty(), "空输入：可实例化到画布", add_error);
        const eng::Node* inst_node = parent.findNode(inst);
        expect(check, inst_node != nullptr && inst_node->inputs.empty() &&
                          inst_node->outputs.size() == 1,
               "空输入：实例 0 输入 / 1 输出");
        eng::ValidationMessages ni_messages;
        expect(check, eng::validateWorkflow(parent, &ni_messages) && ni_messages.empty(),
               "空输入：含该实例的父图校验通过");

        eng::CompositeRegistry::instance().remove(no_inputs.id, nullptr);
    }

    // 清理临时目录
    std::filesystem::remove_all(temp_root, ec);
    std::printf("=== 上级节点自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

