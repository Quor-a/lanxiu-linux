/* qymedia.c — 澜岫音乐播放器
 * 播放 WAV（ALSA aplay 后台），MP3/OGG/FLAC 自动选择外部解码器（mpg123/mpv/ffplay）。
 * 支持播放/暂停/继续/停止/音量；左侧播放列表（添加文件/清空/双击播放/上一首/下一首/循环）。
 * 播放中高亮当前曲目（#qy-media-now 橙色）。
 * 自动化: QYMEDIA_AUTO=WAV路径 启动后自动加载并播放。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"

static GtkWidget *file_label = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *btn_play = NULL;
static GtkWidget *btn_pause = NULL;
static GtkWidget *btn_stop = NULL;
static GtkWidget *playlist = NULL;      /* GtkListBox 播放列表 */
static GtkWidget *win = NULL;

static GPid player_pid = -1;
static gchar *current_file = NULL;
static gboolean paused = FALSE;
static gboolean player_manual_stop = FALSE;

/* 播放列表 */
static GPtrArray *track_list = NULL;    /* gchar* 路径 */
static int current_index = -1;          /* 当前曲目索引 */
static gboolean loop_enabled = FALSE;   /* 循环开关 */

static void on_next_clicked(GtkWidget *w, gpointer ud);   /* 前向声明 */

/* 检查 PATH 中是否存在程序（g_find_program_in_path 返回需 g_free） */
static gboolean has_program(const char *name) {
    gchar *p = g_find_program_in_path(name);
    gboolean ok = (p != NULL);
    g_free(p);
    return ok;
}

/* 切换暂停按钮图标（qy_icon_button 内部是图片子部件） */
static void set_pause_icon(QyIconId id) {
    if (!btn_pause) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(btn_pause));
    if (ch && GTK_IS_IMAGE(ch->data)) {
        GdkPixbuf *pb = qy_icon_pixbuf(id, 18, NULL);
        gtk_image_set_from_pixbuf(GTK_IMAGE(ch->data), pb);
        if (pb) g_object_unref(pb);
    }
    g_list_free(ch);
}

/* 停止当前播放 */
static void stop_player(void) {
    player_manual_stop = TRUE;
    if (player_pid > 0)
        kill(player_pid, SIGKILL);
    paused = FALSE;
    set_pause_icon(QY_ICON_PAUSE);
}

/* 播放器进程自然退出（或被杀）后的回收 */
static void on_player_exited(GPid pid, gint status, gpointer ud) {
    (void)ud;
    g_spawn_close_pid(pid);
    /* 过期 watch（新曲目已启动）直接忽略 */
    if (pid != player_pid) return;
    player_pid = -1;
    paused = FALSE;
    set_pause_icon(QY_ICON_PAUSE);
    if (player_manual_stop) {
        player_manual_stop = FALSE;
        return;
    }
    /* 自然播放结束：循环开启时自动跳到下一首（到尾回第一首） */
    if (loop_enabled)
        on_next_clicked(NULL, NULL);
}

/* 播放文件 */
static void play_file(const char *path) {
    stop_player();
    if (!path || !path[0]) return;
    g_free(current_file);
    current_file = g_strdup(path);
    gtk_label_set_text(GTK_LABEL(file_label), path);
    char st[512];
    g_snprintf(st, sizeof st, "%s: %s...", TR("正在播放"), path);
    gtk_label_set_text(GTK_LABEL(status_label), st);

    /* 按扩展名选择解码器：WAV 用 aplay；MP3/OGG 用 mpg123/mpv/ffplay；FLAC 用 mpv/ffplay */
    const char *ext = "";
    const char *dot = strrchr(path, '.');
    if (dot) ext = dot + 1;
    const char *player = "aplay";
    const char *extra = NULL;
    if (g_ascii_strcasecmp(ext, "mp3") == 0 || g_ascii_strcasecmp(ext, "ogg") == 0 ||
        g_ascii_strcasecmp(ext, "oga") == 0) {
        if (has_program("mpg123")) player = "mpg123";
        else if (has_program("mpv")) { player = "mpv"; extra = "--no-video"; }
        else if (has_program("ffplay")) { player = "ffplay"; extra = "-nodisp -autoexit"; }
        else player = NULL;
    } else if (g_ascii_strcasecmp(ext, "flac") == 0) {
        if (has_program("mpv")) { player = "mpv"; extra = "--no-video"; }
        else if (has_program("ffplay")) { player = "ffplay"; extra = "-nodisp -autoexit"; }
        else player = NULL;
    }
    if (!player) {
        g_printerr("QYMEDIADBG: play %s no decoder\n", path);
        g_snprintf(st, sizeof st, "%s: %s", TR("无法播放"), TR("未找到解码器（请安装 mpg123/ffmpeg）"));
        gtk_label_set_text(GTK_LABEL(status_label), st);
        return;
    }
    gchar *cmd;
    gchar *qpath = g_shell_quote(path);
    if (extra)
        cmd = g_strdup_printf("%s %s %s", player, extra, qpath);
    else
        cmd = g_strdup_printf("%s -q %s", player, qpath);
    g_free(qpath);
    g_printerr("QYMEDIADBG: play %s player=%s\n", path, player);
    gchar **argv = NULL;
    g_shell_parse_argv(cmd, NULL, &argv, NULL);
    g_free(cmd);
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &player_pid, &err)) {
        g_snprintf(st, sizeof st, "%s: %s", TR("无法播放"), err ? err->message : "?");
        gtk_label_set_text(GTK_LABEL(status_label), st);
        g_clear_error(&err);
        player_pid = -1;
    } else {
        player_manual_stop = FALSE;
        g_child_watch_add(player_pid, on_player_exited, NULL);
        paused = FALSE;
        set_pause_icon(QY_ICON_PAUSE);
    }
    g_strfreev(argv);
}

/* ---------- 播放列表 ---------- */
static void highlight_current_row(void) {
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    int idx = 0;
    for (GList *l = ch; l; l = l->next) {
        if (idx == current_index)
            gtk_widget_set_name(GTK_WIDGET(l->data), "qy-media-now");
        else
            gtk_widget_set_name(GTK_WIDGET(l->data), NULL);
        idx++;
    }
    g_list_free(ch);
}

static void play_index(int i) {
    if (i < 0 || i >= (int)track_list->len) return;
    current_index = i;
    play_file((const char *)g_ptr_array_index(track_list, i));
    highlight_current_row();
}

static void on_next_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (track_list->len == 0) return;
    if (current_index < 0) { play_index(0); return; }
    int next = current_index + 1;
    if (next >= (int)track_list->len) {
        if (loop_enabled)
            next = 0;
        else {
            gtk_label_set_text(GTK_LABEL(status_label), TR("已是最后一首"));
            return;
        }
    }
    play_index(next);
}

static void on_prev_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (track_list->len == 0) return;
    if (current_index < 0) { play_index(0); return; }
    int prev = current_index - 1;
    if (prev < 0) {
        if (loop_enabled)
            prev = (int)track_list->len - 1;
        else {
            gtk_label_set_text(GTK_LABEL(status_label), TR("已是第一首"));
            return;
        }
    }
    play_index(prev);
}

/* 添加一个曲目到播放列表 */
static void add_to_playlist(const char *path) {
    if (!path || !path[0]) return;
    for (guint i = 0; i < track_list->len; i++) {
        if (strcmp((const char *)g_ptr_array_index(track_list, i), path) == 0)
            return;
    }
    g_ptr_array_add(track_list, g_strdup(path));
    gchar *base = g_path_get_basename(path);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *lbl = gtk_label_new(base);
    g_free(base);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(row), lbl);
    g_object_set_data_full(G_OBJECT(row), "track-path", g_strdup(path), g_free);
    gtk_list_box_insert(GTK_LIST_BOX(playlist), row, -1);
}

/* GtkFileChooserNative 多选添加音频 */
static void on_add_files(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    GtkFileChooserNative *dlg = gtk_file_chooser_native_new(
        TR("添加音频文件"), GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN, TR("添加"), TR("取消"));
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

    GtkFileFilter *f_audio = gtk_file_filter_new();
    gtk_file_filter_set_name(f_audio, TR("音频文件"));
    gtk_file_filter_add_pattern(f_audio, "*.wav");
    gtk_file_filter_add_pattern(f_audio, "*.mp3");
    gtk_file_filter_add_pattern(f_audio, "*.ogg");
    gtk_file_filter_add_pattern(f_audio, "*.oga");
    gtk_file_filter_add_pattern(f_audio, "*.flac");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f_audio);

    GtkFileFilter *f_all = gtk_file_filter_new();
    gtk_file_filter_set_name(f_all, TR("所有文件"));
    gtk_file_filter_add_pattern(f_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f_all);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList *s = files; s; s = s->next)
            add_to_playlist((const char *)s->data);
        g_slist_free_full(files, g_free);
    }
    g_object_unref(dlg);
}

static void on_clear_playlist(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    stop_player();
    g_ptr_array_set_size(track_list, 0);
    current_index = -1;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    g_free(current_file);
    current_file = NULL;
    gtk_label_set_text(GTK_LABEL(file_label), TR("未选择音频"));
    gtk_label_set_text(GTK_LABEL(status_label), TR("播放列表已清空"));
}

/* 双击播放 */
static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    if (!row) return;
    const char *path = g_object_get_data(G_OBJECT(row), "track-path");
    if (!path) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    int idx = 0;
    for (GList *l = ch; l; l = l->next) {
        if (l->data == (gpointer)row) break;
        idx++;
    }
    g_list_free(ch);
    play_index(idx);
}

/* ---------- 原有控制 ---------- */
static void on_open(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        TR("打开音频"), NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        TR("取消"), GTK_RESPONSE_CANCEL, TR("打开"), GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            add_to_playlist(path);
            play_index((int)track_list->len - 1);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_play(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (current_file)
        play_file(current_file);
}

static void on_pause(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (player_pid <= 0) return;
    if (!paused) {
        kill(player_pid, SIGSTOP);
        paused = TRUE;
        set_pause_icon(QY_ICON_PLAY);
        gtk_label_set_text(GTK_LABEL(status_label), TR("已暂停"));
    } else {
        kill(player_pid, SIGCONT);
        paused = FALSE;
        set_pause_icon(QY_ICON_PAUSE);
        gtk_label_set_text(GTK_LABEL(status_label), TR("正在播放"));
    }
}

static void on_stop(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    stop_player();
    gtk_label_set_text(GTK_LABEL(status_label), TR("已停止"));
}

/* 音量控制（amixer，找不到时仅更新状态不崩溃） */
static void on_volume_changed(GtkRange *range, gpointer ud) {
    (void)ud;
    int vol = (int)gtk_range_get_value(range);
    if (has_program("amixer")) {
        gchar *cmd = g_strdup_printf("amixer -q set Master %d%%", vol);
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
    }
}

/* 循环开关 */
static void on_loop_toggled(GtkToggleButton *btn, gpointer ud) {
    (void)ud;
    loop_enabled = gtk_toggle_button_get_active(btn);
}

/* 自动化: QYMEDIA_AUTO */
static gboolean auto_play(gpointer p) {
    const char *path = (const char *)p;
    add_to_playlist(path);
    play_index((int)track_list->len - 1);
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* 关闭时停止播放器 */
static gboolean on_win_delete(GtkWidget *w, GdkEventAny *ev, gpointer ud) {
    (void)w; (void)ev; (void)ud;
    stop_player();
    return FALSE;
}

static void build_ui(void) {
    qy_load_theme();
    track_list = g_ptr_array_new_with_free_func(g_free);
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫音乐"));
    gtk_widget_set_name(GTK_WIDGET(win), "qy-media-win");
    qy_window_setup(GTK_WINDOW(win), 640, 380, 560, 300, FALSE);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_win_delete), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    qy_add_class(vbox, "qy-app-surface");
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *title = gtk_label_new(TR("音乐"));
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), main_hbox, TRUE, TRUE, 0);

    /* ---------- 左侧播放列表 ---------- */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left_vbox, 220, -1);

    GtkWidget *pl_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *b_add = gtk_button_new_with_label(TR("添加文件"));
    qy_add_class(b_add, "qy-btn");
    GtkWidget *b_clear = gtk_button_new_with_label(TR("清空"));
    qy_add_class(b_clear, "qy-btn qy-btn-danger");
    g_signal_connect(b_add, "clicked", G_CALLBACK(on_add_files), NULL);
    g_signal_connect(b_clear, "clicked", G_CALLBACK(on_clear_playlist), NULL);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_add, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_vbox), pl_bar, FALSE, FALSE, 0);

    GtkWidget *pl_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pl_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    playlist = gtk_list_box_new();
    gtk_widget_set_name(GTK_WIDGET(playlist), "qy-media-list");
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(playlist), FALSE);
    g_signal_connect(playlist, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(pl_sw), playlist);
    gtk_box_pack_start(GTK_BOX(left_vbox), pl_sw, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(main_hbox), left_vbox, FALSE, TRUE, 0);

    /* ---------- 右侧控制区 ---------- */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(main_hbox), right_vbox, TRUE, TRUE, 0);

    file_label = gtk_label_new(TR("未选择音频"));
    qy_add_class(file_label, "qy-mon-info");
    gtk_widget_set_halign(file_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right_vbox), file_label, FALSE, FALSE, 0);

    status_label = gtk_label_new(TR("就绪"));
    qy_add_class(status_label, "qy-mon-info");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right_vbox), status_label, FALSE, FALSE, 0);

    /* 播放/停止控制条 */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
    GtkWidget *b_open = gtk_button_new_with_label(TR("打开音频"));
    qy_add_class(b_open, "qy-btn");
    GtkWidget *b_prev = qy_icon_button(QY_ICON_PREV, 18, TR("上一首"), NULL);
    btn_play = qy_icon_button(QY_ICON_PLAY, 18, TR("播放"), NULL);
    btn_pause = qy_icon_button(QY_ICON_PAUSE, 18, TR("暂停"), NULL);
    btn_stop = gtk_button_new_with_label(TR("停止"));
    GtkWidget *b_next = qy_icon_button(QY_ICON_NEXT, 18, TR("下一首"), NULL);
    g_signal_connect(b_open, "clicked", G_CALLBACK(on_open), NULL);
    g_signal_connect(b_prev, "clicked", G_CALLBACK(on_prev_clicked), NULL);
    g_signal_connect(btn_play, "clicked", G_CALLBACK(on_play), NULL);
    g_signal_connect(btn_pause, "clicked", G_CALLBACK(on_pause), NULL);
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop), NULL);
    g_signal_connect(b_next, "clicked", G_CALLBACK(on_next_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), b_open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn_pause, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), bar, FALSE, FALSE, 0);

    /* 循环 + 音量 */
    GtkWidget *bar2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *loop_btn = gtk_toggle_button_new_with_label(TR("循环"));
    g_signal_connect(loop_btn, "toggled", G_CALLBACK(on_loop_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), loop_btn, FALSE, FALSE, 0);
    GtkWidget *vol_lb = gtk_label_new(TR("音量:"));
    gtk_box_pack_start(GTK_BOX(bar2), vol_lb, FALSE, FALSE, 0);
    GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(vol_scale), FALSE);
    gtk_range_set_value(GTK_RANGE(vol_scale), 80);
    gtk_widget_set_hexpand(vol_scale, TRUE);
    g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_volume_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), vol_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), bar2, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    const char *auto_path = g_getenv("QYMEDIA_AUTO");
    if (auto_path)
        g_timeout_add(800, auto_play, g_strdup(auto_path));
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    build_ui();
    gtk_main();
    return 0;
}