"""硬件支持：设备识别、驱动匹配、固件缺失检测。

装机后最常遇到的问题不是"装不上"，而是"装完了网卡不工作、
触摸屏没反应、蓝牙搜不到设备"。这些问题有一个共同点：
**不报错**。设备就在那儿，就是不工作。

本模块做的事：
1. 建立设备 ID → 驱动 / 固件的映射数据库
2. 扫描本机实际设备，比对数据库，找出"有设备但缺固件/缺驱动"
3. 按类别（网卡/无线/蓝牙/显卡/存储/触摸/声卡/摄像头）分组报告
4. 给出具体的修复命令

**为什么必须有这个数据库**

内核知道自己要什么固件（会打日志 request_firmware failed），
但用户看的是"连不上网"，不是 dmesg。把"内核要 xxx.bin"
翻译成"你的 Intel AX201 网卡缺 iwlwifi 固件，执行 xxx"，
这中间的翻译层就是本模块。

**固件缺失必须按类别分优先级**
缺显卡固件：还能进命令行，能修。
缺网卡固件：连不上网，装不了任何东西——**必须先修**。
缺声卡固件：不影响使用。
所以报告要按"能不能自救"排序。
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class HwError(RuntimeError):
    pass


# 类别的严重度：决定了报告顺序和告警级别。
# 网卡最高——缺了它连不上网，装不了任何东西，无法自救
SEVERITY = {
    "net": 0,       # 有线网卡：缺了上不了网，无法自救
    "wifi": 1,      # 无线：笔记本唯一联网途径
    "storage": 2,   # 存储控制器：认不到盘，系统起不来
    "gpu": 3,       # 显卡：能进命令行，可救
    "bluetooth": 4, # 蓝牙：不影响使用
    "touch": 5,     # 触摸屏：能用鼠标替代
    "audio": 6,     # 声卡：不影响使用
    "camera": 7,    # 摄像头：不影响使用
}

SEVERITY_DESC = {
    "net": "上不了网，也就装不了任何东西——必须先修",
    "wifi": "笔记本通常只有无线一条联网途径",
    "storage": "认不到盘，系统可能起不来",
    "gpu": "进不了图形界面，但能进命令行自救",
    "bluetooth": "不影响基本使用",
    "touch": "可用鼠标替代",
    "audio": "不影响基本使用",
    "camera": "不影响基本使用",
}

CATEGORY_CN = {
    "net": "有线网卡", "wifi": "无线网卡", "storage": "存储控制器",
    "gpu": "显卡", "bluetooth": "蓝牙", "touch": "触摸屏/触控板",
    "audio": "声卡", "camera": "摄像头", "other": "其他",
}


@dataclass
class Device:
    """一个 PCI/USB 设备。"""
    category: str
    vendor: str          # 厂商 ID，如 8086
    device: str          # 设备 ID
    name: str = ""       # 可读名称
    driver: str = ""     # 当前绑定的驱动
    firmware_needed: str = ""   # 需要的固件目录名
    bus: str = "pci"

    @property
    def key(self) -> str:
        return f"{self.vendor}:{self.device}"

    def describe(self) -> str:
        n = self.name or f"{self.vendor}:{self.device}"
        d = f"（驱动 {self.driver}）" if self.driver else "（无驱动）"
        return f"{n}{d}"


# 已知设备库：厂商:设备 → (名称, 类别, 固件目录/驱动)
# 只收录最常见的，够装机自检用；未收录的按类别前缀推断
KNOWN: dict = {
    # Intel 无线
    "8086:2723": ("Intel Wi-Fi 6 AX200", "wifi", "iwlwifi"),
    "8086:2725": ("Intel Wi-Fi 6 AX210", "wifi", "iwlwifi"),
    "8086:51f0": ("Intel Wi-Fi 6E AX211", "wifi", "iwlwifi"),
    "8086:7af0": ("Intel Wi-Fi 7 BE200", "wifi", "iwlwifi"),
    "8086:24fd": ("Intel Wireless 8265", "wifi", "iwlwifi"),
    "8086:095a": ("Intel Wireless 7265", "wifi", "iwlwifi"),
    # Intel 有线
    "8086:15b8": ("Intel Ethernet I219-V", "net", "e1000e"),
    "8086:1570": ("Intel Ethernet I219-LM", "net", "e1000e"),
    "8086:153a": ("Intel Ethernet I217-LM", "net", "e1000e"),
    # Realtek
    "10ec:8168": ("Realtek RTL8168 千兆网卡", "net", "rtl_nic"),
    "10ec:8125": ("Realtek RTL8125 2.5G 网卡", "net", "rtl_nic"),
    "10ec:c822": ("Realtek RTL8822CE 无线", "wifi", "rtw88"),
    "10ec:c852": ("Realtek RTL8852AE 无线", "wifi", "rtw89"),
    "10ec:b852": ("Realtek RTL8852BE 无线", "wifi", "rtw89"),
    # AMD 显卡
    "1002:15d8": ("AMD Raven Ridge 核显", "gpu", "amdgpu"),
    "1002:163f": ("AMD Renoir 核显", "gpu", "amdgpu"),
    "1002:73bf": ("AMD Radeon RX 6700 XT", "gpu", "amdgpu"),
    # Intel 显卡
    "8086:5916": ("Intel HD Graphics 620", "gpu", "i915"),
    "8086:3e92": ("Intel UHD Graphics 630", "gpu", "i915"),
    "8086:9a49": ("Intel Iris Xe Graphics", "gpu", "i915"),
    # NVIDIA
    "10de:2484": ("NVIDIA RTX 3060", "gpu", "nouveau"),
    "10de:2204": ("NVIDIA RTX 3080", "gpu", "nouveau"),
    # 存储
    "144d:a808": ("Samsung NVMe SSD", "storage", ""),
    "8086:f1a6": ("Intel NVMe 控制器", "storage", ""),
    "1b4b:9215": ("Marvell SATA 控制器", "storage", ""),
}

# 类别前缀推断：设备库里没有时，按厂商/设备名关键词猜
INFER_RULES = [
    (r"(?i)wireless|wi-?fi|wlan|802\.11|ax[0-9]{3}|rtl88|ath1[012]k", "wifi"),
    (r"(?i)ethernet|ethernet controller|rtl81|e1000|igb|ixgbe|tg3|r8169", "net"),
    (r"(?i)vga|display|graphics|radeon|geforce|iris|uhd", "gpu"),
    (r"(?i)bluetooth|bt ", "bluetooth"),
    (r"(?i)nvme|sata|sas|raid|storage|ahci", "storage"),
    (r"(?i)audio|sound|alc[0-9]|hda", "audio"),
    (r"(?i)camera|webcam|video capture|uvc", "camera"),
    (r"(?i)touch ?screen|touchpad|touch ?panel|elan|synaptics", "touch"),
]


def infer_category(text: str) -> str:
    for pat, cat in INFER_RULES:
        if re.search(pat, text):
            return cat
    return "other"


def parse_lspci(text: str) -> list:
    """解析 lspci -nn 输出。

    格式：`01:00.0 Network controller [0280]: Intel... [8086:2723]`
    """
    out = []
    for line in text.splitlines():
        m = re.search(r"\[([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\]", line)
        if not m:
            continue
        vendor, device = m.group(1).lower(), m.group(2).lower()
        key = f"{vendor}:{device}"
        name, cat, fw = KNOWN.get(key, ("", "", ""))
        if not name:
            # 去掉 ID 部分剩下的就是设备描述
            desc = re.sub(r"\[[0-9a-fA-F]{4}:[0-9a-fA-F]{4}\]", "", line)
            desc = re.sub(r"^\S+\s", "", desc).strip()
            desc = re.sub(r"\[[0-9a-fA-F]{4}\]$", "", desc).strip()
            name = desc
            cat = infer_category(desc)
        out.append(Device(cat, vendor, device, name, firmware_needed=fw))
    return out


def parse_lsusb(text: str) -> list:
    """解析 lsusb 输出：`Bus 001 Device 002: ID 8087:0026 Intel Corp.`"""
    out = []
    for line in text.splitlines():
        m = re.search(r"ID\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s*(.*)$", line)
        if not m:
            continue
        vendor, device, desc = m.group(1).lower(), m.group(2).lower(), m.group(3).strip()
        cat = infer_category(desc)
        out.append(Device(cat, vendor, device, desc or f"{vendor}:{device}",
                          bus="usb"))
    return out


def firmware_present(root: Path) -> set:
    """列出系统已装的固件目录。"""
    d = Path(root) / "usr" / "lib" / "firmware"
    if not d.exists():
        return set()
    return {p.name for p in d.iterdir() if p.is_dir()}


@dataclass
class Report:
    devices: list = field(default_factory=list)
    missing: list = field(default_factory=list)

    def by_severity(self) -> list:
        """按严重度排序：网卡最前，因为它缺了无法自救。"""
        return sorted(self.missing,
                      key=lambda d: (SEVERITY.get(d.category, 9), d.name))


def scan(root: Path = Path("/"), lspci: str = "", lsusb: str = "") -> Report:
    """扫描设备并比对固件。

    root 用于查已装固件；lspci/lsusb 是命令输出文本
    （传文本而不是直接调命令，是为了可测试）。
    """
    devs = []
    if lspci:
        devs += parse_lspci(lspci)
    if lsusb:
        devs += parse_lsusb(lsusb)

    have = firmware_present(root)
    missing = []
    for d in devs:
        # 只有声明了需要固件的才检查；没声明的不瞎报——
        # 误报会让用户忽略真正的缺失
        if d.firmware_needed and d.firmware_needed not in have:
            missing.append(d)
    return Report(devices=devs, missing=missing)


def scan_report(r: Report, root: Path = Path("/")) -> str:
    if not r.devices:
        return "没有检测到设备（可能没有运行 lspci/lsusb，或在容器里）。"
    L = [f"检测到 {len(r.devices)} 个设备：", ""]
    from collections import Counter
    cnt = Counter(d.category for d in r.devices)
    for cat, n in sorted(cnt.items(), key=lambda x: SEVERITY.get(x[0], 9)):
        L.append(f"  {CATEGORY_CN.get(cat, cat):<12} {n} 个")
    if r.missing:
        L.append("")
        L.append(f"缺少固件 {len(r.missing)} 项（按紧急程度排序）：")
        for d in r.by_severity():
            L.append(f"  ! {d.describe()}")
            L.append(f"    类别：{CATEGORY_CN.get(d.category, d.category)}"
                     f" — {SEVERITY_DESC.get(d.category, '')}")
            L.append(f"    需要：/usr/lib/firmware/{d.firmware_needed}")
            L.append(f"    修复：qypkg install linux-firmware")
    else:
        L.append("")
        L.append("所有需要固件的设备都已就位。")
    return "\n".join(L)


def check_root_cause(missing: list) -> list:
    """给出为什么缺、怎么修。缺固件的根因通常是同一个。"""
    if not missing:
        return []
    cats = sorted({d.category for d in missing},
                  key=lambda c: SEVERITY.get(c, 9))
    out = []
    # 一次性说清根因，不要每个设备重复一遍
    out.append(
        f"根因：{len(missing)} 个设备缺固件，都来自同一个包 linux-firmware。"
        f"一次安装即可全部解决：")
    out.append("  qypkg install linux-firmware")
    if "net" in cats or "wifi" in cats:
        out.append("")
        out.append(
            "其中网卡/无线优先：没有网络就装不了任何东西。"
            "如果这台机器现在上不了网，用另一台机器下载"
            " linux-firmware 包，用 U 盘拷过来离线安装：")
        out.append("  qypkg install ./linux-firmware-*.qyp")
    return out


def main_cli(argv=None) -> int:
    import argparse
    import subprocess

    ap = argparse.ArgumentParser(prog="qyhw",
                                 description="澜岫 Linux 硬件支持与固件检测")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan", help="扫描设备并检查固件")
    sub.add_parser("devices", help="只列出设备")
    sp = sub.add_parser("demo", help="用内置样例演示（无需真机）")
    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "demo":
        # 演示用样例：一台典型的 Intel 笔记本
        sample = """00:02.0 VGA compatible controller [0300]: Intel Corporation Iris Xe [8086:9a49]
00:14.0 USB controller [0c03]: Intel Corporation [8086:51ed]
00:1f.6 Ethernet controller [0200]: Intel Corporation Ethernet I219-V [8086:15b8]
01:00.0 Network controller [0280]: Intel Corporation Wi-Fi 6 AX210 [8086:2725]
02:00.0 Non-Volatile memory controller [0108]: Samsung NVMe SSD [144d:a808]
"""
        r = scan(root, lspci=sample)
        print(scan_report(r, root))
        for line in check_root_cause(r.missing):
            print(line)
        return 0

    def run(cmd: list) -> str:
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=20).stdout
        except Exception:
            return ""

    if a.cmd in ("scan", "devices"):
        r = scan(root, lspci=run(["lspci", "-nn"]), lsusb=run(["lsusb"]))
        if a.cmd == "devices":
            for d in r.devices:
                print(f"  {CATEGORY_CN.get(d.category, d.category):<12}"
                      f"{d.describe()}")
            return 0
        print(scan_report(r, root))
        for line in check_root_cause(r.missing):
            print(line)
        return 1 if r.missing else 0
    return 1
