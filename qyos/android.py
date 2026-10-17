"""安卓设备适配：boot.img、动态分区、A/B 槽、fastboot 刷机。

把发行版装进安卓设备（手机/平板/开发板）与装进 PC 是两回事，
差别不在内核和包管理器，而在**引导链与分区模型完全换了**：

  PC:      UEFI → GRUB → 内核 → initramfs → 切根
  安卓设备: bootloader → boot.img（内核+ramdisk+dtb 打包成一个文件）
           → super 动态分区（system/vendor 是子分区，不是真分区）

几个必须处理对的点：

1. **boot.img 是一个打包格式，不是一个分区**。内核、ramdisk、dtb
   按页对齐塞进同一个文件，bootloader 直接读这个文件。
   页大小错了整个镜像不启动，而错误表现是"黑屏无任何输出"。

2. **A/B 槽不是原子升级**。两者都叫"无缝"，但粒度完全不同：
   * A/B 是整槽切换：更新写进另一侧槽，重启换槽，失败自动回退
   * 原子升级是包级事务：一次升级几十个包，失败回滚到包级快照
   装进安卓设备要**两套都有**：槽切换保证刷不死，包级事务保证
   日常升级不留半状态。只做 A/B 的话，槽内的包级升级坏了还是坏的。

3. **system 分区在安卓设备上是只读的**（verified boot 强制）。
   桌面 Linux 的可写 /usr 在这里不成立，需要 usr-verity 或 overlay。

4. **boot.img 里的 os_version 和 AVB 签名不匹配会直接拒绝启动**，
   而且是硬失败——设备变砖的概率远高于 PC 装机。所以这里对镜像
   做完整的自检后才允许交付。

5. **刷机需要解锁 bootloader，且解锁会清空用户数据**。
   脚本必须明确警告，不能静默执行。
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import util

# boot.img 魔数
BOOT_MAGIC = b"ANDROID!"

# 常见页大小。设备不对就直接不启动，且无任何输出。
PAGE_SIZES = (2048, 4096, 8192, 16384, 65536, 131072)

# boot.img header v4 的字段定义。
# pack 与 unpack 共用同一份定义——两边各写一遍顺序迟早会错，
# 而顺序错了的表现是"能打包、能解析、但设备不启动"，极难排查。
#
# 注意：v3 起字段顺序相对 v0-v2 有调整（os_version 提前、header_size
# 位置变了）。这里按 v4 实现。字节级兼容未经真机核验，
# 首次实机使用请用 unpack_bootimg 比对一次。
HEADER_FIELDS = [
    ("magic",           "8s"),
    ("kernel_size",     "I"),
    ("ramdisk_size",    "I"),
    ("os_version",      "I"),
    ("header_size",     "I"),
    ("kernel_addr",     "I"),
    ("ramdisk_addr",    "I"),
    ("second_size",     "I"),
    ("second_addr",     "I"),
    ("tags_addr",       "I"),
    ("page_size",       "I"),
    ("dtb_size",        "I"),
    ("header_version",  "I"),
    ("name",            "16s"),
    ("cmdline",         "512s"),
    ("id",              "32s"),
    ("extra_cmdline",   "1024s"),
    ("recovery_dtbo_size",   "I"),
    ("recovery_dtbo_offset", "Q"),
    ("dtb_addr",        "Q"),
]
HEADER_FMT = "<" + "".join(f[1] for f in HEADER_FIELDS)
HEADER_SIZE = struct.calcsize(HEADER_FMT)
HEADER_KEYS = [f[0] for f in HEADER_FIELDS]

# 动态分区元数据槽大小
LP_METADATA_SLOTS = 2        # 主备两份，一份坏了还能开
LP_METADATA_SIZE = 65536     # 每个槽的元数据大小


class AndroidError(RuntimeError):
    pass


def align_up(n: int, page: int) -> int:
    return (n + page - 1) // page * page


# ---------------------------------------------------------------- boot.img

@dataclass
class BootImage:
    """一个 Android boot.img 的内容描述。"""
    kernel: bytes = b""
    ramdisk: bytes = b""
    dtb: bytes = b""
    cmdline: str = ""
    page_size: int = 4096
    header_version: int = 4
    os_version: str = ""        # 如 "13.0.0" 或 "qiyuan-1.0"
    name: str = "qiyuan"
    # v4 之后内核与 ramdisk 地址由 bootloader 决定，填 0
    kernel_addr: int = 0
    ramdisk_addr: int = 0
    tags_addr: int = 0

    def os_version_field(self) -> int:
        """把版本号编进 header 的 os_version 字段。

        AOSP 位布局（system/tools/mkbootimg/include/bootimg/bootimg.h）：
            os_version = A[31:25] B[24:18] C[17:11] (Y-2000)[10:4] M[3:0]
        即 Android 版本三段各占 7 位放在高 21 位，patch level 的
        「年偏移 + 月」占低 11 位（无日）。

        填错不会报错，但 AVB/dm-verity 校验会失败，设备直接不启动——
        而且报错信息在 bootloader 阶段，普通用户根本看不到。
        因此这里必须逐位对齐 AOSP，不能自创布局。
        """
        if not self.os_version:
            return 0
        parts = self.os_version.split(".")
        try:
            a = int(parts[0]); b = int(parts[1]) if len(parts) > 1 else 0
            c = int(parts[2]) if len(parts) > 2 else 0
        except (ValueError, IndexError):
            # 非数字版本号（如 "qiyuan-1.0"）：算个稳定哈希填进去，
            # 保证同一版本每次构建出一样的镜像（可复现）
            h = int(hashlib.sha256(self.os_version.encode()).hexdigest()[:8], 16)
            return h & 0x7FFFFFFF

        import datetime
        today = datetime.date.today()
        if a < 100:
            # 单个数字是 Android 版本号（Android 13 = A），不是年份。
            # 三段都进版本区；patch level 用构建当年的年月。
            major, minor, patch = a, b, c
            year, month = today.year - 2000, today.month
        else:
            # 形如 "2026.10"：按 patch level 的年.月 解释，版本区留空。
            major = minor = patch = 0
            year, month = a - 2000, b

        major = max(0, min(0x7f, major))
        minor = max(0, min(0x7f, minor))
        patch = max(0, min(0x7f, patch))
        year = max(0, min(0x7f, year))
        month = max(0, min(0x0f, month))
        return (major << 25) | (minor << 18) | (patch << 11) | (year << 4) | month


def build_boot_image(img: BootImage) -> bytes:
    """打包 boot.img（header v4）。"""
    if img.page_size not in PAGE_SIZES:
        raise AndroidError(
            f"页大小 {img.page_size} 不在常见取值 {PAGE_SIZES} 内。\n"
            f"  页大小错了设备不启动，且表现为黑屏无任何输出——"
            f"这是安卓装机最难排查的问题之一。\n"
            f"  绝大多数现代设备是 4096。")

    page = img.page_size
    ksize = align_up(len(img.kernel), page)
    rsize = align_up(len(img.ramdisk), page)
    dsize = align_up(len(img.dtb), page)

    cmdline = img.cmdline.encode()[:512]
    name = img.name.encode()[:16]

    hdr = struct.pack(
        HEADER_FMT,
        BOOT_MAGIC,
        len(img.kernel), len(img.ramdisk),
        img.os_version_field(),
        0,                            # header_size（装完再回填）
        img.kernel_addr, img.ramdisk_addr,
        0, 0,                         # second stage，已废弃
        img.tags_addr, page,
        len(img.dtb),
        img.header_version,
        name, cmdline, b"", b"",
        0, 0,                         # recovery_dtbo
        0,                            # dtb_addr
    )
    # header_size 字段回填真实 header 长度
    vals = list(struct.unpack(HEADER_FMT, hdr))
    vals[HEADER_KEYS.index("header_size")] = HEADER_SIZE
    # id 段填内核+ramdisk 的 sha1：镜像自身可校验，
    # 不填的话设备侧无法判断传输中是否损坏。
    digest = hashlib.sha1(img.kernel + img.ramdisk).digest()
    blob = dict(zip(HEADER_KEYS, vals))
    blob["id"] = b""
    blob["name"] = b""
    blob["cmdline"] = b""
    blob["extra_cmdline"] = b""
    blob["magic"] = BOOT_MAGIC
    # 先算出除 id 外的完整 header，再对 header+kernel+ramdisk 取 sha1
    pre = struct.pack(HEADER_FMT, *[blob[k] for k in HEADER_KEYS])
    pre = pre + b"\0" * (page - len(pre)) if len(pre) < page else pre[:page]
    full_id = hashlib.sha1(pre + img.kernel + img.ramdisk).digest()
    vals[HEADER_KEYS.index("id")] = full_id + b"\0" * 12   # sha1 20B + 补齐 32B
    hdr = struct.pack(HEADER_FMT, *vals)
    # header 必须整页对齐：bootloader 按页读，不对齐会读错位
    if len(hdr) != page:
        hdr = hdr + b"\0" * (page - len(hdr)) if len(hdr) < page else hdr[:page]

    out = bytearray(hdr)
    out += img.kernel + b"\0" * (ksize - len(img.kernel))
    out += img.ramdisk + b"\0" * (rsize - len(img.ramdisk))
    if img.dtb:
        out += img.dtb + b"\0" * (dsize - len(img.dtb))
    return bytes(out)


def parse_boot_image(data: bytes) -> dict:
    """解析 boot.img header。"""
    if data[:8] != BOOT_MAGIC:
        raise AndroidError("不是有效的 boot.img（魔数不对）")
    if len(data) < HEADER_SIZE:
        raise AndroidError(
            f"文件只有 {len(data)} 字节，header 就要 {HEADER_SIZE} 字节——"
            f"镜像被截断，刷进去必黑屏")
    vals = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    d = dict(zip(HEADER_KEYS, vals))
    d["name"] = d["name"].rstrip(b"\0").decode(errors="replace")
    d["cmdline"] = d["cmdline"].rstrip(b"\0").decode(errors="replace")
    d["id"] = d["id"].hex()
    d["extra_cmdline"] = d["extra_cmdline"].rstrip(b"\0").decode(errors="replace")
    # os_version 按 AOSP 布局解码回可读形式
    ov = d["os_version"]
    if ov:
        major = (ov >> 25) & 0x7f
        minor = (ov >> 18) & 0x7f
        patch = (ov >> 11) & 0x7f
        year = 2000 + ((ov >> 4) & 0x7f)
        month = ov & 0x0f
        d["os_version_decoded"] = (f"{major}.{minor}.{patch}"
                                   f" (patch {year}-{month:02d})")
    else:
        d["os_version_decoded"] = ""
    return d


def verify_boot_image(data: bytes) -> list:
    """自检 boot.img。返回问题列表，空表示通过。

    安卓设备上镜像有问题不会给你报错机会——直接不启动。
    所以交付前必须在这里把能查的都查了。
    """
    problems = []
    if data[:8] != BOOT_MAGIC:
        return ["魔数不对，不是 boot.img"]
    try:
        info = parse_boot_image(data)
    except AndroidError as e:
        return [str(e)]
    except struct.error:
        return ["header 长度不足，镜像被截断"]

    page = info["page_size"]
    if page not in PAGE_SIZES:
        problems.append(f"页大小 {page} 不是常见取值，设备可能不启动")
    if info["kernel_size"] == 0:
        problems.append("内核为空")
    if info["ramdisk_size"] == 0:
        problems.append("没有 initramfs——切不了真根，设备会卡在早期用户空间")
    if info.get("header_size", 0) not in (0, HEADER_SIZE):
        problems.append(
            f"header_size 字段 {info.get('header_size')} 与本实现 "
            f"{HEADER_SIZE} 不符，可能是不同 header 版本——"
            f"实机请用 unpack_bootimg 比对一次")

    # 关键：各部分必须按页对齐，否则 bootloader 读错位
    hdr_size = align_up(HEADER_SIZE, page)
    if len(data) < hdr_size:
        problems.append("文件比 header 还短，镜像被截断")
        return problems
    expected = hdr_size + align_up(info["kernel_size"], page) \
        + align_up(info["ramdisk_size"], page)
    if info["dtb_size"]:
        expected += align_up(info["dtb_size"], page)
    if len(data) < expected:
        problems.append(
            f"文件长度 {len(data)} 小于按其自身 header 计算的应有长度 "
            f"{expected}——镜像被截断，刷进去必黑屏")
    return problems


# ---------------------------------------------------------------- 动态分区

@dataclass
class DynamicPartition:
    """super 分区内的一个子分区。"""
    name: str
    size: int
    readonly: bool = True      # 安卓设备上 system 必须只读（verified boot）
    group: str = "qiyuan_a"


@dataclass
class SuperLayout:
    """super 动态分区布局。"""
    partitions: list
    slot_suffix: str = "_a"
    metadata_slots: int = LP_METADATA_SLOTS

    @property
    def metadata_total(self) -> int:
        """两个槽的元数据 + 备份，还要按 4096 对齐。"""
        n = LP_METADATA_SIZE * self.metadata_slots * 2
        return align_up(align_up(n, 4096), 65536)

    def content_total(self) -> int:
        return sum(p.size for p in self.partitions)

    def super_size(self, align: int = 1 * 1024 * 1024) -> int:
        """super 分区需要多大。元数据 + 所有子分区 + 对齐余量。"""
        return align_up(self.metadata_total + self.content_total(), align)

    def lpmake_cmd(self, super_dev: str, size: int) -> str:
        """生成 lpmake 命令。

        子分区必须带槽后缀（system_a），组名也要带——A/B 设备两组各管
        一个槽，组名不带后缀会导致另一个槽使用时元数据冲突。
        """
        parts = []
        for p in self.partitions:
            seg = (f"--partition {p.name}{self.slot_suffix}:{p.size}:"
                   f"{p.group}")
            if p.readonly:
                seg += " --readonly"
            parts.append(seg)
        groups = " ".join(
            f"--group {g}:{self.group_size(g)}" for g in self.groups())
        return (f"lpmake --device {super_dev}:{size} "
                f"--metadata-slots {self.metadata_slots} "
                f"--metadata-size {LP_METADATA_SIZE} "
                + groups + " " + " ".join(parts))

    def group_size(self, group: str) -> int:
        return sum(p.size for p in self.partitions if p.group == group)

    def groups(self) -> list:
        return sorted({p.group for p in self.partitions})

    def report(self) -> str:
        L = [f"super 动态分区布局（槽 {self.slot_suffix}）"]
        L.append(f"  元数据: {self.metadata_total // 1024}K"
                 f"（{self.metadata_slots} 槽 × 2 份）")
        L.append("  子分区:")
        for p in self.partitions:
            ro = "只读" if p.readonly else "可写"
            L.append(f"    {p.name}{self.slot_suffix:<3} "
                     f"{p.size // (1024*1024):>5}M  {ro}")
        L.append(f"  super 需要: {self.super_size() // (1024*1024)}M")
        return "\n".join(L)


def android_super_layout(root_mb: int = 3072) -> SuperLayout:
    """一套典型的安卓设备 super 布局。

    system 只读是 verified boot 的硬要求；vendor 放硬件相关模块，
    product 放发行版自带的东西。分开是为了让 vendor 能独立更新，
    不跟着 system 一起动——安卓设备的驱动更新频率远高于系统。
    """
    M = 1024 * 1024
    return SuperLayout(partitions=[
        DynamicPartition("system", root_mb * M, readonly=True,
                         group="qiyuan_a"),
        DynamicPartition("vendor", 512 * M, readonly=True,
                         group="qiyuan_a"),
        DynamicPartition("product", 256 * M, readonly=True,
                         group="qiyuan_a"),
    ])


# ---------------------------------------------------------------- A/B 槽

@dataclass
class SlotState:
    """A/B 槽状态。"""
    current: str = "_a"
    other: str = "_b"
    current_ok: bool = True      # 当前槽是否已被标记为成功启动
    other_updatable: bool = True
    retry_left: int = 0

    def switch(self) -> "SlotState":
        return SlotState(current=self.other, other=self.current,
                         current_ok=False, other_updatable=True,
                         retry_left=3)


def ab_notes() -> str:
    """A/B 槽与包级原子升级的关系说明。两者都要有。"""
    return """A/B 槽与原子升级是两件事，安卓设备上两套都要有：

  A/B 槽：整槽切换。更新写进另一侧槽，重启换槽，启动失败自动回退。
    粒度是整个系统，代价是占用双倍空间（system 分区）。

  原子升级：包级事务。一次升级几十个包，任一个失败全部回滚到包级快照。
    粒度是包，不占额外空间。

只做 A/B 的后果：槽内的日常包升级坏了就是坏的，
  要等到下一次整槽更新才可能修好——而整槽更新通常以月为单位。
只做包级事务的后果：包管理器本身或内核被写坏时无人可救，
  因为能回滚的只有数据文件，引导链已经坏了。

所以：整槽更新走 A/B（内核、vendor 这类动引导链的），
    日常软件更新走包级事务（应用、库、配置）。"""


# ---------------------------------------------------------------- 刷机脚本

FASTBOOT_WARN = """# ============ 必读 ============
# 1. 刷机需要 bootloader 已解锁。解锁会清空设备上的全部用户数据，
#    且多数厂商解锁后永久失去保修。
# 2. 解锁方式各厂商不同（fastboot oem unlock / fastboot flashing unlock），
#    请查你的设备型号对应的方法，不要照抄。
# 3. 刷错分区可能让设备无法开机。执行前确认 --disk 指向的设备正确。
# 4. 变砖风险真实存在，且远高于 PC 装机：安卓设备的 bootloader
#    多数没有 PC 那样的恢复菜单。
# ===============================
"""


def fastboot_script(slot: str = "_a", wipe: bool = False,
                    super_img: str = "super.img",
                    boot_img: str = "boot.img",
                    vbmeta: bool = True) -> str:
    """生成 fastboot 刷机脚本。"""
    L = ["#!/bin/bash",
         "# LANXIU Linux 安卓设备刷机脚本 —— 由 qyandroid 生成",
         "set -euo pipefail",
         "",
         FASTBOOT_WARN,
         ""]
    L.append('# 先确认设备进的是 fastboot 模式而不是 fastbootd。')
    L.append('# 动态分区（super）只能在 fastbootd 下刷，普通 fastboot 下')
    L.append('# 刷 super 会直接失败——这是最常见的一步卡住。')
    L.append('echo "当前 fastboot 设备："')
    L.append('fastboot devices')
    L.append('')
    L.append('# 进 fastbootd（若已在其中会无害返回）')
    L.append('fastboot reboot fastboot || true')
    L.append('echo "等待设备进入 fastbootd..."')
    L.append('fastboot devices')
    L.append('sleep 3')
    L.append('')
    L.append(f'# 刷 boot 分区（当前槽 {slot}）')
    L.append(f'fastboot flash boot{slot} {boot_img}')
    L.append('')
    L.append('# 刷动态分区。必须先删旧逻辑分区再重建，')
    L.append('# 否则子分区大小变了会残留旧布局。')
    L.append('fastboot delete-logical-partition system' + slot + ' || true')
    L.append('fastboot delete-logical-partition vendor' + slot + ' || true')
    L.append('fastboot delete-logical-partition product' + slot + ' || true')
    L.append('')
    L.append(f'fastboot flash super {super_img}')
    L.append('')
    if vbmeta:
        L.append('# vbmeta：verified boot 的信任根。')
        L.append('# 用测试密钥签名会触发开机警告，正式发布必须换成自己的密钥。')
        L.append('# --disable-verification 只在开发阶段用，正式发布不该出现。')
        L.append('fastboot --disable-verity --disable-verification '
                 'flash vbmeta' + slot + ' vbmeta.img')
        L.append('')
    if wipe:
        L.append('# 清空用户数据（会删除所有个人文件）')
        L.append('fastboot erase userdata')
        L.append('fastboot erase metadata')
        L.append('')
    else:
        L.append('# 保留用户数据。若首次安装或槽布局变了，')
        L.append('# 需要加 --wipe 重跑一次。')
        L.append('')
    L.append(f'# 切换到 {slot} 槽并重启')
    L.append(f'fastboot set_active {slot.lstrip("_")}')
    L.append('fastboot reboot')
    L.append('')
    L.append('echo "刷机完成。若卡在开机画面，用 fastboot set_active '
             f'{"b" if slot == "_a" else "a"} 切回另一槽即可回退。"')
    return "\n".join(L) + "\n"


def android_fstab(root_dev: str = "/dev/block/by-name/system") -> str:
    """安卓设备的 fstab。

    安卓设备没有 UUID 可依赖（动态分区每次重建逻辑分区 UUID 会变），
    也不能用 /dev/sda1 这种名字（设备节点顺序不稳定）。
    必须走 /dev/block/by-name/，那是按分区名建的符号链接。
    """
    return f"""# 安卓设备 fstab —— 不能用 UUID，也不能用 /dev/sdXN
#
# 为什么不用 UUID（桌面上的做法在这里不成立）：
#   super 是动态分区，每次重建逻辑分区时子分区的 UUID 都会变，
#   写在 fstab 里的 UUID 下一次更新就失效了，表现为"更新后起不来"。
# 为什么不用 /dev/sda1：
#   块设备节点顺序不保证稳定。
# 正确答案：/dev/block/by-name/<分区名>，bootloader 按名字建链接。
{src_line(root_dev, "/", "ext4", "ro")}
{src_line("/dev/block/by-name/vendor", "/vendor", "ext4", "ro")}
{src_line("/dev/block/by-name/userdata", "/data", "f2fs", "nosuid,nodev")}
{src_line("/dev/block/by-name/metadata", "/metadata", "ext4", "nosuid,nodev")}
"""


def src_line(dev: str, mount: str, fs: str, opts: str) -> str:
    return f"{dev}  {mount}  {fs}  {opts}  defaults"


# ---------------------------------------------------------------- 内核片段

ANDROID_KERNEL_ITEMS = [
    ("CONFIG_ANDROID_BINDER_IPC", "y", "Binder IPC（安卓的基础 IPC 机制）"),
    ("CONFIG_ANDROID_BINDERFS", "y", "Binder 设备文件系统"),
    ("CONFIG_ANDROID_BINDER_DEVICES", '"binder,hwbinder,vndbinder"',
     "三个 binder 设备，缺一个硬件服务就起不来"),
    # 注意：以下旧符号在 5.x 后已从上游内核删除，写了会被 kbuild 报未知选项：
    #   CONFIG_ANDROID / CONFIG_ASHMEM / CONFIG_ION / CONFIG_SYNC
    #   CONFIG_ANDROID_LOW_MEMORY_KILLER / CONFIG_SCHED_TUNE
    # 它们的功能已由下列现代等价物承担（内存共享走 memfd + DMA-BUF heaps，
    # 低内存回收走 PSI + lmkd 用户态守护）。
    ("CONFIG_DMABUF_HEAPS", "y", "DMA-BUF heaps——ION 的现代替代（相机/显示/视频）"),
    ("CONFIG_DMABUF_HEAPS_SYSTEM", "y", "系统堆（通用图形缓冲）"),
    ("CONFIG_DMABUF_HEAPS_CMA", "y", "CMA 堆（需要连续物理内存的硬件）"),
    ("CONFIG_SYNC_FILE", "y", "dma-fence 同步框架（旧 CONFIG_SYNC 的替代）"),
    ("CONFIG_STAGING", "y", "staging 驱动（很多安卓驱动在这里）"),
    ("CONFIG_SW_SYNC", "y", "软件同步（同步框架的调试与兼容层）"),
    ("CONFIG_UEVENT_HELPER", "n", "关闭，安卓用 netlink 而非调用 helper"),
    ("CONFIG_FW_LOADER_USER_HELPER", "n", "固件加载走内核直读"),
    ("CONFIG_NET_SCHED", "y", "网络调度（省电与流量控制依赖）"),
    ("CONFIG_NETFILTER_XT_TARGET_MASQUERADE", "y",
     "网络共享需要（旧符号 IP_NF_TARGET_MASQUERADE 已并入 xt_MASQUERADE）"),
    ("CONFIG_USB_CONFIGFS", "y", "USB gadget（adb/mtp/快充都靠它）"),
    ("CONFIG_CONFIGFS_FS", "y", "configfs（USB gadget 依赖）"),
    ("CONFIG_SQUASHFS", "y", "只读压缩文件系统（安卓常用）"),
    ("CONFIG_SQUASHFS_XZ", "y", "squashfs 的 xz 压缩"),
    ("CONFIG_EROFS_FS", "y", "EROFS，安卓上常见的只读文件系统"),
    ("CONFIG_EROFS_FS_XATTR", "y", "EROFS 扩展属性"),
    ("CONFIG_TMPFS_XATTR", "y", "tmpfs 扩展属性（安卓的 /dev 需要）"),
    ("CONFIG_DEVTMPFS", "y", "/dev 自动挂载"),
    ("CONFIG_SECURITY_SELINUX", "y", "SELinux——安卓强制开启"),
    ("CONFIG_SECURITY_SELINUX_DEVELOP", "n", "不能 permissive，正式版必须 enforcing"),
    ("CONFIG_SECURITY_SELINUX_BOOTPARAM", "n", "不允许用内核参数关掉 SELinux"),
    ("CONFIG_DM_VERITY", "y", "dm-verity，verified boot 的基础"),
    ("CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG", "y", "校验根哈希签名"),
    ("CONFIG_MD", "y", "多设备（dm 依赖）"),
    ("CONFIG_BLK_DEV_DM", "y", "device mapper"),
    ("CONFIG_BLK_DEV_LOOP", "y", "loop 设备（动态分区挂载用）"),
    ("CONFIG_PSI", "y", "压力失速信息——安卓靠它做内存回收决策"),
    ("CONFIG_MEMCG", "y", "内存 cgroup（安卓的进程优先级靠它）"),
    ("CONFIG_CPUSETS", "y", "CPU 亲和（大小核调度依赖）"),
    ("CONFIG_ZRAM", "y", "内存压缩——手机内存小，这是标配"),
    ("CONFIG_ZSMALLOC", "y", "zram 的分配器"),
    ("CONFIG_INPUT_TOUCHSCREEN", "y", "触控屏"),
    ("CONFIG_REGULATOR", "y", "电源调节器（手机 SoC 必需）"),
]


def gen_kernel_fragment() -> str:
    """生成 android 内核配置片段。"""
    L = ["# 安卓设备适配片段 —— 由 qyos/android.py 生成",
         "#",
         "# 与桌面内核的差别主要在：Binder IPC、DMA-BUF heaps、dm-verity、",
         "# SELinux 强制、zram、省电与大小核调度。",
         "# 缺 Binder 或缺图形内存堆，设备能起但硬件服务全废（相机/显示/音频）。",
         ""]
    for k, v, why in ANDROID_KERNEL_ITEMS:
        # 注释放单独一行而不是行内：不是所有解析器都剥行内注释，
        # 值里混进注释文字时内核会静默忽略这一项，最难排查
        L.append(f"# {why}")
        L.append(f"{k}={v}")
    L.append("")
    L.append("# 必须关闭：安卓不靠 uevent helper 做热插拔，")
    L.append("# 留着它会在早期用户空间卡住几十秒")
    L.append("# CONFIG_UEVENT_HELPER_PATH is not set")
    L.append("")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyandroid",
                                 description="LANXIU Linux 安卓设备适配")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("bootimg", help="打包 boot.img")
    sp.add_argument("--kernel", required=True)
    sp.add_argument("--ramdisk", required=True)
    sp.add_argument("--dtb", default=None)
    sp.add_argument("--cmdline", default="")
    sp.add_argument("--page-size", type=int, default=4096)
    sp.add_argument("--os-version", default="")
    sp.add_argument("--out", default="boot.img")

    sp = sub.add_parser("verify", help="自检 boot.img")
    sp.add_argument("image")

    sp = sub.add_parser("unpack", help="解出 boot.img 的 kernel/ramdisk/dtb")
    sp.add_argument("image")
    sp.add_argument("--out", default=".",
                    help="输出目录（默认当前目录）")

    sp = sub.add_parser("super", help="动态分区布局")
    sp.add_argument("--root-mb", type=int, default=3072)

    sp = sub.add_parser("flash", help="生成刷机脚本")
    sp.add_argument("--slot", default="_a", choices=["_a", "_b"])
    sp.add_argument("--wipe", action="store_true")
    sp.add_argument("--out", default="flash.sh")

    sp = sub.add_parser("fstab", help="生成安卓设备 fstab")

    sp = sub.add_parser("kernel-fragment", help="生成安卓内核配置片段")
    sp.add_argument("--out", default=None)

    sp = sub.add_parser("notes", help="A/B 槽与原子升级的关系说明")

    a = ap.parse_args(argv)

    if a.cmd == "bootimg":
        img = BootImage(
            kernel=Path(a.kernel).read_bytes(),
            ramdisk=Path(a.ramdisk).read_bytes(),
            dtb=Path(a.dtb).read_bytes() if a.dtb else b"",
            cmdline=a.cmdline, page_size=a.page_size,
            os_version=a.os_version)
        data = build_boot_image(img)
        probs = verify_boot_image(data)
        if probs:
            for p in probs:
                util.log("err", p)
            return 1
        Path(a.out).write_bytes(data)
        util.log("ok", f"已生成 {a.out}（{len(data)} 字节，"
                       f"页 {a.page_size}，自检通过）")
        return 0

    if a.cmd == "verify":
        probs = verify_boot_image(Path(a.image).read_bytes())
        if probs:
            for p in probs:
                util.log("err", p)
            return 1
        info = parse_boot_image(Path(a.image).read_bytes())
        util.log("ok", "自检通过")
        for k, v in info.items():
            print(f"  {k:<16} {v}")
        return 0

    if a.cmd == "unpack":
        data = Path(a.image).read_bytes()
        probs = verify_boot_image(data)
        if probs:
            for p in probs:
                util.log("err", p)
            return 1
        info = parse_boot_image(data)
        page = info["page_size"]
        hdr = align_up(HEADER_SIZE, page)
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        off = hdr
        for part in ("kernel", "ramdisk", "dtb"):
            size = info.get(f"{part}_size", 0)
            if not size:
                continue
            blob = data[off:off + size]
            (out / f"{part}.img").write_bytes(blob)
            util.log("ok", f"{part}.img（{size} 字节）")
            off += align_up(size, page)
        util.log("ok", f"已解出到 {out}/")
        return 0

    if a.cmd == "super":
        print(android_super_layout(a.root_mb).report())
        return 0

    if a.cmd == "flash":
        Path(a.out).write_text(fastboot_script(slot=a.slot, wipe=a.wipe))
        util.log("ok", f"已生成 {a.out}")
        util.log("warn", "刷机会清空设备数据，执行前请确认设备正确")
        return 0

    if a.cmd == "fstab":
        print(android_fstab())
        return 0

    if a.cmd == "kernel-fragment":
        text = gen_kernel_fragment()
        if a.out:
            Path(a.out).write_text(text)
            util.log("ok", f"已生成 {a.out}")
        else:
            print(text)
        return 0

    if a.cmd == "notes":
        print(ab_notes())
        return 0
    return 1
