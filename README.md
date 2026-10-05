# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application


分析完成。`osbook_day04b → osbook_day04c` 的核心提交是 `70d1e74 replace WritePixel with more C++ way` —— 把函数式的 `WritePixel` 重构为**面向对象的 PixelWriter 类体系**。

## 改动概览

| 文件 | 状态 | 说明 |
|---|---|---|
| `kernel/main.cpp` | 修改(+82/−22) | `WritePixel` 函数 → `PixelWriter` 类体系 + placement new |
| `MikanLoaderPkg/Main.c` | 修改(−2) | 仅删除书中 `#@@range_begin/end` 标记，无逻辑变化 |

## 核心重构：函数 → 多态类

**day04b 时**是单一函数 `WritePixel(config, x, y, c)`，内部用 if/else 区分 RGB/BGR 两种像素格式。

**day04c 改为策略模式**：
```
PixelWriter (抽象基类)
 ├── RGBResv8BitPerColorPixelWriter
 └── BGRResv8BitPerColorPixelWriter
```

- **`PixelWriter`（基类）**：构造函数保存 `const FrameBufferConfig& config_`；纯虚函数 `virtual void Write(int x, int y, const PixelColor& c) = 0`；`protected` 提供 `PixelAt(x,y)` 计算地址（`frame_buffer + 4 * (pixels_per_scan_line * y + x)`），把「坐标→地址」换算收敛到基类。
- **两个派生类**：只实现 `Write` 的通道写入顺序差异（RGB 正序 / BGR 交换 R、B），通过 `using PixelWriter::PixelWriter` 继承构造函数。以后新增像素格式只需再加一个子类，不用改调用方。

## 关键点：内核里怎么用 new？

内核是 freestanding 环境（Makefile 里 `-ffreestanding -fno-exceptions -fno-rtti`），**没有堆分配器也没有标准库 new/delete**。所以这里手动提供：

```cpp
void* operator new(size_t size, void* buf) { return buf; }  // placement new
void operator delete(void* obj) noexcept {}                 // 空 delete

char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];  // 静态分配
PixelWriter* pixel_writer;                                       // 全局指针
```

`KernelMain` 里按 `pixel_format` 用 **placement new 在静态数组 `pixel_writer_buf` 上**构造对应子类，避免在编译期就为两种可能各分配一个对象：

```cpp
switch (frame_buffer_config.pixel_format) {
  case kPixelRGBResv8BitPerColor:
    pixel_writer = new(pixel_writer_buf) RGBResv8BitPerColorPixelWriter{frame_buffer_config};
    break;
  case kPixelBGRResv8BitPerColor:
    pixel_writer = new(pixel_writer_buf) BGRResv8BitPerColorPixelWriter{frame_buffer_config};
    break;
}
```

之后统一通过虚函数 `pixel_writer->Write(x, y, ...)` 画图 —— 白色全屏 + 绿色矩形，行为不变。

## 两个值得注意的点

1. **绿色矩形位置变了**：day04b 里是 `WritePixel(config, 100+x, 100+y, ...)`（画在 (100,100) 处），day04c 改成了 `pixel_writer->Write(x, y, ...)`（画在原点 (0,0) 处）。这是重构中实际发生的行为变化，看起来是书中代码的顺手简化而非刻意为之。
2. **纯虚基类 + placement new + 静态 buffer**：这是第 4 天「在没有堆的内核里用多态」的标准做法——对象存于静态区，构造函数手动调用，这也是后续第 5 天引入内存管理（`new` 真正可用）的前奏。

一句话：day04c 不改变画图结果，但把「画点」从函数式改成了**可扩展的多态设计**，并示范了 freestanding 环境下用 placement new 构造对象的技巧。