/**
 * ============================================================================
 *  MikanOS - Day 02b : 读取内存映射并保存到文件 \memmap
 * ============================================================================
 *  这是运行在 UEFI 环境下的引导程序（UEFI Application / .efi）。
 *  它没有传统的 main() 入口，真正的入口是 UefiMain()。
 *  目标：在启动早期把"内存布局"（哪些内存可用、哪些保留、哪些是设备映射等）
 *        获取出来，整理成文本/CSV 写入引导分区根目录下的 \memmap 文件。
 *        因为后续内核必须知道内存布局才能正确管理内存。
 *
 *  UEFI 的关键概念（先理解这些，代码就顺了）：
 *   - gBS : Boot Services 表（全局指针），提供 GetMemoryMap、OpenProtocol 等服务，
 *           这些服务只在"启动阶段"可用，交出控制权给 OS 后即失效。
 *   - gST : System Table，可访问控制台输出等。
 *   - Protocol : UEFI 的"接口"机制。硬件/文件系统通过 Protocol 暴露能力。
 *   - EFI_STATUS : 所有 UEFI 函数统一的返回码，EFI_SUCCESS = 0。
 * ============================================================================
 */
#include  <Uefi.h>                                        // UEFI 基础类型、EFI_STATUS、EFI_HANDLE 等
#include  <Library/UefiLib.h>                             // UefiMain 相关、Print 等辅助库
#include  <Library/UefiBootServicesTableLib.h>            // 提供全局 gBS（Boot Services 表）
#include  <Library/PrintLib.h>                            // AsciiSPrint / AsciiStrLen 等格式化函数
#include  <Protocol/LoadedImage.h>                        // 描述"本镜像被加载"的信息（含启动设备）
#include  <Protocol/SimpleFileSystem.h>                   // 文件系统抽象接口，用来开卷（卷=一个分区）
#include  <Protocol/DiskIo2.h>                            // 磁盘块读写（本文件未直接用，保留作头文件依赖）
#include  <Protocol/BlockIo.h>                            // 块设备接口（本文件未直接用，保留作头文件依赖）

// ============================================================================
//  #@@range_begin/end 是 MikanOS 教材用来"从源码中提取代码片段"的标记，
//  与程序逻辑无关，可忽略。后面所有这类标记同理。
// ============================================================================

// #@@range_begin(struct_memory_map)
/**
 * struct MemoryMap
 * -----------------
 * 内存映射的"容器"。因为 EFI 的内存映射是"先问大小、再填数据"的两步式，
 * 所以需要一个结构把缓冲区和相关元数据包在一起，方便在函数间传递。
 *
 *   buffer_size         : buffer 指向的缓冲区总容量（字节）。
 *                         调用方先告诉 UEFI "我准备了多大的空间"。
 *   buffer              : 指向用于存放内存映射描述符数组的内存。
 *   map_size            : 实际写入的字节数（= 描述符总大小）。既是输入也是输出。
 *   map_key             : "内存映射钥匙"。只要内存布局发生变动，key 就变化。
 *                         可用来检测"拿到映射后内存是否又变了"。
 *   descriptor_size     : 单个 EFI_MEMORY_DESCRIPTOR 结构体的字节大小。
 *                         【重要】不能用 sizeof(EFI_MEMORY_DESCRIPTOR) 想当然，
 *                         因为不同平台对齐/版本不同，必须用 UEFI 返回的这个值。
 *   descriptor_version  : 描述符结构的版本号，通常忽略。
 */
struct MemoryMap {
  UINTN buffer_size;        // 缓冲区容量（字节）
  VOID* buffer;             // 指向描述符缓冲区的首地址
  UINTN map_size;           // 实际使用的字节数（输入=容量，输出=实际）
  UINTN map_key;            // 内存映射标识 key，用于检测映射是否变化
  UINTN descriptor_size;    // 单个描述符的大小（迭代时按它步进）
  UINT32 descriptor_version;// 描述符版本
};
// #@@range_end(struct_memory_map)

// #@@range_begin(get_memory_map)
/**
 * GetMemoryMap
 * ------------
 * 封装 gBS->GetMemoryMap()，把"两步式"请求的细节包起来。
 * 返回后，map->buffer 里就是填满的内存映射描述符数组。
 *
 * 流程说明：
 *   1. 如果 buffer 为空，说明调用方没准备好空间，返回 EFI_BUFFER_TOO_SMALL。
 *   2. 把 map->map_size 先设为"我准备了多少空间"（即 buffer_size），
 *      这是传给 UEFI 的"我能装多少"。
 *   3. 调用 gBS->GetMemoryMap：
 *        - 成功：map_size 被改写为实际使用字节数，buffer 被填满。
 *        - 空间不足：返回 EFI_BUFFER_TOO_SMALL，map_size 变成"最少需要多少"，
 *                    调用方据此重新分配后再试。
 *
 * @param map  指向 MemoryMap 结构体，缓冲区必须已分配。
 * @return EFI_STATUS，EFI_SUCCESS 表示成功。
 */
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
  if (map->buffer == NULL) {            // 没有可用缓冲区，直接失败
    return EFI_BUFFER_TOO_SMALL;
  }

  map->map_size = map->buffer_size;     // 输入：告知 UEFI 我们准备了多大空间

  // 真正发起系统调用，向 Boot Services 索取内存映射
  return gBS->GetMemoryMap(
      &map->map_size,                   // [in/out] 输入=容量，输出=实际字节数
      (EFI_MEMORY_DESCRIPTOR*)map->buffer, // [out] 存放描述符数组的缓冲区
      &map->map_key,                    // [out] 内存映射 key
      &map->descriptor_size,            // [out] 单个描述符大小
      &map->descriptor_version);        // [out] 版本
}
// #@@range_end(get_memory_map)

// #@@range_begin(get_memory_type)
/**
 * GetMemoryTypeUnicode
 * --------------------
 * 把 EFI_MEMORY_TYPE 枚举值（数字）翻译成人类可读的字符串。
 * UEFI 内存类型是一个 32 位枚举，每个取值代表一种内存用途，
 * 例如"可用内存"、"ACPI 回收内存"、"内存映射 I/O"等。
 * 这里用 switch 一一对应，方便最后写文件时人能看懂。
 *
 * 常见类型含义（EfiConventionalMemory 最重要，是可用的普通内存）：
 *   - EfiConventionalMemory  : 常规可用内存，OS 可以自由使用。
 *   - EfiLoaderCode/Data     : 给 EFI 加载器（即引导程序本身）用的代码/数据区。
 *   - EfiBootServicesCode/Data : 引导服务阶段的代码/数据，交权后会被回收。
 *   - EfiRuntimeServicesCode/Data : 运行时服务，交权后仍保留（供 OS 调用）。
 *   - EfiACPIReclaimMemory   : ACPI 表存放区，OS 读完后可以回收。
 *   - EfiACPIMemoryNVS       : ACPI 非易失性存储，不可回收。
 *   - EfiMemoryMappedIO      : 内存映射的 I/O 设备空间，不可当普通内存用。
 *   - EfiUnusableMemory      : 有坏块等原因不可用的内存。
 *
 * @param type EFI 内存类型枚举。
 * @return 对应的宽字符串（L"..."），无法识别时返回 L"InvalidMemoryType"。
 */
const CHAR16* GetMemoryTypeUnicode(EFI_MEMORY_TYPE type) {
  switch (type) {
    case EfiReservedMemoryType:   return L"EfiReservedMemoryType";
    case EfiLoaderCode:           return L"EfiLoaderCode";
    case EfiLoaderData:           return L"EfiLoaderData";
    case EfiBootServicesCode:     return L"EfiBootServicesCode";
    case EfiBootServicesData:     return L"EfiBootServicesData";
    case EfiRuntimeServicesCode:  return L"EfiRuntimeServicesCode";
    case EfiRuntimeServicesData:  return L"EfiRuntimeServicesData";
    case EfiConventionalMemory:   return L"EfiConventionalMemory";
    case EfiUnusableMemory:       return L"EfiUnusableMemory";
    case EfiACPIReclaimMemory:    return L"EfiACPIReclaimMemory";
    case EfiACPIMemoryNVS:        return L"EfiACPIMemoryNVS";
    case EfiMemoryMappedIO:       return L"EfiMemoryMappedIO";
    case EfiMemoryMappedIOPortSpace: return L"EfiMemoryMappedIOPortSpace";
    case EfiPalCode:              return L"EfiPalCode";
    case EfiPersistentMemory:     return L"EfiPersistentMemory";
    case EfiMaxMemoryType:        return L"EfiMaxMemoryType";
    default:                      return L"InvalidMemoryType";
  }
}
// #@@range_end(get_memory_type)

// #@@range_begin(save_memory_map)
/**
 * SaveMemoryMap
 * -------------
 * 把内存映射描述符数组逐条格式化为 CSV 文本，写入已打开的 \memmap 文件。
 * 每行一条内存区间，字段为：
 *   Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute
 *
 * @param map   已填充好内存映射的 MemoryMap。
 * @param file  已打开的 EFI_FILE_PROTOCOL（指向根目录下的 \memmap 文件）。
 * @return EFI_SUCCESS（本函数固定成功返回）。
 */
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  CHAR8 buf[256];                    // 一行文本的临时缓冲区（单行不超过它）
  UINTN len;                         // 每次要写入的字节数
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n"; // CSV 表头
  len = AsciiStrLen(header);         // 算出表头长度
  file->Write(file, &len, header);   // 先写表头到文件

  // 调试打印：确认拿到的缓冲区和实际大小
  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);

  EFI_PHYSICAL_ADDRESS iter;  // 用字节地址做游标，遍历描述符数组
  int i;                      // 行号（描述符下标）

  // 遍历所有描述符：
  //   - 起点  = buffer 首地址
  //   - 终点  = buffer 首地址 + 实际大小（map_size）
  //   - 步长  = descriptor_size（UEFI 返回的单个描述符大小，不能想当然用 sizeof）
  //   【关键】因为描述符是"紧挨着"排列的，用步长递进就能依次拿到每条。
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {

    // 把当前字节地址强转为 EFI_MEMORY_DESCRIPTOR 指针，即取出一条描述符
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;

    // 把这一条格式化成一行 CSV（AsciiSPrint 是安全格式化，有长度上限）
    len = AsciiSPrint(
        buf, sizeof(buf),                     // 输出到 buf，最多 sizeof(buf) 字节
        "%u, %x, %-ls, %08lx, %lx, %lx\n",    // 格式化串（见下方说明）
        i,                                    // %u   : 行号
        desc->Type,                           // %x   : 内存类型（十六进制数）
        GetMemoryTypeUnicode(desc->Type),     // %-ls : 内存类型名字符串（宽字符左对齐）
        desc->PhysicalStart,                  // %08lx: 起始物理地址（8位十六进制）
        desc->NumberOfPages,                  // %lx  : 页数（每页 4KB）
        desc->Attribute & 0xffffflu);         // %lx  : 属性，只取低 20 位
    // 说明：Attribute 是 64 位位图；0xffffflu 是十六进制数 0xfffff（长整型无符号），
    //       即低 20 位全 1 的掩码，用它把高位属性位截掉，只保留常用低 20 位。

    file->Write(file, &len, buf);             // 把这一行写入文件
  }
  return EFI_SUCCESS;
}
// #@@range_end(save_memory_map)

/**
 * OpenRootDir
 * -----------
 * 拿到"本引导程序所在启动设备"的文件系统根目录。
 *
 * 步骤（UEFI Protocol 的典型使用链）：
 *   1. 打开 LoadedImage Protocol：得到本镜像被加载的信息，
 *      其中 loaded_image->DeviceHandle 就是"加载本镜像的设备"（一般是引导分区）。
 *   2. 在该设备上打开 SimpleFileSystem Protocol：只有分区上才有文件系统接口。
 *   3. 调用 fs->OpenVolume() 打开"卷"，拿到该分区的根目录 EFI_FILE_PROTOCOL。
 *
 * 之后就可以用 root 目录句柄去 Open 里面的具体文件了。
 *
 * @param image_handle  UEFI 传给 UefiMain 的本镜像句柄。
 * @param root          [out] 输出的根目录 EFI_FILE_PROTOCOL*。
 * @return EFI_SUCCESS。
 */
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;  // 本镜像的加载信息
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;      // 文件系统接口

  // --- 第 1 步：拿 LoadedImage ---
  gBS->OpenProtocol(
      image_handle,                                    // 要查询的句柄（本镜像）
      &gEfiLoadedImageProtocolGuid,                    // 要打开的 Protocol 的 GUID
      (VOID**)&loaded_image,                           // [out] 拿到的接口指针
      image_handle,                                    // 请求者句柄（自己）
      NULL,                                            // 关联的 AgentHandle，无则 NULL
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);           // 打开方式：按句柄直接取接口

  // --- 第 2 步：在启动设备上拿文件系统 ---
  gBS->OpenProtocol(
      loaded_image->DeviceHandle,                      // 本镜像所在设备
      &gEfiSimpleFileSystemProtocolGuid,               // 文件系统接口 GUID
      (VOID**)&fs,                                     // [out] 文件系统接口
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

  // --- 第 3 步：打开卷，得到根目录 ---
  fs->OpenVolume(fs, root);                            // root 即根目录句柄
  return EFI_SUCCESS;
}

/**
 * UefiMain
 * --------
 * UEFI 应用程序的真正入口（由 UEFI 固件调用）。
 * 整体流程：打印问候 → 取内存映射 → 打开根目录 → 写入 \memmap 文件 → 死循环。
 *
 * @param image_handle  本镜像在 UEFI 中的句柄。
 * @param system_table  系统表，可访问 gST->ConOut 控制台等（本文件未直接用）。
 * @return 正常不会返回（后面有 while(1) 死循环）。
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
  Print(L"Hello, Mikan World!\n");   // 控制台输出问候语（UEFI 下的 printf）

  // #@@range_begin(main)
  // 在栈上分配一块 16KB 的内存映射缓冲区（4096 * 4 = 16384 字节）
  CHAR8 memmap_buf[4096 * 4];

  // 初始化 MemoryMap：容量=16384，buffer=memmap_buf，其余 5 个字段先置 0
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};

  // 1) 调用 Boot Services 获取内存映射，结果填进 memmap.buffer
  GetMemoryMap(&memmap);

  // 2) 打开引导分区的根目录（返回 root_dir 目录句柄）
  EFI_FILE_PROTOCOL* root_dir;
  OpenRootDir(image_handle, &root_dir);

  // 3) 在根目录下创建/打开文件 \memmap
  //    打开模式 = 读 | 写 | 创建（不存在则新建）
  EFI_FILE_PROTOCOL* memmap_file;
  root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",                      // 注意：路径用宽字符串 L""
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);

  // 4) 把内存映射逐条格式化写入文件
  SaveMemoryMap(&memmap, memmap_file);

  // 5) 关闭文件句柄（确保数据落盘、释放资源）
  memmap_file->Close(memmap_file);
  // #@@range_end(main)

  Print(L"All done\n");

  // UEFI 应用一返回，控制权会交回 UEFI Shell/固件。
  // 这里用死循环"挂住"，不让程序退出——后续章节会在这里接管、
  // 关闭 Boot Services 并跳入我们自己的内核。
  while (1);

  return EFI_SUCCESS;  // 实际上到不了这里（被 while(1) 拦住）
}



/*
这段代码是 MikanOS 第 2 天的引导程序（UEFI Application），
核心任务就一件事：**把 UEFI 提供的 "内存布局表" 取出来，整理成文本写进引导分区根目录的 `\memmap` 文件**，
为后续内核管理内存做准备。

gliu@ubuntu:~/edk2$ cat mnt/memmap 
Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute
0, 3, EfiBootServicesCode, 00000000, 1, F
1, 7, EfiConventionalMemory, 00001000, 9F, F
2, 7, EfiConventionalMemory, 00100000, 700, F
3, A, EfiACPIMemoryNVS, 00800000, 8, F
4, 7, EfiConventionalMemory, 00808000, 8, F
5, A, EfiACPIMemoryNVS, 00810000, F0, F
6, 4, EfiBootServicesData, 00900000, B00, F
7, 7, EfiConventionalMemory, 01400000, 3AB36, F
8, 4, EfiBootServicesData, 3BF36000, 20, F
9, 7, EfiConventionalMemory, 3BF56000, 270F, F
10, 1, EfiLoaderCode, 3E665000, 2, F
11, 4, EfiBootServicesData, 3E667000, 217, F
12, 3, EfiBootServicesCode, 3E87E000, B6, F
13, A, EfiACPIMemoryNVS, 3E934000, 12, F
14, 0, EfiReservedMemoryType, 3E946000, 1C, F
15, 3, EfiBootServicesCode, 3E962000, 10A, F
16, 6, EfiRuntimeServicesData, 3EA6C000, 5, F
17, 5, EfiRuntimeServicesCode, 3EA71000, 5, F
18, 6, EfiRuntimeServicesData, 3EA76000, 5, F
19, 5, EfiRuntimeServicesCode, 3EA7B000, 5, F
20, 6, EfiRuntimeServicesData, 3EA80000, 5, F
21, 5, EfiRuntimeServicesCode, 3EA85000, 7, F
22, 6, EfiRuntimeServicesData, 3EA8C000, 8F, F
23, 4, EfiBootServicesData, 3EB1B000, 4DA, F
24, 7, EfiConventionalMemory, 3EFF5000, 4, F
25, 4, EfiBootServicesData, 3EFF9000, 6, F
26, 7, EfiConventionalMemory, 3EFFF000, 1, F
27, 4, EfiBootServicesData, 3F000000, A1B, F
28, 7, EfiConventionalMemory, 3FA1B000, 1, F
29, 3, EfiBootServicesCode, 3FA1C000, 17F, F
30, 5, EfiRuntimeServicesCode, 3FB9B000, 30, F
31, 6, EfiRuntimeServicesData, 3FBCB000, 24, F
32, 0, EfiReservedMemoryType, 3FBEF000, 4, F
33, 9, EfiACPIReclaimMemory, 3FBF3000, 8, F
34, A, EfiACPIMemoryNVS, 3FBFB000, 4, F
35, 4, EfiBootServicesData, 3FBFF000, 201, F
36, 7, EfiConventionalMemory, 3FE00000, 8D, F
37, 4, EfiBootServicesData, 3FE8D000, 20, F
38, 3, EfiBootServicesCode, 3FEAD000, 20, F
39, 4, EfiBootServicesData, 3FECD000, 9, F
40, 3, EfiBootServicesCode, 3FED6000, 1E, F
41, 6, EfiRuntimeServicesData, 3FEF4000, 84, F
42, A, EfiACPIMemoryNVS, 3FF78000, 88, F
43, 6, EfiRuntimeServicesData, FFC00000, 400, 1

*/