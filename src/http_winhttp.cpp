// WinHTTP 版本的 httpGet / httpPost：系统自带、原生支持 TLS，不需要 OpenSSL / vcpkg。
//
// 代价是这份实现只能跑在 Windows 上——这正是把它藏在一个极薄接口后面的原因，
// 换平台时只需提供一个同名的 http_curl.cpp。

#include "mcp/http.hpp"

#ifdef _WIN32

#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

namespace mcp {

namespace {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}

std::string lastErrorText(DWORD code) {
    return "WinHTTP error " + std::to_string(code);
}

struct UrlParts {
    std::wstring host;
    std::wstring pathAndQuery;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool isHttps = true;
};

bool crackUrl(const std::wstring& url, UrlParts& out) {
    wchar_t scheme[32] = {0};
    wchar_t host[512] = {0};
    wchar_t path[4096] = {0};
    wchar_t extra[4096] = {0};

    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.lpszScheme = scheme;
    components.dwSchemeLength = 32;
    components.lpszHostName = host;
    components.dwHostNameLength = 512;
    components.lpszUrlPath = path;
    components.dwUrlPathLength = 4096;
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = 4096;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        return false;
    }

    out.host = host;
    out.isHttps = _wcsicmp(scheme, L"https") == 0;
    out.port = components.nPort;
    // path 之后要接上 ?query 部分，否则 host 能连上但请求的永远不是目标资源
    out.pathAndQuery = std::wstring(path) + std::wstring(extra);
    return true;
}

/// 请求头拼成 WinHTTP 要求的格式：每行一个，用 CRLF 结尾。
std::wstring buildHeaders(const std::vector<HttpHeader>& headers) {
    std::wstring out;
    for (const HttpHeader& header : headers) {
        out += utf8ToWide(header.name + ": " + header.value) + L"\r\n";
    }
    return out;
}

/// GET 与 POST 共用的那一段：建会话 → 建连接 → 发请求 → 收状态码 → 读 body。
/// 两者只有动词、请求头、请求体不同，所以差异都通过参数传进来。
HttpResponse sendOnce(const std::string& url,
                      const wchar_t* verb,
                      const std::vector<HttpHeader>& headers,
                      const std::string& body,
                      std::chrono::milliseconds timeout) {
    HttpResponse response;

    UrlParts parts;
    if (!crackUrl(utf8ToWide(url), parts)) {
        response.error = "URL 解析失败: " + url;
        return response;
    }

    const int timeoutMs = static_cast<int>(timeout.count());

    HINTERNET session = WinHttpOpen(L"weather-mcp/0.1",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (session == nullptr) {
        response.error = lastErrorText(GetLastError());
        return response;
    }

    HINTERNET connection = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (connection == nullptr) {
        response.error = lastErrorText(GetLastError());
        WinHttpCloseHandle(session);
        return response;
    }

    HINTERNET request = WinHttpOpenRequest(connection,
                                           verb,
                                           parts.pathAndQuery.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           parts.isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) {
        response.error = lastErrorText(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    // 超时必须显式设置：默认超时可能长达几十秒甚至更久，
    // MCP 调用挂在那里会让客户端一直等到自己的超时，体验极差。
    WinHttpSetTimeouts(request, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    const std::wstring headerText = buildHeaders(headers);
    // POST 时把 body 交给 WinHttpSendRequest 的 optional 参数，Content-Length 由它自己算。
    // 没有 body 时传 WINHTTP_NO_REQUEST_DATA，否则服务端会一直等一个不存在的请求体。
    const bool hasBody = !body.empty();
    LPVOID optional = hasBody ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA;
    const DWORD optionalLength = hasBody ? static_cast<DWORD>(body.size()) : 0;

    const bool sent = WinHttpSendRequest(request,
                                         headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                            : headerText.c_str(),
                                         headerText.empty() ? 0 : static_cast<DWORD>(-1L),
                                         optional,
                                         optionalLength,
                                         optionalLength,
                                         0) &&
                      WinHttpReceiveResponse(request, nullptr);
    if (!sent) {
        response.error = lastErrorText(GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<long>(status);

    std::string received;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            response.error = lastErrorText(GetLastError());
            break;
        }
        received.append(buffer.data(), read);
    }
    response.body = std::move(received);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

}  // namespace

HttpResponse httpGet(const std::string& url, std::chrono::milliseconds timeout) {
    return sendOnce(url, L"GET", {}, {}, timeout);
}

HttpResponse httpPost(const std::string& url,
                      const std::vector<HttpHeader>& headers,
                      const std::string& body,
                      std::chrono::milliseconds timeout) {
    return sendOnce(url, L"POST", headers, body, timeout);
}

}  // namespace mcp

#endif  // _WIN32
