"""磁盘分区方案。

装机最容易出事的地方就是分区。这里把方案变成可校验的数据，
再据此生成脚本——不让人在命令行上手敲分区参数。

关键决策：
  * 一律用 GPT（BIOS 也能靠保护性 MBR 启动，老机器不用单独处理）
  * 分区按 1MiB 对齐，现代硬盘和 SSD 都要求这个
  * fstab 一律用 UUID，不用 /dev/sda1 —— 设备名在加装硬盘后会漂移，
    这是装机后"系统起不来"最常见的原因
  * EFI 分区必须是 FAT32，这是 UEFI 规范强制的，不是习惯
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path

from . import util

MiB = 1024 * 1024
GiB = 1024 * MiB

# 分区类型 GUID（GPT）
TYPE_EFI = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
TYPE_LINUX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
TYPE_SWAP = "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"
TYPE_BIOS_BOOT = "21686148-6449-6E6F-744E-656564454649"

# 文件系统最小尺寸（低于此值装机后很快就会满）
MIN_SIZE = {
    "/": 8 * GiB,
    "/boot": 512 * MiB,
    "/home": 1 * GiB,
    "/var": 2 * GiB,
    "swap": 256 * MiB,
}
EFI_MIN = 260 * MiB      # 规范建议下限，实际给 512M 更稳妥
EFI_RECOMMENDED = 512 * MiB


class DiskError(RuntimeError):
    pass


@dataclass
class Partition:
    """一个分区的描述。大小用字节，None 表示"占满剩余空间"。"""
    mount: str                 # 挂载点，swap 写 "swap"
    fs: str                    # ext4 / xfs / btrfs / vfat / swap
    size: int | None           # 字节；None = 剩余全部
    type_guid: str = TYPE_LINUX
    flags: list = field(default_factory=list)
    label: str = ""
    options: str = "defaults"
    passno: int = 2            # fsck 顺序，根为 1，其他为 2，EFI/swap 为 0

    @property
    def is_swap(self) -> bool:
        return self.fs == "swap"

    @property
    def is_efi(self) -> bool:
        """按挂载点判定，不按文件系统。

        如果按 fs=="vfat" 判定，那么"EFI 分区误设成 ext4"就查不出来了
        ——恰恰是最该拦住的错误。挂载点是意图，文件系统是可能写错的选项。
        """
        return self.mount in ("/boot/efi", "/efi")


@dataclass
class Layout:
    """一套完整的分区方案。"""
    disk: str                       # 目标磁盘，如 /dev/sda
    partitions: list
    table: str = "gpt"              # gpt / msdos
    bootloader: str = "grub"        # grub / efi-stub / syslinux
    uefi: bool = True
    bios: bool = False              # 是否同时支持 legacy BIOS 启动
    align: int = MiB

    def total_min(self) -> int:
        """这套方案所需的最小磁盘空间。"""
        n = 0
        for p in self.partitions:
            if p.size is not None:
                n += p.size
            else:
                n += MIN_SIZE.get(p.mount, 1 * GiB)
        return n


def align_up(n: int, align: int = MiB) -> int:
    return int(math.ceil(n / align) * align)


def default_layout(disk: str, total_bytes: int, memory_bytes: int = 0,
                   uefi: bool = True, separate_home: bool = True,
                   root_fs: str = "ext4", bootloader: str = "grub") -> Layout:
    """按磁盘大小和内存生成一套合理方案。

    swap 的经验规则（跟着内存走，不是拍脑袋）：
      内存 < 2G   → swap = 2 × 内存
      2G ~ 8G     → swap = 内存
      8G ~ 64G    → swap = 内存 / 2（上限 8G）
      > 64G       → 4G（只为休眠和极端情况留一点）
    """
    parts = []

    if uefi:
        parts.append(Partition("/boot/efi", "vfat", EFI_RECOMMENDED,
                               TYPE_EFI, flags=["boot", "esp"],
                               options="umask=0077", passno=0))
    else:
        # legacy BIOS + GPT 需要一段 bios_grub 分区放 core.img
        parts.append(Partition("", "none", MiB,
                               TYPE_BIOS_BOOT, flags=["bios_grub"]))

    parts.append(Partition("/boot", "ext4", align_up(1 * GiB),
                           options="defaults", passno=2))

    if memory_bytes:
        m = memory_bytes
        if m < 2 * GiB:
            swap = 2 * m
        elif m < 8 * GiB:
            swap = m
        elif m < 64 * GiB:
            swap = min(m // 2, 8 * GiB)
        else:
            swap = 4 * GiB
        parts.append(Partition("swap", "swap", align_up(swap),
                               TYPE_SWAP, passno=0))

    # 根分区：磁盘越大给得越多，但有上限
    if total_bytes >= 100 * GiB:
        root = 40 * GiB
    elif total_bytes >= 40 * GiB:
        root = 25 * GiB
    elif total_bytes >= 20 * GiB:
        root = 15 * GiB
    else:
        root = max(MIN_SIZE["/"], int(total_bytes * 0.6))
    parts.append(Partition("/", root_fs, align_up(root), passno=1))

    if separate_home:
        # None = 吃掉剩余全部
        parts.append(Partition("/home", root_fs, None, passno=2))
    else:
        # 没有独立 /home，就让根吃掉剩余
        parts[-1].size = None

    return Layout(disk=disk, partitions=parts, uefi=uefi,
                  bios=not uefi, bootloader=bootloader)


def validate(layout: Layout, total_bytes: int | None = None) -> dict:
    """校验方案。返回 {ok, errors, warnings}。"""
    errors, warnings = [], []
    seen_mounts: set = set()
    grow: list = []

    if not layout.partitions:
        return {"ok": False, "errors": ["没有任何分区"], "warnings": []}

    has_root = False
    for p in layout.partitions:
        if p.mount == "/":
            has_root = True
        if p.mount and p.mount in seen_mounts and p.mount != "swap":
            errors.append(f"挂载点重复: {p.mount}")
        if p.mount:
            seen_mounts.add(p.mount)
        if p.size is None:
            grow.append(p.mount or "(未挂载)")
        else:
            need = MIN_SIZE.get(p.mount)
            if need and p.mount != "swap" and p.size < need:
                errors.append(
                    f"{p.mount or p.fs} 只有 {util.human_size(p.size)}，"
                    f"低于建议的 {util.human_size(need)}")
        # 对齐
        if p.size is not None and p.size % layout.align != 0:
            warnings.append(f"{p.mount or p.fs} 未按 {layout.align} 对齐，"
                            f"会影响 SSD 性能与寿命")

    if not has_root:
        errors.append("没有根分区 /")

    if len(grow) > 1:
        errors.append(f"只能有一个分区占满剩余空间，当前有 {len(grow)} 个: "
                      f"{', '.join(grow)}")

    if layout.uefi:
        efi = [p for p in layout.partitions if p.is_efi]
        if not efi:
            errors.append("UEFI 启动必须有 EFI 系统分区")
        else:
            e = efi[0]
            if e.fs != "vfat":
                errors.append(f"EFI 分区必须是 FAT32，当前是 {e.fs}")
            if e.size and e.size < EFI_MIN:
                errors.append(
                    f"EFI 分区 {util.human_size(e.size)} 小于规范下限 "
                    f"{util.human_size(EFI_MIN)}，内核和引导器会放不下")
            if "esp" not in e.flags and "boot" not in e.flags:
                warnings.append("EFI 分区建议设置 boot/esp 标志")
    else:
        if not any(p.type_guid == TYPE_BIOS_BOOT for p in layout.partitions):
            warnings.append("legacy BIOS + GPT 需要 bios_grub 分区，"
                            "否则 GRUB 可能装不上")

    if total_bytes:
        need = layout.total_min()
        if need > total_bytes:
            errors.append(
                f"方案需要至少 {util.human_size(need)}，"
                f"磁盘只有 {util.human_size(total_bytes)}")

    return {"ok": not errors, "errors": errors, "warnings": warnings}


def compute_offsets(layout: Layout, total_bytes: int) -> list:
    """算出每个分区的起始/结束偏移（字节）。用于生成分区脚本。"""
    # GPT 在磁盘头尾各留一段：头部 1MiB（含 protective MBR + GPT 头），
    # 尾部 33 个扇区（备份 GPT）
    cur = align_up(1 * MiB, layout.align)
    tail_reserve = align_up(33 * 512, layout.align)
    usable = total_bytes - tail_reserve

    out = []
    grow_part = next((p for p in layout.partitions if p.size is None), None)
    fixed = sum(p.size for p in layout.partitions if p.size is not None)

    for i, p in enumerate(layout.partitions):
        if p is grow_part:
            size = align_up(usable - cur, layout.align)
            if size <= 0:
                raise DiskError(
                    f"磁盘空间不足：固定分区已占满，{p.mount or p.fs} 分不到空间")
        else:
            size = p.size
        out.append({"index": i + 1, "partition": p,
                    "start": cur, "end": cur + size - 1, "size": size})
        cur += size
    return out


def fstab_entry(p: Partition, uuid: str | None = None) -> str:
    """生成一行 /etc/fstab。用 UUID 而不是设备名。"""
    if p.is_swap:
        dev = f"UUID={uuid}" if uuid else "(待生成)"
        return f"{dev}\tnone\tswap\tsw\t0\t0"
    dev = f"UUID={uuid}" if uuid else "(待生成)"
    return f"{dev}\t{p.mount}\t{p.fs}\t{p.options}\t0\t{p.passno}"


def render_fstab(parts: list, uuids: dict | None = None) -> str:
    """渲染完整 fstab。挂载顺序：先 /，再其他，最后 swap。"""
    uuids = uuids or {}
    # 按挂载点深度排序：/ → /boot → /boot/efi。
    # 顺序本身 systemd 会按依赖重排，但人读 fstab 时顺序直观能少犯错。
    def depth(p):
        return len([c for c in p.mount.split("/") if c])

    def key(p):
        if p.is_swap:
            return (99, 0)
        if p.is_efi:
            return (depth(p) + 10, p.mount)   # EFI 放最后，它挂在 /boot 之下
        return (depth(p), p.mount)
    lines = [
        "# /etc/fstab —— 由LANXIU安装器生成",
        "# 一律使用 UUID：设备名 (/dev/sda1) 在加装硬盘后会漂移，",
        "# 用设备名是装机后系统起不来的头号原因。",
        "#",
        "# <设备>  <挂载点>  <类型>  <选项>  <dump>  <fsck顺序>",
    ]
    for p in sorted(parts, key=key):
        if not p.mount and not p.is_swap:
            continue
        lines.append(fstab_entry(p, uuids.get(p.mount)))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- 脚本

def render_partition_script(layout: Layout, total_bytes: int | None = None) -> str:
    """生成可执行的分区脚本。

    不直接调 sgdisk/parted（沙盒里没有，真实机器上也可能缺），
    而是生成脚本让装机环境执行——逻辑在这里可测，执行交给目标机。

    total_bytes 为 None 时不写死偏移量，最后一个分区交给 sgdisk 用
    "0:0" 吃掉剩余空间。这样脚本能适配任意大小的磁盘，
    而不用在构建镜像时就假设目标盘容量。
    """
    if total_bytes is None:
        offs = []
        cur = align_up(1 * MiB, layout.align)
        for i, p in enumerate(layout.partitions, 1):
            if p.size is None:
                offs.append({"index": i, "partition": p,
                             "start": None, "end": None, "size": None})
                continue
            offs.append({"index": i, "partition": p, "start": cur,
                         "end": cur + p.size - 1, "size": p.size})
            cur += p.size
    else:
        offs = compute_offsets(layout, total_bytes)
    lines = [
        "#!/bin/bash",
        "# LANXIU Linux 分区脚本 —— 由安装器生成",
        "# 执行前请确认目标磁盘正确，脚本会清空磁盘上的所有数据",
        "set -euo pipefail",
        f'DISK="{layout.disk}"',
        "",
        '# 二次确认：这是不可逆操作',
        'echo "即将清空 $DISK 上的所有数据："',
        'lsblk "$DISK" 2>/dev/null || true',
        'read -r -p "确认请输入 yes: " _c',
        '[ "$_c" = "yes" ] || { echo "已取消"; exit 1; }',
        "",
        "wipefs -a \"$DISK\"",
        "sgdisk -Z \"$DISK\"",
        "",
    ]
    for o in offs:
        p = o["partition"]
        num = o["index"]
        if o["size"] is None:
            # 0:0 = 从默认位置到磁盘末尾，交给 sgdisk 自己算
            span = f"{num}:0:0"
        else:
            span = f"{num}:{o['start'] // MiB}M:{(o['end'] + 1) // MiB}M"
        if p.type_guid == TYPE_BIOS_BOOT:
            lines.append(
                '# bios_grub：GRUB 在 GPT 上需要这段空间放 core.img')
            lines.append(f'sgdisk -n {span} -t {num}:ef02 "$DISK"')
        elif p.is_efi:
            lines.append('# EFI 系统分区（UEFI 规范强制 FAT32）')
            lines.append(f'sgdisk -n {span} -t {num}:ef00 "$DISK"')
        elif p.is_swap:
            lines.append(f'sgdisk -n {span} -t {num}:8200 "$DISK"')
        else:
            lines.append(f'sgdisk -n {span} -t {num}:8300 "$DISK"')
    lines += ["", 'sgdisk -p "$DISK"', "", "# 通知内核重读分区表",
              'partprobe "$DISK" 2>/dev/null || true',
              'sleep 2', ""]

    # 格式化
    for o in offs:
        p = o["partition"]
        num = o["index"]
        part_dev = f'${{DISK}}{num}' if layout.disk[-1].isdigit() \
            else f'${{DISK}}{num}'
        if p.type_guid == TYPE_BIOS_BOOT:
            continue
        if p.is_efi:
            lines.append(f'mkfs.vfat -F32 "{part_dev}"')
        elif p.is_swap:
            lines.append(f'mkswap "{part_dev}"')
        else:
            lines.append(f'mkfs.{p.fs} -F "{part_dev}"')
    lines += ["", 'echo "分区与格式化完成"', ""]
    return "\n".join(lines)


def render_mount_script(layout: Layout, target: str = "/mnt/qiyuan") -> str:
    """生成挂载脚本。

    挂载顺序按路径深度（先 / 再 /boot 再 /boot/efi），
    卸载时逆序——顺序错了会卸不掉或卸错。
    """
    # 带上分区号，避免脚本里再猜
    numbered = []
    for i, p in enumerate(layout.partitions, 1):
        numbered.append((i, p))

    devname = _dev_name(layout.disk)
    mountable = [(i, p) for i, p in numbered
                 if p.mount and not p.is_swap and p.fs and p.fs != "none"]
    mountable.sort(key=lambda t: len([c for c in t[1].mount.split("/") if c]))

    lines = [
        "#!/bin/bash",
        "# LANXIU Linux 挂载脚本 —— 由安装器生成",
        "set -euo pipefail",
        f'DISK="{layout.disk}"',
        f'TARGET="{target}"',
        "",
        "mkdir -p \"$TARGET\"",
        "",
    ]
    for i, p in mountable:
        lines.append(f'mkdir -p "$TARGET{p.mount}"')
        lines.append(f'mount "{devname(i)}" "$TARGET{p.mount}"')
    for i, p in numbered:
        if p.is_swap:
            lines.append(f'swapon "{devname(i)}"')
    lines += ["", 'echo "已挂载到 $TARGET"', ""]
    return "\n".join(lines)


def render_umount_script(layout: Layout, target: str = "/mnt/qiyuan") -> str:
    """生成卸载脚本（逆序）。装机收尾和失败回滚都要用它。"""
    devname = _dev_name(layout.disk)
    mountable = [(i, p) for i, p in enumerate(layout.partitions, 1)
                 if p.mount and not p.is_swap and p.fs and p.fs != "none"]
    # 逆序：/boot/efi 先卸，/ 最后卸
    mountable.sort(key=lambda t: len([c for c in t[1].mount.split("/") if c]),
                   reverse=True)
    lines = [
        "#!/bin/bash",
        "# LANXIU Linux 卸载脚本 —— 由安装器生成（逆序）",
        "set -uo pipefail",
        f'TARGET="{target}"',
        "",
    ]
    for i, p in mountable:
        lines.append(f'umount "$TARGET{p.mount}" 2>/dev/null || true')
    lines.append(f'umount "$TARGET" 2>/dev/null || true')
    lines.append('echo "已卸载"')
    return "\n".join(lines)


def _dev_name(disk: str):
    """返回生成分区设备名的函数。

    NVMe 是 /dev/nvme0n1p1（多一个 p），普通盘是 /dev/sda1，
    MMC/SD 卡又是 /dev/mmcblk0p1。写死 sda1 在这些机器上会分错区
    ——把系统装到错误的设备上，是很常见的装机事故。
    """
    if "nvme" in disk or "mmcblk" in disk or disk[-1].isdigit():
        sep = "p"
    else:
        sep = ""

    def name(idx: int) -> str:
        return f"${{DISK}}{sep}{idx}"
    return name
