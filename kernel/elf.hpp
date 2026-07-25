/**
 * elf.hpp — ELF64 文件格式数据结构定义（内核自用精简版）
 *
 * ── 什么是 ELF？──────────────────────────────────────────────────────────────
 *
 *   ELF（Executable and Linkable Format，可执行与可链接格式）是 Linux / Unix
 *   系统上可执行文件、共享库和目标文件的标准容器格式，同时也是 MikanOS 内核
 *   镜像（kernel.elf）所采用的格式。
 *
 *   一个 ELF 文件由以下几层视图组成：
 *
 *   ┌─────────────────┐
 *   │   ELF 文件头    │  Elf64_Ehdr — 描述文件类型、目标架构、入口地址、
 *   │  (ELF Header)   │              程序头表 / 节头表在文件中的偏移
 *   ├─────────────────┤
 *   │  程序头表       │  Elf64_Phdr[] — 运行时视图（Segment）：
 *   │ (Program Hdr)   │               告诉操作系统/引导程序"把哪段内容装到内存的哪里"
 *   ├─────────────────┤
 *   │  各节内容       │  .text / .rodata / .data / .bss / .rela.dyn 等
 *   │  (Sections)     │
 *   ├─────────────────┤
 *   │  节头表         │  Elf64_Shdr[] — 链接时视图（Section），加载器通常不需要
 *   │ (Section Hdr)   │
 *   └─────────────────┘
 *
 *   MikanOS 引导程序（UefiMain）通过以下步骤使用本头文件中的结构体：
 *     1. 读取 Elf64_Ehdr.e_entry         → 内核入口点地址（跳转目标）
 *     2. 读取 Elf64_Ehdr.e_phoff/e_phnum → 定位程序头表
 *     3. 遍历 Elf64_Phdr[]，加载 PT_LOAD segment 到对应物理地址
 *     4. 处理 .rela.dyn（Elf64_Rela）    → 对 RELATIVE 重定位条目进行地址修正
 *
 * ── 本文件来源与规范依据 ────────────────────────────────────────────────────
 *
 *   数据结构取自 System V Application Binary Interface AMD64 Architecture
 *   Processor Supplement（通称 System V AMD64 ABI），与 /usr/include/elf.h
 *   中的定义完全兼容，仅保留内核加载所需的最小子集。
 *
 * ── ELF64 vs ELF32 ──────────────────────────────────────────────────────────
 *
 *   ELF64 为 64 位系统设计，地址/偏移量宽度从 32 位扩展到 64 位，
 *   同时程序头字段顺序与 ELF32 不同（p_flags 移至 p_offset 之前）。
 *   本文件仅定义 ELF64 结构体，不兼容 ELF32。
 */
 
#pragma once
 
#include <stdint.h>
 
/* ============================================================================
 * 一、ELF64 基本类型别名
 * ============================================================================
 *
 * ELF 规范定义了一套与平台无关的类型名称，映射到 C99 固定宽度整数类型。
 * 使用专属名称的好处：
 *   ① 代码与字段语义紧密对应（Addr=地址、Off=偏移、Half=半字），可读性高
 *   ② 将来移植到其他宽度（如 32 位 ELF）只需修改类型别名，无需改结构体字段
 *
 * 各类型位宽与有无符号速查：
 *   类型名           位宽   有无符号   用途
 *   Elf64_Addr        64     无        虚拟地址 / 物理地址
 *   Elf64_Off         64     无        文件内字节偏移
 *   Elf64_Half        16     无        短整数字段（节数、机器类型等）
 *   Elf64_Word        32     无        中等整数字段（标志、版本等）
 *   Elf64_Sword       32     有        带符号 32 位整数（保留字段）
 *   Elf64_Xword       64     无        扩展无符号（大小、对齐等）
 *   Elf64_Sxword      64     有        扩展有符号（重定位 addend 等）
 */
 
/** 64 位无符号虚拟/物理地址；sizeof == 8；uintptr_t 保证与指针等宽 */
typedef uintptr_t Elf64_Addr;
 
/** 64 位无符号文件偏移量（从文件头起始处计算的字节数）；sizeof == 8 */
typedef uint64_t  Elf64_Off;
 
/** 16 位无符号半字（half-word）；用于类型、机器码、节数等小整数字段；sizeof == 2 */
typedef uint16_t  Elf64_Half;
 
/** 32 位无符号字（word）；用于标志、版本、名称索引等中等整数字段；sizeof == 4 */
typedef uint32_t  Elf64_Word;
 
/** 32 位有符号字（signed word）；ELF 规范预留，当前结构体中未直接使用；sizeof == 4 */
typedef int32_t   Elf64_Sword;
 
/** 64 位无符号扩展字（extended word）；用于 segment 大小、对齐等需要大范围无符号值的字段；sizeof == 8 */
typedef uint64_t  Elf64_Xword;
 
/** 64 位有符号扩展字（signed extended word）；重定位条目的 addend 字段需要负数，故用有符号类型；sizeof == 8 */
typedef int64_t   Elf64_Sxword;
 
/* ============================================================================
 * 二、ELF 标识字节数组长度
 * ============================================================================ */
 
/**
 * EI_NIDENT — e_ident 数组的固定长度（16 字节）
 *
 * e_ident 是 ELF 文件头最开始的 16 字节魔数+元数据区，其内部各字节含义：
 *
 *   索引  名称            含义
 *   [0]  EI_MAG0        魔数字节 0：固定为 0x7F
 *   [1]  EI_MAG1        魔数字节 1：固定为 'E' (0x45)
 *   [2]  EI_MAG2        魔数字节 2：固定为 'L' (0x4C)
 *   [3]  EI_MAG3        魔数字节 3：固定为 'F' (0x46)
 *   [4]  EI_CLASS       文件类：1=ELF32，2=ELF64
 *   [5]  EI_DATA        字节序：1=小端（ELFDATA2LSB），2=大端
 *   [6]  EI_VERSION     ELF 版本：固定为 1（EV_CURRENT）
 *   [7]  EI_OSABI       目标 OS/ABI：0=System V（通用 Linux 也用此值）
 *   [8]  EI_ABIVERSION  ABI 版本：通常为 0
 *   [9..15] EI_PAD      填充字节，必须为 0，保留供将来扩展
 *
 * 验证方法：hexdump -C kernel.elf | head -1
 * 期望输出：7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
 *           └─ ELF ──┘ 64 LE SV  └──────── 填充 ─────────┘
 */
#define EI_NIDENT 16
 
/* ============================================================================
 * 三、ELF64 文件头（Elf64_Ehdr）
 * ============================================================================
 *
 * 位于 ELF 文件的绝对开头（偏移 0），总大小固定为 64 字节。
 * 用途：描述文件整体属性，提供程序头表和节头表的位置/数量/大小。
 *
 * 内存布局（小端 x86-64）：
 *   偏移  大小  字段名        本项目中的典型值
 *    0    16   e_ident       7F 45 4C 46 02 01 01 00 ...
 *   16     2   e_type        ET_EXEC(2) 或 ET_DYN(3)（PIE 时为 3）
 *   18     2   e_machine     EM_X86_64 = 62 = 0x003E
 *   20     4   e_version     1（EV_CURRENT）
 *   24     8   e_entry       内核入口点虚拟地址（UefiMain 从此跳转）
 *   32     8   e_phoff       程序头表在文件中的偏移（通常紧接文件头，= 64）
 *   40     8   e_shoff       节头表在文件中的偏移（加载器不需要节头表）
 *   48     4   e_flags       目标平台相关标志（x86-64 固定为 0）
 *   52     2   e_ehsize      ELF 文件头字节数（x86-64 固定为 64）
 *   54     2   e_phentsize   单个程序头条目字节数（Elf64_Phdr 固定为 56）
 *   56     2   e_phnum       程序头条目数量
 *   58     2   e_shentsize   单个节头条目字节数（Elf64_Shdr 固定为 64）
 *   60     2   e_shnum       节头条目数量
 *   62     2   e_shstrndx    节名字符串表在节头表中的索引
 *  （合计 64 字节）
 */
typedef struct {
  /**
   * e_ident — ELF 标识字节数组，共 16 字节
   *
   * 最初 4 字节为 ELF 魔数（"\x7fELF"），用于识别文件格式；
   * 后续字节编码文件类（32/64位）、字节序、版本、ABI 类型。
   * 加载器应首先验证魔数和 EI_CLASS，若不匹配则拒绝加载。
   */
  unsigned char e_ident[EI_NIDENT];
 
  /**
   * e_type — ELF 文件类型
   *
   * 常用值：
   *   ET_NONE   0  未知类型
   *   ET_REL    1  可重定位目标文件（.o）
   *   ET_EXEC   2  可执行文件（静态链接，加载到固定地址）
   *   ET_DYN    3  共享目标文件 / PIE 可执行文件
   *               （地址无关；内核若以 -pie 编译则为此值）
   *   ET_CORE   4  核心转储文件
   *
   * MikanOS 内核通常为 ET_EXEC（固定加载到 0x100000）；
   * 若启用 PIE（位置无关可执行），则为 ET_DYN，需处理 RELATIVE 重定位。
   */
  Elf64_Half    e_type;
 
  /**
   * e_machine — 目标指令集架构
   *
   * 常用值：
   *   EM_NONE    0   未指定
   *   EM_386     3   Intel 80386（32 位 x86）
   *   EM_X86_64  62  AMD64 / Intel 64（本项目使用）
   *   EM_AARCH64 183 ARM 64 位
   *
   * 加载器应检查此字段确保架构匹配，避免将 ARM 内核加载到 x86 机器上。
   */
  Elf64_Half    e_machine;
 
  /**
   * e_version — ELF 格式版本号
   *
   * 当前规范唯一合法值为 1（EV_CURRENT）；
   * 为 0（EV_NONE）表示无效文件，应拒绝加载。
   */
  Elf64_Word    e_version;
 
  /**
   * e_entry — 程序入口点虚拟地址（8 字节，偏移 24）
   *
   * 内核加载完成后，引导程序将跳转到此地址开始执行。
   * UefiMain 通过以下代码读取：
   *   UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);
   *
   * 对于 MikanOS，此地址对应 kernel/main.cpp 中的 KernelMain 函数，
   * 或更准确地说，链接脚本中指定的 _start / ENTRY 符号所在地址。
   *
   * 若文件类型为 ET_DYN（PIE），e_entry 是相对于加载基址的偏移；
   * 实际跳转地址 = 加载基址 + e_entry。
   */
  Elf64_Addr    e_entry;
 
  /**
   * e_phoff — 程序头表（Program Header Table）在文件中的字节偏移
   *
   * 程序头表是一个 Elf64_Phdr 数组，描述运行时 segment 布局。
   * 典型值：64（紧接在文件头之后）。
   * 若 e_phnum == 0（无程序头），此字段无意义。
   *
   * 遍历程序头表：
   *   Elf64_Phdr* phdrs = (Elf64_Phdr*)(base + ehdr->e_phoff);
   *   for (int i = 0; i < ehdr->e_phnum; i++) { ... phdrs[i] ... }
   */
  Elf64_Off     e_phoff;
 
  /**
   * e_shoff — 节头表（Section Header Table）在文件中的字节偏移
   *
   * 节头表供链接器和调试器使用，运行时加载器通常不需要。
   * 若 e_shnum == 0 或 e_shoff == 0，表示无节头表（剥离符号的发行版镜像）。
   */
  Elf64_Off     e_shoff;
 
  /**
   * e_flags — 处理器特定标志位掩码
   *
   * x86-64 目标文件规范要求此字段恒为 0；
   * 部分 RISC 架构（如 ARM、RISC-V）会在此字段编码 ABI 版本、浮点调用约定等信息。
   */
  Elf64_Word    e_flags;
 
  /**
   * e_ehsize — ELF 文件头自身的字节大小
   *
   * ELF64 固定为 64，用于版本兼容性检查（若值不符合预期则拒绝解析）。
   * 注意：这是文件头的大小，不是整个文件的大小。
   */
  Elf64_Half    e_ehsize;
 
  /**
   * e_phentsize — 单个程序头条目的字节大小
   *
   * 即 sizeof(Elf64_Phdr) = 56 字节（ELF64 固定值）。
   * 遍历程序头表时应使用此值作为步长，而非硬编码 sizeof，
   * 以应对未来规范扩展新字段的可能。
   */
  Elf64_Half    e_phentsize;
 
  /**
   * e_phnum — 程序头表中条目的数量（即 segment 数量）
   *
   * 程序头表的总字节数 = e_phnum × e_phentsize。
   * 为 0 表示无程序头表（纯目标文件，无法直接执行）。
   * 若值为 PN_XNUM（0xFFFF），实际数量存储在节头表第 0 条目的 sh_info 字段中。
   */
  Elf64_Half    e_phnum;
 
  /**
   * e_shentsize — 单个节头条目的字节大小
   *
   * ELF64 固定为 64 字节（Elf64_Shdr 的大小）；
   * 用于计算节头表总大小和遍历步长。
   */
  Elf64_Half    e_shentsize;
 
  /**
   * e_shnum — 节头表中条目的数量（即节的数量）
   *
   * 为 0 表示无节头表；
   * 若值为 0 且 e_shoff != 0，实际数量在节头表第 0 条目的 sh_size 字段中。
   */
  Elf64_Half    e_shnum;
 
  /**
   * e_shstrndx — 节名字符串表（.shstrtab）所在节头表的索引
   *
   * 节名字符串表是一个连续字符串缓冲区，每个节头通过 sh_name（字节偏移）
   * 索引自己在该缓冲区中的名称字符串（如 ".text\0" ".data\0"）。
   * 值为 SHN_UNDEF（0）表示无节名字符串表（已剥离符号的文件）。
   * 加载内核时此字段通常不需要读取。
   */
  Elf64_Half    e_shstrndx;
} Elf64_Ehdr;
 
/* ============================================================================
 * 四、ELF64 程序头（Elf64_Phdr）— 运行时 Segment 描述符
 * ============================================================================
 *
 * 程序头描述一个 segment（段），即运行时需要装入内存的连续数据块。
 * 与 section（节）不同，section 是链接时的逻辑分组单位，
 * 而 segment 是运行时的物理内存映射单位，多个 section 可合并为一个 segment。
 *
 * ELF64 中 Elf64_Phdr 的字段顺序与 ELF32 不同：
 *   p_flags 在 ELF64 中紧接 p_type（偏移 4），而 ELF32 中在结构末尾。
 *   这样安排使 64 位地址字段自然对齐到 8 字节边界，无需填充。
 *
 * 单个程序头大小：sizeof(Elf64_Phdr) = 56 字节。
 *
 * 内存布局：
 *   偏移  大小  字段名
 *    0     4   p_type    segment 类型（PT_LOAD、PT_DYNAMIC 等）
 *    4     4   p_flags   权限标志（读/写/执行）
 *    8     8   p_offset  segment 在文件中的字节偏移
 *   16     8   p_vaddr   segment 在内存中的虚拟地址
 *   24     8   p_paddr   segment 在内存中的物理地址（嵌入式系统用）
 *   32     8   p_filesz  segment 在文件中占用的字节数
 *   40     8   p_memsz   segment 在内存中占用的字节数（≥ p_filesz）
 *   48     8   p_align   segment 对齐要求（2 的幂次，0/1 表示无要求）
 */
typedef struct {
  /**
   * p_type — segment 类型
   *
   * 决定加载器如何处理本程序头，常见值见下方 PT_* 宏定义。
   * 不认识的类型应忽略跳过（向前兼容）。
   */
  Elf64_Word  p_type;
 
  /**
   * p_flags — segment 访问权限标志位掩码
   *
   * 位定义：
   *   PF_X  0x1  可执行（Executable）— 代码段
   *   PF_W  0x2  可写（Writable）    — 数据段
   *   PF_R  0x4  可读（Readable）    — 几乎所有段都有此位
   *
   * 典型组合：
   *   0x5（PF_R|PF_X）→ 代码段 .text
   *   0x6（PF_R|PF_W）→ 数据段 .data / .bss
   *   0x4（PF_R）     → 只读数据段 .rodata
   *
   * 操作系统在 mmap/mprotect 时将这些标志转换为页表权限位（NX、写保护等）。
   * MikanOS 目前运行在实地址平等映射下，权限标志暂时不强制执行。
   */
  Elf64_Word  p_flags;
 
  /**
   * p_offset — segment 内容在 ELF 文件中的字节偏移
   *
   * 从文件绝对起始处（偏移 0）计算。
   * 加载 segment 时从文件的此位置读取 p_filesz 个字节。
   *
   * 与 p_vaddr 的关系（PT_LOAD 对齐约束）：
   *   (p_offset % p_align) == (p_vaddr % p_align)
   * 即文件偏移和虚拟地址必须以相同方式对齐到 p_align 边界，
   * 保证 mmap 时文件页面直接对应虚拟内存页面。
   */
  Elf64_Off   p_offset;
 
  /**
   * p_vaddr — segment 在虚拟地址空间中的加载地址
   *
   * 对于 ET_EXEC（固定地址可执行文件）：此为实际加载虚拟地址（绝对值）。
   * 对于 ET_DYN（PIE / 共享库）：此为相对于加载基址的偏移；
   *   实际加载地址 = 加载基址 + p_vaddr。
   *
   * 加载器负责将文件中 p_offset 处的数据复制到虚拟地址 p_vaddr 处。
   */
  Elf64_Addr  p_vaddr;
 
  /**
   * p_paddr — segment 在物理地址空间中的加载地址
   *
   * 在启用虚拟内存的通用操作系统中此字段通常被忽略（与 p_vaddr 相同）；
   * 嵌入式系统或裸机加载器（如 MikanOS 引导阶段）在分页尚未开启时，
   * 可能使用此字段作为实际写入目标地址。
   * 本项目当前直接使用 p_vaddr 作为物理地址（恒等映射假设）。
   */
  Elf64_Addr  p_paddr;
 
  /**
   * p_filesz — segment 在 ELF 文件中占用的字节数（可为 0）
   *
   * 实际从文件复制的字节数。
   * .bss 节（未初始化数据）不占文件空间，因此其所在 segment 的
   *   p_filesz < p_memsz；多出的字节（p_memsz - p_filesz）在内存中清零。
   */
  Elf64_Xword p_filesz;
 
  /**
   * p_memsz — segment 在内存中占用的字节数（≥ p_filesz）
   *
   * 加载步骤：
   *   1. 分配 p_memsz 字节（对齐到 p_align 页边界）
   *   2. 将文件 p_offset 处的 p_filesz 字节复制到 p_vaddr
   *   3. 将 [p_vaddr + p_filesz, p_vaddr + p_memsz) 范围清零
   *      （处理 .bss 未初始化数据段）
   */
  Elf64_Xword p_memsz;
 
  /**
   * p_align — segment 在内存和文件中的对齐要求
   *
   * 值为 2 的幂次（如 0x1000 = 4096 表示页对齐）；
   * 0 或 1 表示无对齐要求。
   * 加载器分配内存时应对齐到此边界，以满足 CPU 缓存行和页面映射的要求。
   * x86-64 内核通常要求 2 MiB 大页对齐（p_align = 0x200000）。
   */
  Elf64_Xword p_align;
} Elf64_Phdr;
 
/* ============================================================================
 * 五、程序头 segment 类型常量（PT_*）
 * ============================================================================
 *
 * 加载器通过 p_type 字段判断如何处理每个 segment；未知类型应忽略。
 */
 
/**
 * PT_NULL — 空/未使用条目
 *
 * 程序头表中的占位条目，所有字段均为 0，加载器应直接跳过。
 * 常见于对齐填充或删除 segment 后留下的空槽。
 */
#define PT_NULL    0
 
/**
 * PT_LOAD — 可加载 segment（最重要的类型）
 *
 * 指示加载器将此 segment 的内容从文件复制到内存中的指定地址。
 * 一个 ELF 可执行文件通常有 2~3 个 PT_LOAD segment：
 *   ① 只读代码段（.text + .rodata，权限 R+X）
 *   ② 读写数据段（.data + .bss，权限 R+W）
 *
 * MikanOS 引导程序遍历程序头表时只需处理此类型；
 * 加载后跳转到 Elf64_Ehdr.e_entry 开始执行。
 */
#define PT_LOAD    1
 
/**
 * PT_DYNAMIC — 动态链接信息 segment
 *
 * 指向 .dynamic 节，包含动态链接器所需的元数据（共享库依赖、重定位表地址等）。
 * 内容为 Elf64_Dyn 数组，以 DT_NULL 条目终止。
 *
 * MikanOS 内核若以 PIE 方式编译，会出现此 segment；
 * 引导程序需从中定位 DT_RELA 重定位表并完成地址修正。
 */
#define PT_DYNAMIC 2
 
/**
 * PT_INTERP — 程序解释器路径 segment
 *
 * 包含动态链接器路径字符串（如 "/lib/ld-linux-x86-64.so.2"）。
 * 内核运行时不依赖动态链接器，此 segment 在 MikanOS 中通常不存在。
 */
#define PT_INTERP  3
 
/**
 * PT_NOTE — 辅助信息 segment
 *
 * 包含供操作系统、调试器读取的辅助元数据（构建 ID、OS 版本要求等）。
 * 加载器通常不需要处理，可安全跳过。
 */
#define PT_NOTE    4
 
/**
 * PT_SHLIB — 保留类型（历史遗留，未定义语义）
 *
 * ELF 规范明确声明此类型"保留"且语义未定义；
 * 包含此类型程序头的文件不符合 ABI 规范，应视为异常文件。
 */
#define PT_SHLIB   5
 
/**
 * PT_PHDR — 程序头表自身的 segment
 *
 * 描述程序头表在文件和内存中的位置与大小，
 * 使程序在运行时可以自我查询程序头表（如动态链接器初始化时）。
 * 若存在，必须位于所有 PT_LOAD segment 之前。
 */
#define PT_PHDR    6
 
/**
 * PT_TLS — 线程局部存储（Thread-Local Storage）模板 segment
 *
 * 描述 TLS 初始化镜像（.tdata / .tbss）的位置和大小；
 * 线程创建时将此模板复制到线程专属存储区，实现每线程独立的全局变量。
 * 单线程的 MikanOS 内核暂不使用 TLS，此类型条目可忽略。
 */
#define PT_TLS     7
 
/* ============================================================================
 * 六、ELF64 动态节条目（Elf64_Dyn）
 * ============================================================================
 *
 * .dynamic 节由 Elf64_Dyn 数组构成，数组以 d_tag == DT_NULL 的条目终止。
 * 动态节是运行时动态链接器（或裸机加载器）的"配置清单"，
 * 记录重定位表地址、共享库依赖列表、符号表地址等关键信息。
 *
 * 使用方式（遍历动态节）：
 *   Elf64_Dyn* dyn = (Elf64_Dyn*)(base + dynamic_segment_offset);
 *   for (; dyn->d_tag != DT_NULL; dyn++) {
 *     switch (dyn->d_tag) {
 *       case DT_RELA:  rela_addr = dyn->d_un.d_ptr; break;
 *       case DT_RELASZ: rela_size = dyn->d_un.d_val; break;
 *       ...
 *     }
 *   }
 */
typedef struct {
  /**
   * d_tag — 条目类型标签，决定 d_un 的解释方式
   *
   * 有符号 64 位整数；常用值见下方 DT_* 宏定义。
   * 正值为规范定义的标准标签；负值和大正值为操作系统 / 处理器特定扩展。
   * 遍历时以 d_tag == DT_NULL（0）为终止条件。
   */
  Elf64_Sxword d_tag;
 
  /**
   * d_un — 联合体，根据 d_tag 语义选择解释方式
   *
   * d_val：整数值（大小、数量、标志位）
   *   用于 DT_RELASZ（重定位表字节数）、DT_RELAENT（单条目字节数）等。
   *
   * d_ptr：虚拟地址指针（加载基址 + 相对偏移）
   *   用于 DT_RELA（重定位表起始地址）等。
   *   ETL_DYN 文件中，d_ptr 存储的是运行时加载基址加上偏移的值；
   *   若内核以固定地址加载（ET_EXEC），d_ptr 即为绝对地址。
   */
  union {
    Elf64_Xword d_val;   /* 无符号整数值 */
    Elf64_Addr  d_ptr;   /* 虚拟地址指针 */
  } d_un;
} Elf64_Dyn;
 
/* ============================================================================
 * 七、动态节标签常量（DT_*）— 本项目使用的最小子集
 * ============================================================================ */
 
/**
 * DT_NULL — 动态节数组终止符
 *
 * d_tag == 0 标志数组结束；遍历时必须检查此条件。
 * 对应的 d_un 无意义，忽略。
 */
#define DT_NULL    0
 
/**
 * DT_RELA — .rela.dyn / .rela.plt 重定位表的虚拟地址
 *
 * d_un.d_ptr 指向 Elf64_Rela 数组的起始地址（运行时虚拟地址）。
 * 配合 DT_RELASZ 确定重定位表边界：
 *   Elf64_Rela* rela = (Elf64_Rela*)dyn[DT_RELA].d_un.d_ptr;
 *   size_t count = dyn[DT_RELASZ].d_un.d_val / sizeof(Elf64_Rela);
 *
 * 注意：RELA 与 REL 的区别——RELA 条目显式包含 addend 字段，
 * REL 的 addend 隐含在目标地址处的原始值中；x86-64 统一使用 RELA。
 */
#define DT_RELA    7
 
/**
 * DT_RELASZ — 重定位表的总字节数
 *
 * d_un.d_val 为 Elf64_Rela 数组占用的字节总数。
 * 条目数 = DT_RELASZ / DT_RELAENT（两者结合计算）。
 */
#define DT_RELASZ  8
 
/**
 * DT_RELAENT — 单个重定位条目的字节大小
 *
 * d_un.d_val 应等于 sizeof(Elf64_Rela) = 24；
 * 用于版本兼容性检查，同时作为遍历重定位表的步长。
 */
#define DT_RELAENT 9
 
/* ============================================================================
 * 八、ELF64 重定位条目（Elf64_Rela）— 带显式加数的重定位
 * ============================================================================
 *
 * 重定位（Relocation）是链接器或加载器在最终确定加载地址后，
 * 修正代码/数据中引用绝对地址的过程。
 *
 * Elf64_Rela 描述一处需要修正的地址引用：
 *   • 修正位置（r_offset）
 *   • 修正方式（r_info 中的 type）
 *   • 参考符号（r_info 中的 sym，RELATIVE 重定位时为 0）
 *   • 加数（r_addend）
 *
 * 修正公式（以 R_X86_64_RELATIVE 为例）：
 *   *(uint64_t*)(加载基址 + r_offset) = 加载基址 + r_addend;
 *
 * sizeof(Elf64_Rela) = 24 字节：
 *   偏移  大小  字段
 *    0     8   r_offset
 *    8     8   r_info
 *   16     8   r_addend
 */
typedef struct {
  /**
   * r_offset — 需要修正的内存位置的虚拟地址（相对偏移或绝对地址）
   *
   * 对于 ET_DYN（PIE / 共享库）：r_offset 是相对于加载基址的偏移；
   *   实际修正地址 = 加载基址 + r_offset。
   * 对于 ET_EXEC（固定地址）：r_offset 是虚拟地址的绝对值。
   *
   * 修正操作就是向此地址写入计算结果（通常是 8 字节指针大小的值）。
   */
  Elf64_Addr   r_offset;
 
  /**
   * r_info — 重定位类型与符号索引的打包字段（64 位）
   *
   * 高 32 位：符号表索引（ELF64_R_SYM 宏提取）
   *   对于 R_X86_64_RELATIVE 类型，此值为 0（不引用特定符号，
   *   修正值仅依赖加载基址和 r_addend，无需查符号表）。
   *   其他类型（如 R_X86_64_GLOB_DAT）需用此索引在 .dynsym 中查找符号地址。
   *
   * 低 32 位：重定位类型（ELF64_R_TYPE 宏提取）
   *   决定修正公式，本文件只使用 R_X86_64_RELATIVE（值为 8）。
   */
  Elf64_Xword  r_info;
 
  /**
   * r_addend — 修正计算中的加数（有符号 64 位）
   *
   * 对于 R_X86_64_RELATIVE，修正公式为：
   *   *target = base_addr + r_addend
   * 其中 r_addend 通常是链接时的虚拟地址（假设加载到 0 时的地址）。
   *
   * 有符号设计允许 addend 为负数（如地址修正为 base - 某偏移量的场景）。
   */
  Elf64_Sxword r_addend;
} Elf64_Rela;
 
/* ============================================================================
 * 九、r_info 字段打包/解包宏（ELF64_R_*）
 * ============================================================================ */
 
/**
 * ELF64_R_SYM(i) — 从 r_info 中提取符号表索引（高 32 位）
 *
 * 右移 32 位取高 32 位，得到引用的 .dynsym/.symtab 中的符号索引。
 * 对于 R_X86_64_RELATIVE 类型，结果为 0（无关联符号）。
 *
 * 示例：r_info = 0x0000000000000008 → ELF64_R_SYM = 0, ELF64_R_TYPE = 8
 */
#define ELF64_R_SYM(i)    ((i)>>32)
 
/**
 * ELF64_R_TYPE(i) — 从 r_info 中提取重定位类型（低 32 位）
 *
 * 掩码 0xFFFFFFFF 取低 32 位，得到重定位类型编号。
 * 本项目中预期值为 R_X86_64_RELATIVE（8）。
 */
#define ELF64_R_TYPE(i)   ((i)&0xffffffffL)
 
/**
 * ELF64_R_INFO(s,t) — 将符号索引 s 和重定位类型 t 打包成 r_info 值
 *
 * s 左移 32 位放高 32 位，t 取低 32 位，二者按位或合并。
 * 主要供链接器写 ELF 文件时使用；加载器一般只需 R_SYM / R_TYPE 解包。
 */
#define ELF64_R_INFO(s,t) (((s)<<32)+((t)&0xffffffffL))
 
/* ============================================================================
 * 十、x86-64 重定位类型常量（R_X86_64_*）— 本项目使用的最小子集
 * ============================================================================ */
 
/**
 * R_X86_64_RELATIVE — 基址相对重定位（值为 8）
 *
 * 这是 PIE（位置无关可执行文件）和共享库最常见的重定位类型，
 * 用于修正所有在链接时被假定加载到地址 0 的绝对指针。
 *
 * 触发场景：
 *   C++ 全局对象的虚函数表指针（vptr）、函数指针全局变量、
 *   静态初始化器中引用的全局地址等。
 *
 * 修正公式：
 *   *((uint64_t*)(load_base + r_offset)) = load_base + r_addend;
 *
 *   • load_base：加载器将内核 ELF 实际加载到的物理/虚拟起始地址
 *                （对于固定加载到 0x100000 的 MikanOS，load_base = 0x100000）
 *   • r_offset： 被修正指针在文件映像中的偏移（相对于 load_base）
 *   • r_addend： 链接时假设基址为 0 时该指针的值（即原始链接地址）
 *
 * 工作原理举例（假设 load_base = 0x100000）：
 *   链接时生成：某全局变量 g 的地址 = 0x201234（假设加载到 0x200000 处）
 *   r_addend = 0x201234
 *   加载时修正：*(ptr) = 0x100000 + 0x201234 = 0x301234
 *                               ↑实际基址偏移   ↑链接器写入的原始地址偏移
 *
 * 注意：若内核以 -no-pie 链接（ET_EXEC，固定地址），则不会生成此类重定位，
 * 引导程序也无需执行重定位步骤。
 */
#define R_X86_64_RELATIVE 8
 