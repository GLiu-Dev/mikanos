# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application


**小结**：day03a 的价值不在 "画了什么"，而在搭建了一条可运行、可调试点对点的启动链路 ——**UEFI 固件 → 内存映射 → ELF 载入 → ExitBootServices → 跳转内核 → hlt 空转**。后续 day03 的帧缓冲（framebuffer）和字符显示，就是在这个基础上、替换掉内核的 hlt 占位去实现的。


osbook_day03a 启动流程架构

UEFI 引导加载器 → 载入内核 → 退出 Boot Services → 移交 CPU 控制权

![alt text](image.png)



主模式仍为 `html_svg`（静态）。下面是这份 loader `Main.c` 的逐函数深度分析，最后附一张函数调用结构图。

## 文件定位

这是 **UEFI 引导加载器（UEFI Application）**，运行在固件阶段，只做一件事：**采集内存映射 → 从磁盘读入内核 ELF → 退出 Boot Services → 跳转到内核**。共 1 个入口 `UefiMain` + 4 个辅助函数。它用的所有服务都来自全局 `gBS`（`EFI_BOOT_SERVICES`）。

## 头文件（依赖）

| 头文件 | 用途 |
| --- | --- |
| `Uefi.h` | 基础类型：`EFI_STATUS`、`UINTN`、`EFI_MEMORY_DESCRIPTOR`、`EFI_HANDLE` |
| `UefiBootServicesTableLib.h` | 提供全局 `gBS`（引导服务表） |
| `UefiLib.h` / `PrintLib.h` | `Print()`、`AsciiSPrint()` 等格式化输出 |
| `Protocol/LoadedImage.h` | `EFI_LOADED_IMAGE_PROTOCOL`，用于定位加载镜像的设备 |
| `Protocol/SimpleFileSystem.h` | 从设备拿到可读文件系统 |
| `Protocol/DiskIo2.h`、`BlockIo.h` | 已 include 但本文件**未使用**（书中预留/后续用） |
| `Guid/FileInfo.h` | `EFI_FILE_INFO` + `gEfiFileInfoGuid`，取文件大小 |

## `struct MemoryMap`

把 `gBS->GetMemoryMap` 的 5 个输出参数打包成一个结构，避免主流程到处传 6 个指针。字段即 UEFI 内存映射的 5 项输出。

## `GetMemoryMap()` — 取内存映射的封装

- `buffer == NULL` 时返回 `EFI_BUFFER_TOO_SMALL`（对应规范里"先探测、再分配、再取"的两段式用法）。
- 把 `map_size` 先设为缓冲区大小，调用 `gBS->GetMemoryMap`；成功后固件会回填实际的 `map_size`、`descriptor_size`、`descriptor_version`、`map_key`。

## `GetMemoryTypeUnicode()` — 类型枚举 → 可读字符串

把 `EFI_MEMORY_TYPE` 的每种类型（`EfiReservedMemoryType`…`EfiPersistentMemory`，`EfiMaxMemoryType` 是哨兵）映射成名字，供 CSV 落盘；`default` 返回 `InvalidMemoryType`。

## `SaveMemoryMap()` — 写内存映射 CSV（本文件最值得记的函数）

- 先写表头：`Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute`。
- **关键循环**：从 `map->buffer` 起，按 `iter += desc->descriptor_size` 步进。
  - EFI 规范规定内存描述符数组可能带**尾部对齐填充**，因此 `descriptor_size` **可能大于** `sizeof(EFI_MEMORY_DESCRIPTOR)`。必须用 `descriptor_size` 步进，**不能**用 `sizeof` 硬编码——这是规范里的经典坑，也是这段代码严谨的地方。
- 每行格式：索引、Type 数值、Type 名、物理起始地址、页数、属性（只保留低 20 位 `& 0xfffff`，书中简化）。

## `OpenRootDir()` — 解析"加载镜像所在设备"的根目录

- `gBS->OpenProtocol(image_handle, ...)` 取出 `EFI_LOADED_IMAGE_PROTOCOL`（本镜像的加载信息）。
- 再用 `loaded_image->DeviceHandle` 取 `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`。
- `fs->OpenVolume(fs, root)` 得到根目录 `EFI_FILE_PROTOCOL**`。

## `UefiMain()` — 主流程逐段

入口签名 `EFIAPI UefiMain(EFI_HANDLE, EFI_SYSTEM_TABLE*)`，UEFI 应用的标准入口。

**① 打印** `Hello, Mikan World!`

**② 内存映射**：栈上 16KB 缓冲 `memmap_buf[4096*4]`，初始化结构后调 `GetMemoryMap` 一次成功（未处理 buffer 太小分支——16KB 对常见机器足够）。

**③ 打开根目录** → `OpenRootDir` 得到 `root_dir`。

**④ 写 memmap**：`root_dir->Open(... L"\\memmap", READ|WRITE|CREATE)` 创建文件 → `SaveMemoryMap` 写 CSV → `Close`。

**⑤ `read_kernel` 段（载入内核）**：

- `Open` 打开 `\kernel.elf`（只读）。
- `GetInfo` 取文件大小 `kernel_file_size`（先探测缓冲大小、再取，标准的 EFI_FILE_INFO 用法）。
- `AllocatePages(AllocateAddress, EfiLoaderData, 页数, &kernel_base_addr)`：页数 `(size+0xfff)/0x1000` 向上取整，**固定物理地址 `0x100000`（1MB，x86 传统内核加载点）**。
- `kernel_file->Read` 读内核到该地址。注意：`Read` 的第二参数 `kernel_file_size` 是 **in/out**——传入剩余字节数、返回实际读到的字节数（读到 EOF 时会被改写），所以后面 `Print` 打印的是实际读入大小。
- `Print` 输出内核地址与字节数。

**⑥ `exit_bs` 段（关键握手）**：

- `gBS->ExitBootServices(image_handle, map_key)`。
- 若失败（`EFI_ERROR`）：**重新 `GetMemoryMap` 再 `ExitBootServices` 一次**。原因是自上次取映射后，固件驱动可能分配/释放内存导致 `map_key` 失效——这是 UEFI 开发的经典坑，所以重取映射、重试一次。两次都失败则 `while(1)` 死循环。

**⑦ `call_kernel` 段（交接控制权）**：

- `entry_addr = *(UINT64*)(kernel_base_addr + 24)`：**硬编码读 ELF64 头偏移 24 的 `e_entry` 字段**（ELF64 头里 `e_entry` 正好位于偏移 24、8 字节），前提是内核是标准 ELF64。
- `typedef void EntryPointType(void);` 转型为函数指针并调用。
- 跳入内核后不再返回，因此函数末尾的 `Print("All done")` 和 `while(1)` **实际不可达**（只是语法上兜底）。

## 可讨论的细节/潜在瑕疵

- **`e_entry` 偏移 24 是硬编码假设**：一旦内核 ELF 结构或加载方式变化（如分节加载、重定位）就要改写。
- **`Read` 会改写 `kernel_file_size`**：语义是 in/out，别当纯输入用。
- **`ExitBootServices` 之后 `gBS` 全部失效**：此后不能用任何引导服务；"All done" 用的是固件打印，但不可达，所以无碍。
- **`memmap_buf` 固定 16KB**：若内存映射异常大可能返回 `EFI_BUFFER_TOO_SMALL`，代码没处理这个分支。
- **`DiskIo2.h`/`BlockIo.h` 包含未用**：属于后续章节会用到的预留。

## 与 day03 的关系

day03a 里内核只有 `hlt` 空转，整段 Loader 的逻辑就是后面所有章节的"开机引导骨架"；到 day03b/c/d，内核会被替换成真正往帧缓冲（framebuffer）写像素、显示字符的代码，而这份 Loader 的"取内存映射→载内核→退服务→跳转"流程会一直沿用。

loader 的函数调用与资源结构如下：

![alt text](image-1.png)
