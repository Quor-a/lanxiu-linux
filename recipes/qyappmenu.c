/* qyappmenu - 澜岫开始菜单 (ArcMenu 风格: 搜索+固定网格+常用列表+用户区) */
#include "qyl10n.h"
#include "qyicon.h"
#include "qytheme.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *cmdline;
    int launches;          /* 常用排序 */
    const char *cat;       /* 分类: 系统/文件/工具 */
} AppEntry;

/* 统一图标主题色板 (v1.7): 每应用固定主色 */
static AppEntry apps[] = {
    { "系统设置", "qysettings",      0, "系统" },
    { "系统监视", "qymon",           0, "系统" },
    { "系统安装", "qysetup",         0, "系统" },
    { "用户管理", "qyusers",         0, "系统" },
    { "网络管理", "qynet",           0, "系统" },
    { "文件管理器", "qyfiles",         0, "文件" },
    { "图片查看", "qyview",          0, "文件" },
    { "压缩管理", "qyarc",           0, "文件" },
    { "回收站", "qyfiles --trash", 0, "文件" },
    { "终端", "weston-terminal", 0, "工具" },
    { "文本编辑", "qyedit",          0, "工具" },
    { "软件中心", "qystore",         0, "工具" },
    { "浏览器", "qybrowser",       0, "工具" },
    { "截图工具", "qyshot",          0, "工具" },
    { "屏幕录制", "qyrec",           0, "工具" },
    { "剪贴板", "qyclip",          0, "工具" },
    { "锁屏", "qylock",          0, "系统" },
    { "全局搜索", "qysearch",        0, "系统" },
    { "音乐播放器", "qymedia",         0, "娱乐" },
    { "窗口切换器", "qyswitcher",     0, "系统" },
    { "驱动管理器", "qydriver",       0, "系统" },
    { "Git 工具", "qygit",           0, "工具" },
    { "时钟", "qyclock",         0, "工具" },
    { "天气", "qyweather",         0, "工具" },
    { "笔记", "qynote",           0, "工具" },
    { "单位换算", "qyconvert",      0, "工具" },
    { "帮助中心", "qyhelp",        0, "工具" },
    { "磁盘分析", "qydisk",        0, "工具" },
    { "音量控制", "qyaudio",       0, "工具" },
    { "欢迎", "qywelcome",      0, "系统" },
    { "默认程序", "qymime",       0, "系统" },
    { "电源管理", "qypower",       0, "系统" },
};
#define NAPPS ((int)(sizeof apps / sizeof apps[0]))

static GtkWidget *menu_win = NULL;
static GtkWidget *grid_box = NULL;    /* 固定应用网格容器 */
static GtkWidget *freq_box = NULL;    /* 常用列表容器 */
static GtkWidget *freq_title = NULL;  /* 常用标题(空时隐藏) */
static GtkWidget *search_entry = NULL;

/* ---------- 启动 ---------- */
static void launch_cmd(const char *cmd) {
    GError *err = NULL;
    gchar **argv = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, &err)) { if (err) g_error_free(err); return; }
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    if (err) { g_printerr("launch: %s\n", err->message); g_error_free(err); }
    g_strfreev(argv);
}

/* ---------- 主题 ---------- */
static void load_theme(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    if (gtk_css_provider_load_from_path(p, "/usr/share/themes/lanxiu/gtk-3.0/gtk.css", NULL)) {
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    g_object_unref(p);
}

static void add_class(GtkWidget *w, const char *cls) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* ---------- 常用列表持久化: ~/.config/lanxiu/appmenu-freq ---------- */
#define FREQ_FILE ".config/lanxiu/appmenu-freq"

static char *freq_path(void) {
    return g_build_filename(g_get_home_dir(), FREQ_FILE, NULL);
}

static void freq_save(void) {
    char *dir = g_build_filename(g_get_home_dir(), ".config/lanxiu", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    char *path = freq_path();
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < NAPPS; i++)
            if (apps[i].launches > 0)
                fprintf(f, "%s %d\n", apps[i].name, apps[i].launches);
        fclose(f);
    }
    g_free(path);
}

static void freq_load(void) {
    char *path = freq_path();
    FILE *f = fopen(path, "r");
    if (f) {
        char name[96];
        int n;
        while (fscanf(f, "%95s %d", name, &n) == 2) {
            for (int i = 0; i < NAPPS; i++)
                if (strcmp(apps[i].name, name) == 0)
                    apps[i].launches = n;
        }
        fclose(f);
    }
    g_free(path);
}

/* ---------- 固定区: 图标网格 (4 列) ---------- */
static void on_icon_click(GtkButton *btn, gpointer ud) {
    AppEntry *a = (AppEntry *)ud;
    a->launches++;
    freq_save();
    launch_cmd(a->cmdline);
    gtk_widget_hide(menu_win);
}

/* 应用名 → 统一图标 id（优先 qy_icon_for_app 按命令映射，未识别时本地兜底） */
static QyIconId app_icon_id(const AppEntry *a) {
    QyIconId r = qy_icon_for_app(a->cmdline);
    if (r != QY_ICON_GRID) return r;
    const char *n = a->name;
    if (strstr(n, "文件")) return QY_ICON_FILES;
    if (strstr(n, "图片")) return QY_ICON_IMAGE;
    if (strstr(n, "终端")) return QY_ICON_TERM;
    if (strstr(n, "编辑")) return QY_ICON_EDIT;
    if (strstr(n, "浏览器")) return QY_ICON_BROWSER;
    if (strstr(n, "软件") || strstr(n, "商店")) return QY_ICON_STORE;
    if (strstr(n, "设置")) return QY_ICON_SETTINGS;
    if (strstr(n, "回收")) return QY_ICON_TRASH;
    if (strstr(n, "截图")) return QY_ICON_SHOT;
    if (strstr(n, "录制") || strstr(n, "录屏")) return QY_ICON_VIDEO;
    if (strstr(n, "剪贴")) return QY_ICON_CLIPBOARD;
    if (strstr(n, "锁")) return QY_ICON_LOCK;
    if (strstr(n, "搜索")) return QY_ICON_SEARCH;
    if (strstr(n, "音乐")) return QY_ICON_MUSIC;
    if (strstr(n, "切换")) return QY_ICON_SWITCHER;
    if (strstr(n, "驱动")) return QY_ICON_DRIVER;
    if (strstr(n, "Git")) return QY_ICON_GIT;
    if (strstr(n, "欢迎")) return QY_ICON_WELCOME;
    if (strstr(n, "监视")) return QY_ICON_MONITOR;
    if (strstr(n, "安装")) return QY_ICON_SETUP;
    if (strstr(n, "用户")) return QY_ICON_USERS;
    if (strstr(n, "网络")) return QY_ICON_NETWORK;
    if (strstr(n, "压缩")) return QY_ICON_ARCHIVE;
    if (strstr(n, "时钟")) return QY_ICON_CLOCK;
    if (strstr(n, "天气")) return QY_ICON_WEATHER;
    if (strstr(n, "笔记")) return QY_ICON_NOTE;
    if (strstr(n, "换算")) return QY_ICON_CALC;
    if (strstr(n, "帮助")) return QY_ICON_DOC;
    if (strstr(n, "磁盘")) return QY_ICON_DRIVE;
    return QY_ICON_FILES;
}

/* 统一图标 tile 绘制（玻璃底 + 线稿 + 左上主色圆点） */
static gboolean icon_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    AppEntry *a = (AppEntry *)ud;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double size = al.width < al.height ? al.width : al.height;
    double x = (al.width - size) / 2.0;
    double y = (al.height - size) / 2.0;
    double radius = 12.0 * size / 56.0;
    qy_icon_tile(cr, app_icon_id(a), x, y, size, radius, NULL, NULL);
    /* 左上 4px 主色圆点（保留应用辨识色，不铺满） */
    /* 统一强调橙 #E95420（根除彩虹点） */
    cairo_set_source_rgba(cr, 0.914, 0.329, 0.125, 0.92);
    cairo_arc(cr, x + size * 0.18, y + size * 0.18, size * 0.075, 0, 2 * G_PI);
    cairo_fill(cr);
    return FALSE;
}

static GtkWidget *make_grid_icon(AppEntry *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    /* weston 输入焦点修复（release 238）：应用网格按钮可获焦，支持方向键导航 */
    gtk_widget_set_can_focus(btn, TRUE);
    add_class(btn, "qy-appmenu-item");
    /* 图标画布: 18x18 圆角色块（release 233 统一应用项图标为 18px） */
    GtkWidget *ic = gtk_drawing_area_new();
    gtk_widget_set_size_request(ic, 18, 18);
    g_signal_connect(ic, "draw", G_CALLBACK(icon_draw_cb), a);
    GtkWidget *lb = gtk_label_new(TR(a->name));
    gtk_label_set_max_width_chars(GTK_LABEL(lb), 8);
    gtk_label_set_line_wrap(GTK_LABEL(lb), TRUE);
    gtk_label_set_justify(GTK_LABEL(lb), GTK_JUSTIFY_CENTER);
    add_class(lb, "qy-appmenu-icon-label");
    gtk_container_add(GTK_CONTAINER(btn), v);
    gtk_box_pack_start(GTK_BOX(v), ic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), lb, FALSE, FALSE, 0);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_icon_click), a);
    return btn;
}

/* ---------- 常用区: 双列小列表 ---------- */
static void on_row_click(GtkButton *btn, gpointer ud) {
    AppEntry *a = (AppEntry *)ud;
    a->launches++;
    freq_save();
    launch_cmd(a->cmdline);
    gtk_widget_hide(menu_win);
}

static gboolean freq_icon_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    AppEntry *a = (AppEntry *)ud;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double size = al.width < al.height ? al.width : al.height;
    double x = (al.width - size) / 2.0;
    double y = (al.height - size) / 2.0;
    qy_icon_draw(cr, app_icon_id(a), x, y, size, FALSE, NULL);
    return FALSE;
}

static GtkWidget *make_freq_row(AppEntry *a) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    /* weston 输入焦点修复（release 238）：常用列表按钮可获焦 */
    gtk_widget_set_can_focus(b, TRUE);
    add_class(b, "qy-appmenu-item");
    GtkWidget *ic = gtk_drawing_area_new();
    gtk_widget_set_size_request(ic, 18, 18);
    g_signal_connect(ic, "draw", G_CALLBACK(freq_icon_draw_cb), a);
    GtkWidget *lb = gtk_label_new(TR(a->name));
    gtk_widget_set_halign(lb, GTK_ALIGN_START);
    add_class(lb, "qy-appmenu-freq-label");
    gtk_container_add(GTK_CONTAINER(b), h);
    gtk_box_pack_start(GTK_BOX(h), ic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), lb, TRUE, TRUE, 0);
    g_signal_connect(b, "clicked", G_CALLBACK(on_row_click), a);
    return b;
}

/* ---------- 搜索匹配: 中文名 + 英文名（大小写不敏感） ---------- */
static gboolean app_matches(const AppEntry *a, const char *q) {
    if (!q || !q[0]) return TRUE;
    if (strstr(a->name, q)) return TRUE;
    const char *en = TR(a->name);
    if (en && strstr(en, q)) return TRUE;
    if (en) {
        size_t ql = strlen(q);
        for (const char *p = en; *p; p++) {
            if (g_ascii_tolower(*p) == g_ascii_tolower(q[0]) &&
                g_ascii_strncasecmp(p, q, ql) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/* ---------- 刷新 (按搜索过滤 + 按启动次数排序常用区) ---------- */
static void rebuild(gboolean filtered) {
    const char *q = gtk_entry_get_text(GTK_ENTRY(search_entry));
    GList *ch;

    ch = gtk_container_get_children(GTK_CONTAINER(grid_box));
    for (GList *it = ch; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(ch);
    ch = gtk_container_get_children(GTK_CONTAINER(freq_box));
    for (GList *it = ch; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(ch);

    gboolean any = FALSE;
    gboolean first_cat = TRUE;
    static const char *cats[] = { "系统", "文件", "工具" };
    for (int ci = 0; ci < 3; ci++) {
        int col = 0;
        GtkWidget *cur_row = NULL;
        gboolean cat_any = FALSE;
        for (int i = 0; i < NAPPS; i++) {
            if (strcmp(apps[i].cat, cats[ci]) != 0) continue;
            if (filtered && !app_matches(&apps[i], q)) continue;
            if (!cat_any) {
                GtkWidget *catlb = gtk_label_new(NULL);
                gtk_label_set_markup(GTK_LABEL(catlb), g_strdup_printf("<b>%s</b>", TR(cats[ci])));
                gtk_widget_set_halign(catlb, GTK_ALIGN_START);
                add_class(catlb, "qy-appmenu-cat");
                if (first_cat) {
                    add_class(catlb, "qy-appmenu-cat-first");
                    first_cat = FALSE;
                }
                gtk_box_pack_start(GTK_BOX(grid_box), catlb, FALSE, FALSE, 0);
                cat_any = TRUE;
                any = TRUE;
            }
            if (col == 0) {
                cur_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
                gtk_box_pack_start(GTK_BOX(grid_box), cur_row, FALSE, FALSE, 0);
            }
            gtk_box_pack_start(GTK_BOX(cur_row), make_grid_icon(&apps[i]), TRUE, TRUE, 2);
            col = (col + 1) % 4;
        }
    }
    if (!any) {
        gtk_box_pack_start(GTK_BOX(grid_box), gtk_label_new(TR("无匹配应用")), FALSE, FALSE, 4);
    }

    /* Frequent: launches > 0 的按次数排 */
    AppEntry *sorted[NAPPS];
    for (int i = 0; i < NAPPS; i++) sorted[i] = &apps[i];
    for (int i = 0; i < NAPPS; i++)
        for (int j = i + 1; j < NAPPS; j++)
            if (sorted[j]->launches > sorted[i]->launches) {
                AppEntry *t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }
    int col = 0; GtkWidget *cur_row = NULL;
    int shown = 0;
    for (int i = 0; i < NAPPS && shown < 6; i++) {
        if (sorted[i]->launches == 0) break;
        if (col == 0) {
            cur_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_box_pack_start(GTK_BOX(freq_box), cur_row, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(cur_row), make_freq_row(sorted[i]), TRUE, TRUE, 0);
        col = (col + 1) % 2;
        shown++;
    }
    gtk_widget_show_all(menu_win);
    gtk_widget_set_visible(freq_title, shown > 0);
    gtk_widget_grab_focus(search_entry);   /* 打开即聚焦搜索框 */
}

static void on_search_changed(GtkEditable *e, gpointer ud) {
    rebuild(TRUE);
}

/* 回车启动第一个匹配应用 */
static void on_search_activate(GtkEntry *e, gpointer ud) {
    const char *q = gtk_entry_get_text(e);
    if (!q || !q[0]) return;
    for (int i = 0; i < NAPPS; i++) {
        if (app_matches(&apps[i], q)) {
            apps[i].launches++;
            freq_save();
            launch_cmd(apps[i].cmdline);
            gtk_widget_hide(menu_win);
            return;
        }
    }
}

/* ---------- 电源按钮 ---------- */
static void on_power(GtkButton *b, gpointer ud) {
    launch_cmd("busybox poweroff");
}

static void on_settings_btn(GtkButton *b, gpointer ud) {
    launch_cmd("qysettings");
    gtk_widget_hide(menu_win);
}

static void on_close_clicked(GtkButton *b, gpointer ud) {
    gtk_widget_hide(menu_win);
}

/* 递归查找第一个可见按钮（grid_box 内是 行box→按钮 结构） */
static GtkWidget *find_first_visible_button(GtkWidget *w) {
    if (GTK_IS_BUTTON(w) && gtk_widget_get_visible(w)) return w;
    if (GTK_IS_CONTAINER(w)) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = ch; l; l = l->next) {
            GtkWidget *found = find_first_visible_button(l->data);
            if (found) { g_list_free(ch); return found; }
        }
        g_list_free(ch);
    }
    return NULL;
}

static gboolean on_menu_keypress(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_hide(menu_win);
        return TRUE;
    }
    /* ↓ 聚焦第一个可见应用（网格按钮可继续用方向键在行内移动） */
    if (ev->keyval == GDK_KEY_Down) {
        GtkWidget *b = find_first_visible_button(grid_box);
        if (b) { gtk_widget_grab_focus(b); return TRUE; }
    }
    /* 回车：搜索框有内容时打开第一个匹配应用 */
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        const char *q = search_entry ? gtk_entry_get_text(GTK_ENTRY(search_entry)) : NULL;
        if (q && q[0]) {
            GtkWidget *b = find_first_visible_button(grid_box);
            if (b) { g_signal_emit_by_name(b, "clicked"); return TRUE; }
        }
    }
    return FALSE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    load_theme();

    menu_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(GTK_WIDGET(menu_win), "qyappmenu-win");
    gtk_window_set_decorated(GTK_WINDOW(menu_win), FALSE);
    /* weston 输入焦点修复（release 238）：开始菜单显式接受键盘焦点，
     * 保证 Escape/方向键/回车等按键事件能到达菜单窗口。 */
    gtk_window_set_accept_focus(GTK_WINDOW(menu_win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(menu_win), 460, 420);
    gtk_window_move(GTK_WINDOW(menu_win), 6, 32);
    g_signal_connect(menu_win, "key-press-event", G_CALLBACK(on_menu_keypress), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(menu_win), vbox);

    /* 1. 搜索框 + 关闭按钮 */
    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    search_entry = gtk_search_entry_new();
    add_class(search_entry, "qy-appmenu-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索应用..."));
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(search_entry, "activate", G_CALLBACK(on_search_activate), NULL);
    gtk_box_pack_start(GTK_BOX(search_row), search_entry, TRUE, TRUE, 0);
    GtkWidget *close_btn = qy_icon_button(QY_ICON_CLOSE, 16, NULL, NULL);
    add_class(close_btn, "qy-appmenu-close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(search_row), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), search_row, FALSE, FALSE, 0);

    /* 2. Pinned 固定应用 */
    GtkWidget *pl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(pl), g_strdup_printf("<b>%s</b>", TR("固定")));
    gtk_widget_set_halign(pl, GTK_ALIGN_START);
    add_class(pl, "qy-appmenu-title");
    gtk_box_pack_start(GTK_BOX(vbox), pl, FALSE, FALSE, 0);
    grid_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), grid_box, FALSE, FALSE, 0);

    /* 3. Frequent 常用 */
    GtkWidget *fl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(fl), g_strdup_printf("<b>%s</b>", TR("常用")));
    gtk_widget_set_halign(fl, GTK_ALIGN_START);
    add_class(fl, "qy-appmenu-title");
    freq_title = fl;
    gtk_box_pack_start(GTK_BOX(vbox), fl, FALSE, FALSE, 0);
    freq_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_pack_start(GTK_BOX(vbox), freq_box, FALSE, FALSE, 0);

    /* 4. 底部用户区 */
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *user = gtk_label_new("root");
    gtk_widget_set_halign(user, GTK_ALIGN_START);
    add_class(user, "qy-appmenu-user");
    gtk_box_pack_start(GTK_BOX(bottom), user, TRUE, TRUE, 0);
    GtkWidget *set_btn = qy_icon_button(QY_ICON_SETTINGS, 18, TR("设置"), NULL);
    g_signal_connect(set_btn, "clicked", G_CALLBACK(on_settings_btn), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), set_btn, FALSE, FALSE, 0);
    GtkWidget *pwr_btn = qy_icon_button(QY_ICON_POWER, 18, TR("电源"), NULL);
    g_signal_connect(pwr_btn, "clicked", G_CALLBACK(on_power), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), pwr_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

    g_printerr("QYAPPMENU-START toplevel\n");
    freq_load();      /* 载入历史常用计数 */
    rebuild(FALSE);
    gtk_main();
    return 0;
}
