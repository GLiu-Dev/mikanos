/**
 * @file newlib_support.c
 *
 * newlib C 标准库支持存根（Stub）文件
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   newlib 是专为嵌入式系统和裸机（bare-metal）环境设计的 C 标准库，
 *   MikanOS 通过 -lc 链接 newlib 以使用 sprintf 等标准 C 函数。
 *
 *   newlib 的部分函数（如内存分配相关）依赖操作系统提供的底层系统调用。
 *   在裸机内核环境中，这些系统调用不存在，需要手动提供"存根"（stub）实现，
 *   以满足链接器对符号的要求，同时告知 newlib"此功能在当前环境不可用"。
 *
 * ── 为什么需要 sbrk ──────────────────────────────────────────────────────────
 *
 *   newlib 的 malloc / printf（内部动态分配）/ 其他堆操作均依赖 sbrk() 来扩展堆。
 *   sbrk()（set break）是 Unix 传统的堆增长系统调用：
 *     sbrk(n)：将"程序断点"（堆顶）向上移动 n 字节，返回移动前的旧断点地址。
 *
 *   若不提供 sbrk，链接 -lc 时会报：
 *     undefined reference to `sbrk'
 *
 *   本内核当前阶段只使用 sprintf（格式化到已有缓冲区），不使用 malloc/动态堆，
 *   因此 sbrk 只需返回 NULL 表示"无可分配内存"即可满足链接要求。
 *   若将来需要 malloc，需要实现真正的 sbrk（管理一段内核静态内存区域作为堆）。
 *
 * ── 为什么是 .c 而非 .cpp ─────────────────────────────────────────────────────
 *
 *   sbrk 是 POSIX C 接口，以 C 语言编写避免 C++ name mangling，
 *   确保 newlib 能通过 C 链接约定找到此符号。
 *   kernel/Makefile 中的 %.o: %.c 规则使用 clang（而非 clang++）编译本文件。
 */
 
#include <sys/types.h>   /* caddr_t：Core ADDRess Type，通常为 char*，表示内存地址 */
 
/**
 * sbrk — 堆扩展系统调用存根（Stub）
 *
 * @param incr  请求扩展的字节数（newlib 内部调用时传入正值；shrink 时传入负值）
 * @return      caddr_t（即 char*）：
 *                正常实现返回旧的堆顶地址（扩展前的 break 指针）；
 *                本存根返回 NULL，表示堆内存不可用。
 *
 * ── 返回 NULL 的含义 ─────────────────────────────────────────────────────────
 *
 *   newlib 的 malloc 看到 sbrk 返回 NULL（或失败标志）后，会使 malloc 返回 NULL，
 *   表示内存分配失败。对于仅使用 sprintf（不调用 malloc）的场景，
 *   sbrk 永远不会被实际调用，返回值无关紧要，但符号必须存在以通过链接。
 *
 * ── 若将来需要真正的 sbrk ────────────────────────────────────────────────────
 *
 *   需要在内核中预留一块静态内存区域作为堆，并用指针追踪当前堆顶：
 *
 *     static char heap[1024 * 1024];   // 1 MiB 内核堆
 *     static char* heap_end = heap;
 *
 *     caddr_t sbrk(int incr) {
 *       char* prev = heap_end;
 *       heap_end += incr;
 *       if (heap_end > heap + sizeof(heap)) {
 *         return (caddr_t)-1;  // ENOMEM
 *       }
 *       return prev;
 *     }
 */
caddr_t sbrk(int incr) {
  return NULL;  /* 当前内核无堆管理器，返回 NULL 表示不可分配 */
}
 