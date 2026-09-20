// camera_yolo: 实时 YOLOv5 目标检测（RV1106 / Echo-Mate）
//
// 取流路径: ISP mainpath (/dev/video11) NV12 → RGA letterbox 转 RGB888 640x640
//           → RKNN yolov5 推理 → 打印检测结果与 FPS
//
// 前置条件: rkaiq_3A_server 已在后台运行（3A 由板端脚本启动），
//           传感器已由 DTS/驱动绑定为 gc1084。
//
// 用法:
//   camera_yolo <model.rknn> <video-device> [--count N] [--dump-prefix P]
//               [--every N] [--width W] [--height H]
//
//   默认无限循环，Ctrl-C 干净退出；--count N 抓 N 帧后自动退出。
//   --dump-prefix P 时每 --every 帧落盘 NV12 原帧 P-<seq>.nv12 与
//   RGB888 模型输入 P-<seq>.rgb（640x640，供宿主侧转 JPEG 验收）。
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "image_utils.h"
#include "yolov5.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void handle_signal(int) { g_stop = 1; }

int clamp_coordinate(float value, int maximum)
{
    return std::max(0, std::min(maximum - 1, static_cast<int>(value)));
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

// 统计 NV12 Y 平面均值，用于快速判断 ISP 是否输出有效帧（全零=链路不通）
double y_plane_mean(const unsigned char* buffer, size_t length)
{
    if (buffer == nullptr || length == 0) {
        return -1.0;
    }
    uint64_t sum = 0;
    const size_t step = std::max<size_t>(1, length / 1024);
    size_t count = 0;
    for (size_t i = 0; i < length; i += step) {
        sum += buffer[i];
        ++count;
    }
    return static_cast<double>(sum) / static_cast<double>(count);
}

struct V4l2Capture {
    int fd = -1;
    int width = 0;
    int height = 0;
    int buffer_count = 0;
    bool multiplanar = true;
    std::vector<void*> mapped;
    std::vector<size_t> lengths;

    ~V4l2Capture() { close_device(); }

    v4l2_buf_type buf_type() const
    {
        return multiplanar ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                           : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }

    int open_device(const char* path, int w, int h)
    {
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            std::fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
            return -1;
        }
        struct v4l2_capability cap;
        std::memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            std::fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
            return -1;
        }
        multiplanar = (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        std::printf("device %s: %s\n", path, multiplanar ? "multiplanar" : "single-planar");

        const v4l2_buf_type type = buf_type();
        struct v4l2_format fmt;
        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = type;
        if (multiplanar) {
            fmt.fmt.pix_mp.width = w;
            fmt.fmt.pix_mp.height = h;
            fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            fmt.fmt.pix_mp.num_planes = 1;
            fmt.fmt.pix_mp.plane_fmt[0].sizeimage = w * h * 3 / 2;
            fmt.fmt.pix_mp.plane_fmt[0].bytesperline = w;
        } else {
            fmt.fmt.pix.width = w;
            fmt.fmt.pix.height = h;
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        }
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            std::fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
            return -1;
        }
        if (multiplanar) {
            width = fmt.fmt.pix_mp.width;
            height = fmt.fmt.pix_mp.height;
            std::printf("capture format: %ux%u, bytesperline=%u, sizeimage=%u\n",
                        width, height, fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
                        fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
        } else {
            width = fmt.fmt.pix.width;
            height = fmt.fmt.pix.height;
            std::printf("capture format: %ux%u, bytesperline=%u, sizeimage=%u\n",
                        width, height, fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
        }
        return 0;
    }

    int start_stream()
    {
        const v4l2_buf_type type = buf_type();
        struct v4l2_requestbuffers req;
        std::memset(&req, 0, sizeof(req));
        req.count = 4;
        req.type = type;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            std::fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
            return -1;
        }
        buffer_count = req.count;
        for (unsigned int i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf;
            struct v4l2_plane planes[VIDEO_MAX_PLANES];
            std::memset(&buf, 0, sizeof(buf));
            std::memset(planes, 0, sizeof(planes));
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (multiplanar) {
                buf.m.planes = planes;
                buf.length = 1;
            }
            if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                std::fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
                return -1;
            }
            const unsigned long offset = multiplanar ? buf.m.planes[0].m.mem_offset
                                                     : buf.m.offset;
            const size_t length = multiplanar ? buf.m.planes[0].length : buf.length;
            void* addr = mmap(nullptr, length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, offset);
            if (addr == MAP_FAILED) {
                std::fprintf(stderr, "mmap failed: %s\n", strerror(errno));
                return -1;
            }
            mapped.push_back(addr);
            lengths.push_back(length);
            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
                return -1;
            }
        }
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            std::fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
            return -1;
        }
        std::printf("stream on: %d mmap buffers\n", buffer_count);
        return 0;
    }

    // 等待一帧；返回 0 成功，-1 超时/错误
    int wait_frame(int timeout_ms, unsigned int* index)
    {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR && g_stop) {
                return -2;
            }
            std::fprintf(stderr, "poll failed: %s\n", strerror(errno));
            return -1;
        }
        if (ret == 0) {
            std::fprintf(stderr, "poll timeout: no frame in %d ms (MIPI/ISP stalled?)\n",
                         timeout_ms);
            return -1;
        }
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        std::memset(&buf, 0, sizeof(buf));
        std::memset(planes, 0, sizeof(planes));
        buf.type = buf_type();
        buf.memory = V4L2_MEMORY_MMAP;
        if (multiplanar) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            std::fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
            return -1;
        }
        *index = buf.index;
        return 0;
    }

    int queue_buffer(unsigned int index)
    {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        std::memset(&buf, 0, sizeof(buf));
        std::memset(planes, 0, sizeof(planes));
        buf.type = buf_type();
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;
        if (multiplanar) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
            return -1;
        }
        return 0;
    }

    void close_device()
    {
        if (fd >= 0) {
            if (buffer_count > 0) {
                const v4l2_buf_type type = buf_type();
                ioctl(fd, VIDIOC_STREAMOFF, &type);
            }
            for (size_t i = 0; i < mapped.size(); ++i) {
                if (mapped[i] != nullptr && mapped[i] != MAP_FAILED) {
                    munmap(mapped[i], lengths[i]);
                }
            }
            close(fd);
            fd = -1;
        }
        mapped.clear();
        lengths.clear();
        buffer_count = 0;
    }
};

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

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    if (argc < 3) {
        std::fprintf(stderr,
                     "Usage: %s <model.rknn> <video-device> [--count N] "
                     "[--dump-prefix P] [--every N] [--width W] [--height H]\n",
                     argv[0]);
        return 2;
    }
    const std::string model_path = argv[1];
    const std::string device_path = argv[2];
    int count = 0;
    std::string dump_prefix;
    int every = 1;
    int width = 1280;
    int height = 720;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--count") {
            count = std::atoi(need_value("--count"));
        } else if (arg == "--dump-prefix") {
            dump_prefix = need_value("--dump-prefix");
        } else if (arg == "--every") {
            every = std::atoi(need_value("--every"));
        } else if (arg == "--width") {
            width = std::atoi(need_value("--width"));
        } else if (arg == "--height") {
            height = std::atoi(need_value("--height"));
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (every < 1) {
        every = 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int status = 1;
    int frames = 0;
    int detections_total = 0;
    double elapsed_total_ms = 0.0;
    bool first_frame = true;
    double last_fps = 0.0;
    rknn_app_context_t app = {};
    V4l2Capture capture;
    bool post_initialized = false;

    if (init_yolov5_model(model_path.c_str(), &app) != 0) {
        std::fprintf(stderr, "failed to init RKNN model: %s\n", model_path.c_str());
        goto cleanup;
    }
    if (init_post_process() != 0) {
        std::fprintf(stderr, "failed to init YOLO post-processing\n");
        goto cleanup;
    }
    post_initialized = true;

    if (capture.open_device(device_path.c_str(), width, height) != 0) {
        goto cleanup;
    }
    if (capture.start_stream() != 0) {
        goto cleanup;
    }

    std::printf("model=%s device=%s input=%ux%u\n",
                model_path.c_str(), device_path.c_str(),
                app.model_width, app.model_height);

    while (!g_stop) {
        unsigned int index = 0;
        const int wait_ret = capture.wait_frame(10000, &index);
        if (wait_ret == -2) {
            break;
        }
        if (wait_ret != 0) {
            std::fprintf(stderr, "frame wait failed (captured %d frames so far)\n", frames);
            break;
        }

        const auto frame_started = std::chrono::steady_clock::now();
        const unsigned char* nv12 =
            static_cast<const unsigned char*>(capture.mapped[index]);

        if (first_frame) {
            const double y_mean = y_plane_mean(nv12, capture.lengths[index]);
            std::printf("first frame: Y mean=%.1f (%s)\n", y_mean,
                        y_mean < 1.0 ? "ALL ZERO - ISP path broken" : "has content");
            first_frame = false;
        }

        image_buffer_t src = {};
        src.width = capture.width;
        src.height = capture.height;
        src.width_stride = capture.width;
        src.height_stride = capture.height;
        src.format = IMAGE_FORMAT_YUV420SP_NV12;
        src.virt_addr = const_cast<unsigned char*>(nv12);
        src.size = capture.lengths[index];
        src.fd = -1;

        image_buffer_t model_input = {};
        model_input.width = app.model_width;
        model_input.height = app.model_height;
        model_input.width_stride = app.input_attrs[0].w_stride;
        model_input.height_stride = app.input_attrs[0].h_stride;
        model_input.format = IMAGE_FORMAT_RGB888;
        model_input.virt_addr =
            static_cast<unsigned char*>(app.input_mems[0]->virt_addr);
        model_input.size = app.input_attrs[0].size_with_stride;
        model_input.fd = -1;

        letterbox_t letterbox = {};
        if (convert_image_with_letterbox(&src, &model_input, &letterbox, 114) != 0) {
            std::fprintf(stderr, "convert_image_with_letterbox failed\n");
            capture.queue_buffer(index);
            break;
        }

        object_detect_result_list detections = {};
        if (inference_yolov5_model(&app, &detections) != 0) {
            std::fprintf(stderr, "RKNN inference failed\n");
            capture.queue_buffer(index);
            break;
        }

        const auto frame_finished = std::chrono::steady_clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(
            frame_finished - frame_started).count();
        elapsed_total_ms += frame_ms;
        ++frames;
        detections_total += detections.count;
        if (frames > 1) {
            last_fps = 1000.0 / (elapsed_total_ms / frames);
        }

        std::printf("frame=%d total_ms=%.1f avg_fps=%.2f infer_frames=%d dets=%d\n",
                    frames, frame_ms, last_fps, frames, detections.count);
        print_detections(detections, letterbox, capture.width, capture.height);

        if (!dump_prefix.empty() && (frames % every) == 0) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s-%d.nv12", dump_prefix.c_str(), frames);
            FILE* fp = std::fopen(path, "wb");
            if (fp != nullptr) {
                std::fwrite(nv12, 1, capture.lengths[index], fp);
                std::fclose(fp);
            }
            std::snprintf(path, sizeof(path), "%s-%d.rgb", dump_prefix.c_str(), frames);
            fp = std::fopen(path, "wb");
            if (fp != nullptr) {
                std::fwrite(model_input.virt_addr, 1,
                            app.model_width * app.model_height * 3, fp);
                std::fclose(fp);
            }
        }

        if (capture.queue_buffer(index) != 0) {
            break;
        }
        if (count > 0 && frames >= count) {
            break;
        }
    }

    std::printf("summary: frames=%d detections=%d avg_frame_ms=%.1f avg_fps=%.2f\n",
                frames, detections_total,
                frames > 0 ? elapsed_total_ms / frames : 0.0,
                frames > 0 ? 1000.0 * frames / elapsed_total_ms : 0.0);
    status = 0;

cleanup:
    if (post_initialized) {
        deinit_post_process();
    }
    release_model(&app);
    return status;
}
