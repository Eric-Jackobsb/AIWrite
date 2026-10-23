// provider_spec_selftest.cpp -- Provider 配置表：离线断言
//   （供 aiwrite --provider-selftest 与 api_probe --exec-selftest 共用）
//
// ARCH-Z1（2026-10-23）：从 provider_spec.cpp 拆出（原 1374〜1739 行）。
//   覆盖：合并优先级 / 必填缺失 / 类型错误 / 未知字段 / dup id 覆盖 / 密钥拒绝 /
//         坏 JSON / schema_version 不匹配 / 兜底触发 / web 两种形态校验 / _ 前缀键忽略 /
//         resolve_* 与视觉门控。全部**合成夹具**（不读磁盘上的 providers.json）。
//   计数口径：CheckCounter（passed / failed），返回失败项数。

#include "provider_spec.h"

#include <cstdio>
#include <string>
#include <vector>

namespace aiwrite::ai {
// ============================================================================
//  自检：配置表离线断言（PB2-01；供 aiwrite --provider-selftest /
//        api_probe --exec-selftest 共用；返回失败项数）
// ============================================================================
namespace {

struct CheckCounter {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name, const std::string& detail = {})
    {
        if (condition) {
            ++passed;
            std::printf("   PASS  %s\n", name.c_str());
            return;
        }
        ++failed;
        std::printf("   FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ->  ",
                    detail.c_str());
    }
};

bool has_text(const std::vector<std::string>& items, const std::string& needle)
{
    for (const std::string& item : items) {
        if (item.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 合成的基础层（纯离线夹具；不依赖磁盘上的 providers.json）
std::string base_layer_json()
{
    return R"JSON({
  "schema_version": 1,
  "providers": [
    {
      "id": "alpha", "display": "Alpha（官方 API）", "kind": "official", "protocol": "openai",
      "api_base": "https://alpha.example.com/v1", "env_names": ["ALPHA_API_KEY"],
      "key_ref_default": "brain-ai/alpha",
      "capabilities": { "vision": true, "seed": true, "system_role": true },
      "models": [ {"id": "alpha-text"}, {"id": "alpha-vision", "vision": true} ],
      "vision_model_default": "alpha-vision",
      "limits": { "image_max_bytes": 1048576, "connect_timeout_s": 7, "read_timeout_s": 42 }
    },
    {
      "id": "alpha-web", "display": "Alpha（网页版）", "kind": "web", "protocol": "builtin-web",
      "web": {
        "adapter": "builtin:deepseek",
        "login_url": "https://alpha.example.com/chat",
        "window_title": "Alpha 登录",
        "endpoints": { "host": "https://alpha.example.com",
                       "completion_path": "/api/v0/chat/completion",
                       "challenge_path": "/api/v0/chat/create_pow_challenge",
                       "session_create_path": "/api/v0/chat_session/create",
                       "session_fetch_path": "/api/v0/chat_session/fetch_page",
                       "users_path": "/api/v0/users/current" }
      },
      "models": [ {"id": "default"} ]
    }
  ]
})JSON";
}

} // namespace

int providerSpecSelftest(int* passed_out)
{
    CheckCounter counter;
    std::printf("[配置表自检] Provider 规格表（M_patchB L1 / PB2-01）\n");

    // ---------- 1) 基础层解析 ----------
    {
        SpecLoadReport      report;
        const ProviderSpecs specs =
            merge_provider_specs({{"builtin", base_layer_json()}}, &report);
        counter.check(specs.items.size() == 2, "基础层解析：2 条",
                      "实际 " + std::to_string(specs.items.size()));
        counter.check(specs.ids("") == std::vector<std::string>({"alpha", "alpha-web"}),
                      "条目顺序与 id 列表");
        const ProviderSpec* alpha = specs.find("alpha");
        counter.check(alpha != nullptr && alpha->origin == "builtin", "origin = builtin");
        counter.check(alpha != nullptr && alpha->chat_path == "/chat/completions",
                      "chat_path 默认补全（openai）");
        counter.check(alpha != nullptr && alpha->auth_style == "bearer" &&
                          alpha->auth_header == "Authorization",
                      "auth_style / auth_header 默认补全");
        counter.check(alpha != nullptr && alpha->limits.read_timeout_s == 42,
                      "limits 读入（read_timeout_s=42）");
        counter.check(alpha != nullptr && alpha->models.size() == 2 &&
                          alpha->findModel("alpha-vision") != nullptr &&
                          alpha->findModel("alpha-vision")->vision,
                      "models 解析（含逐模型 vision）");
        counter.check(report.errors.empty(), "基础层无 error");
    }

    // ---------- 2) 用户层：字段级覆盖 ----------
    {
        const std::string user = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "alpha", "api_base": "https://mirror.example.com/v1" } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs =
            merge_provider_specs({{"builtin", base_layer_json()}, {"user", user}}, &report);
        const ProviderSpec* alpha = specs.find("alpha");
        counter.check(alpha != nullptr && alpha->api_base == "https://mirror.example.com/v1",
                      "用户层覆盖 api_base 生效");
        counter.check(alpha != nullptr && alpha->display == "Alpha（官方 API）",
                      "未写字段保持下层值（display 不变）");
        counter.check(alpha != nullptr && alpha->origin == "user", "origin 变为 user");
        counter.check(specs.find("alpha-web") != nullptr, "其余条目不受影响");
    }

    // ---------- 3) 用户层：新增条目 ----------
    {
        const std::string user = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "my-ai", "display": "我的自建服务", "kind": "official",
                   "protocol": "openai", "api_base": "http://192.168.1.10:8000/v1",
                   "env_names": ["MY_AI_KEY"] } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs(
            {{"builtin", base_layer_json()}, {"user.d/10-my.json", user}}, &report);
        const ProviderSpec* added = specs.find("my-ai");
        counter.check(added != nullptr && added->key_ref_default == "brain-ai/my-ai",
                      "新增条目：key_ref_default 自动补全");
        counter.check(added != nullptr && added->origin == "user.d/10-my.json",
                      "新增条目 origin 记录文件名");
        counter.check(specs.items.size() == 3, "新增后共 3 条");
    }


    // ---------- 4) 必填缺失 / 类型错误 / 未知字段 / 明文密钥 ----------
    {
        const std::string broken = R"JSON({
  "schema_version": 1,
  "providers": [
    { "id": "no-kind", "display": "缺 kind", "protocol": "openai" },
    { "id": "bad-models", "display": "models 类型不符", "kind": "official",
      "protocol": "openai", "models": "not-an-array" },
    { "id": "unknown-field", "display": "未知字段", "kind": "official",
      "protocol": "openai", "api_base": "https://x.example.com", "whatever": 1 },
    { "id": "with-secret", "display": "含密钥", "kind": "official", "protocol": "openai",
      "api_base": "https://y.example.com", "api_key": "sk-0123456789abcdef" }
  ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", broken}}, &report);
        counter.check(report.errors.size() >= 2, "必填缺失 + 类型错误各记一次 error",
                      "errors=" + std::to_string(report.errors.size()));
        counter.check(specs.find("no-kind") == nullptr, "缺少 kind 的条目被跳过");
        counter.check(specs.find("bad-models") == nullptr, "models 类型不符的条目被跳过");
        counter.check(specs.find("unknown-field") != nullptr &&
                          has_text(report.warnings, "未知字段"),
                      "未知字段 → 警告但条目保留");
        counter.check(specs.find("with-secret") != nullptr &&
                          has_text(report.warnings, "密钥"),
                      "明文密钥 → 警告（字段被拒绝）");
    }

    // ---------- 5) 坏 JSON / schema_version / 层忽略但其他层生效 ----------
    {
        const std::string bad_json = "{ \"schema_version\": 1, \"providers\": [ }";
        SpecLoadReport    report;
        const ProviderSpecs specs = merge_provider_specs(
            {{"builtin", base_layer_json()}, {"user.d/broken.json", bad_json}}, &report);
        counter.check(has_text(report.errors, "JSON 解析失败"), "坏 JSON → error 记录");
        counter.check(specs.items.size() == 2, "坏层不影响有效层（仍 2 条）");

        const std::string wrong_version = R"JSON({ "schema_version": 2, "providers": [] })JSON";
        SpecLoadReport    report2;
        merge_provider_specs({{"user", wrong_version}}, &report2);
        counter.check(has_text(report2.errors, "schema_version"),
                      "schema_version 不匹配 → error");

        const std::string no_providers = R"JSON({ "schema_version": 1 })JSON";
        SpecLoadReport    report3;
        merge_provider_specs({{"user", no_providers}}, &report3);
        counter.check(has_text(report3.errors, "providers"), "缺少 providers 数组 → error");
    }

    // ---------- 6) 空表 → 最小兜底；replace_all ----------
    {
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({}, &report);
        counter.check(report.fallback_used && specs.items.size() == 2,
                      "空表 → 最小兜底 2 条");
        counter.check(specs.find("deepseek") != nullptr &&
                          specs.find("custom-official") != nullptr,
                      "兜底表含 deepseek 与 custom-official");
        counter.check(specs.find("deepseek") != nullptr &&
                          specs.find("deepseek")->api_base == "https://api.deepseek.com",
                      "兜底 deepseek 地址与改造前一致");

        const std::string replace_all = R"JSON({
  "schema_version": 1, "replace_all": true,
  "providers": [ { "id": "only-one", "display": "唯一", "kind": "official",
                   "protocol": "openai", "api_base": "https://z.example.com" } ]
})JSON";
        SpecLoadReport      report2;
        const ProviderSpecs specs2 =
            merge_provider_specs({{"builtin", base_layer_json()}, {"user", replace_all}},
                                 &report2);
        counter.check(specs2.items.size() == 1 && specs2.find("only-one") != nullptr,
                      "replace_all=true：只保留该层条目");
        counter.check(has_text(report2.warnings, "replace_all"), "replace_all 有提示");
    }


    // ---------- 7) web 条目：两种形态的校验与提示 ----------
    {
        const std::string web_layer = R"JSON({
  "schema_version": 1,
  "providers": [
    { "id": "dom-site", "display": "DOM 站点", "kind": "web", "protocol": "dom",
      "web": { "adapter": "dom", "login_url": "https://site.example.com/" } },
    { "id": "partial-endpoints", "display": "缺端点", "kind": "web", "protocol": "builtin-web",
      "web": { "adapter": "builtin:deepseek", "login_url": "https://s2.example.com/",
               "endpoints": { "host": "https://s2.example.com" } } },
    { "id": "builtin-with-selector", "display": "多余字段", "kind": "web",
      "protocol": "builtin-web",
      "web": { "adapter": "builtin:deepseek", "login_url": "https://s3.example.com/",
               "input_selector": "#prompt" } }
  ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", web_layer}}, &report);
        // M_patchB L1 修订（PB2-26）：dom 缺生成字段 → **登录型站点条目**（警告，**不**丢弃）
        const ProviderSpec* login_only = specs.find("dom-site");
        counter.check(login_only != nullptr && webLoginOnly(login_only) &&
                          !has_text(report.errors, "dom-site") &&
                          has_text(report.warnings, "登录型站点条目"),
                      "dom 缺生成字段 → 登录型站点条目（警告，不丢弃；生成未就绪）");
        const ProviderSpec* partial = specs.find("partial-endpoints");
        counter.check(partial != nullptr &&
                          partial->web.endpoints.host == "https://s2.example.com" &&
                          partial->web.endpoints.completion_path ==
                              defaultDeepseekWebEndpoints().completion_path,
                      "builtin 端点缺字段 → 沿用内置默认");
        counter.check(has_text(report.warnings, "web.endpoints"), "缺端点有警告");
        const ProviderSpec* extra = specs.find("builtin-with-selector");
        counter.check(extra != nullptr && !extra->web.probe_paths.empty(),
                      "builtin 站点：probe_paths 自动补默认两条");
        counter.check(has_text(report.warnings, "不支持"), "adapter 不支持字段 → 警告（R12）");
    }

    // ---------- 7.5) 嵌套 schema 名（`R18`）：文档/示例写**嵌套**，实现**按嵌套读** ----------
    //  * 风险原文（归档 §5 `R18`）：文档用**扁平**字段名（`send_kind`）、实现读**嵌套**
    //    （`send{kind,value}` / `done_when{kind,selector}`）⇒ 照抄也建不出站点。
    //    本用例把「两边一致」钉成断言：嵌套写法**逐字生效**，扁平写法**明确警告 + 跳过该条**。
    {
        const std::string nested_layer = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "nested-dom", "display": "嵌套 DOM", "kind": "web", "protocol": "dom",
      "web": { "adapter": "dom", "login_url": "https://nested.example.com/",
               "input_selector": "#chat-input",
               "send": { "kind": "key", "value": "Enter" },
               "answer_selector": ".markdown-body",
               "done_when": { "kind": "selector_gone", "selector": "button[aria-label*='停止']" } } } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", nested_layer}}, &report);
        const ProviderSpec* nested = specs.find("nested-dom");
        counter.check(nested != nullptr && !webLoginOnly(nested) &&
                          nested->web.send_kind == "key" && nested->web.send_value == "Enter" &&
                          nested->web.done_kind == "selector_gone" &&
                          nested->web.done_selector == "button[aria-label*='停止']" &&
                          nested->web.answer_selector == ".markdown-body",
                      "嵌套 schema（R18）：`send{kind,value}` / `done_when{kind,selector}` 逐字生效");

        const std::string flat_layer = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "flat-dom", "display": "扁平 DOM", "kind": "web", "protocol": "dom",
      "web": { "adapter": "dom", "login_url": "https://flat.example.com/",
               "input_selector": "#chat-input",
               "send_kind": "key", "send_value": "Enter",
               "done_kind": "selector_gone", "done_selector": "button" } } ]
})JSON";
        SpecLoadReport      flat_report;
        const ProviderSpecs flat_specs = merge_provider_specs({{"user", flat_layer}}, &flat_report);
        const ProviderSpec* flat       = flat_specs.find("flat-dom");
        // 现行口径（`PB2-26` 修订后）：缺 `send` 的 `dom` 条目**保留**但降级为
        // 「登录型站点条目」（生成未就绪）—— 即「照抄也建不出站点」，但**当场说清原因**
        counter.check(flat != nullptr && webLoginOnly(flat) &&
                          has_text(flat_report.warnings, "send_kind") &&
                          has_text(flat_report.warnings, "登录型站点条目"),
                      "扁平字段名（错误写法）：条目仍在，但降级为**登录型站点条目**"
                      "（未知字段警告 + 生成未就绪）");
    }

    // ---------- 8) 未实现协议（表里有、程序没有）→ 警告而非报错 ----------
    {
        // ⚠️ 这里的 protocol 必须**确实没实现**（`implemented_protocols()` 里没有）——
        //    2026-10-11（L2 底座）：原用例用 `anthropic`，该协议本批已实现 ⇒ 断言失去意义，
        //    改用仍未实现的 `cohere-v2`；表里写了、程序没有 ⇒ 仍是**警告**（条目保留，可选），
        //    运行期由 `provider_factory` **明确报错**（`R9`）
        const std::string future = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "future-api", "display": "未来协议", "kind": "official",
                   "protocol": "cohere-v2", "api_base": "https://api.cohere.ai" } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", future}}, &report);
        counter.check(specs.find("future-api") != nullptr, "未实现协议的条目仍保留（可显示）");
        counter.check(has_text(report.warnings, "尚未实现"), "未实现协议 → 警告（R9）");
    }

    // ---------- 9) 解析辅助（节点与提示共用） ----------
    {
        const ProviderSpecs fixture =
            merge_provider_specs({{"builtin", base_layer_json()}}, nullptr);
        const ProviderSpec* alpha = fixture.find("alpha");
        counter.check(alpha != nullptr, "解析辅助夹具就绪");
        if (alpha != nullptr) {
            counter.check(resolveApiBase(*alpha, "https://mine.example.com") ==
                              "https://mine.example.com",
                          "resolve_api_base：节点参数优先");
            counter.check(resolveApiBase(*alpha, "  ") == "https://alpha.example.com/v1",
                          "resolve_api_base：空 → 表默认");
            counter.check(resolveKeyRef(*alpha, "") == "brain-ai/alpha",
                          "resolve_key_ref：空 → 表内引用名");
            counter.check(resolveEnvName(*alpha) == "ALPHA_API_KEY",
                          "resolve_env_name：取表内首个");
            counter.check(resolve_model(*alpha, "", "my-model", false) == "my-model",
                          "resolve_model：自定义优先");
            counter.check(resolve_model(*alpha, "alpha-vision", "", false) == "alpha-vision",
                          "resolve_model：节点值在表内 → 用它");
            counter.check(resolve_model(*alpha, "deepseek-chat", "", false) == "alpha-text",
                          "resolve_model：节点值不在表内 → 表内首选");
            counter.check(resolve_model(*alpha, "", "", true) == "alpha-vision",
                          "resolve_model：视觉 → vision_model_default");
            bool declared = false;
            counter.check(spec_model_supports_vision(*alpha, "alpha-text", &declared) == false &&
                              declared,
                          "视觉门控：表内非视觉模型 → false");
            counter.check(spec_model_supports_vision(*alpha, "alpha-vision", &declared) == true &&
                              declared,
                          "视觉门控：表内视觉模型 → true");
            counter.check(spec_model_supports_vision(*alpha, "free-model", &declared) == true &&
                              !declared,
                          "视觉门控：表外模型 → 允许（declared=false）");
        }
    }

    // ---------- 10) 真实配置表可加载（磁盘缺失 → 兜底） ----------
    {
        const ProviderSpecs& specs = providerSpecs();
        counter.check(!specs.items.empty(), "provider_specs() 至少 1 条");
        counter.check(specs.find("deepseek") != nullptr, "真实表中存在 deepseek");
        const ProviderSpec* real = specs.find("deepseek");
        std::printf("   ----  真实表：来源=%s；%s；条目 %zu 条\n",
                    real != nullptr ? real->origin.c_str() : "?",
                    specs.report.summary().c_str(), specs.items.size());
    }

    std::printf("[配置表自检] %d 项通过 / %d 项失败\n", counter.passed, counter.failed);
    if (passed_out != nullptr) {
        *passed_out = counter.passed;
    }
    return counter.failed;
}

} // namespace aiwrite::ai
