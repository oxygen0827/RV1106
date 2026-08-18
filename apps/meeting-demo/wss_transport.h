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
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <thread>

#ifndef TIOCOUTQ
#define TIOCOUTQ 0x5411
#endif

// 传输层统一接口（与具体 WebSocket 配置解耦）
struct IWsTransport {
    virtual ~IWsTransport() = default;
    virtual bool start() = 0;                    // 启动内部 io 线程
    virtual bool connect(const std::string& url, const std::string& cafile) = 0;
    virtual bool send_json(const Json::Value& v) = 0;
    // 二进制帧（API_DOC v2：转写通道推荐直接发裸 PCM bytes）
    virtual bool send_binary(const uint8_t* data, size_t len) = 0;
    // 等待应用队列清空和底层写队列持续空闲，保证 end 排在全部 PCM 后面。
    virtual bool flush(int timeout_ms) = 0;
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
        stopping_.store(false);
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
        remote_close_code_.store(0);
        send_failed_.store(false);
        queue_overflow_reported_.store(false);
        current_url_ = url;
        current_cafile_ = cafile;
        pump_scheduled_.store(false);

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
            // 重连成功后队列里可能还有断线期间入队的帧，pump 已在
            // 断线时退出（pump_scheduled_=false），这里必须重新启动。
            bool schedule = false;
            {
                std::lock_guard<std::mutex> lk(q_mutex_);
                if (!out_queue_.empty() && !pump_scheduled_.exchange(true))
                    schedule = true;
            }
            queue_cv_.notify_all();
            LOGF("ws connected");
            if (schedule) client_.get_io_service().post([this]() { pump_once(); });
            if (on_open) on_open();
        });

        // Do not put application PCM ahead of a server Ping: a queued 100ms
        // frame on RTL8723BS can be worth several seconds of uplink time.
        conn->set_ping_handler([this](Hdl, std::string) {
            if (!stopping_.load()) begin_pong_pause();
            return true;
        });

        conn->set_fail_handler([this](Hdl hdl) {
            (void)hdl;
            auto c = client_.get_con_from_hdl(hdl);
            std::string err = c ? c->get_ec().message() : "unknown";
            LOGF("ws handshake failed: %s", err.c_str());
            if (!intentional_close_.load()) schedule_reconnect();
            if (on_fail) on_fail(err);
        });

        conn->set_close_handler([this](Hdl hdl) {
            (void)hdl;
            auto c = client_.get_con_from_hdl(hdl);
            int code = c ? c->get_remote_close_code() : 0;
            std::string reason = c ? c->get_remote_close_reason() : "";
            remote_close_code_.store(code);
            open_.store(false);
            queue_cv_.notify_all();
            LOGF("ws closed code=%d reason=%s", code, reason.c_str());
            if (on_close) on_close(code, reason);
            if (!intentional_close_.load() && !stopping_.load())
                schedule_reconnect();
        });

        client_.connect(conn);
        return true;
    }

    bool send_json(const Json::Value& v) override {
        // 允许在重连窗口内入队：弱网 1011 后连接会短暂断开，
        // 此时返回 false 会被上层当成致命错误，而队列本可骑过这次重连。
        if (send_failed_.load() || stopping_.load()) {
            send_failed_.store(true);
            return false;
        }
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return enqueue({true, Json::writeString(w, v)});
    }

    bool send_binary(const uint8_t* data, size_t len) override {
        if (send_failed_.load() || stopping_.load()) {
            send_failed_.store(true);
            return false;
        }
        return enqueue({false, std::string(reinterpret_cast<const char*>(data), len)});
    }

    bool flush(int timeout_ms) override {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lk(q_mutex_);
        while (std::chrono::steady_clock::now() < deadline) {
            if (send_failed_.load()) return false;
            const bool app_queue_empty = out_queue_.empty() && !pump_scheduled_.load();
            if (app_queue_empty && !open_.load())
                return remote_close_code_.load() == websocketpp::close::status::normal;
            if (app_queue_empty) return true;
            queue_cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
        return false;
    }

    bool is_open() const override { return open_.load(); }

    void close() override {
        intentional_close_.store(true);
        websocketpp::lib::error_code ec;
        if (open_.load()) client_.close(hdl_, websocketpp::close::status::normal, "", ec);
    }

    void stop() override {
        stopping_.store(true);
        queue_cv_.notify_all();
        // 先删 work guard 再 stop，保证 run() 能退出；join 不依赖 running_ 状态
        if (work_) {
            delete work_;
            work_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            out_queue_.clear();
            out_queue_bytes_ = 0;
        }
        try {
            client_.stop();
        } catch (...) {}
        if (io_thread_.joinable()) io_thread_.join();
    }

private:
    // 所有发送均在 io 线程串行执行。Ping 到达后暂停新的 PCM 入队，自动
    // Pong 只会排在极少待发数据后。
    struct OutFrame {
        bool text;
        std::string data;
    };
    struct QueuedFrame {
        int bytes = 0;
        OutFrame frame;
    };
    QueuedFrame make_queued_frame(OutFrame frame) {
        QueuedFrame queued;
        queued.bytes = static_cast<int>(frame.data.size());
        queued.frame = std::move(frame);
        return queued;
    }

    void begin_pong_pause() {
        pong_pause_until_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(700);
    }

    bool pong_pause_active() const {
        return std::chrono::steady_clock::now() < pong_pause_until_;
    }

    void schedule_reconnect() {
        if (!running_.load() || intentional_close_.load() || stopping_.load()) return;
        if (reconnect_scheduled_.exchange(true)) return;
        client_.set_timer(1000, [this](const websocketpp::lib::error_code& ec) {
            if (ec || stopping_.load() || intentional_close_.load()) {
                reconnect_scheduled_.store(false);
                return;
            }
            reconnect_scheduled_.store(false);
            LOGF("ws reconnecting");
            connect(current_url_, current_cafile_);
        });
    }

    bool enqueue(OutFrame frame) {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            if (!frame.text && out_queue_bytes_ + frame.data.size() > kMaxQueueBytes) {
                if (!queue_overflow_reported_.exchange(true))
                    LOGF("ws audio queue overflow (%zuB); meeting is incomplete",
                         out_queue_bytes_);
                // 溢出即纪要不再完整；无论当前是否在线都标记失败，
                // 让结束流程如实上报而不是悄悄丢帧后自称完整。
                send_failed_.store(true);
                return false;
            }
            out_queue_bytes_ += frame.data.size();
            out_queue_.push_back(make_queued_frame(std::move(frame)));
            if (open_.load() && !pump_scheduled_.exchange(true)) schedule = true;
        }
        queue_cv_.notify_all();
        if (schedule) client_.get_io_service().post([this]() { pump_once(); });
        return true;
    }

    void schedule_pump_retry() {
        client_.set_timer(25, [this](const websocketpp::lib::error_code& ec) {
            if (!ec && !stopping_.load()) pump_once();
        });
    }

    void fail_pump(const std::string& reason) {
        if (!send_failed_.exchange(true)) LOGF("ws send: %s", reason.c_str());
        pump_scheduled_.store(false);
        queue_cv_.notify_all();
    }

    void pump_once() {
        if (stopping_.load()) {
            pump_scheduled_.store(false);
            queue_cv_.notify_all();
            return;
        }

        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            if (out_queue_.empty() || (!open_.load() && !intentional_close_.load())) {
                pump_scheduled_.store(false);
                queue_cv_.notify_all();
                return;
            }
        }

        websocketpp::lib::error_code ec;
        auto conn = client_.get_con_from_hdl(hdl_, ec);
        if (ec || !conn) {
            pump_scheduled_.store(false);
            return;
        }
        if (conn->get_state() != websocketpp::session::state::open) {
            pump_scheduled_.store(false);
            return;
        }
        int kernel_pending = 0;
        if (::ioctl(conn->get_raw_socket().native_handle(), TIOCOUTQ,
                    &kernel_pending) != 0) {
            fail_pump("cannot inspect socket send queue");
            return;
        }
        if (conn->get_buffered_amount() != 0 || kernel_pending > kMaxKernelPending) {
            schedule_pump_retry();
            return;
        }

        if (pong_pause_active()) {
            schedule_pump_retry();
            return;
        }

        OutFrame frame;
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            if (out_queue_.empty()) {
                pump_scheduled_.store(false);
                queue_cv_.notify_all();
                return;
            }
            QueuedFrame queued = std::move(out_queue_.front());
            frame = std::move(queued.frame);
            out_queue_bytes_ -= queued.bytes;
            out_queue_.pop_front();
        }

        client_.send(hdl_, frame.data,
                     frame.text ? websocketpp::frame::opcode::text
                                : websocketpp::frame::opcode::binary,
                     ec);
        if (ec) {
            fail_pump(ec.message());
            return;
        }
        queue_cv_.notify_all();
        schedule_pump_retry();
    }

    static const size_t kMaxQueueBytes = 2u * 1024u * 1024u;
    // Keep the OS send queue shallow so WebSocket Pong control frames cannot
    // sit behind seconds of PCM that websocketpp already handed to the kernel.
    static const int kMaxKernelPending = 6400;
    Client client_;
    std::thread io_thread_;
    std::mutex q_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedFrame> out_queue_;
    size_t out_queue_bytes_ = 0;
    boost::asio::io_service::work* work_ = nullptr;
    Hdl hdl_;
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> pump_scheduled_{false};
    std::atomic<int> remote_close_code_{0};
    std::atomic<bool> send_failed_{false};
    std::atomic<bool> queue_overflow_reported_{false};
    std::atomic<bool> intentional_close_{false};
    std::atomic<bool> reconnect_scheduled_{false};
    std::chrono::steady_clock::time_point pong_pause_until_{};
    std::string current_url_;
    std::string current_cafile_;
};

// 工厂：按 URL scheme 选择明文/TLS 实现
inline IWsTransport* make_ws_transport(const std::string& url) {
    if (url.rfind("wss://", 0) == 0)
        return new WsTransportImpl<websocketpp::config::asio_tls_client>();
    return new WsTransportImpl<websocketpp::config::asio_client>();
}
