"""mobile-data —— 移动数据网络（蜂窝）元包

整合 NetworkManager + ofono 栈的移动网络接入配置。
真机上还需要设备专用 RIL/基带驱动，这里只装通用用户空间。

许可证：MIT
"""

name = "mobile-data"
version = "1.0.0"
release = 1
summary = "移动数据网络（蜂窝）元包：NetworkManager 调制解调器接入"
license = "MIT"

source = []
sha256 = []

depends = ["networkmanager", "libgudev", "dbus"]
makedepends = ["networkmanager", "dbus"]
provides = ["mobile-data"]

requires_build_machine = True
network = False
compression = "gz"


def build(ctx):
    ctx.run("true")


def package(ctx):
    ctx.run("mkdir -p {}/etc/qyinit.d {}/usr/share/qiyuan".format(ctx.destdir))
    # 移动网络接入的 NetworkManager 连接模板（APN 由用户或运营商配置填充）
    ctx.write("{}/usr/share/qiyuan/mobile-connection.nmconnection".format(
        ctx.destdir), """\
# 由澜岫 Linux mobile-data 元包提供的蜂窝连接模板。
# 复制到 /etc/NetworkManager/system-connections/ 并填入你的 APN。
[connection]
id=gsm-auto
type=gsm
autoconnect=true

[gsm]
number=*99#
# apn=
# password-flags=2
""")
    ctx.write("{}/etc/qyinit.d/40-mobile-data.sh".format(ctx.destdir), """\
#!/bin/sh
# 移动数据初始化：等待基带就绪后激活 gsm 连接。
# 失败不阻塞启动（无 SIM/无基带的设备很常见）。
[ -x /usr/bin/nmcli ] || exit 0
( sleep 15 && nmcli connection up gsm-auto ) >/dev/null 2>&1 &
exit 0
""")
    ctx.run("chmod 755 {}/etc/qyinit.d/40-mobile-data.sh".format(ctx.destdir))
