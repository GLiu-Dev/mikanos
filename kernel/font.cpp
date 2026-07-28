/**
 * @file font.cpp
 *
 * フォント描画のプログラムを集めたファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件实现 MikanOS 内核的完整 ASCII 字符渲染子系统，提供：
 *     ① _binary_hankaku_bin_* 外部符号 — 由链接器注入的字体二进制数据引用
 *     ② GetFont(char c)               — 根据字符码查找对应位图数据的指针
 *     ③ WriteAscii(...)               — 向帧缓冲区渲染单个 ASCII 字符
 *
 * ── 与上一版本的核心差异：从手工字体到完整字体文件 ─────────────────────────
 *
 *   上一版本：
 *     手工绘制了 kFontA[16] 数组（仅支持字母 'A'），
 *     WriteAscii 内部有 "if (c != 'A') return" 守卫语句。
 *
 *   本版本：
 *     引入 hankaku.bin（半角字体文件），包含完整 ASCII（及半角假名）字符集，
 *     通过链接器二进制嵌入机制直接包含在内核可执行文件中。
 *     WriteAscii 现在支持 hankaku.bin 中的所有字符（通常 0x00–0xFF，共 256 字符）。
 *
 *   main.cpp 中的对应变化：
 *     旧版：WriteAscii(*pixel_writer, 50, 50, 'A', {0, 0, 0})  // 两个 'A'
 *     新版：for (char c = '!'; c <= '~'; ++c)                  // 全部可打印 ASCII
 *             WriteAscii(*pixel_writer, 8 * i, 50, c, ...)
 *
 * ── hankaku.bin 字体文件格式 ─────────────────────────────────────────────────
 *
 *   hankaku.bin 是一个平坦的二进制字体文件，格式极其简单：
 *
 *     偏移               内容
 *     0x000              字符 0x00（NUL）的 16 字节位图
 *     0x010              字符 0x01（SOH）的 16 字节位图
 *     ...
 *     0x210              字符 0x21（'!'）的 16 字节位图
 *     0x410              字符 0x41（'A'）的 16 字节位图
 *     ...
 *     0xFF0              字符 0xFF 的 16 字节位图
 *
 *   每字符固定 16 字节，对应 8 像素宽 × 16 像素高的位图：
 *     字节 0：第  0 行（最顶行），bit7 = 最左列，bit0 = 最右列
 *     字节 1：第  1 行
 *     ...
 *     字节15：第 15 行（最底行）
 *
 *   查询字符 c 的位图：
 *     偏移 = 16 × (unsigned int)c
 *     指针 = &_binary_hankaku_bin_start + 偏移
 *
 * ── 链接器二进制嵌入机制 ─────────────────────────────────────────────────────
 *
 *   构建系统通过 objcopy 将 hankaku.bin 转换为目标文件：
 *     objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
 *             hankaku.bin hankaku.o
 *
 *   objcopy 会自动生成三个全局符号（符号名根据文件名派生）：
 *     _binary_hankaku_bin_start  → 字体数据第一个字节的地址
 *     _binary_hankaku_bin_end    → 字体数据最后一个字节的下一字节地址
 *     _binary_hankaku_bin_size   → ★ 特殊符号，见下方详解 ★
 *
 *   hankaku.o 与内核其他目标文件一起链接，字体数据被嵌入到内核 ELF 中。
 *   运行时字体数据就在内存里，无需文件 I/O。
 */
 
#include "font.hpp"
 
/* ============================================================================
 * 一、链接器注入的字体二进制数据符号
 * ============================================================================ */
 
// #@@range_begin(hankaku_bin)
 
/**
 * _binary_hankaku_bin_start — 字体数据第一个字节的地址
 *
 * extern 声明告知编译器"该符号在外部定义（由链接器提供）"，
 * 类型声明为 const uint8_t 仅表示我们打算以字节方式访问此符号。
 *
 * 使用方式：
 *   取地址 &_binary_hankaku_bin_start 得到字体数据的起始指针（uint8_t*）。
 *   不直接使用符号值本身（它是起始字节的值，不是地址）。
 */
extern const uint8_t _binary_hankaku_bin_start;
 
/**
 * _binary_hankaku_bin_end — 字体数据结束位置（最后字节的下一地址）
 *
 * 遵循 C++ 的"past-the-end"惯例：指向数据区域之外的第一个字节。
 * 字体数据总字节数 = &_binary_hankaku_bin_end - &_binary_hankaku_bin_start。
 * 本文件中未直接使用（用 size 符号做边界检查），保留声明供将来扩展。
 */
extern const uint8_t _binary_hankaku_bin_end;
 
/**
 * _binary_hankaku_bin_size — ★ 字体文件大小（以一种非直觉的方式编码）★
 *
 * ── 这是本文件中最容易误解的技术细节 ─────────────────────────────────────
 *
 *   objcopy 生成的 _binary_xxx_size 符号与 start/end 不同：
 *     start / end：普通符号，其"地址"即数据的内存位置
 *     size       ：特殊符号，其"地址"（符号值）即文件字节数（不是内存地址）
 *
 *   换句话说：
 *     _binary_hankaku_bin_size 这个符号的"值"（链接器赋予的地址字段）
 *     等于 hankaku.bin 文件的字节数（如 256 × 16 = 4096 字节则值为 0x1000）。
 *
 *   因此正确的使用方式是取它的"地址"并转换为整数：
 *     uintptr_t size = reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size);
 *
 *   错误的用法（不能直接读取其值）：
 *     uint8_t wrong = _binary_hankaku_bin_size;   // 读取的是 0x1000 内存处的数据
 *     size_t  wrong = (size_t)_binary_hankaku_bin_size; // 编译错误，uint8_t→size_t 截断
 *
 *   这种"符号地址即数值"的技巧是 GNU ld / objcopy 的特有约定，不是标准 C++ 行为。
 *   类型声明为 uint8_t 是一种惯例，让编译器知道这是字节级别的符号；
 *   实际上它是一个"大小"，只能通过 reinterpret_cast<uintptr_t>(&...) 正确读取。
 */
extern const uint8_t _binary_hankaku_bin_size;
 
/**
 * GetFont — 根据 ASCII 字符码返回对应 16 字节位图数据的指针
 *
 * ── 参数说明 ────────────────────────────────────────────────────────────────
 *
 *   c（char）
 *     ASCII 字符码（如 'A' = 0x41 = 65，'!' = 0x21 = 33）。
 *     注意：char 在 x86-64 Linux（System V ABI）上为有符号类型（signed char），
 *     范围 -128 ~ 127，因此需要转换为 unsigned int 再计算索引，
 *     避免负数字符码（如扩展 ASCII 0x80~0xFF 在 signed char 下为负值）产生负索引。
 *
 * ── 返回值 ──────────────────────────────────────────────────────────────────
 *
 *   非 nullptr：指向 hankaku.bin 中字符 c 对应的 16 字节位图数据的指针。
 *               调用方可将其作为 uint8_t[16] 使用（font[0]..font[15] 为各行位图）。
 *   nullptr   ：字符码超出字体文件覆盖范围（越界），不渲染此字符。
 *
 * ── 实现原理 ────────────────────────────────────────────────────────────────
 *
 *   步骤 1：计算字节偏移
 *     index = 16 × (unsigned int)c
 *     hankaku.bin 中每个字符固定占 16 字节，字符码 c 的位图从偏移 16*c 开始。
 *     static_cast<unsigned int>(c)：
 *       将 signed char 转为 unsigned int，确保负值字符码（如 (char)0xFF）
 *       被正确解释为大正数（0xFF = 255 → index = 16*255 = 4080），
 *       而非负索引（-1 → index = -16，越界检查能捕获）。
 *
 *   步骤 2：边界检查
 *     if (index >= reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size))
 *       读取 _binary_hankaku_bin_size 符号的"地址"（即文件字节数）作为上限。
 *       若 index ≥ 文件大小（= 字符码超出字体文件收录范围），返回 nullptr。
 *       例：hankaku.bin 收录 256 字符 × 16 字节 = 4096 字节，
 *           合法 index 范围：0 ≤ index < 4096。
 *
 *   步骤 3：返回指针
 *     return &_binary_hankaku_bin_start + index;
 *       从字体数据起始地址偏移 index 字节，指向字符 c 位图的第一个字节。
 *       指针算术：uint8_t* + index（字节偏移），精确定位到目标字符。
 */
const uint8_t* GetFont(char c) {
  // 计算字符 c 在 hankaku.bin 中的字节偏移：
  //   每字符 16 字节，字符码 c（转为无符号）× 16 = 起始偏移
  //   static_cast<unsigned int>(c)：避免 signed char 负值产生负索引
  auto index = 16 * static_cast<unsigned int>(c);
 
  // 边界检查：index 必须小于字体文件总字节数
  //   reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)：
  //     取 _binary_hankaku_bin_size 符号的地址，解释为整数 = 文件字节数
  //     （这是 objcopy 生成 size 符号的特有约定，见上方详细说明）
  if (index >= reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)) {
    return nullptr;  // 字符码超出字体覆盖范围，返回空指针
  }
 
  // 返回字体数据起始地址 + 偏移 = 字符 c 位图数据的第一个字节地址
  return &_binary_hankaku_bin_start + index;
}
// #@@range_end(hankaku_bin)
 
/* ============================================================================
 * 二、WriteAscii — 位图字体单字符渲染函数
 * ============================================================================ */
 
// #@@range_begin(write_ascii)
/**
 * WriteAscii — 在屏幕指定坐标渲染单个 ASCII 字符（支持完整字体文件中的所有字符）
 *
 * ── 与上一版本的差异 ─────────────────────────────────────────────────────────
 *
 *   上一版本：
 *     直接引用文件内的 kFontA[16] 数组，仅支持 'A'，
 *     通过 "if (c != 'A') return" 守卫语句拒绝其他字符。
 *
 *   本版本：
 *     通过 GetFont(c) 查找 hankaku.bin 中字符 c 的位图，
 *     支持字体文件覆盖的所有字符（通常为完整 ASCII 0x00–0xFF）。
 *     越界字符由 GetFont 返回 nullptr，WriteAscii 检查后静默跳过。
 *     渲染算法（位操作提取位图像素）与上一版本完全相同，仅数据源改变。
 *
 * ── 参数说明 ────────────────────────────────────────────────────────────────
 *
 *   writer（PixelWriter&）
 *     像素写入器的引用，通过虚函数 Write() 自动适配 RGB / BGR 帧缓冲格式。
 *     引用语义：写入器必须存在（不允许 null），省去空指针检查。
 *
 *   x, y（int）
 *     字符左上角在屏幕上的像素坐标（x=列，y=行，0=左上角）。
 *     字符向右占 8 像素（dx=0..7），向下占 16 像素（dy=0..15）。
 *     调用方有责任确保渲染区域不超出屏幕边界（本函数不做边界检查）。
 *
 *   c（char）
 *     要渲染的 ASCII 字符码，传给 GetFont(c) 查找位图数据。
 *     GetFont 返回 nullptr 时（字符码超出范围），本函数静默跳过，不渲染。
 *
 *   color（const PixelColor&）
 *     字符前景色（笔画颜色）；背景色保持不变（调用前已绘制背景）。
 *     const 引用：避免拷贝 3 字节结构体，且保证 color 不被修改。
 *
 * ── 位图渲染算法（与上一版本相同） ──────────────────────────────────────────
 *
 *   外层循环 dy（0..15）：遍历字符的每一行，读取 font[dy]
 *   内层循环 dx（0..7） ：遍历该行每一列，提取对应位
 *
 *   核心判断式：(font[dy] << dx) & 0x80u
 *
 *     将 font[dy] 左移 dx 位，使"第 dx 列的位"移到 bit7（最高位），
 *     再与 0x80u 按位与，判断 bit7 是否为 1。
 *     等价含义：检查 font[dy] 的第 (7-dx) 位是否为 1。
 *
 *     位图字节位排列：
 *       bit7 = dx=0（最左列）... bit0 = dx=7（最右列）
 *
 *     示例（某行数据 = 0b01111110）：
 *       dx=0: 0b01111110 << 0 = 0b01111110, & 0x80 =    0 → 不亮
 *       dx=1: 0b01111110 << 1 = 0b11111100, & 0x80 = 0x80 → 点亮
 *       dx=2: 0b01111110 << 2 = 0x1FC,      & 0x80 = 0x80 → 点亮
 *       dx=3: 0b01111110 << 3 = 0x3F8,      & 0x80 = 0x80 → 点亮
 *       dx=4: 0b01111110 << 4 = 0x7F0,      & 0x80 = 0x80 → 点亮
 *       dx=5: 0b01111110 << 5 = 0xFC0,      & 0x80 = 0x80 → 点亮
 *       dx=6: 0b01111110 << 6 = 0x1F80,     & 0x80 = 0x80 → 点亮
 *       dx=7: 0b01111110 << 7 = 0x3F00,     & 0x80 =    0 → 不亮
 *     结果：中间 6 列点亮，两端不亮（对应 'A' 的中横线行）
 *
 * ── main.cpp 中的使用示例（新版）────────────────────────────────────────────
 *
 *   int i = 0;
 *   for (char c = '!'; c <= '~'; ++c, ++i) {
 *     WriteAscii(*pixel_writer, 8 * i, 50, c, {0, 0, 0});
 *   }
 *   '!' = 0x21，'~' = 0x7E，共 94 个可打印 ASCII 字符，
 *   每字符 8 像素宽，从 x=0 开始水平排列，y=50 一行显示。
 */
void WriteAscii(PixelWriter& writer, int x, int y, char c, const PixelColor& color) {
  // 从 hankaku.bin 中查找字符 c 的 16 字节位图数据指针
  const uint8_t* font = GetFont(c);
 
  // 若 GetFont 返回 nullptr（字符码超出字体文件范围），静默跳过，不渲染
  if (font == nullptr) {
    return;
  }
 
  // 遍历字符的 16 行（dy = 字符内行偏移，0 = 顶行，15 = 底行）
  for (int dy = 0; dy < 16; ++dy) {
    // 遍历该行的 8 列（dx = 字符内列偏移，0 = 最左列，7 = 最右列）
    for (int dx = 0; dx < 8; ++dx) {
      // 位图提取：将 font[dy] 左移 dx 位后检查最高位（bit7）
      //   结果非零 → 字符在 (dx, dy) 处有笔画，写入前景色
      //   结果为零 → 字符在 (dx, dy) 处是空白，不写像素（保留背景色）
      if ((font[dy] << dx) & 0x80u) {
        // 字符内相对坐标 (dx, dy) + 字符左上角 (x, y) = 屏幕绝对坐标
        writer.Write(x + dx, y + dy, color);
      }
    }
  }
}
// #@@range_end(write_ascii)
 