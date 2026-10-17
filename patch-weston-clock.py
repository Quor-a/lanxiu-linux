#!/usr/bin/env python3
"""LANXIU weston 补丁: desktop-shell 顶栏时钟支持 clock-format-string (自定义 strftime 格式, 实现中文日期时间)"""
import sys, os

# 在解包后的源码树根目录运行 (cwd = srcdir)
import glob
cands = glob.glob("clients/desktop-shell.c") + glob.glob("*/clients/desktop-shell.c") + glob.glob("**/clients/desktop-shell.c", recursive=True)
if not cands:
    print("no desktop-shell.c anywhere, skip")
    sys.exit(0)
p = cands[0]

s = open(p).read()
if "custom_clock_format" in s:
    print("already patched")
    sys.exit(0)

# 1. 结构体加字段
a1 = "\tenum clock_format clock_format;\n\n\tstruct window *grab_window;"
b1 = "\tenum clock_format clock_format;\n\tchar *custom_clock_format;\n\n\tstruct window *grab_window;"
assert a1 in s, "struct anchor not found"
s = s.replace(a1, b1, 1)

# 2. 配置解析
a2 = '''	else if (strcmp(clock_format, "none") == 0)
		desktop->clock_format = CLOCK_FORMAT_NONE;
	else
		desktop->clock_format = DEFAULT_CLOCK_FORMAT;
	free(clock_format);'''
b2 = a2 + '''
	weston_config_section_get_string(s, "clock-format-string", &desktop->custom_clock_format, NULL);'''
assert a2 in s, "config anchor not found"
s = s.replace(a2, b2, 1)

# 3. redraw 使用自定义格式
a3 = "\tstrftime(string, sizeof string, clock->format_string, timeinfo);"
assert a3 in s, "strftime anchor not found"
s = s.replace(a3, "\tconst char *cf = clock->panel->desktop->custom_clock_format;\n\tstrftime(string, sizeof string, cf ? cf : clock->format_string, timeinfo);", 1)

open(p, "w").write(s)
print("weston custom clock patch applied")
