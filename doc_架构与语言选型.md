# LANXIU Linux 架构与语言选型

> 原则：**每个组件用它最合适的语言，理由必须写在文档里**，不允许"因为流行"。

## 1. 选型矩阵

| 层 | 组件 | 语言 | 理由 |
|---|---|---|---|
| 内核 | linux 6.16 (upstream) | C + 汇编 | 无选择余地；自研内核阶段仍需 C，Rust for Linux 仍依赖 C 骨架 |
| init / PID 1 | qyinit | C | 最不该出 bug 的程序：零依赖、无 GC、无解释器、可静态审计；崩溃即整机死 |
| 服务管理 CLI | qyctl | C | 与 init 同契约（pidfile），必须能在最小 rootfs（无 Python）运行 |
| 系统库 | glibc / GLib / GTK3 | C | 既有上游，自研替代成本无收益 |
| 桌面 shell | qydesktop | C + GTK3 | 与 weston（C）同进程模型，无运行时依赖 |
| 桌面应用 | qyfiles / qyedit / qymon / qyview / qyarc / qysettings | C + GTK3 | 启动 <50ms、常驻内存 <10MB；Python+GTK 会引入解释器与 30MB 常驻 |
| 显示合成 | weston 14 | C | 上游 Wayland 参考实现；补丁级定制（中文时钟、任务栏） |
| 音频 | alsa-lib/utils | C | 内核 ALSA 直通，无 PulseAudio 中间层 |
| **构建系统** | qybuild / qyos | **Python 3** | 依赖图求解、配方元编程、并行调度——这类"逻辑密集 IO 轻"的工作 Python 效率最高；且只在开发机运行，不进目标系统 |
| 打包/仓库 | qypkg | Python | 同上，开发机工具 |
| 包格式 | .qyp (tar+zstd+ed25519) | 数据格式 | 无语言属性 |
| 脚本层 | *.sh | POSIX sh (busybox) | 服务启动脚本；不依赖 bash 扩展以保证最小系统可用 |
| 未来 GUI 工具 | 保留 | C + GTK3 | 一致性优先；若出现"计算密集型 + 高复杂度"，评估 C++ 或 Rust |

## 2. 明确不引入的东西与理由

| 不引入 | 理由 |
|---|---|
| systemd | 目标就是自研 init；systemd 依赖 dbus/udev/cgroup v2 全套，审计面过大 |
| polkit | 权限模型改用"qyctl + sudo + 文件权限"三件套；polkit 需 dbus + 策略文件 XML |
| PulseAudio | 直接 ALSA，省一层重采样与 30MB 常驻 |
| Python 进目标系统 | 最小 rootfs 体积与攻击面控制；Python 只在开发机 |
| Rust（当前阶段） | 工具链体积大、交叉编译链复杂；在 C 生态已成型后再评估局部替换 |

## 3. 演进路径

1. **当前**：上游包 + 自研 C 用户态（init/桌面/应用）+ Python 构建系统
2. **下一步**：polkit 替代品 `qysudo`（自研 setuid 白名单）、安装器（C+GTK3，写盘 + 引导安装）
3. **中期**：自研包管理器进目标系统（用 C 重写 qypkg 的核心子集：安装/查询/校验）
4. **远期**：自研内核模块集（qykmod 路线）与文件系统工具
