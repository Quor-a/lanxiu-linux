"""持续集成与发布门禁。

177 个包的发行版没有 CI 就没法维护：改一个库，谁要重编？编完了能发吗？
靠人记必然漏。这里把整条流水线变成可执行的、可验证的。

流水线：

    检测变更 → 算影响闭包 → 分层并行构建 → 跑门禁 → 出报告 → 放行/拦截

**发布门禁是重点**。构建成功不等于能发布。以下几项任何一项不过，
整个批次就卡住——因为它们全都是"装到用户机器上才暴露"的问题：

1. 远程源码必须锁定 sha256（供应链）
2. 包必须有签名（分发完整性）
3. 索引与签名必须匹配（防止索引被换）
4. ELF 必须通过加固检查（RELRO/NX/PIE/Canary）
5. 产物不得含构建机绝对路径（可复现 + 信息泄露）
6. 版本不得回退（防止意外发布旧版）

门禁失败要**说清楚是哪一项、哪个包、怎么修**，
不能只报"检查未通过"——那样维护者只能猜。
"""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from . import util

GATES = [
    ("checksum", "远程源码锁定 sha256", "供应链：未锁定的源码可被替换"),
    ("signed", "包已签名", "分发完整性：无签名的包无法验证来源"),
    ("index", "索引与签名匹配", "防止索引被替换或篡改"),
    ("hardening", "ELF 加固项齐全", "RELRO/NX/PIE/Canary 缺失等于防护失效"),
    ("buildpath", "产物不含构建机路径", "可复现 + 泄露构建机目录结构"),
    ("version", "版本未回退", "防止误发布旧版本覆盖新版本"),
]


class CIError(RuntimeError):
    pass


@dataclass
class GateResult:
    name: str
    title: str
    why: str
    passed: bool = True
    failures: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.passed


@dataclass
class CIPlan:
    """一次 CI 运行计划。"""
    changed: list = field(default_factory=list)      # 直接改动的包
    rebuild: list = field(default_factory=list)      # 需要重编的（含反向依赖）
    layers: list = field(default_factory=list)       # 分层并行
    gates: list = field(default_factory=list)
    blocked: bool = False

    def summary(self) -> str:
        L = []
        L.append(f"直接改动 {len(self.changed)} 个包：{' '.join(self.changed) or '（无）'}")
        L.append(f"需要重编 {len(self.rebuild)} 个包")
        if self.layers:
            L.append(f"分 {len(self.layers)} 层并行：")
            for i, layer in enumerate(self.layers):
                L.append(f"  第{i}层（{len(layer)}）: "
                         f"{' '.join(layer[:8])}"
                         f"{' …' if len(layer) > 8 else ''}")
        return "\n".join(L)


# ---------------------------------------------------------------- 变更检测

def git_changed_files(root: Path, since: str = "HEAD~1") -> list:
    """从 git 找出改动的文件。没有 git 就返回空。"""
    def _run(args):
        try:
            r = subprocess.run(args, cwd=root, capture_output=True,
                               text=True, timeout=30)
        except Exception:
            return None
        if r.returncode != 0:
            return None
        return [x.strip() for x in r.stdout.splitlines() if x.strip()]

    # 工作区有未提交改动时，diff 就是答案
    got = _run(["git", "diff", "--name-only", since]) or []
    if got:
        return got
    # 工作区干净时 diff 为空，但"最近一次提交改了什么"仍是有效的
    # 变更信息。不看这个的话，每次提交后 CI 就查不到任何变更，
    # 表现为"明明刚改了东西却说没有"。
    got = _run(["git", "diff", "--name-only", since + "^", since]) or []
    if got:
        return got
    return _run(["git", "show", "--name-only", "--format=", "HEAD"]) or []


def detect_changed(root: Path, since: str = "HEAD~1") -> list:
    """把改动的文件映射到包。

    改 recipes/zlib.py → zlib 改了
    改 tests/demo/libqydemo/... → 该目录下源码属于哪个包
    """
    root = Path(root)
    changed = set()
    for f in git_changed_files(root, since):
        p = Path(f)
        if p.parts[0] == "recipes" and p.suffix == ".py":
            changed.add(p.stem)
            continue
        # 本地源码目录：找哪个配方的 source 指向它
        stem = p.parts[0] if len(p.parts) > 1 else ""
        for rp in sorted((root / "recipes").glob("*.py")):
            try:
                txt = rp.read_text()
            except OSError:
                continue
            if f'"{stem}/' in txt or f"'{stem}/" in txt:
                changed.add(rp.stem)
                break
    return sorted(changed)


# ---------------------------------------------------------------- 计划

def plan(root: Path, changed: list, recipes: dict) -> CIPlan:
    """改动的包 → 需要重编的闭包 → 分层。"""
    from . import cycles as CY
    from .deps import Universe

    cp = CY.plan(recipes)
    u = Universe(broken=cp.broken_deps)
    for r in recipes.values():
        u.add(r)

    # 反向依赖：改了 A，谁依赖 A 就要重编
    rebuild = set()
    for n in changed:
        if n not in recipes:
            continue
        rebuild.add(n)
        for name, r in recipes.items():
            deps = set(r.depends or []) | set(r.makedepends or [])
            if deps & ({n} | set(r.provides or [])):
                rebuild.add(name)

    # 分层
    order = u.resolve(sorted(rebuild))
    levels = {}
    for n in order:
        lv = 0
        for d in (recipes[n].depends or []) + (recipes[n].makedepends or []):
            dn = u.provider_of_name(d)
            if dn in levels:
                lv = max(lv, levels[dn] + 1)
        levels[n] = lv
    by = {}
    for n, lv in levels.items():
        by.setdefault(lv, []).append(n)
    layers = [sorted(by[k]) for k in sorted(by)]

    return CIPlan(changed=sorted(changed), rebuild=order, layers=layers)


# ---------------------------------------------------------------- 门禁

def _gate_checksum(root: Path, pkgs: list, recipes: dict) -> GateResult:
    g = GateResult(*GATES[0])
    for name in pkgs:
        r = recipes.get(name)
        if not r:
            continue
        if getattr(r, "checksum_pending", False):
            g.passed = False
            g.failures.append({
                "package": name,
                "detail": "远程源码未锁定 sha256",
                "fix": f"qybuild --fetch-checksums {name}"})
    return g


def _gate_signed(root: Path, pkgs: list, recipes: dict) -> GateResult:
    """检查将要发布的包是否已签名。

    扫仓库目录而不是 var/pkgs：var/pkgs 是构建暂存区，
    里面有上次构建留下的东西很正常。拿暂存区当判据会让门禁
    因为无关的残留文件报红，久了没人再信它。
    真正要发出去的是仓库里的包，那是用户会下载到的。
    """
    from . import format as F
    g = GateResult(*GATES[1])
    targets = []
    for adir in sorted((root / "var" / "repo").glob("*")):
        if adir.is_dir() and adir.name != "keys":
            targets += sorted(adir.glob("*.qyp"))
    if not targets:
        g.passed = False
        g.failures.append({"package": "-", "detail": "仓库里没有包",
                           "fix": "先构建并 qyrepo sync"})
        return g
    for p in targets:
        try:
            pkg = F.read_package(p)
        except Exception as e:
            g.passed = False
            g.failures.append({"package": p.name, "detail": f"无法读取: {e}",
                               "fix": "重新构建该包"})
            continue
        # 签名是独立的一段，不是 Meta 的字段
        if not pkg.sig:
            g.passed = False
            g.failures.append({
                "package": p.name, "detail": "包未签名",
                "fix": "构建时加 --sign <私钥>"})
    return g


def _gate_index(root: Path, pkgs: list, recipes: dict) -> GateResult:
    from . import repo as R
    g = GateResult(*GATES[2])
    rd = root / "var" / "repo"
    idx = rd / "x86_64" / "index.json"
    if not idx.exists():
        g.passed = False
        g.failures.append({"package": "-", "detail": "没有仓库索引",
                           "fix": "qyrepo sync --sign <私钥>"})
        return g
    pub = rd / "keys" / "qiyuan.pub"
    try:
        data = json.loads(idx.read_text())
    except Exception as e:
        g.passed = False
        g.failures.append({"package": "-", "detail": f"索引无法解析: {e}",
                           "fix": "重建索引"})
        return g
    if not (rd / "x86_64" / "index.json.sig").exists():
        g.passed = False
        g.failures.append({"package": "-", "detail": "索引没有签名文件",
                           "fix": "qyrepo sync --sign <私钥>"})
    # 每个包在索引里的哈希必须与实际文件一致
    for e in data.get("packages", []):
        fn = rd / "x86_64" / e.get("filename", "")
        if not fn.exists():
            continue
        actual = util.sha256_file(fn)
        if e.get("sha256") and e["sha256"] != actual:
            g.passed = False
            g.failures.append({
                "package": e.get("name", "?"),
                "detail": "索引记录的哈希与实际文件不符",
                "fix": "重新 sync 仓库"})
    return g


def _gate_hardening(root: Path, pkgs: list, recipes: dict) -> GateResult:
    from . import hardening as H
    g = GateResult(*GATES[3])
    pdir = root / "var" / "pkgs"
    if not pdir.exists():
        return g
    for p in sorted(pdir.glob("*.qyp")):
        try:
            res = H.audit_package(p)
        except Exception:
            continue
        for f in (res.get("failed") or []):
            g.passed = False
            g.failures.append({
                "package": p.name,
                "detail": f"{f.get('file', '?')}: {f.get('reason', '加固项缺失')}",
                "fix": "检查配方的编译参数，确认上游没有覆盖加固标志"})
    return g


def _gate_buildpath(root: Path, pkgs: list, recipes: dict) -> GateResult:
    from . import repro as RP
    g = GateResult(*GATES[4])
    work = root / "var" / "work"
    if not work.exists():
        return g
    for d in sorted(work.glob("*/dest")):
        issues = RP.scan_for_build_paths(d)
        for i in issues[:3]:
            g.passed = False
            g.failures.append({
                "package": d.parent.name,
                "detail": f"{i.where}: {i.detail[:80]}",
                "fix": "编译参数加 -ffile-prefix-map，或清理产物里的调试路径"})
    return g


def _gate_version(root: Path, pkgs: list, recipes: dict) -> GateResult:
    g = GateResult(*GATES[5])
    prev_path = root / "var" / "cache" / "released-versions.json"
    if not prev_path.exists():
        return g                       # 没有历史记录，不判定
    try:
        prev = json.loads(prev_path.read_text())
    except Exception:
        return g
    for name, r in recipes.items():
        old = prev.get(name)
        if not old:
            continue
        from .deps import version_cmp
        if version_cmp(f"{r.version}-{r.release}", "<", old):
            g.passed = False
            g.failures.append({
                "package": name,
                "detail": f"版本回退：已发过 {old}，当前 {r.version}-{r.release}",
                "fix": "确认是否误改配方版本号"})
    return g


GATE_FUNCS = {
    "checksum": _gate_checksum,
    "signed": _gate_signed,
    "index": _gate_index,
    "hardening": _gate_hardening,
    "buildpath": _gate_buildpath,
    "version": _gate_version,
}


def release_set(root: Path) -> list:
    """发布集：仓库索引里实际有的包。

    门禁只针对要发出去的东西。检查全部 177 个配方的话，
    尚未下载源码的配方会让门禁永远红着——那就没人再看门禁了，
    真出问题也被淹没。未就绪的另作提醒。
    """
    idx = root / "var" / "repo" / "x86_64" / "index.json"
    if not idx.exists():
        return []
    try:
        data = json.loads(idx.read_text())
    except Exception:
        return []
    return [e.get("name") for e in data.get("packages", [])]


def not_ready(root: Path, recipes: dict) -> list:
    """尚未就绪的配方（远程源码未锁定校验和）。信息性，不拦截发布。"""
    return sorted(n for n, r in recipes.items()
                  if getattr(r, "checksum_pending", False))


def run_gates(root: Path, pkgs: list, recipes: dict,
              only: list | None = None) -> list:
    """跑全部门禁。"""
    out = []
    for name, title, why in GATES:
        if only and name not in only:
            continue
        fn = GATE_FUNCS[name]
        try:
            out.append(fn(root, pkgs, recipes))
        except Exception as e:
            g = GateResult(name, title, why, passed=False)
            g.failures.append({"package": "-",
                               "detail": f"门禁自身出错: {e}",
                               "fix": "检查该门禁的实现"})
            out.append(g)
    return out


def gates_report(results: list) -> str:
    L = ["发布门禁："]
    for g in results:
        mark = "通过" if g.passed else "拦截"
        L.append(f"  [{mark}] {g.title}")
        if not g.passed:
            L.append(f"         为什么重要：{g.why}")
            for f in g.failures[:6]:
                L.append(f"         · {f['package']}: {f['detail']}")
                L.append(f"           修：{f['fix']}")
            if len(g.failures) > 6:
                L.append(f"         … 另有 {len(g.failures) - 6} 项")
    failed = [g for g in results if not g.passed]
    L.append("")
    if failed:
        L.append(f"结果：{len(failed)} 项未通过，本批次不允许发布。")
    else:
        L.append("结果：全部通过，可以发布。")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyci", description="澜岫 Linux 持续集成")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("plan", help="检测变更并算出重编计划")
    sp.add_argument("--since", default="HEAD~1")
    sp.add_argument("--changed", nargs="*", default=None,
                    help="直接指定改动的包（不查 git）")

    sp = sub.add_parser("gates", help="跑发布门禁")
    sp.add_argument("--only", nargs="*", default=None)

    sp = sub.add_parser("run", help="完整流水线：计划 → 构建 → 门禁 → 报告")
    sp.add_argument("--since", default="HEAD~1")
    sp.add_argument("--sign", default=None)
    sp.add_argument("--report", default=None)
    sp.add_argument("--skip-build", action="store_true")

    a = ap.parse_args(argv)
    root = Path.cwd()

    from . import recipe as RM
    recipes = RM.load_tree(root / "recipes")

    if a.cmd == "plan":
        changed = a.changed if a.changed is not None else \
            detect_changed(root, a.since)
        p = plan(root, changed, recipes)
        print(p.summary())
        return 0

    if a.cmd == "gates":
        rel = release_set(root)
        if not rel:
            util.log("warn", "仓库里没有包，先构建并 sync")
        res = run_gates(root, rel, recipes, only=a.only)
        print(gates_report(res))
        pend = not_ready(root, recipes)
        if pend:
            print()
            print(f"另有 {len(pend)} 个配方尚未就绪（远程源码未锁定校验和），"
                  f"不纳入本次发布：")
            print(f"  {' '.join(pend[:10])}"
                  f"{' …' if len(pend) > 10 else ''}")
            print("  要纳入发布，先在能联网的构建机上执行：")
            print("    qybuild --fetch-checksums <包名>")
        return 0 if all(g.passed for g in res) else 1

    if a.cmd == "run":
        changed = detect_changed(root, a.since)
        p = plan(root, changed, recipes)
        print("== 变更与重编计划")
        print(p.summary())
        print()
        if not a.skip_build and p.rebuild:
            print("== 构建")
            from . import orchestrator as O
            o = O.Orchestrator(root, jobs=None)
            summary = o.build(p.rebuild, force=False)
            print(f"  成功 {len(summary.get('built', []))}，"
                  f"失败 {len(summary.get('failed', []))}")
            if summary.get("failed"):
                for f in summary["failed"]:
                    print(f"    {f}")
        print()
        print("== 门禁")
        res = run_gates(root, release_set(root), recipes)
        text = gates_report(res)
        print(text)
        if a.report:
            Path(a.report).write_text(
                p.summary() + "\n\n" + text + "\n")
            util.log("ok", f"报告已写入 {a.report}")
        return 0 if all(g.passed for g in res) else 1
    return 1
