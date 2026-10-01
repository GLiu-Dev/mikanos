# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application


## 代码结构速览

这段代码是 MikanOS 第 2 天的引导程序（UEFI Application），核心任务就一件事：**把 UEFI 提供的"内存布局表"取出来，整理成文本写进引导分区根目录的 `\memmap` 文件**，为后续内核管理内存做准备。

| 模块 | 作用 |
| --- | --- |
| `struct MemoryMap` | 内存映射的"容器"，把缓冲区和元数据打包传递 |
| `GetMemoryMap()` | 封装 `gBS->GetMemoryMap` 的"两步式"请求 |
| `GetMemoryTypeUnicode()` | 把内存类型枚举数字翻译成可读字符串 |
| `SaveMemoryMap()` | 逐条描述符格式化，写入文件 |
| `OpenRootDir()` | 通过 Protocol 链拿到引导分区根目录 |
| `UefiMain()` | 真正的入口，串起整个流程 |

## 逐模块解读

**1. 头文件** —— 关键是 `UefiBootServicesTableLib.h` 提供全局 `gBS`（Boot Services 表），`UefiLib.h` 提供 `Print`（UEFI 下的 printf）和 `UefiMain` 的接入。

**2. `struct MemoryMap`** —— 为什么不能只传一个数组？因为 EFI 内存映射是"先问容量、再填数据"的两步式协议，需要把 `buffer_size`（容量）、`buffer`（缓冲区）、`map_size`（实际大小）、`descriptor_size`（单个描述符大小）等打包一起传递。**`descriptor_size` 是后面迭代遍历的关键**——它由 UEFI 返回，不能想当然用 `sizeof(EFI_MEMORY_DESCRIPTOR)`，因为不同平台对齐/版本不同。

**3. `GetMemoryMap()`** —— 典型的两次握手：先把 `map_size` 设为"我准备了多大空间"传进去，UEFI 回填时把 `map_size` 改写为实际字节数。若缓冲区太小返回 `EFI_BUFFER_TOO_SMALL`，调用方可据此重新分配。`map_key` 用于检测拿到映射后内存布局是否又变了。

**4. `GetMemoryTypeUnicode()`** —— 纯翻译函数。UEFI 内存类型是 32 位枚举，`EfiConventionalMemory`（可用内存）是最关键的一个，后续内核只敢动这块。

**5. `SaveMemoryMap()`** —— 遍历的核心在于这个循环：

```
for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer; ...
     iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
     iter += map->descriptor_size)   // ← 步长是 UEFI 返回的真实大小
```

描述符在缓冲区里"紧挨着"排成一串，用字节地址做游标、按 `descriptor_size` 步进，就能依次取出每条 `EFI_MEMORY_DESCRIPTOR`。每个字段用 `AsciiSPrint` 格式化成一行 CSV 写入文件。注意 `desc->Attribute & 0xffffflu`：`0xffffflu` 是十六进制数 `0xFFFFF`（u/l 是 unsigned long 后缀），相当于低 20 位全 1 的掩码，用来截掉 64 位属性位图的高位、只保留常用低 20 位。

**6. `OpenRootDir()`** —— UEFI Protocol 机制的典型使用链，三步走：

```
LoadedImage（找到本镜像所在设备）
  → DeviceHandle
  → SimpleFileSystem（在该设备上打开文件系统接口）
  → OpenVolume（打开卷，得到根目录）
```

**7. `UefiMain()`** —— 入口流程：分配 16KB 栈缓冲区 → 初始化结构体 → `GetMemoryMap` 取映射 → `OpenRootDir` 拿根目录 → `Open` 打开/创建 `\memmap` → `SaveMemoryMap` 写入 → `Close` 落盘。最后 `while(1);` 是**故意挂住不让程序退出**——UEFI 应用一返回控制权就交回固件，后续章节会在这里接管、关闭 Boot Services 并跳进自己的内核。

下面这张图把整个执行流程和关键机制串起来：
![alt text](image.png)

## 几个最值得记住的坑

1. **`descriptor_size` 是迭代步长**：遍历描述符数组时，步进必须是 UEFI 返回的这个值，而不是 `sizeof(EFI_MEMORY_DESCRIPTOR)`。
2. **`while(1);` 不是 bug，是设计**：防止 UEFI 应用返回后控制权交还固件，后续在这里接管启动流程。
3. **路径要用宽字符串**：`L"\\memmap"`（`CHAR16`），文件系统接口只认宽字符。
4. **`0xffffflu` 是掩码**：十六进制数 `0xFFFFF`，截取 64 位 Attribute 的低 20 位。

