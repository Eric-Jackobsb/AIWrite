#pragma once

// ============================================================================
//  上级节点（Composition_node / CN）—— 模型 + 注册表 + 独立文件存储
//
//  * CompositeSpec = 一份上级节点规格：暴露输入/输出端口、暴露参数、内部图
//  * 内部图禁止嵌套 Composite（CN-D17）
//  * 独立文件存储：~/.brain-ai/composites/<id>.json（设计文档 §9.3）
//  * 执行时由 Executor 在**工作副本**上就地展开（内部节点成为父图一部分，按帧推进）
//
//  设计要点（Composition_node.md）：
//    * 多输入、强制单输出（CN-D7）—— 校验保证 outputs.size() == 1
//    * 参数选择性暴露（CN-D6）—— exposed_params 只暴露内部参数子集
//    * 上级节点 = 组合层 L4 重启（CN-D20）
//    * 父工作流中的 Composite 节点 = 规格**实例**（端口/参数来自规格，值独立）
// ============================================================================

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph.h"

namespace aiwrite::engine {

// ---------------------------------------------------------- 外部引用 ----
// 暴露端口引用：指向内部图的某节点端口
struct CompositePortRef {
    std::string internal_node;   // 内部节点 id（规格 internal_graph 内）
    std::string internal_port;   // 内部端口 id
    std::string display_name;    // 外部显示名（空 = 用内部端口名）
    PortType    type      = PortType::Any;  // 保存时由内部端口推导并固化
    bool        is_optional = true;         // 外部输入是否可选（默认暴露即可选）
};

// 暴露参数引用：指向内部图的某节点参数
struct CompositeParamRef {
    std::string internal_node;   // 内部节点 id
    std::string param_id;        // 内部参数 id（= 外部实例参数 id）
    std::string display_name;    // 外部显示名（空 = 用内部参数显示名）
};

// -------------------------------------------------------------- 规格 ----
struct CompositeSpec {
    std::string id;                           // 唯一 id（"c1"、"c2"…）
    std::string name;                         // 显示名（新建实例的默认标题）
    std::string description;
    std::vector<CompositePortRef>  inputs;          // 暴露输入（按序；外部端口 id = in1…inN）
    std::vector<CompositePortRef>  outputs;         // 暴露输出（设计：唯一，≤1）
    std::vector<CompositeParamRef> exposed_params;  // 暴露参数（按序；外部参数 id = 内部 param_id）
    Graph internal_graph;                           // 内部图（禁止嵌套 Composite）
};

// 校验规格：id/name 非空、内部图合法、无嵌套、输出唯一、引用端口/参数存在、
// 暴露参数 id 唯一、标记输入端口无内部入边（避免展开后重复占用）
//  * allow_incomplete_draft=true：放行「内部图为空 / 未标记输出」的不完整草稿
//   （仅用于「新建上级节点」的初始空白规格落盘；编辑器「保存」仍走严格校验）
bool validate_composite_spec(const CompositeSpec& spec, std::string* error,
                             bool allow_incomplete_draft = false);

// JSON 序列化 / 反序列化（独立文件 schema）
nlohmann::json composite_to_json(const CompositeSpec& spec);
bool composite_from_json(const nlohmann::json& json, CompositeSpec& spec, std::string* error);

// 依据规格构建 Composite 节点实例（端口/参数按规格；不含连线；值取默认）
//  * 外部输入端口 id = "in1"…"inN"、输出端口 id = "out1"…（稳定，供边引用与序列化）
//  * 外部参数 id = 内部 param_id；实例参数值 = 内部默认值（实例可独立修改）
bool build_composite_node(Node* node, const CompositeSpec& spec, std::string* error);

// 把图中全部 Composite 节点就地展开（供 Executor 在工作副本上调用）
//  * 内部节点以 "<composite节点id>__<内部节点id>" 前缀 id 加入图
//  * 暴露参数值覆盖内部参数；内部边平移；外部输入/输出边重接到内部标记端口
//  * 移除 Composite 占位节点（其运行态由内部节点承载）
//  * replaced_out：composite 节点 id → 其内部新节点 id 列表（重跑定位用；可为 null）
bool expand_composites(Graph& graph,
                       std::unordered_map<std::string, std::vector<std::string>>* replaced_out,
                       std::string* error);

// ------------------------------------------------------------ 注册表 ----
// PS-D9：规格来源（双源）—— 内置只读官方资产 / 用户节点
enum class CompositeSource { None, Builtin, User };

class CompositeRegistry {
public:
    static CompositeRegistry& instance();

    // 双源根目录（默认 paths::builtin_composites_dir() / user_composites_dir()；测试可覆盖）
    //  * set_root = 单源测试模式：内置/用户均指向同一 root（隔离，不触碰用户真实数据）
    void set_root(std::filesystem::path root);
    void set_roots(std::filesystem::path builtin_root, std::filesystem::path user_root);
    const std::filesystem::path& builtin_root() const;
    const std::filesystem::path& user_root() const;
    std::filesystem::path root() const; // 兼容旧调用：返回用户根（写入口）

    // 全量重扫（先内置后用户；用户源覆盖内置源；旧目录首次迁移入用户源）
    void reload();

    // 惰性加载：首次访问自动扫描；规格缺失返回 nullptr
    const CompositeSpec* find(const std::string& id) const;
    std::vector<const CompositeSpec*> list() const;  // 合并后（用户覆盖内置）按 name 排序
    std::size_t size() const { return specs_.size(); }

    // 来源映射：内置项只读（无删除/编辑入口）；用户项可读写
    CompositeSource source_of(const std::string& id) const;
    bool is_builtin(const std::string& id) const { return source_of(id) == CompositeSource::Builtin; }

    // 保存（校验通过才写盘并进内存）；失败返回 false 并写 error
    //  * allow_incomplete_draft=true：跳过「内部图为空 / 未标记输出」校验（新建草稿用）
    //  * 内置源 id 只读：直接 save 被拒（须先 copy_to_user）
    bool save(const CompositeSpec& spec, std::string* error, bool allow_incomplete_draft = false);
    // 删除用户项；内置项被拒；删除后若内置仍有同 id ⇒ 来源回落 Builtin
    bool remove(const std::string& id, std::string* error);
    // 「复制到我的节点库」：把某规格以同 id 写入用户源（用户接管后来源切 User、可编辑）
    bool copy_to_user(const std::string& id, std::string* error);

    // 生成新 id（扫描合并后的全部规格 + 用户目录文件，避免冲突）
    std::string make_id() const;

private:
    CompositeRegistry() = default;

    void ensureLoaded() const;
    void loadFromDir(const std::filesystem::path& dir, CompositeSource source);
    void migrateLegacy();

    mutable std::vector<CompositeSpec> specs_;                 // 合并后规格（用户覆盖内置）
    mutable std::unordered_map<std::string, CompositeSource> source_;
    mutable bool                       loaded_            = false;
    std::filesystem::path              builtin_root_;
    std::filesystem::path              user_root_;
    bool                               roots_overridden_ = false; // 测试注入：跳过旧目录迁移
};

} // namespace aiwrite::engine
