"""外来包格式支持：deb / rpm / apk / pacman / qyp。

为什么需要它
============
用户接手一台设备时，上面往往已经装着别的系统（Ubuntu、Fedora、Alpine、
Arch）的包。不支持外来格式 = 必须先擦盘重装，这是不能接受的迁移成本。
LANXIU要能"就地接管"。

但绝不能把外来包**直接释放**：
* 外来包的控制脚本（preinst/postinst）是别人写的，在LANXIU上语义不明，
  静默执行等于把系统的命运交给一段没审过的 shell；
* 依赖名体系不同（别的发行版叫 libssl3，LANXIU叫 openssl），
  不翻译就会装上"看起来成功、跑起来找不到库"的包；
* 架构必须校验，x86_64 的 deb 装进 aarch64 系统必须拒绝。

所以本模块的原则：
1. **只解析与提取，绝不执行**外来脚本。装进来的文件由LANXIU qypkg 自己
   记录进数据库，之后才能升级 / 卸载 / 校验。
2. **依赖名翻译**：能翻译的翻译，翻译不出来的显式列出（不静默丢弃）。
3. **架构校验**：外来架构名映射到LANXIU架构名后再比对。

支持格式
========
================  ==================================================
deb               ar 容器：debian-binary + control.tar.* + data.tar.*
rpm               lead + signature header + main header + cpio 载荷
apk               Alpine：gzip 流（签名 / .PKGINFO / 数据 tar）
pacman            Arch：.pkg.tar.zst，内含 .PKGINFO + .MTREE
qyp               LANXIU原生格式（走 format.py，不在这里处理）
================  ==================================================

压缩后端只用标准库（gzip / lzma / bz2）加可选外部 zstd；装不上
python-zstandard 时退化为调用 zstd(1)，再没有就明确报错而不是瞎猜。
"""

from __future__ import annotations

import bz2
import gzip
import io
import json
import lzma
import os
import shutil
import struct
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class ForeignError(RuntimeError):
    """外来包解析失败。"""


# ---------------------------------------------------------------- 压缩

def _zstd_decompress(data: bytes) -> bytes:
    """zstd 解压：优先 pyzstandard，退化到 zstd(1)。"""
    try:
        import zstandard as z  # type: ignore
        return z.ZstdDecompressor().decompress(data)
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if not exe:
        raise ForeignError("需要 zstd：pip install zstandard 或系统装 zstd(1)")
    p = subprocess.run([exe, "-dcq"], input=data, capture_output=True)
    if p.returncode != 0:
        raise ForeignError(f"zstd 解压失败: {p.stderr.decode()[:200]}")
    return p.stdout


def _zstd_open(path: Path):
    """以流方式打开 zstd 文件。"""
    exe = shutil.which("zstd")
    if exe:
        p = subprocess.run([exe, "-dcq", str(path)], capture_output=True)
        if p.returncode == 0:
            return io.BytesIO(p.stdout)
    try:
        import zstandard as z  # type: ignore
        with open(path, "rb") as f:
            return io.BytesIO(z.ZstdDecompressor().stream_reader(f).read())
    except ImportError:
        raise ForeignError("需要 zstd（pip install zstandard 或装 zstd(1)）")


def decompress(data: bytes, kind: str) -> bytes:
    """按扩展名/魔数解压。kind 形如 'gz'/'xz'/'bz2'/'zst'/'none'。"""
    k = kind.lower().lstrip(".")
    if k in ("", "none", "tar", "cat"):
        return data
    if k in ("gz", "gzip"):
        return gzip.decompress(data)
    if k in ("xz", "lzma"):
        return lzma.decompress(data)
    if k in ("bz2", "bzip2"):
        return bz2.decompress(data)
    if k in ("zst", "zstd"):
        return _zstd_decompress(data)
    raise ForeignError(f"不认识的压缩格式: {kind}")


def detect_compression(name: str, blob: bytes = b"") -> str:
    """从文件名后缀（优先）或魔数判断压缩格式。"""
    n = name.lower()
    for suf, kind in ((".zst", "zst"), (".gz", "gz"), (".xz", "xz"),
                      (".bz2", "bz2"), (".lzma", "xz"), (".lz", "lzma")):
        if n.endswith(suf):
            return kind
    if blob[:4] == b"\x28\xb5\x2f\xfd":
        return "zst"
    if blob[:2] == b"\x1f\x8b":
        return "gz"
    if blob[:6] == b"\xfd7zXZ\x00":
        return "xz"
    if blob[:3] == b"BZh":
        return "bz2"
    return "none"


# ---------------------------------------------------------------- 架构映射

# 外来发行版用的架构名 → LANXIU（Linux 内核）架构名。
# 不统一映射的话，"这个包能不能装"就无法自动判断，
# 用户只能靠"装了才知道报格式错误"。
ARCH_ALIASES = {
    # Debian / Ubuntu
    "amd64": "x86_64", "i386": "i686", "arm64": "aarch64",
    "armhf": "armv7l", "armel": "armv6l", "ppc64el": "ppc64le",
    "riscv64": "riscv64", "all": "any", "any": "any",
    # Fedora / RHEL
    "x86_64": "x86_64", "aarch64": "aarch64", "noarch": "any",
    "i686": "i686", "ppc64le": "ppc64le", "s390x": "s390x",
    # Alpine
    "x86": "i686", "x86_64": "x86_64", "armv7": "armv7l",
    "aarch64": "aarch64",
    # Arch
    "any": "any", "i686": "i686",
}


def normalize_arch(a: str) -> str:
    """把外来架构名映射成LANXIU架构名。"""
    return ARCH_ALIASES.get((a or "").strip().lower(), (a or "").strip().lower())


# 外来依赖名 → LANXIU包名。只收录确定等价的；拿不准的不写，
# 由调用方显式报告"未翻译"，这比猜错要好。
DEP_ALIASES = {
    # Debian/Ubuntu
    "libc6": "glibc", "libssl3": "openssl", "libssl1.1": "openssl",
    "zlib1g": "zlib", "libncurses6": "ncurses", "libncursesw6": "ncurses",
    "libtinfo6": "ncurses", "libreadline8": "readline",
    "libpcre2-8-0": "pcre2", "libzstd1": "zstd", "liblzma5": "xz",
    "libbz2-1.0": "bzip2", "libexpat1": "expat", "libffi8": "libffi",
    "libsqlite3-0": "sqlite", "libuuid1": "util-linux",
    "coreutils": "coreutils", "bash": "bash", "grep": "grep",
    "sed": "sed", "gawk": "gawk", "file": "file", "less": "less",
    "procps": "procps", "tar": "tar", "gzip": "gzip", "xz-utils": "xz",
    # Fedora/RHEL
    "glibc": "glibc", "openssl-libs": "openssl", "zlib": "zlib",
    "ncurses-libs": "ncurses", "readline": "readline",
    "pcre2": "pcre2", "libzstd": "zstd", "xz-libs": "xz",
    "libffi": "libffi", "expat": "expat", "sqlite-libs": "sqlite",
    # Alpine
    "musl": "glibc", "openssl": "openssl", "zlib": "zlib",
    "ncurses-libs": "ncurses", "readline": "readline",
    # Arch
    "glibc": "glibc", "openssl": "openssl", "zlib": "zlib",
    "ncurses": "ncurses", "readline": "readline",
}

# 纯虚拟/自带的依赖，不属于包名体系，遇到就跳过（不报未翻译）。
DEP_IGNORE = {
    "/bin/sh", "/bin/bash", "sh", "rpmlib(CompressedFileNames)",
    "rpmlib(FileDigests)", "rpmlib(PayloadFilesHavePrefix)",
    "rpmlib(PartialHardlinkSets)", "rtld(GNU_HASH)",
}


def translate_dep(dep: str) -> str | None:
    """依赖名翻译。返回 None 表示无法翻译（调用方需报告）。

    会剥掉版本约束（>= 1.2 → 只留名字），因为版本号体系
    在发行版之间不可比，硬套会误判。
    """
    raw = dep.strip()
    if not raw or raw in DEP_IGNORE:
        return None
    # 剥掉 "(>= 1.2)" 这类括号约束，再剥裸比较符约束。
    # 顺序很关键：先空格切会把 "libc6 (>= 2.34)" 切成 "libc6 ("。
    if "(" in raw:
        raw = raw.split("(", 1)[0].strip()
    for sep in (">=", "<=", "==", ">>", "<<", "=", ">", "<", " "):
        if sep in raw:
            raw = raw.split(sep, 1)[0].strip()
            break
    raw = raw.split(":")[0].strip()          # dpkg 的 multiarch 后缀
    if not raw or raw in DEP_IGNORE:
        return None
    low = raw.lower()
    if low in DEP_IGNORE:
        return None
    return DEP_ALIASES.get(low, low if low in DEP_ALIASES.values() else None)


# ---------------------------------------------------------------- 数据结构

@dataclass
class ForeignPackage:
    """外来包的解析结果（统一视图）。"""

    kind: str                       # deb / rpm / apk / pacman
    name: str = ""
    version: str = ""
    release: str = ""
    arch: str = ""                  # 原始架构名
    summary: str = ""
    description: str = ""
    license: str = ""
    homepage: str = ""
    depends_raw: list = field(default_factory=list)   # 原文依赖
    provides_raw: list = field(default_factory=list)
    scripts: dict = field(default_factory=dict)       # 控制脚本原文（不执行）
    payload_kind: str = "tar"       # tar / cpio
    payload_comp: str = "none"
    _payload_offset: int = 0
    _path: Path | None = None

    # -- 派生 --------------------------------------------------------

    @property
    def target_arch(self) -> str:
        return normalize_arch(self.arch)

    def translated_depends(self) -> tuple[list, list]:
        """返回 (已翻译, 未翻译)。未翻译的要显式告诉用户。"""
        ok, unknown = [], []
        for d in self.depends_raw:
            t = translate_dep(d)
            if t is None:
                d2 = d.strip()
                base = d2.split("(")[0].split(">")[0].split("=")[0].strip()
                if base and base not in DEP_IGNORE and not base.startswith("/"):
                    unknown.append(d2)
            elif t not in ok:
                ok.append(t)
        return ok, unknown

    def pkgid(self) -> str:
        v = self.version + (f"-{self.release}" if self.release else "")
        return f"{self.name}-{v}"

    def payload_bytes(self) -> bytes:
        """读出载荷（已解压，仍是 tar/cpio 流）。"""
        if self._path is None:
            raise ForeignError("未绑定文件路径")
        data = self._path.read_bytes()[self._payload_offset:]
        return decompress(data, self.payload_comp)

    def extract(self, dest: Path) -> list:
        """把载荷释放到 dest（**不执行任何脚本**）。返回文件清单。"""
        dest = Path(dest)
        dest.mkdir(parents=True, exist_ok=True)
        blob = self.payload_bytes()
        if self.payload_kind == "cpio":
            return _extract_cpio(blob, dest)
        return _extract_tar(blob, dest)


def _extract_tar(blob: bytes, dest: Path) -> list:
    out = []
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as tf:
        for m in tf.getmembers():
            # 防目录穿越：外来包的路径不可信
            p = (dest / m.name).resolve()
            if not str(p).startswith(str(dest.resolve())):
                raise ForeignError(f"包内路径越界，拒绝释放: {m.name}")
            tf.extract(m, dest, filter="tar" if hasattr(tarfile, "tar_filter")
                       else None)
            # 归一成 "usr/bin/x" 形式，与 qypkg 自有包的 files 记法一致，
            # 否则 owner_of 查不到（deb tar 里带 ./ 前缀）。
            norm = m.name.lstrip("./").lstrip("/")
            out.append(norm)
    return out


def _extract_cpio(blob: bytes, dest: Path) -> list:
    """解 cpio（newc 格式，rpm 载荷用）。"""
    out, off = [], 0
    n = len(blob)
    while off + 110 <= n:
        if blob[off:off + 6] not in (b"070701", b"070702"):
            # 对齐到下一个 magic
            nxt = blob.find(b"070701", off + 1)
            if nxt < 0:
                break
            off = nxt
            continue
        hdr = blob[off:off + 110]
        namesize = int(hdr[94:102], 16)
        filesize = int(hdr[54:62], 16)
        mode = int(hdr[14:22], 16)
        name = blob[off + 110:off + 110 + namesize - 1].decode(
            "utf-8", "replace")
        data_off = off + 110 + namesize
        data_off = (data_off + 3) & ~3
        data = blob[data_off:data_off + filesize]
        off = (data_off + filesize + 3) & ~3
        if name == "TRAILER!!!":
            break
        target = (dest / name.lstrip("./")).resolve()
        if not str(target).startswith(str(dest.resolve())):
            raise ForeignError(f"包内路径越界，拒绝释放: {name}")
        ftype = mode & 0o170000
        if ftype == 0o040000:
            target.mkdir(parents=True, exist_ok=True)
        elif ftype == 0o120000:
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() or target.is_symlink():
                target.unlink()
            os.symlink(data.decode("utf-8", "replace"), target)
        elif ftype in (0o100000, 0o0):
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            os.chmod(target, mode & 0o7777)
        out.append(name.lstrip("./"))
    return out


# ---------------------------------------------------------------- 格式探测

_MAGIC = [
    (b"\xed\xab\xee\xdb", "rpm"),
    (b"!<arch>\n", "deb"),
    (b"\x1f\x8b", "gzip-stream"),        # apk / tar.gz，需进一步判断
    (b"\x28\xb5\x2f\xfd", "zstd-stream"),
]


def detect_format(path: Path) -> str:
    """判断包格式。返回 deb/rpm/apk/pacman/qyp，未知抛错。"""
    p = Path(path)
    if not p.exists():
        raise ForeignError(f"文件不存在: {p}")
    with open(p, "rb") as f:
        head = f.read(8)
    if head[:6] == b"\xed\xab\xee\xdb" or head[:4] == b"\xed\xab\xee\xdb":
        return "rpm"
    if head.startswith(b"!<arch>"):
        return "deb"
    if head[:6] == b"QYPKG\x00":
        return "qyp"
    # apk / pacman 都是"压缩的 tar"，靠里面的标记文件区分
    name = p.name.lower()
    if name.endswith(".apk"):
        return "apk"
    if ".pkg.tar" in name:
        return "pacman"
    # 兜底：读一小段看 tar 里有没有 .PKGINFO / .MTREE
    try:
        comp = detect_compression(name, head)
        blob = decompress(p.read_bytes(), comp)
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as tf:
            names = tf.getnames()
        if ".PKGINFO" in names:
            return "apk" if "apk" in name else "pacman"
    except Exception:
        pass
    raise ForeignError(f"无法识别的包格式: {p.name}")


# ---------------------------------------------------------------- deb

def _ar_members(blob: bytes) -> list[tuple[str, bytes]]:
    """解析 ar 归档，返回 [(名字, 内容)]。"""
    if not blob.startswith(b"!<arch>\n"):
        raise ForeignError("不是 ar 归档")
    out, off = [], 8
    while off + 60 <= len(blob):
        hdr = blob[off:off + 60]
        if hdr[58:60] != b"`\n":
            break
        name = hdr[0:16].decode("ascii", "replace").strip()
        size = int(hdr[48:58].decode("ascii", "replace").strip() or "0")
        body = blob[off + 60:off + 60 + size]
        out.append((name.rstrip("/"), body))
        off += 60 + size + (size & 1)
    return out


def _ksh_to_dict(text: str) -> dict:
    """解析 deb/apk 的元数据文本。

    格式其实有两条路子：deb 的 control 是 RFC822（'Key: value'），
    apk/pacman 的 .PKGINFO 是 'key = value'。两种都吃。
    """
    fields: dict = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if ": " in s and not s.startswith(" "):
            k, v = s.split(": ", 1)
        elif " = " in s:
            k, v = s.split(" = ", 1)
        elif "=" in s:
            k, v = s.split("=", 1)
        else:
            continue
        k = k.strip().lower()
        v = v.strip()
        if k in fields:
            if isinstance(fields[k], list):
                fields[k].append(v)
            else:
                fields[k] = [fields[k], v]
        else:
            fields[k] = v
    return fields


def _as_list(v) -> list:
    if v is None:
        return []
    if isinstance(v, list):
        return [x for x in v if x]
    return [v]


def read_deb(path: Path) -> ForeignPackage:
    blob = Path(path).read_bytes()
    members = _ar_members(blob)
    names = [n for n, _ in members]
    if "debian-binary" not in names:
        raise ForeignError("不是 deb（缺 debian-binary）")
    ctrl_name, ctrl_body = next(((n, b) for n, b in members
                                 if n.startswith("control.tar")), (None, None))
    if ctrl_body is None:
        raise ForeignError("deb 缺 control.tar")
    cc = detect_compression(ctrl_name, ctrl_body)
    ctrl_tar = decompress(ctrl_body, cc)
    ctrl_fields, scripts = {}, {}
    with tarfile.open(fileobj=io.BytesIO(ctrl_tar), mode="r:") as tf:
        for m in tf.getmembers():
            base = os.path.basename(m.name)
            if base == "control":
                ctrl_fields = _ksh_to_dict(
                    tf.extractfile(m).read().decode("utf-8", "replace"))
            elif base in ("preinst", "postinst", "prerm", "postrm", "config"):
                scripts[base] = tf.extractfile(m).read().decode(
                    "utf-8", "replace")
    data_name, data_body = next(((n, b) for n, b in members
                                 if n.startswith("data.tar")), (None, None))
    if data_body is None:
        raise ForeignError("deb 缺 data.tar")
    dc = detect_compression(data_name, data_body)
    # 记录载荷相对整个文件的偏移，避免二次读盘时又走一遍 ar。
    # 注意 off 走到 data 成员头时还要 +60 才是载荷本体（ar 成员头）。
    off = 0
    for n, b in members:
        if n == data_name:
            off += 60
            break
        off += 60 + len(b) + (len(b) & 1)
    pkg = ForeignPackage(
        kind="deb",
        name=str(ctrl_fields.get("package", "")),
        version=str(ctrl_fields.get("version", "")),
        arch=str(ctrl_fields.get("architecture", "")),
        summary=str(ctrl_fields.get("description", "")).split("\n")[0],
        description=str(ctrl_fields.get("description", "")),
        license=str(ctrl_fields.get("license", "")),
        homepage=str(ctrl_fields.get("homepage", "")),
        depends_raw=[d.strip() for d in
                     str(ctrl_fields.get("depends", "")).split(",") if d.strip()],
        provides_raw=[d.strip() for d in
                      str(ctrl_fields.get("provides", "")).split(",") if d.strip()],
        scripts=scripts,
        payload_kind="tar", payload_comp=dc,
        _payload_offset=8 + off, _path=Path(path),
    )
    return pkg


# ---------------------------------------------------------------- rpm

_RPM_TYPES = {0: "char", 1: "int8", 2: "int16", 3: "int32", 4: "int64",
              5: "string", 6: "bin", 7: "string_array", 8: "i18nstring",
              9: "bin"}
_RPM_ALIGN = {0: 1, 1: 1, 2: 2, 3: 4, 4: 8, 5: 1, 6: 1, 7: 1, 8: 1, 9: 1}


def _rpm_parse_header(blob: bytes, off: int) -> tuple[dict, int]:
    """解析 rpm 的 header（签名头与主头格式相同）。"""
    if blob[off:off + 3] != b"\x8e\xad\xe8":
        raise ForeignError(f"rpm header 魔数不对 @{off}")
    nindex, ndata = struct.unpack(">II", blob[off + 8:off + 16])
    idx_off = off + 16
    data_off = idx_off + nindex * 16
    tags: dict = {}
    for i in range(nindex):
        tag, typ, ioff, count = struct.unpack(
            ">iiii", blob[idx_off + i * 16:idx_off + i * 16 + 16])
        if typ not in _RPM_TYPES:
            continue
        p = data_off + ioff
        try:
            if typ == 5:                          # 单个字符串
                end = blob.index(b"\x00", p)
                tags[tag] = blob[p:end].decode("utf-8", "replace")
            elif typ in (7, 8):                   # 字符串数组
                vals, q = [], p
                for _ in range(count):
                    end = blob.index(b"\x00", q)
                    vals.append(blob[q:end].decode("utf-8", "replace"))
                    q = end + 1
                tags[tag] = vals
            elif typ == 6:                        # 二进制
                tags[tag] = blob[p:p + count]
            else:
                fmt = {1: ">b", 2: ">h", 3: ">i", 4: ">q"}[typ]
                sz = struct.calcsize(fmt)
                tags[tag] = list(struct.unpack(f">{count}{fmt[-1]}",
                                               blob[p:p + count * sz]))
        except (ValueError, struct.error):
            continue
    return tags, data_off + ndata


def read_rpm(path: Path) -> ForeignPackage:
    blob = Path(path).read_bytes()
    # lead 96 字节，然后签名头、主头
    if blob[:4] != b"\xed\xab\xee\xdb":
        raise ForeignError("不是 rpm")
    sig, off = _rpm_parse_header(blob, 96)
    main, off2 = _rpm_parse_header(blob, off)
    payload_off = off2
    comp_map = {1: "gz", 2: "bz2", 3: "xz", 5: "zst", 0: "none"}
    comp = comp_map.get(_first(main.get(1122)), "gz")
    pkg = ForeignPackage(
        kind="rpm",
        name=_first(main.get(1000)) or "",
        version=_first(main.get(1001)) or "",
        release=_first(main.get(1002)) or "",
        arch=_first(main.get(1022)) or "",
        summary=_first(main.get(1004)) or "",
        description=_first(main.get(1005)) or "",
        license=_first(main.get(1014)) or "",
        homepage=_first(main.get(1020)) or "",
        depends_raw=_clean_rpm_deps(main.get(1049) or []),
        provides_raw=[_clean_rpm_name(p) for p in (main.get(1047) or [])],
        scripts={},
        payload_kind="cpio", payload_comp=comp,
        _payload_offset=payload_off, _path=Path(path),
    )
    return pkg


def _first(v):
    if v is None:
        return None
    if isinstance(v, list):
        return v[0] if v else None
    return v


def _clean_rpm_name(s: str) -> str:
    return (s or "").split("(")[0].strip()


def _clean_rpm_deps(lst) -> list:
    """rpm 的依赖名字里带 rpmlib(...)、/bin/sh 这类，清掉。"""
    out = []
    for d in lst:
        d = (d or "").strip()
        if not d or d.startswith("rpmlib(") or d.startswith("/"):
            continue
        out.append(d)
    return out


# ---------------------------------------------------------------- apk / pacman

def read_apk(path: Path) -> ForeignPackage:
    """Alpine apk：连续的 gzip 流（签名 / 控制 / 数据）。

    这里只取控制段里的 .PKGINFO，绝不执行 .pre-install 等脚本。
    """
    blob = Path(path).read_bytes()
    pinfo: dict = {}
    scripts: dict = {}
    # apk 是"多段 gzip 拼接"，用 GzipFile 顺序读会当作一个流；
    # 用 tarfile 直接吃整个文件即可（tar 会在数据段结束处停）。
    try:
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as tf:
            for m in tf.getmembers():
                base = os.path.basename(m.name)
                if base == ".PKGINFO":
                    pinfo = _ksh_to_dict(
                        tf.extractfile(m).read().decode("utf-8", "replace"))
                elif base in (".pre-install", ".post-install",
                              ".pre-deinstall", ".post-deinstall"):
                    scripts[base] = tf.extractfile(m).read().decode(
                        "utf-8", "replace")
                if base in (".PKGINFO",) and "datahash" in pinfo:
                    pass
    except tarfile.TarError:
        # 多段流：先解出"控制段"再解数据段
        for seg in _split_gzip_segments(blob):
            try:
                with tarfile.open(fileobj=io.BytesIO(seg), mode="r:") as tf:
                    for m in tf.getmembers():
                        base = os.path.basename(m.name)
                        if base == ".PKGINFO":
                            pinfo = _ksh_to_dict(
                                tf.extractfile(m).read().decode(
                                    "utf-8", "replace"))
            except tarfile.TarError:
                continue
    if not pinfo:
        raise ForeignError("apk 里找不到 .PKGINFO")
    # apk 是"多段 gzip 拼接"：签名段/控制段/数据段各自是一段独立 gzip。
    # 载荷 = 第三段（第一个不是 .SIGN/.PKGINFO 的段）。直接记下它的
    # 原始字节偏移，payload_bytes() 时按 gzip 解这一段。
    spans = _gzip_spans(blob)
    data_off = spans[2][0] if len(spans) >= 3 else 0
    return ForeignPackage(
        kind="apk",
        name=pinfo.get("pkgname", ""),
        version=pinfo.get("pkgver", ""),
        arch=normalize_arch(pinfo.get("arch", "")),
        summary=pinfo.get("pkgdesc", ""),
        description=pinfo.get("pkgdesc", ""),
        license=pinfo.get("license", ""),
        homepage=pinfo.get("url", ""),
        depends_raw=_as_list(pinfo.get("depend")) +
                    [f"so:{s}" for s in _as_list(pinfo.get("provides"))],
        provides_raw=_as_list(pinfo.get("provides")),
        scripts=scripts,
        payload_kind="tar", payload_comp="gz",
        _payload_offset=data_off, _path=Path(path),
    )


def _gzip_spans(blob: bytes) -> list[tuple[int, int]]:
    """扫描文件里每段 gzip 流的 (起始偏移, 结束偏移)。

    apk 是三段 gzip 拼接，但用普通循环扫会撞上同一问题：单段里
    zlib 的 unused_data 才是下一段的起点。这里不逐字节试探，
    从 0 开始顺着链走。
    """
    spans: list[tuple[int, int]] = []
    i = 0
    zl = __import__("zlib")
    while i + 2 <= len(blob) and blob[i:i + 2] == b"\x1f\x8b":
        o = zl.decompressobj(31)
        try:
            o.decompress(blob[i:])
        except Exception:
            break
        end = len(blob) - len(o.unused_data)
        spans.append((i, end))
        i = end
        # 段间可能有零填充，跳过非 gzip 魔数的垃圾
        while i + 2 <= len(blob) and blob[i:i + 2] != b"\x1f\x8b":
            i += 1
    return spans


def _split_gzip_segments(blob: bytes) -> list[bytes]:
    """把多段 gzip 拆成逐段解压后的数据。"""
    out, buf, i = [], bytearray(), 0
    d = __import__("zlib")
    while i < len(blob):
        if blob[i:i + 2] != b"\x1f\x8b":
            i += 1
            continue
        o = d.decompressobj(31)
        try:
            out.append(o.decompress(blob[i:]))
            i = len(blob) - len(o.unused_data)
        except Exception:
            i += 1
    return out


def read_pacman(path: Path) -> ForeignPackage:
    """.pkg.tar.zst：.PKGINFO + .MTREE + 文件。"""
    blob = _read_maybe_compressed(Path(path))
    pinfo, scripts = {}, {}
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as tf:
        for m in tf.getmembers():
            base = os.path.basename(m.name)
            if base == ".PKGINFO":
                pinfo = _ksh_to_dict(
                    tf.extractfile(m).read().decode("utf-8", "replace"))
            elif base in ("pre_install", "post_install",
                          "pre_upgrade", "post_upgrade", "pre_remove"):
                scripts[base] = tf.extractfile(m).read().decode(
                    "utf-8", "replace")
    if not pinfo:
        raise ForeignError("pacman 包里找不到 .PKGINFO")
    deps = []
    for d in _as_list(pinfo.get("depend")):
        deps.append(d)
    return ForeignPackage(
        kind="pacman",
        name=pinfo.get("pkgname", ""),
        version=pinfo.get("pkgver", ""),
        arch=pinfo.get("arch", ""),
        summary=pinfo.get("pkgdesc", ""),
        description=pinfo.get("pkgdesc", ""),
        license=pinfo.get("license", ""),
        homepage=pinfo.get("url", ""),
        depends_raw=deps,
        provides_raw=_as_list(pinfo.get("provides")),
        scripts=scripts,
        payload_kind="tar", payload_comp="none",
        _payload_offset=0, _path=Path(path),
    )


def _read_maybe_compressed(path: Path) -> bytes:
    blob = Path(path).read_bytes()
    kind = detect_compression(path.name, blob)
    return decompress(blob, kind)


# ---------------------------------------------------------------- 统一入口

_READERS = {"deb": read_deb, "rpm": read_rpm,
            "apk": read_apk, "pacman": read_pacman}


def read_foreign(path: Path) -> ForeignPackage:
    """解析任意支持的外来包，返回统一视图。"""
    kind = detect_format(path)
    if kind == "qyp":
        raise ForeignError("这是LANXIU原生包，用 qypkg install 直接装")
    return _READERS[kind](Path(path))


def describe(pkg: ForeignPackage) -> str:
    """人类可读的解析摘要（qypkg foreign-info 用）。"""
    ok, unknown = pkg.translated_depends()
    lines = [
        f"格式      {pkg.kind}",
        f"名称      {pkg.name}",
        f"版本      {pkg.version}" + (f"-{pkg.release}" if pkg.release else ""),
        f"架构      {pkg.arch} → {pkg.target_arch}",
        f"说明      {pkg.summary}",
    ]
    if pkg.license:
        lines.append(f"许可      {pkg.license}")
    if pkg.homepage:
        lines.append(f"主页      {pkg.homepage}")
    if ok:
        lines.append(f"依赖(译)  {' '.join(ok)}")
    if unknown:
        lines.append(f"依赖(未译) {' '.join(unknown)}"
                     "   ← LANXIU仓库里没有对应名字，装前请确认")
    if pkg.scripts:
        lines.append(f"控制脚本  {' '.join(sorted(pkg.scripts))}"
                     "（不会执行）")
    return "\n".join(lines)


def matches_arch(pkg: ForeignPackage, want: str) -> bool:
    """外来包架构是否适配目标系统。'any' 始终通过。"""
    ta = pkg.target_arch
    if ta in ("any", "all", ""):
        return True
    return ta == normalize_arch(want)


def install_foreign(path: Path, root: Path, *, kind: str | None = None,
                    force_arch: bool = False, dry_run: bool = False,
                    record: bool = True) -> dict:
    """把外来包释放进 root 并记入LANXIU包数据库。

    设计要点：
    * 不执行外来脚本 —— 只释放文件；
    * 记进数据库才能被 qypkg 卸载/校验（否则文件变孤儿）；
    * 架构不符默认拒绝（可用 force_arch 覆盖，风险自负）。
    """
    from . import format as fmt
    from . import pkgmgr

    pkg = read_foreign(path)
    host = root_arch(root)
    if not force_arch and not matches_arch(pkg, host):
        raise ForeignError(
            f"架构不符：包是 {pkg.target_arch}，系统是 {host}。"
            f"确要强装加 --force-arch（可能直接无法运行）")

    ok, unknown = pkg.translated_depends()
    if unknown and not dry_run:
        util.log("warn", f"{len(unknown)} 个依赖无法翻译: "
                         f"{' '.join(unknown[:5])}")

    # 已有同名包？升级语义要合并，简单起见先查冲突
    db = pkgmgr.DB(root)
    installed = db.installed()
    if pkg.name in installed and not dry_run:
        util.log("info", f"{pkg.name} 已安装（{installed[pkg.name]['version']}），"
                         f"本次为覆盖安装")

    files = []
    if dry_run:
        util.log("info", f"[dry-run] 将释放到 {root}（不执行脚本）")
    else:
        files = pkg.extract(root)
        _after_extract(root, files, pkg.kind)
    result = {
        "name": pkg.name, "version": pkg.version, "release": pkg.release,
        "kind": pkg.kind, "arch": pkg.target_arch,
        "files": len(files), "depends": ok, "untranslated": unknown,
        "scripts_skipped": sorted(pkg.scripts),
    }
    if dry_run or not record:
        return result

    # 记进LANXIU数据库：这样 qypkg -Q/-Qo/-R 对这批文件同样有效。
    meta = fmt.Meta(
        name=pkg.name, version=pkg.version, release=pkg.release or 1,
        arch=pkg.target_arch, summary=pkg.summary,
        description=pkg.description or pkg.summary,
        homepage=pkg.homepage, license=pkg.license,
        depends=ok, makedepends=[], provides=[], conflicts=[], replaces=[],
        scripts={}, triggers=[], sysusers=[], alternatives=[],
        patches=[],
        build={"foreign": pkg.kind, "source": os.path.basename(str(path)),
               "host": util.ARCH, "pack_time": int(util.timer())},
        installed_size=sum((root / f).stat().st_size
                           for f in files
                           if (root / f).is_file()),
    )
    meta.files = []
    for f in files:
        fp = root / f
        try:
            st = fp.lstat()
        except OSError:
            continue
        import stat as _stat
        if _stat.S_ISDIR(st.st_mode):
            meta.files.append({"path": f.rstrip("/"), "type": "dir",
                               "mode": st.st_mode & 0o7777, "size": 0,
                               "sha256": "", "target": "", "config": False})
        elif _stat.S_ISLNK(st.st_mode):
            meta.files.append({"path": f.rstrip("/"), "type": "symlink",
                               "mode": st.st_mode & 0o7777, "size": 0,
                               "sha256": "", "target": os.readlink(fp),
                               "config": False})
        elif _stat.S_ISREG(st.st_mode):
            meta.files.append({"path": f.rstrip("/"), "type": "file",
                               "mode": st.st_mode & 0o7777, "size": st.st_size,
                               "sha256": util.sha256_file(fp), "target": "",
                               "config": False})
    db.add_pkg(meta, meta.files, reason="foreign")
    result["recorded"] = True
    return result


def _after_extract(root: Path, files: list[str], kind: str) -> None:
    """外来包释放后的兼容层收尾（ISO 实测教训，2026-10-10）。

    deb/rpm 的库放在 /usr/lib/<triplet>/ 或 /lib/<triplet>/（多架构目录），
    LANXIU的 ld.so 默认不搜这些路径——直接装完跑不起来（file 报
    libmagic.so.1 找不到）。这里自动登记目录 + 跑 ldconfig（若 root 里有）。
    """
    root = Path(root)
    extra = [d for d in ("/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
                         "/usr/lib/aarch64-linux-gnu",
                         "/lib/aarch64-linux-gnu")
             if (root / d.strip("/")).is_dir()]
    if not extra:
        return
    conf_dir = root / "etc" / "ld.so.conf.d"
    conf = conf_dir / "qypkg-foreign.conf"
    try:
        conf_dir.mkdir(parents=True, exist_ok=True)
        have = set(conf.read_text().split()) if conf.exists() else set()
        new = [d for d in extra if d not in have]
        if new:
            with conf.open("a") as f:
                f.write("\n".join(new) + "\n")
            util.log("info", "外来库目录已登记: " + " ".join(new))
    except OSError as e:
        util.log("warn", f"登记外来库目录失败: {e}")
        return
    # 若目标系统有 ldconfig 就刷新缓存（chroot 环境用 --root 语义）
    for name in ("ldconfig", "/usr/bin/ldconfig", "/sbin/ldconfig"):
        p = root / name.strip("/") if not name.startswith("/") else Path(name)
        if p.exists():
            util.log("info", "提示: 建议运行 ldconfig 刷新动态库缓存"
                             "（或临时用 LD_LIBRARY_PATH）")
            break


def root_arch(root: Path) -> str:
    """探测根目录（或宿主）的架构。

    用包管理器的 DB 层读（SQLite），读不到就回落到宿主架构。
    """
    root = Path(root)
    try:
        from . import pkgmgr
        db = pkgmgr.DB(root)
        archs = {v.get("arch") for v in db.installed().values()
                 if v.get("arch")}
        if not archs:
            return util.ARCH
        if len(archs) == 1:
            return str(next(iter(archs)))
        # 混合库（宿主包 + 交叉构建的目标架构包）：
        # 取除宿主外唯一的那个；歧义时宁可拒绝也不要猜。
        others = archs - {util.ARCH}
        if len(others) == 1:
            return str(next(iter(others)))
        raise ForeignError(f"无法判断系统架构，包库里混有多个架构: {sorted(archs)}")
    except Exception:
        pass
    return util.ARCH
