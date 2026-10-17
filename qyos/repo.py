"""仓库：索引生成、签名、验签、同步。

仓库目录结构：

    repo/
      <arch>/
        index.json         索引（含每个包的元数据摘要与包哈希）
        index.json.sig     ed25519 签名
        *.qyp              包文件
      keys/
        <keyid>.pub        可信公钥

验签规则：索引必须签名；索引里记录了每个包的 sha256，安装时按索引校验包。
"""
from __future__ import annotations

import json
import shutil
from pathlib import Path

from . import format as fmt
from . import util


class RepoError(RuntimeError):
    pass


def _newest_only(entries: list) -> list:
    """同名包只保留最新版本。

    仓库里累积旧版本会让包管理器行为不确定——available() 可能返回旧包，
    升级看起来成功了实际装的还是旧的。产品级仓库的索引必须只暴露当前版本。
    """
    from .deps import version_cmp
    best: dict = {}
    for e in entries:
        cur = best.get(e["name"])
        if cur is None:
            best[e["name"]] = e
            continue
        new_ver, cur_ver = str(e["version"]), str(cur["version"])
        if version_cmp(new_ver, ">", cur_ver) or (
                new_ver == cur_ver and e.get("release", 1) > cur.get("release", 1)):
            best[e["name"]] = e
            util.log("info", f"{e['name']} 保留 {e['pkgid']}，"
                             f"索引隐藏旧版 {cur['pkgid']}")
    return [best[k] for k in sorted(best)]


def obsolete_packages(repo_dir: Path, arch: str = util.ARCH) -> list:
    """找出已被新版取代、可以删除的包文件。"""
    from .deps import version_cmp
    seen: dict = {}
    for p in sorted((repo_dir / arch).glob("*.qyp")):
        pkg = fmt.read_package(p)
        m = pkg.meta
        key = m.name
        cur = seen.get(key)
        if cur is None:
            seen[key] = (p, m.version, m.release)
            continue
        if version_cmp(m.version, ">", cur[1]) or (
                m.version == cur[1] and m.release > cur[2]):
            seen[key] = (p, m.version, m.release)
    keep = {v[0] for v in seen.values()}
    return sorted(p for p in (repo_dir / arch).glob("*.qyp") if p not in keep)


def prune(repo_dir: Path, arch: str = util.ARCH) -> list:
    """删除被取代的旧包文件，释放仓库空间。"""
    old = obsolete_packages(repo_dir, arch)
    for p in old:
        p.unlink()
        util.log("info", f"删除旧包 {p.name}")
    return old


def index_entry(pkg: fmt.Package) -> dict:
    m = pkg.meta
    return {
        "name": m.name, "version": m.version, "release": m.release,
        "arch": m.arch, "pkgid": m.pkgid, "summary": m.summary,
        "depends": m.depends, "provides": m.provides,
        "conflicts": m.conflicts, "replaces": m.replaces,
        "installed_size": m.installed_size,
        "filename": pkg.path.name,
        "sha256": util.sha256_file(pkg.path),
        "size": pkg.path.stat().st_size,
        "file_count": len(m.files),
    }


def build_index(repo_dir: Path, arch: str = util.ARCH) -> dict:
    adir = repo_dir / arch
    adir.mkdir(parents=True, exist_ok=True)
    entries = []
    for p in sorted(adir.glob("*.qyp")):
        pkg = fmt.read_package(p)
        if not pkg.verify():
            util.log("warn", f"跳过损坏的包: {p.name}")
            continue
        entries.append(index_entry(pkg))
    entries = _newest_only(entries)
    index = {
        "format": 1,
        "distro": "LANXIU Linux",
        "arch": arch,
        "generated": int(util.timer()),
        "count": len(entries),
        "packages": entries,
    }
    return index


def write_index(repo_dir: Path, index: dict, arch: str = util.ARCH,
                priv_path: Path | None = None) -> Path:
    adir = repo_dir / arch
    adir.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(index, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode()
    idx_path = adir / "index.json"
    util.atomic_write(idx_path, payload)
    if priv_path and Path(priv_path).exists():
        digest = util.sha256_bytes(payload)
        sig = fmt.sign_digest(bytes.fromhex(digest), Path(priv_path))
        sig_path = idx_path.with_suffix(".json.sig")
        util.atomic_write(sig_path, json.dumps(sig, sort_keys=True).encode())
        # 自检：索引和签名是两个文件，若其中一个没真正落地，
        # 客户端就会读到"新索引 + 旧签名"从而误判仓库被篡改。
        # 这种假警报指向的是安全事件，代价很高，所以这里主动验一遍。
        for _ in range(3):
            try:
                if (idx_path.read_bytes() == payload
                        and sig_path.read_bytes()
                        == json.dumps(sig, sort_keys=True).encode()):
                    break
            except OSError:
                pass
            util.log("warn", "索引/签名写入后回读不一致，重写一次")
            util.atomic_write(idx_path, payload)
            util.atomic_write(sig_path, json.dumps(sig, sort_keys=True).encode())
        util.log("ok", f"索引已签名 ({index['count']} 个包, keyid={sig['keyid']})")
    else:
        # 关键：没有密钥重写索引时，必须同时删掉旧签名。
        # 留着旧签名 = 索引与签名不匹配，下次读取会报"仓库已被篡改"，
        # 而实际上只是这次没签名。这种假警报指向安全事件，代价极高。
        stale = idx_path.with_suffix(".json.sig")
        if stale.exists():
            stale.unlink()
            util.log("warn", "未提供签名密钥，索引未签名；"
                             "已删除过期的旧签名文件，避免误报篡改")
        else:
            util.log("warn", "未提供签名密钥，索引未签名")
    return idx_path


def add_packages(repo_dir: Path, pkgs: list, arch: str = util.ARCH,
                 sign: bool = True) -> list:
    adir = repo_dir / arch
    adir.mkdir(parents=True, exist_ok=True)
    added = []
    for p in pkgs:
        p = Path(p)
        dst = adir / p.name
        if dst.exists() and util.sha256_file(dst) == util.sha256_file(p):
            continue
        shutil.copy2(p, dst)
        added.append(dst)
    return added


def load_index(repo_dir: Path, arch: str = util.ARCH,
               pub_path: Path | None = None, require_sig: bool = True) -> dict:
    idx_path = repo_dir / arch / "index.json"
    if not idx_path.exists():
        raise RepoError(f"仓库索引不存在: {idx_path}")
    payload = idx_path.read_bytes()
    if require_sig:
        sig_path = idx_path.with_suffix(".json.sig")
        if not sig_path.exists():
            raise RepoError("索引未签名；确认识别后可加 --allow-unsigned")
        sig = json.loads(sig_path.read_bytes())
        if pub_path is None:
            raise RepoError("验签需要公钥 (--pubkey)")
        if not fmt.verify_digest(bytes.fromhex(util.sha256_bytes(payload)),
                                 sig, Path(pub_path)):
            raise RepoError("索引签名校验失败！仓库可能已被篡改")
        util.log("ok", f"索引签名有效 (keyid={sig.get('keyid')})")
    return json.loads(payload)


def find_in_index(index: dict, name: str) -> dict | None:
    for e in index["packages"]:
        if e["name"] == name:
            return e
    for e in index["packages"]:
        if name in (e.get("provides") or []):
            return e
    return None


# ---------------------------------------------------------------- 多架构

def architectures(repo_dir: Path) -> list:
    """列出仓库里已有的架构。"""
    rd = Path(repo_dir)
    if not rd.exists():
        return []
    out = []
    for d in sorted(rd.iterdir()):
        if not d.is_dir() or d.name == "keys":
            continue
        if (d / "index.json").exists():
            out.append(d.name)
    return out


def write_manifest(repo_dir: Path, priv_path: Path | None = None) -> Path:
    """生成顶层多架构清单。

    客户端得先知道仓库有哪些架构，否则只能猜或硬编码。
    清单本身也要签名——它能指定从哪里取索引，被篡改等于
    把客户端指向攻击者的仓库。
    """
    rd = Path(repo_dir)
    archs = architectures(rd)
    entries = []
    for a in archs:
        idx = rd / a / "index.json"
        try:
            data = json.loads(idx.read_text())
        except Exception:
            continue
        entries.append({
            "arch": a,
            "count": data.get("count", 0),
            "index": f"{a}/index.json",
            "sha256": util.sha256_file(idx),
        })
    manifest = {
        "format": 1,
        "distro": "LANXIU Linux",
        "generated": int(util.timer()),
        "architectures": [e["arch"] for e in entries],
        "repos": entries,
    }
    payload = json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode()
    path = rd / "manifest.json"
    util.atomic_write(path, payload)
    if priv_path and Path(priv_path).exists():
        sig = fmt.sign_digest(bytes.fromhex(util.sha256_bytes(payload)),
                              Path(priv_path))
        util.atomic_write(path.with_suffix(".json.sig"),
                          json.dumps(sig, sort_keys=True).encode())
    return path


def check_arch_consistency(repo_dir: Path, arch: str) -> list:
    """检查索引声明的架构与包内实际架构是否一致。

    交叉编译下最容易出的错：索引说 aarch64，包里其实是 x86_64。
    客户端不会察觉，装到设备上才报"格式错误"——
    而那时已经刷完机了。
    """
    rd = Path(repo_dir)
    idx = rd / arch / "index.json"
    if not idx.exists():
        return [f"{arch} 没有索引"]
    try:
        data = json.loads(idx.read_text())
    except Exception as e:
        return [f"索引无法解析: {e}"]
    problems = []
    declared = data.get("arch")
    if declared != arch:
        problems.append(f"索引里写的是 {declared}，目录却是 {arch}")
    for e in data.get("packages", []):
        if e.get("arch") != arch:
            problems.append(
                f"{e.get('name')}: 索引记的架构是 {e.get('arch')}，"
                f"应为 {arch}")
        fn = rd / arch / e.get("filename", "")
        if not fn.exists():
            continue
        try:
            pkg = fmt.read_package(fn)
        except Exception:
            continue
        if pkg.meta.arch != arch:
            problems.append(
                f"{fn.name}: 包内架构是 {pkg.meta.arch}，应为 {arch}")
    return problems


def repo_report(repo_dir: Path) -> str:
    """仓库全貌：有几个架构、各多少个包。"""
    rd = Path(repo_dir)
    archs = architectures(rd)
    if not archs:
        return "仓库为空。"
    L = [f"仓库 {rd.name}：{len(archs)} 个架构"]
    total = 0
    for a in archs:
        try:
            data = json.loads((rd / a / "index.json").read_text())
            n = data.get("count", 0)
        except Exception:
            n = 0
        total += n
        L.append(f"  {a:<10} {n} 个包")
    L.append(f"  合计 {total} 个包")
    return "\n".join(L)
