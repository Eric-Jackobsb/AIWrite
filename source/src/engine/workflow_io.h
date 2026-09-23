#pragma once

// ============================================================================
//  工作流序列化（设计文档 §4.7 工作流 JSON Schema）
//
//  * JSON schema：
//      { "version": "1.0", "name": "...", "description": "...",
//        "nodes": [ { "id","type","title","position":{"x","y"},"params":{...} } ],
//        "edges": [ { "id","from":{"node","port"},"to":{"node","port"} } ],
//        "viewport": { "x","y","zoom" } }
//  * 载入时以节点注册表为准重建 Node（端口/参数定义取注册表，参数值取 JSON，
//    缺失的参数保持默认值），并对坐标做合法性夹紧，避免损坏文件影响画布。
// ============================================================================

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/graph.h"

namespace aiwrite::engine {

// 版本号（写入 version 字段；读取时按主版本判断兼容性）
// 注意：这里用 constexpr（内部链接）而不是 inline constexpr —— 避免 MSVC 链接期
// 对 inline 变量做 COMDAT 合并时报 LNK1163 invalid selection for COMDAT section。
constexpr const char* kWorkflowVersion = "1.0";

// 图 → JSON
nlohmann::json graph_to_json(const Graph& graph);

// JSON → 图；成功返回 true，失败写 error（失败时 graph 内容未定义，调用方应保持原图）
bool graph_from_json(const nlohmann::json& json, Graph& graph, std::string* error);

// 文件读写（UTF-8）
bool save_workflow(const Graph& graph, const std::filesystem::path& file, std::string* error);
bool load_workflow(const std::filesystem::path& file, Graph& graph, std::string* error);

} // namespace aiwrite::engine
