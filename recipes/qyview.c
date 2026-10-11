/* qyview — 澜岫图片查看器 v2
 * 数据源: GdkPixbuf (PNG/JPEG/BMP 等), cairo 缩放绘制
 * 功能: 打开单图 (argv[1]), 窗口自适应缩放, 滚轮缩放, 拖拽平移, 左右键切换同目录图片,
 *       F11 全屏切换, 空格播放/暂停幻灯片 (每 3s), 底部悬浮工具条
 * 架构: 单文件 GTK3, 与 qyfiles 同款编译方式
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GdkPixbuf *pix = NULL;          /* 原始图 */
static gchar *cur_dir = NULL;          /* 当前图所在目录 */
static GPtrArray *dir_files = NULL;    /* 同目录图片文件列表 */
static int dir_idx = -1;
static double zoom = 1.0;
static double pan_x = 0, pan_y = 0;
static GtkWidget *page_label = NULL;   /* 右下角页码 */
static GtkWindow *g_win = NULL;        /* 主窗口: 标题显示当前文件名 (n/N) */
static gchar *current_path = NULL;     /* 当前图片完整路径（保存用） */
static GtkWidget *save_status = NULL;  /* 底部保存结果提示 */
static GtkWidget *image_info = NULL;   /* 底部图片尺寸/格式/大小信息 */
static GtkWidget *zoom_label = NULL;   /* 底部当前缩放百分比 */
static GtkWidget *play_btn = NULL;      /* 悬浮条播放/暂停按钮 */
static GtkWidget *file_label = NULL;   /* 左下角文件名 */
static GtkWidget *g_da = NULL;          /* 绘图区（幻灯片定时器回调用） */
static guint slide_timer = 0;           /* 幻灯片定时器 id */
static gboolean slide_on = FALSE;      /* 幻灯片是否运行 */

/* 刷新底部缩放百分比显示 */
static void update_zoom(void) {
    if (zoom_label) {
        gchar buf[32];
        g_snprintf(buf, sizeof buf, TR("缩放 %d%%"), (int)(zoom * 100 + 0.5));
        gtk_label_set_text(GTK_LABEL(zoom_label), buf);
    }
}

static const char *IMG_EXT[] = {".png",".jpg",".jpeg",".bmp",".gif",".webp",".xpm", NULL};

static gboolean is_img(const char *name) {
    gchar *low = g_ascii_strdown(name, -1);
    gboolean ok = FALSE;
    for (int i = 0; IMG_EXT[i]; i++)
        if (g_str_has_suffix(low, IMG_EXT[i])) { ok = TRUE; break; }
    g_free(low);
    return ok;
}

static void scan_dir(const char *path) {
    if (!dir_files) dir_files = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(dir_files, 0);
    dir_idx = -1;
    gchar *dir = g_path_get_dirname(path);
    gchar *base = g_path_get_basename(path);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d)))
            if (is_img(n)) {
                g_ptr_array_add(dir_files, g_strdup(n));
                if (!strcmp(n, base)) dir_idx = dir_files->len - 1;
            }
        g_dir_close(d);
    }
    g_free(dir); g_free(base);
}

static gboolean load_path(const char *path) {
    GError *err = NULL;
    GdkPixbuf *p = gdk_pixbuf_new_from_file(path, &err);
    if (!p) {
        g_printerr("qyview: %s\n", err ? err->message : "load failed");
        if (err) g_error_free(err);
        return FALSE;
    }
    if (pix) g_object_unref(pix);
    pix = p;
    g_free(current_path);
    current_path = g_strdup(path);
    zoom = 1.0; pan_x = pan_y = 0;
    update_zoom();
    /* 底部显示图片尺寸/格式/大小 */
    if (image_info) {
        int iw = gdk_pixbuf_get_width(pix);
        int ih = gdk_pixbuf_get_height(pix);
        const char *ext = strrchr(path, '.');
        const char *fmt = ext ? ext + 1 : "?";
        struct stat st;
        long sz = 0;
        if (stat(path, &st) == 0) sz = st.st_size;
        gchar *info = g_strdup_printf("%d×%d %s · %ld KB",
                                      iw, ih, fmt, sz / 1024);
        gtk_label_set_text(GTK_LABEL(image_info), info);
        g_free(info);
    }
    return TRUE;
}

static void load_by_index(GtkWidget *da, int idx) {
    if (idx < 0 || idx >= (int)dir_files->len) return;
    gchar *full = g_build_filename(cur_dir, (gchar*)dir_files->pdata[idx], NULL);
    if (load_path(full)) {
        dir_idx = idx;
        /* 窗口标题: 文件名 (n/N) */
        if (g_win) {
            gchar *ttl = g_strdup_printf("%s (%d/%d)",
                                          (gchar *)dir_files->pdata[idx],
                                          dir_idx + 1, dir_files->len);
            gtk_window_set_title(g_win, ttl);
            g_free(ttl);
        }
        if (page_label) {
            gchar buf[32];
            g_snprintf(buf, sizeof buf, "%d / %d", dir_idx + 1, dir_files->len);
            gtk_label_set_text(GTK_LABEL(page_label), buf);
        }
        if (file_label)
            gtk_label_set_text(GTK_LABEL(file_label), (gchar*)dir_files->pdata[idx]);
        gtk_widget_queue_draw(da);
    }
    g_free(full);
}

static void on_prev(GtkButton *b, gpointer ud) {
    if (!dir_files || dir_files->len == 0) return;
    int n = (dir_idx - 1 + (int)dir_files->len) % (int)dir_files->len;
    load_by_index(GTK_WIDGET(ud), n);
}

static void on_next(GtkButton *b, gpointer ud) {
    if (!dir_files || dir_files->len == 0) return;
    int n = (dir_idx + 1) % (int)dir_files->len;
    load_by_index(GTK_WIDGET(ud), n);
}

static void on_fit(GtkButton *b, gpointer ud) {
    zoom = 1.0; pan_x = pan_y = 0;
    update_zoom();
    gtk_widget_queue_draw(GTK_WIDGET(ud));
}

static void on_zoomin(GtkButton *b, gpointer ud) {
    (void)b;
    zoom *= 1.15;
    if (zoom > 20) zoom = 20;
    update_zoom();
    gtk_widget_queue_draw(GTK_WIDGET(ud));
}

static void on_zoomout(GtkButton *b, gpointer ud) {
    (void)b;
    zoom /= 1.15;
    if (zoom < 0.05) zoom = 0.05;
    update_zoom();
    gtk_widget_queue_draw(GTK_WIDGET(ud));
}

/* 顺时针旋转 90°（QYVIEW_ROTATE=1 可在启动时自动旋转一次，便于自动化验证） */
static void on_rotate(GtkButton *b, gpointer ud) {
    (void)b;
    if (!pix) return;
    GdkPixbuf *r = gdk_pixbuf_rotate_simple(pix, GDK_PIXBUF_ROTATE_CLOCKWISE);
    if (r) {
        g_object_unref(pix);
        pix = r;
        zoom = 1.0; pan_x = pan_y = 0;
        update_zoom();
        gtk_widget_queue_draw(GTK_WIDGET(ud));
    }
}

static gboolean rotate_once(gpointer ud) {
    on_rotate(NULL, ud);
    return G_SOURCE_REMOVE;
}

/* 保存当前图片（格式由扩展名推断），结果提示到底部 */
static void on_save(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!pix || !current_path) return;
    const char *ext = strrchr(current_path, '.');
    const char *type = NULL;
    if (ext) {
        if (!g_ascii_strcasecmp(ext, ".jpg") || !g_ascii_strcasecmp(ext, ".jpeg")) type = "jpeg";
        else if (!g_ascii_strcasecmp(ext, ".png")) type = "png";
        else if (!g_ascii_strcasecmp(ext, ".bmp")) type = "bmp";
        else if (!g_ascii_strcasecmp(ext, ".tiff") || !g_ascii_strcasecmp(ext, ".tif")) type = "tiff";
        else if (!g_ascii_strcasecmp(ext, ".webp")) type = "webp";
    }
    if (!type) {
        if (save_status) gtk_label_set_text(GTK_LABEL(save_status), TR("暂不支持保存该格式"));
        return;
    }
    GError *err = NULL;
    if (gdk_pixbuf_save(pix, current_path, type, &err, NULL)) {
        if (save_status) gtk_label_set_text(GTK_LABEL(save_status), TR("已保存"));
    } else {
        if (save_status) gtk_label_set_text(GTK_LABEL(save_status), TR("保存失败"));
        if (err) { g_printerr("qyview save: %s\n", err->message); g_error_free(err); }
    }
}

static gboolean save_once(gpointer ud) {
    on_save(NULL, ud);
    return G_SOURCE_REMOVE;
}

/* ---------- 幻灯片 ---------- */
static void set_play_icon(gboolean playing) {
    if (!play_btn) return;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(play_btn));
    if (child) gtk_widget_destroy(child);
    GdkPixbuf *pb = qy_icon_pixbuf(playing ? QY_ICON_PAUSE : QY_ICON_PLAY, 18, NULL);
    gtk_container_add(GTK_CONTAINER(play_btn), gtk_image_new_from_pixbuf(pb));
    g_object_unref(pb);
    gtk_widget_show_all(play_btn);
    gtk_widget_set_tooltip_text(play_btn, playing ? TR("暂停幻灯片 (空格)") : TR("播放幻灯片 (空格)"));
}

static gboolean slide_tick(gpointer ud) {
    if (!slide_on) return G_SOURCE_REMOVE;
    if (dir_files && dir_files->len > 1) {
        int n = (dir_idx + 1) % (int)dir_files->len;
        load_by_index(GTK_WIDGET(ud), n);
    }
    return G_SOURCE_CONTINUE;
}

static void toggle_slideshow(void) {
    if (!dir_files || dir_files->len < 2) return;
    slide_on = !slide_on;
    if (slide_on) {
        if (!slide_timer) slide_timer = g_timeout_add(3000, slide_tick, g_da);
        set_play_icon(TRUE);
    } else {
        if (slide_timer) { g_source_remove(slide_timer); slide_timer = 0; }
        set_play_icon(FALSE);
    }
}

static void on_play(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    toggle_slideshow();
}

/* ---------- F11 全屏切换 ---------- */
static void toggle_fullscreen(void) {
    if (!g_win) return;
    GdkWindow *gdkw = gtk_widget_get_window(GTK_WIDGET(g_win));
    gboolean fs = gdkw && (gdk_window_get_state(gdkw) & GDK_WINDOW_STATE_FULLSCREEN);
    if (fs)
        gtk_window_unfullscreen(g_win);
    else
        gtk_window_fullscreen(g_win);
}

static void on_fullscreen(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    toggle_fullscreen();
}

static gboolean on_draw(GtkWidget *da, cairo_t *cr, gpointer ud) {
    GtkAllocation a; gtk_widget_get_allocation(da, &a);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.12);
    cairo_paint(cr);
    if (!pix) return FALSE;
    int iw = gdk_pixbuf_get_width(pix), ih = gdk_pixbuf_get_height(pix);
    double fit = 1.0;
    /* 打开大图时自动缩小适应窗口（小图保持原始尺寸） */
    if (iw && ih) {
        double fx = (double)a.width / (double)iw;
        double fy = (double)a.height / (double)ih;
        fit = fx < fy ? fx : fy;
        if (fit > 1.0) fit = 1.0;
    }
    double z = zoom * fit;
    double dw = iw * z, dh = ih * z;
    /* 居中 + 平移 */
    double ox = (a.width - dw) / 2.0 + pan_x;
    double oy = (a.height - dh) / 2.0 + pan_y;
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.22);
    cairo_rectangle(cr, ox - 1, oy - 1, dw + 2, dh + 2);
    cairo_fill(cr);
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, z, z);
    gdk_cairo_set_source_pixbuf(cr, pix, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer ud) {
    if (ev->direction == GDK_SCROLL_UP) zoom *= 1.15;
    else if (ev->direction == GDK_SCROLL_DOWN) zoom /= 1.15;
    if (zoom < 0.05) zoom = 0.05;
    if (zoom > 20) zoom = 20;
    update_zoom();
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    /* F11 全屏切换 */
    if (ev->keyval == GDK_KEY_F11) {
        toggle_fullscreen();
        return TRUE;
    }
    /* 空格: 播放/暂停幻灯片 */
    if (ev->keyval == GDK_KEY_space) {
        toggle_slideshow();
        return TRUE;
    }
    if (!dir_files || dir_files->len == 0) return FALSE;
    if (ev->keyval == GDK_KEY_Right) {
        int n = (dir_idx + 1) % (int)dir_files->len;
        load_by_index(GTK_WIDGET(ud), n);
    } else if (ev->keyval == GDK_KEY_Left) {
        int n = (dir_idx - 1 + (int)dir_files->len) % (int)dir_files->len;
        load_by_index(GTK_WIDGET(ud), n);
    } else if (ev->keyval == GDK_KEY_0) {
        zoom = 1.0; pan_x = pan_y = 0; gtk_widget_queue_draw(w);
        update_zoom();
    } else if (ev->keyval == GDK_KEY_plus || ev->keyval == GDK_KEY_equal) {
        /* + 或 = 放大 */
        zoom *= 1.15; gtk_widget_queue_draw(w); update_zoom();
    } else if (ev->keyval == GDK_KEY_minus) {
        /* - 缩小 */
        zoom /= 1.15; gtk_widget_queue_draw(w); update_zoom();
    } else if (ev->keyval == GDK_KEY_Delete) {
        /* Delete: 当前图片移到回收站 */
        const char *path = g_ptr_array_index(dir_files, dir_idx);
        GFile *f = g_file_new_for_path(path);
        GError *err = NULL;
        if (g_file_trash(f, NULL, &err)) {
            g_printerr("QYVIEWDBG: trashed %s\n", path);
            g_ptr_array_remove_index(dir_files, dir_idx);
            if (dir_files->len > 0) {
                if (dir_idx >= (int)dir_files->len) dir_idx = 0;
                load_by_index(GTK_WIDGET(ud), dir_idx);
            } else {
                zoom = 1.0; pan_x = pan_y = 0; gtk_widget_queue_draw(w);
            }
        } else {
            g_printerr("QYVIEWDBG: trash fail %s\n", err ? err->message : "?");
            g_clear_error(&err);
        }
    }
    return FALSE;
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    /* 双击还原 */
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        zoom = 1.0; pan_x = pan_y = 0; gtk_widget_queue_draw(w);
    }
    return FALSE;
}

static void activate(GtkApplication *app, gpointer ud) {
    qy_load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    g_win = GTK_WINDOW(win);
    gtk_window_set_title(GTK_WINDOW(win), "澜岫图片查看器");
    qy_window_setup(GTK_WINDOW(win), 700, 500, 480, 360, FALSE);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(win), overlay);

    /* 图片绘制区 */
    GtkWidget *da = gtk_drawing_area_new();
    qy_add_class(da, "qy-app-surface");
    g_da = da;
    gtk_container_add(GTK_CONTAINER(overlay), da);
    g_signal_connect(da, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(da, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(da, "key-press-event", G_CALLBACK(on_key), da);
    g_signal_connect(da, "button-press-event", G_CALLBACK(on_button), NULL);
    gtk_widget_add_events(da, GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(da, TRUE);

    /* 底部悬浮工具条 (GtkOverlay 子层) */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    qy_add_class(bar, "qy-view-bar");
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(bar, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(bar, 10);
    gtk_widget_set_margin_top(bar, 6);

    GtkWidget *b_prev = qy_icon_button(QY_ICON_PREV, 18, TR("上一张"), "qy-view-bar-btn");
    play_btn = qy_icon_button(QY_ICON_PLAY, 18, TR("播放幻灯片 (空格)"), "qy-view-bar-btn");
    GtkWidget *b_next = qy_icon_button(QY_ICON_NEXT, 18, TR("下一张"), "qy-view-bar-btn");
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *b_zoomout = gtk_button_new_with_label("−");
    GtkWidget *b_zoomin = gtk_button_new_with_label("+");
    qy_add_class(b_zoomout, "qy-view-bar-btn");
    qy_add_class(b_zoomin, "qy-view-bar-btn");
    GtkWidget *b_fit = qy_icon_button(QY_ICON_FIT, 18, TR("适应窗口"), "qy-view-bar-btn");
    GtkWidget *b_rot = qy_icon_button(QY_ICON_ROTATE, 18, TR("旋转"), "qy-view-bar-btn");
    GtkWidget *b_save = qy_icon_button(QY_ICON_SAVE, 18, TR("保存"), "qy-view-bar-btn");
    GtkWidget *b_full = qy_icon_button(QY_ICON_MAX, 18, TR("全屏 (F11)"), "qy-view-bar-btn");

    zoom_label = gtk_label_new("");
    qy_add_class(zoom_label, "qy-view-bar-label");
    save_status = gtk_label_new("");
    qy_add_class(save_status, "qy-view-bar-label");
    image_info = gtk_label_new("");
    qy_add_class(image_info, "qy-view-bar-label");

    gtk_box_pack_start(GTK_BOX(bar), b_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), play_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), sep1, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(bar), b_zoomout, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_zoomin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_fit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_rot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), b_full, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), zoom_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), save_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), image_info, FALSE, FALSE, 0);

    g_signal_connect(b_prev, "clicked", G_CALLBACK(on_prev), da);
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play), NULL);
    g_signal_connect(b_next, "clicked", G_CALLBACK(on_next), da);
    g_signal_connect(b_zoomout, "clicked", G_CALLBACK(on_zoomout), da);
    g_signal_connect(b_zoomin, "clicked", G_CALLBACK(on_zoomin), da);
    g_signal_connect(b_fit, "clicked", G_CALLBACK(on_fit), da);
    g_signal_connect(b_rot, "clicked", G_CALLBACK(on_rotate), da);
    g_signal_connect(b_save, "clicked", G_CALLBACK(on_save), da);
    g_signal_connect(b_full, "clicked", G_CALLBACK(on_fullscreen), NULL);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar);

    /* 左下角文件名 + 右下角页码 (悬浮子层) */
    file_label = gtk_label_new("");
    qy_add_class(file_label, "qy-view-filename");
    gtk_widget_set_halign(file_label, GTK_ALIGN_START);
    gtk_widget_set_valign(file_label, GTK_ALIGN_END);
    gtk_widget_set_margin_start(file_label, 14);
    gtk_widget_set_margin_bottom(file_label, 14);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), file_label);

    page_label = gtk_label_new("1 / 1");
    qy_add_class(page_label, "qy-view-page");
    gtk_widget_set_halign(page_label, GTK_ALIGN_END);
    gtk_widget_set_valign(page_label, GTK_ALIGN_END);
    gtk_widget_set_margin_end(page_label, 14);
    gtk_widget_set_margin_bottom(page_label, 14);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), page_label);

    /* ---------- 快捷键 ---------- */
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), accel);
    gtk_widget_add_accelerator(b_prev, "clicked", accel, GDK_KEY_Page_Up, 0, GTK_ACCEL_VISIBLE);     /* PgUp 上一张 */
    gtk_widget_add_accelerator(b_next, "clicked", accel, GDK_KEY_Page_Down, 0, GTK_ACCEL_VISIBLE);   /* PgDn 下一张 */
    gtk_widget_add_accelerator(b_rot, "clicked", accel, GDK_KEY_r, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+R 旋转 */
    gtk_widget_add_accelerator(b_save, "clicked", accel, GDK_KEY_s, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+S 另存 */
    g_printerr("QYVIEWDBG: accel 4 keys\n");

    gtk_widget_show_all(win);

    gchar **args = (gchar**)ud;
    if (args && args[0]) {
        if (load_path(args[0])) {
            scan_dir(args[0]);
            g_free(cur_dir);
            cur_dir = g_path_get_dirname(args[0]);
            load_by_index(da, dir_idx);   /* 刷新页码与标题 */
        }
    }
    /* 自动化验证: QYVIEW_ROTATE=1 启动后自动顺时针旋转一次 */
    if (getenv("QYVIEW_ROTATE")) {
        g_timeout_add(300, rotate_once, da);
    }
    /* 自动化验证: QYVIEW_SAVE=1 启动后自动保存当前图片 */
    if (getenv("QYVIEW_SAVE")) {
        g_timeout_add(900, save_once, da);
    }
    gtk_widget_grab_focus(da);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.view", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), argv + 1);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}