#include "util.h"

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(B64[(v >> 18) & 63]);
        out.push_back(B64[(v >> 12) & 63]);
        out.push_back(B64[(v >> 6) & 63]);
        out.push_back(B64[v & 63]);
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = data[i] << 16;
        out.push_back(B64[(v >> 18) & 63]);
        out.push_back(B64[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(B64[(v >> 18) & 63]);
        out.push_back(B64[(v >> 12) & 63]);
        out.push_back(B64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = b64_val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((acc >> bits) & 0xff);
        }
    }
    return true;
}
