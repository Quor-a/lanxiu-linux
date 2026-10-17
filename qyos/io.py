"""输入设备与外设：键鼠、触摸板、触控笔、手柄、数位板、打印、扫描、多显示器、存储。

这一层的共性是：**设备插上了，但系统没"接住"它**。

触摸板插着但只有基本移动、没有多指手势——用户以为硬件不支持，
其实是走了通用 PS/2 驱动而不是 I2C 驱动。
数位板能画但没有压感——走了 HID 通用驱动而不是厂商驱动。
打印机能找到但打不出来——缺驱动或队列没配对。

几种典型的"接错了"：

**1. 触摸板走了降级驱动**
I2C 触摸板被识别成 PS/2 鼠标，能移动但没有多指手势、
没有点击拖拽。用户只会说"这触摸板不好用"。
判断方法：/proc/bus/input/devices 里看驱动名。

**2. 数位板没压感**
走了通用 HID，笔能用但压力恒定。绘画场景等于不能用。

**3. 打印机找到了但打不出**
CUPS 队列与驱动不匹配，任务卡住或输出乱码。
不检查的话用户会反复重发任务。

**4. 多显示器扩展没开**
外接显示器默认镜像或干脆不亮。
用户以为接口坏了。

**5. SD 读卡器读不出**
多数读卡器走 USB，但有些走专有 PCIe 通道，需要额外模块。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class IoError(RuntimeError):
    pass


# ---------------------------------------------------------------- 输入设备

@dataclass
class InputDevice:
    name: str
    handlers: str = ""     # event* 等
    driver: str = ""       # 驱动名（关键，决定能力）
    phys: str = ""         # 物理路径，含 i2c/ps2/usb


# 驱动 → 能力。走错驱动就是能力缺失
DRIVER_CAPABILITY = {
    "i2c_hid_acpi": ("完整", "多指手势、点击拖拽、掌压抑制"),
    "i2c_hid": ("完整", "多指手势、点击拖拽"),
    "hid_multitouch": ("完整", "多指手势"),
    "synaptics": ("较完整", "多指手势（老驱动，逐渐废弃）"),
    "psmouse": ("降级", "仅基本移动与点击——"
                     "**I2C 触摸板被识别成 PS/2 就是这个**"),
    "libinput": ("完整", "统一输入处理层"),
    "evdev": ("基础", "仅事件转发"),
}


def infer_driver(name: str, phys: str = "") -> str:
    """从设备名与物理路径推断它走的是哪个驱动。

    /proc/bus/input/devices 里没有直接的驱动字段，
    但 phys 路径会暴露总线：isa0060/serio 是 PS/2，i2c 是 I2C-HID，
    usb 走 HID。这恰恰是判断"有没有降级"的可靠依据——
    I2C 触摸板出现在 serio 上，就是被降级成了 PS/2。
    """
    p = (phys or "").lower()
    n = (name or "").lower()
    if p.startswith("i2c") or "i2c" in p:
        return "i2c_hid_acpi"
    if "isa0060" in p or "serio" in p:
        return "psmouse"      # PS/2 通道
    if p.startswith("usb") or "usb" in p:
        return "hid_multitouch"
    # 退化到按名字判断
    if "synaptics" in n or "elan" in n:
        return "synaptics"
    if "touchpad" in n:
        return "i2c_hid_acpi"
    return ""


def parse_input_devices(text: str) -> list:
    """解析 /proc/bus/input/devices。"""
    out = []
    cur: dict = {}
    for line in text.splitlines():
        if not line.strip():
            if cur.get("name"):
                name = cur.get("name", "")
                phys = cur.get("phys", "")
                # 驱动字段在这个文件里不存在，靠总线路径推断
                drv = cur.get("driver") or infer_driver(name, phys)
                out.append(InputDevice(name, cur.get("handlers", ""),
                                       drv, phys))
            cur = {}
            continue
        if line.startswith("N:"):
            cur["name"] = line.split("=", 1)[1].strip().strip('"')
        elif line.startswith("H:"):
            cur["handlers"] = line.split("=", 1)[1].strip()
        elif line.startswith("P:"):
            cur["phys"] = line.split("=", 1)[1].strip()
        elif line.startswith("D:"):
            cur["driver"] = line.split("=", 1)[1].strip()
    if cur.get("name"):
        name = cur.get("name", "")
        phys = cur.get("phys", "")
        drv = cur.get("driver") or infer_driver(name, phys)
        out.append(InputDevice(name, cur.get("handlers", ""), drv, phys))
    return out


def touchpad_diagnose(devs: list) -> str:
    """触摸板诊断：重点是走了降级驱动没有。"""
    pads = [d for d in devs
            if re.search(r"(?i)touchpad|touch pad|synaptics|elan|"
                         r"synps|hid.*mouse", d.name)]
    if not pads:
        return ("没有检测到触摸板。\n"
                "  笔记本上常见原因：BIOS 里禁用了、\n"
                "  或走了 I2C 但 i2c_hid_acpi 模块没加载（qykmod find 触摸板）")
    L = ["触摸板：", ""]
    for p in pads:
        L.append(f"  {p.name}")
        cap, desc = DRIVER_CAPABILITY.get(
            p.driver, ("未知", f"驱动 {p.driver or '（未识别）'}"))
        mark = "  " if cap in ("完整", "较完整") else "! "
        L.append(f"{mark}  驱动 {p.driver or '（未识别）'} → {cap}")
        L.append(f"      {desc}")
        if p.driver == "psmouse":
            # 这是最常见的"触摸板不好用"的真实原因
            L.append("")
            L.append("      # 若这块板实际是 I2C 接口，走 PS/2 就是降级：")
            L.append("      # 能移动但没有多指手势、没有点击拖拽。")
            L.append("      # 检查：qykmod find i2c hid")
    return "\n".join(L)


def is_degraded(dev: InputDevice) -> bool:
    return dev.driver == "psmouse"


# ---------------------------------------------------------------- 数位板

# 数位板品牌 → 需要的驱动。走通用 HID 就没压感
TABLET_DRIVERS = {
    "wacom": ("wacom", "Wacom 全系列，有压感"),
    "huion": ("huion", "绘王，部分型号需内核补丁"),
    "xp-pen": ("xp-pen", "XP-Pen"),
    "ugee": ("ugee", "友基"),
    "gaomon": ("gaomon", "高漫"),
    "veikk": ("veikk", "VEIKK"),
}


def tablet_diagnose(text: str = "") -> str:
    L = ["数位板：", ""]
    if text:
        # 指定了品牌：只列匹配的
        hit = [v for k, v in TABLET_DRIVERS.items() if k in text.lower()]
        if not hit:
            L.append(f"  没有收录品牌「{text}」。已收录：")
            for k, (mod, desc) in TABLET_DRIVERS.items():
                L.append(f"    {k:<10}{mod:<10}{desc}")
            return "\n".join(L)
    else:
        # 没指定：列全部供挑选
        hit = list(TABLET_DRIVERS.values())
        L.append("  已收录品牌（用 --brand 指定可只看某个）：")
    for mod, desc in hit:
        L.append(f"  需要驱动: {mod} — {desc}")
    L.append("")
    L.append("  # 走了通用 HID 的表现：笔能用但**压力恒定**。")
    L.append("  # 绘画场景等于不能用，且不会有任何报错。")
    L.append("  # 排查：xsetwacom --list devices（看压感轴是否存在）")
    return "\n".join(L)


# ---------------------------------------------------------------- 手柄

GAMEPAD_MODULES = ["xpad", "hid_sony", "hid_nintendo", "xone"]


def gamepad_diagnose() -> str:
    L = ["游戏手柄：", ""]
    L.append("  常见驱动模块：")
    for m in GAMEPAD_MODULES:
        L.append(f"    {m}")
    L.append("")
    L.append("  # Xbox 手柄走 xpad；索尼走 hid_sony；")
    L.append("  # Switch 走 hid_nintendo；Xbox One 无线需 xone。")
    L.append("  # 蓝牙配对成功但没反应 = 模块没加载，不是配对问题。")
    return "\n".join(L)


# ---------------------------------------------------------------- 打印扫描

def printer_check(queue: str, driver: str) -> list:
    """打印前检查。

    任务卡住或输出乱码，用户会反复重发——
    越重发队列越堵，最后完全打不出来。
    """
    problems = []
    if not queue:
        problems.append("没有指定打印队列")
    if not driver:
        problems.append("没有指定驱动——"
                        "队列建了但驱动没配对，任务会卡住或输出乱码")
    return problems


def printer_setup(name: str, uri: str, driver: str = "") -> list:
    return [
        f"lpadmin -p {name} -E -v {uri}"
        + (f" -m {driver}" if driver else " -m everywhere"),
        f"lpoptions -d {name}      # 设为默认",
        f"lpstat -p {name}         # 确认队列就绪",
        "# 驱动没配对时任务会卡住或输出乱码，",
        "# 用户会反复重发，最后队列堵死",
    ]


def scanner_check() -> list:
    """扫描仪检查。"""
    problems = []
    import shutil
    if not shutil.which("scanimage"):
        problems.append("没有 sane-utils（scanimage）——扫描仪无法使用。"
                        "执行 qypkg install sane-utils")
    from . import udev as U
    # 扫描仪走 USB，权限不对会"找得到设备但扫不了"
    problems += [x for x in U.group_check(Path("/"))
                 if "scanner" in x]
    return problems


# ---------------------------------------------------------------- 多显示器

def multiscreen_cmd(layout: str = "extend",
                    external: str = "HDMI-A-1",
                    internal: str = "eDP-1",
                    pos: str = "right") -> list:
    """多显示器配置。"""
    if layout == "extend":
        return [f"# 扩展：外接在内置{'右' if pos=='right' else '左'}侧",
                f"wlr-randr --output {external} --on "
                f"--{'right' if pos=='right' else 'left'} {internal}"
                if _wayland() else
                f"xrandr --output {external} --auto "
                f"--{'right-of' if pos=='right' else 'left-of'} {internal}"]
    if layout == "mirror":
        return [f"# 镜像：演示场景用，两边分辨率会被拉成一致",
                f"wlr-randr --output {external} --on --same-as {internal}"
                if _wayland() else
                f"xrandr --output {external} --same-as {internal}"]
    if layout == "external-only":
        return ["# 只用外接（笔记本合盖场景）",
                f"wlr-randr --output {internal} --off"
                if _wayland() else
                f"xrandr --output {internal} --off"]
    raise IoError(f"不支持的布局 {layout}")


def _wayland() -> bool:
    import os
    return os.environ.get("WAYLAND_DISPLAY") is not None


def multiscreen_problems() -> list:
    """多显示器常见问题。"""
    problems = []
    p = Path("/sys/class/drm")
    if p.exists():
        conn = [d.name for d in p.iterdir() if d.name.startswith("card")]
        if not conn:
            problems.append("没有检测到 DRM 输出——外接显示器不会亮，"
                            "常被误判为接口坏了")
    return problems


def edid_problem_hint() -> str:
    return ("外接显示器不亮的排查顺序：\n"
            "  1. 看内核有没有检测到连接：for p in /sys/class/drm/*/status;"
            " do echo $p: $(cat $p); done\n"
            "  2. status 是 disconnected 但线插着 → 线或接口问题\n"
            "  3. status 是 connected 但不亮 → 布局没配（见 multiscreen）\n"
            "  4. 分辨率异常低 → EDID 读不到，回退到 1024x768")


# ---------------------------------------------------------------- 存储外设

def storage_check() -> list:
    """可移动存储检查。"""
    problems = []
    import shutil
    # SD 读卡器
    if not Path("/sys/class/mmc_host").exists() or \
            not any(Path("/sys/class/mmc_host").iterdir()):
        problems.append("没有检测到 SD 读卡器控制器——"
                        "SD 卡插上不会有任何反应，也不会报错")
    for tool, use in (("udisksctl", "自动挂载"),
                      ("lsblk", "查看设备")):
        if not shutil.which(tool):
            problems.append(f"缺少 {tool}（用于{use}）")
    return problems


def mount_cmd(dev: str, label: str = "") -> list:
    """挂载可移动存储。"""
    name = label or Path(dev).name
    return [f"udisksctl mount -b {dev}     # 自动选点，无需 root",
            f"# 或手动：mkdir -p /media/{name} && mount {dev} /media/{name}",
            f"# 用完：udisksctl unmount -b {dev}"
            f"（不卸载直接拔会损坏数据）"]


def raid_check() -> list:
    """RAID 检查。

    顺序刻意为之：**先看阵列有没有降级**，再看工具装没装。
    降级意味着再坏一块就丢数据，比"少了个管理工具"紧急得多。
    先报工具缺失的话，真正的降级告警会被淹没在一条提示里。
    """
    problems = []
    import shutil
    try:
        t = Path("/proc/mdstat").read_text()
    except OSError:
        # 没有 mdstat 就没有软 RAID，此时才说工具的事
        if not shutil.which("mdadm"):
            problems.append("没有 RAID 阵列，也未安装 mdadm——"
                            "若要组建软 RAID，执行 qypkg install mdadm")
        return problems
    has_degraded = False
    for line in t.splitlines():
        if "degraded" in line.lower():
            problems.append(f"阵列降级：{line.strip()}"
                            f"——再坏一块就会丢数据，尽快换盘")
            has_degraded = True
        if re.search(r"\[_+", line):
            problems.append(f"阵列中有缺失设备：{line.strip()}"
                            f"——冗余已失效，再坏一块就会丢数据，尽快换盘")
            has_degraded = True
    if not has_degraded and not shutil.which("mdadm"):
        # 阵列正常但没工具：这才轮到说工具的事
        problems.append("阵列状态正常，但未安装 mdadm——"
                        "出问题时无法手动修复。执行 qypkg install mdadm")
    return problems


# ---------------------------------------------------------------- 安全芯片

def tpm_report() -> str:
    L = ["TPM 安全芯片：", ""]
    devs = sorted(Path("/dev").glob("tpm*")) if Path("/dev").exists() else []
    if not devs:
        L.append("  ! 没有 /dev/tpm* —— 内核未启用 TPM 或机器没有芯片")
        L.append("    需要模块：tpm_tis / tpm_crb")
        L.append("    没有 TPM 时全盘加密只能靠口令，无法防暴力破解")
        return "\n".join(L)
    L.append(f"  设备: {'、'.join(d.name for d in devs)}")
    L.append("  权限: 应属 tss 组 0660——")
    L.append("         权限过宽等于绕过硬件密钥保护")
    return "\n".join(L)


def smartcard_report() -> str:
    L = ["智能卡：", ""]
    import shutil
    if not shutil.which("pcsc_scan"):
        L.append("  ! 没有 pcsc-lite（pcsc_scan）")
        L.append("    执行 qypkg install pcsc-lite ccid")
        return "\n".join(L)
    L.append("  已安装 pcsc-lite")
    L.append("  # 读卡器插着但读不出：ccid 驱动不支持该型号，")
    L.append("  # 或 pcscd 服务没启动（qyinit status pcscd）")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyio",
                                 description="LANXIU Linux 输入设备与外设")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("touchpad", help="触摸板诊断")
    sp.add_argument("--devices-file")
    sp = sub.add_parser("tablet", help="数位板")
    sp.add_argument("--brand", default="")
    sub.add_parser("gamepad", help="手柄")
    sp = sub.add_parser("printer", help="打印机配置")
    sp.add_argument("name"); sp.add_argument("uri")
    sp.add_argument("--driver", default="")
    sub.add_parser("scanner", help="扫描仪检查")
    sp = sub.add_parser("screen", help="多显示器")
    sp.add_argument("--layout", default="extend",
                    choices=["extend", "mirror", "external-only"])
    sp.add_argument("--external", default="HDMI-A-1")
    sp.add_argument("--internal", default="eDP-1")
    sub.add_parser("screen-hint", help="不亮时的排查顺序")
    sub.add_parser("storage", help="可移动存储")
    sp = sub.add_parser("mount", help="挂载命令")
    sp.add_argument("dev")
    sp.add_argument("--label", default="")
    sub.add_parser("raid", help="RAID 检查")
    sub.add_parser("tpm", help="TPM")
    sub.add_parser("smartcard", help="智能卡")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "touchpad":
            if a.devices_file:
                devs = parse_input_devices(Path(a.devices_file).read_text())
                print(touchpad_diagnose(devs))
            else:
                try:
                    devs = parse_input_devices(
                        Path("/proc/bus/input/devices").read_text())
                    print(touchpad_diagnose(devs))
                except OSError:
                    print("读不到 /proc/bus/input/devices——"
                          "容器里常见，或内核未开 CONFIG_INPUT")
            return 0
        if a.cmd == "tablet":
            print(tablet_diagnose(a.brand))
            return 0
        if a.cmd == "gamepad":
            print(gamepad_diagnose())
            return 0
        if a.cmd == "printer":
            probs = printer_check(a.name, a.driver)
            for x in probs:
                util.log("warn", x)
            print("\n".join(printer_setup(a.name, a.uri, a.driver)))
            return 0
        if a.cmd == "scanner":
            probs = scanner_check()
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "扫描仪可用")
            return 1 if probs else 0
        if a.cmd == "screen":
            print("\n".join(multiscreen_cmd(a.layout, a.external,
                                            a.internal)))
            for x in multiscreen_problems():
                util.log("warn", x)
            return 0
        if a.cmd == "screen-hint":
            print(edid_problem_hint())
            return 0
        if a.cmd == "storage":
            probs = storage_check()
            for x in probs:
                util.log("warn", x)
            if not probs:
                util.log("ok", "可移动存储就绪")
            return 0
        if a.cmd == "mount":
            print("\n".join(mount_cmd(a.dev, a.label)))
            return 0
        if a.cmd == "raid":
            probs = raid_check()
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "RAID 状态正常")
            return 1 if probs else 0
        if a.cmd == "tpm":
            print(tpm_report())
            return 0
        if a.cmd == "smartcard":
            print(smartcard_report())
            return 0
    except IoError as e:
        util.log("err", str(e))
        return 1
    return 1
