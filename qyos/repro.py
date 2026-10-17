"""可复现构建（Reproducible Builds）。

同一个配方在任何机器上、任何时间构建，产出的包逐字节相同。
这不是洁癖，是发行版能不能被信任的基础：

* **能验证**：用户可以自己重编一遍，比对哈希，确认官方发布的包
  里没有夹带东西。做不到可复现，这条验证路径就不存在。
* **能审计**：供应链攻击（当年的 xz 后门）正是靠"发布包与源码
  不一致"存在的。可复现让这种不一致必然暴露。
* **能增量**：产出的哈希稳定，缓存才可靠。

破坏可复现的常见来源，逐个处理：

1. **时间戳**：tar 里的 mtime、元数据里的打包时间。
   用 SOURCE_DATE_EPOCH 统一钳制。
2. **构建路径**：产物里嵌入 /home/xxx/build/... 。
   装在机器上不影响运行，但每次构建路径不同 → 哈希不同。
3. **文件顺序**：os.walk 的顺序在不同文件系统上可能不同。
   必须排序后添加。
4. **构建机信息**：主机名、用户名、内核版本写进元数据。
5. **归档器版本**：不同 tar/gzip 版本压缩结果不同。
   这个不追求跨版本一致，只保证同一工具链下稳定。
"""
from __future__ import annotations

import gzip
import hashlib
import io
import json
import os
import tarfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import util

# SOURCE_DATE_EPOCH：可复现构建的标准环境变量。
# 设定后所有时间戳钳制到这个值，构建不再依赖当前时间。
ENV_EPOCH = "SOURCE_DATE_EPOCH"

# 产物里不该出现的构建机痕迹
BUILD_HOST_MARKERS = (
    "/data/workspace",      # 沙盒路径
    "/tmp/",
    "/home/",
    "/root/",
    "/build/",
    "/var/tmp/",
)


class ReproError(RuntimeError):
    pass


def source_date_epoch(default: int | None = None) -> int:
    """取构建时间戳。

    优先 SOURCE_DATE_EPOCH（可复现构建的标准做法）；
    没设就退回 git 最后一次提交时间——那也是稳定的，
    比"当前时间"好得多：同一份源码树编出来始终一样。
    """
    v = os.environ.get(ENV_EPOCH)
    if v and v.isdigit():
        return int(v)
    if default is not None:
        return default
    # 退回 git 提交时间
    try:
        import subprocess
        r = subprocess.run(["git", "log", "-1", "--format=%ct"],
                           capture_output=True, text=True, timeout=10)
        if r.returncode == 0 and r.stdout.strip().isdigit():
            return int(r.stdout.strip())
    except Exception:
        pass
    return 0


def epoch_from_recipe(rec) -> int:
    """从配方推导时间戳。没有 git 时用它。"""
    v = getattr(rec, "source_date_epoch", None)
    if v:
        return int(v)
    return source_date_epoch()


# ---------------------------------------------------------------- tar 归一化

@dataclass
class TarPolicy:
    """打包时的归一化策略。"""
    mtime: int | None = None          # None 表示保留原值（不可复现）
    uid: int = 0
    gid: int = 0
    uname: str = ""
    gname: str = ""
    sort: bool = True

    def apply(self, ti: tarfile.TarInfo) -> tarfile.TarInfo:
        if self.mtime is not None:
            ti.mtime = self.mtime
        ti.uid = self.uid
        ti.gid = self.gid
        ti.uname = self.uname
        ti.gname = self.gname
        return ti


def make_repro_tar(src_dir: Path, out_path: Path, comp: str = "gz",
                   epoch: int | None = None) -> int:
    """打一个可复现的 tar。条目排序 + 时间戳钳制 + 属主归一。

    与 util.make_tar 的区别就在这三点。原实现用文件系统上的
    mtime 和属主，换台机器编出来的哈希就不一样。
    """
    src_dir = Path(src_dir)
    policy = TarPolicy(mtime=epoch)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    entries = []
    for root, dirs, files in os.walk(str(src_dir)):
        dirs.sort()
        for d in dirs:
            entries.append(os.path.join(root, d))
        for f in sorted(files):
            entries.append(os.path.join(root, f))
    if policy.sort:
        # 按 arcname 排序而不是遍历顺序：
        # os.walk 在不同文件系统上返回顺序可能不同，
        # 不排序的话同一份源码编出的包字节不同
        entries.sort(key=lambda p: os.path.relpath(p, str(src_dir)))

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for path in entries:
            arcname = os.path.relpath(path, str(src_dir))
            ti = tf.gettarinfo(path, arcname=arcname)
            if ti is None:
                continue
            ti = policy.apply(ti)
            if ti.isreg():
                with open(path, "rb") as fh:
                    tf.addfile(ti, fh)
            else:
                tf.addfile(ti)
    raw = buf.getvalue()

    if comp == "gz":
        # mtime=0：gzip 头里会写时间戳，不固定它每次都不同
        blob = gzip.compress(raw, compresslevel=9, mtime=0)
    elif comp == "xz":
        import lzma
        blob = lzma.compress(raw, preset=6)
    elif comp == "none":
        blob = raw
    else:
        raise ReproError(f"不支持的压缩: {comp}")

    out_path.write_bytes(blob)
    return len(entries)


# ---------------------------------------------------------------- 产物检查

@dataclass
class ReproIssue:
    kind: str        # timestamp / build_path / host_info / order
    where: str
    detail: str


def scan_for_build_paths(root: Path) -> list:
    """扫产物里是否嵌入了构建机绝对路径。

    装在机器上通常不影响运行，但每次构建路径不同 → 哈希不同，
    而且把构建机的目录结构泄露给了用户。
    """
    root = Path(root)
    issues = []
    exts = {".a", ".o", ".so", ".la", ".pc", ".cmake", ".h", ".hpp",
            ".c", ".cc", ".py", ".sh", ".txt", ".json", ".xml", ".mk"}
    for p in sorted(root.rglob("*")):
        if not p.is_file() or p.is_symlink():
            continue
        if p.suffix not in exts and p.name.count(".") == 0:
            # 二进制可执行文件也查，但要限制体积
            pass
        if p.stat().st_size > 8 * 1024 * 1024:
            continue
        try:
            data = p.read_bytes()
        except OSError:
            continue
        if b"\0" not in data[:1024] and len(data) > 64 * 1024:
            pass
        for marker in BUILD_HOST_MARKERS:
            idx = data.find(marker.encode())
            if idx >= 0:
                snippet = data[max(0, idx - 20):idx + 60]
                try:
                    snippet = snippet.decode("utf-8", "replace")
                except Exception:
                    snippet = repr(snippet)
                issues.append(ReproIssue(
                    kind="build_path",
                    where=str(p.relative_to(root)),
                    detail=f"含构建机路径 {marker}：「…{snippet}…」"))
                break
    return issues


def check_tar_reproducible(tar_path: Path) -> list:
    """检查一个 tar 归档本身是否可复现：时间戳、属主、顺序。"""
    issues = []
    try:
        with tarfile.open(tar_path) as tf:
            mtimes, names = [], []
            for ti in tf:
                names.append(ti.name)
                mtimes.append(ti.mtime)
                if ti.uid != 0 or ti.gid != 0:
                    issues.append(ReproIssue(
                        "owner", ti.name,
                        f"属主 {ti.uid}:{ti.gid}，应为 0:0"))
                if ti.uname or ti.gname:
                    issues.append(ReproIssue(
                        "owner", ti.name,
                        f"属主名 {ti.uname}:{ti.gname}，应为空"))
    except tarfile.TarError as e:
        return [ReproIssue("format", str(tar_path), f"无法读取: {e}")]

    if names != sorted(names):
        issues.append(ReproIssue(
            "order", str(tar_path), "条目未按名称排序，遍历顺序会影响结果"))
    if mtimes:
        span = max(mtimes) - min(mtimes)
        nonzero = sum(1 for m in mtimes if m != 0)
        if nonzero and span > 0:
            issues.append(ReproIssue(
                "timestamp", str(tar_path),
                f"{nonzero} 个条目的 mtime 各不相同"
                f"（跨度 {int(span)} 秒），每次构建结果都不同"))
    return issues


def check_gzip_header(path: Path) -> list:
    """gzip 头里的 mtime 必须固定，否则同一内容每次压缩结果都不同。"""
    try:
        with open(path, "rb") as f:
            head = f.read(10)
    except OSError:
        return []
    if head[:2] != b"\x1f\x8b":
        return []
    mtime = int.from_bytes(head[4:8], "little")
    if mtime != 0:
        return [ReproIssue("timestamp", str(path),
                          f"gzip 头 mtime={mtime}，应为 0")]
    return []


def check_metadata(meta: dict) -> list:
    """元数据里不能出现会变的东西。"""
    issues = []
    b = meta.get("build", {}) or {}

    # 架构不是"构建机信息"——包本来就该声明自己编给哪个架构。
    # 只有当它与 meta.arch 不一致时才是真的泄露了构建机信息。
    host = b.get("host")
    if host and host != meta.get("arch"):
        issues.append(ReproIssue(
            "host_info", "meta.build.host",
            f"记录了 {host}，与包声明的架构 {meta.get('arch')} 不一致——"
            f"交叉编译时这里是构建机而非目标机"))

    # 已按 SOURCE_DATE_EPOCH 固定过的时间戳是可复现的，不该报。
    # 不区分的话，可复现构建也会被判不合格，
    # 维护者看到"总是有 1 处问题"就会忽略这条检查。
    pinned = bool(b.get("reproducible"))
    for k in ("pack_time", "build_time", "timestamp"):
        if not b.get(k):
            continue
        if pinned:
            continue
        issues.append(ReproIssue(
            "timestamp", f"meta.build.{k}",
            f"记录了打包时刻 {b[k]}（每次构建都不同）；"
            f"设 SOURCE_DATE_EPOCH 后可复现"))
    return issues


def report(issues: list) -> str:
    if not issues:
        return "可复现性检查通过：未发现会随构建环境变化的内容"
    L = [f"发现 {len(issues)} 处影响可复现的问题："]
    by = {}
    for i in issues:
        by.setdefault(i.kind, []).append(i)
    titles = {"timestamp": "时间戳", "build_path": "构建机路径",
              "order": "顺序", "owner": "属主", "host_info": "构建机信息"}
    for kind, items in sorted(by.items()):
        L.append(f"\n  【{titles.get(kind, kind)}】{len(items)} 处")
        for i in items[:5]:
            L.append(f"    {i.where}")
            L.append(f"      {i.detail}")
        if len(items) > 5:
            L.append(f"    … 另有 {len(items) - 5} 处")
    return "\n".join(L)


def compare_bytes(a: bytes, b: bytes) -> tuple:
    """比对两次构建的字节。返回 (是否相同, 首个差异位置, 说明)。"""
    if a == b:
        return True, -1, "逐字节相同"
    if len(a) != len(b):
        return False, min(len(a), len(b)), \
            f"长度不同：{len(a)} vs {len(b)}"
    for i in range(len(a)):
        if a[i] != b[i]:
            ctx_a = a[max(0, i - 16):i + 16]
            ctx_b = b[max(0, i - 16):i + 16]
            return False, i, (f"第 {i} 字节不同\n"
                              f"    A: {ctx_a!r}\n"
                              f"    B: {ctx_b!r}")
    return True, -1, "逐字节相同"


def check_package(pkg_path: Path) -> list:
    """检查一个已构建的包是否可复现。"""
    from . import format as F
    issues = []
    pkg = F.read_package(pkg_path)
    m = pkg.meta
    meta_dict = m.to_dict() if hasattr(m, "to_dict") else dict(
        getattr(m, "__dict__", {}))
    issues += check_metadata(meta_dict)
    # 数据段按 header 里的偏移取出（数据段是压缩过的 tar）
    import tempfile
    raw = pkg.raw[pkg.data_off:pkg.data_off + pkg.data_len]
    with tempfile.TemporaryDirectory() as td:
        t = Path(td) / "data.tar"
        t.write_bytes(raw)
        # 可能是 gz/xz 压缩，tarfile 能自动识别
        issues += check_tar_reproducible(t)
    return issues


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyrepro",
                                 description="澜岫 Linux 可复现构建")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("epoch", help="显示当前会用的构建时间戳")

    sp = sub.add_parser("check", help="检查已构建的包")
    sp.add_argument("package")

    sp = sub.add_parser("scan", help="扫构建产物的构建机路径")
    sp.add_argument("--root", default="var/work")

    sp = sub.add_parser("verify", help="连编两次并逐字节比对")
    sp.add_argument("package")
    sp.add_argument("--sign", default=None)

    a = ap.parse_args(argv)

    if a.cmd == "epoch":
        e = source_date_epoch()
        import os
        if os.environ.get(ENV_EPOCH):
            print(f"SOURCE_DATE_EPOCH = {e}（来自环境变量）")
        elif e:
            print(f"{e}（来自 git 最后提交时间；"
                  f"设 {ENV_EPOCH} 可显式指定）")
        else:
            print(f"未设定。构建会写入当前时间，产物不可复现。")
            print(f"  建议：export {ENV_EPOCH}=<git log -1 --format=%ct>")
        return 0

    if a.cmd == "check":
        issues = check_package(Path(a.package))
        print(report(issues))
        return 0 if not issues else 1

    if a.cmd == "scan":
        root = Path(a.root)
        if not root.exists():
            util.log("warn", f"{root} 不存在（还没构建过）")
            return 0
        total = []
        for d in sorted(root.glob("*/dest")):
            got = scan_for_build_paths(d)
            if got:
                total += got
                print(f"{d.parent.name}: {len(got)} 处")
        print()
        print(report(total))
        return 0 if not total else 1

    if a.cmd == "verify":
        import os
        import shutil
        import subprocess
        os.environ[ENV_EPOCH] = str(source_date_epoch() or 1700000000)
        from pathlib import Path as P
        pkgdir = P("var/pkgs")
        name = a.package
        # 第一次
        shutil.rmtree("var/work", ignore_errors=True)
        cmd = ["./bin/qybuild", name, "--force"]
        if a.sign:
            cmd += ["--sign", a.sign]
        r1 = subprocess.run(cmd, capture_output=True, text=True)
        if r1.returncode != 0:
            util.log("err", f"第一次构建失败: {r1.stderr[-300:]}")
            return 1
        found = sorted(pkgdir.glob(f"{name}-*.qyp"))
        if not found:
            util.log("err", "没找到产物")
            return 1
        h1 = util.sha256_file(found[-1])
        b1 = found[-1].read_bytes()
        # 第二次
        shutil.rmtree("var/work", ignore_errors=True)
        r2 = subprocess.run(cmd, capture_output=True, text=True)
        if r2.returncode != 0:
            util.log("err", f"第二次构建失败")
            return 1
        found2 = sorted(pkgdir.glob(f"{name}-*.qyp"))
        b2 = found2[-1].read_bytes()
        same, pos, why = compare_bytes(b1, b2)
        print(f"{name}: {why}")
        if same:
            util.log("ok", f"两次构建逐字节相同（sha256 {h1[:16]}…）")
            return 0
        util.log("err", "两次构建不一致，无法验证发布包的来源")
        return 1
    return 1
