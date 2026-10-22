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
#include "crypto.h"
#include "log.h"
#include "window_geometry.h"
#include "paths.h"

#include "graph.h"
#include "autosave.h"     // PC-01/02：自动保存 / 崩溃恢复（--autosave-assert）
#include "node_registry.h"
#include "recent_files.h"
#include "undo_stack.h"
#include "validate.h"
#include "provider_resolve.h" // M5-02：生效提供商 / 模型名断言
#include "workflow_io.h"
#include "migration.h"       // M_patchAB §4（PC-04）：版本迁移（workflow + config）离线断言
#include "composite_selftest.h" // CN：上级节点（Composition_node）自检
#include "nodes.h"
#include "config.h"
#include "output_archive.h"
#include "paths.h"
#include "session_store.h"
#include "web_owner.h"     // M_patchC（PW-01）：归属（纯函数断言）
#include "ready_level.h"   // M_patchC（PW-03）：就绪三级（纯函数断言）
#include "web_error.h"     // M_patchC（PW-05）：三类归因文案（纯函数断言）
#include "probe_verdict.h" // M_patchAB §2.2/§3.3（PB-07 / PB2-25）：会话诊断（判据收紧 + 错误串反查）
#include "window_table.h"  // M_patchC（PW-02 / PW-06 / PW-08）：窗口表 + 复位计划（纯函数断言）
#include "webview_host.h" // plan_session_boot / interactive_login_request（纯逻辑断言用）
#include "cancel_token.h"        // M_patchAB §2.4（PB-09 V-12）：取消令牌 / 取消监视断言
#include "retry_policy.h"        // M_patchAB §2.6（PB-08 残项）：重试策略断言
#include "deepseek_official_provider.h" // M5-02：多模态请求体断言
#include "provider_spec.h"              // M_patchB L1：Provider 配置表断言
#include "dom_web_client.h"             // L3（PB2-13/15）：DOM 适配器纯函数断言（VB2-22）
#include "webview_host.h"              // L4（PB2-28）：探测脚本适用性断言（VB2-25）
#include "channel_frames.h"           // M7B 批 1：管道协议 v1 帧（VB2-29 / VB2-32 · §6.1）
#include "session_snapshot.h"        // M7B 批 2：L2 快照只读视图（VB2-39⑥ · MB-D0-8）
#include "pydoll_channel.h"          // M7B 批 2：新通道如实报错断言（VB2-40④⑤ · I21）
#include "deepseek_web_client.h"       // L4（PB2-28④）：会话失效识别断言（VB2-27）
#include "image_decode.h"           // M7-04/05：图片格式嗅探 + 解码 + 失败文案断言
#include "image_convert.h"      // M8-14：图片准备（白名单 / 限额 → 缩放 + 转码）离线断言
#include "image_preprocess.h"   // M_8nodes_extension L1-c（§4.2）：图片预处理（用户参数 → 裁剪/缩放）
#include "upload_evidence.h"    // M8-14：上传双证据（I18）离线断言
#include "upload_registry.h"    // M8-14：站点上传注册表 + 契约离线断言
#include "asset_store.h"            // P7a-04：统一资源目录（令牌 / 归档 / 解析）断言
#include "app_version.h"        // M6：诊断报告断言（版本号单一来源）
#include "diagnostics_report.h" // M6（PM-04）：诊断报告离线断言
#include "settings_validation.h" // M6（M6-01）：设置面板校验纯函数断言
#include "webview2_runtime.h"    // M6（PM-01）：依赖自检（WebView2 运行时）
#include "site_template.h"        // PB2-24：自建网页版站点模板 + 落盘断言
#include "official_template.h"    // 自定义 AI 收口：自建官方 API 提供商模板 + 落盘断言
#include "inference_provider.h"   // L2（B2-b · PB2-08/12）：统一推理接口 + 能力门控断言
#include "provider_factory.h"     // L2（B2-b · PB2-09）：按表 protocol 分派断言（R9）
#include "anthropic_provider.h"   // L2（B2-b · PB2-10）：Anthropic Messages 请求构造断言
#include "gemini_provider.h"      // L2（B2-b · PB2-11）：Gemini generateContent 请求构造断言
#include "http_json.h"            // L2（B2-b）：通用 JSON POST / query 拼接断言
#include "http_net.h"             // M_patchAB §2.3（PB-06 剩余）：代理 / 自签证书策略断言
#include "credential.h"           // M_patchAB §2.3（PB-06 剩余）：凭据库配置（目录 / 后端）
#include "text_export.h"          // M_8nodes_extension L1-b（§4.8）：N-08 导出模板 / 自动命名断言

#include <fstream>
#include <sstream>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 引擎层位于 namespace aiwrite::engine，这里起别名便于书写
namespace engine = aiwrite::engine;

#include "api_internal.h"


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
                aiwrite::ai::imageMimeFromPath(path).c_str());
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

// ============================================================================
//  M6（非技术用户可安装化）自检 —— 三个开关各自打印**单行 PASS/FAIL**
//  * 说明：不计入 `--selftest` / `--exec-selftest` 的任何分组数字（口径同「窗口几何」行）
//  * 不联网、不开窗口、不改用户数据（诊断报告本身是只读采集）
// ============================================================================

// PM-04：诊断报告离线断言（分节齐全 / 版本号同源 / 脱敏 / 关键键有值）
bool test_diag_report()
{
    std::printf("[M6] 诊断报告自检\n");
    const aiwrite::utils::DiagnosticsReport report = aiwrite::utils::collectDiagnostics();
    const std::string                    text     = report.text();

    const char* sections[] = {"[报告] AIwrite 诊断信息", "[系统]", "[依赖]",  "[路径]",
                              "[配置]",                    "[站点表]", "[凭据]", "[归档与工作流]"};
    bool        sections_ok = true;
    for (const char* section : sections) {
        const bool found = text.find(section) != std::string::npos;
        if (!found) {
            std::printf("         缺分节: %s\n", section);
            sections_ok = false;
        }
    }

    const std::string version   = report.keyValue("程序版本");
    const std::string webview2  = report.keyValue("WebView2");
    const std::string data_dir  = report.keyValue("数据目录");
    const std::string entries   = report.keyValue("条目总数");
    const std::string unwired   = report.keyValue("未接配置字段");

    const bool version_ok = version == aiwrite::kAppVersion;
    const bool dep_ok     = webview2.find("WebView2 运行时") == 0;
    const bool path_ok    = data_dir.find(".brain-ai") != std::string::npos;
    const bool specs_ok   = entries.find("官方 API") != std::string::npos &&
                            entries.find("网页版") != std::string::npos;
    const bool unwired_ok = unwired.find("项（") != std::string::npos;
    // 脱敏（硬要求）：报告里不得出现常见密钥/令牌形态
    const bool redaction_ok = text.find("sk-") == std::string::npos &&
                              text.find("token =") == std::string::npos &&
                              text.find("api_key =") == std::string::npos;

    const bool ok = sections_ok && version_ok && dep_ok && path_ok && specs_ok && unwired_ok &&
                    redaction_ok;
    std::printf("   %-6s 诊断报告：分节 %s / 版本号同源(%s) %s / 依赖行 %s / 数据目录 %s / "
                "站点表 %s / 未接清单 %s / 脱敏 %s / 共 %zu 行\n",
                ok ? "PASS" : "FAIL", sections_ok ? "齐全" : "缺", version.c_str(),
                version_ok ? "是" : "否", dep_ok ? "是" : "否", path_ok ? "是" : "否",
                specs_ok ? "是" : "否", unwired_ok ? "是" : "否", redaction_ok ? "是" : "否",
                report.lines.size());
    return ok;
}

// PM-01：依赖自检（**只读探测**；缺依赖 ⇒ 返回 false ⇒ exit 1，并打印安装指引）
bool test_dep_check()
{
    std::printf("[M6] 依赖自检\n");
    const aiwrite::utils::WebView2RuntimeInfo info = aiwrite::utils::detectWebview2Runtime();
    std::printf("   %-6s %s\n", info.installed ? "PASS" : "FAIL",
                aiwrite::utils::webview2_runtime_summary(info).c_str());
    // `PQ-02`：安装包可附带 Evergreen Bootstrapper —— 有就报路径（便于支持人员核对）
    const std::string bundled = aiwrite::utils::bundledBootstrapperPath();
    std::printf("   随包引导程序 = %s\n",
                bundled.empty() ? "（未附带：界面/文档给官方下载页）" : bundled.c_str());
    if (!info.installed) {
        std::printf("%s", aiwrite::utils::webview2_install_advice().c_str());
    }
    return info.installed;
}

// M6-01：设置面板校验纯函数断言（默认通过 + 各类越界被拒）
bool test_settings_assert()
{
    std::printf("[M6] 设置校验自检（utils::validate_settings）\n");
    const aiwrite::Config good;

    struct Case {
        const char*  label;
        aiwrite::Config config;
    };
    std::vector<Case> bad_cases;
    {
        aiwrite::Config c = good;
        c.output.archive_dir.clear();
        bad_cases.push_back({"归档目录为空", c});
    }
    {
        aiwrite::Config c = good;
        c.ui.console_height = 10;
        bad_cases.push_back({"Console 高度 10（<60）", c});
    }
    {
        aiwrite::Config c = good;
        c.ui.grid_size = 1;
        bad_cases.push_back({"网格间距 1（<4）", c});
    }
    {
        aiwrite::Config c = good;
        c.output.ttl_days = -1;
        bad_cases.push_back({"归档清理阈值 -1", c});
    }
    {
        aiwrite::Config c = good;
        c.output.max_history = 0;
        bad_cases.push_back({"归档保留份数 0", c});
    }
    {
        aiwrite::Config c = good;
        c.timeout.connect_ms = 10;
        bad_cases.push_back({"连接超时 10ms（<1000）", c});
    }
    {
        aiwrite::Config c = good;
        c.advanced.log_ttl_days = 99999;
        bad_cases.push_back({"logs 保留天数 99999", c});
    }
    {
        aiwrite::Config c = good;
        c.error.retry_count = 99;
        bad_cases.push_back({"重试次数 99（>10）", c});
    }
    {
        aiwrite::Config c = good;
        c.ui.autosave_idle_sec = 0; // PC-01：静默阈值下限 = 1 秒
        bad_cases.push_back({"自动保存间隔 0（<1）", c});
    }
    {
        aiwrite::Config c = good;
        c.ui.autosave_keep = -1; // PC-01：历史份数 0 = 关闭，负数非法
        bad_cases.push_back({"自动保存历史份数 -1", c});
    }
    {
        aiwrite::Config c = good;
        c.ui.autosave_keep = 99; // 上限 50
        bad_cases.push_back({"自动保存历史份数 99（>50）", c});
    }
    // M_patchAB §2.3（PB-06 剩余）：网络与凭据配置
    {
        aiwrite::Config c = good;
        c.network.proxy = "127.0.0.1:7890"; // 缺 scheme
        bad_cases.push_back({"代理缺 http:// 前缀", c});
    }
    {
        aiwrite::Config c = good;
        c.network.proxy = "http://"; // 有 scheme 无主机
        bad_cases.push_back({"代理只有 scheme 没有主机", c});
    }
    {
        aiwrite::Config c = good;
        c.credentials.ttl_days = -1; // 0 = 不清理，负数非法
        bad_cases.push_back({"凭据保留天数 -1", c});
    }
    {
        aiwrite::Config c = good;
        c.credentials.backend = "kms"; // 本版本只支持 dpapi
        bad_cases.push_back({"凭据后端 kms（未实现）", c});
    }

    const std::string default_error = aiwrite::utils::validateSettings(good);
    bool              bad_ok        = true;
    for (const Case& item : bad_cases) {
        const std::string error = aiwrite::utils::validateSettings(item.config);
        if (error.empty()) {
            std::printf("         未拦截: %s\n", item.label);
            bad_ok = false;
        }
    }

    // 边界值必须**通过**（不外扩误伤）
    aiwrite::Config edge = good;
    edge.ui.console_height  = 60;
    edge.ui.grid_size       = 200;
    edge.output.ttl_days    = 0;
    edge.output.max_history = 1;
    edge.ui.autosave_idle_sec = 1;  // PC-01：边界值（下限）
    edge.ui.autosave_keep     = 50; // PC-01：边界值（上限）
    edge.network.proxy        = "http://127.0.0.1:7890"; // PB-06：合法代理形态
    edge.network.insecure_tls = true;                     // PB-06：自签兼容开关**合法**（不拦）
    edge.credentials.ttl_days = 0;                        // PB-06：0 = 不清理
    const bool edge_ok = aiwrite::utils::validateSettings(edge).empty();

    const bool ok = default_error.empty() && bad_ok && edge_ok;
    std::printf("   %-6s 默认配置通过=%s / 越界 %zu 例全部拦截=%s / 边界值通过=%s\n",
                ok ? "PASS" : "FAIL", default_error.empty() ? "是" : "否", bad_cases.size(),
                bad_ok ? "是" : "否", edge_ok ? "是" : "否");
    if (!ok && !default_error.empty()) {
        std::printf("         默认配置被误拒: %s\n", default_error.c_str());
    }
    return ok;
}

// PB2-24：自建网页版站点**闭环**断言（纯函数 + 真实落盘 → 重载 → 立即可用；**自清理**）
//  ① 纯函数：ai::site_template_selftest()（生成 / 校验 / 覆盖策略 / 缺信封护栏）
//  ② 端到端：写 ~/.brain-ai/providers.d/<id>.json → reload → 三处同时生效：
//       · 条目进表（kind=web / adapter=dom）· 进「网页版」下拉来源（ids("web")）
//       · 进 `provider` 参数枚举（engine::provider_param_options()）
//     ＋新建的 ProviderConfig 节点实例**运行前校验通过**（要点②的核心回归）
//  ③ 收尾：删文件 + reload ⇒ 条目消失 / 条目数回落（零残留）
bool test_site_assert()
{
    std::printf("[PB2-24] 自建网页版站点闭环：新建 → 重新加载 → 下拉 / 校验即时可用\n");

    int       pure_passed = 0;
    const int pure_failed = aiwrite::ai::siteTemplateSelftest(&pure_passed);

    // 模拟**应用启动顺序**：先注册节点类型（`ProviderConfig` 的 provider 枚举在此一次性生成），
    // 之后才是「新建站点 → 重新加载」——只有这样才能证明刷新入口（要点②）真的生效
    aiwrite::engine::registerAllNodes();
    const std::size_t enum_before = aiwrite::engine::providerParamOptions().size();

    // ---- 端到端：临时写进真实用户目录（固定 id；开始前清残留，结束前必删）----
    const std::string            site_id = "aiwrite-selftest-site";
    aiwrite::ai::WebSiteTemplate tpl;
    tpl.id              = site_id;
    tpl.display         = "自检站点（AIwrite；由 --site-assert 临时创建）";
    tpl.login_url       = "https://selftest.example.com/chat";
    tpl.input_selector  = "div.selftest-input";
    tpl.send_kind       = "key";
    tpl.send_value      = "Enter";
    tpl.answer_selector = ".selftest-answer";
    tpl.done_kind       = "selector_gone";
    tpl.done_selector   = "button.selftest-stop";
    tpl.cookie_names    = "selftest_session, selftest_token";
    tpl.notes           = "自检写入：本次自检结束前会被删除";

    const std::filesystem::path dir  = aiwrite::paths::userProvidersDir();
    const std::filesystem::path file = dir / (site_id + ".json");
    std::error_code             ec;
    std::filesystem::remove(file, ec); // 上次异常中断的残留（幂等）

    const std::size_t before_items = aiwrite::ai::providerSpecs().items.size();

    std::string err;
    const bool  wrote = aiwrite::ai::write_user_site_file(tpl, dir, false, &err, nullptr);
    if (!wrote) {
        std::printf("         落盘失败: %s\n", err.c_str());
    }
    bool        in_table    = false;
    bool        in_web_ids  = false;
    bool        in_enum     = false;
    bool        enum_grew   = false; // 枚举确实**被重建**（而非注册时恰好已含）
    bool        fresh_ok    = false; // 新建节点（重载后建）校验通过
    bool        stale_ok    = false; // **旧实例**（重载前已存在的节点）刷新后校验通过
    std::string instance_err;
    if (wrote) {
        // 界面「创建并重新加载」的等价动作：reload 表（面板读结果）→ 重建 provider 枚举（宿主）
        aiwrite::ai::reloadProviderSpecs();
        const aiwrite::ai::ProviderSpecs& table   = aiwrite::ai::providerSpecs();
        const aiwrite::ai::ProviderSpec*  created = table.find(site_id);
        in_table = created != nullptr && created->kind == "web" && created->web.adapter == "dom";
        for (const std::string& id : table.ids("web")) {
            if (id == site_id) {
                in_web_ids = true;
            }
        }
        aiwrite::engine::refreshProviderOptions();
        const std::vector<std::string> enum_after = aiwrite::engine::providerParamOptions();
        for (const std::string& id : enum_after) {
            if (id == site_id) {
                in_enum = true;
            }
        }
        enum_grew = enum_after.size() > enum_before; // 证明重建入口真干活（要点②）

        // 新建 ProviderConfig 节点并选中新站点 → **运行前校验必须通过**（要点②回归）
        aiwrite::engine::Graph graph;
        std::string            node_error;
        const std::string      node_id = graph.addNode("ProviderConfig", 0.0f, 0.0f, &node_error);
        aiwrite::engine::Node* node    = graph.findNode(node_id);
        if (node != nullptr) {
            if (aiwrite::engine::Param* provider = node->findParam("provider")) {
                provider->value = site_id;
            }
            if (aiwrite::engine::Param* mode = node->findParam("mode")) {
                mode->value = std::string("web");
            }
            std::vector<std::string> errors;
            graph.validateParams(*node, &errors);
            fresh_ok = errors.empty();
            if (!fresh_ok) {
                instance_err = errors.front();
            }

            // 模拟「重载前已存在的节点」：把实例枚举打回旧值，再走一次刷新入口
            if (aiwrite::engine::Param* provider = node->findParam("provider")) {
                provider->enum_options = {"stale-only"};
            }
            std::vector<std::string> stale_before;
            graph.validateParams(*node, &stale_before); // 未刷新 ⇒ 必须被判越界（证明护栏真实存在）
            aiwrite::engine::refreshProviderOptions(graph);
            std::vector<std::string> stale_errors;
            graph.validateParams(*node, &stale_errors);
            stale_ok = !stale_before.empty() && stale_errors.empty();
            if (!stale_ok && instance_err.empty() && !stale_errors.empty()) {
                instance_err = stale_errors.front();
            }
        }
    }

    // ---- 收尾：删除 + 重载 ⇒ 条目消失 / 条目数回落（零残留；不给用户留垃圾）----
    std::filesystem::remove(file, ec);
    // 单入口（`reload_provider_specs_and_refresh`）—— 无画布 ⇒ 只刷注册表 Definition
    aiwrite::engine::reloadProviderSpecsAndRefresh();
    const bool cleaned = aiwrite::ai::providerSpecs().find(site_id) == nullptr &&
                         !std::filesystem::exists(file);
    const bool count_back = aiwrite::ai::providerSpecs().items.size() == before_items;

    const bool ok = pure_failed == 0 && wrote && in_table && in_web_ids && in_enum && enum_grew &&
                    fresh_ok && stale_ok && cleaned && count_back;
    std::printf("   %-6s 纯函数 %d/%d · 写盘=%s 进表=%s 进网页版下拉=%s 进 provider 枚举=%s"
                "（%zu→%zu）· 新节点校验=%s 旧实例刷新=%s · 清理=%s 条目数回落=%s（%zu → %zu）\n",
                ok ? "PASS" : "FAIL", pure_passed, pure_passed + pure_failed,
                wrote ? "是" : "否", in_table ? "是" : "否", in_web_ids ? "是" : "否",
                in_enum ? "是" : "否", enum_before, enum_before + (enum_grew ? 1u : 0u),
                fresh_ok ? "是" : "否", stale_ok ? "是" : "否", cleaned ? "是" : "否",
                count_back ? "是" : "否", before_items, aiwrite::ai::providerSpecs().items.size());
    if (!instance_err.empty()) {
        std::printf("         节点校验: %s\n", instance_err.c_str());
    }
    return ok;
}


// 自定义 AI 收口：自建官方 API 提供商**闭环**断言（纯函数 + 真实落盘 → 重载 → 立即可用；**自清理**）
//  ① 纯函数：ai::officialTemplateSelftest()（生成 / 校验 / 覆盖策略 / 缺信封护栏）
//  ② 端到端：写 ~/.brain-ai/providers.d/<id>.json → reload → 同时生效：
//       · 条目进表（kind=official）· 进 `provider` 参数枚举（engine::providerParamOptions()）
//     ＋新建的 ProviderConfig 节点实例**运行前校验通过**（要点②的核心回归）
//  ③ 收尾：删文件 + reload ⇒ 条目消失 / 条目数回落（零残留）
bool test_official_assert()
{
    std::printf("[自定义 AI 收口] 自建官方 API 提供商闭环：新建 → 重新加载 → 下拉 / 校验即时可用\n");

    int       pure_passed = 0;
    const int pure_failed = aiwrite::ai::officialTemplateSelftest(&pure_passed);

    // 模拟**应用启动顺序**：先注册节点类型，之后才是「新建提供商 → 重新加载」
    aiwrite::engine::registerAllNodes();
    const std::size_t enum_before = aiwrite::engine::providerParamOptions().size();

    // ---- 端到端：临时写进真实用户目录（固定 id；开始前清残留，结束前必删）----
    const std::string id = "aiwrite-selftest-official";
    aiwrite::ai::OfficialProviderTemplate tpl = aiwrite::ai::officialTemplateDefault();
    tpl.id       = id;
    tpl.display  = "自检 API 提供商（AIwrite；由 --official-assert 临时创建）";
    tpl.protocol = "openai";
    tpl.api_base = "https://selftest.example.com/v1";
    tpl.models   = "selftest-model";
    tpl.env_name = "SELFTEST_KEY";
    tpl.notes    = "自检写入：本次自检结束前会被删除";

    const std::filesystem::path dir  = aiwrite::paths::userProvidersDir();
    const std::filesystem::path file = dir / (id + ".json");
    std::error_code             ec;
    std::filesystem::remove(file, ec); // 上次异常中断的残留（幂等）

    const std::size_t before_items = aiwrite::ai::providerSpecs().items.size();

    std::string err;
    const bool  wrote = aiwrite::ai::writeUserOfficialFile(tpl, dir, false, &err, nullptr);
    if (!wrote) {
        std::printf("         落盘失败: %s\n", err.c_str());
    }
    bool        in_table  = false;
    bool        in_enum   = false;
    bool        enum_grew = false; // 枚举确实**被重建**（而非注册时恰好已含）
    bool        fresh_ok  = false; // 新建节点（重载后建）校验通过
    bool        stale_ok  = false; // **旧实例**（重载前已存在的节点）刷新后校验通过
    std::string instance_err;
    if (wrote) {
        // 界面「创建并重新加载」的等价动作：reload 表（面板读结果）→ 重建 provider 枚举（宿主）
        aiwrite::ai::reloadProviderSpecs();
        const aiwrite::ai::ProviderSpecs& table   = aiwrite::ai::providerSpecs();
        const aiwrite::ai::ProviderSpec*  created = table.find(id);
        in_table = created != nullptr && created->kind == "official";
        aiwrite::engine::refreshProviderOptions();
        const std::vector<std::string> enum_after = aiwrite::engine::providerParamOptions();
        for (const std::string& option : enum_after) {
            if (option == id) {
                in_enum = true;
            }
        }
        enum_grew = enum_after.size() > enum_before; // 证明重建入口真干活（要点②）

        // 新建 ProviderConfig 节点并选中新提供商 → **运行前校验必须通过**（要点②回归）
        aiwrite::engine::Graph graph;
        std::string            node_error;
        const std::string      node_id = graph.addNode("ProviderConfig", 0.0f, 0.0f, &node_error);
        aiwrite::engine::Node* node    = graph.findNode(node_id);
        if (node != nullptr) {
            if (aiwrite::engine::Param* provider = node->findParam("provider")) {
                provider->value = id;
            }
            if (aiwrite::engine::Param* mode = node->findParam("mode")) {
                mode->value = std::string("official");
            }
            std::vector<std::string> errors;
            graph.validateParams(*node, &errors);
            fresh_ok = errors.empty();
            if (!fresh_ok) {
                instance_err = errors.front();
            }

            // 模拟「重载前已存在的节点」：把实例枚举打回旧值，再走一次刷新入口
            if (aiwrite::engine::Param* provider = node->findParam("provider")) {
                provider->enum_options = {"stale-only"};
            }
            std::vector<std::string> stale_before;
            graph.validateParams(*node, &stale_before); // 未刷新 ⇒ 必须被判越界（护栏真实存在）
            aiwrite::engine::refreshProviderOptions(graph);
            std::vector<std::string> stale_errors;
            graph.validateParams(*node, &stale_errors);
            stale_ok = !stale_before.empty() && stale_errors.empty();
            if (!stale_ok && instance_err.empty() && !stale_errors.empty()) {
                instance_err = stale_errors.front();
            }
        }
    }

    // ---- 收尾：删除 + 重载 ⇒ 条目消失 / 条目数回落（零残留；不给用户留垃圾）----
    std::filesystem::remove(file, ec);
    // 单入口（`reload_provider_specs_and_refresh`）—— 无画布 ⇒ 只刷注册表 Definition
    aiwrite::engine::reloadProviderSpecsAndRefresh();
    const bool cleaned = aiwrite::ai::providerSpecs().find(id) == nullptr &&
                         !std::filesystem::exists(file);
    const bool count_back = aiwrite::ai::providerSpecs().items.size() == before_items;

    const bool ok = pure_failed == 0 && wrote && in_table && in_enum && enum_grew &&
                    fresh_ok && stale_ok && cleaned && count_back;
    std::printf("   %-6s 纯函数 %d/%d · 写盘=%s 进表=%s 进 provider 枚举=%s（%zu→%zu）· "
                "新节点校验=%s 旧实例刷新=%s · 清理=%s 条目数回落=%s（%zu → %zu）\n",
                ok ? "PASS" : "FAIL", pure_passed, pure_passed + pure_failed,
                wrote ? "是" : "否", in_table ? "是" : "否", in_enum ? "是" : "否",
                enum_before, enum_before + (enum_grew ? 1u : 0u),
                fresh_ok ? "是" : "否", stale_ok ? "是" : "否", cleaned ? "是" : "否",
                count_back ? "是" : "否", before_items, aiwrite::ai::providerSpecs().items.size());
    if (!instance_err.empty()) {
        std::printf("         节点校验: %s\n", instance_err.c_str());
    }
    return ok;
}

bool test_autosave_assert()
{
    std::printf("[PC-01/02] 自动保存 / 崩溃恢复断言（M_patchAB §4：VC-01 原子写 · VC-02 恢复流程）\n");

    int       passed        = 0;
    const int failed        = aiwrite::engine::autosaveSelftest(&passed);
    const bool pure_ok      = failed == 0;

    // 落点回归：默认目录必须是 snapshots/（与 PC-01 计划「复用 snapshots_dir()」一致）
    const std::filesystem::path expected = aiwrite::paths::snapshotsDir() / "autosave.json";
    const bool                  path_ok  = aiwrite::engine::autosavePaths().latest == expected;

    const bool ok = pure_ok && path_ok;
    std::printf("   %-6s 场景 %d/%d · 默认落点=%s（%s）\n", ok ? "PASS" : "FAIL", passed,
                passed + failed, path_ok ? "是" : "否", expected.string().c_str());
    return ok;
}
