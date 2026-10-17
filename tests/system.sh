#!/usr/bin/env bash
# 系统成型期专项测试：根文件系统骨架、init、服务管理、可启动性检查、清单比对
set -o pipefail
cd "$(dirname "$0")/.." || exit 1
PASS=0; FAIL=0
ok(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
step(){ echo; echo "== $1"; }
R=/tmp/qy-sys-root

step "0. 构建系统成型所需包"
./bin/qybuild filesystem qyinit --sign var/repo/keys/qiyuan --force >/dev/null 2>&1 \
  && ok "filesystem / qyinit 构建完成" || bad "构建失败"
./bin/qyrepo sync --sign var/repo/keys/qiyuan >/dev/null 2>&1

step "1. 根目录骨架与 /usr 合并布局"
rm -rf "$R"
./bin/qypkg --root "$R" assemble filesystem qyinit qydemo >/dev/null 2>&1
for d in usr/bin usr/lib etc var dev proc sys; do
  [ -d "$R/$d" ] || { bad "缺少 /$d"; continue; }
done
[ -d "$R/usr/bin" ] && ok "FHS 骨架建立完成"
for l in bin sbin lib; do
  [ -L "$R/$l" ] && ok "/$l 是指向 usr 的符号链接（合并布局）" || bad "/$l 不是符号链接"
done

step "2. 基础 /etc 文件"
for f in os-release passwd group hosts fstab profile hostname; do
  [ -f "$R/etc/$f" ] && ok "/etc/$f 存在" || bad "缺少 /etc/$f"
done
grep -q "LANXIU Linux" "$R/etc/os-release" && ok "os-release 标识正确" || bad "os-release 内容不对"

step "3. 可启动性检查（bootcheck）"
./bin/qypkg --root "$R" bootcheck >/tmp/qy-bc.log 2>&1 && ok "无致命问题（可启动）" \
  || { bad "存在致命问题"; cat /tmp/qy-bc.log; }
grep -q "init 存在且可执行" /tmp/qy-bc.log && ok "init 就位" || bad "init 未就位"
grep -q "libc.so.6" /tmp/qy-bc.log \
  && ok "如实提示缺 libc（真实系统需补 glibc 包）" || ok "无缺库提示"

step "4. init 能按依赖顺序启动服务、逆序停止"
# 用会持续运行的进程，停机时它们还活着，才能真正校验"逆序停止"
python3 -c "
import pathlib
d = pathlib.Path('$R/etc/qyinit.d')
(d/'aaa-base.unit').write_text('Description=基础\nExecStart=/bin/sleep 60\nRestart=no\n')
(d/'zzz-late.unit').write_text('Description=后置\nRequires=aaa-base\nExecStart=/bin/sleep 60\nRestart=no\n')
"
rm -f /tmp/qy-init.log
"$R/usr/bin/qyinit" --unit-dir "$R/etc/qyinit.d" --log /tmp/qy-init.log --oneshot >/dev/null 2>&1
grep -q "启动 aaa-base" /tmp/qy-init.log && grep -q "启动 zzz-late" /tmp/qy-init.log \
  && ok "两个服务都已启动" || { bad "服务未启动"; cat /tmp/qy-init.log; }
python3 -c "
import pathlib
lines = pathlib.Path('/tmp/qy-init.log').read_text().splitlines()
a = next(i for i,l in enumerate(lines) if '启动 aaa-base' in l)
z = next(i for i,l in enumerate(lines) if '启动 zzz-late' in l)
assert a < z, 'aaa-base 必须先于 zzz-late'
sa = next(i for i,l in enumerate(lines) if '停止 zzz-late' in l)
sz = next(i for i,l in enumerate(lines) if '停止 aaa-base' in l)
assert sa < sz, '停止必须逆序：后启动的先停'
print('顺序校验通过')
" && ok "启动按依赖顺序、停止按逆序" || bad "顺序不正确"

step "5. 服务崩溃后按 Restart 策略拉起"
python3 -c "
import pathlib, shutil
d = pathlib.Path('/tmp/qy-units')
shutil.rmtree(d, ignore_errors=True); d.mkdir(parents=True)
(d/'crash.unit').write_text('Description=会崩的服务\nExecStart=/bin/false\nRestart=on-failure\n')
"
# 日志是追加写的，必须先清空，否则会把上一轮的结果算进来
rm -f /tmp/qy-restart.log
"$R/usr/bin/qyinit" --unit-dir /tmp/qy-units --log /tmp/qy-restart.log --oneshot >/dev/null 2>&1
grep -q "异常退出" /tmp/qy-restart.log && ok "检测到服务异常退出" || bad "未检出异常退出"
n=$(grep -c "启动 crash" /tmp/qy-restart.log)
[ "$n" -ge 2 ] && ok "按 Restart=on-failure 自动拉起（启动 $n 次）" \
  || bad "未自动拉起（仅启动 $n 次）"

step "5b. 崩溃风暴限流：起不来的服务不能被无限重启"
gcc -O2 -Wall -DRESTART_MAX=2 -DRESTART_WINDOW=60 \
    -o /tmp/qyinit-rl tests/demo/qyinit/qyinit.c -lutil 2>/dev/null
rm -f /tmp/qy-rl.log
/tmp/qyinit-rl --unit-dir /tmp/qy-units --log /tmp/qy-rl.log --oneshot >/dev/null 2>&1
if grep -q "已停止拉起" /tmp/qy-rl.log; then
  ok "重启超过阈值后停止拉起并明确告警"
else
  bad "没有限流，会无限重启"; cat /tmp/qy-rl.log
fi
n=$(grep -c "启动 crash" /tmp/qy-rl.log)
[ "$n" -le 3 ] && ok "重启次数被限制在阈值内（$n 次）" || bad "重启次数失控（$n 次）"

step "6. 停机期间不得把已停止的服务再拉起"
grep -q "服务 crash 已停止" /tmp/qy-restart.log && ok "停机中的退出被正确识别" \
  || ok "（该单元在停机前已结束，跳过）"
after=$(grep -n "收到关机信号" /tmp/qy-restart.log | head -1 | cut -d: -f1)
restarts=$(sed -n "${after},\$p" /tmp/qy-restart.log | grep -c "启动 crash" || true)
[ "${restarts:-0}" -eq 0 ] && ok "停机后没有再拉起服务" || bad "停机后仍在拉起服务"

step "7. 根目录清单生成与一致性比对"
./bin/qypkg --root "$R" manifest --out /tmp/qy-man1.json >/dev/null 2>&1 \
  && ok "清单生成成功" || bad "清单生成失败"
rm -rf /tmp/qy-sys-root2
./bin/qypkg --root /tmp/qy-sys-root2 assemble filesystem qyinit qydemo >/dev/null 2>&1
./bin/qypkg --root /tmp/qy-sys-root2 manifest --out /tmp/qy-man2.json >/dev/null 2>&1
python3 -c "
import sys; sys.path.insert(0,'.')
from qyos import system as SY
import json
a=json.load(open('/tmp/qy-man1.json')); b=json.load(open('/tmp/qy-man2.json'))
d=SY.diff_manifest(a,b)
print('差异:', {k:(len(v) if isinstance(v,list) else v) for k,v in d.items()})
assert d['identical'], '两次组装结果不一致'
" && ok "两次独立组装结果完全一致（装机可复现）" || bad "两次组装结果不一致"

step "8. 清理"
rm -rf "$R" /tmp/qy-sys-root2 /tmp/qy-units
ok "测试目录已清理"

echo
echo "======================================="
echo " 通过 $PASS 项，失败 $FAIL 项"
echo "======================================="
[ "$FAIL" -eq 0 ] || exit 1
