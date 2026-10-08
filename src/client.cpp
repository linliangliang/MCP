// MCP 客户端的协议层：发请求、等响应、区分"协议错误"和"工具执行失败"。
//
// 与 server 那边的三类失败对应：
//   1. 进程/管道层面的失败（启动不了、读不到响应）→ connect() 返回 false 或抛异常
//   2. JSON-RPC error（method 不存在、参数不合法）    → 抛异常，带上 code + message
//   3. 工具执行失败（城市找不到）                    → 正常返回 result，由调用方看 isError

#include "mcp/client.hpp"

#include <stdexcept>

#include "mcp/process.hpp"

namespace mcp {

namespace {

bool isSupported(const std::string& version) {
    for (const char* candidate : kSupportedProtocolVersions) {
        if (version == candidate) {
            return true;
        }
    }
    return false;
}

json clientInfo() {
    json info = json::object();
    info["name"] = "mcp-client";
    info["version"] = "0.1.0";
    return info;
}

}  // namespace

McpClient::McpClient() = default;

McpClient::~McpClient() {
    shutdown();
}

void McpClient::setTimeoutMs(int timeoutMs) {
    timeoutMs_ = timeoutMs;
}

bool McpClient::connect(const std::string& command, std::string& error) {
    process_ = std::make_unique<ChildProcess>();
    if (!process_->start(command)) {
        error = process_->lastError();
        process_.reset();
        return false;
    }
    return true;
}

json McpClient::request(const std::string& method, const json& params) {
    if (!process_) {
        throw std::runtime_error("还没有连上 server");
    }

    json request = json::object();
    request["jsonrpc"] = "2.0";
    request["id"] = nextId_++;
    request["method"] = method;
    if (!params.is_null()) {
        request["params"] = params;
    }

    if (!process_->writeLine(request.dump())) {
        throw std::runtime_error("写入 server 失败：" + process_->lastError());
    }

    std::string line;
    if (!process_->readLine(line, timeoutMs_)) {
        throw std::runtime_error("等待 " + method + " 的响应失败：" + process_->lastError());
    }

    const json response = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (response.is_discarded()) {
        throw std::runtime_error("server 返回的不是合法 JSON：" + line.substr(0, 200));
    }
    if (!response.contains("id") || response.at("id") != request.at("id")) {
        throw std::runtime_error(method + " 的响应 id 对不上（期望 " +
                                 request.at("id").dump() + "，实际 " +
                                 response.value("id", json(nullptr)).dump() + "）");
    }
    if (response.contains("error")) {
        const json& error = response.at("error");
        throw std::runtime_error(method + " 返回错误 [" +
                                 std::to_string(error.value("code", 0)) + "] " +
                                 error.value("message", ""));
    }
    return response.value("result", json::object());
}

McpClient::ServerInfo McpClient::initialize() {
    json params = json::object();
    // 报我们支持的最新版本；server 若支持就原样回它，不支持则回它自己的版本。
    params["protocolVersion"] = kProtocolVersion;
    params["capabilities"] = json::object();
    params["clientInfo"] = clientInfo();

    const json result = request("initialize", params);

    ServerInfo info;
    info.protocolVersion = result.value("protocolVersion", "");
    info.serverName = result.contains("serverInfo") ? result.at("serverInfo").value("name", "")
                                                   : std::string();
    info.serverVersion = result.contains("serverInfo")
                             ? result.at("serverInfo").value("version", "")
                             : std::string();

    // 客户端这一侧的对称检查：server 回了一个我们不认的版本，后面必然各种对不上，
    // 与其连上了再出怪问题，不如握手时就说清楚。
    if (!isSupported(info.protocolVersion)) {
        throw std::runtime_error("版本协商失败：server 返回 " +
                                 (info.protocolVersion.empty() ? "(空)" : info.protocolVersion) +
                                 "，客户端支持的最新版本是 " + kProtocolVersion);
    }
    return info;
}

void McpClient::notifyInitialized() {
    if (!process_) {
        throw std::runtime_error("还没有连上 server");
    }
    json notification = json::object();
    notification["jsonrpc"] = "2.0";
    notification["method"] = "notifications/initialized";
    // 通知没有 id，按规范不发也不读响应。
    if (!process_->writeLine(notification.dump())) {
        throw std::runtime_error("发送 notifications/initialized 失败：" + process_->lastError());
    }
}

json McpClient::listTools() {
    return request("tools/list", json::object());
}

json McpClient::callTool(const std::string& name, const json& arguments) {
    json params = json::object();
    params["name"] = name;
    params["arguments"] = arguments;
    return request("tools/call", params);
}

std::string McpClient::toolResultText(const json& result) {
    if (!result.contains("content") || !result.at("content").is_array()) {
        return {};
    }
    std::string text;
    for (const auto& item : result.at("content")) {
        if (item.value("type", "") == "text") {
            if (!text.empty()) {
                text += "\n";
            }
            text += item.value("text", "");
        }
    }
    return text;
}

void McpClient::shutdown() {
    if (!process_) {
        return;
    }
    process_->closeInput();
    process_->wait(3000);
    process_.reset();
}

}  // namespace mcp
