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
#include "utils/window_geometry.h"
#include "utils/paths.h"

#include "engine/graph.h"
#include "engine/node_registry.h"
#include "engine/recent_files.h"
#include "engine/undo_stack.h"
#include "engine/validate.h"
#include "engine/provider_resolve.h" // M5-02：生效提供商 / 模型名断言
#include "engine/workflow_io.h"
#include "nodes/nodes.h"
#include "utils/config.h"
#include "utils/output_archive.h"
#include "utils/paths.h"
#include "web/session_store.h"
#include "web/webview_host.h" // plan_session_boot / interactive_login_request（纯逻辑断言用）
#include "ai/deepseek_official_provider.h" // M5-02：多模态请求体断言

#include <fstream>
#include <sstream>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
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
    bool        exec_selftest  = false;
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
    std::printf("  api_probe --graph-selftest          # 图模型/注册表/撤销栈/序列化 自检（不需要网络）\n");
    std::printf("  api_probe --exec-selftest           # 拓扑排序 + 加载/运行前校验 + 执行器 自检（不需要网络）\n");
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
    // F3 / PD-04：窗口几何字段也要能往返
    modified.ui.window_width     = 1440;
    modified.ui.window_height    = 900;
    modified.ui.window_pos_x     = 120;
    modified.ui.window_pos_y     = 80;
    modified.ui.window_maximized = true;
    if (!aiwrite::save_config(file, modified)) {
        std::printf("   FAIL  保存配置失败\n");
        return false;
    }

    aiwrite::Config reloaded;
    if (!aiwrite::load_config(file, reloaded)) {
        std::printf("   FAIL  重新加载配置失败\n");
        return false;
    }

    const bool geometry_same = reloaded.ui.window_width == modified.ui.window_width &&
                               reloaded.ui.window_height == modified.ui.window_height &&
                               reloaded.ui.window_pos_x == modified.ui.window_pos_x &&
                               reloaded.ui.window_pos_y == modified.ui.window_pos_y &&
                               reloaded.ui.window_maximized == modified.ui.window_maximized;
    const bool same = generated && reloaded.config_version == modified.config_version &&
                      reloaded.general.language == modified.general.language &&
                      reloaded.ui.show_grid == modified.ui.show_grid &&
                      reloaded.output.ttl_days == modified.output.ttl_days &&
                      reloaded.deepseek.model == modified.deepseek.model && geometry_same;
    std::printf("   %-6s 默认生成=%s；字段往返一致=%s（language / show_grid / ttl_days / model / "
                "窗口几何）\n",
                same ? "PASS" : "FAIL", generated ? "是" : "否", same ? "是" : "否");

    // PA-07：进程级配置访问器（app_config / set_app_config）
    const aiwrite::Config before = aiwrite::app_config();
    aiwrite::set_app_config(modified);
    const bool cache_ok = aiwrite::app_config().deepseek.model == modified.deepseek.model &&
                          aiwrite::app_config().output.ttl_days == modified.output.ttl_days &&
                          aiwrite::app_config().ui.show_grid == modified.ui.show_grid;
    aiwrite::set_app_config(before); // 还原，避免污染后续用例
    std::printf("   %-6s PA-07 配置访问器：set→get 一致=%s；默认连接超时=%d ms（未设置时返回默认值）\n",
                cache_ok ? "PASS" : "FAIL", cache_ok ? "是" : "否",
                aiwrite::Config{}.timeout.connect_ms);

    std::filesystem::remove(file, ec);

    // ---- F3 / PD-04：越屏矫正纯函数（utils::fit_window_to_workarea）----
    using aiwrite::utils::WindowRect;
    const WindowRect workarea{0, 0, 1920, 1040};             // 工作区（已扣除任务栏）
    const WindowRect inside{100, 80, 1280, 720};             // 完全在工作区内
    const WindowRect offscreen_right{3000, 200, 1280, 720};  // 完全在右侧屏外
    const WindowRect offscreen_topleft{-200, -100, 1280, 720};
    const WindowRect too_large{0, 0, 3000, 2000};            // 尺寸超出工作区
    const WindowRect too_small{10, 10, 100, 80};             // 尺寸小于最小值保护

    const aiwrite::utils::FitResult keep    = aiwrite::utils::fit_window_to_workarea(inside, workarea);
    const aiwrite::utils::FitResult pulled  =
        aiwrite::utils::fit_window_to_workarea(offscreen_right, workarea);
    const aiwrite::utils::FitResult clamped =
        aiwrite::utils::fit_window_to_workarea(offscreen_topleft, workarea);
    const aiwrite::utils::FitResult shrunk  = aiwrite::utils::fit_window_to_workarea(too_large, workarea);
    const aiwrite::utils::FitResult grown   = aiwrite::utils::fit_window_to_workarea(too_small, workarea);
    const aiwrite::utils::FitResult no_area =
        aiwrite::utils::fit_window_to_workarea(inside, WindowRect{});

    const bool fit_ok = !keep.changed && keep.rect.x == 100 && keep.rect.width == 1280 &&
                        pulled.changed && pulled.rect.x == 640 && pulled.rect.y == 200 &&
                        clamped.changed && clamped.rect.x == 0 && clamped.rect.y == 0 &&
                        shrunk.changed && shrunk.rect.width == 1920 && shrunk.rect.height == 1040 &&
                        grown.changed && grown.rect.width == 640 && grown.rect.height == 480 &&
                        !no_area.changed && no_area.rect.width == 1280 &&
                        aiwrite::utils::has_position(inside) &&
                        !aiwrite::utils::has_position(WindowRect{-1, -1, 0, 0});
    std::printf("   %-6s 窗口几何越屏矫正：区内不变=%s；越屏拉回=%s（x=%d）；超大收缩=%s（%dx%d）；"
                "最小保护=%s（%dx%d）；无工作区不变=%s\n",
                fit_ok ? "PASS" : "FAIL", !keep.changed ? "是" : "否",
                pulled.rect.x == 640 ? "是" : "否", pulled.rect.x,
                shrunk.rect.width == 1920 ? "是" : "否", shrunk.rect.width, shrunk.rect.height,
                grown.rect.width == 640 ? "是" : "否", grown.rect.width, grown.rect.height,
                !no_area.changed ? "是" : "否");

    // ---- PA-08：未接配置字段清单（"改了 config 却没效果"必须可查）----
    const std::vector<std::string> unwired = aiwrite::unwired_config_fields();
    const auto                     has_unwired = [&unwired](const char* needle) {
        for (const std::string& field : unwired) {
            if (field.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    const bool unwired_ok = has_unwired("general.language") && has_unwired("timeout.") &&
                            has_unwired("output.auto_open_on_complete") &&
                            has_unwired("ui.grid_size") && !has_unwired("console_height") &&
                            !has_unwired("running_animation") && !has_unwired("window_width");
    std::printf("   %-6s PA-08 未接配置字段清单：%zu 项；含 general.language=%s；"
                "不含 console_height/running_animation/window_*=%s\n",
                unwired_ok ? "PASS" : "FAIL", unwired.size(),
                has_unwired("general.language") ? "是" : "否",
                (!has_unwired("console_height") && !has_unwired("running_animation") &&
                 !has_unwired("window_width"))
                    ? "是"
                    : "否");

    return same && cache_ok && fit_ok && unwired_ok;
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

    // ---- 会话自动引导决策（2026-09-26 真 bug 的回归断言）----
    //  旧逻辑：只要“窗口已打开”就只等 → 用户手动开的窗口从不探测 → 25s 超时 →「未取得网页版凭证」
    using aiwrite::web::SessionBoot;
    const bool boot_ok =
        aiwrite::web::plan_session_boot(true, false) == SessionBoot::HaveToken &&
        aiwrite::web::plan_session_boot(true, true) == SessionBoot::HaveToken &&
        aiwrite::web::plan_session_boot(false, true) == SessionBoot::ReuseAndProbe &&
        aiwrite::web::plan_session_boot(false, false) == SessionBoot::StartAndProbe;
    // 手动「打开登录窗口」必须在页面加载后自动探测一次（否则运行时只能等到超时）
    const aiwrite::web::LoginRequest interactive = aiwrite::web::interactive_login_request();
    const bool request_ok = interactive.probe_after_load && !interactive.offscreen &&
                            interactive.url.find("deepseek.com") != std::string::npos;

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

    const bool ok = store_ok && cleared_ok && visibility_ok && boot_ok && request_ok;
    std::printf("   %-6s 会话存储写读=%s%s；参数可见性=%s（official 显示 api_key / web 隐藏 "
                "api_key·api_base / 两模式校验均通过）\n",
                ok ? "PASS" : "FAIL", store_ok ? "OK" : "异常", cleared_ok ? "/注销清空 OK" : "/注销异常",
                visibility_ok ? "OK" : "异常");
    std::printf("         会话自动引导决策=%s（含「窗口已开→补探测 ReuseAndProbe」真值表 4 项）；"
                "手动登录窗口自动探测=%s\n",
                boot_ok ? "OK" : "异常", request_ok ? "OK" : "异常");
    return ok;
}

// 设置节点参数（节点/参数不存在时静默跳过，断言由调用方负责）
void set_param(engine::Graph& graph, const std::string& node_id, const std::string& param_id,
               const nlohmann::json& value)
{
    if (engine::Node* node = graph.findNode(node_id)) {
        if (engine::Param* param = node->findParam(param_id)) {
            param->value = value;
        }
    }
}

// M2-05：工作流文件读写 + 最近列表（不需要网络）
bool test_workflow_files()
{
    std::printf("[V-10] 工作流文件 / 最近列表自检\n");

    const std::filesystem::path work_file =
        std::filesystem::temp_directory_path() / "aiwrite_workflow_selftest.json";
    const std::filesystem::path recent_file = aiwrite::paths::recent_file();
    std::error_code             ec;
    std::filesystem::remove(work_file, ec);

    // 备份用户现有 recent.json（测试结束原样还原，避免污染最近列表）
    std::string recent_backup;
    const bool  had_recent = std::filesystem::exists(recent_file, ec);
    if (had_recent) {
        std::ifstream     in(recent_file, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        recent_backup = buffer.str();
    }

    bool ok = true;

    // ---------------------------------------------------- 1. 保存 → 加载 -----
    {
        aiwrite::engine::registerAllNodes();
        aiwrite::engine::Graph graph;
        std::string            error;
        const std::string      text_node = graph.addNode("TextInput", 0.0f, 0.0f, &error);
        const std::string      conf_node = graph.addNode("ProviderConfig", 0.0f, 0.0f, &error);
        set_param(graph, text_node, "text", "自检文本");
        set_param(graph, conf_node, "api_key", "sk-secret-file-test");
        graph.name = "自检工作流";

        const bool saved = aiwrite::engine::save_workflow(graph, work_file, &error);

        aiwrite::engine::Graph loaded;
        std::string            load_error;
        const bool             loaded_ok = aiwrite::engine::load_workflow(work_file, loaded, &load_error);
        const bool             same = loaded_ok && loaded.nodes.size() == graph.nodes.size() &&
                          loaded.edges.size() == graph.edges.size() &&
                          loaded.name == "自检工作流";
        aiwrite::engine::ValidationMessages issues;
        const bool valid = loaded_ok && aiwrite::engine::validateWorkflow(loaded, &issues);

        std::ifstream     in(work_file, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text      = buffer.str();
        const bool        no_secret = text.find("sk-secret-file-test") == std::string::npos;

        const bool pass = saved && same && valid && no_secret;
        std::printf("   %-6s 保存=%s / 往返一致=%s / 加载校验=%s / 密钥未落盘=%s\n",
                    pass ? "PASS" : "FAIL", saved ? "OK" : "失败", same ? "OK" : "不一致",
                    valid ? "OK" : "失败", no_secret ? "OK" : "文件里出现明文 Key");
        ok = ok && pass;
    }

    // -------------------------------------------- 2. 最近列表（recent.json）--
    {
        aiwrite::engine::clear_recent_files();
        const bool empty_ok = aiwrite::engine::load_recent_files().empty();

        aiwrite::engine::push_recent_file("C:/tmp/a.json", "工作流 A");
        aiwrite::engine::push_recent_file("C:/tmp/b.json", "工作流 B");
        aiwrite::engine::push_recent_file("C:/tmp/a.json", "工作流 A"); // 重复 → 置顶不重复
        std::vector<aiwrite::engine::RecentEntry> entries = aiwrite::engine::load_recent_files();
        const bool order_ok = entries.size() == 2 && entries[0].name == "工作流 A" &&
                              entries[1].name == "工作流 B";

        for (int i = 0; i < 12; ++i) {
            aiwrite::engine::push_recent_file("C:/tmp/w" + std::to_string(i) + ".json",
                                              "W" + std::to_string(i));
        }
        entries              = aiwrite::engine::load_recent_files();
        const bool cap_ok = entries.size() == aiwrite::engine::kMaxRecentFiles &&
                            entries.front().name == "W11";

        aiwrite::engine::clear_recent_files();
        const bool cleared = aiwrite::engine::load_recent_files().empty();

        const bool pass = empty_ok && order_ok && cap_ok && cleared;
        std::printf("   %-6s 初始空列表=%s / 去重置顶=%s / 上限 10 条=%s / 清空=%s\n",
                    pass ? "PASS" : "FAIL", empty_ok ? "OK" : "异常", order_ok ? "OK" : "异常",
                    cap_ok ? "OK" : "异常", cleared ? "OK" : "异常");
        ok = ok && pass;
    }

    // ------------------------------------- 3. 非法工作流文件被加载校验拒绝 ----
    {
        const std::filesystem::path bad_file =
            std::filesystem::temp_directory_path() / "aiwrite_workflow_bad.json";
        {
            std::ofstream out(bad_file, std::ios::binary | std::ios::trunc);
            out << "{\"version\":\"1.0\",\"name\":\"坏工作流\",\"nodes\":[{\"id\":\"n1\","
                   "\"type\":\"NotExists\",\"title\":\"未知\",\"position\":{\"x\":0,\"y\":0},"
                   "\"params\":{}}],\"edges\":[]}";
        }

        aiwrite::engine::Graph loaded;
        std::string            error;
        const bool             loaded_ok = aiwrite::engine::load_workflow(bad_file, loaded, &error);
        aiwrite::engine::ValidationMessages issues;
        const bool valid    = loaded_ok && aiwrite::engine::validateWorkflow(loaded, &issues);
        const bool rejected = !valid;
        std::printf("   %-6s 含未知节点类型的文件被拒绝（加载%s / 校验%s）\n",
                    rejected ? "PASS" : "FAIL", loaded_ok ? "成功" : "失败",
                    valid ? "通过（异常）" : "拒绝");
        ok = ok && rejected;
        std::filesystem::remove(bad_file, ec);
    }

    // -------------------------------------------------- 清理 / 还原现场 -----
    std::filesystem::remove(work_file, ec);
    if (had_recent) {
        std::ofstream out(recent_file, std::ios::binary | std::ios::trunc);
        out << recent_backup;
    }
    else {
        std::filesystem::remove(recent_file, ec);
    }
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

        // ---- 参数批量应用（F2 / PD-05：把当前节点参数应用到图中全部同类型节点）----
        {
            Graph             batch_graph;
            const std::string a_id = add_node(check, batch_graph, "LLMGenerate", "批量A");
            const std::string b_id = add_node(check, batch_graph, "LLMGenerate", "批量B");
            const std::string c_id = add_node(check, batch_graph, "LLMGenerate", "批量C");
            const std::string t_id = add_node(check, batch_graph, "TextInput", "批量文本");

            if (engine::Node* a = batch_graph.findNode(a_id)) {
                if (engine::Param* temperature = a->findParam("temperature")) {
                    temperature->value = 1.25;
                }
                if (engine::Param* system_prompt = a->findParam("system_prompt")) {
                    system_prompt->value = std::string("批量系统提示词");
                }
                if (engine::Param* mode = a->findParam("mode")) {
                    mode->value = std::string("official");
                }
                a->x = 100.0f; // 位置不应被批量应用带走
                a->y = 200.0f;
            }

            std::string batch_error;
            const int   affected = batch_graph.copyParamsToSameType(a_id, &batch_error);
            expect(check, affected == 2, "批量应用：更新 2 个同类节点", batch_error);

            const engine::Node* b = batch_graph.findNode(b_id);
            const engine::Node* c = batch_graph.findNode(c_id);
            if (b != nullptr && c != nullptr) {
                const engine::Param* b_temp = b->findParam("temperature");
                const engine::Param* b_sys  = b->findParam("system_prompt");
                const engine::Param* b_mode = b->findParam("mode");
                const engine::Param* c_temp = c->findParam("temperature");
                expect(check,
                       b_temp != nullptr && std::fabs(b_temp->number() - 1.25) < 0.001 &&
                           c_temp != nullptr && std::fabs(c_temp->number() - 1.25) < 0.001,
                       "批量应用：数值参数同步");
                expect(check, b_sys != nullptr && b_sys->text() == "批量系统提示词",
                       "批量应用：文本参数同步");
                expect(check, b_mode != nullptr && b_mode->text() == "official",
                       "批量应用：枚举参数同步");
                expect(check, b->title == c->title && b->x == 0.0f && b->y == 0.0f &&
                                 c->x == 0.0f && c->y == 0.0f,
                       "批量应用：不改动标题与位置");
            }

            const engine::Node* text_node = batch_graph.findNode(t_id);
            if (text_node != nullptr) {
                const engine::Param* text_param = text_node->findParam("text");
                expect(check, text_param != nullptr && text_param->text().empty(),
                       "批量应用：异类节点不受影响");
            }

            // 密钥类参数不复制（避免误扩散），其余同类参数正常复制
            Graph             secret_graph;
            const std::string s1 = add_node(check, secret_graph, "ProviderConfig", "密钥A");
            const std::string s2 = add_node(check, secret_graph, "ProviderConfig", "密钥B");
            if (engine::Node* provider = secret_graph.findNode(s1)) {
                if (engine::Param* key = provider->findParam("api_key")) {
                    key->value = std::string("sk-secret");
                }
                if (engine::Param* base = provider->findParam("api_base")) {
                    base->value = std::string("https://example.test");
                }
            }
            expect(check, secret_graph.copyParamsToSameType(s1, nullptr) == 1,
                   "批量应用：同类 ProviderConfig 命中 1 个");
            const engine::Node* target_provider = secret_graph.findNode(s2);
            if (target_provider != nullptr) {
                const engine::Param* key  = target_provider->findParam("api_key");
                const engine::Param* base = target_provider->findParam("api_base");
                expect(check, key != nullptr && key->text().empty(),
                       "批量应用：is_secret 参数不复制");
                expect(check, base != nullptr && base->text() == "https://example.test",
                       "批量应用：同类非密钥参数已复制");
            }

            // 未知节点 → 返回 0 且写 error（不崩溃、不改图）
            std::string missing_error;
            expect(check,
                   batch_graph.copyParamsToSameType("n404", &missing_error) == 0 &&
                       !missing_error.empty(),
                   "批量应用：未知节点返回 0 并写 error");
        }
    }

    std::printf("=== 图模型自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

// ------------------------------------------------------ 执行器自检（P3-1）-----
engine::Edge make_edge(const std::string& id, const std::string& from_node,
                       const std::string& from_port, const std::string& to_node,
                       const std::string& to_port)
{
    engine::Edge edge;
    edge.id        = id;
    edge.from_node = from_node;
    edge.from_port = from_port;
    edge.to_node   = to_node;
    edge.to_port   = to_port;
    return edge;
}

bool contains_text(const std::vector<std::string>& messages, const std::string& needle)
{
    return std::any_of(messages.begin(), messages.end(), [&needle](const std::string& text) {
        return text.find(needle) != std::string::npos;
    });
}

// M2-03 拓扑排序 + M2-06 加载/运行前校验（P3-2 会在此追加执行相关断言）
int executor_selftest()
{
    using engine::Graph;
    using engine::ValidationMessages;

    Check check;
    std::printf("[P3-1] 拓扑排序 / 加载校验 / 运行前校验 自检\n");
    engine::registerAllNodes();

    // ------------------------------------------------ 1. 拓扑排序（Kahn）-----
    {
        Graph                    graph;
        std::vector<std::string> order;
        std::string              error;
        expect(check, graph.topologicalOrder(&order, &error) && order.empty(),
               "空图：拓扑排序成功且序列为空", error);
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "线性图 n1");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "线性图 n2");
        const std::string n3 = add_node(check, graph, "TextOutput", "线性图 n3");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n2, "text", n3, "text"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 3, "线性图：拓扑排序成功", error);
        if (order.size() == 3) {
            expect(check, order[0] == n1 && order[1] == n2 && order[2] == n3,
                   "线性图：顺序为 n1→n2→n3", order[0] + "→" + order[1] + "→" + order[2]);
        }
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "菱形图 n1");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "菱形图 n2");
        const std::string n3 = add_node(check, graph, "PromptTemplate", "菱形图 n3");
        const std::string n4 = add_node(check, graph, "TextMerge", "菱形图 n4");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n1, "text", n3, "vars"));
        graph.edges.push_back(make_edge("e3", n2, "text", n4, "texts"));
        graph.edges.push_back(make_edge("e4", n3, "text", n4, "texts"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 4 && order.front() == n1 && order.back() == n4,
               "菱形图：起点最先、汇点最后", error);
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "孤立节点 n1");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "孤立节点 n2");
        const std::string n3 = add_node(check, graph, "TextOutput", "孤立节点 n3");
        graph.edges.push_back(make_edge("e1", n2, "text", n3, "text"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 3, "孤立节点也进入拓扑序列", error);
        expect(check, ok && !order.empty() && order.front() == n1, "孤立节点（无入边）排在最前");
    }

    {
        Graph             graph;
        const std::string p1 = add_node(check, graph, "PromptTemplate", "环图 p1");
        const std::string p2 = add_node(check, graph, "PromptTemplate", "环图 p2");
        graph.edges.push_back(make_edge("e1", p1, "text", p2, "vars"));
        graph.edges.push_back(make_edge("e2", p2, "text", p1, "vars"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, !ok && !error.empty(), "两节点环：拓扑排序失败");
        expect(check, !ok && error.find(p1) != std::string::npos &&
                         error.find(p2) != std::string::npos,
               "环错误信息列出环内节点", error);
    }
    {
        Graph             graph;
        const std::string p1 = add_node(check, graph, "PromptTemplate", "自环图 p1");
        graph.edges.push_back(make_edge("e1", p1, "text", p1, "vars"));

        std::vector<std::string> order;
        std::string              error;
        expect(check, !graph.topologicalOrder(&order, &error), "自环：拓扑排序失败", error);
    }
    {
        Graph                    graph;
        std::vector<std::string> ids;
        for (int i = 0; i < 20; ++i) {
            const std::string id =
                add_node(check, graph, "PromptTemplate", "20 节点链 #" + std::to_string(i + 1));
            ids.push_back(id);
            if (i > 0) {
                graph.edges.push_back(
                    make_edge("e" + std::to_string(i), ids[i - 1], "text", ids[i], "vars"));
            }
        }
        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 20, "20 节点链：拓扑排序成功", error);
        expect(check, ok && !order.empty() && order.front() == ids.front() && order.back() == ids.back(),
               "20 节点链：首尾顺序正确");
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "PromptTemplate", "悬空边图 n1");
        graph.edges.push_back(make_edge("e1", "nope", "text", n1, "vars"));

        std::vector<std::string> order;
        std::string              error;
        expect(check, graph.topologicalOrder(&order, &error) && order.size() == 1,
               "悬空边被拓扑排序忽略（交给校验报告）", error);

        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "起点节点不存在"),
               "悬空边：加载校验拒绝");
    }

    // ------------------------------------- 2. 加载校验 / 运行前校验（示例图）----
    Graph             sample;
    const std::string text_in = add_node(check, sample, "TextInput", "示例 n1（文本输入）");
    const std::string tmpl    = add_node(check, sample, "PromptTemplate", "示例 n2（模板）");
    const std::string llm     = add_node(check, sample, "LLMGenerate", "示例 n3（文本生成）");
    const std::string out     = add_node(check, sample, "TextOutput", "示例 n4（文本输出）");
    const std::string provider = add_node(check, sample, "ProviderConfig", "示例 n5（提供商配置）");
    sample.edges.push_back(make_edge("e1", text_in, "text", tmpl, "vars"));
    sample.edges.push_back(make_edge("e2", tmpl, "text", llm, "prompt"));
    sample.edges.push_back(make_edge("e3", provider, "provider", llm, "provider"));
    sample.edges.push_back(make_edge("e4", llm, "text", out, "text"));
    if (engine::Node* node = sample.findNode(text_in)) {
        if (engine::Param* param = node->findParam("text")) {
            param->value = "标题：秋日";
        }
    }
    {
        ValidationMessages errors;
        const bool         ok = engine::validateWorkflow(sample, &errors);
        expect(check, ok, "示例工作流（5 节点 4 连线）：加载校验通过",
               errors.empty() ? std::string() : errors.front());
    }
    { // 无 Key 时只给 warning，不阻断（临时清环境变量后恢复）
        std::string saved;
        if (const char* value = std::getenv("DEEPSEEK_API_KEY")) {
            saved = value;
            if (!saved.empty()) {
                _putenv_s("DEEPSEEK_API_KEY", "");
            }
        }
        ValidationMessages errors;
        ValidationMessages warnings;
        const bool         ok = engine::validateBeforeRun(sample, &errors, &warnings);
        expect(check, ok, "未配置 Key：运行前校验仍通过（不阻断）");
        expect(check, contains_text(warnings, "未配置 API Key"), "未配置 Key：给出 warning");
        if (!saved.empty()) {
            _putenv_s("DEEPSEEK_API_KEY", saved.c_str());
        }
    }

    { // 必填输入未连接：LLMGenerate.prompt（结构仍合法 → 仅运行前拒绝）
        Graph graph = sample;
        graph.removeEdge("e2");
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings),
               "必填输入未连接：运行前校验拒绝");
        expect(check, contains_text(errors, "必填输入") && contains_text(errors, llm),
               "错误信息含节点 id 与「必填输入」", errors.empty() ? std::string() : errors.front());
        ValidationMessages load_errors;
        expect(check, engine::validateWorkflow(graph, &load_errors),
               "必填输入未连接：加载校验仍通过（分层清晰）");
    }
    { // 可变长输入无入边：TextMerge.texts
        Graph             graph = sample;
        const std::string merge = add_node(check, graph, "TextMerge", "悬空 TextMerge");
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings),
               "可变长输入无入边：运行前校验拒绝");
        expect(check, contains_text(errors, merge) && contains_text(errors, "至少需要 1 条连线"),
               "错误信息说明可变长输入要求");
    }
    { // 必填参数为空：PromptTemplate.template
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(tmpl)) {
            if (engine::Param* param = node->findParam("template")) {
                param->value = "";
            }
        }
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) && contains_text(errors, "必填"),
               "必填参数为空：加载校验拒绝");
    }
    { // 端口类型不兼容：Text 输出 → Image 输入
        Graph             graph = sample;
        const std::string preview = add_node(check, graph, "ImagePreview", "图片预览");
        graph.edges.push_back(make_edge("e9", llm, "text", preview, "image"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "端口类型不兼容"),
               "端口类型不兼容：加载校验拒绝");
    }
    { // 端口不存在
        Graph graph = sample;
        graph.edges.push_back(make_edge("e9", tmpl, "nope", out, "text"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "起点端口不存在"),
               "端口不存在：加载校验拒绝");
    }
    { // 非可变长输入被重复占用
        Graph graph = sample;
        graph.edges.push_back(make_edge("e9", tmpl, "text", out, "text"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "重复占用"),
               "输入端口重复占用：加载校验拒绝");
    }
    { // 未知节点类型
        Graph       graph;
        engine::Node unknown;
        unknown.id   = "n1";
        unknown.type = "NotExists";
        graph.nodes.push_back(unknown);
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "未知节点类型"),
               "未知节点类型：加载校验拒绝");
    }
    { // 缺少提供商配置节点 → warning（provider 输入可选，不阻断）
        Graph graph = sample;
        graph.removeNode(provider);
        ValidationMessages errors;
        ValidationMessages warnings;
        const bool         ok = engine::validateBeforeRun(graph, &errors, &warnings);
        expect(check, ok, "缺少提供商节点：运行前校验仍通过（provider 为可选输入）");
        expect(check, contains_text(warnings, "提供商配置"), "缺少提供商节点：给出 warning");
    }
    { // web 模式 → 登录提示 warning（不阻断）
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(provider)) {
            if (engine::Param* mode = node->findParam("mode")) {
                mode->value = "web";
            }
        }
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, engine::validateBeforeRun(graph, &errors, &warnings), "web 模式：运行前校验通过");
        expect(check, contains_text(warnings, "网页版模式"), "web 模式：给出登录提示 warning");
    }
    { // 空图
        Graph              graph;
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "工作流为空"),
               "空图：加载校验拒绝");
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings), "空图：运行前校验拒绝");
    }

    std::printf("=== 执行器自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

// P3-2：执行器核心（分帧 / 值传递 / 失败传播 / 取消 / 安全）
int execution_selftest()
{
    using engine::ExecState;
    using engine::Executor;
    using engine::Graph;
    using engine::NodeState;

    Check check;
    std::printf("[P3-2] 执行器核心自检（本地节点链路 / 失败传播 / 取消）\n");

    engine::registerAllNodes();
    aiwrite::nodes::registerAllExecutors();
    expect_eq(check, std::to_string(engine::NodeExecutorRegistry::instance().size()), "9",
              "已注册 9 个节点执行函数");

    // ------------------------------------------------ 1. 本地链路端到端 ------
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "链路 n1（文本输入）");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "链路 n2（模板）");
        const std::string n3 = add_node(check, graph, "TextOutput", "链路 n3（输出）");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n2, "text", n3, "text"));
        set_param(graph, n1, "text", "标题：秋日");
        set_param(graph, n2, "template", "请根据以下内容续写：\n{vars}");

        std::vector<std::string> console_lines;
        std::vector<std::string> state_log;
        Executor                 executor;
        executor.setConsoleHandler(
            [&console_lines](const std::string& text) { console_lines.push_back(text); });
        executor.setStateHandler([&state_log](const std::string& id, NodeState state) {
            state_log.push_back(id + ":" + engine::nodeStateName(state));
        });

        std::string error;
        expect(check, executor.start(graph, &error), "本地链路：start 通过（运行前校验）", error);
        expect(check, executor.totalCount() == 3, "本地链路：计划含 3 个节点");

        std::vector<std::string> order;
        graph.topologicalOrder(&order, nullptr);
        expect(check, executor.plan() == order, "本地链路：执行计划 == 拓扑序");

        // 分帧：一次 tick 只推进一个节点
        expect(check, executor.tick(&graph), "本地链路：tick #1 推进成功");
        expect(check, executor.finishedCount() == 1, "本地链路：一帧只执行一个节点");

        expect(check, executor.runToCompletion(&graph, 64), "本地链路：runToCompletion 正常结束");
        expect(check, executor.state() == ExecState::Finished, "本地链路：会话状态 Finished");
        expect(check, executor.finishedCount() == 3 && executor.failedCount() == 0,
               "本地链路：完成 3 / 失败 0");

        const engine::Node* node1 = graph.findNode(n1);
        const engine::Node* node2 = graph.findNode(n2);
        const engine::Node* node3 = graph.findNode(n3);
        expect(check,
               node1 != nullptr && node1->state == NodeState::Done && node2 != nullptr &&
                   node2->state == NodeState::Done && node3 != nullptr &&
                   node3->state == NodeState::Done,
               "本地链路：三个节点状态均为 done");

        // TextOutput 节点只有输入端口 → 检查「上游模板节点的输出」与「Console 中的文本输出行」
        const nlohmann::json* templated = executor.outputs().find(n2, "text");
        expect_eq(check, templated != nullptr ? templated->dump() : std::string("(无输出)"),
                  nlohmann::json("请根据以下内容续写：\n标题：秋日").dump(),
                  "本地链路：模板节点输出已替换 {vars}");
        const bool printed =
            std::any_of(console_lines.begin(), console_lines.end(), [](const std::string& line) {
                return line.find("[文本输出]") != std::string::npos &&
                       line.find("标题：秋日") != std::string::npos;
            });
        expect(check, printed, "本地链路：文本输出节点把结果写入 Console 回调");
        expect(check, !console_lines.empty(), "本地链路：console 回调收到输出");
        expect(check, !state_log.empty() && state_log.front() == n1 + ":running",
               "本地链路：状态回调首个事件为 n1:running");
    }

    // ------------------------------------------- 2. 失败传播（设计 §10.4）----
    {
        Graph             graph;
        const std::string t1 = add_node(check, graph, "TextInput", "失败图 n1");
        const std::string t2 = add_node(check, graph, "PromptTemplate", "失败图 n2");
        const std::string l1 = add_node(check, graph, "LLMGenerate", "失败图 n3（生成·占位）");
        // 自检不联网：把生成模式置为 official（官方 API 占位分支，快速失败且信息稳定）
        if (engine::Node* llm_node = graph.findNode(l1)) {
            if (engine::Param* mode = llm_node->findParam("mode")) {
                mode->value = std::string("official");
            }
        }
        const std::string o1 = add_node(check, graph, "TextOutput", "失败图 n4（下游）");
        const std::string t3 = add_node(check, graph, "TextInput", "失败图 n5（无关分支）");
        const std::string o2 = add_node(check, graph, "TextOutput", "失败图 n6（无关分支）");
        graph.edges.push_back(make_edge("e1", t1, "text", t2, "vars"));
        graph.edges.push_back(make_edge("e2", t2, "text", l1, "prompt"));
        graph.edges.push_back(make_edge("e3", l1, "text", o1, "text"));
        graph.edges.push_back(make_edge("e4", t3, "text", o2, "text"));
        set_param(graph, t1, "text", "输入 A");
        set_param(graph, t3, "text", "输入 B");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "失败传播：start 通过", error);
        expect(check, executor.runToCompletion(&graph, 64),
               "失败传播：runToCompletion 正常结束（会话 Finished）");
        expect(check, executor.state() == ExecState::Finished, "失败传播：会话状态 Finished");

        const engine::Node* llm = graph.findNode(l1);
        expect(check, llm != nullptr && llm->state == NodeState::Error,
               "失败传播：LLMGenerate 节点标记 error");
        expect(check, llm != nullptr && llm->error_message.find("缺少 API Key") != std::string::npos,
               "失败传播：错误信息说明「缺少 API Key」", llm != nullptr ? llm->error_message : std::string());
        const engine::Node* downstream = graph.findNode(o1);
        expect(check, downstream != nullptr && downstream->state == NodeState::Skipped,
               "失败传播：下游节点标记 skipped");
        const engine::Node* unrelated = graph.findNode(o2);
        expect(check, unrelated != nullptr && unrelated->state == NodeState::Done,
               "失败传播：无关分支继续执行（done）");
        expect(check, executor.failedCount() == 1 && executor.skippedCount() == 1,
               "失败传播：计数 失败 1 / 跳过 1");
        expect(check, executor.summary().find("完成 4/6") != std::string::npos,
               "失败传播：summary 反映 完成 4/6", executor.summary());

        // PA-01：运行信息只读暴露（输出面板 / workflow.log 明细的数据来源）
        const auto& infos = executor.runInfos();
        expect(check, infos.size() == graph.nodes.size(), "运行信息：记录数等于节点数",
               std::to_string(infos.size()));
        const auto find_info = [&infos](const std::string& id) -> const engine::NodeRunInfo* {
            for (const engine::NodeRunInfo& info : infos) {
                if (info.node_id == id) {
                    return &info;
                }
            }
            return nullptr;
        };
        const engine::NodeRunInfo* llm_info  = find_info(l1);
        const engine::NodeRunInfo* skip_info = find_info(o1);
        const engine::NodeRunInfo* done_info = find_info(t1);
        expect(check,
               llm_info != nullptr && llm_info->state == NodeState::Error &&
                   llm_info->duration_ms >= 0.0 && !llm_info->error.empty(),
               "运行信息：失败节点含 error 与耗时");
        expect(check, skip_info != nullptr && skip_info->state == NodeState::Skipped,
               "运行信息：跳过节点被记录");
        expect(check,
               done_info != nullptr && done_info->state == NodeState::Done &&
                   done_info->type == "TextInput",
               "运行信息：完成节点含类型");
        double sum_ms = 0.0;
        for (const engine::NodeRunInfo& info : infos) {
            sum_ms += info.duration_ms;
        }
        expect(check, sum_ms <= executor.elapsedSeconds() * 1000.0 + 5.0,
               "运行信息：节点耗时之和不超过总耗时", std::to_string(sum_ms) + " ms");
        executor.reset();
        expect(check, executor.runInfos().empty(), "运行信息：reset 后清空");
    }

    // ------------------------------------------- 3. ProviderConfig 安全性 ----
    {
        Graph             graph;
        const std::string p = add_node(check, graph, "ProviderConfig", "配置节点");
        set_param(graph, p, "api_key", "sk-secret-should-not-leak");
        set_param(graph, p, "model", "deepseek-reasoner");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "提供商节点：start 通过", error);
        executor.runToCompletion(&graph, 16);

        const nlohmann::json* provider = executor.outputs().find(p, "provider");
        expect(check, provider != nullptr && provider->value("has_api_key", false),
               "提供商节点：输出 has_api_key = true");
        const std::string dump = provider != nullptr ? provider->dump() : std::string();
        expect(check, dump.find("sk-secret") == std::string::npos,
               "提供商节点：输出**不含** Key 明文（设计 §8.4）", dump);
        expect(check, dump.find("deepseek-reasoner") != std::string::npos,
               "提供商节点：输出包含模型名");
    }

    // ------------------------------------------- 4. 文本合并 / 模板容错 -----
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "TextInput", "合并 a");
        const std::string b = add_node(check, graph, "TextInput", "合并 b");
        const std::string m = add_node(check, graph, "TextMerge", "合并节点");
        const std::string t = add_node(check, graph, "PromptTemplate", "模板（含未知占位符）");
        graph.edges.push_back(make_edge("e1", a, "text", m, "texts"));
        graph.edges.push_back(make_edge("e2", b, "text", m, "texts"));
        graph.edges.push_back(make_edge("e3", m, "text", t, "vars"));
        set_param(graph, a, "text", "第一段");
        set_param(graph, b, "text", "第二段");
        set_param(graph, m, "separator", "|");
        set_param(graph, t, "template", "X{unknown}Y{vars}Z");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "合并/模板：start 通过", error);
        executor.runToCompletion(&graph, 32);

        const nlohmann::json* merged = executor.outputs().find(m, "text");
        expect_eq(check, merged != nullptr ? merged->dump() : std::string("(无输出)"),
                  nlohmann::json("第一段|第二段").dump(), "文本合并：按分隔符拼接");
        const nlohmann::json* templated = executor.outputs().find(t, "text");
        expect_eq(check, templated != nullptr ? templated->dump() : std::string("(无输出)"),
                  nlohmann::json("X{unknown}Y第一段|第二段Z").dump(),
                  "提示词模板：未匹配占位符原样保留，{vars} 正常替换");
    }

    // ------------------------------------------- 5. 图片输入（文件不存在）----
    {
        Graph             graph;
        const std::string img = add_node(check, graph, "ImageInput", "图片输入");
        set_param(graph, img, "path", "Z:/not-exists/aiwrite-selftest.png");

        Executor    executor;
        std::string error;
        expect(check, !executor.start(graph, &error) && error.find("不存在") != std::string::npos,
               "图片输入：文件不存在 → 运行前校验直接拒绝", error);
    }

    // ------------------------------------------- 6. 取消（协作式）------------
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "TextInput", "取消 a");
        const std::string b = add_node(check, graph, "PromptTemplate", "取消 b");
        const std::string c = add_node(check, graph, "TextOutput", "取消 c");
        graph.edges.push_back(make_edge("e1", a, "text", b, "vars"));
        graph.edges.push_back(make_edge("e2", b, "text", c, "text"));
        set_param(graph, a, "text", "取消测试");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "取消：start 通过", error);
        executor.tick(&graph);
        executor.cancel();
        executor.tick(&graph);
        expect(check, executor.state() == ExecState::Cancelled, "取消：会话状态 Cancelled");
        const engine::Node* node_c = graph.findNode(c);
        expect(check, node_c != nullptr && node_c->state == NodeState::Waiting,
               "取消：未执行的节点保持 waiting");
        expect(check, executor.finishedCount() == 1, "取消：已完成 1 个节点");
    }

    // ------------------------------------------- 7. 运行前校验失败 ----------
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "PromptTemplate", "校验失败 a");
        add_node(check, graph, "TextOutput", "校验失败 b（必填输入未连）");
        set_param(graph, a, "template", "只有模板");

        Executor    executor;
        std::string error;
        expect(check, !executor.start(graph, &error) && executor.state() == ExecState::Failed,
               "运行前校验失败：start 拒绝且会话状态 Failed", error);
    }

    // ------------------------------------------- 8. PC-05 运行输出归档 -------
    {
        const std::filesystem::path temp_root =
            std::filesystem::temp_directory_path() / "aiwrite_archive_selftest";
        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);

        std::vector<aiwrite::utils::ArchiveNode> nodes;
        aiwrite::utils::ArchiveNode              a;
        a.node_id = "n1"; a.type = "TextInput";  a.state = "done";  a.duration_ms = 0.5;
        a.text    = "第一段文本";
        aiwrite::utils::ArchiveNode b;
        b.node_id = "n2"; b.type = "TextOutput"; b.state = "done";  b.duration_ms = 1.5;
        b.text    = "最终生成的文档";
        aiwrite::utils::ArchiveNode c;
        c.node_id = "n3"; c.type = "LLMGenerate"; c.state = "error"; c.duration_ms = 12.0;
        c.error   = "缺少 API Key";
        nodes = {a, b, c};

        aiwrite::utils::ArchiveRequest request;
        request.workflow_name = "自检:非法?名字";
        request.archive_dir   = temp_root.string();
        request.keep_history  = false;
        request.ttl_days      = 0;

        const aiwrite::utils::ArchiveResult first =
            aiwrite::utils::archive_run(request, nodes, "完成 2/3，失败 1");
        const std::filesystem::path first_dir(first.dir);
        expect(check, first.ok && !first.dir.empty(), "归档：首次归档成功", first.error);
        expect(check, std::filesystem::exists(first_dir / "run.json"), "归档：run.json 已写出");
        expect(check, std::filesystem::exists(first_dir / "n2-TextOutput.txt"),
               "归档：有输出的节点写出 .txt");
        expect(check, !std::filesystem::exists(first_dir / "n3-LLMGenerate.txt"),
               "归档：无输出的失败节点不写 .txt（仅进 run.json）");
        const std::string dir_name = first_dir.filename().string();
        expect(check, dir_name.find('?') == std::string::npos && dir_name.find(':') == std::string::npos,
               "归档：工作流名中的非法字符被清洗", dir_name);
        {
            std::ifstream     in(first_dir / "n2-TextOutput.txt", std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            const std::string content = buffer.str();
            expect(check,
                   content.find("最终生成的文档") != std::string::npos &&
                       content.find("n2") != std::string::npos &&
                       content.find("状态: done") != std::string::npos,
                   "归档：节点文件含元信息头与正文");
        }
        {
            std::ifstream     in(first_dir / "run.json", std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            const nlohmann::json run_json = nlohmann::json::parse(buffer.str(), nullptr, false);
            // 期望：':' 与 '?' 被清洗为 '_'（archive_run 的 sanitize_name 规则）
            expect(check,
                   !run_json.is_discarded() &&
                       run_json.value("workflow", std::string()) == "自检_非法_名字" &&
                       run_json.contains("nodes") && run_json["nodes"].size() == 3 &&
                       run_json.value("summary", std::string()) == "完成 2/3，失败 1",
                   "归档：run.json 记录工作流名 / 统计 / 3 条节点明细");
        }

        const aiwrite::utils::ArchiveResult second =
            aiwrite::utils::archive_run(request, nodes, "完成 2/3，失败 1");
        std::size_t dir_count = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(temp_root, ec)) {
            if (entry.is_directory(ec)) {
                ++dir_count;
            }
        }
        expect(check, second.ok && dir_count == 1,
               "归档：keep_history=false 只保留最近 1 份", std::to_string(dir_count) + " 份");

        request.archive_dir = (temp_root / "not_a_dir").string();
        {
            std::ofstream blocker(request.archive_dir);
            blocker << "x";
        }
        const aiwrite::utils::ArchiveResult failed =
            aiwrite::utils::archive_run(request, nodes, "x");
        expect(check, !failed.ok && !failed.error.empty(),
               "归档：不可写路径返回错误而不抛异常", failed.error);

        // TTL：造一个 40 天前的旧目录 → ttl_days=30 时被清理（同批的新目录保留）
        {
            const std::time_t old_time = std::time(nullptr) - 40 * 86400;
            std::tm           old_tm{};
#if defined(_WIN32)
            localtime_s(&old_tm, &old_time);
#else
            localtime_r(&old_time, &old_tm);
#endif
            std::ostringstream name;
            name << std::put_time(&old_tm, "%Y%m%d-%H%M%S") << "-旧归档";
            const std::filesystem::path old_dir = temp_root / name.str();
            std::filesystem::create_directories(old_dir, ec);
            std::ofstream marker(old_dir / "run.json");
            marker << "{}";
            marker.close();

            aiwrite::utils::ArchiveRequest ttl_request = request;
            ttl_request.archive_dir = temp_root.string();
            ttl_request.ttl_days    = 30;
            const aiwrite::utils::ArchiveResult ttl_result =
                aiwrite::utils::archive_run(ttl_request, nodes, "ttl 用例");

            std::string remaining;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(temp_root, ec)) {
                if (entry.is_directory(ec)) {
                    remaining += entry.path().filename().string() + " ";
                }
            }
            expect(check,
                   ttl_result.ok && !std::filesystem::exists(old_dir) &&
                       std::filesystem::exists(std::filesystem::path(ttl_result.dir) / "run.json"),
                   "归档：ttl_days=30 清理超期目录且保留本次归档",
                   "ok=" + std::to_string(ttl_result.ok) +
                       " old_exists=" + std::to_string(std::filesystem::exists(old_dir)) +
                       " new=" + ttl_result.dir + " 剩余[" + remaining + "]");
        }

        std::filesystem::remove_all(temp_root, ec);
    }

    // --------------------------------------- M5-02：多模态请求体（离线断言）-----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  vlm_root = fs::temp_directory_path(ec) / "aiwrite-selftest-vlm";
        std::filesystem::remove_all(vlm_root, ec);
        fs::create_directories(vlm_root, ec);
        const fs::path png_a      = vlm_root / "样例图.PNG";  // 大写扩展名：顺带验 MIME 大小写
        const fs::path png_b      = vlm_root / "第二张.png";
        const fs::path empty_file = vlm_root / "空图.png";

        // Python 生成的合法 1×1 PNG（黑 / 红）——十六进制转字节写盘，保证断言可复现
        const std::string png_a_hex =
            "89504E470D0A1A0A0000000D4948445200000001000000010802000000907753"
            "DE0000000C4944415478DA63606060000000040001C8EAEBF90000000049454E"
            "44AE426082"
;
        const std::string png_b_hex =
            "89504E470D0A1A0A0000000D4948445200000001000000010802000000907753"
            "DE0000000C4944415478DA63F8CFC0000003010100F70341430000000049454E"
            "44AE426082"
;
        const auto write_hex = [](const std::filesystem::path& path,
                                  const std::string& hex) {
            std::ofstream out(path, std::ios::binary);
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                const char byte = static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
                out.write(&byte, 1);
            }
        };
        write_hex(png_a, png_a_hex);
        write_hex(png_b, png_b_hex);
        {
            std::ofstream empty(empty_file, std::ios::binary);
        }
        expect(check, fs::exists(png_a) && fs::file_size(png_a) > 0 && fs::exists(png_b),
               "M5-02 夹具：两张 1×1 PNG 已写入临时目录");

        // ---- 1) MIME 推断（大小写不敏感；未知扩展名回退 image/png）----
        expect_eq(check, aiwrite::ai::image_mime_from_path("a.PNG"), "image/png",
                  "M5-02 MIME：.PNG（大写）→ image/png");
        expect_eq(check, aiwrite::ai::image_mime_from_path("b.JpEg"), "image/jpeg",
                  "M5-02 MIME：.JpEg → image/jpeg");
        expect_eq(check, aiwrite::ai::image_mime_from_path("c.webp"), "image/webp",
                  "M5-02 MIME：.webp → image/webp");
        expect_eq(check, aiwrite::ai::image_mime_from_path("d.tif"), "image/png",
                  "M5-02 MIME：未知扩展名回退 image/png");

        // ---- 2) 编码成功路径（与 Python 参考实现的 base64 逐字符比对）----
        {
            std::string       error;
            const std::string encoded =
                aiwrite::ai::encode_image_data_url(png_a.string(), 8 * 1024 * 1024, &error);
            expect_eq(check, encoded, std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNgYGAAAAAEAAHI6uv5AAAAAElFTkSuQmCC"),
                      "M5-02 编码：1×1 PNG → data URL（base64 与参考实现一致）");
            expect(check, error.empty(), "M5-02 编码：成功时不写错误");
        }

        // ---- 3) 编码失败路径（不存在 / 目录 / 空文件 / 超限）----
        {
            std::string error;
            expect(check,
                   aiwrite::ai::encode_image_data_url((vlm_root / "不存在.png").string(), 0,
                                                      &error).empty() &&
                       error.find("不存在") != std::string::npos,
                   "M5-02 编码：文件不存在 → 空串 + 可读错误", error);
            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(vlm_root.string(), 0, &error).empty() &&
                       error.find("不存在") != std::string::npos,
                   "M5-02 编码：目录不是图片 → 报错", error);
            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(empty_file.string(), 0, &error).empty() &&
                       error.find("为空") != std::string::npos,
                   "M5-02 编码：空文件 → 报错", error);
            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(png_a.string(), 4, &error).empty() &&
                       error.find("过大") != std::string::npos,
                   "M5-02 编码：超过上限 → 报错并给出上限", error);
        }

        // ---- 4) 请求体（多模态）：content 数组 + 文本块在前 + 图片按序 ----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.api_base      = "https://open.bigmodel.cn/api/paas/v4";
            request.model         = "glm-4v-flash";
            request.system_prompt = "你是编辑";
            request.prompt        = "这张图讲了什么？";
            request.temperature   = 1.0;
            request.images        = {png_a.string(), png_b.string()};

            const nlohmann::json body = aiwrite::ai::build_request_body(request);
            expect_eq(check, aiwrite::ai::build_endpoint(request.api_base),
                      "https://open.bigmodel.cn/api/paas/v4/chat/completions",
                      "M5-02 端点：智谱 api_base 拼接正确");
            expect(check,
                   body["messages"].size() == 2 && body["messages"][0]["role"] == "system" &&
                       body["messages"][0]["content"] == "你是编辑",
                   "M5-02 请求体：system 消息保持字符串形态");
            const nlohmann::json& content = body["messages"][1]["content"];
            expect(check, content.is_array() && content.size() == 3,
                   "M5-02 请求体：有图时 user content = 数组（1 文本 + 2 图）",
                   content.dump().substr(0, 60));
            expect(check,
                   content.is_array() && content[0]["type"] == "text" &&
                       content[0]["text"] == "这张图讲了什么？",
                   "M5-02 请求体：文本块在前且内容一致");
            expect(check,
                   content.is_array() && content[1]["type"] == "image_url" &&
                       content[1]["image_url"]["url"] == std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNgYGAAAAAEAAHI6uv5AAAAAElFTkSuQmCC") &&
                       content[2]["type"] == "image_url" &&
                       content[2]["image_url"]["url"] == std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mP4z8AAAAMBAQD3A0FDAAAAAElFTkSuQmCC"),
                   "M5-02 请求体：两张图按输入顺序编码为 data URL");
            expect(check,
                   body["model"] == "glm-4v-flash" && body["stream"] == false &&
                       body["temperature"] == 1.0 && !body.contains("seed"),
                   "M5-02 请求体：model / stream=false / 采样参数 / 无 seed 时不发 seed");
        }

        // ---- 5) 纯文本回归：content 必须是字符串（PB-05 行为不变）----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.system_prompt     = "系统";
            request.prompt            = "纯文本提示";
            const nlohmann::json body = aiwrite::ai::build_request_body(request);
            expect(check,
                   body["messages"].size() == 2 && body["messages"][0]["content"].is_string() &&
                       body["messages"][1]["content"].is_string() &&
                       body["messages"][1]["content"] == "纯文本提示",
                   "M5-02 回归：无图时 content 仍为字符串（旧行为不变）");
            expect(check, body["stream"] == false && !body["messages"][1].contains("type"),
                   "M5-02 回归：无图请求体不含多模态字段");
        }

        // ---- 6) 不可用的图片被跳过（纯函数重载的降级行为）----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.prompt            = "只有一张坏图";
            request.images            = {(vlm_root / "坏图.png").string()};
            const nlohmann::json body = aiwrite::ai::build_request_body(request);
            expect(check,
                   body["messages"].size() == 1 && body["messages"][0]["content"].is_string(),
                   "M5-02 降级：不可用图片被跳过（退化为纯文本请求体）");
        }

        // ---- 7) 节点接线（离线，无 HTTP）：模型名覆盖 / VLM 的模式与 Key 校验 ----
        {
            // 7a) 提供商配置句柄：model_custom 覆盖生效模型名
            {
                nlohmann::json params;
                params["provider"]     = "deepseek";
                params["mode"]         = "official";
                params["api_base"]     = "https://open.bigmodel.cn/api/paas/v4";
                params["model"]        = "deepseek-chat";
                params["model_custom"] = "glm-4v-flash";
                engine::ExecutionContext context;
                const nlohmann::json     handle =
                    aiwrite::nodes::execute_provider_config(nlohmann::json::object(), params,
                                                           context);
                expect(check,
                       handle["model"] == "glm-4v-flash" &&
                           handle["model_custom"] == "glm-4v-flash",
                       "M5-02b 提供商配置：model_custom 覆盖 provider 句柄的生效模型名");
            }

            // 7b) 引擎侧生效解析（界面与执行共用同一函数）
            {
                Graph             graph;
                const std::string provider = add_node(check, graph, "ProviderConfig", "解析用配置");
                const std::string llm      = add_node(check, graph, "LLMGenerate", "解析用推理");
                set_param(graph, provider, "model", "deepseek-chat");
                set_param(graph, provider, "model_custom", "glm-4v-flash");
                graph.edges.push_back(make_edge("m1", provider, "provider", llm, "provider"));
                if (const engine::Node* llm_node = graph.findNode(llm)) {
                    const engine::EffectiveProvider effective =
                        engine::resolve_effective_provider(graph, *llm_node);
                    expect(check,
                           effective.from_edge && effective.model == "glm-4v-flash" &&
                               effective.model_custom == "glm-4v-flash",
                           "M5-02b 生效解析：来源节点 model_custom 覆盖枚举模型名",
                           effective.model);
                }
            }

            // 7c) 图片理解 + 网页版 → 明确拒绝（不发起 HTTP）
            {
                Graph             graph;
                const std::string img      = add_node(check, graph, "ImageInput", "VLM 图片输入");
                const std::string text     = add_node(check, graph, "TextInput", "VLM 提示词");
                const std::string vlm      = add_node(check, graph, "VLMGenerate", "图片理解");
                const std::string provider = add_node(check, graph, "ProviderConfig", "VLM 配置 web");
                set_param(graph, img, "path", png_a.string());
                set_param(graph, text, "text", "描述这张图");
                set_param(graph, provider, "mode", "web");
                graph.edges.push_back(make_edge("v1", img, "image", vlm, "image"));
                graph.edges.push_back(make_edge("v2", text, "text", vlm, "prompt"));
                graph.edges.push_back(make_edge("v3", provider, "provider", vlm, "provider"));

                Executor    executor;
                std::string error;
                const bool  started = executor.start(graph, &error);
                expect(check, started,
                       "M5-02b 接线：图片理解（web 模式）运行前校验不阻断（仅警告）", error);
                if (started) {
                    executor.runToCompletion(&graph, 64);
                    const engine::Node* node = graph.findNode(vlm);
                    expect(check,
                           node != nullptr && node->state == engine::NodeState::Error &&
                               node->error_message.find("暂不支持网页版") != std::string::npos,
                           "M5-02b 接线：web 模式 → 「图片理解暂不支持网页版」",
                           node != nullptr ? node->error_message : std::string());
                }
            }

            // 7d) 图片理解 + official 且无 Key → 可操作错误（不发起 HTTP）
            {
                Graph             graph;
                const std::string img      = add_node(check, graph, "ImageInput", "VLM 图片输入 2");
                const std::string text     = add_node(check, graph, "TextInput", "VLM 提示词 2");
                const std::string vlm      = add_node(check, graph, "VLMGenerate", "图片理解 2");
                const std::string provider = add_node(check, graph, "ProviderConfig", "VLM 配置 official");
                set_param(graph, img, "path", png_a.string());
                set_param(graph, text, "text", "描述这张图");
                set_param(graph, provider, "mode", "official");
                set_param(graph, provider, "model_custom", "glm-4v-flash");
                // 指向一个不存在的凭据条目：确保「无 Key」这一分支可离线复现（不读真实凭据）
                set_param(graph, provider, "api_key_ref", "aiwrite-selftest/absent");
                graph.edges.push_back(make_edge("x1", img, "image", vlm, "image"));
                graph.edges.push_back(make_edge("x2", text, "text", vlm, "prompt"));
                graph.edges.push_back(make_edge("x3", provider, "provider", vlm, "provider"));

                Executor    executor;
                std::string error;
                const bool  started = executor.start(graph, &error);
                expect(check, started,
                       "M5-02b 接线：图片理解（official 无 Key）运行前校验不阻断", error);
                if (started) {
                    executor.runToCompletion(&graph, 64);
                    const engine::Node* node = graph.findNode(vlm);
                    expect(check,
                           node != nullptr && node->state == engine::NodeState::Error &&
                               node->error_message.find("缺少 API Key") != std::string::npos,
                           "M5-02b 接线：无 Key → 可操作错误（指向「提供商配置」）",
                           node != nullptr ? node->error_message : std::string());
                }
            }
        }

        std::filesystem::remove_all(vlm_root, ec);
    }

    std::printf("=== 执行器自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
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
        else if (arg == "--exec-selftest") {
            options.exec_selftest = true;
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

    // PA-07：工具侧也载入真实配置到进程缓存（只读用途；provider 等后续补丁读取）
    {
        aiwrite::Config probe_config;
        if (aiwrite::load_config(aiwrite::paths::config_file(), probe_config)) {
            aiwrite::set_app_config(probe_config);
        }
    }

    if (options.sha3_text == "" && !options.selftest && !options.graph_selftest &&
        !options.exec_selftest && options.http_url.empty() && options.chat_prompt.empty()) {
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
        const bool exec_ok =
            (executor_selftest() == 0) && (execution_selftest() == 0); // P3-1 + P3-2（离线）
        const bool files_ok = test_workflow_files();                   // M2-05（离线）

        const char* http_text =
            (http_code == 0) ? "PASS" : (http_code == 2 ? "SKIP(网络不可达)" : "FAIL");
        std::printf("=== 结果: SHA3 %s / HTTP %s / 请求构造 %s / 配置往返 %s / 会话与可见性 %s / "
                    "执行器 %s / 工作流文件 %s ===\n",
                    sha3_ok ? "PASS" : "FAIL", http_text, request_ok ? "PASS" : "FAIL",
                    config_ok ? "PASS" : "FAIL", web_ok ? "PASS" : "FAIL", exec_ok ? "PASS" : "FAIL",
                    files_ok ? "PASS" : "FAIL");
        if (!sha3_ok || http_code == 1 || !request_ok || !config_ok || !web_ok || !exec_ok ||
            !files_ok) {
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

    if (options.exec_selftest) {
        if (executor_selftest() != 0) {
            exit_code = 1;
        }
        if (execution_selftest() != 0) {
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
