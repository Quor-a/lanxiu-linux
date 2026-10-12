# LANXIU 第十三批功能 UI 设计规格

> 依据 `ui-design-guide` skill（品牌深蓝 #0A0E1A/#111724 + 品牌橙 #FF5C1A，禁紫/禁系统字体/禁 emoji，布局非对称留白）。

## 1. 系统托盘 SNI（StatusNotifierItem 接入）

```
DESIGN SPECIFICATION
1. Purpose: 让第三方应用图标进入系统托盘（qydesktop 自绘托盘区改为接收 SNI 注册）。
2. Aesthetic Direction: Industrial/utilitarian（工业简洁——图标等距排列，hover 橙框）
3. Color Palette: 托盘底 #111724、图标 hover 背景 rgba(255,92,26,0.15)、hover 描边 #FF5C1A
4. Typography: 无新增字体；tooltip 用系统字体
5. Layout Strategy: 托盘图标水平排列在顶栏右侧（时钟左侧），固定宽度 24px/图标，hover 显示圆角背景；右键菜单（gtk_menu_popup_at_pointer）
```

实现要点：
- qydesktop 用 DBus 注册 `org.kde.StatusNotifierWatcher`（监听 `ItemRegistered/ItemUnregistered`），接收第三方注册。
- 每个 SNI 图标用 GtkImage 占位（`gdk_pixbuf`），点击发送 `Activate`，右键菜单发送 `ContextMenu` 请求显示 DBus 菜单（简化：先显示"应用菜单"占位）。
- 无第三方注册时保留现有固定系统图标（音量/网络/铃铛/电池）。

## 2. 输入法框架集成引导（qysettings 输入法页增强）

```
DESIGN SPECIFICATION
1. Purpose: 引导用户安装并启用 fcitx5/ibus，并检测运行状态。
2. Aesthetic Direction: Guidance 式（清晰分步——状态检测卡 + 安装指引卡 + 启用开关卡）
3. Color Palette: 卡片 #111724 圆角 12、状态点（已安装绿 #4CAF50 / 未安装灰 #3A4558）、按钮橙 #FF5C1A
4. Layout: 三卡片纵向排列，非对称（左侧状态列 + 右侧说明文字）
```

实现要点：
- 检测 `command -v fcitx5/ibus`；未安装显示安装命令（`sudo qypkg install fcitx5` 提示）。
- 启用开关写 `/etc/profile.d/lanxiu-inputmethod.sh`（已有逻辑，增强 UI 分组）。

## 3. 虚拟桌面窗口管理（qydesktop 工作区）

```
DESIGN SPECIFICATION
1. Purpose: 支持把当前窗口发送到其他工作区（键盘/右键），并显示每工作区窗口概览。
2. Aesthetic Direction: 保持顶栏简洁——右键菜单项 + 快捷键显示，预览弹窗用深色卡片
3. Color Palette: 复用 #111724/#FF5C1A/#9AA7B8
4. Layout: 工作区预览弹窗横向 4 卡片（每卡显示工作区名 + 窗口数 + 迷你窗口缩略占位）
```

实现要点：
- 工作区 Popover 增加「将当前窗口发送到工作区 N」子菜单（wmctrl -t N 或占位写配置）。
- 预览卡片迷你窗口占位（cairo 圆角小方块，橙框当前工作区）。

## 4. 备份任务管理（qybackup 计划列表增强）

```
DESIGN SPECIFICATION
1. Purpose: 可视化查看 crontab 中的备份计划（下次执行时间/上次执行状态）。
2. Aesthetic Direction: 表格式清单（时间线感——左侧计划列表 + 右侧执行日志）
3. Color Palette: 列表深色 #111724、状态标签（成功绿/失败红/待执行橙）
4. Layout: 计划区上下分割（上：计划列表；下：最近执行日志，来自 ~/.local/share/lanxiu/backup.log）
```

实现要点：
- 解析 crontab 中含 `# lanxiu-scheduled-backup` 的行，显示频率/时间/下次执行。
- 备份执行时追加日志到 `~/.local/share/lanxiu/backup.log`（时间|源|目标|状态），列表区读取显示最近 20 条。

## 审计
- 无 purple/violet/indigo/fuchsia；无 Inter/Roboto/Arial/system-ui；无 emoji 图标（全部 cairo 线稿/字符）。
- 布局含非对称/分组卡片，非纯居中对称。