 
#include  <Uefi.h>
#include  <Library/UefiLib.h>
#include  <Library/UefiBootServicesTableLib.h>
#include  <Library/PrintLib.h>
#include  <Library/MemoryAllocationLib.h>
#include  <Library/BaseMemoryLib.h>
#include  <Protocol/LoadedImage.h>
#include  <Protocol/SimpleFileSystem.h>
#include  <Protocol/DiskIo2.h>
#include  <Protocol/BlockIo.h>
#include  <Guid/FileInfo.h>
#include  "frame_buffer_config.hpp"
#include  "elf.hpp"
 
struct MemoryMap {
  UINTN buffer_size;
  VOID* buffer;
  UINTN map_size;
  UINTN map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
};
 
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
 
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
 
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
      gop_handles[0],
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) {
    return status;
  }
 
  FreePool(gop_handles);
 
  return EFI_SUCCESS;
}
 
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
 
void Halt(void) {
  while (1) __asm__("hlt");
}
 
// #@@range_begin(calc_addr_func)
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
// #@@range_end(calc_addr_func)
 
// #@@range_begin(copy_segm_func)
void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
 
    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz);
 
    UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;
    SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0);
  }
}
// #@@range_end(copy_segm_func)
 
/**
 * UefiMain - UEFI 应用程序入口点（MikanOS 引导加载程序主函数）
 *
 * ── 函数签名约定 ────────────────────────────────────────────────────────────
 *
 *   EFI_STATUS EFIAPI
 *     EFIAPI 宏展开为 __attribute__((ms_abi))，指定 Microsoft x64 调用约定：
 *     前四个整数参数通过 RCX / RDX / R8 / R9 传递，与 Linux System V ABI
 *     （使用 RDI / RSI / RDX / RCX）不同。两种 ABI 混用会导致参数寄存器错位，
 *     引发极难调试的运行时错误，因此标注 EFIAPI 至关重要。
 *
 *   image_handle（EFI_HANDLE）
 *     固件为本程序分配的不透明句柄（本质是内部对象指针），用于：
 *       ① 查询 EFI_LOADED_IMAGE_PROTOCOL → 得知本程序所在存储设备
 *       ② 作为 OpenProtocol 的 AgentHandle（标识"谁在打开协议"）
 *       ③ 传给 ExitBootServices 标识退出的程序
 *
 *   system_table（EFI_SYSTEM_TABLE*）
 *     UEFI 系统表根指针。全局变量 gBS 已在 EDK II 库初始化时从
 *     system_table->BootServices 提取，本函数直接使用 gBS。
 *
 * ── 与旧版的核心差异：ELF 程序头驱动的两阶段内核加载 ───────────────────
 *
 *   旧版方案（直接平坦加载）：
 *     AllocatePages(固定地址 0x100000) → Read 整个 ELF 文件到该地址
 *     问题：把 ELF 文件头 + program header + section header 全部扔到内存里，
 *     实际可执行代码/数据混杂着 ELF 元数据，且加载地址硬编码，不支持
 *     链接到其他地址的内核或 PIE（位置无关可执行）内核。
 *
 *   新版方案（ELF 程序头驱动）：
 *     ① AllocatePool → 将 ELF 文件原样读入任意空闲内存（临时缓冲区）
 *     ② CalcLoadAddressRange → 解析 PT_LOAD 段头，计算内核所需地址范围
 *     ③ AllocatePages(精确地址) → 在内核链接地址分配连续物理页
 *     ④ CopyLoadSegments → 按段头描述，将各 PT_LOAD 段复制到目标地址，
 *                           并将 .bss（p_memsz > p_filesz 的部分）清零
 *     ⑤ FreePool → 释放临时缓冲区（ELF 原始文件不再需要）
 *     优势：地址由 ELF 程序头决定（灵活）、内存中只有真正需要执行的段、
 *           .bss 正确清零（旧版不清零会导致未初始化全局变量含垃圾值）。
 *
 * ── 引导流程总览（八个阶段）──────────────────────────────────────────────
 *
 *   阶段 1  获取物理内存映射          → 建立内存布局快照，取得 map_key
 *   阶段 2  打开 ESP 文件系统根目录   → 后续所有文件操作的基础
 *   阶段 3  保存内存映射到 \memmap    → 调试用 CSV（失败可忽略，非致命）
 *   阶段 4  初始化 GOP 帧缓冲区       → 获取显示参数并全屏清白
 *   阶段 5  两阶段 ELF 内核加载       → 临时缓冲 → 解析段头 → 按段复制
 *   阶段 6  退出 UEFI Boot Services   → 移交内存控制权，gBS 永久失效
 *   阶段 7  读取内核入口点地址        → 从已加载 ELF 头偏移 24 字节处读取
 *   阶段 8  构造 FrameBufferConfig 并跳转内核
 *
 * ── 错误处理策略 ──────────────────────────────────────────────────────────
 *
 *   致命错误 → Print 诊断信息 + Halt()（CPU 进入 HLT 休眠，永不返回）
 *   非致命错误 → Print 警告 + 继续（目前仅 \memmap 文件创建失败）
 *   %r 是 EDK II Print 的扩展格式符，将 EFI_STATUS 整数转为可读字符串。
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
 
  // 统一复用的返回值变量，每次 UEFI API 调用后立即覆盖并检查
  EFI_STATUS status;
 
  /* ── 阶段 0：最早期启动确认 ──────────────────────────────────────────── */
 
  // 向 UEFI 文本控制台输出启动标志；若此行不出现，说明固件未能加载本程序
  Print(L"Hello, Mikan World!\n");
 
  /* ── 阶段 1：获取物理内存映射快照 ───────────────────────────────────── */
 
  // 在栈上分配 16 KiB（4096 × 4 字节）作为描述符数组缓冲区。
  // 16 KiB ÷ ~48 B/条 ≈ 340 条，足以覆盖实际内存布局（通常 < 100 条）。
  CHAR8 memmap_buf[4096 * 4];
 
  // 初始化封装结构体：
  //   buffer_size → 告知固件缓冲区容量上限
  //   buffer      → 描述符数组写入位置
  //   后四个字段为 0，调用后由固件填充：
  //     map_size        → 实际使用字节数
  //     map_key         → 内存快照票据（ExitBootServices 必须凭此握手）
  //     descriptor_size → 单条描述符实际字节数（遍历时必须用此步进，
  //                       不能用 sizeof(EFI_MEMORY_DESCRIPTOR)，因固件可能扩展字段）
  //     descriptor_version → 描述符格式版本（当前规范固定为 1）
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  status = GetMemoryMap(&memmap);
  if (EFI_ERROR(status)) {
    // 无内存映射则 map_key 未知，无法安全调用 ExitBootServices，致命错误
    Print(L"failed to get memory map: %r\n", status);
    Halt();
  }
 
  /* ── 阶段 2：打开 ESP 文件系统根目录 ────────────────────────────────── */
 
  // 经由三层协议链定位根目录句柄：
  //   image_handle → EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle（本程序所在设备）
  //   DeviceHandle → EFI_SIMPLE_FILE_SYSTEM_PROTOCOL（FAT32 文件系统接口）
  //   OpenVolume() → EFI_FILE_PROTOCOL*（根目录句柄）
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    Print(L"failed to open root directory: %r\n", status);
    Halt();
  }
 
  /* ── 阶段 3：将内存映射保存为 CSV 文件（非致命操作）────────────────── */
 
  // 在 ESP 根目录下创建或覆盖 \memmap（READ|WRITE|CREATE 模式）
  EFI_FILE_PROTOCOL* memmap_file;
  status = root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(status)) {
    // \memmap 打开失败属于非致命错误——仅用于调试，不影响内核启动。
    // 可能原因：ESP 被固件写保护、空间不足、文件系统损坏。
    // 打印警告后继续执行（优雅降级），体现对"必要操作"与"辅助操作"的分级策略。
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
 
  /* ── 阶段 4：获取 GOP 帧缓冲区并全屏清白 ────────────────────────────── */
 
  // GOP（EFI_GRAPHICS_OUTPUT_PROTOCOL）提供帧缓冲区的直接内存访问（MMIO）。
  // FrameBufferBase 在 ExitBootServices 后依然有效，将封装进 FrameBufferConfig 传给内核。
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  status = OpenGOP(image_handle, &gop);
  if (EFI_ERROR(status)) {
    Print(L"failed to open GOP: %r\n", status);
    Halt();
  }
 
  // 打印显示参数供调试：
  //   HorizontalResolution / VerticalResolution → 屏幕逻辑分辨率（像素）
  //   PixelFormat     → 颜色分量字节序（RGB / BGR / BitMask / BltOnly）
  //   PixelsPerScanLine → 每行物理步长（含行末对齐填充，可能 > HorizontalResolution）
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
 
  // 打印帧缓冲区物理地址范围（[Base, Base+Size)）和总字节数
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  // 将帧缓冲区每字节写为 255（0xFF），实现全屏清白（RGB/BGR 格式均适用）。
  // 此操作同时验证 FrameBufferBase 可按字节直接写入（非 PixelBltOnly 格式）。
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── 阶段 5：两阶段 ELF 内核加载 ────────────────────────────────────── */
  /*
   * ┌────────────────────────────────────────────────────────────────────┐
   * │  为什么需要两阶段加载，而非直接加载到固定地址？                     │
   * │                                                                    │
   * │  旧版直接加载到 0x100000 存在以下问题：                            │
   * │   ① 把 ELF 文件头 + 元数据混入内存执行区域                         │
   * │   ② .bss 段未清零（未初始化全局变量含垃圾值）                      │
   * │   ③ 内核链接地址硬编码为 0x100000，无法支持 PIE 内核               │
   * │                                                                    │
   * │  两阶段流程：                                                       │
   * │   Phase A  AllocatePool → 将 ELF 文件原样读入任意空闲内存          │
   * │             （"临时缓冲区"，地址由固件自由选择）                    │
   * │   Phase B  CalcLoadAddressRange → 解析 PT_LOAD 段头，             │
   * │             确定内核实际占用的虚拟地址范围 [first, last)           │
   * │   Phase C  AllocatePages(AllocateAddress) →                       │
   * │             在 first 精确分配 (last-first) 字节的物理页            │
   * │   Phase D  CopyLoadSegments → 按段头，将各 PT_LOAD 内容           │
   * │             从临时缓冲区复制到目标虚拟地址，.bss 部分清零          │
   * │   Phase E  FreePool → 释放临时缓冲区（ELF 原始文件不再需要）       │
   * └────────────────────────────────────────────────────────────────────┘
   */
 
  // ── Phase A：打开内核 ELF 文件，获取文件大小，读入临时缓冲区 ──
 
  // 以只读模式打开 \kernel.elf；不存在则无法继续引导，致命错误
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\kernel.elf': %r\n", status);
    Halt();
  }
 
  // EFI_FILE_INFO 是可变长结构体（固定头 + UTF-16 文件名）。
  // "kernel.elf" = 10 字符 + 终止符 = 11 个 CHAR16，预留 12 个留余量。
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
  // gEfiFileInfoGuid 指定返回 EFI_FILE_INFO 类型元数据；此处只关心 FileSize 字段
  status = kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to get file information: %r\n", status);
    Halt();
  }
 
  // #@@range_begin(read_kernel)
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;  // 内核文件实际字节数
 
  // AllocatePool：从固件管理的堆（EFI Boot Services 内存池）分配 kernel_file_size 字节。
  // 与 AllocatePages 的区别：
  //   AllocatePool  → 字节粒度，地址由固件自由选择（适合临时数据缓冲）
  //   AllocatePages → 页粒度（4 KiB），可指定精确物理地址（适合长期运行的内核）
  // 此处用 AllocatePool 是因为目标地址尚未确定（需解析 ELF 段头后才知道）。
  // EfiLoaderData → 标记为引导程序数据，内核启动后可视需求回收
  VOID* kernel_buffer;
  status = gBS->AllocatePool(EfiLoaderData, kernel_file_size, &kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pool: %r\n", status);
    Halt();
  }
 
  // 将 ELF 文件整体读入临时缓冲区 kernel_buffer。
  // kernel_file_size 传入为"期望读取字节数"，返回后更新为"实际读取字节数"。
  status = kernel_file->Read(kernel_file, &kernel_file_size, kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"error: %r", status);
    Halt();
  }
  // #@@range_end(read_kernel)
 
  // ── Phase B & C：解析 ELF 段头，计算加载地址范围，分配目标物理页 ──
 
  // #@@range_begin(alloc_pages)
  // 将临时缓冲区起始地址解释为 ELF64 文件头指针（elf.hpp 中定义的 Elf64_Ehdr）
  Elf64_Ehdr* kernel_ehdr = (Elf64_Ehdr*)kernel_buffer;
 
  // CalcLoadAddressRange：扫描所有 PT_LOAD 段头，
  //   kernel_first_addr ← min(p_vaddr)                  所有 LOAD 段中最低虚拟地址
  //   kernel_last_addr  ← max(p_vaddr + p_memsz)         所有 LOAD 段中最高结束地址
  // 两者之差即内核在虚拟地址空间中占用的总字节范围。
  UINT64 kernel_first_addr, kernel_last_addr;
  CalcLoadAddressRange(kernel_ehdr, &kernel_first_addr, &kernel_last_addr);
 
  // 向上取整到 4 KiB 页边界，计算需要分配的物理页数。
  // 公式：(size + 0xfff) / 0x1000 等价于 ceil(size / 4096)
  UINTN num_pages = (kernel_last_addr - kernel_first_addr + 0xfff) / 0x1000;
 
  // AllocatePages(AllocateAddress, ...)：要求固件在精确的 kernel_first_addr 处分配。
  // 若该地址范围已被其他用途占用，返回 EFI_NOT_FOUND 并停机（致命错误）。
  // 注意：kernel_first_addr 由 ELF 段头决定，而非硬编码 0x100000，
  //       这使引导程序能正确加载链接到任意地址的内核（提升灵活性）。
  status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                              num_pages, &kernel_first_addr);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pages: %r\n", status);
    Halt();
  }
  // #@@range_end(alloc_pages)
 
  // ── Phase D & E：按段头复制内容，释放临时缓冲区 ──
 
  // #@@range_begin(copy_segments)
  // CopyLoadSegments：遍历所有 PT_LOAD 段头，执行两步操作：
  //   步骤 1（CopyMem）：将 kernel_buffer 中偏移 p_offset 处的 p_filesz 字节
  //                      复制到虚拟地址 p_vaddr 处（这里即是物理地址，恒等映射）
  //   步骤 2（SetMem 0）：将 [p_vaddr + p_filesz, p_vaddr + p_memsz) 范围清零
  //                       处理 .bss 段（未初始化全局变量）：p_memsz > p_filesz 时，
  //                       多出的字节在文件中不存在，必须显式清零；
  //                       若不清零，这些字节会含有物理内存中的历史垃圾数据，
  //                       导致 C/C++ 全局变量初值不为 0，违反语言规范。
  CopyLoadSegments(kernel_ehdr);
  Print(L"Kernel: 0x%0lx - 0x%0lx\n", kernel_first_addr, kernel_last_addr);
 
  // 释放临时缓冲区：ELF 原始文件的内容已经按段复制到目标地址，
  // kernel_buffer 中的数据不再需要，归还给 Boot Services 内存池。
  // 注意：必须在 ExitBootServices 之前释放；ExitBootServices 后
  //       gBS 失效，无法再调用 FreePool。
  status = gBS->FreePool(kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to free pool: %r\n", status);
    Halt();
  }
  // #@@range_end(copy_segments)
 
  /* ── 阶段 6：退出 UEFI Boot Services ────────────────────────────────── */
 
  // ExitBootServices 是引导流程的"不归路"：调用成功后 gBS 永久失效。
  //
  // map_key 握手机制：
  //   固件验证"内存映射自阶段 1 快照后未发生任何变化"；
  //   若固件内部（事件回调等）在调用前修改了内存分配，map_key 会改变，
  //   此调用返回 EFI_INVALID_PARAMETER，需重新获取内存映射后重试。
  //
  // 重试约束（关键）：
  //   GetMemoryMap 成功后到第二次 ExitBootServices 之间绝对不能有
  //   任何可能触发内存操作的调用——连 Print 都不行；
  //   代码中重试路径直接从 GetMemoryMap 成功后进入第二次调用，中间零额外操作。
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
    // map_key 过期，立即刷新后重试（中间不能有任何额外操作）
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      Print(L"failed to get memory map: %r\n", status);
      Halt();
    }
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      Halt();
    }
  }
  // ！！！从此处起 gBS 永久失效，任何 gBS->xxx() 均为未定义行为 ！！！
 
  /* ── 阶段 7：读取内核入口点地址 ─────────────────────────────────────── */
 
  // #@@range_begin(get_entry_point)
  // 从已加载内核的 ELF 文件头偏移 24 字节处读取 e_entry（程序入口点虚拟地址）。
  //
  // 为什么从 kernel_first_addr + 24 读取，而非从旧版的 kernel_buffer + 24 读取？
  //   ① kernel_buffer 已在阶段 5 Phase E 被 FreePool 释放，访问是未定义行为
  //   ② CopyLoadSegments 将第一个 PT_LOAD 段的内容（通常包含 ELF 文件头）
  //      复制到了 p_vaddr = kernel_first_addr，因此 ELF 文件头现在在：
  //        物理地址 kernel_first_addr（偏移 0）处
  //      → e_entry 在物理地址 kernel_first_addr + 24 处
  //
  // ELF64 文件头偏移速查：
  //   偏移  0 (16B) e_ident    魔数 + 类/字节序/版本/OS ABI
  //   偏移 16 ( 2B) e_type     文件类型
  //   偏移 18 ( 2B) e_machine  目标架构
  //   偏移 20 ( 4B) e_version  ELF 版本
  //   偏移 24 ( 8B) e_entry ←─ 程序入口点虚拟地址（此处读取）
  //
  // 前提：内核采用恒等映射（虚拟地址 == 物理地址），可直接作为物理地址调用。
  UINT64 entry_addr = *(UINT64*)(kernel_first_addr + 24);
  // #@@range_end(get_entry_point)
 
  /* ── 阶段 8：构造 FrameBufferConfig 并跳转内核 ───────────────────────── */
 
  // 将 GOP 显示参数封装为内核/引导程序共用的 FrameBufferConfig 结构体。
  //
  // 字段对应关系（参见 frame_buffer_config.hpp）：
  //   frame_buffer         → 帧缓冲区字节指针（MMIO 物理地址，ExitBS 后依然有效）
  //   pixels_per_scan_line → 每扫描行像素步长（含行末对齐填充）
  //                          ★ 必须用此值计算行首偏移，而非 horizontal_resolution★
  //                          原因：硬件可能在每行末尾插入对齐填充像素；
  //                          误用 horizontal_resolution 会导致第二行以后的图像向左斜切
  //   horizontal_resolution → 屏幕逻辑宽度（用于绘图边界检查）
  //   vertical_resolution   → 屏幕逻辑高度（用于绘图边界检查）
  //   pixel_format          → 先置 0，由下方 switch 填入正确枚举值
  //
  // 为何不直接传 gop->Mode->Info* 指针给内核？
  //   ① 该指针指向 EfiBootServicesData 类型内存，内核可能将其页面回收
  //   ② 内核不应依赖 UEFI 头文件（EFI_GRAPHICS_OUTPUT_MODE_INFORMATION），
  //      FrameBufferConfig 彻底解耦引导程序与内核的编译依赖
  struct FrameBufferConfig config = {
    (UINT8*)gop->Mode->FrameBufferBase,    // 帧缓冲区起始地址（转为字节指针）
    gop->Mode->Info->PixelsPerScanLine,    // 行步长（像素数，含行末填充）
    gop->Mode->Info->HorizontalResolution, // 水平分辨率（像素数）
    gop->Mode->Info->VerticalResolution,   // 垂直分辨率（像素数）
    0                                      // pixel_format 占位，由 switch 填入
  };
 
  // 将 UEFI 的 EFI_GRAPHICS_PIXEL_FORMAT 枚举映射到内核的 PixelFormat 枚举。
  // 显式 switch 而非直接赋值的原因：
  //   ① 两套枚举独立定义，未来任意一方调整顺序时不互相影响
  //   ② 不支持的格式（PixelBitMask / PixelBltOnly）在此处被拦截，
  //      比在内核内部遇到非法值时崩溃更易定位问题
  switch (gop->Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
      // 内存布局（低→高）：[R][G][B][保留]，大多数现代 PC 显卡的默认格式
      config.pixel_format = kPixelRGBResv8BitPerColor;
      break;
    case PixelBlueGreenRedReserved8BitPerColor:
      // 内存布局（低→高）：[B][G][R][保留]，R 和 B 字节与 RGB 格式互换
      config.pixel_format = kPixelBGRResv8BitPerColor;
      break;
    default:
      // PixelBitMask 和 PixelBltOnly 均未实现；遇到则停机，
      // 避免向内核传递无意义的 pixel_format=0 后产生颜色错误
      Print(L"Unimplemented pixel format: %d\n", gop->Mode->Info->PixelFormat);
      Halt();
  }
 
  // 将入口地址转换为接受 FrameBufferConfig 指针的函数指针并调用。
  //
  // 内核侧对应声明（kernel/main.cpp）：
  //   extern "C" void KernelMain(const FrameBufferConfig& config)
  //   C++ 引用（&）与 C 指针（*）在 System V AMD64 ABI 中传参方式相同：
  //   均通过 RDI 寄存器传递 config 结构体的地址。
  //   extern "C" 抑制了 C++ 名称修饰，保证符号名为 "KernelMain"，
  //   与 ELF e_entry 指向的函数匹配。
  //
  // 调用后控制权永久移交内核；本函数以下代码永远不会再执行。
  typedef void EntryPointType(const struct FrameBufferConfig*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(&config);
 
  /* ── 不可达代码：防止编译器警告与意外 CPU 滑行 ───────────────────────── */
 
  // Print 和 while(1) 在正常流程中永远不会执行：
  //   Print  → Boot Services 已退出，实际无效，但保留作最后调试线索
  //   while  → 防止 CPU 滑入函数末尾的随机栈数据
  //   return → 满足 C 编译器"非 void 函数需有 return"的语法要求
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 