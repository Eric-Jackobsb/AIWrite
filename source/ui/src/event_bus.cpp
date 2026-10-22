// ============================================================================
//  事件总线实现（ARCH-E1：EventBus + EditEvent）
//
//  publish 同步分发（UI 线程单线程 FIFO）：拷贝订阅列表后按 kind 匹配分发，
//  回调内 subscribe / unsubscribe 不影响本次分发（下一事件起生效）。
// ============================================================================

#include "event_bus.h"

#include <algorithm>

namespace aiwrite::ui {

EditEvent EditEvent::node(EditEventKind kind, std::string node_id)
{
    EditEvent event;
    event.kind    = kind;
    event.node_id = std::move(node_id);
    return event;
}

EditEvent EditEvent::param(std::string node_id, std::string param_key)
{
    EditEvent event;
    event.kind      = EditEventKind::ParamChanged;
    event.node_id   = std::move(node_id);
    event.param_key = std::move(param_key);
    return event;
}

EditEvent EditEvent::edge(EditEventKind kind, const engine::Edge& edge)
{
    EditEvent event;
    event.kind           = kind;
    event.edge_id        = edge.id;
    event.edge_from_node = edge.from_node;
    event.edge_from_port = edge.from_port;
    event.edge_to_node   = edge.to_node;
    event.edge_to_port   = edge.to_port;
    return event;
}

EditEvent EditEvent::edge(EditEventKind kind, std::string from_node, std::string from_port,
                          std::string to_node, std::string to_port, std::string edge_id)
{
    EditEvent event;
    event.kind           = kind;
    event.edge_from_node = std::move(from_node);
    event.edge_from_port = std::move(from_port);
    event.edge_to_node   = std::move(to_node);
    event.edge_to_port   = std::move(to_port);
    event.edge_id        = std::move(edge_id);
    return event;
}

std::size_t EventBus::subscribe(EditEventKind kind, EditCallback callback)
{
    subscribers_.push_back(Subscriber{next_handle_, kind, std::move(callback)});
    return next_handle_++;
}

void EventBus::unsubscribe(std::size_t handle)
{
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                       [handle](const Subscriber& sub) { return sub.handle == handle; }),
        subscribers_.end());
}

void EventBus::publish(const EditEvent& event)
{
    // 单线程 FIFO：按订阅顺序分发；拷贝列表防回调内 subscribe / unsubscribe 影响本次分发
    const std::vector<Subscriber> snapshot = subscribers_;
    for (const Subscriber& sub : snapshot) {
        if (sub.kind == event.kind) {
            sub.callback(event);
        }
    }
}

std::size_t EventBus::subscriberCount(EditEventKind kind) const
{
    return static_cast<std::size_t>(std::count_if(
        subscribers_.begin(), subscribers_.end(),
        [kind](const Subscriber& sub) { return sub.kind == kind; }));
}

std::size_t EventBus::subscriberCount() const { return subscribers_.size(); }

} // namespace aiwrite::ui
