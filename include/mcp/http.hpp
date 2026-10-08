#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace mcp {

/// HTTP 响应。约定：
///   status == 0 表示传输层就失败了（连接不上、超时、DNS 失败），原因写在 error 里；
///   status != 200 时 body 里是服务端返回的错误内容。
struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;
};

/// 一个请求头。只有 POST 才用得上（Content-Type / Authorization）。
struct HttpHeader {
    std::string name;
    std::string value;
};

/// 同步 GET。
///
/// 这是整个项目里唯一依赖平台的一层：Windows 上由 http_winhttp.cpp 用 WinHTTP 实现，
/// 将来要跨平台只需再加一个 src/http_curl.cpp，上层工具代码一行都不用改。
///
/// 注意：传入的 URL 必须是已经 percent-encode 过的（查询串里的中文尤其要编码），
/// 本函数不做任何 URL 编码。
HttpResponse httpGet(const std::string& url, std::chrono::milliseconds timeout);

/// 同步 POST。body 按原样字节发送（调用方负责 JSON 编码，中文保持 UTF-8 即可）。
///
/// 与 httpGet 共用同一套 WinHTTP 会话与超时设置，区别只在动词、请求头和请求体。
HttpResponse httpPost(const std::string& url,
                      const std::vector<HttpHeader>& headers,
                      const std::string& body,
                      std::chrono::milliseconds timeout);

}  // namespace mcp
