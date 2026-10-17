"""引导器配置。

装完包不等于能开机——还得让固件找到内核。这里负责生成：
  * GRUB 配置（BIOS + UEFI 双路径）
  * EFI stub 直启（内核自带 EFISTUB 时可完全不用 GRUB）
  * EFI 启动项（efibootmgr）

几个容易搞错的点：
  * 内核命令行里 root= 必须用 UUID（root=UUID=...），用设备名会漂移
  * initrd 路径必须是相对 EFI 分区根的，用反斜杠——UEFI 用 DOS 路径
  * 装完 efibootmgr 失败不致命（有些固件不接受），要能降级到默认路径启动
"""
from __future__ import annotations

from pathlib import Path

from . import util


class BootError(RuntimeError):
    pass


def kernel_cmdline(root_uuid: str, root_fs: str = "ext4",
                   resume_uuid: str | None = None,
                   extra: list | None = None, quiet: bool = True) -> str:
    """生成内核命令行。

    root= 一律用 UUID：/dev/sda2 这种写法在加装硬盘、换接口后就会指错设备，
    是"昨天还好好的今天起不来"的典型原因。
    """
    parts = [f"root=UUID={root_uuid}", f"rootfstype={root_fs}", "rw"]
    if resume_uuid:
        parts.append(f"resume=UUID={resume_uuid}")
    parts.append("init=/sbin/init")
    if quiet:
        parts.append("quiet")
    parts.extend(extra or [])
    return " ".join(parts)


def grub_cfg(kernel: str, initrd: str, cmdline: str,
             title: str = "LANXIU Linux",
             fallback_title: str = "LANXIU Linux (救援模式)",
             extra_entries: list | None = None) -> str:
    """生成 grub.cfg。

    同时给一个不带 quiet 的救援项：系统起不来时第一件事就是看内核输出，
    只给一个静默项是给自己找麻烦。
    """
    rescue = cmdline.replace("quiet", "").replace("  ", " ").strip()
    lines = [
        "# 由LANXIU安装器生成，请勿手工编辑",
        "set default=0",
        "set timeout=5",
        "insmod part_gpt",
        "insmod ext2",
        "insmod search_uuid",
        "",
        f'menuentry "{title}" {{',
        "    load_video",
        "    insmod gzio",
        "    insmod part_gpt",
        f"    linux {kernel} {cmdline}",
        f"    initrd {initrd}",
        "}",
        "",
        f'menuentry "{fallback_title}" {{',
        f"    linux {kernel} {rescue}",
        f"    initrd {initrd}",
        "}",
    ]
    for e in (extra_entries or []):
        lines += ["", f'menuentry "{e["title"]}" {{',
                  f"    linux {e['kernel']} {e['cmdline']}",
                  f"    initrd {e['initrd']}", "}"]
    return "\n".join(lines) + "\n"


def render_grub_install(layout, target: str = "/mnt/qiyuan",
                        efi_mount: str = "/boot/efi",
                        disk: str | None = None) -> str:
    """生成安装 GRUB 的脚本（在目标机上 chroot 执行）。"""
    disk = disk or layout.disk
    uefi = layout.uefi
    lines = [
        "#!/bin/bash",
        "# 安装 GRUB —— 由LANXIU安装器生成，需在 chroot 到目标系统后执行",
        "set -euo pipefail",
        f'TARGET="{target}"',
        "",
    ]
    if uefi:
        lines += [
            "# UEFI：装到 EFI 系统分区，并注册启动项",
            'mkdir -p "$TARGET/boot/efi"',
            "grub-install --target=x86_64-efi \\",
            '    --efi-directory="$TARGET/boot/efi" \\',
            '    --bootloader-id=LANXIU \\',
            "    --recheck",
            "",
            "# 有些主板固件不接受 efibootmgr 写入，失败不致命：",
            "# 固件会回退到 EFI 默认路径 /EFI/BOOT/BOOTX64.EFI，",
            "# 所以下面这一行允许失败",
            "efibootmgr --create --disk \"${DISK}\" --part 1 \\",
            "    --label 'LANXIU Linux' \\",
            "    --loader '\\EFI\\LANXIU\\grubx64.efi' || true",
            "",
            "# 兜底：再放一份到默认路径，固件找不到启动项时用它",
            'mkdir -p "$TARGET/boot/efi/EFI/BOOT"',
            'cp "$TARGET/boot/efi/EFI/LANXIU/grubx64.efi" \\',
            '   "$TARGET/boot/efi/EFI/BOOT/BOOTX64.EFI" 2>/dev/null || true',
        ]
    else:
        lines += [
            "# legacy BIOS：装到磁盘 MBR",
            f'grub-install --target=i386-pc --recheck "{layout.disk}"',
        ]
    lines += ["", 'echo "GRUB 安装完成"', ""]
    return "\n".join(lines)


def render_efistub_install(kernel_src: str, initrd_src: str, cmdline: str,
                           esp: str = "/boot/efi",
                           entry_name: str = "LANXIU") -> str:
    """生成 EFI stub 直启的脚本。

    内核开了 CONFIG_EFI_STUB 就可以让固件直接加载内核，完全跳过 GRUB。
    好处是启动链短、少一个出错环节；代价是没有启动菜单和救援项。
    """
    return "\n".join([
        "#!/bin/bash",
        "# EFI stub 直启 —— 内核自带 EFISTUB，固件直接加载内核，无需 GRUB",
        "set -euo pipefail",
        f'ESP="{esp}"',
        f'CMDLINE="{cmdline}"',
        "",
        'mkdir -p "$ESP/EFI/LANXIU"',
        f'cp {kernel_src} "$ESP/EFI/LANXIU/vmlinuz.efi"',
        f'cp {initrd_src} "$ESP/EFI/LANXIU/initramfs.img"',
        "",
        "# 把内核命令行写进内核镜像的 .cmdline 段",
        "# 这样就不依赖 GRUB 传参，固件直接加载即可",
        'printf "%s" "$CMDLINE" > /tmp/cmdline.txt',
        "objcopy \\",
        '    --add-section .cmdline=/tmp/cmdline.txt \\',
        "    --change-section-vma .cmdline=0x30000 \\",
        '    "$ESP/EFI/LANXIU/vmlinuz.efi"',
        "rm -f /tmp/cmdline.txt",
        "",
        "# 注册启动项（失败不致命，固件会回退到默认路径）",
        "efibootmgr --create --disk \"${DISK}\" --part 1 \\",
        f"    --label '{entry_name}' \\",
        f"    --loader '\\EFI\\{entry_name}\\vmlinuz.efi' || true",
        "",
        'echo "EFI stub 安装完成"',
        "",
    ])


def render_chroot_script(target: str, steps: list) -> str:
    """生成 chroot 到目标系统执行的脚本。

    装机最后一步必须在目标系统里跑（生成 initramfs、装引导器、
    设置 root 密码），因为要用到目标系统的工具和库。
    """
    lines = [
        "#!/bin/bash",
        "# 在目标系统内执行 —— 由LANXIU安装器生成",
        "set -euo pipefail",
        f'TARGET="{target}"',
        "",
        "# 让 chroot 里能正常用 /proc /sys /dev",
        "mount --bind /proc \"$TARGET/proc\"",
        "mount --bind /sys \"$TARGET/sys\"",
        "mount --bind /dev \"$TARGET/dev\"",
        "",
        "cleanup() {",
        '    umount "$TARGET/proc" 2>/dev/null || true',
        '    umount "$TARGET/sys" 2>/dev/null || true',
        '    umount "$TARGET/dev" 2>/dev/null || true',
        "}",
        "trap cleanup EXIT",
        "",
    ]
    lines += steps
    lines += ["", 'echo "目标系统内配置完成"', ""]
    return "\n".join(lines)


def image_layout_report(layout, kernel_size: int = 0,
                        initramfs_size: int = 0) -> str:
    """生成镜像布局说明（给人看的，装机前一眼看懂要做什么）。"""
    from . import disk as D
    lines = ["# 装机方案", ""]
    lines.append(f"- 目标磁盘：`{layout.disk}`")
    lines.append(f"- 分区表：{layout.table.upper()}")
    lines.append(f"- 启动方式：{'UEFI' if layout.uefi else 'legacy BIOS'}"
                 f"{'（兼容 BIOS）' if layout.bios and layout.uefi else ''}")
    lines.append(f"- 引导器：{layout.bootloader}")
    lines.append("")
    lines.append("| 分区 | 挂载点 | 文件系统 | 大小 |")
    lines.append("|---|---|---|---|")
    for i, p in enumerate(layout.partitions, 1):
        size = util.human_size(p.size) if p.size else "剩余全部"
        lines.append(f"| {i} | {p.mount or '—'} | {p.fs} | {size} |")
    if kernel_size or initramfs_size:
        lines += ["", f"内核 {util.human_size(kernel_size)} · "
                      f"initramfs {util.human_size(initramfs_size)}"]
    return "\n".join(lines)
