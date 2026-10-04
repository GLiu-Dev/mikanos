# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application


下面从**整体定位、函数拆解、引导流程、关键机制、潜在问题**五个维度分析，并附一张流程图。

## 一、整体定位

这是运行在 UEFI 固件上的一个**标准 UEFI 应用程序**（不是传统 BIOS 引导），是 MikanOS 从"读取到内存映射、点亮图形"到"真正加载内核并跳转"的关键一步。day03b 里程碑的核心：**把控制权从固件完整交接给内核**。

它完成了引导的完整闭环：收集内存信息 → 打开文件系统 → 初始化图形 → 加载内核 → 退出固件服务 → 跳转内核。

## 二、函数拆解

| 函数 | 作用 | 关键点 |
| --- | --- | --- |
| `struct MemoryMap` | 封装 UEFI 内存映射 | 缓存 `buffer` 与 `map_size`/`map_key`/`descriptor_size` 等输出 |
| `GetMemoryMap()` | 包装 `gBS->GetMemoryMap` | `map_key` 是后续 `ExitBootServices` 的握手凭证 |
| `GetMemoryTypeUnicode()` | 内存类型枚举 → 宽字符串 | 仅调试输出用 |
| `SaveMemoryMap()` | 把映射写成 CSV 存到 `\memmap` | 用 `descriptor_size` 步进遍历（而非 `sizeof`） |
| `OpenRootDir()` | 打开 ESP 根目录 | `image_handle → LoadedImage → DeviceHandle → SimpleFileSystem → OpenVolume` |
| `OpenGOP()` | 查找并打开图形输出协议 | `LocateHandleBuffer` 全局搜索 + `OpenProtocol` + `FreePool` |
| `GetPixelFormatUnicode()` | 像素格式枚举 → 宽字符串 | 说明 RGB/BGR 字节顺序差异 |
| `UefiMain()` | 入口，7 阶段总指挥 | `EFIAPI` 调用约定、`gBS` 全局表 |

## 三、引导流程（7 个阶段）

下面这张图把 `UefiMain` 的执行流程和"Boot Services 生命周期"对应起来：

![alt text](image.png)

## 四、关键机制详解

**1. EFIAPI 调用约定（x86-64 = Microsoft ABI）**
`UefiMain` 用 `EFIAPI` 宏声明。前四个整型参数经 `RCX/RDX/R8/R9` 传递、调用者清栈。若误写成 System V 约定，参数会错位、导致崩溃——这是 UEFI 开发最容易踩的坑。

**2. `map_key` 与 `ExitBootServices` 的"快照握手"**
`GetMemoryMap` 输出的 `map_key` 是固件对"内存快照"的版本号。`ExitBootServices` 传入它，固件据此校验**从快照到退出之间内存分配是否被改过**。若期间有事件回调动了内存，`map_key` 失效、调用返回错误。所以代码做了**重取映射 + 立即重试**的补救，且强调重试之间不能有任何可能触发内存操作的调用（连 `Print` 都不行）。

**3. 为什么用 `descriptor_size` 步进遍历**
`EFI_MEMORY_DESCRIPTOR` 是可变大小结构，固件可能加填充对齐，`descriptor_size ≥ sizeof(结构体)`。遍历必须用 `map->descriptor_size` 而非 `sizeof`，否则在填充多的固件上会错位、读到错误数据。`SaveMemoryMap` 里正是这样做的。

**4. 为什么内核加载到 0x100000（1 MiB）**
x86 低 1 MiB 是历史遗留区：0x00000 中断向量表、0x400 BIOS 数据区、0xA0000 传统 VGA 显存、0xF0000 BIOS ROM 影子区。内核避开这些，从 1 MiB 起是安全约定，且与内核链接脚本 LMA/VMA 对齐。

**5. 为什么跳转用"ELF 偏移 24 读 e_entry"**
`Elf64_Ehdr` 中 `e_entry` 位于固定偏移 24（8 字节）。引导程序假设内核**恒等映射**（虚拟地址 == 物理地址），所以直接把这个虚拟地址当物理地址调用，无需建立页表。这是 day03 阶段尚未做分页的简化假设。

**6. 清屏为什么逐字节写 255 就是白色**
对最常见的 RGB/BGR 32 位格式，R/G/B 三通道均为 0xFF 即为最大亮度白。用 `UINT8*` 逐字节写而非按像素写，是**对任意像素格式都安全**的写法（PixelBitMask 下不保证白，但本程序假设标准 RGB/BGR）。

**7. GOP 为什么必须 `LocateHandleBuffer` 搜索**
GOP 安装在**显示控制器设备句柄**上，不是引导程序自己的 `image_handle`。`image_handle` 只关联了 `EFI_LOADED_IMAGE_PROTOCOL`，必须用协议 GUID 全局搜索才能找到显示设备。

## 五、潜在问题与改进点（如实指出）

| 问题 | 位置 | 说明/建议 |
| --- | --- | --- |
| **返回码未逐层检查** | `OpenRootDir`、`OpenGOP`、各 `Open`/`GetInfo`/`Read` | 生产代码应逐步 `EFI_ERROR` 检查并回退，这里为教学简化而省略 |
| **`gop_handles[0]` 越界风险** | `OpenGOP` | 若 `num_gop_handles == 0`，访问下标 0 是未定义行为；应先判 > 0 |
| **整 ELF 直接读入，未按段映射** | 阶段 5 | 依赖链接脚本保证平坦布局与加载地址一致；正规做法应按 `PT_LOAD` 段分别映射 |
| **`Attribute & 0xffffflu` 掩码** | `SaveMemoryMap` | 只取属性低 20 位，属简化输出，非标准全属性 |
| **`GetMemoryMap` 首调未判 buffer 太小** | `GetMemoryMap` | 栈上 16 KiB 通常足够，但严谨做法是失败后用 `AllocatePool` 扩容重试 |
| **GOP 只取第一块** | `OpenGOP` | 多显示器系统未做选择，仅适用主屏场景 |

## 六、小结

这份代码把"UEFI 应用 → 内核"的交接流程完整打通，是 MikanOS 图形化与内核加载的里程碑。它的设计围绕一条核心主线——**尽可能在 Boot Services 有效期内做完所有"需要固件帮忙"的事（内存/文件/图形/协议），一旦 `ExitBootServices` 成功，立刻切换到纯硬件操作（帧缓冲区直写、内存地址跳转）**。所有注释里反复强调的"退出后 gBS 失效"正是 UEFI 与内核编程的分水岭，也是理解这份代码价值的钥匙。
