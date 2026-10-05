#include "web/channel_frames.h"

// 实现纪律：**不抛异常**（非法输入一律走 `bad = true` / 空结果），
// 因为调用方是管道读循环 —— 一个坏帧不得让守护进程或 UI 线程崩掉（`VB2-29`）。

#include <algorithm>

#include "nlohmann/json.hpp"

namespace aiwrite::web::channel {
namespace {

constexpr const char* kKindCmd = "cmd";
constexpr const char* kKindEvt = "evt";
constexpr const char* kKindErr = "err";

bool is_known_kind(const std::string& kind)
{
    return kind == kKindCmd || kind == kKindEvt || kind == kKindErr;
}

// 去掉行尾 `\r` / `\n`（帧以 `\n` 结尾；残留 `\r` 也容忍）
std::string strip_eol(const std::string& line)
{
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r')) {
        --end;
    }
    return line.substr(0, end);
}

bool is_blank(const std::string& text)
{
    for (const char ch : text) {
        if (ch != ' ' && ch != '\t') {
            return false;
        }
    }
    return true;
}

// 载荷文本 → JSON 对象（空 / 非法 → 空对象；**不抛**）
nlohmann::json parse_object(const std::string& text)
{
    if (text.empty() || is_blank(text)) {
        return nlohmann::json::object();
    }
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

Frame bad(std::string reason, const std::string& raw)
{
    Frame frame;
    frame.bad          = true;
    frame.reason       = std::move(reason);
    frame.payload_json = raw.size() > 200 ? raw.substr(0, 200) : raw;
    return frame;
}

// ---- v4（step 14）：站字段组校验（与 Python `protocol.station_fields_reason` **逐条同构**）----
//  * 选择器（`input_selector[]` / `answer_selector[]`）必须是非空字符串数组（多候选 · `M7B-24`）
//  * `send` = `{"kind": "key" | "click", "value": "<键名 / CSS 选择器>"}`
//  * `done_when` = `{"kind": "selector_present" | "selector_gone", ...}`
//  * `poll_ms` / `max_polls` = 整数；`upload_evidence` = 布尔（`I18` **按位开关**）
std::string selector_list_reason(const nlohmann::json& value, const std::string& field)
{
    if (!value.is_array() || value.empty()) {
        return "字段 " + field +
               " 必须是**非空字符串数组**（多候选逐个探测，首个可见且命中者胜）";
    }
    for (const nlohmann::json& item : value) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            return "字段 " + field + " 的元素必须是**非空字符串**（CSS 选择器）";
        }
    }
    return {};
}

std::string station_fields_reason(const nlohmann::json& payload, const std::string& name)
{
    for (const char* field : {"input_selector", "answer_selector"}) {
        if (payload.contains(field)) {
            const std::string reason = selector_list_reason(payload[field], field);
            if (!reason.empty()) {
                return reason;
            }
        }
    }
    if (name == "send_prompt") {
        if (!payload.contains("send") || !payload["send"].is_object()) {
            return "字段 send 必须是对象 {kind, value}（kind = key | click）";
        }
        const nlohmann::json& send = payload["send"];
        const std::string     kind = send.value("kind", std::string());
        if (kind != "key" && kind != "click") {
            return "字段 send.kind 取值非法（应为 key | click）";
        }
        if (send.value("value", std::string()).empty()) {
            return "字段 send.value 必须是**非空字符串**（按键名或 CSS 选择器）";
        }
    }
    if (payload.contains("done_when")) {
        const nlohmann::json& done = payload["done_when"];
        if (!done.is_object()) {
            return "字段 done_when 必须是对象 {kind, selector?}";
        }
        const std::string kind = done.value("kind", std::string());
        if (kind != "selector_present" && kind != "selector_gone") {
            return "字段 done_when.kind 取值非法（应为 selector_present | selector_gone）";
        }
    }
    for (const char* field : {"poll_ms", "max_polls"}) {
        if (payload.contains(field) && !payload[field].is_number_integer()) {
            return std::string("字段 ") + field + " 必须是整数（毫秒 / 轮数）";
        }
    }
    if (payload.contains("upload_evidence") && !payload["upload_evidence"].is_boolean()) {
        return "字段 upload_evidence 必须是**布尔**（I18 按位开关：仅本次运行**有图**才置位）";
    }
    return {};
}

} // namespace

const char* error_code_bad_frame()
{
    return "bad_frame";
}

bool frame_within_limit(const std::string& line)
{
    return line.size() <= kMaxFrameBytes;
}

Frame parse_frame(const std::string& line)
{
    const std::string text = strip_eol(line);
    if (is_blank(text)) {
        return bad("空行（帧内不得有裸换行）", line);
    }
    if (!frame_within_limit(text)) {
        return bad("超过单帧上限 65536 字节", text);
    }

    const nlohmann::json obj = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (obj.is_discarded()) {
        return bad("JSON 不合法（帧必须是 UTF-8 JSON 对象，一行一帧）", text);
    }
    if (!obj.is_object()) {
        return bad("帧必须是 JSON 对象", text);
    }

    if (!obj.contains("v")) {
        return bad("缺少字段 v", text);
    }
    if (!obj["v"].is_number_integer()) {
        return bad("v 必须是整数", text);
    }
    const int version = obj["v"].get<int>();
    if (version != kProtoVersion) {
        return bad("v 不识别（收到 " + std::to_string(version) + "，本机支持 " +
                       std::to_string(kProtoVersion) + "）",
                   text);
    }

    if (!obj.contains("kind") || !obj["kind"].is_string()) {
        return bad("缺少字段 kind（或类型不符）", text);
    }
    const std::string kind = obj["kind"].get<std::string>();
    if (!is_known_kind(kind)) {
        return bad("kind 非法（应为 cmd | evt | err）", text);
    }

    std::string id;
    if (obj.contains("id")) {
        if (!obj["id"].is_string()) {
            return bad("id 必须是字符串", text);
        }
        id = obj["id"].get<std::string>();
    }
    if (kind != kKindErr && id.empty()) {
        return bad("缺少字段 id（请求-响应配对；事件帧用 \"-\"）", text);
    }

    std::string name;
    if (obj.contains("name")) {
        if (!obj["name"].is_string()) {
            return bad("name 必须是字符串", text);
        }
        name = obj["name"].get<std::string>();
    }

    nlohmann::json payload = nlohmann::json::object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() != "v" && it.key() != "kind" && it.key() != "id" && it.key() != "name") {
            payload[it.key()] = it.value();
        }
    }

    Frame frame;
    frame.bad          = false;
    frame.v            = version;
    frame.kind         = kind;
    frame.id           = id;
    frame.name         = name;
    frame.payload_json = payload.dump();
    return frame;
}

std::string make_command(const std::string& name, const std::string& id,
                         const std::string& fields_json)
{
    nlohmann::json obj = parse_object(fields_json);
    obj["v"]    = kProtoVersion;
    obj["kind"] = kKindCmd;
    obj["id"]   = id;
    obj["name"] = name;
    return obj.dump();
}

std::string make_event(const std::string& name, const std::string& id,
                       const std::string& fields_json)
{
    nlohmann::json obj = parse_object(fields_json);
    obj["v"]    = kProtoVersion;
    obj["kind"] = kKindEvt;
    obj["id"]   = id;
    obj["name"] = name;
    return obj.dump();
}

std::string make_error(const std::string& code, const std::string& id, const std::string& hint)
{
    nlohmann::json obj = nlohmann::json::object();
    obj["v"]    = kProtoVersion;
    obj["kind"] = kKindErr;
    obj["id"]   = id;
    obj["name"] = code;
    if (!hint.empty()) {
        obj["hint"] = hint;
    }
    return obj.dump();
}

const std::vector<std::string>& known_commands()
{
    static const std::vector<std::string> commands = {"hello",        "open_tab",     "login_state",
                                                     "upload_image", "send_prompt",  "read_answer",
                                                     "run_script",   "logout_site",  "current_tab",
                                                     "shutdown"};
    return commands;
}

const std::vector<std::string>& known_events()
{
    static const std::vector<std::string> events = {"stage",       "evidence",    "delta",
                                                    "answer_done", "script_done", "tab",
                                                    "error",       "ready"};
    return events;
}

bool is_known_command(const std::string& name)
{
    const std::vector<std::string>& commands = known_commands();
    return std::find(commands.begin(), commands.end(), name) != commands.end();
}

std::vector<std::string> required_fields_of(const std::string& command)
{
    if (command == "open_tab" || command == "login_state" || command == "logout_site") {
        return {"provider"};
    }
    if (command == "upload_image") {
        return {"provider", "images"};
    }
    if (command == "send_prompt") {          // v4（step 14）：站字段组**必需**（I14：由调用方下发）
        return {"provider", "prompt", "input_selector", "send"};
    }
    if (command == "read_answer") {          // v4（step 14）：选择器必需
        return {"provider", "answer_selector"};
    }
    if (command == "run_script") {           // v2（批 3 step 8）：页面内执行诊断 JS
        return {"provider", "script"};
    }
    return {}; // hello / shutdown / current_tab：无必需字段
}

bool validate_command(const Frame& frame, std::string* reason)
{
    const auto fail = [reason](const std::string& text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };
    if (frame.bad) {
        return fail("非法行（须先按 §6.1 丢弃并回 err{bad_frame}）");
    }
    if (frame.kind != kKindCmd) {
        return fail("不是命令帧（kind=" + frame.kind + "）");
    }
    if (!is_known_command(frame.name)) {
        std::string known;
        for (const std::string& name : known_commands()) {
            known += (known.empty() ? "" : " / ") + name;
        }
        return fail("未知命令 " + frame.name + "（已知：" + known + "）");
    }

    const nlohmann::json payload = parse_object(frame.payload_json);
    std::string          missing;
    for (const std::string& field : required_fields_of(frame.name)) {
        if (!payload.contains(field)) {
            missing += (missing.empty() ? "" : " / ") + field;
        }
    }
    if (!missing.empty()) {
        return fail("缺少字段 " + missing + "（命令 " + frame.name + " 必需）");
    }

    if (frame.name == "upload_image") {
        const nlohmann::json& images = payload["images"];
        if (!images.is_array() || images.empty()) {
            return fail("缺少字段 images（命令 upload_image 需要**本地绝对路径**数组）");
        }
        if (images.size() > kMaxImages) {
            return fail("images 超过上限 " + std::to_string(kMaxImages) + "（收到 " +
                        std::to_string(images.size()) + "）");
        }
        for (const nlohmann::json& item : images) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return fail("images 元素必须是**路径字符串**（不传 base64）");
            }
        }
    }
    if (payload.contains("attach")) {
        const nlohmann::json& attach = payload["attach"];
        const std::string value = attach.is_string() ? attach.get<std::string>() : std::string();
        if (value != "auto" && value != "file_input" && value != "drop_zone" &&
            value != "paste_only" && value != "none") {
            return fail("attach 取值非法（应为 auto | file_input | drop_zone | paste_only | none）");
        }
    }
    // v4（step 14）：站字段组校验（与 Python `protocol.station_fields_reason` **逐条同构**）
    const std::string station_reason = station_fields_reason(payload, frame.name);
    if (!station_reason.empty()) {
        return fail(station_reason);
    }
    return true;
}

std::string delta_text_of_frame(const std::string& payload_json, bool* explicit_non_stream)
{
    const auto flag = [explicit_non_stream](bool value) {
        if (explicit_non_stream != nullptr) {
            *explicit_non_stream = value;
        }
    };
    if (payload_json.empty() || is_blank(payload_json)) {
        flag(true);
        return {};
    }
    const nlohmann::json payload = parse_object(payload_json);
    if (payload.contains("text") && payload["text"].is_string()) {
        const std::string text = payload["text"].get<std::string>();
        if (!text.empty()) {
            flag(false);
            return text;
        }
    }
    if (payload.contains("delta") && payload["delta"].is_object()) {
        const nlohmann::json& nested = payload["delta"];
        if (nested.contains("text") && nested["text"].is_string()) {
            const std::string text = nested["text"].get<std::string>();
            if (!text.empty()) {
                flag(false);
                return text;
            }
        }
    }
    flag(true); // I22：无增量 → 必须显式标注「非流式（轮询）」
    return {};
}

} // namespace aiwrite::web::channel


