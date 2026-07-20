/**
 * MikanOS カーネル エントリーポイント（内核入口点）
 *
 * 本文件是 MikanOS 内核的起点。引导加载程序（MikanLoaderPkg/Main.c）
 * 在退出 UEFI Boot Services 后，从 ELF 文件头的 e_entry 字段读取此函数地址
 * 并直接跳转，将帧缓冲区的物理地址和大小作为参数传入。
 *
 * 当前阶段（早期开发）的主要任务：
 *   1. 验证内核能够被正确加载并执行（最小可行内核）
 *   2. 向帧缓冲区写入渐变色彩条纹，直观确认显示输出可用
 *   3. 进入低功耗休眠循环，防止 CPU 空转浪费电力
 */
 
// <cstdint> 提供固定宽度整数类型（uint8_t / uint64_t 等）。
// 在独立内核环境（freestanding）中，标准库大部分头文件不可用，
// 但 <cstdint> 仅依赖编译器内置定义（__INT8_TYPE__ 等），
// 因此在无操作系统支持的裸机环境中可以安全引入。
#include <cstdint>
 
/**
 * KernelMain - 内核 C++ 入口函数
 *
 * 调用约定与链接说明：
 *   extern "C"
 *     关闭 C++ 的名称修饰（name mangling）。
 *     C++ 编译器会将函数名编码为包含参数类型信息的符号
 *     （如 _Z10KernelMainmm），导致引导程序无法通过 ELF e_entry
 *     找到正确的入口地址。加上 extern "C" 后符号名保持为 KernelMain，
 *     与链接脚本和引导程序的期望一致。
 *
 *   参数传递约定（x86-64 System V ABI）：
 *     引导程序以 C 函数指针方式调用本函数：
 *       entry_point(gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize)
 *     按 System V AMD64 ABI，前两个整数参数依次通过 RDI、RSI 寄存器传入。
 *     注意：UEFI 环境使用 Microsoft ABI（参数用 RCX/RDX 传），
 *     但 ExitBootServices 之后 ABI 由内核自行决定；
 *     本内核采用 System V ABI，引导程序调用时也必须遵循此约定。
 *
 * 参数：
 *   frame_buffer_base  GOP 帧缓冲区的物理起始地址（来自 gop->Mode->FrameBufferBase）
 *                      类型为 uint64_t 而非指针，是因为物理地址在跨 UEFI/内核
 *                      边界传递时以整数形式更安全（无指针类型推断问题）
 *   frame_buffer_size  帧缓冲区总字节数（来自 gop->Mode->FrameBufferSize）
 *                      = PixelsPerScanLine × VerticalResolution × 每像素字节数
 *                      注意：这是物理帧缓冲区的实际大小，可能大于
 *                      HorizontalResolution × VerticalResolution × 4，
 *                      因为 PixelsPerScanLine 包含了硬件行末对齐的填充像素
 *
 * 返回值：
 *   void，且函数实际上永不返回（末尾有无限休眠循环）
 */
extern "C" void KernelMain(uint64_t frame_buffer_base,
                           uint64_t frame_buffer_size) {
 
  // 将物理地址整数转换为可按字节寻址的指针。
  // reinterpret_cast<uint8_t*> 是 C++ 风格的"重新解释"转型，
  // 等价于 C 风格的 (uint8_t*)frame_buffer_base，
  // 但意图更明确——我们清楚地知道这是一个裸物理地址转指针的操作，
  // 在内核这样的底层代码中是合法且必要的。
  uint8_t* frame_buffer = reinterpret_cast<uint8_t*>(frame_buffer_base);
 
  // 向帧缓冲区逐字节写入 (i % 256)，产生 0→255→0→255→... 的循环渐变值。
  //
  // 视觉效果分析（以 PixelRedGreenBlueReserved8BitPerColor 格式为例）：
  //   每像素占 4 字节（R, G, B, 保留），字节顺序为低地址→高地址：
  //     byte 0 (R): 0,   1,   2,   ..., 255,  0,   1,   ...
  //     byte 1 (G): 1,   2,   3,   ...,   0,  1,   2,   ...
  //     byte 2 (B): 2,   3,   4,   ...,   1,  2,   3,   ...
  //     byte 3 (X): 3,   4,   5,   ...,   2,  3,   4,   ...（保留，忽略）
  //   相邻像素的 R/G/B 各分量均相差 4（步进 = 每像素字节数），
  //   形成三条相位差为 1 字节的渐变波，叠加后产生周期性彩虹色条纹。
  //
  // 这是最简单的"I'm alive"测试：
  //   - 若屏幕出现彩色条纹 → 内核被正确加载，帧缓冲区可写
  //   - 若屏幕保持全白（引导程序写入的）→ 内核未能执行
  //   - 若屏幕花屏或黑屏 → 帧缓冲区地址或大小有误
  for (uint64_t i = 0; i < frame_buffer_size; ++i) {
    frame_buffer[i] = i % 256;
  }
 
  // 内核主循环：无限执行 HLT 指令，使 CPU 进入低功耗休眠状态。
  //
  // 为什么用 HLT 而非普通的 while(1) 空循环？
  //   while(1) {} 会让 CPU 全速空转（100% 占用率），
  //   在 QEMU/KVM 上会消耗大量宿主机 CPU 资源，产生额外热量。
  //   HLT 指令使逻辑 CPU 暂停执行，等待下一个中断信号（如定时器、键盘、NMI）到来再唤醒，
  //   大幅降低功耗。外层 while(1) 确保 HLT 被中断唤醒后（即使当前没有中断处理程序）
  //   立即再次执行 HLT，而不是返回或滑入非法地址。
  //
  // __asm__("hlt") 是 GCC/Clang 的内联汇编语法：
  //   __asm__("指令字符串") 将一条汇编指令直接内嵌到 C++ 代码中。
  //   "hlt" 对应 x86-64 的 HLT 指令（机器码 0xF4），
  //   执行后 CPU 停止取指，直到收到外部中断、NMI 或 RESET 信号。
  //   注意：当前内核尚未设置 IDT（中断描述符表），任何中断到来都会触发
  //   三重故障（Triple Fault）并导致 CPU 复位，因此实际上这是一个"永久停机"。
  //   未来添加中断处理后，HLT 将成为真正的低功耗等待。
  while (1) __asm__("hlt");
}