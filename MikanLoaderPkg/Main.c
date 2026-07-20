#include  <Uefi.h>
#include  <Library/UefiLib.h>
#include  <Library/UefiBootServicesTableLib.h>
#include  <Library/PrintLib.h>
#include  <Protocol/LoadedImage.h>
#include  <Protocol/SimpleFileSystem.h>
#include  <Protocol/DiskIo2.h>
#include  <Protocol/BlockIo.h>
#include  <Guid/FileInfo.h>
 
/**
 * MemoryMap - 对 UEFI gBS->GetMemoryMap 五个输出参数的封装结构体
 *
 * UEFI 原始 API 的签名为：
 *   EFI_STATUS GetMemoryMap(
 *       UINTN*                 MemoryMapSize,      // [in/out]
 *       EFI_MEMORY_DESCRIPTOR* MemoryMap,          // [out]
 *       UINTN*                 MapKey,             // [out]
 *       UINTN*                 DescriptorSize,     // [out]
 *       UINT32*                DescriptorVersion   // [out]
 *   );
 *
 * 将这五个参数连同调用者管理的缓冲区信息统一放入一个结构体，
 * 方便在 GetMemoryMap / SaveMemoryMap / ExitBootServices 之间传递。
 *
 * 字段说明：
 *
 *   buffer_size
 *     调用者分配的缓冲区字节数（只读，由调用者在初始化时填入）。
 *     传给 gBS->GetMemoryMap 作为缓冲区容量上限；
 *     若实际内存映射大于此值，固件返回 EFI_BUFFER_TOO_SMALL。
 *
 *   buffer
 *     指向存放 EFI_MEMORY_DESCRIPTOR 数组的内存缓冲区。
 *     必须在调用前由调用者分配并置为非 NULL；
 *     调用成功后，从 buffer 起的 map_size 字节即为有效描述符数据。
 *
 *   map_size
 *     [in]  调用前须被设为 buffer_size，告知固件缓冲区容量。
 *     [out] 调用成功后由固件更新为实际填充的字节数（<= buffer_size）。
 *     描述符条数 = map_size / descriptor_size。
 *
 *   map_key
 *     [out] 固件为本次内存映射快照生成的单调递增票据（token）。
 *     gBS->ExitBootServices 必须传入与最近一次 GetMemoryMap 返回值
 *     完全一致的 map_key；若固件在两次调用之间分配/释放了任何内存
 *     （如事件回调触发了内存操作），map_key 会改变，ExitBootServices
 *     将返回 EFI_INVALID_PARAMETER，调用者需重新获取内存映射再重试。
 *
 *   descriptor_size
 *     [out] 单个 EFI_MEMORY_DESCRIPTOR 的实际字节数。
 *     UEFI 规范允许固件在标准结构体末尾追加私有字段，因此
 *     descriptor_size >= sizeof(EFI_MEMORY_DESCRIPTOR)（通常为 48 字节）。
 *     遍历描述符数组时必须用此值步进，而不能用 sizeof(EFI_MEMORY_DESCRIPTOR)，
 *     否则在追加了私有字段的固件上会错位读取（参见 SaveMemoryMap 中的迭代器设计）。
 *
 *   descriptor_version
 *     [out] EFI_MEMORY_DESCRIPTOR 的格式版本号。
 *     当前规范定义的版本为 EFI_MEMORY_DESCRIPTOR_VERSION（值为 1）。
 *     若未来规范扩展了描述符格式，版本号将递增；
 *     本程序未使用此字段，仅将其透传给 gBS->GetMemoryMap 以满足 API 要求。
 */
struct MemoryMap {
  UINTN buffer_size;
  VOID* buffer;
  UINTN map_size;
  UINTN map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
};
 
/**
 * GetMemoryMap - 封装 gBS->GetMemoryMap，将结果填入 MemoryMap 结构体
 *
 * UEFI 的 gBS->GetMemoryMap 描述当前系统所有物理内存区域的类型与属性，
 * 是引导程序在退出 Boot Services 前必须完成的关键步骤。
 * 本函数对原始 API 做了轻量封装：
 *   - 统一管理缓冲区指针与大小，避免调用者手动拆散五个参数
 *   - 在调用前将 map_size 重置为 buffer_size（gBS->GetMemoryMap 要求如此）
 *   - 提前检测空指针，返回语义明确的错误码
 *
 * 调用时序（典型用法）：
 *
 *   ① 初次调用（获取内存映射并取得 map_key）
 *        GetMemoryMap(&memmap);
 *        → memmap.map_key 被固件填入当前快照票据
 *
 *   ② ExitBootServices 失败后的重试调用
 *        GetMemoryMap(&memmap);          // 重新获取，刷新 map_key
 *        gBS->ExitBootServices(..., memmap.map_key);
 *
 *   注意：两次 GetMemoryMap 之间不得有任何可能触发内存分配/释放的操作
 *   （包括 Print、文件 I/O 等），否则 map_key 会在调用间隙失效。
 *
 * 参数：
 *   map  调用者提供的 MemoryMap 结构体指针；
 *        调用前 map->buffer 必须指向足够大的有效缓冲区，
 *        map->buffer_size 必须填入缓冲区字节数。
 *        调用成功后，map->map_size / map_key / descriptor_size /
 *        descriptor_version 均由固件填充完毕。
 *
 * 返回值：
 *   EFI_BUFFER_TOO_SMALL  map->buffer 为 NULL（本函数提前检测）
 *   EFI_BUFFER_TOO_SMALL  缓冲区不足（固件检测，buffer_size 太小）
 *   EFI_INVALID_PARAMETER 参数非法（固件检测）
 *   EFI_SUCCESS           成功，map 各输出字段已填充
 */
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
 
  // 前置检查：buffer 为 NULL 时无法传给固件，提前返回。
  // 使用 EFI_BUFFER_TOO_SMALL 而非 EFI_INVALID_PARAMETER 是为了语义一致——
  // 调用者拿到此错误码后可以分配更大的缓冲区再重试，处理路径与缓冲区不足相同。
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
 
  // gBS->GetMemoryMap 的第一个参数 MemoryMapSize 是双向的：
  //   [in]  传入时须为"缓冲区可用字节数"（即 buffer_size）
  //   [out] 返回时固件将其更新为"实际填充字节数"（即 map_size）
  // 若不在每次调用前将 map_size 重置为 buffer_size，则重复调用时
  // map_size 可能残留上次的较小值，导致固件截断描述符数组。
  map->map_size = map->buffer_size;
 
  // 调用 UEFI Boot Services 的 GetMemoryMap，参数逐一说明：
  //
  //   &map->map_size
  //     传入缓冲区容量，返回实际使用字节数（参见上方说明）。
  //
  //   (EFI_MEMORY_DESCRIPTOR*)map->buffer
  //     强制转换为固件期望的描述符数组类型；
  //     固件从此地址起连续写入若干个 EFI_MEMORY_DESCRIPTOR，
  //     相邻两个描述符之间的步长为 descriptor_size（而非 sizeof 值）。
  //
  //   &map->map_key
  //     接收本次快照的票据；每次内存状态改变固件都会生成新值，
  //     ExitBootServices 以此为握手信号确认内存映射未过期。
  //
  //   &map->descriptor_size
  //     接收单个描述符的实际字节数（含固件私有扩展字段）；
  //     遍历数组时必须用此值作为步长，禁止硬编码 sizeof(EFI_MEMORY_DESCRIPTOR)。
  //
  //   &map->descriptor_version
  //     接收描述符格式版本号（目前恒为 1），本程序未使用但必须传入非 NULL 指针。
  return gBS->GetMemoryMap(
      &map->map_size,
      (EFI_MEMORY_DESCRIPTOR*)map->buffer,
      &map->map_key,
      &map->descriptor_size,
      &map->descriptor_version);
}
 
/**
 * GetMemoryTypeUnicode - 将 EFI_MEMORY_TYPE 枚举值转换为可读的宽字符串
 *
 * UEFI 规范（UEFI Specification 2.x §7.2）定义了 EFI_MEMORY_TYPE 枚举，
 * 用于描述物理内存区域的用途与归属。GetMemoryMap 返回的每个
 * EFI_MEMORY_DESCRIPTOR 都携带一个 Type 字段，本函数将其转成人类可读的
 * UTF-16 字符串，供 SaveMemoryMap 写入 CSV 文件时使用。
 *
 * 返回值是指向字符串字面量的指针（存储在只读数据段 .rodata），
 * 调用者不得修改或释放该指针所指向的内容。
 *
 * 参数：
 *   type  EFI_MEMORY_TYPE 枚举值，通常来自 EFI_MEMORY_DESCRIPTOR.Type
 *
 * 返回值：
 *   对应类型的 UTF-16 字符串指针；若 type 不在已知范围内则返回
 *   L"InvalidMemoryType"（枚举值可能是固件私有扩展或内存损坏导致的非法值）
 *
 * 为什么返回 CHAR16*（宽字符）而非 CHAR8*（ASCII）？
 *   UEFI 内部的字符串 API（Print、AsciiSPrint 的 %-ls 格式符等）
 *   以 UTF-16LE 为原生编码，L"..." 字面量直接兼容，无需转换。
 */
const CHAR16* GetMemoryTypeUnicode(EFI_MEMORY_TYPE type) {
  switch (type) {
    // ── 类型 0：EfiReservedMemoryType ────────────────────────────────────
    // 固件保留区域，不对外公开用途。
    // 任何软件（包括操作系统）都不得使用这段内存；
    // 典型用途：固件内部数据结构、SMRAM 影子区域等。
    case EfiReservedMemoryType: return L"EfiReservedMemoryType";
 
    // ── 类型 1：EfiLoaderCode ─────────────────────────────────────────────
    // 当前正在运行的 UEFI 应用程序（即本引导加载程序）的可执行代码段。
    // ExitBootServices 之后，操作系统可以将此区域回收为可用内存，
    // 因为引导程序代码不再需要执行。
    case EfiLoaderCode: return L"EfiLoaderCode";
 
    // ── 类型 2：EfiLoaderData ─────────────────────────────────────────────
    // 当前 UEFI 应用程序的数据段，包括栈、堆、BSS 等。
    // 本文件中在 UefiMain 栈上分配的 memmap_buf、以及
    // AllocatePages(EfiLoaderData) 分配的内核加载内存，都属于此类型。
    // ExitBootServices 之后同样可被操作系统回收（内核已从中复制完数据后）。
    case EfiLoaderData: return L"EfiLoaderData";
 
    // ── 类型 3：EfiBootServicesCode ──────────────────────────────────────
    // UEFI Boot Services 驱动程序的可执行代码（如 FAT 文件系统驱动、
    // 图形输出协议驱动等）。
    // ExitBootServices 调用后这些驱动停止服务，其内存可被操作系统回收。
    case EfiBootServicesCode: return L"EfiBootServicesCode";
 
    // ── 类型 4：EfiBootServicesData ──────────────────────────────────────
    // UEFI Boot Services 驱动程序的数据区域（配置表、事件队列等）。
    // 同 EfiBootServicesCode，ExitBootServices 后可安全回收。
    case EfiBootServicesData: return L"EfiBootServicesData";
 
    // ── 类型 5：EfiRuntimeServicesCode ───────────────────────────────────
    // UEFI Runtime Services 的可执行代码。
    // Runtime Services（如 GetTime、SetVariable、ResetSystem）在操作系统
    // 运行期间依然有效，因此这段内存在 ExitBootServices 后【不可回收】，
    // 操作系统必须保留并在调用 SetVirtualAddressMap 后维护其虚拟映射。
    case EfiRuntimeServicesCode: return L"EfiRuntimeServicesCode";
 
    // ── 类型 6：EfiRuntimeServicesData ───────────────────────────────────
    // UEFI Runtime Services 的数据区域（NVRAM 变量缓存、时钟寄存器映射等）。
    // 与 EfiRuntimeServicesCode 同理，操作系统必须在整个运行期间保留此区域。
    case EfiRuntimeServicesData: return L"EfiRuntimeServicesData";
 
    // ── 类型 7：EfiConventionalMemory ────────────────────────────────────
    // 普通可用 RAM，固件当前未使用。
    // 这是操作系统内存管理器最感兴趣的类型——内核的物理页分配器
    // 应以此类型的区域作为可分配内存池的来源。
    // 在实际系统上，此类型通常覆盖绝大多数物理内存（几 GiB 的主存）。
    case EfiConventionalMemory: return L"EfiConventionalMemory";
 
    // ── 类型 8：EfiUnusableMemory ─────────────────────────────────────────
    // 已检测到硬件错误（ECC 不可修正错误等）的内存区域，
    // 操作系统和固件均不得使用。应从可用物理内存池中永久排除。
    case EfiUnusableMemory: return L"EfiUnusableMemory";
 
    // ── 类型 9：EfiACPIReclaimMemory ─────────────────────────────────────
    // 存放 ACPI 表（RSDP、RSDT/XSDT、DSDT、SSDT 等）的内存。
    // 操作系统在完成 ACPI 表解析之后，可以将此区域回收为普通内存。
    // 在回收之前必须先读完所有需要的 ACPI 数据。
    case EfiACPIReclaimMemory: return L"EfiACPIReclaimMemory";
 
    // ── 类型 10：EfiACPIMemoryNVS ────────────────────────────────────────
    // ACPI NVS（Non-Volatile Storage）区域，用于在 S3 休眠/唤醒等
    // 电源状态切换时保存固件私有状态。
    // 操作系统在整个运行期间【不得触碰】此区域，休眠恢复依赖于它。
    case EfiACPIMemoryNVS: return L"EfiACPIMemoryNVS";
 
    // ── 类型 11：EfiMemoryMappedIO ───────────────────────────────────────
    // 内存映射 I/O 区域（Memory-Mapped I/O, MMIO）。
    // 对应 PCI/PCIe 设备的 BAR（Base Address Register）空间、
    // 本地 APIC、HPET 等硬件寄存器区域。
    // 操作系统需将这些地址映射到虚拟地址空间中才能访问硬件，
    // 不可将其当作普通 RAM 使用。
    case EfiMemoryMappedIO: return L"EfiMemoryMappedIO";
 
    // ── 类型 12：EfiMemoryMappedIOPortSpace ──────────────────────────────
    // 内存映射 I/O 端口空间（某些平台将传统 x86 I/O 端口映射到内存地址）。
    // 在标准 x86-64 系统上极少出现；主要见于 Itanium（IA-64）平台，
    // 该平台没有独立的 IN/OUT 指令，I/O 端口统一映射到内存空间。
    case EfiMemoryMappedIOPortSpace: return L"EfiMemoryMappedIOPortSpace";
 
    // ── 类型 13：EfiPalCode ───────────────────────────────────────────────
    // PAL（Processor Abstraction Layer）代码区域，专用于 Itanium 架构。
    // PAL 是处理器固件（类似 x86 的 microcode 更新机制），
    // 操作系统不得修改或回收此区域。在 x86-64 系统上几乎不会出现。
    case EfiPalCode: return L"EfiPalCode";
 
    // ── 类型 14：EfiPersistentMemory ─────────────────────────────────────
    // 持久性内存（Persistent Memory，如 Intel Optane DCPMM / NVDIMM）区域。
    // 这类内存在断电后数据不丢失，兼具 DRAM 的字节寻址能力和存储介质的持久性。
    // UEFI 规范 2.6 起引入此类型；操作系统需要专门的 PMEM 驱动来管理。
    case EfiPersistentMemory: return L"EfiPersistentMemory";
 
    // ── 类型 15：EfiMaxMemoryType ─────────────────────────────────────────
    // 枚举上界哨兵值，不代表实际内存类型。
    // UEFI 规范用此值标记枚举范围的末尾，便于边界检查循环（i < EfiMaxMemoryType）。
    // 若在内存映射中出现此值，通常意味着固件实现有误。
    case EfiMaxMemoryType: return L"EfiMaxMemoryType";
 
    // ── 默认：未知 / 固件私有扩展类型 ────────────────────────────────────
    // UEFI 规范允许固件厂商定义 0x70000000~0x7FFFFFFF 范围内的私有类型，
    // 以及操作系统自定义 0x80000000~0xFFFFFFFF 范围的类型。
    // 遇到这些值时返回通用的错误标识，调用者可据此决定是否跳过该区域。
    default: return L"InvalidMemoryType";
  }
}
 
/**
 * SaveMemoryMap - 将 UEFI 内存映射以 CSV 格式写入文件
 *
 * 本函数遍历 GetMemoryMap 返回的内存描述符数组，把每条描述符格式化为
 * 一行 CSV 文本并写入 EFI_FILE_PROTOCOL 文件句柄，供内核或调试工具读取。
 *
 * 输出 CSV 格式（每行字段）：
 *   Index          : 描述符在数组中的序号（从 0 开始）
 *   Type           : EFI_MEMORY_TYPE 枚举值（十六进制整数）
 *   Type(name)     : 对应枚举的可读名称字符串，如 "EfiConventionalMemory"
 *   PhysicalStart  : 该内存区域的物理起始地址（十六进制，8 位补零）
 *   NumberOfPages  : 该区域的 4 KiB 页数（十六进制）
 *   Attribute      : 内存属性标志位（低 20 bit，十六进制）
 *
 * 参数：
 *   map   指向已填充好的 MemoryMap 结构体（由 GetMemoryMap 填充）
 *   file  已打开的可写文件句柄（由调用者负责创建与关闭）
 *
 * 返回值：
 *   EFI_SUCCESS（内部 Write 调用的错误未被检查，生产代码应逐步检查）
 */
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
 
  // 单行 CSV 文本的格式化缓冲区。
  // 256 字节足以容纳一条描述符行（最长的类型名约 30 字符，地址 16 字符，共约 80 字符）。
  CHAR8 buf[256];
 
  // EFI_FILE_PROTOCOL->Write 要求传入"要写的字节数"的指针，
  // 写入完成后固件会将其更新为"实际写入的字节数"。
  // 此变量在写表头和写每行数据时复用。
  UINTN len;
 
  /* ── 写入 CSV 表头 ───────────────────────────────────────────────────── */
 
  // 使用 ASCII 字符串（CHAR8*）而非 UEFI 宽字符串（CHAR16*），
  // 因为文件内容是纯文本 CSV，不需要 UTF-16 编码。
  // 字段顺序与后续 AsciiSPrint 的格式串一一对应。
  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
 
  // AsciiStrLen 返回不含终止符 '\0' 的字节数，
  // 这正是 Write 需要的"内容长度"（不应把 '\0' 写入文件）。
  len = AsciiStrLen(header);
 
  // EFI_FILE_PROTOCOL->Write 原型：
  //   Write(This, BufferSize, Buffer)
  //     This       → 协议自身指针（C 模拟 OOP）
  //     BufferSize → 传入时为"期望写入字节数"，返回后为"实际写入字节数"
  //     Buffer     → 待写数据指针
  // 写入后 len 可能被固件修改（若磁盘满等异常），此处忽略该情况。
  file->Write(file, &len, header);
 
  /* ── 调试输出：打印缓冲区地址和内存映射总大小 ────────────────────────── */
 
  // 打印到 UEFI 控制台（屏幕），供开发者验证内存映射是否已正确获取。
  //   map->buffer   : 存放描述符数组的缓冲区起始地址
  //   map->map_size : GetMemoryMap 实际填写的字节总数（≤ buffer_size）
  // %08lx 格式：十六进制，至少 8 位，不足补零，'l' 对应 UINTN（64 位平台为 64 位整数）
  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);
 
  /* ── 遍历内存描述符数组并逐行写入文件 ───────────────────────────────── */
 
  // 迭代器设计：用物理地址（整数）而非指针步进，
  // 原因是每条描述符的实际大小 map->descriptor_size 由固件决定，
  // 可能大于 sizeof(EFI_MEMORY_DESCRIPTOR)（固件可在末尾附加私有字段）。
  // 若用 EFI_MEMORY_DESCRIPTOR* 指针做 ++ 运算，步长固定为结构体大小，
  // 会错位读取描述符；改用整数加法则可精确步进任意字节数。
  //
  // 循环变量：
  //   iter : 当前描述符的物理地址（从缓冲区起点开始，每次加 descriptor_size）
  //   i    : 描述符序号（CSV 的 Index 列）
  EFI_PHYSICAL_ADDRESS iter;
  int i;
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;  // 终止条件：超出已用区域
       iter += map->descriptor_size, i++) {                        // 步长：固件实际描述符大小
 
    // 将当前地址强制转换为描述符指针以访问各字段。
    // EFI_MEMORY_DESCRIPTOR 主要字段：
    //   Type          (UINT32) : EFI_MEMORY_TYPE 枚举，标识内存用途
    //   PhysicalStart (EFI_PHYSICAL_ADDRESS=UINT64) : 区域物理起始地址，4 KiB 对齐
    //   VirtualStart  (EFI_VIRTUAL_ADDRESS=UINT64)  : 虚拟起始地址（SetVirtualAddressMap 后有效）
    //   NumberOfPages (UINT64) : 区域大小，单位为 4 KiB 页（字节数 = NumberOfPages * 4096）
    //   Attribute     (UINT64) : 内存属性位掩码（见下方说明）
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;
 
    // 格式化一行 CSV 并写入文件。
    // AsciiSPrint 是 EDK II 提供的安全格式化函数，等价于 snprintf（ASCII 版本），
    // 返回实际写入 buf 的字节数（不含 '\0'）。
    //
    // 格式串各字段对应关系：
    //   %u    → i                              : 十进制序号
    //   %x    → desc->Type                    : 枚举值（十六进制）
    //   %-ls  → GetMemoryTypeUnicode(...)      : 左对齐宽字符串（%-ls 中 l 表示宽字符）
    //   %08lx → desc->PhysicalStart            : 物理地址，8 位十六进制补零
    //   %lx   → desc->NumberOfPages            : 页数（十六进制）
    //   %lx   → desc->Attribute & 0xffffflu    : 属性低 20 位（十六进制）
    //
    // 属性字段说明（Attribute & 0xffffflu 只保留低 20 位）：
    //   UEFI 规范中低 20 位是硬件无关的"能力"标志，例如：
    //     bit 0  (0x001) EFI_MEMORY_UC   : 支持不可缓存（Uncacheable）
    //     bit 1  (0x002) EFI_MEMORY_WC   : 支持写合并（Write Combining）
    //     bit 2  (0x004) EFI_MEMORY_WT   : 支持写通（Write Through）
    //     bit 3  (0x008) EFI_MEMORY_WB   : 支持写回（Write Back），普通 RAM 典型值
    //     bit 4  (0x010) EFI_MEMORY_UCE  : 支持 UC 且可导出（UC Exported）
    //     bit 12 (0x1000) EFI_MEMORY_WP  : 写保护
    //     bit 13 (0x2000) EFI_MEMORY_RP  : 读保护
    //     bit 14 (0x4000) EFI_MEMORY_XP  : 执行禁止
    //     bit 15 (0x8000) EFI_MEMORY_NV  : 非易失性（NVDIMM 等）
    //   高位（bit 20 起）是平台相关标志，本函数将其屏蔽以保持输出简洁。
    len = AsciiSPrint(
        buf, sizeof(buf),
        "%u, %x, %-ls, %08lx, %lx, %lx\n",
        i, desc->Type, GetMemoryTypeUnicode(desc->Type),
        desc->PhysicalStart, desc->NumberOfPages,
        desc->Attribute & 0xffffflu);
 
    // 将格式化好的一行写入文件；len 此时为有效字节数（不含 '\0'）
    file->Write(file, &len, buf);
  }
 
  return EFI_SUCCESS;
}
 
/**
 * OpenRootDir - 打开本程序所在 ESP 分区的文件系统根目录
 *
 * UEFI 中访问文件系统需要经历三个层次的协议查询：
 *
 *   image_handle
 *       │  (EFI_LOADED_IMAGE_PROTOCOL)
 *       │  → 得知本程序被加载到哪个设备（DeviceHandle）
 *       ▼
 *   DeviceHandle
 *       │  (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL)
 *       │  → 获得对该设备文件系统的操作接口
 *       ▼
 *   EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
 *       │  OpenVolume()
 *       │  → 打开文件系统根目录，返回 EFI_FILE_PROTOCOL*
 *       ▼
 *   EFI_FILE_PROTOCOL* (根目录句柄)
 *
 * 参数：
 *   image_handle  本 UEFI 程序自身的句柄，由固件在启动时分配
 *   root          输出参数，调用成功后指向根目录的 EFI_FILE_PROTOCOL 指针
 *
 * 返回值：
 *   EFI_SUCCESS（本实现忽略了内部 OpenProtocol 的错误，生产代码应逐步检查）
 */
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
 
  /* ── 步骤 1：通过 image_handle 查询 EFI_LOADED_IMAGE_PROTOCOL ────────── */
 
  // EFI_LOADED_IMAGE_PROTOCOL 描述"已被 UEFI 加载到内存的镜像"的元信息，
  // 其中最重要的字段是 DeviceHandle——指向承载本程序的块设备句柄（即 ESP 所在磁盘分区）。
  // 其他常用字段包括：
  //   ImageBase    → 本程序被加载到内存的起始地址
  //   ImageSize    → 镜像字节数
  //   LoadOptions  → UEFI Shell 或 Boot Manager 传来的命令行参数
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
 
  // EFI_SIMPLE_FILE_SYSTEM_PROTOCOL 是对块设备上文件系统的抽象接口，
  // 只有一个方法：OpenVolume()，用于打开文件系统根目录。
  // 声明在此处，供步骤 2 使用。
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
 
  // gBS->OpenProtocol 是 UEFI 访问任意协议接口的统一入口。
  // 参数含义（共 6 个）：
  //
  //   1. image_handle
  //      要查询的目标句柄——这里就是本程序自己，
  //      因为我们想知道"我自己"被装载在哪个设备上。
  //
  //   2. &gEfiLoadedImageProtocolGuid
  //      协议的全局唯一标识符（GUID），固件凭此在句柄的协议列表中
  //      找到对应的接口实例。gEfiLoadedImageProtocolGuid 由 EDK II 库预定义。
  //
  //   3. (VOID**)&loaded_image
  //      输出参数：固件将协议接口指针写入 loaded_image，
  //      之后即可通过 loaded_image->xxx 访问协议提供的字段和方法。
  //
  //   4. image_handle（第二次出现，含义不同）
  //      "调用者句柄"（AgentHandle）：声明"是谁在打开这个协议"。
  //      固件用此记录协议的使用关系，以便在调用者卸载时自动清理。
  //
  //   5. NULL
  //      "控制器句柄"（ControllerHandle）：仅在 BY_DRIVER 模式下使用，
  //      这里选用 BY_HANDLE_PROTOCOL 模式，所以传 NULL。
  //
  //   6. EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL
  //      打开模式：表示"我只是要读取这个句柄上已安装的协议，不参与驱动绑定"。
  //      其他常见模式：
  //        EFI_OPEN_PROTOCOL_GET_PROTOCOL     → 不做所有权记录，适合一次性读取
  //        EFI_OPEN_PROTOCOL_BY_DRIVER        → 驱动绑定时使用，具有排他性
  //        EFI_OPEN_PROTOCOL_EXCLUSIVE        → 独占模式，阻止其他驱动绑定
  gBS->OpenProtocol(
      image_handle,
      &gEfiLoadedImageProtocolGuid,
      (VOID**)&loaded_image,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
 
  /* ── 步骤 2：通过 DeviceHandle 查询文件系统协议 ─────────────────────── */
 
  // loaded_image->DeviceHandle 是本程序所在存储设备（ESP 分区）的句柄。
  // UEFI 固件会为每个检测到的存储分区创建一个 Handle，并在其上安装多个协议：
  //   - EFI_BLOCK_IO_PROTOCOL         → 以块为单位读写原始扇区
  //   - EFI_DISK_IO_PROTOCOL          → 以字节为单位读写磁盘
  //   - EFI_SIMPLE_FILE_SYSTEM_PROTOCOL → 以文件/目录为单位操作（FAT32/FAT16）
  //
  // 我们直接查询最上层的文件系统协议，无需关心底层 FAT 解析细节。
  //
  // 参数含义（对比步骤 1）：
  //   1. loaded_image->DeviceHandle → 这次查询目标换成了"设备句柄"而非程序句柄
  //   2. &gEfiSimpleFileSystemProtocolGuid → 换成文件系统协议的 GUID
  //   3. (VOID**)&fs → 输出：文件系统协议接口指针
  //   4~6. 其余参数含义与步骤 1 相同
  gBS->OpenProtocol(
      loaded_image->DeviceHandle,
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID**)&fs,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
 
  /* ── 步骤 3：打开文件系统根目录 ─────────────────────────────────────── */
 
  // EFI_SIMPLE_FILE_SYSTEM_PROTOCOL 只有一个方法 OpenVolume()：
  //   OpenVolume(This, Root)
  //     This → 协议自身指针（C 语言模拟 OOP 的惯用写法）
  //     Root → 输出参数，成功后 *root 指向根目录的 EFI_FILE_PROTOCOL 句柄
  //
  // 根目录对应文件系统的"/"（UEFI 中写作 "\"），
  // 拿到 root 后即可调用 root->Open() 打开其下的文件或子目录。
  // 使用完毕后应调用 root->Close(root) 释放句柄（本程序未显式关闭，
  // ExitBootServices 后固件会回收所有资源）。
  fs->OpenVolume(fs, root);
 
  // 本实现统一返回 EFI_SUCCESS。
  // 注意：上述三步的返回值均未检查。若 OpenProtocol 或 OpenVolume 失败，
  // *root 将是野指针，后续 Open/Read 操作会崩溃。
  // 生产级引导程序应对每步返回值进行 EFI_ERROR() 检查并打印诊断信息。
  return EFI_SUCCESS;
}
 
/**
 * UefiMain - UEFI 应用程序入口点（MikanOS 引导加载程序主函数）
 *
 * 本函数是整个 UEFI 引导程序的核心，负责在 UEFI 固件环境下完成以下工作：
 *   1. 获取并保存物理内存映射（供内核将来使用）
 *   2. 从 ESP（EFI 系统分区）读取内核 ELF 文件并加载到固定物理地址
 *   3. 退出 UEFI Boot Services（让固件释放控制权）
 *   4. 跳转到内核入口点，将控制权交给内核
 *
 * 函数签名约定：
 *   - EFI_STATUS EFIAPI：遵循 UEFI 调用约定（x86-64 上等同于 MS ABI）
 *   - image_handle：UEFI 固件为本程序分配的句柄，用于查询本程序自身的
 *                   加载信息（如所在设备、加载地址等）
 *   - system_table：指向 UEFI 系统表的指针，包含固件提供的各种服务表
 *                   （Boot Services、Runtime Services、配置表等）
 *                   注：gBS（全局 Boot Services 指针）在库初始化时已从
 *                   system_table->BootServices 提取，可直接使用
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE* system_table) {
 
  /* ── 阶段 0：确认引导程序已成功启动 ─────────────────────────────────── */
 
  // 向 UEFI 控制台（通常是屏幕）打印启动标志，用于调试确认程序已被 UEFI 加载并运行
  Print(L"Hello, Mikan World!\n");
 
  /* ── 阶段 1：获取物理内存映射 ────────────────────────────────────────── */
 
  // 在栈上分配 16 KiB（4096 * 4 字节）的缓冲区用于存储内存映射。
  // 每个 EFI_MEMORY_DESCRIPTOR 约 40 字节，16 KiB 足以容纳数百条描述符。
  // 使用栈分配而非堆分配，避免在 Boot Services 退出前出现内存泄漏。
  CHAR8 memmap_buf[4096 * 4];
 
  // 初始化 MemoryMap 结构体：
  //   buffer_size      = sizeof(memmap_buf)  ← 告诉 GetMemoryMap 缓冲区有多大
  //   buffer           = memmap_buf          ← 指向实际缓冲区
  //   map_size         = 0  ← 调用后由固件填写实际使用的字节数
  //   map_key          = 0  ← 调用后由固件填写，ExitBootServices 需要此值
  //   descriptor_size  = 0  ← 调用后由固件填写单个描述符的实际大小
  //                          （可能 > sizeof(EFI_MEMORY_DESCRIPTOR)，需用此值步进）
  //   descriptor_version = 0 ← 调用后由固件填写描述符格式版本
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
 
  // 调用封装函数获取当前内存映射。
  // 成功后 memmap.map_key 会被更新——这是一个"快照票据"，
  // 后续 ExitBootServices 必须传入此 key，以证明内存映射在快照后未发生变化。
  GetMemoryMap(&memmap);
 
  /* ── 阶段 2：打开 ESP 根目录 ─────────────────────────────────────────── */
 
  // 通过 image_handle 找到本程序所在的文件系统设备，并打开其根目录。
  // 具体步骤：
  //   a) 用 image_handle 查询 EFI_LOADED_IMAGE_PROTOCOL，获得 DeviceHandle
  //   b) 用 DeviceHandle 查询 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  //   c) 调用 OpenVolume 打开文件系统根目录
  // 引导程序本身和 kernel.elf 都在同一个 ESP 分区上，所以直接用自己的设备句柄即可。
  EFI_FILE_PROTOCOL* root_dir;
  OpenRootDir(image_handle, &root_dir);
 
  /* ── 阶段 3：将内存映射写入文件（供调试用）─────────────────────────── */
 
  // 在 ESP 根目录下创建（或覆盖）文件 \memmap，以 CSV 格式记录内存布局。
  // EFI_FILE_MODE_CREATE：若文件不存在则创建，存在则截断重写
  // EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE：同时具备读写权限
  // 最后一个参数 0：文件属性（普通文件，非目录/只读等）
  EFI_FILE_PROTOCOL* memmap_file;
  root_dir->Open(
      root_dir, &memmap_file, L"\\memmap",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
 
  // 将内存映射以 CSV 格式写入文件（字段：Index, Type, Type名称, 物理起始地址, 页数, 属性）
  SaveMemoryMap(&memmap, memmap_file);
 
  // 关闭文件句柄，确保数据刷新到磁盘缓存（UEFI 环境下不一定立即落盘）
  memmap_file->Close(memmap_file);
 
  /* ── 阶段 4：读取并加载内核 ELF 文件 ────────────────────────────────── */
  // #@@range_begin(read_kernel)
 
  // 以只读模式打开 ESP 根目录下的 \kernel.elf
  EFI_FILE_PROTOCOL* kernel_file;
  root_dir->Open(
      root_dir, &kernel_file, L"\\kernel.elf",
      EFI_FILE_MODE_READ, 0);
 
  // EFI_FILE_INFO 是可变长结构体：固定头部 + 文件名字符串（UTF-16）。
  // 文件名最长 12 个 UTF-16 字符（"kernel.elf\0" = 11 字符 + 终止符），
  // 因此缓冲区大小 = 固定部分 + 12 * sizeof(CHAR16)
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
 
  // 通过 gEfiFileInfoGuid 查询文件元数据，填充到 file_info_buffer。
  // 获取后可读取 FileSize（文件字节数）、FileName 等字段。
  kernel_file->GetInfo(
      kernel_file, &gEfiFileInfoGuid,
      &file_info_size, file_info_buffer);
 
  // 将缓冲区解释为 EFI_FILE_INFO* 并取出文件大小
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;
 
  // 在物理地址 0x100000（1 MiB）处分配足够多的连续物理页用于存放内核。
  //
  // 为什么是 1 MiB？
  //   - 0x000000~0x0FFFFF 是传统 PC 的"低 1 MiB"区域，包含 BIOS 数据区、
  //     中断向量表、视频缓冲区等遗留结构，内核应当避开。
  //   - 1 MiB 是约定俗成的内核加载起点，与内核链接脚本中的 VMA 对应。
  //
  // AllocateAddress：要求固件在"指定"物理地址分配，而非让固件自由选择。
  // EfiLoaderData：类型标记为"引导加载程序数据"，内核启动后可按需回收。
  // 页数计算：(size + 0xfff) / 0x1000 = 向上取整到 4 KiB 页边界
  EFI_PHYSICAL_ADDRESS kernel_base_addr = 0x100000;
  gBS->AllocatePages(
      AllocateAddress, EfiLoaderData,
      (kernel_file_size + 0xfff) / 0x1000, &kernel_base_addr);
 
  // 将内核文件内容整体读入刚分配的物理内存。
  // 注意：此处将整个 ELF 文件（包括 ELF 头、程序头表、各段）原样读入，
  // 并未做 ELF 段映射（没有按 PT_LOAD segment 分别加载）。
  // 这依赖于链接器将内核链接为"平坦二进制"或保证 0x100000 对齐的简单布局。
  kernel_file->Read(kernel_file, &kernel_file_size, (VOID*)kernel_base_addr);
 
  // 打印内核加载地址和大小，便于调试
  Print(L"Kernel: 0x%0lx (%lu bytes)\n", kernel_base_addr, kernel_file_size);
  // #@@range_end(read_kernel)
 
  /* ── 阶段 5：退出 UEFI Boot Services ────────────────────────────────── */
  // #@@range_begin(exit_bs)
 
  // ExitBootServices 是 UEFI 引导流程中的"不归路"操作：
  //   - 调用后，UEFI Boot Services（内存管理、事件系统、设备协议等）全部失效
  //   - gBS 指针不可再用，任何 gBS->xxx() 调用均是未定义行为
  //   - 固件将内存控制权移交给操作系统
  //   - 必须传入最新的 map_key，固件以此验证内存映射自上次查询后未被修改；
  //     若固件在两次调用之间分配/释放了内存，map_key 会变化，调用将返回错误
  EFI_STATUS status;
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
 
  if (EFI_ERROR(status)) {
    // 首次退出失败，说明 map_key 已过期（Boot Services 内部状态变化导致内存映射更新）。
    // 必须重新获取最新的内存映射以得到新的 map_key，再重试一次。
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      // 如果连内存映射都取不到，说明固件状态严重异常，只能挂死报错
      Print(L"failed to get memory map: %r\n", status);
      while (1);  // 无限循环，防止 CPU 执行到非法地址；%r 格式化 EFI_STATUS 为可读字符串
    }
 
    // 用新的 map_key 再次尝试退出 Boot Services
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      // 两次尝试均失败，无法继续引导，挂死
      Print(L"Could not exit boot service: %r\n", status);
      while (1);
    }
  }
 
  // 至此 Boot Services 已成功退出，不得再调用任何 gBS->xxx() 函数
  // #@@range_end(exit_bs)
 
  /* ── 阶段 6：跳转到内核入口点 ───────────────────────────────────────── */
  // #@@range_begin(call_kernel)
 
  // 从 ELF64 文件头中读取入口点虚拟地址（e_entry 字段）。
  //
  // ELF64 文件头（Elf64_Ehdr）布局（字节偏移）：
  //   偏移  0：e_ident[16]  —— 魔数 "\x7fELF" + 类型/字节序/版本等
  //   偏移 16：e_type       (2 字节) —— 文件类型（ET_EXEC=2 可执行文件）
  //   偏移 18：e_machine    (2 字节) —— 目标架构（EM_X86_64=62）
  //   偏移 20：e_version    (4 字节) —— ELF 版本（固定为 1）
  //   偏移 24：e_entry      (8 字节) —— 程序入口点虚拟地址  ← 我们要读取的字段
  //
  // 因此 *(UINT64*)(kernel_base_addr + 24) 即为内核入口点地址。
  // 此处假设内核使用身份映射（虚拟地址 == 物理地址），故可直接作为函数指针调用。
  UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);
 
  // 将入口地址强制转换为函数指针类型并调用。
  // 内核不会返回（它会接管 CPU 并运行自己的无限事件循环），
  // 所以函数签名声明为返回 void。
  typedef void EntryPointType(void);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point();  // 跳入内核，从此引导程序的使命完成
  // #@@range_end(call_kernel)
 
  /* ── 阶段 7：不可达代码（防止编译器警告） ───────────────────────────── */
 
  // 以下代码在正常流程中永远不会执行（内核不返回）。
  // 保留 Print 和 return 是为了：
  //   1. 满足编译器对返回值的要求（函数签名声明返回 EFI_STATUS）
  //   2. 如果内核意外返回，提供一个明确的错误指示，而非随机执行垃圾指令
  Print(L"All done\n");
 
  // 内核意外返回时的安全挂死：防止 CPU 滑入未知内存执行任意代码
  while (1);
  return EFI_SUCCESS;
}
 
 