/* qytodo.c — 澜岫待办清单 (GTK3)
 * 点击待办行切换完成/未完成（已完成显示删除线），
 * 顶部输入新待办，持久化到 ~/.config/lanxiu/qytodo.txt（每行 [x]/[ ] + 文本）。 */
#include <gtk/gtk.h>
#include <string.h>
#include "qyicon.h"
#include "qytheme.h"
#include "qyl10n.h"

static GtkWidget *listbox = NULL;
static GtkWidget *entry   = NULL;
static GtkWidget *status = NULL;

static const char *todo_file(void) {
    static char path[512];
    g_snprintf(path, sizeof path, "%s/.config/lanxiu/qytodo.txt", g_get_home_dir());
    return path;
}

static void todo_save(void) {
    GString *s = g_string_new(NULL);
    GList *ch = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = ch; l; l = l->next) {
        GtkWidget *r = GTK_WIDGET(l->data);
        const char *text = (const char *)g_object_get_data(G_OBJECT(r), "todo-text");
        gboolean done = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "todo-done"));
        g_string_append_printf(s, "%s %s\n", done ? "[x]" : "[ ]", text ? text : "");
    }
    g_list_free(ch);
    gchar *dir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(todo_file(), s->str, -1, NULL);
    g_string_free(s, TRUE);
}

static void todo_load(void) {
    gchar *content = NULL;
    if (!g_file_get_contents(todo_file(), &content, NULL, NULL)) return;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        if (!line[0]) continue;
        gboolean done = g_str_has_prefix(line, "[x]");
        const char *text = line + 4;
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lb = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
        if (done) qy_add_class(lb, "qy-todo-done");
        gtk_container_add(GTK_CONTAINER(row), lb);
        g_object_set_data(G_OBJECT(row), "todo-text", g_strdup(text));
        g_object_set_data(G_OBJECT(row), "todo-done", GINT_TO_POINTER(done));
        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    }
    g_strfreev(lines);
    g_free(content);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    GtkWidget *lb = gtk_bin_get_child(GTK_BIN(row));
    gboolean done = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "todo-done"));
    done = !done;
    g_object_set_data(G_OBJECT(row), "todo-done", GINT_TO_POINTER(done));
    if (done)
        qy_add_class(lb, "qy-todo-done");
    else
        gtk_style_context_remove_class(gtk_widget_get_style_context(lb), "qy-todo-done");
    todo_save();
}

static void on_add(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!text || !text[0]) return;
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *lb = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
    gtk_container_add(GTK_CONTAINER(row), lb);
    g_object_set_data(G_OBJECT(row), "todo-text", g_strdup(text));
    g_object_set_data(G_OBJECT(row), "todo-done", GINT_TO_POINTER(FALSE));
    gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    todo_save();
    if (status) gtk_label_set_text(GTK_LABEL(status), TR("已添加"));
}

static void on_clear_done(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = ch; l; l = l->next) {
        GtkWidget *r = GTK_WIDGET(l->data);
        gboolean done = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "todo-done"));
        if (done) gtk_widget_destroy(r);
    }
    g_list_free(ch);
    todo_save();
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫待办"));
    qy_make_titlebar(GTK_WINDOW(win), TR("澜岫待办"));
    qy_window_setup(GTK_WINDOW(win), 520, 420, 440, 360, TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TR("输入待办事项..."));
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    GtkWidget *b_add = gtk_button_new_with_label(TR("添加"));
    qy_add_class(b_add, "qy-btn");
    g_signal_connect(b_add, "clicked", G_CALLBACK(on_add), NULL);
    g_signal_connect(entry, "activate", G_CALLBACK(on_add), NULL);
    gtk_box_pack_start(GTK_BOX(row), b_add, FALSE, FALSE, 0);
    GtkWidget *b_clear = gtk_button_new_with_label(TR("删除已完成"));
    qy_add_class(b_clear, "qy-btn");
    g_signal_connect(b_clear, "clicked", G_CALLBACK(on_clear_done), NULL);
    gtk_box_pack_start(GTK_BOX(row), b_clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    listbox = gtk_list_box_new();
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(sw), listbox);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 0);

    todo_load();
    gtk_widget_show_all(win);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_main();
    return 0;
}