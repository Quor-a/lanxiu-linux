"""locale（语言环境）与键盘布局。

Ubuntu 装机第一步就是选语言、生成 locale。
我们没有 locale-gen、没有 /etc/locale.conf，
中文用户拿到系统直接乱码——文件名变问号、终端显示方块。

这个模块管三件事：
1. 生成 locale（locale-gen / localedef 的替代流程）
2. 写 /etc/locale.conf（全系统默认 LANG）
3. 装机时把用户选的语言落到系统里

几个必须做对的点：

**LANG 不能默认 C**
C locale 意味着"按字节处理"，非 ASCII 全部按单字节切。
结果是中文文件名显示成问号、sort 排序错乱、
grep 匹配不到——而且不报错，用户只会觉得"这系统对中文支持不好"。

**locale 必须先生成才能用**
设了 LANG=zh_CN.UTF-8 但没生成对应 locale，
程序会回退到 C，表现为"我明明设了中文却还是乱码"。
所以生成和设置必须在同一步完成，不能分开让用户自己做。

**UTF-8 是唯一合理选择**
GBK、BIG5 这类编码在多语言混排时会丢字符，
而现代软件生态基本只测 UTF-8。

**键盘布局要单独管**
语言是中文也可能用美式键盘。两者不该绑死——
把中文和某个中文输入法布局强制绑定，会让外接键盘的用户
发现按键全不对。
"""
from __future__ import annotations

import json
from pathlib import Path

from . import util

# 装机时可选的语言。每种都给 UTF-8——
# 非 UTF-8 编码在多语言混排时会丢字符
LOCALES = {
    "zh_CN": {"desc": "简体中文", "locale": "zh_CN.UTF-8",
              "keymaps": ["us", "cn"], "default_keymap": "us"},
    "zh_TW": {"desc": "繁體中文（台灣）", "locale": "zh_TW.UTF-8",
              "keymaps": ["us", "tw"], "default_keymap": "us"},
    "en_US": {"desc": "English (US)", "locale": "en_US.UTF-8",
              "keymaps": ["us", "uk", "dvorak"], "default_keymap": "us"},
    "en_GB": {"desc": "English (UK)", "locale": "en_GB.UTF-8",
              "keymaps": ["uk", "us"], "default_keymap": "uk"},
    "ja_JP": {"desc": "日本語", "locale": "ja_JP.UTF-8",
              "keymaps": ["jp", "us"], "default_keymap": "jp"},
    "ko_KR": {"desc": "한국어", "locale": "ko_KR.UTF-8",
              "keymaps": ["kr", "us"], "default_keymap": "kr"},
    "de_DE": {"desc": "Deutsch", "locale": "de_DE.UTF-8",
              "keymaps": ["de", "us"], "default_keymap": "de"},
    "fr_FR": {"desc": "Français", "locale": "fr_FR.UTF-8",
              "keymaps": ["fr", "us"], "default_keymap": "fr"},
}

# 兜底：C.UTF-8 在任何 glibc 上都存在，
# 比 C 好——它至少认得多字节字符
FALLBACK = "C.UTF-8"


class LocaleError(RuntimeError):
    pass


def locale_conf_path(root: Path) -> Path:
    return Path(root) / "etc" / "locale.conf"


def locales_path(root: Path) -> Path:
    return Path(root) / "etc" / "locale.gen"


def available(root: Path | None = None) -> dict:
    """返回可选语言。root 给定时只返回该系统已生成的。"""
    if root is None:
        return LOCALES
    generated = generated_locales(root)
    return {k: v for k, v in LOCALES.items()
            if v["locale"] in generated or not generated}


def generated_locales(root: Path) -> list:
    """读取系统已生成的 locale 列表。"""
    p = Path(root) / "usr" / "lib" / "locale" / "locale-archive"
    # locale-archive 是二进制，读不出内容就退到 locale.gen 的声明
    gen = locales_path(root)
    if gen.exists():
        out = []
        for line in gen.read_text().splitlines():
            s = line.strip()
            if s and not s.startswith("#"):
                out.append(s.split()[0])
        return out
    return [] if not p.exists() else []


def gen_locales(root: Path, wanted: list) -> str:
    """生成 /etc/locale.gen。

    locale.gen 里没启用的 locale 不会被生成。
    设了 LANG 却没生成，程序会静默回退到 C——
    表现为"我明明设了中文却还是乱码"，极难排查。
    """
    p = locales_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    L = ["# 由LANXIU Linux 生成",
         "# 这里没启用的 locale 不会被生成；",
         "# 设了 LANG 却没生成会静默回退到 C（表现为乱码但不报错）", ""]
    for w in wanted:
        L.append(f"{w} UTF-8")
    content = "\n".join(L) + "\n"
    util.atomic_write(p, content.encode())
    return content


def set_locale(root: Path, lang: str, keymap: str = "") -> dict:
    """设置系统语言与键盘布局。

    生成与设置必须一步完成，不能让用户自己分两步——
    分开做的结果就是"设了但没生成"这个最难排查的问题。
    """
    # 找到对应的语言项
    item = None
    for k, v in LOCALES.items():
        if v["locale"] == lang or k == lang:
            item = v
            lang = v["locale"]
            break
    if item is None:
        raise LocaleError(
            f"不支持的语言 {lang}"
            f"（可选：{'、'.join(sorted(LOCALES))}）")

    km = keymap or item["default_keymap"]
    if km not in item["keymaps"]:
        raise LocaleError(
            f"{lang} 下没有键盘布局 {km}"
            f"（可选：{'、'.join(item['keymaps'])}）")

    # 先声明要生成，再写配置——顺序反了就会出现
    # "配了 LANG 但 locale 不存在"
    gen_locales(root, [lang, FALLBACK])
    p = locale_conf_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, f"LANG={lang}\n".encode())

    # 键盘布局单独写，不和语言绑死
    kbd = Path(root) / "etc" / "vconsole.conf"
    util.atomic_write(kbd, f"KEYMAP={km}\n".encode())

    return {"lang": lang, "keymap": km, "desc": item["desc"]}


def current(root: Path) -> dict:
    """读取当前设置。"""
    p = locale_conf_path(root)
    out = {"lang": "", "keymap": ""}
    if p.exists():
        for line in p.read_text().splitlines():
            if line.startswith("LANG="):
                out["lang"] = line.split("=", 1)[1].strip().strip('"')
    kbd = Path(root) / "etc" / "vconsole.conf"
    if kbd.exists():
        for line in kbd.read_text().splitlines():
            if line.startswith("KEYMAP="):
                out["keymap"] = line.split("=", 1)[1].strip()
    return out


def check_locale(root: Path) -> list:
    """检查语言环境配置。返回问题列表。"""
    problems = []
    cur = current(root)
    if not cur["lang"]:
        problems.append(
            "没有设置 LANG——系统按 C locale 运行，"
            "非 ASCII 文件名会显示成问号、排序错乱，且不报错"
            "（执行 qylocale set zh_CN 修正）")
        return problems
    if cur["lang"] in ("C", "POSIX"):
        problems.append(
            f"LANG={cur['lang']}：按字节处理，中文会乱码。"
            f"至少用 {FALLBACK}")
    gen = generated_locales(root)
    if gen and cur["lang"] not in gen:
        # 这是最难排查的一类：配了但没生成，静默回退
        problems.append(
            f"LANG={cur['lang']} 但 locale.gen 里没生成它——"
            f"程序会静默回退到 C，表现为设了中文却还是乱码")
    if not cur["keymap"]:
        problems.append("没有设置键盘布局（KEYMAP）")
    return problems


def locale_report(root: Path) -> str:
    cur = current(root)
    L = [f"当前语言: {cur['lang'] or '未设置'}"]
    L.append(f"键盘布局: {cur['keymap'] or '未设置'}")
    L.append("")
    L.append("可选语言：")
    for k, v in sorted(LOCALES.items()):
        mark = "*" if v["locale"] == cur["lang"] else " "
        L.append(f"  {mark} {k:<8} {v['locale']:<14} {v['desc']}")
    problems = check_locale(root)
    if problems:
        L.append("")
        L.append("问题：")
        for x in problems:
            L.append(f"  ! {x}")
    return "\n".join(L)


def install_prompt(default: str = "zh_CN") -> dict:
    """装机时询问语言。返回用户选择。

    默认简体中文：面向中文用户，且不给默认的话
    大部分人直接回车就拿到 C locale，然后一路乱码。
    """
    print("选择系统语言：")
    keys = sorted(LOCALES)
    for i, k in enumerate(keys, 1):
        v = LOCALES[k]
        mark = "（默认）" if k == default else ""
        print(f"  {i}. {v['desc']} [{v['locale']}]{mark}")
    print("  q. 手动输入 locale（例如 en_US.UTF-8）")
    try:
        s = input(f"选择 [1-{len(keys)}/q，回车用默认] ").strip()
    except (EOFError, KeyboardInterrupt):
        s = ""
    if not s:
        return dict(LOCALES[default])
    if s.lower() == "q":
        try:
            manual = input("请输入 locale: ").strip()
        except (EOFError, KeyboardInterrupt):
            manual = ""
        if manual:
            return {"locale": manual, "default_keymap": "us",
                    "keymaps": ["us"], "desc": manual}
        return dict(LOCALES[default])
    if s.isdigit() and 1 <= int(s) <= len(keys):
        return dict(LOCALES[keys[int(s) - 1]])
    # 输入既不是编号也不是 q：直接当 locale 名用，
    # 不报错重来——装机流程中断在语言选择上很挫败
    if "." in s or "_" in s:
        return {"locale": s, "default_keymap": "us",
                "keymaps": ["us"], "desc": s}
    return dict(LOCALES[default])


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qylocale",
                                 description="LANXIU Linux 语言与键盘布局")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出可选语言")
    sub.add_parser("show", help="查看当前设置")
    sub.add_parser("check", help="检查配置问题")
    sp = sub.add_parser("set", help="设置语言")
    sp.add_argument("lang")
    sp.add_argument("--keymap", default="")

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "list":
        print(locale_report(root))
        return 0
    if a.cmd == "show":
        c = current(root)
        print(f"LANG={c['lang'] or '未设置'}")
        print(f"KEYMAP={c['keymap'] or '未设置'}")
        return 0
    if a.cmd == "check":
        problems = check_locale(root)
        for x in problems:
            util.log("err", x)
        if not problems:
            util.log("ok", "语言环境配置正常")
        return 1 if problems else 0
    if a.cmd == "set":
        try:
            r = set_locale(root, a.lang, a.keymap)
        except LocaleError as e:
            util.log("err", str(e))
            return 1
        util.log("ok", f"已设置 {r['lang']}（键盘 {r['keymap']}）")
        util.log("info", "重新登录或重启后生效")
        return 0
    return 1
