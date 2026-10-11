/* qyhelp.c — 澜岫帮助中心
 * 左侧列表选择主题，右侧只读 TextView 显示内置帮助内容。
 * 深色主题，复用 qy-welcome-card 风格；含品牌简介。
 */
#include <gtk/gtk.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

typedef struct {
    const char *title;
    const char *content;
} HelpSection;

static const HelpSection sections[] = {
    { "快速开始",
      "欢迎使用澜岫 LANXIU 桌面。\n"
      "\n"
      "澜岫是一个轻量、快速、美观的 Linux 桌面环境。\n"
      "您可以通过屏幕左侧的 Dock 或按下 Super 键打开开始菜单，\n"
      "在搜索框中输入应用名称即可快速启动应用。\n"
      "\n"
      "常用操作：\n"
      "· Super            打开 / 关闭开始菜单\n"
      "· Super + E        打开文件管理器\n"
      "· Alt + Tab        切换窗口\n"
      "· Ctrl + Alt + T   打开终端\n"
      "· Super + L          锁屏\n"
      "\n"
      "点击右侧「应用指南」可了解各内置应用的用途。" },
    { "应用指南",
      "应用指南\n"
      "\n"
      "· 文件管理器（qyfiles）\n"
      "  管理文件和文件夹，支持复制、剪切、粘贴、删除、压缩等操作。\n"
      "\n"
      "· 终端（weston-terminal）\n"
      "  命令行环境，支持常见 shell 操作。\n"
      "\n"
      "· 系统设置（qysettings）\n"
      "  调整显示、字体、声音、语言、电源等系统参数。\n"
      "\n"
      "· 文本编辑（qyedit）\n"
      "  创建和编辑文本文件。\n"
      "\n"
      "· 软件中心（qystore）\n"
      "  浏览和安装应用。\n"
      "\n"
      "· 时钟 / 天气 / 笔记\n"
      "  查看时间日期、实时天气，记录灵感。\n"
      "\n"
      "· 单位换算（qyconvert）\n"
      "  长度、重量、温度、数据单位之间的快速换算。\n"
      "\n"
      "· 磁盘分析（qydisk）\n"
      "  查看各挂载点的容量使用情况。" },
    { "快捷键",
      "快捷键\n"
      "\n"
      "· Super              开始菜单\n"
      "· Super + E          文件管理器\n"
      "· Alt + Tab          切换窗口\n"
      "· Ctrl + Alt + T     终端\n"
      "· Super + L          锁屏\n"
      "\n"
      "在开始菜单中按方向键可导航应用网格，\n"
      "回车启动选中的应用。" },
    { "常见问题",
      "常见问题\n"
      "\n"
      "Q: 如何安装新应用？\n"
      "A: 打开软件中心（qystore），浏览并安装。\n"
      "\n"
      "Q: 如何更改壁纸和主题？\n"
      "A: 在系统设置中调整外观相关选项。\n"
      "\n"
      "Q: 如何连接网络？\n"
      "A: 打开网络管理（qynet）进行配置。\n"
      "\n"
      "Q: 应用无法启动怎么办？\n"
      "A: 在终端中运行对应命令查看错误输出，或尝试重启桌面。\n"
      "\n"
      "Q: 如何锁屏？\n"
      "A: 按下 Super + L，或点击任务栏的锁屏图标。" },
    { "关于澜岫",
      "澜岫 LANXIU\n"
      "\n"
      "自由如澜 坚固如岫\n"
      "\n"
      "澜岫（LANXIU）是一个面向日常使用的轻量级 Linux 桌面环境，\n"
      "追求简洁、流畅与美观。深色玻璃拟态风格配合品牌橙色，\n"
      "为您带来安静而专注的工作空间。\n"
      "\n"
      "版本：1.6.9\n"
      "\n"
      "· 深色护眼主题，低亮度高对比\n"
      "· 轻量快速，适合日常办公与开发\n"
      "· 内置文件、终端、设置、笔记等常用应用" },
};
#define NSECTIONS ((int)G_N_ELEMENTS(sections))

static GtkWidget *textview = NULL;

static void show_section(int idx) {
    if (idx < 0 || idx >= NSECTIONS) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    gtk_text_buffer_set_text(buf, sections[idx].content, -1);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_buffer_place_cursor(buf, &start);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    if (!row) return;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "section-idx"));
    show_section(idx);
}

/* ---------- UI ---------- */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("帮助中心"));
    gtk_widget_set_name(win, "qy-help-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("帮助中心"));
    qy_window_setup(GTK_WINDOW(win), 640, 440, 560, 400, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 12);
    gtk_container_add(GTK_CONTAINER(win), hbox);

    /* 左侧：帮助主题列表 */
    GtkWidget *sw_list = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw_list), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw_list, 170, -1);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_widget_set_name(listbox, "qy-help-list");
    g_signal_connect(listbox, "row-selected", G_CALLBACK(on_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(sw_list), listbox);

    for (int i = 0; i < NSECTIONS; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "section-idx", GINT_TO_POINTER(i));
        GtkWidget *lbl = gtk_label_new(TR(sections[i].title));
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    }

    /* 右侧：内容文本（只读） */
    GtkWidget *sw_text = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw_text), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    textview = gtk_text_view_new();
    gtk_widget_set_name(textview, "qy-help-text");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textview), 10);
    gtk_container_add(GTK_CONTAINER(sw_text), textview);

    gtk_box_pack_start(GTK_BOX(hbox), sw_list, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sw_text, TRUE, TRUE, 0);

    gtk_widget_show_all(win);

    /* 默认选中第一项 */
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(listbox), first);

    gtk_main();
    return 0;
}