"""设备节点权限与 udev 规则。

硬件接上了、模块加载了、设备节点也出现了——但普通用户打不开它，
于是"设备存在但用不了"。这就是缺设备权限管理层的结果。

典型场景：
- 普通用户访问不了 /dev/video0，相机打不开
- 串口 /dev/ttyUSB0 属于 root:dialout，用户不在组里
- USB 设备拔插后权限变了，上次能用这次不能
- 安卓刷机时 adb 没权限（需要 udev 规则）
- 数位板/游戏手柄需要特定组

几件事必须做对：

**1. 设备节点必须按类别归属固定的组**
video / audio / dialout / input / plugdev / lp（打印机）/ scanner。
不归组就只能靠 chmod 777，那等于所有用户都能访问所有设备。

**2. 规则要按设备的稳定标识匹配，不按 /dev 名**
/dev/video0 在插入第二个摄像头后会变。按设备名写的规则会失效，
且症状是"有时能用有时不能"——最难排查的一类问题。
所以按 idVendor/idProduct 或序列号匹配。

**3. 拔插后权限要保持一致**
udev 规则没写对，重新插拔权限就变了。用户会说
"昨天还好好的"，其实是规则没生效。

**4. 危险的放宽要明确警告**
把设备设成 0666（所有人可读写）能"解决"所有权限问题，
代价是任何用户都能读你的摄像头。
所以生成规则时不默认给 0666。

**5. 规则改动后必须提醒重载**
写了规则不重载，规则不生效。用户会以为写了没用。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class UdevError(RuntimeError):
    pass


# ---------------------------------------------------------------- 类别

@dataclass
class DevClass:
    """一类设备的权限归属。"""
    id: str
    title: str
    group: str         # 归属的组
    mode: str          # 权限位
    patterns: list     # 匹配的设备节点模式
    desc: str = ""


# 组的用途。不归组就只能 777，等于所有用户都能访问所有设备
DEV_CLASSES = [
    DevClass("video", "摄像头/视频", "video", "0660",
             ["/dev/video*", "/dev/v4l/*", "/dev/media*"],
             "相机、采集卡"),
    DevClass("audio", "声音", "audio", "0660",
             ["/dev/snd/*", "/dev/dsp*", "/dev/mixer*"],
             "声卡、麦克风"),
    DevClass("serial", "串口", "dialout", "0660",
             ["/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*"],
             "Arduino、GPS 模块、串口调试"),
    DevClass("input", "输入设备", "input", "0640",
             ["/dev/input/*"],
             "键盘、鼠标、触摸板、手柄。权限给太宽可被键盘记录"),
    DevClass("printer", "打印机", "lp", "0660",
             ["/dev/usb/lp*", "/dev/lp*"],
             "USB 与并口打印机"),
    DevClass("scanner", "扫描仪", "scanner", "0660",
             ["/dev/bus/usb/*/*"],
             "扫描仪走 USB，按 VID/PID 匹配"),
    DevClass("storage", "可移动存储", "plugdev", "0660",
             ["/dev/sd*[1-9]*", "/dev/mmcblk*"],
             "U 盘、SD 卡"),
    DevClass("tpm", "安全芯片", "tss", "0660",
             ["/dev/tpm*", "/dev/tpmrm*"],
             "TPM。权限过宽等于绕过硬件密钥保护"),
    DevClass("dvb", "电视卡", "video", "0660",
             ["/dev/dvb/*"],
             "数字电视接收"),
    DevClass("raw-gpio", "GPIO", "gpio", "0660",
             ["/dev/gpiochip*"],
             "嵌入式 GPIO。可控制物理设备，权限要紧"),
    DevClass("i2c", "I2C 总线", "i2c", "0660",
             ["/dev/i2c-*"],
             "传感器、EEPROM 走这条总线"),
    DevClass("spidev", "SPI", "spi", "0660",
             ["/dev/spidev*"],
             "SPI 外设"),
    DevClass("watchdog", "看门狗", "root", "0600",
             ["/dev/watchdog*"],
             "误触发会重启机器，只给 root"),
]

CLASS_BY_ID = {c.id: c for c in DEV_CLASSES}

# 系统应当存在的组。缺组的话规则里的 GROUP= 不生效，
# 且症状是"规则写了但没用"
REQUIRED_GROUPS = sorted({c.group for c in DEV_CLASSES})


# ---------------------------------------------------------------- 规则生成

def rule_for_class(c: DevClass, comment: str = "") -> str:
    """按类别生成规则。"""
    L = []
    if comment:
        L.append(f"# {comment}")
    for p in c.patterns:
        L.append(f'KERNEL=="{p.split("/")[-1]}", '
                 f'GROUP="{c.group}", MODE="{c.mode}"')
    return "\n".join(L)


def rule_for_usb(vid: str, pid: str, group: str = "plugdev",
                 mode: str = "0660", comment: str = "") -> str:
    """按 USB VID/PID 生成规则。

    按 /dev 名匹配在插入第二个同类设备后会失效，
    症状是"有时能用有时不能"——最难排查的一类。
    所以按 VID/PID 匹配。
    """
    vid, pid = vid.lower().strip(), pid.lower().strip()
    if not re.fullmatch(r"[0-9a-f]{4}", vid):
        raise UdevError(f"厂商 ID 要是 4 位十六进制，收到 {vid}")
    if not re.fullmatch(r"[0-9a-f]{4}", pid):
        raise UdevError(f"产品 ID 要是 4 位十六进制，收到 {pid}")
    L = []
    if comment:
        L.append(f"# {comment}")
    L.append(f'SUBSYSTEM=="usb", ATTR{{idVendor}}=="{vid}", '
             f'ATTR{{idProduct}}=="{pid}", GROUP="{group}", MODE="{mode}"')
    return "\n".join(L)


# 常见需要 udev 规则的设备。adb 是最典型的一个：
# 没有规则时普通用户跑 adb 会一直停在 unauthorized
KNOWN_USB = {
    "adb": ("Android 调试", "18d1", "4ee7", "plugdev"),
    "fastboot": ("Android 刷机", "18d1", "d00d", "plugdev"),
    "arduino": ("Arduino", "2341", "*", "dialout"),
    "esp32": ("ESP32/CP210x", "10c4", "ea60", "dialout"),
    "ch340": ("CH340 串口", "1a86", "7523", "dialout"),
    "ftdi": ("FTDI 串口", "0403", "6001", "dialout"),
    "wch": ("CH9102/CH343", "1a86", "55d4", "dialout"),
}


def known_usb_rule(kind: str) -> str:
    if kind not in KNOWN_USB:
        raise UdevError(f"没有预置 {kind}"
                        f"（可用：{'、'.join(sorted(KNOWN_USB))}）")
    title, vid, pid, group = KNOWN_USB[kind]
    pat = f'ATTR{{idProduct}}=="{pid}"' if pid != "*" else ""
    L = [f"# {title}"]
    if pid == "*":
        L.append(f'SUBSYSTEM=="usb", ATTR{{idVendor}}=="{vid}", '
                 f'GROUP="{group}", MODE="0660"')
    else:
        L.append(f'SUBSYSTEM=="usb", ATTR{{idVendor}}=="{vid}", '
                 f'ATTR{{idProduct}}=="{pid}", '
                 f'GROUP="{group}", MODE="0660"')
    # adb 单独一条：没有它普通用户跑 adb 会一直 unauthorized
    if kind in ("adb", "fastboot"):
        L.append(f'SUBSYSTEM=="usb", ATTR{{idVendor}}=="{vid}", '
                 f'ATTR{{idProduct}}=="{pid}", '
                 f'TAG+="uaccess"')
    return "\n".join(L)


# ---------------------------------------------------------------- 检查

def check_mode_too_open(mode: str) -> str:
    """检查权限是否过宽。

    0666 能"解决"所有权限问题，代价是任何用户都能读你的摄像头。
    """
    try:
        m = int(mode, 8)
    except ValueError:
        raise UdevError(f"权限位要是八进制，收到 {mode}")
    if m & 0o006:      # 其他用户有 rw
        return (f"{mode} 允许任何用户读写该设备——"
                f"等于所有人都能读你的摄像头/麦克风。"
                f"改成 0660 并把用户加入对应组")
    return ""


def rules_path(root: Path) -> Path:
    return Path(root) / "etc" / "udev" / "rules.d" / "70-qiyuan.rules"


def write_rules(root: Path, blocks: list) -> Path:
    p = rules_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    head = ("# 由LANXIU Linux 生成\n"
            "# 修改后需执行：udevadm control --reload && "
            "udevadm trigger\n"
            "# 不重载的话规则不生效，会以为是规则写错了\n\n")
    util.atomic_write(p, (head + "\n\n".join(blocks) + "\n").encode())
    return p


def reload_cmd() -> str:
    return ("udevadm control --reload && udevadm trigger\n"
            "# 不重载规则不生效——会误以为是规则写错了")


def group_check(root: Path) -> list:
    """检查规则里引用的组是否存在。

    缺组的话 GROUP= 不生效，症状是"规则写了但没用"。
    """
    problems = []
    try:
        have = {g.split(":")[0]
                for g in Path("/etc/group").read_text().splitlines() if g}
    except OSError:
        return problems
    for c in DEV_CLASSES:
        if c.group not in have and c.group != "root":
            problems.append(
                f"组 {c.group} 不存在——{c.title} 的规则里 GROUP="
                f" 不会生效，表现为'规则写了但没用'。"
                f"创建：groupadd {c.group}")
    return problems


def user_in_groups(groups: list) -> list:
    """查当前用户是否在所需组里。"""
    import os
    import grp
    try:
        me = os.getlogin()
    except OSError:
        me = ""
    if not me:
        return []
    out = []
    for g in groups:
        try:
            gr = grp.getgrnam(g)
        except KeyError:
            continue
        if me not in gr.gr_mem:
            out.append((g, me))
    return out


def access_report(root: Path = Path("/")) -> str:
    L = ["设备权限：", ""]
    missing = group_check(root)
    if missing:
        for m in missing:
            L.append(f"  ! {m}")
        L.append("")
    else:
        L.append("  所需组都已存在")
    L.append("")
    L.append("  当前用户缺失的组：")
    need = [c.group for c in DEV_CLASSES]
    miss = user_in_groups(need)
    if miss:
        for g, me in miss:
            cls = [c.title for c in DEV_CLASSES if c.group == g]
            L.append(f"  ! {g}（{cls[0] if cls else ''}）"
                     f" — 加入：usermod -aG {g} {me}")
        L.append("")
        L.append("  # 加入组后需重新登录才生效，")
        L.append("  # 不重新登录会以为命令没起作用")
    else:
        L.append("  无（或无法确定当前用户）")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyudev",
                                 description="LANXIU Linux 设备节点权限")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("classes", help="设备类别与归属组")
    sub.add_parser("access", help="当前用户的设备权限")
    sp = sub.add_parser("rule", help="生成类别规则")
    sp.add_argument("cls", choices=[c.id for c in DEV_CLASSES])
    sp = sub.add_parser("usb", help="生成 USB 规则")
    sp.add_argument("vid")
    sp.add_argument("pid")
    sp.add_argument("--group", default="plugdev")
    sp.add_argument("--mode", default="0660")
    sp = sub.add_parser("known", help="预置 USB 规则")
    sp.add_argument("kind", choices=sorted(KNOWN_USB))
    sp = sub.add_parser("gen", help="写入全部类别规则")
    sp.add_argument("--classes", nargs="*", default=[])
    sub.add_parser("reload", help="重载命令")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "classes":
            for c in DEV_CLASSES:
                print(f"  {c.id:<12}{c.title:<12}{c.group:<10}"
                      f"{c.mode}  {c.desc}")
            return 0
        if a.cmd == "access":
            print(access_report(root))
            return 0
        if a.cmd == "rule":
            c = CLASS_BY_ID[a.cls]
            print(rule_for_class(c))
            w = check_mode_too_open(c.mode)
            if w:
                util.log("warn", w)
            return 0
        if a.cmd == "usb":
            print(rule_for_usb(a.vid, a.pid, a.group, a.mode))
            w = check_mode_too_open(a.mode)
            if w:
                util.log("warn", w)
            util.log("info", "按 VID/PID 匹配——"
                             "按 /dev 名匹配在插入第二个同类设备后会失效")
            return 0
        if a.cmd == "known":
            print(known_usb_rule(a.kind))
            if a.kind == "adb":
                util.log("info", "没有这条规则，普通用户跑 adb "
                                 "会一直停在 unauthorized")
            return 0
        if a.cmd == "gen":
            want = [CLASS_BY_ID[x] for x in a.classes] if a.classes \
                else DEV_CLASSES
            blocks = [rule_for_class(c, c.desc) for c in want]
            p = write_rules(root, blocks)
            util.log("ok", f"已写入 {p}")
            print(reload_cmd())
            for m in group_check(root):
                util.log("warn", m)
            return 0
        if a.cmd == "reload":
            print(reload_cmd())
            return 0
    except UdevError as e:
        util.log("err", str(e))
        return 1
    return 1
