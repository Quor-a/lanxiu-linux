"""qydesktop —— 澜岫桌面 shell（GTK3 单窗口桌面：顶栏 + Dock + 壁纸桌面）

源码：recipes/qydesktop.c、recipes/qytheme.css、recipes/weston.ini（随仓库自带）
许可证：MIT
"""

name = "qydesktop"
version = "0.1.0"
release = 276
summary = "澜岫桌面 shell（GTK3 单窗口 + 主题）"
license = "MIT"

source = []
sha256 = []

depends = ["gtk3", "glib"]
makedepends = ["gtk3"]

requires_build_machine = True
network = False
compression = "xz"

import shutil
from pathlib import Path

RECIPE_C = Path(__file__).parent / "qydesktop.c"
RECIPE_FILES_C = Path(__file__).parent / "qyfiles.c"
RECIPE_SETTINGS_C = Path(__file__).parent / "qysettings.c"
RECIPE_APPMENU_C = Path(__file__).parent / "qyappmenu.c"
RECIPE_EDIT_C = Path(__file__).parent / "qyedit.c"
RECIPE_MON_C = Path(__file__).parent / "qymon.c"
RECIPE_VIEW_C = Path(__file__).parent / "qyview.c"
RECIPE_CTL_C = Path(__file__).parent / "qyctl.c"
RECIPE_ARC_C = Path(__file__).parent / "qyarc.c"
RECIPE_STORE_C = Path(__file__).parent / "qystore.c"
RECIPE_INST_C = Path(__file__).parent / "qypkg-inst.c"
RECIPE_NET_C = Path(__file__).parent / "qynet.c"
RECIPE_BROWSER_C = Path(__file__).parent / "qybrowser.c"
RECIPE_SHOT_C = Path(__file__).parent / "qyshot.c"
RECIPE_SHOTCAP_C = Path(__file__).parent / "qyshot-capture.c"
RECIPE_CLIP_C = Path(__file__).parent / "qyclip.c"
RECIPE_LOCK_C = Path(__file__).parent / "qylock.c"
RECIPE_SEARCH_C = Path(__file__).parent / "qysearch.c"
RECIPE_MEDIA_C = Path(__file__).parent / "qymedia.c"
RECIPE_CALC_C = Path(__file__).parent / "qycalc.c"
RECIPE_SWX11_C = Path(__file__).parent / "qysw-x11.c"
RECIPE_SWITCH_C = Path(__file__).parent / "qyswitcher.c"
RECIPE_NOTIFY_C = Path(__file__).parent / "qynotify.c"
RECIPE_NOTIFD_C = Path(__file__).parent / "qynotifd.c"
RECIPE_DRIVER_C = Path(__file__).parent / "qydriver.c"
RECIPE_GIT_C = Path(__file__).parent / "qygit.c"
# v2.x (release 247-252 新应用)：全量集成进 qydesktop 包
RECIPE_CLOCK_C = Path(__file__).parent / "qyclock.c"
RECIPE_WEATHER_C = Path(__file__).parent / "qyweather.c"
RECIPE_NOTE_C = Path(__file__).parent / "qynote.c"
RECIPE_CONVERT_C = Path(__file__).parent / "qyconvert.c"
RECIPE_HELP_C = Path(__file__).parent / "qyhelp.c"
RECIPE_DISK_C = Path(__file__).parent / "qydisk.c"
RECIPE_AUDIO_C = Path(__file__).parent / "qyaudio.c"
RECIPE_POWER_C = Path(__file__).parent / "qypower.c"
RECIPE_REC_C = Path(__file__).parent / "qyrec.c"
RECIPE_BACKUP_C = Path(__file__).parent / "qybackup.c"
RECIPE_MIME_C = Path(__file__).parent / "qymime.c"
RECIPE_COLOR_C = Path(__file__).parent / "qycolor.c"
RECIPE_TODO_C = Path(__file__).parent / "qytodo.c"
RECIPE_QYICON_C = Path(__file__).parent / "qyicon.c"
RECIPE_QYICON_H = Path(__file__).parent / "qyicon.h"


def build(ctx):
    shutil.copy(RECIPE_C, Path(ctx.srcdir) / "qydesktop.c")
    shutil.copy(RECIPE_FILES_C, Path(ctx.srcdir) / "qyfiles.c")
    shutil.copy(RECIPE_SETTINGS_C, Path(ctx.srcdir) / "qysettings.c")
    shutil.copy(RECIPE_APPMENU_C, Path(ctx.srcdir) / "qyappmenu.c")
    shutil.copy(RECIPE_QYICON_C, Path(ctx.srcdir) / "qyicon.c")
    shutil.copy(RECIPE_QYICON_H, Path(ctx.srcdir) / "qyicon.h")
    shutil.copy(Path(__file__).parent / "qyboot.c", Path(ctx.srcdir) / "qyboot.c")
    shutil.copy(RECIPE_EDIT_C, Path(ctx.srcdir) / "qyedit.c")
    shutil.copy(RECIPE_MON_C, Path(ctx.srcdir) / "qymon.c")
    shutil.copy(RECIPE_VIEW_C, Path(ctx.srcdir) / "qyview.c")
    shutil.copy(RECIPE_CTL_C, Path(ctx.srcdir) / "qyctl.c")
    shutil.copy(RECIPE_ARC_C, Path(ctx.srcdir) / "qyarc.c")
    shutil.copy(RECIPE_NET_C, Path(ctx.srcdir) / "qynet.c")
    shutil.copy(RECIPE_STORE_C, Path(ctx.srcdir) / "qystore.c")
    shutil.copy(RECIPE_INST_C, Path(ctx.srcdir) / "qypkg-inst.c")
    shutil.copy(RECIPE_BROWSER_C, Path(ctx.srcdir) / "qybrowser.c")
    shutil.copy(RECIPE_SHOT_C, Path(ctx.srcdir) / "qyshot.c")
    shutil.copy(RECIPE_SHOTCAP_C, Path(ctx.srcdir) / "qyshot-capture.c")
    shutil.copy(RECIPE_CLIP_C, Path(ctx.srcdir) / "qyclip.c")
    shutil.copy(RECIPE_LOCK_C, Path(ctx.srcdir) / "qylock.c")
    shutil.copy(RECIPE_SEARCH_C, Path(ctx.srcdir) / "qysearch.c")
    shutil.copy(RECIPE_MEDIA_C, Path(ctx.srcdir) / "qymedia.c")
    shutil.copy(RECIPE_CALC_C, Path(ctx.srcdir) / "qycalc.c")
    shutil.copy(RECIPE_SWX11_C, Path(ctx.srcdir) / "qysw-x11.c")
    shutil.copy(RECIPE_SWITCH_C, Path(ctx.srcdir) / "qyswitcher.c")
    shutil.copy(RECIPE_NOTIFY_C, Path(ctx.srcdir) / "qynotify.c")
    shutil.copy(RECIPE_NOTIFD_C, Path(ctx.srcdir) / "qynotifd.c")
    shutil.copy(RECIPE_DRIVER_C, Path(ctx.srcdir) / "qydriver.c")
    shutil.copytree(Path(__file__).parent / "git-bundle", Path(ctx.srcdir) / "git-bundle")
    shutil.copy(RECIPE_GIT_C, Path(ctx.srcdir) / "qygit.c")
    # v2.x (release 247-252 新应用)：全量复制源码，随 qydesktop 一并编译
    shutil.copy(RECIPE_CLOCK_C, Path(ctx.srcdir) / "qyclock.c")
    shutil.copy(RECIPE_WEATHER_C, Path(ctx.srcdir) / "qyweather.c")
    shutil.copy(RECIPE_NOTE_C, Path(ctx.srcdir) / "qynote.c")
    shutil.copy(RECIPE_CONVERT_C, Path(ctx.srcdir) / "qyconvert.c")
    shutil.copy(RECIPE_HELP_C, Path(ctx.srcdir) / "qyhelp.c")
    shutil.copy(RECIPE_DISK_C, Path(ctx.srcdir) / "qydisk.c")
    shutil.copy(RECIPE_AUDIO_C, Path(ctx.srcdir) / "qyaudio.c")
    shutil.copy(RECIPE_POWER_C, Path(ctx.srcdir) / "qypower.c")
    shutil.copy(RECIPE_REC_C, Path(ctx.srcdir) / "qyrec.c")
    shutil.copy(RECIPE_BACKUP_C, Path(ctx.srcdir) / "qybackup.c")
    shutil.copy(RECIPE_MIME_C, Path(ctx.srcdir) / "qymime.c")
    shutil.copy(RECIPE_COLOR_C, Path(ctx.srcdir) / "qycolor.c")
    shutil.copy(RECIPE_TODO_C, Path(ctx.srcdir) / "qytodo.c")
    shutil.copy(Path(__file__).parent / "qystore.desktop", Path(ctx.srcdir) / "qystore.desktop")
    shutil.copy(Path(__file__).parent / "qynet.unit", Path(ctx.srcdir) / "qynet.unit")
    shutil.copy(Path(__file__).parent / "start-qynet.sh", Path(ctx.srcdir) / "start-qynet.sh")
    # v1.9.6: 补入此前只在 sysroot 手工存在的脚本/单元源码，使构建可复现
    shutil.copy(Path(__file__).parent / "start-qydesktop.sh", Path(ctx.srcdir) / "start-qydesktop.sh")
    shutil.copy(Path(__file__).parent / "start-sshd.sh", Path(ctx.srcdir) / "start-sshd.sh")
    shutil.copy(Path(__file__).parent / "start-udevd.sh", Path(ctx.srcdir) / "start-udevd.sh")
    shutil.copy(Path(__file__).parent / "start-weston.sh", Path(ctx.srcdir) / "start-weston.sh")
    shutil.copy(Path(__file__).parent / "qyselftest.sh", Path(ctx.srcdir) / "qyselftest.sh")
    shutil.copy(Path(__file__).parent / "sshd.unit", Path(ctx.srcdir) / "sshd.unit")
    shutil.copy(Path(__file__).parent / "udevd.unit", Path(ctx.srcdir) / "udevd.unit")
    shutil.copy(Path(__file__).parent / "weston.unit", Path(ctx.srcdir) / "weston.unit")
    shutil.copy(Path(__file__).parent / "zz-selftest.unit", Path(ctx.srcdir) / "zz-selftest.unit")
    shutil.copy(Path(__file__).parent / "qydesktop.unit", Path(ctx.srcdir) / "qydesktop.unit")
    shutil.copy(Path(__file__).parent / "qynotifd.unit", Path(ctx.srcdir) / "qynotifd.unit")
    for _f in ("qyl10n.c", "qyl10n.h", "qysetup.c", "qysudo.c", "qyusers.c", "qywelcome.c",
               "qyuseradd.sh", "qyinstall.sh", "qyinitpw.sh", "qyboot.unit", "qysudoers"):
        shutil.copy(Path(__file__).parent / _f, Path(ctx.srcdir) / _f)
    # v2.0: 桌面主题 CSS + weston 配置（panel 禁用，顶栏由 qydesktop 提供）
    shutil.copy(Path(__file__).parent / "qytheme.css", Path(ctx.srcdir) / "qytheme.css")
    shutil.copy(Path(__file__).parent / "qytheme.c", Path(ctx.srcdir) / "qytheme.c")
    shutil.copy(Path(__file__).parent / "qytheme.h", Path(ctx.srcdir) / "qytheme.h")
    shutil.copy(Path(__file__).parent / "weston.ini", Path(ctx.srcdir) / "weston.ini")
    ctx.run(
        "export PATH=$PATH:{0}/usr/bin; export PKG_CONFIG_PATH={0}/usr/lib/pkgconfig:{0}/usr/lib/x86_64-linux-gnu/pkgconfig:{0}/usr/share/pkgconfig; "
        "export PKG_CONFIG_SYSROOT_DIR={0}; export PKG_CONFIG_LIBDIR={0}/usr/lib/pkgconfig:{0}/usr/lib/x86_64-linux-gnu/pkgconfig:{0}/usr/share/pkgconfig; "
        "export LD_LIBRARY_PATH={0}/usr/lib/x86_64-linux-gnu:{0}/usr/lib:{0}/lib; "
        "$CC qydesktop.c qyicon.c qytheme.c qyl10n.c -o qydesktop $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyfiles.c qyicon.c qytheme.c qyl10n.c -o qyfiles $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qysettings.c qyicon.c qytheme.c qyl10n.c -o qysettings $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyusers.c qyicon.c qytheme.c qyl10n.c -o qyusers $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qywelcome.c qyicon.c qytheme.c qyl10n.c -o qywelcome $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qysetup.c qyicon.c qytheme.c qyl10n.c -o qysetup $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qysudo.c -o qysudo -O2 -L{0}/usr/lib -lcrypt && "
        "$CC qyappmenu.c qyicon.c qytheme.c qyl10n.c -o qyappmenu $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyedit.c qyicon.c qytheme.c qyl10n.c -o qyedit $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qymon.c qyicon.c qytheme.c qyl10n.c -o qymon $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyview.c qyicon.c qytheme.c qyl10n.c -o qyview $(pkg-config --cflags --libs gtk+-3.0 gdk-pixbuf-2.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyctl.c -o qyctl -O2 -Wall && "
        "$CC qyarc.c qyicon.c qytheme.c qyl10n.c -o qyarc $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qystore.c qyicon.c qytheme.c qyl10n.c -o qystore $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qynet.c qyicon.c qytheme.c qyl10n.c -o qynet $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qybrowser.c qyicon.c qytheme.c qyl10n.c -o qybrowser $(pkg-config --cflags --libs gtk+-3.0) $(pkg-config --cflags --libs libcurl) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qyshot.c qyicon.c qytheme.c qyl10n.c -o qyshot $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qyshot-capture.c -o qyshot-capture -I{0}/usr/include -L{0}/usr/lib -lX11 -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qyclip.c qyicon.c qytheme.c qyl10n.c -o qyclip $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qylock.c qyicon.c qytheme.c qyl10n.c -o qylock $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qysearch.c qyicon.c qytheme.c qyl10n.c -o qysearch $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qymedia.c qyicon.c qytheme.c qyl10n.c -o qymedia $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qycalc.c qyicon.c qytheme.c qyl10n.c -o qycalc $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qysw-x11.c -o qysw-x11 -I{0}/usr/include -L{0}/usr/lib -lX11 -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qyswitcher.c qyicon.c qytheme.c qyl10n.c -o qyswitcher $(pkg-config --cflags --libs gtk+-3.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qynotify.c -o qynotify $(pkg-config --cflags --libs glib-2.0 gio-2.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qynotifd.c -o qynotifd $(pkg-config --cflags --libs glib-2.0 gio-2.0) -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 && "
        "$CC qydriver.c qyicon.c qytheme.c qyl10n.c -o qydriver $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qygit.c qyicon.c qytheme.c qyl10n.c -o qygit $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        # v2.x (release 247-252 新应用)：随 qydesktop 编译安装
        "$CC qyclock.c qyicon.c qytheme.c qyl10n.c -o qyclock $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyweather.c qyicon.c qytheme.c qyl10n.c -o qyweather $(pkg-config --cflags --libs gtk+-3.0) $(pkg-config --cflags --libs json-c libcurl) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qynote.c qyicon.c qytheme.c qyl10n.c -o qynote $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyconvert.c qyicon.c qytheme.c qyl10n.c -o qyconvert $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyhelp.c qyicon.c qytheme.c qyl10n.c -o qyhelp $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qydisk.c qyicon.c qytheme.c qyl10n.c -o qydisk $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyaudio.c qyicon.c qytheme.c qyl10n.c -o qyaudio $(pkg-config --cflags --libs gtk+-3.0) $(pkg-config --cflags --libs alsa) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qypower.c qyicon.c qytheme.c qyl10n.c -o qypower $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qyrec.c qyicon.c qytheme.c qyl10n.c -o qyrec $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qybackup.c qyicon.c qytheme.c qyl10n.c -o qybackup $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qytodo.c qyicon.c qytheme.c qyl10n.c -o qytodo $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qymime.c qyicon.c qytheme.c qyl10n.c -o qymime $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        # release 256: 新增取色器应用，随 qydesktop 编译安装
        "$CC qycolor.c qyicon.c qytheme.c qyl10n.c -o qycolor $(pkg-config --cflags --libs gtk+-3.0) -O2 -ljpeg -lmount -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) && "
        "$CC qypkg-inst.c -o qypkg-inst -O2".format(ctx.sysroot)
    )


def package(ctx):
    ctx.run("mkdir -p {}/usr/bin".format(ctx.destdir))
    ctx.run("cp qydesktop {}/usr/bin/qydesktop".format(ctx.destdir))
    ctx.run("cp qyfiles {}/usr/bin/qyfiles".format(ctx.destdir))
    ctx.run("cp qysettings {}/usr/bin/qysettings".format(ctx.destdir))
    ctx.run("cp qyappmenu {}/usr/bin/qyappmenu".format(ctx.destdir))
    ctx.run("cp qyedit {}/usr/bin/qyedit".format(ctx.destdir))
    ctx.run("cp qymon {}/usr/bin/qymon".format(ctx.destdir))
    ctx.run("cp qyview {}/usr/bin/qyview".format(ctx.destdir))
    ctx.run("cp qyctl {}/usr/bin/qyctl".format(ctx.destdir))
    ctx.run("cp qyarc {}/usr/bin/qyarc".format(ctx.destdir))
    ctx.run("cp qystore {}/usr/bin/qystore".format(ctx.destdir))
    ctx.run("cp qynet {}/usr/bin/qynet".format(ctx.destdir))
    ctx.run("cp qybrowser {}/usr/bin/qybrowser".format(ctx.destdir))
    ctx.run("cp qyshot {}/usr/bin/qyshot".format(ctx.destdir))
    ctx.run("cp qyshot-capture {}/usr/bin/qyshot-capture".format(ctx.destdir))
    ctx.run("cp qyclip {}/usr/bin/qyclip".format(ctx.destdir))
    ctx.run("cp qylock {}/usr/bin/qylock".format(ctx.destdir))
    ctx.run("cp qysearch {}/usr/bin/qysearch".format(ctx.destdir))
    ctx.run("cp qymedia {}/usr/bin/qymedia".format(ctx.destdir))
    ctx.run("cp qysw-x11 {}/usr/bin/qysw-x11".format(ctx.destdir))
    ctx.run("cp qyswitcher {}/usr/bin/qyswitcher".format(ctx.destdir))
    ctx.run("cp qynotify {}/usr/bin/qynotify".format(ctx.destdir))
    ctx.run("cp qynotifd {}/usr/bin/qynotifd".format(ctx.destdir))
    ctx.run("cp qydriver {}/usr/bin/qydriver".format(ctx.destdir))
    ctx.run("cp qygit {}/usr/bin/qygit".format(ctx.destdir))
    # v2.x (release 247-252 新应用)：全量安装
    ctx.run("cp qyclock {}/usr/bin/qyclock".format(ctx.destdir))
    ctx.run("cp qyweather {}/usr/bin/qyweather".format(ctx.destdir))
    ctx.run("cp qynote {}/usr/bin/qynote".format(ctx.destdir))
    ctx.run("cp qyconvert {}/usr/bin/qyconvert".format(ctx.destdir))
    ctx.run("cp qyhelp {}/usr/bin/qyhelp".format(ctx.destdir))
    ctx.run("cp qydisk {}/usr/bin/qydisk".format(ctx.destdir))
    ctx.run("cp qyaudio {}/usr/bin/qyaudio".format(ctx.destdir))
    ctx.run("cp qypower {}/usr/bin/qypower".format(ctx.destdir))
    ctx.run("cp qyrec {}/usr/bin/qyrec".format(ctx.destdir))
    ctx.run("cp qybackup {}/usr/bin/qybackup".format(ctx.destdir))
    ctx.run("cp qymime {}/usr/bin/qymime".format(ctx.destdir))
    ctx.run("cp qytodo {}/usr/bin/qytodo".format(ctx.destdir))
    ctx.run("cp qycolor {}/usr/bin/qycolor".format(ctx.destdir))
    ctx.run("mkdir -p {}/usr/lib/git-core {}/usr/share/git-core".format(ctx.destdir, ctx.destdir))
    if getattr(ctx, "target_arch", None) is None:
        ctx.run("cp git-bundle/git {}/usr/bin/git && chmod 0755 {}/usr/bin/git".format(ctx.destdir, ctx.destdir))
        ctx.run("cp -a git-bundle/git-core/. {}/usr/lib/git-core/".format(ctx.destdir))
        ctx.run("mkdir -p {}/usr/lib/x86_64-linux-gnu".format(ctx.destdir))
        ctx.run("cp git-bundle/lib/*.so* {}/usr/lib/x86_64-linux-gnu/".format(ctx.destdir))
    ctx.run("cp -a git-bundle/templates/. {}/usr/share/git-core/templates".format(ctx.destdir))
    ctx.run("cp qypkg-inst {}/usr/bin/qypkg-inst".format(ctx.destdir))
    ctx.run("cp qysetup {}/usr/bin/qysetup".format(ctx.destdir))
    ctx.run("cp qyuseradd.sh {}/usr/bin/qyuseradd && chmod +x {}/usr/bin/qyuseradd".format(ctx.destdir, ctx.destdir))
    ctx.run("cp qyusers {}/usr/bin/qyusers".format(ctx.destdir))
    ctx.run("cp qyinstall.sh {}/usr/bin/qyinstall && chmod +x {}/usr/bin/qyinstall".format(ctx.destdir, ctx.destdir))
    ctx.run("cp qysudo {}/usr/bin/qysudo && chmod 4755 {}/usr/bin/qysudo".format(ctx.destdir, ctx.destdir, ctx.destdir))
    ctx.install_file("qysudoers", "etc/qysudoers")
    ctx.run("mkdir -p {}/etc/qyinit.d && cp qyinitpw.sh {}/etc/qyinit.d/40-initpw && chmod +x {}/etc/qyinit.d/40-initpw".format(ctx.destdir, ctx.destdir, ctx.destdir))
    ctx.run("$CC qyboot.c -o {}/usr/bin/qyboot -Wl,-rpath-link,/home/agentuser/qiyuan-work/qiyuan-linux/var/sysroot-target/usr/lib $(pkg-config --libs epoxy wayland-client xkbcommon wayland-cursor wayland-egl fribidi pangoft2 gio-unix-2.0 pixman-1 freetype2 libpng zlib harfbuzz gmodule-2.0 libpcre2-8 mount blkid libffi fontconfig) -O2 -lm && chmod 755 {}/usr/bin/qyboot".format(ctx.destdir, ctx.destdir))
    ctx.install_file("qyboot.unit", "etc/qyinit.d/qyboot.unit")
    ctx.run("cp qywelcome {}/usr/bin/qywelcome".format(ctx.destdir))
    # 网络自启单元 (busybox udhcpc DHCP)：
    ctx.run("mkdir -p {}/etc/qyinit.d".format(ctx.destdir))
    ctx.install_file("qynet.unit", "etc/qyinit.d/qynet.unit")
    ctx.run("cp start-qynet.sh {}/usr/bin/start-qynet.sh".format(ctx.destdir))
    ctx.run("chmod 0755 {}/usr/bin/start-qynet.sh".format(ctx.destdir))
    # 软件中心 desktop entry (v1.9.5):
    ctx.run("mkdir -p {}/usr/share/applications".format(ctx.destdir))
    ctx.install_file("qystore.desktop", "usr/share/applications/qystore.desktop")
    # v2.0: 桌面主题 CSS（qydesktop 启动时加载）+ weston 配置
    ctx.run("mkdir -p {}/usr/share/themes/lanxiu/gtk-3.0".format(ctx.destdir))
    ctx.install_file("qytheme.css", "usr/share/themes/lanxiu/gtk-3.0/gtk.css")
    ctx.run("mkdir -p {}/etc/xdg/weston".format(ctx.destdir))
    ctx.install_file("weston.ini", "etc/xdg/weston/weston.ini")
    # v2.x: 桌面版本文件（qysettings 关于页显示）
    ctx.run("mkdir -p {}/usr/share".format(ctx.destdir))
    ctx.run(f"printf '{version}-{release}' > {ctx.destdir}/usr/share/qydesktop-version")
    # v1.9.6: 补入此前只在 sysroot 手工存在的启动脚本与自启单元（源码已入库）
    for _s in ("start-qydesktop.sh", "start-sshd.sh", "start-udevd.sh",
               "start-weston.sh", "qyselftest.sh"):
        ctx.run("cp {0} {1}/usr/bin/{0} && chmod 0755 {1}/usr/bin/{0}".format(_s, ctx.destdir))
    for _u in ("qydesktop.unit", "qynotifd.unit", "sshd.unit", "udevd.unit",
               "weston.unit", "zz-selftest.unit"):
        ctx.install_file(_u, "etc/qyinit.d/" + _u)