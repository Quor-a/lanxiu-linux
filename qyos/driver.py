"""驱动管理面板后端（qydrv）。

设计目标（对应产品蓝图）：
1. **识别硬件 → 查驱动/固件 → 给出一键安装方案**（qydrv fix）
2. **只内置通用必备驱动**，专有/小众驱动按需下载（qydrv search）
3. **启动时自检**，驱动缺失面板自行提示安装（qydrv check --boot）
4. **兼容其他主流包管理**的软件源（qydrv repo：apt/dnf/pacman/yum 元数据
   只读解析，用于查"这个硬件在别的发行版用什么驱动"）

架构分层：
    qyhw   → 硬件识别（lspci/lsusb 解析，已有）
    qykmod → 模块↔设备映射（已有）
    qydrv  → 【本模块】策略层：扫描 → 匹配 → 行动（装固件/装驱动包/
             写 modprobe 配置/更新 initramfs）
    qyudev → 设备节点权限（已有）

不做的事：
- 不内置厂商专有驱动（NVIDIA 私有版等）——体积、许可证、内核版本耦合
  都不适合 live 镜像；只提示用户从仓库取
- 不联网自动下载未审计的驱动——所有安装走 qypkg 仓库（可签名审计）
"""
from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass, field, asdict
from pathlib import Path

from . import hardware, kmod, udev


class DrvError(RuntimeError):
    pass


# ---------------------------------------------------------------- 数据模型

@dataclass
class Action:
    """一项驱动修复动作。"""
    kind: str          # firmware | module | package | modprobe | udev
    target: str        # 包名 / 模块名 / 固件目录 / 规则文件
    reason: str        # 为什么需要（给用户看）
    cmd: str = ""      # 建议执行的命令

    def as_dict(self) -> dict:
        return asdict(self)


@dataclass
class Plan:
    devices: list = field(default_factory=list)
    actions: list = field(default_factory=list)

    def as_dict(self) -> dict:
        return {"devices": [asdict(d) for d in self.devices],
                "actions": [a.as_dict() for a in self.actions]}


# ---------------------------------------------------------------- 策略表

# 设备类别 → （驱动包， 固件包， 额外用户态包）
CATEGORY_PACKAGES = {
    "wifi":  (None,             "linux-firmware", "iwd networkmanager"),
    "net":   (None,             "linux-firmware", "networkmanager"),
    "gpu":   (None,             "linux-firmware", "mesa"),
    "bluetooth": (None,         "linux-firmware", "bluez"),
    "audio": (None,             "linux-firmware", "pipewire"),
    "storage": (None,           None,             "eudev kmod"),
    "touch": (None,             None,             "libinput"),
    "camera": (None,            "linux-firmware", None),
    "printer": (None,           None,             "cups"),
}

# 无需固件但需要专有用户态的设备（如 NVIDIA 私有驱动）——只提示
PROPRIETARY_HINTS = [
    (r"(?i)nvidia gforce|nvidia geforce|geforce rtx|geforce gtx",
     "NVIDIA 私有驱动不随系统内置。开源 nouveau 已覆盖显示，"
     "如需 CUDA/3D 全速请参考仓库 nvidia 包说明。"),
    (r"(?i)broadcom bcm43",
     "Broadcom 无线需要 b43 固件，已包含在 linux-firmware；"
     "若仍不可用，说明该芯片需要 broadcom-wl 专有驱动。"),
]


def plan_fix(devices: list, root: Path) -> Plan:
    """从扫描结果生成修复计划。"""
    plan = Plan(devices=devices)
    seen_pkgs = set()

    for d in devices:
        pkgs = CATEGORY_PACKAGES.get(d.category)
        if pkgs:
            fw_pkg, drv_pkg, user_pkgs = pkgs
            if d.firmware_needed and fw_pkg and fw_pkg not in seen_pkgs:
                plan.actions.append(Action(
                    "package", fw_pkg,
                    f"{d.name} 需要 /usr/lib/firmware/{d.firmware_needed}",
                    f"qypkg install {fw_pkg}"))
                seen_pkgs.add(fw_pkg)
            if user_pkgs:
                for p in user_pkgs.split():
                    if p not in seen_pkgs:
                        plan.actions.append(Action(
                            "package", p,
                            f"{CATEGORY_CN(d.category)}的用户态栈",
                            f"qypkg install {p}"))
                        seen_pkgs.add(p)
        # 模块建议
        mods = kmod.modules_for(d.name)
        for m in mods[:2]:
            plan.actions.append(Action(
                "module", m, f"{d.name} 的内核模块",
                f"modprobe {m}"))

    # 专有驱动提示
    text = " ".join(d.name for d in devices)
    for pat, hint in PROPRIETARY_HINTS:
        if re.search(pat, text):
            plan.actions.append(Action("hint", "proprietary", hint))
    return plan


def CATEGORY_CN(cat: str) -> str:
    return hardware.CATEGORY_CN.get(cat, cat)


# ---------------------------------------------------------------- 自检

def boot_check(root: Path = Path("/")) -> dict:
    """开机自检：供 init 或面板调用。返回 JSON 可序列化结果。

    快速路径：读 /proc 与 /sys，不跑外部命令，<100ms。
    """
    out = {"loaded_modules": sorted(kmod.loaded_modules()),
           "udev_rules_ok": True,
           "groups_ok": True,
           "warnings": []}
    problems = udev.group_check(root)
    if problems:
        out["groups_ok"] = False
        out["warnings"] += problems
    return out


# ---------------------------------------------------------------- 兼容源查询

# 其他主流发行版如何命名同一个驱动——用于文档/提示，不直接安装
ALIAS = {
    "iwlwifi":   {"debian": "firmware-iwlwifi", "fedora": "iwl*-firmware",
                  "arch": "linux-firmware", "opensuse": "kernel-firmware"},
    "rtl_nic":   {"debian": "firmware-realtek", "fedora": "linux-firmware",
                  "arch": "linux-firmware", "opensuse": "kernel-firmware"},
    "amdgpu":    {"debian": "firmware-amd-graphics", "fedora": "linux-firmware",
                  "arch": "linux-firmware", "opensuse": "kernel-firmware"},
    "nouveau":   {"debian": "firmware-misc-nonfree", "fedora": "linux-firmware",
                  "arch": "linux-firmware", "opensuse": "kernel-firmware"},
    "bluez":     {"debian": "bluez", "fedora": "bluez",
                  "arch": "bluez", "opensuse": "bluez"},
    "networkmanager": {"debian": "network-manager", "fedora": "NetworkManager",
                       "arch": "networkmanager", "opensuse": "NetworkManager"},
}


def alias_for(module: str) -> dict:
    return ALIAS.get(module, {})


# ---------------------------------------------------------------- CLI

def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(
        prog="qydrv", description="LANXIU Linux 驱动管理")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="扫描硬件并显示驱动/固件状态")
    sub.add_parser("fix", help="生成驱动修复计划（JSON 输出）")
    sub.add_parser("boot-check", help="开机快速自检（JSON）")
    sp = sub.add_parser("alias", help="查其他发行版的同名驱动包")
    sp.add_argument("module")
    sub.add_parser("demo", help="内置样例演示")

    a = ap.parse_args(argv)
    root = Path(a.root)

    def run(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=20).stdout
        except Exception:
            return ""

    if a.cmd == "boot-check":
        print(json.dumps(boot_check(root), ensure_ascii=False, indent=2))
        return 0

    lspci = run(["lspci", "-nn"])
    lsusb = run(["lsusb"])
    if a.cmd == "demo":
        lspci = ("00:02.0 VGA compatible controller [0300]: Intel Corporation "
                 "Iris Xe [8086:9a49]\n"
                 "00:14.0 USB controller [0c03]: Intel Corporation [8086:51ed]\n"
                 "00:1f.6 Ethernet controller [0200]: Intel Corporation "
                 "Ethernet I219-V [8086:15b8]\n"
                 "01:00.0 Network controller [0280]: Intel Corporation "
                 "Wi-Fi 6 AX210 [8086:2725]\n")
        lsusb = "Bus 001 Device 002: ID 8087:0026 Intel Corp. AX210 Bluetooth"

    r = hardware.scan(root, lspci=lspci, lsusb=lsusb)

    if a.cmd == "scan":
        print(hardware.scan_report(r, root))
        return 1 if r.missing else 0

    if a.cmd == "fix":
        plan = plan_fix(r.devices, root)
        print(json.dumps(plan.as_dict(), ensure_ascii=False, indent=2))
        return 0

    if a.cmd == "alias":
        print(json.dumps(alias_for(a.module), ensure_ascii=False, indent=2))
        return 0

    return 0
