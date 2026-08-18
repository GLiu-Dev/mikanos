#pragma once
#include <stdint.h>

/**
 * @struct MemoryMap
 * @brief UEFI GetMemoryMap 返回的内存映射信息头部结构体
 * 该结构体由 bootloader(loader) 填充，传递给内核 KernelMainNewStack
 * 对应 UEFI 规范中 EFI_MEMORY_DESCRIPTOR 内存表管理信息
 */
struct MemoryMap {
  /// buffer_size：整个内存描述符缓冲区总字节大小
  unsigned long long buffer_size;
  /// buffer：指向一块内存，连续存放多条 MemoryDescriptor
  void* buffer;
  /// map_size：当前有效内存描述符占用的字节数
  unsigned long long map_size;
  /// map_key：内存快照Key，调用 ExitBootServices 需要传入该值
  unsigned long long map_key;
  /// descriptor_size：单条 MemoryDescriptor 的实际字节大小
  /// UEFI规范允许描述符结构存在扩展，不能直接用 sizeof(MemoryDescriptor)
  unsigned long long descriptor_size;
  /// descriptor_version：内存描述符版本号
  uint32_t descriptor_version;
};

/**
 * @struct MemoryDescriptor
 * @brief UEFI 单条内存区域描述符
 * 记录一段连续物理内存区间的属性
 */
struct MemoryDescriptor {
  /// type：内存区域类型（EFI_MEMORY_TYPE）
  uint32_t type;
  /// physical_start：物理内存起始地址
  uintptr_t physical_start;
  /// virtual_start：虚拟地址，UEFI BootService阶段一般为0
  uintptr_t virtual_start;
  /// number_of_pages：占用页数，UEFI标准一页 = 4096字节
  uint64_t number_of_pages;
  /// attribute：内存属性标志位（缓存策略、读写权限等）
  uint64_t attribute;
};

#ifdef __cplusplus
/**
 * @enum MemoryType
 * @brief UEFI EFI_MEMORY_TYPE 枚举封装
 * 一一对应UEFI标准内存类型编号
 */
enum class MemoryType {
  kEfiReservedMemoryType,        // 保留内存，不可使用
  kEfiLoaderCode,                // BootLoader代码段
  kEfiLoaderData,                // BootLoader数据段
  kEfiBootServicesCode,          // BootService时代驱动代码（ExitBootServices后可回收）
  kEfiBootServicesData,          // BootService时代数据（ExitBootServices后可回收）
  kEfiRuntimeServicesCode,       // Runtime服务代码，操作系统必须永久保留
  kEfiRuntimeServicesData,       // Runtime服务数据，操作系统必须永久保留
  kEfiConventionalMemory,        // 空闲可用普通内存（操作系统主要内存来源）
  kEfiUnusableMemory,            // 损坏/故障内存，禁止使用
  kEfiACPIReclaimMemory,         // ACPI表占用，解析完ACPI后可以回收
  kEfiACPIMemoryNVS,             // ACPI NVS区域，操作系统必须永久保留不能覆盖
  kEfiMemoryMappedIO,            // MMIO寄存器区域
  kEfiMemoryMappedIOPortSpace,   // I/O端口映射内存
  kEfiPalCode,
  kEfiPersistentMemory,          // 持久化内存（非易失内存）
  kEfiMaxMemoryType
};

/**
 * @brief 重载 == 运算符，支持 uint32_t 和 MemoryType 直接比较
 * UEFI描述符内type是原生uint32_t，方便和枚举互相判等
 */
inline bool operator==(uint32_t lhs, MemoryType rhs) {
  return lhs == static_cast<uint32_t>(rhs);
}
inline bool operator==(MemoryType lhs, uint32_t rhs) {
  return rhs == lhs;
}

// #@@range_begin(is_available)
/**
 * @brief 判断一块内存区域是否可以被操作系统当作普通内存使用
 *
 * 可以使用的三类：
 * 1. kEfiBootServicesCode：BootLoader驱动代码，ExitBootServices之后不再使用，可以回收
 * 2. kEfiBootServicesData：BootLoader数据区域，可以回收
 * 3. kEfiConventionalMemory：原生空闲内存
 *
 * ⚠️重要：Runtime、ACPI NVS、MMIO、Reserved 都不在可用范围内！
 */
inline bool IsAvailable(MemoryType memory_type) {
  return
    memory_type == MemoryType::kEfiBootServicesCode ||
    memory_type == MemoryType::kEfiBootServicesData ||
    memory_type == MemoryType::kEfiConventionalMemory;
}

/// UEFI标准页面大小：4KB
const int kUEFIPageSize = 4096;
// #@@range_end(is_available)
#endif

/*
1、数据流流程
loader.asm → 调用 UEFI GetMemoryMap 获取内存信息
填充 MemoryMap、多条 MemoryDescriptor
调用 ExitBootServices(map_key)
切换到内核，将 MemoryMap 作为参数传入 KernelMainNewStack
main.cpp 遍历 memory_map.buffer，调用 IsAvailable() 筛选可用物理内存
2、容易踩坑的关键点
不要使用 sizeof(MemoryDescriptor) 遍历表项！
必须使用 memory_map.descriptor_size。UEFI 规范允许后续扩展结构体长度。
cpp
运行
iter += memory_map.descriptor_size; // ✅ 正确
iter += sizeof(MemoryDescriptor);   // ❌ 不规范，未来版本会出错
只有 IsAvailable () 返回 true 的内存才能做内存分配
ACPI NVS、RuntimeServices、MMIO 一旦被覆盖会导致关机、电源、硬件功能异常。
map_key 作用
在调用 ExitBootServices 瞬间内存布局可能改变，传入正确 map_key 保证 UEFI 校验内存快照一致。
3、和 day08 上下文关联
day08b 新增分页 SetupIdentityPageTable()，后续内存管理器需要依靠这份 UEFI 内存图：
找到所有可用物理内存；
避开 MMIO、ACPI、Runtime 保留区域；
构建物理内存分配器。

*/