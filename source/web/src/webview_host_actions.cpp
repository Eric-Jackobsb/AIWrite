// ============================================================================
//  webview_host_actions.cpp — 窗口内动作：脚本槽 / 复位导航 / input[type=file] / CDP 输入
//  * ARCH-Z1b：自 3514 行单文件 `webview_host.cpp` 逐字拆出，**零语义变化**
//  * 模块内契约见 `webview_host_detail.h`；公开 API 见 `webview_host.h`
// ============================================================================

#include "webview_host_detail.h"
#include "log.h"
#include "probe_verdict.h"

namespace aiwrite::web {

void finish_run_script(std::string result, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().script_mutex);
        st().script_result = std::move(result);
        st().script_error  = std::move(error);
        st().script_done   = true;
    }
    st().script_cv.notify_all();
}

// L3：在窗口线程执行 st().script_js（不做解释，原样回传 JSON 结果）
void run_script_in_window()
{
    if (st().webview == nullptr) {
        finish_run_script({}, "WebView2 未就绪");
        return;
    }
    std::string script_js;
    {
        std::lock_guard<std::mutex> lock(st().script_mutex);
        script_js = st().script_js;
    }
    if (script_js.empty()) {
        finish_run_script({}, "脚本为空");
        return;
    }
    const std::wstring script = to_wide(script_js);
    const HRESULT      hr     = st().webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR json_result) -> HRESULT {
                if (FAILED(result)) {
                    finish_run_script({}, "脚本执行失败（HRESULT " + std::to_string(result) + "）");
                }
                else {
                    finish_run_script(json_result == nullptr ? std::string("null")
                                                             : to_utf8(json_result),
                                      {});
                }
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        finish_run_script({}, "调用 ExecuteScript 失败");
    }
}

// ---- `PW-08`（`D-35③` 修订 2026-10-17）：复位 = 同一控制器「导航回新会话页」----
//  * 跨线程协议同「脚本槽」：调用方线程 `navigate_now`（PostMessage + 等 nav_cv）；
//    窗口线程 `run_navigate_in_window` 执行 Navigate；`NavigationCompleted` 回调
//    （见 `on_controller_ready`）置完成信号 —— 回调经窗口线程消息循环派发，不锁消息泵。

// 结束一次「复位导航」（窗口线程调用；ok = NavigationCompleted 成功）
void finish_navigate(bool ok, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().nav_mutex);
        st().nav_ok      = ok;
        st().nav_error   = std::move(error);
        st().nav_pending = false;
        st().nav_done    = true;
    }
    st().nav_cv.notify_all();
}

// 窗口线程：执行 `st().nav_url` 的导航（login_url 即新会话页；完成 = NavigationCompleted）
void run_navigate_in_window()
{
    if (st().webview == nullptr) {
        finish_navigate(false, "WebView2 未就绪（无法导航复位）");
        return;
    }
    std::string url;
    {
        std::lock_guard<std::mutex> lock(st().nav_mutex);
        url = st().nav_url;
    }
    if (url.empty()) {
        finish_navigate(false, "导航目标为空");
        return;
    }
    log::info("[网页版窗口] 复位导航发起: " + st().key + " → " + url);
    const HRESULT hr = st().webview->Navigate(to_wide(url).c_str());
    if (FAILED(hr)) {
        finish_navigate(false, "Navigate 调用失败（HRESULT " + std::to_string(hr) + "）");
    }
    // 成功 ⇒ 等 NavigationCompleted（现有回调会置完成信号 + 提取 Cookie）
}

// 调用方线程：请窗口线程导航回新会话页并**同步等待**完成（镜像 `run_script_now`）
//  * 用 `ctx` 而非 `st()`：`prepare_window` 在 `CtxScope` 绑定前调用，st() 不可用
bool navigate_now(WindowCtx* ctx, const std::string& url, int timeout_ms, std::string* error)
{
    if (ctx == nullptr) {
        if (error != nullptr) {
            *error = "窗口上下文为空";
        }
        return false;
    }
    if (url.empty()) {
        if (error != nullptr) {
            *error = "导航目标为空";
        }
        return false;
    }
    std::lock_guard<std::mutex> call_lock(ctx->nav_call_mutex); // 同窗口并发调用方串行
    const HWND window = ctx->window.load();
    if (window == nullptr || ctx->webview == nullptr) {
        if (error != nullptr) {
            *error = "窗口 / 页面尚未创建完成（" + ctx->key + "）";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->nav_mutex);
        ctx->nav_url     = url;
        ctx->nav_pending = true;
        ctx->nav_done    = false;
        ctx->nav_ok      = false;
        ctx->nav_error.clear();
    }
    ::PostMessageW(window, kNavigateMessage, 0, 0);

    std::unique_lock<std::mutex> lock(ctx->nav_mutex);
    const auto wait_ms = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    if (!ctx->nav_cv.wait_for(lock, wait_ms, [ctx] { return !ctx->nav_pending && ctx->nav_done; })) {
        if (error != nullptr) {
            *error = "复位导航超时（页面未完成加载：" + ctx->key + "）";
        }
        return false;
    }
    if (error != nullptr) {
        *error = ctx->nav_error;
    }
    return ctx->nav_ok;
}

// ---- M8-14：把本地文件交给页面（CDP）--------------------------------------------------

// 结束一次「注入文件」（窗口线程调用）
void finish_set_files(bool ok, std::string detail, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().files_mutex);
        st().files_ok     = ok;
        st().files_detail = std::move(detail);
        st().files_error  = std::move(error);
        st().files_done   = true;
    }
    st().files_cv.notify_all();
}

// 调用一次 CDP 方法（回调在窗口线程；`params_json` 为 UTF-8 JSON 文本）
void call_cdp(const wchar_t* method, const std::string& params_json,
              std::function<void(HRESULT, const std::string&)> on_done)
{
    if (st().webview == nullptr) {
        on_done(E_FAIL, {});
        return;
    }
    const std::wstring method_name = method;
    const std::wstring params      = to_wide(params_json);
    const HRESULT      hr          = st().webview->CallDevToolsProtocolMethod(
        method_name.c_str(), params.c_str(),
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [on_done](HRESULT result, LPCWSTR json_result) -> HRESULT {
                on_done(result, json_result == nullptr ? std::string() : to_utf8(json_result));
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        // 同步失败时回调不会触发 → 必须显式收尾，否则调用方等到超时
        log::warn("[图片上传] CallDevToolsProtocolMethod 同步失败 HRESULT=" + std::to_string(hr));
        on_done(hr, {});
    }
}

// CDP 三步：DOM.getDocument → DOM.querySelector → DOM.setFileInputFiles
void run_set_files_in_window()
{
    if (st().webview == nullptr) {
        finish_set_files(false, {}, "WebView2 未就绪");
        return;
    }
    std::string              selector;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(st().files_mutex);
        selector = st().files_selector;
        paths    = st().files_paths;
    }
    if (selector.empty()) {
        finish_set_files(false, {}, "选择器为空（站点单元未给 attach_selector）");
        return;
    }
    if (paths.empty()) {
        finish_set_files(false, {}, "文件列表为空");
        return;
    }

    call_cdp(L"DOM.getDocument", "{\"depth\":1}",
             [selector, paths](HRESULT result, const std::string& json_text) {
                 if (FAILED(result)) {
                     finish_set_files(false, {}, "CDP DOM.getDocument 失败（HRESULT " +
                                                     std::to_string(result) + "）");
                     return;
                 }
                 int root_node_id = 0;
                 try {
                     root_node_id = nlohmann::json::parse(json_text).at("root").value("nodeId", 0);
                 }
                 catch (const std::exception&) {
                     root_node_id = 0;
                 }
                 if (root_node_id <= 0) {
                     finish_set_files(false, {},
                                      "CDP DOM.getDocument 未返回根节点（原始：" +
                                          json_text.substr(0, 160) + "）");
                     return;
                 }
                 nlohmann::json query;
                 query["nodeId"]   = root_node_id;
                 query["selector"] = selector;
                 call_cdp(L"DOM.querySelector", query.dump(),
                          [selector, paths](HRESULT query_result, const std::string& query_text) {
                              if (FAILED(query_result)) {
                                  finish_set_files(false, {},
                                                   "CDP DOM.querySelector 失败（HRESULT " +
                                                       std::to_string(query_result) + "）");
                                  return;
                              }
                              int node_id = 0;
                              try {
                                  node_id = nlohmann::json::parse(query_text).value("nodeId", 0);
                              }
                              catch (const std::exception&) {
                                  node_id = 0;
                              }
                              if (node_id <= 0) {
                                  finish_set_files(false, {},
                                                   "选择器未命中页面元素：" + selector +
                                                       "（请跑 --upload-selftest 复核 "
                                                       "web.attach_selector）");
                                  return;
                              }
                              nlohmann::json set_params;
                              set_params["nodeId"] = node_id;
                              set_params["files"]  = paths;
                              call_cdp(L"DOM.setFileInputFiles", set_params.dump(),
                                       [selector, node_id, count = paths.size()](
                                           HRESULT set_result, const std::string& /*json*/) {
                                           if (FAILED(set_result)) {
                                               finish_set_files(
                                                   false, {},
                                                   "CDP DOM.setFileInputFiles 失败（HRESULT " +
                                                       std::to_string(set_result) + "，选择器 " +
                                                       selector + "，nodeId " +
                                                       std::to_string(node_id) + "）");
                                               return;
                                           }
                                           finish_set_files(
                                               true,
                                               "CDP DOM.setFileInputFiles 完成（nodeId=" +
                                                   std::to_string(node_id) + "，文件 " +
                                                   std::to_string(count) + " 个）",
                                               {});
                                       });
                          });
             });
}

// ---- M_patchC（`PW-07f` · 2026-10-08 · **自 `wip/pw07n-20261008` 移植回主分支**）-----------------
//  CDP「真实输入 / 发送」（窗口线程执行；与 `run_set_files_in_window` 同形）
//  为什么必须走 CDP：页面脚本写 contenteditable（`execCommand` / `textContent =`）再发
//  `new KeyboardEvent('keydown', {key:'Enter'})` 时 `isTrusted=false` —— 豆包（tiptap /
//  ProseMirror）用自己的 state 重渲染 ⇒ 直接改 DOM 的文本**被抹掉**、合成回车被忽略
//  ⇒ **消息根本没发出去**（真机 2026-10-08 连续三次 `候选池 0`，页面 60 秒内一条消息都没渲染）。
//  图片路径之所以「能用」：图片走 CDP `DOM.setFileInputFiles`（可信）先落进 composer。

// 结束一次「CDP 真实输入」（窗口线程调用）
void finish_cdp_input(bool ok, std::string detail, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(st().cdp_mutex);
        st().cdp_ok     = ok;
        st().cdp_detail = std::move(detail);
        st().cdp_error  = std::move(error);
        st().cdp_done   = true;
    }
    st().cdp_cv.notify_all();
}

// 一步 CDP 调用（成功后把 `label` 追加进诊断串；任一步失败 → 立即如实收尾）
struct CdpInputStep {
    const wchar_t* method   = L"";
    std::string    params;    // UTF-8 JSON 文本
    std::string    label;     // 成功后写入 detail（空 = 中间步骤，不写）
    std::string    fail_note; // 失败时写入 error 的前缀
};

void run_cdp_input_steps(const std::shared_ptr<std::vector<CdpInputStep>>& steps,
                         const std::shared_ptr<std::size_t>&                index,
                         const std::shared_ptr<std::string>&                detail)
{
    if (*index >= steps->size()) {
        finish_cdp_input(true, *detail, {});
        return;
    }
    const CdpInputStep step = (*steps)[*index];
    call_cdp(step.method, step.params,
             [steps, index, detail, label = step.label, fail_note = step.fail_note](
                 HRESULT result, const std::string& /*json*/) {
                 if (FAILED(result)) {
                     finish_cdp_input(false, *detail,
                                      fail_note + "（HRESULT " + std::to_string(result) + "）");
                     return;
                 }
                 if (!label.empty()) {
                     if (!detail->empty()) {
                         *detail += "｜";
                     }
                     *detail += label;
                 }
                 *index += 1;
                 run_cdp_input_steps(steps, index, detail);
             });
}

void run_cdp_input_in_window()
{
    std::string text;
    bool        press_enter = false;
    {
        std::lock_guard<std::mutex> lock(st().cdp_mutex);
        text        = st().cdp_text;
        press_enter = st().cdp_enter;
    }
    if (st().webview == nullptr) {
        finish_cdp_input(false, {}, "CDP 真实输入：WebView 不可用（窗口已关闭？）");
        return;
    }

    auto steps  = std::make_shared<std::vector<CdpInputStep>>();
    auto detail = std::make_shared<std::string>();

    if (!text.empty()) {
        // `Input.insertText` = **组合输入**语义（等同输入法提交）：站点拿到 isTrusted=true 的
        // 文本输入 ⇒ 富文本编辑器走自己的插入事务（DOM 与内部 state 同步，后续不会被抹掉）
        nlohmann::json params;
        params["text"] = text;
        steps->push_back({L"Input.insertText", params.dump(),
                          "CDP Input.insertText OK（" + std::to_string(text.size()) + " 字节）",
                          "CDP Input.insertText 失败"});
    }
    if (press_enter) {
        // 真实回车 = `keyDown`（带 `text="\r"`，与 Puppeteer `keyboard.press('Enter')` 等效）
        //   + `keyUp`；`windowsVirtualKeyCode=13` ⇒ 站点按真实 Enter 处理（不是合成事件）
        const auto enter_params = [](const char* type, const char* text_field) {
            nlohmann::json p;
            p["type"]                  = type;
            p["key"]                   = "Enter";
            p["code"]                  = "Enter";
            p["windowsVirtualKeyCode"] = 13;
            p["nativeVirtualKeyCode"]  = 13;
            p["text"]                  = text_field;
            p["unmodifiedText"]        = text_field;
            return p.dump();
        };
        steps->push_back({L"Input.dispatchKeyEvent", enter_params("keyDown", "\r"), {},
                          "CDP Input.dispatchKeyEvent(Enter keyDown) 失败"});
        steps->push_back({L"Input.dispatchKeyEvent", enter_params("keyUp", "\r"),
                          "CDP Input.dispatchKeyEvent Enter OK",
                          "CDP Input.dispatchKeyEvent(Enter keyUp) 失败"});
    }
    if (steps->empty()) {
        finish_cdp_input(false, {}, "CDP 真实输入：text 与 press_enter 均为空");
        return;
    }

    const auto index = std::make_shared<std::size_t>(0);
    run_cdp_input_steps(steps, index, detail);
}

// M8-14：网络回执挂钩（窗口线程；控制器就绪后调用一次）

} // namespace aiwrite::web
