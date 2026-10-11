/* qyclock.c — 澜岫时钟/日历
 * 独立小窗：大时钟 + 日期星期 + 月历 + 秒表（开始/暂停/复位）+ 今天高亮。
 * 风格沿用 qylock 的大时钟与深色主题（#qy-clock-win 圆角卡片）。
 * 自动化: QYCLOCK_SW=start 启动后自动开始秒表（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <time.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GtkWidget *clock_label   = NULL;   /* #qyclock-time 大时钟 */
static GtkWidget *date_label    = NULL;   /* #qyclock-date 日期星期 */
static GtkWidget *sw_label     = NULL;   /* #qyclock-sw 秒表读数 */
static GtkWidget *sw_btn       = NULL;   /* 秒表 开始/暂停/继续 */
static GtkWidget *cal          = NULL;   /* GtkCalendar */
static int today_y = 0, today_m = 0, today_d = 0;

/* ---------- 秒表状态 ---------- */
static gboolean sw_running  = FALSE;
static gint64   sw_start_us = 0;      /* 本次开始时刻（单调时钟） */
static gint64   sw_accum_us = 0;     /* 已累计运行时长 */

/* 更新大时钟与日期（250ms 调用，秒级闪烁） */
static gboolean update_clock(gpointer p) {
    (void)p;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    g_snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (clock_label) gtk_label_set_text(GTK_LABEL(clock_label), buf);

    static const char *wk[] = { "日", "一", "二", "三", "四", "五", "六" };
    g_snprintf(buf, sizeof buf, "%d年%02d月%02d日 星期%s",
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, wk[tm->tm_wday]);
    if (date_label) gtk_label_set_text(GTK_LABEL(date_label), buf);
    return G_SOURCE_CONTINUE;
}

/* 秒表显示：分钟:秒.十分之一秒 */
static void sw_update_display(void) {
    if (!sw_label) return;
    gint64 total = sw_accum_us;
    if (sw_running)
        total += g_get_monotonic_time() - sw_start_us;
    long ms = total / 1000;
    char buf[32];
    g_snprintf(buf, sizeof buf, "%ld:%02ld.%ld", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10);
    gtk_label_set_text(GTK_LABEL(sw_label), buf);
}

/* 100ms 定时器：仅在秒表运行时更新读数 */
static gboolean sw_tick(gpointer p) {
    (void)p;
    if (sw_running) sw_update_display();
    return G_SOURCE_CONTINUE;
}

static void sw_set_button_text(void) {
    if (!sw_btn) return;
    if (sw_running)
        gtk_button_set_label(GTK_BUTTON(sw_btn), TR("暂停"));
    else if (sw_accum_us > 0)
        gtk_button_set_label(GTK_BUTTON(sw_btn), TR("继续"));
    else
        gtk_button_set_label(GTK_BUTTON(sw_btn), TR("秒表"));
}

/* 秒表按钮：开始 / 暂停 / 继续 */
static void on_sw_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (sw_running) {
        sw_accum_us += g_get_monotonic_time() - sw_start_us;
        sw_running = FALSE;
    } else {
        sw_start_us = g_get_monotonic_time();
        sw_running = TRUE;
    }
    sw_update_display();
    sw_set_button_text();
}

/* 复位按钮：清零并停止 */
static void on_sw_reset_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    sw_running = FALSE;
    sw_accum_us = 0;
    sw_update_display();
    sw_set_button_text();
}

/* 今天：日历选中并标记今天 */
static void cal_select_today(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    today_y = tm->tm_year + 1900;
    today_m = tm->tm_mon;
    today_d = tm->tm_mday;
    gtk_calendar_select_month(GTK_CALENDAR(cal), today_m, today_y);
    gtk_calendar_select_day(GTK_CALENDAR(cal), today_d);
    gtk_calendar_mark_day(GTK_CALENDAR(cal), today_d);
    g_printerr("QYCLOCKDBG: today=%04d-%02d-%02d\n", today_y, today_m + 1, today_d);
}

static void on_today_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    cal_select_today();
}

/* 新建按钮行按钮 */
static GtkWidget *make_clock_btn(const char *label, GCallback cb) {
    GtkWidget *b = gtk_button_new_with_label(label);
    qy_add_class(b, "qy-clock-btn");
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("时钟"));
    gtk_widget_set_name(win, "qy-clock-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("时钟"));
    qy_window_setup(GTK_WINDOW(win), 360, 420, 320, 400, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 大时钟 */
    clock_label = gtk_label_new("--:--:--");
    gtk_widget_set_name(clock_label, "qyclock-time");
    gtk_box_pack_start(GTK_BOX(vbox), clock_label, FALSE, FALSE, 0);

    /* 日期星期 */
    date_label = gtk_label_new("");
    gtk_widget_set_name(date_label, "qyclock-date");
    gtk_box_pack_start(GTK_BOX(vbox), date_label, FALSE, FALSE, 2);

    /* 月历（深色主题） */
    cal = gtk_calendar_new();
    gtk_widget_set_name(cal, "qyclock-cal");
    gtk_box_pack_start(GTK_BOX(vbox), cal, TRUE, TRUE, 4);

    /* 秒表读数 */
    sw_label = gtk_label_new("0:00.0");
    gtk_widget_set_name(sw_label, "qyclock-sw");
    gtk_box_pack_start(GTK_BOX(vbox), sw_label, FALSE, FALSE, 2);

    /* 底部按钮行：秒表 / 复位 / 今天 */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    sw_btn = make_clock_btn(TR("秒表"), G_CALLBACK(on_sw_clicked));
    gtk_box_pack_start(GTK_BOX(row), sw_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_clock_btn(TR("复位"), G_CALLBACK(on_sw_reset_clicked)), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_clock_btn(TR("今天"), G_CALLBACK(on_today_clicked)), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 4);

    gtk_widget_show_all(win);

    update_clock(NULL);
    g_timeout_add(250, update_clock, NULL);   /* 250ms：秒级显示 */
    g_timeout_add(100, sw_tick, NULL);          /* 100ms：秒表十分之一秒 */
    cal_select_today();

    /* 自动化: QYCLOCK_SW=start 启动后自动开始秒表 */
    const char *sw_env = g_getenv("QYCLOCK_SW");
    if (sw_env && strcmp(sw_env, "start") == 0) {
        sw_start_us = g_get_monotonic_time();
        sw_running = TRUE;
        sw_update_display();
        sw_set_button_text();
    }

    gtk_main();
    return 0;
}