"""mpc —— 复数任意精度算术库（GCC 构建依赖）

https://www.multiprecision.org
许可证：LGPL-3.0-or-later
"""

name = "mpc"
version = "1.3.1"
release = 1
summary = "复数任意精度算术库"
homepage = "https://www.multiprecision.org"
license = "LGPL-3.0-or-later"

source = ["https://mirrors.aliyun.com/gnu/mpc/mpc-1.3.1.tar.gz"]
sha256 = ["ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8"]

depends = ["gmp", "mpfr"]


def build(ctx):
    # 显式把 sysroot 头文件与库目录交给 configure（沙箱有时不自动注入依赖包的 CPATH）
    env = "CFLAGS=-I{0}/usr/include LDFLAGS=-L{0}/usr/lib".format(ctx.sysroot)
    ctx.run("./configure " + " ".join(ctx.configure_args()) + " --prefix=/usr --disable-static " + env)
    ctx.run("make")


def package(ctx):
    ctx.run("make DESTDIR={} install".format(ctx.destdir))
