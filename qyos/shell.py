"""桌面外壳补齐：墙纸、开发者选项、重置系统、导航方式、漂浮窗、拖拽、扫码、虚拟鼠标、连接设备、内置浏览器。

这一层是用户每天看到的东西，但它们的失败几乎全是静默的：

**墙纸**
- 设了不生效最常见的原因是**图片格式不被合成器支持**，
  或路径在用户看不到的地方（权限），而界面只显示"已设置"
- 不同形态要设不同尺寸：手机竖屏用桌面横图会被裁得只剩中间一条

**开发者选项**
- 打开 USB 调试后若 udev 规则没配，adb 会一直 unauthorized——
  用户以为"打开了没用"（这正是 qyudev known adb 要解决的）
- 开发者选项默认隐藏且要连点版本号才出现，
  因为误触会关掉安全校验

**重置系统**
- 这是唯一不可逆的操作。必须列出会被删掉什么，
  并且要求二次确认——不能只问"确定吗"

**系统导航方式**
- 手势导航和传统三键导航不能同时生效。
  两套都开着会导致底部区域手势与按钮互相抢，
  表现为"有时候点了没反应"

**自由漂浮**（自由窗口）
- 手机上的自由窗口要限制最小尺寸，
  否则窗口会被缩到看不见却还在占资源

**内容拖拽**
- 跨应用拖拽在 Wayland 上必须走 portal。
  不走的话能拖但松手后什么都没发生——没有任何报错

**扫码**
- 扫码需要相机独占。相机被占用时扫码会黑屏，
  而用户以为扫码器坏了（与 qymedia 的独占仲裁联动）

**虚拟鼠标**
- 触屏/无障碍场景用。没有它，
  只有触屏的设备无法完成右键、精确点击等操作

**连接设备**
- 配对过的设备列表要能查。看不出配对过什么，
  就没法清理残留配对（而残留配对是连接失败的常见原因）

**内置浏览器**
- 浏览器是系统里最大的一块外部代码。
  必须说明它的更新不走发行版包管理——
  否则用户以为系统更新了浏览器也会跟着更
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class ShellError(RuntimeError):
    pass


# ---------------------------------------------------------------- 墙纸

# 形态 → 期望分辨率。手机竖屏用桌面横图会被裁得只剩中间一条
WALLPAPER_SPECS = {
    "phone": (1080, 2400, "竖屏，高是宽的 2 倍多"),
    "tablet": (1600, 2560, "竖屏为主"),
    "desktop": (1920, 1080, "横屏"),
    "workstation": (2560, 1440, "横屏高分"),
}

# 合成器支持的格式。不支持的格式会静默失败
WP_FORMATS = {".png": "PNG（最通用）", ".jpg": "JPEG",
              ".jpeg": "JPEG", ".webp": "WebP（部分合成器不支持）"}


def wallpaper_check(path: Path, form: str = "desktop") -> list:
    """设墙纸前的检查。"""
    problems = []
    p = Path(path)
    if not p.exists():
        problems.append(f"图片不存在: {p}")
        return problems
    suf = p.suffix.lower()
    if suf not in WP_FORMATS:
        problems.append(
            f"格式 {suf} 可能不被合成器支持——"
            f"**设了不生效最常见的原因就是这个**，而界面仍显示'已设置'。"
            f"支持：{'、'.join(WP_FORMATS)}")
    if suf == ".webp":
        problems.append("WebP 在部分合成器上会静默失败，"
                        "建议转成 PNG")
    # 权限：图片在当前用户读不到的地方，合成器（另一用户）读不了
    import os
    if not os.access(str(p), os.R_OK):
        problems.append(f"当前用户读不了 {p}——"
                        f"合成器也读不了，会静默失败")
    exp = WALLPAPER_SPECS.get(form)
    if exp:
        problems.append(
            f"  {form} 期望 {exp[0]}x{exp[1]}（{exp[2]}）"
            f"——尺寸不符会被裁切或拉伸")
    return problems


def wallpaper_cmd(path: Path, output: str = "*") -> str:
    p = Path(path)
    return (f"# 设墙纸后要确认真的生效：只显示'已设置'不代表生效\n"
            f"swaybg -o {output} -i {p} -m fill\n"
            f"# 或用 swww（支持淡入）：swww img {p}\n"
            f"# 验证：swww query")


def wallpaper_report(path: Path, form: str = "desktop") -> str:
    probs = wallpaper_check(path, form)
    L = [f"墙纸 {path}（{form}）：", ""]
    if not probs:
        L.append("  检查通过")
    for x in probs:
        mark = "  " if x.startswith("  ") else "! "
        L.append(f"{mark}{x}")
    L.append("")
    L.append(wallpaper_cmd(path))
    return "\n".join(L)


# ---------------------------------------------------------------- 开发者选项

@dataclass
class DevOption:
    id: str
    title: str
    cmd: str
    risky: bool = False
    desc: str = ""
    # 需要配套的东西。不做配套的话"打开了没用"
    needs: str = ""


DEV_OPTIONS = [
    DevOption("usb-debug", "USB 调试", "开启 adbd 并允许调试",
              desc="打开后若 udev 规则没配，adb 会一直 unauthorized",
              needs="qyudev known adb"),
    DevOption("wireless-debug", "无线调试", "adbd 监听 TCP 端口",
              risky=True,
              desc="同一局域网内任何人都能连——用完必须关",
              needs="qynet adb-tcpip"),
    DevOption("stay-awake", "充电时不休眠", "充电时保持唤醒",
              desc="排查时用，长期开着会加速电池老化"),
    DevOption("force-gpu", "强制 GPU 渲染", "强制走 GPU 合成",
              risky=True,
              desc="驱动不成熟时会花屏或闪退"),
    DevOption("show-touches", "显示触摸位置", "屏幕上显示触点",
              desc="演示与录屏用"),
    DevOption("anim-off", "关闭动画", "把动画时长设为 0",
              desc="让界面显得更快，但不影响真实性能"),
    DevOption("verify-off", "禁用签名校验", "允许安装未签名包",
              risky=True,
              desc="**关掉后装什么都行，等于放弃供应链防护**"),
    DevOption("logcat", "查看系统日志", "实时滚动日志",
              desc="排障首选"),
]


def dev_report() -> str:
    L = ["开发者选项：", ""]
    L.append("  默认隐藏：需在'关于本机'连点版本号 7 次才出现。")
    L.append("  **这样设计的理由是误触会关掉安全校验**，")
    L.append("  而不是为了显得专业。")
    L.append("")
    for o in DEV_OPTIONS:
        tag = " [高风险]" if o.risky else ""
        L.append(f"  {o.id:<16}{o.title}{tag}")
        L.append(f"      {o.desc}")
        if o.needs:
            L.append(f"      配套: {o.needs}")
    return "\n".join(L)


def dev_enable(oid: str) -> dict:
    o = next((x for x in DEV_OPTIONS if x.id == oid), None)
    if o is None:
        raise ShellError(f"没有这个选项 {oid}")
    if o.id == "usb-debug":
        return {"ok": True,
                "hint": "开启后必须配 udev 规则，否则 adb 一直 unauthorized",
                "cmd": "qyudev known adb"}
    if o.id == "wireless-debug":
        return {"ok": True,
                "hint": "同一局域网内任何人都能连，用完必须关",
                "cmd": "qynet adb-tcpip"}
    return {"ok": True, "hint": o.desc, "cmd": o.cmd}


def dev_risky() -> list:
    return [o.id for o in DEV_OPTIONS if o.risky]


# ---------------------------------------------------------------- 重置系统

# 重置会删掉什么。不列清楚就让用户执行是不可接受的
RESET_SCOPES = {
    "apps": "已安装的第三方应用（系统自带应用保留）",
    "data": "用户数据：文档、图片、下载、音乐、视频",
    "accounts": "已登录的账号与凭据",
    "settings": "全部系统设置（恢复默认）",
    "pairings": "蓝牙/WiFi 配对记录",
    "keys": "SSH 密钥、GPG 密钥、加密密钥",
}


def reset_report(scope: list) -> str:
    L = ["重置系统", ""]
    L.append("  !! 这是唯一不可逆的操作。开始之前请确认：")
    L.append("")
    for k in scope:
        if k in RESET_SCOPES:
            L.append(f"  将被删除: {RESET_SCOPES[k]}")
    L.append("")
    L.append("  密钥类（SSH/GPG/加密密钥）删掉后")
    L.append("  **无法找回，且已加密的数据永久打不开**。")
    L.append("  这是重置最严重的后果，必须单独列出。")
    return "\n".join(L)


def reset_check(scope: list, confirmed: bool = False) -> list:
    problems = []
    if not scope:
        problems.append("没有指定重置范围")
        return problems
    if not confirmed:
        problems.append("需要显式确认（--yes）。"
                        "只问'确定吗'不够，必须列出会被删掉什么")
    if "keys" in scope:
        problems.append(
            "范围包含密钥——删掉后已加密的数据永久打不开。"
            "若还有加密卷，先确认能挂载并备份")
    return problems


# ---------------------------------------------------------------- 导航方式

NAV_MODES = {
    "gesture": ("手势导航", "底部上滑返回/回桌面/最近任务"),
    "three-button": ("三键导航", "返回/主页/最近任务 三个按钮"),
    "two-button": ("两键导航", "返回 + 主页"),
}


def navigation_check(current: str, also_enabled: list) -> list:
    """导航方式：两套同时开会导致底部区域互抢。"""
    problems = []
    if current not in NAV_MODES:
        problems.append(f"没有这种导航方式 {current}"
                        f"（可选：{'、'.join(NAV_MODES)}）")
        return problems
    if also_enabled:
        others = [NAV_MODES[x][0] for x in also_enabled if x in NAV_MODES]
        if others:
            problems.append(
                f"当前是 {NAV_MODES[current][0]}，"
                f"但同时启用了 {'、'.join(others)}——"
                f"**两套同时开会导致底部区域手势与按钮互抢**，"
                f"表现为'有时候点了没反应'。只用一套")
    return problems


# ---------------------------------------------------------------- 自由漂浮

FLOAT_MIN_PX = 160      # 手机上再小就看不见了
FLOAT_MAX_RATIO = 0.9   # 不能超过屏幕的 90%


def floating_check(w: int, h: int, screen_w: int, screen_h: int) -> list:
    problems = []
    if w < FLOAT_MIN_PX or h < FLOAT_MIN_PX:
        problems.append(
            f"窗口 {w}x{h} 小于下限 {FLOAT_MIN_PX}px——"
            f"会被缩到看不见却还在占资源")
    if w > screen_w * FLOAT_MAX_RATIO or h > screen_h * FLOAT_MAX_RATIO:
        problems.append(
            f"窗口超过屏幕 {int(FLOAT_MAX_RATIO*100)}%——"
            f"那就不是漂浮窗口了，应该用最大化")
    if w > screen_w or h > screen_h:
        problems.append(f"窗口 {w}x{h} 超出屏幕 {screen_w}x{screen_h}——"
                        f"会有部分内容点不到")
    return problems


# ---------------------------------------------------------------- 拖拽

def drag_check(src_app: str, dst_app: str, kind: str,
               via_portal: bool = True) -> list:
    """跨应用拖拽。Wayland 上不走 portal 会静默失败。"""
    problems = []
    if not via_portal and src_app != dst_app:
        problems.append(
            "跨应用拖拽没有走 portal——"
            "**能拖但松手后什么都没发生，且没有任何报错。**"
            "Wayland 下必须走 xdg-desktop-portal")
    if kind not in ("file", "text", "url", "image"):
        problems.append(f"不支持拖拽类型 {kind}"
                        f"（可选：file、text、url、image）")
    if kind == "file" and not via_portal:
        problems.append("拖文件必须走 portal："
                        "应用之间不能直接交换文件路径")
    return problems


# ---------------------------------------------------------------- 扫码

def scan_check(camera_busy: bool = False,
               camera_permission: str = "allow") -> list:
    """扫码前检查。"""
    problems = []
    if camera_busy:
        problems.append(
            "相机正被其他程序占用——扫码会黑屏。"
            "**用户会以为扫码器坏了**，实际是相机被占。"
            "（见 qymedia request 的独占仲裁）")
    if camera_permission != "allow":
        problems.append(
            f"相机权限是 {camera_permission}——扫码会黑屏且不报错。"
            f"开启：qyperms grant <扫码应用> camera")
    return problems


def scan_cmd() -> str:
    return ("# 扫码依赖相机独占：先申请再扫\n"
            "qymedia request /dev/video0 scanner\n"
            "# 扫完释放，否则其他程序拿不到相机\n"
            "qymedia release /dev/video0 scanner")


# ---------------------------------------------------------------- 虚拟鼠标

VIRTUAL_MOUSE_ACTIONS = ["left-click", "right-click", "middle-click",
                         "scroll", "drag", "move"]


def virtual_mouse_check(actions: list, screen_w: int = 0,
                        screen_h: int = 0) -> list:
    problems = []
    for a in actions:
        if a not in VIRTUAL_MOUSE_ACTIONS:
            problems.append(f"不支持的虚拟鼠标动作 {a}"
                            f"（可选：{'、'.join(VIRTUAL_MOUSE_ACTIONS)}）")
    if not actions:
        problems.append("没有指定要模拟的动作")
    return problems


def virtual_mouse_report() -> str:
    return ("虚拟鼠标：\n"
            "  用途：触屏设备的右键与精确点击、无障碍操作。\n"
            f"  支持: {'、'.join(VIRTUAL_MOUSE_ACTIONS)}\n"
            "  # 没有它，纯触屏设备无法完成右键、\n"
            "  # 精确点击、框选等操作——而这些操作在\n"
            "  # 桌面软件里大量存在")


# ---------------------------------------------------------------- 连接设备

DEV_KINDS = {"bluetooth": "蓝牙", "wifi-direct": "WiFi 直连",
             "usb": "USB", "paired-phone": "配对手机"}


def paired_devices(root: Path = Path("/")) -> list:
    """已配对设备。看不出配对过什么就无法清理残留配对。"""
    p = Path(root) / "var" / "lib" / "qypaired" / "devices.json"
    if not p.exists():
        return []
    try:
        return json.loads(p.read_text()).get("devices", [])
    except Exception:
        return []


def paired_report(root: Path = Path("/")) -> str:
    devs = paired_devices(root)
    L = ["已连接/已配对的设备：", ""]
    if not devs:
        L.append("  （空）")
        L.append("")
        L.append("  看不出配对过什么，就无法清理残留配对——")
        L.append("  **而残留配对是重连失败的常见原因**")
        return "\n".join(L)
    for d in devs:
        L.append(f"  {d.get('kind', '?'):<14}{d.get('name', '?')}")
    return "\n".join(L)


def stale_pairing_hint() -> str:
    return ("配对连不上时的排查：\n"
            "  1. 先在两端都删除配对，再重新配对\n"
            "  **只在一边删会留下残留配对，之后永远连不上**\n"
            "  2. 确认没有别的设备正连着它（多数设备只支持一个连接）\n"
            "  3. 蓝牙要在可发现模式")


# ---------------------------------------------------------------- 内置浏览器

BROWSER_NOTE = (
    "浏览器是系统里最大的一块外部代码，"
    "它的更新不走发行版包管理——"
    "**用户会以为系统更新了浏览器也跟着更**，"
    "实际上两者是分开的。这一点必须写明白，"
    "否则安全更新会被误认为已经覆盖浏览器漏洞。"
)


def browser_report() -> str:
    L = ["内置浏览器：", ""]
    L.append(f"  {BROWSER_NOTE}")
    L.append("")
    L.append("  因此：")
    L.append("   - 浏览器版本要单独查，不能看系统版本")
    L.append("   - 浏览器漏洞要单独跟，不能只等 qysec scan")
    L.append("   - 浏览器自带沙箱，与 qyperms 的权限是两套")
    return "\n".join(L)


# ---------------------------------------------------------------- 主命令

def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyshell",
                                 description="LANXIU Linux 桌面外壳")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("wallpaper", help="墙纸检查")
    sp.add_argument("path")
    sp.add_argument("--form", default="desktop",
                    choices=list(WALLPAPER_SPECS))
    sub.add_parser("dev", help="开发者选项")
    sp = sub.add_parser("dev-enable", help="开启某项")
    sp.add_argument("opt", choices=[o.id for o in DEV_OPTIONS])
    sub.add_parser("dev-risky", help="列出高风险项")
    sp = sub.add_parser("reset", help="重置系统")
    sp.add_argument("--scope", nargs="+",
                    choices=list(RESET_SCOPES), default=[])
    sp.add_argument("--yes", action="store_true")
    sp = sub.add_parser("navigation", help="导航方式")
    sp.add_argument("mode", choices=list(NAV_MODES))
    sp.add_argument("--also", nargs="*", default=[])
    sp = sub.add_parser("floating", help="自由漂浮窗口")
    sp.add_argument("w", type=int)
    sp.add_argument("h", type=int)
    sp.add_argument("--screen", nargs=2, type=int, default=[1080, 2400])
    sp = sub.add_parser("drag", help="内容拖拽检查")
    sp.add_argument("src")
    sp.add_argument("dst")
    sp.add_argument("--kind", default="file",
                    choices=["file", "text", "url", "image"])
    sp.add_argument("--no-portal", action="store_true")
    sp = sub.add_parser("scan", help="扫码检查")
    sp.add_argument("--camera-busy", action="store_true")
    sp.add_argument("--permission", default="allow")
    sub.add_parser("scan-cmd", help="扫码命令")
    sp = sub.add_parser("vmouse", help="虚拟鼠标")
    sp.add_argument("actions", nargs="*")
    sp = sub.add_parser("paired", help="已配对设备")
    sp.add_argument("--hint", action="store_true")
    sub.add_parser("browser", help="内置浏览器说明")
    sp = sub.add_parser("framework", help="输入法/指纹/虚拟键盘框架")
    sp.add_argument("which", nargs="?", default="")
    sp.add_argument("--pkg-missing", action="store_true")
    sp.add_argument("--env-missing", action="store_true")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "wallpaper":
            print(wallpaper_report(Path(a.path), a.form))
            return 0
        if a.cmd == "dev":
            print(dev_report())
            return 0
        if a.cmd == "dev-enable":
            r = dev_enable(a.opt)
            util.log("ok", f"已开启 {a.opt}")
            if r["hint"]:
                util.log("info", r["hint"])
            if r["cmd"]:
                util.log("info", f"配套: {r['cmd']}")
            return 0
        if a.cmd == "dev-risky":
            print("高风险开发者选项（误触会关掉安全校验）：")
            for x in dev_risky():
                print(f"  {x}")
            return 0
        if a.cmd == "reset":
            print(reset_report(a.scope))
            probs = reset_check(a.scope, a.yes)
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "可以执行（仍建议先备份）")
            return 1 if probs else 0
        if a.cmd == "navigation":
            probs = navigation_check(a.mode, a.also)
            for x in probs:
                util.log("err", x)
            if not probs:
                name, desc = NAV_MODES[a.mode]
                util.log("ok", f"{name}：{desc}")
            return 1 if probs else 0
        if a.cmd == "floating":
            sw, sh = a.screen
            probs = floating_check(a.w, a.h, sw, sh)
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "尺寸合适")
            return 1 if probs else 0
        if a.cmd == "drag":
            probs = drag_check(a.src, a.dst, a.kind,
                               via_portal=not a.no_portal)
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "可以拖拽")
            return 1 if probs else 0
        if a.cmd == "scan":
            probs = scan_check(a.camera_busy, a.permission)
            for x in probs:
                util.log("err", x)
            if not probs:
                util.log("ok", "可以扫码")
            return 1 if probs else 0
        if a.cmd == "scan-cmd":
            print(scan_cmd())
            return 0
        if a.cmd == "vmouse":
            probs = virtual_mouse_check(list(a.actions))
            for x in probs:
                util.log("err", x)
            if not probs:
                print(virtual_mouse_report())
            return 1 if probs else 0
        if a.cmd == "paired":
            print(paired_report(root))
            if a.hint:
                print()
                print(stale_pairing_hint())
            return 0
        if a.cmd == "browser":
            print(browser_report())
            return 0
        if a.cmd == "framework":
            if a.pkg_missing or a.env_missing:
                for fid in ([a.which] if a.which
                            else [f.id for f in FRAMEWORKS]):
                    probs = framework_check(
                        fid, not a.pkg_missing, not a.env_missing)
                    for x in probs:
                        util.log("err", x)
                return 1
            print(framework_report(a.which))
            return 0
    except ShellError as e:
        util.log("err", str(e))
        return 1
    return 1

# ---------------------------------------------------------------- 输入法等框架

@dataclass
class Framework:
    """一个"装了包还得配框架"的能力。

    这一组最容易踩的坑是：**包装了但没配框架，
    表现为点了没反应且不报错**。用户不会说"输入法没配"，
    只会说"打不出中文"。
    """
    id: str
    title: str
    pkg: str              # 需要的包
    env: str              # 需要设的环境变量
    check: str            # 怎么确认生效
    symptom: str          # 没配好时的表现
    desc: str = ""


FRAMEWORKS = [
    Framework("ime", "输入法", "fcitx5",
              env="GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx "
                  "XMODIFIERS=@im=fcitx",
              check="fcitx5-diagnose",
              symptom="切不出中文，只能输英文。"
                      "**用户不会说'输入法没配'，只会说'打不出中文'**",
              desc="环境变量没设的话，输入框里根本唤不出候选栏"),
    Framework("fingerprint", "指纹解锁", "fprintd",
              env="—（走 PAM）",
              check="fprintd-enroll",
              symptom="PAM 静默跳过指纹，用户以为没配上，"
                      "反复重试直到账户锁定。"
                      "**且多数消费级传感器没有 Linux 驱动**",
              desc="必须先在 PAM 里加入 pam_fprintd.so"),
    Framework("vkeyboard", "虚拟键盘", "wvkbd",
              env="—（由合成器唤起）",
              check="文本输入框获得焦点时应自动弹出",
              symptom="触屏设备没有虚拟键盘等于没法输入任何东西，"
                      "而用户往往不知道要自己装"),
]


def framework_report(fid: str = "") -> str:
    items = [f for f in FRAMEWORKS if not fid or f.id == fid]
    if not items:
        return f"没有框架 {fid}（可用：{'、'.join(f.id for f in FRAMEWORKS)}）"
    L = []
    for f in items:
        L.append(f"{f.title}（{f.pkg}）：")
        L.append(f"  需要的包: {f.pkg}")
        if f.env != "—（由合成器唤起）" and not f.env.startswith("—"):
            L.append(f"  环境变量: {f.env}")
        L.append(f"  确认生效: {f.check}")
        L.append(f"  没配好的表现: {f.symptom}")
        if f.desc:
            L.append(f"  说明: {f.desc}")
        L.append("")
    return "\n".join(L).rstrip()


def framework_check(fid: str, pkg_installed: bool,
                    env_set: bool = True) -> list:
    """检查框架是否真的可用。"""
    f = next((x for x in FRAMEWORKS if x.id == fid), None)
    if f is None:
        raise ShellError(f"没有框架 {fid}")
    problems = []
    if not pkg_installed:
        problems.append(
            f"包 {f.pkg} 未安装——装它：qypkg install {f.pkg}")
        return problems
    if not env_set:
        problems.append(
            f"{f.title} 的包装了但环境变量没设——"
            f"**点了没反应且不报错**。需要：{f.env}")
    return problems
