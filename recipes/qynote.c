/* qynote.c — 澜岫笔记
 * 纯文件存储（无 sqlite）：~/.local/share/qynote/<标题>.txt。
 * 左侧 GtkListBox 列出所有笔记标题（每个 .txt 的首行），右侧 GtkTextView 编辑正文。
 * 顶栏：新建（默认「未命名笔记」）/ 删除 / 保存（Ctrl+S）。刷新列表保留当前选中。
 * 自动化: QYNOTE_NEW=1 启动后自动新建一篇（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GtkWidget *win         = NULL;
static GtkListBox *listbox   = NULL;
static GtkTextView *textview = NULL;
static GtkWidget *status_lb  = NULL;
static GtkWidget *count_lb   = NULL;   /* 实时字数/行数统计（release 283） */
static GtkWidget *search_entry = NULL;   /* 笔记搜索框（release 276） */
static GtkWidget *stack      = NULL;
static GtkWidget *placeholder = NULL;
static gboolean sort_mtime = FALSE;   /* 按修改时间排序（release 293） */
static GtkWidget *b_sort = NULL;      /* 排序开关按钮 */
static gchar *current_path   = NULL;   /* 当前打开笔记的绝对路径 */
static gboolean loading_list = FALSE;

static void refresh_note_list(void);   /* 前向声明 */

/* ---------- 路径与文件工具 ---------- */
static gchar *notes_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "qynote", NULL);
}

/* 读取文件首行作为标题 */
static gchar *read_first_line(const char *path) {
    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        return g_strdup("");
    }
    char *nl = strchr(content, '\n');
    if (nl) *nl = '\0';
    gchar *title = g_strstrip(g_strdup(content));
    g_free(content);
    return title;
}

/* 从正文提取标题（第一行），空则用「未命名笔记」 */
static gchar *extract_title(const char *content) {
    const char *nl = strchr(content, '\n');
    gchar *title = nl ? g_strndup(content, nl - content) : g_strdup(content);
    g_strstrip(title);
    if (!title[0]) {
        g_free(title);
        return g_strdup(TR("未命名笔记"));
    }
    return title;
}

/* 文件名安全化：替换路径分隔符，空名回退 */
static gchar *sanitize_filename(const char *title) {
    gchar *s = g_strdup(title);
    for (char *p = s; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') *p = '-';
    }
    g_strstrip(s);
    if (!s[0]) {
        g_free(s);
        return g_strdup(TR("未命名笔记"));
    }
    return s;
}

/* 生成不冲突的 <标题>.txt 路径（重名追加数字） */
static gchar *unique_note_path(const char *dir, const char *base_title) {
    gchar *safe = sanitize_filename(base_title);
    gchar *name = g_strdup_printf("%s.txt", safe);
    gchar *path = g_build_filename(dir, name, NULL);
    g_free(name);
    int n = 2;
    while (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        name = g_strdup_printf("%s %d.txt", safe, n);
        path = g_build_filename(dir, name, NULL);
        g_free(name);
        n++;
    }
    g_free(safe);
    return path;
}

/* ---------- 笔记加载/保存 ---------- */
static void load_note(const char *path) {
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(textview);
        gtk_text_buffer_set_text(buf, content, -1);
    }
    g_free(content);
}

static void save_current_note(void) {
    gchar *dir = notes_dir();
    g_mkdir_with_parents(dir, 0700);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(textview);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *content = gtk_text_buffer_get_text(buf, &start, &end, TRUE);

    gchar *title = extract_title(content);
    gchar *safe_title = sanitize_filename(title);

    /* 文件名未变 → 原地保存；标题变了 → 写新文件并删除旧文件 */
    gboolean same_name = FALSE;
    if (current_path) {
        gchar *base = g_path_get_basename(current_path);
        gchar *stem = g_strdup(base);
        size_t len = strlen(stem);
        if (len > 4 && g_str_has_suffix(stem, ".txt")) stem[len - 4] = '\0';
        same_name = (g_strcmp0(stem, safe_title) == 0);
        g_free(base);
        g_free(stem);
    }

    gchar *target = NULL;
    if (same_name)
        target = g_strdup(current_path);
    else
        target = unique_note_path(dir, title);
    g_free(safe_title);

    GError *err = NULL;
    if (g_file_set_contents(target, content, -1, &err)) {
        if (current_path && strcmp(current_path, target) != 0)
            g_remove(current_path);
        g_free(current_path);
        current_path = g_strdup(target);
        long wc = g_utf8_strlen(content, -1);
        gchar *st = g_strdup_printf(TR("已保存（%ld 字）"), wc);
        gtk_label_set_text(GTK_LABEL(status_lb), st);
        g_free(st);
    } else {
        if (err) {
            gtk_label_set_text(GTK_LABEL(status_lb), err->message);
            g_error_free(err);
        }
    }
    g_free(target);
    g_free(content);
    g_free(title);
    g_free(dir);

    refresh_note_list();
}

/* ---------- 列表刷新（保留当前选中） ---------- */
static gint sort_paths(gconstpointer a, gconstpointer b) {
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    return g_utf8_collate(pa, pb);
}

/* 按修改时间排序（新→旧，release 293） */
static gint sort_mtime_cmp(gconstpointer a, gconstpointer b) {
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    GStatBuf sa, sb;
    if (g_stat(pa, &sa) != 0) return -1;
    if (g_stat(pb, &sb) != 0) return 1;
    if (sa.st_mtime == sb.st_mtime) return 0;
    return sa.st_mtime > sb.st_mtime ? -1 : 1;
}

static void refresh_note_list(void) {
    loading_list = TRUE;

    GList *ch = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *it = ch; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(ch);

    gchar *dir = notes_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    guint count = 0;
    if (d) {
        GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (g_str_has_suffix(name, ".txt")) {
                gchar *full = g_build_filename(dir, name, NULL);
                g_ptr_array_add(files, full);
            }
        }
        g_dir_close(d);
        g_ptr_array_sort(files, sort_mtime ? sort_mtime_cmp : sort_paths);

        for (guint i = 0; i < files->len; i++) {
            const char *full = g_ptr_array_index(files, i);
            gchar *title = read_first_line(full);
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *lbl = gtk_label_new(title[0] ? title : TR("无标题"));
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_container_add(GTK_CONTAINER(row), lbl);
            g_object_set_data_full(G_OBJECT(row), "note-path", g_strdup(full), g_free);
            gtk_list_box_insert(listbox, row, -1);
            g_free(title);
            count++;
        }
        g_ptr_array_free(files, TRUE);
    }
    g_free(dir);

    /* 恢复选中：优先当前笔记路径 */
    GtkListBoxRow *sel = NULL;
    if (current_path) {
        GList *rows = gtk_container_get_children(GTK_CONTAINER(listbox));
        for (GList *l = rows; l; l = l->next) {
            const char *p = g_object_get_data(G_OBJECT(l->data), "note-path");
            if (p && strcmp(p, current_path) == 0) {
                sel = GTK_LIST_BOX_ROW(l->data);
                break;
            }
        }
        g_list_free(rows);
    }
    if (sel)
        gtk_list_box_select_row(listbox, sel);
    else {
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(listbox, 0);
        if (first) gtk_list_box_select_row(listbox, first);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(stack), count > 0 ? "list" : "empty");
    loading_list = FALSE;
}

/* ---------- 信号 ---------- */
static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    if (loading_list || !row) return;
    const char *path = g_object_get_data(G_OBJECT(row), "note-path");
    if (path) {
        g_free(current_path);
        current_path = g_strdup(path);
        load_note(current_path);
        gtk_label_set_text(GTK_LABEL(status_lb), "");
    }
}

static void on_new_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    gchar *dir = notes_dir();
    g_mkdir_with_parents(dir, 0700);
    gchar *path = unique_note_path(dir, TR("未命名笔记"));
    gchar *default_content = g_strconcat(TR("未命名笔记"), "\n\n", NULL);
    g_file_set_contents(path, default_content, -1, NULL);
    g_free(default_content);

    g_free(current_path);
    current_path = g_strdup(path);
    load_note(current_path);
    refresh_note_list();
    gtk_widget_grab_focus(GTK_WIDGET(textview));
    gtk_label_set_text(GTK_LABEL(status_lb), TR("已新建"));
    g_free(path);
    g_free(dir);
}

static void on_delete_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!current_path) return;
    gchar *title = read_first_line(current_path);
    gchar *msg = g_strdup_printf(TR("删除笔记「%s」？"), title);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(win),
                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                            "%s", msg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_YES) {
        g_remove(current_path);
        g_free(current_path);
        current_path = NULL;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(textview);
        gtk_text_buffer_set_text(buf, "", -1);
        refresh_note_list();
        gtk_label_set_text(GTK_LABEL(status_lb), TR("已删除"));
    }
    gtk_widget_destroy(dlg);
    g_free(msg);
    g_free(title);
}

/* 导出笔记为 UTF-8 文本 / Markdown（release 275/285） */
static void on_export_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(textview);
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buf, &s);
    gtk_text_buffer_get_end_iter(buf, &e);
    char *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    GtkWidget *dlg = gtk_file_chooser_native_new(TR("导出笔记"), GTK_WINDOW(win),
                          GTK_FILE_CHOOSER_ACTION_SAVE, TR("导出"), TR("取消"));
    GtkFileChooser *fc = GTK_FILE_CHOOSER(dlg);
    gtk_file_chooser_set_current_name(fc, "note.txt");
    /* 格式选择（release 285） */
    GtkWidget *extra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(extra), gtk_label_new(TR("格式:")), FALSE, FALSE, 0);
    GtkWidget *fmt = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fmt), TR("纯文本 (.txt)"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fmt), TR("Markdown (.md)"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(fmt), 0);
    gtk_box_pack_start(GTK_BOX(extra), fmt, FALSE, FALSE, 0);
    gtk_file_chooser_set_extra_widget(fc, extra);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(fc);
        if (path) {
            int md = gtk_combo_box_get_active(GTK_COMBO_BOX(fmt)) == 1;
            gchar *out = g_strdup(path);
            const char *ext = strrchr(out, '.');
            gboolean has_ext = ext && (g_ascii_strcasecmp(ext, ".txt") == 0 ||
                                       g_ascii_strcasecmp(ext, ".md") == 0);
            if (!has_ext) {
                gchar *with_ext = g_strdup_printf("%s%s", out, md ? ".md" : ".txt");
                g_free(out);
                out = with_ext;
            }
            if (g_file_set_contents(out, txt, -1, NULL))
                gtk_label_set_text(GTK_LABEL(status_lb),
                                   md ? TR("已导出为 Markdown") : TR("已导出为文本"));
            else
                gtk_label_set_text(GTK_LABEL(status_lb), TR("导出失败"));
            g_free(out);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
    g_free(txt);
}

static void on_save_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    save_current_note();
}

/* 笔记搜索：按文件名过滤列表（release 276） */
static void on_search_changed(GtkSearchEntry *se, gpointer ud) {
    (void)se; (void)ud;
    const char *q = gtk_entry_get_text(GTK_ENTRY(search_entry));
    GList *ch = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = ch; l; l = l->next) {
        const char *path = g_object_get_data(G_OBJECT(l->data), "note-path");
        gboolean show = TRUE;
        if (q && q[0] && path) {
            gchar *b = g_path_get_basename(path);
            show = b && strstr(b, q) != NULL;
            g_free(b);
        }
        gtk_widget_set_visible(GTK_WIDGET(l->data), show);
    }
    g_list_free(ch);
}

/* Ctrl+S 保存 */
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w; (void)ud;
    if ((ev->state & GDK_CONTROL_MASK) &&
        (ev->keyval == GDK_KEY_s || ev->keyval == GDK_KEY_S)) {
        save_current_note();
        return TRUE;
    }
    /* Ctrl+Shift+S: 导出为文本（release 281） */
    if ((ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
        (ev->keyval == GDK_KEY_s || ev->keyval == GDK_KEY_S)) {
        on_export_clicked(NULL, NULL);
        return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) &&
        (ev->keyval == GDK_KEY_n || ev->keyval == GDK_KEY_N)) {
        on_new_clicked(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

/* 拖放 .txt 文件到窗口打开（release 281） */
static void on_win_drag_recv(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                             GtkSelectionData *data, guint info, guint t, gpointer ud) {
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)t; (void)ud;
    gchar **uris = gtk_selection_data_get_uris(data);
    if (uris && uris[0]) {
        gchar *path = g_filename_from_uri(uris[0], NULL, NULL);
        if (path) load_note(path);
        g_free(path);
    }
    g_strfreev(uris);
}

/* 窗口大小记忆（release 281） */
static void save_win_size(void) {
    if (!win) return;
    int w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(win), &w, &h);
    gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *cfg = g_build_filename(dir, "qynote-win.conf", NULL);
    gchar *txt = g_strdup_printf("%d %d\n", w, h);
    g_file_set_contents(cfg, txt, -1, NULL);
    g_free(txt); g_free(cfg); g_free(dir);
}

static void on_destroy(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    save_win_size();
    gtk_main_quit();
}

static void load_win_size(void) {
    if (!win) return;
    gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
    gchar *cfg = g_build_filename(dir, "qynote-win.conf", NULL);
    gchar *contents = NULL;
    if (g_file_get_contents(cfg, &contents, NULL, NULL)) {
        int w = 0, h = 0;
        if (sscanf(contents, "%d %d", &w, &h) == 2 && w >= 560 && h >= 400)
            gtk_window_resize(GTK_WINDOW(win), w, h);
    }
    g_free(contents); g_free(cfg); g_free(dir);
}

/* 自动化: 每 60 秒自动保存当前笔记（release 280） */
static gboolean auto_save(gpointer p) {
    (void)p;
    if (current_path) save_current_note();
    return G_SOURCE_CONTINUE;
}

/* 自动化: 延迟新建（GSourceFunc 包装） */
static gboolean auto_new(gpointer p) {
    (void)p;
    on_new_clicked(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* ---------- 实时字数 / 行数统计（release 283） ---------- */
static void update_count(GtkTextBuffer *b, gpointer ud) {
    (void)b; (void)ud;
    if (!count_lb || !textview) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(textview);
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    long chars = txt ? g_utf8_strlen(txt, -1) : 0;
    int lines = gtk_text_buffer_get_line_count(buf);
    gchar *st = g_strdup_printf("%ld %s · %d %s", chars, TR("字"), lines, TR("行"));
    gtk_label_set_text(GTK_LABEL(count_lb), st);
    g_free(st);
    g_free(txt);
}

/* ---------- UI ---------- */
/* 排序开关：按修改时间 / 按名称（release 293） */
static void on_sort_toggled(GtkWidget *w, gpointer ud) {
    (void)ud;
    sort_mtime = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    gchar *path = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qynote-sort.conf", NULL);
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(path, sort_mtime ? "1" : "0", -1, NULL);
    g_free(path);
    refresh_note_list();
    if (status_lb)
        gtk_label_set_text(GTK_LABEL(status_lb),
                           sort_mtime ? TR("已按修改时间排序（新在前）") : TR("已按名称排序"));
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("笔记"));
    /* 拖放 .txt 文件到窗口打开（release 281） */
    {
        GtkTargetEntry te = { (gchar *)"text/uri-list", 0, 0 };
        gtk_drag_dest_set(win, GTK_DEST_DEFAULT_ALL, &te, 1, GDK_ACTION_COPY);
        g_signal_connect(win, "drag-data-received", G_CALLBACK(on_win_drag_recv), NULL);
    }
    gtk_widget_set_name(win, "qy-note-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("笔记"));
    qy_window_setup(GTK_WINDOW(win), 680, 460, 560, 400, TRUE);
    load_win_size();
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶栏：新建 / 删除 / 保存 + 状态 */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_new = gtk_button_new_with_label(TR("新建"));
    qy_add_class(b_new, "qy-btn");
    g_signal_connect(b_new, "clicked", G_CALLBACK(on_new_clicked), NULL);
    GtkWidget *b_del = gtk_button_new_with_label(TR("删除"));
    qy_add_class(b_del, "qy-btn qy-btn-danger");
    g_signal_connect(b_del, "clicked", G_CALLBACK(on_delete_clicked), NULL);
    GtkWidget *b_save = gtk_button_new_with_label(TR("保存"));
    qy_add_class(b_save, "qy-btn");
    g_signal_connect(b_save, "clicked", G_CALLBACK(on_save_clicked), NULL);
    GtkWidget *b_exp = gtk_button_new_with_label(TR("导出"));
    qy_add_class(b_exp, "qy-btn");
    g_signal_connect(b_exp, "clicked", G_CALLBACK(on_export_clicked), NULL);
    b_sort = gtk_toggle_button_new_with_label(TR("按时间排序"));
    qy_add_class(b_sort, "qy-btn");
    g_signal_connect(b_sort, "toggled", G_CALLBACK(on_sort_toggled), NULL);
    status_lb = gtk_label_new("");
    qy_add_class(status_lb, "qy-note-status");
    gtk_widget_set_halign(status_lb, GTK_ALIGN_START);
    count_lb = gtk_label_new("");
    qy_add_class(count_lb, "qy-note-count");
    gtk_box_pack_start(GTK_BOX(toolbar), b_new, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_exp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_sort, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), status_lb, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), count_lb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
    /* release 293: 恢复排序偏好 */
    {
        gchar *sp = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qynote-sort.conf", NULL);
        gchar *sv = NULL;
        if (g_file_get_contents(sp, &sv, NULL, NULL) && sv && sv[0] == '1')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_sort), TRUE);
        g_free(sv);
        g_free(sp);
    }

    /* 主体：左列表 + 右编辑区 */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    /* 左侧：搜索框 + 笔记列表 */
    GtkWidget *left_v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索笔记..."));
    gtk_widget_set_size_request(search_entry, 190, -1);
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(left_v), search_entry, FALSE, FALSE, 0);

    stack = gtk_stack_new();
    gtk_widget_set_size_request(stack, 200, -1);

    GtkWidget *sw_list = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw_list), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_set_name(GTK_WIDGET(listbox), "qy-note-list");
    g_signal_connect(listbox, "row-selected", G_CALLBACK(on_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(sw_list), GTK_WIDGET(listbox));
    gtk_stack_add_named(GTK_STACK(stack), sw_list, "list");

    placeholder = gtk_label_new(TR("无笔记\n点击「新建」创建"));
    gtk_label_set_justify(GTK_LABEL(placeholder), GTK_JUSTIFY_CENTER);
    qy_add_class(placeholder, "qy-note-placeholder");
    gtk_stack_add_named(GTK_STACK(stack), placeholder, "empty");

    gtk_box_pack_start(GTK_BOX(left_v), stack, TRUE, TRUE, 0);

    GtkWidget *sw_text = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw_text), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    textview = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_widget_set_name(GTK_WIDGET(textview), "qy-note-text");
    gtk_text_view_set_wrap_mode(textview, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(textview, 10);
    gtk_text_view_set_right_margin(textview, 10);
    gtk_text_view_set_top_margin(textview, 8);
    gtk_container_add(GTK_CONTAINER(sw_text), GTK_WIDGET(textview));

    /* 实时字数统计（release 283） */
    {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(textview);
        g_signal_connect(buf, "changed", G_CALLBACK(update_count), NULL);
        update_count(buf, NULL);
    }

    gtk_box_pack_start(GTK_BOX(hbox), left_v, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sw_text, TRUE, TRUE, 0);

    gtk_widget_show_all(win);

    /* 加载已有笔记 */
    refresh_note_list();

    /* 自动化: QYNOTE_NEW=1 启动后自动新建 */
    if (g_getenv("QYNOTE_NEW"))
        g_timeout_add(600, auto_new, NULL);

    g_timeout_add(60000, auto_save, NULL);   /* 自动保存（release 280） */
    gtk_main();
    return 0;
}