# LANXIU 第十四批：系统托盘 SNI 设计规格

> 依据 `ui-design-guide` skill。品牌：深蓝 #0A0E1A/#111724 + 品牌橙 #FF5C1A；禁紫/禁 emoji/禁系统字体。

## 目标
让第三方应用（如输入法、下载器、即时通讯）的图标通过 **StatusNotifierItem（SNI）** 协议进入 qydesktop 顶栏托盘区，并支持左键激活 / 右键菜单。

## 设计规格

```
DESIGN SPECIFICATION
1. Purpose: 接收 DBus SNI 注册，把第三方托盘图标渲染到顶栏（时钟左侧）。
2. Aesthetic Direction: Industrial/utilitarian——托盘图标 24×24 等距排列，hover 橙色圆角底
3. Color Palette: 托盘底透明、hover rgba(255,92,26,0.15)、描边 #FF5C1A、图标默认 #9AA7B8
4. Typography: 无新增字体；tooltip/菜单沿用系统字体
5. Layout Strategy: 图标从右向左排（时钟左侧），每图标 26×26 热区，图标间 4px
```

## 实现要点
1. qydesktop 注册 `org.kde.StatusNotifierWatcher`（`RegisterStatusNotifierItem` 方法 + `ItemRegistered/ItemUnregistered` 信号）。
2. 收到 `ItemRegistered(service)` 后：解析 `StatusNotifierItem` DBus 对象（`org.kde.StatusNotifierItem` 路径），读取 `IconName`/`IconPixmap`/`ToolTip` 属性。
3. 托盘图标用 GtkImage（IconPixmap 为 ARGB32 数组时直接转 GdkPixbuf；否则用图标主题名 fallback）。
4. 左键点击 → `Activate(x, y)`；右键 → `ContextMenu(x, y)`（第三方显示菜单）。
5. 无第三方注册时保留现有固定系统图标（音量/网络/铃铛/电池）。
6. 限制：GNOME 的 AppIndicator 兼容层不在本环境；先支持标准 SNI（fcitx5 等使用 AppIndicator 的项目需 `APPINDICATOR_SUPPORT=1` 时降级为只读图标）。

## 审计
- 无 purple/violet/fuchsia；无系统字体；无 emoji（托盘图标全部来自第三方自身位图）。
- 布局为顶栏右端紧凑排列（非居中对称），符合品牌非对称留白原则。