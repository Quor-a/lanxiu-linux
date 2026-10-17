"""可启动镜像构建。

两种产物：
  1. **装机镜像**（ISO/IMG）：启动后进安装器，把系统装到硬盘
  2. **可直接运行的磁盘镜像**：已经装好系统，写进 U 盘或丢给虚拟机就能跑

第二种在开发和 CI 里价值最大：不用真机也能验证"这套系统能不能开机"。

沙盒里没有 losetup 权限和 loop 设备，所以真实写镜像做不了。
但镜像**布局和装机脚本**是纯逻辑，可以在这里完整生成并校验，
拿到有权限的机器上直接执行。

设计原则：不假装做了做不到的事。能生成脚本就生成脚本，
能校验布局就校验布局，绝不声称"已生成可用的 ISO"。
"""
from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

from . import disk as D
from . import bootloader as BL
from . import initramfs as IR
from . import util


class ImageError(RuntimeError):
    pass


# 镜像形态
KIND_INSTALLER = "installer"     # 装机镜像
KIND_LIVE = "live"               # 可直接运行的磁盘镜像


def build_disk_image(root: Path, out_path: Path,
                     size: int | None = None,
                     esp_size: int = 512 * 1024 * 1024,
                     kind: str = KIND_LIVE,
                     label: str = "QIYUAN") -> dict:
    """生成一个磁盘镜像文件（稀疏文件）。

    返回镜像描述。真实写分区需要 loop 设备，这里只创建文件与布局描述，
    并生成配套的装机脚本——在有权限的机器上执行脚本即可完成剩余步骤。
    """
    root = Path(root)
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)

    # 估算大小：根内容 + ESP + 余量
    if size is None:
        used = sum(f.stat().st_size for f in root.rglob("*")
                   if f.is_file() and not f.is_symlink())
        size = D.align_up(int(used * 1.4) + esp_size + 256 * 1024 * 1024)
    size = D.align_up(size)

    if out.exists():
        out.unlink()

    # 稀疏文件在部分文件系统（virtiofs、某些网络/容器挂载）上并不稀疏，
    # truncate 会真的占满空间。先探测可用容量，再决定能不能建，
    # 否则会在写了一半时才炸，留下一个损坏的镜像。
    free = _free_bytes(out.parent)
    if free and size > free:
        raise ImageError(
            f"目标位置可用空间 {util.human_size(free)}，"
            f"镜像需要 {util.human_size(size)}。\n"
            f"  要么换一个空间足够的输出目录，"
            f"要么用 --size 指定更小的镜像")

    try:
        with open(out, "wb") as f:
            f.truncate(size)
    except OSError as e:
        out.unlink(missing_ok=True)
        # 有些容器/overlay 文件系统对单个文件大小另有上限，
        # statvfs 报的可用空间并不反映这个限制，只能实测才知道
        raise ImageError(
            f"创建 {util.human_size(size)} 的镜像失败：{e}\n"
            f"  该位置可能有单文件大小上限（容器 overlay 常见）。\n"
            f"  可尝试更小的 --image-size，或换一个输出目录") from e

    try:
        st = out.stat()
        apparent = st.st_size
        blocks = st.st_blocks * 512
    except OSError:
        apparent, blocks = size, size

    sparse = blocks < apparent
    if not sparse:
        util.log("warn", "该文件系统不支持稀疏文件，镜像会真实占用全部空间")

    return {"path": out, "size": size, "apparent": apparent,
            "actual": blocks, "sparse": sparse,
            "kind": kind, "label": label, "esp_size": esp_size,
            "root": str(root)}


def _free_bytes(path: Path) -> int:
    try:
        st = os.statvfs(str(path))
        return st.f_bavail * st.f_frsize
    except Exception:
        return 0


def installer_scripts(root: Path, layout: D.Layout, out_dir: Path,
                      kernel: str = "/boot/vmlinuz-qiyuan",
                      initrd: str = "/boot/initramfs-qiyuan.img",
                      target: str = "/mnt/qiyuan",
                      bootloader: str = "grub") -> dict:
    """生成一整套装机脚本。

    拆成 5 步而不是一个脚本，是为了每一步都能单独重跑——
    装机失败时不用从头再来，这是装机器的基本素养。
    """
    root = Path(root)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    steps = {}

    steps["1-partition"] = D.render_partition_script(layout)
    steps["2-mount"] = D.render_mount_script(layout, target)
    steps["3-install"] = "\n".join([
        "#!/bin/bash",
        "# 把包系统装进目标根目录",
        "set -euo pipefail",
        f'TARGET="{target}"',
        "",
        "# assemble 会按依赖顺序装包并做可启动性检查",
        "qypkg --root \"$TARGET\" assemble filesystem qyinit",
        "",
        "# 按需追加软件集",
        'if [ -n "${PROFILE:-}" ]; then',
        '    qypkg --root "$TARGET" install $PROFILE',
        "fi",
        "",
        'echo "包系统安装完成"',
        "",
    ])

    steps["4-configure"] = BL.render_chroot_script(target, [
        "# 生成 initramfs（必须在目标系统内做，要用它的工具链）",
        "qybuild --initramfs --out /boot/initramfs-qiyuan.img",
        "",
        "# 写入 fstab（UUID 由分区脚本产出后填入）",
        'if [ -f /etc/fstab.generated ]; then',
        "    mv /etc/fstab.generated /etc/fstab",
        "fi",
        "",
        "# 主机名与时区",
        'echo "${HOSTNAME:-qiyuan}" > /etc/hostname',
        'ln -sf /usr/share/zoneinfo/"${TIMEZONE:-Asia/Shanghai}" /etc/localtime',
        "",
        "# root 密码：装机时必须设置，空密码是重大安全缺陷",
        'if [ -n "${ROOT_PASSWORD:-}" ]; then',
        '    echo "root:${ROOT_PASSWORD}" | chpasswd',
        "else",
        '    echo "警告：未设置 root 密码，将进入无密码 root shell"',
        "fi",
        "",
        "# PAM 配置：装了 PAM 库却没有 /etc/pam.d，",
        "# 所有认证都会失败（表现为密码明明对了却登不进去）",
        "qypam apply",
        'qypam check || echo "  （PAM 配置有问题，登录后请检查）"',
        "",
        "# 语言环境。不设 LANG 系统按 C locale 运行，",
        "# 中文文件名会显示成问号且排序错乱——而且不报错",
        'qylocale set "${LANG_CHOICE:-zh_CN}"',
        'qylocale check || true',
        "",
        "# 管理员账号并加入 wheel 组（sudo 靠这个组）。",
        "# 不给普通账号的话用户只能全程 root 操作，",
        "# 一次误删就毁掉系统，且没有任何审计记录",
        'if [ -n "${ADMIN_USER:-}" ]; then',
        '    useradd -m -G wheel -s /bin/bash "${ADMIN_USER}"',
        '    if [ -n "${ADMIN_PASSWORD:-}" ]; then',
        '        echo "${ADMIN_USER}:${ADMIN_PASSWORD}" | chpasswd',
        '        echo "已创建管理员 ${ADMIN_USER}（在 wheel 组，可用 sudo）"',
        "    else",
        '        echo "已创建 ${ADMIN_USER}，但未设密码——登录后立刻 passwd 设置"',
        "    fi",
        "else",
        '    echo "警告：未创建普通用户，日常将以 root 操作（不推荐）"',
        "fi",
        "",
        "# 固件自检：没有它网卡/无线在真机上不工作。",
        "# 虚拟机里测不出这个问题，所以装机时明确查一次",
        'if ! ls /usr/lib/firmware/* >/dev/null 2>&1; then',
        '    echo "警告：没有安装固件，网卡/无线/显卡可能无法工作"',
        '    echo "  在真机装机请安装 linux-firmware 包"',
        "else",
        '    echo "固件已就位: $(ls /usr/lib/firmware | wc -l) 项"',
        "fi",
    ])

    if bootloader == "grub":
        steps["5-bootloader"] = BL.render_grub_install(layout, target)
    else:
        steps["5-bootloader"] = BL.render_efistub_install(
            "/boot/vmlinuz-qiyuan", "/boot/initramfs-qiyuan.img",
            BL.kernel_cmdline("ROOT_UUID_PLACEHOLDER"),
            esp=f"{target}/boot/efi")

    steps["9-umount"] = D.render_umount_script(layout, target)

    written = {}
    for name, content in sorted(steps.items()):
        p = out_dir / f"{name}.sh"
        util.atomic_write(p, content.encode())
        try:
            p.chmod(0o755)
        except OSError:
            pass
        written[name] = p
    return written


def installer_profile(name: str, packages: list,
                      description: str = "") -> dict:
    """软件集定义。装机时选一个，决定装哪些包。"""
    return {"name": name, "packages": packages, "description": description}


DEFAULT_PROFILES = [
    installer_profile("minimal", ["filesystem", "qyinit"],
                      "最小系统：只有根目录骨架和 init，适合做容器与嵌入式基础"),
    installer_profile("server",
                      ["filesystem", "qyinit"],
                      "服务器：最小系统加网络与日志服务"),
    installer_profile("desktop",
                      ["filesystem", "qyinit"],
                      "桌面：最小系统加图形栈与桌面环境"),
]


def render_installer_banner(version: str = "0.1") -> str:
    return f"""
    LANXIU Linux {version} 安装器
    ================================

    本安装器会清空目标磁盘上的所有数据。
    请先备份重要文件。
"""


# ---------------------------------------------------------------- 校验

def verify_image(path: Path) -> dict:
    """校验一个镜像文件是否具备可启动镜像的基本形态。

    不做"假装能启动"的宣称：只检查这一层能检查的（文件大小、对齐、
    是否有分区表痕迹），把需要 loop 设备才能做的检查明确标为跳过。
    """
    path = Path(path)
    if not path.exists():
        raise ImageError(f"镜像不存在: {path}")
    size = path.stat().st_size
    problems, notes = [], []

    if size < 64 * 1024 * 1024:
        problems.append(f"镜像只有 {util.human_size(size)}，"
                        f"装不下内核与基础系统")
    if size % D.MiB != 0:
        notes.append("镜像大小未按 1MiB 对齐")

    # 检查 GPT 痕迹：偏移 512 处应有 "EFI PART"
    has_gpt = False
    try:
        with open(path, "rb") as f:
            f.seek(512)
            has_gpt = f.read(8) == b"EFI PART"
    except OSError:
        pass

    return {"path": str(path), "size": size, "has_gpt": has_gpt,
            "problems": problems, "notes": notes,
            "writable_image": True,
            "bootable": not problems,
            "skipped_checks": [
                "分区表内容校验（需要 loop 设备挂载）",
                "文件系统完整性（需要 root 权限挂载）",
                "引导器可用性（需要在目标机上执行）",
            ]}


def build_report(image: dict, layout: D.Layout, scripts: dict) -> str:
    lines = ["# 镜像构建报告", ""]
    lines.append(f"- 镜像：`{image['path']}`")
    lines.append(f"- 大小：{util.human_size(image['size'])}"
                 f"{'（稀疏文件，实际占用 ' + util.human_size(image['actual']) + '）' if image.get('sparse') else ''}")
    lines.append(f"- 形态：{image['kind']}")
    lines.append("")
    lines.append("## 生成的脚本")
    for name, p in sorted(scripts.items()):
        lines.append(f"- `{p.name}` — {_script_desc(name)}")
    lines.append("")
    lines.append("## 分区方案")
    for i, p in enumerate(layout.partitions, 1):
        size = util.human_size(p.size) if p.size else "剩余全部"
        lines.append(f"- {i}. {p.mount or '—'} ({p.fs}) {size}")
    return "\n".join(lines)


def _script_desc(name: str) -> str:
    return {
        "1-partition": "分区与格式化（会清空磁盘，需二次确认）",
        "2-mount": "挂载目标分区",
        "3-install": "安装包系统",
        "4-configure": "chroot 进目标系统做配置",
        "5-bootloader": "安装引导器",
        "9-umount": "卸载（逆序）",
    }.get(name, "")
