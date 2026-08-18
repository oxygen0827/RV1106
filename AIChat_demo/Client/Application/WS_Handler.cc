#include "WS_Handler.h"
#include "../Utils/user_log.h"

void WSHandler::ws_msg_handle(const std::string& message, bool is_binary, Application* app) {
    // 接收到json消息时的回调
    if(!is_binary) {
        Json::Value root;
        Json::Reader reader;
        // 解析 JSON 字符串
        bool parsingSuccessful = reader.parse(message, root);
        if (!parsingSuccessful) {
            USER_LOG_WARN("Error parsing message: %s", reader.getFormattedErrorMessages().c_str());
            app->eventQueue_.Enqueue(static_cast<int>(AppEvent::fault_happen));
        }
        USER_LOG_INFO("Received JSON message: %s", message.c_str());
        // 获取 JSON 对象中的 type 值
        const Json::Value type = root["type"];
        if (type.isString()) {
            std::string typeStr = type.asString();
            if (typeStr == "vad") {
                handle_vad_message(root, app);
            } else if (typeStr == "voice") {
                handle_voice_message(root, app);
            } else if (typeStr == "asr") {
                if (root["state"].asString() == "text" && root["text"].isString()) {
                    const std::string text = root["text"].asString();
                    USER_LOG_INFO("ASR transcription: %s", text.c_str());
                    app->TranscriptQueue_.Enqueue(text);
                } else if (root["state"].asString() == "end") {
                    app->eventQueue_.Enqueue(static_cast<int>(AppEvent::asr_result));
                }
            } else if (typeStr == "chat") {
                handle_chat_message(root, app);
            } else if (typeStr == "error") {
                USER_LOG_ERROR("server erro msg: %s", message.c_str());
                app->eventQueue_.Enqueue(static_cast<int>(
                    app->is_asr_mode() ? AppEvent::asr_result : AppEvent::fault_happen));
            } else {
                USER_LOG_WARN("Unknown type: %s", typeStr.c_str());
            }
        }

        // 获取 JSON 对象中的 function_call 值
        if (root.isMember("function_call") && root["function_call"].isObject()) {
            handle_intent_message(root);
            app->IntentQueue_.Enqueue(root);
        }

    } else {    
        // 接收到二进制数据时的回调
        // USER_LOG_INFO("Received binary message.");
        BinProtocolInfo protocol_info;
        std::vector<uint8_t> opus_data;
        std::vector<int16_t> pcm_data;

        // 解包二进制数据
        if(app->audio_processor_.UnpackBinFrame(reinterpret_cast<const uint8_t*>(message.data()), message.size(), protocol_info, opus_data)) {
            // 检查版本和类型是否符合预期
            if(protocol_info.version == app->get_ws_protocolVersion() && protocol_info.type == 0) {
                // 将解码后的Opus数据放入队列供播放器使用
                if (app->audio_processor_.decode(opus_data.data(), opus_data.size(), pcm_data)) {
                    if(app->get_first_audio_msg_received() == true) {
                        app->set_first_audio_msg_received(false);
                        app->eventQueue_.Enqueue(static_cast<int>(AppEvent::speaking_msg_received));
                    }
                    app->audio_processor_.addFrameToPlaybackQueue(pcm_data);
                }
            } else {
                USER_LOG_WARN("Received frame with unexpected version or type");
            }
        } else {
            USER_LOG_WARN("Failed to unpack binary frame");
        }
    }

}

// 处理 VAD 消息
void WSHandler::handle_vad_message(const Json::Value& root, Application* app) {
    const Json::Value state = root["state"];
    if (state.isString()) {
        std::string stateStr = state.asString();
        if (stateStr == "no_speech") {
            app->eventQueue_.Enqueue(static_cast<int>(AppEvent::vad_no_speech));
        }
    }
}

// 处理 GLM-4-Voice 会话消息
void WSHandler::handle_voice_message(const Json::Value& root, Application* app) {
    const Json::Value state = root["state"];
    if (state.isString()) {
        std::string stateStr = state.asString();
        if (stateStr == "processing") {
            USER_LOG_INFO("Voice input accepted; waiting for GLM-4-Voice.");
            app->eventQueue_.Enqueue(static_cast<int>(AppEvent::voice_processing));
        } else if (stateStr == "end") {
            USER_LOG_INFO("Received voice response end.");
            app->set_voice_completed(true);
        } else if (stateStr == "text") {
            const Json::Value text = root["text"];
            if (text.isString()) {
                USER_LOG_INFO("Voice response text: %s", text.asString().c_str());
            }
        } else if (stateStr == "no_speech") {
            app->eventQueue_.Enqueue(static_cast<int>(AppEvent::vad_no_speech));
        }
    }
}

// 处理Chat消息
void WSHandler::handle_chat_message(const Json::Value& root, Application* app) {
    const Json::Value dialogue = root["dialogue"];
    if (dialogue.isString()) {
        std::string dialogueStr = dialogue.asString();
        if (dialogueStr == "end") {
            USER_LOG_INFO("Received dialogue end.");
            app->set_dialogue_completed(true);
        }
    }
}

// 处理意图消息
void WSHandler::handle_intent_message(const Json::Value& root) {
    // 处理 function_call 消息
    const Json::Value function_call = root["function_call"];
    // 检查 function_call 是否包含 "name" 和 "arguments"
    if (function_call.isMember("name") && function_call["name"].isString() &&
        function_call.isMember("arguments") && function_call["arguments"].isObject()) {
        // 调用 HandleIntent 处理意图
        IntentHandler::HandleIntent(root);
    } else {
        USER_LOG_ERROR("Invalid function_call structure in JSON: %s", root.toStyledString().c_str());
    }
}
