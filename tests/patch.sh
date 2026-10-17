#!/usr/bin/env bash
# 补丁管理与多架构仓库专项测试
set -o pipefail
cd "$(dirname "$0")/.." || exit 1
PASS=0; FAIL=0
ok(){ echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad(){ echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
step(){ echo; echo "== $1"; }
TMP=/home/agentuser/qyw/.qy-patch
qyrm(){ for _ in 1 2 3 4 5; do rm -rf "$@" 2>/dev/null; gone=1; \
  for t in "$@"; do [ -e "$t" ] && gone=0; done; [ "$gone" -eq 1 ] && return 0; sleep 1; done; return 1; }
qyrm "$TMP"; mkdir -p "$TMP"

step "1. 补丁清单能加载"
./bin/qypatch list > "$TMP/l.txt" 2>&1
grep -q "全系统共" "$TMP/l.txt" && ok "能列出全系统补丁" || bad "列不出补丁"
grep -q "qydemo" "$TMP/l.txt" && ok "样例补丁在清单里" || bad "样例补丁缺失"

step "2. 补丁文件与声明一致"
./bin/qypatch verify >/dev/null 2>&1 && ok "校验通过" || bad "校验失败"

step "3. 声明不完整必须报错"
python3 - <<'PYEOF'
import sys; sys.path.insert(0, '.')
from pathlib import Path
from qyos import patch as P
root = Path('.').resolve()
f = "qydemo-verbose-output.patch"
real = P.util.sha256_file(root/'patches'/f)
# 安全补丁缺 CVE
assert P.verify_patch(root, P.Patch(file=f, sha256=real, kind="security", reason="x")), \
    "安全补丁缺 CVE 没报错"
print("  安全补丁缺 CVE → 报错")
# 上游补丁缺 commit
assert P.verify_patch(root, P.Patch(file=f, sha256=real, kind="upstream", reason="x")), \
    "上游补丁缺 commit 没报错"
print("  上游补丁缺 commit → 报错")
# 未锁定 sha256
assert P.verify_patch(root, P.Patch(file=f, kind="distro", reason="x")), \
    "未锁定 sha256 没报错"
print("  未锁定 sha256 → 报错")
# 校验和不符
assert P.verify_patch(root, P.Patch(file=f, sha256="0"*64, kind="distro", reason="x")), \
    "校验和不符没报错"
print("  校验和不符 → 报错")
# 正常的不能误报
assert not P.verify_patch(root, P.Patch(file=f, sha256=real, kind="distro", reason="x")), \
    "正常补丁被误报"
print("  正常补丁 → 无误报")
PYEOF
[ $? -eq 0 ] && ok "四类问题全部检出且无误报" || bad "校验逻辑不正确"

step "4. 补丁能真正打上源码"
python3 - <<'PYEOF'
import sys, shutil; sys.path.insert(0, '.')
from pathlib import Path
from qyos import patch as P
root = Path('.').resolve()
srcdir = Path('/tmp/qy-patch-test')
shutil.rmtree(srcdir, ignore_errors=True); srcdir.mkdir(parents=True)
(srcdir/'main.c').write_text(
    '#include <stdio.h>\n#include <qydemo.h>\n\nint main(void)\n{\n'
    '    printf("qydemo %s\\n", qydemo_version());\n'
    '    printf("1 + 2 = %d\\n", qydemo_add(1, 2));\n    return 0;\n}\n')
lst = P.load_patches(root, root/'recipes').get('qydemo', [])
assert lst, "没加载到补丁"
applied = P.apply_patches(root, 'qydemo', srcdir.resolve(), lst)
assert applied, "补丁没应用"
txt = (srcdir/'main.c').read_text()
assert "LANXIU Linux" in txt, f"补丁没生效: {txt[:120]}"
print("  补丁已生效，源码含 'LANXIU Linux'")
PYEOF
[ $? -eq 0 ] && ok "补丁真正修改了源码" || bad "补丁未生效"

step "5. 打不上必须报错，不能静默跳过"
python3 - <<'PYEOF'
import sys, shutil; sys.path.insert(0, '.')
from pathlib import Path
from qyos import patch as P
root = Path('.').resolve()
srcdir = Path('/tmp/qy-patch-fail'); shutil.rmtree(srcdir, ignore_errors=True)
srcdir.mkdir(parents=True)
(srcdir/'other.txt').write_text("nothing to patch here\n")
badp = Path('/tmp/qy-nomatch.patch')
badp.write_text("--- a/does-not-exist\n+++ b/does-not-exist\n"
                "@@ -1,1 +1,1 @@\n-aaa\n+bbb\n")
assert P._apply_one(badp, srcdir, P.Patch(file="x")) is False, \
    "打不上竟返回成功"
print("  不匹配的补丁 → _apply_one 返回 False")
# 通过 apply_patches 必须抛错
lst = [P.Patch(file=str(badp), sha256=P.util.sha256_file(badp),
               kind="distro", reason="x")]
try:
    P.apply_patches(root, 'x', srcdir, lst)
    raise AssertionError("打不上竟然没抛错")
except P.PatchError as e:
    assert "打不上" in str(e)
    print("  apply_patches → 抛 PatchError（不会静默跳过）")
PYEOF
[ $? -eq 0 ] && ok "打不上会失败而非静默跳过" || bad "静默跳过风险未排除"

step "6. 打过的补丁写进包元数据"
qyrm var/work
./bin/qybuild qydemo --force >/dev/null 2>&1
python3 - <<'PYEOF'
import sys; sys.path.insert(0, '.')
from qyos import format as F
from pathlib import Path
p = F.read_package(Path('var/pkgs/qydemo-0.1.0-1.x86_64.qyp'))
assert p.meta.patches, "包里没记录补丁"
entry = p.meta.patches[0]
assert entry["file"] == "qydemo-verbose-output.patch"
assert entry["reason"], "没记原因"
print(f"  包内记录: {entry['file']}（{entry['kind']}）")
print(f"  原因: {entry['reason']}")
PYEOF
[ $? -eq 0 ] && ok "包元数据记录了打了哪些补丁" || bad "补丁未记入元数据"

step "7. 仓库多架构支持"
./bin/qyrepo sync --sign var/repo/keys/qiyuan >/dev/null 2>&1
./bin/qyrepo archs > "$TMP/a.txt" 2>&1
grep -q "x86_64" "$TMP/a.txt" && ok "能列出架构" || bad "列不出架构"
grep -q "合计" "$TMP/a.txt" && ok "有合计" || bad "无合计"
./bin/qyrepo manifest --sign var/repo/keys/qiyuan > "$TMP/m.txt" 2>&1
grep -q "已生成" "$TMP/m.txt" && ok "能生成多架构清单" || bad "清单生成失败"
[ -f var/repo/manifest.json ] && ok "清单文件存在" || bad "清单文件缺失"
[ -f var/repo/manifest.json.sig ] && ok "清单已签名" || bad "清单未签名"

step "8. 架构一致性校验能抓出错配"
python3 - <<'PYEOF'
import sys, json, shutil; sys.path.insert(0, '.')
from qyos import repo as R
from pathlib import Path
# 造一个索引说 aarch64 但包是 x86_64 的仓库
fake = Path('/tmp/qy-fake-repo')
shutil.rmtree(fake, ignore_errors=True)
(fake/'aarch64').mkdir(parents=True)
shutil.copy('var/pkgs/qydemo-0.1.0-1.x86_64.qyp', str(fake/'aarch64'))
idx = {"format": 1, "distro": "LANXIU Linux", "arch": "aarch64",
       "generated": 0, "count": 1,
       "packages": [{"name": "qydemo", "version": "0.1.0", "release": 1,
                     "arch": "aarch64", "pkgid": "qydemo-0.1.0-1",
                     "filename": "qydemo-0.1.0-1.x86_64.qyp",
                     "sha256": "", "size": 0, "depends": [],
                     "provides": [], "summary": ""}]}
(fake/'aarch64'/'index.json').write_text(json.dumps(idx))
probs = R.check_arch_consistency(fake, 'aarch64')
assert probs, "错配架构没被检出"
print("  检出:", probs[0][:70])
# 正常的仓库不该报
assert not R.check_arch_consistency(Path('var/repo'), 'x86_64'), \
    "正常仓库被误报"
print("  正常仓库 → 无误报")
PYEOF
[ $? -eq 0 ] && ok "错配架构能检出且不误报" || bad "架构校验不正确"

step "9. 清单可被验签"
python3 - <<'PYEOF'
import sys, json; sys.path.insert(0, '.')
from qyos import format as F
from pathlib import Path
rd = Path('var/repo')
payload = (rd/'manifest.json').read_bytes()
sig = json.loads((rd/'manifest.json.sig').read_text())
pub = rd/'keys'/'qiyuan.pub'
assert F.verify_digest(bytes.fromhex(F.util.sha256_bytes(payload)),
                       sig, pub), "清单验签失败"
print("  清单签名校验通过")
# 篡改后必须失败
bad = payload.replace(b'"architectures"', b'"architecturesX"')
assert not F.verify_digest(bytes.fromhex(F.util.sha256_bytes(bad)), sig, pub)
print("  篡改后验签失败")
PYEOF
[ $? -eq 0 ] && ok "清单可验签且篡改会被发现" || bad "清单签名有问题"

step "10. 清理"
qyrm "$TMP" /tmp/qy-patch-test /tmp/qy-patch-fail /tmp/qy-nomatch.patch /tmp/qy-fake-repo
ok "测试产物已清理"

echo
echo "======================================="
echo " 通过 $PASS 项，失败 $FAIL 项"
echo "======================================="
[ "$FAIL" -eq 0 ] || exit 1
