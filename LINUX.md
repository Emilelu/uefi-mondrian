# Redmi K60 的 Linux initramfs 启动分支

本分支保存实际通过 L2 启动验证的 UEFI 源码改动，保留 Emilelu 上游历史。
原始基线为 1a7afe7d78eb0cad6c0fbb63ff1309a530c7172e。
手机已启动 Linux 7.3.3 自建 initramfs，用户确认 CPUs 0-7 在线、心跳持续且没有重启。
实际 uptime 数值未提供；USB ACM/L3 尚未实现。

## 构建入口

配套移植源码与完整步骤在 https://github.com/lty2ytl31/linux-mondrian 的 verified/boot 分支。
先指定工作区绝对路径，将配套仓库克隆为该工作区的 recipes/，准备用户提供的
linux-7.3.3.tar.xz，并按配套 README 配置 workspace.env。

```bash
# WORKSPACE_DIR 必须先设置为用户选定的绝对路径。
cd "$WORKSPACE_DIR"
python3 recipes/scripts/fetch-locked-sources.py
bash recipes/scripts/install-fedora-deps.sh
bash recipes/scripts/prepare-kernel.sh
bash recipes/scripts/install-uefi-python.sh
JOBS=4 bash recipes/scripts/build-l2.sh
```

依赖入口完整克隆所有必要 Git 仓库，不使用浅克隆。Mu_Basecore 固定到已经
实际编译的 6f805f26a4366fe56790130dc2f8f3e1b4c2fe28；原来的
3d2a9fab4eea41634a0cb4a2feb2580afc59d090 对象不可获取，本分支已更新 gitlink。
两者不被视为等价，细节见配套 facts/uefi-basecore-override.md。

内核、native /init、BootShim 的 make 均带 O=。UEFI/BaseTools 在
build/uefi-out/workspace-* 的导出副本中构建，原始仓库保持无编译产物。
配套 staging 脚本既识别原始上游源码，也识别本分支中已应用的同一文本补丁，
不会重复应用。完整依赖版本由配套 sources.lock.json 固定。

## DTB 与 initramfs

DTB 从配套仓库的手写 kernel/sm8475-xiaomi-mondrian.dts 构建，继承 Linux
7.3.3 的 sm8450.dtsi；未反编译任何现成 DTB。只在独立 build 副本内替换
Platforms/Xiaomi/mondrianPkg/FdtBlob/sm8475-xiaomi-mondrian.dtb。
本分支不提交新编译的 DTB 或 FD，直接沿用仓库历史中的 DTB 不能代表这次 Linux 配置。

Linux Image、手写 DTB、initramfs 和 unixnu.toml 放入带 MONDRIAN 长度/CRC32
头的 FAT ramdisk。UniXNU 使用 EFI LoadOptions、FDT table 与 LoadFile2 加载它们。
原始已启动镜像 SHA256 为
c87b3211b5be06640a8714e9f075e54fd761193a9a0de92f25a0fa997aead49e。

## 已知范围

只使用低位 DXE heap，暂不扩展高位 DDR；原生显示、存储、USB、挂起和调频未启用。
Linux 仍存在 PSCI 电源域 provider 缺失造成的延迟探测，不能声称所有时钟或电源管理已适配。
新编译的镜像仍须单独验收，编译通过不会自动获得 L2。

请用户执行的手动步骤：解锁 bootloader、进入 fastboot，完成 VMware USB 直通，
确认后先检查设备。只允许单次 fastboot boot，禁止 flash。临时启动可能黑屏、
死机、复位，极端情况下存在变砖风险；长按电源或回 fastboot 后启动原 Android 回滚。
脚本遇到不可见设备或检查失败会停止，不循环重试。
