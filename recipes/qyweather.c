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

/* release 306: 多城市管理 */
#define MAX_SAVED_CITIES 8
static GtkWidget *combo = NULL;           /* 已保存城市下拉 */
static gboolean combo_updating = FALSE;    /* 程序化更新下拉时抑制 changed 查询 */

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
    /* release 289: 记住上次查询城市 */
    {
        gchar *path = g_build_filename(g_get_home_dir(), ".config/lanxiu/qyweather-city.conf", NULL);
        gchar *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        g_file_set_contents(path, city, -1, NULL);
        g_free(path);
    }
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

/* ---------- 多城市管理（release 306） ---------- */
static gchar *cities_config_path(void) {
    return g_build_filename(g_get_home_dir(), ".config/lanxiu/qyweather-cities.conf", NULL);
}

/* 当前下拉是否已含 city（用 g_strcmp0 去重比较） */
static gboolean cities_contains(const char *city) {
    if (!city || !city[0]) return FALSE;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gchar *s = NULL;
            gtk_tree_model_get(model, &iter, 0, &s, -1);
            gboolean same = (s && g_strcmp0(s, city) == 0);
            g_free(s);
            if (same) return TRUE;
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    return FALSE;
}

static int cities_count(void) {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    return gtk_tree_model_iter_n_children(model, NULL);
}

/* 把下拉当前内容写回配置文件（每行一个城市名） */
static void cities_save(void) {
    gchar *path = cities_config_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GString *buf = g_string_new(NULL);
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gchar *s = NULL;
            gtk_tree_model_get(model, &iter, 0, &s, -1);
            if (s && s[0]) {
                g_string_append(buf, s);
                g_string_append_c(buf, '\n');
            }
            g_free(s);
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    g_file_set_contents(path, buf->str, (gssize)buf->len, NULL);
    g_string_free(buf, TRUE);
    g_free(path);
}

/* 启动时从配置文件加载城市列表填充下拉（去重，最多 8 个） */
static void cities_load(void) {
    gchar *path = cities_config_path();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i] && cities_count() < MAX_SAVED_CITIES; i++) {
            gchar *city = g_strstrip(lines[i]);
            if (city[0] && !cities_contains(city))
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), city);
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), -1);
}

/* 选中下拉中指定城市（不触发查询） */
static void cities_select(const char *city) {
    if (!city || !city[0]) return;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    int idx = 0;
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gchar *s = NULL;
            gtk_tree_model_get(model, &iter, 0, &s, -1);
            gboolean same = (s && g_strcmp0(s, city) == 0);
            g_free(s);
            if (same) {
                combo_updating = TRUE;
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx);
                combo_updating = FALSE;
                return;
            }
            idx++;
        } while (gtk_tree_model_iter_next(model, &iter));
    }
}

/* 把城市加入下拉并写回配置（去重，最多 8 个）；已存在则只选中 */
static gboolean cities_add(const char *city) {
    gchar *trimmed = g_strstrip(g_strdup(city));
    if (!trimmed[0]) { g_free(trimmed); return FALSE; }
    if (!cities_contains(trimmed)) {
        if (cities_count() >= MAX_SAVED_CITIES) {
            gtk_label_set_text(GTK_LABEL(err_lb), TR("最多保存 8 个城市"));
            gtk_widget_show(err_lb);
            g_free(trimmed);
            return FALSE;
        }
        combo_updating = TRUE;
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), trimmed);
        combo_updating = FALSE;
        cities_save();
    }
    cities_select(trimmed);
    g_free(trimmed);
    return TRUE;
}

/* 「保存城市」：把当前输入的城市保存到列表 */
static void on_save_city(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (text && text[0]) cities_add(text);
}

/* 「删除城市」：删除下拉当前选中的城市并写回配置 */
static void on_delete_city(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    if (active < 0) return;
    combo_updating = TRUE;
    gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(combo), active);
    combo_updating = FALSE;
    cities_save();
}

/* 下拉选择某个城市后自动查询该城市天气 */
static void on_combo_changed(GtkComboBox *cb, gpointer ud) {
    (void)ud;
    if (combo_updating) return;
    const gchar *city = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cb));
    if (city && city[0]) {
        gtk_entry_set_text(GTK_ENTRY(entry), city);
        start_fetch(city);
    }
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

    /* 城市管理行（release 306）：已保存城市下拉 + 删除城市 */
    GtkWidget *city_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    combo = gtk_combo_box_text_new();
    gtk_widget_set_name(combo, "qyweather-city-combo");
    gtk_widget_set_hexpand(combo, TRUE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), -1);
    cities_load();   /* 启动时加载已保存城市填充下拉 */
    GtkWidget *del_btn = gtk_button_new_with_label(TR("删除城市"));
    qy_add_class(del_btn, "qy-btn-danger");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_city), NULL);
    gtk_box_pack_start(GTK_BOX(city_row), combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(city_row), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), city_row, FALSE, FALSE, 0);

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
    GtkWidget *save_btn = gtk_button_new_with_label(TR("保存城市"));
    qy_add_class(save_btn, "qy-btn");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_city), NULL);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    gtk_widget_hide(err_lb);

    /* 初始查询默认城市（release 289：优先记忆的上次城市） */
    const char *city = g_getenv("QYWEATHER_CITY");
    if (city && city[0]) {
        gtk_entry_set_text(GTK_ENTRY(entry), city);
        start_fetch(city);
        cities_add(city);   /* release 306: 环境变量指定城市可加入列表 */
    } else {
        gchar *path = g_build_filename(g_get_home_dir(), ".config/lanxiu/qyweather-city.conf", NULL);
        gchar *content = NULL;
        if (g_file_get_contents(path, &content, NULL, NULL) && content && content[0]) {
            gchar *saved = g_strstrip(content);
            gtk_entry_set_text(GTK_ENTRY(entry), saved);
            start_fetch(saved);
        } else {
            gtk_entry_set_text(GTK_ENTRY(entry), TR("北京"));
            start_fetch(TR("北京"));
        }
        g_free(content);
        g_free(path);
    }

    /* release 306: 下拉选择查询。启动填充完成后才连接，避免加载/环境变量加入时误触发 */
    g_signal_connect(combo, "changed", G_CALLBACK(on_combo_changed), NULL);

    gtk_main();
    curl_global_cleanup();
    return 0;
}