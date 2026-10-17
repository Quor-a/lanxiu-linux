"""软件源管理：多源、优先级、镜像、签名信任、失效降级。

本地仓库（qyrepo）解决的是"怎么组织包"，软件源解决的是
"用户从哪里拿包"。两者不是一回事：

- 源可以有多个：官方源、镜像源、本地缓存源、第三方源
- 源有优先级：同名同版本包在不同源都有，取哪个？
- 源会失效：镜像挂了要能自动降级到下一个，不能就此装不了软件
- 源的信任级别不同：官方源可签名验证，第三方源可能没签名，
  允许未签名不等于等同信任——必须显式标记并让用户看见
- 源会漂移：镜像落后于官方，用户装到旧包却以为是最新的

几个必须做对的点：

**优先级不能用"配置顺序"隐含表达**。用户看不出"写在前面的优先"，
而且重排配置文件就会静默改变装到哪个包。必须是显式数字，
且相同优先级时给出确定的排序规则（否则结果不可复现）。

**失效降级要在取包时发生，不是在配置时**。配置时探测一次，
之后源挂了照样装不上。而且探测失败不该让整个操作失败。

**未签名的源必须显式声明 trusted=no 并要求用户确认**。
静默接受未签名包等于把供应链安全交给运气。
"""
from __future__ import annotations

import json
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class SourceError(RuntimeError):
    pass


# 源的信任级别
TRUSTED = "trusted"        # 有公钥，可验签
UNSIGNED = "unsigned"      # 无签名，需显式允许
LOCAL = "local"            # 本地目录，默认可信（文件就在机器上）


@dataclass
class Source:
    """一个软件源。"""
    name: str
    url: str                    # 本地源是目录路径，远程源是 URL
    priority: int = 50          # 数字小者优先
    arch: str = ""              # 留空表示跟随本机架构
    enabled: bool = True
    trust: str = TRUSTED
    pubkey: str = ""
    comment: str = ""
    # 运行时状态（不写进配置）
    last_ok: float = 0.0
    last_error: str = ""

    @property
    def is_local(self) -> bool:
        return not self.url.startswith(("http://", "https://", "ftp://"))

    def describe(self) -> str:
        marks = []
        if not self.enabled:
            marks.append("已停用")
        if self.trust == UNSIGNED:
            marks.append("未签名")
        if self.is_local:
            marks.append("本地")
        tag = f"（{'、'.join(marks)}）" if marks else ""
        return f"  [{self.priority:>3}] {self.name:<16} {self.url}{tag}"


def sources_path(root: Path) -> Path:
    return Path(root) / "etc" / "qypkg" / "sources.json"


def load_sources(root: Path) -> list:
    """加载源列表。文件不存在时返回内置默认源。"""
    p = sources_path(root)
    if not p.exists():
        return default_sources(root)
    try:
        data = json.loads(p.read_text())
    except Exception as e:
        raise SourceError(f"源配置无法解析: {p}: {e}")
    out = []
    for d in data.get("sources", []):
        out.append(Source(**{k: v for k, v in d.items()
                             if k in Source.__dataclass_fields__}))
    return out


def default_sources(root: Path) -> list:
    """内置默认源：本机架构的官方源 + 本地仓库。

    本地仓库排最高优先级是刻意的——自己构建出来的包应该优先于
    远程同名包，否则本地改了代码却装到远程旧包，
    表现为"我明明改了怎么没生效"。
    """
    return [
        Source(name="local", url=str(Path(root) / "var" / "repo"),
               priority=10, trust=LOCAL,
               comment="本机构建的仓库，优先于远程"),
        Source(name="official", url="https://repo.qiyuan-linux.org",
               priority=50, trust=TRUSTED,
               pubkey="/etc/qypkg/keys/official.pub",
               comment="官方源"),
    ]


def save_sources(root: Path, srcs: list) -> None:
    p = sources_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    data = {"sources": [
        {k: v for k, v in s.__dict__.items()
         if k in Source.__dataclass_fields__}
        for s in srcs]}
    util.atomic_write(p, json.dumps(data, ensure_ascii=False,
                                    indent=1).encode())


def active_sources(srcs: list) -> list:
    """启用中的源，按优先级排序。

    同优先级按名字排序，保证结果确定——
    否则两次解析可能拿到不同的源，装到的包也就不同了。
    """
    return sorted([s for s in srcs if s.enabled],
                  key=lambda s: (s.priority, s.name))


def add_source(root: Path, src: Source) -> list:
    srcs = load_sources(root)
    if any(s.name == src.name for s in srcs):
        raise SourceError(f"已存在同名源 {src.name}")
    srcs.append(src)
    save_sources(root, srcs)
    return srcs


def remove_source(root: Path, name: str) -> list:
    srcs = load_sources(root)
    left = [s for s in srcs if s.name != name]
    if len(left) == len(srcs):
        raise SourceError(f"没有名为 {name} 的源"
                          f"（可用：{' '.join(s.name for s in srcs) or '无'}）")
    save_sources(root, left)
    return left


def enable_source(root: Path, name: str, on: bool) -> list:
    srcs = load_sources(root)
    for s in srcs:
        if s.name == name:
            s.enabled = on
            break
    else:
        raise SourceError(f"没有名为 {name} 的源")
    save_sources(root, srcs)
    return srcs


# ---------------------------------------------------------------- 解析

@dataclass
class Resolution:
    """一次包解析的结果。"""
    name: str
    source: Source | None = None
    entry: dict | None = None
    tried: list = field(default_factory=list)

    @property
    def found(self) -> bool:
        return self.entry is not None


def fetch_index(src: Source, arch: str, timeout: int = 10) -> dict:
    """取一个源的索引。"""
    if src.is_local:
        p = Path(src.url) / arch / "index.json"
        if not p.exists():
            raise SourceError(f"本地源没有索引: {p}")
        return json.loads(p.read_text())
    url = f"{src.url.rstrip('/')}/{arch}/index.json"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read())
    except Exception as e:
        raise SourceError(f"{src.name}: 取索引失败: {e}")


def resolve(root: Path, names: list, arch: str = "",
            allow_unsigned: bool = False, timeout: int = 10) -> dict:
    """按优先级从多个源解析一批包。

    源失效时自动降级到下一个，而不是就此失败——
    镜像挂了就不让用户装软件是不能接受的。
    """
    arch = arch or util.ARCH
    srcs = active_sources(load_sources(root))
    out = {}
    for n in names:
        res = Resolution(name=n)
        for s in srcs:
            try:
                idx = fetch_index(s, arch, timeout)
            except SourceError as e:
                s.last_error = str(e)
                res.tried.append((s.name, f"不可用: {e}"))
                continue
            if s.trust == UNSIGNED and not allow_unsigned:
                res.tried.append((s.name, "未签名，需 --allow-unsigned"))
                continue
            hit = None
            for e in idx.get("packages", []):
                if e.get("name") == n:
                    hit = e
                    break
            if hit is None:
                res.tried.append((s.name, "没有这个包"))
                continue
            hit = dict(hit)
            hit["_source"] = s.name
            hit["_url"] = (s.url.rstrip("/")
                           + f"/{arch}/" + hit["filename"])
            res.source = s
            res.entry = hit
            s.last_ok = time.time()
            break
        out[n] = res
    return out


def resolution_report(results: dict) -> str:
    L = []
    for n, r in results.items():
        if r.found:
            src = r.entry.get("_source")
            L.append(f"  {n:<20} → 来自 {src} "
                     f"{r.entry.get('version')}-{r.entry.get('release')}")
        else:
            L.append(f"  {n:<20} → 没找到")
            for sname, why in r.tried:
                L.append(f"      {sname}: {why}")
    return "\n".join(L)


# ---------------------------------------------------------------- 校验

def check_sources(root: Path, arch: str = "", timeout: int = 8) -> list:
    """检查所有启用的源是否可达、索引是否有效。

    返回问题列表。源不可达本身不是错误（镜像会挂），
    但**所有源都不可达**是——那意味着这台机器装不了任何软件，
    必须明确说出来，而不是让用户一个个试。
    """
    arch = arch or util.ARCH
    srcs = active_sources(load_sources(root))
    problems = []
    reachable = 0
    for s in srcs:
        try:
            idx = fetch_index(s, arch, timeout)
        except SourceError as e:
            problems.append(f"{s.name}: {e}")
            continue
        reachable += 1
        if not idx.get("packages"):
            problems.append(f"{s.name}: 索引里没有包")
        if s.trust == TRUSTED and not Path(s.pubkey).exists() \
                and s.pubkey:
            problems.append(
                f"{s.name}: 声明为可信源但公钥不存在 {s.pubkey}")
    if not srcs:
        problems.append("没有启用任何软件源")
    elif reachable == 0:
        problems.append("所有源都不可达——这台机器无法安装或升级任何软件")
    return problems


def sources_report(root: Path) -> str:
    srcs = load_sources(root)
    if not srcs:
        return "没有配置任何软件源。"
    L = ["软件源（数字小者优先）："]
    for s in active_sources(srcs):
        L.append(s.describe())
        if s.comment:
            L.append(f"        {s.comment}")
    disabled = [s for s in srcs if not s.enabled]
    if disabled:
        L.append("\n已停用：")
        for s in disabled:
            L.append(f"  {s.name}  {s.url}")
    return "\n".join(L)


# ---------------------------------------------------------------- 镜像同步

def mirror_plan(src: Source, arch: str, local_repo: Path) -> dict:
    """算出一个源需要同步哪些包。

    增量同步而不是全量：全量每次都要下几百个包，
    镜像同步一次跑几小时，实际没人会维护。
    """
    local_idx = {}
    p = local_repo / arch / "index.json"
    if p.exists():
        try:
            local_idx = {e["name"]: e
                         for e in json.loads(p.read_text())["packages"]}
        except Exception:
            pass
    remote_idx = fetch_index(src, arch)
    need, update = [], []
    for e in remote_idx.get("packages", []):
        cur = local_idx.get(e["name"])
        if cur is None:
            need.append(e)
        elif cur.get("pkgid") != e.get("pkgid"):
            update.append((cur.get("pkgid"), e.get("pkgid")))
    return {
        "arch": arch, "source": src.name,
        "new": need, "update": update,
        "new_count": len(need), "update_count": len(update),
    }


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qysource",
                                 description="LANXIU Linux 软件源管理")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ap.add_argument("--root", default=".")

    sp = sub.add_parser("list", help="列出软件源")
    sp = sub.add_parser("check", help="检查源是否可用")
    sp.add_argument("--arch", default="")
    sp.add_argument("--timeout", type=int, default=8)
    sp = sub.add_parser("add", help="添加源")
    sp.add_argument("name")
    sp.add_argument("url")
    sp.add_argument("--priority", type=int, default=50)
    sp.add_argument("--trust", choices=[TRUSTED, UNSIGNED, LOCAL],
                    default=TRUSTED)
    sp.add_argument("--pubkey", default="")
    sp.add_argument("--comment", default="")
    sp = sub.add_parser("remove", help="删除源")
    sp.add_argument("name")
    sp = sub.add_parser("enable", help="启用源")
    sp.add_argument("name")
    sp = sub.add_parser("disable", help="停用源")
    sp.add_argument("name")
    sp = sub.add_parser("resolve", help="看某个包会从哪个源来")
    sp.add_argument("names", nargs="+")
    sp.add_argument("--arch", default="")
    sp.add_argument("--allow-unsigned", action="store_true")

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "list":
        print(sources_report(root))
        return 0

    if a.cmd == "check":
        problems = check_sources(root, a.arch, a.timeout)
        if problems:
            for x in problems:
                util.log("err", x)
            return 1
        util.log("ok", "所有启用的源都可用")
        return 0

    if a.cmd == "add":
        add_source(root, Source(
            name=a.name, url=a.url, priority=a.priority,
            trust=a.trust, pubkey=a.pubkey, comment=a.comment))
        util.log("ok", f"已添加源 {a.name}")
        return 0

    if a.cmd == "remove":
        remove_source(root, a.name)
        util.log("ok", f"已删除源 {a.name}")
        return 0

    for cmd, on in (("enable", True), ("disable", False)):
        if a.cmd == cmd:
            enable_source(root, a.name, on)
            util.log("ok", f"已{'启用' if on else '停用'} {a.name}")
            return 0

    if a.cmd == "resolve":
        res = resolve(root, a.names, a.arch,
                      allow_unsigned=a.allow_unsigned)
        print(resolution_report(res))
        return 0 if all(r.found for r in res.values()) else 1
    return 1
