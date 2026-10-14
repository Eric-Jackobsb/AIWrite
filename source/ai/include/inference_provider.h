#pragma once

// ============================================================================
//  统一推理抽象（M_patchAB §3.1 · `B2-b` L2 底座 · `PB2-08` / `PB2-12`）
//
//  设计（拍板依据：`M_patchB` §3 L2 + 决策 `D-09`）：
//    * **一份接口**：`InferenceProvider{name, protocol, caps, generate}` —— 换协议 = 换实现，
//      节点侧只认这一份接口（新增厂商 = 表里加一条 + 工厂加一个分派分支）
//    * **不新开** `generateStream` / `generateWithImage` 虚函数（`D-09`）：
//      流式用 `GenerateParams::on_delta`（本版本暂停，槽位保留），图片用 `images`
//      —— 避免虚函数空转与「两个真值来源」
//    * **能力来自表**（`PB2-12`）：`ProviderCaps{vision,seed,system_role,stream}` 由
//      `providers.json` 的 `capabilities` 解析（`ProviderSpec::caps`），节点不再硬编码判断
//    * 本头文件**不含任何厂商字段**：各协议的请求体构造在各自适配器里（纯函数 → `V-13`）
// ============================================================================

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cancel_token.h"               // `PB-02`：协作式取消
#include "deepseek_official_provider.h" // `ProviderOptions` + `official_chat`（openai 兼容）
#include "provider_spec.h"              // `ProviderCaps` / `ProviderWebSpec` / `ProviderSpec`
#include "web_owner.h"                  // `M_patchC` PW-01：网页通道归属（纯数据）

namespace aiwrite::web {
struct Session; // 前置声明：本头文件不拉 web 模块
} // namespace aiwrite::web

namespace aiwrite::ai {

// ---------------------------------------------------------------- 入参 / 出参 --

// 一次生成的**全部输入**（协议无关）
struct GenerateParams {
    std::string              system_prompt;      // system_role=false 的协议会被**前置拼接**
    std::string              prompt;
    std::vector<std::string> images;             // 本地路径（按序）
    std::size_t image_max_bytes = 8u * 1024u * 1024u;
    double      temperature     = 0.7;
    int         max_tokens      = 2048;
    double      top_p           = 1.0;
    int         seed            = 0;             // >0 且 caps.seed 才随请求发送（M_rerun）
    // 流式（`PB-03` 数据源暂停）：保留槽位，本版本只在**整段到达**后回调一次或不回调
    std::function<void(const std::string&)> on_delta;
    // 0 = 用 provider 的 options 超时（表驱动）
    int connect_timeout_s = 0;
    int read_timeout_s    = 0;
    std::shared_ptr<const CancelToken> cancel;
};

struct GenerateResult {
    bool        ok          = false;
    int         http_status = 0;
    std::string text;
    std::string error;
    std::string raw_head;   // 诊断：站点原始响应前若干字符
    double      elapsed_ms  = 0.0;
    bool        cancelled   = false; // 取消 ≠ 站点 / 网络错误（`PB-02` 口径）
    // 多模态编码指标（与 `OfficialChatResult` 同口径；供 Console 打印）
    std::size_t encoded_bytes = 0;
    double      encode_ms     = 0.0;
    // 诊断：本次走的是哪条协议 / 哪个实现（Console「走了哪条路」一行）
    std::string protocol;
    std::string provider_name;
};

// ---------------------------------------------------------------- 统一接口 --

class InferenceProvider {
public:
    virtual ~InferenceProvider() = default;

    virtual std::string   name() const     = 0; // 实现名，例 "openai-compat"
    virtual std::string   protocol() const = 0; // 表里的 protocol，例 "anthropic"
    virtual ProviderCaps  caps() const     = 0; // 来自表 capabilities（`PB2-12`）
    virtual GenerateResult generate(const GenerateParams& params) = 0;
};

// ---------------------------------------------------------------- 运行期配置 --

// 由「生效条目 + 节点参数」解析出的运行期配置（工厂据此建 provider）
struct ProviderRuntimeConfig {
    std::string api_base;    // 生效地址（空 = 用 options.api_base / 协议默认）
    std::string model;       // 生效模型名
    std::string api_key;     // 已解析（空 = 无凭证；auth_style=none 时合法）
    ProviderOptions options; // 表驱动：chat_path / auth_style / auth_header / env / 超时
    std::string console_tag; // 日志前缀，例 "[文本生成]"（保持既有日志文案）
    std::string provider_id; // 条目 id（诊断）
    std::string display;     // 显示名（诊断）
    // ---- 网页版通道（protocol = deepseek-web）----
    ProviderWebSpec web;                            // 站点端点（表）
    std::function<web::Session()> session_supplier; // 会话来源（节点 / CLI 各自提供）
    web::WebOwner owner;                            // `M_patchC` PW-01：归属（贯穿日志与断言）
};

// ---------------------------------------------------------------- 能力门控 --

// 能力驱动的**门控结果**（`PB2-12`）：纯函数 ⇒ 节点 / 界面 / 自检共用同一判据
struct CapabilityGate {
    // 非空 ⇒ 本次调用**必定失败**：节点应直接报错（不发起请求、不静默降级）
    std::vector<std::string> blocking;
    // 提示（不拦路）：能力项被忽略 / 文案降级，如实告知
    std::vector<std::string> warnings;
    // system_role=false ⇒ 调用方应把系统提示词**前置拼接**进用户提示词
    bool system_prompt_inlined = false;
};

CapabilityGate capability_gate(const ProviderCaps& caps, const GenerateParams& params,
                               bool has_images);

// 表 capabilities → 运行期能力快照
//  * `spec == nullptr`（无表条目，例：临时自定义地址）⇒ **宽松默认**（不拦任何能力，
//    与改造前行为一致：老工作流不受影响）——不变量 `I19`
ProviderCaps caps_from_spec(const ProviderSpec* spec);
ProviderCaps permissive_caps();

// 本次调用的能力快照（节点 / 界面统一入口 `PB2-12`）：
//  * `spec != nullptr` → 用**本条**的 `capabilities`
//  * `spec == nullptr` + `web_mode` → 内置网页版协议（`deepseek-web`）的能力
//    （守 `I2`：老工作流「mode=web 无表条目」的系统提示词前置拼接行为不变）
//  * 其它 → 宽松默认
ProviderCaps caps_for_call(const ProviderSpec* spec, bool web_mode);

// 该协议是否需要 API Key（`auth_style=none` ⇒ 不需要）
bool protocol_needs_key(const ProviderOptions& options);

// ---------------------------------------------------------------- 共用请求件 --

// 认证头 + `extra_headers` + `Accept: application/json`（三条 JSON 协议共用；纯函数）
std::vector<std::pair<std::string, std::string>>
common_json_headers(const ProviderOptions& options, const std::string& api_key);

// `data:<mime>;base64,<...>` → (mime, base64)；非 data URL / 缺 base64 / 空 mime ⇒ false
//  * 各协议的图片块形状不同（Anthropic `source{type:base64,media_type,data}`；
//    Gemini `inline_data{mime_type,data}`）⇒ 拆一次、各自组装（纯函数 → `V-13`）
bool split_data_url(const std::string& data_url, std::string* mime, std::string* base64);

// ---------------------------------------------------------------- 内置实现 --

// openai 兼容（`deepseek` / `zhipu` / `siliconflow` / `ollama` / `openrouter` / `openai` / 自定义）
//  * 复用既有 `official_chat`（含图片 data URL 编码缓存、错误分类、取消语义）⇒ 行为逐字不变
class OpenAiCompatProvider final : public InferenceProvider {
public:
    explicit OpenAiCompatProvider(ProviderRuntimeConfig config);
    std::string    name() const override;
    std::string    protocol() const override;
    ProviderCaps   caps() const override;
    GenerateResult generate(const GenerateParams& params) override;

private:
    ProviderRuntimeConfig config_;
    ProviderCaps          caps_ = permissive_caps();
};

// 内置网页版站点（`deepseek-web`）：收编既有 `web_chat()`（PoW 求解 + SSE 解析）
//  * 对外行为与 `--web-chat` **逐字不变**（不变量 `I2`）：提示词 / 模式 / 思考 / 联网开关同口径
class DeepSeekWebProvider final : public InferenceProvider {
public:
    explicit DeepSeekWebProvider(ProviderRuntimeConfig config);
    std::string    name() const override;
    std::string    protocol() const override;
    ProviderCaps   caps() const override;
    GenerateResult generate(const GenerateParams& params) override;

private:
    ProviderRuntimeConfig config_;
    ProviderCaps          caps_ = permissive_caps();
};

} // namespace aiwrite::ai
