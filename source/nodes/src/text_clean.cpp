#include "nodes.h"

#include "text_process_common.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
//  M_node_expansion L2-03（v5 定稿 §4.3.5.3）：TextClean 文本清理
//    trim / collapse_whitespace / remove_html / remove_control_chars /
//    dedup_lines / remove_empty_lines / remove_prefix / remove_suffix / remove_ai_prefix
//  边界：全部 false → 原文本不变；remove_prefix / remove_suffix 只去开头/结尾一次
// ============================================================================

namespace aiwrite::nodes {
namespace {

using nlohmann::json;
using textproc::text_of;
using textproc::utf8_codepoints;

std::string cps_to_utf8(const std::vector<uint32_t>& cps, std::size_t begin, std::size_t end)
{
    std::string out;
    for (std::size_t i = begin; i < end; ++i) {
        const uint32_t cp = cps[i];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string ltrim_unicode(const std::string& s)
{
    const std::vector<uint32_t> cps = utf8_codepoints(s);
    std::size_t                 begin = 0;
    while (begin < cps.size() && textproc::is_space_cp(cps[begin])) {
        ++begin;
    }
    return cps_to_utf8(cps, begin, cps.size());
}

// 去首尾空白（Unicode 感知：含全角空格）
std::string trim_unicode(const std::string& s)
{
    const std::vector<uint32_t> cps = utf8_codepoints(s);
    std::size_t                 begin = 0;
    std::size_t                 end   = cps.size();
    while (begin < end && textproc::is_space_cp(cps[begin])) {
        ++begin;
    }
    while (end > begin && textproc::is_space_cp(cps[end - 1])) {
        --end;
    }
    return cps_to_utf8(cps, begin, end);
}

std::string strip_prefix_once(const std::string& text, const std::string& prefix)
{
    if (prefix.empty()) {
        return text;
    }
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0
               ? text.substr(prefix.size())
               : text;
}

std::string strip_suffix_once(const std::string& text, const std::string& suffix)
{
    if (suffix.empty()) {
        return text;
    }
    return text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0
               ? text.substr(0, text.size() - suffix.size())
               : text;
}

// 一键去 AI 客套前缀（curated 清单；前缀后跟任意空白一并去除，只动开头）
std::string strip_ai_prefix(const std::string& text)
{
    static const char* kPrefixes[] = {
        "好的，以下是", "好的，这是一个", "好的，这是", "好的，下面是", "好的，如下",
        "好的，请查收", "好的，",        "好的",       "当然，以下是", "当然，",
        "没问题，",     "以下是",         "下面是我为你", "这是我为你准备",
    };
    for (const char* prefix : kPrefixes) {
        const std::string p(prefix);
        if (text.compare(0, p.size(), p) == 0) {
            return ltrim_unicode(text.substr(p.size()));
        }
    }
    return text;
}

} // namespace

json execute_text_clean(const json& inputs, const json& params, engine::ExecutionContext& /*ctx*/)
{
    std::string text = inputs.contains("text") ? text_of(inputs["text"]) : std::string();

    const bool trim                 = params.value("trim", true);
    const bool collapse_whitespace  = params.value("collapse_whitespace", false);
    const bool remove_html          = params.value("remove_html", false);
    const bool remove_control_chars = params.value("remove_control_chars", true);
    const bool dedup_lines          = params.value("dedup_lines", false);
    const bool remove_empty_lines   = params.value("remove_empty_lines", false);
    const bool remove_ai_prefix     = params.value("remove_ai_prefix", false);

    if (remove_control_chars) {
        std::string out;
        out.reserve(text.size());
        for (const unsigned char c : text) {
            const bool control = (c < 0x09) || (c > 0x0D && c < 0x20) || c == 0x7F;
            if (!control) {
                out.push_back(static_cast<char>(c));
            }
        }
        text = std::move(out);
    }

    if (collapse_whitespace) {
        std::string out;
        out.reserve(text.size());
        bool prev_space = false;
        for (const unsigned char c : text) {
            const bool space = textproc::is_space_cp(c);
            if (space) {
                if (!prev_space) {
                    out.push_back(' ');
                }
                prev_space = true;
            }
            else {
                out.push_back(static_cast<char>(c));
                prev_space = false;
            }
        }
        text = std::move(out);
    }

    if (remove_html) {
        std::string out;
        out.reserve(text.size());
        bool in_tag = false;
        for (const unsigned char c : text) {
            if (c == '<') {
                in_tag = true;
                continue;
            }
            if (c == '>') {
                in_tag = false;
                continue;
            }
            if (!in_tag) {
                out.push_back(static_cast<char>(c));
            }
        }
        std::string replaced;
        replaced.reserve(out.size());
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (out.compare(i, 6, "&nbsp;") == 0) { replaced += ' '; i += 5; }
            else if (out.compare(i, 4, "&lt;") == 0) { replaced += '<'; i += 3; }
            else if (out.compare(i, 4, "&gt;") == 0) { replaced += '>'; i += 3; }
            else if (out.compare(i, 5, "&amp;") == 0) { replaced += '&'; i += 4; }
            else if (out.compare(i, 6, "&quot;") == 0) { replaced += '"'; i += 5; }
            else if (out.compare(i, 5, "&#39;") == 0) { replaced += '\''; i += 4; }
            else { replaced += out[i]; }
        }
        text = std::move(replaced);
    }

    if (dedup_lines || remove_empty_lines) {
        std::vector<std::string> lines;
        std::size_t              pos = 0;
        while (pos <= text.size()) {
            const std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(text.substr(pos));
                break;
            }
            lines.push_back(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
        std::vector<std::string> kept;
        for (const std::string& l : lines) {
            if (remove_empty_lines && trim_unicode(l).empty()) {
                continue;
            }
            if (dedup_lines && std::find(kept.begin(), kept.end(), l) != kept.end()) {
                continue;
            }
            kept.push_back(l);
        }
        std::string joined;
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (i > 0) {
                joined += '\n';
            }
            joined += kept[i];
        }
        text = std::move(joined);
    }

    if (remove_ai_prefix) {
        text = strip_ai_prefix(text);
    }

    const std::string remove_prefix =
        textproc::unescape(params.value("remove_prefix", std::string()));
    const std::string remove_suffix =
        textproc::unescape(params.value("remove_suffix", std::string()));
    if (!remove_prefix.empty()) {
        text = strip_prefix_once(text, remove_prefix);
    }
    if (!remove_suffix.empty()) {
        text = strip_suffix_once(text, remove_suffix);
    }

    if (trim) {
        text = trim_unicode(text);
    }

    return text;
}

} // namespace aiwrite::nodes