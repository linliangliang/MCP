#pragma once

#include <string>
#include <vector>

namespace mcp {

/// 让控制台按 UTF-8 解释本进程的输出。
///
/// 为什么需要：源码是 UTF-8、编译加了 /utf-8，程序写出的就是 UTF-8 字节；
/// 但中文 Windows 的控制台默认代码页是 936(GBK)，于是"启动"会被显示成"鍚姩"。
/// 字节本身没坏，是看的那一边解码错了。
///
/// 注意这只影响**终端显示**：输出被重定向到文件或管道时，这两个调用会失败，
/// 而那种情况下字节原样传出去、本来就是正确的 UTF-8，无需处理。
/// 千万别用"转成 GBK 再输出"来解决——那会真把数据改坏。
void useUtf8Console();

/// 把命令行参数取成 UTF-8。
///
/// Windows 上 `main(argc, argv)` 里的 argv 是**系统 ANSI 编码**（中文 Windows 即 GBK），
/// 从 cmd 传一个中文城市名进来，nlohmann 序列化时会直接抛
/// `invalid UTF-8 byte ...`。所以这里绕开 argv，用 GetCommandLineW 拿 UTF-16
/// 再转 UTF-8。非 Windows 平台 argv 本来就是 UTF-8，原样返回。
std::vector<std::string> decodeCommandLineArgs(int argc, char** argv);

}  // namespace mcp
