# Cortex-M 异常栈回溯（libgcc unwind）

本文说明 `fault_entry.S` / `fault_handler.cc` 如何让 libgcc 的 ARM EHABI
unwinder 从异常处理函数继续展开到硬件自动压栈的故障上下文。

结论：**可以实现**。Cortex-M 的异常帧本身包含了展开所需的 PC、SP 和
callee-saved 寄存器信息，缺的只是一条能被 unwinder 识别的“调用链”。
我们在异常入口的 `bl` 调用点上手写一条 `.ARM.exidx` 记录，把硬件异常帧
描述成该汇编桩的调用者帧，剩下的工作交给 libgcc。

整个实现只使用公开的 `_Unwind_Backtrace` 和文档化的 EHABI unwind 表格式，
不依赖 libgcc 私有结构，也没有做栈扫描。

---

## 1. 为什么不能直接回溯

正常函数通过 `bl` 调用，LR 保存返回地址；unwinder 沿 `.ARM.exidx` 中
每个函数的展开记录（prologue 保存了哪些寄存器、SP 调整多少）逐帧回退。

异常处理函数不是被 `bl` 调用的：

- 硬件把 `R0-R3, R12, LR, PC, xPSR` 压到 MSP 或 PSP 上；
- 此时 LR 被硬件改写成 `EXC_RETURN`（例如 `0xFFFFFFF9`）；
- handler 的 C 帧展开完后，unwinder 拿到的“返回地址”是 `EXC_RETURN`，
  不是合法代码地址，因此无法继续。

`R4-R11` 硬件不压栈（它们是 callee-saved），需要 handler 自己保存。

---

## 2. Cortex-M 异常帧

### 2.1 硬件帧布局

异常入口处，`mrs msp/psp` 得到的**已经是帧基址 F**（最低地址，R0 所在处），
不是异常发生前的 SP。

基本帧（`EXC_RETURN` bit4 = 1）：

| 偏移 | 内容 |
|---|---|
| F + 0x00 | R0 |
| F + 0x04 | R1 |
| F + 0x08 | R2 |
| F + 0x0C | R3 |
| F + 0x10 | R12 |
| F + 0x14 | LR（故障时的活 LR） |
| F + 0x18 | PC（故障指令地址） |
| F + 0x1C | xPSR |

FP 扩展帧（`EXC_RETURN` bit4 = 0）：

| 偏移 | 内容 |
|---|---|
| F + 0x00 .. F + 0x1C | 同上（核心部分布局完全不变） |
| F + 0x20 .. F + 0x5C | S0-S15 |
| F + 0x60 | FPSCR |

所以核心寄存器的解析对两种帧完全一致，只有计算“异常前 SP”时帧大小不同：

```
frame_size = (EXC_RETURN & 0x10) ? 0x20 : 0x68
```

### 2.2 EXC_RETURN 位

| 位 | 含义 |
|---|---|
| bit 2 | 0 = 异常帧在 MSP，1 = 在 PSP |
| bit 4 | 1 = 基本帧，0 = FP 扩展帧 |

`fault_entry.S` 用 `tst lr, #4` 选择 MSP/PSP，用 `tst lr, #0x10`
判断帧大小。

### 2.3 对齐 pad 与异常前 SP

当异常发生前的 SP 不是 8 字节对齐时，硬件会额外压入 4 字节 pad，并把
压栈 xPSR 的 bit 9 置 1。因此：

```
pre_exception_sp = F
                 + frame_size
                 + ((stacked_xpsr & 0x200) ? 4 : 0)
```

这个 SP 是继续展开故障函数时的起始 SP。算错 4 字节会导致故障函数的
callee-saved 寄存器弹错位置，回溯链立刻走偏。

### 2.4 MSP / PSP

handler 本身永远运行在 MSP 上。异常帧可能在：

- PSP：线程模式故障（FreeRTOS 任务最常見）；
- MSP：handler 模式故障，或线程使用 MSP。

异常帧所在的栈由 `EXC_RETURN` bit 2 选择。handler 自己保存 `R4-R11`
的缓冲区放在当前 handler 栈（MSP）上，位于 C handler 帧的上方，不会被
C 代码的栈增长覆盖。

---

## 3. libgcc ARM EHABI unwinder 模型

GCC 为每个函数生成 `.ARM.exidx`（必要时加 `.ARM.extab`），里面是
“如何展开这个函数”的字节码。libgcc 的 `_Unwind_Backtrace` 就是逐条执行
这些字节码。

### 3.1 相关字节码

| 字节码 | 含义 |
|---|---|
| `0x9n` | `vsp = r[n]` |
| `0x0n` | `vsp += (n << 2) + 4` |
| `0x8x yy` | 从 vsp 连续 pop `r4-r15`，掩码编码见下 |
| `0xb1 mm` | 从 vsp 连续 pop `r0-r3`，`mm` 是 4 位掩码 |
| `0xb0` | finish：若没有显式 pop PC，则 `PC = LR` |

`0x8x yy` 的编码：

```
m12 = ((x & 0x0F) << 8) | yy       // m12 的 bit i 对应寄存器 i+4
register_mask = m12 << 4
```

例如：

- pop `{r12, lr, pc}`：mask = `0xD000`，m12 = `0xD00` → `0x8d 0x00`
- pop `{r4-r8, r10, r11}`：mask = `0x0DF0`，m12 = `0x0DF` → `0x80 0xdf`

pop 顺序按寄存器号升序，内存布局必须对应。

### 3.2 finish 与 LR

EHABI 用 LR 表示“返回地址”。`finish` 默认把 LR 写入 PC。如果记录里
显式 pop 了 r15，则不会再执行 `PC = LR`。

这一点对故障回溯很重要：

- 硬件帧的 PC 槽（F+0x18）是故障指令，必须显式 pop 到 r15；
- 硬件帧的 LR 槽（F+0x14）是故障时的活 LR，必须恢复到上下文里，
  否则叶子函数（exidx 只有 `finish`）无法继续。

### 3.3 `.cantunwind`

GCC 默认会把“不会传播异常”的函数（典型是叶子函数）标成 `.cantunwind`。
libgcc 的 `__gnu_Unwind_Backtrace` 在 `get_eit_entry` 返回非 OK 时，
**在调用 trace 回调之前**就退出。因此：

- 故障函数是 `.cantunwind` 时，连故障函数自己都不会出现在回调里；
- 所以 `fault_handler.cc` 会先把故障 PC 手动放进结果数组，保证至少能定位
  到故障指令；
- 编译时加 `-funwind-tables` 后，叶子函数会得到一条 `finish` 记录，
  展开可以靠 LR 继续。项目已在顶层 CMakeLists.txt 全局启用该选项。

### 3.4 起始点

`_Unwind_Backtrace` 从“调用它的函数”开始展开（wrapper 把 LR 作为
起始 PC）。因此 handler 中的调用链是：

```
_Unwind_Backtrace
  └─ store_fault_stacktrace（可能被内联）
      └─ fault_handler_c
          └─ [C 帧保存的 LR] fault_entry.S 中的 bl 调用点
              └─ 手写 exidx：展开硬件异常帧
                  └─ 故障函数（真实 exidx）
                      └─ 调用者 ...
```

C 帧到汇编桩这一段的 LR 由编译器正常保存；我们只需要保证汇编桩的
`bl` 返回地址落在那条手写 exidx 记录的地址范围内。

---

## 4. 伪造帧设计

### 4.1 寄存器约定

汇编入口在 `bl fault_handler_c` 之前建立如下约定：

| 寄存器 | 值 | 用途 |
|---|---|---|
| r11 | F，硬件核心帧基址 | 第一条 raw opcode 的 pop 基址 |
| r10 | handler 栈上 R4-R8/R10/R11 保存块地址 | 第二条 pop 的基址 |
| r9 | 异常前 SP | 最后的 `vsp = r9` |

这三个都是 callee-saved 寄存器。C handler 以及中间函数如果使用它们，
必须按 AAPCS 保存/恢复；即使不使用，它们也会保持不变。因此等 unwinder
走到汇编桩时，r9/r10/r11 仍然是异常入口设置的值。

### 4.2 保存块布局

`fault_entry.S` 在 handler 栈上分配 32 字节：

| 偏移 | 内容 |
|---|---|
| +0x00 | R4 |
| +0x04 | R5 |
| +0x08 | R6 |
| +0x0C | R7 |
| +0x10 | R8 |
| +0x14 | R10 |
| +0x18 | R11 |
| +0x1C | R9 |

它对应 `callee_saved_t`。注意块的顺序与 raw opcode 的 pop 掩码一致：
`0x80 0xdf` 弹出 7 个寄存器（R4-R8、R10、R11），**不弹 R9**，把 R9
留给最后的 SP 恢复。

### 4.3 汇编入口流程

```asm
tst     lr, #4
ite     eq
mrseq   r12, msp
mrsne   r12, psp          @ r12 = F（帧基址）

sub     sp, sp, #32
stmia   sp, {r4-r8, r10, r11}
str     r9, [sp, #28]     @ 保存故障时的 R4-R11
mov     r10, sp           @ r10 = 保存块

ldr     r3, [r12, #0x1C]  @ 压栈 xPSR
ubfx    r9, r3, #9, #1
lsl     r9, r9, #2        @ pad = 0 或 4
tst     lr, #0x10
ite     eq
addeq   r9, r9, #0x68
addne   r9, r9, #0x20
add     r9, r9, r12       @ r9 = 异常前 SP

mov     r11, r12          @ r11 = F
mov     r0, r12           @ exception_frame_t *
mov     r1, lr            @ EXC_RETURN
mov     r3, r10           @ callee_saved_t *

.fnstart
.Lfault_bl:
    bl fault_handler_c
    .unwind_raw 0, 0x9b, 0xb1, 0x0f, 0x8d, 0x00, 0x9a, 0x80, 0xdf, 0x99, 0xb0
.fnend
```

保存块在 C handler 帧的上方（C 的栈向下增长），所以不会被覆盖。

### 4.4 raw opcode 逐条解释

```
0x9b          vsp = r11                 ; SP = F
0xb1 0x0f     pop {r0,r1,r2,r3}         ; F+0x00 .. F+0x0C
0x8d 0x00     pop {r12,lr,pc}           ; F+0x10, F+0x14, F+0x18
0x9a          vsp = r10                 ; SP = 保存块
0x80 0xdf     pop {r4,r5,r6,r7,r8,r10,r11}
0x99          vsp = r9                  ; SP = 异常前 SP
0xb0          finish
```

执行完这些 opcode 后，unwinder 的虚拟上下文是：

- PC = 硬件压栈的故障 PC；
- SP = 异常前 SP；
- R0-R3 = 硬件压栈值；
- R12、LR = 硬件压栈值；
- R4-R8、R10、R11 = 异常发生时的值；
- R9 = 异常前 SP（见 4.5）。

由于显式 pop 了 PC，`set_pc = 1`，`finish` 不会再用 LR 覆盖 PC。
之后 unwinder 用故障 PC 查它自己的真实 `.ARM.exidx`，继续往上走。

### 4.5 为什么牺牲 R9

要恢复“异常前 SP”，必须在弹完 R4-R11 之后还有一个未被覆盖的寄存器。
但 R4-R11 全都是 callee-saved，且都可能属于故障上下文。我们选择：

- R11 用作硬件帧基址，先使用，随后被 pop 覆盖；
- R10 用作保存块基址，先使用，随后被 pop 覆盖；
- R9 不参与 pop，最后用作 SP。

代价是 unwinder 看到的是 R9 = 异常前 SP，而不是故障时的 R9。影响：

- 寄存器打印仍然从保存块读取真实 R9，用户看到的值是正确的；
- GCC 的帧指针只使用 R7 或 R11，不会用 R9 作为 `vsp = rN` 的基址，
  所以不会影响栈展开；
- 只有在外部代码手工用 R9 作为帧指针时才会出问题，这在 GCC 生成的
  Cortex-M 代码中不会出现。

这是整个方案唯一的“模拟”折衷。

### 4.6 为什么用公开 API

libgcc 内部有 `__gnu_Unwind_Backtrace(trace, arg, phase2_vrs *)`，可以直接
构造虚拟寄存器上下文，代码更短。但 `phase2_vrs` 是 libgcc 私有结构，
布局随版本可能变化。

本实现改用公开的 `_Unwind_Backtrace`，只依赖：

- EHABI `.ARM.exidx` / `.ARM.extab` 的字节码格式；
- ARM 异常帧布局；
- AAPCS 的 callee-saved 约定。

可移植性更好，也更容易在不同 GCC 版本上审查。

---

## 5. 回溯回调

`fault_handler.cc` 的 `store_fault_stacktrace`：

1. 先把 `fault_pc & ~1` 放入数组第 0 项，保证即使故障函数是 `.cantunwind`，
   也能看到故障位置；
2. 调用 `_Unwind_Backtrace`；
3. 回调跳过 handler 自己的帧，直到 `_Unwind_GetIP` 等于故障 PC；
4. 之后的帧是调用者的返回地址，记录 `pc - 1`（return address 指向调用
   指令的下一条，减 1 才能落到调用点）；
5. 遇到 PC 为 0、数组满、或连续重复 PC 时停止。连续重复保护是为了防止
   走到线程入口 trampoline 后 EHABI 在同一个伪帧上无限循环。

---

## 6. 帧类型与测试

`test/fault_test.cc` 通过 `USE_FP` 生成两种帧：

- `USE_FP=0`：清 `CONTROL.FPCA`，硬件使用基本帧（0x20）；
- `USE_FP=1`：先执行一条浮点指令，硬件使用 FP 扩展帧（0x68）。

调用链：

```
main -> fault_outer -> fault_middle -> fault_leaf
```

`fault_leaf` 执行 `udf #0`。`fault_middle` / `fault_outer` 使用 volatile
局部变量强制生成真实栈帧，构建时加 `-fno-optimize-sibling-calls` 防止
尾调用把链路压平。

已验证：

- 基本帧 + 非叶子故障函数；
- FP 扩展帧 + 非叶子故障函数；
- 基本帧 / FP 扩展帧 + 叶子故障函数（exidx 只有 `finish`，靠 LR 继续）。

`fault_leaf` 是叶子时，如果编译时没有 `-funwind-tables`，GCC 会生成
`.cantunwind`，libgcc 会在调用 trace 回调前停止，结果只剩故障 PC。
因此项目在顶层 CMakeLists.txt 全局加了 `-funwind-tables`。

运行方式：

```bash
cmake --build test/build -j -t fault_basic fault_fp
cmake --build test/build -t sim-fault-basic
# handler 打印完会死循环，Ctrl+C 退出
```

期望的符号化结果：

```
fault_leaf -> fault_middle -> fault_outer -> main -> main_routine
```

最后可能多出一个线程入口 trampoline 的伪帧，属于正常现象。

---

## 7. 限制与注意事项

1. **`-funwind-tables` 是前提**：应用代码没有它就可能生成 `.cantunwind`，
   回溯会在那里停止。预编译的 libc/libgcc 中仍可能有 `.cantunwind` 函数；
   此时至少还能打印故障 PC。
2. **R9 的折衷**：见 4.5。
3. **线程入口伪帧**：线程启动的 trampoline 没有真实调用者，EHABI 可能
   走入一个伪帧；连续重复 PC 保护会终止。
4. **MSP 栈开销**：libgcc unwinder 的 `phase1_vrs` 约 500 字节，加上
   C handler 和打印代码，故障处理在 MSP 上大约需要 800 字节。测试中
   1 KB MSP 可以工作；如果会在很深的 ISR 中故障，建议给故障处理单独的
   应急栈。
5. **嵌套故障**：handler 内部再次 fault 会重入；当前没有做防重入保护。
6. **只恢复核心寄存器**：回溯不需要恢复 VFP 寄存器；故障函数自己的
   `.ARM.exidx` 会从它的栈帧恢复 callee-saved VFP 寄存器。
7. **对齐 pad / MSP 异常帧**：代码按架构实现，但测试只覆盖了 PSP 线程
   上下文且 xPSR bit 9 为 0 的情况。MSP 帧和对齐 pad 分支建议在真实
   硬件上再验证一次。

---

## 8. 构建与链接

- `xeno_backtrace` 需要以 whole-archive 方式链接，才能用强符号覆盖
  startup 中的 weak `HardFault_Handler` / `MemManage_Handler` 等：

  ```cmake
  $<LINK_LIBRARY:WHOLE_ARCHIVE,xeno_backtrace>
  ```

- 主 `arm_simulator/main` 目前没有链接 `xeno_backtrace`，所以它的 fault
  行为仍是 startup 里的默认死循环。需要时在 main 的链接项里加上即可。
- 顶层和 test 工程都已全局启用 `-funwind-tables`。

---

## 9. 文件说明

| 文件 | 作用 |
|---|---|
| `fault_entry.S` | 四个异常入口、保存 R4-R11、计算异常前 SP、手写 exidx 伪帧 |
| `fault_handler.cc` | 打印寄存器、构造回溯、过滤 handler 帧、输出地址 |
| `print_stacktrace.cc` | 普通执行点的回溯（`_Unwind_Backtrace`） |
| `CMakeLists.txt` | 汇编源文件与本库的 `-funwind-tables` |
| `test/fault_test.cc` | 基本帧 / FP 扩展帧 / 叶子函数故障测试 |

---

## 10. 参考

- ARM Cortex-M7 Devices Generic User Guide — CCR / STKALIGN：
  <https://developer.arm.com/documentation/dui0646/b/Cortex-M7-Peripherals/System-control-block/Configuration-and-Control-Register?lang=en>
- OpenOCD 关于 STKALIGN 与异常帧 SP 恢复的讨论：
  <https://sourceforge.net/p/openocd/mailman/openocd-commit/thread/From_openocd-gerrit%40users.sourceforge.net_Fri_Oct_30_23%3A44%3A56_2015/#msg34582707#1>
- LLVM 中 Cortex-M 异常帧展开的实现思路：
  <https://github.com/llvm/llvm-project/pull/153922>
- 本工具链的 libgcc 源码：
  - `libgcc/unwind-arm-common.inc`
  - `libgcc/config/arm/unwind-arm.c`
  - `libgcc/config/arm/pr-support.c`
  - `libgcc/config/arm/libunwind.S`
