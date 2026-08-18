/**
 * @file main.cpp
 *
 * カーネル本体のプログラムを書いたファイル．
 * 内核主程序入口；MikanOS osbook_day08b 版本内核主逻辑
 * 启动流程：帧缓冲初始化 → GDT分段初始化 → 分页初始化 → PCI扫描 → xHCI USB控制器初始化 → 消息循环
 */
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <vector>
#include "frame_buffer_config.hpp"
#include "memory_map.hpp"
#include "graphics.hpp"
#include "mouse.hpp"
#include "font.hpp"
#include "console.hpp"
#include "pci.hpp"
#include "logger.hpp"
#include "usb/memory.hpp"
#include "usb/device.hpp"
#include "usb/classdriver/mouse.hpp"
#include "usb/xhci/xhci.hpp"
#include "usb/xhci/trb.hpp"
#include "interrupt.hpp"
#include "asmfunc.h"
#include "queue.hpp"
#include "segment.hpp"   // day08b新增：分段模块头文件
#include "paging.hpp"    // 分页机制头文件

/// 桌面背景颜色
const PixelColor kDesktopBGColor{45, 118, 237};
/// 文本前景白色
const PixelColor kDesktopFGColor{255, 255, 255};

/// 原地构造缓冲区：不使用堆malloc，内核早期无内存管理器
char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
PixelWriter* pixel_writer;

/// 控制台对象静态缓冲区
char console_buf[sizeof(Console)];
Console* console;

/**
 * @brief printk 内核格式化输出函数
 * @param format 格式化字符串
 * @return 输出字符长度
 * 封装vsprintf，最终输出到Console控制台
 */
int printk(const char* format, ...) {
  va_list ap;
  int result;
  char s[1024];
  va_start(ap, format);
  result = vsprintf(s, format, ap);
  va_end(ap);
  console->PutString(s);
  return result;
}

/// 鼠标光标对象缓冲区
char mouse_cursor_buf[sizeof(MouseCursor)];
MouseCursor* mouse_cursor;

/**
 * @brief 鼠标移动事件回调函数
 * @param displacement_x X方向偏移
 * @param displacement_y Y方向偏移
 * USB鼠标上报位移后调用，移动屏幕光标
 */
void MouseObserver(int8_t displacement_x, int8_t displacement_y) {
  mouse_cursor->MoveRelative({displacement_x, displacement_y});
}

/**
 * @brief Intel平台EHCI转xHCI切换逻辑
 * Intel主板同时存在EHCI(USB2.0)与xHCI(USB3.0)时，
 * 需要把USB2.0端口从EHCI控制器移交到xHCI统一管理，否则USB2设备无法被xHCI识别
 * @param xhc_dev PCI上的xHCI设备
 */
void SwitchEhci2Xhci(const pci::Device& xhc_dev) {
  bool intel_ehc_exist = false;
  // 遍历所有PCI设备，查找Intel EHCI(USB2.0控制器)
  for (int i = 0; i < pci::num_device; ++i) {
    if (pci::devices[i].class_code.Match(0x0cu, 0x03u, 0x20u) /* EHCI */ &&
        0x8086 == pci::ReadVendorId(pci::devices[i])) {
      intel_ehc_exist = true;
      break;
    }
  }
  // 不存在Intel EHCI，无需切换
  if (!intel_ehc_exist) {
    return;
  }
  // USB3PRM：USB3.0端口路由掩码
  uint32_t superspeed_ports = pci::ReadConfReg(xhc_dev, 0xdc);
  pci::WriteConfReg(xhc_dev, 0xd8, superspeed_ports); // USB3_PSSEN 使能USB3端口
  // XUSB2PRM：USB2.0端口路由掩码
  uint32_t ehci2xhci_ports = pci::ReadConfReg(xhc_dev, 0xd4);
  pci::WriteConfReg(xhc_dev, 0xd0, ehci2xhci_ports); // XUSB2PR 将USB2端口切给xHCI

  Log(kDebug, "SwitchEhci2Xhci: SS = %02, xHCI = %02x\n",
      superspeed_ports, ehci2xhci_ports);
}

/// 全局xHCI控制器指针
usb::xhci::Controller* xhc;

/**
 * @brief 内核消息结构体
 * 内核采用事件驱动模型，中断不直接处理业务，只投递消息到主循环
 */
struct Message {
  enum Type {
    kInterruptXHCI,    // xHCI控制器中断事件
  } type;
};

/// 主消息队列全局指针
ArrayQueue<Message>* main_queue;

/**
 * @brief xHCI中断服务例程
 * __attribute__((interrupt))：GCC标记为中断函数，自动生成中断栈帧保存/恢复代码
 * @param frame CPU进入中断时自动压入的寄存器栈帧
 */
__attribute__((interrupt))
void IntHandlerXHCI(InterruptFrame* frame) {
  // 向主循环投递中断消息
  main_queue->Push(Message{Message::kInterruptXHCI});
  // 向APIC发送EOI(中断结束)，通知CPU本次中断处理完成
  NotifyEndOfInterrupt();
}

// #@@range_begin(main_new_stack)
/// 内核栈：1MB，16字节对齐，满足ABI与SSE指令对齐要求
alignas(16) uint8_t kernel_main_stack[1024 * 1024];

/**
 * @brief 内核真正入口函数（切换新栈后执行）
 * extern "C"：关闭C++名字修饰，方便汇编跳转调用
 * @param frame_buffer_config_ref UEFI传入帧缓冲信息
 * @param memory_map_ref UEFI内存分布图
 */
extern "C" void KernelMainNewStack(
    const FrameBufferConfig& frame_buffer_config_ref,
    const MemoryMap& memory_map_ref) {
  // 拷贝参数到本地变量，避免外部引用风险
  FrameBufferConfig frame_buffer_config{frame_buffer_config_ref};
  MemoryMap memory_map{memory_map_ref};
// #@@range_end(main_new_stack)

  // 根据像素格式原地构造像素写入器
  switch (frame_buffer_config.pixel_format) {
    case kPixelRGBResv8BitPerColor:
      pixel_writer = new(pixel_writer_buf)
        RGBResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
    case kPixelBGRResv8BitPerColor:
      pixel_writer = new(pixel_writer_buf)
        BGRResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
  }

  const int kFrameWidth = frame_buffer_config.horizontal_resolution;
  const int kFrameHeight = frame_buffer_config.vertical_resolution;

  // 绘制桌面背景、底部任务栏UI
  FillRectangle(*pixel_writer,
                {0, 0},
                {kFrameWidth, kFrameHeight - 50},
                kDesktopBGColor);
  FillRectangle(*pixel_writer,
                {0, kFrameHeight - 50},
                {kFrameWidth, 50},
                {1, 8, 17});
  FillRectangle(*pixel_writer,
                {0, kFrameHeight - 50},
                {kFrameWidth / 5, 50},
                {80, 80, 80});
  DrawRectangle(*pixel_writer,
                {10, kFrameHeight - 40},
                {30, 30},
                {160, 160, 160});

  // 初始化控制台
  console = new(console_buf) Console{
    *pixel_writer, kDesktopFGColor, kDesktopBGColor
  };
  printk("Welcome to MikanOS!\n");
  SetLogLevel(kWarn); // 设置日志输出等级

  // #@@range_begin(setup_segments_and_page)
  // ========= day08b 核心改动代码 =========
  // 1. 初始化GDT全局描述符表（从segment模块导入）
  SetupSegments();

  // 选择子计算：索引 << 3，RPL=0
  // gdt[1] 代码段 → 0x08
  // gdt[2] 数据段 → 0x10
  const uint16_t kernel_cs = 1 << 3;
  const uint16_t kernel_ss = 2 << 3;

  // 将 DS/ES/FS/GS 段寄存器先置0
  SetDSAll(0);
  // 更新 CS, SS 段寄存器（内部实现包含远跳转刷新CS）
  SetCSSS(kernel_cs, kernel_ss);

  // 2. 创建恒等映射页表：虚拟地址 = 物理地址
  SetupIdentityPageTable();
  // #@@range_end(setup_segments_and_page)

  // 遍历UEFI内存图，打印所有可用内存区域
  const auto memory_map_base = reinterpret_cast<uintptr_t>(memory_map.buffer);
  for (uintptr_t iter = memory_map_base;
       iter < memory_map_base + memory_map.map_size;
       iter += memory_map.descriptor_size) {
    auto desc = reinterpret_cast<MemoryDescriptor*>(iter);
    if (IsAvailable(static_cast<MemoryType>(desc->type))) {
      printk("type = %u, phys = %08lx - %08lx, pages = %lu, attr = %08lx\n",
          desc->type,
          desc->physical_start,
          desc->physical_start + desc->number_of_pages * 4096 - 1,
          desc->number_of_pages,
          desc->attribute);
    }
  }

  // 初始化鼠标光标，起始坐标(300,200)
  mouse_cursor = new(mouse_cursor_buf) MouseCursor{
    pixel_writer, kDesktopBGColor, {300, 200}
  };

  // 内核消息队列：容量32条消息
  std::array<Message, 32> main_queue_data;
  ArrayQueue<Message> main_queue{main_queue_data};
  ::main_queue = &main_queue;

  // 扫描全部PCI总线，枚举所有PCI设备
  auto err = pci::ScanAllBus();
  Log(kDebug, "ScanAllBus: %s\n", err.Name());
  for (int i = 0; i < pci::num_device; ++i) {
    const auto& dev = pci::devices[i];
    auto vendor_id = pci::ReadVendorId(dev);
    auto class_code = pci::ReadClassCode(dev.bus, dev.device, dev.function);
    Log(kDebug, "%d.%d.%d: vend %04x, class %08x, head %02x\n",
        dev.bus, dev.device, dev.function,
        vendor_id, class_code, dev.header_type);
  }

  // 查找xHCI控制器设备，优先选择Intel厂商
  pci::Device* xhc_dev = nullptr;
  for (int i = 0; i < pci::num_device; ++i) {
    // 0c0330 = xHCI 设备类代码
    if (pci::devices[i].class_code.Match(0x0cu, 0x03u, 0x30u)) {
      xhc_dev = &pci::devices[i];
      if (0x8086 == pci::ReadVendorId(*xhc_dev)) {
        break;
      }
    }
  }
  if (xhc_dev) {
    Log(kInfo, "xHC has been found: %d.%d.%d\n",
        xhc_dev->bus, xhc_dev->device, xhc_dev->function);
  }

  // 设置IDT中断描述符：注册xHCI中断处理函数
  SetIDTEntry(idt[InterruptVector::kXHCI], MakeIDTAttr(DescriptorType::kInterruptGate, 0),
              reinterpret_cast<uint64_t>(IntHandlerXHCI), kernel_cs);
  // 加载IDT到CPU IDTR寄存器
  LoadIDT(sizeof(idt) - 1, reinterpret_cast<uintptr_t>(&idt[0]));

  // 读取BSP主CPU的Local APIC ID
  const uint8_t bsp_local_apic_id =
    *reinterpret_cast<const uint32_t*>(0xfee00020) >> 24;
  // 配置PCI MSI中断：xHCI使用MSI向本地APIC发送中断
  pci::ConfigureMSIFixedDestination(
      *xhc_dev, bsp_local_apic_id,
      pci::MSITriggerMode::kLevel, pci::MSIDeliveryMode::kFixed,
      InterruptVector::kXHCI, 0);

  // 读取BAR0，获取xHCI MMIO物理基地址
  const WithError<uint64_t> xhc_bar = pci::ReadBar(*xhc_dev, 0);
  Log(kDebug, "ReadBar: %s\n", xhc_bar.error.Name());
  const uint64_t xhc_mmio_base = xhc_bar.value & ~static_cast<uint64_t>(0xf);
  Log(kDebug, "xHC mmio_base = %08lx\n", xhc_mmio_base);

  // 构造xHCI控制器对象
  usb::xhci::Controller xhc{xhc_mmio_base};
  // Intel平台执行EHCI→xHCI端口移交
  if (0x8086 == pci::ReadVendorId(*xhc_dev)) {
    SwitchEhci2Xhci(*xhc_dev);
  }
  {
    auto err = xhc.Initialize();
    Log(kDebug, "xhc.Initialize: %s\n", err.Name());
  }
  Log(kInfo, "xHC starting\n");
  xhc.Run();
  ::xhc = &xhc;

  // 设置HID鼠标事件回调
  usb::HIDMouseDriver::default_observer = MouseObserver;

  // 遍历所有USB端口，尝试初始化已接入设备
  for (int i = 1; i <= xhc.MaxPorts(); ++i) {
    auto port = xhc.PortAt(i);
    Log(kDebug, "Port %d: IsConnected=%d\n", i, port.IsConnected());
    if (port.IsConnected()) {
      if (auto err = ConfigurePort(xhc, port)) {
        Log(kError, "failed to configure port: %s at %s:%d\n",
            err.Name(), err.File(), err.Line());
        continue;
      }
    }
  }

  // ===================== 内核主事件循环 =====================
  while (true) {
    __asm__("cli"); // 关中断
    if (main_queue.Count() == 0) {
      // 无消息：开中断 + hlt 休眠，等待硬件中断唤醒CPU
      __asm__("sti\n\thlt");
      continue;
    }
    Message msg = main_queue.Front();
    main_queue.Pop();
    __asm__("sti"); // 开中断

    // 处理消息
    switch (msg.type) {
    case Message::kInterruptXHCI:
      // 循环处理xHCI事件环上所有完成事件
      while (xhc.PrimaryEventRing()->HasFront()) {
        if (auto err = ProcessEvent(xhc)) {
          Log(kError, "Error while ProcessEvent: %s at %s:%d\n",
              err.Name(), err.File(), err.Line());
        }
      }
      break;
    default:
      Log(kError, "Unknown message type: %d\n", msg.type);
    }
  }
}

/**
 * @brief C++纯虚函数未实现时的默认回调
 * C++ runtime要求，内核没有libsupc++，必须自行提供
 * 一旦触发代表代码BUG，直接停机
 */
extern "C" void __cxa_pure_virtual() {
  while (1) __asm__("hlt");
}


/*

1. day08a 情况
GDT 所有初始化代码直接写在 KernelMainNewStack 内部：
定义 GDT 局部变量、配置描述符、LoadGDT、内嵌ReloadCS裸函数、手动设置 DS/ES/FS/GS/SS。
2. day08b 修改（对应代码块 setup_segments_and_page）
cpp
运行
SetupSegments();
const uint16_t kernel_cs = 1 << 3;
const uint16_t kernel_ss = 2 << 3;
SetDSAll(0);
SetCSSS(kernel_cs, kernel_ss);
SetupIdentityPageTable();
移除 main 内几十行 GDT 构造代码，改为调用模块函数 SetupSegments()；
GDT 实例转移到 segment.cpp 匿名命名空间，模块化封装；
新增分页初始化 SetupIdentityPageTable()；
段寄存器刷新逻辑封装进汇编函数 SetCSSS。
3. 重要硬件时序顺序（不能调换）
plaintext
SetupSegments()  // 1.加载GDT
SetDSAll / SetCSSS // 2.刷新段寄存器
SetupIdentityPageTable() // 3.开启分页
必须先设置合法 GDT、刷新段寄存器，再启用分页。
4. 补充理解「消息驱动设计」
硬件中断 IntHandlerXHCI 尽量精简：只投递消息，不做复杂处理
复杂 USB 事件解析放到主循环（开中断状态下执行）
hlt 休眠降低 CPU 占用，中断到来自动唤醒 CPU

*/