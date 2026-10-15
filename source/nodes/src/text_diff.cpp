#include "nodes.h"

#include "text_process_common.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
//  M_node_expansion L2-06（v5 定稿 §4.3.5.6）：TextDiff 文本对比
//    diff_mode: line / word / char ；context_lines：0–20 ；output_mode: text / json
//    diff 算法：**自写简单 LCS**（不引入库）
//  边界：两文本相同 → 输出「无差异」；一方为空 → 全部为增/删
// ============================================================================

namespace aiwrite::nodes {
namespace {

using nlohmann::json;
using textproc::text_of;

struct DiffOp {
    std::string op;   // equal / delete / insert
    std::string text;
};

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines; // 空文本 = 0 行（空 token 会制造「删除空行」的假差异）
    }
    std::size_t              pos = 0;
    while (true) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

std::vector<std::string> split_words(const std::string& text)
{
    std::vector<std::string> words;
    std::string              word;
    const auto               flush = [&]() {
        if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    };
    const auto push_cp = [&words](uint32_t cp) {
        std::string piece;
        if (cp < 0x80) {
            piece.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800) {
            piece.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            piece.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000) {
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
        words.push_back(std::move(piece));
    };
    for (uint32_t cp : textproc::utf8_codepoints(text)) {
        if (textproc::is_word_char(cp)) {
            if (cp < 0x80) {
                word.push_back(static_cast<char>(cp));
            }
            else {
                push_cp(cp); // 非 ASCII 词内字符（中文）单独成 token
            }
            continue;
        }
        flush();
        push_cp(cp); // 分隔符 / 空白单独成 token
    }
    flush();
    return words;
}

// 简单 LCS 差异：先剥共同前后缀，再对中间段做 DP（n*m 超限 → 整体视为替换块）
std::vector<DiffOp> diff_tokens(const std::vector<std::string>& a,
                                const std::vector<std::string>& b)
{
    std::vector<DiffOp> ops;

    // 1) 共同前缀
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) {
        ++prefix;
    }
    // 2) 共同后缀
    std::size_t suffix_a = 0;
    std::size_t suffix_b = 0;
    while (suffix_a < a.size() - prefix && suffix_b < b.size() - prefix &&
           a[a.size() - 1 - suffix_a] == b[b.size() - 1 - suffix_b]) {
        ++suffix_a;
        ++suffix_b;
    }

    for (std::size_t i = 0; i < prefix; ++i) {
        ops.push_back({ "equal", a[i] });
    }

    const std::size_t mid_a = a.size() - prefix - suffix_a;
    const std::size_t mid_b = b.size() - prefix - suffix_b;

    // 3) 中间段 LCS
    const std::size_t limit = static_cast<std::size_t>(4000000);
    if (mid_a * mid_b <= limit) {
        std::vector<std::vector<std::uint16_t>> dp(mid_a + 1,
                                                   std::vector<std::uint16_t>(mid_b + 1, 0));
        for (std::size_t i = 1; i <= mid_a; ++i) {
            for (std::size_t j = 1; j <= mid_b; ++j) {
                if (a[prefix + i - 1] == b[prefix + j - 1]) {
                    dp[i][j] = static_cast<std::uint16_t>(dp[i - 1][j - 1] + 1);
                }
                else {
                    dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
                }
            }
        }
        std::vector<DiffOp> middle;
        std::size_t         i = mid_a;
        std::size_t         j = mid_b;
        while (i > 0 && j > 0) {
            if (a[prefix + i - 1] == b[prefix + j - 1]) {
                middle.push_back({ "equal", a[prefix + i - 1] });
                --i;
                --j;
            }
            else if (dp[i - 1][j] >= dp[i][j - 1]) {
                middle.push_back({ "delete", a[prefix + i - 1] });
                --i;
            }
            else {
                middle.push_back({ "insert", b[prefix + j - 1] });
                --j;
            }
        }
        while (i > 0) {
            middle.push_back({ "delete", a[prefix + i - 1] });
            --i;
        }
        while (j > 0) {
            middle.push_back({ "insert", b[prefix + j - 1] });
            --j;
        }
        ops.insert(ops.end(), middle.rbegin(), middle.rend());
    }
    else {
        for (std::size_t i = 0; i < mid_a; ++i) {
            ops.push_back({ "delete", a[prefix + i] });
        }
        for (std::size_t j = 0; j < mid_b; ++j) {
            ops.push_back({ "insert", b[prefix + j] });
        }
    }

    for (std::size_t k = 0; k < suffix_a; ++k) {
        ops.push_back({ "equal", a[a.size() - suffix_a + k] });
    }
    return ops;
}

// 可读差异文本（hunk 式：变化块 + 上下文）
std::string format_diff_text(const std::vector<DiffOp>& ops, int context_lines)
{
    std::vector<bool> near_change(ops.size(), false);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].op != "equal") {
            near_change[i] = true;
        }
    }
    bool any_change = false;
    for (bool nc : near_change) {
        if (nc) {
            any_change = true;
            break;
        }
    }
    if (!any_change) {
        return "（无差异）";
    }
    const std::size_t ctx = static_cast<std::size_t>(std::max(0, context_lines));
    std::vector<bool> show(ops.size(), false);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (!near_change[i]) {
            continue;
        }
        const std::size_t from = (i >= ctx) ? i - ctx : 0;
        const std::size_t to   = std::min(ops.size(), i + ctx + 1);
        for (std::size_t k = from; k < to; ++k) {
            show[k] = true;
        }
    }
    std::string out;
    bool        need_ellipsis = false;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (!show[i]) {
            need_ellipsis = true;
            continue;
        }
        if (need_ellipsis && i > 0 && !show[i - 1]) {
            out += "...\n";
            need_ellipsis = false;
        }
        const char marker = (ops[i].op == "delete") ? '-' : (ops[i].op == "insert") ? '+' : ' ';
        out += marker;
        out += ' '; // 通用 diff 排印：标记列 = 标记 + 空格（上下文行 = 两个空格）
        out += ops[i].text;
        out += '\n';
    }
    return out;
}

json format_diff_json(const std::vector<DiffOp>& ops)
{
    json array = json::array();
    for (const DiffOp& op : ops) {
        array.push_back({ { "op", op.op }, { "text", op.text } });
    }
    return array;
}

} // namespace

json execute_text_diff(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    const std::string a = inputs.contains("text_a") ? text_of(inputs["text_a"]) : std::string();
    const std::string b = inputs.contains("text_b") ? text_of(inputs["text_b"]) : std::string();
    const std::string diff_mode = params.value("diff_mode", std::string("line"));
    const int         context_lines = params.value("context_lines", 3);
    const std::string output_mode   = params.value("output_mode", std::string("text"));

    std::vector<std::string> tokens_a;
    std::vector<std::string> tokens_b;
    if (diff_mode == "char") {
        tokens_a = textproc::split_chars(a);
        tokens_b = textproc::split_chars(b);
    }
    else if (diff_mode == "word") {
        tokens_a = split_words(a);
        tokens_b = split_words(b);
    }
    else {
        tokens_a = split_lines(a);
        tokens_b = split_lines(b);
    }

    const std::vector<DiffOp> ops = diff_tokens(tokens_a, tokens_b);

    if (output_mode == "json") {
        return format_diff_json(ops).dump();
    }

    const std::string text = format_diff_text(ops, context_lines);
    if (text == "（无差异）") {
        ctx.console("[文本对比] 两段文本无差异");
    }
    return text;
}

} // namespace aiwrite::nodes