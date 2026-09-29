// Main.c
// MikanOS day02a UEFI Loader源码
#include <Uefi.h>
#include <Library/UefiLib.h>

/**
 * @brief UEFI应用入口函数
 *        在Loader.inf中通过 ENTRY_POINT = UefiMain 指定本入口
 *
 * @param image_handle 当前efi镜像的句柄，代表Loader.efi本身
 * @param system_table UEFI系统表指针，UEFI所有核心服务都在这里
 * @return EFI_STATUS UEFI状态码，本程序永远走不到return
 */
EFI_STATUS EFIAPI UefiMain(
    EFI_HANDLE image_handle,
    EFI_SYSTEM_TABLE *system_table)
{
    // Print：UefiLib提供的控制台输出函数
    // L"xxx"：UTF-16宽字符串，UEFI控制台只支持UTF16，不加L会乱码
    Print(L"Hello, Mikan World!\n");

    // 无限死循环！
    // 作用：卡住程序，不让它退出回到UEFI Shell
    // 如果没有while(1); 程序执行完会立刻退出，控制台一闪而过看不到输出
    while (1);

    // 下面return永远不会执行，仅满足函数语法要求
    return EFI_SUCCESS;
}


/*
### 1. `EFIAPI`

```
EFI_STATUS EFIAPI UefiMain(...)
```

`EFIAPI` 是 UEFI 定义的宏，本质是**调用约定**。
x64 下等价 `__attribute__((ms_abi))`，用来保证 UEFI 固件和我们代码之间寄存器、栈传参规则一致。

> 
> 如果漏掉`EFIAPI`，会出现栈不匹配，程序运行崩溃！

### 2. 参数名字：`image_handle` / `system_table`

源码用**下划线命名**，不是驼峰，这是 mikanos 教程原版风格：

- `image_handle`：对应标准文档里的`ImageHandle`
- `system_table`：对应标准文档里的`SystemTable`

### 3. 少了 `#include <Library/UefiApplicationEntryPoint.h>`

和前面版本不一样！day02a 这里**不需要**这个头文件。
`UefiApplicationEntryPoint` 是在 `Loader.inf` 的`[LibraryClasses]`声明，负责提供入口桩；
本 C 文件只用到`Print`，只需要`UefiLib.h`。

### 4. `while(1);` 死循环（day02a 最关键的设计）

普通 UEFI 应用执行完 return 之后，**会退出，把控制权还给 UEFI Shell**。
一旦退出，Shell 会清屏或者直接回到命令行，`Hello, Mikan World!` 这句话你来不及看。
写`while(1);`，CPU 卡在这无限循环，停在控制台，你可以一直看到打印的字符串。

> 
> 后面章节写真正内核加载代码时，这个 while (1) 就会被替换成启动内核的逻辑。

### 5. return EFI_SUCCESS;

代码逻辑上永远跑不到这一行。
C 语法要求非 void 函数必须有返回，编译器不会检查是否可达，写上只为消除编译警告。

## 和 Loader.inf 的联动校验

```
[Defines]
  ENTRY_POINT                    = UefiMain ; 和C函数名完全匹配
```

函数名必须严格一致（区分大小写），否则 build 报错找不到入口。

*/