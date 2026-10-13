/* qyfiles - 澜岫文件管理器 (GTK3) — v4: 目录搜索过滤 */
#include "qyl10n.h"
#include "qyicon.h"
#include "qytheme.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib/gstdio.h>
#include <sys/wait.h>

static GtkWidget *view;
static GtkWidget *status;
static GtkWidget *path_entry = NULL;  /* 路径地址栏 */
static char cwd[4096];
static int in_trash = 0;   /* 当前是否处于回收站页 */
static int g_argc = 0;     /* main 传下: --trash 检测用 */
static char **g_argv = NULL;
static char g_search[128] = "";  /* 当前搜索关键字（空=全部） */
static char g_ftype[16] = "";   /* 类型过滤（release 264：空/all=全部；image/doc/audio/video/archive） */
static GSList *cut_sources = NULL; /* 剪切源列表（多选） */
static void chdir_to(const char *path);
static void chdir_trash(void);
static GdkPixbuf *load_icon_for(const char *name, gboolean isdir);

/* 图标视图/空状态（定义在下方，chdir_to/chdir_trash 提前引用） */
static GtkWidget *icon_view = NULL;
static GtkWidget *stack = NULL;
static gboolean icon_mode = FALSE;  /* 当前视图: FALSE=列表 TRUE=图标 */
static GtkWidget *empty_lb = NULL; /* 空状态文字 */

/* ---------- 多标签（GtkNotebook，#qyfiles-tabs） ---------- */
typedef struct {
    char path[4096];       /* 当前目录 */
    int in_trash;          /* 是否回收站视图 */
    char search[128];      /* 本标签搜索关键字 */
    GtkWidget *view;       /* 列表视图 */
    GtkWidget *icon_view;  /* 图标视图 */
    GtkWidget *stack;      /* list/icon/empty 切换 */
    GtkWidget *empty_lb;   /* 空状态文字（本标签） */
    GtkWidget *label;      /* 标签页标题（basename） */
} QyFilesTab;

static GtkWidget *notebook = NULL;
static QyFilesTab *cur_tab = NULL;
static GtkWidget *search_entry = NULL; /* 搜索框（各标签共享控件，文本随标签切换） */

static void update_tab_label(void);

/* v2.0 主题 + 工具栏状态按钮 */
static void load_theme(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    if (gtk_css_provider_load_from_path(p, "/usr/share/themes/lanxiu/gtk-3.0/gtk.css", NULL)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    g_object_unref(p);
}

static void add_class(GtkWidget *w, const char *cls) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* 工具栏按钮（按 普通目录 / 回收站 切换可见性） */
static GtkWidget *b_mk = NULL, *b_del = NULL, *b_ren = NULL;
static GtkWidget *b_nf = NULL;   /* 新建文件按钮 */
static GtkWidget *b_term = NULL; /* 在终端打开按钮 */
static GtkWidget *b_open = NULL;
static GtkWidget *b_cp = NULL, *b_prop = NULL;
static GtkWidget *b_res = NULL, *b_pur = NULL, *b_emp = NULL;
static GtkWidget *b_bm = NULL;   /* 添加书签按钮（release 256） */
static GtkWidget *book_list = NULL; /* 侧边栏书签 GtkListBox */

/* 网络挂载（release: SMB/CIFS + SSHFS） */
static GtkWidget *b_mount = NULL;       /* 工具栏「挂载」按钮 */
static GtkWidget *mount_type_combo = NULL;
static GtkWidget *mount_server_entry = NULL;
static GtkWidget *mount_share_entry = NULL;
static GtkWidget *mount_user_entry = NULL;
static GtkWidget *mount_pass_entry = NULL;
static GtkWidget *mount_point_entry = NULL;
static GtkWidget *mount_status = NULL;   /* #qyfiles-mount-status */
static GtkWidget *mount_share_lbl = NULL;
static gboolean mount_point_auto = TRUE; /* 挂载点未被手动编辑时自动跟随服务器 */

/* 预览面板（release 258） */
static GtkWidget *preview_revealer = NULL;  /* 右侧 GtkRevealer */
static GtkWidget *preview_box = NULL;        /* 预览内容容器 */
static GtkWidget *b_preview = NULL;          /* 工具栏「预览」开关 */

static void set_mode_buttons(void) {
    if (!b_mk) return;
    gboolean trash = in_trash;
    gtk_widget_set_visible(b_mk, !trash);
    gtk_widget_set_visible(b_nf, !trash);
    gtk_widget_set_visible(b_term, !trash);
    gtk_widget_set_visible(b_del, !trash);
    gtk_widget_set_visible(b_ren, !trash);
    gtk_widget_set_visible(b_open, !trash);
    gtk_widget_set_visible(b_cp, !trash);
    gtk_widget_set_visible(b_prop, !trash);
    gtk_widget_set_visible(b_res, trash);
    gtk_widget_set_visible(b_pur, trash);
    gtk_widget_set_visible(b_emp, trash);
    gtk_widget_set_visible(b_bm, !trash);
    gtk_widget_set_visible(b_mount, !trash);
}

/* 把当前标签的目录/回收站/搜索状态回写到 cur_tab */
static void save_cur_tab(void) {
    if (!cur_tab) return;
    g_strlcpy(cur_tab->path, cwd, sizeof cur_tab->path);
    cur_tab->in_trash = in_trash;
    g_strlcpy(cur_tab->search, g_search, sizeof cur_tab->search);
}

/* ---------- freedesktop Trash 规范: ~/.local/share/Trash/files + info ---------- */
static void trash_dir(char *files, size_t fl, char *info, size_t il) {
    const char *home = g_get_home_dir();
    if (files && fl) snprintf(files, fl, "%s/.local/share/Trash/files", home);
    if (info && il) snprintf(info, il, "%s/.local/share/Trash/info", home);
    if (files && fl) g_mkdir_with_parents(files, 0700);
    if (info && il) g_mkdir_with_parents(info, 0700);
}

/* 同名原子命名: name.txt → name.2.txt */
static void unique_trash_name(const char *files, const char *name, char *out, size_t ol) {
    snprintf(out, ol, "%s/%s", files, name);
    if (g_file_test(out, G_FILE_TEST_EXISTS)) {
        const char *dot = strrchr(name, '.');
        for (int i = 2; ; i++) {
            if (dot && dot != name) {
                char base[512]; size_t bl = dot - name;
                if (bl >= sizeof base) bl = sizeof base - 1;
                memcpy(base, name, bl); base[bl] = 0;
                snprintf(out, ol, "%s/%s.%d%s", files, base, i, dot);
            } else {
                snprintf(out, ol, "%s/%s.%d", files, name, i);
            }
            if (!g_file_test(out, G_FILE_TEST_EXISTS)) break;
        }
    }
}

static gboolean trash_file(const char *path, GError **err) {
    char files[4096], info[4096], tname[4096], tinfo[4096];
    trash_dir(files, sizeof files, info, sizeof info);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    unique_trash_name(files, name, tname, sizeof tname);
    /* mv 到 files/ */
    gchar *cmd = g_strdup_printf("mv -b -- %s %s 2>/dev/null", path, tname);
    int rc = system(cmd);
    g_free(cmd);
    if (rc != 0) { g_set_error(err, 0, 1, "move failed"); return FALSE; }
    /* 写 .trashinfo */
    const char *bn = strrchr(tname, '/') + 1;
    snprintf(tinfo, sizeof tinfo, "%s/%s.trashinfo", info, bn);
    GDateTime *now = g_date_time_new_now_local();
    gchar *iso = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
    FILE *f = fopen(tinfo, "w");
    if (f) {
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", path, iso);
        fclose(f);
    }
    g_date_time_unref(now); g_free(iso);
    return TRUE;
}

/* 读取 .trashinfo 的 Path= 与 DeletionDate= 行（path/date 可空） */
static gboolean read_trashinfo2(const char *tinfo, char *path, size_t pl,
                                char *date, size_t dl) {
    FILE *f = fopen(tinfo, "r");
    if (!f) return FALSE;
    char line[4096];
    gboolean ok = FALSE;
    if (path && pl) path[0] = 0;
    if (date && dl) date[0] = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "Path=", 5)) {
            /* 规范: Path 为 URL 编码, 这里按字面处理(%20 之外的常见场景) */
            if (path && pl) g_strlcpy(path, line + 5, pl);
            ok = TRUE;
        } else if (!strncmp(line, "DeletionDate=", 13)) {
            if (date && dl) g_strlcpy(date, line + 13, dl);
        }
    }
    fclose(f);
    return ok;
}

/* 读取 .trashinfo 的 Path= 行 */
static gboolean read_trashinfo(const char *tinfo, char *out, size_t ol) {
    return read_trashinfo2(tinfo, out, ol, NULL, 0);
}

/* ---------- 操作实现 ---------- */
static void selected_name(gchar **name) {
    *name = NULL;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) return;
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    gtk_tree_model_get(m, &it, 1, name, -1);
}

/* 多选：收集所有选中文件名到 GSList（调用者 g_slist_free_full(names, g_free)） */
static GSList *selected_names(void) {
    GSList *list = NULL;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &m);
    for (GList *r = rows; r; r = r->next) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(m, &it, r->data)) {
            gchar *name = NULL;
            gtk_tree_model_get(m, &it, 1, &name, -1);
            if (name) list = g_slist_prepend(list, name);
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return g_slist_reverse(list);
}

static void do_delete(void) {
    GSList *names = selected_names();
    if (!names) return;
    if (in_trash) { g_slist_free_full(names, g_free); return; }
    for (GSList *l = names; l; l = l->next) {
        const char *name = (const char *)l->data;
        gchar *full = g_build_filename(cwd, name, NULL);
        GError *err = NULL;
        if (!trash_file(full, &err)) {
            gchar *msg = g_strdup_printf(TR("删除失败: %s"), err ? err->message : "?");
            gtk_label_set_text(GTK_LABEL(status), msg);
            g_free(msg); g_clear_error(&err);
        }
        g_free(full);
    }
    g_slist_free_full(names, g_free);
    chdir_to(cwd);
}

/* 彻底删除 (Shift+Delete 语义): 回收站内或普通目录 rm -rf */
static void do_purge(void) {
    gchar *name; selected_name(&name);
    if (!name) return;
    gchar *full = in_trash
        ? g_build_filename(g_get_home_dir(), ".local/share/Trash/files", name, NULL)
        : g_build_filename(cwd, name, NULL);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(view)),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
        TR("彻底删除 \"%s\"? 不可恢复!"), name);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gchar *cmd = g_strdup_printf("rm -rf -- %s", full);
        int rc = system(cmd);
        g_free(cmd);
        gtk_label_set_text(GTK_LABEL(status), rc == 0 ? TR("已彻底删除") : TR("删除失败"));
        if (in_trash) {
            char info[4096];
            trash_dir(NULL, 0, info, sizeof info);
            gchar *ti = g_strdup_printf("%s/%s.trashinfo", info, name);
            g_unlink(ti); g_free(ti);
        }
    }
    gtk_widget_destroy(dlg);
    g_free(name); g_free(full);
    if (in_trash) chdir_trash(); else chdir_to(cwd);
}

/* 还原: files/<name> → info 的 Path, 目标已存在则加 .restored 后缀 */
static void do_restore(void) {
    gchar *name; selected_name(&name);
    if (!name) return;
    char info[4096], orig[4096];
    trash_dir(NULL, 0, info, sizeof info);
    gchar *ti = g_strdup_printf("%s/%s.trashinfo", info, name);
    if (!read_trashinfo(ti, orig, sizeof orig)) {
        gtk_label_set_text(GTK_LABEL(status), TR("还原失败: 无元数据"));
        g_free(name); g_free(ti); return;
    }
    if (g_file_test(orig, G_FILE_TEST_EXISTS)) {
        gchar *alt = g_strdup_printf("%s.restored", orig);
        g_strlcpy(orig, alt, sizeof orig);
        g_free(alt);
    }
    gchar *src = g_build_filename(g_get_home_dir(), ".local/share/Trash/files", name, NULL);
    /* 用 g_rename 直接移回原路径（同文件系统原子操作） */
    if (g_rename(src, orig) == 0) {
        g_unlink(ti);
        gchar *msg = g_strdup_printf(TR("已还原到 %s"), orig);
        gtk_label_set_text(GTK_LABEL(status), msg);
        g_free(msg);
    } else {
        gtk_label_set_text(GTK_LABEL(status), TR("还原失败"));
    }
    g_free(ti);
    g_free(src);
    g_free(name);
    chdir_trash();
}

/* 清空回收站 (二次确认) */
static void do_empty_trash(void) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(view)),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
        TR("清空回收站? 全部内容不可恢复!"));
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char files[4096], info[4096];
        trash_dir(files, sizeof files, info, sizeof info);
        gchar *c1 = g_strdup_printf("rm -rf -- %s/* %s/*", files, info);
        system(c1); g_free(c1);
        gtk_label_set_text(GTK_LABEL(status), TR("回收站已清空"));
    }
    gtk_widget_destroy(dlg);
    if (in_trash) chdir_trash();
}

static void do_rename(void) {
    gchar *name; selected_name(&name);
    if (!name) return;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("重命名", GTK_WINDOW(gtk_widget_get_toplevel(view)),
        GTK_DIALOG_MODAL, "_取消", GTK_RESPONSE_CANCEL, "_确定", GTK_RESPONSE_OK, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), name);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("新名称:"), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *nn = gtk_entry_get_text(GTK_ENTRY(entry));
        if (nn[0] && strcmp(nn, name) != 0) {
            gchar *oldf = g_build_filename(cwd, name, NULL);
            gchar *newf = g_build_filename(cwd, nn, NULL);
            gchar *cmd = g_strdup_printf("mv -b -- %s %s", oldf, newf);
            if (system(cmd) == 0) gtk_label_set_text(GTK_LABEL(status), "重命名完成");
            g_free(cmd); g_free(oldf); g_free(newf);
        }
    }
    gtk_widget_destroy(dlg);
    g_free(name);
    chdir_to(cwd);
}

static void do_mkdir(void) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons("新建文件夹", GTK_WINDOW(gtk_widget_get_toplevel(view)),
        GTK_DIALOG_MODAL, "_取消", GTK_RESPONSE_CANCEL, "_确定", GTK_RESPONSE_OK, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "新建文件夹");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 6);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *nn = gtk_entry_get_text(GTK_ENTRY(entry));
        if (nn[0]) {
            gchar *full = g_build_filename(cwd, nn, NULL);
            if (g_mkdir(full, 0755) == 0) gtk_label_set_text(GTK_LABEL(status), "文件夹已创建");
            else gtk_label_set_text(GTK_LABEL(status), "创建失败");
            g_free(full);
        }
    }
    gtk_widget_destroy(dlg);
    chdir_to(cwd);
}

/* 新建文本文件（默认 新建文件.txt，可改名） */
static void do_newfile(void) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons("新建文件", GTK_WINDOW(gtk_widget_get_toplevel(view)),
        GTK_DIALOG_MODAL, "_取消", GTK_RESPONSE_CANCEL, "_确定", GTK_RESPONSE_OK, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "新建文件.txt");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 6);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *nn = gtk_entry_get_text(GTK_ENTRY(entry));
        if (nn[0]) {
            gchar *full = g_build_filename(cwd, nn, NULL);
            if (g_file_set_contents(full, "", 0, NULL) == TRUE)
                gtk_label_set_text(GTK_LABEL(status), "文件已创建");
            else
                gtk_label_set_text(GTK_LABEL(status), "创建失败");
            g_free(full);
        }
    }
    gtk_widget_destroy(dlg);
    chdir_to(cwd);
}

/* 自动化: QYFILES_NEWFILE=1 启动后直接创建未命名.txt（跳过对话框） */
static gboolean auto_newfile(gpointer p) {
    (void)p;
    gchar *full = g_build_filename(cwd, "未命名.txt", NULL);
    if (g_file_set_contents(full, "", 0, NULL))
        g_printerr("QYFILESDBG: newfile created %s\n", full);
    else
        g_printerr("QYFILESDBG: newfile failed\n");
    g_free(full);
    chdir_to(cwd);
    return G_SOURCE_REMOVE;
}

/* 在终端中打开（weston-terminal 启动默认终端，目录为当前用户主目录） */
static void do_open_terminal(void) {
    g_printerr("QYFILESDBG: terminal spawn cwd=%s\n", cwd);
    if (!g_spawn_command_line_async("weston-terminal", NULL))
        g_printerr("QYFILESDBG: terminal spawn failed\n");
}

/* 自动化: QYFILES_TERM=1 启动后在当前目录打开终端 */
static gboolean auto_terminal(gpointer p) {
    (void)p;
    do_open_terminal();
    return G_SOURCE_REMOVE;
}

/* 复制选中文件为副本（"名字 副本.ext" / 已存在则 副本.2.ext） */
static void do_copy_named(const char *name) {
    if (!name || !name[0]) return;
    const char *dot = strrchr(name, '.');
    const char *suffix = TR("副本");
    gchar *dst = NULL;
    if (dot && dot != name) {
        gchar *base = g_strndup(name, dot - name);
        dst = g_strdup_printf("%s%s%s", base, suffix, dot);
        g_free(base);
    } else {
        dst = g_strdup_printf("%s%s", name, suffix);
    }
    if (g_file_test(dst, G_FILE_TEST_EXISTS)) {
        for (int i = 2; ; i++) {
            g_free(dst); dst = NULL;
            if (dot && dot != name) {
                gchar *base = g_strndup(name, dot - name);
                dst = g_strdup_printf("%s%s.%d%s", base, suffix, i, dot);
                g_free(base);
            } else {
                dst = g_strdup_printf("%s%s.%d", name, suffix, i);
            }
            if (!g_file_test(dst, G_FILE_TEST_EXISTS)) break;
        }
    }
    gchar *src_full = g_build_filename(cwd, name, NULL);
    gchar *dst_full = g_build_filename(cwd, dst, NULL);
    gchar *cmd = g_strdup_printf("cp -r -- %s %s", g_shell_quote(src_full), g_shell_quote(dst_full));
    int rc = system(cmd);
    g_free(cmd); g_free(src_full); g_free(dst_full);
    if (rc == 0) {
        gchar *msg = g_strdup_printf("%s: %s", TR("复制成功"), dst);
        gtk_label_set_text(GTK_LABEL(status), msg);
        g_free(msg);
        if (in_trash) chdir_trash(); else chdir_to(cwd);
    } else {
        gtk_label_set_text(GTK_LABEL(status), TR("复制失败"));
    }
    g_free(dst);
}

static gboolean auto_copy(gpointer p) {
    do_copy_named((const char *)p);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void do_copy(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GSList *names = selected_names();
    if (!names) { gtk_label_set_text(GTK_LABEL(status), TR("请先选择一个文件")); return; }
    for (GSList *l = names; l; l = l->next)
        do_copy_named((const char *)l->data);
    g_slist_free_full(names, g_free);
    chdir_to(cwd);
}

/* Ctrl+A 全选当前目录 */
static void on_select_all(GtkAccelGroup *ag, GObject *a, guint keyval,
                          GdkModifierType mod, gpointer ud) {
    (void)ag; (void)a; (void)keyval; (void)mod;
    gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)));
    g_printerr("QYFILESDBG: select all\n");
}

/* ---------- 剪切 / 粘贴（移动文件） ---------- */
static void do_cut(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GSList *names = selected_names();
    if (!names) { gtk_label_set_text(GTK_LABEL(status), TR("请先选择一个文件")); return; }
    g_slist_free_full(cut_sources, g_free);
    cut_sources = NULL;
    int n = 0;
    for (GSList *l = names; l; l = l->next) {
        cut_sources = g_slist_prepend(cut_sources, g_build_filename(cwd, (const char *)l->data, NULL));
        n++;
    }
    cut_sources = g_slist_reverse(cut_sources);
    g_printerr("QYFILESDBG: cut %d items\n", n);
    gchar *msg = g_strdup_printf(TR("已剪切 %d 项"), n);
    gtk_label_set_text(GTK_LABEL(status), msg);
    g_free(msg);
    g_slist_free_full(names, g_free);
}

static void do_paste(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!cut_sources) {
        gtk_label_set_text(GTK_LABEL(status), TR("没有可粘贴的剪切项"));
        return;
    }
    if (in_trash) {
        gtk_label_set_text(GTK_LABEL(status), TR("回收站中不能粘贴"));
        return;
    }
    int ok = 0, fail = 0;
    for (GSList *l = cut_sources; l; l = l->next) {
        const gchar *src = (const gchar *)l->data;
        gchar *base = g_path_get_basename(src);
        gchar *dst = g_build_filename(cwd, base, NULL);
        g_free(base);
        gchar *cmd = g_strdup_printf("mv -b -- %s %s", g_shell_quote(src), g_shell_quote(dst));
        int rc = system(cmd);
        g_free(cmd);
        if (rc == 0) ok++; else fail++;
        g_free(dst);
    }
    if (fail == 0) {
        gtk_label_set_text(GTK_LABEL(status), TR("已粘贴"));
        g_slist_free_full(cut_sources, g_free);
        cut_sources = NULL;
        chdir_to(cwd);
    } else {
        gchar *msg = g_strdup_printf(TR("粘贴失败 %d 项"), fail);
        gtk_label_set_text(GTK_LABEL(status), msg);
        g_free(msg);
    }
}

/* ---------- 属性对话框: 名称 / 路径 / 类型 / 大小 / 修改时间 / 权限 ---------- */
/* 权限位 → rwx 字符串（如 "rwxr-xr-x"） */
static void perm_str(mode_t m, char *out, size_t n) {
    static const char rwx[] = "rwxrwxrwx";
    if (n < 10) { if (n) out[0] = 0; return; }
    for (int i = 0; i < 9; i++)
        out[i] = (m & (0400 >> i)) ? rwx[i] : '-';
    out[9] = 0;
}

static void do_prop_name(const char *name) {
    if (!name || !name[0]) return;
    gchar *full = g_build_filename(cwd, name, NULL);
    struct stat st;
    if (stat(full, &st) != 0) { g_free(full); return; }
    char size_buf[64], mtime_buf[64], perm_buf[64], type_buf[64];
    gboolean isdir = S_ISDIR(st.st_mode);

    /* 类型: 目录 / 按扩展名 */
    if (isdir) {
        g_strlcpy(type_buf, TR("文件夹"), sizeof type_buf);
    } else {
        const char *dot = strrchr(name, '.');
        if (dot && dot != name)
            g_snprintf(type_buf, sizeof type_buf, "%s (%s)", TR("文件"), dot);
        else
            g_strlcpy(type_buf, TR("文件"), sizeof type_buf);
    }

    /* 大小 */
    if (isdir)
        g_snprintf(size_buf, sizeof size_buf, "%s", TR("—"));
    else
        g_snprintf(size_buf, sizeof size_buf, "%lld %s",
                   (long long)st.st_size, TR("字节"));

    /* 修改时间 (localtime) */
    struct tm *tm = localtime(&st.st_mtime);
    if (tm) strftime(mtime_buf, sizeof mtime_buf, "%Y-%m-%d %H:%M:%S", tm);
    else g_strlcpy(mtime_buf, "-", sizeof mtime_buf);

    /* 权限: 八进制 + rwx 字符串 */
    char rwxs[16];
    perm_str(st.st_mode & 07777, rwxs, sizeof rwxs);
    g_snprintf(perm_buf, sizeof perm_buf, "%04o  %s", st.st_mode & 07777, rwxs);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(TR("属性"),
        GTK_WINDOW(gtk_widget_get_toplevel(view)), GTK_DIALOG_MODAL,
        "_确定", GTK_RESPONSE_OK, NULL);
    gtk_widget_set_name(dlg, "qy-files-prop");
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    const char *rows[][2] = {
        { TR("名称"), name },
        { TR("路径"), full },
        { TR("类型"), type_buf },
        { TR("大小"), size_buf },
        { TR("修改时间"), mtime_buf },
        { TR("权限"), perm_buf },
    };
    for (int i = 0; i < 6; i++) {
        GtkWidget *k = gtk_label_new(rows[i][0]);
        gtk_widget_set_halign(k, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), k, 0, i, 1, 1);
        GtkWidget *v = gtk_label_new(rows[i][1]);
        gtk_widget_set_halign(v, GTK_ALIGN_START);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_grid_attach(GTK_GRID(grid), v, 1, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 10);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(full);
}

static gboolean auto_prop(gpointer p) {
    do_prop_name((const char *)p);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void do_prop(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    gchar *name;
    selected_name(&name);
    if (!name) { gtk_label_set_text(GTK_LABEL(status), TR("请先选择一个文件")); return; }
    do_prop_name(name);
    g_free(name);
}

/* ---------- 侧边栏导航 ---------- */
static void on_side(GtkButton *b, gpointer ud) {
    const char *p = (const char *)ud;
    if (!strcmp(p, "TRASH")) chdir_trash();
    else chdir_to(p);
}

static GtkWidget *side_button(const char *label, const char *target, GtkWidget *box) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    add_class(b, "qy-files-side");
    char *t = g_strdup(target);
    g_signal_connect(b, "clicked", G_CALLBACK(on_side), t);
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 1);
    return b;
}

/* ---------- 侧边栏书签（release 256） ---------- */
static gboolean bookmark_exists(const char *path) {
    if (!book_list || !path) return FALSE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(book_list));
    for (GList *l = rows; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "path");
        if (p && !strcmp(p, path)) { g_list_free(rows); return TRUE; }
    }
    g_list_free(rows);
    return FALSE;
}

static GtkListBoxRow *add_bookmark_row(const char *path, const char *label) {
    if (!path || !path[0] || !book_list) return NULL;
    if (bookmark_exists(path)) return NULL;
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *lbl = gtk_label_new(label && label[0] ? label : path);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(lbl, 6);
    gtk_widget_set_margin_end(lbl, 6);
    gtk_widget_set_margin_top(lbl, 3);
    gtk_widget_set_margin_bottom(lbl, 3);
    gtk_container_add(GTK_CONTAINER(row), lbl);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
    gtk_list_box_insert(GTK_LIST_BOX(book_list), row, -1);
    return GTK_LIST_BOX_ROW(row);
}

static void remove_book_row(const char *path) {
    if (!book_list || !path) return;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(book_list));
    for (GList *l = rows; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "path");
        if (p && !strcmp(p, path)) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
            break;
        }
    }
    g_list_free(rows);
}

/* 追加当前路径到书签文件并加入侧边栏 */
static void add_bookmark_path(const char *path) {
    if (!path || !path[0]) return;
    if (bookmark_exists(path)) {
        gtk_label_set_text(GTK_LABEL(status), TR("该书签已存在"));
        return;
    }
    gchar *dir = g_build_filename(g_get_home_dir(), ".config/lanxiu", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *file = g_build_filename(dir, "bookmarks.txt", NULL);
    GString *out = g_string_new(NULL);
    gchar *content = NULL;
    if (g_file_get_contents(file, &content, NULL, NULL) && content)
        g_string_append(out, content);
    g_string_append_printf(out, "%s\n", path);
    g_file_set_contents(file, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    g_free(content);
    g_free(file);
    g_free(dir);
    add_bookmark_row(path, path);
    gtk_label_set_text(GTK_LABEL(status), TR("已添加书签"));
}

static void do_add_bookmark(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (in_trash) {
        gtk_label_set_text(GTK_LABEL(status), TR("回收站不能添加书签"));
        return;
    }
    add_bookmark_path(cwd);
}

/* 左键进入书签目录 */
static void on_book_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    const char *path = row ? g_object_get_data(G_OBJECT(row), "path") : NULL;
    if (path && path[0]) chdir_to(path);
}

/* 移除书签：从文件与列表同时删除 */
static void on_book_remove(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    const char *path = (const char *)ud;
    if (!path || !path[0]) { g_free(ud); return; }
    gchar *file = g_build_filename(g_get_home_dir(), ".config/lanxiu/bookmarks.txt", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(file, &content, NULL, NULL) && content) {
        GString *out = g_string_new(NULL);
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (line[0] && strcmp(line, path) != 0)
                g_string_append_printf(out, "%s\n", line);
        }
        g_strfreev(lines);
        g_file_set_contents(file, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    g_free(content);
    g_free(file);
    remove_book_row(path);
    gtk_label_set_text(GTK_LABEL(status), TR("已移除书签"));
    g_free(ud);
}

/* 书签行右键：移除书签 */
static gboolean on_book_popup(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(book_list), (gint)ev->y);
    if (!row) return TRUE;
    gtk_list_box_select_row(GTK_LIST_BOX(book_list), row);
    const char *path = g_object_get_data(G_OBJECT(row), "path");
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("移除书签"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_book_remove), g_strdup(path ? path : ""));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
    return TRUE;
}

/* 加载默认书签（XDG 用户目录或 ~ 常见目录）+ 用户书签文件 */
static void load_bookmarks(void) {
    const char *home = g_get_home_dir();
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    const char *dl   = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    const char *mus  = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    const char *vids = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    gchar *fb[5] = { NULL, NULL, NULL, NULL, NULL };
    if (!docs) { fb[0] = g_build_filename(home, "文档", NULL); docs = fb[0]; }
    if (!dl)   { fb[1] = g_build_filename(home, "下载", NULL); dl = fb[1]; }
    if (!pics) { fb[2] = g_build_filename(home, "图片", NULL); pics = fb[2]; }
    if (!mus)  { fb[3] = g_build_filename(home, "音乐", NULL); mus = fb[3]; }
    if (!vids) { fb[4] = g_build_filename(home, "视频", NULL); vids = fb[4]; }
    const char *defaults[][2] = {
        { TR("主目录"), home },
        { TR("文档"), docs },
        { TR("下载"), dl },
        { TR("图片"), pics },
        { TR("音乐"), mus },
        { TR("视频"), vids },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(defaults); i++) {
        if (defaults[i][1] && defaults[i][1][0])
            add_bookmark_row(defaults[i][1], defaults[i][0]);
    }
    for (int i = 0; i < 5; i++) g_free(fb[i]);

    gchar *file = g_build_filename(home, ".config/lanxiu/bookmarks.txt", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(file, &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (line[0]) add_bookmark_row(line, line);
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(file);
}

/* ---------- 打开方式子菜单（release 256） ---------- */
static void do_open_with(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    char **pair = (char **)ud;
    gchar *cmd = g_strdup_printf("%s '%s' &", pair[0], pair[1]);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    g_free(pair[0]);
    g_free(pair[1]);
    g_free(pair);
}

/* 按扩展名匹配候选应用 qyview/qyedit/qymedia/qybrowser/qyarc */
static void add_open_with_menu(GtkWidget *menu) {
    gchar *name = NULL;
    selected_name(&name);
    if (!name) return;
    gchar *full = g_build_filename(cwd, name, NULL);
    const char *ext = strrchr(name, '.');
    const char *apps[8];
    int n = 0;
    if (ext) {
        if (!g_ascii_strcasecmp(ext, ".png") || !g_ascii_strcasecmp(ext, ".jpg") ||
            !g_ascii_strcasecmp(ext, ".jpeg") || !g_ascii_strcasecmp(ext, ".bmp") ||
            !g_ascii_strcasecmp(ext, ".gif") || !g_ascii_strcasecmp(ext, ".webp") ||
            !g_ascii_strcasecmp(ext, ".svg") || !g_ascii_strcasecmp(ext, ".ico"))
            apps[n++] = "qyview";
        if (!g_ascii_strcasecmp(ext, ".txt") || !g_ascii_strcasecmp(ext, ".md") ||
            !g_ascii_strcasecmp(ext, ".c") || !g_ascii_strcasecmp(ext, ".h") ||
            !g_ascii_strcasecmp(ext, ".py") || !g_ascii_strcasecmp(ext, ".sh") ||
            !g_ascii_strcasecmp(ext, ".js") || !g_ascii_strcasecmp(ext, ".ini") ||
            !g_ascii_strcasecmp(ext, ".conf") || !g_ascii_strcasecmp(ext, ".desktop") ||
            !g_ascii_strcasecmp(ext, ".log") || !g_ascii_strcasecmp(ext, ".xml"))
            apps[n++] = "qyedit";
        if (!g_ascii_strcasecmp(ext, ".mp3") || !g_ascii_strcasecmp(ext, ".wav") ||
            !g_ascii_strcasecmp(ext, ".flac") || !g_ascii_strcasecmp(ext, ".ogg") ||
            !g_ascii_strcasecmp(ext, ".mp4") || !g_ascii_strcasecmp(ext, ".mkv") ||
            !g_ascii_strcasecmp(ext, ".avi") || !g_ascii_strcasecmp(ext, ".webm"))
            apps[n++] = "qymedia";
        if (!g_ascii_strcasecmp(ext, ".html") || !g_ascii_strcasecmp(ext, ".htm") ||
            !g_ascii_strcasecmp(ext, ".xhtml"))
            apps[n++] = "qybrowser";
        if (!g_ascii_strcasecmp(ext, ".zip") || !g_ascii_strcasecmp(ext, ".tar") ||
            !g_ascii_strcasecmp(ext, ".gz") || !g_ascii_strcasecmp(ext, ".bz2") ||
            !g_ascii_strcasecmp(ext, ".xz") || !g_ascii_strcasecmp(ext, ".7z") ||
            !g_ascii_strcasecmp(ext, ".rar"))
            apps[n++] = "qyarc";
    }
    GtkWidget *sub = gtk_menu_new();
    if (n == 0) {
        GtkWidget *mi = gtk_menu_item_new_with_label(TR("无匹配应用"));
        gtk_widget_set_sensitive(mi, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
    } else {
        for (int i = 0; i < n; i++) {
            GtkWidget *mi = gtk_menu_item_new_with_label(apps[i]);
            char **pair = g_new(char *, 2);
            pair[0] = g_strdup(apps[i]);
            pair[1] = g_strdup(full);
            g_signal_connect(mi, "activate", G_CALLBACK(do_open_with), pair);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
    }
    GtkWidget *top = gtk_menu_item_new_with_label(TR("打开方式"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    g_free(full);
    g_free(name);
}

/* ---------- 右键菜单（真实 GtkMenu） ---------- */
static void do_open_sel(GtkButton *b, gpointer ud);

static void add_menuitem(GtkWidget *menu, const char *label, GCallback cb) {
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_signal_connect(mi, "activate", cb, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

/* 为当前选中行弹出上下文菜单（trigger 可为 NULL——自动化场景） */
static void show_context_menu(GdkEvent *trigger) {
    GtkWidget *menu = gtk_menu_new();
    if (in_trash) {
        add_menuitem(menu, TR("还原"), G_CALLBACK(do_restore));
        add_menuitem(menu, TR("彻底删除"), G_CALLBACK(do_purge));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        add_menuitem(menu, TR("清空回收站"), G_CALLBACK(do_empty_trash));
    } else {
        add_menuitem(menu, TR("打开"), G_CALLBACK(do_open_sel));
        add_open_with_menu(menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        add_menuitem(menu, TR("剪切"), G_CALLBACK(do_cut));
        add_menuitem(menu, TR("复制"), G_CALLBACK(do_copy));
        add_menuitem(menu, TR("粘贴"), G_CALLBACK(do_paste));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        add_menuitem(menu, TR("重命名"), G_CALLBACK(do_rename));
        add_menuitem(menu, TR("删除"), G_CALLBACK(do_delete));
        add_menuitem(menu, TR("属性"), G_CALLBACK(do_prop));
    }
    gtk_widget_show_all(menu);
    g_printerr("QYFILESDBG: context menu ready (%d items)\n", in_trash ? 4 : 7);
    /* Wayland 下必须将菜单 attach 到窗口，否则临时窗口无法定位/显示 */
    gtk_menu_attach_to_widget(GTK_MENU(menu), view, NULL);
    if (trigger)
        gtk_menu_popup_at_widget(GTK_MENU(menu), view,
            GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, trigger);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), view,
            GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static gboolean on_popup(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        GtkTreePath *path = NULL;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
        if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
                (gint)ev->x, (gint)ev->y, &path, NULL, NULL, NULL) || !path)
            return TRUE;
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
        gtk_tree_path_free(path);
        show_context_menu((GdkEvent *)ev);
        return TRUE;
    }
    return FALSE;
}

/* 自动化：QYFILES_MULTI=file1,file2 选中多行并触发剪切（自测多选） */
static gboolean auto_multi(gpointer p) {
    const char *csv = (const char *)p;
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    char *copy = g_strdup(csv);
    char **names = g_strsplit(copy, ",", 0);
    for (int i = 0; names && names[i]; i++) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter_first(m, &it)) {
            do {
                gchar *name = NULL;
                gtk_tree_model_get(m, &it, 1, &name, -1);
                if (name && strcmp(name, names[i]) == 0)
                    gtk_tree_selection_select_iter(sel, &it);
                g_free(name);
            } while (gtk_tree_model_iter_next(m, &it));
        }
    }
    g_strfreev(names);
    g_free(copy);
    g_free(p);
    do_cut(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* 自动化：QYFILES_RIGHTCLICK=文件名 选中该行并弹出右键菜单（自测截图用） */
static gboolean auto_rightclick(gpointer p) {
    const char *target = (const char *)p;
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(m, &it, 1, &name, -1);
            if (name && strcmp(name, target) == 0) {
                GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
                gtk_tree_selection_unselect_all(sel);
                gtk_tree_selection_select_iter(sel, &it);
                g_free(name);
                g_free(p);
                /* 伪造右键事件，使菜单有触发事件（Wayland 下必须） */
                GdkEventButton evt;
                memset(&evt, 0, sizeof evt);
                evt.type = GDK_BUTTON_PRESS;
                evt.button = 3;
                evt.x = 10;
                evt.y = 10;
                evt.time = GDK_CURRENT_TIME;
                evt.window = gtk_widget_get_window(view);
                if (evt.window) g_object_ref(evt.window);
                show_context_menu((GdkEvent *)&evt);
                if (evt.window) g_object_unref(evt.window);
                return G_SOURCE_REMOVE;
            }
            g_free(name);
        } while (gtk_tree_model_iter_next(m, &it));
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* 地址栏回车跳转目录 */
static void on_path_activate(GtkEntry *e, gpointer ud) {
    (void)ud;
    const char *p = gtk_entry_get_text(e);
    if (p && g_file_test(p, G_FILE_TEST_IS_DIR))
        chdir_to(p);
    else
        gtk_label_set_text(GTK_LABEL(status), TR("目录不存在"));
}

/* ---------- 类型过滤（release 264） ---------- */
static void on_refresh(GtkButton *b, gpointer ud);   /* 前向声明（定义在下方） */
static gboolean file_match_type(const char *name, const char *type) {
    if (!type || !type[0] || strcmp(type, "all") == 0) return TRUE;
    const char *ext = strrchr(name, '.');
    if (!ext) return FALSE;
    ext++;
    if (strcmp(type, "image") == 0)
        return g_ascii_strcasecmp(ext, "png") == 0 || g_ascii_strcasecmp(ext, "jpg") == 0 ||
               g_ascii_strcasecmp(ext, "jpeg") == 0 || g_ascii_strcasecmp(ext, "bmp") == 0 ||
               g_ascii_strcasecmp(ext, "webp") == 0;
    if (strcmp(type, "doc") == 0)
        return g_ascii_strcasecmp(ext, "txt") == 0 || g_ascii_strcasecmp(ext, "md") == 0 ||
               g_ascii_strcasecmp(ext, "pdf") == 0 || g_ascii_strcasecmp(ext, "doc") == 0 ||
               g_ascii_strcasecmp(ext, "docx") == 0;
    if (strcmp(type, "audio") == 0)
        return g_ascii_strcasecmp(ext, "mp3") == 0 || g_ascii_strcasecmp(ext, "wav") == 0 ||
               g_ascii_strcasecmp(ext, "ogg") == 0 || g_ascii_strcasecmp(ext, "flac") == 0;
    if (strcmp(type, "video") == 0)
        return g_ascii_strcasecmp(ext, "mp4") == 0 || g_ascii_strcasecmp(ext, "mkv") == 0 ||
               g_ascii_strcasecmp(ext, "webm") == 0 || g_ascii_strcasecmp(ext, "avi") == 0;
    if (strcmp(type, "archive") == 0)
        return g_ascii_strcasecmp(ext, "zip") == 0 || g_ascii_strcasecmp(ext, "tar") == 0 ||
               g_ascii_strcasecmp(ext, "gz") == 0 || g_ascii_strcasecmp(ext, "7z") == 0 ||
               g_ascii_strcasecmp(ext, "rar") == 0;
    return TRUE;
}

static void on_filter_type_changed(GtkComboBox *combo, gpointer ud) {
    (void)ud;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    g_strlcpy(g_ftype, id ? id : "all", sizeof g_ftype);
    on_refresh(NULL, NULL);
}

/* ---------- 导航 ---------- */
/* 启动目录：记住的上次目录，否则主目录（release 270） */
static const char *start_dir(void) {
    static char buf[4096];
    gchar *p = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyfiles-cwd", NULL);
    gchar *data = NULL;
    if (g_file_get_contents(p, &data, NULL, NULL) && data && data[0]) {
        g_strlcpy(buf, data, sizeof buf);
        g_free(data); g_free(p);
        if (g_file_test(buf, G_FILE_TEST_IS_DIR)) return buf;
        return g_get_home_dir();
    }
    g_free(data); g_free(p);
    return g_get_home_dir();
}

static void chdir_to(const char *path) {
    in_trash = 0;
    set_mode_buttons();
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(view)));
    gtk_list_store_clear(store);
    GDir *dir = g_dir_open(path, 0, NULL);
    gint n = 0, ndir = 0, nfile = 0;
    if (!dir) return;
    /* 记住上次目录（release 270） */
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        g_mkdir_with_parents(cdir, 0755);
        gchar *cpath = g_build_filename(cdir, "qyfiles-cwd", NULL);
        g_file_set_contents(cpath, path, -1, NULL);
        g_free(cpath); g_free(cdir);
    }
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
        /* 搜索过滤: 名称包含关键字才显示 */
        if (g_search[0] && !strstr(name, g_search))
            continue;
        gchar *full = g_build_filename(path, name, NULL);
        gboolean isdir = g_file_test(full, G_FILE_TEST_IS_DIR);
        /* 类型过滤（release 264） */
        if (g_ftype[0] && !isdir && !file_match_type(name, g_ftype))
            continue;
        char sizestr[32] = "—";
        if (!isdir) {
            struct stat st;
            if (stat(full, &st) == 0) {
                if (st.st_size >= 1024 * 1024)
                    g_snprintf(sizestr, sizeof sizestr, "%.1f MB", st.st_size / 1024.0 / 1024.0);
                else if (st.st_size >= 1024)
                    g_snprintf(sizestr, sizeof sizestr, "%.1f KB", st.st_size / 1024.0);
                else
                    g_snprintf(sizestr, sizeof sizestr, "%ld B", (long)st.st_size);
            }
        }
        g_free(full);
        GtkTreeIter it;
        GdkPixbuf *pb = load_icon_for(name, isdir);
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, 0, isdir ? TR("[目录]") : TR("[文件]"), 1, name, 2, sizestr, 3, pb, 4, NULL, -1);
        if (pb) g_object_unref(pb);
        n++;
        if (isdir) ndir++; else nfile++;
    }
    g_dir_close(dir);
    gchar *msg = g_strdup_printf(TR("位置: %s · %d 项"), path, n);
    if (n == 0) {
        char tmp[512];
        g_snprintf(tmp, sizeof tmp, "%s — %s", msg, TR("此文件夹为空"));
        g_free(msg);
        msg = g_strdup(tmp);
    }
    gtk_label_set_text(GTK_LABEL(status), msg);
    g_free(msg);
    /* 空目录 → 空状态插图页；否则恢复列表/图标视图 */
    if (n == 0) {
        if (empty_lb) gtk_label_set_text(GTK_LABEL(empty_lb), TR("此文件夹为空"));
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(stack), icon_mode ? "icon" : "list");
    }
    g_strlcpy(cwd, path, sizeof cwd);
    if (path_entry) gtk_entry_set_text(GTK_ENTRY(path_entry), cwd);
    update_tab_label();
}

/* 标签页标题: 显示目录 basename，tooltip 显示完整路径（回收站显示「回收站」） */
static void update_tab_label(void) {
    if (!cur_tab || !cur_tab->label) return;
    if (cur_tab->in_trash) {
        char files[4096], info[4096];
        trash_dir(files, sizeof files, info, sizeof info);
        gtk_label_set_text(GTK_LABEL(cur_tab->label), TR("回收站"));
        gtk_widget_set_tooltip_text(cur_tab->label, files);
    } else {
        gchar *base = g_path_get_basename(cwd[0] ? cwd : "/");
        gtk_label_set_text(GTK_LABEL(cur_tab->label), (base && base[0]) ? base : "/");
        gtk_widget_set_tooltip_text(cur_tab->label, cwd);
        g_free(base);
    }
}

static void chdir_trash(void) {
    in_trash = 1;
    set_mode_buttons();
    char files[4096], info[4096];
    trash_dir(files, sizeof files, info, sizeof info);
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(view)));
    gtk_list_store_clear(store);
    GDir *dir = g_dir_open(files, 0, NULL);
    gint n = 0;
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            gchar *ti = g_strdup_printf("%s/%s.trashinfo", info, name);
            char orig[4096], del[64];
            GtkTreeIter it;
            GdkPixbuf *pb = qy_icon_pixbuf(QY_ICON_TRASH, 48, NULL);
            gtk_list_store_append(store, &it);
            if (read_trashinfo2(ti, orig, sizeof orig, del, sizeof del)) {
                /* 列 0=原位置 1=名称 2=大小 3=图标 4=删除时间 */
                gtk_list_store_set(store, &it, 0, orig, 1, name, 2, TR("—"), 3, pb,
                                   4, del[0] ? del : TR("—"), -1);
            } else {
                gtk_list_store_set(store, &it, 0, TR("(无元数据)"), 1, name, 2, TR("—"), 3, pb,
                                   4, TR("—"), -1);
            }
            if (pb) g_object_unref(pb);
            g_free(ti);
            n++;
        }
        g_dir_close(dir);
    }
    if (n == 0) {
        gtk_label_set_text(GTK_LABEL(status), TR("回收站为空"));
        if (empty_lb) gtk_label_set_text(GTK_LABEL(empty_lb), TR("回收站为空"));
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
    } else {
        gtk_label_set_text(GTK_LABEL(status), TR("位置: 回收站 (工具栏: 还原 / 彻底删除 / 清空)"));
        gtk_stack_set_visible_child_name(GTK_STACK(stack), icon_mode ? "icon" : "list");
    }
    g_strlcpy(cwd, files, sizeof cwd);
    update_tab_label();
}

/* ---------- 搜索过滤 ---------- */
/* Ctrl+F 聚焦搜索框（release 271） */
static gboolean on_main_keypress(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->keyval == GDK_KEY_f && (ev->state & GDK_CONTROL_MASK)) {
        if (search_entry) gtk_widget_grab_focus(search_entry);
        return TRUE;
    }
    return FALSE;
}

static void on_search_changed(GtkSearchEntry *se, gpointer ud) {
    (void)ud;
    const char *text = gtk_entry_get_text(GTK_ENTRY(se));
    g_strlcpy(g_search, text ? text : "", sizeof g_search);
    if (in_trash) chdir_trash(); else chdir_to(cwd);
}

/* 自动化验证: QYFILES_SEARCH=/etc:passwd → 进入 /etc 且只显示含 passwd 的项 */
static gboolean auto_search(gpointer p) {
    if (p) {
        gchar *dir = g_strdup((const char *)p);
        chdir_to(dir);
        g_free(dir);
    } else {
        chdir_to(cwd);
    }
    return G_SOURCE_REMOVE;
}

/* 打开选中项：目录→进入；图片→qyview；文本→qyedit；其它→状态栏提示 */
static void open_path(const char *name) {
    if (in_trash) return;
    gchar *full = g_build_filename(cwd, name, NULL);
    if (g_file_test(full, G_FILE_TEST_IS_DIR)) { chdir_to(full); g_free(full); return; }
    const char *ext = strrchr(name, '.');
    gboolean is_img = FALSE, is_txt = FALSE;
    if (ext) {
        if (!g_ascii_strcasecmp(ext, ".png") || !g_ascii_strcasecmp(ext, ".jpg") ||
            !g_ascii_strcasecmp(ext, ".jpeg") || !g_ascii_strcasecmp(ext, ".bmp") ||
            !g_ascii_strcasecmp(ext, ".gif") || !g_ascii_strcasecmp(ext, ".webp"))
            is_img = TRUE;
        else if (!g_ascii_strcasecmp(ext, ".txt") || !g_ascii_strcasecmp(ext, ".c") ||
                 !g_ascii_strcasecmp(ext, ".h") || !g_ascii_strcasecmp(ext, ".md") ||
                 !g_ascii_strcasecmp(ext, ".sh") || !g_ascii_strcasecmp(ext, ".py") ||
                 !g_ascii_strcasecmp(ext, ".ini") || !g_ascii_strcasecmp(ext, ".conf") ||
                 !g_ascii_strcasecmp(ext, ".desktop") || !g_ascii_strcasecmp(ext, ".log"))
            is_txt = TRUE;
    }
    if (is_img || is_txt) {
        gchar *cmd = g_strdup_printf("%s '%s' &", is_img ? "qyview" : "qyedit", full);
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
    } else {
        char msg[512];
        g_snprintf(msg, sizeof msg, "%s — %s", name, TR("暂不支持打开该类型"));
        gtk_label_set_text(GTK_LABEL(status), msg);
    }
    g_free(full);
}

static void do_open_sel(GtkButton *b, gpointer ud) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_label_set_text(GTK_LABEL(status), TR("请先选择一个文件"));
        return;
    }
    gchar *name;
    gtk_tree_model_get(m, &it, 1, &name, -1);
    open_path(name);
    g_free(name);
}

static void on_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer ud) {
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    gchar *name;
    gtk_tree_model_get_iter(m, &it, path);
    gtk_tree_model_get(m, &it, 1, &name, -1);
    open_path(name);
    g_free(name);
}

/* ---------- 图标视图 ---------- */

/* 空状态插图（cairo，深蓝底 + 橙色文件夹） */
static gboolean empty_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    qy_draw_empty_state(cr, a.width, a.height, "files");
    return FALSE;
}

/* 按文件类型加载图标（目录=文件夹；扩展名映射颜色） */
static QyIconId icon_id_for(const char *name, gboolean isdir) {
    if (isdir) return QY_ICON_FILES;
    const char *dot = strrchr(name, '.');
    if (dot) {
        if (!g_ascii_strcasecmp(dot, ".png") || !g_ascii_strcasecmp(dot, ".jpg")
            || !g_ascii_strcasecmp(dot, ".jpeg") || !g_ascii_strcasecmp(dot, ".webp")
            || !g_ascii_strcasecmp(dot, ".gif") || !g_ascii_strcasecmp(dot, ".bmp"))
            return QY_ICON_IMAGE;
        if (!g_ascii_strcasecmp(dot, ".mp3") || !g_ascii_strcasecmp(dot, ".wav")
            || !g_ascii_strcasecmp(dot, ".flac") || !g_ascii_strcasecmp(dot, ".ogg"))
            return QY_ICON_MUSIC;
        if (!g_ascii_strcasecmp(dot, ".mp4") || !g_ascii_strcasecmp(dot, ".mkv")
            || !g_ascii_strcasecmp(dot, ".avi") || !g_ascii_strcasecmp(dot, ".webm"))
            return QY_ICON_VIDEO;
        if (!g_ascii_strcasecmp(dot, ".zip") || !g_ascii_strcasecmp(dot, ".tar")
            || !g_ascii_strcasecmp(dot, ".gz") || !g_ascii_strcasecmp(dot, ".bz2")
            || !g_ascii_strcasecmp(dot, ".xz") || !g_ascii_strcasecmp(dot, ".7z")
            || !g_ascii_strcasecmp(dot, ".rar"))
            return QY_ICON_ARCHIVE;
        if (!g_ascii_strcasecmp(dot, ".txt") || !g_ascii_strcasecmp(dot, ".md")
            || !g_ascii_strcasecmp(dot, ".c") || !g_ascii_strcasecmp(dot, ".h")
            || !g_ascii_strcasecmp(dot, ".py") || !g_ascii_strcasecmp(dot, ".sh")
            || !g_ascii_strcasecmp(dot, ".js") || !g_ascii_strcasecmp(dot, ".ini")
            || !g_ascii_strcasecmp(dot, ".conf") || !g_ascii_strcasecmp(dot, ".desktop")
            || !g_ascii_strcasecmp(dot, ".log"))
            return QY_ICON_EDIT;
        if (!g_ascii_strcasecmp(dot, ".pdf"))
            return QY_ICON_DOC;
    }
    return QY_ICON_FILE;
}

static GdkPixbuf *load_icon_for(const char *name, gboolean isdir) {
    return qy_icon_pixbuf(icon_id_for(name, isdir), 48, NULL);
}

/* ---------- 右侧预览面板 ---------- */
static gboolean preview_is_image(const char *ext) {
    return ext && (!g_ascii_strcasecmp(ext, ".png") || !g_ascii_strcasecmp(ext, ".jpg") ||
                   !g_ascii_strcasecmp(ext, ".jpeg") || !g_ascii_strcasecmp(ext, ".bmp") ||
                   !g_ascii_strcasecmp(ext, ".webp"));
}

static gboolean preview_is_text(const char *ext) {
    return ext && (!g_ascii_strcasecmp(ext, ".txt") || !g_ascii_strcasecmp(ext, ".c") ||
                   !g_ascii_strcasecmp(ext, ".h") || !g_ascii_strcasecmp(ext, ".md") ||
                   !g_ascii_strcasecmp(ext, ".sh") || !g_ascii_strcasecmp(ext, ".py") ||
                   !g_ascii_strcasecmp(ext, ".ini") || !g_ascii_strcasecmp(ext, ".conf") ||
                   !g_ascii_strcasecmp(ext, ".desktop") || !g_ascii_strcasecmp(ext, ".log"));
}

/* 从当前活动视图（列表/图标）取选中文件名 */
static gboolean preview_selected_name(gchar **name) {
    *name = NULL;
    if (icon_mode && GTK_IS_ICON_VIEW(icon_view)) {
        GList *items = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(icon_view));
        if (items) {
            GtkTreeModel *m = gtk_icon_view_get_model(GTK_ICON_VIEW(icon_view));
            GtkTreeIter it;
            if (gtk_tree_model_get_iter(m, &it, items->data))
                gtk_tree_model_get(m, &it, 1, name, -1);
            g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
        }
    }
    if (!*name)
        selected_name(name);
    return *name != NULL;
}

static void preview_size_str(goffset size, char *buf, size_t n) {
    if (size >= 1024 * 1024)
        g_snprintf(buf, n, "%.1f MB", size / 1024.0 / 1024.0);
    else if (size >= 1024)
        g_snprintf(buf, n, "%.1f KB", size / 1024.0);
    else
        g_snprintf(buf, n, "%lld B", (long long)size);
}

/* 刷新预览内容（选中文件时显示缩略图 / 文本前 500 字节 / 图标+信息） */
static void update_preview(void) {
    if (!preview_revealer || !preview_box) return;

    GList *ch = gtk_container_get_children(GTK_CONTAINER(preview_box));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    gchar *name = NULL;
    if (!preview_selected_name(&name) || !name) {
        GtkWidget *lb = gtk_label_new(TR("未选择文件"));
        add_class(lb, "qy-files-preview-empty");
        gtk_box_pack_start(GTK_BOX(preview_box), lb, TRUE, TRUE, 0);
        g_free(name);
        return;
    }

    gchar *full = g_build_filename(cwd, name, NULL);
    struct stat st;
    gboolean has_stat = (stat(full, &st) == 0);
    gboolean isdir = g_file_test(full, G_FILE_TEST_IS_DIR);
    const char *ext = strrchr(name, '.');

    /* 图片：缩略图 128x128 */
    if (!isdir && preview_is_image(ext)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(full, 128, 128, TRUE, NULL);
        if (pb) {
            GtkWidget *img = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
            gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(preview_box), img, TRUE, TRUE, 8);
            GtkWidget *lb = gtk_label_new(name);
            add_class(lb, "qy-files-preview-name");
            gtk_label_set_line_wrap(GTK_LABEL(lb), TRUE);
            gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
            gtk_box_pack_start(GTK_BOX(preview_box), lb, FALSE, FALSE, 4);
            if (has_stat) {
                char sb[32];
                preview_size_str(st.st_size, sb, sizeof sb);
                GtkWidget *sz = gtk_label_new(sb);
                add_class(sz, "qy-files-preview-dim");
                gtk_label_set_xalign(GTK_LABEL(sz), 0.0);
                gtk_box_pack_start(GTK_BOX(preview_box), sz, FALSE, FALSE, 2);
            }
            g_free(full); g_free(name);
            return;
        }
        /* 解码失败落到通用信息 */
    }

    /* 文本：只读显示前 500 字节 */
    if (!isdir && preview_is_text(ext)) {
        gchar *content = NULL;
        gsize len = 0;
        if (g_file_get_contents(full, &content, &len, NULL)) {
            if (len > 500) len = 500;
            GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
            GtkWidget *tv = gtk_text_view_new();
            gtk_widget_set_name(tv, "qyfiles-preview-text");
            gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), content, len);
            gtk_container_add(GTK_CONTAINER(sw), tv);
            gtk_box_pack_start(GTK_BOX(preview_box), sw, TRUE, TRUE, 0);
            g_free(content);
            g_free(full); g_free(name);
            return;
        }
    }

    /* 目录 / 其它：图标 + 名称 + 大小 + 类型 */
    GdkPixbuf *ipb = qy_icon_pixbuf(icon_id_for(name, isdir), 48, NULL);
    GtkWidget *ic = gtk_image_new_from_pixbuf(ipb);
    if (ipb) g_object_unref(ipb);
    gtk_widget_set_halign(ic, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(preview_box), ic, FALSE, FALSE, 8);

    GtkWidget *lb = gtk_label_new(name);
    add_class(lb, "qy-files-preview-name");
    gtk_label_set_line_wrap(GTK_LABEL(lb), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
    gtk_box_pack_start(GTK_BOX(preview_box), lb, FALSE, FALSE, 4);

    char size_buf[64] = "—";
    if (has_stat && !isdir)
        preview_size_str(st.st_size, size_buf, sizeof size_buf);
    GtkWidget *sz = gtk_label_new(size_buf);
    add_class(sz, "qy-files-preview-dim");
    gtk_label_set_xalign(GTK_LABEL(sz), 0.0);
    gtk_box_pack_start(GTK_BOX(preview_box), sz, FALSE, FALSE, 2);

    char type_buf[96];
    if (isdir)
        g_strlcpy(type_buf, TR("文件夹"), sizeof type_buf);
    else if (ext)
        g_snprintf(type_buf, sizeof type_buf, "%s (%s)", TR("文件"), ext);
    else
        g_strlcpy(type_buf, TR("文件"), sizeof type_buf);
    GtkWidget *ty = gtk_label_new(type_buf);
    add_class(ty, "qy-files-preview-dim");
    gtk_label_set_xalign(GTK_LABEL(ty), 0.0);
    gtk_box_pack_start(GTK_BOX(preview_box), ty, FALSE, FALSE, 2);

    g_free(full);
    g_free(name);
}

/* 工具栏「预览」开关 */
static void on_preview_toggled(GtkToggleButton *btn, gpointer ud) {
    (void)ud;
    gboolean active = gtk_toggle_button_get_active(btn);
    if (preview_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(preview_revealer), active);
    if (active) update_preview();
}

/* 选中变化时刷新预览 */
static void on_view_cursor_changed(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (preview_revealer && gtk_revealer_get_reveal_child(GTK_REVEALER(preview_revealer)))
        update_preview();
    /* 状态栏显示选中项数与总大小（release 275） */
    GSList *names = selected_names();
    guint n = g_slist_length(names);
    if (n > 0 && status) {
        gchar *msg = NULL;
        if (in_trash) {
            msg = g_strdup_printf(TR("已选中 %u 项"), n);
        } else {
            gint64 total = 0;
            for (GSList *l = names; l; l = l->next) {
                char full[4096];
                g_snprintf(full, sizeof full, "%s/%s", cwd, (const char *)l->data);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) total += st.st_size;
            }
            char sz[64];
            if (total >= (1 << 20))
                g_snprintf(sz, sizeof sz, "%.1f MB", total / (double)(1 << 20));
            else
                g_snprintf(sz, sizeof sz, "%lld KB", (long long)(total / 1024));
            msg = g_strdup_printf(TR("已选中 %u 项 · 共 %s"), n, sz);
        }
        gtk_label_set_text(GTK_LABEL(status), msg);
        g_free(msg);
    }
    g_slist_free_full(names, g_free);
}

/* 图标视图双击/回车打开 */
static void on_icon_activated(GtkIconView *iv, GtkTreePath *path, gpointer ud) {
    (void)iv; (void)ud;
    GtkTreeModel *m = gtk_icon_view_get_model(iv);
    GtkTreeIter it;
    gchar *name = NULL;
    if (gtk_tree_model_get_iter(m, &it, path))
        gtk_tree_model_get(m, &it, 1, &name, -1);
    if (!name) return;
    open_path(name);
    g_free(name);
}

/* 更新视图切换按钮的图标与提示（随当前视图状态切换: 图标视图/列表视图） */
static void set_view_btn(GtkWidget *b, gboolean icon_mode) {
    if (!b) return;
    GtkWidget *img = gtk_bin_get_child(GTK_BIN(b));
    if (img) gtk_widget_destroy(img);
    gtk_container_add(GTK_CONTAINER(b),
        gtk_image_new_from_pixbuf(qy_icon_pixbuf(icon_mode ? QY_ICON_LIST : QY_ICON_GRID, 18, NULL)));
    gtk_widget_set_tooltip_text(b, icon_mode ? TR("列表视图") : TR("图标视图"));
}

/* 切换列表/图标视图 */
static void on_toggle_view(GtkButton *b, gpointer ud) {
    (void)ud;
    icon_mode = !icon_mode;
    const char *cur = stack ? gtk_stack_get_visible_child_name(GTK_STACK(stack)) : NULL;
    /* 空状态页不随视图切换，保持插图展示 */
    if (g_strcmp0(cur, "empty") != 0)
        gtk_stack_set_visible_child_name(GTK_STACK(stack), icon_mode ? "icon" : "list");
    set_view_btn(GTK_WIDGET(b), icon_mode);
    g_printerr("QYFILESDBG: view=%s\n", icon_mode ? "icon" : "list");
}

/* 自动化: QYFILES_VIEW=icon 启动后切换图标视图 */
static gboolean auto_view_icon(gpointer p) {
    on_toggle_view(NULL, p);
    return G_SOURCE_REMOVE;
}

static void on_refresh(GtkButton *b, gpointer ud) {
    if (in_trash) chdir_trash();
    else chdir_to(cwd);
}

static void on_up(GtkButton *b, gpointer ud) {
    if (in_trash) { chdir_to("/"); return; }
    gchar *parent = g_path_get_dirname(cwd);
    chdir_to(parent);
    g_free(parent);
}

static void on_home(GtkButton *b, gpointer ud) { chdir_to(g_get_home_dir()); }

/* ---------- 网络挂载（SMB/CIFS + SSHFS） ---------- */

/* 异步命令执行：捕获 stdout/stderr，退出后更新 #qyfiles-mount-status */
typedef struct {
    GtkWidget *status;    /* 状态标签 */
    GString *out;
} MountJob;

static gboolean mount_read_io(GIOChannel *ch, GIOCondition cond, gpointer ud) {
    MountJob *job = (MountJob *)ud;
    if (cond & (G_IO_HUP | G_IO_ERR)) return FALSE;
    gchar buf[512];
    gsize n = 0;
    GError *err = NULL;
    GIOStatus st = g_io_channel_read_chars(ch, buf, sizeof buf - 1, &n, &err);
    if (st == G_IO_STATUS_NORMAL && n > 0) {
        buf[n] = 0;
        g_string_append(job->out, buf);
        return TRUE;
    }
    g_clear_error(&err);
    return st != G_IO_STATUS_EOF;
}

static void mount_exit_cb(GPid pid, gint status, gpointer ud) {
    MountJob *job = (MountJob *)ud;
    g_spawn_close_pid(pid);
    if (job->status) {
        gchar *out = g_strstrip(job->out->str);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            gtk_label_set_text(GTK_LABEL(job->status), out[0] ? out : TR("挂载成功"));
        else
            gtk_label_set_text(GTK_LABEL(job->status), out[0] ? out : TR("挂载失败"));
    }
    if (job->status)
        g_object_remove_weak_pointer(G_OBJECT(job->status), (gpointer *)&job->status);
    g_string_free(job->out, TRUE);
    g_free(job);
}

static void mount_spawn(const gchar *cmd) {
    if (!mount_status) return;
    gtk_label_set_text(GTK_LABEL(mount_status), TR("执行中…"));
    gchar **argv = NULL;
    GError *err = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, &err)) {
        gtk_label_set_text(GTK_LABEL(mount_status),
                          err && err->message ? err->message : TR("命令解析失败"));
        g_clear_error(&err);
        return;
    }
    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &pid, NULL, &out_fd, &err_fd, &err)) {
        gtk_label_set_text(GTK_LABEL(mount_status),
                          err && err->message ? err->message : TR("启动失败"));
        g_clear_error(&err);
        g_strfreev(argv);
        return;
    }
    g_strfreev(argv);
    MountJob *job = g_new0(MountJob, 1);
    job->status = mount_status;
    job->out = g_string_new(NULL);
    g_object_add_weak_pointer(G_OBJECT(job->status), (gpointer *)&job->status);
    GIOChannel *ch_out = g_io_channel_unix_new(out_fd);
    GIOChannel *ch_err = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(ch_out, NULL, NULL);
    g_io_channel_set_encoding(ch_err, NULL, NULL);
    g_io_channel_set_close_on_unref(ch_out, TRUE);
    g_io_channel_set_close_on_unref(ch_err, TRUE);
    g_io_add_watch(ch_out, G_IO_IN | G_IO_HUP | G_IO_ERR, mount_read_io, job);
    g_io_add_watch(ch_err, G_IO_IN | G_IO_HUP | G_IO_ERR, mount_read_io, job);
    g_io_channel_unref(ch_out);
    g_io_channel_unref(ch_err);
    g_child_watch_add(pid, mount_exit_cb, job);
}

/* 由服务器地址生成默认挂载点 /mnt/lanxiu-<名> */
static gchar *mount_default_point(const char *server) {
    const char *at = strrchr(server ? server : "", '@');
    const char *s = at ? at + 1 : (server ? server : "");
    GString *g = g_string_new("/mnt/lanxiu-");
    for (const char *p = s; *p && *p != '/'; p++) {
        if (g_ascii_isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
            g_string_append_c(g, *p);
        else if (*p == ':')
            g_string_append_c(g, '-');
    }
    return g_string_free(g, FALSE);
}

static void mount_update_default_point(void) {
    if (!mount_point_entry) return;
    gchar *mp = mount_default_point(gtk_entry_get_text(GTK_ENTRY(mount_server_entry)));
    gtk_entry_set_text(GTK_ENTRY(mount_point_entry), mp);
    g_free(mp);
}

static void mount_on_server_changed(GtkEditable *ed, gpointer ud) {
    (void)ed; (void)ud;
    if (mount_point_auto) mount_update_default_point();
}

static void mount_on_point_changed(GtkEditable *ed, gpointer ud) {
    (void)ed; (void)ud;
    mount_point_auto = FALSE;
}

/* 类型切换：SMB 用「共享路径」，SSHFS 用「远程路径」 */
static void mount_on_type_changed(GtkComboBox *cb, gpointer ud) {
    (void)ud;
    int idx = gtk_combo_box_get_active(cb);
    if (mount_share_lbl)
        gtk_label_set_text(GTK_LABEL(mount_share_lbl), idx == 1 ? TR("远程路径") : TR("共享路径"));
}

static void mount_do_mount(void) {
    const char *server = gtk_entry_get_text(GTK_ENTRY(mount_server_entry));
    const char *share  = gtk_entry_get_text(GTK_ENTRY(mount_share_entry));
    const char *user   = gtk_entry_get_text(GTK_ENTRY(mount_user_entry));
    const char *pass   = gtk_entry_get_text(GTK_ENTRY(mount_pass_entry));
    const char *mp     = gtk_entry_get_text(GTK_ENTRY(mount_point_entry));
    if (!server[0] || !mp[0]) {
        gtk_label_set_text(GTK_LABEL(mount_status), TR("请填写服务器地址与挂载点"));
        return;
    }
    gchar *sudo_path = g_find_program_in_path("sudo");
    gchar *cmd = NULL;
    int type = gtk_combo_box_get_active(GTK_COMBO_BOX(mount_type_combo));
    if (type == 1) {
        /* SSHFS */
        if (!g_find_program_in_path("sshfs")) {
            gtk_label_set_text(GTK_LABEL(mount_status), TR("未安装 sshfs"));
            g_free(sudo_path);
            return;
        }
        gchar *host = NULL;
        if (user[0]) host = g_strdup_printf("%s@%s", user, server);
        else         host = g_strdup(server);
        gchar *src = g_strdup_printf("%s:%s", host, share[0] ? share : "/");
        gchar *q_src = g_shell_quote(src);
        gchar *q_mp = g_shell_quote(mp);
        cmd = g_strdup_printf("sshfs %s %s", q_src, q_mp);
        g_free(q_src); g_free(q_mp);
        g_free(src); g_free(host);
    } else {
        /* SMB/CIFS */
        if (!g_find_program_in_path("mount.cifs")) {
            gtk_label_set_text(GTK_LABEL(mount_status), TR("未安装 mount.cifs"));
            g_free(sudo_path);
            return;
        }
        gchar *src = g_strdup_printf("//%s/%s", server, share[0] ? share : "");
        gchar *opts = NULL;
        if (user[0])
            opts = g_strdup_printf("username=%s,password=%s", user, pass);
        else
            opts = g_strdup("guest");
        gchar *q_src = g_shell_quote(src);
        gchar *q_mp = g_shell_quote(mp);
        if (sudo_path)
            cmd = g_strdup_printf("sudo mount -t cifs %s %s -o %s", q_src, q_mp, opts);
        else
            cmd = g_strdup_printf("mount -t cifs %s %s -o %s", q_src, q_mp, opts);
        g_free(q_src); g_free(q_mp); g_free(opts); g_free(src);
    }
    g_free(sudo_path);
    if (cmd) {
        mount_spawn(cmd);
        g_free(cmd);
    }
}

static void mount_do_umount(void) {
    const char *mp = gtk_entry_get_text(GTK_ENTRY(mount_point_entry));
    if (!mp[0]) {
        gtk_label_set_text(GTK_LABEL(mount_status), TR("请填写要卸载的挂载点"));
        return;
    }
    gchar *sudo_path = g_find_program_in_path("sudo");
    gchar *q_mp = g_shell_quote(mp);
    gchar *cmd = NULL;
    if (sudo_path)
        cmd = g_strdup_printf("sudo umount %s", q_mp);
    else
        cmd = g_strdup_printf("umount %s", q_mp);
    g_free(sudo_path);
    g_free(q_mp);
    mount_spawn(cmd);
    g_free(cmd);
}

static void mount_on_response(GtkDialog *dlg, gint response, gpointer ud) {
    (void)ud;
    if (response == GTK_RESPONSE_CANCEL || response == GTK_RESPONSE_DELETE_EVENT) {
        mount_status = NULL;   /* 对话框销毁后不再更新 */
        gtk_widget_destroy(GTK_WIDGET(dlg));
    } else if (response == 1) {
        mount_do_mount();
    } else if (response == 2) {
        mount_do_umount();
    }
}

/* 工具栏「挂载」：弹出「挂载远程」对话框 */
static void on_mount_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkWidget *parent = gtk_widget_get_toplevel(view);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(TR("挂载远程"),
        GTK_WINDOW(parent), GTK_DIALOG_MODAL,
        "_取消", GTK_RESPONSE_CANCEL,
        TR("卸载"), 2,
        TR("挂载"), 1, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), 1);
    g_signal_connect(dlg, "response", G_CALLBACK(mount_on_response), NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_add(GTK_CONTAINER(box), grid);

    mount_type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mount_type_combo), NULL, "SMB/CIFS");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mount_type_combo), NULL, "SSHFS");
    gtk_combo_box_set_active(GTK_COMBO_BOX(mount_type_combo), 0);
    g_signal_connect(mount_type_combo, "changed", G_CALLBACK(mount_on_type_changed), NULL);

    mount_server_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(mount_server_entry), "192.168.1.100");
    g_signal_connect(mount_server_entry, "changed", G_CALLBACK(mount_on_server_changed), NULL);

    mount_share_lbl = gtk_label_new(TR("共享路径"));
    gtk_widget_set_halign(mount_share_lbl, GTK_ALIGN_START);
    mount_share_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(mount_share_entry), "share / /");

    mount_user_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(mount_user_entry), TR("用户名（可空）"));
    mount_pass_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(mount_pass_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(mount_pass_entry), TR("密码"));

    mount_point_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(mount_point_entry), "/mnt/lanxiu-");
    mount_point_auto = TRUE;
    g_signal_connect(mount_point_entry, "changed", G_CALLBACK(mount_on_point_changed), NULL);

    mount_status = gtk_label_new(TR("就绪"));
    gtk_widget_set_name(mount_status, "qyfiles-mount-status");
    gtk_label_set_line_wrap(GTK_LABEL(mount_status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(mount_status), 0.0);
    gtk_widget_set_hexpand(mount_status, TRUE);

    const char *labels[][2] = {
        { TR("类型"), NULL },
        { TR("服务器地址"), NULL },
        { NULL, NULL },
        { TR("用户名"), NULL },
        { TR("密码"), NULL },
        { TR("挂载点"), NULL },
    };
    GtkWidget *entries[] = { NULL, mount_server_entry, mount_share_entry,
                             mount_user_entry, mount_pass_entry, mount_point_entry };
    int row = 0;
    for (int i = 0; i < 6; i++) {
        if (i == 2) {
            GtkWidget *lbl = mount_share_lbl;
            gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), mount_share_entry, 1, row, 1, 1);
            row++;
            continue;
        }
        GtkWidget *lbl = gtk_label_new(labels[i][0]);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        if (i == 0) gtk_grid_attach(GTK_GRID(grid), mount_type_combo, 1, row, 1, 1);
        else        gtk_grid_attach(GTK_GRID(grid), entries[i], 1, row, 1, 1);
        row++;
    }
    gtk_grid_attach(GTK_GRID(grid), mount_status, 0, row, 2, 1);

    gtk_widget_show_all(dlg);
    /* 打开时按当前服务器值刷新默认挂载点 */
    mount_update_default_point();
}

/* ---------- 多标签页 ---------- */

/* 新建一个目录标签页：每个标签拥有独立的列表模型/树视图/图标视图/空状态 */
static QyFilesTab *create_tab(const char *path) {
    QyFilesTab *t = g_new0(QyFilesTab, 1);
    GtkListStore *store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_STRING);

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, TR("类型/原位置"), r1, "text", 0, NULL);
    GtkCellRenderer *r2 = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, TR("名称"), r2, "text", 1, NULL);
    GtkCellRenderer *r3 = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, TR("大小"), r3, "text", 2, NULL);
    GtkCellRenderer *r4 = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, TR("删除时间"), r4, "text", 4, NULL);
    GtkTreeViewColumn *c0 = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 0);
    GtkTreeViewColumn *c1 = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 1);
    GtkTreeViewColumn *c2 = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 2);
    GtkTreeViewColumn *c3 = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 3);
    gtk_tree_view_column_set_clickable(c0, TRUE);
    gtk_tree_view_column_set_sort_indicator(c0, TRUE);
    gtk_tree_view_column_set_sort_column_id(c0, 0);
    gtk_tree_view_column_set_clickable(c1, TRUE);
    gtk_tree_view_column_set_sort_indicator(c1, TRUE);
    gtk_tree_view_column_set_sort_column_id(c1, 1);
    gtk_tree_view_column_set_clickable(c2, TRUE);
    gtk_tree_view_column_set_sort_indicator(c2, TRUE);
    gtk_tree_view_column_set_sort_column_id(c2, 2);
    gtk_tree_view_column_set_clickable(c3, TRUE);
    gtk_tree_view_column_set_sort_indicator(c3, TRUE);
    gtk_tree_view_column_set_sort_column_id(c3, 4);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), 1, GTK_SORT_ASCENDING);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), GTK_SELECTION_MULTIPLE);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), tv);

    GtkWidget *iv = gtk_icon_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(iv), 1);
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(iv), 3);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(iv), 100);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(iv), GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_item_padding(GTK_ICON_VIEW(iv), 4);
    gtk_icon_view_set_spacing(GTK_ICON_VIEW(iv), 8);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(iv), 8);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(iv), 8);
    GtkWidget *scroll_icon = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll_icon), iv);

    GtkWidget *st = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(st), scroll, "list");
    gtk_stack_add_named(GTK_STACK(st), scroll_icon, "icon");

    GtkWidget *empty_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *eda = gtk_drawing_area_new();
    gtk_widget_set_size_request(eda, 240, 170);
    gtk_widget_set_halign(eda, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(eda, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(eda, TRUE);
    g_signal_connect(eda, "draw", G_CALLBACK(empty_draw_cb), NULL);
    GtkWidget *elb = gtk_label_new(TR("此文件夹为空"));
    gtk_widget_set_halign(elb, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(empty_page), eda, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(empty_page), elb, FALSE, FALSE, 4);
    gtk_stack_add_named(GTK_STACK(st), empty_page, "empty");
    gtk_stack_set_visible_child_name(GTK_STACK(st), "list");

    g_signal_connect(tv, "row-activated", G_CALLBACK(on_activated), NULL);
    g_signal_connect(tv, "button-press-event", G_CALLBACK(on_popup), NULL);
    g_signal_connect(tv, "cursor-changed", G_CALLBACK(on_view_cursor_changed), NULL);
    g_signal_connect(iv, "item-activated", G_CALLBACK(on_icon_activated), NULL);
    g_signal_connect(iv, "selection-changed", G_CALLBACK(on_view_cursor_changed), NULL);

    /* 标签页标题 = 目录 basename，tooltip = 完整路径 */
    const char *dir = (path && path[0]) ? path : "/";
    gchar *base = g_path_get_basename(dir);
    GtkWidget *lbl = gtk_label_new((base && base[0]) ? base : "/");
    gtk_widget_set_tooltip_text(lbl, dir);
    g_free(base);

    int page_idx = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), st, lbl);

    g_strlcpy(t->path, dir, sizeof t->path);
    t->in_trash = 0;
    t->search[0] = 0;
    t->view = tv;
    t->icon_view = iv;
    t->stack = st;
    t->empty_lb = elb;
    t->label = lbl;
    g_object_set_data(G_OBJECT(st), "tab", t);

    save_cur_tab();
    cur_tab = t;
    view = tv;
    icon_view = iv;
    stack = st;
    empty_lb = elb;
    in_trash = 0;
    g_search[0] = 0;
    g_strlcpy(cwd, dir, sizeof cwd);   /* 先切 cwd，避免 search-changed 误刷旧目录 */
    if (search_entry) gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    set_mode_buttons();
    chdir_to(dir);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_idx);
    return t;
}

/* 工具栏「+ 新标签」: 打开主目录新标签 */
static void on_new_tab(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    create_tab(g_get_home_dir());
}

/* 工具栏「关闭标签」: 关闭当前标签（至少保留一个） */
static void on_close_tab(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!notebook) return;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    if (n <= 1) {
        gtk_label_set_text(GTK_LABEL(status), TR("至少保留一个标签"));
        return;
    }
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), cur);
    QyFilesTab *t = page ? g_object_get_data(G_OBJECT(page), "tab") : NULL;
    int target = cur > 0 ? cur - 1 : 1;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), target);
    gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), cur);
    if (t) g_free(t);
}

/* 切换标签: 保存旧标签状态，载入新标签（不丢失各自目录） */
static void on_tab_switch(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer ud) {
    (void)nb; (void)page_num; (void)ud;
    QyFilesTab *t = g_object_get_data(G_OBJECT(page), "tab");
    if (!t || t == cur_tab) return;
    save_cur_tab();
    cur_tab = t;
    view = t->view;
    icon_view = t->icon_view;
    stack = t->stack;
    empty_lb = t->empty_lb;
    g_strlcpy(cwd, t->path, sizeof cwd);
    in_trash = t->in_trash;
    g_strlcpy(g_search, t->search, sizeof g_search);
    if (search_entry) gtk_entry_set_text(GTK_ENTRY(search_entry), g_search);
    if (path_entry) gtk_entry_set_text(GTK_ENTRY(path_entry), cwd);
    set_mode_buttons();
    if (in_trash) chdir_trash(); else chdir_to(cwd);
    if (preview_revealer && gtk_revealer_get_reveal_child(GTK_REVEALER(preview_revealer)))
        update_preview();
}

static void activate(GtkApplication *app, gpointer ud) {
    load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫文件管理器"));
    qy_make_titlebar(GTK_WINDOW(win), TR("澜岫文件管理器"));
    qy_window_setup(GTK_WINDOW(win), 920, 560, 720, 480, FALSE);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_main_keypress), NULL);

    GtkWidget *hpane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    qy_add_class(hpane, "qy-app-surface");
    gtk_container_add(GTK_CONTAINER(win), hpane);

    /* 侧边栏 */
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(side, 168, -1);
    gtk_container_set_border_width(GTK_CONTAINER(side), 4);
    const char *home = g_get_home_dir();
    static char p_home[512], p_docs[512], p_dl[512], p_pics[512];
    snprintf(p_home, sizeof p_home, "%s", home);
    snprintf(p_docs, sizeof p_docs, TR("%s/文档"), home);
    snprintf(p_dl, sizeof p_dl, TR("%s/下载"), home);
    snprintf(p_pics, sizeof p_pics, TR("%s/图片"), home);
    side_button(TR("主目录"), p_home, side);
    side_button(TR("图片"), p_pics, side);
    side_button(TR("文档"), p_docs, side);
    side_button(TR("下载"), p_dl, side);
    side_button(TR("根目录 /"), "/", side);
    side_button(TR("回收站"), "TRASH", side);
    /* 书签分组（release 256） */
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    GtkWidget *bm_hdr = gtk_label_new(TR("书签"));
    gtk_widget_set_halign(bm_hdr, GTK_ALIGN_START);
    add_class(bm_hdr, "qy-files-bm-header");
    gtk_box_pack_start(GTK_BOX(side), bm_hdr, FALSE, FALSE, 0);
    book_list = gtk_list_box_new();
    qy_add_class(book_list, "qy-files-book");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(book_list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(book_list), TRUE);
    g_signal_connect(book_list, "row-activated", G_CALLBACK(on_book_activated), NULL);
    g_signal_connect(book_list, "button-press-event", G_CALLBACK(on_book_popup), NULL);
    GtkWidget *bm_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bm_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(bm_sw), book_list);
    gtk_box_pack_start(GTK_BOX(side), bm_sw, TRUE, TRUE, 0);
    load_bookmarks();
    gtk_box_pack_start(GTK_BOX(hpane), side, FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(hpane), vbox, TRUE, TRUE, 0);

    /* 右侧预览面板（GtkRevealer，宽 260） */
    preview_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(preview_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_reveal_child(GTK_REVEALER(preview_revealer), FALSE);
    gtk_widget_set_name(preview_revealer, "qyfiles-preview");
    GtkWidget *preview_v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(preview_v, 260, -1);
    gtk_widget_set_margin_start(preview_v, 8);
    gtk_widget_set_margin_end(preview_v, 8);
    gtk_widget_set_margin_top(preview_v, 8);
    gtk_widget_set_margin_bottom(preview_v, 8);
    gtk_container_add(GTK_CONTAINER(preview_revealer), preview_v);
    GtkWidget *ph = gtk_label_new(TR("预览"));
    add_class(ph, "qy-files-preview-title");
    gtk_label_set_xalign(GTK_LABEL(ph), 0.0);
    gtk_box_pack_start(GTK_BOX(preview_v), ph, FALSE, FALSE, 0);
    preview_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(preview_v), preview_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hpane), preview_revealer, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *toolbar = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(toolbar), hbox);
    add_class(toolbar, "qy-files-toolbar");
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 2);
    /* 路径地址栏 */
    GtkWidget *pathbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(path_entry), "/");
    g_signal_connect(path_entry, "activate", G_CALLBACK(on_path_activate), NULL);
    gtk_box_pack_start(GTK_BOX(pathbar), path_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), pathbar, FALSE, FALSE, 2);
    GtkWidget *b_ref = qy_icon_button(QY_ICON_REFRESH, 18, TR("刷新"), NULL);
    add_class(b_ref, "qy-btn");
    g_signal_connect(b_ref, "clicked", G_CALLBACK(on_refresh), NULL);
    GtkWidget *b_home = gtk_button_new_with_label(TR("主目录"));
    add_class(b_home, "qy-btn");
    g_signal_connect(b_home, "clicked", G_CALLBACK(on_home), NULL);
    GtkWidget *b_up = qy_icon_button(QY_ICON_UP, 18, TR("上级目录"), NULL);
    add_class(b_up, "qy-btn");
    g_signal_connect(b_up, "clicked", G_CALLBACK(on_up), NULL);
    b_mk = gtk_button_new_with_label(TR("新建文件夹"));
    add_class(b_mk, "qy-btn");
    g_signal_connect(b_mk, "clicked", G_CALLBACK(do_mkdir), NULL);
    b_nf = gtk_button_new_with_label(TR("新建文件"));
    add_class(b_nf, "qy-btn");
    g_signal_connect(b_nf, "clicked", G_CALLBACK(do_newfile), NULL);
    b_term = gtk_button_new_with_label(TR("终端"));
    add_class(b_term, "qy-btn");
    g_signal_connect(b_term, "clicked", G_CALLBACK(do_open_terminal), NULL);
    b_del = qy_icon_button(QY_ICON_DELETE, 18, TR("删除"), "qy-btn-danger");
    add_class(b_del, "qy-btn");
    g_signal_connect(b_del, "clicked", G_CALLBACK(do_delete), NULL);
    b_ren = gtk_button_new_with_label(TR("重命名"));
    add_class(b_ren, "qy-btn");
    g_signal_connect(b_ren, "clicked", G_CALLBACK(do_rename), NULL);
    b_cp = qy_icon_button(QY_ICON_COPY, 18, TR("复制"), NULL);
    add_class(b_cp, "qy-btn");
    g_signal_connect(b_cp, "clicked", G_CALLBACK(do_copy), NULL);
    b_prop = gtk_button_new_with_label(TR("属性"));
    add_class(b_prop, "qy-btn");
    g_signal_connect(b_prop, "clicked", G_CALLBACK(do_prop), NULL);
    b_open = gtk_button_new_with_label(TR("打开"));
    add_class(b_open, "qy-btn");
    g_signal_connect(b_open, "clicked", G_CALLBACK(do_open_sel), NULL);
    b_bm = gtk_button_new_with_label(TR("添加书签"));
    add_class(b_bm, "qy-btn");
    g_signal_connect(b_bm, "clicked", G_CALLBACK(do_add_bookmark), NULL);
    b_mount = gtk_button_new_with_label(TR("挂载"));
    add_class(b_mount, "qy-btn");
    g_signal_connect(b_mount, "clicked", G_CALLBACK(on_mount_clicked), NULL);
    GtkWidget *b_view = qy_icon_button(QY_ICON_GRID, 18, TR("图标视图"), NULL);
    add_class(b_view, "qy-btn");
    g_signal_connect(b_view, "clicked", G_CALLBACK(on_toggle_view), NULL);
    b_preview = gtk_toggle_button_new_with_label(TR("预览"));
    add_class(b_preview, "qy-btn");
    g_signal_connect(b_preview, "toggled", G_CALLBACK(on_preview_toggled), NULL);
    /* 多标签工具栏按钮（始终可见） */
    GtkWidget *b_new_tab = qy_icon_button(QY_ICON_PLUS, 18, TR("新标签"), NULL);
    add_class(b_new_tab, "qy-btn");
    g_signal_connect(b_new_tab, "clicked", G_CALLBACK(on_new_tab), NULL);
    GtkWidget *b_close_tab = gtk_button_new_with_label(TR("关闭标签"));
    add_class(b_close_tab, "qy-btn");
    g_signal_connect(b_close_tab, "clicked", G_CALLBACK(on_close_tab), NULL);
    b_res = gtk_button_new_with_label(TR("还原"));
    add_class(b_res, "qy-btn");
    add_class(b_res, "qy-btn-danger");
    g_signal_connect(b_res, "clicked", G_CALLBACK(do_restore), NULL);
    b_pur = gtk_button_new_with_label(TR("彻底删除"));
    add_class(b_pur, "qy-btn");
    add_class(b_pur, "qy-btn-danger");
    g_signal_connect(b_pur, "clicked", G_CALLBACK(do_purge), NULL);
    b_emp = gtk_button_new_with_label(TR("清空回收站"));
    add_class(b_emp, "qy-btn");
    add_class(b_emp, "qy-btn-danger");
    g_signal_connect(b_emp, "clicked", G_CALLBACK(do_empty_trash), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b_ref, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_mk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_nf, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_term, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_ren, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_cp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_prop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_bm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_mount, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_view, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_preview, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_new_tab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_close_tab, FALSE, FALSE, 0);
    search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(search_entry, 220, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索当前目录"));
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    /* 类型过滤下拉（release 264） */
    GtkWidget *ftype_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ftype_combo), "all", TR("全部"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ftype_combo), "image", TR("图片"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ftype_combo), "doc", TR("文档"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ftype_combo), "audio", TR("音频"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ftype_combo), "video", TR("视频"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ftype_combo), "archive", TR("压缩包"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ftype_combo), 0);
    g_signal_connect(ftype_combo, "changed", G_CALLBACK(on_filter_type_changed), NULL);
    gtk_box_pack_end(GTK_BOX(hbox), ftype_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), search_entry, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), b_open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_res, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_pur, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), b_emp, FALSE, FALSE, 0);

    /* ---------- 多标签内容区（GtkNotebook，#qyfiles-tabs） ---------- */
    notebook = gtk_notebook_new();
    gtk_widget_set_name(notebook, "qyfiles-tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_tab_switch), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    /* 首个标签 = 主目录（或上次记忆目录） */
    create_tab(start_dir());

    status = gtk_label_new(TR("位置: /"));
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    add_class(status, "qy-files-status");
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 2);

    /* ---------- 文件管理器标准快捷键（GtkAccelGroup） ---------- */
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), accel);
    gtk_widget_add_accelerator(b_ref, "clicked", accel, GDK_KEY_r, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);   /* Ctrl+R 刷新 */
    gtk_widget_add_accelerator(b_up, "clicked", accel, GDK_KEY_BackSpace, 0, GTK_ACCEL_VISIBLE);            /* Backspace 上一级 */
    gtk_widget_add_accelerator(b_mk, "clicked", accel, GDK_KEY_n, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);      /* Ctrl+N 新建文件夹 */
    gtk_widget_add_accelerator(b_del, "clicked", accel, GDK_KEY_Delete, 0, GTK_ACCEL_VISIBLE);              /* Delete 删除到回收站 */
    gtk_widget_add_accelerator(b_pur, "clicked", accel, GDK_KEY_Delete, GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE); /* Shift+Delete 彻底删除 */
    gtk_widget_add_accelerator(b_ren, "clicked", accel, GDK_KEY_F2, 0, GTK_ACCEL_VISIBLE);                 /* F2 重命名 */
    gtk_widget_add_accelerator(b_cp, "clicked", accel, GDK_KEY_c, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);     /* Ctrl+C 复制 */
    gtk_widget_add_accelerator(b_home, "clicked", accel, GDK_KEY_Home, GDK_MOD1_MASK, GTK_ACCEL_VISIBLE);   /* Alt+Home 主目录 */
    gtk_widget_add_accelerator(b_open, "clicked", accel, GDK_KEY_Return, 0, GTK_ACCEL_VISIBLE);             /* Enter 打开 */
    gtk_widget_add_accelerator(b_open, "clicked", accel, GDK_KEY_o, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);  /* Ctrl+O 打开 */
    gtk_widget_add_accelerator(b_ref, "clicked", accel, GDK_KEY_F5, 0, GTK_ACCEL_VISIBLE);                 /* F5 刷新 */
    gtk_widget_add_accelerator(search_entry, "grab-focus", accel, GDK_KEY_f, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+F 搜索 */
    gtk_widget_add_accelerator(path_entry, "grab-focus", accel, GDK_KEY_l, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+L 地址栏 */
    gtk_widget_add_accelerator(b_new_tab, "clicked", accel, GDK_KEY_t, GDK_CONTROL_MASK | GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+Shift+T 新标签 */
    /* Ctrl+A 全选（多选模式已启用） */
    GClosure *selcl = g_cclosure_new(G_CALLBACK(on_select_all), NULL, NULL);
    gtk_accel_group_connect(accel, GDK_KEY_a, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, selcl);
    /* 剪切/粘贴（功能存在但无工具栏按钮，用隐藏按钮承载加速键） */
    GtkWidget *b_cut = gtk_button_new();
    GtkWidget *b_paste = gtk_button_new();
    g_signal_connect(b_cut, "clicked", G_CALLBACK(do_cut), NULL);
    g_signal_connect(b_paste, "clicked", G_CALLBACK(do_paste), NULL);
    gtk_widget_add_accelerator(b_cut, "clicked", accel, GDK_KEY_x, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);   /* Ctrl+X 剪切 */
    gtk_widget_add_accelerator(b_paste, "clicked", accel, GDK_KEY_v, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+V 粘贴 */
    g_printerr("QYFILESDBG: accel 13 keys\n");

    gtk_widget_show_all(win);
    /* argv[1]=="--trash" → 启动即进回收站视图 (调试/自证用)；
     * argv[1] 为目录时直接打开该目录（qybrowser 下载管理「打开所在目录」）。 */
    if (g_argc > 1 && !strcmp(g_argv[1], "--trash"))
        chdir_trash();
    else if (g_argc > 1 && g_file_test(g_argv[1], G_FILE_TEST_IS_DIR))
        chdir_to(g_argv[1]);
    else
        chdir_to(home);
    /* 自动化验证: QYFILES_COPY=<文件名> 启动后自动复制该文件 */
    const char *cp = g_getenv("QYFILES_COPY");
    if (cp && cp[0])
        g_timeout_add(300, (GSourceFunc)auto_copy, g_strdup(cp));
    /* 自动化验证: QYFILES_PROP=<文件名> 启动后自动打开属性对话框 */
    const char *pp = g_getenv("QYFILES_PROP");
    if (pp && pp[0])
        g_timeout_add(300, (GSourceFunc)auto_prop, g_strdup(pp));
    /* 自动化验证: QYFILES_RIGHTCLICK=<文件名> 启动后弹出右键菜单 */
    const char *rclk = g_getenv("QYFILES_RIGHTCLICK");
    if (rclk && rclk[0])
        g_timeout_add(800, (GSourceFunc)auto_rightclick, g_strdup(rclk));
    /* 自动化验证: QYFILES_MULTI=file1,file2 选中多行并触发剪切 */
    const char *multi = g_getenv("QYFILES_MULTI");
    if (multi && multi[0])
        g_timeout_add(600, (GSourceFunc)auto_multi, g_strdup(multi));
    /* 自动化验证: QYFILES_VIEW=icon 启动后切换图标视图 */
    const char *qv = g_getenv("QYFILES_VIEW");
    if (qv && g_strcmp0(qv, "icon") == 0)
        g_timeout_add(800, auto_view_icon, NULL);
    /* 自动化验证: QYFILES_SEARCH=/etc:passwd 启动后进入 /etc 并过滤 */
    const char *sf = g_getenv("QYFILES_SEARCH");
    if (sf && sf[0]) {
        char *colon = strchr(sf, ':');
        if (colon) {
            g_strlcpy(g_search, colon + 1, sizeof g_search);
            gchar *dir = g_strndup(sf, (gsize)(colon - sf));
            g_timeout_add(300, (GSourceFunc)auto_search, g_strdup(dir));
            g_free(dir);
        } else {
            g_strlcpy(g_search, sf, sizeof g_search);
            g_timeout_add(300, (GSourceFunc)auto_search, NULL);
        }
    }
    /* 自动化验证: QYFILES_NEWFILE=1 启动后新建未命名.txt */
    if (g_getenv("QYFILES_NEWFILE"))
        g_timeout_add(900, (GSourceFunc)auto_newfile, NULL);
    /* 自动化验证: QYFILES_TERM=1 启动后在当前目录打开终端 */
    if (g_getenv("QYFILES_TERM"))
        g_timeout_add(1200, (GSourceFunc)auto_terminal, NULL);
}

int main(int argc, char **argv) {
    g_argc = argc; g_argv = argv;
    GtkApplication *app = gtk_application_new("com.lanxiu.files", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    /* 只把 argv[0] 交给 GtkApplication: 否则 --trash 会被其命令行解析器判为
       "Unknown option" 而直接退出 (自启/调试场景静默失败根因) */
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}
