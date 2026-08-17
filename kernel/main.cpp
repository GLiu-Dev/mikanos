/**
 * @file main.cpp
 *
 * カーネル本体のプログラムを書いたファイル．
 * 【翻译】本文件编写内核主体程序
 */
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <vector>

// 项目内部头文件
#include "frame_buffer_config.hpp"    // 帧缓冲区配置结构体（分辨率、像素格式、显存地址）
#include "memory_map.hpp"              // 上一段你拿到的UEFI内存映射、内存描述符定义
#include "graphics.hpp"                // 绘图基础：矩形填充、像素写入器
#include "mouse.hpp"                   // 鼠标光标管理
#include "font.hpp"                    // ASCII字体点阵
#include "console.hpp"                 // 文本控制台（终端）
#include "pci.hpp"                     // PCI总线扫描、配置空间访问
#include "logger.hpp"                  // 日志系统（Debug/Info/Warn/Error分级）
#include "usb/memory.hpp"              // USB驱动内存分配工具
#include "usb/device.hpp"              // USB通用设备抽象
#include "usb/classdriver/mouse.hpp"   // USB HID鼠标类驱动
#include "usb/xhci/xhci.hpp"           // xHCI（USB3.0主机控制器）控制器
#include "usb/xhci/trb.hpp"            // xHCI传输请求块TRB
#include "interrupt.hpp"               // IDT中断描述符表相关
#include "asmfunc.h"                   // C++调用汇编函数声明
#include "queue.hpp"                   // 静态数组环形队列

// ===== 全局常量：桌面配色 =====
const PixelColor kDesktopBGColor{45, 118, 237}; // 桌面背景蓝色
const PixelColor kDesktopFGColor{255, 255, 255}; // 文字白色

// ===== 放置对象的静态缓冲区（裸机 new placement 定位new） =====
// MikanOS早期还没有实现通用堆内存malloc，不能直接new，使用静态内存+定位new
char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
PixelWriter* pixel_writer;  // 像素写入器抽象接口

char console_buf[sizeof(Console)];
Console* console;           // 文本控制台实例指针

/**
 * @brief printk：内核格式化打印函数，类似Linux printk
 * @param format 格式化字符串
 * @return 输出字符长度
 * 原理：使用vsprintf先写入临时缓冲区，再调用控制台输出字符串
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

char mouse_cursor_buf[sizeof(MouseCursor)];
MouseCursor* mouse_cursor;  // 鼠标光标实例指针

/**
 * @brief 鼠标移动事件回调函数
 * 当USB鼠标上报位移数据时被调用
 * @param displacement_x X方向相对位移
 * @param displacement_y Y方向相对位移
 */
void MouseObserver(int8_t displacement_x, int8_t displacement_y) {
  mouse_cursor->MoveRelative({displacement_x, displacement_y});
}

/**
 * @brief SwitchEhci2Xhci
 * Intel平台特有：把USB2.0端口从EHCI控制器切换到xHCI控制器管理
 * Intel主板同时存在EHCI(USB2.0)与xHCI(USB3)，默认USB2.0由EHCI接管；
 * 如果不切换，xHCI无法识别USB2设备，只能识别USB3设备。
 */
void SwitchEhci2Xhci(const pci::Device& xhc_dev) {
  bool intel_ehc_exist = false;
  // 遍历所有PCI设备，查找Intel EHCI控制器
  for (int i = 0; i < pci::num_device; ++i) {
    // PCI类别码：0c 03 20 → EHCI
    if (pci::devices[i].class_code.Match(0x0cu, 0x03u, 0x20u)
        && 0x8086 == pci::ReadVendorId(pci::devices[i])) { // 0x8086 Intel厂商ID
      intel_ehc_exist = true;
      break;
    }
  }
  if (!intel_ehc_exist) {
    return; // 无Intel EHCI，无需切换
  }
  // 读取xHCI私有配置寄存器，切换USB端口归属
  uint32_t superspeed_ports = pci::ReadConfReg(xhc_dev, 0xdc); // USB3PRM
  pci::WriteConfReg(xhc_dev, 0xd8, superspeed_ports); // USB3_PSSEN
  uint32_t ehci2xhci_ports = pci::ReadConfReg(xhc_dev, 0xd4); // XUSB2PRM
  pci::WriteConfReg(xhc_dev, 0xd0, ehci2xhci_ports); // XUSB2PR
  Log(kDebug, "SwitchEhci2Xhci: SS = %02, xHCI = %02x\n",
      superspeed_ports, ehci2xhci_ports);
}

usb::xhci::Controller* xhc; // xHCI控制器全局指针

/**
 * @brief 内核消息结构体
 * 内核使用消息队列实现【中断上下文】与【主循环上下文】解耦
 * ⚠️ 中断处理函数禁止执行耗时操作，只允许投递消息，延后到主循环处理
 */
struct Message {
  enum Type {
    kInterruptXHCI,    // xHCI控制器产生中断事件
  } type;
};

ArrayQueue<Message>* main_queue; // 主消息队列全局指针

/**
 * @brief xHCI中断服务例程 ISR
 * __attribute__((interrupt))：GCC扩展，自动生成中断函数栈帧、iret返回
 * 中断上下文：禁止复杂处理，仅向队列投递消息，发送EOI通知APIC中断结束
 */
__attribute__((interrupt))
void IntHandlerXHCI(InterruptFrame* frame) {
  main_queue->Push(Message{Message::kInterruptXHCI});
  NotifyEndOfInterrupt(); // 向本地APIC发送EOI，允许接收后续中断
}

/**
 * @brief 内核入口函数 KernelMain
 * extern "C"：关闭C++名字重整，引导程序（C/汇编）可以直接调用
 * 参数：
 *   frame_buffer_config：UEFI提供的帧缓冲区信息（显存物理地址、分辨率、像素格式）
 *   memory_map：UEFI GetMemoryMap 获取的整机内存布局表（就是上一节结构体）
 */
extern "C" void KernelMain(const FrameBufferConfig& frame_buffer_config,
                           const MemoryMap& memory_map)
{
  // 步骤1：根据像素格式构造对应的像素写入器（定位new）
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

  // 步骤2：绘制桌面UI
  FillRectangle(*pixel_writer, {0, 0}, {kFrameWidth, kFrameHeight - 50}, kDesktopBGColor); // 主桌面蓝色背景
  FillRectangle(*pixel_writer, {0, kFrameHeight - 50}, {kFrameWidth, 50}, {1, 8, 17}); // 底部任务栏深色
  FillRectangle(*pixel_writer, {0, kFrameHeight - 50}, {kFrameWidth / 5, 50}, {80, 80, 80}); // 任务栏左侧灰色区域
  DrawRectangle(*pixel_writer, {10, kFrameHeight - 40}, {30, 30}, {160, 160, 160}); // 简易方框图标

  // 步骤3：初始化控制台终端
  console = new(console_buf) Console{
    *pixel_writer, kDesktopFGColor, kDesktopBGColor
  };
  printk("Welcome to MikanOS!\n");
  SetLogLevel(kWarn); // 设置日志输出等级：只输出Warn及以上（关闭Debug日志）

  // 筛选内核可用内存类型：ExitBootServices后可以使用的内存区域
  const std::array available_memory_types{
    MemoryType::kEfiBootServicesCode,
    MemoryType::kEfiBootServicesData,
    MemoryType::kEfiConventionalMemory,
  };

  // 步骤4：遍历UEFI内存映射表，打印可用物理内存区间
  printk("memory_map: %p\n", &memory_map);
  // 重要：不能用sizeof(MemoryDescriptor)循环！必须使用descriptor_size步进（UEFI规范要求）
  for (uintptr_t iter = reinterpret_cast<uintptr_t>(memory_map.buffer);
       iter < reinterpret_cast<uintptr_t>(memory_map.buffer) + memory_map.map_size;
       iter += memory_map.descriptor_size) {
    auto desc = reinterpret_cast<MemoryDescriptor*>(iter);
    for (int i = 0; i < available_memory_types.size(); ++i) {
      if (desc->type == available_memory_types[i]) {
        // 打印：内存类型、物理起止地址、页面数量、内存属性
        printk("type = %u, phys = %08lx - %08lx, pages = %lu, attr = %08lx\n",
            desc->type,
            desc->physical_start,
            desc->physical_start + desc->number_of_pages * 4096 - 1,
            desc->number_of_pages,
            desc->attribute);
      }
    }
  }

  // 步骤5：初始化鼠标光标
  mouse_cursor = new(mouse_cursor_buf) MouseCursor{
    pixel_writer, kDesktopBGColor, {300, 200} // 初始坐标(300,200)
  };

  // 创建主消息队列（静态数组环形队列，32条消息容量）
  std::array<Message, 32> main_queue_data;
  ArrayQueue<Message> main_queue{main_queue_data};
  ::main_queue = &main_queue;

  // 步骤6：扫描全部PCI总线设备
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

  // 步骤7：查找xHCI控制器（优先Intel厂商设备）
  pci::Device* xhc_dev = nullptr;
  for (int i = 0; i < pci::num_device; ++i) {
    // PCI类别码 0c 03 30 = xHCI USB3主机控制器
    if (pci::devices[i].class_code.Match(0x0cu, 0x03u, 0x30u)) {
      xhc_dev = &pci::devices[i];
      if (0x8086 == pci::ReadVendorId(*xhc_dev)) {
        break; // 找到Intel xHCI，优先选用
      }
    }
  }
  if (xhc_dev) {
    Log(kInfo, "xHC has been found: %d.%d.%d\n",
        xhc_dev->bus, xhc_dev->device, xhc_dev->function);
  }

  // 步骤8：配置IDT，注册xHCI中断处理函数
  const uint16_t cs = GetCS(); // 获取当前代码段选择子
  // 设置IDT表项：中断门，绑定IntHandlerXHCI函数
  SetIDTEntry(idt[InterruptVector::kXHCI], MakeIDTAttr(DescriptorType::kInterruptGate, 0),
              reinterpret_cast<uint64_t>(IntHandlerXHCI), cs);
  LoadIDT(sizeof(idt) - 1, reinterpret_cast<uintptr_t>(&idt[0])); // lidt加载IDT

  // 获取BSP（启动处理器）本地APIC ID
  const uint8_t bsp_local_apic_id =
    *reinterpret_cast<const uint32_t*>(0xfee00020) >> 24;
  // 配置PCI MSI中断：xHCI使用MSI向本地APIC发送中断
  pci::ConfigureMSIFixedDestination(
      *xhc_dev, bsp_local_apic_id,
      pci::MSITriggerMode::kLevel, pci::MSIDeliveryMode::kFixed,
      InterruptVector::kXHCI, 0);

  // 读取BAR寄存器，获取xHCI MMIO物理基地址
  const WithError<uint64_t> xhc_bar = pci::ReadBar(*xhc_dev, 0);
  Log(kDebug, "ReadBar: %s\n", xhc_bar.error.Name());
  const uint64_t xhc_mmio_base = xhc_bar.value & ~static_cast<uint64_t>(0xf); // 地址4KB对齐
  Log(kDebug, "xHC mmio_base = %08lx\n", xhc_mmio_base);

  // 构造xHCI控制器对象
  usb::xhci::Controller xhc{xhc_mmio_base};
  // Intel平台执行EHCI→xHCI端口切换
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

  // 设置USB鼠标事件回调
  usb::HIDMouseDriver::default_observer = MouseObserver;

  // 遍历所有USB端口，检测已接入设备并初始化端口
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

  // ========== 内核主事件循环（无限循环）==========
  while (true) {
    __asm__("cli"); // 关中断
    if (main_queue.Count() == 0) {
      // 无消息：开中断 + hlt 休眠CPU，等待硬件中断唤醒
      __asm__("sti\n\thlt");
      continue;
    }
    Message msg = main_queue.Front();
    main_queue.Pop();
    __asm__("sti"); // 开中断

    switch (msg.type) {
    case Message::kInterruptXHCI:
      // 处理xHCI事件环上所有完成事件
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
 * @brief __cxa_pure_virtual
 * C++运行时函数：纯虚函数被调用时触发
 * 裸机环境没有libsupc++，必须自定义实现防止链接报错
 * 一旦触发直接停机
 */
extern "C" void __cxa_pure_virtual() {
  while (1) __asm__("hlt");
}