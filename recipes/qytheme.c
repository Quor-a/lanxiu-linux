/* qytheme.c — 澜岫统一主题助手实现
 * 加载 /usr/share/themes/lanxiu/gtk-3.0/gtk.css（深色桌面主题），
 * 并读取 /etc/qytheme.conf 的 accent= 加载强调色覆盖 CSS。
 * 提供便捷 CSS 类添加函数。
 */
#include "qyicon.h"
#include "qytheme.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>

void qy_load_theme(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    if (gtk_css_provider_load_from_path(p, "/usr/share/themes/lanxiu/gtk-3.0/gtk.css", NULL)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    g_object_unref(p);

    /* 强调色: /etc/qytheme.conf accent=orange|blue|green（禁紫，锁橙默认） */
    char accent[32] = "orange";
    gchar *content = NULL;
    if (g_file_get_contents("/etc/qytheme.conf", &content, NULL, NULL)) {
        char *line = content;
        while (line && *line) {
            if (strncmp(line, "accent=", 7) == 0) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                if (line[7])
                    g_strlcpy(accent, line + 7, sizeof accent);
                break;
            }
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
        g_free(content);
    }
    const char *color = "#E95420";  /* orange */
    /* 仅保留锁橙强调色；blue/green 分支已按审计移除（禁多色系） */

    /* 覆盖 CSS: 主按钮 / 标题 / 进度条 */
    gchar *css = g_strdup_printf(
        ".qy-btn { background-image: none; background-color: %s; border-color: %s; color: #ffffff; }\n"
        ".qy-btn:hover { background-image: none; background-color: %s; }\n"
        ".qy-mon-cpu { background-color: %s; border-color: %s; color: #ffffff; }\n"
        ".qy-mon-title { color: %s; }\n"
        "progressbar trough { background-color: #222a38; }\n"
        "progressbar progress { background-color: %s; }\n",
        color, color, color, color, color, color, color);
    GtkCssProvider *p2 = gtk_css_provider_new();
    if (gtk_css_provider_load_from_data(p2, css, -1, NULL)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(p2),
            GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    }
    g_object_unref(p2);
    g_free(css);
}

void qy_add_class(GtkWidget *w, const char *cls) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* 可拖动标题栏：GTK HeaderBar 作为 CSD 标题栏。
 * HeaderBar 自带 press/拖动处理（begin_move_drag），
 * 在没有服务端窗口装饰的合成器（如 weston）上也能按住拖动窗口。 */
GtkWidget *qy_make_titlebar(GtkWindow *win, const char *title) {
    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    if (title && title[0])
        gtk_header_bar_set_title(GTK_HEADER_BAR(hb), title);
    gtk_window_set_titlebar(win, hb);return hb;
}

/* 图标按钮：无边框，内嵌 qy_icon_pixbuf 线稿图标，可选 tooltip 与 CSS 类。
 * cls 为空时使用主题默认类 qy-icon-btn（见 qytheme.css）。 */
GtkWidget *qy_icon_button(QyIconId id, int px, const char *tip, const char *cls) {
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    qy_add_class(btn, cls ? cls : "qy-icon-btn");
    gtk_container_add(GTK_CONTAINER(btn),
                      gtk_image_new_from_pixbuf(qy_icon_pixbuf(id, px, NULL)));
    if (tip && tip[0])
        gtk_widget_set_tooltip_text(btn, tip);
    return btn;
}

/* 窗口初始化：默认尺寸 + 最小尺寸，center 为 TRUE 时窗口居中显示。 */
/* 窗口大小记忆: ~/.config/lanxiu/win-size.conf 按应用名保存（release 281） */
static void win_size_save(GtkWidget *w, gpointer ud) {
    gchar *cfg = (gchar *)ud;
    int sw = 0, sh = 0;
    gtk_window_get_size(GTK_WINDOW(w), &sw, &sh);
    const char *app = g_get_prgname();
    if (cfg && app && app[0]) {
        GKeyFile *kf = g_key_file_new();
        g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, NULL);
        g_key_file_set_integer(kf, app, "w", sw);
        g_key_file_set_integer(kf, app, "h", sh);
        gsize len = 0;
        gchar *data = g_key_file_to_data(kf, &len, NULL);
        g_file_set_contents(cfg, data, len, NULL);
        g_free(data);
        g_key_file_free(kf);
    }
    g_free(cfg);
}

void qy_window_setup(GtkWindow *win, int w, int h, int min_w, int min_h, gboolean center) {
    gtk_window_set_default_size(win, w, h);
    gtk_widget_set_size_request(GTK_WIDGET(win), min_w, min_h);
    if (center)
        gtk_window_set_position(win, GTK_WIN_POS_CENTER);
    /* 记忆窗口大小（release 281） */
    const char *app = g_get_prgname();
    if (app && app[0]) {
        gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
        g_mkdir_with_parents(dir, 0755);
        gchar *cfg = g_build_filename(dir, "win-size.conf", NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, NULL)) {
            int sw = g_key_file_get_integer(kf, app, "w", NULL);
            int sh = g_key_file_get_integer(kf, app, "h", NULL);
            if (sw >= min_w && sh >= min_h)
                gtk_window_resize(win, sw, sh);
        }
        g_key_file_free(kf);
        g_signal_connect(win, "destroy", G_CALLBACK(win_size_save), g_strdup(cfg));
        g_free(cfg);
        g_free(dir);
    }
}