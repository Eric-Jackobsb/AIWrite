#include "nodes.h"

#include "text_process_common.h"

#include <regex>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
//  M_node_expansion L2-02（v5 定稿 §4.3.5.2）：TextExtract 文本提取
//    extract_mode: regex / between / json_path / key_value
//    output_mode:  texts（多段端口）/ text（单段；多匹配时输出 JSON 数组文本）
//  边界：无匹配 → 空输出；正则非法 → 报错
// ============================================================================

namespace aiwrite::nodes {
namespace {

using nlohmann::json;
using textproc::text_of;

// 正则提取：优先捕获组 1，否则整个匹配
std::vector<std::string> extract_regex(const std::string& text, const std::string& pattern,
                                       std::string* error)
{
    std::vector<std::string> out;
    std::regex               regex;
    if (!textproc::make_regex(pattern, false, regex, error)) {
        return out;
    }
    for (std::sregex_iterator it(text.begin(), text.end(), regex), end; it != end; ++it) {
        const std::smatch& m = *it;
        out.push_back((m.size() > 1 && m[1].matched) ? m[1].str() : m.str());
    }
    return out;
}

// 起止标记之间：扫描全部非重叠区间
std::vector<std::string> extract_between(const std::string& text, const std::string& start_marker,
                                         const std::string& end_marker)
{
    std::vector<std::string> out;
    if (start_marker.empty() || end_marker.empty()) {
        return out;
    }
    std::size_t pos = 0;
    while (true) {
        const std::size_t s = text.find(start_marker, pos);
        if (s == std::string::npos) {
            break;
        }
        const std::size_t content_start = s + start_marker.size();
        const std::size_t e = text.find(end_marker, content_start);
        if (e == std::string::npos) {
            break;
        }
        out.push_back(text.substr(content_start, e - content_start));
        pos = e + end_marker.size();
    }
    return out;
}

// 点分 JSON 路径：a.b[0].c（只读取值；取不到 → 空）
json json_path_lookup(const json& root, const std::string& path)
{
    const json*      current = &root;
    std::size_t      i       = 0;
    const std::size_t n       = path.size();
    while (i < n && current != nullptr) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        if (path[i] == '[') {
            const std::size_t close = path.find(']', i);
            if (close == std::string::npos) {
                break;
            }
            const int index = std::atoi(path.substr(i + 1, close - i - 1).c_str());
            if (!current->is_array() || index < 0 ||
                static_cast<std::size_t>(index) >= current->size()) {
                return json();
            }
            current = &(*current)[static_cast<std::size_t>(index)];
            i       = close + 1;
            continue;
        }
        const std::size_t dot = path.find('.', i);
        const std::size_t brk = path.find('[', i);
        std::size_t       end = path.size();
        if (dot != std::string::npos) {
            end = std::min(end, dot);
        }
        if (brk != std::string::npos) {
            end = std::min(end, brk);
        }
        const std::string key = path.substr(i, end - i);
        if (!current->is_object() || !current->contains(key)) {
            return json();
        }
        current = &(*current)[key];
        i       = end;
    }
    return (current == nullptr) ? json() : *current;
}

// 键值提取：匹配「key：value」或「key: value」行
std::vector<std::string> extract_key_value(const std::string& text, const std::string& key_name)
{
    std::vector<std::string> out;
    if (key_name.empty()) {
        return out;
    }
    const std::string pattern = "^[ \\t]*" + key_name + "[ \\t]*[:：][ \\t]*(.+)$";
    std::regex        regex;
    std::string       error;
    if (!textproc::make_regex(pattern, false, regex, &error)) {
        return out;
    }
    std::string        line;
    std::istringstream stream(text);
    while (std::getline(stream, line)) {
        std::smatch m;
        if (std::regex_match(line, m, regex) && m.size() > 1) {
            out.push_back(m[1].str());
        }
    }
    return out;
}

} // namespace


json execute_text_extract(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    const std::string text = inputs.contains("text") ? text_of(inputs["text"]) : std::string();
    const std::string mode = params.value("extract_mode", std::string("regex"));
    const std::string output_mode = params.value("output_mode", std::string("texts"));

    std::vector<std::string> items;
    std::string              single_text;
    if (mode == "between") {
        items = extract_between(text, params.value("start_marker", std::string()),
                                params.value("end_marker", std::string()));
    }
    else if (mode == "json_path") {
        const std::string path = params.value("json_path", std::string());
        if (path.empty()) {
            throw engine::NodeError("文本提取：JSON 路径为空（json_path 模式）");
        }
        json parsed = json::parse(text, nullptr, false);
        if (parsed.is_discarded()) {
            throw engine::NodeError("文本提取：输入不是合法 JSON（json_path 模式）");
        }
        const json value = json_path_lookup(parsed, path);
        if (value.is_array()) {
            for (const json& item : value) {
                items.push_back(text_of(item));
            }
        }
        else {
            single_text = text_of(value);
        }
    }
    else if (mode == "key_value") {
        items = extract_key_value(text, params.value("key_name", std::string()));
    }
    else { // regex
        std::string error;
        items = extract_regex(text, params.value("pattern", std::string()), &error);
        if (!error.empty()) {
            throw engine::NodeError("文本提取：正则非法 —— " + error);
        }
    }

    if (output_mode == "text") {
        if (!items.empty()) {
            // 多匹配 → JSON 数组文本；单匹配 → 该段
            single_text = (items.size() == 1) ? items[0] : json(items).dump();
        }
        return json{{"texts", json::array()}, {"text", single_text}};
    }
    json array = json::array();
    for (const std::string& item : items) {
        array.push_back(item);
    }
    ctx.console("[文本提取] 命中 " + std::to_string(array.size()) + " 段");
    return json{{"texts", std::move(array)}, {"text", single_text}};
}

} // namespace aiwrite::nodes
