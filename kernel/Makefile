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
#     hankaku.txt
#         ↓  makefont.py
#     hankaku.bin
#         ↓  objcopy
#     hankaku.o                     （字体数据，含 _binary_hankaku_bin_* 符号）
#
#     main.cpp / graphics.cpp / font.cpp
#         ↓  clang++ $(CXXFLAGS)
#     main.o / graphics.o / font.o
#
#     newlib_support.c
#         ↓  clang $(CFLAGS)        ← 新增：C 源文件使用单独的编译规则
#     newlib_support.o              ← 新增：newlib sbrk 存根目标文件
#
#     *.o + hankaku.o + newlib_support.o
#         ↓  ld.lld -lc             ← 新增：链接 newlib libc（提供 sprintf 等）
#     kernel.elf
#
# ── 与上一版本的差异 ─────────────────────────────────────────────────────────
#
#   ① OBJS 新增 newlib_support.o
#   ② 新增 CFLAGS（C 编译器标志，比 CXXFLAGS 少 -fno-exceptions/-fno-rtti/-std=c++17）
#   ③ kernel.elf 链接命令新增 -lc（链接 newlib libc，提供 sprintf/printf 族函数）
#   ④ 新增 %.o: %.c 编译规则（用 clang 编译 C 源文件）
#   ⑤ 新增 .%.d: %.c 依赖文件生成规则（为 C 源文件生成头文件依赖追踪）
#
# =============================================================================
 
# -----------------------------------------------------------------------------
# 变量定义
# -----------------------------------------------------------------------------
 
# TARGET — 最终构建目标：内核 ELF 可执行文件
TARGET = kernel.elf
 
# OBJS — 链接所需的全部目标文件列表
#   main.o          ← main.cpp          内核主函数 KernelMain
#   graphics.o      ← graphics.cpp      像素写入器 Write() 实现
#   font.o          ← font.cpp          字体渲染 GetFont/WriteAscii/WriteString
#   hankaku.o       ← hankaku.bin       半角字体二进制（objcopy 嵌入）
#   newlib_support.o ← newlib_support.c  sbrk() 存根（满足 newlib libc 链接要求）
#
#   引入 newlib_support.o 的原因：
#     main.cpp 使用 sprintf（来自 newlib libc -lc），
#     newlib 的内存分配路径依赖 sbrk() 系统调用；
#     裸机内核无此系统调用，需提供空存根以通过链接。
OBJS = main.o graphics.o font.o hankaku.o newlib_support.o
 
# DEPENDS — 自动依赖文件（.d 文件）路径列表
#   格式为 ./<name>.d，由 clang/clang++ -MM 自动生成，
#   记录每个 .o 文件实际依赖的所有头文件，
#   通过末尾的 -include $(DEPENDS) 包含进 Makefile，补全头文件修改触发重编译的能力。
DEPENDS = $(join $(dir $(OBJS)),$(addprefix .,$(notdir $(OBJS:.o=.d))))
 
# -----------------------------------------------------------------------------
# 编译标志
# -----------------------------------------------------------------------------
 
# CFLAGS — clang 编译 .c 文件时使用的选项（新增）
#   比 CXXFLAGS 少三个 C++ 专用选项：
#     无 -fno-exceptions：C 没有异常机制，无需禁用
#     无 -fno-rtti       ：C 没有 RTTI，无需禁用
#     无 -std=c++17      ：C 使用默认 C 标准（通常 C11）
#   其余选项与 CXXFLAGS 含义相同（见下方 CXXFLAGS 详解）。
CFLAGS   += -O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone
 
# CXXFLAGS — clang++ 编译 .cpp 文件时使用的选项
#
#   -O2              : 优化等级 2，消除死代码，内联小函数（如 PixelAt）
#   -Wall            : 启用所有常见警告
#   -g               : 生成 DWARF 调试信息（QEMU + GDB 调试使用）
#   --target=x86_64-elf : 交叉编译目标：64 位 x86 裸机 ELF（无 OS）
#   -ffreestanding   : 独立环境模式，不依赖 libc/crt0，不假设 main 为入口
#   -mno-red-zone    : 禁用 x86-64 Red Zone（栈指针以下 128 字节保留区）
#                      内核中断处理不保留红区，禁用避免中断栈帧覆盖数据
#   -fno-exceptions  : 禁用 C++ 异常（需要 libunwind/EH 帧，裸机无支持）
#   -fno-rtti        : 禁用 RTTI（dynamic_cast/typeid，增加体积，内核不用）
#   -std=c++17       : 使用 C++17 标准
CXXFLAGS += -O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone \
            -fno-exceptions -fno-rtti -std=c++17
 
# LDFLAGS — ld.lld 链接时使用的选项
#
#   --entry KernelMain : 指定 ELF 入口符号，链接器将其地址写入 e_entry 字段
#                        引导加载程序读取 e_entry 并跳转到 KernelMain
#   -z norelro         : 禁用 RELRO（裸机无 OS 加载器，无法执行 mprotect）
#   --image-base 0x100000 : 内核加载基地址（1 MiB），与引导加载程序 AllocatePages 对应
#   --static           : 静态链接（裸机无动态链接器），消除 .plt/.got.plt
LDFLAGS  += --entry KernelMain -z norelro --image-base 0x100000 --static
 
# -----------------------------------------------------------------------------
# 构建规则
# -----------------------------------------------------------------------------
 
.PHONY: all
 
# all — 默认目标，触发完整内核构建
all: $(TARGET)
 
.PHONY: clean
 
# clean — 清理所有编译产物（.o 目标文件）
clean:
    rm -rf *.o
 
# kernel.elf — 链接规则：将所有目标文件链接为内核 ELF
#
#   依赖：$(OBJS) Makefile
#     任意 .o 更新或 Makefile 修改（选项变化）时重新链接。
#
#   命令：ld.lld $(LDFLAGS) -o kernel.elf $(OBJS) -lc
#     -lc（新增）：链接 newlib 的 C 标准库（libc.a）。
#       提供：sprintf/printf 族函数、字符串函数（strlen/memcpy 等）、数学函数等。
#       newlib 的内存分配路径调用 sbrk()，由 newlib_support.o 提供存根。
#       -lc 必须放在 $(OBJS) 之后（链接器按从左到右顺序解析依赖）：
#         $(OBJS) 中的符号引用 libc 中的函数 → -lc 在后面提供这些函数的定义。
#         若 -lc 放在 $(OBJS) 前面，链接器在解析 libc 时尚未看到引用，不会链接。
kernel.elf: $(OBJS) Makefile
    ld.lld $(LDFLAGS) -o kernel.elf $(OBJS) -lc
 
# %.o: %.cpp — 通用规则：将 .cpp 文件编译为 .o 目标文件
#
#   clang++：LLVM C++ 编译器，与 -lc（newlib）配合使用
#   $(CPPFLAGS)：C 预处理器标志（如 -I 头文件路径，通常由命令行传入）
#   $(CXXFLAGS)：C++ 编译器标志（见上方详解）
#   -c：只编译不链接
#   $<：第一个依赖（%.cpp 源文件）
%.o: %.cpp Makefile
    clang++ $(CPPFLAGS) $(CXXFLAGS) -c $<
 
# .%.d: %.cpp — C++ 源文件自动依赖文件生成规则
#
#   -MM：输出 Makefile 格式的头文件依赖关系（不含系统头文件），不生成目标文件
#   $(eval OBJ = ...)：在规则体内执行 Make 变量赋值
#   sed：修正 .d 文件中目标名的路径前缀
.%.d: %.cpp
    clang++ $(CPPFLAGS) $(CXXFLAGS) -MM $< > $@
    $(eval OBJ = $(<:.cpp=.o))
    sed --in-place 's|$(notdir $(OBJ))|$(OBJ)|' $@
 
# %.o: %.c — C 源文件编译规则（新增）
#
#   与 %.o: %.cpp 规则的唯一差异：
#     编译器：clang（而非 clang++）
#     编译标志：$(CFLAGS)（而非 $(CXXFLAGS)，无 -fno-exceptions/-fno-rtti/-std=c++17）
#
#   适用文件：newlib_support.c
#   使用 clang 而非 clang++ 的原因：
#     ① sbrk() 是 C 接口，避免 C++ name mangling 导致符号名不匹配
#     ② C 文件不需要 C++ 运行时特性
%.o: %.c Makefile
    clang $(CPPFLAGS) $(CFLAGS) -c $<
 
# .%.d: %.c — C 源文件自动依赖文件生成规则（新增）
#
#   与 .%.d: %.cpp 规则对应，为 .c 文件生成头文件依赖追踪。
#   使用 clang 和 $(CFLAGS)（与 C 编译规则一致）。
#   sed 修正目标路径格式（与 .cpp 规则相同逻辑）。
.%.d: %.c
    clang $(CPPFLAGS) $(CFLAGS) -MM $< > $@
    $(eval OBJ = $(<:.c=.o))
    sed --in-place 's|$(notdir $(OBJ))|$(OBJ)|' $@
 
# hankaku.bin: hankaku.txt — 字体文本转二进制
#
#   makefont.py 将 hankaku.txt（点阵字体描述文本）转换为平坦二进制：
#   每字符 16 字节，字符 c 从偏移 16*c 开始，共 256 字符 × 16 = 4096 字节。
hankaku.bin: hankaku.txt
    ../tools/makefont.py -o $@ $<
 
# hankaku.o: hankaku.bin — 将字体二进制嵌入 ELF 目标文件
#
#   objcopy -I binary -O elf64-x86-64 -B i386:x86-64：
#     将 hankaku.bin 的原始字节嵌入 ELF 目标文件，
#     自动生成三个全局符号供内核 font.cpp 使用：
#       _binary_hankaku_bin_start  → 字体数据起始地址
#       _binary_hankaku_bin_end    → 字体数据结束地址
#       _binary_hankaku_bin_size   → 文件字节数（编码在符号"地址"字段中）
hankaku.o: hankaku.bin
    objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
 
# .%.d: %.bin — 二进制文件的依赖占位规则
#
#   DEPENDS 列表包含 .hankaku.d，但 hankaku.o 来自二进制文件（无 #include 依赖）。
#   touch $@ 创建空的占位 .d 文件，防止 Make 报"No rule to make target .hankaku.d"。
.%.d: %.bin
    touch $@
 
.PHONY: depends
 
# depends — 手动触发依赖文件生成
#   $(MAKE) $(DEPENDS) 以 .d 文件列表为目标递归调用 make，
#   触发 .%.d 规则生成各源文件的头文件依赖信息。
depends:
    $(MAKE) $(DEPENDS)
 
# -include $(DEPENDS) — 将自动依赖文件包含进 Makefile
#
#   - 前缀：文件不存在时不报错（首次构建尚无 .d 文件）。
#   包含后 Make 获知头文件修改→对应 .o 需要重编译的依赖关系，
#   避免"修改头文件后 Make 不重编译"的隐蔽 bug。
-include $(DEPENDS)
 
 
 