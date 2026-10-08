// ChildProcess 的 Windows 实现：CreateProcess + 匿名管道。
//
// 三个必须做对的点，漏一个就会连不上或者死锁：
//   1. 父进程持有的那一端管道必须 SetHandleInformation(..., 0) 取消继承，
//      否则句柄泄漏到子进程，父进程关掉 stdin 后子进程收不到 EOF；
//   2. 子进程那一端（childStdin / childStdout）在父进程里必须 CloseHandle，
//      否则子进程退出后管道不会 EOF，readLine 会一直等；
//   3. CreateProcessW 会就地改写命令行缓冲区，所以必须传一份可写的副本。

#include "mcp/process.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#error "src/process_win.cpp 只能在 Windows 上编译；其他平台请另写 src/process_posix.cpp"
#endif

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace mcp {

namespace {

std::string winError(const std::string& what, unsigned long code) {
    return what + " 失败（GetLastError=" + std::to_string(code) + "）";
}

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return wide;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                          needed, nullptr, nullptr);
    return utf8;
}

/// 路径带空格时必须加引号，且 CreateProcessW 要求缓冲区可写。
std::vector<wchar_t> buildCommandLine(const std::string& command) {
    const std::wstring wide = L"\"" + toWide(command) + L"\"";
    std::vector<wchar_t> buffer(wide.begin(), wide.end());
    buffer.push_back(L'\0');
    return buffer;
}

}  // namespace

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    std::string pending;  // 已读到但还没凑成一整行的字节
    std::string error;

    ~Impl() {
        if (stdinWrite != nullptr) {
            ::CloseHandle(stdinWrite);
        }
        if (stdoutRead != nullptr) {
            ::CloseHandle(stdoutRead);
        }
        if (process != nullptr) {
            ::CloseHandle(process);
        }
    }
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() = default;

const std::string& ChildProcess::lastError() const {
    return impl_->error;
}

bool ChildProcess::start(const std::string& command) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE childStdin = nullptr;
    HANDLE parentStdin = nullptr;
    if (!::CreatePipe(&childStdin, &parentStdin, &security, 0)) {
        impl_->error = winError("CreatePipe(stdin)", ::GetLastError());
        return false;
    }

    HANDLE parentStdout = nullptr;
    HANDLE childStdout = nullptr;
    if (!::CreatePipe(&parentStdout, &childStdout, &security, 0)) {
        impl_->error = winError("CreatePipe(stdout)", ::GetLastError());
        ::CloseHandle(childStdin);
        ::CloseHandle(parentStdin);
        return false;
    }

    ::SetHandleInformation(parentStdin, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(parentStdout, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdin;
    startup.hStdOutput = childStdout;
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION info{};
    std::vector<wchar_t> commandLine = buildCommandLine(command);
    const BOOL started = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
                                          /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &startup,
                                          &info);

    ::CloseHandle(childStdin);
    ::CloseHandle(childStdout);

    if (!started) {
        impl_->error = winError("CreateProcessW(" + command + ")", ::GetLastError());
        ::CloseHandle(parentStdin);
        ::CloseHandle(parentStdout);
        return false;
    }

    ::CloseHandle(info.hThread);
    impl_->process = info.hProcess;
    impl_->stdinWrite = parentStdin;
    impl_->stdoutRead = parentStdout;
    return true;
}

bool ChildProcess::writeLine(const std::string& line) {
    if (impl_->stdinWrite == nullptr) {
        impl_->error = "stdin 已关闭";
        return false;
    }
    const std::string payload = line + "\n";
    DWORD written = 0;
    const BOOL ok = ::WriteFile(impl_->stdinWrite, payload.data(),
                                static_cast<DWORD>(payload.size()), &written, nullptr);
    if (!ok || written != payload.size()) {
        impl_->error = winError("WriteFile(stdin)", ::GetLastError());
        return false;
    }
    return true;
}

bool ChildProcess::readLine(std::string& line, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buffer[4096];

    while (true) {
        const size_t newline = impl_->pending.find('\n');
        if (newline != std::string::npos) {
            line = impl_->pending.substr(0, newline);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            impl_->pending.erase(0, newline + 1);
            return true;
        }

        DWORD available = 0;
        if (!::PeekNamedPipe(impl_->stdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
            // 管道断了（子进程退出，且写端全部关闭）。
            if (!impl_->pending.empty()) {
                line = impl_->pending;
                impl_->pending.clear();
                return true;
            }
            impl_->error = "server 的 stdout 已关闭";
            return false;
        }

        if (available > 0) {
            DWORD read = 0;
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
            if (::ReadFile(impl_->stdoutRead, buffer, want, &read, nullptr) && read > 0) {
                impl_->pending.append(buffer, read);
            }
            continue;
        }

        if (::WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
            if (!impl_->pending.empty()) {
                line = impl_->pending;
                impl_->pending.clear();
                return true;
            }
            impl_->error = "server 进程已退出，且没有更多输出";
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            impl_->error = "等待 server 响应超时";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::closeInput() {
    if (impl_->stdinWrite != nullptr) {
        ::CloseHandle(impl_->stdinWrite);
        impl_->stdinWrite = nullptr;
    }
}

int ChildProcess::wait(int timeoutMs) {
    if (impl_->process == nullptr) {
        return -1;
    }
    if (::WaitForSingleObject(impl_->process, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
        return -1;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(impl_->process, &code)) {
        return -1;
    }
    return static_cast<int>(code);
}

std::string executableDir() {
    std::vector<wchar_t> buffer(MAX_PATH);
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return {};
    }
    std::wstring full(buffer.data(), static_cast<size_t>(length));
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return toUtf8(full.substr(0, slash));
}

}  // namespace mcp
