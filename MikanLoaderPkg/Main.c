/**
 * @file Main.c
 *
 * MikanOS UEFI 引导加载程序主文件
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件是 MikanOS 引导加载程序（MikanLoader）的核心，实现从 UEFI 环境
 *   启动并跳转到内核的完整流程。主函数 UefiMain 由 UEFI 固件调用。
 *
 * ── 整体执行流程（UefiMain）─────────────────────────────────────────────────
 *
 *   ① 打印启动信息
 *   ② 获取 UEFI 内存映射（GetMemoryMap）并保存到文件（\memmap）
 *   ③ 打开 GOP（图形输出协议），获取帧缓冲区信息，将屏幕填白
 *   ④ 从文件系统读取内核 ELF（\kernel.elf）
 *      - AllocatePool：分配临时缓冲区，读取 ELF 全文
 *      - CalcLoadAddressRange：解析 PT_LOAD 段，确定内核虚拟地址范围
 *      - AllocatePages：在内核指定的虚拟地址处分配物理内存
 *      - CopyLoadSegments：将各 PT_LOAD 段从临时缓冲区复制到最终地址（含 .bss 清零）
 *      - FreePool：释放临时缓冲区
 *   ⑤ ExitBootServices：退出 UEFI Boot Services（禁止再调用大多数 BS 函数）
 *      - 含 map_key 握手：失败时重新获取内存映射并重试
 *   ⑥ 构造 FrameBufferConfig，从 ELF e_entry 读取内核入口地址
 *   ⑦ 跳转到内核 KernelMain（以 &config 为参数）
 *
 * ── UEFI 编程约定 ────────────────────────────────────────────────────────────
 *
 *   gBS（gEfi Boot Services Table）：通过 UefiBootServicesTableLib.h 全局访问。
 *   EFI_STATUS：UEFI 返回值类型，EFI_SUCCESS = 0，错误时最高位置 1。
 *   EFI_ERROR(status)：检查 status 是否为错误（等价于 status & (1UL << 63)）。
 *   Print(L"...")：UEFI 宽字符串打印函数（16 位 CHAR16，L 前缀）。
 *   Halt()：无限 HLT 循环，用于不可恢复错误时停机。
 */
 
#include  <Uefi.h>                          // UEFI 基础类型（EFI_STATUS, EFI_HANDLE 等）
#include  <Library/UefiLib.h>               // Print() 等通用 UEFI 库函数
#include  <Library/UefiBootServicesTableLib.h>  // gBS（Boot Services 全局指针）
#include  <Library/PrintLib.h>              // AsciiSPrint（格式化到 ASCII 缓冲区）
#include  <Library/MemoryAllocationLib.h>   // AllocatePool / FreePool 封装
#include  <Library/BaseMemoryLib.h>         // CopyMem / SetMem（内存操作）
#include  <Protocol/LoadedImage.h>          // EFI_LOADED_IMAGE_PROTOCOL
#include  <Protocol/SimpleFileSystem.h>     // EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, EFI_FILE_PROTOCOL
#include  <Protocol/DiskIo2.h>              // （保留，未直接使用）
#include  <Protocol/BlockIo.h>              // （保留，未直接使用）
#include  <Guid/FileInfo.h>                 // EFI_FILE_INFO, gEfiFileInfoGuid
#include  "frame_buffer_config.hpp"         // FrameBufferConfig, PixelFormat 枚举（共享于内核）
#include  "elf.hpp"                         // Elf64_Ehdr, Elf64_Phdr, PT_LOAD 等 ELF64 类型
 
/* ============================================================================
 * 一、内存映射结构体
 * ============================================================================ */
 
/**
 * MemoryMap — 封装 UEFI GetMemoryMap 返回的内存映射信息
 *
 * 字段说明：
 *   buffer_size    ：buffer 的字节容量（调用前设置，调用后保持原值）
 *   buffer         ：存放内存描述符数组的缓冲区指针（由调用方分配）
 *   map_size       ：GetMemoryMap 填写的实际使用字节数（≤ buffer_size）
 *   map_key        ：当前内存映射的唯一标识键值（ExitBootServices 需要此值）
 *   descriptor_size：每个 EFI_MEMORY_DESCRIPTOR 的实际字节数
 *                    （UEFI 规范允许描述符比 sizeof(EFI_MEMORY_DESCRIPTOR) 更大）
 *   descriptor_version：描述符结构体版本号（目前为 1）
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
 * 二、辅助函数：内存映射获取与保存
 * ============================================================================ */
 
/**
 * GetMemoryMap — 调用 UEFI Boot Service 获取当前内存映射
 *
 * @param map  MemoryMap 结构体指针（调用前 buffer 和 buffer_size 须已设置）
 * @return     EFI_SUCCESS 或错误码
 *
 * ── 实现细节 ─────────────────────────────────────────────────────────────────
 *
 *   若 map->buffer == NULL：直接返回 EFI_BUFFER_TOO_SMALL（防御性检查）。
 *
 *   gBS->GetMemoryMap(...) 参数：
 *     &map->map_size      ：输入 = 缓冲区大小，输出 = 实际使用字节数
 *     map->buffer         ：存放 EFI_MEMORY_DESCRIPTOR 数组
 *     &map->map_key       ：输出内存映射键值（ExitBootServices 需要）
 *     &map->descriptor_size：输出每个描述符的实际大小
 *     &map->descriptor_version：输出描述符版本号
 *
 *   map_key 是内存映射版本的"时间戳"：
 *     内存映射每次变化（如分配/释放），map_key 值随之更新。
 *     ExitBootServices 需要传入"与当前内存映射匹配的 map_key"，
 *     若不匹配（内存映射已变化）则返回 EFI_INVALID_PARAMETER，需重试。
 */
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
 
  map->map_size = map->buffer_size;
  return gBS->GetMemoryMap(
      &map->map_size,
      (EFI_MEMORY_DESCRIPTOR*)map->buffer,
      &map->map_key,
      &map->descriptor_size,
      &map->descriptor_version);
}
 
/**
 * GetMemoryTypeUnicode — 将 EFI 内存类型枚举转为可读的宽字符串
 *
 * @param type  EFI_MEMORY_TYPE 枚举值
 * @return      对应的 CHAR16* 字符串（字符串字面量，无需释放）
 *
 * 用于 SaveMemoryMap 将内存类型名称写入 CSV 文件，便于调试。
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
 * SaveMemoryMap — 将内存映射以 CSV 格式保存到指定文件
 *
 * @param map   已填充的 MemoryMap 结构体
 * @param file  目标文件协议指针（已打开，可写）
 * @return      EFI_SUCCESS 或写文件错误码
 *
 * ── 实现细节 ─────────────────────────────────────────────────────────────────
 *
 *   先写 CSV 头行（Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute）。
 *
 *   遍历内存描述符数组：
 *     iter 从 map->buffer 开始，每次步进 map->descriptor_size（而非 sizeof）。
 *     使用 descriptor_size 而非 sizeof(EFI_MEMORY_DESCRIPTOR) 的原因：
 *       UEFI 规范允许固件扩展描述符结构体，实际大小可能大于标准定义，
 *       使用固件报告的 descriptor_size 确保指针正确对齐到下一个描述符。
 *
 *   AsciiSPrint：将单个描述符格式化到 buf（类似 snprintf）。
 *     输出字段：索引、类型数值、类型名称、物理起始地址、页数、属性（低 20 位）。
 *     Attribute & 0xffffflu：仅保留低 20 位属性标志（屏蔽保留位）。
 */
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  EFI_STATUS status;
  CHAR8 buf[256];
  UINTN len;
 
  // 写 CSV 头行
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);
  status = file->Write(file, &len, header);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);
 
  // 遍历内存描述符数组，逐条写入 CSV
  EFI_PHYSICAL_ADDRESS iter;
  int i;
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {
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
 
/* ============================================================================
 * 三、辅助函数：文件系统与 GOP
 * ============================================================================ */
 
/**
 * OpenRootDir — 打开引导设备的 EFI 文件系统根目录
 *
 * @param image_handle  引导加载程序的 EFI_HANDLE
 * @param root          输出：根目录 EFI_FILE_PROTOCOL 指针
 * @return              EFI_SUCCESS 或错误码
 *
 * ── 实现步骤 ─────────────────────────────────────────────────────────────────
 *
 *   步骤 1：通过 OpenProtocol 获取 EFI_LOADED_IMAGE_PROTOCOL：
 *     loaded_image->DeviceHandle = 加载本 EFI 应用的设备句柄（U 盘/虚拟磁盘）
 *
 *   步骤 2：通过 loaded_image->DeviceHandle 获取 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL：
 *     fs = 该设备上的文件系统驱动
 *
 *   步骤 3：fs->OpenVolume(fs, root)：
 *     打开文件系统卷，获取根目录 EFI_FILE_PROTOCOL。
 *     后续通过 root->Open(...) 打开文件。
 */
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
 
  // 获取加载镜像协议（含 DeviceHandle：加载本 EFI 应用的设备）
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
 
  // 获取该设备上的简单文件系统协议
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
 * OpenGOP — 获取第一个 GOP（图形输出协议）实例
 *
 * @param image_handle  引导加载程序句柄
 * @param gop           输出：EFI_GRAPHICS_OUTPUT_PROTOCOL 指针
 * @return              EFI_SUCCESS 或错误码
 *
 * ── 实现步骤 ─────────────────────────────────────────────────────────────────
 *
 *   步骤 1：LocateHandleBuffer 枚举所有支持 GOP 的设备句柄。
 *     ByProtocol：按协议 GUID 搜索
 *     gop_handles：动态分配的句柄数组（需 FreePool 释放）
 *     num_gop_handles：找到的句柄数量
 *
 *   步骤 2：对 gop_handles[0]（第一个 GOP 设备）调用 OpenProtocol 获取 gop 指针。
 *     通常系统只有一个显示控制器（多显示器时需枚举所有 handle）。
 *
 *   步骤 3：FreePool(gop_handles) 释放 LocateHandleBuffer 分配的句柄数组。
 *     必须在使用后释放，防止内存泄漏（Boot Services 内存在 ExitBootServices 后失效，
 *     但释放是良好实践）。
 */
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
  EFI_STATUS status;
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
 
  // 枚举所有支持 GOP 的设备句柄
  status = gBS->LocateHandleBuffer(
      ByProtocol,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      &num_gop_handles,
      &gop_handles);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  // 打开第一个 GOP 实例
  status = gBS->OpenProtocol(
      gop_handles[0],
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  FreePool(gop_handles);  // 释放 LocateHandleBuffer 动态分配的句柄数组
 
  return EFI_SUCCESS;
}
 
/**
 * GetPixelFormatUnicode — 将 EFI_GRAPHICS_PIXEL_FORMAT 枚举转为可读宽字符串
 *
 * @param fmt   EFI_GRAPHICS_PIXEL_FORMAT 枚举值
 * @return      格式名称字符串
 *
 * 用于 UefiMain 中打印 GOP 信息，便于调试。
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
 
/* ============================================================================
 * 四、辅助函数：停机
 * ============================================================================ */
 
/**
 * Halt — 无限 HLT 循环，用于不可恢复错误时停机
 *
 * __asm__("hlt")：内联 x86 HLT 指令，使 CPU 进入低功耗等待状态。
 * while (1)：中断（NMI 等）可能唤醒 CPU，再次 hlt 重新进入等待。
 */
void Halt(void) {
  while (1) __asm__("hlt");
}
 
/* ============================================================================
 * 五、ELF 加载辅助函数
 * ============================================================================ */
 
/**
 * CalcLoadAddressRange — 计算内核 ELF 所有 PT_LOAD 段的虚拟地址范围
 *
 * @param ehdr   ELF64 文件头指针（指向临时缓冲区中的 ELF 数据）
 * @param first  输出：所有 PT_LOAD 段中 p_vaddr 的最小值（内核加载基址）
 * @param last   输出：所有 PT_LOAD 段中 (p_vaddr + p_memsz) 的最大值（内核末地址）
 *
 * ── 为何需要此函数 ───────────────────────────────────────────────────────────
 *
 *   内核 ELF 通常有多个 PT_LOAD 段（如 .text+.rodata 一段，.data+.bss 另一段）。
 *   需要在内核指定的虚拟地址处 AllocatePages，分配覆盖所有 PT_LOAD 段的连续内存。
 *   本函数遍历 PT_LOAD 段，取 p_vaddr 最小值和 p_vaddr+p_memsz 最大值，
 *   得到需要分配的内存范围 [first, last)。
 *
 * ── 实现细节 ─────────────────────────────────────────────────────────────────
 *
 *   Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff)：
 *     ELF 程序头表（Program Header Table）起始地址 = 文件基址 + e_phoff 偏移。
 *     e_phoff：程序头表在 ELF 文件中的偏移（字节）。
 *
 *   *first = MAX_UINT64, *last = 0：
 *     初始值使 MIN/MAX 操作正确运行（任何实际地址都小于 MAX_UINT64）。
 *
 *   for 循环遍历所有程序头（ehdr->e_phnum 个）：
 *     if (phdr[i].p_type != PT_LOAD) continue：
 *       跳过非加载段（PT_NULL, PT_NOTE, PT_GNU_STACK 等）。
 *     *first = MIN(*first, phdr[i].p_vaddr)：
 *       更新最小虚拟地址。
 *     *last = MAX(*last, phdr[i].p_vaddr + phdr[i].p_memsz)：
 *       p_memsz = 段在内存中的大小（可能 > p_filesz，差值部分为 .bss 零初始化）。
 *       更新最大虚拟地址末端。
 */
void CalcLoadAddressRange(Elf64_Ehdr* ehdr, UINT64* first, UINT64* last) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  *first = MAX_UINT64;
  *last = 0;
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    *first = MIN(*first, phdr[i].p_vaddr);
    *last = MAX(*last, phdr[i].p_vaddr + phdr[i].p_memsz);
  }
}
 
/**
 * CopyLoadSegments — 将 ELF 各 PT_LOAD 段复制到最终虚拟地址（含 .bss 清零）
 *
 * @param ehdr  ELF64 文件头指针（临时缓冲区中的完整 ELF 数据）
 *
 * ── 调用前提 ─────────────────────────────────────────────────────────────────
 *
 *   必须在 CalcLoadAddressRange + AllocatePages 之后调用：
 *     AllocatePages 已在内核指定的虚拟地址处分配物理内存，
 *     CopyLoadSegments 将 ELF 数据复制到这些地址。
 *
 * ── 实现细节 ─────────────────────────────────────────────────────────────────
 *
 *   对每个 PT_LOAD 段：
 *
 *   UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset：
 *     该段在临时缓冲区（ehdr）中的起始地址。
 *     p_offset：段在 ELF 文件中的字节偏移。
 *
 *   CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz)：
 *     将 p_filesz 字节从 ELF 文件中复制到内核目标虚拟地址 p_vaddr。
 *     p_filesz = 段在文件中的字节数（不含 .bss 部分）。
 *
 *   UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz：
 *     p_memsz - p_filesz = .bss 段大小（在内存中需要存在但在文件中不占空间）。
 *     对于无 .bss 的段，remain_bytes = 0，SetMem 调用无效果。
 *
 *   SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0)：
 *     将 .bss 区域（p_vaddr + p_filesz 之后 remain_bytes 字节）清零。
 *     C 标准要求 .bss（未初始化全局变量）在程序启动时为零，此步骤满足该要求。
 */
void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
 
    // 复制段数据（p_filesz 字节）：ELF 文件偏移 → 内核虚拟地址
    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz);
 
    // 清零 .bss 区域（p_memsz - p_filesz 字节）
    UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;
    SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0);
  }
}
 
/* ============================================================================
 * 六、UefiMain — 引导加载程序入口函数
 * ============================================================================ */
 
/**
 * UefiMain — UEFI 引导加载程序主函数
 *
 * @param image_handle  本 EFI 应用的句柄（由固件传入）
 * @param system_table  UEFI 系统表指针（含 Boot Services / Runtime Services 等）
 * @return              EFI_SUCCESS（实际上此函数不会正常返回，末尾进入无限循环）
 *
 * EFIAPI：UEFI 调用约定（Microsoft ABI：RCX, RDX, R8, R9 传参），
 *   确保与固件调用约定兼容。
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
  EFI_STATUS status;
 
  Print(L"Hello, Mikan World!\n");
 
  /* ── 步骤 1：获取内存映射 ──────────────────────────────────────────────── */
 
  /**
   * memmap_buf[4096 * 4]：16 KiB 栈上缓冲区，用于存放内存描述符数组。
   *   典型系统有约 30~50 个内存区域，每个描述符约 48 字节，16 KiB 足够。
   *
   * MemoryMap 初始化：{sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0}
   *   buffer_size = sizeof(memmap_buf)（告知 GetMemoryMap 缓冲区大小）
   *   buffer = memmap_buf（存放内存描述符的缓冲区）
   *   其他字段 = 0（由 GetMemoryMap 填写）
   */
  CHAR8 memmap_buf[4096 * 4];
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  status = GetMemoryMap(&memmap);
  if (EFI_ERROR(status)) {
    Print(L"failed to get memory map: %r\n", status);
    Halt();
  }
 
  /* ── 步骤 2：保存内存映射到文件 ────────────────────────────────────────── */
 
  /**
   * OpenRootDir：打开引导设备文件系统根目录。
   * root_dir->Open(..., L"\\memmap", CREATE)：创建/打开 \memmap 文件（CSV 格式）。
   *   EFI_FILE_MODE_READ | WRITE | CREATE：读写模式，不存在则创建。
   *   若打开失败：打印 "Ignored."（memmap 文件不是必需的），继续执行。
   *   若打开成功：SaveMemoryMap 写入所有内存区域信息，然后 Close 关闭文件。
   */
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    Print(L"failed to open root directory: %r\n", status);
    Halt();
  }
 
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
 
  /* ── 步骤 3：打开 GOP 并将屏幕填白 ─────────────────────────────────────── */
 
  /**
   * OpenGOP：获取 EFI_GRAPHICS_OUTPUT_PROTOCOL 实例。
   * 打印分辨率、像素格式、帧缓冲区地址和大小（调试信息）。
   *
   * 将帧缓冲区所有字节设为 255（填白）：
   *   frame_buffer[i] = 255 将所有像素的 R/G/B/Reserved 字节全部置 255。
   *   结果为白色（R=255, G=255, B=255），与内核的白色背景一致。
   */
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
 
  // 将帧缓冲区所有字节置 255（白色背景，内核接管前预填充）
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── 步骤 4：加载内核 ELF 文件（两阶段加载）────────────────────────────── */
 
  /**
   * ── 阶段 1：打开 \kernel.elf 并读入临时缓冲区 ──────────────────────────
   *
   * root_dir->Open(..., L"\\kernel.elf", READ)：以只读模式打开内核文件。
   *
   * kernel_file->GetInfo(..., &gEfiFileInfoGuid, ...)：
   *   获取 EFI_FILE_INFO，其中 FileSize = 内核 ELF 文件大小（字节）。
   *   file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12：
   *     EFI_FILE_INFO 末尾有变长 FileName 字段（CHAR16 数组）。
   *     +12 CHAR16 = 24 字节，足够存放 "kernel.elf\0"（11 字符）的宽字符串。
   *
   * AllocatePool(EfiLoaderData, kernel_file_size, &kernel_buffer)：
   *   从 UEFI Boot Services 堆分配 kernel_file_size 字节的临时缓冲区。
   *   EfiLoaderData：分配类型（不被 ExitBootServices 自动释放，需手动 FreePool）。
   *
   * kernel_file->Read(kernel_file, &kernel_file_size, kernel_buffer)：
   *   将整个 ELF 文件读入 kernel_buffer（kernel_file_size 字节）。
   */
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\kernel.elf': %r\n", status);
    Halt();
  }
 
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
 
  /**
   * ── 阶段 2：解析 ELF，在内核虚拟地址处分配内存，复制段数据 ─────────────
   *
   * kernel_ehdr：将临时缓冲区解释为 ELF64 文件头（Elf64_Ehdr）。
   *
   * CalcLoadAddressRange：遍历 PT_LOAD 段，找出 [kernel_first_addr, kernel_last_addr)。
   *
   * num_pages = ceil((kernel_last_addr - kernel_first_addr) / 4096)：
   *   (size + 0xfff) / 0x1000：向上取整到 4 KiB 页边界（0x1000 = 4096 = 页大小）。
   *
   * AllocatePages(AllocateAddress, EfiLoaderData, num_pages, &kernel_first_addr)：
   *   AllocateAddress：在指定物理地址（kernel_first_addr）处分配内存，
   *   不允许固件选择其他地址（内核链接时已固定 --image-base 0x100000）。
   *   若该地址已被占用（如固件保留区域）则失败。
   *
   * CopyLoadSegments：将各 PT_LOAD 段数据从临时缓冲区复制到最终虚拟地址，
   *   并将 .bss 区域（p_memsz > p_filesz 的部分）清零。
   *
   * FreePool(kernel_buffer)：释放临时缓冲区（ELF 数据已复制，不再需要）。
   */
  Elf64_Ehdr* kernel_ehdr = (Elf64_Ehdr*)kernel_buffer;
  UINT64 kernel_first_addr, kernel_last_addr;
  CalcLoadAddressRange(kernel_ehdr, &kernel_first_addr, &kernel_last_addr);
 
  UINTN num_pages = (kernel_last_addr - kernel_first_addr + 0xfff) / 0x1000;
  status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                              num_pages, &kernel_first_addr);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pages: %r\n", status);
    Halt();
  }
 
  CopyLoadSegments(kernel_ehdr);
  Print(L"Kernel: 0x%0lx - 0x%0lx\n", kernel_first_addr, kernel_last_addr);
 
  status = gBS->FreePool(kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to free pool: %r\n", status);
    Halt();
  }
 
  /* ── 步骤 5：ExitBootServices（含 map_key 握手重试）───────────────────── */
 
  /**
   * ExitBootServices 的 map_key 握手机制：
   *
   *   UEFI 规定：ExitBootServices 的第二个参数 map_key 必须等于
   *   "最近一次 GetMemoryMap 返回的 map_key"。
   *   任何在 GetMemoryMap 和 ExitBootServices 之间改变内存映射的操作
   *   （如 AllocatePool/Pages, FreePool）都会使 map_key 失效。
   *
   *   因此 FreePool 之后，需要重新调用 GetMemoryMap 获取最新 map_key。
   *   但"重新 GetMemoryMap"之后也不能再有改变内存映射的操作！
   *
   *   标准重试模式（本代码采用）：
   *     status = ExitBootServices(image_handle, memmap.map_key)
   *     if (EFI_ERROR(status)) {
   *       // map_key 过期，重新获取
   *       GetMemoryMap(&memmap)
   *       // 此后禁止任何改变内存映射的操作（包括 Print！）
   *       ExitBootServices(image_handle, memmap.map_key)
   *     }
   *
   *   注意：重试路径中 GetMemoryMap 失败时调用了 Print，
   *   这实际上违反了"不能在 GetMemoryMap 和 ExitBootServices 之间调用可能改变内存的函数"，
   *   但这是不可恢复的错误情况，实践上可接受。
   *
   * ExitBootServices 成功后：
   *   ① gBS 中大多数 Boot Services（AllocatePool, OpenProtocol 等）不再可用
   *   ② Print() 不再可用（依赖 Boot Services）
   *   ③ 固件将控制台输出控制权交给操作系统
   *   ④ 仍可使用 Runtime Services 和直接硬件访问（如帧缓冲区写入）
   */
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
    // 首次 ExitBootServices 失败（map_key 过期），重新获取内存映射
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      Print(L"failed to get memory map: %r\n", status);
      Halt();
    }
    // 使用新 map_key 重试（此行之前不可有任何改变内存映射的操作）
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      Halt();
    }
  }
 
  /* ── 步骤 6：读取内核入口地址并构造 FrameBufferConfig ──────────────────── */
 
  /**
   * entry_addr = *(UINT64*)(kernel_first_addr + 24)：
   *   ELF64 文件头格式（Elf64_Ehdr）：
   *     偏移 0  ：e_ident[16]（魔数 + 字长/字节序/版本等）
   *     偏移 16 ：e_type（2 字节）
   *     偏移 18 ：e_machine（2 字节）
   *     偏移 20 ：e_version（4 字节）
   *     偏移 24 ：e_entry（8 字节）← 内核入口点虚拟地址（KernelMain）
   *   偏移 +24 直接读取 e_entry 字段（64 位虚拟地址 = KernelMain 函数地址）。
   *   此时 kernel_first_addr 是已加载到最终地址的内核 ELF 的基址。
   *
   * struct FrameBufferConfig config：
   *   将 GOP 信息打包为共享结构体（与内核 frame_buffer_config.hpp 中的定义相同）。
   *   (UINT8*)gop->Mode->FrameBufferBase：帧缓冲区字节指针
   *   gop->Mode->Info->PixelsPerScanLine：每行像素数（含行末填充步长）
   *   gop->Mode->Info->HorizontalResolution：可见水平像素数
   *   gop->Mode->Info->VerticalResolution  ：可见垂直像素数
   *   pixel_format（初始 0）：由 switch 根据 GOP 像素格式转换为内核使用的枚举值
   *
   * switch(PixelFormat)：
   *   PixelRedGreenBlueReserved8BitPerColor → kPixelRGBResv8BitPerColor
   *   PixelBlueGreenRedReserved8BitPerColor → kPixelBGRResv8BitPerColor
   *   其他格式（PixelBitMask, PixelBltOnly）：本内核不支持，Halt()。
   */
  UINT64 entry_addr = *(UINT64*)(kernel_first_addr + 24);
 
  struct FrameBufferConfig config = {
    (UINT8*)gop->Mode->FrameBufferBase,
    gop->Mode->Info->PixelsPerScanLine,
    gop->Mode->Info->HorizontalResolution,
    gop->Mode->Info->VerticalResolution,
    0
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
 
  /* ── 步骤 7：跳转到内核 KernelMain ─────────────────────────────────────── */
 
  /**
   * typedef void EntryPointType(const struct FrameBufferConfig*)：
   *   定义函数类型：返回 void，接受 const FrameBufferConfig* 参数。
   *   （内核 KernelMain 声明为 const FrameBufferConfig& 引用，
   *    在 ABI 层等价于指针，entry_point(&config) 正确传入）。
   *
   * EntryPointType* entry_point = (EntryPointType*)entry_addr：
   *   将内核入口地址强转为函数指针。
   *
   * entry_point(&config)：
   *   调用内核 KernelMain(&config)，控制权永久转交给内核。
   *   KernelMain 末尾为无限 HLT 循环，永不返回。
   *
   * Print(L"All done\n") 和 while(1)：
   *   理论上永不执行（内核不会返回），保留作防御性代码。
   */
  typedef void EntryPointType(const struct FrameBufferConfig*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(&config);
 
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 