#include "mcp/transport_stdio.hpp"

#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace mcp {

void initTransport() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // 关掉与 C stdio 的同步，减少一层缓冲；stdio 由我们自己 flush 控制。
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
}

bool readLine(std::string& line) {
    if (!std::getline(std::cin, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

void writeLine(const std::string& line) {
    std::cout << line << '\n' << std::flush;
}

}  // namespace mcp
