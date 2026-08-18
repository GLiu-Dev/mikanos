#pragma once
#include <stdint.h>

/**
 * @brief asmfunc.h
 * 声明用汇编实现的底层硬件操作函数
 * extern "C"：关闭C++名称修饰，保证C++代码可以正常调用汇编函数
 * 这些函数实现在 asmfunc.asm，直接操作CPU指令、端口、系统寄存器
 */
extern "C" {
  /**
   * @brief 向I/O端口输出32位数据（out dword）
   * @param addr I/O端口地址（16位）
   * @param data 要写入的32位数据
   * x86独有IO端口寻址，常用于PCI配置空间、传统硬件控制器
   */
  void IoOut32(uint16_t addr, uint32_t data);

  /**
   * @brief 从I/O端口读取32位数据（in dword）
   * @param addr I/O端口地址
   * @return 读取到的32位值
   */
  uint32_t IoIn32(uint16_t addr);

  /**
   * @brief 获取当前CS(代码段寄存器)的值
   * @return cs寄存器数值（段选择子）
   */
  uint16_t GetCS(void);

  /**
   * @brief 加载IDT（中断描述符表）到CPU IDTR寄存器
   * @param limit IDT表大小-1（表边界）
   * @param offset IDT物理基地址
   * 指令：lidt [参数]
   */
  void LoadIDT(uint16_t limit, uint64_t offset);

  /**
   * @brief 加载GDT（全局描述符表）到CPU GDTR寄存器
   * @param limit GDT表大小-1
   * @param offset GDT物理基地址
   * 指令：lgdt [参数]
   */
  void LoadGDT(uint16_t limit, uint64_t offset);

  /**
   * @brief 设置CS代码段寄存器、SS栈段寄存器
   * @param cs 代码段选择子
   * @param ss 栈段选择子
   * ⚠️ CS不能直接mov修改！必须通过远跳转lret/far jmp修改
   * MikanOS内部实现使用 lret 方式加载新CS
   * day08b 中在 SetupSegments() 之后调用，刷新段寄存器
   */
  void SetCSSS(uint16_t cs, uint16_t ss);

  /**
   * @brief 将 DS, ES, FS, GS 全部设置为同一个值
   * @param value 段选择子（day08b传入0）
   */
  void SetDSAll(uint16_t value);

  /**
   * @brief 设置CR3寄存器
   * @param value PML4页表物理基地址
   * CR3存放四级分页根页表地址；SetupIdentityPageTable()末尾调用
   * 仅填写页表地址，**不会开启分页**；开启分页需要修改CR0.PG位
   */
  void SetCR3(uint64_t value);
}