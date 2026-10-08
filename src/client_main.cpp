// mcp-client：拉起 weather-mcp 子进程，跑完整握手，调一次 get_weather，把结果写入本地文件。
//
// 用法：
//   mcp-client.exe                                  # 默认查杭州，结果写到当前目录 weather-result.txt
//   mcp-client.exe --city 武汉 --days 3 --out d:/tmp/wuhan.txt
//   mcp-client.exe --json                           # 落成 JSON-RPC 原始 result 而不是纯文本
//
// 日志一律走 stderr，stdout 只留给人看的一行摘要——
// 这个约定是跟 server 学的：stdout 一旦混入非预期内容，解析方就会判断失败。

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/client.hpp"
#include "mcp/console.hpp"
#include "mcp/process.hpp"

namespace {

using json = nlohmann::json;

struct Options {
    std::string serverPath;
    std::string city = "杭州";
    int days = 2;
    std::string outputPath = "weather-result.txt";
    bool asJson = false;
};

void printUsage(const Options& defaults) {
    std::cout << "用法: mcp-client.exe [选项]\n"
              << "  --exe  <路径>   server 可执行文件（默认 " << defaults.serverPath << "）\n"
              << "  --city <城市>   城市名，中文或英文（默认 " << defaults.city << "）\n"
              << "  --days <1-16>   预报天数（默认 " << defaults.days << "）\n"
              << "  --out  <路径>   结果写入的文件（默认 " << defaults.outputPath << "）\n"
              << "  --json          写原始 JSON-RPC result，而不是纯文本\n"
              << "  -h, --help      打印本帮助\n";
}

bool writeTextFile(const std::string& path, const std::string& content, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "打不开文件：" + path;
        return false;
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.flush();
    if (!file) {
        error = "写入文件失败：" + path;
        return false;
    }
    return true;
}

Options parseArgs(const std::vector<std::string>& args, bool& wantHelp) {
    Options options;
    options.serverPath = mcp::executableDir() + "\\weather-mcp.exe";
    wantHelp = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string arg = args[i];
        auto value = [&](const std::string& name) -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("选项 " + name + " 缺少取值");
            }
            return args[++i];
        };

        if (arg == "--exe") {
            options.serverPath = value(arg);
        } else if (arg == "--city") {
            options.city = value(arg);
        } else if (arg == "--days") {
            options.days = std::stoi(value(arg));
        } else if (arg == "--out") {
            options.outputPath = value(arg);
        } else if (arg == "--json") {
            options.asJson = true;
        } else if (arg == "-h" || arg == "--help") {
            wantHelp = true;
        } else {
            throw std::runtime_error("未知参数：" + arg + "（用 --help 看用法）");
        }
    }

    if (options.days < 1 || options.days > 16) {
        throw std::runtime_error("--days 必须在 1-16 之间，收到 " + std::to_string(options.days));
    }
    return options;
}

int run(int argc, char** argv) {
    // argv 在中文 Windows 上是 GBK，中文城市名必须按 UTF-16 取回再转 UTF-8。
    const std::vector<std::string> args = mcp::decodeCommandLineArgs(argc, argv);
    bool wantHelp = false;
    const Options options = parseArgs(args, wantHelp);
    if (wantHelp) {
        Options defaults;
        defaults.serverPath = mcp::executableDir() + "\\weather-mcp.exe";
        printUsage(defaults);
        return 0;
    }

    mcp::McpClient client;
    std::string error;
    std::cerr << "[client] 启动 server: " << options.serverPath << std::endl;
    if (!client.connect(options.serverPath, error)) {
        std::cerr << "[client] 启动失败：" << error << std::endl;
        return 1;
    }

    const mcp::McpClient::ServerInfo info = client.initialize();
    std::cerr << "[client] 握手完成：server=" << info.serverName << " v" << info.serverVersion
              << "，协议版本=" << info.protocolVersion << std::endl;

    client.notifyInitialized();

    const json tools = client.listTools();
    std::cerr << "[client] 可用工具：";
    for (const auto& tool : tools.value("tools", json::array())) {
        std::cerr << tool.value("name", "?") << " ";
    }
    std::cerr << std::endl;

    json arguments = json::object();
    arguments["city"] = options.city;
    arguments["days"] = options.days;
    const json result = client.callTool("get_weather", arguments);

    const bool toolFailed = result.value("isError", false);
    const std::string content =
        options.asJson ? result.dump(2) + "\n" : mcp::McpClient::toolResultText(result) + "\n";

    if (!writeTextFile(options.outputPath, content, error)) {
        std::cerr << "[client] " << error << std::endl;
        return 1;
    }
    client.shutdown();

    const std::string absolute =
        std::filesystem::absolute(options.outputPath).lexically_normal().string();
    std::cout << "已写入 " << absolute << "（" << content.size() << " 字节）" << std::endl;
    if (toolFailed) {
        std::cerr << "[client] 工具返回 isError=true，内容已照样写入文件" << std::endl;
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // 必须在任何输出之前：否则中文日志和"已写入 ..."在 GBK 控制台上会变成乱码。
    mcp::useUtf8Console();
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[client] 失败：" << e.what() << std::endl;
        return 1;
    }
}
