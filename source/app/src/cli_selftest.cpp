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
#include "readonly_view.h"   // ARCH-R3：ReadOnlyView 渲染视图断言（零拷贝 / 同源）
#include "commands.h"        // ARCH-R4：命令路由层断言（快捷键表完整性 / 唯一性）
#include "event_bus.h"       // ARCH-E1：编辑事件断言（EventBus 机制 + 8 种类型 + 操作→事件）
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
//  CLI 自检域（CLN-14 拆分）—— 定义见 ui/include/cli_internal.h
//  * 原 main.cpp 匿名块函数提升为全局命名空间（跨 TU 与 cli_diag.cpp / main.cpp 共享）
// ============================================================================

std::size_t utf8_length(const std::string& text)
{
    std::size_t count = 0;
    for (const char raw : text) {
        if ((static_cast<unsigned char>(raw) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string utf8_prefix(const std::string& text, std::size_t max_chars)
{
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            continue; // 续字节：仍在同一个码点内
        }
        if (chars == max_chars) {
            return text.substr(0, i);
        }
        ++chars;
    }
    return text;
}

constexpr std::size_t kVa07ProbeChars = 24;

int officialGenerateSelftest(const std::string& provider_id)
{
    const aiwrite::ai::ProviderSpecs specs = aiwrite::ai::providerSpecs();

    // 条目：显式 `--provider <id>` 优先，否则第一个 official 条目
    std::string entry_id = provider_id;
    if (entry_id.empty()) {
        const std::vector<std::string> ids = specs.ids("official");
        entry_id                           = ids.empty() ? std::string() : ids.front();
    }
    if (entry_id.empty()) {
        std::printf("[官方 API 自检] ✗ 配置表里没有 official 条目\n");
        return 2;
    }
    const aiwrite::ai::ProviderSpec* spec = specs.find(entry_id);
    if (spec == nullptr) {
        std::printf("[官方 API 自检] ✗ 表里没有 id=%s（用 --provider-dump 查看全部）\n", entry_id.c_str());
        return 2;
    }
    if (spec->kind != "official") {
        std::printf("[官方 API 自检] ✗ 条目 %s 不是 official 条目（kind=%s）\n", entry_id.c_str(),
                    spec->kind.c_str());
        return 2;
    }
    if (spec->protocol != "openai") {
        std::printf("[官方 API 自检] ✗ 条目 %s 的 protocol=%s 本版本尚未实现（已实现：%s）\n",
                    entry_id.c_str(), spec->protocol.c_str(),
                    joinNames(aiwrite::ai::implementedProtocols()).c_str());
        return 2;
    }

    const std::string api_base = aiwrite::ai::resolveApiBase(*spec, std::string());
    const std::string model    = aiwrite::ai::resolve_model(*spec, std::string(), std::string(), false);
    const std::string key_ref  = aiwrite::ai::resolveKeyRef(*spec, std::string());
    if (api_base.empty()) {
        std::printf("[官方 API 自检] ✗ 条目 %s 未填 api_base：请用 --api-base 指定，或在 providers.json 补齐\n",
                    entry_id.c_str());
        return 2;
    }

    // Key：环境变量（表内 env 名按序）→ 凭据库（与节点运行期同序，PB-06）
    std::string       key;
    std::string       key_source;
    for (const std::string& env_name : spec->env_names) {
        const char* value = std::getenv(env_name.c_str());
        if (value != nullptr && *value != '\0') {
            key        = value;
            key_source = "环境变量 " + env_name;
            break;
        }
    }
    if (key.empty() && !key_ref.empty()) {
        std::string       load_error;
        const std::string stored = aiwrite::utils::loadCredential(key_ref, &load_error);
        if (!stored.empty()) {
            key        = stored;
            key_source = "凭据库 " + key_ref;
        }
    }
    if (key.empty()) {
        std::printf("[官方 API 自检] ✗ 未找到 API Key（环境变量 %s / 凭据库 %s）\n"
                    "  提示：在界面「提供商配置 → API Key」填一次即入库（PB-06）\n",
                    joinNames(spec->env_names).c_str(), key_ref.c_str());
        return 2;
    }

    aiwrite::ai::OfficialChatRequest request;
    request.options         = aiwrite::ai::providerOptionsFrom(*spec);
    request.api_base        = api_base;
    request.options.api_base = api_base;
    request.api_key         = key;
    request.model           = model;
    request.prompt          = "请只回复 pong";
    request.temperature     = 0.0;
    request.max_tokens      = 16;
    std::printf("[官方 API 自检] 条目=%s · 地址=%s · 模型=%s · 凭据来源=%s（脱敏：不打印 Key）\n",
                entry_id.c_str(), api_base.c_str(), model.c_str(), key_source.c_str());
    std::printf("[官方 API 自检] 发一次真实生成（「请只回复 pong」，消耗极小额度）…\n");

    const aiwrite::ai::OfficialChatResult result = aiwrite::ai::officialChat(request);
    std::printf("[官方 API 自检] HTTP %d · 耗时 %lld ms · 取消=%s\n", result.http_status,
                static_cast<long long>(result.elapsed_ms), result.cancelled ? "是" : "否");
    if (!result.ok || result.text.empty()) {
        std::printf("[官方 API 自检] FAIL：%s\n",
                    result.error.empty() ? "未取到文本" : result.error.c_str());
        return 1;
    }
    std::string reply = result.text;
    if (reply.size() > 120) {
        reply.resize(120);
        reply += "…";
    }
    std::printf("[官方 API 自检] PASS：回复=[%s]\n", reply.c_str());
    return 0;
}

int runSelftest(bool use_web, const std::string& provider_id)
{
    aiwrite::ui::EditorState& state = aiwrite::ui::editor();
    state.createSampleWorkflow();

    // 默认（自检不联网）：把生成相关节点的 mode 置为 official（官方 API 占位分支）
    // --run-selftest --web：置为 web，走真实网页版生成
    // 注意：LLMGenerate 的模式取「provider 端口（ProviderConfig）」优先，故两处都要设
    // M_patchB L1 修订（PB2-22 / 决策 D-22②）：站点**不回落** → `--web` 必须显式选**网页版条目**
    std::string web_entry = provider_id;
    if (use_web && web_entry.empty()) {
        const std::vector<std::string> web_entry_ids = aiwrite::ai::providerSpecs().ids("web");
        web_entry = web_entry_ids.empty() ? std::string() : web_entry_ids.front();
    }
    if (use_web) {
        // PB2-30①：显式指定的条目必须真的存在且是网页版条目（严格解析，**不**回落）
        const aiwrite::ai::ProviderSpec* spec = aiwrite::ai::providerSpecs().find(web_entry);
        if (web_entry.empty()) {
            std::printf("[运行自检] ✗ 配置表里没有网页版条目（kind=web）：无法执行 --web 自检\n");
            return 1;
        }
        if (spec == nullptr) {
            std::printf("[运行自检] ✗ 表里没有 id=%s（用 --provider-dump 查看全部）\n", web_entry.c_str());
            return 2;
        }
        if (spec->kind != "web") {
            std::printf("[运行自检] ✗ 条目 %s 不是网页版条目（kind=%s）：--web 自检需要 kind=web"
                        "（决策 D-22②，站点不回落）\n",
                        web_entry.c_str(), spec->kind.c_str());
            return 2;
        }
        if (aiwrite::ai::webSiteError(spec) != std::string()) {
            std::printf("[运行自检] ✗ 条目 %s 站点不可用：%s\n", web_entry.c_str(),
                        aiwrite::ai::webSiteError(spec).c_str());
            return 2;
        }
        if (aiwrite::ai::webLoginOnly(spec)) {
            // M8-37（零开关）：生成字段未回填**不再判定为失败** —— 运行期会自动识别选择器；
            //  这里只如实提示（原「登录型条目 → 返回 1」的行为已被自动识别取代）
            std::printf("[运行自检] ⚠ 条目 %s 的生成字段未回填（web.input_selector / send / "
                        "answer_selector）：本次运行将**自动识别**（结果打印在下方）；"
                        "如需固化可用 --web-dom-dump --provider %s 取建议选择器\n",
                        web_entry.c_str(), web_entry.c_str());
        }
    }
    for (aiwrite::engine::Node& node : state.graph.nodes) {
        if (use_web && node.type == "ProviderConfig") {
            if (aiwrite::engine::Param* provider = node.findParam("provider")) {
                provider->value = web_entry; // 决策 D-22②：站点不回落，必须选网页版条目
            }
        }
        if (node.type != "LLMGenerate" && node.type != "ProviderConfig") {
            continue;
        }
        if (aiwrite::engine::Param* mode = node.findParam("mode")) {
            mode->value = std::string(use_web ? "web" : "official");
        }
    }
    std::printf("[运行自检] 模式：%s\n", use_web ? "web（网页版真实生成）" : "official（离线占位）");
    if (use_web) {
        std::printf("[运行自检] 网页版条目：%s（站点取自该条目，不回落）\n", web_entry.c_str());
    }

    std::printf("[运行自检] 示例工作流：节点 %zu，连线 %zu\n", state.graph.nodes.size(),
                state.graph.edges.size());
    // PC-05：本次自检把运行结果归档到临时目录（不污染用户 outputs），便于断言
    {
        aiwrite::Config selftest_config         = aiwrite::appConfig();
        const std::filesystem::path archive_root =
            std::filesystem::temp_directory_path() / "aiwrite_run_selftest_outputs";
        std::error_code cleanup_ec;
        std::filesystem::remove_all(archive_root, cleanup_ec);
        selftest_config.output.archive_dir  = archive_root.string();
        selftest_config.output.keep_history = false;
        selftest_config.output.max_history  = 1;
        selftest_config.output.ttl_days     = 0;
        aiwrite::setAppConfig(selftest_config);
    }
    // VA-07 判据②的「运行前基线」：与 save_workflow 落盘形式逐字节同源
    // （engine/workflow_io.cpp： graph_to_json(graph).dump(2) + "\n"）。
    // 用途：网页版答案常以工作流自身的种子文本开头 ⇒「答案前缀命中 JSON」可能只是
    //       模型**合法复述参数值**；只有「命中处不在运行前基线里」才算运行结果泄漏。
    const std::string json_before = aiwrite::engine::graphToJson(state.graph).dump(2) + "\n";

    if (!state.startRun()) {
        std::printf("[运行自检] FAIL：start_run 被拒绝（%s）\n", state.status.c_str());
        return 1;
    }

    int guard = 0;
    while (state.executor().running() && guard++ < 4096) {
        state.tickRun();
    }

    std::printf("[运行自检] 状态栏文本：%s\n", state.runStatusText().c_str());
    for (const aiwrite::engine::Node& node : state.graph.nodes) {
        std::string detail;
        if (!node.error_message.empty()) {
            detail = "（" + node.error_message + "）";
        }
        std::printf("[运行自检] %-3s %-15s %-8s %s\n", node.id.c_str(), node.type.c_str(),
                    aiwrite::engine::nodeStateName(node.state), detail.c_str());
    }
    std::printf("[运行自检] %s\n", state.executor().summary().c_str());

    const bool finished = state.executor().state() == aiwrite::engine::ExecState::Finished;

    // ---- PA-02：输出面板数据通道（与面板「复制全文 / 导出」同一实现）----
    const std::string output_text = aiwrite::ui::runOutputText(state.graph, state.executor());
    std::printf("[运行自检] 输出面板文本：%zu 字符\n", output_text.size());
    if (!output_text.empty()) {
        const std::size_t head = output_text.size() > 100 ? 100 : output_text.size();
        std::printf("[运行自检] 输出面板预览：%s…\n", output_text.substr(0, head).c_str());
    }

    // VA-07 取样：LLMGenerate 节点的运行结果（跑完不得出现在工作流 JSON 里）
    std::string llm_result;
    for (const aiwrite::engine::Node& node : state.graph.nodes) {
        if (node.type == "LLMGenerate") {
            llm_result = aiwrite::ui::node_output_text(state.graph, state.executor(), node.id);
            break;
        }
    }

    // ---- M2-05：工作流文件往返（保存 → 打开，走 app 侧同一代码路径）----
    // 备份/还原 recent.json，避免自检污染真实最近列表
    const std::filesystem::path recentFile = aiwrite::paths::recentFile();
    std::string                 recent_backup;
    std::error_code             ec;
    const bool                  had_recent = std::filesystem::exists(recentFile, ec);
    if (had_recent) {
        std::ifstream     in(recentFile, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        recent_backup = buffer.str();
    }

    const std::filesystem::path temp_file =
        std::filesystem::temp_directory_path() / "aiwrite_run_selftest_workflow.json";
    std::string file_error;
    const bool  saved      = state.saveWorkflowTo(temp_file.string(), &file_error);
    const std::size_t nodes_before = state.graph.nodes.size();
    const bool  opened     = saved && state.openWorkflowFrom(temp_file.string(), &file_error);
    const bool  same       = opened && state.graph.nodes.size() == nodes_before;
    std::printf("[运行自检] 工作流文件往返：保存=%s / 打开=%s / 节点数一致=%s%s\n",
                saved ? "OK" : "失败", opened ? "OK" : "失败", same ? "OK" : "不一致",
                file_error.empty() ? "" : ("（" + file_error + "）").c_str());

    // ---- PC-05：运行结果已自动归档（每节点 .txt + run.json）----
    bool        archive_ok = false;
    std::size_t archive_files = 0;
    bool        doc_found  = false;
    if (!state.last_archive_dir.empty()) {
        const std::filesystem::path dir(state.last_archive_dir);
        archive_ok = std::filesystem::exists(dir / "run.json");
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".txt") {
                continue;
            }
            ++archive_files;
            std::ifstream     in(entry.path(), std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            const std::string content    = buffer.str();
            // 归档是**全文**落盘 ⇒ 这里只做「含生成文档」的存在性判断，
            // 故沿用字节前缀即可（VA-07 的纯净判据见下方，那条必须按码点取样）。
            const std::size_t probe_len  = llm_result.size() < 24 ? llm_result.size() : 24;
            if (!content.empty() &&
                (llm_result.empty() ||
                 content.find(llm_result.substr(0, probe_len)) != std::string::npos)) {
                doc_found = true; // 归档里确实含生成文档（离线模式下 LLM 失败，退化为"有内容"）
            }
        }
    }
    std::printf("[运行自检] PC-05 归档：%s（目录 %s，节点文件 %zu 个，含生成文档=%s）\n",
                archive_ok && doc_found ? "OK" : "失败",
                state.last_archive_dir.empty() ? "（无）" : state.last_archive_dir.c_str(),
                archive_files, doc_found ? "是" : "否");

    // VA-07：保存出来的 JSON 不得包含运行结果（运行态值原则）
    // 判据（TST-L01 缺陷修复 —— 原实现按**字节**取样，见 anonymous namespace 的说明）：
    //   ① 取样 = 答案前 kVa07ProbeChars 个 **UTF-8 码点**（不再按 24 字节截断）
    //   ② 命中但**不算**泄漏：该取样在「运行前 JSON 基线」里就已存在
    //      ⇒ 命中的是工作流自身参数值（模型合法复述种子文本），不是运行结果被写回
    //   ③ 已知盲区（如实登记）：若运行结果前缀与某个运行前参数值**逐字相同**，
    //      两者无法区分（属于不可观测差异）；由 VA-01 三处一致 + 人工复核兜底。
    bool        json_clean      = true;
    bool        json_static_hit = false;
    std::size_t json_chars      = 0;
    std::size_t baseline_chars  = 0;
    std::size_t probe_chars     = 0;
    std::size_t probe_bytes     = 0;
    std::size_t hit_index       = std::string::npos;
    std::string probe;
    if (saved && !llm_result.empty()) {
        std::ifstream     json_in(temp_file, std::ios::binary);
        std::stringstream json_buffer;
        json_buffer << json_in.rdbuf();
        const std::string json = json_buffer.str();
        json_chars          = json.size();
        baseline_chars      = json_before.size();
        probe               = utf8_prefix(llm_result, kVa07ProbeChars);
        probe_chars         = utf8_length(probe);
        probe_bytes         = probe.size();
        hit_index           = json.find(probe);
        json_static_hit     = (hit_index != std::string::npos) &&
                              json_before.find(probe) != std::string::npos;
        json_clean = (hit_index == std::string::npos) || json_static_hit;
    }
    std::printf("[运行自检] VA-07 工作流 JSON 不含运行结果：%s\n", json_clean ? "OK" : "失败");
    {
        // 诊断：命中/未命中都要能复盘（取样口径 = 答案前 N 个**码点**；转义换行/引号便于比对）
        std::string head;
        for (const char c : probe) {
            if (c == '\n') {
                head += "\\n";
            }
            else if (c == '\r') {
                head += "\\r";
            }
            else if (c == '"') {
                head += "\\\"";
            }
            else {
                head += c;
            }
        }
        const std::string hit =
            (hit_index == std::string::npos) ? std::string("未命中") : std::to_string(hit_index);
        std::printf("[运行自检] VA-07 诊断：llm_result=%zu 字节/%zu 码点 / 取样=%zu 码点（%zu 字节）/ "
                    "json=%zu 字节（运行前基线 %zu 字节）/ 命中位置=%s / 命中运行前内容=%s / "
                    "取样文本=[%s]\n",
                    llm_result.size(), utf8_length(llm_result), probe_chars, probe_bytes, json_chars,
                    baseline_chars, hit.c_str(), json_static_hit ? "是" : "否", head.c_str());
    }

    std::filesystem::remove(temp_file, ec);
    if (had_recent) {
        std::ofstream out(recentFile, std::ios::binary | std::ios::trunc);
        out << recent_backup;
    }
    else {
        std::filesystem::remove(recentFile, ec);
    }

    // ---- VA-01（PA-03）：三处呈现同源（画布节点摘要 / 参数面板结果区 / 输出面板全文）----
    bool consistency_ok = false;
    {
        const std::string all = aiwrite::ui::runOutputText(state.graph, state.executor());
        std::size_t       compared  = 0;
        bool              contained = true;
        for (const aiwrite::engine::NodeRunInfo& info : state.executor().runInfos()) {
            const std::string per_node =
                aiwrite::ui::node_output_text(state.graph, state.executor(), info.node_id);
            if (per_node.empty()) {
                continue;
            }
            ++compared;
            if (all.find(per_node) == std::string::npos) {
                contained = false; // 输出面板全文必须包含每个节点全文
            }
        }
        consistency_ok = contained && compared > 0;
        std::printf("[运行自检] VA-01 三处一致：%s（比对 %zu 个有输出的节点，全文 %zu 字符）\n",
                    consistency_ok ? "OK" : "失败", compared, all.size());
    }

    // ---- P1-c：参数驱动（"无硬编码"断言）----
    // 改「文本输入」的「文本内容」参数（等价于在参数面板输入框里打字）后重新运行，
    // 断言：① 该节点运行输出 == 新文本（且与上次不同）② 下游「提示词模板」输出包含新文本
    // （即：真正发给 AI 的提示词来自输入框，而不是任何写死的示例文本）
    bool param_driven_ok = false;
    {
        const std::string      marker     = "P1C 参数驱动：这段文本来自输入框。";
        aiwrite::engine::Node* input_node = nullptr;
        aiwrite::engine::Node* tpl_node   = nullptr;
        for (aiwrite::engine::Node& node : state.graph.nodes) {
            if (input_node == nullptr && node.type == "TextInput") {
                input_node = &node;
            }
            if (tpl_node == nullptr && node.type == "PromptTemplate") {
                tpl_node = &node;
            }
        }
        aiwrite::engine::Param* text_param =
            (input_node != nullptr) ? input_node->findParam("text") : nullptr;
        if (input_node != nullptr && tpl_node != nullptr && text_param != nullptr) {
            const std::string before =
                aiwrite::ui::node_output_text(state.graph, state.executor(), input_node->id);
            text_param->value = marker; // 模拟"参数面板输入框里改写文本内容"
            state.startRun();
            int guard2 = 0;
            while (state.executor().running() && guard2++ < 4096) {
                state.tickRun();
            }
            const std::string after  = aiwrite::ui::node_output_text(state.graph, state.executor(),
                                                                     input_node->id);
            const std::string prompt = aiwrite::ui::node_output_text(state.graph, state.executor(),
                                                                     tpl_node->id);
            const bool        changed = (after == marker) && (after != before);
            const bool        flowed  = prompt.find(marker) != std::string::npos;
            param_driven_ok           = changed && flowed;
            std::printf("[运行自检] P1-c 参数驱动（无硬编码）：%s（节点输出随参数变化=%s / "
                        "下游提示词含新文本=%s）\n",
                        param_driven_ok ? "OK" : "失败", changed ? "是" : "否", flowed ? "是" : "否");
        }
        else {
            std::printf("[运行自检] P1-c 参数驱动（无硬编码）：跳过（图中缺少 TextInput / "
                        "PromptTemplate 节点）\n");
            param_driven_ok = true;
        }
    }

    // ---- PB-01：线程化运行（事件队列 + Graph 不被工作线程写 + 快照同源）----
    bool async_ok = false;
    if (!use_web) {
        // 归位到 Idle（上一轮运行把状态写进了 Graph；异步运行只应改副本）
        for (aiwrite::engine::Node& node : state.graph.nodes) {
            node.state = aiwrite::engine::NodeState::Idle;
            node.error_message.clear();
        }

        std::string start_error;
        const bool  started = state.executor().startAsync(&state.graph, &start_error);

        std::vector<aiwrite::engine::RunEvent> events;
        int                                    guard3 = 0;
        while (state.executor().state() == aiwrite::engine::ExecState::Running && guard3++ < 20000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            state.executor().pumpEvents(&events);
        }
        // 【API 契约】先 join 再取最后一轮事件：会话结束的那一刻 state_ 已翻转为非 Running，
        // 而 worker 的 Finished 事件可能尚未入队 —— 必须先 stopAsync()（join）再 pumpEvents()
        state.executor().stopAsync();
        state.executor().pumpEvents(&events);

        std::size_t state_events = 0;
        std::size_t output_events = 0;
        std::size_t console_events = 0;
        std::size_t finish_events = 0;
        std::size_t delta_events  = 0;
        for (const aiwrite::engine::RunEvent& event : events) {
            switch (event.kind) {
            case aiwrite::engine::RunEvent::Kind::NodeState: ++state_events; break;
            case aiwrite::engine::RunEvent::Kind::NodeOutput: ++output_events; break;
            case aiwrite::engine::RunEvent::Kind::Console: ++console_events; break;
            case aiwrite::engine::RunEvent::Kind::Delta: ++delta_events; break;
            case aiwrite::engine::RunEvent::Kind::Finished: ++finish_events; break;
            }
        }

        // ① 主线程尚未应用事件 → Graph 仍是 Idle（证明工作线程只改自己的副本）
        bool graph_untouched = true;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (node.state != aiwrite::engine::NodeState::Idle) {
                graph_untouched = false;
                break;
            }
        }

        // ② 快照 == 权威数据（runInfos / outputs）
        const aiwrite::engine::RunSnapshot snapshot =
            aiwrite::engine::makeSnapshot(state.graph, state.executor());
        bool same_view = snapshot.nodes.size() == state.executor().runInfos().size();
        for (const aiwrite::engine::NodeRunInfo& info : state.executor().runInfos()) {
            const aiwrite::engine::RunNodeView* view = snapshot.find(info.node_id);
            if (view == nullptr || view->state != info.state || view->type != info.type ||
                std::fabs(view->duration_ms - info.duration_ms) > 0.0001) {
                same_view = false;
                break;
            }
        }

        bool text_same = true;
        for (const aiwrite::engine::NodeRunInfo& info : state.executor().runInfos()) {
            const std::string authoritative =
                aiwrite::engine::nodeOutputText(state.graph, state.executor().outputs(), info.node_id);
            const aiwrite::engine::RunNodeView* view = snapshot.find(info.node_id);
            const std::string from_snapshot = (view != nullptr) ? view->text : std::string();
            if (authoritative != from_snapshot) {
                text_same = false;
                break;
            }
        }

        async_ok = started && finish_events == 1 && state_events > 0 && output_events > 0 &&
                   console_events > 0 && graph_untouched && same_view && text_same &&
                   state.executor().state() != aiwrite::engine::ExecState::Running;
        std::printf("[运行自检] PB-01 线程化：%s（启动=%s / 状态事件 %zu / 输出事件 %zu / Console %zu / "
                    "增量事件 %zu / Finished %zu / Graph 未被工作线程写=%s / 快照一致=%s / 文本一致=%s）\n",
                    async_ok ? "OK" : "失败", started ? "是" : "否", state_events, output_events,
                    console_events, delta_events, finish_events, graph_untouched ? "是" : "否",
                    same_view ? "是" : "否", text_same ? "是" : "否");
    }
    else {
        std::printf("[运行自检] PB-01 线程化：跳过（--web 模式避免二次联网；该断言在离线模式验证）\n");
        async_ok = true;
    }

    // ---- PB-01 第二步：EditorState 异步入口（start_run_async + tick_run 泵事件）----
    bool session_ok = true;
    if (!use_web) {
        for (aiwrite::engine::Node& node : state.graph.nodes) {
            node.state = aiwrite::engine::NodeState::Idle;
            node.error_message.clear();
        }

        const bool started = state.startRunAsync();
        int        guard4  = 0;
        while (state.session_active() && guard4++ < 40000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            state.tickRun(); // 主线程：pump_run_events（写 Graph + 维护 run_snapshot）
        }
        const bool session_ended = !state.session_active();
        // ARCH-R2：EditorState 构造时订阅 RunEvent —— startRunAsync 重置通知，Finished 事件置位
        //  * 走到这里 run_finished_ == true 证明「订阅 → 泵事件 → 回调」链路工作（不再主动 pump）
        const bool subscribed_notify = state.run_finished_;

        // 读模型 == 权威数据（状态 / 类型 / 文本），且 Graph 节点状态已由事件应用到位
        const aiwrite::engine::RunSnapshot& view = state.run_snapshot_view();
        bool same_view = view.nodes.size() == state.executor().runInfos().size();
        for (const aiwrite::engine::NodeRunInfo& info : state.executor().runInfos()) {
            const aiwrite::engine::RunNodeView* item = view.find(info.node_id);
            const aiwrite::engine::Node*        node = state.graph.findNode(info.node_id);
            if (item == nullptr || node == nullptr || item->state != info.state ||
                node->state != info.state ||
                item->text != aiwrite::engine::nodeOutputText(state.graph, state.executor().outputs(),
                                                              info.node_id)) {
                same_view = false;
                break;
            }
        }
        state.stopRunAsync(); // 幂等（收尾已 join）
        session_ok = started && session_ended && same_view && subscribed_notify &&
                     state.executor().state() != aiwrite::engine::ExecState::Running;
        std::printf("[运行自检] PB-01 第二步（EditorState 异步 + RunSnapshot）：%s（启动=%s / 会话结束=%s / "
                    "快照·Graph == 权威=%s / 订阅通知=%s / 节点 %zu 个 / 状态 %s）\n",
                    session_ok ? "OK" : "失败", started ? "是" : "否", session_ended ? "是" : "否",
                    same_view ? "是" : "否", subscribed_notify ? "是" : "否", view.nodes.size(),
                    aiwrite::engine::execStateName(state.executor().state()));
    }
    else {
        std::printf("[运行自检] PB-01 第二步：跳过（--web 模式避免二次联网）\n");
    }

    // ---- M_textio P5：汇点节点（TextOutput）的运行态值可读（回归断言）----
    bool sink_ok = false;
    {
        std::string output_id;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (node.type == "TextOutput") {
                output_id = node.id;
            }
        }
        std::string source_id;
        if (!output_id.empty()) {
            if (const aiwrite::engine::Edge* edge = state.graph.findEdgeIntoInput(output_id, "text")) {
                source_id = edge->from_node;
            }
        }
        std::string sink_text;
        std::string source_text;
        bool sink_same = false;
        if (!output_id.empty() && !source_id.empty()) {
            sink_text   = aiwrite::engine::nodeOutputText(state.graph, state.executor().outputs(), output_id);
            source_text = aiwrite::engine::nodeOutputText(state.graph, state.executor().outputs(), source_id);
            const aiwrite::engine::RunSnapshot snap = aiwrite::engine::makeSnapshot(state.graph, state.executor());
            const aiwrite::engine::RunNodeView* view = snap.find(output_id);
            sink_same = (sink_text == source_text) && view != nullptr && view->text == source_text;
        }
        sink_ok = sink_same;
        std::printf("[运行自检] M_textio 汇点结果可读：%s（TextOutput=%s / 上游=%s / 汇点 %zu 字符 / 上游 %zu 字符）\n",
                    sink_ok ? "OK" : "失败", output_id.c_str(), source_id.c_str(), sink_text.size(),
                    source_text.size());
    }

    // ---- 参数范围安全断言（防“Slider 断言崩溃”回归）----
    //  ImGui 的 Slider* 只接受 half-range（imgui_widgets.cpp SliderBehavior：
    //  IM_ASSERT(p_max <= T_MAX / 2)）；参数定义越界会让控件一渲染就 abort。
    bool param_range_ok = true;
    {
        std::size_t checked   = 0;
        std::string first_bad = "";
        for (const aiwrite::engine::Definition* definition :
             aiwrite::engine::NodeRegistry::instance().listTypes()) {
            for (const aiwrite::engine::Param& param : definition->params) {
                if (!param.min_value.has_value() || !param.max_value.has_value()) {
                    continue;
                }
                ++checked;
                const double lo    = *param.min_value;
                const double hi    = *param.max_value;
                const double limit = (param.type == aiwrite::engine::ParamType::Int)
                                         ? 1073741823.0
                                         : 1.7e38; // IM_S32_MAX/2 与 ≈FLT_MAX/2
                const bool bad = !(lo <= hi) || lo < -limit || hi > limit;
                if (bad) {
                    param_range_ok = false;
                    if (first_bad.empty()) {
                        first_bad = definition->type + "." + param.id;
                        std::printf("[运行自检]   越界参数：%s min=%g max=%g（安全上限 ±%g）\n",
                                    first_bad.c_str(), lo, hi, limit);
                    }
                }
            }
        }
        std::printf("[运行自检] 参数范围安全（ImGui Slider 上限）：%s（检查 %zu 个带范围参数）\n",
                    param_range_ok ? "OK" : "失败", checked);
    }

    // ---- M_rerun：文本生成「重新生成（新 seed）」断言（离线）----
    bool seed_ok = false;
    {
        aiwrite::ai::OfficialChatRequest request;
        request.api_key = "sk-selftest";
        request.prompt  = "hi";
        request.model   = "deepseek-chat";
        request.seed    = 12345;
        const nlohmann::json with_seed = aiwrite::ai::buildRequestBody(request);
        request.seed                   = 0;
        const nlohmann::json without_seed = aiwrite::ai::buildRequestBody(request);
        bool has_param = false;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (node.type != "LLMGenerate") {
                continue;
            }
            if (const aiwrite::engine::Param* seed_param = node.findParam("seed")) {
                has_param = (seed_param->number(-1.0) == 0.0);
            }
        }
        seed_ok = with_seed.contains("seed") && with_seed["seed"].get<int>() == 12345 &&
                  !without_seed.contains("seed") && has_param;
        std::printf("[运行自检] M_rerun 新 seed：%s（含 seed=%s / 不含 seed=%s / 节点含 seed 参数=%s）\n",
                    seed_ok ? "OK" : "失败", with_seed.contains("seed") ? "是" : "否",
                    without_seed.contains("seed") ? "是" : "否", has_param ? "是" : "否");
    }

    // ---- F1（M3-04 重做）：复制/粘贴的模型级断言（先断言、后接线 UI）----
    // 说明：画布层守卫见 node_canvas.cpp（clamp_position / 未知手柄反向推送 / 视图跟随前校验），
    //       这里覆盖模型侧：新 id 唯一、位置偏移且有限、参数复制、选区内部连线重映射、可撤销。
    bool paste_ok = false;
    {
        // 注意：paste_clipboard() 内部会压撤销快照（Graph 重新分配）→ 源节点必须**按值**捕获，
        //       不能持有 Node* 指针（否则断言读到悬空内存，误报失败）。
        struct Source {
            std::string id;
            std::string type;
            float       x = 0.0f;
            float       y = 0.0f;
            std::vector<std::pair<std::string, nlohmann::json>> params;
        };
        std::vector<Source> sources;
        // 优先取「存在连线」的一对节点（覆盖"选区内部连线被一并粘贴"这条路径）
        std::vector<std::string> prefer;
        for (const aiwrite::engine::Edge& edge : state.graph.edges) {
            const aiwrite::engine::Node* from = state.graph.findNode(edge.from_node);
            const aiwrite::engine::Node* to   = state.graph.findNode(edge.to_node);
            if (from != nullptr && to != nullptr && from->type != "ProviderConfig" &&
                to->type != "ProviderConfig") {
                prefer = {edge.from_node, edge.to_node};
                break;
            }
        }

        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (sources.size() >= 2) {
                break;
            }
            const bool wanted = !prefer.empty()
                                    ? (node.id == prefer[0] || node.id == prefer[1])
                                    : (node.type == "TextInput" || node.type == "PromptTemplate");
            if (!wanted) {
                continue;
            }
            Source source;
            source.id   = node.id;
            source.type = node.type;
            source.x    = node.x;
            source.y    = node.y;
            for (const aiwrite::engine::Param& param : node.params) {
                source.params.emplace_back(param.id, param.value);
            }
            sources.push_back(std::move(source));
        }

        const auto in_selection = [&sources](const std::string& id) {
            for (const Source& source : sources) {
                if (source.id == id) {
                    return true;
                }
            }
            return false;
        };

        // 选区**内部**连线数量（这些应被一并粘贴）
        std::size_t internal_edges = 0;
        for (const aiwrite::engine::Edge& edge : state.graph.edges) {
            if (in_selection(edge.from_node) && in_selection(edge.to_node)) {
                ++internal_edges;
            }
        }

        std::set<std::string> ids_before;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            ids_before.insert(node.id);
        }
        const std::size_t nodes_before_paste = state.graph.nodes.size();
        const std::size_t edges_before_paste = state.graph.edges.size();

        state.selected_nodes.clear();
        for (const Source& source : sources) {
            state.selected_nodes.push_back(source.id);
        }
        state.copySelection();
        const bool copied = state.has_clipboard();
        const bool pasted = state.pasteClipboard();

        std::vector<const aiwrite::engine::Node*> added;
        for (const aiwrite::engine::Node& node : state.graph.nodes) {
            if (ids_before.find(node.id) == ids_before.end()) {
                added.push_back(&node);
            }
        }

        const std::size_t edges_after_paste = state.graph.edges.size();

        const bool count_ok = copied && pasted && added.size() == sources.size() &&
                              state.graph.nodes.size() == nodes_before_paste + sources.size() &&
                              edges_after_paste == edges_before_paste + internal_edges;

        // 诊断：粘贴后"新增节点之间的连线"数量（应与选区内部连线数一致）
        std::size_t added_edges = 0;
        for (const aiwrite::engine::Edge& edge : state.graph.edges) {
            bool from_added = false;
            bool to_added   = false;
            for (const aiwrite::engine::Node* node : added) {
                from_added = from_added || node->id == edge.from_node;
                to_added   = to_added || node->id == edge.to_node;
            }
            if (from_added && to_added) {
                ++added_edges;
            }
        }

        bool placement_ok = true;
        bool params_ok    = true;
        for (const aiwrite::engine::Node* node : added) {
            const Source* origin = nullptr;
            for (const Source& source : sources) {
                if (source.type == node->type) {
                    origin = &source;
                    break;
                }
            }
            if (origin == nullptr) {
                placement_ok = false;
                continue;
            }
            constexpr float kOffset = 40.0f;
            if (!std::isfinite(node->x) || !std::isfinite(node->y) ||
                std::fabs(node->x) > 1.0e6f || std::fabs(node->y) > 1.0e6f ||
                std::fabs(node->x - (origin->x + kOffset)) > 0.01f ||
                std::fabs(node->y - (origin->y + kOffset)) > 0.01f) {
                placement_ok = false;
            }
            for (const std::pair<std::string, nlohmann::json>& expected : origin->params) {
                const aiwrite::engine::Param* actual = node->findParam(expected.first);
                if (actual == nullptr || actual->value != expected.second) {
                    params_ok = false;
                }
            }
        }

        const bool selection_ok = state.selected_nodes.size() == 2 &&
                                  state.request_navigate_to_content; // 视图跟随延后到画布绘制后
        const bool undo_ok = (state.undo.canUndo() && state.undoOnce() &&
                              state.graph.nodes.size() == nodes_before_paste &&
                              state.graph.edges.size() == edges_before_paste);
        // 空剪贴板：清空选择后复制 → 剪贴板应为空，粘贴被拒绝
        state.selected_nodes.clear();
        state.copySelection();
        const bool empty_ok = !state.has_clipboard() && !state.pasteClipboard();

        paste_ok = count_ok && placement_ok && params_ok && selection_ok && undo_ok && empty_ok;
        std::printf("[运行自检] F1 复制/粘贴：%s（新增 %zu 节点 / 连线 期望+%zu 实际+%zu 新增节点间 %zu / "
                    "位置=%s / 参数=%s / 选择与跟随=%s / 可撤销=%s / 空剪贴板=%s）\n",
                    paste_ok ? "OK" : "失败", added.size(), internal_edges,
                    edges_after_paste - edges_before_paste, added_edges,
                    placement_ok ? "OK" : "失败", params_ok ? "OK" : "失败",
                    selection_ok ? "OK" : "失败", undo_ok ? "OK" : "失败",
                    empty_ok ? "OK" : "失败");
    }

    // ---- ARCH-R3：ReadOnlyView 渲染视图断言（ARCH-D18 行为等价量化）----
    //  * 零拷贝：nodes_for_render / edges_for_render 返回 **graph 内部数组的同一份引用**
    //  * find_node 与 graph.findNode 同源命中
    //  * snapshot / config 与底层对象同源（视图不复制数据）
    bool readonly_view_ok = false;
    {
        const aiwrite::ui::SelectionReadView sel_view{state.selected_nodes, state.selected_links,
                                                      state.selected_node};
        const aiwrite::ui::ReadOnlyView rv{state.graph, sel_view, state.run_snapshot_view(),
                                           aiwrite::appConfig()};
        const bool nodes_same = (&rv.nodes_for_render() == &state.graph.nodes);
        const bool edges_same = (&rv.edges_for_render() == &state.graph.edges);
        const bool find_same  = (rv.find_node("n1") == state.graph.findNode("n1")) &&
                                (rv.find_node("zz_missing") == nullptr);
        const bool snap_same  = (&rv.snapshot == &state.run_snapshot_view());
        const bool cfg_same   = (&rv.config == &aiwrite::appConfig());
        const bool sel_same   = (&rv.selection.nodes == &state.selected_nodes) &&
                                (&rv.selection.links == &state.selected_links) &&
                                (rv.selection.single == state.selected_node);
        readonly_view_ok = nodes_same && edges_same && find_same && snap_same && cfg_same &&
                           sel_same;
        std::printf("[运行自检] ReadOnlyView：%s（节点零拷贝=%s / 连线零拷贝=%s / find_node=%s / "
                    "快照同源=%s / 配置同源=%s / 选中同源=%s）\n",
                    readonly_view_ok ? "OK" : "失败", nodes_same ? "是" : "否",
                    edges_same ? "是" : "否", find_same ? "是" : "否", snap_same ? "是" : "否",
                    cfg_same ? "是" : "否", sel_same ? "是" : "否");
    }

    // ---- ARCH-R4：命令路由层断言（快捷键表完整性 / 唯一性）----
    //  * 四组绑定齐全：Main 的 Delete / Ctrl+S（保存工作流）·
    //    CompositeEditor 的 Delete / Ctrl+S（保存上级节点）—— 修 Ctrl+S 的静态证据
    //  * 同一作用域 × 命令在表内不重复（表驱动唯一性，避免二义触发）
    bool command_route_ok = false;
    {
        bool has_main_delete = false;
        bool has_main_save   = false;
        bool has_ce_delete   = false;
        bool has_ce_save     = false;
        bool table_unique    = true;
        for (std::size_t i = 0; i < aiwrite::ui::shortcutTableCount(); ++i) {
            const aiwrite::ui::ShortcutBinding& binding = aiwrite::ui::shortcutTable()[i];
            for (std::size_t j = i + 1; j < aiwrite::ui::shortcutTableCount(); ++j) {
                const aiwrite::ui::ShortcutBinding& other = aiwrite::ui::shortcutTable()[j];
                if (binding.scope == other.scope && binding.command == other.command) {
                    table_unique = false;
                }
            }
            if (binding.scope == aiwrite::ui::CommandScope::Main) {
                if (binding.command == aiwrite::ui::AppCommand::DeleteSelection) {
                    has_main_delete = true;
                }
                if (binding.command == aiwrite::ui::AppCommand::SaveWorkflow) {
                    has_main_save = true;
                }
            }
            else if (binding.scope == aiwrite::ui::CommandScope::CompositeEditor) {
                if (binding.command == aiwrite::ui::AppCommand::CompositeDeleteSelection) {
                    has_ce_delete = true;
                }
                if (binding.command == aiwrite::ui::AppCommand::CompositeSave) {
                    has_ce_save = true;
                }
            }
        }
        command_route_ok = has_main_delete && has_main_save && has_ce_delete && has_ce_save &&
                           table_unique;
        std::printf("[运行自检] 命令路由：%s（Main.Delete=%s / Main.Ctrl+S=%s / ce.Delete=%s / "
                    "ce.Ctrl+S=%s / 表内唯一=%s）\n",
                    command_route_ok ? "OK" : "失败", has_main_delete ? "有" : "无",
                    has_main_save ? "有" : "无", has_ce_delete ? "有" : "无",
                    has_ce_save ? "有" : "无", table_unique ? "是" : "否");
    }

    // ---- ARCH-E1：编辑事件断言（EventBus 机制 + 8 种类型完整性 + 编辑操作→事件映射）----
    //  * 机制：按 kind 分组订阅（发 NodeAdded 不触发 NodeRemoved 订阅者）· 同 kind 按订阅顺序
    //    FIFO 分发 · 重复订阅追加 · unsubscribe 后不再收到（ARCH-D7 形态 1：通知）
    //  * 类型：EditEventKind::Count == 8（ARCH-D12）
    //  * 映射：createNode → NodeAdded · deleteNode → NodeRemoved（真实 editor() 操作，结束后恢复）
    bool edit_event_ok = false;
    {
        using aiwrite::ui::EditEvent;
        using aiwrite::ui::EditEventKind;

        // ① 机制（用独立临时总线验证，不污染 editor() 的事件总线）
        aiwrite::ui::EventBus bus;
        std::vector<std::string> received;
        const std::size_t h1 = bus.subscribe(EditEventKind::NodeAdded,
            [&received](const EditEvent& e) { received.push_back("A:" + e.node_id); });
        const std::size_t h2 = bus.subscribe(EditEventKind::NodeAdded,
            [&received](const EditEvent& e) { received.push_back("B:" + e.node_id); });
        const std::size_t h3 = bus.subscribe(EditEventKind::NodeRemoved,
            [&received](const EditEvent& e) { received.push_back("R:" + e.node_id); });
        bus.publish(EditEvent::node(EditEventKind::NodeAdded, "n_test"));
        // NodeRemoved 订阅者 h3 不得收到 NodeAdded → 只收到 A/B 两条（按订阅顺序 FIFO）
        const bool kind_filter_ok = received.size() == 2 && received[0] == "A:n_test" &&
                                    received[1] == "B:n_test";
        bus.unsubscribe(h1);
        bus.publish(EditEvent::node(EditEventKind::NodeAdded, "n2"));
        const bool unsubscribe_ok = received.size() == 3 && received[2] == "B:n2";
        bus.publish(EditEvent::node(EditEventKind::NodeRemoved, "n3"));
        const bool node_removed_ok = received.size() == 4 && received[3] == "R:n3";
        const bool count_ok = (bus.subscriberCount(EditEventKind::NodeAdded) == 1) &&
                              (bus.subscriberCount() == 2); // h2 + h3

        // ② 8 种类型完整性（ARCH-D12）
        const bool eight_kinds = static_cast<int>(EditEventKind::Count) == 8;

        // ③ 编辑操作 → 事件映射（真实 editor() 操作；断言后删除恢复原图）
        bool op_add = false;
        bool op_remove = false;
        const std::size_t ha = state.event_bus().subscribe(
            EditEventKind::NodeAdded, [&op_add](const EditEvent& e) { op_add = !e.node_id.empty(); });
        const std::size_t hr = state.event_bus().subscribe(
            EditEventKind::NodeRemoved,
            [&op_remove](const EditEvent& e) { op_remove = !e.node_id.empty(); });
        const std::string tmp_id = state.createNode("TextInput", 0.0f, 0.0f);
        const bool add_mapping_ok = op_add && !tmp_id.empty();
        const bool remove_mapping_ok = state.deleteNode(tmp_id) && op_remove;
        state.event_bus().unsubscribe(ha);
        state.event_bus().unsubscribe(hr);

        edit_event_ok = kind_filter_ok && unsubscribe_ok && node_removed_ok && count_ok &&
                        eight_kinds && add_mapping_ok && remove_mapping_ok;
        std::printf("[运行自检] 编辑事件：%s（kind分组=%s / FIFO顺序=%s / 注销=%s / "
                    "NodeRemoved=%s / 计数=%s / 8种=%s / createNode→NodeAdded=%s / "
                    "deleteNode→NodeRemoved=%s）\n",
                    edit_event_ok ? "OK" : "失败", kind_filter_ok ? "是" : "否",
                    kind_filter_ok ? "是" : "否", unsubscribe_ok ? "是" : "否",
                    node_removed_ok ? "是" : "否", count_ok ? "是" : "否",
                    eight_kinds ? "是" : "否", add_mapping_ok ? "是" : "否",
                    remove_mapping_ok ? "是" : "否");
    }

    const bool pass = finished && saved && opened && same && json_clean && archive_ok && doc_found &&
                      consistency_ok && param_driven_ok && async_ok && session_ok && sink_ok && seed_ok &&
                      param_range_ok && paste_ok && readonly_view_ok && command_route_ok &&
                      edit_event_ok;
    std::printf("[运行自检] %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int webChatSelftest(const std::string& prompt)
{
    aiwrite::web::LoginRequest request;
    request.offscreen              = true;
    request.timeout_seconds        = 0;    // 生命周期由本函数控制
    request.probe_after_load       = true; // 目的：拿到 Cookie + userToken
    request.auto_close_after_probe = false; // 窗口保留给后续 PoW 求解复用

    aiwrite::web::LoginWindow& window = aiwrite::web::loginWindow();
    std::string error;
    if (!window.start(request, &error)) {
        std::printf("[网页版生成] 启动登录窗口失败: %s\n", error.c_str());
        return 1;
    }

    // 等探测完成（窗口保持打开，供后续 PoW 求解复用）
    for (int i = 0; i < 200; ++i) { // ≤20 秒
        if (!aiwrite::web::SessionStore::instance().snapshot().probe.ran_at.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const aiwrite::web::Session session = aiwrite::web::SessionStore::instance().snapshot();
    std::printf("[网页版生成] 会话：Cookie %zu 条，userToken %s\n", session.cookieCount(),
                session.user_token.empty() ? "未获取" : "已获取（仅内存）");
    if (session.user_token.empty()) {
        std::printf("[网页版生成] FAIL：请先登录一次（`aiwrite.exe --login-selftest` 或界面"
                    "参数面板的「打开登录窗口」）\n");
        return 1;
    }

    aiwrite::ai::WebChatRequest chat;
    chat.prompt = prompt;
    const aiwrite::ai::WebChatResult result = aiwrite::ai::webChat(session, chat);

    std::printf("[网页版生成] HTTP %d，PoW 尝试 %lld 次\n", result.http_status,
                result.pow_attempts);
    std::printf("---- 原始响应前几行 ----\n%s\n", result.raw_head.c_str());
    std::printf("---- 生成文本 ----\n%s\n", result.text.c_str());
    if (!result.error.empty()) {
        std::printf("---- 错误 ----\n%s\n", result.error.c_str());
    }

    const bool pass = result.ok && !result.text.empty();
    std::printf("[网页版生成] %s\n", pass ? "PASS" : "FAIL");

    aiwrite::web::loginWindow().requestClose(); // 收尾：关闭离屏窗口
    aiwrite::web::loginWindow().join();
    aiwrite::web::SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : 1;
}

int webSessionSelftest()
{
    aiwrite::web::LoginWindow& window = aiwrite::web::loginWindow();

    // ---- 步骤 1：模拟用户手动点「打开登录窗口」（历史行为：不自动探测）----
    aiwrite::web::LoginRequest manual;
    manual.offscreen        = true;  // 离屏，不打扰用户
    manual.probe_after_load = false; // 【关键】刻意不探测 → 内存无 userToken
    std::string error;
    if (!window.start(manual, &error)) {
        std::printf("[会话自检] FAIL 启动登录窗口失败：%s\n", error.c_str());
        return 1;
    }
    for (int i = 0; i < 100; ++i) { // ≤10 秒：等页面加载（Cookie 提取即可）
        if (aiwrite::web::SessionStore::instance().snapshot().loggedIn) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const aiwrite::web::Session before = aiwrite::web::SessionStore::instance().snapshot();
    const bool                 token_empty = before.user_token.empty();
    std::printf("[会话自检] 步骤 1：窗口已开（Cookie %zu 条，userToken %s）\n",
                before.cookieCount(), token_empty ? "空（符合预期）" : "非空（场景不成立）");

    // ---- 步骤 2：ensure_session 必须自己补探测，而不是干等 ----
    const bool ok = aiwrite::web::ensureSession(30000, &error);
    const aiwrite::web::Session after = aiwrite::web::SessionStore::instance().snapshot();
    if (ok) {
        std::printf("[会话自检] 步骤 2：ensure_session=OK（userToken 长度 %zu，仅内存）\n",
                    after.user_token.size());
    }
    else {
        std::printf("[会话自检] 步骤 2：ensure_session=失败：%s\n", error.c_str());
    }

    const bool pass = token_empty && ok;
    std::printf("[会话自检] %s（判定：开窗口时无凭证=%s / ensure_session 取得凭证=%s）\n",
                pass ? "PASS" : "FAIL", token_empty ? "是" : "否", ok ? "是" : "否");

    window.requestClose(); // 收尾：关闭离屏窗口
    window.join();
    aiwrite::web::SessionStore::instance().clear(); // 自检不留会话
    return pass ? 0 : 1;
}

int vlm_selftest(const std::string& image_path, const std::string& api_base,
                 const std::string& model, const std::string& key_ref)
{
    aiwrite::ai::OfficialChatRequest request;
    request.api_base    = api_base;
    request.model       = model;
    request.prompt      = "用一句中文描述这张图片的主要内容。";
    request.images      = {image_path};
    request.temperature = 1.0;
    request.max_tokens  = 512;
    request.top_p       = 0.95;

    // ---- ① 离线：图片编码 + 请求体构造 ----
    std::string       encode_error;
    const auto        encode_started = std::chrono::steady_clock::now();
    const std::string data_url =
        aiwrite::ai::encode_image_data_url(image_path, request.image_max_bytes, &encode_error);
    const double      encode_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encode_started)
            .count();
    if (data_url.empty()) {
        std::printf("[图片理解自检] FAIL：图片编码失败：%s\n", encode_error.c_str());
        return 1;
    }
    const nlohmann::json       body     = aiwrite::ai::buildRequestBody(request);
    const nlohmann::json&      messages = body["messages"];
    // 注意：未设置 system_prompt 时 messages 只有 1 条 → 取最后一条（user）
    const nlohmann::json&      parts    = messages.back()["content"];
    const bool                 body_ok  =
        parts.is_array() && parts.size() == 2 && parts[0]["type"] == "text" &&
        parts[1]["type"] == "image_url" &&
        parts[1]["image_url"]["url"].get<std::string>() == data_url;
    std::printf("[图片理解自检] 端点：%s\n", aiwrite::ai::buildEndpoint(api_base).c_str());
    std::printf("[图片理解自检] 模型：%s\n", model.c_str());
    std::printf("[图片理解自检] 图片：%s（data URL %zu 字符）\n", image_path.c_str(),
                data_url.size());
    // P7a-10：编码后体积与耗时（base64 字符 ≈ 原始 × 1.33）—— 离线即可核对，不必真发请求
    std::printf("[图片理解自检] 编码后体积：%s（base64 字符 %zu，原始约 %s），耗时 %.0f ms\n",
                aiwrite::ai::humanBytes(data_url.size()).c_str(), data_url.size(),
                aiwrite::ai::humanBytes(static_cast<std::size_t>(
                    static_cast<double>(data_url.size()) / 1.33)).c_str(),
                encode_ms);
    if (!body_ok) {
        std::printf("[图片理解自检] FAIL：请求体不是预期的多模态结构（text + image_url）\n");
        return 1;
    }
    std::printf("[图片理解自检] 离线请求体构造 PASS\n");

    // ---- ② 联网：Key 解析（参数 → 环境变量 → 凭据库引用）----
    const aiwrite::utils::ResolvedSecret secret =
        aiwrite::utils::resolveSecret(std::string(), key_ref);
    if (secret.key.empty()) {
        std::printf("[图片理解自检] 未找到 API Key（环境变量 DEEPSEEK_API_KEY 或凭据库 %s）\n",
                    key_ref.c_str());
        std::printf("  提示：在界面「提供商配置 → API Key」填写一次即会自动入库（ref=%s）\n",
                    key_ref.c_str());
        return 2;
    }
    request.api_key = secret.key;
    std::printf("[图片理解自检] 凭据来源=%s，开始真实调用…\n", secret.source.c_str());

    const aiwrite::ai::OfficialChatResult result = aiwrite::ai::officialChat(request);
    std::printf("[图片理解自检] HTTP %d，耗时 %.0f ms\n", result.http_status, result.elapsed_ms);
    if (!result.ok || result.text.empty()) {
        std::printf("[图片理解自检] FAIL：%s\n",
                    result.error.empty() ? "未取到文本" : result.error.c_str());
        return 1;
    }
    std::printf("---- 生成文本 ----\n%s\n", result.text.c_str());
    std::printf("[图片理解自检] PASS\n");
    return 0;
}

std::string joinNames(const std::vector<std::string>& items)
{
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += "、";
        }
        text += item;
    }
    return text.empty() ? std::string("（无）") : text;
}

int provider_selftest(const std::string& provider_id, const std::string& api_base,
                      const std::string& model, const std::string& key_ref)
{
    const aiwrite::ai::ProviderSpecs specs = aiwrite::ai::providerSpecs();
    std::printf("[Provider 自检] 表：%s；条目 %zu 条（official %zu / web %zu）\n",
                specs.report.summary().c_str(), specs.items.size(),
                specs.countKind("official"), specs.countKind("web"));
    printSpecList(specs);

    int       offline_passed = 0;
    const int offline_failed = aiwrite::ai::providerSpecSelftest(&offline_passed);
    if (provider_id.empty()) {
        std::printf("[Provider 自检] 未指定 --provider：仅完成离线部分（表校验 + 断言）\n");
        return offline_failed == 0 ? 0 : 1;
    }

    const aiwrite::ai::ProviderSpec* spec = specs.find(provider_id);
    if (spec == nullptr) {
        std::printf("[Provider 自检] FAIL：表里没有 id=%s（用 --provider-dump 查看全部）\n",
                    provider_id.c_str());
        return 1;
    }

    // ---- 网页版条目：只检查登录态与端点一致性（不发送内容，避免误触发站点）----
    if (spec->kind == "web") {
        const std::string token =
            aiwrite::web::SessionStore::instance().snapshot().user_token;
        const bool loggedIn = !token.empty();
        std::printf("[Provider 自检] 网页版 %s：适配器=%s\n", spec->display.c_str(),
                    spec->web.adapter.c_str());
        std::printf("[Provider 自检] 登录页：%s\n", spec->web.login_url.c_str());
        if (spec->web.adapter == "dom") {
            // M_patchB L1 修订（PB2-26）：DOM 条目**没有内置端点**（真实交互由 DOM 执行器在页面内完成）
            //  * 如实显示「不适用 / 生成未就绪」，避免把内置默认（DeepSeek）端点误读成本条目的端点
            std::printf("[Provider 自检] 端点：不适用（DOM 适配器；无内置端点）\n");
            std::printf("[Provider 自检] 生成：%s\n",
                        aiwrite::ai::webLoginOnly(spec)
                            ? "未回填（生成字段缺失）—— **运行时会自动识别**选择器（M8-37）；"
                              "固化可用 --web-dom-dump"
                            : "字段齐全（声明值优先；未命中时仍会自动识别兜底）");
        }
        else {
            std::printf("[Provider 自检] 端点：host=%s completion=%s challenge=%s\n",
                        spec->web.endpoints.host.c_str(),
                        spec->web.endpoints.completion_path.c_str(),
                        spec->web.endpoints.challenge_path.c_str());
        }
        std::printf("[Provider 自检] 探测路径：%s\n", joinNames(spec->web.probe_paths).c_str());
        std::printf("[Provider 自检] 登录态=%s（内存凭证，程序退出即销毁）\n",
                    loggedIn ? "有" : "无");
        if (!loggedIn) {
            std::printf("  提示：先在界面「提供商配置 → 打开登录窗口」登录一次，或跑 "
                        "--web-session-selftest\n");
            return 2;
        }
        return offline_failed == 0 ? 0 : 1;
    }

    // ---- API 条目：协议实现检查 → Key 解析（env 列表 → 凭据库）→ 一条 ping ----
    //  * L2（`B2-b` · PB2-09/10/11）：**按表 protocol 经工厂分派**（不再只认 openai）
    if (!aiwrite::ai::factoryImplements(spec->protocol)) {
        std::printf("[Provider 自检] %s 的 protocol=%s 本版本尚未实现（工厂已实现：%s）"
                    "：跳过联网测试\n",
                    spec->display.c_str(), spec->protocol.c_str(),
                    aiwrite::ai::factoryProtocolsText().c_str());
        return offline_failed == 0 ? 0 : 1;
    }
    const std::string effective_base = aiwrite::ai::resolveApiBase(*spec, api_base);
    const std::string effective_model =
        aiwrite::ai::resolve_model(*spec, model, std::string(), false);
    const std::string effective_ref = aiwrite::ai::resolveKeyRef(*spec, key_ref);
    std::printf("[Provider 自检] 生效地址=%s；生效模型=%s；引用名=%s\n", effective_base.c_str(),
                effective_model.c_str(), effective_ref.c_str());
    if (effective_base.empty()) {
        std::printf("[Provider 自检] FAIL：该条目未填 api_base，请用 --api-base 指定\n");
        return 1;
    }

    std::string key;
    std::string key_source;
    for (const std::string& env_name : spec->env_names) {
        const char* value = std::getenv(env_name.c_str());
        if (value != nullptr && *value != '\0') {
            key        = value;
            key_source = "环境变量 " + env_name;
            break;
        }
    }
    if (key.empty()) {
        std::string       load_error;
        const std::string stored = aiwrite::utils::loadCredential(effective_ref, &load_error);
        if (!stored.empty()) {
            key        = stored;
            key_source = "凭据库 " + effective_ref;
        }
    }
    if (key.empty()) {
        std::printf("[Provider 自检] 未找到 API Key（环境变量 %s / 凭据库 %s）\n",
                    joinNames(spec->env_names).c_str(), effective_ref.c_str());
        std::printf("  提示：在界面「提供商配置 → API Key」填写一次即自动入库\n");
        return 2;
    }
    std::printf("[Provider 自检] 凭据来源=%s，发一条 ping（消耗极小额度）…\n", key_source.c_str());

    // ---- L2（`B2-b`）：经**工厂**按表 protocol 分派（openai / anthropic / gemini 三协议同一入口）----
    aiwrite::ai::ProviderRuntimeConfig runtime;
    runtime.api_base    = effective_base;
    runtime.model       = effective_model;
    runtime.api_key     = key;
    runtime.options     = aiwrite::ai::providerOptionsFrom(*spec);
    runtime.console_tag = "[Provider 自检]";
    const aiwrite::ai::ProviderCreation created = aiwrite::ai::createProvider(*spec, runtime);
    if (created.provider == nullptr) {
        std::printf("[Provider 自检] FAIL：%s\n", created.error.c_str());
        return 1;
    }
    std::printf("[Provider 自检] 协议=%s，实现=%s\n", created.provider->protocol().c_str(),
                created.provider->name().c_str());

    aiwrite::ai::GenerateParams ping_params;
    ping_params.prompt      = "请只回复：pong";
    ping_params.max_tokens  = 16;
    ping_params.temperature = 0.0;
    ping_params.top_p       = 1.0;
    const aiwrite::ai::GenerateResult ping = created.provider->generate(ping_params);
    std::printf("[Provider 自检] HTTP %d，耗时 %.0f ms\n", ping.http_status, ping.elapsed_ms);
    if (!ping.ok || ping.text.empty()) {
        std::printf("[Provider 自检] FAIL：%s\n",
                    ping.error.empty() ? "未取到文本" : ping.error.c_str());
        return 1;
    }
    std::printf("[Provider 自检] 返回：%s\n", ping.text.substr(0, 120).c_str());
    std::printf("[Provider 自检] PASS\n");
    return offline_failed == 0 ? 0 : 1;
}

