#pragma once

// ============================================================================
//  事件总线（ARCH-E1：EventBus + EditEvent 落地 · 阶段 2 编辑侧事件化）
//
//  架构落点：ARCHITECTURE.md §1.3 通信层 / §1.8.1 `EventBus` / 决策 ARCH-D7 / D12
//   * 形态 1：通知（规则 4）—— 生产者只发事件，不关心谁消费；消费者各自订阅处理
//   * 渲染 / 读取仍直接读核心状态（规则 3），事件只用于「变化通知」，不携带数据本身
//   * 单线程 FIFO：publish 同步按订阅顺序分发（UI 线程），不跨线程、不存历史、不合并
//   * 本批落地 EditEvent（8 种，ARCH-D12）；RunEvent 仍走 RunSession 现有队列（ARCH-D10）
//
//  E1 范围（事件系统就绪）：EventBus + EditEvent 类型 + 编辑操作发布点 + 单元断言；
//  E2（本批）：Undo / autosave 改为订阅 —— Undo 订阅「修改前」通道 publish_before_edit
//    压快照（快照式撤销需修改前状态，而 EditEvent 在修改后发布）；autosave 订阅内容类
//    EditEvent 替代每帧 content signature 轮询；RunSession 感知留 ARCH-D22 / E2 后。
//  E3（本批）：EventBus 已随 Graph + Undo 迁到文档级对象 `WorkflowModel`
//    （editor().model().event_bus()；§1.8.11 依赖图：WorkflowModel 依赖 EventBus）。
// ============================================================================

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "graph.h"   // engine::Edge（连线事件负载）

namespace aiwrite::ui {

// 8 种编辑事件类型（ARCH-D12）
enum class EditEventKind {
    NodeAdded,         // 节点创建
    NodeRemoved,       // 节点删除（连带其连线由 NodeRemoved 隐含）
    NodeMoved,         // 节点位置变化（拖拽）
    EdgeAdded,         // 连线创建
    EdgeRemoved,       // 连线删除
    ParamChanged,      // 参数值变化
    TitleChanged,      // 标题变化
    SelectionChanged,  // 选中变化
    Count              // 哨兵：类型总数（断言用 = 8）
};

// 编辑事件（通知形态：只携带最小标识负载；具体数据消费者直接读核心状态）
struct EditEvent {
    EditEventKind kind = EditEventKind::Count;
    // 节点类事件负载（NodeAdded / NodeRemoved / NodeMoved / ParamChanged / TitleChanged）
    std::string node_id;
    // 参数事件负载（ParamChanged；未跟踪到具体 key 时留空 = 通知「有参数变了」）
    std::string param_key;
    // 连线事件负载（EdgeAdded / EdgeRemoved）
    std::string edge_id;
    std::string edge_from_node;
    std::string edge_from_port;
    std::string edge_to_node;
    std::string edge_to_port;

    // 静态工厂（发布点免手动填全部字段）
    static EditEvent node(EditEventKind kind, std::string node_id);
    static EditEvent param(std::string node_id, std::string param_key);
    static EditEvent edge(EditEventKind kind, const engine::Edge& edge);
    static EditEvent edge(EditEventKind kind, std::string from_node, std::string from_port,
                          std::string to_node, std::string to_port, std::string edge_id);
};

// 事件总线（单线程 FIFO 通知总线；订阅者按类型分组，按订阅顺序分发）
class EventBus {
public:
    using EditCallback = std::function<void(const EditEvent&)>;
    // ARCH-E2：修改前通道（Undo 订阅化）—— 快照式撤销需要「修改前」状态，而 EditEvent
    // 在修改**后**发布；总线另提供「修改前」通道：编辑操作在修改前调用
    // publish_before_edit(label)，Undo 订阅此通道压快照；修改完成后照常 publish(EditEvent)
    // 通知其他消费者（autosave / UI）。
    using BeforeEditCallback = std::function<void(const std::string& label)>;

    // 订阅某种编辑事件；返回句柄 id（>0）。重复订阅同一类型 = 追加（FIFO 顺序）。
    std::size_t subscribe(EditEventKind kind, EditCallback callback);
    // 按句柄注销；不存在的句柄为空操作。
    void unsubscribe(std::size_t handle);
    // 发布：同步分发给该类型全部订阅者（拷贝列表防回调改容器）。
    void publish(const EditEvent& event);

    // ARCH-E2：修改前通道 —— 订阅 / 注销 / 发布（单线程 FIFO，与 publish 同构）
    std::size_t subscribe_before_edit(BeforeEditCallback callback);
    void        unsubscribe_before_edit(std::size_t handle);
    void        publish_before_edit(const std::string& label);

    // 自检辅助（--run-selftest 单元断言用）
    std::size_t subscriberCount(EditEventKind kind) const;
    std::size_t subscriberCount() const;
    std::size_t before_subscriber_count() const;

private:
    struct Subscriber {
        std::size_t   handle;
        EditEventKind kind;
        EditCallback  callback;
    };
    struct BeforeSubscriber {
        std::size_t        handle;
        BeforeEditCallback callback;
    };
    std::vector<Subscriber>       subscribers_;        // 按订阅顺序（FIFO）
    std::vector<BeforeSubscriber> before_subscribers_; // 修改前通道（FIFO；Undo 订阅）
    std::size_t                   next_handle_         = 1;
    std::size_t                   before_next_handle_  = 1;
};

} // namespace aiwrite::ui
