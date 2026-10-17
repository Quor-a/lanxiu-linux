"""通用包（Flatpak / Snap / AppImage）与运行时管理。

发行版自己的包格式解决不了两类需求：
1. 上游只想发一个包给所有发行版，不想为每个发行版维护配方
2. 用户想装最新版应用，不想等发行版打包

所以必须支持跨发行版通用包。但它们各有各的问题，
不加以约束就会把发行版的完整性破坏掉：

**AppImage**：单文件、加执行权限直接跑、不留残留（这是它最大的优点）。
问题是没有来源验证——任何人都能做一个同名 AppImage。
所以必须校验签名或校验和，否则"下载一个文件双击就跑"
等于把任意代码放进系统。

**Flatpack**：有运行时（runtime）概念，应用共享基础运行时，
比 Snap 省空间。权限是声明式的，可以精确控制。
问题是默认权限往往过宽——很多应用申请了 home 和 network
但其实只需要读自己的配置。

**Snap**：自带依赖，沙箱运行。问题是闭源服务端、
且自动更新无法控制（对服务器环境是灾难）。

本模块不做"安装它们"，而是：
- 登记已装的通用包，纳入统一管理（不然它们游离在包管理器之外，
  没人知道机器上装了什么）
- 校验权限声明，把过宽的申请明确指出来
- 管理运行时版本（多版本共存与切换）
- 沙箱策略生成

**为什么要把它们纳入登记**：不登记的话，安全扫描看不到它们，
"这台机器装了什么"这个问题就答不准。而答不准的清单
在出安全事件时是要命的。
"""
from __future__ import annotations

import json
import os
import stat
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class AppError(RuntimeError):
    pass


# 三种通用包
APPIMAGE = "appimage"
FLATPAK = "flatpak"
SNAP = "snap"
KINDS = (APPIMAGE, FLATPAK, SNAP)


# ---------------------------------------------------------------- 权限

# 危险权限：给了等于把机器的相应部分交出去
DANGEROUS_PERMS = {
    "filesystem=home": "可读写整个家目录，包括 SSH 私钥、浏览器密码库",
    "filesystem=host": "可读写整个文件系统",
    "system-ownership": "可修改系统目录",
    "no-sandbox": "完全不隔离，等同于直接运行",
    "docker": "可启动容器，等于拿到 root（容器逃逸即整机失守）",
    "classic": "Snap 经典模式，不做任何隔离",
    "root": "以 root 运行",
    "system-backup": "可读取系统备份",
}

# 常见但值得复核的权限
REVIEW_PERMS = {
    "network": "可访问网络（可能外传数据）",
    "camera": "可调用摄像头",
    "microphone": "可调用麦克风",
    "bluetooth": "可访问蓝牙设备",
    "removable-media": "可读写 U 盘等可移动介质",
    "gpg-keys": "可读取 GPG 私钥",
    "ssh-keys": "可读取 SSH 私钥",
    "password-manager-service": "可读取密码管理器",
}


@dataclass
class AppPerm:
    """一个权限申请。"""
    name: str
    granted: bool = True
    reason: str = ""        # 为什么需要这个权限


@dataclass
class App:
    """一个已登记的通用包。"""
    app_id: str
    name: str
    kind: str                    # appimage / flatpak / snap
    version: str = ""
    runtime: str = ""            # 依赖的运行时
    perms: list = field(default_factory=list)
    path: str = ""               # AppImage 文件路径
    sha256: str = ""             # AppImage 必须校验
    signed: bool = False
    summary: str = ""

    def dangerous(self) -> list:
        return [p for p in self.perms
                if p.granted and p.name in DANGEROUS_PERMS]

    def needs_review(self) -> list:
        return [p for p in self.perms
                if p.granted and p.name in REVIEW_PERMS]


def app_db_path(root: Path) -> Path:
    return Path(root) / "var" / "lib" / "qyapp" / "apps.json"


def load_apps(root: Path) -> list:
    p = app_db_path(root)
    if not p.exists():
        return []
    try:
        out = []
        for d in json.loads(p.read_text())["apps"]:
            perms = [AppPerm(**x) for x in d.pop("perms", [])]
            out.append(App(**d, perms=perms))
        return out
    except Exception:
        return []


def save_apps(root: Path, apps: list) -> None:
    p = app_db_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    data = {"apps": []}
    for a in apps:
        d = dict(a.__dict__)
        d["perms"] = [x.__dict__ for x in a.perms]
        data["apps"].append(d)
    util.atomic_write(p, json.dumps(data, ensure_ascii=False,
                                    indent=1).encode())


def register_app(root: Path, app: App) -> list:
    apps = load_apps(root)
    apps = [a for a in apps if a.app_id != app.app_id]
    apps.append(app)
    save_apps(root, apps)
    return apps


def perm_report(app: App) -> str:
    L = [f"{app.name}（{app.kind}）的权限："]
    for p in app.perms:
        L.append(f"  {'✓' if p.granted else '✗'} {p.name}"
                 + (f"  — {p.reason}" if p.reason else ""))
    d = app.dangerous()
    if d:
        L.append("")
        L.append(f"  ! {len(d)} 个高危权限，等于把机器相应部分交出去：")
        for p in d:
            L.append(f"    {p.name}: {DANGEROUS_PERMS[p.name]}")
    r = app.needs_review()
    if r:
        L.append("")
        L.append(f"  ? {len(r)} 个值得复核：")
        for p in r:
            L.append(f"    {p.name}: {REVIEW_PERMS[p.name]}")
    if not d and not r:
        L.append("\n  没有需要特别注意的权限。")
    return "\n".join(L)


def audit_apps(apps: list) -> list:
    """审计所有通用包的权限，返回问题。"""
    problems = []
    for a in apps:
        for p in a.dangerous():
            problems.append(f"{a.name}: 高危权限 {p.name}——"
                            f"{DANGEROUS_PERMS[p.name]}")
        if a.kind == APPIMAGE and not a.sha256 and not a.signed:
            # AppImage 最大的优点是不留残留，最大的风险是没有来源验证。
            # 一个没校验和也没签名的 AppImage 可以是任何人做的
            problems.append(
                f"{a.name}: AppImage 既无校验和也无签名——"
                f"无法确认它没被替换。下载后立刻记录 sha256")
    return problems


# ---------------------------------------------------------------- AppImage

def appimage_check(path: Path) -> dict:
    """检查一个 AppImage 文件。

    AppImage 的价值是"下载、加执行权限、双击就跑、不留残留"。
    所以这里只做静态检查，不安装、不解压、不写任何东西进系统。
    """
    p = Path(path)
    info = {"path": str(p), "exists": p.exists(),
            "executable": False, "size": 0, "sha256": "",
            "problems": []}
    if not p.exists():
        info["problems"].append("文件不存在")
        return info
    st = p.stat()
    info["size"] = st.st_size
    # 必须有执行权限，否则"双击就跑"不成立
    info["executable"] = bool(st.st_mode & stat.S_IXUSR)
    if not info["executable"]:
        info["problems"].append(
            "没有执行权限。chmod +x 后才能运行（AppImage 靠自身可执行位启动）")
    if st.st_size < 1024:
        info["problems"].append(
            f"只有 {st.st_size} 字节，不是有效的 AppImage")
    info["sha256"] = util.sha256_file(p)
    return info


def appimage_run_cmd(path: Path, extract: bool = False) -> str:
    """给出运行命令。

    默认直接执行（不留残留，这是 AppImage 的核心价值）。
    --extract 用于 FUSE 不可用的环境（容器里常见），
    它会解到临时目录——那就不是"不留残留"了，所以明确标注。
    """
    p = Path(path)
    if extract:
        return (f"{p} --appimage-extract-and-run"
                f"   # FUSE 不可用时的兜底：会解到临时目录，"
                f"退出后需自行清理")
    return f"{p}"


# ---------------------------------------------------------------- 运行时

@dataclass
class Runtime:
    """一个运行时（flatpak runtime / SDK）。

    运行时多版本共存是常态：老应用依赖旧运行时，
    升级运行时会让它们崩掉。所以不能"升级"，只能"共存 + 切换"。
    """
    name: str            # 如 org.freedesktop.Platform
    version: str         # 如 23.08
    arch: str = ""
    installed: bool = True
    size: int = 0
    used_by: list = field(default_factory=list)


def runtime_db_path(root: Path) -> Path:
    return Path(root) / "var" / "lib" / "qyapp" / "runtimes.json"


def load_runtimes(root: Path) -> list:
    p = runtime_db_path(root)
    if not p.exists():
        return []
    try:
        return [Runtime(**d) for d in json.loads(p.read_text())["runtimes"]]
    except Exception:
        return []


def save_runtimes(root: Path, rts: list) -> None:
    p = runtime_db_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps(
        {"runtimes": [r.__dict__ for r in rts]},
        ensure_ascii=False, indent=1).encode())


def runtime_versions(rts: list, name: str) -> list:
    """同一运行时的所有已装版本，按版本号降序。"""
    same = [r for r in rts if r.name == name and r.installed]
    return sorted(same, key=lambda r: _vkey(r.version), reverse=True)


def _vkey(v: str) -> tuple:
    """版本号排序键：23.08 和 24.08 要按数字比，不能按字符串。"""
    parts = []
    for seg in v.replace("-", ".").split("."):
        num = ""
        for ch in seg:
            if ch.isdigit():
                num += ch
            else:
                break
        parts.append(int(num) if num else 0)
    return tuple(parts)


def runtime_report(root: Path) -> str:
    rts = load_runtimes(root)
    if not rts:
        return "没有安装任何运行时。"
    L = ["已安装的运行时："]
    by_name: dict = {}
    for r in rts:
        by_name.setdefault(r.name, []).append(r)
    for name, vers in sorted(by_name.items()):
        vs = sorted(vers, key=lambda r: _vkey(r.version), reverse=True)
        L.append(f"  {name}")
        for r in vs:
            used = f"被 {len(r.used_by)} 个应用使用" if r.used_by else "暂无应用使用"
            L.append(f"    {r.version:<12} {r.arch:<10} {used}")
        if len(vs) > 1:
            L.append(f"    （{len(vs)} 个版本共存）")
    return "\n".join(L)


def runtime_check_removal(root: Path, name: str, version: str) -> list:
    """删除运行时前检查：还有应用在用就不能删。

    直接删掉正在被使用的运行时，那些应用会立刻起不来，
    而报错通常是"找不到运行时"，看不出是谁删的、为什么。
    """
    rts = load_runtimes(root)
    problems = []
    for r in rts:
        if r.name == name and r.version == version:
            if r.used_by:
                problems.append(
                    f"{name} {version} 仍被 {len(r.used_by)} 个应用使用："
                    f"{'、'.join(r.used_by[:5])}"
                    + ("…" if len(r.used_by) > 5 else ""))
    return problems


# ---------------------------------------------------------------- 沙箱

def sandbox_policy(app: App) -> str:
    """生成一个应用的沙箱策略（bwrap 风格）。

    只给申请过的权限。没申请的就给最小集——
    默认全给的话，权限声明就形同虚设。
    """
    L = [f"# {app.name}（{app.app_id}）的沙箱策略",
         "# 由LANXIU Linux 生成：只放行已申请的权限", "",
         "bwrap \\",
         "  --die-with-parent \\",
         "  --ro-bind /usr /usr \\",
         "  --proc /proc \\",
         "  --dev /dev \\",
         "  --tmpfs /tmp \\"]
    for p in app.perms:
        if not p.granted:
            continue
        if p.name == "network":
            L.append("  --share-net \\")
        elif p.name == "camera":
            L.append("  --dev-bind /dev/video0 /dev/video0 \\")
        elif p.name == "audio":
            L.append("  --dev-bind /dev/snd /dev/snd \\")
        elif p.name.startswith("filesystem="):
            target = p.name.split("=", 1)[1]
            if target == "home":
                L.append("  --bind $HOME $HOME \\")
            else:
                L.append(f"  --bind {target} {target} \\")
        elif p.name == "removable-media":
            L.append("  --bind /run/media /run/media \\")
    # 去掉最后一行的续行符
    if L and L[-1].endswith("\\"):
        L[-1] = L[-1][:-1].rstrip()
    L += ["", f"  {app.path or app.app_id}"]
    return "\n".join(L) + "\n"


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyapp",
                                 description="LANXIU Linux 通用包与运行时")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ap.add_argument("--root", default=".")

    sub.add_parser("list", help="列出已登记的通用包")
    sub.add_parser("audit", help="审计权限")
    sp = sub.add_parser("perms", help="查看某个应用的权限")
    sp.add_argument("app_id")
    sp = sub.add_parser("sandbox", help="生成沙箱策略")
    sp.add_argument("app_id")
    sub.add_parser("runtimes", help="列出运行时")
    sp = sub.add_parser("check-appimage", help="检查 AppImage 文件")
    sp.add_argument("path")
    sp = sub.add_parser("run", help="给出 AppImage 运行命令")
    sp.add_argument("path")
    sp.add_argument("--extract", action="store_true",
                    help="FUSE 不可用时的兜底方式")

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "list":
        apps = load_apps(root)
        if not apps:
            print("没有登记任何通用包。")
            return 0
        print("已登记的通用包：")
        for x in apps:
            d = len(x.dangerous())
            tag = f"  ! {d} 个高危权限" if d else ""
            print(f"  {x.name:<20} {x.kind:<10} {x.version:<12}{tag}")
        return 0

    if a.cmd == "audit":
        apps = load_apps(root)
        problems = audit_apps(apps)
        if problems:
            for x in problems:
                util.log("err", x)
            return 1
        util.log("ok", f"{len(apps)} 个通用包权限无高危项")
        return 0

    if a.cmd == "perms":
        for x in load_apps(root):
            if x.app_id == a.app_id:
                print(perm_report(x))
                return 0
        util.log("err", f"没有 {a.app_id}")
        return 1

    if a.cmd == "sandbox":
        for x in load_apps(root):
            if x.app_id == a.app_id:
                print(sandbox_policy(x))
                return 0
        util.log("err", f"没有 {a.app_id}")
        return 1

    if a.cmd == "runtimes":
        print(runtime_report(root))
        return 0

    if a.cmd == "check-appimage":
        info = appimage_check(Path(a.path))
        print(f"  文件: {info['path']}")
        print(f"  大小: {util.human_size(info['size'])}")
        print(f"  可执行: {'是' if info['executable'] else '否'}")
        print(f"  sha256: {info['sha256']}")
        for x in info["problems"]:
            util.log("err", x)
        return 1 if info["problems"] else 0

    if a.cmd == "run":
        print(appimage_run_cmd(Path(a.path), a.extract))
        return 0
    return 1
