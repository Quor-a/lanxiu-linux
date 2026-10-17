"""命令行入口：qybuild（构建）/ qypkg（包管理）/ qyrepo（仓库）。"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from qyos import util  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


# ============================================================ qybuild

def build_main(argv=None) -> int:
    from qyos import builder as B
    from qyos import format as fmt

    p = argparse.ArgumentParser(prog="qybuild", description="LANXIU Linux 构建系统")
    p.add_argument("targets", nargs="*", help="包名；留空或 all 表示全部")
    p.add_argument("--root", default=str(ROOT), help="项目根目录")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    p.add_argument("--force", action="store_true", help="忽略缓存强制重编")
    p.add_argument("--keep", action="store_true", help="保留构建中间目录")
    p.add_argument("--sign", metavar="PRIVKEY", help="打包时用该私钥签名")
    p.add_argument("--gen-key", metavar="PATH", help="生成 ed25519 密钥对后退出")
    p.add_argument("--list", action="store_true", help="列出配方树")
    p.add_argument("--deps", metavar="PKG", help="打印某包的构建顺序")
    p.add_argument("--audit", metavar="PKG", help="审计已构建包的二进制加固项")
    p.add_argument("--fetch-checksums", metavar="PKG", dest="fetch_sum",
                   help="下载源码算出 sha256 并写回配方（补齐后再构建）")
    p.add_argument("--kernel-config", action="store_true",
                   help="合并内核配置片段并校验，输出 .config")
    p.add_argument("--fragment", action="append", default=[],
                   help="内核配置片段路径，可重复")
    p.add_argument("--arch", default="x86_64", help="目标架构")
    p.add_argument("--target-arch", default=None,
                   help="交叉编译：产物运行的架构（与构建机不同时即交叉）")
    p.add_argument("--sysroot", default=None,
                   help="交叉编译时目标机根目录（头文件与库在这）")
    p.add_argument("--out", help="输出路径")
    p.add_argument("--initramfs", action="store_true", help="构建 initramfs")
    p.add_argument("--bootstrap", action="store_true",
                   help="分析自举：构建闭包、循环依赖、分关计划")
    p.add_argument("--include-bootstrap", action="store_true",
                   help="全量构建时一并构建工具链大包（需真实构建机）")
    p.add_argument("--targets", nargs="*", default=None,
                   help="自举分析的目标包（默认全部）")
    p.add_argument("--cycles", action="store_true",
                   help="分析配方库里的循环依赖，并给出打破方案")
    p.add_argument("--shlibdeps", metavar="PKG",
                   help="审计某包：产物实际链接了哪些库、应自动补哪些依赖")
    p.add_argument("--orchestrate", action="store_true",
                   help="用编排器分层并行构建（推荐用于全量构建）")
    p.add_argument("--report", metavar="PATH", help="编排后写出耗时报告")
    p.add_argument("-q", "--quiet", action="store_true")
    a = p.parse_args(argv)

    if a.gen_key:
        keyid = fmt.gen_keypair(Path(a.gen_key))
        util.log("ok", f"已生成密钥对 {a.gen_key}（keyid={keyid}）")
        return 0

    b = B.Builder(Path(a.root), jobs=a.jobs,
                  sign_key=Path(a.sign) if a.sign else None,
                  verbose=not a.quiet,
                  target_arch=getattr(a, "target_arch", None))
    if getattr(a, "sysroot", None):
        b.sysroot = Path(a.sysroot)
    if b.cross is not None:
        util.log("info", b.cross.describe())
        for w in b.cross.validate():
            util.log("warn", w)
        from . import crosstool as CT
        missing = CT.check_toolchain(b.target_arch)
        if missing:
            util.log("warn", f"交叉工具链尚不齐全，缺：{' '.join(missing[:6])}")
            util.log("info", f"需要 {CT.tool_prefix(b.target_arch)}gcc 等；"
                             f"先用 bootstrap 编出交叉工具链")

    if a.list:
        util.log("ok", f"配方树共 {len(b.recipes)} 个包")
        for name, r in sorted(b.recipes.items()):
            print(f"  {r.pkgid:<28} {r.summary}")
        return 0

    if a.deps:
        from qyos.deps import Universe
        u = Universe()
        for r in b.recipes.values():
            u.add(r)
        print(" -> ".join(u.resolve([a.deps])))
        return 0

    if getattr(a, "fetch_sum", None):
        from qyos import recipe as RM
        rec = RM.load(Path("recipes") / f"{a.fetch_sum}.py")
        remote = [x for x in rec.source if x.startswith(("http", "ftp"))]
        if not remote:
            util.log("err", f"{rec.name} 没有远程源码")
            return 1
        sums = []
        for url in remote:
            util.log("step", f"下载 {url}")
            try:
                from qyos import download as DL
                f = DL.Fetcher(Path("/tmp/qy-src-cache"))
                path = f.get(url)
                sums.append(util.sha256_file(path))
                util.log("ok", f"{path.name}  {sums[-1]}")
            except Exception as e:
                util.log("err", f"下载失败: {e}")
                return 1
        # 写回配方：去掉 checksum_pending，填上 sha256
        src = rec.path.read_text()
        import re
        src = re.sub(r"sha256 = \[.*?\]", "sha256 = ["
                     + ", ".join(f'"{x}"' for x in sums) + "]",
                     src, count=1, flags=re.S)
        src = re.sub(r"\n\n# .*?checksum_pending = True", "", src,
                     flags=re.S)
        src = src.replace("checksum_pending = True", "")
        rec.path.write_text(src)
        util.log("ok", f"已写回 {rec.path}")
        return 0

    if getattr(a, "kernel_config", False):
        from qyos import kernel as K
        frags = [K.parse_fragment(Path(x).read_text()) for x in a.fragment]
        if not frags:
            util.log("err", "至少要用 --fragment 指定一个配置片段")
            return 1
        cfg, changes = K.merge({}, frags)
        res = K.validate(cfg, a.arch)
        print(K.report(res))
        out = Path(a.out) if a.out else Path("kernel/config/merged.config")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(K.render(cfg))
        util.log("ok", f"合并 {len(frags)} 个片段 → {len(cfg)} 项，写入 {out}")
        if changes:
            util.log("info", f"片段覆盖基线 {len(changes)} 处")
        return 0 if res["ok"] else 1

    if getattr(a, "initramfs", False):
        from qyos import initramfs as IR
        out = Path(a.out) if a.out else Path("var/boot/initramfs.img")
        r = IR.build(root_target=getattr(a, "root", "") or "", out_path=out)
        info = IR.inspect(out)
        for p_ in info["problems"]:
            util.log("err", p_)
        util.log("ok" if info["bootable"] else "err",
                 f"initramfs {'可启动' if info['bootable'] else '不可启动'}"
                 f"（{info['count']} 项）")
        return 0 if info["bootable"] else 1

    if getattr(a, "bootstrap", False):
        from qyos import bootstrap as BS
        from qyos import recipe as RM
        recipes = RM.load_tree(Path("recipes"))
        plan_ = BS.plan(recipes, a.targets)
        print(BS.render_report(plan_))
        return 0 if plan_["ok"] else 1

    if a.audit:
        from qyos import hardening as H
        pkg = ROOT / "var" / "pkgs"
        cands = sorted(pkg.glob(f"{a.audit}-*.qyp"))
        if not cands:
            util.log("err", f"没有找到 {a.audit} 的构建产物")
            return 1
        res = H.audit_package(cands[-1])
        print(H.report(res))
        return 0 if not res["failed"] else 1

    if a.cycles:
        from qyos import recipe as RM
        from qyos import cycles as CY
        recipes = RM.load_tree(Path(a.root) / "recipes")
        try:
            cp = CY.plan(recipes)
        except CY.CycleError as e:
            util.log("err", str(e))
            return 1
        print(cp.summary())
        if cp.rebuild_pass2:
            util.log("warn", f"第二轮务必重编 "
                             f"{' '.join(cp.rebuild_pass2)}："
                             f"首轮它们是功能不全的断开版")
        return 0

    if a.shlibdeps:
        from qyos import shlibdeps as SD
        from qyos.deps import Universe
        rec = B.Builder(Path(a.root), arch=a.arch,
                        sign_key=Path(a.sign) if a.sign else None).recipes.get(a.shlibdeps)
        if rec is None:
            util.log("err", f"没有这个配方: {a.shlibdeps}")
            return 1
        u = Universe()
        for r in B.Builder(Path(a.root), arch=a.arch).recipes.values():
            u.add(r)
        b = B.Builder(Path(a.root), arch=a.arch)
        avail = {e.get("name") for e in b._repo_index().get("packages", [])}
        dest = b.paths["work"] / rec.name / "dest"
        if not dest.exists():
            util.log("warn", f"{rec.name} 还没构建过（无 {dest}）")
            print(f"  声明的依赖: {' '.join(rec.depends) or '（无）'}")
            return 0
        print(SD.report(SD.scan_package(dest, rec, u, available=avail)))
        return 0

    if a.orchestrate:
        from qyos import orchestrator as O
        o = O.Orchestrator(Path(a.root), jobs=a.jobs)
        summary = o.build(a.targets or None, force=a.force)
        if a.report:
            o.write_report(Path(a.report), summary)
        return 0 if not summary["failed"] else 1

    targets = a.targets or ["all"]
    print(f"[qybuild] 项目根 {a.root} · {b.sandbox.describe()}")
    if "all" in targets:
        b.build_all(force=a.force, keep=a.keep,
                    include_bootstrap=getattr(a, "include_bootstrap", False))
    else:
        # 定向构建不跳过任何包（用户点名要谁就编谁）；重包依赖已由
        # build_many 的 installed 过滤处理（交叉时 glibc 由 sysroot 提供）。
        b.build_many(targets, force=a.force, keep=a.keep)
    return 0


# ============================================================ qyrepo

def repo_main(argv=None) -> int:
    from qyos import repo as R
    p = argparse.ArgumentParser(prog="qyrepo", description="LANXIU Linux 仓库工具")
    p.add_argument("action", choices=["add", "index", "list", "verify",
                                      "prune", "sync", "archs", "manifest"])
    p.add_argument("--repo", default=str(ROOT / "var" / "repo"))
    p.add_argument("--arch", default=util.ARCH)
    p.add_argument("--pkgs", nargs="*", default=[], help="add 时要加入的包文件")
    p.add_argument("--sign", metavar="PRIVKEY")
    p.add_argument("--pubkey", metavar="PUBKEY")
    p.add_argument("--from-built", action="store_true",
                   help="add 时自动纳入 var/pkgs 下所有已构建包")
    a = p.parse_args(argv)

    repo_dir = Path(a.repo)

    if a.action == "archs":
        print(R.repo_report(repo_dir))
        for arch in R.architectures(repo_dir):
            probs = R.check_arch_consistency(repo_dir, arch)
            for x in probs:
                util.log("err", f"{arch}: {x}")
        return 0

    if a.action == "manifest":
        path = R.write_manifest(repo_dir, Path(a.sign) if a.sign else None)
        util.log("ok", f"已生成 {path}")
        import json as _j
        d = _j.loads(path.read_text())
        print(f"  架构: {' '.join(d['architectures'])}")
        return 0

    if a.action == "add":
        pkgs = list(a.pkgs)
        if a.from_built or not pkgs:
            pkgs += [str(x) for x in (ROOT / "var" / "pkgs").glob("*.qyp")]
        added = R.add_packages(repo_dir, pkgs, arch=a.arch)
        util.log("ok", f"新增 {len(added)} 个包到仓库")
        index = R.build_index(repo_dir, a.arch)
        R.write_index(repo_dir, index, a.arch,
                      Path(a.sign) if a.sign else None)
        return 0

    if a.action == "sync":
        # 仓库 = 当前构建产物的镜像：清掉仓库里所有旧包，重新纳入 var/pkgs
        # 版本号回退（开发时常见）时，只靠 prune 是清不干净的
        adir = repo_dir / a.arch
        if adir.exists():
            for p in adir.glob("*.qyp"):
                p.unlink()
        pkgs = [str(x) for x in (ROOT / "var" / "pkgs").glob("*.qyp")]
        R.add_packages(repo_dir, pkgs, arch=a.arch)
        index = R.build_index(repo_dir, a.arch)
        R.write_index(repo_dir, index, a.arch, Path(a.sign) if a.sign else None)
        util.log("ok", f"仓库已与构建产物同步（{index['count']} 个包）")
        # 交叉编译下最容易出的错：索引说 aarch64 而包里是 x86_64。
        # 客户端不会察觉，装到设备上才报格式错误——那时已经刷完机了。
        probs = R.check_arch_consistency(repo_dir, a.arch)
        for x in probs:
            util.log("err", x)
        if probs:
            return 1
        return 0

    if a.action == "index":
        index = R.build_index(repo_dir, a.arch)
        R.write_index(repo_dir, index, a.arch,
                      Path(a.sign) if a.sign else None)
        return 0

    if a.action == "prune":
        old = R.prune(repo_dir, a.arch)
        util.log("ok", f"清理了 {len(old)} 个被取代的旧包")
        index = R.build_index(repo_dir, a.arch)
        R.write_index(repo_dir, index, a.arch,
                      Path(a.sign) if a.sign else None)
        return 0

    if a.action == "list":
        idx = R.load_index(repo_dir, a.arch,
                           Path(a.pubkey) if a.pubkey else None,
                           require_sig=bool(a.pubkey))
        print(f"{idx['distro']} · {idx['arch']} · {idx['count']} 个包")
        for e in idx["packages"]:
            print(f"  {e['pkgid']:<26} {util.human_size(e['size']):>6}  {e['summary']}")
        return 0

    if a.action == "verify":
        idx = R.load_index(repo_dir, a.arch,
                           Path(a.pubkey) if a.pubkey else None,
                           require_sig=bool(a.pubkey))
        bad = []
        for e in idx["packages"]:
            f = repo_dir / e["arch"] / e["filename"]
            if not f.exists() or util.sha256_file(f) != e["sha256"]:
                bad.append(e["filename"])
        if bad:
            util.log("err", f"{len(bad)} 个包校验失败: {bad}")
            return 1
        util.log("ok", f"{idx['count']} 个包全部校验通过")
        return 0
    return 1


def release_main(argv=None) -> int:
    """发布工程：版本号、清单、变更日志、完整性校验。"""
    import argparse as _ap
    from qyos import release as RLM
    from qyos import profile as PFM

    ap = _ap.ArgumentParser(prog="qyrelease", description="LANXIU Linux 发布工程")
    ap.add_argument("action", choices=["profiles", "bump", "manifest",
                                       "verify", "changelog"])
    ap.add_argument("--version", default="0.1.0-alpha.1")
    ap.add_argument("--bump", default="stage",
                    choices=["major", "minor", "patch", "stage", "promote"])
    ap.add_argument("--dir", default="var/release", help="产物目录")
    ap.add_argument("--stage", default=RLM.ALPHA, choices=list(RLM.STAGES))
    ap.add_argument("--kernel", default="")
    ap.add_argument("--notes", default="")
    ap.add_argument("--sign", default=None, help="私钥路径")
    ap.add_argument("--pubkey", default=None)
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--profile")
    ap.add_argument("--since")
    a = ap.parse_args(argv)

    if a.action == "profiles":
        print(PFM.render_summary(PFM.PROFILES))
        if a.profile:
            pr = PFM.get(a.profile)
            # 显示"选了 8 个包实际会装 91 个"——这才是用户关心的数字。
            # 只报选中数会让桌面形态看起来很轻，装机时才发现要装 90 多个包。
            try:
                from qyos import recipe as RM
                from qyos import cycles as CY
                from qyos.deps import Universe
                rs = RM.load_tree(Path("recipes"))
                cpl = CY.plan(rs)
                u = Universe()
                for n, r in rs.items():
                    skip = set(cpl.broken_deps.get(n, ()))
                    u.add(type("W", (), {
                        "name": n, "version": r.version,
                        "release": r.release, "provides": r.provides,
                        "depends": [d for d in (r.depends or []) if d not in skip],
                        "makedepends": [d for d in (r.makedepends or [])
                                        if d not in skip]})())
                total = len(u.resolve(pr.all_packages()))
                print(f"\n实际会安装 {total} 个包（含依赖展开）")
            except Exception as e:
                print(f"\n（依赖展开失败: {e}）")
            print()
            print(f"# {pr.title}（{pr.name}）")
            print(pr.description)
            print()
            print("必装:", " ".join(pr.packages))
            if pr.extra_packages:
                print("可选:", " ".join(pr.extra_packages))
            print("排除:", "、".join(pr.excludes) or "（未声明）")
            print("服务:", " ".join(pr.services) or "（无）")
            print("内核片段:", " ".join(pr.kernel_fragments))
            print("需要 initramfs:", "是" if pr.initramfs else "否")
            print()
            print(PFM.render_sysctl(pr))
        return 0

    if a.action == "bump":
        v = RLM.Version.parse(a.version)
        nv = v.next(a.bump)
        print(f"{v} --{a.bump}--> {nv}")
        return 0

    if a.action == "manifest":
        d = Path(a.dir)
        if not d.exists():
            util.log("err", f"产物目录不存在: {d}")
            return 1
        rel = RLM.build_release(d, a.version, a.stage, kernel=a.kernel,
                                notes=a.notes)
        out = d / "RELEASE.json"
        RLM.write_manifest(rel, out,
                           Path(a.sign) if a.sign else None)
        util.log("ok", f"发布清单已写入 {out}（{len(rel.artifacts)} 个产物）")
        for art in rel.artifacts:
            print(f"  {art['name']}  {util.human_size(art['size'])}  "
                  f"{art['sha256'][:16]}…")
        return 0

    if a.action == "verify":
        m = Path(a.manifest) if a.manifest else Path(a.dir) / "RELEASE.json"
        if not m.exists():
            util.log("err", f"清单不存在: {m}")
            return 1
        r = RLM.verify_manifest(
            m, Path(a.pubkey) if a.pubkey else None)
        for pr in r["problems"]:
            util.log("err", pr)
        if r["ok"]:
            util.log("ok", f"发布 {r['version']} 校验通过："
                           f"{r['checked']}/{r['artifacts']} 个产物完好")
        return 0 if r["ok"] else 1

    if a.action == "changelog":
        entries = RLM.changelog_since(Path("."), a.since)
        print(RLM.render_changelog(entries, a.version))
        return 0
    return 1


def security_main(argv=None) -> int:
    """安全公告与漏洞响应。"""
    import argparse as _ap
    from qyos import security as SE

    ap = _ap.ArgumentParser(prog="qysec", description="LANXIU Linux 安全响应")
    ap.add_argument("action", choices=["scan", "list", "show", "add",
                                       "rebuild", "by-cve"])
    ap.add_argument("--root", help="要扫描的已装系统根目录")
    ap.add_argument("--db", default="var/security/advisories.json")
    ap.add_argument("--id")
    ap.add_argument("--cve")
    ap.add_argument("--package")
    ap.add_argument("--changed", nargs="*", help="重建闭包：改了哪些包")
    a = ap.parse_args(argv)

    db = SE.AdvisoryDB(Path(a.db))

    if a.action == "list":
        if not db.advisories:
            util.log("info", "暂无安全公告")
            return 0
        rows = sorted(db.advisories.values(),
                      key=lambda x: SE.SEVERITY_ORDER.get(x.severity, 9))
        for adv in rows:
            print(f"  {adv.severity:9s} {adv.id}  {adv.title}")
        print(f"\n共 {len(rows)} 条")
        return 0

    if a.action == "show":
        adv = db.get(a.id or "")
        if not adv:
            util.log("err", f"公告不存在: {a.id}")
            return 1
        print(json.dumps(adv.to_dict(), ensure_ascii=False, indent=1))
        return 0

    if a.action == "by-cve":
        cve = a.cve or (sys.argv[-1] if len(sys.argv) > 2 else "")
        for adv in db.by_cve(cve):
            print(f"  {adv.id} [{adv.severity}] {adv.title}")
            for x in adv.affected:
                print(f"    {x.package}: 引入 {x.introduced} "
                      f"修复 {x.fixed or '未修复'}")
        return 0

    if a.action == "scan":
        root = Path(a.root) if a.root else Path("/")
        res = SE.scan_system(root, db)
        print(SE.render_scan(res))
        return 1 if res["total"] else 0

    if a.action == "rebuild":
        from qyos import recipe as RM
        recipes = RM.load_tree(Path("recipes"))
        changed = a.changed or []
        if not changed:
            util.log("err", "请用 --changed 指定改了哪些包")
            return 1
        order = SE.rebuild_closure(recipes, changed)
        print(f"改动 {', '.join(changed)} 后需要重编 {len(order)} 个包：")
        for n in order:
            print(f"  {n}")
        return 0

    if a.action == "add":
        util.log("info", "请用 JSON 文件定义公告后导入（交互式录入待实现）")
        return 1
    return 1


def disk_main(argv=None) -> int:
    """磁盘分区与装机脚本。"""
    import argparse as _ap
    from qyos import disk as DK
    from qyos import image as IM
    from qyos import bootloader as BL

    ap = _ap.ArgumentParser(prog="qydisk", description="LANXIU Linux 磁盘与镜像")
    ap.add_argument("action", choices=["layout", "scripts", "image", "verify"])
    ap.add_argument("--disk", default="/dev/sda")
    ap.add_argument("--size", default="100G", help="磁盘大小，如 100G / 500G")
    ap.add_argument("--memory", default="8G", help="内存大小，用于算 swap")
    ap.add_argument("--no-uefi", action="store_true", help="legacy BIOS 启动")
    ap.add_argument("--no-home", action="store_true", help="不单独分 /home")
    ap.add_argument("--root-fs", default="ext4")
    ap.add_argument("--bootloader", default="grub", choices=["grub", "efi-stub"])
    ap.add_argument("--out", help="输出路径")
    ap.add_argument("--image-size", help="镜像大小，如 4G（默认按内容估算）")
    ap.add_argument("--target", default="/mnt/qiyuan")
    a = ap.parse_args(argv)

    def parse_size(x: str) -> int:
        x = x.strip().upper()
        for suf, mul in (("GIB", 1024**3), ("GB", 1000**3), ("G", 1024**3),
                         ("MIB", 1024**2), ("MB", 1000**2), ("M", 1024**2),
                         ("TIB", 1024**4), ("TB", 1000**4), ("T", 1024**4)):
            if x.endswith(suf):
                return int(float(x[:-len(suf)]) * mul)
        return int(x)

    total = parse_size(a.size)
    mem = parse_size(a.memory)
    layout = DK.default_layout(a.disk, total, mem, uefi=not a.no_uefi,
                               separate_home=not a.no_home,
                               root_fs=a.root_fs, bootloader=a.bootloader)
    v = DK.validate(layout, total)
    for e in v["errors"]:
        util.log("err", e)
    for w in v["warnings"]:
        util.log("warn", w)
    if not v["ok"]:
        return 1

    if a.action == "layout":
        print(BL.image_layout_report(layout))
        print()
        print("## fstab（装机时填入真实 UUID）")
        print(DK.render_fstab(layout.partitions))
        return 0

    if a.action == "scripts":
        out = Path(a.out) if a.out else Path("var/install/scripts")
        written = IM.installer_scripts(Path("."), layout, out,
                                       target=a.target,
                                       bootloader=a.bootloader)
        util.log("ok", f"已生成 {len(written)} 个脚本到 {out}")
        for n, p_ in sorted(written.items()):
            print(f"  {p_.name}")
        return 0

    if a.action == "image":
        out = Path(a.out) if a.out else Path("var/install/qiyuan.img")
        img = IM.build_disk_image(
            Path(a.target), out,
            size=parse_size(a.image_size) if a.image_size else None)
        scripts = IM.installer_scripts(Path("."), layout, out.parent / "scripts",
                                       target=a.target, bootloader=a.bootloader)
        print(IM.build_report(img, layout, scripts))
        util.log("warn", "镜像文件已创建，但分区表与引导器需在具备 "
                         "loop 设备与 root 权限的机器上执行脚本完成")
        return 0

    if a.action == "verify":
        out = Path(a.out) if a.out else Path("var/install/qiyuan.img")
        r = IM.verify_image(out)
        for pr in r["problems"]:
            util.log("err", pr)
        for n in r["notes"]:
            util.log("warn", n)
        util.log("info", "以下检查需要 loop 设备/root 权限，已在沙盒中跳过：")
        for sk in r["skipped_checks"]:
            util.log("info", f"  - {sk}")
        return 0 if r["bootable"] else 1
    return 1


def rootfs_report(check: dict) -> str:
    from qyos import rootfs as RF
    return RF.report_boot_check(check)


# ============================================================ qypkg

def pkg_main(argv=None) -> int:
    from qyos import pkgmgr as PM
    from qyos import transaction as txn
    from qyos import repo as R
    p = argparse.ArgumentParser(prog="qypkg", description="LANXIU Linux 包管理器")
    p.add_argument("--root", default="/", help="目标根文件系统（测试用）")
    p.add_argument("--repo", default=str(ROOT / "var" / "repo"))
    p.add_argument("--pubkey", default=str(ROOT / "var" / "repo" / "keys" / "qiyuan.pub"))
    p.add_argument("--allow-unsigned", action="store_true")
    p.add_argument("-n", "--dry-run", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("install", aliases=["-S"], help="安装包")
    sp.add_argument("names", nargs="+")
    sp.add_argument("--as-deps", action="store_true")
    sp.add_argument("-n", "--dry-run", action="store_true", help="只演练不写入")

    sp = sub.add_parser("remove", aliases=["-R"], help="卸载包")
    sp.add_argument("names", nargs="+")
    sp.add_argument("-r", "--recursive", action="store_true")
    sp.add_argument("--clean-orphans", action="store_true")

    sp = sub.add_parser("upgrade", aliases=["-U"], help="升级")
    sp.add_argument("names", nargs="*")
    sp.add_argument("-n", "--dry-run", action="store_true")

    sp = sub.add_parser("list", aliases=["-Q"], help="列出已安装")
    sp = sub.add_parser("search", aliases=["-Ss"], help="搜索仓库")
    sp.add_argument("keyword")
    sp = sub.add_parser("info", aliases=["-Qi"], help="查看详情")
    sp.add_argument("name")
    sp = sub.add_parser("owns", aliases=["-Qo"], help="查文件属于哪个包")
    sp.add_argument("path")
    sp = sub.add_parser("why", help="查谁依赖它（判断能否安全卸载）")
    sp.add_argument("name")
    sp = sub.add_parser("verify", help="校验文件是否被改动")
    sp.add_argument("name", nargs="?")
    sp = sub.add_parser("orphans", help="清理孤儿包")
    sp = sub.add_parser("history", help="操作历史")
    sp.add_argument("--limit", type=int, default=20)
    sp = sub.add_parser("snapshots", help="列出可用于回滚的快照")
    sp = sub.add_parser("rollback", help="回滚到指定快照")
    sp.add_argument("txid")
    sp = sub.add_parser("recover", help="检查并回滚未完成的事务")
    sp.add_argument("--dry-run", action="store_true", help="只报告不回滚")
    sp = sub.add_parser("assemble", help="组装可启动的根文件系统")
    sp.add_argument("packages", nargs="*")
    sp.add_argument("--no-devnodes", action="store_true")
    sp.add_argument("--arch", default=None,
                    help="异架构 rootfs（如 aarch64，配对应仓库）")
    sp = sub.add_parser("bootcheck", help="检查根目录能否启动")
    sp = sub.add_parser("manifest", help="生成根目录完整清单")
    sp.add_argument("--out", help="清单输出路径")

    sp = sub.add_parser("foreign", help="安装外来格式包（deb/rpm/apk/pacman）")
    sp.add_argument("files", nargs="+", help="外来包文件路径")
    sp.add_argument("--info", action="store_true", help="只查看元信息不安装")
    sp.add_argument("--force-arch", action="store_true",
                    help="架构不符时强装（可能无法运行）")
    sp.add_argument("--dry-run", action="store_true", help="只演练不写入")

    sp = sub.add_parser("layer", help="overlayfs 分层：就地接管其他系统")
    sp.add_argument("action", choices=["take", "leave", "list"],
                    help="take=接管 leave=还原 list=列出")
    sp.add_argument("--base", help="原系统根目录（take 时必填）")
    sp.add_argument("--name", default="default", help="层名")
    sp.add_argument("--keep-upper", action="store_true",
                    help="leave 时保留写入层（迁移用）")

    a = p.parse_args(argv)
    a.dry_run_sub = getattr(a, "dry_run", False) if a.cmd else False
    pubkey = Path(a.pubkey) if Path(a.pubkey).exists() else None

    # 事务管理命令不能构造 Manager：它初始化时会自动恢复 pending 事务，
    # 那样 recover --dry-run 就永远看不到待回滚的事务了
    if a.cmd in ("recover", "snapshots", "rollback"):
        if a.cmd == "snapshots":
            snaps = txn.list_snapshots(Path(a.root))
            if not snaps:
                print("没有可用快照")
            for s_ in snaps:
                print(f"  {s_['txid']}  {s_['action']:<10} "
                      f"{s_['files']:>3} 个文件  {s_['time']}")
            return 0
        if a.cmd == "rollback":
            n = txn.rollback_to(Path(a.root), a.txid)
            util.log("ok", f"已回滚 {n} 个文件")
            return 0
        pend = txn.pending_transactions(Path(a.root))
        if not pend:
            util.log("ok", "没有未完成的事务")
        for j in pend:
            print(f"  {j['txid']}  {j['action']}  {len(j['entries'])} 项变更  "
                  f"{j.get('detail', '')}")
        if getattr(a, "dry_run", False):
            util.log("warn", "演练模式：以上事务尚未回滚")
            return 0
        n = txn.recover(Path(a.root), auto=True)
        if n:
            util.log("ok", f"已回滚 {n} 个未完成的事务")
        return 0

    if a.cmd == "assemble":
        from qyos import system as SY
        res = SY.assemble(Path(a.root), a.packages, Path(a.repo), pubkey,
                          allow_unsigned=a.allow_unsigned,
                          make_devnodes=not getattr(a, "no_devnodes", False),
                          arch=getattr(a, "arch", None))
        print()
        print(rootfs_report(res["check"]))
        return 0 if res["bootable"] else 1

    if a.cmd == "bootcheck":
        from qyos import rootfs as RF
        res = RF.boot_check(Path(a.root))
        print(RF.report_boot_check(res))
        return 0 if not res["error"] else 1

    if a.cmd == "manifest":
        from qyos import system as SY
        data = SY.manifest(Path(a.root), Path(a.out) if a.out else None)
        print(f"共 {data['file_count']} 个文件，{len(data['packages'])} 个包")
        return 0

    dry = a.dry_run or getattr(a, "dry_run_sub", False)
    m = PM.Manager(Path(a.root), Path(a.repo), pubkey,
                   dry_run=dry, allow_unsigned=a.allow_unsigned)

    try:
        if a.cmd == "layer":
            from qyos import layering as LG
            if a.action == "list":
                rows = LG.list_layers()
                if not rows:
                    print("（还没有任何层）")
                    return 0
                for r in rows:
                    st = "已挂载" if r.get("mounted") else "未挂载"
                    print(f"  {r.get('name') or '?':<12} {st}  "
                          f"底层={r.get('base', r.get('lower'))}")
                return 0
            if a.action == "take":
                if not a.base:
                    util.log("err", "take 需要 --base <原系统根>")
                    return 1
                base = Path(a.base).resolve()
                snap = LG.takeover_snapshot(base)
                if snap.get("distro"):
                    util.log("info", f"接管目标: {snap['distro']}"
                             f"（{snap.get('package_system', '?')} 包系统）")
                try:
                    m = LG.create_layer(base, a.name)
                except LG.LayerError as e:
                    util.log("err", str(e))
                    return 1
                util.log("ok", f"层 '{a.name}' 已挂载")
                print(f"  原系统（只读）: {m.lower}")
                print(f"  写入层:         {m.upper}")
                print(f"  接下来把包装进: qypkg --root {m.merged} foreign <包>")
                print(f"  还原:           qypkg layer leave --name {a.name}")
                return 0
            # leave
            try:
                LG.destroy_layer(a.name, keep_upper=a.keep_upper)
            except LG.LayerError as e:
                util.log("err", str(e))
                return 1
            util.log("ok", f"层 '{a.name}' 已卸载"
                     f"{'（写入层保留）' if a.keep_upper else '，原系统未动过'}")
            return 0
        if a.cmd == "foreign":
            from qyos import foreign as FG
            rc = 0
            for f in a.files:
                fp = Path(f)
                try:
                    pkg = FG.read_foreign(fp)
                except FG.ForeignError as e:
                    util.log("err", f"{fp.name}: {e}")
                    rc = 1
                    continue
                if a.info:
                    print(FG.describe(pkg))
                    continue
                try:
                    r = FG.install_foreign(
                        fp, Path(a.root),
                        force_arch=a.force_arch, dry_run=a.dry_run)
                except FG.ForeignError as e:
                    util.log("err", f"{fp.name}: {e}")
                    rc = 1
                    continue
                tag = "[演练] " if a.dry_run else ""
                util.log("ok", f"{tag}已安装 {r['name']} {r['version']}"
                         f"（{r['kind']}，{r['arch']}，"
                         f"{r['files']} 个文件）")
                if r["untranslated"]:
                    util.log("warn", "依赖未自动对应LANXIU包名: "
                             + " ".join(r["untranslated"])
                             + " —— 装完若跑不起来先查这些")
            return rc
        elif a.cmd in ("install", "-S"):
            m.install(a.names, as_explicit=not a.as_deps)
        elif a.cmd in ("remove", "-R"):
            m.remove(a.names, recursive=a.recursive,
                     clean_orphans=getattr(a, "clean_orphans", False))
        elif a.cmd in ("upgrade", "-U"):
            m.upgrade(a.names or None)
        elif a.cmd in ("list", "-Q"):
            rows = m.list_installed()
            print(f"已安装 {len(rows)} 个包（root={a.root}）")
            for r in rows:
                print(f"  {r['name']:<20} {r['version']}-{r['release']:<4} "
                      f"{r.get('reason',''):<11} {r['summary']}")
        elif a.cmd in ("search", "-Ss"):
            for e in m.search(a.keyword):
                print(f"  {e['pkgid']:<26} {e['summary']}")
        elif a.cmd in ("info", "-Qi"):
            info = m.info(a.name)
            if not info:
                util.log("err", f"找不到 {a.name}")
                return 1
            for k, v in info.items():
                if k in ("meta",):
                    continue
                print(f"  {k:<14} {v}")
        elif a.cmd in ("owns", "-Qo"):
            # 用户可能给绝对路径（指目标系统里的位置）也可能给相对路径
            path = a.path.lstrip("/")
            owner = m.db.owner_of(path) or m.db.owner_of("/" + path)
            if owner:
                print(f"/{path} 属于 {owner}")
            else:
                util.log("err", f"/{path} 不属于任何已安装的包")
                util.log("info",
                         "可能由装机脚本生成，或不是通过包管理器安装的")
                return 1
        elif a.cmd == "why":
            deps = m.db.dependents(a.name)
            if not deps:
                if not m.db.get(a.name):
                    util.log("err", f"{a.name} 未安装")
                    return 1
                print(f"没有已安装的包依赖 {a.name}，可以安全卸载")
            else:
                print(f"{len(deps)} 个已安装的包依赖 {a.name}：")
                for d in deps:
                    print(f"  {d}")
        elif a.cmd == "verify":
            probs = m.verify(getattr(a, "name", None))
            if not probs:
                util.log("ok", "所有已安装包的文件校验通过")
            for pkg, path, why in probs:
                print(f"  {pkg:<16} {why:<6} {path}")
            return 1 if probs else 0
        elif a.cmd == "orphans":
            m.clean_orphans()
        elif a.cmd == "history":
            for h in m.history(a.limit):
                print(f"  #{h['id']:<4} {h['action']:<9} {h['detail']}")
    except (PM.PkgError, R.RepoError) as e:
        util.log("err", str(e))
        return 1
    return 0
