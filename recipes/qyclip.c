/* qyclip.c — 澜岫剪贴板管理器
 * 监听剪贴板 owner-change，保存文本历史，点击条目可复制回剪贴板。
 * 支持：固定条目（右键固定/取消固定，置顶显示橙色锁图标，不受 50 条上限清理影响）、
 *      顶部搜索框实时过滤（忽略大小写）、点击复制回剪贴板并显示状态。
 * 自动化: QYCLIP_AUTO=文本 启动后模拟复制该文本（用于截图/自测）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include "qytheme.h"
#include "qyl10n.h"

#define MAX_HIST 50

typedef struct {
    gchar *text;
    gboolean pinned;
} ClipItem;

static GtkWidget *listbox = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *search_entry = NULL;
static GPtrArray *history = NULL;
static gchar *search_query = NULL;

static void rebuild_list(void);

/* ---------- 数据 ---------- */
static void clip_item_free(gpointer p) {
    ClipItem *it = (ClipItem *)p;
    if (it) {
        g_free(it->text);
        g_free(it);
    }
}

/* 固定条目不受 50 条上限影响：仅清理最旧的未固定条目 */
static void trim_history(void) {
    int unpinned = 0;
    for (guint i = 0; i < history->len; i++) {
        ClipItem *it = (ClipItem *)g_ptr_array_index(history, i);
        if (!it->pinned) unpinned++;
    }
    while (unpinned > MAX_HIST) {
        gboolean removed = FALSE;
        for (guint i = 0; i < history->len; i++) {
            ClipItem *it = (ClipItem *)g_ptr_array_index(history, i);
            if (!it->pinned) {
                g_ptr_array_remove_index(history, i);
                unpinned--;
                removed = TRUE;
                break;
            }
        }
        if (!removed) break;
    }
}

/* 添加一条历史（去重，限制未固定条数，更新 UI） */
static void add_history(const char *text) {
    if (!text || !text[0]) return;
    if (history->len > 0) {
        ClipItem *last = (ClipItem *)g_ptr_array_index(history, history->len - 1);
        if (strcmp(last->text, text) == 0) return;
    }
    ClipItem *it = g_new0(ClipItem, 1);
    it->text = g_strdup(text);
    it->pinned = FALSE;
    g_ptr_array_add(history, it);
    trim_history();
    rebuild_list();
}

/* 忽略大小写的过滤匹配 */
static gboolean text_matches(const char *text, const char *query) {
    if (!query || !query[0]) return TRUE;
    gchar *t = g_ascii_strdown(text, -1);
    gchar *q = g_ascii_strdown(query, -1);
    gboolean m = (strstr(t, q) != NULL);
    g_free(t);
    g_free(q);
    return m;
}

/* ---------- 固定锁图标（自绘橙色锁，不用 emoji） ---------- */
static GdkPixbuf *lock_pixbuf_orange(int px) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    GdkRGBA orange = { 0.914, 0.329, 0.125, 1.0 };   /* #E95420 */
    qy_icon_draw(cr, QY_ICON_LOCK, px * 0.12, px * 0.12, px * 0.76, FALSE, &orange);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, px, px);
    cairo_surface_destroy(surf);
    return pb;
}

/* ---------- 列表重建 ---------- */
static void append_row(ClipItem *it) {
    GtkWidget *row = gtk_list_box_row_new();
    if (it->pinned) {
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *img = gtk_image_new_from_pixbuf(lock_pixbuf_orange(14));
        gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
        GtkWidget *lbl = gtk_label_new(it->text);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(h), lbl, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(row), h);
        gtk_widget_set_name(row, "qy-clip-pinned");
    } else {
        GtkWidget *lbl = gtk_label_new(it->text);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_container_add(GTK_CONTAINER(row), lbl);
    }
    g_object_set_data(G_OBJECT(row), "clip-item", it);
    gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
}

static void rebuild_list(void) {
    GList *ch = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    /* 固定条目置顶，未固定条目按新→旧排列；均受搜索过滤 */
    GPtrArray *pinned = g_ptr_array_new();
    GPtrArray *normal = g_ptr_array_new();
    for (int i = (int)history->len - 1; i >= 0; i--) {
        ClipItem *it = (ClipItem *)g_ptr_array_index(history, i);
        if (!text_matches(it->text, search_query)) continue;
        if (it->pinned)
            g_ptr_array_add(pinned, it);
        else
            g_ptr_array_add(normal, it);
    }
    for (guint i = 0; i < pinned->len; i++)
        append_row((ClipItem *)g_ptr_array_index(pinned, i));
    for (guint i = 0; i < normal->len; i++)
        append_row((ClipItem *)g_ptr_array_index(normal, i));
    g_ptr_array_free(pinned, TRUE);
    g_ptr_array_free(normal, TRUE);

    char st[256];
    if (history->len == 0)
        gtk_label_set_text(GTK_LABEL(status_label), TR("暂无历史"));
    else {
        g_snprintf(st, sizeof st, "%s: %d", TR("剪贴板历史"), (int)history->len);
        gtk_label_set_text(GTK_LABEL(status_label), st);
    }
    gtk_widget_show_all(listbox);
}

/* ---------- 信号 ---------- */
static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    if (!row) return;
    ClipItem *it = (ClipItem *)g_object_get_data(G_OBJECT(row), "clip-item");
    if (!it) return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), it->text, -1);
    char st[512];
    g_snprintf(st, sizeof st, "%s: %s", TR("已复制"), it->text);
    gtk_label_set_text(GTK_LABEL(status_label), st);
}

static void on_toggle_pin(GtkWidget *mi, gpointer ud) {
    (void)mi;
    ClipItem *it = (ClipItem *)ud;
    it->pinned = !it->pinned;
    rebuild_list();
}

/* 右键菜单：固定 / 取消固定 */
static gboolean on_list_button_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != GDK_BUTTON_SECONDARY) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(listbox), ev->y);
    if (!row) return FALSE;
    ClipItem *it = (ClipItem *)g_object_get_data(G_OBJECT(row), "clip-item");
    if (!it) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(it->pinned ? TR("取消固定") : TR("固定"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_toggle_pin), it);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
    return TRUE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer ud) {
    (void)ud;
    g_free(search_query);
    search_query = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    rebuild_list();
}

/* 清空历史（含固定条目，显式操作） */
static void on_clear_history(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (history) g_ptr_array_set_size(history, 0);
    gtk_label_set_text(GTK_LABEL(status_label), TR("暂无历史"));
    rebuild_list();
}

/* ---------- 剪贴板监听 ---------- */
static void got_text(GtkClipboard *clip, const char *text, gpointer data) {
    (void)clip; (void)data;
    if (text && text[0])
        add_history(text);
}

static void on_owner_change(GtkClipboard *clip, GdkEvent *event, gpointer data) {
    (void)clip; (void)event; (void)data;
    gtk_clipboard_request_text(clip, got_text, NULL);
}

/* 自动化: 设置剪贴板文本 */
static gboolean auto_read_clip(gpointer p);   /* 前向声明 */
static gboolean auto_set_clip(gpointer p) {
    const char *text = (const char *)p;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
    g_timeout_add(600, auto_read_clip, NULL);
    g_free(p);
    return G_SOURCE_REMOVE;
}
static gboolean auto_read_clip(gpointer p) {
    (void)p;
    gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), got_text, NULL);
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer ud) {
    (void)ud;
    qy_load_theme();
    history = g_ptr_array_new_with_free_func(clip_item_free);

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("剪贴板"));
    gtk_widget_set_name(GTK_WIDGET(win), "qy-clip-win");
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 420);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶栏：搜索框 + 清空 */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索历史..."));
    gtk_widget_set_hexpand(search_entry, TRUE);
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(top), search_entry, TRUE, TRUE, 0);
    GtkWidget *b_clear = gtk_button_new_with_label(TR("清空历史"));
    g_signal_connect(b_clear, "clicked", G_CALLBACK(on_clear_history), NULL);
    gtk_box_pack_start(GTK_BOX(top), b_clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    listbox = gtk_list_box_new();
    gtk_widget_set_name(GTK_WIDGET(listbox), "qy-clip-list");
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), NULL);
    g_signal_connect(listbox, "button-press-event", G_CALLBACK(on_list_button_press), NULL);
    gtk_container_add(GTK_CONTAINER(sw), listbox);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    status_label = gtk_label_new(TR("暂无历史"));
    qy_add_class(status_label, "qy-mon-info");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    /* 监听剪贴板变化 */
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    g_signal_connect(clip, "owner-change", G_CALLBACK(on_owner_change), NULL);

    const char *auto_text = g_getenv("QYCLIP_AUTO");
    if (auto_text)
        g_timeout_add(800, auto_set_clip, g_strdup(auto_text));

    gtk_widget_show_all(win);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.clip", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}