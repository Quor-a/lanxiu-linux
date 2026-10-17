"""通知与窗口表现：通知、全屏通知、息屏显示、免打扰、后台弹窗、画中画、悬浮窗。

这一层最容易被做成"给什么都能过"，结果是用户被弹窗淹没、
或者反过来——重要来电被静默吞掉。所以每一项都有明确规则：

**通知**
- 静音不等于不显示。用户看完再消失，比弹出就消失更不容易错过
- 通知要有分组：一个应用刷屏 50 条会把其他应用全挤掉，
  必须折叠成"共 50 条"

**全屏通知（来电）**
- 只有来电、闹钟这类"必须立即处理"的才能用。
  普通应用申请全屏通知要单独授权，否则等于允许它随时劫持屏幕

**息屏显示（AOD）**
- 只在屏幕关闭时显示，内容受限（时间、通知图标）。
  让应用自定义 AOD 内容会导致烧屏和耗电

**免打扰**
- 必须有例外名单。全部屏蔽的话，紧急联系人也打不进来——
  这是免打扰最常被抱怨的地方

**后台弹出界面**
- 默认拒绝。它能打断用户当前操作、能伪造登录框，
  是钓鱼的主要手段

**画中画 / 悬浮窗**
- 画中画只允许视频类，且尺寸受限
- 悬浮窗能盖住其他应用，必须单独授权

**热更新**
- 不重启替换自身代码——绕过发行版审计。
  允许它等于放弃"系统里跑的是什么版本"这件事
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class NotifyError(RuntimeError):
    pass


# ---------------------------------------------------------------- 通知

# 紧急度。决定了能不能全屏、能不能穿透免打扰
URGENCY = {
    "low": "低",        # 普通通知，静默进入通知中心
    "normal": "普通",   # 显示但不打断
    "high": "高",       # 显示并提示音
    "critical": "紧急", # 可全屏、可穿透免打扰（来电、闹钟）
}

# 哪些类别可以穿透免打扰。列白名单而不是黑名单：
# 黑名单会被"我这个也很重要"慢慢撑开
DND_WHITELIST = {"incoming-call", "alarm", "emergency"}


@dataclass
class Notification:
    app: str
    title: str
    body: str = ""
    urgency: str = "normal"
    category: str = ""      # incoming-call / alarm / message …
    group: str = ""         # 分组键，同组的会折叠
    actions: list = field(default_factory=list)


def can_show(n: Notification, dnd: bool, dnd_allow: set | None = None) -> dict:
    """判断一条通知能不能显示。"""
    allow = dnd_allow if dnd_allow is not None else DND_WHITELIST
    if dnd:
        if n.category in allow or n.urgency == "critical":
            return {"show": True, "reason": "紧急类别可穿透免打扰"}
        return {"show": False,
                "reason": f"免打扰中，{n.category or '普通通知'}被静音"}
    return {"show": True, "reason": ""}


def can_fullscreen(n: Notification) -> dict:
    """能不能用全屏通知。

    全屏会劫持屏幕，只有必须立即处理的才行。
    普通应用拿到这个能力，等于允许它随时盖住用户的屏幕。
    """
    if n.urgency == "critical":
        return {"ok": True, "reason": ""}
    return {"ok": False,
            "reason": f"{n.title} 不是紧急通知，不能用全屏显示。"
                      f"全屏会劫持屏幕，仅限来电、闹钟这类"
                      f"必须立即处理的场景"}


def group_notifications(items: list) -> list:
    """折叠同组通知。

    一个应用刷屏 50 条会把其他应用全挤掉。
    折叠成"共 50 条"，用户的通知中心才可用。
    """
    out = []
    by_group: dict = {}
    for n in items:
        key = (n.app, n.group) if n.group else None
        if key is None:
            out.append(n)
            continue
        by_group.setdefault(key, []).append(n)
    for (app, g), ns in by_group.items():
        if len(ns) == 1:
            out.append(ns[0])
        else:
            out.append(Notification(
                app=app,
                title=f"{ns[0].title} 等 {len(ns)} 条",
                body=f"来自 {app}·{g}",
                urgency=max((x.urgency for x in ns),
                            key=lambda u: list(URGENCY).index(u)),
                category=ns[0].category,
                group=g,
            ))
    return out


# ---------------------------------------------------------------- 息屏显示

# AOD 只允许这些内容。让应用自定义会导致烧屏与耗电
AOD_ALLOWED = {"clock", "date", "notification-icons", "battery"}

# AOD 必须定期微调位置，否则固定内容长时间显示会烧屏
AOD_SHIFT_PIXELS = 3


def aod_check(content: list) -> list:
    """检查息屏显示的内容。返回不允许的项。"""
    return [c for c in content if c not in AOD_ALLOWED]


def aod_report() -> str:
    return ("息屏显示：\n"
            f"  允许的内容: {'、'.join(sorted(AOD_ALLOWED))}\n"
            f"  位置偏移: 每周期 {AOD_SHIFT_PIXELS} 像素"
            f"（不偏移会烧屏）\n"
            f"  自定义内容: 不允许——会导致烧屏与耗电")


# ---------------------------------------------------------------- 免打扰

@dataclass
class DndRule:
    enabled: bool = False
    allow: list = field(default_factory=lambda: sorted(DND_WHITELIST))
    # 例外联系人：全屏蔽的话紧急联系人也打不进来，
    # 这是免打扰最常被抱怨的地方
    allow_contacts: list = field(default_factory=list)
    schedule: str = ""      # 如 "22:00-07:00"


def dnd_report(r: DndRule) -> str:
    L = [f"免打扰: {'开启' if r.enabled else '关闭'}"]
    if r.schedule:
        L.append(f"  时段: {r.schedule}")
    L.append(f"  允许穿透: {'、'.join(r.allow)}")
    if r.allow_contacts:
        L.append(f"  例外联系人: {'、'.join(r.allow_contacts)}")
    else:
        L.append("  ! 没有设置例外联系人——"
                 "紧急联系人的来电也会被静音")
    return "\n".join(L)


# ---------------------------------------------------------------- 窗口

WINDOW_MODES = {
    "normal": "普通窗口",
    "fullscreen": "全屏",
    "pip": "画中画",
    "floating": "悬浮窗",
    "aod": "息屏显示",
}

# 画中画只允许这些类别。任何应用都能画中画的话，
# 屏幕上会飘满小窗口
PIP_ALLOWED_CATEGORIES = {"video", "call", "navigation", "timer"}

# 悬浮窗尺寸下限：太小的悬浮窗用户点不到，
# 且容易做成"看不见但一直在"的东西
FLOATING_MIN_PX = 80


def pip_check(category: str) -> dict:
    if category in PIP_ALLOWED_CATEGORIES:
        return {"ok": True, "reason": ""}
    return {"ok": False,
            "reason": f"{category} 不能使用画中画。"
                      f"仅允许 {'、'.join(sorted(PIP_ALLOWED_CATEGORIES))}"
                      f"——否则屏幕上会飘满小窗口"}


def floating_check(size_px: int) -> dict:
    if size_px >= FLOATING_MIN_PX:
        return {"ok": True, "reason": ""}
    return {"ok": False,
            "reason": f"悬浮窗 {size_px}px 小于下限 {FLOATING_MIN_PX}px——"
                      f"太小的窗口点不到，且容易做成"
                      f"看不见却一直在的东西"}


# ---------------------------------------------------------------- 后台弹窗

def background_popup_check(granted: bool, category: str = "") -> dict:
    """后台弹出界面。默认拒绝。"""
    if granted:
        return {"ok": True, "reason": ""}
    return {"ok": False,
            "reason": "后台弹出界面默认拒绝——"
                      "它能打断当前操作、能伪造登录框，"
                      "是钓鱼的主要手段。"
                      "确实需要的话在设置里单独授权"}


# ---------------------------------------------------------------- 热更新

def hot_update_check(signed: bool, audited: bool) -> dict:
    """热更新：不重启替换自身代码。

    允许它等于放弃"系统里跑的是什么版本"这件事——
    出事后无法复现现场。
    """
    if not signed:
        return {"ok": False,
                "reason": "热更新包未签名——无法确认来源，拒绝加载"}
    if not audited:
        return {"ok": False,
                "reason": "热更新未经发行版审计：系统里跑的将不是"
                          "你构建的那个版本，出事后无法复现现场"}
    return {"ok": True, "reason": ""}


# ---------------------------------------------------------------- 汇总

def capability_matrix() -> str:
    L = ["通知与窗口表现：", ""]
    L.append(f"  {'能力':<16}{'默认':<8}说明")
    rows = [
        ("notification", "允许", "普通通知"),
        ("fullscreen-notify", "拒绝", "全屏通知（仅紧急）"),
        ("aod", "允许", "息屏显示（内容受限）"),
        ("pop-background", "拒绝", "后台弹出界面"),
        ("pip", "询问", "画中画（限视频/通话/导航）"),
        ("overlay", "询问", "悬浮窗（有尺寸下限）"),
        ("hot-update", "拒绝", "热更新（需签名且经审计）"),
    ]
    for n, d, desc in rows:
        L.append(f"  {n:<18}{d:<8}{desc}")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qynotify",
                                 description="LANXIU Linux 通知与窗口")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("matrix", help="能力矩阵")
    sp = sub.add_parser("dnd", help="免打扰状态")
    sp.add_argument("--on", action="store_true")
    sp.add_argument("--contacts", nargs="*", default=[])
    sub.add_parser("aod", help="息屏显示规则")
    sp = sub.add_parser("pip", help="画中画检查")
    sp.add_argument("category")
    sp = sub.add_parser("floating", help="悬浮窗检查")
    sp.add_argument("size", type=int)
    sp = sub.add_parser("fullscreen", help="全屏通知检查")
    sp.add_argument("urgency", choices=list(URGENCY))
    sp = sub.add_parser("popup", help="后台弹窗检查")
    sp.add_argument("--granted", action="store_true")
    sp = sub.add_parser("hot-update", help="热更新检查")
    sp.add_argument("--signed", action="store_true")
    sp.add_argument("--audited", action="store_true")
    sp = sub.add_parser("group", help="通知分组演示")
    sp.add_argument("count", type=int)

    a = ap.parse_args(argv)

    if a.cmd == "matrix":
        print(capability_matrix())
        return 0

    if a.cmd == "dnd":
        r = DndRule(enabled=a.on, allow_contacts=list(a.contacts))
        print(dnd_report(r))
        return 0

    if a.cmd == "aod":
        print(aod_report())
        return 0

    if a.cmd == "pip":
        r = pip_check(a.category)
        print(r["reason"] or "允许使用画中画")
        return 0 if r["ok"] else 1

    if a.cmd == "floating":
        r = floating_check(a.size)
        print(r["reason"] or "尺寸符合要求")
        return 0 if r["ok"] else 1

    if a.cmd == "fullscreen":
        n = Notification("app", "测试", urgency=a.urgency)
        r = can_fullscreen(n)
        print(r["reason"] or "可以使用全屏通知")
        return 0 if r["ok"] else 1

    if a.cmd == "popup":
        r = background_popup_check(a.granted)
        print(r["reason"] or "已授权")
        return 0 if r["ok"] else 1

    if a.cmd == "hot-update":
        r = hot_update_check(a.signed, a.audited)
        print(r["reason"] or "允许热更新")
        return 0 if r["ok"] else 1

    if a.cmd == "group":
        ns = [Notification("chat", f"消息 {i}", group="chat")
              for i in range(a.count)]
        out = group_notifications(ns)
        print(f"{a.count} 条同组通知 → 折叠为 {len(out)} 条：")
        for n in out:
            print(f"  {n.title}")
        return 0
    return 1
