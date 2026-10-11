/* qyrec.c — 澜岫屏幕录制 (GTK3)
 * 依赖 ffmpeg 的 x11grab 后端录制 X11 屏幕 + 默认 PulseAudio 输入。
 * 会话检测：Wayland 且无 wf-recorder/pipewire 时提示暂不支持；
 * 无 ffmpeg 时提示；即使无法录制，界面与状态机也完整可用、不崩溃。
 */
#include "qyl10n.h"
#include "qytheme.h"
#include "qyicon.h"
#include <gtk/gtk.h>
#include <glib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

static GtkWidget *win = NULL;
static GtkWidget *status_label = NULL;   /* #qy-rec-status */
static GtkWidget *folder_btn = NULL;      /* GtkFileChooserButton 选择保存目录 */
static GtkWidget *file_entry = NULL;      /* 文件名输入框 */
static GtkWidget *rec_btn = NULL;               /* .qy-rec-btn 开始/停止 */
static GtkWidget *hint_label = NULL;          /* 底部提示 */

static gboolean recording = FALSE;            /* 录制状态机 */
static GPid rec_pid = 0;                        /* ffmpeg 进程 */
static gboolean recording_available = FALSE;    /* 当前会话是否支持录制 */
static gchar *unsupported_reason = NULL;

/* ---------- 状态与 UI ---------- */
static void set_status(const char *text) {
    if (status_label)
        gtk_label_set_text(GTK_LABEL(status_label), text);
}

static void set_recording_ui(gboolean rec) {
    if (!rec_btn) return;
    GtkStyleContext *sc = gtk_widget_get_style_context(rec_btn);
    if (rec) {
        gtk_button_set_label(GTK_BUTTON(rec_btn), TR("停止"));
        gtk_style_context_add_class(sc, "qy-rec-btn-stop");
        set_status(TR("正在录制..."));
    } else {
        gtk_button_set_label(GTK_BUTTON(rec_btn), TR("开始录制"));
        gtk_style_context_remove_class(sc, "qy-rec-btn-stop");
        if (recording_available)
            set_status(TR("未录制"));
        else
            set_status(unsupported_reason ? unsupported_reason : TR("未录制"));
    }
}

/* ---------- 支持性检测 ---------- */
static gboolean detect_recording_support(void) {
    const char *wayland = g_getenv("WAYLAND_DISPLAY");
    gboolean has_ffmpeg = FALSE;
    gchar *p = g_find_program_in_path("ffmpeg");
    has_ffmpeg = (p != NULL);
    g_free(p);

    if (wayland && wayland[0]) {
        gboolean has_wf = FALSE, has_pw = FALSE;
        p = g_find_program_in_path("wf-recorder");
        has_wf = (p != NULL);
        g_free(p);
        p = g_find_program_in_path("pipewire");
        has_pw = (p != NULL);
        g_free(p);
        if (!has_wf && !has_pw) {
            g_free(unsupported_reason);
            unsupported_reason = g_strdup(TR("当前 Wayland 会话暂不支持屏幕录制（需 pipewire/wf-recorder）"));
            return FALSE;
        }
    }
    if (!has_ffmpeg) {
        g_free(unsupported_reason);
        unsupported_reason = g_strdup(TR("未检测到 ffmpeg，无法屏幕录制"));
        return FALSE;
    }
    return TRUE;
}

/* ---------- 默认保存目录与文件名: ~/Videos + 录制-<时间戳>.mkv ---------- */
static char *default_rec_dir(void) {
    const gchar *dir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    if (!dir || !dir[0]) dir = g_get_home_dir();
    return g_strdup(dir);
}

static char *default_rec_name(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    return g_strdup_printf("录制-%04d%02d%02d_%02d%02d%02d.mkv",
                           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* ---------- 录制进程管理 ---------- */
static void on_rec_child_exited(GPid pid, gint status, gpointer ud) {
    (void)ud;
    g_spawn_close_pid(pid);
    rec_pid = 0;
    if (recording) {
        recording = FALSE;
        set_recording_ui(FALSE);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            set_status(TR("录制已停止"));
            g_printerr("QYRECDBG: exited cleanly\n");
        } else {
            char buf[128];
            g_snprintf(buf, sizeof buf, TR("录制已结束（ffmpeg 退出码 %d）"),
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            set_status(buf);
            g_printerr("QYRECDBG: ffmpeg exit status=%d\n", status);
        }
    }
}

/* 兜底：5 秒后进程仍未退出则强杀 */
static gboolean force_kill_timeout(gpointer p) {
    (void)p;
    if (recording && rec_pid > 0) {
        g_printerr("QYRECDBG: force kill %d\n", (int)rec_pid);
        kill(rec_pid, SIGKILL);
    }
    return G_SOURCE_REMOVE;
}

static void stop_recording(void) {
    if (rec_pid > 0) {
        g_printerr("QYRECDBG: stop pid=%d\n", (int)rec_pid);
        kill(rec_pid, SIGTERM);
        set_status(TR("正在停止..."));
        g_timeout_add(5000, force_kill_timeout, NULL);
    }
}

static void start_recording(void) {
    const gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(folder_btn));
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(file_entry));
    if (!dir || !dir[0] || !name || !name[0]) {
        set_status(TR("请先选择保存路径"));
        return;
    }
    gchar *out = g_build_filename(dir, name, NULL);
    gchar *out_dir = g_path_get_dirname(out);
    g_mkdir_with_parents(out_dir, 0700);
    g_free(out_dir);

    /* 屏幕分辨率（优先主显示器几何，X11/Wayland 均可用） */
    GdkDisplay *disp = gdk_display_get_default();
    int sw = 1920, sh = 1080;
    if (disp) {
        GdkMonitor *mon = gdk_display_get_monitor(disp, 0);
        if (mon) {
            GdkRectangle geo;
            gdk_monitor_get_geometry(mon, &geo);
            sw = geo.width > 0 ? geo.width : sw;
            sh = geo.height > 0 ? geo.height : sh;
        }
    }
    char res[32];
    g_snprintf(res, sizeof res, "%dx%d", sw, sh);

    /* ffmpeg -f x11grab -video_size <分辨率> -i :0 -f pulse -i default <路径> */
    gchar *argv[] = {
        "ffmpeg", "-y",
        "-f", "x11grab", "-video_size", res, "-i", ":0",
        "-f", "pulse", "-i", "default",
        "-vcodec", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
        out, NULL
    };
    GError *err = NULL;
    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                  NULL, NULL, &rec_pid, &err);
    if (err) {
        set_status(err->message);
        g_error_free(err);
        rec_pid = 0;
        g_free(out);
        return;
    }
    recording = TRUE;
    set_recording_ui(TRUE);
    g_child_watch_add(rec_pid, on_rec_child_exited, NULL);
    g_printerr("QYRECDBG: recording start pid=%d res=%s -> %s\n",
                (int)rec_pid, res, out);
    g_free(out);
}

/* ---------- 开始/停止按钮 ---------- */
static void on_rec_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (recording) {
        stop_recording();
        return;
    }
    if (!recording_available) {
        set_status(unsupported_reason ? unsupported_reason : TR("当前会话不支持屏幕录制"));
        return;
    }
    start_recording();
}

/* 关闭窗口时结束录制，避免留下孤儿 ffmpeg 进程 */
static gboolean on_win_delete(GtkWidget *w, GdkEventAny *ev, gpointer ud) {
    (void)w; (void)ev; (void)ud;
    if (recording && rec_pid > 0)
        kill(rec_pid, SIGTERM);
    return FALSE;   /* 继续默认关闭流程 */
}

/* ---------- 窗口构建 ---------- */
static void activate(GtkApplication *app, gpointer ud) {
    (void)ud;
    qy_load_theme();
    win = gtk_application_window_new(app);
    gtk_widget_set_name(GTK_WIDGET(win), "qy-rec-win");
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫录屏"));
    gtk_window_set_default_size(GTK_WINDOW(win), 400, 260);
    gtk_widget_set_size_request(GTK_WIDGET(win), 380, 240);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶部状态 */
    status_label = gtk_label_new(TR("未录制"));
    gtk_widget_set_name(GTK_WIDGET(status_label), "qy-rec-status");
    gtk_widget_set_halign(status_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* 保存路径：目录选择按钮 + 文件名输入框 */
    GtkWidget *path_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *path_lb = gtk_label_new(TR("保存到:"));
    gtk_widget_set_halign(path_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(path_row), path_lb, FALSE, FALSE, 0);

    folder_btn = gtk_file_chooser_button_new(TR("选择保存目录"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_widget_set_hexpand(folder_btn, TRUE);
    char *def_dir = default_rec_dir();
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(folder_btn), def_dir);
    g_free(def_dir);
    gtk_box_pack_start(GTK_BOX(path_row), folder_btn, TRUE, TRUE, 0);

    file_entry = gtk_entry_new();
    char *def_name = default_rec_name();
    gtk_entry_set_text(GTK_ENTRY(file_entry), def_name);
    g_free(def_name);
    gtk_entry_set_width_chars(GTK_ENTRY(file_entry), 16);
    gtk_box_pack_start(GTK_BOX(path_row), file_entry, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), path_row, FALSE, FALSE, 0);

    /* 开始/停止按钮 */
    rec_btn = gtk_button_new_with_label(TR("开始录制"));
    qy_add_class(rec_btn, "qy-rec-btn");
    gtk_widget_set_size_request(rec_btn, -1, 40);
    g_signal_connect(rec_btn, "clicked", G_CALLBACK(on_rec_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), rec_btn, FALSE, FALSE, 4);

    /* 底部提示 */
    hint_label = gtk_label_new(NULL);
    qy_add_class(hint_label, "qy-rec-hint");
    gtk_label_set_line_wrap(GTK_LABEL(hint_label), TRUE);
    gtk_label_set_justify(GTK_LABEL(hint_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(hint_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), hint_label, TRUE, TRUE, 0);

    /* 会话支持检测 */
    recording_available = detect_recording_support();
    if (!recording_available) {
        set_status(unsupported_reason);
        gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "qy-rec-err");
        gtk_widget_set_sensitive(rec_btn, FALSE);
        if (hint_label) {
            gtk_label_set_text(GTK_LABEL(hint_label),
                TR("本机未检测到可用的屏幕录制后端，可安装 ffmpeg（X11）或 pipewire/wf-recorder（Wayland）后重试。"));
        }
    } else {
        if (hint_label) {
            const char *wld = g_getenv("WAYLAND_DISPLAY");
            gtk_label_set_text(GTK_LABEL(hint_label),
                wld ? TR("将通过 ffmpeg x11grab 尝试录制（XWayland）。")
                    : TR("通过 ffmpeg x11grab 录制屏幕与系统声音。"));
        }
    }

    g_signal_connect(win, "delete-event", G_CALLBACK(on_win_delete), NULL);
    gtk_widget_show_all(win);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.rec", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}