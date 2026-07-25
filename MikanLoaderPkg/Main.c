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
#include  "frame_buffer_config.hpp"
 
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
 
/**
 * UefiMain - UEFI 应用程序入口点（MikanOS 引导加载程序主函数）
 *
 * ── 函数签名约定 ────────────────────────────────────────────────────────────
 *
 *   EFI_STATUS EFIAPI
 *     EFIAPI 宏展开为 __attribute__((ms_abi))，指定 Microsoft x64 调用约定：
 *     前四个整数参数依次通过 RCX / RDX / R8 / R9 传递，调用者负责清栈。
 *     与 Linux System V ABI（参数用 RDI/RSI/RDX/RCX）不同；
 *     混用两种 ABI 会导致参数寄存器错位，引发极难调试的运行时错误。
 *
 *   image_handle（EFI_HANDLE）
 *     固件为本程序分配的不透明句柄（本质是内部对象指针），用于：
 *       ① 查询 EFI_LOADED_IMAGE_PROTOCOL → 得知本程序所在存储设备
 *       ② 作为 OpenProtocol 的 AgentHandle（标识"谁在打开协议"）
 *       ③ 传给 ExitBootServices 标识退出的程序
 *
 *   system_table（EFI_SYSTEM_TABLE*）
 *     UEFI 系统表根指针，包含所有固件服务的入口：
 *       BootServices    → 引导期服务（内存/文件/协议，ExitBootServices 后失效）
 *       RuntimeServices → 运行时服务（时钟/NVRAM/重启，全程有效）
 *       ConfigurationTable → ACPI / SMBIOS 等配置表数组
 *     全局变量 gBS 已在 EDK II 库初始化时从 system_table->BootServices 提取，
 *     本函数直接使用 gBS，无需再通过 system_table 间接访问。
 *
 * ── 引导流程总览（七个阶段）──────────────────────────────────────────────────
 *
 *   阶段 1  获取物理内存映射          → 建立内存布局快照，取得 map_key
 *   阶段 2  打开 ESP 文件系统根目录   → 后续所有文件操作的基础
 *   阶段 3  保存内存映射到 \memmap    → 调试用 CSV（失败可忽略，非致命）
 *   阶段 4  初始化 GOP 帧缓冲区       → 获取显示参数并全屏清白
 *   阶段 5  加载内核 ELF 到物理内存   → \kernel.elf → 物理地址 0x100000
 *   阶段 6  退出 UEFI Boot Services   → 移交内存控制权，gBS 永久失效
 *   阶段 7  构造 FrameBufferConfig 并跳转内核
 *
 * ── 错误处理策略 ──────────────────────────────────────────────────────────────
 *
 *   致命错误（fatal）  → Print 诊断信息 + Halt()（CPU 进入 HLT 休眠，永不返回）
 *   非致命错误（soft） → Print 警告 + 继续执行（目前仅 \memmap 文件创建失败）
 *   status 变量全函数复用，每次 UEFI API 调用后立即覆盖并检查；
 *   %r 是 EDK II Print 的扩展格式符，将 EFI_STATUS 整数转为可读字符串。
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
 
  // 统一复用的返回值变量，避免局部变量泛滥，同时保留错误码供 Print(%r) 格式化
  EFI_STATUS status;
 
  /* ── 阶段 0：最早期启动确认 ──────────────────────────────────────────── */
 
  // 向 UEFI 文本控制台输出启动标志（EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL）。
  // 若此行不出现，说明固件未能加载本程序（检查 BOOT 顺序或 .efi 文件路径）。
  Print(L"Hello, Mikan World!\n");
 
  /* ── 阶段 1：获取物理内存映射快照 ───────────────────────────────────── */
 
  // 在栈上分配 16 KiB（4096 × 4 字节）作为描述符数组缓冲区。
  // 选栈分配而非堆（AllocatePool）：生命周期与函数帧绑定，无需手动释放，
  // 且 16 KiB ÷ ~48 B/条 ≈ 340 条，足覆盖实际内存布局（通常 < 100 条）。
  CHAR8 memmap_buf[4096 * 4];
 
  // 初始化封装结构体：
  //   buffer_size = sizeof(memmap_buf)  → 告知固件缓冲区容量上限
  //   buffer      = memmap_buf          → 描述符数组写入位置
  //   后四个字段为 0，调用后由固件填充：
  //     map_size         → 实际使用字节数
  //     map_key          → 内存快照票据（ExitBootServices 必须凭此握手）
  //     descriptor_size  → 单条描述符实际字节数（含固件私有扩展，遍历时必须用此步进）
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
  //   OpenVolume() → EFI_FILE_PROTOCOL*（根目录句柄，后续 Open/Read 的起点）
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    // 根目录不可用则无法读取任何文件，致命错误
    Print(L"failed to open root directory: %r\n", status);
    Halt();
  }
 
  /* ── 阶段 3：将内存映射保存为 CSV 文件（非致命操作）────────────────── */
 
  // 在 ESP 根目录下创建或覆盖 \memmap：
  //   READ | WRITE → 读写权限（UEFI Write 也需要 READ 标志）
  //   CREATE       → 不存在则创建，已存在则截断重写
  //   末尾 0       → 普通文件属性（非只读/隐藏/系统/目录）
  EFI_FILE_PROTOCOL* memmap_file;
  status = root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
 
  if (EFI_ERROR(status)) {
    // \memmap 打开失败属于非致命错误——仅用于调试，不影响内核启动。
    // 可能原因：ESP 分区被固件写保护、分区空间不足、文件系统损坏。
    // 打印警告后继续执行（优雅降级），体现对"必要操作"与"辅助操作"的分级策略。
    Print(L"failed to open file '\\memmap': %r\n", status);
    Print(L"Ignored.\n");
  } else {
    // 文件成功打开，写入内存映射 CSV
    // （字段：Index, Type, Type名称, PhysicalStart, NumberOfPages, Attribute）
    status = SaveMemoryMap(&memmap, memmap_file);
    if (EFI_ERROR(status)) {
      // 写入一半的 CSV 比没有文件更容易误导调试，故选择停机
      Print(L"failed to save memory map: %r\n", status);
      Halt();
    }
    // Close 失败意味着文件系统缓存刷新异常（文件内容可能不完整），停机
    status = memmap_file->Close(memmap_file);
    if (EFI_ERROR(status)) {
      Print(L"failed to close memory map: %r\n", status);
      Halt();
    }
  }
 
  /* ── 阶段 4：获取 GOP 帧缓冲区并全屏清白 ────────────────────────────── */
 
  // GOP（EFI_GRAPHICS_OUTPUT_PROTOCOL）提供像素级帧缓冲区的直接内存访问，
  // 替代了旧式 VGA/VESA 接口。其 FrameBufferBase 是 MMIO 物理地址，
  // 在 ExitBootServices 后依然有效，将封装进 FrameBufferConfig 传给内核。
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  status = OpenGOP(image_handle, &gop);
  if (EFI_ERROR(status)) {
    // GOP 不可用时内核无法输出任何图形，致命错误
    Print(L"failed to open GOP: %r\n", status);
    Halt();
  }
 
  // 打印显示参数供调试：
  //   HorizontalResolution / VerticalResolution → 屏幕逻辑分辨率（像素）
  //   PixelFormat     → 颜色分量字节序（RGB/BGR/BitMask/BltOnly）
  //   PixelsPerScanLine → 每扫描行物理步长（含硬件行末对齐填充，
  //                       可能 > HorizontalResolution；
  //                       行首偏移必须乘以此值而非 HorizontalResolution）
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
 
  // 打印帧缓冲区物理地址范围：
  //   FrameBufferBase                    → 起始地址（含）
  //   FrameBufferBase + FrameBufferSize  → 结束地址（不含）
  //   FrameBufferSize = PixelsPerScanLine × VerticalResolution × 4（每像素字节数）
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  // 将帧缓冲区每字节写为 255（0xFF），实现全屏清白。
  // 对 RGB/BGR 格式：R=G=B=255 即最大亮度白色，Reserved 字节被显示控制器忽略。
  // 此操作同时验证 FrameBufferBase 可直接按字节写入（非 PixelBltOnly 格式）。
  // 注意：这是引导程序做的粗粒度清屏；内核随后会通过 PixelWriter 再次按坐标填白，
  // 仅覆盖逻辑可见区域（不含行末对齐填充），语义更精确。
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── 阶段 5：读取并加载内核 ELF 文件 ────────────────────────────────── */
 
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
  // 缓冲区 = sizeof(EFI_FILE_INFO) 固定部分 + 12 × 2 字节
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
 
  // gEfiFileInfoGuid 指定返回 EFI_FILE_INFO 类型元数据（大小、时间戳、文件名等）；
  // 此处只关心 FileSize 字段以确定内存分配大小
  status = kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to get file information: %r\n", status);
    Halt();
  }
 
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;  // 内核文件实际字节数
 
  // #@@range_begin(alloc_error)
  // 在物理地址 0x100000（1 MiB）申请连续物理页存放内核镜像。
  //
  // 选择 1 MiB 的原因（避开 x86 低 1 MiB 遗留区域）：
  //   0x00000~0x003FF  实模式中断向量表（IVT）
  //   0x00400~0x004FF  BIOS 数据区（BDA）
  //   0x0A000~0x0BFFF  传统 VGA 显存映射
  //   0x0F000~0x0FFFF  BIOS ROM 影子区
  // 且与内核链接脚本的 LMA/VMA 起始地址 0x100000 保持一致。
  //
  // AllocateAddress → 要求固件在精确的指定物理地址分配（不允许自由选择）
  // EfiLoaderData   → 标记为引导程序数据，内核启动后可视需求回收
  // 页数 = (size + 0xfff) / 0x1000 → 向上取整到 4 KiB 页边界
  EFI_PHYSICAL_ADDRESS kernel_base_addr = 0x100000;
  status = gBS->AllocatePages(
      AllocateAddress, EfiLoaderData,
      (kernel_file_size + 0xfff) / 0x1000, &kernel_base_addr);
  if (EFI_ERROR(status)) {
    // 失败原因：0x100000 处已被固件占用，或物理内存不足（极少见）
    Print(L"failed to allocate pages: %r", status);
    Halt();
  }
  // #@@range_end(alloc_error)
 
  // 将整个 ELF 文件原样读入已分配的物理内存。
  // kernel_file_size 传入为"期望读取字节数"，返回后更新为"实际读取字节数"。
  // 未按 PT_LOAD segment 分段映射，依赖链接脚本保证 LMA == VMA == 0x100000 的平坦布局。
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
  //   固件验证"内存映射自阶段 1 快照后未发生任何变化"；
  //   若固件内部（事件回调等）在调用前修改了内存分配，map_key 会改变，
  //   此调用返回 EFI_INVALID_PARAMETER，需重新获取内存映射后重试。
  //
  // 重试约束（关键）：
  //   GetMemoryMap 成功后到第二次 ExitBootServices 之间
  //   绝对不能有任何可能触发内存操作的调用——连 Print 都不行；
  //   代码中重试路径直接从 GetMemoryMap 成功分支进入第二次调用，中间零额外操作。
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
 
  /* ── 阶段 7：构造 FrameBufferConfig 并跳转内核 ───────────────────────── */
 
  // 从 ELF64 文件头偏移 24 字节处读取 e_entry（程序入口点虚拟地址）。
  // ELF64 文件头偏移速查：
  //   偏移  0 (16B) e_ident    魔数 "\x7fELF" + 类/字节序/版本/OS ABI
  //   偏移 16 ( 2B) e_type     文件类型（ET_EXEC=2）
  //   偏移 18 ( 2B) e_machine  目标架构（EM_X86_64=62）
  //   偏移 20 ( 4B) e_version  ELF 版本（固定为 1）
  //   偏移 24 ( 8B) e_entry ←─ 程序入口点虚拟地址（此处读取）
  // 前提：内核采用恒等映射（虚拟地址 == 物理地址），可直接作为物理地址调用。
  UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);
 
  // 将 GOP 的显示参数封装为 FrameBufferConfig 结构体，传递给内核。
  //
  // 字段对应关系（参见 frame_buffer_config.hpp）：
  //   frame_buffer         → 帧缓冲区字节指针（MMIO 物理地址，ExitBS 后依然有效）
  //   pixels_per_scan_line → 每扫描行像素步长（含行末对齐填充，行首寻址必须用此值）
  //   horizontal_resolution → 屏幕逻辑宽度（用于绘图边界检查）
  //   vertical_resolution   → 屏幕逻辑高度（用于绘图边界检查）
  //   pixel_format          → 先置 0，由下方 switch 填入正确枚举值
  //
  // 为何不直接传 gop->Mode->Info* 指针给内核？
  //   ① 该指针指向 EfiBootServicesData 类型内存，内核可能将其页面回收用作他用
  //   ② 内核不应依赖 UEFI 头文件（EFI_GRAPHICS_OUTPUT_MODE_INFORMATION），
  //      FrameBufferConfig 彻底解耦引导程序与内核的编译依赖
  struct FrameBufferConfig config = {
    (UINT8*)gop->Mode->FrameBufferBase,    // 帧缓冲区起始地址（转为字节指针）
    gop->Mode->Info->PixelsPerScanLine,    // 行步长（像素数）
    gop->Mode->Info->HorizontalResolution, // 水平分辨率（像素数）
    gop->Mode->Info->VerticalResolution,   // 垂直分辨率（像素数）
    0                                      // pixel_format 占位，由 switch 填入
  };
 
  // 将 UEFI 的 EFI_GRAPHICS_PIXEL_FORMAT 枚举映射到内核的 PixelFormat 枚举。
  //
  // 为何不直接赋值（两套枚举当前数值碰巧相同）？
  //   ① 两套枚举独立定义，未来任意一方调整顺序时不会互相影响
  //   ② 显式 switch 使不支持的格式（PixelBitMask / PixelBltOnly）在此处被拦截，
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
      // PixelBitMask（需解析掩码结构体）和 PixelBltOnly（无直接内存访问）均未实现，
      // 遇到则安全停机，避免传递无意义的 pixel_format=0 给内核后产生颜色错误
      Print(L"Unimplemented pixel format: %d\n", gop->Mode->Info->PixelFormat);
      Halt();
  }
 
  // 将入口地址转换为接受 FrameBufferConfig 指针的函数指针并调用。
  //
  // 内核侧对应声明（kernel/main.cpp）：
  //   extern "C" void KernelMain(const FrameBufferConfig& config)
  //   C++ 引用（&）与 C 指针（*）在 System V AMD64 ABI 中传参方式相同：
  //   均通过 RDI 寄存器传递 config 结构体的地址。
  //   extern "C" 抑制了名称修饰，保证符号名为 KernelMain，与 e_entry 对应。
  //
  // 调用后控制权永久移交内核；本函数以下代码永远不会再执行。
  typedef void EntryPointType(const struct FrameBufferConfig*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(&config);
 
  /* ── 不可达代码：防止编译器警告与意外 CPU 滑行 ───────────────────────── */
 
  // 以下代码在正常流程中永远不会执行，保留原因：
  //   1. C 编译器要求非 void 函数所有路径须有 return，否则产生警告
  //   2. 若内核意外返回，Print 提供最后一条调试线索
  //      （Boot Services 已退出，Print 实际不可用，但不会立即在野指针上崩溃）
  //   3. while(1) 防止 CPU 执行完 Print 后滑入函数末尾的随机栈数据
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 