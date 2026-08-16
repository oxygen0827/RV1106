#include "http_client.h"
#include "util.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <cstring>
#include <sys/time.h>

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

template <typename Stream>
static bool read_response(Stream& stream, const char* protocol, HttpResponse& out) {
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaxResponseBytes);

    boost::system::error_code ec;
    http::read(stream, buffer, parser, ec);
    if (ec) {
        LOGF("%s read response: %s", protocol, ec.message().c_str());
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
        asio::ip::tcp::resolver::results_type eps = res.resolve(p.host, p.port);
        asio::connect(sock, eps);

        struct timeval tv { timeout_sec, 0 };
        setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        asio::write(sock, asio::buffer(request));
        return read_response(sock, "http", out);
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
        asio::ip::tcp::resolver::results_type eps = res.resolve(p.host, p.port);
        asio::connect(sock.lowest_layer(), eps);

        struct timeval tv { timeout_sec, 0 };
        setsockopt(sock.lowest_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock.lowest_layer().native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // SNI
        if (!SSL_set_tlsext_host_name(sock.native_handle(), p.host.c_str())) {
            LOGF("tls: set SNI failed");
            return false;
        }
        sock.handshake(asio::ssl::stream_base::client);

        asio::write(sock, asio::buffer(request));
        return read_response(sock, "https", out);
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
