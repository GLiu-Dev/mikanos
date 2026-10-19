
```
正确的启动链（run_qemu.sh 帮你做的）：
QEMU ──加载──▶ OVMF 固件 ──从 FAT 盘查找──▶ EFI/BOOT/BOOTX64.EFI(加载器)
        ──加载器读取──▶ 根目录 kernel.elf ──▶ KernelMain 画渐变+AA

你现在卡在这里：
QEMU ──加载──▶ OVMF 固件 ──找不到可启动盘──▶ PXE 网络启动（就是你看到的画面）
```

## 标准编译 + 启动流程（osbook_day05a 属于经典 era，用的是 `run_qemu.sh`）

**① 构建内核**

```
cd $HOME/osbook/mikanos/kernel
git checkout osbook_day05a
source $HOME/osbook/devenv/buildenv.sh
make clean
make          # 生成 kernel/kernel.elf
```

**② 构建 UEFI 加载器（需要先把 MikanLoaderPkg 链到 edk2）**

```
cd $HOME/edk2
ln -sfn $HOME/osbook/mikanos/MikanLoaderPkg .
source edksetup.sh
build         # 生成 Build/MikanLoaderX64/DEBUG_CLANG*/X64/Loader.efi
```

**③ 关键一步：用配套脚本启动**（不是直接 qemu 裸跑）

```
$HOME/osbook/devenv/run_qemu.sh \
  $HOME/edk2/Build/MikanLoaderX64/DEBUG_CLANG38/X64/Loader.efi \
  $HOME/osbook/mikanos/kernel/kernel.elf
```

`run_qemu.sh` 做三件事：把 FAT 盘里放入 `EFI/BOOT/BOOTX64.EFI`（= 加载器改名）和 `kernel.elf`、写入 `startup.nsh`，再以 `-drive if=pflash,file=...OVMF_CODE.fd` + `-drive ...OVMF_VARS.fd` + `-drive ...hd.img` 启动 QEMU。

## 如果 `run_qemu.sh` 缺失或想手工排查，等价命令

```
cd $HOME/osbook/devenv
qemu-img create -f raw hd.img 200M
mkfs.fat -n 'MIKANOS' -s 2 -f 2 -R 32 -F 32 hd.img
mkdir -p mnt
sudo mount -o loop hd.img mnt
sudo mkdir -p mnt/EFI/BOOT
sudo cp <Loader.efi>  mnt/EFI/BOOT/BOOTX64.EFI     # 必须放在这个路径且改名
sudo cp <kernel.elf>  mnt/kernel.elf               # 加载器固定读 \kernel.elf
sudo sh -c 'echo "BOOTX64.EFI" > mnt/startup.nsh'
sudo umount mnt
qemu-system-x86_64 -m 1G \
  -drive if=pflash,format=raw,readonly,file=$HOME/osbook/devenv/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$HOME/osbook/devenv/OVMF_VARS.fd \
  -drive file=$HOME/osbook/devenv/hd.img,format=raw,if=ide
```

