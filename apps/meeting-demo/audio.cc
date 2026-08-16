#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "audio.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <thread>

// ---------------- AudioCapture ----------------

bool AudioCapture::start(int rate_hz, std::function<void(const int16_t*, size_t)> cb) {
    cb_ = std::move(cb);
    int err = snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        LOGF("capture open: %s", snd_strerror(err));
        return false;
    }
    snd_pcm_hw_params_t* hp;
    snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(pcm_, hp);
    snd_pcm_hw_params_set_access(pcm_, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, hp, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, hp, 1);
    unsigned int r = rate_hz;
    snd_pcm_hw_params_set_rate_near(pcm_, hp, &r, nullptr);
    unsigned int period_us = 100000, buffer_us = 400000;
    snd_pcm_hw_params_set_period_time_near(pcm_, hp, &period_us, nullptr);
    snd_pcm_hw_params_set_buffer_time_near(pcm_, hp, &buffer_us, nullptr);
    if ((err = snd_pcm_hw_params(pcm_, hp)) < 0) {
        LOGF("capture hw_params: %s", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }
    LOGF("capture started: %u Hz mono S16_LE", r);
    run_.store(true);
    th_ = std::thread(&AudioCapture::loop, this);
    return true;
}

void AudioCapture::loop() {
    const size_t FRAMES = 1600;  // 100ms @16kHz
    std::vector<int16_t> buf(FRAMES);
    while (run_.load()) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), FRAMES);
        if (n < 0) {
            n = snd_pcm_recover(pcm_, (int)n, 1);
            if (n < 0) {
                LOGF("capture read: %s", snd_strerror((int)n));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        if (n > 0 && cb_) cb_(buf.data(), (size_t)n);
    }
}

void AudioCapture::stop() {
    run_.store(false);
    if (th_.joinable()) th_.join();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

// ---------------- Playback ----------------

bool Playback::start(int rate_hz) {
    int err = snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        LOGF("playback open: %s", snd_strerror(err));
        return false;
    }
    snd_pcm_hw_params_t* hp;
    snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(pcm_, hp);
    snd_pcm_hw_params_set_access(pcm_, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, hp, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, hp, 1);
    unsigned int r = rate_hz;
    snd_pcm_hw_params_set_rate_near(pcm_, hp, &r, nullptr);
    unsigned int period_us = 40000, buffer_us = 200000;
    snd_pcm_hw_params_set_period_time_near(pcm_, hp, &period_us, nullptr);
    snd_pcm_hw_params_set_buffer_time_near(pcm_, hp, &buffer_us, nullptr);
    if ((err = snd_pcm_hw_params(pcm_, hp)) < 0) {
        LOGF("playback hw_params: %s", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }
    LOGF("playback started: %u Hz mono S16_LE", r);
    run_.store(true);
    th_ = std::thread(&Playback::loop, this);
    return true;
}

void Playback::push(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(pcm);
        queued_bytes_ += pcm.size() * sizeof(int16_t);
        while (queued_bytes_ > 8 * 24000 * 2) {  // 上限 8 秒
            queued_bytes_ -= q_.front().size() * sizeof(int16_t);
            q_.pop_front();
        }
    }
    cv_.notify_one();
}

void Playback::clear() {
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
    queued_bytes_ = 0;
}

bool Playback::queue_empty() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.empty();
}

size_t Playback::queued_bytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return queued_bytes_;
}

void Playback::loop() {
    while (run_.load()) {
        std::vector<int16_t> chunk;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(100), [this]() { return !q_.empty() || !run_.load(); });
            if (!run_.load()) break;
            if (q_.empty()) continue;
            chunk.swap(q_.front());
            q_.pop_front();
            queued_bytes_ -= chunk.size() * sizeof(int16_t);
        }
        // 边收边播：按 40ms 节拍写
        size_t off = 0;
        while (off < chunk.size() && run_.load()) {
            size_t step = 960;  // 40ms @24kHz
            if (off + step > chunk.size()) step = chunk.size() - off;
            snd_pcm_sframes_t n = snd_pcm_writei(pcm_, chunk.data() + off, step);
            if (n < 0) {
                n = snd_pcm_recover(pcm_, (int)n, 1);
                if (n < 0) {
                    LOGF("playback write: %s", snd_strerror((int)n));
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                continue;
            }
            off += n;
        }
    }
}

void Playback::stop() {
    run_.store(false);
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    if (pcm_) {
        snd_pcm_drain(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

// ---------------- Mp3Decoder ----------------

Mp3Decoder::Mp3Decoder() {
    dec_ = new mp3dec_t;
    mp3dec_init(static_cast<mp3dec_t*>(dec_));
}

Mp3Decoder::~Mp3Decoder() {
    delete static_cast<mp3dec_t*>(dec_);
    dec_ = nullptr;
}

bool Mp3Decoder::decode(const uint8_t* data, size_t len,
                        const std::function<void(const int16_t*, size_t)>& out_cb) {
    mp3dec_t* dec = static_cast<mp3dec_t*>(dec_);
    mp3dec_frame_info_t info;
    size_t off = 0;
    while (off < len) {
        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(dec, data + off, len - off, pcm, &info);
        if (samples <= 0) break;  // 结束或错误
        off += info.frame_bytes;
        if (info.channels == 2) {  // 双声道混为单声道
            for (int i = 0; i < samples; i++)
                pcm[i] = (int16_t)(((int)pcm[2 * i] + (int)pcm[2 * i + 1]) / 2);
        }
        if (out_cb) out_cb(pcm, (size_t)samples);
    }
    return true;
}
