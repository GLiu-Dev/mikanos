/**
 * @file newlib_support.c
 *
 * newlib C 标准库支持存根（Stub）文件
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   newlib 是为裸机/嵌入式环境设计的 C 标准库，MikanOS 通过 -lc 链接它
 *   以使用 sprintf 等标准函数。newlib 的部分函数依赖操作系统提供的底层系统调用，
 *   在裸机环境下需要手动提供"存根"（stub）实现。
 *
 * ── 为何需要 sbrk ────────────────────────────────────────────────────────────
 *
 *   newlib 的内存分配路径（malloc / printf 内部分配等）调用 sbrk() 扩展堆。
 *   若不提供 sbrk，链接 -lc 时报：
 *     undefined reference to `sbrk'
 *
 *   本内核当前仅使用 sprintf（格式化到已有缓冲区，不调用 malloc），
 *   sbrk 实际上不会被调用，但符号必须存在以通过链接。
 *
 * ── 与上一版本的差异 ─────────────────────────────────────────────────────────
 *
 *   旧版：return NULL;
 *     返回 NULL 不是 sbrk 失败的标准 POSIX 约定（POSIX 规定失败返回 (void*)-1）。
 *     虽然在当前"sbrk 不被实际调用"的场景下无影响，但不规范。
 *
 *   新版：errno = ENOMEM; return (caddr_t)-1;
 *     符合 POSIX 标准的 sbrk 失败语义：
 *       ① errno = ENOMEM：设置错误码为"内存不足"
 *       ② return (caddr_t)-1：返回 -1 强转为地址类型，是 POSIX 规定的失败返回值
 *     这使得如果将来 sbrk 被实际调用（如意外触发 malloc），
 *     newlib 会正确解读失败并向调用方报告内存分配失败，而非产生未定义行为。
 *
 * ── 为什么是 .c 而非 .cpp ────────────────────────────────────────────────────
 *
 *   sbrk 是 POSIX C 接口，以 C 编写避免 C++ name mangling，
 *   确保 newlib 通过 C 链接约定找到此符号。
 *   Makefile 的 %.o: %.c 规则用 clang（而非 clang++）编译本文件。
 */
 
#include <errno.h>       // errno（全局错误码变量）、ENOMEM（内存不足错误码）
#include <sys/types.h>   // caddr_t：Core ADDRess Type，通常为 char*
 
/**
 * sbrk — 堆扩展系统调用存根（Stub）
 *
 * @param incr  请求扩展堆的字节数（正值=扩展，负值=收缩；本存根忽略此参数）
 * @return      失败时返回 (caddr_t)-1，并设置 errno = ENOMEM
 *
 * ── POSIX sbrk 语义 ──────────────────────────────────────────────────────────
 *
 *   正常实现：
 *     sbrk(n) 将"程序断点"（program break，堆顶指针）向上移动 n 字节，
 *     返回移动前的旧断点地址（即新分配内存的起始地址）。
 *     失败（地址空间不足）时：errno = ENOMEM，return (caddr_t)-1。
 *
 *   本存根：
 *     直接模拟失败情况，告知 newlib"无可用堆内存"。
 *     newlib 的 malloc 收到 (caddr_t)-1 后返回 NULL（内存分配失败），
 *     调用方应检查 NULL 并处理分配失败。
 *
 * ── 将来实现真正 sbrk 的方案 ─────────────────────────────────────────────────
 *
 *   若内核将来需要 malloc 支持，可在内核中预留一块静态内存作为堆：
 *
 *     static char heap[1024 * 1024];   // 1 MiB 内核堆
 *     static char* heap_end = heap;
 *
 *     caddr_t sbrk(int incr) {
 *       char* prev = heap_end;
 *       if (heap_end + incr > heap + sizeof(heap)) {
 *         errno = ENOMEM;
 *         return (caddr_t)-1;          // 堆溢出
 *       }
 *       heap_end += incr;
 *       return (caddr_t)prev;          // 返回旧的堆顶（新分配块的起始）
 *     }
 */
caddr_t sbrk(int incr) {
  errno = ENOMEM;        // 设置错误码：内存不足（POSIX 规定的失败语义）
  return (caddr_t)-1;   // 返回 -1 强转为地址类型（POSIX sbrk 失败的标准返回值）
}
 