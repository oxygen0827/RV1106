#include "AIchat_c_interface.h"
#include "../Application/Application.h"
#include <cstring>
#ifdef __arm__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif

extern "C" {

// 创建并初始化Application对象
void* create_aichat_app(const char* address, int port, const char* token,
                        const char* deviceId, int protocolVersion,
                        int sample_rate, int channels, int frame_duration,
                        int asr_mode) {
    auto* app = new Application(
        std::string(address), port, std::string(token), std::string(deviceId),
        protocolVersion, sample_rate, channels, frame_duration, asr_mode != 0
    );
    return static_cast<void*>(app);
}


// 销毁Application对象
void destroy_aichat_app(void* app_ptr) {
    if (app_ptr) {
        auto* app = static_cast<Application*>(app_ptr);
        delete app;
    }
}

// 运行Application对象
void run_aichat_app(void* app_ptr) {
    if (app_ptr) {
        auto* app = static_cast<Application*>(app_ptr);
        app->Run();
    }
}

// 发送停止信号
void stop_aichat_app(void* app_ptr) {
    if (app_ptr) {
        auto* app = static_cast<Application*>(app_ptr);
        app->Stop();
    }
}

// 获取当前状态
ChatState get_aichat_app_state(void* app_ptr) {
    if (app_ptr) {
        auto* app = static_cast<Application*>(app_ptr);
        return static_cast<ChatState>(app->getState());
    }
    return ChatState::fault; // 默认返回错误状态
}

// 获取 Intent 数据
bool get_aichat_app_intent(void* app_ptr, IntentData* intent_data) {
    if (app_ptr && intent_data) {
        auto* app = static_cast<Application*>(app_ptr);
        if(app->IntentQueue_.IsEmpty()) {
            return false; // Intent 队列为空
        }
        auto intent_opt = app->IntentQueue_.Dequeue(); // 获取 std::optional<Json::Value>
        if (intent_opt.has_value()) {
            const Json::Value& intent = intent_opt.value();

            // 提取 function_call 的 name
            if (intent.isMember("function_call") && intent["function_call"].isObject()) {
                const Json::Value& function_call = intent["function_call"];
                if (function_call.isMember("name") && function_call["name"].isString()) {
                    std::strncpy(intent_data->function_name, function_call["name"].asCString(), sizeof(intent_data->function_name) - 1);
                    intent_data->function_name[sizeof(intent_data->function_name) - 1] = '\0'; // 确保字符串以 null 结尾
                } else {
                    std::strncpy(intent_data->function_name, "unknown", sizeof(intent_data->function_name) - 1);
                    intent_data->function_name[sizeof(intent_data->function_name) - 1] = '\0';
                }

                // 提取 arguments 的键值对
                if (function_call.isMember("arguments") && function_call["arguments"].isObject()) {
                    const Json::Value& arguments = function_call["arguments"];
                    intent_data->argument_count = 0;

                    for (const auto& key : arguments.getMemberNames()) {
                        if (intent_data->argument_count < 10) { // 限制最多存储 10 个键值对
                            std::strncpy(intent_data->argument_keys[intent_data->argument_count], key.c_str(), sizeof(intent_data->argument_keys[0]) - 1);
                            intent_data->argument_keys[intent_data->argument_count][sizeof(intent_data->argument_keys[0]) - 1] = '\0';

                            std::strncpy(intent_data->argument_values[intent_data->argument_count], arguments[key].asCString(), sizeof(intent_data->argument_values[0]) - 1);
                            intent_data->argument_values[intent_data->argument_count][sizeof(intent_data->argument_values[0]) - 1] = '\0';

                            intent_data->argument_count++;
                        }
                    }
                } else {
                    intent_data->argument_count = 0; // 没有 arguments
                }

                return true; // 成功提取数据
            }
        }
    }

    // 如果没有值或 app_ptr 为空，返回默认值
    if (intent_data) {
        std::strncpy(intent_data->function_name, "none", sizeof(intent_data->function_name) - 1);
        intent_data->function_name[sizeof(intent_data->function_name) - 1] = '\0';
        intent_data->argument_count = 0;
    }
    return false; // 未提取到数据
}

bool get_aichat_asr_text(void* app_ptr, char* buffer, size_t buffer_size) {
    if (!app_ptr || !buffer || buffer_size == 0) {
        return false;
    }
    auto* app = static_cast<Application*>(app_ptr);
    if (app->TranscriptQueue_.IsEmpty()) {
        return false;
    }
    auto text_opt = app->TranscriptQueue_.Dequeue();
    if (!text_opt.has_value()) {
        return false;
    }
    std::strncpy(buffer, text_opt->c_str(), buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    return true;
}


} // extern "C"
