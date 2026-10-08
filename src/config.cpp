// 本地配置文件的读取：只关心一件事——把 API key 从源码里挪出去。
//
// 刻意不做的事：不在源码里留任何默认 key。写死默认值等于把密钥提交进版本库，
// 一旦仓库 push 到远端就收不回来了。

#include "mcp/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "mcp/json_rpc.hpp"
#include "mcp/process.hpp"

namespace mcp {

namespace {

constexpr const char* kConfigFileName = "deepseek.local.json";

std::string joinPath(const std::string& directory, const std::string& file) {
    if (directory.empty()) {
        return file;
    }
    if (directory.back() == '/' || directory.back() == '\\') {
        return directory + file;
    }
    return directory + "/" + file;
}

bool readFileText(const std::string& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
    return true;
}

/// 读单个候选文件。文件不存在时 source 留空（表示"这个候选没命中"）。
LlmConfig parseConfigFile(const std::string& path) {
    LlmConfig config;
    std::string text;
    if (!readFileText(path, text)) {
        return config;
    }
    config.source = path;

    const json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        config.error = "配置文件不是合法的 JSON 对象：" + path;
        return config;
    }
    config.apiKey = document.value("apiKey", "");
    config.model = document.value("model", config.model);
    return config;
}

}  // namespace

const char* configFileName() {
    return kConfigFileName;
}

LlmConfig loadLlmConfig(const std::string& explicitPath) {
    std::vector<std::string> candidates;
    if (!explicitPath.empty()) {
        candidates.push_back(explicitPath);
    }
    if (const char* fromEnv = std::getenv("DEEPSEEK_CONFIG")) {
        const std::string path = fromEnv;
        if (!path.empty()) {
            candidates.push_back(path);
        }
    }
    candidates.push_back(joinPath(executableDir(), kConfigFileName));
    candidates.push_back(kConfigFileName);

    // 记住第一个"文件存在但拿不到 key"的情况：报错时要说清楚是文件没建，
    // 还是建了但内容不对——这两种情况用户的下一步完全不同。
    LlmConfig firstFound;
    for (const std::string& path : candidates) {
        const LlmConfig parsed = parseConfigFile(path);
        if (parsed.source.empty()) {
            continue;
        }
        if (parsed.hasKey()) {
            return parsed;
        }
        if (firstFound.source.empty()) {
            firstFound = parsed;
        }
    }

    if (const char* fromEnv = std::getenv("DEEPSEEK_API_KEY")) {
        LlmConfig fromEnvConfig;
        fromEnvConfig.apiKey = fromEnv;
        fromEnvConfig.source = "环境变量 DEEPSEEK_API_KEY";
        if (fromEnvConfig.hasKey()) {
            return fromEnvConfig;
        }
    }

    return firstFound;
}

}  // namespace mcp
