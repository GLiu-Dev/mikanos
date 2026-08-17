#include  <Uefi.h>
// EDK2 基础头文件，定义EFI_STATUS、EFI_HANDLE、各种UEFI基础类型
#include  <Library/UefiLib.h>
// UEFI工具库，提供Print()等基础函数
#include  <Library/UefiBootServicesTableLib.h>
// 提供全局变量 gBS (EFI_BOOT_SERVICES*, UEFI启动服务表)
#include  <Library/PrintLib.h>
// 字符串格式化 AsciiSPrint、Unicode字符串工具
#include  <Library/MemoryAllocationLib.h>
// UEFI内存分配函数：AllocatePool / AllocatePages / FreePool
#include  <Library/BaseMemoryLib.h>
// 内存拷贝CopyMem、内存置零SetMem
#include  <Protocol/LoadedImage.h>
// EFI_LOADED_IMAGE_PROTOCOL：获取当前镜像所在设备句柄
#include  <Protocol/SimpleFileSystem.h>
// 简单文件系统协议，读写ESP分区文件
#include  <Protocol/DiskIo2.h>
// 磁盘IO协议（本代码未使用）
#include  <Protocol/BlockIo.h>
// 块设备协议（本代码未使用）
#include  <Guid/FileInfo.h>
// 文件信息GUID，用于获取文件大小
#include  "frame_buffer_config.hpp"
// 自定义结构体：帧缓冲区配置，传递给内核
// #@@range_begin(include_map_header)
#include  "memory_map.hpp"
// 你之前分析的内存映射结构体，用于在内核和引导程序之间传递内存表
// #@@range_end(include_map_header)
#include  "elf.hpp"
// ELF64 程序头、文件头结构体定义，解析kernel.elf

/**
 * @brief 获取UEFI内存映射，填充自定义MemoryMap结构体
 * @param map 出入参：存放内存映射缓冲区信息
 * @return EFI_SUCCESS 成功
 * 包装 gBS->GetMemoryMap UEFI标准调用
 */
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
  map->map_size = map->buffer_size;
  // UEFI标准API：获取整机内存描述符数组
  return gBS->GetMemoryMap(
      &map->map_size,               // [IN/OUT]缓冲区有效字节长度
      (EFI_MEMORY_DESCRIPTOR*)map->buffer, // 输出缓冲区
      &map->map_key,                // 内存映射密钥！ExitBootServices必需
      &map->descriptor_size,        // UEFI返回单个描述符大小（重点！不能sizeof）
      &map->descriptor_version);    // 描述符版本号
}

/**
 * @brief 将EFI内存类型枚举转为Unicode字符串（用于日志打印）
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
 * @brief 将内存映射信息导出写入文件 \memmap（方便调试）
 * @param map 内存映射数据
 * @param file 已打开文件句柄
 */
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  EFI_STATUS status;
  CHAR8 buf[256];
  UINTN len;
  // CSV表头
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);
  status = file->Write(file, &len, header);
  if (EFI_ERROR(status)) {
    return status;
  }
  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);
  // 遍历所有内存描述符，使用descriptor_size步进（UEFI规范强制要求）
  EFI_PHYSICAL_ADDRESS iter;
  int i;
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;
    // 格式化一行CSV文本
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
 * @brief 打开当前启动镜像所在分区的根目录
 * @param image_handle 当前UEFI程序镜像句柄
 * @param root 输出：根目录文件协议指针
 */
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;

  // 1. 获取LoadedImage协议：拿到镜像信息
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
  // 2. 在镜像所在设备上打开SimpleFileSystem协议
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
  // 3. 打开卷，得到根目录
  return fs->OpenVolume(fs, root);
}

/**
 * @brief 定位并打开 GOP（Graphics Output Protocol）显卡协议
 * GOP = UEFI标准帧缓冲区接口，获取屏幕分辨率、显存物理地址
 */
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
  EFI_STATUS status;
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
  // 查找所有支持GOP协议的硬件句柄
  status = gBS->LocateHandleBuffer(
      ByProtocol,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      &num_gop_handles,
      &gop_handles);
  if (EFI_ERROR(status)) {
    return status;
  }
  // 打开第一个GOP设备
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
  FreePool(gop_handles); // 释放句柄数组
  return EFI_SUCCESS;
}

/**
 * @brief 将GOP像素格式枚举转为字符串，调试打印
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
 * @brief 停机函数：无限hlt，出错时调用
 */
void Halt(void) {
  while (1) __asm__("hlt");
}

/**
 * @brief 扫描ELF Program Header，计算所有PT_LOAD段的最小虚拟地址、最大虚拟地址
 * 作用：确定内核需要占用的虚拟地址区间，提前分配物理内存页
 * @param ehdr ELF文件头
 * @param first [OUT] 最低虚拟地址
 * @param last [OUT] 最高虚拟地址
 */
void CalcLoadAddressRange(Elf64_Ehdr* ehdr, UINT64* first, UINT64* last) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  *first = MAX_UINT64;
  *last = 0;
  // 遍历所有程序段
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue; // 只关心需要加载到内存的段
    *first = MIN(*first, phdr[i].p_vaddr);
    *last = MAX(*last, phdr[i].p_vaddr + phdr[i].p_memsz);
  }
}

/**
 * @brief ELF加载核心函数：拷贝PT_LOAD段到目标虚拟地址
 * 1. 将文件内数据复制到p_vaddr
 * 2. 文件大小之外的内存区域清零（bss段）
 */
void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    // 段在ELF文件中的偏移地址
    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    // 拷贝文件内有效数据
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz);
    // bss区域：文件中不存在，内存需要填充0
    UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;
    SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0);
  }
}

/**
 * UEFI应用程序入口函数（EDK II标准入口）
 * @param image_handle 当前镜像句柄
 * @param system_table UEFI系统表指针
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
  EFI_STATUS status;
  Print(L"Hello, Mikan World!\n");

  // 4页大小缓冲区，用来存放内存描述符数组
  CHAR8 memmap_buf[4096 * 4];
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  status = GetMemoryMap(&memmap);
  if (EFI_ERROR(status)) {
    Print(L"failed to get memory map: %r\n", status);
    Halt();
  }

  // 打开ESP分区根目录
  EFI_FILE_PROTOCOL* root_dir;
  status = OpenRootDir(image_handle, &root_dir);
  if (EFI_ERROR(status)) {
    Print(L"failed to open root directory: %r\n", status);
    Halt();
  }

  // 尝试创建\memmap文件，导出内存映射CSV（调试用，失败不崩溃）
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

  // 打开GOP显卡协议，获取帧缓冲区信息
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

  // 简单测试：把整个显存填充白色
  UINT8* frame_buffer = (UINT8*)gop->Mode->FrameBufferBase;
  for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i) {
    frame_buffer[i] = 255;
  }

  // 打开内核文件 kernel.elf
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"failed to open file '\\kernel.elf': %r\n", status);
    Halt();
  }

  // 获取kernel.elf文件大小
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

  // 在UEFI堆上分配缓冲区读取整个ELF文件
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

  Elf64_Ehdr* kernel_ehdr = (Elf64_Ehdr*)kernel_buffer;
  UINT64 kernel_first_addr, kernel_last_addr;
  // 计算内核所有加载段虚拟地址范围
  CalcLoadAddressRange(kernel_ehdr, &kernel_first_addr, &kernel_last_addr);

  // 向上取整，计算需要多少个4KB页面
  UINTN num_pages = (kernel_last_addr - kernel_first_addr + 0xfff) / 0x1000;
  // AllocateAddress：强制分配【指定虚拟地址对应的物理内存】
  // 关键点：内核ELF使用非0起始虚拟地址，必须让物理内存映射到对应VA
  status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                              num_pages, &kernel_first_addr);
  if (EFI_ERROR(status)) {
    Print(L"failed to allocate pages: %r\n", status);
    Halt();
  }

  // 将ELF中的段拷贝到目标虚拟地址，填充bss
  CopyLoadSegments(kernel_ehdr);
  Print(L"Kernel: 0x%0lx - 0x%0lx\n", kernel_first_addr, kernel_last_addr);

  // ELF文件读取完成，释放临时读取缓冲区
  status = gBS->FreePool(kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"failed to free pool: %r\n", status);
    Halt();
  }

  // 【至关重要】退出UEFI BootServices
  // 一旦成功调用，绝大多数UEFI服务、协议全部失效！
  // 注意：ExitBootServices可能失败，失败时必须重新获取memory map再重试
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
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

  // ELF64入口地址：e_entry 在文件偏移24字节处
  UINT64 entry_addr = *(UINT64*)(kernel_first_addr + 24);

  // 构造帧缓冲区配置，传递给内核
  struct FrameBufferConfig config = {
    (UINT8*)gop->Mode->FrameBufferBase,
    gop->Mode->Info->PixelsPerScanLine,
    gop->Mode->Info->HorizontalResolution,
    gop->Mode->Info->VerticalResolution,
    0
  };
  // 转换UEFI像素格式到内核枚举
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

  // #@@range_begin(pass_memory_map)
  // 定义内核入口函数指针类型，和main.cpp KernelMain 签名一致
  typedef void EntryPointType(const struct FrameBufferConfig*,
                              const struct MemoryMap*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  // 跳转内核！从此引导程序代码不再执行
  entry_point(&config, &memmap);
  // #@@range_end(pass_memory_map)

  // 内核正常情况下不会返回，走到这里说明内核异常返回
  Print(L"All done\n");
  while (1);
  return EFI_SUCCESS;
}