#include  <Uefi.h>
#include  <Library/UefiLib.h>
#include  <Library/UefiBootServicesTableLib.h>
#include  <Library/PrintLib.h>
#include  <Library/MemoryAllocationLib.h>
#include  <Protocol/LoadedImage.h>
#include  <Protocol/SimpleFileSystem.h>
#include  <Protocol/DiskIo2.h>
#include  <Protocol/BlockIo.h>
#include  <Guid/FileInfo.h>
 
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
 
/**
 * Halt - 安全停机函数：无限执行 HLT 指令使 CPU 进入低功耗休眠
 *
 * 作为独立函数而非在每处错误路径内联 while(1) 的原因：
 *   1. 语义更清晰：调用 Halt() 明确表达"程序在此终止"的意图，
 *      读者无需判断 while(1) 是错误停机还是正常事件循环。
 *   2. 便于调试：在 Halt 上设置断点即可捕获所有致命错误路径。
 *   3. 可扩展性：未来可在此函数内添加错误日志刷盘、蜂鸣报警等操作。
 *
 * HLT 指令（机器码 0xF4）：
 *   使当前逻辑 CPU 停止取指，等待外部中断/NMI/RESET 信号后唤醒。
 *   当前引导程序阶段尚未设置 IDT，任何中断都会触发三重故障（Triple Fault）
 *   导致 CPU 复位，因此 HLT 实际上是永久停机。
 *   外层 while(1) 确保万一 HLT 被意外唤醒后立即再次执行 HLT，
 *   而不是滑入函数返回地址处的随机内存。
 */
// #@@range_begin(halt)
void Halt(void) {
  while (1) __asm__("hlt");
}
// #@@range_end(halt)
 
/**
 * UefiMain - UEFI 应用程序入口点（MikanOS 引导加载程序主函数）
 *
 * 本版本相比前一版本的核心改进：全面的错误检查与优雅降级
 * ─────────────────────────────────────────────────────────
 * 前一版本：所有 UEFI API 调用均忽略返回值，错误时行为未定义。
 * 本版本  ：每个 UEFI API 调用都检查返回值，错误分为两类：
 *   ① 致命错误（fatal）   → 打印诊断信息后调用 Halt() 安全停机
 *   ② 非致命错误（soft）  → 打印警告后继续执行（目前仅 \memmap 文件）
 *
 * 引导流程总览（共七个阶段）：
 *   阶段 1  获取物理内存映射          → 建立内存布局快照
 *   阶段 2  打开 ESP 文件系统根目录   → 后续文件读写前提
 *   阶段 3  保存内存映射到 \memmap    → 调试用，失败可忽略
 *   阶段 4  初始化 GOP 帧缓冲区       → 获取显示参数并清屏
 *   阶段 5  加载内核 ELF 到物理内存   → \kernel.elf → 0x100000
 *   阶段 6  退出 UEFI Boot Services   → 移交内存控制权
 *   阶段 7  跳转内核并传递显示参数    → entry_point(Base, Size)
 *
 * 参数：
 *   image_handle   固件为本程序分配的句柄（查设备/作调用者标识/传给ExitBS）
 *   system_table   UEFI 系统表根指针（全局 gBS 已从其 BootServices 字段提取）
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
 
  // 统一复用的状态变量：每次 UEFI API 调用后立即覆盖并检查。
  // 将所有返回值检查集中到同一变量，避免局部变量泛滥，
  // 同时在调用失败时 status 保留了可供 Print(%r) 格式化的错误码。
  EFI_STATUS status;
 
  /* ── 阶段 0：最早期启动确认 ──────────────────────────────────────────── */
 
  // 向 UEFI 控制台（EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL）打印启动标志。
  // 若此行不出现，说明固件未能正常加载本程序（可检查 BOOT 顺序或 EFI 文件路径）。
  Print(L"Hello, Mikan World!\n");
 
  /* ── 阶段 1：获取物理内存映射快照 ───────────────────────────────────── */
 
  // 在栈上分配 16 KiB 缓冲区。选栈而非堆（AllocatePool）的原因：
  //   - 生命周期与函数帧绑定，无需手动释放
  //   - 16 KiB 可容纳 ~340 条描述符（每条 ~48 B），足覆盖实际内存布局
  CHAR8 memmap_buf[4096 * 4];
 
  // 初始化结构体（buffer_size=容量, buffer=存储位置, 其余输出字段初始为 0）
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
 
  // 获取内存映射。成功后 memmap.map_key 持有"快照票据"，
  // ExitBootServices 必须凭此票据证明内存状态未被修改。
  // 失败原因通常是缓冲区太小（EFI_BUFFER_TOO_SMALL）或固件状态异常。
  status = GetMemoryMap(&memmap);
  if (EFI_ERROR(status)) {
    // %r 是 EDK II Print 的扩展格式符，将 EFI_STATUS 整数转为可读字符串
    // （如 "Buffer Too Small"、"Invalid Parameter" 等）
    Print(L"failed to get memory map: %r\n", status);
    Halt();  // 无内存映射则无法安全退出 Boot Services，致命错误
  }
 
  /* ── 阶段 2：打开 ESP 文件系统根目录 ────────────────────────────────── */
 
  // 经由三层协议链获取根目录句柄（详见 OpenRootDir 的注释）：
  //   image_handle → EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle
  //   DeviceHandle → EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  //   OpenVolume() → EFI_FILE_PROTOCOL*（根目录）
  // 本版本 OpenRootDir 内部也已加入错误检查，会将错误码透传回来。
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    Print(L"failed to open root directory: %r\n", status);
    Halt();  // 根目录不可用则无法读取任何文件，致命错误
  }
 
  /* ── 阶段 3：将内存映射保存为 CSV 文件（非致命操作）────────────────── */
 
  // 在 ESP 根目录下创建或覆盖 \memmap 文件。
  //   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE → 读写权限
  //   EFI_FILE_MODE_CREATE → 不存在则创建，存在则截断重写
  //   末尾参数 0 → 普通文件属性（非只读/隐藏/目录）
  EFI_FILE_PROTOCOL* memmap_file;
  status = root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
 
  if (EFI_ERROR(status)) {
    // \memmap 文件打开失败属于非致命错误，原因可能是：
    //   - ESP 分区只读（某些固件对系统分区加了写保护）
    //   - 分区空间不足
    //   - 文件系统损坏
    // \memmap 仅用于调试，不影响内核加载和启动，因此打印警告后继续执行。
    // 这是本版本引入的"优雅降级"设计：区分必要操作与辅助操作的错误策略。
    Print(L"failed to open file '\\memmap': %r\n", status);
    Print(L"Ignored.\n");
  } else {
    // 文件打开成功，执行写入流程；写入和关闭失败则为致命错误
    // （写入一半的 CSV 比没有文件更容易误导调试，故选择停机而非继续）
 
    // 将内存描述符数组格式化为 CSV 逐行写入文件
    // （字段：Index, Type, Type名称, PhysicalStart, NumberOfPages, Attribute）
    status = SaveMemoryMap(&memmap, memmap_file);
    if (EFI_ERROR(status)) {
      Print(L"failed to save memory map: %r\n", status);
      Halt();
    }
 
    // 关闭文件句柄，通知文件系统驱动该文件的写操作已完成。
    // Close 的返回值在之前的版本中被忽略；本版本明确检查，
    // 因为 Close 失败可能意味着文件系统缓存刷新异常（文件内容可能不完整）。
    status = memmap_file->Close(memmap_file);
    if (EFI_ERROR(status)) {
      Print(L"failed to close memory map: %r\n", status);
      Halt();
    }
  }
 
  /* ── 阶段 4：获取 GOP 帧缓冲区并清屏 ────────────────────────────────── */
 
  // GOP（EFI_GRAPHICS_OUTPUT_PROTOCOL）提供直接内存访问的像素帧缓冲区。
  // 其 FrameBufferBase 是 MMIO 物理地址，在 ExitBootServices 后依然有效，
  // 将作为参数传入内核供其直接绘制屏幕。
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  status = OpenGOP(image_handle, &gop);
  if (EFI_ERROR(status)) {
    // GOP 不可用时内核将无法输出任何图形，属于致命错误。
    // 注意：若此处 Halt()，屏幕上可能什么都看不到（GOP 尚未初始化），
    // 但串口或 UEFI 文本控制台仍可显示此错误信息。
    Print(L"failed to open GOP: %r\n", status);
    Halt();
  }
 
  // 打印显示参数供调试确认：
  //   HorizontalResolution / VerticalResolution → 屏幕逻辑分辨率（像素数）
  //   PixelFormat     → 颜色分量字节序（RGB/BGR/BitMask/BltOnly），
  //                     决定内核写像素时的字节排列方式
  //   PixelsPerScanLine → 每扫描行的物理步长（含硬件行末对齐填充），
  //                       计算行首偏移必须乘以此值而非 HorizontalResolution，
  //                       否则在有填充的显卡上画面会呈现斜切（shear）效果
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
 
  // 打印帧缓冲区物理地址范围，供内核或调试器参考：
  //   FrameBufferBase                     → 起始地址（含）
  //   FrameBufferBase + FrameBufferSize   → 结束地址（不含）
  //   FrameBufferSize = PixelsPerScanLine × VerticalResolution × 每像素字节数
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  // 将帧缓冲区每字节写为 255（0xFF），实现全屏清白。
  // 对 RGB/BGR 格式：R=G=B=255 即最大亮度白色，Reserved 字节被显示控制器忽略。
  // 此操作同时验证 FrameBufferBase 地址可直接按字节写入（非 PixelBltOnly 格式）。
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── 阶段 5：读取并加载内核 ELF 文件 ────────────────────────────────── */
 
  // 以只读模式打开 \kernel.elf；失败则无法继续引导，致命错误
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\kernel.elf': %r\n", status);
    Halt();
  }
 
  // EFI_FILE_INFO 是可变长结构体（固定头部 + UTF-16 文件名字符串）。
  // "kernel.elf" = 10 字符 + 终止符 = 11 个 CHAR16，预留 12 个留余量。
  // 缓冲区大小 = 固定部分 + 12 × sizeof(CHAR16)（每个 CHAR16 占 2 字节）。
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
 
  // 通过 gEfiFileInfoGuid 查询文件元数据（大小、时间戳、文件名等）。
  // GetInfo 是泛型接口，GUID 决定返回信息类型；
  // 此处只关心 EFI_FILE_INFO.FileSize 字段以确定后续内存分配大小。
  status = kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to get file information: %r\n", status);
    Halt();
  }
 
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;  // 内核文件的实际字节数
 
  // #@@range_begin(alloc_error)
  // 在物理地址 0x100000（1 MiB）处申请连续物理页存放内核镜像。
  //
  // 选择 1 MiB 作为加载基址（避开 x86 低 1 MiB 遗留区）：
  //   0x00000~0x003FF  实模式中断向量表（IVT）
  //   0x00400~0x004FF  BIOS 数据区（BDA）
  //   0x0A000~0x0BFFF  传统 VGA 显存映射
  //   0x0F000~0x0FFFF  BIOS ROM 影子区
  // 且与内核链接脚本的 LMA/VMA 起始地址 0x100000 一致。
  //
  // AllocateAddress → 要求固件在精确的指定地址分配（不允许固件自由选择）
  // EfiLoaderData   → 标记为引导程序数据，内核启动后按需回收
  // 页数计算：(size + 0xfff) / 0x1000 = 向上取整到 4 KiB 页边界
  //
  // 本版本新增了此处的错误检查（前版本忽略了 AllocatePages 的返回值）。
  // 可能的失败原因：0x100000 处已被固件占用（某些固件会在低地址放置数据结构）
  // 或物理内存不足（极不可能，但仍需处理）。
  EFI_PHYSICAL_ADDRESS kernel_base_addr = 0x100000;
  status = gBS->AllocatePages(
      AllocateAddress, EfiLoaderData,
      (kernel_file_size + 0xfff) / 0x1000, &kernel_base_addr);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pages: %r", status);
    Halt();
  }
  // #@@range_end(alloc_error)
 
  // 将内核 ELF 文件整体读入刚分配的物理内存。
  // kernel_file_size 传入时为"期望读取字节数"，返回后更新为"实际读取字节数"。
  // 本实现未按 PT_LOAD segment 分段映射，直接原样加载，
  // 依赖链接脚本保证 LMA == VMA == 0x100000 的平坦布局。
  status = kernel_file->Read(kernel_file, &kernel_file_size, (VOID*)kernel_base_addr);
  if (EFI_ERROR(status)) {
    Print(L"error: %r", status);
    Halt();
  }
  Print(L"Kernel: 0x%0lx (%lu bytes)\n", kernel_base_addr, kernel_file_size);
 
  /* ── 阶段 6：退出 UEFI Boot Services ────────────────────────────────── */
  // #@@range_begin(exit_bs)
 
  // ExitBootServices 是引导流程的"不归路"：调用成功后 gBS 永久失效。
  //
  // map_key 握手机制：
  //   固件验证"内存映射自阶段1快照后未发生任何变化"；
  //   若固件内部（事件回调等）在本次调用前修改了内存分配，map_key 会改变，
  //   调用返回 EFI_INVALID_PARAMETER，需重新获取内存映射后重试。
  //
  // 重试约束（关键）：
  //   GetMemoryMap 与第二次 ExitBootServices 之间绝对不能有任何
  //   可能触发内存操作的调用——连 Print 也不行，
  //   否则 map_key 在间隙再次失效，导致无法退出的死循环。
  //   （观察代码：重试路径中 GetMemoryMap 失败才调用 Print，
  //    成功路径直接进入第二次 ExitBootServices，中间无任何其他调用。）
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
  // #@@range_end(exit_bs)
 
  /* ── 阶段 7：读取内核入口地址并跳转，传递 GOP 显示参数 ─────────────── */
 
  // 从 ELF64 文件头偏移 24 字节处读取 e_entry 字段（程序入口点虚拟地址）。
  //
  // ELF64 文件头字段偏移速查：
  //   偏移  0 (16B): e_ident    魔数 "\x7fELF" + 类/字节序/版本/OS ABI
  //   偏移 16 ( 2B): e_type     文件类型（ET_EXEC=2 可执行文件）
  //   偏移 18 ( 2B): e_machine  目标架构（EM_X86_64=62）
  //   偏移 20 ( 4B): e_version  ELF 版本（固定为 1）
  //   偏移 24 ( 8B): e_entry ←─ 程序入口点虚拟地址（此处读取）
  //   偏移 32 ( 8B): e_phoff    程序头表（PHT）文件偏移
  //   ...
  //
  // 前提：内核采用恒等映射（虚拟地址 == 物理地址），
  // 因此 e_entry 可直接作为物理地址调用，无需先建立页表。
  UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);
 
  // 将入口地址转换为接受两个 UINT64 参数的函数指针并调用。
  //
  // 函数签名 EntryPointType(UINT64, UINT64) 与内核的：
  //   extern "C" void KernelMain(uint64_t frame_buffer_base,
  //                              uint64_t frame_buffer_size)
  // 完全对应（extern "C" 抑制了 C++ 名称修饰，保证符号名为 KernelMain）。
  //
  // 参数传递（System V AMD64 ABI，内核侧采用此 ABI）：
  //   第 1 参数 FrameBufferBase → RDI 寄存器
  //   第 2 参数 FrameBufferSize → RSI 寄存器
  //
  // 为何在 ExitBootServices 之后仍能使用 gop 指针？
  //   gop->Mode->FrameBufferBase / FrameBufferSize 是 MMIO 物理地址和静态数值，
  //   它们在 ExitBootServices 后依然有效（不依赖 Boot Services 的任何服务）。
  //   只要不再调用 gop->QueryMode / gop->SetMode / gop->Blt 等方法即可安全读取。
  //
  // 调用后控制权永久移交内核，本函数以下代码永远不会执行。
  typedef void EntryPointType(UINT64, UINT64);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize);
 
  /* ── 不可达代码：防止编译器警告与意外 CPU 滑行 ───────────────────────── */
 
  // 以下代码在正常流程中永远不会执行。
  // 保留原因：
  //   1. C 编译器要求非 void 函数所有路径须有 return（否则警告/错误）
  //   2. 若内核意外返回，Print 提供最后一条调试信息
  //      （Boot Services 已退出，Print 已不可用，但至少不会立即在野指针上崩溃）
  //   3. while(1) 防止 CPU 执行完 Print 后滑入函数末尾的随机栈数据
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 
 