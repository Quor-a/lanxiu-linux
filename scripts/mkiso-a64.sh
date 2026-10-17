#!/bin/bash
# mkiso-a64.sh — LANXIU Linux aarch64 live ISO 打包
# 前置：
#   1. var/pkgs/kernel-arm64-7.2.9-1.aarch64.qyp（ARM64 Image，qybuild 交叉构建）
#   2. /tmp/a64rootfs（55 包全量 rootfs，qypkg assemble --arch aarch64 组装）
#   3. grubaa64.efi（apt install cd-boot-images-arm64 提供）
#   4. mtools/xorriso（apt install mtools xorriso）
# 用法: bash scripts/mkiso-a64.sh
set -e
Q="$(cd "$(dirname "$0")/.." && pwd)"
ISO=/tmp/qiyuan-aarch64-live.iso

# ---- 1. ARM64 内核 ----
rm -rf /tmp/kchk && mkdir -p /tmp/kchk && cd /tmp/kchk
sh "$Q/../andrepo/scripts/qyextract.sh" "$Q/var/pkgs/kernel-arm64-7.2.9-1.aarch64.qyp" 2>/dev/null \
  || sh /home/agentuser/andrepo/scripts/qyextract.sh "$Q/var/pkgs/kernel-arm64-7.2.9-1.aarch64.qyp"
VMLINUZ=/tmp/kchk/boot/vmlinuz-7.2.9-arm64
[ -f "$VMLINUZ" ] || { echo "缺 ARM64 内核"; exit 1; }

# ---- 2. live initramfs（静态 aarch64 busybox + 脚本 init）----
BB="$Q/var/sysroot-target/usr/bin/busybox"
[ -x "$BB" ] || { echo "缺静态 aarch64 busybox（var/sysroot-target）"; exit 1; }
rm -rf /tmp/a64ir && mkdir -p /tmp/a64ir/{bin,dev,proc,sys,newroot,run,media,ro,upper}
cp "$BB" /tmp/a64ir/bin/busybox
cat > /tmp/a64ir/init <<'EOF'
#!/bin/busybox sh
/bin/busybox --install -s /bin 2>/dev/null
mkdir -p /proc /sys /dev /newroot /run /media
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
echo "[a64-live-init] 查找 live 介质（sr0/vda）..."
FOUND=""
for i in $(seq 1 20); do
  for dev in /dev/sr0 /dev/vda /dev/vdc; do
    [ -b "$dev" ] || continue
    if mount -t iso9660 -o ro "$dev" /media 2>/dev/null; then FOUND="$dev"; break; fi
  done
  [ -n "$FOUND" ] && break
  sleep 1
done
[ -n "$FOUND" ] || { echo "[a64-live-init] 未找到介质，进救援"; exec /bin/sh; }
echo "[a64-live-init] 介质: $FOUND"
[ -f /media/live/rootfs.squashfs ] || { echo "无 rootfs.squashfs"; exec /bin/sh; }
mkdir -p /ro
mount -t squashfs -o loop,ro /media/live/rootfs.squashfs /ro || exec /bin/sh
mkdir -p /upper
mount -t tmpfs -o size=512m tmpfs /upper
mkdir -p /upper/upper /upper/work /newroot
mount -t overlay overlay -o lowerdir=/ro,upperdir=/upper/upper,workdir=/upper/work /newroot || { echo "overlay 失败"; exec /bin/sh; }
echo "[a64-live-init] switch_root -> /usr/bin/qyinit"
exec switch_root /newroot /usr/bin/qyinit
EOF
chmod 755 /tmp/a64ir/init
cd /tmp/a64ir && find . | cpio -o -H newc 2>/dev/null | gzip -9 > /tmp/a64-live-initramfs.img

# ---- 3. squashfs（55 包 rootfs）----
rm -rf /tmp/qyiso-a64 && mkdir -p /tmp/qyiso-a64/{live,boot/grub}
sudo mksquashfs /tmp/a64rootfs /tmp/qyiso-a64/live/rootfs.squashfs -comp zstd -e /tmp/a64rootfs/var/lib/qypkg -noappend
cp "$VMLINUZ" /tmp/qyiso-a64/boot/vmlinuz
cp /tmp/a64-live-initramfs.img /tmp/qyiso-a64/boot/initramfs.img
cat > /tmp/qyiso-a64/boot/grub/grub.cfg <<'EOF'
set timeout=3
set default=0
menuentry "LANXIU Linux aarch64 (live)" {
    linux /boot/vmlinuz console=ttyAMA0
    initrd /boot/initramfs.img
}
EOF

# ---- 4. ESP + ISO ----
GRUBAA64=${GRUBAA64:-/usr/share/cd-boot-images-arm64/tree/efi/boot/grubaa64.efi}
rm -f /tmp/esp-a64.img
dd if=/dev/zero of=/tmp/esp-a64.img bs=1M count=96 2>/dev/null
mformat -i /tmp/esp-a64.img -C -T 196000 -v QYEFI ::
mmd -i /tmp/esp-a64.img ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub
mcopy -i /tmp/esp-a64.img "$GRUBAA64" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i /tmp/esp-a64.img "$VMLINUZ" ::/boot/vmlinuz
mcopy -i /tmp/esp-a64.img /tmp/a64-live-initramfs.img ::/boot/initramfs.img
mcopy -i /tmp/esp-a64.img /tmp/qyiso-a64/boot/grub/grub.cfg ::/boot/grub/grub.cfg
rm -f "$ISO"
xorriso -as mkisofs -V QIYUAN_A64 -r -J -append_partition 2 0xef /tmp/esp-a64.img \
  -partition_offset 16 -o "$ISO" /tmp/qyiso-a64 2>&1 | tail -1
ls -la "$ISO"
echo "A64-ISO-COMPLETE"
