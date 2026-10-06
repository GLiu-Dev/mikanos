# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application

`osbook_day04d` 的改动完全集中在**引导加载器按 ELF LOAD 段来定位和加载内核**，下面是详细解析。

---

# osbook_day04d 改动全解析（osbook_day04c → osbook_day04d）

## 0. 总览

| 项 | 内容 |
| --- | --- |
| 主题 | **locate kernel according to LOAD segments**（按 LOAD 段定位内核） |
| 提交 | `c19f663`(核心) + `3246d38`(清理) |
| 变更文件 | 3 个，+140 / −12 |
| 内核代码 | **`kernel/main.cpp` 零改动** |

```
 MikanLoaderPkg/Main.c  | 76 +++++++++++++++++++++++++++++++++++--------
 MikanLoaderPkg/elf.hpp |  1 +   （符号链接 → ../kernel/elf.hpp）
 kernel/elf.hpp         | 75 ++++ （新增，ELF64 格式定义）
```

## 1. 提交分解

**① `3246d38` remove review tags（4 行删除，无逻辑变化）**
只删掉了 Main.c 里两处书中标记 `#@@range_begin/end(alloc_error)` 和 `#@@range_begin/end(exit_bs)`。

**② `c19f663` locate kernel according to LOAD segments（+140/−8，真正的主体）**
新增 `kernel/elf.hpp` 及其符号链接，并把加载器里「固定地址读内核」的代码全部重写为「解析 ELF、按段加载」。

## 2. 新增 `kernel/elf.hpp` —— 手写 ELF64 格式

75 行，把内核解析 ELF 所需的类型与结构完整定义出来：

| 定义 | 内容 |
| --- | --- |
| 基础类型 | `Elf64_Addr/Off/Half/Word/Xword/Sxword` 等（`uintptr_t`/`uint64_t`…） |
| `Elf64_Ehdr` | ELF 文件头：`e_entry`(入口)、`e_phoff`(程序头偏移)、`e_phnum`(程序头数量) 等 |
| `Elf64_Phdr` | 程序段头：`p_type/p_offset/p_vaddr/p_filesz/p_memsz` |
| 段类型常量 | `PT_NULL/PT_LOAD/PT_DYNAMIC/PT_INTERP/PT_NOTE/PT_SHLIB/PT_PHDR/PT_TLS` |
| 动态结构 | `Elf64_Dyn`、`Elf64_Rela`（重定位项）及 `R_X86_64_RELATIVE` |

> 
> 注意：`Elf64_Rela`/`DT_RELA`/`R_X86_64_RELATIVE` 这批**动态重定位**结构本天还没被使用，是预置接口，为后续引入重定位预留。

`MikanLoaderPkg/elf.hpp` 依旧是 1 行符号链接指向 `../kernel/elf.hpp`，让 C 的加载器与 C++ 内核共享同一份格式定义。

## 3. `Main.c` 加载流程重写 —— 新旧对比

### 旧逻辑（day04c）

```
固定 kernel_base_addr = 0x100000
→ AllocatePages(固定地址, 按文件大小分页)
→ 直接把文件内容 Read 到 0x100000
→ entry = *(UINT64*)(base + 24)   // 假定 ELF 头就在 0x100000
```

缺点：假设内核一定从 1MB 开始、且 ELF 头在文件最前，不通用。

### 新逻辑（day04d）

```
① AllocatePool 读入整个内核文件到临时 kernel_buffer
② 把 buffer 强转为 Elf64_Ehdr*，解析
③ CalcLoadAddressRange() 算出所有 LOAD 段的地址范围 [first, last)
④ AllocatePages(AllocateAddress, 按范围大小分页, first_addr)
⑤ CopyLoadSegments() 把各 LOAD 段拷贝到目标地址（并清 bss）
⑥ FreePool 释放临时 buffer
⑦ ExitBootServices
⑧ entry = *(UINT64*)(kernel_first_addr + 24)   // 从 ELF 头 e_entry 读入口
⑨ 组装 FrameBufferConfig → 跳转
```

### 两个核心新函数

```
void CalcLoadAddressRange(Elf64_Ehdr* ehdr, UINT64* first, UINT64* last) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  *first = MAX_UINT64; *last = 0;
  for (i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    *first = MIN(*first, phdr[i].p_vaddr);
    *last  = MAX(*last,  phdr[i].p_vaddr + phdr[i].p_memsz);
  }
}
```

遍历全部程序段头，只统计 `PT_LOAD` 段，取「最低起始地址」到「最高结束地址」，得到内核占用的连续地址区间。

```
void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  ...
  for (i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz); // 拷段
    UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;
    SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0);   // 清bss
  }
}
```

逐段把文件里的段拷贝到各自 `p_vaddr`，再用 `SetMem` 把 `memsz − filesz` 的部分清零——**这正是 bss 段（未初始化全局区）的处理**，是正规加载器必须做的一步。

## 4. 几个关键点

1. **为什么改成两段式（先 Pool 读文件、再 Pages 拷贝）**：临时用 `AllocatePool`（任意地址的小块内存）读文件解析，之后才按解析出的 `vaddr` 用 `AllocatePages(AllocateAddress)` 在精确地址分配，用完 `FreePool` 释放临时 buffer。内核最终加载到 ELF 自己声明的地址，而不是固定的 1MB。
2. **入口地址读取**：`entry_addr = *(UINT64*)(kernel_first_addr + 24)` —— ELF 文件头 `e_entry` 字段恰好位于偏移 24，且 ELF 头属于第一个 LOAD 段、已被拷入 `kernel_first_addr`，因此直接在该地址读取入口点。
3. **新增 include**：`<Library/BaseMemoryLib.h>`（提供 `CopyMem`/`SetMem`）。
4. **行为上**：因为 Makefile 仍用 `--image-base 0x100000`，首个 LOAD 段的 `p_vaddr` 实际还是 1MB，所以功能结果不变，但加载方式从「猜固定地址」变成了「按 ELF 声明的段地址正规加载」——为内核体积变大、出现多段后仍能正确加载打基础。

## 5. 一句话总结

**osbook_day04d 是引导加载器的「正规化」提交**：新增 `kernel/elf.hpp` 的 ELF64 定义，让加载器不再把内核硬塞到固定 1MB，而是解析 ELF 头、按 `PT_LOAD` 段计算地址范围并拷贝（含 bss 清零）、从 ELF 头读取入口再跳转。内核代码本身（`main.cpp`）在这一步完全没动。
