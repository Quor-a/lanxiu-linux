"""根文件系统组装。

发行版不是一堆包的集合，是一棵有约定的目录树。这里负责：

  1. FHS 目录骨架（/bin /sbin /lib 的符号链接布局、/proc /sys /dev 挂载点）
  2. 基础 /etc：os-release、passwd、group、hosts、fstab、profile
  3. 可启动性检查：根目录缺什么、init 在哪、动态库能否解析
  4. 设备节点清单（真正的 mknod 在装机时做，这里只给清单）

符号链接布局按现代发行版的合并做法：/bin -> usr/bin，/sbin -> usr/bin，
/lib -> usr/lib。这样 /usr 是唯一真相源，便于做原子升级与快照。
"""
from __future__ import annotations

import os
import stat
from pathlib import Path

from . import util

# 目录骨架：[路径, 权限, 说明]
FHS_DIRS = [
    ("usr", 0o755, "用户空间主体"),
    ("usr/bin", 0o755, "用户命令"),
    ("usr/sbin", 0o755, "系统管理命令"),
    ("usr/lib", 0o755, "库"),
    ("usr/lib64", 0o755, "64 位库"),
    ("usr/include", 0o755, "头文件"),
    ("usr/share", 0o755, "架构无关数据"),
    ("usr/share/doc", 0o755, "文档"),
    ("usr/share/man", 0o755, "手册页"),
    ("usr/src", 0o755, "源码"),
    ("usr/local", 0o755, "本地安装"),
    ("usr/local/bin", 0o755, "本地命令"),
    ("usr/local/lib", 0o755, "本地库"),
    ("etc", 0o755, "配置文件"),
    ("etc/opt", 0o755, "附加软件配置"),
    ("var", 0o755, "可变数据"),
    ("var/log", 0o755, "日志"),
    ("var/cache", 0o755, "缓存"),
    ("var/lib", 0o755, "状态数据"),
    ("var/spool", 0o755, "队列"),
    ("var/tmp", 0o1777, "临时文件（重启保留）"),
    ("tmp", 0o1777, "临时文件"),
    ("run", 0o755, "运行时数据（tmpfs）"),
    ("home", 0o755, "用户主目录"),
    ("root", 0o700, "root 主目录"),
    ("opt", 0o755, "附加软件"),
    ("srv", 0o755, "服务数据"),
    ("boot", 0o755, "启动文件"),
    ("mnt", 0o755, "临时挂载点"),
    ("media", 0o755, "可移动介质"),
    ("dev", 0o755, "设备文件"),
    ("proc", 0o555, "内核与进程信息（虚拟）"),
    ("sys", 0o555, "内核对象（虚拟）"),
]

# /usr 合并后，这些是符号链接
MERGE_LINKS = [
    ("bin", "usr/bin"),
    ("sbin", "usr/bin"),
    ("lib", "usr/lib"),
    ("lib64", "usr/lib64"),
    ("usr/sbin", "bin") if False else ("var/run", "../run"),
]

# 启动时必须存在的设备节点（真正创建在装机/启动时做）
DEV_NODES = [
    ("dev/null", "c", 1, 3, 0o666),
    ("dev/zero", "c", 1, 5, 0o666),
    ("dev/full", "c", 1, 7, 0o666),
    ("dev/random", "c", 1, 8, 0o666),
    ("dev/urandom", "c", 1, 9, 0o666),
    ("dev/tty", "c", 5, 0, 0o666),
    ("dev/console", "c", 5, 1, 0o600),
    ("dev/kmsg", "c", 1, 11, 0o644),
    ("dev/ptmx", "c", 5, 2, 0o666),
]

OS_RELEASE = """NAME="LANXIU Linux"
VERSION="{version}"
ID=qiyuan
VERSION_ID="{version}"
PRETTY_NAME="LANXIU Linux {version}"
HOME_URL="https://example.invalid/qiyuan"
BUG_REPORT_URL="https://example.invalid/qiyuan/bugs"
"""

PASSWD = """root:x:0:0:root:/root:/bin/sh
bin:x:1:1:bin:/dev/null:/bin/false
daemon:x:6:6:daemon:/dev/null:/bin/false
nobody:x:65534:65534:nobody:/:/bin/false
"""

GROUP = """root:x:0:
bin:x:1:
daemon:x:6:
sys:x:3:
tty:x:5:
wheel:x:10:
kmem:x:15:
disk:x:6:
floppy:x:11:
uucp:x:14:
dialout:x:20:
audio:x:29:
video:x:44:
cdrom:x:24:
input:x:104:
render:x:105:
seat:x:106:
kvm:x:108:
lp:x:109:
tape:x:110:
sgx:x:111:
nogroup:x:65534:
"""

HOSTS = """127.0.0.1  localhost
::1        localhost ip6-localhost ip6-loopback
"""

FSTAB = """# <设备>  <挂载点>  <类型>  <选项>  <dump>  <pass>
# 实际装机时由安装器按分区方案改写
"""

PROFILE = """# 全系统登录 shell 环境
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
export LANG=C.UTF-8
umask 022

if [ -d /etc/profile.d ]; then
    for f in /etc/profile.d/*.sh; do
        [ -r "$f" ] && . "$f"
    done
fi
"""

INPUTRC = """set enable-keypad on
set completion-ignore-case on
"""


def create_skeleton(root: Path, version: str = "0.1") -> list:
    """在 root 下建立完整目录骨架与基础 /etc。返回创建/修改过的路径。"""
    root = Path(root)
    touched: list = []

    for rel, mode, _desc in FHS_DIRS:
        d = root / rel
        if not d.exists():
            d.mkdir(parents=True, exist_ok=True)
            touched.append(rel)
        try:
            os.chmod(d, mode)
        except OSError:
            pass

    for link, target in MERGE_LINKS:
        p = root / link
        if p.is_symlink() or p.exists():
            continue
        p.parent.mkdir(parents=True, exist_ok=True)
        p.symlink_to(target)
        touched.append(link)

    files = {
        "etc/os-release": OS_RELEASE.format(version=version),
        "etc/passwd": PASSWD,
        "etc/group": GROUP,
        "etc/hosts": HOSTS,
        "etc/fstab": FSTAB,
        "etc/profile": PROFILE,
        "etc/inputrc": INPUTRC,
        "etc/hostname": "qiyuan\n",
    }
    for rel, content in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if not p.exists():
            p.write_text(content)
            touched.append(rel)

    # /etc 下常用子目录
    for sub in ("etc/profile.d", "etc/skel", "etc/sysctl.d", "etc/modules-load.d",
                "etc/qyinit.d", "var/log/qyinit"):
        d = root / sub
        if not d.exists():
            d.mkdir(parents=True, exist_ok=True)
            touched.append(sub)

    return touched


def devnode_list() -> list:
    """返回启动前必须存在的设备节点清单。"""
    out = []
    for path, kind, major, minor, mode in DEV_NODES:
        out.append({"path": path, "type": kind, "major": major,
                    "minor": minor, "mode": mode})
    return out


def make_devnodes(root: Path) -> list:
    """真正创建设备节点。需要 root 权限；失败不中断。"""
    root = Path(root)
    made = []
    for n in devnode_list():
        p = root / n["path"]
        p.parent.mkdir(parents=True, exist_ok=True)
        if p.exists():
            continue
        try:
            os.mknod(p, 0o600 | (stat.S_IFCHR if n["type"] == "c" else stat.S_IFBLK),
                     os.makedev(n["major"], n["minor"]))
            os.chmod(p, n["mode"])
            made.append(n["path"])
        except (OSError, PermissionError):
            pass
    return made


# ---------------------------------------------------------------- 检查

def resolve_in_root(root: Path, relpath: str, depth: int = 8):
    """在 root 内部解析路径，符号链接按"root 即 /"来解析。

    不能直接 Path.exists()：包里的符号链接多是绝对路径（/usr/bin/foo），
    从宿主看它指向宿主的 /usr，永远判成不存在——可启动性检查就全错了。
    """
    root = Path(root)
    cur = relpath
    for _ in range(depth):
        full = root / cur.lstrip("/")
        if full.is_symlink():
            target = os.readlink(full)
            if os.path.isabs(target):
                cur = target
            else:
                cur = str(Path(cur).parent / target)
            continue
        return full if full.exists() else None
    return None


def boot_check(root: Path) -> dict:
    """可启动性检查。装机后、打包镜像前必跑一遍。

    返回 {ok: [..], warn: [..], error: [..]}
    """
    root = Path(root)
    ok, warn, error = [], [], []

    # 1. init 必须存在且可执行（在 root 内解析符号链接）
    init = None
    for cand in ("sbin/init", "usr/sbin/init", "bin/init", "init"):
        real = resolve_in_root(root, cand)
        if real is not None and os.access(real, os.X_OK):
            init = cand
            break
    if init:
        ok.append(f"init 存在且可执行: /{init}")
    else:
        error.append("找不到可执行的 init（/sbin/init 或 /usr/sbin/init）")

    # 2. 关键目录
    for d in ("usr/bin", "usr/lib", "etc", "var", "dev", "proc", "sys"):
        if (root / d).is_dir():
            ok.append(f"目录存在: /{d}")
        else:
            error.append(f"缺少关键目录 /{d}")

    # 3. /bin /sbin 符号链接（usr 合并布局）
    for link, target in (("bin", "usr/bin"), ("sbin", "usr/bin"), ("lib", "usr/lib")):
        p = root / link
        if p.is_symlink():
            ok.append(f"/{link} -> {target}")
        elif p.is_dir():
            warn.append(f"/{link} 是目录而非指向 {target} 的符号链接，"
                        f"usr 合并布局未生效")
        else:
            warn.append(f"/{link} 不存在")

    # 4. 身份库
    for f in ("etc/passwd", "etc/group"):
        if (root / f).is_file():
            ok.append(f"/{f} 存在")
        else:
            error.append(f"缺少 /{f}")

    # 5. ELF 解释器：没有它所有动态程序都跑不起来
    interp = _find_interp(root)
    if interp:
        ok.append(f"存在动态解释器: {interp}")
    else:
        warn.append("未找到 ELF 解释器（ld-linux），动态程序将无法运行")

    # 6. 必要的动态库
    missing_libs = _missing_libs(root)
    if missing_libs:
        warn.append("以下程序依赖的库在根内缺失: " + ", ".join(sorted(missing_libs)[:8]))
    else:
        ok.append("已安装程序的动态依赖可在根内解析")

    # 7. 临时目录权限
    # 有些文件系统（容器里的 virtiofs/9p、部分 CI 环境）根本不保存权限位，
    # 这时报"权限不对"是误导——先探测一下再判断
    chmod_works = _chmod_supported(root)
    for d, want in (("tmp", 0o1777), ("var/tmp", 0o1777)):
        p = root / d
        if p.is_dir() and (p.stat().st_mode & 0o7777) == want:
            ok.append(f"/{d} 权限正确 (1777)")
        elif not chmod_works:
            warn.append(f"/{d} 权限无法验证：该文件系统不保存权限位"
                        f"（装机到真实磁盘后需复查）")
        elif p.is_dir():
            warn.append(f"/{d} 权限应为 1777，实际 "
                        f"{oct(p.stat().st_mode & 0o7777)}")

    return {"ok": ok, "warn": warn, "error": error}


def _chmod_supported(root: Path) -> bool:
    """探测该文件系统是否真的保存权限位。"""
    import tempfile
    try:
        with tempfile.TemporaryDirectory(dir=str(root)) as td:
            p = Path(td) / ".chmod-probe"
            p.mkdir()
            os.chmod(p, 0o700)
            return (p.stat().st_mode & 0o7777) == 0o700
    except Exception:
        return False


def _find_interp(root: Path):
    for d in ("usr/lib", "lib", "usr/lib64", "lib64"):
        base = root / d
        if not base.is_dir():
            continue
        for p in base.glob("ld-linux*so*"):
            if p.is_file() or p.is_symlink():
                return str(p.relative_to(root))
    return None


def _missing_libs(root: Path, limit: int = 200) -> set:
    """检查已安装 ELF 的动态依赖是否都能在根内找到。"""
    import subprocess
    have: set = set()
    for d in ("usr/lib", "lib", "usr/lib64", "lib64", "usr/local/lib"):
        base = root / d
        if base.is_dir():
            for p in base.rglob("*.so*"):
                have.add(p.name)
    missing: set = set()
    n = 0
    for d in ("usr/bin", "bin", "usr/sbin", "sbin"):
        base = root / d
        if not base.is_dir():
            continue
        for p in sorted(base.iterdir()):
            if n >= limit:
                break
            if not p.is_file() or p.is_symlink():
                continue
            try:
                with open(p, "rb") as f:
                    if f.read(4) != b"\x7fELF":
                        continue
            except OSError:
                continue
            n += 1
            try:
                out = subprocess.run(["objdump", "-p", str(p)],
                                     capture_output=True, text=True,
                                     timeout=10).stdout
            except Exception:
                continue
            for line in out.splitlines():
                if "NEEDED" in line:
                    lib = line.split()[-1]
                    if lib not in have:
                        missing.add(lib)
    return missing


def report_boot_check(res: dict) -> str:
    lines = []
    for level, label in (("error", "致命"), ("warn", "警告"), ("ok", "通过")):
        items = res[level]
        if not items:
            continue
        lines.append(f"{label} {len(items)} 项：")
        for i in items:
            lines.append(f"  - {i}")
    if not res["error"] and not res["warn"]:
        lines.append("可启动性检查全部通过")
    return "\n".join(lines)
