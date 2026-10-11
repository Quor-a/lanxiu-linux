/* qyconvert.c — 澜岫单位/货币转换器
 * 支持 长度/重量/温度/数据 四类常见单位换算：
 * 顶部类别下拉，from/to 单位下拉 + 输入框，实时显示换算结果大字。
 * 温度使用专用公式（C/F/K），其余按 value*from_factor/to_factor。
 * 自动化: QYCONVERT_VALUE=12 启动后填入输入框（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <math.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

typedef struct {
    const char *name;
    double factor;      /* 相对基准单位的倍数（温度类别不使用） */
} Unit;

typedef struct {
    const char *name;
    const Unit *units;
    int n;
} Category;

/* ---------- 单位表 ---------- */
static const Unit length_units[] = {
    { "米 (m)",        1.0 },
    { "千米 (km)",   1000.0 },
    { "毫米 (mm)",     0.001 },
    { "厘米 (cm)",     0.01 },
    { "英寸 (in)",   0.0254 },
    { "英尺 (ft)",   0.3048 },
};
static const Unit weight_units[] = {
    { "千克 (kg)",       1.0 },
    { "克 (g)",          0.001 },
    { "磅 (lb)",  0.45359237 },
    { "盎司 (oz)", 0.028349523125 },
};
static const Unit temp_units[] = {
    { "摄氏度 (°C)", 0.0 },
    { "华氏度 (°F)", 0.0 },
    { "开尔文 (K)",  0.0 },
};
static const Unit data_units[] = {
    { "字节 (B)",         1.0 },
    { "千字节 (KB)",   1024.0 },
    { "兆字节 (MB)", 1048576.0 },
    { "吉字节 (GB)", 1073741824.0 },
};

enum { CAT_LENGTH = 0, CAT_WEIGHT, CAT_TEMP, CAT_DATA };

static const Category cats[] = {
    { "长度", length_units, G_N_ELEMENTS(length_units) },
    { "重量", weight_units, G_N_ELEMENTS(weight_units) },
    { "温度", temp_units,   G_N_ELEMENTS(temp_units) },
    { "数据", data_units,   G_N_ELEMENTS(data_units) },
};
#define NCATS ((int)G_N_ELEMENTS(cats))

static GtkWidget *cat_combo = NULL;   /* 类别 */
static GtkWidget *from_combo = NULL;  /* 原单位 */
static GtkWidget *to_combo = NULL;    /* 目标单位 */
static GtkWidget *entry = NULL;       /* 输入值 */
static GtkWidget *result_lb = NULL;   /* #qy-convert-result */

/* 温度专用换算：C=0 F=1 K=2 */
static double temp_convert(double v, int from, int to) {
    double c;
    switch (from) {
        case 1:  c = (v - 32.0) * 5.0 / 9.0; break;   /* F→C */
        case 2:  c = v - 273.15; break;               /* K→C */
        default: c = v;                               /* C */
    }
    switch (to) {
        case 0:  return c;
        case 1:  return c * 9.0 / 5.0 + 32.0;
        case 2:  return c + 273.15;
        default: return v;
    }
}

/* 结果格式化：整数直接显示，小数最多 6 位有效数字 */
static void format_result(double v, char *buf, size_t sz) {
    if (v == (gint64)v && fabs(v) < 1e15) {
        g_snprintf(buf, sz, "%" G_GINT64_FORMAT, (gint64)v);
    } else {
        g_snprintf(buf, sz, "%.6g", v);
    }
}

/* ---------- 换算 ---------- */
static void do_convert(void) {
    int cat = gtk_combo_box_get_active(GTK_COMBO_BOX(cat_combo));
    int fi = gtk_combo_box_get_active(GTK_COMBO_BOX(from_combo));
    int ti = gtk_combo_box_get_active(GTK_COMBO_BOX(to_combo));
    if (cat < 0 || cat >= NCATS || fi < 0 || ti < 0) return;

    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    char *end = NULL;
    double v = g_ascii_strtod(text, &end);
    if (end == text) {                       /* 非法输入 */
        gtk_label_set_text(GTK_LABEL(result_lb), "—");
        return;
    }

    double out;
    if (cat == CAT_TEMP) {
        out = temp_convert(v, fi, ti);
    } else {
        const Unit *units = cats[cat].units;
        out = v * units[fi].factor / units[ti].factor;
    }

    char buf[64];
    format_result(out, buf, sizeof buf);
    gtk_label_set_text(GTK_LABEL(result_lb), buf);
    g_printerr("QYCONVERTDBG: cat=%d from=%d to=%d in=%.6g out=%s\n",
               cat, fi, ti, v, buf);
}

/* ---------- 类别切换：重建单位下拉 ---------- */
static void repopulate_units(int cat) {
    if (cat < 0 || cat >= NCATS) return;
    const Category *c = &cats[cat];

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(from_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(to_combo));
    for (int i = 0; i < c->n; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(from_combo), TR(c->units[i].name));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(to_combo), TR(c->units[i].name));
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(from_combo), 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(to_combo), c->n > 1 ? 1 : 0);
    do_convert();
}

static void on_cat_changed(GtkComboBox *box, gpointer ud) {
    (void)box; (void)ud;
    repopulate_units(gtk_combo_box_get_active(box));
}

static void on_unit_changed(GtkComboBox *box, gpointer ud) {
    (void)box; (void)ud;
    do_convert();
}

static void on_entry_changed(GtkEditable *e, gpointer ud) {
    (void)e; (void)ud;
    do_convert();
}

/* 交换单位 */
static void on_swap_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    int fi = gtk_combo_box_get_active(GTK_COMBO_BOX(from_combo));
    int ti = gtk_combo_box_get_active(GTK_COMBO_BOX(to_combo));
    gtk_combo_box_set_active(GTK_COMBO_BOX(from_combo), ti);
    gtk_combo_box_set_active(GTK_COMBO_BOX(to_combo), fi);
    do_convert();
}

/* ---------- UI ---------- */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("单位换算"));
    gtk_widget_set_name(win, "qy-convert-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("单位换算"));
    qy_window_setup(GTK_WINDOW(win), 380, 460, 340, 420, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 内容卡片 */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    qy_add_class(card, "qy-welcome-card");
    gtk_box_pack_start(GTK_BOX(vbox), card, TRUE, TRUE, 0);

    /* 类别 */
    GtkWidget *cat_lb = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(cat_lb), g_strdup_printf("<b>%s</b>", TR("类别")));
    gtk_widget_set_halign(cat_lb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), cat_lb, FALSE, FALSE, 0);

    cat_combo = gtk_combo_box_text_new();
    for (int i = 0; i < NCATS; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cat_combo), TR(cats[i].name));
    gtk_combo_box_set_active(GTK_COMBO_BOX(cat_combo), CAT_LENGTH);
    g_signal_connect(cat_combo, "changed", G_CALLBACK(on_cat_changed), NULL);
    gtk_box_pack_start(GTK_BOX(card), cat_combo, FALSE, FALSE, 0);

    /* 单位行：从 / 到 */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);

    GtkWidget *from_lb = gtk_label_new(TR("从"));
    gtk_widget_set_halign(from_lb, GTK_ALIGN_START);
    from_combo = gtk_combo_box_text_new();
    GtkWidget *to_lb = gtk_label_new(TR("到"));
    gtk_widget_set_halign(to_lb, GTK_ALIGN_START);
    to_combo = gtk_combo_box_text_new();

    gtk_grid_attach(GTK_GRID(grid), from_lb, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), from_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), to_lb, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), to_combo, 1, 1, 1, 1);
    g_signal_connect(from_combo, "changed", G_CALLBACK(on_unit_changed), NULL);
    g_signal_connect(to_combo, "changed", G_CALLBACK(on_unit_changed), NULL);
    gtk_box_pack_start(GTK_BOX(card), grid, FALSE, FALSE, 0);

    /* 输入框 */
    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "1");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TR("输入数值"));
    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), NULL);
    gtk_box_pack_start(GTK_BOX(card), entry, FALSE, FALSE, 0);

    /* 结果大字 */
    result_lb = gtk_label_new("—");
    gtk_widget_set_name(result_lb, "qy-convert-result");
    gtk_box_pack_start(GTK_BOX(card), result_lb, TRUE, TRUE, 0);

    /* 交换单位按钮（品牌橙） */
    GtkWidget *swap_btn = gtk_button_new_with_label(TR("交换单位"));
    qy_add_class(swap_btn, "qy-convert-btn");
    g_signal_connect(swap_btn, "clicked", G_CALLBACK(on_swap_clicked), NULL);
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(btn_row), swap_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), btn_row, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    /* 初始单位表 */
    repopulate_units(CAT_LENGTH);

    /* 自动化: QYCONVERT_VALUE=12 填入输入框 */
    const char *val = g_getenv("QYCONVERT_VALUE");
    if (val && val[0])
        gtk_entry_set_text(GTK_ENTRY(entry), val);

    gtk_main();
    return 0;
}