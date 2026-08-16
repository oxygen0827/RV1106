// 会议纪要助手板端 demo（Echo-Mate RV1106）
//
// 链路：ALSA 采集 16kHz/mono/s16 PCM（100ms 帧）
//       → binary PCM → /ws/transcribe；Base64 + JSON → /ws/host
//       ← transcription / answer_text（流式）/ answer_audio（24kHz MP3，逐句）
//       → minimp3 软解 → 播放队列 → ALSA 24kHz 边收边播
//
// 传输层抽象：WsTransport（本文件之上不再关心 WebSocket 细节），
// 将来 RTC 路线 = 新增 RtcTransport 实现同一回调面。
//
// 用法：
//   meeting_demo --server ws://HOST:PORT [--topic 名称] [--mode listen|host|full]
//                [--cafile /root/bin/cacert.pem] [--duplex-upload 0|1]
//                [--auto-host-every N] [--record FILE]
//
// 控制台（host/full 模式）：
//   Enter  按住提问 / 再按结束提问（send end_of_speech）
//   s/S    打断当前回答（send stop）
//   q     退出（结束会议并清理）
// listen 模式：q 退出。

#include <json/json.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "audio.h"
#include "http_client.h"
#include "util.h"
#include "wss_transport.h"

static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_asking{false};     // 正在录音提问
static std::atomic<bool> g_answering{false};  // 提问后、done 前的回答阶段

static void on_signal(int) { g_quit.store(true); }

static bool parse_json_object(const std::string& body, Json::Value& out) {
    Json::CharReaderBuilder rb;
    std::string errors;
    std::istringstream input(body);
    return Json::parseFromStream(rb, input, &out, &errors) && out.isObject();
}

static std::string one_line(std::string value) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    return value;
}

static bool write_file_atomic(const std::string& path, const std::string& data) {
    if (path.empty()) return true;
    std::string tmp = path + ".tmp." + std::to_string((long long)getpid());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    (void)::fchmod(fd, 0600);
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        written += (size_t)n;
    }
    bool ok = (::fsync(fd) == 0);
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

static void log_final_minutes(const Json::Value& snapshot) {
    const Json::Value& analysis = snapshot["analysis"];
    const Json::Value& overview = snapshot["overview"];
    const Json::Value& understanding = snapshot["understanding"];
    std::string state = analysis.get("state", "unknown").asString();
    long long revision = snapshot.get("revision", 0).asInt64();
    long long transcripts = snapshot.get("transcriptCount", 0).asInt64();
    LOGF("[纪要完成] state=%s revision=%lld transcripts=%lld",
         state.c_str(), revision, transcripts);

    std::string headline = one_line(overview.get("headline", "暂无会议概要").asString());
    LOGF("[概要] %s", headline.c_str());
    std::string goal = one_line(understanding.get("meetingGoal", "").asString());
    LOGF("[目标] %s", goal.empty() ? "暂未明确" : goal.c_str());

    const Json::Value& topics = understanding["topics"];
    if (!topics.isArray() || topics.empty()) {
        LOGF("[议题] 暂无可归纳内容");
        LOGF("[结论] 暂无明确结论");
        LOGF("[待办] 暂无待办");
        return;
    }
    for (Json::ArrayIndex i = 0; i < topics.size(); ++i) {
        const Json::Value& topic = topics[i];
        std::string title = one_line(topic.get("title", "未命名议题").asString());
        LOGF("[议题%u] %s", (unsigned)(i + 1), title.c_str());

        const Json::Value& consensus = topic["consensus"];
        bool has_consensus = false;
        if (consensus.isArray()) {
            for (Json::ArrayIndex j = 0; j < consensus.size(); ++j) {
                std::string text = one_line(consensus[j].asString());
                if (!text.empty()) {
                    has_consensus = true;
                    LOGF("[结论] %s", text.c_str());
                }
            }
        }
        if (!has_consensus) LOGF("[结论] 暂无明确结论");
        const Json::Value& todos = topic["todos"];
        bool has_todo = false;
        if (todos.isArray()) {
            for (Json::ArrayIndex j = 0; j < todos.size(); ++j) {
                const Json::Value& todo = todos[j];
                std::string owner = one_line(todo.get("owner", "待认领").asString());
                std::string action = one_line(todo.get("action", "").asString());
                std::string deadline = one_line(todo.get("deadline", "").asString());
                if (action.empty()) continue;
                has_todo = true;
                if (deadline.empty()) LOGF("[待办] %s：%s", owner.c_str(), action.c_str());
                else LOGF("[待办] %s：%s（%s）", owner.c_str(), action.c_str(), deadline.c_str());
            }
        }
        if (!has_todo) LOGF("[待办] 暂无待办");
    }
}

static bool transcript_snapshot_complete(const Json::Value& snapshot) {
    const Json::Value& transcript = snapshot["transcript"];
    if (!transcript.isArray() || !snapshot["count"].isIntegral() ||
        snapshot["count"].asUInt64() != transcript.size()) {
        return false;
    }
    for (const Json::Value& item : transcript) {
        if (!item.isObject() || !item["text"].isString() ||
            item.get("is_final", false).asBool() == false) {
            return false;
        }
    }
    return true;
}

static bool understanding_snapshot_final(const Json::Value& snapshot) {
    return snapshot["analysis"].isObject() &&
           snapshot["analysis"].get("state", "").asString() == "final" &&
           snapshot["overview"].isObject() && snapshot["understanding"].isObject();
}

static bool count_equals(const Json::Value& value, Json::UInt64 expected) {
    return value.isIntegral() && value.asUInt64() == expected;
}

static bool completion_state_matches(const Json::Value& snapshot, Json::UInt64 count) {
    auto matches = [count](const Json::Value& state) {
        return state.isObject() && count_equals(state["transcriptCount"], count) &&
               count_equals(state["committedTranscriptCount"], count) &&
               count_equals(state["cursor"], count) &&
               count_equals(state["pendingTranscriptCount"], 0) &&
               state.get("sourceComplete", false).asBool() &&
               state.get("analysisComplete", false).asBool();
    };
    return matches(snapshot) && matches(snapshot["transcriptState"]);
}

static bool audio_backlog_empty(const Json::Value& snapshot) {
    const Json::Value& backlog = snapshot["audioBacklog"];
    return backlog.isObject() && count_equals(backlog["queued_ms"], 0) &&
           count_equals(backlog["dropped_ms"], 0) &&
           count_equals(backlog["dropped_frames"], 0);
}

static bool final_snapshots_consistent(const std::string& session_id,
                                       const Json::Value& end,
                                       const Json::Value& transcript,
                                       const Json::Value& understanding,
                                       std::string& reason) {
    const Json::UInt64 count = transcript["count"].asUInt64();
    const Json::Value& analysis = understanding["analysis"];
    const Json::Value& decision = understanding["decisionState"];
    auto fail = [&](const char* message) {
        reason = message;
        return false;
    };

    if (end.get("status", "").asString() != "ended")
        return fail("结束接口状态不是 ended");
    if (!count_equals(end["transcript_count"], count))
        return fail("结束接口与最终转写条数不一致");
    if (transcript.get("session_id", "").asString() != session_id ||
        understanding.get("sessionId", "").asString() != session_id)
        return fail("最终快照 Session 不一致");
    if (understanding.get("status", "").asString() != "ended")
        return fail("理解快照尚未结束");
    if (!completion_state_matches(end, count) ||
        !completion_state_matches(transcript, count) ||
        !completion_state_matches(understanding, count))
        return fail("服务端完整性状态未全部确认");
    if (!count_equals(analysis["lastSuccessfulCursor"], count) ||
        !count_equals(analysis["pendingTranscriptCount"], 0) ||
        !decision.isObject() || !count_equals(decision["cursor"], count))
        return fail("最终理解没有追平全部转写");
    if (!audio_backlog_empty(end) || !audio_backlog_empty(transcript) ||
        !audio_backlog_empty(understanding))
        return fail("服务端音频积压未清空或发生丢帧");
    return true;
}

static void log_unseen_final_transcripts(const Json::Value& snapshot,
                                         const std::vector<std::string>& streamed_texts) {
    const Json::Value& transcript = snapshot["transcript"];
    std::vector<bool> consumed(streamed_texts.size(), false);
    for (const Json::Value& item : transcript) {
        if (item.get("is_final", false).asBool() == false) continue;
        const std::string text = item.get("text", "").asString();
        bool already_streamed = false;
        for (size_t i = 0; i < streamed_texts.size(); ++i) {
            if (!consumed[i] && streamed_texts[i] == text) {
                consumed[i] = true;
                already_streamed = true;
                break;
            }
        }
        if (already_streamed) continue;
        LOGF("[转写] %s: %s", item.get("speaker", "说话人").asString().c_str(),
             text.c_str());
    }
}

static Json::Value audio_frame_msg(const int16_t* pcm, size_t n) {
    Json::Value f;
    f["type"] = "audio";
    f["data"] = base64_encode(reinterpret_cast<const uint8_t*>(pcm), n * sizeof(int16_t));
    return f;
}

int main(int argc, char** argv) {
    std::string server = "wss://clare.vinex.top/voice-api";
    std::string topic = "会议纪要助手demo";
    std::string mode = "listen";
    std::string cafile = "/root/bin/cacert.pem";
    int duplex_upload = 0;      // 1 = 播放回答期间仍上行转写（全双工）；0 = 半双工时序（默认）
    int auto_host_every = 0;    // >0 时每 N 秒自动发起一轮问答（无人值守演示）
    int vad_enable = 1;         // 转写上行静音门控（VAD）：只传有声帧，省 80%+ 带宽
    int vad_threshold = 0;      // VAD 帧 RMS 阈值；0 = 自适应（2.5×噪声底，下限 140）
    std::string inject_file;   // 非空：从 16kHz s16 文件喂帧（替代麦克风，联调诊断用）
    std::string record_file = "/root/meeting_demo/latest-meeting.json";
    int debug_log = 0;         // 1 = 打印每 50 帧时序等诊断日志（默认关）

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { LOGF("missing value for %s", name); exit(2); }
            return argv[++i];
        };
        if (a == "--server") server = next("--server");
        else if (a == "--topic") topic = next("--topic");
        else if (a == "--mode") mode = next("--mode");
        else if (a == "--cafile") cafile = next("--cafile");
        else if (a == "--duplex-upload") duplex_upload = atoi(next("--duplex-upload").c_str());
        else if (a == "--auto-host-every") auto_host_every = atoi(next("--auto-host-every").c_str());
        else if (a == "--vad") vad_enable = atoi(next("--vad").c_str());
        else if (a == "--vad-threshold") vad_threshold = atoi(next("--vad-threshold").c_str());
        else if (a == "--inject") inject_file = next("--inject");
        else if (a == "--record") record_file = next("--record");
        else if (a == "--debug") debug_log = 1;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: meeting_demo --server URL [--topic T] [--mode listen|host|full] "
                        "[--cafile P] [--duplex-upload 0|1] [--auto-host-every N] "
                        "[--record FILE]\n");
            return 0;
        } else {
            LOGF("unknown arg: %s", a.c_str());
            return 2;
        }
    }
    if (mode != "listen" && mode != "host" && mode != "full") {
        LOGF("bad mode: %s (listen|host|full)", mode.c_str());
        return 2;
    }
    const bool use_transcribe = (mode == "listen" || mode == "full");
    const bool use_host = (mode == "host" || mode == "full");
    const bool use_understanding = use_transcribe;

    // HTTP base：由 WS URL 换 scheme 得到（真实后端 http(s) 与 ws(s) 同源）
    std::string http_base = server;
    if (http_base.rfind("wss://", 0) == 0) http_base.replace(0, 3, "https");
    else if (http_base.rfind("ws://", 0) == 0) http_base.replace(0, 2, "http");
    else { LOGF("--server must start with ws:// or wss://"); return 2; }

    LOGF("=== 会议纪要助手板端 demo ===");
    LOGF("server=%s mode=%s duplex_upload=%d auto_host_every=%d", server.c_str(),
         mode.c_str(), duplex_upload, auto_host_every);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- 1. 创建会议 ----
    Json::Value req;
    req["topic"] = topic;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    HttpResponse resp;
    if (!http_request("POST", http_base + "/api/session", Json::writeString(w, req), cafile, 10,
                      resp) ||
        resp.status != 200) {
        LOGF("create session failed: status=%d body=%s", resp.status, resp.body.c_str());
        return 1;
    }
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream is(resp.body);
    Json::parseFromStream(rb, is, &v, &errs);
    std::string session_id = v.get("session_id", "").asString();
    if (session_id.empty()) {
        LOGF("no session_id in response: %s", resp.body.c_str());
        return 1;
    }
    LOGF("session created: %s topic=%s", session_id.c_str(), topic.c_str());

    // ---- 2. 音频：采集 + 播放 ----
    Playback playback;
    auto abort_session = [&](const char* reason) -> int {
        LOGF("startup failed: %s", reason);
        g_quit.store(true);
        HttpResponse endr;
        http_request("POST", http_base + "/api/session/" + session_id + "/end",
                     "", cafile, 10, endr);
        LOGF("session %s aborted: status=%d", session_id.c_str(), endr.status);
        return 1;
    };
    // 基础转写 (listen) 不依赖播放设备，避免 host 旁路阻断建会。
    if (use_host && !playback.start(24000)) return abort_session("playback init failed");
    AudioCapture capture;
    Mp3Decoder mp3;

    // ---- 3. WebSocket 传输（工厂按 scheme 选明文/TLS 实现）----
    std::unique_ptr<IWsTransport> ws_tr(make_ws_transport(server));
    std::unique_ptr<IWsTransport> ws_ho(make_ws_transport(server));
    std::mutex streamed_finals_mutex;
    std::vector<std::string> streamed_final_texts;

    auto attach_handlers = [&](IWsTransport& wst, const char* name, bool is_host) {
        wst.on_open = [name]() { LOGF("[%s] open", name); };
        wst.on_fail = [name](const std::string& e) { LOGF("[%s] fail: %s", name, e.c_str()); };
        wst.on_close = [name](int code, const std::string& reason) {
            const char* hint = code == 4009 ? "(同 Session 已有转写发布者)"
                              : code == 4500 ? "(服务端 ASR 建连失败，可重连)"
                              : code == 4004 ? "(Session 不存在或已结束)" : "";
            LOGF("[%s] closed %d %s%s", name, code, reason.c_str(), hint);
        };
        if (!is_host) {
            wst.on_message = [&](const Json::Value& m) {
                if (!m.isObject()) return;
                std::string t = m.get("type", "").asString();
                if (t == "transcript") {
                    bool is_final = m.get("is_final", true).asBool();
                    if (is_final) {
                        std::lock_guard<std::mutex> lock(streamed_finals_mutex);
                        streamed_final_texts.push_back(m.get("text", "").asString());
                    }
                    LOGF("[转写%s] %s: %s", is_final ? "" : "-中间",
                         m.get("speaker", "说话人").asString().c_str(),
                         m.get("text", "").asString().c_str());
                }
            };
            // 转写通道断线自动重连（服务端 keepalive ping 超时/网络抖动时保会议）
            wst.on_close = [&, name](int code, const std::string& reason) {
                if (g_quit.load()) return;
                if (code == 4004) {
                    LOGF("[%s] session 已结束，不再重连", name);
                    return;
                }
                LOGF("[%s] 断线 (%d %s)，2s 后重连...", name, code, reason.c_str());
                std::thread([&]() {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    if (g_quit.load()) return;
                    static int attempt = 0;
                    LOGF("[%s] 重连尝试 #%d", name, ++attempt);
                    wst.connect(server + "/ws/transcribe/" + session_id, cafile);
                }).detach();
            };
            return;
        }
        wst.on_message = [&](const Json::Value& m) {
            if (!m.isObject()) return;
            std::string t = m.get("type", "").asString();
            if (t == "transcription") {
                LOGF("[host] Q: %s", m.get("text", "").asString().c_str());
                g_answering.store(true);
            } else if (t == "answer_text") {
                std::string txt = m.get("text", "").asString();
                if (!txt.empty()) std::printf("%s", txt.c_str()), std::fflush(stdout);
                if (m.get("done", false).asBool()) {
                    std::printf("\n");
                    std::fflush(stdout);
                }
            } else if (t == "answer_audio") {
                std::vector<uint8_t> mp3_bytes;
                if (base64_decode(m.get("data", "").asString(), mp3_bytes) && !mp3_bytes.empty()) {
                    mp3.decode(mp3_bytes.data(), mp3_bytes.size(),
                               [&](const int16_t* pcm, size_t n) {
                                   playback.push(std::vector<int16_t>(pcm, pcm + n));
                               });
                } else {
                    LOGF("[host] bad answer_audio payload");
                }
            } else if (t == "done") {
                g_answering.store(false);
                LOGF("[host] <answer done>");
            } else if (t == "ping") {
                // 服务端保活，无需回应
            } else if (t == "error") {
                g_answering.store(false);
                playback.clear();
                LOGF("[host] server error: %s", m.get("message", "").asString().c_str());
            } else {
                LOGF("[host] unknown msg type: %s", t.c_str());
            }
        };
    };

    auto abort_startup = [&](const char* reason) -> int {
        playback.stop();
        return abort_session(reason);
    };

    if (use_transcribe) {
        attach_handlers(*ws_tr, "transcribe", false);
        ws_tr->start();
        if (!ws_tr->connect(server + "/ws/transcribe/" + session_id, cafile)) {
            return abort_startup("transcribe connect failed");
        }
    }
    if (use_host) {
        attach_handlers(*ws_ho, "host", true);
        ws_ho->start();
        if (!ws_ho->connect(server + "/ws/host/" + session_id, cafile)) {
            return abort_startup("host connect failed");
        }
    }

    // ---- 4. 采集 → 上行 ----
    // 所需通道全部握手完成后才开采集，避免 send 在 !open 时被静默丢弃。
    auto required_ws_open = [&]() {
        return (!use_transcribe || ws_tr->is_open()) && (!use_host || ws_ho->is_open());
    };
    for (int i = 0; i < 250 && !g_quit.load() && !required_ws_open(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!required_ws_open()) {
        return abort_startup("required WebSocket handshake timed out");
    }

    // 服务端在进入收帧循环前已完成 ASR connect。留 2 秒预热时间，再发一帧
    // 静音作为首帧；不要发送 2 秒连续 PCM，否则弱网上行会积压 64KB 静音。
    if (use_transcribe) {
        LOGF("等待 2 秒让服务端 ASR 预热...");
        for (int i = 0; i < 20 && !g_quit.load() && required_ws_open(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!required_ws_open() || g_quit.load()) {
            return abort_startup("WebSocket closed during ASR warmup");
        }
        std::vector<int16_t> sil(1600, 0);
        ws_tr->send_binary(reinterpret_cast<const uint8_t*>(sil.data()),
                           sil.size() * sizeof(int16_t));
    }
    // 半双工时序：回答播放期间暂停转写上行，避免喇叭回声污染 ASR（议题2 默认方案）
    auto uplink_paused = [&]() {
        return !duplex_upload && (g_answering.load() || !playback.queue_empty());
    };

    std::atomic<int> frame_seq{0};
    // 采集/注入共用的帧回调
    // VAD 状态：前视环（最近 8 帧 / 800ms）+ 拖尾（有声后延续 3s）
    struct VadState {
        std::vector<int16_t> ring[8];
        int ring_fill = 0, ring_head = 0;
        int hangover = 0;
        bool in_speech = false;
    } vad;
    // 采集/注入共用的帧回调（VAD + 上行分派）
    std::function<void(const int16_t*, size_t)> cb_inject = [&, vad](const int16_t* pcm, size_t n) mutable {
            int seq = frame_seq.fetch_add(1) + 1;
            auto t0 = std::chrono::steady_clock::now();
            // 帧能量（RMS）
            double sum = 0;
            for (size_t i = 0; i < n; i++) sum += (double)pcm[i] * pcm[i];
            int rms = (int)std::sqrt(sum / n);
            // 自适应阈值：静音帧 EMA 估计噪声底，阈值 = max(140, 2.5×底)
            static double noise_floor = 100.0;
            int thr = vad_threshold;
            if (thr <= 0) {
                bool loud = (rms > noise_floor * 2.5 && rms > 140);
                if (!loud) noise_floor = 0.9 * noise_floor + 0.1 * rms;
                thr = (int)std::max(140.0, 2.5 * noise_floor);
            }
            bool voice = (rms >= thr);
            if (debug_log && (seq % 100 == 0 || voice))
                LOGF("rms=%d thr=%d floor=%.0f voice=%d", rms, thr, noise_floor, voice ? 1 : 0);

            auto send_tr = [&](const int16_t* p, size_t len) {
                if (use_transcribe && !uplink_paused())
                    ws_tr->send_binary(reinterpret_cast<const uint8_t*>(p),
                                       len * sizeof(int16_t));
            };

            if (use_transcribe) {
                if (vad_enable) {
                    if (voice) {
                        if (!vad.in_speech) {  // 起始：冲掉前视环
                            for (int i = 0; i < vad.ring_fill; i++) {
                                auto& f = vad.ring[(vad.ring_head + i) % 8];
                                send_tr(f.data(), f.size());
                            }
                            vad.ring_fill = 0;
                            vad.ring_head = 0;
                            vad.in_speech = true;
                            LOGF("VAD: 语音起始 @frame#%d (rms=%d)", seq, rms);
                        }
                        vad.hangover = 30;
                        send_tr(pcm, n);
                    } else {
                        if (vad.in_speech && vad.hangover > 0) {
                            vad.hangover--;
                            send_tr(pcm, n);
                            if (vad.hangover == 0) {
                                vad.in_speech = false;
                                LOGF("VAD: 语音结束 @frame#%d", seq);
                            }
                        } else {
                            vad.in_speech = false;
                            // ring_head 始终指向最旧帧；未满时追加，满后覆盖并滚动。
                            if (vad.ring_fill < 8) {
                                int tail = (vad.ring_head + vad.ring_fill) % 8;
                                vad.ring[tail].assign(pcm, pcm + n);
                                vad.ring_fill++;
                            } else {
                                vad.ring[vad.ring_head].assign(pcm, pcm + n);
                                vad.ring_head = (vad.ring_head + 1) % 8;
                            }
                        }
                    }
                } else {
                    send_tr(pcm, n);
                }
                // 静音保活：无语音时每 1s 发一帧，防止服务端空闲断开
                static auto last_silence = std::chrono::steady_clock::now();
                if (!voice && !vad.in_speech) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_silence >= std::chrono::seconds(1)) {
                        last_silence = now;
                        send_tr(pcm, n);
                    }
                }
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            if (debug_log && (seq <= 3 || seq % 50 == 0))
                LOGF("capture frame #%d sent in %lldms", seq, (long long)ms);
            // 环境噪声提示（30s 一次，常显）：便于现场判断麦克风音量/阈值
            static int last_noise_seq = -300;
            if (seq - last_noise_seq >= 300) {
                last_noise_seq = seq;
                LOGF("环境噪声 rms~%d (语音触发值 %d)，说话应超过此值", rms, thr);
            }
            if (use_host && g_asking.load()) {
                Json::Value f = audio_frame_msg(pcm, n);
                ws_ho->send_json(f);
            }
    };

    // 文件注入模式（联调诊断）：从 16kHz s16 文件按 100ms 节奏喂帧
    std::thread inject_thread;
    if (!inject_file.empty()) {
        LOGF("inject mode: %s (麦克风采集停用)", inject_file.c_str());
        inject_thread = std::thread([&]() {
            FILE* fp = std::fopen(inject_file.c_str(), "rb");
            if (!fp) {
                LOGF("inject: cannot open %s", inject_file.c_str());
                return;
            }
            std::vector<int16_t> chunk(1600);
            while (!g_quit.load()) {
                size_t got = std::fread(chunk.data(), sizeof(int16_t), 1600, fp);
                if (got < 1600) {
                    std::fseek(fp, 0, SEEK_SET);  // 循环播放
                    got = std::fread(chunk.data(), sizeof(int16_t), 1600, fp);
                    if (got < 1600) break;
                }
                cb_inject(chunk.data(), got);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::fclose(fp);
        });
        LOGF("inject thread started (Ctrl+C/q 退出)");
    } else if (!capture.start(16000, cb_inject)) {
        return abort_startup("capture init failed");
    }

    // ---- 5. 控制台 + 自动演示 ----
    LOGF("ready. 控制台: Enter=按住提问/结束提问  s=打断  q=退出");
    if (auto_host_every > 0)
        LOGF("自动演示: 每 %d 秒自动提问一轮", auto_host_every);

    std::thread auto_thread;
    if (auto_host_every > 0 && use_host) {
        auto_thread = std::thread([&]() {
            auto next_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!g_quit.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                auto now = std::chrono::steady_clock::now();
                if (now < next_at) continue;
                next_at = now + std::chrono::seconds(auto_host_every);
                if (g_asking.load() || g_answering.load()) continue;
                LOGF("[auto] 开始提问（4 秒）...");
                g_asking.store(true);
                for (int i = 0; i < 40 && !g_quit.load(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_quit.load()) break;
                g_asking.store(false);
                Json::Value m;
                m["type"] = "end_of_speech";
                ws_ho->send_json(m);
                LOGF("[auto] end_of_speech 已发送");
            }
        });
    }

    // 用 poll 轮询 stdin（不阻塞），保证 SIGINT/SIGTERM 能及时响应退出
    std::string line;
    std::string input_buf;
    // “理解”轮询线程：每 5s 拉全量快照，snapshotRevision 变化时显示 headline
    // （独立线程，避免阻塞控制台输入与自动问答）
    std::thread understanding_thread;
    if (use_understanding) {
        understanding_thread = std::thread([&]() {
            long long last_snap_rev = -1;
            while (!g_quit.load()) {
                for (int i = 0; i < 50 && !g_quit.load(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_quit.load()) break;
                HttpResponse ur;
                if (http_request("GET", http_base + "/api/session/" + session_id +
                                           "/understanding",
                                 "", cafile, 8, ur) &&
                    ur.status == 200) {
                    try {
                            Json::Value u;
                            if (parse_json_object(ur.body, u)) {
                            long long rev = u.get("snapshotRevision", -1).asInt64();
                            if (rev != last_snap_rev) {
                                last_snap_rev = rev;
                                std::string headline = "(暂无)";
                                const Json::Value& ov = u["overview"];
                                if (ov.isObject() && ov["headline"].isString())
                                    headline = ov["headline"].asString();
                                LOGF("[理解#%lld] %s", rev, headline.c_str());
                            }
                        }
                    } catch (const std::exception& e) {
                        LOGF("[理解] 解析异常: %s", e.what());
                    }
                }
            }
        });
    }
    while (!g_quit.load()) {
        struct pollfd pfd { STDIN_FILENO, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, 300);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[256];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                // EOF（后台运行等场景）：继续运行，信号负责退出
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            input_buf.append(buf, (size_t)n);
            size_t nl;
            while ((nl = input_buf.find('\n')) != std::string::npos) {
                line = input_buf.substr(0, nl);
                input_buf.erase(0, nl + 1);
                goto handle_line;
            }
            continue;
        }
        continue;
    handle_line:
        if (line.empty() && use_host) {
            if (!g_asking.load()) {
                LOGF(">> 开始提问 (再按 Enter 结束)");
                g_asking.store(true);
            } else {
                g_asking.store(false);
                Json::Value m;
                m["type"] = "end_of_speech";
                ws_ho->send_json(m);
                LOGF(">> end_of_speech 已发送");
            }
        } else if ((line == "s" || line == "S") && use_host) {
            Json::Value m;
            m["type"] = "stop";
            ws_ho->send_json(m);
            playback.clear();
            LOGF(">> stop 已发送");
        } else if (line == "q" || line == "Q") {
            LOGF(">> 结束会议");
            g_quit.store(true);
        }
    }

    // ---- 6. 清理：停音频生产 → end → 关 WS → end session ----
    LOGF("shutting down...");
    LOGF("stopping capture...");
    capture.stop();
    if (inject_thread.joinable()) inject_thread.join();
    if (auto_thread.joinable()) auto_thread.join();
    if (understanding_thread.joinable()) understanding_thread.join();
    bool uplink_flush_ok = true;
    if (use_transcribe) {
        if (!ws_tr->is_open()) {
            uplink_flush_ok = false;
        } else {
            Json::Value m;
            m["type"] = "end";
            uplink_flush_ok = ws_tr->send_json(m) && ws_tr->flush(8000);
            // The server may emit the ASR tail after consuming end. Keep the
            // socket readable briefly so that final reaches the streaming UI;
            // the canonical HTTP transcript below remains the final authority.
            if (uplink_flush_ok) {
                for (int i = 0; i < 50 && ws_tr->is_open(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!uplink_flush_ok)
            LOGF("[纪要失败] 转写通道断开或音频上行未完整排空");
    }
    LOGF("closing ws...");
    ws_tr->close();
    ws_ho->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    LOGF("stopping ws io...");
    ws_tr->stop();
    ws_ho->stop();
    LOGF("stopping playback...");
    playback.stop();

    HttpResponse endr;
    bool end_ok = http_request("POST", http_base + "/api/session/" + session_id + "/end",
                               "", cafile, 20, endr) &&
                  endr.status == 200;
    LOGF("session %s ended: status=%d body=%s", session_id.c_str(), endr.status,
         endr.body.c_str());

    Json::Value end_json;
    bool end_json_ok = end_ok && parse_json_object(endr.body, end_json);
    bool analysis_final = end_json_ok && end_json.get("analysis_final", false).asBool();
    Json::Value transcript_json;
    Json::Value understanding_json;
    bool transcript_ok = !use_understanding;
    bool understanding_ok = !use_understanding;
    bool snapshots_consistent = !use_understanding;
    if (use_understanding && end_ok) {
        HttpResponse tr;
        transcript_ok = http_request("GET", http_base + "/api/session/" + session_id +
                                               "/transcript",
                                     "", cafile, 10, tr) &&
                        tr.status == 200 && parse_json_object(tr.body, transcript_json) &&
                        transcript_snapshot_complete(transcript_json);
        HttpResponse ur;
        understanding_ok = http_request("GET", http_base + "/api/session/" + session_id +
                                                  "/understanding",
                                        "", cafile, 10, ur) &&
                           ur.status == 200 && parse_json_object(ur.body, understanding_json);
        bool snapshot_final = understanding_ok &&
                              understanding_snapshot_final(understanding_json);
        std::string consistency_error;
        snapshots_consistent = transcript_ok && snapshot_final && end_json_ok &&
                               final_snapshots_consistent(session_id, end_json,
                                                          transcript_json,
                                                          understanding_json,
                                                          consistency_error);
        if (transcript_ok) {
            std::vector<std::string> streamed;
            {
                std::lock_guard<std::mutex> lock(streamed_finals_mutex);
                streamed = streamed_final_texts;
            }
            log_unseen_final_transcripts(transcript_json, streamed);
        }
        if (uplink_flush_ok && analysis_final && snapshots_consistent)
            log_final_minutes(understanding_json);
        else if (!analysis_final)
            LOGF("[纪要失败] 服务端最终分析未完成");
        else if (!transcript_ok)
            LOGF("[纪要失败] 无法读取完整转写");
        else if (!snapshot_final)
            LOGF("[纪要失败] 服务端未返回最终理解快照");
        else if (!snapshots_consistent)
            LOGF("[纪要失败] 最终数据不一致: %s", consistency_error.c_str());
        understanding_ok = snapshot_final;
    }

    const bool complete_minutes_ok = !use_understanding ||
        (end_ok && end_json_ok && uplink_flush_ok && analysis_final && transcript_ok &&
         understanding_ok && snapshots_consistent);
    bool record_ok = !use_understanding || record_file.empty();
    if (use_understanding && complete_minutes_ok && !record_file.empty()) {
        Json::Value record;
        record["schemaVersion"] = "1.0";
        record["sessionId"] = session_id;
        record["topic"] = topic;
        record["end"] = end_json;
        record["transcript"] = transcript_json;
        record["understanding"] = understanding_json;
        Json::StreamWriterBuilder record_writer;
        record_writer["indentation"] = "  ";
        record_writer["emitUTF8"] = true;
        record_ok = write_file_atomic(record_file, Json::writeString(record_writer, record));
        if (record_ok) LOGF("[纪要已保存] %s", record_file.c_str());
        else LOGF("[纪要失败] 无法保存 %s", record_file.c_str());
    }
    LOGF("bye.");
    const bool minutes_ok = complete_minutes_ok && record_ok;
    return end_ok && minutes_ok ? 0 : 1;
}
