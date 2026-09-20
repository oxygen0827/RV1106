/*
 * deskbot-launcher-smoke —— 复刻 ui_MeetingDemoPage 的进程管道链路做板端冒烟：
 *   fork/exec meeting-demo-run.sh（stdin 与合并后的 stdout/stderr 走管道）
 *   → 校验 Session、transcribe WS、partial/final 转写、最终理解与本地纪要
 *   → 写入 q，校验子进程优雅结束 rc=0 且无孤儿。
 * 不依赖 LVGL/DeskBot，可在板端单独验证「图标页面的启动逻辑」。
 * 用法: deskbot-launcher-smoke [SERVER]   (默认 ws://127.0.0.1:8700)
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_stdin_fd = -1;
static int g_stdout_fd = -1;
static pid_t g_pid = -1;
static pthread_t g_thread;
static atomic_int g_exit_rc = -999;
static atomic_int g_child_done;
static atomic_int g_session_created;
static atomic_int g_transcribe_open;
static atomic_int g_ready;
static atomic_int g_partial_count;
static atomic_int g_final_count;
static atomic_int g_session_ended;
static atomic_int g_minutes_complete;
static atomic_int g_record_saved;
static atomic_int g_goal_seen;
static atomic_int g_topic_seen;
static atomic_int g_consensus_seen;
static atomic_int g_todo_seen;

static void observe_child_line(const char *line)
{
    if (strstr(line, "session created:") != NULL) atomic_store(&g_session_created, 1);
    if (strstr(line, "[transcribe] open") != NULL) atomic_store(&g_transcribe_open, 1);
    if (strstr(line, "ready. ") != NULL) atomic_store(&g_ready, 1);
    if (strstr(line, "[转写-中间]") != NULL) atomic_fetch_add(&g_partial_count, 1);
    if (strstr(line, "[转写]") != NULL) atomic_fetch_add(&g_final_count, 1);
    if (strstr(line, " ended: status=200") != NULL) atomic_store(&g_session_ended, 1);
    if (strstr(line, "[纪要完成]") != NULL) atomic_store(&g_minutes_complete, 1);
    if (strstr(line, "[纪要已保存]") != NULL) atomic_store(&g_record_saved, 1);
    if (strstr(line, "[目标]") != NULL) atomic_store(&g_goal_seen, 1);
    if (strstr(line, "[议题1]") != NULL) atomic_store(&g_topic_seen, 1);
    if (strstr(line, "[结论]") != NULL) atomic_store(&g_consensus_seen, 1);
    if (strstr(line, "[待办]") != NULL) atomic_store(&g_todo_seen, 1);
}

static void *reader_thread(void *arg)
{
    (void)arg;
    char buf[512];
    char line[1024];
    size_t line_len = 0;
    for (;;) {
        ssize_t n = read(g_stdout_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n') {
                    if (line_len > 0) {
                        line[line_len] = '\0';
                        observe_child_line(line);
                        printf("  [child] %s\n", line);
                        fflush(stdout);
                        line_len = 0;
                    }
                } else if (c != '\r' && line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
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
    if (line_len > 0) {
        line[line_len] = '\0';
        observe_child_line(line);
        printf("  [child] %s\n", line);
    }
    int st = 0;
    if (waitpid(g_pid, &st, 0) == g_pid) {
        atomic_store(&g_exit_rc, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        printf("[smoke] child exited rc=%d\n", atomic_load(&g_exit_rc));
    }
    atomic_store(&g_child_done, 1);
    return NULL;
}

static int start_child(const char *server)
{
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);   // 自成进程组，兜底时整组可杀（sh + meeting_demo）
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        char sh_cmd[512];
        snprintf(sh_cmd, sizeof(sh_cmd),
                 "APP=/root/meeting_demo MODE=listen SERVER=%s "
                 "EXTRA_ARGS='--vad 0 --record /tmp/meeting-smoke-latest.json' "
                 "exec /root/meeting_demo/meeting-demo-run.sh", server);
        execl("/bin/sh", "sh", "-c", sh_cmd, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    g_pid = pid;
    g_stdin_fd = in_pipe[1];
    g_stdout_fd = out_pipe[0];
    fcntl(g_stdout_fd, F_SETFL, fcntl(g_stdout_fd, F_GETFL) | O_NONBLOCK);
    if (pthread_create(&g_thread, NULL, reader_thread, NULL) != 0) return -1;
    return 0;
}

static void send_key(const char *s)
{
    if (g_stdin_fd >= 0) write(g_stdin_fd, s, strlen(s));
}

static void wait_child_dead(int timeout_s)
{
    // 宽限期内等优雅退出；超时按进程组 TERM(2s)/KILL 兜底，
    // 与 ui_MeetingDemoPage 的 meeting_stop() 策略一致。
    for (int i = 0; i < timeout_s * 20; i++) {
        if (kill(-g_pid, 0) != 0) break;  // 进程组已不存在
        usleep(50000);
    }
    if (kill(-g_pid, 0) == 0) {
        printf("[smoke] timeout, SIGTERM group\n");
        kill(-g_pid, SIGTERM);
        usleep(2000000);
        if (kill(-g_pid, 0) == 0) kill(-g_pid, SIGKILL);
        usleep(500000);
    }
    pthread_join(g_thread, NULL);
}

static int record_is_complete(void)
{
    FILE *fp = fopen("/tmp/meeting-smoke-latest.json", "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    long size = ftell(fp);
    if (size <= 0 || size > 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return 0; }
    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[n] = '\0';
    int complete = n == (size_t)size &&
                   strstr(buf, "\"transcript\"") != NULL &&
                   strstr(buf, "\"understanding\"") != NULL &&
                   strstr(buf, "\"analysis_final\" : true") != NULL &&
                   strstr(buf, "\"sourceComplete\" : true") != NULL &&
                   strstr(buf, "\"analysisComplete\" : true") != NULL &&
                   strstr(buf, "\"state\" : \"final\"") != NULL &&
                   strstr(buf, "\"transcript_count\" : 2") != NULL &&
                   strstr(buf, "\"lastSuccessfulCursor\" : 2") != NULL &&
                   strstr(buf, "\"pendingTranscriptCount\" : 0") != NULL &&
                   strstr(buf, "\"dropped_frames\" : 0") != NULL &&
                   strstr(buf, "同比增长了百分之二十三") != NULL &&
                   strstr(buf, "复盘第三季度增长并明确后续动作") != NULL &&
                   strstr(buf, "第三季度增长") != NULL &&
                   strstr(buf, "整理海外市场增长明细") != NULL;
    free(buf);
    return complete;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    unlink("/tmp/meeting-smoke-latest.json");
    const char *server = (argc > 1) ? argv[1] : "ws://127.0.0.1:8700";
    printf("[smoke] server=%s\n", server);
    if (start_child(server) != 0) { perror("start_child"); return 1; }
    printf("[smoke] child pid=%d\n", (int)g_pid);

    // 等到第一句 final、第二句仍为 partial 时立即结束，验证 end 会把尾句
    // 补成 final 并纳入最终理解；--vad 0 使检查不受环境音量影响。
    int tail_barrier = 0;
    for (int i = 0; i < 200 && !atomic_load(&g_child_done); i++) {
        int finals = atomic_load(&g_final_count);
        if (finals == 1 && atomic_load(&g_partial_count) >= 2) {
            tail_barrier = 1;
            break;
        }
        // If the second ordinary final already arrived, this run did not test
        // end-triggered tail finalization and must fail instead of false-passing.
        if (finals >= 2) break;
        usleep(100000);
    }
    printf("[smoke] core session=%d ws=%d ready=%d partial=%d final=%d tail_barrier=%d\n",
           atomic_load(&g_session_created), atomic_load(&g_transcribe_open),
           atomic_load(&g_ready), atomic_load(&g_partial_count),
           atomic_load(&g_final_count), tail_barrier);
    printf("[smoke] q (结束会议)\n");  send_key("q\n");

    wait_child_dead(65);
    if (g_stdin_fd >= 0) close(g_stdin_fd);
    g_stdin_fd = -1;

    // 验收 1：进程组已清干净（sh 被收尸、无残留进程组）
    int group_gone = (kill(-g_pid, 0) != 0);
    // 验收 2：无孤儿 meeting_demo（kill(0,0) 只能看组，直接扫 /proc comm）
    FILE *fp = popen("pidof meeting_demo", "r");
    char buf[64] = {0};
    if (fp) { fread(buf, 1, sizeof(buf) - 1, fp); pclose(fp); }
    int orphan = (buf[0] != '\0');
    int record_ok = record_is_complete();
    int core_ok = tail_barrier && atomic_load(&g_session_created) &&
                  atomic_load(&g_transcribe_open) &&
                  atomic_load(&g_ready) && atomic_load(&g_partial_count) >= 2 &&
                  atomic_load(&g_final_count) >= 2 && atomic_load(&g_session_ended) &&
                  atomic_load(&g_minutes_complete) && atomic_load(&g_record_saved) && record_ok;
    core_ok = core_ok && atomic_load(&g_goal_seen) && atomic_load(&g_topic_seen) &&
              atomic_load(&g_consensus_seen) && atomic_load(&g_todo_seen);
    int exit_rc = atomic_load(&g_exit_rc);
    int pass = core_ok && exit_rc == 0 && group_gone && !orphan;
    printf("[smoke] end=%d minutes=%d saved=%d record=%d fields=%d/%d/%d/%d "
           "partial=%d final=%d "
           "group_gone=%d orphan=%d child_rc=%d\n",
           atomic_load(&g_session_ended), atomic_load(&g_minutes_complete),
           atomic_load(&g_record_saved), record_ok, atomic_load(&g_goal_seen),
           atomic_load(&g_topic_seen), atomic_load(&g_consensus_seen),
           atomic_load(&g_todo_seen), atomic_load(&g_partial_count),
           atomic_load(&g_final_count), group_gone, orphan, exit_rc);
    printf("[smoke] result: %s\n",
           pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
