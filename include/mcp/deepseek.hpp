#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "mcp/json_rpc.hpp"

namespace mcp {

/// DeepSeek 对话客户端（OpenAI 兼容的 /chat/completions）。
///
/// 这个类只做一件事：把 messages + tools 发上去，把 assistant 的 message 原样带回来。
/// "该不该调工具、调哪个"由模型决定，真正的执行在调用方（WeatherAgent）手里——
/// LLM 和工具执行分开，是这类代码唯一清爽的切法。
///
/// 支持 function calling（tool_calls），这是"模型指挥工具"的标准姿势：
/// 比让模型吐 JSON 再正则去抓要稳得多。
class DeepSeekClient {
public:
    /// apiKey 为空时依次回落到环境变量 DEEPSEEK_API_KEY、再到源码里的默认值。
    explicit DeepSeekClient(std::string apiKey = {}, std::string model = "deepseek-chat");

    /// 返回 assistant 的 message 对象（可能带 tool_calls）。失败抛 std::runtime_error。
    /// tools 为 null 或空数组时不带 tools 字段（用于最后一轮让模型只做总结）。
    json chat(const json& messages, const json& tools);

private:
    std::string apiKey_;
    std::string model_;
};

/// 把 MCP 的 tools/list 结果翻译成 OpenAI/DeepSeek 的 tools 格式。
///
/// 直接复用 server 自己声明的 inputSchema，好处是工具改了参数这边不用跟着改，
/// 也不会出现"两边 schema 不一致导致模型传错参数"的问题。
json toolsToFunctionSchemas(const json& toolsList);

}  // namespace mcp
