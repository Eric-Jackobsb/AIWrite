#include "ui/property_panel.h"

#include "engine/node_registry.h"
#include "engine/provider_resolve.h"
#include "ui/editor_state.h"
#include "ui/output_panel.h"
#include "ui/text_view.h"
#include "ui/texture_cache.h"
#include "utils/asset_store.h"
#include "utils/file_dialog.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "web/pydoll_channel.h"
#include "web/session_store.h"
#include "web/webview_host.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aiwrite::ui {
namespace {

// ImGui 的 Slider* 只接受 half-range 内的数值（imgui_widgets.cpp SliderBehavior 的
//  IM_ASSERT(p_max <= T_MAX / 2)）——参数定义一旦越界，控件一渲染就 abort。
//  这里给出安全上限；越界范围自动降级为 Input* + 手动裁剪（见 draw_param_widget）。
constexpr double kSliderLimitInt   = 1073741823.0;  // IM_S32_MAX / 2
constexpr double kSliderLimitFloat = 1.7e38;         // ≈ FLT_MAX / 2

using engine::Node;
using engine::Param;
using engine::ParamType;

std::vector<utils::FileFilter> filters_for(const Node& node)
{
    if (node.type == "ImageInput") {
        return {{"图片文件", "png,jpg,jpeg,bmp,webp"}, {"所有文件", "*"}};
    }
    return {{"所有文件", "*"}};
}

// 校验当前参数，返回错误文案（空 = 通过）
std::string param_error(const Param& param)
{
    std::string error;
    engine::Graph::validateParam(param, &error);
    return error;
}

// P7a-15：「输入」分组的端口连通清单
//  * 判据与运行前校验一致：非 optional 且连接数 0 → 必填未连（红字）
//  * 变长端口显示已连条数（P7a-01 起「图片理解」的 image 为变长）
void draw_input_port_summary(const Node& node)
{
    if (node.inputs.empty()) {
        ImGui::TextDisabled("本节点没有输入端口");
        return;
    }
    ImGui::Separator();
    ImGui::TextDisabled("输入端口（%d）", static_cast<int>(node.inputs.size()));
    for (const engine::Port& port : node.inputs) {
        const int   connections = editor().graph.inputConnectionCount(node.id, port.id);
        std::string text        = port.display_name;
        if (port.is_variadic) {
            text += "（变长）";
        }
        if (connections > 0) {
            text += "：已连 " + std::to_string(connections) + " 条";
            if (port.is_variadic && connections > 1) {
                text += "（按顺序全部送入）";
            }
            ImGui::BulletText("%s", text.c_str());
        }
        else if (port.is_optional) {
            ImGui::TextDisabled("  · %s：未连接（可选）", text.c_str());
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::BulletText("%s：未连接（必填）", text.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void help_marker(const std::string& description)
{
    if (description.empty()) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string param_label(const Param& param)
{
    return param.display_name + (param.is_required ? " *" : "");
}

// --------------------------------------------- 新通道会话任务（M7B 批 3 step 11）--
//  ⚠️ **为什么必须有这个**：新通道（`web/pydoll_channel`）的函数**全是阻塞式**（`M7.md` `Q4`），
//  而登录 / 收尾 / 按站点注销都要等浏览器 —— 直接在 UI 线程调会让界面卡死。
//  ⇒ 统一走**后台线程**；UI 线程只读下面这份**缓存**（加锁读，微秒级）。
//
//  ⚠️ **M7B step 12（`M7B-44`）· 任务记账按 `node.id`**（**不再**是进程级单例）：
//   * 病象（2026-10-04 实测）：一份全局任务被**所有** ProviderConfig 节点共用 ⇒ 在节点 A 点的登录，
//     切到节点 B 也显示同一份状态；B 点按钮还会因 A 在跑而被**静默忽略**（跨节点串状态）。
//   * 现在：状态 / 禁用 / 去重**都只作用于本节点**；跨节点的只剩**进程级事实**（浏览器会话在不在跑）
//     —— 且必须**如实标注为进程级**（`I21` 同族：不冒充、不静默）。
//  * **同一节点内**：同一时刻只跑一个任务（连点 → **明确忽略 + 记日志**，**不排队**）
//  * 任务体**不得**把异常抛出线程（`std::thread` 里未捕获异常 = `std::terminate`）⇒ 统一兜底
//  * 进程退出前由 `wait_web_tasks()` 统一 join（`main.cpp` 调用）—— 避免静态对象析构时
//    仍有 joinable 线程（同样是 `std::terminate`）
// 「打开登录窗口」的登录态轮询上限（秒）—— **M7B step 12（`M7B-44`）**：`300` → `120`
//  * 理由（2026-10-04 实测）：轮询期间用户关掉窗口时，旧实现**每一轮都自愈重开**（一次点击 → 4 个窗口，
//    见 `M7B.md` step 12 物证）；自愈修掉之后，过长的上限只会让「没登成」的会话干等 5 分钟
//    ⇒ 收敛到 2 分钟，并把「窗口被关闭 = 本次登录结束」如实回报（`I21`）
constexpr int kLoginPollSeconds = 120;

// **M7B step 13（`M7B-45`）**：`Tab` = 「刷新当前 tab」观测任务（**只读**）
//  * 病象（2026-10-04 实测）：渲染路径每帧调 `tab_on_site()` = **同步 IPC**（新建连接 2 s 超时 +
//    `current_tab` 8 s 超时；实测 30–140 ms、最坏 2.1 s）⇒ 帧率 2–10 fps（登录后卡死）
//  * 修法：观测**只在后台线程**做，结果写进本节点缓存；渲染路径**只读缓存**（`A1` 渲染路径零 IPC）
enum class WebTaskKind { None, Login, Logout, Shutdown, Tab };

struct WebTask {
    std::mutex        mutex;
    bool              running = false;
    WebTaskKind       kind    = WebTaskKind::None;
    std::string       node_id;  // 归属节点（**拒绝跨节点串状态** · `M7B-44`）
    std::string       site_key; // 归属站点（显示 / 日志）
    std::string       title;    // 任务名（显示 / 日志）
    std::string       status;   // 最新状态（**可操作**文案，`I21`）
    bool              last_ok = false;
    // **M7B step 13（`M7B-45`）**：最近一次「当前 tab」观测（**只由后台任务写**）
    //  * `tab_known=false` ⇒ 界面如实显示「未观测」（**不假装**「不在当前 tab」 —— `I21`）
    bool              tab_known   = false;
    bool              tab_on_site = false;
    std::string       tab_current; // 观测到的当前站点（原始值；空 = 读不到）
    std::string       tab_at;      // 观测时间（本地时钟 `HH:MM:SS`）
    std::thread       worker;

    ~WebTask() { join_worker(); }

    void join_worker()
    {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

// 节点级任务表（`node.id` → 任务）
//  * 值类型含 `std::mutex` / `std::thread` ⇒ **不可移动** ⇒ 只用 `try_emplace` 就地构造
//  * `std::map` 是节点式容器：插入**不会**让已有元素的引用失效（`web_task(node_id)` 返回引用安全）
std::mutex                      g_web_tasks_mutex;
std::map<std::string, WebTask>& web_tasks()
{
    static std::map<std::string, WebTask> tasks;
    return tasks;
}

WebTask& web_task(const std::string& node_id)
{
    std::lock_guard<std::mutex> lock(g_web_tasks_mutex);
    WebTask&                    task = web_tasks().try_emplace(node_id).first->second;
    task.node_id                     = node_id;
    return task;
}

// 读缓存（UI 线程用；**不阻塞**）—— `node_id` 为空 = 无节点上下文 → 返回**空视图**
struct WebTaskView {
    bool        running = false;
    WebTaskKind kind    = WebTaskKind::None;
    std::string title;
    std::string status;
    bool        last_ok = false;
    // **M7B step 13（`M7B-45`）**：tab 观测快照（渲染路径**只**读这里，不再发 IPC）
    bool        tab_known   = false;
    bool        tab_on_site = false;
    std::string tab_current;
    std::string tab_at;
};

WebTaskView web_task_view(const std::string& node_id)
{
    WebTaskView view;
    if (node_id.empty()) {
        return view;
    }
    WebTask&                    task = web_task(node_id);
    std::lock_guard<std::mutex> lock(task.mutex);
    view.running     = task.running;
    view.kind        = task.kind;
    view.title       = task.title;
    view.status      = task.status;
    view.last_ok     = task.last_ok;
    view.tab_known   = task.tab_known;
    view.tab_on_site = task.tab_on_site;
    view.tab_current = task.tab_current;
    view.tab_at      = task.tab_at;
    return view;
}

void web_task_done(const std::string& node_id, const std::string& title, const std::string& status,
                   bool ok)
{
    WebTask&                    task = web_task(node_id);
    std::lock_guard<std::mutex> lock(task.mutex);
    task.running = false;
    task.title   = title;
    task.status  = status;
    task.last_ok = ok;
}

// **M7B step 13（`M7B-45`）**：写「当前 tab」观测结果 —— **只允许后台任务调用**
//  * 渲染路径**不得**调用（它只读 `web_task_view()`）：观测 = 发 IPC ⇒ 必须留在后台线程
//  * `known=false` = 读不到（会话未启动 / 浏览器已关闭）⇒ 界面如实显示「未观测」，不编造站点
void web_task_set_tab(const std::string& node_id, bool known, bool on_site,
                      const std::string& current, const std::string& at)
{
    WebTask&                    task = web_task(node_id);
    std::lock_guard<std::mutex> lock(task.mutex);
    task.tab_known   = known;
    task.tab_on_site = on_site;
    task.tab_current = current;
    task.tab_at      = at;
}

// 本地时钟 `HH:MM:SS`（观测时间戳；失败 → 空串 ⇒ 界面不显示时间，不编造）
std::string clock_now()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
    if (::localtime_s(&tm, &now) != 0) {
        return {};
    }
    char buffer[16] = {};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm) == 0) {
        return {};
    }
    return buffer;
}

// 起一个后台任务（**按节点去重**：同一节点已有任务在跑 → false + 记日志，**不排队**）
//  * `site_key` 只用于显示 / 日志（归属证据）；命令里的站点仍由调用方下发（`I14`）
bool start_web_task(const std::string& node_id, WebTaskKind kind, const std::string& site_key,
                    const std::string& title, std::function<void()> body)
{
    WebTask& task = web_task(node_id);
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        if (task.running) {
            log::info("[网页版会话] 节点（" + node_id + "）已有后台任务在运行（" + task.title +
                      "）→ 忽略该节点的重复请求：" + title);
            return false;
        }
    }
    task.join_worker();   // **本节点**上一个已结束：回收线程
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.running  = true;
        task.kind     = kind;
        task.node_id  = node_id;
        task.site_key = site_key;
        task.title    = title;
        task.status   = title + "…（后台执行 · 界面不会卡）";
        task.last_ok  = false;
    }
    task.worker = std::thread([node_id, body = std::move(body)] {
        try {
            body();
        }
        catch (const std::exception& ex) {
            web_task_done(node_id, "会话任务异常", std::string("异常：") + ex.what(), false);
        }
        catch (...) {
            web_task_done(node_id, "会话任务异常", "未知异常（详见 app.log）", false);
        }
    });
    return true;
}

// 任一节点有任务在跑 —— **只**用于「进程级」按钮给出**显式**禁用理由（不静默、不冒充本节点状态）
//  * `*who` = 正在执行的任务名，界面据此写出「在等谁」（`M7B-44`：跨节点的信息必须**明说**）
//  * **M7B step 13（`M7B-45`）**：`WebTaskKind::Tab`（刷新当前 tab）是**只读**观测 ⇒ **不算**
//    进程级操作，不构成「关闭浏览器会话」的禁用理由（`M7B-44` 纪律：禁用理由必须是真的）
bool any_web_task_running(std::string* who)
{
    std::lock_guard<std::mutex> lock(g_web_tasks_mutex);
    for (auto& entry : web_tasks()) {
        WebTask&                    task = entry.second;
        std::lock_guard<std::mutex> task_lock(task.mutex);
        if (task.kind == WebTaskKind::Tab) {
            continue;
        }
        if (task.running) {
            if (who != nullptr) {
                *who = task.title;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 网页版会话 --
// 登录窗口（独立线程，不阻塞主界面）：进程内单例见 web::login_window()
bool is_web_mode(const Node& node)
{
    const Param* mode = node.findParam("mode");
    return mode != nullptr && mode->text() == "web";
}

// ------------------------------------------------------------ 生效站点上下文 --
// M_patchB L1 续（PB2-17/18）：ProviderConfig 的「生效条目 → 站点参数 → 会话键」
//  * 生效条目 = 按**该节点自身参数**解析（PB2-20：ProviderConfig 自己就是配置来源，不看 provider 连线）
//  * 站点参数**只**来自条目自己的 `web` 段（PB2-22 / 决策 D-22② / `I14`：**不**回落内置默认站点）
//  * 会话键 = 站点 origin（SessionStore 按站点归档；决策 D-19）
struct WebSiteContext {
    engine::EffectiveProvider effective;
    ai::ProviderWebSpec       site;
    std::string               provider_id;    // 生效 web 条目 id（非网页版条目 = 空）
    bool                      on_web_entry = false; // 生效条目本身是不是网页版条目
    bool                      site_usable  = false; // 站点是否可用（false → 错误块，**不打开任何窗口**）
    std::string               site_error;     // 不可用原因（含三条引导；空 = 可用）
    std::string               field_warnings; // 可选字段缺失（回落 + 警告；决策 D-26）
    std::string               login_url;      // 生效登录页（仅 site_usable 时有意义）
    std::string               site_key;       // 站点键（origin）
    web::LoginRequest         manual;         // 「打开登录窗口」用（页面加载后自动探测）
    web::LoginRequest         probe;          // 「探测网页版协议」用
};

WebSiteContext web_site_context(const engine::Node& node, const engine::Graph& graph)
{
    WebSiteContext ctx;
    ctx.effective      = engine::resolve_display_provider(graph, node); // PB2-20：按自身条目解析
    ctx.on_web_entry   = ctx.effective.is_web();
    ctx.site           = ai::strict_web_spec_for(ctx.effective.spec);        // PB2-22：**不**回落
    ctx.provider_id    = ai::strict_web_provider_id_for(ctx.effective.spec);
    ctx.site_error     = ai::web_site_error(ctx.effective.spec);
    ctx.site_usable    = ctx.site_error.empty();
    ctx.field_warnings = ai::web_site_field_warnings(ctx.effective.spec);
    if (ctx.site_usable) {
        ctx.manual    = web::interactive_login_request(ctx.site, ctx.provider_id);
        ctx.probe     = web::probe_login_request(ctx.site, ctx.provider_id);
        ctx.login_url = ctx.manual.url;
        ctx.site_key  = web::login_request_site(ctx.manual);
    }
    return ctx;
}

// 该站点用于界面展示的 Cookie 名（**只**取条目自己的 `cookie_names`；空 = 该条目未声明 → 不显示具体 Cookie）
//  * L4（PB2-27 / 不变量 I15）：界面**不得**再回落到任何厂商专有 Cookie 名（一律以条目为准）
std::string site_cookie_name(const WebSiteContext& ctx)
{
    return ctx.site.cookie_names.empty() ? std::string() : ctx.site.cookie_names.front();
}

// 按**站点**注销（PB2-19 / M7B step 11）：清该站点的内存会话 + 删该 origin 的数据
// （**不影响**其他站点；不删除登录 profile）
//  * **新通道**：`channel::logout_site`（`Storage.clearDataForOrigin`）—— **阻塞** ⇒ 走后台任务
// 按**任意站点**注销（生效条目 + 已登录站点列表里的逐条「注销」都走这里）
//  * 新通道：`channel::logout_site` = `Storage.clearDataForOrigin`（**只清该 origin**）——
//    **阻塞** ⇒ 走后台任务；`key` 只用于显示（命令里的 origin 由 `site.url` 派生 · `I14`）
//  * `node_id` = 发起该操作的节点（**任务记账按节点** · `M7B-44`：不必再传进程级单例）
void logout_web_session_other(const std::string& node_id, const web::LoginRequest& site,
                              const std::string& key)
{
    const bool started = start_web_task(
        node_id, WebTaskKind::Logout, key, "按站点注销（" + key + "）", [node_id, site, key] {
            std::string error;
            if (web::channel::logout_site(site, 20000, &error)) {
                web_task_done(node_id, "按站点注销完成",
                              "已清该站点（" + key + "）的 Cookie / localStorage（其他站点不受影响）",
                              true);
            }
            else {
                web_task_done(node_id, "按站点注销失败", error, false);
            }
        });
    if (started) {
        log::info("网页版按站点注销：已在后台执行（节点 " + node_id + " · 站点 " + key + "）");
    }
}

// 按**生效条目站点**注销：站点参数只来自条目（PB2-17 / `I14`），转调上面的通用实现
void logout_web_session(const std::string& node_id, const WebSiteContext& ctx)
{
    logout_web_session_other(node_id, ctx.manual, ctx.site_key);
}

// 高级操作：删除**整个**登录 profile（= 清掉所有站点；PB2-19 / R14 边界说明）
//  * **M7B step 11**：profile 路径改 **Pydoll**（`paths::pydoll_profile()`）—— 这是「整体替换」后
//    用户实际在用的那一份；旧 `~/.brain-ai/webview2` 退役归批 5（`M7B-32`，**不自动删用户数据**）
void delete_web_profile(const std::string& node_id)
{
    // ① 旧通道（WebView2 内嵌窗口）收尾 —— 批 5 随其退役
    web::login_window().request_close();
    web::login_window().join();
    web::SessionStore::instance().clear_all();

    // ② 新通道会话（浏览器 + 守护进程）**不随本进程消失** ⇒ 必须显式关（阻塞 ⇒ 后台任务）
    const std::filesystem::path profile = paths::pydoll_profile();
    start_web_task(node_id, WebTaskKind::Shutdown, "（所有站点）", "删除整个登录 profile（所有站点）",
                   [node_id, profile] {
        web::channel::shutdown_session();
        std::error_code remove_ec;
        std::filesystem::remove_all(profile, remove_ec);
        if (remove_ec) {
            log::warn("网页版注销：profile 目录删除失败 " + profile.string() + "（" +
                      remove_ec.message() + "）");
            web_task_done(node_id, "删除 profile 失败",
                          "目录删除失败 " + profile.string() + "（" + remove_ec.message() + "）", false);
        }
        else {
            log::info("网页版注销：profile 已删除 " + profile.string() +
                      "（**全部站点**，下次需重新登录）");
            web_task_done(node_id, "登录 profile 已删除",
                          profile.string() + " 已删除（**全部站点**，下次需重新登录）", true);
        }
    });
}

// ProviderConfig 节点在 web 模式下显示「生效站点 + 会话状态 + 登录入口」（PB2-17/18/19）
//  * 站点身份唯一来源 = **生效条目自己的 `web` 段**（不变量 `I11` / `I14`）：登录页 / 窗口标题 / 探测路径 / Cookie 名
//  * 站点不可用（非网页版条目 / 缺 `web.login_url`）→ **错误块 + 三条引导 + 一键改选**，
//    且**不打开任何窗口**（PB2-22 / 决策 D-22②）
//  * 返回 true = 就地改了节点参数（外层据此压快照 + 标记变更）
bool draw_web_session_section(engine::Node& node, const engine::Graph& graph)
{
    const WebSiteContext ctx     = web_site_context(node, graph);
    bool                 changed = false;

    ImGui::Separator();
    ImGui::TextUnformatted("网页版会话（Cookie 只存内存，程序退出即销毁）");

    if (!ctx.site_usable) {
        // ---- 站点不可用：明确报错 + 一键改选网页版条目；两个会开窗的按钮**禁用** ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("✗ %s", ctx.site_error.c_str());
        ImGui::PopStyleColor();

        const std::vector<std::string> web_ids = ai::provider_specs().ids("web");
        if (!web_ids.empty()) {
            if (ImGui::Button(("把「提供商」改为「" + web_ids.front() + "」").c_str())) {
                if (engine::Param* provider = node.findParam("provider")) {
                    provider->value = web_ids.front();
                    if (engine::Param* mode = node.findParam("mode")) {
                        mode->value = std::string("web"); // 建议值（可再改回）
                    }
                    changed = true;
                    log::info("参数变更: " + node.id + ".provider → " + web_ids.front() +
                              "（按 D-22② 引导改选网页版条目，可撤销）");
                }
            }
        }
        ImGui::BeginDisabled(true);
        ImGui::Button("打开登录窗口（Pydoll）", ImVec2(-FLT_MIN, 0.0f));
        ImGui::Button("探测网页版协议（dev）", ImVec2(-FLT_MIN, 0.0f));
        ImGui::EndDisabled();
        ImGui::TextDisabled("（站点不可用：**不会**回落到内置默认站点 —— 请先选定网页版站点条目）");
        return changed;
    }

    const web::Session session = web::SessionStore::instance().snapshot(ctx.site_key);

    // ---- 生效站点（按条目）----
    ImGui::TextDisabled("站点条目：%s（来源 %s）",
                        ctx.effective.display.empty() ? ctx.effective.provider.c_str()
                                                      : ctx.effective.display.c_str(),
                        ctx.effective.spec_origin.empty() ? "未知" : ctx.effective.spec_origin.c_str());
    if (!ctx.field_warnings.empty()) {
        ImGui::TextDisabled("%s", ctx.field_warnings.c_str()); // 决策 D-26：回落 + 警告
    }
    ImGui::TextDisabled("登录页：%s", ctx.login_url.c_str());
    ImGui::TextDisabled("适配器：%s",
                        ctx.site.adapter.empty() ? "builtin" : ctx.site.adapter.c_str());

    // ---- 该站点会话状态（L4 / PB2-27：**站点无关**判据 —— 不看 userToken，不变量 I15）----
    const ai::WebSessionVerdict verdict =
        ai::web_session_state(ctx.effective.spec, web::web_session_evidence(session));
    const std::string cookie_name = site_cookie_name(ctx);
    if (verdict.state == ai::WebSessionState::logged_in) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "状态：已登录（该站点，Cookie %zu 条）",
                           session.cookie_count());
        ImGui::TextDisabled("来源：%s", session.url.c_str());
        ImGui::TextDisabled("更新：%s", session.updated_at.c_str());
        if (!cookie_name.empty()) {
            if (const web::Cookie* cookie = session.find(cookie_name)) {
                ImGui::TextDisabled("%s = %s", cookie_name.c_str(),
                                    web::mask_value(cookie->value).c_str());
            }
        }
    }
    else {
        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.30f, 1.0f), "状态：%s（该站点）",
                           ai::web_session_state_label(verdict.state).c_str());
    }
    ImGui::TextDisabled("%s", verdict.reason.c_str());
    if (verdict.state != ai::WebSessionState::logged_in) {
        ImGui::TextDisabled("点「打开登录窗口」在该站点**手动登录**；登录后**无需**再点「探测网页版协议」。");
    }

    // 协议探测状态（M4-06/M4-08 逆向用）
    //  * L4（PB2-27 / D-28①）：`userToken` 行**仅当**条目配了 `token_expr` 才显示；
    //    否则说明「本条目为 DOM 站点」（生成不依赖 userToken）
    //  * L4（PB2-27 / 不变量 I16）：**探测不适用**的站点不再显示红字「探测错误」（那是 DeepSeek 端点 404）
    const web::ProbeResult& probe = session.probe;
    if (ai::web_shows_user_token(ctx.effective.spec)) {
        if (probe.has_user_token) {
            ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "userToken：%s",
                               probe.user_token_masked.c_str());
        }
        else {
            ImGui::TextDisabled("userToken：未获取（网页版接口需要它，请先登录）");
        }
    }
    else {
        ImGui::TextDisabled("本条目为 DOM 站点：登录态由浏览器 profile 维持（生成不依赖 userToken）");
    }
    if (!probe.challenge_json.empty()) {
        ImGui::TextDisabled("PoW 挑战：已获取（%zu 字节，见 Console / app.log）",
                            probe.challenge_json.size());
    }
    if (!probe.error.empty() && ai::probe_is_applicable(ctx.site)) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "探测错误：%s", probe.error.c_str());
    }

    // ---- 浏览器会话（**M7B step 11**：新通道 Pydoll · **step 12**：按节点记账 `M7B-44`）----
    //  * `channel::session_ready()` = **纯查询**（不阻塞）⇒ UI 线程可直接调；⚠️ 它是**进程级**事实
    //    （整个进程一个守护进程 + 一个浏览器，各站点各占一个 tab · `MB-D2`）⇒ 必须**如实标注为进程级**，
    //    **不得**冒充「本节点」的状态（`M7B-44`：拒绝跨节点全局通知）
    //  * 登录 / 关会话 / 按站点注销都要等浏览器（**阻塞**）⇒ 一律走**后台任务**；
    //    UI 只读**本节点**的 `web_task_view(node.id)` 缓存（加锁读，微秒级）
    const bool        channel_alive = web::channel::session_ready();
    const WebTaskView task          = web_task_view(node.id);   // 本地快照（加锁读 · 微秒级）
    ImGui::TextDisabled("浏览器会话（进程级 · 影响所有站点）：%s",
                        channel_alive ? "运行中（Pydoll · profile 记住登录态）" : "未启动");
    // **M7B step 13（`M7B-45`）**：渲染路径**零 IPC**（不变量 `A1`）——
    //  「本节点站点 tab」**只读后台观测缓存**，**不再**每帧调 `tab_on_site()`
    //  * 旧实现在这一行发**同步 IPC**（新建连接 2 s 超时 + `current_tab` 8 s 超时；
    //    实测 30–140 ms、最坏 2.1 s）⇒ 帧率 2–10 fps（登录后卡死的根因）
    //  * 观测改由**后台任务**（`WebTaskKind::Tab`）执行，结果写进本节点缓存
    ImGui::TextDisabled("本节点站点 tab：%s",
                        !task.tab_known ? "未观测（点右侧「刷新」）"
                                        : (task.tab_on_site ? "在当前 tab" : "不在当前 tab"));
    ImGui::SameLine();
    ImGui::BeginDisabled(task.running);
    if (ImGui::SmallButton("刷新")) {
        const std::string node_id = node.id;
        const std::string key     = ctx.site_key;
        const std::string login   = ctx.login_url;
        if (start_web_task(node_id, WebTaskKind::Tab, key, "刷新当前 tab（" + key + "）",
                           [node_id, key, login] {
                // ⚠️ 本函数体在**后台线程**执行：同步 IPC 只允许出现在这里（`Q4`）
                const std::string current = web::channel::current_tab_site();
                if (current.empty()) {
                    web_task_set_tab(node_id, /*known=*/false, false, {}, clock_now());
                    web_task_done(node_id, "当前 tab 未观测到",
                                  "读不到当前 tab（会话未启动，或浏览器已被关闭）—— 界面**不会**"
                                  "自动重试：请点「打开登录窗口」，或稍后再点「刷新」。",
                                  false);
                    return;
                }
                // 归一比较（与旧面板 `tab_on_site` 语义一致）；`site` 回包已是 origin
                const bool here =
                    !key.empty() && web::site_key_of(current) == web::site_key_of(key);
                web_task_set_tab(node_id, /*known=*/true, here, current, clock_now());
                web_task_done(node_id, here ? "当前 tab 在本站点" : "当前 tab 在其他站点",
                              "已观测当前 tab：" + current, true);
            })) {
            log::info("网页版会话：已在后台刷新「当前 tab」（节点 " + node_id + " · 站点 " +
                      login + "）");
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("在**后台线程**读一次当前 tab（同步 IPC 只允许出现在后台线程 —— `Q4`）；\n"
                          "渲染路径永远不发 IPC（`M7B-45` 修掉的卡顿根因）。");
    }
    if (task.tab_known && !task.tab_at.empty()) {
        ImGui::TextDisabled("（上次观测 %s：%s）", task.tab_at.c_str(),
                            task.tab_current.empty() ? "（空）" : task.tab_current.c_str());
    }
    // 本节点最近一次会话任务（**只**属于本节点 —— 别的节点在跑不会显示在这里）
    if (!task.status.empty()) {
        ImGui::TextColored(task.last_ok ? ImVec4(0.31f, 0.75f, 0.42f, 1.0f)
                                        : ImVec4(0.85f, 0.70f, 0.30f, 1.0f),
                           "本节点最近任务：%s", task.status.c_str());
    }
    // 「关闭浏览器会话」是**进程级**操作 ⇒ 任一节点有任务时必须禁用，且**写出归属**
    // （不静默禁用、不借别的节点的状态文案 —— `M7B-44`）
    std::string busy_title;
    const bool  busy_anywhere = any_web_task_running(&busy_title);
    if (channel_alive) {
        ImGui::BeginDisabled(busy_anywhere);
        if (ImGui::Button("关闭浏览器会话（进程级 · 所有站点）", ImVec2(-FLT_MIN, 0.0f))) {
            const std::string node_id = node.id;
            start_web_task(node_id, WebTaskKind::Shutdown, ctx.site_key, "关闭浏览器会话",
                           [node_id] {
                // `shutdown_session()` = `Browser.close` → 等进程退出（≤60 s）—— 阻塞 ⇒ 后台
                if (web::channel::shutdown_session()) {
                    web_task_done(node_id, "浏览器会话已关闭",
                                  "守护进程 + 浏览器已收尾（登录态已落盘）", true);
                }
                else {
                    web_task_done(node_id, "浏览器会话", "本次没有活动会话（无需关闭）", true);
                }
            });
            log::info("网页版会话：已在后台关闭浏览器会话（由节点 " + node_id +
                      " 触发 · **所有站点**）");
        }
        ImGui::EndDisabled();
        if (busy_anywhere) {
            ImGui::TextDisabled("（进程级操作暂不可用：正在执行「%s」）", busy_title.c_str());
        }
    }

    ImGui::Spacing();
    // **只**受**本节点**任务控制（别的节点在跑不禁用本节点按钮 —— 拒绝跨节点串状态）
    ImGui::BeginDisabled(task.running);
    if (ImGui::Button("打开登录窗口（Pydoll）", ImVec2(-FLT_MIN, 0.0f))) {
        // 站点参数来自**生效条目**（PB2-17 / `I14`：无站点回落）；值拷贝进后台线程
        const web::LoginRequest site    = ctx.manual;
        const std::string       login   = ctx.login_url;
        const std::string       node_id = node.id;
        const bool              started = start_web_task(
            node_id, WebTaskKind::Login, ctx.site_key, "打开登录窗口（" + ctx.site_key + "）",
            [node_id, site, login] {
                std::string error;
                if (web::channel::login_site(site, kLoginPollSeconds, &error)) {
                    web_task_done(node_id, "登录成功",
                                  "该站点已登录（" + login + "）；浏览器窗口保持打开，供网页节点复用",
                                  true);
                }
                else {
                    web_task_done(node_id, "登录未完成", error, false);
                }
            });
        if (started) {
            log::info("网页版登录窗口：已在后台打开（节点 " + node_id + " · 站点 " + login + "）");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("站点取自「生效条目」：%s\n"
                          "会弹出**独立浏览器窗口**（profile = ~/.brain-ai/pydoll-profile）；\n"
                          "登录完成后窗口**保持打开** —— 网页版文字生成会复用同一会话。\n"
                          "登录态判据 = 条目 `web.cookie_names` 命中（不变量 I15′）；\n"
                          "**关掉该窗口 = 结束本次登录**（本通道不会再自动弹窗；可重新点本按钮）。",
                          ctx.login_url.c_str());
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    // L4（PB2-28 / I16）：协议探测只对**内置协议站点**适用；DOM 站点改跑**只读诊断**
    //  * ⚠️ **本按钮仍走旧通道**（`web::login_window()` 的内嵌窗口：探测要在页面内取
    //    userToken / 解 PoW）—— 该能力属 `builtin:deepseek` 协议栈，批 5 随其退役（`M7B-42`）；
    //    新通道侧的对应能力是 CLI `--web-dom-dump` / `--web-adapter-selftest`（step 9 已换代）
    web::LoginWindow& window        = web::login_window();
    const bool        probe_applicable = ai::probe_is_applicable(ctx.site);
    if (ImGui::Button(probe_applicable ? "探测网页版协议（dev）"
                                       : "只读诊断（该站点不适用协议探测）",
                      ImVec2(-FLT_MIN, 0.0f))) {
        if (web::window_on_site(ctx.site_key)) {
            web::request_protocol_probe(); // 窗口已开在该站点：直接在当前页面里探测 / 诊断
        }
        else {
            if (window.running()) {
                window.request_close();
                window.join();
            }
            std::string error;
            if (!window.start(ctx.probe, &error)) {
                log::warn("启动探测窗口失败: " + error);
            }
        }
        log::info(probe_applicable
                      ? ("已触发网页版协议探测（站点 " + ctx.login_url + "；结果写入 Console 与 app.log）")
                      : ("已触发只读诊断（站点 " + ctx.login_url +
                         "；该站点不适用协议探测：只读页面信息，不请求端点、不读 userToken）"));
    }
    if (ImGui::IsItemHovered()) {
        if (probe_applicable) {
            ImGui::SetTooltip("在已登录页面内读取 userToken 并请求 PoW 挑战（同源，绕过跨域与反爬）；\n"
                              "Token 只以脱敏形式记录");
        }
        else {
            ImGui::SetTooltip("该条目不是内置协议站点（无 PoW / 站点端点）：只读检查页面 URL / 标题 /\n"
                              "localStorage 键名 / Cookie 名 / 输入框候选数 —— **不**请求任何站点端点，\n"
                              "也**不**读取 userToken；登录态由该站点 Cookie 判定。");
        }
    }

    if (verdict.state == ai::WebSessionState::logged_in) {
        ImGui::Spacing();
        if (ImGui::Button("注销该站点（清会话 + 删该站点 Cookie）", ImVec2(-FLT_MIN, 0.0f))) {
            logout_web_session(node.id, ctx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("只影响本站点：内存会话 + 该 origin 的 Cookie / localStorage；\n"
                              "其他站点（例如另一个网页版条目）的登录态不受影响。");
        }
    }

    // ---- 已登录站点列表（多站点并存的可视化 + 逐站点注销；PB2-18/19）----
    const std::vector<std::string> sites = web::SessionStore::instance().sites();
    if (!sites.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("已登录站点（%zu）", sites.size());
        for (const std::string& site : sites) {
            const web::Session item = web::SessionStore::instance().snapshot(site);
            ImGui::PushID(site.c_str());
            ImGui::TextDisabled("%s（Cookie %zu%s）", site.c_str(), item.cookie_count(),
                                item.user_token.empty() ? "" : "，有凭证");
            ImGui::SameLine();
            if (ImGui::SmallButton("注销")) {
                // 新通道：按 origin 清站点数据（`Storage.clearDataForOrigin`）—— 阻塞 ⇒ 后台任务
                web::LoginRequest other;
                other.url           = site; // 只需 origin：注销按站点
                other.provider_id   = ctx.provider_id;
                logout_web_session_other(node.id, other, site);
            }
            ImGui::PopID();
        }
    }

    // ---- 高级：删除整个登录 profile（= 清掉**所有**站点；二次确认）----
    ImGui::Spacing();
    static bool confirm_delete_profile = false;
    if (!confirm_delete_profile) {
        if (ImGui::SmallButton("高级：删除整个登录 profile（所有站点）")) {
            confirm_delete_profile = true;
        }
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "将删除 ~/.brain-ai/pydoll-profile（新通道浏览器 profile）"
                           "—— 所有站点都要重新登录");
        if (ImGui::SmallButton("确认删除##profile")) {
            delete_web_profile(node.id);
            confirm_delete_profile = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("取消##profile")) {
            confirm_delete_profile = false;
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped("新通道登录：弹出**独立浏览器窗口**（Pydoll · profile ~/.brain-ai/pydoll-profile）；"
                       "登录后窗口保持打开，供网页版节点复用。登录 / 收尾 / 注销均在**后台执行**，"
                       "界面不会卡（进度见上方状态行与 app.log）。");

    return changed;
}

// 小写副本（参数搜索过滤用：ASCII 大小写不敏感，中文按字节比较）
std::string lower_copy(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

// 立即写回参数值（**不依赖"失焦判定"**）；返回是否发生变化
//  * 背景：原先统一写 `Widget(...) && IsItemDeactivatedAfterEdit()`，而"值发生变化"与"控件失焦"
//    往往不在同一帧（多行文本尤其明显）→ 会出现「改了文本内容，运行结果还是旧值」。
//    现改为：值变化即写回模型；`edited` 只在编辑结束时通知外层（日志/状态栏不刷屏）。
bool write_param(Param& param, const nlohmann::json& value)
{
    if (param.value == value) {
        return false;
    }
    param.value = value;
    return true;
}

// ============================================================================
//  M_patchB L4 / v16（**仅界面层**）：提供商下拉的「合并显示」
//
//  背景：配置表里同一家 AI 常有两条 —— 官方 API 条目（`deepseek`）与网页版条目（`deepseek-web`），
//        下拉里就出现两个名字（且 `-web` 后缀像内部 id，用户困惑）。
//  规则（**只改界面**：配置表 / `ai/**` / `engine/**` / `nodes/**` 一律不动）：
//    ① `xxx-web` 存在同名无后缀条目 `xxx`（如 `deepseek`+`deepseek-web`、`gemini`+`gemini-web`）
//       → **合并为一项**（显示 `xxx`，下拉里不再出现 `xxx-web`）：
//          · 「模式」= web      → 节点 `provider` 写入 `xxx-web`（运行期照常解析该网页站点）
//          · 「模式」= official → 写入 `xxx`
//    ② `xxx-web` 没有同名无后缀条目（如 `kimi-web` / `chatgpt-web`）→ 仍**独立一项**，
//       但显示名**去掉 `-web` 后缀**（即下拉里永远不会出现 `-web` 字样）
//    ③ 其它条目（官方 API / 自定义）原样显示
//  关键性质：合并只是「一个显示项 ↔ 两个真实 id」。写进节点的 id **始终是表内真实存在的 id**，
//            因此生效解析、`--provider-*` 自检、工作流 JSON、网页版会话键控全部无需改动。
// ============================================================================
struct ProviderChoice {
    std::string id;     // 主 id（无后缀条目；无后缀可用时 = 该 web 条目自身）
    std::string web_id; // 网页版孪生 id（空 = 无孪生）
    std::string label;  // 下拉显示名（永不含 `-web`）
};

// `base_id` 是否有网页版孪生（`base_id-web` 且 kind=web）
std::string web_twin_of(const ai::ProviderSpecs& table, const std::string& base_id)
{
    const std::string       candidate = base_id + "-web";
    const ai::ProviderSpec* spec      = table.find(candidate);
    return (spec != nullptr && spec->kind == "web") ? candidate : std::string();
}

// `id` 是否是「已被合并」的 web 条目（存在同名无后缀条目）
bool is_merged_web_id(const ai::ProviderSpecs& table, const std::string& id)
{
    const std::string suffix = "-web";
    if (id.size() <= suffix.size() ||
        id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    return table.find(id.substr(0, id.size() - suffix.size())) != nullptr;
}

// 构建下拉项（每次绘制重建：条目很少，且「重新加载配置表」后立即生效）
//  配对来源两条（**只在界面层**）：
//   ① 同名后缀规则：`xxx` ↔ `xxx-web`（deepseek / gemini …）
//   ② 品牌别名表：id 不同名但同属一家（openai ↔ chatgpt-web、anthropic ↔ claude-web、zhipu ↔ chatglm-web）
std::vector<ProviderChoice> build_provider_choices()
{
    const ai::ProviderSpecs& table = ai::provider_specs();

    struct AliasPair {
        const char* official;
        const char* web;
    };
    static const AliasPair kAliases[] = {
        {"openai", "chatgpt-web"},     // OpenAI（API） ↔ ChatGPT 网页版
        {"anthropic", "claude-web"},   // Anthropic Claude（API） ↔ Claude 网页版
        {"zhipu", "chatglm-web"},      // 智谱 GLM（API） ↔ 智谱清言网页版
        {"deepseek", "deepseek-web"},  // 与后缀规则一致（冗余写明，便于阅读）
        {"gemini", "gemini-web"},
    };

    std::vector<ProviderChoice> out;
    out.reserve(table.items.size());
    std::vector<std::string> merged_web_ids; // 已被合并的 web id（不再单列）
    for (const ai::ProviderSpec& spec : table.items) {
        if (spec.kind != "web") {
            continue;
        }
        if (is_merged_web_id(table, spec.id)) {
            merged_web_ids.push_back(spec.id);
            continue;
        }
        for (const AliasPair& alias : kAliases) {
            if (spec.id == alias.web && table.find(alias.official) != nullptr) {
                merged_web_ids.push_back(spec.id);
                break;
            }
        }
    }
    const auto is_merged = [&merged_web_ids](const std::string& id) {
        return std::find(merged_web_ids.begin(), merged_web_ids.end(), id) != merged_web_ids.end();
    };

    for (const ai::ProviderSpec& spec : table.items) {
        if (spec.kind == "web" && is_merged(spec.id)) {
            continue; // → 合并进它的官方条目，不再单列
        }
        ProviderChoice item;
        if (spec.kind == "web") {
            item.id    = spec.id;
            item.label = spec.id.size() > 4 ? spec.id.substr(0, spec.id.size() - 4) : spec.id;
        }
        else {
            item.id     = spec.id;
            item.label  = spec.id;
            item.web_id = web_twin_of(table, spec.id); // ① 同名后缀
            if (item.web_id.empty()) {                 // ② 品牌别名
                for (const AliasPair& alias : kAliases) {
                    if (spec.id == alias.official && table.find(alias.web) != nullptr) {
                        item.web_id = alias.web;
                        break;
                    }
                }
            }
        }
        out.push_back(item);
    }

    // 诊断（仅在结果变化时打印一次）：便于核对「合并后下拉里到底有哪些项」
    {
        static std::string last_signature;
        std::string        signature;
        for (const ProviderChoice& choice : out) {
            signature += choice.label + "[" + choice.id +
                         (choice.web_id.empty() ? "" : ("/" + choice.web_id)) + "] ";
        }
        if (signature != last_signature) {
            last_signature = signature;
            log::info("[提供商标] 下拉合并结果（界面层）：" + signature);
        }
    }
    return out;
}

// 切换「模式」时让 `provider` 跟随其孪生 id（official ↔ web）——**只改节点的参数值**，
// 使「不带 -web 的条目」在 web 模式下也能正常工作（写进去的仍是表内真实 id）
void sync_provider_id_with_mode(Node& node)
{
    const engine::Param* mode_param = node.findParam("mode");
    engine::Param*       provider   = node.findParam("provider");
    if (mode_param == nullptr || provider == nullptr) {
        return;
    }
    const std::string mode    = mode_param->text();
    const std::string current = provider->text();
    for (const ProviderChoice& choice : build_provider_choices()) {
        if (choice.id != current && (choice.web_id.empty() || choice.web_id != current)) {
            continue;
        }
        const std::string wanted =
            (mode == "web" && !choice.web_id.empty()) ? choice.web_id : choice.id;
        if (wanted == current) {
            return;
        }
        provider->value = wanted;
        log::info("参数变更: " + node.id + ".provider → " + wanted +
                  "（随「模式」配对到该条目的网页版 / 官方版；可撤销）");
        return;
    }
}

// 绘制单个参数控件
//  返回 edited = 本次编辑结束（失焦/回车）；begin_edit = 控件刚被激活（值尚未变化，
//  外层在此刻压撤销快照）；changed_now = 值已写回模型（编辑过程中每帧都可能为 true）
bool draw_param_widget(Node& node, Param& param, bool& begin_edit, bool& changed_now,
                       const std::vector<std::string>* enum_override = nullptr)
{
    bool edited             = false;
    const std::string label = param_label(param);
    (void)label;

    const auto note_activation = [&begin_edit]() {
        if (ImGui::IsItemActivated()) {
            begin_edit = true;
        }
    };

    ImGui::PushID(param.id.c_str());

    switch (param.type) {
    case ParamType::String: {
        std::string value         = param.text();
        ImGuiInputTextFlags flags = 0;
        if (param.is_secret) {
            flags |= ImGuiInputTextFlags_Password;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool touched =
            ImGui::InputTextWithHint("##value", param.display_name.c_str(), &value, flags);
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Text: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool touched =
            ImGui::InputTextMultiline("##value", &value,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f));
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Int: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        int value = static_cast<int>(std::lround(param.number(fallback)));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        // 修复（崩溃）：SliderInt 只支持 ±IM_S32_MAX/2；范围越界（如 0..2147483647）时
        //  改用 InputInt，并把输入裁回参数声明的范围，避免 ImGui 断言 abort。
        const bool in_slider_range =
            has_range && (*param.min_value >= -kSliderLimitInt) && (*param.max_value <= kSliderLimitInt);
        const bool touched =
            in_slider_range ? ImGui::SliderInt("##value", &value, static_cast<int>(*param.min_value),
                                              static_cast<int>(*param.max_value))
                            : ImGui::InputInt("##value", &value);
        if (touched) {
            if (has_range) {
                const double clamped = std::clamp(static_cast<double>(value), *param.min_value, *param.max_value);
                value                = static_cast<int>(std::lround(clamped));
            }
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Float: {
        const double fallback =
            param.default_value.is_number() ? param.default_value.get<double>() : 0.0;
        float value = static_cast<float>(param.number(fallback));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool has_range = param.min_value.has_value() && param.max_value.has_value();
        // 与 Int 同理：SliderFloat 只支持 ±FLT_MAX/2；越界时降级为 InputFloat + 裁剪。
        const bool in_slider_range = has_range && std::isfinite(*param.min_value) &&
                                     std::isfinite(*param.max_value) &&
                                     (*param.min_value >= -kSliderLimitFloat) &&
                                     (*param.max_value <= kSliderLimitFloat);
        const bool touched =
            in_slider_range ? ImGui::SliderFloat("##value", &value, static_cast<float>(*param.min_value),
                                                static_cast<float>(*param.max_value), "%.2f")
                            : ImGui::InputFloat("##value", &value, 0.1f, 1.0f, "%.2f");
        if (touched) {
            if (has_range) {
                value = static_cast<float>(
                    std::clamp(static_cast<double>(value), *param.min_value, *param.max_value));
            }
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Bool: {
        bool value = param.flag();
        if (ImGui::Checkbox("##value", &value)) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    case ParamType::Enum: {
        const std::string current = param.text();
        // ---- v16（仅界面）：提供商下拉「合并显示」（`-web` 项合并 / 去后缀；见上方说明）----
        if (param.id == "provider") {
            const std::vector<ProviderChoice> choices    = build_provider_choices();
            const engine::Param*              mode_param = node.findParam("mode");
            const std::string mode = mode_param != nullptr ? mode_param->text() : std::string();
            std::vector<std::string> labels;
            std::vector<std::string> values; // 与 labels 对齐：选中后写入 `provider` 的真实 id
            labels.reserve(choices.size() + 1);
            values.reserve(choices.size() + 1);
            int index = -1;
            for (const ProviderChoice& choice : choices) {
                labels.push_back(choice.label);
                values.push_back((mode == "web" && !choice.web_id.empty()) ? choice.web_id
                                                                          : choice.id);
                if (choice.id == current ||
                    (!choice.web_id.empty() && choice.web_id == current)) {
                    index = static_cast<int>(labels.size()) - 1;
                }
            }
            if (index < 0) {
                // 表外值（手改 JSON / 旧工作流里的其它 id）：**如实显示**，不静默替换
                labels.insert(labels.begin(), current);
                values.insert(values.begin(), current);
                index = 0;
            }
            std::vector<const char*> items;
            items.reserve(labels.size());
            for (const std::string& choice_label : labels) {
                items.push_back(choice_label.c_str());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!items.empty() &&
                ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
                const int safe_index = std::clamp(index, 0, static_cast<int>(values.size()) - 1);
                const std::string wanted = values[static_cast<std::size_t>(safe_index)];
                if (wanted != current) {
                    write_param(param, wanted); // 写入的始终是表内真实 id（运行期照常解析）
                }
                edited      = true;
                changed_now = true;
            }
            note_activation();
            break;
        }
        // 允许调用方按上下文覆盖可选项（扩展位）。
        //  * ⚠️ M_patchB L2（PB2-20 / 决策 D-21）：**不得**用它裁剪 ProviderConfig 的「模式」
        //    （恒 {official, web}，不变量 I13）；目前无调用方传入
        //  * 若当前值不在可选项里，**如实显示当前值**（调用方给出提示）
        std::vector<std::string> options =
            enum_override != nullptr ? *enum_override : param.enum_options;
        if (!current.empty() &&
            std::find(options.begin(), options.end(), current) == options.end()) {
            options.insert(options.begin(), current);
        }
        int index = 0;
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (options[i] == current) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::vector<const char*> items;
        items.reserve(options.size());
        for (const std::string& option : options) {
            items.push_back(option.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        // 注意：Combo 在"点击下拉项"的当帧就返回 true，而该帧的 IsItemDeactivatedAfterEdit()
        // 并不成立（被点击的是弹出层里的选项，属于另一个 item），
        // 因此这里必须"值变化即写回"，否则会出现「选了 Web 但模式没变」的现象。
        if (!items.empty() &&
            ImGui::Combo("##value", &index, items.data(), static_cast<int>(items.size()))) {
            const int safe_index = std::clamp(index, 0, static_cast<int>(options.size()) - 1);
            param.value          = options[static_cast<std::size_t>(safe_index)];
            edited               = true;
            changed_now          = true;
        }
        // ---- v16（仅界面）：切换「模式」→ 让 `provider` 配对到该条目的网页版 / 官方版 ----
        //  * 使「不带 `-web` 的条目」（如 `deepseek` / `gemini`）在 web 模式下直接可用
        //  * 写入的仍是表内真实 id → 运行期与自检无需任何改动；可撤销
        if (param.id == "mode" && changed_now) {
            sync_provider_id_with_mode(node);
        }
        note_activation();
        break;
    }
    case ParamType::File: {
        // P7a-02：File 参数支持**多值**（每行一个路径；换行为分隔符，见 utils/paths.h）
        //  * 2 行输入框 + 右侧按钮组（浏览…=替换 / 多选…=追加去重 / 清空）
        //  * 多行控件的 SameLine 落点不可靠 → 用 SetCursorScreenPos 定位按钮组
        std::string value = param.text();

        const ImVec2 start    = ImGui::GetCursorScreenPos();
        const float  spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float  button_w = 84.0f;
        const float  field_w  = ImGui::GetContentRegionAvail().x - button_w - spacing;
        const float  field_h  = ImGui::GetTextLineHeight() * 2.0f +
                               ImGui::GetStyle().FramePadding.y * 2.0f;

        const bool touched =
            ImGui::InputTextMultiline("##value", &value, ImVec2(field_w, field_h));
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();

        const ImVec2 after_field = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x + field_w + spacing, start.y));
        ImGui::BeginGroup();
        if (ImGui::Button("浏览…", ImVec2(button_w, 0.0f))) {
            const std::string picked = utils::open_file(filters_for(node), param.text());
            if (!picked.empty()) {
                write_param(param, picked);
                edited      = true;
                changed_now = true;
            }
        }
        if (ImGui::Button("多选…", ImVec2(button_w, 0.0f))) {
            const std::vector<std::string> picked =
                utils::open_files(filters_for(node), param.text());
            if (!picked.empty()) {
                std::vector<std::string> merged = paths::split_path_list(param.text());
                for (const std::string& item : picked) {
                    if (std::find(merged.begin(), merged.end(), item) == merged.end()) {
                        merged.push_back(item);
                    }
                }
                write_param(param, paths::join_path_list(merged));
                edited      = true;
                changed_now = true;
            }
        }
        if (ImGui::Button("清空", ImVec2(button_w, 0.0f))) {
            write_param(param, std::string());
            edited      = true;
            changed_now = true;
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(after_field);

        const std::vector<std::string> entries = paths::split_path_list(param.text());
        const int count = static_cast<int>(entries.size());
        if (count > 1) {
            ImGui::TextDisabled("共 %d 张图片（每行一个路径）", count);
        }

        // P7a-04/07：令牌 → 资源目录里的真实路径（可见完整路径；缺失时红字告警）
        bool has_legacy = false;
        for (const std::string& entry : entries) {
            if (asset::needs_migration(entry)) {
                has_legacy = true;
                continue;
            }
            bool              missing = false;
            std::string       resolve_error;
            const std::string local = asset::to_local_path(entry, &missing, &resolve_error);
            if (missing || local.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                                   resolve_error.empty() ? "图片资源缺失（可能已被删除）"
                                                         : resolve_error.c_str());
                ImGui::PopStyleColor();
            }
            else {
                ImGui::TextDisabled("→ %s", local.c_str());
            }
        }

        // P7a-06：外部路径 → 一键迁进资源目录（失败项**保持原值**，不丢用户数据）
        if (has_legacy) {
            ImGui::TextDisabled("其中含**外部路径**：迁移进资源目录后，换目录 / 换机仍能取到图片");
            if (ImGui::Button("迁移到资源目录", ImVec2(150.0f, 0.0f))) {
                std::vector<std::string> migrated;
                std::vector<std::string> failures;
                for (const std::string& entry : entries) {
                    if (!asset::needs_migration(entry)) {
                        migrated.push_back(entry);
                        continue;
                    }
                    std::string       import_error;
                    const std::string token = asset::import_file(entry, &import_error);
                    if (token.empty()) {
                        if (!import_error.empty()) {
                            failures.push_back(import_error);
                        }
                        migrated.push_back(entry);
                        continue;
                    }
                    migrated.push_back(token);
                }
                write_param(param, paths::join_path_list(migrated));
                edited      = true;
                changed_now = true;
                log::info("[资源目录] 迁移完成：" + std::to_string(entries.size()) + " 项，失败 " +
                          std::to_string(failures.size()) + " 项");
                if (!failures.empty()) {
                    log::error("[资源目录] 迁移失败：" + failures.front());
                }
            }
        }
        break;
    }
    case ParamType::Directory: {
        std::string value = param.text();
        ImGui::SetNextItemWidth(-FLT_MIN - 90.0f);
        const bool touched = ImGui::InputTextWithHint("##value", "(未选择)", &value);
        if (touched) {
            changed_now = write_param(param, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        ImGui::SameLine();
        if (ImGui::Button("选择…", ImVec2(80.0f, 0.0f))) {
            const std::string picked = utils::pick_folder(param.text());
            if (!picked.empty()) {
                write_param(param, picked);
                edited      = true;
                changed_now = true;
            }
        }
        break;
    }
    case ParamType::Color: {
        // 以 "#RRGGBB" 字符串存储
        const std::string hex = param.text();
        float rgb[3]          = {1.0f, 1.0f, 1.0f};
        if (hex.size() == 7 && hex[0] == '#') {
            for (int i = 0; i < 3; ++i) {
                rgb[i] = static_cast<float>(
                    std::strtol(hex.substr(static_cast<std::size_t>(1 + i * 2), 2).c_str(), nullptr, 16) /
                    255.0);
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::ColorEdit3("##value", rgb)) {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
                          static_cast<int>(rgb[0] * 255.0f), static_cast<int>(rgb[1] * 255.0f),
                          static_cast<int>(rgb[2] * 255.0f));
            changed_now = write_param(param, std::string(buffer));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            edited = true;
        }
        note_activation();
        break;
    }
    }

    ImGui::PopID();
    return edited;
}

} // namespace

void draw_property_panel(const char* window_title, bool* open, Node* node,
                         PropertyEditResult& result)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(window_title, open)) {
        ImGui::End();
        return;
    }

    if (node == nullptr) {
        ImGui::TextDisabled("未选中节点");
        ImGui::Separator();
        ImGui::TextWrapped("在画布中单击一个节点即可编辑其参数；"
                           "Ctrl+单击 或框选可多选，右键菜单可复制 / 删除。");
        ImGui::End();
        return;
    }

    engine::registerAllNodes();
    const engine::Definition* definition = engine::NodeRegistry::instance().find(node->type);

    // ---- P7a-15：三组可折叠（输入 / 参数 / 运行状态）----
    //  * 折叠状态由 ImGui 持久化到 `io.IniFilename`（app.cpp 已指向数据目录）→ **跨运行保留**
    if (ImGui::CollapsingHeader("输入", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ---- 概览 ----
        ImGui::Text("节点 %s", node->id.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", node->type.c_str());
        ImGui::TextDisabled("分类: %s    状态: %s",
                            definition != nullptr ? engine::categoryName(definition->category) : "未知",
                            engine::nodeStateName(node->state));

        ImGui::TextUnformatted("标题");
        if (editor().request_focus_title) { // 双击节点 / 右键"改名" → 自动聚焦
            ImGui::SetKeyboardFocusHere();
            editor().request_focus_title = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool title_touched = ImGui::InputTextWithHint("##title", "节点标题", &node->title);
        if (title_touched || ImGui::IsItemDeactivatedAfterEdit()) {
            result.renamed = true;
        }
        if (ImGui::IsItemActivated()) {
            result.begin_edit = true; // 改名前的快照
        }

        if (definition != nullptr && !definition->description.empty()) {
            ImGui::TextWrapped("%s", definition->description.c_str());
        }

        // P7a-15：输入端口连通清单（必填未连 → 红字）
        draw_input_port_summary(*node);
    }

    // ---- P7a-15：「参数」分组（可折叠）----
    //  * 分组体为既有代码，**保持原缩进**以便本次改动可审（纯格式差异，功能不受影响）
    if (ImGui::CollapsingHeader("参数", ImGuiTreeNodeFlags_DefaultOpen)) {
    // ---- 生效提供商（P1-a）：provider 输入优先，覆盖节点自身参数 ----
    // 解决"改了节点「模式」却不生效"的困惑；official（官方 API）与图片理解（M5-02）给出红字与提示
    if (engine::uses_provider(node->type)) {
        const engine::EffectiveProvider effective =
            engine::resolve_effective_provider(editor().graph, *node);
        if (node->type == "LLMGenerate") {
            if (effective.from_edge) {
                ImGui::TextDisabled("生效：%s（来自 提供商配置 %s）· 模型 %s", effective.mode.c_str(),
                                    effective.source_node.c_str(), effective.model.c_str());
            }
            else {
                ImGui::TextDisabled("生效：%s（节点自身设置；provider 输入未连接）",
                                    effective.mode.c_str());
            }
        }
        else if (node->type == "VLMGenerate") {
            // M5-02：图片理解只走 official（OpenAI 兼容多模态）；模型名来自「模型（自定义）」
            ImGui::TextDisabled("生效：图片理解 · 模式 %s · 模型 %s%s", effective.mode.c_str(),
                                effective.model.empty() ? "（未设置）" : effective.model.c_str(),
                                effective.from_edge ? "" : "（provider 输入未连接）");
        }

        const std::string reason = engine::unwired_reason(editor().graph, *node);
        if (!reason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s：本次运行该节点必定失败，下游会被跳过。", reason.c_str());
            ImGui::PopStyleColor();

            // 「已接线」提示与一键切换只针对文本生成（图片理解没有网页版入口，见 M5-02）
            if (node->type == "LLMGenerate" && engine::official_not_wired(effective)) {
                ImGui::TextDisabled(
                    "网页版已接线（provider.mode = web）：切换后即可真实生成，需先登录一次。");

                const std::string target_id = effective.from_edge ? effective.source_node : node->id;
                const std::string target_label =
                    effective.from_edge ? "提供商配置 " + target_id : std::string("本节点");
                if (ImGui::Button(("把 " + target_label + " 改为 web").c_str())) {
                    if (engine::Node* target = editor().graph.findNode(target_id)) {
                        if (engine::Param* mode = target->findParam("mode")) {
                            editor().snapshot("切换提供商为网页版"); // 先压快照：可撤销
                            mode->value = std::string("web");
                            editor().set_status(target_label + " 已切换为网页版（可撤销）");
                            log::info("[参数面板] " + target_label + " mode → web");
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("网页版走 DeepSeek 网页版会话（需已登录一次）；\n"
                                      "官方 API（API Key）已接线（PB-05）：填 Key 即可用");
                }
            }
        }
    }

    // ---- 校验汇总（设计 §6.3；条件隐藏的参数不参与校验）----
    std::vector<std::string> errors;
    for (const Param& param : node->params) {
        if (!engine::param_visible(*node, param)) {
            continue;
        }
        const std::string error = param_error(param);
        if (!error.empty()) {
            errors.push_back(param.display_name + ": " + error);
        }
    }
    if (errors.empty()) {
        ImGui::TextColored(ImVec4(0.31f, 0.75f, 0.42f, 1.0f), "参数校验通过");
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "参数校验：%d 项错误",
                           static_cast<int>(errors.size()));
        for (const std::string& error : errors) {
            ImGui::BulletText("%s", error.c_str());
        }
    }

    ImGui::Separator();

    // ---- 参数控件（F2：搜索过滤 + 只读标记；条件隐藏的参数不显示）----
    static char param_filter[64] = {};
    ImGui::SetNextItemWidth(-72.0f);
    ImGui::InputTextWithHint("##param_filter", "搜索参数（id / 名称 / 说明）", param_filter,
                             sizeof(param_filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("清除##param_filter")) {
        param_filter[0] = '\0';
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("清空搜索框（重新显示全部参数）");
    }
    const std::string filter_text = lower_copy(param_filter);
    const auto        matches_filter = [&filter_text](const Param& param) {
        if (filter_text.empty()) {
            return true;
        }
        return lower_copy(param.id).find(filter_text) != std::string::npos ||
               lower_copy(param.display_name).find(filter_text) != std::string::npos ||
               lower_copy(param.description).find(filter_text) != std::string::npos;
    };

    // ---- M_patchB L1 续（PB2-17）+ L2 修订（PB2-20）：ProviderConfig 自己的生效条目 ----
    //  * 按**该节点自身参数**解析（此前误用 resolve_effective_provider()：它对 ProviderConfig
    //    恒返回默认构造 → 站点区/提示永远回落内置默认，见 M_patchB §9.1）
    //  * **撤回**「web 条目自动锁 mode=web」（决策 D-21）：只提示、不静默改写
    const bool                is_provider_node = node->type == "ProviderConfig";
    engine::EffectiveProvider provider_effective;
    if (is_provider_node) {
        provider_effective = engine::resolve_display_provider(editor().graph, *node);
    }

    if (node->params.empty()) {
        ImGui::TextDisabled("该节点没有参数");
    }
    int hidden_params   = 0;
    int filtered_params = 0;
    int matched_params  = 0;
    for (Param& param : node->params) {
        if (!engine::param_visible(*node, param)) {
            ++hidden_params;
            continue;
        }
        if (!matches_filter(param)) {
            ++filtered_params;
            continue;
        }
        ++matched_params;
        ImGui::PushID(param.id.c_str());
        ImGui::TextUnformatted(param_label(param).c_str());
        if (param.is_secret) {
            ImGui::SameLine();
            ImGui::TextDisabled("（密钥：仅存内存、不写盘）");
        }
        if (param.value != param.default_value) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "已改");
        }
        help_marker(param.description);
        ImGui::PopID();

        // M_patchB L2 修订（PB2-20 / 决策 D-21）：ProviderConfig 的「模式」**恒为 {official, web}**
        //  * **不再**按条目 kind 裁剪候选 / 覆盖下拉项（不变量 I13：两项始终可选）
        //  * 与条目类型不一致时只给**橙色提示**（mode_kind_hint），绝不静默改写用户选择
        bool param_begin_edit  = false;
        bool param_changed_now = false;
        const bool param_edit_ended =
            draw_param_widget(*node, param, param_begin_edit, param_changed_now);
        // 只在「模式 = official」时提示（web 模式下的站点问题由站点区的**错误块**给出，避免重复；PB2-22）
        if (is_provider_node && param.id == "mode" && param.text() != "web") {
            const std::string hint = engine::mode_kind_hint(provider_effective);
            if (!hint.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f), "  ⚠ %s", hint.c_str());
            }
        }
        // 「切换提供商」→ 带出一次「模式」建议值（web 条目建议 web / official 条目建议 official）
        //  * 只带出一次建议：之后用户可任意改选，**不再**被改写（D-21）
        if (is_provider_node && param.id == "provider" && (param_changed_now || param_edit_ended)) {
            if (engine::Param* mode_param = node->findParam("mode")) {
                const std::string suggestion = engine::provider_mode_suggestion(*node);
                if (!suggestion.empty() && mode_param->text() != suggestion) {
                    result.begin_edit = true; // 外层据此压一次撤销快照（可撤销）
                    mode_param->value = suggestion;
                    result.changed    = true;
                    log::info("参数变更: " + node->id + ".mode → " + suggestion +
                              "（切换提供商带出的建议值，可再改回）");
                }
            }
        }
        if (param_changed_now || param_edit_ended) {
            result.changed = true; // 值已写回模型（编辑过程中即时生效）
        }
        if (param_edit_ended) {
            log::info("参数变更: " + node->id + "." + param.id); // 每段编辑只记一次
        }
        if (param_begin_edit) {
            result.begin_edit = true;
        }

        const std::string error = param_error(param);
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  %s", error.c_str());
        }
        ImGui::Spacing();
    }

    if (hidden_params > 0) {
        ImGui::TextDisabled("（%d 个 %s 专属参数已按当前模式隐藏）", hidden_params,
                            node->type == "ProviderConfig" ? "官方 API" : "条件");
    }
    if (!filter_text.empty()) {
        ImGui::TextDisabled("搜索「%s」：命中 %d 项，已过滤 %d 项（共 %d 项）", param_filter,
                            matched_params, filtered_params, matched_params + filtered_params);
    }

    // ---- 网页版（web 模式）：会话状态 + 登录入口（PB2-17/18/19）----
    if (node->type == "ProviderConfig") {
        if (is_web_mode(*node)) {
            if (draw_web_session_section(*node, editor().graph)) {
                result.begin_edit = true; // 先压快照（可撤销）
                result.changed    = true;
            }
        }
        else {
            ImGui::Spacing();
            if (provider_effective.is_web()) {
                ImGui::TextDisabled("该提供商是网页版条目（建议「模式」= web）：把「模式」切到 web "
                                    "即可在此处打开该站点的登录窗口。");
            }
            else {
                ImGui::TextDisabled("提示：把「模式」切到 web 需要**网页版站点条目**"
                                    "（如 deepseek-web，或自建站点条目 —— 见使用说明 §9）；"
                                    "非网页版条目**不会**回落到内置默认站点（决策 D-22②）。");
            }
        }
    }
    } // P7a-15：「参数」分组结束

    // M5-01 收尾：图片输入的尺寸摘要（只读文件头，不需要 GL 上下文）
    // P7a-04/07：先解析资源令牌 → 真实路径；缺失时给可操作告警（不再只说「读不到文件头」）
    if (node->type == "ImageInput") {
        const engine::Param*           path_param = node->findParam("path");
        const std::vector<std::string> entries =
            path_param != nullptr ? paths::split_path_list(path_param->text())
                                  : std::vector<std::string>();
        if (!entries.empty()) {
            bool              missing = false;
            std::string       resolve_error;
            const std::string image_path = asset::to_local_path(entries.front(), &missing, &resolve_error);
            if (missing || image_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                                   resolve_error.empty() ? "图片资源缺失（可能已被删除）"
                                                         : resolve_error.c_str());
                ImGui::PopStyleColor();
            }
            else {
                int         width  = 0;
                int         height = 0;
                std::string error;
                if (image_size(image_path, &width, &height, &error)) {
                    if (entries.size() > 1) {
                        ImGui::TextDisabled("图片尺寸：%d×%d（共 %d 张，此处显示第 1 张）", width,
                                            height, static_cast<int>(entries.size()));
                    }
                    else {
                        ImGui::TextDisabled("图片尺寸：%d×%d", width, height);
                    }
                }
                else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                    ImGui::TextWrapped("%s", error.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    }

    // ---- 运行状态（PA-03）：只读展示 + 复制（与输出面板 / 画布节点摘要同源）----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("运行状态", ImGuiTreeNodeFlags_DefaultOpen)) {
        const engine::RunSnapshot& snapshot = editor().run_snapshot_view();
        const engine::RunNodeView* run      = snapshot.find(node->id);

        if (run == nullptr) {
            ImGui::TextDisabled("尚未运行（点工具栏「▶ 运行」；首次网页版会话约数秒）");
        }
        else {
            ImGui::TextDisabled("状态: %s    耗时: %.2f ms", engine::nodeStateName(run->state),
                                run->duration_ms);
            if (!run->error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("错误：%s", run->error.c_str());
                ImGui::PopStyleColor();
            }

            // M5-03：图片结果预览（与输出面板共用纹理缓存）
            if (!run->images.empty()) {
                for (std::size_t index = 0; index < run->images.size(); ++index) {
                    ImGui::PushID(static_cast<int>(3000 + index));
                    const TextureInfo texture = texture_for(run->images[index]);
                    if (texture.texture != 0) {
                        const float limit  = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
                        float       width  = static_cast<float>(texture.width);
                        float       height = static_cast<float>(texture.height);
                        if (width > limit && width > 0.0f) {
                            const float shrink = limit / width;
                            width *= shrink;
                            height *= shrink;
                        }
                        ImGui::Image(
                            reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture.texture)),
                            ImVec2(width, height));
                        ImGui::TextDisabled("预览 %d×%d", texture.width, texture.height);
                    }
                    else {
                        ImGui::TextDisabled("图片不可用：%s", texture.error.c_str());
                    }
                    ImGui::PopID();
                }
            }

            const std::string body = node_output_text(editor().graph, snapshot, node->id);
            if (!body.empty()) {
                draw_readonly_text("##prop_run_result", body, 8.0f);
                if (ImGui::Button("复制运行结果")) {
                    copy_text(body, "[参数面板] " + node->id);
                }
            }
            if (node->type == "TextOutput") { // M_textio P3：最终输出
                std::string label = "输出";
                if (const engine::Param* label_param = node->findParam("label")) {
                    const std::string text = label_param->text();
                    if (!text.empty()) {
                        label = text;
                    }
                }
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "【最终输出】%s", label.c_str());
                ImGui::BeginDisabled(body.empty());
                if (ImGui::Button("导出为文档…")) {
                    export_node_document(editor(), node->id);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("导出为 .md/.txt（与预览同源，含元信息头）");
                }
            }
            else if (run->error.empty() && run->images.empty()) {
                ImGui::TextDisabled("（该节点无输出）");
            }

            if (!editor().last_archive_dir.empty()) {
                ImGui::TextDisabled("已归档：%s", editor().last_archive_dir.c_str());
                if (ImGui::SmallButton("复制归档路径")) {
                    copy_text(editor().last_archive_dir, "[参数面板] 归档路径");
                }
            }
        }
    }

    // ---- M_rerun：「重新生成（新 seed）」—— 等效“再次运行”，但使用新的随机种子 ----
    if (node->type == "LLMGenerate") {
        ImGui::Separator();
        if (const engine::Param* seed_param = node->findParam("seed")) {
            ImGui::TextDisabled("随机种子：%d（0 = 不指定；「重新生成」会写入新值）",
                                static_cast<int>(seed_param->number(0.0)));
        }
        const bool rerunning = editor().executor.running() || editor().session_active();
        ImGui::BeginDisabled(rerunning);
        if (ImGui::Button("重新生成（新 seed）", ImVec2(-FLT_MIN, 0.0f))) {
            if (engine::Param* seed_param = node->findParam("seed")) {
                editor().snapshot("重新生成（新 seed）"); // 先压快照：可撤销
                const long long stamp = static_cast<long long>(std::time(nullptr));
                const int       value = static_cast<int>(stamp % 1000000000LL); // ∈ 参数范围 [0, 1e9]
                seed_param->value     = (value > 0) ? value : 1;
                const int applied     = static_cast<int>(seed_param->number(0.0));
                log::info("[重跑] " + node->id + " 新 seed=" + std::to_string(applied));
                editor().set_status("已为 " + node->id + " 设置新 seed=" + std::to_string(applied) +
                                    "，开始重新生成…");
                editor().start_run_async();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("等效于「再次运行」，但使用新的随机种子：\n"
                              "· 官方 API：请求体带 seed\n"
                              "· 网页版：协议无 seed 槽位（忽略，但重跑结果也会不同）\n"
                              "· 会先记录一步撤销，可回退到旧 seed");
        }
    }

    // ---- 底部操作（F2：批量应用 / 重置 均需二次确认）----
    static std::string confirm_apply_node; // 待确认"应用到同类节点"的节点 id
    static std::string confirm_reset_node; // 待确认"重置为默认值"的节点 id

    ImGui::Separator();
    int same_type_others = 0;
    for (const engine::Node& other : editor().graph.nodes) {
        if (other.id != node->id && other.type == node->type) {
            ++same_type_others;
        }
    }
    ImGui::TextDisabled("同类型（%s）其他节点：%d 个", node->type.c_str(), same_type_others);

    if (confirm_apply_node == node->id) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "确认把 %s 的参数应用到 %d 个同类节点？（密钥类参数不复制）",
                           node->id.c_str(), same_type_others);
        if (ImGui::Button("确认应用", ImVec2(120.0f, 0.0f))) {
            editor().snapshot("批量应用参数到同类节点"); // 先压快照：整批应用可一次撤销
            std::string error;
            const int   affected = editor().graph.copyParamsToSameType(node->id, &error);
            if (affected > 0) {
                result.changed = true;
                editor().set_status("已把 " + node->id + " 的参数应用到 " +
                                    std::to_string(affected) + " 个同类节点（可撤销）");
                log::info("[参数面板] 批量应用参数: " + node->id + " → " +
                          std::to_string(affected) + " 个同类节点");
            }
            else {
                editor().set_status("批量应用未生效：" +
                                    (error.empty() ? std::string("没有同类节点") : error));
            }
            confirm_apply_node.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##apply", ImVec2(80.0f, 0.0f))) {
            confirm_apply_node.clear();
        }
    }
    else {
        ImGui::BeginDisabled(same_type_others == 0);
        if (ImGui::Button("把当前参数应用到全部同类节点", ImVec2(-FLT_MIN, 0.0f))) {
            confirm_apply_node = node->id; // 二次确认（批量改动多节点）
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("按参数 id 复制到同类型节点（不改动标题 / 位置 / 连线）；\n"
                              "密钥类参数（API Key）不会被复制");
        }
    }

    ImGui::Separator();
    if (confirm_reset_node == node->id) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "确认把 %s 的全部参数重置为默认值？",
                           node->id.c_str());
        if (ImGui::Button("确认重置", ImVec2(120.0f, 0.0f))) {
            editor().snapshot("重置参数为默认值"); // 先压快照：可撤销（D-M3-5）
            for (Param& param : node->params) {
                param.reset_to_default();
            }
            result.changed = true;
            log::info("参数重置为默认值: " + node->id);
            editor().set_status("已把 " + node->id + " 的参数重置为默认值（可撤销）");
            confirm_reset_node.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##reset", ImVec2(80.0f, 0.0f))) {
            confirm_reset_node.clear();
        }
    }
    else {
        if (ImGui::Button("重置为默认值", ImVec2(-FLT_MIN, 0.0f))) {
            confirm_reset_node = node->id; // 二次确认（避免误触清空编辑）
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("把该节点全部参数恢复为定义中的默认值（需再确认一次；\n"
                              "执行前会自动记录一步撤销，可回退）");
        }
    }

    ImGui::End();
}

// **M7B step 11**：等新通道（Pydoll）的后台会话任务结束（进程退出前必须调 —— 见头文件）
//  * **M7B step 12（`M7B-44`）**：任务按 `node.id` 记账 ⇒ 这里**遍历所有节点**逐个 join；
//    并**先**请求取消正在进行的登录态轮询（否则退出时要把 120 s 上限干等完 —— 实测旧版
//    关窗后还多等了 3 分钟）
void wait_web_tasks()
{
    // ① 请停止：正在轮询登录态的 `login_site` / `pydoll_login` 应尽早结束（幂等）
    web::channel::request_cancel_session_ops();

    // ② 先说一声（否则用户只看到界面卡一下，无从解释）—— 顺手快照，**不持锁 join**
    std::vector<std::string> node_ids;
    {
        std::lock_guard<std::mutex> lock(g_web_tasks_mutex);
        for (auto& entry : web_tasks()) {
            WebTask&                    task = entry.second;
            std::lock_guard<std::mutex> task_lock(task.mutex);
            node_ids.push_back(entry.first);
            if (task.running) {
                log::info("[网页版会话] 退出前等待后台任务结束：节点 " + entry.first + " · " +
                          task.title + "（" + task.status + "）");
            }
        }
    }

    // ③ 逐个 join（**不持 `g_web_tasks_mutex`**：任务体收尾会调 `web_task_done()` 再取它）
    for (const std::string& node_id : node_ids) {
        web_task(node_id).join_worker();
    }
}

} // namespace aiwrite::ui
