// provider_spec.cpp -- Provider 配置表：加载 / 合并 / 缓存（唯一入口）
//
// ARCH-Z1（2026-10-23）：原 1743 行单文件按职责拆成 5 个 TU，本文件 = 加载层。
//   ① 层拼装与优先级（builtin -> source -> user.d/*.json -> user）
//   ② replace_all / schema_version 的层间规则  ③ 进程内快照与 reload
//   解析/校验/兜底 = provider_spec_parse.cpp；站点派生 = provider_spec_web.cpp；
//   纯查询 = provider_spec_query.cpp；离线断言 = provider_spec_selftest.cpp。
//   内部契约（仅 3 个入口）= ai/include/provider_spec_detail.h。
//   行为等价：错误/警告的**文本与顺序**逐字保留（自检与 CLI 都断言原文）。

#include "provider_spec.h"
#include "provider_spec_detail.h"

#include "log.h"
#include "paths.h"
#include "string_util.h"   // CLN-05：trim 收敛

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {

using nlohmann::json;
using provider_detail::applyEntry;
using provider_detail::fallbackSpecs;
using provider_detail::kSchemaVersion;
using provider_detail::startsWith;
using provider_detail::validateFinal;

// ============================================================================
//  公开接口：合并 / 加载 / 缓存（PB2-01 / PB2-02 / PB2-03）
// ============================================================================
ProviderSpecs merge_provider_specs(
    const std::vector<std::pair<std::string, std::string>>& layers, SpecLoadReport* report)
{
    SpecLoadReport  local;
    SpecLoadReport& rep = report != nullptr ? *report : local;
    rep                 = SpecLoadReport{};

    std::vector<ProviderSpec>          items;
    std::map<std::string, std::size_t> index;

    for (const auto& layer : layers) {
        const std::string& label = layer.first;
        if (utils::trim(layer.second).empty()) {
            continue; // 文件不存在（loader 不传）或空文件
        }
        rep.files.push_back(label);

        json root;
        try {
            root = json::parse(layer.second);
        }
        catch (const std::exception& ex) {
            rep.errors.push_back("「" + label + "」JSON 解析失败：" + ex.what() + "（已忽略该层）");
            continue;
        }
        if (!root.is_object()) {
            rep.errors.push_back("「" + label + "」顶层应为对象（已忽略该层）");
            continue;
        }
        if (!root.contains("schema_version") || !root["schema_version"].is_number_integer()) {
            rep.errors.push_back("「" + label + "」缺少 schema_version（应为整数 1；已忽略该层）");
            continue;
        }
        if (root["schema_version"].get<int>() != kSchemaVersion) {
            rep.errors.push_back("「" + label + "」schema_version=" +
                                 std::to_string(root["schema_version"].get<int>()) +
                                 " 不受支持（本程序支持 " + std::to_string(kSchemaVersion) +
                                 "；已忽略该层）");
            continue;
        }
        const bool user_layer = startsWith(label, "user");
        if (root.contains("replace_all")) {
            if (!root["replace_all"].is_boolean()) {
                rep.warnings.push_back("「" + label + "」replace_all 类型不符（已忽略）");
            }
            else if (root["replace_all"].get<bool>()) {
                if (!user_layer) {
                    rep.warnings.push_back("「" + label + "」内置层不支持 replace_all（已忽略）");
                }
                else {
                    items.clear();
                    index.clear();
                    rep.warnings.push_back("「" + label + "」replace_all=true：已忽略下层条目");
                }
            }
        }
        if (!root.contains("providers") || !root["providers"].is_array()) {
            rep.errors.push_back("「" + label + "」缺少 providers 数组（已忽略该层）");
            continue;
        }
        for (const json& entry : root["providers"]) {
            if (!entry.is_object()) {
                rep.errors.push_back("「" + label + "」providers[] 含非对象条目（已跳过）");
                continue;
            }
            if (!entry.contains("id") || !entry["id"].is_string() ||
                utils::trim(entry["id"].get<std::string>()).empty()) {
                rep.errors.push_back("「" + label + "」providers[] 条目缺少 id（已跳过）");
                continue;
            }
            const std::string id = utils::trim(entry["id"].get<std::string>());

            ProviderSpec working;
            const auto   found = index.find(id);
            if (found != index.end()) {
                working = items[found->second]; // 字段级合并：只覆盖本层出现的字段
            }
            if (!applyEntry(entry, label, &working, &rep)) {
                continue; // 该条被跳过；已有条目不受影响
            }
            working.id     = id;
            working.origin = label;
            if (found == index.end()) {
                index[id] = items.size();
                items.push_back(std::move(working));
            }
            else {
                items[found->second] = std::move(working);
            }
        }
    }

    validateFinal(&items, &rep);
    if (items.empty()) {
        items             = fallbackSpecs();
        rep.fallback_used = true;
        rep.warnings.push_back(
            "配置表缺失或不可用：已使用最小兜底表（custom-official + deepseek）");
    }
    rep.ok = rep.errors.empty();

    ProviderSpecs result;
    result.items  = std::move(items);
    result.report = rep;
    return result;
}


ProviderSpecs loadProviderSpecs()
{
    std::vector<std::pair<std::string, std::string>> layers;

    const auto add_file = [&layers](const std::filesystem::path& file, const std::string& label) {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec) || std::filesystem::is_directory(file, ec)) {
            return;
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            return;
        }
        std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
        if (utils::trim(text).empty()) {
            return;
        }
        layers.emplace_back(label, std::move(text));
    };

    // ① 随程序发布的内置表（权威默认）
    add_file(paths::providersAssetFile(), "builtin");
    // ② 开发态兜底（源码树直跑 / 自检）
#ifdef AIWRITE_SOURCE_DIR
    add_file(std::filesystem::path(AIWRITE_SOURCE_DIR) / "assets" / "providers.json", "source");
#endif
    // ③ 用户新增条目（文件名升序）
    std::error_code                    ec;
    std::vector<std::filesystem::path> user_files;
    const std::filesystem::path        user_dir = paths::userProvidersDir();
    if (std::filesystem::is_directory(user_dir, ec)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(user_dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
                user_files.push_back(entry.path());
            }
        }
    }
    std::sort(user_files.begin(), user_files.end());
    for (const std::filesystem::path& file : user_files) {
        add_file(file, "user.d/" + file.filename().string());
    }
    // ④ 用户字段级覆盖
    add_file(paths::userProvidersFile(), "user");

    return merge_provider_specs(layers, nullptr);
}

namespace {

std::mutex& specs_mutex()
{
    static std::mutex value;
    return value;
}

std::shared_ptr<const ProviderSpecs>& specs_holder()
{
    static std::shared_ptr<const ProviderSpecs> value;
    return value;
}

} // namespace

std::shared_ptr<const ProviderSpecs> providerSpecsSnapshot()
{
    std::lock_guard<std::mutex> lock(specs_mutex());
    if (!specs_holder()) {
        specs_holder() = std::make_shared<const ProviderSpecs>(loadProviderSpecs());
        const SpecLoadReport& report = specs_holder()->report;
        log::info("[配置表] " + report.summary() + "，条目 " +
                  std::to_string(specs_holder()->items.size()) + " 条");
        for (const std::string& warning : report.warnings) {
            log::warn("[配置表] " + warning);
        }
        for (const std::string& error : report.errors) {
            log::error("[配置表] " + error);
        }
    }
    return specs_holder();
}

const ProviderSpecs& providerSpecs()
{
    return *providerSpecsSnapshot();
}

void reloadProviderSpecs()
{
    std::shared_ptr<const ProviderSpecs> fresh =
        std::make_shared<const ProviderSpecs>(loadProviderSpecs());
    {
        std::lock_guard<std::mutex> lock(specs_mutex());
        specs_holder() = fresh;
    }
    log::info("[配置表] 已重新加载：" + fresh->report.summary() + "，条目 " +
              std::to_string(fresh->items.size()) + " 条（含网页版站点 " +
              std::to_string(fresh->countKind("web")) + " 个）");
    for (const std::string& warning : fresh->report.warnings) {
        log::warn("[配置表] " + warning);
    }
    for (const std::string& error : fresh->report.errors) {
        log::error("[配置表] " + error);
    }
}

} // namespace aiwrite::ai
