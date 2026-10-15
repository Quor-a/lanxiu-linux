/* qynet - 澜岫网络管理器 (GTK3)
 * 显示网络接口列表: 接口名 / 状态 / IPv4 / MAC
 * 数据源: ip -o link show + ip -o -4 addr show（netlink，不依赖 /sys）
 * 连接: 无 IP 时 ip link up + udhcpc；已有 IP 显示已连接
 * 自动化: QYNET_CONNECT=eth0 启动后自动执行连接动作
 * 架构: 单文件 GTK3，与 qyfiles 同款编译方式
 */
#include "qyl10n.h"
#include "qytheme.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

enum { C_IF, C_STATE, C_IP, C_MAC, N_COLS };

static GtkListStore *store = NULL;
static GtkWidget *count_label = NULL;
static GtkWidget *status_label = NULL;
static GtkListBox *wifi_list = NULL;        /* #qynet-wifi-list */
static GtkWidget *wifi_status_label = NULL; /* 「未检测到无线网卡」等提示 */
static GtkWidget *disconn_btn = NULL;        /* 以太网「断开」按钮（release 298） */

/* 以太网接口枚举（release 298）：定义在 refresh_list 之后 */
static gboolean has_eth_iface(void);

/* ---------- 实时流量曲线 ---------- */
#define RATE_HISTORY 60
static double rate_hist[RATE_HISTORY] = {0};   /* 历史速率（字节/秒） */
static int rate_pos = 0;                       /* 下一个写入位置 */
static int rate_count = 0;                    /* 已采样的有效点数 */
static guint64 prev_bytes = 0;                /* 上一次总字节数 */
static gboolean have_prev = FALSE;            /* 是否有上一次采样 */
static gint64 prev_time = 0;                   /* 上一次采样时间（us） */
static GtkWidget *rate_label = NULL;          /* #qynet-rate */
static GtkWidget *graph = NULL;               /* #qynet-graph */

/* 读取 /proc/net/dev 所有非 lo 接口的总 RX+TX 字节数 */
static guint64 read_net_total(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return 0;
    char line[512];
    guint64 total = 0;
    /* 跳过两行表头 */
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        size_t n = (size_t)(colon - line);
        char ifname[64];
        if (n >= sizeof ifname) n = sizeof ifname - 1;
        memcpy(ifname, line, n);
        ifname[n] = 0;
        g_strstrip(ifname);
        if (!strcmp(ifname, "lo")) continue;
        unsigned long long rx = 0, tx = 0;
        /* 格式:  rx_bytes packets errs drop fifo frame compressed multicast  tx_bytes ...
         * 中间 7 个字段用 %*u 跳过（数值可能超过 32 位，但跳过时溢出不影响消费整个 token） */
        if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   &rx, &tx) >= 1)
            total += rx + tx;
    }
    fclose(f);
    return total;
}

/* 每秒采样一次：差分算出速率，写入 60 点历史，更新标签并重绘曲线 */
static gboolean rate_tick(gpointer ud) {
    (void)ud;
    guint64 now_bytes = read_net_total();
    gint64 now_time = g_get_monotonic_time();
    double rate = 0.0;
    if (have_prev && now_time > prev_time) {
        double dt = (now_time - prev_time) / 1e6;   /* 秒 */
        if (dt > 0) {
            if (now_bytes >= prev_bytes)
                rate = (double)(now_bytes - prev_bytes) / dt;
            else
                rate = 0.0;   /* 网卡重启/计数器复位：按 0 处理 */
        }
    }
    prev_bytes = now_bytes;
    prev_time = now_time;
    have_prev = TRUE;

    rate_hist[rate_pos] = rate;
    rate_pos = (rate_pos + 1) % RATE_HISTORY;
    if (rate_count < RATE_HISTORY) rate_count++;

    if (rate_label) {
        gchar *txt = g_strdup_printf(TR("当前速率: %.0f KB/s"), rate / 1024.0);
        gtk_label_set_text(GTK_LABEL(rate_label), txt);
        g_free(txt);
    }
    if (graph) gtk_widget_queue_draw(graph);
    return G_SOURCE_CONTINUE;
}

/* 曲线绘制：深蓝底 #101826，橙线 #FF5C1A（下行段用绿色） */
static gboolean graph_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int W = a.width, H = a.height;

    /* 深蓝底 */
    cairo_set_source_rgb(cr, 0x10 / 255.0, 0x18 / 255.0, 0x26 / 255.0);  /* #101826 */
    cairo_paint(cr);
    if (W < 10 || H < 10) return FALSE;

    /* 横向网格线 */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i < 4; i++) {
        double y = H * i / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, W, y);
        cairo_stroke(cr);
    }
    /* 基线 */
    cairo_move_to(cr, 0, H - 1);
    cairo_line_to(cr, W, H - 1);
    cairo_stroke(cr);

    /* 最大值（至少 1，避免除零） */
    double maxv = 1.0;
    for (int i = 0; i < rate_count; i++)
        if (rate_hist[i] > maxv) maxv = rate_hist[i];
    if (maxv <= 0) maxv = 1.0;

    int n = rate_count;
    if (n < 2) n = 2;
    double prev_x = 0, prev_y = 0, prev_v = 0;
    gboolean started = FALSE;
    for (int i = 0; i < n; i++) {
        int idx = (rate_pos - n + i + RATE_HISTORY) % RATE_HISTORY;  /* 最旧 → 最新 */
        double v = rate_hist[idx];
        double x = (n > 1) ? (double)i / (n - 1) * W : 0;
        double y = H - 6 - (v / maxv) * (H - 16);
        if (!started) {
            started = TRUE;
            prev_x = x; prev_y = y; prev_v = v;
            continue;
        }
        /* 下行（当前样本低于前一样本）用绿色，否则品牌橙 #FF5C1A */
        if (v < prev_v)
            cairo_set_source_rgb(cr, 0.20, 0.85, 0.35);    /* 绿色下行 */
        else
            cairo_set_source_rgb(cr, 1.0, 0x5C / 255.0, 0x1A / 255.0);  /* #FF5C1A */
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, prev_x, prev_y);
        cairo_line_to(cr, x, y);
        cairo_stroke(cr);
        prev_x = x; prev_y = y; prev_v = v;
    }
    return FALSE;
}

/* 读取指定接口的 IPv4 地址（多个用空格分隔） */
static void read_ip(char *buf, size_t n, const char *ifname) {
    buf[0] = 0;
    char cmd[128];
    snprintf(cmd, sizeof cmd, "ip -o -4 addr show %s 2>/dev/null", ifname);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "inet ");
        if (p) {
            p += 5;
            char *sp = strchr(p, ' ');
            if (sp) *sp = 0;
            if (buf[0]) strncat(buf, " ", n - strlen(buf) - 1);
            strncat(buf, p, n - strlen(buf) - 1);
        }
    }
    pclose(f);
}

/* 获取当前选中的接口名（没有选中时取第一个非 lo 接口） */
static const char *selected_ifname(void) {
    static char buf[64] = "";
    buf[0] = 0;
    /* 简化：直接读第一个非 lo 接口 */
    FILE *f = popen("ip -o link show 2>/dev/null | grep -v ': lo:' | head -1", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof line, f)) {
            char *colon = strchr(line, ':');
            if (colon) {
                char tmp[64];
                if (sscanf(line, "%*d: %63[^:]:", tmp) == 1)
                    snprintf(buf, sizeof buf, "%s", tmp);
            }
        }
        pclose(f);
    }
    return buf;
}

/* 刷新接口列表 */
static void refresh_list(void) {
    gtk_list_store_clear(store);
    FILE *f = popen("ip -o link show 2>/dev/null", "r");
    if (!f) return;
    char line[1024];
    int n_if = 0, n_up = 0;
    while (fgets(line, sizeof line, f)) {
        /* 格式: 2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 ... state UP ... link/ether 52:54:00:.. brd .. */
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char ifname[64] = "";
        if (sscanf(line, "%*d: %63[^:]:", ifname) != 1 || !ifname[0])
            continue;
        /* 状态: state UP / DOWN / UNKNOWN */
        char state[32] = "-";
        char *st = strstr(line, "state ");
        if (st) {
            st += 6;
            char *sp = strchr(st, ' ');
            if (sp) *sp = 0;
            snprintf(state, sizeof state, "%s", st);
        }
        /* MAC: link/ether 52:54:... 或 link/loopback 00:00:... */
        char mac[64] = "-";
        char *macp = strstr(line, "link/");
        if (macp) {
            macp += 5;
            if (sscanf(macp, "%*s %63s", mac) != 1) {
                char *sp = strchr(macp, ' ');
                if (sp) *sp = 0;
                snprintf(mac, sizeof mac, "%s", macp);
            }
        }
        /* IPv4 */
        char ip[256] = "-";
        read_ip(ip, sizeof ip, ifname);
        if (!ip[0]) snprintf(ip, sizeof ip, "-");
        /* 在线统计: state UP 且非 lo */
        if (strcmp(state, "UP") == 0 && strcmp(ifname, "lo") != 0) n_up++;
        n_if++;
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, C_IF, ifname, C_STATE, state,
                              C_IP, ip, C_MAC, mac, -1);
    }
    pclose(f);
    if (count_label) {
        gchar *txt = g_strdup_printf(TR("接口 %d 个 · 在线 %d"), n_if, n_up);
        gtk_label_set_text(GTK_LABEL(count_label), txt);
        g_free(txt);
    }
    /* 无以太网接口时禁用「断开」按钮（release 298） */
    if (disconn_btn)
        gtk_widget_set_sensitive(disconn_btn, has_eth_iface());
}

/* 读取 DNS 服务器（release 275）：/etc/resolv.conf nameserver 前 2 个 */
static void append_dns_str(GString *gs) {
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < 2) {
        if (strncmp(line, "nameserver", 10) == 0) {
            char *p = line + 10;
            while (*p == ' ' || *p == '\t') p++;
            char *e = p;
            while (*e && *e != '\n' && *e != ' ') e++;
            *e = 0;
            if (n == 0) g_string_append_printf(gs, TR(" DNS: %s"), p);
            else g_string_append_printf(gs, " %s", p);
            n++;
        }
    }
    fclose(f);
}

/* 连接以太网：已有 IP 视为已连接；无 IP 则 up + udhcpc */
static void do_connect(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *ifname = ud ? (const char *)ud : selected_ifname();
    if (!ifname || !ifname[0]) return;
    char ip[256] = "-";
    read_ip(ip, sizeof ip, ifname);
    if (ip[0] && strcmp(ip, "-") != 0 && strcmp(ip, "") != 0) {
        g_printerr("QYNETDBG: connect %s already up (%s)\n", ifname, ip);
        if (status_label) {
            GString *gs = g_string_new("");
            g_string_printf(gs, TR("已连接 %s（%s）"), ifname, ip);
            append_dns_str(gs);
            gtk_label_set_text(GTK_LABEL(status_label), gs->str);
            g_string_free(gs, TRUE);
        }
        return;
    }
    g_printerr("QYNETDBG: connect %s (dhcp)\n", ifname);
    if (status_label)
        gtk_label_set_text(GTK_LABEL(status_label), TR("正在连接..."));
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "ip link set dev %s up 2>/dev/null; udhcpc -i %s >/dev/null 2>&1 &",
             ifname, ifname);
    g_spawn_command_line_async(cmd, NULL);
    g_timeout_add(3000, (GSourceFunc)refresh_list, NULL);
    if (status_label) {
        gchar *s = g_strdup_printf(TR("%s 连接请求已发送"), ifname);
        gtk_label_set_text(GTK_LABEL(status_label), s);
        g_free(s);
    }
}

/* ---------- 以太网断开（release 298） ---------- */

/* 获取第一个以太网接口（非 lo、非 wlan/wl/wifi；枚举失败回退 eth0） */
static const char *find_eth_iface(void) {
    static char buf[64] = "eth0";
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &it, C_IF, &name, -1);
            gboolean is_wifi = name && (strncmp(name, "wlan", 4) == 0 ||
                                        strncmp(name, "wl", 2) == 0 ||
                                        strncmp(name, "wifi", 4) == 0);
            if (name && strcmp(name, "lo") != 0 && !is_wifi) {
                g_strlcpy(buf, name, sizeof buf);
                g_free(name);
                return buf;
            }
            g_free(name);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    }
    return buf;
}

/* 是否存在以太网接口（非 lo、非 wlan/wl/wifi） */
static gboolean has_eth_iface(void) {
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &it, C_IF, &name, -1);
            gboolean is_wifi = name && (strncmp(name, "wlan", 4) == 0 ||
                                        strncmp(name, "wl", 2) == 0 ||
                                        strncmp(name, "wifi", 4) == 0);
            gboolean is_eth = name && strcmp(name, "lo") != 0 && !is_wifi;
            g_free(name);
            if (is_eth) return TRUE;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    }
    return FALSE;
}

/* 断开以太网：确认后 ip link down，并刷新状态（release 298） */
static void do_disconnect(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *ifname = ud ? (const char *)ud : find_eth_iface();
    if (!ifname || !ifname[0]) return;
    gchar *msg = g_strdup_printf(TR("确认断开以太网 %s?"), ifname);
    GtkWidget *cdlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                             GTK_BUTTONS_YES_NO, "%s", msg);
    g_free(msg);
    int cans = gtk_dialog_run(GTK_DIALOG(cdlg));
    gtk_widget_destroy(cdlg);
    if (cans != GTK_RESPONSE_YES) return;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "ip link set dev %s down 2>/dev/null", ifname);
    g_spawn_command_line_async(cmd, NULL);
    if (status_label) {
        gchar *s = g_strdup_printf(TR("已断开 %s"), ifname);
        gtk_label_set_text(GTK_LABEL(status_label), s);
        g_free(s);
    }
    g_timeout_add(3000, (GSourceFunc)refresh_list, NULL);
}

/* ---------- WiFi 热点连接（wpa_supplicant + udhcpc） ---------- */
static const char *find_wlan_iface(void) {
    static char buf[32] = "wlan0";
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &it, C_IF, &name, -1);
            if (name && strncmp(name, "wlan", 4) == 0) {
                g_strlcpy(buf, name, sizeof buf);
                g_free(name);
                return buf;
            }
            g_free(name);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    }
    return buf;
}

/* ---------- WiFi 扫描显示 ---------- */
static gboolean has_program(const char *name) {
    gchar *p = g_find_program_in_path(name);
    gboolean ok = (p != NULL);
    g_free(p);
    return ok;
}

/* 检测系统是否存在无线网卡（接口名以 wlan/wl/wifi 开头） */
static gboolean has_wlan_iface(void) {
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &it, C_IF, &name, -1);
            gboolean w = name && (strncmp(name, "wlan", 4) == 0 ||
                                  strncmp(name, "wl", 2) == 0 ||
                                  strncmp(name, "wifi", 4) == 0);
            g_free(name);
            if (w) return TRUE;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    }
    return FALSE;
}

/* 清空 WiFi 列表 */
static void wifi_list_clear(void) {
    if (!wifi_list) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(wifi_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

/* 添加一行 WiFi: SSID + 信号强度（%）+ 文本信号条（▂▄▆█ 块字符，非 emoji） */
static void wifi_list_add(const char *ssid, int sig, gboolean enc) {
    if (!ssid || !ssid[0]) return;
    if (sig < 0) sig = 0;
    if (sig > 100) sig = 100;
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 8);
    gtk_widget_set_margin_end(hbox, 8);
    gtk_widget_set_margin_top(hbox, 3);
    gtk_widget_set_margin_bottom(hbox, 3);
    gchar *ssid_text = g_strdup_printf("%s %s", enc ? "🔒" : "·", ssid);
    GtkWidget *ssid_lbl = gtk_label_new(ssid_text);
    g_free(ssid_text);
    gtk_widget_set_halign(ssid_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(ssid_lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), ssid_lbl, TRUE, TRUE, 0);
    const char *bars = "▂";
    if (sig >= 75) bars = "▂▄▆█";
    else if (sig >= 50) bars = "▂▄▆";
    else if (sig >= 25) bars = "▂▄";
    gchar *sig_txt = g_strdup_printf("%s %d%%", bars, sig);
    GtkWidget *sig_lbl = gtk_label_new(sig_txt);
    gtk_widget_set_halign(sig_lbl, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(hbox), sig_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), hbox);
    gchar *tip = g_strdup_printf("%s — %d%%", ssid, sig);
    gtk_widget_set_tooltip_text(row, tip);
    g_free(tip);
    g_object_set_data_full(G_OBJECT(row), "ssid", g_strdup(ssid), g_free);
    gtk_list_box_insert(wifi_list, row, -1);
    gtk_widget_show_all(row);
}

/* 显示/隐藏扫描状态提示 */
static void wifi_show_status(const char *msg) {
    if (!wifi_status_label) return;
    gtk_label_set_text(GTK_LABEL(wifi_status_label), msg);
    gtk_widget_set_no_show_all(wifi_status_label, FALSE);
    gtk_widget_show(wifi_status_label);
}
static void wifi_hide_status(void) {
    if (!wifi_status_label) return;
    gtk_widget_set_no_show_all(wifi_status_label, TRUE);
    gtk_widget_hide(wifi_status_label);
}

/* 扫描并显示周围 WiFi（优先 nmcli，回退 iw dev <wlan> scan）。
 * 无无线网卡时显示「未检测到无线网卡」，不崩溃。 */
static void wifi_scan(void) {
    wifi_list_clear();
    wifi_hide_status();
    if (!has_wlan_iface()) {
        g_printerr("QYNETWIFI: no wireless interface, hiding scan\n");
        wifi_show_status(TR("未检测到无线网卡"));
        return;
    }
    gboolean any = FALSE;

    /* 优先 nmcli: 信号强度直接是百分比 */
    if (has_program("nmcli")) {
        gchar *out = NULL, *err = NULL;
        gint status = 0;
        if (g_spawn_command_line_sync("nmcli -t -f SSID,SIGNAL dev wifi list",
                                      &out, &err, &status, NULL) && out) {
            char *line = out;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                char *last_colon = strrchr(line, ':');
                if (last_colon && last_colon != line) {
                    char *ssid = g_strndup(line, last_colon - line);
                    int sig = atoi(last_colon + 1);
                    /* nmcli 用 \: 转义冒号，还原为 : */
                    gchar *esc = strstr(ssid, "\\:");
                    while (esc) {
                        memmove(esc, esc + 1, strlen(esc));
                        esc = strstr(ssid, "\\:");
                    }
                    if (ssid[0] && ssid[0] != ':') {
                        wifi_list_add(ssid, sig, FALSE);
                        any = TRUE;
                    }
                    g_free(ssid);
                }
                line = nl ? nl + 1 : NULL;
            }
        }
        g_free(out);
        g_free(err);
    }

    /* 回退 iw dev <wlan> scan: 信号 dBm → %（-100dBm=0%，-50dBm=100%） */
    if (!any && has_program("iw")) {
        const char *iface = find_wlan_iface();
        gchar *cmd = g_strdup_printf("iw dev %s scan 2>/dev/null", iface);
        gchar *out = NULL, *err = NULL;
        gint status = 0;
        if (g_spawn_command_line_sync(cmd, &out, &err, &status, NULL) && out) {
            char *line = out;
            char *cur_ssid = NULL;
            double cur_signal = 0;
            gboolean have_signal = FALSE;
            gboolean cur_enc = FALSE;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                if (strncmp(line, "BSS ", 4) == 0) {
                    /* 新 BSS: 提交上一个 */
                    if (cur_ssid) {
                        int pct = have_signal ? (int)((cur_signal + 100.0) * 2.0) : 0;
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        wifi_list_add(cur_ssid, pct, cur_enc);
                        any = TRUE;
                        g_free(cur_ssid);
                        cur_ssid = NULL;
                        have_signal = FALSE;
                        cur_enc = FALSE;
                    }
                } else if (strncmp(line, "\tSSID:", 6) == 0) {
                    g_free(cur_ssid);
                    cur_ssid = g_strstrip(g_strdup(line + 6));
                } else if (strncmp(line, "\tsignal:", 8) == 0) {
                    cur_signal = atof(line + 8);
                    have_signal = TRUE;
                } else if (strncmp(line, "\tRSN:", 5) == 0 || strncmp(line, "\tWPA:", 5) == 0) {
                    cur_enc = TRUE;
                }
                line = nl ? nl + 1 : NULL;
            }
            /* 提交最后一个 */
            if (cur_ssid) {
                int pct = have_signal ? (int)((cur_signal + 100.0) * 2.0) : 0;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                wifi_list_add(cur_ssid, pct, cur_enc);
                any = TRUE;
                g_free(cur_ssid);
            }
        }
        g_free(cmd);
        g_free(out);
        g_free(err);
    }

    if (!any)
        wifi_show_status(TR("未检测到无线网卡"));
    g_printerr("QYNETWIFI: scan done ssids=%d\n", any ? 1 : 0);
}

static void wifi_connect(const char *ssid, const char *psk) {
    if (!ssid || !ssid[0]) return;
    const char *ifname = find_wlan_iface();
    /* 生成 wpa_supplicant.conf（写入 /tmp，避免污染系统配置） */
    GString *conf = g_string_new("network={\n");
    g_string_append_printf(conf, "    ssid=\"%s\"\n", ssid);
    if (psk && psk[0])
        g_string_append_printf(conf, "    psk=\"%s\"\n", psk);
    else
        g_string_append(conf, "    key_mgmt=NONE\n");
    g_string_append(conf, "}\n");
    g_file_set_contents("/tmp/qywifi.conf", conf->str, conf->len, NULL);
    g_string_free(conf, TRUE);
    g_printerr("QYNETWIFI: connect ssid=%s iface=%s\n", ssid, ifname);
    gchar *cmd = g_strdup_printf(
        "wpa_supplicant -B -i %s -c /tmp/qywifi.conf >/dev/null 2>&1; "
        "udhcpc -i %s >/dev/null 2>&1 &", ifname, ifname);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    /* 记住最近连接（release 269） */
    gchar *wdir = g_build_filename(g_get_home_dir(), ".config", "lanxiu", NULL);
    g_mkdir_with_parents(wdir, 0755);
    gchar *wpath = g_build_filename(wdir, "qynet-wifi.conf", NULL);
    gchar *wdata = g_strdup_printf("%s|%s", ssid, psk ? psk : "");
    g_file_set_contents(wpath, wdata, -1, NULL);
    g_free(wdata); g_free(wpath); g_free(wdir);
    if (status_label) {
        gchar *s = g_strdup_printf(TR("正在连接 WiFi %s（%s）..."), ssid, ifname);
        gtk_label_set_text(GTK_LABEL(status_label), s);
        g_free(s);
    }
    g_timeout_add(3000, (GSourceFunc)refresh_list, NULL);
}

/* 状态栏点击复制当前信息（release 281） */
static gboolean on_status_clicked(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w; (void)ev; (void)ud;
    const char *txt = status_label ? gtk_label_get_text(GTK_LABEL(status_label)) : "";
    if (txt && txt[0]) {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, txt, -1);
    }
    return TRUE;
}

/* 显示/隐藏 WiFi 密码（release 281） */
static void on_psk_show_toggled(GtkToggleButton *btn, gpointer ud) {
    GtkEntry *e = GTK_ENTRY(ud);
    gtk_entry_set_visibility(e, gtk_toggle_button_get_active(btn));
}

static void on_wifi_clicked(GtkWidget *w, gpointer ud) {
    (void)w;
    const char *preset_ssid = ud ? (const char *)ud : NULL;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(TR("连接 WiFi"), NULL,
        GTK_DIALOG_MODAL, TR("连接"), GTK_RESPONSE_OK,
        TR("断开"), GTK_RESPONSE_REJECT,
        TR("取消"), GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *ssid_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ssid_e), TR("WiFi 名称 (SSID)"));
    if (preset_ssid && preset_ssid[0])
        gtk_entry_set_text(GTK_ENTRY(ssid_e), preset_ssid);
    GtkWidget *psk_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(psk_e), TR("密码（开放网络留空）"));
    gtk_entry_set_visibility(GTK_ENTRY(psk_e), FALSE);
    gtk_box_pack_start(GTK_BOX(box), ssid_e, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), psk_e, FALSE, FALSE, 6);
    GtkWidget *show_cb = gtk_check_button_new_with_label(TR("显示密码"));
    g_signal_connect(show_cb, "toggled", G_CALLBACK(on_psk_show_toggled), psk_e);
    gtk_box_pack_start(GTK_BOX(box), show_cb, FALSE, FALSE, 6);
    gtk_widget_show_all(dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_OK) {
        const char *ssid = gtk_entry_get_text(GTK_ENTRY(ssid_e));
        const char *psk = gtk_entry_get_text(GTK_ENTRY(psk_e));
        wifi_connect(ssid, psk);
    } else if (resp == GTK_RESPONSE_REJECT) {
        /* 断开 WiFi（release 270）——增加确认（release 282） */
        GtkWidget *cdlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                 GTK_BUTTONS_YES_NO, "%s", TR("确认断开当前 WiFi?"));
        int cans = gtk_dialog_run(GTK_DIALOG(cdlg));
        gtk_widget_destroy(cdlg);
        if (cans != GTK_RESPONSE_YES) {
            gtk_widget_destroy(dlg);
            return;
        }
        g_spawn_command_line_async("pkill -x wpa_supplicant 2>/dev/null; pkill -x udhcpc 2>/dev/null; true", NULL);
        if (status_label)
            gtk_label_set_text(GTK_LABEL(status_label), TR("已断开 WiFi"));
        g_timeout_add(3000, (GSourceFunc)refresh_list, NULL);
    }
    gtk_widget_destroy(dlg);
}

/* 点击扫描列表中的 WiFi 行: 沿用现有连接逻辑（预填 SSID 的对话框） */
static void on_wifi_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    const char *ssid = g_object_get_data(G_OBJECT(row), "ssid");
    if (ssid && ssid[0])
        on_wifi_clicked(NULL, (gpointer)ssid);
}

/* ---------- WiFi 忘记网络（release 299） ---------- */

/* 判断配置行是否属于指定 SSID。
 * qynet-wifi.conf 实际格式为 "SSID|密码"（wifi_connect 写入），
 * 这里同时兼容 "SSID=密码" 与无分隔符的裸 SSID 行。 */
static gboolean wifi_line_is(const char *line, const char *ssid) {
    gchar *copy = g_strdup(line);
    gchar *trimmed = g_strstrip(copy);
    if (!trimmed[0]) { g_free(copy); return FALSE; }
    gboolean match = FALSE;
    char *sep = strchr(trimmed, '|');
    if (!sep) sep = strchr(trimmed, '=');
    if (sep) {
        *sep = 0;
        match = (strcmp(trimmed, ssid) == 0);
    } else {
        match = (strcmp(trimmed, ssid) == 0);
    }
    g_free(copy);
    return match;
}

/* 检查指定 SSID 是否已保存在 wifi 配置文件中 */
static gboolean wifi_password_has(const char *ssid) {
    if (!ssid || !ssid[0]) return FALSE;
    gchar *wpath = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qynet-wifi.conf", NULL);
    gchar *data = NULL;
    gboolean found = FALSE;
    if (g_file_get_contents(wpath, &data, NULL, NULL) && data && data[0]) {
        char *p = data;
        while (p && *p && !found) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            if (wifi_line_is(p, ssid)) found = TRUE;
            p = nl ? nl + 1 : NULL;
        }
    }
    g_free(data);
    g_free(wpath);
    return found;
}

/* 从 wifi 配置文件删除指定 SSID 条目：逐行过滤重写，保留其他条目。
 * 返回删除的条目数。 */
static int wifi_password_forget(const char *ssid) {
    if (!ssid || !ssid[0]) return 0;
    gchar *wpath = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qynet-wifi.conf", NULL);
    gchar *data = NULL;
    gsize len = 0;
    int removed = 0;
    if (!g_file_get_contents(wpath, &data, &len, NULL) || !data) {
        g_free(data);
        g_free(wpath);
        return 0;
    }
    GString *out = g_string_new(NULL);
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        if (wifi_line_is(p, ssid)) {
            removed++;
        } else {
            g_string_append_len(out, p, linelen);
            if (nl) g_string_append_c(out, '\n');
        }
        p = nl ? nl + 1 : NULL;
    }
    if (removed > 0)
        g_file_set_contents(wpath, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    g_free(data);
    g_free(wpath);
    return removed;
}

/* 获取当前连接的 WiFi SSID（iw dev <iface> link），未连接返回空串 */
static const char *current_wifi_ssid(void) {
    static char buf[128] = "";
    buf[0] = 0;
    if (!has_program("iw")) return buf;
    const char *iface = find_wlan_iface();
    gchar *cmd = g_strdup_printf("iw dev %s link 2>/dev/null", iface);
    gchar *out = NULL, *err = NULL;
    gint status = 0;
    if (g_spawn_command_line_sync(cmd, &out, &err, &status, NULL) && out) {
        char *line = out;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (strncmp(line, "SSID:", 5) == 0) {
                g_strlcpy(buf, g_strstrip(line + 5), sizeof buf);
                break;
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    g_free(cmd);
    g_free(out);
    g_free(err);
    return buf;
}

/* 「忘记网络」按钮（release 299）：删除已保存的 WiFi 密码条目 */
static void on_wifi_forget_clicked(GtkWidget *w, gpointer ud) {
    (void)w;
    (void)ud;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(wifi_list);
    const char *ssid = row ? g_object_get_data(G_OBJECT(row), "ssid") : NULL;
    if (!ssid || !ssid[0]) {
        wifi_show_status(TR("请先选择要忘记的 WiFi"));
        return;
    }
    if (!wifi_password_has(ssid)) {
        wifi_show_status(TR("该网络未保存，无需忘记"));
        return;
    }
    gchar *msg = g_strdup_printf(TR("忘记已保存的网络 %s?"), ssid);
    GtkWidget *cdlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                             GTK_BUTTONS_YES_NO, "%s", msg);
    g_free(msg);
    int cans = gtk_dialog_run(GTK_DIALOG(cdlg));
    gtk_widget_destroy(cdlg);
    if (cans != GTK_RESPONSE_YES) return;

    if (wifi_password_forget(ssid) > 0) {
        const char *cur = current_wifi_ssid();
        gchar *s;
        if (cur && cur[0] && strcmp(cur, ssid) == 0)
            s = g_strdup_printf(TR("已忘记 %s（已断开该网络的自动连接）"), ssid);
        else
            s = g_strdup_printf(TR("已忘记 %s"), ssid);
        wifi_show_status(s);
        g_free(s);
    } else {
        wifi_show_status(TR("该网络未保存，无需忘记"));
    }
    refresh_list();
}

/* 自动化: QYNET_WIFI=SSID|密码 */
static gboolean auto_wifi(gpointer p) {
    char *arg = (char *)p;
    char *bar = strchr(arg, '|');
    if (bar) *bar = 0;
    wifi_connect(arg, bar ? bar + 1 : "");
    g_free(arg);
    return G_SOURCE_REMOVE;
}

/* 自动化: QYNET_CONNECT=eth0 */
static gboolean auto_connect(gpointer p) {
    do_connect(NULL, p);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫网络管理器"));
    gtk_window_set_default_size(GTK_WINDOW(win), 620, 660);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* WiFi 扫描区 (#qynet-wifi-list): 扫描按钮 + 状态 + 列表 */
    {
        GtkWidget *wifi_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *wifi_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *wifi_title = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(wifi_title), TR("<b>WiFi 网络</b>"));
        gtk_widget_set_halign(wifi_title, GTK_ALIGN_START);
        GtkWidget *b_scan = gtk_button_new_with_label(TR("扫描 WiFi"));
        qy_add_class(b_scan, "qy-btn");
        g_signal_connect(b_scan, "clicked", G_CALLBACK(wifi_scan), NULL);
        gtk_box_pack_start(GTK_BOX(wifi_hdr), wifi_title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(wifi_hdr), b_scan, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(wifi_box), wifi_hdr, FALSE, FALSE, 0);

        wifi_status_label = gtk_label_new(TR("未检测到无线网卡"));
        gtk_widget_set_halign(wifi_status_label, GTK_ALIGN_START);
        gtk_widget_set_no_show_all(wifi_status_label, TRUE);
        gtk_widget_hide(wifi_status_label);
        gtk_box_pack_start(GTK_BOX(wifi_box), wifi_status_label, FALSE, FALSE, 0);

        wifi_list = GTK_LIST_BOX(gtk_list_box_new());
        gtk_widget_set_name(GTK_WIDGET(wifi_list), "qynet-wifi-list");
        g_signal_connect(wifi_list, "row-activated", G_CALLBACK(on_wifi_row_activated), NULL);
        GtkWidget *wifi_sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_widget_set_size_request(wifi_sw, -1, 150);
        gtk_container_add(GTK_CONTAINER(wifi_sw), GTK_WIDGET(wifi_list));
        gtk_box_pack_start(GTK_BOX(wifi_box), wifi_sw, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), wifi_box, FALSE, FALSE, 0);
    }

    /* 列表 */
    store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    const char *titles[N_COLS] = { TR("接口"), TR("状态"), TR("IP 地址"), TR("MAC 地址") };
    for (int i = 0; i < N_COLS; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            titles[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    /* 底部: 连接/刷新按钮 + 状态 */
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *b_conn = gtk_button_new_with_label(TR("连接"));
    qy_add_class(b_conn, "qy-btn");
    g_signal_connect(b_conn, "clicked", G_CALLBACK(do_connect), NULL);
    GtkWidget *b_disconn = gtk_button_new_with_label(TR("断开"));
    qy_add_class(b_disconn, "qy-btn");
    g_signal_connect(b_disconn, "clicked", G_CALLBACK(do_disconnect), NULL);
    disconn_btn = b_disconn;
    GtkWidget *b_refresh = gtk_button_new_with_label(TR("刷新"));
    gtk_widget_set_tooltip_text(b_refresh, TR("重新扫描网络接口"));
    qy_add_class(b_refresh, "qy-btn");
    g_signal_connect(b_refresh, "clicked", G_CALLBACK(refresh_list), NULL);
    GtkWidget *b_wifi = gtk_button_new_with_label(TR("WiFi 连接"));
    qy_add_class(b_wifi, "qy-btn");
    g_signal_connect(b_wifi, "clicked", G_CALLBACK(on_wifi_clicked), NULL);
    GtkWidget *b_forget = gtk_button_new_with_label(TR("忘记网络"));
    qy_add_class(b_forget, "qy-btn");
    gtk_widget_set_tooltip_text(b_forget, TR("删除选中 WiFi 的已保存密码"));
    g_signal_connect(b_forget, "clicked", G_CALLBACK(on_wifi_forget_clicked), NULL);
    count_label = gtk_label_new("");
    status_label = gtk_label_new("");
    GtkWidget *status_eb = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(status_eb), status_label);
    gtk_widget_set_tooltip_text(status_eb, TR("点击复制当前状态信息"));
    g_signal_connect(status_eb, "button-press-event", G_CALLBACK(on_status_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hb), b_conn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_disconn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_wifi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), b_forget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), count_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_eb, FALSE, FALSE, 0);

    /* 实时流量曲线 */
    rate_label = gtk_label_new(TR("当前速率: 0 KB/s"));
    qy_add_class(rate_label, "qy-mon-info");
    gtk_widget_set_name(rate_label, "qynet-rate");
    gtk_widget_set_halign(rate_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), rate_label, FALSE, FALSE, 0);

    graph = gtk_drawing_area_new();
    gtk_widget_set_name(graph, "qynet-graph");
    gtk_widget_set_size_request(graph, -1, 120);
    g_signal_connect(graph, "draw", G_CALLBACK(graph_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), graph, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    refresh_list();
    wifi_scan();                          /* 启动时扫描一次 WiFi */
    rate_tick(NULL);                      /* 立即采样一次 */
    g_timeout_add_seconds(1, rate_tick, NULL);

    /* 自动化: QYNET_CONNECT=eth0 自动连接 */
    const char *qc = g_getenv("QYNET_CONNECT");
    if (qc && qc[0])
        g_timeout_add(600, auto_connect, g_strdup(qc));
    /* 自动化: QYNET_WIFI=SSID|密码 自动连接 WiFi */
    const char *qw = g_getenv("QYNET_WIFI");
    if (qw && qw[0])
        g_timeout_add(800, auto_wifi, g_strdup(qw));
    /* 记住的 WiFi 自动重连（release 269） */
    if (!(qw && qw[0])) {
        gchar *wpath = g_build_filename(g_get_home_dir(), ".config", "lanxiu", "qynet-wifi.conf", NULL);
        gchar *wdata = NULL;
        if (g_file_get_contents(wpath, &wdata, NULL, NULL) && wdata && wdata[0])
            g_timeout_add(900, auto_wifi, g_strstrip(wdata));
        g_free(wdata); g_free(wpath);
    }

    gtk_main();
    return 0;
}