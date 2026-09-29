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
#include "ai/provider_spec.h"              // M_patchB L1：Provider 配置表断言
#include "ai/dom_web_client.h"             // L3（PB2-13/15）：DOM 适配器纯函数断言（VB2-22）
#include "web/webview_host.h"              // L4（PB2-28）：探测脚本适用性断言（VB2-25）
#include "ai/deepseek_web_client.h"       // L4（PB2-28④）：会话失效识别断言（VB2-27）
#include "utils/image_decode.h"           // M7-04/05：图片格式嗅探 + 解码 + 失败文案断言
#include "utils/asset_store.h"            // P7a-04：统一资源目录（令牌 / 归档 / 解析）断言

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
    std::string image_path;   // --image-decode：诊断某一图片文件（格式嗅探 / 解码 / 尺寸）
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
    std::printf("  api_probe --image-decode <图片路径>  # 图片诊断：内容嗅探 / MIME / 解码（stb|wic）/ 尺寸 / WIC 能力\n");
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

    // ---- P7a-12/13（UI A 档）：默认布局常显 + 老配置显式 false 尊重原值 ----
    {
        const std::filesystem::path defaults_file =
            std::filesystem::temp_directory_path() / "aiwrite_config_defaults.toml";
        std::error_code ec3;
        std::filesystem::remove(defaults_file, ec3);

        // ① 键**不存在** → 用新默认（节点库 / 参数面板**显示**，P7a-12）
        aiwrite::Config fresh;
        (void)aiwrite::load_config(defaults_file, fresh);
        const bool default_visible = fresh.ui.show_node_library && fresh.ui.show_property_panel;
        std::printf("   %-6s P7a-12 默认布局：节点库=%s / 参数面板=%s（新默认应为 true）\n",
                    default_visible ? "PASS" : "FAIL",
                    fresh.ui.show_node_library ? "true" : "false",
                    fresh.ui.show_property_panel ? "true" : "false");

        // ② 显式 false → **尊重原值**（P7a-13：按「键是否存在」判定）
        {
            std::ofstream out(defaults_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 1\n\n[ui]\nshow_node_library = false\n"
                   "show_property_panel = false\n";
        }
        aiwrite::Config explicit_off;
        (void)aiwrite::load_config(defaults_file, explicit_off);
        const bool respected =
            !explicit_off.ui.show_node_library && !explicit_off.ui.show_property_panel;
        std::printf("   %-6s P7a-13 老配置显式 false：被尊重（未被新默认覆盖）=%s\n",
                    respected ? "PASS" : "FAIL", respected ? "是" : "否");

        // ③ 只写其中一个键 → 另一个走新默认（字段级独立）
        {
            std::ofstream out(defaults_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 1\n\n[ui]\nshow_node_library = false\n";
        }
        aiwrite::Config partial;
        (void)aiwrite::load_config(defaults_file, partial);
        const bool field_level = !partial.ui.show_node_library && partial.ui.show_property_panel;
        std::printf("   %-6s P7a-13 字段级独立：显式 false 保持 / 缺失键走新默认=%s\n",
                    field_level ? "PASS" : "FAIL", field_level ? "是" : "否");

        std::filesystem::remove(defaults_file, ec3);
    }

    // ---- M_patchB L1（PB2-06）：多 provider 实例参数 + 旧单节迁移 + 备份 ----
    bool multi_ok = false;
    {
        const std::filesystem::path multi_file =
            std::filesystem::temp_directory_path() / "aiwrite_config_multi.toml";
        std::error_code ec2;
        std::filesystem::remove(multi_file, ec2);
        std::filesystem::remove(multi_file.string() + ".bak", ec2);

        // ① 旧格式（只有 [providers.deepseek]）→ 加载应自动迁移进映射（幂等）
        {
            std::ofstream out(multi_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 1\n\n[providers.deepseek]\nprovider = \"deepseek\"\n"
                   "mode = \"web\"\napi_base = \"https://api.deepseek.com\"\n"
                   "model = \"deepseek-reasoner\"\napi_key_ref = \"brain-ai/deepseek\"\n";
        }
        aiwrite::Config legacy;
        const bool      loaded   = aiwrite::load_config(multi_file, legacy);
        const bool      migrated = loaded && legacy.providers.count("deepseek") == 1 &&
                              legacy.providers["deepseek"].mode == "web" &&
                              legacy.providers["deepseek"].model == "deepseek-reasoner" &&
                              legacy.deepseek.model == "deepseek-reasoner";

        // ② 增加第二个 provider → 保存 → 重新加载：两条都在；`.bak` 已生成
        legacy.providers["zhipu"].provider    = "zhipu";
        legacy.providers["zhipu"].api_base    = "https://open.bigmodel.cn/api/paas/v4";
        legacy.providers["zhipu"].model       = "glm-4-flash";
        legacy.providers["zhipu"].api_key_ref = "brain-ai/zhipu";
        legacy.deepseek.model                 = "deepseek-chat"; // 旧成员 = deepseek 条的权威
        const bool saved     = aiwrite::save_config(multi_file, legacy);
        const bool backed_up = std::filesystem::exists(multi_file.string() + ".bak", ec2);

        aiwrite::Config again;
        const bool      reloaded_ok = aiwrite::load_config(multi_file, again);
        const bool      multi       = reloaded_ok && again.providers.size() == 2 &&
                          again.providers["zhipu"].model == "glm-4-flash" &&
                          again.providers["zhipu"].api_base == "https://open.bigmodel.cn/api/paas/v4" &&
                          again.providers["deepseek"].model == "deepseek-chat" &&
                          again.deepseek.model == "deepseek-chat";
        multi_ok = migrated && saved && backed_up && multi;
        std::printf("   %-6s PB2-06 多 provider 实例参数：旧单节迁移=%s；新增条目往返=%s；"
                    "保存前备份 .bak=%s\n",
                    multi_ok ? "PASS" : "FAIL", migrated ? "OK" : "异常", multi ? "OK" : "异常",
                    backed_up ? "已生成" : "缺失");

        std::filesystem::remove(multi_file, ec2);
        std::filesystem::remove(multi_file.string() + ".bak", ec2);
    }

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

    return same && cache_ok && fit_ok && unwired_ok && multi_ok;
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
    // M_patchB L1 续（PB2-17）：无参重载 = 旧常量（逐字一致，钉住不变量 I2）；站点参数的按条目的
    // 断言在 --exec-selftest 的 VB2-17 块（改表即换站点）
    const aiwrite::web::LoginRequest interactive = aiwrite::web::interactive_login_request();
    const bool request_ok = interactive.probe_after_load && !interactive.offscreen &&
                            interactive.url == "https://chat.deepseek.com/" &&
                            interactive.window_title ==
                                "AIwrite · DeepSeek 网页版登录（登录后关闭本窗口）";

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
    expect_eq(check, std::to_string(registry.size()), "8", "注册表包含 8 个节点类型");

    const char* expected_types[] = {"TextInput",      "ImageInput",    "PromptTemplate",
                                    "TextMerge",      "ProviderConfig", "LLMGenerate",
                                    "VLMGenerate",    "TextOutput"};
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
        {NodeCategory::Output, 1, "输出"}}; // M7：ImagePreview 移除后「输出」只剩 TextOutput
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
        const std::string vlm   = add_node(check, graph, "VLMGenerate", "图片理解（类型不兼容用例）");
        graph.edges.push_back(make_edge("e9", llm, "text", vlm, "image"));
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
    expect_eq(check, std::to_string(engine::NodeExecutorRegistry::instance().size()), "8",
              "已注册 8 个节点执行函数");

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
        const fs::path png_c      = vlm_root / "第三张.png";  // P7a-03：三图请求体夹具
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
        // P7a-03：第三张（1×1 蓝）—— 与前两张**字节级不同**，便于断言「三张图各自独立」
        const std::string png_c_hex =
            "89504E470D0A1A0A0000000D4948445200000001000000010802000000907753"
            "DE0000000C49444154789C636060F80F00010301000889C2EC0000000049454E"
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
        write_hex(png_c, png_c_hex);
        {
            std::ofstream empty(empty_file, std::ios::binary);
        }
        expect(check, fs::exists(png_a) && fs::file_size(png_a) > 0 && fs::exists(png_b),
               "M5-02 夹具：两张 1×1 PNG 已写入临时目录");
        expect(check, fs::exists(png_c) && fs::file_size(png_c) > 0,
               "P7a-03 夹具：第三张 1×1 PNG（蓝）已写入临时目录");

        // ---- 1) MIME 推断（大小写不敏感；未知扩展名回退 image/png）----
        expect_eq(check, aiwrite::ai::image_mime_from_path("a.PNG"), "image/png",
                  "M5-02 MIME：.PNG（大写）→ image/png");
        expect_eq(check, aiwrite::ai::image_mime_from_path("b.JpEg"), "image/jpeg",
                  "M5-02 MIME：.JpEg → image/jpeg");
        expect_eq(check, aiwrite::ai::image_mime_from_path("c.webp"), "image/webp",
                  "M5-02 MIME：.webp → image/webp");
        expect_eq(check, aiwrite::ai::image_mime_from_path("d.tif"), "image/tiff",
                  "M7-05 MIME：.tif（TIFF 属已知扩展名）→ image/tiff");
        expect_eq(check, aiwrite::ai::image_mime_from_path("e.xyz"), "image/png",
                  "M5-02 MIME：未知扩展名回退 image/png");

        // ---- 1b) M7-04/05：内容嗅探优先 + 解码 + 可操作失败文案 ----
        {
            const fs::path webp_fake = vlm_root / "伪装.png"; // 内容 WebP、扩展名 .png（实测场景）
            {
                std::ofstream              out(webp_fake, std::ios::binary);
                const unsigned char        head[] = {'R', 'I',  'F',  'F',  0x1E, 0x00, 0x00, 0x00,
                                                     'W', 'E',  'B',  'P',  'V',  'P',  '8',  ' ',
                                                     0x12, 0x00, 0x00, 0x00};
                out.write(reinterpret_cast<const char*>(head), sizeof(head));
            }
            const aiwrite::utils::ImageMagic magic = aiwrite::utils::sniffImage(webp_fake.string());
            expect(check, magic.format == aiwrite::utils::ImageFormat::WebP,
                   "M7-05 嗅探：RIFF/WEBP 文件头 → WebP（不看扩展名）");
            expect(check, magic.mismatch, "M7-05 嗅探：内容 WebP + 扩展名 .png → 标记不一致");
            expect_eq(check, aiwrite::ai::image_mime_from_path(webp_fake.string()), "image/webp",
                      "M7-05 MIME：WebP 伪装 .png → image/webp（不再谎报 image/png）");
            expect_eq(check, aiwrite::ai::image_mime_from_path(png_a.string()), "image/png",
                      "M7-05 MIME：真 PNG（大写扩展名）→ image/png");

            const unsigned char png_head[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            expect_eq(check, aiwrite::ai::image_mime_from_bytes(png_head, sizeof(png_head)), "image/png",
                      "M7-05 MIME（纯函数）：PNG 魔数 → image/png");
            expect_eq(check, aiwrite::ai::image_mime_from_bytes(nullptr, 0), "",
                      "M7-05 MIME（纯函数）：空数据 → 空串（调用方回退扩展名）");

            const std::string message = aiwrite::utils::decodeErrorMessage(
                webp_fake.string(), magic, "stb：unknown image type；WIC：没有可用解码器");
            expect(check,
                   message.find("WebP") != std::string::npos &&
                       message.find("另存为") != std::string::npos,
                   "M7-04 文案：指明「内容其实是 WebP」+ 可操作建议（另存为 PNG/JPG）", message);
            expect(check, message.find("图片理解") != std::string::npos,
                   "M7-04 文案：说明「本地预览失败不影响发给模型」", message);

            const aiwrite::utils::DecodedImage decoded = aiwrite::utils::decodeImageRgba8(png_a.string());
            expect(check,
                   decoded.error.empty() && decoded.width == 1 && decoded.height == 1 &&
                       decoded.rgba.size() == 4,
                   "M7-04 解码：真 PNG → 1×1 RGBA8（decoder=" + decoded.decoder + "）", decoded.error);
            expect(check, !decoded.decoder.empty(), "M7-04 解码：记录实际解码器（stb / wic）");

            const fs::path text_fake = vlm_root / "文本.png";
            {
                std::ofstream out(text_fake, std::ios::binary);
                out << "this is not an image at all\n";
            }
            const aiwrite::utils::DecodedImage broken = aiwrite::utils::decodeImageRgba8(text_fake.string());
            expect(check, !broken.error.empty() && broken.error.find("无法识别") != std::string::npos,
                   "M7-04 解码：非图片内容（.png 后缀）→ 「无法识别的图片格式」+ 文件头", broken.error);
            expect(check, broken.error.find("解码器原因") != std::string::npos,
                   "M7-04 解码：错误文案附解码器原因（可诊断）", broken.error);

            int         width      = 0;
            int         height     = 0;
            std::string size_error;
            expect(check,
                   aiwrite::utils::readImageSize(png_a.string(), &width, &height, &size_error) &&
                       width == 1 && height == 1,
                   "M7-04 尺寸：真 PNG → 1×1", size_error);
            expect(check,
                   !aiwrite::utils::readImageSize(empty_file.string(), &width, &height, &size_error) &&
                       size_error.find("为空") != std::string::npos,
                   "M7-04 尺寸：空文件 → 可操作错误", size_error);
            expect(check,
                   !aiwrite::utils::decodeImageRgba8((vlm_root / "无此文件.png").string()).error.empty(),
                   "M7-04 解码：文件不存在 → 明确错误");
        }

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

        // ---- 4b) P7a-03：**三张图**请求体（content 数组含 3 个 image_url）----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.model  = "glm-4v-flash";
            request.prompt = "三张图分别是什么颜色？";
            request.images = {png_a.string(), png_b.string(), png_c.string()};

            const nlohmann::json  body    = aiwrite::ai::build_request_body(request);
            const nlohmann::json& content = body["messages"].back()["content"];

            int image_blocks = 0;
            for (const nlohmann::json& item : content) {
                if (item.value("type", std::string()) == "image_url") {
                    ++image_blocks;
                }
            }
            expect(check, content.is_array() && content.size() == 4 && image_blocks == 3,
                   "P7a-03 请求体：3 张图 → content 含 3 个 image_url（+1 文本块）",
                   "size=" + std::to_string(content.size()) +
                       " image_url=" + std::to_string(image_blocks));
            expect_eq(check, content[3]["image_url"]["url"].get<std::string>(),
                      std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYPgPAAEDAQAIicLsAAAAAElFTkSuQmCC"),
                      "P7a-03 请求体：第 3 张按输入顺序编码为 data URL（与参考实现一致）");
        }

        // ---- 4c) P7a-01：image 端口变长（3 张图输入同一条端口）----
        {
            engine::Graph     graph;
            const std::string img1 = add_node(check, graph, "ImageInput", "P7a 图片输入 1");
            const std::string img2 = add_node(check, graph, "ImageInput", "P7a 图片输入 2");
            const std::string img3 = add_node(check, graph, "ImageInput", "P7a 图片输入 3");
            const std::string text = add_node(check, graph, "TextInput", "P7a 提示词");
            const std::string vlm  = add_node(check, graph, "VLMGenerate", "P7a 图片理解");
            set_param(graph, img1, "path", png_a.string());
            set_param(graph, img2, "path", png_b.string());
            set_param(graph, img3, "path", png_c.string());
            set_param(graph, text, "text", "三张图分别是什么颜色？");
            graph.edges.push_back(make_edge("i1", img1, "image", vlm, "image"));
            graph.edges.push_back(make_edge("i2", img2, "image", vlm, "image"));
            graph.edges.push_back(make_edge("i3", img3, "image", vlm, "image"));
            graph.edges.push_back(make_edge("i4", text, "text", vlm, "prompt"));

            const engine::Node* vlm_node = graph.findNode(vlm);
            const engine::Port* image_port =
                vlm_node != nullptr ? vlm_node->findPort("image", engine::PortDirection::Input)
                                    : nullptr;
            expect(check, image_port != nullptr && image_port->is_variadic,
                   "P7a-01 端口：VLMGenerate.image 已声明为变长（is_variadic）");
            expect(check, graph.inputConnectionCount(vlm, "image") == 3,
                   "P7a-01 连线：3 个「图片输入」可同时连入 image（不被替换）",
                   std::to_string(graph.inputConnectionCount(vlm, "image")));

            std::vector<std::string> load_errors;
            expect(check, engine::validateWorkflow(graph, &load_errors),
                   "P7a-01 校验：变长 image 端口不再报「输入端口被重复占用」",
                   load_errors.empty() ? std::string() : load_errors.front());

            std::vector<std::string> run_errors;
            std::vector<std::string> run_warnings;
            expect(check, engine::validateBeforeRun(graph, &run_errors, &run_warnings),
                   "P7a-01 运行前校验：三个图片输入 + 提示词全部就绪",
                   run_errors.empty() ? std::string() : run_errors.front());
        }

        // ---- 4d) P7a-02：多选路径串（解析 / 拼接 / 参数校验）----
        {
            const std::vector<std::string> list =
                aiwrite::paths::split_path_list("A.png\r\n  B.png  \nA.png\n\nC.png");
            expect(check,
                   list.size() == 3 && list[0] == "A.png" && list[1] == "B.png" &&
                       list[2] == "C.png",
                   "P7a-02 多值解析：CRLF / 首尾空白 / 空行 / 重复项 → 保序去重",
                   std::to_string(list.size()));
            expect_eq(check, aiwrite::paths::join_path_list(list), std::string("A.png\nB.png\nC.png"),
                      "P7a-02 多值拼接：每行一个路径");
            expect(check, aiwrite::paths::split_path_list("   \n\t\n").empty(),
                   "P7a-02 多值解析：全空白 → 空列表");
            expect(check, aiwrite::paths::split_path_list(png_a.string()).size() == 1,
                   "P7a-02 单值解析：无换行 → 长度 1（**旧工作流语义不变**，I19）");

            engine::Graph     param_graph;
            const std::string image_id =
                add_node(check, param_graph, "ImageInput", "P7a 多选参数校验");
            engine::Node*  image      = param_graph.findNode(image_id);
            engine::Param* path_param = image != nullptr ? image->findParam("path") : nullptr;
            expect(check, path_param != nullptr, "P7a-02 参数校验：ImageInput.path 存在");
            if (path_param != nullptr) {
                std::string       reason;
                const std::string three = aiwrite::paths::join_path_list(
                    {png_a.string(), png_b.string(), png_c.string()});
                path_param->value = three;
                expect(check, engine::Graph::validateParam(*path_param, &reason),
                       "P7a-02 参数校验：三张图全部存在 → 通过", reason);

                path_param->value = three + "\n" + (vlm_root / "缺失.png").string();
                const bool failed = !engine::Graph::validateParam(*path_param, &reason);
                expect(check,
                       failed && reason.find("缺失.png") != std::string::npos &&
                           reason.find("共 4 项") != std::string::npos &&
                           reason.find("1 项不可用") != std::string::npos,
                       "P7a-02 参数校验：缺 1 张 → 列出缺失路径 + 项数（可操作文案）", reason);
            }
        }

        // ---- 5b) P7a-08：编码缓存（同图重跑不重复编码）----
        {
            aiwrite::ai::reset_image_encode_cache();
            std::string       error;
            const std::string first = aiwrite::ai::encode_image_data_url(png_a.string(), 0, &error);
            const std::string again = aiwrite::ai::encode_image_data_url(png_a.string(), 0, &error);
            expect(check, !first.empty() && first == again,
                   "P7a-08 缓存：同一张图两次编码 → 结果一致且非空");
            expect(check, aiwrite::ai::image_encode_count() == 1,
                   "P7a-08 缓存：同图编码两次 → 实际只编码 1 次",
                   std::to_string(aiwrite::ai::image_encode_count()));

            (void)aiwrite::ai::encode_image_data_url(png_b.string(), 0, &error);
            expect(check, aiwrite::ai::image_encode_count() == 2,
                   "P7a-08 缓存：换一张图 → 编码计数 +1");

            { // 内容/大小变化 → 缓存键变化 → 重编码（不会吃到过期内容）
                std::ofstream appended(png_b, std::ios::binary | std::ios::app);
                appended.write("x", 1);
            }
            (void)aiwrite::ai::encode_image_data_url(png_b.string(), 0, &error);
            expect(check, aiwrite::ai::image_encode_count() == 3,
                   "P7a-08 缓存：文件变化（大小 / 时间不同）→ 重新编码");

            aiwrite::ai::reset_image_encode_cache();
            expect(check, aiwrite::ai::image_encode_count() == 0,
                   "P7a-08 缓存：reset 后计数归零（自检用例彼此隔离）");
        }

        // ---- 5c) P7a-09 / P7a-11：三类可操作诊断 + 内容策略分类 ----
        {
            std::string       error;
            const std::string oversize =
                aiwrite::ai::encode_image_data_url(png_a.string(), 4, &error);
            expect(check,
                   oversize.empty() && error.find("**体积**") != std::string::npos &&
                       error.find("字节") != std::string::npos &&
                       error.find("image_max_bytes") != std::string::npos,
                   "P7a-09 体积类：直接拒绝 + 实际/上限/引导齐全（含字节数与配置项名）", error);

            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(empty_file.string(), 0, &error).empty() &&
                       error.find("**格式**") != std::string::npos,
                   "P7a-11 格式类：空文件 → 标「格式」并给换图引导", error);

            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url((vlm_root / "无此图.png").string(), 0, &error)
                           .empty() &&
                       error.find("**路径**") != std::string::npos,
                   "P7a-11 路径类：文件不存在 → 标「路径」", error);

            const std::string policy = aiwrite::ai::classify_http_error(
                400, R"({"error":{"message":"content_policy_violation"}})");
            const std::string param_error =
                aiwrite::ai::classify_http_error(400, R"({"error":{"message":"invalid model"}})");
            expect(check,
                   policy.find("**内容策略**") != std::string::npos &&
                       param_error.find("请求不合法") != std::string::npos && policy != param_error,
                   "P7a-11 内容策略类：400 + 策略关键词 → 与「参数不合法」明确区分", param_error);
            expect(check,
                   aiwrite::ai::classify_http_error(401, "{}").find("API Key") != std::string::npos &&
                       aiwrite::ai::classify_http_error(429, "{}").find("限流") != std::string::npos &&
                       aiwrite::ai::classify_http_error(503, "{}").find("服务端") != std::string::npos,
                   "PB-05 回归：401 / 429 / 503 分类文案不变");
            expect_eq(check, aiwrite::ai::human_bytes(4096), std::string("4.0 KB"),
                      "P7a-10 体积文本：< 1 MB → KB（小图不再显示成 0.00 MB）");
            expect_eq(check, aiwrite::ai::human_bytes(2u * 1024u * 1024u), std::string("2.0 MB"),
                      "P7a-10 体积文本：≥ 1 MB → MB");
        }

        // ---- 6) P7a-04/05/06/07：统一资源目录（内容寻址 + 令牌 + 兼容 + 缺失文案）----
        {
            std::string       error;
            const std::string token_a = aiwrite::asset::import_file(png_a.string(), &error);
            expect(check,
                   aiwrite::asset::is_token(token_a) &&
                       token_a.rfind(aiwrite::asset::kTokenPrefix, 0) == 0 &&
                       token_a.size() == std::string(aiwrite::asset::kTokenPrefix).size() + 64,
                   "P7a-04 归档：导入图片 → 资源令牌（aiwrite-asset: + 64 位摘要）", token_a);
            expect(check,
                   !aiwrite::asset::is_token("aiwrite-asset:xyz") &&
                       !aiwrite::asset::is_token(png_a.string()) &&
                       !aiwrite::asset::is_token(std::string()),
                   "P7a-04 判定：非法摘要 / 普通路径 / 空串都不是令牌");

            const auto count_assets = []() {
                std::size_t count = 0;
                std::error_code ec;
                const std::filesystem::path root = aiwrite::asset::images_root();
                if (std::filesystem::exists(root, ec)) {
                    for (const std::filesystem::directory_entry& entry :
                         std::filesystem::directory_iterator(root, ec)) {
                        if (entry.is_regular_file(ec)) {
                            ++count;
                        }
                    }
                }
                return count;
            };
            const std::size_t before = count_assets();
            const std::string token_a2 = aiwrite::asset::import_file(png_a.string(), &error);
            expect(check, token_a2 == token_a && count_assets() == before,
                   "P7a-04 去重：同内容再导入 → 同一令牌且资源目录文件数不变",
                   std::to_string(before) + " → " + std::to_string(count_assets()));

            const std::string token_b = aiwrite::asset::import_file(png_b.string(), &error);
            expect(check, !token_b.empty() && token_b != token_a,
                   "P7a-04 归档：不同内容 → 不同令牌");

            bool              missing = false;
            const std::string local_a = aiwrite::asset::to_local_path(token_a, &missing, &error);
            expect(check,
                   !missing && !local_a.empty() && std::filesystem::exists(local_a) &&
                       local_a.find("assets") != std::string::npos,
                   "P7a-04 解析：令牌 → 资源目录内的真实文件", local_a);
            expect(check, aiwrite::asset::to_local_path(png_c.string()) == png_c.string(),
                   "P7a-06 兼容：旧绝对路径**原样返回**（不变量 I19）");
            expect(check,
                   aiwrite::asset::needs_migration(png_c.string()) &&
                       !aiwrite::asset::needs_migration(token_a),
                   "P7a-06 判定：外部路径需迁移、资源令牌不需要");
            expect(check,
                   aiwrite::asset::migration_hint(png_c.string()).find("迁移到资源目录") !=
                       std::string::npos,
                   "P7a-06 文案：迁移提示指向「迁移到资源目录」");

            // 内容优先的规范扩展名（内容 WebP 却叫 .png → 归档成 .webp）
            std::string disguised_token;
            {
                const fs::path disguised = vlm_root / "伪装.png";
                {
                    std::ofstream              out(disguised, std::ios::binary);
                    const unsigned char        head[] = {'R',  'I',  'F',  'F',  0x1E, 0x00, 0x00, 0x00,
                                                         'W',  'E',  'B',  'P',  'V',  'P',  '8',  ' ',
                                                         0x12, 0x00, 0x00, 0x00};
                    out.write(reinterpret_cast<const char*>(head), sizeof(head));
                }
                disguised_token = aiwrite::asset::import_file(disguised.string(), &error);
                expect(check,
                       !disguised_token.empty() &&
                           aiwrite::asset::to_local_path(disguised_token).find(".webp") !=
                               std::string::npos,
                       "P7a-04 扩展名：内容优先（WebP 存成 .png → 归档为 .webp）",
                       aiwrite::asset::to_local_path(disguised_token));
            }

            // 归档失败路径
            std::string import_error;
            expect(check,
                   aiwrite::asset::import_file((vlm_root / "不存在.png").string(), &import_error)
                           .empty() &&
                       !import_error.empty(),
                   "P7a-04 归档失败：文件不存在 → 空令牌 + 可读错误", import_error);

            // 清理：删掉本块导入的资源（自检**不在用户资源目录留垃圾**）
            for (const std::string& created : {token_a, token_b, disguised_token}) {
                const std::string created_path = aiwrite::asset::to_local_path(created);
                if (!created_path.empty()) {
                    std::error_code cleanup_code;
                    std::filesystem::remove(created_path, cleanup_code);
                }
            }
        }

        // ---- 7) P7a-05/06/07：令牌进工作流的往返 / 旧路径兼容 / 缺失文案 ----
        {
            std::string       error;
            const std::string token      = aiwrite::asset::import_file(png_b.string(), &error);
            const fs::path    asset_file = aiwrite::asset::to_local_path(token);
            expect(check, !asset_file.empty() && std::filesystem::exists(asset_file),
                   "P7a-05 准备：资源已归档", asset_file.string());

            // P7a-05：令牌写进工作流 → 保存 → 读回 → 运行（换目录 / 换机语义）
            {
                engine::Graph     graph;
                const std::string img = add_node(check, graph, "ImageInput", "P7a 令牌工作流");
                set_param(graph, img, "path", token);
                const fs::path workflow_file = vlm_root / "p7a_asset_roundtrip.json";

                std::string save_error;
                expect(check, engine::save_workflow(graph, workflow_file, &save_error),
                       "P7a-05 保存：含资源令牌的工作流可落盘", save_error);

                engine::Graph loaded;
                std::string   load_error;
                const bool    loaded_ok = engine::load_workflow(workflow_file, loaded, &load_error);
                expect(check, loaded_ok, "P7a-05 加载：工作流可读回", load_error);
                const engine::Node* loaded_node = loaded.findNode(img);
                expect(check,
                       loaded_node != nullptr && loaded_node->findParam("path") != nullptr &&
                           loaded_node->findParam("path")->text() == token,
                       "P7a-05 往返：文件里存的是**令牌**（不是绝对路径）→ 换机仍有效");

                Executor                 executor;
                std::vector<std::string> console_lines;
                executor.setConsoleHandler(
                    [&console_lines](const std::string& text) { console_lines.push_back(text); });
                std::string start_error;
                const bool  started = executor.start(loaded, &start_error);
                expect(check, started, "P7a-05 运行：start 通过", start_error);
                if (started) {
                    executor.runToCompletion(&loaded, 64);
                    const engine::Node* after = loaded.findNode(img);
                    expect(check, after != nullptr && after->state == engine::NodeState::Done,
                           "P7a-05 运行：ImageInput 完成（令牌已解析为资源路径）");
                    bool resolved_seen = false;
                    for (const std::string& line : console_lines) {
                        if (line.find(aiwrite::asset::images_root().string()) != std::string::npos) {
                            resolved_seen = true;
                        }
                    }
                    expect(check, resolved_seen, "P7a-05 运行：Console 打印解析后的资源路径");
                }
            }

            // P7a-06：旧绝对路径 → 照旧可运行 + Console 给迁移提示（**不静默改写**文件）
            {
                engine::Graph     graph;
                const std::string img = add_node(check, graph, "ImageInput", "P7a 旧绝对路径");
                set_param(graph, img, "path", png_c.string());

                Executor                 executor;
                std::vector<std::string> console_lines;
                executor.setConsoleHandler(
                    [&console_lines](const std::string& text) { console_lines.push_back(text); });
                std::string start_error;
                const bool  started = executor.start(graph, &start_error);
                expect(check, started, "P7a-06 兼容：旧绝对路径不阻断运行", start_error);
                if (started) {
                    executor.runToCompletion(&graph, 64);
                    const engine::Node* after = graph.findNode(img);
                    expect(check, after != nullptr && after->state == engine::NodeState::Done,
                           "P7a-06 兼容：旧绝对路径照旧可运行（不变量 I19）");
                    bool hint_seen = false;
                    for (const std::string& line : console_lines) {
                        if (line.find("迁移到资源目录") != std::string::npos) {
                            hint_seen = true;
                        }
                    }
                    expect(check, hint_seen, "P7a-06 提示：Console 出现「迁移到资源目录」引导");
                }
            }

            // P7a-07：资源被删除 → 文案可操作 + 校验失败；重新归档可自愈
            {
                std::error_code remove_code;
                std::filesystem::remove(asset_file, remove_code);

                bool              gone_missing = false;
                std::string       gone_error;
                const std::string gone =
                    aiwrite::asset::to_local_path(token, &gone_missing, &gone_error);
                expect(check,
                       gone_missing && gone.empty() &&
                           gone_error.find("图片资源缺失") != std::string::npos &&
                           gone_error.find(aiwrite::asset::images_root().string()) !=
                               std::string::npos &&
                           gone_error.find("重新选择") != std::string::npos,
                       "P7a-07 缺失文案：含「资源目录完整路径 + 预期文件 + 下一步」", gone_error);

                engine::Graph     graph;
                const std::string img        = add_node(check, graph, "ImageInput", "P7a 已删资源校验");
                engine::Node*     image      = graph.findNode(img);
                engine::Param*    path_param = image != nullptr ? image->findParam("path") : nullptr;
                if (path_param != nullptr) {
                    std::string reason;
                    path_param->value = token;
                    expect(check,
                           !engine::Graph::validateParam(*path_param, &reason) &&
                               reason.find("图片资源缺失") != std::string::npos,
                           "P7a-07 校验：令牌指向的**已删除**资源 → 校验失败且文案可操作", reason);

                    // 反向：重新归档同一张图 → 令牌不变（内容寻址）→ 校验恢复通过
                    std::string       reimport_error;
                    const std::string again =
                        aiwrite::asset::import_file(png_b.string(), &reimport_error);
                    expect(check, again == token,
                           "P7a-04 幂等：重新归档同一内容 → 令牌不变（内容寻址可自愈）");
                    expect(check, engine::Graph::validateParam(*path_param, &reason),
                           "P7a-07 自愈：资源重新归档后校验恢复通过", reason);
                }
            }

            // 清理：删掉本次自检导入的资源（不在用户资源目录留垃圾）
            const std::string cleanup_path = aiwrite::asset::to_local_path(token);
            if (!cleanup_path.empty()) {
                std::error_code cleanup_code;
                std::filesystem::remove(cleanup_path, cleanup_code);
            }
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

        // ---- 8) 快照图片通道（M5-03：无 GL 也能验，UI 据此渲染缩略图）----
        //      M7：ImagePreview 汇点已移除 —— 图片结果只来自 image 端口（如「图片输入」）
        {
            Graph             graph;
            const std::string img    = add_node(check, graph, "ImageInput", "快照图片输入");
            const std::string text   = add_node(check, graph, "TextInput", "快照文本");
            const std::string output = add_node(check, graph, "TextOutput", "快照文本输出");
            set_param(graph, img, "path", png_a.string());
            set_param(graph, text, "text", "纯文本");
            graph.edges.push_back(make_edge("s2", text, "text", output, "text"));

            Executor    executor;
            std::string error;
            const bool  started = executor.start(graph, &error);
            expect(check, started, "M5-03 快照：图片链路 start 通过", error);
            if (started) {
                executor.runToCompletion(&graph, 64);
                const engine::RunSnapshot snapshot = engine::makeSnapshot(graph, executor);
                const engine::RunNodeView* img_view = snapshot.find(img);
                expect(check,
                       img_view != nullptr && img_view->images.size() == 1 &&
                           img_view->images[0] == png_a.string(),
                       "M5-03 快照：ImageInput 的 image 端口进入 RunNodeView.images");
                const engine::RunNodeView* text_view = snapshot.find(output);
                expect(check, text_view != nullptr && text_view->images.empty(),
                       "M5-03 快照：纯文本节点的 images 为空（不误判为图片）");
            }
        }

        // ---- 9) 示例工作流 E-02（图片转小说）：可加载、可校验 ----
#ifdef AIWRITE_SOURCE_DIR
        {
            std::error_code             example_ec;
            const std::filesystem::path example =
                std::filesystem::path(AIWRITE_SOURCE_DIR) / "workflows" / "examples" /
                "E-02_图片转小说.json";
            expect(check, std::filesystem::exists(example, example_ec),
                   "M5-04 示例：E-02 图片转小说 文件存在", example.string());
            if (std::filesystem::exists(example, example_ec)) {
                Graph       graph;
                std::string error;
                const bool  loaded = engine::load_workflow(example, graph, &error);
                expect(check, loaded, "M5-04 示例：E-02 可加载（JSON → Graph）", error);
                if (loaded) {
                    expect(check, graph.nodes.size() == 5 && graph.edges.size() == 4,
                           "M5-04 示例：E-02 为 5 节点 4 连线",
                           std::to_string(graph.nodes.size()) + " 节点 / " +
                               std::to_string(graph.edges.size()) + " 连线");
                    engine::ValidationMessages errors;
                    const bool               valid = engine::validateWorkflow(graph, &errors);
                    expect(check, valid, "M5-04 示例：E-02 加载校验通过（端口/参数/无环）",
                           errors.empty() ? std::string() : errors.front());
                }
            }
        }
#endif

        std::filesystem::remove_all(vlm_root, ec);
    }

    // ---- M_patchB L1 / PB2-04：请求参数化（端点 / 认证 / env / 超时，纯函数）----
    {
        using aiwrite::ai::ProviderOptions;
        std::printf("   -- 请求参数化（M_patchB L1 / PB2-04）--\n");
        // 端点拼接
        expect_eq(check, aiwrite::ai::build_endpoint("https://api.deepseek.com"),
                  "https://api.deepseek.com/chat/completions", "PB2-04 端点：默认地址");
        expect_eq(check, aiwrite::ai::build_endpoint("https://a.example.com/v1/"),
                  "https://a.example.com/v1/chat/completions", "PB2-04 端点：尾斜杠 + 路径前缀");
        expect_eq(check, aiwrite::ai::build_endpoint("https://b.example.com", "/v1/messages"),
                  "https://b.example.com/v1/messages", "PB2-04 端点：自定义路径（Anthropic）");
        expect_eq(check, aiwrite::ai::build_endpoint("", ""),
                  "https://api.deepseek.com/chat/completions", "PB2-04 端点：空串回退默认");
        expect_eq(check,
                  aiwrite::ai::resolve_chat_path("/v1beta/models/{model}:generateContent",
                                                 "gemini-2.0-flash"),
                  "/v1beta/models/gemini-2.0-flash:generateContent",
                  "PB2-04 端点：{model} 占位替换（Gemini 风格）");
        // 认证头
        ProviderOptions bearer;
        const auto auth_bearer = aiwrite::ai::build_auth_headers(bearer, "sk-test");
        expect(check, auth_bearer.size() == 1 && auth_bearer[0].first == "Authorization" &&
                          auth_bearer[0].second == "Bearer sk-test",
               "PB2-04 认证：bearer → Authorization: Bearer …");
        ProviderOptions api_key;
        api_key.auth_style  = "api-key";
        api_key.auth_header = "api-key";
        const auto auth_azure = aiwrite::ai::build_auth_headers(api_key, "k1");
        expect(check, auth_azure.size() == 1 && auth_azure[0].first == "api-key" &&
                          auth_azure[0].second == "k1",
               "PB2-04 认证：api-key → api-key: <key>（Azure 风格）");
        ProviderOptions anthropic;
        anthropic.auth_style  = "x-api-key";
        anthropic.auth_header = "x-api-key";
        const auto auth_claude = aiwrite::ai::build_auth_headers(anthropic, "k2");
        expect(check, auth_claude.size() == 1 && auth_claude[0].first == "x-api-key",
               "PB2-04 认证：x-api-key（Anthropic 风格）");
        ProviderOptions none;
        none.auth_style = "none";
        expect(check, aiwrite::ai::build_auth_headers(none, "k3").empty(),
               "PB2-04 认证：none → 不带头（本地 Ollama）");
        ProviderOptions query;
        query.auth_style = "query";
        expect(check, aiwrite::ai::build_auth_headers(query, "k4").empty(),
               "PB2-04 认证：query → 不走头，走 URL 拼接");
        // env 名按序（表内 env_names）
        expect_eq(check, aiwrite::ai::resolve_api_key("param-key", {"A_KEY", "B_KEY"}), "param-key",
                  "PB2-04 env：节点参数优先");
        expect_eq(check, aiwrite::ai::resolve_api_key(std::string(), {"AIWRITE_ABSENT_KEY"}), "",
                  "PB2-04 env：未命中的 env 名 → 空（无兜底）");
        expect_eq(check, aiwrite::ai::resolve_api_key(std::string(), {}), "",
                  "PB2-04 env：空列表 = 仅 DEEPSEEK_API_KEY（未设置 → 空）");
        // 协议默认值（来自配置表默认补全规则）
        expect_eq(check, aiwrite::ai::default_chat_path("openai"), "/chat/completions",
                  "PB2-04 默认值：openai → /chat/completions");
        expect_eq(check, aiwrite::ai::default_chat_path("anthropic"), "/v1/messages",
                  "PB2-04 默认值：anthropic → /v1/messages");
        expect_eq(check, aiwrite::ai::default_auth_style("anthropic"), "x-api-key",
                  "PB2-04 默认值：anthropic → x-api-key");
        expect_eq(check, aiwrite::ai::default_auth_header("api-key"), "api-key",
                  "PB2-04 默认值：auth_style=api-key → 头名 api-key");
    }

    // ---- M_patchB L1 / PB2-05：网页版站点参数化（探测脚本渲染；纯离线，不需要 WebView2）----
    {
        std::printf("   -- 网页版站点参数化（M_patchB L1 / PB2-05）--\n");
        aiwrite::web::LoginRequest default_request;
        const std::string default_script = aiwrite::web::probe_kickoff_script(default_request);
        expect(check, default_script.find("'/api/v0/chat/create_pow_challenge'") != std::string::npos &&
                         default_script.find("'/api/v0/chat/completion'") != std::string::npos &&
                         default_script.find("'/api/v0/users/current'") != std::string::npos,
               "PB2-05 探测脚本：无站点参数 = 内置 DeepSeek 默认（行为不变）");

        aiwrite::web::LoginRequest custom;
        custom.challenge_path  = "/api/v9/pow";
        custom.completion_path = "/api/v9/chat";
        custom.probe_paths     = {"/api/v9/me"};
        const std::string custom_script = aiwrite::web::probe_kickoff_script(custom);
        expect(check, custom_script.find("'/api/v9/pow'") != std::string::npos &&
                         custom_script.find("'/api/v9/chat'") != std::string::npos &&
                         custom_script.find("['/api/v9/me']") != std::string::npos,
               "PB2-05 探测脚本：站点路径来自配置表（改表即改探测目标）");
        expect(check, custom_script.find("'/api/v0/chat/completion'") == std::string::npos,
               "PB2-05 探测脚本：自定义站点后不再残留内置路径");
    }

    {
        std::printf("   -- 网页版站点参数按条目（M_patchB L1 续 / VB2-17）--\n");

        // ---- VB2-17：登录请求按**生效条目**构造（改表即换站点，零改码）----
        const aiwrite::web::LoginRequest legacy = aiwrite::web::interactive_login_request();
        expect(check, legacy.probe_after_load && !legacy.offscreen &&
                          legacy.url == "https://chat.deepseek.com/" &&
                          legacy.window_title == "AIwrite · DeepSeek 网页版登录（登录后关闭本窗口）",
               "VB2-17 兼容：无参 interactive_login_request 与旧常量逐字一致（不变量 I2）");

        const aiwrite::ai::ProviderSpecs table    = aiwrite::ai::load_provider_specs();
        const aiwrite::ai::ProviderSpec* web_spec = table.find("deepseek-web");
        bool                             table_ok = false;
        if (web_spec != nullptr) {
            const aiwrite::web::LoginRequest from_table =
                aiwrite::web::interactive_login_request(web_spec->web, web_spec->id);
            table_ok = from_table.url == web_spec->web.login_url &&
                       from_table.window_title == web_spec->web.window_title &&
                       from_table.provider_id == "deepseek-web" && from_table.probe_after_load &&
                       !from_table.offscreen &&
                       from_table.probe_paths == web_spec->web.probe_paths &&
                       from_table.challenge_path == web_spec->web.endpoints.challenge_path &&
                       aiwrite::web::login_request_site(from_table) == "https://chat.deepseek.com";
        }
        expect(check, table_ok, "VB2-17 表驱动：登录页 / 窗口标题 / 探测路径 / 站点键取自条目");

        const aiwrite::ai::ProviderWebSpec empty_spec;
        const aiwrite::web::LoginRequest   fallback =
            aiwrite::web::interactive_login_request(empty_spec, std::string());
        expect(check, fallback.url == legacy.url && fallback.window_title == legacy.window_title &&
                          fallback.probe_after_load,
               "VB2-17 回落：条目缺字段时回落到内置默认站点（DeepSeek）");

        const aiwrite::web::LoginRequest probe_req = aiwrite::web::probe_login_request(
            web_spec != nullptr ? web_spec->web : empty_spec,
            web_spec != nullptr ? web_spec->id : std::string());
        expect(check, probe_req.probe_after_load && !probe_req.url.empty(),
               "VB2-17 探测窗口：同样按条目构造（probe_after_load=true）");

        // ---- 站点键（origin）----
        const bool origin_ok =
            aiwrite::web::site_key_of("https://chat.deepseek.com/") == "https://chat.deepseek.com" &&
            aiwrite::web::site_key_of("HTTPS://Chat.DeepSeek.com/a/b?x=1") ==
                "https://chat.deepseek.com" &&
            aiwrite::web::site_key_of("about:blank") == "about:blank" &&
            aiwrite::web::site_key_of("").empty();
        expect(check, origin_ok, "VB2-16 站点键 = origin（忽略路径/查询/大小写；非 URL 原样）");

        // ---- VB2-18 / PB2-20（决策 D-21）：「模式」候选恒两项 + ProviderConfig 自参数解析 ----
        {
            aiwrite::engine::registerAllNodes();
            aiwrite::engine::Graph     mode_graph;
            std::string                mode_error;
            const std::string          mode_id =
                mode_graph.addNode("ProviderConfig", 0.0f, 0.0f, &mode_error);
            aiwrite::engine::Node* mode_node = mode_graph.findNode(mode_id);
            bool                   options_ok       = false;
            bool                   self_web_ok      = false;
            bool                   not_rewritten_ok = false;
            bool                   unknown_ok       = false;
            if (mode_node != nullptr) {
                const auto select_provider = [mode_node](const std::string& provider) {
                    if (aiwrite::engine::Param* p = mode_node->findParam("provider")) {
                        p->value = provider;
                    }
                };
                const auto set_mode = [mode_node](const std::string& value) {
                    if (aiwrite::engine::Param* p = mode_node->findParam("mode")) {
                        p->value = value;
                    }
                };

                // ① 候选**恒为 {official, web}**（official 条目 / web 条目 / 表外 id 都一样）
                const std::vector<std::string> expect_modes{"official", "web"};
                select_provider("deepseek-web");
                const std::vector<std::string> web_modes =
                    aiwrite::engine::provider_mode_options(*mode_node);
                select_provider("zhipu");
                const std::vector<std::string> api_modes =
                    aiwrite::engine::provider_mode_options(*mode_node);
                select_provider("no-such-provider");
                const std::vector<std::string> unknown_modes =
                    aiwrite::engine::provider_mode_options(*mode_node);
                options_ok = web_modes == expect_modes && api_modes == expect_modes &&
                             unknown_modes == expect_modes;

                // ② web 条目：自参数解析（面板/校验统一入口）→ 命中**该条目**（kind / 显示名 / 站点）
                select_provider("deepseek-web");
                const aiwrite::engine::EffectiveProvider web_self =
                    aiwrite::engine::resolve_display_provider(mode_graph, *mode_node);
                const aiwrite::ai::ProviderWebSpec web_site =
                    aiwrite::ai::web_spec_for(web_self.spec, web_self.specs.get());
                self_web_ok = web_self.resolved && !web_self.from_edge && web_self.spec != nullptr &&
                              web_self.kind == "web" && !web_self.display.empty() &&
                              web_self.spec->id == "deepseek-web" && !web_site.login_url.empty() &&
                              web_site.login_url == web_self.spec->web.login_url &&
                              aiwrite::ai::web_provider_id_for(web_self.spec, web_self.specs.get()) ==
                                  std::string("deepseek-web");

                // ③ kind 与 mode 不一致时**不改写** mode：
                //    official 条目 + web → kind 仍 official、mode 保持 web（将用内置默认站点）
                //    web 条目 + official → mode **保持 official**（撤回「静默改写为 web」的回归位）
                select_provider("deepseek");
                set_mode("web");
                const aiwrite::engine::EffectiveProvider official_self =
                    aiwrite::engine::resolve_display_provider(mode_graph, *mode_node);
                select_provider("deepseek-web");
                set_mode("official");
                const aiwrite::engine::EffectiveProvider web_official =
                    aiwrite::engine::resolve_display_provider(mode_graph, *mode_node);
                not_rewritten_ok =
                    official_self.kind == "official" && official_self.mode == "web" &&
                    web_official.kind == "web" && web_official.mode == "official" &&
                    !aiwrite::engine::mode_kind_hint(official_self).empty() &&
                    !aiwrite::engine::mode_kind_hint(web_official).empty();

                // ④ 表外 id → 条目为空（**不**回落显示为 official / 不猜条目）
                select_provider("no-such-provider");
                const aiwrite::engine::EffectiveProvider unknown_self =
                    aiwrite::engine::resolve_display_provider(mode_graph, *mode_node);
                unknown_ok = unknown_self.resolved && unknown_self.spec == nullptr &&
                             unknown_self.kind.empty();
            }
            expect(check, options_ok,
                   "VB2-18① 「模式」候选恒 {official, web}（official / web 条目、表外 id 都一样；D-21）");
            expect(check, self_web_ok,
                   "VB2-18② ProviderConfig 自参数解析按自身条目（kind=web / 显示名 / 站点与登录页取自条目）");
            expect(check, not_rewritten_ok,
                   "VB2-18③ kind 与 mode 不一致时**不改写** mode（official+web 保持 web；web+official 保持 official）");
            expect(check, unknown_ok,
                   "VB2-18④ 表外 id → 条目为空（不回落显示为 official）");
        }

        // ---- VB2-19 / PB2-22（决策 D-22② / D-26 / 不变量 I14）：**严格**站点解析（不回落）----
        {
            const aiwrite::ai::ProviderSpecs site_table = aiwrite::ai::load_provider_specs();
            const aiwrite::ai::ProviderSpec* official      = site_table.find("deepseek");
            const aiwrite::ai::ProviderSpec* web_entry_spec = site_table.find("deepseek-web");
            const aiwrite::ai::ProviderSpec* no_such        = site_table.find("no-such-provider");

            const aiwrite::ai::ProviderWebSpec official_site =
                aiwrite::ai::strict_web_spec_for(official);
            expect(check, official_site.login_url.empty() && official_site.adapter.empty(),
                   "VB2-19① 非网页版条目 → 严格解析为空（**不**回落表内第一个 web 条目）");

            const aiwrite::ai::ProviderWebSpec strict_web =
                aiwrite::ai::strict_web_spec_for(web_entry_spec);
            expect(check,
                   web_entry_spec != nullptr && !strict_web.login_url.empty() &&
                       strict_web.login_url == web_entry_spec->web.login_url &&
                       strict_web.window_title == web_entry_spec->web.window_title,
                   "VB2-19② 网页版条目 → 严格解析取**它自己**的 web 段（登录页 / 窗口标题一致）");

            expect(check,
                   aiwrite::ai::strict_web_spec_for(nullptr).login_url.empty() &&
                       aiwrite::ai::strict_web_spec_for(no_such).login_url.empty(),
                   "VB2-19③ nullptr / 表外 id → 严格解析为空（面板不再显示误导性站点）");

            expect(check,
                   aiwrite::ai::strict_web_provider_id_for(official).empty() &&
                       aiwrite::ai::strict_web_provider_id_for(web_entry_spec) == "deepseek-web",
                   "VB2-19④ 站点条目 id：非网页版条目为空 / 网页版条目 = 该条目 id");

            expect(check,
                   aiwrite::ai::web_site_error(web_entry_spec).empty() &&
                       !aiwrite::ai::web_site_error(official).empty() &&
                       !aiwrite::ai::web_site_error(nullptr).empty() &&
                       aiwrite::ai::web_adapter_implemented("builtin:deepseek") &&
                       aiwrite::ai::web_adapter_implemented("dom") &&
                       !aiwrite::ai::web_adapter_implemented("builtin:unknown-site"),
                   "VB2-19⑤ 站点错误文案（含三条引导）+ 适配器门控（builtin:deepseek/dom 已实现；未知适配器仍拒绝）");
        }

        // ---- VB2-21 / PB2-26（登录型站点条目）：可加载 + 严格解析可用 + 生成未就绪被如实标记 ----
        {
            const auto has_warn = [](const std::vector<std::string>& items, const std::string& needle) {
                for (const std::string& item : items) {
                    if (item.find(needle) != std::string::npos) {
                        return true;
                    }
                }
                return false;
            };

            const aiwrite::ai::ProviderSpecs login_table = aiwrite::ai::load_provider_specs();
            const aiwrite::ai::ProviderSpec* kimi    = login_table.find("kimi-web");
            const aiwrite::ai::ProviderSpec* ds_web2 = login_table.find("deepseek-web");

            expect(check, kimi != nullptr && kimi->kind == "web" && !kimi->web.login_url.empty(),
                   "VB2-21① 登录型站点条目可加载（kimi-web；不再因缺生成字段被跳过）");

            expect(check,
                   kimi != nullptr && aiwrite::ai::web_site_error(kimi).empty() &&
                       aiwrite::ai::strict_web_spec_for(kimi).login_url == kimi->web.login_url,
                   "VB2-21② 登录型条目站点可用（严格解析给出该条目的登录页；无错误）");

            expect(check,
                   kimi != nullptr && aiwrite::ai::web_login_only(kimi) &&
                       aiwrite::ai::web_adapter_implemented(kimi->web.adapter),
                   "VB2-21③ 生成未就绪被如实标记（dom 适配器**已实现**，但该条缺生成字段 → 登录型条目拦截）");

            expect(check,
                   ds_web2 != nullptr && !aiwrite::ai::web_login_only(ds_web2) &&
                       aiwrite::ai::web_adapter_implemented(ds_web2->web.adapter),
                   "VB2-21④ 已就绪条目不受影响（deepseek-web 仍是可生成条目）");

            expect(check, has_warn(login_table.report.warnings, "登录型站点条目"),
                   "VB2-21⑤ 加载报告把「缺生成字段」降级为**警告**（含「登录型站点条目」字样）");
        }

        // ---- VB2-22 / PB2-13（L3 DOM 适配器）：纯函数（轮询钳制 / 注入转义 / 脚本常量 / 就绪度 / 前置校验）----
        {
            aiwrite::ai::ProviderWebSpec site;
            site.adapter         = "dom";
            site.login_url       = "https://site.example.com/";
            site.input_selector  = "div[contenteditable='true']";
            site.send_kind       = "key";
            site.send_value      = "Enter";
            site.answer_selector = ".markdown-body";
            site.done_kind       = "selector_gone";
            site.done_selector   = "button[aria-label*='停止']";

            int poll_ms   = 0;
            int max_polls = 0;
            aiwrite::ai::clamp_poll_params(site, &poll_ms, &max_polls); // 结构体默认 500 / 120
            const bool clamp_default = (poll_ms == 500 && max_polls == 120);
            aiwrite::ai::ProviderWebSpec tiny;
            tiny.answer_poll_ms   = 100;
            tiny.answer_max_polls = 5;
            aiwrite::ai::clamp_poll_params(tiny, &poll_ms, &max_polls); // 极小 → 下限
            const bool clamp_low = (poll_ms == 200 && max_polls == 10);
            aiwrite::ai::ProviderWebSpec huge;
            huge.answer_poll_ms   = 99999;
            huge.answer_max_polls = 99999;
            aiwrite::ai::clamp_poll_params(huge, &poll_ms, &max_polls); // 超大 → 上限
            const bool clamp_high = (poll_ms == 2000 && max_polls == 600);
            expect(check, clamp_default && clamp_low && clamp_high,
                   "VB2-22① 轮询参数钳制（默认 500ms/120 次不变；极小 → 200ms/10 次；超大 → 2000ms/600 次）");

            const std::string tricky_prompt = "第一行 \"引号\" \\ 反斜杠\n第二行\t制表";
            aiwrite::ai::DomChatRequest request;
            request.prompt = tricky_prompt;
            request.site   = site;
            bool cfg_ok    = false;
            try {
                const nlohmann::json cfg = nlohmann::json::parse(aiwrite::ai::dom_cfg_json(request));
                cfg_ok = cfg.value("prompt", std::string()) == tricky_prompt &&
                         cfg.value("input_selector", std::string()) == site.input_selector &&
                         cfg.value("send_kind", std::string()) == "key" &&
                         cfg.value("send_value", std::string()) == "Enter" &&
                         cfg.value("answer_selector", std::string()) == site.answer_selector &&
                         cfg.value("done_kind", std::string()) == "selector_gone" &&
                         cfg.value("done_selector", std::string()) == site.done_selector;
            }
            catch (const std::exception&) {
                cfg_ok = false;
            }
            expect(check, cfg_ok,
                   "VB2-22② 注入配置可解析且**转义安全**（提示词含引号 / 反斜杠 / 换行 / 制表）");

            const std::string kick = aiwrite::ai::dom_kickoff_script();
            const std::string poll = aiwrite::ai::dom_poll_script();
            const std::string prb  = aiwrite::ai::dom_probe_script();
            expect(check,
                   kick.find("window.__aiwriteDom") != std::string::npos &&
                       poll.find("window.__aiwriteDom") != std::string::npos &&
                       prb.find("window.__aiwriteDomProbe") != std::string::npos &&
                       kick.find("input_selector") != std::string::npos &&
                       poll.find("answer_selector") != std::string::npos,
                   "VB2-22③ 页面脚本常量（kickoff/poll 读 __aiwriteDom；probe 读 __aiwriteDomProbe）");

            aiwrite::ai::ProviderSpec ready_spec;
            ready_spec.kind = "web";
            ready_spec.web  = site;
            aiwrite::ai::ProviderSpec short_spec = ready_spec;
            short_spec.web.answer_selector.clear();
            expect(check, !aiwrite::ai::web_login_only(&ready_spec) &&
                              aiwrite::ai::web_login_only(&short_spec) &&
                              aiwrite::ai::web_adapter_implemented("dom"),
                   "VB2-22④ 就绪度推断：生成字段齐 → 非登录型；缺 answer_selector → 登录型（补齐即就绪）");

            aiwrite::ai::DomChatRequest bad;
            bad.site = short_spec.web; // 缺 answer_selector
            const aiwrite::ai::DomChatResult bad_result = aiwrite::ai::dom_chat(bad);
            expect(check, !bad_result.ok && !bad_result.error.empty(),
                   "VB2-22⑤ dom_chat 前置校验（缺生成字段 → 立即报错；不打开窗口、不发送）");
        }
    }

    {
        std::printf("   -- L4 登录态判定 / 文案与渲染条件（M_patchB PB2-27 / D-27 / D-28① / I15）--\n");

        using aiwrite::ai::ProviderSpec;
        using aiwrite::ai::WebSessionEvidence;
        using aiwrite::ai::WebSessionState;
        using aiwrite::ai::WebSessionVerdict;

        // ---- VB2-24：登录态判定（纯函数 · 5 例；判据**不看** userToken）----
        {
            ProviderSpec dom; // DOM 站点（未配 cookie_names）
            dom.id            = "site-a";
            dom.kind          = "web";
            dom.web.adapter   = "dom";
            dom.web.login_url = "https://site-a.example.com/";

            ProviderSpec dom_named      = dom; // 配了 cookie_names
            dom_named.web.cookie_names  = {"session_token", "sid"};

            const WebSessionVerdict v4 = aiwrite::ai::web_session_state(&dom, WebSessionEvidence{});

            WebSessionEvidence hit; // ① cookie_names 命中
            hit.cookies_known = true;
            hit.cookie_count  = 2;
            hit.cookie_names  = {"sid", "other"};
            const WebSessionVerdict v1 = aiwrite::ai::web_session_state(&dom_named, hit);

            WebSessionEvidence miss = hit; // ② 未命中
            miss.cookie_names       = {"other", "another"};
            const WebSessionVerdict v2 = aiwrite::ai::web_session_state(&dom_named, miss);

            WebSessionEvidence cookies_only; // ③ 未配 cookie_names 但该 origin 有 Cookie
            cookies_only.cookies_known = true;
            cookies_only.cookie_count  = 3;
            const WebSessionVerdict v3 = aiwrite::ai::web_session_state(&dom, cookies_only);

            ProviderSpec token_expr_spec   = dom; // ⑤ 配了 token_expr 但没有 token
            token_expr_spec.web.token_expr = "localStorage.getItem('userToken')";
            WebSessionEvidence no_cookies;
            no_cookies.cookies_known = true;
            const WebSessionVerdict v5 = aiwrite::ai::web_session_state(&token_expr_spec, no_cookies);

            expect(check, v1.state == WebSessionState::logged_in,
                   "VB2-24① 条目 cookie_names 命中 → logged_in");
            expect(check, v2.state == WebSessionState::logged_out,
                   "VB2-24② cookie_names 未命中（已读过该站点 Cookie）→ logged_out");
            expect(check, v3.state == WebSessionState::logged_in,
                   "VB2-24③ 未配 cookie_names 但该 origin 有 Cookie → logged_in（D-27 并集）");
            expect(check,
                   v4.state == WebSessionState::unknown &&
                       v4.reason.find("尚未读取") != std::string::npos,
                   "VB2-24④ 空会话（从未读过 Cookie）→ unknown（不误报「未登录」）");
            expect(check,
                   v5.state == WebSessionState::logged_out &&
                       v5.reason.find("userToken") == std::string::npos,
                   "VB2-24⑤ 配了 token_expr 但没有 token → 仍按 Cookie 判（判据**不看** userToken；I15）");
        }

        // ---- VB2-26：文案与渲染条件（可离线断言部分）----
        {
            const aiwrite::ai::ProviderSpecs  table = aiwrite::ai::load_provider_specs();
            const aiwrite::ai::ProviderSpec* kimi   = table.find("kimi-web");
            const aiwrite::ai::ProviderSpec* ds_web = table.find("deepseek-web");

            const std::string kimi_warn = aiwrite::ai::web_site_field_warnings(kimi);
            expect(check,
                   kimi != nullptr && !kimi_warn.empty() &&
                       kimi_warn.find("无法自动探测凭证") == std::string::npos &&
                       kimi_warn.find("协议探测不适用") != std::string::npos,
                   "VB2-26① DOM 条目的字段警告**不再**含「无法自动探测凭证」（改为「协议探测不适用」）");

            ProviderSpec with_token        = {};
            with_token.kind                = "web";
            with_token.web.token_expr      = "localStorage.getItem('userToken')";
            ProviderSpec without_token     = with_token;
            without_token.web.token_expr.clear();
            expect(check,
                   aiwrite::ai::web_shows_user_token(&with_token) &&
                       !aiwrite::ai::web_shows_user_token(&without_token) &&
                       aiwrite::ai::web_shows_user_token(ds_web) &&
                       !aiwrite::ai::web_shows_user_token(kimi),
                   "VB2-26② userToken 行渲染条件 = !token_expr.empty()（D-28①：deepseek-web 显示 / DOM 站点不显示）");

            const WebSessionVerdict kimi_verdict =
                aiwrite::ai::web_session_state(kimi, WebSessionEvidence{});
            const WebSessionVerdict ds_verdict =
                aiwrite::ai::web_session_state(ds_web, WebSessionEvidence{});
            expect(check,
                   kimi_verdict.reason.find("userToken") == std::string::npos &&
                       kimi_verdict.reason.find("ds_session_id") == std::string::npos &&
                       kimi_verdict.reason.find("/api/v0/") == std::string::npos &&
                       ds_verdict.reason.find("/api/v0/") == std::string::npos,
                   "VB2-26③ 会话结论文案只描述**站点无关**原因（Cookie / 页面），无 DeepSeek 专有名词");
        }

        // ---- VB2-25：协议探测适用性（纯函数 + 脚本分支；PB2-28 / 不变量 I16）----
        {
            const aiwrite::ai::ProviderSpecs  table        = aiwrite::ai::load_provider_specs();
            const aiwrite::ai::ProviderSpec*  deepseek_web = table.find("deepseek-web");
            const aiwrite::ai::ProviderSpec*  kimi_web     = table.find("kimi-web");

            aiwrite::ai::ProviderWebSpec custom_dom; // 自建 dom 条目：无任何探测字段
            custom_dom.adapter   = "dom";
            custom_dom.login_url = "https://site.example.com/";
            aiwrite::ai::ProviderWebSpec dom_with_paths = custom_dom;
            dom_with_paths.probe_paths                   = {"/api/mine/me"};
            aiwrite::ai::ProviderWebSpec dom_with_token = custom_dom;
            dom_with_token.token_expr                   = "localStorage.getItem('myToken')";

            expect(check, aiwrite::ai::probe_is_applicable(deepseek_web),
                   "VB2-25① 内置协议站点（deepseek-web）→ 协议探测**适用**");
            expect(check, kimi_web != nullptr && !aiwrite::ai::probe_is_applicable(kimi_web),
                   "VB2-25② DOM 站点（kimi-web）→ 协议探测**不适用**（I16）");
            expect(check,
                   !aiwrite::ai::probe_is_applicable(custom_dom) &&
                       aiwrite::ai::probe_is_applicable(dom_with_paths) &&
                       aiwrite::ai::probe_is_applicable(dom_with_token),
                   "VB2-25③ 自建 dom 条目：无探测字段 → 不适用；显式配 probe_paths / token_expr → 适用");

            const aiwrite::web::LoginRequest builtin_request; // 默认 = 无参 / CLI 路径
            const std::string builtin_script = aiwrite::web::probe_kickoff_script(builtin_request);
            expect(check,
                   builtin_request.probe_applicable &&
                       builtin_script.find("/api/v0/chat/create_pow_challenge") != std::string::npos &&
                       builtin_script.find("localStorage.getItem('userToken')") != std::string::npos,
                   "VB2-25④ 默认参数 → 脚本仍是内置 DeepSeek 行为（probe_applicable 默认 true；守 I2）");

            aiwrite::web::LoginRequest readonly_request;
            readonly_request.probe_applicable = false;
            const std::string readonly_script =
                aiwrite::web::probe_kickoff_script(readonly_request);
            expect(check,
                   readonly_script.find("/api/v0/") == std::string::npos &&
                       readonly_script.find("localStorage.getItem('userToken')") == std::string::npos &&
                       readonly_script.find("location.href") != std::string::npos &&
                       readonly_script.find("document.cookie") != std::string::npos,
                   "VB2-25⑤ 不适用分支脚本**不含** `/api/v0/` 与 `localStorage.getItem('userToken')`（只读诊断；I16）");
        }

        // ---- VB2-27（新增 · PB2-25 / PB2-28④）：会话失效识别（纯函数）----
        {
            const std::string auth_body = "{\"code\":40002,\"msg\":\"auth failed\"}";
            const std::string pow_body  = "{\"data\":{\"biz_code\":40003}}";
            const std::string ok_body   = "{\"code\":0,\"data\":{\"v\":\"hi\"}}";
            const std::string spaced    = "{ \"code\": 40002 }";

            const std::string hint_401 = aiwrite::ai::web_session_failure_hint(401, "");
            const std::string hint_2   = aiwrite::ai::web_session_failure_hint(200, auth_body);
            const std::string hint_3   = aiwrite::ai::web_session_failure_hint(200, pow_body);
            const std::string hint_ok  = aiwrite::ai::web_session_failure_hint(200, ok_body);

            expect(check, !hint_401.empty() && aiwrite::ai::web_session_failure_needs_relogin(401, ""),
                   "VB2-27① HTTP 401 → 识别为「会话已失效」且需重新登录");
            expect(check,
                   hint_2.find("重新登录") != std::string::npos &&
                       aiwrite::ai::web_session_failure_needs_relogin(200, auth_body) &&
                       aiwrite::ai::web_session_failure_needs_relogin(200, spaced),
                   "VB2-27② code=40002（含带空格写法）→ 提示重新登录并作废该站点会话");
            expect(check,
                   hint_3.find("40003") != std::string::npos &&
                       !aiwrite::ai::web_session_failure_needs_relogin(200, pow_body),
                   "VB2-27③ code=40003 → 给可操作提示但**不**作废会话（PoW / 频率 / 前端版本）");
            expect(check, hint_ok.empty() && !aiwrite::ai::web_session_failure_needs_relogin(200, ok_body),
                   "VB2-27④ 正常响应 → 不误报（hint 为空）");
            expect(check,
                   hint_401.find("userToken") == std::string::npos &&
                       hint_2.find("ds_session_id") == std::string::npos,
                   "VB2-27⑤ 失效提示文案不含厂商专有物（userToken / ds_session_id；I15 口径）");
        }
    }


    {
        std::printf("   -- 会话按站点键控（M_patchB L1 续 / PB2-16）--\n");

        using aiwrite::web::Session;
        using aiwrite::web::SessionStore;
        SessionStore::instance().clear_all(); // 先清干净，避免影响其他自检

        Session session_a;
        session_a.site = "https://a.example.com";
        session_a.cookies.push_back({"site_a", "va", false, true, 0.0});
        SessionStore::instance().set(session_a);
        Session session_b;
        session_b.site = "https://b.example.com";
        session_b.cookies.push_back({"site_b", "vb", false, true, 0.0});
        SessionStore::instance().set(session_b);
        expect(check,
               SessionStore::instance().logged_in("https://a.example.com") &&
                   SessionStore::instance().snapshot("https://a.example.com").has("site_a") &&
                   SessionStore::instance().logged_in("https://b.example.com"),
               "VB2-16 多站点并存：写入 B 后 A 仍在（互不覆盖）");

        aiwrite::web::ProbeResult probe_a;
        probe_a.ok        = true;
        probe_a.token_raw = "token-a";
        SessionStore::instance().set_probe(std::move(probe_a), "https://a.example.com");
        aiwrite::web::ProbeResult probe_b;
        probe_b.ok        = true;
        probe_b.token_raw = "token-b";
        SessionStore::instance().set_probe(std::move(probe_b), "https://b.example.com");
        expect(check, SessionStore::instance().has_token("https://a.example.com") &&
                          SessionStore::instance().has_token("https://b.example.com"),
               "VB2-16 凭证按站点：两个站点各自持有 userToken");

        SessionStore::instance().clear("https://a.example.com");
        expect(check, !SessionStore::instance().has_token("https://a.example.com") &&
                          !SessionStore::instance().logged_in("https://a.example.com") &&
                          SessionStore::instance().has_token("https://b.example.com") &&
                          SessionStore::instance().logged_in("https://b.example.com"),
               "VB2-16 按站点注销：清 A 不影响 B");

        const std::vector<std::string> sites = SessionStore::instance().sites();
        expect(check, sites.size() == 1 && sites.front() == "https://b.example.com",
               "VB2-16 sites()：只列出仍有会话的站点（顺序稳定）");

        // ---- 兼容：不传站点的旧 API（默认槽）行为与改造前一致 ----
        SessionStore::instance().clear_all();
        Session plain; // 不带站点 = 改造前的用法
        plain.url = "https://chat.deepseek.com/";
        plain.cookies.push_back({"ds_session_id", "vv", false, true, 0.0});
        SessionStore::instance().set(plain);
        const bool legacy_slot_ok = SessionStore::instance().logged_in() &&
                                    SessionStore::instance().cookie_count() == 1 &&
                                    SessionStore::instance().snapshot().has("ds_session_id");
        SessionStore::instance().clear();
        expect(check, legacy_slot_ok && !SessionStore::instance().logged_in() &&
                          SessionStore::instance().cookie_count() == 0,
               "VB2-16 兼容：不传站点的旧 API（默认槽）行为不变");
        SessionStore::instance().clear_all(); // 归还干净状态
    }

    {
        std::printf("   -- Provider 配置表（M_patchB L1 / PB2-01）--\n");
        int       spec_passed = 0;
        const int spec_failed = aiwrite::ai::provider_spec_selftest(&spec_passed);
        check.passed += spec_passed;
        check.failed += spec_failed;
    }

    std::printf("=== 执行器自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

// M7 诊断入口：--image-decode <路径>（只读、不联网、不改任何状态）
//   用途：① 排查「无法识别/无法预览某张图」② 验证 stb / WIC 两条解码路径
int image_decode_report(const std::string& path)
{
    std::printf("=== 图片诊断（M7）===\n");
    std::printf("路径        : %s\n", path.c_str());

    const aiwrite::utils::ImageMagic magic = aiwrite::utils::sniffImage(path);
    std::printf("内容嗅探    : %s（扩展名 %s；按扩展名的 MIME %s）\n",
                aiwrite::utils::imageFormatName(magic.format),
                magic.ext_label.empty() ? "(无)" : magic.ext_label.c_str(), magic.ext_mime.c_str());
    std::printf("内容 MIME   : %s（发给模型时使用）\n",
                aiwrite::ai::image_mime_from_path(path).c_str());
    std::printf("扩展名一致  : %s\n", magic.mismatch ? "否 —— 扩展名与实际格式不一致" : "是");
    std::printf("文件头      : %s\n", magic.head_hex.empty() ? "(读不到)" : magic.head_hex.c_str());
    std::printf("WIC 能力    : %s\n", aiwrite::utils::wicInfo().note.c_str());

    const aiwrite::utils::DecodedImage decoded = aiwrite::utils::decodeImageRgba8(path);
    if (decoded.error.empty()) {
        std::printf("本地解码    : OK（解码器 %s，%d×%d，RGBA8 %zu 字节）\n", decoded.decoder.c_str(),
                    decoded.width, decoded.height, decoded.rgba.size());
    }
    else {
        std::printf("本地解码    : FAIL\n  可操作提示: %s\n", decoded.error.c_str());
    }

    int         width      = 0;
    int         height     = 0;
    std::string size_error;
    if (aiwrite::utils::readImageSize(path, &width, &height, &size_error)) {
        std::printf("只读尺寸    : %d×%d\n", width, height);
    }
    else {
        std::printf("只读尺寸    : FAIL（%s）\n", size_error.c_str());
    }
    std::printf("结论        : %s\n", decoded.error.empty() ? "本地可预览" : "本地不可预览（见上）");
    return decoded.error.empty() ? 0 : 2;
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
        else if (arg == "--image-decode") {
            options.image_path = next("--image-decode");
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
        !options.exec_selftest && options.http_url.empty() && options.chat_prompt.empty() &&
        options.image_path.empty()) {
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

    if (!options.image_path.empty()) { // M7：图片诊断（只读，不联网）
        const int image_code = image_decode_report(options.image_path);
        if (image_code != 0) {
            exit_code = image_code;
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
