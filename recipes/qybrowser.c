/* qybrowser.c — 澜岫简易浏览器
 * libcurl 下载网页 → 提取纯文本 → GtkTextView 渲染
 * 适合轻量桌面（无 WebKit）：快速查看网页文字内容。
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GtkWidget *url_entry = NULL;
static GtkWidget *view      = NULL;
static GtkWidget *status_label = NULL;
static GtkTextBuffer *buffer = NULL;
static GtkWidget *back_btn = NULL, *fwd_btn = NULL;   /* 前进/后退 */
static GtkWidget *dl_spinner = NULL;   /* 下载中加载动画（release 256） */

/* 下载管理（release: 下载历史面板） */
static GPtrArray *dl_entries = NULL;      /* DownloadEntry 列表 */
static GtkWidget *dl_revealer = NULL;    /* 底部下载历史面板 */
static GtkWidget *dl_listbox = NULL;      /* 下载历史 GtkListBox */
static GtkWidget *dl_url_entry = NULL;    /* 面板内下载地址输入 */
static GtkWidget *dl_toggle_btn = NULL;   /* 工具栏「下载」切换按钮 */

/* 浏览历史栈 */
static GPtrArray *hist = NULL;
static int hist_pos = -1;
static gboolean hist_nav = FALSE;   /* 历史导航时不重复入栈 */
static GtkWidget *hist_btn = NULL;      /* 工具栏「历史」按钮（release 264） */
static GtkWidget *hist_popover = NULL;  /* 历史弹出面板 */
static GtkWidget *hist_listbox = NULL;  /* 历史 GtkListBox */

static gboolean finish_fetch(gpointer ud);   /* 前向声明 */
static void on_link_clicked(GtkWidget *w, gpointer ud);   /* 前向声明 */
static void open_url(const char *url);   /* 前向声明 */

/* ---------- libcurl 写回调 ---------- */
struct curl_buf { char *data; size_t len; };
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    struct curl_buf *b = (struct curl_buf *)ud;
    size_t n = size * nmemb;
    char *p = (char *)realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

/* ---------- HTML → 纯文本 ---------- */
static void append_entity(GString *out, const char **pp) {
    const char *p = *pp;
    if      (!strncmp(p, "&amp;", 5))  g_string_append_c(out, '&');
    else if (!strncmp(p, "&lt;", 4))   g_string_append_c(out, '<');
    else if (!strncmp(p, "&gt;", 4))   g_string_append_c(out, '>');
    else if (!strncmp(p, "&quot;", 6)) g_string_append_c(out, '"');
    else if (!strncmp(p, "&nbsp;", 6)) g_string_append_c(out, ' ');
    else if (!strncmp(p, "&#", 3)) {
        const char *e = strchr(p + 2, ';');
        if (e) {
            char num[16];
            size_t L = e - (p + 2);
            if (L < sizeof num) {
                memcpy(num, p + 2, L); num[L] = 0;
                int code = atoi(num);
                if (code > 0 && code < 0x110000) g_string_append_unichar(out, code);
                *pp = e + 1;
                return;
            }
        }
    }
    *pp += 2;
}

static char *html_to_text(const char *html) {
    GString *out = g_string_new(NULL);
    const char *p = html;
    int in_tag = 0, in_skip = 0;
    while (*p) {
        if (in_skip) {
            const char *cl = strstr(p, in_skip == 1 ? "</script" : "</style");
            if (cl) { p = cl + 8; in_skip = 0; }
            else break;
        }
        if (in_tag) {
            const char *e = strchr(p, '>');
            if (!e) break;
            if (g_ascii_strncasecmp(p, "script", 6) == 0) in_skip = 1;
            else if (g_ascii_strncasecmp(p, "style", 5) == 0) in_skip = 2;
            p = e + 1;
            in_tag = 0;
            continue;
        }
        char c = *p;
        if (c == '<') { in_tag = 1; p++; continue; }
        if (c == '&') { append_entity(out, &p); continue; }
        if (c == '\n') g_string_append_c(out, '\n');
        else if (c == '\r') { /* skip */ }
        else if (c >= 0x20) g_string_append_c(out, c);
        p++;
    }
    /* 压缩 3+ 空行为 1 行 */
    char *txt = g_string_free(out, FALSE);
    GString *clean = g_string_new(NULL);
    int blanks = 0;
    for (char *q = txt; *q; q++) {
        if (*q == '\n') { blanks++; if (blanks <= 2) g_string_append_c(clean, '\n'); }
        else { blanks = 0; g_string_append_c(clean, *q); }
    }
    g_free(txt);
    return g_string_free(clean, FALSE);
}

/* 提取 HTML 中的链接：返回 "url | 文本" 数组（手动扫描，避免 GRegex match_all 在旧 GLib 的 bug） */
static char **extract_links(const char *html, int *n_out) {
    *n_out = 0;
    if (!html) return NULL;
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    GRegex *tag_re = g_regex_new("<[^>]+>", 0, 0, NULL);
    const char *p = html;
    while ((p = strstr(p, "href")) != NULL) {
        /* 向前确认这是 <a ... href=...> */
        const char *before = p;
        while (before > html && before[-1] != '<' && before[-1] != '>')
            before--;
        if (before > html && before[-1] == '<' && g_ascii_strncasecmp(before, "a", 1) == 0) {
            const char *q = p + 4;
            while (*q == ' ' || *q == '\t' || *q == '=')
                q++;
            if (*q == '"' || *q == '\'') {
                char quote = *q++;
                const char *url_start = q;
                const char *url_end = strchr(q, quote);
                if (url_end) {
                    gchar *url = g_strndup(url_start, (gsize)(url_end - url_start));
                    const char *tag_end = strchr(url_end, '>');
                    const char *close = tag_end ? strstr(tag_end, "</a>") : NULL;
                    gchar *raw = NULL;
                    if (tag_end && close && close > tag_end + 1)
                        raw = g_strndup(tag_end + 1, (gsize)(close - (tag_end + 1)));
                    gchar *clean = NULL;
                    if (tag_re)
                        clean = g_regex_replace(tag_re, raw ? raw : "", -1, 0, "", 0, NULL);
                    gchar *effective = clean ? clean : g_strdup(raw ? raw : "");
                    gchar *s = g_strstrip(effective);
                    if (url[0] && s && s[0])
                        g_ptr_array_add(arr, g_strdup_printf("%s | %s", url, s));
                    g_free(effective);
                    g_free(raw);
                    g_free(url);
                    p = close ? close + 4 : url_end;
                    continue;
                }
            }
        }
        p += 4;
    }
    if (tag_re) g_regex_unref(tag_re);
    *n_out = arr->len;
    return (char **)g_ptr_array_free(arr, FALSE);
}

/* ---------- 下载线程 ---------- */
typedef struct {
    char *url;
    char *text;
    char **links;    /* 提取的页面链接 "url | 文本" */
    int n_links;
    char  err[256];
    long  http_code;
    size_t bytes;
} FetchResult;

static GtkWidget *link_list = NULL;   /* 右侧链接面板 */

/* 提取链接（公共入口，供 file:// 与 http 共用） */
static void extract_links_into(FetchResult *r, const char *html) {
    if (html)
        r->links = extract_links(html, &r->n_links);
}

static gpointer fetch_thread(gpointer ud) {
    FetchResult *r = (FetchResult *)ud;
    /* file:// 本地文件支持 */
    if (g_str_has_prefix(r->url, "file://")) {
        gchar *path = r->url + 7;
        gchar *data = NULL;
        gsize len = 0;
        if (g_file_get_contents(path, &data, &len, NULL) && data) {
            r->text = html_to_text(data);
            extract_links_into(r, data);
            r->bytes = len;
            g_free(data);
        } else {
            g_strlcpy(r->err, TR("无法读取本地文件"), sizeof r->err);
        }
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, (GSourceFunc)finish_fetch, r, NULL);
        return NULL;
    }
    CURL *curl = curl_easy_init();
    struct curl_buf buf = { NULL, 0 };
    long code = 0;
    char errbuf[CURL_ERROR_SIZE] = "";
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, r->url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "qybrowser/0.1 (LANXIU Linux)");
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            r->http_code = code;
            r->bytes = buf.len;
            if (buf.data) {
                r->text = html_to_text(buf.data);
                extract_links_into(r, buf.data);
                g_free(buf.data);
            } else {
                g_strlcpy(r->err, TR("无数据"), sizeof r->err);
            }
        } else {
            g_strlcpy(r->err, errbuf[0] ? errbuf : curl_easy_strerror(res), sizeof r->err);
        }
        curl_easy_cleanup(curl);
    }
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, (GSourceFunc)finish_fetch, r, NULL);
    return NULL;
}

/* ---------- 主线程更新 ---------- */
static gboolean finish_fetch(gpointer ud) {
    FetchResult *r = (FetchResult *)ud;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (r->text) {
        gchar *head = g_strdup_printf("%s\n%s\n", TR("页面内容"), "——————————————————");
        gtk_text_buffer_insert(buffer, &end, head, -1);
        gtk_text_buffer_insert(buffer, &end, r->text, -1);
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
        g_free(head);
        g_free(r->text);
    }
    char st[256];
    if (r->http_code > 0)
        g_snprintf(st, sizeof st, "HTTP %ld · %zu %s", r->http_code, r->bytes, TR("字节"));
    else
        g_snprintf(st, sizeof st, "%s: %s", TR("下载失败"), r->err);
    gtk_label_set_text(GTK_LABEL(status_label), st);
    if (dl_spinner) gtk_spinner_stop(GTK_SPINNER(dl_spinner));

    /* 填充右侧链接面板 */
    if (link_list) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(link_list));
        for (GList *l = children; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(children);
        if (r->n_links > 0) {
            for (int i = 0; i < r->n_links; i++) {
                GtkWidget *b = gtk_button_new_with_label(r->links[i]);
                gtk_widget_set_halign(b, GTK_ALIGN_START);
                gtk_widget_set_tooltip_text(b, r->links[i]);
                g_signal_connect(b, "clicked", G_CALLBACK(on_link_clicked), g_strdup(r->links[i]));
                gtk_box_pack_start(GTK_BOX(link_list), b, FALSE, FALSE, 0);
            }
        } else {
            GtkWidget *lbl = gtk_label_new(TR("无链接"));
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(link_list), lbl, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(link_list);
    }
    g_printerr("QYBROWSERDBG: links=%d\n", r->n_links);

    g_free(r->url);
    if (r->links) {
        for (int i = 0; i < r->n_links; i++) g_free(r->links[i]);
        g_free(r->links);
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* 点击链接：提取 URL 并打开 */
static void on_link_clicked(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *item = (const char *)ud;
    const char *sep = strchr(item, ' ');
    if (sep && url_entry) {
        gchar *url = g_strndup(item, (gsize)(sep - item));
        gtk_entry_set_text(GTK_ENTRY(url_entry), url);
        open_url(url);
        g_free(url);
    }
    g_free(ud);
}

/* ---------- 打开 URL ---------- */
static void open_url(const char *url) {
    if (!url || !url[0]) return;
    if (!hist) hist = g_ptr_array_new_with_free_func(g_free);
    /* 历史导航时不重复入栈 */
    if (!hist_nav) {
        /* 清空当前位置之后的前进历史 */
        while (hist_pos < (int)hist->len - 1)
            g_ptr_array_remove_index(hist, hist->len - 1);
        g_ptr_array_add(hist, g_strdup(url));
        hist_pos = (int)hist->len - 1;
        /* 持久化浏览历史（release 264）：~/.config/lanxiu/qybrowser-history.txt，每行 URL */
        {
            gchar *hp = g_build_filename(g_get_user_config_dir(), "lanxiu", "qybrowser-history.txt", NULL);
            gchar *old = NULL; gsize olen = 0;
            if (!g_file_get_contents(hp, &old, &olen, NULL)) old = g_strdup("");
            gchar **ls = g_strsplit(old, "\n", -1);
            int n = g_strv_length(ls);
            if (n == 0 || g_strcmp0(ls[n-1], url) != 0) {
                gchar *nl = g_strdup_printf("%s%s%s", old && old[0] ? old : "", old && old[0] ? "\n" : "", url);
                g_file_set_contents(hp, nl, -1, NULL);
                g_free(nl);
            }
            g_strfreev(ls); g_free(old); g_free(hp);
        }
    }
    /* 每次打开替换正文 */
    gtk_text_buffer_set_text(buffer, "", -1);
    if (link_list) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(link_list));
        for (GList *l = children; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(children);
    }
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gchar *head = g_strdup_printf("%s\n%s\n", url, "——————————————————");
    gtk_text_buffer_insert(buffer, &end, head, -1);
    g_free(head);
    gtk_label_set_text(GTK_LABEL(status_label), TR("正在下载..."));
    if (dl_spinner) gtk_spinner_start(GTK_SPINNER(dl_spinner));
    if (back_btn) gtk_widget_set_sensitive(back_btn, hist_pos > 0);
    if (fwd_btn) gtk_widget_set_sensitive(fwd_btn, hist_pos < (int)hist->len - 1);
    FetchResult *r = g_new0(FetchResult, 1);
    r->url = g_strdup(url);
    GThread *t = g_thread_new("qyb-fetch", fetch_thread, r);
    g_thread_unref(t);
}

/* ---------- 浏览历史弹窗（release 264） ---------- */
static void on_hist_row(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)row;
    const char *url = ud ? (const char *)ud : NULL;
    if (url && url[0]) open_url(url);
}

static void on_show_history(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(TR("浏览历史"), NULL, GTK_DIALOG_MODAL,
        TR("关闭"), GTK_RESPONSE_CLOSE, NULL);
    qy_window_setup(GTK_WINDOW(dlg), 460, 340, 380, 260, FALSE);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *lb = gtk_list_box_new();
    gchar *hp = g_build_filename(g_get_user_config_dir(), "lanxiu", "qybrowser-history.txt", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(hp, &content, NULL, NULL) && content) {
        gchar **ls = g_strsplit(content, "\n", -1);
        int n = g_strv_length(ls);
        int start = n > 100 ? n - 100 : 0;
        for (int i = start; i < n; i++) {
            if (!ls[i] || !ls[i][0]) continue;
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *lbl = gtk_label_new(ls[i]);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_container_add(GTK_CONTAINER(row), lbl);
            g_signal_connect(row, "activate", G_CALLBACK(on_hist_row), g_strdup(ls[i]));
            gtk_list_box_insert(GTK_LIST_BOX(lb), row, -1);
        }
        g_strfreev(ls);
    }
    g_free(content); g_free(hp);
    gtk_container_add(GTK_CONTAINER(sw), lb);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy), NULL);
}

static void on_back(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (hist_pos > 0) {
        hist_pos--;
        hist_nav = TRUE;
        open_url((const char *)g_ptr_array_index(hist, hist_pos));
        hist_nav = FALSE;
        gtk_entry_set_text(GTK_ENTRY(url_entry), (const char *)g_ptr_array_index(hist, hist_pos));
    }
}

static void on_fwd(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (hist_pos < (int)hist->len - 1) {
        hist_pos++;
        hist_nav = TRUE;
        open_url((const char *)g_ptr_array_index(hist, hist_pos));
        hist_nav = FALSE;
        gtk_entry_set_text(GTK_ENTRY(url_entry), (const char *)g_ptr_array_index(hist, hist_pos));
    }
}

/* ---------- 书签管理（存 ~/.config/lanxiu/qybrowser-bookmarks.txt，每行 名称|URL） ---------- */
typedef struct { gchar *name; gchar *url; } BmEntry;

static GPtrArray *bm_entries = NULL;   /* BmEntry 列表 */
static GtkWidget *bm_popover = NULL;    /* 工具栏「书签」弹出面板 */
static GtkWidget *bm_listbox = NULL;    /* 书签 GtkListBox */

static gchar *bm_file_path(void) {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *p = g_build_filename(dir, "qybrowser-bookmarks.txt", NULL);
    g_free(dir);
    return p;
}

static void bm_entry_free(gpointer p) {
    BmEntry *e = (BmEntry *)p;
    g_free(e->name);
    g_free(e->url);
    g_free(e);
}

/* 由 URL 推导书签名（http(s) 用主机名，其余用 URL 本身） */
static gchar *bm_name_for_url(const char *url) {
    const char *h = NULL;
    if (g_str_has_prefix(url, "https://")) h = url + 8;
    else if (g_str_has_prefix(url, "http://")) h = url + 7;
    if (h) {
        const char *end = strpbrk(h, "/:?#");
        if (!end) end = h + strlen(h);
        return g_strndup(h, (gsize)(end - h));
    }
    return g_strdup(url);
}

/* 书签行：名称 + URL 两行 */
static GtkWidget *bm_make_row(BmEntry *e) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_set_border_width(GTK_CONTAINER(v), 4);
    gtk_container_add(GTK_CONTAINER(row), v);
    GtkWidget *name = gtk_label_new(e->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(v), name, FALSE, FALSE, 0);
    GtkWidget *url = gtk_label_new(e->url);
    gtk_label_set_xalign(GTK_LABEL(url), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(url), PANGO_ELLIPSIZE_END);
    gtk_widget_set_opacity(url, 0.65);
    gtk_box_pack_start(GTK_BOX(v), url, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(row), "bm-entry", e);   /* 所有权仍在 bm_entries */
    return row;
}

/* 写入书签文件（全量） */
static void bm_save(void) {
    if (!bm_entries) return;
    gchar *path = bm_file_path();
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < bm_entries->len; i++) {
        BmEntry *e = (BmEntry *)g_ptr_array_index(bm_entries, i);
        g_string_append_printf(out, "%s|%s\n", e->name, e->url);
    }
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    g_free(path);
}

/* 添加当前页为书签（去重） */
static void bm_add_url(const char *url) {
    if (!url || !url[0]) return;
    for (guint i = 0; i < bm_entries->len; i++) {
        BmEntry *e = (BmEntry *)g_ptr_array_index(bm_entries, i);
        if (e->url && !strcmp(e->url, url)) {
            gtk_label_set_text(GTK_LABEL(status_label), TR("已收藏"));
            return;
        }
    }
    BmEntry *e = g_new0(BmEntry, 1);
    e->name = bm_name_for_url(url);
    e->url = g_strdup(url);
    g_ptr_array_add(bm_entries, e);
    if (bm_listbox) {
        GtkWidget *row = bm_make_row(e);
        gtk_list_box_insert(GTK_LIST_BOX(bm_listbox), row, -1);
        gtk_widget_show_all(row);
    }
    bm_save();
    gtk_label_set_text(GTK_LABEL(status_label), TR("已收藏"));
}

/* 收藏当前页（工具栏按钮 + 弹层面板按钮共用） */
static void on_bookmark(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *url = gtk_entry_get_text(GTK_ENTRY(url_entry));
    bm_add_url(url);
}

/* 删除一条书签：从数组与列表移除并保存 */
static void bm_delete_row(GtkListBoxRow *row) {
    if (!row) return;
    BmEntry *e = (BmEntry *)g_object_get_data(G_OBJECT(row), "bm-entry");
    if (e) {
        for (guint i = 0; i < bm_entries->len; i++) {
            if (g_ptr_array_index(bm_entries, i) == e) {
                g_ptr_array_remove_index(bm_entries, i);   /* free_func 释放 e */
                break;
            }
        }
    }
    gtk_widget_destroy(GTK_WIDGET(row));
    bm_save();
    gtk_label_set_text(GTK_LABEL(status_label), TR("已删除书签"));
}

/* 右键菜单：删除 */
static void bm_on_menu_delete(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    bm_delete_row(GTK_LIST_BOX_ROW(ud));
}

/* 右键书签行 → 「删除」 */
static gboolean bm_on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(bm_listbox), (gint)ev->y);
    if (!row) return TRUE;
    gtk_list_box_select_row(GTK_LIST_BOX(bm_listbox), row);
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("删除"));
    g_signal_connect(mi, "activate", G_CALLBACK(bm_on_menu_delete), row);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
    return TRUE;
}

/* 弹层内「删除」按钮：删除当前选中书签 */
static void bm_delete_selected(GtkWidget *b, gpointer ud) {
    (void)b; (void)ud;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(bm_listbox));
    if (row) bm_delete_row(row);
}

/* 点击书签行 → 打开 URL */
static void bm_on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    BmEntry *e = row ? (BmEntry *)g_object_get_data(G_OBJECT(row), "bm-entry") : NULL;
    if (e && e->url && e->url[0]) {
        gtk_entry_set_text(GTK_ENTRY(url_entry), e->url);
        open_url(e->url);
        if (bm_popover) gtk_popover_popdown(GTK_POPOVER(bm_popover));
    }
}

/* 启动时从 qybrowser-bookmarks.txt 加载书签 */
static void bm_load(void) {
    if (!bm_listbox) return;
    if (!bm_entries) bm_entries = g_ptr_array_new_with_free_func(bm_entry_free);
    gchar *path = bm_file_path();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", 0);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!line[0]) continue;
            gchar **parts = g_strsplit(line, "|", 2);
            if (parts[0] && parts[0][0] && parts[1] && parts[1][0]) {
                BmEntry *e = g_new0(BmEntry, 1);
                e->name = g_strdup(parts[0]);
                e->url = g_strdup(parts[1]);
                g_ptr_array_add(bm_entries, e);
                GtkWidget *row = bm_make_row(e);
                gtk_list_box_insert(GTK_LIST_BOX(bm_listbox), row, -1);
                gtk_widget_show_all(row);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
}

/* 工具栏「书签」：弹出/收起 GtkPopover */
static void on_show_bookmarks(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!bm_popover) return;
    if (gtk_widget_get_visible(GTK_WIDGET(bm_popover)))
        gtk_popover_popdown(GTK_POPOVER(bm_popover));
    else
        gtk_popover_popup(GTK_POPOVER(bm_popover));
}

static gboolean auto_show_bookmarks(gpointer p) {
    (void)p;
    on_show_bookmarks(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* 自动化辅助 */
static gboolean auto_open_second(gpointer p) {
    const char *url = (const char *)p;
    gtk_entry_set_text(GTK_ENTRY(url_entry), url);
    open_url(url);
    g_free(p);
    return G_SOURCE_REMOVE;
}
static gboolean auto_go_back(gpointer p) {
    (void)p;
    on_back(NULL, NULL);
    return G_SOURCE_REMOVE;
}
static gboolean auto_bookmark(gpointer p) {
    (void)p;
    on_bookmark(NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void on_open(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *url = gtk_entry_get_text(GTK_ENTRY(url_entry));
    /* 自动补 http:// */
    char *full = NULL;
    if (strstr(url, "://") || g_str_has_prefix(url, "about:"))
        full = g_strdup(url);
    else
        full = g_strdup_printf("http://%s", url);
    open_url(full);
    g_free(full);
}

static gboolean on_entry_activate(GtkWidget *w, gpointer ud) {
    on_open(w, ud);
    return TRUE;
}

static void on_clear(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    gtk_text_buffer_set_text(buffer, "", -1);
}

/* ---------- 正文右键菜单（release 256: 复制/全选） ---------- */
static void on_view_copy(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    gtk_text_buffer_copy_clipboard(buffer, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
}

static void on_view_select_all(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_select_range(buffer, &start, &end);
}

static gboolean on_view_button_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkTextIter start, end;
    gboolean has_sel = gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
    GtkWidget *mi_copy = gtk_menu_item_new_with_label(TR("复制"));
    gtk_widget_set_sensitive(mi_copy, has_sel);
    g_signal_connect(mi_copy, "activate", G_CALLBACK(on_view_copy), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_copy);
    GtkWidget *mi_all = gtk_menu_item_new_with_label(TR("全选"));
    g_signal_connect(mi_all, "activate", G_CALLBACK(on_view_select_all), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_all);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), view, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
    return TRUE;
}

/* ---------- 下载管理（底部 GtkRevealer 历史面板） ----------
 * 历史存 ~/.config/lanxiu/downloads.txt，每行: 路径|状态|时间
 * 状态: downloading | done | failed。 */
typedef struct {
    gchar *path;       /* 完整路径 */
    gchar *status;     /* downloading | done | failed */
    gint64  ts;        /* Unix 时间戳 */
    GPid    pid;       /* 子进程 pid（downloading 时有效） */
    GtkWidget *row;
    GtkWidget *status_lbl;
} DownloadEntry;

static void dl_entry_free(gpointer p) {
    DownloadEntry *e = (DownloadEntry *)p;
    g_free(e->path);
    g_free(e->status);
    g_free(e);
}

static gchar *dl_history_path(void) {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *p = g_build_filename(dir, "downloads.txt", NULL);
    g_free(dir);
    return p;
}

static void dl_save(void) {
    if (!dl_entries) return;
    gchar *path = dl_history_path();
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < dl_entries->len; i++) {
        DownloadEntry *e = (DownloadEntry *)g_ptr_array_index(dl_entries, i);
        g_string_append_printf(out, "%s|%s|%" G_GINT64_FORMAT "\n",
                               e->path, e->status, e->ts);
    }
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    g_free(path);
}

/* 状态文字: 进行中 / 完成(含大小) / 失败 */
static gchar *dl_status_text(DownloadEntry *e) {
    if (!strcmp(e->status, "downloading")) return g_strdup(TR("进行中"));
    if (!strcmp(e->status, "failed")) return g_strdup(TR("失败"));
    GStatBuf st;
    if (g_stat(e->path, &st) == 0 && st.st_size > 0) {
        if (st.st_size >= 1024 * 1024)
            return g_strdup_printf(TR("完成 %d MB"), (int)(st.st_size / (1024 * 1024)));
        if (st.st_size >= 1024)
            return g_strdup_printf(TR("完成 %d KB"), (int)(st.st_size / 1024));
        return g_strdup_printf(TR("完成 %d B"), (int)st.st_size);
    }
    return g_strdup(TR("完成"));
}

/* 创建历史行: 文件名 + 状态 */
static GtkWidget *dl_make_row(DownloadEntry *e) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(h), 4);
    gtk_container_add(GTK_CONTAINER(row), h);

    const gchar *base = g_path_get_basename(e->path);
    GtkWidget *name_lbl = gtk_label_new(base);
    g_free((gchar *)base);
    gtk_widget_set_hexpand(name_lbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(h), name_lbl, TRUE, TRUE, 0);

    GtkWidget *st_lbl = gtk_label_new(NULL);
    e->status_lbl = st_lbl;
    gtk_box_pack_start(GTK_BOX(h), st_lbl, FALSE, FALSE, 0);
    return row;
}

static void dl_update_row(DownloadEntry *e) {
    if (e && e->status_lbl) {
        gchar *s = dl_status_text(e);
        gtk_label_set_text(GTK_LABEL(e->status_lbl), s);
        g_free(s);
    }
}

/* curl 子进程结束: 更新状态并持久化 */
static void dl_on_exit(GPid pid, gint status, gpointer ud) {
    g_spawn_close_pid(pid);
    DownloadEntry *e = (DownloadEntry *)ud;
    if (!e) return;
    e->pid = 0;
    g_free(e->status);
    e->status = g_strdup((WIFEXITED(status) && WEXITSTATUS(status) == 0) ? "done" : "failed");
    dl_update_row(e);
    dl_save();
}

/* 打开所在目录（qyfiles，支持目录参数） */
static void dl_open_dir(DownloadEntry *e) {
    if (!e || !e->path) return;
    gchar *dir = g_path_get_dirname(e->path);
    gchar *cmd = g_strdup_printf("qyfiles %s", g_shell_quote(dir));
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    g_free(dir);
}

/* 打开文件: 文本类用 qyedit，图片类用 qyview */
static void dl_open_file(DownloadEntry *e) {
    if (!e || !e->path) return;
    const char *ext = strrchr(e->path, '.');
    const char *app = "qyview";
    if (ext) {
        static const char *text_exts[] = {
            ".txt", ".html", ".htm", ".c", ".h", ".py", ".sh", ".md",
            ".json", ".xml", ".log", ".conf", ".css", ".js", ".ini", ".csv", NULL
        };
        for (int i = 0; text_exts[i]; i++) {
            if (g_ascii_strcasecmp(ext, text_exts[i]) == 0) { app = "qyedit"; break; }
        }
    }
    gchar *cmd = g_strdup_printf("%s %s", app, g_shell_quote(e->path));
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

static void dl_on_menu_dir(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    dl_open_dir((DownloadEntry *)ud);
}

static void dl_on_menu_file(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    dl_open_file((DownloadEntry *)ud);
}

/* 右键历史条目: 打开所在目录 / 打开文件 */
static gboolean dl_on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(dl_listbox), (gint)ev->y);
    if (!row) return FALSE;
    DownloadEntry *e = (DownloadEntry *)g_object_get_data(G_OBJECT(row), "dl-entry");
    if (!e) return FALSE;

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("打开所在目录"));
    g_signal_connect(mi, "activate", G_CALLBACK(dl_on_menu_dir), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("打开文件"));
    g_signal_connect(mi, "activate", G_CALLBACK(dl_on_menu_file), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
    return TRUE;
}

/* 双击/回车打开文件 */
static void dl_on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    DownloadEntry *e = row ? (DownloadEntry *)g_object_get_data(G_OBJECT(row), "dl-entry") : NULL;
    dl_open_file(e);
}

/* 启动下载: 用 curl -L -o <目标> <url>，并接入历史状态记录 */
static void dl_start(const char *url) {
    if (!url || !url[0] || !dl_listbox) return;
    const char *dl_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    gchar *dir = g_strdup(dl_dir && dl_dir[0] ? dl_dir : g_get_home_dir());
    g_mkdir_with_parents(dir, 0755);

    const char *slash = strrchr(url, '/');
    gchar *fname = g_strdup(slash ? slash + 1 : url);
    char *q = strpbrk(fname, "?#");
    if (q) *q = 0;
    if (!fname[0]) { g_free(fname); fname = g_strdup("index.html"); }
    gchar *path = g_build_filename(dir, fname, NULL);
    g_free(fname);
    g_free(dir);

    if (!dl_entries) dl_entries = g_ptr_array_new_with_free_func(dl_entry_free);
    DownloadEntry *e = g_new0(DownloadEntry, 1);
    e->path = path;
    e->status = g_strdup("downloading");
    e->ts = time(NULL);
    e->row = dl_make_row(e);
    g_object_set_data_full(G_OBJECT(e->row), "dl-entry", e, NULL);
    gtk_list_box_insert(GTK_LIST_BOX(dl_listbox), e->row, 0);
    gtk_widget_show_all(e->row);
    g_ptr_array_add(dl_entries, e);
    dl_update_row(e);
    dl_save();

    gchar *qpath = g_shell_quote(e->path);
    gchar *qurl = g_shell_quote(url);
    gchar *cmd = g_strdup_printf("curl -L -o %s %s", qpath, qurl);
    g_free(qpath);
    g_free(qurl);
    GError *err = NULL;
    gchar **argv = NULL;
    if (g_shell_parse_argv(cmd, NULL, &argv, &err)) {
        GPid pid = 0;
        if (g_spawn_async(NULL, argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                          NULL, NULL, &pid, &err)) {
            e->pid = pid;
            g_child_watch_add(pid, dl_on_exit, e);
        } else {
            g_free(e->status);
            e->status = g_strdup("failed");
            dl_update_row(e);
            dl_save();
        }
        g_strfreev(argv);
    } else {
        g_free(e->status);
        e->status = g_strdup("failed");
        dl_update_row(e);
        dl_save();
    }
    g_clear_error(&err);
    g_free(cmd);
}

/* 启动时加载下载历史 */
static void dl_load(void) {
    if (!dl_listbox) return;
    if (!dl_entries) dl_entries = g_ptr_array_new_with_free_func(dl_entry_free);
    gchar *path = dl_history_path();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", 0);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!line[0]) continue;
            gchar **parts = g_strsplit(line, "|", 3);
            if (parts[0] && parts[0][0] && parts[1] && parts[1][0] && parts[2]) {
                DownloadEntry *e = g_new0(DownloadEntry, 1);
                e->path = g_strdup(parts[0]);
                e->status = g_strdup(parts[1]);
                e->ts = g_ascii_strtoll(parts[2], NULL, 10);
                /* 上次会话中断的下载视为失败 */
                if (!strcmp(e->status, "downloading")) {
                    g_free(e->status);
                    e->status = g_strdup("failed");
                }
                e->row = dl_make_row(e);
                g_object_set_data_full(G_OBJECT(e->row), "dl-entry", e, NULL);
                gtk_list_box_insert(GTK_LIST_BOX(dl_listbox), e->row, 0);
                gtk_widget_show_all(e->row);
                dl_update_row(e);
                g_ptr_array_add(dl_entries, e);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
}

static void on_dl_start_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *u = gtk_entry_get_text(GTK_ENTRY(dl_url_entry));
    if (!u || !u[0]) u = gtk_entry_get_text(GTK_ENTRY(url_entry));
    dl_start(u);
}

/* 工具栏「下载」按钮: 切换底部历史面板 */
static void on_dl_toggle(GtkWidget *w, gpointer ud) {
    (void)ud;
    gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    if (active && dl_revealer) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(dl_revealer), TRUE);
        const char *cur = gtk_entry_get_text(GTK_ENTRY(url_entry));
        if (dl_url_entry && !gtk_entry_get_text(GTK_ENTRY(dl_url_entry))[0] && cur[0])
            gtk_entry_set_text(GTK_ENTRY(dl_url_entry), cur);
    } else if (dl_revealer) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(dl_revealer), FALSE);
    }
}

/* ---------- 主窗口 ---------- */
static void activate(GtkApplication *app, gpointer ud) {
    (void)ud;
    qy_load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫浏览器"));
    qy_make_titlebar(GTK_WINDOW(win), TR("澜岫浏览器"));
    qy_window_setup(GTK_WINDOW(win), 720, 520, 560, 420, FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 工具栏: 后退/前进 + 地址栏 + 打开 + 收藏 + 清空 */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    back_btn = qy_icon_button(QY_ICON_BACK, 18, TR("后退"), NULL);
    gtk_widget_set_sensitive(back_btn, FALSE);
    fwd_btn = qy_icon_button(QY_ICON_FORWARD, 18, TR("前进"), NULL);
    gtk_widget_set_sensitive(fwd_btn, FALSE);
    g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(fwd_btn, "clicked", G_CALLBACK(on_fwd), NULL);
    url_entry = gtk_entry_new();
    qy_add_class(url_entry, "qy-search-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "https://...");
    GtkWidget *btn = gtk_button_new_with_label(TR("打开"));
    qy_add_class(btn, "qy-btn");
    GtkWidget *bm_btn = qy_icon_button(QY_ICON_BOOKMARK, 18, TR("收藏"), NULL);
    g_signal_connect(bm_btn, "clicked", G_CALLBACK(on_bookmark), NULL);
    GtkWidget *bm_list_btn = qy_icon_button(QY_ICON_LIST, 18, TR("书签"), NULL);
    g_signal_connect(bm_list_btn, "clicked", G_CALLBACK(on_show_bookmarks), NULL);
    GtkWidget *btn2 = gtk_button_new_with_label(TR("清空"));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_open), NULL);
    g_signal_connect(btn2, "clicked", G_CALLBACK(on_clear), NULL);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(bar), back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), url_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), bm_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), bm_list_btn, FALSE, FALSE, 0);
    hist_btn = qy_icon_button(QY_ICON_CLOCK, 18, TR("历史"), NULL);
    g_signal_connect(hist_btn, "clicked", G_CALLBACK(on_show_history), NULL);
    gtk_box_pack_start(GTK_BOX(bar), hist_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn2, FALSE, FALSE, 0);

    /* 书签 GtkPopover：列表 + 添加当前页 + 删除 */
    bm_popover = gtk_popover_new(bm_list_btn);
    gtk_popover_set_position(GTK_POPOVER(bm_popover), GTK_POS_BOTTOM);
    gtk_popover_set_modal(GTK_POPOVER(bm_popover), FALSE);
    GtkWidget *bm_v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(bm_v, 8);
    gtk_widget_set_margin_end(bm_v, 8);
    gtk_widget_set_margin_top(bm_v, 6);
    gtk_widget_set_margin_bottom(bm_v, 6);
    gtk_container_add(GTK_CONTAINER(bm_popover), bm_v);
    GtkWidget *bm_add_btn = gtk_button_new_with_label(TR("添加当前页为书签"));
    qy_add_class(bm_add_btn, "qy-btn");
    g_signal_connect(bm_add_btn, "clicked", G_CALLBACK(on_bookmark), NULL);
    gtk_box_pack_start(GTK_BOX(bm_v), bm_add_btn, FALSE, FALSE, 0);
    GtkWidget *bm_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bm_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(bm_sw, 280, 200);
    bm_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(bm_listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(bm_listbox), TRUE);
    g_signal_connect(bm_listbox, "row-activated", G_CALLBACK(bm_on_row_activated), NULL);
    g_signal_connect(bm_listbox, "button-press-event", G_CALLBACK(bm_on_button_press), NULL);
    gtk_container_add(GTK_CONTAINER(bm_sw), bm_listbox);
    gtk_box_pack_start(GTK_BOX(bm_v), bm_sw, TRUE, TRUE, 0);
    GtkWidget *bm_del_btn = gtk_button_new_with_label(TR("删除"));
    qy_add_class(bm_del_btn, "qy-btn");
    g_signal_connect(bm_del_btn, "clicked", G_CALLBACK(bm_delete_selected), NULL);
    gtk_box_pack_start(GTK_BOX(bm_v), bm_del_btn, FALSE, FALSE, 0);
    /* 下载管理: 工具栏「下载」按钮切换底部历史面板 */
    dl_toggle_btn = gtk_toggle_button_new_with_label(TR("下载"));
    qy_add_class(dl_toggle_btn, "qy-btn");
    g_signal_connect(dl_toggle_btn, "toggled", G_CALLBACK(on_dl_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(bar), dl_toggle_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    /* 正文: 左侧纯文本视图 + 右侧链接面板 */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    qy_add_class(paned, "qy-app-surface");
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    view = gtk_text_view_new();
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
    g_signal_connect(view, "button-press-event", G_CALLBACK(on_view_button_press), NULL);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_paned_add1(GTK_PANED(paned), sw);

    /* 右侧链接面板 */
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *side_title = gtk_label_new(TR("页面链接"));
    gtk_widget_set_halign(side_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(side), side_title, FALSE, FALSE, 0);
    GtkWidget *side_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(side_sw, 260, -1);
    link_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(side_sw), link_list);
    gtk_box_pack_start(GTK_BOX(side), side_sw, TRUE, TRUE, 0);
    gtk_paned_add2(GTK_PANED(paned), side);
    gtk_paned_set_position(GTK_PANED(paned), 440);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    /* 下载历史面板（GtkRevealer，默认收起） */
    dl_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(dl_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(dl_revealer), 200);
    GtkWidget *dl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *dl_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *dl_lb = gtk_label_new(TR("下载"));
    gtk_widget_set_halign(dl_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(dl_header), dl_lb, FALSE, FALSE, 0);
    dl_url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dl_url_entry), "https://...");
    gtk_widget_set_hexpand(dl_url_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(dl_header), dl_url_entry, TRUE, TRUE, 0);
    GtkWidget *dl_start_btn = gtk_button_new_with_label(TR("下载"));
    qy_add_class(dl_start_btn, "qy-btn");
    g_signal_connect(dl_start_btn, "clicked", G_CALLBACK(on_dl_start_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(dl_header), dl_start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dl_box), dl_header, FALSE, FALSE, 0);

    GtkWidget *dl_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dl_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(dl_sw, -1, 140);
    dl_listbox = gtk_list_box_new();
    gtk_widget_add_events(dl_listbox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(dl_listbox, "button-press-event", G_CALLBACK(dl_on_button_press), NULL);
    g_signal_connect(dl_listbox, "row-activated", G_CALLBACK(dl_on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(dl_sw), dl_listbox);
    gtk_box_pack_start(GTK_BOX(dl_box), dl_sw, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(dl_revealer), dl_box);
    gtk_box_pack_start(GTK_BOX(vbox), dl_revealer, FALSE, FALSE, 0);

    /* 状态栏（含加载动画） */
    GtkWidget *status_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    dl_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(dl_spinner, 16, 16);
    gtk_box_pack_start(GTK_BOX(status_hb), dl_spinner, FALSE, FALSE, 0);
    status_label = gtk_label_new(TR("就绪"));
    qy_add_class(status_label, "qy-mon-info");
    qy_add_class(status_label, "qy-browser-status");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status_hb), status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_hb, FALSE, FALSE, 0);

    /* 自动化: QYBROWSER_URL 启动后自动打开 */
    const char *auto_url = g_getenv("QYBROWSER_URL");
    if (auto_url) {
        gtk_entry_set_text(GTK_ENTRY(url_entry), auto_url);
        open_url(auto_url);
        if (g_getenv("QYBROWSER_BOOKMARK"))
            g_timeout_add(3000, auto_bookmark, NULL);
        if (g_getenv("QYBROWSER_SHOWBOOKMARKS"))
            g_timeout_add(4000, auto_show_bookmarks, NULL);
        const char *second = g_getenv("QYBROWSER_URL2");
        if (second) {
            g_timeout_add(5000, auto_open_second, g_strdup(second));
            if (g_getenv("QYBROWSER_BACK"))
                g_timeout_add(9000, auto_go_back, NULL);
        }
    }

    /* ---------- 浏览器标准快捷键 ---------- */
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), accel);
    gtk_widget_add_accelerator(url_entry, "grab-focus", accel, GDK_KEY_l, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+L 地址栏 */
    gtk_widget_add_accelerator(back_btn, "clicked", accel, GDK_KEY_Left, GDK_MOD1_MASK, GTK_ACCEL_VISIBLE);     /* Alt+← 后退 */
    gtk_widget_add_accelerator(fwd_btn, "clicked", accel, GDK_KEY_Right, GDK_MOD1_MASK, GTK_ACCEL_VISIBLE);    /* Alt+→ 前进 */
    gtk_widget_add_accelerator(bm_btn, "clicked", accel, GDK_KEY_d, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);      /* Ctrl+D 收藏 */
    /* Ctrl+R 刷新（图标按钮重新打开当前地址） */
    GtkWidget *b_reload = qy_icon_button(QY_ICON_REFRESH, 18, TR("刷新"), NULL);
    g_signal_connect(b_reload, "clicked", G_CALLBACK(on_entry_activate), NULL);
    gtk_widget_add_accelerator(b_reload, "clicked", accel, GDK_KEY_r, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_box_pack_start(GTK_BOX(bar), b_reload, FALSE, FALSE, 0);
    g_printerr("QYBROWSERDBG: accel 5 keys\n");

    gtk_widget_show_all(win);

    /* 加载书签 + 下载历史 */
    bm_load();
    dl_load();
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    GtkApplication *app = gtk_application_new("com.lanxiu.browser", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    curl_global_cleanup();
    return rc;
}