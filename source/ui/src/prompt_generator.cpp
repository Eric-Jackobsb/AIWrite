#include "prompt_generator.h"

#include "log.h"

#include "credential.h"                 // 凭据库（N-05 解析同款）
#include "deepseek_official_provider.h" // ai：official_chat / resolve_api_key
#include "provider_spec.h"              // ai：配置表（条目 / resolve_api_base / resolve_model / resolve_key_ref）

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aiwrite::ui {
namespace {

using nlohmann::json;

struct PromptGenState {
    std::mutex  mutex;
    bool        running  = false;
    std::string node_id;  // 最近一次请求的节点
    std::string result;   // 成功文本（消费后清空）
    std::string error;    // 失败信息（消费后清空）
    std::thread worker;   // 可 join（退出统一收尾）
};

PromptGenState& gen()
{
    static PromptGenState instance;
    return instance;
}

// N-05 解析复用：节点参数 → 生效 API 地址 / 模型 / Key（official / 兼容 API 通道）
//  * 与 local_nodes.cpp `resolve_official_key` 同口径，但**只读节点自身参数**（无需 provider 连线）
bool resolve_generate_target(const json& params, ai::OfficialChatRequest* request, std::string* error)
{
    const std::string      provider_id = params.value("provider", std::string("deepseek"));
    const ai::ProviderSpec* spec       = ai::provider_specs().find(provider_id);
    if (spec != nullptr) {
        request->options = ai::provider_options_from(*spec);
    }
    request->api_base = params.value("api_base", std::string());
    if (spec != nullptr) {
        request->api_base = ai::resolve_api_base(*spec, request->api_base);
    }
    request->options.api_base = request->api_base;

    const std::string model_custom = params.value("model_custom", std::string());
    request->model                = params.value("model", std::string("deepseek-chat"));
    if (spec != nullptr) {
        request->model = ai::resolve_model(*spec, request->model, model_custom, false);
    }
    else if (!model_custom.empty()) {
        request->model = model_custom;
    }

    const std::string key_ref = params.value("api_key_ref", std::string());
    const std::string key     = params.value("api_key", std::string());
    request->api_key = ai::resolve_api_key(key, spec != nullptr ? spec->env_names
                                                                : std::vector<std::string>());
    if (request->api_key.empty() && !key.empty()) {
        request->api_key = key;
    }
    if (request->api_key.empty() && spec != nullptr) {
        const std::string ref = ai::resolve_key_ref(*spec, key_ref);
        std::string       load_error;
        request->api_key = utils::load_credential(ref, &load_error);
    }
    if (request->api_key.empty()) {
        const std::string env_hint = (spec != nullptr && !spec->env_names.empty())
                                         ? spec->env_names.front()
                                         : std::string("DEEPSEEK_API_KEY");
        *error = "缺少 API Key：请在「提示词模板 → API Key」填写，或设置环境变量 " + env_hint;
        return false;
    }
    return true;
}

// 工作线程主体：构建请求 → official_chat → 写回共享状态
void run_generation(const std::string& node_id, const json& params)
{
    std::string result_text;
    std::string error_text;

    const std::string mode = params.value("mode", std::string("official"));
    if (mode == "web") {
        error_text = "网页版暂不支持「生成提示词」：请把「模式」切到 official 并填写 API Key"
                     "（或使用官方 API 条目）";
    }
    else {
        const std::string instruction = params.value("template", std::string());
        if (instruction.empty()) {
            error_text = "模板为空：请先在「模板」里写下「给 AI 的指令」再点「生成提示词」";
        }
        else {
            ai::OfficialChatRequest request;
            request.prompt         = instruction;
            request.system_prompt  = params.value("system_prompt", std::string());
            request.temperature    = params.value("temperature", 0.7);
            request.max_tokens     = params.value("max_tokens", 2048);
            request.top_p          = params.value("top_p", 1.0);
            request.top_k          = params.value("top_k", 0);
            request.stop           = params.value("stop", std::vector<std::string>{});
            request.seed           = params.value("seed", 0);
            if (resolve_generate_target(params, &request, &error_text)) {
                const ai::OfficialChatResult result = ai::official_chat(request);
                if (result.ok && !result.text.empty()) {
                    result_text = result.text;
                }
                else {
                    error_text = "AI 生成失败：" +
                                 (result.error.empty() ? std::string("未取到文本") : result.error);
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(gen().mutex);
    gen().running = false;
    gen().node_id = node_id;
    gen().result  = result_text;
    gen().error   = error_text;
    log::info(std::string("[生成提示词] ") + (result_text.empty() ? "失败：" + error_text
                                                                  : "成功（" +
                                                                        std::to_string(result_text.size()) +
                                                                        " 字节）"));
}

} // namespace

void request_prompt_generation(const std::string& node_id, const json& params)
{
    std::lock_guard<std::mutex> lock(gen().mutex);
    if (gen().running) {
        return; // 已在生成中：忽略新请求（按钮已禁用，防御）
    }
    gen().running = true;
    gen().node_id = node_id;
    gen().result.clear();
    gen().error.clear();
    gen().worker = std::thread([node_id, params]() { run_generation(node_id, params); });
}

bool prompt_generation_running()
{
    std::lock_guard<std::mutex> lock(gen().mutex);
    return gen().running;
}

bool prompt_generation_consume(const std::string& node_id, std::string* result, std::string* error)
{
    std::lock_guard<std::mutex> lock(gen().mutex);
    if (gen().running || gen().node_id != node_id || (gen().result.empty() && gen().error.empty())) {
        return false;
    }
    if (result != nullptr) {
        *result = gen().result;
    }
    if (error != nullptr) {
        *error = gen().error;
    }
    gen().result.clear();
    gen().error.clear();
    gen().node_id.clear();
    return true;
}

void stop_prompt_generation(int wait_ms)
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(gen().mutex);
        if (gen().worker.joinable()) {
            worker = std::move(gen().worker);
        }
    }
    if (worker.joinable()) {
        worker.join();
        return;
    }
    // running 但线程句柄已被分离的极端情况：轮询等待至超时
    for (int waited = 0; prompt_generation_running() && waited < wait_ms; waited += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace aiwrite::ui

