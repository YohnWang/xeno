# xeno::cortexm::basic_loader 使用说明

单文件 Cortex-M boot → app 跳转类，header-only，位于
`xeno/toolkit/include/xeno/cortexm_loader.h`。

和 FreeRTOS 一样，**包含本头文件之前必须先包含平台设备头**，本头文件自己不包含任何
设备头、也不含任何芯片内存地址：

```cpp
#include"gd32h73x_75x.h"        // 平台设备头（先）
#include<xeno/cortexm_loader.h> // 跳转类（后）
```

## 特性

* **Header only。** 只依赖标准库，不需要构建步骤。
* **全 Cortex-M 通用。** 通过 CMSIS 宏和寄存器成员的存在性适配内核，而不是写死某个
  芯片：M3/M4/M7/M23/M33/M35P/M52/M55/M85 等；M0/M0+/M1 没有 VTOR，返回
  `not_supported`。
* **内存布局由调用方提供。** 可执行区域和栈区域都可多次添加；某一类区域一个都没
  添加时，跳过该类区间校验。
* **错误方式可配置。** 编译期选择异常或错误码，默认异常；错误码模式可以用
  `-fno-exceptions` 编译。
* **跳转前清干净运行环境。** 关中断、关 D/I-Cache、关 MPU、清 fault/NVIC/SysTick/
  FPU 残留、写 VTOR，然后切 MSP/PSP 并跳到新镜像的复位向量。
* **成功不返回。** 返回只可能是失败（异常或错误码）。

## 环境要求

* GCC（用到 `__attribute__((naked))` 和 GCC 内联汇编）。
* Thumb Cortex-M 目标。
* 设备头必须先包含；`__NVIC_PRIO_BITS`、`SCB`、`NVIC`、`SysTick` 等宏缺一不可，
  否则编译期直接 `#error`。
* 异常模式需要工具链开启异常；错误码模式不依赖异常。

## 快速开始（异常模式，默认）

```cpp
#include"gd32h73x_75x.h"
#include<xeno/cortexm_loader.h>

int goto_app()
{
    xeno::cortexm::loader ld;   // basic_loader<error_mode::exceptions>

    // 可执行区域：向量表和复位向量必须落在其中。
    // 起始地址 + 长度；也可以写成 add_executable_region({begin, end})。
    ld.add_executable_region(0x08040000U, 3584U * 1024U);

    // 栈区域：初始 MSP 必须落在其中。可添加多段。
    ld.add_stack_region(0x20000000U, 128U * 1024U);  // DTCM
    ld.add_stack_region(0x24000000U, 832U * 1024U);  // AXISRAM
    ld.add_stack_region(0x30000000U, 32U * 1024U);   // SRAM0 + SRAM1

    try
    {
        ld.jump_to(0x08040000U);   // 成功不返回
    }
    catch(const xeno::cortexm::loader_error &e)
    {
        fmt::println("goto_app: {} (code {})", e.what(), static_cast<int>(e.code()));
        return 1;
    }
    return 0;
}
```

## 错误码模式

```cpp
xeno::cortexm::error_code_loader ld;   // basic_loader<error_mode::error_code>

ld.add_executable_region({0x08040000U, 0x08400000U});
ld.add_stack_region({0x24000000U, 0x240D0000U});

auto result = ld.jump_to(0x08040000U);
if(!result)
{
    fmt::println("goto_app: {}",
                 xeno::cortexm::to_string(result.error()));
    return 1;
}
// 走到这里说明跳转成功（jump_to 成功不返回）
```

两种模式共用实现，区别只在 `jump_to()` 的失败路径：异常模式抛 `loader_error`，
错误码模式返回 `std::unexpected(loader_errc)`。

## API 速查

```cpp
namespace xeno::cortexm
{
enum class loader_errc
{
    vector_table,   // address/向量表不合法
    stack_pointer,  // 初始 MSP 不合法
    reset_vector,   // 复位向量不合法
    not_supported,  // 内核无 VTOR，或 secure 态构建
    context,        // 不在特权线程模式
};

constexpr std::string_view to_string(loader_errc) noexcept;

class loader_error : public std::runtime_error
{
public:
    loader_errc code() const noexcept;
};

enum class error_mode { exceptions, error_code };

struct region        { std::uintptr_t begin; std::uintptr_t end; };
struct image_entry   { std::uintptr_t vector_table;
                       std::uintptr_t stack_pointer;
                       std::uintptr_t reset_vector; };

template<error_mode Mode = error_mode::exceptions>
class basic_loader
{
public:
    using jump_result = std::conditional_t<Mode == error_mode::exceptions,
                                           void,
                                           std::expected<void, loader_errc>>;

    basic_loader &add_executable_region(region r);
    basic_loader &add_executable_region(std::uintptr_t begin, std::uintptr_t length);
    basic_loader &add_stack_region(region r);
    basic_loader &add_stack_region(std::uintptr_t begin, std::uintptr_t length);

    std::expected<image_entry, loader_errc>
    inspect(std::uintptr_t address) const noexcept;

    jump_result jump_to(std::uintptr_t address);
};

using loader = basic_loader<>;                                   // 默认：异常
using error_code_loader = basic_loader<error_mode::error_code>;  // 错误码
}
```

公共常量：

```cpp
inline constexpr bool is_supported;                          // 当前内核/构建是否可跳转
inline constexpr std::uintptr_t vector_table_alignment;      // 向量表硬件对齐字节数
```

### 区域入参

```cpp
ld.add_executable_region({0x08040000U, 0x08400000U}); // {begin, end}，右端不含
ld.add_executable_region(0x08040000U, 3584U * 1024U); // 起始地址 + 长度
```

`length` 为字节数，内部换算为 `end = begin + length`。区域校验按类别启用：没有添加
任何可执行区域时，不校验向量表/复位向量地址；没有添加任何栈区域时，不校验初始
MSP 所属内存。

### inspect 与 jump_to

* `inspect(address)`：只校验并返回 `image_entry`（向量表地址、初始 MSP、复位向量），
  不改变任何硬件状态，可以先用它做预检。
* `jump_to(address)`：`address` 是镜像**向量表基址**，从 `[address]` 取初始 MSP、
  `[address+4]` 取复位向量。成功不返回；失败按模式抛异常或返回错误码。

## 校验规则

`jump_to()` 先调用 `inspect()`，按顺序检查：

1. 当前内核/构建受支持（否则 `not_supported`）。
2. 向量表地址满足 VTOR 硬件对齐：M0+ 为 256 字节（TBLOFF 从 bit8 起），其余为
   128 字节（bit7 起）。
3. 若添加了可执行区域，`[address, address+8)` 必须完整落在某个区域内。
4. 有 D-Cache 时先按地址失效 8 字节，避免读到烧写前的旧缓存行；无 D-Cache 时用
   `__DSB()` 保证烧写完成。
5. 初始 MSP 非零，且按内核要求对齐：ARMv6-M 4 字节，ARMv7-M/ARMv8-M 8 字节。
6. 若添加了栈区域，初始 MSP 必须满足 `begin < sp <= end`（初始栈顶通常正好等于
   区域末尾）。
7. 复位向量 bit0 必须为 1（Thumb），跳转目标 `reset & ~1` 不能小于向量表地址；
   若添加了可执行区域，还必须落在某个区域内。

`inspect()` 不校验镜像内容（CRC、签名等）；这些由上层负责。

## 跳转流程

`jump_to()` 校验通过后：

1. 检查调用上下文是**特权线程模式**（`IPSR == 0` 且 `CONTROL.nPRIV == 0`），否则
   `context` 错误。
2. 关中断。
3. 关 D-Cache、I-Cache（有则关）；关 MPU（有则关）。
4. 清 `SHCSR` 和可配置 fault 状态；清全部 `NVIC->ICER/ICPR`；停 SysTick、清
   PendSV/SysTick 挂起；系统异常优先级归零；关 FPU 惰性压栈。
5. `DSB/ISB`，写 `SCB->VTOR`，再 `DSB/ISB`。
6. naked 汇编切栈并跳转：

```
msr control, #0        ; 特权线程模式 + MSP，并清 FPCA/SFPA
msr msplim/psplim, #0  ; 仅 ARMv8-M mainline
msr psp, #0            ; 已切到 MSP，再清 PSP
msr msp, <新 MSP>
msr basepri/faultmask, #0 ; 仅 Thumb-2 内核
cpsie i
bx <复位向量>
```

调用者可能正跑在 FreeRTOS 任务的 PSP 上，因此必须先切回 MSP 再清 PSP；从改写 MSP
那一刻起不再使用栈，`switch_to()` 是 naked 函数，整段只有寄存器和内联汇编。

## 前提与限制

* 必须在特权线程模式调用；异常处理里调用返回 `context`。
* 只支持**当前安全域内**的跳转。secure → non-secure 需要 SAU/IDAU 配置和 BLXNS，
  不在本类范围内；用 `-mcmse` 编译的 secure 态构建一律返回 `not_supported`。
  非 secure 构建（M23/M33/M55/M85 常规用法）可以正常使用。
* M0/M1 没有 VTOR；未声明 `__VTOR_PRESENT=1` 的 M0+/M23 也视为不支持，均返回
  `not_supported`。
* 不做镜像内容校验。
* 成功跳转不返回；跳转前会重新打开中断。此时 NVIC/SysTick 已全部关闭，只有 NMI
  等不可屏蔽异常可能在 `cpsie i` 与 `bx` 之间进入，这时 MSP/VTOR 已经指向新镜像。
* 区域列表用 `std::vector` 保存，依赖堆；在带堆的 boot 环境里使用。
* 非线程安全：同一个 loader 对象不要并发使用。

## 支持的内核与验证

编译矩阵（`-Wall -Wextra -Werror`）：

* CMSIS 6.1：cortex-m0、m0plus、m1、m3、m4、m7、m23、m33、m35p、m52、m55、m85。
* CMSIS 5.3（`SHP` 命名的旧头）：cortex-m0、m3、m4。
* 厂商 CMSIS 5.1 + GD32H73x 设备头：cortex-m7（覆盖设备头不定义
  `__VTOR_PRESENT` 的情况）。
* `-mcmse` secure 构建：`is_supported == false`，返回 `not_supported`。
* `-fno-exceptions`：错误码模式可正常编译。

`switch_to()` 的反汇编确认只有 `msr/cpsie/dsb/isb/bx`，没有任何栈访问指令。实际
跳转行为需在目标板上验证。
