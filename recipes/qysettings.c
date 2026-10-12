/* qysettings - 澜岫系统设置 (GTK3) */
#include "qyl10n.h"
#include "qytheme.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

/* ---------- 语言页回调 ---------- */
/* 同步 weston 顶栏时钟格式（顶栏由 weston 补丁渲染，读 weston.ini 的
   clock-format-string，不经 qyl10n）：en → %m/%d %H:%M，zh → %m月%d日 %H:%M */
static void write_weston_clock(const char *fmt) {
    const char *ini = "/etc/xdg/weston/weston.ini";
    gchar *buf = NULL; gsize len = 0;
    if (!g_file_get_contents(ini, &buf, &len, NULL)) return;
    GString *s = g_string_new(NULL);
    gchar **lines = g_strsplit(buf, "\n", -1);
    gboolean replaced = FALSE;
    for (int i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "clock-format-string=")) {
            g_string_append_printf(s, "clock-format-string=%s\n", fmt);
            replaced = TRUE;
        } else {
            g_string_append(s, lines[i]);
            g_string_append_c(s, '\n');
        }
    }
    if (!replaced)
        g_string_append_printf(s, "[shell]\nclock-format-string=%s\n", fmt);
    g_file_set_contents(ini, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
    g_strfreev(lines);
    g_free(buf);
}

static void on_lang_zh(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    FILE *f = fopen("/etc/qylang", "w");
    if (f) { fputs("zh", f); fclose(f); }
    write_weston_clock("%m月%d日 %H:%M");
}
static void on_lang_en(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    FILE *f = fopen("/etc/qylang", "w");
    if (f) { fputs("en", f); fclose(f); }
    write_weston_clock("%m/%d %H:%M");
}


static gboolean vol_changed(GtkRange *r, gpointer ud);
static gboolean br_changed(GtkRange *r, gpointer ud);

static gchar *read_first_line(const char *path) {
    gchar *buf = NULL; gsize len = 0;
    if (g_file_get_contents(path, &buf, &len, NULL)) {
        gchar *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        gchar *s = g_strdup(buf);
        g_free(buf);
        return s;
    }
    return g_strdup(TR("未知"));
}

static gchar *read_cpu_model(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return g_strdup(TR("未知"));
    gchar *line = NULL;
    size_t cap = 0;
    ssize_t n;
    gchar *model = g_strdup(TR("未知"));
    while ((n = getline(&line, &cap, f)) != -1) {
        if (g_str_has_prefix(line, "model name")) {
            char *v = strchr(line, ':');
            if (v) {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                char *p = strchr(v, '\n');
                if (p) *p = 0;
                g_free(model);
                model = g_strdup(v);
            }
            break;
        }
    }
    g_free(line);
    fclose(f);
    return model;
}

static gchar *read_uptime(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return g_strdup("--");
    double up = 0;
    fscanf(f, "%lf", &up);
    fclose(f);
    int d = (int)up;
    return g_strdup_printf("%d%s %02d:%02d:%02d", d / 86400, TR("天"),
                            (d % 86400) / 3600, (d % 3600) / 60, d % 60);
}

static gchar *read_load(void) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return g_strdup("--");
    double l1 = 0, l5 = 0, l15 = 0;
    fscanf(f, "%lf %lf %lf", &l1, &l5, &l15);
    fclose(f);
    return g_strdup_printf("%.2f  %.2f  %.2f", l1, l5, l15);
}

static GtkWidget *row(const char *k, const char *v) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *kl = gtk_label_new(k);
    gtk_widget_set_size_request(kl, 150, -1);
    gtk_widget_set_halign(kl, GTK_ALIGN_START);
    GtkWidget *vl = gtk_label_new(v);
    gtk_widget_set_halign(vl, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(vl), TRUE);
    gtk_box_pack_start(GTK_BOX(h), kl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), vl, TRUE, TRUE, 0);
    return h;
}

/* 设置页分组标题: 在 row 列表开头插入一个加粗小标题（qy-settings-group） */
static GtkWidget *settings_group(GtkBox *box, const char *title) {
    GtkWidget *l = gtk_label_new(title);
    qy_add_class(l, "qy-settings-group");
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_box_pack_start(box, l, FALSE, FALSE, 0);
    return l;
}

/* 实验性功能提示（橙色小字）: 加在实验性页面顶部。
 * 本设置结构为左侧平铺标签页（无二级分组），因此按审计降级方案
 * 在每个实验性页面顶部加提示标签（.qy-exp-hint / #qy-settings-exp-hint）。 */
static void settings_exp_hint(GtkBox *box) {
    GtkWidget *h = gtk_label_new(TR("以下功能为实验性，可能不稳定"));
    gtk_widget_set_name(h, "qy-settings-exp-hint");
    gtk_widget_set_halign(h, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(h, 4);
    qy_add_class(h, "qy-exp-hint");
    gtk_box_pack_start(box, h, FALSE, FALSE, 0);
}

/* 从设置启动其他应用 */
static void launch_app(GtkButton *b, gpointer cmd) {
    char buf[128];
    g_snprintf(buf, sizeof buf, "%s &", (const char *)cmd);
    g_spawn_command_line_async(buf, NULL);
}

/* 应用桌面分辨率: 改写 /etc/xdg/weston/weston.ini 的 mode=，提示重启桌面 */
static void on_res_apply(GtkWidget *w, gpointer ud) {
    (void)w;
    GtkWidget *combo = GTK_WIDGET(ud);
    const char *sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (!sel || !*sel) return;
    const char *path = "/etc/xdg/weston/weston.ini";
    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) return;
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "mode=", 5) == 0) {
            g_string_append_printf(out, "mode=%s", sel);
            if (nl) g_string_append_c(out, '\n');
            replaced = 1;
        } else {
            g_string_append_len(out, line, len);
            if (nl) g_string_append_c(out, '\n');
        }
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "\n[output]\nname=Virtual-1\nmode=%s\n", sel);
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);

    /* 自动化验证(QY_SETTINGS_RES)不弹对话框，直接生效 */
    if (g_getenv("QY_SETTINGS_RES")) return;

    /* 交互模式: 提示重启桌面以应用分辨率 */
    GtkWidget *parent = gtk_widget_get_toplevel(combo);
    if (!GTK_IS_WINDOW(parent)) parent = NULL;
    GtkWidget *dlg = gtk_message_dialog_new(
        parent ? GTK_WINDOW(parent) : NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "%s", TR("分辨率已保存，重启桌面后生效。"));
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), TR("稍后"), GTK_RESPONSE_CANCEL,
                           TR("重启桌面"), GTK_RESPONSE_OK, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        g_spawn_command_line_async("/usr/bin/qyctl restart weston", NULL);
    }
    gtk_widget_destroy(dlg);
}

/* 自动化验证: QY_SETTINGS_RES=N 启动后自动应用分辨率 */
static gboolean auto_res_apply(gpointer p) {
    on_res_apply(NULL, p);
    return G_SOURCE_REMOVE;
}

/* 友好提示对话框（失败/完成提示，不崩溃） */
static void show_info_dialog(const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ---------- 主机名修改 ---------- */
/* 应用主机名: 有 hostnamectl 则调用之（并同步 /etc/hostname）；
 * 无 hostnamectl 则直接写 /etc/hostname 并提示重启后生效。失败给出友好提示。 */
static void on_host_apply(GtkWidget *w, gpointer ud) {
    (void)w;
    GtkWidget *entry = GTK_WIDGET(ud);
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!name || !name[0]) return;

    /* 主机名合法性: 不允许空白/斜杠/换行 */
    for (const char *p = name; *p; p++) {
        if (*p == ' ' || *p == '/' || *p == '\\' || *p == '\n' || *p == '\t') {
            show_info_dialog(TR("主机名不能包含空格或斜杠。"));
            return;
        }
    }

    char cur[256] = "";
    if (gethostname(cur, sizeof cur) != 0) cur[0] = 0;
    if (g_strcmp0(cur, name) == 0) {
        if (!g_getenv("QY_SETTINGS_HOST"))
            show_info_dialog(TR("主机名未变化。"));
        return;
    }

    gboolean ok = FALSE;
    if (g_find_program_in_path("hostnamectl")) {
        gchar *quoted = g_shell_quote(name);
        gchar *cmd = g_strdup_printf("hostnamectl set-hostname %s", quoted);
        g_free(quoted);
        ok = g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
        if (ok) {
            /* 同步写入 /etc/hostname，保证无 systemd 环境也生效 */
            g_file_set_contents("/etc/hostname", name, -1, NULL);
            if (!g_getenv("QY_SETTINGS_HOST"))
                show_info_dialog(TR("主机名已更新。"));
        }
    } else {
        ok = g_file_set_contents("/etc/hostname", name, -1, NULL);
        if (ok && !g_getenv("QY_SETTINGS_HOST"))
            show_info_dialog(TR("主机名已写入 /etc/hostname，重启后生效。"));
    }
    if (!ok && !g_getenv("QY_SETTINGS_HOST"))
        show_info_dialog(TR("设置主机名失败，请检查权限。"));
}

/* 自动化验证: QY_SETTINGS_HOST=<name> 启动后自动应用主机名 */
static gboolean auto_host_apply(gpointer p) {
    on_host_apply(NULL, p);
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* ---------- NTP 时间同步开关 ---------- */
static GtkWidget *ntp_sw = NULL;

/* 读取当前 NTP 状态: 优先 timedatectl show -p NTP；
 * 无 timedatectl 时回退 /etc/qysettings.conf 的 ntp= 与 /etc/systemd/timesyncd.conf */
static gboolean ntp_read_state(void) {
    if (g_find_program_in_path("timedatectl")) {
        gchar *out = NULL;
        if (g_spawn_command_line_sync("timedatectl show -p NTP", &out, NULL, NULL, NULL) && out) {
            gboolean yes = strstr(out, "NTP=yes") != NULL;
            g_free(out);
            return yes;
        }
    }
    gchar *content = NULL;
    if (g_file_get_contents("/etc/qysettings.conf", &content, NULL, NULL)) {
        gboolean yes = strstr(content, "ntp=1") != NULL;
        g_free(content);
        if (yes) return TRUE;
    }
    if (g_file_get_contents("/etc/systemd/timesyncd.conf", &content, NULL, NULL)) {
        gboolean yes = strstr(content, "NTP=") != NULL;
        g_free(content);
        return yes;
    }
    return FALSE;
}

/* NTP 开关回调: 有 timedatectl 调用 set-ntp；否则写 /etc/qysettings.conf 并提示。
 * 失败返回 TRUE 阻止开关状态改变，并弹出友好提示。 */
static gboolean on_ntp_state_set(GtkSwitch *sw, gboolean state, gpointer ud) {
    (void)sw; (void)ud;
    gboolean auto_mode = g_getenv("QY_SETTINGS_NTP") != NULL;
    gboolean ok = FALSE;

    if (g_find_program_in_path("timedatectl")) {
        gchar *cmd = g_strdup_printf("timedatectl set-ntp %s", state ? "true" : "false");
        gchar *out = NULL, *err = NULL;
        gint es = 0;
        gboolean spawned = g_spawn_command_line_sync(cmd, &out, &err, &es, NULL);
        g_free(out); g_free(err); g_free(cmd);
        /* 命令成功执行且退出码为 0 视为成功（避免 GLib 新旧 API 差异） */
        ok = spawned && es == 0;
        if (!ok && !auto_mode)
            show_info_dialog(TR("NTP 设置失败（timedatectl 不可用或权限不足）。"));
    } else {
        ok = g_file_set_contents("/etc/qysettings.conf",
                                 state ? "ntp=1\n" : "ntp=0\n", -1, NULL);
        if (ok && !auto_mode)
            show_info_dialog(TR("NTP 设置已保存到 /etc/qysettings.conf，重启后生效。"));
        else if (!ok && !auto_mode)
            show_info_dialog(TR("NTP 设置失败，无法写入 /etc/qysettings.conf。"));
    }
    return !ok;
}

/* 自动化验证: QY_SETTINGS_NTP=1/0 启动后自动切换 NTP。
 * 不屏蔽 state-set：让 on_ntp_state_set 真正执行 timedatectl set-ntp（auto_mode 下不弹窗）。 */
static gboolean auto_ntp_set(gpointer p) {
    gboolean want = GPOINTER_TO_INT(p) != 0;
    gtk_switch_set_active(GTK_SWITCH(ntp_sw), want);
    return G_SOURCE_REMOVE;
}

/* ---------- 夜间模式（显示页开关 → /etc/qysettings.conf night_mode=1/0） ---------- */
static GtkWidget *night_sw = NULL;

/* 在 /etc/qysettings.conf 中合并写入 key=value（保留其它键，避免覆盖 NTP 等设置） */
static gboolean qysettings_conf_write_key(const char *key, const char *value) {
    const char *path = "/etc/qysettings.conf";
    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL))
        content = g_strdup("");
    GString *out = g_string_new(NULL);
    char *line = content;
    size_t klen = strlen(key);
    gboolean replaced = FALSE;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0) {
            g_string_append_printf(out, "%s%s\n", key, value);
            replaced = TRUE;
        } else {
            g_string_append_len(out, line, len);
            if (nl) g_string_append_c(out, '\n');
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!replaced)
        g_string_append_printf(out, "%s%s\n", key, value);
    gboolean ok = g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_string_free(out, TRUE);
    g_free(content);
    return ok;
}

/* 夜间模式开关回调: 写 /etc/qysettings.conf night_mode=1/0 */
static gboolean on_night_state_set(GtkSwitch *sw, gboolean state, gpointer ud) {
    (void)sw; (void)ud;
    gboolean auto_mode = g_getenv("QY_SETTINGS_NIGHT") != NULL;
    gboolean ok = qysettings_conf_write_key("night_mode=", state ? "1" : "0");
    if (!ok && !auto_mode)
        show_info_dialog(TR("夜间模式设置失败，无法写入 /etc/qysettings.conf。"));
    return !ok;
}

/* 自动化验证: QY_SETTINGS_NIGHT=1/0 启动后自动切换夜间模式 */
static gboolean auto_night_set(gpointer p) {
    gboolean want = GPOINTER_TO_INT(p) != 0;
    gtk_switch_set_active(GTK_SWITCH(night_sw), want);
    return G_SOURCE_REMOVE;
}

/* ---------- 电源: 熄屏时间 ---------- */
static const int idle_secs[] = { 0, 60, 300, 600, 1800, 3600 };

static void on_idle_apply(GtkWidget *w, gpointer ud) {
    (void)w;
    int idx = GPOINTER_TO_INT(ud);
    int secs = idle_secs[idx];
    const char *path = "/etc/xdg/weston/weston.ini";
    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) return;
    GString *out = g_string_new(NULL);
    char *line = content;
    int in_core = 0, replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char line2[256];
        snprintf(line2, sizeof line2, "%.*s", (int)len, line);
        int is_core = strncmp(line2, "[core]", 6) == 0;
        if (is_core) {
            g_string_append_printf(out, "%s\nidle-time=%d", line2, secs);
            replaced = 1;
        } else if (in_core && strncmp(line2, "idle-time=", 11) == 0) {
            g_string_append_printf(out, "idle-time=%d", secs);
            replaced = 1;
        } else {
            g_string_append_len(out, line, len);
        }
        if (nl) g_string_append_c(out, '\n');
        in_core = is_core;
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_IDLE=N 秒启动后自动应用 */
static gboolean auto_idle_apply(gpointer p) {
    on_idle_apply(NULL, p);
    return G_SOURCE_REMOVE;
}

/* ---------- 隐私: 应用权限开关 ---------- */
#define PERM_CONF "/etc/qyperm.conf"
static int g_perm_loading = 0;

static const char *perm_read(const char *key, const char *def) {
    static char val[16];
    snprintf(val, sizeof val, "%s", def);
    FILE *f = fopen(PERM_CONF, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *eq = strchr(line, '=');
            if (eq && strncmp(line, key, strlen(key)) == 0 &&
                eq - line == (long)strlen(key)) {
                snprintf(val, sizeof val, "%s", eq + 1);
            }
        }
        fclose(f);
    }
    return val;
}

static void perm_write(const char *key, int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(PERM_CONF, &content, &len, NULL); /* 不存在则 content=NULL */
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, on ? "on" : "off");
    g_file_set_contents(PERM_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_perm_toggled(GtkWidget *sw, gpointer ud) {
    if (g_perm_loading) return;
    const char *key = (const char *)ud;
    perm_write(key, gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* 自动化验证: QY_SETTINGS_PERM=camera:off 启动后关闭对应开关 */
static gboolean auto_perm_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_PERM");
    if (env) {
        char key[64] = "", mode[16] = "";
        if (sscanf(env, "%63[^:]:%15s", key, mode) == 2) {
            perm_write(key, strcmp(mode, "off") != 0);
        }
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 主题色: 写 /etc/qytheme.conf ---------- */
#define THEME_CONF "/etc/qytheme.conf"
static GtkWidget *g_accent_combo = NULL;

static void accent_write(const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(THEME_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "accent=", 8) == 0) {
            g_string_append_printf(out, "accent=%s", val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "accent=%s\n", val);
    g_file_set_contents(THEME_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_accent_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *accents[] = { "orange", "purple", "blue", "green" };
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_accent_combo));
    if (act < 0) act = 0;
    if (act > 3) act = 3;
    accent_write(accents[act]);
    qy_load_theme();  /* 立即应用强调色 */
}

/* 自动化验证: QY_SETTINGS_ACCENT=purple 启动后写入 */
static gboolean auto_accent_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_ACCENT");
    if (env) {
        accent_write(env);
        qy_load_theme();
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 壁纸: 写 /etc/qywallpaper.conf ---------- */
#define WALLPAPER_CONF "/etc/qywallpaper.conf"
static GtkWidget *g_wallpaper_combo = NULL;

static void wallpaper_write(const char *name) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(WALLPAPER_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "wallpaper=", 11) == 0) {
            g_string_append_printf(out, "wallpaper=%s", name);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "wallpaper=%s\n", name);
    g_file_set_contents(WALLPAPER_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_wallpaper_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_wallpaper_combo));
    if (name && name[0]) wallpaper_write(name);
}

/* 自动化验证: QY_SETTINGS_WALLPAPER=lanxiu.png 启动后写入 */
static gboolean auto_wallpaper_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_WALLPAPER");
    if (env && env[0]) wallpaper_write(env);
    return G_SOURCE_REMOVE;
}

/* ---------- 窗口行为: 开关+动作写 /etc/qywinbehavior.conf ---------- */
#define WIN_CONF "/etc/qywinbehavior.conf"
static int g_win_loading = 0;
static GtkWidget *g_win_combo = NULL;

static void win_write(const char *key, const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(WIN_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, val);
    g_file_set_contents(WIN_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_win_toggled(GtkWidget *sw, gpointer ud) {
    if (g_win_loading) return;
    const char *key = (const char *)ud;
    win_write(key, gtk_switch_get_active(GTK_SWITCH(sw)) ? "on" : "off");
}

static void on_win_action_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *actions[] = { "maximize", "shade" };
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_win_combo));
    if (act < 0) act = 0;
    if (act > 1) act = 1;
    win_write("dblclick", actions[act]);
}

/* 自动化验证: QY_SETTINGS_WIN=drag:off 启动后写入 */
static gboolean auto_win_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_WIN");
    if (env) {
        char key[64] = "", mode[16] = "";
        if (sscanf(env, "%63[^:]:%15s", key, mode) == 2)
            win_write(key, strcmp(mode, "off") == 0 ? "off" : "on");
    }
    return G_SOURCE_REMOVE;
}

/* ---------- WiFi: 开关写 /etc/qywifi.conf ---------- */
#define WIFI_CONF "/etc/qywifi.conf"
static int g_wifi_loading = 0;

static void wifi_write(int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(WIFI_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "wifi=", 5) == 0) {
            g_string_append_printf(out, "wifi=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "wifi=%s\n", on ? "on" : "off");
    g_file_set_contents(WIFI_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_wifi_toggled(GtkWidget *sw, gpointer ud) {
    if (g_wifi_loading) return;
    (void)ud;
    wifi_write(gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* 自动化验证: QY_SETTINGS_WIFI=off 启动后关闭 */
static gboolean auto_wifi_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_WIFI");
    if (env) wifi_write(strcmp(env, "off") == 0 ? 0 : 1);
    return G_SOURCE_REMOVE;
}

/* ---------- 启动项: 自启应用开关 ---------- */
#define AUTOSTART_CONF "/etc/qyautostart.conf"

static void on_autostart_toggled(GtkWidget *sw, gpointer ud) {
    const char *app = (const char *)ud;
    int on = gtk_switch_get_active(GTK_SWITCH(sw));
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(AUTOSTART_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(app);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, app, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", app, on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", app, on ? "on" : "off");
    g_file_set_contents(AUTOSTART_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_AUTOSTART=qymon:off 启动后写入 */
static gboolean auto_autostart_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_AUTOSTART");
    if (env) {
        char app[64] = "", mode[16] = "";
        if (sscanf(env, "%63[^:]:%15s", app, mode) == 2) {
            GtkWidget *sw = gtk_switch_new();
            gtk_switch_set_active(GTK_SWITCH(sw), strcmp(mode, "off") != 0);
            on_autostart_toggled(sw, app);
            gtk_widget_destroy(sw);
        }
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 笔和Ink: 开关写 /etc/qypen.conf ---------- */
#define PEN_CONF "/etc/qypen.conf"
static int g_pen_loading = 0;

static void pen_write(const char *key, int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(PEN_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, on ? "on" : "off");
    g_file_set_contents(PEN_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_pen_toggled(GtkWidget *sw, gpointer ud) {
    if (g_pen_loading) return;
    const char *key = (const char *)ud;
    pen_write(key, gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* 自动化验证: QY_SETTINGS_PEN=off 启动后关闭 */
static gboolean auto_pen_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_PEN");
    if (env) pen_write("pen", strcmp(env, "off") != 0);
    return G_SOURCE_REMOVE;
}

/* ---------- 输入: 键盘布局 ---------- */
#define INPUT_CONF "/etc/qyinput.conf"
static GtkWidget *g_input_combo = NULL;

static void input_write(const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(INPUT_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "layout=", 8) == 0) {
            g_string_append_printf(out, "layout=%s", val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "layout=%s\n", val);
    g_file_set_contents(INPUT_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_input_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *layouts[] = { "us", "gb", "de", "fr" };
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
    if (act < 0) act = 0;
    if (act > 3) act = 3;
    input_write(layouts[act]);
}

/* 自动化验证: QY_SETTINGS_INPUT=layout:de 启动后写入 */
static gboolean auto_input_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_INPUT");
    if (env && strstr(env, "layout:")) {
        const char *layout = strstr(env, "layout:") + 7;
        if (*layout) input_write(layout);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 输入法: 无输入法 / fcitx5 / ibus ----------
 * 写入 /etc/profile.d/lanxiu-inputmethod.sh（GTK_IM_MODULE/QT_IM_MODULE/XMODIFIERS）。 */
#define IM_SCRIPT "/etc/profile.d/lanxiu-inputmethod.sh"
static GtkWidget *g_im_combo = NULL;

static void im_write(const char *im) {
    /* im: "none" 写空脚本；"fcitx5" 用 fcitx；"ibus" 用 ibus */
    if (!im || !strcmp(im, "none")) {
        g_file_set_contents(IM_SCRIPT, "", 0, NULL);
        return;
    }
    const char *module = strcmp(im, "ibus") == 0 ? "ibus" : "fcitx";
    gchar *content = g_strdup_printf(
        "#!/bin/sh\n"
        "export GTK_IM_MODULE=%s\n"
        "export QT_IM_MODULE=%s\n"
        "export XMODIFIERS=@im=%s\n",
        module, module, module);
    g_file_set_contents(IM_SCRIPT, content, -1, NULL);
    g_free(content);
}

/* 从已有脚本推断当前输入法: 0 无 / 1 fcitx5 / 2 ibus / -1 未知 */
static int im_current_from_file(void) {
    gchar *content = NULL;
    if (g_file_get_contents(IM_SCRIPT, &content, NULL, NULL) && content) {
        if (strstr(content, "GTK_IM_MODULE=ibus")) { g_free(content); return 2; }
        if (strstr(content, "GTK_IM_MODULE=fcitx")) { g_free(content); return 1; }
    }
    g_free(content);
    return -1;
}

/* 检测系统中已安装的输入法框架: 0 无 / 1 fcitx5 / 2 ibus */
static int im_detect_default(void) {
    gchar *p = g_find_program_in_path("fcitx5");
    if (p) { g_free(p); return 1; }
    p = g_find_program_in_path("ibus");
    if (p) { g_free(p); return 2; }
    return 0;
}

static void on_im_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_im_combo));
    if (act < 0) act = 0;
    if (act == 0) im_write("none");
    else if (act == 1) im_write("fcitx5");
    else im_write("ibus");
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(gtk_widget_get_toplevel(w)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%s", TR("已保存输入法配置，请重启后生效"));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* 自动化验证: QY_SETTINGS_IM=none|fcitx5|ibus 启动后写入 */
static gboolean auto_im_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_IM");
    if (env) {
        if (!strcmp(env, "fcitx5")) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), 1);
            im_write("fcitx5");
        } else if (!strcmp(env, "ibus")) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), 2);
            im_write("ibus");
        } else {
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), 0);
            im_write("none");
        }
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 按流量计费: 开关 ---------- */
#define METERED_CONF "/etc/qymetered.conf"
static int g_metered_loading = 0;

static void on_metered_toggled(GtkWidget *sw, gpointer ud) {
    if (g_metered_loading) return;
    (void)ud;
    int on = gtk_switch_get_active(GTK_SWITCH(sw));
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(METERED_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "metered=", 9) == 0) {
            g_string_append_printf(out, "metered=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "metered=%s\n", on ? "on" : "off");
    g_file_set_contents(METERED_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_METERED=on 启动后开启 */
static gboolean auto_metered_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_METERED");
    if (env) {
        GtkWidget *sw = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(sw), strcmp(env, "off") != 0);
        on_metered_toggled(sw, NULL);
        gtk_widget_destroy(sw);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 打印机: 添加请求写 /etc/qyprinter.conf ---------- */
#define PRINTER_CONF "/etc/qyprinter.conf"

static void printer_add_request(void) {
    GDateTime *dt = g_date_time_new_now_local();
    gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    gchar *log = g_strdup_printf("add_request=%s\n", ts);
    g_file_set_contents(PRINTER_CONF, log, -1, NULL);
    g_free(log);
    g_date_time_unref(dt);
}

static void on_printer_add(GtkWidget *w, gpointer ud) {
    printer_add_request();
    if (w) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(w)),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "%s", TR("已发送添加请求"));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
}

/* 自动化验证: QY_SETTINGS_PRINTER=add 启动后发送添加请求 */
static gboolean auto_printer_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_PRINTER");
    if (env && strcmp(env, "add") == 0) printer_add_request();
    return G_SOURCE_REMOVE;
}

/* ---------- HD Color: HDR + 颜色配置 ---------- */
#define HDR_CONF "/etc/qyhdr.conf"
static int g_hdr_loading = 0;
static GtkWidget *g_hdr_combo = NULL;

static void hdr_write(const char *key, const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(HDR_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, val);
    g_file_set_contents(HDR_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_hdr_toggled(GtkWidget *sw, gpointer ud) {
    if (g_hdr_loading) return;
    (void)ud;
    hdr_write("hdr", gtk_switch_get_active(GTK_SWITCH(sw)) ? "on" : "off");
}

static void on_hdr_profile_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *profiles[] = { "srgb", "p3", "vivid" };
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_hdr_combo));
    if (act < 0) act = 0;
    if (act > 2) act = 2;
    hdr_write("profile", profiles[act]);
}

/* 自动化验证: QY_SETTINGS_HDR=off 启动后关闭 */
static gboolean auto_hdr_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_HDR");
    if (env) hdr_write("hdr", strcmp(env, "off") == 0 ? "off" : "on");
    return G_SOURCE_REMOVE;
}

/* ---------- 投影: 模式写 /etc/qyproject.conf ---------- */
#define PROJECT_CONF "/etc/qyproject.conf"
static GtkWidget *g_proj_combo = NULL;

static void on_project_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *modes[] = { "pc_only", "mirror", "extend", "second_only" };
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_proj_combo));
    if (act < 0) act = 0;
    if (act > 3) act = 3;
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(PROJECT_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "mode=", 5) == 0) {
            g_string_append_printf(out, "mode=%s", modes[act]);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "mode=%s\n", modes[act]);
    g_file_set_contents(PROJECT_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_PROJECT=mode:extend 启动后写入 */
static gboolean auto_project_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_PROJECT");
    if (env && strstr(env, "mode:")) {
        const char *mode = strstr(env, "mode:") + 5;
        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(PROJECT_CONF, &content, &len, NULL);
        GString *out = g_string_new(NULL);
        if (content) g_string_append(out, content);
        g_free(content);
        if (strstr(out->str, "mode=")) {
            GString *tmp = g_string_new(NULL);
            char *line = out->str;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, "mode=", 5) == 0)
                    g_string_append_printf(tmp, "mode=%s", mode);
                else
                    g_string_append_len(tmp, line, llen);
                if (nl) g_string_append_c(tmp, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_string_free(out, TRUE);
            out = tmp;
        } else {
            g_string_append_printf(out, "mode=%s\n", mode);
        }
        g_file_set_contents(PROJECT_CONF, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 远程桌面: 开关写 /etc/qyremotedesktop.conf ---------- */
#define RDP_CONF "/etc/qyremotedesktop.conf"
static int g_rdp_loading = 0;

static void rdp_write(int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(RDP_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "rdp=", 4) == 0) {
            g_string_append_printf(out, "rdp=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "rdp=%s\n", on ? "on" : "off");
    g_file_set_contents(RDP_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_rdp_toggled(GtkWidget *sw, gpointer ud) {
    if (g_rdp_loading) return;
    (void)ud;
    rdp_write(gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* 自动化验证: QY_SETTINGS_RDP=off 启动后关闭 */
static gboolean auto_rdp_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_RDP");
    if (env) rdp_write(strcmp(env, "off") == 0 ? 0 : 1);
    return G_SOURCE_REMOVE;
}

/* ---------- 图形: 模式写 /etc/qygraphics.conf ---------- */
#define GRAPHICS_CONF "/etc/qygraphics.conf"
static GtkWidget *g_gfx_combo = NULL;

static void on_graphics_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *mode = "default";
    int act = gtk_combo_box_get_active(GTK_COMBO_BOX(g_gfx_combo));
    if (act == 1) mode = "performance";
    else if (act == 2) mode = "power_saver";
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(GRAPHICS_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "mode=", 5) == 0) {
            g_string_append_printf(out, "mode=%s", mode);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "mode=%s\n", mode);
    g_file_set_contents(GRAPHICS_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_GRAPHICS=mode:performance 启动后写入 */
static gboolean auto_graphics_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_GRAPHICS");
    if (env && strstr(env, "mode:")) {
        const char *mode = strstr(env, "mode:") + 5;
        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(GRAPHICS_CONF, &content, &len, NULL);
        GString *out = g_string_new(NULL);
        if (content) g_string_append(out, content);
        g_free(content);
        if (strstr(out->str, "mode=")) {
            GString *tmp = g_string_new(NULL);
            char *line = out->str;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, "mode=", 5) == 0)
                    g_string_append_printf(tmp, "mode=%s", mode);
                else
                    g_string_append_len(tmp, line, llen);
                if (nl) g_string_append_c(tmp, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_string_free(out, TRUE);
            out = tmp;
        } else {
            g_string_append_printf(out, "mode=%s\n", mode);
        }
        g_file_set_contents(GRAPHICS_CONF, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 触摸板: 开关+灵敏度 ---------- */
#define TOUCHPAD_CONF "/etc/qytouchpad.conf"
static int g_tp_loading = 0;
static GtkWidget *g_tp_scale = NULL;

static void touchpad_write(const char *key, const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(TOUCHPAD_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, val);
    g_file_set_contents(TOUCHPAD_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_touchpad_toggled(GtkWidget *sw, gpointer ud) {
    if (g_tp_loading) return;
    (void)ud;
    touchpad_write("touchpad", gtk_switch_get_active(GTK_SWITCH(sw)) ? "on" : "off");
}

static void on_touchpad_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    int v = (int)gtk_range_get_value(GTK_RANGE(g_tp_scale));
    gchar *s = g_strdup_printf("%d", v);
    touchpad_write("sensitivity", s);
    g_free(s);
}

/* 自动化验证: QY_SETTINGS_TOUCHPAD=off 启动后关闭 */
static gboolean auto_touchpad_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_TOUCHPAD");
    if (env) touchpad_write("touchpad", strcmp(env, "off") == 0 ? "off" : "on");
    return G_SOURCE_REMOVE;
}

/* ---------- 蓝牙: 开关写 /etc/qybluetooth.conf ---------- */
#define BT_CONF "/etc/qybluetooth.conf"
static int g_bt_loading = 0;

static void bt_write(int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(BT_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "bluetooth=", 11) == 0) {
            g_string_append_printf(out, "bluetooth=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "bluetooth=%s\n", on ? "on" : "off");
    g_file_set_contents(BT_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_bt_toggled(GtkWidget *sw, gpointer ud) {
    if (g_bt_loading) return;
    (void)ud;
    bt_write(gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* 自动化验证: QY_SETTINGS_BT=off 启动后关闭 */
static gboolean auto_bt_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_BT");
    if (env) bt_write(strcmp(env, "off") == 0 ? 0 : 1);
    return G_SOURCE_REMOVE;
}

/* ---------- 多显示器: 布局写 /etc/qydisplay.conf ---------- */
#define DISPLAY_CONF "/etc/qydisplay.conf"
static GtkWidget *g_disp_combo = NULL;

static void on_display_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *layout = gtk_combo_box_get_active(GTK_COMBO_BOX(g_disp_combo)) == 1 ? "mirror" : "extend";
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(DISPLAY_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "layout=", 8) == 0) {
            g_string_append_printf(out, "layout=%s", layout);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "layout=%s\n", layout);
    g_file_set_contents(DISPLAY_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_DISPLAY=layout:mirror 启动后写入 */
static gboolean auto_display_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_DISPLAY");
    if (env && strstr(env, "layout:mirror")) {
        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(DISPLAY_CONF, &content, &len, NULL);
        GString *out = g_string_new(NULL);
        if (content) g_string_append(out, content);
        g_free(content);
        if (strstr(out->str, "layout=")) {
            GString *tmp = g_string_new(NULL);
            char *line = out->str;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, "layout=", 8) == 0)
                    g_string_append(tmp, "layout=mirror");
                else
                    g_string_append_len(tmp, line, llen);
                if (nl) g_string_append_c(tmp, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_string_free(out, TRUE);
            out = tmp;
        } else {
            g_string_append(out, "layout=mirror\n");
        }
        g_file_set_contents(DISPLAY_CONF, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 网络重置: 记录日志 ---------- */
static void on_net_reset(GtkWidget *w, gpointer ud) {
    (void)ud;
    GDateTime *dt = g_date_time_new_now_local();
    gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    gchar *log = g_strdup_printf("network reset at %s\n", ts);
    g_file_set_contents("/etc/qynetreset.log", log, -1, NULL);
    g_free(log);
    g_date_time_unref(dt);
    if (w) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(w)),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "%s", TR("网络已重置"));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
}

/* 自动化验证: QY_SETTINGS_NETRESET=1 启动后重置 */
static gboolean auto_net_reset(gpointer p) {
    on_net_reset(NULL, p);
    return G_SOURCE_REMOVE;
}

/* ---------- 鼠标: 设置写 /etc/qymouse.conf ---------- */
#define MOUSE_CONF "/etc/qymouse.conf"
static GtkWidget *g_mouse_combo = NULL;
static GtkWidget *g_mouse_scale = NULL;
static GtkWidget *g_mouse_spin = NULL;

static void mouse_write(const char *key, const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(MOUSE_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, val);
    g_file_set_contents(MOUSE_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_mouse_apply(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    const char *primary = gtk_combo_box_get_active(GTK_COMBO_BOX(g_mouse_combo)) == 1 ? "right" : "left";
    mouse_write("primary", primary);
    int speed = (int)gtk_range_get_value(GTK_RANGE(g_mouse_scale));
    gchar *spd = g_strdup_printf("%d", speed);
    mouse_write("double_speed", spd);
    g_free(spd);
    int lines = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_mouse_spin));
    gchar *lins = g_strdup_printf("%d", lines);
    mouse_write("scroll_lines", lins);
    g_free(lins);
}

/* 自动化验证: QY_SETTINGS_MOUSE=primary:right 启动后写入 */
static gboolean auto_mouse_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_MOUSE");
    if (env) {
        char key[64] = "", mode[32] = "";
        if (sscanf(env, "%63[^:]:%31s", key, mode) == 2)
            mouse_write(key, mode);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 自动播放: 配置 ---------- */
#define AUTOPLAY_CONF "/etc/qyautoplay.conf"
static int g_autoplay_loading = 0;

static void autoplay_write(const char *key, const char *val) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(AUTOPLAY_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, val);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, val);
    g_file_set_contents(AUTOPLAY_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_autoplay_toggled(GtkWidget *sw, gpointer ud) {
    if (g_autoplay_loading) return;
    (void)ud;
    autoplay_write("autoplay", gtk_switch_get_active(GTK_SWITCH(sw)) ? "on" : "off");
}

static void on_autoplay_action(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *val = (const char *)ud;
    autoplay_write("action", val);
}

/* 自动化验证: QY_SETTINGS_AUTOPLAY=off 启动后关闭 */
static gboolean auto_autoplay_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_AUTOPLAY");
    if (env) {
        autoplay_write("autoplay", strcmp(env, "off") == 0 ? "off" : "on");
        autoplay_write("action", strcmp(env, "none") == 0 ? "none" : "open");
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 多任务: 开关 ---------- */
#define MULTI_CONF "/etc/qymultitask.conf"
static int g_multi_loading = 0;

static void multi_write(const char *key, int on) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(MULTI_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", key, on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, on ? "on" : "off");
    g_file_set_contents(MULTI_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_multi_toggled(GtkWidget *sw, gpointer ud) {
    if (g_multi_loading) return;
    const char *key = (const char *)ud;
    multi_write(key, gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* 自动化验证: QY_SETTINGS_MULTI=snap:off 启动后写入 */
static gboolean auto_multi_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_MULTI");
    if (env) {
        char key[64] = "", mode[16] = "";
        if (sscanf(env, "%63[^:]:%15s", key, mode) == 2)
            multi_write(key, strcmp(mode, "off") != 0);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 代理: 写入 /etc/environment ---------- */
#define ENV_CONF "/etc/environment"

static void proxy_write_env(const char *key, const char *value) {
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(ENV_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(key);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, key, klen) == 0) {
            g_string_append_printf(out, "%s=%s", key, value);
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", key, value);
    g_file_set_contents(ENV_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

static void on_proxy_apply(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *url = (const char *)ud;
    if (!url || !*url) return;
    proxy_write_env("http_proxy", url);
    proxy_write_env("https_proxy", url);
    proxy_write_env("HTTP_PROXY", url);
    proxy_write_env("HTTPS_PROXY", url);
}

/* 自动化验证: QY_SETTINGS_PROXY=http://host:port 启动后写入 */
static gboolean auto_proxy_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_PROXY");
    if (env) {
        proxy_write_env("http_proxy", env);
        proxy_write_env("https_proxy", env);
        proxy_write_env("HTTP_PROXY", env);
        proxy_write_env("HTTPS_PROXY", env);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 防火墙: 开关 ---------- */
#define FIREWALL_CONF "/etc/qyfirewall.conf"

static void on_firewall_toggled(GtkWidget *sw, gpointer ud) {
    (void)ud;
    int on = gtk_switch_get_active(GTK_SWITCH(sw));
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(FIREWALL_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "firewall=", 10) == 0) {
            g_string_append_printf(out, "firewall=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "firewall=%s\n", on ? "on" : "off");
    g_file_set_contents(FIREWALL_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_FIREWALL=on 启动后开启 */
static gboolean auto_firewall_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_FIREWALL");
    if (env) {
        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(FIREWALL_CONF, &content, &len, NULL);
        GString *out = g_string_new(NULL);
        if (content) g_string_append(out, content);
        g_free(content);
        if (strstr(out->str, "firewall=")) {
            GString *tmp = g_string_new(NULL);
            char *line = out->str;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, "firewall=", 10) == 0)
                    g_string_append_printf(tmp, "firewall=%s", strcmp(env, "on") == 0 ? "on" : "off");
                else
                    g_string_append_len(tmp, line, llen);
                if (nl) g_string_append_c(tmp, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_string_free(out, TRUE);
            out = tmp;
        } else {
            g_string_append_printf(out, "firewall=%s\n", strcmp(env, "on") == 0 ? "on" : "off");
        }
        g_file_set_contents(FIREWALL_CONF, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- SSH 服务: 开关（写 /etc/qyssh.conf，qynotifd 启动/停止 sshd） ---------- */
#define SSH_CONF "/etc/qyssh.conf"

static void on_sshd_toggled(GtkWidget *sw, gpointer ud) {
    (void)ud;
    int on = gtk_switch_get_active(GTK_SWITCH(sw));
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(SSH_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "sshd=", 5) == 0) {
            g_string_append_printf(out, "sshd=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "sshd=%s\n", on ? "on" : "off");
    g_file_set_contents(SSH_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_SSHD=on 启动后开启 */
static gboolean auto_sshd_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_SSHD");
    if (env) {
        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(SSH_CONF, &content, &len, NULL);
        GString *out = g_string_new(NULL);
        if (content) g_string_append(out, content);
        g_free(content);
        if (strstr(out->str, "sshd=")) {
            GString *tmp = g_string_new(NULL);
            char *line = out->str;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, "sshd=", 5) == 0)
                    g_string_append_printf(tmp, "sshd=%s", strcmp(env, "on") == 0 ? "on" : "off");
                else
                    g_string_append_len(tmp, line, llen);
                if (nl) g_string_append_c(tmp, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_string_free(out, TRUE);
            out = tmp;
        } else {
            g_string_append_printf(out, "sshd=%s\n", strcmp(env, "on") == 0 ? "on" : "off");
        }
        g_file_set_contents(SSH_CONF, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 恢复默认设置：删除全部 qy*.conf ---------- */
static void do_reset_settings(void) {
    static const char *files[] = {
        "/etc/qywallpaper.conf", "/etc/qynotif.conf", "/etc/qyfirewall.conf",
        "/etc/qyssh.conf", "/etc/qywifi.conf", "/etc/qybluetooth.conf",
        "/etc/qydisplay.conf", "/etc/qyhdr.conf", "/etc/qygraphics.conf",
        "/etc/qypen.conf"
    };
    int removed = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++)
        if (g_remove(files[i]) == 0) removed++;
    g_printerr("QYSETTINGSDBG: reset done (%d files removed)\n", removed);
}

static void on_reset_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    do_reset_settings();
}

/* 自动化验证: QY_SETTINGS_RESET=1 启动后恢复默认设置 */
static gboolean auto_reset_settings(gpointer p) {
    (void)p;
    do_reset_settings();
    return G_SOURCE_REMOVE;
}

/* ---------- 时区设置：写 /etc/timezone + 软链 /etc/localtime ---------- */
static void apply_timezone(const char *tz) {
    if (!tz || !tz[0]) return;
    g_file_set_contents("/etc/timezone", tz, -1, NULL);
    gchar *zone = g_strdup_printf("/usr/share/zoneinfo/%s", tz);
    gchar *cmd = g_strdup_printf("ln -sf %s /etc/localtime", zone);
    g_spawn_command_line_async(cmd, NULL);
    g_printerr("QYSETTINGSDBG: timezone=%s\n", tz);
    g_free(zone);
    g_free(cmd);
}

static void on_tz_changed(GtkComboBox *combo, gpointer ud) {
    (void)ud;
    const char *tz = gtk_combo_box_get_active_id(combo);
    apply_timezone(tz);
}

/* 自动化验证: QY_SETTINGS_TZ=UTC 启动后切换时区 */
static gboolean auto_tz_apply(gpointer p) {
    const char *tz = (const char *)p;
    apply_timezone(tz);
    return G_SOURCE_REMOVE;
}

/* ---------- 通知: 应用通知开关 ---------- */
#define NOTIF_CONF "/etc/qynotif.conf"
static int g_notif_loading = 0;

static void on_notif_toggled(GtkWidget *sw, gpointer ud) {
    if (g_notif_loading) return;
    const char *app = (const char *)ud;
    int on = gtk_switch_get_active(GTK_SWITCH(sw));
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(NOTIF_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    size_t klen = strlen(app);
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, app, klen) == 0 && line[klen] == '=') {
            g_string_append_printf(out, "%s=%s", app, on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "%s=%s\n", app, on ? "on" : "off");
    g_file_set_contents(NOTIF_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_NOTIF=qymon:off 启动后关闭对应应用通知 */
static gboolean auto_notif_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_NOTIF");
    if (env) {
        char app[64] = "", mode[16] = "";
        if (sscanf(env, "%63[^:]:%15s", app, mode) == 2) {
            gchar *content = NULL;
            gsize len = 0;
            g_file_get_contents(NOTIF_CONF, &content, &len, NULL);
            GString *out = g_string_new(NULL);
            char *line = content;
            int replaced = 0;
            size_t klen = strlen(app);
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, app, klen) == 0 && line[klen] == '=') {
                    g_string_append_printf(out, "%s=%s", app, strcmp(mode, "off") != 0 ? "on" : "off");
                    replaced = 1;
                } else {
                    g_string_append_len(out, line, llen);
                }
                if (nl) g_string_append_c(out, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
            if (!replaced)
                g_string_append_printf(out, "%s=%s\n", app, strcmp(mode, "off") != 0 ? "on" : "off");
            g_file_set_contents(NOTIF_CONF, out->str, out->len, NULL);
            g_string_free(out, TRUE);
        }
    }
    return G_SOURCE_REMOVE;
}

/* ---------- 应用页: 启动已安装应用 ---------- */
static void on_app_launch(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *cmd = (const char *)ud;
    gchar *full = g_strdup_printf("/usr/bin/%s", cmd);
    g_spawn_command_line_async(full, NULL);
    g_free(full);
}

/* ---------- 专注助手: 免打扰模式 ---------- */
#define FOCUS_CONF "/etc/qyfocus.conf"

static void on_focus_toggled(GtkWidget *sw, gpointer ud) {
    (void)ud;
    int on = gtk_switch_get_active(GTK_SWITCH(sw));
    gchar *content = NULL;
    gsize len = 0;
    g_file_get_contents(FOCUS_CONF, &content, &len, NULL);
    GString *out = g_string_new(NULL);
    char *line = content;
    int replaced = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (strncmp(line, "dnd=", 4) == 0) {
            g_string_append_printf(out, "dnd=%s", on ? "on" : "off");
            replaced = 1;
        } else {
            g_string_append_len(out, line, llen);
        }
        if (nl) g_string_append_c(out, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    if (!replaced)
        g_string_append_printf(out, "dnd=%s\n", on ? "on" : "off");
    g_file_set_contents(FOCUS_CONF, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* 自动化验证: QY_SETTINGS_FOCUS=on 启动后开启免打扰 */
static gboolean auto_focus_apply(gpointer p) {
    (void)p;
    const char *env = g_getenv("QY_SETTINGS_FOCUS");
    if (env) {
        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(FOCUS_CONF, &content, &len, NULL);
        GString *out = g_string_new(NULL);
        if (content) g_string_append(out, content);
        g_free(content);
        if (strstr(out->str, "dnd=")) {
            GString *tmp = g_string_new(NULL);
            char *line = out->str;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                size_t llen = nl ? (size_t)(nl - line) : strlen(line);
                if (strncmp(line, "dnd=", 4) == 0)
                    g_string_append_printf(tmp, "dnd=%s", strcmp(env, "on") == 0 ? "on" : "off");
                else
                    g_string_append_len(tmp, line, llen);
                if (nl) g_string_append_c(tmp, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_string_free(out, TRUE);
            out = tmp;
        } else {
            g_string_append_printf(out, "dnd=%s\n", strcmp(env, "on") == 0 ? "on" : "off");
        }
        g_file_set_contents(FOCUS_CONF, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* 日期时间标签每秒刷新 */
static gboolean update_dt(gpointer p)
{
    GtkWidget *l = (GtkWidget *)p;
    if (!l) return G_SOURCE_REMOVE;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return G_SOURCE_CONTINUE;
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", tm);
    gtk_label_set_text(GTK_LABEL(l), buf);
    return G_SOURCE_CONTINUE;
}

/* Ctrl+1~9 直达分类页（accelerator 回调，nb 作为 user_data） */
static void on_accel_page(GtkAccelGroup *ag, GObject *a, guint keyval,
                          GdkModifierType mod, gpointer ud) {
    (void)ag; (void)a; (void)mod;
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9)
        gtk_notebook_set_current_page(GTK_NOTEBOOK((GtkWidget *)ud), keyval - GDK_KEY_1);
}

/* Ctrl+W 关闭设置窗口 */
static void on_accel_close(GtkAccelGroup *ag, GObject *a, guint keyval,
                           GdkModifierType mod, gpointer ud) {
    (void)ag; (void)a; (void)keyval; (void)mod;
    gtk_widget_destroy(GTK_WIDGET(ud));
}

/* 带图标的左侧导航 tab：小图标 + 标题（qy_icon_pixbuf 线稿） */
static GtkWidget *tab_with_icon(const char *title, QyIconId icon) {
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new_from_pixbuf(qy_icon_pixbuf(icon, 16, NULL));
    GtkWidget *lbl = gtk_label_new(title);
    gtk_box_pack_start(GTK_BOX(hb), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), lbl, FALSE, FALSE, 0);
    gtk_widget_show_all(hb);
    return hb;
}

/* 从 tab 控件取标题文本（兼容「图标+文字」的 hbox tab） */
static const char *tab_label_text(GtkWidget *w) {
    if (!w) return NULL;
    if (GTK_IS_LABEL(w))
        return gtk_label_get_text(GTK_LABEL(w));
    if (GTK_IS_BOX(w)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        const char *t = NULL;
        for (GList *l = kids; l; l = l->next) {
            if (GTK_IS_LABEL(l->data)) {
                t = gtk_label_get_text(GTK_LABEL(l->data));
                break;
            }
        }
        g_list_free(kids);
        return t;
    }
    return NULL;
}

/* ---------- 字体页（release 256: 交互化） ---------- */
static GtkWidget *font_combo = NULL;
static GtkWidget *font_size_spin = NULL;
static GtkWidget *font_preview = NULL;

/* 无 fc-list 时回退的内置常见字体列表 */
static const char *font_builtin_list[] = {
    "Sans", "Sans Serif", "Monospace", "Noto Sans CJK SC",
    "Noto Serif CJK SC", "Noto Sans", "Noto Serif",
    "DejaVu Sans", "DejaVu Serif", "DejaVu Sans Mono",
    "WenQuanYi Micro Hei", "Droid Sans Fallback",
    "Liberation Sans", "Liberation Serif", "Liberation Mono",
    "FreeSans", "FreeSerif", "FreeMono",
    "Nimbus Sans", "Noto Color Emoji",
    NULL
};

static void font_add_unique(GPtrArray *arr, const char *name) {
    if (!name || !name[0]) return;
    for (guint i = 0; i < arr->len; i++) {
        if (g_strcmp0((const char *)g_ptr_array_index(arr, i), name) == 0) return;
    }
    g_ptr_array_add(arr, g_strdup(name));
}

/* 用 fc-list 填充字体下拉（前 20 个常见字体）；无 fc-list 时回退内置列表 */
static void font_fill_combo(void) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(font_combo));
    GPtrArray *fonts = g_ptr_array_new_with_free_func(g_free);
    /* 首选通用/常见字体，保证 Sans/Sans Serif/Monospace/Noto CJK 一定在列表 */
    const char *essential[] = {"Sans", "Sans Serif", "Monospace", "Noto Sans CJK SC", NULL};
    for (int i = 0; essential[i]; i++)
        font_add_unique(fonts, essential[i]);

    gchar *out = NULL;
    GError *err = NULL;
    if (g_spawn_command_line_sync("fc-list : family", &out, NULL, NULL, &err) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i] && (int)fonts->len < 20; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!line[0]) continue;
            /* 一行可能含多个族名（逗号分隔） */
            gchar **parts = g_strsplit(line, ",", -1);
            for (int j = 0; parts[j] && (int)fonts->len < 20; j++) {
                gchar *part = g_strstrip(parts[j]);
                if (part[0]) font_add_unique(fonts, part);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
    }
    g_free(out);
    g_clear_error(&err);

    /* 不足 20 个时用内置常见字体补足（fc-list 缺失时即回退整份内置列表） */
    for (int i = 0; font_builtin_list[i] && (int)fonts->len < 20; i++)
        font_add_unique(fonts, font_builtin_list[i]);

    for (guint i = 0; i < fonts->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font_combo),
                                       (const char *)g_ptr_array_index(fonts, i));
    g_ptr_array_free(fonts, TRUE);
}

/* 更新预览标签：所选字体族 + 字号 */
static void font_update_preview(void) {
    if (!font_preview) return;
    const char *family = "Sans";
    gchar *t = font_combo ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(font_combo)) : NULL;
    if (t && t[0]) family = t;
    int size = 11;
    if (font_size_spin) size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(font_size_spin));
    gchar *esc = g_markup_escape_text(family, -1);
    gchar *markup = g_strdup_printf(
        "<span font_family='%s' size='%d'>%s</span>", esc, size * 1024, TR("澜岫 LANXIU 预览"));
    gtk_label_set_markup(GTK_LABEL(font_preview), markup);
    g_free(markup);
    g_free(esc);
    g_free(t);
}

static void on_font_combo_changed(GtkComboBox *cb, gpointer ud) {
    (void)cb; (void)ud;
    font_update_preview();
}

static void on_font_size_changed(GtkSpinButton *sb, gpointer ud) {
    (void)sb; (void)ud;
    font_update_preview();
}

/* 读取已有 ~/.config/lanxiu/font.conf 作为默认值 */
static void font_load_current(void) {
    gchar *path = g_build_filename(g_get_home_dir(), ".config/lanxiu/font.conf", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (g_str_has_prefix(line, "font=")) {
                const char *f = line + 5;
                GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(font_combo));
                GtkTreeIter it;
                if (gtk_tree_model_get_iter_first(m, &it)) {
                    do {
                        gchar *name = NULL;
                        gtk_tree_model_get(m, &it, 0, &name, -1);
                        if (name && !strcmp(name, f)) {
                            gtk_combo_box_set_active_iter(GTK_COMBO_BOX(font_combo), &it);
                            g_free(name);
                            break;
                        }
                        g_free(name);
                    } while (gtk_tree_model_iter_next(m, &it));
                }
            } else if (g_str_has_prefix(line, "size=")) {
                int sz = atoi(line + 5);
                if (sz >= 9 && sz <= 18)
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(font_size_spin), sz);
            }
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(font_combo)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(font_combo), 0);
}

/* 应用字体设置：写 ~/.config/lanxiu/font.conf */
static void on_font_apply(GtkButton *b, gpointer ud) {
    (void)ud;
    const char *family = "Sans";
    gchar *t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(font_combo));
    if (t && t[0]) family = t;
    int size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(font_size_spin));
    gchar *dir = g_build_filename(g_get_home_dir(), ".config/lanxiu", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, "font.conf", NULL);
    gchar *content = g_strdup_printf("font=%s\nsize=%d\n", family, size);
    g_file_set_contents(path, content, -1, NULL);
    g_free(content);
    g_free(path);
    g_free(dir);
    g_free(t);

    /* 自动化验证: QY_SETTINGS_FONT=1 不弹对话框 */
    if (g_getenv("QY_SETTINGS_FONT")) return;

    GtkWidget *parent = gtk_widget_get_toplevel(GTK_WIDGET(b));
    if (!GTK_IS_WINDOW(parent)) parent = NULL;
    GtkWidget *dlg = gtk_message_dialog_new(
        parent ? GTK_WINDOW(parent) : NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%s", TR("字体设置已保存，重启桌面后生效。"));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* 设置页搜索：输入关键词，切换标签名包含关键词的第一个页面 */
static void on_settings_search(GtkSearchEntry *se, gpointer ud) {
    const char *needle = gtk_entry_get_text(GTK_ENTRY(se));
    GtkWidget *nb = (GtkWidget *)ud;
    if (!needle || !needle[0]) return;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), i);
        GtkWidget *lbl = gtk_notebook_get_tab_label(GTK_NOTEBOOK(nb), page);
        if (!lbl) continue;
        const char *title = tab_label_text(lbl);
        if (title && g_strrstr(title, needle)) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), i);
            g_printerr("QYSETTINGSSEARCH: %s -> %s\n", needle, title);
            return;
        }
    }
    g_printerr("QYSETTINGSSEARCH: %s -> (无匹配)\n", needle);
}

static void activate(GtkApplication *app, gpointer ud) {
    qy_load_theme();
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫系统设置"));
    qy_make_titlebar(GTK_WINDOW(win), TR("澜岫系统设置"));
    qy_window_setup(GTK_WINDOW(win), 620, 420, 560, 420, FALSE);

    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_set_name(nb, "qysettings-notebook");
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_LEFT);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(win), vbox);
    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), TR("搜索设置项（如 声音/分辨率/语言）"));
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_settings_search), nb);
    gtk_box_pack_start(GTK_BOX(vbox), search_entry, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), nb, TRUE, TRUE, 0);

    /* 关于本机 */
    GtkWidget *v1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v1), 14);
    settings_group(GTK_BOX(v1), TR("关于设置"));
    struct utsname u;
    uname(&u);
    struct sysinfo si;
    sysinfo(&si);
    gchar *mem = g_strdup_printf("%.1f MB", si.totalram / 1024.0 / 1024.0);
    gchar *osrel = read_first_line("/etc/lanxiu-release");
    gchar *deskver = read_first_line("/usr/share/qydesktop-version");
    gchar *cpumodel = read_cpu_model();
    gchar *cpucores = g_strdup_printf("%ld", sysconf(_SC_NPROCESSORS_ONLN));
    gchar *uptime = read_uptime();
    gchar *load = read_load();
    GtkWidget *logo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(logo), "<span size='xx-large' weight='bold'>澜岫 LANXIU</span>");
    gtk_widget_set_halign(logo, GTK_ALIGN_CENTER);
    qy_add_class(logo, "qy-about-logo");
    gtk_box_pack_start(GTK_BOX(v1), logo, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("操作系统"), osrel), FALSE, FALSE, 0);
    {
        gchar *platform = g_strdup_printf("澜岫 Linux %s", u.machine);
        gtk_box_pack_start(GTK_BOX(v1), row(TR("系统平台"), platform), FALSE, FALSE, 0);
        g_free(platform);
    }
    gtk_box_pack_start(GTK_BOX(v1), row(TR("内核版本"), u.release), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("处理器架构"), u.machine), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("CPU 型号"), cpumodel), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("CPU 核心数"), cpucores), FALSE, FALSE, 0);
    /* 主机名: 当前值显示 + 输入框 + 应用按钮（#qysettings-host-apply） */
    {
        char hostbuf[256] = "";
        if (gethostname(hostbuf, sizeof hostbuf) != 0) g_strlcpy(hostbuf, u.nodename, sizeof hostbuf);
        GtkWidget *host_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *host_k = gtk_label_new(TR("主机名"));
        gtk_widget_set_size_request(host_k, 150, -1);
        gtk_widget_set_halign(host_k, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(host_row), host_k, FALSE, FALSE, 0);
        GtkWidget *host_cur = gtk_label_new(hostbuf);
        gtk_widget_set_halign(host_cur, GTK_ALIGN_START);
        gtk_label_set_selectable(GTK_LABEL(host_cur), TRUE);
        gtk_box_pack_start(GTK_BOX(host_row), host_cur, FALSE, FALSE, 0);
        GtkWidget *host_entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(host_entry), hostbuf);
        gtk_widget_set_size_request(host_entry, 160, -1);
        gtk_box_pack_start(GTK_BOX(host_row), host_entry, FALSE, FALSE, 0);
        GtkWidget *b_host = gtk_button_new_with_label(TR("应用"));
        gtk_widget_set_name(b_host, "qysettings-host-apply");
        qy_add_class(b_host, "qy-btn");
        g_signal_connect(b_host, "clicked", G_CALLBACK(on_host_apply), host_entry);
        gtk_box_pack_start(GTK_BOX(host_row), b_host, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v1), host_row, FALSE, FALSE, 0);
        /* 自动化验证: QY_SETTINGS_HOST=<name> 启动后自动应用主机名 */
        const char *env_host = g_getenv("QY_SETTINGS_HOST");
        if (env_host && env_host[0]) {
            gtk_entry_set_text(GTK_ENTRY(host_entry), env_host);
            g_timeout_add(600, auto_host_apply, g_strdup(env_host));
        }
    }
    gtk_box_pack_start(GTK_BOX(v1), row(TR("内存总量"), mem), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("运行时间"), uptime), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("负载均值"), load), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("桌面环境"), "qydesktop (GTK3)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("桌面版本"), deskver ? deskver : TR("未知")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), row(TR("显示协议"), "Wayland (weston)"), FALSE, FALSE, 0);
    g_free(mem); g_free(osrel); g_free(deskver); g_free(cpumodel); g_free(cpucores);
    g_free(uptime); g_free(load);

    /* 关于页快捷启动 */
    GtkWidget *btn_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *b_mon = gtk_button_new_with_label(TR("打开系统监视"));
    qy_add_class(b_mon, "qy-about-btn");
    g_signal_connect(b_mon, "clicked", G_CALLBACK(launch_app), (gpointer)"qymon");
    GtkWidget *b_store = gtk_button_new_with_label(TR("打开软件中心"));
    qy_add_class(b_store, "qy-about-btn");
    g_signal_connect(b_store, "clicked", G_CALLBACK(launch_app), (gpointer)"qystore");
    GtkWidget *b_files = gtk_button_new_with_label(TR("打开文件管理器"));
    qy_add_class(b_files, "qy-about-btn");
    g_signal_connect(b_files, "clicked", G_CALLBACK(launch_app), (gpointer)"qyfiles");
    gtk_box_pack_start(GTK_BOX(btn_hb), b_mon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_hb), b_store, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_hb), b_files, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v1), btn_hb, FALSE, FALSE, 10);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), v1, tab_with_icon(TR("关于"), QY_ICON_SETTINGS));

    /* 显示 */
    GtkWidget *v2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v2), 14);
    settings_group(GTK_BOX(v2), TR("显示设置"));
    gtk_box_pack_start(GTK_BOX(v2), row(TR("合成器"), "weston 14.0.2"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v2), row(TR("后端"), "DRM (bochs-drm / pixman)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v2), row(TR("壁纸"), TR("程序化生成 · 自动轮换")), FALSE, FALSE, 0);
    /* 桌面分辨率: 下拉选择并写入 weston.ini 的 mode= */
    {
        static const char *res_choices[] = {"1024x768", "1280x720", "1280x800",
                                             "1366x768", "1600x900", "1920x1080",
                                             "2560x1440", "3840x2160", NULL};
        char cur_res[64] = "1280x800";
        FILE *wf = fopen("/etc/xdg/weston/weston.ini", "r");
        if (wf) {
            char line[256];
            while (fgets(line, sizeof line, wf)) {
                if (strncmp(line, "mode=", 5) == 0) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = 0;
                    snprintf(cur_res, sizeof cur_res, "%s", line + 5);
                }
            }
            fclose(wf);
        }
        GtkWidget *res_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *rl = gtk_label_new(TR("桌面分辨率"));
        gtk_widget_set_size_request(rl, 150, -1);
        gtk_widget_set_halign(rl, GTK_ALIGN_START);
        GtkWidget *res_combo = gtk_combo_box_text_new();
        int cur_idx = 0;
        for (int i = 0; res_choices[i]; i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), res_choices[i]);
            if (strcmp(res_choices[i], cur_res) == 0) cur_idx = i;
        }
        /* 环境变量 QY_SETTINGS_RES 强制选择（自动化验证） */
        const char *env_res = g_getenv("QY_SETTINGS_RES");
        if (env_res) {
            for (int i = 0; res_choices[i]; i++)
                if (strcmp(res_choices[i], env_res) == 0) { cur_idx = i; break; }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(res_combo), cur_idx);
        GtkWidget *b_res = gtk_button_new_with_label(TR("应用"));
        qy_add_class(b_res, "qy-btn");
        g_signal_connect(b_res, "clicked", G_CALLBACK(on_res_apply), res_combo);
        gtk_box_pack_start(GTK_BOX(res_row), rl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(res_row), res_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(res_row), b_res, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v2), res_row, FALSE, FALSE, 0);
        /* 自动化验证: 启动后自动应用所选分辨率 */
        if (env_res)
            g_timeout_add(600, auto_res_apply, res_combo);
    }

    /* 夜间模式开关: 写 /etc/qysettings.conf night_mode=1/0（qydesktop 22:00-6:00 叠加暖色遮罩） */
    {
        GtkWidget *night_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *nl = gtk_label_new(TR("夜间模式"));
        gtk_widget_set_size_request(nl, 150, -1);
        gtk_widget_set_halign(nl, GTK_ALIGN_START);
        night_sw = gtk_switch_new();
        gtk_widget_set_halign(night_sw, GTK_ALIGN_START);
        g_signal_connect(night_sw, "state-set", G_CALLBACK(on_night_state_set), NULL);
        gtk_box_pack_start(GTK_BOX(night_row), nl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(night_row), night_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v2), night_row, FALSE, FALSE, 0);
        /* 说明文字 */
        GtkWidget *night_desc = gtk_label_new(TR("22:00-6:00 自动启用暖色护眼"));
        gtk_widget_set_halign(night_desc, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(v2), night_desc, FALSE, FALSE, 0);

        /* 当前状态: /etc/qysettings.conf night_mode=1 */
        gchar *content = NULL;
        gboolean night_on = FALSE;
        if (g_file_get_contents("/etc/qysettings.conf", &content, NULL, NULL)) {
            night_on = strstr(content, "night_mode=1") != NULL;
            g_free(content);
        }
        g_signal_handlers_block_by_func(night_sw, on_night_state_set, NULL);
        gtk_switch_set_active(GTK_SWITCH(night_sw), night_on);
        g_signal_handlers_unblock_by_func(night_sw, on_night_state_set, NULL);

        /* 自动化验证: QY_SETTINGS_NIGHT=1/0 启动后自动切换夜间模式 */
        const char *env_night = g_getenv("QY_SETTINGS_NIGHT");
        if (env_night && (g_strcmp0(env_night, "1") == 0 || g_strcmp0(env_night, "0") == 0)) {
            gboolean want = g_strcmp0(env_night, "1") == 0;
            if (want != night_on)
                g_timeout_add(800, auto_night_set, GINT_TO_POINTER(want));
        }
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), v2, tab_with_icon(TR("显示"), QY_ICON_MONITOR));

    /* 字体（release 256: 交互化） */
    GtkWidget *v3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v3), 14);
    settings_group(GTK_BOX(v3), TR("字体设置"));
    /* 字体族下拉 */
    GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *fl = gtk_label_new(TR("字体族"));
    gtk_widget_set_size_request(fl, 150, -1);
    gtk_widget_set_halign(fl, GTK_ALIGN_START);
    font_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(font_combo, 240, -1);
    g_signal_connect(font_combo, "changed", G_CALLBACK(on_font_combo_changed), NULL);
    gtk_box_pack_start(GTK_BOX(font_row), fl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_row), font_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v3), font_row, FALSE, FALSE, 0);
    /* 字号 */
    GtkWidget *size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sl = gtk_label_new(TR("字号"));
    gtk_widget_set_size_request(sl, 150, -1);
    gtk_widget_set_halign(sl, GTK_ALIGN_START);
    font_size_spin = gtk_spin_button_new_with_range(9, 18, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(font_size_spin), 11);
    g_signal_connect(font_size_spin, "value-changed", G_CALLBACK(on_font_size_changed), NULL);
    gtk_box_pack_start(GTK_BOX(size_row), sl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(size_row), font_size_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v3), size_row, FALSE, FALSE, 0);
    /* 预览标签（用所选字体实时渲染） */
    font_preview = gtk_label_new(NULL);
    gtk_widget_set_halign(font_preview, GTK_ALIGN_START);
    qy_add_class(font_preview, "qy-font-preview");
    gtk_box_pack_start(GTK_BOX(v3), font_preview, FALSE, FALSE, 8);
    /* 应用按钮（品牌橙 #qysettings-font-apply） */
    GtkWidget *b_font_apply = gtk_button_new_with_label(TR("应用"));
    gtk_widget_set_name(b_font_apply, "qysettings-font-apply");
    g_signal_connect(b_font_apply, "clicked", G_CALLBACK(on_font_apply), NULL);
    gtk_box_pack_start(GTK_BOX(v3), b_font_apply, FALSE, FALSE, 8);

    font_fill_combo();
    font_load_current();
    font_update_preview();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), v3, tab_with_icon(TR("字体"), QY_ICON_SETTINGS));

    /* 服务 */
    GtkWidget *v4 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v4), 14);
    settings_group(GTK_BOX(v4), TR("服务设置"));
    gtk_box_pack_start(GTK_BOX(v4), row(TR("1 号进程"), "qyinit"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v4), row(TR("会话管理"), "seatd"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v4), row(TR("设备管理"), "eudev"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v4), row(TR("单元目录"), "/etc/qyinit.d"), FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), v4, tab_with_icon(TR("服务"), QY_ICON_SETTINGS));

    /* 声音 (ALSA amixer Master) */
    GtkWidget *v5 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v5), 14);
    settings_group(GTK_BOX(v5), TR("声音设置"));
    GtkWidget *vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(vol), TRUE);
    gtk_box_pack_start(GTK_BOX(v5), gtk_label_new(TR("输出音量 (Master)")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v5), vol, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v5), row(TR("音频后端"), "ALSA (amixer)"), FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), v5, tab_with_icon(TR("声音"), QY_ICON_VOLUME));

    /* 显示: 亮度 (backlight 探测) */
    GtkWidget *v6 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v6), 14);
    settings_group(GTK_BOX(v6), TR("亮度设置"));
    GtkWidget *br = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(br), TRUE);
    gtk_box_pack_start(GTK_BOX(v6), gtk_label_new(TR("屏幕亮度")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v6), br, FALSE, FALSE, 0);
    {
        GDir *bld = g_dir_open("/sys/class/backlight", 0, NULL);
        const gchar *bln = NULL;
        if (bld) { bln = g_dir_read_name(bld); }
        if (bln) {
            gchar *blinfo = g_strdup_printf("%s (/sys/class/backlight)", bln);
            gtk_box_pack_start(GTK_BOX(v6), row(TR("背光设备"), blinfo), FALSE, FALSE, 0);
            g_free(blinfo);
        } else {
            gtk_box_pack_start(GTK_BOX(v6), row(TR("背光设备"), TR("无 (虚拟显示不支持)")), FALSE, FALSE, 0);
        }
        if (bld) g_dir_close(bld);
    }
    g_signal_connect(vol, "value-changed", G_CALLBACK(vol_changed), NULL);
    g_signal_connect(br, "value-changed", G_CALLBACK(br_changed), NULL);
    /* 初始音量读取 (amixer get Master → [xx%]) */
    {
        gchar *out = NULL;
        gint v0 = 75;
        if (g_spawn_command_line_sync("amixer get Master", &out, NULL, NULL, NULL) && out) {
            gchar *pct = strstr(out, "[");
            if (pct) {
                gint v = atoi(pct + 1);
                if (v >= 0 && v <= 100) v0 = v;
            }
            g_free(out);
        }
        gtk_range_set_value(GTK_RANGE(vol), v0);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), v6, tab_with_icon(TR("亮度"), QY_ICON_MONITOR));

    /* 日期时间（每秒刷新） */
    GtkWidget *vdt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vdt), 16);
    settings_group(GTK_BOX(vdt), TR("日期时间设置"));
    GtkWidget *dt_label = gtk_label_new(NULL);
    gtk_widget_set_halign(dt_label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(dt_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vdt), dt_label, FALSE, FALSE, 0);
    /* 时区: /etc/timezone 内容（缺失时回退 UTC） */
    {
        char tz[128] = "UTC";
        FILE *tzf = fopen("/etc/timezone", "r");
        if (tzf) {
            if (fgets(tz, sizeof tz, tzf)) {
                char *nl = strchr(tz, '\n');
                if (nl) *nl = 0;
            }
            fclose(tzf);
        }
        gchar *tzl = g_strdup_printf("%s: %s", TR("时区"), tz);
        GtkWidget *l = gtk_label_new(tzl);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vdt), l, FALSE, FALSE, 0);
        g_free(tzl);
    }
    /* NTP 时间同步开关: 有 timedatectl 用 set-ntp，否则写 /etc/qysettings.conf */
    {
        GtkWidget *ntp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l = gtk_label_new(TR("NTP 时间同步"));
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        ntp_sw = gtk_switch_new();
        gtk_widget_set_halign(ntp_sw, GTK_ALIGN_START);
        g_signal_connect(ntp_sw, "state-set", G_CALLBACK(on_ntp_state_set), NULL);
        gtk_box_pack_start(GTK_BOX(ntp_row), l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ntp_row), ntp_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vdt), ntp_row, FALSE, FALSE, 0);

        gboolean ntp_on = ntp_read_state();
        g_signal_handlers_block_by_func(ntp_sw, on_ntp_state_set, NULL);
        gtk_switch_set_active(GTK_SWITCH(ntp_sw), ntp_on);
        g_signal_handlers_unblock_by_func(ntp_sw, on_ntp_state_set, NULL);

        /* 自动化验证: QY_SETTINGS_NTP=1/0 启动后自动切换 NTP */
        const char *env_ntp = g_getenv("QY_SETTINGS_NTP");
        if (env_ntp && (g_strcmp0(env_ntp, "1") == 0 || g_strcmp0(env_ntp, "0") == 0)) {
            gboolean want = g_strcmp0(env_ntp, "1") == 0;
            if (want != ntp_on)
                g_timeout_add(800, auto_ntp_set, GINT_TO_POINTER(want));
        }
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vdt, tab_with_icon(TR("日期时间"), QY_ICON_CLOCK));

    /* 语言 */
    GtkWidget *vlang = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vlang), 16);
    GtkWidget *llb = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(llb), TR("<b>界面语言 / Interface Language</b>"));
    gtk_widget_set_halign(llb, GTK_ALIGN_START);
    qy_add_class(llb, "qy-settings-group");
    gtk_box_pack_start(GTK_BOX(vlang), llb, FALSE, FALSE, 0);
    /* 当前语言（/etc/qylang: zh/en，默认 zh） */
    {
        char lang[16] = "zh";
        FILE *lf = fopen("/etc/qylang", "r");
        if (lf) {
            if (fgets(lang, sizeof lang, lf)) {
                char *nl = strchr(lang, '\n');
                if (nl) *nl = 0;
            }
            fclose(lf);
        }
        gchar *cur = g_strdup_printf("%s: %s", TR("当前语言"),
                                     strncmp(lang, "en", 2) == 0
                                         ? "English" : TR("简体中文"));
        GtkWidget *cl = gtk_label_new(cur);
        gtk_widget_set_halign(cl, GTK_ALIGN_START);
        qy_add_class(cl, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vlang), cl, FALSE, FALSE, 0);
        g_free(cur);
    }
    GtkWidget *bzh = gtk_button_new_with_label(TR("简体中文"));
    GtkWidget *ben = gtk_button_new_with_label("English");
    g_signal_connect(bzh, "clicked", G_CALLBACK(on_lang_zh), NULL);
    g_signal_connect(ben, "clicked", G_CALLBACK(on_lang_en), NULL);
    gtk_box_pack_start(GTK_BOX(vlang), bzh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vlang), ben, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vlang), gtk_label_new(TR("切换后重新启动生效 / Takes effect after reboot")), FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vlang, tab_with_icon(TR("语言"), QY_ICON_SETTINGS));

    /* 驱动页: 已加载内核模块 (/proc/modules) */
    {
        GtkWidget *vdrv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *drv_text = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(drv_text), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(drv_text), GTK_WRAP_NONE);
        GtkTextBuffer *db = gtk_text_view_get_buffer(GTK_TEXT_VIEW(drv_text));
        GtkWidget *drv_sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_container_add(GTK_CONTAINER(drv_sw), drv_text);
        gtk_box_pack_start(GTK_BOX(vdrv), drv_sw, TRUE, TRUE, 0);
        FILE *mf = fopen("/proc/modules", "r");
        GString *s = g_string_new(NULL);
        char line[1024];
        int nm = 0;
        if (mf) {
            while (fgets(line, sizeof line, mf)) {
                char name[128] = ""; unsigned long sz = 0; int refs = 0;
                if (sscanf(line, "%127s %lu %d", name, &sz, &refs) == 3) {
                    g_string_append_printf(s, "%s  %lu KB  refs=%d\n",
                                           name, sz / 1024, refs);
                    nm++;
                }
            }
            fclose(mf);
        }
        if (!nm) g_string_append(s, TR("无已加载模块"));
        gchar *txt = g_strdup_printf(TR("已加载内核模块 %d 个：\n\n%s"), nm, s->str);
        gtk_text_buffer_set_text(db, txt, -1);
        g_free(txt);
        g_string_free(s, TRUE);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vdrv, tab_with_icon(TR("驱动"), QY_ICON_DRIVER));
    }

    /* 存储页: df -kP 磁盘占用（挂载点/容量/已用/进度条） */
    {
        GtkWidget *vst = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        settings_group(GTK_BOX(vst), TR("存储设置"));
        GtkWidget *tip = gtk_label_new(TR("磁盘占用（df 实时数据）"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vst), tip, FALSE, FALSE, 0);
        FILE *df = popen("df -kP 2>/dev/null", "r");
        char line[1024];
        int first = 1;
        if (df) {
            while (fgets(line, sizeof line, df)) {
                if (first) { first = 0; continue; } /* 跳过表头 */
                char fs[128] = "", mp[128] = "";
                unsigned long blocks = 0, used = 0, avail = 0;
                int pct = 0;
                if (sscanf(line, "%127s %lu %lu %lu %d%% %127s",
                           fs, &blocks, &used, &avail, &pct, mp) == 6) {
                    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                    GtkWidget *l = gtk_label_new(mp);
                    gtk_widget_set_size_request(l, 110, -1);
                    gtk_widget_set_halign(l, GTK_ALIGN_START);
                    GtkWidget *pb = gtk_progress_bar_new();
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pb),
                                                  used / (double)(blocks + 1));
                    gchar *info = g_strdup_printf("%d%% · %lu MB / %lu MB",
                                                  pct, used / 1024, blocks / 1024);
                    GtkWidget *il = gtk_label_new(info);
                    g_free(info);
                    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
                    gtk_box_pack_start(GTK_BOX(row), pb, TRUE, TRUE, 0);
                    gtk_box_pack_start(GTK_BOX(row), il, FALSE, FALSE, 0);
                    gtk_box_pack_start(GTK_BOX(vst), row, FALSE, FALSE, 0);
                }
            }
            pclose(df);
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vst, tab_with_icon(TR("存储"), QY_ICON_FILES));
    }

    /* 电源页: 熄屏时间（写 weston.ini [core] idle-time=） */
    {
        static const char *idle_choices[] = { "从不", "1 分钟", "5 分钟",
                                               "10 分钟", "30 分钟", "1 小时" };
        GtkWidget *vpo = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        settings_group(GTK_BOX(vpo), TR("电源设置"));
        GtkWidget *tip = gtk_label_new(TR("屏幕熄灭时间（合成器空闲超时）"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vpo), tip, FALSE, FALSE, 0);
        GtkWidget *po_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *pl = gtk_label_new(TR("熄屏时间"));
        gtk_widget_set_size_request(pl, 150, -1);
        gtk_widget_set_halign(pl, GTK_ALIGN_START);
        GtkWidget *idle_combo = gtk_combo_box_text_new();
        int cur_idx = 2; /* 默认 5 分钟 */
        for (int i = 0; i < (int)G_N_ELEMENTS(idle_choices); i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(idle_combo),
                                           TR(idle_choices[i]));
        }
        /* 读当前 idle-time（0=从不） */
        FILE *wf = fopen("/etc/xdg/weston/weston.ini", "r");
        if (wf) {
            char line[256];
            while (fgets(line, sizeof line, wf)) {
                if (strncmp(line, "idle-time=", 11) == 0) {
                    int cur = atoi(line + 11);
                    for (int i = 0; i < (int)G_N_ELEMENTS(idle_secs); i++)
                        if (idle_secs[i] == cur) cur_idx = i;
                }
            }
            fclose(wf);
        }
        /* 环境变量 QY_SETTINGS_IDLE=N 强制选择（自动化验证） */
        const char *env_idle = g_getenv("QY_SETTINGS_IDLE");
        if (env_idle) {
            int want = atoi(env_idle);
            for (int i = 0; i < (int)G_N_ELEMENTS(idle_secs); i++)
                if (idle_secs[i] == want) cur_idx = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(idle_combo), cur_idx);
        GtkWidget *b_idle = gtk_button_new_with_label(TR("应用"));
        qy_add_class(b_idle, "qy-btn");
        g_signal_connect(b_idle, "clicked", G_CALLBACK(on_idle_apply),
                         GINT_TO_POINTER(cur_idx));
        gtk_box_pack_start(GTK_BOX(po_row), pl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(po_row), idle_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(po_row), b_idle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vpo), po_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vpo, tab_with_icon(TR("电源"), QY_ICON_POWER));
        /* 自动化验证: 启动后自动应用 */
        if (env_idle)
            g_timeout_add(700, auto_idle_apply, GINT_TO_POINTER(cur_idx));
    }

    /* 网络页: 当前网络概况（默认接口/IP/MAC/网关/DNS） */
    {
        GtkWidget *vnet = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vnet), 14);
        settings_group(GTK_BOX(vnet), TR("网络设置"));
        char main_if[64] = "eth0";
        char rline[256];
        FILE *rf = popen("ip route show default 2>/dev/null", "r");
        if (rf) {
            if (fgets(rline, sizeof rline, rf)) {
                char *devp = strstr(rline, "dev ");
                if (devp) {
                    devp += 4;
                    char *sp = strchr(devp, ' ');
                    if (sp) *sp = 0;
                    snprintf(main_if, sizeof main_if, "%s", devp);
                }
            }
            pclose(rf);
        }
        char ip[128] = "-";
        char cmd[160];
        snprintf(cmd, sizeof cmd, "ip -o -4 addr show %s 2>/dev/null", main_if);
        FILE *ipf = popen(cmd, "r");
        if (ipf) {
            if (fgets(rline, sizeof rline, ipf)) {
                char *p = strstr(rline, "inet ");
                if (p) {
                    p += 5;
                    char *sp = strchr(p, ' ');
                    if (sp) *sp = 0;
                    snprintf(ip, sizeof ip, "%s", p);
                }
            }
            pclose(ipf);
        }
        char mac[64] = "-";
        snprintf(cmd, sizeof cmd, "ip -o link show %s 2>/dev/null", main_if);
        FILE *mf = popen(cmd, "r");
        if (mf) {
            if (fgets(rline, sizeof rline, mf)) {
                char *mp = strstr(rline, "link/");
                if (mp) {
                    mp += 5;
                    if (sscanf(mp, "%*s %63s", mac) != 1) {
                        char *sp = strchr(mp, ' ');
                        if (sp) *sp = 0;
                        snprintf(mac, sizeof mac, "%s", mp);
                    }
                }
            }
            pclose(mf);
        }
        char gw[128] = "-";
        FILE *gf = popen("ip route show default 2>/dev/null", "r");
        if (gf) {
            if (fgets(rline, sizeof rline, gf)) {
                char *p = strstr(rline, "default via ");
                if (p) {
                    p += 12;
                    char *sp = strchr(p, ' ');
                    if (sp) *sp = 0;
                    snprintf(gw, sizeof gw, "%s", p);
                }
            }
            pclose(gf);
        }
        char dns[128] = "-";
        FILE *dnf = fopen("/etc/resolv.conf", "r");
        if (dnf) {
            int n = 0;
            while (fgets(rline, sizeof rline, dnf) && n < 4) {
                if (strncmp(rline, "nameserver", 10) == 0) {
                    char *p = rline + 10;
                    while (*p == ' ') p++;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    if (n == 0) snprintf(dns, sizeof dns, "%s", p);
                    else {
                        strncat(dns, ", ", sizeof dns - strlen(dns) - 1);
                        strncat(dns, p, sizeof dns - strlen(dns) - 1);
                    }
                    n++;
                }
            }
            fclose(dnf);
        }
        gtk_box_pack_start(GTK_BOX(vnet), row(TR("网络接口"), main_if), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vnet), row(TR("IP 地址"), ip), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vnet), row(TR("MAC 地址"), mac), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vnet), row(TR("默认网关"), gw), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vnet), row(TR("DNS 服务器"), dns), FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vnet, tab_with_icon(TR("网络"), QY_ICON_NETWORK));
    }

    /* 隐私页: 应用权限开关（写 /etc/qyperm.conf） */
    {
        static const char *perm_keys[] = {
            "location", "camera", "microphone",
            "notifications", "background", "filesystem"
        };
        static const char *perm_labels[] = {
            "位置", "摄像头", "麦克风",
            "通知", "后台应用", "文件系统访问"
        };
        GtkWidget *vpr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vpr), 14);
        settings_group(GTK_BOX(vpr), TR("隐私设置"));
        gtk_box_pack_start(GTK_BOX(vpr), row(TR("允许应用访问"), "（写 /etc/qyperm.conf）"), FALSE, FALSE, 0);
        g_perm_loading = 1;
        for (int i = 0; i < 6; i++) {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l = gtk_label_new(TR(perm_labels[i]));
            gtk_widget_set_size_request(l, 180, -1);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            GtkWidget *sw = gtk_switch_new();
            const char *cur = perm_read(perm_keys[i], "on");
            gtk_switch_set_active(GTK_SWITCH(sw), strcmp(cur, "off") != 0);
            g_signal_connect(sw, "state-set", G_CALLBACK(on_perm_toggled),
                             (gpointer)perm_keys[i]);
            /* 自动化: QY_SETTINGS_PERM=camera:off 强制关闭对应开关 */
            const char *env = g_getenv("QY_SETTINGS_PERM");
            if (env) {
                char key[64] = "", mode[16] = "";
                if (sscanf(env, "%63[^:]:%15s", key, mode) == 2 &&
                    strcmp(key, perm_keys[i]) == 0) {
                    gtk_switch_set_active(GTK_SWITCH(sw), strcmp(mode, "off") != 0);
                }
            }
            gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vpr), r, FALSE, FALSE, 0);
        }
        g_perm_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vpr, tab_with_icon(TR("隐私"), QY_ICON_LOCK));
        /* 自动化验证: 启动后把配置写入文件 */
        if (g_getenv("QY_SETTINGS_PERM"))
            g_timeout_add(800, auto_perm_apply, NULL);
    }

    /* 应用页: 已安装应用列表（可启动） */
    {
        static const char *apps_installed[][2] = {
            { "系统设置", "qysettings" }, { "系统监视", "qymon" },
            { "系统安装", "qysetup" },    { "用户管理", "qyusers" },
            { "网络管理", "qynet" },      { "文件管理器", "qyfiles" },
            { "图片查看", "qyview" },     { "压缩管理", "qyarc" },
            { "回收站", "qyfiles --trash" }, { "终端", "weston-terminal" },
            { "文本编辑", "qyedit" },     { "软件中心", "qystore" },
        };
        GtkWidget *vap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vap), 14);
        settings_group(GTK_BOX(vap), TR("应用程序设置"));
        GtkWidget *tip = gtk_label_new(TR("已安装应用（点击启动）"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vap), tip, FALSE, FALSE, 0);
        GtkWidget *ap_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ap_scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *ap_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        for (int i = 0; i < 12; i++) {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l = gtk_label_new(TR(apps_installed[i][0]));
            gtk_widget_set_size_request(l, 180, -1);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            gchar *cmd0;
            const char *cmdname = apps_installed[i][1];
            if (strchr(cmdname, ' ')) {
                char bin_path[128];
                sscanf(cmdname, "%127s", bin_path);
                cmd0 = g_strdup_printf("/usr/bin/%s", bin_path);
            } else {
                cmd0 = g_strdup_printf("/usr/bin/%s", cmdname);
            }
            gboolean exists = g_file_test(cmd0, G_FILE_TEST_EXISTS);
            g_free(cmd0);
            GtkWidget *cmdl = gtk_label_new(apps_installed[i][1]);
            gtk_widget_set_halign(cmdl, GTK_ALIGN_START);
            qy_add_class(cmdl, "qy-dim");
            GtkWidget *b = gtk_button_new_with_label(TR("启动"));
            gtk_widget_set_sensitive(b, exists);
            if (exists)
                g_signal_connect(b, "clicked", G_CALLBACK(on_app_launch),
                                 (gpointer)apps_installed[i][1]);
            gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), cmdl, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(r), b, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(ap_list), r, FALSE, FALSE, 0);
        }
        gtk_container_add(GTK_CONTAINER(ap_scroll), ap_list);
        gtk_box_pack_start(GTK_BOX(vap), ap_scroll, TRUE, TRUE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vap, tab_with_icon(TR("应用程序"), QY_ICON_GRID));
    }

    /* 专注助手页: 免打扰模式开关（写 /etc/qyfocus.conf） */
    {
        GtkWidget *vfo = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vfo), 14);
        settings_group(GTK_BOX(vfo), TR("专注设置"));
        gtk_box_pack_start(GTK_BOX(vfo), row(TR("专注助手"), TR("免打扰时屏蔽通知弹窗")), FALSE, FALSE, 0);
        GtkWidget *fo_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *fo_l = gtk_label_new(TR("免打扰模式"));
        gtk_widget_set_size_request(fo_l, 180, -1);
        gtk_widget_set_halign(fo_l, GTK_ALIGN_START);
        GtkWidget *fo_sw = gtk_switch_new();
        FILE *ff = fopen(FOCUS_CONF, "r");
        int dnd = 0;
        if (ff) {
            char line[64];
            while (fgets(line, sizeof line, ff))
                if (strncmp(line, "dnd=", 4) == 0)
                    dnd = strncmp(line + 4, "on", 2) == 0;
            fclose(ff);
        }
        gtk_switch_set_active(GTK_SWITCH(fo_sw), dnd);
        const char *env_f = g_getenv("QY_SETTINGS_FOCUS");
        if (env_f)
            gtk_switch_set_active(GTK_SWITCH(fo_sw), strcmp(env_f, "on") == 0);
        g_signal_connect(fo_sw, "state-set", G_CALLBACK(on_focus_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(fo_row), fo_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(fo_row), fo_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vfo), fo_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vfo, tab_with_icon(TR("专注"), QY_ICON_SETTINGS));
        if (env_f)
            g_timeout_add(900, auto_focus_apply, NULL);
    }

    /* 通知页: 应用通知开关（写 /etc/qynotif.conf） */
    {
        static const char *notif_apps[][2] = {
            { "系统监视", "qymon" },   { "文件管理器", "qyfiles" },
            { "软件中心", "qystore" },  { "文本编辑", "qyedit" },
            { "图片查看", "qyview" }, { "网络管理", "qynet" },
        };
        GtkWidget *vno = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vno), 14);
        settings_group(GTK_BOX(vno), TR("通知设置"));
        GtkWidget *tip = gtk_label_new(TR("哪些应用可以发送通知"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vno), tip, FALSE, FALSE, 0);
        g_notif_loading = 1;
        for (int i = 0; i < 6; i++) {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l = gtk_label_new(TR(notif_apps[i][0]));
            gtk_widget_set_size_request(l, 180, -1);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            GtkWidget *sw = gtk_switch_new();
            /* 读当前配置 */
            gchar *content = NULL;
            gsize len = 0;
            int cur_on = 1;
            if (g_file_get_contents(NOTIF_CONF, &content, &len, NULL)) {
                char *line = content;
                size_t klen = strlen(notif_apps[i][1]);
                while (line && *line) {
                    if (strncmp(line, notif_apps[i][1], klen) == 0 &&
                        line[klen] == '=') {
                        cur_on = strncmp(line + klen + 1, "off", 3) != 0;
                        break;
                    }
                    char *nl = strchr(line, '\n');
                    line = nl ? nl + 1 : NULL;
                }
                g_free(content);
            }
            gtk_switch_set_active(GTK_SWITCH(sw), cur_on);
            /* 自动化: QY_SETTINGS_NOTIF=app:off */
            const char *env = g_getenv("QY_SETTINGS_NOTIF");
            if (env) {
                char app[64] = "", mode[16] = "";
                if (sscanf(env, "%63[^:]:%15s", app, mode) == 2 &&
                    strcmp(app, notif_apps[i][1]) == 0) {
                    gtk_switch_set_active(GTK_SWITCH(sw), strcmp(mode, "off") != 0);
                }
            }
            g_signal_connect(sw, "state-set", G_CALLBACK(on_notif_toggled),
                             (gpointer)notif_apps[i][1]);
            gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vno), r, FALSE, FALSE, 0);
        }
        g_notif_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vno, tab_with_icon(TR("通知"), QY_ICON_SETTINGS));
        if (g_getenv("QY_SETTINGS_NOTIF"))
            g_timeout_add(950, auto_notif_apply, NULL);
    }

    /* 防火墙页: 状态 + 开关（写 /etc/qyfirewall.conf） */
    {
        GtkWidget *vfw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vfw), 14);
        settings_group(GTK_BOX(vfw), TR("防火墙设置"));
        int rules = -1;
        FILE *it = popen("iptables -L -n 2>/dev/null | grep -vc '^Chain\\|^$\\|^target'", "r");
        if (it) {
            char buf[32];
            if (fgets(buf, sizeof buf, it))
                rules = atoi(buf);
            pclose(it);
        }
        gchar *fw_status;
        if (rules >= 0)
            fw_status = g_strdup_printf(TR("防火墙规则 %d 条"), rules);
        else
            fw_status = g_strdup_printf("%s", TR("未检测到 iptables"));
        gtk_box_pack_start(GTK_BOX(vfw), row(TR("防火墙状态"), fw_status), FALSE, FALSE, 0);
        g_free(fw_status);
        GtkWidget *fw_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *fw_l = gtk_label_new(TR("启用防火墙"));
        gtk_widget_set_size_request(fw_l, 180, -1);
        gtk_widget_set_halign(fw_l, GTK_ALIGN_START);
        GtkWidget *fw_sw = gtk_switch_new();
        FILE *fwf = fopen(FIREWALL_CONF, "r");
        int fw_on = 0;
        if (fwf) {
            char line[64];
            while (fgets(line, sizeof line, fwf))
                if (strncmp(line, "firewall=", 10) == 0)
                    fw_on = strncmp(line + 10, "on", 2) == 0;
            fclose(fwf);
        }
        gtk_switch_set_active(GTK_SWITCH(fw_sw), fw_on);
        const char *env_fw = g_getenv("QY_SETTINGS_FIREWALL");
        if (env_fw)
            gtk_switch_set_active(GTK_SWITCH(fw_sw), strcmp(env_fw, "on") == 0);
        g_signal_connect(fw_sw, "state-set", G_CALLBACK(on_firewall_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(fw_row), fw_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(fw_row), fw_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vfw), fw_row, FALSE, FALSE, 0);

        /* SSH 服务开关（qynotifd 消费 qyssh.conf） */
        GtkWidget *ssh_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ssh_l = gtk_label_new(TR("启用 SSH 服务"));
        gtk_widget_set_size_request(ssh_l, 180, -1);
        gtk_widget_set_halign(ssh_l, GTK_ALIGN_START);
        GtkWidget *ssh_sw = gtk_switch_new();
        FILE *sshf = fopen(SSH_CONF, "r");
        int ssh_on = 0;
        if (sshf) {
            char line[64];
            while (fgets(line, sizeof line, sshf))
                if (strncmp(line, "sshd=", 5) == 0)
                    ssh_on = strncmp(line + 5, "on", 2) == 0;
            fclose(sshf);
        }
        gtk_switch_set_active(GTK_SWITCH(ssh_sw), ssh_on);
        const char *env_ssh = g_getenv("QY_SETTINGS_SSHD");
        if (env_ssh)
            gtk_switch_set_active(GTK_SWITCH(ssh_sw), strcmp(env_ssh, "on") == 0);
        g_signal_connect(ssh_sw, "state-set", G_CALLBACK(on_sshd_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(ssh_row), ssh_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ssh_row), ssh_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vfw), ssh_row, FALSE, FALSE, 0);

        /* 恢复默认设置按钮 */
        GtkWidget *reset_btn = gtk_button_new_with_label(TR("恢复默认设置"));
        gtk_widget_set_tooltip_text(reset_btn, TR("删除全部 qy*.conf，恢复出厂默认设置"));
        g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(vfw), reset_btn, FALSE, FALSE, 0);

        /* 时区选择 */
        GtkWidget *tz_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *tz_l = gtk_label_new(TR("时区"));
        gtk_widget_set_size_request(tz_l, 180, -1);
        gtk_widget_set_halign(tz_l, GTK_ALIGN_START);
        GtkWidget *tz_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tz_combo), "Asia/Shanghai", "上海 Asia/Shanghai");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tz_combo), "Asia/Tokyo", "东京 Asia/Tokyo");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tz_combo), "UTC", "UTC");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tz_combo), "Europe/London", "伦敦 Europe/London");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tz_combo), "America/New_York", "纽约 America/New_York");
        gchar *tz_cur = NULL;
        g_file_get_contents("/etc/timezone", &tz_cur, NULL, NULL);
        const char *active_tz = tz_cur ? g_strstrip(tz_cur) : "Asia/Shanghai";
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(tz_combo), active_tz))
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(tz_combo), "Asia/Shanghai");
        g_free(tz_cur);
        const char *env_tz = g_getenv("QY_SETTINGS_TZ");
        if (env_tz) {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(tz_combo), env_tz);
            g_timeout_add(1000, auto_tz_apply, g_strdup(env_tz));
        }
        g_signal_connect(tz_combo, "changed", G_CALLBACK(on_tz_changed), NULL);
        gtk_box_pack_start(GTK_BOX(tz_row), tz_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tz_row), tz_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vfw), tz_row, FALSE, FALSE, 0);

        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vfw, tab_with_icon(TR("防火墙"), QY_ICON_LOCK));
        if (env_fw)
            g_timeout_add(1000, auto_firewall_apply, NULL);
        if (env_ssh)
            g_timeout_add(1000, auto_sshd_apply, NULL);
        if (g_getenv("QY_SETTINGS_RESET"))
            g_timeout_add(1400, auto_reset_settings, NULL);
    }

    /* 代理页: HTTP/HTTPS 代理（写 /etc/environment） */
    {
        GtkWidget *vpx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vpx), 14);
        settings_group(GTK_BOX(vpx), TR("代理设置"));
        gtk_box_pack_start(GTK_BOX(vpx), row(TR("代理服务器"), TR("写入 /etc/environment，重启应用生效")), FALSE, FALSE, 0);
        GtkWidget *px_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *px_l = gtk_label_new(TR("代理地址"));
        gtk_widget_set_size_request(px_l, 150, -1);
        gtk_widget_set_halign(px_l, GTK_ALIGN_START);
        GtkWidget *px_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(px_entry), "http://主机:端口");
        gtk_widget_set_size_request(px_entry, 320, -1);
        GtkWidget *px_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(px_btn, "qy-btn");
        g_signal_connect(px_btn, "clicked", G_CALLBACK(on_proxy_apply), px_entry);
        gtk_box_pack_start(GTK_BOX(px_row), px_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(px_row), px_entry, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(px_row), px_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vpx), px_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vpx, tab_with_icon(TR("代理"), QY_ICON_NETWORK));
        if (g_getenv("QY_SETTINGS_PROXY"))
            g_timeout_add(1050, auto_proxy_apply, NULL);
    }

    /* 设备页: USB 与输入设备 */
    {
        GtkWidget *vdev = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vdev), 14);
        settings_group(GTK_BOX(vdev), TR("设备设置"));
        GtkWidget *tip = gtk_label_new(TR("USB 与输入设备"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vdev), tip, FALSE, FALSE, 0);
        GtkWidget *dev_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dev_scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *dev_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        int n = 0;
        FILE *inf = fopen("/proc/bus/input/devices", "r");
        if (inf) {
            char line[256];
            while (fgets(line, sizeof line, inf)) {
                if (strncmp(line, "N: Name=", 8) == 0) {
                    char *p = line + 8;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    gchar *txt = g_strdup_printf("· %s", p);
                    GtkWidget *il = gtk_label_new(txt);
                    gtk_widget_set_halign(il, GTK_ALIGN_START);
                    gtk_box_pack_start(GTK_BOX(dev_list), il, FALSE, FALSE, 0);
                    g_free(txt);
                    n++;
                }
            }
            fclose(inf);
        }
        DIR *ud = opendir("/sys/bus/usb/devices");
        if (ud) {
            struct dirent *de;
            while ((de = readdir(ud))) {
                if (de->d_name[0] == '.') continue;
                char mp[160];
                char vendor[128] = "", product[128] = "";
                snprintf(mp, sizeof mp, "/sys/bus/usb/devices/%s/manufacturer",
                         de->d_name);
                FILE *vf = fopen(mp, "r");
                if (vf) {
                    if (fgets(vendor, sizeof vendor, vf)) {
                        char *nl = strchr(vendor, '\n');
                        if (nl) *nl = 0;
                    }
                    fclose(vf);
                }
                snprintf(mp, sizeof mp, "/sys/bus/usb/devices/%s/product",
                         de->d_name);
                FILE *pf = fopen(mp, "r");
                if (pf) {
                    if (fgets(product, sizeof product, pf)) {
                        char *nl = strchr(product, '\n');
                        if (nl) *nl = 0;
                    }
                    fclose(pf);
                }
                if (vendor[0] || product[0]) {
                    gchar *txt = g_strdup_printf("· %s %s (%s)", vendor,
                                                 product, de->d_name);
                    GtkWidget *ul = gtk_label_new(txt);
                    gtk_widget_set_halign(ul, GTK_ALIGN_START);
                    gtk_box_pack_start(GTK_BOX(dev_list), ul, FALSE, FALSE, 0);
                    g_free(txt);
                    n++;
                }
            }
            closedir(ud);
        }
        if (n == 0) {
            GtkWidget *empty = gtk_label_new(TR("未检测到外接设备"));
            gtk_widget_set_halign(empty, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(dev_list), empty, FALSE, FALSE, 0);
        }
        gtk_container_add(GTK_CONTAINER(dev_scroll), dev_list);
        gtk_box_pack_start(GTK_BOX(vdev), dev_scroll, TRUE, TRUE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vdev, tab_with_icon(TR("设备"), QY_ICON_SETTINGS));
    }

    /* 多任务页: 分屏/贴靠/虚拟桌面开关（写 /etc/qymultitask.conf） */
    {
        static const char *multi_keys[] = { "split", "snap", "vd" };
        static const char *multi_labels[] = { "分屏", "窗口贴靠", "虚拟桌面" };
        GtkWidget *vmt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vmt), 14);
        settings_group(GTK_BOX(vmt), TR("多任务设置"));
        GtkWidget *tip = gtk_label_new(TR("多任务处理"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vmt), tip, FALSE, FALSE, 0);
        g_multi_loading = 1;
        for (int i = 0; i < 3; i++) {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l = gtk_label_new(TR(multi_labels[i]));
            gtk_widget_set_size_request(l, 180, -1);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            GtkWidget *sw = gtk_switch_new();
            gchar *content = NULL;
            gsize len = 0;
            int cur_on = 1;
            if (g_file_get_contents(MULTI_CONF, &content, &len, NULL)) {
                char *line = content;
                size_t klen = strlen(multi_keys[i]);
                while (line && *line) {
                    if (strncmp(line, multi_keys[i], klen) == 0 &&
                        line[klen] == '=') {
                        cur_on = strncmp(line + klen + 1, "off", 3) != 0;
                        break;
                    }
                    char *nl = strchr(line, '\n');
                    line = nl ? nl + 1 : NULL;
                }
                g_free(content);
            }
            gtk_switch_set_active(GTK_SWITCH(sw), cur_on);
            const char *env = g_getenv("QY_SETTINGS_MULTI");
            if (env) {
                char key[64] = "", mode[16] = "";
                if (sscanf(env, "%63[^:]:%15s", key, mode) == 2 &&
                    strcmp(key, multi_keys[i]) == 0) {
                    gtk_switch_set_active(GTK_SWITCH(sw), strcmp(mode, "off") != 0);
                }
            }
            g_signal_connect(sw, "state-set", G_CALLBACK(on_multi_toggled),
                             (gpointer)multi_keys[i]);
            gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vmt), r, FALSE, FALSE, 0);
        }
        g_multi_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vmt, tab_with_icon(TR("多任务"), QY_ICON_SWITCHER));
        if (g_getenv("QY_SETTINGS_MULTI"))
            g_timeout_add(1100, auto_multi_apply, NULL);
    }

    /* 自动播放页: U 盘/光盘插入行为（写 /etc/qyautoplay.conf） */
    {
        static const char *ap_actions[] = { "open", "ask", "none" };
        static const char *ap_labels[] = { "打开文件管理器", "每次询问", "不操作" };
        GtkWidget *vap2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vap2), 14);
        settings_group(GTK_BOX(vap2), TR("自动播放设置"));
        settings_exp_hint(GTK_BOX(vap2));
        GtkWidget *tip = gtk_label_new(TR("自动播放"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vap2), tip, FALSE, FALSE, 0);
        /* 自动播放开关 */
        GtkWidget *ap_row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ap_l1 = gtk_label_new(TR("插入可移动设备时自动播放"));
        gtk_widget_set_size_request(ap_l1, 240, -1);
        gtk_widget_set_halign(ap_l1, GTK_ALIGN_START);
        GtkWidget *ap_sw = gtk_switch_new();
        g_autoplay_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 1;
        if (g_file_get_contents(AUTOPLAY_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "autoplay=", 10) == 0) {
                    cur_on = strncmp(line + 10, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        gtk_switch_set_active(GTK_SWITCH(ap_sw), cur_on);
        const char *env_ap = g_getenv("QY_SETTINGS_AUTOPLAY");
        if (env_ap)
            gtk_switch_set_active(GTK_SWITCH(ap_sw), strcmp(env_ap, "off") != 0);
        g_signal_connect(ap_sw, "state-set", G_CALLBACK(on_autoplay_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(ap_row1), ap_l1, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ap_row1), ap_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vap2), ap_row1, FALSE, FALSE, 0);
        /* 动作选择 */
        GtkWidget *ap_row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ap_l2 = gtk_label_new(TR("插入 U 盘时"));
        gtk_widget_set_size_request(ap_l2, 150, -1);
        gtk_widget_set_halign(ap_l2, GTK_ALIGN_START);
        GtkWidget *ap_combo = gtk_combo_box_text_new();
        int cur_act = 0;
        for (int i = 0; i < 3; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ap_combo),
                                           TR(ap_labels[i]));
        if (1) {
            gchar *c2 = NULL;
            gsize l2 = 0;
            if (g_file_get_contents(AUTOPLAY_CONF, &c2, &l2, NULL)) {
                char *line = c2;
                while (line && *line) {
                    if (strncmp(line, "action=", 7) == 0) {
                        char *p = line + 7;
                        char *nl = strchr(p, '\n');
                        if (nl) *nl = 0;
                        for (int i = 0; i < 3; i++)
                            if (strcmp(p, ap_actions[i]) == 0) cur_act = i;
                        break;
                    }
                    char *nl = strchr(line, '\n');
                    line = nl ? nl + 1 : NULL;
                }
                g_free(c2);
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(ap_combo), cur_act);
        GtkWidget *ap_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(ap_btn, "qy-btn");
        g_signal_connect(ap_btn, "clicked", G_CALLBACK(on_autoplay_action),
                         (gpointer)ap_actions[cur_act]);
        gtk_box_pack_start(GTK_BOX(ap_row2), ap_l2, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ap_row2), ap_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ap_row2), ap_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vap2), ap_row2, FALSE, FALSE, 0);
        g_autoplay_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vap2, tab_with_icon(TR("自动播放"), QY_ICON_PLAY));
        if (env_ap)
            g_timeout_add(1150, auto_autoplay_apply, NULL);
    }

    /* 鼠标页: 主按键/双击速度/滚轮行数（写 /etc/qymouse.conf） */
    {
        GtkWidget *vms = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vms), 14);
        settings_group(GTK_BOX(vms), TR("鼠标设置"));
        gtk_box_pack_start(GTK_BOX(vms), row(TR("鼠标"), TR("设置写入 /etc/qymouse.conf")), FALSE, FALSE, 0);
        /* 主按键 */
        GtkWidget *mb_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *mb_l = gtk_label_new(TR("主按键"));
        gtk_widget_set_size_request(mb_l, 150, -1);
        gtk_widget_set_halign(mb_l, GTK_ALIGN_START);
        g_mouse_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_mouse_combo), TR("右手（默认）"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_mouse_combo), TR("左手"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_mouse_combo), 0);
        const char *env_ms = g_getenv("QY_SETTINGS_MOUSE");
        if (env_ms && strstr(env_ms, "primary:right"))
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_mouse_combo), 1);
        gtk_box_pack_start(GTK_BOX(mb_row), mb_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(mb_row), g_mouse_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vms), mb_row, FALSE, FALSE, 0);
        /* 双击速度 */
        GtkWidget *ds_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ds_l = gtk_label_new(TR("双击速度"));
        gtk_widget_set_size_request(ds_l, 150, -1);
        gtk_widget_set_halign(ds_l, GTK_ALIGN_START);
        g_mouse_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 10, 1);
        gtk_widget_set_size_request(g_mouse_scale, 260, -1);
        gtk_scale_set_value_pos(GTK_SCALE(g_mouse_scale), GTK_POS_RIGHT);
        gtk_range_set_value(GTK_RANGE(g_mouse_scale), 5);
        gtk_box_pack_start(GTK_BOX(ds_row), ds_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ds_row), g_mouse_scale, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vms), ds_row, FALSE, FALSE, 0);
        /* 滚轮行数 */
        GtkWidget *sw_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *sw_l = gtk_label_new(TR("滚轮行数"));
        gtk_widget_set_size_request(sw_l, 150, -1);
        gtk_widget_set_halign(sw_l, GTK_ALIGN_START);
        g_mouse_spin = gtk_spin_button_new_with_range(1, 20, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_mouse_spin), 3);
        gtk_box_pack_start(GTK_BOX(sw_row), sw_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sw_row), g_mouse_spin, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vms), sw_row, FALSE, FALSE, 0);
        /* 应用 */
        GtkWidget *ms_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(ms_btn, "qy-btn");
        g_signal_connect(ms_btn, "clicked", G_CALLBACK(on_mouse_apply), NULL);
        GtkWidget *ms_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(ms_hb), ms_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vms), ms_hb, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vms, tab_with_icon(TR("鼠标"), QY_ICON_SETTINGS));
        if (env_ms)
            g_timeout_add(1200, auto_mouse_apply, NULL);
    }

    /* 网络重置页: 危险操作 + 确认（写 /etc/qynetreset.log） */
    {
        GtkWidget *vnr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vnr), 14);
        settings_group(GTK_BOX(vnr), TR("网络重置设置"));
        gtk_box_pack_start(GTK_BOX(vnr), row(TR("网络重置"), TR("恢复网卡出厂设置（危险）")), FALSE, FALSE, 0);
        GtkWidget *nr_btn = gtk_button_new_with_label(TR("重置网络"));
        qy_add_class(nr_btn, "qy-btn");
        g_signal_connect(nr_btn, "clicked", G_CALLBACK(on_net_reset), NULL);
        GtkWidget *nr_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(nr_hb), nr_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vnr), nr_hb, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vnr, tab_with_icon(TR("网络重置"), QY_ICON_NETWORK));
        if (g_getenv("QY_SETTINGS_NETRESET"))
            g_timeout_add(1250, auto_net_reset, NULL);
    }

    /* 多显示器页: 输出列表 + 布局（写 /etc/qydisplay.conf） */
    {
        GtkWidget *vd = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vd), 14);
        settings_group(GTK_BOX(vd), TR("多显示器设置"));
        settings_exp_hint(GTK_BOX(vd));
        gtk_box_pack_start(GTK_BOX(vd), row(TR("检测到的显示器"), "Virtual-1 (1280x800)"), FALSE, FALSE, 0);
        GtkWidget *dl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *dl_l = gtk_label_new(TR("多显示器模式"));
        gtk_widget_set_size_request(dl_l, 150, -1);
        gtk_widget_set_halign(dl_l, GTK_ALIGN_START);
        g_disp_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_disp_combo), TR("扩展桌面"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_disp_combo), TR("复制屏幕"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_disp_combo), 0);
        GtkWidget *dl_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(dl_btn, "qy-btn");
        g_signal_connect(dl_btn, "clicked", G_CALLBACK(on_display_apply), NULL);
        gtk_box_pack_start(GTK_BOX(dl_row), dl_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(dl_row), g_disp_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(dl_row), dl_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vd), dl_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vd, tab_with_icon(TR("多显示器"), QY_ICON_MONITOR));
        if (g_getenv("QY_SETTINGS_DISPLAY"))
            g_timeout_add(1300, auto_display_apply, NULL);
    }

    /* 蓝牙页: 开关 + 适配器检测（写 /etc/qybluetooth.conf） */
    {
        GtkWidget *vbt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vbt), 14);
        settings_group(GTK_BOX(vbt), TR("蓝牙设置"));
        GtkWidget *bt_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *bt_l = gtk_label_new(TR("蓝牙"));
        gtk_widget_set_size_request(bt_l, 180, -1);
        gtk_widget_set_halign(bt_l, GTK_ALIGN_START);
        GtkWidget *bt_sw = gtk_switch_new();
        g_bt_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 0;
        if (g_file_get_contents(BT_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "bluetooth=", 11) == 0) {
                    cur_on = strncmp(line + 11, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        const char *env_bt = g_getenv("QY_SETTINGS_BT");
        if (env_bt)
            cur_on = strcmp(env_bt, "off") != 0;
        gtk_switch_set_active(GTK_SWITCH(bt_sw), cur_on);
        g_signal_connect(bt_sw, "state-set", G_CALLBACK(on_bt_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(bt_row), bt_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bt_row), bt_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbt), bt_row, FALSE, FALSE, 0);
        g_bt_loading = 0;
        int n = 0;
        DIR *btd = opendir("/sys/class/bluetooth");
        if (btd) {
            struct dirent *de;
            while ((de = readdir(btd))) {
                if (strncmp(de->d_name, "hci", 3) == 0) {
                    gchar *txt = g_strdup_printf("· %s", de->d_name);
                    GtkWidget *bl = gtk_label_new(txt);
                    gtk_widget_set_halign(bl, GTK_ALIGN_START);
                    gtk_box_pack_start(GTK_BOX(vbt), bl, FALSE, FALSE, 0);
                    g_free(txt);
                    n++;
                }
            }
            closedir(btd);
        }
        if (n == 0) {
            GtkWidget *empty = gtk_label_new(TR("未检测到蓝牙适配器"));
            gtk_widget_set_halign(empty, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(vbt), empty, FALSE, FALSE, 0);
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbt, tab_with_icon(TR("蓝牙"), QY_ICON_NETWORK));
        if (env_bt)
            g_timeout_add(1350, auto_bt_apply, NULL);
    }

    /* 触摸板页: 开关 + 灵敏度（写 /etc/qytouchpad.conf） */
    {
        GtkWidget *vtp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vtp), 14);
        settings_group(GTK_BOX(vtp), TR("触摸板设置"));
        settings_exp_hint(GTK_BOX(vtp));
        GtkWidget *tp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *tp_l = gtk_label_new(TR("启用触摸板"));
        gtk_widget_set_size_request(tp_l, 180, -1);
        gtk_widget_set_halign(tp_l, GTK_ALIGN_START);
        GtkWidget *tp_sw = gtk_switch_new();
        g_tp_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 1;
        if (g_file_get_contents(TOUCHPAD_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "touchpad=", 10) == 0) {
                    cur_on = strncmp(line + 10, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        const char *env_tp = g_getenv("QY_SETTINGS_TOUCHPAD");
        if (env_tp)
            cur_on = strcmp(env_tp, "off") != 0;
        gtk_switch_set_active(GTK_SWITCH(tp_sw), cur_on);
        g_signal_connect(tp_sw, "state-set", G_CALLBACK(on_touchpad_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(tp_row), tp_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tp_row), tp_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vtp), tp_row, FALSE, FALSE, 0);
        g_tp_loading = 0;
        GtkWidget *sen_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *sen_l = gtk_label_new(TR("灵敏度"));
        gtk_widget_set_size_request(sen_l, 180, -1);
        gtk_widget_set_halign(sen_l, GTK_ALIGN_START);
        g_tp_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 5, 1);
        gtk_widget_set_size_request(g_tp_scale, 260, -1);
        gtk_scale_set_value_pos(GTK_SCALE(g_tp_scale), GTK_POS_RIGHT);
        gtk_range_set_value(GTK_RANGE(g_tp_scale), 3);
        gtk_box_pack_start(GTK_BOX(sen_row), sen_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sen_row), g_tp_scale, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vtp), sen_row, FALSE, FALSE, 0);
        GtkWidget *tp_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(tp_btn, "qy-btn");
        g_signal_connect(tp_btn, "clicked", G_CALLBACK(on_touchpad_apply), NULL);
        GtkWidget *tp_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(tp_hb), tp_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vtp), tp_hb, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vtp, tab_with_icon(TR("触摸板"), QY_ICON_SETTINGS));
        if (env_tp)
            g_timeout_add(1400, auto_touchpad_apply, NULL);
    }

    /* 图形页: 显卡信息 + 模式（写 /etc/qygraphics.conf） */
    {
        GtkWidget *vg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vg), 14);
        settings_group(GTK_BOX(vg), TR("图形设置"));
        settings_exp_hint(GTK_BOX(vg));
        gtk_box_pack_start(GTK_BOX(vg), row(TR("显卡"), TR("未检测到独立显卡")), FALSE, FALSE, 0);
        GtkWidget *gfx_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *gfx_l = gtk_label_new(TR("图形模式"));
        gtk_widget_set_size_request(gfx_l, 150, -1);
        gtk_widget_set_halign(gfx_l, GTK_ALIGN_START);
        g_gfx_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_gfx_combo), TR("默认"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_gfx_combo), TR("高性能"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_gfx_combo), TR("省电"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_gfx_combo), 0);
        const char *env_gfx = g_getenv("QY_SETTINGS_GRAPHICS");
        if (env_gfx && strstr(env_gfx, "mode:performance"))
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_gfx_combo), 1);
        GtkWidget *gfx_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(gfx_btn, "qy-btn");
        g_signal_connect(gfx_btn, "clicked", G_CALLBACK(on_graphics_apply), NULL);
        gtk_box_pack_start(GTK_BOX(gfx_row), gfx_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(gfx_row), g_gfx_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(gfx_row), gfx_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vg), gfx_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vg, tab_with_icon(TR("图形"), QY_ICON_MONITOR));
        if (env_gfx)
            g_timeout_add(1450, auto_graphics_apply, NULL);
    }

    /* 远程桌面页: 开关 + 端口（写 /etc/qyremotedesktop.conf） */
    {
        GtkWidget *vrd = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vrd), 14);
        settings_group(GTK_BOX(vrd), TR("远程桌面设置"));
        settings_exp_hint(GTK_BOX(vrd));
        gtk_box_pack_start(GTK_BOX(vrd), row(TR("远程桌面"), TR("允许远程连接到这台电脑")), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vrd), row(TR("端口"), "3389"), FALSE, FALSE, 0);
        GtkWidget *rdp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *rdp_l = gtk_label_new(TR("启用远程桌面"));
        gtk_widget_set_size_request(rdp_l, 180, -1);
        gtk_widget_set_halign(rdp_l, GTK_ALIGN_START);
        GtkWidget *rdp_sw = gtk_switch_new();
        g_rdp_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 0;
        if (g_file_get_contents(RDP_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "rdp=", 4) == 0) {
                    cur_on = strncmp(line + 4, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        const char *env_rdp = g_getenv("QY_SETTINGS_RDP");
        if (env_rdp)
            cur_on = strcmp(env_rdp, "off") != 0;
        gtk_switch_set_active(GTK_SWITCH(rdp_sw), cur_on);
        g_signal_connect(rdp_sw, "state-set", G_CALLBACK(on_rdp_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(rdp_row), rdp_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rdp_row), rdp_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vrd), rdp_row, FALSE, FALSE, 0);
        g_rdp_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vrd, tab_with_icon(TR("远程桌面"), QY_ICON_MONITOR));
        if (env_rdp)
            g_timeout_add(1500, auto_rdp_apply, NULL);
    }

    /* 投影页: 投影模式（写 /etc/qyproject.conf） */
    {
        GtkWidget *vpj = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vpj), 14);
        settings_group(GTK_BOX(vpj), TR("投影设置"));
        settings_exp_hint(GTK_BOX(vpj));
        gtk_box_pack_start(GTK_BOX(vpj), row(TR("投影"), TR("选择第二屏幕的投影模式")), FALSE, FALSE, 0);
        GtkWidget *pj_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *pj_l = gtk_label_new(TR("投影模式"));
        gtk_widget_set_size_request(pj_l, 150, -1);
        gtk_widget_set_halign(pj_l, GTK_ALIGN_START);
        g_proj_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_proj_combo), TR("仅电脑屏幕"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_proj_combo), TR("复制屏幕"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_proj_combo), TR("扩展桌面"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_proj_combo), TR("仅第二屏幕"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_proj_combo), 2);
        const char *env_pj = g_getenv("QY_SETTINGS_PROJECT");
        if (env_pj && strstr(env_pj, "mode:extend"))
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_proj_combo), 2);
        else if (env_pj && strstr(env_pj, "mode:mirror"))
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_proj_combo), 1);
        GtkWidget *pj_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(pj_btn, "qy-btn");
        g_signal_connect(pj_btn, "clicked", G_CALLBACK(on_project_apply), NULL);
        gtk_box_pack_start(GTK_BOX(pj_row), pj_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(pj_row), g_proj_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(pj_row), pj_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vpj), pj_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vpj, tab_with_icon(TR("投影"), QY_ICON_MONITOR));
        if (env_pj)
            g_timeout_add(1550, auto_project_apply, NULL);
    }

    /* HD Color 页: HDR 开关 + 颜色配置文件（写 /etc/qyhdr.conf） */
    {
        GtkWidget *vhd = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vhd), 14);
        settings_group(GTK_BOX(vhd), TR("HD Color 设置"));
        settings_exp_hint(GTK_BOX(vhd));
        gtk_box_pack_start(GTK_BOX(vhd), row(TR("HD Color"), TR("高动态范围颜色与显示配置文件")), FALSE, FALSE, 0);
        GtkWidget *hdr_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *hdr_l = gtk_label_new(TR("HDR 视频"));
        gtk_widget_set_size_request(hdr_l, 180, -1);
        gtk_widget_set_halign(hdr_l, GTK_ALIGN_START);
        GtkWidget *hdr_sw = gtk_switch_new();
        g_hdr_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 0;
        if (g_file_get_contents(HDR_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "hdr=", 4) == 0) {
                    cur_on = strncmp(line + 4, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        const char *env_hdr = g_getenv("QY_SETTINGS_HDR");
        if (env_hdr)
            cur_on = strcmp(env_hdr, "off") != 0;
        gtk_switch_set_active(GTK_SWITCH(hdr_sw), cur_on);
        g_signal_connect(hdr_sw, "state-set", G_CALLBACK(on_hdr_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(hdr_row), hdr_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hdr_row), hdr_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vhd), hdr_row, FALSE, FALSE, 0);
        g_hdr_loading = 0;
        GtkWidget *pf_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *pf_l = gtk_label_new(TR("颜色配置文件"));
        gtk_widget_set_size_request(pf_l, 180, -1);
        gtk_widget_set_halign(pf_l, GTK_ALIGN_START);
        g_hdr_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_hdr_combo), "sRGB");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_hdr_combo), "Display P3");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_hdr_combo), TR("鲜艳"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_hdr_combo), 0);
        GtkWidget *pf_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(pf_btn, "qy-btn");
        g_signal_connect(pf_btn, "clicked", G_CALLBACK(on_hdr_profile_apply), NULL);
        gtk_box_pack_start(GTK_BOX(pf_row), pf_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(pf_row), g_hdr_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(pf_row), pf_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vhd), pf_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vhd, tab_with_icon("HD Color", QY_ICON_MONITOR));
        if (env_hdr)
            g_timeout_add(1600, auto_hdr_apply, NULL);
    }

    /* 打印机页: 状态 + 添加打印机（写 /etc/qyprinter.conf） */
    {
        GtkWidget *vpr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vpr), 14);
        settings_group(GTK_BOX(vpr), TR("打印机设置"));
        gtk_box_pack_start(GTK_BOX(vpr), row(TR("打印机和扫描仪"), TR("管理打印机")), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vpr), row(TR("打印机"), TR("未检测到打印机")), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vpr), row(TR("默认打印机"), TR("无")), FALSE, FALSE, 0);
        GtkWidget *pr_btn = gtk_button_new_with_label(TR("添加打印机"));
        qy_add_class(pr_btn, "qy-btn");
        g_signal_connect(pr_btn, "clicked", G_CALLBACK(on_printer_add), NULL);
        GtkWidget *pr_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(pr_hb), pr_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vpr), pr_hb, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vpr, tab_with_icon(TR("打印机"), QY_ICON_SETTINGS));
        if (g_getenv("QY_SETTINGS_PRINTER"))
            g_timeout_add(1650, auto_printer_apply, NULL);
    }

    /* 流量计费页: 开关 + 数据用量（写 /etc/qymetered.conf） */
    {
        GtkWidget *vmt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vmt), 14);
        settings_group(GTK_BOX(vmt), TR("流量计费设置"));
        settings_exp_hint(GTK_BOX(vmt));
        gtk_box_pack_start(GTK_BOX(vmt), row(TR("按流量计费的连接"), TR("限制后台数据下载")), FALSE, FALSE, 0);
        /* 数据用量: /proc/net/dev 累加 rx+tx */
        double total_b = 0;
        FILE *nd = fopen("/proc/net/dev", "r");
        if (nd) {
            char line[512];
            while (fgets(line, sizeof line, nd)) {
                if (strchr(line, ':')) {
                    char *p = strchr(line, ':') + 1;
                    unsigned long long rx = 0, tx = 0;
                    if (sscanf(p, "%llu", &rx) == 1) {
                        char *q = p;
                        int i = 0;
                        for (i = 0; i < 8 && q; i++) {
                            while (*q == ' ') q++;
                            while (*q && *q != ' ') q++;
                        }
                        if (q) sscanf(q, "%llu", &tx);
                    }
                    total_b += (double)rx + (double)tx;
                }
            }
            fclose(nd);
        }
        gchar *usage = g_strdup_printf("%.2f GB", total_b / (1024.0 * 1024.0 * 1024.0));
        gtk_box_pack_start(GTK_BOX(vmt), row(TR("本月数据用量"), usage), FALSE, FALSE, 0);
        g_free(usage);
        GtkWidget *m_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *m_l = gtk_label_new(TR("按流量计费"));
        gtk_widget_set_size_request(m_l, 180, -1);
        gtk_widget_set_halign(m_l, GTK_ALIGN_START);
        GtkWidget *m_sw = gtk_switch_new();
        g_metered_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 0;
        if (g_file_get_contents(METERED_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "metered=", 9) == 0) {
                    cur_on = strncmp(line + 9, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        const char *env_mt = g_getenv("QY_SETTINGS_METERED");
        if (env_mt)
            cur_on = strcmp(env_mt, "off") != 0;
        gtk_switch_set_active(GTK_SWITCH(m_sw), cur_on);
        g_signal_connect(m_sw, "state-set", G_CALLBACK(on_metered_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(m_row), m_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(m_row), m_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vmt), m_row, FALSE, FALSE, 0);
        g_metered_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vmt, tab_with_icon(TR("流量计费"), QY_ICON_NETWORK));
        if (env_mt)
            g_timeout_add(1700, auto_metered_apply, NULL);
    }

    /* 输入页: 键盘布局（写 /etc/qyinput.conf） */
    {
        GtkWidget *vin = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vin), 14);
        settings_group(GTK_BOX(vin), TR("输入设置"));
        gtk_box_pack_start(GTK_BOX(vin), row(TR("键盘"), TR("输入法与键盘布局")), FALSE, FALSE, 0);
        gchar *content = NULL;
        gsize len = 0;
        gchar *cur = g_strdup("us");
        if (g_file_get_contents(INPUT_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "layout=", 8) == 0) {
                    char *p = line + 8;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    if (*p) { g_free(cur); cur = g_strdup(p); }
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        gtk_box_pack_start(GTK_BOX(vin), row(TR("当前布局"), cur), FALSE, FALSE, 0);
        g_free(cur);
        GtkWidget *in_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *in_l = gtk_label_new(TR("键盘布局"));
        gtk_widget_set_size_request(in_l, 150, -1);
        gtk_widget_set_halign(in_l, GTK_ALIGN_START);
        g_input_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_input_combo), "US");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_input_combo), "GB");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_input_combo), "DE");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_input_combo), "FR");
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_input_combo), 0);
        const char *env_in = g_getenv("QY_SETTINGS_INPUT");
        if (env_in) {
            const char *layout = strstr(env_in, "layout:");
            if (layout) {
                layout += 7;
                const char *layouts[] = { "us", "gb", "de", "fr" };
                for (int i = 0; i < 4; i++)
                    if (strcmp(layout, layouts[i]) == 0)
                        gtk_combo_box_set_active(GTK_COMBO_BOX(g_input_combo), i);
            }
        }
        GtkWidget *in_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(in_btn, "qy-btn");
        g_signal_connect(in_btn, "clicked", G_CALLBACK(on_input_apply), NULL);
        gtk_box_pack_start(GTK_BOX(in_row), in_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(in_row), g_input_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(in_row), in_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vin), in_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vin, tab_with_icon(TR("输入"), QY_ICON_KEYBOARD));
        if (env_in)
            g_timeout_add(1750, auto_input_apply, NULL);
    }

    /* 输入法页: 无输入法 / fcitx5 / ibus（写 /etc/profile.d/lanxiu-inputmethod.sh） */
    {
        GtkWidget *vim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vim), 14);
        settings_group(GTK_BOX(vim), TR("输入法设置"));
        gtk_box_pack_start(GTK_BOX(vim), row(TR("输入法"), TR("fcitx5 / ibus 框架选择")), FALSE, FALSE, 0);

        GtkWidget *im_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *im_l = gtk_label_new(TR("输入法"));
        gtk_widget_set_size_request(im_l, 150, -1);
        gtk_widget_set_halign(im_l, GTK_ALIGN_START);
        g_im_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_im_combo), TR("无输入法"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_im_combo), "fcitx5");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_im_combo), "ibus");
        int im_cur = im_current_from_file();
        if (im_cur < 0)
            im_cur = im_detect_default();
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), im_cur);
        const char *env_im = g_getenv("QY_SETTINGS_IM");
        if (env_im) {
            if (!strcmp(env_im, "fcitx5")) gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), 1);
            else if (!strcmp(env_im, "ibus")) gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), 2);
            else gtk_combo_box_set_active(GTK_COMBO_BOX(g_im_combo), 0);
        }
        gtk_box_pack_start(GTK_BOX(im_row), im_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(im_row), g_im_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vim), im_row, FALSE, FALSE, 0);

        /* 说明标签 #qysettings-im-hint */
        GtkWidget *im_hint = gtk_label_new(TR("需安装对应输入法框架并重启后生效"));
        gtk_widget_set_name(im_hint, "qysettings-im-hint");
        gtk_widget_set_halign(im_hint, GTK_ALIGN_START);
        gtk_widget_set_margin_top(im_hint, 4);
        qy_add_class(im_hint, "qy-exp-hint");
        gtk_box_pack_start(GTK_BOX(vim), im_hint, FALSE, FALSE, 0);

        /* 状态检测标签（release 265） */
        GtkWidget *im_status = gtk_label_new(NULL);
        gtk_widget_set_name(im_status, "qysettings-im-status");
        gtk_widget_set_halign(im_status, GTK_ALIGN_START);
        gtk_widget_set_margin_top(im_status, 2);
        qy_add_class(im_status, "qy-exp-hint");
        int im_det = im_detect_default();
        if (im_det == 1)
            gtk_label_set_text(GTK_LABEL(im_status), TR("已检测到 fcitx5 输入法框架"));
        else if (im_det == 2)
            gtk_label_set_text(GTK_LABEL(im_status), TR("已检测到 ibus 输入法框架"));
        else
            gtk_label_set_text(GTK_LABEL(im_status), TR("未检测到输入法框架，可运行: sudo qypkg install fcitx5"));
        gtk_box_pack_start(GTK_BOX(vim), im_status, FALSE, FALSE, 0);

        /* 应用按钮 #qysettings-im-apply（品牌橙） */
        GtkWidget *im_btn = gtk_button_new_with_label(TR("应用"));
        gtk_widget_set_name(im_btn, "qysettings-im-apply");
        g_signal_connect(im_btn, "clicked", G_CALLBACK(on_im_apply), NULL);
        gtk_box_pack_start(GTK_BOX(vim), im_btn, FALSE, FALSE, 0);

        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vim, tab_with_icon(TR("输入法"), QY_ICON_KEYBOARD));
        if (env_im)
            g_timeout_add(1850, auto_im_apply, NULL);
    }

    /* 笔和Ink页: 手写笔开关（写 /etc/qypen.conf） */
    {
        GtkWidget *vpn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vpn), 14);
        settings_group(GTK_BOX(vpn), TR("笔和Ink设置"));
        settings_exp_hint(GTK_BOX(vpn));
        gtk_box_pack_start(GTK_BOX(vpn), row(TR("笔和Ink"), TR("手写笔设置")), FALSE, FALSE, 0);
        g_pen_loading = 1;
        static const char *pen_keys[] = { "pen", "ignore_touch" };
        static const char *pen_labels[] = { "手写笔", "书写时忽略触摸" };
        for (int i = 0; i < 2; i++) {
            GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *p_l = gtk_label_new(TR(pen_labels[i]));
            gtk_widget_set_size_request(p_l, 180, -1);
            gtk_widget_set_halign(p_l, GTK_ALIGN_START);
            GtkWidget *p_sw = gtk_switch_new();
            gchar *content = NULL;
            gsize len = 0;
            int cur_on = 0;
            if (g_file_get_contents(PEN_CONF, &content, &len, NULL)) {
                char *line = content;
                size_t klen = strlen(pen_keys[i]);
                while (line && *line) {
                    if (strncmp(line, pen_keys[i], klen) == 0 && line[klen] == '=') {
                        cur_on = strncmp(line + klen + 1, "off", 3) != 0;
                        break;
                    }
                    char *nl = strchr(line, '\n');
                    line = nl ? nl + 1 : NULL;
                }
                g_free(content);
            }
            const char *env_pn = g_getenv("QY_SETTINGS_PEN");
            if (env_pn && i == 0)
                cur_on = strcmp(env_pn, "off") != 0;
            gtk_switch_set_active(GTK_SWITCH(p_sw), cur_on);
            g_signal_connect(p_sw, "state-set", G_CALLBACK(on_pen_toggled),
                             (gpointer)pen_keys[i]);
            gtk_box_pack_start(GTK_BOX(p_row), p_l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(p_row), p_sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vpn), p_row, FALSE, FALSE, 0);
        }
        g_pen_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vpn, tab_with_icon(TR("笔和Ink"), QY_ICON_EDIT));
        if (g_getenv("QY_SETTINGS_PEN"))
            g_timeout_add(1800, auto_pen_apply, NULL);
    }

    /* 启动项页: 开机自启应用开关（写 /etc/qyautostart.conf） */
    {
        static const char *ast_apps[][2] = {
            { "系统监视", "qymon" },
            { "终端", "weston-terminal" },
            { "文件管理器", "qyfiles" },
            { "软件中心", "qystore" },
        };
        GtkWidget *vst = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vst), 14);
        settings_group(GTK_BOX(vst), TR("启动项设置"));
        GtkWidget *tip = gtk_label_new(TR("哪些应用开机自动启动"));
        gtk_widget_set_halign(tip, GTK_ALIGN_START);
        qy_add_class(tip, "qy-settings-curlang");
        gtk_box_pack_start(GTK_BOX(vst), tip, FALSE, FALSE, 0);
        for (int i = 0; i < 4; i++) {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l = gtk_label_new(TR(ast_apps[i][0]));
            gtk_widget_set_size_request(l, 180, -1);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            GtkWidget *sw = gtk_switch_new();
            gchar *content = NULL;
            gsize len = 0;
            int cur_on = 0;
            if (g_file_get_contents(AUTOSTART_CONF, &content, &len, NULL)) {
                char *line = content;
                const char *key = ast_apps[i][1];
                size_t klen = strlen(key);
                while (line && *line) {
                    if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
                        cur_on = strncmp(line + klen + 1, "off", 3) != 0;
                        break;
                    }
                    char *nl = strchr(line, '\n');
                    line = nl ? nl + 1 : NULL;
                }
                g_free(content);
            }
            const char *env = g_getenv("QY_SETTINGS_AUTOSTART");
            if (env) {
                char app[64] = "", mode[16] = "";
                if (sscanf(env, "%63[^:]:%15s", app, mode) == 2 &&
                    strcmp(app, ast_apps[i][1]) == 0) {
                    cur_on = strcmp(mode, "off") != 0;
                }
            }
            gtk_switch_set_active(GTK_SWITCH(sw), cur_on);
            g_signal_connect(sw, "state-set", G_CALLBACK(on_autostart_toggled),
                             (gpointer)ast_apps[i][1]);
            gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vst), r, FALSE, FALSE, 0);
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vst, tab_with_icon(TR("启动项"), QY_ICON_GRID));
        if (g_getenv("QY_SETTINGS_AUTOSTART"))
            g_timeout_add(1850, auto_autostart_apply, NULL);
    }

    /* WiFi 页: 开关 + 无线网卡检测（写 /etc/qywifi.conf） */
    {
        GtkWidget *vwf = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vwf), 14);
        settings_group(GTK_BOX(vwf), TR("WiFi 设置"));
        gtk_box_pack_start(GTK_BOX(vwf), row(TR("WiFi"), TR("无线网络连接")), FALSE, FALSE, 0);
        /* 检测无线网卡 */
        char wlan[64] = "";
        FILE *iwf = popen("iw dev 2>/dev/null | awk \"/Interface/ {print $2}\" | head -1", "r");
        if (iwf) {
            char buf[64];
            if (fgets(buf, sizeof buf, iwf)) {
                char *nl = strchr(buf, '\n');
                if (nl) *nl = 0;
                if (*buf) g_strlcpy(wlan, buf, sizeof wlan);
            }
            pclose(iwf);
        }
        gchar *wlan_disp;
        if (wlan[0])
            wlan_disp = g_strdup(wlan);
        else
            wlan_disp = g_strdup(TR("未检测到无线网卡"));
        gtk_box_pack_start(GTK_BOX(vwf), row(TR("无线网卡"), wlan_disp), FALSE, FALSE, 0);
        g_free(wlan_disp);
        /* 可用网络 */
        GtkWidget *net_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *net_l = gtk_label_new(TR("可用网络"));
        gtk_widget_set_size_request(net_l, 150, -1);
        gtk_widget_set_halign(net_l, GTK_ALIGN_START);
        gchar *nets = NULL;
        if (wlan[0]) {
            gchar *cmd = g_strdup_printf("iw dev %s scan 2>/dev/null | grep SSID | head -5", wlan);
            FILE *sf = popen(cmd, "r");
            g_free(cmd);
            if (sf) {
                GString *list = g_string_new(NULL);
                char buf[128];
                while (fgets(buf, sizeof buf, sf)) {
                    char *p = strchr(buf, ':');
                    if (p) {
                        p += 2;
                        char *nl = strchr(p, '\n');
                        if (nl) *nl = 0;
                        g_string_append_printf(list, "%s\n", p);
                    }
                }
                pclose(sf);
                if (list->len > 0)
                    nets = g_strchomp(g_strdup(list->str));
                else
                    nets = g_strdup(TR("无可用网络"));
                g_string_free(list, TRUE);
            } else {
                nets = g_strdup(TR("无可用网络"));
            }
        } else {
            nets = g_strdup(TR("无可用网络"));
        }
        GtkWidget *net_val = gtk_label_new(nets);
        g_free(nets);
        gtk_widget_set_halign(net_val, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(net_row), net_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(net_row), net_val, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vwf), net_row, FALSE, FALSE, 0);
        /* WiFi 开关 */
        GtkWidget *wf_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *wf_l = gtk_label_new(TR("启用 WiFi"));
        gtk_widget_set_size_request(wf_l, 180, -1);
        gtk_widget_set_halign(wf_l, GTK_ALIGN_START);
        GtkWidget *wf_sw = gtk_switch_new();
        g_wifi_loading = 1;
        gchar *content = NULL;
        gsize len = 0;
        int cur_on = 0;
        if (g_file_get_contents(WIFI_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "wifi=", 5) == 0) {
                    cur_on = strncmp(line + 5, "off", 3) != 0;
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        const char *env_wf = g_getenv("QY_SETTINGS_WIFI");
        if (env_wf)
            cur_on = strcmp(env_wf, "off") != 0;
        gtk_switch_set_active(GTK_SWITCH(wf_sw), cur_on);
        g_signal_connect(wf_sw, "state-set", G_CALLBACK(on_wifi_toggled), NULL);
        gtk_box_pack_start(GTK_BOX(wf_row), wf_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(wf_row), wf_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vwf), wf_row, FALSE, FALSE, 0);
        g_wifi_loading = 0;
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vwf, tab_with_icon(TR("WiFi"), QY_ICON_NETWORK));
        if (env_wf)
            g_timeout_add(1900, auto_wifi_apply, NULL);
    }

    /* 窗口行为页: 拖动/贴靠/双击动作（写 /etc/qywinbehavior.conf） */
    {
        GtkWidget *vwb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vwb), 14);
        settings_group(GTK_BOX(vwb), TR("窗口行为设置"));
        gtk_box_pack_start(GTK_BOX(vwb), row(TR("窗口行为"), TR("窗口拖动与贴靠设置")), FALSE, FALSE, 0);
        g_win_loading = 1;
        static const char *win_keys[] = { "drag", "snap" };
        static const char *win_labels[] = { "自由拖动窗口", "边缘贴靠" };
        for (int i = 0; i < 2; i++) {
            GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *l = gtk_label_new(TR(win_labels[i]));
            gtk_widget_set_size_request(l, 180, -1);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            GtkWidget *sw = gtk_switch_new();
            gchar *content = NULL;
            gsize len = 0;
            int cur_on = 1;
            if (g_file_get_contents(WIN_CONF, &content, &len, NULL)) {
                char *line = content;
                size_t klen = strlen(win_keys[i]);
                while (line && *line) {
                    if (strncmp(line, win_keys[i], klen) == 0 && line[klen] == '=') {
                        cur_on = strncmp(line + klen + 1, "off", 3) != 0;
                        break;
                    }
                    char *nl = strchr(line, '\n');
                    line = nl ? nl + 1 : NULL;
                }
                g_free(content);
            }
            const char *env_wb = g_getenv("QY_SETTINGS_WIN");
            if (env_wb) {
                char key[64] = "", mode[16] = "";
                if (sscanf(env_wb, "%63[^:]:%15s", key, mode) == 2 &&
                    strcmp(key, win_keys[i]) == 0) {
                    cur_on = strcmp(mode, "off") != 0;
                }
            }
            gtk_switch_set_active(GTK_SWITCH(sw), cur_on);
            g_signal_connect(sw, "state-set", G_CALLBACK(on_win_toggled),
                             (gpointer)win_keys[i]);
            gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), sw, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vwb), r, FALSE, FALSE, 0);
        }
        g_win_loading = 0;
        GtkWidget *dc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *dc_l = gtk_label_new(TR("双击标题栏动作"));
        gtk_widget_set_size_request(dc_l, 180, -1);
        gtk_widget_set_halign(dc_l, GTK_ALIGN_START);
        g_win_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_win_combo), TR("最大化"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_win_combo), TR("卷起"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_win_combo), 0);
        GtkWidget *dc_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(dc_btn, "qy-btn");
        g_signal_connect(dc_btn, "clicked", G_CALLBACK(on_win_action_apply), NULL);
        gtk_box_pack_start(GTK_BOX(dc_row), dc_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(dc_row), g_win_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(dc_row), dc_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vwb), dc_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vwb, tab_with_icon(TR("窗口行为"), QY_ICON_SWITCHER));
        if (g_getenv("QY_SETTINGS_WIN"))
            g_timeout_add(1950, auto_win_apply, NULL);
    }

    /* 壁纸页: 桌面背景选择（写 /etc/qywallpaper.conf） */
    {
        GtkWidget *vwl = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vwl), 14);
        settings_group(GTK_BOX(vwl), TR("壁纸设置"));
        gtk_box_pack_start(GTK_BOX(vwl), row(TR("壁纸"), TR("桌面背景图片")), FALSE, FALSE, 0);
        /* 当前壁纸 */
        gchar *cur_wall = g_strdup("lanxiu.png");
        gchar *content = NULL;
        gsize len = 0;
        if (g_file_get_contents(WALLPAPER_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "wallpaper=", 11) == 0) {
                    char *p = line + 11;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    if (*p) { g_free(cur_wall); cur_wall = g_strdup(p); }
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        gtk_box_pack_start(GTK_BOX(vwl), row(TR("当前壁纸"), cur_wall), FALSE, FALSE, 0);
        g_free(cur_wall);
        /* 壁纸列表（/usr/share/backgrounds/*.png） */
        GtkWidget *wl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *wl_l = gtk_label_new(TR("壁纸"));
        gtk_widget_set_size_request(wl_l, 150, -1);
        gtk_widget_set_halign(wl_l, GTK_ALIGN_START);
        g_wallpaper_combo = gtk_combo_box_text_new();
        int wall_idx = 0, cur_idx = 0;
        GDir *wdir = g_dir_open("/usr/share/backgrounds", 0, NULL);
        if (wdir) {
            const gchar *wn;
            while ((wn = g_dir_read_name(wdir))) {
                if (g_str_has_suffix(wn, ".png") || g_str_has_suffix(wn, ".jpg")) {
                    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_wallpaper_combo), wn);
                    if (!strcmp(wn, cur_wall) || wall_idx == 0)
                        cur_idx = wall_idx;
                    wall_idx++;
                }
            }
            g_dir_close(wdir);
        }
        if (wall_idx == 0) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_wallpaper_combo), "lanxiu.png");
            cur_idx = 0;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_wallpaper_combo), cur_idx);
        const char *env_wl = g_getenv("QY_SETTINGS_WALLPAPER");
        if (env_wl && env_wl[0]) {
            GtkTreeModel *wmodel = gtk_combo_box_get_model(GTK_COMBO_BOX(g_wallpaper_combo));
            GtkTreeIter wit;
            int widx = -1, wpos = 0;
            if (gtk_tree_model_get_iter_first(wmodel, &wit)) {
                do {
                    gchar *wtxt = NULL;
                    gtk_tree_model_get(wmodel, &wit, 0, &wtxt, -1);
                    if (wtxt && !strcmp(wtxt, env_wl)) { widx = wpos; }
                    g_free(wtxt);
                    if (widx >= 0) break;
                    wpos++;
                } while (gtk_tree_model_iter_next(wmodel, &wit));
            }
            if (widx >= 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_wallpaper_combo), widx);
        }
        GtkWidget *wl_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(wl_btn, "qy-btn");
        g_signal_connect(wl_btn, "clicked", G_CALLBACK(on_wallpaper_apply), NULL);
        gtk_box_pack_start(GTK_BOX(wl_row), wl_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(wl_row), g_wallpaper_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(wl_row), wl_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vwl), wl_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vwl, tab_with_icon(TR("壁纸"), QY_ICON_IMAGE));
        if (env_wl)
            g_timeout_add(2000, auto_wallpaper_apply, NULL);
    }

    /* 主题色页: 强调色选择（写 /etc/qytheme.conf） */
    {
        GtkWidget *vth = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vth), 14);
        settings_group(GTK_BOX(vth), TR("主题色设置"));
        gtk_box_pack_start(GTK_BOX(vth), row(TR("主题色"), TR("设置界面强调色")), FALSE, FALSE, 0);
        /* 当前主题色 */
        gchar *cur_accent = g_strdup("orange");
        gchar *content = NULL;
        gsize len = 0;
        if (g_file_get_contents(THEME_CONF, &content, &len, NULL)) {
            char *line = content;
            while (line && *line) {
                if (strncmp(line, "accent=", 8) == 0) {
                    char *p = line + 8;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    if (*p) { g_free(cur_accent); cur_accent = g_strdup(p); }
                    break;
                }
                char *nl = strchr(line, '\n');
                line = nl ? nl + 1 : NULL;
            }
            g_free(content);
        }
        gtk_box_pack_start(GTK_BOX(vth), row(TR("当前主题色"), cur_accent), FALSE, FALSE, 0);
        g_free(cur_accent);
        /* 强调色下拉 */
        GtkWidget *ac_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ac_l = gtk_label_new(TR("强调色"));
        gtk_widget_set_size_request(ac_l, 150, -1);
        gtk_widget_set_halign(ac_l, GTK_ALIGN_START);
        g_accent_combo = gtk_combo_box_text_new();
        const char *accents[] = { "orange", "purple", "blue", "green" };
        const char *accent_labels[] = { "橙色", "紫色", "蓝色", "绿色" };
        int cur_ac_idx = 0;
        for (int i = 0; i < 4; i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_accent_combo), TR(accent_labels[i]));
            if (strcmp(accents[i], cur_accent) == 0) cur_ac_idx = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_accent_combo), cur_ac_idx);
        const char *env_ac = g_getenv("QY_SETTINGS_ACCENT");
        if (env_ac) {
            for (int i = 0; i < 4; i++)
                if (strcmp(accents[i], env_ac) == 0)
                    gtk_combo_box_set_active(GTK_COMBO_BOX(g_accent_combo), i);
        }
        GtkWidget *ac_btn = gtk_button_new_with_label(TR("应用"));
        qy_add_class(ac_btn, "qy-btn");
        g_signal_connect(ac_btn, "clicked", G_CALLBACK(on_accent_apply), NULL);
        gtk_box_pack_start(GTK_BOX(ac_row), ac_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ac_row), g_accent_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ac_row), ac_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vth), ac_row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vth, tab_with_icon(TR("主题色"), QY_ICON_MONITOR));
        if (env_ac)
            g_timeout_add(2050, auto_accent_apply, NULL);
    }

    /* ---------- 设置页快捷键（GtkAccelGroup） ---------- */
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(win), accel);
    for (int i = 1; i <= 9; i++) {
        GClosure *cl = g_cclosure_new(G_CALLBACK(on_accel_page), nb, NULL);
        gtk_accel_group_connect(accel, GDK_KEY_1 + (i - 1), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, cl);
    }
    GClosure *clc = g_cclosure_new(G_CALLBACK(on_accel_close), win, NULL);
    gtk_accel_group_connect(accel, GDK_KEY_w, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, clc);
    /* Esc 关闭设置窗口 */
    gtk_accel_group_connect(accel, GDK_KEY_Escape, 0, GTK_ACCEL_VISIBLE, clc);
    /* Ctrl+F 聚焦设置搜索框 */
    gtk_widget_add_accelerator(search_entry, "grab-focus", accel, GDK_KEY_f, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    g_printerr("QYSETTINGSDKG: accel 12 keys\n");

    gtk_widget_show_all(win);

    /* 日期时间每秒刷新 */
    g_timeout_add_seconds(1, update_dt, dt_label);

    /* 环境变量 QY_SETTINGS_PAGE=N 直达标签页（默认 0=关于） */
    const char *pg = g_getenv("QY_SETTINGS_PAGE");
    if (pg) {
        int n = atoi(pg);
        if (n >= 0 && n < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb))) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), n);
            g_print("QYSETTINGS_DEBUG: set page=%d total=%d\n", n,
                     gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)));
        }
    }
}

static gboolean vol_changed(GtkRange *r, gpointer ud) {
    gint v = (gint)gtk_range_get_value(r);
    gchar *cmd = g_strdup_printf("amixer -q sset Master %d%% 2>/dev/null", v);
    system(cmd);
    g_free(cmd);
    return FALSE;
}

static gboolean br_changed(GtkRange *r, gpointer ud) {
    gint v = (gint)gtk_range_get_value(r);
    GDir *bld = g_dir_open("/sys/class/backlight", 0, NULL);
    if (bld) {
        const gchar *bln;
        gchar *blpath = NULL;
        while ((bln = g_dir_read_name(bld))) { blpath = g_strdup_printf("/sys/class/backlight/%s", bln); break; }
        g_dir_close(bld);
        if (blpath) {
            gchar *bmaxf = g_strdup_printf("%s/max_brightness", blpath);
            gchar *bcurf = g_strdup_printf("%s/brightness", blpath);
            gchar *mx = read_first_line(bmaxf);
            int maxv = atoi(mx);
            if (maxv > 0) {
                FILE *f = fopen(bcurf, "w");
                if (f) { fprintf(f, "%d", maxv * v / 100); fclose(f); }
            }
            g_free(mx); g_free(bmaxf); g_free(bcurf); g_free(blpath);
        }
    }
    return FALSE;
}


int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.settings", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
