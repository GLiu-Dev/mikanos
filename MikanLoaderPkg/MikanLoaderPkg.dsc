;----------------------------------------------------------------------
; MikanLoaderPkg.dsc
; MikanOS day02a 平台描述文件
; 顶层编译配置，build命令读取这个dsc来编译整个工程
;----------------------------------------------------------------------

#@range_begin(defines)
[Defines]                          ; 【固定段】平台基础元信息
  PLATFORM_NAME                  = MikanLoaderPkg
                                  ; 平台名称，自定义，用于区分不同平台工程
  PLATFORM_GUID                  = d3f11f4e-71e9-11e8-a7e1-33fd4f7d5a3e
                                  ; 平台唯一GUID，全局标识这个平台
  PLATFORM_VERSION               = 0.1
                                  ; 平台版本号，仅标记用途，build不校验
  DSC_SPECIFICATION              = 0x00010005
                                  ; DSC文件遵循EDKII DSC规范版本1.5
                                  ; 0x00010005 = Major 1, Minor 5
  OUTPUT_DIRECTORY               = Build/MikanLoader$(ARCH)
                                  ; 编译产物输出根目录
                                  ; $(ARCH) 是EDK内置变量，这里ARCH=X64，实际路径 Build/MikanLoaderX64
  SUPPORTED_ARCHITECTURES        = X64
                                  ; 本平台支持的CPU架构：仅x86_64（64位）
                                  ; MikanOS只做x64，所以只写X64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
                                  ; 允许的编译目标，用 | 分隔多个选项
                                  ; DEBUG：带调试信息，断言/调试打印开启
                                  ; RELEASE：发布版，去掉调试信息，优化
                                  ; NOOPT：不做代码优化，方便调试底层汇编
#@range_end(defines)

#@range_begin(library_classes)
[LibraryClasses]                   ; 【库绑定段】全局库类解析规则
                                  ; 语法：库类名 | 该库类对应的inf文件路径
                                  ; 含义：当模块声明依赖某个LibraryClass时，build自动链接后面这个库实现
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
                                  ; UefiApplicationEntryPoint库实现，给UEFI App提供入口桩
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
                                  ; UefiLib实现，提供Print等封装API
#@range_end(library_classes)
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
                                  ; BaseLib：基础工具库，字符串、位运算、简单工具函数
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
                                  ; 内存操作库：CopyMem、SetMem等内存读写函数
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
                                  ; 空DebugLib，不输出调试日志，减少代码体积
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
                                  ; 设备路径相关工具库（后续章节会用到）
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
                                  ; 内存分配封装库，封装BootServices的AllocatePool
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
                                  ; PCD(平台配置数据库)空实现，本工程暂时不用PCD
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
                                  ; Print底层实现库，UefiLib的Print依赖这个库
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
                                  ; UEFI启动服务表封装，拿到BootServices指针
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
                                  ; UEFI运行时服务表封装，拿到RuntimeServices指针

#@range_begin(components)
[Components]                       ; 【组件段】列出本平台需要编译的所有模块(.inf)
  MikanLoaderPkg/Loader.inf
                                  ; 要编译的模块：Loader.inf，最终生成Loader.efi
#@range_end(components)
