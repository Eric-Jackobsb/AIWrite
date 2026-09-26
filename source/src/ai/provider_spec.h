#pragma once

// ============================================================================
//  Provider 配置表（M_patchB L1 / PB2-01）
//
//  数据来源（唯一权威 = 随程序发布的 assets/providers.json）：
//    ① <exe>/assets/providers.json                 随程序发布的内置表
//    ② $AIWRITE_SOURCE_DIR/assets/providers.json   开发态兜底
//    ③ ~/.brain-ai/providers.d/*.json              用户新增条目（文件名升序）
//    ④ ~/.brain-ai/providers.json                  用户字段级覆盖（可 replace_all）
//    ⑤ C++ 最小兜底（仅 ①〜② 都缺失时：custom-official + deepseek）
//
//  设计约束（见 docs/actionPlan/M_patchB.md §0.4 / 附录 B·C）：
//    * 厂商元数据与网页版站点元数据**全部来自表**，C++ 内不出现具体厂商常量
//      （唯一例外：本实现里的最小兜底表，2 条）
//    * 加载 / 合并 / 校验**全部是纯函数**，无网络、无 Key 即可断言
//    * 表里**禁止**出现明文密钥（键名/值双重检测 → 警告 + 拒绝该字段）
//    * 表坏 / 缺 → 明确报错 + 上一份可用表或最小兜底，绝不崩溃
//    * `kind=official` 与 `kind=web` **同一套加载/合并/校验/覆盖**（web 同机制）
// ============================================================================

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aiwrite::ai {

// ---------------------------------------------------------------- 条目子结构 --

struct ProviderModelSpec {          // 候选模型（vision 仅对视觉能力有意义）
    std::string id;
    std::string label;
    bool        vision = false;
};

struct ProviderCaps {               // 能力表（驱动节点门控与提示）
    bool vision      = false;
    bool seed        = true;
    bool system_role = true;
    bool stream      = false;       // 本版本流式暂停（PB-03）
};

struct ProviderLimits {             // 限额（取代写死的 15s / 180s / 8MB）
    std::size_t image_max_bytes   = 8u * 1024u * 1024u;
    int         connect_timeout_s = 15;
    int         read_timeout_s    = 180;
};

// 网页版站点端点（内置适配器用；默认值 = 现有 DeepSeek 网页版行为，逐字一致）
struct ProviderWebEndpoints {
    std::string host                = "https://chat.deepseek.com";
    std::string completion_path     = "/api/v0/chat/completion";
    std::string challenge_path      = "/api/v0/chat/create_pow_challenge";
    std::string session_create_path = "/api/v0/chat_session/create";
    std::string session_fetch_path  = "/api/v0/chat_session/fetch_page";
    std::string users_path          = "/api/v0/users/current";
};

struct ProviderWebSpec {            // 网页版站点描述（kind=web）
    std::string adapter;            // builtin:deepseek（内置）| dom（通用 DOM）
    std::string login_url;
    std::string window_title;
    ProviderWebEndpoints     endpoints;      // builtin 用
    std::vector<std::string> probe_paths;    // 协议探测路径（空 = 用默认两条）
    std::vector<std::string> cookie_names;   // 需要的 Cookie（最小必要）
    std::string              token_expr;     // 页面内取 token 表达式
    // dom 专用
    std::string input_selector;
    std::string send_kind;                   // key | click
    std::string send_value;                  // key 名 或 选择器
    std::string answer_selector;
    std::string done_kind;                   // selector_gone | selector_present
    std::string done_selector;
    int         answer_poll_ms   = 500;
    int         answer_max_polls = 120;
};

// ---- M_patchB L4（PB2-27 / 决策 D-27 / 不变量 I15）：**站点无关**的「已登录」判定 ----
//  * 判据取**并集**（`D-27`）：① 条目 `cookie_names` 命中 ② 该 origin 的 Cookie 非空
//  * **不看** `userToken` / `ds_session_id` / 任何 `/api/v0/*` 端点是否可用（`I15`）
//  * 证据由调用方提供（见 `web::web_session_evidence(session)`）→ 本层**不依赖** `web/**`，
//    因此可在 `api_probe --exec-selftest` 里做纯函数断言（`VB2-24`）
enum class WebSessionState {
    unknown,    // 尚无证据（该站点本轮还没读过 Cookie：重启后 / 从未开过窗口）
    logged_in,  // 有站点无关的登录证据
    logged_out, // 已读过该站点 Cookie，但没有登录证据（未登录 / 会话已失效）
};

struct WebSessionEvidence {
    bool                     cookies_known = false; // 是否已读过该站点的 Cookie 快照
    std::size_t              cookie_count  = 0;     // 该 origin 的 Cookie 条数
    std::vector<std::string> cookie_names;          // 该 origin 实际存在的 Cookie 名
};

struct WebSessionVerdict {
    WebSessionState state  = WebSessionState::unknown;
    std::string     reason; // 站点无关的原因（可直接上界面 / 日志）
};

// ------------------------------------------------------------------ 条目本体 --

struct ProviderSpec {
    std::string id;        // 唯一键（provider 参数取值；用户不可改）
    std::string display;   // 下拉显示名
    std::string kind;      // official | web
    std::string protocol;  // openai | anthropic | gemini | dom | deepseek-web
    // ---- official ----
    std::string api_base;
    std::string chat_path;
    std::string auth_style;   // bearer | api-key | x-api-key | query | none
    std::string auth_header;  // 空 = 按 auth_style 默认
    std::vector<std::pair<std::string, std::string>> extra_headers;
    std::vector<std::string> env_names;       // 按序尝试
    std::string key_ref_default;              // 空 = brain-ai/<id>
    std::vector<ProviderModelSpec> models;    // 空 = 模型自由填写
    std::string vision_model_default;
    ProviderCaps   caps;
    ProviderLimits limits;
    // ---- web ----
    ProviderWebSpec web;
    // ---- 元信息 ----
    bool        verified = false;
    std::string notes;
    std::string docs_url;
    std::string origin;   // builtin | source | user.d/<file> | user | fallback

    const ProviderModelSpec* find_model(const std::string& model_id) const;
};

// 网页版内置适配器（builtin:deepseek）的默认端点 —— 与改造前常量逐字一致
const ProviderWebEndpoints& default_deepseek_web_endpoints();

// ------------------------------------------------------------------ 加载报告 --

struct SpecLoadReport {
    bool                     ok            = true;   // 无 error（warnings 不影响可用性）
    bool                     fallback_used = false;  // 是否用了最小兜底表
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> files;                  // 实际参与的层（含标签）

    std::string summary() const;                     // 一行摘要（Console / 自检用）
    std::string error_of(const std::string& id) const;  // 未使用（保留扩展位）
};

struct ProviderSpecs {
    std::vector<ProviderSpec> items;
    SpecLoadReport            report;

    const ProviderSpec*      find(const std::string& id) const;
    std::vector<std::string> ids(const std::string& kind /*空 = 全部*/) const;
    std::size_t              count_kind(const std::string& kind) const;
};

// ---- 纯函数（离线断言用；不碰文件系统）----
// layers：{层标签, JSON 文本}，**低 → 高优先级**
ProviderSpecs merge_provider_specs(
    const std::vector<std::pair<std::string, std::string>>& layers, SpecLoadReport* report);

// ---- 真实读盘：按上文 ①→⑤ 顺序组层后调用 merge_provider_specs ----
ProviderSpecs load_provider_specs();

// 进程内缓存（首次访问自动加载；UI「重新加载配置表」/ CLI 自检调用 reload）
//  * provider_specs()          —— 取当前表（引用；跨 reload 可能失效，长生命周期请用快照）
//  * provider_specs_snapshot() —— 取共享快照（工作线程/节点执行请用这个，避免 reload 竞态）
const ProviderSpecs&                 provider_specs();
std::shared_ptr<const ProviderSpecs> provider_specs_snapshot();
void                                 reload_provider_specs();

// ---- M_patchB L1 续（PB2-17）：节点生效条目 → 网页版站点参数 ----
//  * 生效条目是 web 条目 → 用它自己的 `web` 段
//  * 否则（official 条目 / 表里没有该 id / nullptr）→ 回落**表内第一个 web 条目**
//    （内置表即 `deepseek-web`），保持改造前行为「未选网页版条目时用内置默认站点」；
//    字段为空时由调用方按内置默认回落（见 `web::login_request_of`）
//  * `table` 可为快照（工作线程请传 `provider_specs_snapshot().get()`，避免 reload 竞态）
ProviderWebSpec web_spec_for(const ProviderSpec* spec, const ProviderSpecs* table);
std::string     web_provider_id_for(const ProviderSpec* spec, const ProviderSpecs* table);
inline ProviderWebSpec web_spec_for(const ProviderSpec* spec)
{
    return web_spec_for(spec, &provider_specs());
}
inline std::string web_provider_id_for(const ProviderSpec* spec)
{
    return web_provider_id_for(spec, &provider_specs());
}

// ---- M_patchB L1 修订（PB2-22 / 决策 D-22② / 不变量 I14）：**严格**站点解析（不回落）----
//  * 站点只能来自 `kind == "web"` 条目自己的 `web` 段；非网页版条目 / 表外 id / nullptr → **空**
//  * 与上方 `web_spec_for()`（保留旧回落语义，**仅供 CLI / 自检**守 `I2`）严格区分：
//    界面 / 运行前校验 / 运行期**必须**用本组
ProviderWebSpec strict_web_spec_for(const ProviderSpec* spec);
std::string     strict_web_provider_id_for(const ProviderSpec* spec);

// 站点**不可用**的可操作错误文案（空串 = 可用）：界面（红）/ 运行前校验（「本次运行必定失败」）/ 运行期（NodeError）共用
//  * 条目为空 / 非网页版条目 / `web.login_url` 为空（决策 `D-26`：**不**回落内置默认站点）
std::string web_site_error(const ProviderSpec* spec);

// 站点**可选字段**缺失清单（可回落内置默认，但要点明；决策 `D-26`：回落 + 警告）——空串 = 无缺失
std::string web_site_field_warnings(const ProviderSpec* spec);

// 该 `web.adapter` 本版本是否已实现（R12：表里有、程序没实现 → 运行时**明确报错**；空 adapter = 视为内置默认）
bool web_adapter_implemented(const std::string& adapter);

// M_patchB L1 修订（PB2-26）：**登录型站点条目**（`adapter=dom` 且生成字段未就绪）
//  * 登录 / 协议探测可用；生成会在运行时**明确报错**（不猜选择器、不静默降级）
bool web_login_only(const ProviderSpec* spec);

// ---- M_patchB L4（PB2-27）：站点无关的登录态判定 / 显示条件 / 探测适用性（全部**纯函数**）----

// 「已登录 / 未登录 / 未确认」判定（判据见 `D-27`；**不看** `userToken`，不变量 `I15`）
//  * `spec` 为空 / 非网页版条目 → `unknown`（没有站点登录态这个概念）
//  * 配了 `cookie_names` → 命中任一即 `logged_in`；已读过 Cookie 但未命中 → `logged_out`
//  * 未配 `cookie_names` → 该 origin 的 Cookie 非空即 `logged_in`（并集，不强制配 Cookie 名）
WebSessionVerdict web_session_state(const ProviderSpec* spec, const WebSessionEvidence& evidence);

// 状态标签（界面 / 状态栏用）：已登录 / 未登录 / 未确认
std::string web_session_state_label(WebSessionState state);

// `D-28` ①：界面是否显示 `userToken` 行 —— **仅当**条目显式配了 `token_expr`
//  * 通用 DOM 站点不产生 `userToken` → 不显示该行（改为「登录态由浏览器 profile 维持」）
//  * 内置 `deepseek-web` 配了 `token_expr` → 行为不变（守 `I2`）
bool web_shows_user_token(const ProviderSpec* spec);

// ---- M_patchB L4（PB2-28 / 不变量 `I16`）：**协议探测**（PoW 挑战 / 站点端点 / `localStorage.userToken`）
//      对该条目是否**适用** ----
//  * 内置适配器（`adapter` 空 / `builtin:*`）→ **适用**（协议探测就是为它设计的）
//  * `dom` 站点 → 仅当**显式**配了 `probe_paths` 或 `token_expr` 才适用
//    （`web.endpoints` 对 `dom` 无意义，加载期已给警告）；否则**不适用** ——
//    页面内只做**只读诊断**，**不注入**任何 DeepSeek 端点 / `userToken` 读取
bool probe_is_applicable(const ProviderWebSpec& web);
bool probe_is_applicable(const ProviderSpec* spec);

// ---- 本版本「代码能力」清单（提示「表里有、程序还没实现」的条目，R9/R12）----
std::vector<std::string> implemented_protocols();      // 例：{"openai", "deepseek-web"}
std::vector<std::string> implemented_web_adapters();   // 例：{"builtin:deepseek"}
std::string              protocol_display(const std::string& protocol);

// ---- 解析辅助（纯函数；节点 / 提示 / 自检共用）----
std::string resolve_api_base(const ProviderSpec& spec, const std::string& param_value);
std::string resolve_key_ref(const ProviderSpec& spec, const std::string& param_value);
std::string resolve_env_name(const ProviderSpec& spec);   // 表里第一个 env 名（可空）
// model_custom 非空 → 覆盖；否则节点 model 若属于该条目 → 用它；否则用表内首选
// （for_vision=true 时优先 vision_model_default / 首个视觉模型）
std::string resolve_model(const ProviderSpec& spec, const std::string& param_model,
                          const std::string& model_custom, bool for_vision);
// 生效模型是否支持视觉：在表内且 vision=false → false；不在表内 → true（交给后端）
// *declared 表示「该模型是否在表内被声明过」
bool        spec_model_supports_vision(const ProviderSpec& spec, const std::string& model,
                                       bool* declared);
std::string default_chat_path(const std::string& protocol);
std::string default_auth_style(const std::string& protocol);
std::string default_auth_header(const std::string& auth_style);

// ---- 自检：配置表相关离线断言（返回失败项数；api_probe --exec-selftest 也调用）----
// 覆盖：合并优先级 / 必填缺失 / 类型错误 / 未知字段 / dup id 覆盖 / 密钥拒绝 /
//      坏 JSON / schema_version 不匹配 / 兜底触发 / web 两种形态校验 / _ 前缀键忽略
//   * passed_out：可选，回传通过项数（便于调用方并入自己的计数）
int provider_spec_selftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
