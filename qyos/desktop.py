"""桌面外壳：状态栏、设置面板、工具调用。

没有这一层，用户拿到的桌面只是"能画窗口"——
改个时区要开终端敲命令，连 WiFi 要手写配置文件。

三块内容：

**状态栏**：把系统当前状态显示出来（网络、音量、电池、时间、输入法）。
它只读，不改任何东西——只读是刻意的，状态栏一旦能改配置，
就成了提权攻击的入口。

**设置面板**：改系统配置。改配置需要权限，所以**必须走 polkit**，
不能给面板 root。直接给 root 的话，任何能画界面的程序都能改系统配置；
走 polkit 后每次提权有单独授权提示、有审计记录、可配置成"每次询问"。

**工具调用**：把 qypkg / qynet / qylocale / qyhw / qyapp 这些命令
包装成桌面能调用的动作。每个动作声明：
  - 是否需要提权（决定走不走 polkit）
  - 是否需要网络（决定能不能离线用）
  - 会不会重启服务（决定要不要先警告用户）

最后这一条很关键：用户点"改时区"不会预期它重启 cron，
但改时区确实要重启定时任务。不提前说明，用户会觉得"系统自己抽风"。
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class DesktopError(RuntimeError):
    pass


@dataclass
class Action:
    """一个可被桌面调用的动作。"""
    id: str
    title: str
    command: str
    privileged: bool = False      # 需要提权 → 走 polkit
    needs_network: bool = False
    restarts_service: str = ""    # 会重启哪个服务（要提前告知用户）
    panel: str = "settings"       # 属于哪个面板
    group: str = "系统"
    desc: str = ""

    def warn(self) -> str:
        if self.restarts_service:
            return (f"会重启 {self.restarts_service}——"
                    f"正在进行的工作可能中断")
        return ""


# 桌面可调动作。每一项都对应一个真实的用户需求
ACTIONS: list = [
    # 网络
    Action("net.wifi", "连接 WiFi", "qynet wifi", privileged=True,
           panel="network", group="网络"),
    Action("net.status", "网络状态", "qynet ports", panel="network",
           group="网络"),
    Action("net.firewall", "防火墙", "qynet firewall", privileged=True,
           panel="network", group="网络", restarts_service="nftables"),
    Action("net.ssh", "远程连接（SSH）", "qynet ssh", privileged=True,
           panel="network", group="网络", restarts_service="sshd"),
    # 时间与地区
    Action("time.locale", "语言与键盘", "qylocale set", privileged=True,
           panel="region", group="时间地区"),
    Action("time.timezone", "时区", "qyinit set-timezone", privileged=True,
           panel="region", group="时间地区", restarts_service="定时任务"),
    Action("time.ntp", "时间同步", "qynet add-port chrony 123 --proto udp",
           privileged=True, panel="region", group="时间地区",
           needs_network=True),
    # 软件
    Action("pkg.install", "安装软件", "qypkg install", privileged=True,
           panel="software", group="软件", needs_network=True),
    Action("pkg.upgrade", "系统更新", "qypkg upgrade", privileged=True,
           panel="software", group="软件", needs_network=True,
           restarts_service="已更新的服务"),
    Action("pkg.sources", "软件源", "qysource list", privileged=True,
           panel="software", group="软件"),
    Action("app.audit", "通用包权限", "qyapp audit", panel="software",
           group="软件"),
    # 硬件
    Action("hw.scan", "检测硬件", "qyhw scan", privileged=True,
           panel="hardware", group="硬件"),
    Action("hw.firmware", "安装固件", "qypkg install linux-firmware",
           privileged=True, panel="hardware", group="硬件",
           needs_network=True),
    # 文件
    Action("files.open", "打开", "qyfiles open", panel="files", group="文件"),
    Action("files.extract", "解压", "qyfiles extract", panel="files",
           group="文件"),
    # 系统
    Action("sys.ports", "端口管理", "qynet ports", privileged=True,
           panel="system", group="系统"),
    Action("sys.rollback", "回滚上次更新", "qypkg rollback",
           privileged=True, panel="system", group="系统",
           restarts_service="系统"),
    # 电源：关机重启这类不该开终端敲命令
    Action("power.shutdown", "关机", "qyctl power-do shutdown",
           privileged=True, panel="power", group="电源"),
    Action("power.reboot", "重启", "qyctl power-do reboot",
           privileged=True, panel="power", group="电源"),
    Action("power.suspend", "挂起", "qyctl power-do suspend",
           privileged=True, panel="power", group="电源"),
    Action("power.hibernate", "休眠", "qyctl power-do hibernate",
           privileged=True, panel="power", group="电源"),
    # 显示与声音
    Action("disp.brightness", "亮度", "qyctl brightness --percent",
           privileged=True, panel="display", group="显示"),
    Action("disp.rotate", "旋转屏幕", "qyctl display", panel="display",
           group="显示"),
    Action("disp.screenshot", "截图", "qyctl screenshot", panel="display",
           group="显示"),
    Action("disp.record", "屏幕录制", "qyctl record", panel="display",
           group="显示"),
    Action("disp.cast", "投屏", "qyctl cast", panel="display", group="显示"),
    Action("snd.output", "声音输出", "qyctl audio", panel="sound",
           group="声音"),
    # 设备
    Action("dev.usb", "USB 设备", "qyctl devices", panel="devices",
           group="设备"),
    Action("dev.camera", "摄像头", "qyctl devices", panel="devices",
           group="设备"),
    Action("dev.fingerprint", "指纹", "qypkg install fprintd",
           privileged=True, panel="devices", group="设备",
           needs_network=True),
    Action("dev.nfc", "NFC", "qypkg install neard", privileged=True,
           panel="devices", group="设备", needs_network=True),
    Action("dev.gps", "定位（GPS）", "qypkg install gpsd",
           privileged=True, panel="devices", group="设备",
           needs_network=True),
    # 网络
    Action("net.vpn", "VPN", "qyctl vpn", panel="network", group="网络"),
    Action("net.dns", "私人 DNS", "qyctl privdns", panel="network",
           group="网络", privileged=True),
    Action("net.wlan", "无线开关", "qyctl wlan-on", privileged=True,
           panel="network", group="网络"),
    # 硬件底层：模块没加载、设备没权限，上面全是空的
    Action("hw.kmod", "内核模块", "qykmod initramfs", privileged=True,
           panel="hardware", group="硬件"),
    Action("hw.udev", "设备权限", "qyudev access", privileged=True,
           panel="hardware", group="硬件"),
    Action("hw.input", "触摸板与输入设备", "qyio touchpad",
           panel="hardware", group="硬件"),
    Action("hw.screen", "多显示器", "qyio screen", panel="hardware",
           group="硬件"),
    Action("hw.print", "打印机与扫描仪", "qyio scanner", panel="hardware",
           group="硬件"),
    Action("hw.raid", "RAID 状态", "qyio raid", panel="hardware",
           group="硬件"),
    Action("hw.tpm", "安全芯片", "qyio tpm", panel="hardware", group="硬件"),
    Action("hw.sensor", "传感器", "qysensor scan", panel="hardware",
           group="硬件"),
    Action("hw.fan", "风扇曲线", "qysensor fan", privileged=True,
           panel="hardware", group="硬件"),
    Action("hw.backlight", "屏幕与键盘背光", "qysensor backlight",
           privileged=True, panel="hardware", group="硬件"),
    Action("hw.thunderbolt", "雷电安全等级", "qysensor thunderbolt",
           privileged=True, panel="hardware", group="硬件"),
    Action("hw.flight", "飞行模式", "qysensor flight --on",
           privileged=True, panel="hardware", group="硬件"),
    Action("hw.sensor", "传感器", "qysensor scan", panel="hardware",
           group="硬件"),
    Action("hw.fan", "风扇曲线", "qysensor fan", privileged=True,
           panel="hardware", group="硬件"),
    Action("hw.backlight", "屏幕与键盘背光", "qysensor backlight",
           privileged=True, panel="hardware", group="硬件"),
    Action("hw.thunderbolt", "雷电安全等级", "qysensor thunderbolt",
           privileged=True, panel="hardware", group="硬件"),
    Action("hw.flight", "飞行模式", "qysensor flight --on",
           privileged=True, panel="hardware", group="硬件"),
    # 权限与隐私
    Action("perm.caps", "应用权限", "qyperms list", panel="privacy",
           group="权限"),
    Action("perm.audit", "危险权限组合", "qyperms audit", panel="privacy",
           group="权限"),
    Action("perm.ready", "能力就绪度", "qyperms ready", panel="privacy",
           group="权限"),
    # 通知与窗口
    Action("notify.dnd", "免打扰", "qynotify dnd", panel="notification",
           group="通知"),
    Action("notify.aod", "息屏显示", "qynotify aod", panel="notification",
           group="通知"),
    Action("notify.pip", "画中画", "qynotify matrix", panel="notification",
           group="通知"),
    # 媒体
    Action("media.ready", "相机麦克风状态", "qymedia ready", panel="media",
           group="媒体"),
    # 关于本机
    Action("about.info", "关于本机", "qyctl overview", panel="about",
           group="本机"),
    # 桌面外观与日常操作
    Action("ui.wallpaper", "墙纸", "qyshell wallpaper", panel="appearance",
           group="外观"),
    Action("ui.nav", "系统导航方式", "qyshell navigation", panel="appearance",
           group="外观"),
    Action("ui.float", "自由漂浮窗口", "qyshell floating", panel="appearance",
           group="外观"),
    Action("ui.drag", "内容拖拽", "qyshell drag", panel="appearance",
           group="外观"),
    Action("ui.framework", "输入法与指纹框架", "qyshell framework",
           panel="appearance", group="外观"),
    Action("dev.options", "开发者选项", "qyshell dev", panel="developer",
           group="开发者"),
    Action("dev.reset", "重置系统", "qyshell reset --yes",
           privileged=True, panel="developer", group="开发者"),
    Action("dev.browser", "内置浏览器", "qyshell browser", panel="developer",
           group="开发者"),
    Action("tool.proc", "进程管理", "qyproc ps", panel="tools", group="工具"),
    Action("tool.cpu", "CPU 与调频", "qyproc cpu", panel="tools", group="工具"),
    Action("tool.gpu", "显卡", "qyproc gpu", panel="tools", group="工具"),
    Action("tool.usage", "包使用时间", "qyproc usage", panel="tools",
           group="工具"),
    Action("tool.registry", "工具一览", "qyproc tools", panel="tools",
           group="工具"),
    Action("about.thermal", "温度", "qyctl thermal", panel="about",
           group="本机"),
    Action("about.battery", "电池", "qyctl battery", panel="about",
           group="本机"),
]


@dataclass
class TrayItem:
    """状态栏上的一个条目。只读，不修改任何东西。"""
    id: str
    title: str
    source: str          # 从哪读状态（命令或文件路径）
    icon: str = ""
    order: int = 50


# 状态栏条目。全部只读——状态栏能改配置就等于开了提权口子
TRAY: list = [
    TrayItem("network", "网络", "qynet ports", icon="network", order=10),
    TrayItem("bluetooth", "蓝牙", "/sys/class/bluetooth", icon="bt", order=20),
    TrayItem("audio", "音量", "qyapp list", icon="audio", order=30),
    TrayItem("battery", "电池", "/sys/class/power_supply", icon="battery",
             order=40),
    TrayItem("clock", "时间", "date", icon="clock", order=90),
    TrayItem("power", "电源", "qyctl battery", icon="battery", order=45),
    TrayItem("thermal", "温度", "qyctl thermal", icon="thermal", order=85),
]


def panels() -> list:
    """返回所有面板（按出现顺序去重）。"""
    seen, out = set(), []
    for a in ACTIONS:
        if a.panel not in seen:
            seen.add(a.panel)
            out.append(a.panel)
    return out


PANEL_CN = {
    "network": "网络", "region": "时间地区", "software": "软件",
    "hardware": "硬件", "files": "文件", "system": "系统",
    "power": "电源", "display": "显示", "sound": "声音",
    "devices": "设备", "about": "关于本机",
    "privacy": "权限与隐私", "notification": "通知与窗口", "media": "媒体",
    "appearance": "外观与操作", "developer": "开发者", "tools": "工具",
}


def actions_of(panel: str) -> list:
    return [a for a in ACTIONS if a.panel == panel]


# ---------------------------------------------------------------- polkit

def polkit_policy(actions: list | None = None) -> str:
    """生成 polkit 策略。

    每条提权动作一条规则。默认 auth_admin（输管理员密码），
    而不是 yes（直接放行）——默认放行的策略等于没有策略。
    """
    acts = [a for a in (actions or ACTIONS) if a.privileged]
    L = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<!DOCTYPE policyconfig PUBLIC "-//freedesktop//DTD PolicyKit '
         'Policy Configuration 1.0//EN"',
         ' "http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">',
         '<policyconfig>',
         '',
         '  <vendor>LANXIU Linux</vendor>',
         '  <vendor_url>https://qiyuan-linux.org</vendor_url>',
         '',
         '  <!-- 默认 auth_admin：每次提权都要输管理员密码。',
         '       改成 yes 等于取消授权，任何能画界面的程序都能改系统配置 -->',
         '']
    for a in acts:
        aid = a.id.replace(".", "-")
        L += [
            f'  <action id="org.qiyuan.desktop.{aid}">',
            f'    <description>{a.title}</description>',
            f'    <message>需要管理员权限才能{a.title}</message>',
            '    <defaults>',
            '      <allow_any>auth_admin</allow_any>',
            '      <allow_inactive>auth_admin</allow_inactive>',
            '      <allow_active>auth_admin</allow_active>',
            '    </defaults>',
            '  </action>',
            '',
        ]
    L.append('</policyconfig>')
    return "\n".join(L) + "\n"


def write_polkit(root: Path, actions: list | None = None) -> Path:
    d = Path(root) / "usr" / "share" / "polkit-1" / "actions"
    d.mkdir(parents=True, exist_ok=True)
    p = d / "org.qiyuan.desktop.policy"
    util.atomic_write(p, polkit_policy(actions).encode())
    return p


def check_polkit(root: Path) -> list:
    """检查提权动作是否都有对应策略。

    有提权动作但没有策略，polkit 默认拒绝——
    表现为"点按钮没反应"，用户完全不知道是缺策略。
    """
    p = Path(root) / "usr" / "share" / "polkit-1" / "actions" \
        / "org.qiyuan.desktop.policy"
    problems = []
    if not p.exists():
        return ["没有 polkit 策略文件——所有提权动作都会被默认拒绝，"
                "表现为点按钮没反应"]
    text = p.read_text()
    for a in ACTIONS:
        if not a.privileged:
            continue
        aid = a.id.replace(".", "-")
        if f'id="org.qiyuan.desktop.{aid}"' not in text:
            problems.append(f"{a.id}（{a.title}）需要提权但没有对应策略")
    if "auth_admin" not in text:
        problems.append("策略里没有 auth_admin——默认放行等于取消授权")
    return problems


# ---------------------------------------------------------------- 桌面文件

def desktop_entry(a: Action) -> str:
    """生成一个动作的 .desktop 文件。"""
    exec_ = a.command
    if a.privileged:
        # 走 pkexec：提权有单独提示与审计
        exec_ = "pkexec " + exec_
    return "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        f"Name={a.title}",
        f"Comment={a.desc or a.title}",
        f"Exec={exec_}",
        f"Categories=Settings;X-LANXIU-{a.panel};",
        "Icon=preferences-system",
        "Terminal=false",
        "NoDisplay=true",
    ]) + "\n"


def install_layout(destdir: str | Path) -> list:
    """把桌面外壳的布局写进安装目录。"""
    root = Path(destdir)
    out = []

    d = root / "usr" / "share" / "applications"
    d.mkdir(parents=True, exist_ok=True)
    for a in ACTIONS:
        p = d / f"qiyuan-{a.id.replace('.', '-')}.desktop"
        util.atomic_write(p, desktop_entry(a).encode())
        out.append(str(p))

    # 动作清单：桌面启动时读它来构建面板，
    # 这样加一个动作不用改桌面代码
    j = root / "usr" / "share" / "qiyuan" / "actions.json"
    j.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(j, json.dumps(
        {"actions": [a.__dict__ for a in ACTIONS],
         "tray": [t.__dict__ for t in TRAY],
         "panels": [{"id": p, "title": PANEL_CN.get(p, p)}
                    for p in panels()]},
        ensure_ascii=False, indent=1).encode())
    out.append(str(j))

    out.append(str(write_polkit(root)))
    return out


def check_layout(root: Path) -> list:
    problems = []
    j = Path(root) / "usr" / "share" / "qiyuan" / "actions.json"
    if not j.exists():
        problems.append("没有动作清单——桌面无法构建设置面板")
    problems += check_polkit(root)
    # 提权动作必须走 pkexec，否则点了会因为权限不足静默失败
    d = Path(root) / "usr" / "share" / "applications"
    if d.exists():
        for a in ACTIONS:
            if not a.privileged:
                continue
            p = d / f"qiyuan-{a.id.replace('.', '-')}.desktop"
            if p.exists() and "pkexec" not in p.read_text():
                problems.append(f"{a.id} 需提权但没走 pkexec，点击会静默失败")
    return problems


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qydesktop",
                                 description="LANXIU Linux 桌面外壳")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("actions", help="列出可调动作")
    sub.add_parser("panels", help="列出面板")
    sp = sub.add_parser("panel", help="查看某面板的动作")
    sp.add_argument("which", metavar="面板名")
    sub.add_parser("tray", help="状态栏条目")
    sub.add_parser("polkit", help="生成 polkit 策略")
    sub.add_parser("check", help="检查布局")

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "actions":
        for x in ACTIONS:
            marks = []
            if x.privileged:
                marks.append("需提权")
            if x.needs_network:
                marks.append("需联网")
            tag = f"  [{'、'.join(marks)}]" if marks else ""
            print(f"  {x.id:<18}{x.title:<14}{x.group}{tag}")
            w = x.warn()
            if w:
                print(f"       ! {w}")
        return 0

    if a.cmd == "panels":
        for p in panels():
            n = len(actions_of(p))
            print(f"  {p:<12}{PANEL_CN.get(p, p):<8}{n} 个动作")
        return 0

    if a.cmd == "panel":
        items = actions_of(a.which)
        if not items:
            util.log("err", f"没有面板 {a.which}"
                            f"（可用：{'、'.join(panels())}）")
            return 1
        print(f"{PANEL_CN.get(a.which, a.which)}：")
        for x in items:
            print(f"  {x.title:<16}{x.command}")
            w = x.warn()
            if w:
                print(f"     ! {w}")
        return 0

    if a.cmd == "tray":
        print("状态栏条目（全部只读）：")
        for t in sorted(TRAY, key=lambda x: x.order):
            print(f"  {t.title:<8}{t.source}")
        return 0

    if a.cmd == "polkit":
        print(polkit_policy(), end="")
        return 0

    if a.cmd == "check":
        problems = check_layout(root)
        for x in problems:
            util.log("err", x)
        if not problems:
            util.log("ok", "桌面外壳布局完整")
        return 1 if problems else 0
    return 1
