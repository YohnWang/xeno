#pragma once

/*
 * 通用 Cortex-M boot -> app 跳转类。
 *
 * 使用方式与 FreeRTOS 相同：包含本头文件之前必须先包含平台设备头，例如
 *
 *     #include "gd32h73x_75x.h"
 *     #include <FreeRTOS.h>
 *     #include <xeno/cortexm_loader.h>
 *
 * 本头文件不包含任何设备头，靠 CMSIS 宏/寄存器成员的存在性适配不同内核。
 *
 * 用法：
 *
 *     xeno::cortexm::loader ld;                  // 默认异常模式
 *     ld.add_executable_region(0x08040000U, 3584U * 1024U);
 *     ld.add_stack_region(0x24000000U, 832U * 1024U);
 *     ld.jump_to(0x08040000U);                   // 成功不返回
 *
 * jump_to(address) 把 address 当作镜像向量表基址：从 [address] 取初始 MSP，
 * 从 [address+4] 取复位向量，校验后关中断/cache/MPU、清异常与 NVIC/SysTick
 * 残留、改写 VTOR，最后切栈并跳到复位向量。
 *
 * 错误方式由模板参数选择：
 *   - basic_loader<error_mode::exceptions>（别名 loader，默认）：失败抛 loader_error
 *   - basic_loader<error_mode::error_code>（别名 error_code_loader）：失败返回
 *     std::expected<void, loader_errc>
 *
 * 前提：
 *   - 在特权线程模式调用（异常处理/非特权线程会得到 loader_errc::context）；
 *   - 只做当前安全域内的跳转。TrustZone 的 secure -> non-secure 需要 SAU 配置
 *     和 BLXNS，不在本实现范围内；secure 态构建（-mcmse）一律返回 not_supported；
 *   - Cortex-M0/M1（无 VTOR）返回 not_supported；
 *   - 只校验架构级合法性，不做镜像内容（CRC/签名）校验。
 */

#if !defined(__cplusplus)
#error "<xeno/cortexm_loader.h> requires C++"
#endif

/* 与 FreeRTOS 相同的契约：设备头必须已经包含。
 * __NVIC_PRIO_BITS/SCB/NVIC/SysTick 同时用于排除裸 core 头。 */
#if !defined(__CMSIS_GCC_H) || !defined(__CORTEX_M) || !defined(__NVIC_PRIO_BITS) \
    || !defined(SCB) || !defined(NVIC) || !defined(SysTick)
#error "include the CMSIS device header (e.g. gd32h73x_75x.h) before <xeno/cortexm_loader.h>"
#endif

#if !defined(__ARM_ARCH_PROFILE) || (__ARM_ARCH_PROFILE != 'M') || !defined(__thumb__)
#error "<xeno/cortexm_loader.h> requires a Thumb Cortex-M target"
#endif

#include<cstddef>
#include<cstdint>
#include<expected>
#include<stdexcept>
#include<string>
#include<string_view>
#include<type_traits>
#include<utility>
#include<vector>

/* ------------------------------------------------------------------------- */
/* 内核能力检测                                                               */
/* ------------------------------------------------------------------------- */

/* 厂商 CMSIS 5.1 的 core_cm7.h 不定义 __VTOR_PRESENT，但 VTOR 宏和成员都存在，
 * 所以先用 SCB_VTOR_TBLOFF_Msk 的存在性兜底；CMSIS 6.x 的 M0+/M23 只有设备头
 * 声明 __VTOR_PRESENT==1 时才会提供这个宏。 */
#if defined(SCB_VTOR_TBLOFF_Msk) || (defined(__VTOR_PRESENT) && (__VTOR_PRESENT == 1U))
#  define XENO_CORTEXM_HAS_VTOR 1
#else
#  define XENO_CORTEXM_HAS_VTOR 0
#endif

/* BASEPRI/FAULTMASK 只在 Thumb-2 内核（M3/M4/M7/M33/M35P/M52/M55/M85）上存在；
 * ARMv6-M 和 Cortex-M23 没有。 */
#if defined(__ARM_ARCH_ISA_THUMB) && (__ARM_ARCH_ISA_THUMB >= 2)
#  define XENO_CORTEXM_MAINLINE 1
#else
#  define XENO_CORTEXM_MAINLINE 0
#endif

/* MSPLIM/PSPLIM 只在 ARMv8-M mainline 上可用。 */
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 8) && XENO_CORTEXM_MAINLINE
#  define XENO_CORTEXM_STACK_LIMITS 1
#else
#  define XENO_CORTEXM_STACK_LIMITS 0
#endif

/* 初始 MSP 对齐：ARMv6-M 只要求 4 字节，其余要求 8 字节。 */
#if defined(__ARM_ARCH_6M__)
#  define XENO_CORTEXM_STACK_ALIGN 4U
#else
#  define XENO_CORTEXM_STACK_ALIGN 8U
#endif

/* 用 -mcmse 编译的 secure 态代码不在这里支持。 */
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3)
#  define XENO_CORTEXM_SECURE 1
#else
#  define XENO_CORTEXM_SECURE 0
#endif

namespace xeno::cortexm
{

inline constexpr bool is_supported = XENO_CORTEXM_HAS_VTOR && !XENO_CORTEXM_SECURE;

/* 向量表硬件对齐要求：M0+ 的 VTOR TBLOFF 从 bit8 起（256 字节），其余从 bit7
 * 起（128 字节）。 */
#if defined(SCB_VTOR_TBLOFF_Pos)
inline constexpr std::uintptr_t vector_table_alignment =
    std::uintptr_t{1} << SCB_VTOR_TBLOFF_Pos;
#else
inline constexpr std::uintptr_t vector_table_alignment = 128U;
#endif

enum class loader_errc
{
    vector_table,   // address/向量表不合法
    stack_pointer,  // 初始 MSP 不合法
    reset_vector,   // 复位向量不合法
    not_supported,  // 内核无 VTOR，或 secure 态构建
    context,        // 不在特权线程模式
};

[[nodiscard]] constexpr std::string_view to_string(loader_errc e) noexcept
{
    switch(e)
    {
    case loader_errc::vector_table:  return "invalid vector table";
    case loader_errc::stack_pointer: return "invalid stack pointer";
    case loader_errc::reset_vector:  return "invalid reset vector";
    case loader_errc::not_supported: return "jump not supported";
    case loader_errc::context:       return "not privileged thread mode";
    }
    return {};
}

class loader_error : public std::runtime_error
{
public:
    explicit loader_error(loader_errc code)
        : std::runtime_error(std::string(to_string(code))), code_(code)
    {}

    loader_errc code() const noexcept { return code_; }

private:
    loader_errc code_;
};

enum class error_mode
{
    exceptions,  // 默认
    error_code,
};

/* [begin, end)；作为初始栈顶时允许 sp == end。 */
struct region
{
    std::uintptr_t begin;
    std::uintptr_t end;
};

struct image_entry
{
    std::uintptr_t vector_table;
    std::uintptr_t stack_pointer;
    std::uintptr_t reset_vector;  // Thumb 地址（bit0 = 1）
};

namespace cortexm_detail
{

inline bool region_contains(const std::vector<region> &regions,
                            std::uintptr_t addr) noexcept
{
    for(const auto &r : regions)
        if(addr >= r.begin && addr < r.end)
            return true;
    return false;
}

/* 向量表两个 word 必须都在区域内。 */
inline bool region_contains_image(const std::vector<region> &regions,
                                  std::uintptr_t addr) noexcept
{
    for(const auto &r : regions)
        if(addr >= r.begin && addr < r.end && (r.end - addr) >= 8U)
            return true;
    return false;
}

inline bool region_contains_stack(const std::vector<region> &regions,
                                  std::uintptr_t sp) noexcept
{
    for(const auto &r : regions)
        if(sp > r.begin && sp <= r.end)
            return true;
    return false;
}

/*
 * 关中断、关 cache、关 MPU，清掉 boot 留下的异常/中断外设状态，最后把向量表
 * 指到新镜像。此函数还在 boot 自己的栈上跑，可以随便用 C。
 */
inline void quiesce(std::uintptr_t vector_table) noexcept
{
    __disable_irq();

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    /* 内部按 set/way 做 clean+invalidate 再清 DC 位，脏行不清掉就关 cache 会丢数据。 */
    SCB_DisableDCache();
#endif
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    SCB_DisableICache();
#endif

#if defined(__MPU_PRESENT) && (__MPU_PRESENT == 1U) && defined(MPU)
    MPU->CTRL = 0U;
#endif

    /* 关掉可配置 fault，并把已经锁存的 fault 状态清掉。 */
    SCB->SHCSR = 0U;
#if defined(SCB_CFSR_MEMFAULTSR_Msk)
    SCB->CFSR = 0xFFFFFFFFU;
    SCB->HFSR = 0xFFFFFFFFU;
    SCB->DFSR = 0xFFFFFFFFU;
    SCB->AFSR = 0xFFFFFFFFU;
#endif

    /* 全关、全清：boot 打开的 IRQ 编号在 app 里可能是另一个外设。
     * ICER/ICPR 的数组长度随内核不同，用 sizeof 推。 */
    for(std::size_t i = 0U; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); ++i)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* 停 SysTick，清 PendSV/SysTick 挂起位。 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;

    /* 系统异常优先级回到复位默认值。用 NVIC_SetPriority 而不是直接写
     * SCB->SHPR/SHP：CMSIS 5.3 叫 SHP、5.1/6.x 叫 SHPR。 */
    NVIC_SetPriority(SVCall_IRQn, 0U);
    NVIC_SetPriority(PendSV_IRQn, 0U);
    NVIC_SetPriority(SysTick_IRQn, 0U);
#if XENO_CORTEXM_MAINLINE
    NVIC_SetPriority(MemoryManagement_IRQn, 0U);
    NVIC_SetPriority(BusFault_IRQn, 0U);
    NVIC_SetPriority(UsageFault_IRQn, 0U);
#endif

    /* 关 FPU 惰性压栈；FPCA 之后随 CONTROL 一起清。 */
#if defined(__FPU_PRESENT) && (__FPU_PRESENT == 1U) && defined(FPU_FPCCR_ASPEN_Msk)
    FPU->FPCCR &= ~(FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk);
#endif

    __DSB();
    __ISB();

#if XENO_CORTEXM_HAS_VTOR
    SCB->VTOR = static_cast<std::uint32_t>(vector_table);
    __DSB();
    __ISB();
#else
    (void)vector_table;
#endif
}

/*
 * 切栈并跳转。r0 = 新 MSP，r1 = 复位向量（Thumb）。
 *
 * 从 msr control 开始就不再碰栈：新镜像的栈还没初始化，此时任何 push/pop
 * 都会踩到不属于自己的内存。参数匿名，靠 AAPCS 约定落在 r0/r1。
 */
[[noreturn]] inline __attribute__((naked, noinline))
void switch_to(std::uintptr_t, std::uintptr_t) noexcept
{
    __asm__ volatile(
        ".syntax unified\n\t"
        ".thumb\n\t"
        "movs    r2, #0\n\t"
        /* 先切回 MSP 再清 PSP：调用者可能是 FreeRTOS 任务，正用 PSP 当栈；
         * 若先清 PSP，紧跟着的任何异常压栈都会写到 0 地址。 */
        "msr     control, r2\n\t"   /* 特权线程模式 + MSP，并清 FPCA/SFPA */
        "isb\n\t"
#if XENO_CORTEXM_STACK_LIMITS
        "msr     msplim, r2\n\t"
        "msr     psplim, r2\n\t"
#endif
        "msr     psp, r2\n\t"
        "msr     msp, r0\n\t"       /* MSP 指向新镜像栈顶 */
        "dsb\n\t"
        "isb\n\t"
#if XENO_CORTEXM_MAINLINE
        /* 恢复复位后的异常屏蔽状态；NVIC 已全关，此时开中断安全。 */
        "msr     basepri, r2\n\t"
        "msr     faultmask, r2\n\t"
#endif
        "cpsie   i\n\t"
        "isb\n\t"
        /* 跳到 app 的 Reset_Handler：从这一刻起属于新镜像。 */
        "bx      r1\n\t"
    );
}

} // namespace cortexm_detail

/*
 * 通用跳转器。
 *
 * 可执行区域（向量表/复位向量必须落在其中）和栈区域（初始 MSP 必须落在其中）
 * 都可以多次添加；某一类区域一个都没添加时，跳过该类区间校验，只做架构级检查。
 * 区域有两种入参写法：
 *   add_executable_region({begin, end});              // 右端不含
 *   add_executable_region(begin, length);             // length 为字节数
 */
template<error_mode Mode = error_mode::exceptions>
class basic_loader
{
public:
    using jump_result = std::conditional_t<
        Mode == error_mode::exceptions,
        void,
        std::expected<void, loader_errc>>;

    basic_loader() = default;

    basic_loader &add_executable_region(region r)
    {
        executable_regions_.push_back(r);
        return *this;
    }

    basic_loader &add_executable_region(std::uintptr_t begin, std::uintptr_t length)
    {
        return add_executable_region(region{begin, begin + length});
    }

    basic_loader &add_stack_region(region r)
    {
        stack_regions_.push_back(r);
        return *this;
    }

    basic_loader &add_stack_region(std::uintptr_t begin, std::uintptr_t length)
    {
        return add_stack_region(region{begin, begin + length});
    }

    /* 只校验并返回入口信息，不跳转。 */
    [[nodiscard]] std::expected<image_entry, loader_errc>
    inspect(std::uintptr_t address) const noexcept
    {
        if constexpr(!is_supported)
        {
            return std::unexpected(loader_errc::not_supported);
        }
        else
        {
            if((address % vector_table_alignment) != 0U)
                return std::unexpected(loader_errc::vector_table);

            if(!executable_regions_.empty()
               && !cortexm_detail::region_contains_image(executable_regions_, address))
                return std::unexpected(loader_errc::vector_table);

            /* app 可能是刚烧进 flash 的，缓存里还留着烧写前的旧行。 */
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
            SCB_InvalidateDCache_by_Addr(reinterpret_cast<void *>(address), 8);
#else
            __DSB();
#endif

            const auto *const words =
                reinterpret_cast<volatile const std::uint32_t *>(address);
            const std::uintptr_t sp = words[0];
            const std::uintptr_t reset = words[1];

            if(sp == 0U || (sp % XENO_CORTEXM_STACK_ALIGN) != 0U)
                return std::unexpected(loader_errc::stack_pointer);

            if(!stack_regions_.empty()
               && !cortexm_detail::region_contains_stack(stack_regions_, sp))
                return std::unexpected(loader_errc::stack_pointer);

            const std::uintptr_t target = reset & ~std::uintptr_t{1U};

            if((reset & 1U) == 0U || target < address)
                return std::unexpected(loader_errc::reset_vector);

            if(!executable_regions_.empty()
               && !cortexm_detail::region_contains(executable_regions_, target))
                return std::unexpected(loader_errc::reset_vector);

            return image_entry{address, sp, reset};
        }
    }

    /*
     * 跳转到 address 处的镜像（向量表基址）。成功不返回；
     * 失败按 Mode 抛 loader_error 或返回 std::unexpected。
     */
    jump_result jump_to(std::uintptr_t address)
    {
        const auto entry = inspect(address);
        if(!entry)
            return fail(entry.error());

        /* quiesce 之后就不再是安全的 C 运行环境，先把调用上下文检查掉。 */
        if(__get_IPSR() != 0U || (__get_CONTROL() & 1U) != 0U)
            return fail(loader_errc::context);

        cortexm_detail::quiesce(entry->vector_table);
        cortexm_detail::switch_to(entry->stack_pointer, entry->reset_vector);
        std::unreachable();
    }

private:
    static jump_result fail(loader_errc code)
    {
        if constexpr(Mode == error_mode::exceptions)
            throw loader_error(code);
        else
            return std::unexpected(code);
    }

    std::vector<region> executable_regions_;
    std::vector<region> stack_regions_;
};

using loader = basic_loader<>;                                   // 默认：异常
using error_code_loader = basic_loader<error_mode::error_code>;  // 错误码

} // namespace xeno::cortexm
