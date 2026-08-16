/**
 * ui_MeetingDemoPage —— 会议纪要 demo 启动页
 *
 * 页面职责：
 *   1. fork/exec /root/meeting_demo/meeting-demo-run.sh（SERVER 优先读
 *      /root/meeting_demo/server.conf，缺省 ws://192.168.31.97:8700）。
 *   2. 后台读线程从子进程合并后的 stdout/stderr 管道收行，推入环形缓冲；
 *      500ms LVGL 定时器在 UI 线程取出行并追加到转写文本区（LVGL
 *      对象只由 UI 线程触碰）。
 *   3. 基础 Demo 使用 listen 模式；退出按钮向子进程 stdin 写 q。
 *
 * 生命周期：init 启动进程；deinit/返回按钮先写 q 退出子进程（SIGTERM/
 * SIGKILL 兜底），join 读线程后销毁定时器。可重复进入/退出。
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

#define LINE_MAX_BYTES    400   // 单行最大字节（约 130 个汉字）
#define LOG_MAX_LINES     40    // 环形缓冲最多保留行数（控制整段重渲染成本）
#define DISP_MAX_BYTES    (LOG_MAX_LINES * (LINE_MAX_BYTES + 1) + 64)  // 显示缓冲

#define MEETING_SH_CMD \
    "APP=/root/meeting_demo MODE=listen " \
    "SERVER=$(cat /root/meeting_demo/server.conf 2>/dev/null || echo ws://192.168.31.97:8700) " \
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
    pthread_t  thread;
    pthread_mutex_t mtx;        // 保护 ring 与上述状态字段
    line_ring_t ring;
} meeting_run_t;

static meeting_run_t g_run;

static lv_timer_t *g_ui_timer;    // 500ms：转写文本 + 状态 + 按钮显隐
static lv_obj_t   *g_status_label;
static lv_obj_t   *g_text_label;
static lv_obj_t   *g_text_container;
static lv_obj_t   *g_start_btn;
static int         g_started_once;   // 是否至少启动过一次（区分 未开始/已停止）
static int         g_stopping;       // 正在退出收尾（定时器不再刷状态）

// 显示缓冲（仅 UI 线程访问）
static char g_disp_buf[DISP_MAX_BYTES];
static size_t g_disp_len;

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

// 原子地取走 stdin 写端，确保读线程与退出路径只有一方关闭。
static int state_take_stdin_fd(void)
{
    pthread_mutex_lock(&g_run.mtx);
    int fd = g_run.stdin_fd;
    g_run.stdin_fd = -1;
    pthread_mutex_unlock(&g_run.mtx);
    return fd;
}

static void state_set_child_running(int v)
{
    pthread_mutex_lock(&g_run.mtx);
    g_run.child_running = v;
    if (v == 0) g_run.pid = -1;
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
    if (w == g_run.pid) {
        char msg[96];
        snprintf(msg, sizeof(msg), "[meeting_demo 退出 rc=%d]", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        ring_append(msg, strlen(msg));
    } else {
        ring_append("[meeting_demo 已结束]", strlen("[meeting_demo 已结束]"));
    }
    if (g_run.stdout_fd >= 0) { close(g_run.stdout_fd); g_run.stdout_fd = -1; }
    int stdin_fd = state_take_stdin_fd();
    if (stdin_fd >= 0) close(stdin_fd);
    state_set_child_running(0);
    return NULL;
}

///////////////////// 启动 / 停止 ////////////////////

static int meeting_start(void)
{
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

    if (pthread_create(&g_run.thread, NULL, meeting_reader_thread, NULL) != 0) {
        g_run.thread_started = 0;
        return -1;
    }
    g_run.thread_started = 1;
    return 0;
}

static void meeting_stdin_write(const char *s)
{
    pthread_mutex_lock(&g_run.mtx);
    int fd = g_run.stdin_fd;
    if (fd >= 0) {
        ssize_t n = write(fd, s, strlen(s));
        (void)n;
    }
    pthread_mutex_unlock(&g_run.mtx);
}

static void meeting_stop(void)
{
    // 1. 写 q 让子进程优雅退出（meeting_demo 的 stdin 协议），随后关写端
    int fd = state_take_stdin_fd();
    if (fd >= 0) {
        write(fd, "q\n", 2);
        close(fd);
    }

    // 2. 等子进程退出（读线程收尸并置 child_running=0）。
    //    q 路径在本地 mock 下 <1s；真实服务端 session/end HTTP 实测 5~15s
    //    （板端 Wi-Fi 上行慢）。基础 Demo 给 12s 宽限，
    //    超时按进程组 TERM/KILL 兜底——服务端已收到 end 请求会自行收尾，
    //    不留下孤儿 meeting_demo。
    int i;
    for (i = 0; i < 240; i++) {          // 最多 12s
        if (!state_child_running()) break;
        usleep(50000);
    }
    if (state_child_running()) {
        pthread_mutex_lock(&g_run.mtx);
        pid_t pid = g_run.pid;
        pthread_mutex_unlock(&g_run.mtx);
        if (pid > 0) kill(-pid, SIGTERM);       // 整组：sh + meeting_demo
        for (i = 0; i < 40; i++) {       // 再等 2s
            if (!state_child_running()) break;
            usleep(50000);
        }
        if (state_child_running() && pid > 0) kill(-pid, SIGKILL);
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

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 1. 取环形缓冲中的新行
    int got = 0;
    pthread_mutex_lock(&g_run.mtx);
    char *line;
    while (ring_pop_locked(&line)) {
        ui_disp_append_line(line);
        got = 1;
    }
    pthread_mutex_unlock(&g_run.mtx);

    // 2. 更新文本区与状态
    if (got && g_text_label != NULL) {
        lv_label_set_text(g_text_label, g_disp_buf);
        lv_obj_update_layout(g_text_container);
        lv_obj_scroll_to_y(g_text_container, LV_COORD_MAX, LV_ANIM_OFF);
    }
    if (g_stopping) return;   // 退出收尾中，状态保持「正在退出...」
    if (state_child_running()) {
        if (g_start_btn != NULL) lv_obj_add_flag(g_start_btn, LV_OBJ_FLAG_HIDDEN);
        ui_status_set("运行中", "2ECC71");
    } else {
        if (g_start_btn != NULL) lv_obj_remove_flag(g_start_btn, LV_OBJ_FLAG_HIDDEN);
        if (g_started_once) ui_status_set("已停止", "E74C3C");
        else ui_status_set("未开始", "F1C40F");
    }
}

static void ui_event_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_stopping = 1;
    ui_status_set("正在退出...", "F1C40F");
    meeting_stop();
    g_stopping = 0;
    lv_lib_pm_OpenPrePage(&page_manager);
}

static void ui_event_ask(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    // 按住开始提问，松开结束提问（meeting_demo 的 Enter 是切换语义）
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED) {
        meeting_stdin_write("\n");
    }
}

static void ui_event_start(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (state_child_running()) return;   // 已运行，忽略

    // 新会议：清空转写显示
    g_disp_buf[0] = '\0';
    g_disp_len = 0;
    if (g_text_label != NULL)
        lv_label_set_text(g_text_label, "正在启动 meeting_demo ...\n");

    if (meeting_start() != 0) {
        ui_status_set("启动失败", "E74C3C");
        ring_append("meeting_demo 启动失败（检查 /root/meeting_demo）",
                    strlen("meeting_demo 启动失败（检查 /root/meeting_demo）"));
    } else {
        g_started_once = 1;
        ui_status_set("运行中", "2ECC71");
    }
}

static void ui_event_interrupt(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        meeting_stdin_write("s\n");
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
    pthread_mutex_init(&g_run.mtx, NULL);
    memset(g_disp_buf, 0, sizeof(g_disp_buf));
    g_disp_len = 0;

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
    lv_obj_set_size(g_text_container, 310, 102);
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

    // 「开启会议」按钮（进程未运行时可见）
    g_start_btn = lv_button_create(root);
    lv_obj_set_pos(g_start_btn, 5, 156);
    lv_obj_set_size(g_start_btn, 310, 36);
    lv_obj_add_flag(g_start_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(g_start_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(g_start_btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_start_btn, lv_color_hex(0x2E7D5B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_start_btn, lv_color_hex(0x1F5A41), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_start_btn, ui_event_start, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_label = lv_label_create(g_start_btn);
    lv_obj_align(start_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(start_label, "开启会议");
    lv_obj_set_style_text_font(start_label, &ui_font_meeting22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(start_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    // 底部按钮行
    lv_obj_t *ask_btn = lv_button_create(root);
    lv_obj_set_pos(ask_btn, 5, 196);
    lv_obj_set_size(ask_btn, 120, 40);
    lv_obj_add_flag(ask_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(ask_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ask_btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ask_btn, lv_color_hex(0x2E86DE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ask_btn, lv_color_hex(0x1B5E9E), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ask_btn, ui_event_ask, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ask_btn, ui_event_ask, LV_EVENT_RELEASED, NULL);
    lv_obj_t *ask_label = lv_label_create(ask_btn);
    lv_obj_align(ask_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ask_label, "按住提问");
    lv_obj_set_style_text_font(ask_label, &ui_font_meeting14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ask_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ask_btn, LV_OBJ_FLAG_HIDDEN);  // Host 问答不进入基础 Demo

    lv_obj_t *intr_btn = lv_button_create(root);
    lv_obj_set_pos(intr_btn, 135, 196);
    lv_obj_set_size(intr_btn, 75, 40);
    lv_obj_add_flag(intr_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(intr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(intr_btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(intr_btn, lv_color_hex(0xE67E22), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(intr_btn, lv_color_hex(0xB55E16), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(intr_btn, ui_event_interrupt, LV_EVENT_CLICKED, NULL);
    lv_obj_t *intr_label = lv_label_create(intr_btn);
    lv_obj_align(intr_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(intr_label, "打断");
    lv_obj_set_style_text_font(intr_label, &ui_font_meeting14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(intr_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(intr_btn, LV_OBJ_FLAG_HIDDEN);  // 随 Host 问答一并延后

    lv_obj_t *quit_btn = lv_button_create(root);
    lv_obj_set_pos(quit_btn, 5, 196);
    lv_obj_set_size(quit_btn, 305, 40);
    lv_obj_add_flag(quit_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(quit_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(quit_btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(quit_btn, lv_color_hex(0xC0392B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(quit_btn, lv_color_hex(0x8E281D), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(quit_btn, ui_event_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *quit_label = lv_label_create(quit_btn);
    lv_obj_align(quit_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(quit_label, "退出");
    lv_obj_set_style_text_font(quit_label, &ui_font_meeting14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(quit_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    // 初始化状态 + UI 定时器（进程由「开启会议」按钮启动）
    g_started_once = 0;
    g_stopping = 0;
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
