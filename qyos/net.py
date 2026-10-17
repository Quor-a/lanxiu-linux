"""网络服务与端口：服务端口分配、远程连接、共享存储、文件传输、穿透。

这块解决的是"装完包之后，服务怎么对外提供能力"。没有它，
装了 openssh 也没有 sshd_config，装了 nfs-utils 也没有 /etc/exports，
用户拿到一个装了软件但不能用服务的系统。

几个必须做对的点：

**端口必须集中分配，不能各写各的**
两个服务抢同一个端口，症状是"第二个起不来"，而日志常常只说
"address already in use"，看不出是谁占了。集中登记后能直接指出冲突方。

**特权端口（<1024）要显式标注**
它们需要 root 或 CAP_NET_BIND_SERVICE。不标注的话，
以普通用户启动会失败，而错误信息往往看不出是权限问题。

**监听地址默认不能是 0.0.0.0**
把数据库、缓存这类服务默认暴露到所有网卡是常见的安全事故。
生成器默认只监听本机，要对外必须显式声明，并说明风险。

**共享存储导出必须检查路径是否真的存在**
导出一个不存在的路径，客户端挂载时报的是"权限被拒绝"或
"没有该文件或目录"，看不出是服务端配置错了——排查方向会完全跑偏。

**穿透不等于开放端口**
内网穿透把内部服务暴露到公网，必须显式声明并提示风险。
静默暴露一个没鉴权的服务，等于直接把机器交给别人。
"""
from __future__ import annotations

import ipaddress
import json
from dataclasses import dataclass, field
from pathlib import Path

from . import util

# 众所周知端口，避免误分配
WELL_KNOWN = {
    22: "ssh", 25: "smtp", 53: "dns", 80: "http", 110: "pop3",
    143: "imap", 443: "https", 465: "smtps", 587: "submission",
    993: "imaps", 995: "pop3s",
    2049: "nfs", 3306: "mysql", 5432: "postgresql", 6379: "redis",
    8080: "http-alt", 27017: "mongodb",
}

PRIVILEGED_MAX = 1023     # <=1023 需要特权
DYNAMIC_START = 49152     # 动态/私有端口起点


class NetError(RuntimeError):
    pass


@dataclass
class Port:
    """一个服务的端口声明。"""
    service: str
    port: int
    proto: str = "tcp"        # tcp / udp
    bind: str = "127.0.0.1"   # 监听地址
    desc: str = ""

    @property
    def privileged(self) -> bool:
        return self.port <= PRIVILEGED_MAX

    @property
    def exposed(self) -> bool:
        """是否监听在所有网卡上。"""
        return self.bind in ("0.0.0.0", "::", "*", "")

    def describe(self) -> str:
        marks = []
        if self.privileged:
            marks.append("需特权")
        if self.exposed:
            marks.append("对外暴露")
        tag = f"  [{'、'.join(marks)}]" if marks else ""
        return (f"  {self.port:<6}{self.proto:<5}{self.bind:<16}"
                f"{self.service}{tag}")


def port_db_path(root: Path) -> Path:
    return Path(root) / "etc" / "qynet" / "ports.json"


def load_ports(root: Path) -> list:
    p = port_db_path(root)
    if not p.exists():
        return []
    try:
        return [Port(**d) for d in json.loads(p.read_text())["ports"]]
    except Exception:
        return []


def save_ports(root: Path, ports: list) -> None:
    p = port_db_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    util.atomic_write(p, json.dumps(
        {"ports": [x.__dict__ for x in ports]},
        ensure_ascii=False, indent=1).encode())


def register_port(root: Path, p: Port) -> list:
    """登记一个端口，冲突时报错并指出是谁占了。"""
    ports = load_ports(root)
    for e in ports:
        if e.port == p.port and e.proto == p.proto and e.bind == p.bind:
            raise NetError(
                f"端口冲突：{p.port}/{p.proto} 已被 {e.service} 占用"
                f"（{e.desc or '无说明'}）")
    if p.port in WELL_KNOWN and WELL_KNOWN[p.port] != p.service:
        util.log("warn",
                 f"{p.port} 是 {WELL_KNOWN[p.port]} 的众所周知端口，"
                 f"分配给 {p.service} 会让客户端连错服务")
    ports.append(p)
    save_ports(root, ports)
    return ports


def suggest_port(root: Path, proto: str = "tcp") -> int:
    """在动态端口范围内建议一个未占用的端口。

    不从 1024 开始扫：那个范围被各种服务占着，扫出来的"空闲"
    随时会被别的包占用。动态范围是专门留给临时分配的。
    """
    used = {e.port for e in load_ports(root) if e.proto == proto}
    n = DYNAMIC_START
    while n in used:
        n += 1
    return n


def port_report(root: Path) -> str:
    ports = load_ports(root)
    if not ports:
        return "没有登记任何端口。"
    L = ["已登记的端口："]
    for p in sorted(ports, key=lambda x: x.port):
        L.append(p.describe())
    exposed = [p for p in ports if p.exposed]
    if exposed:
        L.append("")
        L.append(f"{len(exposed)} 个服务监听在所有网卡上，"
                 f"确认这些是真正需要对外的：")
        for p in exposed:
            L.append(f"  {p.port}/{p.proto}  {p.service}")
    return "\n".join(L)


def check_ports(root: Path) -> list:
    """检查端口配置的问题。"""
    ports = load_ports(root)
    problems = []
    seen = {}
    for p in ports:
        key = (p.port, p.proto, p.bind)
        if key in seen:
            problems.append(
                f"端口冲突：{p.port}/{p.proto} 同时被 "
                f"{seen[key]} 和 {p.service} 声明")
        seen[key] = p.service
        if p.privileged and p.bind == "127.0.0.1":
            # 特权端口 + 只听本机：常见于本地代理，不算错，仅提示
            pass
        try:
            if p.bind not in ("0.0.0.0", "::", "*"):
                ipaddress.ip_address(p.bind)
        except ValueError:
            problems.append(f"{p.service}: 监听地址不是合法 IP: {p.bind}")
    return problems


# ---------------------------------------------------------------- 远程连接

def sshd_config(root: Path, port: int = 22,
                allow_root: bool = False,
                password_auth: bool = False) -> str:
    """生成 sshd 配置。默认只许密钥登录、禁止 root 密码登录。

    允许 root 用密码登录等于把机器交给任何一个能猜出密码的人。
    要开必须显式声明。
    """
    L = [
        "# 由LANXIU Linux 生成",
        f"Port {port}",
        "AddressFamily inet",
        "ListenAddress 0.0.0.0",
        "",
        "# 认证：默认只允许密钥。密码登录会被暴力破解，",
        "# 要开必须显式声明 password_auth=True",
        f"PasswordAuthentication {'yes' if password_auth else 'no'}",
        f"PermitRootLogin {'yes' if allow_root else 'prohibit-password'}",
        "PubkeyAuthentication yes",
        "PermitEmptyPasswords no",
        "MaxAuthTries 3",
        "LoginGraceTime 30",
        "",
        "# 加固",
        "X11Forwarding no",
        "AllowAgentForwarding no",
        "PermitTunnel no",
        "ClientAliveInterval 300",
        "ClientAliveCountMax 2",
        "",
        "Subsystem sftp /usr/lib/openssh/sftp-server",
    ]
    if not password_auth:
        L.insert(9, "#  开启方法：qynet ssh --password-auth")
    return "\n".join(L) + "\n"


def sshd_check(cfg_text: str) -> list:
    """检查 sshd 配置里危险的项。返回警告列表。"""
    warns = []
    for line in cfg_text.splitlines():
        s = line.strip()
        if s.startswith("#") or not s:
            continue
        parts = s.split(None, 1)
        if len(parts) != 2:
            continue
        k, v = parts[0].lower(), parts[1].strip().lower()
        if k == "permitrootlogin" and v == "yes":
            warns.append("允许 root 直接登录：一旦密码泄露整机失守，"
                         "建议 prohibit-password 配合密钥")
        if k == "passwordauthentication" and v == "yes":
            warns.append("允许密码登录：会持续被暴力破解，"
                         "建议改用密钥")
        if k == "permitemptypasswords" and v == "yes":
            warns.append("允许空密码：任何人都能直接登录")
        if k == "port" and v.isdigit() and int(v) > 65535:
            warns.append(f"端口号非法: {v}")
    return warns


# ---------------------------------------------------------------- 共享存储

@dataclass
class Export:
    """一个共享存储导出项。"""
    path: str
    clients: str = "*"          # NFS 客户端，Samba 里表示可访问范围
    options: str = "rw,sync,no_subtree_check"
    proto: str = "nfs"          # nfs / samba
    desc: str = ""


def nfs_exports(root: Path, exports: list) -> str:
    """生成 /etc/exports。"""
    L = ["# 由LANXIU Linux 生成"]
    for e in exports:
        L.append(f"{e.path}\t{e.clients}({e.options})")
    return "\n".join(L) + "\n"


def check_exports(root: Path, exports: list) -> list:
    """检查共享导出。

    导出不存在的路径是最难排查的一类错误：客户端挂载时报的是
    "权限被拒绝"或"没有该文件或目录"，完全看不出是服务端配置错了，
    排查方向会整个跑偏。
    """
    problems = []
    for e in exports:
        if not e.path.startswith("/"):
            problems.append(f"{e.path}: 必须是绝对路径")
            continue
        real = Path(root) / e.path.lstrip("/")
        if root != Path("/") and not real.exists():
            problems.append(
                f"{e.path}: 导出路径不存在（客户端会报权限被拒绝，"
                f"但真正原因是服务端路径错了）")
        if e.clients == "*":
            problems.append(
                f"{e.path}: 对所有客户端开放（{e.options}）——"
                f"应限定网段，例如 192.168.1.0/24")
        if "no_root_squash" in e.options:
            problems.append(
                f"{e.path}: 开了 no_root_squash，客户端的 root "
                f"等于本机 root。只有在确实需要时才开")
    return problems


# ---------------------------------------------------------------- 文件传输

def vsftpd_config(root: Path, anonymous: bool = False,
                  local_enable: bool = True,
                  chroot: bool = True, pasv_ports: tuple = (30000, 31000)) -> str:
    """生成 vsftpd 配置。

    匿名访问默认关闭：开了之后任何人都能往机器上放文件，
    而 FTP 是明文协议，凭据在链路上能被直接看到。
    """
    L = [
        "# 由LANXIU Linux 生成",
        "listen=YES",
        "listen_ipv6=NO",
        f"anonymous_enable={'YES' if anonymous else 'NO'}",
        f"local_enable={'YES' if local_enable else 'NO'}",
        "write_enable=YES",
        "dirmessage_enable=YES",
        "xferlog_enable=YES",
        "connect_from_port_20=YES",
        "",
        "# 把用户限制在自己的家目录里，否则能遍历整个文件系统",
        f"chroot_local_user={'YES' if chroot else 'NO'}",
        "allow_writeable_chroot=YES",
        "",
        "# 被动模式端口范围。不固定的话防火墙只能全开，",
        "# 而全开等于把一万个端口暴露出去",
        f"pasv_min_port={pasv_ports[0]}",
        f"pasv_max_port={pasv_ports[1]}",
        "",
        "# FTP 是明文协议，凭据能被直接看到。能用 SFTP 就别用 FTP",
        "ssl_enable=NO",
    ]
    return "\n".join(L) + "\n"


def ftp_check(cfg_text: str) -> list:
    warns = []
    for line in cfg_text.splitlines():
        s = line.strip()
        if s.startswith("#") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        k, v = k.strip().lower(), v.strip().lower()
        if k == "anonymous_enable" and v == "yes":
            warns.append("开了匿名访问：任何人都能上传文件到这台机器")
        if k == "ssl_enable" and v == "no":
            warns.append("未启用 TLS：FTP 明文传输，"
                         "账号密码在链路上可被直接读取")
        if k == "chroot_local_user" and v == "no":
            warns.append("未限制用户目录：登录后可遍历整个文件系统")
    return warns


# ---------------------------------------------------------------- 穿透

@dataclass
class Tunnel:
    """一条内网穿透。

    穿透是把内部服务暴露到公网，风险等级和开端口完全不同：
    开端口至少还在内网里，穿透是任何人都能直接访问。
    """
    name: str
    local_port: int
    local_addr: str = "127.0.0.1"
    public_port: int = 0
    proto: str = "tcp"
    auth: str = ""              # 鉴权方式，留空表示无鉴权
    desc: str = ""

    @property
    def has_auth(self) -> bool:
        return bool(self.auth.strip())


def check_tunnels(tunnels: list) -> list:
    warns = []
    for t in tunnels:
        # 只报无鉴权这一条。原先还报"公网端口是特权端口"，
        # 但 80/443 是 web 常态，那条几乎总是命中就变成了噪音——
        # 而噪音会让维护者连带忽略真正重要的无鉴权警告。
        # 穿透服务端的权限是部署问题，不属于这里的配置校验
        if not t.has_auth:
            warns.append(
                f"{t.name}: 穿透到公网且没有鉴权——任何人都能访问"
                f"（本地 {t.local_addr}:{t.local_port}"
                f" → 公网 {t.public_port or '自动分配'}）")
    return warns


# ---------------------------------------------------------------- 防火墙

def firewall_rules(ports: list, tunnels: list | None = None) -> str:
    """按登记的端口生成 nftables 规则。

    只放行登记过的端口：没登记却能被访问，说明有别的东西在监听，
    那正是要排查的。
    """
    L = ["#!/usr/sbin/nft -f", "# 由LANXIU Linux 生成", "",
         "table inet filter {", "  chain input {",
         "    type filter hook input priority 0; policy drop;", "",
         "    # 已建立的连接和本机回环必须放行，",
         "    # 少了这两条会把整台机器锁死",
         "    ct state established,related accept",
         "    iif lo accept", ""]
    for p in ports:
        if not p.exposed:
            continue      # 只听本机的不需要对外放行
        L.append(f"    # {p.service}: {p.desc or '无说明'}")
        L.append(f"    {p.proto} dport {p.port} accept")
    if tunnels:
        L.append("")
        L.append("    # 穿透端口")
        for t in tunnels:
            if t.public_port:
                L.append(f"    {t.proto} dport {t.public_port} accept"
                         f"  # {t.name}")
    L += ["  }", "}"]
    return "\n".join(L) + "\n"


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qynet",
                                 description="LANXIU Linux 网络服务与端口")
    ap.add_argument("--root", default=".")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="列出登记的端口")
    sp = sub.add_parser("add-port", help="登记端口")
    sp.add_argument("service"); sp.add_argument("port", type=int)
    sp.add_argument("--proto", default="tcp")
    sp.add_argument("--bind", default="127.0.0.1")
    sp.add_argument("--desc", default="")
    sub.add_parser("check", help="检查端口配置")
    sub.add_parser("firewall", help="生成防火墙规则")
    sp = sub.add_parser("ssh", help="生成 sshd 配置")
    sp.add_argument("--port", type=int, default=22)
    sp.add_argument("--allow-root", action="store_true")
    sp.add_argument("--password-auth", action="store_true")
    sp = sub.add_parser("ftp", help="生成 vsftpd 配置")
    sp.add_argument("--anonymous", action="store_true")
    sp.add_argument("--no-chroot", action="store_true")
    sub.add_parser("wifi", help="无线状态")
    sp = sub.add_parser("wifi-connect", help="连接 WiFi")
    sp.add_argument("ssid")
    sp.add_argument("--iface", default="wlan0")
    sp.add_argument("--psk", default="", help="不给则走交互式输入")
    sub.add_parser("bluetooth", help="蓝牙状态")
    sub.add_parser("wwan", help="移动网络状态")
    sp = sub.add_parser("vnic", help="创建虚拟网卡")
    sp.add_argument("name")
    sp.add_argument("kind", choices=["veth", "bridge", "tun", "dummy", "macvlan"])
    sp.add_argument("--link", default="")
    sub.add_parser("vnic-check", help="检查虚拟网卡支持")
    sp = sub.add_parser("router", help="配成路由器")
    sp.add_argument("lan"); sp.add_argument("wan")
    sp.add_argument("--subnet", default="192.168.100.0/24")
    sp = sub.add_parser("adb", help="adb 检查")
    sp = sub.add_parser("adb-pair", help="无线调试配对")
    sp.add_argument("host"); sp.add_argument("port", type=int)
    sp.add_argument("code")
    sp = sub.add_parser("adb-tcpip", help="开 TCP 调试模式")
    sp.add_argument("--port", type=int, default=5555)

    a = ap.parse_args(argv)
    root = Path(a.root)

    if a.cmd == "ports":
        print(port_report(root))
        return 0

    if a.cmd == "add-port":
        try:
            register_port(root, Port(a.service, a.port, a.proto,
                                     a.bind, a.desc))
        except NetError as e:
            # 抛栈会让用户只看到 traceback 而看不清冲突方是谁
            util.log("err", str(e))
            return 1
        util.log("ok", f"已登记 {a.port}/{a.proto} → {a.service}")
        return 0

    if a.cmd == "check":
        problems = check_ports(root)
        for x in problems:
            util.log("err", x)
        if problems:
            return 1
        util.log("ok", "端口配置无冲突")
        return 0

    if a.cmd == "firewall":
        print(firewall_rules(load_ports(root)))
        return 0

    if a.cmd == "ssh":
        cfg = sshd_config(root, a.port, a.allow_root, a.password_auth)
        print(cfg, end="")
        for w in sshd_check(cfg):
            util.log("warn", w)
        return 0

    if a.cmd == "wifi":
        print(wifi_report(root))
        for x in check_vnic_support():
            util.log("warn", x)
        return 0

    if a.cmd == "wifi-connect":
        print(wifi_connect_cmd(a.ssid, a.psk, a.iface))
        return 0

    if a.cmd == "bluetooth":
        print(bluetooth_report(root))
        return 0

    if a.cmd == "wwan":
        print(wwan_report(root))
        return 0

    if a.cmd == "vnic":
        for c in vnic_commands(Vnic(a.name, a.kind, a.link)):
            print(c)
        return 0

    if a.cmd == "vnic-check":
        problems = check_vnic_support()
        for x in problems:
            util.log("err", x)
        if not problems:
            util.log("ok", "虚拟网卡支持正常")
        return 1 if problems else 0

    if a.cmd == "router":
        problems = check_router_ready(a.lan, a.wan)
        for x in problems:
            util.log("warn", x)
        print("\n".join(router_setup(a.lan, a.wan, a.subnet)))
        return 0

    if a.cmd == "adb":
        problems = adb_check()
        for x in problems:
            util.log("err", x)
        if not problems:
            util.log("ok", "adb 可用")
        return 1 if problems else 0

    if a.cmd == "adb-pair":
        print(adb_pair_cmd(a.host, a.port, a.code))
        return 0

    if a.cmd == "adb-tcpip":
        print(adb_tcpip_cmd(a.port))
        return 0

    if a.cmd == "ftp":
        cfg = vsftpd_config(root, anonymous=a.anonymous,
                            chroot=not a.no_chroot)
        print(cfg, end="")
        for w in ftp_check(cfg):
            util.log("warn", w)
        return 0
    return 1

# ---------------------------------------------------------------- 无线

def wifi_report(root: Path) -> str:
    """无线网络状态报告。

    无线故障排查的第一件事是看有没有固件、有没有驱动。
    有设备但没固件，表现是"搜不到任何 WiFi"——
    用户会以为是路由器问题，其实是固件没装。
    """
    L = ["无线状态：", ""]
    # 无线设备是否存在
    devs = []
    p = Path("/sys/class/net")
    if p.exists():
        for d in sorted(p.iterdir()):
            if (d / "wireless").exists() or (d / "phy80211").exists():
                devs.append(d.name)
    if not devs:
        L.append("  ! 没有检测到无线网卡")
        L.append("    可能原因：没装 linux-firmware（最常见）、"
                 "网卡被硬开关关掉、内核没开对应驱动")
        L.append("    检查：qyhw scan")
    else:
        L.append(f"  无线接口: {'、'.join(devs)}")
    #  supplicant 是否有
    import shutil
    for exe, name in (("wpa_supplicant", "wpa_supplicant"), ("iwd", "iwd")):
        if shutil.which(exe):
            L.append(f"  认证服务: {name} 已安装")
            break
    else:
        L.append("  ! 没有安装无线认证服务（wpa_supplicant 或 iwd）")
        L.append("    没有它，即使搜到 WiFi 也连不上")
    # 法规域
    reg = Path("/etc/conf.d/wireless-regdom")
    if reg.exists():
        L.append(f"  法规域: {reg.read_text().strip()}")
    else:
        L.append("  ! 没有设置无线法规域——内核会用最保守的世界域，")
        L.append("    结果是 5GHz 大量信道不可用，网速明显偏低且不报错")
    return "\n".join(L)


def wifi_connect_cmd(ssid: str, psk: str = "",
                     iface: str = "wlan0") -> str:
    """给出连接 WiFi 的命令。

    明文密码不写进命令行历史——走交互式输入或临时文件。
    把密码放在命令行上，同机任何用户 ps 一下就能看到。
    """
    if psk:
        return ("# 不要把密码写在命令行上：ps 就能看到\n"
                "wpa_passphrase \"{}\" | tee /tmp/wpa.conf\n"
                "  # 交互式输入密码\n"
                "wpa_supplicant -B -i {} -c /tmp/wpa.conf\n"
                "rm -f /tmp/wpa.conf   # 用完立刻删").format(ssid, iface)
    return (f"iw dev {iface} scan | grep SSID        # 先确认能搜到\n"
            f"wpa_cli -i {iface} add_network\n"
            f"wpa_cli -i {iface} set_network 0 ssid '\"{ssid}\"'")


# ---------------------------------------------------------------- 蓝牙

def bluetooth_report(root: Path) -> str:
    L = ["蓝牙状态：", ""]
    import shutil
    if not shutil.which("bluetoothctl"):
        L.append("  ! 没有安装 bluez——蓝牙完全不可用")
        L.append("    执行 qypkg install bluez")
        return "\n".join(L)
    p = Path("/sys/class/bluetooth")
    if p.exists() and any(p.iterdir()):
        L.append(f"  适配器: {'、'.join(x.name for x in sorted(p.iterdir()))}")
    else:
        L.append("  ! 没有检测到蓝牙适配器")
        L.append("    笔记本上常见原因：功能键把它关了，"
                 "或内核没开 CONFIG_BT")
    L.append("")
    L.append("  配对：bluetoothctl → scan on → pair <MAC> → trust <MAC>")
    return "\n".join(L)


# ---------------------------------------------------------------- 移动网络

def wwan_report(root: Path) -> str:
    """移动网络（4G/5G 模块）状态。

    移动网络模块多数走 USB 或 PCIe，需要 ModemManager + 固件。
    没有固件的模块在 mmcli 里根本不出现——
    用户会以为"没插卡"，实际是固件没装。
    """
    L = ["移动网络：", ""]
    import shutil
    if not shutil.which("mmcli"):
        L.append("  ! 没有安装 ModemManager")
        return "\n".join(L)
    L.append("  调制解调器：mmcli -L")
    L.append("  若列表为空：可能是模块缺固件（qyhw scan 检查），")
    L.append("  或模块处于飞行模式（部分笔记本有硬件开关）")
    return "\n".join(L)


# ---------------------------------------------------------------- 虚拟网卡

@dataclass
class Vnic:
    """一个虚拟网卡。

    容器、虚拟机、VPN 都依赖它。
    没有 CONFIG_TUN 或 bridge 模块，这些都起不来，
    而报错往往是"设备不存在"，看不出是内核没开。
    """
    name: str
    kind: str              # veth / bridge / tun / dummy / macvlan
    link: str = ""         # veth 的对端，bridge 的从属
    mtu: int = 1500
    desc: str = ""


def vnic_commands(v: Vnic) -> list:
    """生成创建虚拟网卡的命令。"""
    n, k = v.name, v.kind
    if k == "veth":
        peer = v.link or (n + "-peer")
        return [f"ip link add {n} type veth peer name {peer}",
                f"ip link set {n} up", f"ip link set {peer} up"]
    if k == "bridge":
        cmds = [f"ip link add {n} type bridge", f"ip link set {n} up"]
        if v.link:
            cmds.append(f"ip link set {v.link} master {n}")
        return cmds
    if k == "tun":
        # TUN 由程序创建，这里只给权限检查
        return [f"# {n} 由用户态程序（VPN/代理）通过 /dev/net/tun 创建",
                "ls -l /dev/net/tun || echo '缺 CONFIG_TUN，VPN 无法工作'"]
    if k == "dummy":
        return [f"ip link add {n} type dummy", f"ip link set {n} up"]
    if k == "macvlan":
        parent = v.link or "eth0"
        return [f"ip link add {n} link {parent} type macvlan mode bridge",
                f"ip link set {n} up"]
    raise NetError(f"不支持的虚拟网卡类型 {k}")


def check_vnic_support() -> list:
    """检查内核是否支持虚拟网卡。返回缺失项。"""
    problems = []
    if not Path("/dev/net/tun").exists():
        problems.append("/dev/net/tun 不存在——缺 CONFIG_TUN，"
                        "VPN 与所有基于 TUN 的代理都无法工作")
    return problems


# ---------------------------------------------------------------- 路由

def router_setup(lan: str, wan: str,
                 subnet: str = "192.168.100.0/24") -> list:
    """把本机配成路由器（LAN 口共享 WAN 上网）。

    顺序不能乱：
    1. 开转发（不开的话包根本不会被路由）
    2. 开 NAT（不开的话内网包出得去回不来）
    3. 开 DHCP（给内网设备发地址）
    """
    return [
        "# 1. 开转发。不做这一步后面全都不生效，",
        "#    而且没有任何报错——包就是默默地被丢掉",
        "sysctl -w net.ipv4.ip_forward=1",
        f"echo 'net.ipv4.ip_forward=1' > /etc/sysctl.d/30-router.conf",
        "",
        "# 2. NAT。内网地址在公网上不可路由，",
        "#    不做这步内网设备出得去但回不来",
        f"nft add table nat",
        f"nft add chain nat postrouting {{ type nat hook postrouting "
        f"priority 100 \; }}",
        f"nft add rule nat postrouting oifname \"{wan}\" masquerade",
        "",
        "# 3. 给 LAN 口配地址",
        f"ip addr add {subnet.replace('0/24', '1/24')} dev {lan}",
        f"ip link set {lan} up",
        "",
        "# 4. DHCP（需要 dnsmasq 或类似）",
        f"# dnsmasq --interface={lan} --dhcp-range="
        f"{subnet.replace('0/24', '100')},{subnet.replace('0/24', '250')},12h",
    ]


def check_router_ready(lan: str, wan: str) -> list:
    """配路由器前的检查。"""
    problems = []
    p = Path("/proc/sys/net/ipv4/ip_forward")
    if p.exists() and p.read_text().strip() == "0":
        problems.append("IP 转发未开启——包不会被路由，且不报错")
    for iface in (lan, wan):
        if not (Path("/sys/class/net") / iface).exists():
            problems.append(f"接口 {iface} 不存在")
    return problems


# ---------------------------------------------------------------- ADB

def adb_pair_cmd(host: str, port: int, code: str) -> str:
    """安卓 11+ 无线调试配对。

    无线调试比 USB 更安全：不暴露 USB 调试给所有连过的电脑，
    配对码一次有效。
    """
    return (f"adb pair {host}:{port} {code}\n"
            f"adb connect {host}:{port}\n"
            f"adb devices      # 确认出现 device 而不是 unauthorized")


def adb_tcpip_cmd(usb_port: int = 5555) -> str:
    """先用数据线开一次 TCP 模式，之后就能拔线无线调试。"""
    return (f"adb tcpip {usb_port}      # 需先用数据线连一次\n"
            f"adb connect <手机IP>:{usb_port}\n"
            f"# 之后可拔掉数据线")


def adb_check() -> list:
    """检查 adb 是否可用及常见问题。"""
    import shutil
    problems = []
    if not shutil.which("adb"):
        return ["没有安装 android-tools（adb）——"
                "无法调试或刷机安卓设备。执行 qypkg install android-tools"]
    p = Path("/dev/bus/usb")
    if p.exists() and not any(p.iterdir()):
        problems.append("USB 总线上没有设备——检查数据线与手机是否处于"
                        "文件传输模式（仅充电模式不暴露 adb）")
    return problems
