#ifndef APP_EVENTS_H
#define APP_EVENTS_H

enum class AppEvent {
    fault_happen,
    fault_solved,
    startup_done,
    to_stop,
    wake_detected,
    vad_no_speech,
    vad_end,
    voice_processing,
    speaking_msg_received,
    speaking_end,
    dialogue_end,
    asr_start,
    asr_result,
    // Add more events here...
};

#endif // APP_EVENTS_H
