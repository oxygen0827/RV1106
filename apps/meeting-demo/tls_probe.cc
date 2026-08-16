// TLS 链路探针：验证板端 wss（asio_tls_client + CA bundle 校验 + SNI）可用性。
// 用法: tls_probe wss://echo.websocket.org /root/bin/cacert.pem
// 行为: 握手成功 → 发一条 JSON → 应收到 echo 原样返回 → 关闭。
// 用途: 切真实后端前，把 TLS 问题与协议/网络问题分开定位。
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "util.h"
#include "wss_transport.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: tls_probe <wss-url> <cafile> [--insecure]\n");
        return 2;
    }
    std::string url = argv[1];
    std::string cafile = argv[2];
    bool insecure = (argc > 3 && std::string(argv[3]) == "--insecure");
    if (insecure) cafile.clear();

    IWsTransport* t = make_ws_transport(url);
    std::atomic<bool> got_echo{false};
    std::atomic<bool> failed{false};

    t->on_open = [&]() { LOGF("TLS handshake OK, connection open"); };
    t->on_fail = [&](const std::string& e) {
        LOGF("handshake FAILED: %s", e.c_str());
        failed.store(true);
    };
    t->on_close = [&](int code, const std::string& r) {
        LOGF("closed code=%d reason=%s", code, r.c_str());
    };
    t->on_message = [&](const Json::Value& v) {
        LOGF("echo received: %s", Json::writeString(
            []() { Json::StreamWriterBuilder w; w["indentation"]=""; return w; }(), v).c_str());
        got_echo.store(true);
    };

    t->start();
    if (!t->connect(url, cafile)) {
        LOGF("connect() failed");
        return 1;
    }

    // 等握手 + echo，最多 10s
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool sent = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (failed.load()) break;
        if (!sent && t->is_open()) {
            Json::Value m;
            m["type"] = "probe";
            m["ts"] = (Json::UInt64)std::time(nullptr);
            t->send_json(m);
            sent = true;
        }
        if (got_echo.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    t->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    t->stop();
    delete t;

    if (!got_echo.load()) {
        LOGF("RESULT: FAIL (no echo)");
        return 1;
    }
    LOGF("RESULT: PASS — 板端 wss + CA 校验链路可用");
    return 0;
}
