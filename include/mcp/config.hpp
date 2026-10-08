#pragma once

#include <string>

namespace mcp {

/// 大模型的本地配置。
///
/// 它存在的唯一理由：API key 不能进版本库。
/// 仓库里只提交 `deepseek.example.json` 作为模板，每人把模板复制成
/// `deepseek.local.json` 再填自己的 key——后者已在 .gitignore 里。
struct LlmConfig {
    std::string apiKey;
    std::string model = "deepseek-chat";

    /// 这份配置是从哪读来的。启动时显示出来，免得"改了配置没生效"时查半天。
    std::string source;

    /// 读到了配置文件但内容有问题（不是 JSON、没有 apiKey）时填这里。
    std::string error;

    bool hasKey() const { return !apiKey.empty(); }
};

/// 按优先级找配置：
///   1. explicitPath（命令行 --config）
///   2. 环境变量 DEEPSEEK_CONFIG 指向的文件
///   3. exe 同目录下的配置文件
///   4. 当前工作目录下的配置文件
///   5. 环境变量 DEEPSEEK_API_KEY（没有配置文件时的最后退路）
///
/// 一个都没找到就返回 hasKey()==false 的配置，是否算失败交给调用方决定
/// （CLI 直接报错退出，GUI 把提示写进对话框）。
LlmConfig loadLlmConfig(const std::string& explicitPath = {});

/// 配置文件名，报错信息里要用。
const char* configFileName();

}  // namespace mcp
