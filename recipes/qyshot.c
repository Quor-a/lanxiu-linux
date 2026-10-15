/* qyshot.c — 澜岫截图工具
 * 在 X11 显示后端下截取整个屏幕（gdk_get_default_root_window），
 * 支持延时截图（3/5/10 秒）、PNG/JPEG/BMP 格式选择、
 * 截图后弹出 GtkFileChooserNative 保存对话框，
 * 以及自动截图（QYSHOT_AUTO）与复制到剪贴板。
 */
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "qytheme.h"
#include "qyl10n.h"

static GtkWidget *preview = NULL;      /* 截图预览 */
static GdkPixbuf *shot_pixbuf = NULL;
static GtkWidget *status_label = NULL;
static GtkWidget *win = NULL;           /* 主窗口（保存对话框父窗口） */
static GtkWidget *cap_btn = NULL;       /* 全屏截图按钮（倒计时文字） */
static GtkWidget *region_btn = NULL;    /* 区域截图按钮 */
static GtkWidget *delay_combo = NULL;   /* 延时选择 */
static GtkWidget *format_combo = NULL; /* 保存格式选择 */
static gboolean auto_copy = FALSE;     /* 截图后自动复制到剪贴板（release 292） */
static GtkWidget *b_autocopy = NULL;   /* 自动复制开关按钮 */
static GtkWidget *quality_spin = NULL; /* JPEG 质量滑块（release 298） */
static int jpeg_quality = 85;          /* JPEG 保存质量（release 298） */

static void on_fmt_changed(GtkComboBox *combo, gpointer ud) {
    (void)ud;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    /* release 298: 质量控件仅在 JPEG 格式下可用 */
    if (quality_spin)
        gtk_widget_set_sensitive(quality_spin, strcmp(id, "jpeg") == 0);
    gchar *dir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    gchar *path = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyshot-fmt", NULL);
    g_file_set_contents(path, id, -1, NULL);
    g_free(path);
}

/* 质量滑块变化：更新全局质量并持久化（release 298） */
static void on_quality_changed(GtkSpinButton *spin, gpointer ud) {
    (void)ud;
    jpeg_quality = gtk_spin_button_get_value_as_int(spin);
    gchar *path = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyshot-quality", NULL);
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    gchar *val = g_strdup_printf("%d", jpeg_quality);
    g_file_set_contents(path, val, -1, NULL);
    g_free(val);
    g_free(path);
}
static char last_path[512] = "";

static guint countdown_id = 0;          /* 倒计时定时器 id */
static int countdown_secs = 0;          /* 剩余秒数 */

static gboolean on_capture_wrap(gpointer p);   /* 前向声明 */

/* 解析 qyshot-capture 输出的 24-bit BMP（gdk-pixbuf 无 BMP 加载器时自解析） */
static GdkPixbuf *bmp_to_pixbuf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return NULL;
    }
    int w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
    int h = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
    int offset = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24);
    if (w <= 0 || h <= 0 || w > 20000 || h > 20000 || offset < 54) {
        fclose(f);
        return NULL;
    }
    fseek(f, offset, SEEK_SET);
    int row_size = (w * 3 + 3) & ~3;
    unsigned char *row = malloc(row_size);
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    if (!row || !pb) {
        free(row);
        if (pb) g_object_unref(pb);
        fclose(f);
        return NULL;
    }
    int n_channels = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, row_size, f) != (size_t)row_size) break;
        guchar *dst = pixels + (h - 1 - y) * rowstride;   /* BMP 自底向上 */
        for (int x = 0; x < w; x++) {
            dst[x * n_channels]     = row[x * 3 + 2];    /* R */
            dst[x * n_channels + 1] = row[x * 3 + 1];    /* G */
            dst[x * n_channels + 2] = row[x * 3];        /* B */
        }
    }
    free(row);
    fclose(f);
    return pb;
}

/* 截取整个屏幕: Wayland 优先用 grim/weston-screenshooter，否则回退 X11 qyshot-capture */
static GdkPixbuf *capture_screen(void) {
    const char *disp = g_getenv("QYSHOT_DISPLAY");
    if (!disp) disp = ":0";
    char tmp[96];
    GdkPixbuf *pb = NULL;
    char cmd[640];
    const char *wld = g_getenv("WAYLAND_DISPLAY");
    const char *tool = NULL;
    if (wld) {
        if (g_find_program_in_path("grim")) tool = "grim";
        else if (g_find_program_in_path("weston-screenshooter")) tool = "weston-screenshooter";
    }
    if (tool) {
        g_printerr("QYSHOTDBG: wayland capture via %s\n", tool);
        g_snprintf(tmp, sizeof tmp, "/tmp/qyshot-%d.png", (int)getpid());
        g_snprintf(cmd, sizeof cmd, "%s %s 2>/dev/null", tool, tmp);
        if (system(cmd) == 0)
            pb = gdk_pixbuf_new_from_file(tmp, NULL);
        unlink(tmp);
        return pb;
    }
    /* X11 回退（测试环境/无 Wayland 截图工具时仍可用） */
    g_printerr("QYSHOTDBG: x11 fallback capture disp=%s\n", disp);
    g_snprintf(tmp, sizeof tmp, "/tmp/qyshot-%d.bmp", (int)getpid());
    g_snprintf(cmd, sizeof cmd, "DISPLAY=%s /usr/bin/qyshot-capture %s 2>/dev/null", disp, tmp);
    if (system(cmd) == 0)
        pb = bmp_to_pixbuf(tmp);
    unlink(tmp);
    return pb;
}

/* 生成默认文件名: 截图_年月日_时分秒.<ext> */
static void make_default_path(char *buf, size_t sz, const char *fmt) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    const char *ext = !strcmp(fmt, "jpeg") ? "jpg" : fmt;
    g_snprintf(buf, sz, "%s/截图_%04d%02d%02d_%02d%02d%02d.%s",
               g_get_user_special_dir(G_USER_DIRECTORY_PICTURES),
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec, ext);
}

/* 手动写出 24-bit BMP（gdk-pixbuf 无 BMP 写入模块时兜底） */
static gboolean write_bmp(GdkPixbuf *pb, const char *path, GError **err) {
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int row_size = (w * 3 + 3) & ~3;
    int data_size = row_size * h;
    int file_size = 54 + data_size;
    unsigned char hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xFF;
    hdr[3] = (file_size >> 8) & 0xFF;
    hdr[4] = (file_size >> 16) & 0xFF;
    hdr[5] = (file_size >> 24) & 0xFF;
    hdr[10] = 54;                       /* 像素数据偏移 */
    hdr[14] = 40;                       /* DIB 头大小 */
    hdr[18] = w & 0xFF;
    hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF;
    hdr[21] = (w >> 24) & 0xFF;
    hdr[22] = h & 0xFF;
    hdr[23] = (h >> 8) & 0xFF;
    hdr[24] = (h >> 16) & 0xFF;
    hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1;                        /* 颜色平面 */
    hdr[28] = 24;                       /* 每像素位数 */
    hdr[34] = data_size & 0xFF;
    hdr[35] = (data_size >> 8) & 0xFF;
    hdr[36] = (data_size >> 16) & 0xFF;
    hdr[37] = (data_size >> 24) & 0xFF;

    FILE *f = fopen(path, "wb");
    if (!f) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot open %s", path);
        return FALSE;
    }
    if (fwrite(hdr, 1, 54, f) != 54) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "write header failed");
        fclose(f);
        return FALSE;
    }
    int n_channels = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    const guchar *pixels = gdk_pixbuf_get_pixels(pb);
    unsigned char *row = malloc(row_size);
    if (!row) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "out of memory");
        fclose(f);
        return FALSE;
    }
    for (int y = 0; y < h; y++) {
        const guchar *src = pixels + (h - 1 - y) * rowstride;  /* BMP 自底向上 */
        for (int x = 0; x < w; x++) {
            row[x * 3]     = src[x * n_channels + 2];  /* B */
            row[x * 3 + 1] = src[x * n_channels + 1];  /* G */
            row[x * 3 + 2] = src[x * n_channels];      /* R */
        }
        if (fwrite(row, 1, row_size, f) != (size_t)row_size) {
            g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "write pixels failed");
            free(row);
            fclose(f);
            return FALSE;
        }
    }
    free(row);
    fclose(f);
    return TRUE;
}

/* 按格式保存 pixbuf；JPEG 质量由滑块决定（release 298），BMP 优先 gdk-pixbuf 写入、失败时手动兜底 */
static gboolean save_pixbuf(GdkPixbuf *pb, const char *path, const char *fmt, GError **err) {
    if (!strcmp(fmt, "jpeg")) {
        gchar *quality = g_strdup_printf("%d", jpeg_quality);
        gboolean ok = gdk_pixbuf_save(pb, path, "jpeg", err, "quality", quality, NULL);
        g_free(quality);
        return ok;
    }
    if (!strcmp(fmt, "bmp")) {
        if (gdk_pixbuf_save(pb, path, "bmp", err, NULL))
            return TRUE;
        g_clear_error(err);
        return write_bmp(pb, path, err);
    }
    return gdk_pixbuf_save(pb, path, "png", err, NULL);
}

/* 当前选中的保存格式: png / jpeg / bmp */
static const char *current_format(void) {
    const char *id = format_combo ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(format_combo)) : NULL;
    if (id && id[0]) return id;
    return "png";
}

static void update_preview(void) {
    if (preview && shot_pixbuf) {
        int w = gdk_pixbuf_get_width(shot_pixbuf);
        int h = gdk_pixbuf_get_height(shot_pixbuf);
        /* 缩放到预览区（最大 560x360） */
        GdkPixbuf *scaled = NULL;
        if (w > 560 || h > 360) {
            double s = 560.0 / w < 360.0 / h ? 560.0 / w : 360.0 / h;
            scaled = gdk_pixbuf_scale_simple(shot_pixbuf, (int)(w * s), (int)(h * s),
                                             GDK_INTERP_BILINEAR);
        }
        gtk_image_set_from_pixbuf(GTK_IMAGE(preview), scaled ? scaled : shot_pixbuf);
        if (scaled) g_object_unref(scaled);
    }
}

/* 保存对话框（GtkFileChooserNative） */
static void on_save(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!shot_pixbuf) return;
    const char *fmt = current_format();
    char def[512];
    make_default_path(def, sizeof def, fmt);
    gchar *base = g_path_get_basename(def);

    GtkFileChooserNative *dlg = gtk_file_chooser_native_new(
        TR("保存截图"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SAVE,
        TR("保存"), TR("取消"));
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), base);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    g_free(base);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            GError *err = NULL;
            if (save_pixbuf(shot_pixbuf, path, fmt, &err)) {
                g_strlcpy(last_path, path, sizeof last_path);
                char st[640];
                g_snprintf(st, sizeof st, "%s: %s", TR("已保存"), path);
                gtk_label_set_text(GTK_LABEL(status_label), st);
                gchar *title = g_strdup_printf("%s - %s", TR("澜岫截图"), path);
                gtk_window_set_title(GTK_WINDOW(win), title);
                g_free(title);
            } else {
                gtk_label_set_text(GTK_LABEL(status_label), err ? err->message : TR("保存失败"));
                g_clear_error(&err);
            }
            g_free(path);
        }
    }
    g_object_unref(dlg);
}

/* 全屏截图（截完后自动弹保存对话框） */
static void on_capture(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (shot_pixbuf) { g_object_unref(shot_pixbuf); shot_pixbuf = NULL; }
    shot_pixbuf = capture_screen();
    if (!shot_pixbuf) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("截图失败：无法访问屏幕"));
        return;
    }
    update_preview();
    char st[128];
    g_snprintf(st, sizeof st, "%d×%d %s",
               gdk_pixbuf_get_width(shot_pixbuf), gdk_pixbuf_get_height(shot_pixbuf),
               TR("截图完成"));
    gtk_label_set_text(GTK_LABEL(status_label), st);
    /* release 292: 截图后自动复制到剪贴板 */
    if (auto_copy) {
        gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), shot_pixbuf);
        gtk_label_set_text(GTK_LABEL(status_label), TR("截图完成，已自动复制到剪贴板"));
    }
    /* 截图后自动弹出保存对话框（QYSHOT_AUTO 自动化模式除外） */
    if (!g_getenv("QYSHOT_AUTO"))
        on_save(NULL, NULL);
}

/* 倒计时：每秒更新按钮文字，到 0 执行截图 */
static gboolean countdown_tick(gpointer p) {
    (void)p;
    countdown_secs--;
    if (countdown_secs <= 0) {
        countdown_id = 0;
        gtk_button_set_label(GTK_BUTTON(cap_btn), TR("全屏截图"));
        gtk_widget_set_sensitive(cap_btn, TRUE);
        on_capture(NULL, NULL);
        return G_SOURCE_REMOVE;
    }
    char buf[64];
    g_snprintf(buf, sizeof buf, TR("%d 秒后截图..."), countdown_secs);
    gtk_button_set_label(GTK_BUTTON(cap_btn), buf);
    return G_SOURCE_CONTINUE;
}

/* 截图按钮：有延时则启动倒计时（再次点击取消倒计时，release 295），否则立即截图 */
static void on_capture_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (countdown_id) {
        g_source_remove(countdown_id);
        countdown_id = 0;
        gtk_button_set_label(GTK_BUTTON(cap_btn), TR("全屏截图"));
        gtk_widget_set_sensitive(cap_btn, TRUE);
        if (status_label) gtk_label_set_text(GTK_LABEL(status_label), TR("已取消倒计时截图"));
        return;
    }
    int delay = 0;
    const char *sel = gtk_combo_box_get_active_id(GTK_COMBO_BOX(delay_combo));
    if (sel) delay = atoi(sel);
    if (delay > 0) {
        countdown_secs = delay;
        gtk_widget_set_sensitive(cap_btn, FALSE);
        char buf[64];
        g_snprintf(buf, sizeof buf, TR("%d 秒后截图..."), countdown_secs);
        gtk_button_set_label(GTK_BUTTON(cap_btn), buf);
        countdown_id = g_timeout_add_seconds(1, countdown_tick, NULL);
        g_printerr("QYSHOTDBG: delay %ds start\n", delay);
    } else {
        on_capture(NULL, NULL);
    }
}

/* 复制到剪贴板 */
static void on_copy(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!shot_pixbuf) return;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_image(cb, shot_pixbuf);
    gtk_label_set_text(GTK_LABEL(status_label), TR("已复制到剪贴板"));
}

/* 自动复制开关：切换即写配置（release 292） */
static void on_autocopy_toggled(GtkWidget *w, gpointer ud) {
    (void)ud;
    auto_copy = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    gchar *path = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyshot-copy", NULL);
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(path, auto_copy ? "1" : "0", -1, NULL);
    g_free(path);
    if (status_label)
        gtk_label_set_text(GTK_LABEL(status_label),
                           auto_copy ? TR("已开启：截图后自动复制到剪贴板") : TR("已关闭自动复制"));
}

/* 打开截图文件夹（release 300）：使用与保存截图相同的目录逻辑。
 * 优先使用最近一次保存路径的目录，否则使用默认图片目录；
 * 目录不存在则创建；打开失败时在状态栏显示错误，不崩溃。 */
static void on_open_folder(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    gchar *dir = NULL;
    if (last_path[0]) {
        dir = g_path_get_dirname(last_path);
    } else {
        const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
        if (pics && pics[0])
            dir = g_strdup(pics);
        else
            dir = g_strdup(g_get_home_dir());
    }
    if (!dir || !dir[0]) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("无法确定截图文件夹"));
        g_free(dir);
        return;
    }
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        char st[320];
        g_snprintf(st, sizeof st, "%s: %s", TR("无法创建截图文件夹"), dir);
        gtk_label_set_text(GTK_LABEL(status_label), st);
        g_free(dir);
        return;
    }
    GError *err = NULL;
    gchar *uri = g_filename_to_uri(dir, NULL, &err);
    if (!uri) {
        char st[480];
        g_snprintf(st, sizeof st, "%s: %s",
                   TR("打开截图文件夹失败"), err ? err->message : TR("未知错误"));
        gtk_label_set_text(GTK_LABEL(status_label), st);
        g_clear_error(&err);
        g_free(dir);
        return;
    }
    if (gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, &err)) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("已打开截图文件夹"));
    } else {
        char st[480];
        g_snprintf(st, sizeof st, "%s: %s",
                   TR("打开截图文件夹失败"), err ? err->message : TR("无默认文件管理器"));
        gtk_label_set_text(GTK_LABEL(status_label), st);
        g_clear_error(&err);
    }
    g_free(uri);
    g_free(dir);
}

/* ---------- 区域截图（X11） ----------
 * X11：全屏抓取根窗口为 pixbuf，弹出全屏无边框选择窗口，鼠标拖选矩形，
 *      松开后裁剪选中区域并进入现有保存流程。
 * Wayland（或非 X11 构建）：按钮禁用并提示。
 */
#ifdef GDK_WINDOWING_X11
typedef struct {
    GdkPixbuf *shot;      /* 全屏截图 */
    GtkWidget *win;       /* 选择窗口 */
    GtkWidget *da;        /* 绘图区 */
    int iw, ih;           /* 截图尺寸 */
    gboolean dragging;    /* 是否正在拖选 */
    int x0, y0, x1, y1;   /* 选择矩形（窗口坐标） */
} RegionSel;

static gboolean region_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    RegionSel *rs = (RegionSel *)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (!rs->shot || a.width < 1 || a.height < 1) return FALSE;
    /* 截图铺满窗口 */
    double sx = (double)a.width / rs->iw;
    double sy = (double)a.height / rs->ih;
    cairo_save(cr);
    cairo_scale(cr, sx, sy);
    gdk_cairo_set_source_pixbuf(cr, rs->shot, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    /* 拖选时：选区外暗化 + 橙色虚线框 */
    if (rs->dragging) {
        int x0 = MIN(rs->x0, rs->x1), y0 = MIN(rs->y0, rs->y1);
        int x1 = MAX(rs->x0, rs->x1), y1 = MAX(rs->y0, rs->y1);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_rectangle(cr, 0, 0, a.width, y0); cairo_fill(cr);
        cairo_rectangle(cr, 0, y1, a.width, a.height - y1); cairo_fill(cr);
        cairo_rectangle(cr, 0, y0, x0, y1 - y0); cairo_fill(cr);
        cairo_rectangle(cr, x1, y0, a.width - x1, y1 - y0); cairo_fill(cr);
        cairo_set_source_rgb(cr, 1.0, 0x5C / 255.0, 0x1A / 255.0);   /* #FF5C1A */
        cairo_set_line_width(cr, 1.5);
        double dashes[] = {4, 4};
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    return FALSE;
}

static gboolean region_button_cb(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    RegionSel *rs = (RegionSel *)ud;
    if (ev->button == 1) {
        rs->dragging = TRUE;
        rs->x0 = rs->x1 = (int)ev->x;
        rs->y0 = rs->y1 = (int)ev->y;
        gtk_widget_queue_draw(w);
        return TRUE;
    }
    if (ev->button == 3) {
        /* 右键取消选择 */
        gtk_widget_destroy(rs->win);
        return TRUE;
    }
    return FALSE;
}

static gboolean region_motion_cb(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    RegionSel *rs = (RegionSel *)ud;
    if (rs->dragging) {
        rs->x1 = (int)ev->x;
        rs->y1 = (int)ev->y;
        gtk_widget_queue_draw(w);
    }
    return TRUE;
}

static gboolean region_release_cb(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    RegionSel *rs = (RegionSel *)ud;
    if (ev->button != 1 || !rs->dragging) return TRUE;
    rs->dragging = FALSE;
    int x0 = MIN(rs->x0, rs->x1), y0 = MIN(rs->y0, rs->y1);
    int x1 = MAX(rs->x0, rs->x1), y1 = MAX(rs->y0, rs->y1);
    int aw = gtk_widget_get_allocated_width(w);
    int ah = gtk_widget_get_allocated_height(w);
    /* 窗口坐标 → 截图坐标 */
    double sx = aw > 0 ? (double)rs->iw / aw : 1.0;
    double sy = ah > 0 ? (double)rs->ih / ah : 1.0;
    int ix0 = (int)(x0 * sx), iy0 = (int)(y0 * sy);
    int ix1 = (int)(x1 * sx), iy1 = (int)(y1 * sy);
    int cw = ix1 - ix0, ch = iy1 - iy0;
    if (cw > 0 && ch > 0 && rs->shot) {
        GdkPixbuf *crop = gdk_pixbuf_new(GDK_COLORSPACE_RGB,
                                         gdk_pixbuf_get_has_alpha(rs->shot), 8, cw, ch);
        if (crop) {
            gdk_pixbuf_copy_area(rs->shot, ix0, iy0, cw, ch, crop, 0, 0);
            if (shot_pixbuf) g_object_unref(shot_pixbuf);
            shot_pixbuf = crop;
            update_preview();
            gchar *st = g_strdup_printf("%d×%d %s", cw, ch, TR("区域截图完成"));
            gtk_label_set_text(GTK_LABEL(status_label), st);
            g_free(st);
        }
    }
    gtk_widget_destroy(rs->win);
    /* 进入现有保存流程 */
    if (!g_getenv("QYSHOT_AUTO"))
        on_save(NULL, NULL);
    return TRUE;
}

static gboolean region_key_cb(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)ud;
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

/* 选择窗口销毁：释放截图并释放状态结构 */
static void region_sel_destroy(GtkWidget *w, gpointer ud) {
    (void)w;
    RegionSel *rs = (RegionSel *)ud;
    if (rs->shot) g_object_unref(rs->shot);
    g_free(rs);
}
#endif /* GDK_WINDOWING_X11 */

/* 判断区域截图是否可用（X11 且非 Wayland 会话） */
static gboolean region_screenshot_supported(void) {
#ifdef GDK_WINDOWING_X11
    if (g_getenv("WAYLAND_DISPLAY")) return FALSE;
    return TRUE;
#else
    return FALSE;
#endif
}

/* 区域截图入口 */
static void on_region_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (g_getenv("WAYLAND_DISPLAY")) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("Wayland 不支持区域截图"));
        return;
    }
#ifdef GDK_WINDOWING_X11
    GdkWindow *root = gdk_get_default_root_window();
    if (!root) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("截图失败：无法访问屏幕"));
        return;
    }
    int sw = gdk_screen_width(), sh = gdk_screen_height();
    GdkPixbuf *shot = gdk_pixbuf_get_from_window(root, 0, 0, sw, sh);
    if (!shot) {
        gtk_label_set_text(GTK_LABEL(status_label), TR("截图失败：无法访问屏幕"));
        return;
    }
    RegionSel *rs = g_new0(RegionSel, 1);
    rs->shot = shot;
    rs->iw = gdk_pixbuf_get_width(shot);
    rs->ih = gdk_pixbuf_get_height(shot);

    rs->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(rs->win), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(rs->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(rs->win), sw, sh);
    gtk_window_move(GTK_WINDOW(rs->win), 0, 0);
    gtk_window_fullscreen(GTK_WINDOW(rs->win));

    rs->da = gtk_drawing_area_new();
    gtk_widget_add_events(rs->da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON1_MOTION_MASK |
                                  GDK_BUTTON_RELEASE_MASK | GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(rs->da, TRUE);
    gtk_container_add(GTK_CONTAINER(rs->win), rs->da);
    g_signal_connect(rs->da, "draw", G_CALLBACK(region_draw_cb), rs);
    g_signal_connect(rs->da, "button-press-event", G_CALLBACK(region_button_cb), rs);
    g_signal_connect(rs->da, "motion-notify-event", G_CALLBACK(region_motion_cb), rs);
    g_signal_connect(rs->da, "button-release-event", G_CALLBACK(region_release_cb), rs);
    g_signal_connect(rs->win, "key-press-event", G_CALLBACK(region_key_cb), NULL);
    g_signal_connect(rs->win, "destroy", G_CALLBACK(region_sel_destroy), rs);

    gtk_widget_show_all(rs->win);
    gtk_window_present(GTK_WINDOW(rs->win));
    gtk_widget_grab_focus(rs->da);
    /* 十字光标（顶层窗口与绘图区都设置，X11 下层窗口默认继承父光标） */
    GdkDisplay *dpy = gdk_display_get_default();
    GdkCursor *cur = gdk_cursor_new_for_display(dpy, GDK_CROSSHAIR);
    if (cur) {
        GdkWindow *gdkw = gtk_widget_get_window(rs->win);
        if (gdkw) gdk_window_set_cursor(gdkw, cur);
        GdkWindow *daw = gtk_widget_get_window(rs->da);
        if (daw) gdk_window_set_cursor(daw, cur);
        g_object_unref(cur);
    }
#else
    gtk_label_set_text(GTK_LABEL(status_label), TR("Wayland 不支持区域截图"));
#endif
}

/* 根据构建/运行环境刷新区域截图按钮可用性 */
static void update_region_button_state(void) {
    if (!region_btn) return;
    gboolean ok = region_screenshot_supported();
    gtk_widget_set_sensitive(region_btn, ok);
    gtk_widget_set_tooltip_text(region_btn,
        ok ? TR("区域截图") : TR("Wayland 不支持区域截图"));
}

/* 自动化: QYSHOT_AUTO=保存路径 启动后自动截图保存退出 */
static gboolean auto_shot(gpointer p) {
    (void)p;
    const char *path = g_getenv("QYSHOT_AUTO");
    if (shot_pixbuf) { g_object_unref(shot_pixbuf); shot_pixbuf = NULL; }
    shot_pixbuf = capture_screen();
    if (!shot_pixbuf) return G_SOURCE_REMOVE;
    GError *err = NULL;
    if (gdk_pixbuf_save(shot_pixbuf, path, "png", &err, NULL)) {
        g_printerr("QYSHOT_AUTO: saved %s\n", path);
        exit(0);
    } else {
        g_printerr("QYSHOT_AUTO: fail %s\n", err ? err->message : "?");
        g_clear_error(&err);
        exit(1);
    }
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer ud) {
    (void)ud;
    qy_load_theme();
    win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫截图"));
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 480);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 预览区 */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    qy_add_class(sw, "qy-app-surface");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 360);
    preview = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(sw), preview);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    /* 控制行：延时 + 格式 */
    GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(ctl), gtk_label_new(TR("延时:")), FALSE, FALSE, 0);
    delay_combo = gtk_combo_box_text_new();
    qy_add_class(delay_combo, "qy-shot-combo");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(delay_combo), "0",  TR("0 秒（立即）"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(delay_combo), "3",  TR("3 秒"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(delay_combo), "5",  TR("5 秒"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(delay_combo), "10", TR("10 秒"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(delay_combo), 0);
    gtk_box_pack_start(GTK_BOX(ctl), delay_combo, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ctl), gtk_label_new(TR("格式:")), FALSE, FALSE, 0);
    format_combo = gtk_combo_box_text_new();
    qy_add_class(format_combo, "qy-shot-combo");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "png",  "PNG");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "jpeg", "JPEG");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "bmp",  "BMP");
    /* 记忆上次格式（release 269） */
    gchar *fmt_path = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyshot-fmt", NULL);
    gchar *fmt_old = NULL;
    if (g_file_get_contents(fmt_path, &fmt_old, NULL, NULL)) {
        const char *want = g_strstrip(fmt_old);
        for (int i = 0; i < 3; i++) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(format_combo), i);
            const char *cur = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format_combo));
            if (cur && g_strcmp0(cur, want) == 0) break;
        }
        g_free(fmt_old);
    }
    g_free(fmt_path);
    g_signal_connect(format_combo, "changed", G_CALLBACK(on_fmt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(ctl), format_combo, FALSE, FALSE, 0);

    /* 质量滑块（release 298）：仅 JPEG 格式可用，值持久化到 qyshot-quality */
    gtk_box_pack_start(GTK_BOX(ctl), gtk_label_new(TR("质量:")), FALSE, FALSE, 0);
    quality_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(quality_spin), jpeg_quality);
    gtk_entry_set_width_chars(GTK_ENTRY(quality_spin), 3);
    qy_add_class(quality_spin, "qy-shot-combo");
    gtk_box_pack_start(GTK_BOX(ctl), quality_spin, FALSE, FALSE, 0);
    /* 启动加载上次质量（release 298） */
    {
        gchar *qpath = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyshot-quality", NULL);
        gchar *qval = NULL;
        if (g_file_get_contents(qpath, &qval, NULL, NULL)) {
            int v = atoi(g_strstrip(qval));
            if (v >= 1 && v <= 100) {
                jpeg_quality = v;
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(quality_spin), v);
            }
            g_free(qval);
        }
        g_free(qpath);
    }
    g_signal_connect(quality_spin, "value-changed", G_CALLBACK(on_quality_changed), NULL);
    /* release 300: 打开截图文件夹按钮（默认目录与保存截图一致） */
    GtkWidget *b_open_folder = gtk_button_new_with_label(TR("打开文件夹"));
    qy_add_class(b_open_folder, "qy-shot-combo");
    g_signal_connect(b_open_folder, "clicked", G_CALLBACK(on_open_folder), NULL);
    gtk_box_pack_start(GTK_BOX(ctl), b_open_folder, FALSE, FALSE, 0);
    on_fmt_changed(GTK_COMBO_BOX(format_combo), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), ctl, FALSE, FALSE, 0);

    /* 按钮行 */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    cap_btn = gtk_button_new_with_label(TR("全屏截图"));
    qy_add_class(cap_btn, "qy-shot-btn");
    region_btn = gtk_button_new_with_label(TR("区域截图"));
    qy_add_class(region_btn, "qy-shot-btn");
    GtkWidget *b_save = qy_icon_button(QY_ICON_SAVE, 18, TR("保存"), NULL);
    GtkWidget *b_copy = qy_icon_button(QY_ICON_COPY, 18, TR("复制到剪贴板"), NULL);
    b_autocopy = gtk_toggle_button_new_with_label(TR("自动复制"));
    qy_add_class(b_autocopy, "qy-shot-btn");
    g_signal_connect(cap_btn, "clicked", G_CALLBACK(on_capture_clicked), NULL);
    g_signal_connect(region_btn, "clicked", G_CALLBACK(on_region_clicked), NULL);
    g_signal_connect(b_save, "clicked", G_CALLBACK(on_save), NULL);
    g_signal_connect(b_copy, "clicked", G_CALLBACK(on_copy), NULL);
    g_signal_connect(b_autocopy, "toggled", G_CALLBACK(on_autocopy_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(bar), cap_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), region_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_autocopy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
    /* release 292: 恢复自动复制开关状态 */
    {
        gchar *cp = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qyshot-copy", NULL);
        gchar *val = NULL;
        if (g_file_get_contents(cp, &val, NULL, NULL) && val && val[0] == '1')
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_autocopy), TRUE);
        g_free(val);
        g_free(cp);
    }

    /* 状态栏 */
    status_label = gtk_label_new(TR("就绪"));
    qy_add_class(status_label, "qy-mon-info");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    const char *auto_path = g_getenv("QYSHOT_AUTO");
    if (auto_path)
        g_timeout_add(800, auto_shot, NULL);

    update_region_button_state();
    gtk_widget_show_all(win);
    /* 启动后自动截一帧（X11 后端需窗口就绪后） */
    g_timeout_add(400, (GSourceFunc)on_capture_wrap, NULL);
}

/* 包装: 返回 FALSE 的一次性回调 */
static gboolean on_capture_wrap(gpointer p) {
    (void)p;
    on_capture(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* 模式截图: --window（当前窗口）/ --region（区域选择）
 * X11 有 import/xdotool 时用 ImageMagick；否则回退全屏截图。
 * Wayland 无窗口/区域选择协议，回退全屏并打印提示。 */
static int capture_mode_screenshot(const char *mode) {
    const char *wld = g_getenv("WAYLAND_DISPLAY");
    const char *disp = g_getenv("QYSHOT_DISPLAY");
    if (!disp) disp = ":0";
    const char *out = !strcmp(mode, "--window") ? "/tmp/qyshot-window.png" : "/tmp/qyshot-region.png";
    char cmd[768];
    const char *xdo = NULL, *imp = NULL;
    if (!wld) {
        xdo = g_find_program_in_path("xdotool");
        imp = g_find_program_in_path("import");
    }
    if (!strcmp(mode, "--window") && xdo && imp) {
        g_snprintf(cmd, sizeof cmd, "import -window $(%s getactivewindow) %s 2>/dev/null", xdo, out);
    } else if (!strcmp(mode, "--region") && imp) {
        g_snprintf(cmd, sizeof cmd, "%s %s 2>/dev/null", imp, out);
    } else {
        const char *bmp = !strcmp(mode, "--window") ? "/tmp/qyshot-window.bmp" : "/tmp/qyshot-region.bmp";
        g_printerr("QYSHOTMODE: %s 回退全屏（%s）\n", mode,
                   wld ? "Wayland 无窗口/区域选择协议" : "无 xdotool/import");
        g_snprintf(cmd, sizeof cmd, "DISPLAY=%s /usr/bin/qyshot-capture %s 2>/dev/null", disp, bmp);
        return system(cmd) == 0 ? 0 : 1;
    }
    g_printerr("QYSHOTMODE: %s -> %s\n", mode, cmd);
    return system(cmd) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--window") || !strcmp(argv[1], "--region"))) {
        return capture_mode_screenshot(argv[1]);
    }
    GtkApplication *app = gtk_application_new("com.lanxiu.shot", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}