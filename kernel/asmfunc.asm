; asmfunc.asm
; MikanOS 底层汇编函数实现
; System V AMD64 ABI (Linux / macOS 64位调用约定)
; 参数传入顺序：RDI, RSI, RDX, RCX, R8, R9
bits 64                 ; 生成64位长模式指令
section .text           ; 代码段

; -----------------------------------------------------------------------------
; void IoOut32(uint16_t addr, uint32_t data);
; 向x86 I/O端口输出32位数据
global IoOut32
IoOut32:
    mov dx, di        ; RDI(第1参数)=addr → DX(I/O端口号，必须16位)
    mov eax, esi      ; RSI(第2参数)=data → EAX，out指令使用EAX传递32位数据
    out dx, eax       ; out dx,eax：向端口dx写入eax
    ret

; -----------------------------------------------------------------------------
; uint32_t IoIn32(uint16_t addr);
; 从I/O端口读取32位数据，返回值存EAX
global IoIn32
IoIn32:
    mov dx, di        ; RDI=addr → DX
    in eax, dx        ; 从端口dx读入数据到EAX
    ret

; -----------------------------------------------------------------------------
; uint16_t GetCS(void);
; 获取当前CS（代码段选择子）
global GetCS
GetCS:
    xor eax, eax      ; 清空EAX（同时RAX高32位清零）
    mov ax, cs        ; 将CS段选择子送入AX
    ret               ; 返回值在AX/EAX

; -----------------------------------------------------------------------------
; void LoadIDT(uint16_t limit, uint64_t offset);
; 加载中断描述符表 IDTR寄存器
; 参数：RDI=limit(IDT界限), RSI=offset(IDT基地址)
global LoadIDT
LoadIDT:
    push rbp
    mov rbp, rsp      ; 建立栈帧
    sub rsp, 10       ; 在栈上分配10字节临时空间：IDTR结构 [2字节limit][8字节offset]
    mov [rsp], di     ; 低2字节：limit
    mov [rsp + 2], rsi; 后面8字节：IDT线性地址
    lidt [rsp]        ; lidt 内存操作数，加载IDTR
    mov rsp, rbp
    pop rbp
    ret

; #@@range_begin(load_gdt)
; -----------------------------------------------------------------------------
; void LoadGDT(uint16_t limit, uint64_t offset);
; 加载全局描述符表 GDTR寄存器
; 参数：RDI=limit(GDT界限), RSI=offset(GDT基地址)
global LoadGDT
LoadGDT:
    push rbp
    mov rbp, rsp
    sub rsp, 10       ; GDTR结构同样10字节：2字节limit + 8字节地址
    mov [rsp], di
    mov [rsp + 2], rsi
    lgdt [rsp]        ; lgdt 加载GDTR
    mov rsp, rbp
    pop rbp
    ret
; #@@range_end(load_gdt)

; #@@range_begin(set_cs)
; -----------------------------------------------------------------------------
; void SetCSSS(uint16_t cs, uint16_t ss);
; 设置CS代码段选择子、SS栈段选择子
; ⚠️重点：CS不能直接mov修改，必须使用远返回retf/远跳转
; 参数：RDI=新CS，RSI=新SS
global SetCSSS
SetCSSS:
    push rbp
    mov rbp, rsp
    mov ss, si        ; SS可以直接赋值，先设置栈段
    mov rax, .next    ; 把标签.next的地址存入rax（新RIP）

    ; 构造retf需要的栈布局： [CS][RIP]
    push rdi          ; 压入新CS
    push rax          ; 压入新指令地址（.next）
    o64 retf          ; 64位远返回：弹出RIP，再弹出CS，同时更新CS与RIP
.next:
    mov rsp, rbp
    pop rbp
    ret
; #@@range_end(set_cs)

; #@@range_begin(set_dsall)
; -----------------------------------------------------------------------------
; void SetDSAll(uint16_t value);
; 批量设置 DS ES FS GS 四个数据段寄存器
; 参数 RDI = 段选择子
global SetDSAll
SetDSAll:
    mov ds, di
    mov es, di
    mov fs, di
    mov gs, di
    ret
; #@@range_end(set_dsall)

; #@@range_begin(set_cr3)
; -----------------------------------------------------------------------------
; void SetCR3(uint64_t value);
; 设置CR3寄存器（四级分页根页表PML4物理地址）
; 参数 RDI = PML4物理地址
global SetCR3
SetCR3:
    mov cr3, rdi
    ret
; #@@range_end(set_cr3)

; #@@range_begin(set_main_stack)
; -----------------------------------------------------------------------------
; 内核入口点，loader跳转至此
; extern 声明外部符号（在ld脚本定义）
extern kernel_main_stack      ; 内核栈起始地址
extern KernelMainNewStack     ; C++内核主函数
global KernelMain
KernelMain:
    ; 设置栈指针：栈向低地址增长，栈顶放在栈区域上方
    mov rsp, kernel_main_stack + 1024 * 1024
    call KernelMainNewStack     ; 调用C++内核入口 KernelMainNewStack

.fin:
    hlt                         ; CPU停机指令
    jmp .fin                    ; 停机后如果被中断唤醒，再次停机
; #@@range_end(set_main_stack)