"""overlayfs 分层——就地接管其他系统的根。

设计动机
========
用户机器上往往已经装着 Ubuntu/Debian/Fedora。传统做法是重装或 chroot，
两者都要动原系统。这个模块给出的第三条路：

    原系统根 = overlayfs 只读下层（lowerdir）
    LANXIU包   = 写入上层（upperdir）
    合成视图 = merged 目录

原系统一个字节都不改；LANXIU qypkg 的 --root 指向 merged，装进来的包、
foreign.py 适配的 deb/apk 都落上层；不要了就 umount——上层目录一删，
机器回到接管前状态。这是"支持其他系统包"的安全底座。

关键取舍：
* 只挂不查不行——挂之前必须确认 lowerdir 看起来像个 Linux 根（有 /usr
  或 /etc），否则用户手滑把 /home 当根挂上来，后续操作全错位。
* workdir 必须和 upperdir 同一文件系统，且必须为空——内核硬性要求。
* 权限：mount 需要 root。非 root 时给出明确错误而不是半途炸。
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import util


class LayerError(Exception):
    pass


@dataclass
class LayerMount:
    """一次 overlay 挂载的全部参数（便于挂/卸/巡检）。"""
    lower: Path          # 原系统根（只读）
    upper: Path          # LANXIU写入层
    work: Path           # overlayfs 内部工作目录
    merged: Path         # 合成视图（qypkg --root 指这里）

    def options(self) -> str:
        return (f"lowerdir={self.lower},upperdir={self.upper},"
                f"workdir={self.work}")

    def to_json(self) -> dict:
        return {"lower": str(self.lower), "upper": str(self.upper),
                "work": str(self.work), "merged": str(self.merged)}


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True)


def looks_like_root(path: Path) -> tuple[bool, str]:
    """lowerdir 看起来像 Linux 根吗？防止把 /home 之类挂成系统层。"""
    path = Path(path)
    if not path.is_dir():
        return False, f"{path} 不存在或不是目录"
    has_usr = (path / "usr").is_dir()
    has_etc = (path / "etc").is_dir()
    if not (has_usr or has_etc):
        return False, (f"{path} 里没有 usr/ 或 etc/，"
                       "不像 Linux 根——确认没指错目录")
    return True, ""


def create_layer(base: Path, name: str,
                 state_root: Path | None = None) -> LayerMount:
    """建一套分层目录并挂载。返回 LayerMount。

    目录布局（默认 state_root=/var/lib/qypkg/layers）：
        <state_root>/<name>/
            upper/    LANXIU写的东西全在这
            work/     overlayfs 内部用
            merged/   合成视图，qypkg --root 指这里
            meta.json 挂载参数（rollback/巡检要读）
    base 必须像 Linux 根；重复 create 同名层会先卸旧的再挂新的（幂等）。
    """
    base = Path(base).resolve()
    ok, why = looks_like_root(base)
    if not ok:
        raise LayerError(why)

    if os.geteuid() != 0:
        raise LayerError("挂载 overlayfs 需要 root（sudo qypkg ...）。"
                         "非 root 环境请用 --dry-run 只生成目录布局")

    state_root = Path(state_root or "/var/lib/qypkg/layers")
    layer_root = state_root / name
    upper = layer_root / "upper"
    work = layer_root / "work"
    merged = layer_root / "merged"
    for d in (upper, work, merged):
        d.mkdir(parents=True, exist_ok=True)
    # 内核要求 workdir 为空
    for p in work.iterdir():
        if p.is_dir() and not p.is_symlink():
            shutil.rmtree(p)
        else:
            p.unlink()

    m = LayerMount(lower=base, upper=upper, work=work, merged=merged)
    r = _run(["mount", "-t", "overlay", "overlay",
              "-o", m.options(), str(merged)])
    if r.returncode != 0:
        raise LayerError(f"overlay 挂载失败: {r.stderr.strip()}")
    # 记录挂载参数，qypkg layers 子命令和回滚要用
    import json
    meta = layer_root / "meta.json"
    meta.write_text(json.dumps({**m.to_json(), "name": name,
                                "base": str(base),
                                "created": int(util.timer())},
                               ensure_ascii=False, indent=1))
    return m


def destroy_layer(name: str, state_root: Path | None = None,
                  keep_upper: bool = False) -> None:
    """卸载并清理。keep_upper=True 保留写入层（升级/迁移时用）。"""
    state_root = Path(state_root or "/var/lib/qypkg/layers")
    layer_root = state_root / name
    merged = layer_root / "merged"
    if merged.is_mount() if hasattr(merged, "is_mount") else _is_mount(merged):
        r = _run(["umount", str(merged)])
        if r.returncode != 0:
            raise LayerError(f"卸载失败: {r.stderr.strip()}")
    if not keep_upper:
        shutil.rmtree(layer_root, ignore_errors=True)


def _is_mount(path: Path) -> bool:
    try:
        return os.path.ismount(path)
    except OSError:
        return False


def list_layers(state_root: Path | None = None) -> list[dict]:
    """列出所有层及其挂载状态。"""
    state_root = Path(state_root or "/var/lib/qypkg/layers")
    import json
    out = []
    if not state_root.is_dir():
        return out
    for d in sorted(state_root.iterdir()):
        meta_f = d / "meta.json"
        if not meta_f.is_file():
            continue
        try:
            meta = json.loads(meta_f.read_text())
        except Exception:
            continue
        merged = Path(meta.get("merged", d / "merged"))
        meta["mounted"] = _is_mount(merged)
        out.append(meta)
    return out


def takeover_snapshot(base: Path) -> dict:
    """接管前的基线盘点——记录原系统发行版和关键状态。

    有了它，卸层时才能告诉用户"你回到的是什么"。
    """
    base = Path(base)
    info: dict = {"base": str(base)}
    os_release = base / "etc" / "os-release"
    if os_release.is_file():
        for line in os_release.read_text(errors="replace").splitlines():
            if line.startswith("PRETTY_NAME="):
                info["distro"] = line.split("=", 1)[1].strip('"')
                break
    dpkg_db = base / "var" / "lib" / "dpkg" / "status"
    rpm_db = base / "var" / "lib" / "rpm"
    apk_db = base / "etc" / "apk" / "world"
    if dpkg_db.is_file():
        info["package_system"] = "dpkg"
    elif rpm_db.is_dir():
        info["package_system"] = "rpm"
    elif apk_db.is_file():
        info["package_system"] = "apk"
    return info
