# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application

 `osbook_day03d → osbook_day04a` **唯一的改动就是新增了 `kernel/Makefile`**，源代码一行没动（`kernel/main.cpp` 在 day03d 就已存在）。

## 改动概览

| 项 | 内容 |
| --- | --- |
| 改动文件 | `kernel/Makefile`（新增，20 行） |
| 涉及提交 | ① `8fe697a` add Makefile for the kernel   ② `1765af5` use -std=c++17 in kernel/Makefile |
| 源码变更 | 无（`main.cpp` 未变） |
| 目录结构 | 仓库根目录、kernel 目录均与 day03d 相同 |

第②个提交只做了一件事：在 `CXXFLAGS` 里追加 `-std=c++17`，用编译期特性版本取代了 `clang++` 的默认标准。

## Makefile 逐段解析

**目标与对象**

```
TARGET = kernel.elf
OBJS = main.o
```

产物是从 `main.cpp` 编译链接出的裸机内核镜像 `kernel.elf`。

**编译选项（clang++）**

```
-O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone
-fno-exceptions -fno-rtti -std=c++17
```

- `--target=x86_64-elf`：交叉编译到裸机 x86_64 ELF，不依赖宿主 OS
- `-ffreestanding`：无标准库/宿主环境假设
- `-mno-red-zone`：禁用红区 —— 内核里中断/汇编不会按 ABI 预留 128 字节红区，必须关掉
- `-fno-exceptions -fno-rtti`：关闭 C++ 异常与运行时类型信息（内核无运行时支持）
- `-std=c++17`：按 C++17 编译

**链接选项（ld.lld）**

```
--entry KernelMain -z norelro --image-base 0x100000 --static
```

- `--entry KernelMain`：入口符号为 `KernelMain`
- `--image-base 0x100000`：内核加载到物理地址 1MB（0x100000），避开 BIOS/引导区域，是经典内核装载点
- `-z norelro` + `--static`：固定地址静态链接，不启用动态加载相关的 RELRO

**规则**

```
all:     默认目标，构建 kernel.elf
clean:   删除 *.o
kernel.elf: 由 OBJS + Makefile 变更触发重链接（ld.lld）
%.o: %.cpp  模式规则，clang++ 逐个编译
```

注意 `kernel.elf` 和 `%.o` 都把 `Makefile` 列为依赖，改 Makefile 会触发重编译/重链接。

**与 day03 的关系**：day03 时内核代码已能编译出可运行结果（`main.cpp` 里 `KernelMain` 填满 framebuffer 后 `hlt` 死循环），但构建是靠命令手动完成；day04a 这一步把构建固化成了可复用的 Makefile，为后续代码量增长铺路。至此内核源码仍是「一个 C++ 文件 + 一个 Makefile」的最小结构。