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
#include <sys/stat.h>
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
static GtkWidget *search_entry = NULL;        /* 播放列表搜索框（release 301） */
static gchar *status_before_search = NULL;    /* 搜索前状态文本，清空关键字时恢复 */

/* ---------- 封面显示（同目录 cover.jpg/png/webp，128x128） ---------- */
static GtkWidget *cover_stack = NULL;   /* img/empty 二选一 */
static GtkWidget *cover_fmt_badge = NULL; /* 封面格式徽标（release 280） */
static gboolean shuffle_enabled = FALSE;   /* 随机播放（release 280） */
static GtkWidget *eq_preset_btns[4] = { NULL, NULL, NULL, NULL }; /* 预设按钮（高亮用） */
static GtkWidget *cover_img = NULL;     /* 有封面时显示 GdkPixbuf */
static GtkWidget *cover_empty = NULL;   /* 无封面时 qy_draw_empty_state(kind="note") */

/* ---------- 封面库网格视图（release 303） ---------- */
static GtkWidget *pl_stack = NULL;        /* 播放列表/封面库切换容器 */
static GtkWidget *cover_flow = NULL;      /* 封面库 GtkFlowBox 网格 */
static gboolean cover_lib_active = FALSE; /* 当前是否处于封面库模式 */

/* ---------- 均衡器（占位：6 段滑杆，写 /etc/qymedia-eq.conf，无真实音频后端） ---------- */
static GtkWidget *eq_scales[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
static void eq_save(void);   /* 前向声明（on_eq_preset 提前引用） */
static const char *eq_band_names[6] = { "63Hz", "250Hz", "1kHz", "4kHz", "8kHz", "16kHz" };
static double g_play_speed = 1.0;   /* 播放速度（release 267） */
static GtkWidget *speed_combo = NULL;
static int lyric_font_size = 15;    /* 歌词字号（release 277） */
static GtkCssProvider *lyric_css = NULL;

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
    /* 状态提示（release 278） */
    {
        gchar *st = g_strdup_printf(TR("播放速度: %.2fx"), g_play_speed);
        gtk_label_set_text(GTK_LABEL(status_label), st);
        g_free(st);
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
static GHashTable *play_counts = NULL; /* 播放计数: path -> count（release 283） */

static void on_next_clicked(GtkWidget *w, gpointer ud);   /* 前向声明 */
static void on_play(GtkWidget *w, gpointer ud);           /* 前向声明（媒体快捷键用） */
static void on_pause(GtkWidget *w, gpointer ud);          /* 前向声明（媒体快捷键用） */
static void on_open(GtkWidget *w, gpointer ud);           /* 前向声明（媒体快捷键用） */
static void lyric_font_apply(void);                       /* 前向声明（歌词字号快捷键用） */
static void on_clear_playlist(GtkWidget *w, gpointer ud); /* 前向声明（右键菜单用） */
static void on_remove_selected(GtkMenuItem *mi, gpointer ud); /* 前向声明（多选批量移除） */
static void on_track_reset_count(GtkMenuItem *mi, gpointer ud); /* 前向声明（重置播放计数，release 287） */
static void on_clear_all_counts(GtkMenuItem *mi, gpointer ud);   /* 前向声明（清空全部计数，release 291） */
static void on_clean_invalid(GtkWidget *w, gpointer ud); /* 前向声明（清理失效项，release 298） */
static void playlist_save(void);                          /* 前向声明（add_to_playlist 自动保存用） */
static void add_to_playlist(const char *path);            /* 前向声明（on_track_remove 提前引用） */
static void playlist_row_add(const char *path, guint seq);                        /* 前向声明（拖拽重建用） */
static void count_increment(const char *path);              /* 前向声明（play_file 播放计数用） */
static void on_playlist_drag_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *data,
                                 guint info, guint t, gpointer ud);               /* 前向声明（拖拽排序） */
static void on_playlist_drag_recv(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                                  GtkSelectionData *data, guint info, guint t, gpointer ud); /* 前向声明（拖拽排序） */
static void playlist_filter_apply(void);  /* 前向声明（搜索过滤，release 301） */
static void playlist_filter_rows(void);    /* 前向声明（搜索过滤，release 301） */
static void cover_grid_rebuild(void);    /* 前向声明（封面库刷新，release 303） */

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
    /* 停止后窗口标题恢复默认（release 276） */
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫音乐"));
    gtk_label_set_text(GTK_LABEL(status_label), TR("已停止"));
    lyric_clear();
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
        gchar *title = g_strdup_printf(TR("▶ 澜岫音乐 — %s"), base);
        gtk_window_set_title(GTK_WINDOW(win), title);
        g_free(title); g_free(base);
    }
    cover_update(path);
    /* 滚动当前曲目到可见（release 278） */
    {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
        for (GList *l = ch; l; l = l->next) {
            const char *p = g_object_get_data(G_OBJECT(l->data), "track-path");
            if (p && strcmp(p, path) == 0) {
                gtk_widget_grab_focus(GTK_WIDGET(l->data));
                break;
            }
        }
        g_list_free(ch);
    }
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
        count_increment(path);   /* 播放计数 +1（release 283） */
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
    /* 随机播放（release 280） */
    if (shuffle_enabled && track_list->len > 1) {
        int r;
        do {
            r = g_random_int_range(0, (gint)track_list->len);
        } while (r == current_index);
        play_index(r);
        return;
    }
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
/* 歌词字号应用（release 277）：动态 CSS 覆盖 #qy-media-lyric */
static void lyric_font_apply(void) {
    if (!lyric_view) return;
    if (!lyric_css) lyric_css = gtk_css_provider_new();
    gchar *css = g_strdup_printf("#qy-media-lyric { font-size: %dpx; }", lyric_font_size);
    gtk_css_provider_load_from_data(lyric_css, css, -1, NULL);
    GdkScreen *scr = gdk_screen_get_default();
    if (scr)
        gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(lyric_css), GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
    g_free(css);
}

static void on_lyric_font_changed(GtkComboBox *combo, gpointer ud) {
    (void)ud;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    lyric_font_size = id ? atoi(id) : 15;
    if (lyric_font_size < 10) lyric_font_size = 10;
    if (lyric_font_size > 30) lyric_font_size = 30;
    lyric_font_apply();
    gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(cdir, 0755);
    gchar *cp = g_build_filename(cdir, "qymedia-lyric-font.conf", NULL);
    gchar *s = g_strdup_printf("%d", lyric_font_size);
    g_file_set_contents(cp, s, -1, NULL);
    g_free(s); g_free(cp); g_free(cdir);
}

/* 歌词视图 Ctrl+滚轮调节字号（release 278） */
static gboolean on_lyric_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer ud) {
    (void)w; (void)ud;
    if (!(ev->state & GDK_CONTROL_MASK)) return FALSE;
    if (ev->direction == GDK_SCROLL_UP) {
        if (lyric_font_size < 30) { lyric_font_size++; lyric_font_apply(); }
        return TRUE;
    }
    if (ev->direction == GDK_SCROLL_DOWN) {
        if (lyric_font_size > 10) { lyric_font_size--; lyric_font_apply(); }
        return TRUE;
    }
    return FALSE;
}

/* 媒体快捷键：空格播放/暂停，←/→ 上一首/下一首（release 278） */
static gboolean on_win_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w; (void)ud;
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_o || ev->keyval == GDK_KEY_O)) {
        on_open(NULL, NULL);   /* Ctrl+O 添加音频文件（release 278） */
        return TRUE;
    }
    /* Ctrl+= / Ctrl+- 调节歌词字号（release 278） */
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_equal || ev->keyval == GDK_KEY_plus)) {
        if (lyric_font_size < 30) { lyric_font_size++; lyric_font_apply(); }
        return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_minus)) {
        if (lyric_font_size > 10) { lyric_font_size--; lyric_font_apply(); }
        return TRUE;
    }
    switch (ev->keyval) {
        case GDK_KEY_space: case GDK_KEY_KP_Space:
            if (player_pid > 0)
                on_pause(GTK_WIDGET(btn_pause), NULL);
            else
                on_play(GTK_WIDGET(btn_play), NULL);
            return TRUE;
        case GDK_KEY_Left:
            on_prev_clicked(NULL, NULL);
            return TRUE;
        case GDK_KEY_Right:
            on_next_clicked(NULL, NULL);
            return TRUE;
    }
    return FALSE;
}

/* 播放列表右键「在文件管理器中显示」（release 278） */
static void on_track_open_dir(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    if (!ud) return;
    const char *path = (const char *)ud;
    gchar *dir = g_path_get_dirname(path);
    gchar *cmd = g_strdup_printf("qyfiles \"%s\"", dir);
    GError *err = NULL;
    if (!g_spawn_command_line_async(cmd, &err))
        g_printerr("QYMEDIA: 打开目录失败: %s\n", err ? err->message : "?");
    g_clear_error(&err);
    g_free(cmd); g_free(dir); g_free((gpointer)ud);
}

/* 播放列表右键「复制路径」（release 278） */
static void on_track_copy_path(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    if (ud) {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, (const char *)ud, -1);
        g_free(ud);
    }
}

/* 播放列表右键「播放」（release 276） */
static void on_track_play(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    if (ud) {
        play_file((const char *)ud);
        g_free(ud);
    }
}

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
    cover_grid_rebuild();      /* 封面库网格同步（release 303） */
    g_free(ud);
}

static gboolean on_track_row_press(GtkWidget *row, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->button != 3) return FALSE;
    const char *path = g_object_get_data(G_OBJECT(row), "track-path");
    if (!path) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_play = gtk_menu_item_new_with_label(TR("播放"));
    g_signal_connect(mi_play, "activate", G_CALLBACK(on_track_play), (gpointer)g_strdup(path));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_play);
    GtkWidget *mi_copy = gtk_menu_item_new_with_label(TR("复制路径"));
    g_signal_connect(mi_copy, "activate", G_CALLBACK(on_track_copy_path), (gpointer)g_strdup(path));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_copy);
    GtkWidget *mi_dir = gtk_menu_item_new_with_label(TR("在文件管理器中显示"));
    g_signal_connect(mi_dir, "activate", G_CALLBACK(on_track_open_dir), (gpointer)g_strdup(path));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_dir);
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("从播放列表移除"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_track_remove), (gpointer)g_strdup(path));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    GtkWidget *mi_rm = gtk_menu_item_new_with_label(TR("移除选中曲目"));
    g_signal_connect(mi_rm, "activate", G_CALLBACK(on_remove_selected), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_rm);
    GtkWidget *mi_cnt = gtk_menu_item_new_with_label(TR("重置播放计数"));
    g_signal_connect(mi_cnt, "activate", G_CALLBACK(on_track_reset_count), (gpointer)g_strdup(path));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_cnt);
    GtkWidget *mi_all = gtk_menu_item_new_with_label(TR("清空全部播放计数"));
    g_signal_connect(mi_all, "activate", G_CALLBACK(on_clear_all_counts), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_all);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *mi_clr = gtk_menu_item_new_with_label(TR("清空播放列表"));
    g_signal_connect(mi_clr, "activate", G_CALLBACK(on_clear_playlist), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_clr);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), row, NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* 移除选中曲目（release 281 多选批量） */
static void on_remove_selected(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    GList *sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(playlist));
    if (!sel) return;
    for (GList *l = sel; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "track-path");
        if (!p) continue;
        for (guint i = 0; i < track_list->len; i++) {
            if (strcmp((const char *)g_ptr_array_index(track_list, i), p) == 0) {
                g_ptr_array_remove_index(track_list, i);
                break;
            }
        }
    }
    g_list_free(sel);
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    for (guint i = 0; i < track_list->len; i++)
        playlist_row_add((const char *)g_ptr_array_index(track_list, i), i + 1);
    if (current_index >= 0) highlight_current_row();
    playlist_save();
    playlist_filter_apply();   /* 保持搜索过滤状态（release 301） */
    cover_grid_rebuild();      /* 封面库网格同步（release 303） */
}

/* 拖拽源: 输出曲目路径（release 280 拖拽排序） */
static void on_playlist_drag_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *data,
                                 guint info, guint t, gpointer ud) {
    (void)ctx; (void)info; (void)t; (void)ud;
    const char *path = g_object_get_data(G_OBJECT(w), "track-path");
    if (path) gtk_selection_data_set_text(data, path, -1);
}

/* 拖拽目标: 把源曲目移动到当前行位置并重建列表（release 280） */
static void on_playlist_drag_recv(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                                  GtkSelectionData *data, guint info, guint t, gpointer ud) {
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)t; (void)ud;
    gchar *src = (gchar *)gtk_selection_data_get_text(data);
    if (!src) return;
    const char *dst = g_object_get_data(G_OBJECT(w), "track-path");
    if (dst && strcmp(src, dst) != 0) {
        int si = -1, di = -1;
        for (guint i = 0; i < track_list->len; i++) {
            const char *p = (const char *)g_ptr_array_index(track_list, i);
            if (si < 0 && strcmp(p, src) == 0) si = (int)i;
            if (di < 0 && strcmp(p, dst) == 0) di = (int)i;
        }
        if (si >= 0 && di >= 0) {
            gchar *sp = (gchar *)g_ptr_array_steal_index(track_list, si);
            if (di > si) di--;
            g_ptr_array_insert(track_list, di, sp);
            /* 重建列表 UI（track_list 已更新，只重画行） */
            GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
            for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
            g_list_free(ch);
            for (guint i = 0; i < track_list->len; i++)
                playlist_row_add((const char *)g_ptr_array_index(track_list, i), i + 1);
            if (current_index >= 0) highlight_current_row();   /* 拖拽后保持高亮 */
            playlist_save();
            playlist_filter_apply();   /* 保持搜索过滤状态（release 301） */
            cover_grid_rebuild();      /* 封面库网格同步（release 303） */
        }
    }
    g_free(src);
}

/* ---------- 播放列表搜索过滤（release 301） ---------- */
/* 路径或文件名是否包含关键字（大小写不敏感） */
static gboolean playlist_row_matches(const char *path, const char *kw) {
    if (!kw || !kw[0]) return TRUE;
    if (!path) return FALSE;
    gchar *path_l = g_utf8_strdown(path, -1);
    gchar *kw_l = g_utf8_strdown(kw, -1);
    gboolean hit = FALSE;
    if (path_l && kw_l && strstr(path_l, kw_l))
        hit = TRUE;
    if (!hit) {
        gchar *base = g_path_get_basename(path);
        gchar *base_l = base ? g_utf8_strdown(base, -1) : NULL;
        if (base_l && kw_l && strstr(base_l, kw_l))
            hit = TRUE;
        g_free(base_l);
        g_free(base);
    }
    g_free(path_l);
    g_free(kw_l);
    return hit;
}

/* 按当前搜索关键字重新应用行可见性（不修改状态栏） */
static void playlist_filter_rows(void) {
    if (!playlist || !search_entry) return;
    const char *kw = gtk_entry_get_text(GTK_ENTRY(search_entry));
    if (!kw || !kw[0]) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next) {
        const char *path = g_object_get_data(G_OBJECT(l->data), "track-path");
        gboolean vis = path && playlist_row_matches(path, kw);
        gtk_widget_set_visible(GTK_WIDGET(l->data), vis);
    }
    g_list_free(ch);
}

/* 应用搜索过滤并更新匹配计数提示（匹配 N/M 首） */
static void playlist_filter_apply(void) {
    if (!playlist || !search_entry) return;
    const char *kw = gtk_entry_get_text(GTK_ENTRY(search_entry));
    gboolean has_kw = kw && kw[0] != '\0';
    if (!has_kw && !status_before_search) return;

    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    guint total = g_list_length(ch);
    guint matched = 0;
    for (GList *l = ch; l; l = l->next) {
        gboolean vis = TRUE;
        if (has_kw) {
            const char *path = g_object_get_data(G_OBJECT(l->data), "track-path");
            vis = path && playlist_row_matches(path, kw);
        }
        gtk_widget_set_visible(GTK_WIDGET(l->data), vis);
        if (vis) matched++;
    }
    g_list_free(ch);

    if (!status_label) return;
    if (has_kw) {
        if (!status_before_search) {
            const gchar *cur = gtk_label_get_text(GTK_LABEL(status_label));
            status_before_search = g_strdup(cur ? cur : "");
        }
        gchar *st = g_strdup_printf(TR("匹配 %u/%u 首"), matched, total);
        gtk_label_set_text(GTK_LABEL(status_label), st);
        g_free(st);
    } else if (status_before_search) {
        gtk_label_set_text(GTK_LABEL(status_label), status_before_search);
        g_free(status_before_search);
        status_before_search = NULL;
    }
}

/* 搜索框文本变化回调 */
static void on_search_changed(GtkSearchEntry *entry, gpointer ud) {
    (void)entry; (void)ud;
    playlist_filter_apply();
}

/* 纯 UI: 添加一行播放列表（不修改 track_list，重建用） */
static void playlist_row_add(const char *path, guint seq) {
    if (!path || !path[0]) return;
    gchar *base = g_path_get_basename(path);
    GtkWidget *row = gtk_list_box_row_new();
    guint count = play_counts ? GPOINTER_TO_UINT(g_hash_table_lookup(play_counts, path)) : 0;
    gchar *numbered = count > 0
        ? g_strdup_printf("%u. %s  · %s%u %s", seq, base, TR("播放"), count, TR("次"))
        : g_strdup_printf("%u. %s", seq, base);
    GtkWidget *lbl = gtk_label_new(numbered);
    g_free(numbered);
    g_free(base);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(row), lbl);
    gtk_widget_set_tooltip_text(row, path);   /* 悬停显示完整路径 */
    /* tooltip 附文件大小 + 播放次数（release 278/284） */
    {
        GStatBuf sb;
        guint cnt = play_counts ? GPOINTER_TO_UINT(g_hash_table_lookup(play_counts, path)) : 0;
        if (g_stat(path, &sb) == 0 && S_ISREG(sb.st_mode)) {
            gchar *sz = g_format_size(sb.st_size);
            gchar *tip = cnt > 0
                ? g_strdup_printf("%s\n%s\n%s%u %s", path, sz, TR("播放"), cnt, TR("次"))
                : g_strdup_printf("%s\n%s", path, sz);
            gtk_widget_set_tooltip_text(row, tip);
            g_free(tip); g_free(sz);
        } else if (cnt > 0) {
            gchar *tip = g_strdup_printf("%s\n%s%u %s", path, TR("播放"), cnt, TR("次"));
            gtk_widget_set_tooltip_text(row, tip);
            g_free(tip);
        }
    }
    g_object_set_data_full(G_OBJECT(row), "track-path", g_strdup(path), g_free);
    g_object_set_data(G_OBJECT(row), "track-lbl", lbl);       /* 播放计数刷新用（release 283） */
    g_object_set_data(G_OBJECT(row), "track-seq", GUINT_TO_POINTER(seq));
    gtk_list_box_insert(GTK_LIST_BOX(playlist), row, -1);
    g_signal_connect(row, "button-press-event", G_CALLBACK(on_track_row_press), NULL);
    /* 拖拽排序（release 280） */
    GtkTargetEntry te = { (gchar *)"text/plain", 0, 0 };
    gtk_drag_source_set(row, GDK_BUTTON1_MASK, &te, 1, GDK_ACTION_MOVE);
    gtk_drag_dest_set(row, GTK_DEST_DEFAULT_ALL, &te, 1, GDK_ACTION_MOVE);
    g_signal_connect(row, "drag-data-get", G_CALLBACK(on_playlist_drag_get), NULL);
    g_signal_connect(row, "drag-data-received", G_CALLBACK(on_playlist_drag_recv), NULL);
    /* 搜索过滤（release 301）：新行按当前关键字决定初始可见性 */
    if (search_entry) {
        const char *kw = gtk_entry_get_text(GTK_ENTRY(search_entry));
        if (kw && kw[0]) {
            gboolean vis = path && playlist_row_matches(path, kw);
            gtk_widget_set_visible(row, vis);
        }
    }
}

/* 添加曲目到播放列表（去重 + 持久化） */
static void add_to_playlist(const char *path) {
    if (!path || !path[0]) return;
    for (guint i = 0; i < track_list->len; i++) {
        if (strcmp((const char *)g_ptr_array_index(track_list, i), path) == 0)
            return;
    }
    g_ptr_array_add(track_list, g_strdup(path));
    playlist_row_add(path, track_list->len);
    playlist_save();   /* 添加后自动保存 */
    playlist_filter_apply();   /* 保持搜索过滤状态（release 301） */
    cover_grid_rebuild();      /* 封面库网格同步（release 303） */
}

/* 窗口拖放: 拖入音频文件直接加入播放列表并播放（release 280） */
static void on_win_drag_recv(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                             GtkSelectionData *data, guint info, guint t, gpointer ud) {
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)t; (void)ud;
    gchar **uris = gtk_selection_data_get_uris(data);
    if (!uris) return;
    for (int i = 0; uris[i]; i++) {
        gchar *path = g_filename_from_uri(uris[i], NULL, NULL);
        if (path) {
            add_to_playlist(path);
            if (i == 0) play_index((int)track_list->len - 1);
            g_free(path);
        }
    }
    g_strfreev(uris);
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

/* ---------- 播放计数（~/.config/lanxiu/qymedia-count.conf，release 283） ---------- */
static gchar *count_path(void) {
    return g_build_filename(g_get_home_dir(), ".config/lanxiu/qymedia-count.conf", NULL);
}

static void counts_save(void) {
    if (!play_counts) return;
    gchar *path = count_path();
    GKeyFile *kf = g_key_file_new();
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, play_counts);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        gint c = GPOINTER_TO_INT(val);
        if (c > 0)
            g_key_file_set_integer(kf, "counts", (const char *)key, c);
    }
    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, NULL);
    if (data) {
        gchar *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        GError *err = NULL;
        g_file_set_contents(path, data, len, &err);
        if (err) g_clear_error(&err);
        g_free(data);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void counts_load(void) {
    play_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar *path = count_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        gchar **keys = g_key_file_get_keys(kf, "counts", &n, NULL);
        for (gsize i = 0; i < n; i++) {
            gint c = g_key_file_get_integer(kf, "counts", keys[i], NULL);
            if (c > 0)
                g_hash_table_replace(play_counts, g_strdup(keys[i]), GUINT_TO_POINTER(c));
        }
        g_strfreev(keys);
    }
    g_key_file_free(kf);
    g_free(path);
}

/* 更新播放列表行的文字（含播放次数） */
static void playlist_row_refresh(const char *path, guint seq, guint count) {
    if (!playlist) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "track-path");
        if (p && strcmp(p, path) == 0) {
            GtkWidget *lbl = g_object_get_data(G_OBJECT(l->data), "track-lbl");
            if (lbl) {
                gchar *base = g_path_get_basename(path);
                gchar *txt = count > 0
                    ? g_strdup_printf("%u. %s  · %s%u %s", seq, base, TR("播放"), count, TR("次"))
                    : g_strdup_printf("%u. %s", seq, base);
                gtk_label_set_text(GTK_LABEL(lbl), txt);
                g_free(txt);
                g_free(base);
            }
            break;
        }
    }
    g_list_free(ch);
}

/* 曲目开始播放时计数 +1 并持久化 */
static void count_increment(const char *path) {
    if (!path || !path[0] || !play_counts) return;
    guint c = GPOINTER_TO_UINT(g_hash_table_lookup(play_counts, path));
    g_hash_table_replace(play_counts, g_strdup(path), GUINT_TO_POINTER(c + 1));
    counts_save();
    /* 找到该曲目所在行并更新显示（行号按当前列表位置） */
    guint seq = 0;
    for (guint i = 0; i < track_list->len; i++) {
        if (strcmp((const char *)g_ptr_array_index(track_list, i), path) == 0) {
            seq = i + 1;
            break;
        }
    }
    if (seq) playlist_row_refresh(path, seq, c + 1);
}

/* 重置选中曲目的播放计数（release 287） */
static void on_track_reset_count(GtkMenuItem *mi, gpointer ud) {
    (void)mi;
    const char *path = (const char *)ud;
    if (play_counts && path)
        g_hash_table_remove(play_counts, path);
    counts_save();
    if (path) {
        for (guint i = 0; i < track_list->len; i++) {
            if (strcmp((const char *)g_ptr_array_index(track_list, i), path) == 0) {
                playlist_row_refresh(path, i + 1, 0);
                break;
            }
        }
    }
    g_free(ud);
}

/* 清空全部播放计数（release 291） */
static void on_clear_all_counts(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (play_counts) g_hash_table_remove_all(play_counts);
    counts_save();
    for (guint i = 0; i < track_list->len; i++)
        playlist_row_refresh((const char *)g_ptr_array_index(track_list, i), i + 1, 0);
    if (status_label)
        gtk_label_set_text(GTK_LABEL(status_label), TR("已清空全部播放计数"));
}

/* 播放计数排序比较器：按播放次数降序（release 284） */
static gint sort_count_cmp(gconstpointer a, gconstpointer b) {
    const char *pa = *(char *const *)a;
    const char *pb = *(char *const *)b;
    guint ca = play_counts ? GPOINTER_TO_UINT(g_hash_table_lookup(play_counts, pa)) : 0;
    guint cb = play_counts ? GPOINTER_TO_UINT(g_hash_table_lookup(play_counts, pb)) : 0;
    if (cb != ca) return cb > ca ? 1 : -1;   /* 降序：次数多在前 */
    return strcmp(pa, pb);
}

/* 工具栏「按次数排序」：按播放次数降序重排播放列表 */
static void on_sort_by_count(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!track_list || track_list->len < 2 || !play_counts) return;
    g_ptr_array_sort(track_list, sort_count_cmp);
    /* 重建列表 UI（track_list 已排序） */
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    for (guint i = 0; i < track_list->len; i++)
        playlist_row_add((const char *)g_ptr_array_index(track_list, i), i + 1);
    if (current_index >= 0) highlight_current_row();
    playlist_save();
    gtk_label_set_text(GTK_LABEL(status_label), TR("已按播放次数排序"));
    playlist_filter_rows();   /* 保持搜索过滤状态（release 301） */
    cover_grid_rebuild();      /* 封面库网格同步（release 303） */
}

/* 工具栏「保存列表」 */
static void on_save_playlist(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    playlist_save();
    gtk_label_set_text(GTK_LABEL(status_label), TR("播放列表已保存"));
}

/* 导出为 M3U 播放列表文件（release 294） */
static void on_export_m3u(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!track_list || track_list->len == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("播放列表为空"));
        return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new(TR("导出 M3U 播放列表"), NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE, TR("导出"), GTK_RESPONSE_ACCEPT,
        TR("取消"), GTK_RESPONSE_CANCEL, NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "playlist.m3u");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            GString *s = g_string_new("#EXTM3U\n");
            for (guint i = 0; i < track_list->len; i++) {
                const char *p = g_ptr_array_index(track_list, i);
                gchar *base = g_path_get_basename(p);
                g_string_append_printf(s, "#EXTINF:0,%s\n%s\n", base, p);
                g_free(base);
            }
            g_file_set_contents(path, s->str, s->len, NULL);
            g_string_free(s, TRUE);
            gtk_label_set_text(GTK_LABEL(status_label), TR("已导出 M3U 播放列表"));
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_clear_playlist(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (track_list && track_list->len > 0) {
        gchar *msg = g_strdup_printf(TR("确认清空 %u 首曲目?"), track_list->len);
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(w)),
                                                GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_YES_NO, "%s", msg);
        g_free(msg);
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
    cover_grid_rebuild();      /* 封面库网格同步（release 303） */
}

/* 工具栏「清理失效项」：检查并移除播放列表中已不存在的文件（release 298） */
static void on_clean_invalid(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!track_list || track_list->len == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("没有失效的文件"));
        return;
    }
    /* 统计失效文件数量 */
    guint n = 0;
    for (guint i = 0; i < track_list->len; i++) {
        const char *p = (const char *)g_ptr_array_index(track_list, i);
        if (!g_file_test(p, G_FILE_TEST_EXISTS))
            n++;
    }
    if (n == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("没有失效的文件"));
        return;
    }
    /* 确认对话框（参照 on_clear_playlist） */
    gchar *msg = g_strdup_printf(TR("发现 %u 个失效文件，从播放列表移除？"), n);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(w)),
                                            GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                            GTK_BUTTONS_YES_NO, "%s", msg);
    g_free(msg);
    int ans = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (ans != GTK_RESPONSE_YES) return;

    /* 当前播放的曲目是否也被移除（其文件已不存在） */
    gboolean current_removed = (current_file && !g_file_test(current_file, G_FILE_TEST_EXISTS));

    /* 倒序遍历移除失效项（track_list 带 g_free free_func，remove_index 会释放字符串） */
    for (guint i = track_list->len; i > 0; i--) {
        const char *p = (const char *)g_ptr_array_index(track_list, i - 1);
        if (!g_file_test(p, G_FILE_TEST_EXISTS))
            g_ptr_array_remove_index(track_list, i - 1);
    }

    /* 重建播放列表 UI（track_list 已更新，只重画行） */
    GList *ch = gtk_container_get_children(GTK_CONTAINER(playlist));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    for (guint i = 0; i < track_list->len; i++)
        playlist_row_add((const char *)g_ptr_array_index(track_list, i), i + 1);

    /* 调整 current_index：当前曲目被移除则停止播放并置 -1；否则按路径重新定位 */
    if (current_removed) {
        stop_player();
        current_index = -1;
        g_free(current_file);
        current_file = NULL;
        gtk_label_set_text(GTK_LABEL(file_label), TR("未选择音频"));
        cover_update(NULL);
    } else if (current_file) {
        current_index = -1;
        for (guint i = 0; i < track_list->len; i++) {
            if (strcmp((const char *)g_ptr_array_index(track_list, i), current_file) == 0) {
                current_index = (int)i;
                break;
            }
        }
    } else {
        current_index = -1;
    }
    if (current_index >= 0) highlight_current_row();

    playlist_save();   /* 清理后持久化 */
    gchar *st = g_strdup_printf(TR("已移除 %u 个失效文件"), n);
    gtk_label_set_text(GTK_LABEL(status_label), st);
    g_free(st);
    playlist_filter_rows();   /* 保持搜索过滤状态（release 301） */
    cover_grid_rebuild();      /* 封面库网格同步（release 303） */
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
        if (current_file) {
            gchar *base = g_path_get_basename(current_file);
            gchar *t = g_strdup_printf(TR("⏸ 澜岫音乐 — %s"), base ? base : "");
            gtk_window_set_title(GTK_WINDOW(win), t);
            g_free(t); g_free(base);
        }
    } else {
        kill(player_pid, SIGCONT);
        paused = FALSE;
        lyric_seg_start = g_get_monotonic_time();
        set_pause_icon(QY_ICON_PAUSE);
        gtk_label_set_text(GTK_LABEL(status_label), TR("正在播放"));
        if (current_file) {
            gchar *base = g_path_get_basename(current_file);
            gchar *t = g_strdup_printf(TR("▶ 澜岫音乐 — %s"), base ? base : "");
            gtk_window_set_title(GTK_WINDOW(win), t);
            g_free(t); g_free(base);
        }
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
    gtk_widget_set_tooltip_text(GTK_WIDGET(btn), loop_enabled ? TR("循环播放：开") : TR("循环播放：关"));
}

/* 随机播放开关（release 280） */
static void on_shuffle_toggled(GtkToggleButton *btn, gpointer ud) {
    (void)ud;
    shuffle_enabled = gtk_toggle_button_get_active(btn);
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
    /* 记忆窗口大小（release 278） */
    if (win) {
        int cw = 0, ch = 0;
        gtk_window_get_size(GTK_WINDOW(win), &cw, &ch);
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        g_mkdir_with_parents(cdir, 0755);
        gchar *cp = g_build_filename(cdir, "qymedia-win.conf", NULL);
        gchar *sz = g_strdup_printf("%d %d", cw, ch);
        g_file_set_contents(cp, sz, -1, NULL);
        g_free(sz); g_free(cp); g_free(cdir);
    }
    stop_player();
    return FALSE;
}

/* ---------- 封面显示 ---------- */

/* 无封面时的空状态插图（qy_draw_empty_state kind="note"） */
/* 点击封面放大预览（release 278） */
static gboolean on_cover_clicked(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ev; (void)ud;
    if (gtk_stack_get_visible_child(GTK_STACK(cover_stack)) != cover_img) return FALSE;
    GdkPixbuf *pb = gtk_image_get_pixbuf(GTK_IMAGE(cover_img));
    if (!pb) return FALSE;
    int w0 = gdk_pixbuf_get_width(pb), h0 = gdk_pixbuf_get_height(pb);
    GdkPixbuf *big = gdk_pixbuf_scale_simple(pb, w0 * 3, h0 * 3, GDK_INTERP_BILINEAR);
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), TR("封面预览"));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(win));
    gtk_window_set_default_size(GTK_WINDOW(dlg), w0 * 3 + 20, h0 * 3 + 20);
    GtkWidget *img = gtk_image_new_from_pixbuf(big);
    g_object_unref(big);
    gtk_container_add(GTK_CONTAINER(dlg), img);
    gtk_widget_show_all(dlg);
    return TRUE;
}

static gboolean cover_empty_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    qy_draw_empty_state(cr, a.width, a.height, "note");
    return FALSE;
}

/* 查找曲目同目录 cover.jpg/png/webp 并加载为指定尺寸的 pixbuf（找不到返回 NULL）。
 * release 303 起从 cover_update 中抽出，供封面库网格复用。 */
static GdkPixbuf *find_cover_for(const char *audio_path, int size) {
    if (!audio_path || !audio_path[0]) return NULL;
    gchar *dir = g_path_get_dirname(audio_path);
    static const char *names[] = { "cover.jpg", "cover.png", "cover.webp", NULL };
    GdkPixbuf *pb = NULL;
    for (int i = 0; names[i] && !pb; i++) {
        gchar *full = g_build_filename(dir, names[i], NULL);
        pb = gdk_pixbuf_new_from_file_at_scale(full, size, size, TRUE, NULL);
        g_free(full);
    }
    g_free(dir);
    return pb;
}

/* 根据正在播放的音频路径查找同目录 cover.jpg/png/webp 并显示；
 * 找不到封面时回退到空状态插图。 */
static void cover_update(const char *audio_path) {
    if (!cover_stack) return;
    GdkPixbuf *pb = find_cover_for(audio_path, 128);
    if (cover_fmt_badge) {
        const char *dot = audio_path ? strrchr(audio_path, '.') : NULL;
        gchar *up = dot ? g_ascii_strup(dot + 1, -1) : NULL;
        gtk_label_set_text(GTK_LABEL(cover_fmt_badge), up ? up : "");
        g_free(up);
    }
    if (pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(cover_img), pb);
        g_object_unref(pb);
        gtk_stack_set_visible_child_name(GTK_STACK(cover_stack), "img");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(cover_stack), "empty");
    }
}

/* ---------- 封面库网格视图（release 303） ---------- */

/* 单个网格项：64x64 封面 + 下方文件名（最多 2 行省略）。 */
static GtkWidget *cover_grid_item_new(const char *path) {
    const int cover_sz = 64;
    GdkPixbuf *pb = find_cover_for(path, cover_sz);
    if (!pb) pb = qy_icon_pixbuf(QY_ICON_MUSIC, cover_sz, NULL);   /* 无封面占位图标 */

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    g_object_unref(pb);
    gtk_widget_set_size_request(img, cover_sz, cover_sz);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);

    gchar *base = g_path_get_basename(path);
    GtkWidget *lbl = gtk_label_new(base);
    g_free(base);
    gtk_label_set_lines(GTK_LABEL(lbl), 2);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 10);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.5);
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

    GtkWidget *child = gtk_flow_box_child_new();
    gtk_container_add(GTK_CONTAINER(child), box);
    g_object_set_data_full(G_OBJECT(child), "track-path", g_strdup(path), g_free);
    gtk_widget_set_tooltip_text(child, path);
    return child;
}

/* 按当前 track_list 重建封面库网格（仅封面库模式生效）。 */
static void cover_grid_rebuild(void) {
    if (!cover_flow || !cover_lib_active) return;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(cover_flow));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    if (!track_list) return;
    for (guint i = 0; i < track_list->len; i++) {
        const char *path = (const char *)g_ptr_array_index(track_list, i);
        gtk_container_add(GTK_CONTAINER(cover_flow), cover_grid_item_new(path));
    }
    gtk_widget_show_all(cover_flow);
}

/* 点击封面库网格项 → 找到路径在 track_list 的索引并播放（复用 play_index）。 */
static void on_cover_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer ud) {
    (void)box; (void)ud;
    if (!child) return;
    const char *path = g_object_get_data(G_OBJECT(child), "track-path");
    if (!path) return;
    for (guint i = 0; i < track_list->len; i++) {
        if (strcmp((const char *)g_ptr_array_index(track_list, i), path) == 0) {
            play_index((int)i);
            return;
        }
    }
}

/* 工具栏「封面库」切换：开启显示网格，关闭恢复播放列表。 */
static void on_cover_lib_toggled(GtkToggleButton *btn, gpointer ud) {
    (void)ud;
    if (!pl_stack) return;
    cover_lib_active = gtk_toggle_button_get_active(btn);
    if (cover_lib_active) {
        cover_grid_rebuild();
        gtk_stack_set_visible_child_name(GTK_STACK(pl_stack), "covers");
        /* 搜索过滤激活时不覆盖「匹配 N/M 首」状态（release 301） */
        const char *kw = search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : "";
        if (status_label && (!kw || !kw[0]))
            gtk_label_set_text(GTK_LABEL(status_label), TR("封面库视图"));
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(pl_stack), "list");
        if (search_entry) {
            const char *kw = gtk_entry_get_text(GTK_ENTRY(search_entry));
            if (kw && kw[0]) playlist_filter_apply();   /* 恢复匹配计数状态（release 301） */
        }
    }
}

/* ---------- 均衡器（占位实现） ---------- */

/* 预设曲线（6 段：63Hz/250Hz/1kHz/4kHz/8kHz/16kHz）release 270 */
static const double eq_flat[6] = { 0, 0, 0, 0, 0, 0 };
static const double eq_pop[6]  = { 3, 1, 0, 1, 2, 3 };
static const double eq_rock[6] = { 4, 2, -1, 1, 3, 4 };
static const double eq_aco[6]  = { 2, 1, 0, 1, 2, 2 };

/* 均衡器滑杆双击复位 0 dB（release 281） */
static gboolean on_eq_scale_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        gtk_range_set_value(GTK_RANGE(w), 0.0);
        return TRUE;
    }
    return FALSE;
}

static void on_eq_preset(GtkButton *b, gpointer ud) {
    (void)b;
    const double *vals = (const double *)ud;
    for (int i = 0; i < 6; i++)
        if (eq_scales[i]) gtk_range_set_value(GTK_RANGE(eq_scales[i]), vals[i]);
    eq_save();
    gtk_label_set_text(GTK_LABEL(status_label), TR("均衡器预设已应用"));
    /* 高亮当前预设按钮（release 281） */
    for (int i = 0; i < 4; i++)
        if (eq_preset_btns[i])
            gtk_widget_unset_state_flags(eq_preset_btns[i], GTK_STATE_FLAG_SELECTED);
    const void *p = (const void *)ud;
    int pidx = (p == eq_flat) ? 0 : (p == eq_pop) ? 1 : (p == eq_rock) ? 2 : (p == eq_aco) ? 3 : -1;
    if (pidx >= 0 && eq_preset_btns[pidx])
        gtk_widget_set_state_flags(eq_preset_btns[pidx], GTK_STATE_FLAG_SELECTED, FALSE);
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

/* 滑杆值格式: +3 dB（release 282）——sysroot 头文件无 set_format_value_func，改用 draw_value 数字 + tooltip 单位 */
static void on_eq_changed(GtkRange *r, gpointer ud) {
    (void)ud;
    eq_save();
    gchar *tip = g_strdup_printf(TR("当前 %.0f dB"), gtk_range_get_value(r));
    gtk_widget_set_tooltip_text(GTK_WIDGET(r), tip);
    g_free(tip);
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
            gchar *tip = g_strdup_printf(TR("均衡器 %s（±12 dB，双击复位 0 dB）"), eq_band_names[i]);
            gtk_widget_set_tooltip_text(scale, tip);
            g_free(tip);
            g_signal_connect(scale, "value-changed", G_CALLBACK(on_eq_changed), NULL);
            g_signal_connect(scale, "button-press-event", G_CALLBACK(on_eq_scale_press), NULL);
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
        gtk_widget_set_tooltip_text(b_flat, TR("全部频段归零"));
        gtk_widget_set_tooltip_text(b_pop,  TR("提升低频与高频，适合流行乐"));
        gtk_widget_set_tooltip_text(b_rock, TR("增强低音与高频临场感"));
        gtk_widget_set_tooltip_text(b_aco,  TR("中频突出，适合原声/人声"));
        qy_add_class(b_flat, "qy-btn"); qy_add_class(b_pop, "qy-btn");
        qy_add_class(b_rock, "qy-btn"); qy_add_class(b_aco, "qy-btn");
        g_signal_connect(b_flat, "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_flat);
        g_signal_connect(b_pop,  "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_pop);
        g_signal_connect(b_rock, "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_rock);
        g_signal_connect(b_aco,  "clicked", G_CALLBACK(on_eq_preset), (gpointer)eq_aco);
        eq_preset_btns[0] = b_flat; eq_preset_btns[1] = b_pop;
        eq_preset_btns[2] = b_rock; eq_preset_btns[3] = b_aco;
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
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_win_key_press), NULL);   /* 空格/方向键（release 278） */
    /* 拖放音频文件到窗口播放（release 280） */
    {
        GtkTargetEntry dte = { (gchar *)"text/uri-list", 0, 0 };
        gtk_drag_dest_set(win, GTK_DEST_DEFAULT_ALL, &dte, 1, GDK_ACTION_COPY);
        g_signal_connect(win, "drag-data-received", G_CALLBACK(on_win_drag_recv), NULL);
    }
    /* 恢复上次窗口大小（release 278） */
    {
        gchar *cp = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qymedia-win.conf", NULL);
        gchar *sz = NULL;
        if (g_file_get_contents(cp, &sz, NULL, NULL)) {
            gchar **parts = g_strsplit(sz ? sz : "", " ", 2);
            if (parts[0] && parts[1]) {
                int rw = atoi(parts[0]), rh = atoi(parts[1]);
                if (rw > 400 && rh > 240)
                    gtk_window_resize(GTK_WINDOW(win), rw, rh);
            }
            g_strfreev(parts);
        }
        g_free(sz); g_free(cp);
    }

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
    GtkWidget *b_sort = gtk_button_new_with_label(TR("次数排序"));
    qy_add_class(b_sort, "qy-btn");
    GtkWidget *b_cover = gtk_toggle_button_new_with_label(TR("封面库"));   /* release 303 */
    qy_add_class(b_cover, "qy-btn");
    gtk_widget_set_tooltip_text(b_cover, TR("封面库网格视图（开/关）"));
    GtkWidget *b_m3u = gtk_button_new_with_label(TR("导出 M3U"));
    qy_add_class(b_m3u, "qy-btn");
    GtkWidget *b_clean = gtk_button_new_with_label(TR("清理失效项"));
    qy_add_class(b_clean, "qy-btn");
    g_signal_connect(b_add, "clicked", G_CALLBACK(on_add_files), NULL);
    g_signal_connect(b_save, "clicked", G_CALLBACK(on_save_playlist), NULL);
    g_signal_connect(b_clear, "clicked", G_CALLBACK(on_clear_playlist), NULL);
    g_signal_connect(b_sort, "clicked", G_CALLBACK(on_sort_by_count), NULL);
    g_signal_connect(b_cover, "toggled", G_CALLBACK(on_cover_lib_toggled), NULL);
    g_signal_connect(b_m3u, "clicked", G_CALLBACK(on_export_m3u), NULL);
    g_signal_connect(b_clean, "clicked", G_CALLBACK(on_clean_invalid), NULL);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_add, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_sort, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_cover, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_m3u, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_clean, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pl_bar), b_clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_vbox), pl_bar, FALSE, FALSE, 0);

    /* 播放列表搜索框（release 301）：位于工具栏下方、列表上方 */
    search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索播放列表…"));
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(left_vbox), search_entry, FALSE, FALSE, 0);

    GtkWidget *pl_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pl_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    playlist = gtk_list_box_new();
    gtk_widget_set_name(GTK_WIDGET(playlist), "qy-media-list");
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(playlist), FALSE);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(playlist), GTK_SELECTION_MULTIPLE);   /* 多选（release 281） */
    g_signal_connect(playlist, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(pl_sw), playlist);

    /* 播放列表 / 封面库切换容器（release 303） */
    pl_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(pl_stack), pl_sw, "list");

    GtkWidget *cover_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cover_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    cover_flow = gtk_flow_box_new();
    gtk_widget_set_name(GTK_WIDGET(cover_flow), "qy-media-covers");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(cover_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(cover_flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(cover_flow), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(cover_flow), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(cover_flow), 6);
    g_signal_connect(cover_flow, "child-activated", G_CALLBACK(on_cover_activated), NULL);
    gtk_container_add(GTK_CONTAINER(cover_sw), cover_flow);
    gtk_stack_add_named(GTK_STACK(pl_stack), cover_sw, "covers");
    gtk_stack_set_visible_child_name(GTK_STACK(pl_stack), "list");

    gtk_box_pack_start(GTK_BOX(left_vbox), pl_stack, TRUE, TRUE, 0);

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
    /* 封面格式徽标（release 280） */
    GtkWidget *cover_overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(cover_overlay), cover_stack);
    cover_fmt_badge = gtk_label_new("");
    gtk_widget_set_halign(cover_fmt_badge, GTK_ALIGN_END);
    gtk_widget_set_valign(cover_fmt_badge, GTK_ALIGN_START);
    gtk_widget_set_margin_end(cover_fmt_badge, 4);
    gtk_widget_set_margin_top(cover_fmt_badge, 4);
    gtk_overlay_add_overlay(GTK_OVERLAY(cover_overlay), cover_fmt_badge);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(cover_overlay), cover_fmt_badge, TRUE);
    gtk_box_pack_start(GTK_BOX(cover_hbox), cover_overlay, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(cover_stack, TR("点击封面放大预览"));
    g_signal_connect(cover_stack, "button-press-event", G_CALLBACK(on_cover_clicked), NULL);

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
    g_signal_connect(lyric_view, "scroll-event", G_CALLBACK(on_lyric_scroll), NULL);
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
    gtk_widget_set_tooltip_text(speed_combo, TR("播放速度"));
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
    /* 歌词字号下拉（release 277） */
    GtkWidget *lyric_font_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lyric_font_combo), "13", TR("歌词 小"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lyric_font_combo), "15", TR("歌词 中"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lyric_font_combo), "18", TR("歌词 大"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(lyric_font_combo), "15");
    gtk_widget_set_tooltip_text(lyric_font_combo, TR("歌词字号（也可 Ctrl+= / Ctrl+滚轮调节）"));
    {
        gchar *cdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
        gchar *cp = g_build_filename(cdir, "qymedia-lyric-font.conf", NULL);
        gchar *s = NULL;
        if (g_file_get_contents(cp, &s, NULL, NULL)) {
            int v = atoi(s ? s : "15");
            if (v >= 10 && v <= 30) {
                char id[8];
                if (v < 14) g_strlcpy(id, "13", 8);
                else if (v < 17) g_strlcpy(id, "15", 8);
                else g_strlcpy(id, "18", 8);
                lyric_font_size = atoi(id);
                gtk_combo_box_set_active_id(GTK_COMBO_BOX(lyric_font_combo), id);
            }
        }
        g_free(s); g_free(cp); g_free(cdir);
    }
    lyric_font_apply();
    g_signal_connect(lyric_font_combo, "changed", G_CALLBACK(on_lyric_font_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), lyric_font_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_vbox), bar, FALSE, FALSE, 0);

    /* 循环 + 均衡器 + 音量 */
    GtkWidget *bar2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *eq_btn = gtk_button_new_with_label(TR("均衡器"));
    gtk_widget_set_tooltip_text(eq_btn, TR("六段均衡器（63Hz-16kHz）"));
    qy_add_class(eq_btn, "qy-btn");
    g_signal_connect(eq_btn, "clicked", G_CALLBACK(on_eq_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), eq_btn, FALSE, FALSE, 0);
    GtkWidget *loop_btn = gtk_toggle_button_new_with_label(TR("循环"));
    g_signal_connect(loop_btn, "toggled", G_CALLBACK(on_loop_toggled), NULL);
    GtkWidget *shuffle_btn = gtk_toggle_button_new_with_label(TR("随机"));
    gtk_widget_set_tooltip_text(shuffle_btn, TR("随机播放（开/关）"));
    g_signal_connect(shuffle_btn, "toggled", G_CALLBACK(on_shuffle_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(bar2), loop_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar2), shuffle_btn, FALSE, FALSE, 0);
    GtkWidget *vol_lb = gtk_label_new(TR("音量:"));
    gtk_box_pack_start(GTK_BOX(bar2), vol_lb, FALSE, FALSE, 0);
    GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_widget_set_tooltip_text(vol_scale, TR("音量（0-100%）"));
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

    /* 启动时加载播放计数 + 上次保存的播放列表 */
    counts_load();
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