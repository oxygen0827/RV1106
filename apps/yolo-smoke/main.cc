#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "image_utils.h"
#include "yolov5.h"

namespace {

void release_model(rknn_app_context_t* app)
{
    const rknn_context context = app->rknn_ctx;

    if (context != 0) {
        for (unsigned int i = 0; i < app->io_num.n_input; ++i) {
            if (app->input_mems[i] != nullptr) {
                rknn_destroy_mem(context, app->input_mems[i]);
                app->input_mems[i] = nullptr;
            }
        }
        for (unsigned int i = 0; i < app->io_num.n_output; ++i) {
            if (app->output_mems[i] != nullptr) {
                rknn_destroy_mem(context, app->output_mems[i]);
                app->output_mems[i] = nullptr;
            }
        }
        rknn_destroy(context);
        app->rknn_ctx = 0;
    }

    free(app->input_attrs);
    free(app->output_attrs);
    app->input_attrs = nullptr;
    app->output_attrs = nullptr;
}

int clamp_coordinate(float value, int maximum)
{
    return std::max(0, std::min(maximum - 1, static_cast<int>(value)));
}

int resize_letterbox_cpu(const image_buffer_t& source,
                         image_buffer_t* destination,
                         letterbox_t* letterbox)
{
    if (source.format != IMAGE_FORMAT_RGB888 || source.virt_addr == nullptr ||
        destination->format != IMAGE_FORMAT_RGB888 ||
        destination->virt_addr == nullptr) {
        return -1;
    }

    const float scale_x = static_cast<float>(destination->width) / source.width;
    const float scale_y = static_cast<float>(destination->height) / source.height;
    const float scale = std::min(scale_x, scale_y);
    const int resized_width = std::max(1, static_cast<int>(source.width * scale));
    const int resized_height = std::max(1, static_cast<int>(source.height * scale));
    const int x_pad = (destination->width - resized_width) / 2;
    const int y_pad = (destination->height - resized_height) / 2;

    std::memset(destination->virt_addr, 114, destination->size);
    for (int y = 0; y < resized_height; ++y) {
        const float source_y = (y + 0.5f) / scale - 0.5f;
        const int y0 = std::max(0, static_cast<int>(std::floor(source_y)));
        const int y1 = std::min(source.height - 1, y0 + 1);
        const float y_weight = source_y - std::floor(source_y);

        for (int x = 0; x < resized_width; ++x) {
            const float source_x = (x + 0.5f) / scale - 0.5f;
            const int x0 = std::max(0, static_cast<int>(std::floor(source_x)));
            const int x1 = std::min(source.width - 1, x0 + 1);
            const float x_weight = source_x - std::floor(source_x);
            unsigned char* output = destination->virt_addr +
                ((y + y_pad) * destination->width + x + x_pad) * 3;

            for (int channel = 0; channel < 3; ++channel) {
                const float top_left = source.virt_addr[(y0 * source.width + x0) * 3 + channel];
                const float top_right = source.virt_addr[(y0 * source.width + x1) * 3 + channel];
                const float bottom_left = source.virt_addr[(y1 * source.width + x0) * 3 + channel];
                const float bottom_right = source.virt_addr[(y1 * source.width + x1) * 3 + channel];
                const float top = top_left + (top_right - top_left) * x_weight;
                const float bottom = bottom_left + (bottom_right - bottom_left) * x_weight;
                output[channel] = static_cast<unsigned char>(
                    top + (bottom - top) * y_weight + 0.5f);
            }
        }
    }

    letterbox->scale = scale;
    letterbox->x_pad = x_pad;
    letterbox->y_pad = y_pad;
    return 0;
}

void print_detections(const object_detect_result_list& detections,
                      const letterbox_t& letterbox,
                      int source_width,
                      int source_height)
{
    std::printf("detections=%d\n", detections.count);
    for (int i = 0; i < detections.count; ++i) {
        const object_detect_result& result = detections.results[i];
        const int left = clamp_coordinate(
            (result.box.left - letterbox.x_pad) / letterbox.scale, source_width);
        const int top = clamp_coordinate(
            (result.box.top - letterbox.y_pad) / letterbox.scale, source_height);
        const int right = clamp_coordinate(
            (result.box.right - letterbox.x_pad) / letterbox.scale, source_width);
        const int bottom = clamp_coordinate(
            (result.box.bottom - letterbox.y_pad) / letterbox.scale, source_height);

        std::printf("detection[%d] class=%s confidence=%.3f box=%d,%d,%d,%d\n",
                    i, coco_cls_to_name(result.cls_id), result.prop,
                    left, top, right, bottom);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "Usage: %s MODEL IMAGE [RUNS]\n", argv[0]);
        return 2;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed_runs = argc == 4 ? std::strtol(argv[3], &end, 10) : 3;
    if (errno != 0 || parsed_runs < 1 || parsed_runs > 100 ||
        (argc == 4 && (end == argv[3] || *end != '\0'))) {
        std::fprintf(stderr, "RUNS must be an integer from 1 to 100\n");
        return 2;
    }
    const int runs = static_cast<int>(parsed_runs);

    int status = 1;
    bool post_process_initialized = false;
    rknn_app_context_t app = {};
    image_buffer_t source = {};
    image_buffer_t model_input = {};
    letterbox_t letterbox = {};
    std::vector<double> elapsed_ms;
    object_detect_result_list detections = {};
    double total_ms = 0.0;

    if (init_yolov5_model(argv[1], &app) != 0) {
        std::fprintf(stderr, "failed to initialize RKNN model: %s\n", argv[1]);
        goto cleanup;
    }
    if (init_post_process() != 0) {
        std::fprintf(stderr, "failed to initialize YOLO post-processing labels\n");
        goto cleanup;
    }
    post_process_initialized = true;

    if (read_image(argv[2], &source) != 0 || source.virt_addr == nullptr) {
        std::fprintf(stderr, "failed to read input image: %s\n", argv[2]);
        goto cleanup;
    }

    model_input.width = app.model_width;
    model_input.height = app.model_height;
    model_input.width_stride = app.model_width;
    model_input.height_stride = app.model_height;
    model_input.format = IMAGE_FORMAT_RGB888;
    model_input.virt_addr = static_cast<unsigned char*>(app.input_mems[0]->virt_addr);
    model_input.size = app.input_attrs[0].size_with_stride;
    model_input.fd = -1;
    if (resize_letterbox_cpu(source, &model_input, &letterbox) != 0) {
        std::fprintf(stderr, "failed to resize input image for the model\n");
        goto cleanup;
    }

    elapsed_ms.reserve(runs);
    for (int i = 0; i < runs; ++i) {
        const auto started = std::chrono::steady_clock::now();
        if (inference_yolov5_model(&app, &detections) != 0) {
            std::fprintf(stderr, "RKNN inference failed on run %d\n", i + 1);
            goto cleanup;
        }
        const auto finished = std::chrono::steady_clock::now();
        elapsed_ms.push_back(
            std::chrono::duration<double, std::milli>(finished - started).count());
    }

    for (double value : elapsed_ms) {
        total_ms += value;
    }
    std::printf("image=%dx%d model=%dx%d runs=%d\n",
                source.width, source.height, app.model_width, app.model_height, runs);
    for (int i = 0; i < runs; ++i) {
        std::printf("inference[%d]=%.3fms\n", i + 1, elapsed_ms[i]);
    }
    std::printf("inference_average=%.3fms inference_fps=%.3f\n",
                total_ms / runs, 1000.0 * runs / total_ms);
    print_detections(detections, letterbox, source.width, source.height);
    status = detections.count > 0 ? 0 : 3;

cleanup:
    free(source.virt_addr);
    if (post_process_initialized) {
        deinit_post_process();
    }
    release_model(&app);
    return status;
}
