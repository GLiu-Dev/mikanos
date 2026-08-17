#pragma once
// 头文件保护宏：防止同一头文件被多次 include，替代传统 #ifndef 守卫
// 注意：#pragma once 是编译器扩展（GCC/Clang/MSVC 都支持），不是标准C/C++

#include <stdint.h>
// 引入固定宽度整数类型：uint8_t uint32_t uint64_t uintptr_t 等
// uintptr_t：指针宽度无符号整数，32位系统=uint32_t，64位=uint64_t

/**
 * @brief UEFI GetMemoryMap() 函数入参结构体，对应 UEFI Spec 定义
 * 用来存放内存描述符数组的缓冲区信息
 */
struct MemoryMap {
  unsigned long long buffer_size;    // 缓冲区总字节大小：存放所有 MemoryDescriptor 的内存大小
  void* buffer;                      // 指向内存描述符数组首地址（一块连续缓冲区）
  unsigned long long map_size;       // 当前有效内存映射占用字节数（<= buffer_size）
  unsigned long long map_key;        // 内存映射密钥！重要：调用 ExitBootServices 必须传入该key
                                     // UEFI 用它校验获取内存映射之后内存布局有没有变动
  unsigned long long descriptor_size;// 单个 MemoryDescriptor 结构体实际大小
                                     // UEFI 规范允许不同固件使用扩展描述符，不能直接用 sizeof(MemoryDescriptor)！
  uint32_t descriptor_version;       // MemoryDescriptor 版本号
};

/**
 * @brief UEFI 内存区域描述符
 * 一条描述符代表一段连续物理内存区间，标记内存用途
 */
struct MemoryDescriptor {
  uint32_t type;                     // 内存类型编号，对应下方 MemoryType 枚举
  uintptr_t physical_start;          // 物理起始地址（PA）
  uintptr_t virtual_start;           // 虚拟起始地址（UEFI Boot Service 阶段不一定启用分页，常为0）
  uint64_t number_of_pages;          // 该区域占用页面数量（UEFI 默认页面大小 4KB）
  uint64_t attribute;                // 内存属性标志位：可读、可写、可执行、缓存策略等
};

#ifdef __cplusplus
// 如果以 C++ 模式编译（MikanOS 内核主体使用C++）启用下面代码
// C 语言没有 enum class，因此使用条件编译屏蔽

/**
 * @brief UEFI 标准内存类型枚举
 * 与 UEFI Specification EFI_MEMORY_TYPE 一一对应
 */
enum class MemoryType {
  kEfiReservedMemoryType,        // 0: 保留内存，不可被OS使用
  kEfiLoaderCode,                // 1: UEFI 加载器代码段
  kEfiLoaderData,                // 2: UEFI 加载器数据段
  kEfiBootServicesCode,          // 3: Boot Service 运行时代码（ExitBootServices之后可以回收）
  kEfiBootServicesData,          // 4: Boot Service 运行时数据（ExitBootServices之后可以回收）
  kEfiRuntimeServicesCode,       // 5: Runtime Service 代码（OS运行期间必须永久保留，不能占用）
  kEfiRuntimeServicesData,       // 6: Runtime Service 数据（永久保留）
  kEfiConventionalMemory,        // 7: ✅【重点】常规可用空闲内存！操作系统可以自由分配使用
  kEfiUnusableMemory,            // 8: 损坏/故障内存，禁止使用
  kEfiACPIReclaimMemory,         // 9: ACPI 可回收内存：读取完ACPI表之后OS可以回收利用
  kEfiACPIMemoryNVS,             // 10: ACPI NVS 内存：休眠相关，OS绝对不能覆盖
  kEfiMemoryMappedIO,            // 11: MMIO 内存映射IO区域（设备寄存器，不是RAM）
  kEfiMemoryMappedIOPortSpace,   // 12: MMIO Port空间
  kEfiPalCode,                   // 13: PAL代码（安腾架构相关，x86_64一般遇不到）
  kEfiPersistentMemory,          // 14: 持久内存（NVDIMM）
  kEfiMaxMemoryType              // 15: 枚举边界标记，用于遍历上限
};

/**
 * @brief 重载 == 运算符：允许 uint32_t(原始数值) == MemoryType(枚举)
 * 示例：if (desc.type == MemoryType::kEfiConventionalMemory)
 * UEFI 返回的 MemoryDescriptor.type 是 uint32_t 原始数字，不能直接和 enum class 比较
 * enum class 强类型枚举，默认不会隐式转换为整数，所以需要手动重载相等运算符
 */
inline bool operator==(uint32_t lhs, MemoryType rhs) {
  return lhs == static_cast<uint32_t>(rhs);
}

/**
 * @brief 反向重载：MemoryType == uint32_t
 * 支持写法：if (MemoryType::kEfiConventionalMemory == desc.type)
 */
inline bool operator==(MemoryType lhs, uint32_t rhs) {
  return rhs == lhs;
}

#endif