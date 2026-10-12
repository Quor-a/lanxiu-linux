/* qysearch.c — 澜岫全局搜索
 * 顶部弹窗式搜索：应用 + /usr/bin 程序 实时匹配，点击运行。
 * 自动化: QYSEARCH_TERM=关键词 启动后自动搜索（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <dirent.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

typedef struct {
    const char *name;
    const char *cmd;
    QyIconId    icon;
} SearchApp;

static const SearchApp apps[] = {
    { "文件管理",  "qyfiles",     QY_ICON_FILES     },
    { "文本编辑",  "qyedit",      QY_ICON_EDIT      },
    { "终端",      "weston-terminal", QY_ICON_TERM  },
    { "设置中心",  "qysettings",  QY_ICON_SETTINGS  },
    { "系统监视",  "qymon",       QY_ICON_MONITOR   },
    { "网络管理",  "qynet",       QY_ICON_NETWORK   },
    { "应用商店",  "qystore",     QY_ICON_STORE     },
    { "浏览器",    "qybrowser",   QY_ICON_BROWSER   },
    { "截图工具",  "qyshot",      QY_ICON_SHOT      },
    { "剪贴板",    "qyclip",      QY_ICON_CLIPBOARD },
    { "锁屏",      "qylock",      QY_ICON_LOCK      },
    { "系统安装",  "qysetup",     QY_ICON_SETUP     },
    { "取色器",    "qycolor",     QY_ICON_COLOR     },
};
#define NAPPS ((int)(sizeof apps / sizeof apps[0]))

static GtkWidget *listbox = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *search_entry = NULL;

static void on_result_clicked(GtkWidget *w, gpointer ud);
static gboolean on_row_button_press(GtkWidget *row, GdkEventButton *ev, gpointer ud);

/* 结果行：cmd 为点击执行命令；path 为完整文件路径（非文件结果传 NULL，
 * 供右键菜单「复制路径 / 打开所在目录」使用）。 */
static void add_result_row(QyIconId icon, const char *text, const char *cmd, const char *path) {
    GtkWidget *row = gtk_button_new();
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 8);
    gtk_widget_set_margin_end(hbox, 8);
    gtk_widget_set_margin_top(hbox, 4);
    gtk_widget_set_margin_bottom(hbox, 4);
    GdkPixbuf *pb = qy_icon_pixbuf(icon, 16, NULL);
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    if (pb) g_object_unref(pb);
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), hbox);
    gtk_widget_set_halign(row, GTK_ALIGN_FILL);
    gtk_widget_set_tooltip_text(row, cmd);
    g_signal_connect(row, "clicked", G_CALLBACK(on_result_clicked), g_strdup(cmd));
    /* 右键菜单：复制路径 / 打开所在目录 */
    g_signal_connect(row, "button-press-event", G_CALLBACK(on_row_button_press), NULL);
    g_object_set_data_full(G_OBJECT(row), "result-cmd", g_strdup(cmd), g_free);
    if (path)
        g_object_set_data_full(G_OBJECT(row), "result-path", g_strdup(path), g_free);
    gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
}

static void on_result_clicked(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *cmd = (const char *)ud;
    gchar *s = g_strdup_printf("%s &", cmd);
    g_spawn_command_line_async(s, NULL);
    g_free(s);
    g_free(ud);
}

/* ---------- 结果列表右键菜单：复制路径 / 打开所在目录（qyfiles） ---------- */
static void on_copy_path(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *path = (const char *)ud;
    if (path && path[0]) {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, path, -1);
        g_printerr("QYSEARCHDBG: copied path %s\n", path);
    }
    g_free(ud);
}

static void on_open_dir(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *path = (const char *)ud;
    if (path && path[0]) {
        gchar *dir = g_path_get_dirname(path);
        gchar *cmd = g_strdup_printf("qyfiles %s", dir);
        g_spawn_command_line_async(cmd, NULL);
        g_printerr("QYSEARCHDBG: open dir %s\n", dir);
        g_free(cmd);
        g_free(dir);
    }
    g_free(ud);
}

static gboolean on_row_button_press(GtkWidget *row, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS) {
        const char *path = g_object_get_data(G_OBJECT(row), "result-path");
        if (!path) return FALSE;   /* 非文件结果不显示路径菜单 */
        GtkWidget *menu = gtk_menu_new();
        GtkWidget *mi = gtk_menu_item_new_with_label(TR("复制路径"));
        g_signal_connect(mi, "activate", G_CALLBACK(on_copy_path), g_strdup(path));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        mi = gtk_menu_item_new_with_label(TR("打开所在目录"));
        g_signal_connect(mi, "activate", G_CALLBACK(on_open_dir), g_strdup(path));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
        return TRUE;
    }
    return FALSE;
}

/* 清空结果 */
static void clear_results(void) {
    gtk_container_foreach(GTK_CONTAINER(listbox), (GtkCallback)gtk_widget_destroy, NULL);
}

/* 执行搜索 */
static void do_search(const char *q) {
    clear_results();
    if (!q || !q[0]) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("输入关键词搜索应用或程序"));
        return;
    }
    int n = 0;
    /* 搜索内置应用 */
    for (int i = 0; i < NAPPS; i++) {
        const char *name = TR(apps[i].name);
        if (strcasestr(name, q) || strcasestr(apps[i].cmd, q)) {
            add_result_row(apps[i].icon, name, apps[i].cmd, NULL);
            n++;
        }
    }
    /* 搜索 /usr/bin 程序（文件名匹配，最多 25 个） */
    if (n < 40) {
        DIR *d = opendir("/usr/bin");
        if (d) {
            struct dirent *e;
            int fn = 0;
            while ((e = readdir(d)) != NULL && fn < 25) {
                if (e->d_name[0] == '.') continue;
                if (strcasestr(e->d_name, q)) {
                    gchar *path = g_strdup_printf("/usr/bin/%s", e->d_name);
                    add_result_row(QY_ICON_TERM, e->d_name, e->d_name, path);
                    g_free(path);
                    n++; fn++;
                }
            }
            closedir(d);
        }
    }
    /* 搜索常见目录中的文件（最多 15 个，点击用 qyfiles 打开所在目录） */
    if (n < 40) {
        const char *dirs[] = { "/home/user/Desktop", "/home/user/Documents",
                               "/etc/qy" };
        int fn = 0;
        for (size_t di = 0; di < G_N_ELEMENTS(dirs); di++) {
            DIR *d = opendir(dirs[di]);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != NULL && fn < 15) {
                if (e->d_name[0] == '.') continue;
                if (strcasestr(e->d_name, q)) {
                    gchar *cmd = g_strdup_printf("qyfiles %s", dirs[di]);
                    gchar *path = g_strdup_printf("%s/%s", dirs[di], e->d_name);
                    add_result_row(QY_ICON_FILE, e->d_name, cmd, path);
                    g_free(path);
                    g_free(cmd);
                    n++; fn++;
                }
            }
            closedir(d);
        }
    }
    char st[128];
    if (n == 0)
        g_snprintf(st, sizeof st, "%s: %s", TR("未找到匹配"), q);
    else
        g_snprintf(st, sizeof st, "%s %d", TR("搜索结果"), n);
    gtk_label_set_text(GTK_LABEL(status_label), st);
    gtk_widget_show_all(listbox);   /* 窗口已显示后插入的行需显式显示 */
    g_printerr("QYSEARCHDBG: results=%d term=%s\n", n, q ? q : "");
}

static void on_search_changed(GtkEditable *e, gpointer ud) {
    (void)e; (void)ud;
    const char *q = gtk_entry_get_text(GTK_ENTRY(search_entry));
    do_search(q);
}

static void on_close(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    gtk_main_quit();
}

static void activate(GtkApplication *app, gpointer ud) {
    (void)ud;
    qy_load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("全局搜索"));
    qy_window_setup(GTK_WINDOW(win), 560, 420, 480, 360, TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl = gtk_image_new_from_pixbuf(qy_icon_pixbuf(QY_ICON_SEARCH, 16, NULL));
    search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索应用或程序..."));
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    GtkWidget *b_close = qy_icon_button(QY_ICON_CLOSE, 16, TR("关闭"), NULL);
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close), NULL);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), b_close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(sw), listbox);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    status_label = gtk_label_new(TR("输入关键词搜索应用或程序"));
    qy_add_class(status_label, "qy-mon-info");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    /* 自动化: QYSEARCH_TERM */
    const char *term = g_getenv("QYSEARCH_TERM");
    if (term) {
        gtk_entry_set_text(GTK_ENTRY(search_entry), term);
        do_search(term);
    }
    gtk_widget_grab_focus(search_entry);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.search", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}