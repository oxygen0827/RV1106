#include "http_client.h"
#include "util.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <cstring>
#include <sys/time.h>

#include <openssl/ssl.h>
#include <sstream>

namespace asio = boost::asio;

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
        asio::streambuf buf;
        boost::system::error_code ec;
        asio::read_until(sock, buf, "\r\n\r\n", ec);
        if (ec && ec != asio::error::eof) {
            LOGF("http read header: %s", ec.message().c_str());
            return false;
        }
        std::istream is(&buf);
        std::string line, headers;
        std::getline(is, line);  // status line
        if (sscanf(line.c_str(), "HTTP/%*d.%*d %d", &out.status) != 1) return false;
        std::string h;
        while (std::getline(is, h) && h != "\r" && !h.empty()) headers += h + "\n";
        (void)headers;
        // 读到 EOF（服务器对 Connection: close 会关闭）；EINTR 重试
        std::ostringstream body;
        do {
            ec.clear();
            asio::read(sock, buf, ec);
        } while (ec == asio::error::interrupted);
        if (ec && ec != asio::error::eof && ec != asio::error::operation_aborted) {
            LOGF("http read body: %s", ec.message().c_str());
            return false;
        }
        body << &buf;
        out.body = body.str();
        return true;
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
        asio::streambuf buf;
        boost::system::error_code ec;
        asio::read_until(sock, buf, "\r\n\r\n", ec);
        if (ec && ec != asio::error::eof) {
            LOGF("https read header: %s", ec.message().c_str());
            return false;
        }
        std::istream is(&buf);
        std::string line, h;
        std::getline(is, line);
        if (sscanf(line.c_str(), "HTTP/%*d.%*d %d", &out.status) != 1) return false;
        while (std::getline(is, h) && h != "\r" && !h.empty()) {}
        std::ostringstream body;
        do {
            ec.clear();
            asio::read(sock, buf, ec);
        } while (ec == asio::error::interrupted);
        if (ec && ec != asio::error::eof && ec != asio::error::operation_aborted) {
            LOGF("https read body: %s", ec.message().c_str());
            return false;
        }
        body << &buf;
        out.body = body.str();
        return true;
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
