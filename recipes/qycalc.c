/* qycalc.c — 澜岫计算器
 * 简洁桌面计算器：数字 + 四则运算 + 括号，按钮式输入。
 * 自动化: QYCALC_EXPR=表达式 启动后自动求值并输出 QYCALCDBG。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

static GtkWidget *expr_label  = NULL;   /* 当前输入表达式 */
static GtkWidget *result_label = NULL;  /* 结果 */
static GtkWidget *win = NULL;
static char expr_buf[256] = {0};

/* ---------- 简单表达式求值器（+ - * / 和括号，含优先级） ---------- */
static const char *ep;
static int ep_error;

static double parse_expr(void);   /* 前向声明 */

static void skip_sp(void) {
    while (*ep == ' ') ep++;
}

static double parse_number(void) {
    skip_sp();
    if (*ep == '(') {
        ep++;
        double v = parse_expr();
        skip_sp();
        if (*ep == ')') ep++;
        else ep_error = 1;
        return v;
    }
    char *end = NULL;
    double v = strtod(ep, &end);
    if (end == ep) { ep_error = 1; return 0; }
    ep = end;
    return v;
}

static double parse_term(void) {
    double v = parse_number();
    for (;;) {
        skip_sp();
        char op = *ep;
        if (op == '*' || op == '/') {
            ep++;
            double rhs = parse_number();
            if (op == '*') v *= rhs;
            else if (rhs == 0) { ep_error = 1; return 0; }
            else v /= rhs;
        } else break;
    }
    return v;
}

static double parse_expr(void) {
    double v = parse_term();
    for (;;) {
        skip_sp();
        char op = *ep;
        if (op == '+' || op == '-') {
            ep++;
            double rhs = parse_term();
            if (op == '+') v += rhs; else v -= rhs;
        } else break;
    }
    return v;
}

static int eval_expr(const char *s, double *out) {
    ep = s;
    ep_error = 0;
    double v = parse_expr();
    skip_sp();
    if (ep_error || *ep != '\0') return -1;
    *out = v;
    return 0;
}

/* ---------- 输入与显示 ---------- */
/* 点击结果复制到剪贴板（release 274） */
static gboolean on_result_copy(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->button != 1) return FALSE;
    const char *txt = gtk_label_get_text(GTK_LABEL(result_label));
    if (txt && txt[0] == '=' && txt[1] == ' ') {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, txt + 2, -1);
        GtkWidget *win = gtk_widget_get_toplevel(w);
        if (win) gtk_window_set_title(GTK_WINDOW(win), TR("澜岫计算器 — 已复制"));
        return TRUE;
    }
    return FALSE;
}

static void update_display(void) {
    gtk_label_set_text(GTK_LABEL(expr_label), expr_buf[0] ? expr_buf : "0");
}

static void append_char(const char c) {
    size_t n = strlen(expr_buf);
    if (n + 2 >= sizeof expr_buf) return;
    expr_buf[n] = c;
    expr_buf[n + 1] = '\0';
    update_display();
}

static void on_num_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    append_char(*(const char *)ud);
}

static void on_op_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    const char *op = (const char *)ud;
    size_t n = strlen(expr_buf);
    if (n > 0 && strchr("+-*/(", expr_buf[n - 1])) return;  /* 避免重复运算符 */
    g_strlcat(expr_buf, op, sizeof expr_buf);
    update_display();
}

static void on_clear_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    expr_buf[0] = '\0';
    gtk_label_set_text(GTK_LABEL(result_label), "");
    update_display();
}

static void on_backspace_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    size_t n = strlen(expr_buf);
    if (n > 0) expr_buf[n - 1] = '\0';
    update_display();
}

static void on_equal_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    double result = 0;
    if (expr_buf[0] && eval_expr(expr_buf, &result) == 0) {
        char out[96];
        if (result == (double)(long)result)
            g_snprintf(out, sizeof out, "= %ld  0x%lx", (long)result, (unsigned long)(long)result);
        else
            g_snprintf(out, sizeof out, "= %g", result);
        gtk_label_set_text(GTK_LABEL(result_label), out);
        g_printerr("QYCALCDBG: expr=%s result=%g\n", expr_buf, result);
    } else {
        gtk_label_set_text(GTK_LABEL(result_label), TR("= 错误"));
        g_printerr("QYCALCDBG: expr=%s error\n", expr_buf);
    }
}

/* ---------- 科学模式 ---------- */
static int sci_mode = 0;             /* 0=角度制 DEG, 1=弧度制 RAD */
static GtkWidget *sci_grid = NULL;   /* 可展开的科学按钮区 */
static GtkWidget *sci_toggle_btn = NULL;
static GtkWidget *sci_mode_btn = NULL;

/* 读取当前显示值（求值表达式；失败返回 FALSE） */
static gboolean current_value(double *out) {
    if (expr_buf[0] && eval_expr(expr_buf, out) == 0) return TRUE;
    *out = 0;
    return FALSE;
}

/* 用结果替换当前表达式并刷新显示 */
static void set_result_value(double v) {
    g_snprintf(expr_buf, sizeof expr_buf, "%g", v);
    update_display();
    char out[64];
    if (v == (double)(long)v)
        g_snprintf(out, sizeof out, "= %ld", (long)v);
    else
        g_snprintf(out, sizeof out, "= %g", v);
    gtk_label_set_text(GTK_LABEL(result_label), out);
    g_printerr("QYCALCDBG: sci expr=%s result=%g\n", expr_buf, v);
}

/* 显示错误结果 */
static void sci_error(void) {
    gtk_label_set_text(GTK_LABEL(result_label), TR("= 错误"));
}

/* 一元函数（角度制下 sin/cos/tan 先转弧度）。
 * 数学函数用纯 C 实现，避免链接 -lm（编译命令不含 -lm）。 */
static void sci_unary(double (*fn)(double), gboolean is_trig) {
    double v = 0;
    if (!current_value(&v)) { sci_error(); return; }
    if (is_trig && sci_mode == 0) v = v * 3.141592653589793 / 180.0;
    double r = fn(v);
    /* 检查 NaN / 无穷（不依赖 libm 的 isnan/isinf） */
    if (r != r || r > 1e308 || r < -1e308) { sci_error(); return; }
    set_result_value(r);
}

/* ---------- 纯 C 数学函数（不链接 libm） ---------- */
static double sci_sin(double x) {
    /* 归约到 [-π, π] */
    const double pi = 3.141592653589793, two_pi = 6.283185307179586;
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    /* sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ... */
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i <= 14; i++) {
        term *= -x2 / ((2.0 * i) * (2.0 * i + 1.0));
        sum += term;
        if (term < 1e-16 && term > -1e-16) break;
    }
    return sum;
}

static double sci_cos(double x) {
    const double pi = 3.141592653589793, two_pi = 6.283185307179586;
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    /* cos(x) = 1 - x^2/2! + x^4/4! - ... */
    double x2 = x * x, term = 1.0, sum = 1.0;
    for (int i = 1; i <= 14; i++) {
        term *= -x2 / ((2.0 * i - 1.0) * (2.0 * i));
        sum += term;
        if (term < 1e-16 && term > -1e-16) break;
    }
    return sum;
}

static double sci_tan(double x) {
    double c = sci_cos(x);
    if (c == 0) return 0.0 / 0.0;   /* NaN */
    return sci_sin(x) / c;
}

static double sci_sqrt(double x) {
    if (x < 0) return 0.0 / 0.0;    /* NaN */
    if (x == 0) return 0;
    double g = x;
    for (int i = 0; i < 64; i++) {
        double ng = 0.5 * (g + x / g);
        if (ng == g) break;
        g = ng;
    }
    return g;
}

static double sci_ln(double x) {
    if (x <= 0) return 0.0 / 0.0; /* NaN */
    /* 归一化到 [0.5, 2): x = m * 2^e */
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 0.5)  { x *= 2.0; e--; }
    /* ln(m) = 2*(y + y^3/3 + y^5/5 + ...), y = (m-1)/(m+1) */
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y, term = y, sum = y;
    for (int i = 1; i <= 200; i++) {
        term *= y2;
        double add = term / (2.0 * i + 1.0);
        sum += add;
        if (add < 1e-16 && add > -1e-16) break;
    }
    return 2.0 * sum + e * 0.6931471805599453;
}

static double sci_log10(double x) {
    if (x <= 0) return 0.0 / 0.0;   /* NaN */
    return sci_ln(x) / 2.302585092994046;
}

static double sci_sq_fn(double v) { return v * v; }
static double sci_inv_fn(double v) { return v == 0 ? 0.0 / 0.0 : 1.0 / v; }

static void on_sin_clicked(GtkButton *b, gpointer ud) { (void)b; (void)ud; sci_unary(sci_sin, TRUE); }
static void on_cos_clicked(GtkButton *b, gpointer ud) { (void)b; (void)ud; sci_unary(sci_cos, TRUE); }
static void on_tan_clicked(GtkButton *b, gpointer ud) { (void)b; (void)ud; sci_unary(sci_tan, TRUE); }
static void on_sqrt_clicked(GtkButton *b, gpointer ud) { (void)b; (void)ud; sci_unary(sci_sqrt, FALSE); }
static void on_log_clicked(GtkButton *b, gpointer ud)  { (void)b; (void)ud; sci_unary(sci_log10, FALSE); }
static void on_ln_clicked(GtkButton *b, gpointer ud)   { (void)b; (void)ud; sci_unary(sci_ln, FALSE); }
static void on_sq_clicked(GtkButton *b, gpointer ud)   { (void)b; (void)ud; sci_unary(sci_sq_fn, FALSE); }
static void on_inv_clicked(GtkButton *b, gpointer ud)  { (void)b; (void)ud; sci_unary(sci_inv_fn, FALSE); }

/* π：把常量插入表达式（运算符/括号后直接拼接，数字后自动补乘号） */
static void on_pi_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *pi = "3.141592653589793";
    size_t n = strlen(expr_buf);
    if (n == 0 || strchr("+-*/(", expr_buf[n - 1]))
        g_strlcat(expr_buf, pi, sizeof expr_buf);
    else {
        g_strlcat(expr_buf, "*", sizeof expr_buf);
        g_strlcat(expr_buf, pi, sizeof expr_buf);
    }
    update_display();
}

/* 角度制 / 弧度制切换按钮（同时也是模式提示） */
static void on_mode_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    sci_mode = !sci_mode;
    gtk_button_set_label(GTK_BUTTON(sci_mode_btn), sci_mode ? "RAD" : "DEG");
    gtk_widget_set_tooltip_text(sci_mode_btn, sci_mode ? TR("弧度制") : TR("角度制"));
}

/* 展开/收起科学模式按钮区 */
static void on_sci_toggle(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    gboolean show = !gtk_widget_get_visible(sci_grid);
    gtk_widget_set_visible(sci_grid, show);
    gtk_button_set_label(GTK_BUTTON(sci_toggle_btn), show ? TR("标准模式") : TR("科学模式"));
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(sci_toggle_btn));
    if (GTK_IS_WINDOW(top))
        gtk_window_resize(GTK_WINDOW(top), 280, show ? 520 : 380);
}

/* 科学区按钮 */
static GtkWidget *sci_button(const char *label, GCallback cb) {
    GtkWidget *b = gtk_button_new_with_label(label);
    qy_add_class(b, "qy-btn");
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

/* ---------- 键盘输入：数字/运算符/回车/退格 ---------- */
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w; (void)ud;
    if (ev->keyval >= GDK_KEY_0 && ev->keyval <= GDK_KEY_9) {
        char c = (char)('0' + (ev->keyval - GDK_KEY_0));
        append_char(c);
        return TRUE;
    }
    switch (ev->keyval) {
        case GDK_KEY_plus: case GDK_KEY_KP_Add: append_char('+'); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract: append_char('-'); return TRUE;
        case GDK_KEY_asterisk: case GDK_KEY_KP_Multiply: append_char('*'); return TRUE;
        case GDK_KEY_slash: case GDK_KEY_KP_Divide: append_char('/'); return TRUE;
        case GDK_KEY_parenleft: append_char('('); return TRUE;
        case GDK_KEY_parenright: append_char(')'); return TRUE;
        case GDK_KEY_period: case GDK_KEY_KP_Decimal: append_char('.'); return TRUE;
        case GDK_KEY_BackSpace: on_backspace_clicked(NULL, NULL); return TRUE;
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_equal:
            on_equal_clicked(NULL, NULL);
            return TRUE;
        case GDK_KEY_Escape: case GDK_KEY_c: case GDK_KEY_C:
            on_clear_clicked(NULL, NULL);
            return TRUE;
    }
    return FALSE;
}

/* 自动化验证: QYCALC_KEY=12+34= 启动后模拟键盘输入 */
static gboolean auto_key_input(gpointer p) {
    const char *s = (const char *)p;
    for (const char *q = s; *q; q++) {
        GdkEventKey ev;
        memset(&ev, 0, sizeof ev);
        if (*q >= '0' && *q <= '9') ev.keyval = GDK_KEY_0 + (*q - '0');
        else if (*q == '+') ev.keyval = GDK_KEY_plus;
        else if (*q == '-') ev.keyval = GDK_KEY_minus;
        else if (*q == '*') ev.keyval = GDK_KEY_asterisk;
        else if (*q == '/') ev.keyval = GDK_KEY_slash;
        else if (*q == '(') ev.keyval = GDK_KEY_parenleft;
        else if (*q == ')') ev.keyval = GDK_KEY_parenright;
        else if (*q == '.') ev.keyval = GDK_KEY_period;
        else if (*q == '=') ev.keyval = GDK_KEY_equal;
        else if (*q == 'c' || *q == 'C') ev.keyval = GDK_KEY_C;
        else continue;
        on_key_press(NULL, &ev, NULL);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 按钮网格 ---------- */
static GtkWidget *calc_button(const char *label, const char *ud, GCallback cb) {
    GtkWidget *b = gtk_button_new_with_label(label);
    qy_add_class(b, "qy-btn");
    g_signal_connect(b, "clicked", cb, g_strdup(ud));
    return b;
}

/* ---------- 自动化: QYCALC_EXPR=1+2*3 启动后求值 ---------- */
static gboolean auto_calc(gpointer p) {
    const char *expr = (const char *)p;
    g_strlcpy(expr_buf, expr, sizeof expr_buf);
    update_display();
    double result = 0;
    if (eval_expr(expr_buf, &result) == 0) {
        char out[96];
        if (result == (double)(long)result)
            g_snprintf(out, sizeof out, "= %ld  0x%lx", (long)result, (unsigned long)(long)result);
        else
            g_snprintf(out, sizeof out, "= %g", result);
        gtk_label_set_text(GTK_LABEL(result_label), out);
        g_printerr("QYCALCDBG: expr=%s result=%g\n", expr_buf, result);
    } else {
        g_printerr("QYCALCDBG: expr=%s error\n", expr_buf);
    }
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("计算器"));
    gtk_window_set_default_size(GTK_WINDOW(win), 280, 380);
    gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    expr_label = gtk_label_new("0");
    gtk_widget_set_halign(expr_label, GTK_ALIGN_END);
    qy_add_class(expr_label, "qy-calc-expr");
    gtk_box_pack_start(GTK_BOX(vbox), expr_label, FALSE, FALSE, 2);

    result_label = gtk_label_new("");
    gtk_widget_set_halign(result_label, GTK_ALIGN_END);
    qy_add_class(result_label, "qy-calc-result");
    gtk_widget_set_tooltip_text(result_label, TR("点击复制结果"));
    gtk_widget_add_events(result_label, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(result_label, "button-press-event", G_CALLBACK(on_result_copy), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), result_label, FALSE, FALSE, 2);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_box_pack_start(GTK_BOX(vbox), grid, TRUE, TRUE, 0);

    /* 行 0: C ⌫ ( ) */
    gtk_grid_attach(GTK_GRID(grid), calc_button("C", "", G_CALLBACK(on_clear_clicked)), 0, 0, 1, 1);
    GtkWidget *b_bs = qy_icon_button(QY_ICON_BACK, 18, TR("退格"), "qy-btn");
    g_signal_connect(b_bs, "clicked", G_CALLBACK(on_backspace_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), b_bs, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("(", "(", G_CALLBACK(on_op_clicked)), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button(")", ")", G_CALLBACK(on_op_clicked)), 3, 0, 1, 1);
    /* 行 1: 7 8 9 / */
    gtk_grid_attach(GTK_GRID(grid), calc_button("7", "7", G_CALLBACK(on_num_clicked)), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("8", "8", G_CALLBACK(on_num_clicked)), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("9", "9", G_CALLBACK(on_num_clicked)), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("/", "/", G_CALLBACK(on_op_clicked)), 3, 1, 1, 1);
    /* 行 2: 4 5 6 * */
    gtk_grid_attach(GTK_GRID(grid), calc_button("4", "4", G_CALLBACK(on_num_clicked)), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("5", "5", G_CALLBACK(on_num_clicked)), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("6", "6", G_CALLBACK(on_num_clicked)), 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("*", "*", G_CALLBACK(on_op_clicked)), 3, 2, 1, 1);
    /* 行 3: 1 2 3 - */
    gtk_grid_attach(GTK_GRID(grid), calc_button("1", "1", G_CALLBACK(on_num_clicked)), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("2", "2", G_CALLBACK(on_num_clicked)), 1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("3", "3", G_CALLBACK(on_num_clicked)), 2, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button("-", "-", G_CALLBACK(on_op_clicked)), 3, 3, 1, 1);
    /* 行 4: 0 . = + */
    gtk_grid_attach(GTK_GRID(grid), calc_button("0", "0", G_CALLBACK(on_num_clicked)), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), calc_button(".", ".", G_CALLBACK(on_op_clicked)), 1, 4, 1, 1);
    GtkWidget *eq = gtk_button_new_with_label("=");
    qy_add_class(eq, "qy-btn qy-btn-accent");
    g_signal_connect(eq, "clicked", G_CALLBACK(on_equal_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), eq, 2, 4, 2, 1);

    /* 科学模式按钮区（默认隐藏，点「科学模式」展开） */
    sci_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(sci_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(sci_grid), 6);
    /* 行 0: sin cos tan x² */
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("sin", G_CALLBACK(on_sin_clicked)), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("cos", G_CALLBACK(on_cos_clicked)), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("tan", G_CALLBACK(on_tan_clicked)), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("x²", G_CALLBACK(on_sq_clicked)), 3, 0, 1, 1);
    /* 行 1: sqrt log ln 1/x */
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("√", G_CALLBACK(on_sqrt_clicked)), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("log", G_CALLBACK(on_log_clicked)), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("ln", G_CALLBACK(on_ln_clicked)), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("1/x", G_CALLBACK(on_inv_clicked)), 3, 1, 1, 1);
    /* 行 2: ( ) π [DEG/RAD] */
    gtk_grid_attach(GTK_GRID(sci_grid), calc_button("(", "(", G_CALLBACK(on_op_clicked)), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), calc_button(")", ")", G_CALLBACK(on_op_clicked)), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(sci_grid), sci_button("π", G_CALLBACK(on_pi_clicked)), 2, 2, 1, 1);
    sci_mode_btn = sci_button("DEG", G_CALLBACK(on_mode_clicked));
    gtk_widget_set_tooltip_text(sci_mode_btn, TR("角度制"));
    gtk_grid_attach(GTK_GRID(sci_grid), sci_mode_btn, 3, 2, 1, 1);
    gtk_widget_set_no_show_all(sci_grid, TRUE);
    gtk_widget_hide(sci_grid);
    gtk_box_pack_start(GTK_BOX(vbox), sci_grid, FALSE, FALSE, 0);

    /* 科学模式切换按钮 */
    sci_toggle_btn = gtk_button_new_with_label(TR("科学模式"));
    qy_add_class(sci_toggle_btn, "qy-btn");
    gtk_widget_set_name(sci_toggle_btn, "qycalc-sci-toggle");
    g_signal_connect(sci_toggle_btn, "clicked", G_CALLBACK(on_sci_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), sci_toggle_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    /* 自动化验证: QYCALC_EXPR=1+2*3 启动后求值 */
    const char *env_expr = g_getenv("QYCALC_EXPR");
    if (env_expr && env_expr[0])
        g_timeout_add(700, auto_calc, g_strdup(env_expr));

    /* 键盘输入支持 */
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), NULL);
    /* 自动化验证: QYCALC_KEY=12+34= 启动后模拟键盘输入 */
    const char *env_key = g_getenv("QYCALC_KEY");
    if (env_key && env_key[0])
        g_timeout_add(900, auto_key_input, g_strdup(env_key));

    gtk_main();
    return 0;
}