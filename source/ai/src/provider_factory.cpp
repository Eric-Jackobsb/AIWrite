#include "provider_factory.h"

#include "anthropic_provider.h"
#include "gemini_provider.h"
#include "log.h"

namespace aiwrite::ai {

std::vector<std::string> factory_protocols()
{
    return {"openai", "anthropic", "gemini", "deepseek-web"};
}

std::string factory_protocols_text()
{
    std::string text;
    for (const std::string& protocol : factory_protocols()) {
        if (!text.empty()) {
            text += " / ";
        }
        text += protocol;
    }
    return text;
}

bool factory_implements(const std::string& protocol)
{
    for (const std::string& item : factory_protocols()) {
        if (item == protocol) {
            return true;
        }
    }
    return false;
}

ProviderCreation create_provider(const ProviderSpec& spec, ProviderRuntimeConfig config)
{
    ProviderCreation out;
    // 能力快照 / 诊断字段一律对齐到**本条**（换条目即换能力，`PB2-12`）
    config.provider_id = spec.id;
    if (config.display.empty()) {
        config.display = spec.display;
    }
    const std::string protocol = spec.protocol.empty() ? std::string("openai") : spec.protocol;

    if (protocol == "openai") {
        out.provider = std::make_unique<OpenAiCompatProvider>(std::move(config));
        return out;
    }
    if (protocol == "anthropic") {
        out.provider = std::make_unique<AnthropicProvider>(std::move(config));
        return out;
    }
    if (protocol == "gemini") {
        out.provider = std::make_unique<GeminiProvider>(std::move(config));
        return out;
    }
    if (protocol == "deepseek-web") {
        if (!config.session_supplier) {
            out.error = "protocol=deepseek-web 需要**网页版会话**（本次调用未提供会话来源）——"
                        "该协议只能在已完成登录的运行上下文中使用";
            return out;
        }
        out.provider = std::make_unique<DeepSeekWebProvider>(std::move(config));
        return out;
    }
    if (protocol == "dom") {
        // DOM 生成需要**页面上下文**（选答案 / 差分基线 / 上传槽）⇒ 不经本工厂；
        // 节点在 mode=web 且 web.adapter=dom 时自动走 `dom_chat`（L3 / PB2-13）
        out.error = "protocol=dom 由通用 DOM 适配器承载（需要页面上下文）——"
                    "节点在 mode=web 且 web.adapter=dom 时自动走 DOM 路径，本工厂不建对象";
        return out;
    }

    // R9：表里有、程序没实现 ⇒ 明确报错（绝不静默回落到别的协议）
    out.unsupported_protocol = true;
    out.error = "未知 protocol「" + protocol + "」：本版本工厂已实现 " + factory_protocols_text() +
                "（另 `dom` 由 DOM 适配器承载）——请在 providers.json 里改用已实现协议，"
                "或等待该协议接入（表可以写，程序必须说清「还没实现」）";
    log::info("[Provider 工厂] 未实现 protocol：" + protocol + "（条目 " + spec.id + "）");
    return out;
}

} // namespace aiwrite::ai
