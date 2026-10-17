#!/usr/bin/env python3
"""生成用户手册。

文档生成器而不是手写文档：命令选项、配方字段、形态清单这些会变，
手写必然与代码脱节——而脱节的文档比没有文档更危险，用户按它操作
会直接踩坑。这里从代码里读实际定义，保证写出来的每个选项都存在。

生成的产物是 Markdown，可以转 HTML/PDF，也可以直接读。
"""
from __future__ import annotations

import argparse
import importlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))   # 脚本在 scripts/ 下，要能 import qyos
OUT = ROOT / "docs"


def sh(*args: str) -> str:
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    return (r.stdout or r.stderr).strip()


def strip_ansi(s: str) -> str:
    import re
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def cmd_help(bin_name: str) -> str:
    """取某个命令行工具的 --help 输出。"""
    return strip_ansi(sh(f"./bin/{bin_name}", "--help"))


def recipe_fields() -> list:
    from qyos import recipe as R
    return list(R.FIELDS)


def profiles() -> list:
    from qyos import profile as PF
    return sorted(PF.PROFILES.items())


def human(n: int) -> str:
    """字节数转易读形式。min_disk 存的是字节，直接打印是一串天文数字。"""
    for unit in ("B", "K", "M", "G", "T"):
        if n < 1024 or unit == "T":
            return f"{int(n)}{unit}" if unit != "B" else f"{n}B"
        n /= 1024.0
    return str(n)


def kernel_fragments() -> list:
    """读 kernel/config/ 下真实的片段文件，而不是另维护一份清单。"""
    d = ROOT / "kernel" / "config"
    if not d.exists():
        return []
    out = []
    for f in sorted(d.glob("*.fragment")):
        head = ""
        for line in f.read_text().splitlines():
            if line.startswith("#") and len(line.strip()) > 1:
                head = line.lstrip("# ").strip()
                break
        out.append((f.stem, head))
    return out


def recipe_count() -> int:
    from qyos import recipe as R
    return len(R.load_tree(ROOT / "recipes"))


def gen_user_manual() -> str:
    n = recipe_count()
    L = []
    w = L.append

    w("# LANXIU Linux 用户手册")
    w("")
    w("面向装机并使用LANXIU Linux 的人：怎么装、怎么装软件、怎么升级、出问题怎么办。")
    w("")
    w("想改配方、加包、改构建系统，请看《维护者手册》。")
    w("")
    w("---")
    w("")

    w("## 1. 安装")
    w("")
    w("### 1.1 需要什么")
    w("")
    w("| 形态 | 最小内存 | 最小磁盘 | 说明 |")
    w("|---|---|---|---|")
    for name, p in profiles():
        w(f"| {p.title} | {human(p.min_memory)} | {human(p.min_disk)} | {p.description} |")
    w("")
    w("构建机另算：编译整套系统需要 ≥8 核 / 32G 内存 / 200G 磁盘。")
    w("")
    w("### 1.2 装机")
    w("")
    w("装机分六步，每步可单独重跑——某一步失败不必从头再来：")
    w("")
    w("```bash")
    w("# 先生成装机脚本（不直接动磁盘，可以先看一遍脚本再执行）")
    w("./bin/qydisk scripts --disk /dev/sda --target /mnt/target \\")
    w("        --memory 8G --size 100G --profile desktop")
    w("```")
    w("")
    w("生成 6 个脚本，按顺序执行，每步可单独重跑：")
    w("")
    w("| 脚本 | 做什么 |")
    w("|---|---|")
    w("| `1-partition.sh` | 分区（会先二次确认再清空磁盘） |")
    w("| `2-mount.sh` | 挂载到 target |")
    w("| `3-install.sh` | 按形态装包并做可启动性检查 |")
    w("| `4-configure.sh` | chroot 内配置（主机名、locale、用户） |")
    w("| `5-bootloader.sh` | 装引导器 |")
    w("| `9-umount.sh` | 卸载 |")
    w("")
    w("**先生成脚本而不是直接执行**，是为了让你在运行前能逐行看一遍——")
    w("尤其 1-partition.sh，它会清空你指定的整块磁盘。")
    w("")
    w("**清空磁盘前会强制二次确认**——这一步不可逆，脚本会先 wipefs 清掉旧签名。")
    w("")
    w("分区方案是算出来的，不是手填的：swap 跟着内存走（<2G 给 2 倍，")
    w("8~64G 给一半上限 8G，>64G 固定 4G）。以下情形会被明确拒绝，")
    w("不会让你装到一半才发现：")
    w("")
    w("- 磁盘小于方案所需")
    w("- EFI 分区小于 260M（规范下限）")
    w("- EFI 分区不是 FAT32")
    w("- 同时有两个「自动增长」的分区")
    w("")
    w("### 1.3 为什么 fstab 用 UUID")
    w("")
    w("`root=/dev/sda2` 在加装硬盘、换接口后就指错设备——")
    w("「昨天还好好的今天起不来」的头号原因。LANXIU一律写 UUID。")
    w("")
    w("---")
    w("")

    w("## 2. 装软件")
    w("")
    w("```bash")
    w("qypkg install vim git")
    w("qypkg remove --unused vim")
    w("qypkg search editor")
    w("qypkg info vim")
    w("qypkg list --installed")
    w("```")
    w("")
    w("### 2.1 包格式")
    w("")
    w("自建的 `.qyp` 格式：128 字节头 + JSON 元数据 + tar 数据 + 可分离签名。")
    w("**不解压就能校验签名**——仓库索引和包本身都带签名，")
    w("任何一环被篡改都会被明确拒绝，不会静默装上。")
    w("")
    w("### 2.2 打了哪些补丁")
    w("")
    w("包元数据里记录了它相对上游打了哪些补丁、为什么打。")
    w("你可以查到手上的包和上游源码差在哪，不必猜：")
    w("")
    w("```bash")
    w("./bin/qypatch list --package vim")
    w("```")
    w("")
    w("补丁和源码一样锁定 sha256——补丁被改会静默改变产物，")
    w("那是供应链攻击的入口。")
    w("")
    w("### 2.3 依赖")
    w("")
    w("依赖关系是**自动发现的**：安装时会解析出完整闭包一起装上。")
    w("包里的程序链接了哪些共享库，由构建系统扫描产物得出，")
    w("不靠维护者手写——手写的必然遗漏。")
    w("")
    w("卸载时会拒绝仍在被依赖的包：")
    w("")
    w("```")
    w("qypkg remove openssl")
    w("!!! 无法卸载：curl、openssh、python 仍依赖它")
    w("```")
    w("")
    w("### 2.4 查文件归属")
    w("")
    w("```bash")
    w("qypkg owns /usr/bin/vim      # 这个文件属于哪个包")
    w("qypkg why vim                # 谁依赖它（判断能不能删）")
    w("```")
    w("")
    w("---")
    w("")

    w("## 3. 升级")
    w("")
    w("```bash")
    w("qypkg upgrade")
    w("```")
    w("")
    w("### 3.1 升级是整批事务")
    w("")
    w("一次升级往往横跨几十个包。装到第 30 个失败、前 29 个已写进系统，")
    w("断电就是一台半残的机器。LANXIU的做法是**整批事务**：")
    w("")
    w("1. 先做文件级快照")
    w("2. 再写入")
    w("3. 失败自动全部还原")
    w("")
    w("下次启动还会自动检出上次没完成的事务并回滚——**断电不会留下半升级状态**。")
    w("")
    w("### 3.2 升级前的三项检查")
    w("")
    w("不通过就根本不开始，不会让你升到一半：")
    w("")
    w("- 新依赖是否在仓库里")
    w("- 是否与已装包冲突")
    w("- 磁盘空间够不够（含快照）")
    w("")
    w("### 3.3 回滚")
    w("")
    w("```bash")
    w("qypkg snapshots                    # 看有哪些快照")
    w("qypkg rollback 20260930-032946     # 回滚到指定快照")
    w("```")
    w("")
    w("快照按事务保留，升级成功后仍可用于回滚。")
    w("")
    w("---")
    w("")

    w("## 4. 安全更新")
    w("")
    w("```bash")
    w("qysec scan --root /                # 这台机器上有什么没修")
    w("qysec by-cve --cve CVE-2026-0001   # 某个 CVE 影响哪些包")
    w("```")
    w("")
    w("按**版本区间**判定，不是按版本号相等。CVE 影响 1.2~2.0，")
    w("只查 `version == 1.2` 会漏掉绝大多数受影响的安装。")
    w("")
    w("扫描输出示例：")
    w("")
    w("```")
    w("[critical] libqydemo-0.1.0  CVE-2026-0001 → 修于 0.2.0（可修复）")
    w("[critical] qydemo-0.1.0     CVE-2026-0001 → 尚无修复版本（待上游修复）")
    w("```")
    w("")
    w("「待上游修复」也会如实报出来，不会假装没问题。")
    w("")
    w("---")
    w("")

    w("## 5. 可复现构建：你可以自己验证")
    w("")
    w("同一个配方在任何机器上构建，产出的包逐字节相同。")
    w("这意味着**你可以自己重编一遍，比对哈希**，")
    w("确认官方发布的包里没有夹带任何源码之外的东西。")
    w("")
    w("```bash")
    w("export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)")
    w("./bin/qybuild zlib --force")
    w("./bin/qyrepro verify zlib    # 连编两次，逐字节比对")
    w("```")
    w("")
    w("做不到可复现，这条验证路径就不存在——你只能选择相信。")
    w("")
    w("---")
    w("")
    w("## 6. 安卓设备")
    w("")
    w("手机/平板/开发板形态（mobile）。引导链与 PC 完全不同：")
    w("boot.img 把内核、initramfs、设备树打包成一个文件，")
    w("system/vendor 是 super 动态分区里的子分区。")
    w("")
    w("**更新分两层**：整槽更新走 A/B（切槽，失败自动回退），")
    w("日常软件更新走包级事务。两套都要有——")
    w("只有 A/B 的话，槽内的包升级坏了要等下次整槽更新才能修。")
    w("")
    w("刷机需要解锁 bootloader，**解锁会清空全部用户数据**。")
    w("")
    w("---")
    w("")
    w("## 7. 出问题怎么办")
    w("")
    w("### 5.1 起不来")
    w("")
    w("GRUB 菜单里**有一个非静默的救援项**——系统起不来时第一件事就是看内核输出。")
    w("只给一个 `quiet` 项是给自己找麻烦。")
    w("")
    w("### 5.2 升级后异常")
    w("")
    w("```bash")
    w("qypkg snapshots && qypkg rollback <上个快照>")
    w("```")
    w("")
    w("### 5.3 装机后权限不对")
    w("")
    w("某些文件系统（virtiofs、容器 overlay）不支持 chmod，")
    w("装机脚本会提示「装机到真实磁盘后需复查」。在真实磁盘上不会发生。")
    w("")
    w("---")
    w("")
    w(f"当前配方库 {n} 个包。手册由 `scripts/gen_docs.py` 从代码生成，")
    w("命令选项与形态清单均取自实际定义，不会与代码脱节。")
    w("")
    return "\n".join(L)


def gen_maintainer_manual() -> str:
    import re
    L = []
    w = L.append

    w("# LANXIU Linux 维护者手册")
    w("")
    w("面向改配方、加包、动构建系统的人。")
    w("")
    w("---")
    w("")

    w("## 1. 配方怎么写")
    w("")
    w("配方是纯 Python 模块：声明式字段 + `build()` / `package()` 两个函数。")
    w("")
    w("```python")
    w('"""zlib —— 通用无损数据压缩库。"""')
    w("")
    w('name = "zlib"')
    w('version = "1.3.1"')
    w("release = 1")
    w('summary = "通用无损数据压缩库"')
    w('license = "Zlib"')
    w('provides = ["libz.so.1"]')
    w("")
    w('source = ["https://zlib.net/fossils/zlib-1.3.1.tar.gz"]')
    w('sha256 = ["38ef96b8dfe510d42707d9c781079c3709d0"]')
    w("")
    w("depends = []")
    w("makedepends = []")
    w("")
    w("def build(ctx):")
    w('    ctx.run("./configure --prefix=/usr")')
    w('    ctx.run("make")')
    w("")
    w("def package(ctx):")
    w('    ctx.run("make DESTDIR={} install".format(ctx.destdir))')
    w("```")
    w("")
    w("### 1.1 可用字段")
    w("")
    w("| 字段 | 说明 |")
    w("|---|---|")
    desc = {
        "name": "包名，与文件名一致",
        "version": "上游版本号",
        "release": "打包修订号（改打包方式但版本不变时 +1）",
        "epoch": "版本比较权重，默认 0，极少需要",
        "summary": "一句话描述",
        "description": "详细描述",
        "homepage": "上游主页",
        "license": "许可证 SPDX 表达式",
        "source": "源码列表（URL 或本地路径）",
        "sha256": "与 source 一一对应的校验和",
        "depends": "运行期依赖",
        "makedepends": "构建期依赖",
        "provides": "提供的能力名（供虚拟依赖反查）",
        "conflicts": "冲突包",
        "replaces": "取代的包",
        "options": "打包选项（含 `!shlibdeps` 可关掉自动依赖发现）",
        "arch": "目标架构，默认当前",
        "strip_components": "解包时剥掉几层目录，默认 1",
        "network": "build() 期间是否需要网络，默认 False",
        "compression": "gz / xz / none，默认 gz",
        "checksum_pending": "源码校验和待补（**构建前会被拒绝**）",
        "bootstrap_stage": "自举阶段标记",
        "multipass": "自举过程中需编多遍",
        "requires_build_machine": "需真实构建机（时长/磁盘/网络门槛）",
        "cycle_break": "循环依赖时首轮可暂时断开的依赖",
    }
    for f in recipe_fields():
        w(f"| `{f}` | {desc.get(f, '')} |")
    w("")
    w("### 1.2 两段式：build 与 package 必须分开")
    w("")
    w("`build()` 编译，`package()` 只往 `destdir` 里装文件。")
    w("这样打包时不会把构建中间产物混进包里，")
    w("也让「换个安装前缀重新打包」不需要重编。")
    w("")
    w("### 1.3 源码校验和")
    w("")
    w("远程源码必须锁定 sha256。没锁的配方会**被拒绝构建**：")
    w("")
    w("```")
    w("gcc: 远程源码缺少 sha256，拒绝构建。")
    w("  请在能联网的构建机上执行：qybuild --fetch-checksums gcc")
    w("```")
    w("")
    w("静默接受未校验的远程源码等于给供应链攻击敞开大门。")
    w("")
    w("---")
    w("")

    w("## 2. 构建")
    w("")
    w("```bash")
    w("./bin/qybuild zlib                  # 单包")
    w("./bin/qybuild all --sign KEY        # 全量（跳过需真实构建机的）")
    w("./bin/qybuild all --orchestrate     # 分层并行")
    w("./bin/qybuild --deps python         # 看装它牵出多少包")
    w("./bin/qybuild --cycles              # 查循环依赖")
    w("./bin/qybuild --shlibdeps qydemo    # 审计自动依赖")
    w("./bin/qybuild --kernel-config       # 校验内核配置")
    w("```")
    w("")
    w("### 2.1 沙箱")
    w("")
    w("构建在隔离环境中执行：命名空间隔离 + 默认断网。")
    w("默认断网是有意的——构建期偷偷下载会让产物不可复现。")
    w("")
    w("### 2.2 缓存")
    w("")
    w("缓存键 = 配方内容 + **源码内容** + 依赖版本。")
    w("源码内容必须进键：只算配方哈希的话，改了 C 源码但没改版本号时缓存会命中，")
    w("编出来的是**旧代码打新版本号**的包——能装上、能跑，只是行为是旧的，最难排查。")
    w("")
    w("### 2.3 加固项")
    w("")
    w("RELRO / NX / PIE / Canary 全系统统一开启。")
    w("编译参数写对不等于产物生效（上游常常覆盖它），")
    w("所以是直接**检查产物本身**。")
    w("")
    w("---")
    w("")

    w("## 3. 循环依赖")
    w("")
    w("包库大了必然撞上。图形栈里就有一个真实的：")
    w("")
    w("```")
    w("cairo → fontconfig → freetype → harfbuzz → cairo")
    w("```")
    w("")
    w("处理办法是**断一环，两遍构建**：")
    w("")
    w("```python")
    w("cycle_break = [\"harfbuzz\"]   # 写在 freetype 配方里")
    w("```")
    w("")
    w("首轮编一个不带 harfbuzz 的 freetype（能渲染基本字形），")
    w("用它编出 harfbuzz，然后**必须重编 freetype**。")
    w("")
    w("> 忘了重编，系统会带着一个不支持复杂文本排版的 freetype 发布出去——")
    w("> 能跑、能过测试，中文和阿拉伯文渲染是错的。")
    w("")
    w("`qybuild --cycles` 会明确列出第二轮要重编哪些包。")
    w("")
    w("---")
    w("")

    w("## 4. 系统形态")
    w("")
    from qyos import recipe as R
    from qyos import profile as PF
    from qyos.deps import Universe
    from qyos import cycles as CY
    rs = R.load_tree(ROOT / "recipes")
    cpl = CY.plan(rs)
    u = Universe(broken=cpl.broken_deps)
    for r in rs.values():
        u.add(r)
    w("| 形态 | 标题 | 必装 | 可选 | 实际会装 | 最小磁盘 | 最小内存 |")
    w("|---|---|---|---|---|---|---|")
    for name, p in profiles():
        total = len(u.resolve(p.all_packages()))
        w(f"| {name} | {p.title} | {len(p.packages)} | {len(p.extra_packages)} "
          f"| {total} | {human(p.min_disk)} | {human(p.min_memory)} |")
    w("")
    w("形态必须**声明它排除了什么**——只写「包含桌面」不写「不装打印服务」，")
    w("用户拿到手才知道少了什么。")
    w("")
    w("---")
    w("")

    w("## 5. 内核")
    w("")
    w("配置项上万条，手写 `.config` 不可维护。改成**基线 + 片段叠加**：")
    w("")
    frags = kernel_fragments()
    if frags:
        w("| 片段 | 说明 |")
        w("|---|---|")
        for k, v in frags:
            w(f"| `{k}` | {v} |")
        w("")
    w("构建时强制校验必需项和禁用项。这份清单本身就是发行版的安全姿态声明——")
    w("写进代码，每次构建都查，不靠人记。")
    w("")
    w("---")
    w("")

    w("## 6. 安卓设备适配")
    w("")
    w("把系统装进安卓设备，引导链与分区模型完全换了：")
    w("")
    w("```")
    w("PC:       UEFI → GRUB → 内核 → initramfs → 切根")
    w("安卓设备: bootloader → boot.img（内核+ramdisk+dtb 打包成一个文件）")
    w("          → super 动态分区（system/vendor 是子分区，不是真分区）")
    w("```")
    w("")
    w("```bash")
    w("./bin/qyandroid bootimg --kernel vmlinuz --ramdisk initrd.img \\")
    w("        --dtb board.dtb --page-size 4096 --os-version 13.0.0")
    w("./bin/qyandroid verify boot.img      # 交付前自检")
    w("./bin/qyandroid super --root-mb 3072  # 动态分区布局")
    w("./bin/qyandroid flash --slot _a       # 生成刷机脚本")
    w("./bin/qyandroid fstab                 # 安卓设备 fstab")
    w("./bin/qyandroid kernel-fragment       # 安卓内核配置片段")
    w("./bin/qyandroid notes                 # A/B 与原子升级的关系")
    w("```")
    w("")
    w("### 6.1 页大小错了设备直接黑屏")
    w("")
    w("boot.img 的各部分必须按页对齐，页大小不匹配时 bootloader 读错位，")
    w("表现为**黑屏无任何输出**——这是安卓装机最难排查的问题。")
    w("工具会在打包时校验页大小取值并自检对齐。")
    w("")
    w("### 6.2 A/B 槽不是原子升级，两套都要有")
    w("")
    w("两者都叫「无缝」，粒度完全不同：")
    w("")
    w("| | 粒度 | 代价 |")
    w("|---|---|---|")
    w("| A/B 槽 | 整个系统，重启换槽 | 占用双倍 system 空间 |")
    w("| 原子升级 | 包级事务，失败回滚到包级快照 | 不占额外空间 |")
    w("")
    w("只做 A/B：槽内的日常包升级坏了就是坏的，要等下次整槽更新（以月计）。")
    w("只做包级事务：包管理器或内核被写坏时无人可救，引导链已经坏了。")
    w("")
    w("所以：**整槽更新走 A/B（内核、vendor 这类动引导链的），")
    w("日常软件更新走包级事务**。")
    w("")
    w("### 6.3 system 分区必须只读")
    w("")
    w("verified boot 强制要求。桌面 Linux 的可写 /usr 在这里不成立。")
    w("")
    w("### 6.4 fstab 不能用 UUID")
    w("")
    w("super 是动态分区，重建逻辑分区时子分区 UUID 会变——")
    w("写在 fstab 里的 UUID 下次更新就失效，表现为「更新后起不来」。")
    w("也不能用 /dev/sda1（块设备节点顺序不稳定）。")
    w("必须用 `/dev/block/by-name/<分区名>`。")
    w("")
    w("### 6.5 刷机风险")
    w("")
    w("解锁 bootloader 会清空全部用户数据，且多数厂商解锁后失去保修。")
    w("变砖风险真实存在且远高于 PC 装机——安卓设备的 bootloader")
    w("多数没有 PC 那样的恢复菜单。生成的脚本会明确警告。")
    w("")
    w("---")
    w("")
    w("## 7. 持续集成与发布门禁")
    w("")
    w("177 个包没有 CI 就没法维护：改一个库，谁要重编？编完了能发吗？")
    w("")
    w("```bash")
    w("./bin/qyci plan                 # 查 git 改动 → 算出谁要重编")
    w("./bin/qyci plan --changed zlib  # 或直接指定")
    w("./bin/qyci gates                # 跑发布门禁")
    w("./bin/qyci run --report ci.txt  # 完整流水线")
    w("```")
    w("")
    w("影响面示例：")
    w("")
    w("```")
    w("改 zlib    → 重编 58 个包，分 7 层")
    w("改 ncurses → 重编 23 个包，分 4 层")
    w("```")
    w("")
    w("### 7.1 六道发布门禁")
    w("")
    w("构建成功不等于能发布。以下任一项不过，整个批次卡住——")
    w("它们全都是「装到用户机器上才暴露」的问题：")
    w("")
    w("| 门禁 | 拦截什么 |")
    w("|---|---|")
    w("| 远程源码锁定 sha256 | 供应链：未锁定的源码可被替换 |")
    w("| 包已签名 | 分发完整性：无法验证来源 |")
    w("| 索引与签名匹配 | 索引被替换或篡改 |")
    w("| ELF 加固项齐全 | RELRO/NX/PIE/Canary 缺失 |")
    w("| 产物不含构建机路径 | 可复现 + 泄露构建机目录 |")
    w("| 版本未回退 | 误发布旧版本覆盖新版本 |")
    w("")
    w("门禁失败会**说清是哪一项、哪个包、怎么修**——")
    w("只报「检查未通过」的话维护者只能猜。")
    w("")
    w("门禁只针对**发布集**（仓库里实际有的包）。检查全部配方的话，")
    w("尚未下载源码的配方会让门禁永远红着，那就没人再看它了。")
    w("未就绪的配方单列提醒，不阻塞发布。")
    w("")
    w("---")
    w("")
    w("## 8. 可复现构建")
    w("")
    w("同一个配方在任何机器上、任何时间构建，产出逐字节相同。")
    w("")
    w("这不是洁癖，是发行版能不能被信任的基础：")
    w("")
    w("- **能验证**：用户可自己重编一遍比对哈希，确认官方包里没夹带东西")
    w("- **能审计**：供应链攻击正是靠「发布包与源码不一致」存在的，")
    w("  可复现让这种不一致必然暴露")
    w("- **能增量**：产出哈希稳定，缓存才可靠")
    w("")
    w("```bash")
    w("export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)")
    w("./bin/qybuild all --sign KEY")
    w("./bin/qyrepro check var/pkgs/zlib-*.qyp   # 检查单个包")
    w("./bin/qyrepro verify zlib                 # 连编两次逐字节比对")
    w("./bin/qyrepro epoch                       # 当前会用哪个时间戳")
    w("```")
    w("")
    w("破坏可复现的来源，逐个处理：")
    w("")
    w("| 来源 | 处理 |")
    w("|---|---|")
    w("| 时间戳 | SOURCE_DATE_EPOCH 钳制，gzip 头 mtime 固定为 0 |")
    w("| 构建路径 | 编译加 `-ffile-prefix-map` |")
    w("| 文件顺序 | 按 arcname 排序，不依赖 os.walk 顺序 |")
    w("| 构建机信息 | 属主归一为 0:0，不记主机名 |")
    w("")
    w("未设 SOURCE_DATE_EPOCH 时行为与以前一致（保留真实打包时间），")
    w("因为开发期调试确实需要知道包是什么时候打的。")
    w("")
    w("---")
    w("")
    w("## 9. 交叉编译")
    w("")
    w("给 aarch64 设备编 177 个包，不可能只在 aarch64 机器上原生编译——")
    w("手机类设备根本装不下编译环境。")
    w("")
    w("```bash")
    w("./bin/qycross info --host aarch64        # 看三元组与模式")
    w("./bin/qycross env --host aarch64 --shell # 输出可 source 的环境变量")
    w("./bin/qycross cmake --host aarch64 --out cross.cmake")
    w("./bin/qycross meson --host aarch64 --out cross.meson")
    w("./bin/qycross args --host aarch64 --name zlib   # autotools 参数")
    w("./bin/qycross check /path/to/elf --arch aarch64 # 校验架构")
    w("./bin/qybuild --target-arch aarch64 all --sign KEY")
    w("```")
    w("")
    w("### 9.1 build / host / target 必须分清")
    w("")
    w("混淆了就是几天的白工：")
    w("")
    w("| | 含义 |")
    w("|---|---|")
    w("| build | 编译器在哪台机器上运行（构建机） |")
    w("| host | 编出来的程序在哪台机器上运行（目标机） |")
    w("| target | 编出来的编译器会生成哪种机器的代码 |")
    w("")
    w("- 原生编译：build = host = target")
    w("- 交叉编译：build = x86_64，host = aarch64")
    w("- 编交叉编译器：build = x86_64，host = x86_64，target = aarch64")
    w("- 加拿大交叉：三者都不同（在 A 上编跑在 B 上、生成 C 代码的编译器）")
    w("")
    w("### 9.2 三个真实的坑")
    w("")
    w("**不能用宿主 pkg-config**——它返回构建机的库路径，编出的包链接到")
    w("x86_64 的库，装到设备上直接「找不到共享库」。工具会自动指向目标 sysroot。")
    w("")
    w("**构建期要跑的程序必须给 build 编译**。很多项目构建时先编一个代码")
    w("生成器再执行它（protobuf、wayland-scanner、gdbus-codegen）。")
    w("交叉编译时那个生成器是 aarch64 的，在 x86_64 上跑不了。")
    w("所以同时注入 `CC_FOR_BUILD`。")
    w("")
    w("**--target 只在编 binutils/gcc 时给**。给普通包传 --target 会让")
    w("configure 报错或行为异常。工具按包名自动判断。")
    w("")
    w("### 9.3 产物架构会强制校验")
    w("")
    w("最容易出的错是「编出来还是 x86_64 却当成 aarch64 发出去」——")
    w("装上设备报格式错误，而构建日志看起来一切正常。")
    w("打包后会自动扫包内所有 ELF 确认架构。")
    w("")
    w("---")
    w("")
    w("## 10. 补丁管理")
    w("")
    w("发行版必然要打补丁——上游有安全问题时不能等它发版。")
    w("没有补丁机制，发行版就只是转发上游。")
    w("")
    w("```bash")
    w("./bin/qypatch list                 # 全系统补丁一览")
    w("./bin/qypatch list --package zlib  # 单个包的补丁")
    w("./bin/qypatch verify               # 校验补丁文件与声明")
    w("./bin/qypatch stale                # 找出上游已包含、可删的补丁")
    w("./bin/qypatch checksum patches/x.patch")
    w("```")
    w("")
    w("补丁声明在 `recipes/patches.toml`，一个清单而不是散在配方里——")
    w("散在 177 个配方里没人看得见全貌。")
    w("")
    w("### 10.1 三类补丁，风险完全不同")
    w("")
    w("| 类型 | 说明 | 必须填 |")
    w("|---|---|---|")
    w("| upstream | 从上游 cherry-pick | `upstream_commit` + `fixed_in` |")
    w("| distro | 本发行版自己写的 | `reason` |")
    w("| security | 修 CVE | `cve` |")
    w("")
    w("上游补丁必须记 commit id——不记的话三个月后没人知道它从哪来、")
    w("上游哪个版本已包含、能不能删。安全补丁必须记 CVE，")
    w("否则安全扫描无法关联。")
    w("")
    w("### 10.2 三条硬性规则")
    w("")
    w("- **补丁要有 sha256**。跟源码一样。补丁被改会静默改变产物，")
    w("  这正是供应链攻击的入口")
    w("- **打不上必须失败，不能静默跳过**。打不上通常意味着上游改了代码、")
    w("  补丁已失效。静默跳过的后果是「以为修了其实没修」")
    w("- **打了补丁要在包元数据里留痕**。用户能查到这个包和上游差在哪")
    w("")
    w("---")
    w("")
    w("## 11. 多架构仓库")
    w("")
    w("```bash")
    w("./bin/qyrepo archs                     # 有哪些架构、各多少个包")
    w("./bin/qyrepo manifest --sign KEY       # 生成顶层多架构清单")
    w("./bin/qyrepo sync --arch aarch64 --sign KEY")
    w("```")
    w("")
    w("顶层清单让客户端知道仓库支持哪些架构，不必猜或硬编码。")
    w("**清单本身要签名**——它能指定从哪里取索引，")
    w("被篡改等于把客户端指向攻击者的仓库。")
    w("")
    w("`sync` 后会自动校验架构一致性：交叉编译下最容易出的错是")
    w("索引说 aarch64 而包里其实是 x86_64，")
    w("客户端不会察觉，装到设备上才报格式错误——那时已经刷完机了。")
    w("")
    w("---")
    w("")
    w("## 12. 发布")
    w("")
    w("```bash")
    w("./bin/qyrelease bump --version 0.1.0-rc.2 --bump promote")
    w("./bin/qyrelease manifest --dir var/release --sign KEY")
    w("./bin/qyrelease verify --dir var/release --pubkey KEY.pub")
    w("./bin/qyrelease changelog --version 0.1.0")
    w("```")
    w("")
    w("**清单里每个产物都有 sha256，清单本身也签名。**")
    w("只发镜像不发校验和，用户无法判断拿到的是不是被中间人换过的东西。")
    w("")
    w("---")
    w("")

    w("## 13. 软件源")
    w("")
    w("本地仓库解决「怎么组织包」，软件源解决「用户从哪拿包」。")
    w("")
    w("```bash")
    w("./bin/qysource list                  # 有哪些源、优先级")
    w("./bin/qysource check                 # 源是否可达")
    w("./bin/qysource resolve qydemo        # 看某个包会从哪个源来")
    w("./bin/qysource add mirror URL --priority 30")
    w("./bin/qysource disable official")
    w("```")
    w("")
    w("几个刻意的设计：")
    w("")
    w("- **本地源优先级最高**。自己构建的包应优先于远程同名包，")
    w("  否则本地改了代码却装到远程旧包，表现为「我明明改了怎么没生效」")
    w("- **优先级是显式数字**，不用配置顺序隐含表达——")
    w("  重排配置文件不该静默改变装到哪个包")
    w("- **同优先级按名字排序**，保证两次解析结果一致")
    w("- **源失效自动降级到下一个**，不就此装不了软件")
    w("- **未签名的源需 `--allow-unsigned`**，静默接受等于把供应链交给运气")
    w("")
    w("---")
    w("")
    w("## 14. 网络服务与端口")
    w("")
    w("装完包之后，服务怎么对外提供能力。")
    w("")
    w("```bash")
    w("./bin/qynet add-port ssh 22 --bind 0.0.0.0 --desc '远程管理'")
    w("./bin/qynet ports                    # 已登记端口")
    w("./bin/qynet check                    # 冲突检查")
    w("./bin/qynet firewall                 # 生成 nftables 规则")
    w("./bin/qynet ssh                      # 生成 sshd_config")
    w("./bin/qynet ftp                      # 生成 vsftpd.conf")
    w("```")
    w("")
    w("四个必须做对的点：")
    w("")
    w("- **端口集中登记**。两个服务抢同一端口，日志只说")
    w("  `address already in use`，看不出是谁占了；登记后能直接指出冲突方")
    w("- **特权端口（<1024）标注**，它们需要 root 或 `CAP_NET_BIND_SERVICE`")
    w("- **监听默认 `127.0.0.1`**。把数据库默认暴露到所有网卡是常见事故，")
    w("  要对外必须显式声明并复核")
    w("- **防火墙只放行对外暴露的端口**。没登记却能被访问，")
    w("  说明有别的东西在监听——那正是要排查的")
    w("")
    w("sshd 默认禁密码登录、禁 root 直接登录；")
    w("vsftpd 默认不匿名、限制在用户目录，并提示 FTP 是明文协议。")
    w("")
    w("---")
    w("")
    w("## 15. 通用包与运行时")
    w("")
    w("发行版自己的包格式解决不了两类需求：上游只想发一个包给所有发行版，")
    w("用户想装最新版而不想等发行版打包。所以必须支持通用包。")
    w("")
    w("```bash")
    w("./bin/qyapp list                     # 已登记的通用包")
    w("./bin/qyapp audit                    # 审计权限")
    w("./bin/qyapp perms org.example.Editor")
    w("./bin/qyapp sandbox org.example.Editor")
    w("./bin/qyapp run Foo.AppImage         # 直接跑，不留残留")
    w("./bin/qyapp run Foo.AppImage --extract   # FUSE 不可用时的兜底")
    w("./bin/qyapp runtimes                 # 运行时多版本")
    w("```")
    w("")
    w("三种通用包各有各的风险，不加约束会破坏发行版完整性：")
    w("")
    w("| 格式 | 优点 | 风险与约束 |")
    w("|---|---|---|")
    w("| AppImage | 单文件、不留残留 | 无来源验证，必须记录 sha256 或验签 |")
    w("| Flatpak | 共享运行时、权限声明式 | 默认权限常过宽，需审计 |")
    w("| Snap | 自带依赖、沙箱 | 经典模式无隔离，需重点审计 |")
    w("")
    w("**必须纳入登记**：不登记的话安全扫描看不到它们，")
    w("「这台机器装了什么」就答不准，出安全事件时这是要命的。")
    w("")
    w("运行时只能共存 + 切换，不能「升级」——")
    w("老应用依赖旧运行时，升级运行时会让它们崩掉。")
    w("删除被使用中的运行时会被拦下。")
    w("")
    w("---")
    w("")
    w("## 16. 命令行工具")
    w("")
    for tool, title in (("qybuild", "构建系统"), ("qypkg", "包管理器"),
                        ("qyrepo", "仓库管理"), ("qysec", "安全响应"),
                        ("qyrelease", "发布工程"), ("qyandroid", "安卓设备适配"),
                        ("qyci", "持续集成"), ("qyrepro", "可复现构建"),
                        ("qydisk", "磁盘与镜像"), ("qycross", "交叉编译"),
                        ("qypatch", "补丁管理"),
                        ("qysource", "软件源"), ("qynet", "网络服务与端口"),
                        ("qyapp", "通用包与运行时"),
                        ("qypam", "PAM 认证配置"),
                        ("qylocale", "语言与键盘布局"),
                        ("qyhw", "硬件支持与固件检测"),
                        ("qyfiles", "文件类型与解压"),
                        ("qydesktop", "桌面外壳"),
                        ("qyrun", "脚本执行系统"),
                        ("qyctl", "系统控制"),
                        ("qyperms", "权限框架"),
                        ("qymedia", "媒体与个人数据"),
                        ("qynotify", "通知与窗口"),
                        ("qykmod", "内核模块管理"),
                        ("qyudev", "设备节点权限"),
                        ("qyio", "输入设备与外设"),
                        ("qysensor", "传感器与平台控制"),
                        ("qyshell", "桌面外壳"),
                        ("qyproc", "进程与资源"),
                        ("qydrv", "驱动管理")):
        w(f"### {tool}（{title}）")
        w("")
        w("```")
        w(cmd_help(tool))
        w("```")
        w("")
    # 其余非 qy 前缀的 bin（mount/umount/kill/dmesg 等 util-linux 工具封装）
    # 也要有章节——测试会核对 bin/ 下每个文件都有 "### <name>"。
    # 之前这里是硬编码清单，新增工具就会漏——改为扫描 bin/ 补全。
    for b in sorted(p.name for p in (ROOT / "bin").glob("*") if p.is_file()):
        if b.startswith("qy"):
            continue
        w(f"### {b}（系统工具）")
        w("")
        w("```")
        w(strip_ansi(sh(f"./bin/{b}", "--help"))[:2000])
        w("```")
        w("")
    w("---")
    w("")
    w("本手册由 `scripts/gen_docs.py` 从代码实际定义生成。")
    w("改了命令行选项或配方字段后重跑一次即可，不会与代码脱节。")
    w("")
    return "\n".join(L)


def verify_docs(text: str) -> list:
    """校验文档里写到的命令真的存在。

    脱节的文档比没有文档更危险：用户照着敲，命令不存在，
    第一反应是"这个系统有问题"而不是"文档过期了"。
    这里把文档里出现的每个 ./bin/xxx 和 qyxxx subcmd 都实际查一遍。
    """
    import re
    problems = []
    bins = {b.name for b in (ROOT / "bin").glob("*") if b.is_file()}
    for m in set(re.findall(r"\./bin/([a-z0-9_-]+)", text)):
        if m not in bins:
            problems.append(f"文档引用了不存在的命令 ./bin/{m}"
                            f"（实际有：{' '.join(sorted(bins))}）")
    # qyxxx subcmd
    used = set(re.findall(r"\b(qy[a-z]+)\s+([a-z][a-z0-9_-]*)", text))
    for tool, sub in sorted(used):
        exe = ROOT / "bin" / tool
        if not exe.exists():
            problems.append(f"文档引用了不存在的命令 {tool}")
            continue
        help_txt = strip_ansi(sh(str(exe), "--help"))
        # 只有真正用了子命令的工具才校验子命令。
        # qybuild zlib 里的 zlib 是位置参数（包名），不是子命令——
        # 不加这个判断会对正常的示例报一堆假错，
        # 假错比漏检更糟：人会对校验结果免疫，然后真的错被忽略。
        import re as _re
        # 只能取"位置参数"那组。--bump {major,minor,...} 也是 {...}，
        # 但它是选项的取值，不是子命令——取错了会对正常示例报一堆假错。
        # 判据：这个 { 前面紧邻的 token 不是 - 开头的选项。
        choices = set()
        for m in _re.finditer(r"(\S+)?\s*\{([a-zA-Z0-9_,\-\|]+)\}", help_txt):
            prev = m.group(1)
            if prev and prev.startswith("-"):
                continue
            for c in m.group(2).split(","):
                choices.update(c.split("|"))
        if not choices:
            continue                      # 无子命令，跳过
        if sub not in choices:
            problems.append(f"{tool} 没有 {sub} 这个子命令"
                            f"（可用：{' '.join(sorted(choices))}）")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    files = {
        "用户手册.md": gen_user_manual(),
        "维护者手册.md": gen_maintainer_manual(),
    }
    bad = 0
    for name, text in files.items():
        probs = verify_docs(text)
        if probs:
            bad += 1
            print(f"!! {name} 里有 {len(probs)} 处命令与代码不符：")
            for x in probs:
                print(f"   - {x}")
        p = OUT / name
        p.write_text(text)
        print(f"已生成 docs/{name}（{len(text.splitlines())} 行）")
    if bad:
        print("\n命令校验未通过：文档里写到的命令必须在代码里真实存在，"
              "否则用户照着操作会直接踩坑。")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
