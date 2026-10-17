"""媒体与个人数据：相机、麦克风、音视频、图片、通讯录、通话、短信、日历、健康。

这一层的关键是**每一类数据都要有明确的归属与授权路径**。
硬件接上了、驱动在了，但没有人声明"这个数据归谁管、谁能读"，
应用拿了也用不了——这是移动场景最常见的"功能没反应"。

几件事必须做对：

**相机**
- 独占访问：两个程序同时开摄像头会互相抢，
  表现为其中一个拿到黑屏。必须由框架仲裁
- 权限提示：被拒时应用拿到的是"拒绝"，
  用户看到的是黑屏，必须翻译成"去开权限"

**麦克风**
- 与相机同理，但多一条：后台录音必须单独授权。
  不区分前后台的话，用户只能全给或全不给

**通话/短信**
- 走 ModemManager（移动网络模块）或 SIP。
  没有框架时拨号盘能打开但打不出去，
  用户会以为是没信号

**联系人/日历/通话记录**
- 这些不是文件，是结构化数据。
  每个应用自己读文件会导致格式冲突和数据损坏，
  必须有统一服务（EDS / Evolution Data Server 之类）

**健康**
- 传感器数据，走 iio。持续采集耗电，
  所以要能看出是哪个应用在持续读

**监控（截屏/录屏）**
- 属于高危能力，见 perms.py 的危险组合检查
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class MediaError(RuntimeError):
    pass


# ---------------------------------------------------------------- 类别

@dataclass
class MediaKind:
    """一类媒体或个人数据。"""
    id: str
    title: str
    cap: str            # 需要的权限能力
    framework: str      # 需要的框架包
    device: str = ""    # 相关设备节点
    desc: str = ""


KINDS: list = [
    MediaKind("camera", "相机", "camera", "pipewire",
              device="/dev/video*",
              desc="拍照、录像、扫码。独占设备，需框架仲裁"),
    MediaKind("microphone", "麦克风", "microphone", "pipewire",
              device="/proc/asound/cards",
              desc="录音、语音输入。后台录音需单独授权"),
    MediaKind("audio", "音频文件", "storage", "pipewire",
              desc="播放与解码音频"),
    MediaKind("video", "视频文件", "storage", "ffmpeg",
              desc="播放与解码视频"),
    MediaKind("picture", "图片", "storage", "gdk-pixbuf",
              desc="图片查看与缩略图"),
    MediaKind("contacts", "联系人", "contacts", "evolution-data-server",
              desc="通讯录。结构化数据，不能让应用各读各的文件"),
    MediaKind("calls", "通话记录", "calls", "modemmanager",
              desc="通话历史"),
    MediaKind("phone", "电话", "phone", "modemmanager",
              desc="拨号与来电。缺框架时拨号盘能开但打不出去"),
    MediaKind("sms", "短信", "sms", "modemmanager",
              desc="收发短信"),
    MediaKind("calendar", "日历", "calendar", "evolution-data-server",
              desc="日程与提醒"),
    MediaKind("health", "健康", "health", "iio-sensor-proxy",
              device="/sys/bus/iio/devices",
              desc="步数、心率等传感器数据。持续采集耗电"),
    MediaKind("nearby", "附近设备", "nearby", "bluez",
              desc="发现周边蓝牙/WiFi 设备"),
    MediaKind("monitor", "屏幕监控", "monitor", "xdg-desktop-portal",
              desc="截屏录屏。走 portal 授权，不能由应用自行截取"),
]

KIND_BY_ID = {k.id: k for k in KINDS}


# ---------------------------------------------------------------- 就绪度

def readiness(root: Path = Path("/")) -> dict:
    """每类数据的硬件层与框架层是否就绪。

    分开看是重点：硬件在但框架没装，应用依然拿不到。
    只报"相机可用"或"相机不可用"会把两类问题混为一谈，
    用户照着提示装了固件发现还是不行。
    """
    import glob as _g
    from . import perms as PM

    out = {}
    for k in KINDS:
        info = {"id": k.id, "title": k.title,
                "hardware": None, "framework": None, "problems": []}
        if k.device:
            if "*" in k.device:
                info["hardware"] = bool(_g.glob(k.device))
            else:
                info["hardware"] = Path(k.device).exists()
            if not info["hardware"]:
                info["problems"].append(
                    f"硬件缺失：{k.device} 不存在"
                    f"（先查固件 qyhw scan）")
        info["framework"] = _pkg_installed(root, k.framework)
        if not info["framework"]:
            info["problems"].append(
                f"框架缺失：{k.framework} 未安装——"
                f"硬件在但没有框架支撑，应用依然拿不到。"
                f"执行 qypkg install {k.framework}")
        out[k.id] = info
    return out


def _pkg_installed(root: Path, pkg: str) -> bool:
    try:
        from . import pkgmgr as Pg
        db = Pg.open_db(root)
        if hasattr(db, "installed"):
            return bool(db.installed(pkg))
    except Exception:
        pass
    return False


def readiness_report(root: Path = Path("/")) -> str:
    r = readiness(root)
    L = ["媒体与个人数据就绪度：", ""]
    for kid, info in r.items():
        hw = "—" if info["hardware"] is None else ("有" if info["hardware"] else "无")
        fw = "有" if info["framework"] else "无"
        mark = "  " if not info["problems"] else "! "
        L.append(f"{mark}{info['title']:<10}硬件 {hw}  框架 {fw}")
        for p in info["problems"]:
            L.append(f"    {p}")
    return "\n".join(L)


# ---------------------------------------------------------------- 独占访问

@dataclass
class Holder:
    """当前占用某个设备的程序。"""
    device: str
    package: str
    since: str


def holders_path(root: Path) -> Path:
    return Path(root) / "run" / "qymedia" / "holders.json"


def load_holders(root: Path) -> list:
    p = holders_path(root)
    if not p.exists():
        return []
    try:
        return [Holder(**d) for d in json.loads(p.read_text()).get("holders", [])]
    except Exception:
        return []


def request_device(root: Path, device: str, package: str) -> dict:
    """申请独占使用某设备。

    两个程序同时开摄像头会互相抢，表现为其中一个拿到黑屏——
    而且不报错，用户以为摄像头坏了。所以必须仲裁。
    """
    import time
    hs = load_holders(root)
    cur = [h for h in hs if h.device == device]
    if cur and cur[0].package != package:
        return {
            "granted": False,
            "reason": f"{device} 正被 {cur[0].package} 占用"
                      f"（自 {cur[0].since}）。"
                      f"同一时刻只有一个程序能使用，"
                      f"否则后者会拿到黑屏且不报错",
            "holder": cur[0].package,
        }
    if not cur:
        hs.append(Holder(device, package, time.strftime("%F %T")))
        p = holders_path(root)
        p.parent.mkdir(parents=True, exist_ok=True)
        util.atomic_write(p, json.dumps(
            {"holders": [h.__dict__ for h in hs]},
            ensure_ascii=False, indent=1).encode())
    return {"granted": True, "reason": ""}


def release_device(root: Path, device: str, package: str) -> bool:
    hs = load_holders(root)
    left = [h for h in hs
            if not (h.device == device and h.package == package)]
    if len(left) == len(hs):
        return False
    p = holders_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps(
        {"holders": [h.__dict__ for h in left]},
        ensure_ascii=False, indent=1).encode())
    return True


# ---------------------------------------------------------------- 应用声明

def manifest_path(root: Path, package: str) -> Path:
    return Path(root) / "usr" / "share" / "qymedia" / (package + ".json")


@dataclass
class MediaManifest:
    """一个应用用到的媒体能力声明。

    没有声明，系统就不知道它要用相机，
    于是第一次打开时才弹权限——而那时已经黑屏了一次。
    声明让系统能提前准备好提示。
    """
    package: str
    uses: list = field(default_factory=list)      # 用到的能力
    background: list = field(default_factory=list)  # 需要在后台用的
    optional: list = field(default_factory=list)    # 没有也能正常工作


def write_manifest(root: Path, m: MediaManifest) -> Path:
    p = manifest_path(root, m.package)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps(m.__dict__, ensure_ascii=False,
                                    indent=1).encode())
    return p


def check_manifest(root: Path, m: MediaManifest) -> list:
    """检查声明是否合理。返回问题列表。"""
    problems = []
    known = set(KIND_BY_ID)
    for c in m.uses + m.background + m.optional:
        if c not in known:
            problems.append(f"{m.package} 声明了未知能力 {c}")
    # 后台用的必须是 uses 的子集——
    # 声明"后台用相机"却没声明"用相机"是自相矛盾
    for c in m.background:
        if c not in m.uses:
            problems.append(
                f"{m.package} 声明后台使用 {c}，"
                f"但没有声明使用 {c}——自相矛盾")
    # 后台使用高危能力要提醒
    from . import perms as PM
    for c in m.background:
        cap = PM.CAP_BY_ID.get(c)
        if cap and cap.background_sensitive:
            problems.append(
                f"{m.package} 要在后台使用{cap.title}——"
                f"这类能力默认只允许前台使用，需用户显式授权")
    return problems


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qymedia",
                                 description="LANXIU Linux 媒体与个人数据")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("kinds", help="列出数据类别")
    sub.add_parser("ready", help="就绪度")
    sp = sub.add_parser("request", help="申请独占设备")
    sp.add_argument("device"); sp.add_argument("package")
    sp = sub.add_parser("release", help="释放设备")
    sp.add_argument("device"); sp.add_argument("package")
    sp = sub.add_parser("declare", help="写入应用声明")
    sp.add_argument("package")
    sp.add_argument("--uses", nargs="*", default=[])
    sp.add_argument("--background", nargs="*", default=[])
    sp.add_argument("--optional", nargs="*", default=[])

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "kinds":
        for k in KINDS:
            print(f"  {k.id:<12}{k.title:<10}{k.desc}")
        return 0

    if a.cmd == "ready":
        print(readiness_report(root))
        return 0

    if a.cmd == "request":
        r = request_device(root, a.device, a.package)
        if r["granted"]:
            util.log("ok", f"{a.package} 获得 {a.device}")
            return 0
        util.log("err", r["reason"])
        return 1

    if a.cmd == "release":
        if release_device(root, a.device, a.package):
            util.log("ok", f"{a.package} 已释放 {a.device}")
            return 0
        util.log("warn", f"{a.package} 并未占用 {a.device}")
        return 1

    if a.cmd == "declare":
        m = MediaManifest(a.package, list(a.uses),
                          list(a.background), list(a.optional))
        problems = check_manifest(root, m)
        p = write_manifest(root, m)
        util.log("ok", f"已写入 {p}")
        for x in problems:
            util.log("warn", x)
        return 0
    return 1
