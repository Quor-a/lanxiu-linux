# 宿主环境 vs LANXIU 桌面差距审计（host-gap-audit）

> 审计日期：2026-10（基于 README「桌面环境开发进展」至 v0.1.0-245 + 源码盘点）
> 对象：宿主构建机（Xfce4 + openbox + weston 开发环境） ↔ 澜岫 LANXIU Linux 桌面（`recipes/*.c`，39 个 qy* GUI 应用 + qytheme/qynotifd/qyshot-capture/qysw-x11 等辅助件）
> 方法：盘点宿主 `/usr/bin`、`.desktop`、字体/主题/工具链 → 逐个核对 qy*.c 的窗口标题、TR() 字符串、功能关键词 → 输出「宿主有 / LANXIU 没有或不够完善」的可执行差距清单
> 约束：只盘点与对比，**未改任何源码、未 commit**。

---

## 0. 宿主能力快照（盘点结果）

| 类别 | 宿主具备 | 宿主缺失（LANXIU 需要时得自带） |
|---|---|---|
| 桌面 | xfce4-panel / xfce4-terminal / thunar / xfce4-settings-* / openbox / fcitx5 / google-chrome / qemu / byobu / htop | 无 gnome-calculator/kcalc、无 scrot/flameshot/gnome-screenshot、无截图 GUI、无 nload/iperf/ncdu/ranger |
| 系统/网络 | systemctl / journalctl / timedatectl / hostnamectl / localectl / passwd / chsh / upower / udisksctl / rsync / curl / wget / git / ffmpeg / import / convert / xclip / xsel / cpupower / findmnt / fusermount / lsblk | 无 nmcli / iwctl / bluetoothctl / pactl / amixer（宿主无，LANXIU rootfs 自带 amixer/alsamixer/ip/iw/udhcpc/7za/weston-screenshooter）/ gparted / fdisk / parted / smartctl |
| 媒体 | ffplay（无 mpg123/mpv/vlc） | 无音频播放 GUI、无录制 GUI（LANXIU 需自带 aplay/ffmpeg） |
| 远程/虚拟化 | Xvnc / vncserver / Xvfb / qemu / tigervncpasswd | 无 x11vnc、无真实 VNC 服务端接入 |
| 字体/主题 | 197 个字体（Noto Sans/Serif CJK SC 齐全）、/usr/share/themes 20+（含 lanxiu）、/usr/share/icons 10+ | — |

LANXIU `var/sysroot/usr/bin` 已内置：`7za alsamixer amixer aplay curl ip iw tar udhcpc weston weston-screenshooter weston-terminal git` 及全部 qy* 应用；**未内置**：`ffmpeg、rsync、wpa_supplicant、bluetoothctl、htop、screenshot GUI`。

---

## 1. 必备小工具缺失

> 判定：宿主有同类能力（命令或 .desktop），LANXIU 对应功能「没有、只读、或未闭环」。

| # | 宿主能力 | LANXIU 现状 | 建议实现 | 依赖与难度 |
|---|---|---|---|---|
| 1.1 | xfce4-calculator / gnome-calculator（宿主未装，但计算器是桌面必备） | **已有** `qycalc`：四则+括号+优先级、物理键盘、C/⌫。 | 属已补齐。可再加：百分比/正负号、历史记录、结果复制、科学计算面板。 | 无新依赖。难度：低 |
| 1.2 | xfce-backdrop-settings（壁纸） | **已有** `qysettings` 壁纸页 + qydesktop 程序化生成/自动轮换/`/etc/qywallpaper.conf` 闭环。 | 属已补齐。可加：壁纸缩略图预览、从任意路径选图、随机壁纸收藏。 | 无新依赖。难度：低 |
| 1.3 | xfce4-appearance-settings + `fc-list` 197 字体 | **半成品**：`qysettings`「字体」页只有 3 行只读文本（西文 DejaVu、中文 Noto Sans CJK、回退 fontconfig），无任何交互。 | 改为可交互：字体下拉/列表（fc-list 扫描）、字号预览、写入 `~/.config/fontconfig/fonts.conf` 或 `/etc/fonts/local.conf`；预览窗即时渲染。 | 依赖 fontconfig 库/命令（rootfs 需带 `fc-list`）。难度：低-中 |
| 1.4 | timedatectl（宿主有） | **部分**：`qysettings` 日期时间页显示大时钟/时区/NTP 状态；顶栏时钟+日历。 | 补手动设置日期时间（`timedatectl set-time`）、NTP 开/关、时区搜索列表；同步到 weston。 | rootfs 需 timedatectl 或手写 /etc/localtime+rtc。难度：低 |
| 1.5 | xfce4-mouse-settings / xfce4-keyboard-settings | **部分**：`qysettings` 鼠标页（主按键/双击速度/滚轮行数）、触摸板页（启用/灵敏度）、输入页（布局下拉），均写 conf。 | 补：鼠标指针大小/主题、自然滚动方向、加速度曲线；触摸板「打字时禁用」；键盘重复率/延迟。需 libinput/udev 配置消费端。 | 依赖 libinput 配置。难度：中 |
| 1.6 | apt/dpkg（宿主） | **已有**：`qystore` 搜索/筛选/详情/安装/卸载（qysudo -n）。 | 属已补齐。可加：批量安装/卸载、依赖冲突提示、已安装应用管理入口。 | 无新依赖。难度：低 |
| 1.7 | htop.desktop / info.desktop（系统信息面板） | **已有**：`qysettings` 关于页（OS/内核/架构/平台/主机名/CPU/内存/运行时间/负载/桌面版本 + Logo + 3 个快捷按钮）；`qymon` 大数字。 | 属已补齐。可加：一键复制系统信息、GPU/磁盘详情、发行版 Logo 横幅。 | 无新依赖。难度：低 |
| 1.8 | 磁盘格式化：宿主无 gparted/fdisk，LANXIU `qydisk` 只读 | **缺失**：`qydisk` 只有 df 总容量/已用/可用 + 刷新，无分区/格式化。 | 分两步：① 低风险：`qydisk` 加挂载树（findmnt -T）、文件系统类型（lsblk -f）、设备列表；② 高风险：U 盘格式化（udisksctl + mkfs.vfat/mkfs.ext4），需新增 parted/e2fsprogs/dosfstools 配方。 | 需新配方（parted、e2fsprogs、dosfstools）。难度：高 |
| 1.9 | nload/htop 网络速率（宿主无 nload，htop 有） | **已有**：`qymon` NET 紫色曲线 + 信息栏 ↓rx↑tx；顶栏 CPU/内存迷你条。 | 属已补齐。可加：接口选择下拉、按天/月历史图表、速率告警。 | 无新依赖。难度：低 |
| 1.10 | 流量统计（/proc/net/dev） | **已有**：`qysettings` 流量计费页「本月数据用量 GB + 按流量计费开关」。 | 属已补齐。可加：按日/周图表、重置统计、计费阈值提醒。 | 无新依赖。难度：低 |

---

## 2. UI 细节缺失

| # | 宿主能力 / 参照 | LANXIU 现状 | 建议实现 | 依赖与难度 |
|---|---|---|---|---|
| 2.1 | Xfce 桌面图标可自由排布/改名 | **缺失**：`qydesktop` 桌面 7 个图标固定坐标竖排（`x, y+dy*N`），右键菜单无改名/移动。 | 图标拖拽（GtkFixed drag&drop）、右键「重命名」对话框（同步 .desktop/快捷方式）、位置持久化 `~/.config/lanxiu/desktop-icons`。 | 无新依赖。难度：中 |
| 2.2 | xfce4-panel 任务栏窗口预览 | **缺失**：任务 pill 仅「图标+标题+关闭按钮」（v0.1.0-235），无缩略图。 | 悬停弹出窗口缩略图：Wayland 需 weston 补丁提供 surface 抓图；X11 回退 XGetImage。 | 依赖合成器补丁/协议。难度：高 |
| 2.3 | openbox/xfwm4 最小化动画 + 标题栏按钮 | **缺失**：`qy_make_titlebar()` 仅 `show_close_button=TRUE`，无最小化/最大化按钮，无动画。 | 标题栏补「最小化/最大化」按钮，写 `/tmp/xdg/qy-min/qy-max` 由 weston 补丁消费；动画随合成器能力。 | 依赖 weston 补丁。难度：中-高 |
| 2.4 | xfce4-notifyd 通知 + 控制中心 | **部分**：顶栏铃铛（cairo 线稿）+ 未读点/徽标 + 通知历史对话框；`qysettings` 通知/免打扰页。 | 做「通知中心」下拉面板：历史列表 + 快捷开关（免打扰/蓝牙/WiFi/音量/亮度），类似手机下拉通知栏。 | 无新依赖。难度：中 |
| 2.5 | thunar 右键菜单（书签/打开方式/压缩/发送到） | **部分**：`qyfiles` 右键 7 项（打开/剪切/复制/粘贴/重命名/删除/属性），回收站模式 3 项。 | 右键增强：「打开方式」子菜单（调 `qymime`）、「添加到书签」（侧边栏持久化）、「压缩…」（调 qyarc）、「在终端打开」（已有工具栏按钮）。 | 无新依赖。难度：低 |
| 2.6 | GTK 文本框默认右键菜单 | **不统一**：`qyedit` 的 GtkTextView 依赖 GTK3 默认弹出菜单；`qybrowser` 正文 `editable=FALSE` 无菜单、无法复制选中文本。 | 统一实现文本框右键菜单（剪切/复制/粘贴/全选），至少给 qybrowser 正文补「复制/全选」；文件列表搜索框同样处理。 | 无新依赖。难度：低 |
| 2.7 | 按钮 hover / 焦点环 | **部分**：`qytheme.css` 有 hover/active/pressed；entry 有橙边焦点；按钮键盘焦点环不明确。 | 补 `button:focus` / `button:focus-visible` 焦点环（品牌橙描边）+ Tab 键盘导航可见性；列表/图标视图选中态统一。 | 无新依赖。难度：低 |
| 2.8 | 空状态插图 | **缺失**：`qyfiles`「此文件夹为空」文字、`qyclip`「暂无历史」、`qynote`「无笔记」均为纯文字。 | 用 cairo/qyicon 画简单空状态插图（文件夹/剪贴板/音符线稿）+ 引导文字 + 行动按钮（如「新建文件夹」）。 | 复用 qyicon.c。难度：低-中 |
| 2.9 | 加载动画 | **缺失**：`qybrowser`「正在下载...」、`qystore`「执行中…」纯文字，无动画。 | 统一 GtkSpinner 或 cairo 旋转环组件（`.qy-spinner`），应用到浏览器下载、软件中心安装、备份/恢复、录屏录制中。 | 无新依赖。难度：低 |

---

## 3. 功能界面缺失

| # | 宿主能力 / 参照 | LANXIU 现状 | 建议实现 | 依赖与难度 |
|---|---|---|---|---|
| 3.1 | thunar：标签页/预览/网络挂载/搜索 | **部分**：`qyfiles` 已具备列表+图标视图、列头排序、多选、右键菜单、路径地址栏、目录搜索过滤、属性、复制/剪切/粘贴、回收站、新建文件/文件夹、终端打开、键盘加速键 15+。 | 分阶段：① 预览侧栏（选中文本/图片/音频即时预览）；② 多标签页（GtkNotebook 目录页）；③ 可移动设备挂载（lsblk + udisksctl mount/unmount，侧边栏显示）；④ 网络挂载（sftp/smb，需 gvfs）。 | ①无依赖（低）；②无依赖（低）；③需 udisks 配方（中）；④需 gvfs 大配方（高） |
| 3.2 | 图片查看（ristretto 未装；参照常规看图器） | **较完善**：`qyview` 已有 EXIF（厂商/型号/拍摄时间/尺寸/文件大小）、旋转、保存、缩放百分比/滚轮缩放/平移、适应窗口、底部导航、键盘（←→空格 PgUp/PgDn +/-/0/Ctrl+R/Ctrl+S/Delete）、Delete 回收站、幻灯片（空格）。 | 补：幻灯片过渡动画（淡入淡出/滑动）、缩放动画（平滑插值）、EXIF 扩展（镜头/ISO/GPS）、多选删除/批量重命名。 | 无新依赖。难度：低-中 |
| 3.3 | vim 语法高亮 | **缺失**：`qyedit` 有查找高亮（非语法）、状态栏行/列/字符、字号±、自动保存、查找/替换、快捷键。 | 引入 GtkSourceView 做语法高亮/行号/括号匹配/自动缩进；若库体积敏感，可自研轻量高亮（按扩展名加载关键字正则 + GtkTextTag）。 | gtksourceview 需入 rootfs（中）；自研轻量方案无新依赖（中） |
| 3.4 | 音乐播放器（宿主无，参照常规播放器） | **部分**：`qymedia` 播放列表、上/下一首、循环、音量、多格式解码（aplay/mpg123/mpffplay 探测）。 | 补：封面（同目录 cover.jpg 或 ID3 APIC）、歌词（LRC 同目录解析 + 滚动高亮）、简易均衡器（alsamixer 通道或 10 段 EQ）、播放进度条/时间。 | 封面/歌词无新依赖（低）；均衡器依赖 ALSA（中） |
| 3.5 | google-chrome | **部分**：`qybrowser` 纯文本渲染（libcurl + HTML 文本提取）、历史栈前进/后退、收藏/书签管理窗口、页面链接面板、file://、快捷键 5+。 | 补：下载管理（提取链接 URL 下载到 ~/下载 + 列表/进度）、多标签页、CSS 渲染增强。完整图形浏览器需 WebKitGTK 或 Chromium 配方（长期）。 | 下载管理无新依赖（低）；多标签（中）；完整浏览器（极高） |
| 3.6 | htop | **较完善**：`qymon` 大数字 CPU/内存/磁盘/温度 + 三色曲线 + NET 曲线 + 信息栏（运行时间/负载/进程/CPU 频率/网络速率）+ 全量进程列表（排序/结束进程）。 | 补：GPU 占用（/sys/class/drm 或 nvidia-smi）、传感器图表（各 thermal_zone/风扇转速）、磁盘 IO 曲线、网络接口选择、进程搜索/信号菜单。 | GPU 需驱动接口（中）；其余读 /proc//sys（低中） |
| 3.7 | xfce4-settings-manager | **较完善**：`qysettings` 39 页左侧图标导航、搜索框自动切页、分组标题、快捷键 Ctrl+1~9/Ctrl+F/Esc、恢复默认、QY_SETTINGS_PAGE/N 直达。 | 补：「实验性功能」分组（开发者开关集中管理）、搜索多结果列表（不止跳第一个）、设置导出/导入。 | 无新依赖。难度：低 |

---

## 4. 宿主特有而 LANXIU 未实现（命令能力 → GUI 映射）

| # | 宿主命令/能力 | LANXIU 现状 | 建议实现的 GUI 映射 | 依赖与难度 |
|---|---|---|---|---|
| 4.1 | `htop` 交互（树/F 键/信号/搜索） | qymon 可排序、可 kill | qymon 加：进程搜索框、信号子菜单（TERM/KILL/STOP/CONT）、树视图切换、按 CPU 排序 | 读 /proc。难度：中 |
| 4.2 | `tmux` / `screen` / `byobu` 分屏多标签 | LANXIU 无自家终端，桌面/文件管理器调用 `weston-terminal`（单窗口无标签无分屏） | 新增 `qyterm` 终端配方：VTE 库多标签 + 水平/垂直分屏；或先给 weston-terminal 写包装器支持多实例布局 | VTE 入 rootfs（新配方）。难度：高 |
| 4.3 | `udisksctl` 挂载 | qyfiles 不显示/不挂载可移动磁盘；qysettings 自动播放页只写 conf | qyfiles 侧边栏「可移动设备」：lsblk 枚举 + udisksctl mount/unmount；插入 U 盘时 qynotifd 轮询 /sys/block 发通知并按 autoplay 动作打开 | udisks 配方。难度：中 |
| 4.4 | `upower` 电池细节（energy-rate/health/time-to-full） | qypower 直接读 /sys/class/power_supply（电压/剩余时间/充放电状态） | qypower/qymon 接入 upower：健康度、功率、循环次数、充满时间；顶栏电池图标百分比 | upower 配方。难度：低-中 |
| 4.5 | `ffmpeg` 转码 | 仅 qyrec 用 ffmpeg 录屏；qymedia 探测 mpv/ffplay 解码 | ① 媒体转换 GUI（选文件→目标格式 mp3/ogg/mp4→ffmpeg 转码→进度条）；② 录音功能并入 qyrec（arecord/ffmpeg 录音） | ffmpeg 需入 rootfs（qyrec 已要求）。难度：中 |
| 4.6 | `import` / `convert`（ImageMagick） | qyshot 用自研 qyshot-capture（X11）+ weston-screenshooter（Wayland） | ① qyshot X11 回退 `import -window` 实现窗口/区域截图；② qyview「另存为其它格式」+ 批量格式转换（convert 或 pixbuf save） | ImageMagick 可选；pixbuf 已有。难度：低 |
| 4.7 | `systemctl` / `journalctl` 服务与日志 | qysettings「服务」页只有 3 行只读（qyinit/seatd/eudev）；无日志查看器 | ① 服务管理页（启动/停止/重启 qyinit.d 服务，状态点亮）；② 日志查看器（读 /var/log 或 journalctl 输出到深色 TextView） | 读 /var/log。难度：中 |
| 4.8 | `timedatectl` / `hostnamectl` / `localectl` | 日期时间页手写读 /etc/timezone；语言页写 /etc/qylang | ① 设置页加「主机名」编辑（写 /etc/hostname + hostnamectl）；② NTP 开关用 timedatectl；③ locale 用 localectl set-locale 同步 | 依赖命令入 rootfs。难度：低 |
| 4.9 | `passwd` / `chsh` | qyusers 建用户/改密/删用户/加 wheel | 用户详情加「默认 shell」下拉（chsh -s）、组成员管理、账户禁用 | 无新依赖。难度：低 |
| 4.10 | `vncserver` / `Xvnc` / `tigervncpasswd` | qysettings 远程桌面页只写 qyremotedesktop.conf（3389 端口说明），无真实服务 | qynotifd 消费 conf：启动 Xvnc 承载 weston 会话（或 x11vnc），端口/密码可配；设置页显示实际连接地址 | Xvnc 配方。难度：高 |
| 4.11 | `qemu` 虚拟机 | 无 VM 管理 GUI | 新增 `qyvm`：qemu-system-x86_64 图形启动、磁盘镜像创建、快照 | qemu 大配方。难度：高 |
| 4.12 | `google-chrome` 完整浏览器 | qybrowser 纯文本渲染 | 长期：WebKitGTK 或 Chromium 配方，标签页/下载/密码/扩展 | 体积巨大。难度：极高 |
| 4.13 | `fcitx5` 输入法 | qysettings 输入页只有键盘布局下拉 | 集成 fcitx5：输入法引擎选择、候选词设置、托盘输入法指示器 | fcitx5 配方。难度：中-高 |
| 4.14 | `findmnt` / `lsblk` / `fusermount` | qydisk 只显示 df 汇总 | qydisk 加：挂载树（findmnt -T）、设备/文件系统表（lsblk -f）、fuse 挂载状态 | 无新依赖。难度：低 |
| 4.15 | `xclip` / `xsel` | qyclip 已实现剪贴板历史/搜索/固定 | 无需新功能；可给 qyfiles 加「粘贴图片/文本」、qyedit 粘贴时自动入历史 | 无新依赖。难度：低 |

---

## 5. 优先级排序

### 5.1 低难度高价值（可直接做，无新依赖）

1. **字体设置页交互化**（1.3）：从只读 3 行改为字体下拉 + 预览 + fontconfig 写入。
2. **qyfiles 右键增强**（2.5）：打开方式子菜单 + 侧边栏书签 + 压缩入口。
3. **文本框右键菜单统一**（2.6）：重点补 qybrowser 正文的复制/全选。
4. **按钮焦点环 + 键盘导航**（2.7）：`button:focus-visible` 品牌橙描边。
5. **加载动画**（2.9）：GtkSpinner/cairo 旋转环统一应用到下载/安装/备份/录制。
6. **qyclock 补计时器**（倒计时）：秒表已有，补「计时器」标签页。
7. **qymon 进程搜索 + 信号菜单**（4.1 的低配版）。
8. **qysettings 主机名修改 + NTP 开关**（4.8）。
9. **qyview 另存为其它格式 / 批量转换**（4.6②）。
10. **空状态插图**（2.8）：qyicon 扩 EMPTY 系列线稿。
11. **qydisk 挂载树与文件系统信息**（4.14）。
12. **qypower 接入 upower 健康度**（4.4 的低配版）。

### 5.2 中难度（需小配方或较多 UI 工作）

1. 桌面图标拖拽/改名/持久化（2.1）
2. 通知中心快捷开关面板（2.4）
3. qyfiles 预览侧栏 + 多标签页（3.1①②）
4. qymedia 封面 + LRC 歌词滚动（3.4）
5. qymon GPU/传感器/磁盘 IO 曲线（3.6）
6. qysettings 服务管理页 + 日志查看器（4.7）
7. qyfiles 可移动设备挂载（4.3，需 udisks）
8. 媒体转换 GUI（4.5）
9. qyedit 轻量语法高亮 + 自动缩进（3.3 自研方案）

### 5.3 高难度（需新配方 / 合成器补丁 / 大依赖）

1. `qyterm` 终端应用（VTE 多标签/分屏）（4.2）
2. 窗口最小化/最大化按钮 + 动画（2.3，weston 补丁）
3. 任务栏窗口缩略图预览（2.2，合成器）
4. qyfiles 网络挂载（gvfs/sftp/smb）（3.1④）
5. 远程桌面真实服务（Xvnc）（4.10）
6. `qyvm` 虚拟机管理器（qemu）（4.11）
7. 完整图形浏览器（WebKitGTK/Chromium）（4.12）
8. qyedit 完整语法高亮（GtkSourceView 入 rootfs）（3.3）
9. 磁盘分区/格式化 GUI（1.8）
10. fcitx5 输入法深度集成（4.13）

---

## 附：审计依据（LANXIU 现状快照，源码盘点）

- **qysettings**（259 条 TR，39 页）：关于/显示/字体/服务/声音/亮度/日期时间/语言/驱动/存储/电源/网络/隐私/应用程序/专注/通知/防火墙/代理/设备/多任务/自动播放/鼠标/网络重置/多显示器/蓝牙/触摸板/图形/远程桌面/投影/HD Color/打印机/流量计费/输入/笔和Ink/启动项/WiFi/窗口行为/壁纸/主题色。
- **qyfiles**（81 条 TR）：列表/图标视图、列排序、多选、右键 7 项、地址栏、搜索过滤、复制/剪切/粘贴、回收站、属性、新建文件/文件夹、终端、刷新、15+ 快捷键。
- **qyview**（22 条 TR）：EXIF 信息面板、旋转、保存、缩放、适应窗口、导航、幻灯片、全屏、Delete 回收站。
- **qyedit**（30 条 TR）：查找/替换/高亮、行列字符状态栏、字号±、自动保存、快捷键。
- **qymedia**（33 条 TR）：播放列表、上下首、循环、音量、多格式解码探测。
- **qybrowser**（23 条 TR）：文本渲染、历史栈、收藏/书签窗口、页面链接、file://、快捷键。
- **qymon**（19 条 TR）：四色曲线（CPU/内存/磁盘/NET）、温度、TOP5、全进程列表、结束进程。
- **qydesktop**（51 条 TR）：顶栏三段（Logo/任务栏/时钟/状态区/托盘/电源）、Dock 6 图标、桌面 7 图标（固定坐标）、右键菜单 9 项、壁纸程序化生成/轮换、通知铃铛/历史、音量滑块、分辨率菜单、电源菜单、快捷键守护 15 绑定。
- 辅助件：qytheme（CSD 标题栏仅关闭按钮）、qyicon（55 个 cairo 线稿图标）、qynotifd（conf 消费/通知/电池/快捷键/自动锁屏）、qyshot-capture/qysw-x11（X11 辅助）。