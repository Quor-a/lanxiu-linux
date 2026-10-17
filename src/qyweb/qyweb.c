/* 澜岫 Linux 轻量浏览器 —— 极简 WebKitGTK 外壳
 *
 * 设计依据：用户 2026-10-08 决策"轻量浏览器"。
 * 依赖只有 WebKitGTK + GTK3，编译后二进制 < 50KB，
 * 不依赖任何 GNOME 桌面栈（不用 epiphany 的几十个依赖）。
 *
 * 功能：
 *   - 地址栏 + 前进/后退/刷新/主页
 *   - 标签页（Ctrl+T 新建，Ctrl+W 关闭）
 *   - 新窗口链接转为新标签
 *   - 下载交给 WebKit 默认处理
 *   - 支持命令行传入 URL：qyweb https://example.com
 *
 * 编译：
 *   gcc qyweb.c -o qyweb $(pkg-config --cflags --libs webkit2gtk-4.1 gtk+-3.0)
 */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdlib.h>
#include <string.h>

#define HOME_URL "https://www.wikipedia.org/"

static GtkWidget *notebook;

/* ---------- 标签页 ---------- */

static WebKitWebView *current_view(void)
{
    gint n = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (n < 0)
        return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), n);
    return WEBKIT_WEB_VIEW(page);
}

static void on_title_changed(WebKitWebView *view, GParamSpec *ps,
                             GtkWidget *label)
{
    const gchar *title = webkit_web_view_get_title(view);
    gtk_label_set_text(GTK_LABEL(label), title ? title : "新标签页");
}

/* 链接弹窗 → 新标签 */
static WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *a,
                                gpointer user_data);

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event,
                            gpointer user_data)
{
    GtkWidget *entry = GTK_WIDGET(user_data);
    if (event == WEBKIT_LOAD_COMMITTED ||
        event == WEBKIT_LOAD_FINISHED) {
        const gchar *uri = webkit_web_view_get_uri(view);
        if (uri && gtk_widget_get_visible(entry))
            gtk_entry_set_text(GTK_ENTRY(entry), uri);
    }
}

static GtkWidget *add_tab(const gchar *url, gboolean switch_to)
{
    WebKitWebView *view =
        WEBKIT_WEB_VIEW(webkit_web_view_new());
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);

    WebKitSettings *settings = webkit_web_view_get_settings(view);
    /* 轻量路线：默认开启 JS 与硬件加速，关闭不需要的合规/隐私探测 */
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_media_stream(settings, FALSE);
    webkit_settings_set_enable_webaudio(settings, FALSE);
    webkit_settings_set_enable_plugins(settings, FALSE);
    webkit_settings_set_user_agent(
        settings,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) LanxiuBrowser/1.0 Safari/605.1.15");

    gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(view));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new("新标签页");
    GtkWidget *close = gtk_button_new_from_icon_name(
        "window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);

    gint idx = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, box);
    gtk_widget_show_all(scrolled);

    g_signal_connect(view, "notify::title",
                     G_CALLBACK(on_title_changed), label);
    g_signal_connect(view, "create",
                     G_CALLBACK(on_create), NULL);
    g_signal_connect(close, "clicked",
                     G_CALLBACK(gtk_widget_destroy), scrolled);

    webkit_web_view_load_uri(view, url ? url : HOME_URL);

    if (switch_to)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), idx);
    return GTK_WIDGET(view);
}

static WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *a,
                                gpointer user_data)
{
    (void)view; (void)user_data;
    WebKitURIRequest *req = webkit_navigation_action_get_request(a);
    const gchar *uri = webkit_uri_request_get_uri(req);
    GtkWidget *nv = add_tab(uri, TRUE);
    return WEBKIT_WEB_VIEW(nv);
}

/* ---------- 工具栏 ---------- */

static void on_back(GtkButton *b, gpointer d)
{
    WebKitWebView *v = current_view();
    if (v) webkit_web_view_go_back(v);
}

static void on_forward(GtkButton *b, gpointer d)
{
    WebKitWebView *v = current_view();
    if (v) webkit_web_view_go_forward(v);
}

static void on_reload(GtkButton *b, gpointer d)
{
    WebKitWebView *v = current_view();
    if (v) webkit_web_view_reload(v);
}

static void on_home(GtkButton *b, gpointer d)
{
    WebKitWebView *v = current_view();
    if (v) webkit_web_view_load_uri(v, HOME_URL);
}

/* 地址栏回车：补全 scheme 后加载 */
static void navigate_from_entry(GtkEntry *entry, gpointer d)
{
    const gchar *text = gtk_entry_get_text(entry);
    if (!text || !*text)
        return;
    WebKitWebView *v = current_view();
    if (!v)
        return;
    if (g_str_has_prefix(text, "http://") || g_str_has_prefix(text, "https://")
        || g_str_has_prefix(text, "file://") || g_str_has_prefix(text, "about:"))
        webkit_web_view_load_uri(v, text);
    else if (strchr(text, ' ') == NULL && strchr(text, '.') != NULL) {
        gchar *url = g_strdup_printf("https://%s", text);
        webkit_web_view_load_uri(v, url);
        g_free(url);
    } else {
        gchar *q = g_uri_escape_string(text, NULL, TRUE);
        gchar *url = g_strdup_printf(
            "https://duckduckgo.com/?q=%s", q);
        webkit_web_view_load_uri(v, url);
        g_free(q);
        g_free(url);
    }
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_t ||
                                           ev->keyval == GDK_KEY_T)) {
        add_tab(HOME_URL, TRUE);
        return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_w ||
                                           ev->keyval == GDK_KEY_W)) {
        gint n = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
        if (n >= 0)
            gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), n);
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0)
            add_tab(HOME_URL, TRUE);
        return TRUE;
    }
    /* Ctrl+L 聚焦地址栏 */
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_l ||
                                           ev->keyval == GDK_KEY_L)) {
        GtkWidget *e = GTK_WIDGET(d);
        gtk_widget_grab_focus(e);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 750);
    gtk_window_set_title(GTK_WINDOW(win), "澜岫浏览器");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(tb), 4);

    GtkWidget *back = gtk_button_new_from_icon_name(
        "go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *fwd = gtk_button_new_from_icon_name(
        "go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *rel = gtk_button_new_from_icon_name(
        "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *hom = gtk_button_new_from_icon_name(
        "go-home-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *entry = gtk_entry_new();

    gtk_box_pack_start(GTK_BOX(tb), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), fwd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), rel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), hom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), entry, TRUE, TRUE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);

    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    g_signal_connect(back, "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(fwd, "clicked", G_CALLBACK(on_forward), NULL);
    g_signal_connect(rel, "clicked", G_CALLBACK(on_reload), NULL);
    g_signal_connect(hom, "clicked", G_CALLBACK(on_home), NULL);
    g_signal_connect(entry, "activate",
                     G_CALLBACK(navigate_from_entry), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), entry);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    const gchar *start = (argc > 1) ? argv[1] : HOME_URL;
    GtkWidget *view = add_tab(start, TRUE);
    g_signal_connect(view, "load-changed",
                     G_CALLBACK(on_load_changed), entry);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
