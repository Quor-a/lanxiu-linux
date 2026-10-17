"""LANXIU Linux（LANXIU Linux）构建与包管理内核。

模块划分：
    util     哈希/归档/下载/进程/日志
    format   .qyp 包格式与 ed25519 签名
    recipe   构建配方（Python 声明式）
    sandbox  构建隔离（环境净化 + 命名空间）
    deps     依赖求解
    builder  构建器
    repo     仓库索引与验签
    pkgmgr   包管理器核心
    cli      命令行入口
"""
__version__ = "0.1.0"
__distro__ = "LANXIU Linux"
