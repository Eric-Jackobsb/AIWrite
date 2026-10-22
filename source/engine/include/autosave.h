#pragma once

// ============================================================================
//  自动保存与崩溃恢复（M_patchAB_rest §4 · `PC-01` / `PC-02`）
//
//  为什么在 engine/ 而不是 ui/：
//   * `ui/**` 属 GUI 可执行目标（`aiwrite`），`api_probe` 链接不到 ⇒ 逻辑放在 ui 里
//     就**无法离线断言**（VC-01 / VC-02 都要求可复现证据）
//   * 本文件只依赖 Graph + workflow_io（`graph_to_json` / `graph_from_json`）+ utils（paths / log）
//     ⇒ 纯函数 + 文件 IO，无 ImGui / 无窗口 / 无全局状态
//
//  落盘布局（`dir` 默认 = `paths::snapshots_dir()` = `~/.brain-ai/snapshots`）：
//    <dir>/autosave.json         最新快照（**唯一**恢复来源）
//    <dir>/autosave_1.json …     历史快照（1 = 次新；份数 = keep；keep = 0 ⇒ 不留）
//    <dir>/autosave.json.tmp     写入中的临时文件（进程被杀时残留 ⇒ 启动清理，不参与恢复）
//
//  原子性（VC-01）：写序列 = ①轮转历史 ②写 `.tmp` 并校验 ③`rename` 覆盖 `autosave.json`。
//   `autosave.json` 只被第 ③ 步「整文件替换」，**任何时刻都是完整 JSON**；中断只会留下
//   `.tmp`（由 `autosave_cleanup_temp()` 清理，且 `autosave_probe()` 永不读它）。
//  历史轮转用**复制旧最新**（而不是先把它改名走），代价是多一次拷贝，换来「不存在
//  autosave.json 缺失窗口」——崩溃恢复场景下这个取舍是刻意的。
// ============================================================================

#include <filesystem>
#include <string>

#include "graph.h"

namespace aiwrite::engine {

// 历史份数默认值（`ui.autosave_keep` 的默认值；界面可改）
inline constexpr int kAutosaveKeepDefault = 3;

// 快照文件路径集合（`dir` 为空 ⇒ `paths::snapshots_dir()`）
struct AutosavePaths {
    std::filesystem::path dir;
    std::filesystem::path latest; // dir/autosave.json
    std::filesystem::path temp;   // dir/autosave.json.tmp

    // dir/autosave_<index>.json（index ≥ 1；1 = 次新）
    std::filesystem::path history(int index) const;
};

AutosavePaths autosavePaths(const std::filesystem::path& dir = {});

// 快照元信息（写盘时写进 JSON 的 `autosave` 节；启动判定只读它，不解析图）
struct AutosaveInfo {
    bool                  ok = false;
    std::filesystem::path file;
    std::string           workflow_path; // 快照所属工作流文件（空 = 未命名）
    std::string           name;          // 工作流名
    std::string           saved_at;      // 本地时间 "YYYY-MM-DD HH:MM:SS"
    int                   nodes = 0;
    int                   edges = 0;
    std::string           error;         // ok = false 时的原因（中文，可直接展示）
};

// 读快照元信息（文件不存在 / 损坏 ⇒ ok = false 并写 error；不抛异常）
AutosaveInfo autosaveProbe(const std::filesystem::path& dir = {});

// 内容指纹：只含 name / description / nodes / edges（**不含 viewport**）
//  * 用途 = 节流判据「工作流是否真的变了」：画布平移/缩放不算变更，不会触发写盘
//  * 同一图 ⇒ 同串；改任一节点参数/坐标/连线/标题 ⇒ 串变
std::string graphContentSignature(const Graph& graph);

// 写快照（原子；成功后按 keep 轮转历史）。失败不影响已有 `autosave.json`。
bool autosave_write(const Graph& graph, const std::string& workflow_path, int keep,
                    AutosaveInfo* info, std::string* error,
                    const std::filesystem::path& dir = {});

// 载入快照到图（复用 workflow_io；失败保持原图内容不定义 ⇒ 调用方应保持原图）
bool autosaveLoad(Graph& graph, std::string* error, const std::filesystem::path& dir = {});

// 删除快照（最新 + 全部历史；语义 = 「已无未保存更改」）
bool autosaveClear(std::string* error, const std::filesystem::path& dir = {});

// 清理残留 `.tmp`（启动时调用；VC-01）。返回删除个数。
int autosave_cleanup_temp(std::string* error = nullptr,
                          const std::filesystem::path& dir = {});

// PC-02 判据：是否需要提示「恢复未保存的更改？」
//  * 有快照 **且**（快照未关联工作流文件 / 关联文件不存在 / 快照比该文件新）⇒ true
//  * 关联文件与快照同样新或更新（= 已保存）⇒ false（不打扰）
//  * 快照不存在 / 损坏 ⇒ false（由 `autosave_probe` 的 error 说明原因）
bool autosaveShouldOfferRestore(const AutosaveInfo& info, std::string* reason);

// VC-01 / VC-02 离线自检（**全程在系统临时目录内构造场景，不碰用户数据**；
// 结束时删除自检目录）。返回失败项数，通过数写 *passed_out。
int autosaveSelftest(int* passed_out);

} // namespace aiwrite::engine
