#pragma once

#include <string>

namespace mcp {

/// 必须在读写任何字节前调用。
///
/// Windows 上有两个必须切 binary 模式的理由：
///   1. 默认 text 模式会把 '\n' 翻译成 "\r\n"，会干扰按行分帧的 JSON-RPC；
///   2. 字节 0x1A(Ctrl-Z) 在 text 模式下会被当成 EOF，一旦响应体里出现该字节，
///      客户端会认为流结束了。
/// 其他平台无此问题，因此这段是 #ifdef _WIN32 包着的。
void initTransport();

/// 读一行；到 EOF 返回 false。会自动去掉行尾可能残留的 '\r'。
bool readLine(std::string& line);

/// 写一行 + '\n' + flush。
/// 注意：stdout 只允许出现协议报文，任何日志请写 stderr。
void writeLine(const std::string& line);

}  // namespace mcp
