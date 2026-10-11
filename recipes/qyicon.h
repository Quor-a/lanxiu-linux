/*
 * qyicon.h — 澜岫统一 cairo 图标层
 * 与托盘/通知铃铛同源：1.5px 线宽、圆帽圆角、玻璃底 + 单一强调色。
 * 供 qydesktop / qyappmenu / qyfiles 等复用，替代「彩色块+字符」。
 */
#ifndef QYICON_H
#define QYICON_H

#include <gtk/gtk.h>

/* 图标标识：每个枚举对应一个 cairo 线稿绘制函数 */
typedef enum {
    QY_ICON_TERM,      /* 终端 */
    QY_ICON_BROWSER,   /* 浏览器 */
    QY_ICON_FILES,     /* 文件管理器 */
    QY_ICON_IMAGE,     /* 图片查看 */
    QY_ICON_ARCHIVE,   /* 压缩管理 */
    QY_ICON_TRASH,     /* 回收站 */
    QY_ICON_EDIT,      /* 文本编辑 */
    QY_ICON_STORE,     /* 软件中心 */
    QY_ICON_SHOT,      /* 截图工具 */
    QY_ICON_CLIPBOARD, /* 剪贴板 */
    QY_ICON_LOCK,      /* 锁屏 */
    QY_ICON_SEARCH,    /* 全局搜索 */
    QY_ICON_MUSIC,     /* 音乐播放器 */
    QY_ICON_SWITCHER,  /* 窗口切换器 */
    QY_ICON_DRIVER,    /* 驱动管理器 */
    QY_ICON_GIT,       /* Git 工具 */
    QY_ICON_WELCOME,   /* 欢迎 */
    QY_ICON_HOME,      /* 主文件夹 */
    QY_ICON_SETTINGS,  /* 系统设置 */
    QY_ICON_MONITOR,   /* 系统监视 */
    QY_ICON_SETUP,     /* 系统安装 */
    QY_ICON_USERS,     /* 用户管理 */
    QY_ICON_NETWORK,   /* 网络管理 */
    QY_ICON_VIDEO,     /* 视频 */
    QY_ICON_DOC,       /* PDF/文档 */
    QY_ICON_FILE,      /* 通用文件 */
    QY_ICON_BACK,      /* 返回 */
    QY_ICON_FORWARD,   /* 前进 */
    QY_ICON_UP,        /* 上移/父目录 */
    QY_ICON_REFRESH,   /* 刷新 */
    QY_ICON_DELETE,    /* 删除（复用 draw_trash） */
    QY_ICON_COPY,      /* 复制 */
    QY_ICON_CUT,       /* 剪切 */
    QY_ICON_PASTE,     /* 粘贴（复用 draw_clipboard） */
    QY_ICON_CLOSE,     /* 关闭 */
    QY_ICON_MIN,       /* 最小化 */
    QY_ICON_MAX,       /* 最大化 */
    QY_ICON_PLUS,      /* 加号 */
    QY_ICON_VOLUME,    /* 音量 */
    QY_ICON_POWER,     /* 电源 */
    QY_ICON_GRID,      /* 应用网格 */
    QY_ICON_PLAY,      /* 播放 */
    QY_ICON_PAUSE,     /* 暂停 */
    QY_ICON_STOP,      /* 停止 */
    QY_ICON_PREV,      /* 上一个 */
    QY_ICON_NEXT,      /* 下一个 */
    QY_ICON_BOOKMARK,  /* 书签 */
    QY_ICON_LIST,      /* 列表视图 */
    QY_ICON_CALC,      /* 计算器 */
    QY_ICON_SAVE,      /* 保存 */
    QY_ICON_ROTATE,    /* 旋转 */
    QY_ICON_FIT,       /* 适应窗口 */
    QY_ICON_OPEN,      /* 打开/展开 */
    QY_ICON_CLOCK,     /* 时钟 */
    QY_ICON_KEYBOARD, /* 键盘 */
    QY_ICON_WEATHER,   /* 天气 */
    QY_ICON_NOTE,      /* 笔记 */
    QY_ICON_COUNT
} QyIconId;

/* 在 (x,y) 起点的 size×size 区域内绘制线稿图标（24×24 逻辑坐标缩放）。
 * color 为描边颜色（NULL 时用白色）；filled 预留（TRUE 时部分闭合形状填充）。 */
void qy_icon_draw(cairo_t *cr, QyIconId id,
                  double x, double y, double size,
                  gboolean filled, const GdkRGBA *color);

/* 玻璃拟态圆角底 + 白色线稿图标（替代「彩色块+字符」） */
void qy_icon_tile(cairo_t *cr, QyIconId id,
                  double x, double y, double size, double radius,
                  const GdkRGBA *bg, const GdkRGBA *color);

/* 渲染为 GdkPixbuf（供 GtkImage / GtkIconView 使用） */
GdkPixbuf *qy_icon_pixbuf(QyIconId id, int px, const GdkRGBA *bg);

/* 应用名 → 统一图标映射（供启动器/任务栏/应用菜单复用） */
QyIconId qy_icon_for_app(const char *app);

#endif /* QYICON_H */