"""pam.d 配置集生成。

PAM 装上一堆 .so 但不给配置，等于装了个不工作的库——
程序调 pam_start() 会找不到对应 service 的配置，
然后按默认策略失败，表现为"密码明明对了却登不进去"。

Ubuntu 在 /etc/pam.d/ 下有一整套按服务分的文件。
我们生成最小可用集，每项都对应一类真实故障。

关键：必须有 other 这个兜底文件。
没匹配到的服务会退到 other，缺失时 PAM 直接拒绝所有认证。
"""
from __future__ import annotations

from pathlib import Path

from . import util

# 失败锁定：SSH 可以被无限次猜密码，
# 而没有任何提示告诉管理员正在被爆破
FAILLOCK_DENY = 5
FAILLOCK_TIME = 900      # 15 分钟


def _common_auth(use_faillock: bool = True) -> list:
    L = ["auth    required    pam_env.so"]
    if use_faillock:
        L += [
            f"auth    required    pam_faillock.so preauth silent deny={FAILLOCK_DENY}"
            f" unlock_time={FAILLOCK_TIME}",
        ]
    L.append("auth    required    pam_unix.so nullok")
    if use_faillock:
        L.append(
            f"auth    required    pam_faillock.so authfail deny={FAILLOCK_DENY}"
            f" unlock_time={FAILLOCK_TIME}")
    return L


COMMON_ACCOUNT = [
    "account required    pam_unix.so",
    "account required    pam_faillock.so",
]

COMMON_PASSWORD = [
    # 没有 pam_pwquality，用户可以设空口令或 "123456"
    "password required   pam_pwquality.so retry=3",
    "password required   pam_unix.so sha512 shadow try_first_pass",
]

COMMON_SESSION = [
    "session required    pam_limits.so",
    "session required    pam_env.so",
    "session required    pam_unix.so",
]


def pam_d_files(root: Path, allow_root_login: bool = True) -> dict:
    """生成 /etc/pam.d 下的配置集。返回 {文件名: 内容}。"""
    files: dict = {}

    def stack(lines: list) -> str:
        return "\n".join(lines) + "\n"

    # 兜底：没匹配到的服务退到这里。
    # 缺了它，任何没写专门配置的服务都会被直接拒绝认证，
    # 而且报错看不出是 PAM 配置缺失
    files["other"] = stack(
        _common_auth() + COMMON_ACCOUNT + COMMON_PASSWORD + COMMON_SESSION)

    # 本地登录
    files["login"] = stack(
        _common_auth() + COMMON_ACCOUNT + COMMON_PASSWORD + COMMON_SESSION)

    # su：切换用户。没有它 su 直接不能用
    files["su"] = stack(
        _common_auth() + COMMON_ACCOUNT + COMMON_PASSWORD + COMMON_SESSION)

    # 屏幕锁、图形登录等走 system-auth
    files["system-auth"] = stack(
        _common_auth() + COMMON_ACCOUNT + COMMON_PASSWORD + COMMON_SESSION)

    # passwd 改口令（单独一条，不带 faillock——
    # 改口令时触发失败锁定会让用户锁死自己）
    files["passwd"] = stack(
        ["auth    required    pam_unix.so"]
        + COMMON_ACCOUNT + COMMON_PASSWORD + COMMON_SESSION)

    return files


def write_pam_d(root: Path, allow_root_login: bool = True) -> list:
    """写入 /etc/pam.d。返回写入的文件列表。"""
    d = Path(root) / "etc" / "pam.d"
    d.mkdir(parents=True, exist_ok=True)
    written = []
    for name, content in pam_d_files(root, allow_root_login).items():
        p = d / name
        util.atomic_write(p, content.encode())
        written.append(str(p))
    return written


def check_pam_d(root: Path) -> list:
    """检查 PAM 配置的问题。返回问题列表。"""
    d = Path(root) / "etc" / "pam.d"
    problems = []
    if not d.exists():
        return ["没有 /etc/pam.d——装了 PAM 库但没配置，"
                "所有认证都会失败（表现为密码对了也登不进去）"]
    have = {p.name for p in d.iterdir() if p.is_file()}

    # other 是兜底，缺了会拒绝所有没专门配置的服务
    if "other" not in have:
        problems.append("缺 /etc/pam.d/other："
                        "没有专门配置的服务会被直接拒绝认证")
    for need in ("login", "su", "system-auth", "passwd"):
        if need not in have:
            problems.append(f"缺 /etc/pam.d/{need}")

    # 口令强度：没有 pwquality 就没有强度检查
    pw = d / "passwd"
    if pw.exists():
        try:
            t = pw.read_text()
            if "pam_pwquality" not in t:
                problems.append(
                    "/etc/pam.d/passwd 没有 pam_pwquality："
                    "用户可以设空口令或 123456")
        except OSError:
            pass

    # 资源限制
    sa = d / "system-auth"
    if sa.exists():
        try:
            t = sa.read_text()
            if "pam_limits" not in t:
                problems.append(
                    "/etc/pam.d/system-auth 没有 pam_limits："
                    "ulimit 不生效，一个 fork 炸弹能拖死机器")
        except OSError:
            pass
    return problems


def pam_report(root: Path) -> str:
    d = Path(root) / "etc" / "pam.d"
    if not d.exists():
        return "没有 /etc/pam.d（PAM 装了也不会工作）"
    L = [f"/etc/pam.d 下的配置（{len(list(d.iterdir()))} 个）："]
    for p in sorted(d.iterdir()):
        if p.is_file():
            L.append(f"  {p.name}")
    problems = check_pam_d(root)
    if problems:
        L.append("")
        L.append("问题：")
        for x in problems:
            L.append(f"  ! {x}")
    else:
        L.append("\n配置完整。")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qypam",
                                 description="LANXIU Linux PAM 配置")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show", help="查看 /etc/pam.d")
    sub.add_parser("check", help="检查配置问题")
    sub.add_parser("apply", help="生成/覆盖配置集")
    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "show":
        print(pam_report(root))
        return 0
    if a.cmd == "check":
        problems = check_pam_d(root)
        for x in problems:
            util.log("err", x)
        if not problems:
            util.log("ok", "PAM 配置完整")
        return 1 if problems else 0
    if a.cmd == "apply":
        w = write_pam_d(root)
        util.log("ok", f"已写入 {len(w)} 个配置文件")
        for x in w:
            util.log("info", f"  {x}")
        return 0
    return 1
