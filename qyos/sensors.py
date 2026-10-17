"""传感器与平台控制：加速度、陀螺仪、环境光、磁力计、气压、霍尔、温度、风扇、背光、指示灯、雷电、GPIO。

这一层的共性是：**硬件有，但没人把它的读数变成可用的信息**。

几个必须做对的点：

**1. 传感器要能自检是否可信**
一个加速度计出厂没校准，读数恒定偏 300mg——
自动旋转会一直在错误的角度。而用户看不到读数，
只看到"转屏不动"或"转屏乱转"。
所以要有静止检测：静止时读数应接近重力，偏离太多就是没校准。

**2. 传感器必须由代理统一提供，不给应用直读**
直读等于任何应用都能持续获取设备朝向——
这是隐私数据，不是硬件数据。所以走 iio-sensor-proxy，
应用只能通过 D-Bus 拿，且能看到是谁在拿。

**3. 风扇曲线不能只靠内核**
内核默认曲线往往保守（噪音大）或激进（温度高）。
没有用户态控制，笔记本要么吵要么烫。
但**写错曲线会导致过热**——所以下限必须有保护值。

**4. 背光要分键盘背光与屏幕背光**
两者 sysfs 路径不同（leds 与 backlight），
混在一起会导致改了屏幕亮度结果键盘灯变了。

**5. 雷电/USB4 有安全等级**
雷电设备能直接 DMA 访问内存。不设安全等级，
插一个恶意设备就能读走全部内存——
这是"接个外设丢数据"的真实途径。

**6. GPIO 能控制物理设备**
权限给宽了等于让别人能操作你的继电器、马达。
所以 GPIO 默认不给普通用户。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import util


class SensorError(RuntimeError):
    pass


# ---------------------------------------------------------------- 传感器类型

@dataclass
class SensorKind:
    """一类传感器。"""
    id: str
    title: str
    sysfs_type: str       # iio 里的 type 值
    channels: list        # 常见通道名
    unit: str = ""
    desc: str = ""
    # 静止时的期望值，用于自检。None 表示无法自检
    rest_expect: tuple | None = None
    privacy: bool = False   # 是否属于隐私数据


SENSOR_KINDS = [
    SensorKind("accel", "加速度计", "accel",
               ["in_accel_x_raw", "in_accel_y_raw", "in_accel_z_raw"],
               unit="m/s²",
               desc="屏幕自动旋转、自由落体检测、计步",
               rest_expect=(0, 0, 9.8),
               privacy=True),
    SensorKind("gyro", "陀螺仪", "anglvel",
               ["in_anglvel_x_raw", "in_anglvel_y_raw", "in_anglvel_z_raw"],
               unit="rad/s",
               desc="游戏、防抖、精确旋转",
               rest_expect=(0, 0, 0),
               privacy=True),
    SensorKind("light", "环境光", "illuminance",
               ["in_illuminance_raw", "in_intensity_raw"],
               unit="lux",
               desc="自动亮度",
               privacy=True),
    SensorKind("magn", "磁力计", "magn",
               ["in_magn_x_raw", "in_magn_y_raw", "in_magn_z_raw"],
               unit="µT",
               desc="电子罗盘、导航朝向",
               privacy=True),
    SensorKind("baro", "气压计", "pressure",
               ["in_pressure_raw"],
               unit="kPa",
               desc="海拔、天气、楼层识别"),
    SensorKind("temp", "温度传感器", "temp",
               ["in_temp_raw"],
               unit="°C",
               desc="机器温度与降温策略"),
    SensorKind("proximity", "接近传感器", "proximity",
               ["in_proximity_raw"],
               desc="通话时熄屏"),
    SensorKind("hall", "霍尔传感器", "hall",
               ["in_hall_raw"],
               desc="翻盖/皮套检测"),
]

KIND_BY_ID = {s.id: s for s in SENSOR_KINDS}

# 属于隐私的传感器：直读等于任何应用都能持续获取设备朝向
PRIVACY_SENSORS = [s.id for s in SENSOR_KINDS if s.privacy]


# ---------------------------------------------------------------- 扫描

@dataclass
class Sensor:
    name: str          # iio:deviceN
    type: str
    path: Path
    channels: list = field(default_factory=list)


def scan_iio(root: Path = Path("/")) -> list:
    """扫描 IIO 设备。"""
    base = root / "sys" / "bus" / "iio" / "devices"
    if not base.exists():
        return []
    out = []
    for d in sorted(base.iterdir()):
        if not d.name.startswith("iio:device"):
            continue
        name = ""
        nf = d / "name"
        if nf.exists():
            try:
                name = nf.read_text().strip()
            except OSError:
                pass
        chans = []
        try:
            for f in d.iterdir():
                if f.name.startswith(("in_", "out_")) and \
                        f.name.endswith("_raw"):
                    chans.append(f.name)
        except OSError:
            pass
        # 从通道名推断类型
        kind = ""
        for s in SENSOR_KINDS:
            if any(s.sysfs_type in c for c in chans):
                kind = s.id
                break
        out.append(Sensor(d.name, kind, d, sorted(chans)))
    return out


def sensors_report(root: Path = Path("/")) -> str:
    devs = scan_iio(root)
    L = ["传感器：", ""]
    if not devs:
        L.append("  ! 没有 IIO 设备——内核未启用 CONFIG_IIO，"
                 "或这是一台虚拟机/容器")
        L.append("    没有传感器时：屏幕不会自动旋转、"
                 "亮度不会自动调、皮套检测不工作")
        L.append("    且这些功能都表现为'开关无效'，而不是报错")
        return "\n".join(L)
    for d in devs:
        k = KIND_BY_ID.get(d.type)
        title = k.title if k else d.type or "未知类型"
        mark = "  " if d.type else "? "
        L.append(f"{mark}{d.name:<14}{title:<10}"
                 f"{'（' + (k.unit or '') + '）' if k and k.unit else ''}")
        if d.channels:
            L.append(f"    通道: {'、'.join(d.channels[:4])}")
        if k and k.privacy:
            L.append("    ! 属于隐私数据：应走 sensor-proxy，"
                     "不让应用直读")
        if not d.type:
            L.append("    ? 无法从通道名判断类型——"
                     "可能命名不规范")
    return "\n".join(L)


# ---------------------------------------------------------------- 校准自检

def calibration_check(kind: str, readings: list) -> list:
    """检查传感器读数是否可信。

    加速度计没校准，静止时读数恒定偏 300mg——
    自动旋转会一直在错误角度。用户看不到读数，
    只看到"转屏不动"或"转屏乱转"。
    """
    k = KIND_BY_ID.get(kind)
    if k is None:
        raise SensorError(f"没有这类传感器: {kind}")
    if k.rest_expect is None:
        return []
    if len(readings) != len(k.rest_expect):
        raise SensorError(
            f"{k.title} 需要 {len(k.rest_expect)} 个轴的读数，"
            f"收到 {len(readings)} 个")
    problems = []
    if kind == "accel":
        # 静止时合矢量应接近重力 9.8
        mag = sum(x * x for x in readings) ** 0.5
        if abs(mag - 9.8) > 1.5:
            problems.append(
                f"静止时合矢量 {mag:.2f}，应接近 9.8——"
                f"偏差 {abs(mag-9.8):.2f} 说明未校准或器件异常。"
                f"后果：屏幕自动旋转角度一直偏，"
                f"用户只看到'转屏不对'")
        # 单轴偏移
        for i, v in enumerate(readings[:2]):
            # 零偏阈值取 0.5：真实加速度计零偏通常 <0.2，
            # 阈值太松会让明显偏移的器件通过自检
            if abs(v) > 0.5:
                problems.append(
                    f"{'XYZ'[i]} 轴静止时读数 {v:.2f}，"
                    f"应接近 0——存在零点偏移，需校准")
    elif kind == "gyro":
        # 静止时各轴应接近 0
        for i, v in enumerate(readings):
            if abs(v) > 0.2:
                problems.append(
                    f"{'XYZ'[i]} 轴静止时读数 {v:.3f}，应接近 0——"
                    f"存在零偏。后果：屏幕会缓慢自转、"
                    f"或游戏视角漂移")
    return problems


def uncalibrated_hint(kind: str) -> str:
    return ("校准方式：\n"
            "  1. 静止放置 3 秒后读一次，记录各轴偏移\n"
            "  2. 写入校准文件（多数发行版在 /etc/iio-sensor-proxy/）\n"
            "  3. 或安装 iio-sensor-proxy，它由代理统一处理\n"
            "  注意：**没有校准的加速度计会让自动旋转一直偏**，\n"
            "  而用户只会觉得'这功能有问题'")


# ---------------------------------------------------------------- 代理访问

def proxy_required(sensor: str) -> bool:
    """这类传感器是否必须走代理。"""
    return sensor in PRIVACY_SENSORS


def access_model_report() -> str:
    L = ["传感器访问模型：", ""]
    L.append("  隐私类传感器（" + "、".join(PRIVACY_SENSORS) + "）：")
    L.append("    必须走 iio-sensor-proxy，应用通过 D-Bus 获取。")
    L.append("    **直读等于任何应用都能持续获取设备朝向**——")
    L.append("    这是隐私数据，不是硬件数据。")
    L.append("")
    L.append("  非隐私类（气压、温度、接近、霍尔）：")
    L.append("    可由 hwmon / iio 直接读取。")
    return "\n".join(L)


# ---------------------------------------------------------------- 风扇

# 风扇曲线。写错会导致过热，所以下限必须有保护值
FAN_PROTECTION = {
    "min_duty_at_70c": 40,    # 70°C 时至少 40% 转速
    "critical_temp": 90,      # 超过就全速且不理会用户曲线
    "hysteresis": 5,          # 回滞，避免转速频繁跳变
}


@dataclass
class FanPoint:
    temp: int
    duty: int


def fan_curve_check(points: list) -> list:
    """检查风扇曲线。

    写错曲线会导致过热。最低温度点必须覆盖到 70°C，
    且高温段的转速不能低于保护值。
    """
    problems = []
    if not points:
        return ["曲线为空"]
    pts = sorted(points, key=lambda p: p.temp)
    if pts[0].temp > 40:
        problems.append(
            f"最低温度点 {pts[0].temp}°C 过高——"
            f"低于此温度时风扇完全不转，"
            f"中负载下会积热")
    # 覆盖到高温段
    if pts[-1].temp < 80:
        problems.append(
            f"曲线只到 {pts[-1].temp}°C——"
            f"超过后没有对应档位，内核会回退到默认策略，"
            f"而那时机器已经很热了")
    # 检查单调性：温度升转速降是最危险的错误
    for a, b in zip(pts, pts[1:]):
        if b.duty < a.duty:
            problems.append(
                f"{a.temp}°C→{b.temp}°C 转速反而从 {a.duty}% "
                f"降到 {b.duty}%——温度越高转得越慢，会烧")
    # 高温段保护
    for p in pts:
        if p.temp >= 70 and p.duty < FAN_PROTECTION["min_duty_at_70c"]:
            problems.append(
                f"{p.temp}°C 时转速 {p.duty}% 低于保护值 "
                f"{FAN_PROTECTION['min_duty_at_70c']}%——"
                f"用户曲线不能把机器置于过热风险")
    # 回滞
    for a, b in zip(pts, pts[1:]):
        if b.temp - a.temp < FAN_PROTECTION["hysteresis"]:
            problems.append(
                f"{a.temp}°C 与 {b.temp}°C 间隔小于 "
                f"{FAN_PROTECTION['hysteresis']}°C——"
                f"没有回滞会导致转速频繁跳变，噪音忽大忽小")
    return problems


def fan_curve_report(points: list) -> str:
    pts = sorted(points, key=lambda p: p.temp)
    L = ["风扇曲线：", ""]
    for p in pts:
        L.append(f"  {p.temp}°C  →  {p.duty}%")
    L.append("")
    L.append(f"  保护值: {FAN_PROTECTION['critical_temp']}°C 以上强制全速，"
             f"不理会用户曲线")
    L.append(f"  回滞下限: {FAN_PROTECTION['hysteresis']}°C")
    probs = fan_curve_check(pts)
    if probs:
        L.append("")
        L.append("  问题：")
        for x in probs:
            L.append(f"    ! {x}")
    return "\n".join(L)


def fan_paths(root: Path = Path("/")) -> list:
    """找风扇控制接口。"""
    out = []
    base = root / "sys" / "class" / "hwmon"
    if not base.exists():
        return out
    for d in sorted(base.iterdir()):
        try:
            for f in d.iterdir():
                if f.name.startswith("pwm") and \
                        not f.name.endswith(("_mode", "_enable", "_freq")):
                    out.append(f)
        except OSError:
            continue
    return out


def fan_report(root: Path = Path("/")) -> str:
    ps = fan_paths(root)
    L = ["风扇：", ""]
    if not ps:
        L.append("  ! 没有找到风扇控制接口（hwmon/pwm）——"
                 "虚拟机常见，或内核未开 CONFIG_HWMON")
        L.append("    没有接口时只能靠内核默认曲线：")
        L.append("    通常要么噪音大，要么温度高")
        return "\n".join(L)
    for p in ps:
        L.append(f"  {p}")
    L.append("")
    L.append("  # 手动接管要先把模式从自动改成手动：")
    L.append("  # echo 1 > pwmX_enable（0=全速 1=手动 2=自动）")
    L.append("  # **忘记改回来会导致过热**")
    return "\n".join(L)


# ---------------------------------------------------------------- 背光

def backlight_kinds(root: Path = Path("/")) -> dict:
    """区分屏幕背光与键盘背光。

    两者 sysfs 路径不同（backlight 与 leds），
    混在一起会导致改了屏幕亮度结果键盘灯变了。
    """
    out = {"screen": [], "keyboard": []}
    bl = root / "sys" / "class" / "backlight"
    if bl.exists():
        out["screen"] = sorted(x.name for x in bl.iterdir())
    leds = root / "sys" / "class" / "leds"
    if leds.exists():
        for d in sorted(leds.iterdir()):
            n = d.name.lower()
            if re.search(r"kbd|keyboard|::kbd_backlight|platform::", n):
                out["keyboard"].append(d.name)
    return out


def backlight_report(root: Path = Path("/")) -> str:
    k = backlight_kinds(root)
    L = ["背光（屏幕与键盘是两套，别混）：", ""]
    L.append(f"  屏幕: {'、'.join(k['screen']) or '无'}")
    L.append(f"  键盘: {'、'.join(k['keyboard']) or '无'}")
    if not k["screen"] and not k["keyboard"]:
        L.append("")
        L.append("  ! 都没有——虚拟机/容器常见")
    L.append("")
    L.append("  屏幕: /sys/class/backlight/<名>/brightness")
    L.append("  键盘: /sys/class/leds/<名>/brightness")
    L.append("")
    L.append("  # 键盘背光最大值常是 255，屏幕常是 0~max_brightness，")
    L.append("  # 写成同一个值会导致一边过亮一边几乎不亮。")
    return "\n".join(L)


def kbd_backlight_cmd(value: int | None = None,
                      percent: int | None = None) -> str:
    from . import control as C
    k = backlight_kinds()
    if not k["keyboard"]:
        raise SensorError("没有键盘背光设备")
    d = k["keyboard"][0]
    if percent is not None:
        return (f"MAX=$(cat /sys/class/leds/{d}/max_brightness)\n"
                f"echo $(( MAX * {percent} / 100 )) "
                f"> /sys/class/leds/{d}/brightness")
    if value is None:
        raise SensorError("value 和 percent 要给一个")
    return (f"MAX=$(cat /sys/class/leds/{d}/max_brightness)\n"
            f"[ {value} -le $MAX ] || echo '超过最大值，不会生效'\n"
            f"echo {value} > /sys/class/leds/{d}/brightness")


# ---------------------------------------------------------------- 雷电

# 雷电设备能 DMA 直接访问内存——不设安全等级，
# 插一个恶意设备就能读走全部内存
TB_LEVELS = {
    "none": "无保护：设备可 DMA 访问全部内存",
    "user": "仅已授权设备可用，需用户确认",
    "secure": "仅已授权设备，且每次连接需确认",
    "dponly": "仅允许 DisplayPort，禁止 PCIe（最安全）",
}


def thunderbolt_report(root: Path = Path("/")) -> str:
    base = root / "sys" / "bus" / "thunderbolt" / "devices"
    L = ["雷电 / USB4：", ""]
    if base.exists():
        devs = sorted(x.name for x in base.iterdir())
        L.append(f"  设备: {'、'.join(devs) or '（当前未连接）'}")
    else:
        L.append("  当前状态: 没有雷电控制器——"
                 "内核未开 CONFIG_THUNDERBOLT，或机器没有该接口")
    L.append("")
    L.append("  安全等级（决定设备能否 DMA 访问内存）：")
    for k, v in TB_LEVELS.items():
        L.append(f"    {k:<10}{v}")
    L.append("")
    L.append("  # **雷电设备能直接 DMA 读内存**——不设安全等级，")
    L.append("  # 插一个恶意设备就能读走全部内存。")
    L.append("  # 这是'接个外设丢数据'的真实途径，不是理论风险。")
    L.append("  # 注意：现在没有控制器不等于以后不会有——")
    L.append("  # 接上 USB4 扩展坞后这套规则立刻生效，")
    L.append("  # 所以原则要写在这里，而不是只在有硬件时才提。")
    return "\n".join(L)


def thunderbolt_level_cmd(level: str) -> str:
    if level not in TB_LEVELS:
        raise SensorError(
            f"没有这个安全等级 {level}"
            f"（可选：{'、'.join(TB_LEVELS)}）")
    return f"echo {level} > /sys/bus/thunderbolt/devices/0-0/security"


# ---------------------------------------------------------------- GPIO

def gpio_report(root: Path = Path("/")) -> str:
    devs = sorted(Path("/dev").glob("gpiochip*"))
    L = ["GPIO：", ""]
    if devs:
        L.append(f"  控制器: {'、'.join(d.name for d in devs)}")
    else:
        L.append("  当前状态: 没有 /dev/gpiochip* ——"
                 "内核未开 CONFIG_GPIO_CDEV，或这不是嵌入式平台")
    L.append("")
    L.append("  # 权限必须收紧：能操作 GPIO 等于能操作物理设备")
    L.append("  # （继电器、马达、门禁）。默认不给普通用户，")
    L.append("  # 需要时加入 gpio 组。")
    L.append("  # 放宽权限 = 任何用户都能开你的继电器、马达。")
    L.append("  # 这条原则与当前有没有控制器无关——")
    L.append("  # 接上扩展板后立刻适用。")
    return "\n".join(L)


# ---------------------------------------------------------------- 飞行模式

def rfkill_kinds() -> list:
    """列出可切换的无线类型。"""
    return ["wifi", "bluetooth", "wwan", "gps", "nfc", "fm"]


def flight_mode_cmd(on: bool) -> str:
    """飞行模式。

    必须同时关掉所有无线类型，只关 wifi 的话
    用户以为开了飞行模式，其实蓝牙还在。
    """
    act = "block" if on else "unblock"
    L = [f"# 飞行模式要关掉全部无线类型，"
         f"只关 wifi 的话蓝牙还开着"]
    for k in rfkill_kinds():
        L.append(f"rfkill {act} {k}")
    L.append("rfkill list   # 确认全部为 yes")
    return "\n".join(L)


def main_cli(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="qysensor",
                                 description="LANXIU Linux 传感器与平台控制")
    ap.add_argument("--root", default="/")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="扫描传感器")
    sub.add_parser("access", help="访问模型")
    sp = sub.add_parser("calibrate", help="校准自检")
    sp.add_argument("kind", choices=[s.id for s in SENSOR_KINDS])
    sp.add_argument("readings", nargs="+", type=float)
    sp = sub.add_parser("cal-hint", help="校准指引")
    sp.add_argument("kind", choices=[s.id for s in SENSOR_KINDS])
    sp = sub.add_parser("fan-curve", help="风扇曲线检查")
    sp.add_argument("points", nargs="+", help="格式 温度:转速")
    sub.add_parser("fan", help="风扇控制接口")
    sub.add_parser("backlight", help="背光（屏幕/键盘）")
    sp = sub.add_parser("kbd-light", help="键盘背光")
    sp.add_argument("--percent", type=int)
    sp.add_argument("--value", type=int)
    sub.add_parser("thunderbolt", help="雷电安全")
    sp = sub.add_parser("tb-level", help="设置雷电安全等级")
    sp.add_argument("level", choices=list(TB_LEVELS))
    sub.add_parser("gpio", help="GPIO")
    sp = sub.add_parser("flight", help="飞行模式")
    sp.add_argument("--on", action="store_true")

    a = ap.parse_args(argv)
    root = Path(a.root)

    try:
        if a.cmd == "scan":
            print(sensors_report(root))
            return 0
        if a.cmd == "access":
            print(access_model_report())
            return 0
        if a.cmd == "calibrate":
            probs = calibration_check(a.kind, a.readings)
            for x in probs:
                util.log("warn", x)
            if not probs:
                util.log("ok", "读数在合理范围")
            return 1 if probs else 0
        if a.cmd == "cal-hint":
            print(uncalibrated_hint(a.kind))
            return 0
        if a.cmd == "fan-curve":
            pts = []
            for s in a.points:
                if ":" not in s:
                    util.log("err", f"要用 温度:转速 格式，收到 {s}")
                    return 1
                t, d = s.split(":", 1)
                pts.append(FanPoint(int(t), int(d)))
            print(fan_curve_report(pts))
            return 0
        if a.cmd == "fan":
            print(fan_report(root))
            return 0
        if a.cmd == "backlight":
            print(backlight_report(root))
            return 0
        if a.cmd == "kbd-light":
            print(kbd_backlight_cmd(a.value, a.percent))
            return 0
        if a.cmd == "thunderbolt":
            print(thunderbolt_report(root))
            return 0
        if a.cmd == "tb-level":
            print(thunderbolt_level_cmd(a.level))
            util.log("info", TB_LEVELS[a.level])
            return 0
        if a.cmd == "gpio":
            print(gpio_report(root))
            return 0
        if a.cmd == "flight":
            print(flight_mode_cmd(a.on))
            return 0
    except SensorError as e:
        util.log("err", str(e))
        return 1
    return 1
