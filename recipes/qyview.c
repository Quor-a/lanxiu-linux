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
static GtkWidget *slide_spin = NULL;    /* 幻灯片间隔微调（release 270） */
static int slide_ms = 3000;             /* 幻灯片间隔毫秒 */
static GtkWidget *file_label = NULL;   /* 左下角文件名 */
static GtkWidget *g_da = NULL;          /* 绘图区（幻灯片定时器回调用） */
static guint slide_timer = 0;           /* 幻灯片定时器 id */
static gboolean slide_on = FALSE;      /* 幻灯片是否运行 */

/* ---------- 幻灯片淡入淡出（GtkStack 双绘图区交替） ---------- */
static GtkWidget *g_stack = NULL;     /* GtkStack（CROSSFADE 过渡） */
static GtkWidget *g_view_a = NULL;     /* 子绘图区 A */
static GtkWidget *g_view_b = NULL;    /* 子绘图区 B */
static int g_view_cur = 0;             /* 当前可见子区索引（0=A, 1=B） */

/* 设置某个子绘图区要显示的 pixbuf（持有引用，替换时释放旧图） */
static void view_set_pixbuf(GtkWidget *view, GdkPixbuf *pb) {
    if (pb) g_object_ref(pb);
    g_object_set_data_full(G_OBJECT(view), "qy-view-pixbuf", pb,
                           (GDestroyNotify)g_object_unref);
    gtk_widget_queue_draw(view);
}

/* 取子绘图区当前要绘制的 pixbuf（无数据时返回 NULL，绘制黑色背景） */
static GdkPixbuf *view_get_pixbuf(GtkWidget *view) {
    return g_object_get_data(G_OBJECT(view), "qy-view-pixbuf");
}

/* 重绘两个子绘图区（缩放/平移变化时保证可见区刷新） */
static void queue_all_views(void) {
    if (g_view_a) gtk_widget_queue_draw(g_view_a);
    if (g_view_b) gtk_widget_queue_draw(g_view_b);
}

/* 另存为 / 批量转换 */
static GtkWidget *save_fmt_combo = NULL; /* 另存为目标格式下拉（PNG/JPEG/BMP） */

/* 右侧属性/EXIF 侧栏（定义在 load_path 之后，先声明原型） */
static void update_info_sidebar(void);

/* ---------- 右侧属性/EXIF 侧栏 ---------- */
static GtkWidget *info_revealer = NULL; /* GtkRevealer 容器 */
static GtkWidget *info_name = NULL;     /* 文件名 */
static GtkWidget *info_dim  = NULL;    /* 尺寸(像素) */
static GtkWidget *info_size = NULL;    /* 文件大小 */
static GtkWidget *info_exif = NULL;   /* EXIF 摘要 */

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
    update_info_sidebar();
    return TRUE;
}

/* ---------- 右侧属性/EXIF 侧栏 ---------- */
static void format_size(char *buf, size_t n, gint64 sz) {
    if (sz < 1024)
        g_snprintf(buf, n, "%ld B", (long)sz);
    else if (sz < 1024 * 1024)
        g_snprintf(buf, n, "%.1f KB", sz / 1024.0);
    else
        g_snprintf(buf, n, "%.2f MB", sz / (1024.0 * 1024.0));
}

static void update_info_sidebar(void) {
    if (!info_name || !pix || !current_path) return;

    gchar *base = g_path_get_basename(current_path);
    gtk_label_set_text(GTK_LABEL(info_name), base);
    g_free(base);

    int iw = gdk_pixbuf_get_width(pix), ih = gdk_pixbuf_get_height(pix);
    gchar *dim = g_strdup_printf("%d × %d px", iw, ih);
    gtk_label_set_text(GTK_LABEL(info_dim), dim);
    g_free(dim);

    struct stat st;
    if (stat(current_path, &st) == 0) {
        char buf[48];
        format_size(buf, sizeof buf, (gint64)st.st_size);
        gtk_label_set_text(GTK_LABEL(info_size), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(info_size), "—");
    }

    const char *make = gdk_pixbuf_get_option(pix, "exif-ifd0-Make");
    const char *model = gdk_pixbuf_get_option(pix, "exif-ifd0-Model");
    const char *dt    = gdk_pixbuf_get_option(pix, "exif-photo-DateTimeOriginal");
    if (make || model || dt) {
        GString *s = g_string_new(NULL);
        if (make)  g_string_append_printf(s, "%s: %s\n", TR("厂商"), make);
        if (model) g_string_append_printf(s, "%s: %s\n", TR("型号"), model);
        if (dt)    g_string_append_printf(s, "%s: %s", TR("拍摄时间"), dt);
        gtk_label_set_text(GTK_LABEL(info_exif), s->str);
        g_string_free(s, TRUE);
    } else {
        gtk_label_set_text(GTK_LABEL(info_exif), TR("无 EXIF 数据"));
    }
}

static void toggle_info_sidebar(void) {
    if (!info_revealer) return;
    gboolean show = !gtk_revealer_get_reveal_child(GTK_REVEALER(info_revealer));
    if (show) update_info_sidebar();
    gtk_revealer_set_reveal_child(GTK_REVEALER(info_revealer), show);
}

static void on_info(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    toggle_info_sidebar();
}

static void load_by_index(GtkWidget *da, int idx) {
    if (idx < 0 || idx >= (int)dir_files->len) return;
    gchar *full = g_build_filename(cur_dir, (gchar*)dir_files->pdata[idx], NULL);
    if (load_path(full)) {
        dir_idx = idx;
        /* 幻灯片淡入淡出：把新图放到当前隐藏的子绘图区，再切换可见子区触发 CROSSFADE */
        if (g_stack && g_view_a && g_view_b) {
            GtkWidget *next = (g_view_cur == 0) ? g_view_b : g_view_a;
            view_set_pixbuf(next, pix);
            gtk_stack_set_visible_child(GTK_STACK(g_stack), next);
            g_view_cur = 1 - g_view_cur;
        }
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
        queue_all_views();
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
    (void)b; (void)ud;
    zoom = 1.0; pan_x = pan_y = 0;
    update_zoom();
    queue_all_views();
}

static void on_zoomin(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    zoom *= 1.15;
    if (zoom > 20) zoom = 20;
    update_zoom();
    queue_all_views();
}

static void on_zoomout(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    zoom /= 1.15;
    if (zoom < 0.05) zoom = 0.05;
    update_zoom();
    queue_all_views();
}

/* 顺时针旋转 90°（QYVIEW_ROTATE=1 可在启动时自动旋转一次，便于自动化验证） */
static void on_rotate(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!pix) return;
    GdkPixbuf *r = gdk_pixbuf_rotate_simple(pix, GDK_PIXBUF_ROTATE_CLOCKWISE);
    if (r) {
        g_object_unref(pix);
        pix = r;
        zoom = 1.0; pan_x = pan_y = 0;
        update_zoom();
        /* 两个子绘图区都同步到旋转后的图 */
        if (g_view_a) view_set_pixbuf(g_view_a, pix);
        if (g_view_b) view_set_pixbuf(g_view_b, pix);
        queue_all_views();
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

/* ---------- 另存为其它格式 ---------- */
/* 返回当前下拉框选择的格式 id：png / jpeg / bmp */
static const char *save_fmt_id(void) {
    if (!save_fmt_combo) return "png";
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(save_fmt_combo));
    return (id && id[0]) ? id : "png";
}

/* 另存为：GtkFileChooserNative(ACTION_SAVE) + 格式由窗口内下拉决定 */
static void on_save_as(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!pix || !current_path) return;
    const char *fmt = save_fmt_id();
    const char *ext = (!strcmp(fmt, "jpeg")) ? "jpg" : fmt;
    gchar *base = g_path_get_basename(current_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    gchar *suggest = g_strdup_printf("%s.%s", base, ext);

    GtkFileChooserNative *dlg = gtk_file_chooser_native_new(TR("另存为"), g_win,
                                                 GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 TR("保存"), TR("取消"));
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
    if (res == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        GError *err = NULL;
        if (gdk_pixbuf_save(pix, path, fmt, &err, NULL)) {
            if (save_status) {
                gchar *s = g_strdup_printf("%s %s", TR("已另存为"), path);
                gtk_label_set_text(GTK_LABEL(save_status), s);
                g_free(s);
            }
        } else {
            if (save_status) gtk_label_set_text(GTK_LABEL(save_status), TR("另存失败"));
            if (err) { g_printerr("qyview save-as: %s\n", err->message); g_clear_error(&err); }
        }
        g_free(path);
    }
    g_object_unref(dlg);
    g_free(suggest);
    g_free(base);
}

/* ---------- 批量转换 ---------- */
/* 批量转换目标扩展名 */
static gboolean is_batch_img(const char *name) {
    static const char *BATCH_EXT[] = {".png", ".jpg", ".jpeg", ".bmp", ".webp", NULL};
    gchar *low = g_ascii_strdown(name, -1);
    gboolean ok = FALSE;
    for (int i = 0; BATCH_EXT[i]; i++)
        if (g_str_has_suffix(low, BATCH_EXT[i])) { ok = TRUE; break; }
    g_free(low);
    return ok;
}

/* 批量转换对话框的 UI 状态（工作线程通过 g_idle_add 更新主线程控件） */
typedef struct {
    gchar *src;              /* 源目录 */
    gchar *dst;              /* 目标目录 */
    gchar *fmt;              /* 目标格式 id */
    gchar *ext;              /* 目标扩展名 */
    GtkWidget *dlg;           /* 批量转换对话框 */
    GtkWidget *combo;         /* 目标格式下拉 */
    GtkWidget *bar;           /* GtkProgressBar */
    GtkWidget *status;        /* 状态标签 */
    GThread *thread;         /* 工作线程 */
    int total;
    int done;
    gboolean finished;
    gboolean thread_started;
    gboolean thread_done;
} BatchUi;

/* 工作线程回调：在主线程更新进度/完成状态 */
static gboolean batch_update_idle(gpointer p) {
    BatchUi *ui = (BatchUi *)p;
    if (ui->bar && ui->total > 0)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->bar),
                                      (double)ui->done / (double)ui->total);
    if (ui->status) {
        if (ui->finished)
            gtk_label_set_text(GTK_LABEL(ui->status), TR("批量转换完成"));
        else {
            gchar *s = g_strdup_printf("%s %d/%d", TR("转换中"), ui->done, ui->total);
            gtk_label_set_text(GTK_LABEL(ui->status), s);
            g_free(s);
        }
    }
    if (ui->finished && !ui->thread_done) {
        ui->thread_done = TRUE;
        if (ui->thread) g_thread_join(ui->thread);
        GtkWidget *close = gtk_dialog_get_widget_for_response(GTK_DIALOG(ui->dlg),
                                                                GTK_RESPONSE_CLOSE);
        if (close) gtk_widget_set_sensitive(close, TRUE);
        gtk_widget_set_sensitive(ui->combo, TRUE);
    }
    return ui->finished ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

/* 工作线程：遍历源目录图片，转换到目标目录 */
static gpointer batch_worker(gpointer p) {
    BatchUi *ui = (BatchUi *)p;
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(ui->src, 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name(dir)))
            if (is_batch_img(n)) g_ptr_array_add(files, g_strdup(n));
        g_dir_close(dir);
    }
    ui->total = (int)files->len;
    for (guint i = 0; i < files->len; i++) {
        const char *name = (const char *)files->pdata[i];
        gchar *srcp = g_build_filename(ui->src, name, NULL);
        GError *err = NULL;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(srcp, &err);
        if (pb) {
            gchar *noext = g_strdup(name);
            char *dot = strrchr(noext, '.');
            if (dot) *dot = 0;
            gchar *outname = g_strdup_printf("%s.%s", noext, ui->ext);
            gchar *dstp = g_build_filename(ui->dst, outname, NULL);
            if (!gdk_pixbuf_save(pb, dstp, ui->fmt, &err, NULL)) {
                if (err) {
                    g_printerr("qyview batch: %s -> %s: %s\n", name, outname, err->message);
                    g_clear_error(&err);
                }
            }
            g_free(noext);
            g_free(outname);
            g_free(dstp);
            g_object_unref(pb);
        } else {
            if (err) {
                g_printerr("qyview batch: %s: %s\n", name, err->message);
                g_clear_error(&err);
            }
        }
        g_free(srcp);
        ui->done = (int)i + 1;
        g_idle_add(batch_update_idle, ui);
    }
    g_ptr_array_free(files, TRUE);
    ui->finished = TRUE;
    g_idle_add(batch_update_idle, ui);
    return NULL;
}

/* 运行中禁止用窗口关闭按钮关掉批量转换对话框 */
static gboolean on_batch_delete(GtkWidget *w, GdkEventAny *ev, gpointer p) {
    (void)w; (void)ev;
    BatchUi *ui = (BatchUi *)p;
    if (ui->thread_started && !ui->thread_done) return TRUE;
    return FALSE;
}

static void on_batch_response(GtkDialog *dlg, gint resp, gpointer p) {
    BatchUi *ui = (BatchUi *)p;
    if (resp == GTK_RESPONSE_ACCEPT) {
        if (ui->thread_started) return;
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->combo));
        if (!id || !id[0]) id = "png";
        ui->fmt = g_strdup(id);
        ui->ext = g_strdup((!strcmp(id, "jpeg")) ? "jpg" : id);
        gtk_widget_set_sensitive(ui->combo, FALSE);
        GtkWidget *close = gtk_dialog_get_widget_for_response(dlg, GTK_RESPONSE_CLOSE);
        if (close) gtk_widget_set_sensitive(close, FALSE);
        if (ui->bar) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->bar), 0.0);
        if (ui->status) gtk_label_set_text(GTK_LABEL(ui->status), TR("转换中…"));
        ui->thread_started = TRUE;
        ui->thread = g_thread_new("qyview-batch", batch_worker, ui);
    } else if (resp == GTK_RESPONSE_CLOSE) {
        gtk_widget_destroy(GTK_WIDGET(dlg));
    }
}

static void on_batch_destroy(GtkWidget *w, gpointer p) {
    (void)w;
    BatchUi *ui = (BatchUi *)p;
    if (ui->thread_started && !ui->thread_done && ui->thread)
        g_thread_join(ui->thread);
    g_free(ui->src);
    g_free(ui->dst);
    g_free(ui->fmt);
    g_free(ui->ext);
    g_free(ui);
}

/* 选择源/目标目录后弹出批量转换对话框 */
static void show_batch_dialog(const char *src, const char *dst) {
    BatchUi *ui = g_new0(BatchUi, 1);
    ui->src = g_strdup(src);
    ui->dst = g_strdup(dst);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(TR("批量转换"), g_win, GTK_DIALOG_MODAL,
                                                 TR("开始转换"), GTK_RESPONSE_ACCEPT,
                                                 TR("关闭"), GTK_RESPONSE_CLOSE, NULL);
    ui->dlg = dlg;
    gtk_window_set_default_size(GTK_WINDOW(dlg), 360, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget *frow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *fl = gtk_label_new(TR("目标格式"));
    ui->combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->combo), "png", "PNG");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->combo), "jpeg", "JPEG");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->combo), "bmp", "BMP");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->combo), 0);
    gtk_box_pack_start(GTK_BOX(frow), fl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(frow), ui->combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), frow, FALSE, FALSE, 0);

    ui->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui->bar), FALSE);
    gtk_box_pack_start(GTK_BOX(box), ui->bar, FALSE, FALSE, 0);

    ui->status = gtk_label_new(TR("选择格式后点击开始转换"));
    gtk_label_set_xalign(GTK_LABEL(ui->status), 0.0);
    gtk_box_pack_start(GTK_BOX(box), ui->status, FALSE, FALSE, 0);

    gtk_widget_show_all(content);
    g_signal_connect(dlg, "response", G_CALLBACK(on_batch_response), ui);
    g_signal_connect(dlg, "delete-event", G_CALLBACK(on_batch_delete), ui);
    g_signal_connect(dlg, "destroy", G_CALLBACK(on_batch_destroy), ui);
    gtk_widget_show(dlg);
}

/* 批量转换入口：依次选择源目录、目标目录，再弹对话框选格式并执行 */
static void on_batch_convert(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkFileChooserNative *dlg = gtk_file_chooser_native_new(TR("选择源目录"), g_win,
                                                 GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                 TR("选择"), TR("取消"));
    gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
    gchar *src = (res == GTK_RESPONSE_ACCEPT) ?
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg)) : NULL;
    g_object_unref(dlg);
    if (!src) return;

    dlg = gtk_file_chooser_native_new(TR("选择目标目录"), g_win,
                                      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                      TR("选择"), TR("取消"));
    res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
    gchar *dst = (res == GTK_RESPONSE_ACCEPT) ?
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg)) : NULL;
    g_object_unref(dlg);
    if (!dst) { g_free(src); return; }

    show_batch_dialog(src, dst);
    g_free(src);
    g_free(dst);
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

static void on_slide_interval(GtkSpinButton *sp, gpointer ud) {
    (void)ud;
    slide_ms = (int)(gtk_spin_button_get_value(sp) * 1000);
    if (slide_ms < 1000) slide_ms = 1000;
    /* 记忆间隔（release 281） */
    gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *cfg = g_build_filename(dir, "qyview-slide.conf", NULL);
    gchar *txt = g_strdup_printf("%d\n", slide_ms / 1000);
    g_file_set_contents(cfg, txt, -1, NULL);
    g_free(txt); g_free(cfg); g_free(dir);
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
        if (!slide_timer) slide_timer = g_timeout_add(slide_ms, slide_tick, g_da);
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
    GdkPixbuf *pb = view_get_pixbuf(da);
    if (!pb) return FALSE;
    int iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
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
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->direction == GDK_SCROLL_UP) zoom *= 1.15;
    else if (ev->direction == GDK_SCROLL_DOWN) zoom /= 1.15;
    if (zoom < 0.05) zoom = 0.05;
    if (zoom > 20) zoom = 20;
    update_zoom();
    queue_all_views();
    return TRUE;
}

static void on_open_dialog(void);   /* 前向声明（Ctrl+O 打开图片） */
static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    /* F11 全屏切换 */
    if (ev->keyval == GDK_KEY_F11) {
        toggle_fullscreen();
        return TRUE;
    }
    /* ESC: 全屏时退出全屏，否则关闭窗口（release 278） */
    if (ev->keyval == GDK_KEY_Escape) {
        if (g_win) {
            GdkWindow *gdkw = gtk_widget_get_window(GTK_WIDGET(g_win));
            if (gdkw && (gdk_window_get_state(gdkw) & GDK_WINDOW_STATE_FULLSCREEN)) {
                gtk_window_unfullscreen(g_win);
                return TRUE;
            }
            gtk_window_close(g_win);
        }
        return TRUE;
    }
    /* 空格: 播放/暂停幻灯片 */
    if (ev->keyval == GDK_KEY_space) {
        toggle_slideshow();
        return TRUE;
    }
    /* Ctrl+O: 打开图片（release 281） */
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_o || ev->keyval == GDK_KEY_O)) {
        on_open_dialog();
        return TRUE;
    }
    /* Ctrl+S: 保存当前图片（release 293） */
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_s || ev->keyval == GDK_KEY_S)) {
        on_save(NULL, NULL);
        return TRUE;
    }
    /* i: 切换右侧属性/EXIF 侧栏 */
    if (ev->keyval == GDK_KEY_i || ev->keyval == GDK_KEY_I) {
        toggle_info_sidebar();
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
        zoom = 1.0; pan_x = pan_y = 0;
        queue_all_views();
        update_zoom();
    } else if (ev->keyval == GDK_KEY_plus || ev->keyval == GDK_KEY_equal) {
        /* + 或 = 放大 */
        zoom *= 1.15; queue_all_views(); update_zoom();
    } else if (ev->keyval == GDK_KEY_minus) {
        /* - 缩小 */
        zoom /= 1.15; queue_all_views(); update_zoom();
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
                zoom = 1.0; pan_x = pan_y = 0;
                queue_all_views();
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
        zoom = 1.0; pan_x = pan_y = 0;
        queue_all_views();
    }
    return FALSE;
}

/* Ctrl+O: 选择图片打开（release 281） */
static void on_open_dialog(void) {
    GtkWidget *dlg = gtk_file_chooser_native_new(TR("打开图片"), GTK_WINDOW(g_win),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN, TR("打开"), TR("取消"));
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, TR("图片文件"));
    gtk_file_filter_add_mime_type(f, "image/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            scan_dir(path);
            for (guint i = 0; i < dir_files->len; i++) {
                if (strcmp((const char *)g_ptr_array_index(dir_files, i), path) == 0) {
                    load_by_index(GTK_WIDGET(g_da), (int)i);
                    break;
                }
            }
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

/* 拖放图片文件到窗口打开（release 281） */
static void on_win_drag_recv(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                             GtkSelectionData *data, guint info, guint t, gpointer ud) {
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)t; (void)ud;
    gchar **uris = gtk_selection_data_get_uris(data);
    if (uris && uris[0]) {
        gchar *path = g_filename_from_uri(uris[0], NULL, NULL);
        if (path) {
            scan_dir(path);
            for (guint i = 0; i < dir_files->len; i++) {
                if (strcmp((const char *)g_ptr_array_index(dir_files, i), path) == 0) {
                    load_by_index(GTK_WIDGET(g_da), (int)i);
                    break;
                }
            }
            g_free(path);
        }
    }
    g_strfreev(uris);
}

static void activate(GtkApplication *app, gpointer ud) {
    qy_load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    g_win = GTK_WINDOW(win);
    gtk_window_set_title(GTK_WINDOW(win), "澜岫图片查看器");
    qy_window_setup(GTK_WINDOW(win), 700, 500, 480, 360, FALSE);
    /* 拖放图片文件到窗口打开（release 281） */
    {
        GtkTargetEntry te = { (gchar *)"text/uri-list", 0, 0 };
        gtk_drag_dest_set(win, GTK_DEST_DEFAULT_ALL, &te, 1, GDK_ACTION_COPY);
        g_signal_connect(win, "drag-data-received", G_CALLBACK(on_win_drag_recv), NULL);
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶部工具条：另存为 / 格式下拉 / 批量转换 */
    GtkWidget *topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    qy_add_class(topbar, "qy-view-bar");
    gtk_widget_set_margin_start(topbar, 8);
    gtk_widget_set_margin_end(topbar, 8);
    gtk_widget_set_margin_top(topbar, 6);
    gtk_widget_set_margin_bottom(topbar, 4);

    GtkWidget *b_saveas = gtk_button_new_with_label(TR("另存为"));
    qy_add_class(b_saveas, "qy-view-bar-btn");
    g_signal_connect(b_saveas, "clicked", G_CALLBACK(on_save_as), NULL);

    save_fmt_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(save_fmt_combo), "png", "PNG");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(save_fmt_combo), "jpeg", "JPEG");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(save_fmt_combo), "bmp", "BMP");
    gtk_combo_box_set_active(GTK_COMBO_BOX(save_fmt_combo), 0);
    qy_add_class(save_fmt_combo, "qy-view-bar-btn");

    GtkWidget *b_batch = gtk_button_new_with_label(TR("批量转换"));
    qy_add_class(b_batch, "qy-view-bar-btn");
    g_signal_connect(b_batch, "clicked", G_CALLBACK(on_batch_convert), NULL);

    gtk_box_pack_start(GTK_BOX(topbar), b_saveas, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(topbar), save_fmt_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(topbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(topbar), b_batch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), topbar, FALSE, FALSE, 0);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), overlay, TRUE, TRUE, 0);

    /* 图片绘制区：GtkStack 双绘图区，幻灯片切换时 CROSSFADE 淡入淡出 */
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 300);
    gtk_stack_set_homogeneous(GTK_STACK(stack), TRUE);
    g_stack = stack;

    GtkWidget *da = gtk_drawing_area_new();
    qy_add_class(da, "qy-app-surface");
    g_view_a = da;
    GtkWidget *da2 = gtk_drawing_area_new();
    qy_add_class(da2, "qy-app-surface");
    g_view_b = da2;

    gtk_container_add(GTK_CONTAINER(stack), da);
    gtk_container_add(GTK_CONTAINER(stack), da2);
    gtk_stack_set_visible_child(GTK_STACK(stack), da);
    g_view_cur = 0;

    g_da = da;
    gtk_container_add(GTK_CONTAINER(overlay), stack);

    /* 两个子绘图区共用同一套事件处理 */
    g_signal_connect(da, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(da, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(da, "key-press-event", G_CALLBACK(on_key), da);
    g_signal_connect(da, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(da2, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(da2, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(da2, "key-press-event", G_CALLBACK(on_key), da);
    g_signal_connect(da2, "button-press-event", G_CALLBACK(on_button), NULL);
    gtk_widget_add_events(da, GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    gtk_widget_add_events(da2, GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(da, TRUE);
    gtk_widget_set_can_focus(da2, TRUE);

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
    /* 幻灯片间隔（release 270） */
    slide_spin = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(slide_spin), 3);
    /* 加载上次间隔（release 281） */
    {
        gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
        gchar *cfg = g_build_filename(dir, "qyview-slide.conf", NULL);
        gchar *contents = NULL;
        if (g_file_get_contents(cfg, &contents, NULL, NULL)) {
            int sec = atoi(contents);
            if (sec >= 1 && sec <= 10)
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(slide_spin), sec);
        }
        g_free(contents); g_free(cfg); g_free(dir);
    }
    qy_add_class(slide_spin, "qy-view-bar-btn");
    gtk_widget_set_tooltip_text(slide_spin, TR("幻灯片间隔（秒）"));
    g_signal_connect(slide_spin, "value-changed", G_CALLBACK(on_slide_interval), NULL);
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *b_zoomout = gtk_button_new_with_label("−");
    GtkWidget *b_zoomin = gtk_button_new_with_label("+");
    qy_add_class(b_zoomout, "qy-view-bar-btn");
    qy_add_class(b_zoomin, "qy-view-bar-btn");
    GtkWidget *b_fit = qy_icon_button(QY_ICON_FIT, 18, TR("适应窗口"), "qy-view-bar-btn");
    GtkWidget *b_rot = qy_icon_button(QY_ICON_ROTATE, 18, TR("旋转"), "qy-view-bar-btn");
    GtkWidget *b_save = qy_icon_button(QY_ICON_SAVE, 18, TR("保存"), "qy-view-bar-btn");
    GtkWidget *b_info = qy_icon_button(QY_ICON_LIST, 18, TR("信息 (i)"), "qy-view-bar-btn");
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
    gtk_box_pack_start(GTK_BOX(bar), b_info, FALSE, FALSE, 0);
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
    g_signal_connect(b_info, "clicked", G_CALLBACK(on_info), NULL);
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

    /* ---------- 右侧属性/EXIF 侧栏 ---------- */
    info_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(info_revealer),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(info_revealer), 180);
    gtk_revealer_set_reveal_child(GTK_REVEALER(info_revealer), FALSE);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(panel, "qy-view-info-panel");
    gtk_widget_set_size_request(panel, 220, -1);
    gtk_container_set_border_width(GTK_CONTAINER(panel), 14);

    GtkWidget *title = gtk_label_new(TR("信息"));
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    qy_add_class(title, "qy-view-info-title");
    gtk_box_pack_start(GTK_BOX(panel), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    GtkWidget *cap, *val;
    cap = gtk_label_new(TR("文件名"));
    gtk_widget_set_halign(cap, GTK_ALIGN_START);
    qy_add_class(cap, "qy-view-info-cap");
    gtk_box_pack_start(GTK_BOX(panel), cap, FALSE, FALSE, 0);
    val = gtk_label_new("—");
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(val), 0);
    gtk_label_set_line_wrap(GTK_LABEL(val), TRUE);
    qy_add_class(val, "qy-view-info-val");
    gtk_box_pack_start(GTK_BOX(panel), val, FALSE, FALSE, 0);
    info_name = val;

    cap = gtk_label_new(TR("尺寸"));
    gtk_widget_set_halign(cap, GTK_ALIGN_START);
    qy_add_class(cap, "qy-view-info-cap");
    gtk_box_pack_start(GTK_BOX(panel), cap, FALSE, FALSE, 0);
    val = gtk_label_new("—");
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(val), 0);
    qy_add_class(val, "qy-view-info-val");
    gtk_box_pack_start(GTK_BOX(panel), val, FALSE, FALSE, 0);
    info_dim = val;

    cap = gtk_label_new(TR("文件大小"));
    gtk_widget_set_halign(cap, GTK_ALIGN_START);
    qy_add_class(cap, "qy-view-info-cap");
    gtk_box_pack_start(GTK_BOX(panel), cap, FALSE, FALSE, 0);
    val = gtk_label_new("—");
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(val), 0);
    qy_add_class(val, "qy-view-info-val");
    gtk_box_pack_start(GTK_BOX(panel), val, FALSE, FALSE, 0);
    info_size = val;

    cap = gtk_label_new("EXIF");
    gtk_widget_set_halign(cap, GTK_ALIGN_START);
    qy_add_class(cap, "qy-view-info-cap");
    gtk_box_pack_start(GTK_BOX(panel), cap, FALSE, FALSE, 0);
    val = gtk_label_new("—");
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(val), 0);
    gtk_label_set_line_wrap(GTK_LABEL(val), TRUE);
    qy_add_class(val, "qy-view-info-val");
    gtk_box_pack_start(GTK_BOX(panel), val, FALSE, FALSE, 0);
    info_exif = val;

    gtk_container_add(GTK_CONTAINER(info_revealer), panel);
    gtk_box_pack_start(GTK_BOX(hbox), info_revealer, FALSE, FALSE, 0);

    /* ---------- 快捷键 ---------- */
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), accel);
    gtk_widget_add_accelerator(b_prev, "clicked", accel, GDK_KEY_Page_Up, 0, GTK_ACCEL_VISIBLE);     /* PgUp 上一张 */
    gtk_widget_add_accelerator(b_next, "clicked", accel, GDK_KEY_Page_Down, 0, GTK_ACCEL_VISIBLE);   /* PgDn 下一张 */
    gtk_widget_add_accelerator(b_rot, "clicked", accel, GDK_KEY_r, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+R 旋转 */
    gtk_widget_add_accelerator(b_save, "clicked", accel, GDK_KEY_s, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); /* Ctrl+S 另存 */
    gtk_widget_add_accelerator(b_info, "clicked", accel, GDK_KEY_i, 0, GTK_ACCEL_VISIBLE);            /* i 信息侧栏 */
    g_printerr("QYVIEWDBG: accel 5 keys\n");

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