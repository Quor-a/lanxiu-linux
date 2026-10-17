"""解压/压缩、文件类型识别与默认应用。

两块内容，但它们是一件事的两半：
拿到一个文件，先要知道它是什么（MIME/扩展名），
才知道该用什么打开（默认应用），其中压缩包要先解压。

**为什么扩展名不可信**
用户改个后缀名就能让 `.jpg` 实际是可执行文件。
按扩展名决定用什么打开，等于把类型判断交给文件名。
所以先读文件头（magic bytes）再定类型，扩展名只作兜底。

**为什么解压必须防路径穿越**
恶意压缩包里可以有 `../../etc/passwd` 这样的条目。
不检查直接解压，就能往系统任意位置写文件——
这是压缩软件历史上反复出现的漏洞（zip slip）。
所以每个条目都要校验解压后的真实路径是否在目标目录内。

**为什么要有默认应用**
没有默认应用，双击文件弹出的不是应用而是"选择程序"对话框，
每次都问。而用户 99% 的时候要的是同一个程序。
所以按 MIME 登记默认应用，且必须是"已安装的"——
指向一个不存在的程序，双击会静默失败。
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tarfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class FilesError(RuntimeError):
    pass


# ---------------------------------------------------------------- 类型识别

# 文件头签名。按最长匹配优先排列
MAGIC = [
    (b"PK\x03\x04", "application/zip"),
    (b"PK\x05\x06", "application/zip"),
    (b"PK\x07\x08", "application/zip"),
    (b"7z\xbc\xaf\x27\x1c", "application/x-7z-compressed"),
    (b"Rar!\x1a\x07", "application/x-rar-compressed"),
    (b"\x1f\x8b", "application/gzip"),
    (b"\xfd7zXZ\x00", "application/x-xz"),
    (b"\x04\x22\x4d\x18", "application/x-lz4"),
    (b"(\xb5/\xfd", "application/zstd"),
    (b"BZh", "application/x-bzip2"),
    (b"\x89PNG\r\n\x1a\n", "image/png"),
    (b"\xff\xd8\xff", "image/jpeg"),
    (b"GIF87a", "image/gif"),
    (b"GIF89a", "image/gif"),
    (b"RIFF", "image/webp"),          # 需再确认 WEBP，这里够用
    (b"%PDF", "application/pdf"),
    (b"\x7fELF", "application/x-executable"),
    (b"ustar", "application/x-tar"),
    (b"\x25\x21\x50\x53", "application/postscript"),
    (b"ID3", "audio/mpeg"),
    (b"\xff\xfb", "audio/mpeg"),
    (b"OggS", "audio/ogg"),
    (b"fLaC", "audio/flac"),
    (b"\x00\x00\x00\x20ftyp", "video/mp4"),
    (b"\x1a\x45\xdf\xa3", "video/x-matroska"),
]

# 扩展名兜底：magic 识别不出时用它
EXT_MAP = {
    ".zip": "application/zip", ".7z": "application/x-7z-compressed",
    ".rar": "application/x-rar-compressed", ".gz": "application/gzip",
    ".tgz": "application/gzip", ".xz": "application/x-xz",
    ".zst": "application/zstd", ".bz2": "application/x-bzip2",
    ".tar": "application/x-tar", ".png": "image/png",
    ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".gif": "image/gif",
    ".webp": "image/webp", ".pdf": "application/pdf",
    ".mp3": "audio/mpeg", ".ogg": "audio/ogg", ".flac": "audio/flac",
    ".wav": "audio/wav", ".mp4": "video/mp4", ".mkv": "video/x-matroska",
    ".webm": "video/webm", ".txt": "text/plain", ".md": "text/markdown",
    ".sh": "text/x-shellscript", ".py": "text/x-python",
    ".json": "application/json", ".html": "text/html",
    ".desktop": "application/x-desktop",
}

# 大类别：用于决定"用什么程序打开"
KIND = {
    "image": "image", "video": "video", "audio": "audio", "text": "text",
}


def kind_of(mime: str) -> str:
    head = mime.split("/")[0]
    return KIND.get(head, head)


def sniff(path: Path, head_bytes: int = 4096) -> str:
    """读文件头判断类型。扩展名只作兜底，因为改名就能骗过扩展名判断。"""
    p = Path(path)
    if not p.exists():
        raise FilesError(f"文件不存在: {p}")
    if p.is_dir():
        return "inode/directory"
    try:
        with p.open("rb") as f:
            head = f.read(head_bytes)
    except OSError as e:
        raise FilesError(f"读不了 {p}: {e}")
    for sig, mime in MAGIC:
        if head.startswith(sig):
            return mime
    # tar 的 magic 在偏移 257 处，不在开头
    if len(head) > 262 and head[257:262] == b"ustar":
        return "application/x-tar"
    return EXT_MAP.get(p.suffix.lower(), "application/octet-stream")


def describe(path: Path) -> str:
    mime = sniff(path)
    k = kind_of(mime)
    cn = {"image": "图片", "video": "视频", "audio": "音频",
          "text": "文本", "inode": "目录",
          "application": "应用数据"}.get(k, k)
    return f"{mime}（{cn}）"


# ---------------------------------------------------------------- 解压

ARCHIVE_MIME = {
    "application/zip", "application/x-7z-compressed",
    "application/x-rar-compressed", "application/gzip",
    "application/x-xz", "application/zstd", "application/x-bzip2",
    "application/x-tar",
}


def is_archive(path: Path) -> bool:
    try:
        return sniff(path) in ARCHIVE_MIME
    except FilesError:
        return False


def _safe_target(dest: Path, name: str) -> Path:
    """校验解压目标路径，防路径穿越（zip slip）。

    恶意压缩包里可以有 `../../etc/passwd`。不检查直接解压，
    就能往系统任意位置写文件。
    """
    dest = Path(dest).resolve()
    # 去掉绝对路径前缀，再拼到 dest 下
    rel = name.lstrip("/")
    target = (dest / rel).resolve()
    if not (target == dest or str(target).startswith(str(dest) + os.sep)):
        raise FilesError(f"压缩包条目路径越界，拒绝解压: {name}")
    return target


def extract(path: Path, dest: Path, strip_components: int = 0) -> list:
    """解压到 dest。返回解压出的顶层条目。

    只支持 zip / tar 及其压缩变体（Python 标准库能做的）。
    rar / 7z 走外部命令（p7zip），没装就明确报错——
    静默失败会让人以为"解压成功了但文件没出来"。
    """
    p, dest = Path(path), Path(dest)
    mime = sniff(p)
    dest.mkdir(parents=True, exist_ok=True)
    out: list = []

    if mime == "application/zip":
        with zipfile.ZipFile(p) as z:
            for info in z.infolist():
                # 目录条目也要校验：../ 目录同样能穿越
                _safe_target(dest, info.filename)
            for info in z.infolist():
                if info.is_dir():
                    continue
                t = _safe_target(dest, info.filename)
                t.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, t.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                out.append(info.filename)
        return sorted(out)

    if mime in ("application/x-tar", "application/gzip", "application/x-xz",
                "application/zstd", "application/x-bzip2"):
        # 压缩的 tar 用 "r:*" 自动识别；zstd 需要 Python 3.14+，
        # 老版本退到外部命令
        try:
            mode = "r:*"
            if mime == "application/zstd":
                import sys
                if sys.version_info < (3, 14):
                    return _extract_external(p, dest, ["tar", "--zstd", "-xf",
                                                       str(p), "-C", str(dest)])
            with tarfile.open(p, mode) as t:
                for m in t.getmembers():
                    _safe_target(dest, m.name)
                for m in t.getmembers():
                    if m.isdir():
                        continue
                    rel = _strip(m.name, strip_components)
                    if rel is None:
                        continue
                    t.extract(m, dest)
                    out.append(rel)
            return sorted(out)
        except tarfile.ReadError:
            return _extract_external(p, dest, ["tar", "-xf", str(p),
                                               "-C", str(dest)])

    if mime in ("application/x-7z-compressed", "application/x-rar-compressed"):
        exe = "7z" if mime.endswith("7z-compressed") else "7z"
        if shutil.which(exe) is None:
            raise FilesError(
                f"{mime} 需要 p7zip，未安装。"
                f"执行 qypkg install p7zip 后再试")
        return _extract_external(p, dest, [exe, "x", "-y",
                                           f"-o{dest}", str(p)])
    raise FilesError(f"不支持的格式: {mime}")


def _extract_external(p: Path, dest: Path, cmd: list) -> list:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except FileNotFoundError:
        raise FilesError(f"缺少解压工具: {cmd[0]}")
    if r.returncode != 0:
        raise FilesError(f"解压失败: {r.stderr.strip()[:200]}")
    return sorted(x.name for x in dest.iterdir())


def _strip(name: str, n: int):
    if n <= 0:
        return name
    parts = name.split("/")
    if len(parts) <= n:
        return None
    return "/".join(parts[n:])


def extract_report(path: Path, dest: Path) -> str:
    try:
        items = extract(path, dest)
    except FilesError as e:
        return f"解压失败: {e}"
    n = len(items)
    preview = "、".join(items[:5])
    if n > 5:
        preview += f"…（共 {n} 项）"
    return f"已解压 {n} 项到 {dest}\n  {preview}"


# ---------------------------------------------------------------- 默认应用

@dataclass
class Assoc:
    mime: str
    app: str
    args: str = "%f"


def assoc_path(root: Path) -> Path:
    return Path(root) / "etc" / "qyfiles" / "associations.json"


# 按大类别的默认应用。必须是发行版里真实存在的包提供的程序
DEFAULTS = {
    "image": ("image-viewer", ""),
    "video": ("video-player", ""),
    "audio": ("music-player", ""),
    "text": ("text-editor", ""),
    "application/pdf": ("document-viewer", ""),
    "inode/directory": ("file-manager", ""),
}


def load_assoc(root: Path) -> list:
    p = assoc_path(root)
    if not p.exists():
        return []
    try:
        return [Assoc(**d) for d in json.loads(p.read_text())["assoc"]]
    except Exception:
        return []


def save_assoc(root: Path, items: list) -> None:
    p = assoc_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps(
        {"assoc": [a.__dict__ for a in items]},
        ensure_ascii=False, indent=1).encode())


def set_default(root: Path, mime: str, app: str,
                installed: set | None = None) -> list:
    """设置某类文件的默认应用。

    必须校验程序已安装：指向不存在的程序，双击会静默失败——
    用户点了很多次没反应，完全不知道是配置指错了。
    """
    if installed is not None and app not in installed:
        raise FilesError(
            f"{app} 未安装，不能设为默认应用"
            f"（已装：{'、'.join(sorted(installed)) or '无'}）")
    items = [a for a in load_assoc(root) if a.mime != mime]
    items.append(Assoc(mime, app))
    save_assoc(root, items)
    return items


def default_for(root: Path, mime: str) -> str:
    """查某类文件的默认应用。先看精确 MIME，再看大类别。"""
    for a in load_assoc(root):
        if a.mime == mime:
            return a.app
    k = kind_of(mime)
    for a in load_assoc(root):
        if a.mime == k:
            return a.app
    return DEFAULTS.get(mime, DEFAULTS.get(k, ("", "")))[0]


def open_cmd(root: Path, path: Path, installed: set | None = None) -> str:
    """给出打开某个文件的命令。"""
    mime = sniff(path)
    app = default_for(root, mime)
    if not app:
        raise FilesError(f"{mime} 没有默认应用")
    if installed is not None and app not in installed:
        raise FilesError(
            f"默认应用 {app} 未安装——双击会静默失败，"
            f"这正是最难让用户理解的一类问题")
    return f"{app} {path}"


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyfiles",
                                 description="LANXIU Linux 文件类型与解压")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("type", help="识别文件类型")
    sp.add_argument("path")
    sp = sub.add_parser("extract", help="解压")
    sp.add_argument("path")
    sp.add_argument("dest")
    sp = sub.add_parser("open", help="给出打开命令")
    sp.add_argument("path")
    sp = sub.add_parser("defaults", help="查看默认应用")
    sp = sub.add_parser("set-default", help="设置默认应用")
    sp.add_argument("mime")
    sp.add_argument("app")
    sp = sub.add_parser("trash", help="移入回收站")
    sp.add_argument("path")
    sub.add_parser("trash-list", help="列出回收站")
    sp = sub.add_parser("restore", help="还原")
    sp.add_argument("name")
    sub.add_parser("trash-empty", help="清空回收站")
    sp = sub.add_parser("copy", help="复制命令")
    sp.add_argument("src"); sp.add_argument("dest")
    sp = sub.add_parser("cut", help="移动命令")
    sp.add_argument("src"); sp.add_argument("dest")
    sp = sub.add_parser("paste-check", help="粘贴前检查")
    sp.add_argument("src"); sp.add_argument("dest")
    sp = sub.add_parser("share", help="分享")
    sp.add_argument("path")
    sp.add_argument("--to", default="")
    sp = sub.add_parser("import-check", help="导入前检查")
    sp.add_argument("src"); sp.add_argument("dest")
    sp = sub.add_parser("export", help="导出")
    sp.add_argument("src"); sp.add_argument("out")
    sp.add_argument("--archive", action="store_true")
    sp = sub.add_parser("shortcuts", help="快捷键")
    sp.add_argument("--app-keys", nargs="*", default=[],
                    help="格式 动作=按键")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "type":
            print(describe(Path(a.path)))
            return 0
        if a.cmd == "extract":
            print(extract_report(Path(a.path), Path(a.dest)))
            return 0
        if a.cmd == "open":
            print(open_cmd(root, Path(a.path)))
            return 0
        if a.cmd == "defaults":
            items = load_assoc(root)
            if not items:
                print("没有设置默认应用。")
                return 0
            for x in items:
                print(f"  {x.mime:<28} → {x.app}")
            return 0
        if a.cmd == "set-default":
            set_default(root, a.mime, a.app)
            util.log("ok", f"{a.mime} 默认用 {a.app}")
            return 0
        if a.cmd == "trash":
            r = trash(root, Path(a.path))
            util.log("ok", f"已移入回收站: {r['original']}")
            util.log("info", f"还原：qyfiles restore {r['name']}")
            return 0
        if a.cmd == "trash-list":
            print(trash_report(root))
            return 0
        if a.cmd == "restore":
            d = trash_restore(root, a.name)
            util.log("ok", f"已还原到 {d}")
            return 0
        if a.cmd == "trash-empty":
            n = trash_empty(root)
            util.log("ok", f"已清空 {n} 项（不可恢复）")
            return 0
        if a.cmd == "copy":
            print("\n".join(copy_cmd(Path(a.src), Path(a.dest))))
            return 0
        if a.cmd == "cut":
            print("\n".join(cut_cmd(Path(a.src), Path(a.dest))))
            return 0
        if a.cmd == "paste-check":
            probs = paste_check(Path(a.dest), Path(a.src))
            for x in probs:
                util.log("warn", x)
            if not probs:
                util.log("ok", "可以粘贴")
            return 1 if probs else 0
        if a.cmd == "share":
            print(share_cmd(Path(a.path), a.to))
            return 0
        if a.cmd == "import-check":
            probs = import_check(Path(a.src), Path(a.dest))
            for x in probs:
                util.log("warn", x)
            if not probs:
                util.log("ok", "可以导入")
            return 0
        if a.cmd == "export":
            print("\n".join(export_cmd(Path(a.src), Path(a.out),
                                        a.archive)))
            return 0
        if a.cmd == "shortcuts":
            extra = {}
            for kv in a.app_keys:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    extra[k] = v
            print(shortcut_report(extra))
            return 0
    except FilesError as e:
        util.log("err", str(e))
        return 1
    return 1

# ---------------------------------------------------------------- 回收站

TRASH_DIR = ".qy-trash"


def trash_dir(root: Path) -> Path:
    """回收站目录。

    直接删除是不可逆的。用户误删一次就会失去对系统的信任，
    而 rm 不给你第二次机会。
    """
    return Path(root) / "var" / "lib" / "qytrash"


def trash_meta(root: Path) -> Path:
    return trash_dir(root) / "index.json"


def _load_trash(root: Path) -> list:
    p = trash_meta(root)
    if not p.exists():
        return []
    try:
        return json.loads(p.read_text()).get("items", [])
    except Exception:
        return []


def _save_trash(root: Path, items: list) -> None:
    p = trash_meta(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps({"items": items},
                                    ensure_ascii=False, indent=1).encode())


def trash(root: Path, path: Path) -> dict:
    """移入回收站而不是真删。

    记录原路径是必须的——不知道原来在哪就等于删了，
    用户从回收站里翻出来的东西放不回去，那回收站就形同虚设。
    """
    import time
    src = Path(path)
    if not src.exists():
        raise FilesError(f"不存在: {src}")
    d = trash_dir(root)
    d.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = f"{stamp}-{src.name}"
    dst = d / name
    if dst.exists():
        raise FilesError(f"回收站里已存在同名项 {name}")
    shutil.move(str(src), str(dst))
    items = _load_trash(root)
    items.append({"name": name, "original": str(src),
                  "time": time.strftime("%F %T"),
                  "is_dir": src.is_dir()})
    _save_trash(root, items)
    return {"name": name, "original": str(src)}


def trash_list(root: Path) -> list:
    return _load_trash(root)


def trash_restore(root: Path, name: str) -> Path:
    """还原。目标路径被占用时要报错，不能静默覆盖——
    覆盖掉的是用户后来的新文件，那比删错还糟。"""
    items = _load_trash(root)
    hit = next((x for x in items if x["name"] == name), None)
    if hit is None:
        raise FilesError(f"回收站里没有 {name}")
    src = trash_dir(root) / name
    dst = Path(hit["original"])
    if dst.exists():
        raise FilesError(
            f"原路径 {dst} 已被占用，不能覆盖——"
            f"那可能是你后来新建的文件。请先移开它，"
            f"或手动从回收站取出：{src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dst))
    _save_trash(root, [x for x in items if x["name"] != name])
    return dst


def trash_empty(root: Path) -> int:
    items = _load_trash(root)
    d = trash_dir(root)
    for x in items:
        t = d / x["name"]
        if t.is_dir():
            shutil.rmtree(t, ignore_errors=True)
        elif t.exists():
            t.unlink()
    _save_trash(root, [])
    return len(items)


def trash_report(root: Path) -> str:
    items = _load_trash(root)
    if not items:
        return "回收站为空"
    L = [f"回收站 {len(items)} 项：", ""]
    for x in items[:8]:
        L.append(f"  {x['time']}  {x['original']}")
    if len(items) > 8:
        L.append(f"  …（共 {len(items)} 项）")
    L.append("")
    L.append("  还原：qyfiles restore <名称>")
    L.append("  清空：qyfiles trash-empty（不可恢复）")
    return "\n".join(L)


# ---------------------------------------------------------------- 复制粘贴

def copy_cmd(src: Path, dest: Path) -> list:
    """复制。目录要 -r，且要保留时间戳——
    不保留的话备份出来的文件全是当前时间，
    之后按时间排序或增量备份全乱。"""
    s, d = Path(src), Path(dest)
    if s.is_dir():
        return [f"cp -a {s} {d}   # -a 保留时间戳与权限"]
    return [f"cp -p {s} {d}"]


def cut_cmd(src: Path, dest: Path) -> list:
    return [f"mv {src} {dest}"]


def paste_check(dest: Path, src: Path) -> list:
    """粘贴前的检查。"""
    problems = []
    d, s = Path(dest), Path(src)
    if not s.exists():
        problems.append(f"源 {s} 不存在——可能已被移动或删除")
    if d.exists() and d.is_file():
        problems.append(
            f"目标 {d} 已存在，会被覆盖")
    return problems


# ---------------------------------------------------------------- 分享

def share_cmd(path: Path, target: str = "") -> str:
    """分享给其他应用。

    分享必须走 portal 或临时文件，不能直接给路径：
    直接给路径等于把你的整个文件系统结构暴露给对方，
    且对方能一直访问那个路径。
    """
    p = Path(path)
    if not p.exists():
        raise FilesError(f"不存在: {p}")
    if target:
        return (f"# 走 portal 导出：对方拿到的是副本，"
                f"不是你的原路径\n"
                f"# 直接给路径会把你的目录结构暴露出去，"
                f"且对方之后还能一直访问\n"
                f"qyfiles share-export {p} --to {target}")
    return (f"# 导出到临时副本再分享\n"
            f"cp -a {p} /tmp/share-{p.name} && "
            f"echo /tmp/share-{p.name}")


# ---------------------------------------------------------------- 导入导出

def import_check(src: Path, dest: Path) -> list:
    """导入前检查。"""
    problems = []
    s, d = Path(src), Path(dest)
    if not s.exists():
        problems.append(f"源 {s} 不存在")
        return problems
    if s.resolve() == d.resolve():
        problems.append("源和目标相同——导入到自己身上会覆盖原文件")
    # 导入压缩包要先解压，直接导入压缩包内容会变成一堆看不懂的文件
    try:
        if is_archive(s):
            problems.append(
                f"{s.name} 是压缩包——"
                f"先解压再导入，否则会变成一堆看不懂的文件：\n"
                f"    qyfiles extract {s} <目标目录>")
    except FilesError:
        pass
    return problems


def export_cmd(src: Path, out: Path, archive: bool = False) -> list:
    s, o = Path(src), Path(out)
    if archive:
        return [f"tar -caf {o}.tar.zst -C {s.parent} {s.name}",
                f"# 用 zstd：比 gzip 快且压缩率更高"]
    if s.is_dir():
        return [f"cp -a {s} {o}"]
    return [f"cp -p {s} {o}"]


# ---------------------------------------------------------------- 快捷键

# 常用快捷键。冲突要能查出来——
# 两个动作抢同一个键，按下后执行哪个是不确定的
SHORTCUTS = {
    "screenshot": "Print",
    "screenshot-region": "Shift+Print",
    "record": "Ctrl+Shift+R",
    "file-manager": "Super+E",
    "terminal": "Ctrl+Alt+T",
    "settings": "Super+I",
    "notification-center": "Super+N",
    "search": "Super",
    "lock": "Super+L",
    "switch-window": "Alt+Tab",
    "close-window": "Alt+F4",
    "copy": "Ctrl+C",
    "cut": "Ctrl+X",
    "paste": "Ctrl+V",
    "undo": "Ctrl+Z",
    "select-all": "Ctrl+A",
}


def shortcut_conflicts(extra: dict | None = None) -> list:
    """检查快捷键冲突。extra 是应用额外注册的。"""
    from collections import Counter
    all_k = dict(SHORTCUTS)
    if extra:
        all_k.update(extra)
    cnt = Counter(all_k.values())
    out = []
    for key, n in cnt.items():
        if n > 1:
            who = [k for k, v in all_k.items() if v == key]
            out.append(f"{key} 被 {'、'.join(who)} 同时占用——"
                       f"按下后执行哪个是不确定的")
    return out


def shortcut_report(extra: dict | None = None) -> str:
    L = ["快捷键：", ""]
    for a, k in sorted(SHORTCUTS.items(), key=lambda x: x[1]):
        L.append(f"  {k:<16}{a}")
    if extra:
        L.append("")
        L.append("应用注册：")
        for a, k in sorted(extra.items()):
            L.append(f"  {k:<16}{a}")
    conflicts = shortcut_conflicts(extra)
    if conflicts:
        L.append("")
        L.append("冲突：")
        for c in conflicts:
            L.append(f"  ! {c}")
    return "\n".join(L)
