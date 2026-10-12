/* qybackup.c — 澜岫备份/恢复工具 (GTK3)
 * 使用 rsync -a --delete 做增量备份（后台进程 + g_child_watch 回收）；
 * 系统无 rsync 时回退 tar czf 打包备份。
 * 备份列表（目录或 .tar.gz）支持恢复回源目录、删除备份。
 * 自动化: QYBACKUP_AUTO=1 启动后自动用当前配置执行一次备份（用于自测）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"

static GtkWidget *src_chooser  = NULL;   /* 源目录选择 */
static GtkWidget *dst_chooser  = NULL;   /* 目标目录选择 */
static GtkWidget *name_entry   = NULL;   /* 备份名称输入 */
static GtkWidget *status_label = NULL;   /* #qy-backup-status */
static GtkWidget *backup_btn   = NULL;  /* .qy-backup-btn */
static GtkWidget *listbox      = NULL;   /* 备份列表 */
static gboolean busy = FALSE;            /* 是否有后台任务运行 */

/* 备份任务上下文（供 child watch 回调） */
typedef struct {
    gchar *label;
    gboolean is_tar;
} BackupJob;

static void set_status(const char *text) {
    if (status_label)
        gtk_label_set_text(GTK_LABEL(status_label), text);
}

/* 检查 PATH 中是否存在程序（g_find_program_in_path 返回需 g_free） */
static gboolean has_program(const char *name) {
    gchar *p = g_find_program_in_path(name);
    gboolean ok = (p != NULL);
    g_free(p);
    return ok;
}

static void refresh_backup_list(void);

/* 默认备份名: backup-YYYYMMDD */
static gchar *default_backup_name(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    return g_strdup_printf("backup-%04d%02d%02d",
                           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

/* 当前目标目录 */
static gchar *target_dir(void) {
    const gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dst_chooser));
    return g_strdup(dir ? dir : "");
}

/* 当前源目录 */
static gchar *source_dir(void) {
    const gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(src_chooser));
    return g_strdup(dir ? dir : "");
}

/* 启动后台命令并回收子进程。
 * 说明: g_spawn_command_line_async 不暴露子进程 pid，无法配合 g_child_watch 回收；
 * 这里用 g_shell_parse_argv 解析命令行字符串后以 g_spawn_async 启动
 * （G_SPAWN_DO_NOT_REAP_CHILD + g_child_watch_add，与 qyrec 同模式）。
 */
static void spawn_bg(const gchar *cmd, GChildWatchFunc cb, gpointer data) {
    gchar **argv = NULL;
    GError *err = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, &err)) {
        busy = FALSE;
        gtk_widget_set_sensitive(backup_btn, TRUE);
        set_status(err ? err->message : TR("无法解析命令"));
        g_clear_error(&err);
        return;
    }
    GPid pid = 0;
    if (!g_spawn_async(NULL, argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &pid, &err)) {
        busy = FALSE;
        gtk_widget_set_sensitive(backup_btn, TRUE);
        if (err) {
            set_status(err->message);
            g_clear_error(&err);
        } else {
            set_status(TR("无法启动命令"));
        }
        g_strfreev(argv);
        return;
    }
    g_strfreev(argv);
    if (cb)
        g_child_watch_add(pid, cb, data);
}

/* 后台任务结束（备份/恢复/删除共用） */
static void on_job_exited(GPid pid, gint status, gpointer ud) {
    BackupJob *job = (BackupJob *)ud;
    g_spawn_close_pid(pid);
    busy = FALSE;
    gtk_widget_set_sensitive(backup_btn, TRUE);
    char st[512];
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        g_snprintf(st, sizeof st, "%s「%s」", TR("完成"), job->label);
    } else {
        g_snprintf(st, sizeof st, "%s「%s」（%s %d）", TR("失败"), job->label,
                    TR("退出码"), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    set_status(st);
    refresh_backup_list();
    g_free(job->label);
    g_free(job);
}

/* ---------- 备份列表 ---------- */
static gint sort_names(gconstpointer a, gconstpointer b) {
    const char *na = *(const char *const *)a;
    const char *nb = *(const char *const *)b;
    return g_utf8_collate(na, nb);
}

static void refresh_backup_list(void) {
    GList *ch = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    gchar *dir = target_dir();
    if (!dir || !dir[0]) { g_free(dir); return; }
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            gchar *full = g_build_filename(dir, name, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_DIR) ||
                g_str_has_suffix(name, ".tar.gz"))
                g_ptr_array_add(items, g_strdup(name));
            g_free(full);
        }
        g_dir_close(d);
        g_ptr_array_sort(items, sort_names);
        for (guint i = 0; i < items->len; i++) {
            const char *name = g_ptr_array_index(items, i);
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *lbl = gtk_label_new(name);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_container_add(GTK_CONTAINER(row), lbl);
            g_object_set_data_full(G_OBJECT(row), "backup-name", g_strdup(name), g_free);
            gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
        }
        g_ptr_array_free(items, TRUE);
    }
    g_free(dir);
}

/* 构造备份命令（与「立即备份」完全一致）。
 * 源/目标/名称不完整时返回 NULL；is_tar_out 非空时写入是否使用 tar 回退。 */
static gchar *build_backup_command(gboolean *is_tar_out) {
    gchar *src = source_dir();
    gchar *dst = target_dir();
    const char *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
    if (!src || !src[0] || !dst || !dst[0] || !name || !name[0]) {
        g_free(src);
        g_free(dst);
        return NULL;
    }
    gboolean use_rsync = has_program("rsync");
    if (is_tar_out) *is_tar_out = !use_rsync;

    gchar *qsrc = g_shell_quote(src);
    gchar *cmd = NULL;
    if (use_rsync) {
        /* rsync -a --delete <源>/ <目标>/<名称>/ */
        gchar *dest_dir = g_build_filename(dst, name, NULL);
        gchar *qdest = g_shell_quote(dest_dir);
        cmd = g_strdup_printf("rsync -a --delete %s/ %s/", qsrc, qdest);
        g_free(dest_dir);
        g_free(qdest);
    } else {
        /* 回退: tar czf <目标>/<名称>.tar.gz -C <源> . */
        gchar *archive = g_build_filename(dst, name, NULL);
        gchar *qarchive = g_shell_quote(archive);
        cmd = g_strdup_printf("tar czf %s -C %s .", qarchive, qsrc);
        g_free(archive);
        g_free(qarchive);
    }
    g_free(qsrc);
    g_free(src);
    g_free(dst);
    return cmd;
}

/* ---------- 立即备份 ---------- */
static void on_backup_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (busy) return;
    gchar *dst = target_dir();
    if (!dst || !dst[0]) {
        set_status(TR("请先选择目标目录"));
        g_free(dst);
        return;
    }
    g_mkdir_with_parents(dst, 0755);
    g_free(dst);

    gboolean is_tar = FALSE;
    gchar *cmd = build_backup_command(&is_tar);
    if (!cmd) {
        set_status(TR("请先选择源目录、目标目录并填写备份名称"));
        return;
    }
    BackupJob *job = g_new0(BackupJob, 1);
    job->label = g_strdup(gtk_entry_get_text(GTK_ENTRY(name_entry)));
    job->is_tar = is_tar;

    busy = TRUE;
    gtk_widget_set_sensitive(backup_btn, FALSE);
    set_status(TR("正在备份..."));
    spawn_bg(cmd, on_job_exited, job);
    g_free(cmd);
}

/* ---------- 恢复 ---------- */
static void on_restore_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (busy) return;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox));
    if (!row) { set_status(TR("请先选择要恢复的备份")); return; }
    const char *name = g_object_get_data(G_OBJECT(row), "backup-name");
    if (!name) return;
    gchar *src = source_dir();
    gchar *dst = target_dir();
    if (!src || !src[0] || !dst || !dst[0]) {
        set_status(TR("请先选择源目录与目标目录"));
        g_free(src); g_free(dst);
        return;
    }
    gchar *full = g_build_filename(dst, name, NULL);
    gboolean is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);

    gchar *msg = g_strdup_printf(TR("将备份「%s」恢复到 %s ？\n目标目录中的同名文件可能被覆盖。"),
                                 name, src);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(gtk_widget_get_toplevel(w)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(msg);
    if (resp != GTK_RESPONSE_YES) {
        g_free(src); g_free(dst); g_free(full);
        return;
    }

    gboolean use_rsync = has_program("rsync");
    gchar *cmd = NULL;
    if (is_dir) {
        gchar *qfull = g_shell_quote(full);
        gchar *qsrc = g_shell_quote(src);
        if (use_rsync)
            cmd = g_strdup_printf("rsync -a %s/ %s/", qfull, qsrc);
        else
            cmd = g_strdup_printf("cp -a %s/. %s/", qfull, qsrc);
        g_free(qfull); g_free(qsrc);
    } else {
        /* .tar.gz */
        gchar *qfull = g_shell_quote(full);
        gchar *qsrc = g_shell_quote(src);
        cmd = g_strdup_printf("tar xzf %s -C %s", qfull, qsrc);
        g_free(qfull); g_free(qsrc);
    }

    BackupJob *job = g_new0(BackupJob, 1);
    job->label = g_strdup(name);
    job->is_tar = !is_dir;
    busy = TRUE;
    gtk_widget_set_sensitive(backup_btn, FALSE);
    set_status(TR("正在恢复..."));
    spawn_bg(cmd, on_job_exited, job);
    g_free(cmd);
    g_free(full);
    g_free(src); g_free(dst);
}

/* ---------- 删除备份 ---------- */
static void on_delete_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (busy) return;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox));
    if (!row) { set_status(TR("请先选择要删除的备份")); return; }
    const char *name = g_object_get_data(G_OBJECT(row), "backup-name");
    if (!name) return;
    gchar *dst = target_dir();
    if (!dst || !dst[0]) { set_status(TR("请先选择目标目录")); g_free(dst); return; }
    gchar *full = g_build_filename(dst, name, NULL);

    gchar *msg = g_strdup_printf(TR("删除备份「%s」？\n此操作不可恢复。"), name);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(gtk_widget_get_toplevel(w)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "%s", msg);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(msg);
    if (resp != GTK_RESPONSE_YES) {
        g_free(dst); g_free(full);
        return;
    }

    gchar *qfull = g_shell_quote(full);
    gchar *cmd = g_strdup_printf("rm -rf %s", qfull);
    g_free(qfull);

    BackupJob *job = g_new0(BackupJob, 1);
    job->label = g_strdup(name);
    job->is_tar = !g_file_test(full, G_FILE_TEST_IS_DIR);
    busy = TRUE;
    gtk_widget_set_sensitive(backup_btn, FALSE);
    set_status(TR("正在删除..."));
    spawn_bg(cmd, on_job_exited, job);
    g_free(cmd);
    g_free(dst); g_free(full);
}

/* ---------- 自动化 ---------- */
static gboolean auto_backup(gpointer p) {
    (void)p;
    on_backup_clicked(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* ---------- 计划备份（crontab） ---------- */
#define SCHED_MARKER "# lanxiu-scheduled-backup"

static GtkWidget *sched_freq_combo   = NULL;  /* 频率下拉: 每日/每周/每月 */
static GtkWidget *sched_time_entry  = NULL;  /* 时间输入 HH:MM */
static GtkWidget *sched_status_label = NULL;

static gboolean has_crontab(void) {
    return has_program("crontab");
}

/* 读取当前用户 crontab（无 crontab 命令或无内容时返回空字符串） */
static gchar *read_crontab(void) {
    if (!has_crontab()) return g_strdup("");
    gchar *out = NULL, *err = NULL;
    gint status = 0;
    if (!g_spawn_command_line_sync("crontab -l", &out, &err, &status, NULL)) {
        g_free(out);
        g_free(err);
        return g_strdup("");
    }
    /* crontab -l 无任务时退出码非 0（stderr 提示 no crontab），视为空 */
    if (status != 0 || !out) {
        g_free(out);
        g_free(err);
        return g_strdup("");
    }
    g_free(err);
    return g_strdup(out);
}

/* 写回 crontab：临时文件 + crontab <tmpfile>（避免 stdin 管道复杂性） */
static void write_crontab(const char *content) {
    if (!has_crontab()) return;
    gchar *tmp = g_build_filename(g_get_tmp_dir(), "lanxiu-crontab-XXXXXX", NULL);
    gint fd = g_mkstemp(tmp);
    if (fd < 0) { g_free(tmp); return; }
    GError *err = NULL;
    g_file_set_contents(tmp, content, -1, &err);
    if (err) {
        g_clear_error(&err);
        close(fd);
        g_unlink(tmp);
        g_free(tmp);
        return;
    }
    close(fd);
    gchar *q = g_shell_quote(tmp);
    gchar *cmd = g_strdup_printf("crontab %s", q);
    gchar *out = NULL, *errout = NULL;
    gint status = 0;
    g_spawn_command_line_sync(cmd, &out, &errout, &status, NULL);
    g_free(q);
    g_free(cmd);
    g_free(out);
    g_free(errout);
    g_unlink(tmp);
    g_free(tmp);
}

/* 从 crontab 文本中移除计划备份行（含标记的行） */
static gchar *crontab_without_schedule(const char *content) {
    GString *out = g_string_new(NULL);
    gchar **lines = g_strsplit(content ? content : "", "\n", 0);
    for (int i = 0; lines[i]; i++) {
        if (strstr(lines[i], SCHED_MARKER))
            continue;
        g_string_append(out, lines[i]);
        g_string_append_c(out, '\n');
    }
    g_strfreev(lines);
    return g_string_free(out, FALSE);
}

/* 启用计划备份：合并当前 crontab 并追加一行计划任务 */
static void on_sched_enable(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (busy) return;
    if (!has_crontab()) {
        set_status(TR("系统无 crontab"));
        return;
    }
    gchar *cmd = build_backup_command(NULL);
    if (!cmd) {
        set_status(TR("请先选择源目录、目标目录并填写备份名称"));
        return;
    }
    const char *time_text = gtk_entry_get_text(GTK_ENTRY(sched_time_entry));
    int hh = -1, mm = -1;
    if (sscanf(time_text, "%d:%d", &hh, &mm) != 2 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        set_status(TR("时间格式应为 HH:MM"));
        g_free(cmd);
        return;
    }
    int freq = gtk_combo_box_get_active(GTK_COMBO_BOX(sched_freq_combo));
    if (freq < 0) freq = 0;
    gchar *line = NULL;
    if (freq == 1)            /* 每周 */
        line = g_strdup_printf("%d %d * * 0 %s %s", mm, hh, cmd, SCHED_MARKER);
    else if (freq == 2)       /* 每月 */
        line = g_strdup_printf("%d %d 1 * * %s %s", mm, hh, cmd, SCHED_MARKER);
    else                      /* 每日 */
        line = g_strdup_printf("%d %d * * * %s %s", mm, hh, cmd, SCHED_MARKER);
    g_free(cmd);

    gchar *existing = read_crontab();
    gchar *clean = crontab_without_schedule(existing);
    g_free(existing);
    GString *newtab = g_string_new(clean);
    g_free(clean);
    if (newtab->len && newtab->str[newtab->len - 1] != '\n')
        g_string_append_c(newtab, '\n');
    g_string_append(newtab, line);
    g_string_append_c(newtab, '\n');
    write_crontab(newtab->str);
    g_free(line);
    g_string_free(newtab, TRUE);

    set_status(TR("已启用计划备份"));
    if (sched_status_label) {
        gchar *s = g_strdup_printf(TR("计划: %s %s"), TR("已启用"), time_text);
        gtk_label_set_text(GTK_LABEL(sched_status_label), s);
        g_free(s);
    }
}

/* 停用计划备份：从 crontab 中移除计划行 */
static void on_sched_disable(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (busy) return;
    if (!has_crontab()) {
        set_status(TR("系统无 crontab"));
        return;
    }
    gchar *existing = read_crontab();
    gchar *clean = crontab_without_schedule(existing);
    g_free(existing);
    write_crontab(clean);
    g_free(clean);

    set_status(TR("已停用计划备份"));
    if (sched_status_label)
        gtk_label_set_text(GTK_LABEL(sched_status_label), TR("计划: 未启用"));
}

/* 启动时刷新计划状态 */
static void refresh_schedule_status(void) {
    if (!sched_status_label) return;
    gchar *ct = read_crontab();
    gboolean enabled = ct && strstr(ct, SCHED_MARKER) != NULL;
    gtk_label_set_text(GTK_LABEL(sched_status_label),
                       enabled ? TR("计划: 已启用") : TR("计划: 未启用"));
    g_free(ct);
}

/* ---------- UI ---------- */
static void activate(GtkApplication *app, gpointer ud) {
    (void)ud;
    qy_load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    gtk_widget_set_name(GTK_WIDGET(win), "qy-backup-win");
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫备份"));
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 460);
    gtk_widget_set_size_request(GTK_WIDGET(win), 540, 420);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 标题 */
    GtkWidget *title = gtk_label_new(TR("备份 / 恢复"));
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

    /* 源目录 */
    GtkWidget *src_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *src_lb = gtk_label_new(TR("源目录:"));
    gtk_widget_set_halign(src_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(src_row), src_lb, FALSE, FALSE, 0);
    src_chooser = gtk_file_chooser_button_new(TR("选择源目录"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_widget_set_hexpand(src_chooser, TRUE);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(src_chooser), g_get_home_dir());
    gtk_box_pack_start(GTK_BOX(src_row), src_chooser, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), src_row, FALSE, FALSE, 0);

    /* 目标目录 */
    GtkWidget *dst_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *dst_lb = gtk_label_new(TR("目标目录:"));
    gtk_widget_set_halign(dst_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(dst_row), dst_lb, FALSE, FALSE, 0);
    dst_chooser = gtk_file_chooser_button_new(TR("选择目标目录"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_widget_set_hexpand(dst_chooser, TRUE);
    gchar *def_dst = g_build_filename(g_get_home_dir(), "backups", NULL);
    g_mkdir_with_parents(def_dst, 0755);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dst_chooser), def_dst);
    g_free(def_dst);
    gtk_box_pack_start(GTK_BOX(dst_row), dst_chooser, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), dst_row, FALSE, FALSE, 0);

    /* 备份名称 */
    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *name_lb = gtk_label_new(TR("备份名称:"));
    gtk_widget_set_halign(name_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(name_row), name_lb, FALSE, FALSE, 0);
    name_entry = gtk_entry_new();
    gchar *def_name = default_backup_name();
    gtk_entry_set_text(GTK_ENTRY(name_entry), def_name);
    g_free(def_name);
    gtk_box_pack_start(GTK_BOX(name_row), name_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), name_row, FALSE, FALSE, 0);

    /* 立即备份按钮 */
    backup_btn = gtk_button_new_with_label(TR("立即备份"));
    qy_add_class(backup_btn, "qy-backup-btn");
    gtk_widget_set_size_request(backup_btn, -1, 36);
    g_signal_connect(backup_btn, "clicked", G_CALLBACK(on_backup_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), backup_btn, FALSE, FALSE, 2);

    /* 状态 */
    status_label = gtk_label_new(TR("就绪"));
    gtk_widget_set_name(GTK_WIDGET(status_label), "qy-backup-status");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    /* ---------- 计划备份（crontab） ---------- */
    GtkWidget *sched_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sched_sep, FALSE, FALSE, 0);

    GtkWidget *sched_title = gtk_label_new(TR("计划备份"));
    gtk_widget_set_halign(sched_title, GTK_ALIGN_START);
    qy_add_class(sched_title, "qy-settings-group");
    gtk_box_pack_start(GTK_BOX(vbox), sched_title, FALSE, FALSE, 0);

    GtkWidget *sched_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sched_l = gtk_label_new(TR("频率"));
    gtk_box_pack_start(GTK_BOX(sched_row), sched_l, FALSE, FALSE, 0);
    sched_freq_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sched_freq_combo), TR("每日"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sched_freq_combo), TR("每周"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sched_freq_combo), TR("每月"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(sched_freq_combo), 0);
    gtk_box_pack_start(GTK_BOX(sched_row), sched_freq_combo, FALSE, FALSE, 0);

    GtkWidget *time_lb = gtk_label_new(TR("时间"));
    gtk_box_pack_start(GTK_BOX(sched_row), time_lb, FALSE, FALSE, 0);
    sched_time_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(sched_time_entry), 6);
    gtk_entry_set_text(GTK_ENTRY(sched_time_entry), "02:00");
    gtk_box_pack_start(GTK_BOX(sched_row), sched_time_entry, FALSE, FALSE, 0);

    GtkWidget *sched_enable_btn = gtk_button_new_with_label(TR("启用"));
    qy_add_class(sched_enable_btn, "qy-btn");
    g_signal_connect(sched_enable_btn, "clicked", G_CALLBACK(on_sched_enable), NULL);
    gtk_box_pack_start(GTK_BOX(sched_row), sched_enable_btn, FALSE, FALSE, 0);

    GtkWidget *sched_disable_btn = gtk_button_new_with_label(TR("停用计划"));
    qy_add_class(sched_disable_btn, "qy-btn");
    g_signal_connect(sched_disable_btn, "clicked", G_CALLBACK(on_sched_disable), NULL);
    gtk_box_pack_start(GTK_BOX(sched_row), sched_disable_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), sched_row, FALSE, FALSE, 0);

    sched_status_label = gtk_label_new(TR("计划: 未启用"));
    gtk_widget_set_halign(sched_status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), sched_status_label, FALSE, FALSE, 0);

    /* 备份列表 */
    GtkWidget *list_lb = gtk_label_new(TR("已有备份:"));
    gtk_widget_set_halign(list_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), list_lb, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    listbox = gtk_list_box_new();
    gtk_widget_set_name(GTK_WIDGET(listbox), "qy-backup-list");
    gtk_container_add(GTK_CONTAINER(sw), listbox);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    /* 恢复/删除按钮 */
    GtkWidget *op_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *b_restore = gtk_button_new_with_label(TR("恢复"));
    qy_add_class(b_restore, "qy-btn");
    GtkWidget *b_delete = gtk_button_new_with_label(TR("删除"));
    qy_add_class(b_delete, "qy-btn qy-btn-danger");
    g_signal_connect(b_restore, "clicked", G_CALLBACK(on_restore_clicked), NULL);
    g_signal_connect(b_delete, "clicked", G_CALLBACK(on_delete_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(op_bar), b_restore, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(op_bar), b_delete, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(op_bar), NULL, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), op_bar, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    refresh_backup_list();
    refresh_schedule_status();

    if (g_getenv("QYBACKUP_AUTO"))
        g_timeout_add(800, auto_backup, NULL);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.backup", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}