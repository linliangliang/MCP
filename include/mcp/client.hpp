#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "mcp/json_rpc.hpp"
#include "mcp/process.hpp"

namespace mcp {

/// MCP 客户端：拉起 server 子进程，在 stdio 上用换行分隔的 JSON 完成握手与调用。
///
/// 与 server 的分层一一对应：本类只管"协议对话"，
/// 字节读写和进程创建都在 ChildProcess 里，业务逻辑在 tools 里。
///
/// 失败一律抛 std::runtime_error：调用方只有 main，没必要多设计一套错误码。
class McpClient {
public:
    struct ServerInfo {
        std::string protocolVersion;
        std::string serverName;
        std::string serverVersion;
    };

    McpClient();
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    /// 拉起 server 进程。失败返回 false 并填 error（进程层面的失败不是协议错误）。
    bool connect(const std::string& command, std::string& error);

    /// initialize 握手：报上我们支持的最新版本，拿回协商后的版本与 server 信息。
    ServerInfo initialize();

    /// notifications/initialized：通知，按规范不读响应。
    void notifyInitialized();

    json listTools();
    json callTool(const std::string& name, const json& arguments);

    /// tools/call 的 result 是 {"content":[{"type":"text","text":...}],"isError":bool}，
    /// 把所有 text 片段拼成一段话。CLI 和 Agent 都用它，别各写一遍。
    static std::string toolResultText(const json& result);

    /// 关掉 server 的 stdin，等它自己退出。
    void shutdown();

    /// 单次读响应的超时（毫秒）。server 内部最多两次 HTTP，各 10s，默认给 30s。
    void setTimeoutMs(int timeoutMs);

private:
    json request(const std::string& method, const json& params);

    std::unique_ptr<ChildProcess> process_;
    int nextId_ = 1;
    int timeoutMs_ = 30000;
};

}  // namespace mcp
