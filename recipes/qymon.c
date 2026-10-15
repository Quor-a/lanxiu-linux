/* qymon — 澜岫系统监视器 v1
 * 数据源: /proc/stat (CPU), /proc/meminfo (内存), /proc/loadavg, /proc/uptime
 * UI: GTK3 + GtkDrawingArea 实时曲线 (cairo), 1s 定时刷新
 * 架构: 单文件, 与 qyfiles 同款编译方式 (gcc + pkg-config gtk+-3.0)
 */
#include "qyl10n.h"
#include "qytheme.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/statvfs.h>

#define NHIST 120   /* 曲线历史点数 */
#define NTEMP 60    /* 温度曲线历史点数（每 2s 追加一点，约 2 分钟） */

static void add_class(GtkWidget *w, const char *cls) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

static double cpu_hist[NHIST];  /* 0..1 */
static double mem_hist[NHIST];  /* 0..1 */
static double disk_hist[NHIST]; /* 0..1 */
static double net_hist[NHIST];  /* 0..1 (KB/s / 1024 归一化) */
static int hist_n = 0;
static double temp_hist[NTEMP]; /* 温度历史（°C） */
static int temp_hist_n = 0;
static int temp_tick = 0;       /* 每 2 次 tick（2s）追加一个温度点 */
static GtkWidget *temp_da = NULL;  /* 温度曲线 #qymon-temp */

/* ---------- 磁盘 IO 吞吐曲线（release 288） ---------- */
#define NIO 60                      /* 磁盘 IO 历史点数（每 tick 一点） */
static double io_hist[NIO];
static int io_hist_n = 0;
static GtkWidget *io_da = NULL;    /* 磁盘 IO 曲线 #qymon-io */
static GtkWidget *io_label = NULL; /* 磁盘 IO 大数字 */
static gint64 io_prev_rd = -1, io_prev_wr = -1, io_prev_us = 0;
static char line_buf[256];

/* ---------- 曲线图例点击开关（release 300） ---------- */
#define NCURVES 6
static gboolean curve_show[NCURVES] = {TRUE, TRUE, TRUE, TRUE, TRUE, TRUE};
static GtkWidget *curve_toggle[NCURVES] = {NULL, NULL, NULL, NULL, NULL, NULL};
static GtkWidget *main_da = NULL;   /* 主曲线区（CPU/内存/磁盘/网络） */
static long prev_total = 0, prev_idle = 0;
static unsigned long mem_total_kb = 1;
static GtkWidget *cpu_label = NULL, *mem_label = NULL, *info_label = NULL;
static GtkWidget *gpu_label = NULL;   /* GPU 信息 #qymon-gpu */
static GtkWidget *title_label = NULL;
static GtkWidget *disk_label = NULL;
static GtkWidget *proc_label = NULL;   /* 内存占用 TOP 5 进程列表 */
static GtkWidget *proc_info_label = NULL; /* 进程列表信息（数量/操作提示） */
static GtkListStore *proc_store = NULL;   /* 全量进程列表 */
static GtkWidget *proc_tv = NULL;         /* 进程列表视图 */
static GtkWidget *proc_search = NULL;     /* GtkSearchEntry 进程过滤 */
static gchar *proc_filter = NULL;         /* 当前过滤文本（小写） */

/* 右键菜单选中的进程上下文 */
static gint  ctx_pid = 0;
static gchar *ctx_name = NULL;

/* CPU% 两次采样差分（release 271） */
typedef struct { gint64 ticks; gint64 mono_us; } ProcCpuSample;
static GHashTable *cpu_prev = NULL;   /* gint64* pid -> ProcCpuSample* */
static long clk_tck = 0;

enum { P_PID, P_NAME, P_MEM, P_CPU, N_PCOLS };

/* 进程行 CPU% 格式化（GtkCellRendererText data func） */
static void cpu_cell_data_func(GtkTreeViewColumn *col, GtkCellRenderer *r,
                               GtkTreeModel *m, GtkTreeIter *it, gpointer ud) {
    (void)col; (void)ud;
    double v = 0;
    gtk_tree_model_get(m, it, P_CPU, &v, -1);
    gchar *s = g_strdup_printf("%.1f%%", v);
    g_object_set(r, "text", s, NULL);
    g_free(s);
}

/* 刷新全量进程列表（/proc/PID/comm + status VmRSS），默认按内存降序。
 * 若设置了 proc_filter，仅保留进程名（忽略大小写）包含过滤词的条目。 */
static void refresh_proc_list(void) {
    if (!proc_store) return;
    /* CPU% 差分采样：初始化哈希表与 CLK_TCK（release 271） */
    if (!cpu_prev)
        cpu_prev = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    if (clk_tck <= 0) {
        clk_tck = sysconf(_SC_CLK_TCK);
        if (clk_tck <= 0) clk_tck = 100;
    }
    gtk_list_store_clear(proc_store);
    GDir *dir = g_dir_open("/proc", 0, NULL);
    if (!dir) return;
    const char *ent;
    int n = 0;
    GHashTable *seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL); /* 本次出现的 pid 集合 */
    while ((ent = g_dir_read_name(dir)) != NULL) {
        if (!g_ascii_isdigit(ent[0])) continue;
        gchar *cp = g_strdup_printf("/proc/%s/comm", ent);
        gchar *comm = NULL;
        if (g_file_get_contents(cp, &comm, NULL, NULL) && comm) {
            g_strstrip(comm);
            /* 搜索过滤：进程名包含过滤词（strstr 忽略大小写） */
            if (proc_filter && proc_filter[0]) {
                gchar *low = g_ascii_strdown(comm, -1);
                gboolean match = strstr(low, proc_filter) != NULL;
                g_free(low);
                if (!match) {
                    g_free(comm);
                    g_free(cp);
                    continue;
                }
            }
            gchar *sp = g_strdup_printf("/proc/%s/status", ent);
            gchar *status = NULL;
            long rss_kb = 0;
            if (g_file_get_contents(sp, &status, NULL, NULL) && status) {
                char *vm = strstr(status, "VmRSS:");
                if (vm) rss_kb = atol(vm + 7);
            }
            g_free(sp);
            g_free(status);
            /* CPU% 两次采样差分（release 271） */
            gint64 pid64 = g_ascii_strtoll(ent, NULL, 10);
            gint64 *pid_key = g_new(gint64, 1);
            *pid_key = pid64;
            g_hash_table_add(seen, pid_key);
            double cpu = 0.0;
            gchar *statp = g_strdup_printf("/proc/%s/stat", ent);
            gchar *stat = NULL;
            if (g_file_get_contents(statp, &stat, NULL, NULL) && stat) {
                char *rp = strchr(stat, ')');   /* comm 可能含空格/括号，从最后一个 ')' 后开始解析 */
                if (rp) {
                    unsigned long long utime = 0, stime = 0;
                    /* 字段：state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime */
                    char *p = rp + 1;
                    int field = 0;   /* 0=state, 1=ppid, ... 11=utime, 12=stime */
                    while (*p) {
                        while (*p == ' ' || *p == '\t') p++;
                        if (!*p) break;
                        if (field == 0) {
                            p++;   /* state 是单个字符（R/S/D/Z 等） */
                        } else {
                            char *end = NULL;
                            unsigned long long v = strtoull(p, &end, 10);
                            if (end == p) break;   /* 解析失败，防止死循环 */
                            p = end;
                            if (field == 11) utime = v;
                            else if (field == 12) stime = v;
                        }
                        field++;
                    }
                    if (field >= 13) {
                        gint64 now_us = g_get_monotonic_time();
                        gint64 ticks = (gint64)(utime + stime);
                        ProcCpuSample *prev = g_hash_table_lookup(cpu_prev, pid_key);
                        if (prev && prev->mono_us > 0 && now_us > prev->mono_us) {
                            double dt = (now_us - prev->mono_us) / 1000000.0;
                            gint64 dticks = ticks - prev->ticks;
                            if (dticks < 0) dticks = 0;
                            if (dt > 0)
                                cpu = (double)dticks / (dt * clk_tck) * 100.0;
                            if (cpu < 0) cpu = 0;
                            if (cpu > 1000) cpu = 1000;   /* 防御多核异常 */
                        }
                        if (prev) {
                            prev->ticks = ticks;
                            prev->mono_us = now_us;
                        } else {
                            ProcCpuSample *ns = g_new0(ProcCpuSample, 1);
                            ns->ticks = ticks;
                            ns->mono_us = now_us;
                            g_hash_table_insert(cpu_prev, g_new(gint64, 1), ns);
                        }
                    }
                }
            }
            g_free(statp);
            g_free(stat);
            GtkTreeIter it;
            gtk_list_store_append(proc_store, &it);
            gtk_list_store_set(proc_store, &it,
                               P_PID, (int)pid64,
                               P_NAME, comm,
                               P_MEM, rss_kb,
                               P_CPU, cpu, -1);
            n++;
            g_free(comm);
        }
        g_free(cp);
    }
    g_dir_close(dir);
    /* 清理已消失进程的 CPU 采样（长期运行内存有界） */
    GHashTableIter hit;
    gpointer hk, hv;
    g_hash_table_iter_init(&hit, cpu_prev);
    while (g_hash_table_iter_next(&hit, &hk, &hv)) {
        if (!g_hash_table_contains(seen, hk))
            g_hash_table_iter_remove(&hit);
    }
    g_hash_table_destroy(seen);
    g_printerr("QYMONDBG: procs=%d\n", n);
    if (proc_info_label) {
        if (proc_filter && proc_filter[0] && n == 0) {
            gtk_label_set_text(GTK_LABEL(proc_info_label), TR("无匹配进程"));
        } else {
            gchar *s = g_strdup_printf(TR("进程 %d 个 · 点击选择后结束"), n);
            gtk_label_set_text(GTK_LABEL(proc_info_label), s);
            g_free(s);
        }
    }
}
static GtkWidget *temp_label = NULL;   /* CPU 温度大数字 */
static unsigned long net_rx_prev = 0, net_tx_prev = 0;   /* 网络速率 */
static gint64 net_time_prev = 0;

/* 读取当前 CPU 频率 (MHz) */
static double read_cpu_mhz(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char buf[256];
    double mhz = 0;
    while (fgets(buf, sizeof buf, f)) {
        if (strncmp(buf, "cpu MHz", 7) == 0) {
            char *c = strchr(buf, ':');
            if (c) { mhz = atof(c + 1); break; }
        }
    }
    fclose(f);
    return mhz;
}

/* 读取 CPU 温度 (℃), 失败返回 -1 */
static double read_cpu_temp(void) {
    for (int i = 0; i < 8; i++) {
        char path[96];
        g_snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE *f = fopen(path, "r");
        if (f) {
            long t = 0;
            fscanf(f, "%ld", &t);
            fclose(f);
            if (t > 0) return t / 1000.0;
        }
    }
    return -1;
}

/* 检查 PATH 中是否存在程序 */
static gboolean has_program(const char *name) {
    gchar *p = g_find_program_in_path(name);
    gboolean ok = (p != NULL);
    g_free(p);
    return ok;
}

/* 读取第一个显卡名称并显示在 #qymon-gpu。
 * 数据源: lspci | grep -i -E "vga|3d|display"（无 lspci 或未找到则显示未检测到）。 */
static void read_gpu_info(void) {
    if (!gpu_label) return;
    const char *fallback = TR("GPU: 未检测到 GPU");
    gchar *out = NULL, *err = NULL;
    gint status = 0;
    if (!has_program("lspci")) {
        gtk_label_set_text(GTK_LABEL(gpu_label), fallback);
        return;
    }
    if (!g_spawn_command_line_sync("lspci | grep -i -E 'vga|3d|display'",
                                   &out, &err, &status, NULL) || !out) {
        gtk_label_set_text(GTK_LABEL(gpu_label), fallback);
        g_free(out);
        g_free(err);
        return;
    }
    char *line = out;
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    gchar *name = NULL;
    char *colon = strrchr(line, ':');
    if (colon && colon[1]) {
        name = g_strdup(colon + 1);
        g_strstrip(name);
    }
    if (name && name[0]) {
        gchar *txt = g_strdup_printf("GPU: %s", name);
        gtk_label_set_text(GTK_LABEL(gpu_label), txt);
        g_free(txt);
        g_free(name);
    } else {
        gtk_label_set_text(GTK_LABEL(gpu_label), fallback);
    }
    g_free(out);
    g_free(err);
}

/* ---------- hwmon 硬件传感器（风扇 / 附加温度） ---------- */
static GtkWidget *fan_label = NULL;    /* 风扇转速 #qymon-fan */
static GtkWidget *hwmon_label = NULL;  /* 附加温度 #qymon-hwmon */

/* 将 hwmon 名称映射为友好显示名（尽力而为） */
static char *hwmon_display_name(const char *name) {
    if (!name || !name[0]) return g_strdup(TR("传感器"));
    if (!g_ascii_strcasecmp(name, "nvme")) return g_strdup("NVMe");
    if (!g_ascii_strcasecmp(name, "coretemp")) return g_strdup("CPU");
    if (!g_ascii_strcasecmp(name, "k10temp")) return g_strdup("CPU");
    if (!g_ascii_strcasecmp(name, "zenpower")) return g_strdup("CPU");
    if (!g_ascii_strcasecmp(name, "cpu_thermal")) return g_strdup("CPU");
    if (!g_ascii_strcasecmp(name, "acpitz")) return g_strdup("ACPI");
    if (!g_ascii_strcasecmp(name, "thinkpad")) return g_strdup("ThinkPad");
    if (!g_ascii_strcasecmp(name, "radeon")) return g_strdup("GPU");
    if (!g_ascii_strcasecmp(name, "amdgpu")) return g_strdup("GPU");
    char *s = g_ascii_strdown(name, -1);
    if (s[0]) s[0] = g_ascii_toupper((guchar)s[0]);
    return s;
}

/* 扫描 /sys/class/hwmon 下各 hwmon 子目录的 fan*_input（RPM）与 temp*_input（°C）。
 * 尽力而为：只显示存在的传感器；无传感器时隐藏标签。 */
static void read_hwmon(void) {
    if (!fan_label && !hwmon_label) return;
    GDir *dir = g_dir_open("/sys/class/hwmon", 0, NULL);
    GString *fans = g_string_new(NULL);
    GString *temps = g_string_new(NULL);
    if (dir) {
        const char *hw;
        while ((hw = g_dir_read_name(dir)) != NULL) {
            gchar *hwpath = g_strdup_printf("/sys/class/hwmon/%s", hw);
            gchar *name = NULL;
            gchar *namef = g_strdup_printf("%s/name", hwpath);
            if (g_file_get_contents(namef, &name, NULL, NULL) && name)
                g_strstrip(name);
            if (!name || !name[0]) { g_free(name); name = g_strdup(hw); }
            g_free(namef);

            GDir *hdir = g_dir_open(hwpath, 0, NULL);
            if (hdir) {
                const char *f;
                int fan_idx = 0;
                while ((f = g_dir_read_name(hdir)) != NULL) {
                    /* 风扇: fan*_input → RPM */
                    if (g_str_has_prefix(f, "fan") && g_str_has_suffix(f, "_input")) {
                        gchar *fp = g_strdup_printf("%s/%s", hwpath, f);
                        gchar *val = NULL;
                        if (g_file_get_contents(fp, &val, NULL, NULL) && val) {
                            long rpm = atol(val);
                            if (rpm > 0) {
                                fan_idx++;
                                if (fans->len) g_string_append(fans, " · ");
                                g_string_append_printf(fans, "%s%d: %ld RPM",
                                                       TR("风扇"), fan_idx, rpm);
                            }
                        }
                        g_free(fp); g_free(val);
                    }
                    /* 附加温度: temp*_input → 摄氏度 */
                    if (g_str_has_prefix(f, "temp") && g_str_has_suffix(f, "_input")) {
                        gchar *tp = g_strdup_printf("%s/%s", hwpath, f);
                        gchar *val = NULL;
                        if (g_file_get_contents(tp, &val, NULL, NULL) && val) {
                            long milli = atol(val);
                            if (milli > 0) {
                                double c = milli / 1000.0;
                                /* 优先 temp*_label，其次 hwmon 名称 */
                                gchar *label = NULL;
                                char base[64];
                                g_strlcpy(base, f, sizeof base);
                                char *in = strstr(base, "_input");
                                if (in) strcpy(in, "_label");
                                gchar *lp = g_strdup_printf("%s/%s", hwpath, base);
                                g_file_get_contents(lp, &label, NULL, NULL);
                                if (label) g_strstrip(label);
                                if (!label || !label[0]) {
                                    g_free(label);
                                    label = hwmon_display_name(name);
                                }
                                if (temps->len) g_string_append(temps, " · ");
                                g_string_append_printf(temps, "%s: %d°C",
                                                       label, (int)(c + 0.5));
                                g_free(label);
                                g_free(lp);
                            }
                        }
                        g_free(tp); g_free(val);
                    }
                }
                g_dir_close(hdir);
            }
            g_free(name);
            g_free(hwpath);
        }
        g_dir_close(dir);
    }

    gboolean has_fan = fans->len > 0;
    gboolean has_temp = temps->len > 0;
    if (has_fan && fan_label) {
        gtk_label_set_text(GTK_LABEL(fan_label), fans->str);
        gtk_widget_set_visible(fan_label, TRUE);
    } else if (fan_label) {
        gtk_widget_set_visible(fan_label, FALSE);
    }
    if (has_temp && hwmon_label) {
        gtk_label_set_text(GTK_LABEL(hwmon_label), temps->str);
        gtk_widget_set_visible(hwmon_label, TRUE);
    } else if (hwmon_label) {
        gtk_widget_set_visible(hwmon_label, FALSE);
    }
    g_printerr("QYMONDBG: hwmon fans=[%s] temps=[%s]\n",
                 has_fan ? fans->str : "-", has_temp ? temps->str : "-");
    g_string_free(fans, TRUE);
    g_string_free(temps, TRUE);
}

/* 每 2s 刷新 hwmon 传感器 */
static gboolean hwmon_tick(gpointer ud) {
    (void)ud;
    read_hwmon();
    return G_SOURCE_CONTINUE;
}

/* 读取首个非 lo 接口的收发速率 (KB/s), 与上次调用差分 */
static gboolean read_net_speed(double *rx_kbs, double *tx_kbs) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return FALSE;
    char line[512];
    fgets(line, sizeof line, f);   /* 表头1 */
    fgets(line, sizeof line, f);   /* 表头2 */
    unsigned long rx = 0, tx = 0;
    gboolean found = FALSE;
    while (fgets(line, sizeof line, f)) {
        char ifname[64];
        if (sscanf(line, " %63[^:]: %lu %*lu %*lu %*lu %*lu %*lu %*lu %*lu %lu",
                   ifname, &rx, &tx) >= 3) {
            if (strcmp(ifname, "lo") != 0) { found = TRUE; break; }
        }
    }
    fclose(f);
    if (!found) return FALSE;
    gint64 now = g_get_monotonic_time();
    if (net_time_prev == 0) {
        net_rx_prev = rx; net_tx_prev = tx; net_time_prev = now;
        *rx_kbs = 0; *tx_kbs = 0;
        return TRUE;
    }
    double dt = (double)(now - net_time_prev) / 1000000.0;
    if (dt <= 0) dt = 1.0;
    *rx_kbs = (rx - net_rx_prev) / dt / 1024.0;
    *tx_kbs = (tx - net_tx_prev) / dt / 1024.0;
    if (*rx_kbs < 0) *rx_kbs = 0;
    if (*tx_kbs < 0) *tx_kbs = 0;
    net_rx_prev = rx; net_tx_prev = tx; net_time_prev = now;
    return TRUE;
}

/* 磁盘 IO 吞吐（KB/s）：/proc/diskstats 所有磁盘 read+write 扇区差分（release 288） */
static void read_disk_io_speed(double *kb_s) {
    *kb_s = 0;
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;
    gint64 rd = 0, wr = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned major = 0, minor = 0;
        char name[64] = "";
        unsigned long long rd_io = 0, rm = 0, rd_sec = 0, rt = 0;
        unsigned long long wr_io = 0, wm = 0, wr_sec = 0;
        if (sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu",
                   &major, &minor, name, &rd_io, &rm, &rd_sec, &rt,
                   &wr_io, &wm, &wr_sec) >= 10) {
            rd += (gint64)rd_sec;
            wr += (gint64)wr_sec;
        }
    }
    fclose(f);
    gint64 now = g_get_monotonic_time();
    if (io_prev_rd >= 0 && now > io_prev_us) {
        double dt = (double)(now - io_prev_us) / 1000000.0;
        if (dt > 0) {
            double sectors = (double)((rd - io_prev_rd) + (wr - io_prev_wr));
            if (sectors < 0) sectors = 0;
            *kb_s = sectors * 512.0 / 1024.0 / dt;   /* 每扇区 512B */
        }
    }
    io_prev_rd = rd;
    io_prev_wr = wr;
    io_prev_us = now;
}

static gboolean tick(gpointer ud) {
    /* --- CPU: /proc/stat 首行 cpu  user nice system idle iowait irq softirq steal --- */
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        long u, n, s, idle, iow, irq, sirq, steal, total;
        if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
                   &u, &n, &s, &idle, &iow, &irq, &sirq, &steal) == 8) {
            total = u + n + s + idle + iow + irq + sirq + steal;
            long dtotal = total - prev_total;
            long didle = (idle + iow) - prev_idle;
            double usage = dtotal > 0 ? 1.0 - (double)didle / (double)dtotal : 0.0;
            if (usage < 0) usage = 0; if (usage > 1) usage = 1;
            prev_total = total; prev_idle = idle + iow;
            if (hist_n < NHIST) {
                cpu_hist[hist_n] = usage; mem_hist[hist_n] = -1;
                disk_hist[hist_n] = -1; net_hist[hist_n] = -1; hist_n++;
            } else {
                memmove(cpu_hist, cpu_hist + 1, sizeof(double) * (NHIST - 1));
                memmove(mem_hist, mem_hist + 1, sizeof(double) * (NHIST - 1));
                memmove(disk_hist, disk_hist + 1, sizeof(double) * (NHIST - 1));
                memmove(net_hist, net_hist + 1, sizeof(double) * (NHIST - 1));
                cpu_hist[NHIST - 1] = usage;
            }
        }
        fclose(f);
    }
    /* --- 内存: /proc/meminfo MemTotal / MemAvailable --- */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        unsigned long avail = 0; char key[64]; unsigned long val; char unit[16];
        int got_total = 0, got_avail = 0;
        while (fgets(line_buf, sizeof line_buf, f)) {
            if (sscanf(line_buf, "%63s %lu %15s", key, &val, unit) >= 2) {
                if (!strcmp(key, "MemTotal:")) { mem_total_kb = val; got_total = 1; }
                else if (!strcmp(key, "MemAvailable:")) { avail = val; got_avail = 1; }
                if (got_total && got_avail) break;
            }
        }
        fclose(f);
        double used = 1.0 - (double)avail / (double)(mem_total_kb ? mem_total_kb : 1);
        if (used < 0) used = 0; if (used > 1) used = 1;
        if (hist_n > 0) mem_hist[hist_n - 1] = used;
    }
    /* --- uptime & loadavg: 信息栏 --- */
    FILE *f2 = fopen("/proc/uptime", "r");
    if (f2) {
        double up = 0;
        fscanf(f2, "%lf", &up);
        fclose(f2);
        double l1 = 0, l5 = 0, l15 = 0;
        long run = 0, total = 0;
        f2 = fopen("/proc/loadavg", "r");
        if (f2) {
            fscanf(f2, "%lf %lf %lf %ld/%ld", &l1, &l5, &l15, &run, &total);
            fclose(f2);
        }
        int d = (int)up;
        char info[256];
        double mhz = read_cpu_mhz();
        char freq[32] = "";
        if (mhz >= 1000) g_snprintf(freq, sizeof freq, "%.2f GHz", mhz / 1000.0);
        else if (mhz > 0) g_snprintf(freq, sizeof freq, "%d MHz", (int)mhz);
        g_snprintf(info, sizeof info,
                    TR("运行 %d天 %02d:%02d · 负载 %.2f %.2f %.2f · 进程 %ld/%ld"),
                    d / 86400, (d % 86400) / 3600, (d % 3600) / 60, l1, l5, l15, run, total);
        if (freq[0]) {
            size_t L = strlen(info);
            g_snprintf(info + L, sizeof info - L, " · CPU %s", freq);
        }
        double rx = 0, tx = 0;
        if (read_net_speed(&rx, &tx)) {
            double net_kbs = rx + tx;
            if (net_kbs > 1024) net_kbs = 1024;
            if (hist_n > 0) net_hist[hist_n - 1] = net_kbs / 1024.0;
            size_t L = strlen(info);
            g_snprintf(info + L, sizeof info - L, " · 下行 %.0fKB/s 上行 %.0fKB/s", rx, tx);
        }
        if (info_label) gtk_label_set_text(GTK_LABEL(info_label), info);
    }
    /* --- 内存占用 TOP 5 进程列表 --- */
    {
        typedef struct { char name[64]; long rss; } ProcEnt;
        ProcEnt procs[256];
        int np = 0;
        GDir *dir = g_dir_open("/proc", 0, NULL);
        if (dir) {
            const char *pn;
            while ((pn = g_dir_read_name(dir)) && np < 256) {
                if (!g_ascii_isdigit(pn[0])) continue;
                char path[64], line[256];
                g_snprintf(path, sizeof path, "/proc/%s/comm", pn);
                FILE *f = fopen(path, "r");
                if (f) {
                    if (fgets(line, sizeof line, f)) {
                        char *nl = strchr(line, '\n');
                        if (nl) *nl = 0;
                        g_strlcpy(procs[np].name, line, sizeof procs[np].name);
                    }
                    fclose(f);
                } else {
                    g_strlcpy(procs[np].name, "?", sizeof procs[np].name);
                }
                long rss = 0;
                g_snprintf(path, sizeof path, "/proc/%s/status", pn);
                f = fopen(path, "r");
                if (f) {
                    while (fgets(line, sizeof line, f)) {
                        if (!strncmp(line, "VmRSS:", 6)) { rss = atol(line + 6); break; }
                    }
                    fclose(f);
                }
                procs[np].rss = rss;
                np++;
            }
            g_dir_close(dir);
        }
        for (int i = 0; i < np && i < 5; i++) {
            int best = i;
            for (int j = i + 1; j < np; j++)
                if (procs[j].rss > procs[best].rss) best = j;
            ProcEnt t = procs[i]; procs[i] = procs[best]; procs[best] = t;
        }
        GString *s = g_string_new(NULL);
        g_string_append_printf(s, "<b>%s</b>\n", TR("内存占用 TOP 5"));
        for (int i = 0; i < np && i < 5; i++) {
            g_string_append_printf(s, "%s   %ld MB\n",
                                   procs[i].name, (procs[i].rss + 512) / 1024);
        }
        if (proc_label) gtk_label_set_markup(GTK_LABEL(proc_label), s->str);
        g_string_free(s, TRUE);
    }
    /* --- 大数字百分比标签 --- */
    double last_cpu = hist_n ? cpu_hist[hist_n - 1] : 0;
    double last_mem = hist_n ? mem_hist[hist_n - 1] : 0;
    char big[96];
    g_snprintf(big, sizeof big, "CPU %d%%", (int)(last_cpu * 100 + 0.5));
    if (cpu_label) gtk_label_set_text(GTK_LABEL(cpu_label), big);
    g_snprintf(big, sizeof big, TR("内存 %d%%"), (int)(last_mem * 100 + 0.5));
    if (mem_label) gtk_label_set_text(GTK_LABEL(mem_label), big);
    /* 磁盘使用率: statvfs("/") */
    struct statvfs sv;
    double disk = 0.0;
    if (statvfs("/", &sv) == 0 && sv.f_blocks > 0) {
        disk = 1.0 - (double)sv.f_bavail / (double)sv.f_blocks;
        if (disk < 0) disk = 0; if (disk > 1) disk = 1;
        if (hist_n > 0) disk_hist[hist_n - 1] = disk;
        /* 内存+磁盘 合并大数字标签（release 267） */
        char diskbig[96];
        g_snprintf(diskbig, sizeof diskbig, TR("内存 %d%% · 磁盘 %d%%"),
                   (int)(last_mem * 100 + 0.5), (int)(disk * 100 + 0.5));
        if (mem_label) gtk_label_set_text(GTK_LABEL(mem_label), diskbig);
    }
    g_snprintf(big, sizeof big, TR("磁盘 %d%%"), (int)(disk * 100 + 0.5));
    if (disk_label) gtk_label_set_text(GTK_LABEL(disk_label), big);
    /* CPU 温度: 标签显示「CPU 温度: NN°C」，每 2s 追加一个历史点 */
    double temp = read_cpu_temp();
    if (temp > 0) {
        g_snprintf(big, sizeof big, TR("CPU 温度: %d°C"), (int)(temp + 0.5));
        temp_tick++;
        if ((temp_tick % 2) == 0) {
            if (temp_hist_n < NTEMP) {
                temp_hist[temp_hist_n++] = temp;
            } else {
                memmove(temp_hist, temp_hist + 1, sizeof(double) * (NTEMP - 1));
                temp_hist[NTEMP - 1] = temp;
            }
        }
    } else {
        g_snprintf(big, sizeof big, TR("CPU 温度: --"));
    }
    if (temp_label) gtk_label_set_text(GTK_LABEL(temp_label), big);
    if (temp_da) gtk_widget_queue_draw(temp_da);

    /* --- 磁盘 IO 吞吐曲线（release 288） --- */
    {
        double io_kbs = 0;
        read_disk_io_speed(&io_kbs);
        if (io_hist_n < NIO) {
            io_hist[io_hist_n++] = io_kbs;
        } else {
            memmove(io_hist, io_hist + 1, sizeof(double) * (NIO - 1));
            io_hist[NIO - 1] = io_kbs;
        }
        if (io_label) {
            gchar *s = g_strdup_printf("%s %.0f KB/s", TR("磁盘IO"), io_kbs);
            gtk_label_set_text(GTK_LABEL(io_label), s);
            g_free(s);
        }
        if (io_da) gtk_widget_queue_draw(io_da);
    }
    /* 窗口标题实时显示 CPU 使用率 */
    GtkWidget *win = gtk_widget_get_toplevel(GTK_WIDGET(ud));
    if (win && GTK_IS_WINDOW(win)) {
        gchar *ttl = g_strdup_printf("%s — CPU %d%%", TR("澜岫系统监视器"),
                                     (int)(last_cpu * 100 + 0.5));
        gtk_window_set_title(GTK_WINDOW(win), ttl);
        if (title_label) gtk_label_set_text(GTK_LABEL(title_label), ttl);
        g_free(ttl);
    }
    /* 全量进程列表：每 tick 刷新，搜索过滤在 refresh_proc_list 内生效 */
    refresh_proc_list();
    gtk_widget_queue_draw(GTK_WIDGET(ud));
    return G_SOURCE_CONTINUE;
}

static void draw_series(cairo_t *cr, double *hist, int n, double r, double g, double b,
                        int w, int h) {
    cairo_set_source_rgb(cr, r, g, b);
    cairo_set_line_width(cr, 1.5);
    int started = 0;
    for (int i = 0; i < n; i++) {
        if (hist[i] < 0) continue;
        double x = (double)i / (double)(NHIST - 1) * w;
        double y = h - hist[i] * (h - 8) - 2;
        if (!started) { cairo_move_to(cr, x, y); started = 1; }
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
}

static gboolean on_draw(GtkWidget *da, cairo_t *cr, gpointer ud) {
    GtkAllocation a;
    gtk_widget_get_allocation(da, &a);
    int w = a.width, h = a.height;
    /* 背景 */
    cairo_set_source_rgb(cr, 0.13, 0.14, 0.16);
    cairo_paint(cr);
    /* 25%/50%/75% 参考线 */
    cairo_set_source_rgb(cr, 0.25, 0.26, 0.28);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i <= 3; i++) {
        double y = h - (double)i / 4.0 * (h - 8) - 2;
        cairo_move_to(cr, 0, y); cairo_line_to(cr, w, y);
    }
    cairo_stroke(cr);
    if (hist_n > 1) {
        if (curve_show[0]) draw_series(cr, cpu_hist, hist_n, 0.95, 0.55, 0.15, w, h / 2 - 4);   /* CPU 橙 */
        if (curve_show[1]) draw_series(cr, mem_hist, hist_n, 0.45, 0.65, 0.95, w, h / 2 - 4);   /* MEM 蓝 */
        if (curve_show[2]) draw_series(cr, disk_hist, hist_n, 0.20, 0.83, 0.60, w, h / 2 - 4); /* DISK 绿 */
        if (curve_show[3]) draw_series(cr, net_hist, hist_n, 0.18, 0.76, 0.76, w, h / 2 - 4); /* NET 青（禁紫） */
    }
    /* 图例文字（带彩色方块） */
    cairo_set_font_size(cr, 12);
    double last_cpu = hist_n ? cpu_hist[hist_n - 1] : 0;
    double last_mem = hist_n ? mem_hist[hist_n - 1] : 0;
    double last_disk = hist_n ? disk_hist[hist_n - 1] : 0;
    /* CPU 橙方块 + 文字 */
    cairo_set_source_rgb(cr, 0.95, 0.55, 0.15);
    cairo_rectangle(cr, 8, 8, 10, 10);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 24, 18);
    cairo_show_text(cr, g_strdup_printf("CPU %3.0f%%", last_cpu * 100));
    /* NET 青方块 + 文字 (KB/s)（禁紫） */
    double last_net = hist_n ? net_hist[hist_n - 1] : 0;
    cairo_set_source_rgb(cr, 0.18, 0.76, 0.76);
    cairo_rectangle(cr, w - 190, 8, 10, 10);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, w - 176, 18);
    cairo_show_text(cr, g_strdup_printf("NET %3.0f KB/s", last_net * 1024));
    /* MEM 蓝方块 + 文字 */
    cairo_set_source_rgb(cr, 0.45, 0.65, 0.95);
    cairo_rectangle(cr, 8, h / 2.0 + 4, 10, 10);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 24, h / 2.0 + 14);
    cairo_show_text(cr, g_strdup_printf("MEM %3.0f%% of %lu MB", last_mem * 100, mem_total_kb / 1024));
    /* DISK 绿方块 + 文字 */
    cairo_set_source_rgb(cr, 0.20, 0.83, 0.60);
    cairo_rectangle(cr, w - 140, h / 2.0 + 4, 10, 10);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, w - 126, h / 2.0 + 14);
    cairo_show_text(cr, g_strdup_printf("DISK %3.0f%%", last_disk * 100));
    return FALSE;
}

/* 温度曲线: 深蓝底 + 橙色线 #FF5C1A，显示 60 点历史 */
static gboolean on_temp_draw(GtkWidget *da, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(da, &a);
    int w = a.width, h = a.height;
    /* 深蓝底 */
    cairo_set_source_rgb(cr, 0.06, 0.09, 0.16);
    cairo_paint(cr);
    /* 1/3、2/3 参考线 */
    cairo_set_source_rgb(cr, 0.20, 0.23, 0.30);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i <= 2; i++) {
        double y = h - (double)i / 3.0 * (h - 10) - 3;
        cairo_move_to(cr, 0, y); cairo_line_to(cr, w, y);
    }
    cairo_stroke(cr);
    /* 曲线隐藏时只保留背景/网格（release 300） */
    if (!curve_show[4]) return FALSE;
    if (temp_hist_n < 2) {
        /* 数据不足时仅显示当前温度文字 */
        if (temp_hist_n == 1) {
            char txt[64];
            g_snprintf(txt, sizeof txt, "%.0f°C", temp_hist[temp_hist_n - 1]);
            cairo_set_font_size(cr, 11);
            cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
            cairo_move_to(cr, 8, 16);
            cairo_show_text(cr, txt);
        }
        return FALSE;
    }
    /* 动态范围: 最小 2°C 跨度，避免曲线太平 */
    double tmin = 1000, tmax = -1000;
    for (int i = 0; i < temp_hist_n; i++) {
        if (temp_hist[i] < tmin) tmin = temp_hist[i];
        if (temp_hist[i] > tmax) tmax = temp_hist[i];
    }
    if (tmax - tmin < 2.0) { tmin -= 1; tmax += 1; }
    /* 橙色线 #FF5C1A */
    cairo_set_source_rgb(cr, 1.0, 0.36, 0.10);
    cairo_set_line_width(cr, 1.8);
    int started = 0;
    for (int i = 0; i < temp_hist_n; i++) {
        double x = (double)i / (double)(NTEMP - 1) * w;
        double y = h - (temp_hist[i] - tmin) / (tmax - tmin) * (h - 12) - 4;
        if (!started) { cairo_move_to(cr, x, y); started = 1; }
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    /* 右上角当前温度 */
    char txt[64];
    g_snprintf(txt, sizeof txt, "%.0f°C", temp_hist[temp_hist_n - 1]);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr, 0.95, 0.55, 0.15);
    cairo_move_to(cr, w - 70, 16);
    cairo_show_text(cr, txt);
    return FALSE;
}

/* 磁盘 IO 吞吐曲线（release 288）：浅蓝线，动态范围，右上角当前 KB/s */
static gboolean on_io_draw(GtkWidget *da, cairo_t *cr, gpointer ud) {
    (void)ud;
    GtkAllocation a;
    gtk_widget_get_allocation(da, &a);
    int w = a.width, h = a.height;
    cairo_set_source_rgb(cr, 0.06, 0.09, 0.16);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.20, 0.23, 0.30);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i <= 2; i++) {
        double y = h - (double)i / 3.0 * (h - 10) - 3;
        cairo_move_to(cr, 0, y); cairo_line_to(cr, w, y);
    }
    cairo_stroke(cr);
    /* 曲线隐藏时只保留背景/网格（release 300） */
    if (!curve_show[5]) return FALSE;
    if (io_hist_n < 2) return FALSE;
    double imin = 1000000, imax = -1000000;
    for (int i = 0; i < io_hist_n; i++) {
        if (io_hist[i] < imin) imin = io_hist[i];
        if (io_hist[i] > imax) imax = io_hist[i];
    }
    if (imax - imin < 8.0) { imin = imin > 4 ? imin - 4 : 0; imax += 4; }
    cairo_set_source_rgb(cr, 0.35, 0.80, 0.95);   /* 浅蓝 */
    cairo_set_line_width(cr, 1.8);
    int started = 0;
    for (int i = 0; i < io_hist_n; i++) {
        double x = (double)i / (double)(NIO - 1) * w;
        double y = h - (io_hist[i] - imin) / (imax - imin) * (h - 12) - 4;
        if (!started) { cairo_move_to(cr, x, y); started = 1; }
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    char txt[64];
    g_snprintf(txt, sizeof txt, "%.0f KB/s", io_hist[io_hist_n - 1]);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr, 0.35, 0.80, 0.95);
    cairo_move_to(cr, w - 96, 16);
    cairo_show_text(cr, txt);
    return FALSE;
}

/* 结束选中的进程（走 qysudo 密码验证，避免普通用户误杀系统进程） */
static void on_kill_clicked(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (!proc_tv || !proc_store) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(proc_tv));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        if (proc_info_label)
            gtk_label_set_text(GTK_LABEL(proc_info_label), TR("请先选择要结束的进程"));
        return;
    }
    int pid = 0; gchar *name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(proc_store), &it, P_PID, &pid, P_NAME, &name, -1);
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s %d (%s)?", TR("确认结束进程"), pid, name ? name : "?");
    int rc = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (rc == GTK_RESPONSE_YES) {
        gchar *cmd = g_strdup_printf("qysudo kill -9 %d", pid);
        g_spawn_command_line_async(cmd, NULL);
        g_printerr("QYMONDBG: kill pid=%d name=%s\n", pid, name ? name : "?");
        g_free(cmd);
        if (proc_info_label) {
            gchar *s = g_strdup_printf(TR("已请求结束 %d (%s)"), pid, name ? name : "?");
            gtk_label_set_text(GTK_LABEL(proc_info_label), s);
            g_free(s);
        }
    }
    g_free(name);
}

/* 搜索框内容变化：更新过滤词并立即刷新进程列表 */
static void on_proc_search_changed(GtkSearchEntry *e, gpointer ud) {
    (void)ud;
    g_free(proc_filter);
    proc_filter = g_ascii_strdown(gtk_entry_get_text(GTK_ENTRY(e)), -1);
    refresh_proc_list();
}

/* 直接向进程发送信号（kill(2)），并反馈结果 */
static void kill_proc(int pid, const char *name, int sig) {
    if (kill(pid, sig) == 0) {
        if (proc_info_label) {
            gchar *s = g_strdup_printf(TR("已向 %d (%s) 发送信号 %d"),
                                   pid, name ? name : "?", sig);
            gtk_label_set_text(GTK_LABEL(proc_info_label), s);
            g_free(s);
        }
        g_printerr("QYMONDBG: kill pid=%d sig=%d name=%s\n", pid, sig, name ? name : "?");
    } else {
        if (proc_info_label) {
            gchar *s = g_strdup_printf(TR("发送信号 %d 到 %d 失败: %s"),
                                         sig, pid, g_strerror(errno));
            gtk_label_set_text(GTK_LABEL(proc_info_label), s);
            g_free(s);
        }
        g_printerr("QYMONDBG: kill pid=%d sig=%d err=%s\n", pid, sig, g_strerror(errno));
    }
    refresh_proc_list();
}

/* 右键菜单：结束进程 (SIGTERM)，带确认对话框 */
static void on_menu_sigterm(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (ctx_pid <= 0) return;
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s %d (%s)?", TR("确认结束进程"), ctx_pid, ctx_name ? ctx_name : "?");
    int rc = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (rc == GTK_RESPONSE_YES)
        kill_proc(ctx_pid, ctx_name, SIGTERM);
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
}

/* 右键菜单：强制结束 (SIGKILL)，带确认对话框 */
static void on_menu_sigkill(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (ctx_pid <= 0) return;
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s %d (%s)?", TR("确认强制结束进程"), ctx_pid, ctx_name ? ctx_name : "?");
    int rc = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (rc == GTK_RESPONSE_YES)
        kill_proc(ctx_pid, ctx_name, SIGKILL);
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
}

/* 右键菜单：暂停进程 (SIGSTOP) */
static void on_menu_sigstop(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (ctx_pid <= 0) return;
    kill_proc(ctx_pid, ctx_name, SIGSTOP);
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
}

/* 右键菜单：继续进程 (SIGCONT) */
static void on_menu_sigcont(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (ctx_pid <= 0) return;
    kill_proc(ctx_pid, ctx_name, SIGCONT);
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
}

/* 右键菜单：复制进程名到剪贴板 */
static void on_menu_copy_name(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (ctx_pid <= 0 || !ctx_name) return;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, ctx_name, -1);
    if (proc_info_label) {
        gchar *s = g_strdup_printf(TR("已复制进程名: %s"), ctx_name);
        gtk_label_set_text(GTK_LABEL(proc_info_label), s);
        g_free(s);
    }
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
}

/* 右键菜单：复制 PID 到剪贴板 */
static void on_menu_copy_pid(GtkMenuItem *mi, gpointer ud) {
    (void)mi; (void)ud;
    if (ctx_pid <= 0) return;
    gchar *pidstr = g_strdup_printf("%d", ctx_pid);
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, pidstr, -1);
    if (proc_info_label) {
        gchar *s = g_strdup_printf(TR("已复制 PID: %s"), pidstr);
        gtk_label_set_text(GTK_LABEL(proc_info_label), s);
        g_free(s);
    }
    g_free(pidstr);
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
}

/* 进程列表右键：选中行并弹出信号菜单 */
static gboolean on_proc_button_press(GtkWidget *tv, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->button != 3 || ev->type != GDK_BUTTON_PRESS) return FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv));
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv), (gint)ev->x, (gint)ev->y,
                                      &path, NULL, NULL, NULL)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
        gtk_tree_path_free(path);
    }
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) return TRUE;
    g_free(ctx_name);
    ctx_name = NULL;
    ctx_pid = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(proc_store), &it,
                       P_PID, &ctx_pid, P_NAME, &ctx_name, -1);

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(TR("结束进程"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_sigterm), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("强制结束"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_sigkill), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("暂停进程"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_sigstop), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("继续进程"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_sigcont), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    mi = gtk_menu_item_new_with_label(TR("复制进程名"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_copy_name), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    mi = gtk_menu_item_new_with_label(TR("复制 PID"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_copy_pid), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_menu_attach_to_widget(GTK_MENU(menu), tv, NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (const GdkEvent *)ev);
    return TRUE;
}

/* 刷新进程列表按钮 */
static void on_proc_refresh(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    refresh_proc_list();
}

/* ---------- 曲线图例点击开关（release 300） ----------
 * 顶部一排 GtkToggleButton（带彩色圆点标签），点击显示/隐藏对应曲线。
 * 状态持久化到 ~/.config/lanxiu/qymon-curves.conf（每行 1/0，共 6 行）。 */
static const char *curve_name(int i) {
    switch (i) {
        case 0: return "CPU";
        case 1: return TR("内存");
        case 2: return TR("磁盘");
        case 3: return TR("网络");
        case 4: return TR("温度");
        default: return "IO";
    }
}

static const char *curve_color(int i) {
    switch (i) {
        case 0: return "#f28c26";   /* CPU 橙 */
        case 1: return "#73a6f2";  /* MEM 蓝 */
        case 2: return "#33d499";     /* DISK 绿 */
        case 3: return "#2ec2c2";    /* NET 青 */
        case 4: return "#ff5c1a";    /* TEMP 橙红 */
        default: return "#59ccf2";   /* IO 浅蓝 */
    }
}

static gchar *curve_config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "lanxiu", "qymon-curves.conf", NULL);
}

/* 启动时加载曲线开关状态（每行 1/0，共 6 行） */
static void curve_load_config(void) {
    gchar *path = curve_config_path();
    FILE *f = fopen(path, "r");
    if (f) {
        for (int i = 0; i < NCURVES; i++) {
            int v = 1;
            if (fscanf(f, "%d", &v) == 1)
                curve_show[i] = v != 0;
        }
        fclose(f);
    }
    g_free(path);
}

/* 切换时写盘 */
static void curve_save_config(void) {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "lanxiu", NULL);
    gchar *path = g_build_filename(dir, "qymon-curves.conf", NULL);
    g_mkdir_with_parents(dir, 0755);
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < NCURVES; i++)
            fprintf(f, "%d\n", curve_show[i] ? 1 : 0);
        fclose(f);
    }
    g_free(path);
    g_free(dir);
}

/* 曲线开关按钮回调：更新 curve_show[] 并重绘对应曲线区 */
static void on_curve_toggled(GtkWidget *btn, gpointer ud) {
    int i = GPOINTER_TO_INT(ud);
    if (i < 0 || i >= NCURVES) return;
    curve_show[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn));
    if (i <= 3) {
        if (main_da) gtk_widget_queue_draw(main_da);
    } else if (i == 4) {
        if (temp_da) gtk_widget_queue_draw(temp_da);
    } else {
        if (io_da) gtk_widget_queue_draw(io_da);
    }
    curve_save_config();
}

/* 构造一个曲线开关按钮：彩色圆点 + 名称，激活状态与 curve_show[] 同步 */
static GtkWidget *curve_toggle_new(int i) {
    GtkWidget *btn = gtk_toggle_button_new();
    GtkWidget *lbl = gtk_label_new(NULL);
    gchar *markup = g_strdup_printf("<span foreground='%s'>●</span> %s",
                                    curve_color(i), curve_name(i));
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(markup);
    gtk_container_add(GTK_CONTAINER(btn), lbl);
    /* 先按配置设置激活状态，再连接信号，避免启动时触发保存 */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), curve_show[i]);
    g_signal_connect(btn, "toggled", G_CALLBACK(on_curve_toggled), GINT_TO_POINTER(i));
    return btn;
}

static void activate(GtkApplication *app, gpointer ud) {
    qy_load_theme();
    curve_load_config();   /* 加载曲线开关状态（release 300） */
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), TR("澜岫系统监视器"));
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 600);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* 顶部标题: 澜岫系统监视器 — CPU xx% */
    title_label = gtk_label_new(NULL);
    add_class(title_label, "qy-mon-title");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), title_label, FALSE, FALSE, 0);

    /* 顶部大数字: CPU / 内存 / 磁盘 */
    GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    cpu_label = gtk_label_new("CPU 0%");
    add_class(cpu_label, "qy-mon-cpu");
    char lbl[64];
    g_snprintf(lbl, sizeof lbl, "%s 0%%", TR("内存"));
    mem_label = gtk_label_new(lbl);
    add_class(mem_label, "qy-mon-mem");
    g_snprintf(lbl, sizeof lbl, "%s 0%%", TR("磁盘"));
    disk_label = gtk_label_new(lbl);
    add_class(disk_label, "qy-mon-disk");
    g_snprintf(lbl, sizeof lbl, "%s --", TR("温度"));
    temp_label = gtk_label_new(lbl);
    add_class(temp_label, "qy-mon-temp");
    gtk_widget_set_name(temp_label, "qymon-temp");   /* #qymon-temp */
    g_snprintf(lbl, sizeof lbl, "%s --", TR("磁盘IO"));
    io_label = gtk_label_new(lbl);
    add_class(io_label, "qy-mon-temp");
    gtk_widget_set_name(io_label, "qymon-io");        /* #qymon-io（release 288） */
    gtk_box_pack_start(GTK_BOX(hrow), cpu_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hrow), mem_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hrow), disk_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hrow), temp_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hrow), io_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hrow, FALSE, FALSE, 0);

    /* 曲线图例开关行（release 300）：点击按钮显示/隐藏对应曲线 */
    GtkWidget *legend_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = 0; i < NCURVES; i++) {
        curve_toggle[i] = curve_toggle_new(i);
        gtk_box_pack_start(GTK_BOX(legend_bar), curve_toggle[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), legend_bar, FALSE, FALSE, 0);

    /* 实时曲线区（固定高度，下方让位给进程列表） */
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, -1, 140);
    g_signal_connect(da, "draw", G_CALLBACK(on_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), da, FALSE, FALSE, 0);
    main_da = da;   /* 曲线开关需要重绘主曲线区（release 300） */

    /* 温度曲线区（深蓝底 + 橙线 #FF5C1A，60 点历史；无 thermal_zone 时隐藏） */
    temp_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(temp_da, -1, 72);
    g_signal_connect(temp_da, "draw", G_CALLBACK(on_temp_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), temp_da, FALSE, FALSE, 0);

    /* 磁盘 IO 曲线区（release 288：/proc/diskstats 吞吐，60 点历史） */
    io_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(io_da, -1, 72);
    g_signal_connect(io_da, "draw", G_CALLBACK(on_io_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), io_da, FALSE, FALSE, 0);

    /* 内存占用 TOP 5 进程列表（快速视图） */
    proc_label = gtk_label_new(NULL);
    add_class(proc_label, "qy-mon-proc");
    gtk_label_set_xalign(GTK_LABEL(proc_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), proc_label, FALSE, FALSE, 0);

    /* 全量进程列表（可排序、可选择） */
    proc_store = gtk_list_store_new(N_PCOLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_LONG, G_TYPE_DOUBLE);
    proc_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(proc_store));
    GtkCellRenderer *pr = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("PID", pr, "text", P_PID, NULL);
    gtk_tree_view_column_set_sort_column_id(col, P_PID);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(proc_tv), col);
    col = gtk_tree_view_column_new_with_attributes(TR("进程"), pr, "text", P_NAME, NULL);
    gtk_tree_view_column_set_sort_column_id(col, P_NAME);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(proc_tv), col);
    col = gtk_tree_view_column_new_with_attributes(TR("内存 KB"), pr, "text", P_MEM, NULL);
    gtk_tree_view_column_set_sort_column_id(col, P_MEM);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(proc_tv), col);
    /* CPU% 列（release 271）：double 值排序，自定义格式化显示 */
    col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, TR("CPU%"));
    gtk_tree_view_column_pack_start(col, pr, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, pr, cpu_cell_data_func, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(col, P_CPU);
    gtk_tree_view_column_set_clickable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(proc_tv), col);
    /* 进程搜索框 + 信息标签：搜索框输入即过滤（进程刷新时应用），置于列表上方 */
    GtkWidget *proc_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    proc_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(proc_search), TR("搜索进程…"));
    g_signal_connect(proc_search, "search-changed", G_CALLBACK(on_proc_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(proc_top), proc_search, TRUE, TRUE, 0);
    proc_info_label = gtk_label_new("");
    add_class(proc_info_label, "qy-mon-info");
    gtk_widget_set_halign(proc_info_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(proc_top), proc_info_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), proc_top, FALSE, FALSE, 0);

    GtkWidget *psw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(psw, -1, 190);
    gtk_container_add(GTK_CONTAINER(psw), proc_tv);
    gtk_box_pack_start(GTK_BOX(vbox), psw, TRUE, TRUE, 0);

    /* 进程行右键菜单：结束进程 / 强制结束 */
    gtk_widget_add_events(proc_tv, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(proc_tv, "button-press-event", G_CALLBACK(on_proc_button_press), NULL);

    /* 进程操作按钮行 */
    GtkWidget *pbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_kill = gtk_button_new_with_label(TR("结束进程"));
    qy_add_class(b_kill, "qy-btn");
    g_signal_connect(b_kill, "clicked", G_CALLBACK(on_kill_clicked), NULL);
    GtkWidget *b_pref = gtk_button_new_with_label(TR("刷新进程"));
    qy_add_class(b_pref, "qy-btn");
    g_signal_connect(b_pref, "clicked", G_CALLBACK(on_proc_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(pbar), b_kill, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pbar), b_pref, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), pbar, FALSE, FALSE, 0);

    /* 底部信息栏: 运行时间 / 负载 / 进程 */
    info_label = gtk_label_new("");
    add_class(info_label, "qy-mon-info");
    gtk_widget_set_halign(info_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), info_label, FALSE, FALSE, 0);

    /* GPU 信息行（#qymon-gpu） */
    gpu_label = gtk_label_new(TR("GPU: 未检测到 GPU"));
    gtk_widget_set_name(gpu_label, "qymon-gpu");
    add_class(gpu_label, "qy-mon-info");
    gtk_widget_set_halign(gpu_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), gpu_label, FALSE, FALSE, 0);

    /* hwmon 传感器信息区（#qymon-fan 风扇转速 / #qymon-hwmon 附加温度；无传感器时隐藏） */
    fan_label = gtk_label_new(NULL);
    gtk_widget_set_name(fan_label, "qymon-fan");
    add_class(fan_label, "qy-mon-info");
    gtk_widget_set_halign(fan_label, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(fan_label, TRUE);
    gtk_widget_hide(fan_label);
    gtk_box_pack_start(GTK_BOX(vbox), fan_label, FALSE, FALSE, 0);

    hwmon_label = gtk_label_new(NULL);
    gtk_widget_set_name(hwmon_label, "qymon-hwmon");
    add_class(hwmon_label, "qy-mon-info");
    gtk_widget_set_halign(hwmon_label, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(hwmon_label, TRUE);
    gtk_widget_hide(hwmon_label);
    gtk_box_pack_start(GTK_BOX(vbox), hwmon_label, FALSE, FALSE, 0);

    /* 默认按内存降序 */
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(proc_store), P_MEM, GTK_SORT_DESCENDING);
    refresh_proc_list();

    /* 读取 GPU 信息（无 lspci / 未找到显卡时显示「未检测到 GPU」） */
    read_gpu_info();

    /* hwmon 传感器: 启动立即读取，之后每 2s 刷新 */
    read_hwmon();
    g_timeout_add_seconds(2, hwmon_tick, NULL);

    gtk_widget_show_all(win);
    g_timeout_add_seconds(1, tick, da);
    /* 首帧立即采样 */
    tick(da);

    /* 无 thermal_zone 时隐藏温度相关 UI */
    if (read_cpu_temp() < 0) {
        if (temp_label) gtk_widget_set_visible(temp_label, FALSE);
        if (temp_da) gtk_widget_set_visible(temp_da, FALSE);
        if (curve_toggle[4]) gtk_widget_set_visible(curve_toggle[4], FALSE);
    }
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.lanxiu.mon", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *own_argv[2] = { argv[0], NULL };
    int rc = g_application_run(G_APPLICATION(app), 1, own_argv);
    g_object_unref(app);
    return rc;
}
