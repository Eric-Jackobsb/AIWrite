#include "composite.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "log.h"
#include "paths.h"

namespace aiwrite::engine {

// -------------------------------------------------------------- 注册表 ----
CompositeRegistry& CompositeRegistry::instance()
{
    static CompositeRegistry registry;
    return registry;
}

void CompositeRegistry::setRoot(std::filesystem::path root)
{
    builtin_root_     = root;
    user_root_        = std::move(root);
    roots_overridden_ = true; // 测试隔离：跳过旧目录迁移（不触碰用户真实数据）
    specs_.clear();
    source_.clear();
    reload();
}

void CompositeRegistry::setRoots(std::filesystem::path builtinRoot,
                                  std::filesystem::path userRoot)
{
    builtin_root_     = std::move(builtinRoot);
    user_root_        = std::move(userRoot);
    roots_overridden_ = true;
    specs_.clear();
    source_.clear();
    reload();
}

const std::filesystem::path& CompositeRegistry::builtinRoot() const
{
    return builtin_root_.empty() ? paths::builtinCompositesDir() : builtin_root_;
}

const std::filesystem::path& CompositeRegistry::userRoot() const
{
    return user_root_.empty() ? paths::userCompositesDir() : user_root_;
}

std::filesystem::path CompositeRegistry::root() const
{
    return userRoot(); // 兼容旧调用：用户根 = 写入口
}

void CompositeRegistry::reload()
{
    specs_.clear();
    source_.clear();
    loadFromDir(builtinRoot(), CompositeSource::Builtin);
    loadFromDir(userRoot(), CompositeSource::User); // 用户源后扫 ⇒ 覆盖内置源
    if (!roots_overridden_) {
        migrateLegacy(); // 旧 ~/.brain-ai/composites 首次并入用户源
    }
    loaded_ = true;
}

const CompositeSpec* CompositeRegistry::find(const std::string& id) const
{
    ensureLoaded();
    for (const CompositeSpec& spec : specs_) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<const CompositeSpec*> CompositeRegistry::list() const
{
    ensureLoaded();
    std::vector<const CompositeSpec*> result;
    result.reserve(specs_.size());
    for (const CompositeSpec& spec : specs_) {
        result.push_back(&spec);
    }
    std::sort(result.begin(), result.end(),
              [](const CompositeSpec* a, const CompositeSpec* b) { return a->name < b->name; });
    return result;
}

void CompositeRegistry::ensureLoaded() const
{
    if (!loaded_) {
        // 场景：workflow 加载早于 UI 首帧 → 注册表在第一次访问时扫描目录
        const_cast<CompositeRegistry*>(this)->reload();
    }
}

void CompositeRegistry::loadFromDir(const std::filesystem::path& dir, CompositeSource source)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return; // 目录不存在 = 空注册表
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream file(entry.path());
        if (!file.is_open()) {
            log::warn("[上级节点] 无法读取文件: " + entry.path().string());
            continue;
        }
        nlohmann::json document;
        try {
            file >> document;
        }
        catch (const nlohmann::json::parse_error&) {
            log::warn("[上级节点] 跳过损坏文件: " + entry.path().string());
            continue;
        }
        CompositeSpec spec;
        std::string   parse_error;
        if (!compositeFromJson(document, spec, &parse_error)) {
            log::warn("[上级节点] 跳过无效规格 " + entry.path().string() + "：" + parse_error);
            continue;
        }
        // PS-D9：后扫覆盖前扫 —— 用户源覆盖内置源（同 id 只保留最后加载的一份）
        specs_.erase(std::remove_if(specs_.begin(), specs_.end(),
                                    [&spec](const CompositeSpec& s) { return s.id == spec.id; }),
                     specs_.end());
        specs_.push_back(std::move(spec));
        source_[specs_.back().id] = source;
    }
    std::sort(specs_.begin(), specs_.end(),
              [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
}

bool CompositeRegistry::save(const CompositeSpec& spec, std::string* error,
                             bool allow_incomplete_draft)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::string validate_error;
    if (!validate_composite_spec(spec, &validate_error, allow_incomplete_draft)) {
        return fail(validate_error);
    }

    // PS-D9：内置源只读 —— 直接覆盖内置 id 被拒（须先「复制到我的节点库」）
    if (sourceOf(spec.id) == CompositeSource::Builtin) {
        return fail("内置官方预设为只读：请先「复制到我的节点库」再编辑");
    }

    const std::filesystem::path dir = userRoot();
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return fail("无法创建上级节点目录: " + dir.string());
    }

    const std::filesystem::path file = dir / (spec.id + ".json");
    std::ofstream               out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return fail("无法写入文件: " + file.string());
    }
    out << compositeToJson(spec).dump(4);
    out.close();
    if (!out.good()) {
        return fail("写入文件失败: " + file.string());
    }

    // 写盘成功 → 更新内存副本（同 id 覆盖；来源 = 用户）
    auto it = std::find_if(specs_.begin(), specs_.end(),
                           [&spec](const CompositeSpec& s) { return s.id == spec.id; });
    if (it != specs_.end()) {
        *it = spec;
    }
    else {
        specs_.push_back(spec);
        std::sort(specs_.begin(), specs_.end(),
                  [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
    }
    source_[spec.id] = CompositeSource::User;

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool CompositeRegistry::remove(const std::string& id, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    const CompositeSource source = sourceOf(id);
    if (source == CompositeSource::None) {
        if (error != nullptr) {
            error->clear();
        }
        return true; // 幂等：不存在视为删除成功
    }
    if (source == CompositeSource::Builtin) {
        return fail("内置官方预设为只读：不能删除");
    }

    const std::filesystem::path file = userRoot() / (id + ".json");
    std::error_code             ec;
    if (std::filesystem::exists(file, ec)) {
        if (!std::filesystem::remove(file, ec) || ec) {
            return fail("删除文件失败: " + file.string());
        }
    }
    // 重新扫描双源：用户副本删除后，若内置仍有同 id ⇒ 来源回落 Builtin（PS-D9 §2.2）
    reload();

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

CompositeSource CompositeRegistry::sourceOf(const std::string& id) const
{
    ensureLoaded();
    const auto it = source_.find(id);
    return (it != source_.end()) ? it->second : CompositeSource::None;
}

bool CompositeRegistry::copyToUser(const std::string& id, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    const CompositeSpec* spec = find(id);
    if (spec == nullptr) {
        return fail("找不到上级节点规格: " + id);
    }
    if (sourceOf(id) == CompositeSource::User) {
        if (error != nullptr) {
            error->clear();
        }
        return true; // 已是用户源，无需复制
    }

    // 写入用户源（同 id 覆盖官方资产是有意为之：用户接管后来源切 User、可编辑）
    const std::filesystem::path dir = userRoot();
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return fail("无法创建上级节点目录: " + dir.string());
    }
    const std::filesystem::path file = dir / (id + ".json");
    std::ofstream               out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return fail("无法写入文件: " + file.string());
    }
    out << compositeToJson(*spec).dump(4);
    out.close();
    if (!out.good()) {
        return fail("写入文件失败: " + file.string());
    }

    // 内存：同 id 覆盖为当前规格内容，来源切 User
    auto it = std::find_if(specs_.begin(), specs_.end(),
                           [&id](const CompositeSpec& s) { return s.id == id; });
    if (it != specs_.end()) {
        *it = *spec;
    }
    else {
        specs_.push_back(*spec);
        std::sort(specs_.begin(), specs_.end(),
                  [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
    }
    source_[id] = CompositeSource::User;

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// PS-D9：旧目录迁移 —— 首次 reload 把 ~/.brain-ai/composites/ 遗留文件并入用户源
//（复制后删除原文件，用户节点不丢失）；进程内只尝试一次，成功后旧目录清空 ⇒ 幂等。
//  * 仅在默认用户根（未 set_root/set_roots 覆盖）时执行，避免测试误触碰用户真实数据。
void CompositeRegistry::migrateLegacy()
{
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;

    const std::filesystem::path legacy = paths::compositesDir();
    std::error_code             ec;
    if (!std::filesystem::is_directory(legacy, ec)) {
        return;
    }
    int migrated = 0;
    for (const auto& entry : std::filesystem::directory_iterator(legacy, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        nlohmann::json document;
        try {
            std::ifstream in(entry.path());
            if (!in.is_open()) {
                continue;
            }
            in >> document;
        }
        catch (const nlohmann::json::parse_error&) {
            log::warn("[上级节点] 旧目录迁移：跳过无法解析的文件 " + entry.path().string());
            continue;
        }
        CompositeSpec spec;
        std::string   parse_error;
        if (!compositeFromJson(document, spec, &parse_error)) {
            log::warn("[上级节点] 旧目录迁移：跳过无效规格 " + entry.path().string() + "：" +
                      parse_error);
            continue;
        }
        // 复制到用户源（同 id 覆盖语义）
        const std::filesystem::path dst = userRoot() / (spec.id + ".json");
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            log::error("[上级节点] 旧目录迁移：无法写入 " + dst.string());
            continue;
        }
        out << compositeToJson(spec).dump(4);
        out.close();
        if (!out.good()) {
            log::error("[上级节点] 旧目录迁移：写入失败 " + dst.string());
            continue;
        }
        // 写入成功 → 删除原文件；内存并入（来源 = User，覆盖内置）
        const std::string id = spec.id;
        std::error_code   rm_ec;
        std::filesystem::remove(entry.path(), rm_ec);
        specs_.erase(std::remove_if(specs_.begin(), specs_.end(),
                                    [&id](const CompositeSpec& s) { return s.id == id; }),
                     specs_.end());
        specs_.push_back(std::move(spec));
        source_[id] = CompositeSource::User;
        ++migrated;
    }
    std::sort(specs_.begin(), specs_.end(),
              [](const CompositeSpec& a, const CompositeSpec& b) { return a.id < b.id; });
    if (migrated > 0) {
        log::info("[上级节点] 旧目录迁移完成：已并入用户源 " + std::to_string(migrated) +
                  " 个规格（" + legacy.string() + "）");
    }
}

std::string CompositeRegistry::makeId() const
{
    ensureLoaded();
    std::set<int> used;
    for (const CompositeSpec& spec : specs_) {
        if (spec.id.rfind("c", 0) == 0) {
            try {
                used.insert(std::stoi(spec.id.substr(1)));
            }
            catch (...) {
            }
        }
    }
    int candidate = 1;
    while (used.count(candidate) > 0) {
        ++candidate;
    }
    return "c" + std::to_string(candidate);
}

} // namespace aiwrite::engine
