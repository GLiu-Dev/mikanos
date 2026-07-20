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
  CHAR8 buf[256];
  UINTN len;
 
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);
  file->Write(file, &len, header);
 
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
    file->Write(file, &len, buf);
  }
 
  return EFI_SUCCESS;
}
 
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
 
  gBS->OpenProtocol(
      image_handle,
      &gEfiLoadedImageProtocolGuid,
      (VOID**)&loaded_image,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
 
  gBS->OpenProtocol(
      loaded_image->DeviceHandle,
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID**)&fs,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
 
  fs->OpenVolume(fs, root);
 
  return EFI_SUCCESS;
}
 
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
  gBS->LocateHandleBuffer(
      ByProtocol,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      &num_gop_handles,
      &gop_handles);
 
  gBS->OpenProtocol(
      gop_handles[0],
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
 
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
 * UefiMain - UEFI 应用程序入口点（MikanOS 引导加载程序主函数）
 *
 * 本函数是整个引导流程的总调度者，按顺序完成七个阶段后将控制权
 * 连同显示参数一起移交给内核：
 *
 *   阶段 1  获取物理内存映射          → 供内核掌握物理内存布局
 *   阶段 2  打开 ESP 文件系统根目录   → 后续所有文件操作的前提
 *   阶段 3  将内存映射写入 \memmap    → 调试用 CSV 文件
 *   阶段 4  初始化 GOP 帧缓冲区       → 获取显示参数并清屏（全白）
 *   阶段 5  加载内核 ELF 到物理内存   → 读取 \kernel.elf 到 0x100000
 *   阶段 6  退出 UEFI Boot Services   → 将内存控制权移交操作系统
 *   阶段 7  跳转内核并传递显示参数    → entry_point(FrameBufferBase, FrameBufferSize)
 *
 * 与上一版本的关键变化：
 *   内核入口点签名从 void(void) 升级为 void(UINT64, UINT64)，
 *   引导程序现在将 GOP 帧缓冲区的物理地址和大小作为参数直接传入内核，
 *   使内核从一启动就能访问显示输出，无需自行重新查询 GOP 协议。
 *
 * 函数签名约定：
 *   EFI_STATUS EFIAPI
 *     EFIAPI 指定 Microsoft x64 调用约定（前四个整数参数依次用
 *     RCX/RDX/R8/R9 传递），与 Linux System V ABI（RDI/RSI/...）不同。
 *     混用两种 ABI 会导致参数寄存器错位，引发难以调试的运行时错误。
 *
 *   image_handle（EFI_HANDLE）
 *     固件为本程序分配的不透明句柄，用途：
 *       ① 查询 EFI_LOADED_IMAGE_PROTOCOL → 得知本程序所在存储设备
 *       ② 作为 OpenProtocol 的 AgentHandle（调用者标识）
 *       ③ 作为 ExitBootServices 的程序标识
 *
 *   system_table（EFI_SYSTEM_TABLE*）
 *     UEFI 系统表根指针；全局变量 gBS 已在库初始化时提取自
 *     system_table->BootServices，本函数直接使用 gBS。
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
 
  /* ── 阶段 0：最早期启动确认 ──────────────────────────────────────────── */
 
  // 向 UEFI 控制台输出启动标志。若此行不出现，说明 UEFI 固件未能正确加载本程序。
  Print(L"Hello, Mikan World!\n");
 
  /* ── 阶段 1：获取物理内存映射快照 ───────────────────────────────────── */
 
  // 在栈上分配 16 KiB（4096 × 4 字节）缓冲区存储描述符数组。
  // 每条 EFI_MEMORY_DESCRIPTOR 约 48 字节，16 KiB 可容纳 ~340 条，
  // 足以覆盖现代 PC 的内存布局（通常 < 100 条）。
  // 选栈分配而非 AllocatePool：生命周期与函数帧一致，无需手动释放，
  // 也不会在 Boot Services 退出前引入额外的堆内存碎片。
  CHAR8 memmap_buf[4096 * 4];
 
  // 初始化封装结构体：
  //   buffer_size = sizeof(memmap_buf) → 缓冲区容量上限，传给固件
  //   buffer      = memmap_buf         → 描述符数组写入目标
  //   其余四个字段初始化为 0，调用后由固件填充：
  //     map_size         → 实际使用的字节数
  //     map_key          → 内存映射快照票据（ExitBootServices 必须使用此值）
  //     descriptor_size  → 单条描述符的实际字节数（含固件私有扩展字段，
  //                        遍历时必须用此值步进而非 sizeof(EFI_MEMORY_DESCRIPTOR)）
  //     descriptor_version → 描述符格式版本号（当前规范固定为 1）
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  GetMemoryMap(&memmap);
 
  /* ── 阶段 2：打开 ESP 文件系统根目录 ────────────────────────────────── */
 
  // 经由三层协议查询定位根目录：
  //   image_handle → EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle
  //   DeviceHandle → EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  //   OpenVolume() → EFI_FILE_PROTOCOL*（根目录句柄）
  EFI_FILE_PROTOCOL* root_dir;
  OpenRootDir(image_handle, &root_dir);
 
  /* ── 阶段 3：将内存映射保存为 CSV 文件 ──────────────────────────────── */
 
  // 在 ESP 根目录下创建或覆盖 \memmap 文件（CSV 格式，供调试或内核读取）。
  // 标志说明：
  //   EFI_FILE_MODE_READ   | EFI_FILE_MODE_WRITE → 读写权限
  //   EFI_FILE_MODE_CREATE → 文件不存在则创建，已存在则截断重写
  // 最后一个参数 0 = 普通文件属性（非只读 / 非隐藏 / 非目录）。
  EFI_FILE_PROTOCOL* memmap_file;
  root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
 
  // 将描述符数组格式化为 CSV（字段：Index, Type, Type名称,
  // PhysicalStart, NumberOfPages, Attribute）并逐行写入文件
  SaveMemoryMap(&memmap, memmap_file);
 
  // 关闭文件句柄，释放资源；数据已写入文件系统缓存
  memmap_file->Close(memmap_file);
 
  /* ── 阶段 4：获取 GOP 帧缓冲区并清屏 ────────────────────────────────── */
 
  // GOP（EFI_GRAPHICS_OUTPUT_PROTOCOL）是 UEFI 标准的图形输出接口，
  // 提供像素级帧缓冲区的直接内存访问。与旧式 VGA/VESA 相比，
  // GOP 在 ExitBootServices 后依然有效——其帧缓冲区是 MMIO 地址，
  // 不依赖 Boot Services，操作系统可直接继续使用。
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  OpenGOP(image_handle, &gop);
 
  // 打印显示参数到控制台，供调试确认：
  //   HorizontalResolution / VerticalResolution → 屏幕逻辑分辨率（像素）
  //   PixelFormat   → 颜色分量字节顺序（RGB/BGR/BitMask/BltOnly）
  //   PixelsPerScanLine → 每扫描行的物理步长，可能大于 HorizontalResolution
  //                       （硬件行末对齐填充），计算行首偏移必须乘以此值
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
 
  // 打印帧缓冲区物理地址范围：
  //   FrameBufferBase                           → 起始物理地址（包含）
  //   FrameBufferBase + FrameBufferSize         → 结束物理地址（不含）
  //   FrameBufferSize = PixelsPerScanLine × VerticalResolution × 每像素字节数
  // 这两个值在 ExitBootServices 后仍然有效，将直接传递给内核使用。
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  // 将帧缓冲区每字节写为 255（0xFF），实现全屏清白。
  // 对 RGB/BGR 格式：R=G=B=255 即为最大亮度白色，Reserved 字节被显示控制器忽略。
  // 清屏有两重意义：
  //   ① 消除固件启动画面，为内核提供干净的初始画面
  //   ② 验证 FrameBufferBase 地址可直接写入（若格式为 PixelBltOnly 则无效）
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
 
  /* ── 阶段 5：读取并加载内核 ELF 文件 ────────────────────────────────── */
 
  // 以只读模式打开 ESP 根目录下的 \kernel.elf
  EFI_FILE_PROTOCOL* kernel_file;
  root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
 
  // EFI_FILE_INFO 是可变长结构体（固定头 + UTF-16 文件名）。
  // "kernel.elf" = 10 字符 + 终止符，预留 12 个 CHAR16（每个 2 字节）留有余量。
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
 
  // 查询文件元数据，gEfiFileInfoGuid 指定返回 EFI_FILE_INFO 类型信息。
  // 调用后 file_info_buffer 中包含 FileSize、时间戳、文件名等字段。
  kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
 
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;  // 内核文件的实际字节数
 
  // 在物理地址 0x100000（1 MiB）申请连续物理页，用于存放内核镜像。
  //
  // 选择 1 MiB 的原因（避开 x86 低 1 MiB 遗留区域）：
  //   0x00000~0x003FF  实模式中断向量表（IVT）
  //   0x00400~0x004FF  BIOS 数据区（BDA）
  //   0x0A000~0x0BFFF  传统 VGA 显存映射
  //   0x0F000~0x0FFFF  BIOS ROM 影子区
  //   以上区域在 UEFI 模式下虽不再使用，但约定从 1 MiB 开始更安全，
  //   且与内核链接脚本中设定的 LMA/VMA 起始地址 0x100000 一致。
  //
  // AllocateAddress → 要求固件在精确的指定物理地址分配，不允许固件自由选择
  // EfiLoaderData   → 标记为引导程序数据，内核启动后可按需回收
  // 页数 = (size + 0xfff) / 0x1000：向上取整到 4 KiB 页边界
  EFI_PHYSICAL_ADDRESS kernel_base_addr = 0x100000;
  gBS->AllocatePages(
      AllocateAddress, EfiLoaderData,
      (kernel_file_size + 0xfff) / 0x1000, &kernel_base_addr);
 
  // 将整个 ELF 文件原样读入分配好的物理内存。
  // kernel_file_size 传入时为"期望读取字节数"，返回后更新为"实际读取字节数"。
  // 本实现未按 PT_LOAD segment 分段映射，直接原样加载，
  // 依赖链接脚本保证加载地址（LMA）与虚拟地址（VMA）相同且以 0x100000 为基址。
  kernel_file->Read(kernel_file, &kernel_file_size, (VOID*)kernel_base_addr);
  Print(L"Kernel: 0x%0lx (%lu bytes)\n", kernel_base_addr, kernel_file_size);
 
  /* ── 阶段 6：退出 UEFI Boot Services ────────────────────────────────── */
  // #@@range_begin(exit_bs)
 
  // ExitBootServices 是引导流程中的"不归路"操作。调用成功后：
  //   - 所有 Boot Services（gBS->xxx）立即永久失效
  //   - 固件停止定时器事件、关闭协议驱动、回收 Boot Services 内存
  //   - 物理内存控制权正式移交给操作系统
  //
  // 第二参数 memmap.map_key 是握手凭证（快照票据）：
  //   固件验证内存映射自上次 GetMemoryMap 快照后未发生任何变化；
  //   若固件内部（如事件回调）在两次调用之间修改了内存分配，
  //   map_key 会改变，此调用将返回 EFI_INVALID_PARAMETER。
  EFI_STATUS status;
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
 
  if (EFI_ERROR(status)) {
    // 首次失败：map_key 已过期，必须立即重新获取内存映射刷新 map_key。
    // 重要约束：GetMemoryMap 与下一次 ExitBootServices 之间
    // 绝对不能有任何可能触发内存操作的调用（包括 Print、文件 I/O 等），
    // 否则 map_key 会在间隙再次失效，陷入无法退出的死循环。
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      // 连内存映射都取不到，固件状态严重异常，安全挂死。
      // %r 是 EDK II Print 的扩展格式符，将 EFI_STATUS 格式化为可读字符串。
      Print(L"failed to get memory map: %r\n", status);
      while (1);  // 无限循环，防止 CPU 滑入非法地址执行随机指令
    }
 
    // 用刷新后的 map_key 立即重试，正常情况下必定成功
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      while (1);
    }
  }
  // ！！！从此处起 gBS 指针永久失效，任何 gBS->xxx() 均为未定义行为 ！！！
  // #@@range_end(exit_bs)
 
  /* ── 阶段 7：读取内核入口地址并跳转，传递显示参数 ───────────────────── */
 
  // 从 ELF64 文件头偏移 24 字节处读取 e_entry 字段（程序入口点虚拟地址）。
  //
  // ELF64 文件头（Elf64_Ehdr）字段偏移速查：
  //   偏移  0 (16B): e_ident       魔数 "\x7fELF" + 类/字节序/版本/OS ABI
  //   偏移 16 ( 2B): e_type        文件类型（ET_EXEC=2）
  //   偏移 18 ( 2B): e_machine     目标架构（EM_X86_64=62）
  //   偏移 20 ( 4B): e_version     ELF 版本（固定为 1）
  //   偏移 24 ( 8B): e_entry  ←── 程序入口点虚拟地址（此处读取）
  //   偏移 32 ( 8B): e_phoff       程序头表文件偏移
  //   ...
  //
  // 前提假设：内核使用恒等映射（虚拟地址 == 物理地址），
  // 因此 e_entry 中的虚拟地址可直接作为物理地址调用，无需先建立页表。
  UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);
 
  // #@@range_begin(call_kernel)
 
  // 将入口地址转换为接受两个 UINT64 参数的函数指针并调用。
  //
  // 与上一版本的核心变化：
  //   旧版：typedef void EntryPointType(void)
  //         entry_point()               // 内核无法获知帧缓冲区信息
  //   新版：typedef void EntryPointType(UINT64, UINT64)
  //         entry_point(FrameBufferBase, FrameBufferSize)  // 直接传递显示参数
  //
  // 这一变化使内核从第一条指令起就能访问显示输出，无需再查询 GOP 协议
  // （Boot Services 退出后 GOP 协议接口已无法通过标准 UEFI API 重新获取，
  //  但帧缓冲区的物理地址本身依然有效——它是 MMIO 地址，不依赖 Boot Services）。
  //
  // 调用约定（System V AMD64 ABI，内核采用此 ABI）：
  //   第一个参数 FrameBufferBase → RDI 寄存器
  //   第二个参数 FrameBufferSize → RSI 寄存器
  //   内核的 KernelMain(uint64_t frame_buffer_base, uint64_t frame_buffer_size)
  //   按同样约定接收这两个参数。
  //
  // 调用后控制权永久移交内核，本函数后续代码永远不会再被执行。
  typedef void EntryPointType(UINT64, UINT64);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize);
  // #@@range_end(call_kernel)
 
  /* ── 不可达代码：防止编译器警告与意外滑行 ───────────────────────────── */
 
  // 以下代码在正常流程中永远不会执行。
  // 保留原因：
  //   1. 满足 C 编译器对"所有路径均须有 return"的要求（函数签名返回 EFI_STATUS）
  //   2. 若内核意外返回，Print 提供最后一条调试线索
  //      （Boot Services 已退出，Print 实际不可用，但至少不会立即在野指针上崩溃）
  //   3. while(1) 防止 CPU 执行完 Print 后滑入函数末尾的随机栈数据
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 
 