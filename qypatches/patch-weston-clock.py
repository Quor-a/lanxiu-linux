#!/usr/bin/env python3
"""LANXIU weston 补丁: desktop-shell 顶栏时钟支持 clock-format-string (自定义 strftime, 实现中文日期时间)"""
import sys, os, glob

cands = glob.glob("clients/desktop-shell.c") + glob.glob("*/clients/desktop-shell.c")
if not cands:
    print("no desktop-shell.c anywhere, skip")
    sys.exit(0)
p = cands[0]
s = open(p).read()
if "custom_clock_format" in s:
    print("already patched")
    sys.exit(0)

# 0. pango 头
a0 = "#include \"window.h\""
if a0 in s and "pango/pango.h" not in s:
    s = s.replace(a0, a0 + "\n#include <pango/pango.h>", 1)

# 1. struct desktop 加字段
a1 = "\tenum clock_format clock_format;\n\n\tstruct window *grab_window;"
b1 = "\tenum clock_format clock_format;\n\tchar *custom_clock_format;\n\n\tstruct window *grab_window;"
assert a1 in s, "desktop struct anchor"
s = s.replace(a1, b1, 1)

# 2. struct panel 加字段
a2 = "\tenum weston_desktop_shell_panel_position panel_position;\n\tenum clock_format clock_format;\n\tuint32_t color;"
b2 = "\tenum weston_desktop_shell_panel_position panel_position;\n\tenum clock_format clock_format;\n\tchar *custom_clock_format;\n\tuint32_t color;"
assert a2 in s, "panel struct anchor"
s = s.replace(a2, b2, 1)

# 3. panel_create: 拷贝自定义格式
a3 = "\tpanel->clock_format = desktop->clock_format;"
b3 = "\tpanel->clock_format = desktop->clock_format;\n\tpanel->custom_clock_format = desktop->custom_clock_format;"
assert a3 in s, "panel_create anchor"
s = s.replace(a3, b3, 1)

# 4. 配置解析
a4 = '''\telse if (strcmp(clock_format, "none") == 0)
\t\tdesktop->clock_format = CLOCK_FORMAT_NONE;
\telse
\t\tdesktop->clock_format = DEFAULT_CLOCK_FORMAT;
\tfree(clock_format);'''
b4 = a4 + '\n\tweston_config_section_get_string(s, "clock-format-string", &desktop->custom_clock_format, NULL);'
assert a4 in s, "config anchor"
s = s.replace(a4, b4, 1)

# 5. redraw: 自定义格式 + cr 创建后选 CJK 字体 (不动原绘制流程)
a5 = "\tcr = widget_cairo_create(clock->panel->widget);\n\tcairo_set_font_size(cr, 14);"
assert a5 in s, "cairo create anchor"
s = s.replace(a5,
"\tcr = widget_cairo_create(clock->panel->widget);\n"
"\tcairo_set_font_size(cr, 14);\n"
"\tcairo_select_font_face(cr, \"Noto Sans CJK SC\", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);", 1)

# 5b. strftime 用自定义格式
a5b = "\tstrftime(string, sizeof string, clock->format_string, timeinfo);"
assert a5b in s, "strftime anchor"
s = s.replace(a5b,
"\tconst char *cf = clock->panel->custom_clock_format;\n"
"\tstrftime(string, sizeof string, cf ? cf : clock->format_string, timeinfo);", 1)

open(p, "w").write(s)
print("weston custom clock patch applied to", p)
