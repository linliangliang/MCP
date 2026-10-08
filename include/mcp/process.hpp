#pragma once

#include <memory>
#include <string>

namespace mcp {

/// server 子进程 + 两根管道（写 stdin / 读 stdout）。
///
/// 这是客户端里唯一依赖平台的部分，刻意用 PIMPL 把 windows.h 挡在头文件外：
/// 将来补一个 src/process_posix.cpp 就能跨平台，client.cpp 一行都不用改。
///
/// stderr 不做重定向、直接继承父进程：server 的日志照常打到控制台，
/// 同时也绕开了"stderr 管道写满 → 子进程阻塞 → 父进程等不到响应"这个经典死锁。
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// 启动 command（UTF-8）。失败返回 false，原因见 lastError()。
    bool start(const std::string& command);
    const std::string& lastError() const;

    /// 写一行（自动补 '\n' 并立即发出）。
    bool writeLine(const std::string& line);

    /// 读一行（自动去掉行尾 '\r'）。超时或流结束返回 false。
    /// 超时是必须的：server 崩掉后父进程不能永远卡在 ReadFile 上。
    bool readLine(std::string& line, int timeoutMs);

    /// 关闭 stdin：server 见到 EOF 就会自己退出。
    void closeInput();

    /// 等待进程退出并返回退出码；超时返回 -1。
    int wait(int timeoutMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 当前可执行文件所在目录（用来定位同目录下的 server，省得每次手填绝对路径）。
std::string executableDir();

}  // namespace mcp
