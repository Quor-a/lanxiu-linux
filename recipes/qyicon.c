/*
 * qyicon.c — 澜岫统一 cairo 图标层
 * 风格与托盘线稿一致：24×24 逻辑坐标、1.5px 描边、圆帽圆角。
 * 供 qydesktop / qyappmenu 使用，替代「彩色块+字符」。
 */
#include "qyicon.h"
#include <string.h>

/* ---------- 通用圆角矩形 ---------- */
static void qy_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* ---------- 各图标绘制（24×24 逻辑坐标，中心 12,12） ---------- */

#define PX(v) (x + (v) * s / 24.0)
#define PY(v) (y + (v) * s / 24.0)
#define SET_LINE(cr_) \
    do { \
        double _lw = s / 16.0; \
        cairo_set_line_width((cr_), _lw); \
        cairo_set_line_cap((cr_), CAIRO_LINE_CAP_ROUND); \
        cairo_set_line_join((cr_), CAIRO_LINE_JOIN_ROUND); \
    } while (0)

static void draw_term(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(5), PY(5.5), PX(14), PY(13), PX(3));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9), PY(10.5)); cairo_line_to(cr, PX(12), PY(13)); cairo_line_to(cr, PX(9), PY(15.5));
    cairo_move_to(cr, PX(13.5), PY(15.5)); cairo_line_to(cr, PX(17.5), PY(15.5));
    cairo_stroke(cr);
}

static void draw_browser(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(12), PY(12), PX(7.2), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(4.8), PY(12)); cairo_line_to(cr, PX(19.2), PY(12));
    cairo_move_to(cr, PX(12), PY(4.8)); cairo_line_to(cr, PX(12), PY(19.2));
    cairo_stroke(cr);
}

static void draw_files(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6), PY(4.5), PX(12.5), PY(15), PX(2.2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(15), PY(4.5)); cairo_line_to(cr, PX(18.5), PY(8));
    cairo_move_to(cr, PX(15), PY(8)); cairo_line_to(cr, PX(18.5), PY(8));
    cairo_stroke(cr);
}

static void draw_image(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(4.8), PY(6), PX(14.4), PY(12), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(7), PY(15.5)); cairo_line_to(cr, PX(10.5), PY(10.5));
    cairo_line_to(cr, PX(13), PY(13.5)); cairo_line_to(cr, PX(15.5), PY(11.5));
    cairo_line_to(cr, PX(18.5), PY(15.5));
    cairo_stroke(cr);
    cairo_arc(cr, PX(9.5), PY(9), PX(1.4), 0, 2 * G_PI);
    cairo_stroke(cr);
}

static void draw_archive(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6), PY(5), PX(12), PY(14), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10.2), PY(5)); cairo_line_to(cr, PX(10.2), PY(9));
    cairo_move_to(cr, PX(13.8), PY(5)); cairo_line_to(cr, PX(13.8), PY(9));
    cairo_move_to(cr, PX(10.2), PY(9)); cairo_line_to(cr, PX(13.8), PY(9));
    cairo_stroke(cr);
}

static void draw_trash(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(7.5), PY(8.5), PX(9), PY(10.5), PX(1.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(5.5), PY(7.5)); cairo_line_to(cr, PX(18.5), PY(7.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10), PY(7.5)); cairo_curve_to(cr, PX(10), PY(5.5), PX(14), PY(5.5), PX(14), PY(7.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10.5), PY(11)); cairo_line_to(cr, PX(10.5), PY(16.5));
    cairo_move_to(cr, PX(13.5), PY(11)); cairo_line_to(cr, PX(13.5), PY(16.5));
    cairo_stroke(cr);
}

static void draw_edit(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(5.5), PY(18.5)); cairo_line_to(cr, PX(18.5), PY(5.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(15), PY(5.5)); cairo_line_to(cr, PX(18.5), PY(9));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(5), PY(19)); cairo_line_to(cr, PX(7.5), PY(17.5));
    cairo_stroke(cr);
}

static void draw_store(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(7), PY(9), PX(10), PY(9), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9.5), PY(9)); cairo_curve_to(cr, PX(9.5), PY(6.5), PX(14.5), PY(6.5), PX(14.5), PY(9));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(7), PY(12)); cairo_line_to(cr, PX(17), PY(12));
    cairo_stroke(cr);
}

static void draw_shot(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(4.5), PY(8), PX(15), PY(9.5), PX(2.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(8.5), PY(8)); cairo_curve_to(cr, PX(8.5), PY(6.5), PX(9.5), PY(5.8), PX(12), PY(5.8));
    cairo_curve_to(cr, PX(14.5), PY(5.8), PX(15.5), PY(6.5), PX(15.5), PY(8));
    cairo_stroke(cr);
    cairo_arc(cr, PX(12), PY(12.8), PX(2.8), 0, 2 * G_PI);
    cairo_stroke(cr);
}

static void draw_clipboard(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(7), PY(5.5), PX(10), PY(13.5), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10), PY(5.5)); cairo_curve_to(cr, PX(10), PY(3.5), PX(14), PY(3.5), PX(14), PY(5.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9.5), PY(10.5)); cairo_line_to(cr, PX(14.5), PY(10.5));
    cairo_move_to(cr, PX(9.5), PY(13.5)); cairo_line_to(cr, PX(14.5), PY(13.5));
    cairo_stroke(cr);
}

static void draw_lock(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(9), PY(11)); cairo_curve_to(cr, PX(9), PY(7), PX(15), PY(7), PX(15), PY(11));
    cairo_stroke(cr);
    qy_rounded_rect(cr, PX(6.5), PY(10.5), PX(11), PY(8.5), PX(2));
    cairo_stroke(cr);
    cairo_arc(cr, PX(12), PY(14), PX(1.2), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(12), PY(15.2)); cairo_line_to(cr, PX(12), PY(17));
    cairo_stroke(cr);
}

static void draw_search(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(10), PY(10.5), PX(5.2), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(14.2), PY(14.2)); cairo_line_to(cr, PX(19.5), PY(19.5));
    cairo_stroke(cr);
}

static void draw_music(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(10.5), PY(5.5)); cairo_line_to(cr, PX(10.5), PY(16.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10.5), PY(5.5)); cairo_curve_to(cr, PX(14), PY(6.5), PX(15.5), PY(9.5), PX(15), PY(12.5));
    cairo_stroke(cr);
    cairo_arc(cr, PX(10.5), PY(17.5), PX(1.8), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_arc(cr, PX(14.5), PY(15.5), PX(1.8), 0, 2 * G_PI);
    cairo_stroke(cr);
}

static void draw_switcher(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(13.5), PY(8.5)); cairo_line_to(cr, PX(9.5), PY(12)); cairo_line_to(cr, PX(13.5), PY(15.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10.5), PY(8.5)); cairo_line_to(cr, PX(14.5), PY(12)); cairo_line_to(cr, PX(10.5), PY(15.5));
    cairo_stroke(cr);
}

static void draw_driver(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(5), PY(8.5), PX(14), PY(7.5), PX(1.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(5), PY(15)); cairo_curve_to(cr, PX(5), PY(16.8), PX(19), PY(16.8), PX(19), PY(15));
    cairo_stroke(cr);
    cairo_arc(cr, PX(12), PY(12), PX(2.6), 0, 2 * G_PI);
    cairo_stroke(cr);
}

static void draw_git(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(7.5), PY(6.5), PX(2), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_arc(cr, PX(16.5), PY(6.5), PX(2), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_arc(cr, PX(12), PY(17.5), PX(2), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(7.5), PY(8.5)); cairo_line_to(cr, PX(7.5), PY(14));
    cairo_curve_to(cr, PX(7.5), PY(16.5), PX(12), PY(16.5), PX(12), PY(15.5));
    cairo_move_to(cr, PX(16.5), PY(8.5)); cairo_line_to(cr, PX(16.5), PY(12));
    cairo_curve_to(cr, PX(16.5), PY(14), PX(12), PY(14.5), PX(12), PY(15.5));
    cairo_stroke(cr);
}

static void draw_welcome(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(12), PY(4.5));
    cairo_line_to(cr, PX(13.6), PY(10.4));
    cairo_line_to(cr, PX(19.5), PY(12));
    cairo_line_to(cr, PX(13.6), PY(13.6));
    cairo_line_to(cr, PX(12), PY(19.5));
    cairo_line_to(cr, PX(10.4), PY(13.6));
    cairo_line_to(cr, PX(4.5), PY(12));
    cairo_line_to(cr, PX(10.4), PY(10.4));
    cairo_close_path(cr);
    cairo_stroke(cr);
}

static void draw_home(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(5.5), PY(11.5)); cairo_line_to(cr, PX(12), PY(5.5)); cairo_line_to(cr, PX(18.5), PY(11.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(6.5), PY(10.5)); cairo_line_to(cr, PX(6.5), PY(18.5));
    cairo_line_to(cr, PX(17.5), PY(18.5)); cairo_line_to(cr, PX(17.5), PY(10.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10), PY(18.5)); cairo_line_to(cr, PX(10), PY(14));
    cairo_line_to(cr, PX(14), PY(14)); cairo_line_to(cr, PX(14), PY(18.5));
    cairo_stroke(cr);
}

static void draw_settings(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(12), PY(12), PX(4.5), 0, 2 * G_PI);
    cairo_stroke(cr);
    /* 8 个齿（固定 24 网格坐标，避免 math.h） */
    const double teeth[8][4] = {
        { 18.2, 12.0, 20.6, 12.0 },
        { 16.4, 16.4, 18.1, 18.1 },
        { 12.0, 18.2, 12.0, 20.6 },
        { 7.6, 16.4, 5.9, 18.1 },
        { 5.8, 12.0, 3.4, 12.0 },
        { 7.6, 7.6, 5.9, 5.9 },
        { 12.0, 5.8, 12.0, 3.4 },
        { 16.4, 7.6, 18.1, 5.9 },
    };
    int i;
    for (i = 0; i < 8; i++) {
        cairo_move_to(cr, PX(teeth[i][0]), PY(teeth[i][1]));
        cairo_line_to(cr, PX(teeth[i][2]), PY(teeth[i][3]));
    }
    cairo_stroke(cr);
}

static void draw_monitor(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(5), PY(6), PX(14), PY(10), PX(1.8));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(8.5), PY(16.5)); cairo_line_to(cr, PX(15.5), PY(16.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10), PY(16.5)); cairo_line_to(cr, PX(10), PY(19.5));
    cairo_move_to(cr, PX(14), PY(16.5)); cairo_line_to(cr, PX(14), PY(19.5));
    cairo_move_to(cr, PX(8.5), PY(19.5)); cairo_line_to(cr, PX(15.5), PY(19.5));
    cairo_stroke(cr);
}

static void draw_setup(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(12), PY(5)); cairo_line_to(cr, PX(12), PY(13.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(8), PY(10.5)); cairo_line_to(cr, PX(12), PY(15)); cairo_line_to(cr, PX(16), PY(10.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(6.5), PY(18)); cairo_line_to(cr, PX(17.5), PY(18));
    cairo_stroke(cr);
}

static void draw_users(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(12), PY(8), PX(3.5), 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(6.5), PY(19)); cairo_curve_to(cr, PX(7.5), PY(15), PX(16.5), PY(15), PX(17.5), PY(19));
    cairo_stroke(cr);
}

static void draw_network(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(6), PY(12)); cairo_line_to(cr, PX(18), PY(12));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(15), PY(9)); cairo_line_to(cr, PX(18), PY(12)); cairo_line_to(cr, PX(15), PY(15));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9), PY(9)); cairo_line_to(cr, PX(6), PY(12)); cairo_line_to(cr, PX(9), PY(15));
    cairo_stroke(cr);
    cairo_arc(cr, PX(12), PY(12), PX(1.2), 0, 2 * G_PI);
    cairo_stroke(cr);
}

static void draw_video(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(4.8), PY(6.5), PX(14.4), PY(11), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10.5), PY(9.5)); cairo_line_to(cr, PX(15), PY(12)); cairo_line_to(cr, PX(10.5), PY(14.5));
    cairo_close_path(cr);
    cairo_stroke(cr);
}

static void draw_doc(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6.5), PY(4.5), PX(11), PY(15), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9), PY(9)); cairo_line_to(cr, PX(15), PY(9));
    cairo_move_to(cr, PX(9), PY(12)); cairo_line_to(cr, PX(15), PY(12));
    cairo_move_to(cr, PX(9), PY(15)); cairo_line_to(cr, PX(13), PY(15));
    cairo_stroke(cr);
}

static void draw_file(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6.5), PY(4.5), PX(11), PY(15), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(14.5), PY(4.5)); cairo_line_to(cr, PX(17.5), PY(7.5));
    cairo_line_to(cr, PX(14.5), PY(7.5));
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9.5), PY(11.5)); cairo_line_to(cr, PX(15), PY(11.5));
    cairo_stroke(cr);
}

static void draw_back(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(16.5), PY(12)); cairo_line_to(cr, PX(7.5), PY(12));
    cairo_move_to(cr, PX(10.5), PY(8.5)); cairo_line_to(cr, PX(6), PY(12)); cairo_line_to(cr, PX(10.5), PY(15.5));
    cairo_stroke(cr);
}
static void draw_forward(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(7.5), PY(12)); cairo_line_to(cr, PX(16.5), PY(12));
    cairo_move_to(cr, PX(13.5), PY(8.5)); cairo_line_to(cr, PX(18), PY(12)); cairo_line_to(cr, PX(13.5), PY(15.5));
    cairo_stroke(cr);
}
static void draw_up(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6), PY(9), PX(12), PY(9), PX(1.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(9), PY(9)); cairo_line_to(cr, PX(9), PY(7.5));
    cairo_line_to(cr, PX(13.5), PY(7.5)); cairo_line_to(cr, PX(13.5), PY(9));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(12), PY(16)); cairo_line_to(cr, PX(12), PY(11.5));
    cairo_move_to(cr, PX(9.5), PY(14)); cairo_line_to(cr, PX(12), PY(11.5)); cairo_line_to(cr, PX(14.5), PY(14));
    cairo_stroke(cr);
}
static void draw_refresh(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(12), PY(12), PX(6.4), 5.31, 4.11 + 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(10.8), PY(5.0)); cairo_line_to(cr, PX(8.3), PY(8.8));
    cairo_move_to(cr, PX(10.8), PY(5.0)); cairo_line_to(cr, PX(6.4), PY(6.0));
    cairo_stroke(cr);
}
static void draw_copy(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6), PY(7), PX(10), PY(12), PX(2));
    cairo_stroke(cr);
    qy_rounded_rect(cr, PX(9), PY(5), PX(10), PY(12), PX(2));
    cairo_stroke(cr);
}
static void draw_cut(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(6.5), PY(6.5), PX(2), 0, 2 * G_PI);
    cairo_arc(cr, PX(6.5), PY(17.5), PX(2), 0, 2 * G_PI);
    cairo_move_to(cr, PX(8.5), PY(5.5)); cairo_line_to(cr, PX(18), PY(14.5));
    cairo_move_to(cr, PX(8.5), PY(18.5)); cairo_line_to(cr, PX(18), PY(9.5));
    cairo_stroke(cr);
}
static void draw_close(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(6), PY(6)); cairo_line_to(cr, PX(18), PY(18));
    cairo_move_to(cr, PX(18), PY(6)); cairo_line_to(cr, PX(6), PY(18));
    cairo_stroke(cr);
}
static void draw_min(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(6.5), PY(12)); cairo_line_to(cr, PX(17.5), PY(12));
    cairo_stroke(cr);
}
static void draw_max(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6.5), PY(6.5), PX(11), PY(11), PX(2));
    cairo_stroke(cr);
}
static void draw_plus(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(12), PY(6)); cairo_line_to(cr, PX(12), PY(18));
    cairo_move_to(cr, PX(6), PY(12)); cairo_line_to(cr, PX(18), PY(12));
    cairo_stroke(cr);
}
static void draw_volume(cairo_t *cr, double x, double y, double s) {
    cairo_move_to(cr, PX(6), PY(9.5));
    cairo_line_to(cr, PX(10), PY(9.5));
    cairo_line_to(cr, PX(15), PY(5.5));
    cairo_line_to(cr, PX(15), PY(18.5));
    cairo_line_to(cr, PX(10), PY(14.5));
    cairo_line_to(cr, PX(6), PY(14.5));
    cairo_close_path(cr); cairo_stroke(cr);
    cairo_arc(cr, PX(17), PY(12), PX(3.5), -0.9, 0.9); cairo_stroke(cr);
    cairo_arc(cr, PX(18.8), PY(12), PX(6.2), -0.9, 0.9); cairo_stroke(cr);
}
static void draw_power(cairo_t *cr, double x, double y, double s) {
    cairo_arc(cr, PX(12), PY(12.8), PX(5.2), G_PI * 0.2, G_PI * 1.8); cairo_stroke(cr);
    cairo_move_to(cr, PX(12), PY(5)); cairo_line_to(cr, PX(12), PY(10.5)); cairo_stroke(cr);
}
static void draw_grid(cairo_t *cr, double x, double y, double s) {
    qy_rounded_rect(cr, PX(6), PY(6), PX(5.2), PY(5.2), PX(1.2));
    qy_rounded_rect(cr, PX(12.8), PY(6), PX(5.2), PY(5.2), PX(1.2));
    qy_rounded_rect(cr, PX(6), PY(12.8), PX(5.2), PY(5.2), PX(1.2));
    qy_rounded_rect(cr, PX(12.8), PY(12.8), PX(5.2), PY(5.2), PX(1.2));
    cairo_stroke(cr);
}

static void draw_play(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(9), PY(7.5)); cairo_line_to(cr, PX(9), PY(16.5));
    cairo_line_to(cr, PX(16.5), PY(12)); cairo_close_path(cr);
    cairo_stroke(cr);
}

static void draw_pause(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(9.5), PY(7.5)); cairo_line_to(cr, PX(9.5), PY(16.5));
    cairo_move_to(cr, PX(14.5), PY(7.5)); cairo_line_to(cr, PX(14.5), PY(16.5));
    cairo_stroke(cr);
}

static void draw_stop(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    qy_rounded_rect(cr, PX(8), PY(7.5), PX(8), PY(9), PX(1.8));
    cairo_stroke(cr);
}

static void draw_prev(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(15.5), PY(7.5)); cairo_line_to(cr, PX(15.5), PY(16.5));
    cairo_move_to(cr, PX(9), PY(12)); cairo_line_to(cr, PX(14), PY(8.5)); cairo_line_to(cr, PX(14), PY(15.5));
    cairo_stroke(cr);
}

static void draw_next(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(8.5), PY(7.5)); cairo_line_to(cr, PX(8.5), PY(16.5));
    cairo_move_to(cr, PX(15), PY(12)); cairo_line_to(cr, PX(10), PY(8.5)); cairo_line_to(cr, PX(10), PY(15.5));
    cairo_stroke(cr);
}

static void draw_bookmark(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(8), PY(5.5)); cairo_line_to(cr, PX(16), PY(5.5));
    cairo_line_to(cr, PX(16), PY(18.5)); cairo_line_to(cr, PX(12), PY(15.5));
    cairo_line_to(cr, PX(8), PY(18.5)); cairo_close_path(cr);
    cairo_stroke(cr);
}

static void draw_list(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(10), PY(6.5)); cairo_line_to(cr, PX(18), PY(6.5));
    cairo_move_to(cr, PX(10), PY(12)); cairo_line_to(cr, PX(18), PY(12));
    cairo_move_to(cr, PX(10), PY(17.5)); cairo_line_to(cr, PX(18), PY(17.5));
    cairo_arc(cr, PX(7), PY(6.5), PX(1.2), 0, 2*G_PI);
    cairo_arc(cr, PX(7), PY(12), PX(1.2), 0, 2*G_PI);
    cairo_arc(cr, PX(7), PY(17.5), PX(1.2), 0, 2*G_PI);
    cairo_stroke(cr);
}

static void draw_calc(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    qy_rounded_rect(cr, PX(6), PY(4.5), PX(12), PY(15), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(8.5), PY(8.5)); cairo_line_to(cr, PX(15.5), PY(8.5));
    cairo_arc(cr, PX(10), PY(11.5), PX(0.9), 0, 2*G_PI);
    cairo_arc(cr, PX(14), PY(11.5), PX(0.9), 0, 2*G_PI);
    cairo_arc(cr, PX(10), PY(15.5), PX(0.9), 0, 2*G_PI);
    cairo_arc(cr, PX(14), PY(15.5), PX(0.9), 0, 2*G_PI);
    cairo_stroke(cr);
}

static void draw_save(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    qy_rounded_rect(cr, PX(6), PY(5.5), PX(12), PY(13), PX(2));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(7.5), PY(5.5)); cairo_line_to(cr, PX(7.5), PY(10));
    cairo_line_to(cr, PX(16.5), PY(10)); cairo_line_to(cr, PX(16.5), PY(18.5));
    cairo_stroke(cr);
}

static void draw_rotate(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    qy_rounded_rect(cr, PX(8.5), PY(8.5), PX(7), PY(7), PX(1.5));
    cairo_stroke(cr);
    cairo_arc(cr, PX(12), PY(12), PX(6.6), -0.35, 1.45);
    cairo_stroke(cr);
    cairo_move_to(cr, PX(18), PY(6.5)); cairo_line_to(cr, PX(18), PY(9.5));
    cairo_line_to(cr, PX(15), PY(9.5));
    cairo_stroke(cr);
}

static void draw_fit(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_move_to(cr, PX(8), PY(6.5)); cairo_line_to(cr, PX(6.5), PY(6.5)); cairo_line_to(cr, PX(6.5), PY(8));
    cairo_move_to(cr, PX(16), PY(6.5)); cairo_line_to(cr, PX(17.5), PY(6.5)); cairo_line_to(cr, PX(17.5), PY(8));
    cairo_move_to(cr, PX(8), PY(17.5)); cairo_line_to(cr, PX(6.5), PY(17.5)); cairo_line_to(cr, PX(6.5), PY(16));
    cairo_move_to(cr, PX(16), PY(17.5)); cairo_line_to(cr, PX(17.5), PY(17.5)); cairo_line_to(cr, PX(17.5), PY(16));
    qy_rounded_rect(cr, PX(10.8), PY(10.8), PX(2.4), PY(2.4), PX(0.4));
    cairo_stroke(cr);
}

static void draw_open(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    qy_rounded_rect(cr, PX(5.5), PY(10), PX(13), PY(8.5), PX(1.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(8), PY(10)); cairo_line_to(cr, PX(8), PY(7.5));
    cairo_line_to(cr, PX(13), PY(7.5)); cairo_line_to(cr, PX(14.5), PY(10));
    cairo_stroke(cr);
}

static void draw_clock(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    cairo_arc(cr, PX(12), PY(12), PX(7.5), 0, 2 * G_PI);
    cairo_move_to(cr, PX(12), PY(12)); cairo_line_to(cr, PX(12), PY(7.5));
    cairo_move_to(cr, PX(12), PY(12)); cairo_line_to(cr, PX(15), PY(13));
    cairo_stroke(cr);
}

static void draw_keyboard(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    qy_rounded_rect(cr, PX(5), PY(7.5), PX(14), PY(9), PX(1.5));
    cairo_stroke(cr);
    cairo_move_to(cr, PX(7.5), PY(10.5)); cairo_line_to(cr, PX(16.5), PY(10.5));
    cairo_move_to(cr, PX(8.5), PY(13.5)); cairo_line_to(cr, PX(15.5), PY(13.5));
    cairo_stroke(cr);
}

static void draw_weather(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    /* 太阳（左上） */
    cairo_arc(cr, PX(8.5), PY(8.5), PX(2.4), 0, 2 * G_PI);
    cairo_stroke(cr);
    /* 六条光芒（固定角度，避免引入 math.h/-lm） */
    static const struct { double ix, iy, ox, oy; } rays[6] = {
        { 12.40, 8.50, 14.00, 8.50 },
        { 10.45, 11.88, 11.25, 13.26 },
        { 6.55, 11.88, 5.75, 13.26 },
        { 4.60, 8.50, 3.00, 8.50 },
        { 6.55, 5.12, 5.75, 4.24 },
        { 10.45, 5.12, 11.25, 4.24 },
    };
    for (int i = 0; i < 6; i++) {
        cairo_move_to(cr, PX(rays[i].ix), PY(rays[i].iy));
        cairo_line_to(cr, PX(rays[i].ox), PY(rays[i].oy));
    }
    cairo_stroke(cr);
    /* 云朵（右下） */
    cairo_move_to(cr, PX(10.2), PY(17.2));
    cairo_line_to(cr, PX(18.0), PY(17.2));
    cairo_arc(cr, PX(16.0), PY(14.6), PX(2.9), G_PI * 0.5, G_PI * 1.5);
    cairo_arc(cr, PX(13.2), PY(13.8), PX(3.1), G_PI * 0.3, G_PI * 1.2);
    cairo_line_to(cr, PX(10.2), PY(17.2));
    cairo_stroke(cr);
}

static void draw_note(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    /* 页面 */
    qy_rounded_rect(cr, PX(5.5), PY(4.5), PX(13), PY(15), PX(1.5));
    cairo_stroke(cr);
    /* 右上折角 */
    cairo_move_to(cr, PX(15.0), PY(4.5)); cairo_line_to(cr, PX(15.0), PY(8.0));
    cairo_line_to(cr, PX(18.5), PY(8.0));
    cairo_stroke(cr);
    /* 正文行 */
    cairo_move_to(cr, PX(8.0), PY(11.0)); cairo_line_to(cr, PX(16.0), PY(11.0));
    cairo_move_to(cr, PX(8.0), PY(14.5)); cairo_line_to(cr, PX(14.0), PY(14.5));
    cairo_stroke(cr);
}

static void draw_drive(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    /* 硬盘外壳 */
    qy_rounded_rect(cr, PX(4.5), PY(7.0), PX(15), PY(10), PX(2));
    cairo_stroke(cr);
    /* 磁盘主轴圆点 */
    cairo_arc(cr, PX(8.5), PY(12), PX(1.4), 0, 2 * G_PI);
    cairo_stroke(cr);
    /* 读写臂槽线 */
    cairo_move_to(cr, PX(11.5), PY(12)); cairo_line_to(cr, PX(17.5), PY(12));
    cairo_stroke(cr);
    /* 顶部状态灯 */
    cairo_arc(cr, PX(17.0), PY(8.8), PX(0.9), 0, 2 * G_PI);
    cairo_fill(cr);
}

static void draw_color(cairo_t *cr, double x, double y, double s) {
    SET_LINE(cr);
    /* 颜料滴轮廓 */
    cairo_move_to(cr, PX(12), PY(4.5));
    cairo_curve_to(cr, PX(7.0), PY(10), PX(6.0), PY(14), PX(6.0), PY(15.5));
    cairo_curve_to(cr, PX(6.0), PY(19), PX(8.5), PY(21), PX(12), PY(21));
    cairo_curve_to(cr, PX(15.5), PY(21), PX(18.0), PY(19), PX(18.0), PY(15.5));
    cairo_curve_to(cr, PX(18.0), PY(14), PX(17.0), PY(10), PX(12), PY(4.5));
    cairo_close_path(cr);
    cairo_stroke(cr);
    /* 中央色点 */
    cairo_arc(cr, PX(12), PY(15.5), PX(2.0), 0, 2 * G_PI);
    cairo_stroke(cr);
}

#undef SET_LINE

/* ---------- 函数表 ---------- */
static void (* const drawers[QY_ICON_COUNT])(cairo_t *, double, double, double) = {
    draw_term, draw_browser, draw_files, draw_image, draw_archive,
    draw_trash, draw_edit, draw_store, draw_shot, draw_clipboard,
    draw_lock, draw_search, draw_music, draw_switcher, draw_driver,
    draw_git, draw_welcome, draw_home, draw_settings, draw_monitor,
    draw_setup, draw_users, draw_network,
    draw_video, draw_doc, draw_file,
    draw_back, draw_forward, draw_up, draw_refresh,
    draw_trash, draw_copy, draw_cut, draw_clipboard, /* DELETE/PASTE 复用 */
    draw_close, draw_min, draw_max, draw_plus,
    draw_volume, draw_power,
    draw_grid,
    draw_play, draw_pause, draw_stop,
    draw_prev, draw_next, draw_bookmark,
    draw_list, draw_calc, draw_save,
    draw_rotate, draw_fit, draw_open,
    draw_clock, draw_keyboard,
    draw_weather, draw_note, draw_drive,
    draw_color,
};

/* ---------- 统一分发器 ---------- */
void qy_icon_draw(cairo_t *cr, QyIconId id, double x, double y, double size,
                          gboolean filled, const GdkRGBA *color) {
    (void)filled;
    if (id < 0 || id >= QY_ICON_COUNT) id = QY_ICON_SETTINGS;
    if (color) cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
    else cairo_set_source_rgb(cr, 1, 1, 1);
    double lw = size / 16.0;
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    drawers[id](cr, x, y, size);
}

/* ---------- 玻璃拟态圆角底 + 线稿 ---------- */
void qy_icon_tile(cairo_t *cr, QyIconId id, double x, double y, double size,
                  double radius, const GdkRGBA *bg, const GdkRGBA *color) {
    if (bg) cairo_set_source_rgba(cr, bg->red, bg->green, bg->blue, bg->alpha);
    else cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    qy_rounded_rect(cr, x, y, size, size, radius);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
    cairo_stroke(cr);
    qy_icon_draw(cr, id, x, y, size, FALSE, color);
}

/* ---------- Pixbuf 渲染（供 GtkImage/GtkIconView） ---------- */
GdkPixbuf *qy_icon_pixbuf(QyIconId id, int px, const GdkRGBA *bg) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    if (bg) cairo_set_source_rgba(cr, bg->red, bg->green, bg->blue, bg->alpha);
    else cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    double pad = px * 0.12;
    qy_icon_draw(cr, id, pad, pad, px - pad * 2, FALSE, NULL);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, px, px);
    cairo_surface_destroy(surf);
    return pb;
}

/* ---------- 应用名 → 图标映射 ---------- */
QyIconId qy_icon_for_app(const char *app) {
    if (!app) return QY_ICON_GRID;
    if (g_str_has_prefix(app, "qybrowser")) return QY_ICON_BROWSER;
    if (g_str_has_prefix(app, "qyfiles")) return QY_ICON_FILES;
    if (g_str_has_prefix(app, "qyterm") || g_str_has_prefix(app, "qyterminal") ||
        g_str_has_prefix(app, "weston-terminal")) return QY_ICON_TERM;
    if (g_str_has_prefix(app, "qyclock")) return QY_ICON_CLOCK;
    if (g_str_has_prefix(app, "qyweather")) return QY_ICON_WEATHER;
    if (g_str_has_prefix(app, "qynote")) return QY_ICON_NOTE;
    if (g_str_has_prefix(app, "qysettings")) return QY_ICON_SETTINGS;
    if (g_str_has_prefix(app, "qysearch")) return QY_ICON_SEARCH;
    if (g_str_has_prefix(app, "qycalc")) return QY_ICON_CALC;
    if (g_str_has_prefix(app, "qyshot")) return QY_ICON_SHOT;
    if (g_str_has_prefix(app, "qyrec")) return QY_ICON_VIDEO;
    if (g_str_has_prefix(app, "qyclip")) return QY_ICON_CLIPBOARD;
    if (g_str_has_prefix(app, "qylock")) return QY_ICON_LOCK;
    if (g_str_has_prefix(app, "qysetup")) return QY_ICON_SETUP;
    if (g_str_has_prefix(app, "qyedit")) return QY_ICON_EDIT;
    if (g_str_has_prefix(app, "qymon")) return QY_ICON_MONITOR;
    if (g_str_has_prefix(app, "qynet")) return QY_ICON_NETWORK;
    if (g_str_has_prefix(app, "qystore")) return QY_ICON_STORE;
    if (g_str_has_prefix(app, "qymedia") || g_str_has_prefix(app, "qymusic")) return QY_ICON_PLAY;
    if (g_str_has_prefix(app, "qyview")) return QY_ICON_IMAGE;
    if (g_str_has_prefix(app, "qywelcome")) return QY_ICON_GRID;
    if (g_str_has_prefix(app, "qyswitcher")) return QY_ICON_SWITCHER;
    if (g_str_has_prefix(app, "qyusers")) return QY_ICON_USERS;
    if (g_str_has_prefix(app, "qydriver")) return QY_ICON_DRIVER;
    if (g_str_has_prefix(app, "qygit")) return QY_ICON_FILES;
    if (g_str_has_prefix(app, "qyconvert")) return QY_ICON_CALC;
    if (g_str_has_prefix(app, "qyhelp")) return QY_ICON_DOC;
    if (g_str_has_prefix(app, "qydisk")) return QY_ICON_DRIVE;
    if (g_str_has_prefix(app, "qyaudio")) return QY_ICON_VOLUME;
    if (g_str_has_prefix(app, "qypower")) return QY_ICON_POWER;
    if (g_str_has_prefix(app, "qycolor")) return QY_ICON_COLOR;
    return QY_ICON_GRID;
}

/* ---------- 空状态插图 ---------- */
void qy_draw_empty_state(cairo_t *cr, double width, double height, const char *kind) {
    if (cr == NULL || width <= 0 || height <= 0) return;

    /* 深蓝底（圆角铺满） */
    double bg_r = (width < 40 || height < 40) ? 8.0 : 16.0;
    cairo_set_source_rgb(cr, 0.07, 0.10, 0.16);      /* 深蓝底 #121A29 */
    qy_rounded_rect(cr, 0, 0, width, height, bg_r);
    cairo_fill(cr);

    double cx = width / 2.0, cy = height / 2.0;
    double box_w = width * 0.54, box_h = height * 0.50;
    if (box_w < 40) box_w = 40;
    if (box_h < 32) box_h = 32;
    double bx = cx - box_w / 2.0;
    double by = cy - box_h / 2.0 + height * 0.02;

    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    /* 圆角方框（较亮蓝描边） */
    cairo_set_source_rgb(cr, 0.25, 0.33, 0.47);      /* 框线 #3F5478 */
    qy_rounded_rect(cr, bx, by, box_w, box_h, 14);
    cairo_stroke(cr);

    /* 内凹线：框内底部一条内凹平台线（左侧小竖线 + 横线，形成托盘感） */
    double tray_y = by + box_h * 0.80;
    cairo_move_to(cr, bx + box_w * 0.16, tray_y);
    cairo_line_to(cr, bx + box_w * 0.30, tray_y - box_h * 0.10);
    cairo_line_to(cr, bx + box_w * 0.70, tray_y - box_h * 0.10);
    cairo_line_to(cr, bx + box_w * 0.84, tray_y);
    cairo_stroke(cr);

    /* 中央小图形（橙色点缀） */
    cairo_set_source_rgb(cr, 1.0, 0.36, 0.10);       /* 主橙 #FF5C1A */
    double gs = box_w * 0.30;
    if (gs < 20) gs = 20;
    double gx = cx - gs / 2.0;
    double gy = by + box_h * 0.32 - gs / 2.0;

    if (kind && g_str_has_prefix(kind, "search")) {
        /* 放大镜 */
        cairo_arc(cr, gx + gs * 0.40, gy + gs * 0.40, gs * 0.26, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, gx + gs * 0.60, gy + gs * 0.60);
        cairo_line_to(cr, gx + gs * 0.88, gy + gs * 0.88);
        cairo_stroke(cr);
    } else if (kind && g_str_has_prefix(kind, "note")) {
        /* 笔记页 */
        qy_rounded_rect(cr, gx + gs * 0.14, gy + gs * 0.05, gs * 0.72, gs * 0.90, 3);
        cairo_stroke(cr);
        cairo_move_to(cr, gx + gs * 0.28, gy + gs * 0.35);
        cairo_line_to(cr, gx + gs * 0.72, gy + gs * 0.35);
        cairo_move_to(cr, gx + gs * 0.28, gy + gs * 0.55);
        cairo_line_to(cr, gx + gs * 0.58, gy + gs * 0.55);
        cairo_stroke(cr);
    } else if (kind && g_str_has_prefix(kind, "apps")) {
        /* 应用网格 2×2 */
        double cell = gs * 0.30;
        double off = gs * 0.18;
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                qy_rounded_rect(cr, gx + off + col * cell, gy + off + row * cell,
                                cell * 0.62, cell * 0.62, 2.5);
                cairo_stroke(cr);
            }
        }
    } else {
        /* 默认 files：文件夹 */
        double fx = gx, fy = gy + gs * 0.08;
        qy_rounded_rect(cr, fx + gs * 0.04, fy + gs * 0.14, gs * 0.92, gs * 0.66, 3.5);
        cairo_stroke(cr);
        cairo_move_to(cr, fx + gs * 0.14, fy + gs * 0.22);
        cairo_line_to(cr, fx + gs * 0.38, fy + gs * 0.10);
        cairo_line_to(cr, fx + gs * 0.62, fy + gs * 0.10);
        cairo_line_to(cr, fx + gs * 0.86, fy + gs * 0.22);
        cairo_stroke(cr);
    }
}