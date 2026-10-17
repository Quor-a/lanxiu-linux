"""进程与资源：进程管理、CPU、显卡、包使用时间、内置系统包、工具调用登记。

**进程管理**
- 结束进程要区分"请求退出"和"强制杀"。
  强制杀不给程序保存数据的机会，
  用户会丢掉正在编辑的内容且不知道为什么
- 系统关键进程必须挡住。杀掉 init 会直接死机，
  而用户只是想"关掉卡住的东西"

**CPU**
- 调频策略（governor）默认是 powersave 还是 performance
  决定了体感速度。用户觉得"机器慢"常常是这个
- 核心数与在线核心数不同：核心被离线了会显示
  比实际少，用户以为 CPU 坏了

**显卡**
- 双显卡（核显+独显）切换时若没切干净，
  会出现"屏幕黑但系统还活着"
- 没有硬件加速时浏览器视频会掉帧且 CPU 占满

**包使用时间**
- 记录包的安装时间和最后使用时间。
  没有这个就无法知道哪些包是装了一次再没用过的，
  也就没法清理——系统会越来越臃肿

**内置系统包**
- 必须明确哪些包不可卸载。用户误删基础包
  会让系统起不来，而包管理器如果允许删就是失职

**工具调用**
- 系统里有 20 多个命令行工具，没有统一登记
  就没人知道有哪些、怎么调。
  这一层把工具登记成可查询的表
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class ProcError(RuntimeError):
    pass


# ---------------------------------------------------------------- 进程

# 不可杀的关键进程。杀掉 init 会直接死机
CRITICAL_PROCS = {"qyinit", "init", "systemd", "udevd", "dbus-daemon"}


@dataclass
class Proc:
    pid: int
    name: str
    cpu: float = 0.0
    mem_kb: int = 0


def list_procs() -> list:
    out = []
    p = Path("/proc")
    if not p.exists():
        return out
    for d in p.iterdir():
        if not d.name.isdigit():
            continue
        try:
            stat = (d / "stat").read_text()
            name = stat[stat.index("(") + 1:stat.rindex(")")]
            parts = stat[stat.rindex(")") + 2:].split()
            utime, stime = int(parts[11]), int(parts[12])
            out.append(Proc(int(d.name), name,
                            float(utime + stime), 0))
        except (OSError, ValueError, IndexError):
            continue
    return sorted(out, key=lambda x: x.pid)


def kill_check(name_or_pid, force: bool = False) -> list:
    """结束进程前的检查。"""
    problems = []
    # 按名字判断关键进程
    key = str(name_or_pid)
    if key in CRITICAL_PROCS or (
            key.isdigit() and _proc_name(int(key)) in CRITICAL_PROCS):
        problems.append(
            f"{key} 是系统关键进程——杀掉会直接死机，"
            f"而用户往往只是想'关掉卡住的东西'。"
            f"不允许结束")
        return problems
    if force:
        problems.append(
            "强制结束（SIGKILL）不给程序保存数据的机会——"
            "用户会丢掉正在编辑的内容，且不知道为什么。"
            "先尝试普通结束，不行再强制")
    return problems


def _proc_name(pid: int) -> str:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        return stat[stat.index("(") + 1:stat.rindex(")")]
    except Exception:
        return ""


def kill_cmd(pid: int, force: bool = False) -> str:
    return f"kill -{'9' if force else 'TERM'} {pid}"


def top_procs(n: int = 10) -> str:
    ps = list_procs()
    if not ps:
        return "读不到进程（容器里可能只有有限视图）"
    L = [f"进程（共 {len(ps)} 个）：", ""]
    for p in ps[:n]:
        mark = "!" if p.name in CRITICAL_PROCS else " "
        L.append(f"{mark} {p.pid:<8}{p.name}")
    L.append("")
    L.append("  # 标 ! 的是系统关键进程，不允许结束")
    return "\n".join(L)


# ---------------------------------------------------------------- CPU

# 调频策略。默认策略决定了体感速度
GOVERNORS = {
    "performance": "始终最高频——最快，最耗电最热",
    "powersave": "始终最低频——最省电，最慢",
    "ondemand": "按需升频——老策略",
    "schedutil": "按调度器负载调频（现代默认，推荐）",
    "conservative": "渐进升降频——平稳但响应慢",
}


def cpu_info() -> dict:
    info = {"model": "", "cores": 0, "online": 0, "governor": "",
            "flags": []}
    try:
        t = Path("/proc/cpuinfo").read_text()
    except OSError:
        return info
    m = re.search(r"model name\s*:\s*(.+)", t)
    if m:
        info["model"] = m.group(1).strip()
    info["cores"] = t.count("processor\t:")
    m = re.search(r"flags\s*:\s*(.+)", t)
    if m:
        info["flags"] = m.group(1).split()
    # 在线核心数：被离线的话会少于实际
    try:
        info["online"] = len(Path("/sys/devices/system/cpu/online")
                             .read_text().strip().split(","))
    except OSError:
        info["online"] = info["cores"]
    # 当前调频策略
    try:
        info["governor"] = Path(
            "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
        ).read_text().strip()
    except OSError:
        info["governor"] = ""
    return info


def cpu_report() -> str:
    i = cpu_info()
    L = ["CPU：", ""]
    L.append(f"  型号: {i['model'] or '未知'}")
    L.append(f"  核心: {i['cores']} 个，在线 {i['online']} 个")
    if i["cores"] and i["online"] < i["cores"]:
        L.append("  ! 在线核心少于总数——"
                 "部分核心被离线了，会显得比实际慢，"
                 "用户常误以为是 CPU 坏了")
    if i["governor"]:
        L.append(f"  调频策略: {i['governor']} — "
                 f"{GOVERNORS.get(i['governor'], '')}")
    else:
        L.append("  调频策略: 读不到（虚拟机常见）")
    L.append("")
    L.append("  可选策略：")
    for k, v in GOVERNORS.items():
        L.append(f"    {k:<14}{v}")
    L.append("")
    L.append("  # **用户觉得'机器慢'常常是调频策略的锅**，")
    L.append("  # 不是 CPU 不行")
    return "\n".join(L)


def governor_cmd(g: str) -> str:
    if g not in GOVERNORS:
        raise ProcError(f"没有这个策略 {g}"
                        f"（可选：{'、'.join(GOVERNORS)}）")
    return (f"echo {g} > /sys/devices/system/cpu/cpufreq/"
            f"policy0/scaling_governor\n"
            f"# 重启后失效，要持久化需写 cpufreq 配置")


# ---------------------------------------------------------------- 显卡

# 显卡类型判断。双显卡没切干净会"黑屏但系统还活着"
GPU_KINDS = {"i915": "Intel 核显", "amdgpu": "AMD",
             "nouveau": "NVIDIA（开源）", "nvidia": "NVIDIA（专有）",
             "virtio_gpu": "虚拟显卡"}


def gpu_info() -> list:
    """列出显卡设备。

    只取 cardN，不带 -XXX 后缀。card0-Virtual-1 之类是**连接器**
    （代表一个输出接口），不是显卡——把它们当显卡会让
    "检测到几张卡"这个数字失真。
    """
    import re as _re
    out = []
    base = Path("/sys/class/drm")
    if not base.exists():
        return out
    for d in sorted(base.iterdir()):
        if not _re.fullmatch(r"card\d+", d.name):
            continue
        out.append((d.name, _driver_of(d)))
    return out


def _driver_of(card: Path) -> str:
    """读显卡驱动名。

    先试 driver 符号链接；解析不了时退回 uevent 里的 DRIVER=。
    只读一种方式的话，在某些文件系统（如 virtiofs）上
    会全部识别成"未识别驱动"，于是"没装驱动"的提示满天飞——
    那是误报，会让人不再相信这个检查。
    """
    drv = card / "device" / "driver"
    if drv.exists():
        try:
            return drv.resolve().name
        except OSError:
            pass
    ue = card / "device" / "uevent"
    if ue.exists():
        try:
            for line in ue.read_text().splitlines():
                if line.startswith("DRIVER="):
                    return line.split("=", 1)[1].strip()
        except OSError:
            pass
    return ""


def gpu_report() -> str:
    gpus = gpu_info()
    L = ["显卡：", ""]
    if not gpus:
        L.append("  ! 没有 DRM 设备——虚拟机或没装驱动")
        L.append("    **没有硬件加速时浏览器视频会掉帧且 CPU 占满**")
        return "\n".join(L)
    for dev, drv in gpus:
        title = GPU_KINDS.get(drv, drv or "（未识别驱动）")
        L.append(f"  {dev:<10}{title}")
    names = [d for _, d in gpus]
    # 双显卡
    has_intel = "i915" in names
    has_nvidia = any(n.startswith("nvidia") for n in names if n)
    if has_intel and has_nvidia:
        L.append("")
        L.append("  ! 检测到核显 + NVIDIA 独显——")
        L.append("    切换时若没切干净会出现"
                 "**屏幕黑但系统还活着**")
    return "\n".join(L)


def accel_check() -> list:
    """硬件加速可用性。"""
    problems = []
    gpus = gpu_info()
    if not gpus:
        problems.append("没有显卡设备——无法硬件加速，"
                        "视频会掉帧且 CPU 占满")
        return problems
    if not Path("/dev/dri").exists() or not any(Path("/dev/dri").iterdir()):
        problems.append("/dev/dri 为空——没有渲染节点，"
                        "应用拿不到硬件加速")
    return problems


# ---------------------------------------------------------------- 包使用时间

def usage_db(root: Path) -> Path:
    return Path(root) / "var" / "lib" / "qypkg" / "usage.json"


def _load_usage(root: Path) -> list:
    p = usage_db(root)
    if not p.exists():
        return []
    try:
        return json.loads(p.read_text()).get("packages", [])
    except Exception:
        return []


def record_install(root: Path, package: str) -> None:
    """记录安装时间。"""
    items = _load_usage(root)
    now = time.strftime("%F %T")
    for it in items:
        if it.get("package") == package:
            it["installed"] = now
            it.setdefault("last_used", now)
            _save_usage(root, items)
            return
    items.append({"package": package, "installed": now,
                  "last_used": now, "count": 0})
    _save_usage(root, items)


def record_use(root: Path, package: str) -> None:
    items = _load_usage(root)
    hit = next((x for x in items if x.get("package") == package), None)
    if hit is None:
        record_install(root, package)
        return
    hit["last_used"] = time.strftime("%F %T")
    hit["count"] = int(hit.get("count", 0)) + 1
    _save_usage(root, items)


def _save_usage(root: Path, items: list) -> None:
    p = usage_db(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps({"packages": items},
                                    ensure_ascii=False, indent=1).encode())


def usage_report(root: Path) -> str:
    """包使用报告。

    没有这个就无法知道哪些包装了一次再没用过，
    也就没法清理——系统会越来越臃肿。
    """
    items = _load_usage(root)
    if not items:
        return "没有使用记录"
    L = ["包安装与使用时间：", ""]
    L.append(f"  {'包':<20}{'安装时间':<22}{'最后使用':<22}次数")
    for it in sorted(items, key=lambda x: x.get("last_used", "")):
        L.append(f"  {it.get('package', '?'):<20}"
                 f"{it.get('installed', '-'):<22}"
                 f"{it.get('last_used', '-'):<22}"
                 f"{it.get('count', 0)}")
    return "\n".join(L)


def unused_packages(root: Path, days: int = 90) -> list:
    """多久没用过的包。"""
    import datetime
    out = []
    now = datetime.datetime.now()
    for it in _load_usage(root):
        lu = it.get("last_used", "")
        try:
            t = datetime.datetime.strptime(lu, "%F %T")
        except ValueError:
            continue
        if (now - t).days >= days:
            out.append((it.get("package", "?"), (now - t).days))
    return sorted(out, key=lambda x: -x[1])


# ---------------------------------------------------------------- 内置系统包

# 不可卸载的包。误删会让系统起不来
BUILTIN_ESSENTIAL = [
    "filesystem", "qyinit", "glibc", "linux-firmware", "pam", "sudo",
]
BUILTIN_RECOMMENDED = [
    "qypkg", "qybuild", "logrotate", "syslog-ng", "networkmanager",
]


def builtin_list() -> str:
    L = ["内置系统包：", ""]
    L.append("  不可卸载（删掉系统起不来）：")
    for p in BUILTIN_ESSENTIAL:
        L.append(f"    {p}")
    L.append("")
    L.append("  建议保留（删掉会明显影响可用性）：")
    for p in BUILTIN_RECOMMENDED:
        L.append(f"    {p}")
    L.append("")
    L.append("  # 包管理器必须挡住第一类的卸载请求。")
    L.append("  # 用户误删基础包会让系统起不来，")
    L.append("  # 而包管理器如果允许删就是失职")
    return "\n".join(L)


def can_remove(package: str) -> dict:
    if package in BUILTIN_ESSENTIAL:
        return {"ok": False,
                "reason": f"{package} 是系统基础包，不允许卸载——"
                          f"删掉系统起不来"}
    if package in BUILTIN_RECOMMENDED:
        return {"ok": True, "warn":
                f"{package} 建议保留，删掉会明显影响可用性"}
    return {"ok": True, "reason": ""}


# ---------------------------------------------------------------- 工具登记

@dataclass
class Tool:
    name: str
    purpose: str
    module: str


def tool_registry() -> list:
    """登记系统里所有命令行工具。

    有 20 多个工具，没有统一登记就没人知道有哪些、怎么调。
    """
    return [
        Tool("qybuild", "构建配方为包", "builder"),
        Tool("qypkg", "包管理：安装/升级/回滚", "pkgmgr"),
        Tool("qyrepo", "仓库索引与签名", "repo"),
        Tool("qycross", "交叉编译配置", "crosstool"),
        Tool("qyci", "持续集成门禁", "ci"),
        Tool("qyrepro", "可复现构建", "repro"),
        Tool("qypatch", "补丁管理", "patch"),
        Tool("qysec", "安全响应与 CVE", "security"),
        Tool("qyrelease", "发布工程", "release"),
        Tool("qysource", "软件源管理", "sources"),
        Tool("qydisk", "分区、镜像、装机脚本", "disk"),
        Tool("qyandroid", "安卓设备适配", "android"),
        Tool("qyinit", "1 号进程与服务管理", "—"),
        Tool("qypam", "PAM 配置", "pamd"),
        Tool("qylocale", "语言与键盘布局", "locale"),
        Tool("qynet", "网络服务、端口、共享存储", "net"),
        Tool("qyhw", "硬件与固件映射", "hardware"),
        Tool("qyfiles", "文件管理、回收站、解压", "files"),
        Tool("qyrun", "多步脚本与断点续跑", "runner"),
        Tool("qyctl", "系统控制：电源、显示、设备", "control"),
        Tool("qyperms", "权限框架", "perms"),
        Tool("qymedia", "媒体与个人数据", "media"),
        Tool("qynotify", "通知与窗口表现", "notify"),
        Tool("qykmod", "内核模块管理", "kmod"),
        Tool("qyudev", "设备节点权限", "udev"),
        Tool("qyio", "输入设备与外设", "io"),
        Tool("qysensor", "传感器与平台控制", "sensors"),
        Tool("qyshell", "桌面外壳", "shell"),
        Tool("qyproc", "进程、CPU、显卡、使用统计", "proc"),
        Tool("qydesktop", "面板与状态栏定义", "desktop"),
    ]


def tool_report() -> str:
    L = ["命令行工具（统一登记）：", ""]
    for t in tool_registry():
        L.append(f"  {t.name:<12}{t.purpose}")
    L.append("")
    L.append(f"  共 {len(tool_registry())} 个工具")
    return "\n".join(L)


def find_tool(keyword: str) -> list:
    k = keyword.lower()
    return [t for t in tool_registry()
            if k in t.name.lower() or k in t.purpose.lower()]


# ---------------------------------------------------------------- 主命令

def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyproc",
                                 description="LANXIU Linux 进程与资源")
    ap.add_argument("--root", default="/")
    sub = ap.add_parsers = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("ps", help="进程列表")
    sp.add_argument("--top", type=int, default=10)
    sp = sub.add_parser("kill", help="结束进程")
    sp.add_argument("target")
    sp.add_argument("--force", action="store_true")
    sub.add_parser("cpu", help="CPU 信息")
    sp = sub.add_parser("governor", help="调频策略")
    sp.add_argument("mode", nargs="?", choices=list(GOVERNORS))
    sub.add_parser("gpu", help="显卡")
    sub.add_parser("accel", help="硬件加速检查")
    sp = sub.add_parser("usage", help="包使用时间")
    sp.add_argument("--unused", type=int, metavar="DAYS")
    sp = sub.add_parser("mark-used", help="记录使用")
    sp.add_argument("package")
    sub.add_parser("builtin", help="内置系统包")
    sp = sub.add_parser("can-remove", help="能否卸载")
    sp.add_argument("package")
    sub.add_parser("tools", help="工具登记")
    sp = sub.add_parser("find-tool", help="按用途找工具")
    sp.add_argument("keyword")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "ps":
            print(top_procs(a.top))
            return 0
        if a.cmd == "kill":
            probs = kill_check(a.target, a.force)
            for x in probs:
                util.log("err", x)
            # 强制杀只是警告，不是拒绝——有时候确实必须
            blocking = [x for x in probs if "不允许" in x]
            if blocking:
                return 1
            print(kill_cmd(int(a.target) if a.target.isdigit() else 0,
                           a.force) if a.target.isdigit()
                  else f"pkill -{'9' if a.force else 'TERM'} {a.target}")
            return 0
        if a.cmd == "cpu":
            print(cpu_report())
            return 0
        if a.cmd == "governor":
            if a.mode is None:
                for k, v in GOVERNORS.items():
                    print(f"  {k:<14}{v}")
                return 0
            print(governor_cmd(a.mode))
            return 0
        if a.cmd == "gpu":
            print(gpu_report())
            return 0
        if a.cmd == "accel":
            probs = accel_check()
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "硬件加速可用")
            return 1 if probs else 0
        if a.cmd == "usage":
            if a.unused:
                items = unused_packages(root, a.unused)
                if not items:
                    print(f"没有超过 {a.unused} 天未使用的包")
                else:
                    print(f"{a.unused} 天以上未使用：")
                    for p, d in items:
                        print(f"  {p:<20}{d} 天")
                return 0
            print(usage_report(root))
            return 0
        if a.cmd == "mark-used":
            record_use(root, a.package)
            util.log("ok", f"已记录 {a.package} 的使用")
            return 0
        if a.cmd == "builtin":
            print(builtin_list())
            return 0
        if a.cmd == "can-remove":
            r = can_remove(a.package)
            if not r["ok"]:
                util.log("err", r["reason"])
                return 1
            if r.get("warn"):
                util.log("warn", r["warn"])
            util.log("ok", "可以卸载")
            return 0
        if a.cmd == "tools":
            print(tool_report())
            return 0
        if a.cmd == "find-tool":
            hits = find_tool(a.keyword)
            if not hits:
                util.log("warn", f"没有匹配“{a.keyword}”的工具")
                return 1
            for t in hits:
                print(f"  {t.name:<12}{t.purpose}")
            return 0
    except ProcError as e:
        util.log("err", str(e))
        return 1
    return 1
