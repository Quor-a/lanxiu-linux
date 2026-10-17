"""内核模块管理：加载、黑名单、参数、依赖与固件联动。

这是硬件支持的根。模块没加载，上面所有抽象都是空的——
设备节点不会出现，框架找不到硬件，应用拿到"设备不存在"。

几个必须做对的点：

**1. 模块名不等于驱动名**
用户看到的是"我的 AX210 网卡"，内核里叫 `iwlwifi`。
不给这层映射，用户根本不知道该加载什么。
所以要有 设备 → 模块 的映射表，并且反查也要能查。

**2. 黑名单是排障手段，不是禁用手段**
nouveau 和 NVIDIA 专有驱动冲突时，很多教程让"禁用 nouveau"。
但黑名单写错了（比如拼错模块名）会静默失效——
模块照常加载，用户以为禁掉了其实没有，然后疑难杂症。
所以黑名单必须校验模块名真实存在。

**3. 模块参数要持久化到 /etc/modprobe.d/**
只在命令行 modprobe 传参，重启就没了。
用户会以为"设置了没生效"。

**4. 固件缺失要在这里就说**
模块加载成功但固件缺失，表现为 `dmesg: failed to load xxx.bin`，
而 lsmod 里模块是在的——用户看到的是"驱动装了但不工作"。
所以加载后要检查固件，而不是只看 lsmod。

**5. initramfs 里的模块要单独管**
早期启动需要的模块（磁盘控制器、文件系统）不在 initramfs 里，
系统直接起不来，且报错是"找不到根设备"，完全看不出是模块问题。
"""
from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class KmodError(RuntimeError):
    pass


# ---------------------------------------------------------------- 设备→模块

# 设备描述关键词 → 内核模块。
# 用户看到的是设备名，内核里是模块名，中间这层翻译必须有人做
DEVICE_MODULES = {
    # 无线
    "iwlwifi": ["intel wireless", "ax200", "ax210", "ax211", "be200",
                "8265", "7265", "intel wi-fi"],
    "rtw88": ["rtl8822", "rtl8821", "rtl8723"],
    "rtw89": ["rtl8852", "rtl8851", "rtl8922"],
    "ath11k": ["qca6390", "qca6490", "wcn685"],
    "ath10k": ["qca6174", "qca9377"],
    "mt76": ["mt7921", "mt7922", "mt7615", "mediatek wi-fi"],
    "brcmfmac": ["bcm43", "broadcom wireless", "brcm"],
    # 有线
    "e1000e": ["i219", "i217", "intel ethernet", "82579"],
    "igb": ["i210", "i211", "82576"],
    "r8169": ["rtl8168", "rtl8169", "realtek ethernet"],
    "r8125": ["rtl8125"],
    "tg3": ["broadcom netxtreme", "bcm57"],
    # 显卡
    "i915": ["intel hd graphics", "intel uhd", "iris xe", "intel graphics"],
    "amdgpu": ["radeon", "amd radeon", "navi", "polaris", "vega", "rdna"],
    "nouveau": ["nvidia", "geforce", "quadro", "rtx", "gtx"],
    "nvidia": ["nvidia", "geforce", "rtx", "gtx"],
    # 存储
    "nvme": ["nvme", "non-volatile memory"],
    "ahci": ["sata", "ahci"],
    "xhci_hcd": ["usb 3", "xhci"],
    # 输入
    "hid_multitouch": ["touchscreen", "touch screen", "hid touch"],
    "psmouse": ["ps/2 mouse", "synaptics", "elan touchpad"],
    "i2c_hid_acpi": ["i2c hid", "touchpad"],
    # 蓝牙
    "btusb": ["bluetooth", "bt usb"],
    "btintel": ["intel bluetooth"],
    # 声音
    "snd_hda_intel": ["hd audio", "hda intel", "realtek alc"],
    "snd_usb_audio": ["usb audio", "usb headset"],
    # 摄像头
    "uvcvideo": ["camera", "webcam", "uvc", "video capture"],
    # 虚拟化
    "virtio_gpu": ["virtio gpu", "virtual gpu"],
    "virtio_net": ["virtio network"],
    # 特殊
    "tpm_tis": ["tpm", "trusted platform"],
    "thunderbolt": ["thunderbolt", "usb4"],
    "mmc_block": ["sd card", "sdhc", "mmc"],
    "nvidia_uvm": ["nvidia cuda"],
}


def modules_for(text: str) -> list:
    """按设备描述反查需要的模块。"""
    t = text.lower()
    out = []
    for mod, keys in DEVICE_MODULES.items():
        for k in keys:
            if k in t:
                out.append(mod)
                break
    return out


def module_desc(mod: str) -> str:
    """反查模块管什么设备。用于"lsmod 里这个是什么"。"""
    keys = DEVICE_MODULES.get(mod)
    if not keys:
        return ""
    return "、".join(keys[:3])


# ---------------------------------------------------------------- 状态

def loaded_modules() -> set:
    """当前已加载的模块。"""
    out = set()
    try:
        t = Path("/proc/modules").read_text()
    except OSError:
        return out
    for line in t.splitlines():
        out.add(line.split()[0])
    return out


def module_info(mod: str) -> dict:
    """查模块信息：是否存在、是否已加载、依赖什么。"""
    info = {"module": mod, "exists": False, "loaded": False,
            "depends": [], "firmware": [], "filename": ""}
    if mod in loaded_modules():
        info["loaded"] = True
        info["exists"] = True
    # modinfo 查是否存在与依赖
    try:
        r = subprocess.run(["modinfo", mod], capture_output=True,
                           text=True, timeout=15)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return info
    if r.returncode != 0:
        return info
    info["exists"] = True
    for line in r.stdout.splitlines():
        if line.startswith("depends:"):
            v = line.split(":", 1)[1].strip()
            info["depends"] = [x for x in v.split(",") if x]
        elif line.startswith("firmware:"):
            info["firmware"].append(line.split(":", 1)[1].strip())
        elif line.startswith("filename:"):
            info["filename"] = line.split(":", 1)[1].strip()
    return info


def firmware_of(mod: str) -> list:
    """模块需要的固件文件列表。

    模块加载成功但固件缺失，lsmod 里模块是在的，
    用户看到的是"驱动装了但不工作"。
    """
    return module_info(mod).get("firmware", [])


# ---------------------------------------------------------------- 加载

def load_cmd(mod: str, params: dict | None = None) -> str:
    p = " ".join(f"{k}={v}" for k, v in (params or {}).items())
    return f"modprobe {mod} {p}".strip()


def check_before_load(mod: str) -> list:
    """加载前的检查。返回问题列表。"""
    problems = []
    info = module_info(mod)
    if not info["exists"]:
        problems.append(
            f"内核里没有模块 {mod}——"
            f"内核编译时未包含它，或名字写错了。"
            f"确认：modinfo {mod}")
        # 给出相近的模块名，拼错是最常见的原因
        near = _similar(mod)
        if near:
            problems.append(f"你是否想找：{'、'.join(near)}")
        return problems
    if info["loaded"]:
        problems.append(f"{mod} 已加载（这在多数情况下不是问题）")
    return problems


def _similar(mod: str, limit: int = 3) -> list:
    """相近模块名，用于"名字写错了"的提示。

    黑名单写错模块名会静默失效：模块照常加载，
    用户以为禁掉了其实没有，然后疑难杂症。
    """
    import difflib
    try:
        r = subprocess.run(["find", "/lib/modules", "-name", "*.ko*"],
                           capture_output=True, text=True, timeout=30)
        names = {Path(x).name.split(".")[0] for x in r.stdout.splitlines()}
    except Exception:
        names = set(DEVICE_MODULES)
    return difflib.get_close_matches(mod, list(names), n=limit, cutoff=0.6)


# ---------------------------------------------------------------- 黑名单

def blacklist_path(root: Path) -> Path:
    return Path(root) / "etc" / "modprobe.d" / "qiyuan-blacklist.conf"


def blacklist(root: Path, mod: str, reason: str = "") -> str:
    """把一个模块加入黑名单。

    必须校验模块名真实存在。写错会静默失效——
    模块照常加载，用户以为禁掉了其实没有。
    """
    if not module_info(mod)["exists"]:
        near = _similar(mod)
        hint = f"你是否想找：{'、'.join(near)}" if near else \
               "（内核里没有这个模块，确认内核是否编译了它）"
        raise KmodError(f"模块 {mod} 不存在，黑名单不会生效。{hint}")
    return f"blacklist {mod}"


def blacklist_warning(mod: str) -> str:
    """黑名单的副作用提示。"""
    warns = {
        "nouveau": "禁用 nouveau 后若专有驱动也没装上，会没有图形界面",
        "i915": "禁用 i915 会导致 Intel 核显不可用",
        "amdgpu": "禁用 amdgpu 会导致 AMD 显卡不可用",
        "uvcvideo": "禁用 uvcvideo 会导致所有 USB 摄像头不可用",
        "btusb": "禁用 btusb 会导致蓝牙不可用",
    }
    return warns.get(mod, "")


# ---------------------------------------------------------------- 参数

def param_file(root: Path, mod: str) -> Path:
    return Path(root) / "etc" / "modprobe.d" / f"qiyuan-{mod}.conf"


def param_content(mod: str, params: dict) -> str:
    """生成模块参数配置。

    只在命令行 modprobe 传参，重启就没了——
    用户会以为"设置了没生效"。所以必须落文件。
    """
    L = [f"# {mod} 的模块参数（由澜岫 Linux 生成）"]
    for k, v in params.items():
        L.append(f"options {mod} {k}={v}")
    return "\n".join(L) + "\n"


# 常见模块参数。给错参数模块会加载失败，
# 而失败只报"Invalid argument"，不说哪个参数错
KNOWN_PARAMS = {
    "iwlwifi": {"11n_disable": "0/1，禁用 802.11n（排查兼容性时用）",
                "power_save": "true/false，省电模式（可能影响吞吐）",
                "bt_coex_active": "蓝牙共存，关掉可能解决蓝牙干扰"},
    "snd_hda_intel": {"model": "声卡型号，解决没声音或耳机无声",
                      "power_save": "省电，可能导致爆音"},
    "i915": {"enable_psr": "面板自刷新，可能导致闪烁",
             "modeset": "1 启用内核模式设置"},
    "amdgpu": {"dc": "显示核心，关掉会没有图形"},
    "uvcvideo": {"quirks": "摄像头兼容性参数"},
    "psmouse": {"proto": "触摸板协议"},
    "nouveau": {"modeset": "1 启用内核模式设置"},
}


# ---------------------------------------------------------------- initramfs

# 早期启动必需的模块类型。不在 initramfs 里系统直接起不来，
# 且报错是"找不到根设备"，完全看不出是模块问题
BOOT_CRITICAL = ["storage", "filesystem", "keyboard"]


def initramfs_modules(root: Path) -> list:
    """列出应进 initramfs 的模块。"""
    out = []
    for cat in BOOT_CRITICAL:
        for mod, keys in DEVICE_MODULES.items():
            if cat == "storage" and mod in ("nvme", "ahci", "mmc_block"):
                out.append(mod)
            if cat == "filesystem" and mod in ("ext4", "btrfs", "xfs"):
                out.append(mod)
    # USB 键盘在早期也可能用到
    out.append("usbhid")
    out.append("xhci_hcd")
    return sorted(set(out))


def initramfs_report(root: Path) -> str:
    mods = initramfs_modules(root)
    L = ["早期启动需要的模块（不在 initramfs 里系统起不来）：", ""]
    for m in mods:
        info = module_info(m)
        mark = "已加载" if info["loaded"] else ("存在" if info["exists"] else "缺失")
        L.append(f"  {m:<16}{mark}")
    L.append("")
    L.append("  # 缺失时启动会报'找不到根设备'，")
    L.append("  # 而真实原因是模块没进 initramfs——极易误判为磁盘坏了")
    return "\n".join(L)


# ---------------------------------------------------------------- 报告

def report_for(text: str) -> str:
    """按设备描述给出模块诊断。"""
    mods = modules_for(text)
    if not mods:
        return (f"没有在映射表里找到匹配“{text}”的模块。\n"
                f"  可用 lspci -k 查看内核实际绑定的驱动，\n"
                f"  把结果加到 DEVICE_MODULES 里能让后面的人都受益。")
    L = [f"“{text}”可能需要的模块：", ""]
    for m in mods:
        info = module_info(m)
        state = "已加载" if info["loaded"] else \
                ("未加载（但内核里有）" if info["exists"] else "内核里没有")
        L.append(f"  {m:<16}{state}")
        if info["firmware"]:
            L.append(f"      需要固件: {'、'.join(info['firmware'][:3])}")
        if info["depends"]:
            L.append(f"      依赖: {'、'.join(info['depends'])}")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qykmod",
                                 description="澜岫 Linux 内核模块管理")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("find", help="按设备名查模块")
    sp.add_argument("text", nargs="+")
    sp = sub.add_parser("info", help="模块详情")
    sp.add_argument("module")
    sp = sub.add_parser("load", help="生成加载命令")
    sp.add_argument("module")
    sp.add_argument("--params", nargs="*", default=[],
                    help="格式 k=v")
    sp = sub.add_parser("blacklist", help="生成黑名单条目")
    sp.add_argument("module")
    sp = sub.add_parser("param", help="生成模块参数配置")
    sp.add_argument("module")
    sp.add_argument("params", nargs="+", help="格式 k=v")
    sub.add_parser("initramfs", help="早期启动模块")
    sp = sub.add_parser("firmware", help="模块需要的固件")
    sp.add_argument("module")
    sub.add_parser("params-known", help="已知可用参数")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "find":
            print(report_for(" ".join(a.text)))
            return 0
        if a.cmd == "info":
            i = module_info(a.module)
            print(f"模块: {i['module']}")
            print(f"  存在: {'是' if i['exists'] else '否'}")
            print(f"  已加载: {'是' if i['loaded'] else '否'}")
            if i["depends"]:
                print(f"  依赖: {'、'.join(i['depends'])}")
            if i["firmware"]:
                print(f"  固件: {'、'.join(i['firmware'])}")
            if i["filename"]:
                print(f"  文件: {i['filename']}")
            d = module_desc(a.module)
            if d:
                print(f"  用于: {d}")
            return 0 if i["exists"] else 1
        if a.cmd == "load":
            probs = check_before_load(a.module)
            for x in probs:
                util.log("warn", x)
            params = {}
            for kv in a.params:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    params[k] = v
            print(load_cmd(a.module, params))
            fw = firmware_of(a.module)
            if fw:
                util.log("info", f"加载后需确认固件就位："
                                 f"{'、'.join(fw[:3])}")
                util.log("info", "模块在但固件缺 = '驱动装了却不工作'")
            return 0
        if a.cmd == "blacklist":
            line = blacklist(root, a.module)
            print(line)
            w = blacklist_warning(a.module)
            if w:
                util.log("warn", f"副作用：{w}")
            util.log("info", f"写入 {blacklist_path(root)} 后需重建 initramfs")
            return 0
        if a.cmd == "param":
            params = {}
            for kv in a.params:
                if "=" not in kv:
                    util.log("err", f"参数要用 k=v 格式，收到 {kv}")
                    return 1
                k, v = kv.split("=", 1)
                params[k] = v
            print(param_content(a.module, params), end="")
            util.log("info", f"写入 {param_file(root, a.module)}"
                             f"——只在命令行传参重启就没了")
            return 0
        if a.cmd == "initramfs":
            print(initramfs_report(root))
            return 0
        if a.cmd == "firmware":
            fw = firmware_of(a.module)
            if not fw:
                print(f"{a.module} 不需要固件，或内核里没有该模块")
                return 1
            print(f"{a.module} 需要的固件：")
            for f in fw:
                print(f"  {f}")
            return 0
        if a.cmd == "params-known":
            for m, ps in KNOWN_PARAMS.items():
                print(f"  {m}:")
                for k, d in ps.items():
                    print(f"    {k:<18}{d}")
            return 0
    except KmodError as e:
        util.log("err", str(e))
        return 1
    return 1
