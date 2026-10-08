// WeatherAgent：模型决策 → MCP 真调工具 → 结果回灌 → 生成自然语言。
//
// 这里最容易写错的一点是第二轮请求：工具结果必须以 role="tool" 的消息追加进历史，
// 并且 tool_call_id 要和模型给出的 id 对得上，否则模型会报"找不到对应的调用"。

#include "mcp/agent.hpp"

#include <ctime>
#include <stdexcept>

namespace mcp {

namespace {

constexpr int kMaxToolRounds = 4;

// 必须把"今天是几号"塞给模型：工具返回的是 2026-10-08 这种日期，
// 而用户说的是"明天"，模型不知道当天日期就只能猜——实测它会把明天答成今天。
constexpr const char* kSystemPrompt =
    "你是一个天气助手。用户问天气时，必须调用 get_weather 工具拿到真实数据后再回答，"
    "绝对不能凭记忆编造气温或天气状况。从用户的问法里推断参数："
    "city 用城市名（中文或英文均可）；days 是预报天数，问「今天」给 1、问「明天」给 2、"
    "问「未来一周」给 7、没说明天数就给 2。"
    "工具返回的文本里第一行是当前天气，后面的「未来 N 天预报」第一行是今天，"
    "往下依次是明天、后天，请按日期对号入座。"
    "回答用自然口语的中文，把日期、天气、气温范围、降水概率讲清楚，"
    "并在需要时提醒带伞。回答要简短，三四句话以内。";

std::string todayString() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return buffer;
}

std::string messageText(const json& message) {
    return message.value("content", "");
}

json toolCallsOf(const json& message) {
    if (!message.contains("tool_calls") || !message.at("tool_calls").is_array()) {
        return json::array();
    }
    return message.at("tool_calls");
}

}  // namespace

WeatherAgent::WeatherAgent(std::string serverPath, LlmConfig llm)
    : serverPath_(std::move(serverPath)), llm_(llm.apiKey, llm.model) {}

void WeatherAgent::connect() {
    std::string error;
    if (!client_.connect(serverPath_, error)) {
        throw std::runtime_error("启动 MCP server 失败：" + error);
    }
    serverInfo_ = client_.initialize();
    client_.notifyInitialized();
    tools_ = toolsToFunctionSchemas(client_.listTools());

    history_ = json::array();
    json system = json::object();
    system["role"] = "system";
    system["content"] = std::string(kSystemPrompt) + "今天的日期是 " + todayString() + "。";
    history_.push_back(system);
    connected_ = true;
}

void WeatherAgent::reset() {
    if (!connected_) {
        return;
    }
    history_ = json::array({history_.at(0)});  // 只保留 system 提示
}

std::string WeatherAgent::ask(const std::string& question) {
    if (!connected_) {
        connect();
    }

    json user = json::object();
    user["role"] = "user";
    user["content"] = question;
    history_.push_back(user);

    for (int round = 0; round < kMaxToolRounds; ++round) {
        const json assistant = llm_.chat(history_, tools_);
        history_.push_back(assistant);

        const json toolCalls = toolCallsOf(assistant);
        if (toolCalls.empty()) {
            // 模型没要调工具：说明它认为直接回答就够了（比如你在跟它打招呼）。
            return messageText(assistant);
        }
        executeToolCalls(toolCalls);
    }

    // 工具轮次用尽（模型反复调工具）：不再给工具，让它把手上的结果总结成话。
    const json summary = llm_.chat(history_, json());
    history_.push_back(summary);
    return messageText(summary);
}

void WeatherAgent::executeToolCalls(const json& toolCalls) {
    for (const auto& call : toolCalls) {
        const json function = call.contains("function") && call.at("function").is_object()
                                  ? call.at("function")
                                  : json::object();
        const std::string name = function.value("name", "");
        const std::string rawArguments = function.value("arguments", "{}");
        json arguments = json::parse(rawArguments, nullptr, /*allow_exceptions=*/false);
        if (arguments.is_discarded() || !arguments.is_object()) {
            arguments = json::object();
        }

        json toolMessage = json::object();
        toolMessage["role"] = "tool";
        toolMessage["tool_call_id"] = call.value("id", "");
        toolMessage["content"] = callToolWithRetry(name, arguments);
        history_.push_back(toolMessage);
    }
}

std::string WeatherAgent::callToolWithRetry(const std::string& name, const json& arguments) {
    try {
        return McpClient::toolResultText(client_.callTool(name, arguments));
    } catch (const std::exception& firstError) {
        // server 可能已经退出了（比如上一次用完就 EOF）：重新拉起一次再试。
        // 拿不到工具结果也要把失败原因交给模型——它才知道该怎么跟用户解释。
        try {
            connect();
            return McpClient::toolResultText(client_.callTool(name, arguments));
        } catch (const std::exception& retryError) {
            return "调用工具失败：" + std::string(retryError.what()) +
                   "（首次失败原因：" + firstError.what() + "）";
        }
    }
}

}  // namespace mcp
