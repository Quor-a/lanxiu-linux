/* qyclock.c — 澜岫时钟/日历/闹钟/世界时钟/计时器
 * 页0: 大时钟 + 日期星期 + 月历 + 秒表（开始/暂停/复位）+ 今天高亮。
 * 页1: 闹钟（时间输入 + 添加闹钟 + 闹钟列表 + 删除；到点 qynotify 或弹窗；
 *       存储 ~/.config/lanxiu/alarms.txt，每行 HH:MM）。
 * 页2: 世界时钟（北京/伦敦/纽约/东京/悉尼，UTC 偏移 +8/+0/-5/+9/+10，每 1s 刷新）。
 * 页3: 计时器（分/秒 SpinButton + 开始/停止重置；大字 MM:SS，1s 递减；
 *       到 0 调用 qynotify 计时结束，无 qynotify 则弹窗，并自动复位）。
 * 风格沿用 qylock 的大时钟与深色主题（#qy-clock-win）。
 * 自动化: QYCLOCK_SW=start 启动后自动开始秒表（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <time.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GtkWidget *clock_label   = NULL;   /* #qyclock-time 大时钟 */
static GtkWidget *date_label    = NULL;   /* #qyclock-date 日期星期 */
static GtkWidget *sw_label     = NULL;   /* #qyclock-sw 秒表读数 */
static GtkWidget *sw_btn         = NULL;   /* 秒表 开始/暂停/继续 */
static GtkWidget *cal            = NULL;   /* GtkCalendar */
static int today_y = 0, today_m = 0, today_d = 0;

/* ---------- 秒表状态 ---------- */
static gboolean sw_running  = FALSE;
static gint64   sw_start_us = 0;      /* 本次开始时刻（单调时钟） */
static gint64   sw_accum_us = 0;     /* 已累计运行时长 */

/* ---------- 闹钟 ---------- */
static GtkWidget *alarm_list = NULL;    /* GtkListBox，每行 HH:MM */
static GtkWidget *alarm_hour = NULL;      /* 时 GtkSpinButton */
static GtkWidget *alarm_min  = NULL;      /* 分 GtkSpinButton */
static gchar *alarm_fired_min = NULL;    /* 已触发过的分钟 HH:MM（避免同分钟重复触发） */

/* ---------- 世界时钟 ---------- */
typedef struct { const char *city; int offset; } CityEntry;
static const CityEntry CITIES[] = {
    { "北京", 8 },
    { "伦敦", 0 },
    { "纽约", -5 },
    { "东京", 9 },
    { "悉尼", 10 },
};
#define NCITIES ((int)(sizeof CITIES / sizeof CITIES[0]))
static GtkWidget *world_time_labels[NCITIES] = { NULL, NULL, NULL, NULL, NULL };

/* ---------- 计时器 ---------- */
static GtkWidget *timer_label  = NULL;   /* #qyclock-timer 大字 MM:SS */
static GtkWidget *timer_min    = NULL;   /* 分 SpinButton 0-99 */
static GtkWidget *timer_sec    = NULL;   /* 秒 SpinButton 0-59 */
static GtkWidget *timer_start  = NULL;   /* 开始按钮 */
static GtkWidget *timer_stop   = NULL;   /* 停止/重置按钮 */
static int   timer_remaining  = 0;      /* 剩余秒数 */
static gboolean timer_running  = FALSE;
static guint timer_source      = 0;      /* 1s 定时器 id */

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

/* ---------- 闹钟 ---------- */
static char *alarm_path(void) {
    return g_build_filename(g_get_home_dir(), ".config", "lanxiu", "alarms.txt", NULL);
}

static void alarm_save(void) {
    char *dir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GString *s = g_string_new(NULL);
    GList *ch = gtk_container_get_children(GTK_CONTAINER(alarm_list));
    for (GList *it = ch; it; it = it->next) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(GTK_WIDGET(it->data)));
        if (GTK_IS_LABEL(child)) {
            const char *txt = gtk_label_get_text(GTK_LABEL(child));
            if (strlen(txt) == 5 && txt[2] == ':')
                g_string_append_printf(s, "%s\n", txt);
        }
    }
    g_list_free(ch);
    char *path = alarm_path();
    g_file_set_contents(path, s->str, -1, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

static void alarm_add_row(const char *hhmm) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *lb = gtk_label_new(hhmm);
    gtk_widget_set_halign(lb, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lb, 12);
    gtk_widget_set_margin_end(lb, 12);
    gtk_widget_set_margin_top(lb, 7);
    gtk_widget_set_margin_bottom(lb, 7);
    qy_add_class(row, "qy-alarm-row");
    gtk_container_add(GTK_CONTAINER(row), lb);
    gtk_list_box_insert(GTK_LIST_BOX(alarm_list), row, -1);
}

static void alarm_load(void) {
    char *path = alarm_path();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (strlen(lines[i]) == 5 && lines[i][2] == ':')
                alarm_add_row(lines[i]);
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
}

/* 到点提醒：优先 qynotify，不可用则弹 GtkDialog */
static void show_alarm_dialog(const char *hhmm) {
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "%s", TR("闹钟提醒"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", hhmm);
    gtk_window_set_title(GTK_WINDOW(dlg), TR("闹钟"));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void alarm_fire(const char *hhmm) {
    gchar *cmd = g_strdup_printf("qynotify 闹钟提醒:%s", hhmm);
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) {
        g_printerr("qyclock: qynotify 不可用: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        show_alarm_dialog(hhmm);
    }
    g_free(cmd);
}

/* 1s 定时器：整分钟匹配闹钟 */
static gboolean alarm_tick(gpointer ud) {
    (void)ud;
    if (!alarm_list) return G_SOURCE_CONTINUE;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char now[8];
    g_snprintf(now, sizeof now, "%02d:%02d", tm->tm_hour, tm->tm_min);

    /* 同一分钟只处理一次，避免 1s 定时器反复触发 */
    if (alarm_fired_min && !strcmp(alarm_fired_min, now))
        return G_SOURCE_CONTINUE;
    g_free(alarm_fired_min);
    alarm_fired_min = g_strdup(now);

    GList *ch = gtk_container_get_children(GTK_CONTAINER(alarm_list));
    for (GList *it = ch; it; it = it->next) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(GTK_WIDGET(it->data)));
        if (GTK_IS_LABEL(child)) {
            const char *txt = gtk_label_get_text(GTK_LABEL(child));
            if (!strcmp(txt, now)) alarm_fire(now);
        }
    }
    g_list_free(ch);
    return G_SOURCE_CONTINUE;
}

static void on_alarm_add(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    int h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(alarm_hour));
    int m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(alarm_min));
    char buf[8];
    g_snprintf(buf, sizeof buf, "%02d:%02d", h, m);
    alarm_add_row(buf);
    alarm_save();

    /* 若添加的就是当前分钟，立即触发一次 */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char now[8];
    g_snprintf(now, sizeof now, "%02d:%02d", tm->tm_hour, tm->tm_min);
    if (!strcmp(now, buf)) alarm_fire(buf);
}

static void on_alarm_delete(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(alarm_list));
    if (sel) {
        gtk_widget_destroy(GTK_WIDGET(sel));
        alarm_save();
    }
}

/* ---------- 世界时钟 ---------- */
static gboolean world_tick(gpointer ud) {
    (void)ud;
    time_t now = time(NULL);
    for (int i = 0; i < NCITIES; i++) {
        if (!world_time_labels[i]) continue;
        time_t t = now + CITIES[i].offset * 3600;   /* 城市时间 = UTC + 偏移 */
        struct tm *tm = gmtime(&t);
        char buf[16];
        g_snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        gtk_label_set_text(GTK_LABEL(world_time_labels[i]), buf);
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- 计时器 ---------- */
/* 显示当前剩余时间 MM:SS */
static void timer_update_display(void) {
    if (!timer_label) return;
    char buf[16];
    int total = timer_remaining > 0 ? timer_remaining : 0;
    g_snprintf(buf, sizeof buf, "%02d:%02d", total / 60, total % 60);
    gtk_label_set_text(GTK_LABEL(timer_label), buf);
}

/* 复位显示为 SpinButton 当前设定的时长（未运行时调用） */
static void timer_reset_display(void) {
    if (!timer_label || !timer_min || !timer_sec) return;
    if (timer_running) return;   /* 运行中不干扰倒计时读数 */
    int m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(timer_min));
    int s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(timer_sec));
    char buf[16];
    g_snprintf(buf, sizeof buf, "%02d:%02d", m, s);
    gtk_label_set_text(GTK_LABEL(timer_label), buf);
}

/* 到点提醒：优先 qynotify，不可用则弹 GtkDialog（与闹钟一致） */
static void timer_fire_finished(void) {
    gchar *cmd = g_strdup("qynotify 计时结束");
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err)) {
        g_printerr("qyclock: qynotify 不可用: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                "%s", TR("计时结束"));
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s",
                                                 TR("倒计时已结束"));
        gtk_window_set_title(GTK_WINDOW(dlg), TR("计时器"));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
    g_free(cmd);
}

/* 1s 递减；到 0 提醒并自动复位为设定时长 */
static gboolean timer_tick(gpointer p) {
    (void)p;
    if (!timer_running) return G_SOURCE_CONTINUE;
    if (timer_remaining > 0) {
        timer_remaining--;
        timer_update_display();
    }
    if (timer_remaining <= 0) {
        timer_running = FALSE;
        if (timer_start) gtk_button_set_label(GTK_BUTTON(timer_start), TR("开始"));
        timer_fire_finished();
        /* 自动复位：回到 SpinButton 设定时长 */
        timer_reset_display();
    }
    return G_SOURCE_CONTINUE;
}

static void on_timer_start(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (timer_running) return;   /* 运行中忽略重复开始 */
    int m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(timer_min));
    int s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(timer_sec));
    timer_remaining = m * 60 + s;
    if (timer_remaining <= 0) return;
    timer_running = TRUE;
    gtk_button_set_label(GTK_BUTTON(timer_start), TR("开始"));
    timer_update_display();
    if (!timer_source)
        timer_source = g_timeout_add(1000, timer_tick, NULL);
}

static void on_timer_stop(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    timer_running = FALSE;
    timer_remaining = 0;
    gtk_button_set_label(GTK_BUTTON(timer_start), TR("开始"));
    timer_reset_display();
}

/* make_clock_btn 在下方定义，这里先声明（build_timer_page 使用） */
static GtkWidget *make_clock_btn(const char *label, GCallback cb);

/* 计时器页面 */
static GtkWidget *build_timer_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 14);

    timer_label = gtk_label_new("05:00");
    gtk_widget_set_name(timer_label, "qyclock-timer");
    gtk_box_pack_start(GTK_BOX(page), timer_label, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

    GtkWidget *ml = gtk_label_new(TR("分"));
    GtkWidget *sl = gtk_label_new(TR("秒"));
    timer_min = gtk_spin_button_new_with_range(0, 99, 1);
    timer_sec = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(timer_min), 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(timer_sec), 0);
    gtk_entry_set_width_chars(GTK_ENTRY(timer_min), 3);
    gtk_entry_set_width_chars(GTK_ENTRY(timer_sec), 3);
    /* 未运行时，修改时长即时预览大字 */
    g_signal_connect(timer_min, "value-changed", G_CALLBACK(timer_reset_display), NULL);
    g_signal_connect(timer_sec, "value-changed", G_CALLBACK(timer_reset_display), NULL);

    gtk_box_pack_start(GTK_BOX(row), ml, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), timer_min, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), sl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), timer_sec, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    timer_start = make_clock_btn(TR("开始"), G_CALLBACK(on_timer_start));
    timer_stop  = make_clock_btn(TR("停止/重置"), G_CALLBACK(on_timer_stop));
    gtk_box_pack_start(GTK_BOX(btns), timer_start, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btns), timer_stop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), btns, FALSE, FALSE, 0);

    return page;
}

/* ---------- 页面构建 ---------- */
static GtkWidget *build_alarm_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);

    /* 时间输入行 */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hl = gtk_label_new(TR("时"));
    GtkWidget *ml = gtk_label_new(TR("分"));
    alarm_hour = gtk_spin_button_new_with_range(0, 23, 1);
    alarm_min  = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(alarm_hour), 7);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(alarm_min), 0);
    gtk_entry_set_width_chars(GTK_ENTRY(alarm_hour), 3);
    gtk_entry_set_width_chars(GTK_ENTRY(alarm_min), 3);
    GtkWidget *add = gtk_button_new_with_label(TR("添加闹钟"));
    qy_add_class(add, "qy-clock-btn");
    g_signal_connect(add, "clicked", G_CALLBACK(on_alarm_add), NULL);

    gtk_box_pack_start(GTK_BOX(row), hl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), alarm_hour, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ml, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), alarm_min, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    /* 闹钟列表 */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 200);
    alarm_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(alarm_list), GTK_SELECTION_SINGLE);
    qy_add_class(alarm_list, "qy-alarm-list");
    gtk_container_add(GTK_CONTAINER(sw), alarm_list);
    gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);

    /* 删除按钮 */
    GtkWidget *del = gtk_button_new_with_label(TR("删除"));
    gtk_widget_set_halign(del, GTK_ALIGN_END);
    qy_add_class(del, "qy-clock-btn");
    g_signal_connect(del, "clicked", G_CALLBACK(on_alarm_delete), NULL);
    gtk_box_pack_start(GTK_BOX(page), del, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *build_world_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 14);

    for (int i = 0; i < NCITIES; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(row, 8);
        gtk_widget_set_margin_bottom(row, 8);
        qy_add_class(row, "qy-world-row");

        GtkWidget *city = gtk_label_new(CITIES[i].city);
        gtk_widget_set_halign(city, GTK_ALIGN_START);
        qy_add_class(city, "qy-world-city");
        gchar *tz = g_strdup_printf("UTC%+d", CITIES[i].offset);
        GtkWidget *offset = gtk_label_new(tz);
        g_free(tz);
        qy_add_class(offset, "qy-world-offset");

        GtkWidget *tl = gtk_label_new("--:--:--");
        qy_add_class(tl, "qy-world-time");
        world_time_labels[i] = tl;

        gtk_box_pack_start(GTK_BOX(row), city, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), offset, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(row), tl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    }
    return page;
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
    qy_window_setup(GTK_WINDOW(win), 420, 540, 360, 480, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* 底部 Tab 的 Notebook */
    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_BOTTOM);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(nb), FALSE);
    gtk_container_add(GTK_CONTAINER(win), nb);

    /* 页0: 时钟/日历/秒表 */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);

    clock_label = gtk_label_new("--:--:--");
    gtk_widget_set_name(clock_label, "qyclock-time");
    gtk_box_pack_start(GTK_BOX(vbox), clock_label, FALSE, FALSE, 0);

    date_label = gtk_label_new("");
    gtk_widget_set_name(date_label, "qyclock-date");
    gtk_box_pack_start(GTK_BOX(vbox), date_label, FALSE, FALSE, 2);

    cal = gtk_calendar_new();
    gtk_widget_set_name(cal, "qyclock-cal");
    gtk_box_pack_start(GTK_BOX(vbox), cal, TRUE, TRUE, 4);

    sw_label = gtk_label_new("0:00.0");
    gtk_widget_set_name(sw_label, "qyclock-sw");
    gtk_box_pack_start(GTK_BOX(vbox), sw_label, FALSE, FALSE, 2);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    sw_btn = make_clock_btn(TR("秒表"), G_CALLBACK(on_sw_clicked));
    gtk_box_pack_start(GTK_BOX(row), sw_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_clock_btn(TR("复位"), G_CALLBACK(on_sw_reset_clicked)), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_clock_btn(TR("今天"), G_CALLBACK(on_today_clicked)), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 4);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbox, gtk_label_new(TR("时钟")));

    /* 页1: 闹钟 */
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_alarm_page(), gtk_label_new(TR("闹钟")));

    /* 页2: 世界时钟 */
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_world_page(), gtk_label_new(TR("世界时钟")));

    /* 页3: 计时器 */
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_timer_page(), gtk_label_new(TR("计时器")));

    gtk_widget_show_all(win);

    update_clock(NULL);
    g_timeout_add(250, update_clock, NULL);   /* 250ms：秒级显示 */
    g_timeout_add(100, sw_tick, NULL);          /* 100ms：秒表十分之一秒 */
    g_timeout_add(1000, alarm_tick, NULL);   /* 1s：闹钟检查 */
    g_timeout_add(1000, world_tick, NULL);    /* 1s：世界时钟刷新 */
    timer_source = g_timeout_add(1000, timer_tick, NULL);  /* 1s：倒计时递减 */
    world_tick(NULL);
    cal_select_today();
    alarm_load();
    timer_reset_display();

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