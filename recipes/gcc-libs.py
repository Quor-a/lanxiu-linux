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
    # 只构建 target 库（libstdc++-v3 / libgomp / libgcc）
    ctx.run("mkdir -p build && cd build && ../configure --prefix=/usr "
            "--host=aarch64-qiyuan-linux-gnu "
            "--disable-multilib --enable-languages=c,c++ "
            "--enable-shared --disable-bootstrap "
            "--with-sysroot={root} --with-gxx-include-dir=/usr/include/c++/15.2.0 "
            "--disable-nls --enable-linker-build-id".format(root=str(ctx.sysroot)))
    ctx.run("cd build && make -j2")


def package(ctx):
    import glob, os, shutil
    destdir = str(ctx.destdir)
    # 从 gcc 已安装的系统树与构建树收集运行库
    for libdir in ['usr/lib', 'usr/lib64']:
        src = os.path.join('build', 'x86_64-pc-linux-gnu', libdir.split('/')[-1])
        if os.path.isdir(src):
            for f in glob.glob(src + '/.libs/lib*.so*'):
                shutil.copy(f, os.path.join(destdir, 'usr/lib'))
    ctx.run("mkdir -p {}/usr/lib".format(destdir))
    # configure 生成的库
    ctx.run("find build -name 'libstdc++.so.6*' -o -name 'libgomp.so.1*' -o -name 'libgcc_s.so.1*' -o -name 'libitm.so.1*' | while read f; do cp -P \"$f\" {}/usr/lib/ 2>/dev/null || true; done".format(destdir))
    # dev symlink：g++ 链接需要 -lstdc++ 找到 libstdc++.so
    ctx.run("cd {}/usr/lib && ln -sf libstdc++.so.6 libstdc++.so || true".format(destdir))
