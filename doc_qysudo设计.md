qysudo 设计 (v1.6.0):
- setuid-root C 程序 /usr/bin/qysudo
- 校验: /etc/qysudoers (格式: user ALL=(ALL) ALL; 组: %wheel ALL=(ALL) ALL)
- 校验当前用户密码: 读 /etc/shadow (getspnam 需 libshadow? 自实现: 解析 shadow 文件行, crypt() 校验, 链接 libcrypt)
- PAM 不引 (自研精简)
- 执行: fork+setgid(0)+setuid(0)+execvp 命令
- 安全: 组为 wheel + /etc/shadow root:600
配套:
- /etc/shadow (root:600): root 密码默认空锁 (!), 首次登录 qyinit 提示设置? v1.6.0: root 无密码锁住, 用户用 busybox passwd root 设置
- busybox adduser -D? LANXIU无 useradd → 提供 qyuseradd 包装脚本调 busybox adduser + 自动加 wheel 组
