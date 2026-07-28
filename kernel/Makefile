 
# =============================================================================
# MikanOS カーネル ビルドスクリプト
# =============================================================================
#
# 文件职责概述
# ─────────────────────────────────────────────────────────────────────────────
#
#   本 Makefile 负责将内核源代码编译、链接为可被引导加载程序加载的 ELF 文件。
#
#   完整构建流程：
#     hankaku.txt                     （半角字体描述文本）
#         ↓  makefont.py
#     hankaku.bin                     （打包好的 8×16 字体二进制）
#         ↓  objcopy
#     hankaku.o                       （嵌入字体数据的目标文件，含 _binary_* 符号）
#
#     main.cpp / graphics.cpp / font.cpp
#         ↓  clang++
#     main.o / graphics.o / font.o    （普通 C++ 目标文件）
#
#     main.o + graphics.o + font.o + hankaku.o
#         ↓  ld.lld
#     kernel.elf                      （内核可执行文件，引导加载程序加载并跳转的目标）
#
# =============================================================================
 
# -----------------------------------------------------------------------------
# 变量定义
# -----------------------------------------------------------------------------
 
# TARGET — 最终构建目标：内核 ELF 可执行文件
#   ELF（Executable and Linkable Format）是 Linux/x86-64 标准可执行格式。
#   引导加载程序 Main.c 通过以下步骤加载此文件：
#     1. 读取 ELF 头（Elf64_Ehdr）中的程序头表（Program Headers）
#     2. 按 PT_LOAD 段将数据复制到 p_vaddr 指定的物理地址
#     3. 读取 e_entry（偏移 0x18）获取 KernelMain 入口地址并跳转
TARGET = kernel.elf
 
# OBJS — 链接所需的全部目标文件列表
#   main.o     ← main.cpp     内核主函数 KernelMain
#   graphics.o ← graphics.cpp 像素写入器 Write() 实现（RGB / BGR 字节序）
#   font.o     ← font.cpp     字体渲染 GetFont() / WriteAscii() 实现
#   hankaku.o  ← hankaku.bin  半角字体二进制数据（objcopy 嵌入，含 _binary_* 符号）
#
#   hankaku.o 的构建规则特殊（不走 %.cpp → %.o 的通用规则），见下方专属规则。
OBJS = main.o graphics.o font.o hankaku.o
 
# DEPENDS — 自动依赖文件（.d 文件）路径列表，格式为 ./<name>.d
#
#   Make 的依赖关系由此处声明的规则静态定义；但 C/C++ 源文件之间存在动态依赖
#   （#include 关系），若头文件改变而 Makefile 不知道，Make 不会重新编译。
#   .d 文件由 clang++ -MM 自动生成，记录每个 .o 文件实际依赖的所有头文件，
#   在最后通过 -include $(DEPENDS) 包含进 Makefile，补全隐式头文件依赖。
#
#   公式解析：
#     $(OBJS:.o=.d)          将 OBJS 中每个 .o 后缀替换为 .d
#                            → "main.d graphics.d font.d hankaku.d"
#     $(notdir ...)          去除路径前缀（本项目均在同级目录，无实际效果）
#     $(addprefix .,  ...)   在每个文件名前加点 → ".main.d .graphics.d ..."
#     $(dir $(OBJS))         提取各目标文件的目录部分（均为 "./"）
#     $(join dirs, names)    将目录列表和文件名列表逐一拼接
#                            → "./. main.d ./. graphics.d ..."（实际为 "./.main.d"）
#
#   最终 DEPENDS = "./.main.d ./.graphics.d ./.font.d ./.hankaku.d"
#   以点（.）开头使 .d 文件在 ls 时隐藏，保持目录整洁。
DEPENDS = $(join $(dir $(OBJS)),$(addprefix .,$(notdir $(OBJS:.o=.d))))
 
# -----------------------------------------------------------------------------
# 编译标志（C++ 编译器选项）
# -----------------------------------------------------------------------------
 
# CXXFLAGS — clang++ 编译 .cpp → .o 时使用的选项
#   += 语法：追加到已有变量（若命令行传入了 CXXFLAGS，保留原有值）
#
#   -O2
#     开启优化等级 2（较高优化，但不含激进的体积/速度优化）。
#     消除死代码，内联小函数（如 PixelAt()），生成更高效的机器码。
#     保留 -g 调试信息，与优化兼容（部分变量会被优化消除，调试时须注意）。
#
#   -Wall
#     启用所有常见警告（unused variable、sign comparison、missing return 等）。
#     将潜在 bug 在编译阶段暴露，是良好工程实践的基础。
#
#   -g
#     生成 DWARF 调试信息，嵌入目标文件。
#     QEMU + GDB 远程调试时可用于单步执行、查看变量、设置断点。
#
#   --target=x86_64-elf
#     指定交叉编译目标三元组（target triple）：
#       架构 = x86_64（64 位 x86）
#       厂商 = 省略（通用）
#       OS   = elf（裸机 ELF，无操作系统）
#     clang 是多目标编译器，同一个 clang 可编译 ARM、RISC-V 等不同架构，
#     --target 显式指定目标，避免编译成宿主机（x86_64-linux-gnu）格式。
#
#   -ffreestanding
#     "独立环境"模式：声明代码不依赖宿主操作系统提供的标准库（libc、libstdc++）。
#     编译器不假设 main() 是程序入口，不链接 crt0.o / crti.o 等启动文件。
#     内核是裸机程序，不能调用 malloc、printf 等需要 OS 支持的函数，
#     此标志防止编译器悄悄插入对这些函数的隐式调用。
#
#   -mno-red-zone
#     禁用 x86-64 System V ABI 定义的"红区"（Red Zone）优化。
#
#     红区：RSP（栈指针）之下 128 字节的保留区域，叶子函数（leaf function）
#           可直接使用此区域存放临时变量，无需 SUB RSP, N 调整栈指针。
#           在用户态程序中性能有利，因为信号处理在用户栈上运行，
#           内核会在切换前保存 RSP 以外的红区。
#
#     内核中禁用的原因：
#       x86-64 硬件中断发生时，CPU 直接将 RSP-128 以下作为红区，
#       但若内核中断处理程序使用了红区（因为 CPU 不会因中断保留红区），
#       中断栈帧会覆盖正在使用的红区数据，导致栈损坏（难以复现的崩溃）。
#       -mno-red-zone 强制所有函数用 SUB RSP, N 显式分配栈帧，彻底规避此问题。
#
#   -fno-exceptions
#     禁用 C++ 异常机制（try / catch / throw）。
#     C++ 异常需要：① 运行时库支持（libunwind / libstdc++）②  EH 帧（.eh_frame 段）。
#     裸机内核环境没有这些运行时支持，禁用异常可：
#       - 消除 EH 帧（减小内核体积）
#       - 避免链接器报"undefined reference to __gxx_personality_v0"
#       - 消除异常处理的隐式代码生成开销
#
#   -fno-rtti
#     禁用 C++ 运行时类型信息（RTTI，Run-Time Type Information）。
#     RTTI 支持 dynamic_cast<> 和 typeid() 运算符，需要在每个多态类的
#     虚函数表中附加类型描述符（type_info 对象），增加内核体积。
#     本内核不使用 dynamic_cast / typeid，禁用 RTTI 可减小内核体积，
#     同时避免链接 libstdc++ 中的类型信息相关符号。
#
#   -std=c++17
#     指定 C++ 标准版本为 C++17。
#     主要用到的 C++17 特性：结构化绑定、if constexpr、内联变量等。
#     本项目中 using PixelWriter::PixelWriter（继承构造函数）是 C++11 特性，
#     但显式指定 c++17 确保所有现代语法可用。
CXXFLAGS += -O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone \
            -fno-exceptions -fno-rtti -std=c++17
 
# LDFLAGS — ld.lld 链接 .o → kernel.elf 时使用的选项
#   += 语法：追加到已有变量（保留命令行传入的原有值）
#
#   --entry KernelMain
#     指定 ELF 入口点符号名为 "KernelMain"。
#     链接器将此符号的虚拟地址写入 ELF 头的 e_entry 字段（偏移 0x18）。
#     引导加载程序读取 e_entry 并以函数指针方式调用：
#       UINT64 entry_addr = *(UINT64*)(kernel_first_addr + 24);
#       ((void (*)(const FrameBufferConfig*))entry_addr)(&config);
#     KernelMain 使用 extern "C" 禁用 C++ name mangling，
#     确保链接器能找到名为 "KernelMain" 的符号。
#
#   -z norelro
#     禁用 RELRO（RELocation Read-Only）安全特性。
#     RELRO 将链接后的重定位表标记为只读（PROT_READ），防止攻击者篡改 GOT。
#     裸机内核没有 OS 加载器来执行 mprotect() 操作，此特性无意义，禁用避免
#     链接器生成 PT_GNU_RELRO 程序头（简化内核的程序头结构）。
#
#   --image-base 0x100000
#     指定内核 ELF 的加载基地址为 0x100000（物理地址 1 MiB）。
#     x86 实模式（Legacy）传统上将 0x00000–0xFFFFF 用于 BIOS/IVT 等，
#     1 MiB 以上是可用的"高内存"区域。
#     引导加载程序使用 AllocatePages(AllocateAddress, ..., &kernel_first_addr)
#     在此精确物理地址分配内存，并将 PT_LOAD 段复制到对应位置。
#     所有代码中的绝对地址（全局变量地址、函数指针等）均以此基地址为基准计算。
#
#   --static
#     生成静态链接的可执行文件（不依赖动态库 .so）。
#     裸机内核环境没有动态链接器（ld-linux.so），所有代码必须静态包含在 ELF 中。
#     此选项还消除 .plt（Procedure Linkage Table）和 .got.plt（GOT for PLT）节，
#     简化内核的内存布局。
LDFLAGS  += --entry KernelMain -z norelro --image-base 0x100000 --static
 
# -----------------------------------------------------------------------------
# 构建规则
# -----------------------------------------------------------------------------
 
# .PHONY — 声明伪目标（phony target）
#   Make 默认将目标名视为文件名，若存在同名文件则认为目标已是最新而跳过构建。
#   .PHONY 告知 Make：这些目标永远需要执行，与同名文件无关。
#   all、clean、depends 都不对应实际文件，故声明为伪目标。
.PHONY: all
 
# all — 默认目标（执行 make 时若不指定目标，构建此目标）
#   依赖 $(TARGET) = kernel.elf，触发完整内核构建流程。
#   Makefile 第一个出现的非伪目标（或第一个 .PHONY 后的目标）为默认目标，
#   此处显式声明 all 为第一个目标以明确意图。
all: $(TARGET)
 
.PHONY: clean
 
# clean — 清理所有编译产物（.o 目标文件）
#   rm -rf *.o：强制递归删除当前目录下所有 .o 文件，-f 忽略不存在时的错误。
#   注意：kernel.elf 和 hankaku.bin 未被清理（如需彻底清理，需手动删除或扩展规则）。
clean:
    rm -rf *.o
 
# kernel.elf — 链接规则：将所有目标文件链接为内核 ELF
#
#   依赖：$(OBJS) Makefile
#     $(OBJS) = main.o graphics.o font.o hankaku.o
#       任何 .o 文件更新时重新链接（确保 kernel.elf 始终是最新版本）。
#     Makefile 本身：若编译/链接选项改变（修改 Makefile），重新链接。
#
#   命令：ld.lld $(LDFLAGS) -o kernel.elf $(OBJS)
#     ld.lld：LLVM 的链接器（与 clang++ 配套，支持交叉链接目标 ELF）。
#             比 GNU ld 更快，兼容性好，支持 --entry、--image-base 等选项。
#     -o kernel.elf：指定输出文件名。
#     $(OBJS)：所有输入目标文件，链接器合并其中的代码/数据节（.text, .data, .bss 等）。
#
#   链接产物 kernel.elf 的典型节（Section）布局：
#     .text   — 所有 .cpp 的机器码 + hankaku.o 无代码（仅数据）
#     .rodata — 只读数据（如虚函数表、字符串字面量）
#     .data   — 已初始化全局变量（如 pixel_writer 指针）
#     .bss    — 未初始化全局变量（如 pixel_writer_buf 数组，运行时清零）
#     .binary — hankaku.o 中 objcopy 嵌入的字体二进制数据
kernel.elf: $(OBJS) Makefile
    ld.lld $(LDFLAGS) -o kernel.elf $(OBJS)
 
# %.o: %.cpp — 通用规则：将任意 .cpp 文件编译为对应的 .o 目标文件
#
#   模式规则（Pattern Rule）：% 是通配符，匹配任意字符串。
#     main.cpp     → main.o
#     graphics.cpp → graphics.o
#     font.cpp     → font.o
#   （hankaku.o 不走此规则，有专属规则，见下方）
#
#   依赖：%.cpp Makefile
#     源文件改变时重新编译；Makefile 改变（编译选项修改）时也重新编译。
#
#   命令：clang++ $(CPPFLAGS) $(CXXFLAGS) -c $<
#     clang++：LLVM 的 C++ 编译器前端。
#     $(CPPFLAGS)：C 预处理器标志（如 -I 头文件搜索路径），通常由命令行传入。
#     $(CXXFLAGS)：见上方 CXXFLAGS 详解（-O2 -Wall -g --target=... 等）。
#     -c：只编译不链接，生成 .o 目标文件（不调用链接器）。
#     $<：Make 自动变量，表示第一个依赖文件（即 %.cpp 源文件）。
#         等价于将 $< 展开后的文件名（如 main.cpp）传给 clang++。
%.o: %.cpp Makefile
    clang++ $(CPPFLAGS) $(CXXFLAGS) -c $<
 
# .%.d: %.cpp — 自动依赖文件生成规则
#
#   作用：分析 .cpp 文件的 #include 依赖关系，生成 .d 文件。
#   .d 文件内容示例（font.d）：
#     font.o: font.cpp font.hpp graphics.hpp frame_buffer_config.hpp
#   当 font.hpp 或 graphics.hpp 修改时，Make 通过 .d 文件知道需要重新编译 font.o。
#
#   依赖：%.cpp
#     源文件改变时重新生成依赖文件（因为 #include 列表可能改变）。
#     注意：此规则不依赖 Makefile 本身（避免循环依赖）。
#
#   命令详解：
#
#   clang++ $(CPPFLAGS) $(CXXFLAGS) -MM $< > $@
#     -MM：只输出 Makefile 格式的依赖关系，不生成目标文件，不包含系统头文件。
#          输出格式：目标文件.o: 源文件.cpp 头文件1.hpp 头文件2.hpp ...
#     $<：第一个依赖（%.cpp 源文件）
#     > $@：将输出重定向到目标文件（$@ = .%.d，如 .font.d）
#
#   $(eval OBJ = $(<:.cpp=.o))
#     $(eval ...)：在规则命令中执行 Make 变量赋值（通常变量赋值只能在规则外）。
#     $(<:.cpp=.o)：将 $<（如 font.cpp）的 .cpp 后缀替换为 .o → "font.o"。
#     OBJ = font.o（用于下一行的 sed 命令）。
#
#   sed --in-place 's|$(notdir $(OBJ))|$(OBJ)|' $@
#     -MM 生成的依赖文件中目标名只有文件名（如 "font.o:"），没有路径前缀。
#     若目标文件在子目录中（如 build/font.o），依赖文件中的路径需要匹配。
#     sed 将 "font.o" 替换为完整路径 "$(OBJ)"（本项目在同一目录，替换前后相同）。
#     --in-place：直接修改 .d 文件（不创建临时文件）。
#     's|...|...|'：使用 | 作为分隔符（避免路径中的 / 需要转义）。
.%.d: %.cpp
    clang++ $(CPPFLAGS) $(CXXFLAGS) -MM $< > $@
    $(eval OBJ = $(<:.cpp=.o))
    sed --in-place 's|$(notdir $(OBJ))|$(OBJ)|' $@
 
# hankaku.bin: hankaku.txt — 字体文本转二进制
#
#   hankaku.txt：
#     人类可读的字体描述文件，每个字符用 16 行文本描述其 8×16 位图。
#     格式示例（字符 'A'）：
#       ........   ← 第 0 行（. = 空白，@ = 像素）
#       ...@@...
#       ..@..@..
#       .@....@.
#       .@@@@@@.
#       .@....@.
#       .@....@.
#       ........
#       （省略剩余行）
#
#   makefont.py：
#     Python 脚本，读取 hankaku.txt，将每行 8 个字符（./@ 或 0/1）
#     转换为一个字节（'@'=1→bit，'.'=0→bit），每字符生成 16 字节，
#     按字符码顺序（0x00 ~ 0xFF）打包写入输出文件。
#
#   -o $@：输出到 $@（目标文件 hankaku.bin）
#   $<   ：第一个依赖（hankaku.txt）
#
#   hankaku.bin 格式：
#     偏移 16*c 处存放字符码 c 的 16 字节位图（共 256 字符 × 16 字节 = 4096 字节）。
hankaku.bin: hankaku.txt
    ../tools/makefont.py -o $@ $<
 
# hankaku.o: hankaku.bin — 将字体二进制嵌入 ELF 目标文件
#
#   objcopy 将任意二进制文件转换为目标文件格式，嵌入其字节内容并生成三个链接符号。
#
#   命令：objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
#
#     -I binary
#       指定输入格式为"纯二进制"（raw binary），即逐字节读取 $< 的内容，
#       不做任何格式解析（不当作 ELF/PE/COFF）。
#
#     -O elf64-x86-64
#       指定输出格式为 64 位 x86 ELF，与内核其他 .o 文件格式一致，
#       ld.lld 可以直接链接。
#
#     -B i386:x86-64
#       指定目标体系结构（BFD architecture）为 x86-64，
#       与 -O elf64-x86-64 配合指定完整的目标平台。
#
#     $<：第一个依赖（hankaku.bin）
#     $@：目标文件（hankaku.o）
#
#   objcopy 自动生成的三个全局符号（符号名由文件名派生，. 和 / 替换为 _）：
#
#     _binary_hankaku_bin_start
#       ：字体数据第一个字节在内存中的地址。
#        取地址 &_binary_hankaku_bin_start 得到 uint8_t* 类型的字体起始指针。
#
#     _binary_hankaku_bin_end
#       ：字体数据最后字节的下一个地址（past-the-end 指针）。
#        数据大小 = &end - &start（字节数）。
#
#     _binary_hankaku_bin_size
#       ：★ 特殊符号 ★ — 其"符号值"（链接器地址字段）= hankaku.bin 文件字节数。
#        正确读取方式：reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)
#        错误读取方式：直接读取符号值（uint8_t）会得到 0x1000 地址处的数据，而非大小。
#
#   font.cpp 使用这三个符号实现 GetFont(char c)：
#     index = 16 * (unsigned int)c;
#     if (index >= reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)) return nullptr;
#     return &_binary_hankaku_bin_start + index;
hankaku.o: hankaku.bin
    objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
 
# .%.d: %.bin — 二进制文件的依赖占位规则
#
#   .d 文件生成规则 .%.d: %.cpp 仅处理 .cpp 文件；
#   hankaku.o 由 hankaku.bin 生成（非 .cpp），但 DEPENDS 列表包含 .hankaku.d，
#   若不为 .bin 文件提供对应规则，Make 会报"No rule to make target .hankaku.d"。
#
#   touch $@：创建一个空的 .hankaku.d 文件（若已存在则更新时间戳）。
#   空 .d 文件表示"hankaku.o 无额外头文件依赖"（符合事实：hankaku.o 来自二进制，无 #include）。
#   此规则是一个占位符，防止 Make 在找不到 .hankaku.d 时报错。
.%.d: %.bin
    touch $@
 
.PHONY: depends
 
# depends — 手动触发依赖文件生成
#
#   $(MAKE) $(DEPENDS)
#     $(MAKE)：递归调用当前 make 命令（继承 -j 并发数等选项）。
#     $(DEPENDS)：将 DEPENDS 列表（./.main.d ./.graphics.d ./.font.d ./.hankaku.d）
#                 作为构建目标传给 make，触发 .%.d 规则生成各 .d 文件。
#
#   通常在以下情况手动执行 make depends：
#     - 添加了新的 #include 语句
#     - 首次克隆仓库（尚无 .d 文件）
#   正常增量构建无需手动执行（-include $(DEPENDS) 会在需要时自动更新）。
depends:
    $(MAKE) $(DEPENDS)
 
# -include $(DEPENDS) — 将自动依赖文件包含进 Makefile
#
#   -include（带前缀 -）：
#     若 .d 文件不存在，不报错，静默跳过（首次构建时 .d 文件尚未生成）。
#     普通 include 在文件不存在时会报错并终止构建。
#
#   每个 .d 文件内容形如：
#     main.o: main.cpp graphics.hpp font.hpp frame_buffer_config.hpp
#   被 include 后，Make 获知：若 graphics.hpp 改变，则 main.o 需要重新编译。
#
#   若无此机制，修改头文件后只有修改 .cpp 才能触发重编译，
#   头文件改动会被 Make 忽略，导致编译结果与源码不一致（隐蔽的增量编译 bug）。
-include $(DEPENDS)
 
 
 
 