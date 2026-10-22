#include "composite_selftest.h"

#include "composite.h"
#include "executor.h"
#include "nodes.h"
#include "node_registry.h"
#include "validate.h"
#include "workflow_io.h"

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
    eng::CompositeRegistry::instance().set_root(temp_root);

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
        const nlohmann::json     json = eng::composite_to_json(spec);
        eng::CompositeSpec       round;
        std::string              error;
        expect(check, eng::composite_from_json(json, round, &error), "JSON 往返：反序列化成功",
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
        const nlohmann::json doc = eng::graph_to_json(graph);
        expect(check, !doc["nodes"][0].value("composite_id", std::string()).empty(),
               "图集成：工作流 JSON 含 composite_id");
        eng::Graph loaded;
        std::string load_error;
        expect(check, eng::graph_from_json(doc, loaded, &load_error),
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
            out << eng::composite_to_json(builtin_spec).dump(4);
        }
        eng::CompositeRegistry::instance().set_roots(builtin_root8, user_root8);

        std::string error8;
        expect(check, eng::CompositeRegistry::instance().find("c1") != nullptr,
               "双源：内置源扫描命中");
        expect(check, eng::CompositeRegistry::instance().source_of("c1") ==
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
        expect(check, eng::CompositeRegistry::instance().copy_to_user("c1", &error8),
               "双源：copy_to_user 成功", error8);
        expect(check, std::filesystem::exists(user_root8 / "c1.json"), "双源：用户源文件已落盘");
        expect(check, eng::CompositeRegistry::instance().source_of("c1") ==
                          eng::CompositeSource::User,
               "双源：复制后来源 = User");
        expect(check, eng::CompositeRegistry::instance().save(modified, &error8),
               "双源：用户源 save 成功（可编辑）", error8);
        expect(check, eng::CompositeRegistry::instance().find("c1")->name == "改名后的官方预设",
               "双源：用户源覆盖生效（find 取用户内容）");

        // 删除用户副本 → 来源回落 Builtin（官方预设恢复，只读回归）
        expect(check, eng::CompositeRegistry::instance().remove("c1", &error8),
               "双源：删除用户副本成功", error8);
        expect(check, eng::CompositeRegistry::instance().source_of("c1") ==
                          eng::CompositeSource::Builtin,
               "双源：删除后来源回落 Builtin");
        expect(check, eng::CompositeRegistry::instance().find("c1")->name == "自检上级",
               "双源：回落的内置规格恢复（名称还原）");
        expect(check, !eng::CompositeRegistry::instance().save(modified, &error8) &&
                          error8.find("只读") != std::string::npos,
               "双源：回落后 save 再次被拒（只读回归）", error8);

        // 回到单源临时根（后续清理按 temp_root 进行）
        eng::CompositeRegistry::instance().set_root(temp_root);
        std::filesystem::remove_all(builtin_root8, ec8);
        std::filesystem::remove_all(user_root8, ec8);
    }

    // 清理临时目录
    std::filesystem::remove_all(temp_root, ec);
    std::printf("=== 上级节点自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

