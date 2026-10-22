#pragma once

// ============================================================================
//  节点注册表（设计文档 §4.3 分类 / §4.4 九个节点 / §4.5 端口类型 / §9.2 实现）
//
//  * Definition 描述一个节点类型：分类、输入输出端口、参数（含默认值/范围/枚举）
//  * registerAllNodes() 注册 MVP 的 8 个节点；幂等，可重复调用
//  * Graph::addNode() 依据本表创建节点实例
// ============================================================================

#include <string>
#include <vector>

#include "graph.h"

namespace aiwrite::engine {

// 设计 §4.3 节点分类
//  * M_node_expansion L2（v5 定稿 §4.3.2）：新增顶层「辅助功能」—— 6 个文本处理节点
//    落进 Auxiliary；子分类「文本处理（TextProcess）」为**伪两层，只改 UI 渲染**，不进本枚举
enum class NodeCategory { Input, Process, Config, Inference, Output, Auxiliary };

const char* categoryName(NodeCategory category);        // "输入" / "处理" / ...
const char* categoryKey(NodeCategory category);         // "input" / "process" / ...（供序列化）

struct Definition {
    std::string       type;          // 如 "TextInput"（设计 §4.7 的 type 字段）
    std::string       display_name;  // 如 "文本输入"
    std::string       title;         // 新建节点的默认标题
    NodeCategory      category = NodeCategory::Input;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<Param> params;
    std::string       description;
};

class NodeRegistry {
public:
    static NodeRegistry& instance();

    void registerNode(Definition definition);
    const Definition* find(const std::string& type) const;
    // PB2-24 要点②：**可写**查找（供「配置表重载后重建参数枚举」就地改注册表；
    //  * 只在刷新枚举这类**入口单一**的场合使用；其它地方一律走 `find()` 的只读语义
    Definition* findMutable(const std::string& type);

    // 全部类型（按分类、再按注册顺序）
    std::vector<const Definition*> listTypes() const;
    std::vector<const Definition*> listByCategory(NodeCategory category) const;

    std::size_t size() const;
    void        clear();

private:
    std::vector<Definition> definitions_;
};

// 注册 MVP 的 8 个节点（幂等）
void registerAllNodes();

// ============================================================================
//  PB2-24 要点②：`provider` 参数**动态枚举**（配置表重载后下拉 / 校验即时跟上）
//
//  背景（§3.5 现状证据）：`registerAllNodes()` 幂等 ⇒ 「提供商」枚举在**首次注册时一次性**
//  生成；运行中 `ai::reload_provider_specs()` 只换表，**不会**动注册表里的枚举 ⇒
//  ① 界面下拉走 `ai::provider_specs()` 的实时表（`PB2-31` 合并显示）**已自动刷新**；
//  ② 但**运行前校验**（`Graph::validateParams`）比对的是节点实例里的 `enum_options` 副本 ⇒
//     刚新建、曾未注册过的站点 id 会被误判「取值不在枚举范围内」。
//
//  用法：任何 `ai::reload_provider_specs()` **之后必须刷新下游**（否则新建站点会被运行前校验
//  误判「取值不在枚举范围内」）。两条现行路径（**效果一致**，都在 `--site-assert` 的覆盖范围内）：
//   * 调用方**持有画布**（CLI / 未来菜单命令）⇒ `reload_provider_specs_and_refresh(&graph)`
//     —— 重载 + 两层刷新一次完成（单入口）；
//   * 设置面板**无画布**（`ui/src/settings_panel.cpp`）⇒ 自己 `ai::reload_provider_specs()`
//     读表显示错误 / 警告，再置 `SettingsPanelResult.provider_table_reloaded`
//     ⇒ 宿主 `ui/src/app.cpp` 调 `refresh_provider_options(state.graph)`。
//   * 带 `graph` 的刷新会**保留已选值**（只换选项表）
// ============================================================================

// 当前配置表里的提供商 id（official 在前、web 在后；与 N-05 注册时的口径完全一致）
std::vector<std::string> providerOptionIds();

// 只刷新**注册表 Definition**（之后新建的节点实例用新枚举）
void refreshProviderOptions();

// 刷新注册表 + **已有图**里所有节点实例的 `provider` 枚举（运行前校验因此不再误判）
void refreshProviderOptions(Graph& graph);

// 重载配置表 + 刷新下游（UI / CLI 只需调用这一个；`graph` 可为 nullptr）
//  * 单一入口：避免「谁 reload 谁忘了刷新」——`R8` 的配套要求
void reloadProviderSpecsAndRefresh(Graph* graph = nullptr);

// 只读：当前注册表里 `ProviderConfig.provider` 的枚举选项（自检 / 界面提示用；空 = 未注册）
std::vector<std::string> providerParamOptions();

} // namespace aiwrite::engine
