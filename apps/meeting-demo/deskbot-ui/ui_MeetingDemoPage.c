/**
 * ui_MeetingDemoPage —— 会议纪要 demo 启动页
 *
 * 页面职责：
 *   1. fork/exec /root/meeting_demo/meeting-demo-run.sh（SERVER 优先读
 *      /root/meeting_demo/server.conf，缺省生产 Voice API）。
 *   2. 后台读线程从子进程合并后的 stdout/stderr 管道收行，推入环形缓冲；
 *      500ms LVGL 定时器在 UI 线程取出行并追加到转写文本区（LVGL
 *      对象只由 UI 线程触碰）。
 *   3. 使用 listen 模式完成转写与理解；「结束会议」只结束子进程并留在
 *      当前页面，最终纪要生成后可继续查看或开启下一场会议。
 *
 * 生命周期：按钮启动进程；返回按钮异步写 q，deinit 仅做最终兜底清理
 * （SIGTERM/SIGKILL + join）。可重复进入/退出。
 */
#include "ui_MeetingDemoPage.h"

#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

///////////////////// 常量 ////////////////////

#define LINE_MAX_BYTES    640   // 单行最大字节（约 210 个汉字）
#define LOG_MAX_LINES     80    // 保留完整结束阶段，避免纪要字段被日志突发覆盖
#define DISP_MAX_BYTES    (LOG_MAX_LINES * (LINE_MAX_BYTES + 1) + 64)  // 显示缓冲

#define MEETING_SH_CMD \
    "APP=/root/meeting_demo MODE=listen " \
    "SERVER=$(cat /root/meeting_demo/server.conf 2>/dev/null || " \
    "echo wss://clare.vinex.top/voice-api) " \
    "exec /root/meeting_demo/meeting-demo-run.sh"

///////////////////// 运行状态 ////////////////////

typedef struct {
    char lines[LOG_MAX_LINES][LINE_MAX_BYTES];
    int  head;    // 下一写入位置（环形）
    int  count;   // 当前行数（<= LOG_MAX_LINES）
} line_ring_t;

typedef struct {
    pid_t      pid;             // -1 = 无进程
    int        stdin_fd;        // 子进程 stdin 写端；-1 = 已关闭
    int        stdout_fd;       // 子进程 stdout 读端（非阻塞）；-1 = 已关闭
    int        thread_started;
    int        child_running;   // 1 = 进程存活
    int        child_ready;     // 1 = WS/音频链路已就绪
    int        last_exit_rc;    // 最近一次会议退出码；-999 = 尚未结束
    pthread_t  thread;
    pthread_mutex_t mtx;        // 保护 ring 与上述状态字段
    line_ring_t ring;
} meeting_run_t;

static meeting_run_t g_run;

static lv_timer_t *g_ui_timer;    // 500ms：转写文本 + 状态 + 按钮显隐
static lv_obj_t   *g_status_label;
static lv_obj_t   *g_text_label;
static lv_obj_t   *g_text_container;
static lv_obj_t   *g_action_btn;
static lv_obj_t   *g_action_label;
static int         g_started_once;   // 是否至少启动过一次（区分 未开始/已停止）
static int         g_stopping;       // 正在结束并等待最终纪要
static int         g_return_pending; // 子进程结束后再返回，避免阻塞 LVGL 线程
static int         g_stop_ticks;     // 500ms tick；超时后终止卡住的子进程组

// 显示缓冲（仅 UI 线程访问）
static char g_disp_buf[DISP_MAX_BYTES];
static size_t g_disp_len;
static char g_partial_line[LINE_MAX_BYTES];
static char g_render_buf[DISP_MAX_BYTES + LINE_MAX_BYTES];

///////////////////// 环形缓冲 ////////////////////

static void ring_append(const char *line, size_t len)
{
    pthread_mutex_lock(&g_run.mtx);
    if (len >= LINE_MAX_BYTES) len = LINE_MAX_BYTES - 1;
    char *dst = g_run.ring.lines[g_run.ring.head];
    memcpy(dst, line, len);
    dst[len] = '\0';
    g_run.ring.head = (g_run.ring.head + 1) % LOG_MAX_LINES;
    if (g_run.ring.count < LOG_MAX_LINES) g_run.ring.count++;
    pthread_mutex_unlock(&g_run.mtx);
}

// 取出一行；返回 1 表示取到（行首地址放入 *out，由调用方持锁读取后 drop）
static int ring_pop_locked(char **out)
{
    if (g_run.ring.count <= 0) return 0;
    int idx = (g_run.ring.head - g_run.ring.count + LOG_MAX_LINES) % LOG_MAX_LINES;
    *out = g_run.ring.lines[idx];
    g_run.ring.count--;
    return 1;
}

// 状态查询辅助
static int state_child_running(void)
{
    pthread_mutex_lock(&g_run.mtx);
    int r = g_run.child_running;
    pthread_mutex_unlock(&g_run.mtx);
    return r;
}

static int state_child_ready(void)
{
    pthread_mutex_lock(&g_run.mtx);
    int ready = g_run.child_ready;
    pthread_mutex_unlock(&g_run.mtx);
    return ready;
}

static int state_last_exit_rc(void)
{
    pthread_mutex_lock(&g_run.mtx);
    int rc = g_run.last_exit_rc;
    pthread_mutex_unlock(&g_run.mtx);
    return rc;
}

// 原子地取走 stdin 写端，确保读线程与退出路径只有一方关闭。
static int state_take_stdin_fd(void)
{
    pthread_mutex_lock(&g_run.mtx);
    int fd = g_run.stdin_fd;
    g_run.stdin_fd = -1;
    pthread_mutex_unlock(&g_run.mtx);
    return fd;
}

static void state_set_child_ready(int ready)
{
    pthread_mutex_lock(&g_run.mtx);
    g_run.child_ready = ready;
    pthread_mutex_unlock(&g_run.mtx);
}

static void state_set_child_result(int rc)
{
    pthread_mutex_lock(&g_run.mtx);
    g_run.last_exit_rc = rc;
    g_run.child_ready = 0;
    g_run.child_running = 0;
    g_run.pid = -1;
    pthread_mutex_unlock(&g_run.mtx);
}

///////////////////// 子进程读线程 ////////////////////

static void *meeting_reader_thread(void *arg)
{
    (void)arg;
    char buf[512];
    char line[LINE_MAX_BYTES];
    size_t line_len = 0;
    for (;;) {
        ssize_t n = read(g_run.stdout_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n') {
                    if (line_len > 0) {
                        line[line_len] = '\0';
                        if (strstr(line, "ready. ") != NULL) state_set_child_ready(1);
                        ring_append(line, line_len);
                        line_len = 0;
                    }
                } else if (c != '\r') {
                    if (line_len < sizeof(line) - 1) line[line_len++] = c;
                }
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            usleep(30000);
            continue;
        }
        break;
    }

    if (line_len > 0) ring_append(line, line_len);

    // 唯一负责收尸的线程
    int st = 0;
    pid_t w = waitpid(g_run.pid, &st, 0);
    int rc = -1;
    if (w == g_run.pid) {
        char msg[96];
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        snprintf(msg, sizeof(msg), "[meeting_demo 退出 rc=%d]", rc);
        ring_append(msg, strlen(msg));
    } else {
        ring_append("[meeting_demo 已结束]", strlen("[meeting_demo 已结束]"));
    }
    if (g_run.stdout_fd >= 0) { close(g_run.stdout_fd); g_run.stdout_fd = -1; }
    int stdin_fd = state_take_stdin_fd();
    if (stdin_fd >= 0) close(stdin_fd);
    state_set_child_result(rc);
    return NULL;
}

///////////////////// 启动 / 停止 ////////////////////

static int meeting_start(void)
{
    // 上一场会议的读线程可能已经退出但尚未 join；重用状态前先收尾。
    if (g_run.thread_started && !state_child_running()) {
        pthread_join(g_run.thread, NULL);
        g_run.thread_started = 0;
    }

    // 清理可能残留的旧实例：页面/DeskBot 异常退出后孤儿 meeting_demo 会
    // 一直占着声卡，导致新实例 playback/capture open busy。先 TERM 再 KILL。
    (void)system("killall -q -TERM meeting_demo 2>/dev/null");
    usleep(300000);
    (void)system("killall -q -KILL meeting_demo 2>/dev/null");

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        // 子进程：fork 后只用 async-signal-safe 调用 + exec
        // 自成进程组：父进程兜底时 kill(-pid) 可连带 wrapper/meeting_demo 整组
        setpgid(0, 0);
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        execl("/bin/sh", "sh", "-c", MEETING_SH_CMD, (char *)NULL);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    g_run.pid = pid;
    g_run.stdin_fd = in_pipe[1];
    g_run.stdout_fd = out_pipe[0];
    fcntl(g_run.stdout_fd, F_SETFL, fcntl(g_run.stdout_fd, F_GETFL) | O_NONBLOCK);
    g_run.child_running = 1;
    g_run.child_ready = 0;
    g_run.last_exit_rc = -999;

    if (pthread_create(&g_run.thread, NULL, meeting_reader_thread, NULL) != 0) {
        g_run.thread_started = 0;
        return -1;
    }
    g_run.thread_started = 1;
    return 0;
}

static void meeting_request_stop(void)
{
    int fd = state_take_stdin_fd();
    if (fd >= 0) {
        write(fd, "q\n", 2);
        close(fd);
    }
}

static void meeting_signal_process_group(int sig)
{
    pthread_mutex_lock(&g_run.mtx);
    pid_t pid = g_run.pid;
    pthread_mutex_unlock(&g_run.mtx);
    if (pid > 0) kill(-pid, sig);
}

static void meeting_stop(void)
{
    // 1. 写 q 让子进程优雅结束并生成最终纪要，随后关写端。
    meeting_request_stop();

    // 2. 等子进程退出（读线程收尸并置 child_running=0）。
    //    真实服务端需要清空 ASR、生成最终理解并下载完整快照，给 60s 宽限。
    //    超时按进程组 TERM/KILL 兜底——服务端已收到 end 请求会自行收尾，
    //    不留下孤儿 meeting_demo。
    int i;
    for (i = 0; i < 1200; i++) {         // 最多 60s
        if (!state_child_running()) break;
        usleep(50000);
    }
    if (state_child_running()) {
        meeting_signal_process_group(SIGTERM);  // 整组：sh + meeting_demo
        for (i = 0; i < 40; i++) {       // 再等 2s
            if (!state_child_running()) break;
            usleep(50000);
        }
        if (state_child_running()) meeting_signal_process_group(SIGKILL);
    }

    // 3. join 读线程（子进程死后管道 EOF，线程必然退出）
    if (g_run.thread_started) {
        pthread_join(g_run.thread, NULL);
        g_run.thread_started = 0;
    }
}

///////////////////// UI 事件 ////////////////////

static void ui_status_set(const char *text, const char *color_hex)
{
    if (g_status_label == NULL) return;
    lv_label_set_text(g_status_label, text);
    lv_obj_set_style_text_color(g_status_label, lv_color_hex((uint32_t)strtoul(color_hex, NULL, 16)),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void ui_disp_append_line(const char *line)
{
    // 超出容量时从头部丢弃整行
    size_t add = strlen(line) + 1;   // 含 '\n'
    while (g_disp_len + add >= sizeof(g_disp_buf) && g_disp_len > 0) {
        char *nl = strchr(g_disp_buf, '\n');
        if (nl == NULL) { g_disp_len = 0; break; }
        size_t drop = (size_t)(nl - g_disp_buf) + 1;
        memmove(g_disp_buf, nl + 1, g_disp_len - drop);
        g_disp_len -= drop;
    }
    if (g_disp_len == 0) {
        strncpy(g_disp_buf, line, sizeof(g_disp_buf) - 2);
        g_disp_len = strlen(g_disp_buf);
        g_disp_buf[g_disp_len++] = '\n';
        g_disp_buf[g_disp_len] = '\0';
    } else {
        strncpy(g_disp_buf + g_disp_len, line, sizeof(g_disp_buf) - g_disp_len - 2);
        g_disp_len += strlen(g_disp_buf + g_disp_len);
        g_disp_buf[g_disp_len++] = '\n';
        g_disp_buf[g_disp_len] = '\0';
    }
}

static void ui_render_text(void)
{
    if (g_text_label == NULL) return;
    if (g_partial_line[0] != '\0')
        snprintf(g_render_buf, sizeof(g_render_buf), "%s%s\n", g_disp_buf, g_partial_line);
    else
        snprintf(g_render_buf, sizeof(g_render_buf), "%s", g_disp_buf);
    lv_label_set_text(g_text_label, g_render_buf);
    lv_obj_update_layout(g_text_container);
    lv_obj_scroll_to_y(g_text_container, LV_COORD_MAX, LV_ANIM_OFF);
}

static void ui_action_set(const char *text, uint32_t bg, uint32_t pressed, int disabled)
{
    if (g_action_btn == NULL || g_action_label == NULL) return;
    lv_label_set_text(g_action_label, text);
    lv_obj_set_style_bg_color(g_action_btn, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_action_btn, lv_color_hex(pressed), LV_PART_MAIN | LV_STATE_PRESSED);
    if (disabled) lv_obj_add_state(g_action_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(g_action_btn, LV_STATE_DISABLED);
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 1. 只把转写、理解和用户可处理的错误放到屏幕；技术日志留在 stdout。
    int got = 0;
    pthread_mutex_lock(&g_run.mtx);
    char *line;
    while (ring_pop_locked(&line)) {
        char *payload = line;
        if (strncmp(payload, "[meeting] ", 10) == 0) payload += 10;
        char *partial = strstr(payload, "[转写-中间]");
        char *final = strstr(payload, "[转写]");
        if (partial != NULL) {
            strncpy(g_partial_line, partial, sizeof(g_partial_line) - 1);
            g_partial_line[sizeof(g_partial_line) - 1] = '\0';
            got = 1;
        } else if (final != NULL) {
            g_partial_line[0] = '\0';
            ui_disp_append_line(final);
            got = 1;
        } else if (strstr(payload, "[理解#") != NULL ||
                   strstr(payload, "[纪要完成]") != NULL ||
                   strstr(payload, "[概要]") != NULL ||
                   strstr(payload, "[目标]") != NULL ||
                   strstr(payload, "[议题") != NULL ||
                   strstr(payload, "[结论]") != NULL ||
                   strstr(payload, "[待办]") != NULL) {
            if (strstr(payload, "[纪要完成]") != NULL) g_partial_line[0] = '\0';
            ui_disp_append_line(strchr(payload, '['));
            got = 1;
        } else if (strstr(payload, "[纪要已保存]") != NULL) {
            ui_disp_append_line("纪要已保存到设备");
            got = 1;
        } else if (strstr(payload, "failed") != NULL || strstr(payload, "失败") != NULL ||
                   (strstr(payload, "[meeting_demo 退出 rc=") != NULL &&
                    strstr(payload, "rc=0]") == NULL)) {
            ui_disp_append_line(payload);
            got = 1;
        }
    }
    pthread_mutex_unlock(&g_run.mtx);

    // 2. 更新文本区与状态
    if (got) ui_render_text();
    int child_running = state_child_running();
    if (child_running && g_stopping) {
        g_stop_ticks++;
        if (g_stop_ticks == 120) meeting_signal_process_group(SIGTERM);
        else if (g_stop_ticks == 124) meeting_signal_process_group(SIGKILL);
    }
    if (child_running) {
        if (g_stopping) {
            const char *label = g_return_pending ? "正在返回" : "生成纪要中";
            ui_status_set(label, "F1C40F");
            ui_action_set(label, 0x5B626A, 0x5B626A, 1);
        } else if (state_child_ready()) {
            ui_status_set("记录中", "2ECC71");
            ui_action_set("结束会议", 0xC0392B, 0x8E281D, 0);
        } else {
            ui_status_set("连接中", "F1C40F");
            ui_action_set("取消", 0x5B626A, 0x43484E, 0);
        }
    } else {
        g_stopping = 0;
        g_stop_ticks = 0;
        if (g_return_pending) {
            g_return_pending = 0;
            lv_lib_pm_OpenPrePage(&page_manager);
            return;
        }
        ui_action_set("开启会议", 0x2E7D5B, 0x1F5A41, 0);
        if (!g_started_once) ui_status_set("未开始", "F1C40F");
        else if (state_last_exit_rc() == 0) ui_status_set("已完成", "2ECC71");
        else ui_status_set("失败", "E74C3C");
    }
}

static void ui_event_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!state_child_running()) {
        lv_lib_pm_OpenPrePage(&page_manager);
        return;
    }
    if (!g_stopping) g_stop_ticks = 0;
    g_stopping = 1;
    g_return_pending = 1;
    ui_status_set("正在返回", "F1C40F");
    ui_action_set("正在返回", 0x5B626A, 0x5B626A, 1);
    meeting_request_stop();
}

static void ui_event_action(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (state_child_running()) {
        if (g_stopping) return;
        g_stopping = 1;
        g_stop_ticks = 0;
        ui_status_set("生成纪要中", "F1C40F");
        ui_action_set("生成纪要中", 0x5B626A, 0x5B626A, 1);
        meeting_request_stop();
        return;
    }

    // 新会议：清空上一场转写与纪要显示。
    pthread_mutex_lock(&g_run.mtx);
    memset(&g_run.ring, 0, sizeof(g_run.ring));
    pthread_mutex_unlock(&g_run.mtx);
    g_disp_buf[0] = '\0';
    g_disp_len = 0;
    g_partial_line[0] = '\0';
    g_return_pending = 0;
    g_stop_ticks = 0;
    if (g_text_label != NULL)
        lv_label_set_text(g_text_label, "正在连接会议服务...\n");

    if (meeting_start() != 0) {
        ui_status_set("启动失败", "E74C3C");
        ring_append("会议程序启动失败", strlen("会议程序启动失败"));
    } else {
        g_started_once = 1;
        ui_status_set("连接中", "F1C40F");
        ui_action_set("取消", 0x5B626A, 0x43484E, 0);
    }
}

///////////////////// SCREEN init ////////////////////

void ui_MeetingDemoPage_init(void)
{
    // 子进程可能在 UI 写控制键前自行退出；EPIPE 只作为写失败处理。
    signal(SIGPIPE, SIG_IGN);
    // 状态初始化
    memset(&g_run, 0, sizeof(g_run));
    g_run.pid = -1;
    g_run.stdin_fd = -1;
    g_run.stdout_fd = -1;
    g_run.last_exit_rc = -999;
    pthread_mutex_init(&g_run.mtx, NULL);
    memset(g_disp_buf, 0, sizeof(g_disp_buf));
    g_disp_len = 0;
    g_partial_line[0] = '\0';

    // 根容器
    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x1E2329), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(root, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 顶栏：返回按钮 + 标题 + 状态
    lv_obj_t *back_btn = lv_button_create(root);
    lv_obj_set_width(back_btn, 50);
    lv_obj_set_height(back_btn, 32);
    lv_obj_set_pos(back_btn, 4, 2);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(back_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, 48, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(back_btn, ui_event_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_obj_align(back_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(back_label, "<");   // ASCII 左箭头（montserrat 必有该字形）
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xD8DEE6), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_obj_set_pos(title, 62, 3);
    lv_label_set_text(title, "会议纪要");
    lv_obj_set_style_text_font(title, &ui_font_meeting22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    g_status_label = lv_label_create(root);
    lv_obj_set_width(g_status_label, LV_SIZE_CONTENT);
    lv_obj_set_height(g_status_label, LV_SIZE_CONTENT);
    lv_obj_set_pos(g_status_label, 234, 7);
    lv_label_set_text(g_status_label, "未开始");
    lv_obj_set_style_text_font(g_status_label, &ui_font_meeting14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xF1C40F), LV_PART_MAIN | LV_STATE_DEFAULT);

    // 转写文本区（滚动容器 + 自动换行 label）
    g_text_container = lv_obj_create(root);
    lv_obj_set_pos(g_text_container, 5, 38);
    lv_obj_set_size(g_text_container, 310, 150);
    lv_obj_set_style_bg_color(g_text_container, lv_color_hex(0x14181D), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_text_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_text_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_text_container, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(g_text_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_text_container, LV_SCROLLBAR_MODE_AUTO);

    g_text_label = lv_label_create(g_text_container);
    lv_label_set_long_mode(g_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_text_label, lv_pct(100));
    lv_obj_set_style_text_font(g_text_label, &ui_font_meeting14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_text_label, lv_color_hex(0xDCE3EA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_text_label, "点击「开启会议」开始录音与转写\n");

    // 底部固定动作按钮：开启 -> 结束 -> 生成中，避免状态切换造成布局跳动。
    g_action_btn = lv_button_create(root);
    lv_obj_set_pos(g_action_btn, 5, 196);
    lv_obj_set_size(g_action_btn, 310, 40);
    lv_obj_add_flag(g_action_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(g_action_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(g_action_btn, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_action_btn, lv_color_hex(0x2E7D5B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_action_btn, lv_color_hex(0x1F5A41), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g_action_btn, 150, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_add_event_cb(g_action_btn, ui_event_action, LV_EVENT_CLICKED, NULL);
    g_action_label = lv_label_create(g_action_btn);
    lv_obj_align(g_action_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(g_action_label, "开启会议");
    lv_obj_set_style_text_font(g_action_label, &ui_font_meeting14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_action_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    // 初始化状态 + UI 定时器（进程由「开启会议」按钮启动）
    g_started_once = 0;
    g_stopping = 0;
    g_return_pending = 0;
    g_stop_ticks = 0;
    g_ui_timer = lv_timer_create(ui_timer_cb, 500, NULL);   // 转写/状态

    lv_scr_load_anim(root, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

///////////////////// SCREEN deinit ////////////////////

void ui_MeetingDemoPage_deinit(void)
{
    meeting_stop();
    if (g_ui_timer != NULL) {
        lv_timer_delete(g_ui_timer);
        g_ui_timer = NULL;
    }
    pthread_mutex_destroy(&g_run.mtx);
}
