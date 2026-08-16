#pragma once
// ALSA 采集/播放 + MP3(minimp3) 解码 + 播放队列
#include <atomic>
#include <cstdint>

#include <alsa/asoundlib.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// 16kHz 单声道采集，每 100ms 回调一次（1600 样本）
class AudioCapture {
public:
    // cb: (int16 样本指针, 样本数)；运行在采集线程
    bool start(int rate_hz, std::function<void(const int16_t*, size_t)> cb);
    void stop();

private:
    void loop();
    snd_pcm_t* pcm_ = nullptr;
    std::thread th_;
    std::function<void(const int16_t*, size_t)> cb_;
    std::atomic<bool> run_{false};
};

// 播放队列：push 24kHz 单声道 PCM；播放线程按序写 ALSA。
// 队列有界（约 8s），满时丢弃最旧（异常兜底）。
class Playback {
public:
    bool start(int rate_hz);
    void push(const std::vector<int16_t>& pcm);
    void clear();          // 打断：丢弃未播内容
    bool queue_empty() const;
    size_t queued_bytes() const;
    void stop();

private:
    void loop();
    snd_pcm_t* pcm_ = nullptr;
    std::thread th_;
    std::atomic<bool> run_{false};
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<int16_t>> q_;
    size_t queued_bytes_ = 0;
};

// MP3 解码（minimp3，公有领域单头库）。输出 24kHz 单声道 s16。
class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();
    // 一次喂入一段完整 MP3（一个 answer_audio 消息）；解码后回调输出 PCM 块
    bool decode(const uint8_t* data, size_t len,
                const std::function<void(const int16_t*, size_t)>& out_cb);

private:
    void* dec_ = nullptr;  // mp3dec_t*
};
