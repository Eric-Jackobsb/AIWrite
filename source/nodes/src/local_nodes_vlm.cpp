#include "nodes.h"

#include "provider_spec.h"          // M_patchB L1：配置表（能力表决定支不支持视觉）
#include "provider_resolve.h"       // M_patchB L1：EffectiveProvider
#include "inference_provider.h"     // 能力门控（表驱动）
#include "cancel_token.h"           // PB-02：取消令牌
#include "config.h"                 // L1-a（§3.3 增量②）：超时 + 重试
#include "retry_policy.h"           // 可重试状态 / 退避 / 次数钳制（纯函数）
#include "deepseek_official_provider.h"
#include "upload_registry.h"        // M8-14：站点上传模板（网页版图片上传 → 再发送）
#include "dom_web_client.h"         // L3（PB2-13）：通用 DOM 站点适配器
#include "log.h"
#include "session_store.h"
#include "webview_host.h"
#include "local_nodes_detail.h"     // ARCH-Z1：公用函数（nodes_detail）

#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::nodes {

using namespace nodes_detail;  // 原匿名命名空间函数（同 TU 直接调用，名字查找不变）
using nlohmann::json;
// --- N-07 图片理解（M5-02：OpenAI 兼容多模态 /chat/completions）---------------
//  * 输入：prompt + image（可多张）+ provider 句柄
//  * 图片 → data URL（`ai::encode_image_data_url`）→ `messages[].content` 数组
//  * 模型名：provider 句柄的 model（「提供商配置 → 模型（自定义）」优先，见 D-07）
//  * 网页版没有图片入口 → 明确报错并指向 official
json execute_vlm_generate(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    // ---- M_patchB L1（PB2-05）：生效提供商来自配置表（能力表决定「支不支持视觉」）----
    const engine::EffectiveProvider effective = effective_provider_of(ctx);
    std::string mode         = "official";
    std::string model        = std::string();
    std::string model_custom = std::string();
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        mode         = it->value("mode", mode);
        model        = it->value("model", model);
        model_custom = it->value("model_custom", model_custom);
    }
    if (effective.resolved) {
        mode  = effective.mode;
        model = effective.model;
    }

    const std::string prompt = inputs.contains("prompt") ? text_of(inputs["prompt"]) : std::string();
    if (prompt.empty()) {
        throw engine::NodeError("图片理解：提示词为空（请连接「提示词模板」或「文本输入」到 prompt）");
    }

    // 图片来自 image 输入：P7a-01 端口已变长 → 单值 / 数组 / 嵌套数组统一展平
    std::vector<std::string> images;
    if (const auto it = inputs.find("image"); it != inputs.end()) {
        images = collect_image_paths(*it);
    }
    if (images.empty()) {
        throw engine::NodeError("图片理解：未连接图片（请把「图片输入」节点的 image 连到本节点 image 端口）");
    }
    // M_8nodes_extension L1-b（§4.7）：多图顺序 —— by_name = 按文件名排序（稳定，等值保相对序）
    if (params.value("image_order", std::string("edge_order")) == "by_name") {
        std::stable_sort(images.begin(), images.end());
    }
    // M_8nodes_extension L1-c（§4.7）：图片预处理 —— 与 N-02 同名参数**共用**（发送前应用）
    //  * 本地变换（读取 → 裁剪 → 缩放 → 编码）发生在**上传 / 编码前**，不改双证据链路（I18）
    {
        const ai::ImagePreprocessParams pre = preprocess_params_from(params);
        if (pre.resize_mode != "none" || pre.crop_enabled) {
            std::vector<std::string> processed;
            const std::string        pre_error =
                apply_image_preprocess(images, pre, ctx, &processed, "[图片理解]");
            if (!pre_error.empty()) {
                throw engine::NodeError("图片理解预处理失败：" + pre_error);
            }
            images = std::move(processed);
        }
    }

    // ---- M8-14：网页版图片理解（**先上传，再发送**）----
    //  * 上传走**站点上传模板**（`ai/upload/**`）：每站一个独立上传单元 + 每站格式识别/转换
    //  * 不变量 `I18`：上传**双证据齐备**（页面附件区 + 站点上传请求回执）才注入提示词；
    //    不齐备 → 本节点直接报错，**不发送**（绝不把「图没上去」当成「模型没看懂」）
    //  * 「看图」由**站点自己的能力**完成（豆包 B3 已实测真视觉；Kimi 待侦察）
    if (mode == "web") {
        const std::string site_error = ai::webSiteError(effective.spec);
        if (!site_error.empty()) {
            throw engine::NodeError("图片理解（网页版）不可用：" + site_error);
        }
        const ai::ProviderWebSpec site   = ai::strictWebSpecFor(effective.spec);
        const std::string        site_id = ai::strictWebProviderIdFor(effective.spec);
        if (site.adapter != "dom") {
            throw engine::NodeError(
                "图片理解（网页版）不可用：站点「" + effective.display + "」的适配器 " +
                site.adapter + " 尚未接图片上传（当前支持：adapter=dom 的站点 + 站点上传单元）");
        }
        const std::string upload_error = ai::siteUploadError(site.upload_adapter);
        if (!upload_error.empty()) {
            throw engine::NodeError("图片理解（网页版）不可用：" + upload_error);
        }
        // M8-37（零开关）：生成字段未回填**不再拦路** —— dom_chat 运行期自动识别输入框 / 答案容器
        //  （识别结果只打印；要固化可用 --web-dom-dump --provider <id> 取建议选择器）
        if (ai::webLoginOnly(effective.spec)) {
            ctx.console("[图片理解] 站点「" + site.login_url +
                        "」的生成字段未回填（web.input_selector / send / answer_selector）—— "
                        "本次运行将**自动识别**；如需固化可用 --web-dom-dump --provider " + site_id +
                        " 取建议选择器写进 providers.json");
        }
        const std::string dom_field_warnings = ai::webSiteFieldWarnings(effective.spec);
        if (!dom_field_warnings.empty()) {
            ctx.console("[图片理解] " + dom_field_warnings); // 决策 D-26：可回落 + 警告
        }
        ctx.console("[图片理解] 站点：" + site.login_url + "（网页版 · 上传单元 " +
                    site.upload_adapter + "；图片 " + std::to_string(images.size()) +
                    " 张 / 提示词 " + std::to_string(prompt.size()) + " 字节）");

        ai::DomChatRequest dom_request;
        dom_request.prompt          = prompt;
        dom_request.site            = site;
        dom_request.provider_id     = site_id;
        dom_request.owner           = web_owner_of(ctx, site, site_id); // M_patchC PW-01：归属
        dom_request.image_values    = images; // 上传模板内部再解析令牌 / 校验存在性（I19）
        dom_request.image_max_bytes =
            effective.spec != nullptr ? effective.spec->limits.image_max_bytes : 0;
        // M_8nodes_extension L1-a（§3.3 增量③）：web 线总超时 = 节点覆盖优先（不缩短既有行为）
        dom_request.timeout_ms = std::max(
            dom_request.timeout_ms,
            std::max(params.value("timeout_connect_ms", 0), params.value("timeout_first_byte_ms", 0)));
        const ai::DomChatResult dom = ai::domChat(dom_request);
        for (const std::string& line : dom.conversions) {
            ctx.console("[图片理解] 转换：" + line); // 每张的格式处理都如实记录（可追溯）
        }
        // M8-37（口径①：只打印不落盘）：把自动识别到的选择器如实带出来，便于**可选**回填
        if (dom.input_auto) {
            ctx.console("[图片理解] 自动识别输入框：" + dom.input_used);
        }
        if (dom.answer_auto) {
            ctx.console("[图片理解] 自动识别答案容器：" + dom.answer_fingerprint);
        }
        if (dom.uploaded) {
            ctx.console("[图片理解] 上传双证据齐备 —— 页面：" + dom.upload_page + "｜网络：" +
                        dom.upload_net);
        }
        if (!dom.ok) {
            throw engine::NodeError("图片理解（网页版）失败：" + dom.error);
        }
        if (!dom.warning.empty()) {
            ctx.console("[图片理解] " + dom.warning); // R13：如实提示（可能仍在生成中）
        }
        ctx.console("[图片理解] 网页版完成：输出 " + std::to_string(dom.text.size()) +
                    " 字节，轮询 " + std::to_string(dom.polls) + " 次 / " +
                    std::to_string(dom.elapsed_ms) + " ms" +
                    (dom.steps.empty() ? "" : ("｜" + dom.steps)));
        return dom.text;
    }
    // 能力门控（`PB2-12`，表驱动）：① 逐模型的视觉声明（表内 vision=false ⇒ 拒绝并给替代）
    //                              ② provider 级 `capabilities.vision`（表里声明不支持 ⇒ 拦路）
    bool declared = false;
    if (effective.spec != nullptr) {
        if (!ai::spec_model_supports_vision(*effective.spec, model, &declared)) {
            const std::string suggested = !effective.spec->vision_model_default.empty()
                                              ? effective.spec->vision_model_default
                                              : std::string("（见 --provider-dump）");
            throw engine::NodeError("图片理解：" + effective.display + " 的模型 " + model +
                                    " 不支持视觉（表内视觉模型：" + suggested +
                                    "；可在「模型（自定义）」填写）");
        }
        if (!declared && !model.empty()) {
            ctx.console("[图片理解] 提示：配置表未声明模型 " + model +
                        " 的视觉能力，已按允许调用处理（若失败请改用表内视觉模型）");
        }
    }
    const ai::ProviderCaps vlm_caps = ai::capsForCall(effective.spec, false);
    ai::GenerateParams     vlm_gate_params;
    vlm_gate_params.prompt = prompt;
    vlm_gate_params.images = images;
    const ai::CapabilityGate vlm_gate = ai::capability_gate(vlm_caps, vlm_gate_params, true);
    for (const std::string& warning : vlm_gate.warnings) {
        ctx.console("[图片理解] " + warning);
    }
    if (!vlm_gate.blocking.empty()) {
        throw engine::NodeError("图片理解：" + vlm_gate.blocking.front());
    }
    if (model.empty()) {
        throw engine::NodeError("图片理解：未解析到模型名（请连接「提供商配置」节点，"
                                "并在「模型（自定义）」填写视觉模型名，如 glm-4v-flash）");
    }
    for (const std::string& path : images) {
        std::error_code code;
        if (!std::filesystem::exists(std::filesystem::path(path), code)) {
            throw engine::NodeError("图片理解：图片文件不存在: " + path);
        }
    }

    const std::string system_prompt = params.value("system_prompt", std::string());

    ai::OfficialChatRequest official;
    if (effective.spec != nullptr) {
        official.options         = ai::providerOptionsFrom(*effective.spec);
        official.image_max_bytes = effective.spec->limits.image_max_bytes;
    }
    official.api_base         = effective.api_base; // 工作流参数优先 → 表默认 → 引擎默认
    official.options.api_base = effective.api_base;
    official.model            = model;
    official.system_prompt    = system_prompt;
    official.prompt           = prompt;
    official.images           = images;
    official.temperature      = params.value("temperature", 0.7);
    official.max_tokens       = params.value("max_tokens", 2048);
    official.top_p            = params.value("top_p", 1.0);
    official.api_key = resolve_official_key(params, inputs, ctx, "[图片理解]", effective).key;
    if (official.api_key.empty()) {
        const std::string env_text =
            effective.spec != nullptr && !effective.spec->env_names.empty()
                ? effective.spec->env_names.front()
                : std::string("DEEPSEEK_API_KEY");
        throw engine::NodeError("图片理解（官方/兼容 API）缺少 API Key：请在「提供商配置」填写"
                                " API Key（会自动入库），或设置环境变量 " + env_text +
                                "，或使用凭据库 " +
                                (effective.key_ref.empty() ? std::string("（未设置引用名）")
                                                           : effective.key_ref));
    }
    console_provider_line(ctx, "[图片理解]", effective, "official");
    // PB-02（真取消）：图片理解同样受本运行的取消令牌管（多模态编码完成后即发请求）
    official.cancel = ctx.cancel_token;
    // M_8nodes_extension L1-a：`seed` 参数面对称（与 N-06 同属公共面 §5.1；>0 随请求发送）
    official.seed = params.value("seed", 0);
    // M_8nodes_extension L1-b：公共参数面采样项随请求发送（非零 / 非空才写进 body）
    official.top_k             = params.value("top_k", 0);
    official.stop              = params.value("stop", std::vector<std::string>{});
    official.frequency_penalty = params.value("frequency_penalty", 0.0);
    official.presence_penalty  = params.value("presence_penalty", 0.0);

    // ---- M_8nodes_extension L1-a（§3.3 增量② / §5.3）：图片理解接入超时 + 重试 ----
    //  * 现状只有文本生成走重试；本项把**同一份**读取接进 N-07（节点级覆盖优先，0 = 回落全局）
    const aiwrite::Config& runtime_config = aiwrite::appConfig();
    const int              override_connect_ms    = params.value("timeout_connect_ms", 0);
    const int              override_first_byte_ms = params.value("timeout_first_byte_ms", 0);
    if (official.options.connect_timeout_s <= 0) {
        official.options.connect_timeout_s =
            std::max(1, (override_connect_ms > 0 ? override_connect_ms
                                                 : runtime_config.timeout.connect_ms) /
                            1000);
    }
    if (official.options.read_timeout_s <= 0) {
        official.options.read_timeout_s =
            std::max(1, (override_first_byte_ms > 0 ? override_first_byte_ms
                                                    : runtime_config.timeout.first_byte_ms) /
                            1000);
    }
    const nodes_detail::RetryPlan retry = nodes_detail::resolve_retry_plan(params, runtime_config);
    const int max_retries = ai::clampRetryCount(retry.enabled, retry.count);

    ctx.console("[图片理解] 请求：" + official.model + "，图片 " + std::to_string(images.size()) +
                " 张，提示词 " + std::to_string(prompt.size()) + " 字节" +
                (system_prompt.empty() ? "" : "（含系统提示词）") +
                (max_retries > 0 ? ("；重试上限 " + std::to_string(max_retries) + " 次") : ""));
    ai::OfficialChatResult result;
    for (int attempt = 0;; ++attempt) {
        result = ai::officialChat(official);
        if (result.ok && !result.text.empty()) {
            break;
        }
        // 取消优先：取消导致的失败不重试（归因由执行器记「[取消]」）
        if (result.cancelled || ctx.is_cancelled()) {
            throw engine::NodeError(
                result.error.empty() ? std::string("图片理解已取消（用户点了「停止」）")
                                     : result.error);
        }
        if (attempt >= max_retries || !ai::httpStatusRetryable(result.http_status)) {
            break; // 不可重试 / 已用尽
        }
        const int delay_ms = ai::retryDelayMs(retry.interval_ms, attempt);
        ctx.console("[图片理解] 第 " + std::to_string(attempt + 1) + " 次重试（HTTP " +
                    std::to_string(result.http_status) + "，等 " + std::to_string(delay_ms) +
                    " ms；可重试失败 = 429 / 5xx / 网络超时）");
        if (ai::sleepInterruptible(ctx.cancel_token.get(), delay_ms)) {
            throw engine::NodeError("图片理解已取消（用户点了「停止」）：重试等待被中断");
        }
    }
    // P7a-10：编码后体积与耗时（**失败路径也打印** —— 便于判断是否被体积/格式挡住）
    if (result.encoded_bytes > 0) {
        char buffer[256] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "[图片理解] 编码：%zu 张 → base64 后 %s（原始约 %s，≈1.33×），耗时 %.0f ms%s",
                      images.size(), ai::humanBytes(result.encoded_bytes).c_str(),
                      ai::humanBytes(static_cast<std::size_t>(
                          static_cast<double>(result.encoded_bytes) / 1.33)).c_str(),
                      result.encode_ms,
                      result.encode_ms < 1.0 ? "（命中编码缓存，P7a-08）" : "");
        ctx.console(buffer);
    }
    if (!result.ok || result.text.empty()) {
        throw engine::NodeError("图片理解失败：" +
                                (result.error.empty() ? std::string("未取到文本") : result.error));
    }
    ctx.console("[图片理解] 完成：HTTP " + std::to_string(result.http_status) + "，输出 " +
                std::to_string(result.text.size()) + " 字节，耗时 " +
                std::to_string(static_cast<long long>(result.elapsed_ms)) + " ms");
    return result.text;
}
} // namespace aiwrite::nodes
