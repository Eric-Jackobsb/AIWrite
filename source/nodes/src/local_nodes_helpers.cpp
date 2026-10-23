#include "nodes.h"

#include "provider_spec.h"          // M_patchB L1：配置表（生效提供商 / 站点端点）
#include "provider_resolve.h"   // M_patchB L1：EffectiveProvider
#include "inference_provider.h" // M_patchAB §3.1（B2-b · PB2-12）：能力门控（表驱动）

#include "deepseek_official_provider.h"
#include "cancel_token.h"     // M_patchAB §2.1（PB-02）：取消令牌 / 可中断等待
#include "credential.h"
#include "config.h"           // M_patchAB §2.6（PB-08 残项）：timeout.* / error.* 接线
#include "retry_policy.h"     // M_patchAB §2.6：可重试状态 / 退避 / 次数钳制（纯函数）
#include "asset_store.h"         // P7a-04：资源令牌解析（统一资源目录）
#include "paths.h"               // P7a-01/02：image 端口多值 + 多选路径解析
#include "deepseek_web_client.h"
#include "dom_web_client.h"         // L3（PB2-13）：通用 DOM 站点适配器
#include "upload_registry.h" // M8-14：站点上传模板（网页版图片上传 → 再发送）
#include "image_preprocess.h" // M_8nodes_extension L1-c（§4.2）：图片预处理（裁剪 / 缩放）
#include "image_decode.h" // L1-c（§4.2）：目录批量 —— 内容嗅探过滤（sniffImage）
#include "log.h"
#include "session_store.h"
#include "webview_host.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "local_nodes_detail.h"  // ARCH-Z1：模块内契约（10 个公用入口 + Key 解析结果）

namespace aiwrite::nodes {
namespace nodes_detail {
using nlohmann::json;

// 端口值文本化（Text/Number/Bool/其它一律转可读文本）
std::string text_of(const json& value)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

// P7a-01：图片输入值统一展平为路径列表
//  * 形态可能为：单值字符串 / 多值数组（多条边） / 嵌套数组（多选图片输入 → 变长端口）
//  * 递归展平、忽略空值；顺序 = 边的插入序（外层）× 多选顺序（内层）
std::vector<std::string> collect_image_paths(const json& value)
{
    std::vector<std::string> paths;
    if (value.is_array()) {
        for (const json& item : value) {
            const std::vector<std::string> nested = collect_image_paths(item);
            paths.insert(paths.end(), nested.begin(), nested.end());
        }
        return paths;
    }
    const std::string path = text_of(value);
    if (!path.empty()) {
        // P7a-04：防御 —— 上游若直接给出资源令牌（正常路径由「图片输入」已解析），这里也解析一次
        if (aiwrite::asset::isToken(path)) {
            bool             missing = false;
            std::string      error;
            const std::string local = aiwrite::asset::toLocalPath(path, &missing, &error);
            if (!missing && !local.empty()) {
                paths.push_back(local);
                return paths;
            }
        }
        paths.push_back(path);
    }
    return paths;
}

// ============================================================================
//  M_8nodes_extension L1-c（§4.2 / §4.7）：目录批量 + 图片预处理（N-02 / N-07 共用）
// ============================================================================

// 目录批量：扫描目录下**所有**图片（扩展名白名单 + 内容嗅探过滤；文件名排序保证确定性）
//  * 内容嗅探为 Unknown（扩展名骗人，如 .png 实为文本）**不放行** —— 宁缺毋滥
std::vector<std::string> collect_directory_images(const std::string& directory)
{
    std::vector<std::string>         result;
    std::error_code                  ec;
    static const std::vector<std::string> kImageExts = {".png", ".jpg", ".jpeg", ".bmp",
                                                        ".webp", ".gif"};
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (std::find(kImageExts.begin(), kImageExts.end(), ext) == kImageExts.end()) {
            continue;
        }
        const utils::ImageMagic magic = utils::sniffImage(entry.path().string());
        if (magic.format == utils::ImageFormat::Unknown) {
            continue;
        }
        result.push_back(entry.path().string());
    }
    std::sort(result.begin(), result.end()); // 确定性顺序（按文件名）
    return result;
}

// 预处理参数（N-02 / N-07 共用同名参数；JSON → 结构体）
ai::ImagePreprocessParams preprocess_params_from(const json& params)
{
    ai::ImagePreprocessParams pre;
    pre.resize_mode       = params.value("resize_mode", std::string("none"));
    pre.resize_max_width  = params.value("resize_max_width", 0);
    pre.resize_max_height = params.value("resize_max_height", 0);
    pre.crop_enabled      = params.value("crop_enabled", false);
    pre.crop_x            = params.value("crop_x", 0);
    pre.crop_y            = params.value("crop_y", 0);
    pre.crop_w            = params.value("crop_w", 0);
    pre.crop_h            = params.value("crop_h", 0);
    return pre;
}

// 对图片列表应用预处理（读取后、编码前；文档 §4.2 执行顺序）；失败返回可操作错误
//  * 无参数变化时 preprocess_image 零开销（原路径原样返回，不复制）
std::string apply_image_preprocess(const std::vector<std::string>& in,
                                   const ai::ImagePreprocessParams& pre,
                                   engine::ExecutionContext& ctx,
                                   std::vector<std::string>* out, const std::string& tag)
{
    out->clear();
    for (const std::string& image : in) {
        const ai::PreprocessOutcome outcome = ai::preprocess_image(image, pre);
        if (!outcome.ok) {
            return outcome.error;
        }
        if (outcome.changed) {
            ctx.console(tag + " 预处理：" + image + " → " + outcome.note);
        }
        out->push_back(outcome.path);
    }
    return {};
}

// 支持 \n \t \r \\ 转义（分隔符 / 模板参数的习惯写法）
std::string unescape(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            result += text[i];
            continue;
        }
        switch (text[++i]) {
        case 'n': result += '\n'; break;
        case 't': result += '\t'; break;
        case 'r': result += '\r'; break;
        case '\\': result += '\\'; break;
        default:
            result += '\\';
            result += text[i];
            break;
        }
    }
    return result;
}

// ---- 推理节点公共：官方 / 兼容 API 的 Key 解析（PB-06 三级优先级 + 首次自动入库）----
//  1) 节点参数 api_key → 2) 环境变量（**表内 env 名按序**；空列表 = DEEPSEEK_API_KEY）
//  3) provider 输入来源「提供商配置」节点的 api_key / api_key_ref（凭据库）
//  * M_patchB L1：env 名与引用名默认值来自配置表（effective），换厂商不再串味
//  * tag：Console 前缀（"[文本生成]" / "[图片理解]"），保证既有日志文案不变
//  * 返回空 key = 未配置（由调用方给出可操作错误）

OfficialKeyResolution resolve_official_key(const json& params, const json& /*inputs*/,
                                           const engine::ExecutionContext& ctx,
                                           const std::string& tag,
                                           const engine::EffectiveProvider& effective)
{
    OfficialKeyResolution resolution;
    const std::vector<std::string> env_names =
        effective.spec != nullptr ? effective.spec->env_names : std::vector<std::string>();

    const std::string param_key = params.value("api_key", std::string());
    if (!param_key.empty()) {
        resolution.key    = param_key;
        resolution.source = "node";
    }
    else {
        resolution.key = ai::resolveApiKey(std::string(), env_names);
        if (!resolution.key.empty()) {
            resolution.source = "env";
        }
    }
    if (!resolution.key.empty() || ctx.graph == nullptr || ctx.current_node_id.empty()) {
        return resolution;
    }
    const engine::Edge* edge = ctx.graph->findEdgeIntoInput(ctx.current_node_id, "provider");
    if (edge == nullptr) {
        return resolution;
    }
    const engine::Node* source_node = ctx.graph->findNode(edge->from_node);
    if (source_node == nullptr) {
        return resolution;
    }
    std::string param_key_from_config;
    if (const engine::Param* key_param = source_node->findParam("api_key")) {
        param_key_from_config = key_param->text();
    }
    if (const engine::Param* ref_param = source_node->findParam("api_key_ref")) {
        resolution.ref = ref_param->text();
    }
    if (resolution.ref.empty()) {
        resolution.ref = effective.key_ref; // 表内默认引用名（如 brain-ai/zhipu）
    }
    const utils::ResolvedSecret resolved =
        utils::resolveSecret(param_key_from_config, resolution.ref);
    resolution.key    = resolved.key;
    resolution.source = resolved.source;
    if (resolved.key.empty()) {
        return resolution;
    }
    ctx.console(tag + " 凭据来源=" + resolved.source + "（长度 " +
                std::to_string(resolved.key.size()) + "）");
    if (resolved.source == "node" && !resolution.ref.empty()) {
        std::string save_error;
        if (utils::saveCredential(resolution.ref, resolved.key, &save_error)) {
            ctx.console(tag + " 已把 API Key 存入凭据库（ref=" + resolution.ref + "），下次无需再填");
        }
        else {
            ctx.console(tag + " 凭据库保存失败：" + save_error);
        }
    }
    return resolution;
}

// 生效提供商（M_patchB L1 / PB2-05）：由「当前节点 + provider 输入连线」解析，含配置表默认值
engine::EffectiveProvider effective_provider_of(const engine::ExecutionContext& ctx)
{
    if (ctx.graph == nullptr || ctx.current_node_id.empty()) {
        return {};
    }
    const engine::Node* node = ctx.graph->findNode(ctx.current_node_id);
    if (node == nullptr) {
        return {};
    }
    return engine::resolveEffectiveProvider(*ctx.graph, *node);
}

// Console 一行：让用户看清「到底用了哪家、走哪条路、地址是什么」
void console_provider_line(const engine::ExecutionContext& ctx, const std::string& tag,
                           const engine::EffectiveProvider& effective, const std::string& mode)
{
    std::string text = tag + " 提供商=" + effective.display + "（来源 " +
                       (effective.spec_origin.empty() ? std::string("未在表内")
                                                      : effective.spec_origin) +
                       "）/ 模式=" + mode;
    if (!effective.api_base.empty()) {
        text += " / 地址=" + effective.api_base;
    }
    if (!effective.model.empty()) {
        text += " / 模型=" + effective.model;
    }
    ctx.console(text);
}

// M_patchC（`PW-01` · 不变量 `I24` / `I26`）：**归属** —— 把「运行会话 + 当前节点 + 站点」打包
//  * `run_id` / `node_id` 来自执行器（只读运行态）；`site_key` = 该站点 origin（`site_key_of`）
//  * 空 `run_id` / `node_id`（没有执行器上下文的调用点）⇒ 未标注，行为与改造前一致
web::WebOwner web_owner_of(const engine::ExecutionContext& ctx, const ai::ProviderWebSpec& site,
                           const std::string& provider_id)
{
    web::WebOwner owner;
    owner.run_id   = ctx.run_id;
    owner.node_id  = ctx.current_node_id;
    owner.site_key = web::login_request_site(web::boot_login_request(site, provider_id));
    return owner;
}

}  // namespace nodes_detail
}  // namespace aiwrite::nodes
