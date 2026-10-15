#include "nodes.h"

#include "text_process_common.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ============================================================================
//  M_node_expansion L2-05（v5 定稿 §4.3.5.5）：TextStats 文本统计
//    stats_mode: basic / word_freq / all ；output_mode: text / number
//    number 端口 = 中文字数（对小说创作最有用）—— 激活闲置的 PortType::Number
//  边界：空输入 → 全 0
// ============================================================================

namespace aiwrite::nodes {
namespace {

using nlohmann::json;
using textproc::text_of;
using textproc::utf8_codepoints;

// 英文单词 = 连续 ASCII 字母；中文词 = 单个 CJK 码点
void collect_words(const std::string& text, std::vector<std::string>& out)
{
    std::string english_word;
    const auto  flush_english = [&]() {
        if (!english_word.empty()) {
            out.push_back(english_word);
            english_word.clear();
        }
    };
    for (uint32_t cp : utf8_codepoints(text)) {
        if (cp < 0x80 && std::isalpha(static_cast<unsigned char>(cp)) != 0) {
            english_word.push_back(static_cast<char>(cp));
            continue;
        }
        flush_english();
        if (textproc::is_cjk(cp)) {
            std::string piece;
            if (cp < 0x10000) {
                piece.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                piece.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                piece.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else {
                piece.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                piece.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                piece.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                piece.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            out.push_back(piece);
        }
    }
    flush_english();
}

// 段落数：空行（或仅空白行）分隔的文本块数
std::size_t count_paragraphs(const std::string& text)
{
    std::size_t paragraphs   = 0;
    bool        in_paragraph = false;
    std::size_t pos          = 0;
    while (true) {
        const std::size_t nl   = text.find('\n', pos);
        const std::string line = (nl == std::string::npos) ? text.substr(pos)
                                                           : text.substr(pos, nl - pos);
        bool blank = true;
        for (const unsigned char c : line) {
            if (!textproc::is_space_cp(c)) {
                blank = false;
                break;
            }
        }
        if (blank) {
            in_paragraph = false;
        }
        else {
            if (!in_paragraph) {
                ++paragraphs;
                in_paragraph = true;
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return paragraphs;
}

struct BasicStats {
    std::size_t chars_with_ws = 0;
    std::size_t chars_no_ws   = 0;
    std::size_t cjk_count     = 0;
    std::size_t english_words = 0;
    std::size_t lines         = 0;
    std::size_t paragraphs    = 0;
};

BasicStats compute_basic(const std::string& text)
{
    BasicStats s;
    if (text.empty()) {
        return s;
    }
    const std::vector<uint32_t> cps = utf8_codepoints(text);
    s.chars_with_ws = cps.size();
    for (uint32_t cp : cps) {
        if (!textproc::is_space_cp(cp)) {
            ++s.chars_no_ws;
        }
        if (textproc::is_cjk(cp)) {
            ++s.cjk_count;
        }
    }
    s.lines      = 1;
    s.paragraphs = count_paragraphs(text);
    for (const char c : text) {
        if (c == '\n') {
            ++s.lines;
        }
    }
    std::vector<std::string> words;
    collect_words(text, words);
    for (const std::string& w : words) {
        const std::vector<uint32_t> wcps = utf8_codepoints(w);
        if (wcps.size() == 1 && textproc::is_cjk(wcps[0])) {
            continue; // 中文字已计入 cjk_count
        }
        ++s.english_words;
    }
    return s;
}

std::vector<std::pair<std::string, std::size_t>> word_freq(const std::string& text, int top_n)
{
    std::vector<std::string>                   words;
    collect_words(text, words);
    std::map<std::string, std::size_t>         counts;
    for (const std::string& w : words) {
        ++counts[w];
    }
    std::vector<std::pair<std::string, std::size_t>> items(counts.begin(), counts.end());
    std::stable_sort(items.begin(), items.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    if (top_n > 0 && static_cast<int>(items.size()) > top_n) {
        items.resize(static_cast<std::size_t>(top_n));
    }
    return items;
}

} // namespace

json execute_text_stats(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    const std::string text = inputs.contains("text") ? text_of(inputs["text"]) : std::string();
    const std::string stats_mode  = params.value("stats_mode", std::string("basic"));
    const std::string output_mode = params.value("output_mode", std::string("text"));
    const int         top_n       = params.value("top_n", 10);

    const bool want_basic     = (stats_mode == "basic" || stats_mode == "all");
    const bool want_word_freq = (stats_mode == "word_freq" || stats_mode == "all");

    const BasicStats basic = want_basic ? compute_basic(text) : BasicStats();
    const std::size_t cjk  = want_basic ? basic.cjk_count : textproc::count_cjk(text);

    if (output_mode == "number") {
        ctx.console("[文本统计] 中文字数 = " + std::to_string(cjk) + "（number 端口）");
        return json{{ "text", json() }, { "number", static_cast<int>(cjk) }};
    }

    std::string report;
    if (want_basic) {
        report += "字符数（含空白）: " + std::to_string(basic.chars_with_ws) + "\n";
        report += "字符数（不含空白）: " + std::to_string(basic.chars_no_ws) + "\n";
        report += "中文字数: " + std::to_string(basic.cjk_count) + "\n";
        report += "英文单词数: " + std::to_string(basic.english_words) + "\n";
        report += "行数: " + std::to_string(basic.lines) + "\n";
        report += "段落数: " + std::to_string(basic.paragraphs) + "\n";
    }
    if (want_word_freq) {
        report += "\n词频 Top " + std::to_string(top_n) + ":\n";
        const auto items = word_freq(text, top_n);
        if (items.empty()) {
            report += "（无）\n";
        }
        for (const auto& item : items) {
            report += item.first + "  " + std::to_string(item.second) + "\n";
        }
    }
    ctx.console("[文本统计] 中文字数 = " + std::to_string(cjk));
    return json{{ "text", report }, { "number", static_cast<int>(cjk) }};
}

} // namespace aiwrite::nodes