#pragma once

// ============================================================================
//  M_node_expansion L2（v5 定稿 §4.3.5）：6 个文本处理节点的**共享纯函数**
//  （TextSplit / TextExtract / TextClean / TextReplace / TextStats / TextDiff）
//  * 全部 UTF-8 感知：中文字符按**码点**统计 / 边界判定，不做字节级截断
//  * 无第三方依赖（正则用 std::regex；diff 用自写 LCS）
//  * 本头文件只放**纯函数**（可被 api_probe 离线断言直接调用）
// ============================================================================

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace aiwrite::nodes::textproc {

using nlohmann::json;

// 端口值文本化（与 local_nodes.cpp 同名行为一致：非字符串一律 dump）
inline std::string text_of(const json& value)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

// 支持 \n \t \r \\ 转义（与 local_nodes.cpp `unescape` 同口径 —— 分隔符等参数的习惯写法）
inline std::string unescape(const std::string& text)
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

// UTF-8 安全：把字符串拆成**码点**数组（非法字节按原始字节 1:1 保留，保证可逆）
inline std::vector<uint32_t> utf8_codepoints(const std::string& s)
{
    std::vector<uint32_t> out;
    out.reserve(s.size());
    std::size_t       i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t            cp  = c;
        std::size_t         len = 1;
        if (c >= 0xF0) {
            cp  = c & 0x07;
            len = 4;
        }
        else if (c >= 0xE0) {
            cp  = c & 0x0F;
            len = 3;
        }
        else if (c >= 0xC0) {
            cp  = c & 0x1F;
            len = 2;
        }
        bool ok = (i + len <= n);
        for (std::size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) {
            out.push_back(cp);
            i += len;
        }
        else {
            out.push_back(static_cast<uint32_t>(c));
            i += 1;
        }
    }
    return out;
}

inline bool is_cjk(uint32_t cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK 统一表意
           (cp >= 0x3400 && cp <= 0x4DBF) ||  // 扩展 A
           (cp >= 0xF900 && cp <= 0xFAFF) ||  // 兼容表意
           (cp >= 0x20000 && cp <= 0x2A6DF);  // 扩展 B
}

inline bool is_word_char(uint32_t cp)
{
    if (cp < 0x80) {
        return std::isalnum(static_cast<unsigned char>(cp)) != 0 || cp == '_';
    }
    return true; // 非 ASCII（中文等）：视为词内字符
}

inline bool is_space_cp(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v' ||
           cp == 0x00A0 || cp == 0x3000; // 含全角空格
}

// 构造正则（ECMAScript），失败时把 error 写回；返回是否成功
inline bool make_regex(const std::string& pattern, bool ignore_case, std::regex& out,
                       std::string* error)
{
    try {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (ignore_case) {
            flags |= std::regex::icase;
        }
        out = std::regex(pattern, flags);
        return true;
    }
    catch (const std::regex_error& e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return false;
    }
}

// 按码点拆分字符串（每个码点一个元素）
inline std::vector<std::string> split_chars(const std::string& s)
{
    const std::vector<uint32_t> cps = utf8_codepoints(s);
    std::vector<std::string>    out;
    out.reserve(cps.size());
    for (uint32_t cp : cps) {
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
        out.push_back(std::move(piece));
    }
    return out;
}

// 统计中文字数（对小说创作最有用 —— TextStats number 端口）
inline std::size_t count_cjk(const std::string& s)
{
    std::size_t count = 0;
    for (uint32_t cp : utf8_codepoints(s)) {
        if (is_cjk(cp)) {
            ++count;
        }
    }
    return count;
}

} // namespace aiwrite::nodes::textproc
