#pragma once
// 极简阻塞式 HTTP/HTTPS 客户端（用于 session 管理的少量请求）
#include <string>

struct HttpResponse {
    int status = 0;      // HTTP 状态码
    std::string body;    // 响应体
};

// method: GET/POST；url: http(s)://host[:port]/path
// body: 请求体（POST 用，可为空）；cafile: HTTPS 校验证书 bundle（http 时忽略）
// timeout_sec: 连接+读超时。失败返回 false 并在日志打印原因。
bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& cafile,
                  int timeout_sec, HttpResponse& out);
