# LANXIU 下一批功能 UI 设计规格

> 依据 `ui-design-guide` skill 输出（品牌色/深色底为既有设计系统覆盖项）。

## 1. qyview 增强：图片查看器（全屏/幻灯片）

```
DESIGN SPECIFICATION
====================
1. Purpose: 让图片查看器承担"看图"主场景——沉浸全屏浏览与自动幻灯片，保留现有缩放/旋转/翻页。
2. Aesthetic Direction: Editorial/magazine（编辑式——大图为主，暗色留白，底部细工具栏悬浮）
3. Color Palette: 底 #0A0E1A（近黑）、工具栏 #111724（半透明 rgba(17,23,36,0.85)）、强调橙 #FF5C1A、次要文字 #9AA7B8、白色 #E8ECF4
4. Typography: 系统无字体依赖（GTK 主题字体）；界面仅用小字号计数/文件名，不引入新字体
5. Layout Strategy: 非对称——图片居中偏上（留出下方工具条），左下角文件名（小字）、右下角页码；全屏时图片按窗口等比适配并留 6% 边距（打破顶满布局）；幻灯片切换用淡入（GtkStack transition 0.3s）
```

实现要点（GTK3）：
- 全屏按钮（F11）切换 GtkWindow fullscreen；空格键开始/暂停幻灯片（间隔 3s，g_timeout_add）；方向键左右翻页。
- 底部悬浮工具条（GtkOverlay 子层）：上一张/播放暂停/下一张/缩放滑杆/全屏/旋转。
- 复用 qyview.c 现有 GdkPixbuf 加载与缩放逻辑。

## 2. qyaudio：音频/音量控制面板（ALSA）

```
DESIGN SPECIFICATION
====================
1. Purpose: 提供系统级音量/输出设备控制，替代仅顶栏滑块的哑控制——显示当前设备、主音量、简单均衡（高/中/低音滑杆占位）。
2. Aesthetic Direction: Industrial/utilitarian（工业感——仪表式音量表、暗底高对比、橙色主音量）
3. Color Palette: 面板 #101826、表体 #1c2434、主音量橙 #FF5C1A、刻度 #3a4558、文字 #E8ECF4
4. Typography: 不依赖新字体；用等宽数字显示 dB/百分比（GTK 默认 monospace）
5. Layout Strategy: 左侧垂直主音量滑杆（不对称——占左 40%），右侧设备列表（输出/输入分组）+ 静音按钮；底部一行"高/中/低音"小滑杆（占位）
```

实现要点（GTK3 + ALSA，sysroot 有 alsa 头与 amixer）：
- `snd_mixer_open` 打开 default，遍历 "Master" 等控件，读/写音量百分比（`snd_mixer_selem_set_playback_volume_all`）。
- 无 ALSA 设备时显示友好错误（`#qyaudio-err`），不崩溃。
- 窗口 420x360，类 `qy-audio-win`。

## 3. qypower：电池/电源状态

```
DESIGN SPECIFICATION
====================
1. Purpose: 显示电池与电源状态（充电/百分比/剩余时间），并提供"省电模式"开关（写 /etc/qypower.conf）。
2. Aesthetic Direction: Luxury/refined（精致仪表风——环形电量指示、深蓝底、橙色电量弧）
3. Color Palette: 深蓝 #0A0E1A、面板 #111724、电量弧橙 #FF5C1A、充电绿 #4CAF50（电量>20 时）、低电红 #E5484D（<20）
4. Typography: 大数字百分比（GTK 默认），无新字体
5. Layout Strategy: 环形电量图居中偏左（cairo arc 绘制），右侧信息列（充电状态/剩余时间/温度/循环次数占位）；底部"省电模式"开关（写 /etc/qypower.conf）+ 亮度滑杆（写 backlight）
```

实现要点（GTK3 + cairo + /sys/class/power_supply）：
- 读 `/sys/class/power_supply/*/`（capacity/status/voltage_now），无电池时显示"未检测到电池"。
- 窗口 400x320，类 `qy-power-win`。

## 审计（三应用通用）
- 无 purple/violet/indigo/fuchsia；无 Inter/Roboto/Arial/system-ui；无 emoji 图标（用 cairo 线稿图标，复用 qyicon.c）。
- 布局均含不对称/留白破格，非纯居中对称。