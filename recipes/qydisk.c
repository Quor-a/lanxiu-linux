/* qydisk.c — 澜岫磁盘分析
 * 读取 `df -h` 输出并解析（g_spawn_command_line_sync），
 * 每行显示：挂载点（#qy-disk-name）+ 已用比例进度条（橙色填充）
 *         + 容量文本（#qy-disk-size）。
 * 顶部汇总总容量/已用/可用（#qy-disk-sum）。
 * 自动化: QYDISK_REFRESH=1 启动后延迟 1s 自动刷新一次（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

typedef struct {
    gchar *mount;
    gchar *size_str;
    gchar *used_str;
    gchar *avail_str;
    guint64 size;
    guint64 used;
    guint64 avail;
    int pct;
} DiskInfo;

static GtkWidget *sum_lb = NULL;   /* #qy-disk-sum */
static GtkWidget *rows_box = NULL;  /* 挂载点列表容器 */
static GtkWidget *err_lb = NULL;    /* 错误提示 */

/* 解析 "1.5G"、"512M" 等人类可读容量为字节数 */
static guint64 parse_size(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end == s) return 0;
    switch (g_ascii_toupper(*end)) {
        case 'K': v *= 1024.0; break;
        case 'M': v *= 1024.0 * 1024; break;
        case 'G': v *= 1024.0 * 1024 * 1024; break;
        case 'T': v *= 1024.0 * 1024 * 1024 * 1024; break;
        case 'P': v *= 1024.0 * 1024 * 1024 * 1024 * 1024; break;
        default: break;
    }
    return (guint64)v;
}

static void disk_info_free(gpointer p) {
    DiskInfo *d = (DiskInfo *)p;
    if (!d) return;
    g_free(d->mount);
    g_free(d->size_str);
    g_free(d->used_str);
    g_free(d->avail_str);
    g_free(d);
}

/* 解析 df -h 一行：从行尾取 挂载点/Use%/可用/已用/容量，文件系统名可能含空格 */
static void parse_df_line(char *line, GPtrArray *list) {
    g_strstrip(line);
    if (!line[0] || g_str_has_prefix(line, "Filesystem")) return;

    gchar **t = g_strsplit_set(line, " \t", -1);
    int n = 0;
    while (t[n]) n++;
    if (n < 6) {                       /* 至少: fs size used avail use% mount */
        g_strfreev(t);
        return;
    }

    DiskInfo *d = g_new0(DiskInfo, 1);
    d->mount    = g_strdup(t[n - 1]);
    d->avail_str = g_strdup(t[n - 3]);
    d->used_str = g_strdup(t[n - 4]);
    d->size_str = g_strdup(t[n - 5]);

    d->pct = atoi(t[n - 2]);           /* "Use%" 中的数字部分 */
    d->size = parse_size(d->size_str);
    d->used = parse_size(d->used_str);
    d->avail = parse_size(d->avail_str);
    g_strfreev(t);

    if (d->pct < 0) d->pct = 0;
    if (d->pct > 100) d->pct = 100;
    g_ptr_array_add(list, d);
}

/* 刷新：执行 df -h 并重建列表与汇总 */
static void refresh_disk(void) {
    gchar *out = NULL, *err = NULL;
    GError *error = NULL;
    gboolean ok = g_spawn_command_line_sync("df -h", &out, &err, NULL, &error);
    if (!ok || !out) {
        if (err_lb) {
            gchar *msg = g_strdup_printf("%s: %s", TR("磁盘信息读取失败"),
                                         error ? error->message : (err ? err : "df"));
            gtk_label_set_text(GTK_LABEL(err_lb), msg);
            gtk_widget_show(err_lb);
            g_free(msg);
        }
        g_clear_error(&error);
        g_free(err);
        g_free(out);
        return;
    }
    g_free(err);

    GPtrArray *list = g_ptr_array_new_with_free_func(disk_info_free);
    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++)
        parse_df_line(lines[i], list);
    g_strfreev(lines);
    g_free(out);

    /* 重建行列表 */
    GList *ch = gtk_container_get_children(GTK_CONTAINER(rows_box));
    for (GList *it = ch; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(ch);

    guint64 total = 0, used = 0, avail = 0;
    for (guint i = 0; i < list->len; i++) {
        DiskInfo *d = g_ptr_array_index(list, i);
        total += d->size;
        used  += d->used;
        avail += d->avail;

        /* 每行：挂载点 + 容量文本 + 进度条 */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        qy_add_class(row, "qy-disk-row");

        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name = gtk_label_new(d->mount);
        gtk_widget_set_name(name, "qy-disk-name");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);

        gchar *size_txt = g_strdup_printf("%s 已用 %s (%d%%) · 可用 %s",
                                          d->size_str, d->used_str, d->pct, d->avail_str);
        GtkWidget *size = gtk_label_new(size_txt);
        gtk_widget_set_name(size, "qy-disk-size");
        gtk_label_set_xalign(GTK_LABEL(size), 1.0);
        g_free(size_txt);

        gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(top), size, FALSE, FALSE, 0);

        GtkWidget *bar = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), d->pct / 100.0);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), FALSE);
        gtk_widget_set_size_request(bar, -1, 10);

        gtk_box_pack_start(GTK_BOX(row), top, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), bar, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rows_box), row, FALSE, FALSE, 6);
    }

    /* 汇总 */
    if (sum_lb) {
        gchar *total_s = g_format_size(total);
        gchar *used_s  = g_format_size(used);
        gchar *avail_s = g_format_size(avail);
        gchar *sum = g_strdup_printf("%s %s · %s %s · %s %s",
                                     TR("总容量"), total_s,
                                     TR("已用"), used_s,
                                     TR("可用"), avail_s);
        gtk_label_set_text(GTK_LABEL(sum_lb), sum);
        g_free(sum);
        g_free(total_s);
        g_free(used_s);
        g_free(avail_s);
    }

    g_printerr("QYDISKDBG: rows=%u total=%" G_GUINT64_FORMAT " used=%" G_GUINT64_FORMAT
               " avail=%" G_GUINT64_FORMAT "\n",
               list->len, total, used, avail);
    g_ptr_array_free(list, TRUE);

    if (err_lb) gtk_widget_hide(err_lb);
}

/* 自动化刷新包装（GSourceFunc） */
static gboolean auto_refresh(gpointer p) {
    (void)p;
    refresh_disk();
    return G_SOURCE_REMOVE;
}

/* ---------- UI ---------- */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("磁盘分析"));
    gtk_widget_set_name(win, "qy-disk-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("磁盘分析"));
    qy_window_setup(GTK_WINDOW(win), 560, 480, 480, 420, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶部汇总 + 刷新 */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    sum_lb = gtk_label_new("");
    gtk_widget_set_name(sum_lb, "qy-disk-sum");
    gtk_label_set_xalign(GTK_LABEL(sum_lb), 0.0);
    GtkWidget *refresh_btn = gtk_button_new_with_label(TR("刷新"));
    qy_add_class(refresh_btn, "qy-btn");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(refresh_disk), NULL);
    gtk_box_pack_start(GTK_BOX(top), sum_lb, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    err_lb = gtk_label_new("");
    gtk_widget_set_name(err_lb, "qy-disk-err");
    gtk_label_set_xalign(GTK_LABEL(err_lb), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), err_lb, FALSE, FALSE, 0);

    /* 滚动列表 */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(sw), rows_box);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    gtk_widget_show_all(win);
    gtk_widget_hide(err_lb);

    refresh_disk();

    /* 自动化: QYDISK_REFRESH=1 启动后延迟刷新一次 */
    if (g_getenv("QYDISK_REFRESH"))
        g_timeout_add(1000, auto_refresh, NULL);

    gtk_main();
    return 0;
}