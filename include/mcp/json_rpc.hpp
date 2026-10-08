#pragma once

#include <array>
#include <string>

#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/// 本实现支持的最新协议版本：客户端提的版本我们都不支持时，回落成这个。
inline constexpr const char* kProtocolVersion = "2025-11-25";

/// 本实现能接受的版本（从新到旧）。必须与真实客户端支持的集合有交集，
/// 否则握不上手。CodeBuddy 的 MCP 客户端用的是官方 TS SDK，其集合为：
///   2025-11-25 / 2025-06-18 / 2025-03-26 / 2024-11-05 / 2024-10-07
/// 注意别自己编版本号：写进去一个客户端不认的日期，症状是握手直接失败，
/// 而且错误信息只出现在客户端面板上，server 侧的日志看不出任何异常。
inline constexpr std::array<const char*, 5> kSupportedProtocolVersions = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", "2024-10-07"};

/// JSON-RPC 2.0 标准错误码，另加 MCP 常用的服务端错误起点。
enum class ErrorCode {
    ParseError = -32700,       // 收到的不是合法 JSON
    InvalidRequest = -32600,   // 是 JSON 但不是合法请求对象
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

/// 成功响应：{"jsonrpc":"2.0","id":<id>,"result":<result>}
json makeResponse(const json& id, const json& result);

/// 错误响应：{"jsonrpc":"2.0","id":<id>,"error":{"code":..,"message":..}}
json makeError(const json& id, ErrorCode code, const std::string& message);

/// 请求是否带 id：没有 id 的是通知(notification)，按规范不应回响应。
bool isNotification(const json& request);

/// 协议版本协商：客户端要的版本我们支持，就原样回它；否则回 kProtocolVersion。
///
/// 为什么必须原样回，而不是一律回自己的最新版：客户端只认自己提过的版本。
/// 客户端请求 2025-03-26、服务端回 2025-11-25，客户端会直接判定握手失败
/// （表现为面板上 "Server's protocol version is not supported: ..."）。
std::string negotiateProtocolVersion(const std::string& requested);

}  // namespace mcp
