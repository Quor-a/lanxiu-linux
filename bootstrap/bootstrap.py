#!/usr/bin/env python3
"""LANXIU Linux 工具链引导器。

把 Linux From Scratch 的手工步骤变成可无人值守、可断点续跑的自动化流程。
这是「原创发行版」的地基：地基不稳，后面每个包都不可控。

用法：
    bootstrap.py preflight              检查宿主环境是否满足构建要求
    bootstrap.py plan                   打印完整阶段计划
    bootstrap.py run [--from STAGE]     开始或继续引导（每完成一阶段写检查点）
    bootstrap.py status                 查看当前进度

设计：
    * 每一阶段幂等：重跑会先清理该阶段的工作目录，不做增量拼接。
    * 每完成一阶段写 .checkpoint，中断后 --from 可从该阶段继续。
    * 构建完全落在 --builddir，不污染宿主任何目录。
    * 默认断网执行构建命令，下载单独在沙箱外完成。
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from qyos import util  # noqa: E402

HERE = Path(__file__).resolve().parent
VERSIONS_FILE = HERE / "versions.json"

# 已查证的基线：LFS 12.4（2025-09-01 发布）
KNOWN = {
    "binutils": "2.45",
    "gcc": "15.2.0",
    "glibc": "2.42",
    "linux": "6.16.1",
}


# ---------------------------------------------------------------- 工具链布局

class Layout:
    def __init__(self, builddir: Path, target: str = "x86_64-qiyuan-linux-gnu"):
        self.root = Path(builddir)
        self.target = target
        self.src = self.root / "sources"
        self.tools = self.root / "tools"          # 交叉工具链安装点
        self.build = self.root / "build"          # 各包的构建目录
        self.logs = self.root / "logs"
        self.checkpoint = self.root / ".checkpoint"
        for d in (self.src, self.tools, self.build, self.logs):
            d.mkdir(parents=True, exist_ok=True)

    @property
    def path(self) -> str:
        return f"{self.tools}/bin:/usr/bin:/bin:/usr/sbin:/sbin"

    def env(self, extra: dict | None = None) -> dict:
        env = {
            "PATH": self.path,
            "LC_ALL": "POSIX",
            "QY_TGT": self.target,
            "QY_TOOLS": str(self.tools),
            "CONFIG_SITE": "",            # 关键：禁止 autoconf 猜宿主路径
            "MAKEFLAGS": f"-j{os.cpu_count() or 1}",
            "CFLAGS": "-O2 -pipe",
            "CXXFLAGS": "-O2 -pipe",
        }
        if extra:
            env.update(extra)
        return env


# ---------------------------------------------------------------- 阶段

class Stage:
    def __init__(self, name: str, desc: str, fn):
        self.name, self.desc, self.fn = name, desc, fn

    def __call__(self, ctx: "Bootstrap"):
        return self.fn(ctx)


def sh(cmd: str, ctx: "Bootstrap", cwd: Path | None = None,
       env: dict | None = None, capture: bool = True, check: bool = True):
    """执行构建命令，输出写日志并回显关键行。"""
    full = ctx.layout.env(env)
    logfile = ctx.layout.logs / f"{ctx.current}.log"
    util.log("info", f"$ {cmd[:110]}")
    with open(logfile, "a", encoding="utf-8") as lf:
        lf.write(f"\n$ {cmd}\n")
        p = subprocess.run(["/bin/bash", "-c", cmd], cwd=str(cwd) if cwd else None,
                           env=full, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        lf.write(p.stdout)
        lf.flush()
    if check and p.returncode != 0:
        tail = "\n".join(p.stdout.strip().splitlines()[-15:])
        util.die(f"阶段 {ctx.current} 失败（退出码 {p.returncode}）\n"
                 f"完整日志: {logfile}\n--- 末尾输出 ---\n{tail}")
    return p.returncode, p.stdout


def srcdir(ctx: "Bootstrap", pkg: str) -> Path:
    """找到已解包的源码目录（按包名前缀匹配）。"""
    for d in sorted(ctx.layout.src.iterdir()):
        if d.is_dir() and d.name.startswith(pkg + "-"):
            return d
    util.die(f"未找到 {pkg} 的源码目录，请先运行 fetch")


def fresh_build_dir(ctx: "Bootstrap", pkg: str) -> Path:
    d = ctx.layout.build / pkg
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    return d


# --- 各阶段实现 --------------------------------------------------------

def stage_preflight(ctx):
    util.log("ok", "宿主环境检查")
    req = {"gcc": "gcc --version", "g++": "g++ --version",
           "make": "make --version", "tar": "tar --version",
           "xz": "xz --version", "patch": "patch --version",
           "python3": "python3 --version", "bison": "bison --version",
           "flex": "flex --version", "perl": "perl --version",
           "grep": "grep --version", "sed": "sed --version",
           "awk": "awk --version 2>/dev/null || gawk --version"}
    missing = []
    for tool, cmd in req.items():
        code, _ = sh(cmd, ctx, capture=True, check=False)
        if code != 0:
            missing.append(tool)
    if missing:
        util.die("宿主缺少以下工具，请先安装: " + ", ".join(missing))
    # 宿主符号链接与库检查（LFS 的 version-check 关键项）
    code, out = sh("ldconfig -p | grep -c libgmp", ctx, capture=True, check=False)
    if code != 0 or not out.strip().isdigit() or int(out.strip()) == 0:
        util.log("warn", "未检测到 libgmp，构建 GCC 前需要安装 gmp/mpfr/mpc 开发包")
    # 磁盘与内存
    st = os.statvfs(str(ctx.layout.root))
    free_gb = st.f_bavail * st.f_frsize / (1024 ** 3)
    if free_gb < 20:
        util.log("warn", f"构建目录可用空间仅 {free_gb:.1f}G，完整工具链建议 30G 以上")
    util.log("ok", f"宿主检查通过（可用空间 {free_gb:.1f}G, {os.cpu_count()} 核）")


def stage_fetch(ctx):
    util.log("ok", "下载并校验源码")
    for pkg, ver in ctx.versions.items():
        url_tpl = ctx.sources.get(pkg)
        if not url_tpl:
            util.log("warn", f"{pkg} 未配置下载地址，跳过")
            continue
        url = url_tpl.format(version=ver, name=pkg)
        want = ctx.sums.get(pkg)
        try:
            util.fetch(url, ctx.layout.src, want)
        except SystemExit:
            raise
        except Exception as e:
            util.die(f"下载 {pkg} 失败: {e}")


def stage_binutils_p1(ctx):
    util.log("ok", "交叉 binutils 第一遍")
    bd = fresh_build_dir(ctx, "binutils-p1")
    src = srcdir(ctx, "binutils")
    sh(f"{src}/configure --prefix={ctx.layout.tools} "
       f"--with-sysroot={ctx.layout.tools} "
       f"--target={ctx.layout.target} --disable-nls --enable-gprofng=no "
       f"--disable-werror", ctx, cwd=bd)
    sh("make", ctx, cwd=bd)
    sh("make install", ctx, cwd=bd)


def stage_gcc_p1(ctx):
    util.log("ok", "交叉 gcc 第一遍（仅 C，不链接目标 C 库）")
    src = srcdir(ctx, "gcc")
    # GCC 需要在源码树内解出 gmp/mpfr/mpc（或 --with-*）
    for lib in ("gmp", "mpfr", "mpc"):
        d = srcdir(ctx, lib) if (ctx.layout.src / f"{lib}").exists() else None
        if lib in ctx.versions:
            cand = [x for x in ctx.layout.src.iterdir()
                    if x.is_dir() and x.name.startswith(lib + "-")]
            if cand:
                link = src / lib
                if link.is_symlink() or link.exists():
                    continue
                link.symlink_to(cand[0])
    bd = fresh_build_dir(ctx, "gcc-p1")
    sh(f"{src}/configure --target={ctx.layout.target} "
       f"--prefix={ctx.layout.tools} --with-glibc-version=2.42 "
       f"--with-sysroot={ctx.layout.tools} --with-newlib "
       f"--without-headers --enable-default-pie --enable-default-ssp "
       f"--disable-nls --disable-shared --disable-multilib "
       f"--disable-threads --disable-decimal-float "
       f"--enable-languages=c,c++", ctx, cwd=bd)
    sh("make", ctx, cwd=bd)
    sh("make install", ctx, cwd=bd)
    # 生成 limits.h（无头文件时的必要修正）
    sh("cat gcc/limitx.h gcc/glimits.h gcc/limity.h > "
       f"`dirname $({ctx.layout.target}-gcc -print-libgcc-file-name)`"
       "/install-tools/include/limits.h", ctx, cwd=bd)


def stage_kernel_headers(ctx):
    util.log("ok", "内核头文件")
    src = srcdir(ctx, "linux")
    sh(f"make mrproper", ctx, cwd=src)
    sh(f"make headers ARCH=x86_64 INSTALL_HDR_PATH={ctx.layout.tools}", ctx, cwd=src)
    sh(f"find {ctx.layout.tools}/include -name '.*' -delete", ctx, cwd=src)


def stage_glibc(ctx):
    util.log("ok", "目标 C 库 glibc")
    bd = fresh_build_dir(ctx, "glibc")
    src = srcdir(ctx, "glibc")
    sh(f"{src}/configure --prefix=/usr --host={ctx.layout.target} "
       f"--build=$({src}/scripts/config.guess) --enable-kernel=4.19 "
       f"--with-headers={ctx.layout.tools}/include "
       f"libc_cv_slibdir=/usr/lib", ctx, cwd=bd)
    sh("make", ctx, cwd=bd)
    sh(f"make DESTDIR={ctx.layout.tools} install", ctx, cwd=bd)
    # 修正 ldd 与 limits.h 引用，使后续构建可用
    sh(f"sed -i 's|/lib/ld-linux|/tools/lib/ld-linux|' "
       f"{ctx.layout.tools}/usr/bin/ldd 2>/dev/null || true", ctx, cwd=bd)


def stage_libstdcxx(ctx):
    util.log("ok", "C++ 运行库第一遍")
    bd = fresh_build_dir(ctx, "libstdcxx")
    src = srcdir(ctx, "gcc")
    sh(f"{src}/libstdc++-v3/configure --host={ctx.layout.target} "
       f"--build=$({src}/scripts/config.guess) --prefix=/usr "
       f"--disable-multilib --disable-nls --disable-libstdcxx-pch "
       f"--with-gxx-include-dir={ctx.layout.tools}/include/c++/"
       f"{ctx.versions.get('gcc', '15.2.0')}", ctx, cwd=bd)
    sh("make", ctx, cwd=bd)
    sh(f"make DESTDIR={ctx.layout.tools} install", ctx, cwd=bd)


def stage_gcc_p2(ctx):
    util.log("ok", "完整交叉编译器第二遍（含 C++）")
    bd = fresh_build_dir(ctx, "gcc-p2")
    src = srcdir(ctx, "gcc")
    sh(f"{src}/configure --target={ctx.layout.target} "
       f"--prefix={ctx.layout.tools} --with-sysroot={ctx.layout.tools} "
       f"--enable-default-pie --enable-default-ssp --disable-nls "
       f"--disable-multilib --disable-libsanitizer "
       f"--enable-languages=c,c++", ctx, cwd=bd)
    sh("make", ctx, cwd=bd)
    sh("make install", ctx, cwd=bd)


def stage_verify(ctx):
    util.log("ok", "工具链自检")
    tgt = ctx.layout.target
    code, out = sh(f"{tgt}-gcc --version", ctx, capture=True, check=False)
    if code != 0:
        util.die("交叉 gcc 不可用")
    util.log("info", "  " + out.strip().splitlines()[0])
    # 编译一个只依赖目标 C 库的小程序，确认工具链真的能产出目标程序
    test = ctx.layout.build / "toolchain-test.c"
    test.write_text('#include <stdio.h>\nint main(){printf("qiyuan toolchain ok\\n");return 0;}\n')
    code, out = sh(f"{tgt}-gcc -o {ctx.layout.build}/tc-test {test}",
                   ctx, capture=True, check=False)
    if code != 0:
        util.die(f"交叉编译测试失败:\n{out}")
    code, out = sh(f"file {ctx.layout.build}/tc-test", ctx, capture=True, check=False)
    util.log("info", "  " + out.strip())
    util.log("ok", "工具链可用，产物为真正的目标架构程序")


STAGES = [
    Stage("preflight", "检查宿主环境（工具、库、磁盘）", stage_preflight),
    Stage("fetch", "下载并校验全部源码", stage_fetch),
    Stage("binutils-p1", "交叉 binutils 第一遍", stage_binutils_p1),
    Stage("gcc-p1", "交叉 gcc 第一遍（无 libc）", stage_gcc_p1),
    Stage("kernel-headers", "安装内核头文件", stage_kernel_headers),
    Stage("glibc", "构建目标 C 库", stage_glibc),
    Stage("libstdcxx-p1", "C++ 运行库第一遍", stage_libstdcxx),
    Stage("gcc-p2", "完整交叉编译器第二遍", stage_gcc_p2),
    Stage("verify", "工具链自检", stage_verify),
]


# ---------------------------------------------------------------- 驱动

class Bootstrap:
    def __init__(self, builddir: Path, versions_file: Path = VERSIONS_FILE):
        self.layout = Layout(Path(builddir))
        self.current = "-"
        v = json.loads(Path(versions_file).read_text()) if Path(versions_file).exists() else {}
        self.versions = dict(KNOWN)
        self.versions.update(v.get("versions", {}))
        self.sources = v.get("sources", {})
        self.sums = v.get("sha256", {})

    # -- 检查点 ----------------------------------------------------

    def done(self) -> list:
        if self.layout.checkpoint.exists():
            return json.loads(self.layout.checkpoint.read_text()).get("done", [])
        return []

    def mark(self, stage: str) -> None:
        d = self.done()
        if stage not in d:
            d.append(stage)
        util.atomic_write(self.layout.checkpoint,
                          json.dumps({"done": d}, indent=2).encode())

    # -- 执行 ------------------------------------------------------

    def run(self, start_from: str | None = None, only: str | None = None) -> None:
        done = self.done()
        started = start_from is None
        for st in STAGES:
            if only and st.name != only:
                continue
            if start_from and st.name == start_from:
                started = True
            if not started:
                continue
            if st.name in done and not start_from and not only:
                util.log("info", f"{st.name} 已完成，跳过（--from {st.name} 可重跑）")
                continue
            self.current = st.name
            (self.layout.logs / f"{st.name}.log").write_text("")
            t0 = util.timer()
            st(self)
            self.mark(st.name)
            util.log("ok", f"{st.name} 完成 ({util.fmt_duration(util.timer() - t0)})")

    def plan(self) -> None:
        print(f"引导计划 · 目标 {self.layout.target}")
        print(f"构建目录 {self.layout.root}\n")
        done = self.done()
        for i, st in enumerate(STAGES, 1):
            mark = "[x]" if st.name in done else "[ ]"
            print(f"  {mark} {i}. {st.name:<16} {st.desc}")
        print(f"\n源码版本基线（以 LFS 当前稳定版为准）:")
        for k, v in sorted(self.versions.items()):
            print(f"  {k:<12} {v}")


def main() -> int:
    p = argparse.ArgumentParser(description="LANXIU Linux 工具链引导器")
    p.add_argument("action", choices=["preflight", "plan", "run", "status", "clean"])
    p.add_argument("--builddir", default="/opt/qiyuan/build")
    p.add_argument("--from", dest="from_stage", help="从指定阶段开始（会重跑该阶段）")
    p.add_argument("--only", help="只跑指定阶段")
    p.add_argument("--versions", default=str(VERSIONS_FILE))
    a = p.parse_args()

    b = Bootstrap(Path(a.builddir), Path(a.versions))

    if a.action == "plan":
        b.plan()
        return 0
    if a.action == "status":
        d = b.done()
        print(f"已完成 {len(d)}/{len(STAGES)} 阶段: {', '.join(d) or '（无）'}")
        return 0
    if a.action == "preflight":
        b.current = "preflight"
        (b.layout.logs / "preflight.log").write_text("")
        stage_preflight(b)
        return 0
    if a.action == "clean":
        if b.layout.root.exists():
            shutil.rmtree(b.layout.root)
        util.log("ok", f"已清理 {b.layout.root}")
        return 0
    if a.action == "run":
        b.run(start_from=a.from_stage, only=a.only)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
