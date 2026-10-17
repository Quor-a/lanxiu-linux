"""补丁管理。

发行版必然要打补丁——上游有安全问题时不能等它发版，构建在某些
环境上失败也得自己修。没有补丁机制，发行版就只是"转发上游"。

补丁按来源分三类，处理方式和风险完全不同：

1. **上游补丁**（backport）：从上游仓库 cherry-pick 某个已修复的提交。
   风险最低，且上游发新版后可以整批删掉。
   必须记录 commit id——不记的话三个月后没人知道这补丁从哪来、
   上游哪个版本已经包含了它、能不能删。

2. **发行版补丁**：自己写的，修构建失败、改默认路径、适配本发行版。
   这类补丁不会消失，需要长期维护，每次改上游版本都要检查是否还能打上。

3. **安全补丁**：修 CVE。必须记 CVE 编号，
   这样安全扫描能自动关联"这个包打了哪些 CVE 的补丁"。

几个必须做对的点：

- **补丁要有校验和**。跟源码一样，本地补丁也需要锁定。
  补丁文件被改了会静默改变产物——而这正是供应链攻击的入口。
- **打不上必须报错，不能静默跳过**。补丁打不上通常意味着上游改了代码，
  补丁已经失效或需要 rebase；静默跳过的结果是"以为修了其实没修"。
- **fuzz 要递减**。第一个补丁改了行号，后面的补丁就偏移了。
  从 -p1 一路试到 -p0 是常见做法，但要先试最严格的。
- **打了补丁要在元数据里留痕**。用户拿到包应该能查到它打了哪些补丁，
  否则无从判断这个包和上游有什么区别。
"""
from __future__ import annotations

import hashlib
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class PatchError(RuntimeError):
    pass


# 补丁类型
UPSTREAM = "upstream"      # 从上游 backport
DISTRO = "distro"          # 本发行版自己写的
SECURITY = "security"      # 修 CVE


@dataclass
class Patch:
    """一个补丁。"""
    file: str                       # 相对 patches/ 目录的路径
    sha256: str = ""                # 留空则构建时拒绝
    kind: str = DISTRO
    reason: str = ""                # 为什么打这个补丁
    cve: str = ""                   # 安全补丁填 CVE 编号
    upstream_commit: str = ""       # 上游补丁填 commit id
    fixed_in: str = ""              # 上游从哪个版本起已包含此修复
    strip: int | None = None        # None 表示自动探测
    series: int = 0                 # 应用顺序，默认按声明顺序

    def describe(self) -> str:
        tag = {"upstream": "上游", "distro": "发行版",
               "security": "安全"}.get(self.kind, self.kind)
        L = [f"[{tag}] {self.file}"]
        if self.cve:
            L.append(f"  {self.cve}")
        if self.upstream_commit:
            L.append(f"  上游提交 {self.upstream_commit[:12]}"
                     + (f"，{self.fixed_in} 起已包含" if self.fixed_in else ""))
        L.append(f"  {self.reason}")
        return "\n".join(L)


def patches_dir(root: Path) -> Path:
    return Path(root) / "patches"


def load_patches(root: Path, recipes_path: Path) -> dict:
    """加载 recipes/patches.toml（或 .py）里声明的补丁清单。

    用单独一个清单文件而不是写在配方里，是为了能一眼看到
    "全系统一共打了多少补丁、哪些 CVE 已经修了"——
    散在 177 个配方里就没人看得见全貌了。
    """
    root = Path(root)
    for name in ("patches.toml", "patches.py", "patches.json"):
        p = recipes_path / name
        if not p.exists():
            continue
        if name.endswith(".toml"):
            try:
                import tomllib
                data = tomllib.loads(p.read_text())
            except ImportError:
                data = _parse_minimal_toml(p.read_text())
        elif name.endswith(".json"):
            import json
            data = json.loads(p.read_text())
        else:
            import importlib.util
            spec = importlib.util.spec_from_file_location("qypatches", p)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            data = getattr(m, "PATCHES", {})
        return _build_map(data)
    return {}


def _parse_minimal_toml(text: str) -> dict:
    """极简 TOML 解析，只支持本文件需要的结构。

    不引第三方库：解析器本身也是供应链的一部分，
    为一个 [[package.patch]] 的清单引入依赖不值得。
    """
    out: dict = {}
    cur_pkg = None
    cur: dict | None = None
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # [[包名]] 开一个新包的补丁列表，并立即开第一个补丁条目。
        # 写成 [[包名]] 而不是 [[package]] 是刻意的——清单要能一眼看出
        # 哪个包打了补丁，多一层嵌套只是增加阅读负担。
        m = re.match(r'^\[\[([^\].]+)\]\]$', line)
        if m:
            cur_pkg = m.group(1).strip()
            out.setdefault(cur_pkg, [])
            cur = {}
            out[cur_pkg].append(cur)
            continue
        # 连续多个补丁：以 file= 作为新条目的起点
        m = re.match(r'^(\w+)\s*=\s*(.*)$', line)
        if m and cur is not None:
            k, v = m.group(1), m.group(2).strip()
            if k == "file" and cur.get("file"):
                cur = {}
                out[cur_pkg].append(cur)
            if v.startswith('"') and v.endswith('"'):
                cur[k] = v[1:-1]
            elif v.lstrip("-").isdigit():
                cur[k] = int(v)
            else:
                cur[k] = v
            continue
    return {k: [d for d in v if d.get("file")] for k, v in out.items()}


def _build_map(data) -> dict:
    out = {}
    for pkg, items in (data or {}).items():
        lst = []
        for i, d in enumerate(items):
            if isinstance(d, Patch):
                lst.append(d)
                continue
            lst.append(Patch(
                file=d.get("file", ""),
                sha256=d.get("sha256", ""),
                kind=d.get("kind", DISTRO),
                reason=d.get("reason", ""),
                cve=d.get("cve", ""),
                upstream_commit=d.get("upstream_commit", ""),
                fixed_in=d.get("fixed_in", ""),
                strip=d.get("strip"),
                series=d.get("series", i)))
        out[pkg] = sorted(lst, key=lambda p: p.series)
    return out


def verify_patch(root: Path, p: Patch) -> list:
    """校验一个补丁。返回问题列表。"""
    problems = []
    path = Path(p.file)
    if not path.is_absolute():
        path = patches_dir(root) / p.file
    if not path.exists():
        problems.append(f"补丁文件不存在: {path}")
        return problems
    actual = util.sha256_file(path)
    if not p.sha256:
        problems.append(f"{p.file}: 未锁定 sha256——"
                        f"补丁被改会静默改变产物，这正是供应链攻击的入口")
    elif p.sha256 != actual:
        problems.append(f"{p.file}: 校验和不符（声明 {p.sha256[:12]}…"
                        f" 实际 {actual[:12]}…）")
    if p.kind == SECURITY and not p.cve:
        problems.append(f"{p.file}: 安全补丁必须填 CVE 编号，"
                        f"否则安全扫描无法关联")
    if p.kind == UPSTREAM and not p.upstream_commit:
        problems.append(f"{p.file}: 上游补丁必须填 commit id，"
                        f"否则三个月后没人知道它从哪来、能不能删")
    if not p.reason:
        problems.append(f"{p.file}: 没写为什么打这个补丁")
    return problems


def apply_patches(root: Path, name: str, srcdir: Path,
                  patches: list, dry_run: bool = False) -> list:
    """给源码打补丁。返回已应用的列表。

    打不上就抛错，绝不静默跳过——补丁打不上通常意味着上游改了代码，
    补丁已失效或需要 rebase；静默跳过的结果是"以为修了其实没修"。
    """
    applied = []
    for p in patches:
        path = Path(p.file)
        if not path.is_absolute():
            path = patches_dir(root) / p.file
        if not path.exists():
            raise PatchError(f"{name}: 补丁文件不存在 {path}")
        actual = util.sha256_file(path)
        if p.sha256 and p.sha256 != actual:
            raise PatchError(
                f"{name}: 补丁 {p.file} 校验和不符"
                f"（声明 {p.sha256[:12]}… 实际 {actual[:12]}…）\n"
                f"  补丁被改会静默改变产物，这是供应链攻击的入口")
        if not p.sha256:
            util.log("warn", f"{name}: 补丁 {p.file} 未锁定 sha256")
        ok = _apply_one(path, srcdir, p)
        if not ok:
            raise PatchError(
                f"{name}: 补丁 {p.file} 打不上。\n"
                f"  多半是上游改了代码，补丁已失效或需要 rebase。\n"
                f"  检查：cd {srcdir} && patch -p1 --dry-run < {path}")
        applied.append(p)
        util.log("ok", f"{name}: 已应用补丁 {p.file}")
    return applied


def _apply_one(patch_path: Path, srcdir: Path, p: Patch) -> bool:
    """打单个补丁。strips 从声明值或 1→0 递减尝试。"""
    strips = [p.strip] if p.strip is not None else [1, 0, 2]
    for s in strips:
        # 先 dry-run，失败不留下半成品
        r = subprocess.run(
            ["patch", f"-p{s}", "--dry-run", "--forward",
             "-i", str(patch_path)],
            cwd=srcdir, capture_output=True, text=True)
        if r.returncode != 0:
            continue
        r2 = subprocess.run(
            ["patch", f"-p{s}", "--forward", "-i", str(patch_path)],
            cwd=srcdir, capture_output=True, text=True)
        if r2.returncode == 0:
            return True
    return False


def report(patches: dict) -> str:
    """全系统补丁一览。"""
    if not patches:
        return "当前没有打任何补丁。"
    total = sum(len(v) for v in patches.values())
    by_kind = {}
    cves = []
    for pkg, lst in patches.items():
        for p in lst:
            by_kind.setdefault(p.kind, []).append((pkg, p))
            if p.cve:
                cves.append((pkg, p.cve))
    L = [f"全系统共 {total} 个补丁，影响 {len(patches)} 个包："]
    titles = {UPSTREAM: "上游 backport", DISTRO: "发行版自有",
              SECURITY: "安全修复"}
    for kind, items in sorted(by_kind.items()):
        L.append(f"\n  【{titles.get(kind, kind)}】{len(items)} 个")
        for pkg, p in items[:6]:
            L.append(f"    {pkg}: {p.file}"
                     + (f"（{p.cve}）" if p.cve else ""))
        if len(items) > 6:
            L.append(f"    … 另有 {len(items) - 6} 个")
    if cves:
        L.append(f"\n  已修 CVE（{len(cves)} 个）：")
        for pkg, cve in cves[:10]:
            L.append(f"    {cve} → {pkg}")
    return "\n".join(L)


def stale_patches(patches: dict, recipes: dict) -> list:
    """找出可能已失效的补丁：补丁声明了 fixed_in，
    而当前配方版本已经达到或超过它。

    这类补丁该删了。留着不仅多余，还会让"我们改了上游什么"
    这份清单失真——清单失真后，没人敢信它。
    """
    from .deps import version_cmp
    out = []
    for pkg, lst in patches.items():
        r = recipes.get(pkg)
        if not r:
            continue
        for p in lst:
            if p.fixed_in and version_cmp(r.version, ">=", p.fixed_in):
                out.append((pkg, p, r.version))
    return out


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qypatch",
                                 description="LANXIU Linux 补丁管理")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("list", help="列出全系统补丁")
    sp.add_argument("--package", default=None)

    sp = sub.add_parser("verify", help="校验补丁文件与声明是否一致")

    sp = sub.add_parser("stale", help="找出上游已包含、可以删掉的补丁")

    sp = sub.add_parser("checksum", help="算出补丁文件的 sha256")
    sp.add_argument("file")

    a = ap.parse_args(argv)
    root = Path.cwd()

    if a.cmd == "checksum":
        p = Path(a.file)
        if not p.exists():
            util.log("err", f"文件不存在: {p}")
            return 1
        print(util.sha256_file(p))
        return 0

    from . import recipe as RM
    recipes = RM.load_tree(root / "recipes")
    patches = load_patches(root, root / "recipes")

    if a.cmd == "list":
        if a.package:
            lst = patches.get(a.package, [])
            if not lst:
                print(f"{a.package} 没有补丁")
                return 0
            for p in lst:
                print(p.describe())
                print()
        else:
            print(report(patches))
        return 0

    if a.cmd == "verify":
        problems = []
        for pkg, lst in patches.items():
            for p in lst:
                for x in verify_patch(root, p):
                    problems.append(f"{pkg}: {x}")
        if problems:
            for x in problems:
                util.log("err", x)
            util.log("err", f"{len(problems)} 处问题")
            return 1
        util.log("ok", f"{sum(len(v) for v in patches.values())} 个补丁"
                       f"全部校验通过")
        return 0

    if a.cmd == "stale":
        stale = stale_patches(patches, recipes)
        if not stale:
            print("没有已失效的补丁。")
            return 0
        print(f"{len(stale)} 个补丁可能已失效（上游已包含该修复）：")
        for pkg, p, ver in stale:
            print(f"  {pkg} {ver} ≥ {p.fixed_in}  可删: {p.file}")
        return 0
    return 1
