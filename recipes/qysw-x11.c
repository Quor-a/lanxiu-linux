/* qysw-x11.c — 窗口切换器 X11 辅助程序
 * 用法:
 *   qysw-x11          枚举根窗口子窗口, 输出 "id|app|title" 每行
 *   qysw-x11 -a <id>  激活指定窗口 (XRaiseWindow + XSetInputFocus)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 读取 _NET_WM_PID，再映射 /proc/<pid>/comm 得到应用名 */
static char *window_app(Display *dpy, Window w) {
    Atom prop = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom type;
    int format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    unsigned long pid = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_CARDINAL,
                           &type, &format, &nitems, &bytes_after, &data) == Success
        && data && format == 32 && nitems >= 1) {
        pid = *(unsigned long *)data;
        XFree(data);
    }
    if (pid == 0) return NULL;
    char path[64];
    snprintf(path, sizeof path, "/proc/%lu/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[128];
    char *app = NULL;
    if (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (buf[0]) app = strdup(buf);
    }
    fclose(f);
    return app;
}

static void list_windows(Display *dpy) {
    Window root = DefaultRootWindow(dpy);
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int n = 0;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n) && children) {
        for (unsigned int i = 0; i < n; i++) {
            char *name = NULL;
            if (XFetchName(dpy, children[i], &name) && name) {
                char *app = window_app(dpy, children[i]);
                printf("0x%lx|%s|%s\n", (unsigned long)children[i],
                       app ? app : "X11", name);
                if (app) free(app);
                XFree(name);
            }
        }
        XFree(children);
    }
}

static void activate_window(Display *dpy, unsigned long id) {
    Window w = (Window)id;
    XRaiseWindow(dpy, w);
    XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
    XMapRaised(dpy, w);
    XFlush(dpy);
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "qysw-x11: cannot open display\n");
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "-a") == 0) {
        activate_window(dpy, strtoul(argv[2], NULL, 0));
    } else {
        list_windows(dpy);
    }
    XCloseDisplay(dpy);
    return 0;
}