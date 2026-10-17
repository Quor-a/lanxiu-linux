"""系统形态（profile）封装。

同一个发行版可以长成不同样子：容器里跑的最小系统、服务器、桌面、
开发工作站。差别不在内核和包管理器，而在**选哪些包 + 怎么配置**。

这里把形态定义成数据，好处是：
  * 装机时选一个形态，装哪些包是确定的、可复现的
  * 形态自带的配置（服务开关、内核参数、默认服务集）跟着包一起走
  * 包库扩大后，形态是唯一需要人工维护的"精选清单"

刻意做对的一件事：形态必须**声明它排除了什么**。
只写"包含桌面"不写"不装打印服务"，用户拿到手才知道少了什么。
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class ProfileError(RuntimeError):
    pass


@dataclass
class Profile:
    """一个系统形态。"""
    name: str
    title: str
    description: str = ""
    packages: list = field(default_factory=list)      # 必装
    extra_packages: list = field(default_factory=list)  # 可选（装机时勾选）
    excludes: list = field(default_factory=list)      # 明确排除
    services: list = field(default_factory=list)      # 默认启用的服务
    kernel_fragments: list = field(default_factory=list)
    sysctl: dict = field(default_factory=dict)
    min_disk: int = 4 * 1024 * 1024 * 1024
    min_memory: int = 512 * 1024 * 1024
    initramfs: bool = True
    graphical: bool = False

    def all_packages(self) -> list:
        return list(dict.fromkeys(self.packages + self.extra_packages))

    def to_dict(self) -> dict:
        return {
            "name": self.name, "title": self.title,
            "description": self.description,
            "packages": self.packages, "extra_packages": self.extra_packages,
            "excludes": self.excludes, "services": self.services,
            "kernel_fragments": self.kernel_fragments, "sysctl": self.sysctl,
            "min_disk": self.min_disk, "min_memory": self.min_memory,
            "initramfs": self.initramfs, "graphical": self.graphical,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Profile":
        return cls(**{k: v for k, v in d.items() if k in cls.__dataclass_fields__})


# ---------------------------------------------------------------- 内置形态

PROFILES: dict = {}


def _register(p: Profile) -> Profile:
    PROFILES[p.name] = p
    return p


# 所有形态都必须带的四项。缺任何一项，系统在真机上要么起不来、
# 要么没法用：
#   linux-firmware — 没它网卡/无线/显卡不工作（虚拟机里测不出）
#   pam            — 没它 login/su/sudo 全部不成立
#   sudo           — 没它没有权限提升，只能全程 root
#   glibc-locales  — 没它中文直接乱码
# minimal 例外：容器镜像不需要固件，且要保持最小
ESSENTIAL = ["pam", "sudo", "glibc-locales"]


def _with_essentials(pkgs: list, firmware: bool = True) -> list:
    out = list(ESSENTIAL)
    if firmware:
        out.insert(0, "linux-firmware")
    return out + [x for x in pkgs if x not in ESSENTIAL]


_register(Profile(
    name="minimal",
    title="最小系统",
    description="只有根目录骨架、init 与基础工具。适合容器镜像、"
                "嵌入式设备和作为其他形态的地基。",
    packages=_with_essentials(["filesystem", "qyinit"], firmware=False),
    excludes=["图形栈", "打印服务", "蓝牙", "办公套件", "固件（容器镜像不需要）"],
    services=[],
    kernel_fragments=["base-x86_64", "hardening"],
    min_disk=2 * 1024 * 1024 * 1024,
    min_memory=256 * 1024 * 1024,
    graphical=False,
))

_register(Profile(
    name="server",
    title="服务器",
    description="最小系统加网络、日志、远程管理。无图形界面，"
                "默认开启串口控制台——服务器排查故障靠它。",
    packages=_with_essentials(["filesystem", "qyinit"]),
    extra_packages=["ssh-server", "log-daemon", "ntp-client"],
    excludes=["图形栈", "桌面环境", "蓝牙", "办公套件"],
    services=["hostname", "sysctl"],
    kernel_fragments=["base-x86_64", "hardening", "container"],
    sysctl={
        "net.ipv4.ip_forward": "1",
        "net.ipv4.conf.all.rp_filter": "1",
        "kernel.dmesg_restrict": "1",
        "net.core.somaxconn": "1024",
    },
    min_disk=8 * 1024 * 1024 * 1024,
    min_memory=1 * 1024 * 1024 * 1024,
    graphical=False,
))

_register(Profile(
    name="desktop",
    title="桌面",
    description="带图形栈与桌面环境。默认启用声音、网络管理、"
                "电源管理与打印。",
    packages=_with_essentials(["filesystem", "qyinit"]),
    extra_packages=["gui-base", "desktop-env", "audio", "network-manager",
                    "power-manager", "printing"],
    excludes=["服务器守护进程"],
    services=["hostname", "sysctl"],
    kernel_fragments=["base-x86_64", "hardening", "desktop"],
    sysctl={
        "kernel.dmesg_restrict": "1",
        "vm.swappiness": "10",
    },
    min_disk=20 * 1024 * 1024 * 1024,
    min_memory=4 * 1024 * 1024 * 1024,
    graphical=True,
))

_register(Profile(
    name="workstation",
    title="开发工作站",
    description="桌面形态加开发工具链。适合本机开发与构建。",
    packages=_with_essentials(["filesystem", "qyinit"]),
    extra_packages=["gui-base", "desktop-env", "audio", "network-manager",
                    "power-manager", "printing", "toolchain", "vcs",
                    "editor", "containers"],
    # 工作站不预装服务器守护进程（需要时可单独安装），这一点必须写清楚，
    # 否则用户不知道为什么机器上没有 sshd
    excludes=["服务器守护进程（可按需单独安装）"],
    services=["hostname", "sysctl"],
    kernel_fragments=["base-x86_64", "hardening", "desktop", "container"],
    sysctl={"kernel.dmesg_restrict": "1", "vm.swappiness": "10",
            "fs.inotify.max_user_watches": "524288"},
    min_disk=40 * 1024 * 1024 * 1024,
    min_memory=8 * 1024 * 1024 * 1024,
    graphical=True,
))


def get(name: str) -> Profile:
    if name not in PROFILES:
        raise ProfileError(
            f"未知形态: {name}（可用: {', '.join(sorted(PROFILES))}）")
    return PROFILES[name]


def load_dir(path: Path) -> dict:
    """从目录加载自定义形态（JSON），覆盖或补充内置形态。"""
    out = dict(PROFILES)
    for p in sorted(Path(path).glob("*.json")):
        try:
            d = json.loads(p.read_text())
        except (json.JSONDecodeError, OSError) as e:
            raise ProfileError(f"形态文件 {p} 解析失败: {e}") from e
        prof = Profile.from_dict(d)
        out[prof.name] = prof
    return out


def validate(profile: Profile, available: set) -> dict:
    """校验形态：列出的包是否都存在，资源需求是否合理。

    形态里写了个不存在的包，装机到一半才失败是最难受的——
    这里提前查出来。
    """
    errors, warnings = [], []
    for pkg in profile.all_packages():
        if pkg not in available:
            warnings.append(f"形态 {profile.name} 引用了配方库里没有的包: {pkg}")
    if not profile.packages:
        errors.append(f"形态 {profile.name} 没有必装包")
    if "filesystem" not in profile.packages:
        warnings.append("形态未包含 filesystem，根目录骨架可能缺失")
    if "qyinit" not in profile.packages:
        warnings.append("形态未包含 init，系统将无法启动")
    if profile.graphical and profile.min_memory < 2 * 1024 * 1024 * 1024:
        warnings.append("图形形态的内存下限低于 2G，实际跑不动桌面")
    if not profile.excludes:
        warnings.append("形态未声明排除项，用户无法预知少了什么")
    return {"ok": not errors, "errors": errors, "warnings": warnings}


def render_sysctl(profile: Profile) -> str:
    lines = [f"# 由LANXIU形态 {profile.name}（{profile.title}）生成"]
    for k, v in sorted(profile.sysctl.items()):
        lines.append(f"{k} = {v}")
    return "\n".join(lines) + "\n"


def render_summary(profiles: dict) -> str:
    lines = ["| 形态 | 标题 | 必装 | 可选 | 图形 | 最小磁盘 | 最小内存 |",
             "|---|---|---|---|---|---|---|"]
    for name, p in sorted(profiles.items()):
        lines.append(
            f"| {name} | {p.title} | {len(p.packages)} | "
            f"{len(p.extra_packages)} | {'是' if p.graphical else '否'} | "
            f"{util.human_size(p.min_disk)} | {util.human_size(p.min_memory)} |")
    return "\n".join(lines)

PROFILES["mobile"] = Profile(
    name="mobile",
    title="移动设备",
    description="安卓设备/手机/平板形态。含触控、电源管理、移动网络与"
                "zram。系统分区只读（verified boot 强制），整机走 A/B 槽更新。",
    packages=_with_essentials(["filesystem", "qyinit"]),
    extra_packages=["touch", "power-manager", "mobile-data", "audio",
                    "bluetooth", "zram-setup", "gui-base"],
    excludes=["服务器守护进程", "打印服务"],
    services=["hostname", "sysctl"],
    kernel_fragments=["base-aarch64", "hardening", "android"],
    sysctl={
        "vm.swappiness": 100,          # zram 下要积极换出才能省内存
        "vm.vfs_cache_pressure": 200,  # 内存紧张，缓存该放就放
        "kernel.dmesg_restrict": 1,
        "net.ipv4.tcp_congestion_control": "bbr",
    },
    min_disk=8 * 1024 * 1024 * 1024,
    min_memory=2 * 1024 * 1024 * 1024,
    initramfs=True,
    graphical=True,
)

