"""内核配置管理。

内核有上万个配置项，手写 `.config` 不可维护。发行版的做法是：
一个基线配置 + 若干"片段"（fragment）叠加，构建时合并。

这里实现片段合并与校验：

    merge(base, fragments)  把片段合并到基线，后写的覆盖先写的
    validate(config)        检查发行版必需项是否开启、危险项是否关闭
    diff(a, b)              两份配置的差异，用于版本升级时审查

必需项与禁用项的清单本身就是发行版的安全姿态声明——写在这里，
每次构建内核都强制检查，不靠人记。
"""
from __future__ import annotations

import re
from pathlib import Path

# 发行版必须开启的选项：[选项, 原因]
REQUIRED = [
    ("CONFIG_DEVTMPFS", "启动时自动填充 /dev，否则设备节点要手工建"),
    ("CONFIG_DEVTMPFS_MOUNT", "自动挂载 devtmpfs 到 /dev"),
    ("CONFIG_PROC_FS", "/proc，几乎所有工具都依赖"),
    ("CONFIG_SYSFS", "/sys，udev 与硬件管理依赖"),
    ("CONFIG_BLK_DEV_INITRD", "支持 initramfs，没有它无法切真根"),
    ("CONFIG_TMPFS", "/tmp 与 /run 需要"),
    ("CONFIG_UNIX", "Unix 域套接字，日志与进程通信依赖"),
    ("CONFIG_INET", "TCP/IP"),
    ("CONFIG_MODULES", "内核模块，驱动按需加载"),
    ("CONFIG_MODULE_UNLOAD", "允许卸载模块"),
    ("CONFIG_BLOCK", "块设备支持"),
    ("CONFIG_EXT4_FS", "ext4 文件系统"),
    ("CONFIG_VFAT_FS", "EFI 启动分区必须是 FAT"),
    ("CONFIG_EFI", "UEFI 启动"),
    ("CONFIG_EFI_STUB", "内核可直接被 UEFI 加载，可省掉 bootloader"),
    ("CONFIG_SERIAL_8250", "串口控制台，服务器排查故障的命脉"),
    ("CONFIG_PRINTK", "内核日志"),
    ("CONFIG_FUTEX", "glibc 的线程同步依赖，关了所有多线程程序都会卡"),
    ("CONFIG_SECCOMP", "沙箱基础设施"),
    ("CONFIG_CGROUPS", "资源控制，服务管理器依赖"),
    ("CONFIG_NAMESPACES", "容器与隔离的基础"),
]

# 必须关闭的选项：[选项, 原因]
FORBIDDEN = [
    ("CONFIG_MODULES_SIG_FORCE", "强制模块签名会让第三方驱动全部加载失败"),
    ("CONFIG_DEBUG_KERNEL", "调试选项显著拖慢系统，发行版不应默认开启"),
    ("CONFIG_KGDB", "内核调试器，仅开发用"),
]

# 安全加固建议开启
HARDENING = [
    ("CONFIG_STACKPROTECTOR", "内核栈保护"),
    ("CONFIG_STACKPROTECTOR_STRONG", "强化的栈保护"),
    ("CONFIG_RELOCATABLE", "内核地址随机化前提"),
    ("CONFIG_RANDOMIZE_BASE", "内核地址空间随机化（KASLR）"),
    ("CONFIG_STRICT_KERNEL_RWX", "内核代码段不可写"),
    ("CONFIG_STRICT_MODULE_RWX", "模块代码段不可写"),
    ("CONFIG_DEFAULT_MMAP_MIN_ADDR", "禁止映射低地址，阻断空指针解引用利用"),
    ("CONFIG_SECURITY_DMESG_RESTRICT", "限制非特权用户读内核日志"),
    ("CONFIG_RANDOM_TRUST_BOOTLOADER", "不使用（应为 n，让内核自己收集熵）"),
]

# 架构相关的必需项
ARCH_REQUIRED = {
    "x86_64": ["CONFIG_64BIT", "CONFIG_X86_64", "CONFIG_IA32_EMULATION"],
    "aarch64": ["CONFIG_ARM64", "CONFIG_64BIT", "CONFIG_OF"],
}

# 安卓设备必需项。缺 Binder/DMA-BUF heaps 时设备能起来但硬件服务全废
# （相机、显示、音频都不能用），且不报错——最难排查的一类问题。
#
# 注意：CONFIG_ANDROID / CONFIG_ION / CONFIG_ASHMEM / CONFIG_SYNC 这些旧符号
# 在 5.x 之后已从上游内核删除，再写进片段会被 kbuild 报未知选项。
# 图形内存共享现在走 DMA-BUF heaps（ION 的官方替代，Android 12 起）。
ANDROID_REQUIRED = [
    ("CONFIG_ANDROID_BINDER_IPC", "Binder IPC，安卓的基础进程通信机制"),
    ("CONFIG_ANDROID_BINDERFS", "Binder 设备文件系统"),
    ("CONFIG_DMABUF_HEAPS", "DMA-BUF heaps，ION 的现代替代，"
                            "相机/显示/视频编解码都依赖它"),
    ("CONFIG_DM_VERITY", "verified boot 的基础，缺了设备无法通过校验启动"),
    ("CONFIG_SECURITY_SELINUX", "安卓强制 SELinux，缺了无法启动到正常状态"),
    ("CONFIG_ZRAM", "内存压缩，手机内存小，这是标配"),
    ("CONFIG_DEVTMPFS", "/dev 自动挂载"),
    ("CONFIG_USB_CONFIGFS", "USB gadget，adb 调试与 MTP 传文件依赖"),
    ("CONFIG_INPUT_TOUCHSCREEN", "触控屏，手机没有鼠标键盘"),
    ("CONFIG_REGULATOR", "电源调节器，手机 SoC 必需"),
]

# 逻辑矛盾：单项都合法但组合起来不成立
CONTRADICTIONS = [
    ("CONFIG_ANDROID_BINDERFS", "y", "CONFIG_ANDROID_BINDER_IPC", None,
     "开了 binderfs 却没开 Binder IPC，设备节点建不出来"),
    ("CONFIG_DM_VERITY", "y", "CONFIG_MD", None,
     "dm-verity 依赖 device mapper（CONFIG_MD/BLK_DEV_DM）"),
    ("CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG", "y", "CONFIG_DM_VERITY", None,
     "校验根哈希签名需要先有 dm-verity"),
    ("CONFIG_ZRAM", "y", "CONFIG_ZSMALLOC", None,
     "zram 需要 zsmalloc 分配器"),
]


def parse_config(text: str) -> dict:
    """解析 .config 文本为 {选项: 值}。值可以是 y/m/n 或字符串。"""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip()
        else:
            m = re.match(r"^# (CONFIG_\w+) is not set$", line)
            if m:
                out[m.group(1)] = "n"
    return out


def parse_fragment(text: str) -> list:
    """解析片段文件，保留顺序（合并时后面的覆盖前面的）。

    会剥掉行内注释。不剥的话 `CONFIG_X=y  # 说明` 的值会变成
    "y  # 说明"，写进 .config 后内核根本认不出这一项——
    而它不会报错，只是静默忽略，于是"明明配了却没生效"。
    """
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        # 剥行内注释：只在引号外出现的 # 才算注释起点
        if "#" in line:
            in_q = False
            cut = len(line)
            for i, ch in enumerate(line):
                if ch == '"':
                    in_q = not in_q
                elif ch == "#" and not in_q:
                    cut = i
                    break
            line = line[:cut].strip()
        if not line:
            continue
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
        if m:
            out.append((m.group(1), m.group(2).strip()))
            continue
        m = re.match(r"^# (CONFIG_\w+) is not set$", line)
        if m:
            out.append((m.group(1), "n"))
    return out


def merge(base: dict, fragments: list) -> tuple:
    """把片段依次合并到基线。返回 (新配置, 变更列表)。"""
    cfg = dict(base)
    changes = []
    for frag in fragments:
        for key, val in frag:
            old = cfg.get(key)
            if old != val:
                changes.append({"option": key, "old": old, "new": val})
                cfg[key] = val
    return cfg, changes


def render(cfg: dict) -> str:
    """渲染成 .config 文本（有序输出，保证可复现）。"""
    lines = ["# 由LANXIU Linux 内核配置管理生成，请勿手工编辑"]
    for k in sorted(cfg):
        v = cfg[k]
        if v == "n":
            lines.append(f"# {k} is not set")
        else:
            lines.append(f"{k}={v}")
    return "\n".join(lines) + "\n"


def validate(cfg: dict, arch: str = "x86_64", android: bool = False) -> dict:
    """校验配置。返回 {missing, forbidden, hardening_missing, notes}。"""
    missing, forbidden, hard_missing, notes = [], [], [], []

    if android:
        for opt, why in ANDROID_REQUIRED:
            v = cfg.get(opt)
            if v != "y" and v != "m":
                missing.append({"option": opt, "reason": why, "current": v})

    for opt, why in REQUIRED:
        v = cfg.get(opt)
        if v != "y" and v != "m":
            missing.append({"option": opt, "reason": why, "current": v})

    for opt in ARCH_REQUIRED.get(arch, []):
        if cfg.get(opt) != "y":
            missing.append({"option": opt, "reason": f"{arch} 架构必需",
                            "current": cfg.get(opt)})

    for opt, why in FORBIDDEN:
        if cfg.get(opt) == "y":
            forbidden.append({"option": opt, "reason": why})

    # 单项都合法但组合起来不成立的情况。
    # 这类问题不会被"必需项检查"抓到——每一项都是 y，
    # 但设备照样起不来，所以必须单独查。
    for a, want, b, bwant, why in CONTRADICTIONS:
        if cfg.get(a) == want:
            if bwant is None and cfg.get(b) != "y" and cfg.get(b) != "m":
                notes.append(f"{a} 已开但 {b} 未开——{why}")
            elif bwant is not None and cfg.get(b) != bwant:
                notes.append(f"{a}={want} 与 {b}={bwant} 冲突——{why}")

    for opt, why in HARDENING:
        # RANDOM_TRUST_BOOTLOADER 特殊：它应该是 n
        if opt == "CONFIG_RANDOM_TRUST_BOOTLOADER":
            if cfg.get(opt) == "y":
                hard_missing.append({"option": opt, "reason": why})
            continue
        v = cfg.get(opt)
        # 有些项是数值（如 DEFAULT_MMAP_MIN_ADDR=65536），只要不是 n 就算开了
        if opt == "CONFIG_DEFAULT_MMAP_MIN_ADDR":
            if v in (None, "n", "0", ""):
                hard_missing.append({"option": opt, "reason": why})
            continue
        if v != "y":
            hard_missing.append({"option": opt, "reason": why})

    # 一致性检查
    if cfg.get("CONFIG_MODULES") != "y":
        for opt in ("CONFIG_EXT4_FS", "CONFIG_VFAT_FS"):
            if cfg.get(opt) == "m":
                notes.append(f"{opt} 设为模块但 CONFIG_MODULES=n，"
                             f"该文件系统将不可用")
    if cfg.get("CONFIG_BLK_DEV_INITRD") != "y":
        notes.append("未启用 initramfs 支持，系统将无法切真根")

    return {"missing": missing, "forbidden": forbidden,
            "hardening_missing": hard_missing, "notes": notes,
            "ok": not missing and not forbidden}


def diff(a: dict, b: dict) -> dict:
    """比对两份配置。升级内核版本时用来审查变化。"""
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    changed = []
    for k in sorted(set(a) & set(b)):
        if a[k] != b[k]:
            changed.append({"option": k, "from": a[k], "to": b[k]})
    return {"only_in_first": only_a, "only_in_second": only_b,
            "changed": changed, "identical": not (only_a or only_b or changed)}


def report(res: dict) -> str:
    lines = []
    if res["missing"]:
        lines.append(f"缺少必需项 {len(res['missing'])} 个：")
        for m in res["missing"]:
            lines.append(f"  [!] {m['option']}（当前 {m['current']}）"
                         f" —— {m['reason']}")
    if res["forbidden"]:
        lines.append(f"开启了不应开启的项 {len(res['forbidden'])} 个：")
        for f in res["forbidden"]:
            lines.append(f"  [!] {f['option']} —— {f['reason']}")
    if res["hardening_missing"]:
        lines.append(f"安全加固项未开启 {len(res['hardening_missing'])} 个：")
        for h in res["hardening_missing"]:
            lines.append(f"  [~] {h['option']} —— {h['reason']}")
    for n in res["notes"]:
        lines.append(f"  [i] {n}")
    if not lines:
        lines.append("内核配置校验全部通过")
    return "\n".join(lines)
