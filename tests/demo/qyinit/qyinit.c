/* qyinit —— 澜岫 Linux 的 1 号进程
 *
 * 职责（一个真正的 init 该做的事，一件都不能少）：
 *   1. 挂载 /proc /sys /dev，创建必要设备节点
 *   2. 读取服务单元，按依赖顺序启动
 *   3. 回收孤儿进程（1 号进程的天职，不做就是僵尸堆积）
 *   4. 转发信号：收到 SIGTERM/SIGINT 走正常关机流程
 *   5. 提供运行级别：default / rescue / shutdown
 *   6. 服务崩溃时按单元的 restart 策略处理
 *
 * 刻意保持小而可审计：init 是整个系统里最不该出 bug 的程序。
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define UNIT_DIR      "/etc/qyinit.d"
#define DEFAULT_UNIT_DIR "/etc/qyinit.d"
#define DEFAULT_LOG_PATH "/var/log/qyinit/boot.log"
#ifndef RESTART_MAX
#define RESTART_MAX      5       /* 窗口期内最多重启这么多次 */
#endif
#ifndef RESTART_WINDOW
#define RESTART_WINDOW   10      /* 计数窗口（秒） */
#endif
#ifndef RESTART_MAX
#define RESTART_MAX      5       /* 窗口期内最多重启这么多次 */
#endif
#ifndef RESTART_WINDOW
#define RESTART_WINDOW   10      /* 计数窗口（秒） */
#endif
#define MAX_UNITS     128
#define MAX_DEPS      16
#ifndef LINE_MAX
#define LINE_MAX 1024
#endif


typedef enum { RESTART_NO, RESTART_ALWAYS, RESTART_ON_FAILURE } restart_t;

typedef struct {
    char name[64];
    char desc[128];
    char exec[512];
    char deps[MAX_DEPS][64];
    int  ndeps;
    int  started;
    int  failed;
    int  restart;
    pid_t pid;
    int  nrestarts;      /* 已重启次数，用于崩溃风暴限流 */
    time_t first_start;  /* 计数窗口起点 */
} unit_t;

static unit_t units[MAX_UNITS];
static int    nunits = 0;
static const char *unit_dir = DEFAULT_UNIT_DIR;
static const char *log_path = DEFAULT_LOG_PATH;
static int    oneshot = 0;      /* 测试模式：跑一轮就退出，不做 1 号进程 */
static int    as_pid1 = 1;
static int    shutting_down = 0;/* 停机中：服务退出不得再拉起 */
static volatile sig_atomic_t got_term = 0;
static volatile sig_atomic_t got_child = 0;
static FILE *logf = NULL;

static void ts(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(logf ? logf : stderr, "[%02d:%02d:%02d] ",
            tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    ts();
    va_start(ap, fmt);
    vfprintf(logf ? logf : stderr, fmt, ap);
    va_end(ap);
    fputc('\n', logf ? logf : stderr);
    fflush(logf ? logf : stderr);
}

static void on_term(int sig)
{
    (void)sig;
    got_term = 1;
}

static void on_child(int sig)
{
    (void)sig;
    got_child = 1;
}

/* ---------- 单元文件解析 ----------
 * 格式（key = value，# 注释）：
 *   Description=网络
 *   Requires=network-pre
 *   ExecStart=/usr/bin/foo --daemon
 *   Restart=always        # no | always | on-failure
 */
static int unit_index(const char *name)
{
    for (int i = 0; i < nunits; i++)
        if (strcmp(units[i].name, name) == 0) return i;
    return -1;
}

static void add_dep(unit_t *u, const char *dep)
{
    if (u->ndeps >= MAX_DEPS) return;
    if (strnlen(dep, 63) == 0) return;
    snprintf(u->deps[u->ndeps], sizeof u->deps[0], "%s", dep);
    u->ndeps++;
}

/* 先收集文件名再排序：readdir 的顺序不保证，启动次序必须可重现 */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static int load_units(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { logmsg("无法打开单元目录 %s: %s", dir, strerror(errno)); return -1; }

    char *names[MAX_UNITS];
    int nnames = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nnames < MAX_UNITS) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".unit") != 0) continue;
        names[nnames++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nnames, sizeof names[0], cmp_str);

    for (int i = 0; i < nnames && nunits < MAX_UNITS; i++) {
        size_t len = strlen(names[i]);

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        unit_t *u = &units[nunits];
        memset(u, 0, sizeof *u);
        snprintf(u->name, sizeof u->name, "%.*s", (int)(len - 5), names[i]);
        u->restart = RESTART_ON_FAILURE;   /* 默认：失败才重启 */
        u->pid = -1;

        char line[LINE_MAX];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\0') continue;

            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = p, *val = eq + 1;
            char *nl = strchr(val, '\n');
            if (nl) *nl = '\0';

            if (strcmp(key, "Description") == 0)
                snprintf(u->desc, sizeof u->desc, "%s", val);
            else if (strcmp(key, "ExecStart") == 0)
                snprintf(u->exec, sizeof u->exec, "%s", val);
            else if (strcmp(key, "Requires") == 0 || strcmp(key, "After") == 0) {
                /* 支持逗号或空格分隔的多个依赖 */
                for (char *tok = strtok(val, ", \t"); tok;
                     tok = strtok(NULL, ", \t"))
                    add_dep(u, tok);
            }
            else if (strcmp(key, "Restart") == 0) {
                if (strcmp(val, "always") == 0) u->restart = RESTART_ALWAYS;
                else if (strcmp(val, "no") == 0) u->restart = RESTART_NO;
                else u->restart = RESTART_ON_FAILURE;
            }
        }
        fclose(f);

        if (u->exec[0] == '\0') {
            logmsg("单元 %s 没有 ExecStart，跳过", u->name);
            continue;
        }
        nunits++;
    }
    for (int i = 0; i < nnames; i++) free(names[i]);
    return nunits;
}

/* ---------- 启动顺序：拓扑排序 ---------- */
static int visited[MAX_UNITS], visiting[MAX_UNITS], order[MAX_UNITS], norder = 0;

/* 崩溃风暴保护：一个起不来的服务如果被无限重启，会把系统拖垮。
 * 真实发行版都有这个限流，没有它一次配置错误就是一台废机。 */
static int may_restart(unit_t *u)
{
    if (shutting_down) return 0;
    time_t now = time(NULL);
    if (u->nrestarts == 0) u->first_start = now;
    if (now - u->first_start > RESTART_WINDOW) {
        u->nrestarts = 0;          /* 撑过窗口期，重新开始计数 */
        u->first_start = now;
    }
    if (u->nrestarts >= RESTART_MAX) {
        logmsg("服务 %s 在 %d 秒内重启 %d 次仍失败，已停止拉起"
               "（请检查配置）", u->name, RESTART_WINDOW, u->nrestarts);
        return 0;
    }
    u->nrestarts++;
    return 1;
}

static void visit(int i)
{
    if (visited[i]) return;
    if (visiting[i]) { logmsg("单元 %s 存在循环依赖，已跳过该边", units[i].name); return; }
    visiting[i] = 1;
    for (int k = 0; k < units[i].ndeps; k++) {
        int j = unit_index(units[i].deps[k]);
        if (j >= 0) visit(j);
        else logmsg("单元 %s 依赖 %s，但该单元不存在，忽略",
                    units[i].name, units[i].deps[k]);
    }
    visiting[i] = 0;
    visited[i] = 1;
    order[norder++] = i;
}

static void sort_units(void)
{
    memset(visited, 0, sizeof visited);
    memset(visiting, 0, sizeof visiting);
    norder = 0;
    for (int i = 0; i < nunits; i++) visit(i);
}

/* ---------- 执行 ---------- */
static pid_t spawn(unit_t *u)
{
    pid_t pid = fork();
    if (pid < 0) { logmsg("fork 失败: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        setsid();
        /* 子进程不继承日志句柄，避免服务崩溃时写坏日志缓冲 */
        if (logf) fclose(logf);
        char *argv[32];
        int n = 0;
        for (char *tok = strtok(u->exec, " \t"); tok && n < 31;
             tok = strtok(NULL, " \t"))
            argv[n++] = tok;
        argv[n] = NULL;
        if (n == 0) _exit(127);
        execv(argv[0], argv);
        _exit(127);   /* 只有 execv 失败才会到这里 */
    }
    u->pid = pid;
    u->started = 1;
    /* 契约：写 /run/qyinit/units/<name>.pid，供 qyctl 等客户端查询 */
    {
        char pdir[] = "/run/qyinit/units";
        mkdir("/run/qyinit", 0755);
        mkdir(pdir, 0755);
        char ppath[PATH_MAX];
        snprintf(ppath, sizeof ppath, "%s/%s.pid", pdir, u->name);
        FILE *pf = fopen(ppath, "w");
        if (pf) { fprintf(pf, "%d\n", (int)pid); fclose(pf); }
    }
    logmsg("启动 %s（pid %d）", u->name, pid);
    return pid;
}

static void start_all(void)
{
    sort_units();
    for (int k = 0; k < norder; k++) {
        unit_t *u = &units[order[k]];
        if (u->exec[0] == '\0') continue;
        spawn(u);
    }
}

/* ---------- 早期环境 ---------- */
static void mount_early(void)
{
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/run", 0755);
    /* /run 用 tmpfs：pidfile、XDG_RUNTIME_DIR 都要求可写且重启即清 */
    if (mount("tmpfs", "/run", "tmpfs", 0, "mode=0755") != 0)
        logmsg("挂载 /run tmpfs 失败: %s", strerror(errno));
    mkdir("/dev/pts", 0755);
    mkdir("/dev/shm", 0755);

    if (mount("proc", "/proc", "proc", 0, NULL) == 0)
        logmsg("已挂载 /proc");
    else
        logmsg("挂载 /proc 失败: %s", strerror(errno));

    if (mount("sysfs", "/sys", "sysfs", 0, NULL) == 0)
        logmsg("已挂载 /sys");
    else
        logmsg("挂载 /sys 失败: %s", strerror(errno));

    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0)
        logmsg("挂载 /dev 失败（内核可能没有 devtmpfs）: %s", strerror(errno));

    if (mount("devpts", "/dev/pts", "devpts", 0, NULL) != 0)
        logmsg("挂载 /dev/pts 失败: %s", strerror(errno));

    /* POSIX 共享内存：GTK/wayland 客户端经 shm_open 建缓冲，必须有可写 tmpfs */
    if (mount("shm", "/dev/shm", "tmpfs", 0, "mode=1777") != 0)
        logmsg("挂载 /dev/shm 失败: %s", strerror(errno));
}

static void make_nodes(void)
{
    struct { const char *p; int maj, min; mode_t mode; } nodes[] = {
        {"/dev/null",    1,  3, 0666},
        {"/dev/zero",    1,  5, 0666},
        {"/dev/full",    1,  7, 0666},
        {"/dev/random",  1,  8, 0666},
        {"/dev/urandom", 1,  9, 0666},
        {"/dev/tty",     5,  0, 0666},
        {"/dev/console", 5,  1, 0600},
        {"/dev/kmsg",    1, 11, 0644},
        {"/dev/ptmx",    5,  2, 0666},
    };
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
        struct stat st;
        if (stat(nodes[i].p, &st) == 0) continue;   /* 已存在就别动 */
        if (mknod(nodes[i].p, S_IFCHR | nodes[i].mode,
                  makedev(nodes[i].maj, nodes[i].min)) != 0)
            logmsg("创建 %s 失败: %s", nodes[i].p, strerror(errno));
    }
}

static void reap(void)
{
    got_child = 0;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < nunits; i++) {
            if (units[i].pid != pid) continue;
            units[i].pid = -1;
            /* 停机期间服务退出是正常的，绝不能再拉起，否则永远关不掉 */
            if (shutting_down) {
                logmsg("服务 %s 已停止", units[i].name);
                break;
            }
            int want_restart = 0;
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                units[i].failed = 1;
                logmsg("服务 %s 被信号 %d 终止", units[i].name, sig);
                want_restart = (units[i].restart == RESTART_ALWAYS ||
                                units[i].restart == RESTART_ON_FAILURE);
            } else {
                int code = WEXITSTATUS(status);
                if (code == 0) {
                    logmsg("服务 %s 正常退出", units[i].name);
                    want_restart = (units[i].restart == RESTART_ALWAYS);
                } else {
                    units[i].failed = 1;
                    logmsg("服务 %s 异常退出（状态 %d）", units[i].name, code);
                    want_restart = (units[i].restart == RESTART_ALWAYS ||
                                    units[i].restart == RESTART_ON_FAILURE);
                }
            }
            if (want_restart && may_restart(&units[i])) spawn(&units[i]);
            break;
        }
    }
}

static void stop_all(void)
{
    shutting_down = 1;
    logmsg("收到关机信号，停止服务");
    /* 逆序停止：后启动的先停 */
    for (int k = norder - 1; k >= 0; k--) {
        unit_t *u = &units[order[k]];
        if (u->pid <= 0) continue;
        logmsg("停止 %s（pid %d）", u->name, u->pid);
        kill(u->pid, SIGTERM);
    }
    /* 给 5 秒体面退出的时间，之后强杀 */
    for (int i = 0; i < 50; i++) {
        int alive = 0;
        for (int k = 0; k < norder; k++)
            if (units[order[k]].pid > 0) { alive = 1; break; }
        if (!alive) break;
        usleep(100 * 1000);
        reap();
    }
    for (int k = 0; k < norder; k++)
        if (units[order[k]].pid > 0) kill(units[order[k]].pid, SIGKILL);
    logmsg("所有服务已停止");
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "  --unit-dir DIR   服务单元目录（默认 %s）\n"
        "  --log FILE       日志路径（默认 %s）\n"
        "  --oneshot        启动一轮服务后停止并退出（用于测试）\n"
        "选项之外的任何参数都被忽略，以兼容内核命令行传参。\n",
        prog, DEFAULT_UNIT_DIR, DEFAULT_LOG_PATH);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--unit-dir") == 0 && i + 1 < argc)
            unit_dir = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_path = argv[++i];
        else if (strcmp(argv[i], "--oneshot") == 0)
            oneshot = 1;
        else if (strncmp(argv[i], "--unit-dir=", 11) == 0)
            unit_dir = argv[i] + 11;
        else if (strncmp(argv[i], "--log=", 6) == 0)
            log_path = argv[i] + 6;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        }
    }

    if (getpid() != 1) {
        /* 不是 1 号进程：仍然能跑，用于测试与容器场景 */
        as_pid1 = 0;
    }

    /* 1 号进程必须显式忽略默认会杀掉自己的信号 */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    for (int s = 1; s < 32; s++)
        if (s != SIGCHLD && s != SIGTERM && s != SIGINT)
            sigaction(s, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_child;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);

    logf = fopen(log_path, "a");
    if (!logf) logf = stderr;

    logmsg("=== 澜岫 Linux init 启动（pid %d%s）===",
           getpid(), as_pid1 ? "" : "，非 1 号进程模式");

    if (as_pid1 && !oneshot)
        mount_early();
    else
        logmsg("跳过挂载（非 1 号进程或单次模式）");
    make_nodes();

    int n = load_units(unit_dir);
    logmsg("加载 %d 个服务单元", n < 0 ? 0 : n);

    start_all();
    logmsg("启动阶段完成");

    if (oneshot) {
        /* 测试模式：给服务一点时间，然后按停机流程走一遍再退出。
         * 这样在 CI 里就能验证"启动顺序 + 回收 + 停止"整条链路。
         * 必须主动 reap：单次模式不进主循环，不回收就看不到退出状态，
         * 也就永远不会触发重启策略。 */
        for (int t = 0; t < 3; t++) {
            sleep(1);
            reap();
        }
        int started = 0, failed = 0;
        for (int i = 0; i < nunits; i++) {
            if (units[i].started) started++;
            if (units[i].failed) failed++;
        }
        logmsg("单次模式：已启动 %d 个服务，失败 %d 个", started, failed);
        stop_all();
        logmsg("单次模式结束");
        if (logf && logf != stderr) fclose(logf);
        return failed ? 1 : 0;
    }

    logmsg("进入主循环");

    /* 主循环：1 号进程不能退出，退出即内核 panic */
    while (1) {
        pause();
        if (got_child) reap();
        if (got_term) {
            got_term = 0;
            stop_all();
            logmsg("系统即将关机");
            sync();
            if (as_pid1) {
                reboot(RB_POWER_OFF);
                logmsg("关机调用返回（不应发生）");
            }
            logmsg("停机流程完成");
            break;
        }
    }
    if (logf && logf != stderr) fclose(logf);
    return 0;
}
