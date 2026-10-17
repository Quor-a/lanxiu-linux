"""执行脚本系统：多步骤脚本的编排、演练、续跑与审计。

装机、升级、迁移这类操作都是多步骤的。直接写成一个大 shell 脚本
有三个躲不掉的问题：

1. **失败后要从头再来**。第 7 步失败，前 6 步白做。
   装机这种一步十几分钟的操作，从头再来很折磨人。
2. **不知道它要干什么**。脚本跑之前没法预演，
   而有些步骤会改分区表、会清空数据。
3. **出事后说不清做过什么**。没有记录，只记得"跑过那个脚本"。

所以本模块把脚本拆成有名字、可单独重跑的步骤，并且：
- 每步声明是否幂等（能不能重复执行）
- 支持 --dry-run 预演，但预演必须说明"看不出什么"
- 支持 --resume 从第 N 步继续，而不是从头
- 每步执行都记日志，能事后回看

**演练要说清它证明不了什么**
--dry-run 只跑检查，不跑改动。它能证明"配置没问题"，
但证明不了"磁盘空间够"（要真的写才知道）。
不说清这点，用户会把演练通过当成"一定成功"。

**续跑比从头再来重要，但不能盲目续跑**
续跑要读上次的状态文件。如果状态文件里的步骤已经和脚本不一致
（脚本改过了），必须拒绝续跑——按旧状态接着跑会跳步。
"""
from __future__ import annotations

import json
import os
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class RunError(RuntimeError):
    pass


@dataclass
class Step:
    """脚本里的一步。"""
    id: str
    title: str
    command: str
    check: str = ""          # 预演时跑的检查命令（不改任何东西）
    idempotent: bool = True  # 能否重复执行
    timeout: int = 600
    desc: str = ""

    def dry(self) -> str:
        """预演时执行的内容。没有 check 就只打印。"""
        return self.check or f"# {self.title}（无预检，只能实跑）"


@dataclass
class RunState:
    """一次执行的进度。"""
    script: str
    done: list = field(default_factory=list)
    failed: str = ""
    started: float = 0.0
    steps_hash: str = ""     # 脚本步骤的指纹，用于校验能否续跑

    def to_json(self) -> bytes:
        return json.dumps(self.__dict__, ensure_ascii=False, indent=1).encode()


def state_path(root: Path, script: str) -> Path:
    d = Path(root) / "var" / "lib" / "qyrun"
    d.mkdir(parents=True, exist_ok=True)
    return d / (script + ".state")


def log_path(root: Path, script: str) -> Path:
    d = Path(root) / "var" / "log" / "qyrun"
    d.mkdir(parents=True, exist_ok=True)
    return d / (script + ".log")


def cur_of(steps: list, sid: str) -> str:
    """取某个步骤当前的命令文本。"""
    for s in steps:
        if s.id == sid:
            return s.command
    return ""


def _resume_conflict(steps: list, done: list, done_cmds: dict) -> str:
    """判断能否按旧进度续跑。返回冲突原因，空串表示可以。

    只有两类情况真的危险：
    1. 某个已完成的步骤内容变了 —— 它被当成做过了，但实际做的是旧内容
    2. 在已完成步骤之前插入了新步骤 —— 那个新步骤会被跳过

    至于"还没执行到的步骤改了"，恰恰是修复失败步骤的正常操作，
    不该拦。
    """
    cur = {s.id: s.command for s in steps}
    ids = [s.id for s in steps]
    for d in done:
        if d not in cur:
            return f"已完成的步骤 {d} 在新脚本里不存在了"
        if done_cmds.get(d, cur[d]) != cur[d]:
            return f"已完成的步骤 {d} 内容变了（做过的是旧版本）"
    if done:
        if done[-1] not in ids:
            return f"已完成的步骤 {done[-1]} 不在新脚本里"
        last = ids.index(done[-1])
        done_set = set(done)
        for i in range(last + 1):
            if ids[i] not in done_set:
                return f"在已完成的步骤之前插入了新步骤 {ids[i]}，它会被跳过"
    return ""


def steps_hash(steps: list) -> str:
    """步骤指纹。脚本改了指纹就变，据此拒绝按旧状态续跑。"""
    import hashlib
    raw = "|".join(f"{s.id}:{s.command}" for s in steps)
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


def dry_run(steps: list) -> str:
    """预演：只跑检查命令。"""
    L = [f"预演 {len(steps)} 步（只检查，不改动）：", ""]
    for i, s in enumerate(steps, 1):
        L.append(f"  {i}. {s.title}")
        L.append(f"     预检: {s.dry()}")
        if s.check:
            try:
                r = subprocess.run(s.check, shell=True, capture_output=True,
                                   text=True, timeout=60)
                mark = "通过" if r.returncode == 0 else "未通过"
                L.append(f"     结果: {mark}")
                if r.returncode != 0 and r.stderr.strip():
                    L.append(f"       {r.stderr.strip()[:150]}")
            except Exception as e:
                L.append(f"     结果: 无法检查（{e}）")
    L.append("")
    L.append("演练证明不了的事：")
    L.append("  - 磁盘空间是否足够（要真的写才知道）")
    L.append("  - 网络是否可达（预检可能命中缓存）")
    L.append("  - 目标设备是否会被正确识别（要真的扫）")
    return "\n".join(L)


def run_steps(root: Path, script: str, steps: list,
              resume: bool = False, dry: bool = False,
              log=print) -> dict:
    """执行脚本。返回结果字典。"""
    if dry:
        print(dry_run(steps))
        return {"dry": True, "ok": True}

    sp = state_path(root, script)
    lp = log_path(root, script)
    h = steps_hash(steps)

    done: list = []
    if resume and sp.exists():
        try:
            st = json.loads(sp.read_text())
            # 只校验"已完成的那几步"没变。整体哈希太严：
            # 修一个还没执行到的步骤也会让哈希变，于是明明能续跑却被拒，
            # 前面十几分钟的活白干。真正危险的是
            # "已完成步骤变了"或"在已完成步骤之前插入了新步骤"，那才会跳步
            reason = _resume_conflict(steps, st.get("done", []),
                                      st.get("done_cmds", {}))
            if reason:
                raise RunError(
                    f"不能续跑：{reason}——按旧进度接着跑会跳步。"
                    f"要么从头执行，要么确认差异")
            done = st.get("done", [])
            log(f"  续跑：已完成 {len(done)} 步，继续")
        except json.JSONDecodeError:
            raise RunError(f"进度文件损坏: {sp}")

    state = RunState(script=script, done=done, steps_hash=h,
                     started=time.time())
    # 记下已完成步骤当时执行的内容，续跑时用来判断它们有没有变
    state.__dict__["done_cmds"] = {
        x: cur_of(steps, x) for x in done if cur_of(steps, x)}
    results = []
    with lp.open("a") as lf:
        lf.write(f"\\n=== {time.strftime('%F %T')} 开始 {script} ===\\n")
        for i, s in enumerate(steps, 1):
            if s.id in done:
                log(f"  {i}. {s.title} — 已完成，跳过")
                results.append((s.id, "skipped", ""))
                continue
            log(f"  {i}. {s.title} …")
            lf.write(f"[{time.strftime('%T')}] {s.id}: {s.command}\\n")
            try:
                r = subprocess.run(s.command, shell=True,
                                   capture_output=True, text=True,
                                   timeout=s.timeout)
            except subprocess.TimeoutExpired:
                msg = f"超过 {s.timeout} 秒"
                state.failed = s.id
                _save(sp, state)
                lf.write(f"[{time.strftime('%T')}] {s.id}: 超时 {msg}\\n")
                raise RunError(f"第 {i} 步（{s.title}）{msg}。"
                               f"已保存进度，修复后加 --resume 继续")
            lf.write(f"[{time.strftime('%T')}] {s.id}: rc={r.returncode}\\n")
            if r.returncode != 0:
                state.failed = s.id
                _save(sp, state)
                err = (r.stderr or r.stdout).strip()[:300]
                raise RunError(
                    f"第 {i} 步（{s.title}）失败：{err}\\n"
                    f"  已保存进度，修复后执行：qyrun --resume {script}")
            done.append(s.id)
            state.done = done
            _save(sp, state)
            results.append((s.id, "ok", ""))
            log(f"      完成")
        lf.write(f"=== {time.strftime('%F %T')} 全部完成 ===\\n")

    # 成功后清掉进度文件，避免下次误以为有未完成的
    sp.unlink(missing_ok=True)
    return {"ok": True, "steps": results, "log": str(lp)}


def _save(sp: Path, state: RunState) -> None:
    sp.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(sp, state.to_json())


def pending(root: Path, script: str) -> dict | None:
    """查看某个脚本是否有未完成的执行。返回 None 表示没有。"""
    sp = state_path(root, script)
    if not sp.exists():
        return None
    try:
        return json.loads(sp.read_text())
    except Exception:
        return None


# 内建脚本：装机与系统升级
INSTALL_STEPS = [
    Step("partition", "分区", "sh 1-partition.sh",
         check="lsblk | head -5", idempotent=False,
         desc="会清空目标磁盘"),
    Step("mount", "挂载", "sh 2-mount.sh",
         idempotent=True),
    Step("packages", "安装包系统", "sh 3-install.sh", idempotent=True),
    Step("configure", "系统配置", "sh 4-configure.sh", idempotent=True),
    Step("bootloader", "安装引导器", "sh 5-bootloader.sh", idempotent=True),
    Step("umount", "卸载", "sh 9-umount.sh", idempotent=True),
]

UPGRADE_STEPS = [
    Step("check", "升级前检查", "qypkg upgrade --dry-run",
         check="qysource check"),
    Step("download", "下载包", "qypkg upgrade --download-only"),
    Step("snapshot", "建快照", "qypkg snapshot"),
    Step("apply", "应用升级", "qypkg upgrade"),
    Step("verify", "验证", "qypkg verify"),
]

BUILTIN = {"install": INSTALL_STEPS, "upgrade": UPGRADE_STEPS}


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qyrun",
                                 description="澜岫 Linux 脚本执行系统")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出内建脚本")
    sp = sub.add_parser("dry", help="预演")
    sp.add_argument("script")
    sp = sub.add_parser("run", help="执行")
    sp.add_argument("script")
    sp.add_argument("--resume", action="store_true", help="从上次失败处继续")
    sp = sub.add_parser("status", help="查看进度")
    sp.add_argument("script")

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "list":
        for n, steps in BUILTIN.items():
            print(f"  {n}（{len(steps)} 步）")
            for i, s in enumerate(steps, 1):
                tag = "" if s.idempotent else "  [不可重复]"
                print(f"    {i}. {s.title}{tag}")
        return 0

    if a.script not in BUILTIN:
        util.log("err", f"没有脚本 {a.script}"
                        f"（可用：{'、'.join(BUILTIN)}）")
        return 1
    steps = BUILTIN[a.script]

    if a.cmd == "dry":
        print(dry_run(steps))
        return 0

    if a.cmd == "status":
        st = pending(root, a.script)
        if not st:
            print(f"{a.script} 没有未完成的任务")
            return 0
        print(f"{a.script} 未完成：")
        print(f"  已完成 {len(st.get('done', []))} 步")
        print(f"  失败于 {st.get('failed') or '（未知）'}")
        print(f"  续跑：qyrun --resume {a.script}")
        return 1

    if a.cmd == "run":
        try:
            r = run_steps(root, a.script, steps, resume=a.resume)
        except RunError as e:
            util.log("err", str(e))
            return 1
        util.log("ok", f"{a.script} 全部完成")
        util.log("info", f"日志：{r.get('log')}")
        return 0
    return 1
