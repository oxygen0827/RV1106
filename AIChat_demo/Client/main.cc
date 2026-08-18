#include <iostream>
#include <stdexcept>

#include "Application/Application.h"

void print_usage(const char* progname) {
    std::cout << "Usage: " << progname
              << " <server_address> <port> <token> [asr]" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        std::string address = argv[1];
        int port = std::stoi(argv[2]);
        std::string token = argv[3];
        std::string device_id = "00:11:22:33:44:55";
        int protocol_version = 2;
        int sample_rate = 16000;
        int channels = 1;
        int frame_duration = 40;
        bool asr_mode = argc >= 5 && std::string(argv[4]) == "asr";

        Application app(
            address, port, token, device_id, protocol_version,
            sample_rate, channels, frame_duration, asr_mode
        );
        app.Run();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
