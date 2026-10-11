/* qyweather.c — 澜岫天气
 * libcurl 拉取 https://wttr.in/?format=j1 的 JSON，json-c 解析：
 * 顶部城市 + 当前温度 + 描述，下方 3 天预报（日期/最高/最低/描述）。
 * 输入框改城市回车刷新，失败显示友好错误（#qyweather-err）而非崩溃。
 * 自动化: QYWEATHER_CITY=上海 启动后自动查询该城市（用于自测截图）。
 */
#include <gtk/gtk.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "qytheme.h"
#include "qyl10n.h"
#include "qyicon.h"

#define FORECAST_DAYS 3

static GtkWidget *city_lb  = NULL;   /* #qyweather-city */
static GtkWidget *temp_lb  = NULL;   /* #qyweather-temp */
static GtkWidget *desc_lb  = NULL;   /* #qyweather-desc */
static GtkWidget *err_lb   = NULL;   /* #qyweather-err */
static GtkWidget *day_lb[FORECAST_DAYS];
static GtkWidget *hi_lb[FORECAST_DAYS];
static GtkWidget *lo_lb[FORECAST_DAYS];
static GtkWidget *fdesc_lb[FORECAST_DAYS];
static GtkWidget *entry   = NULL;
static gboolean fetch_busy = FALSE;

/* 解析结果（线程填充，idle 回调读取） */
typedef struct {
    gboolean ok;
    char city[128];
    char temp[32];
    char desc[128];
    char date[FORECAST_DAYS][32];
    char hi[FORECAST_DAYS][16];
    char lo[FORECAST_DAYS][16];
    char fdesc[FORECAST_DAYS][128];
    char err[256];
} WeatherResult;

static gboolean fetch_done(gpointer data);   /* 前向声明 */

/* ---------- curl 写回调：追加到 GString ---------- */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    GString *s = (GString *)ud;
    g_string_append_len(s, ptr, size * nmemb);
    return size * nmemb;
}

/* 从 JSON 中取字符串字段（路径已知存在时） */
static const char *jstr(json_object *obj, const char *key) {
    json_object *v = NULL;
    if (obj && json_object_object_get_ex(obj, key, &v) && v)
        return json_object_get_string(v);
    return NULL;
}

/* 取 weatherDesc[0].value */
static const char *j_weather_desc(json_object *cond) {
    json_object *arr = NULL;
    if (cond && json_object_object_get_ex(cond, "weatherDesc", &arr) &&
        json_object_is_type(arr, json_type_array)) {
        json_object *first = json_object_array_get_idx(arr, 0);
        if (first) return jstr(first, "value");
    }
    return NULL;
}

/* ---------- 后台线程：curl 拉取 + json-c 解析 ---------- */
static gpointer fetch_thread(gpointer data) {
    char *city = (char *)data;
    WeatherResult *r = g_new0(WeatherResult, 1);

    CURL *curl = curl_easy_init();
    if (!curl) {
        r->ok = FALSE;
        g_strlcpy(r->err, TR("无法初始化网络库"), sizeof r->err);
        g_idle_add((GSourceFunc)fetch_done, r);
        g_free(city);
        return NULL;
    }

    CURLcode rc = CURLE_OK;
    GString *buf = g_string_new(NULL);
    char *esc = curl_easy_escape(curl, city, 0);
    char *url = g_strdup_printf("https://wttr.in/%s?format=j1", esc ? esc : "");
    if (esc) curl_free(esc);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LANXIU-Weather/1.0");
    rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        r->ok = FALSE;
        g_snprintf(r->err, sizeof r->err, "%s: %s", TR("网络请求失败"), curl_easy_strerror(rc));
    } else if (buf->len == 0) {
        r->ok = FALSE;
        g_strlcpy(r->err, TR("服务器返回空数据"), sizeof r->err);
    } else {
        json_object *root = json_tokener_parse(buf->str);
        if (!root || !json_object_is_type(root, json_type_object)) {
            r->ok = FALSE;
            g_strlcpy(r->err, TR("天气数据解析失败（请检查城市名）"), sizeof r->err);
        } else {
            /* 当前天气 */
            json_object *cc_arr = NULL, *cc = NULL;
            if (json_object_object_get_ex(root, "current_condition", &cc_arr) &&
                json_object_is_type(cc_arr, json_type_array))
                cc = json_object_array_get_idx(cc_arr, 0);
            if (cc) {
                const char *t = jstr(cc, "temp_C");
                const char *d = j_weather_desc(cc);
                g_strlcpy(r->temp, t ? t : "--", sizeof r->temp);
                g_strlcpy(r->desc, d ? d : TR("未知"), sizeof r->desc);
            }
            /* 3 天预报 */
            json_object *weather_arr = NULL;
            if (json_object_object_get_ex(root, "weather", &weather_arr) &&
                json_object_is_type(weather_arr, json_type_array)) {
                int n = (int)json_object_array_length(weather_arr);
                if (n > FORECAST_DAYS) n = FORECAST_DAYS;
                for (int i = 0; i < n; i++) {
                    json_object *day = json_object_array_get_idx(weather_arr, i);
                    if (!day) break;
                    const char *date = jstr(day, "date");
                    const char *hi = jstr(day, "maxtempC");
                    const char *lo = jstr(day, "mintempC");
                    g_strlcpy(r->date[i], date ? date : "--", sizeof r->date[i]);
                    g_strlcpy(r->hi[i],  hi ? hi : "--", sizeof r->hi[i]);
                    g_strlcpy(r->lo[i],  lo ? lo : "--", sizeof r->lo[i]);
                    /* 描述：取当日中午（hourly[5]）的 weatherDesc，不足则取第 0 条 */
                    json_object *hourly = NULL;
                    const char *fd = NULL;
                    if (json_object_object_get_ex(day, "hourly", &hourly) &&
                        json_object_is_type(hourly, json_type_array)) {
                        int hn = (int)json_object_array_length(hourly);
                        json_object *h = json_object_array_get_idx(hourly, hn > 5 ? 5 : 0);
                        if (h) fd = j_weather_desc(h);
                    }
                    g_strlcpy(r->fdesc[i], fd ? fd : "", sizeof r->fdesc[i]);
                }
            }
            g_strlcpy(r->city, city, sizeof r->city);
            r->ok = TRUE;
            json_object_put(root);
        }
    }
    g_string_free(buf, TRUE);
    g_free(url);
    g_free(city);

    g_idle_add((GSourceFunc)fetch_done, r);
    return NULL;
}

/* ---------- 主线程：应用结果 ---------- */
static gboolean fetch_done(gpointer data) {
    WeatherResult *r = (WeatherResult *)data;
    fetch_busy = FALSE;

    if (!r->ok) {
        gtk_label_set_text(GTK_LABEL(err_lb), r->err);
        gtk_widget_show(err_lb);
        gtk_label_set_text(GTK_LABEL(city_lb), gtk_entry_get_text(GTK_ENTRY(entry)));
        gtk_label_set_text(GTK_LABEL(temp_lb), "--");
        gtk_label_set_text(GTK_LABEL(desc_lb), "");
        g_printerr("QYWEATHERDBG: error=%s\n", r->err);
    } else {
        char buf[64];
        gtk_label_set_text(GTK_LABEL(err_lb), "");
        gtk_widget_hide(err_lb);
        gtk_label_set_text(GTK_LABEL(city_lb), r->city);
        g_snprintf(buf, sizeof buf, "%s°C", r->temp);
        gtk_label_set_text(GTK_LABEL(temp_lb), buf);
        gtk_label_set_text(GTK_LABEL(desc_lb), r->desc);
        for (int i = 0; i < FORECAST_DAYS; i++) {
            gtk_label_set_text(GTK_LABEL(day_lb[i]), r->date[i]);
            g_snprintf(buf, sizeof buf, "%s°C", r->hi[i]);
            gtk_label_set_text(GTK_LABEL(hi_lb[i]), buf);
            g_snprintf(buf, sizeof buf, "%s°C", r->lo[i]);
            gtk_label_set_text(GTK_LABEL(lo_lb[i]), buf);
            gtk_label_set_text(GTK_LABEL(fdesc_lb[i]), r->fdesc[i]);
        }
        g_printerr("QYWEATHERDBG: city=%s temp=%s desc=%s\n", r->city, r->temp, r->desc);
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* ---------- 发起查询 ---------- */
static void start_fetch(const char *city) {
    if (fetch_busy) return;
    if (!city || !city[0]) return;
    fetch_busy = TRUE;
    gtk_label_set_text(GTK_LABEL(err_lb), TR("正在查询..."));
    gtk_widget_show(err_lb);
    GThread *th = g_thread_new("weather-fetch", fetch_thread, g_strdup(city));
    g_thread_unref(th);
}

static void on_entry_activate(GtkEntry *e, gpointer ud) {
    (void)ud;
    start_fetch(gtk_entry_get_text(e));
}

/* ---------- UI ---------- */
static GtkWidget *make_day_card(int idx) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    qy_add_class(box, "qy-weather-day-card");
    gtk_widget_set_hexpand(box, TRUE);

    day_lb[idx] = gtk_label_new("--");
    qy_add_class(day_lb[idx], "qy-weather-day");
    hi_lb[idx]  = gtk_label_new("--");
    qy_add_class(hi_lb[idx], "qy-weather-hi");
    lo_lb[idx]  = gtk_label_new("--");
    qy_add_class(lo_lb[idx], "qy-weather-lo");
    fdesc_lb[idx] = gtk_label_new("");
    qy_add_class(fdesc_lb[idx], "qy-weather-fdesc");
    gtk_label_set_line_wrap(GTK_LABEL(fdesc_lb[idx]), TRUE);

    gtk_box_pack_start(GTK_BOX(box), day_lb[idx], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hi_lb[idx], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lo_lb[idx], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), fdesc_lb[idx], FALSE, FALSE, 0);
    return box;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    qy_load_theme();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), TR("天气"));
    gtk_widget_set_name(win, "qy-weather-win");
    qy_make_titlebar(GTK_WINDOW(win), TR("天气"));
    qy_window_setup(GTK_WINDOW(win), 380, 420, 340, 400, TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(win), outer);

    /* 内容卡片（复用 .qy-welcome-card 风格） */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    qy_add_class(card, "qy-welcome-card");
    gtk_box_pack_start(GTK_BOX(outer), card, TRUE, TRUE, 0);

    city_lb = gtk_label_new(TR("北京"));
    gtk_widget_set_name(city_lb, "qyweather-city");
    gtk_box_pack_start(GTK_BOX(card), city_lb, FALSE, FALSE, 0);

    temp_lb = gtk_label_new("--");
    gtk_widget_set_name(temp_lb, "qyweather-temp");
    gtk_box_pack_start(GTK_BOX(card), temp_lb, FALSE, FALSE, 0);

    desc_lb = gtk_label_new("");
    gtk_widget_set_name(desc_lb, "qyweather-desc");
    gtk_box_pack_start(GTK_BOX(card), desc_lb, FALSE, FALSE, 0);

    err_lb = gtk_label_new("");
    gtk_widget_set_name(err_lb, "qyweather-err");
    gtk_box_pack_start(GTK_BOX(card), err_lb, FALSE, FALSE, 2);

    /* 3 天预报 */
    GtkWidget *fc_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(fc_title), g_strdup_printf("<b>%s</b>", TR("3 天预报")));
    gtk_widget_set_halign(fc_title, GTK_ALIGN_START);
    qy_add_class(fc_title, "qy-weather-fc-title");
    gtk_box_pack_start(GTK_BOX(card), fc_title, FALSE, FALSE, 6);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    for (int i = 0; i < FORECAST_DAYS; i++)
        gtk_grid_attach(GTK_GRID(grid), make_day_card(i), i, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(card), grid, TRUE, TRUE, 2);

    /* 城市输入行 */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry = gtk_entry_new();
    qy_add_class(entry, "qy-search-entry");
    gtk_entry_set_text(GTK_ENTRY(entry), TR("北京"));
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TR("输入城市，回车查询"));
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    GtkWidget *btn = gtk_button_new_with_label(TR("查询"));
    qy_add_class(btn, "qy-btn");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    gtk_widget_hide(err_lb);

    /* 初始查询默认城市 */
    const char *city = g_getenv("QYWEATHER_CITY");
    if (!city || !city[0]) city = "北京";
    gtk_entry_set_text(GTK_ENTRY(entry), city);
    start_fetch(city);

    gtk_main();
    curl_global_cleanup();
    return 0;
}