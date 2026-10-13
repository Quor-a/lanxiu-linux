/* qymedia.c — 澜岫音乐播放器
 * 播放 WAV（ALSA aplay 后台），MP3/OGG/FLAC 自动选择外部解码器（mpg123/mpv/ffplay）。
 * 支持播放/暂停/继续/停止/音量；左侧播放列表（添加文件/清空/双击播放/上一首/下一首/循环）。
 * 播放中高亮当前曲目（#qy-media-now 橙色）。
 * 自动化: QYMEDIA_AUTO=WAV路径 启动后自动加载并播放。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyicon.h"
#include "qyl10n.h"

static GtkWidget *file_label = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *btn_play = NULL;
static GtkWidget *btn_pause = NULL;
static GtkWidget *btn_stop = NULL;
static GtkWidget *playlist = NULL;      /* GtkListBox 播放列表 */
static GtkWidget *win = NULL;

/* ---------- 封面显示（同目录 cover.jpg/png/webp，128x128） ---------- */
static GtkWidget *cover_stack = NULL;   /* img/empty 二选一 */
static GtkWidget *cover_img = NULL;     /* 有封面时显示 GdkPixbuf */
static GtkWidget *cover_empty = NULL;   /* 无封面时 qy_draw_empty_state(kind="note") */

/* ---------- 均衡器（占位：6 段滑杆，写 /etc/qymedia-eq.conf，无真实音频后端） ---------- */
static GtkWidget *eq_scales[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
static void eq_save(void);   /* 前向声明（on_eq_preset 提前引用） */
static const char *eq_band_names[6] = { "63Hz", "250Hz", "1kHz", "4kHz", "8kHz", "16kHz" };
static double g_play_speed = 1.0;   /* 播放速度（release 267） */
static GtkWidget *speed_combo = NULL;

static void on_speed_changed(GtkComboBox *combo, gpointer ud) {
    (void)ud;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    g_play_speed = id ? g_ascii_strtod(id, NULL) : 1.0;
    if (g_play_speed <= 0.0) g_play_speed = 1.0;
    /* 记忆播放速度（release 275）：~/.config/lanxiu/qymedia-speed.conf */
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        g_mkdir_with_parents(cdir, 0755);
        gchar *cp = g_build_filename(cdir, "qymedia-speed.conf", NULL);
        gchar *sv = g_strdup_printf("%.2f", g_play_speed);
        g_file_set_contents(cp, sv, -1, NULL);
        g_free(sv); g_free(cp); g_free(cdir);
    }
    gchar *s = g_strdup_printf(TR("播放速度 %.1fx（下次播放生效）"), g_play_speed);
    if (status_label) gtk_label_set_text(GTK_LABEL(status_label), s);
    g_free(s);
}

static void cover_update(const char *audio_path);   /* 前向声明 */

/* ---------- 本地歌词（LRC） ---------- */
typedef struct {
    double time;       /* 秒 */
    gchar *text;
} LyricLine;

static GPtrArray *lyrics = NULL;        /* LyricLine*，按时间排序 */
static GtkWidget *lyric_view = NULL;    /* #qy-media-lyric */
static GtkTextBuffer *lyric_buf = NULL;
static GtkTextTag *lyric_hl_tag = NULL; /* 当前行高亮 tag（橙底白字） */
static GPtrArray *lyric_offsets = NULL;  /* 每行在 buffer 中的起始偏移 */
static gint64 lyric_seg_start = 0;       /* 当前播放段起始 monotonic us */
static double lyric_elapsed = 0.0;       /* 已累计播放秒数（暂停时冻结） */

static void lyric_update_highlight(double t);   /* 前向声明 */

static GPid player_pid = -1;
static gchar *current_file = NULL;
static gboolean paused = FALSE;
static gboolean player_manual_stop = FALSE;

/* 播放列表 */
static GPtrArray *track_list = NULL;    /* gchar* 路径 */
static int current_index = -1;          /* 当前曲目索引 */
static gboolean loop_enabled = FALSE;   /* 循环开关 */

static void on_next_clicked(GtkWidget *w, gpointer ud);   /* 前向声明 */
static void playlist_save(void);                          /* 前向声明（add_to_playlist 自动保存用） */
static void add_to_playlist(const char *path);            /* 前向声明（on_track_remove 提前引用） */

/* 检查 PATH 中是否存在程序（g_find_program_in_path 返回需 g_free） */
static gboolean has_program(const char *name) {
    gchar *p = g_find_program_in_path(name);
    gboolean ok = (p != NULL);
    g_free(p);
    return ok;
}

/* 清空已加载歌词 */
static void lyric_clear(void) {
    if (lyrics) {
        for (guint i = 0; i < lyrics->len; i++) {
            LyricLine *ll = g_ptr_array_index(lyrics, i);
            g_free(ll->text);
            g_slice_free(LyricLine, ll);
        }
        g_ptr_array_set_size(lyrics, 0);
    }
    if (lyric_offsets) g_ptr_array_set_size(lyric_offsets, 0);
    if (lyric_buf) gtk_text_buffer_set_text(lyric_buf, "", -1);
}

static int lyric_cmp(const void *a, const void *b) {
    const LyricLine *la = *(const LyricLine * const *)a;
    const LyricLine *lb = *(const LyricLine * const *)b;
    return la->time < lb->time ? -1 : (la->time > lb->time ? 1 : 0);
}

/* 高亮当前时间对应的歌词行（橙底白字 tag） */
static void lyric_update_highlight(double t) {
    if (!lyrics || lyrics->len == 0 || !lyric_buf || !lyric_offsets || !lyric_hl_tag)
        return;
    int cur = 0;
    for (guint i = 0; i < lyrics->len; i++) {
        LyricLine *ll = g_ptr_array_index(lyrics, i);
        if (ll->time <= t) cur = (int)i; else break;
    }
    if (cur < 0) cur = 0;
    if (cur >= (int)lyrics->len) cur = (int)lyrics->len - 1;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(lyric_buf, &start, &end);
    gtk_text_buffer_remove_tag(lyric_buf, lyric_hl_tag, &start, &end);

    guint off = GPOINTER_TO_UINT(g_ptr_array_index(lyric_offsets, cur));
    guint next_off;
    if (cur + 1 < (int)lyrics->len)
        next_off = GPOINTER_TO_UINT(g_ptr_array_index(lyric_offsets, cur + 1)) - 1; /* 去掉行尾换行 */
    else {
        gtk_text_buffer_get_end_iter(lyric_buf, &end);
        next_off = gtk_text_iter_get_offset(&end);
    }
    gtk_text_buffer_get_iter_at_offset(lyric_buf, &start, off);
    gtk_text_buffer_get_iter_at_offset(lyric_buf, &end, next_off);
    gtk_text_buffer_apply_tag(lyric_buf, lyric_hl_tag, &start, &end);

    gtk_text_buffer_get_iter_at_offset(lyric_buf, &start, off);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(lyric_view), &start, 0.0, FALSE, 0, 0);
}

/* 加载同名 .lrc：同目录 <歌名>.lrc，解析 [mm:ss.xx] 行 */
static void lyric_load(const char *audio_path) {
    lyric_clear();
    if (!audio_path || !audio_path[0]) {
        if (lyric_buf) gtk_text_buffer_set_text(lyric_buf, TR("无歌词文件"), -1);
        return;
    }

    gchar *dir = g_path_get_dirname(audio_path);
    gchar *base = g_path_get_basename(audio_path);
    char *dot = strrchr(base, '.');
    gchar *stem = g_strndup(base, dot ? (gsize)(dot - base) : strlen(base));
    gchar *lrcname = g_strdup_printf("%s.lrc", stem);
    gchar *lrcpath = g_build_filename(dir, lrcname, NULL);
    g_free(dir); g_free(base); g_free(stem); g_free(lrcname);

    gchar *content = NULL;
    if (!g_file_get_contents(lrcpath, &content, NULL, NULL) || !content) {
        g_free(content); g_free(lrcpath);
        if (lyric_buf) gtk_text_buffer_set_text(lyric_buf, TR("无歌词文件"), -1);
        return;
    }
    g_free(lrcpath);

    if (!lyrics) lyrics = g_ptr_array_new();
    char **lines = g_strsplit(content, "\n", -1);
    g_free(content);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!line[0]) continue;
        /* 跳过元数据标签 [ti:][ar:][al:][by:][offset:] */
        if (line[0] == '[' &&
            (g_ascii_strncasecmp(line + 1, "ti:", 3) == 0 ||
             g_ascii_strncasecmp(line + 1, "ar:", 3) == 0 ||
             g_ascii_strncasecmp(line + 1, "al:", 3) == 0 ||
             g_ascii_strncasecmp(line + 1, "by:", 3) == 0 ||
             g_ascii_strncasecmp(line + 1, "offset:", 7) == 0))
            continue;
        char *p = line;
        double times[8];
        int ntimes = 0;
        while (p[0] == '[') {
            char *rb = strchr(p, ']');
            if (!rb) break;
            int mm;
            double ss;
            if (sscanf(p + 1, "%d:%lf", &mm, &ss) == 2 && ntimes < 8)
                times[ntimes++] = mm * 60.0 + ss;
            p = rb + 1;
        }
        if (ntimes > 0 && p[0]) {
            for (int k = 0; k < ntimes; k++) {
                LyricLine *ll = g_slice_new(LyricLine);
                ll->time = times[k];
                ll->text = g_strdup(p);
                g_ptr_array_add(lyrics, ll);
            }
        }
    }
    g_strfreev(lines);

    if (lyrics->len == 0) {
        if (lyric_buf) gtk_text_buffer_set_text(lyric_buf, TR("无歌词文件"), -1);
        return;
    }

    qsort(lyrics->pdata, lyrics->len, sizeof(gpointer), lyric_cmp);

    if (!lyric_buf) return;
    gtk_text_buffer_set_text(lyric_buf, "", -1);
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(lyric_buf, &iter);
    for (guint i = 0; i < lyrics->len; i++) {
        LyricLine *ll = g_ptr_array_index(lyrics, i);
        if (i > 0) gtk_text_buffer_insert(lyric_buf, &iter, "\n", 1);
        guint off = gtk_text_iter_get_offset(&iter);
        g_ptr_array_add(lyric_offsets, GUINT_TO_POINTER(off));
        gtk_text_buffer_insert(lyric_buf, &iter, ll->text, -1);
    }
    if (!lyric_hl_tag)
        lyric_hl_tag = gtk_text_buffer_create_tag(lyric_buf, "qy-media-lyric-hl",
            "background", "#FF5C1A", "foreground", "#ffffff",
            "weight", PANGO_WEIGHT_BOLD, NULL);
    lyric_update_highlight(0.0);
}

/* 当前播放到的时间（暂停冻结） */
static double lyric_current_time(void) {
    if (lyric_seg_start > 0)
        return lyric_elapsed + (g_get_monotonic_time() - lyric_seg_start) / 1000000.0;
    return lyric_elapsed;
}

/* 定时器：播放中滚动高亮当前歌词行 */
static gboolean lyric_tick(gpointer ud) {
    (void)ud;
    if (lyrics && lyrics->len > 0 && lyric_view)
        lyric_update_highlight(lyric_current_time());
    return G_SOURCE_CONTINUE;
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
    lyric_seg_start = 0;
    lyric_elapsed = 0.0;
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
    lyric_seg_start = 0;
    lyric_elapsed = 0.0;
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
    /* 窗口标题显示当前曲目（release 275） */
    {
        gchar *base = g_path_get_basename(path);
        gchar *title = g_strdup_printf(TR("澜岫音乐 — %s"), base);
        gtk_window_set_title(GTK_WINDOW(win), title);
        g_free(title); g_free(base);
    }
    cover_update(path);
    char st[512];
    g_snprintf(st, sizeof st, "%s: %s...", TR("正在播放"), path);
    gtk_label_set_text(GTK_LABEL(status_label), st);
    /* 自动查找同名 .lrc 歌词（不阻塞播放） */
    lyric_load(path);

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
    /* 播放速度（release 267）：mpv --speed / ffplay -af atempo */
    if (g_play_speed != 1.0 && player && (strstr(player, "mpv") || strstr(player, "ffplay"))) {
        gchar *speed_opt = g_strdup_printf(strstr(player, "mpv") ? "--speed=%.2f" : "-af atempo=%.2f", g_play_speed);
        if (extra)
            cmd = g_strdup_printf("%s %s %s %s", player, extra, speed_opt, qpath);
        else
            cmd = g_strdup_printf("%s -q %s %s", player, speed_opt, qpath);
        g_free(speed_opt);
    } else if (extra)
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
        lyric_elapsed = 0.0;
        lyric_seg_start = g_get_monotonic_time();
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
/* 播放列表右键「从播放列表移除」（release 273） */
static void on_track_remove(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    const char *path = (const char *)ud;
    if (path && path[0]) {
        for (guint i = 0; i < track_list->len; i++) {
            if (strcmp((const char *)g_ptr_array_index(track_list, i), path) == 0) {
                g_ptr_array_remove_index(track_list, i);
                break;
            }
        }
    }
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    for (guint i = 0; i < track_list->len; i++)
        add_to_playlist((const char *)g_ptr_array_index(track_list, i));
    playlist_save();
    g_free(ud);
}

static gboolean on_track_row_press(GtkWidget *row, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->button != 3) return FALSE;
    const char *path = g_object_get_data(G_OBJECT(row), "track-path");
    if (!path) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("从播放列表移除"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_track_remove), (gpointer)g_strdup(path));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), row, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

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
    gtk_widget_set_tooltip_text(row, path);   /* 悬停显示完整路径 */
    g_object_set_data_full(G_OBJECT(row), "track-path", g_strdup(path), g_free);
    gtk_list_box_insert(GTK_LIST_BOX(playlist), row, -1);
    g_signal_connect(row, "button-press-event", G_CALLBACK(on_track_row_press), NULL);
    playlist_save();   /* 添加后自动保存 */
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

/* ---------- 播放列表持久化（~/.config/lanxiu/qymedia-playlist.txt） ---------- */
static gchar *playlist_path(void) {
    return g_build_filename(g_get_home_dir(), ".config/lanxiu/qymedia-playlist.txt", NULL);
}

/* 每行一个文件路径，保存当前播放列表 */
static void playlist_save(void) {
    if (!track_list) return;
    gchar *path = playlist_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < track_list->len; i++)
        g_string_append_printf(s, "%s\n", (const char *)g_ptr_array_index(track_list, i));
    GError *err = NULL;
    if (!g_file_set_contents(path, s->str, (gssize)s->len, &err)) {
        g_printerr("QYMEDIADBG: playlist save failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    } else {
        g_printerr("QYMEDIADBG: playlist saved (%u tracks)\n", track_list->len);
    }
    g_string_free(s, TRUE);
    g_free(path);
}

/* 启动时加载上次保存的播放列表 */
static void playlist_load(void) {
    if (!track_list || !playlist) return;
    gchar *path = playlist_path();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content && content[0]) {
        char **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (line[0])
                add_to_playlist(line);
        }
        g_strfreev(lines);
        g_printerr("QYMEDIADBG: playlist loaded from %s\n", path);
    }
    g_free(content);
    g_free(path);
}

/* 工具栏「保存列表」 */
static void on_save_playlist(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    playlist_save();
    gtk_label_set_text(GTK_LABEL(status_label), TR("播放列表已保存"));
}

static void on_clear_playlist(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (track_list && track_list->len > 0) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(w)),
                                                GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_YES_NO,
                                                TR("确认清空播放列表?"));
        int ans = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (ans != GTK_RESPONSE_YES) return;
    }
    stop_player();
    g_ptr_array_set_size(track_list, 0);
    current_index = -1;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    g_free(current_file);
    current_file = NULL;
    cover_update(NULL);
    gtk_label_set_text(GTK_LABEL(file_label), TR("未选择音频"));
    gtk_label_set_text(GTK_LABEL(status_label), TR("播放列表已清空"));
    playlist_save();   /* 清空后持久化（空列表） */
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
        lyric_elapsed = lyric_current_time();
        lyric_seg_start = 0;
        set_pause_icon(QY_ICON_PLAY);
        gtk_label_set_text(GTK_LABEL(status_label), TR("已暂停"));
    } else {
        kill(player_pid, SIGCONT);
        paused = FALSE;
        lyric_seg_start = g_get_monotonic_time();
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
    /* 记忆音量（release 275）：写入 ~/.config/lanxiu/qymedia-vol.conf */
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        g_mkdir_with_parents(cdir, 0755);
        gchar *cp = g_build_filename(cdir, "qymedia-vol.conf", NULL);
        gchar *s = g_strdup_printf("%d", vol);
        g_file_set_contents(cp, s, -1, NULL);
        g_free(s); g_free(cp); g_free(cdir);
    }
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

/* ---------- 封面显示 ---------- */

/* 无封面时的空状态插图（qy_draw_empty_state kind="note"） */
static gboolean cover_empty_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    qy_draw_empty_state(cr, a.width, a.height, "note");
    return FALSE;
}

/* 根据正在播放的音频路径查找同目录 cover.jpg/png/webp 并显示；
 * 找不到封面时回退到空状态插图。 */
static void cover_update(const char *audio_path) {
    if (!cover_stack) return;
    GdkPixbuf *pb = NULL;
    if (audio_path && audio_path[0]) {
        gchar *dir = g_path_get_dirname(audio_path);
        static const char *names[] = { "cover.jpg", "cover.png", "cover.webp", NULL };
        for (int i = 0; names[i] && !pb; i++) {
            gchar *full = g_build_filename(dir, names[i], NULL);
            pb = gdk_pixbuf_new_from_file_at_scale(full, 128, 128, TRUE, NULL);
            g_free(full);
        }
        g_free(dir);
    }
    if (pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(cover_img), pb);
        g_object_unref(pb);
        gtk_stack_set_visible_child_name(GTK_STACK(cover_stack), "img");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(cover_stack), "empty");
    }
}

/* ---------- 均衡器（占位实现） ---------- */

/* 预设曲线（6 段：63Hz/250Hz/1kHz/4kHz/8kHz/16kHz）release 270 */
static const double eq_flat[6] = { 0, 0, 0, 0, 0, 0 };
static const double eq_pop[6]  = { 3, 1, 0, 1, 2, 3 };
static const double eq_rock[6] = { 4, 2, -1, 1, 3, 4 };
static const double eq_aco[6]  = { 2, 1, 0, 1, 2, 2 };

static void on_eq_preset(GtkButton *b, gpointer ud) {
    (void)b;
    const double *vals = (const double *)ud;
    for (int i = 0; i < 6; i++)
        if (eq_scales[i]) gtk_range_set_value(GTK_RANGE(eq_scales[i]), vals[i]);
    eq_save();
}

/* 把 6 段滑杆当前值写入 ~/.config/lanxiu/qymedia-eq.conf（[eq] bandN=值） */
static void eq_save(void) {
    GString *s = g_string_new("[eq]\n");
    for (int i = 0; i < 6; i++) {
        double v = eq_scales[i] ? gtk_range_get_value(GTK_RANGE(eq_scales[i])) : 0.0;
        g_string_append_printf(s, "band%d=%.0f\n", i + 1, v);
    }
    GError *err = NULL;
    gchar *eqdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(eqdir, 0755);
    gchar *eqpath = g_build_filename(eqdir, "qymedia-eq.conf", NULL);
    if (!g_file_set_contents(eqpath, s->str, (gssize)s->len, &err)) {
        g_printerr("QYMEDIADBG: eq save failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    } else {
        g_printerr("QYMEDIADBG: eq saved to %s\n", eqpath);
    }
    g_free(eqpath); g_free(eqdir);
    g_string_free(s, TRUE);
}

static void on_eq_changed(GtkRange *r, gpointer ud) {
    (void)r; (void)ud;
    eq_save();
}

/* 工具栏「均衡器」按钮: 弹出 6 个垂直滑杆的 GtkPopover */
static void on_eq_clicked(GtkButton *b, gpointer ud) {
    (void)ud;
    static GtkWidget *popover = NULL;
    if (!popover) {
        popover = gtk_popover_new(GTK_WIDGET(b));
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
        for (int i = 0; i < 6; i++) {
            GtkWidget *lbl = gtk_label_new(eq_band_names[i]);
            gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
            gtk_grid_attach(GTK_GRID(grid), lbl, i, 0, 1, 1);
            GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -12, 12, 1);
            gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
            gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
            gtk_widget_set_size_request(scale, 44, 140);
            gtk_range_set_value(GTK_RANGE(scale), 0);
            eq_scales[i] = scale;
            g_signal_connect(scale, "value-changed", G_CALLBACK(on_eq_changed), NULL);
            gtk_grid_attach(GTK_GRID(grid), scale, i, 1, 1, 1);
        }
        /* 加载已保存均衡器配置（release 270） */
        gchar *eqpath = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qymedia-eq.conf", NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, eqpath, G_KEY_FILE_NONE, NULL)) {
            for (int i = 0; i < 6; i++) {
                gchar *key = g_strdup_printf("band%d", i + 1);
                gint val = g_key_file_get_integer(kf, "eq", key, NULL);
                g_free(key);
                if (eq_scales[i]) gtk_range_set_value(GTK_RANGE(eq_scales[i]), val);
            }
        }
        g_key_file_free(kf);
        g_free(eqpath);
        /* 预设按钮（release 270） */
        GtkWidget *preset_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *b_flat = gtk_button_new_with_label(TR("平直"));
        GtkWidget *b_pop  = gtk_button_new_with_label(TR("流行"));
        GtkWidget *b_rock = gtk_button_new_with_label(TR("摇滚"));
        GtkWidget *b_aco  = gtk_button_new_with_label(TR("原声"));
        qy_add_class(b_flat, "qy-btn"); qy_add_class(b_pop, "qy-btn");
        qy_add_class(b_rock, "qy-btn"); qy_add_class(b_aco, "qy-btn");
        g_signal_connect(b_flat, "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_flat);
        g_signal_connect(b_pop,  "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_pop);
        g_signal_connect(b_rock, "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_rock);
        g_signal_connect(b_aco,  "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_aco);
        gtk_box_pack_start(GTK_BOX(preset_row), b_flat, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(preset_row), b_pop, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(preset_row), b_rock, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(preset_row), b_aco, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), preset_row, 0, 2, 6, 1);
        gtk_container_add(GTK_CONTAINER(popover), grid);
    }
    gtk_popover_popup(GTK_POPOVER(popover));
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
    GtkWidget *b_save = gtk_button_new_with_label(TR("保存列表"));
    qy_add_class(b_save, "qy-btn");
    GtkWidget *b_clear = gtk_button_new_with_label(TR("清空列表"));
    qy_add_class(b_clear, "qy-btn qy-btn-danger");
    g_signal_connect(b_add, "clicked", G_CALLBACK(on_add_files), NULL);
    g_signal_connect(b_save, "clicked", G_CALLBACK(on_save_playlist), NULL);
    g_signal_connect(b_clear, "clicked", G_CALLBACK(on_clear_playlist), NULL);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_add, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_save, FALSE, FALSE, 0);
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

    /* 封面显示: 左侧 128x128，右侧为文件/状态信息 */
    GtkWidget *cover_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    cover_stack = gtk_stack_new();
    cover_img = gtk_image_new();
    gtk_widget_set_size_request(cover_img, 128, 128);
    gtk_widget_set_halign(cover_img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(cover_img, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(cover_stack), cover_img, "img");
    cover_empty = gtk_drawing_area_new();
    gtk_widget_set_size_request(cover_empty, 128, 128);
    g_signal_connect(cover_empty, "draw", G_CALLBACK(cover_empty_draw), NULL);
    gtk_stack_add_named(GTK_STACK(cover_stack), cover_empty, "empty");
    gtk_stack_set_visible_child_name(GTK_STACK(cover_stack), "empty");
    gtk_box_pack_start(GTK_BOX(cover_hbox), cover_stack, FALSE, FALSE, 0);

    GtkWidget *cover_info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(cover_hbox), cover_info, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), cover_hbox, FALSE, FALSE, 0);

    file_label = gtk_label_new(TR("未选择音频"));
    qy_add_class(file_label, "qy-mon-info");
    gtk_widget_set_halign(file_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(cover_info), file_label, FALSE, FALSE, 0);

    status_label = gtk_label_new(TR("就绪"));
    qy_add_class(status_label, "qy-mon-info");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(cover_info), status_label, FALSE, FALSE, 0);

    /* 右侧歌词面板（只读，#qy-media-lyric） */
    lyrics = g_ptr_array_new();
    lyric_offsets = g_ptr_array_new();
    GtkWidget *lyric_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lyric_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(lyric_sw, -1, 150);
    lyric_view = gtk_text_view_new();
    gtk_widget_set_name(lyric_view, "qy-media-lyric");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(lyric_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(lyric_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(lyric_view), FALSE);
    lyric_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(lyric_view));
    gtk_text_buffer_set_text(lyric_buf, TR("无歌词文件"), -1);
    gtk_container_add(GTK_CONTAINER(lyric_sw), lyric_view);
    gtk_box_pack_start(GTK_BOX(right_vbox), lyric_sw, TRUE, TRUE, 0);

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
    /* 播放速度下拉（release 267） */
    speed_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(speed_combo), "0.5", "0.5x");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(speed_combo), "1.0", "1.0x");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(speed_combo), "1.5", "1.5x");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(speed_combo), "2.0", "2.0x");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(speed_combo), "1.0");
    /* 恢复上次播放速度（release 275） */
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        gchar *cp = g_build_filename(cdir, "qymedia-speed.conf", NULL);
        gchar *s = NULL;
        if (g_file_get_contents(cp, &s, NULL, NULL)) {
            double sp = g_ascii_strtod(s ? s : "1.0", NULL);
            if (sp > 0.0 && sp <= 4.0) {
                const char *ids[] = { "0.5", "1.0", "1.5", "2.0" };
                const char *best = "1.0";
                double bd = 10.0;
                for (guint i = 0; i < 4; i++) {
                    double d = g_ascii_strtod(ids[i], NULL);
                    if (fabs(d - sp) < bd) { bd = fabs(d - sp); best = ids[i]; }
                }
                g_play_speed = g_ascii_strtod(best, NULL);
                gtk_combo_box_set_active_id(GTK_COMBO_BOX(speed_combo), best);
            }
        }
        g_free(s); g_free(cp); g_free(cdir);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(speed_combo), 1);
    g_signal_connect(speed_combo, "changed", G_CALLBACK(on_speed_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), speed_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), bar, FALSE, FALSE, 0);

    /* 循环 + 均衡器 + 音量 */
    GtkWidget *bar2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *eq_btn = gtk_button_new_with_label(TR("均衡器"));
    qy_add_class(eq_btn, "qy-btn");
    g_signal_connect(eq_btn, "clicked", G_CALLBACK(on_eq_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), eq_btn, FALSE, FALSE, 0);
    GtkWidget *loop_btn = gtk_toggle_button_new_with_label(TR("循环"));
    g_signal_connect(loop_btn, "toggled", G_CALLBACK(on_loop_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), loop_btn, FALSE, FALSE, 0);
    GtkWidget *vol_lb = gtk_label_new(TR("音量:"));
    gtk_box_pack_start(GTK_BOX(bar2), vol_lb, FALSE, FALSE, 0);
    GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(vol_scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(vol_scale), GTK_POS_LEFT);
    gtk_range_set_value(GTK_RANGE(vol_scale), 80);
    /* 恢复上次音量（release 275） */
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        gchar *cp = g_build_filename(cdir, "qymedia-vol.conf", NULL);
        gchar *s = NULL;
        if (g_file_get_contents(cp, &s, NULL, NULL)) {
            int v = atoi(s ? s : "80");
            if (v >= 0 && v <= 100) gtk_range_set_value(GTK_RANGE(vol_scale), v);
        }
        g_free(s); g_free(cp); g_free(cdir);
    }
    gtk_widget_set_hexpand(vol_scale, TRUE);
    g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_volume_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), vol_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), bar2, FALSE, FALSE, 0);

    /* 启动时加载上次保存的播放列表 */
    playlist_load();

    gtk_widget_show_all(win);
    cover_update(NULL);

    /* 歌词高亮定时器（不阻塞播放） */
    g_timeout_add(300, lyric_tick, NULL);

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