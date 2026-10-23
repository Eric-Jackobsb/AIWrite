// ARCH-Z1 去重专项（v1.14）：`provider_common.h` 的实现（Anthropic / Gemini 共用片段）
//  * 见 `anthropic_provider.cpp` / `gemini_provider.cpp` 的调用点；本文件不含任何厂商协议知识
#include "provider_common.h"

#include "deepseek_official_provider.h" // `encode_image_data_url`（体积上限 + 可操作错误）

namespace aiwrite::ai {

bool prepare_image_data_urls(const GenerateParams& params, std::vector<std::string>* urls,
                             GenerateResult* result)
{
    urls->clear();
    for (const std::string& path : params.images) {
        std::string       encode_error;
        const std::string data_url =
            encode_image_data_url(path, params.image_max_bytes, &encode_error);
        if (data_url.empty()) {
            result->error = "图片准备失败：" + (encode_error.empty() ? path : encode_error);
            return false;
        }
        urls->push_back(data_url);
        result->encoded_bytes += data_url.size();
    }
    return true;
}

bool map_http_failure(const HttpJsonResult& http, const std::string& provider_label,
                      GenerateResult* result)
{
    result->http_status = http.http_status;
    result->elapsed_ms  = http.elapsed_ms;
    result->raw_head    = http.raw_head;
    if (http.cancelled) {
        result->cancelled = true;
        result->error     = http.error;
        return true;
    }
    if (!http.ok) {
        result->error = provider_label + " 调用失败：" + http.error;
        return true;
    }
    return false;
}

}  // namespace aiwrite::ai
