# mikanos
MikanOS: An Educational Operating System

# Files

- MikanLoaderPkg
    - The MikanOS loader as a UEFI Application

一张静态说明图来直观呈现 `osbook_day05a` 的改进。

![alt text](image.png)

## 结论

`osbook_day05a` 相比其前一个标签 `osbook_day04d`，改动很小但标志着**第 5 章“字符/字体绘制”的开始**。两者之间共 4 个提交（`draw 'A' for test`、`delete review tags`、`add a font for 'A'`、`remove review tags`），核心改进如下：

**1. 引入字体数据 `kFontA[16]`（kernel/main.cpp）**

- 新增字符 'A' 的 8×16 位图字体数组，16 个字节，每字节一行、每位一个像素，是后续 ASCII 字库的基础雏形。

**2. 新增字符绘制函数 `WriteAscii()`（kernel/main.cpp）**

- 之前内核无法显示任何文字；现在通过逐位扫描字体数组，对置位像素调用 `PixelWriter::Write()` 落点，从而把 'A' 渲染到帧缓冲。
- 目前仅支持 'A'（`if (c != 'A') return;`），其余字符尚需后续版本补齐。

**3. KernelMain 实际输出字符**

- 在渐变测试色带之上，于坐标 (50,50) 和 (58,50) 各绘制一个黑色 'A'，首次实现了“在屏幕上写出文字”。

**4. 清理审查标记（kernel + loader，纯整理）**

- 删除了 `MikanLoaderPkg/Main.c` 和 `kernel/main.cpp` 中大量 `#@@range_begin/end` 审查注释标签，无功能变化。

**一句话总结**：`osbook_day05a` 为内核新增了“位图字体 + 逐位写点”的字符绘制能力，是 Day 5 字体渲染功能的起点；同时清理了此前留下的书稿审查注释。