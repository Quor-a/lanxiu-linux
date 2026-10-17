#!/bin/bash
# 墨语言测试
#   tests/*.mo         正常运行用例，期望退出码写在首行注释：# expect: N
#   tests/*/           多文件用例，目录含 main.mo 与若干库文件
#   tests/errors/*.mo  编译错误用例，期望报错含：# expect-error: <片段>
CC=${1:-$(cd "$(dirname "$0")/.." && pwd)/bin/moc}
CC="$(cd "$(dirname "$CC")" && pwd)/$(basename "$CC")"
pass=0; fail=0

run_one() {
  local name=$1 exp=$2
  ( cd /tmp/mot && "$CC" ) >/dev/null 2>&1 || { echo "FAIL(编译) $name"; fail=$((fail+1)); return; }
  chmod +x /tmp/mot/out.elf 2>/dev/null
  # 必须在 /tmp/mot 里跑：原来是在仓库根目录跑，
  # 于是凡是碰文件系统的用例（建目录、写文件）都会污染仓库，
  # 第二次跑就因为 "已存在" 而失败 —— mkdir 返回 EEXIST 被当成错误。
  ( cd /tmp/mot && ./out.elf ); got=$?
  if [ "$got" = "$exp" ]; then echo "PASS $name = $got"; pass=$((pass+1));
  else echo "FAIL $name 期望 $exp 实得 $got"; fail=$((fail+1)); fi
}

for f in $(cd "$(dirname "$0")/.." && pwd)/tests/*.mo; do
  exp=$(grep -m1 '# expect:' "$f" | sed 's/.*expect: *//')
  [ -z "$exp" ] && exp=0
  rm -rf /tmp/mot && mkdir -p /tmp/mot && cp lib/*.mo /tmp/mot/ 2>/dev/null; cp "$f" /tmp/mot/test.mo
  run_one "$(basename $f)" "$exp"
done

for d in $(cd "$(dirname "$0")/.." && pwd)/tests/*/; do
  [ "$(basename $d)" = "errors" ] && continue
  [ -f "$d/main.mo" ] || continue
  exp=$(grep -m1 '# expect:' "$d/main.mo" | sed 's/.*expect: *//')
  [ -z "$exp" ] && exp=0
  rm -rf /tmp/mot && mkdir -p /tmp/mot
  # 标准库始终从 lib/ 取最新一份，再拷测试自己的文件（可覆盖）。
  # 这样 tests/*/ 下不需要维护库副本，也就不会出现副本与 lib/ 不同步。
  cp $(cd "$(dirname "$0")/.." && pwd)/lib/*.mo /tmp/mot/
  # 【LANXIU】拷整个测试目录内容（含子目录数据），保持相对路径结构
  cp -r "$d"* /tmp/mot/ && mv /tmp/mot/main.mo /tmp/mot/test.mo
  run_one "$(basename $d)/" "$exp"
done

# 编译错误用例：必须编译失败，且报错包含期望片段
for f in $(cd "$(dirname "$0")/.." && pwd)/tests/errors/*.mo; do
  frag=$(grep -m1 '# expect-error:' "$f" | sed 's/.*expect-error: *//')
  rm -rf /tmp/mot && mkdir -p /tmp/mot && cp "$f" /tmp/mot/test.mo
  out=$( cd /tmp/mot && "$CC" 2>&1 )
  rc=$?
  if [ $rc -eq 0 ]; then echo "FAIL $(basename $f) 应报错但编译通过"; fail=$((fail+1));
  elif [ -n "$frag" ] && ! echo "$out" | grep -q "$frag"; then
    echo "FAIL $(basename $f) 报错不含 '$frag'（实得：$out）"; fail=$((fail+1));
  else echo "PASS $(basename $f) 已拦下：$out"; pass=$((pass+1)); fi
done

echo "通过 $pass / 失败 $fail"
