// JSON-RPC 报文构造 + MCP method 分派。
//
// 三类“失败”要区分清楚，这是最容易写错的地方：
//   1. 报文不是合法 JSON            → main 里拦下，回 -32700（ JSON parse error）
//   2. method 不认识 / 请求不合法     → JSON-RPC error(-32601 / -32600)
//   3. 工具本身执行失败               → 正常响应，result.content + isError:true
//      第 3 类不是协议错误，必须把失败原因作为文本交给模型，模型才有机会纠正。

#include "mcp/server.hpp"

#include <iostream>

namespace mcp {

// ---------------------------------------------------------------- json_rpc

json makeResponse(const json& id, const json& result) {
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response;
}

json makeError(const json& id, ErrorCode code, const std::string& message) {
    json error = json::object();
    error["code"] = static_cast<int>(code);
    error["message"] = message;

    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error;
    return response;
}

bool isNotification(const json& request) {
    return !request.contains("id");
}

json serverInfo() {
    json info = json::object();
    info["name"] = "weather-mcp";
    info["version"] = "0.1.0";
    return info;
}

std::string negotiateProtocolVersion(const std::string& requested) {
    for (const char* supported : kSupportedProtocolVersions) {
        if (requested == supported) {
            return requested;
        }
    }
    if (!requested.empty()) {
        std::cerr << "[mcp] 客户端请求的协议版本不支持: " << requested << "，回落到 " << kProtocolVersion
                  << std::endl;
    }
    return kProtocolVersion;
}

// ------------------------------------------------------------ ToolRegistry

void ToolRegistry::add(ToolDef tool) {
    tools_.push_back(std::move(tool));
}

const ToolDef* ToolRegistry::find(const std::string& name) const {
    for (const auto& tool : tools_) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

std::vector<ToolDef> ToolRegistry::list() const {
    return tools_;
}

// ------------------------------------------------------------ handleRequest

namespace {

json textContent(const std::string& text) {
    json item = json::object();
    item["type"] = "text";
    item["text"] = text;
    json content = json::array();
    content.push_back(item);
    return content;
}

/// 把 ToolResult 包成 MCP 规定的 tools/call 结果:
/// {"content":[{"type":"text","text":...}],"isError":<bool>}
json toCallResult(const ToolResult& toolResult) {
    json result = json::object();
    result["content"] = textContent(toolResult.text);
    result["isError"] = toolResult.isError;
    return result;
}

}  // namespace

std::optional<json> handleRequest(const ToolRegistry& registry, const json& request) {
    if (!request.is_object()) {
        return makeError(nullptr, ErrorCode::InvalidRequest, "request must be a JSON object");
    }

    const json id = request.contains("id") ? request.at("id") : json(nullptr);
    const std::string method = request.value("method", "");

    // 通知(没有 id)：按 JSON-RPC 规范不回任何响应。
    // notifications/initialized 就走这条路，千万别回包，否则有些客户端会报错。
    if (isNotification(request)) {
        std::cerr << "[mcp] <- " << method << " (notification, no response)" << std::endl;
        return std::nullopt;
    }

    std::cerr << "[mcp] <- " << method << std::endl;

    if (method == "initialize") {
        const json params = request.contains("params") && request.at("params").is_object()
                                ? request.at("params")
                                : json::object();
        const std::string requested = params.value("protocolVersion", "");
        const std::string negotiated = negotiateProtocolVersion(requested);
        std::cerr << "[mcp] initialize: requested=" << (requested.empty() ? "(未指定)" : requested)
                  << " -> negotiated=" << negotiated << std::endl;

        json capabilities = json::object();
        capabilities["tools"] = json::object();  // 只声明 tools，暂不支持 resources/prompts

        json result = json::object();
        result["protocolVersion"] = negotiated;
        result["capabilities"] = capabilities;
        result["serverInfo"] = serverInfo();
        return makeResponse(id, result);
    }

    if (method == "ping") {
        return makeResponse(id, json::object());
    }

    if (method == "tools/list") {
        json items = json::array();
        for (const auto& tool : registry.list()) {
            json item = json::object();
            item["name"] = tool.name;
            item["description"] = tool.description;
            item["inputSchema"] = tool.inputSchema;
            items.push_back(item);
        }
        json result = json::object();
        result["tools"] = items;
        return makeResponse(id, result);
    }

    if (method == "tools/call") {
        const json params = request.contains("params") && request.at("params").is_object()
                                ? request.at("params")
                                : json::object();
        const std::string name = params.value("name", "");
        const json args = params.contains("arguments") && params.at("arguments").is_object()
                              ? params.at("arguments")
                              : json::object();

        const ToolDef* tool = registry.find(name);
        if (tool == nullptr) {
            return makeError(id, ErrorCode::InvalidParams, "unknown tool: " + name);
        }

        // 工具内部任何异常都不能让进程退出——进程一挂，整个 MCP 会话就断了。
        try {
            return makeResponse(id, toCallResult(tool->handler(args)));
        } catch (const std::exception& e) {
            ToolResult failure;
            failure.text = std::string("工具内部错误: ") + e.what();
            failure.isError = true;
            return makeResponse(id, toCallResult(failure));
        }
    }

    return makeError(id, ErrorCode::MethodNotFound, "method not found: " + method);
}

}  // namespace mcp
