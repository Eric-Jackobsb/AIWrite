#include "nodes.h"

#include "provider_spec.h"          // M_patchB L1：配置表（生效提供商 / 站点端点）
#include "provider_resolve.h"       // M_patchB L1：EffectiveProvider
#include "inference_provider.h"     // M_patchAB §3.1（B2-b · PB2-12）：能力门控（表驱动）
#include "deepseek_official_provider.h"
#include "cancel_token.h"           // M_patchAB §2.1（PB-02）：取消令牌 / 可中断等待
#include "config.h"                 // M_patchAB §2.6（PB-08 残项）：timeout.* / error.* 接线
#include "retry_policy.h"           // M_patchAB §2.6：可重试状态 / 退避 / 次数钳制（纯函数）
#include "deepseek_web_client.h"
#include "dom_web_client.h"         // L3（PB2-13）：通用 DOM 站点适配器
#include "log.h"
#include "session_store.h"
#include "webview_host.h"
#include "local_nodes_detail.h"     // ARCH-Z1：公用函数（nodes_detail）

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiwrite::nodes {

using namespace nodes_detail;  // 原匿名命名空间函数（同 TU 直接调用，名字查找不变）
using nlohmann::json;
json execute_llm_generate(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    // ---- M_patchB L1（PB2-05）：生效提供商来自配置表（默认地址 / 生效模型 / 凭据引用名）----
    const engine::EffectiveProvider effective = effective_provider_of(ctx);
    std::string mode  = params.value("mode", std::string("official"));
    std::string model = params.value("model", std::string("deepseek-chat"));
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        mode  = it->value("mode", mode);
        model = it->value("model", model);
    }
    if (effective.resolved) {
        mode  = effective.mode;  // 表优先（web 条目自动锁 web；老工作流参数行为不变）
        model = effective.model; // 表解析后的生效模型名（model_custom 优先，其次节点值，最后表内首选）
    }

    const std::string prompt_raw = inputs.contains("prompt") ? text_of(inputs["prompt"]) : "";
    if (prompt_raw.empty()) {
        throw engine::NodeError("文本生成：提示词为空（请连接 PromptTemplate 或直接填写 prompt）");
    }

    // ---- M_patchAB §3.1（`B2-b` · `PB2-12`）：**能力驱动**（门控与提示文案来自配置表）----
    //  * 取代此前散落的 `mode == "web"` 硬编码判断：视觉 / seed / 系统角色由表 `capabilities` 决定
    //  * 无表条目时按调用通道取能力（`web` → 内置网页版协议的能力），守 `I2` 既有行为
    const std::string        system_prompt = params.value("system_prompt", std::string());
    const int                seed_param    = params.value("seed", 0);
    const ai::ProviderCaps   caps          = ai::capsForCall(effective.spec, mode == "web");
    ai::GenerateParams       gate_params;
    gate_params.system_prompt = system_prompt;
    gate_params.seed          = seed_param;
    const ai::CapabilityGate gate = ai::capability_gate(caps, gate_params, false);
    for (const std::string& warning : gate.warnings) {
        ctx.console("[文本生成] " + warning);
    }
    if (!gate.blocking.empty()) {
        throw engine::NodeError("文本生成：" + gate.blocking.front());
    }
    std::string prompt = prompt_raw;
    if (gate.system_prompt_inlined) {
        // 该通道没有 system 角色槽位 ⇒ 前置拼接（与改造前网页版口径逐字一致）
        prompt = system_prompt + "\n\n" + prompt;
    }

    if (mode != "web") {
        // PB-05 + M_patchB L1：官方 / 兼容 API —— 端点·认证·超时·env 名按**配置表**取值
        ai::OfficialChatRequest official;
        if (effective.spec != nullptr) {
            official.options         = ai::providerOptionsFrom(*effective.spec);
            official.image_max_bytes = effective.spec->limits.image_max_bytes;
        }
        official.api_base         = effective.api_base; // 工作流参数优先 → 表默认 → 引擎默认
        official.options.api_base = effective.api_base;
        official.model            = model;
        official.system_prompt    = system_prompt;
        official.prompt           = prompt_raw;
        official.temperature      = params.value("temperature", 0.7);
        official.max_tokens       = params.value("max_tokens", 2048);
        official.top_p            = params.value("top_p", 1.0);
        official.seed             = params.value("seed", 0); // M_rerun：新 seed 重跑
        // M_8nodes_extension L1-b：公共参数面采样项随请求发送（非零 / 非空才写进 body）
        official.top_k             = params.value("top_k", 0);
        official.stop              = params.value("stop", std::vector<std::string>{});
        official.frequency_penalty = params.value("frequency_penalty", 0.0);
        official.presence_penalty  = params.value("presence_penalty", 0.0);
        // M_8nodes_extension L1-e（§4.6）：JSON 输出 + 多候选（非默认才写；网页版忽略）
        official.response_format = params.value("response_format", std::string());
        official.n               = params.value("n", 1);
        if (official.n > 1) {
            ctx.console("[文本生成] 提示：候选数 n=" + std::to_string(official.n) +
                        "，text 端口输出**第一个**候选（多候选端口 texts 待后续）");
        }
        // PB-06：Key 三级优先级 —— 节点参数 → 环境变量（表内 env 名按序）→ provider 来源节点凭据库
        official.api_key = resolve_official_key(params, inputs, ctx, "[文本生成]", effective).key;
        if (official.api_key.empty()) {
            const std::string env_text =
                effective.spec != nullptr && !effective.spec->env_names.empty()
                    ? effective.spec->env_names.front()
                    : std::string("DEEPSEEK_API_KEY");
            throw engine::NodeError("文本生成（官方 API）缺少 API Key：请在「提供商配置」填写 API Key，"
                                    "或设置环境变量 " + env_text + "（凭据引用名：" +
                                    (effective.key_ref.empty() ? std::string("未设置") : effective.key_ref) +
                                    "）");
        }
        console_provider_line(ctx, "[文本生成]", effective, "official");
        // PB-02（真取消）：把本运行的取消令牌交给 HTTP 层（置位 ⇒ 主动关 socket）
        official.cancel = ctx.cancel_token;

        // ---- M_patchAB §2.6（`PB-08` 残项）：把 `config.toml [timeout] / [error]` 接上线 ----
        //  * 超时：**表未声明**（`limits.* <= 0`）时才用配置值（表优先，PB2-04 口径不变）
        //  * 重试：`error.retry_enabled / retry_count / retry_interval_ms` —— 只对**可重试**失败
        //    （429 / 5xx / 网络超时）重试；凭证 / 余额 / 请求不合法**不重试**（避免加剧风控）
        //  * ---- M_8nodes_extension L1-a（§3.3 / §5.3）：**节点级覆盖优先**（0 / 空 = 回落全局）----
        const aiwrite::Config& runtime_config = aiwrite::appConfig();
        const int              override_connect_ms     = params.value("timeout_connect_ms", 0);
        const int              override_first_byte_ms  = params.value("timeout_first_byte_ms", 0);
        const int              override_stream_idle_ms = params.value("timeout_stream_idle_ms", 0);
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
        if (override_stream_idle_ms > 0) {
            ctx.console("[文本生成] 提示：流式空闲超时（timeout_stream_idle_ms）当前由全局配置管理，"
                        "本节点覆盖项保留（官方 API 走连接 / 首字节超时）");
        }
        // 重试参数：节点覆盖优先；retry_count=0 → 全部用全局（含开关 / 次数 / 间隔）
        const nodes_detail::RetryPlan retry = nodes_detail::resolve_retry_plan(params, runtime_config);
        const int max_retries = ai::clampRetryCount(retry.enabled, retry.count);
        ctx.console("[文本生成] 官方 API 请求：" + official.model + "，提示词 " +
                    std::to_string(official.prompt.size()) + " 字节" +
                    (system_prompt.empty() ? "" : "（含系统提示词）") +
                    (max_retries > 0 ? ("；重试上限 " + std::to_string(max_retries) + " 次") : ""));
        ai::OfficialChatResult official_result;
        for (int attempt = 0;; ++attempt) {
            official_result = ai::officialChat(official);
            if (official_result.ok && !official_result.text.empty()) {
                break;
            }
            // 取消优先：取消导致的失败**不重试**（归因由执行器记「[取消]」）
            if (official_result.cancelled || ctx.is_cancelled()) {
                throw engine::NodeError(
                    official_result.error.empty()
                        ? std::string("文本生成（官方 API）已取消（用户点了「停止」）")
                        : official_result.error);
            }
            if (attempt >= max_retries ||
                !ai::httpStatusRetryable(official_result.http_status)) {
                break; // 不可重试 / 已用尽
            }
            const int delay_ms = ai::retryDelayMs(retry.interval_ms, attempt);
            ctx.console("[文本生成] 第 " + std::to_string(attempt + 1) + " 次重试（HTTP " +
                        std::to_string(official_result.http_status) + "，等 " +
                        std::to_string(delay_ms) + " ms；可重试失败 = 429 / 5xx / 网络超时）");
            if (ai::sleepInterruptible(ctx.cancel_token.get(), delay_ms)) {
                throw engine::NodeError("文本生成（官方 API）已取消（用户点了「停止」）：重试等待被中断");
            }
        }
        if (!official_result.ok || official_result.text.empty()) {
            throw engine::NodeError("文本生成（官方 API）失败：" +
                                    (official_result.error.empty() ? std::string("未取到文本")
                                                                   : official_result.error));
        }
        ctx.console("[文本生成] 官方 API 完成：HTTP " + std::to_string(official_result.http_status) +
                    "，输出 " + std::to_string(official_result.text.size()) + " 字节，耗时 " +
                    std::to_string(static_cast<long long>(official_result.elapsed_ms)) + " ms");
        return official_result.text;
    }

    // 网页版：凭证（Cookie + userToken）只在内存，且**按站点**归档（PB2-18）
    //  * 站点**只能**来自生效条目自己的 `web` 段（PB2-22 / 决策 D-22② / 不变量 I14：**不**回落内置默认站点）
    //  * 站点与当前登录窗口不一致时，ensure_session 按串行策略重开窗口
    //    （页面内 PoW 求解依赖该站点页面；决策 D-20）
    const std::string site_error = ai::webSiteError(effective.spec);
    if (!site_error.empty()) {
        throw engine::NodeError("文本生成（网页版）不可用：" + site_error);
    }
    const ai::ProviderWebSpec site      = ai::strictWebSpecFor(effective.spec);
    const std::string        site_id    = ai::strictWebProviderIdFor(effective.spec);
    const std::string        site_label = site.login_url;

    // ---- L3（PB2-13）：通用 DOM 适配器（选择器驱动）—— 站点 = 纯数据，程序里无站点专有 C++ ----
    if (site.adapter == "dom") {
        // M8-37（零开关）：生成字段（选择器 / 发送 / 取答案）**未回填不再拦路** ——
        //  dom_chat 会在运行期自动识别（输入框打分 / 答案容器基线差分），识别结果只打印、不回写配置
        if (ai::webLoginOnly(effective.spec)) {
            ctx.console("[文本生成] 站点「" + site_label +
                        "」的生成字段未回填（web.input_selector / send / answer_selector）—— "
                        "本次运行将**自动识别**（结果打印在下方；如需固化可用 --web-dom-dump "
                        "--provider " + site_id + " 取建议选择器写进 providers.json）");
        }
        const std::string dom_field_warnings = ai::webSiteFieldWarnings(effective.spec);
        if (!dom_field_warnings.empty()) {
            ctx.console("[文本生成] " + dom_field_warnings); // 决策 D-26：可回落 + 警告
        }
        ctx.console("[文本生成] 站点：" + site_label + "（适配器 dom · 选择器驱动；提示词 " +
                    std::to_string(prompt.size()) + " 字节）");
        ai::DomChatRequest dom_request;
        dom_request.prompt      = prompt;
        dom_request.site        = site;
        dom_request.provider_id = site_id;
        dom_request.owner       = web_owner_of(ctx, site, site_id); // M_patchC PW-01：归属
        // M_8nodes_extension L1-a（§3.3 增量③）：web 线总超时 = 节点覆盖优先
        //  * 用户显式填了连接 / 首字节超时覆盖时才改变（取较大者，**不缩短**既有 180000 行为）
        dom_request.timeout_ms = std::max(
            dom_request.timeout_ms,
            std::max(params.value("timeout_connect_ms", 0), params.value("timeout_first_byte_ms", 0)));
        const ai::DomChatResult dom = ai::domChat(dom_request);
        if (!dom.ok) {
            throw engine::NodeError("文本生成（网页版 · DOM 适配器）失败：" + dom.error);
        }
        if (dom.input_auto) {
            ctx.console("[文本生成] 自动识别输入框：" + dom.input_used); // M8-37：只打印不落盘
        }
        if (dom.answer_auto) {
            ctx.console("[文本生成] 自动识别答案容器：" + dom.answer_fingerprint); // M8-37
        }
        if (!dom.warning.empty()) {
            ctx.console("[文本生成] " + dom.warning); // R13：如实提示（可能仍在生成中）
        }
        ctx.console("[文本生成] DOM 适配器完成：输出 " + std::to_string(dom.text.size()) +
                    " 字节，轮询 " + std::to_string(dom.polls) + " 次 / " +
                    std::to_string(dom.elapsed_ms) + " ms" +
                    (dom.steps.empty() ? "" : ("｜" + dom.steps)));
        return dom.text;
    }

    if (!ai::webAdapterImplemented(site.adapter)) {
        // R12：表里有、程序没实现 → 运行时**明确报错**（登录 / 协议探测仍可用）
        throw engine::NodeError("文本生成（网页版）不可用：站点「" + site_label + "」的适配器 " +
                                site.adapter +
                                " 本版本尚未实现（已实现：builtin:deepseek、dom）"
                                "——该站点可用于登录 / 协议探测");
    }
    const std::string field_warnings = ai::webSiteFieldWarnings(effective.spec);
    if (!field_warnings.empty()) {
        ctx.console("[文本生成] " + field_warnings); // 决策 D-26：可回落 + 警告
    }
    web::LoginRequest        boot     = web::boot_login_request(site, site_id);
    boot.owner = web_owner_of(ctx, site, site_id); // M_patchC（PW-02）：归属贯穿（内置协议路径）
    const std::string        site_key = web::login_request_site(boot);

    if (!web::SessionStore::instance().hasToken(site_key) ||
        !web::windowOnSite(boot.owner, site_key)) {
        std::string boot_error;
        ctx.console("[文本生成] 正在准备网页版会话（站点 " + site_label + "；首次约数秒）…");
        if (!web::ensureSession(boot, 25000, &boot_error)) {
            throw engine::NodeError("文本生成（网页版）不可用：" + boot_error);
        }
    }
    const web::Session session = web::SessionStore::instance().snapshot(site_key);

    ai::WebChatRequest request;
    request.prompt           = prompt;
    request.model_type       = (model == "expert") ? "expert" : "default";
    request.thinking_enabled = (model == "deepseek-reasoner");
    request.cancel           = ctx.cancel_token; // PB-02（真取消）：挑战 / 生成两段都可中断
    // M_patchB L1（PB2-05 / PB2-17）：站点端点来自**生效条目**（PB2-22：站点不可用时上面已抛错，不会用内置默认）
    if (!site.endpoints.host.empty()) {
        request.endpoints = site.endpoints;
    }
    console_provider_line(ctx, "[文本生成]", effective, "web");
    if (effective.resolved && !effective.is_web()) {
        ctx.console("[文本生成] 提示：当前「提供商」不是网页版条目，站点参数用内置默认"
                    "（DeepSeek 网页版）；如需按表配置站点，请在「提供商配置 → 提供商」选择 "
                    "一个网页版条目（如 deepseek-web）");
    }
    else if (effective.is_web()) {
        ctx.console("[文本生成] 站点：" + site_label + "（适配器 " +
                    (site.adapter.empty() ? std::string("builtin") : site.adapter) + "）");
    }
    // PB-03-min：把执行器的增量回调接到网页版 SSE（边收边吐 → UI 逐字呈现）
    if (ctx.on_delta) {
        request.on_delta = ctx.on_delta;
    }

    // 采样参数：该通道的请求体没有这些槽位（协议口径，`deepseek-web` 的请求体只有
    // prompt/model_type/thinking/search）——改过就提示一次；seed 由上面的**能力门控**统一提示
    const double temperature = params.value("temperature", 0.7);
    const int    max_tokens  = params.value("max_tokens", 2048);
    const double top_p       = params.value("top_p", 1.0);
    if (temperature != 0.7 || max_tokens != 2048 || top_p != 1.0) {
        ctx.console("[文本生成] 提示：本通道不支持 temperature / max_tokens / top_p，已忽略"
                    "（这些参数仅对官方 API 生效）");
    }

    ctx.console("[文本生成] 网页版请求：模型 " + model + "，提示词 " +
                std::to_string(prompt.size()) + " 字节" +
                (system_prompt.empty() ? "" : "（含系统提示词 " +
                                                 std::to_string(system_prompt.size()) + " 字节）"));
    const ai::WebChatResult result = ai::webChat(session, request);
    if (!result.ok || result.text.empty()) {
        throw engine::NodeError("文本生成（网页版）失败：" +
                                (result.error.empty() ? std::string("未取到文本") : result.error));
    }

    log::info("[文本生成] 网页版完成：HTTP " + std::to_string(result.http_status) + "，输出 " +
              std::to_string(result.text.size()) + " 字节");
    ctx.console("[文本生成] 完成，输出 " + std::to_string(result.text.size()) + " 字节");
    return result.text;
}

} // namespace aiwrite::nodes
