// =============================================================================
//  AIwrite 主程序入口（M1：技术验证 + 项目骨架）
//  GUI 程序（/SUBSYSTEM:WINDOWS + mainCRTStartup），启动时初始化日志与配置目录
// =============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // M7B 批 1 step 2：--pipe-selftest（CreateProcessW / WaitForSingleObject）

#include "app.h"
#include "editor_state.h"
#include "output_panel.h"
#include "deepseek_official_provider.h"
#include "provider_spec.h"   // M_patchB L1：Provider 配置表（--provider-selftest）
#include "provider_factory.h"    // L2（B2-b · PB2-09）：按表 protocol 分派
#include "inference_provider.h"  // L2（B2-b · PB2-08）：统一推理接口（ping 走同一入口）
#include "node_registry.h"
#include "workflow_io.h"   // VA-07：graph_to_json（运行前 JSON 基线 / TST-L01 判据）
#include "deepseek_web_client.h"
#include "dom_web_client.h"      // L3（PB2-13/15）：通用 DOM 站点适配器 + 选择器探测
#include "upload_supply_probe.h"   // M8 Phase 2：C++ 供料探针（--web-supply-probe）
#include "upload_registry.h"  // M8-14：站点上传模板（--upload-selftest）
#include "image_convert.h"    // M8-14：图片准备（离线断言）
#include "upload_evidence.h"  // M8-14：上传双证据（离线断言）
#include "config.h"
#include "text_export.h"
#include "credential.h"
#include "log.h"
#include "paths.h"
#include "webview_host.h"
#include "pipe_client.h"      // M7B 批 1 step 2：命名管道客户端（--pipe-selftest / M7B-02）
#include "pydoll_channel.h"   // M7B 批 2：新通道（--pydoll-login / --pydoll-selftest）
#include "session_store.h"   // M_patchB：--provider-selftest 的网页版登录态检查
#include "connection_test.h" // M_patchAB（PB2-07 / PB2-30②）：退出前收尾后台诊断线程
#include "prompt_generator.h" // M_8nodes_extension L1-d（§4.3）：退出前收尾「生成提示词」线程
#include "session_snapshot.h" // 批 3（FEA-L02）：--web-session-list 的 L2 快照只读元数据

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "cli_internal.h"

// ============================================================================
//  入口 main（CLN-14 拆分后保留）：CLI 参数分派 + 启动/退出收尾
//  * 自检 / 诊断函数与通用辅助已拆至 cli_selftest.cpp / cli_diag.cpp（cli_internal.h 声明）
// ============================================================================

int main(int argc, char** argv)
{
    aiwrite::ui::AppOptions options;
    bool login_selftest   = false;
    bool run_selftest_flag = false;
    bool run_selftest_web  = false;
    bool run_selftest_official = false; // M_patchAB §2.4（PB-09）：有 Key 时真实生成一次
    bool web_probe_flag    = false;
    bool web_session_selftest_flag = false;
    bool dom_adapter_selftest_flag = false; // L3（PB2-15）：选择器探测 / 诊断
    bool dom_dump_flag             = false; // L4（PB2-29 / v15）：选择器候选枚举
    bool pipe_selftest_flag        = false; // M7B 批 1 step 2（M7B-02）：命名管道冒烟
    bool        pydoll_selftest_flag = false; // M7B 批 2：新通道自检（**不开浏览器**）
    std::string pydoll_login_id;              // M7B 批 2：--pydoll-login <id>（非空 = 走新通道登录）
    std::string web_chat_prompt;
    bool        vlm_selftest_flag = false;                              // M5-02 图片理解自检
    std::string vlm_image;                                              // --image
    std::string vlm_api_base;                                           // 空 = 用表/内置默认（D-07 智谱）
    std::string vlm_model;                                              // 空 = 用表/内置默认
    std::string vlm_key_ref;                                            // 空 = 用表/内置默认
    bool        provider_selftest_flag = false;                         // M_patchB L1 配置表自检
    bool        provider_dump_flag     = false;                         // 打印生效配置表
    bool        supply_probe_flag  = false;                             // M8 Phase 2：--web-supply-probe（**只读**供料探针）
    bool        upload_selftest_flag = false;                           // M8-14：--upload-selftest（离线三组；带 --provider 追加真机）
    std::string supply_get_path;                                        // --get <路径?查询串>（空 = 只读模式）
    std::string provider_id;                                            // --provider <id>
    std::string report_json_path;                                       // --json <报告路径>
    std::string report_prev_path;                                       // --json-diff <上一轮报告>
    bool        web_session_list_flag = false;                          // --web-session-list
    int  selftest_timeout = 30;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--console") {
            options.console = true;
        }
        else if (arg == "--open") {
            // 启动即打开指定工作流（见 ui/app.h AppOptions::open_workflow）
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --open 缺少取值（工作流 JSON 路径）\n");
                return 2;
            }
            options.open_workflow = argv[++i];
        }
        else if (arg == "--auto-open-composite") {
            // [调试] 程序化走「新建上级节点」路径，复现窗口栈断言（配合看门狗）
            options.auto_open_composite = true;
        }
        else if (arg == "--login-selftest") {
            login_selftest = true;
        }
        else if (arg == "--pipe-selftest") {
            pipe_selftest_flag = true;
        }
        else if (arg == "--pydoll-selftest") {
            pydoll_selftest_flag = true;
        }
        else if (arg == "--pydoll-login") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --pydoll-login 缺少取值（provider id）\n");
                return 2;
            }
            pydoll_login_id = argv[++i];
        }
        else if (arg == "--cred-list") {
            // M_patchAB §2.3（PB-06 剩余）：TTL 取自 `[credentials].ttl_days`
            //  * CLI 分支早于界面配置加载 ⇒ 这里先**只读**载入一次真实配置（与 api_probe 同口径）
            aiwrite::Config cli_config;
            if (aiwrite::loadConfig(aiwrite::paths::configFile(), cli_config)) {
                aiwrite::setAppConfig(cli_config);
            }
            const int ttl_days = aiwrite::appConfig().credentials.ttl_days;
            const std::vector<aiwrite::utils::CredentialInfo> items =
                aiwrite::utils::listCredentials(ttl_days);
            std::printf("凭据库（%s）：%zu 条（后端 %s / 保留 %d 天）\n",
                        aiwrite::utils::credentialsDir().c_str(), items.size(),
                        aiwrite::appConfig().credentials.backend.c_str(), ttl_days);
            for (const aiwrite::utils::CredentialInfo& info : items) {
                std::printf("  %-28s 更新=%lld 剩余TTL=%d 天\n", info.ref.c_str(),
                            info.updated_at, info.ttl_remaining_days);
            }
            return 0;
        }
        else if (arg == "--cred-erase") {
            if (i + 1 >= argc) {
                std::printf("用法: aiwrite --cred-erase <ref>\n");
                return 2;
            }
            const std::string target = argv[++i];
            std::string       erase_error;
            const bool        erased = aiwrite::utils::eraseCredential(target, &erase_error);
            std::printf("凭据 %s：%s\n", target.c_str(),
                        erased ? "已删除" : erase_error.c_str());
            return erased ? 0 : 1;
        }
        else if (arg == "--export-selftest") {
            const int failed = aiwrite::utils::textExportSelftest();
            std::printf("文本导出自检：%s（失败 %d 项，详见日志）\n", failed == 0 ? "PASS" : "FAIL", failed);
            return failed == 0 ? 0 : 1;
        }
        else if (arg == "--cred-selftest") {
            const int failed = aiwrite::utils::credentialSelftest();
            std::printf("凭据自检：%s（失败 %d 项，详见日志）\n", failed == 0 ? "PASS" : "FAIL", failed);
            return failed == 0 ? 0 : 1;
        }
        else if (arg == "--cred-purge") {
            aiwrite::Config cli_config;
            if (aiwrite::loadConfig(aiwrite::paths::configFile(), cli_config)) {
                aiwrite::setAppConfig(cli_config);
            }
            const int   ttl_days = aiwrite::appConfig().credentials.ttl_days;
            std::string purge_error;
            const int   removed = aiwrite::utils::purgeExpiredCredentials(ttl_days, &purge_error);
            std::printf("已清理过期凭据 %d 个（TTL %d 天，取自 [credentials].ttl_days）\n", removed,
                        ttl_days);
            return 0;
        }
        else if (arg == "--web-probe") {
            web_probe_flag = true;
        }
        else if (arg == "--web-adapter-selftest") {
            dom_adapter_selftest_flag = true;
        }
        else if (arg == "--web-dom-dump") {
            dom_dump_flag = true; // L4（PB2-29）：只读枚举页面候选元素 + 建议选择器
        }
        else if (arg == "--web-supply-probe") {
            supply_probe_flag = true; // M8 Phase 2：只读供料探针（起有头 WebView2 + 人工登录）
        }
        else if (arg == "--upload-selftest") {
            upload_selftest_flag = true; // M8-14：站点上传模板自检（离线三组；带 --provider 追加真机一次）
        }
        else if (arg == "--get") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --get 缺少取值（路径?查询串）\n");
                return 2;
            }
            supply_get_path = argv[++i];
        }
        else if (arg == "--web-session-selftest") {
            web_session_selftest_flag = true;
        }
        else if (arg == "--web-session-list") {
            web_session_list_flag = true; // 批 3（FEA-L02）：会话 / 登录态清单
        }
        else if (arg == "--json") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --json 缺少取值（报告路径）\n");
                return 2;
            }
            report_json_path = argv[++i];
        }
        else if (arg == "--json-diff") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --json-diff 缺少取值（上一轮报告路径）\n");
                return 2;
            }
            report_prev_path = argv[++i];
        }
        else if (arg == "--web-chat") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --web-chat 缺少取值\n");
                return 2;
            }
            web_chat_prompt = argv[++i];
        }
        else if (arg == "--run-selftest") {
            run_selftest_flag = true;
        }
        else if (arg == "--vlm-selftest") {
            vlm_selftest_flag = true;
        }
        else if (arg == "--provider-selftest") {
            provider_selftest_flag = true;
        }
        else if (arg == "--provider-dump") {
            provider_dump_flag = true;
        }
        else if (arg == "--provider") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --provider 缺少取值\n");
                return 2;
            }
            provider_id = argv[++i];
        }
        else if (arg == "--image") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --image 缺少取值\n");
                return 2;
            }
            vlm_image = argv[++i];
        }
        else if (arg == "--api-base") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --api-base 缺少取值\n");
                return 2;
            }
            vlm_api_base = argv[++i];
        }
        else if (arg == "--model") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --model 缺少取值\n");
                return 2;
            }
            vlm_model = argv[++i];
        }
        else if (arg == "--key-ref") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --key-ref 缺少取值\n");
                return 2;
            }
            vlm_key_ref = argv[++i];
        }
        else if (arg == "--web") {
            run_selftest_web = true; // 与 --run-selftest 组合：真实调用网页版生成
        }
        else if (arg == "--official") {
            run_selftest_official = true; // 与 --run-selftest 组合：有 Key 时真实生成一次（PB-09）
        }
        else if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 --timeout 缺少取值\n");
                return 2;
            }
            selftest_timeout = std::atoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    aiwrite::log::init(options.console);
    aiwrite::log::info("AIwrite 启动（M1 技术验证 + 项目骨架）");

    const int dir_failures = aiwrite::paths::ensureDataDirs();
    if (dir_failures != 0) {
        aiwrite::log::warn("有 " + std::to_string(dir_failures) + " 个数据目录创建失败，请检查权限");
    }
    aiwrite::log::info("数据目录: " + aiwrite::paths::dataRoot().string());

    // M7B 批 1 step 2（M7B-02）：命名管道冒烟（起 Python 守护进程 → hello → ready → shutdown）
    if (pipe_selftest_flag) {
        // --timeout 的口径是**秒**（与 --login-selftest 一致）→ 本函数用毫秒
        const int code = pipeSelftest(selftest_timeout * 1000);
        aiwrite::log::info("AIwrite 管道自检退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // M7B 批 2（§6.2 · M7B-19 门槛）：新通道（Pydoll 守护进程）—— 自检 / 冒烟
    if (pydoll_selftest_flag) {
        const int code = aiwrite::web::channel::selftest(30'000);
        aiwrite::log::info("AIwrite 新通道自检退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }
    if (!pydoll_login_id.empty()) {
        const int code = aiwrite::web::channel::pydollLogin(pydoll_login_id, selftest_timeout);
        aiwrite::log::info("AIwrite 新通道登录退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // 网页版会话自动引导回归（窗口已开但内存无凭证 → ensure_session 应自动补探测）
    if (web_session_selftest_flag) {
        const int session_code = webSessionSelftest();
        aiwrite::log::info("AIwrite 会话自检退出，返回码 " + std::to_string(session_code));
        aiwrite::log::shutdown();
        return session_code;
    }

    // 批 3（FEA-L02）：会话 / 登录态清单（**报告命令**：有没有会话都算成功）
    if (web_session_list_flag) {
        const int list_code = webSessionList(report_json_path);
        aiwrite::log::info("AIwrite 会话清单退出，返回码 " + std::to_string(list_code));
        aiwrite::log::shutdown();
        return list_code;
    }

    // 网页版登录自检（不需要 GUI，不进消息循环）
    if (login_selftest) {
        const int selftest_code = aiwrite::web::selftest(selftest_timeout);
        aiwrite::log::info("AIwrite 登录自检退出，返回码 " + std::to_string(selftest_code));
        aiwrite::log::shutdown();
        return selftest_code;
    }

    // L3（PB2-15）+ 批 3（FEA-L01）：选择器探测 / 诊断（只读，不发送内容）
    if (dom_adapter_selftest_flag) {
        int code = 2;
        if (provider_id == "all") {
            // ---- 批量：全部 `adapter=dom` 条目（表驱动；逐站独立）----
            nlohmann::json report;
            code = aiwrite::ai::domProbeAll(selftest_timeout, &report, nullptr);
            if (!report_json_path.empty()) {
                std::string write_error;
                if (writeTextFile(report_json_path, report.dump(2) + "\n", &write_error)) {
                    std::printf("[批量探测] 机器读版已写入：%s\n", report_json_path.c_str());
                }
                else {
                    std::printf("[批量探测] 报告落盘失败：%s\n", write_error.c_str());
                    code = (code == 0) ? 1 : code;
                }
            }
            if (!report_prev_path.empty()) {
                std::string prev_text;
                std::string read_error;
                if (!readTextFile(report_prev_path, &prev_text, &read_error)) {
                    std::printf("[批量探测] 读上一轮报告失败：%s\n", read_error.c_str());
                    code = (code == 0) ? 1 : code;
                }
                else {
                    nlohmann::json prev;
                    bool           parsed = true;
                    try {
                        prev = nlohmann::json::parse(prev_text);
                    }
                    catch (const std::exception& ex) {
                        parsed = false;
                        std::printf("[批量探测] 上一轮报告不是合法 JSON：%s\n", ex.what());
                        code = (code == 0) ? 1 : code;
                    }
                    if (parsed) {
                        std::vector<std::string> lines;
                        std::printf("\n----- 与上一轮报告对比（%s）-----\n",
                                    report_prev_path.c_str());
                        if (aiwrite::ai::dom_report_diff(prev, report, &lines)) {
                            for (const std::string& line : lines) {
                                std::printf("  %s\n", line.c_str());
                            }
                        }
                        else {
                            std::printf("  对比失败：两侧报告的 sites 数组不兼容"
                                        "（如实报错，不假装「无变化」）\n");
                            code = (code == 0) ? 1 : code;
                        }
                    }
                }
            }
            std::printf("\n[批量探测] 退出码 %d"
                        "（0=全部可用 / 3=需登录 / 1=有真失败 / 2=无可测站点）\n",
                        code);
        }
        else if (provider_id.empty()) {
            std::printf("[选择器探测] 请用 --provider <id> 指定要诊断的网页版条目；当前表里的 web 条目：\n");
            for (const aiwrite::ai::ProviderSpec& spec : aiwrite::ai::providerSpecs().items) {
                if (spec.kind != "web") {
                    continue;
                }
                const char* kind_text = (spec.web.adapter == "dom")
                                            ? (aiwrite::ai::webLoginOnly(&spec) ? "dom · 未回填"
                                                                                  : "dom")
                                            : spec.web.adapter.c_str();
                std::printf("   %-16s %-28s %s\n", spec.id.c_str(), spec.display.c_str(), kind_text);
            }
            std::printf("  例：aiwrite.exe --web-adapter-selftest --provider kimi-web\n");
            std::printf("      aiwrite.exe --web-adapter-selftest --provider all --json build\\loop\\probe.json\n");
        }
        else {
            code = aiwrite::ai::domAdapterSelftest(provider_id, selftest_timeout);
        }
        aiwrite::log::info("AIwrite 选择器探测退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // L4（PB2-29 / v15）：选择器**候选枚举**（只读）—— 输出页面候选元素指纹 + 建议选择器
    if (dom_dump_flag) {
        int code = 2;
        if (provider_id.empty()) {
            std::printf("[候选枚举] 请用 --provider <id> 指定网页版条目"
                        "（例：--web-dom-dump --provider kimi-web）\n");
        }
        else {
            code = aiwrite::ai::domSelectorDump(provider_id, selftest_timeout);
        }
        aiwrite::log::info("AIwrite 选择器候选枚举退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // 网页版协议探测（需要 profile 里已有登录态）
    // M8（Phase 2 · Gate）：C++ 供料探针 —— WebView2 会话能否为 C++ 原生 HTTP 供料
    if (supply_probe_flag) {
        const int code = aiwrite::ai::uploadSupplyProbe(provider_id, selftest_timeout, supply_get_path);
        aiwrite::log::info("AIwrite 供料探针退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // M8-14：站点上传模板自检（离线三组；带 --provider 追加真机一次）
    if (upload_selftest_flag) {
        int passed = 0;
        int failed = 0;
        std::printf("=== 站点上传模板自检（M8-14 · 方案 E）===\n");
        std::printf("-- ① 图片准备（格式识别 → 白名单 / 限额 → 缩放 + 转码）--\n");
        int group = 0;
        failed += aiwrite::ai::imageConvertSelftest(&group);
        passed += group;
        std::printf("-- ② 上传双证据判定（I18：无证据不发送）--\n");
        group = 0;
        failed += aiwrite::ai::uploadEvidenceSelftest(&group);
        passed += group;
        std::printf("-- ③ 站点注册表与契约（每站独立实现）--\n");
        group = 0;
        failed += aiwrite::ai::uploadRegistrySelftest(&group);
        passed += group;

        int code = failed == 0 ? 0 : 1;

        if (provider_id.empty()) {
            std::printf("[真机] 未指定 --provider → 跳过真机（示例：--upload-selftest --provider "
                        "doubao-web --image <图片路径>）\n");
        }
        else {
            const aiwrite::ai::ProviderSpec* spec = nullptr;
            for (const aiwrite::ai::ProviderSpec& candidate : aiwrite::ai::providerSpecs().items) {
                if (candidate.id == provider_id) {
                    spec = &candidate;
                    break;
                }
            }
            if (spec == nullptr || spec->kind != "web") {
                std::printf("[真机] ✗ 条目 %s 不是**网页版**条目（或不存在）—— 请用 --provider <web 条目 id>\n",
                            provider_id.c_str());
                code = 2;
            }
            else if (!aiwrite::ai::webSiteError(spec).empty()) {
                std::printf("[真机] ✗ 站点不可用：%s\n", aiwrite::ai::webSiteError(spec).c_str());
                code = 2;
            }
            else {
                aiwrite::ai::UploadPlan plan;
                plan.site        = spec->web;
                plan.provider_id = spec->id;
                if (!vlm_image.empty()) {
                    plan.image_values.push_back(vlm_image);
                }
                plan.max_bytes  = spec->limits.image_max_bytes;
                plan.timeout_ms = selftest_timeout > 0 ? selftest_timeout * 1000 : 180000;
                std::printf("[真机] 站点 %s｜upload_adapter=%s｜attach_selector=%s｜图片 %s\n",
                            spec->id.c_str(),
                            plan.site.upload_adapter.empty() ? "(未声明)"
                                                             : plan.site.upload_adapter.c_str(),
                            plan.site.attach_selector.empty() ? "(用站点单元默认)"
                                                              : plan.site.attach_selector.c_str(),
                            vlm_image.empty() ? "(未给 --image)" : vlm_image.c_str());
                // 接入就绪度（**先把缺什么讲清楚**，再尝试 —— 免得把「缺字段」误读成「上传坏了」）
                std::printf("[真机] 就绪度：adapter=%s｜answer_selector=%s｜生成字段 %s\n",
                            plan.site.adapter.empty() ? "(空=内置默认)" : plan.site.adapter.c_str(),
                            plan.site.answer_selector.empty() ? "(未声明)"
                                                              : plan.site.answer_selector.c_str(),
                            aiwrite::ai::webLoginOnly(spec)
                                ? "未回填（**运行时会自动识别**，M8-37）"
                                : "齐全");
                if (plan.site.adapter != "dom") {
                    std::printf("[真机] ⚠ 该条目 adapter=%s（图片上传当前只支持 dom 站点 + 上传单元）\n",
                                plan.site.adapter.c_str());
                }
                if (aiwrite::ai::webLoginOnly(spec)) {
                    std::printf("[真机] 提示：生成字段未回填**不影响本次运行** —— 发送 / 取答案会"
                                "自动识别选择器（识别结果打印在 Console / 日志里）。"
                                "若要固化为数据（可选）：\n"
                                "         ① aiwrite.exe --web-dom-dump --provider %s"
                                "（候选枚举 → 建议选择器）\n"
                                "         ② aiwrite.exe --web-adapter-selftest --provider %s"
                                "（只读复核命中数；登录后手动发一条消息再跑，回答容器才会出现）\n",
                                provider_id.c_str(), provider_id.c_str());
                }
                if (vlm_image.empty()) {
                    // 只做就绪度检查：**不**发起上传（免得把「没给图片」误读成「上传失败」）
                    std::printf("[真机] 未给 --image → 仅做就绪度检查；真机一次请这样跑："
                                "--upload-selftest --provider %s --image \"D:\\a.png\"\n",
                                provider_id.c_str());
                }
                else {
                    const aiwrite::ai::UploadResult result = aiwrite::ai::uploadImages(plan);
                    for (const std::string& line : result.conversions) {
                        std::printf("[真机] 转换：%s\n", line.c_str());
                    }
                    if (!result.steps.empty()) {
                        std::printf("[真机] 步骤：%s\n", result.steps.c_str());
                    }
                    if (result.ok) {
                        std::printf("[真机] ✓ 双证据齐备 —— 页面：%s｜网络：%s\n",
                                    result.page_evidence.c_str(), result.net_evidence.c_str());
                    }
                    else {
                        std::printf("[真机] ✗ %s\n", result.error.c_str());
                        if (!result.page_evidence.empty() || !result.net_evidence.empty()) {
                            std::printf(
                                "[真机]   已得证据 —— 页面：%s｜网络：%s\n",
                                result.page_evidence.empty() ? "(缺失)"
                                                             : result.page_evidence.c_str(),
                                result.net_evidence.empty() ? "(缺失)"
                                                            : result.net_evidence.c_str());
                        }
                        code = 1;
                    }
                }
            }
        }

        std::printf("=== 站点上传模板自检结果：%d 通过 / %d 失败（返回码 %d）===\n", passed, failed,
                    code);
        aiwrite::log::info("AIwrite 站点上传模板自检退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    if (web_probe_flag) {
        // M_patchB L1 修订（PB2-23）：`--web-probe --provider <id>` = 按**条目**的站点（严格解析，不回落）
        const int probe_code =
            provider_id.empty()
                ? aiwrite::web::protocolProbe(selftest_timeout)
                : aiwrite::web::protocolProbeForProvider(provider_id, selftest_timeout);
        aiwrite::log::info("AIwrite 协议探测退出，返回码 " + std::to_string(probe_code));
        aiwrite::log::shutdown();
        return probe_code;
    }

    // 网页版生成（PoW + SSE；需要 profile 里已有登录态）
    if (!web_chat_prompt.empty()) {
        const int chat_code = webChatSelftest(web_chat_prompt);
        aiwrite::log::info("AIwrite 网页版生成退出，返回码 " + std::to_string(chat_code));
        aiwrite::log::shutdown();
        return chat_code;
    }

    // 图片理解自检（M5-02）：离线请求体断言 + 有 Key 时真实读图生成
    if (vlm_selftest_flag) {
        std::string image = vlm_image;
#ifdef AIWRITE_SOURCE_DIR
        if (image.empty()) {
            image = (std::filesystem::path(AIWRITE_SOURCE_DIR) / "assets" / "images" /
                     "flamingo.png")
                        .string();
        }
#endif
        if (image.empty()) {
            std::fprintf(stderr, "请用 --image <路径> 指定要理解的图片\n");
            return 2;
        }
        const std::string effective_api_base =
            vlm_api_base.empty() ? std::string("https://open.bigmodel.cn/api/paas/v4")
                                 : vlm_api_base;
        const std::string effective_model =
            vlm_model.empty() ? std::string("glm-4v-flash") : vlm_model;
        const std::string effective_key_ref =
            vlm_key_ref.empty() ? std::string("brain-ai/zhipu") : vlm_key_ref;
        const int vlm_code = vlm_selftest(image, effective_api_base, effective_model,
                                          effective_key_ref);
        aiwrite::log::info("AIwrite 图片理解自检退出，返回码 " + std::to_string(vlm_code));
        aiwrite::log::shutdown();
        return vlm_code;
    }

    // Provider 配置表（M_patchB L1）：打印生效表 / 表校验 + 离线断言（+ 可选联网 ping）
    if (provider_dump_flag) {
        const int dump_code = providerDump();
        aiwrite::log::shutdown();
        return dump_code;
    }
    if (provider_selftest_flag) {
        const int code = provider_selftest(provider_id, vlm_api_base, vlm_model, vlm_key_ref);
        aiwrite::log::info("AIwrite Provider 自检退出，返回码 " + std::to_string(code));
        aiwrite::log::shutdown();
        return code;
    }

    // 执行自检（不需要 GUI）：验证 EditorState 的运行接线
    if (run_selftest_flag) {
        // M_patchAB §2.4（PB-09）：`--run-selftest --official` = 有 Key 时**真实生成一次**
        // （与 `--run-selftest` / `--run-selftest --web` 并列；缺 Key ⇒ 2，语义同其余自检）
        const int selftest_code = run_selftest_official && !run_selftest_web
                                      ? officialGenerateSelftest(provider_id)
                                      : runSelftest(run_selftest_web, provider_id);
        aiwrite::log::info("AIwrite 运行自检退出，返回码 " + std::to_string(selftest_code));
        aiwrite::log::shutdown();
        return selftest_code;
    }

    const int exit_code = aiwrite::ui::run(options);

    // 退出前收尾：后台诊断线程（`PB2-07` / `PB2-30②`）→「生成提示词」（L1-d）→ 再关网页版窗口 → 最后关日志
    //  * 顺序理由：诊断 / 生成可能正在用站点窗口 / 写日志 ⇒ 先等它结束（超时则分离并如实记日志）
    aiwrite::ui::stopDiagnostics();
    aiwrite::ui::stopPromptGeneration();

    // 若用户留着网页版登录窗口就退出了程序：先收尾（关窗口、等线程结束），再关日志
    aiwrite::web::stopLoginWindow();

    aiwrite::log::info("AIwrite 退出，返回码 " + std::to_string(exit_code));
    aiwrite::log::shutdown();
    return exit_code;
}
