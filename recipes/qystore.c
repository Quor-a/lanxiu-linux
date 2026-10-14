/* qystore — 澜岫软件中心 (v1.9.5)
 * GTK3 GUI: 浏览 /usr/share/qyrepo/index.json (147 包离线仓库)
 * 搜索过滤 / 已装标记 (/var/lib/qypkg/installed/) / 安装·卸载按钮 (qysudo -n)
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qyl10n.h"
#include "qytheme.h"
#include "qyicon.h"

#define REPO_INDEX "/usr/share/qyrepo/index.json"
#define REPO_DIR   "/usr/share/qyrepo/pkgs"
#define INST_DIR   "/var/lib/qypkg/installed"

enum { C_NAME, C_VER, C_SIZE, C_STATUS, C_FILE, C_N };

static GtkListStore *store;
static GtkWidget *info_label;
static GtkWidget *btn_act;
static GtkWidget *count_label;
static char g_file[512] = "", g_name[128] = "", g_ver[64] = "", g_status[32] = "";
static char g_filter[256] = "";      /* 当前搜索词 */
static int g_status_filter = 0;       /* 0=全部 1=已安装 2=可安装 */
static GtkWidget *b_all = NULL, *b_inst = NULL, *b_avail = NULL;
static GtkWidget *spinner = NULL;    /* 搜索/安装加载动画（release 256） */
static GtkWidget *list_stack = NULL; /* 列表 / 空状态 切换 */
static GtkWidget *empty_label = NULL; /* 空状态文字（未找到匹配的应用） */

/* 空状态插图（cairo，深蓝底 + 橙色放大镜） */
static gboolean empty_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    qy_draw_empty_state(cr, a.width, a.height, "search");
    return FALSE;
}

/* 根据当前列表是否为空，在「列表 / 空状态」之间切换 */
static void update_list_view(void) {
    if (!list_stack) return;
    int nrows = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), NULL);
    if (nrows == 0) {
        if (empty_label) gtk_label_set_text(GTK_LABEL(empty_label), TR("未找到匹配的应用"));
        gtk_stack_set_visible_child_name(GTK_STACK(list_stack), "empty");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(list_stack), "list");
    }
}

static int is_installed(const char *name) {
    char p[300];
    snprintf(p, sizeof p, INST_DIR "/%s", name);
    return access(p, F_OK) == 0;
}


/* 读包 meta json (QYPKG 128B 头: meta_off@16 meta_len@24) */
static char *pkg_meta(const char *file) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", REPO_DIR, file);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[128];
    if (fread(hdr, 1, 128, f) != 128 || memcmp(hdr, "QYPKG", 5) != 0) { fclose(f); return NULL; }
    unsigned long long mo = 0, ml = 0;
    for (int i = 7; i >= 0; i--) mo = (mo << 8) | hdr[16 + i];
    for (int i = 7; i >= 0; i--) ml = (ml << 8) | hdr[24 + i];
    if (ml > 4u << 20) { fclose(f); return NULL; }
    char *js = malloc(ml + 1);
    if (!js || fread(js, 1, ml, f) != ml) { fclose(f); free(js); return NULL; }
    js[ml] = 0;
    fclose(f);
    return js;
}
static int json_str(const char *js, const char *key, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(js, pat);
    if (!k) return -1;
    k = strchr(k + strlen(pat), ':');
    if (!k) return -1;
    while (*k && *k != '"') k++;
    if (*k != '"') return -1;
    k++;
    size_t i = 0;
    while (*k && *k != '"' && i + 1 < outsz) out[i++] = *k++;
    out[i] = 0;
    return 0;
}

/* 读取 JSON 数字字段（如 "size": 98209） */
static long json_num(const char *js, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(js, pat);
    if (!k) return -1;
    k = strchr(k + strlen(pat), ':');
    if (!k) return -1;
    k++;
    while (*k == ' ' || *k == '\t') k++;
    return atol(k);
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b || fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0;
    fclose(f);
    if (len) *len = n;
    return b;
}

static void human_size(long v, char *out, size_t n) {
    if (v >= 1048576) snprintf(out, n, "%.1f MB", v / 1048576.0);
    else if (v >= 1024) snprintf(out, n, "%ld KB", v / 1024);
    else snprintf(out, n, "%ld B", v);
}

/* 组装包详情多行文本: 名称-版本 / 描述 / 依赖 / 大小（release 283） */
static void pkg_detail_text(const char *name, const char *ver, const char *file,
                            char *out, size_t outsz) {
    char *js = pkg_meta(file ? file : "");
    if (js) {
        char desc[512] = "", deps[256] = "", sizestr[64] = "";
        json_str(js, "description", desc, sizeof desc);
        if (!desc[0]) json_str(js, "summary", desc, sizeof desc);
        long sz = json_num(js, "size");
        if (sz > 0) human_size(sz, sizestr, sizeof sizestr);
        /* depends 数组首段扫名字 */
        const char *dp = strstr(js, "\"depends\"");
        if (dp) {
            size_t j = 0;
            const char *q = strchr(dp, '[');
            if (q) {
                q++;
                while (*q && *q != ']' && j + 1 < sizeof deps) {
                    if (*q == '"') {
                        q++;
                        if (j && j + 1 < sizeof deps) deps[j++] = ',';
                        while (*q && *q != '"' && j + 1 < sizeof deps) deps[j++] = *q++;
                    }
                    q++;
                }
            }
            deps[j] = 0;
        }
        free(js);
        snprintf(out, outsz, "%s-%s\n%s\n%s: %s\n%s: %s",
                 name, ver ? ver : "", desc,
                 TR("依赖"), deps,
                 TR("大小"), sizestr[0] ? sizestr : "-");
    } else {
        snprintf(out, outsz, "%s-%s", name, ver ? ver : "");
    }
}


/* 在 store 中按包名查文件名, 找到返回 0 并填 out */
static int find_file_by_name(const char *name, char *out, size_t outsz) {
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it)) return -1;
    do {
        gchar *n = NULL, *f = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &it, C_NAME, &n, C_FILE, &f, -1);
        int hit = (n && strcmp(n, name) == 0);
        if (hit) snprintf(out, outsz, "%s", f ? f : "");
        g_free(n); g_free(f);
        if (hit) return 0;
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    return -1;
}

/* 组装安装命令: 依赖包在前, 目标包在后 (qypkg-inst 支持多包顺序安装) */
static void build_install_cmd(const char *file, char *out, size_t outsz) {
    char *js = pkg_meta(file);
    char deps[512] = "";
    if (js) {
        const char *dp = strstr(js, "\"depends\"");
        if (dp) {
            size_t j = 0;
            const char *q = strchr(dp, '[');
            if (q) {
                q++;
                while (*q && *q != ']' && j + 1 < sizeof deps) {
                    if (*q == '"') {
                        q++;
                        if (j && j + 1 < sizeof deps) deps[j++] = ' ';
                        while (*q && *q != '"' && j + 1 < sizeof deps) deps[j++] = *q++;
                    }
                    q++;
                }
            }
            deps[j] = 0;
        }
        free(js);
    }
    /* deps 是空格分隔包名 */
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "qysudo -n /usr/bin/qypkg-inst");
    char *save = NULL;
    for (char *t = strtok_r(deps, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        char df[256];
        if (is_installed(t)) continue;              /* 已装跳过 */
        if (find_file_by_name(t, df, sizeof df) != 0) continue;  /* 仓库无此包(如 glibc 系统自带) */
        strncat(cmd, " '", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, df, sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, "'", sizeof cmd - strlen(cmd) - 1);
    }
    char tail[600];
    snprintf(tail, sizeof tail, "/%s'", file);
    strncat(cmd, " '", sizeof cmd - strlen(cmd) - 1);
    strncat(cmd, REPO_DIR, sizeof cmd - strlen(cmd) - 1);
    strncat(cmd, tail, sizeof cmd - strlen(cmd) - 1);
    snprintf(out, outsz, "%s", cmd);
}

/* 状态栏: 软件包总数 / 已安装数 */
static void update_count_label(void) {
    if (!count_label) return;
    int total = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), NULL), inst = 0;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it)) {
        do {
            gchar *n = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &it, C_NAME, &n, -1);
            if (n && is_installed(n)) inst++;
            g_free(n);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    }
    char buf[128];
    snprintf(buf, sizeof buf, "%d %s / %d %s", total, TR("个软件包"), inst, TR("已安装"));
    gtk_label_set_text(GTK_LABEL(count_label), buf);
}

static void load_repo(const char *filter, int status_filter) {
    gtk_list_store_clear(store);
    size_t len = 0;
    char *js = slurp(REPO_INDEX, &len);
    if (!js) {
        gtk_list_store_insert_with_values(store, NULL, -1,
            C_NAME, TR("仓库缺失"), C_FILE, "", -1);
        return;
    }
    /* index.json: {"packagesTR(":[{...}]} 或 [...] — 找 ")filename" 条目逐包扫 */
    const char *p = js;
    while ((p = strstr(p, "\"filename\""))) {
        const char *q = strchr(p, ':');
        while (q && *q != '"') q++;
        if (!q) break;
        q++;
        char file[256]; size_t i = 0;
        while (*q && *q != '"' && i + 1 < sizeof file) file[i++] = *q++;
        file[i] = 0;
        p = q;
        /* 向前找本条目的 name/version/size: 在 filename 之前 600 字节窗口内 */
        const char *win_start = p - 600 < js ? js : p - 600;
        char name[128] = "", ver[64] = "";
        /* 简易: 在窗口内找最后一个 "name":"..." */
        const char *w = win_start, *last = NULL;
        while (w && w < p) {
            w = strstr(w, "\"name\"");
            if (w && w < p) { last = w; w += 6; }
        }
        if (last) {
            const char *k = strchr(last, ':');
            while (k && *k != '"') k++;
            if (k) { k++; size_t j = 0; while (*k && *k != '"' && j + 1 < sizeof name) name[j++] = *k++; }
        }
        last = NULL; w = win_start;
        while (w && w < p) {
            w = strstr(w, "\"version\"");
            if (w && w < p) { last = w; w += 9; }
        }
        if (last) {
            const char *k = strchr(last, ':');
            while (k && *k != '"') k++;
            if (k) { k++; size_t j = 0; while (*k && *k != '"' && j + 1 < sizeof ver) ver[j++] = *k++; }
        }
        long size = 0;
        w = win_start;
        while (w && w < p) {
            w = strstr(w, "\"size\"");
            if (w && w < p) { size = strtol(strchr(w, ':') + 1, NULL, 10); w += 6; }
        }
        if (!name[0]) continue;
        if (filter && filter[0] && !strstr(name, filter)) continue;
        if (status_filter == 1 && !is_installed(name)) continue;
        if (status_filter == 2 && is_installed(name)) continue;
        char sz[32], status[64];
        human_size(size, sz, sizeof sz);
        snprintf(status, sizeof status, "%s", is_installed(name) ? TR("OK 已安装") : TR("可安装"));
        gtk_list_store_insert_with_values(store, NULL, -1,
            C_NAME, name, C_VER, ver, C_SIZE, sz, C_STATUS, status,
            C_FILE, file, -1);
    }
    free(js);
    update_count_label();
    update_list_view();
}

static void refresh_row_status(void) {
    /* 重载仓库以刷新已装标记（保留当前搜索与状态筛选） */
    load_repo(g_filter, g_status_filter);
}

/* 刷新仓库按钮: 重载列表 + 状态栏短暂提示 */
static gboolean restore_count(gpointer p) {
    (void)p;
    update_count_label();
    return G_SOURCE_REMOVE;
}

static void on_refresh(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    refresh_row_status();
    if (count_label) gtk_label_set_text(GTK_LABEL(count_label), TR("仓库已刷新"));
    g_timeout_add_seconds(5, restore_count, NULL);
}

/* 自动化验证: QYSTORE_REFRESH=1 启动后自动刷新仓库 */
static gboolean on_refresh_auto(gpointer p) {
    (void)p;
    on_refresh(NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void show_row(GtkTreeView *tv, GtkTreeIter *itp);
static void detail_dialog(GtkWidget *parent);

/* 单击 / 方向键选择行时刷新底部详情卡（release 283） */
static void on_cursor_changed(GtkTreeView *tv, gpointer ud) {
    (void)ud;
    GtkTreePath *path = NULL;
    gtk_tree_view_get_cursor(tv, &path, NULL);
    if (!path) return;
    GtkTreeIter it;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    if (gtk_tree_model_get_iter(m, &it, path)) show_row(tv, &it);
    gtk_tree_path_free(path);
}

/* 双击行：刷新详情卡并打开包详情对话框（release 283） */
static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer ud) {
    (void)col; (void)ud;
    GtkTreeIter it;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    if (!gtk_tree_model_get_iter(m, &it, path)) return;
    show_row(tv, &it);
    detail_dialog(gtk_widget_get_toplevel(GTK_WIDGET(tv)));
}

/* 复制当前包名到剪贴板（release 277） */
static void on_copy_pkg_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!g_name[0]) return;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, g_name, -1);
}

static void show_row(GtkTreeView *tv, GtkTreeIter *itp) {
    GtkTreeIter it = *itp;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    gchar *name = NULL, *file = NULL, *ver = NULL;
    gtk_tree_model_get(m, &it, C_NAME, &name, C_VER, &ver, C_FILE, &file, -1);
    snprintf(g_name, sizeof g_name, "%s", name ? name : "");
    snprintf(g_ver, sizeof g_ver, "%s", ver ? ver : "");
    snprintf(g_file, sizeof g_file, "%s", file ? file : "");
    snprintf(g_status, sizeof g_status, "%s", is_installed(g_name) ? "installed" : "available");
    char txt[1024];
    pkg_detail_text(g_name, g_ver, g_file, txt, sizeof txt);
    {
        size_t L = strlen(txt);
        snprintf(txt + L, sizeof txt - L, "\n%s",
                 g_status[0] == 'i' ? TR("已安装，可卸载") : TR("未安装，可安装"));
    }
    gtk_label_set_text(GTK_LABEL(info_label), txt);
    gtk_button_set_label(GTK_BUTTON(btn_act),
        g_status[0] == 'i' ? TR("卸载") : TR("安装"));
    g_free(name); g_free(ver); g_free(file);
}

/* 包详情对话框（release 283）：双击行查看完整元数据 */
static void detail_dialog(GtkWidget *parent) {
    if (!g_name[0] || !g_file[0]) return;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        g_name, parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        TR("复制包名"), GTK_RESPONSE_APPLY,
        TR("关闭"), GTK_RESPONSE_CLOSE, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    char txt[2048];
    pkg_detail_text(g_name, g_ver, g_file, txt, sizeof txt);
    {
        size_t L = strlen(txt);
        snprintf(txt + L, sizeof txt - L, "\n%s: %s\n%s: %s",
                 TR("状态"), g_status[0] == 'i' ? TR("已安装") : TR("可安装"),
                 TR("路径"), g_file);
    }
    GtkWidget *label = gtk_label_new(txt);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(content), label, TRUE, TRUE, 0);
    gtk_widget_show_all(content);
    int ans = gtk_dialog_run(GTK_DIALOG(dlg));
    if (ans == GTK_RESPONSE_APPLY)
        on_copy_pkg_clicked(NULL, NULL);
    gtk_widget_destroy(dlg);
}

static void append_log(const char *msg) {
    char txt[1024];
    snprintf(txt, sizeof txt, "%s\n%s", gtk_label_get_text(GTK_LABEL(info_label)), msg);
    gtk_label_set_text(GTK_LABEL(info_label), txt);
}

static void on_open_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (g_status[0] != 'i') return;
    GError *err = NULL;
    if (!g_spawn_command_line_async(g_name, &err) && err) {
        g_printerr("qystore: 启动 %s 失败: %s\n", g_name, err->message);
        g_clear_error(&err);
    }
}

static void on_act(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!g_file[0]) return;
    char cmd[1024];
    int rc;
    if (g_status[0] == 'i') {
        /* 卸载确认（release 268） */
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(w)),
                                                GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_YES_NO,
                                                TR("确认卸载 %s?"), g_name);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s",
                                                 TR("该应用将从系统中移除。"));
        int ans = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (ans != GTK_RESPONSE_YES) return;
        snprintf(cmd, sizeof cmd, "qysudo -n /usr/bin/qypkg-inst -r %s", g_name);
    } else {
        build_install_cmd(g_file, cmd, sizeof cmd);
    }
    append_log(TR("执行中…"));
    /* 安装/卸载期间禁用操作按钮，防止并发操作（release 283） */
    gtk_widget_set_sensitive(btn_act, FALSE);
    gtk_button_set_label(GTK_BUTTON(btn_act), TR("执行中…"));
    if (spinner) {
        gtk_spinner_start(GTK_SPINNER(spinner));
        while (gtk_events_pending()) gtk_main_iteration();
    }
    rc = system(cmd);
    if (spinner) gtk_spinner_stop(GTK_SPINNER(spinner));
    char msg[128];
    snprintf(msg, sizeof msg, rc == 0 ? TR("完成 OK") : TR("失败 (rc=%d)"), rc);
    append_log(msg);
    snprintf(g_status, sizeof g_status, "%s", is_installed(g_name) ? "installed" : "available");
    gtk_widget_set_sensitive(btn_act, TRUE);
    gtk_button_set_label(GTK_BUTTON(btn_act), g_status[0] == 'i' ? TR("卸载") : TR("安装"));
    refresh_row_status();
}

static void on_search(GtkSearchEntry *e, gpointer ud) {
    (void)ud;
    g_strlcpy(g_filter, gtk_entry_get_text(GTK_ENTRY(e)), sizeof g_filter);
    if (spinner) {
        gtk_spinner_start(GTK_SPINNER(spinner));
        while (gtk_events_pending()) gtk_main_iteration();
    }
    load_repo(g_filter, g_status_filter);
    if (spinner) gtk_spinner_stop(GTK_SPINNER(spinner));
}

/* 状态筛选按钮：0 全部 / 1 已安装 / 2 可安装 */
static void on_filter_clicked(GtkWidget *w, gpointer ud) {
    g_status_filter = GPOINTER_TO_INT(ud);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_all), g_status_filter == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_inst), g_status_filter == 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_avail), g_status_filter == 2);
    load_repo(g_filter, g_status_filter);
}

/* 状态列颜色: 已安装绿 / 可安装橙 */
static void status_color_cb(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                            GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    (void)col; (void)data;
    gchar *name = NULL;
    gtk_tree_model_get(model, iter, C_NAME, &name, -1);
    g_object_set(renderer, "foreground",
                 (name && is_installed(name)) ? "#6ee7a0" : "#ffb38a", NULL);
    g_free(name);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫软件中心"));
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 480);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), TR("搜索软件包…"));
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search), NULL);
    spinner = gtk_spinner_new();
    gtk_widget_set_size_request(spinner, 16, 16);
    GtkWidget *b_refresh = gtk_button_new_with_label(TR("刷新"));
    qy_add_class(b_refresh, "qy-btn");
    g_signal_connect(b_refresh, "clicked", G_CALLBACK(on_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(search_row), search, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), b_refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), search_row, FALSE, FALSE, 0);

    /* 状态筛选行: 全部 / 已安装 / 可安装 */
    GtkWidget *filter_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b_all = gtk_toggle_button_new_with_label(TR("全部"));
    b_inst = gtk_toggle_button_new_with_label(TR("已安装"));
    b_avail = gtk_toggle_button_new_with_label(TR("可安装"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_all), TRUE);
    g_signal_connect(b_all, "clicked", G_CALLBACK(on_filter_clicked), GINT_TO_POINTER(0));
    g_signal_connect(b_inst, "clicked", G_CALLBACK(on_filter_clicked), GINT_TO_POINTER(1));
    g_signal_connect(b_avail, "clicked", G_CALLBACK(on_filter_clicked), GINT_TO_POINTER(2));
    gtk_box_pack_start(GTK_BOX(filter_row), b_all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter_row), b_inst, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter_row), b_avail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), filter_row, FALSE, FALSE, 0);

    store = gtk_list_store_new(C_N, G_TYPE_STRING, G_TYPE_STRING,
                               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw, TRUE);
    GtkTreeView *tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
    const char *titles[C_FILE] = { TR("名称"), TR("版本"), TR("大小"), TR("状态") };
    for (int c = 0; c < C_FILE - 1; c++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[c], r, "text", c, NULL);
        gtk_tree_view_append_column(tv, col);
    }
    /* 状态列: 已安装绿 / 可安装橙 */
    {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[C_FILE - 1], r, "text", C_STATUS, NULL);
        gtk_tree_view_column_set_cell_data_func(col, r, status_color_cb, NULL, NULL);
        gtk_tree_view_append_column(tv, col);
    }
    gtk_tree_view_set_headers_visible(tv, TRUE);
    g_signal_connect(tv, "cursor-changed", G_CALLBACK(on_cursor_changed), NULL);
    g_signal_connect(tv, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(tv));

    /* 空状态页：搜索无结果时显示 cairo 空状态插图 + 文字 */
    GtkWidget *empty_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *empty_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(empty_da, 260, 180);
    gtk_widget_set_halign(empty_da, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty_da, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(empty_da, TRUE);
    g_signal_connect(empty_da, "draw", G_CALLBACK(empty_draw_cb), NULL);
    empty_label = gtk_label_new(TR("未找到匹配的应用"));
    gtk_widget_set_halign(empty_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(empty_page), empty_da, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(empty_page), empty_label, FALSE, FALSE, 4);

    list_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(list_stack), sw, "list");
    gtk_stack_add_named(GTK_STACK(list_stack), empty_page, "empty");
    gtk_stack_set_visible_child_name(GTK_STACK(list_stack), "list");
    gtk_box_pack_start(GTK_BOX(vbox), list_stack, TRUE, TRUE, 0);

    count_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(count_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), count_label, FALSE, FALSE, 0);

    info_label = gtk_label_new(TR("选择一个软件包"));
    gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(info_label), TRUE);
    GtkWidget *card = gtk_event_box_new();
    qy_add_class(card, "qy-store-card");
    gtk_widget_set_margin_start(GTK_WIDGET(card), 2);
    gtk_widget_set_margin_end(GTK_WIDGET(card), 2);
    gtk_container_add(GTK_CONTAINER(card), info_label);
    gtk_box_pack_start(GTK_BOX(vbox), card, FALSE, FALSE, 0);

    /* 打开按钮（release 270）：已安装时启动应用 */
    GtkWidget *b_open = gtk_button_new_with_label(TR("打开"));
    g_signal_connect(b_open, "clicked", G_CALLBACK(on_open_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), b_open, FALSE, FALSE, 0);
    GtkWidget *b_copy = gtk_button_new_with_label(TR("复制包名"));
    qy_add_class(b_copy, "qy-btn");
    g_signal_connect(b_copy, "clicked", G_CALLBACK(on_copy_pkg_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), b_copy, FALSE, FALSE, 0);

    btn_act = gtk_button_new_with_label(TR("安装"));
    g_signal_connect(btn_act, "clicked", G_CALLBACK(on_act), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), btn_act, FALSE, FALSE, 0);

    load_repo(NULL, 0);
    /* 自动选中首行, 打开即显示详情 */
    {
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first))
            show_row(GTK_TREE_VIEW(tv), &first);
    }
    gtk_widget_show_all(win);

    /* 自动化验证: QYSTORE_REFRESH=1 启动后 2 秒自动刷新仓库 */
    if (getenv("QYSTORE_REFRESH"))
        g_timeout_add_seconds(2, on_refresh_auto, NULL);
    gtk_main();
    return 0;
}
