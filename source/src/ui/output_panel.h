#pragma once

// ============================================================================
//  输出面板（PA-02：地基补丁 A —— 结果回流）
//
//  * 数据源：Executor::runInfos()（状态/耗时/错误）+ Executor::outputs()（运行态值）
//  * **只读**：不写回 Graph / 参数 / 撤销快照（运行态值原则，M_patchA §0.3）
//  * 由「视图」菜单开关，状态持久化到 config.toml 的 [ui] show_output_window
//  * node_output_text / run_output_text 与自检共用同一实现（避免"面板看到的"与
//    "导出的"不一致）
// ============================================================================

#include <string>

namespace aiwrite::engine {
class Executor;
class Graph;
struct RunSnapshot;
} // namespace aiwrite::engine

namespace aiwrite::ui {

struct EditorState;

// 单节点结果文本（多输出端口按「端口名：值」依次拼接；无结果返回空串）
std::string node_output_text(const engine::Graph& graph, const engine::Executor& executor,
                             const std::string& node_id);

// 全部结果文本（含每节点标题行：id · 类型 · 状态 · 耗时；失败节点带错误行）
// 供「复制全文 / 导出到文件」与无界面自检使用
std::string run_output_text(const engine::Graph& graph, const engine::Executor& executor);

// ---- PB-01：读模型重载（GUI 用；运行中读快照，避免跨线程读 Executor 运行态）----
//  * 快照中的节点文本已由 makeSnapshot 用同一 nodeOutputText 规则落定 → 与上面两个重载逐字符一致
std::string node_output_text(const engine::Graph& graph, const engine::RunSnapshot& snapshot,
                             const std::string& node_id);
std::string run_output_text(const engine::Graph& graph, const engine::RunSnapshot& snapshot);

// 绘制输出面板（可停靠；默认不显示由调用方控制）
void draw_output_panel(const char* title, bool* open, const EditorState& state);

} // namespace aiwrite::ui
