# qyinstall 安装器设计（v1.5.0）

## 目标
从 live ISO 把LANXIU安装到硬盘，重启后独立引导（不依赖光盘）。

## 架构决策（为什么这样做）
1. **目标引导 = EFISTUB**，不用 GRUB：
   - 内核已开 CONFIG_EFI_STUB=y，可直接 `efibootmgr`/efivar 写启动项 → 零 bootloader 包依赖
   - 系统内没有 grub-install/grub-mkrescue（那两个只在宿主机有）→ 自举反而最简
2. **分区**：sfdisk（util-linux 已在）写 GPT/MBR：p1 EFI 256M vfat + p2 root 其余 ext4
   - mkfs.vfat 缺 → busybox 无 mkfs.vfat → 装 dosfstools（新配方）**或** EFI 分区用 mke2fs? UEFI 规范要求 FAT
   - 决策：新配方 dosfstools（极小，无依赖），busybox 无 mkfs.vfat
3. **复制**：unsquashfs 缺 → 安装器 C 程序直接用 libarchive? 不引库 → 用 dd + squashfs 挂载后 cp -a：
   - mount -o loop rootfs.squashfs /mnt/src; cp -a /mnt/src/* /mnt/target
   - squashfs 内核支持已有 → 零用户态依赖 ✓
4. **EFI 启动项**：efibootmgr 缺 → 两方案：
   a) 新配方 efibootmgr (efivar 库)
   b) 直接写 NVRAM 太复杂 → **fallback: EFI 分区放 /EFI/BOOT/BOOTX64.EFI = 拷贝 vmlinuz（EFI_STUB 内核可直接是 EFI 可执行！）+ 启动参数放 EFI 分区** — 但 stub 需要 cmdline 内嵌或附加
   - 决策：内置 cmdline 到内核 (CONFIG_CMDLINE) 或用附加 .cmdline 文件（efi stub 支持 loader .linux/.initrd/.cmdline 协议）
   - v1.5.0 简化：BOOTX64.EFI=vmlinuz + /EFI/BOOT/linux.cmdline 文件? — EFI stub 不读外部 cmdline 文件，只有 systemd-boot/GRUB 会
   - **最终 v1.5.0 方案**：新配方 efibootmgr + efivar，安装后 efibootmgr -c 写 NVRAM 项；QEMU 测试用 OVMF
5. **界面**：先 CLI（qyinstall /dev/sda），GUI 后续（qysettings 内嵌页）

## 步骤（qyinstall CLI，C 或 bash？）
→ **bash 脚本**（安装是一次性流程编排，busybox sh 可跑，C 编排反而繁）：
1. sfdisk 分区
2. mkfs.vfat p1 + mkfs.ext4 p2
3. mount → cp -a（从挂载的 squashfs）+ 拷 /boot（vmlinuz/System.map/config）
4. 生成 /etc/fstab (UUID)
5. 拷 initramfs（最小 root 直启版，非 live 版 — 新 initramfs: root=UUID 直挂 ext4）
6. mkfs.vfat 的 p1 拷 EFI/BOOT/BOOTX64.EFI=vmlinuz（stub 直启，cmdline 用 efibootmgr -c -u 传）
7. efibootmgr 写启动项
8. 摘载 → 完成

## 待补配方
- dosfstools（mkfs.vfat）
- efibootmgr + efivar
- 安装版 initramfs（root= 参数直挂）
