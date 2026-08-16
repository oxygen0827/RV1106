#pragma once
// WebSocket 传输层（websocketpp）
// —— "传输层抽象"的核心：上层只依赖 IWsTransport 接口，
//    实现按 URL scheme 选择明文（ws://）或 TLS（wss://）后端；
//    将来 RTC 路线只需新增 RtcTransport 实现同一接口。
#include "util.h"

#include <json/json.h>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>          // asio_tls_client（wss://）
#include <websocketpp/config/asio_no_tls_client.hpp>  // asio_client（ws://）

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// 传输层统一接口（与具体 WebSocket 配置解耦）
struct IWsTransport {
    virtual ~IWsTransport() = default;
    virtual bool start() = 0;                    // 启动内部 io 线程
    virtual bool connect(const std::string& url, const std::string& cafile) = 0;
    virtual bool send_json(const Json::Value& v) = 0;
    // 二进制帧（API_DOC v2：转写通道推荐直接发裸 PCM bytes）
    virtual bool send_binary(const uint8_t* data, size_t len) = 0;
    virtual bool is_open() const = 0;
    virtual void close() = 0;
    virtual void stop() = 0;

    // 回调（运行在 io 线程）
    std::function<void(const Json::Value&)> on_message;
    std::function<void()> on_open;
    std::function<void(const std::string&)> on_fail;
    std::function<void(int code, const std::string&)> on_close;
};

// TLS 初始化策略：TLS 配置必须在 get_connection 前设置 handler；
// 明文配置（asio_client = asio_no_tls）没有也不允许 tls_init handler。
template <typename ConfigT>
struct TlsSetup {
    template <typename ClientT>
    static void setup(ClientT& c, const std::string& cafile) {
        c.set_tls_init_handler([cafile](websocketpp::connection_hdl) {
            auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
                boost::asio::ssl::context::tls_client);
            if (!cafile.empty()) {
                ctx->load_verify_file(cafile);
            } else {
                ctx->set_default_verify_paths();
            }
            ctx->set_verify_mode(boost::asio::ssl::verify_peer);
            return ctx;
        });
    }
};

template <>
struct TlsSetup<websocketpp::config::asio_client> {
    template <typename ClientT>
    static void setup(ClientT&, const std::string&) {}
};

template <typename ConfigT>
class WsTransportImpl : public IWsTransport {
public:
    using Client = websocketpp::client<ConfigT>;
    using Hdl = websocketpp::connection_hdl;

    WsTransportImpl() {
        client_.init_asio();
        client_.set_error_channels(websocketpp::log::elevel::all);
        client_.clear_access_channels(websocketpp::log::alevel::all);
    }

    ~WsTransportImpl() override { stop(); }

    bool start() override {
        if (running_.load()) return true;
        running_.store(true);
        // work guard：io_service 空闲时也不让 run() 提前返回，
        // 否则 connect() 在 start() 之后排队会永远不被处理
        work_ = new boost::asio::io_service::work(client_.get_io_service());
        io_thread_ = std::thread([this]() {
            try {
                client_.run();
            } catch (const std::exception& e) {
                LOGF("ws io thread: %s", e.what());
            }
            running_.store(false);
        });
        return true;
    }

    bool connect(const std::string& url, const std::string& cafile) override {
        TlsSetup<ConfigT>::setup(client_, cafile);  // TLS 配置必须先于 get_connection

        websocketpp::lib::error_code ec;
        auto conn = client_.get_connection(url, ec);
        if (ec) {
            LOGF("ws get_connection: %s", ec.message().c_str());
            return false;
        }
        hdl_ = conn->get_handle();

        conn->set_message_handler([this](Hdl hdl, typename Client::message_ptr msg) {
            (void)hdl;
            if (msg->get_opcode() != websocketpp::frame::opcode::text) return;
            Json::Value v;
            Json::CharReaderBuilder rbuilder;
            std::string errs;
            std::istringstream is(msg->get_payload());
            if (!Json::parseFromStream(rbuilder, is, &v, &errs)) {
                LOGF("ws non-json message (%zuB)", msg->get_payload().size());
                return;
            }
            if (on_message) on_message(v);
        });

        conn->set_open_handler([this](Hdl hdl) {
            (void)hdl;
            open_.store(true);
            LOGF("ws connected");
            if (on_open) on_open();
        });

        conn->set_fail_handler([this](Hdl hdl) {
            (void)hdl;
            auto c = client_.get_con_from_hdl(hdl);
            std::string err = c ? c->get_ec().message() : "unknown";
            LOGF("ws handshake failed: %s", err.c_str());
            if (on_fail) on_fail(err);
        });

        conn->set_close_handler([this](Hdl hdl) {
            (void)hdl;
            auto c = client_.get_con_from_hdl(hdl);
            int code = c ? c->get_local_close_code() : 0;
            std::string reason = c ? c->get_local_close_reason() : "";
            open_.store(false);
            LOGF("ws closed code=%d reason=%s", code, reason.c_str());
            if (on_close) on_close(code, reason);
        });

        client_.connect(conn);
        return true;
    }

    bool send_json(const Json::Value& v) override {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            out_queue_.push_back({true, Json::writeString(w, v)});
        }
        // post 到 io 线程执行（io_service::post 线程安全）
        client_.get_io_service().post([this]() { flush_out_queue(); });
        return true;
    }

    bool send_binary(const uint8_t* data, size_t len) override {
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            out_queue_.push_back({false, std::string(reinterpret_cast<const char*>(data), len)});
        }
        client_.get_io_service().post([this]() { flush_out_queue(); });
        return true;
    }

    void flush_out_queue() {
        std::vector<OutFrame> batch;
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            batch.swap(out_queue_);
        }
        for (const OutFrame& f : batch) {
            if (!open_.load()) continue;  // 未连接/已关闭时静默丢弃
            websocketpp::lib::error_code ec;
            client_.send(hdl_, f.data,
                         f.text ? websocketpp::frame::opcode::text
                                : websocketpp::frame::opcode::binary,
                         ec);
            if (ec) LOGF("ws send: %s", ec.message().c_str());
        }
    }

    bool is_open() const override { return open_.load(); }

    void close() override {
        websocketpp::lib::error_code ec;
        if (open_.load()) client_.close(hdl_, websocketpp::close::status::normal, "", ec);
    }

    void stop() override {
        // 先删 work guard 再 stop，保证 run() 能退出；join 不依赖 running_ 状态
        if (work_) {
            delete work_;
            work_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            out_queue_.clear();
        }
        try {
            client_.stop();
        } catch (...) {}
        if (io_thread_.joinable()) io_thread_.join();
    }

private:
    // 发送编组：非 io 线程直接 send 会让 asio reactor 的异步写注册与 epoll
    // 竞争丢失（帧被无限期延迟）——统一 post 到 io 线程串行发送。
    struct OutFrame {
        bool text;
        std::string data;
    };
    Client client_;
    std::thread io_thread_;
    std::mutex q_mutex_;
    std::vector<OutFrame> out_queue_;
    boost::asio::io_service::work* work_ = nullptr;
    Hdl hdl_;
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
};

// 工厂：按 URL scheme 选择明文/TLS 实现
inline IWsTransport* make_ws_transport(const std::string& url) {
    if (url.rfind("wss://", 0) == 0)
        return new WsTransportImpl<websocketpp::config::asio_tls_client>();
    return new WsTransportImpl<websocketpp::config::asio_client>();
}
