#pragma once

// ============================================================================
//  本地节点（N-01…N-09）—— **模块内契约**(ARCH-Z1 第 4 组)
//
//  * 只允许 `nodes/src/local_nodes*.cpp` 自己 include；**不是**公开 API（公开入口 = `nodes.h`）
//  * 逐字搬迁（零语义变化）：原 1242 行 = 11 个匿名命名空间公用函数 + 8 个节点执行器；
//    公用函数被「文本 / 图片 / 推理」三组执行器共同调用 ⇒ 收进 `nodes_detail` 并给出声明
//  * 拆分版图：
//      `local_nodes_helpers.cpp`  值文本化 / 图片路径收集 / 预处理 / 转义 / Key 解析 / 提供商文案 / 归属
//      `local_nodes.cpp`          文本输入 / 图片输入 / 提示词模板 / 文本合并 / 提供商配置 / 文本输出
//      `local_nodes_llm.cpp`      N-06 文本生成（官方 + 兼容 + 网页版 DOM 适配器）
//      `local_nodes_vlm.cpp`      N-07 图片理解（官方多模态 + 网页版上传后发送）
// ============================================================================

#include "nodes.h"             // engine::ExecutionContext / engine::EffectiveProvider
#include "config.h"            // Config（重试计划的全局回落）
#include "provider_resolve.h"  // M_patchB L1：EffectiveProvider
#include "provider_spec.h"     // ai::ProviderWebSpec
#include "image_preprocess.h"  // M_8nodes_extension L1-c：ai::ImagePreprocessParams
#include "webview_host.h"      // PW-01：web::WebOwner

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aiwrite::nodes {
namespace nodes_detail {

// ---- 推理节点公共：官方 / 兼容 API 的 Key 解析结果（原匿名命名空间内结构体）----
struct OfficialKeyResolution {
    std::string key;
    std::string source;
    std::string ref;
};

std::string              text_of(const nlohmann::json& value);
std::vector<std::string> collect_image_paths(const nlohmann::json& value);
std::vector<std::string> collect_directory_images(const std::string& directory);
ai::ImagePreprocessParams preprocess_params_from(const nlohmann::json& params);
std::string apply_image_preprocess(const std::vector<std::string>& in,
                                   const ai::ImagePreprocessParams& pre,
                                   engine::ExecutionContext& ctx,
                                   std::vector<std::string>* out, const std::string& tag);
std::string unescape(const std::string& text);
OfficialKeyResolution resolve_official_key(const nlohmann::json& params,
                                           const nlohmann::json& /*inputs*/,
                                           const engine::ExecutionContext& ctx,
                                           const std::string& tag,
                                           const engine::EffectiveProvider& effective);
engine::EffectiveProvider effective_provider_of(const engine::ExecutionContext& ctx);
void console_provider_line(const engine::ExecutionContext& ctx, const std::string& tag,
                           const engine::EffectiveProvider& effective, const std::string& mode);
web::WebOwner web_owner_of(const engine::ExecutionContext& ctx, const ai::ProviderWebSpec& site,
                           const std::string& provider_id);

// ---- 推理节点公共：重试计划（原 llm / vlm 两处 18 行逐字复制 · 去重专项）----
//  * 节点级 `retry_count>0` ⇒ 参数优先（未给的项回落到全局配置）
//  * 其它 ⇒ 全用全局 `config.toml [error]`
struct RetryPlan {
    bool enabled     = false;
    int  count       = 0;
    int  interval_ms = 0;
};
RetryPlan resolve_retry_plan(const nlohmann::json& params, const Config& runtime_config);

}  // namespace nodes_detail
}  // namespace aiwrite::nodes
