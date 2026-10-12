/* qycolor - 澜岫取色器 (GTK3)
 * 窗口 360x280，深色主题：#qy-color-preview 大色块 + #qy-color-hex 值。
 * 三组 R/G/B 滑杆实时更新；随机颜色 / 复制 HEX；X11 支持屏幕取色，
 * Wayland 环境提示不支持。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GtkWidget *win        = NULL;
static GtkWidget *preview    = NULL;
static GtkWidget *hex_label = NULL;
static GtkWidget *r_scale   = NULL;
static GtkWidget *g_scale  = NULL;
static GtkWidget *b_scale  = NULL;
static GtkCssProvider *preview_css = NULL;
static char cur_hex[16] = "#E95420";

static void update_color(void) {
    int r = (int)gtk_range_get_value(GTK_RANGE(r_scale));
    int g = (int)gtk_range_get_value(GTK_RANGE(g_scale));
    int b = (int)gtk_range_get_value(GTK_RANGE(b_scale));
    g_snprintf(cur_hex, sizeof cur_hex, "#%02X%02X%02X", r, g, b);
    gtk_label_set_text(GTK_LABEL(hex_label), cur_hex);
    if (preview_css) {
        gchar *css = g_strdup_printf("#qy-color-preview { background-color: %s; }", cur_hex);
        gtk_css_provider_load_from_data(preview_css, css, -1, NULL);
        g_free(css);
    }
    if (preview) gtk_widget_queue_draw(preview);
    g_printerr("QYCOLORDBG: hex=%s rgb=%d,%d,%d\n", cur_hex, r, g, b);
}

static gboolean preview_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    GdkRGBA c;
    gdk_rgba_parse(&c, cur_hex);
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_rectangle(cr, 0, 0, al.width, al.height);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
    cairo_rectangle(cr, 0.5, 0.5, al.width - 1, al.height - 1);
    cairo_stroke(cr);
    return FALSE;
}

static void on_scale_changed(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    update_color();
}

static void on_random_color(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    gtk_range_set_value(GTK_RANGE(r_scale), g_random_int_range(0, 256));
    gtk_range_set_value(GTK_RANGE(g_scale), g_random_int_range(0, 256));
    gtk_range_set_value(GTK_RANGE(b_scale), g_random_int_range(0, 256));
}

static void on_copy_hex(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, cur_hex, -1);
    g_printerr("QYCOLORDBG: copied %s\n", cur_hex);
}

/* X11：读取光标所在像素；Wayland：提示不支持 */
static void on_pick_screen(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
#ifdef GDK_WINDOWING_X11
    GdkDisplay *display = gdk_display_get_default();
    if (display && GDK_IS_X11_DISPLAY(display)) {
        GdkScreen *screen = gdk_display_get_default_screen(display);
        GdkWindow *root = gdk_screen_get_root_window(screen);
        gint x = 0, y = 0;
        GdkDeviceManager *dm = gdk_display_get_device_manager(display);
        GdkDevice *dev = dm ? gdk_device_manager_get_client_pointer(dm) : NULL;
        if (dev) gdk_device_get_position(dev, &screen, &x, &y);
        GdkPixbuf *pb = root ? gdk_pixbuf_get_from_window(root, x, y, 1, 1) : NULL;
        if (pb) {
            guchar *px = gdk_pixbuf_get_pixels(pb);
            gtk_range_set_value(GTK_RANGE(r_scale), px[0]);
            gtk_range_set_value(GTK_RANGE(g_scale), px[1]);
            gtk_range_set_value(GTK_RANGE(b_scale), px[2]);
            g_object_unref(pb);
            return;
        }
    }
#endif
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
        TR("Wayland 环境暂不支持屏幕取色"));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(win, "qy-color-win");
    gtk_window_set_title(GTK_WINDOW(win), TR("取色器"));
    qy_make_titlebar(GTK_WINDOW(win), TR("取色器"));
    qy_window_setup(GTK_WINDOW(win), 360, 280, 320, 260, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶部大色块 */
    preview = gtk_drawing_area_new();
    gtk_widget_set_name(preview, "qy-color-preview");
    gtk_widget_set_size_request(preview, -1, 64);
    g_signal_connect(preview, "draw", G_CALLBACK(preview_draw_cb), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), preview, FALSE, FALSE, 0);

    /* 预览色块的 CSS provider（随滑杆实时更新），须在滑杆初值前创建 */
    preview_css = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(preview),
                                   GTK_STYLE_PROVIDER(preview_css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* HEX 值（等宽大字） */
    hex_label = gtk_label_new(cur_hex);
    gtk_widget_set_name(hex_label, "qy-color-hex");
    gtk_label_set_selectable(GTK_LABEL(hex_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), hex_label, FALSE, FALSE, 0);

    /* R/G/B 三组滑杆 */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    const char *names[3] = { "R", "G", "B" };
    int initial[3] = { 0xE9, 0x54, 0x20 };
    GtkWidget *scales[3];
    for (int i = 0; i < 3; i++) {
        GtkWidget *lb = gtk_label_new(names[i]);
        qy_add_class(lb, "qy-color-scale");
        gtk_widget_set_halign(lb, GTK_ALIGN_START);
        scales[i] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
        gtk_range_set_value(GTK_RANGE(scales[i]), initial[i]);
        gtk_widget_set_size_request(scales[i], 240, -1);
        g_signal_connect(scales[i], "value-changed", G_CALLBACK(on_scale_changed), NULL);
        gtk_grid_attach(GTK_GRID(grid), lb, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), scales[i], 1, i, 1, 1);
    }
    r_scale = scales[0];
    g_scale = scales[1];
    b_scale = scales[2];
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    /* 按钮行：随机颜色 / 屏幕取色 / 复制 HEX */
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hb, GTK_ALIGN_CENTER);
    GtkWidget *b_random = gtk_button_new_with_label(TR("随机颜色"));
    GtkWidget *b_pick   = gtk_button_new_with_label(TR("屏幕取色"));
    GtkWidget *b_copy   = gtk_button_new_with_label(TR("复制 HEX"));
    qy_add_class(b_random, "qy-color-btn");
    qy_add_class(b_pick, "qy-color-btn");
    qy_add_class(b_copy, "qy-color-btn");
    g_signal_connect(b_random, "clicked", G_CALLBACK(on_random_color), NULL);
    g_signal_connect(b_pick, "clicked", G_CALLBACK(on_pick_screen), NULL);
    g_signal_connect(b_copy, "clicked", G_CALLBACK(on_copy_hex), NULL);
    gtk_box_pack_start(GTK_BOX(hb), b_random, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_pick, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    update_color();

    gtk_main();
    return 0;
}