// =============================================================================
//  api_probe —— M1 技术验证命令行工具
//
//  验证项：
//    V-04  HTTP 请求（cpp-httplib + OpenSSL）
//    V-05  SHA3-256（OpenSSL EVP，对照标准测试向量）
//    V-06  DeepSeek API 最小调用（非流式，对应 M1-06）
//
//  用法：
//    api_probe --selftest                      # V-04 + V-05
//    api_probe --http <url>
//    api_probe --sha3 <text>
//    api_probe --chat "<prompt>" [--model deepseek-chat]
//                                [--api-base https://api.deepseek.com] [--insecure]
//    api_probe --chat "<prompt>" --dry-run      # 只打印/断言请求构造，不需要 Key
//    api_probe --selftest                      # V-04 + V-05 + V-06 请求构造 + V-08 配置往返

//
//  说明：API Key 只从环境变量 DEEPSEEK_API_KEY 读取，不写入磁盘、不硬编码。
// =============================================================================
#include "utils/crypto.h"
#include "utils/log.h"
#include "utils/paths.h"

#include "engine/graph.h"
#include "engine/node_registry.h"
#include "engine/undo_stack.h"
#include "engine/workflow_io.h"
#include "utils/config.h"
#include "web/session_store.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// 引擎层位于 namespace aiwrite::engine，这里起别名便于书写
namespace engine = aiwrite::engine;

namespace {

struct Options {
    bool        selftest = false;
    bool        graph_selftest = false;
    bool        insecure = false;
    bool        dry_run  = false;   // --chat --dry-run：只打印/断言请求，不发网络请求
    std::string http_url;
    std::string sha3_text;
    std::string chat_prompt;
    std::string model    = "deepseek-chat";
    std::string api_base = "https://api.deepseek.com";
};

// NIST FIPS 202 标准测试向量
constexpr const char* kSha3Abc   = "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532";
constexpr const char* kSha3Empty = "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a";

void print_usage()
{
    std::printf("api_probe - AIwrite M1 技术验证工具\n");
    std::printf("用法:\n");
    std::printf("  api_probe --selftest\n");
    std::printf("  api_probe --graph-selftest          # 图模型/注册表/撤销栈自检（不需要网络）\n");
    std::printf("  api_probe --sha3 <text>\n");
    std::printf("  api_probe --http <url>\n");
    std::printf("  api_probe --chat \"<prompt>\" [--model <name>] [--api-base <url>] [--insecure]\n");
    std::printf("     --dry-run   只打印请求（URL / Authorization 脱敏 / body）并做结构断言，不发起网络请求\n");
    std::printf("  自检 --selftest 覆盖: V-05 SHA3 / V-04 HTTP / V-06 请求构造 / V-08 配置往返 / V-09 会话与可见性\n");

}

bool test_sha3()
{
    struct Vector {
        const char* text;
        const char* expected;
    };
    const Vector vectors[] = {
        {"abc", kSha3Abc},
        {"", kSha3Empty},
    };

    bool all_ok = true;
    std::printf("[V-05] SHA3-256 (OpenSSL EVP)\n");
    for (const Vector& vector : vectors) {
        const std::string actual = aiwrite::crypto::sha3_256(vector.text);
        const bool ok            = (actual == vector.expected);
        all_ok                   = all_ok && ok;
        std::printf("   %-6s SHA3-256(\"%s\") = %s\n", ok ? "PASS" : "FAIL", vector.text,
                    actual.c_str());
        if (!ok) {
            std::printf("          期望: %s\n", vector.expected);
        }
    }
    return all_ok;
}

// V-06 请求构造自检（无需 API Key）：打印将发送的请求并做结构断言
bool test_chat_request_shape(const Options& options)
{
    std::printf("[V-06] 请求构造自检（不发起网络请求）\n");

    nlohmann::json body;
    body["model"]    = options.model;
    body["stream"]   = false;
    body["messages"] = nlohmann::json::array(
        {nlohmann::json{{"role", "user"}, {"content", options.chat_prompt}}});

    const char* key        = std::getenv("DEEPSEEK_API_KEY");
    const std::string auth = (key != nullptr && *key != '\0')
                                 ? "Bearer ****（已设置 DEEPSEEK_API_KEY，值已脱敏）"
                                 : "(未设置 DEEPSEEK_API_KEY，仅校验请求构造)";

    std::printf("       POST %s/chat/completions\n", options.api_base.c_str());
    std::printf("       Authorization: %s\n", auth.c_str());
    std::printf("       Content-Type: application/json\n");
    std::printf("       body: %s\n", body.dump(2).c_str());

    const bool ok =
        body.contains("model") && body["model"].is_string() &&
        !body["model"].get<std::string>().empty() && body.contains("stream") &&
        body["stream"].is_boolean() && !body["stream"].get<bool>() && body.contains("messages") &&
        body["messages"].is_array() && body["messages"].size() == 1 &&
        body["messages"][0].value("role", std::string()) == "user" &&
        body["messages"][0].value("content", std::string()) == options.chat_prompt;

    std::printf("   %-6s 请求体结构: model 非空 / stream=false / messages[1]{role=user, content=prompt}\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// M1-07 配置读写往返自检（在临时文件上做，不改动用户配置）
bool test_config_roundtrip()
{
    std::printf("[V-08] 配置加载 / 保存往返自检（临时文件）\n");

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "aiwrite_config_roundtrip.toml";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    aiwrite::Config created;
    if (!aiwrite::load_config(file, created)) { // 文件不存在 → 生成默认配置并落盘
        std::printf("   FAIL  默认配置生成失败\n");
        return false;
    }
    const bool generated = std::filesystem::exists(file);

    aiwrite::Config modified         = created;
    modified.general.language        = "zh-CN-selftest";
    modified.ui.show_grid            = !created.ui.show_grid;
    modified.output.ttl_days         = 7;
    modified.deepseek.model          = "deepseek-reasoner";
    if (!aiwrite::save_config(file, modified)) {
        std::printf("   FAIL  保存配置失败\n");
        return false;
    }

    aiwrite::Config reloaded;
    if (!aiwrite::load_config(file, reloaded)) {
        std::printf("   FAIL  重新加载配置失败\n");
        return false;
    }

    const bool same = generated && reloaded.config_version == modified.config_version &&
                      reloaded.general.language == modified.general.language &&
                      reloaded.ui.show_grid == modified.ui.show_grid &&
                      reloaded.output.ttl_days == modified.output.ttl_days &&
                      reloaded.deepseek.model == modified.deepseek.model;
    std::printf("   %-6s 默认生成=%s；字段往返一致=%s（language / show_grid / ttl_days / model）\n",
                same ? "PASS" : "FAIL", generated ? "是" : "否", same ? "是" : "否");

    std::filesystem::remove(file, ec);
    return same;
}

// M2：网页版会话存储 + 参数条件可见性自检（不需要网络）
bool test_web_session_and_visibility()
{
    std::printf("[V-09] 会话存储 / 参数条件可见性自检\n");

    // ---- 会话存储：写入 → 快照 → 查找 → 注销 ----
    aiwrite::web::Session session;
    session.url = "https://chat.deepseek.com/";
    aiwrite::web::Cookie cookie;
    cookie.name  = "ds_session_id";
    cookie.value = "0123456789abcdef0123456789abcdef";
    session.cookies.push_back(cookie);
    aiwrite::web::SessionStore::instance().set(session);

    const aiwrite::web::Session snapshot = aiwrite::web::SessionStore::instance().snapshot();
    const bool store_ok = snapshot.logged_in && snapshot.cookie_count() == 1 &&
                          snapshot.has("ds_session_id") &&
                          aiwrite::web::mask_value(cookie.value) == "0123****cdef(len=32)";
    aiwrite::web::SessionStore::instance().clear();
    const bool cleared_ok = !aiwrite::web::SessionStore::instance().logged_in() &&
                            aiwrite::web::SessionStore::instance().cookie_count() == 0;

    // ---- 参数条件可见性：ProviderConfig 在 official 显示 api_key，在 web 隐藏（且不参与校验）----
    aiwrite::engine::registerAllNodes();
    aiwrite::engine::Graph graph;
    std::string            error;
    const std::string      id       = graph.addNode("ProviderConfig", 0.0f, 0.0f, &error);
    aiwrite::engine::Node* provider = graph.findNode(id);

    bool visibility_ok = false;
    if (provider != nullptr) {
        aiwrite::engine::Param* mode = provider->findParam("mode");
        std::vector<std::string> errors;
        const bool official_visible = aiwrite::engine::param_visible(*provider, "api_key");
        const bool official_valid   = graph.validateParams(*provider, &errors);
        if (mode != nullptr) {
            mode->value = "web";
        }
        errors.clear();
        const bool web_hidden = !aiwrite::engine::param_visible(*provider, "api_key") &&
                                !aiwrite::engine::param_visible(*provider, "api_base");
        const bool web_valid = graph.validateParams(*provider, &errors);
        visibility_ok =
            (mode != nullptr) && official_visible && web_hidden && official_valid && web_valid;
    }

    const bool ok = store_ok && cleared_ok && visibility_ok;
    std::printf("   %-6s 会话存储写读=%s%s；参数可见性=%s（official 显示 api_key / web 隐藏 "
                "api_key·api_base / 两模式校验均通过）\n",
                ok ? "PASS" : "FAIL", store_ok ? "OK" : "异常", cleared_ok ? "/注销清空 OK" : "/注销异常",
                visibility_ok ? "OK" : "异常");
    return ok;
}

// V-04：HTTP GET
// 返回：0 = 通过（2xx~4xx，链路连通）；1 = 失败；2 = 跳过（网络/代理不可达，非代码问题）
int test_http(const std::string& url, bool insecure)
{
    std::printf("[V-04] HTTP GET %s\n", url.c_str());
    std::printf("       连接超时 15s / 读取超时 30s / 跟随重定向 / 证书校验: %s\n",
                insecure ? "关闭（--insecure）" : "开启");

    httplib::Client client(url);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(30, 0);
    client.enable_server_certificate_verification(!insecure);
    client.set_follow_location(true);

    const auto start = std::chrono::steady_clock::now();
    auto res         = client.Get("/");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();

    if (!res) {
        const auto error = res.error();
        // 连接类错误视为"网络/代理不可达" → SKIP（不算代码失败）；TLS 类错误才算 FAIL
        const bool network_unreachable =
            error == httplib::Error::Connection || error == httplib::Error::ConnectionTimeout ||
            error == httplib::Error::Read || error == httplib::Error::Write ||
            error == httplib::Error::ProxyConnection;
        std::printf("   %-6s 请求失败: %s（TLS 校验结果: %ld）\n",
                    network_unreachable ? "SKIP" : "FAIL", httplib::to_string(error).c_str(),
                    insecure ? 0L : client.get_openssl_verify_result());
        if (network_unreachable) {
            std::printf("          判定为网络/代理不可达 → SKIP（不计入失败；如为证书问题可加 --insecure 复测）\n");
            return 2;
        }
        return 1;
    }

    // 未带 Key 访问 api.deepseek.com 会返回 401，同样说明链路连通
    const bool reachable = res->status >= 200 && res->status < 500;
    std::printf("   %-6s HTTP %d，耗时 %lld ms，响应 %zu 字节\n", reachable ? "PASS" : "FAIL",
                res->status, static_cast<long long>(elapsed), res->body.size());
    if (!reachable) {
        std::printf("         响应: %s\n", res->body.substr(0, 200).c_str());
    }
    return reachable ? 0 : 1;
}

// V-06：DeepSeek 官方 API 最小调用（M1-06）
int test_chat(const Options& options)
{
    if (options.dry_run) { // --dry-run：只校验请求构造，不需要 API Key
        return test_chat_request_shape(options) ? 0 : 1;
    }

    std::printf("[V-06] DeepSeek API 调用（model=%s, base=%s）\n", options.model.c_str(),
                options.api_base.c_str());

    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key == nullptr || *key == '\0') {
        std::printf("   SKIP  未设置环境变量 DEEPSEEK_API_KEY，跳过（其余验证项不受影响）\n");
        return 2;
    }

    httplib::Client client(options.api_base);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(180, 0);
    client.enable_server_certificate_verification(!options.insecure);

    nlohmann::json body;
    body["model"]    = options.model;
    body["stream"]   = false;
    body["messages"] = nlohmann::json::array(
        {nlohmann::json{{"role", "user"}, {"content", options.chat_prompt}}});

    const httplib::Headers headers = {
        {"Authorization", std::string("Bearer ") + key},
        {"Content-Type", "application/json"},
    };

    const auto start = std::chrono::steady_clock::now();
    auto res         = client.Post("/chat/completions", headers, body.dump(), "application/json");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();

    if (!res) {
        std::printf("   FAIL  请求失败: %s\n", httplib::to_string(res.error()).c_str());
        return 1;
    }
    if (res->status != 200) {
        std::printf("   FAIL  HTTP %d\n", res->status);
        std::printf("         响应: %s\n", res->body.substr(0, 400).c_str());
        return 1;
    }

    try {
        const nlohmann::json response = nlohmann::json::parse(res->body);
        std::string content;
        if (response.contains("choices") && !response["choices"].empty()) {
            content = response["choices"][0]["message"]["content"].get<std::string>();
        }

        std::printf("   PASS  HTTP 200，耗时 %lld ms\n", static_cast<long long>(elapsed));
        if (response.contains("usage")) {
            const auto& usage = response["usage"];
            std::printf("         tokens: prompt=%d completion=%d total=%d\n",
                        usage.value("prompt_tokens", 0), usage.value("completion_tokens", 0),
                        usage.value("total_tokens", 0));
        }
        std::printf("--------- 生成结果 ---------\n%s\n----------------------------\n",
                    content.c_str());
        return 0;
    }
    catch (const std::exception& ex) {
        std::printf("   FAIL  响应解析失败: %s\n", ex.what());
        std::printf("         响应: %s\n", res->body.substr(0, 400).c_str());
        return 1;
    }
}

} // namespace

namespace {

// ---------------------------------------------------------- 图模型自检 -------
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

void expect_eq(Check& check, const std::string& actual, const std::string& wanted,
               const std::string& name)
{
    expect(check, actual == wanted, name, "期望 " + wanted + "，实际 " + actual);
}

// 创建节点并返回其 id（空 id 表示失败）
std::string add_node(Check& check, engine::Graph& graph, const std::string& type,
                     const std::string& name)
{
    std::string error;
    const std::string id = graph.addNode(type, 0.0f, 0.0f, &error);
    expect(check, !id.empty(), name, error);
    return id;
}

int graph_selftest()
{
    using engine::Graph;
    using engine::NodeCategory;
    using engine::NodeRegistry;
    using engine::PortType;

    Check check;
    std::printf("[P1] 图模型 / 节点注册表 / 撤销栈 自检\n");

    // ---------------------------------------------------- 1. 节点注册表 -----
    engine::registerAllNodes();
    NodeRegistry& registry = NodeRegistry::instance();
    expect_eq(check, std::to_string(registry.size()), "9", "注册表包含 9 个节点类型");

    const char* expected_types[] = {"TextInput",      "ImageInput",    "PromptTemplate",
                                    "TextMerge",      "ProviderConfig", "LLMGenerate",
                                    "VLMGenerate",    "TextOutput",    "ImagePreview"};
    for (const char* type : expected_types) {
        expect(check, registry.find(type) != nullptr, std::string("已注册 ") + type);
    }

    struct CategoryCount {
        NodeCategory category;
        std::size_t  count;
        const char*  name;
    };
    const CategoryCount categories[] = {
        {NodeCategory::Input, 2, "输入"}, {NodeCategory::Process, 2, "处理"},
        {NodeCategory::Config, 1, "配置"}, {NodeCategory::Inference, 2, "推理"},
        {NodeCategory::Output, 2, "输出"}};
    for (const CategoryCount& item : categories) {
        expect(check, registry.listByCategory(item.category).size() == item.count,
               std::string("分类 ") + item.name + " 节点数 = " + std::to_string(item.count));
    }

    if (const engine::Definition* text_input = registry.find("TextInput")) {
        expect(check,
               text_input->outputs.size() == 1 && text_input->outputs[0].id == "text" &&
                   text_input->outputs[0].type == PortType::Text,
               "TextInput 输出端口 = text:text");
        expect(check, text_input->params.size() == 1 && text_input->params[0].is_required,
               "TextInput 参数 text 为必填");
    }
    if (const engine::Definition* llm = registry.find("LLMGenerate")) {
        expect(check,
               llm->inputs.size() == 2 && llm->inputs[0].id == "prompt" &&
                   llm->inputs[1].type == PortType::Provider,
               "LLMGenerate 输入 = prompt:text + provider:provider");
        bool has_temperature = false;
        bool has_max_tokens  = false;
        for (const engine::Param& param : llm->params) {
            if (param.id == "temperature" && param.min_value && param.max_value) {
                has_temperature = true;
            }
            if (param.id == "max_tokens" && param.min_value && param.max_value) {
                has_max_tokens = true;
            }
        }
        expect(check, has_temperature && has_max_tokens,
               "LLMGenerate 含 temperature / max_tokens 范围");
    }
    if (const engine::Definition* provider = registry.find("ProviderConfig")) {
        bool has_mode = false;
        for (const engine::Param& param : provider->params) {
            if (param.id == "mode" &&
                std::find(param.enum_options.begin(), param.enum_options.end(),
                          std::string("official")) != param.enum_options.end()) {
                has_mode = true;
            }
        }
        expect(check,
               has_mode && provider->outputs.size() == 1 &&
                   provider->outputs[0].type == PortType::Provider,
               "ProviderConfig = provider 输出 + mode 枚举");
    }

    // ---------------------------------------------------- 2. Graph 增删 -----
    Graph graph;
    const std::string text_id = add_node(check, graph, "TextInput", "创建 TextInput 节点");
    expect_eq(check, text_id, "n1", "节点 id = n1");
    if (const engine::Node* node = graph.findNode(text_id)) {
        expect(check,
               node->params.size() == 1 &&
                   node->params[0].value == node->params[0].default_value,
               "新建节点的参数值 = 默认值");
    }

    std::string error;
    const std::string unknown_id = graph.addNode("NotExists", 0.0f, 0.0f, &error);
    expect(check, unknown_id.empty() && !error.empty(), "未注册类型被拒绝", error);

    const std::string llm_id = add_node(check, graph, "LLMGenerate", "创建 LLMGenerate 节点");
    if (!text_id.empty() && !llm_id.empty()) {
        const std::string edge_id = graph.addEdge(text_id, "text", llm_id, "prompt", &error);
        expect(check, !edge_id.empty(), "连接 text -> prompt 成功", error);
        expect_eq(check, edge_id, "e1", "连线 id = e1");

        expect(check, graph.addEdge(text_id, "text", llm_id, "prompt", &error).empty(),
               "重复连线被拒绝", error);

        expect(check, graph.addEdge(text_id, "text", text_id, "text", &error).empty(),
               "自连接被拒绝", error);

        const std::string vlm_id = add_node(check, graph, "VLMGenerate", "创建 VLMGenerate 节点");
        if (!vlm_id.empty()) {
            expect(check,
                   graph.addEdge(text_id, "text", vlm_id, "image", &error).empty() &&
                       error.find("类型不兼容") != std::string::npos,
                   "text -> image 被拒绝（类型校验）", error);

            const std::string tpl_id =
                add_node(check, graph, "PromptTemplate", "创建 PromptTemplate 节点");
            if (!tpl_id.empty()) {
                expect(check, !graph.addEdge(text_id, "text", tpl_id, "vars", &error).empty(),
                       "text -> any 端口允许连接", error);
            }
        }

        const std::string text2_id =
            add_node(check, graph, "TextInput", "创建第二个 TextInput 节点");
        if (!text2_id.empty()) {
            const std::size_t before = graph.edges.size();
            expect(check,
                   !graph.addEdge(text2_id, "text", llm_id, "prompt", &error).empty() &&
                       graph.edges.size() == before,
                   "输入端口已有连接时被替换（边数不变）", error);
            const engine::Edge* into = graph.findEdgeIntoInput(llm_id, "prompt");
            expect(check, into != nullptr && into->from_node == text2_id,
                   "替换后连线来源 = 第二个 TextInput", into != nullptr ? into->from_node : "null");
        }

        graph.removeNode(llm_id);
        bool dangling = false;
        for (const engine::Edge& item : graph.edges) {
            if (item.from_node == llm_id || item.to_node == llm_id) {
                dangling = true;
            }
        }
        expect(check, !dangling, "删除节点时相关连线被一并清理");

        // 直接删除连线（右键菜单「删除连线」/ 工具栏「删除选中」走的正是这条路径）
        Graph edge_graph;
        const std::string edge_a = add_node(check, edge_graph, "TextInput", "删边用例 A");
        const std::string edge_b = add_node(check, edge_graph, "TextOutput", "删边用例 B");
        if (!edge_a.empty() && !edge_b.empty()) {
            std::string edge_error;
            const std::string removed_edge_id =
                edge_graph.addEdge(edge_a, "text", edge_b, "text", &edge_error);
            expect(check, !removed_edge_id.empty(), "删边用例：连线创建成功", edge_error);
            expect(check, edge_graph.removeEdge(removed_edge_id), "removeEdge 删除连线成功");
            expect(check, edge_graph.edges.empty() && edge_graph.nodes.size() == 2,
                   "删边后：连线 0 条、节点保留");
            expect(check, !edge_graph.removeEdge(removed_edge_id), "重复删除同一条连线返回 false");
        }
    }

    // 可变长输入端口允许叠加多条连线（TextMerge.texts）
    {
        Graph merge_graph;
        const std::string merge_id = add_node(check, merge_graph, "TextMerge", "创建 TextMerge 节点");
        const std::string a_id     = add_node(check, merge_graph, "TextInput", "TextMerge 用例 A");
        const std::string b_id     = add_node(check, merge_graph, "TextInput", "TextMerge 用例 B");
        if (!merge_id.empty() && !a_id.empty() && !b_id.empty()) {
            merge_graph.addEdge(a_id, "text", merge_id, "texts", &error);
            merge_graph.addEdge(b_id, "text", merge_id, "texts", &error);
            expect(check, merge_graph.inputConnectionCount(merge_id, "texts") == 2,
                   "可变长输入端口可叠加多条连线");
        }
    }

    // 复制节点
    {
        Graph clone_graph;
        const std::string source_id = add_node(check, clone_graph, "TextInput", "克隆用例源节点");
        if (!source_id.empty()) {
            if (engine::Node* source = clone_graph.findNode(source_id)) {
                source->params[0].value = std::string("原始文本");
            }

            std::string clone_error;
            const std::string copy_id =
                clone_graph.cloneNode(source_id, 24.0f, 24.0f, &clone_error);
            expect(check, !copy_id.empty() && copy_id != source_id, "复制节点得到新 id", clone_error);

            const engine::Node* source = clone_graph.findNode(source_id);
            const engine::Node* copy   = clone_graph.findNode(copy_id);
            if (source != nullptr && copy != nullptr) {
                expect(check, copy->title == source->title + " 副本", "复制节点标题带「副本」");
                expect(check, copy->params[0].text() == "原始文本", "复制节点保留参数值");
                expect(check, copy->x == source->x + 24.0f, "复制节点位置偏移");
            }
        }
    }

    // ------------------------------------------------ 3. 参数校验 -----------
    {
        Graph param_graph;
        const std::string param_text_id =
            add_node(check, param_graph, "TextInput", "参数校验用 TextInput");
        if (engine::Node* text = param_graph.findNode(param_text_id)) {
            std::string reason;
            expect(check, !Graph::validateParam(text->params[0], &reason),
                   "必填文本为空 → 校验失败", reason);
            text->params[0].value = std::string("hello");
            expect(check, Graph::validateParam(text->params[0], &reason), "填写后校验通过", reason);
        }

        const std::string param_llm_id =
            add_node(check, param_graph, "LLMGenerate", "参数校验用 LLMGenerate");
        if (engine::Node* llm = param_graph.findNode(param_llm_id)) {
            engine::Param* temperature = llm->findParam("temperature");
            expect(check, temperature != nullptr, "找到 temperature 参数");
            if (temperature != nullptr) {
                std::string reason;
                temperature->value = 5.0; // 超出 0 ~ 2
                expect(check, !Graph::validateParam(*temperature, &reason),
                       "temperature 超范围 → 校验失败", reason);
                temperature->value = 0.9;
                expect(check, Graph::validateParam(*temperature, &reason),
                       "temperature 合法值 → 校验通过", reason);
            }
        }

        const std::string provider_id =
            add_node(check, param_graph, "ProviderConfig", "参数校验用 ProviderConfig");
        if (engine::Node* provider = param_graph.findNode(provider_id)) {
            engine::Param* mode = provider->findParam("mode");
            expect(check, mode != nullptr, "找到 mode 参数");
            if (mode != nullptr) {
                std::string reason;
                mode->value = std::string("unknown-mode");
                expect(check, !Graph::validateParam(*mode, &reason), "枚举非法值 → 校验失败", reason);
                mode->value = std::string("web");
                expect(check, Graph::validateParam(*mode, &reason), "枚举合法值 → 校验通过", reason);
            }
        }

        const std::string image_id =
            add_node(check, param_graph, "ImageInput", "参数校验用 ImageInput");
        if (engine::Node* image = param_graph.findNode(image_id)) {
            engine::Param* path = image->findParam("path");
            if (path != nullptr) {
                std::string reason;
                path->value = std::string("C:/not-exists/definitely-missing.png");
                expect(check, !Graph::validateParam(*path, &reason),
                       "图片路径不存在 → 校验失败", reason);
            }
        }

        expect(check,
               engine::numericIdOf("n12") == 12 && engine::numericIdOf("e3") == 3 &&
                   engine::numericIdOf("x") == 0,
               "numericIdOf 解析 n12 / e3 / 非法值");
    }

    // ------------------------------------------------ 4. 撤销 / 重做 -------
    {
        Graph current;
        engine::UndoStack undo;
        const std::string first_id = add_node(check, current, "TextInput", "撤销用例：初始节点");
        (void)first_id;

        undo.push(current, "创建第二个节点");
        const std::string second_id =
            add_node(check, current, "TextOutput", "撤销用例：第二个节点");
        (void)second_id;
        expect(check, current.nodes.size() == 2, "操作后节点数 = 2");

        Graph restored;
        std::string label;
        expect(check, undo.undo(current, restored, &label) && restored.nodes.size() == 1,
               "撤销后回到 1 个节点", label);
        expect(check, undo.canRedo(), "撤销后可以重做");
        current = restored;

        expect(check, undo.redo(current, restored, &label) && restored.nodes.size() == 2,
               "重做后回到 2 个节点", label);
        current = restored;
        expect(check, !undo.canRedo(), "重做后重做栈为空");

        Graph snapshot;
        for (int i = 0; i < 60; ++i) {
            undo.push(snapshot, "深度测试");
        }
        expect(check, undo.undoDepth() == engine::UndoStack::kMaxDepth, "撤销栈深度被限制为 50",
               std::to_string(undo.undoDepth()));

        undo.push(current, "新操作");
        expect(check, !undo.canRedo(), "产生新操作后重做分支被丢弃");

        undo.clear();
        expect(check, !undo.canUndo() && !undo.canRedo(), "clear() 清空撤销与重做栈");
    }

    // ------------------------------------------------ 5. 工作流序列化（设计 §4.7）----
    {
        Graph source;
        source.name          = "往返测试";
        source.description   = "round-trip";
        source.viewport_x    = 12.5f;
        source.viewport_y    = -30.0f;
        source.viewport_zoom = 1.5f;

        const std::string ser_a = add_node(check, source, "TextInput", "序列化用例 A");
        const std::string ser_b = add_node(check, source, "LLMGenerate", "序列化用例 B");
        const std::string ser_c = add_node(check, source, "ProviderConfig", "序列化用例 C");
        if (!ser_a.empty() && !ser_b.empty() && !ser_c.empty()) {
            std::string edge_error;
            source.addEdge(ser_a, "text", ser_b, "prompt", &edge_error);
            // 与默认工作流一致：ProviderConfig → LLMGenerate.provider
            source.addEdge(ser_c, "provider", ser_b, "provider", &edge_error);

            if (engine::Node* node_a = source.findNode(ser_a)) {
                node_a->title = "序列化用例 A";
                if (engine::Param* param = node_a->findParam("text")) {
                    param->value = std::string("往返文本");
                }
            }
            if (engine::Node* node_b = source.findNode(ser_b)) {
                if (engine::Param* param = node_b->findParam("temperature")) {
                    param->value = 1.25;
                }
            }
        }

        const nlohmann::json document = engine::graph_to_json(source);
        expect(check,
               document.contains("version") && document.contains("name") &&
                   document.contains("nodes") && document.contains("edges") &&
                   document.contains("viewport"),
               "序列化：包含 version / name / nodes / edges / viewport");
        expect(check, document["nodes"].size() == 3 && document["edges"].size() == 2,
               "序列化：节点/连线数量正确",
               std::to_string(document["nodes"].size()) + "/" + std::to_string(document["edges"].size()));

        bool secret_written = false;
        for (const auto& node_json : document["nodes"]) {
            if (node_json.contains("params") && node_json["params"].contains("api_key")) {
                secret_written = true;
            }
        }
        expect(check, !secret_written, "序列化：is_secret 参数（api_key）不写入文件");

        Graph       restored;
        std::string restore_error;
        expect(check, engine::graph_from_json(document, restored, &restore_error),
               "反序列化：成功", restore_error);
        expect(check, restored.nodes.size() == 3 && restored.edges.size() == 2,
               "反序列化：节点/连线数量一致");
        expect(check, restored.name == source.name && restored.description == source.description,
               "反序列化：name / description 一致");
        expect(check, std::fabs(restored.viewport_zoom - source.viewport_zoom) < 0.001f,
               "反序列化：viewport.zoom 一致");

        const engine::Node* restored_a = restored.findNode(ser_a);
        expect(check,
               restored_a != nullptr && restored_a->type == "TextInput" &&
                   restored_a->title == "序列化用例 A",
               "反序列化：节点 id/类型/标题一致");
        const engine::Param* restored_text =
            (restored_a != nullptr) ? restored_a->findParam("text") : nullptr;
        expect(check, restored_text != nullptr && restored_text->text() == "往返文本",
               "反序列化：文本参数一致");

        const engine::Node* restored_b = restored.findNode(ser_b);
        const engine::Param* restored_temperature =
            (restored_b != nullptr) ? restored_b->findParam("temperature") : nullptr;
        expect(check,
               restored_temperature != nullptr &&
                   std::fabs(restored_temperature->number() - 1.25) < 0.001,
               "反序列化：数值参数一致");
        const engine::Edge* into_provider = restored.findEdgeIntoInput(ser_b, "provider");
        expect(check, into_provider != nullptr && into_provider->from_node == ser_c,
               "反序列化：provider 连线一致（ProviderConfig → LLMGenerate.provider）");

        // ---- 文件往返（default.json 走的正是这条路径）----
        const std::filesystem::path temp_file =
            std::filesystem::temp_directory_path() / "aiwrite_workflow_roundtrip.json";
        std::string io_error;
        expect(check, engine::save_workflow(source, temp_file, &io_error), "保存到文件成功", io_error);
        Graph from_file;
        expect(check, engine::load_workflow(temp_file, from_file, &io_error), "从文件加载成功", io_error);
        expect(check,
               from_file.nodes.size() == source.nodes.size() &&
                   from_file.edges.size() == source.edges.size(),
               "文件往返：节点/连线数量一致");
        std::error_code remove_error;
        std::filesystem::remove(temp_file, remove_error);

        // ---- 非法输入被拒绝 ----
        std::string bad_error;
        Graph       bad_graph;
        expect(check,
               !engine::graph_from_json(nlohmann::json::object(), bad_graph, &bad_error) &&
                   !bad_error.empty(),
               "反序列化：缺少 nodes 时被拒绝");
        expect(check,
               !engine::graph_from_json(nlohmann::json{{"nodes", "not-an-array"}}, bad_graph, &bad_error),
               "反序列化：nodes 类型错误时被拒绝");
    }

    std::printf("=== 图模型自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--selftest") {
            options.selftest = true;
        }
        else if (arg == "--dry-run") {
            options.dry_run = true;
        }
        else if (arg == "--graph-selftest") {
            options.graph_selftest = true;
        }
        else if (arg == "--insecure") {
            options.insecure = true;
        }
        else if (arg == "--sha3") {
            options.sha3_text = next("--sha3");
        }
        else if (arg == "--http") {
            options.http_url = next("--http");
        }
        else if (arg == "--chat") {
            options.chat_prompt = next("--chat");
        }
        else if (arg == "--model") {
            options.model = next("--model");
        }
        else if (arg == "--api-base") {
            options.api_base = next("--api-base");
        }
        else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    aiwrite::log::init(false);
    aiwrite::log::info("api_probe 启动（M1 技术验证）");

    if (options.sha3_text == "" && !options.selftest && !options.graph_selftest &&
        options.http_url.empty() && options.chat_prompt.empty()) {
        print_usage();
        aiwrite::log::shutdown();
        return 2;
    }

    int exit_code = 0;

    if (!options.sha3_text.empty()) {
        const std::string actual = aiwrite::crypto::sha3_256(options.sha3_text);
        std::printf("SHA3-256(\"%s\") = %s\n", options.sha3_text.c_str(), actual.c_str());
    }

    if (options.selftest) {
        std::printf("=== AIwrite M1 自检 ===\n");
        const bool sha3_ok   = test_sha3();
        const int  http_code = test_http("https://api.deepseek.com", options.insecure);

        Options request_options = options;
        if (request_options.chat_prompt.empty()) {
            request_options.chat_prompt = "用一句话介绍你自己";
        }
        const bool request_ok = test_chat_request_shape(request_options);
        const bool config_ok  = test_config_roundtrip();
        const bool web_ok     = test_web_session_and_visibility();

        const char* http_text =
            (http_code == 0) ? "PASS" : (http_code == 2 ? "SKIP(网络不可达)" : "FAIL");
        std::printf("=== 结果: SHA3 %s / HTTP %s / 请求构造 %s / 配置往返 %s / 会话与可见性 %s ===\n",
                    sha3_ok ? "PASS" : "FAIL", http_text, request_ok ? "PASS" : "FAIL",
                    config_ok ? "PASS" : "FAIL", web_ok ? "PASS" : "FAIL");
        if (!sha3_ok || http_code == 1 || !request_ok || !config_ok || !web_ok) {
            exit_code = 1;
        }
    }
    else if (!options.http_url.empty()) {
        const int http_code = test_http(options.http_url, options.insecure);
        exit_code           = (http_code == 0) ? 0 : (http_code == 2 ? 2 : 1);
    }

    if (options.graph_selftest) {
        if (graph_selftest() != 0) {
            exit_code = 1;
        }
    }

    if (!options.chat_prompt.empty()) {
        const int chat_code = test_chat(options);
        if (chat_code == 1) {
            exit_code = 1;
        }
    }

    aiwrite::log::info("api_probe 结束，返回码 " + std::to_string(exit_code));
    aiwrite::log::shutdown();
    return exit_code;
}
