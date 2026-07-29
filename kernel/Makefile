# =============================================================================
# MikanOS カーネル ビルドスクリプト
# =============================================================================
#
# 文件职责概述
# ─────────────────────────────────────────────────────────────────────────────
#
#   完整构建流程：
#     hankaku.txt → makefont.py → hankaku.bin → objcopy → hankaku.o
#     *.cpp → clang++ → *.o
#     *.c   → clang   → *.o     （newlib_support.c 的 sbrk 存根）
#     *.o + hankaku.o → ld.lld -lc → kernel.elf
#
# ── 与上一版本的差异 ─────────────────────────────────────────────────────────
#
#   OBJS 新增 console.o：
#     Console 类（console.cpp / console.hpp）提供多行文本输出和屏幕滚动。
#
# =============================================================================
 
# TARGET — 最终构建目标：内核 ELF 可执行文件
TARGET = kernel.elf
 
# OBJS — 链接所需的全部目标文件
#   main.o           ← main.cpp          内核主函数 KernelMain
#   graphics.o       ← graphics.cpp      像素写入器 Write() 实现（RGB/BGR）
#   font.o           ← font.cpp          字体渲染 GetFont/WriteAscii/WriteString
#   hankaku.o        ← hankaku.bin       半角字体二进制（objcopy 嵌入）
#   newlib_support.o ← newlib_support.c  sbrk() POSIX 存根（满足 -lc 链接要求）
#   console.o        ← console.cpp       Console 类（多行文本控制台，含屏幕滚动）
OBJS = main.o graphics.o font.o hankaku.o newlib_support.o console.o
 
# DEPENDS — 自动生成的头文件依赖追踪文件列表（.d 文件，点号开头隐藏）
#   通过末尾 -include $(DEPENDS) 引入，确保头文件修改触发相关 .o 重编译。
DEPENDS = $(join $(dir $(OBJS)),$(addprefix .,$(notdir $(OBJS:.o=.d))))
 
# -----------------------------------------------------------------------------
# 编译标志
# -----------------------------------------------------------------------------
 
# CFLAGS — clang 编译 .c 文件（newlib_support.c 等）时使用的选项
#   比 CXXFLAGS 少：-fno-exceptions（C 无异常）、-fno-rtti（C 无 RTTI）、-std=c++17
CFLAGS   += -O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone
 
# CXXFLAGS — clang++ 编译 .cpp 文件时使用的选项
#   -O2             : 优化等级 2
#   -Wall           : 启用所有常见警告
#   -g              : 生成 DWARF 调试信息（QEMU + GDB 使用）
#   --target=x86_64-elf : 交叉编译：64 位 x86 裸机 ELF
#   -ffreestanding  : 独立环境，不依赖 libc/crt0，不假设 main 为入口
#   -mno-red-zone   : 禁用 Red Zone（防止中断覆盖栈数据）
#   -fno-exceptions : 禁用 C++ 异常（裸机无 libunwind 支持）
#   -fno-rtti       : 禁用 RTTI（减小体积，内核不用 dynamic_cast/typeid）
#   -std=c++17      : 使用 C++17 标准
CXXFLAGS += -O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone \
            -fno-exceptions -fno-rtti -std=c++17
 
# LDFLAGS — ld.lld 链接选项
#   --entry KernelMain  : ELF 入口符号（e_entry 字段），引导加载程序读取并跳转
#   -z norelro          : 禁用 RELRO（裸机无 OS 加载器，无法执行 mprotect）
#   --image-base 0x100000: 内核加载基地址（1 MiB），与引导加载程序 AllocatePages 对应
#   --static            : 静态链接，消除 .plt/.got.plt
LDFLAGS  += --entry KernelMain -z norelro --image-base 0x100000 --static
 
# -----------------------------------------------------------------------------
# 构建规则
# -----------------------------------------------------------------------------
 
.PHONY: all
# all — 默认目标，触发完整内核构建
all: $(TARGET)
 
.PHONY: clean
# clean — 清理所有 .o 目标文件
clean:
    rm -rf *.o
 
# kernel.elf — 链接规则
#
#   -lc（链接 newlib libc）：提供 sprintf/printf 族、字符串函数等。
#   -lc 必须在 $(OBJS) 之后：链接器从左到右解析，$(OBJS) 引用 libc 符号，
#   -lc 在后提供这些符号的定义（若放在前面，链接器已扫描 libc 但未见引用，不链接）。
#   newlib_support.o 提供 sbrk() 存根，满足 newlib 内存分配路径的链接要求。
kernel.elf: $(OBJS) Makefile
    ld.lld $(LDFLAGS) -o kernel.elf $(OBJS) -lc
 
# %.o: %.cpp — C++ 源文件编译规则
#   clang++：LLVM C++ 编译器
#   $<：第一个依赖（%.cpp 源文件）
%.o: %.cpp Makefile
    clang++ $(CPPFLAGS) $(CXXFLAGS) -c $<
 
# .%.d: %.cpp — C++ 源文件头文件依赖生成
#   -MM：输出 Makefile 格式依赖（不含系统头文件），不生成目标文件
#   $(eval OBJ = ...)：在规则体内赋值 Make 变量
#   sed：修正 .d 文件中目标名的路径格式
.%.d: %.cpp
    clang++ $(CPPFLAGS) $(CXXFLAGS) -MM $< > $@
    $(eval OBJ = $(<:.cpp=.o))
    sed --in-place 's|$(notdir $(OBJ))|$(OBJ)|' $@
 
# %.o: %.c — C 源文件编译规则（用于 newlib_support.c）
#   clang（非 clang++）：C 文件无需 C++ 运行时，且避免 name mangling
#   $(CFLAGS)：C 编译标志（无 -fno-exceptions/-fno-rtti/-std=c++17）
%.o: %.c Makefile
    clang $(CPPFLAGS) $(CFLAGS) -c $<
 
# .%.d: %.c — C 源文件头文件依赖生成
#   与 .%.d: %.cpp 逻辑相同，使用 clang 和 $(CFLAGS)
.%.d: %.c
    clang $(CPPFLAGS) $(CFLAGS) -MM $< > $@
    $(eval OBJ = $(<:.c=.o))
    sed --in-place 's|$(notdir $(OBJ))|$(OBJ)|' $@
 
# hankaku.bin — 字体文本转二进制
#   makefont.py：将 hankaku.txt 位图描述转换为平坦二进制（每字符 16 字节）
hankaku.bin: hankaku.txt
    ../tools/makefont.py -o $@ $<
 
# hankaku.o — 字体二进制嵌入 ELF 目标文件
#   objcopy 生成三个链接符号供 font.cpp 使用：
#     _binary_hankaku_bin_start  → 字体数据起始地址
#     _binary_hankaku_bin_end    → 字体数据结束地址
#     _binary_hankaku_bin_size   → 文件字节数（编码在符号"地址"字段）
hankaku.o: hankaku.bin
    objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
 
# .%.d: %.bin — 二进制文件的依赖占位规则
#   touch $@ 创建空 .d 文件，防止 Make 报"No rule to make target .hankaku.d"
.%.d: %.bin
    touch $@
 
.PHONY: depends
# depends — 手动触发所有 .d 依赖文件生成
depends:
    $(MAKE) $(DEPENDS)
 
# -include：文件不存在时不报错（首次构建尚无 .d 文件）
# 包含后 Make 知道头文件修改需重编译对应 .o
-include $(DEPENDS)
 
 