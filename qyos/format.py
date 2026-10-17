"""LANXIU Linux 包格式 —— .qyp

单文件三段式容器，便于分发、验签与元数据快速查询：

    ┌──────────────────────────────────────────────┐
    │ header  128B  魔数/版本/段偏移/段长度/段哈希   │
    ├──────────────────────────────────────────────┤
    │ meta    JSON  包名版本依赖/文件清单/安装脚本   │
    ├──────────────────────────────────────────────┤
    │ data    tar.gz 根文件系统相对路径的数据         │
    ├──────────────────────────────────────────────┤
    │ sig     JSON  ed25519 对 (meta+data) 哈希签名  │
    └──────────────────────────────────────────────┘

签名段可分离：仓库对包的签名也可以放在仓库索引里，包内 sig 为空。
"""
from __future__ import annotations

import base64
import io
import json
import os
import struct
import tarfile
from dataclasses import dataclass, field, asdict
from pathlib import Path

from . import util

MAGIC = b"QYPKG\x00\x01"
FORMAT_VERSION = 1
HEADER_FMT = "<8sII6Q32s32s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 128


class PackageError(RuntimeError):
    pass


# ---------------------------------------------------------------- 元数据

@dataclass
class FileEntry:
    path: str          # 相对根，如 "usr/bin/foo"
    sha256: str = ""
    size: int = 0
    mode: int = 0o644
    type: str = "file"  # file / dir / symlink
    target: str = ""    # 符号链接目标
    # 是否是配置文件。/etc 下的文件用户会改，升级时不能无条件覆盖——
    # 去掉这个标记，每次升级都会毁掉用户改过的配置
    config: bool = False


@dataclass
class Meta:
    name: str
    version: str
    release: int = 1
    arch: str = util.ARCH
    summary: str = ""
    description: str = ""
    homepage: str = ""
    license: str = ""
    depends: list = field(default_factory=list)
    makedepends: list = field(default_factory=list)
    provides: list = field(default_factory=list)
    conflicts: list = field(default_factory=list)
    replaces: list = field(default_factory=list)
    files: list = field(default_factory=list)
    scripts: dict = field(default_factory=dict)
    triggers: list = field(default_factory=list)
    sysusers: list = field(default_factory=list)
    alternatives: list = field(default_factory=list)
    build: dict = field(default_factory=dict)
    patches: list = field(default_factory=list)
    installed_size: int = 0

    @property
    def pkgid(self) -> str:
        return f"{self.name}-{self.version}-{self.release}"

    @property
    def evr(self) -> str:
        return f"{self.version}-{self.release}"

    def to_json(self) -> bytes:
        d = asdict(self)
        d["files"] = [asdict(f) if isinstance(f, FileEntry) else f for f in self.files]
        return json.dumps(d, ensure_ascii=False, sort_keys=True,
                          separators=(",", ":")).encode("utf-8")

    @staticmethod
    def from_json(data: bytes) -> "Meta":
        d = json.loads(data.decode("utf-8"))
        d["files"] = [FileEntry(**f) for f in d.get("files", [])]
        return Meta(**d)


# ---------------------------------------------------------------- 签名

def gen_keypair(priv_path: Path) -> str:
    """生成 ed25519 密钥对，返回公钥指纹（keyid）。"""
    from cryptography.hazmat.primitives.asymmetric import ed25519
    priv = ed25519.Ed25519PrivateKey.generate()
    pub = priv.public_key()
    from cryptography.hazmat.primitives import serialization as ser
    priv_path.parent.mkdir(parents=True, exist_ok=True)
    priv_path.write_bytes(priv.private_bytes(
        ser.Encoding.PEM, ser.PrivateFormat.PKCS8, ser.NoEncryption()))
    pub_path = priv_path.with_suffix(priv_path.suffix + ".pub")
    pub_path.write_bytes(pub.public_bytes(
        ser.Encoding.PEM, ser.PublicFormat.SubjectPublicKeyInfo))
    os.chmod(priv_path, 0o600)
    keyid = util.sha256_bytes(pub.public_bytes(
        ser.Encoding.Raw, ser.PublicFormat.Raw))[:16]
    return keyid


def _load_priv(priv_path: Path):
    from cryptography.hazmat.primitives import serialization as ser
    return ser.load_pem_private_key(priv_path.read_bytes(), password=None)


def _load_pub(pub_path: Path):
    from cryptography.hazmat.primitives import serialization as ser
    return ser.load_pem_public_key(pub_path.read_bytes())


def sign_digest(digest: bytes, priv_path: Path) -> dict:
    """对 64 字节 (meta_hash+data_hash) 签名。"""
    priv = _load_priv(priv_path)
    raw = priv.sign(digest)
    from cryptography.hazmat.primitives import serialization as ser
    pub = priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    return {"alg": "ed25519",
            "keyid": util.sha256_bytes(pub)[:16],
            "sig": base64.b64encode(raw).decode()}


def verify_digest(digest: bytes, sig: dict, pub_path: Path) -> bool:
    try:
        _load_pub(pub_path).verify(base64.b64decode(sig["sig"]), digest)
        return True
    except Exception:
        return False


# ---------------------------------------------------------------- 打包

def scan_destdir(destdir: Path, config_files: list | None = None) -> list:
    """扫描安装目录，生成文件清单（跳过构建残留）。

    config_files: 需要标记为配置文件的路径前缀（通常是 etc/ 下的）。
    """
    entries = []
    cfgs = [str(c).lstrip("/") for c in (config_files or [])]
    for dirpath, dirnames, filenames in os.walk(destdir):
        rel_dir = Path(dirpath).relative_to(destdir)
        dirnames.sort()
        filenames.sort()
        for d in dirnames:
            p = rel_dir / d
            full = destdir / p
            if full.is_symlink():
                entries.append(FileEntry(str(p), type="symlink",
                                         target=os.readlink(full)))
                dirnames.remove(d)
            else:
                entries.append(FileEntry(str(p), type="dir",
                                         mode=full.stat().st_mode & 0o7777))
        for f in filenames:
            p = rel_dir / f
            full = destdir / p
            if full.is_symlink():
                entries.append(FileEntry(str(p), type="symlink",
                                         target=os.readlink(full)))
                continue
            st = full.lstat()
            rel = str(p)
            is_cfg = any(rel == c or rel.startswith(c.rstrip("/") + "/")
                         for c in cfgs)
            entries.append(FileEntry(rel, sha256=util.sha256_file(full),
                                     size=st.st_size,
                                     mode=st.st_mode & 0o7777,
                                     config=is_cfg))
    return entries


def build_package(meta: Meta, destdir: Path, out_path: Path,
                  priv_path: Path | None = None, comp: str = "gz",
                  config_files: list | None = None) -> Path:
    """把安装目录打包成 .qyp。

    config_files 不写进包元数据，它只在打包时决定哪些 FileEntry
    要打上 config 标记——标记一旦落进包里，装的时候才知道要保护。
    """
    meta.files = [asdict(f) for f in scan_destdir(destdir, config_files)]
    meta.installed_size = sum(f.size for f in scan_destdir(destdir)
                              ) if False else sum(
        (f["size"] for f in meta.files if isinstance(f, dict)))
    # 可复现构建：设置了 SOURCE_DATE_EPOCH 时用固定时间戳并归一化归档。
    # 不设时行为与以前一致（保留真实时间），
    # 因为开发期调试确实需要知道包是什么时候打的。
    from . import repro as RP
    epoch = RP.source_date_epoch()
    reproducible = bool(os.environ.get(RP.ENV_EPOCH))
    meta.build["pack_time"] = epoch if reproducible else int(util.timer())
    meta.build["compression"] = comp
    if reproducible:
        meta.build["reproducible"] = True
        meta.build["source_date_epoch"] = epoch

    meta_bytes = meta.to_json()
    meta_hash = util.sha256_bytes(meta_bytes)

    tmp_tar = out_path.with_suffix(".tar.tmp")
    if reproducible:
        RP.make_repro_tar(destdir, tmp_tar, comp, epoch=epoch)
    else:
        util.make_tar(destdir, tmp_tar, comp)
    data_bytes = tmp_tar.read_bytes()
    data_hash = util.sha256_bytes(data_bytes)
    tmp_tar.unlink()

    digest = bytes.fromhex(meta_hash + data_hash)
    sig_bytes = b""
    if priv_path and priv_path.exists():
        sig_bytes = json.dumps(sign_digest(digest, priv_path),
                               sort_keys=True).encode()

    off = HEADER_SIZE
    meta_off, meta_len = off, len(meta_bytes)
    data_off = meta_off + meta_len
    data_len = len(data_bytes)
    sig_off = data_off + data_len
    sig_len = len(sig_bytes)

    header = struct.pack(
        HEADER_FMT, MAGIC, FORMAT_VERSION, 0,
        meta_off, meta_len, data_off, data_len, sig_off, sig_len,
        bytes.fromhex(meta_hash), bytes.fromhex(data_hash))

    blob = header + meta_bytes + data_bytes + sig_bytes
    util.atomic_write(out_path, blob)
    return out_path


# ---------------------------------------------------------------- 解包

class Package:
    """已打开的 .qyp 包，支持读取元数据、校验、解出数据。"""

    def __init__(self, path: Path):
        self.path = Path(path)
        raw = self.path.read_bytes()
        if len(raw) < HEADER_SIZE or raw[:6] != MAGIC[:6]:
            raise PackageError(f"不是有效的 qyp 包: {self.path}")
        (magic, ver, self.flags,
         self.meta_off, self.meta_len,
         self.data_off, self.data_len,
         self.sig_off, self.sig_len,
         self.meta_hash, self.data_hash) = struct.unpack(HEADER_FMT, raw[:HEADER_SIZE])
        self.version_format = ver
        self.raw = raw

    @property
    def meta(self) -> Meta:
        if not hasattr(self, "_meta"):
            self._meta = Meta.from_json(
                self.raw[self.meta_off:self.meta_off + self.meta_len])
        return self._meta

    @property
    def sig(self) -> dict:
        if self.sig_len == 0:
            return {}
        return json.loads(self.raw[self.sig_off:self.sig_off + self.sig_len])

    @property
    def digest(self) -> bytes:
        return self.meta_hash + self.data_hash

    def verify(self) -> bool:
        """校验数据段与元数据段哈希是否自洽。"""
        return (util.sha256_bytes(
            self.raw[self.meta_off:self.meta_off + self.meta_len]) == self.meta_hash.hex()
            and util.sha256_bytes(
                self.raw[self.data_off:self.data_off + self.data_len]) == self.data_hash.hex())

    def verify_signature(self, pub_path: Path) -> bool:
        s = self.sig
        if not s:
            return False
        return verify_digest(self.digest, s, pub_path)

    def extract(self, dest: Path, check_hashes: bool = True) -> list:
        """解出数据段到 dest，返回解出的相对路径列表。"""
        dest.mkdir(parents=True, exist_ok=True)
        buf = io.BytesIO(self.raw[self.data_off:self.data_off + self.data_len])
        with tarfile.open(fileobj=buf, mode="r:*") as tf:
            tf.extractall(dest)
        out = []
        if check_hashes:
            for f in self.meta.files:
                real = dest / f.path
                if f.type == "file" and real.is_symlink() is False and real.exists():
                    if util.sha256_file(real) != f.sha256:
                        raise PackageError(f"文件校验失败: {f.path}")
                out.append(f.path)
        else:
            out = [f.path for f in self.meta.files]
        return out


def read_package(path: Path) -> Package:
    return Package(path)
