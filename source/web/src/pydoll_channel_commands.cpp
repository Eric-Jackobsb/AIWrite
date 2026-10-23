// ============================================================================//  pydoll_channel_commands.cpp — 内容返回：run_script / connect_session / tab 判定 / joinNames//  * ARCH-Z1b：自 1282 行单文件 `pydoll_channel.cpp` 逐字拆出，**零语义变化**//  * 模块内契约见 `pydoll_channel_detail.h`；公开 API 见 `pydoll_channel.h`// ============================================================================
#include "pydoll_channel_detail.h"

namespace aiwrite::web::channel {

// ---- `run_script`（v2 · 批 3 step 8）：页面内执行诊断 JS（**诊断换代的地基**）----
//  * 守护进程**未在运行** → false + 可操作原因（**不自动起浏览器**：起浏览器归执行器 / `--pydoll-login`）
//  * 服务端截断时（> 48 KiB）**仍返回 true**，但 `*error` 带说明 ⇒ 调用方**必须提示已截断**
bool run_script(const SiteRef& site, const std::string& script_js, int timeout_ms,
                std::string* json_result, std::string* error)
{
    const std::string pipeName = daemonPipeName();
    PipeClient        client;
    if (!client.connect(pipeName, 2'000, error)) {
        if (error != nullptr) {
            *error = "Python 守护进程未在运行（管道 " + pipeName + " 连不上）—— "
                     "请先建立会话（`--pydoll-login <id>`），或由执行器按需启动"
                     "（`M7B-20` 接线后自动）";
        }
        return false;
    }
    nlohmann::json response;
    if (!client.call("run_script", {{"provider", site.provider_id}, {"script", script_js}},
                     &response, timeout_ms > 0 ? timeout_ms : kTimeoutScriptMs, error)) {
        client.close();
        return false;
    }
    client.close();

    // 结果原样交付（字符串直取；结构 / 标量 → JSON 文本）；截断事实**不隐藏**
    if (json_result != nullptr) {
        if (response.contains("result")) {
            const nlohmann::json& result = response["result"];
            *json_result = result.is_string() ? result.get<std::string>() : result.dump();
        } else {
            *json_result = "null";
        }
    }
    if (response.value("truncated", false) && error != nullptr) {
        *error = "脚本结果超过单帧余量（48 KiB）：已截断（`truncated=true`）—— "
                 "请收窄脚本返回的数据量（如分页 / 只回候选名）";
    }
    return true;
}

// ---- 内容返回（**v4 · step 14**）：`upload_image` / `send_prompt` / `read_answer` ----

// 连守护进程（**不自动起浏览器**：起会话归调用方 / `--pydoll-login`）
bool connect_session(PipeClient* client, int connect_ms, std::string* error)
{
    const std::string pipeName = daemonPipeName();
    if (client->connect(pipeName, connect_ms, error)) {
        return true;
    }
    if (error != nullptr) {
        *error = "新通道会话未在运行（管道 " + pipeName + " 连不上）—— "
                 "请先「打开登录窗口」建立会话（`--pydoll-login <id>`），再重试";
    }
    return false;
}


bool upload_image(const SiteRef& site, const std::vector<std::string>& images,
                  const std::string& attach, const std::string& attach_selector,
                  int timeout_ms, std::string* error)
{
    if (images.empty()) {
        if (error != nullptr) {
            *error = "upload_image 需要至少一个**本地绝对路径**（images[]）";
        }
        return false;
    }
    PipeClient client;
    if (!connect_session(&client, 2'000, error)) {
        return false;
    }
    nlohmann::json fields = {{"provider", site.provider_id}, {"images", images}};
    if (!attach.empty()) {
        fields["attach"] = attach;
    }
    if (!attach_selector.empty()) {
        fields["attach_selector"] = attach_selector;   // 文件输入框由调用方按条目下发（I14）
    }
    nlohmann::json ack;
    const bool     ok = client.call("upload_image", fields, &ack,
                                    timeout_ms > 0 ? timeout_ms : kTimeoutInjectMs, error);
    client.close();
    return ok;
}

bool send_prompt(const SiteRef& site, const std::string& prompt,
                 const std::vector<std::string>& input_selector,
                 const std::string& send_kind, const std::string& send_value,
                 bool upload_evidence, int timeout_ms, std::string* error)
{
    if (input_selector.empty()) {
        if (error != nullptr) {
            *error = "send_prompt 需要 `input_selector[]`（多候选，首个「命中且可见」者胜）—— "
                     "选择器由条目 `web.input_selector` 派生（`I14`：本层不认识站点）";
        }
        return false;
    }
    PipeClient client;
    if (!connect_session(&client, 2'000, error)) {
        return false;
    }
    nlohmann::json send = {{"kind", send_kind.empty() ? std::string("key") : send_kind},
                           {"value", send_value}};
    nlohmann::json fields = {{"provider", site.provider_id},
                             {"prompt", prompt},
                             {"input_selector", input_selector},
                             {"send", send}};
    if (upload_evidence) {
        fields["upload_evidence"] = true;   // I18 按位开关：仅**有图**的本次运行置位
    }
    nlohmann::json ack;
    const bool     ok = client.call("send_prompt", fields, &ack,
                                    timeout_ms > 0 ? timeout_ms : kTimeoutInjectMs, error);
    client.close();
    return ok;
}

bool read_answer(const SiteRef& site, const std::vector<std::string>& answer_selector,
                 const std::string& done_when_kind, int poll_ms, int max_polls,
                 int timeout_ms, std::string* text, bool* truncated, int* text_bytes,
                 std::string* error)
{
    if (text != nullptr) {
        text->clear();
    }
    if (truncated != nullptr) {
        *truncated = false;
    }
    if (text_bytes != nullptr) {
        *text_bytes = 0;
    }
    if (answer_selector.empty()) {
        if (error != nullptr) {
            *error = "read_answer 需要 `answer_selector[]`（条目 `web.answer_selector` 派生 · `I14`）";
        }
        return false;
    }
    PipeClient client;
    if (!connect_session(&client, 2'000, error)) {
        return false;
    }
    nlohmann::json fields = {{"provider", site.provider_id},
                             {"answer_selector", answer_selector}};
    if (!done_when_kind.empty()) {
        fields["done_when"] = {{"kind", done_when_kind}};
    }
    if (poll_ms > 0) {
        fields["poll_ms"] = poll_ms;
    }
    if (max_polls > 0) {
        fields["max_polls"] = max_polls;
    }
    nlohmann::json ack;
    const bool     ok = client.call("read_answer", fields, &ack,
                                    timeout_ms > 0 ? timeout_ms : kTimeoutAnswerMs, error);
    client.close();
    if (!ok) {
        return false;   // 未取到答案 / 超时：错误已由服务端给可操作 hint（不假装空答案）
    }
    if (text != nullptr) {
        *text = ack.value("text", std::string());
    }
    if (text_bytes != nullptr) {
        *text_bytes = ack.value("text_bytes", 0);
    }
    const bool cut = ack.value("truncated", false);
    if (truncated != nullptr) {
        *truncated = cut;
    }
    if (cut && error != nullptr) {
        *error = "回答正文超过单帧余量（48 KiB）：已截断（`truncated=true`）—— "
                 "界面 / 日志**必须**标注「已截断」，不得假装完整";
    }
    return true;
}

std::string currentTabSite()
{
    PipeClient  client;
    std::string connect_error;
    if (!client.connect(daemonPipeName(), 2'000, &connect_error)) {
        return {};   // 无会话 → 无 tab（如实回空串；**不假装**「不在任何站点」）
    }
    nlohmann::json ack;
    std::string    call_error;
    const bool     ok =
        client.call("current_tab", nlohmann::json::object(), &ack, 8'000, &call_error);
    client.close();
    if (!ok) {
        return {};   // 命令失败同样是「未知」—— 不编造站点
    }
    return ack.value("site", std::string());
}

bool tabOnSite(const std::string& site)
{
    const std::string want = siteKeyOf(site);   // 归一：既接受 origin，也接受完整 URL
    if (want.empty()) {
        return false;
    }
    const std::string current = currentTabSite();
    return !current.empty() && siteKeyOf(current) == want;
}


std::string joinNames(const std::vector<std::string>& names)
{
    std::string text;
    for (std::size_t index = 0; index < names.size(); ++index) {
        text += (index == 0 ? "" : " / ") + names[index];
    }
    return text.empty() ? "（条目未声明）" : text;
}


// `--pydoll-login <id>`：起**有头**浏览器 → 打开条目站点 → 等**人工登录** → 写内存会话 → 关窗
//  * 合规（§13）：有头窗口 + 用户手动操作；**不代填密码、不绕过验证、不注入站点端点**
//  * 判据（`I15′`）：条目 `cookie_names` 命中 ⇒ `logged_in`（**去 `userToken`**）
//  * 退出码：0 = 登录成功；1 = 超时 / 失败；2 = 参数 / 依赖问题

} // namespace aiwrite::web::channel

