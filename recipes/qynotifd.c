/* qynotifd.c — 澜岫通知/设置守护进程
 * 1) 监控 /etc/qy*.conf 变化：为每个开关写入发送"设置已更改"通知
 *    （让 qysettings 的空壳开关有消费端）
 * 2) 消费 qyautostart.conf：内容每行作为命令启动
 * 3) 消费 qynotif.conf：qynotif=off 时禁用通知；qymon=on/off 控制系统监视通知
 * 4) 网络/防火墙/蓝牙等特殊动作（环境无对应工具时仅记日志）
 * 通知统一走 /tmp/qynotif/latest.msg（qydesktop 顶栏显示）。
 */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>

static gboolean notif_enabled = TRUE;
static gboolean mon_enabled = FALSE;

/* 发送通知（写入 latest.msg + 追加 history.log 历史 + 持久化到用户数据目录） */
static void send_notif(const char *title, const char *msg) {
    if (!notif_enabled) return;
    g_mkdir_with_parents("/tmp/qynotif", 0755);
    gchar *content = g_strdup_printf("%s|%s\n", title, msg);
    g_file_set_contents("/tmp/qynotif/latest.msg", content, -1, NULL);
    g_free(content);
    /* 追加通知历史（保留最近约 8KB） */
    gchar *hist = NULL;
    g_file_get_contents("/tmp/qynotif/history.log", &hist, NULL, NULL);
    GString *h = g_string_new(hist ? hist : "");
    g_free(hist);
    g_string_append_printf(h, "%s|%s\n", title, msg);
    if (h->len > 8192) {
        const char *tail = h->str + (h->len - 8192);
        const char *nl = strchr(tail, '\n');
        if (nl) tail = nl + 1;
        g_string_erase(h, 0, tail - h->str);
    }
    g_file_set_contents("/tmp/qynotif/history.log", h->str, h->len, NULL);
    g_string_free(h, TRUE);

    /* 持久化历史 ~/.local/share/lanxiu/notifications.log（时间|应用|标题|正文，最多 200 行） */
    gchar *dir = g_build_filename(g_get_user_data_dir(), "lanxiu", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, "notifications.log", NULL);
    g_free(dir);
    GDateTime *now = g_date_time_new_now_local();
    gchar *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(now);
    GString *line = g_string_new(NULL);
    g_string_append_printf(line, "%s|%s|%s|%s\n", ts, "qynotifd",
                           title ? title : "", msg ? msg : "");
    g_free(ts);
    gchar *old = NULL;
    if (g_file_get_contents(path, &old, NULL, NULL) && old) {
        GString *out = g_string_new(NULL);
        gchar **lines = g_strsplit(old, "\n", 0);
        int n = 0;
        for (int i = 0; lines[i] && lines[i][0]; i++) n++;
        int start = n - 199;   /* 保留最近 199 行 + 新行 = 200 行 */
        if (start < 0) start = 0;
        for (int i = start; lines[i] && lines[i][0]; i++)
            g_string_append_printf(out, "%s\n", lines[i]);
        g_strfreev(lines);
        g_string_append(out, line->str);
        g_file_set_contents(path, out->str, out->len, NULL);
        g_string_free(out, TRUE);
    } else {
        g_file_set_contents(path, line->str, line->len, NULL);
    }
    g_string_free(line, TRUE);
    g_free(old);
    g_free(path);
}

/* ---------- 电源/电池监控 ---------- */
static int battery_low_notified = 0;

static gboolean battery_check_cb(gpointer p) {
    (void)p;
    static const char *bats[] = { "/sys/class/power_supply/BAT0",
                                  "/sys/class/power_supply/BAT1" };
    int found = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(bats); i++) {
        if (!g_file_test(bats[i], G_FILE_TEST_IS_DIR)) continue;
        found = 1;
        gchar *cap_path = g_strdup_printf("%s/capacity", bats[i]);
        gchar *status_path = g_strdup_printf("%s/status", bats[i]);
        gchar *cap = NULL, *status = NULL;
        g_file_get_contents(cap_path, &cap, NULL, NULL);
        g_file_get_contents(status_path, &status, NULL, NULL);
        int level = cap ? atoi(g_strstrip(cap)) : -1;
        const char *st = status ? g_strstrip(status) : "";
        int charging = (g_ascii_strcasecmp(st, "Charging") == 0) ||
                       (g_ascii_strcasecmp(st, "Full") == 0);
        g_printerr("QYNOTIFDBG: battery=%d status=%s\n",
                   level, st[0] ? st : "unknown");
        /* 低电量通知（放电且 <=20%；恢复 >20% 后重置） */
        if (level >= 0 && !charging) {
            if (level <= 20) {
                if (!battery_low_notified) {
                    gchar *m = g_strdup_printf("电量不足：%d%%", level);
                    send_notif("电源", m);
                    g_free(m);
                    battery_low_notified = 1;
                }
            } else {
                battery_low_notified = 0;
            }
        }
        g_free(cap); g_free(status);
        g_free(cap_path); g_free(status_path);
        break;
    }
    if (!found)
        g_printerr("QYNOTIFDBG: battery absent\n");
    return G_SOURCE_CONTINUE;
}

/* 读取简单键值 conf（如 qynotif.conf 的 qymon=off） */
static gboolean conf_flag(const char *path, const char *key) {
    gchar *c = NULL;
    if (!g_file_get_contents(path, &c, NULL, NULL) || !c) {
        g_free(c);
        return FALSE;
    }
    gboolean on = FALSE;
    gchar *line = c;
    while (line && *line) {
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == '=') {
            on = (line[strlen(key) + 1] == 'o' && line[strlen(key) + 2] == 'n');
            break;
        }
        gchar *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(c);
    return on;
}

/* 前向声明：进程存活检查（定义在文件后部，供 run_autostart 使用） */
static gboolean proc_alive(const char *name);

/* 自动启动白名单：仅允许启动已知 qy* 应用，防止 conf 被篡改后执行任意命令 */
static const char *autostart_whitelist[] = {
    "qydesktop", "qynotifd", "qynet", "qyfiles", "qyedit", "qymon",
    "qybrowser", "qyshot", "qyclip", "qysettings", "qystore", "qyarc",
    "qymedia", "qycalc", "qygit", "qyview", "qysearch", "qynotify",
    "qyswitcher", "qyappmenu", "qydriver", "qylock", NULL
};

static gboolean autostart_allowed(const char *name) {
    for (int i = 0; autostart_whitelist[i]; i++)
        if (strcmp(name, autostart_whitelist[i]) == 0) return TRUE;
    return FALSE;
}

/* 执行自动启动项（qyautostart.conf 格式：应用名=on/off） */
static void run_autostart(void) {
    gchar *c = NULL;
    if (!g_file_get_contents("/etc/qyautostart.conf", &c, NULL, NULL) || !c) {
        g_free(c);
        return;
    }
    gchar **lines = g_strsplit(c, "\n", 0);
    g_free(c);
    for (int i = 0; lines && lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!line[0] || line[0] == '#') continue;
        /* 自定义命令（release 303）: cmd:<完整命令行>，去掉前缀后整行作为命令启动 */
        if (g_str_has_prefix(line, "cmd:")) {
            const char *cmdline = line + 4;
            if (cmdline[0]) {
                gchar *cmd = g_strdup_printf("%s &", cmdline);
                g_spawn_command_line_async(cmd, NULL);
                g_free(cmd);
                g_printerr("qynotifd: autostart cmd: %s\n", cmdline);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *name = line;
        gboolean on = (eq[1] == 'o' && eq[2] == 'n');
        if (on && autostart_allowed(name)) {
            /* 防重复：应用已在运行则跳过（设置页重复保存不会重复启动） */
            if (proc_alive(name)) {
                g_printerr("qynotifd: autostart %s already running\n", name);
                continue;
            }
            gchar *cmd = g_strdup_printf("%s &", name);
            g_spawn_command_line_async(cmd, NULL);
            g_free(cmd);
            g_printerr("qynotifd: autostart %s\n", name);
        }
    }
    g_strfreev(lines);
}

/* 处理单个 conf 变化 */
static void handle_conf(const char *basename) {
    gchar path[512];
    g_snprintf(path, sizeof path, "/etc/%s", basename);
    const char *title = "设置";

    if (g_str_has_prefix(basename, "qyautostart")) {
        run_autostart();
        send_notif("自动启动", "启动项配置已更新");
        return;
    }
    if (g_str_has_prefix(basename, "qynotif")) {
        gchar *c = NULL;
        g_file_get_contents(path, &c, NULL, NULL);
        notif_enabled = !(c && strstr(c, "qynotif=off"));
        mon_enabled = conf_flag(path, "qymon");
        g_free(c);
        send_notif("通知", notif_enabled ? "通知服务已启用" : "通知服务已关闭");
        return;
    }
    if (g_str_has_prefix(basename, "qywifi")) {
        gboolean on = conf_flag(path, "wifi");
        gchar cmd[128];
        g_snprintf(cmd, sizeof cmd, "ip link set dev wlan0 %s 2>/dev/null", on ? "up" : "down");
        g_spawn_command_line_async(cmd, NULL);
        if (on) {
            if (access("/sys/class/net/wlan0", F_OK) == 0)
                send_notif("WiFi", "WiFi 已开启");
            else
                send_notif("WiFi", "未检测到无线网卡（wlan0）");
        } else {
            send_notif("WiFi", "WiFi 已关闭");
        }
        return;
    }
    if (g_str_has_prefix(basename, "qybluetooth")) {
        gboolean on = conf_flag(path, "bluetooth");
        gchar cmd[128];
        g_snprintf(cmd, sizeof cmd, "rfkill unblock bluetooth 2>/dev/null; %s", on ? "bluetoothctl power on 2>/dev/null" : "bluetoothctl power off 2>/dev/null");
        g_spawn_command_line_async(cmd, NULL);
        if (on) {
            if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
                send_notif("蓝牙", "蓝牙已开启");
            else
                send_notif("蓝牙", "未检测到蓝牙适配器");
        } else {
            send_notif("蓝牙", "蓝牙已关闭");
        }
        return;
    }
    if (g_str_has_prefix(basename, "qyfirewall")) {
        gboolean on = conf_flag(path, "firewall");
        if (on) {
            /* 启用：清空旧规则 → 默认拒绝入站 → 放行回环与已建立连接 */
            g_spawn_command_line_async("iptables -F", NULL);
            g_spawn_command_line_async("iptables -P INPUT DROP", NULL);
            g_spawn_command_line_async("iptables -A INPUT -i lo -j ACCEPT", NULL);
            g_spawn_command_line_async("iptables -A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT", NULL);
        } else {
            /* 关闭：清空规则并把默认策略改回 ACCEPT */
            g_spawn_command_line_async("iptables -F", NULL);
            g_spawn_command_line_async("iptables -P INPUT ACCEPT", NULL);
        }
        send_notif("防火墙", on ? "防火墙已启用" : "防火墙已关闭");
        return;
    }
    if (g_str_has_prefix(basename, "qyssh")) {
        gboolean on = conf_flag(path, "sshd");
        if (on) {
            g_mkdir_with_parents("/run/sshd", 0755);
            g_spawn_command_line_async("/usr/sbin/sshd", NULL);
            send_notif("SSH", "SSH 服务已开启");
            g_printerr("qynotifd: sshd on\n");
        } else {
            g_spawn_command_line_async("pkill -x sshd", NULL);
            send_notif("SSH", "SSH 服务已关闭");
            g_printerr("qynotifd: sshd off\n");
        }
        return;
    }
    if (g_str_has_prefix(basename, "qyprinter")) {
        send_notif("打印机", "打印机设置已保存");
        return;
    }
    if (g_str_has_prefix(basename, "qydisplay") || g_str_has_prefix(basename, "qyhdr")
        || g_str_has_prefix(basename, "qygraphics") || g_str_has_prefix(basename, "qypen")) {
        send_notif("显示", "显示设置已保存（重启桌面后生效）");
        return;
    }
    /* 通用 */
    gchar *n = g_strdup(basename + 2); /* 去掉 qy 前缀 */
    gchar *dot = strrchr(n, '.');
    if (dot) *dot = 0;
    gchar *msg = g_strdup_printf("%s 设置已更新", n);
    send_notif(title, msg);
    g_free(n);
    g_free(msg);
}

static void on_dir_changed(GFileMonitor *mon, GFile *file, GFile *other,
                           GFileMonitorEvent evt, gpointer ud) {
    (void)mon; (void)other; (void)ud;
    if (evt != G_FILE_MONITOR_EVENT_CHANGED &&
        evt != G_FILE_MONITOR_EVENT_CREATED)
        return;
    if (!file) return;
    char *basename = g_file_get_basename(file);
    if (basename && g_str_has_prefix(basename, "qy") && g_str_has_suffix(basename, ".conf")) {
        g_printerr("qynotifd: conf changed: %s\n", basename);
        handle_conf(basename);
    }
    g_free(basename);
}

static gboolean load_initial_conf(gpointer ud) {
    (void)ud;
    /* 启动时消费已有配置 */
    gchar *c = NULL;
    g_file_get_contents("/etc/qynotif.conf", &c, NULL, NULL);
    notif_enabled = !(c && strstr(c, "qynotif=off"));
    mon_enabled = conf_flag("/etc/qynotif.conf", "qymon");
    g_free(c);
    run_autostart();
    if (mon_enabled)
        send_notif("系统监视", "系统监视通知已开启");
    return G_SOURCE_REMOVE;
}

/* ---------- 自动锁屏（消费 /etc/xdg/weston/weston.ini 的 idle-time） ---------- */
static guint lock_timer = 0;
static guint unlock_poll = 0;
static int lock_idle_secs = 0;
static guint64 last_irq_count = 0;

static gboolean lock_cb(gpointer ud);
static gboolean unlock_poll_cb(gpointer ud);

/* 读取键盘/鼠标相关 IRQ 计数（i8042/atkbd/USB 控制器），用于空闲检测 */
static guint64 read_input_irqs(void) {
    gchar *c = NULL;
    guint64 total = 0;
    if (g_file_get_contents("/proc/interrupts", &c, NULL, NULL) && c) {
        gchar **lines = g_strsplit(c, "\n", 0);
        for (int i = 0; lines[i]; i++) {
            if (strstr(lines[i], "i8042") || strstr(lines[i], "atkbd") ||
                strstr(lines[i], "uhci") || strstr(lines[i], "ehci") ||
                strstr(lines[i], "xhci")) {
                const char *p = strchr(lines[i], ':');
                if (p) total += strtoull(p + 1, NULL, 10);
            }
        }
        g_strfreev(lines);
        g_free(c);
    }
    return total;
}

static int read_idle_secs(void) {
    gchar *c = NULL;
    if (!g_file_get_contents("/etc/xdg/weston/weston.ini", &c, NULL, NULL) || !c) {
        g_free(c);
        return 0;
    }
    int secs = 0;
    gchar *line = c;
    while (line && *line) {
        if (strncmp(line, "idle-time=", 10) == 0) {
            secs = atoi(line + 10);
            break;
        }
        gchar *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
    g_free(c);
    return secs;
}

/* 检查进程是否存活（遍历 /proc/comm，不依赖 pidof——busybox 可能未提供） */
static gboolean proc_alive(const char *name) {
    GDir *dir = g_dir_open("/proc", 0, NULL);
    if (!dir) return FALSE;
    const char *ent;
    gboolean found = FALSE;
    while ((ent = g_dir_read_name(dir)) != NULL) {
        if (!g_ascii_isdigit(ent[0])) continue;
        gchar *cp = g_strdup_printf("/proc/%s/comm", ent);
        gchar *comm = NULL;
        if (g_file_get_contents(cp, &comm, NULL, NULL) && comm) {
            g_strstrip(comm);
            if (strcmp(comm, name) == 0) { found = TRUE; g_free(comm); g_free(cp); break; }
        }
        g_free(comm);
        g_free(cp);
    }
    g_dir_close(dir);
    return found;
}

/* qylock 是否正在运行（遍历 /proc） */
static gboolean qylock_running(void) {
    return proc_alive("qylock");
}

static gboolean unlock_poll_cb(gpointer ud) {
    (void)ud;
    if (!qylock_running()) {
        g_printerr("qynotifd: unlocked, rearm autolock (%ds)\n", lock_idle_secs);
        unlock_poll = 0;
        if (lock_idle_secs > 0)
            lock_timer = g_timeout_add_seconds((guint)lock_idle_secs, lock_cb, NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean lock_cb(gpointer ud) {
    (void)ud;
    if (qylock_running()) {
        /* 已锁：等待解锁 */
        if (!unlock_poll)
            unlock_poll = g_timeout_add(2000, unlock_poll_cb, NULL);
        return G_SOURCE_REMOVE;
    }
    /* 空闲检测：有键盘/鼠标 IRQ 活动则重置定时器，不锁屏（打字/看视频不会误锁） */
    guint64 irq_now = read_input_irqs();
    if (last_irq_count != 0 && irq_now != last_irq_count) {
        last_irq_count = irq_now;
        g_printerr("qynotifd: input active, autolock postponed (%ds)\n", lock_idle_secs);
        if (lock_idle_secs > 0)
            lock_timer = g_timeout_add_seconds((guint)lock_idle_secs, lock_cb, NULL);
        return G_SOURCE_REMOVE;
    }
    last_irq_count = irq_now;
    g_printerr("qynotifd: autolock idle=%ds triggered\n", lock_idle_secs);
    /* qylock 是 Wayland 客户端，需带桌面会话环境启动（用 g_spawn_async 显式传 env） */
    const char *xrd = g_getenv("XDG_RUNTIME_DIR");
    const char *wld = g_getenv("WAYLAND_DISPLAY");
    if (!xrd) xrd = "/tmp/qyxdg";
    if (!wld) wld = "qy";
    gchar **envp = g_environ_setenv(g_get_environ(), "XDG_RUNTIME_DIR", xrd, TRUE);
    envp = g_environ_setenv(envp, "WAYLAND_DISPLAY", wld, TRUE);
    gchar *argv[] = { "/usr/bin/qylock", NULL };
    g_spawn_async(NULL, argv, envp,
                  G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_strfreev(envp);
    if (!unlock_poll)
        unlock_poll = g_timeout_add(2000, unlock_poll_cb, NULL);
    return G_SOURCE_REMOVE;
}

/* 轮询 weston.ini：idle-time 变化时重新武装锁屏定时器 */
static gboolean lock_poll_cb(gpointer ud) {
    (void)ud;
    int secs = read_idle_secs();
    if (secs != lock_idle_secs) {
        lock_idle_secs = secs;
        if (lock_timer) { g_source_remove(lock_timer); lock_timer = 0; }
        if (secs > 0 && !qylock_running()) {
            g_printerr("qynotifd: autolock idle=%ds armed\n", secs);
            lock_timer = g_timeout_add_seconds((guint)secs, lock_cb, NULL);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- 全局快捷键守护（evdev；weston 不提供自定义命令绑定时的可行方案） ----------
 * 配置: weston.ini 的 [bindings] 段 或 /etc/qyshortcuts.conf（同名 keyfile）
 *   单键:  KEY_SYSRQ=qyshot
 *   修饰键: KEY_LEFTMETA=qyappmenu（Super 松开触发，防组合误触）
 *   组合:  KEY_LEFTMETA+KEY_L=qylock
 * 机制: root 直接读 /dev/input/event*，组合键第二键按下时检查修饰键状态。 */
#define QYSHORTCUTS_MAX 32
typedef struct { int code1; int code2; char cmd[128]; } Shortcut;
static Shortcut shortcuts[QYSHORTCUTS_MAX];
static int n_shortcuts = 0;
static gboolean *key_state = NULL;
static gint64 meta_suppress_until = 0;
#define SHORTCUT_META_MS 250

static gboolean is_meta_key(int code) {
    return code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}

static int keycode_by_name(const char *name) {
    if (!name) return -1;
    struct { const char *name; int code; } map[] = {
        { "KEY_LEFTMETA", KEY_LEFTMETA }, { "Super", KEY_LEFTMETA },
        { "KEY_RIGHTMETA", KEY_RIGHTMETA },
        { "KEY_SYSRQ", KEY_SYSRQ }, { "PrtSc", KEY_SYSRQ },
        { "KEY_L", KEY_L }, { "L", KEY_L }, { "KEY_A", KEY_A }, { "A", KEY_A },
        { "KEY_S", KEY_S }, { "S", KEY_S }, { "KEY_D", KEY_D }, { "D", KEY_D },
        { "KEY_E", KEY_E }, { "E", KEY_E }, { "KEY_T", KEY_T }, { "T", KEY_T },
        { "KEY_I", KEY_I }, { "I", KEY_I }, { "KEY_R", KEY_R }, { "R", KEY_R },
        { "KEY_TAB", KEY_TAB }, { "KEY_F1", KEY_F1 }, { "KEY_F2", KEY_F2 },
        { "KEY_F4", KEY_F4 }, { "KEY_F5", KEY_F5 }, { "KEY_F11", KEY_F11 },
        { "KEY_VOLUMEUP", KEY_VOLUMEUP }, { "XF86AudioRaiseVolume", KEY_VOLUMEUP },
        { "KEY_VOLUMEDOWN", KEY_VOLUMEDOWN }, { "XF86AudioLowerVolume", KEY_VOLUMEDOWN },
        { "KEY_MUTE", KEY_MUTE }, { "XF86AudioMute", KEY_MUTE },
        { "KEY_PLAYPAUSE", KEY_PLAYPAUSE }, { "XF86AudioPlay", KEY_PLAYPAUSE },
        { "KEY_NEXTSONG", KEY_NEXTSONG }, { "XF86AudioNext", KEY_NEXTSONG },
        { "KEY_PREVIOUSSONG", KEY_PREVIOUSSONG }, { "XF86AudioPrev", KEY_PREVIOUSSONG },
        { "KEY_STOPCD", KEY_STOPCD }, { "XF86AudioStop", KEY_STOPCD },
        { "KEY_BRIGHTNESSUP", KEY_BRIGHTNESSUP }, { "XF86MonBrightnessUp", KEY_BRIGHTNESSUP },
        { "KEY_BRIGHTNESSDOWN", KEY_BRIGHTNESSDOWN }, { "XF86MonBrightnessDown", KEY_BRIGHTNESSDOWN },
        { "KEY_POWER", KEY_POWER }, { "XF86PowerOff", KEY_POWER },
        { "KEY_SLEEP", KEY_SLEEP }, { "XF86Sleep", KEY_SLEEP },
        { "KEY_ESC", KEY_ESC }, { "KEY_DELETE", KEY_DELETE },
        { "KEY_BACKSPACE", KEY_BACKSPACE }, { "KEY_LEFT", KEY_LEFT },
        { "KEY_RIGHT", KEY_RIGHT }, { "KEY_UP", KEY_UP }, { "KEY_DOWN", KEY_DOWN },
        { "KEY_ENTER", KEY_ENTER }, { "KEY_SPACE", KEY_SPACE },
        { "KEY_HOME", KEY_HOME }, { "KEY_END", KEY_END },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++)
        if (!g_ascii_strcasecmp(name, map[i].name)) return map[i].code;
    return -1;
}

static void load_bindings(void) {
    n_shortcuts = 0;
    const char *paths[] = {
        "/etc/xdg/weston/weston.ini",
        "/etc/weston.ini",
        "/etc/qyshortcuts.conf",
    };
    for (int p = 0; p < 3 && n_shortcuts < QYSHORTCUTS_MAX; p++) {
        GKeyFile *kf = g_key_file_new();
        if (!g_key_file_load_from_file(kf, paths[p], G_KEY_FILE_NONE, NULL)) {
            g_key_file_free(kf);
            continue;
        }
        gsize nk = 0;
        gchar **keys = g_key_file_get_keys(kf, "bindings", &nk, NULL);
        for (gsize i = 0; keys && i < nk && n_shortcuts < QYSHORTCUTS_MAX; i++) {
            char *cmd = g_key_file_get_string(kf, "bindings", keys[i], NULL);
            if (!cmd || !cmd[0]) { g_free(cmd); continue; }
            char keybuf[64];
            g_strlcpy(keybuf, keys[i], sizeof keybuf);
            char *plus = strchr(keybuf, '+');
            int c2 = 0;
            int c1 = keycode_by_name(keybuf);
            if (plus) {
                *plus = 0;
                c1 = keycode_by_name(keybuf);
                c2 = keycode_by_name(plus + 1);
            }
            if (c1 >= 0) {
                shortcuts[n_shortcuts].code1 = c1;
                shortcuts[n_shortcuts].code2 = c2;
                g_strlcpy(shortcuts[n_shortcuts].cmd, cmd, sizeof shortcuts[0].cmd);
                n_shortcuts++;
                g_printerr("QYSHORTCUTS: bind %s=%s code=%d+%d\n", keys[i], cmd, c1, c2);
            }
            g_free(cmd);
        }
        g_strfreev(keys);
        g_key_file_free(kf);
    }
    g_printerr("QYSHORTCUTS: loaded=%d\n", n_shortcuts);
}

static void run_cmd(const char *cmd) {
    g_printerr("QYSHORTCUT: run %s\n", cmd);
    gchar *full = g_strdup_printf("%s &", cmd);
    g_spawn_command_line_async(full, NULL);
    g_free(full);
}

/* Super 松开触发（若配置了 Super 单键） */
static gboolean meta_release_cb(gpointer p) {
    (void)p;
    if (g_get_monotonic_time() < meta_suppress_until) return G_SOURCE_REMOVE;
    for (int i = 0; i < n_shortcuts; i++) {
        if (shortcuts[i].code2 == 0 && is_meta_key(shortcuts[i].code1)) {
            if (!key_state[shortcuts[i].code1]) {
                run_cmd(shortcuts[i].cmd);
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }
    }
    return G_SOURCE_REMOVE;
}

static void handle_key(unsigned code, int value) {
    if (!key_state || code >= KEY_MAX) return;
    if (value == 1) key_state[code] = TRUE;
    else if (value == 0) key_state[code] = FALSE;
    else return;
    if (value != 1) return;
    /* 按下事件 */
    for (int i = 0; i < n_shortcuts; i++) {
        Shortcut *sc = &shortcuts[i];
        if (sc->code2 != 0) {
            if (sc->code2 == (int)code && key_state[sc->code1]) {
                meta_suppress_until = g_get_monotonic_time() + 300000; /* 300ms 抑制 Super 单键 */
                run_cmd(sc->cmd);
                return;
            }
        } else if (!is_meta_key(sc->code1) && sc->code1 == (int)code) {
            if (g_get_monotonic_time() < meta_suppress_until) return;
            run_cmd(sc->cmd);
            return;
        }
    }
    /* Super 单键：启动松开触发窗口 */
    if (is_meta_key(code)) {
        meta_suppress_until = 0;
        g_timeout_add(SHORTCUT_META_MS, meta_release_cb, NULL);
    }
}

static gboolean on_input_ready(GIOChannel *ch, GIOCondition cond, gpointer ud) {
    (void)ud;
    if (cond & (G_IO_ERR | G_IO_HUP)) return FALSE;
    struct input_event ev;
    ssize_t r = read(g_io_channel_unix_get_fd(ch), &ev, sizeof ev);
    if (r < (ssize_t)sizeof ev) return TRUE;
    if (ev.type == EV_KEY) handle_key(ev.code, ev.value);
    return TRUE;
}

static void watch_input_device(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_channel_set_encoding(ch, NULL, NULL);
    g_io_channel_set_buffered(ch, FALSE);
    g_io_add_watch(ch, G_IO_IN | G_IO_ERR | G_IO_HUP, on_input_ready, NULL);
    g_io_channel_unref(ch);
    g_printerr("QYSHORTCUTS: watching %s\n", path);
}

/* 已监听设备注册表：防止重扫时重复 open 同一 event 节点 */
#define QYSHORTCUT_MAX_DEV 32
static int watched_dev_idx[QYSHORTCUT_MAX_DEV];
static int n_watched_dev = 0;

/* 扫描 /dev/input/event*：boot 早期设备可能尚未出现，需定时重试；
 * 同时兼容真实系统上的热插拔（USB 键鼠后插）。 */
static void shortcut_watch_scan(void) {
    if (!key_state) {
        key_state = g_new0(gboolean, KEY_MAX);
        load_bindings();
    }
    for (int i = 0; i < QYSHORTCUT_MAX_DEV; i++) {
        gchar *path = g_strdup_printf("/dev/input/event%d", i);
        int exists = g_file_test(path, G_FILE_TEST_EXISTS);
        int known = -1;
        for (int j = 0; j < n_watched_dev; j++) {
            if (watched_dev_idx[j] == i) { known = j; break; }
        }
        if (exists) {
            if (known < 0) {
                watch_input_device(path);
                watched_dev_idx[n_watched_dev++] = i;
                g_printerr("QYSHORTCUTS: added event%d\n", i);
            }
        } else if (known >= 0) {
            /* 设备已移除：仅清注册表；旧 fd 由 G_IO_HUP 触发 watch 移除并关闭 */
            watched_dev_idx[known] = watched_dev_idx[--n_watched_dev];
            g_printerr("QYSHORTCUTS: removed event%d\n", i);
        }
        g_free(path);
    }
    g_printerr("QYSHORTCUTS: watched=%d\n", n_watched_dev);
}

static gboolean shortcut_rescan_cb(gpointer ud) {
    (void)ud;
    shortcut_watch_scan();
    return G_SOURCE_CONTINUE;
}

static void shortcut_watch_start(void) {
    shortcut_watch_scan();
    /* 关键修复（release 240）：boot 早期 /dev/input/event* 尚未出现时，
     * 旧实现只扫一次（找到 0 设备且不重试）导致全局快捷键永久失效。
     * 改为每 3 秒重扫，直到设备就绪；同时覆盖热插拔场景。 */
    g_timeout_add_seconds(3, shortcut_rescan_cb, NULL);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_mkdir_with_parents("/tmp/qynotif", 0755);

    GFile *dir = g_file_new_for_path("/etc");
    GError *err = NULL;
    GFileMonitor *mon = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, &err);
    if (!mon) {
        g_printerr("qynotifd: monitor /etc: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return 1;
    }
    g_signal_connect(mon, "changed", G_CALLBACK(on_dir_changed), NULL);
    g_object_unref(dir);

    g_timeout_add(500, load_initial_conf, NULL);
    g_timeout_add(5000, lock_poll_cb, NULL);
    /* 电池监控：QYNOTIF_BATTERY=1 时 1 秒后立即检查一次，否则每 60s */
    if (g_getenv("QYNOTIF_BATTERY"))
        g_timeout_add(1000, battery_check_cb, NULL);
    else
        g_timeout_add_seconds(60, battery_check_cb, NULL);
    g_printerr("qynotifd: started\n");
    shortcut_watch_start();
    g_main_loop_run(loop);
    g_object_unref(mon);
    return 0;
}