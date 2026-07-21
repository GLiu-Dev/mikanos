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
 
/**
 * OpenGOP - 查找并打开 GOP（Graphics Output Protocol）接口
 *
 * GOP（EFI_GRAPHICS_OUTPUT_PROTOCOL）是 UEFI 提供的标准图形输出协议，
 * 替代了旧式的 VGA/VESA/UGA 接口。它提供：
 *   - 像素级帧缓冲区（Frame Buffer）的物理地址和大小
 *   - 显示分辨率、像素格式、每扫描行像素数等显示参数
 *   - 模式切换（QueryMode / SetMode）
 *   - 硬件加速 Blt（Block Transfer）操作（本程序未使用）
 *
 * 本函数的工作流程：
 *   ① LocateHandleBuffer：在全局 Handle 数据库中搜索所有安装了 GOP 的句柄
 *   ② OpenProtocol：打开第一个找到的 GOP 句柄，取得协议接口指针
 *   ③ FreePool：释放 LocateHandleBuffer 动态分配的句柄数组
 *
 * 为什么需要 LocateHandleBuffer 而不能直接用 image_handle？
 *   GOP 安装在"显示控制器设备句柄"上，而非本程序的 image_handle 上。
 *   image_handle 只关联了引导程序自身的 EFI_LOADED_IMAGE_PROTOCOL，
 *   必须通过协议 GUID 全局搜索才能找到显示设备句柄。
 *
 * 参数：
 *   image_handle  本 UEFI 程序的句柄，作为 OpenProtocol 的"调用者句柄"传入
 *   gop           输出参数，成功后 *gop 指向 EFI_GRAPHICS_OUTPUT_PROTOCOL 接口
 *
 * 返回值：
 *   EFI_SUCCESS（内部调用的错误未检查，生产代码应逐步检查每步返回值）
 */
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
 
  /* ── 步骤 1：搜索所有安装了 GOP 的设备句柄 ──────────────────────────── */
 
  // num_gop_handles：LocateHandleBuffer 返回找到的句柄总数
  // gop_handles：    动态分配的 EFI_HANDLE 数组，由 LocateHandleBuffer 填充，
  //                  调用者负责用 FreePool 释放
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
 
  // gBS->LocateHandleBuffer 在固件的全局 Handle 数据库中搜索句柄，
  // 找到所有安装了指定协议的设备，并将匹配句柄的指针数组写入 gop_handles。
  //
  // 参数含义（共 5 个）：
  //
  //   1. ByProtocol（SearchType）
  //      搜索策略，枚举值之一：
  //        AllHandles   → 返回所有句柄（不过滤）
  //        ByRegisterNotify → 按注册通知过滤（高级用法）
  //        ByProtocol   → 仅返回安装了指定协议 GUID 的句柄  ← 本函数使用此项
  //
  //   2. &gEfiGraphicsOutputProtocolGuid
  //      要搜索的协议 GUID（仅在 ByProtocol 模式下使用）。
  //      gEfiGraphicsOutputProtocolGuid 由 EDK II 库预定义为
  //      {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}。
  //
  //   3. NULL（SearchKey）
  //      注册通知搜索时使用的 Key，ByProtocol 模式下传 NULL 即可。
  //
  //   4. &num_gop_handles（NoHandles）
  //      输出参数：返回找到的句柄数量。
  //      若系统有多块显卡，此值可能 > 1；若为 0 则表示没有可用的显示输出。
  //
  //   5. &gop_handles（Buffer）
  //      输出参数：固件在 Boot Services 内存池中分配一个 EFI_HANDLE 数组，
  //      并将指针写入此处。调用者必须在使用完毕后调用 FreePool 释放。
  //      若不释放，ExitBootServices 后该内存泄漏（虽然固件会统一回收，
  //      但在 Boot Services 阶段长期运行时会造成内存碎片）。
  gBS->LocateHandleBuffer(
      ByProtocol,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      &num_gop_handles,
      &gop_handles);
 
  /* ── 步骤 2：打开第一个 GOP 句柄，取得协议接口指针 ──────────────────── */
 
  // gop_handles[0]：取第一个找到的显示设备句柄。
  // 多显示器系统中 gop_handles 可能有多个元素；
  // 引导程序通常只需主显示器，故直接取下标 0。
  // 注意：若 num_gop_handles == 0，此处访问 gop_handles[0] 是未定义行为；
  // 生产代码应先检查 num_gop_handles > 0。
  //
  // 参数含义（与 OpenRootDir 中的 OpenProtocol 调用结构相同）：
  //
  //   1. gop_handles[0]                      → 要查询的目标句柄（显示设备）
  //   2. &gEfiGraphicsOutputProtocolGuid      → 要打开的协议 GUID
  //   3. (VOID**)gop                          → 输出：协议接口指针写入 *gop
  //   4. image_handle                         → 调用者句柄（本引导程序自身）
  //   5. NULL                                 → 控制器句柄（BY_HANDLE_PROTOCOL 不需要）
  //   6. EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL → 打开模式：读取已安装协议，不参与驱动绑定
  //
  // 成功后 *gop 即为 EFI_GRAPHICS_OUTPUT_PROTOCOL* 接口，
  // 可通过 (*gop)->Mode 访问当前显示模式信息：
  //   (*gop)->Mode->Info->HorizontalResolution  水平分辨率（像素）
  //   (*gop)->Mode->Info->VerticalResolution    垂直分辨率（像素）
  //   (*gop)->Mode->Info->PixelFormat           像素颜色格式（见 GetPixelFormatUnicode）
  //   (*gop)->Mode->Info->PixelsPerScanLine     每扫描行实际像素数（>= 水平分辨率，
  //                                             因为硬件可能有行末填充对齐）
  //   (*gop)->Mode->FrameBufferBase             帧缓冲区物理起始地址（可直接写入）
  //   (*gop)->Mode->FrameBufferSize             帧缓冲区总字节数
  gBS->OpenProtocol(
      gop_handles[0],
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
 
  /* ── 步骤 3：释放句柄数组 ────────────────────────────────────────────── */
 
  // LocateHandleBuffer 在 Boot Services 内存池中动态分配了 gop_handles 数组，
  // 必须用 FreePool（来自 MemoryAllocationLib）显式释放，对应 AllocatePool。
  // 释放后 gop_handles 指针变为悬空指针，但本函数随即返回，不会再使用它。
  // *gop（协议接口）已在步骤 2 中取得，不受 gop_handles 释放的影响。
  FreePool(gop_handles);
 
  return EFI_SUCCESS;
}
 
/**
 * GetPixelFormatUnicode - 将 EFI_GRAPHICS_PIXEL_FORMAT 枚举值转换为可读宽字符串
 *
 * EFI_GRAPHICS_PIXEL_FORMAT 定义像素在帧缓冲区内存中的颜色分量排列方式，
 * 直接决定了操作系统或引导程序向帧缓冲区写入像素时的字节顺序。
 *
 * 本函数仅用于调试输出（UefiMain 中的 Print 调用），
 * 返回值是指向只读数据段 .rodata 的字符串字面量指针，调用者不得修改或释放。
 *
 * 参数：
 *   fmt  EFI_GRAPHICS_PIXEL_FORMAT 枚举值，来自 gop->Mode->Info->PixelFormat
 *
 * 返回值：
 *   对应格式的 UTF-16 字符串；未知值返回 L"InvalidPixelFormat"
 */
const CHAR16* GetPixelFormatUnicode(EFI_GRAPHICS_PIXEL_FORMAT fmt) {
  switch (fmt) {
    // PixelRedGreenBlueReserved8BitPerColor（值 = 0）
    // 每像素 32 位，字节布局（低地址 → 高地址）：
    //   [R 8bit][G 8bit][B 8bit][Reserved 8bit]
    // 即内存中 byte0=Red, byte1=Green, byte2=Blue, byte3=填充（忽略）。
    // 在小端（x86-64）系统中，32 位整数值为 0x00BBGGRR 的逆序读取，
    // 实际写入时：pixel = (R) | (G << 8) | (B << 16)。
    // 这是 UEFI GOP 中最常见的格式，大多数现代显卡默认使用此格式。
    case PixelRedGreenBlueReserved8BitPerColor:
      return L"PixelRedGreenBlueReserved8BitPerColor";
 
    // PixelBlueGreenRedReserved8BitPerColor（值 = 1）
    // 每像素 32 位，字节布局（低地址 → 高地址）：
    //   [B 8bit][G 8bit][R 8bit][Reserved 8bit]
    // 即内存中 byte0=Blue, byte1=Green, byte2=Red, byte3=填充（忽略）。
    // 在小端系统中：pixel = (B) | (G << 8) | (R << 16)。
    // 等同于 Windows/Linux 图形中常见的 BGR 格式（也称 BGRA32）。
    // 注意：与上一种格式 R 和 B 通道互换，混淆会导致画面颜色错误。
    case PixelBlueGreenRedReserved8BitPerColor:
      return L"PixelBlueGreenRedReserved8BitPerColor";
 
    // PixelBitMask（值 = 2）
    // 自定义位掩码格式：R/G/B/Reserved 各占哪些 bit 由
    // gop->Mode->Info->PixelInformation（EFI_PIXEL_BITMASK 结构体）指定。
    // 该结构体包含 RedMask、GreenMask、BlueMask、ReservedMask 四个 UINT32，
    // 需要额外解析后才能正确写入像素。
    // 常见于某些旧式或嵌入式显示控制器，灵活性高但处理较繁琐。
    case PixelBitMask:
      return L"PixelBitMask";
 
    // PixelBltOnly（值 = 3）
    // 帧缓冲区不可直接内存访问（不存在可线性寻址的像素内存）。
    // 只能通过 gop->Blt() 方法（硬件 Block Transfer）来读写像素，
    // 无法用 FrameBufferBase 地址直接写入。
    // 内核不能假设此时 FrameBufferBase 可用，必须走 Blt API。
    // 极少见，主要出现在某些固件模拟环境或旧式显卡驱动中。
    case PixelBltOnly:
      return L"PixelBltOnly";
 
    // PixelFormatMax（值 = 4）
    // 枚举上界哨兵值，与 EfiMaxMemoryType 同理，不代表实际格式。
    // 用于边界检查循环（fmt < PixelFormatMax）；
    // 若在运行时遇到此值，通常意味着固件或驱动存在 bug。
    case PixelFormatMax:
      return L"PixelFormatMax";
 
    // 未知格式：固件私有扩展或内存损坏导致的非法值
    default:
      return L"InvalidPixelFormat";
  }
}
 
/**
 * UefiMain - UEFI 应用程序入口点（MikanOS 引导加载程序主函数）
 *
 * 本函数是整个引导流程的总指挥，按顺序完成以下六个阶段：
 *
 *   阶段 1  获取并保存物理内存映射  → 供内核掌握物理内存布局
 *   阶段 2  打开 ESP 文件系统根目录 → 后续文件读写的前提
 *   阶段 3  将内存映射写入 \memmap  → 调试用，内核可从文件读取
 *   阶段 4  初始化 GOP 帧缓冲区     → 清屏（全白）并获取显示参数
 *   阶段 5  加载内核 ELF 到物理内存 → 读取 \kernel.elf 到 0x100000
 *   阶段 6  退出 Boot Services      → 移交内存控制权给操作系统
 *   阶段 7  跳转到内核入口点        → 从此引导程序使命终结
 *
 * 函数签名约定：
 *   EFI_STATUS EFIAPI
 *     EFIAPI 宏指定 UEFI 调用约定（x86-64 上等同于 Microsoft ABI：
 *     前四个整数参数依次用 RCX/RDX/R8/R9 传递，调用者清栈）。
 *     与 Linux System V ABI 不同——混用会导致参数寄存器错位。
 *
 *   image_handle（EFI_HANDLE）
 *     UEFI 固件为本程序分配的不透明句柄（本质是指针，指向内部对象）。
 *     用途：① 查询 EFI_LOADED_IMAGE_PROTOCOL 得知程序所在设备
 *           ② 作为 OpenProtocol 的"调用者句柄"（AgentHandle）
 *           ③ 作为 ExitBootServices 的程序标识
 *
 *   system_table（EFI_SYSTEM_TABLE*）
 *     UEFI 系统表，是固件暴露所有服务的根入口：
 *       system_table->BootServices    → 引导期服务（内存/文件/协议等）
 *       system_table->RuntimeServices → 运行时服务（时钟/NVRAM/重启等）
 *       system_table->ConfigurationTable → 配置表数组（ACPI/SMBIOS 等）
 *     全局变量 gBS 在库初始化时已从 system_table->BootServices 提取，
 *     本函数直接使用 gBS 而无需再通过 system_table 间接访问。
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
 
  /* ── 阶段 0：确认引导程序已成功启动 ─────────────────────────────────── */
 
  // 向 UEFI 控制台（EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL，通常输出到屏幕）打印
  // 启动标志，用于最早期调试——若此行不出现，说明 UEFI 未能正常加载本程序。
  Print(L"Hello, Mikan World!\n");
 
  /* ── 阶段 1：获取物理内存映射 ────────────────────────────────────────── */
 
  // 在栈上分配 16 KiB（4096 * 4 字节）的缓冲区用于存储内存描述符数组。
  // 选择栈分配而非堆分配（AllocatePool）的原因：
  //   - 避免在 Boot Services 退出前出现内存泄漏
  //   - 缓冲区生命周期与 UefiMain 栈帧一致，无需手动释放
  //   - 16 KiB 在现代系统上足以容纳全部内存描述符（通常 < 100 条，每条约 48 字节）
  CHAR8 memmap_buf[4096 * 4];
 
  // 初始化 MemoryMap 结构体，字段含义：
  //   sizeof(memmap_buf) → buffer_size：告知 GetMemoryMap 缓冲区容量上限
  //   memmap_buf         → buffer：描述符数组存储位置
  //   后四个 0           → map_size / map_key / descriptor_size / descriptor_version
  //                        均为输出字段，调用后由固件填充
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
 
  // 调用封装函数获取当前物理内存快照。
  // 成功后最关键的输出字段：
  //   memmap.map_key        → ExitBootServices 的"快照票据"，必须在退出前使用
  //   memmap.map_size       → 有效描述符数据的字节总数
  //   memmap.descriptor_size → 单条描述符的实际大小（步进遍历时使用此值而非 sizeof）
  GetMemoryMap(&memmap);
 
  /* ── 阶段 2：打开 ESP 文件系统根目录 ────────────────────────────────── */
 
  // 通过 image_handle 三步定位根目录：
  //   image_handle → EFI_LOADED_IMAGE_PROTOCOL → DeviceHandle
  //   DeviceHandle → EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  //   OpenVolume() → EFI_FILE_PROTOCOL*（根目录句柄）
  // 引导程序自身与 kernel.elf 位于同一 ESP 分区，故直接使用自己的设备句柄。
  EFI_FILE_PROTOCOL* root_dir;
  OpenRootDir(image_handle, &root_dir);
 
  /* ── 阶段 3：将内存映射保存为 CSV 文件 ─────────────────────────────── */
 
  // 在 ESP 根目录下创建或覆盖 \memmap 文件。
  // 三个模式标志的组合含义：
  //   EFI_FILE_MODE_READ   → 允许读取（Write 也需要此标志）
  //   EFI_FILE_MODE_WRITE  → 允许写入
  //   EFI_FILE_MODE_CREATE → 文件不存在则创建，已存在则截断（覆盖）
  // 最后一个参数 0 表示文件属性为普通文件（非只读、非目录、非隐藏等）。
  EFI_FILE_PROTOCOL* memmap_file;
  root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
 
  // 将内存映射格式化为 CSV（字段：Index, Type, Type名称, 物理地址, 页数, 属性）并写入
  SaveMemoryMap(&memmap, memmap_file);
 
  // 关闭文件句柄，将文件系统缓冲区中的写入内容标记为完成。
  // 注意：UEFI FAT 驱动不一定在 Close 时立即刷盘，但句柄必须释放以避免资源泄漏。
  memmap_file->Close(memmap_file);
 
  /* ── 阶段 4：初始化 GOP 帧缓冲区（清屏并获取显示参数）──────────────── */
  // #@@range_begin(gop)
 
  // 通过 LocateHandleBuffer 全局搜索 EFI_GRAPHICS_OUTPUT_PROTOCOL，
  // 打开第一块显示设备的 GOP 接口。
  // GOP 提供像素级帧缓冲区的直接内存访问，是内核图形子系统的基础。
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  OpenGOP(image_handle, &gop);
 
  // 将显示参数打印到控制台，供调试确认显示模式：
  //   HorizontalResolution → 水平像素数（如 1920）
  //   VerticalResolution   → 垂直像素数（如 1080）
  //   PixelFormat          → 颜色分量排列（RGB/BGR/BitMask/BltOnly，影响写像素的字节顺序）
  //   PixelsPerScanLine    → 每扫描行的实际像素步长（可能 > 水平分辨率，因为硬件行末对齐）
  //                          计算行首地址时应乘以此值而非 HorizontalResolution，
  //                          否则在有行末填充的显卡上会出现画面斜切
  Print(L"Resolution: %ux%u, Pixel Format: %s, %u pixels/line\n",
      gop->Mode->Info->HorizontalResolution,
      gop->Mode->Info->VerticalResolution,
      GetPixelFormatUnicode(gop->Mode->Info->PixelFormat),
      gop->Mode->Info->PixelsPerScanLine);
 
  // 打印帧缓冲区的物理地址范围和总字节数。
  //   FrameBufferBase → 帧缓冲区物理起始地址（可直接通过指针写入像素）
  //   FrameBufferSize → 帧缓冲区总字节数
  //   结束地址 = FrameBufferBase + FrameBufferSize（不含该地址本身）
  // 这两个值在 ExitBootServices 后依然有效，内核可直接使用（MMIO 地址，非普通 RAM）。
  Print(L"Frame Buffer: 0x%0lx - 0x%0lx, Size: %lu bytes\n",
      gop->Mode->FrameBufferBase,
      gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize,
      gop->Mode->FrameBufferSize);
 
  // 将帧缓冲区每个字节写入 255（0xFF），实现全屏清白。
  //
  // 为什么全部写 255 能显示白色？
  //   对于 PixelRedGreenBlueReserved8BitPerColor 和
  //       PixelBlueGreenRedReserved8BitPerColor 格式，
  //   每像素 4 字节，R/G/B 各分量均为 0xFF 时即为最大亮度白色（255,255,255）。
  //   Reserved 字节也被写为 0xFF，但显示控制器会忽略该通道。
  //
  // 使用 UINT8* 逐字节写入而非按像素（UINT32）写入，
  // 是因为 FrameBufferSize 是字节数，且该写法对任意像素格式都安全
  // （PixelBitMask 下 0xFF 不一定是白色，但本程序假设使用标准 RGB/BGR 格式）。
  //
  // 此操作也验证了帧缓冲区物理地址可直接写入（即非 PixelBltOnly 格式）。
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }
  // #@@range_end(gop)
 
  /* ── 阶段 5：读取并加载内核 ELF 文件 ────────────────────────────────── */
 
  // 以只读模式打开 ESP 根目录下的 \kernel.elf
  EFI_FILE_PROTOCOL* kernel_file;
  root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
 
  // EFI_FILE_INFO 是可变长结构体：固定头部 + UTF-16 文件名字符串。
  // "kernel.elf" 为 10 字符，加终止符共 11 个 CHAR16，预留 12 个留有余量。
  // 缓冲区大小 = 固定部分 + 12 * sizeof(CHAR16)（每个 CHAR16 占 2 字节）
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
 
  // 通过 gEfiFileInfoGuid 查询文件元数据，填充 file_info_buffer。
  // GetInfo 是泛型接口，第二参数 GUID 决定返回的信息类型：
  //   gEfiFileInfoGuid     → EFI_FILE_INFO（大小、时间戳、文件名等）
  //   gEfiFileSystemInfoGuid → EFI_FILE_SYSTEM_INFO（卷标、剩余空间等）
  kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
 
  // 将缓冲区解释为 EFI_FILE_INFO* 并取出文件字节数。
  // EFI_FILE_INFO.FileSize 是文件的实际数据大小（不含文件系统元数据）。
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;
 
  // 在物理地址 0x100000（1 MiB）处申请连续物理页，用于存放内核镜像。
  //
  // 选择 1 MiB 作为加载基址的原因：
  //   - 0x000000～0x0FFFFF 是 x86 "低 1 MiB"遗留区域，包含：
  //       0x00000～0x003FF  实模式中断向量表（IVT）
  //       0x00400～0x004FF  BIOS 数据区（BDA）
  //       0x0A000～0x0BFFF  传统 VGA 显存映射
  //       0x0F000～0x0FFFF  BIOS ROM 影子区
  //     内核应避开这些区域，从 1 MiB 起始是安全约定。
  //   - 内核链接脚本的 LMA/VMA 从 0x100000 开始，与此处对齐一致。
  //
  // AllocateAddress：要求固件在"指定"物理地址分配，不让固件自由选择。
  // EfiLoaderData  ：标记为引导加载程序数据，内核启动后可视需求回收。
  // 页数计算：(size + 0xfff) / 0x1000 = 向上取整到 4 KiB 页边界。
  EFI_PHYSICAL_ADDRESS kernel_base_addr = 0x100000;
  gBS->AllocatePages(
      AllocateAddress, EfiLoaderData,
      (kernel_file_size + 0xfff) / 0x1000, &kernel_base_addr);
 
  // 将内核 ELF 文件内容整体读入刚分配的物理内存。
  // kernel_file_size 传入时为"请求读取字节数"，返回后更新为"实际读取字节数"。
  // 本实现将整个 ELF 文件（含 ELF 头、程序头表、各段数据）原样读入，
  // 未按 PT_LOAD segment 分别映射——依赖链接脚本保证平坦布局与加载地址一致。
  kernel_file->Read(kernel_file, &kernel_file_size, (VOID*)kernel_base_addr);
 
  // 打印内核加载地址与实际读取字节数，确认加载成功
  Print(L"Kernel: 0x%0lx (%lu bytes)\n", kernel_base_addr, kernel_file_size);

  /*
  用循环写 GOP FrameBuffer 全屏白色，只是把图形画面刷白；
  但 UEFI 的 Print() 走的是 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL（文本控制台，标准字符输出），
  它有独立的前景 / 背景文本颜色寄存器，二者互不干扰。

  1, 两套输出互相独立
    a. GOP：图形层，像素直接操作，用于内核图形、全屏画面；
    b. ConOut（SimpleTextOutput）：文本层，Print/PrintLib 全部走这个，独立字符调色板、光标、文本缓存。
  2, 手动写 FrameBuffer 只能改像素底色，不能修改文本控制台的字符颜色属性。
  3, UEFI 驱动（FS、BlockIO、DiskIo）执行文件 / 内存操作时，极大概率临时篡改 ConOut 文本属性，
    这是实验里文字变色的核心原因。
  4, ExitBootServices 之后 ConOut 彻底失效，不能再调用 Print，这也是为什么后面报错打印需要在退出前完成。
  */
 
  /* ── 阶段 6：退出 UEFI Boot Services ────────────────────────────────── */
  // #@@range_begin(exit_bs)
 
  // ExitBootServices 是引导流程中的"不归路"操作：
  //   - 调用成功后，所有 Boot Services（gBS->xxx）立即失效，不可再调用
  //   - 固件停止 Timer 事件、关闭驱动、回收 Boot Services 内存
  //   - 物理内存的控制权正式移交给操作系统
  //   - 第二参数 map_key 是握手凭证：固件验证内存映射自上次快照后未发生变化；
  //     若固件内部在两次调用之间修改了内存分配（如事件回调），map_key 会改变，
  //     调用将返回 EFI_INVALID_PARAMETER
  EFI_STATUS status;
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
 
  if (EFI_ERROR(status)) {
    // 首次失败说明 map_key 已过期（固件内部状态在调用间隙发生了变化）。
    // 唯一的补救措施是重新获取内存映射以刷新 map_key，然后立即重试。
    // 重要约束：GetMemoryMap 与第二次 ExitBootServices 之间不得有任何
    // 可能触发内存操作的调用（包括 Print），否则 map_key 再次失效。
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      // 若连内存映射都取不到，固件状态严重异常，只能挂死报错。
      // %r 是 EDK II Print 的扩展格式符，将 EFI_STATUS 格式化为可读字符串
      // （如 "Invalid Parameter"、"Out of Resources" 等）。
      Print(L"failed to get memory map: %r\n", status);
      while (1);  // 无限循环，防止 CPU 滑入非法地址执行垃圾指令
    }
 
    // 用刷新后的 map_key 立即重试，正常情况下此次必定成功
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      // 两次尝试均失败，无法继续引导，安全挂死
      Print(L"Could not exit boot service: %r\n", status);
      while (1);
    }
  }
 
  // ！！！从此处开始，gBS 指针已失效，以下任何 gBS->xxx() 均是未定义行为 ！！！
  // #@@range_end(exit_bs)
 
  /* ── 阶段 7：跳转到内核入口点 ───────────────────────────────────────── */
  // #@@range_begin(call_kernel)
 
  // 从 ELF64 文件头读取程序入口点虚拟地址（e_entry 字段）。
  //
  // ELF64 文件头（Elf64_Ehdr）各字段偏移（字节）：
  //   偏移  0 : e_ident[16]   魔数 "\x7fELF" + 类型/字节序/版本/OS ABI 等
  //   偏移 16 : e_type   (2B) 文件类型（ET_EXEC=2 可执行，ET_DYN=3 共享对象）
  //   偏移 18 : e_machine(2B) 目标架构（EM_X86_64=62）
  //   偏移 20 : e_version(4B) ELF 格式版本（固定为 1）
  //   偏移 24 : e_entry  (8B) 程序入口点虚拟地址  ← 此处读取
  //   偏移 32 : e_phoff  (8B) 程序头表（PHT）文件偏移
  //   ...
  //
  // 此处假设内核使用恒等映射（虚拟地址 == 物理地址），
  // 因此 e_entry 中的虚拟地址可直接作为物理地址调用，
  // 无需建立页表即可跳转。
  UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);
 
  // 将入口地址强制转换为无参数无返回值的函数指针并调用。
  //
  // typedef 的作用：
  //   EntryPointType 是函数类型（非指针），用于声明 entry_point 为"指向该类型函数的指针"。
  //   等价写法：void (*entry_point)(void) = (void (*)(void))entry_addr;
  //   使用 typedef 使意图更清晰。
  //
  // 调用后控制权永久移交内核，本函数后续代码不会再被执行。
  // 内核将接管 CPU，运行自己的初始化流程和事件循环。
  typedef void EntryPointType(void);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point();  // 跳入内核 ——— 引导程序使命至此完成
  // #@@range_end(call_kernel)
 
  /* ── 不可达代码：防止编译器警告与意外滑行 ───────────────────────────── */
 
  // 以下代码在正常流程中永远不会执行。
  // 保留的意义：
  //   1. 满足 C 编译器对"非 void 函数所有路径均需 return"的要求
  //   2. 若内核意外返回，Print 提供最后一条调试信息（Boot Services 已退出，
  //      实际上 Print 此时不可用，但至少不会立即崩溃在无意义的地址上）
  //   3. while(1) 防止 CPU 执行到函数末尾后滑入栈上的随机数据
  Print(L"All done\n");
 
  while (1);
  return EFI_SUCCESS;
}
 
 