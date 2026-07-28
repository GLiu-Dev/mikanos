 
/**
 * @file Main.c
 *
 * MikanOS UEFI ブートローダーのメインプログラム.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件是 MikanOS 的 UEFI 引导加载程序入口，完成从固件交出控制权到
 *   跳入内核这一完整的启动序列，主要工作包括：
 *
 *     ① 获取 UEFI 内存映射                    → GetMemoryMap / SaveMemoryMap
 *     ② 打开文件系统根目录                    → OpenRootDir
 *     ③ 将内存映射保存到文件（调试用）        → SaveMemoryMap
 *     ④ 获取 GOP 帧缓冲区信息                 → OpenGOP
 *     ⑤ 两阶段加载内核 ELF 到正确物理地址    → CalcLoadAddressRange + CopyLoadSegments
 *     ⑥ 退出 UEFI Boot Services               → ExitBootServices（含重试机制）
 *     ⑦ 构造 FrameBufferConfig 并跳入内核     → entry_point(&config)
 *
 * ── UEFI 执行环境说明 ────────────────────────────────────────────────────────
 *
 *   UEFI 应用程序运行在固件提供的环境中：
 *     - CPU 处于 64 位保护模式（long mode）
 *     - 调用约定：Microsoft x64 ABI（前 4 参数用 RCX/RDX/R8/R9）
 *     - gST（gEfiSystemTable）：系统表，提供 ConOut（控制台）等
 *     - gBS（gEfiBootServicesTable）：Boot Services 表，提供内存/协议等服务
 *   ExitBootServices 调用后，gBS 失效，固件内存管理终止，内核接管一切。
 */
 
/* ── UEFI 标准头文件 ──────────────────────────────────────────────────────── */
#include  <Uefi.h>                          /* EFI_STATUS, EFI_HANDLE, VOID* 等基础类型 */
#include  <Library/UefiLib.h>              /* Print() 等便利函数 */
#include  <Library/UefiBootServicesTableLib.h> /* gBS（Boot Services 全局指针） */
#include  <Library/PrintLib.h>             /* AsciiSPrint()：格式化到 ASCII 缓冲区 */
#include  <Library/MemoryAllocationLib.h>  /* FreePool() 等内存操作封装 */
#include  <Library/BaseMemoryLib.h>        /* CopyMem(), SetMem() */
#include  <Protocol/LoadedImage.h>         /* EFI_LOADED_IMAGE_PROTOCOL：获取加载镜像信息 */
#include  <Protocol/SimpleFileSystem.h>    /* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL：文件系统访问 */
#include  <Protocol/DiskIo2.h>             /* EFI_DISK_IO2_PROTOCOL（间接依赖，未直接使用） */
#include  <Protocol/BlockIo.h>             /* EFI_BLOCK_IO_PROTOCOL（间接依赖，未直接使用） */
#include  <Guid/FileInfo.h>                /* EFI_FILE_INFO, gEfiFileInfoGuid：获取文件元数据 */
 
/* ── 与内核共享的结构体定义 ─────────────────────────────────────────────────── */
#include  "frame_buffer_config.hpp"  /* FrameBufferConfig 结构体、PixelFormat 枚举（C/C++ 共用） */
#include  "elf.hpp"                  /* Elf64_Ehdr, Elf64_Phdr, PT_LOAD 等 ELF64 数据结构 */
 
/* ============================================================================
 * 一、数据结构定义
 * ============================================================================ */
 
/**
 * MemoryMap — 封装 UEFI GetMemoryMap 调用所需的全部输入/输出字段
 *
 * UEFI 的内存映射描述了物理内存中每个区域的类型（EfiLoaderData、EfiConventionalMemory 等）、
 * 起始物理地址、页数和属性。引导加载程序需要它来：
 *   ① 找到可用内存区域（AllocatePages 自动处理，此处仅用于保存调试文件）
 *   ② 在 ExitBootServices 时提供正确的 map_key（握手凭证）
 *
 * 字段说明：
 *   buffer_size      ：调用方提供的缓冲区字节数（输入）
 *   buffer           ：存储内存描述符数组的缓冲区指针（输入）
 *   map_size         ：实际写入的字节数（输出，可能 < buffer_size）
 *   map_key          ：内存映射版本号（输出）；ExitBootServices 以此为凭证，
 *                      若版本与固件不一致则返回 EFI_INVALID_PARAMETER，需重试
 *   descriptor_size  ：单个 EFI_MEMORY_DESCRIPTOR 的实际字节数（输出）
 *                      不同固件实现可能在标准结构末尾追加扩展字段，故不可用 sizeof 替代
 *   descriptor_version：描述符格式版本号（输出，当前固件均为 1）
 */
struct MemoryMap {
  UINTN buffer_size;
  VOID* buffer;
  UINTN map_size;
  UINTN map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
};
 
/* ============================================================================
 * 二、辅助函数
 * ============================================================================ */
 
/**
 * GetMemoryMap — 调用 UEFI Boot Services 获取当前内存映射
 *
 * @param map  调用方填好 buffer/buffer_size 的 MemoryMap 结构体指针；
 *             函数成功后 map_size / map_key / descriptor_size / descriptor_version
 *             被固件填充。
 *
 * @return EFI_SUCCESS         成功获取
 *         EFI_BUFFER_TOO_SMALL buffer 为 NULL（提前检测，避免传入固件）
 *         其他 EFI_STATUS      固件 GetMemoryMap 返回的错误码
 *
 * 实现说明：
 *   将 map->buffer_size 赋给 map->map_size，作为"缓冲区容量"传入固件；
 *   固件调用后将 map->map_size 更新为"实际写入字节数"。
 *   gBS->GetMemoryMap 的完整签名：
 *     GetMemoryMap(MemoryMapSize*, MemoryMap*, MapKey*, DescriptorSize*, DescriptorVersion*)
 */
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
  /* buffer 为 NULL 表示调用方未提供有效缓冲区，提前返回错误 */
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
 
  /* 将缓冲区容量赋给 map_size，GetMemoryMap 会将其更新为实际写入字节数 */
  map->map_size = map->buffer_size;
  return gBS->GetMemoryMap(
      &map->map_size,
      (EFI_MEMORY_DESCRIPTOR*)map->buffer,  /* 缓冲区起始地址，强转为描述符指针 */
      &map->map_key,                         /* 输出：内存映射版本号（ExitBootServices 凭证） */
      &map->descriptor_size,                 /* 输出：单个描述符实际字节数 */
      &map->descriptor_version);             /* 输出：描述符版本号 */
}
 
/**
 * GetMemoryTypeUnicode — 将 EFI_MEMORY_TYPE 枚举值转换为可读的 UTF-16 字符串
 *
 * @param type  EFI 内存类型枚举（来自 EFI_MEMORY_DESCRIPTOR.Type）
 * @return      对应类型名的 UTF-16 字符串字面量（L"..." 格式）
 *
 * 用于 SaveMemoryMap 将内存类型名以文本形式写入 CSV 文件，便于调试分析。
 * 常见类型含义：
 *   EfiLoaderData        : 引导加载程序自身使用的数据内存（本程序分配的内存属此类）
 *   EfiConventionalMemory: 可自由使用的普通 RAM（内核堆、栈等应从此分配）
 *   EfiBootServicesCode/Data: Boot Services 代码/数据（ExitBootServices 后可回收）
 *   EfiRuntimeServicesCode/Data: 运行时服务代码/数据（ExitBootServices 后仍需保留）
 *   EfiACPIReclaimMemory : ACPI 表所在内存（OS 读取 ACPI 后可回收）
 *   EfiMemoryMappedIO    : MMIO 区域（不可作为 RAM 使用）
 */
const CHAR16* GetMemoryTypeUnicode(EFI_MEMORY_TYPE type) {
  switch (type) {
    case EfiReservedMemoryType: return L"EfiReservedMemoryType";
    case EfiLoaderCode: return L"EfiLoaderCode";
    case EfiLoaderData: return L"EfiLoaderData";
    case EfiBootServicesCode: return L"EfiBootServicesCode";
    case EfiBootServicesData: return L"EfiBootServicesData";
    case EfiRuntimeServicesCode: return L"EfiRuntimeServicesCode";
    case EfiRuntimeServicesData: return L"EfiRuntimeServicesData";
    case EfiConventionalMemory: return L"EfiConventionalMemory";
    case EfiUnusableMemory: return L"EfiUnusableMemory";
    case EfiACPIReclaimMemory: return L"EfiACPIReclaimMemory";
    case EfiACPIMemoryNVS: return L"EfiACPIMemoryNVS";
    case EfiMemoryMappedIO: return L"EfiMemoryMappedIO";
    case EfiMemoryMappedIOPortSpace: return L"EfiMemoryMappedIOPortSpace";
    case EfiPalCode: return L"EfiPalCode";
    case EfiPersistentMemory: return L"EfiPersistentMemory";
    case EfiMaxMemoryType: return L"EfiMaxMemoryType";
    default: return L"InvalidMemoryType";
  }
}
 
/**
 * SaveMemoryMap — 将内存映射以 CSV 格式写入文件（调试用途）
 *
 * @param map   已通过 GetMemoryMap 填充的内存映射结构体
 * @param file  目标文件协议指针（由 UefiMain 打开 \memmap 文件后传入）
 * @return      EFI_SUCCESS 或写入失败时的错误码
 *
 * CSV 格式（首行为列头）：
 *   Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute
 *
 * 遍历方式：
 *   以字节指针（EFI_PHYSICAL_ADDRESS = UINT64）从 buffer 起始地址开始，
 *   每次步进 descriptor_size（固件实际描述符大小，可能 > sizeof(EFI_MEMORY_DESCRIPTOR)），
 *   将每个描述符强转为 EFI_MEMORY_DESCRIPTOR* 读取字段。
 *
 * 注意：Attribute 字段低 20 位掩码（& 0xffffflu）过滤保留位，只显示标准属性位。
 */
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  EFI_STATUS status;
  CHAR8 buf[256];   /* ASCII 格式化缓冲区（每行内存描述符文本） */
  UINTN len;
 
  /* 写入 CSV 列头 */
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);  /* 不含 '\0' 的字节数 */
  status = file->Write(file, &len, header);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);
 
  /* 以字节指针步进遍历所有内存描述符 */
  EFI_PHYSICAL_ADDRESS iter;
  int i;
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {          /* 步进 descriptor_size 字节 */
 
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;
 
    /* 格式化当前描述符为 CSV 行，写入 buf */
    len = AsciiSPrint(
        buf, sizeof(buf),
        "%u, %x, %-ls, %08lx, %lx, %lx\n",
        i,                                           /* 序号 */
        desc->Type,                                  /* 类型枚举值（十六进制） */
        GetMemoryTypeUnicode(desc->Type),            /* 类型名（宽字符串，%-ls 左对齐） */
        desc->PhysicalStart,                         /* 起始物理地址 */
        desc->NumberOfPages,                         /* 页数（4 KiB/页） */
        desc->Attribute & 0xffffflu);                /* 属性位（低 20 位有效） */
 
    status = file->Write(file, &len, buf);
    if (EFI_ERROR(status)) {
      return status;
    }
  }
 
  return EFI_SUCCESS;
}
 
/**
 * OpenRootDir — 获取引导加载程序所在卷（ESP 分区）的文件系统根目录
 *
 * @param image_handle  当前 UEFI 应用的句柄（由 UefiMain 参数传入）
 * @param root          输出：文件系统根目录协议指针
 * @return              EFI_SUCCESS 或错误码
 *
 * 两步 OpenProtocol 流程：
 *   步骤 1：image_handle → EFI_LOADED_IMAGE_PROTOCOL
 *     获取 LoadedImage，从中取出 DeviceHandle（加载本程序的存储设备句柄）。
 *
 *   步骤 2：DeviceHandle → EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
 *     从存储设备句柄获取文件系统协议（SimpleFileSystem），
 *     再调用 OpenVolume 得到根目录 EFI_FILE_PROTOCOL*。
 *
 * EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL：
 *   以"协议查询"方式打开，不增加引用计数，适合只读查询场景。
 */
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
 
  /* 步骤 1：从 image_handle 获取 LoadedImage 协议，取出 DeviceHandle */
  status = gBS->OpenProtocol(
      image_handle,
      &gEfiLoadedImageProtocolGuid,    /* 要查询的协议 GUID */
      (VOID**)&loaded_image,           /* 输出：协议接口指针 */
      image_handle,                    /* AgentHandle：发起查询的句柄（本程序自身） */
      NULL,                            /* ControllerHandle：非驱动场景填 NULL */
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  /* 步骤 2：从 DeviceHandle（存储设备）获取 SimpleFileSystem 协议 */
  status = gBS->OpenProtocol(
      loaded_image->DeviceHandle,               /* 加载本程序的块设备句柄（如 ESP 分区） */
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID**)&fs,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  /* OpenVolume：打开文件系统根目录，root 指向根目录的 EFI_FILE_PROTOCOL */
  return fs->OpenVolume(fs, root);
}
 
/**
 * OpenGOP — 获取 GOP（Graphics Output Protocol）帧缓冲区接口
 *
 * @param image_handle  当前 UEFI 应用句柄
 * @param gop           输出：GOP 协议指针
 * @return              EFI_SUCCESS 或错误码
 *
 * GOP 提供对显示硬件帧缓冲区的直接读写访问，是 UEFI 下最常用的图形接口：
 *   gop->Mode->FrameBufferBase  : 帧缓冲区物理起始地址（直接内存映射，可直接写像素）
 *   gop->Mode->Info->PixelFormat: 像素字节序（RGB/BGR）
 *   gop->Mode->Info->HorizontalResolution / VerticalResolution: 分辨率
 *   gop->Mode->Info->PixelsPerScanLine: 每扫描行像素数（含行末对齐填充）
 *
 * 使用 LocateHandleBuffer 枚举所有支持 GOP 的句柄，选取第一个（gop_handles[0]）。
 * 多显示器场景下 gop_handles[0] 通常是主显示器，本项目不处理多显示器。
 * FreePool(gop_handles) 释放 LocateHandleBuffer 分配的句柄数组。
 */
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
  EFI_STATUS status;
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
 
  /* 枚举所有支持 EFI_GRAPHICS_OUTPUT_PROTOCOL 的设备句柄 */
  status = gBS->LocateHandleBuffer(
      ByProtocol,                         /* 按协议 GUID 搜索 */
      &gEfiGraphicsOutputProtocolGuid,
      NULL,                               /* SearchKey：ByProtocol 时填 NULL */
      &num_gop_handles,                   /* 输出：找到的句柄数量 */
      &gop_handles);                      /* 输出：句柄数组（AllocatePool 分配，需 FreePool） */
  if (EFI_ERROR(status)) {
    return status;
  }
 
  /* 打开第一个 GOP 句柄的图形输出协议 */
  status = gBS->OpenProtocol(
      gop_handles[0],                     /* 第一个支持 GOP 的设备（通常为主显示器） */
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,                        /* 输出：GOP 协议接口指针 */
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  /* 释放 LocateHandleBuffer 分配的句柄数组（GOP 协议已打开，句柄数组不再需要） */
  FreePool(gop_handles);
 
  return EFI_SUCCESS;
}
 
/**
 * GetPixelFormatUnicode — 将 EFI_GRAPHICS_PIXEL_FORMAT 枚举转换为可读字符串
 *
 * @param fmt  GOP 报告的像素格式枚举值
 * @return     对应格式名的 UTF-16 字符串
 *
 * 用于 UefiMain 打印 GOP 信息行，便于调试确认硬件像素字节序。
 * MikanOS 仅支持 PixelRedGreenBlueReserved8BitPerColor 和
 * PixelBlueGreenRedReserved8BitPerColor 两种格式，其他格式会在 UefiMain 中 Halt。
 */
const CHAR16* GetPixelFormatUnicode(EFI_GRAPHICS_PIXEL_FORMAT fmt) {
  switch (fmt) {
    case PixelRedGreenBlueReserved8BitPerColor:
      return L"PixelRedGreenBlueReserved8BitPerColor";
    case PixelBlueGreenRedReserved8BitPerColor:
      return L"PixelBlueGreenRedReserved8BitPerColor";
    case PixelBitMask:
      return L"PixelBitMask";
    case PixelBltOnly:
      return L"PixelBltOnly";
    case PixelFormatMax:
      return L"PixelFormatMax";
    default:
      return L"InvalidPixelFormat";
  }
}
 
/**
 * Halt — 进入无限 HLT 循环，停止 CPU 执行
 *
 * 在发生不可恢复错误时调用，防止 CPU 继续执行无效指令。
 * HLT 指令使 CPU 进入低功耗等待状态，直到下一个中断唤醒后再次 HLT，循环往复。
 *
 * UEFI 环境中尚未配置内核的中断处理，此处 HLT 实质上永久挂起系统，
 * 需要用户手动重启（在 QEMU 中可关闭虚拟机）。
 */
void Halt(void) {
  while (1) __asm__("hlt");
}
 
/**
 * CalcLoadAddressRange — 扫描 ELF 程序头表，计算所有 PT_LOAD 段的总地址范围
 *
 * @param ehdr   指向临时缓冲区中 ELF 头的指针（由 AllocatePool + Read 读入）
 * @param first  输出：所有 PT_LOAD 段中最小的 p_vaddr（加载起始虚拟地址）
 * @param last   输出：所有 PT_LOAD 段中最大的 p_vaddr + p_memsz（加载结束虚拟地址）
 *
 * ── 为什么需要两阶段加载 ─────────────────────────────────────────────────────
 *
 *   内核 ELF 中的 PT_LOAD 段要求被加载到特定虚拟地址（p_vaddr），
 *   这些地址在链接时由 --image-base 0x100000 确定。
 *   UEFI AllocatePages(AllocateAddress, ...) 可在指定物理地址分配内存，
 *   但调用前必须知道所需的地址范围和页数，因此需要先解析 ELF 头。
 *
 *   两阶段流程：
 *     阶段 1（本函数）：AllocatePool 分配任意位置的临时缓冲区 → 读入整个 ELF →
 *                       解析程序头表，得到 [first, last) 范围
 *     阶段 2：AllocatePages(AllocateAddress) 在 first 处分配精确大小的内存 →
 *             CopyLoadSegments 将各段从缓冲区复制到正确地址 → FreePool 释放临时缓冲区
 *
 * ── ELF 程序头表访问方式 ─────────────────────────────────────────────────────
 *
 *   程序头表起始地址：(UINT64)ehdr + ehdr->e_phoff（e_phoff 是相对于 ELF 头的偏移）
 *   程序头数量：ehdr->e_phnum
 *   phdr[i] 访问：编译器以 sizeof(Elf64_Phdr) 步进（固定大小结构体数组）
 *
 *   只处理 p_type == PT_LOAD 的段（其他如 PT_NOTE、PT_GNU_STACK 等忽略）。
 *   PT_LOAD 段包含：.text（代码）、.rodata（只读数据）、.data（已初始化数据）、
 *                   .bss（未初始化数据，p_filesz < p_memsz，差值需在内存中清零）
 *
 * ── MIN / MAX 初始化技巧 ─────────────────────────────────────────────────────
 *
 *   *first 初始化为 MAX_UINT64（所有可能的 UINT64 值中最大值），
 *   使得第一个 PT_LOAD 段的 p_vaddr 必然小于它，MIN 取到真实值。
 *   *last 初始化为 0，使得第一个段的 p_vaddr + p_memsz 必然大于 0，MAX 取到真实值。
 */
void CalcLoadAddressRange(Elf64_Ehdr* ehdr, UINT64* first, UINT64* last) {
  /* 程序头表起始地址：ELF 头地址 + e_phoff（程序头表在文件中的偏移） */
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  *first = MAX_UINT64;  /* 初始化为最大值，确保第一个 PT_LOAD 段能更新它 */
  *last = 0;
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;  /* 跳过非加载段 */
    *first = MIN(*first, phdr[i].p_vaddr);
    *last  = MAX(*last,  phdr[i].p_vaddr + phdr[i].p_memsz);
  }
}
 
/**
 * CopyLoadSegments — 将 ELF 各 PT_LOAD 段从临时缓冲区复制到最终加载地址
 *
 * @param ehdr  指向临时缓冲区中 ELF 头的指针（AllocatePages 已在 first..last 分配内存）
 *
 * 调用时机：
 *   AllocatePages(AllocateAddress, ..., &kernel_first_addr) 已在正确物理地址分配内存后调用。
 *   此时临时缓冲区（kernel_buffer）和目标物理地址（p_vaddr）均可写。
 *
 * ── 两步处理每个 PT_LOAD 段 ──────────────────────────────────────────────────
 *
 *   步骤 A：CopyMem — 复制文件数据（p_filesz 字节）
 *     源地址：(UINT64)ehdr + phdr[i].p_offset
 *       p_offset：该段在 ELF 文件中的字节偏移（相对于文件/缓冲区起始）
 *       (UINT64)ehdr 是缓冲区在内存中的实际地址，加上 p_offset 得到段数据位置
 *     目标地址：phdr[i].p_vaddr
 *       段应被加载到的虚拟/物理地址（本项目无分页，虚拟地址 = 物理地址）
 *     复制长度：phdr[i].p_filesz（文件中存储的字节数）
 *
 *   步骤 B：SetMem — 清零 .bss 区域（p_memsz - p_filesz 字节）
 *     .bss 段（未初始化全局变量）在 ELF 文件中不占空间（p_filesz = 0 或很小），
 *     但在内存中需要清零的空间为 p_memsz 字节。
 *     差值 remain_bytes = p_memsz - p_filesz 即为需要清零的 .bss 区域大小。
 *     清零起始地址：p_vaddr + p_filesz（紧接复制数据之后）
 *     若 p_memsz == p_filesz（该段无 .bss），remain_bytes = 0，SetMem 为空操作。
 *
 * ── 为什么不直接用 AllocatePages + 文件读取到目标地址 ────────────────────────
 *
 *   UEFI File->Read 需要知道目标地址和大小，但在解析 ELF 头之前不知道目标地址；
 *   而解析 ELF 头需要先把文件读入内存。因此必须先用 AllocatePool 读入整个 ELF，
 *   再解析头部，再 AllocatePages，再逐段复制，形成两阶段流程。
 */
void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
 
    /* 步骤 A：计算该段在临时缓冲区中的源地址，复制 p_filesz 字节到目标地址 */
    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz);
 
    /* 步骤 B：清零 .bss 区域（p_memsz > p_filesz 时存在 .bss，否则为 0 字节操作） */
    UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;
    SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0);
  }
}
 
/* ============================================================================
 * 三、UefiMain — UEFI 引导加载程序主函数
 * ============================================================================ */
 
/**
 * UefiMain — UEFI 固件调用的引导加载程序入口
 *
 * @param image_handle  本 UEFI 应用的句柄（固件分配，用于 OpenProtocol 等调用）
 * @param system_table  UEFI 系统表指针（提供 ConOut、BootServices、RuntimeServices 等）
 * @return              正常情况下永不返回（内核接管后 UEFI 环境已销毁）；
 *                      若不得不返回，返回 EFI_SUCCESS 告知固件正常退出。
 *
 * ── 执行阶段总览 ─────────────────────────────────────────────────────────────
 *
 *   [A] 获取内存映射并保存到文件        （调试信息，内核不依赖此文件）
 *   [B] 获取 GOP 帧缓冲区信息
 *   [C] 将帧缓冲区整体设为白色          （视觉确认 GOP 写入正常）
 *   [D] 两阶段加载 kernel.elf：
 *       D1. AllocatePool → 读入整个 ELF 文件到临时缓冲区
 *       D2. CalcLoadAddressRange → 解析 PT_LOAD 段，确定目标地址范围
 *       D3. AllocatePages(AllocateAddress) → 在正确物理地址分配内存
 *       D4. CopyLoadSegments → 将各段复制到目标地址，清零 .bss
 *       D5. FreePool → 释放临时缓冲区
 *   [E] ExitBootServices（含 map_key 过期重试机制）
 *   [F] 构造 FrameBufferConfig，读取内核入口地址，跳入内核
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
  EFI_STATUS status;
 
  Print(L"Hello, Mikan World!\n");
 
  /* ── [A] 获取内存映射 ───────────────────────────────────────────────────── */
 
  /**
   * 在栈上分配 16 KiB（4096×4 字节）的缓冲区用于存储内存描述符数组。
   * 典型 UEFI 环境有 50~200 个内存区域，每个描述符约 48 字节，
   * 4096×4 = 16384 字节通常足够（若不够 GetMemoryMap 会返回 EFI_BUFFER_TOO_SMALL）。
   * MemoryMap 结构体以 {buffer_size, buffer, 0, 0, 0, 0} 初始化：
   *   buffer_size = sizeof(memmap_buf)（16 KiB）
   *   buffer      = memmap_buf（栈上缓冲区地址）
   *   其余字段初始为 0，由 GetMemoryMap 填充
   */
  CHAR8 memmap_buf[4096 * 4];
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  status = GetMemoryMap(&memmap);
  if (EFI_ERROR(status)) {
    Print(L"failed to get memory map: %r\n", status);
    Halt();
  }
 
  /* 打开引导分区（ESP）的根目录，用于后续读取 kernel.elf 和写入 memmap 文件 */
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    Print(L"failed to open root directory: %r\n", status);
    Halt();
  }
 
  /**
   * 打开（或创建）\memmap 文件，将内存映射写入（调试用途）。
   * EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE：
   *   三个标志组合表示"读写打开，若不存在则创建"。
   * 失败时仅打印警告并继续（非致命错误，内核不依赖此文件）。
   */
  EFI_FILE_PROTOCOL* memmap_file;
  status = root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\memmap': %r\n", status);
    Print(L"Ignored.\n");  /* 非致命：SD 卡写保护等情况下允许继续 */
  } else {
    status = SaveMemoryMap(&memmap, memmap_file);
    if (EFI_ERROR(status)) {
      Print(L"failed to save memory map: %r\n", status);
      Halt();
    }
    status = memmap_file->Close(memmap_file);
    if (EFI_ERROR(status)) {
      Print(L"failed to close memory map: %r\n", status);
      Halt();
    }
  }
 
  /* ── [B] 获取 GOP 帧缓冲区 ─────────────────────────────────────────────── */
 
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  status = OpenGOP(image_handle, &gop);
  if (EFI_ERROR(status)) {
    Print(L"failed to open GOP: %r\n", status);
    Halt();
  }
 
  /* 打印显示器分辨率、像素格式和帧缓冲区地址范围（调试信息） */
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  /* ── [C] 将帧缓冲区整体填为白色（视觉验证 GOP 写入正常）────────────────── */
 
  /**
   * FrameBufferBase 是帧缓冲区的物理地址，直接映射到内存，可用字节指针读写。
   * 将所有字节设为 255（0xFF）后：
   *   RGB 格式：R=255, G=255, B=255（白色）
   *   BGR 格式：B=255, G=255, R=255（白色，字节序不影响全白效果）
   * 遍历 FrameBufferSize 字节（含行末填充区域），确保整个帧缓冲区被覆盖。
   */
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── [D1] 打开并读取 kernel.elf 到临时缓冲区 ───────────────────────────── */
 
  /* 以只读模式打开内核 ELF 文件 */
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\kernel.elf': %r\n", status);
    Halt();
  }
 
  /**
   * 获取 kernel.elf 的文件信息（主要为了取 FileSize）。
   *
   * EFI_FILE_INFO 末尾有变长的文件名字符串（CHAR16[]），
   * sizeof(EFI_FILE_INFO) 仅包含固定字段部分。
   * 文件名最长 12 个 CHAR16（"kernel.elf" 含终止符），故缓冲区大小为：
   *   sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12
   * GetInfo(&gEfiFileInfoGuid, ...) 通过 GUID 指定要查询的信息类型为 EFI_FILE_INFO。
   */
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
  status = kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to get file information: %r\n", status);
    Halt();
  }
 
  /* 从 EFI_FILE_INFO 中取出文件大小（字节数） */
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;
 
  /**
   * AllocatePool：在 UEFI 堆上分配 kernel_file_size 字节的临时缓冲区。
   *   EfiLoaderData：内存类型标记为"引导加载程序数据"，ExitBootServices 后可被 OS 回收。
   *   kernel_buffer：输出指针，指向分配的内存区域（任意物理地址，由固件选择）。
   * 目的：将整个 kernel.elf 文件读入内存，以便解析 ELF 头（阶段 D2）。
   */
  VOID* kernel_buffer;
  status = gBS->AllocatePool(EfiLoaderData, kernel_file_size, &kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pool: %r\n", status);
    Halt();
  }
 
  /* 将 kernel.elf 全部内容读入临时缓冲区（kernel_file_size 既是输入长度也是输出实际读取量） */
  status = kernel_file->Read(kernel_file, &kernel_file_size, kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"error: %r", status);
    Halt();
  }
 
  /* ── [D2] 解析 ELF 程序头表，确定加载地址范围 ──────────────────────────── */
 
  /**
   * 将临时缓冲区首地址解释为 ELF 头指针，传入 CalcLoadAddressRange。
   * 函数扫描所有 PT_LOAD 段，输出：
   *   kernel_first_addr：最小 p_vaddr（内核加载起始地址，由 --image-base 0x100000 决定）
   *   kernel_last_addr ：最大 p_vaddr + p_memsz（内核加载结束地址）
   */
  Elf64_Ehdr* kernel_ehdr = (Elf64_Ehdr*)kernel_buffer;
  UINT64 kernel_first_addr, kernel_last_addr;
  CalcLoadAddressRange(kernel_ehdr, &kernel_first_addr, &kernel_last_addr);
 
  /* ── [D3] 在正确物理地址分配页面 ──────────────────────────────────────── */
 
  /**
   * 计算所需页数：向上取整到 4 KiB（0x1000）边界。
   *   (size + 0xfff) / 0x1000 是标准的向上整除公式：
   *   若 size = 0x5001，则 (0x5001 + 0xfff) / 0x1000 = 0x6000 / 0x1000 = 6 页。
   *
   * AllocatePages(AllocateAddress, ...)：
   *   AllocateAddress 模式要求固件在 kernel_first_addr 指定的精确物理地址分配内存，
   *   若该地址已被占用或不可用则返回 EFI_NOT_FOUND。
   *   分配成功后，[kernel_first_addr, kernel_first_addr + num_pages × 4KiB) 可写。
   *   此地址必须与 kernel.elf 链接时的 --image-base 0x100000 一致，
   *   否则内核中的绝对地址引用（全局变量、函数指针等）将指向错误位置。
   */
  UINTN num_pages = (kernel_last_addr - kernel_first_addr + 0xfff) / 0x1000;
  status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                              num_pages, &kernel_first_addr);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pages: %r\n", status);
    Halt();
  }
 
  /* ── [D4] 将各 PT_LOAD 段从临时缓冲区复制到最终加载地址 ─────────────────── */
 
  /**
   * CopyLoadSegments 遍历程序头表，对每个 PT_LOAD 段：
   *   1. CopyMem：将 p_filesz 字节从临时缓冲区复制到 p_vaddr
   *   2. SetMem：将 p_vaddr + p_filesz 后的 (p_memsz - p_filesz) 字节清零（.bss 段）
   * 执行后内核代码和数据已完整加载到正确的物理/虚拟地址。
   */
  CopyLoadSegments(kernel_ehdr);
  Print(L"Kernel: 0x%0lx - 0x%0lx\n", kernel_first_addr, kernel_last_addr);
 
  /* ── [D5] 释放临时缓冲区 ────────────────────────────────────────────────── */
 
  /**
   * FreePool：释放 AllocatePool 分配的临时缓冲区（kernel_buffer）。
   * 此时内核数据已复制到目标地址，临时缓冲区不再需要，释放以归还 UEFI 堆内存。
   * 必须在 ExitBootServices 之前调用（ExitBootServices 后 gBS 失效，FreePool 不可用）。
   */
  status = gBS->FreePool(kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to free pool: %r\n", status);
    Halt();
  }
 
  /* ── [E] ExitBootServices — 退出 UEFI Boot Services ─────────────────────── */
 
  /**
   * ExitBootServices 通知固件引导加载程序已完成，OS 接管内存和硬件控制权。
   * 调用成功后：
   *   - gBS（Boot Services）全部失效（Print、AllocatePool 等均不可再调用）
   *   - UEFI 定时器中断和事件系统停止
   *   - 固件标记 EfiBootServicesCode/Data 内存为可回收
   *
   * ── map_key 握手机制与重试 ──────────────────────────────────────────────────
   *
   *   ExitBootServices 第二参数为 memmap.map_key（内存映射版本号）。
   *   固件要求传入的 map_key 与当前实际内存映射版本一致，否则返回 EFI_INVALID_PARAMETER。
   *
   *   map_key 可能过期的原因：
   *     在 GetMemoryMap（填充 map_key）到 ExitBootServices（消费 map_key）之间，
   *     任何导致内存映射变化的操作（如 Print 内部分配内存、Open 打开文件等）
   *     都会使固件递增内存映射版本，导致 map_key 过期。
   *
   *   重试流程：
   *     第一次 ExitBootServices 失败 → 重新 GetMemoryMap（更新 map_key）
   *     → 立即第二次 ExitBootServices（两次调用之间不允许任何其他 Boot Services 调用）
   *     若第二次仍失败，则属于不可恢复错误，Halt。
   *
   *   关键约束：
   *     重试路径中的 GetMemoryMap 和第二次 ExitBootServices 之间
   *     绝对不能调用任何其他 Boot Services（包括 Print！），
   *     否则内存映射再次变化，map_key 立即再次过期，形成永久失败。
   */
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
    /* 第一次失败：map_key 可能已过期，重新获取最新内存映射 */
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      Print(L"failed to get memory map: %r\n", status);
      Halt();
    }
    /* 立即以最新 map_key 重试（此后至 ExitBootServices 返回前不可调用其他 Boot Services） */
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      Halt();
    }
  }
 
  /* ── [F] 构造 FrameBufferConfig，跳入内核 ───────────────────────────────── */
 
  /**
   * 读取内核 ELF 入口地址（e_entry 字段）。
   *
   * ELF 头（Elf64_Ehdr）布局中，e_entry 字段位于偏移 0x18（24 字节）处：
   *   偏移 0x00：e_ident[16]（ELF 魔数和标识）
   *   偏移 0x10：e_type(2) + e_machine(2) + e_version(4)
   *   偏移 0x18：e_entry(8) ← 入口点虚拟地址（KernelMain 的地址）
   *
   * kernel_first_addr 是加载到内存后 ELF 头的起始地址（PT_LOAD 段已复制到此）。
   * *(UINT64*)(kernel_first_addr + 24)：
   *   将 kernel_first_addr + 24 解释为 UINT64 指针，解引用得到 e_entry 的值
   *   = KernelMain 函数在内存中的实际地址。
   */
  UINT64 entry_addr = *(UINT64*)(kernel_first_addr + 24);
 
  /**
   * 构造 FrameBufferConfig 结构体，传递给内核。
   *
   * 此结构体在 frame_buffer_config.hpp 中定义，C 和 C++ 共用（extern "C" 兼容）：
   *   frame_buffer         ：帧缓冲区物理地址（GOP FrameBufferBase 强转为 UINT8*）
   *   pixels_per_scan_line ：每扫描行像素数（含行末填充，内核 PixelAt() 用此计算字节地址）
   *   horizontal_resolution：水平分辨率（内核用于遍历像素列）
   *   vertical_resolution  ：垂直分辨率（内核用于遍历像素行）
   *   pixel_format         ：初始为 0，由下方 switch 填充
   *
   * pixel_format 转换：
   *   GOP 使用 EFI_GRAPHICS_PIXEL_FORMAT 枚举（UEFI 规范定义）；
   *   内核使用 PixelFormat 枚举（frame_buffer_config.hpp 定义）。
   *   switch 语句在两者之间做映射，不支持的格式（如 PixelBltOnly）调用 Halt。
   */
  struct FrameBufferConfig config = {
    (UINT8*)gop->Mode->FrameBufferBase,    /* frame_buffer */
    gop->Mode->Info->PixelsPerScanLine,    /* pixels_per_scan_line */
    gop->Mode->Info->HorizontalResolution, /* horizontal_resolution */
    gop->Mode->Info->VerticalResolution,   /* vertical_resolution */
    0                                      /* pixel_format（由 switch 填充） */
  };
  switch (gop->Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
      config.pixel_format = kPixelRGBResv8BitPerColor;
      break;
    case PixelBlueGreenRedReserved8BitPerColor:
      config.pixel_format = kPixelBGRResv8BitPerColor;
      break;
    default:
      Print(L"Unimplemented pixel format: %d\n", gop->Mode->Info->PixelFormat);
      Halt();
  }
 
  /**
   * 跳入内核 KernelMain。
   *
   * typedef void EntryPointType(const struct FrameBufferConfig*);
   *   定义函数类型：接受 FrameBufferConfig 常量指针、返回 void。
   *   内核中 KernelMain 声明为：
   *     extern "C" void KernelMain(const FrameBufferConfig& frame_buffer_config);
   *   C++ 引用在 ABI 层面以指针传递，因此此处传 &config（指针）与内核的引用参数完全匹配。
   *
   * EntryPointType* entry_point = (EntryPointType*)entry_addr;
   *   将入口地址强转为函数指针，准备调用。
   *
   * entry_point(&config);
   *   调用内核 KernelMain，传入 FrameBufferConfig 的地址。
   *   此调用后控制权永久转移到内核，KernelMain 进入 HLT 无限循环，永不返回。
   *   若内核异常返回，后续的 Print 和 while(1) 提供最后的防护。
   */
  typedef void EntryPointType(const struct FrameBufferConfig*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(&config);
 
  /* 以下代码正常情况下永远不会执行（内核永不返回） */
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 
 