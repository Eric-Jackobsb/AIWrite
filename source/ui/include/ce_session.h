#pragma once

// ============================================================================
//  上级节点编辑会话（ARCH-Z2：`CeSession` 对称拆分 · ARCHITECTURE.md §1.8 / ARCH-D3）
//
//  主编辑器侧对象 ↔ ce 侧对象（对称拆分的四个落点）：
//    WorkflowModel（Graph + Undo + EventBus）  ↔ CeModel（CompositeSpec 工作副本 + 撤销栈 + dirty）
//    WorkflowFile （打开 / 保存 / 最近列表）    ↔ CeFile  （打开 / 保存 / 校验 + 注册表 + 热重载）
//    SelectionState（选中集合）                ↔ CeSelection（内部图选中；单选给参数编辑区）
//    ——（主画布无对应：规格标记是 ce 独有域）  ↔ CeSpec  （对外输入/输出/暴露参数 + 显示名缓冲）
//
//  * `CeSession` = 会话级门面（对称 `EditorController` 的编排 + `RunSession` 的会话部分）：
//    会话开关 / 错误文案 / 名称·描述缓冲 / 试运行执行器与读模型 + 上面 4 个对象。
//  * 渲染只读 `CeReadOnlyView`（对称 `ReadOnlyView` · §4 判据 7）：每帧构建一次、只存引用、
//    零拷贝、不跨帧存活；渲染代码只经视图读，写路径经 `ceSession()` 的编辑 API。
//  * 规则 6（§1.1）：全部跨帧可变状态都是对象成员 —— 无文件级 static、无跨 TU extern，
//    也无「散落在函数里的 static 缓冲」（原 `ce_name_buffers()` 已收进 `CeSpec`）。
// ============================================================================

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "composite.h" // engine::CompositeSpec / CompositePortRef / CompositeParamRef
#include "config.h"    // Config（渲染视图字段）
#include "executor.h"  // engine::Executor / RunSnapshot（会话级试运行）
#include "graph.h"     // engine::Graph

namespace aiwrite::ui {

class EditorController; // ARCH-A3：主画布编排层（启动注入；本模块不拥有）

// ---------------------------------------------------------------------------
//  CeModel（文档级 · 对称 `WorkflowModel`）
//  * 持有规格工作副本（`spec()`；内部图 = `graph()`）与撤销栈
//  * 撤销粒度 = **整份 CompositeSpec 快照**（含对外输入/输出/暴露参数；主画布是整张 Graph），
//    深度 30；`engine::UndoStack` 固定绑 `Graph` 且深 50，故本对象自带同角色小栈
//  * 不保存文件（→ CeFile）、不做规格标记编辑（→ CeSpec）、不渲染
// ---------------------------------------------------------------------------
class CeModel {
public:
    engine::CompositeSpec&       spec() { return spec_; }
    const engine::CompositeSpec& spec() const { return spec_; }
    engine::Graph&               graph() { return spec_.internal_graph; }
    const engine::Graph&         graph() const { return spec_.internal_graph; }

    // 修改前压快照（对称 `WorkflowModel::snapshot`）：压栈 + 丢重做分支 + 置脏
    void pushSnapshot(const std::string& label);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undoOnce(); // 无快照返回 false（error 清理由调用方负责，同原语义）
    bool redoOnce();
    void clearHistory(); // 打开 / 关闭会话

    std::size_t undoDepth() const { return undo_.size(); }
    std::size_t redoDepth() const { return redo_.size(); }

    // 未保存更改（顶栏「● 有未保存更改」/ 关闭确认）
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markSaved() { dirty_ = false; }

    // 整份替换（打开已有规格 / 新建空白草稿）；不碰撤销栈与 dirty（由 CeSession 编排）
    void load(engine::CompositeSpec spec);

private:
    struct Entry {
        engine::CompositeSpec spec;
        std::string           label;
    };

    engine::CompositeSpec spec_;  // 规格工作副本（唯一真相；保存时才写注册表）
    std::vector<Entry>    undo_;  // 撤销栈（深 30）
    std::vector<Entry>    redo_;  // 重做栈
    bool                  dirty_ = false;
};

// ---------------------------------------------------------------------------
//  CeSelection（文档级 · 对称 `SelectionState`）
//  * 只存内部图选中 id（ce 画布为「局部单选」语义：多选/无选 → `single()` 为空）
//  * 每帧由画布交互层（`interactorSyncSelection` → `on_selection`）同步，变化才写
//  * 不持有图、不渲染
// ---------------------------------------------------------------------------
class CeSelection {
public:
    const std::vector<std::string>& nodes() const { return nodes_; }
    const std::string&              single() const { return single_; } // 单选节点 id（参数编辑区）
    bool                            empty() const { return nodes_.empty(); }
    bool is_selected(const std::string& node_id) const;

    // 画布同步（每帧调用安全：内置变化检测）
    void sync(const std::vector<std::string>& node_ids);
    void clear();

private:
    std::vector<std::string> nodes_;
    std::string              single_;
};

// ---------------------------------------------------------------------------
//  CeSpec（ce 独有域 · 规格标记）
//  * 对外输入 / 对外输出（唯一）/ 暴露参数（param_id 全局唯一）三处标记的查询与增删
//  * 外部显示名编辑缓冲（key = 节点id|端口id|in|out）—— 原 `ce_name_buffers()` 函数内 static
//  * 不压快照、不置脏（调用方先 `snapshot()` 再调本对象，与原行为一致）
// ---------------------------------------------------------------------------
class CeSpec {
public:
    explicit CeSpec(CeModel& model) : model_(model) {}

    // ---- 查询（渲染只读）----
    bool has_input(const std::string& node_id, const std::string& port_id) const;
    bool has_output(const std::string& node_id, const std::string& port_id) const;
    bool has_exposed(const std::string& node_id, const std::string& param_id) const;
    // 暴露参数 id 冲突：同一 param_id 已被**其它**内部节点暴露（外部实例参数 id 会撞车）
    bool param_id_conflict(const std::string& node_id, const std::string& param_id) const;

    // ---- 修改（写路径）----
    void toggle_input(const std::string& node_id, const std::string& port_id, bool expose);
    void set_output(const std::string& node_id, const std::string& port_id); // 单选：清旧再置新
    void toggle_exposed(const std::string& node_id, const std::string& param_id, bool expose);
    // 内部节点删除后的引用清理（输入 / 输出 / 暴露参数三处残留）
    void drop_node_refs(const std::string& node_id);
    void clear();

    // ---- 外部显示名 ----
    std::array<char, 128>& name_buffer(const std::string& key, const std::string& initial);
    void                   reset_name_buffers() { name_bufs_.clear(); }
    // 显示名写回（输入 / 输出端口 ref）；找不到对应引用返回 false
    bool set_input_display_name(const std::string& node_id, const std::string& port_id,
                                const std::string& name);
    bool set_output_display_name(const std::string& node_id, const std::string& port_id,
                                 const std::string& name);

private:
    CeModel&                                     model_;
    std::map<std::string, std::array<char, 128>> name_bufs_;
};

// ---------------------------------------------------------------------------
//  CeFile（文件级 · 对称 `WorkflowFile`）
//  * 打开：注册表 → 工作副本（内置预设只读拦截；规格缺失 → 空白草稿，保持原行为）
//  * 保存：名称/描述同步 → 校验 + 写注册表 + 落盘 → 主画布同 id 实例热重载
//  * 不持会话开关 / 不渲染
// ---------------------------------------------------------------------------
class CeFile {
public:
    explicit CeFile(CeModel& model) : model_(model) {}

    bool open(const std::string& composite_id, std::string* error);
    bool save(const std::string& name, const std::string& description, std::string* error,
              EditorController& main_editor);

private:
    CeModel& model_;
};

// ---------------------------------------------------------------------------
//  CeSession（会话级门面）
//  * 原 `CompositeEditorState` 的字段按归属下沉：会话状态 → 本对象；工作副本 + 撤销 → CeModel；
//    规格标记 → CeSpec；内部图选中 → CeSelection；打开/保存 → CeFile
//  * 唯一实例 = `ceSession()`（定义于 ce_session.cpp；函数内 static 承载对象，规则 6）
// ---------------------------------------------------------------------------
class CeSession {
public:
    // ---- 会话级状态（原 `CompositeEditorState` 直落成员）----
    bool                  active    = false; // 编辑窗口是否打开
    bool                  ask_close = false; // 关闭确认弹窗
    std::string           error;             // 上次操作错误（展示用）
    std::array<char, 256> name_buf{};        // 名称输入缓冲
    std::array<char, 256> desc_buf{};        // 描述输入缓冲

    // ---- 试运行「执行连线」（沙箱副本；运行态值不进规格 / 撤销快照 / JSON）----
    engine::Executor    run_executor;
    engine::RunSnapshot run_snapshot;
    bool                run_active = false;
    std::string         run_status;

    // ---- 四个对象（拆分后各归其位）----
    CeModel&           model() { return model_; }
    const CeModel&     model() const { return model_; }
    CeFile&            file() { return file_; }
    CeSelection&       selection() { return selection_; }
    const CeSelection& selection() const { return selection_; }
    CeSpec&            spec_marks() { return spec_; }
    const CeSpec&      spec_marks() const { return spec_; }

    // ---- 会话编排 ----
    void open(const std::string& composite_id);
    void requestClose();
    void forceClose();
    bool save(std::string* error_out, EditorController& main_editor);
    // 修改前压快照（转发 CeModel；供 ce_state / ce_draw 调用）
    void snapshot(const std::string& label) { model_.pushSnapshot(label); }
    // 名称 / 描述缓冲 → 工作副本（每帧同步；与原 `state.working.name = state.name_buf.data()` 等价）
    void syncNameBuffers();

private:
    CeModel     model_;
    CeFile      file_{model_};
    CeSelection selection_;
    CeSpec      spec_{model_};
};

CeSession& ceSession();

// ---------------------------------------------------------------------------
//  CeReadOnlyView（每帧构建 · 只存引用 · 零拷贝 · §4 判据 7）
//  * 构建者：`app.cpp` 主循环在调用 `drawCompositeEditor` 前构建一次
//  * 字段 = 三个核心 + 配置（工作流数据 / 选中 / 运行快照 / 配置），对称 `ReadOnlyView`
//  * 工作流数据形态 = 接口收敛：访问器返回内部图数组的 const 引用（不拷贝）
//  * 快照为空 = 空对象（`RunSnapshot::running = false` 即未运行）
// ---------------------------------------------------------------------------
struct CeReadOnlyView {
    // ---- 工作流数据（零拷贝）----
    const std::vector<engine::Node>& nodes_for_render() const { return graph_.nodes; }
    const std::vector<engine::Edge>& edges_for_render() const { return graph_.edges; }
    const engine::Node* find_node(const std::string& node_id) const { return graph_.findNode(node_id); }
    const engine::Edge* find_edge(const std::string& edge_id) const { return graph_.findEdge(edge_id); }
    // 过渡访问器：绘制层整图只读查询（findEdgeIntoInput / 对齐吸附 / 小地图）
    const engine::Graph& graph() const { return graph_; }

    // ---- 顶部展示（只读快照字段）----
    const std::string& id() const { return model_.spec().id; }
    const std::string& name() const { return model_.spec().name; }
    bool               dirty() const { return model_.dirty(); }

    // ---- 三个核心 + 配置 ----
    const CeSelection&         selection; // 内部图选中（只读）
    const engine::RunSnapshot& snapshot;  // 试运行读模型（空对象 = 未运行）
    const Config&              config;    // 应用配置

    CeReadOnlyView(const CeModel& model, const CeSelection& sel, const engine::RunSnapshot& snap,
                   const Config& cfg)
        : selection(sel), snapshot(snap), config(cfg), graph_(model.graph()), model_(model) {}

private:
    const engine::Graph& graph_; // 唯一持有的数据引用（零拷贝）
    const CeModel&       model_; // 顶栏 id / name / dirty 的只读来源
};

} // namespace aiwrite::ui
