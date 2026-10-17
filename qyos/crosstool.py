"""交叉编译支持（build / host / target 三元组）。

为什么必须有：给 aarch64 设备编 177 个包，如果只能在 aarch64 机器上
原生编译，就得有一台真实的 ARM 机器，而且手机类设备根本装不下编译环境。
交叉编译让 x86_64 构建机直接产出 aarch64 的包。

三个概念必须分清，混淆了就是几天的白工：

  build  = 编译器在哪台机器上运行（构建机）
  host   = 编出来的程序在哪台机器上运行（目标机）
  target = 编出来的编译器会生成哪种机器的代码（只在编编译器时有意义）

  原生编译：build = host = target
  交叉编译：build = x86_64，host = aarch64
  编交叉编译器时：build = x86_64，host = x86_64，target = aarch64
                 （编译器跑在 x86_64 上，产出 aarch64 代码）

**加拿大交叉（Canadian Cross）**：三种都不同，
在 A 机器上编一个跑在 B 上、生成 C 代码的编译器。极少需要，
但代码必须支持，否则第三种组合会被当成非法。

几个真实的坑：

1. **不能用 host 上的 pkg-config**。它会返回构建机的库路径，
   编出的包链接到 x86_64 的库，装到设备上直接"找不到共享库"。
   必须用 target 专用的 pkg-config。

2. **编译期要能跑的程序必须给 build 编译**。很多项目构建时会先编一个
   代码生成器再跑它（protobuf、wayland-scanner、glib 的 gdbus-codegen）。
   交叉编译时这个生成器是 aarch64 的，在 x86_64 上跑不了。
   处理方式：CC_FOR_BUILD 单独指向宿主编译器。

3. **autotools 的 --host 不能省略**。只给 --build 会被当成原生编译。
   而且 --host 触发交叉模式后会禁用运行测试，这正是我们要的。

4. **--target 只在编 binutils/gcc 时给**。给普通包传 --target 会让
   configure 报错或行为异常，这是个常见误用。
"""
from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class CrossError(RuntimeError):
    pass


# 已知架构的规范名。uname -m 的输出在不同系统上不统一
# （x86_64 / amd64，aarch64 / arm64），必须归一。
ARCH_ALIASES = {
    "amd64": "x86_64",
    "x86-64": "x86_64",
    "arm64": "aarch64",
    "armv8l": "aarch64",
    "armv7l": "armv7hl",
    "armv6l": "armv6hl",
    "i686": "i686",
    "i386": "i686",
    "riscv64": "riscv64",
    "loongarch64": "loongarch64",
}

# 各架构的基础三元组后缀
ARCH_TRIPLET = {
    "x86_64": "x86_64-qiyuan-linux-gnu",
    "i686": "i686-qiyuan-linux-gnu",
    "aarch64": "aarch64-qiyuan-linux-gnu",
    "armv7hl": "armv7hl-qiyuan-linux-gnueabihf",
    "armv6hl": "armv6hl-qiyuan-linux-gnueabihf",
    "riscv64": "riscv64-qiyuan-linux-gnu",
    "loongarch64": "loongarch64-qiyuan-linux-gnu",
}

# 需要 --target 的包（只有编译器/二进制工具链需要）
NEEDS_TARGET = ("binutils", "gcc", "gdb", "llvm", "clang")


def normalize_arch(name: str) -> str:
    """把各种写法归一到规范架构名。不归一的话同一台机器在不同脚本里
    会被当成两种架构，sysroot 路径对不上。"""
    return ARCH_ALIASES.get(name, name)


def triplet(arch: str) -> str:
    arch = normalize_arch(arch)
    if arch not in ARCH_TRIPLET:
        raise CrossError(
            f"未知架构 {arch}。已知：{' '.join(sorted(ARCH_TRIPLET))}")
    return ARCH_TRIPLET[arch]


@dataclass
class Triple:
    """build / host / target 三元组。"""
    build: str                     # 构建机架构
    host: str                      # 目标机架构
    target: str | None = None      # 只有编编译器时才有

    @property
    def cross(self) -> bool:
        return normalize_arch(self.build) != normalize_arch(self.host)

    @property
    def canadian(self) -> bool:
        """加拿大交叉：三个都不同。"""
        return bool(self.target) and self.target not in (
            self.build, self.host)

    @property
    def build_triplet(self) -> str:
        return triplet(self.build)

    @property
    def host_triplet(self) -> str:
        return triplet(self.host)

    @property
    def target_triplet(self) -> str | None:
        return triplet(self.target) if self.target else None

    def describe(self) -> str:
        L = [f"build  = {self.build_triplet}（编译器在哪跑）",
             f"host   = {self.host_triplet}（产物在哪跑）"]
        if self.target:
            L.append(f"target = {self.target_triplet}（产出哪种代码）")
        L.append("")
        if self.canadian:
            L.append("模式：加拿大交叉（三者都不同）")
        elif self.cross:
            L.append("模式：交叉编译")
        else:
            L.append("模式：原生编译")
        return "\n".join(L)

    def validate(self, name: str = "") -> list:
        """检查三元组组合是否合法。返回警告列表（不是错误）。

        加拿大交叉（三者都不同）本身是合法的——在 A 上编一个跑在 B 上、
        生成 C 代码的编译器。所以这里只提示，不判错。
        """
        out = []
        if not self.target:
            return out
        if self.canadian:
            # 合法但罕见：确认是编译器类包才合理
            if name and not any(n in name for n in NEEDS_TARGET):
                out.append(
                    f"加拿大交叉（target={self.target}）：只有编编译器时"
                    f"才有意义，给 {name} 传 --target 多半是误用。")
        elif self.target != self.host and name and not any(
                n in name for n in NEEDS_TARGET):
            out.append(f"{name} 不是编译器类包，传 --target 多半是误用。")
        return out


def detect_build() -> str:
    """探测构建机架构。"""
    return normalize_arch(os.uname().machine)


# ---------------------------------------------------------------- 环境

def tool_prefix(host: str) -> str:
    """交叉工具链前缀，如 aarch64-qiyuan-linux-gnu-"""
    return triplet(host) + "-"


@dataclass
class CrossEnv:
    """交叉编译所需的环境变量集合。"""
    triple_obj: Triple
    sysroot: Path | None = None      # 目标机根目录（头文件和库在这）
    build_sysroot: Path | None = None  # 构建期工具的根目录
    extra: dict = field(default_factory=dict)

    def meson_native_file(self) -> str:
        """生成 meson 构建机（native）描述文件。

        交叉构建时 meson 的 pkg-config 默认只查目标 sysroot，
        构建期代码生成工具（wayland-scanner 等）必须用宿主原生版本——
        native file 把构建机工具链和 pkg-config 显式指回宿主。
        """
        return (
            "[binaries]\n"
            "c = 'gcc'\n"
            "cpp = 'g++'\n"
            "ar = 'ar'\n"
            "strip = 'strip'\n"
            "pkgconfig = 'pkg-config'\n"
            "wayland-scanner = '/usr/bin/wayland-scanner'\n"
            # 构建机脚本解释器（生成器脚本 shebang 常裸 python3）
            "python3 = '/usr/bin/python3'\n"
            "python = '/usr/bin/python3'\n"
            # glib 构建期工具（gnome.compile_resources 等用）——宿主原生 2.84
            "glib-compile-resources = '/usr/local/bin/glib-compile-resources'\n"
            "glib-compile-schemas = '/usr/local/bin/glib-compile-schemas'\n"
            "glib-genmarshal = '/usr/local/bin/glib-genmarshal'\n"
            "glib-mkenums = '/usr/local/bin/glib-mkenums'\n"
        )

    def meson_cross_file(self) -> str:
        """生成 meson 交叉编译文件内容（配方写入 build 目录后传 --cross-file）。"""
        t = self.triple_obj
        pre = tool_prefix(t.host)
        cpu = "aarch64" if "aarch64" in t.host else "x86_64"
        lines = [
            "[binaries]",
            f"c = '{pre}gcc'",
            f"cpp = '{pre}g++'",
            f"ar = '{pre}ar'",
            f"strip = '{pre}strip'",
            # 必须绝对路径指宿主 pkgconf：配方常把 sysroot/usr/bin 前插 PATH，
            # 裸名会命中 target 的 aarch64 pkgconf（宿主无法执行）
            f"pkg-config = '{shutil.which('pkgconf') or shutil.which('pkg-config')}'",
            "",
            "[host_machine]",
            "system = 'linux'",
            f"cpu_family = '{cpu}'",
            f"cpu = '{cpu}'",
            "endian = 'little'",
            "",
            "[built-in options]",
        ]
        if self.sysroot:
            lines.append(f"c_args = ['--sysroot={self.sysroot}']")
            lines.append(f"cpp_args = ['--sysroot={self.sysroot}']")
            lines.append(f"c_link_args = ['--sysroot={self.sysroot}']")
            lines.append(f"cpp_link_args = ['--sysroot={self.sysroot}']")
            # 交叉产物宿主无法直接跑（binfmt 无对应 loader）——
            # 让 meson 用编译+运行分离的 sanity check（须在 [properties] 段）
            lines.append("")
            lines.append("[properties]")
            lines.append("needs_exe_wrapper = true")
        return "\n".join(lines) + "\n"

    def env(self) -> dict:
        t = self.triple_obj
        e = {}
        if not t.cross:
            # 原生编译：只加最基础的，不要污染
            if self.sysroot:
                e["SYSROOT"] = str(self.sysroot)
            e.update(self.extra)
            return e

        pre = tool_prefix(t.host)
        e["AR"] = pre + "ar"
        e["AS"] = pre + "as"
        e["LD"] = pre + "ld"
        e["NM"] = pre + "nm"
        e["RANLIB"] = pre + "ranlib"
        e["STRIP"] = pre + "strip"
        e["OBJCOPY"] = pre + "objcopy"
        e["OBJDUMP"] = pre + "objdump"
        e["READELF"] = pre + "readelf"
        e["CC"] = pre + "gcc"
        # 内核与 busybox 等 Kbuild 体系只认 CROSS_COMPILE（不看 CC），
        # 不注入的话它们回落到宿主 gcc，编出来的还是 x86_64 的包。
        e["CROSS_COMPILE"] = pre
        e["CXX"] = pre + "g++"
        e["CPP"] = pre + "cpp"

        # 构建期要跑的程序必须用宿主编译器编。
        # 很多项目构建时会先编一个代码生成器再执行它，
        # 交叉编译时那个生成器是目标架构的，在构建机上跑不了。
        e["CC_FOR_BUILD"] = "cc"
        e["CXX_FOR_BUILD"] = "c++"
        e["AR_FOR_BUILD"] = "ar"
        e["LD_FOR_BUILD"] = "ld"
        # GNU install -s 会调宿主 strip，剥 aarch64 二进制直接报
        # "Unable to recognise the format"。strip-program 指到目标 strip。
        e["STRIPPROG"] = pre + "strip"
        # 裸 strip 也要是目标的（ncurses 等 make install 用 install -s，
        # 内部写死调 PATH 里的 strip）。shim 目录前置到 PATH。
        e["PATH"] = "/usr/local/qycross/shim:" + os.environ.get(
            "PATH", "/usr/local/bin:/usr/bin:/bin")

        if self.sysroot:
            e["SYSROOT"] = str(self.sysroot)
            # pkg-config 必须用目标机的库路径。宿主 pkgconf + 下面这两个
            # 变量就够了：PKG_CONFIG_SYSROOT_DIR 让宿主 pkgconf 把
            # -I/-L 自动加上 sysroot 前缀，PKG_CONFIG_LIBDIR 限定它
            # 只在 sysroot 里找 .pc（顺手排除宿主库）。
            # 不要在 sysroot 里塞 shell 包装器 —— 那是构建产物目录，
            # 会被打包/回滚逻辑当成系统文件，清理时就消失了。
            host_pc = shutil.which("pkgconf") or shutil.which("pkg-config")
            if host_pc:
                e["PKG_CONFIG"] = host_pc
            e["PKG_CONFIG_SYSROOT_DIR"] = str(self.sysroot)
            e["PKG_CONFIG_LIBDIR"] = ":".join([
                str(self.sysroot / "usr" / "lib" / "pkgconfig"),
                str(self.sysroot / "usr" / "share" / "pkgconfig"),
                str(self.sysroot / "usr" / "lib64" / "pkgconfig"),
            ])
            # 不让宿主路径混进来
            e["PKG_CONFIG_ALLOW_SYSTEM_CFLAGS"] = ""
            e["PKG_CONFIG_ALLOW_SYSTEM_LIBS"] = ""
        e.update(self.extra)
        return e


def configure_args(t: Triple, sysroot: Path | None = None,
                   name: str = "") -> list:
    """生成 autotools 的 --build/--host/--target 参数。"""
    args = []
    if t.cross:
        # --build 与 --host 都要给。只给 --host 有些 configure 会猜错，
        # 只给 --build 会被当成原生编译。
        args.append(f"--build={t.build_triplet}")
        args.append(f"--host={t.host_triplet}")
    elif t.build != t.host:
        args.append(f"--host={t.host_triplet}")

    # --target 只有编译器类包需要。给普通包传会出错或行为异常，
    # 这是个常见误用。
    if t.target and (name in NEEDS_TARGET or
                     any(n in name for n in NEEDS_TARGET)):
        args.append(f"--target={t.target_triplet}")

    if sysroot:
        args.append(f"--with-sysroot={sysroot}")
    return args


def cmake_toolchain(t: Triple, sysroot: Path | None = None) -> str:
    """生成 CMake 工具链文件内容。

    CMake 不用 --host 那套，靠工具链文件描述目标平台。
    不写这个文件的话 CMake 会检测当前机器，编出 x86_64 的东西。
    """
    if not t.cross:
        return ""
    L = [
        "# LANXIU Linux CMake 交叉工具链 —— 由 qyos/crosstool.py 生成",
        "# 不指定这个文件，CMake 会按构建机检测，编出错误架构的产物",
        "",
        f"set(CMAKE_SYSTEM_NAME Linux)",
        f"set(CMAKE_SYSTEM_PROCESSOR {normalize_arch(t.host)})",
        "",
        "# 目标机根目录，头文件和库在这里找",
    ]
    if sysroot:
        L += [f"set(CMAKE_SYSROOT {sysroot})",
              "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)",
              "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)",
              "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)",
              "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)",
              ""]
    pre = tool_prefix(t.host)
    L += [f"set(CMAKE_C_COMPILER {pre}gcc)",
          f"set(CMAKE_CXX_COMPILER {pre}g++)",
          f"set(CMAKE_ASM_COMPILER {pre}gcc)",
          f"set(CMAKE_AR {pre}ar)",
          f"set(CMAKE_RANLIB {pre}ranlib)",
          f"set(CMAKE_STRIP {pre}strip)",
          f"set(CMAKE_OBJCOPY {pre}objcopy)",
          f"set(CMAKE_OBJDUMP {pre}objdump)",
          ""]
    return "\n".join(L)


def meson_cross_file(t: Triple, sysroot: Path | None = None) -> str:
    """生成 meson 交叉编译描述文件。"""
    if not t.cross:
        return ""
    pre = tool_prefix(t.host)
    L = [
        "# LANXIU Linux meson 交叉描述 —— 由 qyos/crosstool.py 生成",
        "[binaries]",
        f"c = '{pre}gcc'",
        f"cpp = '{pre}g++'",
        f"ar = '{pre}ar'",
        f"strip = '{pre}strip'",
        f"pkgconfig = '{sysroot}/usr/bin/pkg-config'" if sysroot
        else "pkgconfig = 'pkg-config'",
        # 构建机原生代码生成工具——交叉时必须用宿主版本
        # （wayland-scanner 生成协议 C 源，产物要在构建机上跑）
        "wayland-scanner = '/usr/bin/wayland-scanner'",
        "",
        "[host_machine]",
        "system = 'linux'",
        "cpu_family = '" + cpu_family(t.host) + "'",
        "cpu = '" + normalize_arch(t.host) + "'",
        "endian = 'little'",
        "",
    ]
    if sysroot:
        L += ["[properties]",
              f"sys_root = '{sysroot}'",
              # 交叉产物宿主无法直接跑（binfmt 无对应 loader）——让 meson
              # 用编译+运行分离的 sanity check，不要试图执行目标二进制
              "needs_exe_wrapper = true",
              ""]
    return "\n".join(L)


def cpu_family(arch: str) -> str:
    a = normalize_arch(arch)
    if a in ("x86_64", "i686"):
        return "x86"
    if a in ("aarch64", "arm64"):
        return "aarch64"
    if a.startswith("arm"):
        return "arm"
    if a.startswith("riscv"):
        return "riscv64"
    return a


# ---------------------------------------------------------------- 检查

def verify_arch(binary: Path, want: str) -> tuple:
    """检查一个 ELF 文件是不是目标架构。返回 (是否正确, 说明)。

    交叉编译最容易出的错是"编出来还是 x86_64 却当成 aarch64 发出去"。
    装上设备后报"格式错误"，而构建日志看起来一切正常。
    """
    want = normalize_arch(want)
    try:
        import subprocess
        out = subprocess.run(["file", "-b", str(binary)],
                             capture_output=True, text=True,
                             timeout=30).stdout
    except Exception as e:
        return False, f"无法检测: {e}"
    out = out.lower()
    marks = {
        "x86_64": ("x86-64", "x86_64"),
        "i686": ("80386", "i686", "32-bit"),
        "aarch64": ("aarch64", "arm aarch64"),
        "armv7hl": ("arm,", "eabi"),
        "riscv64": ("riscv",),
    }
    hit = [w for w in marks.get(want, (want,)) if w in out]
    if hit:
        return True, f"{binary.name}: {out.strip()}"
    return False, f"{binary.name} 不是 {want}（检测到 {out.strip()}）"


def scan_pkg_arch(pkg_path: Path, want: str) -> list:
    """扫一个包里所有 ELF，确认架构一致。"""
    from . import format as F
    import tempfile
    import subprocess
    problems = []
    pkg = F.read_package(pkg_path)
    with tempfile.TemporaryDirectory() as td:
        pkg.extract(Path(td), check_hashes=False)
        for p in Path(td).rglob("*"):
            if p.is_symlink():
                # 符号链接指向包内其它文件，架构由目标文件决定，
                # 单独判会误报（如 busybox 的 sh -> busybox）
                continue
            if not p.is_file():
                continue
            try:
                with open(p, "rb") as f:
                    if f.read(4) != b"\x7fELF":
                        continue
            except OSError:
                continue
            okk, why = verify_arch(p, want)
            if not okk:
                problems.append(why)
    return problems


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qycross",
                                 description="LANXIU Linux 交叉编译")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("info", help="显示当前三元组")
    sp.add_argument("--host", default=None, help="目标架构")
    sp.add_argument("--target", default=None, help="仅编编译器时需要")

    sp = sub.add_parser("env", help="输出交叉编译环境变量")
    sp.add_argument("--host", required=True)
    sp.add_argument("--sysroot", default=None)
    sp.add_argument("--shell", action="store_true",
                    help="输出 export 形式，可直接 source")

    sp = sub.add_parser("cmake", help="生成 CMake 工具链文件")
    sp.add_argument("--host", required=True)
    sp.add_argument("--sysroot", default=None)
    sp.add_argument("--out", default=None)

    sp = sub.add_parser("meson", help="生成 meson 交叉描述")
    sp.add_argument("--host", required=True)
    sp.add_argument("--sysroot", default=None)
    sp.add_argument("--out", default=None)

    sp = sub.add_parser("args", help="生成 autotools 参数")
    sp.add_argument("--host", required=True)
    sp.add_argument("--target", default=None)
    sp.add_argument("--sysroot", default=None)
    sp.add_argument("--name", default="", help="包名（决定要不要 --target）")

    sp = sub.add_parser("check", help="检查 ELF 是不是目标架构")
    sp.add_argument("file")
    sp.add_argument("--arch", required=True)

    a = ap.parse_args(argv)

    build = detect_build()

    if a.cmd == "info":
        t = Triple(build=build, host=a.host or build, target=a.target)
        print(t.describe())
        for p in t.validate():
            util.log("warn", p)
        return 0

    if a.cmd == "env":
        t = Triple(build=build, host=a.host)
        ce = CrossEnv(t, sysroot=Path(a.sysroot) if a.sysroot else None)
        e = ce.env()
        if a.shell:
            for k, v in sorted(e.items()):
                # 值里可能有空格或引号，必须转义
                print(f"export {k}='{v}'")
        else:
            for k, v in sorted(e.items()):
                print(f"{k}={v}")
        return 0

    if a.cmd == "cmake":
        t = Triple(build=build, host=a.host)
        text = cmake_toolchain(t, Path(a.sysroot) if a.sysroot else None)
        if a.out:
            Path(a.out).write_text(text)
            util.log("ok", f"已生成 {a.out}")
        else:
            print(text)
        return 0

    if a.cmd == "meson":
        t = Triple(build=build, host=a.host)
        text = meson_cross_file(t, Path(a.sysroot) if a.sysroot else None)
        if a.out:
            Path(a.out).write_text(text)
            util.log("ok", f"已生成 {a.out}")
        else:
            print(text)
        return 0

    if a.cmd == "args":
        t = Triple(build=build, host=a.host, target=a.target)
        print(" ".join(configure_args(
            t, Path(a.sysroot) if a.sysroot else None, a.name)))
        return 0

    if a.cmd == "check":
        okk, why = verify_arch(Path(a.file), a.arch)
        if okk:
            util.log("ok", why)
            return 0
        util.log("err", why)
        return 1
    return 1


def check_toolchain(host: str) -> list:
    """检查交叉工具链是否已就绪。返回缺失的工具列表。

    交叉编译最常见的一步卡住是"工具链还没编出来就开始编包"，
    报错信息是 gcc: command not found，看不出缺的是交叉工具链。
    """
    import shutil
    pre = tool_prefix(host)
    need = ["gcc", "g++", "ar", "as", "ld", "nm", "ranlib", "strip",
            "objcopy", "readelf"]
    return [pre + t for t in need if not shutil.which(pre + t)]
