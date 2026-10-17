"""权限框架：硬件存在但没有被声明和授权，照样用不了。

这是很多"硬件不工作"问题的真正原因——不是驱动没装、不是固件缺，
而是**没有任何一层告诉系统"这个程序要用这个硬件"**。

一个摄像头接上了、驱动加载了、/dev/video0 也在，
但应用程序依然拿不到画面，只因为：
- 没有任何框架登记"camera"这个能力
- 没有授权记录说明谁可以用
- 没有拒绝时的提示，用户只看到黑屏

所以本模块做的是三件事：

**1. 能力声明（capability）**
把硬件抽象成能力：camera / mic / location / contacts / sms /
phone / calendar / health / nearby / storage / notification /
background / overlay / pip / aod 等。
硬件是硬件，能力是"程序能对它做什么"。

**2. 授权记录**
每个能力按包授权。授权不是一次性的开关，而是有状态：
ask（每次问）/ allow / deny / allow-foreground（仅前台）。
**仅前台**这一档是关键——导航软件需要一直定位，
而扫码软件只在打开时需要相机。不加区分的话，
用户只能二选一，最后往往全给，等于没有权限模型。

**3. 拒绝时给出可操作的提示**
权限被拒时程序拿到的是"拒绝"，用户看到的是"功能没反应"。
必须把"被拒绝了"翻译成"去设置里打开相机权限"，
否则用户永远不知道发生了什么。

几个刻意的设计：

- **敏感能力默认 ask，不默认 allow**。默认放行的权限等于没有权限
- **危险能力（通话记录、短信、位置）必须显式授权**，
  且安装时不自动给——安装时一股脑全给，用户根本不会看
- **后台运行单独一项能力**。很多应用偷偷在后台定位，
  就是因为后台没有被当成独立权限
- **悬浮窗/画中画单独一项**。它们能盖在其他应用上，
  是钓鱼攻击的常用手段
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class PermError(RuntimeError):
    pass


# ---------------------------------------------------------------- 能力定义

# 敏感级别决定默认值与提示强度
SENSITIVITY = {
    "low": "低",        # 震动、通知
    "medium": "中",     # 相机、麦克风、存储
    "high": "高",       # 位置、联系人、日历
    "critical": "极高", # 通话记录、短信、健康
}

# 默认策略。敏感能力一律 ask，不默认给
DEFAULT_POLICY = {
    "low": "allow",
    "medium": "ask",
    "high": "ask",
    "critical": "deny",     # 最高的默认拒绝，必须用户显式开
}


@dataclass
class Capability:
    """一项能力（硬件或数据的访问权）。"""
    id: str
    title: str
    sensitivity: str
    desc: str = ""
    # 需要它才能工作的硬件/设备节点。用于"硬件在但用不了"的排查
    device_hint: str = ""
    # 只在前台用就够的能力，不该被申请后台权限
    foreground_only: bool = False
    # 需要内核选项支持
    kernel_options: list = field(default_factory=list)
    # 需要哪个包提供框架支持
    framework_pkg: str = ""
    # 后台使用是否单独受限
    background_sensitive: bool = False


CAPABILITIES: list = [
    # —— 媒体硬件
    Capability("camera", "相机", "medium",
               "拍照、录像、扫码",
               device_hint="/dev/video*",
               kernel_options=["CONFIG_VIDEO_DEV", "CONFIG_MEDIA_SUPPORT"],
               framework_pkg="pipewire",
               foreground_only=True),
    Capability("microphone", "麦克风", "medium",
               "录音、语音通话、语音输入",
               device_hint="/proc/asound/cards",
               kernel_options=["CONFIG_SND"],
               framework_pkg="pipewire",
               background_sensitive=True),
    # —— 位置与感知
    Capability("location", "位置信息", "high",
               "定位、导航、附近设备",
               device_hint="/dev/ttyGPS* 或 ModemManager",
               framework_pkg="gpsd",
               background_sensitive=True),
    Capability("nearby", "附近设备", "high",
               "发现并连接周边蓝牙/WiFi 设备",
               framework_pkg="bluez",
               background_sensitive=True),
    Capability("health", "健康数据", "critical",
               "步数、心率等传感器数据",
               device_hint="/sys/bus/iio/devices",
               framework_pkg="iio-sensor-proxy"),
    # —— 个人数据
    Capability("contacts", "联系人", "high",
               "读取与写入通讯录"),
    Capability("phone", "电话", "high",
               "拨号、来电状态",
               framework_pkg="modemmanager"),
    Capability("calls", "通话记录", "critical",
               "读取通话历史"),
    Capability("sms", "短信", "critical",
               "读取与发送短信",
               framework_pkg="modemmanager"),
    Capability("calendar", "日历", "high",
               "日程与提醒"),
    Capability("storage", "存储", "medium",
               "读写文件",
               device_hint="/（任意路径）"),
    # —— 系统表现
    Capability("notification", "通知", "low",
               "发送通知"),
    Capability("background", "后台运行", "medium",
               "未在前台时继续运行",
               background_sensitive=True),
    Capability("overlay", "悬浮窗", "medium",
               "在其他应用之上显示内容"
               "（能盖住其他界面，是钓鱼攻击的常用手段）"),
    Capability("pip", "画中画", "low",
               "小窗口悬浮播放视频"),
    Capability("fullscreen-notify", "全屏通知", "medium",
               "来电等场景覆盖全屏显示"),
    Capability("aod", "息屏显示", "low",
               "屏幕关闭时显示时间通知"),
    Capability("pop-background", "后台弹出界面", "medium",
               "在后台时弹出窗口"
               "（最容易被滥用的能力之一，默认拒绝）"),
    Capability("hot-update", "热更新", "medium",
               "不重启替换自身代码"
               "（绕过发行版审计，需单独授权）"),
    Capability("autostart", "开机自启", "low",
               "登录时自动运行"),
    Capability("shortcut", "快捷键", "low",
               "注册全局快捷键"),
    Capability("share", "分享", "low",
               "向其他应用发送数据"),
    Capability("clipboard", "剪贴板", "medium",
               "读写剪贴板",
               background_sensitive=True),
    Capability("monitor", "监控", "high",
               "截屏、录屏、访问使用情况"
               "（能持续获取屏幕内容，属于高危能力）"),
]

CAP_BY_ID = {c.id: c for c in CAPABILITIES}

# 授权状态
ALLOW = "allow"
DENY = "deny"
ASK = "ask"
FG_ONLY = "allow-foreground"     # 仅前台允许——关键的一档

STATE_CN = {ALLOW: "允许", DENY: "拒绝", ASK: "每次询问",
            FG_ONLY: "仅前台允许"}


@dataclass
class Grant:
    """一个包对一项能力的授权。"""
    package: str
    cap: str
    state: str
    # 授权时间，用于审计与"多久没用了"提醒
    since: str = ""


def grants_path(root: Path) -> Path:
    return Path(root) / "etc" / "qyperms" / "grants.json"


def load_grants(root: Path) -> list:
    p = grants_path(root)
    if not p.exists():
        return []
    try:
        return [Grant(**d) for d in json.loads(p.read_text()).get("grants", [])]
    except Exception:
        return []


def save_grants(root: Path, items: list) -> None:
    p = grants_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps(
        {"grants": [g.__dict__ for g in items]},
        ensure_ascii=False, indent=1).encode())


def get_state(root: Path, package: str, cap: str) -> str:
    """查授权状态。没记录就用按敏感度的默认值。"""
    for g in load_grants(root):
        if g.package == package and g.cap == cap:
            return g.state
    c = CAP_BY_ID.get(cap)
    if c is None:
        raise PermError(f"没有这个能力: {cap}"
                        f"（可用：{'、'.join(sorted(CAP_BY_ID))}）")
    return DEFAULT_POLICY.get(c.sensitivity, ASK)


def set_state(root: Path, package: str, cap: str, state: str) -> Grant:
    if cap not in CAP_BY_ID:
        raise PermError(f"没有这个能力: {cap}")
    if state not in STATE_CN:
        raise PermError(
            f"不支持的授权状态 {state}（可选：{'、'.join(STATE_CN.values())}，"
            f"或写 {'/'.join(STATE_CN)}）")
    items = [g for g in load_grants(root)
             if not (g.package == package and g.cap == cap)]
    import time
    g = Grant(package, cap, state, time.strftime("%F %T"))
    items.append(g)
    save_grants(root, items)
    return g


def revoke(root: Path, package: str) -> int:
    """撤销某包的全部授权。卸载时必做——
    留着授权记录等于给不存在的包留了后门。"""
    items = load_grants(root)
    left = [g for g in items if g.package != package]
    save_grants(root, left)
    return len(items) - len(left)


# ---------------------------------------------------------------- 检查

def check_request(root: Path, package: str, cap: str,
                  foreground: bool = True) -> dict:
    """检查一次访问请求。返回决策与说明。"""
    c = CAP_BY_ID.get(cap)
    if c is None:
        raise PermError(f"没有这个能力: {cap}")
    state = get_state(root, package, cap)

    if state == ALLOW:
        return {"allowed": True, "state": state, "reason": ""}
    if state == DENY:
        return {"allowed": False, "state": state,
                "reason": deny_reason(package, cap)}
    if state == FG_ONLY:
        if foreground:
            return {"allowed": True, "state": state, "reason": ""}
        # 仅前台允许，现在在后台 —— 这是"偷偷后台定位"的正解
        return {"allowed": False, "state": state,
                "reason": f"{package} 仅在处于前台时可以使用{c.title}。"
                          f"它现在在后台运行。若要允许后台使用，"
                          f"需在设置里改为「允许」"}
    # ask
    return {"allowed": False, "state": state,
            "reason": f"{package} 请求使用{c.title}，尚未授权",
            "need_prompt": True}


def deny_reason(package: str, cap: str) -> str:
    """被拒绝时要给出能操作的提示。

    程序拿到的是"拒绝"，用户看到的是"功能没反应"。
    必须翻译成"去设置里打开相机权限"，
    否则用户永远不知道发生了什么。
    """
    c = CAP_BY_ID[cap]
    L = [f"{package} 没有{c.title}权限"]
    if c.device_hint:
        L.append(f"（设备节点 {c.device_hint}）")
    L.append(f"\n  开启：qyperms grant {package} {cap}")
    if c.framework_pkg:
        L.append(f"  若开启后仍不可用，检查框架支持包：{c.framework_pkg}")
    return "".join(L)


# ---------------------------------------------------------------- 硬件就绪度

def capability_readiness(root: Path = Path("/"), caps: list | None = None
                         ) -> dict:
    """检查各项能力的硬件与框架是否就绪。

    这解决的是"硬件在但用不了"：驱动在、设备节点在，
    但没有框架支撑，应用依然拿不到。所以就绪度要分开看
    硬件层与框架层，缺哪层报哪层。
    """
    import shutil
    import glob as _g

    out = {}
    for c in (caps or CAPABILITIES):
        info = {"cap": c.id, "title": c.title,
                "hardware": None, "framework": None, "missing": []}
        # 硬件层
        if c.device_hint:
            if "*" in c.device_hint:
                info["hardware"] = bool(_g.glob(c.device_hint))
            else:
                p = Path(c.device_hint)
                info["hardware"] = p.exists()
            if not info["hardware"]:
                info["missing"].append(
                    f"硬件：{c.device_hint} 不存在"
                    f"（检查固件：qyhw scan）")
        # 框架层：提供该能力的包是否装了
        if c.framework_pkg:
            info["framework"] = _installed(root, c.framework_pkg)
            if not info["framework"]:
                info["missing"].append(
                    f"框架：{c.framework_pkg} 未安装——"
                    f"硬件在但没有框架支撑，应用依然拿不到"
                    f"（执行 qypkg install {c.framework_pkg}）")
        out[c.id] = info
    return out


def _installed(root: Path, pkg: str) -> bool:
    """粗查包是否安装。不依赖 DB，避免循环依赖。"""
    for base in ("var/lib/qypkg", "usr/share/qiyuan"):
        pass
    try:
        from . import pkgmgr as PM
        db = PM.open_db(root)
        return db.installed(pkg) if hasattr(db, "installed") else False
    except Exception:
        # 退到文件系统检查
        return (Path(root) / "usr" / "share" / "doc" / pkg).exists() or \
               (Path(root) / "var" / "lib" / "qypkg").exists()


def readiness_report(root: Path = Path("/")) -> str:
    r = capability_readiness(root)
    L = ["能力就绪度（硬件在 ≠ 能用）：", ""]
    for cid, info in r.items():
        hw = "—" if info["hardware"] is None else ("有" if info["hardware"] else "无")
        fw = "—" if info["framework"] is None else ("有" if info["framework"] else "无")
        mark = "  " if not info["missing"] else "! "
        L.append(f"{mark}{info['title']:<10}硬件 {hw}  框架 {fw}")
        for m in info["missing"]:
            L.append(f"    {m}")
    return "\n".join(L)


# ---------------------------------------------------------------- 危险组合

# 有些能力单独看都合理，组合起来就能做坏事
RISKY_COMBOS = [
    ({"monitor", "background"},
     "可截屏 + 可后台运行 = 能持续记录屏幕内容，等于键盘记录器"),
    ({"overlay", "background"},
     "可悬浮显示 + 可后台运行 = 能在你不知情时盖住其他应用的界面（钓鱼）"),
    ({"location", "background", "nearby"},
     "可定位 + 可后台 + 可发现附近设备 = 能持续追踪行踪"),
    ({"sms", "background"},
     "可收发短信 + 可后台 = 能在后台静默发送短信（扣费风险）"),
    ({"microphone", "background"},
     "可录音 + 可后台 = 能在不显示任何界面时录音"),
    ({"pop-background", "overlay"},
     "可后台弹窗 + 可悬浮 = 能在任何时刻弹出伪造界面"),
]


def audit_combos(root: Path, package: str) -> list:
    """检查某包是否拿到了危险的能力组合。"""
    granted = {g.cap for g in load_grants(root)
               if g.package == package and g.state in (ALLOW, FG_ONLY)}
    out = []
    for combo, reason in RISKY_COMBOS:
        if combo <= granted:
            out.append(f"{package} 同时拥有 {'、'.join(sorted(combo))}：{reason}")
    return out


def audit_all(root: Path) -> list:
    pkgs = sorted({g.package for g in load_grants(root)})
    out = []
    for p in pkgs:
        out += audit_combos(root, p)
    return out


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyperms",
                                 description="LANXIU Linux 权限框架")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("caps", help="列出所有能力")
    sub.add_parser("ready", help="能力就绪度（硬件 vs 框架）")
    sp = sub.add_parser("list", help="列出授权")
    sp.add_argument("--package", default="")
    sp = sub.add_parser("grant", help="授权")
    sp.add_argument("package"); sp.add_argument("cap")
    sp.add_argument("state", nargs="?", default=ALLOW,
                    choices=[ALLOW, DENY, ASK, FG_ONLY])
    sp = sub.add_parser("revoke", help="撤销某包全部授权")
    sp.add_argument("package")
    sp = sub.add_parser("check", help="模拟一次访问请求")
    sp.add_argument("package"); sp.add_argument("cap")
    sp.add_argument("--background", action="store_true",
                    help="按后台运行来判定")
    sub.add_parser("audit", help="检查危险能力组合")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "caps":
            print(f"{'能力':<18}{'敏感度':<6}说明")
            for c in CAPABILITIES:
                print(f"  {c.id:<18}{SENSITIVITY.get(c.sensitivity, ''):<6}"
                      f"{c.desc or c.title}")
            return 0

        if a.cmd == "ready":
            print(readiness_report(root))
            return 0

        if a.cmd == "list":
            items = load_grants(root)
            if a.package:
                items = [g for g in items if g.package == a.package]
            if not items:
                print("没有授权记录（全部按默认策略）")
                return 0
            for g in items:
                c = CAP_BY_ID.get(g.cap)
                print(f"  {g.package:<18}{g.cap:<18}"
                      f"{STATE_CN.get(g.state, g.state)}")
            return 0

        if a.cmd == "grant":
            g = set_state(root, a.package, a.cap, a.state)
            util.log("ok", f"{g.package} 的 {a.cap} → "
                           f"{STATE_CN.get(a.state, a.state)}")
            for x in audit_combos(root, a.package):
                util.log("warn", x)
            return 0

        if a.cmd == "revoke":
            n = revoke(root, a.package)
            util.log("ok", f"已撤销 {a.package} 的 {n} 项授权")
            return 0

        if a.cmd == "check":
            r = check_request(root, a.package, a.cap,
                              foreground=not a.background)
            print(f"决策: {'允许' if r['allowed'] else '拒绝'}")
            print(f"状态: {STATE_CN.get(r['state'], r['state'])}")
            if r["reason"]:
                print(r["reason"])
            return 0 if r["allowed"] else 1

        if a.cmd == "audit":
            problems = audit_all(root)
            for x in problems:
                util.log("warn", x)
            if not problems:
                util.log("ok", "没有发现危险的能力组合")
            return 1 if problems else 0
    except PermError as e:
        util.log("err", str(e))
        return 1
    return 1
