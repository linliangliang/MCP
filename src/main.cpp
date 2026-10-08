// MCP server 入口。
//
// 三条铁律（违反任意一条都会让客户端连不上，且症状极难定位）：
//   1. stdout 只能出现换行分隔的 JSON-RPC 报文；
//   2. 所有日志/调试信息必须走 stderr；
//   3. 每条响应写完必须 flush，否则客户端会一直等到超时。
//
// stdio 传输用的是“换行分隔的单行 JSON”，不是 LSP 那种 Content-Length 分帧。

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "mcp/console.hpp"
#include "mcp/json_rpc.hpp"
#include "mcp/server.hpp"
#include "mcp/tools/weather.hpp"
#include "mcp/transport_stdio.hpp"

int main() {
    // 必须在任何输出之前。stderr 的中文日志（城市名、版本告警）否则会显示成乱码。
    mcp::useUtf8Console();
    mcp::initTransport();
    std::cerr << "[mcp] weather-mcp started" << std::endl;

    mcp::ToolRegistry registry;
    mcp::registerWeatherTools(registry);

    std::string line;
    while (mcp::readLine(line)) {
        if (line.empty()) {
            continue;
        }

        // 单行解析失败只跳过这一行，不能让进程退出：后续还可能收到合法请求。
        mcp::json request = mcp::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (request.is_discarded()) {
            std::cerr << "[mcp] bad json, skipped: " << line.substr(0, 120) << std::endl;
            continue;
        }

        const std::optional<mcp::json> response = mcp::handleRequest(registry, request);
        if (!response.has_value()) {
            continue;  // 通知类消息，按规范不回响应
        }

        const std::string payload = response->dump();
        mcp::writeLine(payload);
        std::cerr << "[mcp] -> " << (response->contains("error") ? "error" : "result")
                  << " id=" << response->value("id", mcp::json(nullptr)).dump()
                  << " (" << payload.size() << " bytes)" << std::endl;
    }

    std::cerr << "[mcp] stdin closed, exiting" << std::endl;
    return 0;
}
