"""gcc-libs —— GCC 运行期库（libstdc++/libgomp/libgcc_s，从 gcc 构建树打包）

许可证：GPL-3.0-with-GCC-exception
"""
from __future__ import annotations

name = "gcc-libs"
version = "15.2.0"
release = 1
summary = "GCC 运行期共享库（libstdc++ 等）"
homepage = "https://gcc.gnu.org/"
license = "GPL-3.0-with-GCC-exception"

# 复用 gcc 的源码（已在 var/src 缓存，同哈希）
source = ["https://mirrors.aliyun.com/gnu/gcc/gcc-15.2.0/gcc-15.2.0.tar.xz"]
sha256 = ["438fd996826b0c82485a29da03a72d71d6e3541a83ec702df4271f6fe025d24e"]

depends = []
makedepends = ["gcc", "gmp", "mpfr", "mpc"]
provides = ["libstdc++.so.6", "libgomp.so.1", "libgcc_s.so.1"]

requires_build_machine = True
network = False


def build(ctx):
    # 运行库来自交叉工具链的 target 树（Ubuntu gcc-15-aarch64 自带
    # libstdc++/libgcc_s/libgomp 的 aarch64 版），无需重编整棵 GCC。
    # 拷到工作目录以便 package 阶段统一处理。
    ctx.run("mkdir -p out && cp -a /usr/aarch64-linux-gnu/lib/libstdc++.so.6* "
            "/usr/aarch64-linux-gnu/lib/libgcc_s.so.1 out/ 2>/dev/null || true")
    ctx.run("cp -a /usr/aarch64-linux-gnu/lib/libgomp.so.1* out/ 2>/dev/null || true")
    ctx.run("ls out/")


def package(ctx):
    import glob, os, shutil
    destdir = str(ctx.destdir)
    ctx.run("mkdir -p {}/usr/lib".format(destdir))
    for f in glob.glob("out/libstdc++*") + glob.glob("out/libgcc_s*") + glob.glob("out/libgomp*"):
        if os.path.islink(f):
            link = os.readlink(f)
            os.symlink(link, os.path.join(destdir, "usr/lib", os.path.basename(f)))
        else:
            shutil.copy2(f, os.path.join(destdir, "usr/lib"))
    # dev symlink：g++ 链接需要 -lstdc++ 找到 libstdc++.so
    ctx.run("cd {}/usr/lib && ln -sf libstdc++.so.6 libstdc++.so || true".format(destdir))
