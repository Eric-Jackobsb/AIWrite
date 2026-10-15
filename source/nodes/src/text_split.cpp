#include "nodes.h"

#include "text_process_common.h"

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

// ============================================================================
//  M_node_expansion L2-01（v5 定稿 §4.3.5.1）：TextSplit 文本分块
//    split_mode: by_separator / by_length / by_paragraph / by_regex
//  边界：空输入 → 空 texts；分隔符不存在 → 单段（原文本）；max_length=0 → 报错（校验层拦）
// ============================================================================

namespace aiwrite::nodes {
namespace {

using nlohmann::json;
using textproc::text_of;
using textproc::unescape;

// 按分隔符拆分（Java/C# split 语义：结尾正好是分隔符 → 末尾补一个空段）
std::vector<std::string> split_by_separator(const std::string& text, const std::string& sep,
                                            bool keep_separator)
{
    std::vector<std::string> result;
    if (sep.empty()) {
        result.push_back(text);
        return result;
    }
    std::size_t pos = 0;
    while (true) {
        const std::size_t hit = text.find(sep, pos);
        if (hit == std::string::npos) {
            result.push_back(text.substr(pos));
            break;
        }
        std::string chunk = text.substr(pos, hit - pos);
        if (keep_separator) {
            chunk += sep;
        }
        result.push_back(chunk);
        pos = hit + sep.size();
        if (pos >= text.size()) {
            result.push_back(std::string()); // 结尾分隔符后无内容
            break;
        }
    }
    return result;
}

// 按最大长度拆分（overlap 运行时钳制到 max_length/2；按**码点**计长，不切坏中文）
std::vector<std::string> split_by_length(const std::string& text, int max_length, int overlap)
{
    const std::vector<std::string> chars = textproc::split_chars(text);
    std::vector<std::string>       result;
    if (chars.empty()) {
        return result;
    }
    const std::size_t max_len = static_cast<std::size_t>(std::max(1, max_length));
    const std::size_t overlap_capped =
        std::min(static_cast<std::size_t>(std::max(0, overlap)), max_len / 2);
    const std::size_t step = std::max<std::size_t>(1, max_len - overlap_capped);
    for (std::size_t i = 0; i < chars.size();) {
        std::string chunk;
        for (std::size_t j = i; j < chars.size() && j < i + max_len; ++j) {
            chunk += chars[j];
        }
        result.push_back(chunk);
        if (i + max_len >= chars.size()) {
            break;
        }
        i += step;
    }
    return result;
}

// 按段落拆分：以「一个或多个空行」为界（\n 开头可带空白）
std::vector<std::string> split_by_paragraph(const std::string& text, bool keep_separator)
{
    std::vector<std::string> result;
    const std::regex         sep_regex("\\n[ \\t]*\\n");
    std::size_t              pos = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), sep_regex), end; it != end; ++it) {
        const std::string chunk = text.substr(pos, it->position() - pos);
        result.push_back(keep_separator ? chunk + it->str() : chunk);
        pos = it->position() + it->length();
    }
    result.push_back(text.substr(pos));
    return result;
}

// 按正则拆分（匹配处为分隔位置；keep_separator → 分隔串保留在块尾）
std::vector<std::string> split_by_regex(const std::string& text, const std::string& pattern,
                                        bool keep_separator, std::string* error)
{
    std::vector<std::string> result;
    std::regex               regex;
    if (!textproc::make_regex(pattern, false, regex, error)) {
        return result;
    }
    std::size_t pos = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), regex), end; it != end; ++it) {
        const std::string chunk = text.substr(pos, it->position() - pos);
        result.push_back(keep_separator ? chunk + it->str() : chunk);
        pos = it->position() + it->length();
    }
    result.push_back(text.substr(pos));
    return result;
}

} // namespace

json execute_text_split(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    const std::string text = inputs.contains("text") ? text_of(inputs["text"]) : std::string();
    const std::string mode = params.value("split_mode", std::string("by_separator"));
    const bool        keep = params.value("keep_separator", false);

    if (text.empty()) {
        return json::array(); // 空输入 → 空 texts（part2 §4.3.5.1 边界；单输出端口直接返回值）
    }

    std::vector<std::string> chunks;
    if (mode == "by_length") {
        chunks = split_by_length(text, params.value("max_length", 2000),
                                 params.value("overlap", 0));
    }
    else if (mode == "by_paragraph") {
        chunks = split_by_paragraph(text, keep);
    }
    else if (mode == "by_regex") {
        std::string error;
        chunks = split_by_regex(text, params.value("regex_pattern", std::string()), keep, &error);
        if (!error.empty()) {
            throw engine::NodeError("文本分块：正则非法 —— " + error);
        }
    }
    else { // by_separator
        chunks = split_by_separator(text, unescape(params.value("separator", std::string("\n\n"))),
                                    keep);
    }

    if (chunks.size() == 1 && chunks[0] == text) {
        ctx.console("[文本分块] 未找到分隔，输出单段（原文本）");
    }
    json output = json::array();
    for (const std::string& chunk : chunks) {
        output.push_back(chunk);
    }
    return output; // 单输出端口 texts：直接返回数组（执行器约定）
}

} // namespace aiwrite::nodes
