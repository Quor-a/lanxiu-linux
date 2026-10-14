/* qydesktop - 澜岫桌面 shell v2.0（单一全屏桌面窗口）
 *
 * v2.0 架构变化：把 顶栏 + 左侧 Dock + 壁纸桌面图标 合并进**一个全屏
 * DESKTOP 窗口**，彻底规避 Wayland 下多窗口 DOCK 定位失效（gtk_window_move
 * 在 Wayland 被忽略导致 bar/dock 错位）的问题。
 *
 * 布局（1280x800）:
 *   +------------------------------+ 0
 *   | [开始] | 窗口任务栏 ... 时钟 [电源] | 30  顶栏
 *   +------+-----------------------+
 *   | Dock | 壁纸 + 桌面图标          |
 *   | [文件] |                       |
 *   | [终端] |                       |
 *   | [设置] |                       |
 *   | [软件] |                       |
 *   | [监视] |                       |
 *   | [回收站]|                       |
 *   | [应用] |                       |
 *   +------+-----------------------+
 *
 * 任务栏数据源：weston 合成器补丁每 500ms 写的 /tmp/xdg/qy-windows，
 * 点击窗口按钮写 /tmp/xdg/qy-focus 让合成器激活对应窗口。
 * 主题：/usr/share/themes/lanxiu/gtk-3.0/gtk.css（GTK CSS，qytheme.css）。
 */
#include "qyl10n.h"
#include "qyicon.h"
#include <gtk/gtk.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <glib/gstdio.h>

#define QY_WINDOWS  "/tmp/xdg/qy-windows"
#define QY_FOCUS    "/tmp/xdg/qy-focus"
#define QY_WINOP    "/tmp/xdg/qy-winop"
#define QY_CSS      "/usr/share/themes/lanxiu/gtk-3.0/gtk.css"
#define QY_WALL     "/usr/share/backgrounds/lanxiu.png"

#define SCREEN_W  1280
#define SCREEN_H  800
#define BAR_H     40
#define DOCK_W    66

static void add_class(GtkWidget *w, const char *cls);   /* 主题助手（定义见下） */

/* ---------- 应用注册表 ---------- */
/* Dock/桌面图标统一走 qyicon（task_icon_id 按应用名映射），glyph 字段已废弃删除 */
typedef struct {
    const char *name;
    const char *css;      /* 主题颜色类 */
    const char *cmdline;
    const char *exe;      /* /proc/<pid>/comm */
    int  pid;
    GtkWidget *dot;       /* 运行指示灯 */
} AppEntry;

static AppEntry apps[] = {
    { "文件",   "c-files",    "qyfiles",          "qyfiles",      0, NULL },
    { "终端",   "c-term",     "weston-terminal",  "weston-termi", 0, NULL },
    { "设置",   "c-settings", "qysettings",       "qysettings",   0, NULL },
    { "软件中心", "c-store",    "qystore",          "qystore",      0, NULL },
    { "监视",   "c-mon",     "qymon",             "qymon",        0, NULL },
    { "回收站", "c-trash",   "qyfiles --trash",   "qyfiles",      0, NULL },
    { "时钟",   "c-clock",   "qyclock",           "qyclock",      0, NULL },
    { "天气",   "c-weather", "qyweather",         "qyweather",    0, NULL },
    { "笔记",   "c-note",    "qynote",            "qynote",       0, NULL },
    { "换算",   "c-convert", "qyconvert",         "qyconvert",    0, NULL },
    { "帮助",   "c-help",    "qyhelp",            "qyhelp",       0, NULL },
    { "磁盘",   "c-disk",    "qydisk",            "qydisk",       0, NULL },
    { "待办",   "c-todo",    "qytodo",            "qytodo",       0, NULL },
};
#define NAPPS ((int)(sizeof apps / sizeof apps[0]))

static GtkWidget *taskbar_box   = NULL;
static GtkWidget *dock_vbox     = NULL;
static GtkWidget *active_title  = NULL;
static guint     active_id     = 0;   /* 最近点击的任务栏窗口（本地高亮反馈） */
static long      active_until  = 0;   /* 高亮截止时间戳 */
static GtkWidget *clock_label  = NULL;
static GtkWidget *clock_btn   = NULL;  /* 顶栏时钟按钮（点击弹出日历） */
static gboolean clock_24h = TRUE;     /* 12/24 小时制（release 272） */
static GtkWidget *power_btn   = NULL;  /* 顶栏电源按钮（关机/重启/注销） */
static void on_clock_clicked(GtkWidget *w, gpointer ud);   /* 前向声明 */
static gboolean on_clock_right_press(GtkWidget *w, GdkEventButton *ev, gpointer ud);   /* 前向声明 */
static GtkWidget *mon_label    = NULL;  /* 顶栏 CPU/内存小部件 */
static GtkWidget *mon_draw     = NULL;  /* 顶栏 CPU/内存迷你条 (cairo) */
static double    mon_cpu = 0.0, mon_mem = 0.0;  /* 当前使用率 0~1 */
static GtkWidget *desktop_fixed = NULL;
static GtkWidget *ws_label = NULL;   /* 顶栏工作区指示标签 #qy-ws-ind */

/* ---------- 夜间模式（22:00-6:00 暖色遮罩） ---------- */
static GtkWidget *night_overlay = NULL;   /* 顶层 RGBA 暖色遮罩 */
static gboolean night_visible = FALSE;

/* 读取夜间模式开关: 用户 ~/.config/lanxiu/night-mode.conf (enabled=1) 优先，
 * 其次兼容 /etc/qysettings.conf 的 night_mode=1（设置页开关写这里）。 */
static gboolean night_read_enabled(void) {
    gchar *user = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "night-mode.conf", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(user, &content, NULL, NULL) && content) {
        if (strstr(content, "enabled=1")) {
            g_free(content);
            g_free(user);
            return TRUE;
        }
        g_free(content);
    }
    g_free(user);
    if (g_file_get_contents("/etc/qysettings.conf", &content, NULL, NULL) && content) {
        gboolean on = strstr(content, "night_mode=1") != NULL;
        g_free(content);
        if (on) return TRUE;
    }
    return FALSE;
}

/* 暖色遮罩绘制: rgba(255,150,60,0.08) */
static gboolean night_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)w; (void)ud;
    cairo_set_source_rgba(cr, 1.0, 150.0 / 255.0, 60.0 / 255.0, 0.08);
    cairo_paint(cr);
    return FALSE;
}

/* 检查时间并更新遮罩显隐（22:00-6:00 且配置开启时显示） */
static void night_update(void) {
    if (!night_overlay) return;
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    gboolean night_time = (tm_.tm_hour >= 22 || tm_.tm_hour < 6);
    gboolean enabled = night_read_enabled();
    gboolean show = enabled && night_time;
    if (show != night_visible) {
        night_visible = show;
        gtk_widget_set_visible(night_overlay, show);
        g_printerr("QYDESKTOPDBG: night-mode %s (enabled=%d hour=%d)\n",
                   show ? "on" : "off", enabled, tm_.tm_hour);
    }
}

/* 每 60s 检查一次夜间模式 */
static gboolean night_tick(gpointer ud) {
    (void)ud;
    night_update();
    return G_SOURCE_CONTINUE;
}

/* ---------- 工作区指示器（顶栏右侧，时钟左侧） ---------- */
#define WORKSPACE_MAX  4
#define QY_WS_CONF     "/etc/qyworkspaces.conf"
static int g_workspace = 1;

static void ws_save_conf(void) {
    FILE *f = fopen(QY_WS_CONF, "w");
    if (f) {
        fprintf(f, "count=%d\ncurrent=%d\n", WORKSPACE_MAX, g_workspace);
        fclose(f);
    }
}

static void ws_apply(void) {
    gchar *cmd = g_strdup_printf("weston --workspace=%d 2>/dev/null", g_workspace);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

static void ws_update_label(void) {
    if (!ws_label) return;
    gchar *mk = g_strdup_printf("<span foreground=\"#E95420\">%d</span>/%d",
                                g_workspace, WORKSPACE_MAX);
    gtk_label_set_markup(GTK_LABEL(ws_label), mk);
    g_free(mk);
    g_printerr("QYDESKTOPDBG: workspace=%d\n", g_workspace);
}

static void ws_goto(int n) {
    if (n < 1) n = 1;
    if (n > WORKSPACE_MAX) n = WORKSPACE_MAX;
    if (n == g_workspace) return;
    g_workspace = n;
    ws_save_conf();
    ws_apply();
    ws_update_label();
}

static void ws_prev_clicked(GtkWidget *w, gpointer ud) { (void)w; (void)ud; ws_goto(g_workspace - 1); }
static void ws_next_clicked(GtkWidget *w, gpointer ud) { (void)w; (void)ud; ws_goto(g_workspace + 1); }

/* 工作区指示器滚轮切换（release 277） */
static gboolean on_ws_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT)
        ws_goto(g_workspace - 1);
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT)
        ws_goto(g_workspace + 1);
    return TRUE;
}

/* ---------- 工作区预览（悬停指示器弹出 GtkPopover） ---------- */
/* 从 /etc/qyworkspaces.conf 读取 count=N（默认 4） */
static int ws_count_from_conf(void) {
    int n = WORKSPACE_MAX;
    gchar *content = NULL;
    if (g_file_get_contents(QY_WS_CONF, &content, NULL, NULL) && content) {
        char *line = content;
        while (line && *line) {
            if (strncmp(line, "count=", 6) == 0) {
                int c = atoi(line + 6);
                if (c >= 1 && c <= WORKSPACE_MAX) n = c;
                break;
            }
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
        g_free(content);
    }
    return n;
}

/* 统计任务栏中的窗口数量（占位：所有工作区共用同一数值） */
static int count_open_windows(void) {
    FILE *f = fopen(QY_WINDOWS, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        char *rest = tab + 1;
        char *at = strchr(rest, '@');
        if (at) *at = 0;
        char *nl = strchr(rest, '\n');
        if (nl) *nl = 0;
        if (!rest[0]) continue;
        if (strncmp(rest, "qydesktop", 9) == 0) continue;
        if (strcmp(rest, "qyappmenu") == 0) continue;
        n++;
    }
    fclose(f);
    return n;
}

static GtkWidget *ws_pop = NULL;

static void on_ws_pop_destroy(GtkWidget *w, gpointer ud) {
    (void)ud;
    if (ws_pop == w) ws_pop = NULL;
}

/* 点击预览中的工作区按钮：复用现有切换逻辑 */
static void ws_pop_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    int n = GPOINTER_TO_INT(ud);
    ws_goto(n);
    if (ws_pop) gtk_widget_destroy(ws_pop);
}

/* 悬停工作区指示器：弹出 4 个工作区按钮（当前工作区橙色高亮） */
static gboolean on_ws_enter(GtkWidget *w, GdkEventCrossing *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->mode != GDK_CROSSING_NORMAL) return FALSE;
    if (ws_pop) return FALSE;   /* 已在显示，避免在指示器子部件间移动时反复重建 */
    ws_pop = gtk_popover_new(w);
    gtk_popover_set_position(GTK_POPOVER(ws_pop), GTK_POS_BOTTOM);
    gtk_popover_set_modal(GTK_POPOVER(ws_pop), FALSE);
    g_signal_connect(ws_pop, "destroy", G_CALLBACK(on_ws_pop_destroy), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(v, 10);
    gtk_widget_set_margin_end(v, 10);
    gtk_widget_set_margin_top(v, 8);
    gtk_widget_set_margin_bottom(v, 8);
    gtk_container_add(GTK_CONTAINER(ws_pop), v);

    int nws = ws_count_from_conf();
    int nwin = count_open_windows();
    for (int i = 1; i <= nws; i++) {
        gboolean cur = (i == g_workspace);
        GtkWidget *btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        add_class(btn, "qy-ws-pop-item");
        if (cur) add_class(btn, "qy-ws-pop-current");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_add(GTK_CONTAINER(btn), row);
        gchar *name = g_strdup_printf(TR("工作区 %d"), i);
        GtkWidget *lb = gtk_label_new(name);
        g_free(name);
        gtk_box_pack_start(GTK_BOX(row), lb, FALSE, FALSE, 0);
        gchar *cnt = g_strdup_printf(TR("%d 个窗口"), nwin);
        GtkWidget *clb = gtk_label_new(cnt);
        g_free(cnt);
        add_class(clb, "qy-ws-pop-count");
        gtk_box_pack_end(GTK_BOX(row), clb, FALSE, FALSE, 0);
        g_signal_connect(btn, "clicked", G_CALLBACK(ws_pop_clicked), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(v), btn, FALSE, FALSE, 0);
    }

    /* 快捷键提示（release 265） */
    GtkWidget *hint = gtk_label_new(TR("Ctrl+Alt+←/→ 切换工作区"));
    gtk_widget_set_margin_top(hint, 4);
    add_class(hint, "qy-exp-hint");
    gtk_box_pack_start(GTK_BOX(v), hint, FALSE, FALSE, 0);

    gtk_widget_show_all(ws_pop);
    gtk_popover_popup(GTK_POPOVER(ws_pop));
    return FALSE;
}

/* 启动时读取 /etc/qyworkspaces.conf 的 current=N */
static void ws_load(void) {
    gchar *content = NULL;
    if (g_file_get_contents(QY_WS_CONF, &content, NULL, NULL) && content) {
        char *line = content;
        while (line && *line) {
            if (strncmp(line, "current=", 8) == 0) {
                int n = atoi(line + 8);
                if (n >= 1 && n <= WORKSPACE_MAX) g_workspace = n;
                break;
            }
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
        g_free(content);
    }
}

/* ---------- 工具 ---------- */
static void launch_cmd(const char *cmd) {
    GError *err = NULL;
    gchar **argv = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, &err)) { if (err) g_error_free(err); return; }
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    if (err) { g_printerr("launch: %s\n", err->message); g_error_free(err); }
    g_strfreev(argv);
}

static int proc_running(const char *exe) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], buf[256];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\n")] = 0;
            if (strcmp(buf, exe) == 0) { fclose(f); closedir(d); return atoi(e->d_name); }
        }
        fclose(f);
    }
    closedir(d);
    return 0;
}

/* ---------- 主题 ---------- */
static void load_theme(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    if (gtk_css_provider_load_from_path(p, QY_CSS, NULL)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    g_object_unref(p);
}

static void add_class(GtkWidget *w, const char *cls) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* ---------- 时钟 ---------- */
/* 顶栏迷你资源条: 87x6, 两条圆角矩形 (CPU 左 / 内存 右) */
static gboolean mon_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    int W = 87, H = 6;
    cairo_set_source_rgb(cr, 0.11, 0.14, 0.19);   /* #1c2331 trough */
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.914, 0.329, 0.125);  /* #E95420 */
    double cw = mon_cpu * 38.0;
    if (cw > 0) { cairo_rectangle(cr, 1, 1, cw, H - 2); cairo_fill(cr); }
    double mw = mon_mem * 38.0;
    if (mw > 0) { cairo_rectangle(cr, 46, 1, mw, H - 2); cairo_fill(cr); }
    return FALSE;
}

static gboolean tick(gpointer data) {
    char buf[64];
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    strftime(buf, sizeof buf, clock_24h ? TR("%m月%d日 %H:%M") : TR("%m月%d日 %I:%M %p"), &tm_);
    const char *wd[] = { TR("日"), TR("一"), TR("二"), TR("三"), TR("四"), TR("五"), TR("六") };
    char full[96];
    g_snprintf(full, sizeof full, TR("周%s %s"), wd[tm_.tm_wday], buf);
    gtk_label_set_text(GTK_LABEL(clock_label), full);
    char tip[160];
    strftime(tip, sizeof tip, TR("%Y年%m月%d日 %A %H:%M:%S"), &tm_);
    gtk_widget_set_tooltip_text(clock_label, tip);
    return G_SOURCE_CONTINUE;
}

/* ---------- 顶栏 CPU/内存小部件 ---------- */
static gboolean mon_tick(gpointer data) {
    static long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return G_SOURCE_CONTINUE;
    long u, n, s, idle, iow, irq, sirq, steal;
    if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
               &u, &n, &s, &idle, &iow, &irq, &sirq, &steal) == 8) {
        long total = u + n + s + idle + iow + irq + sirq + steal;
        long dtotal = total - prev_total;
        long didle = (idle + iow) - prev_idle;
        double usage = dtotal > 0 ? 1.0 - (double)didle / (double)dtotal : 0.0;
        prev_total = total;
        prev_idle = idle + iow;
        fclose(f);
        unsigned long avail = 0, total_kb = 0;
        char key[64];
        unsigned long val;
        char unit[16];
        f = fopen("/proc/meminfo", "r");
        if (f) {
            while (fscanf(f, "%63s %lu %15s", key, &val, unit) >= 2) {
                if (!strcmp(key, "MemTotal:")) total_kb = val;
                else if (!strcmp(key, "MemAvailable:")) { avail = val; break; }
            }
            fclose(f);
        }
        double mem = total_kb > 0 ? 1.0 - (double)avail / (double)total_kb : 0.0;
        char buf[80];
        g_snprintf(buf, sizeof buf, "CPU %d%% · MEM %d%%",
                   (int)(usage * 100 + 0.5), (int)(mem * 100 + 0.5));
        gtk_label_set_text(GTK_LABEL(data), "");
        if (mon_draw) gtk_widget_set_tooltip_text(mon_draw, buf);
        mon_cpu = usage;
        mon_mem = mem;
        if (mon_draw) gtk_widget_queue_draw(mon_draw);
    } else {
        fclose(f);
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- 应用菜单 ---------- */
/* 调试输出：同时写入 /tmp/qyappmenu-err.log 与 stderr（串口可读）。 */
static void appmenu_debug(const char *fmt, ...) {
    va_list ap;
    FILE *f = fopen("/tmp/qyappmenu-err.log", "a");
    va_start(ap, fmt);
    if (f) { vfprintf(f, fmt, ap); fclose(f); }
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* 启动应用菜单。修复：开始菜单按钮不要只依赖 clicked（release 事件可能
 * 不被投递到按钮，导致有 active 高亮但 qyappmenu 不启动），改为
 * button-press-event（按下即启动，已验证按下事件可靠到达）+ activate（键盘）。
 */
static void launch_appmenu(void) {
    GError *err = NULL;
    appmenu_debug("QYDESKTOPDBG: on_appmenu launch qyappmenu\n");
    if (!g_spawn_command_line_async("qyappmenu", &err)) {
        if (err) {
            appmenu_debug("QYDESKTOPDBG: qyappmenu spawn FAILED: %s\n", err->message);
            g_printerr("launch: %s\n", err->message);
            g_error_free(err);
        }
        return;
    }
    appmenu_debug("QYDESKTOPDBG: qyappmenu spawn OK\n");
}

/* 兼容键盘 activate 与正常 release 的 clicked 路径 */
static void on_appmenu_clicked(GtkButton *b, gpointer ud) { launch_appmenu(); }

/* 按下即启动（修复 release 不投递的问题）；返回 FALSE 保留 GTK active 高亮 */
static gboolean on_appmenu_pressed(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    if (ev->button == 1)
        launch_appmenu();
    return FALSE;
}

/* 诊断：按钮内 drawing area 是否收到按下事件（排查事件投递路径） */
static gboolean on_appmenu_da_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    appmenu_debug("QYDESKTOPDBG: app_da button-press btn=%u x=%.0f y=%.0f\n", ev->button, ev->x, ev->y);
    return FALSE;
}

/* ---------- 窗口任务栏：读 weston 补丁的窗口列表 ---------- */
typedef struct { guint id; gchar title[96]; } WinInfo;

static int taskbar_parse(WinInfo *wins, int max) {
    FILE *f = fopen(QY_WINDOWS, "r");
    int n = 0;
    if (!f) return 0;
    char line[256];
    while (n < max && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *rest = tab + 1;
        char *at = strchr(rest, '@');
        if (at) *at = 0;
        char *nl = strchr(rest, '\n');
        if (nl) *nl = 0;
        if (!rest[0]) continue;
        /* 过滤壳层自身窗口与启动器 */
        if (strncmp(rest, "qydesktop", 9) == 0) continue;
        if (strcmp(rest, "qyappmenu") == 0) continue;
        /* 重复标题去重：同一应用多实例只显示一个按钮 */
        int dup = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(wins[i].title, rest) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        wins[n].id = (guint)strtoul(line, NULL, 10);
        g_strlcpy(wins[n].title, rest, sizeof wins[n].title);
        n++;
    }
    fclose(f);
    return n;
}

static void on_task_clicked(GtkButton *btn, gpointer ud) {
    guint id = GPOINTER_TO_UINT(ud);
    active_id = id;
    active_until = time(NULL) + 3;   /* 点击后高亮 3 秒 */
    FILE *f = fopen(QY_FOCUS, "w");
    if (f) { fprintf(f, "%u", id); fclose(f); }
}

/* 任务 pill 关闭按钮：写 /tmp/xdg/qy-winop，weston 合成器补丁消费后关闭窗口 */
static void on_task_close_clicked(GtkButton *btn, gpointer ud) {
    guint id = GPOINTER_TO_UINT(ud);
    FILE *f = fopen(QY_WINOP, "w");
    if (f) { fprintf(f, "%u close\n", id); fclose(f); }
}

/* ---------- 任务栏窗口预览（hover Popover） ---------- */
typedef struct {
    gchar title[96];
    GtkWidget *pop;      /* 该任务 pill 关联的预览 popover */
} TaskPreviewData;

static void task_preview_data_free(TaskPreviewData *d) {
    if (d && d->pop) gtk_widget_destroy(d->pop);
    g_slice_free(TaskPreviewData, d);
}

/* 统计任务栏中同名窗口数量（taskbar_parse 去重前的原始计数） */
static int count_windows_for_title(const char *title) {
    FILE *f = fopen(QY_WINDOWS, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        char *rest = tab + 1;
        char *at = strchr(rest, '@');
        if (at) *at = 0;
        char *nl = strchr(rest, '\n');
        if (nl) *nl = 0;
        if (!rest[0]) continue;
        if (strncmp(rest, "qydesktop", 9) == 0) continue;
        if (strcmp(rest, "qyappmenu") == 0) continue;
        if (strcmp(rest, title) == 0) n++;
    }
    fclose(f);
    return n;
}

/* 按窗口标题推断可启动的命令行（用于「打开新窗口」） */
static const char *app_cmdline_for_title(const char *title) {
    for (int i = 0; i < NAPPS; i++)
        if (apps[i].name && strstr(title, apps[i].name))
            return apps[i].cmdline;
    if (strstr(title, "图片") || strstr(title, "图像") || strstr(title, "查看")) return "qyview";
    if (strstr(title, "文本") || strstr(title, "编辑器")) return "qyedit";
    if (strstr(title, "浏览器")) return "qybrowser";
    if (strstr(title, "压缩")) return "qyarc";
    if (strstr(title, "剪贴板")) return "qyclip";
    if (strstr(title, "截图")) return "qyshot";
    if (strstr(title, "网络")) return "qynet";
    if (strstr(title, "驱动")) return "qydriver";
    if (strstr(title, "锁屏")) return "qylock";
    if (strstr(title, "欢迎")) return "qywelcome";
    if (strstr(title, "用户")) return "qyusers";
    return NULL;
}

/* 「切换窗口」：wmctrl 有则用，无则提示 */
static void preview_switch_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    GtkWidget *pop = GTK_WIDGET(ud);
    const char *title = g_object_get_data(G_OBJECT(pop), "qy-preview-title");
    GtkWidget *hint = g_object_get_data(G_OBJECT(pop), "qy-preview-hint");
    if (g_find_program_in_path("wmctrl")) {
        gchar *q = g_shell_quote(title ? title : "");
        gchar *cmd = g_strdup_printf("wmctrl -a %s 2>/dev/null", q);
        g_free(q);
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
        if (hint) gtk_label_set_text(GTK_LABEL(hint), TR("已请求切换窗口"));
    } else if (hint) {
        gtk_label_set_text(GTK_LABEL(hint), TR("未安装 wmctrl，无法切换窗口"));
    }
}

/* 「打开新窗口」：识别应用则启动，否则提示 */
static void preview_new_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    GtkWidget *pop = GTK_WIDGET(ud);
    const char *title = g_object_get_data(G_OBJECT(pop), "qy-preview-title");
    GtkWidget *hint = g_object_get_data(G_OBJECT(pop), "qy-preview-hint");
    const char *cmdline = app_cmdline_for_title(title ? title : "");
    if (cmdline) {
        launch_cmd(cmdline);
        if (hint) gtk_label_set_text(GTK_LABEL(hint), TR("已打开新窗口"));
    } else if (hint) {
        gtk_label_set_text(GTK_LABEL(hint), TR("无法识别应用，不能打开新窗口"));
    }
}

static GtkWidget *build_task_preview_popover(GtkWidget *anchor, const char *title) {
    GtkWidget *pop = gtk_popover_new(anchor);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_popover_set_modal(GTK_POPOVER(pop), FALSE);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(v, 12);
    gtk_widget_set_margin_end(v, 12);
    gtk_widget_set_margin_top(v, 10);
    gtk_widget_set_margin_bottom(v, 10);
    gtk_container_add(GTK_CONTAINER(pop), v);

    GtkWidget *name = gtk_label_new(title);
    add_class(name, "qy-preview-title");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_box_pack_start(GTK_BOX(v), name, FALSE, FALSE, 0);

    gchar *cnt = g_strdup_printf(TR("打开窗口数: %d"), count_windows_for_title(title));
    GtkWidget *count = gtk_label_new(cnt);
    g_free(cnt);
    add_class(count, "qy-preview-count");
    gtk_label_set_xalign(GTK_LABEL(count), 0.0);
    gtk_box_pack_start(GTK_BOX(v), count, FALSE, FALSE, 0);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_sw = gtk_button_new_with_label(TR("切换窗口"));
    add_class(b_sw, "qy-btn");
    GtkWidget *b_new = gtk_button_new_with_label(TR("打开新窗口"));
    add_class(b_new, "qy-btn");
    gtk_box_pack_start(GTK_BOX(hb), b_sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_new, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), hb, FALSE, FALSE, 0);

    GtkWidget *hint = gtk_label_new("");
    add_class(hint, "qy-preview-hint");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(v), hint, FALSE, FALSE, 0);

    g_object_set_data_full(G_OBJECT(pop), "qy-preview-title", g_strdup(title), g_free);
    g_object_set_data(G_OBJECT(pop), "qy-preview-hint", hint);
    g_signal_connect(b_sw, "clicked", G_CALLBACK(preview_switch_clicked), pop);
    g_signal_connect(b_new, "clicked", G_CALLBACK(preview_new_clicked), pop);
    return pop;
}

/* 悬停任务 pill：弹出窗口预览 Popover */
static gboolean on_task_enter(GtkWidget *w, GdkEventCrossing *ev, gpointer ud) {
    (void)ev; (void)ud;
    TaskPreviewData *d = g_object_get_data(G_OBJECT(w), "qy-task-preview");
    if (!d) return FALSE;
    if (!d->pop)
        d->pop = build_task_preview_popover(w, d->title);
    gtk_popover_popup(GTK_POPOVER(d->pop));
    return FALSE;
}

/* 按窗口标题映射统一图标 id（任务 pill 图标） */
static QyIconId task_icon_id(const char *title) {
    /* qy_icon_for_app 统一映射兜底（未识别时回退到本地标题匹配） */
    QyIconId r = qy_icon_for_app(title);
    if (r != QY_ICON_GRID) return r;
    if (strstr(title, "文件") || strstr(title, "主文件夹")) return QY_ICON_FILES;
    if (strstr(title, "终端") || strstr(title, "Terminal")) return QY_ICON_TERM;
    if (strstr(title, "设置"))     return QY_ICON_SETTINGS;
    if (strstr(title, "监视"))     return QY_ICON_MONITOR;
    if (strstr(title, "软件中心")) return QY_ICON_STORE;
    if (strstr(title, "回收站"))   return QY_ICON_TRASH;
    if (strstr(title, "文本") || strstr(title, "编辑器")) return QY_ICON_EDIT;
    if (strstr(title, "图片") || strstr(title, "图像") || strstr(title, "查看")) return QY_ICON_IMAGE;
    if (strstr(title, "压缩"))     return QY_ICON_ARCHIVE;
    if (strstr(title, "浏览器"))   return QY_ICON_BROWSER;
    if (strstr(title, "时钟"))     return QY_ICON_CLOCK;
    if (strstr(title, "天气"))     return QY_ICON_WEATHER;
    if (strstr(title, "笔记"))     return QY_ICON_NOTE;
    if (strstr(title, "音量"))     return QY_ICON_VOLUME;
    if (strstr(title, "电源"))     return QY_ICON_POWER;
    return QY_ICON_FILE;
}

static guint read_focus(void) {
    FILE *f = fopen(QY_FOCUS, "r");
    guint id = 0;
    if (f) {
        if (fscanf(f, "%u", &id) != 1) id = 0;
        fclose(f);
    }
    return id;
}

static void refresh_taskbar(void) {
    if (!taskbar_box) return;

    WinInfo wins[16];
    int n = taskbar_parse(wins, 16);
    guint focus = read_focus();
    long now = time(NULL);
    if (active_title) {
        const char *t = "";
        for (int i = 0; i < n; i++)
            if (wins[i].id == focus) t = wins[i].title;
        gtk_label_set_text(GTK_LABEL(active_title), t);
    }

    /* 签名对比：窗口列表拼接 "id:title" 未变化时直接复用现有 pill，
     * 避免每 500ms 重建导致 hover 关闭按钮丢失。 */
    static char sig[2048] = "";
    char new_sig[2048] = "";
    for (int i = 0; i < n; i++) {
        char item[128];
        g_snprintf(item, sizeof item, "%u:%s\n", wins[i].id, wins[i].title);
        g_strlcat(new_sig, item, sizeof new_sig);
    }
    if (strcmp(sig, new_sig) == 0) {
        /* 不重建：仅就地更新激活高亮 */
        GList *ch = gtk_container_get_children(GTK_CONTAINER(taskbar_box));
        for (GList *it = ch; it; it = it->next) {
            GtkWidget *b = GTK_WIDGET(it->data);
            guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "qy-win-id"));
            gboolean act = (id == active_id && now < active_until) || id == focus;
            if (act)
                add_class(b, "qy-task-pill-active");
            else
                gtk_style_context_remove_class(gtk_widget_get_style_context(b),
                                             "qy-task-pill-active");
        }
        g_list_free(ch);
        return;
    }
    g_strlcpy(sig, new_sig, sizeof sig);

    GList *ch = gtk_container_get_children(GTK_CONTAINER(taskbar_box));
    for (GList *it = ch; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(ch);

    for (int i = 0; i < n; i++) {
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        add_class(b, "qy-task-pill");
        g_object_set_data(G_OBJECT(b), "qy-win-id", GUINT_TO_POINTER(wins[i].id));
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_container_add(GTK_CONTAINER(b), hb);
        GdkPixbuf *pb = qy_icon_pixbuf(task_icon_id(wins[i].title), 22, NULL);
        GtkWidget *img = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
        gtk_box_pack_start(GTK_BOX(hb), img, FALSE, FALSE, 0);
        GtkWidget *tl = gtk_label_new(wins[i].title);
        add_class(tl, "qy-task-label");
        gtk_box_pack_start(GTK_BOX(hb), tl, FALSE, FALSE, 4);

        /* 右侧小关闭按钮：始终可见（release 235 起不再 hover 才显示），
         * 点击写 /tmp/xdg/qy-winop 让 weston 关闭对应窗口。 */
        GtkWidget *close = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
        add_class(close, "qy-task-close");
        GdkPixbuf *cpb = qy_icon_pixbuf(QY_ICON_CLOSE, 12, NULL);
        gtk_container_add(GTK_CONTAINER(close), gtk_image_new_from_pixbuf(cpb));
        g_object_unref(cpb);
        gtk_widget_set_no_show_all(close, FALSE);
        gtk_widget_set_visible(close, TRUE);
        gtk_box_pack_start(GTK_BOX(hb), close, FALSE, FALSE, 2);
        g_signal_connect(close, "clicked", G_CALLBACK(on_task_close_clicked),
                       GUINT_TO_POINTER(wins[i].id));

        gtk_widget_set_tooltip_text(b, wins[i].title);
        if ((wins[i].id == active_id && now < active_until) ||
            wins[i].id == focus)
            add_class(b, "qy-task-pill-active");
        g_signal_connect(b, "clicked", G_CALLBACK(on_task_clicked),
                         GUINT_TO_POINTER(wins[i].id));
        /* hover 窗口预览：应用名 + 窗口数 + 切换/打开新窗口 */
        TaskPreviewData *tp = g_slice_new0(TaskPreviewData);
        g_strlcpy(tp->title, wins[i].title, sizeof tp->title);
        tp->pop = NULL;
        g_object_set_data_full(G_OBJECT(b), "qy-task-preview", tp,
                               (GDestroyNotify)task_preview_data_free);
        g_signal_connect(b, "enter-notify-event", G_CALLBACK(on_task_enter), NULL);
        gtk_box_pack_start(GTK_BOX(taskbar_box), b, FALSE, FALSE, 6);
        gtk_widget_show_all(b);
    }
}

static gboolean taskbar_tick(gpointer ud) { refresh_taskbar(); return G_SOURCE_CONTINUE; }

/* ---------- Dock ---------- */
static void dock_click(GtkButton *btn, gpointer ud) { launch_cmd(((AppEntry *)ud)->cmdline); }

/* Dock 图标：cairo 线稿（与桌面/任务栏同源） */
static gboolean dock_icon_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    AppEntry *a = (AppEntry *)ud;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double size = al.width < al.height ? al.width : al.height;
    double x = (al.width - size) / 2.0;
    double y = (al.height - size) / 2.0;
    qy_icon_draw(cr, task_icon_id(a->name), x, y, size, FALSE, NULL);
    return FALSE;
}

static GtkWidget *dock_icon(AppEntry *a) {
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    add_class(btn, "qy-dock-icon");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(btn), v);
    GtkWidget *ic = gtk_drawing_area_new();
    gtk_widget_set_size_request(ic, 26, 26);
    g_signal_connect(ic, "draw", G_CALLBACK(dock_icon_draw_cb), a);
    gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(v), ic, TRUE, TRUE, 0);
    GtkWidget *dot = gtk_label_new("");
    gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
    add_class(dot, "qy-dock-dot");
    gtk_box_pack_start(GTK_BOX(v), dot, FALSE, FALSE, 0);
    gtk_widget_set_visible(dot, FALSE);
    a->dot = dot;
    gtk_widget_set_tooltip_text(btn, TR(a->name));
    g_object_set_data(G_OBJECT(btn), "app", a);
    g_signal_connect(btn, "clicked", G_CALLBACK(dock_click), a);
    return btn;
}

static gboolean dock_tick(gpointer ud) {
    (void)ud;
    /* 运行指示点 */
    for (int i = 0; i < NAPPS; i++) {
        int pid = proc_running(apps[i].exe);
        apps[i].pid = pid;
        if (apps[i].dot) gtk_widget_set_visible(apps[i].dot, pid > 0);
    }

    /* 激活态动态焦点：读焦点窗口标题，匹配 apps[] 的 name/cmdline */
    int focus_app = -1;
    guint focus = read_focus();
    if (focus) {
        WinInfo wins[16];
        int n = taskbar_parse(wins, 16);
        const char *title = "";
        for (int i = 0; i < n; i++)
            if (wins[i].id == focus) { title = wins[i].title; break; }
        if (title[0]) {
            /* 先按应用名匹配，再按命令行兜底 */
            for (int i = 0; i < NAPPS; i++)
                if (apps[i].name && strstr(title, apps[i].name)) { focus_app = i; break; }
            if (focus_app < 0)
                for (int i = 0; i < NAPPS; i++)
                    if (apps[i].cmdline && strstr(title, apps[i].cmdline)) { focus_app = i; break; }
        }
    }

    /* 先清除所有 dock 图标的激活态，再给匹配焦点的加回 */
    if (dock_vbox) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(dock_vbox));
        for (GList *it = ch; it; it = it->next) {
            GtkWidget *w = GTK_WIDGET(it->data);
            AppEntry *a = (AppEntry *)g_object_get_data(G_OBJECT(w), "app");
            if (!a) continue;
            gtk_style_context_remove_class(gtk_widget_get_style_context(w),
                                           "qy-dock-active");
            if (focus_app >= 0 && a == &apps[focus_app])
                gtk_style_context_add_class(gtk_widget_get_style_context(w),
                                            "qy-dock-active");
        }
        g_list_free(ch);
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- 桌面图标 ---------- */
/* 应用名 → 统一图标 id（qyicon 接入点） */
static gboolean desktop_icon_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    QyIconId id = (QyIconId)GPOINTER_TO_INT(ud);
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double size = al.width < al.height ? al.width : al.height;
    double x = (al.width - size) / 2.0;
    double y = (al.height - size) / 2.0;
    qy_icon_draw(cr, id, x, y, size, FALSE, NULL);
    return FALSE;
}

static void desktop_icon_click(GtkButton *btn, gpointer ud) {
    launch_cmd((const char *)ud);
}

static GtkWidget *desktop_icon(QyIconId iid, const char *css,
                               const char *label, const char *cmdline) {
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    /* release 242: 桌面图标仅供鼠标点击，不参与键盘焦点/默认激活。
     * 若保持可聚焦，桌面窗口获得键盘焦点后 gtk_window_activate_focus 会
     * 自动“点击”默认聚焦的“主文件夹”图标 → qyfiles 随桌面自启动，
     * 窗口堆叠覆盖顶栏 logo 与左侧 Dock，导致开始菜单无法打开。 */
    gtk_widget_set_can_focus(btn, FALSE);
    add_class(btn, "qy-desktop-icon");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(btn), v);

    /* 统一 cairo 线稿图标（44×44 玻璃圆角底） */
    GtkWidget *circle = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(circle), GTK_RELIEF_NONE);
    add_class(circle, "qy-desktop-glyph");
    add_class(circle, css);
    gtk_widget_set_size_request(circle, 44, 44);
    GtkWidget *da = gtk_drawing_area_new();
    g_signal_connect(da, "draw", G_CALLBACK(desktop_icon_draw_cb),
                     GINT_TO_POINTER(iid));
    gtk_container_add(GTK_CONTAINER(circle), da);

    GtkWidget *lb = gtk_label_new(label);
    add_class(lb, "qy-desktop-label");
    gtk_box_pack_start(GTK_BOX(v), circle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), lb, FALSE, FALSE, 0);
    g_signal_connect(btn, "clicked", G_CALLBACK(desktop_icon_click),
                     (gpointer)g_strdup(cmdline));
    return btn;
}

/* ---------- 壁纸（供菜单刷新） ---------- */
static GdkPixbuf *wallpaper = NULL;
static guint wall_seed = 0;

/* 程序化生成品牌壁纸: 深蓝底 + 橙光柔光圆斑 + 白色星点（种子不同图案不同） */
static GdkPixbuf *gen_wallpaper(guint seed) {
    int w = 1600, h = 900;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    /* 基底垂直渐变 */
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgb(pat, 0, 0.045 + (seed % 2) * 0.015, 0.058, 0.13);
    cairo_pattern_add_color_stop_rgb(pat, 1, 0.05 + (seed % 3) * 0.015, 0.09 + (seed % 2) * 0.02, 0.17);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    /* 橙色柔光圆斑 */
    srand(seed);
    for (int i = 0; i < 8; i++) {
        double cx = rand() % w, cy = rand() % h;
        double rad = 60 + rand() % 240;
        cairo_pattern_t *rg = cairo_pattern_create_radial(cx, cy, 10, cx, cy, rad);
        cairo_pattern_add_color_stop_rgba(rg, 0, 0.91, 0.33, 0.13, 0.26);
        cairo_pattern_add_color_stop_rgba(rg, 1, 0.91, 0.33, 0.13, 0);
        cairo_set_source(cr, rg);
        cairo_arc(cr, cx, cy, rad, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(rg);
    }
    /* 星空点缀: 随机白色小星点（不同种子分布不同） */
    for (int i = 0; i < 90; i++) {
        double sx = rand() % w, sy = rand() % h;
        double sr = 0.5 + (rand() % 25) / 20.0;   /* 0.5~1.75px 半径 */
        cairo_set_source_rgba(cr, 1, 1, 1, 0.25 + (rand() % 60) / 100.0);
        cairo_arc(cr, sx, sy, sr, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);
    cairo_surface_destroy(surf);
    return pb;
}

/* 换一张新壁纸（右键刷新 / 定时轮换共用） */
static void rotate_wallpaper(void) {
    if (wallpaper) g_object_unref(wallpaper);
    wall_seed++;
    wallpaper = gen_wallpaper(wall_seed);
    if (desktop_fixed) gtk_widget_queue_draw(desktop_fixed);
}

/* ---------- 壁纸配置加载（用户 ~/.config/lanxiu/wallpaper.conf 优先，兼容 /etc/qywallpaper.conf） ---------- */
static gboolean wall_manual = FALSE;   /* 用户手动设置了壁纸（停止自动轮换） */

/* 按命名渐变绘制壁纸（内置 4 款：深蓝 / 暗橙 / 山脊 / 纯黑），预览与全尺寸共用 */
static void wallpaper_draw_gradient(cairo_t *cr, double w, double h, const char *key) {
    if (!key) key = "darkblue";
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, 0, h);
    if (strcmp(key, "darkorange") == 0) {
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.17, 0.07, 0.02);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.72, 0.31, 0.07);
    } else if (strcmp(key, "ridge") == 0) {
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.03, 0.05, 0.13);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.22, 0.13, 0.06);
    } else if (strcmp(key, "black") == 0) {
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.03, 0.03, 0.03);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.0, 0.0, 0.0);
    } else {  /* darkblue: #0B1020 -> #1B2A4A */
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.043, 0.063, 0.125);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.106, 0.165, 0.290);
    }
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);

    if (strcmp(key, "ridge") == 0) {
        /* 山脊：两层深色三角形山脊剪影 */
        cairo_set_source_rgba(cr, 0.02, 0.03, 0.08, 0.85);
        cairo_move_to(cr, 0, h);
        cairo_line_to(cr, w * 0.22, h * 0.42);
        cairo_line_to(cr, w * 0.45, h * 0.78);
        cairo_line_to(cr, w * 0.70, h * 0.38);
        cairo_line_to(cr, w, h * 0.62);
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.01, 0.02, 0.05, 0.95);
        cairo_move_to(cr, 0, h);
        cairo_line_to(cr, w * 0.12, h * 0.66);
        cairo_line_to(cr, w * 0.34, h * 0.90);
        cairo_line_to(cr, w * 0.58, h * 0.58);
        cairo_line_to(cr, w * 0.85, h * 0.82);
        cairo_line_to(cr, w, h * 0.72);
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

/* 生成指定命名的渐变壁纸（1600x900） */
static GdkPixbuf *gen_gradient_wallpaper(const char *key) {
    int w = 1600, h = 900;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    wallpaper_draw_gradient(cr, w, h, key);
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);
    cairo_surface_destroy(surf);
    return pb;
}

/* 读取壁纸配置（用户 conf 优先，其次 /etc/qywallpaper.conf）。
 * 成功按配置设置 wallpaper 时返回 TRUE（并标记手动壁纸，停止自动轮换）。 */
static gboolean wallpaper_load_config(void) {
    const char *home = g_get_home_dir();
    gchar *user = g_build_filename(home && home[0] ? home : "/root",
                                   ".config/lanxiu/wallpaper.conf", NULL);
    const char *paths[] = { user, "/etc/qywallpaper.conf" };
    gboolean loaded = FALSE;

    for (guint pi = 0; pi < G_N_ELEMENTS(paths) && !loaded; pi++) {
        gchar *conf = NULL;
        if (!g_file_get_contents(paths[pi], &conf, NULL, NULL) || !conf) {
            g_free(conf);
            continue;
        }
        char *line = conf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            /* path=（新配置）与 wallpaper=（旧 /etc 配置）均为图片路径 */
            if ((strncmp(line, "path=", 5) == 0 && line[5]) ||
                (strncmp(line, "wallpaper=", 10) == 0 && line[10])) {
                const char *p = line[5] && strncmp(line, "path=", 5) == 0 ? line + 5 : line + 10;
                GdkPixbuf *pb = gdk_pixbuf_new_from_file(p, NULL);
                if (pb) {
                    if (wallpaper) g_object_unref(wallpaper);
                    wallpaper = pb;
                    loaded = TRUE;
                    wall_manual = TRUE;
                }
            } else if (strncmp(line, "gradient=", 9) == 0 && line[9]) {
                GdkPixbuf *pb = gen_gradient_wallpaper(line + 9);
                if (pb) {
                    if (wallpaper) g_object_unref(wallpaper);
                    wallpaper = pb;
                    loaded = TRUE;
                    wall_manual = TRUE;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        g_free(conf);
    }
    g_free(user);
    return loaded;
}

/* 重新加载壁纸并立即刷新桌面（设置对话框应用后调用） */
static void wallpaper_reload(void) {
    gboolean loaded = wallpaper_load_config();
    if (!loaded) {
        /* 无配置：回退默认壁纸（文件存在则用，否则程序化生成） */
        if (wallpaper) g_object_unref(wallpaper);
        wallpaper = gdk_pixbuf_new_from_file(QY_WALL, NULL);
        if (!wallpaper) wallpaper = gen_wallpaper(0);
    }
    if (desktop_fixed) gtk_widget_queue_draw(desktop_fixed);
    g_printerr("QYDESKTOPDBG: wallpaper reloaded (loaded=%d)\n", loaded);
}

/* ---------- 桌面右键菜单 ---------- */
static void menu_new_folder(GtkMenuItem *mi, gpointer ud) {
    const char *home = g_get_home_dir();
    if (!home || !home[0]) home = "/root";
    gchar *path = g_strdup_printf("%s/新建文件夹", home);
    int i = 1;
    while (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = g_strdup_printf("%s/新建文件夹%d", home, i++);
    }
    g_mkdir_with_parents(path, 0755);
    g_free(path);
}

static void menu_open_terminal(GtkMenuItem *mi, gpointer ud) { launch_cmd("weston-terminal"); }
static void menu_open_files(GtkMenuItem *mi, gpointer ud) { launch_cmd("qyfiles"); }
static void menu_open_settings(GtkMenuItem *mi, gpointer ud) { launch_cmd("qysettings"); }
static void menu_open_monitor(GtkMenuItem *mi, gpointer ud) { launch_cmd("qymon"); }

/* 顶栏 CPU/内存迷你条点击打开系统监视（release 273） */
static gboolean mon_press_cb(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ev; (void)ud;
    launch_cmd("qymon");
    return TRUE;
}
static void menu_open_driver(GtkMenuItem *mi, gpointer ud) { launch_cmd("qydriver"); }
static void menu_open_trash(GtkMenuItem *mi, gpointer ud) { launch_cmd("qyfiles --trash"); }
static void menu_refresh_wallpaper(GtkMenuItem *mi, gpointer ud) {
    rotate_wallpaper();
}

/* ---------- 壁纸设置对话框（#qy-wallpaper-win） ---------- */
typedef struct {
    const char *key;
    const char *label;
} WallPreset;

static const WallPreset wall_presets[] = {
    { "darkblue",   "深蓝渐变" },
    { "darkorange", "暗橙渐变" },
    { "ridge",      "山脊风格" },
    { "black",      "纯黑" },
};
#define NWALL_PRESETS ((int)(sizeof wall_presets / sizeof wall_presets[0]))

static const char *wall_dlg_preset = "darkblue";  /* 当前选中的内置壁纸 key */
static gchar *wall_dlg_custom = NULL;             /* 自定义图片路径（NULL=未选） */
static GtkWidget *wall_dlg_custom_lbl = NULL;
static GtkWidget *wall_dlg_preset_box = NULL;     /* 内置壁纸按钮容器 */

/* 绘制内置壁纸预览缩略图（GtkDrawingArea + cairo 渐变） */
static gboolean wall_preset_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    const char *key = (const char *)ud;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double ww = al.width, hh = al.height;
    double r = 8;
    cairo_new_path(cr);
    cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
    cairo_arc(cr, ww - r, r, r, 3 * G_PI / 2, 2 * G_PI);
    cairo_arc(cr, ww - r, hh - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, hh - r, r, G_PI / 2, G_PI);
    cairo_close_path(cr);
    cairo_clip(cr);
    wallpaper_draw_gradient(cr, ww, hh, key);
    cairo_restore(cr);
    /* 边框 */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.3);
    cairo_new_path(cr);
    cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
    cairo_arc(cr, ww - r, r, r, 3 * G_PI / 2, 2 * G_PI);
    cairo_arc(cr, ww - r, hh - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, hh - r, r, G_PI / 2, G_PI);
    cairo_close_path(cr);
    cairo_stroke(cr);
    return FALSE;
}

/* 高亮当前选中的内置壁纸按钮（仅保留一个 .qy-wall-selected） */
static void wall_preset_highlight(const char *key) {
    if (!wall_dlg_preset_box) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(wall_dlg_preset_box));
    for (GList *it = ch; it; it = it->next) {
        const char *k = g_object_get_data(G_OBJECT(it->data), "wall-key");
        if (k && key && strcmp(k, key) == 0)
            add_class(GTK_WIDGET(it->data), "qy-wall-selected");
        else
            gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(it->data)),
                                           "qy-wall-selected");
    }
    g_list_free(ch);
}

static void on_wall_preset_clicked(GtkButton *b, gpointer ud) {
    (void)ud;
    const char *key = g_object_get_data(G_OBJECT(b), "wall-key");
    if (!key) return;
    wall_dlg_preset = key;
    g_free(wall_dlg_custom);
    wall_dlg_custom = NULL;
    if (wall_dlg_custom_lbl)
        gtk_label_set_text(GTK_LABEL(wall_dlg_custom_lbl), TR("未选择自定义图片"));
    wall_preset_highlight(key);
}

static GtkWidget *wall_preset_button(const WallPreset *p) {
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    add_class(btn, "qy-wall-preset");
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(btn), v);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 120, 68);
    g_signal_connect(da, "draw", G_CALLBACK(wall_preset_draw_cb), (gpointer)p->key);
    gtk_box_pack_start(GTK_BOX(v), da, FALSE, FALSE, 0);
    GtkWidget *lb = gtk_label_new(p->label);
    add_class(lb, "qy-wall-preset-label");
    gtk_box_pack_start(GTK_BOX(v), lb, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(btn), "wall-key", (gpointer)p->key);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_wall_preset_clicked), NULL);
    return btn;
}

/* 选择自定义图片（png/jpg，GtkFileChooserNative） */
static void on_wall_custom_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkWidget *dlg = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkFileChooserNative *fc = gtk_file_chooser_native_new(
        TR("选择壁纸图片"), GTK_WINDOW(dlg), GTK_FILE_CHOOSER_ACTION_OPEN,
        TR("选择"), TR("取消"));
    GtkFileFilter *f_img = gtk_file_filter_new();
    gtk_file_filter_set_name(f_img, TR("图片文件"));
    gtk_file_filter_add_pattern(f_img, "*.png");
    gtk_file_filter_add_pattern(f_img, "*.jpg");
    gtk_file_filter_add_pattern(f_img, "*.jpeg");
    gtk_file_filter_add_pattern(f_img, "*.webp");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f_img);
    GtkFileFilter *f_all = gtk_file_filter_new();
    gtk_file_filter_set_name(f_all, TR("所有文件"));
    gtk_file_filter_add_pattern(f_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f_all);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (path) {
            g_free(wall_dlg_custom);
            wall_dlg_custom = path;
            if (wall_dlg_custom_lbl)
                gtk_label_set_text(GTK_LABEL(wall_dlg_custom_lbl), path);
        }
    }
    g_object_unref(fc);
}

/* 写入 ~/.config/lanxiu/wallpaper.conf 并立即刷新桌面壁纸 */
static void wallpaper_apply(void) {
    const char *home = g_get_home_dir();
    gchar *dir = g_build_filename(home && home[0] ? home : "/root", ".config/lanxiu", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *conf = g_build_filename(dir, "wallpaper.conf", NULL);
    if (wall_dlg_custom && wall_dlg_custom[0]) {
        gchar *content = g_strdup_printf("path=%s\n", wall_dlg_custom);
        g_file_set_contents(conf, content, -1, NULL);
        g_free(content);
    } else {
        gchar *content = g_strdup_printf("gradient=%s\n",
                                         wall_dlg_preset ? wall_dlg_preset : "darkblue");
        g_file_set_contents(conf, content, -1, NULL);
        g_free(content);
    }
    g_free(conf);
    g_free(dir);
    wallpaper_reload();
}

/* 打开对话框时回显当前配置（用户 conf 优先） */
static void wall_dialog_load_current(void) {
    const char *home = g_get_home_dir();
    gchar *user = g_build_filename(home && home[0] ? home : "/root",
                                   ".config/lanxiu/wallpaper.conf", NULL);
    const char *paths[] = { user, "/etc/qywallpaper.conf" };
    gchar *path_val = NULL;
    gchar *grad_val = NULL;
    for (guint pi = 0; pi < G_N_ELEMENTS(paths); pi++) {
        gchar *conf = NULL;
        if (!g_file_get_contents(paths[pi], &conf, NULL, NULL) || !conf) {
            g_free(conf);
            continue;
        }
        char *line = conf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (strncmp(line, "path=", 5) == 0 && line[5] && !path_val)
                path_val = g_strdup(line + 5);
            else if (strncmp(line, "wallpaper=", 10) == 0 && line[10] && !path_val)
                path_val = g_strdup(line + 10);
            else if (strncmp(line, "gradient=", 9) == 0 && line[9] && !grad_val)
                grad_val = g_strdup(line + 9);
            line = nl ? nl + 1 : NULL;
        }
        g_free(conf);
        if (path_val || grad_val) break;   /* 用户配置优先，读到即停 */
    }
    g_free(user);
    g_free(wall_dlg_custom);
    wall_dlg_custom = NULL;
    if (path_val && g_file_test(path_val, G_FILE_TEST_EXISTS)) {
        wall_dlg_custom = g_strdup(path_val);
        if (wall_dlg_custom_lbl)
            gtk_label_set_text(GTK_LABEL(wall_dlg_custom_lbl), path_val);
        wall_preset_highlight(NULL);   /* 自定义图片时不高亮任何内置壁纸 */
    } else {
        const char *known = "darkblue";
        if (grad_val) {
            for (int i = 0; i < NWALL_PRESETS; i++) {
                if (strcmp(grad_val, wall_presets[i].key) == 0) { known = wall_presets[i].key; break; }
            }
        }
        wall_dlg_preset = known;
        if (wall_dlg_custom_lbl)
            gtk_label_set_text(GTK_LABEL(wall_dlg_custom_lbl), TR("未选择自定义图片"));
        wall_preset_highlight(wall_dlg_preset);
    }
    g_free(path_val);
    g_free(grad_val);
}

static void on_wall_dialog_response(GtkDialog *dlg, gint response, gpointer ud) {
    (void)ud;
    if (response == GTK_RESPONSE_ACCEPT) {
        wallpaper_apply();
        gtk_widget_destroy(GTK_WIDGET(dlg));
    } else if (response == GTK_RESPONSE_CANCEL || response == GTK_RESPONSE_DELETE_EVENT) {
        gtk_widget_destroy(GTK_WIDGET(dlg));
    }
}

/* 弹出壁纸设置对话框 */
static void wallpaper_dialog_show(void) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        TR("更换壁纸"), NULL, GTK_DIALOG_MODAL,
        TR("关闭"), GTK_RESPONSE_CANCEL,
        TR("应用"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dlg, "qy-wallpaper-win");
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    g_signal_connect(dlg, "response", G_CALLBACK(on_wall_dialog_response), NULL);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(box), vbox);

    GtkWidget *lbl = gtk_label_new(TR("内置壁纸"));
    add_class(lbl, "qy-wall-dlg-title");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    wall_dlg_preset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), wall_dlg_preset_box, FALSE, FALSE, 0);
    for (int i = 0; i < NWALL_PRESETS; i++)
        gtk_box_pack_start(GTK_BOX(wall_dlg_preset_box),
                           wall_preset_button(&wall_presets[i]), FALSE, FALSE, 0);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *b_custom = gtk_button_new_with_label(TR("自定义图片…"));
    add_class(b_custom, "qy-btn");
    g_signal_connect(b_custom, "clicked", G_CALLBACK(on_wall_custom_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hb), b_custom, FALSE, FALSE, 0);
    wall_dlg_custom_lbl = gtk_label_new(TR("未选择自定义图片"));
    add_class(wall_dlg_custom_lbl, "qy-wall-custom-lbl");
    gtk_label_set_ellipsize(GTK_LABEL(wall_dlg_custom_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(hb), wall_dlg_custom_lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    wall_dialog_load_current();
}

/* 桌面右键菜单「更换壁纸」入口 */
static void menu_wallpaper_dialog(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    wallpaper_dialog_show();
}

/* 自动轮换壁纸 (默认 600s, 可用 QY_WALL_INTERVAL 秒覆盖) */
static gboolean auto_wall_tick(gpointer ud) {
    /* 用户已手动设置壁纸（对话框 / 配置文件）时停止自动轮换 */
    if (wall_manual) return G_SOURCE_CONTINUE;
    rotate_wallpaper();
    return G_SOURCE_CONTINUE;
}

static gboolean auto_close_about(gpointer p) {
    gtk_widget_destroy(GTK_WIDGET(p));
    return G_SOURCE_REMOVE;
}

static void menu_about(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT,
                                            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "%s", TR("澜岫 Linux 桌面"));
    /* 系统信息: 发行版 / 内核 / 内存 */
    gchar *os = NULL;
    g_file_get_contents("/etc/os-release", &os, NULL, NULL);
    gchar *distro = g_strdup(TR("未知发行版"));
    if (os) {
        const char *p = strstr(os, "PRETTY_NAME=");
        if (p) {
            p += 12;
            const char *e = strchr(p, '\n');
            gchar *name = g_strstrip(g_strndup(p, e ? (gsize)(e - p) : strlen(p)));
            if (name[0] == '"' && name[strlen(name) - 1] == '"') {
                name[strlen(name) - 1] = 0;
                g_free(distro);
                distro = g_strdup(name + 1);
            } else {
                g_free(distro);
                distro = g_strdup(name);
            }
            g_free(name);
        }
    }
    g_free(os);
    gchar *kver = NULL;
    g_spawn_command_line_sync("uname -r", &kver, NULL, NULL, NULL);
    gchar *mem = NULL;
    g_file_get_contents("/proc/meminfo", &mem, NULL, NULL);
    int mem_mb = -1;
    if (mem) {
        const char *p = strstr(mem, "MemTotal:");
        if (p) mem_mb = atoi(p + 9) / 1024;
    }
    g_free(mem);
    gchar *info = g_strdup_printf("%s\n%s: %s\n%s: %s\n%s: %d MB",
        TR("GTK3 单窗口桌面壳层 · weston + 自研任务栏补丁"),
        TR("发行版"), distro,
        TR("内核"), kver ? g_strstrip(kver) : TR("未知"),
        TR("内存"), mem_mb);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", info);
    g_free(distro); g_free(kver); g_free(info);
    g_printerr("QYDESKTOPDBG: about shown\n");
    if (g_getenv("QYDESKTOP_ABOUT")) {
        gtk_widget_show_all(dlg);
        g_timeout_add(4000, auto_close_about, dlg);
    } else {
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
}

static gboolean desk_button_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    if (ev->button == 3) {
        GtkWidget *menu = gtk_menu_new();
        add_class(menu, "qy-menu");
        GtkWidget *mi;
        mi = gtk_menu_item_new_with_label(TR("新建文件夹"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_new_folder), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("打开终端"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_terminal), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        mi = gtk_menu_item_new_with_label(TR("文件管理器"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_files), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("系统设置"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_settings), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("系统监视"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_monitor), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("驱动管理器"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_driver), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("回收站"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_open_trash), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        mi = gtk_menu_item_new_with_label(TR("刷新壁纸"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_refresh_wallpaper), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("更换壁纸"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_wallpaper_dialog), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("关于澜岫"));
        add_class(mi, "qy-menu-item");
        g_signal_connect(mi, "activate", G_CALLBACK(menu_about), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
        g_printerr("QYDESKTOPDBG: desktop right-click menu\n");
    }
    return FALSE;
}

/* 自动化：QYDESKTOP_RIGHTCLICK=1 启动后模拟桌面右键弹出菜单 */
static gboolean auto_desktop_rightclick(gpointer p) {
    (void)p;
    GdkEventButton ev;
    memset(&ev, 0, sizeof ev);
    ev.type = GDK_BUTTON_PRESS;
    ev.button = 3;
    ev.x = 220; ev.y = 220;
    ev.x_root = 220; ev.y_root = 220;
    ev.time = gtk_get_current_event_time();
    desk_button_press(NULL, &ev, NULL);
    return G_SOURCE_REMOVE;
}

/* ---------- 壁纸绘制 ---------- */
static gboolean desk_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    guint width = gtk_widget_get_allocated_width(w);
    guint height = gtk_widget_get_allocated_height(w);
    if (wallpaper) {
        int pw = gdk_pixbuf_get_width(wallpaper), ph = gdk_pixbuf_get_height(wallpaper);
        cairo_save(cr);
        cairo_scale(cr, (double)width / pw, (double)height / ph);
        gdk_cairo_set_source_pixbuf(cr, wallpaper, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, 0, height);
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.04, 0.06, 0.12);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.06, 0.09, 0.16);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
    }
    return FALSE;
}

/* ---------- 顶栏 ---------- */
static GtkWidget *notif_label = NULL;
static GtkWidget *notif_badge = NULL;   /* 未读徽标（圆点/数字），初始隐藏 */
static gchar *last_notif_content = NULL;
static GtkWidget *res_label = NULL;

/* 未读计数 = /tmp/qynotif/history.log 行数（qynotifd 保留最近约 8KB） */
static int notif_history_count(void) {
    gchar *hist = NULL;
    int n = 0;
    if (g_file_get_contents("/tmp/qynotif/history.log", &hist, NULL, NULL) && hist) {
        char *p = hist;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            if (p[0]) n++;
            p = nl ? nl + 1 : NULL;
        }
        g_free(hist);
    }
    return n;
}

/* 显示未读徽标：历史 >9 条显示数字徽标，否则显示 6px 橙色圆点 */
static void notif_badge_show_unread(void) {
    if (!notif_badge) return;
    int n = notif_history_count();
    GtkStyleContext *ctx = gtk_widget_get_style_context(notif_badge);
    gtk_style_context_remove_class(ctx, "qy-notif-dot");
    gtk_style_context_remove_class(ctx, "qy-notif-badge");
    if (n > 9) {
        gtk_style_context_add_class(ctx, "qy-notif-badge");
        gchar *s = g_strdup_printf("%d", n);
        gtk_label_set_text(GTK_LABEL(notif_badge), s);
        g_free(s);
    } else {
        gtk_style_context_add_class(ctx, "qy-notif-dot");
        gtk_label_set_text(GTK_LABEL(notif_badge), "");
    }
    gtk_widget_show(notif_badge);
    gchar *tip = g_strdup_printf(TR("%d 条未读通知"), n);
    gtk_widget_set_tooltip_text(notif_badge, tip);
    g_free(tip);
}

/* 点击铃铛查看通知后隐藏未读徽标（已读） */
static void notif_badge_hide(void) {
    if (notif_badge) gtk_widget_hide(notif_badge);
}

/* 分辨率快捷切换：改写 weston.ini 的 mode=（start-weston.sh 消费） */
static void apply_resolution(const char *mode) {
    if (!mode || !mode[0]) return;
    const char *path = "/etc/xdg/weston/weston.ini";
    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        content = g_strdup("[core]\nshell=desktop-shell.so\n");
    }
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "mode=", 5) == 0) {
            g_string_append_printf(out, "mode=%s", mode);
            if (nl) g_string_append_c(out, '\n');
            replaced = 1;
        } else {
            g_string_append_len(out, line, len);
            if (nl) g_string_append_c(out, '\n');
        }
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "\n[output]\nname=Virtual-1\nmode=%s\n", mode);
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    g_printerr("QYDESKTOPDBG: resolution=%s\n", mode);
    /* 顶栏按钮同步显示 */
    if (res_label) gtk_label_set_text(GTK_LABEL(res_label), mode);
    /* 通知用户（qydesktop 顶栏气泡读取） */
    gchar *body = g_strdup_printf(TR("分辨率已设置为 %s，重启桌面后生效"), mode);
    gchar *full = g_strdup_printf("%s|%s\n", TR("分辨率"), body);
    g_mkdir_with_parents("/tmp/qynotif", 0755);
    g_file_set_contents("/tmp/qynotif/latest.msg", full, -1, NULL);
    g_free(body); g_free(full);
}

/* 自动化: QYDESKTOP_RES=1024x768 启动后自动设置分辨率 */
static gboolean auto_res_apply(gpointer p) {
    apply_resolution((const char *)p);
    return G_SOURCE_REMOVE;
}

/* ---------- 通知中心快捷开关面板 ---------- */
enum { QUICK_WIFI, QUICK_BT, QUICK_DND, QUICK_POWER };

/* 读取键值 conf 的 on/off 状态（如 wifi=on） */
static gboolean conf_read_onoff(const char *path, const char *key) {
    gchar *c = NULL;
    gboolean on = FALSE;
    if (g_file_get_contents(path, &c, NULL, NULL) && c) {
        char *line = c;
        size_t keylen = strlen(key);
        while (line && *line) {
            if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
                on = (g_ascii_strncasecmp(line + keylen + 1, "on", 2) == 0);
                break;
            }
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
        g_free(c);
    }
    return on;
}

/* 读取键值 conf 的数字状态（如 dnd=1 / power_saver=1） */
static gboolean conf_read_num(const char *path, const char *key) {
    gchar *c = NULL;
    gboolean on = FALSE;
    if (g_file_get_contents(path, &c, NULL, NULL) && c) {
        char *line = c;
        size_t keylen = strlen(key);
        while (line && *line) {
            if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
                on = (line[keylen + 1] == '1');
                break;
            }
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
        g_free(c);
    }
    return on;
}

/* 写键值 conf（on/off 格式，保留其它行） */
static void conf_write_onoff(const char *path, const char *key, int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(path, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    size_t keylen = strlen(key);
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            g_string_append_printf(out, "%s=%s", key, on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, on ? "on" : "off");
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 写键值 conf（数字 1/0 格式，保留其它行） */
static void conf_write_num(const char *path, const char *key, int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(path, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    size_t keylen = strlen(key);
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            g_string_append_printf(out, "%s=%d", key, on ? 1 : 0);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%d\n", key, on ? 1 : 0);
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 切换快捷开关背景色（CSS .qy-quick-toggle-on） */
static void quick_toggle_apply_style(GtkWidget *btn, gboolean on) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
    if (on)
        gtk_style_context_add_class(ctx, "qy-quick-toggle-on");
    else
        gtk_style_context_remove_class(ctx, "qy-quick-toggle-on");
}

/* 快捷开关点击：写对应 conf + 切换外观 + 反馈提示 */
static void on_quick_toggle(GtkToggleButton *btn, gpointer ud) {
    int which = GPOINTER_TO_INT(ud);
    gboolean on = gtk_toggle_button_get_active(btn);
    const char *path = NULL, *key = NULL;
    gboolean numeric = FALSE;
    const char *msg = NULL;
    GtkWidget *hint = g_object_get_data(G_OBJECT(btn), "hint-label");

    switch (which) {
    case QUICK_WIFI:
        path = "/etc/qywifi.conf"; key = "wifi";
        msg = on ? TR("WiFi 已开启") : TR("WiFi 已关闭");
        break;
    case QUICK_BT:
        path = "/etc/qybt.conf"; key = "bluetooth";
        msg = on ? TR("蓝牙已开启") : TR("蓝牙已关闭");
        break;
    case QUICK_DND:
        path = "/etc/qynotifd.conf"; key = "dnd"; numeric = TRUE;
        msg = on ? TR("免打扰已开启") : TR("免打扰已关闭");
        break;
    case QUICK_POWER:
        path = "/etc/qypower.conf"; key = "power_saver"; numeric = TRUE;
        msg = on ? TR("省电模式已开启") : TR("省电模式已关闭");
        break;
    default:
        return;
    }

    if (numeric) conf_write_num(path, key, on);
    else         conf_write_onoff(path, key, on);

    quick_toggle_apply_style(GTK_WIDGET(btn), on);
    if (hint) gtk_label_set_text(GTK_LABEL(hint), msg);

    if (which == QUICK_DND) {
        /* 通知 qynotifd 重启以应用免打扰（写 conf 已触发其 /etc 监视，重启兜底） */
        g_spawn_command_line_async(
            "sh -c 'pkill -x qynotifd 2>/dev/null; sleep 0.1; qynotifd 2>/dev/null &'", NULL);
    } else if (which == QUICK_WIFI && !proc_running("qynotifd") && hint) {
        /* 无后端（qynotifd 未运行）时提示仅保存配置 */
        gtk_label_set_text(GTK_LABEL(hint), TR("WiFi 后端未运行（qynotifd），仅保存配置"));
    }
}

/* 构造一个快捷开关：图标 + 文字，带状态背景色 */
static GtkWidget *quick_toggle_new(const char *label, QyIconId icon, int which,
                                   gboolean active, GtkWidget *hint) {
    GtkWidget *btn = gtk_toggle_button_new();
    add_class(btn, "qy-quick-toggle");
    if (active) add_class(btn, "qy-quick-toggle-on");
    gtk_widget_set_size_request(btn, -1, 54);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_add(GTK_CONTAINER(btn), v);
    GdkPixbuf *pb = qy_icon_pixbuf(icon, 20, NULL);
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    if (pb) g_object_unref(pb);
    gtk_box_pack_start(GTK_BOX(v), img, FALSE, FALSE, 0);
    GtkWidget *lb = gtk_label_new(label);
    add_class(lb, "qy-quick-toggle-label");
    gtk_box_pack_start(GTK_BOX(v), lb, FALSE, FALSE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), active);
    g_object_set_data(G_OBJECT(btn), "hint-label", hint);
    g_signal_connect(btn, "toggled", G_CALLBACK(on_quick_toggle), GINT_TO_POINTER(which));
    return btn;
}

static GtkWidget *notif_pop = NULL;   /* 当前打开的通知中心 popover */

static void on_notif_pop_destroy(GtkWidget *w, gpointer ud) {
    (void)ud;
    if (notif_pop == w) notif_pop = NULL;
}

/* 通知历史全文：持久化文件 ~/.local/share/lanxiu/notifications.log + 现有内存历史 /tmp/qynotif/history.log */
static gchar *notif_history_text(void) {
    GString *out = g_string_new(NULL);
    gchar *dir = g_build_filename(g_get_user_data_dir(), "lanxiu", NULL);
    gchar *path = g_build_filename(dir, "notifications.log", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content && content[0])
        g_string_append(out, content);
    g_free(content);
    g_free(path);
    g_free(dir);
    gchar *hist = NULL;
    if (g_file_get_contents("/tmp/qynotif/history.log", &hist, NULL, NULL) && hist && hist[0])
        g_string_append(out, hist);
    g_free(hist);
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return g_strdup(TR("暂无通知"));
    }
    return g_string_free(out, FALSE);
}

/* 「全部已读」：隐藏未读徽标，历史保留（release 267） */
static void on_notif_read_all(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    notif_badge_hide();
}

/* 「清空历史」：删除持久化通知文件并清空内存历史 */
static void on_notif_clear_history(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    gchar *dir = g_build_filename(g_get_user_data_dir(), "lanxiu", NULL);
    gchar *path = g_build_filename(dir, "notifications.log", NULL);
    g_unlink(path);
    g_free(path);
    g_free(dir);
    g_mkdir_with_parents("/tmp/qynotif", 0755);
    g_file_set_contents("/tmp/qynotif/history.log", "", 0, NULL);
    notif_badge_hide();
    /* 若通知中心打开，立即刷新历史文本 */
    if (notif_pop) {
        GtkWidget *tv = g_object_get_data(G_OBJECT(notif_pop), "hist-tv");
        if (tv) {
            gchar *txt = notif_history_text();
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), txt, -1);
            g_free(txt);
        }
    }
}

/* 点击通知图标（左键）：弹出通知中心（快捷开关行 + 历史列表） */
static gboolean on_notif_clicked(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ev; (void)ud;
    if (ev && ev->button != 1) return FALSE;   /* 右键由 on_notif_right_press 处理 */
    if (notif_pop) gtk_widget_destroy(notif_pop);

    GtkWidget *pop = gtk_popover_new(w);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_popover_set_modal(GTK_POPOVER(pop), FALSE);
    g_signal_connect(pop, "destroy", G_CALLBACK(on_notif_pop_destroy), NULL);
    notif_pop = pop;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_container_add(GTK_CONTAINER(pop), vbox);

    GtkWidget *title = gtk_label_new(TR("通知中心"));
    add_class(title, "qy-notif-panel-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

    /* 快捷开关行（GtkGrid 4 列） */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    gboolean wifi_on = conf_read_onoff("/etc/qywifi.conf", "wifi");
    gboolean bt_on   = conf_read_onoff("/etc/qybt.conf", "bluetooth");
    gboolean dnd_on  = conf_read_num("/etc/qynotifd.conf", "dnd");
    gboolean pwr_on  = conf_read_num("/etc/qypower.conf", "power_saver");

    /* 提示标签（开关反馈），创建于 grid 之后，快捷开关引用它 */
    GtkWidget *hint = gtk_label_new("");
    add_class(hint, "qy-preview-hint");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), hint, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid),
        quick_toggle_new(TR("WiFi"), QY_ICON_NETWORK, QUICK_WIFI, wifi_on, hint), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        quick_toggle_new(TR("蓝牙"), QY_ICON_DRIVER, QUICK_BT, bt_on, hint), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        quick_toggle_new(TR("勿扰"), QY_ICON_LOCK, QUICK_DND, dnd_on, hint), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        quick_toggle_new(TR("省电"), QY_ICON_POWER, QUICK_POWER, pwr_on, hint), 3, 0, 1, 1);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    GtkWidget *hist_hdr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hist_hdr = gtk_label_new(TR("通知历史"));
    add_class(hist_hdr, "qy-notif-panel-title");
    gtk_label_set_xalign(GTK_LABEL(hist_hdr), 0.0);
    gtk_widget_set_hexpand(hist_hdr, TRUE);
    gtk_box_pack_start(GTK_BOX(hist_hdr_box), hist_hdr, TRUE, TRUE, 0);
    GtkWidget *readall_btn = gtk_button_new_with_label(TR("全部已读"));
    add_class(readall_btn, "qy-btn");
    g_signal_connect(readall_btn, "clicked", G_CALLBACK(on_notif_read_all), NULL);
    gtk_box_pack_start(GTK_BOX(hist_hdr_box), readall_btn, FALSE, FALSE, 0);
    GtkWidget *clear_btn = gtk_button_new_with_label(TR("清空历史"));
    add_class(clear_btn, "qy-btn");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_notif_clear_history), NULL);
    gtk_box_pack_start(GTK_BOX(hist_hdr_box), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hist_hdr_box, FALSE, FALSE, 0);

    gchar *hist = notif_history_text();
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw, 320, 180);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_name(tv, "qy-notif-history");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), hist, -1);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);
    g_object_set_data(G_OBJECT(pop), "hist-tv", tv);
    g_free(hist);

    gtk_widget_show_all(pop);
    gtk_popover_popup(GTK_POPOVER(pop));
    notif_badge_hide();   /* 已读：点击查看后隐藏未读徽标 */
    return TRUE;
}

/* 通知显示：读取 /tmp/qynotif/latest.msg（qynotify/qynotifd 写入） */
static gboolean notif_tick(gpointer ud) {
    (void)ud;
    gchar *content = NULL;
    if (g_file_get_contents("/tmp/qynotif/latest.msg", &content, NULL, NULL) && content) {
        if (!last_notif_content || strcmp(last_notif_content, content) != 0) {
            char *bar = strchr(content, '|');
            if (bar) {
                *bar = 0;
                gchar *title = g_strdup_printf("%s", content);
                gtk_label_set_text(GTK_LABEL(notif_label), title);
                gtk_widget_set_tooltip_text(notif_label, bar + 1);
                g_free(title);
            }
            g_free(last_notif_content);
            last_notif_content = g_strdup(content);
            notif_badge_show_unread();   /* 新通知：显示未读徽标 */
        }
        g_free(content);
    } else {
        gtk_label_set_text(GTK_LABEL(notif_label), "");
    }
    return G_SOURCE_CONTINUE;
}

static void tray_launch(const char *cmd) {
    gchar *s = g_strdup_printf("%s &", cmd);
    g_spawn_command_line_async(s, NULL);
    g_free(s);
}
static void on_tray_clicked(GtkWidget *w, gpointer ud) {
    tray_launch((const char *)ud);
}
/* ---------- 托盘统一 qyicon 线稿图标（替代 emoji，去 AI 感） ---------- */
enum { TRAY_NET, TRAY_DRV, TRAY_VOL, TRAY_CLIP, TRAY_SHOT, TRAY_LOCK, TRAY_PWR, TRAY_BAT };

static QyIconId tray_icon_id(int t) {
    switch (t) {
    case TRAY_NET:  return QY_ICON_NETWORK;
    case TRAY_DRV:  return QY_ICON_DRIVER;
    case TRAY_VOL:  return QY_ICON_VOLUME;
    case TRAY_CLIP: return QY_ICON_CLIPBOARD;
    case TRAY_SHOT: return QY_ICON_SHOT;
    case TRAY_LOCK: return QY_ICON_LOCK;
    case TRAY_PWR:  return QY_ICON_POWER;
    case TRAY_BAT:  return QY_ICON_POWER;
    default:        return QY_ICON_SETTINGS;
    }
}

static gboolean tray_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    QyIconId id = tray_icon_id(GPOINTER_TO_INT(ud));
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    gboolean pre = (gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_PRELIGHT) != 0;
    GdkRGBA col;
    gdk_rgba_parse(&col, pre ? "#ffffff" : "#c9cdd4");
    qy_icon_draw(cr, id, (al.width - 18) / 2.0, (al.height - 18) / 2.0, 18, FALSE, &col);
    return FALSE;
}

/* 通知铃铛 cairo 线稿（替代 🔔 emoji） */
static gboolean bell_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    double cx = gtk_widget_get_allocated_width(w) / 2.0;
    double cy = gtk_widget_get_allocated_height(w) / 2.0;
    cairo_set_source_rgb(cr, 0.79, 0.80, 0.83);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - 5, cy - 1);
    cairo_curve_to(cr, cx - 5, cy - 5.5, cx - 3, cy - 7.5, cx, cy - 7.5);
    cairo_curve_to(cr, cx + 3, cy - 7.5, cx + 5, cy - 5.5, cx + 5, cy - 1);
    cairo_line_to(cr, cx + 6.2, cy + 2);
    cairo_line_to(cr, cx - 6.2, cy + 2);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy + 5, 1.4, 0, 2 * G_PI); cairo_stroke(cr);
    cairo_move_to(cr, cx - 1.2, cy - 7.8); cairo_line_to(cr, cx + 1.2, cy - 7.8); cairo_stroke(cr);
    return FALSE;
}

/* 托盘图标 hover 高亮：由 tray_draw_cb 读取 GTK_STATE_FLAG_PRELIGHT 状态实现 */
static GtkWidget *make_tray_icon_btn_cb(int icon, const char *tip, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    add_class(b, "qy-status-btn");
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 28, 28);
    g_signal_connect(da, "draw", G_CALLBACK(tray_draw_cb), GINT_TO_POINTER(icon));
    gtk_container_add(GTK_CONTAINER(b), da);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *make_tray_icon_btn(int icon, const char *tip, const char *cmd) {
    return make_tray_icon_btn_cb(icon, tip, G_CALLBACK(on_tray_clicked), g_strdup(cmd));
}

/* ---------- 电源菜单：关机 / 重启 / 注销 ---------- */
static void on_power_action(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    const char *cmd = (const char *)ud;
    g_printerr("QYDESKTOPDBG: power action %s\n", cmd);
    g_spawn_command_line_async(cmd, NULL);
}

static void on_power_btn_clicked(GtkWidget *w, gpointer ud) {
    (void)ud;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi;
    mi = gtk_menu_item_new_with_label(TR("关机"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_power_action), g_strdup("poweroff"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("重启"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_power_action), g_strdup("reboot"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("注销"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_power_action), g_strdup("pkill -x weston"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_widget(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
    g_printerr("QYDESKTOPDBG: power menu shown\n");
}

/* 自动化: QYDESKTOP_POWER=1 启动后弹出电源菜单 */
static gboolean auto_power_menu(gpointer p) {
    (void)p;
    on_power_btn_clicked(power_btn, NULL);
    return G_SOURCE_REMOVE;
}

/* ---------- 音量快捷滑块（amixer Master） ---------- */
static void on_vol_scale_changed(GtkRange *range, gpointer ud) {
    (void)ud;
    int v = (int)gtk_range_get_value(range);
    gchar *cmd = g_strdup_printf("amixer -q sset Master %d%% 2>/dev/null", v);
    g_spawn_command_line_async(cmd, NULL);
    g_printerr("QYDESKTOPDBG: volume=%d\n", v);
    g_free(cmd);
}

static void on_vol_btn_clicked(GtkWidget *w, gpointer ud) {
    (void)ud;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_range_set_value(GTK_RANGE(scale), 70);
    gtk_widget_set_size_request(scale, 160, -1);
    gtk_container_add(GTK_CONTAINER(item), scale);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_vol_scale_changed), NULL);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_widget(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* 自动化: QYDESKTOP_VOL=70 启动后设置音量 */
static gboolean auto_set_volume(gpointer p) {
    int v = atoi((const char *)p);
    gchar *cmd = g_strdup_printf("amixer -q sset Master %d%% 2>/dev/null", v);
    g_spawn_command_line_async(cmd, NULL);
    g_printerr("QYDESKTOPDBG: volume=%d\n", v);
    g_free(cmd);
    return G_SOURCE_REMOVE;
}

/* ---------- 托盘图标右键菜单 ---------- */
/* 简单提示对话框（amixer 缺失等场景） */
static void show_info_dialog(const char *title, const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* 菜单项通用启动器 */
static void tray_menu_launch(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    launch_cmd((const char *)ud);
}

/* 检测是否存在电池（/sys/class/power_supply/BAT*） */
static gboolean has_battery(void) {
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d) return FALSE;
    const char *n;
    gboolean found = FALSE;
    while ((n = g_dir_read_name(d))) {
        if (g_str_has_prefix(n, "BAT") || g_str_has_prefix(n, "bat")) {
            found = TRUE;
            break;
        }
    }
    g_dir_close(d);
    return found;
}

/* 音量右键：打开 qyaudio / 静音切换（amixer set Master toggle，无 amixer 则提示） */
static void on_vol_open_audio(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    launch_cmd("qyaudio");
}

static void on_vol_mute(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (!g_find_program_in_path("amixer")) {
        show_info_dialog(TR("静音"), TR("未找到 amixer，无法调节音量"));
        return;
    }
    g_spawn_command_line_async("amixer -q set Master toggle 2>/dev/null", NULL);
    g_printerr("QYDESKTOPDBG: amixer Master toggle\n");
}

static gboolean on_vol_right_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != 3) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("打开音频管理器"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_vol_open_audio), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("静音切换"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_vol_mute), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* 网络右键：打开 qynet */
static gboolean on_net_right_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != 3) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("打开网络管理器"));
    g_signal_connect(mi, "activate", G_CALLBACK(tray_menu_launch), (gpointer)"qynet");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* 通知铃铛右键：勿扰开关 / 清空历史 */
static void on_notif_dnd_toggle(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    gboolean on = conf_read_num("/etc/qynotifd.conf", "dnd");
    conf_write_num("/etc/qynotifd.conf", "dnd", !on);
    g_printerr("QYDESKTOPDBG: dnd -> %d\n", !on);
    /* 重启 qynotifd 应用免打扰（写 conf 已触发其 /etc 监视，重启兜底） */
    g_spawn_command_line_async(
        "sh -c 'pkill -x qynotifd 2>/dev/null; sleep 0.1; qynotifd 2>/dev/null &'", NULL);
}

static void on_notif_clear_menu(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    on_notif_clear_history(NULL, NULL);
}

static gboolean on_notif_right_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != 3) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("免打扰开关"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_notif_dnd_toggle), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("清空历史"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_notif_clear_menu), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("全部已读"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_notif_read_all), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* 电池右键：打开 qypower（左键同样打开 qypower） */
static void on_bat_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    launch_cmd("qypower");
}

static gboolean on_bat_right_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != 3) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("打开电源管理"));
    g_signal_connect(mi, "activate", G_CALLBACK(tray_menu_launch), (gpointer)"qypower");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* 品牌 logo：玻璃 tile + 澜岫星线稿（cairo，禁字符） */
static gboolean app_logo_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double size = al.width < al.height ? al.width : al.height;
    double x = (al.width - size) / 2.0;
    double y = (al.height - size) / 2.0;
    qy_icon_tile(cr, QY_ICON_WELCOME, x, y, size, 8, NULL, NULL);
    return FALSE;
}

static void build_bar(void) {
    GtkWidget *bar = gtk_event_box_new();
    add_class(bar, "qy-bar");
    gtk_widget_set_size_request(bar, SCREEN_W, BAR_H);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(bar), hbox);

    /* 左区（固定 240px）：品牌 logo 玻璃 tile + 当前应用标题 */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(left, 240, -1);
    add_class(left, "qy-bar-left");
    GtkWidget *app_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(app_btn), GTK_RELIEF_NONE);
    add_class(app_btn, "qy-logo-btn");
    gtk_widget_set_size_request(app_btn, 32, 32);
    GtkWidget *app_da = gtk_drawing_area_new();
    g_signal_connect(app_da, "draw", G_CALLBACK(app_logo_draw_cb), NULL);
    g_signal_connect(app_da, "button-press-event", G_CALLBACK(on_appmenu_da_press), NULL);
    gtk_container_add(GTK_CONTAINER(app_btn), app_da);
    g_signal_connect(app_btn, "activate", G_CALLBACK(on_appmenu_clicked), NULL);
    g_signal_connect(app_btn, "button-press-event", G_CALLBACK(on_appmenu_pressed), NULL);
    gtk_widget_set_tooltip_text(app_btn, TR("显示应用"));
    gtk_box_pack_start(GTK_BOX(left), app_btn, FALSE, FALSE, 0);
    active_title = gtk_label_new(NULL);
    add_class(active_title, "qy-active-title");
    gtk_box_pack_start(GTK_BOX(left), active_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), left, FALSE, FALSE, 8);

    /* 窗口任务栏（弹性占中靠左，pill 按钮） */
    taskbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hbox), taskbar_box, TRUE, TRUE, 0);

    /* 右区（状态）：工作区指示器 + 时钟 + 资源监视 + 通知 + 托盘 */
    GtkWidget *st = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    /* 工作区指示器：‹ 1/4 ›（顶栏右侧、时钟左侧） */
    ws_load();
    GtkWidget *ws_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *ws_prev = gtk_button_new_with_label("‹");
    add_class(ws_prev, "qy-ws-btn");
    gtk_widget_set_tooltip_text(ws_prev, TR("上一个工作区"));
    g_signal_connect(ws_prev, "clicked", G_CALLBACK(ws_prev_clicked), NULL);
    ws_label = gtk_label_new(NULL);
    gtk_widget_set_name(ws_label, "qy-ws-ind");
    add_class(ws_label, "qy-ws-ind");
    GtkWidget *ws_next = gtk_button_new_with_label("›");
    add_class(ws_next, "qy-ws-btn");
    gtk_widget_set_tooltip_text(ws_next, TR("下一个工作区"));
    g_signal_connect(ws_next, "clicked", G_CALLBACK(ws_next_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(ws_box), ws_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ws_box), ws_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ws_box), ws_next, FALSE, FALSE, 0);
    /* 工作区指示器整体包进事件盒，悬停弹出 4 工作区预览 */
    GtkWidget *ws_eb = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(ws_eb), ws_box);
    g_signal_connect(ws_eb, "enter-notify-event", G_CALLBACK(on_ws_enter), NULL);
    g_signal_connect(ws_eb, "scroll-event", G_CALLBACK(on_ws_scroll), NULL);
    gtk_box_pack_start(GTK_BOX(st), ws_eb, FALSE, FALSE, 2);
    ws_update_label();

    clock_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(clock_btn), GTK_RELIEF_NONE);
    add_class(clock_btn, "qy-clock");
    clock_label = gtk_label_new("");
    add_class(clock_label, "qy-clock-label");
    gtk_container_add(GTK_CONTAINER(clock_btn), clock_label);
    g_signal_connect(clock_btn, "clicked", G_CALLBACK(on_clock_clicked), NULL);
    g_signal_connect(clock_btn, "button-press-event", G_CALLBACK(on_clock_right_press), NULL);
    /* 读取已保存的 12/24 小时制（release 272） */
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        gchar *cp = g_build_filename(cdir, "qydesktop-clock24", NULL);
        clock_24h = g_file_test(cp, G_FILE_TEST_EXISTS);
        g_free(cp); g_free(cdir);
    }
    gtk_box_pack_start(GTK_BOX(st), clock_btn, FALSE, FALSE, 0);
    tick(clock_label);
    g_timeout_add_seconds(1, tick, clock_label);

    GtkWidget *mon_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(mon_hb, 87, 8);
    mon_draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(mon_draw, 87, 6);
    gtk_widget_set_halign(mon_draw, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(mon_draw, GTK_ALIGN_CENTER);
    g_signal_connect(mon_draw, "draw", G_CALLBACK(mon_draw_cb), NULL);
    gtk_widget_add_events(mon_draw, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(mon_draw, "button-press-event", G_CALLBACK(mon_press_cb), NULL);
    gtk_widget_set_tooltip_text(mon_draw, TR("系统资源（点击打开系统监视）"));
    gtk_box_pack_start(GTK_BOX(mon_hb), mon_draw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), mon_hb, FALSE, FALSE, 6);
    mon_label = gtk_label_new("");
    add_class(mon_label, "qy-mon-widget");
    gtk_box_pack_start(GTK_BOX(st), mon_label, FALSE, FALSE, 6);
    g_timeout_add_seconds(2, mon_tick, mon_label);

    /* 通知显示（可点击查看历史） */
    GtkWidget *notif_eb = gtk_event_box_new();
    add_class(notif_eb, "qy-status-btn");
    gtk_widget_add_events(notif_eb, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(notif_eb, "button-press-event", G_CALLBACK(on_notif_clicked), NULL);
    g_signal_connect(notif_eb, "button-press-event", G_CALLBACK(on_notif_right_press), NULL);
    gtk_widget_set_tooltip_text(notif_eb, TR("查看通知（右键菜单）"));
    /* 铃铛 cairo 线稿 + 通知标题 label（有通知时显示标题）
     * 未读徽标：GtkOverlay 叠加在铃铛图标右上角（外缘 2px） */
    GtkWidget *notif_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *bell_overlay = gtk_overlay_new();
    /* 铃铛外包 20×20 对齐容器作为 overlay 主 child，徽标可跨到角落 */
    GtkWidget *bell_align = gtk_alignment_new(0.5, 0.5, 0.0, 0.0);
    gtk_widget_set_size_request(bell_align, 20, 20);
    GtkWidget *bell_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(bell_da, 16, 16);
    g_signal_connect(bell_da, "draw", G_CALLBACK(bell_draw_cb), NULL);
    gtk_container_add(GTK_CONTAINER(bell_align), bell_da);
    gtk_container_add(GTK_CONTAINER(bell_overlay), bell_align);
    notif_badge = gtk_label_new(NULL);
    gtk_widget_set_halign(notif_badge, GTK_ALIGN_END);
    gtk_widget_set_valign(notif_badge, GTK_ALIGN_START);
    gtk_widget_set_margin_end(notif_badge, 0);
    gtk_widget_set_margin_top(notif_badge, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(bell_overlay), notif_badge);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(bell_overlay), notif_badge, TRUE);
    gtk_widget_set_no_show_all(notif_badge, TRUE);  /* 初始隐藏，出现未读再显示 */
    gtk_widget_hide(notif_badge);
    gtk_container_add(GTK_CONTAINER(notif_box), bell_overlay);
    notif_label = gtk_label_new(NULL);
    gtk_container_add(GTK_CONTAINER(notif_box), notif_label);
    gtk_container_add(GTK_CONTAINER(notif_eb), notif_box);
    gtk_box_pack_start(GTK_BOX(st), notif_eb, FALSE, FALSE, 0);

    /* 系统托盘: 网络/声音/剪贴板/截图/锁屏（cairo 线稿图标，右键菜单） */
    GtkWidget *tray_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *net_btn = make_tray_icon_btn(TRAY_NET, TR("网络"), "qynet");
    gtk_widget_set_tooltip_text(net_btn, TR("网络控制（打开 qynet 查看连接）"));
    g_signal_connect(net_btn, "button-press-event", G_CALLBACK(on_net_right_press), NULL);
    gtk_box_pack_start(GTK_BOX(tray_hb), net_btn, FALSE, FALSE, 0);
    GtkWidget *drv_btn = make_tray_icon_btn(TRAY_DRV, TR("驱动"), "qydriver");
    gtk_widget_set_tooltip_text(drv_btn, TR("驱动管理（打开 qydriver）"));
    gtk_box_pack_start(GTK_BOX(tray_hb), drv_btn, FALSE, FALSE, 0);
    GtkWidget *vol_btn = make_tray_icon_btn_cb(TRAY_VOL, TR("声音"), G_CALLBACK(on_vol_btn_clicked), NULL);
    gtk_widget_set_tooltip_text(vol_btn, TR("音量控制（点击打开滑块，右键打开 qyaudio/静音）"));
    g_signal_connect(vol_btn, "button-press-event", G_CALLBACK(on_vol_right_press), NULL);
    gtk_box_pack_start(GTK_BOX(tray_hb), vol_btn, FALSE, FALSE, 0);
    GtkWidget *clip_btn = make_tray_icon_btn(TRAY_CLIP, TR("剪贴板"), "qyclip");
    gtk_widget_set_tooltip_text(clip_btn, TR("剪贴板（打开 qyclip）"));
    gtk_box_pack_start(GTK_BOX(tray_hb), clip_btn, FALSE, FALSE, 0);
    GtkWidget *shot_btn = make_tray_icon_btn(TRAY_SHOT, TR("截图"), "qyshot");
    gtk_widget_set_tooltip_text(shot_btn, TR("截图（打开 qyshot）"));
    gtk_box_pack_start(GTK_BOX(tray_hb), shot_btn, FALSE, FALSE, 0);
    GtkWidget *lock_btn = make_tray_icon_btn(TRAY_LOCK, TR("锁屏"), "qylock");
    gtk_widget_set_tooltip_text(lock_btn, TR("锁屏（打开 qylock）"));
    gtk_box_pack_start(GTK_BOX(tray_hb), lock_btn, FALSE, FALSE, 0);
    /* 电池（系统存在电池时显示） */
    if (has_battery()) {
        GtkWidget *bat_btn = make_tray_icon_btn_cb(TRAY_BAT, TR("电池"), G_CALLBACK(on_bat_clicked), NULL);
        gtk_widget_set_tooltip_text(bat_btn, TR("电池状态（点击查看）"));
        /* 电池百分比 tooltip（release 268） */
        gchar *bat_cap = NULL;
        if (g_file_get_contents("/sys/class/power_supply/BAT0/capacity", &bat_cap, NULL, NULL)) {
            gchar *tip = g_strdup_printf(TR("电池 %s%%"), g_strstrip(bat_cap));
            gtk_widget_set_tooltip_text(bat_btn, tip);
            g_free(tip);
            g_free(bat_cap);
        }
        g_signal_connect(bat_btn, "button-press-event", G_CALLBACK(on_bat_right_press), NULL);
        gtk_box_pack_start(GTK_BOX(tray_hb), bat_btn, FALSE, FALSE, 0);
    }
    power_btn = make_tray_icon_btn_cb(TRAY_PWR, TR("电源"), G_CALLBACK(on_power_btn_clicked), NULL);
    gtk_widget_set_tooltip_text(power_btn, TR("电源控制（打开 qypower）"));
    gtk_box_pack_start(GTK_BOX(tray_hb), power_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(st), tray_hb, FALSE, FALSE, 4);
    gtk_widget_set_margin_end(st, 8);
    gtk_box_pack_end(GTK_BOX(hbox), st, FALSE, FALSE, 0);

    gtk_fixed_put(GTK_FIXED(desktop_fixed), bar, 0, 0);
    gtk_widget_show_all(bar);
    g_timeout_add_seconds(1, notif_tick, NULL);
}

/* ---------- 左侧 Dock ---------- */
static void build_dock(void) {
    GtkWidget *dock = gtk_event_box_new();
    add_class(dock, "qy-dock");
    gtk_widget_set_size_request(dock, DOCK_W, SCREEN_H - BAR_H - 8);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(dock), vbox);
    dock_vbox = vbox;

    for (int i = 0; i < NAPPS; i++)
        gtk_box_pack_start(GTK_BOX(vbox), dock_icon(&apps[i]), FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 4);

    /* 底部应用网格：qyicon GRID 图标 */
    GtkWidget *grid_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(grid_btn), GTK_RELIEF_NONE);
    add_class(grid_btn, "qy-dock-icon");
    add_class(grid_btn, "c-grid");
    GtkWidget *gd = gtk_drawing_area_new();
    gtk_widget_set_size_request(gd, 26, 26);
    gtk_widget_set_halign(gd, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(gd, GTK_ALIGN_CENTER);
    g_signal_connect(gd, "draw", G_CALLBACK(desktop_icon_draw_cb), GINT_TO_POINTER(QY_ICON_GRID));
    gtk_container_add(GTK_CONTAINER(grid_btn), gd);
    gtk_widget_set_tooltip_text(grid_btn, TR("显示应用"));
    g_signal_connect(grid_btn, "activate", G_CALLBACK(on_appmenu_clicked), NULL);
    g_signal_connect(grid_btn, "button-press-event", G_CALLBACK(on_appmenu_pressed), NULL);
    gtk_box_pack_end(GTK_BOX(vbox), grid_btn, FALSE, FALSE, 0);

    gtk_fixed_put(GTK_FIXED(desktop_fixed), dock, 0, BAR_H);
    gtk_widget_show_all(dock);
}

/* ---------- 桌面窗口 ---------- */
static void build_desktop(void) {
    const char *seed_env = getenv("QY_WALL_SEED");   /* 演示/测试: 指定生成种子 */
    /* 优先读取壁纸配置（用户 ~/.config/lanxiu/wallpaper.conf，兼容 /etc/qywallpaper.conf） */
    if (!wallpaper_load_config()) {
        if (seed_env && seed_env[0]) {
            wallpaper = gen_wallpaper((guint)strtoul(seed_env, NULL, 10));
        } else {
            wallpaper = gdk_pixbuf_new_from_file(QY_WALL, NULL);
            if (!wallpaper) wallpaper = gen_wallpaper(0);   /* 文件缺失回退到程序化生成 */
        }
    }
    const char *iv_env = getenv("QY_WALL_INTERVAL");   /* 自动轮换秒数(默认600) */
    int wall_interval = (iv_env && atoi(iv_env) > 0) ? atoi(iv_env) : 600;
    g_timeout_add_seconds(wall_interval, auto_wall_tick, NULL);
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "qydesktop");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DESKTOP);
    gtk_window_set_default_size(GTK_WINDOW(win), SCREEN_W, SCREEN_H);
    gtk_window_move(GTK_WINDOW(win), 0, 0);
    gtk_window_set_keep_below(GTK_WINDOW(win), TRUE);

    desktop_fixed = gtk_fixed_new();
    /* 顶层用 GtkOverlay 承载壁纸/桌面与夜间模式暖色遮罩 */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(win), overlay);
    gtk_container_add(GTK_CONTAINER(overlay), desktop_fixed);

    night_overlay = gtk_drawing_area_new();
    gtk_widget_set_halign(night_overlay, GTK_ALIGN_FILL);
    gtk_widget_set_valign(night_overlay, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(night_overlay, SCREEN_W, SCREEN_H);
    g_signal_connect(night_overlay, "draw", G_CALLBACK(night_draw), NULL);
    gtk_widget_set_no_show_all(night_overlay, TRUE);
    gtk_widget_hide(night_overlay);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), night_overlay);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), night_overlay, TRUE);

    GtkWidget *darea = gtk_drawing_area_new();
    gtk_widget_set_size_request(darea, SCREEN_W, SCREEN_H);
    gtk_widget_add_events(darea, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(darea, "draw", G_CALLBACK(desk_draw), NULL);
    g_signal_connect(darea, "button-press-event", G_CALLBACK(desk_button_press), NULL);
    gtk_fixed_put(GTK_FIXED(desktop_fixed), darea, 0, 0);

    /* 桌面图标（左上竖排） */
    int x = 110, y = 60, dy = 110;
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_HOME, "c-home", TR("主文件夹"), "qyfiles"), x, y);
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_TRASH, "c-trash", TR("回收站"), "qyfiles --trash"), x, y + dy);
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_STORE, "c-store", TR("软件中心"), "qystore"), x, y + dy * 2);
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_TERM, "c-term", TR("终端"), "weston-terminal"), x, y + dy * 3);
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_SETTINGS, "c-settings", TR("设置"), "qysettings"), x, y + dy * 4);
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_MONITOR, "c-mon", TR("系统监视"), "qymon"), x, y + dy * 5);
    gtk_fixed_put(GTK_FIXED(desktop_fixed),
                  desktop_icon(QY_ICON_IMAGE, "c-view", TR("图片查看"), "qyview"), x, y + dy * 6);

    gtk_widget_show_all(win);
    /* weston 输入焦点修复（release 238）：桌面窗口显示后主动 present 获取键盘焦点，
     * 确保 VNC/sendkey 的键盘输入（含 Escape 关闭菜单）能到达桌面壳层。
     * release 242：不再调用 gtk_window_activate_focus —— 它会激活默认聚焦的
     * 桌面图标（“主文件夹”），导致 qyfiles 随桌面自启动覆盖顶栏/左侧 Dock。 */
    gtk_window_present(GTK_WINDOW(win));

    /* 夜间模式: 启动时立即检查，之后每 60s 检查时间窗口 */
    night_update();
    g_timeout_add_seconds(60, night_tick, NULL);
}

/* ---------- 入口 ---------- */
/* 时钟 12/24 小时制（release 272）：写 ~/.config/lanxiu/qydesktop-clock24 */
static void clock_set_24h(gboolean on) {
    clock_24h = on;
    gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(cdir, 0755);
    gchar *cp = g_build_filename(cdir, "qydesktop-clock24", NULL);
    if (on) g_file_set_contents(cp, "1", -1, NULL);
    else g_remove(cp);
    g_free(cp); g_free(cdir);
    tick(clock_label);
}

static void on_clock_12_toggle(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    clock_set_24h(!clock_24h);
}

static gboolean on_clock_right_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != 3) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(clock_24h ? TR("切换到 12 小时制") : TR("切换到 24 小时制"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_clock_12_toggle), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static void on_clock_clicked(GtkWidget *w, gpointer ud) {
    (void)ud;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *cal = gtk_calendar_new();
    gtk_container_add(GTK_CONTAINER(item), cal);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_widget(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
    g_printerr("QYDESKTOPDBG: calendar shown\n");
}

static gboolean auto_calendar(gpointer p) {
    (void)p;
    on_clock_clicked(clock_btn, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean auto_about(gpointer p);

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_DFL);
    gtk_init(&argc, &argv);
    load_theme();
    build_desktop();
    build_bar();
    build_dock();
    /* 首启向导 */
    if (access("/etc/.qywelcomed", F_OK) != 0) launch_cmd("qywelcome");
    g_timeout_add_seconds(1, taskbar_tick, NULL);
    g_timeout_add_seconds(2, dock_tick, NULL);
    /* 自动化: QYDESKTOP_RES=1024x768 启动后自动设置分辨率 */
    const char *res_env = g_getenv("QYDESKTOP_RES");
    if (res_env && res_env[0])
        g_timeout_add(600, auto_res_apply, g_strdup(res_env));
    /* 自动化: QYDESKTOP_RIGHTCLICK=1 启动后模拟桌面右键 */
    if (g_getenv("QYDESKTOP_RIGHTCLICK"))
        g_timeout_add(1000, auto_desktop_rightclick, NULL);
    /* 自动化: QYDESKTOP_VOL=70 启动后设置音量 */
    const char *vol_env = g_getenv("QYDESKTOP_VOL");
    if (vol_env && vol_env[0])
        g_timeout_add(700, auto_set_volume, (gpointer)vol_env);
    /* 自动化: QYDESKTOP_ABOUT=1 启动后弹出关于窗口 */
    if (g_getenv("QYDESKTOP_ABOUT"))
        g_timeout_add(1200, (GSourceFunc)auto_about, NULL);
    /* 自动化: QYDESKTOP_CALENDAR=1 启动后弹出日历 */
    if (g_getenv("QYDESKTOP_CALENDAR"))
        g_timeout_add(1000, (GSourceFunc)auto_calendar, NULL);
    /* 自动化: QYDESKTOP_POWER=1 启动后弹出电源菜单 */
    if (g_getenv("QYDESKTOP_POWER"))
        g_timeout_add(1100, (GSourceFunc)auto_power_menu, NULL);
    gtk_main();
    return 0;
}

static gboolean auto_about(gpointer p) {
    (void)p;
    menu_about(NULL, NULL);
    return G_SOURCE_REMOVE;
}