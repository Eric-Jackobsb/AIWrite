#include "autosave.h"

#include "log.h"
#include "node_registry.h"
#include "paths.h"
#include "workflow_io.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace aiwrite::engine {

namespace {

// 快照 JSON 的元信息节（顶层 `autosave` 对象）
//  * 放在工作流 JSON 里 ⇒ 快照本身就是一份可被「打开工作流…」直接读的工作流文件
//  * 旧解析器只读已知键 ⇒ `autosave` 节对 workflow_io 完全无害（不报警、不报错）
constexpr const char* kMetaKey = "autosave";

// 历史轮转的最大扫描数（`autosave_clear()` 用它兜底清理，与 `ui.autosave_keep` 解耦）
constexpr int kHistoryScanLimit = 64;

std::string now_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_s(&local, &now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

bool read_text(const std::filesystem::path& file, std::string* out)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return false;
    }
    *out = std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return true;
}

// 原子写：先写 `<file>.tmp` 并校验，再 `rename` 整文件替换（VC-01）
//  * 与 utils/config.cpp、ai/site_template.cpp 同一写法（仓库既有约定）
//  * 任何一步失败都不会破坏 `<file>` 的既有内容
bool write_text_atomic(const std::filesystem::path& file, const std::string& body,
                       std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::error_code code;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), code);
        if (code) {
            return fail("无法创建目录 " + file.parent_path().string() + "（" + code.message() + "）");
        }
    }

    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return fail("无法写入临时文件 " + temp.string());
        }
        stream << body;
        if (!stream.good()) {
            stream.close();
            std::filesystem::remove(temp, code);
            return fail("写入不完整（原快照保持不变）：" + temp.string());
        }
    }

    std::filesystem::rename(temp, file, code);
    if (code) {
        std::error_code ignore;
        std::filesystem::remove(temp, ignore);
        return fail("快照替换失败：" + file.string() + "（" + code.message() + "）");
    }
    return true;
}

} // namespace

AutosavePaths autosave_paths(const std::filesystem::path& dir)
{
    AutosavePaths layout;
    // 默认落点 = `~/.brain-ai/snapshots`（复用 utils::paths::snapshots_dir()，不新增目录语义）
    layout.dir    = dir.empty() ? aiwrite::paths::snapshots_dir() : dir;
    layout.latest = layout.dir / "autosave.json";
    layout.temp   = layout.dir / "autosave.json.tmp";
    return layout;
}

std::filesystem::path AutosavePaths::history(int index) const
{
    return dir / ("autosave_" + std::to_string(index) + ".json");
}

std::string graph_content_signature(const Graph& graph)
{
    nlohmann::json document = graph_to_json(graph);
    // 视口（平移 / 缩放）不算「工作流变更」⇒ 不动画布也会被节流判为「无变化」
    document.erase("version");
    document.erase("viewport");
    return document.dump();
}

AutosaveInfo autosave_probe(const std::filesystem::path& dir)
{
    const AutosavePaths layout = autosave_paths(dir);

    AutosaveInfo info;
    info.file = layout.latest;

    std::error_code code;
    if (!std::filesystem::exists(layout.latest, code)) {
        info.error = "没有自动保存快照（" + layout.latest.string() + "）";
        return info;
    }

    std::string raw;
    if (!read_text(layout.latest, &raw)) {
        info.error = "无法读取快照：" + layout.latest.string();
        return info;
    }

    try {
        const nlohmann::json document = nlohmann::json::parse(raw);
        if (!document.is_object() || !document.contains("nodes") || !document["nodes"].is_array()) {
            info.error = "快照不是工作流 JSON（缺 nodes 数组）";
            return info;
        }

        info.name  = document.value("name", std::string());
        info.nodes = static_cast<int>(document["nodes"].size());
        info.edges = (document.contains("edges") && document["edges"].is_array())
                         ? static_cast<int>(document["edges"].size())
                         : 0;

        if (const auto meta = document.find(kMetaKey);
            meta != document.end() && meta->is_object()) {
            info.workflow_path = meta->value("workflow_path", std::string());
            info.saved_at      = meta->value("saved_at", std::string());
            if (const std::string meta_name = meta->value("name", std::string());
                !meta_name.empty()) {
                info.name = meta_name;
            }
        }

        info.ok = true;
        info.error.clear();
        return info;
    }
    catch (const std::exception& ex) {
        info.error = std::string("快照损坏（JSON 解析失败）：") + ex.what();
        return info;
    }
}

bool autosave_write(const Graph& graph, const std::string& workflow_path, int keep,
                    AutosaveInfo* info, std::string* error, const std::filesystem::path& dir)
{
    const AutosavePaths layout = autosave_paths(dir);
    std::error_code     code;

    nlohmann::json document = graph_to_json(graph);
    nlohmann::json meta;
    meta["workflow_path"] = workflow_path;
    meta["name"]          = graph.name;
    meta["saved_at"]      = now_string();
    meta["saved_at_epoch"] = static_cast<long long>(std::time(nullptr)); // 仅便于排查（判定用文件 mtime）
    meta["nodes"]          = static_cast<int>(graph.nodes.size());
    meta["edges"]          = static_cast<int>(graph.edges.size());
    document[kMetaKey]     = std::move(meta);

    // ① 轮转历史（**先复制旧最新**，再原子替换 —— 保证任何时刻 autosave.json 都完整）
    if (keep > 0 && std::filesystem::exists(layout.latest, code)) {
        std::filesystem::remove(layout.history(keep), code); // 丢掉最旧一份
        for (int index = keep - 1; index >= 1; --index) {
            const std::filesystem::path from = layout.history(index);
            code.clear();
            if (std::filesystem::exists(from, code)) {
                std::filesystem::rename(from, layout.history(index + 1), code);
            }
        }
        code.clear();
        std::filesystem::copy_file(layout.latest, layout.history(1),
                                   std::filesystem::copy_options::overwrite_existing, code);
        if (code) {
            log::warn("[自动保存] 历史快照轮转失败（继续写最新）：" + code.message());
            code.clear();
        }
    }

    // ②③ 原子写最新
    if (!write_text_atomic(layout.latest, document.dump(2) + "\n", error)) {
        return false;
    }

    if (info != nullptr) {
        AutosaveInfo written;
        written.ok            = true;
        written.file          = layout.latest;
        written.workflow_path = workflow_path;
        written.name          = graph.name;
        written.saved_at      = document[kMetaKey].value("saved_at", std::string());
        written.nodes         = static_cast<int>(graph.nodes.size());
        written.edges         = static_cast<int>(graph.edges.size());
        *info                 = std::move(written);
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool autosave_load(Graph& graph, std::string* error, const std::filesystem::path& dir)
{
    const AutosavePaths layout = autosave_paths(dir);

    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::string raw;
    if (!read_text(layout.latest, &raw)) {
        return fail("无法读取快照：" + layout.latest.string());
    }
    try {
        return graph_from_json(nlohmann::json::parse(raw), graph, error);
    }
    catch (const std::exception& ex) {
        return fail(std::string("快照 JSON 解析失败：") + ex.what());
    }
}

bool autosave_clear(std::string* error, const std::filesystem::path& dir)
{
    const AutosavePaths layout = autosave_paths(dir);
    std::error_code     code;
    std::filesystem::remove(layout.latest, code);
    code.clear();
    std::filesystem::remove(layout.temp, code); // 连同半成品一起清掉
    for (int index = 1; index <= kHistoryScanLimit; ++index) {
        code.clear();
        std::filesystem::remove(layout.history(index), code);
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

int autosave_cleanup_temp(std::string* error, const std::filesystem::path& dir)
{
    const AutosavePaths layout = autosave_paths(dir);
    std::error_code     code;
    int                 removed = 0;
    if (std::filesystem::exists(layout.temp, code)) {
        code.clear();
        if (std::filesystem::remove(layout.temp, code)) {
            ++removed;
        }
        else if (code && error != nullptr) {
            *error = "残留临时文件删除失败：" + layout.temp.string() + "（" + code.message() + "）";
        }
    }
    else if (error != nullptr) {
        error->clear();
    }
    return removed;
}

bool autosave_should_offer_restore(const AutosaveInfo& info, std::string* reason)
{
    const auto answer = [reason](bool value, const std::string& text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return value;
    };

    if (!info.ok) {
        return answer(false, info.error);
    }

    // 未命名工作流：快照无处可比 ⇒ 视为「有未保存的更改」，提示恢复（否则丢稿）
    if (info.workflow_path.empty()) {
        return answer(true, "快照未关联工作流文件（未命名工作流）");
    }

    std::error_code code;
    if (!std::filesystem::exists(info.workflow_path, code)) {
        return answer(true, "原工作流文件不存在：" + info.workflow_path);
    }

    code.clear();
    const auto autosave_time = std::filesystem::last_write_time(info.file, code);
    if (code) {
        return answer(false, "无法读取快照时间：" + code.message());
    }
    code.clear();
    const auto workflow_time = std::filesystem::last_write_time(info.workflow_path, code);
    if (code) {
        return answer(false, "无法读取工作流文件时间：" + code.message());
    }

    if (autosave_time > workflow_time) {
        return answer(true, "快照比 " + info.workflow_path + " 新（有未保存的更改）");
    }
    return answer(false, "快照不比 " + info.workflow_path + " 新（已保存）");
}

// ------------------------------------------------ 自检（VC-01 / VC-02）--------
namespace {

// 自检计数助手（与 ai::site_template_selftest / provider_spec_selftest 同风格）
struct CheckCounter {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name, const std::string& detail = {})
    {
        if (condition) {
            ++passed;
            std::printf("   PASS  %s\n", name.c_str());
        }
        else {
            ++failed;
            std::printf("   FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ->  ",
                        detail.c_str());
        }
        std::fflush(stdout); // 本机 stdout 可能被 shell 集成吞掉/缓冲：逐项落盘更可靠（AI_README §3）
    }
};

// 构造一份确定性测试图（2 节点 / 1 连线 + 非默认视口）
Graph make_selftest_graph(std::string* node_id, std::string* error)
{
    registerAllNodes(); // 幂等（与 workflow_io 的加载路径同一做法）

    Graph graph;
    graph.name        = "自检工作流";
    graph.description = "PC-01/02 自检：自动保存 + 崩溃恢复";

    const std::string input  = graph.addNode("TextInput", 10.0f, 20.0f, error);
    const std::string prompt = graph.addNode("PromptTemplate", 300.0f, 20.0f, error);
    if (input.empty() || prompt.empty()) {
        return graph;
    }

    if (Node* node = graph.findNode(input)) {
        if (Param* param = node->findParam("text")) {
            param->value = "崩溃恢复自检：这段文字必须能从快照里恢复";
        }
    }
    graph.addEdge(input, "text", prompt, "vars", error);

    graph.viewport_x    = 123.0f;
    graph.viewport_y    = -45.0f;
    graph.viewport_zoom = 1.5f;

    if (node_id != nullptr) {
        *node_id = input;
    }
    return graph;
}

} // namespace

int autosave_selftest(int* passed_out)
{
    std::printf("[PC-01/02] 自动保存 / 崩溃恢复自检（场景全部在系统临时目录内构造）\n");
    CheckCounter counter;

    std::error_code             code;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(code) / "aiwrite-autosave-selftest";
    std::filesystem::remove_all(dir, code);
    std::filesystem::create_directories(dir, code);

    const AutosavePaths layout = autosave_paths(dir);
    counter.check(layout.latest.filename() == "autosave.json" && layout.dir == dir,
                  "路径：快照落在 <dir>/autosave.json（历史 autosave_<N>.json）");

    // ---- 构造 ----
    std::string error;
    std::string input_id;
    const Graph graph           = make_selftest_graph(&input_id, &error);
    const std::string signature = graph_content_signature(graph);
    counter.check(!input_id.empty() && graph.nodes.size() == 2 && graph.edges.size() == 1,
                  "构造：测试图 2 节点 / 1 连线", error);

    const std::filesystem::path workflow_file = dir / "自检工作流.json";

    // ---- ① PC-01：写快照（原子；无 .tmp 残留）----
    AutosaveInfo info;
    const bool   wrote = autosave_write(graph, workflow_file.string(), kAutosaveKeepDefault, &info,
                                        &error, dir);
    counter.check(wrote && std::filesystem::exists(layout.latest), "写：快照落盘（父目录自动创建）",
                  error);
    counter.check(!std::filesystem::exists(layout.temp), "写：无残留 .tmp（rename 原子替换完成）");
    counter.check(info.ok && info.nodes == 2 && info.edges == 1 &&
                      info.workflow_path == workflow_file.string(),
                  "写：元信息回填（节点 2 / 连线 1 / 关联路径）");
    counter.check(info.saved_at.size() == 19, "写：元信息带本地时间戳（YYYY-MM-DD HH:MM:SS）",
                  info.saved_at);

    const AutosaveInfo probed = autosave_probe(dir);
    counter.check(probed.ok && probed.workflow_path == workflow_file.string() && probed.nodes == 2,
                  "读：autosave_probe 读出元信息（不解析整图）", probed.error);

    // ---- ② VC-02：恢复后图内容与快照一致 ----
    Graph      restored;
    const bool loaded = autosave_load(restored, &error, dir);
    counter.check(loaded, "恢复：autosave_load 成功", error);
    counter.check(graph_content_signature(restored) == signature,
                  "恢复：图内容与快照一致（内容指纹逐字相同）");
    {
        const Node*  node  = restored.findNode(input_id);
        const Param* param = (node != nullptr) ? node->findParam("text") : nullptr;
        counter.check(param != nullptr &&
                          param->value == "崩溃恢复自检：这段文字必须能从快照里恢复",
                      "恢复：节点参数值未丢（文本逐字一致）");
    }
    counter.check(restored.viewport_zoom > 1.4f && restored.viewport_zoom < 1.6f,
                  "恢复：视口（平移 / 缩放）一并恢复");
    {
        Graph direct;
        counter.check(load_workflow(layout.latest, direct, &error) &&
                          graph_content_signature(direct) == signature,
                      "兼容：快照本身是可打开的普通工作流 JSON（autosave 节不影响解析）", error);
    }

    // ---- ③ 节流判据：内容指纹只对「真正的变更」敏感 ----
    {
        Graph panned         = graph;
        panned.viewport_x    = graph.viewport_x + 500.0f;
        panned.viewport_zoom = 2.0f;
        counter.check(graph_content_signature(panned) == signature,
                      "指纹：平移 / 缩放不算变更（不会触发写盘）");

        Graph edited = graph;
        if (Node* node = edited.findNode(input_id)) {
            if (Param* param = node->findParam("text")) {
                // 注意：`Param::value` 是 nlohmann::json，**不要写 `value += "..."`**
                // ——json::operator+= 的语义是 push_back（数组追加），对字符串会抛 type_error
                param->value = std::string("改过的文本（内容指纹应随之变化）");
            }
        }
        counter.check(graph_content_signature(edited) != signature, "指纹：参数变化算变更");
    }

    // ---- ④ VC-01：模拟「写到一半进程被杀」----
    {
        std::ofstream half(layout.temp, std::ios::binary | std::ios::trunc);
        half << "{\n  \"version\": \"1.0\",\n  \"nodes\": [ { \"id\": \"n1\"";
        half.close();
    }
    counter.check(std::filesystem::exists(layout.temp), "中断：已构造半截 JSON 的残留 .tmp");
    const AutosaveInfo after_crash = autosave_probe(dir);
    counter.check(after_crash.ok && after_crash.nodes == 2,
                  "中断：恢复来源仍是**完整**快照（永不读 .tmp）", after_crash.error);

    std::string cleanup_error;
    counter.check(autosave_cleanup_temp(&cleanup_error, dir) == 1 &&
                      !std::filesystem::exists(layout.temp),
                  "中断：残留 .tmp 可被清理（返回删除个数）", cleanup_error);
    {
        Graph after_cleanup;
        counter.check(autosave_load(after_cleanup, &error, dir) &&
                          graph_content_signature(after_cleanup) == signature,
                      "中断：清理后快照仍可正常加载（没产生半个 JSON）", error);
    }
    counter.check(autosave_write(graph, workflow_file.string(), kAutosaveKeepDefault, nullptr,
                                 &error, dir) &&
                      !std::filesystem::exists(layout.temp),
                  "中断后重写：成功且不留 .tmp", error);

    // ---- ⑤ 历史轮转（保留最近 N 份）----
    for (int round = 1; round <= 3; ++round) {
        Graph next       = graph;
        next.description = "第 " + std::to_string(round) + " 次";
        autosave_write(next, workflow_file.string(), 2, nullptr, &error, dir);
    }
    counter.check(std::filesystem::exists(layout.history(1)) &&
                      std::filesystem::exists(layout.history(2)) &&
                      !std::filesystem::exists(layout.history(3)),
                  "轮转：keep=2 ⇒ 只有 autosave_1 / autosave_2（最旧的被丢弃）");

    autosave_clear(&error, dir);
    counter.check(!std::filesystem::exists(layout.latest) &&
                      !std::filesystem::exists(layout.history(1)) &&
                      !std::filesystem::exists(layout.history(2)),
                  "清理：autosave_clear 同时删最新与历史（= 已无未保存更改）");

    autosave_write(graph, workflow_file.string(), 0, nullptr, &error, dir);
    counter.check(!std::filesystem::exists(layout.history(1)),
                  "轮转：keep=0 ⇒ 不留历史（只保留 autosave.json）");

    // ---- ⑥ VC-02：恢复判定（4 种场景）----
    std::string reason;
    autosave_clear(&error, dir);
    const AutosaveInfo none = autosave_probe(dir);
    counter.check(!none.ok && !autosave_should_offer_restore(none, &reason),
                  "判定：无快照 ⇒ 不提示（不打扰用户）");

    autosave_write(graph, "", 0, nullptr, &error, dir);
    counter.check(autosave_should_offer_restore(autosave_probe(dir), &reason),
                  "判定：未命名工作流 + 有快照 ⇒ 提示恢复", reason);

    const std::filesystem::path missing = dir / "不存在的工作流.json";
    autosave_write(graph, missing.string(), 0, nullptr, &error, dir);
    counter.check(autosave_should_offer_restore(autosave_probe(dir), &reason),
                  "判定：原工作流文件不存在 ⇒ 提示恢复", reason);

    const auto now = std::filesystem::file_time_type::clock::now();
    save_workflow(graph, workflow_file, &error);
    std::filesystem::last_write_time(workflow_file, now - std::chrono::hours(1), code);
    autosave_write(graph, workflow_file.string(), 0, nullptr, &error, dir);
    counter.check(autosave_should_offer_restore(autosave_probe(dir), &reason),
                  "判定：快照比工作流新 ⇒ 提示恢复", reason);

    std::filesystem::last_write_time(layout.latest, now - std::chrono::hours(2), code);
    std::filesystem::last_write_time(workflow_file, now, code);
    counter.check(!autosave_should_offer_restore(autosave_probe(dir), &reason),
                  "判定：工作流比快照新（已保存）⇒ 不提示", reason);

    // ---- ⑦ 自清理（零残留）----
    std::filesystem::remove_all(dir, code);
    counter.check(!std::filesystem::exists(dir), "自检：临时目录已清理（不碰用户数据）");

    if (passed_out != nullptr) {
        *passed_out = counter.passed;
    }
    std::printf("   自动保存自检结果：%d 通过 / %d 失败\n", counter.passed, counter.failed);
    return counter.failed;
}

} // namespace aiwrite::engine
