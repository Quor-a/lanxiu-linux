/* qymime — 澜岫默认程序/MIME 关联设置
 * 维护 ~/.config/lanxiu/mimeapps.list（格式: text/plain=qyedit.desktop），
 * 供 qyfiles 等按文件 MIME 匹配默认程序使用（本工具只生成/维护配置）。
 * 深色主题，窗口类名 #qy-mime-win。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <glib/gstdio.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

typedef struct {
    const char *mime;   /* MIME 类型键 */
    const char *name;   /* 显示名（中文） */
    const char *def;    /* 默认程序（无 .desktop 后缀） */
} MimeEntry;

static const MimeEntry MIMES[] = {
    { "text/plain",         "文本文件",   "qyedit" },
    { "image/png",          "PNG 图片",   "qyview" },
    { "image/jpeg",         "JPEG 图片",  "qyview" },
    { "audio/*",            "音频文件",   "qymedia" },
    { "video/*",            "视频文件",   "qymedia" },
    { "text/html",          "网页",       "qybrowser" },
    { "application/zip",    "ZIP 压缩包", "qyarc" },
    { "application/pdf",    "PDF 文档",   "qyview" },
};
#define NMIMES ((int)(sizeof MIMES / sizeof MIMES[0]))

/* 候选程序（ComboBox 中的 id 与显示名一致；"无" id 为空串） */
static const char *APPS[] = { "qyedit", "qyview", "qymedia", "qybrowser", "qyarc", NULL };

typedef struct {
    const MimeEntry *entry;
    GtkWidget *combo;      /* GtkComboBoxText */
    GtkWidget *cur_label;    /* 当前默认程序 */
} MimeRow;

static MimeRow rows[NMIMES];
static gboolean suppress_save = FALSE;   /* 初始化/重置时阻止误写盘 */

/* ---------- 配置读写 ---------- */
static char *mime_path(void) {
    return g_build_filename(g_get_home_dir(), ".config", "lanxiu", "mimeapps.list", NULL);
}

/* 程序名 → 中文显示名 */
static const char *app_cn(const char *app) {
    if (!app || !app[0]) return TR("未设置");
    if (!strcmp(app, "qyedit"))    return TR("文本编辑");
    if (!strcmp(app, "qyview"))   return TR("图片查看");
    if (!strcmp(app, "qymedia"))  return TR("媒体播放");
    if (!strcmp(app, "qybrowser")) return TR("浏览器");
    if (!strcmp(app, "qyarc"))     return TR("压缩管理");
    return app;
}

/* 设置 Combo 当前项；app 为空表示"无"，未知程序归入"无" */
static void set_combo_app(GtkWidget *combo, const char *app) {
    if (!app || !app[0]) app = "";
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), app))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "");
}

static void mime_save(void) {
    char *dir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GString *s = g_string_new(NULL);
    for (int i = 0; i < NMIMES; i++) {
        const char *app = gtk_combo_box_get_active_id(GTK_COMBO_BOX(rows[i].combo));
        if (app && app[0])
            g_string_append_printf(s, "%s=%s.desktop\n", rows[i].entry->mime, app);
    }
    char *path = mime_path();
    g_file_set_contents(path, s->str, -1, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

static void mime_load(void) {
    char *path = mime_path();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *eq = strchr(lines[i], '=');
            if (!eq) continue;
            *eq = 0;
            char *app = eq + 1;
            if (g_str_has_suffix(app, ".desktop"))
                app[strlen(app) - strlen(".desktop")] = 0;
            for (int r = 0; r < NMIMES; r++) {
                if (!strcmp(rows[r].entry->mime, lines[i])) {
                    set_combo_app(rows[r].combo, app);
                    break;
                }
            }
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
}

/* ---------- UI ---------- */
static void update_cur_label(MimeRow *row) {
    const char *app = gtk_combo_box_get_active_id(GTK_COMBO_BOX(row->combo));
    gchar *txt = g_strdup_printf("%s: %s", TR("当前"), app_cn(app));
    gtk_label_set_text(GTK_LABEL(row->cur_label), txt);
    g_free(txt);
}

static void on_combo_changed(GtkComboBox *cb, gpointer ud) {
    (void)cb;
    MimeRow *row = ud;
    update_cur_label(row);
    if (suppress_save) return;
    mime_save();
}

static GtkWidget *make_row(MimeRow *row) {
    const MimeEntry *e = row->entry;
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(h, 8);
    gtk_widget_set_margin_end(h, 8);
    gtk_widget_set_margin_top(h, 5);
    gtk_widget_set_margin_bottom(h, 5);

    /* 左: MIME 名 + 类型 */
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *name = gtk_label_new(TR(e->name));
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    qy_add_class(name, "qy-mime-name");
    GtkWidget *mime = gtk_label_new(e->mime);
    gtk_widget_set_halign(mime, GTK_ALIGN_START);
    qy_add_class(mime, "qy-mime-type");
    gtk_box_pack_start(GTK_BOX(v), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), mime, FALSE, FALSE, 0);

    /* 中: 当前默认程序 */
    row->cur_label = gtk_label_new("");
    gtk_widget_set_halign(row->cur_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(row->cur_label, TRUE);
    qy_add_class(row->cur_label, "qy-mime-current");

    /* 右: 候选程序 */
    row->combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(row->combo), "", TR("无"));
    for (int i = 0; APPS[i]; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(row->combo), APPS[i], APPS[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(row->combo), 0);
    gtk_widget_set_size_request(row->combo, 110, -1);
    g_signal_connect(row->combo, "changed", G_CALLBACK(on_combo_changed), row);

    gtk_box_pack_start(GTK_BOX(h), v, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), row->cur_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(h), row->combo, FALSE, FALSE, 0);

    set_combo_app(row->combo, e->def);
    update_cur_label(row);
    return h;
}

/* 重置: 删除配置文件并把各 MIME 恢复为内置默认 */
static void on_reset(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    char *path = mime_path();
    g_remove(path);
    g_free(path);
    suppress_save = TRUE;
    for (int i = 0; i < NMIMES; i++) {
        set_combo_app(rows[i].combo, rows[i].entry->def);
        update_cur_label(&rows[i]);
    }
    suppress_save = FALSE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("默认程序"));
    gtk_widget_set_name(win, "qy-mime-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("默认程序"));
    qy_window_setup(GTK_WINDOW(win), 520, 400, 480, 360, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶部说明 */
    GtkWidget *desc = gtk_label_new(TR("选择文件类型对应的默认程序，配置保存在 ~/.config/lanxiu/mimeapps.list"));
    gtk_widget_set_name(desc, "qy-mime-desc");
    gtk_widget_set_halign(desc, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc), 0);
    gtk_box_pack_start(GTK_BOX(vbox), desc, FALSE, FALSE, 0);

    /* 常见 MIME 列表 */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(sw), list);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    suppress_save = TRUE;
    for (int i = 0; i < NMIMES; i++) {
        rows[i].entry = &MIMES[i];
        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), make_row(&rows[i]));
        gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
    }

    /* 重置默认 */
    GtkWidget *btn = gtk_button_new_with_label(TR("重置默认"));
    gtk_widget_set_halign(btn, GTK_ALIGN_END);
    qy_add_class(btn, "qy-mime-btn");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_reset), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);

    mime_load();
    suppress_save = FALSE;

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}