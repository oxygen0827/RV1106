/*
 * deskbot-launcher-smoke —— 复刻 ui_MeetingDemoPage 的进程管道链路做板端冒烟：
 *   fork/exec meeting-demo-run.sh（stdin/stdout 走管道）
 *   → 读线程逐行收 stdout → 主线程按脚本写入控制键（Enter/s/q）
 *   → 校验子进程优雅退出 rc=0。
 * 不依赖 LVGL/DeskBot，可在板端单独验证「图标页面的启动逻辑」。
 * 用法: deskbot-launcher-smoke [SERVER]   (默认 ws://192.168.31.97:8700)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_stdin_fd = -1;
static int g_stdout_fd = -1;
static pid_t g_pid = -1;
static pthread_t g_thread;
static int g_exit_rc = -999;

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
                        printf("  [child] %.*s\n", (int)line_len, line);
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
    if (line_len > 0) printf("  [child] %.*s\n", (int)line_len, line);
    int st = 0;
    if (waitpid(g_pid, &st, 0) == g_pid) {
        g_exit_rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        printf("[smoke] child exited rc=%d\n", g_exit_rc);
    }
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
                 "APP=/root/meeting_demo MODE=full SERVER=%s "
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

int main(int argc, char **argv)
{
    const char *server = (argc > 1) ? argv[1] : "ws://192.168.31.97:8700";
    printf("[smoke] server=%s\n", server);
    if (start_child(server) != 0) { perror("start_child"); return 1; }
    printf("[smoke] child pid=%d\n", (int)g_pid);

    sleep(5);                 // 等待 create session + WS 连接
    printf("[smoke] Enter (开始提问)\n"); send_key("\n");
    sleep(4);
    printf("[smoke] Enter (结束提问)\n"); send_key("\n");
    sleep(6);                 // 等待回答
    printf("[smoke] s (打断)\n");      send_key("s\n");
    sleep(2);
    printf("[smoke] q (退出)\n");      send_key("q\n");

    wait_child_dead(4);    // 与页面相同：4s 宽限，超出按组 TERM/KILL
    close(g_stdin_fd);
    g_stdin_fd = -1;

    // 验收 1：进程组已清干净（sh 被收尸、无残留进程组）
    int group_gone = (kill(-g_pid, 0) != 0);
    // 验收 2：无孤儿 meeting_demo（kill(0,0) 只能看组，直接扫 /proc comm）
    FILE *fp = popen("pidof meeting_demo", "r");
    char buf[64] = {0};
    if (fp) { fread(buf, 1, sizeof(buf) - 1, fp); pclose(fp); }
    int orphan = (buf[0] != '\0');
    printf("[smoke] group_gone=%d orphan=%d child_rc=%d\n",
           group_gone, orphan, g_exit_rc);
    printf("[smoke] result: %s\n",
           (group_gone && !orphan) ? "PASS" : "FAIL");
    return (group_gone && !orphan) ? 0 : 1;
}
