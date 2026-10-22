#include "official_template.h"

#include "log.h"
#include "site_template.h" // providerIdError / writeUserProviderFile / splitNames / joinNames（复用）
#include "string_util.h"   // CLN-05：trim 收敛

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

using ordered_json = nlohmann::ordered_json;

std::string notes_of(const OfficialProviderTemplate& tpl)
{
    if (!tpl.notes.empty()) {
        return tpl.notes;
    }
    return "由 AIwrite 界面创建（API 地址 / 模型名请按账号实际可用值核对）";
}

// 自检用的**合成基础层**（纯离线夹具；不依赖磁盘上的 providers.json）
std::string selftest_base_layer_json()
{
    return R"JSON({
  "schema_version": 1,
  "providers": [
    {
      "id": "base-official", "display": "Base（官方 API）", "kind": "official",
      "protocol": "openai", "api_base": "https://base.example.com/v1",
      "models": [ { "id": "base-text" } ]
    }
  ]
})JSON";
}

// 自检计数助手（与 site_template_selftest 同风格：逐项打印，失败带原因）
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

} // namespace

// --------------------------------------------------------------------- 模板 --

OfficialProviderTemplate officialTemplateDefault()
{
    OfficialProviderTemplate tpl;
    tpl.id           = "my-provider";
    tpl.display      = "我的 API 提供商";
    tpl.protocol     = "openai";
    tpl.api_base     = "";
    tpl.chat_path    = "";
    tpl.auth_style   = "";
    tpl.models       = "";
    tpl.vision_model = "";
    tpl.vision       = false;
    tpl.seed         = true;
    tpl.system_role  = true;
    tpl.env_name     = "";
    tpl.key_ref      = "";
    tpl.notes        = "由 AIwrite 界面创建（API 地址 / 模型名请按账号实际可用值核对）";
    return tpl;
}
OfficialProviderTemplate officialTemplateFromSpec(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "official") {
        return officialTemplateDefault();
    }
    OfficialProviderTemplate tpl = officialTemplateDefault();
    tpl.id           = spec->id;
    tpl.display      = spec->display.empty() ? spec->id : spec->display;
    tpl.protocol     = spec->protocol;
    tpl.api_base     = spec->api_base;
    tpl.chat_path    = spec->chat_path;
    tpl.auth_style   = spec->auth_style;
    tpl.vision       = spec->caps.vision;
    tpl.seed         = spec->caps.seed;
    tpl.system_role  = spec->caps.system_role;
    tpl.vision_model = spec->vision_model_default;
    tpl.env_name     = joinNames(spec->env_names);
    tpl.key_ref      = spec->key_ref_default;
    std::vector<std::string> model_ids;
    for (const ProviderModelSpec& m : spec->models) {
        model_ids.push_back(m.id);
    }
    tpl.models = joinNames(model_ids);
    tpl.notes  = spec->notes;
    if (tpl.notes.empty()) {
        tpl.notes = "照抄自内置条目 " + spec->id + "（自建：请按需修改 API 地址 / 模型名）";
    }
    return tpl;
}

// --------------------------------------------------------------- 校验 / 落盘 --

std::string officialIdError(const std::string& id)
{
    return providerIdError(id, "提供商");
}

std::string officialTemplateError(const OfficialProviderTemplate& tpl)
{
    const std::string id_problem = officialIdError(tpl.id);
    if (!id_problem.empty()) {
        return id_problem;
    }
    if (tpl.protocol != "openai" && tpl.protocol != "anthropic" && tpl.protocol != "gemini") {
        return "协议 " + tpl.protocol +
               " 本版本未实现（官方 API 只支持 openai / anthropic / gemini）";
    }
    const std::string url = utils::trim(tpl.api_base);
    if (url.empty()) {
        return "API 地址不能为空（完整前缀，含 /v1 等；例 https://api.example.com/v1）";
    }
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        return "API 地址必须以 http:// 或 https:// 开头：" + url;
    }
    return {};
}

std::string officialFileJson(const OfficialProviderTemplate& tpl)
{
    ordered_json entry;
    entry["id"]       = tpl.id;
    entry["display"]  = tpl.display.empty() ? tpl.id : tpl.display;
    entry["kind"]     = "official";
    entry["protocol"] = tpl.protocol;
    entry["verified"] = false; // 照抄模板一律标「未验证」（不实测不填 verified）

    entry["api_base"] = utils::trim(tpl.api_base);
    // 只写**非空**的可选字段：留空 = 走解析期按 protocol 默认（provider_spec.cpp 已实现）
    const std::string chat_path = utils::trim(tpl.chat_path);
    if (!chat_path.empty()) {
        entry["chat_path"] = chat_path;
    }
    const std::string auth_style = utils::trim(tpl.auth_style);
    if (!auth_style.empty()) {
        entry["auth_style"] = auth_style;
    }
    const std::vector<std::string> envs = splitNames(tpl.env_name);
    if (!envs.empty()) {
        entry["env_names"] = envs;
    }
    const std::string key_ref = utils::trim(tpl.key_ref);
    if (!key_ref.empty()) {
        entry["key_ref_default"] = key_ref;
    }
    entry["capabilities"] =
        ordered_json{{"vision", tpl.vision}, {"seed", tpl.seed},
                     {"system_role", tpl.system_role}, {"stream", false}}; // 本版本流式暂停（PB-03）
    const std::vector<std::string> models = splitNames(tpl.models);
    if (!models.empty()) {
        ordered_json arr = ordered_json::array();
        for (const std::string& m : models) {
            arr.push_back(ordered_json{{"id", m}});
        }
        entry["models"] = arr;
    }
    const std::string vision_model = utils::trim(tpl.vision_model);
    if (!vision_model.empty()) {
        entry["vision_model_default"] = vision_model;
    }
    const std::string entry_notes = notes_of(tpl);
    if (!entry_notes.empty()) {
        entry["notes"] = entry_notes;
    }

    ordered_json root;
    root["schema_version"] = 1; // 信封（缺它 = 整层被忽略；与站点条目同一规则）
    root["_comment"] = "由 AIwrite 界面生成：AIwrite →「设置」→「提供商与站点表」→"
                       "「重新加载配置表」即可生效（无需重启）。每个提供商一份文件，可直接分享/备份。";
    root["providers"] = ordered_json::array({entry});
    return root.dump(2) + "\n";
}

bool writeUserOfficialFile(const OfficialProviderTemplate& tpl, const std::filesystem::path& dir,
                           bool overwrite, std::string* error, std::filesystem::path* written)
{
    const std::string problem = officialTemplateError(tpl);
    if (!problem.empty()) {
        if (error != nullptr) {
            *error = problem;
        }
        return false;
    }
    return writeUserProviderFile(tpl.id, officialFileJson(tpl), dir, overwrite, error, written);
}

// ============================================================================
//  自检（离线；`api_probe --official-assert` 调用；返回失败项数）
// ============================================================================

int officialTemplateSelftest(int* passed_out)
{
    std::printf("  —— 官方 API 提供商模板自检（自定义 AI 收口）——\n");
    CheckCounter counter;

    // ---- ① id 合法性（含路径穿越；与站点同一规则 `providerIdError`）----
    counter.check(!officialIdError("").empty(), "id：空串被拒");
    counter.check(!officialIdError("../evil").empty(), "id：路径穿越被拒（.. + 斜杠）");
    counter.check(!officialIdError("a/b").empty(), "id：斜杠被拒");
    counter.check(!officialIdError("a b").empty(), "id：空格被拒");
    counter.check(!officialIdError("  ab").empty(), "id：首部空白被拒");
    counter.check(!officialIdError("ab ").empty(), "id：尾部空白被拒");
    counter.check(!officialIdError("中文").empty(), "id：非 ASCII 被拒");
    counter.check(officialIdError("1ab").empty(), "id：数字开头合法");
    counter.check(officialIdError("my-provider_2.0").empty(), "id：合法字符组合通过");
    counter.check(!officialIdError(std::string(65, 'a')).empty(), "id：超 64 被拒");

    // ---- ② 模板整体校验（api_base 必填 / http(s) / 协议实现）----
    OfficialProviderTemplate tpl = officialTemplateDefault();
    tpl.id = "my-ai";
    tpl.api_base = "https://api.example.com/v1";
    counter.check(officialTemplateError(tpl).empty(), "模板：合法条目零错误");

    OfficialProviderTemplate no_base = tpl;
    no_base.api_base = "";
    counter.check(!officialTemplateError(no_base).empty(), "模板：api_base 空被拒");

    OfficialProviderTemplate bad_url = tpl;
    bad_url.api_base = "ftp://x";
    counter.check(!officialTemplateError(bad_url).empty(), "模板：非 http(s) 地址被拒");

    OfficialProviderTemplate bad_proto = tpl;
    bad_proto.protocol = "dom";
    counter.check(!officialTemplateError(bad_proto).empty(), "模板：未实现协议被拒");

    // ---- ③ 生成 JSON：带信封 + 零错误合并 + 字段往返 ----
    const std::string json = officialFileJson(tpl);
    counter.check(json.find("\"schema_version\"") != std::string::npos, "生成：文件带信封");
    counter.check(json.find("\"kind\": \"official\"") != std::string::npos, "生成：kind=official");
    SpecLoadReport report;
    const ProviderSpecs merged = merge_provider_specs(
        {{"builtin", selftest_base_layer_json()}, {"user.d", json}}, &report);
    counter.check(report.errors.empty(), "合并：生成的文件零错误",
                  report.errors.empty() ? "" : report.errors.front());
    const ProviderSpec* confirmed = merged.find("my-ai");
    counter.check(confirmed != nullptr && confirmed->kind == "official",
                  "合并：条目进表且 kind=official");
    counter.check(confirmed != nullptr && confirmed->api_base == tpl.api_base,
                  "合并：api_base 往返一致");
    counter.check(confirmed != nullptr && confirmed->protocol == "openai",
                  "合并：protocol 往返一致");
    counter.check(confirmed != nullptr && confirmed->key_ref_default == "brain-ai/my-ai",
                  "合并：key_ref 未填 → 默认 brain-ai/<id>");
    counter.check(confirmed != nullptr && confirmed->models.empty(),
                  "合并：models 未填 → 模型自由填写");

    // ---- ④ 带模型 / 能力 / 视觉默认模型 / env_names ----
    OfficialProviderTemplate rich = tpl;
    rich.models        = "model-a, model-b"; // 逗号分隔（含空格）
    rich.vision        = true;
    rich.vision_model  = "model-a";
    rich.seed          = false;
    rich.env_name      = "MY_AI_KEY";
    const std::string  rich_json = officialFileJson(rich);
    SpecLoadReport     report_rich;
    const ProviderSpecs merged_rich = merge_provider_specs(
        {{"builtin", selftest_base_layer_json()}, {"user.d", rich_json}}, &report_rich);
    const ProviderSpec* rich_spec = merged_rich.find("my-ai");
    counter.check(rich_spec != nullptr && rich_spec->models.size() == 2 &&
                      rich_spec->models[0].id == "model-a",
                  "生成：模型清单（逗号分隔 + 空格归一）往返一致");
    counter.check(rich_spec != nullptr && rich_spec->caps.vision &&
                      rich_spec->vision_model_default == "model-a" && !rich_spec->caps.seed,
                  "生成：能力（vision / seed=false）+ 视觉默认模型往返一致");
    counter.check(rich_spec != nullptr && rich_spec->env_names.size() == 1 &&
                      rich_spec->env_names.front() == "MY_AI_KEY",
                  "生成：env_names 往返一致");

    // ---- ⑤ 转义 / 中文往返 ----
    OfficialProviderTemplate escaped = tpl;
    escaped.id      = "esc-ai";
    escaped.display = "带\"引号\"与\\反斜杠的服务";
    escaped.notes   = "中文备注：自建（未实测）";
    SpecLoadReport     report_esc;
    const ProviderSpecs specs_esc = merge_provider_specs(
        {{"builtin", selftest_base_layer_json()},
         {"user.d", officialFileJson(escaped)}},
        &report_esc);
    const ProviderSpec* esc_spec = specs_esc.find("esc-ai");
    counter.check(esc_spec != nullptr && esc_spec->display == escaped.display,
                  "转义：显示名（引号 / 反斜杠 / 中文）往返一致");

    // ---- ⑥ 照抄内置条目（R18：照抄就能建）----
    ProviderSpec source;
    source.id                  = "deepseek";
    source.kind                = "official";
    source.display             = "DeepSeek（官方 API）";
    source.protocol            = "openai";
    source.api_base            = "https://api.deepseek.com";
    source.chat_path           = "/chat/completions";
    source.auth_style          = "bearer";
    source.env_names           = {"DEEPSEEK_API_KEY"};
    source.key_ref_default     = "brain-ai/deepseek";
    source.caps.vision         = true;
    source.caps.seed           = true;
    source.caps.system_role    = true;
    source.models.push_back(ProviderModelSpec{"deepseek-chat", "deepseek-chat（通用）", false});
    source.models.push_back(ProviderModelSpec{"deepseek-flash", "deepseek-flash（视觉）", true});
    source.vision_model_default = "deepseek-flash";
    source.notes                = "已实测";
    const OfficialProviderTemplate copied = officialTemplateFromSpec(&source);
    counter.check(copied.id == "deepseek" && copied.api_base == "https://api.deepseek.com",
                  "照抄：提供商身份（id / api_base）已抄");
    counter.check(copied.models == "deepseek-chat,deepseek-flash", "照抄：模型清单已抄");
    counter.check(copied.vision && copied.vision_model == "deepseek-flash", "照抄：视觉能力已抄");
    counter.check(copied.env_name == "DEEPSEEK_API_KEY", "照抄：env_names 已抄");
    counter.check(!officialFileJson(copied).empty(), "照抄：可直接生成文件 JSON");

    // ---- ⑦ 落盘（**自建临时目录**；不碰用户数据）----
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "aiwrite-official-template-selftest";
    std::filesystem::remove_all(dir, ec);
    std::string           err;
    std::filesystem::path written;
    counter.check(writeUserOfficialFile(tpl, dir, false, &err, &written) &&
                      std::filesystem::exists(written),
                  "落盘：首次写成功（目录自动创建）", err);
    counter.check(!writeUserOfficialFile(tpl, dir, false, &err, nullptr),
                  "落盘：同名文件默认拒绝覆盖");
    counter.check(err.find("已存在同名文件") != std::string::npos,
                  "落盘：拒绝文案可操作（点明如何覆盖）", err);
    OfficialProviderTemplate changed = tpl;
    changed.display = "改名后";
    counter.check(writeUserOfficialFile(changed, dir, true, &err, nullptr), "落盘：显式覆盖成功",
                  err);
    {
        std::ifstream in(written, std::ios::binary);
        std::string   body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        counter.check(body.find("改名后") != std::string::npos, "落盘：覆盖后内容已更新");
        counter.check(body.find("\"schema_version\"") != std::string::npos, "落盘：文件带信封");
    }
    counter.check(!std::filesystem::exists(std::filesystem::path(written.string() + ".tmp")),
                  "落盘：无残留 .tmp");
    OfficialProviderTemplate bad = tpl;
    bad.id = "../evil";
    counter.check(!writeUserOfficialFile(bad, dir, true, &err, nullptr), "落盘：坏 id 不落盘");
    std::filesystem::remove_all(dir, ec);
    counter.check(!std::filesystem::exists(dir), "落盘：自检目录已清理（零残留）");

    if (passed_out != nullptr) {
        *passed_out = counter.passed;
    }
    std::printf("   官方 API 提供商模板自检结果：%d 通过 / %d 失败\n", counter.passed, counter.failed);
    return counter.failed;
}

} // namespace aiwrite::ai

