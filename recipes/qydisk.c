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

/* 挂载树 */
static GtkListStore *mount_store = NULL;   /* 挂载树数据 */
static GtkWidget *mount_tv = NULL;         /* 挂载树视图 */
static gchar *g_mount_target = NULL;         /* 当前选中挂载点（卸载用，release 286） */
static GtkWidget *b_unmount = NULL;         /* 卸载按钮（release 286） */
static GtkWidget *g_win = NULL;               /* 主窗口（release 286：卸载确认对话框父窗口） */
static void refresh_all(GtkWidget *w, gpointer p);   /* 前向声明 */
static GtkWidget *fs_info_label = NULL;    /* #qydisk-fs-info 选中挂载点详情 */

enum { M_TARGET, M_FSTYPE, M_SIZE, M_USED, M_AVAIL, N_MCOLS };

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

/* ---------- 挂载树 / 文件系统信息 ---------- */
/* 向挂载树添加一行 */
static void mount_add_row(const char *target, const char *fstype,
                          const char *size, const char *used, const char *avail) {
    if (!mount_store) return;
    GtkTreeIter it;
    gtk_list_store_append(mount_store, &it);
    gtk_list_store_set(mount_store, &it,
                       M_TARGET, target ? target : "?",
                       M_FSTYPE, fstype ? fstype : "?",
                       M_SIZE,   size   ? size   : "—",
                       M_USED,   used   ? used   : "—",
                       M_AVAIL,  avail  ? avail  : "—", -1);
}

/* 回退方案：用 mount 获取 挂载点/类型，用 df -h 补 容量/已用/可用 */
static void mount_fallback(void) {
    gchar *mout = NULL, *derr = NULL, *dout = NULL;
    GError *err = NULL;
    gboolean ok = g_spawn_command_line_sync("mount", &mout, &derr, NULL, &err);
    if (!ok || !mout) {
        g_clear_error(&err);
        g_free(derr);
        g_free(mout);
        g_printerr("qydisk: mount 不可用，挂载树为空\n");
        return;
    }
    g_free(derr);
    ok = g_spawn_command_line_sync("df -h", &dout, &derr, NULL, &err);
    g_clear_error(&err);
    g_free(derr);

    /* 解析 df -h：挂载点 -> (容量, 已用, 可用) */
    GHashTable *dfmap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (ok && dout) {
        gchar **lines = g_strsplit(dout, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar **t = g_strsplit_set(lines[i], " \t", -1);
            int n = 0;
            while (t[n]) n++;
            if (n >= 6 && !g_str_has_prefix(t[0], "Filesystem")) {
                /* t[n-5]=容量 t[n-4]=已用 t[n-3]=可用 t[n-1]=挂载点 */
                gchar *v = g_strdup_printf("%s|%s|%s", t[n - 5], t[n - 4], t[n - 3]);
                g_hash_table_replace(dfmap, g_strdup(t[n - 1]), v);
            }
            g_strfreev(t);
        }
        g_strfreev(lines);
    }
    g_free(dout);

    gchar **lines = g_strsplit(mout, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!line[0]) continue;
        gchar **t = g_strsplit_set(line, " \t", -1);
        int n = 0;
        while (t[n]) n++;
        /* mount 格式: device on mountpoint type fstype (options) */
        if (n >= 5 && !strcmp(t[1], "on") && !strcmp(t[3], "type")) {
            const char *mp = t[2];
            const char *fs = t[4];
            const char *size = "—", *used = "—", *avail = "—";
            gchar *dfv = g_hash_table_lookup(dfmap, mp);
            if (dfv) {
                gchar **parts = g_strsplit(dfv, "|", 3);
                if (parts[0]) size = parts[0];
                if (parts[1]) used = parts[1];
                if (parts[2]) avail = parts[2];
                mount_add_row(mp, fs, size, used, avail);
                g_strfreev(parts);
            } else {
                mount_add_row(mp, fs, size, used, avail);
            }
        }
        g_strfreev(t);
    }
    g_strfreev(lines);
    g_free(mout);
    g_hash_table_destroy(dfmap);
}

/* 刷新挂载树：优先 findmnt -rno TARGET,FSTYPE,SIZE,USED,AVAIL */
static void refresh_mount_tree(void) {
    if (!mount_store) return;
    gtk_list_store_clear(mount_store);

    gchar *out = NULL, *err_out = NULL;
    GError *error = NULL;
    gboolean ok = g_spawn_command_line_sync(
        "findmnt -rno TARGET,FSTYPE,SIZE,USED,AVAIL",
        &out, &err_out, NULL, &error);
    if (ok && out && out[0]) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!line[0]) continue;
            gchar **t = g_strsplit_set(line, " \t", -1);
            int n = 0;
            while (t[n]) n++;
            /* 挂载点 类型 容量 已用 可用；findmnt -r 每行 5 列，异常少列时补 "—" */
            if (n >= 2) {
                const char *target = t[0];
                const char *fstype = t[1];
                const char *size = "—", *used = "—", *avail = "—";
                if (n >= 5) {
                    size  = t[n - 3];
                    used  = t[n - 2];
                    avail = t[n - 1];
                }
                mount_add_row(target, fstype, size, used, avail);
            }
            g_strfreev(t);
        }
        g_strfreev(lines);
    } else {
        g_clear_error(&error);
        g_free(err_out);
        g_free(out);
        mount_fallback();
    }
    g_free(err_out);
    g_free(out);
    g_printerr("QYDISKDBG: mount rows=%d\n",
               mount_store ? gtk_tree_model_iter_n_children(GTK_TREE_MODEL(mount_store), NULL) : 0);
}

/* 点击挂载树行：读 /proc/mounts 匹配，显示设备/类型/只读/选项 */
static void on_mount_cursor_changed(GtkTreeView *tv, gpointer ud) {
    (void)tv; (void)ud;
    if (!fs_info_label || !mount_store) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(mount_tv));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        if (b_unmount) gtk_widget_set_sensitive(b_unmount, FALSE);
        return;
    }
    gchar *target = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(mount_store), &it, M_TARGET, &target, -1);
    if (!target) {
        if (b_unmount) gtk_widget_set_sensitive(b_unmount, FALSE);
        return;
    }
    g_free(g_mount_target);
    g_mount_target = g_strdup(target);
    if (b_unmount)
        gtk_widget_set_sensitive(b_unmount,
            target[0] == '/' && strcmp(target, "/") != 0);

    gchar *content = NULL;
    if (g_file_get_contents("/proc/mounts", &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char dev[256], mp[512], type[64], opts[1024];
            if (sscanf(lines[i], "%255s %511s %63s %1023s", dev, mp, type, opts) == 4) {
                if (!strcmp(mp, target)) {
                    gboolean ro = strstr(opts, "ro") != NULL;
                    gchar *s = g_strdup_printf("%s: %s\n%s: %s\n%s: %s\n%s: %s",
                                               TR("设备"), dev,
                                               TR("类型"), type,
                                               TR("只读"), ro ? TR("是") : TR("否"),
                                               TR("选项"), opts);
                    gtk_label_set_text(GTK_LABEL(fs_info_label), s);
                    g_free(s);
                    break;
                }
            }
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(target);
}

/* 卸载当前选中挂载点（release 286） */
static void on_unmount_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!g_mount_target || g_mount_target[0] != '/') return;
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        TR("确定卸载 %s 吗？"), g_mount_target);
    gtk_window_set_title(GTK_WINDOW(dlg), TR("卸载"));
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_YES) return;

    gchar *cmd = g_strdup_printf("qysudo -n umount '%s' 2>&1", g_mount_target);
    GError *err = NULL;
    gchar *out = NULL, *errout = NULL;
    gint status = 1;
    gboolean ok = g_spawn_command_line_sync(cmd, &out, &errout, &status, &err);
    if (!ok || status != 0) {
        gchar *msg = (errout && errout[0]) ? errout : out;
        gtk_label_set_text(GTK_LABEL(err_lb), (msg && msg[0]) ? msg : TR("卸载失败"));
    } else {
        gtk_label_set_text(GTK_LABEL(err_lb), TR("已卸载"));
    }
    g_free(out); g_free(errout); g_free(cmd);
    if (err) g_clear_error(&err);
    refresh_all(NULL, NULL);
}

/* 自动化刷新包装（GSourceFunc） */
static gboolean auto_refresh(gpointer p) {
    (void)p;
    refresh_disk();
    refresh_mount_tree();
    return G_SOURCE_REMOVE;
}

/* 刷新按钮：同时刷新 df 汇总与挂载树 */
static void refresh_all(GtkWidget *w, gpointer p) {
    (void)w; (void)p;
    refresh_disk();
    refresh_mount_tree();
}

/* ---------- UI ---------- */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_win = win;   /* release 286 */
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
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(refresh_all), NULL);
    b_unmount = gtk_button_new_with_label(TR("卸载选中"));
    qy_add_class(b_unmount, "qy-btn qy-btn-danger");
    gtk_widget_set_sensitive(b_unmount, FALSE);
    g_signal_connect(b_unmount, "clicked", G_CALLBACK(on_unmount_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), sum_lb, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), b_unmount, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    err_lb = gtk_label_new("");
    gtk_widget_set_name(err_lb, "qy-disk-err");
    gtk_label_set_xalign(GTK_LABEL(err_lb), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), err_lb, FALSE, FALSE, 0);

    /* 滚动列表（df 汇总） */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(sw), rows_box);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    /* 挂载树区（底部） */
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);
    GtkWidget *mt_label = gtk_label_new(TR("挂载树"));
    gtk_widget_set_name(mt_label, "qy-disk-mount-title");
    gtk_label_set_xalign(GTK_LABEL(mt_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), mt_label, FALSE, FALSE, 0);

    mount_store = gtk_list_store_new(N_MCOLS, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    mount_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(mount_store));
    GtkCellRenderer *mr = gtk_cell_renderer_text_new();
    const char *mtitles[N_MCOLS] = { TR("挂载点"), TR("类型"), TR("容量"),
                                       TR("已用"), TR("可用") };
    for (int i = 0; i < N_MCOLS; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            mtitles[i], mr, "text", i, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_expand(col, i == M_TARGET);
        gtk_tree_view_append_column(GTK_TREE_VIEW(mount_tv), col);
    }
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(mount_tv), TRUE);
    g_signal_connect(mount_tv, "cursor-changed", G_CALLBACK(on_mount_cursor_changed), NULL);

    GtkWidget *msw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(msw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(msw, -1, 150);
    gtk_container_add(GTK_CONTAINER(msw), mount_tv);
    gtk_box_pack_start(GTK_BOX(vbox), msw, FALSE, FALSE, 0);

    fs_info_label = gtk_label_new("");
    gtk_widget_set_name(fs_info_label, "qydisk-fs-info");
    gtk_label_set_xalign(GTK_LABEL(fs_info_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(fs_info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), fs_info_label, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    gtk_widget_hide(err_lb);

    refresh_disk();
    refresh_mount_tree();

    /* 自动化: QYDISK_REFRESH=1 启动后延迟刷新一次 */
    if (g_getenv("QYDISK_REFRESH"))
        g_timeout_add(1000, auto_refresh, NULL);

    gtk_main();
    return 0;
}