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
#include  <Protocol/LoadedImage.h>         /* EFI_LOADED_IMAGE_PROTOCOL */
#include  <Protocol/SimpleFileSystem.h>    /* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
#include  <Protocol/DiskIo2.h>             /* 间接依赖，未直接使用 */
#include  <Protocol/BlockIo.h>             /* 间接依赖，未直接使用 */
#include  <Guid/FileInfo.h>                /* EFI_FILE_INFO, gEfiFileInfoGuid */
 
/* ── 与内核共享的结构体定义 ─────────────────────────────────────────────────── */
#include  "frame_buffer_config.hpp"  /* FrameBufferConfig 结构体、PixelFormat 枚举 */
#include  "elf.hpp"                  /* Elf64_Ehdr, Elf64_Phdr, PT_LOAD 等 ELF64 结构 */
 
/* ============================================================================
 * 一、数据结构定义
 * ============================================================================ */
 
/**
 * MemoryMap — 封装 UEFI GetMemoryMap 调用所需的全部输入/输出字段
 *
 * 字段说明：
 *   buffer_size      ：调用方提供的缓冲区字节数（输入）
 *   buffer           ：存储内存描述符数组的缓冲区指针（输入）
 *   map_size         ：实际写入的字节数（输出，可能 < buffer_size）
 *   map_key          ：内存映射版本号（输出）；ExitBootServices 以此为凭证，
 *                      若固件版本不一致则返回 EFI_INVALID_PARAMETER，需重试
 *   descriptor_size  ：单个 EFI_MEMORY_DESCRIPTOR 的实际字节数（输出）
 *                      不同固件可能在标准结构末尾追加扩展字段，不可用 sizeof 替代
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
 * @param map  调用方填好 buffer/buffer_size 的结构体指针；
 *             成功后 map_size/map_key/descriptor_size/descriptor_version 被固件填充。
 * @return     EFI_SUCCESS 或错误码
 *
 * map->map_size 先作为"缓冲区容量"输入，固件调用后更新为"实际写入字节数"。
 */
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
 
  map->map_size = map->buffer_size;  /* 将缓冲区容量传给固件 */
  return gBS->GetMemoryMap(
      &map->map_size,
      (EFI_MEMORY_DESCRIPTOR*)map->buffer,
      &map->map_key,           /* 输出：内存映射版本号（ExitBootServices 凭证） */
      &map->descriptor_size,   /* 输出：单个描述符实际字节数（步进遍历用） */
      &map->descriptor_version);
}
 
/**
 * GetMemoryTypeUnicode — 将 EFI_MEMORY_TYPE 枚举转换为可读 UTF-16 字符串
 *
 * 用于 SaveMemoryMap 将内存类型名以文本形式写入 CSV 文件（调试用）。
 * 常见类型含义：
 *   EfiLoaderData        : 引导加载程序数据（本程序分配的内存属此类）
 *   EfiConventionalMemory: 可自由使用的普通 RAM
 *   EfiBootServicesCode/Data: ExitBootServices 后可被 OS 回收
 *   EfiRuntimeServicesCode/Data: ExitBootServices 后仍需保留
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
 * @param file  目标文件协议指针（\memmap 文件）
 * @return      EFI_SUCCESS 或写入失败时的错误码
 *
 * CSV 格式（首行为列头）：
 *   Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute
 *
 * 遍历方式：
 *   以字节指针从 buffer 起始，每次步进 descriptor_size 字节（固件实际描述符大小），
 *   强转为 EFI_MEMORY_DESCRIPTOR* 读取字段。
 *   Attribute & 0xffffflu：过滤保留位，只显示低 20 位标准属性。
 */
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  EFI_STATUS status;
  CHAR8 buf[256];
  UINTN len;
 
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);
  status = file->Write(file, &len, header);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);
 
  EFI_PHYSICAL_ADDRESS iter;
  int i;
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {  /* 步进 descriptor_size 字节 */
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;
    len = AsciiSPrint(
        buf, sizeof(buf),
        "%u, %x, %-ls, %08lx, %lx, %lx\n",
        i, desc->Type, GetMemoryTypeUnicode(desc->Type),
        desc->PhysicalStart, desc->NumberOfPages,
        desc->Attribute & 0xffffflu);
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
 * @param image_handle  当前 UEFI 应用的句柄
 * @param root          输出：文件系统根目录协议指针
 *
 * 两步 OpenProtocol 流程：
 *   步骤 1：image_handle → EFI_LOADED_IMAGE_PROTOCOL → DeviceHandle（存储设备）
 *   步骤 2：DeviceHandle → EFI_SIMPLE_FILE_SYSTEM_PROTOCOL → OpenVolume → 根目录
 *
 * EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL：以只读查询方式打开，不增加引用计数。
 */
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
 
  /* 步骤 1：获取 LoadedImage，取出 DeviceHandle（加载本程序的块设备） */
  status = gBS->OpenProtocol(
      image_handle,
      &gEfiLoadedImageProtocolGuid,
      (VOID**)&loaded_image,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  /* 步骤 2：从 DeviceHandle 获取 SimpleFileSystem，再打开根目录 */
  status = gBS->OpenProtocol(
      loaded_image->DeviceHandle,
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID**)&fs,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  return fs->OpenVolume(fs, root);
}
 
/**
 * OpenGOP — 获取 GOP（Graphics Output Protocol）帧缓冲区接口
 *
 * @param image_handle  当前 UEFI 应用句柄
 * @param gop           输出：GOP 协议指针
 *
 * GOP 提供帧缓冲区直接内存访问：
 *   gop->Mode->FrameBufferBase  : 帧缓冲区物理地址
 *   gop->Mode->Info->PixelFormat: RGB/BGR 字节序
 *   gop->Mode->Info->HorizontalResolution / VerticalResolution
 *   gop->Mode->Info->PixelsPerScanLine
 *
 * LocateHandleBuffer 枚举所有 GOP 设备，选取第一个（通常为主显示器）。
 * FreePool(gop_handles) 释放 LocateHandleBuffer 分配的句柄数组。
 */
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
  EFI_STATUS status;
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
 
  status = gBS->LocateHandleBuffer(
      ByProtocol,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      &num_gop_handles,
      &gop_handles);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  status = gBS->OpenProtocol(
      gop_handles[0],  /* 第一个 GOP 设备（通常为主显示器） */
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  FreePool(gop_handles);  /* GOP 协议已打开，句柄数组不再需要 */
  return EFI_SUCCESS;
}
 
/**
 * GetPixelFormatUnicode — 将 EFI_GRAPHICS_PIXEL_FORMAT 转换为可读字符串
 *
 * 用于 UefiMain 打印 GOP 信息行，便于调试确认硬件像素字节序。
 * MikanOS 仅支持 RGB 和 BGR 两种格式，其他格式在 UefiMain 中 Halt。
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
 * 发生不可恢复错误时调用，防止 CPU 继续执行无效指令。
 * HLT 使 CPU 进入低功耗等待状态，直到中断唤醒后再次 HLT，循环往复。
 */
void Halt(void) {
  while (1) __asm__("hlt");
}
 
/**
 * CalcLoadAddressRange — 扫描 ELF 程序头表，计算所有 PT_LOAD 段的总地址范围
 *
 * @param ehdr   指向临时缓冲区中 ELF 头的指针
 * @param first  输出：所有 PT_LOAD 段中最小的 p_vaddr
 * @param last   输出：所有 PT_LOAD 段中最大的 p_vaddr + p_memsz
 *
 * ── 两阶段加载的必要性 ───────────────────────────────────────────────────────
 *
 *   内核 ELF 的 PT_LOAD 段要求加载到特定虚拟地址（p_vaddr，由 --image-base 决定）。
 *   AllocatePages(AllocateAddress) 在指定物理地址分配内存，需要先知道地址范围。
 *   因此必须先把 ELF 读入任意位置（AllocatePool），解析头部得到范围，
 *   再 AllocatePages 在正确地址分配，再复制，再释放临时缓冲区。
 *
 * ── 程序头表访问 ─────────────────────────────────────────────────────────────
 *
 *   phdr = (UINT64)ehdr + ehdr->e_phoff（程序头表相对 ELF 头的偏移）
 *   phdr[i]：以 sizeof(Elf64_Phdr) 步进的数组访问
 *   只处理 p_type == PT_LOAD 的段（其他段忽略）
 *
 * ── MIN/MAX 初始化技巧 ───────────────────────────────────────────────────────
 *
 *   *first 初始化为 MAX_UINT64：确保第一个 PT_LOAD 段的 p_vaddr 必然更新它。
 *   *last  初始化为 0：确保第一个段的 p_vaddr + p_memsz 必然更新它。
 */
void CalcLoadAddressRange(Elf64_Ehdr* ehdr, UINT64* first, UINT64* last) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  *first = MAX_UINT64;
  *last = 0;
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    *first = MIN(*first, phdr[i].p_vaddr);
    *last  = MAX(*last,  phdr[i].p_vaddr + phdr[i].p_memsz);
  }
}
 
/**
 * CopyLoadSegments — 将 ELF 各 PT_LOAD 段从临时缓冲区复制到最终加载地址
 *
 * @param ehdr  指向临时缓冲区中 ELF 头的指针
 *              （调用时 AllocatePages 已在正确物理地址分配好内存）
 *
 * 对每个 PT_LOAD 段执行两步操作：
 *
 *   步骤 A — CopyMem：复制文件数据（p_filesz 字节）
 *     源地址：(UINT64)ehdr + phdr[i].p_offset（段在临时缓冲区中的位置）
 *     目标地址：phdr[i].p_vaddr（段应加载的虚拟/物理地址）
 *     长度：phdr[i].p_filesz（ELF 文件中存储的字节数）
 *
 *   步骤 B — SetMem：清零 .bss 区域（p_memsz - p_filesz 字节）
 *     .bss（未初始化全局变量）在文件中不占空间，但内存中需要清零。
 *     remain_bytes = p_memsz - p_filesz：需要清零的字节数。
 *     若 p_memsz == p_filesz（无 .bss），remain_bytes=0，SetMem 为空操作。
 */
void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
 
    /* 步骤 A：复制 p_filesz 字节的文件数据到目标地址 */
    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz);
 
    /* 步骤 B：清零 .bss 区域（p_memsz > p_filesz 时存在 .bss） */
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
 * @param image_handle  本 UEFI 应用的句柄（固件分配）
 * @param system_table  UEFI 系统表指针
 * @return              正常情况下永不返回；若返回，返回 EFI_SUCCESS。
 *
 * ── 执行阶段总览 ─────────────────────────────────────────────────────────────
 *
 *   [A] 获取内存映射并保存到文件（调试用）
 *   [B] 获取 GOP 帧缓冲区信息，将帧缓冲区整体设为白色
 *   [C] 两阶段加载 kernel.elf：
 *       C1. AllocatePool + Read → ELF 读入临时缓冲区
 *       C2. CalcLoadAddressRange → 解析 PT_LOAD 段，确定地址范围
 *       C3. AllocatePages(AllocateAddress) → 在正确物理地址分配内存
 *       C4. CopyLoadSegments → 复制段 + 清零 .bss
 *       C5. FreePool → 释放临时缓冲区
 *   [D] ExitBootServices（含 map_key 过期重试机制）
 *   [E] 构造 FrameBufferConfig，读取内核入口地址，跳入内核
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
  EFI_STATUS status;
 
  Print(L"Hello, Mikan World!\n");
 
  /* ── [A] 获取内存映射 ──────────────────────────────────────────────────── */
 
  /**
   * 16 KiB 栈上缓冲区存储内存描述符数组（典型环境 50~200 个区域，通常足够）。
   * MemoryMap 以 {buffer_size, buffer, 0, 0, 0, 0} 初始化，其余字段由 GetMemoryMap 填充。
   */
  CHAR8 memmap_buf[4096 * 4];
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  status = GetMemoryMap(&memmap);
  if (EFI_ERROR(status)) {
    Print(L"failed to get memory map: %r\n", status);
    Halt();
  }
 
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    Print(L"failed to open root directory: %r\n", status);
    Halt();
  }
 
  /**
   * 打开（或创建）\memmap 文件，写入内存映射 CSV（调试用，非致命）。
   * EFI_FILE_MODE_READ | WRITE | CREATE：读写打开，不存在则创建。
   * 失败时仅打印警告并继续（如 SD 卡写保护等情况）。
   */
  EFI_FILE_PROTOCOL* memmap_file;
  status = root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\memmap': %r\n", status);
    Print(L"Ignored.\n");
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
 
  /* ── [B] 获取 GOP 帧缓冲区，整体填白 ──────────────────────────────────── */
 
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  status = OpenGOP(image_handle, &gop);
  if (EFI_ERROR(status)) {
    Print(L"failed to open GOP: %r\n", status);
    Halt();
  }
 
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  /**
   * 将帧缓冲区所有字节设为 255（0xFF）→ 全白（RGB/BGR 均显示白色）。
   * 用于视觉验证 GOP 直接内存写入正常。
   * 内核 KernelMain 也会再次填白，此处为引导阶段的初步验证。
   */
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── [C1] 打开并读取 kernel.elf 到临时缓冲区 ──────────────────────────── */
 
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\kernel.elf': %r\n", status);
    Halt();
  }
 
  /**
   * EFI_FILE_INFO 末尾有变长文件名（CHAR16[]），
   * sizeof(EFI_FILE_INFO) + sizeof(CHAR16)*12 为含文件名的缓冲区大小。
   * GetInfo(&gEfiFileInfoGuid) 按 GUID 查询文件元数据，主要取 FileSize。
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
 
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;
 
  /**
   * AllocatePool(EfiLoaderData)：在 UEFI 堆上分配临时缓冲区（任意物理地址）。
   * EfiLoaderData：内存类型标记，ExitBootServices 后可被 OS 回收。
   * 读入整个 ELF 文件，以便解析程序头表（阶段 C2）。
   */
  VOID* kernel_buffer;
  status = gBS->AllocatePool(EfiLoaderData, kernel_file_size, &kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pool: %r\n", status);
    Halt();
  }
  status = kernel_file->Read(kernel_file, &kernel_file_size, kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"error: %r", status);
    Halt();
  }
 
  /* ── [C2] 解析 ELF 程序头表，确定加载地址范围 ─────────────────────────── */
 
  Elf64_Ehdr* kernel_ehdr = (Elf64_Ehdr*)kernel_buffer;
  UINT64 kernel_first_addr, kernel_last_addr;
  CalcLoadAddressRange(kernel_ehdr, &kernel_first_addr, &kernel_last_addr);
 
  /* ── [C3] 在正确物理地址分配页面 ─────────────────────────────────────── */
 
  /**
   * (size + 0xfff) / 0x1000：向上取整到 4 KiB 页边界的标准公式。
   * AllocatePages(AllocateAddress)：在 kernel_first_addr 精确物理地址分配内存，
   *   必须与链接器 --image-base 0x100000 一致，否则内核绝对地址引用出错。
   */
  UINTN num_pages = (kernel_last_addr - kernel_first_addr + 0xfff) / 0x1000;
  status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                              num_pages, &kernel_first_addr);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pages: %r\n", status);
    Halt();
  }
 
  /* ── [C4] 将各 PT_LOAD 段复制到最终地址（含清零 .bss）─────────────────── */
 
  CopyLoadSegments(kernel_ehdr);
  Print(L"Kernel: 0x%0lx - 0x%0lx\n", kernel_first_addr, kernel_last_addr);
 
  /* ── [C5] 释放临时缓冲区 ─────────────────────────────────────────────── */
 
  /**
   * 内核数据已复制到目标地址，临时缓冲区不再需要。
   * 必须在 ExitBootServices 之前调用（之后 gBS 失效）。
   */
  status = gBS->FreePool(kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to free pool: %r\n", status);
    Halt();
  }
 
  /* ── [D] ExitBootServices — 退出 UEFI Boot Services ──────────────────── */
 
  /**
   * ExitBootServices 通知固件引导加载程序已完成，OS 接管内存和硬件控制权。
   * 成功后 gBS 全部失效，UEFI 定时器/事件停止，固件释放 BootServices 内存。
   *
   * ── map_key 握手机制与重试 ──────────────────────────────────────────────
   *
   *   第二参数为 memmap.map_key（内存映射版本号）。
   *   若传入的版本与固件当前版本不一致（map_key 过期），
   *   返回 EFI_INVALID_PARAMETER，需重新获取内存映射后重试。
   *
   *   map_key 过期原因：
   *     GetMemoryMap 到 ExitBootServices 之间的任何操作（如 Print、Open 等）
   *     若导致内存映射变化（内存分配/释放），固件递增版本号。
   *
   *   重试约束（关键）：
   *     重试路径中的 GetMemoryMap 和第二次 ExitBootServices 之间
   *     绝对不能调用任何其他 Boot Services（包括 Print！），
   *     否则内存映射再次变化，map_key 立即再次过期，形成永久失败。
   */
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
    /* 第一次失败：重新获取最新内存映射（更新 map_key） */
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      Print(L"failed to get memory map: %r\n", status);
      Halt();
    }
    /* 立即重试（此后至函数返回前不可调用任何其他 Boot Services） */
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      Halt();
    }
  }
 
  /* ── [E] 构造 FrameBufferConfig，跳入内核 ──────────────────────────────── */
 
  /**
   * 读取内核入口地址（ELF e_entry 字段）。
   *
   * ELF64 头布局（偏移以字节计）：
   *   0x00：e_ident[16]（ELF 魔数和标识）
   *   0x10：e_type(2) + e_machine(2) + e_version(4)
   *   0x18：e_entry(8) ← 入口点虚拟地址（KernelMain 地址）
   *
   * kernel_first_addr：加载后 ELF 头的起始地址（PT_LOAD 段已复制到此）。
   * *(UINT64*)(kernel_first_addr + 24)：将地址 +24 解释为 UINT64 指针，解引用得 e_entry。
   */
  UINT64 entry_addr = *(UINT64*)(kernel_first_addr + 24);
 
  /**
   * 构造 FrameBufferConfig，传递帧缓冲区信息给内核。
   *
   * pixel_format 转换：
   *   GOP 使用 EFI_GRAPHICS_PIXEL_FORMAT 枚举（UEFI 规范）；
   *   内核使用 PixelFormat 枚举（frame_buffer_config.hpp）。
   *   switch 在两者之间做映射，不支持的格式调用 Halt。
   */
  struct FrameBufferConfig config = {
    (UINT8*)gop->Mode->FrameBufferBase,    /* frame_buffer：帧缓冲区物理地址 */
    gop->Mode->Info->PixelsPerScanLine,    /* pixels_per_scan_line：含行末填充的步长 */
    gop->Mode->Info->HorizontalResolution, /* horizontal_resolution：实际显示列数 */
    gop->Mode->Info->VerticalResolution,   /* vertical_resolution：实际显示行数 */
    0                                      /* pixel_format：由 switch 填充 */
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
   *   函数类型：接受 FrameBufferConfig 常量指针、返回 void。
   *   内核中 KernelMain 声明为 C++ 引用参数（const FrameBufferConfig&），
   *   但 C++ 引用在 ABI 层面以指针传递，entry_point(&config) 完全兼容。
   *
   * 调用后控制权永久转移到内核，KernelMain 进入 HLT 无限循环，永不返回。
   */
  typedef void EntryPointType(const struct FrameBufferConfig*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(&config);
 
  /* 以下代码正常情况下永远不会执行 */
  Print(L"All done\n");
  while (1);
  return EFI_SUCCESS;
}