"""系统控制：电源、显示、声音、设备、感知、网络。

这一层把"用户能看到的开关"和"系统里真实存在的机制"连起来。
难点不在于列出开关，而在于每个开关背后都有会咬人的细节：

**电源**
- 休眠（hibernate）需要 swap 且 swap 要大于内存。
  不满足时休眠会失败，而失败发生在已经关掉大部分设备之后——
  用户看到的是"合上盖子后再打开发现没睡成"。
  所以必须在发起前检查，而不是让它失败。
- 强制重启不等同步，会丢数据。它只该在完全无响应时用，
  所以要明确标注，不能做成普通菜单项。
- 定时开机靠 RTC 唤醒，很多主板不支持——
  不支持时静默失败，用户第二天发现机器没起来。

**显示**
- 亮度：写 backlight 需要权限，且范围是 0~max_brightness，
  写超了会失败。不同机器 max 值不同，不能硬编码。
- 旋转依赖陀螺仪（iio 传感器）。没有陀螺仪的机器
  给了旋转开关也没用，所以要先检测再显示这个开关。
- 屏幕识别：EDID 决定分辨率与刷新率。读不到 EDID
  会回退到 1024x768，用户以为显卡坏了。

**GPU 虚拟显示**
安卓容器/云手机场景需要把真实 GPU 或虚拟 GPU 呈给上层。
没有 virtio-gpu 或 DRM 虚拟驱动，投屏和云手机都起不来。

**声音**
默认输出设备可能是错的（HDMI 优先于模拟输出）。
用户插着耳机却从扬声器出声，是常见的"没声音"报告成因。

**充电**
充电阈值（ conservation mode）能延长电池寿命，
但设太低会让用户以为"充不进去电"。所以要说明。

**自我感知**
系统要知道自己在什么环境里跑（物理机/虚拟机/容器）。
这个判断错了，后面的电源管理、GPU 选择、定时任务全都会做无用功。
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


class CtlError(RuntimeError):
    pass


# ================================================================ 环境感知

# 运行环境。判断错了后面全做无用功
ENV_PHYSICAL = "physical"      # 物理机
ENV_VM = "vm"                  # 虚拟机
ENV_CONTAINER = "container"    # 容器
ENV_UNKNOWN = "unknown"


def detect_env(root: Path = Path("/")) -> str:
    """判断运行环境。

    顺序很重要：容器里也能看到虚拟化特征，
    所以先判容器。判反了会把容器的电源管理全做一遍，
    而容器根本没有电源状态。
    """
    # 1. 容器：/.dockerenv 或 cgroup 里有 docker/kubepods
    if Path("/.dockerenv").exists():
        return ENV_CONTAINER
    try:
        cg = Path("/proc/1/cgroup").read_text()
        if re.search(r"docker|kubepods|containerd|lxc", cg):
            return ENV_CONTAINER
    except OSError:
        pass
    # 2. 虚拟机：CPU 特征或 DMI 厂商
    try:
        cpu = Path("/proc/cpuinfo").read_text()
        if "hypervisor" in cpu:
            return ENV_VM
    except OSError:
        pass
    for f in ("/sys/class/dmi/id/product_name",
              "/sys/class/dmi/id/sys_vendor"):
        try:
            t = Path(f).read_text().lower()
        except OSError:
            continue
        if re.search(r"kvm|qemu|vmware|virtualbox|xen|hyper-v|bochs", t):
            return ENV_VM
    # 3. 有电源/电池设备基本是物理机
    if Path("/sys/class/power_supply").exists() and \
            any(Path("/sys/class/power_supply").iterdir()):
        return ENV_PHYSICAL
    return ENV_UNKNOWN


ENV_CN = {
    ENV_PHYSICAL: "物理机", ENV_VM: "虚拟机",
    ENV_CONTAINER: "容器", ENV_UNKNOWN: "未能确定",
}

# 不该在某环境下做的事。做无用功还算轻的，
# 有些操作在容器里会直接失败并留下半截状态
ENV_BLOCKED = {
    ENV_CONTAINER: ["休眠", "重启", "关机", "定时开机", "充电阈值",
                    "屏幕亮度", "电池管理"],
    ENV_VM: ["定时开机", "充电阈值", "电池管理", "指纹解锁"],
}


def blocked_actions(env: str) -> list:
    return ENV_BLOCKED.get(env, [])


def self_info(root: Path = Path("/")) -> dict:
    """系统对自身运行环境的认知。"""
    env = detect_env(root)
    info = {"env": env, "env_cn": ENV_CN.get(env, env),
            "blocked": blocked_actions(env)}
    # 运行时长
    try:
        up = float(Path("/proc/uptime").read_text().split()[0])
        info["uptime_s"] = int(up)
        info["uptime"] = humanize_seconds(int(up))
        info["boot_time"] = time.strftime(
            "%F %T", time.localtime(time.time() - up))
    except (OSError, IndexError, ValueError):
        info["uptime_s"] = 0
        info["uptime"] = "未知"
        info["boot_time"] = "未知"
    # CPU
    try:
        cpu = Path("/proc/cpuinfo").read_text()
        info["cpu_cores"] = cpu.count("processor\t:")
        m = re.search(r"model name\s*:\s*(.+)", cpu)
        info["cpu_model"] = m.group(1).strip() if m else "未知"
    except OSError:
        info["cpu_cores"] = 0
        info["cpu_model"] = "未知"
    # 内存
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                info["mem_total_mb"] = int(line.split()[1]) // 1024
                break
    except OSError:
        info["mem_total_mb"] = 0
    return info


def humanize_seconds(s: int) -> str:
    d, r = divmod(s, 86400)
    h, r = divmod(r, 3600)
    m = r // 60
    if d:
        return f"{d} 天 {h} 小时 {m} 分"
    if h:
        return f"{h} 小时 {m} 分"
    return f"{m} 分"


# ================================================================ 电源

@dataclass
class PowerAction:
    id: str
    title: str
    command: str
    risky: bool = False       # 会丢数据
    need_root: bool = True
    desc: str = ""


POWER_ACTIONS = [
    PowerAction("shutdown", "关机", "poweroff", desc="正常关机，会先同步数据"),
    PowerAction("reboot", "重启", "reboot", desc="正常重启"),
    PowerAction("suspend", "挂起（待机）",
                "echo mem > /sys/power/state",
                desc="内存保持供电，唤醒快；断电则丢失"),
    PowerAction("hibernate", "休眠", "echo disk > /sys/power/state",
                desc="写到 swap 后断电，唤醒慢但不断电也不丢"),
    PowerAction("force-reboot", "强制重启",
                "echo b > /proc/sysrq-trigger", risky=True,
                desc="不同步、不卸载，直接重启。只在完全无响应时用"),
    PowerAction("force-shutdown", "强制关机",
                "echo o > /proc/sysrq-trigger", risky=True,
                desc="直接断电。会丢数据，可能损坏文件系统"),
]


def hibernate_ready(root: Path = Path("/")) -> list:
    """休眠前的检查。

    休眠失败发生在已经关掉大部分设备之后——用户看到的是
    "合上盖子后打开发现没睡成"。所以必须提前检查。
    """
    problems = []
    mem_mb = 0
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                mem_mb = int(line.split()[1]) // 1024
                break
    except OSError:
        pass
    swaps = []
    try:
        for line in Path("/proc/swaps").read_text().splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 3:
                swaps.append((parts[0], int(parts[2]) // 1024))
    except OSError:
        pass
    if not swaps:
        problems.append("没有启用 swap——休眠无处可写，会直接失败。"
                        "先创建 swap 分区或 swap 文件")
        return problems
    total = sum(s[1] for s in swaps)
    if mem_mb and total < mem_mb:
        problems.append(
            f"swap 只有 {total}MB，内存有 {mem_mb}MB——"
            f"休眠镜像写不下，会失败。swap 至少要等于内存大小")
    # resume 参数：没有它休眠后起不来，
    # 而且表现为"正常开机但之前的工作全没了"
    try:
        cmdline = Path("/proc/cmdline").read_text()
        if "resume=" not in cmdline:
            problems.append(
                "内核命令行没有 resume= 参数——休眠后无法从镜像恢复，"
                "会变成一次普通开机，之前的工作全部丢失")
    except OSError:
        pass
    return problems


def scheduled_power(action: str, when: str) -> str:
    """定时关机/重启。"""
    if action == "shutdown":
        return f"shutdown -h {when}"
    if action == "reboot":
        return f"shutdown -r {when}"
    raise CtlError(f"不支持的定时动作 {action}")


def rtc_wake(when_epoch: int) -> list:
    """定时开机。靠 RTC 唤醒，很多主板不支持。

    不支持时静默失败——用户第二天发现机器没起来，
    且完全不知道为什么。所以要把检查写进去。
    """
    return [
        f"# 先确认 RTC 支持唤醒，不支持的话下面全都不生效",
        f"ls /sys/class/rtc/rtc0/wakealarm || echo '此机器不支持定时开机'",
        f"echo 0 > /sys/class/rtc/rtc0/wakealarm",
        f"echo {when_epoch} > /sys/class/rtc/rtc0/wakealarm",
        f"cat /sys/class/rtc/rtc0/wakealarm   # 读回来确认写进去了",
        f"# 注意：部分主板需要在 BIOS 里启用 RTC wake / Power on by alarm",
    ]


# ================================================================ 显示

def backlight_devices() -> list:
    p = Path("/sys/class/backlight")
    if not p.exists():
        return []
    return sorted(x.name for x in p.iterdir())


def brightness_cmd(value: int | None = None,
                   percent: int | None = None) -> str:
    """设置亮度。

    范围是 0 ~ max_brightness，不同机器 max 不同，不能硬编码——
    写超了不报错，只是没反应，看不出是值越界。

    没有背光设备时仍返回模板（用 <设备> 占位）而不是报错：
    用户需要先看到用法，才知道该去哪找设备名。
    """
    devs = backlight_devices()
    d = devs[0] if devs else "<设备名>"
    if percent is not None:
        return ("\n".join([
            "# 按百分比设置：先读最大值，不能硬编码",
            f"MAX=$(cat /sys/class/backlight/{d}/max_brightness)",
            "# 越界写入不报错，只是没反应——所以显式检查一次",
            f"V=$(( MAX * {percent} / 100 ))",
            f"[ $V -le $MAX ] || echo '超过最大值，不会生效'",
            f"echo $V > /sys/class/backlight/{d}/brightness",
        ]))
    if value is None:
        raise CtlError("value 和 percent 至少要给一个")
    return (f"MAX=$(cat /sys/class/backlight/{d}/max_brightness)\n"
            f"# 检查越界：写超了不会报错，只是没反应\n"
            f"[ {value} -le $MAX ] || echo '超过最大值，不会生效'\n"
            f"echo {value} > /sys/class/backlight/{d}/brightness")


def edid_info() -> dict:
    """读显示器 EDID。

    读不到 EDID 会回退到 1024x768，用户以为显卡坏了。
    所以读不到要明确指出，而不是默默用低分辨率。
    """
    out = {"found": False, "name": "", "path": ""}
    for p in sorted(Path("/sys/class/drm").glob("*/edid")):
        try:
            data = p.read_bytes()
        except OSError:
            continue
        if not data or data.strip(b"\x00") == b"":
            continue
        out["found"] = True
        out["path"] = str(p)
        # EDID 里的显示器名在 0x36 起的 13 字节
        try:
            desc = data[0x36:0x36 + 13]
            out["name"] = desc.decode("ascii", errors="ignore").strip()
        except Exception:
            pass
        break
    return out


def display_report() -> str:
    L = ["显示：", ""]
    ed = edid_info()
    if ed["found"]:
        L.append(f"  显示器: {ed['name'] or '（未报告名称）'}")
        L.append(f"  EDID: {ed['path']}")
    else:
        L.append("  ! 读不到 EDID——会回退到 1024x768，"
                 "表现为分辨率异常低，常被误判为显卡故障")
    devs = backlight_devices()
    if devs:
        L.append(f"  背光设备: {'、'.join(devs)}")
    else:
        L.append("  无背光设备（虚拟机/容器常见）")
    # 陀螺仪：没有它旋转开关给了也没用
    ii = Path("/sys/bus/iio/devices")
    has_gyro = False
    if ii.exists():
        for d in ii.iterdir():
            try:
                if "gyro" in (d / "name").read_text().lower() or \
                   (d / "in_anglvel_scale").exists():
                    has_gyro = True
                    break
            except OSError:
                continue
    L.append(f"  陀螺仪: {'有（可自动旋转）' if has_gyro else '无（旋转开关不可用）'}")
    return "\n".join(L)


def screenshot_cmd(out: str = "~/Pictures/screenshot.png",
                   region: bool = False) -> str:
    if region:
        return f"grim -g \"$(slurp)\" {out}   # Wayland 下框选截图"
    return f"grim {out}   # Wayland 全屏截图；X11 用 import 或 xwd"


def record_cmd(out: str = "~/Videos/record.mp4", audio: bool = True) -> str:
    a = " --audio" if audio else ""
    return f"wf-recorder{a} -f {out}   # Wayland 录屏；X11 用 ffmpeg -f x11grab"


def cast_cmd(kind: str = "miracast", port: int = 0) -> str:
    """投屏。"""
    if kind == "miracast":
        return ("# Miracast/WiFi Display：需要无线网卡支持 P2P\n"
                "iw dev | grep -A2 'type P2P' || "
                "echo '此网卡不支持 P2P，无法用 Miracast'\n"
                "# 可用时：wfd-supplicant 或 gnome-network-displays")
    if kind == "vnc":
        return f"wayvnc 0.0.0.0:{port or 5900}   # Wayland 远程桌面"
    raise CtlError(f"不支持的投屏方式 {kind}")


def virtual_gpu_report() -> str:
    """安卓 GPU 转虚拟显卡（云手机/容器场景）。

    没有 virtio-gpu 或 DRM 虚拟驱动，上层拿不到显示设备，
    表现为"系统起来了但没有画面"。
    """
    L = ["虚拟显示（容器/云手机场景）：", ""]
    p = Path("/dev/dri")
    if p.exists() and any(p.iterdir()):
        L.append(f"  DRM 设备: {'、'.join(x.name for x in sorted(p.iterdir()))}")
    else:
        L.append("  ! /dev/dri 为空——没有渲染节点，上层拿不到显示设备")
        L.append("    需要：CONFIG_DRM_VIRTIO_GPU 或 virgl 软件渲染")
    if Path("/dev/udmabuf").exists():
        L.append("  udmabuf: 可用（零拷贝共享缓冲区）")
    if shutil_which("virgl_test_server"):
        L.append("  virgl: 已安装（可做软件 3D 加速）")
    else:
        L.append("  virgl: 未安装——容器里只能软件渲染，3D 极慢")
    return "\n".join(L)


def shutil_which(x: str) -> bool:
    import shutil
    return shutil.which(x) is not None


# ================================================================ 声音

def audio_report() -> str:
    L = ["声音：", ""]
    p = Path("/proc/asound/cards")
    if p.exists():
        txt = p.read_text().strip()
        if txt:
            L.append("  声卡：")
            for line in txt.splitlines():
                L.append(f"    {line.strip()}")
        else:
            L.append("  ! 没有检测到声卡（检查固件：qyhw scan）")
    else:
        L.append("  ! /proc/asound 不存在——内核未启用 ALSA")
    # 默认输出设备选错是"没声音"的常见成因
    L.append("")
    L.append("  # 插着耳机却从扬声器出声，通常是默认设备选错了：")
    L.append("  wpctl status           # 查看所有输出")
    L.append("  wpctl set-default <id> # 指定默认输出")
    return "\n".join(L)


# ================================================================ 设备

DEVICE_CHECKS = {
    "usb": ("USB", "/sys/bus/usb/devices",
            "lsusb 无输出通常是用仅充电的数据线，或 USB 控制器未启用"),
    "camera": ("摄像头", "/dev/video*",
               "没有 /dev/video* 说明 UVC 驱动未加载或内核未开 CONFIG_VIDEO_DEV"),
    "nfc": ("NFC", "/dev/nfc*",
            "多数 NFC 走 USB 或串口，先确认 USB 里有该设备"),
    "fingerprint": ("指纹", "/sys/bus/platform/devices/*fprint*",
                    "指纹需要 fprintd 与 libfprint，且多数消费级传感器无 Linux 驱动"),
    "gps": ("GPS", "/dev/ttyGPS*",
            "GPS 多走串口或 USB，先确认设备存在"),
    "typec": ("Type-C", "/sys/class/typec",
              "没有 typec 目录说明内核未开 CONFIG_TYPEC，"
              "只有供电没有 Alt Mode/DP"),
    "headphone": ("耳机", "/sys/class/sound",
                  "插拔检测靠 jack 检测接口，部分机器报不出插入事件"),
}


def device_report() -> str:
    L = ["设备：", ""]
    for key, (cn, pat, hint) in DEVICE_CHECKS.items():
        ok = False
        if "*" in pat:
            import glob
            ok = bool(glob.glob(pat))
        else:
            ok = Path(pat).exists() and any(Path(pat).iterdir()) \
                if Path(pat).is_dir() else Path(pat).exists()
        if key == "typec":
            ok = Path("/sys/class/typec").exists()
        mark = "有" if ok else "无"
        L.append(f"  {cn:<8}{mark}")
        if not ok:
            L.append(f"      {hint}")
    return "\n".join(L)


# ================================================================ 网络

def private_dns(hostname: str) -> list:
    """私人 DNS（DoT）。

    配了之后如果 DoT 服务器不可达，会表现为"所有网站打不开"，
    而不是"DNS 失败"——排查方向会完全跑偏。
    """
    return [
        f"# DoT：配了之后服务器不可达表现为'所有网站打不开'，",
        f"# 而不是'DNS 失败'，排查很容易跑偏",
        f"resolvectl dns <接口> {hostname}",
        f"resolvectl domain <接口> ~.",
        f"resolvectl status   # 确认 DoT 已生效",
        f"# 出问题时的回退：resolvectl revert <接口>",
    ]


def dns_auto_detect() -> str:
    """DNS 路由自动识别：判断当前 DNS 实际用的什么。"""
    return ("# 先确认现在实际用的是谁解析——很多'网络问题'其实是 DNS 问题\n"
            "resolvectl status\n"
            "cat /etc/resolv.conf\n"
            "# 注意：resolv.conf 可能是符号链接，改错文件等于没改\n"
            "ls -l /etc/resolv.conf")


def vpn_check() -> list:
    problems = []
    if not Path("/dev/net/tun").exists():
        problems.append("/dev/net/tun 不存在——缺 CONFIG_TUN，"
                        "所有基于 TUN 的 VPN 都无法工作")
    import shutil
    if not shutil.which("wg"):
        problems.append("没有 wireguard-tools（wg）——无法配置 WireGuard VPN")
    return problems


def wlan_toggle_cmd(on: bool) -> str:
    """无线开关。

    硬件开关（rfkill hard block）优先于软件开关——
    网卡被硬挡住时，软件层面怎么开都没用。
    所以命令里必须带上检查，而不是只给一条 rfkill unblock。
    """
    return "\n".join([
        "# 硬件开关优先于软件开关：被 hard blocked 时软件怎么开都没用",
        "rfkill unblock wifi" if on else "rfkill block wifi",
        "rfkill list   # 确认 Soft blocked 与 Hard blocked 都为 no",
    ])


# ================================================================ 电池

def battery_report() -> str:
    p = Path("/sys/class/power_supply")
    L = ["电池与充电：", ""]
    L.append("  # 充电阈值（conservation mode）能延长电池寿命，")
    L.append("  # 但设太低用户会以为'充不进去电'，建议下限不低于 60")
    L.append("")
    if not p.exists() or not any(p.iterdir()):
        L.append("  ! 没有电源设备（虚拟机/容器常见）")
        return "\n".join(L)
    for d in sorted(p.iterdir()):
        try:
            typ = (d / "type").read_text().strip()
        except OSError:
            continue
        L.append(f"  {d.name}（{typ}）")
        for f in ("capacity", "status", "charge_control_start_threshold",
                  "charge_control_end_threshold"):
            fp = d / f
            if fp.exists():
                try:
                    L.append(f"    {f}: {fp.read_text().strip()}")
                except OSError:
                    pass
    return "\n".join(L)


# ================================================================ 感知

def thermal_report() -> str:
    L = ["温度：", ""]
    p = Path("/sys/class/thermal")
    if p.exists():
        for d in sorted(p.iterdir()):
            try:
                t = int((d / "temp").read_text().strip())
                typ = ""
                tf = d / "type"
                if tf.exists():
                    typ = tf.read_text().strip()
                L.append(f"  {typ or d.name}: {t/1000:.1f}°C")
            except (OSError, ValueError):
                continue
    if len(L) <= 2:
        L.append("  ! 读不到温度传感器——无法做降温策略")
    return "\n".join(L)


# ================================================================ 汇总

def overview(root: Path = Path("/")) -> str:
    """系统概览（关于本机）。"""
    info = self_info(root)
    L = ["关于本机：", ""]
    L.append(f"  运行环境: {info['env_cn']}")
    if info["blocked"]:
        L.append(f"  此环境不可用: {'、'.join(info['blocked'])}")
    L.append(f"  CPU: {info['cpu_model']}（{info['cpu_cores']} 核）")
    L.append(f"  内存: {info['mem_total_mb']} MB")
    L.append(f"  已运行: {info['uptime']}")
    L.append(f"  开机时间: {info['boot_time']}")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyctl",
                                 description="LANXIU Linux 系统控制")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("overview", help="关于本机")
    sub.add_parser("power", help="电源动作")
    sp = sub.add_parser("power-do", help="执行电源动作")
    sp.add_argument("action", choices=[a.id for a in POWER_ACTIONS])
    sp = sub.add_parser("hibernate-check", help="休眠前检查")
    sp = sub.add_parser("schedule", help="定时关机/重启")
    sp.add_argument("action", choices=["shutdown", "reboot"])
    sp.add_argument("when", help="如 +30 或 22:00")
    sp = sub.add_parser("rtc-wake", help="定时开机")
    sp.add_argument("epoch", type=int)
    sub.add_parser("display", help="显示状态")
    sp = sub.add_parser("brightness", help="设置亮度")
    sp.add_argument("--percent", type=int)
    sp.add_argument("--value", type=int)
    sub.add_parser("screenshot", help="截图命令")
    sub.add_parser("record", help="录屏命令")
    sp = sub.add_parser("cast", help="投屏")
    sp.add_argument("--kind", choices=["miracast", "vnc"], default="miracast")
    sub.add_parser("vgpu", help="虚拟显示/GPU")
    sub.add_parser("audio", help="声音状态")
    sub.add_parser("devices", help="设备列表")
    sub.add_parser("battery", help="电池与充电")
    sub.add_parser("thermal", help="温度")
    sp = sub.add_parser("privdns", help="私人 DNS")
    sp.add_argument("hostname")
    sub.add_parser("dns", help="DNS 自动识别")
    sub.add_parser("vpn", help="VPN 检查")
    sub.add_parser("wlan-on", help="开启无线")
    sub.add_parser("wlan-off", help="关闭无线")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "overview":
            print(overview(root))
            return 0
        if a.cmd == "power":
            env = detect_env(root)
            blocked = blocked_actions(env)
            for p in POWER_ACTIONS:
                tag = "  [会丢数据]" if p.risky else ""
                off = "  [此环境不可用]" if p.title in blocked else ""
                print(f"  {p.id:<16}{p.title}{tag}{off}")
                if p.desc:
                    print(f"      {p.desc}")
            return 0
        if a.cmd == "power-do":
            act = next(x for x in POWER_ACTIONS if x.id == a.action)
            env = detect_env(root)
            if act.title in blocked_actions(env):
                msg = (f"{act.title} 在{ENV_CN.get(env)}里不可用——"
                       f"此环境没有对应的硬件状态，执行只会留下半截状态")
                util.log("err", msg)
                print(f"拒绝执行: {msg}")
                return 1
            if act.risky:
                util.log("warn", f"{act.title}: {act.desc}")
                util.log("warn", "未保存的工作会全部丢失")
            if act.id == "hibernate":
                probs = hibernate_ready(root)
                if probs:
                    for x in probs:
                        util.log("err", x)
                    return 1
            print(act.command)
            return 0
        if a.cmd == "hibernate-check":
            probs = hibernate_ready(root)
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "可以休眠")
            return 1 if probs else 0
        if a.cmd == "schedule":
            print(scheduled_power(a.action, a.when))
            return 0
        if a.cmd == "rtc-wake":
            print("\n".join(rtc_wake(a.epoch)))
            return 0
        if a.cmd == "display":
            print(display_report())
            return 0
        if a.cmd == "brightness":
            print(brightness_cmd(a.value, a.percent))
            return 0
        if a.cmd == "screenshot":
            print(screenshot_cmd())
            return 0
        if a.cmd == "record":
            print(record_cmd())
            return 0
        if a.cmd == "cast":
            print(cast_cmd(a.kind))
            return 0
        if a.cmd == "vgpu":
            print(virtual_gpu_report())
            return 0
        if a.cmd == "audio":
            print(audio_report())
            return 0
        if a.cmd == "devices":
            print(device_report())
            return 0
        if a.cmd == "battery":
            print(battery_report())
            return 0
        if a.cmd == "thermal":
            print(thermal_report())
            return 0
        if a.cmd == "privdns":
            print("\n".join(private_dns(a.hostname)))
            return 0
        if a.cmd == "dns":
            print(dns_auto_detect())
            return 0
        if a.cmd == "vpn":
            probs = vpn_check()
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "VPN 支持正常")
            return 1 if probs else 0
        if a.cmd == "wlan-on":
            print(wlan_toggle_cmd(True))
            return 0
        if a.cmd == "wlan-off":
            print(wlan_toggle_cmd(False))
            return 0
    except CtlError as e:
        util.log("err", str(e))
        return 1
    return 1
