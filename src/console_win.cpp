// 控制台代码页与命令行参数编码的 Windows 实现。
//
// 非 Windows 平台下控制台和 argv 本来就是 UTF-8，所以走另一侧分支——
// 这个文件的存在只是为了让 main.cpp / client_main.cpp / qt_main.cpp 里不必写 #ifdef。

#include "mcp/console.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW（WIN32_LEAN_AND_MEAN 下不会随 windows.h 带进来）
#endif

namespace mcp {

#ifdef _WIN32
namespace {

std::string wideToUtf8(const std::wstring& wide) {
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

}  // namespace
#endif

void useUtf8Console() {
#ifdef _WIN32
    // 65001 = UTF-8。SetConsoleOutputCP 管输出，SetConsoleCP 管控制台输入
    // （server 交互调试时会从控制台读 JSON，里面有中文城市名）。
    // 两者在句柄不是控制台时都会失败，忽略返回值即可。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
#endif
}

std::vector<std::string> decodeCommandLineArgs(int argc, char** argv) {
#ifdef _WIN32
    int count = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (wide == nullptr || count <= 0) {
        return std::vector<std::string>(argv, argv + argc);
    }
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        args.push_back(wideToUtf8(wide[i]));
    }
    ::LocalFree(wide);
    return args;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

}  // namespace mcp
