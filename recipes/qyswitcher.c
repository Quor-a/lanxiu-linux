/* qyswitcher.c — 澜岫窗口切换器（Alt-Tab）
 * 弹出式窗口列表（X11 枚举），每项两行显示 应用名 + 窗口标题，点击激活窗口。
 * 选中项显示窗口缩略图预览（X11 尝试 import 抓图；Wayland 显示「预览不可用」）。
 * 自动化: QYSWITCH_AUTO=1 启动后自动枚举；QYSWITCH_DISPLAY 指定 DISPLAY（默认 :0）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"

static GtkWidget *listbox = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *count_label = NULL;
static GtkWidget *win = NULL;
static GtkWidget *preview_popover = NULL;
static GtkWidget *preview_image = NULL;
static GtkWidget *preview_label = NULL;

/* 行数据 */
typedef struct {
    gchar *id;       /* X11 窗口 id（NULL=运行中应用行） */
    gchar *app;
    gchar *title;
    gboolean is_app; /* 运行中应用行 */
} SwRow;

static void sw_row_free(gpointer p) {
    SwRow *r = (SwRow *)p;
    g_free(r->id);
    g_free(r->app);
    g_free(r->title);
    g_free(r);
}

/* 枚举 X 窗口 */
static char **fetch_windows(int *out_n) {
    const char *dpy = g_getenv("QYSWITCH_DISPLAY");
    if (!dpy) dpy = ":0";
    gchar *cmd = g_strdup_printf("DISPLAY=%s /usr/bin/qysw-x11", dpy);
    gchar *out = NULL;
    if (!g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL) || !out) {
        g_free(cmd);
        g_free(out);
        if (out_n) *out_n = 0;
        return NULL;
    }
    g_free(cmd);
    gchar **lines = g_strsplit(out, "\n", 0);
    g_free(out);
    int n = 0;
    while (lines && lines[n] && lines[n][0]) n++;
    if (out_n) *out_n = n;
    return lines;
}

static void clear_rows(void) {
    gtk_container_foreach(GTK_CONTAINER(listbox), (GtkCallback)gtk_widget_destroy, NULL);
}

/* 已知澜岫应用（用于检测运行中） */
static const char *known_apps[] = {
    "qyfiles", "qyedit", "qyterm", "qysettings", "qymon", "qynet",
    "qystore", "qybrowser", "qyshot", "qyclip", "qylock", "qysearch",
    "qymedia", "qyswitcher", "qyview", "qyarc", "qyctl", "qywallpaper",
    "qydesktop", "qyappmenu", "qyusers", "qysudo",
};
#define NKNOWN ((int)(sizeof known_apps / sizeof known_apps[0]))

/* 两行行：应用名（上）+ 窗口标题（下） */
static GtkWidget *make_row(const char *id, const char *app, const char *title, gboolean is_app) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_set_border_width(GTK_CONTAINER(v), 4);
    gtk_container_add(GTK_CONTAINER(row), v);
    GtkWidget *app_lbl = gtk_label_new(app && app[0] ? app : "?");
    gtk_label_set_xalign(GTK_LABEL(app_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(v), app_lbl, FALSE, FALSE, 0);
    GtkWidget *title_lbl = gtk_label_new(title && title[0] ? title : (is_app ? TR("已运行") : ""));
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_opacity(title_lbl, 0.7);
    gtk_box_pack_start(GTK_BOX(v), title_lbl, FALSE, FALSE, 0);
    SwRow *sr = g_new0(SwRow, 1);
    sr->id = g_strdup(id);
    sr->app = g_strdup(app ? app : "");
    sr->title = g_strdup(title ? title : "");
    sr->is_app = is_app;
    g_object_set_data_full(G_OBJECT(row), "sw-row", sr, sw_row_free);
    return row;
}

/* 运行中澜岫应用 */
static void add_running_apps(void) {
    gchar *out = NULL;
    if (!g_spawn_command_line_sync("pgrep -a -f /usr/bin/qy", &out, NULL, NULL, NULL) || !out) {
        g_free(out);
        return;
    }
    gchar **lines = g_strsplit(out, "\n", 0);
    g_free(out);
    for (int i = 0; lines && lines[i]; i++) {
        if (!lines[i][0]) continue;
        /* 行: pid /usr/bin/name ... */
        char *name = strrchr(lines[i], '/');
        if (!name) continue;
        name++;
        char *space = strchr(name, ' ');
        if (space) *space = 0;
        for (int k = 0; k < NKNOWN; k++) {
            if (strcmp(name, known_apps[k]) == 0) {
                GtkWidget *row = make_row(NULL, name, TR("已运行"), TRUE);
                gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
                break;
            }
        }
    }
    g_strfreev(lines);
}

/* 进程存活检查（/proc/comm，不依赖 pidof） */
static gboolean proc_alive(const char *name) {
    GDir *dir = g_dir_open("/proc", 0, NULL);
    if (!dir) return FALSE;
    const char *ent;
    gboolean found = FALSE;
    while ((ent = g_dir_read_name(dir)) != NULL) {
        if (!g_ascii_isdigit(ent[0])) continue;
        gchar *cp = g_strdup_printf("/proc/%s/comm", ent);
        gchar *comm = NULL;
        if (g_file_get_contents(cp, &comm, NULL, NULL) && comm) {
            g_strstrip(comm);
            if (strcmp(comm, name) == 0) { found = TRUE; g_free(comm); g_free(cp); break; }
        }
        g_free(comm);
        g_free(cp);
    }
    g_dir_close(dir);
    return found;
}

/* 激活 X11 窗口 */
static void activate_window(const char *id) {
    const char *dpy = g_getenv("QYSWITCH_DISPLAY");
    if (!dpy) dpy = ":0";
    gchar *cmd = g_strdup_printf("DISPLAY=%s /usr/bin/qysw-x11 -a %s", dpy, id);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

/* 点击/回车行：X11 窗口则激活；运行中应用则启动 */
static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    SwRow *sr = row ? (SwRow *)g_object_get_data(G_OBJECT(row), "sw-row") : NULL;
    if (!sr) return;
    if (sr->id && sr->id[0]) {
        activate_window(sr->id);
        return;
    }
    if (sr->is_app && sr->app && sr->app[0]) {
        if (proc_alive(sr->app)) {
            gchar *msg = g_strdup_printf("%s %s", sr->app, TR("已在运行"));
            gtk_label_set_text(GTK_LABEL(status_label), msg);
            g_free(msg);
            return;
        }
        gchar *cmd = g_strdup_printf("%s &", sr->app);
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
    }
}

/* 选中行：更新预览 popover + 窗口 N/M 计数 */
static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)ud;
    /* 预览 */
    if (preview_popover) {
        SwRow *sr = row ? (SwRow *)g_object_get_data(G_OBJECT(row), "sw-row") : NULL;
        gtk_image_clear(GTK_IMAGE(preview_image));
        if (sr && sr->id && sr->id[0] && !g_getenv("WAYLAND_DISPLAY")) {
            /* X11: 尝试 import 抓窗口缩略图 */
            const char *dpy = g_getenv("QYSWITCH_DISPLAY");
            if (!dpy) dpy = ":0";
            gchar *png = g_strdup("/tmp/qysw-preview.png");
            gchar *cmd = g_strdup_printf("DISPLAY=%s import -window %s %s 2>/dev/null",
                                         dpy, sr->id, png);
            gchar *out = NULL;
            gboolean ok = FALSE;
            if (g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL))
                ok = g_file_test(png, G_FILE_TEST_EXISTS);
            g_free(cmd); g_free(out);
            GdkPixbuf *pb = ok ? gdk_pixbuf_new_from_file_at_scale(png, 320, 200, TRUE, NULL) : NULL;
            if (pb) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(preview_image), pb);
                g_object_unref(pb);
                gtk_widget_show(preview_image);
                gtk_widget_hide(preview_label);
            } else {
                gtk_label_set_text(GTK_LABEL(preview_label), TR("预览不可用"));
                gtk_widget_show(preview_label);
                gtk_widget_hide(preview_image);
            }
            g_unlink(png);
            g_free(png);
        } else {
            gtk_label_set_text(GTK_LABEL(preview_label), TR("预览不可用"));
            gtk_widget_show(preview_label);
            gtk_widget_hide(preview_image);
        }
        if (row) gtk_popover_popup(GTK_POPOVER(preview_popover));
    }
    /* 窗口 N/M */
    if (count_label && row) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(box));
        int total = g_list_length(children);
        int idx = 0;
        for (GList *l = children; l; l = l->next) {
            idx++;
            if (l->data == row) break;
        }
        g_list_free(children);
        if (idx >= 1) {
            char buf[64];
            g_snprintf(buf, sizeof buf, "%s %d/%d", TR("窗口"), idx, total);
            gtk_label_set_text(GTK_LABEL(count_label), buf);
        }
    }
}

/* 填充窗口列表 */
static void populate(void) {
    clear_rows();
    int n = 0;
    gchar **lines = fetch_windows(&n);
    if (!n) {
        /* Wayland 会话没有 X11 窗口枚举 → 退化为运行中应用列表 */
        if (g_getenv("WAYLAND_DISPLAY"))
            gtk_label_set_text(GTK_LABEL(status_label), TR("Wayland 模式：运行中应用"));
        else
            gtk_label_set_text(GTK_LABEL(status_label), TR("未找到窗口"));
        add_running_apps();
        gtk_widget_show_all(listbox);
        return;
    }
    for (int i = 0; i < n; i++) {
        char *line = lines[i];
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = 0;
        char *id = line;
        char *p2 = strchr(p1 + 1, '|');
        const char *app = "X11";
        const char *title = p1 + 1;
        if (p2) {
            *p2 = 0;
            app = p1 + 1;
            title = p2 + 1;
        }
        GtkWidget *row = make_row(id, app, title, FALSE);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    }
    g_strfreev(lines);
    /* 运行中澜岫应用 */
    add_running_apps();
    char st[128];
    g_snprintf(st, sizeof st, "%s %d · %s", TR("窗口"), n, TR("已运行应用"));
    gtk_label_set_text(GTK_LABEL(status_label), st);
    gtk_widget_show_all(listbox);
}

static void on_refresh(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    populate();
}

static gboolean auto_sw(gpointer p) {
    (void)p;
    populate();
    const char *wld = g_getenv("WAYLAND_DISPLAY");
    g_printerr("QYSWITCHDBG: mode=%s wayland=%s\n",
               wld ? "wayland" : "x11", wld ? wld : "none");
    return G_SOURCE_REMOVE;
}

static void build_ui(void) {
    qy_load_theme();
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("窗口切换器"));
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 360);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl = gtk_label_new("Tab");
    GtkWidget *b_refresh = gtk_button_new_with_label(TR("刷新"));
    g_signal_connect(b_refresh, "clicked", G_CALLBACK(on_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), b_refresh, FALSE, FALSE, 0);
    count_label = gtk_label_new("");
    gtk_widget_set_halign(count_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand(count_label, TRUE);
    gtk_box_pack_end(GTK_BOX(row), count_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(listbox), TRUE);
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), NULL);
    g_signal_connect(listbox, "row-selected", G_CALLBACK(on_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(sw), listbox);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), sw, TRUE, TRUE, 0);

    /* 预览 popover（附着在列表右侧，选中时弹出） */
    preview_popover = gtk_popover_new(sw);
    gtk_popover_set_position(GTK_POPOVER(preview_popover), GTK_POS_RIGHT);
    GtkWidget *pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(pv), 6);
    gtk_container_add(GTK_CONTAINER(preview_popover), pv);
    preview_image = gtk_image_new();
    gtk_widget_set_size_request(preview_image, 320, 200);
    gtk_box_pack_start(GTK_BOX(pv), preview_image, TRUE, TRUE, 0);
    preview_label = gtk_label_new(TR("预览不可用"));
    gtk_widget_set_opacity(preview_label, 0.6);
    gtk_box_pack_start(GTK_BOX(pv), preview_label, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    status_label = gtk_label_new("");
    qy_add_class(status_label, "qy-mon-info");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    if (g_getenv("QYSWITCH_AUTO"))
        g_timeout_add(600, auto_sw, NULL);
    else
        populate();
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    build_ui();
    gtk_main();
    return 0;
}