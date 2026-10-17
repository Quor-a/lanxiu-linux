"""LANXIU Linux 构建配方。

配方是纯 Python 模块，声明式字段 + build()/package() 两个函数：

    name = "zlib"
    version = "1.3.1"
    release = 1
    source = ["https://.../zlib-1.3.1.tar.gz"]
    sha256 = ["..."]
    depends = []
    makedepends = []

    def build(ctx):
        ctx.run("./configure --prefix=/usr")
        ctx.run("make")

    def package(ctx):
        ctx.run("make DESTDIR={destdir} install")

用 Python 而非 shell DSL，是为了让解析器、静态检查和依赖元数据抽取都不需要
二次实现一套语言；单人维护成本最低，也便于做条件化打包。
"""
from __future__ import annotations

import importlib.util
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import util

# 配方里可声明的字段
FIELDS = ("name", "version", "release", "epoch", "summary", "description",
          "homepage", "license", "source", "sha256", "depends", "makedepends",
          "provides", "conflicts", "replaces", "options", "arch",
          "strip_components", "network", "compression",
          "checksum_pending", "source_pending", "bootstrap_stage",
          "multipass",
          "requires_build_machine", "cycle_break",
          "config_files", "sysusers", "alternatives", "triggers")


class RecipeError(RuntimeError):
    pass


@dataclass
class Recipe:
    name: str
    version: str
    release: int = 1
    epoch: int = 0
    summary: str = ""
    description: str = ""
    homepage: str = ""
    license: str = ""
    source: list = field(default_factory=list)
    sha256: list = field(default_factory=list)
    depends: list = field(default_factory=list)
    makedepends: list = field(default_factory=list)
    provides: list = field(default_factory=list)
    conflicts: list = field(default_factory=list)
    replaces: list = field(default_factory=list)
    options: list = field(default_factory=list)
    arch: str = util.ARCH
    strip_components: int = 1
    network: bool = False      # build() 期间是否需要网络
    compression: str = "gz"
    checksum_pending: bool = False   # 远程源码校验和待补（构建前拒绝）
    source_pending: bool = False     # 上游源码地址待补（不纳入构建）
    bootstrap_stage: int = 0         # 自举阶段标记
    multipass: bool = False          # 自举过程中需编多遍
    requires_build_machine: bool = False  # 需真实构建机（时长/磁盘/网络门槛）
    cycle_break: list = field(default_factory=list)  # 首轮可暂时断开的依赖
    # /etc 下会被用户改的文件。升级时无条件覆盖等于毁掉用户配置，
    # 所以必须能标记出来做三方比对
    config_files: list = field(default_factory=list)
    sysusers: list = field(default_factory=list)      # 需要创建的系统用户/组
    alternatives: list = field(default_factory=list)  # 提供的可替换命令
    triggers: list = field(default_factory=list)      # 触发的系统动作
    path: Path = None          # type: ignore[assignment]
    _mod: object = None

    @property
    def pkgid(self) -> str:
        return f"{self.name}-{self.version}-{self.release}"

    @property
    def filename(self) -> str:
        return f"{self.pkgid}.{self.arch}.qyp"

    def build_fn(self, ctx):
        fn = getattr(self._mod, "build", None)
        if fn:
            fn(ctx)

    def package_fn(self, ctx):
        fn = getattr(self._mod, "package", None)
        if fn:
            fn(ctx)
        else:
            # 约定优于配置：不写 package() 时，默认按常见安装目标搬文件
            ctx.default_package()

    def has_check(self) -> bool:
        return hasattr(self._mod, "check")

    def check_fn(self, ctx):
        getattr(self._mod, "check")(ctx)


def load(path: Path) -> Recipe:
    path = Path(path)
    if not path.exists():
        raise RecipeError(f"配方不存在: {path}")
    name = path.stem
    spec = importlib.util.spec_from_file_location(f"recipe_{name}", path)
    mod = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    sys.modules[spec.name] = mod  # type: ignore[union-attr]
    try:
        spec.loader.exec_module(mod)  # type: ignore[union-attr]
    except Exception as e:
        raise RecipeError(f"加载配方 {path} 失败: {e}") from e

    values = {}
    for f in FIELDS:
        if hasattr(mod, f):
            values[f] = getattr(mod, f)
    if "name" not in values:
        values["name"] = name
    if "version" not in values:
        raise RecipeError(f"配方 {path} 缺少 version")
    # 远程源码必须锁定校验和，否则构建不可复现；本地源码目录可以留空。
    # 例外：上游大包（内核/gcc 之类）在配方刚落地时往往还没下回来，
    # 这时允许声明 checksum_pending，构建阶段会明确拒绝并提示怎么补。
    for s in values.get("source", []):
        if (s.startswith(("http://", "https://", "ftp://"))
                and not values.get("sha256")
                and not values.get("checksum_pending")):
            raise RecipeError(
                f"配方 {values['name']}: 远程源码必须提供 sha256，否则无法保证可复现。"
                f"若尚未下载，可在配方里写 checksum_pending = True，"
                f"构建前用 qybuild fetch-checksums {values['name']} 补齐")
    if values.get("sha256") and len(values["sha256"]) != len(values.get("source", [])):
        raise RecipeError(
            f"配方 {values['name']}: source 与 sha256 数量不匹配")

    r = Recipe(**values)
    r.path = path
    r._mod = mod
    return r


def load_tree(root: Path) -> dict:
    """加载整个配方树，返回 {包名: Recipe}。"""
    root = Path(root)
    recipes = {}
    for p in sorted(root.rglob("*.py")):
        if p.name.startswith("_"):
            continue
        r = load(p)
        if r.name in recipes:
            util.log("warn", f"配方重名: {r.name} ({p})")
        recipes[r.name] = r
    return recipes
