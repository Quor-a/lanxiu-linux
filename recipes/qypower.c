/* qypower.c — 澜岫电池/电源状态面板
 * 读取 /sys/class/power_supply 下各电池节点 (capacity/status/voltage_now/energy_now/power_now)，
 * cairo 环形电量图（深蓝底环 + 橙色电量弧）+ 右侧信息列，
 * 底部省电模式开关（写 /etc/qypower.conf）+ 亮度滑杆（写 backlight，可写时启用）。
 * 无电池时显示"未检测到电池"，不崩溃。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

#define POWER_CONF "/etc/qypower.conf"

static GtkWidget *pct_label = NULL;     /* #qy-power-pct 中央大百分比 */
static GtkWidget *status_label = NULL; /* #qy-power-status */
static GtkWidget *time_label = NULL;   /* 剩余时间 */
static GtkWidget *volt_label = NULL;  /* 电压 */
static GtkWidget *ring_da = NULL;      /* 环形电量绘制区 */
static GtkWidget *saver_sw = NULL;    /* 省电模式开关 */
static GtkWidget *bright_scale = NULL; /* 亮度滑杆 */
static char battery_path[256] = "";    /* 电池目录 */
static char backlight_path[256] = ""; /* 背光目录 */
static int capacity = -1;
static gboolean charging = FALSE;
static long voltage_now = 0;
static long energy_now = 0, power_now = 0;

/* ---------- sysfs 读取 ---------- */
static int read_int_file(const char *path, long *val) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int rc = fscanf(f, "%ld", val);
    fclose(f);
    return rc == 1 ? 0 : -1;
}

static int read_str_file(const char *path, char *buf, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return 0;
}

static gboolean find_battery(void) {
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d) return FALSE;
    const gchar *name;
    gboolean found = FALSE;
    while (!found && (name = g_dir_read_name(d))) {
        gchar *dir = g_build_filename("/sys/class/power_supply", name, NULL);
        gchar *cap = g_build_filename(dir, "capacity", NULL);
        gchar *type = g_build_filename(dir, "type", NULL);
        char tbuf[32] = "";
        read_str_file(type, tbuf, sizeof tbuf);
        if (g_file_test(cap, G_FILE_TEST_EXISTS) &&
            (tbuf[0] == 0 || g_str_has_prefix(tbuf, "Battery"))) {
            g_strlcpy(battery_path, dir, sizeof battery_path);
            found = TRUE;
        }
        g_free(dir); g_free(cap); g_free(type);
    }
    g_dir_close(d);
    return found;
}

static void refresh_power(void) {
    if (battery_path[0]) {
        gchar *cap = g_build_filename(battery_path, "capacity", NULL);
        gchar *st  = g_build_filename(battery_path, "status", NULL);
        gchar *vol = g_build_filename(battery_path, "voltage_now", NULL);
        gchar *en  = g_build_filename(battery_path, "energy_now", NULL);
        gchar *pw  = g_build_filename(battery_path, "power_now", NULL);
        long v = 0;
        capacity = (read_int_file(cap, &v) == 0) ? (int)v : -1;
        char s[32] = "Unknown";
        read_str_file(st, s, sizeof s);
        charging = strstr(s, "Charg") != NULL;
        voltage_now = 0; energy_now = 0; power_now = 0;
        read_int_file(vol, &voltage_now);
        read_int_file(en, &energy_now);
        read_int_file(pw, &power_now);
        g_free(cap); g_free(st); g_free(vol); g_free(en); g_free(pw);
    } else {
        capacity = -1;
    }
}

/* ---------- UI 刷新 ---------- */
static void update_ui(void) {
    if (capacity < 0) {
        gtk_label_set_text(GTK_LABEL(pct_label), "--");
        gtk_label_set_text(GTK_LABEL(status_label), TR("未检测到电池"));
        gtk_label_set_text(GTK_LABEL(time_label), TR("剩余时间 --"));
        gtk_label_set_text(GTK_LABEL(volt_label), TR("电压 --"));
        gtk_widget_set_sensitive(saver_sw, FALSE);
    } else {
        gchar buf[64];
        g_snprintf(buf, sizeof buf, "%d%%", capacity);
        gtk_label_set_text(GTK_LABEL(pct_label), buf);

        const char *st;
        if (charging) st = TR("充电中");
        else if (capacity >= 100) st = TR("已充满");
        else st = TR("放电中");
        gtk_label_set_text(GTK_LABEL(status_label), st);

        /* 剩余时间估算: energy_now/power_now (µWh / µW) */
        if (power_now > 0 && energy_now > 0) {
            double hours = (double)energy_now / (double)power_now;
            int h = (int)hours;
            int m = (int)((hours - h) * 60 + 0.5);
            g_snprintf(buf, sizeof buf, TR("剩余时间 %d 小时 %d 分"), h, m);
        } else {
            g_snprintf(buf, sizeof buf, "%s", TR("剩余时间 --"));
        }
        gtk_label_set_text(GTK_LABEL(time_label), buf);

        if (voltage_now > 0)
            g_snprintf(buf, sizeof buf, "电压 %.2f V", voltage_now / 1000000.0);
        else
            g_snprintf(buf, sizeof buf, "%s", TR("电压 --"));
        gtk_label_set_text(GTK_LABEL(volt_label), buf);
        gtk_widget_set_sensitive(saver_sw, TRUE);
    }
    gtk_widget_queue_draw(ring_da);
}

static gboolean power_tick(gpointer ud) {
    (void)ud;
    refresh_power();
    update_ui();
    return G_SOURCE_CONTINUE;
}

/* ---------- 环形电量图 ---------- */
static gboolean ring_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    double cx = a.width / 2.0, cy = a.height / 2.0;
    double r = (a.width < a.height ? a.width : a.height) / 2.0 - 10.0;
    if (r < 10) return FALSE;

    /* 深蓝底环 */
    cairo_set_line_width(cr, 10.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, 0.11, 0.14, 0.20);   /* #1c2434 */
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_stroke(cr);

    /* 电量弧 */
    double frac = capacity >= 0 ? capacity / 100.0 : 0.0;
    if (frac > 0) {
        double start = -G_PI / 2.0;
        double end = start + 2 * G_PI * frac;
        if (charging)
            cairo_set_source_rgb(cr, 0.298, 0.686, 0.314);   /* 充电绿 #4CAF50 */
        else if (capacity < 20)
            cairo_set_source_rgb(cr, 0.898, 0.282, 0.302);   /* 低电红 #E5484D */
        else
            cairo_set_source_rgb(cr, 1.0, 0.361, 0.102);      /* 主橙 #FF5C1A */
        cairo_arc(cr, cx, cy, r, start, end);
        cairo_stroke(cr);
    }
    return FALSE;
}

/* ---------- 省电模式 ---------- */
static void on_saver_toggled(GtkSwitch *sw, gboolean state, gpointer ud) {
    (void)sw; (void)ud;
    FILE *f = fopen(POWER_CONF, "w");
    if (f) {
        fprintf(f, "power_saver=%d\n", state ? 1 : 0);
        fclose(f);
    }
    g_printerr("QYPOWERDBG: power_saver=%d\n", state ? 1 : 0);
}

static gboolean on_saver_state_set(GtkSwitch *sw, gboolean state, gpointer ud) {
    on_saver_toggled(sw, state, ud);
    return FALSE;   /* 让开关正常更新显示状态 */
}

static void power_conf_load(void) {
    gchar *content = NULL;
    if (!g_file_get_contents(POWER_CONF, &content, NULL, NULL)) return;
    char *line = content;
    while (line && *line) {
        int v = 0;
        if (sscanf(line, "power_saver=%d", &v) == 1)
            gtk_switch_set_active(GTK_SWITCH(saver_sw), v != 0);
        char *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
}

/* ---------- 亮度 ---------- */
static gboolean find_backlight(void) {
    GDir *d = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!d) return FALSE;
    const gchar *name;
    gboolean found = FALSE;
    while (!found && (name = g_dir_read_name(d))) {
        gchar *dir = g_build_filename("/sys/class/backlight", name, NULL);
        gchar *br = g_build_filename(dir, "brightness", NULL);
        if (g_file_test(br, G_FILE_TEST_EXISTS)) {
            g_strlcpy(backlight_path, dir, sizeof backlight_path);
            found = TRUE;
        }
        g_free(dir); g_free(br);
    }
    g_dir_close(d);
    return found;
}

static void brightness_apply(int pct) {
    if (!backlight_path[0]) return;
    gchar *maxp = g_build_filename(backlight_path, "max_brightness", NULL);
    gchar *brp = g_build_filename(backlight_path, "brightness", NULL);
    long maxv = 100;
    read_int_file(maxp, &maxv);
    if (maxv <= 0) maxv = 100;
    long val = maxv * pct / 100;
    if (val < 1) val = 1;
    FILE *f = fopen(brp, "w");
    if (f) { fprintf(f, "%ld\n", val); fclose(f); }
    g_free(maxp); g_free(brp);
}

static void on_bright_changed(GtkRange *r, gpointer ud) {
    (void)ud;
    brightness_apply((int)gtk_range_get_value(r));
}

static void brightness_init(void) {
    if (!backlight_path[0]) {
        gtk_widget_set_sensitive(bright_scale, FALSE);
        gtk_widget_set_tooltip_text(bright_scale, TR("无可用背光设备"));
        return;
    }
    gchar *brp = g_build_filename(backlight_path, "brightness", NULL);
    if (access(brp, W_OK) != 0) {
        gtk_widget_set_sensitive(bright_scale, FALSE);
        gtk_widget_set_tooltip_text(bright_scale, TR("无权限调整亮度"));
        g_free(brp);
        return;
    }
    g_free(brp);

    gchar *maxp = g_build_filename(backlight_path, "max_brightness", NULL);
    long maxv = 100, cur = 0;
    read_int_file(maxp, &maxv);
    g_free(maxp);
    gchar *curp = g_build_filename(backlight_path, "brightness", NULL);
    read_int_file(curp, &cur);
    g_free(curp);
    if (maxv <= 0) maxv = 100;
    int pct = cur * 100 / maxv;
    g_signal_handlers_block_by_func(bright_scale, on_bright_changed, NULL);
    gtk_range_set_value(GTK_RANGE(bright_scale), pct);
    g_signal_handlers_unblock_by_func(bright_scale, on_bright_changed, NULL);
}

/* ---------- UI ---------- */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("电源管理"));
    gtk_widget_set_name(win, "qy-power-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("电源管理"));
    qy_window_setup(GTK_WINDOW(win), 400, 320, 360, 280, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 主区：环形图 + 信息列 */
    GtkWidget *main_h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_pack_start(GTK_BOX(vbox), main_h, TRUE, TRUE, 0);

    GtkWidget *ring_box = gtk_overlay_new();
    gtk_widget_set_size_request(ring_box, 150, 150);
    ring_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(ring_da, 150, 150);
    g_signal_connect(ring_da, "draw", G_CALLBACK(ring_draw_cb), NULL);
    gtk_container_add(GTK_CONTAINER(ring_box), ring_da);
    pct_label = gtk_label_new("--");
    gtk_widget_set_name(pct_label, "qy-power-pct");
    gtk_widget_set_halign(pct_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(pct_label, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(ring_box), pct_label);
    gtk_box_pack_start(GTK_BOX(main_h), ring_box, FALSE, FALSE, 0);

    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    status_label = gtk_label_new("--");
    gtk_widget_set_name(status_label, "qy-power-status");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    time_label = gtk_label_new("--");
    gtk_widget_set_name(time_label, "qy-power-time");
    gtk_widget_set_halign(time_label, GTK_ALIGN_START);
    volt_label = gtk_label_new("--");
    gtk_widget_set_name(volt_label, "qy-power-voltage");
    gtk_widget_set_halign(volt_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(info), status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), time_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), volt_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_h), info, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    /* 底部：省电模式 + 亮度 */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *saver_lb = gtk_label_new(TR("省电模式"));
    qy_add_class(saver_lb, "qy-power-saver-label");
    saver_sw = gtk_switch_new();
    qy_add_class(saver_sw, "qy-power-saver");
    g_signal_connect(saver_sw, "state-set", G_CALLBACK(on_saver_state_set), NULL);
    GtkWidget *bright_lb = gtk_label_new(TR("亮度"));
    qy_add_class(bright_lb, "qy-power-bright-label");
    bright_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    qy_add_class(bright_scale, "qy-power-bright");
    gtk_scale_set_draw_value(GTK_SCALE(bright_scale), FALSE);
    g_signal_connect(bright_scale, "value-changed", G_CALLBACK(on_bright_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), saver_lb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), saver_sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), bright_lb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), bright_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    /* 初始化数据 */
    find_battery();
    find_backlight();
    refresh_power();
    update_ui();
    brightness_init();
    power_conf_load();
    g_timeout_add_seconds(2, power_tick, NULL);

    gtk_main();
    return 0;
}