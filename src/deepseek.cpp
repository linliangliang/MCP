// DeepSeek /chat/completions 的调用封装。

#include "mcp/deepseek.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

#include "mcp/http.hpp"

namespace mcp {

namespace {

constexpr const char* kEndpoint = "https://api.deepseek.com/chat/completions";

// 大模型比天气 API 慢得多，尤其是带工具的第二轮，超时给到 90 秒。
constexpr auto kTimeout = std::chrono::seconds(90);

}  // namespace

DeepSeekClient::DeepSeekClient(std::string apiKey, std::string model)
    : apiKey_(std::move(apiKey)), model_(std::move(model)) {}

json DeepSeekClient::chat(const json& messages, const json& tools) {
    // key 一律由外部注入（本地配置文件 / 环境变量 / 命令行），源码里不再有任何默认值。
    if (apiKey_.empty()) {
        throw std::runtime_error("没有配置 DeepSeek API key，无法调用大模型");
    }

    json request = json::object();
    request["model"] = model_;
    request["messages"] = messages;
    request["stream"] = false;
    if (!tools.is_null() && tools.is_array() && !tools.empty()) {
        request["tools"] = tools;
        request["tool_choice"] = "auto";
    }

    std::vector<HttpHeader> headers;
    headers.push_back({"Content-Type", "application/json; charset=utf-8"});
    headers.push_back({"Authorization", "Bearer " + apiKey_});

    const HttpResponse response = httpPost(kEndpoint, headers, request.dump(), kTimeout);
    if (response.status == 0) {
        throw std::runtime_error("调用 DeepSeek 失败（网络层）：" + response.error);
    }
    if (response.status != 200) {
        throw std::runtime_error("DeepSeek 返回 HTTP " + std::to_string(response.status) + "：" +
                                 response.body.substr(0, 300));
    }

    const json parsed = json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        throw std::runtime_error("DeepSeek 返回的不是合法 JSON：" + response.body.substr(0, 200));
    }
    if (parsed.contains("error")) {
        throw std::runtime_error("DeepSeek 返回错误：" +
                                 parsed.at("error").value("message", response.body.substr(0, 200)));
    }
    if (!parsed.contains("choices") || !parsed.at("choices").is_array() ||
        parsed.at("choices").empty()) {
        throw std::runtime_error("DeepSeek 响应里没有 choices：" + response.body.substr(0, 200));
    }
    return parsed.at("choices").at(0).value("message", json::object());
}

json toolsToFunctionSchemas(const json& toolsList) {
    json tools = json::array();
    const json& items = toolsList.contains("tools") && toolsList.at("tools").is_array()
                            ? toolsList.at("tools")
                            : json::array();
    for (const auto& tool : items) {
        json function = json::object();
        function["name"] = tool.value("name", "");
        function["description"] = tool.value("description", "");
        function["parameters"] =
            tool.contains("inputSchema") ? tool.at("inputSchema") : json::object();

        json entry = json::object();
        entry["type"] = "function";
        entry["function"] = function;
        tools.push_back(entry);
    }
    return tools;
}

}  // namespace mcp
