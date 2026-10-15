#include "nodes.h"

#include "text_process_common.h"

#include <cstdint>
#include <regex>
#include <string>

// ============================================================================
//  M_node_expansion L2-04（v5 定稿 §4.3.5.4）：TextReplace 文本替换
//    pattern / replacement / use_regex / case_sensitive / max_replacements / whole_word
//  边界：无匹配 → 原文本不变；正则非法 → 报错；替换为空 → 删除匹配
//  说明：case_sensitive=false 的大小写折叠仅作用于 ASCII（非 ASCII 无折叠）；
//        whole_word 按「码点词边界」判定（中文整串成词，天然满足）
// ============================================================================

namespace aiwrite::nodes {
namespace {

using nlohmann::json;
using textproc::text_of;
using textproc::utf8_codepoints;

bool ascii_lower_equal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

// 边界判定：whole_word 用 —— 匹配区间前后都不是词内字符（中文整串成词，天然满足）
bool is_word_boundary_ok(const std::string& text, std::size_t begin, std::size_t end)
{
    const std::vector<uint32_t> cps = utf8_codepoints(text);
    std::size_t                 cp_begin = 0;
    std::size_t                 cp_end   = cps.size();
    {
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < cps.size(); ++i) {
            if (bytes == begin) {
                cp_begin = i;
            }
            if (bytes == end) {
                cp_end = i;
                break;
            }
            const uint32_t cp = cps[i];
            bytes += (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
        }
    }
    const bool prev_ok = (cp_begin == 0) || !textproc::is_word_char(cps[cp_begin - 1]);
    const bool next_ok = (cp_end >= cps.size()) || !textproc::is_word_char(cps[cp_end]);
    return prev_ok && next_ok;
}

// 普通文本替换（case / whole_word / max_count）
std::string replace_plain(const std::string& text, const std::string& pattern,
                          const std::string& replacement, bool case_sensitive, bool whole_word,
                          int max_count)
{
    if (pattern.empty()) {
        return text;
    }
    std::string result;
    std::size_t pos   = 0;
    int         count = 0;
    while (pos <= text.size()) {
        std::size_t hit = std::string::npos;
        if (case_sensitive) {
            hit = text.find(pattern, pos);
        }
        else {
            for (std::size_t i = pos; i + pattern.size() <= text.size(); ++i) {
                if (ascii_lower_equal(text.substr(i, pattern.size()), pattern)) {
                    hit = i;
                    break;
                }
            }
        }
        if (hit == std::string::npos) {
            result.append(text, pos, std::string::npos);
            break;
        }
        if (whole_word && !is_word_boundary_ok(text, hit, hit + pattern.size())) {
            result.append(text, pos, hit - pos + 1);
            pos = hit + 1;
            continue;
        }
        result.append(text, pos, hit - pos);
        result += replacement;
        ++count;
        pos = hit + pattern.size();
        if (max_count > 0 && count >= max_count) {
            result.append(text, pos, std::string::npos);
            break;
        }
    }
    return result;
}

// 正则替换（max_count 用迭代器手动限量；whole_word 做边界复检）
std::string replace_regex(const std::string& text, const std::string& pattern,
                          const std::string& replacement, bool case_sensitive, bool whole_word,
                          int max_count, std::string* error)
{
    std::regex regex;
    if (!textproc::make_regex(pattern, !case_sensitive, regex, error)) {
        return text;
    }
    std::string result;
    std::size_t pos   = 0;
    int         count = 0;
    while (pos < text.size()) {
        std::smatch m;
        if (!std::regex_search(text.cbegin() + static_cast<std::ptrdiff_t>(pos), text.cend(), m,
                               regex)) {
            result.append(text, pos, std::string::npos);
            break;
        }
        const std::size_t match_begin = pos + static_cast<std::size_t>(m.position());
        const std::size_t match_end   = match_begin + m.length();
        if (whole_word && !is_word_boundary_ok(text, match_begin, match_end)) {
            result.append(text, pos, match_begin - pos + 1);
            pos = match_begin + 1;
            continue;
        }
        result.append(text, pos, match_begin - pos);
        result += replacement;
        ++count;
        pos = match_end;
        if (max_count > 0 && count >= max_count) {
            result.append(text, pos, std::string::npos);
            break;
        }
        if (m.length() == 0 && pos < text.size()) {
            result += text[pos];
            ++pos;
        }
    }
    return result;
}

} // namespace

json execute_text_replace(const json& inputs, const json& params, engine::ExecutionContext& /*ctx*/)
{
    const std::string text = inputs.contains("text") ? text_of(inputs["text"]) : std::string();
    const std::string pattern     = params.value("pattern", std::string());
    const std::string replacement = params.value("replacement", std::string());
    const bool        use_regex   = params.value("use_regex", false);
    const bool        case_sensitive = params.value("case_sensitive", true);
    const int         max_replacements = params.value("max_replacements", 0);
    const bool        whole_word       = params.value("whole_word", false);

    if (pattern.empty()) {
        return text; // 空查找模式 = 不做替换（边界）
    }

    if (use_regex) {
        std::string error;
        const std::string result = replace_regex(text, pattern, replacement, case_sensitive,
                                                 whole_word, max_replacements, &error);
        if (!error.empty()) {
            throw engine::NodeError("文本替换：正则非法 —— " + error);
        }
        return result;
    }
    return replace_plain(text, pattern, replacement, case_sensitive, whole_word,
                         max_replacements);
}

} // namespace aiwrite::nodes