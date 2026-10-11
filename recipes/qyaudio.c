/* qyaudio.c — 澜岫音量控制面板 (ALSA)
 * 打开 default 混音器，遍历控件找 Master（或第一个 Playback 控件），
 * 左侧垂直主音量滑杆实时写 snd_mixer_selem_set_playback_volume_all，
 * 右侧 GtkListBox 列出 /proc/asound 输出/输入设备，
 * 底部高/中/低音滑杆为占位（写 /etc/qyaudio.conf，无真实后端）。
 * 无 ALSA 设备 / 打开失败时显示 #qyaudio-err 友好错误，不崩溃。
 */
#include <gtk/gtk.h>
#include <alsa/asoundlib.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

#define AUDIO_CONF "/etc/qyaudio.conf"

static snd_mixer_t *mixer = NULL;
static snd_mixer_elem_t *master = NULL;
static long vol_min = 0, vol_max = 100;
static GtkWidget *vol_scale = NULL;      /* 主音量垂直滑杆 */
static GtkWidget *vol_pct_label = NULL; /* 百分比数字 */
static GtkWidget *mute_btn = NULL;        /* 静音切换按钮 */
static GtkWidget *err_label = NULL;      /* #qyaudio-err */
static GtkWidget *dev_list = NULL;        /* 设备列表 */
static GtkWidget *high_scale = NULL, *mid_scale = NULL, *low_scale = NULL;
static gboolean updating = FALSE;        /* 程序化设值时屏蔽信号回环 */

/* ---------- ALSA ---------- */
static long pct_to_raw(int pct) {
    return vol_min + (vol_max - vol_min) * pct / 100;
}

static int raw_to_pct(long raw) {
    if (vol_max <= vol_min) return 0;
    return (int)((raw - vol_min) * 100 / (vol_max - vol_min));
}

static void on_mute_toggled(GtkToggleButton *tb, gpointer ud);  /* 前向声明 */

static gboolean alsa_open(void) {
    int err;
    if ((err = snd_mixer_open(&mixer, 0)) < 0) return FALSE;
    if ((err = snd_mixer_attach(mixer, "default")) < 0) goto fail;
    if ((err = snd_mixer_selem_register(mixer, NULL, NULL)) < 0) goto fail;
    if ((err = snd_mixer_load(mixer)) < 0) goto fail;

    /* 遍历控件：优先 "Master"，否则第一个带 Playback 音量的控件 */
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(mixer);
         e; e = snd_mixer_elem_next(e)) {
        if (snd_mixer_elem_get_type(e) != SND_MIXER_ELEM_SIMPLE) continue;
        if (!snd_mixer_selem_has_playback_volume(e)) continue;
        const char *name = snd_mixer_selem_get_name(e);
        if (!master) master = e;
        if (name && strstr(name, "Master")) { master = e; break; }
    }
    if (!master) goto fail;
    snd_mixer_selem_get_playback_volume_range(master, &vol_min, &vol_max);
    return TRUE;

fail:
    if (mixer) { snd_mixer_close(mixer); mixer = NULL; }
    master = NULL;
    return FALSE;
}

/* 从 ALSA 读取当前音量/静音并刷新控件 */
static void refresh_from_alsa(void) {
    if (!master) return;
    long raw = 0;
    int sw = 1;
    if (snd_mixer_selem_get_playback_volume(master, SND_MIXER_SCHN_FRONT_LEFT, &raw) == 0) {
        int pct = raw_to_pct(raw);
        updating = TRUE;
        gtk_range_set_value(GTK_RANGE(vol_scale), pct);
        updating = FALSE;
        gchar buf[16];
        g_snprintf(buf, sizeof buf, "%d%%", pct);
        gtk_label_set_text(GTK_LABEL(vol_pct_label), buf);
    }
    if (snd_mixer_selem_get_playback_switch(master, SND_MIXER_SCHN_FRONT_LEFT, &sw) == 0) {
        g_signal_handlers_block_by_func(mute_btn, on_mute_toggled, NULL);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute_btn), sw ? FALSE : TRUE);
        g_signal_handlers_unblock_by_func(mute_btn, on_mute_toggled, NULL);
    }
}

static void on_vol_changed(GtkRange *r, gpointer ud) {
    (void)ud;
    if (updating) return;
    int pct = (int)gtk_range_get_value(r);
    if (master) snd_mixer_selem_set_playback_volume_all(master, pct_to_raw(pct));
    gchar buf[16];
    g_snprintf(buf, sizeof buf, "%d%%", pct);
    gtk_label_set_text(GTK_LABEL(vol_pct_label), buf);
    g_printerr("QYAUDIODBG: vol=%d%%\n", pct);
}

static void on_mute_toggled(GtkToggleButton *tb, gpointer ud) {
    (void)ud;
    if (!master) return;
    int on = gtk_toggle_button_get_active(tb) ? 0 : 1;
    snd_mixer_selem_set_playback_switch_all(master, on);
    g_printerr("QYAUDIODBG: mute=%d\n", on ? 0 : 1);
}

/* ---------- /proc/asound 设备列表 ---------- */
static GPtrArray *read_cards(void) {
    GPtrArray *cards = g_ptr_array_new_with_free_func(g_free);
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return cards;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!g_ascii_isdigit(*p)) continue;          /* 跳过非声卡行 */
        char *name = strchr(p, '[');
        char *close = name ? strchr(name, ']') : NULL;
        char *colon = strchr(p, ':');
        if (!name || !close) continue;
        char *desc = colon ? colon + 1 : NULL;
        while (desc && (*desc == ' ' || *desc == ':')) desc++;
        char *cardname = g_strndup(name + 1, close - name - 1);
        if (desc && *desc) {
            char *nl = strchr(desc, '\n');
            if (nl) *nl = 0;
            g_ptr_array_add(cards, g_strdup_printf("%s · %s", cardname, desc));
        } else {
            g_ptr_array_add(cards, g_strdup(cardname));
        }
        g_free(cardname);
    }
    fclose(f);
    return cards;
}

static void read_devices(GPtrArray **outs, GPtrArray **ins) {
    *outs = g_ptr_array_new_with_free_func(g_free);
    *ins = g_ptr_array_new_with_free_func(g_free);

    GPtrArray *cards = read_cards();
    if (cards->len == 0) {
        g_ptr_array_free(cards, TRUE);
        return;
    }

    /* /proc/asound/pcm 判断每张卡的 playback/capture（卡号是十六进制） */
    FILE *f = fopen("/proc/asound/pcm", "r");
    GHashTable *play = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *capt = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            char *dash = strchr(p, '-');
            char *colon = strchr(p, ':');
            if (!dash || !colon) continue;
            int len = dash - p;
            if (len <= 0 || len > 4) continue;
            char id[8];
            memcpy(id, p, len); id[len] = 0;
            if (strstr(colon, "playback")) g_hash_table_add(play, g_strdup(id));
            if (strstr(colon, "capture")) g_hash_table_add(capt, g_strdup(id));
        }
        fclose(f);
    }

    for (guint i = 0; i < cards->len; i++) {
        const char *name = (const char *)cards->pdata[i];
        char id[8];
        g_snprintf(id, sizeof id, "%02x", i);
        gboolean has_play = f ? g_hash_table_contains(play, id) : TRUE;
        gboolean has_capt = f ? g_hash_table_contains(capt, id) : TRUE;
        if (has_play) g_ptr_array_add(*outs, g_strdup_printf("输出 · %s", name));
        if (has_capt) g_ptr_array_add(*ins, g_strdup_printf("输入 · %s", name));
    }
    g_hash_table_destroy(play);
    g_hash_table_destroy(capt);
    g_ptr_array_free(cards, TRUE);
}

static void list_add_row(GtkWidget *list, const char *text, const char *cls) {
    GtkWidget *lb = gtk_label_new(text);
    qy_add_class(lb, cls);
    gtk_widget_set_halign(lb, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(list), lb);
}

static void rebuild_devlist(void) {
    GList *ch = gtk_container_get_children(GTK_CONTAINER(dev_list));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    GPtrArray *outs = NULL, *ins = NULL;
    read_devices(&outs, &ins);

    if (outs->len == 0 && ins->len == 0) {
        list_add_row(dev_list, TR("未检测到声卡"), "qy-audio-dev-none");
    } else {
        list_add_row(dev_list, TR("输出设备"), "qy-audio-dev-header");
        for (guint i = 0; i < outs->len; i++)
            list_add_row(dev_list, (const char *)outs->pdata[i], "qy-audio-dev-row");
        list_add_row(dev_list, TR("输入设备"), "qy-audio-dev-header");
        for (guint i = 0; i < ins->len; i++)
            list_add_row(dev_list, (const char *)ins->pdata[i], "qy-audio-dev-row");
    }
    g_ptr_array_free(outs, TRUE);
    g_ptr_array_free(ins, TRUE);
}

/* ---------- 底部 EQ 占位 ---------- */
static void eq_write(void) {
    int h = (int)gtk_range_get_value(GTK_RANGE(high_scale));
    int m = (int)gtk_range_get_value(GTK_RANGE(mid_scale));
    int l = (int)gtk_range_get_value(GTK_RANGE(low_scale));
    FILE *f = fopen(AUDIO_CONF, "w");
    if (f) {
        fprintf(f, "[eq]\n");
        fprintf(f, "high=%d\n", h);
        fprintf(f, "mid=%d\n", m);
        fprintf(f, "low=%d\n", l);
        fclose(f);
    }
    g_printerr("QYAUDIODBG: eq high=%d mid=%d low=%d\n", h, m, l);
}

static void on_eq_changed(GtkRange *r, gpointer ud) {
    (void)r; (void)ud;
    eq_write();
}

static void eq_load(void) {
    gchar *content = NULL;
    if (!g_file_get_contents(AUDIO_CONF, &content, NULL, NULL)) return;
    int h = 50, m = 50, l = 50;
    char *line = content;
    while (line && *line) {
        if (sscanf(line, "high=%d", &h) == 1) { /* matched */ }
        else if (sscanf(line, "mid=%d", &m) == 1) { /* matched */ }
        else if (sscanf(line, "low=%d", &l) == 1) { /* matched */ }
        char *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(content);
    updating = TRUE;
    gtk_range_set_value(GTK_RANGE(high_scale), h);
    gtk_range_set_value(GTK_RANGE(mid_scale), m);
    gtk_range_set_value(GTK_RANGE(low_scale), l);
    updating = FALSE;
}

/* ---------- UI ---------- */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("音量控制"));
    gtk_widget_set_name(win, "qy-audio-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("音量控制"));
    qy_window_setup(GTK_WINDOW(win), 420, 360, 380, 320, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 错误提示（默认隐藏） */
    err_label = gtk_label_new(TR("无法打开音频设备（ALSA 不可用）"));
    gtk_widget_set_name(err_label, "qyaudio-err");
    gtk_widget_set_no_show_all(err_label, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), err_label, FALSE, FALSE, 0);

    /* 主区：左音量 + 右设备 */
    GtkWidget *main_h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(vbox), main_h, TRUE, TRUE, 0);

    /* 左：垂直主音量 */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left, 132, -1);
    GtkWidget *vol_title = gtk_label_new(TR("主音量"));
    qy_add_class(vol_title, "qy-audio-title");
    gtk_box_pack_start(GTK_BOX(left), vol_title, FALSE, FALSE, 0);
    vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 100, 1);
    qy_add_class(vol_scale, "qy-audio-vol");
    gtk_scale_set_draw_value(GTK_SCALE(vol_scale), FALSE);
    gtk_range_set_value(GTK_RANGE(vol_scale), 50);
    g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_vol_changed), NULL);
    gtk_box_pack_start(GTK_BOX(left), vol_scale, TRUE, TRUE, 0);
    vol_pct_label = gtk_label_new("50%");
    qy_add_class(vol_pct_label, "qy-audio-vol-pct");
    gtk_box_pack_start(GTK_BOX(left), vol_pct_label, FALSE, FALSE, 0);
    mute_btn = gtk_toggle_button_new_with_label(TR("静音"));
    qy_add_class(mute_btn, "qy-audio-mute");
    g_signal_connect(mute_btn, "toggled", G_CALLBACK(on_mute_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(left), mute_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_h), left, FALSE, FALSE, 0);

    /* 右：设备列表 */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *dev_title = gtk_label_new(TR("音频设备"));
    qy_add_class(dev_title, "qy-audio-title");
    gtk_widget_set_halign(dev_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right), dev_title, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    dev_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dev_list), GTK_SELECTION_NONE);
    qy_add_class(dev_list, "qy-audio-devlist");
    gtk_container_add(GTK_CONTAINER(sw), dev_list);
    gtk_box_pack_start(GTK_BOX(right), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_h), right, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    /* 底部：高/中/低音占位滑杆 */
    GtkWidget *eq_h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    const char *eq_names[3] = { "高音", "中音", "低音" };
    for (int i = 0; i < 3; i++) {
        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *lb = gtk_label_new(TR(eq_names[i]));
        qy_add_class(lb, "qy-audio-eq-label");
        GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        qy_add_class(sc, "qy-audio-eq");
        gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
        gtk_range_set_value(GTK_RANGE(sc), 50);
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_eq_changed), NULL);
        if (i == 0) high_scale = sc;
        else if (i == 1) mid_scale = sc;
        else low_scale = sc;
        gtk_box_pack_start(GTK_BOX(vb), lb, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vb), sc, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(eq_h), vb, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), eq_h, FALSE, FALSE, 0);

    gtk_widget_show_all(win);

    /* 初始化 ALSA */
    if (alsa_open()) {
        refresh_from_alsa();
    } else {
        gtk_widget_show(err_label);
        gtk_widget_set_sensitive(vol_scale, FALSE);
        gtk_widget_set_sensitive(mute_btn, FALSE);
    }

    rebuild_devlist();
    eq_load();

    gtk_main();
    return 0;
}