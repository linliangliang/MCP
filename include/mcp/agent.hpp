#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "mcp/client.hpp"
#include "mcp/config.hpp"
#include "mcp/deepseek.hpp"

namespace mcp {

/// 把"大模型"和"MCP server"串起来的最小 agent。
///
/// 一轮对话就是所谓的 tool-calling 循环：
///   用户提问 → 模型决定要不要调工具
///           → 要：通过 McpClient 真的去调用 weather-mcp
///           → 工具结果作为 role=tool 的消息塞回对话
///           → 再问模型，拿到给用户的自然语言回答
///
/// 对话历史一直留在内存里，所以接着问"那上海呢"也能对得上上下文。
/// server 子进程也只拉起一次，多轮复用。
class WeatherAgent {
public:
    /// serverPath: weather-mcp.exe 的路径；llm 配置里带 API key（来自本地配置文件）。
    WeatherAgent(std::string serverPath, LlmConfig llm);

    /// 握手 + 取工具列表。ask() 在未连接时会自动调用；也可以提前调来做启动检查。
    void connect();
    bool connected() const { return connected_; }

    McpClient::ServerInfo serverInfo() const { return serverInfo_; }

    /// 跑一轮，返回给用户的最终回答（中间可能调过若干次工具）。
    std::string ask(const std::string& question);

    /// 清空对话历史（server 连接保留）。
    void reset();

private:
    void executeToolCalls(const json& toolCalls);
    std::string callToolWithRetry(const std::string& name, const json& arguments);

    std::string serverPath_;
    DeepSeekClient llm_;
    McpClient client_;
    json tools_;
    json history_;
    McpClient::ServerInfo serverInfo_;
    bool connected_ = false;
};

}  // namespace mcp
