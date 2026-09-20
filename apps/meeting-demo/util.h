#pragma once
// 会议纪要助手板端 demo —— 通用小工具（Base64 / 日志）
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// 标准 Base64 编码（无换行）
std::string base64_encode(const uint8_t* data, size_t len);

// 标准 Base64 解码；失败返回 false
bool base64_decode(const std::string& in, std::vector<uint8_t>& out);

// 本地时间 HH:MM:SS
inline std::string now_str() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

#define LOG_TAG "[meeting]"
#define LOGF(...)                                                                      \
    do {                                                                               \
        std::printf(LOG_TAG " " __VA_ARGS__);                                          \
        std::printf("\n");                                                             \
        std::fflush(stdout);                                                           \
    } while (0)
