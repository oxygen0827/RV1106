#include "http_client.h"
#include "util.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <cstring>
#include <chrono>
#include <functional>

#include <openssl/ssl.h>
#include <sstream>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

static const std::uint64_t kMaxResponseBytes = 8u * 1024u * 1024u;

struct UrlParts {
    bool tls = false;
    std::string host;
    std::string port;
    std::string path;
};

static bool parse_url(const std::string& url, UrlParts& p) {
    const char* s = url.c_str();
    if (strncmp(s, "https://", 8) == 0) { p.tls = true; s += 8; }
    else if (strncmp(s, "http://", 7) == 0) { p.tls = false; s += 7; }
    else return false;
    const char* slash = strchr(s, '/');
    std::string hostport(s, slash ? (size_t)(slash - s) : strlen(s));
    p.path = slash ? slash : "/";
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(':') == colon) {
        p.host = hostport.substr(0, colon);
        p.port = hostport.substr(colon + 1);
    } else {
        p.host = hostport;
        p.port = p.tls ? "443" : "80";
    }
    return !p.host.empty();
}

class AsyncDeadline {
public:
    using Completion = std::function<void(boost::system::error_code)>;

    AsyncDeadline(asio::io_context& io, int timeout_sec,
                  std::function<void()> cancel)
        : io_(io), timer_(io), cancel_(std::move(cancel)),
          expires_at_(std::chrono::steady_clock::now() +
                      std::chrono::seconds(timeout_sec)) {
        timer_.expires_at(expires_at_);
        timer_.async_wait([this](const boost::system::error_code& ec) {
            if (ec) return;
            expired_ = true;
            cancel_();
        });
    }

    ~AsyncDeadline() {
        boost::system::error_code ignored;
        timer_.cancel(ignored);
        while (io_.poll_one() != 0) {}
    }

    template <typename Start>
    bool run(Start start, boost::system::error_code& operation_ec) {
        operation_ec.clear();
        if (std::chrono::steady_clock::now() >= expires_at_) {
            expired_ = true;
            cancel_();
            return false;
        }
        bool done = false;
        io_.restart();
        start(Completion([&](boost::system::error_code ec) {
            operation_ec = ec;
            done = true;
        }));
        while (!done && !expired_) io_.run_one();
        while (!done) io_.run_one();  // drain the cancellation completion
        if (std::chrono::steady_clock::now() >= expires_at_) expired_ = true;
        return !expired_ && !operation_ec;
    }

    bool expired() const { return expired_; }

private:
    asio::io_context& io_;
    asio::steady_timer timer_;
    std::function<void()> cancel_;
    std::chrono::steady_clock::time_point expires_at_;
    bool expired_ = false;
};

template <typename Stream>
static bool read_response(Stream& stream, AsyncDeadline& deadline,
                          const char* protocol, HttpResponse& out) {
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaxResponseBytes);

    boost::system::error_code ec;
    bool ok = deadline.run([&](const AsyncDeadline::Completion& done) {
        http::async_read(stream, buffer, parser,
                         [done](boost::system::error_code read_ec, std::size_t) {
                             done(read_ec);
                         });
    }, ec);
    if (!ok) {
        if (deadline.expired()) LOGF("%s request timed out", protocol);
        else LOGF("%s read response: %s", protocol, ec.message().c_str());
        return false;
    }

    const http::response<http::string_body>& response = parser.get();
    out.status = response.result_int();
    out.body = response.body();
    return true;
}

static bool do_plain(const UrlParts& p, const std::string& request, int timeout_sec,
                     HttpResponse& out) {
    try {
        asio::io_context io;
        asio::ip::tcp::resolver res(io);
        asio::ip::tcp::socket sock(io);
        AsyncDeadline deadline(io, timeout_sec, [&res, &sock]() {
            boost::system::error_code ignored;
            res.cancel();
            sock.close(ignored);
        });
        boost::system::error_code ec;
        asio::ip::tcp::resolver::results_type eps;
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                res.async_resolve(
                    asio::ip::tcp::v4(), p.host, p.port,
                    [&, done](boost::system::error_code resolve_ec,
                              asio::ip::tcp::resolver::results_type results) {
                        if (!resolve_ec) eps = std::move(results);
                        done(resolve_ec);
                    });
            }, ec)) {
            LOGF("http resolve: %s", deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                asio::async_connect(
                    sock, eps,
                    [done](boost::system::error_code connect_ec,
                           const asio::ip::tcp::endpoint&) { done(connect_ec); });
            }, ec)) {
            LOGF("http connect: %s", deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                asio::async_write(
                    sock, asio::buffer(request),
                    [done](boost::system::error_code write_ec, std::size_t) {
                        done(write_ec);
                    });
            }, ec)) {
            LOGF("http write: %s", deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }
        return read_response(sock, deadline, "http", out);
    } catch (const std::exception& e) {
        LOGF("http error: %s", e.what());
        return false;
    }
}

static bool do_tls(const UrlParts& p, const std::string& request, const std::string& cafile,
                   int timeout_sec, HttpResponse& out) {
    try {
        asio::io_context io;
        asio::ssl::context ctx(asio::ssl::context::tls_client);
        if (!cafile.empty()) {
            ctx.load_verify_file(cafile);
        } else {
            ctx.set_default_verify_paths();
        }
        ctx.set_verify_mode(asio::ssl::verify_peer);

        asio::ip::tcp::resolver res(io);
        asio::ssl::stream<asio::ip::tcp::socket> sock(io, ctx);
        AsyncDeadline deadline(io, timeout_sec, [&res, &sock]() {
            boost::system::error_code ignored;
            res.cancel();
            sock.lowest_layer().close(ignored);
        });
        boost::system::error_code ec;
        asio::ip::tcp::resolver::results_type eps;
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                res.async_resolve(
                    asio::ip::tcp::v4(), p.host, p.port,
                    [&, done](boost::system::error_code resolve_ec,
                              asio::ip::tcp::resolver::results_type results) {
                        if (!resolve_ec) eps = std::move(results);
                        done(resolve_ec);
                    });
            }, ec)) {
            LOGF("https resolve: %s",
                 deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                asio::async_connect(
                    sock.lowest_layer(), eps,
                    [done](boost::system::error_code connect_ec,
                           const asio::ip::tcp::endpoint&) { done(connect_ec); });
            }, ec)) {
            LOGF("https connect: %s", deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }

        if (!SSL_set_tlsext_host_name(sock.native_handle(), p.host.c_str())) {
            LOGF("tls: set SNI failed");
            return false;
        }
        sock.set_verify_callback(asio::ssl::host_name_verification(p.host));
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                sock.async_handshake(
                    asio::ssl::stream_base::client,
                    [done](boost::system::error_code handshake_ec) {
                        done(handshake_ec);
                    });
            }, ec)) {
            LOGF("https handshake: %s",
                 deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }
        if (!deadline.run([&](const AsyncDeadline::Completion& done) {
                asio::async_write(
                    sock, asio::buffer(request),
                    [done](boost::system::error_code write_ec, std::size_t) {
                        done(write_ec);
                    });
            }, ec)) {
            LOGF("https write: %s", deadline.expired() ? "timeout" : ec.message().c_str());
            return false;
        }
        return read_response(sock, deadline, "https", out);
    } catch (const std::exception& e) {
        LOGF("https error: %s", e.what());
        return false;
    }
}

bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& cafile,
                  int timeout_sec, HttpResponse& out) {
    UrlParts p;
    if (!parse_url(url, p)) {
        LOGF("bad url: %s", url.c_str());
        return false;
    }
    std::ostringstream req;
    req << method << " " << p.path << " HTTP/1.1\r\n"
        << "Host: " << p.host << "\r\n"
        << "User-Agent: meeting-demo/1.0\r\n"
        << "Accept: application/json\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;

    return p.tls ? do_tls(p, req.str(), cafile, timeout_sec, out)
                 : do_plain(p, req.str(), timeout_sec, out);
}
