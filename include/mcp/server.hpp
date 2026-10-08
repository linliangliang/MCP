#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/json_rpc.hpp"

namespace mcp {

/// 工具的执行结果。
///
/// 注意 isError 的语义：它是“工具跑完了，但业务上失败了”，
/// 文本依然会作为 content 返回给模型，让模型能自我纠正。
/// 这与“协议层错误”（JSON-RPC error，比如 method 不存在）是两回事。
struct ToolResult {
    std::string text;
    bool isError = false;
};

using ToolHandler = std::function<ToolResult(const json& args)>;

/// 一个工具的定义。inputSchema 是给模型看的 JSON Schema。
struct ToolDef {
    std::string name;
    std::string description;
    json inputSchema;
    ToolHandler handler;
};

/// 工具注册表：负责持有工具定义并按名查找。
class ToolRegistry {
public:
    void add(ToolDef tool);
    const ToolDef* find(const std::string& name) const;
    std::vector<ToolDef> list() const;

private:
    std::vector<ToolDef> tools_;
};

/// MCP 请求分派入口。返回 nullptr 表示这是通知、不应发送任何响应。
/// 调用方只需把返回的 JSON 原样写成一行即可。
std::optional<json> handleRequest(const ToolRegistry& registry, const json& request);

/// 构造 capabilities + serverInfo，供 initialize 响应使用。
json serverInfo();

}  // namespace mcp
