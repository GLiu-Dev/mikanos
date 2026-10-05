# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application


`osbook_day04a → osbook_day04b` 是**第 4 天功能的核心实现**（day04a 只是用 Makefile 做准备）。这次改动把「逐字节填帧缓冲」升级成了真正的像素绘制。

## 改动概览

| 文件 | 状态 | 说明 |
|---|---|---|
| `kernel/frame_buffer_config.hpp` | 新增 | 定义 `FrameBufferConfig` 结构与 `PixelFormat` 枚举 |
| `MikanLoaderPkg/frame_buffer_config.hpp` | 新增(1行) | 符号链接指向 `../kernel/frame_buffer_config.hpp` |
| `MikanLoaderPkg/Main.c` | 修改(+28) | 引导加载器把帧缓冲配置传给内核 |
| `kernel/main.cpp` | 修改(+55) | 新增 `WritePixel`，实现像素/矩形绘制 |
| `kernel/Doxyfile` | 新增 | Doxygen 文档配置（2494 行，自动生成） |

涉及 5 个提交：核心是 `f54403b`(pixel writer 类与函数)、`3643f03`(引导加载器传帧缓冲配置给内核)、`30ae0bd`(给 PixelFormat 枚举加 `kPixel` 前缀)、`55defb9`/`6f81c5e`(doxygen 注释与 review tag)。

## 核心思路：把帧缓冲配置打包成结构体传递

**新增的 `kernel/frame_buffer_config.hpp`** 定义了一个 C/C++ 都兼容的配置结构：
```c
enum PixelFormat { kPixelRGBResv8BitPerColor, kPixelBGRResv8BitPerColor };
struct FrameBufferConfig {
  uint8_t* frame_buffer;          // 帧缓冲基址
  uint32_t pixels_per_scan_line;  // 每扫描行像素数
  uint32_t horizontal_resolution; // 水平分辨率
  uint32_t vertical_resolution;   // 垂直分辨率
  enum PixelFormat pixel_format;  // 像素格式
};
```
关键点：`MikanLoaderPkg/frame_buffer_config.hpp` 用**符号链接**复用同一份头文件，让 UEFI 引导加载器（C 代码）和内核（C++ 代码）共享同一结构定义，避免两份拷贝不一致。

## 引导加载器端（Main.c）—— 组装并传递配置

之前内核入口只收两个参数 `KernelMain(base, size)`；现在改为传一个结构体指针：
- 从 GOP（图形输出协议）读取 `FrameBufferBase`、`PixelsPerScanLine`、水平/垂直分辨率填入 `config`
- 把 UEFI 的像素格式映射到内核枚举（`PixelRedGreenBlueReserved8BitPerColor → kPixelRGBResv8BitPerColor`，BGR 同理）
- 入口签名变为 `void EntryPointType(const struct FrameBufferConfig*)`，调用 `entry_point(&config)`
- 遇到不支持的像素格式打印错误并 `Halt()`

## 内核端（main.cpp）—— 真正画像素

新增 `PixelColor{r,g,b}` 结构和核心函数：
```cpp
int WritePixel(const FrameBufferConfig& config, int x, int y, const PixelColor& c) {
  const int pixel_position = config.pixels_per_scan_line * y + x;
  if (config.pixel_format == kPixelRGBResv8BitPerColor) {
    uint8_t* p = &config.frame_buffer[4 * pixel_position];
    p[0]=c.r; p[1]=c.g; p[2]=c.b;
  } else if (/* BGR */) { /* 交换 r/b 顺序 */ }
  else return -1;
}
```
- 用 `pixels_per_scan_line * y + x` 做坐标→地址换算，**注意用每行像素数而非水平分辨率**（UEFI 中两者可能不同）
- 按 `kPixelRGB` / `kPixelBGR` 区分像素通道存储顺序
- `KernelMain` 签名改为接收 `const FrameBufferConfig&`：先以 `{255,255,255}`（白色）填充整个屏幕，再在坐标 (100,100) 处画一个 200×100 的绿色矩形 `{0,255,0}`，之后 `hlt` 死循环

## 小结

day04b 的实质是：**引入 `FrameBufferConfig` 作为内核与引导加载器的数据契约，并把写入逻辑抽象成 `WritePixel`，实现画全屏白底 + 一个绿色矩形**。相比 day04a 只是搭好了构建骨架，day04b 才真正落地了第 4 天「画个像素」的核心功能；顺带用 Doxygen 建了文档框架，并清理了书中的 `#@@range_begin/end` 代码范围标记。
