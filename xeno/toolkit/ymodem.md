# xeno::ymodem_receiver 使用说明

单文件 YMODEM-1K 接收端，header-only，位于 `xeno/toolkit/include/xeno/ymodem.h`。
支持 SOH(128B) / STX(1KB) 帧、CRC16/XMODEM、Block 0 文件名与大小解析。

## IO 接入

通过继承 + deducing this 在编译期绑定 IO，不需要虚函数或函数指针：

```cpp
struct serial_io : xeno::ymodem_receiver
{
    // 必须返回 int：0..255 是数据，负数表示暂无数据。
    // 不能声明为 char/int8_t，否则 0x80..0xFF 会被符号扩展成负数。
    int try_getchar()
    {
        char c;
        if(!uart_try_read(c))
            return -1;
        return static_cast<unsigned char>(c); // 关键：先转 unsigned char
    }

    // 发送一个字节。
    void putchar(int c)
    {
        uart_write(static_cast<std::uint8_t>(c));
    }

    // 可选：类内默认用 std::chrono::steady_clock；返回 int 或 int64_t 毫秒。
    std::int64_t time() const
    {
        return board_millis();
    }
};
```

`try_getchar()` 必须是单字节、非阻塞的，返回类型由类内 `static_assert` 强制为 `int`。
类内不做等待；半帧超时由 `time()` 计时并自动发 NAK，握手阶段完全没有收到字节时
自动重发 `'C'`，连续 3 次无进展抛 `timeout`。若底层缓冲区是 `char`/`int8_t`，
返回前务必转 `unsigned char`，否则 `0x80..0xFF` 会变成负数并被当作“暂无数据”。

## 公共接口

```cpp
static constexpr std::size_t kUnknown;

std::string filename() const;   // Block 0 解析成功后有效
std::size_t filesize() const;   // kUnknown 表示发送方未提供大小

std::int64_t time() const;      // 默认 steady_clock；可覆盖为 int/int64_t
template<class Rep, class Period>
void sleep_for(const std::chrono::duration<Rep, Period> &); // 默认 this_thread
void set_timeout(std::uint32_t ms); // 默认 1000ms，0 表示关闭

bool        start_link(this auto &&self);
std::size_t receive(this auto &&self, auto &&f);
bool        finish_batch(this auto &&self);
void        cancel(this auto &&self);
void        reset();
```

### 异常

非正常结束不再用返回值表示，而是抛出异常：

| 异常 | 触发条件 |
|---|---|
| `ymodem_receiver::cancelled` | 远端在帧头首字节发来 `CAN(0x18)` 或 `CTRL_C(0x03)`；`finish_batch()` 等待空 Block 0 时收到 CAN；或在中止后再次调用 `start_link()/receive()/finish_batch()` |
| `ymodem_receiver::failed` | 握手阶段收到 EOT；收到空文件头（文件名为空）；已知文件大小但数据未收完就收到 EOT；`finish_batch()` 收到非空 Block 0（发送方还有下一个文件） |
| `ymodem_receiver::timeout` | 握手阶段连续 3 次重发 `'C'` 无响应，或半帧连续 3 次 NAK 后仍无进展；继承 `failed` |

`cancelled` 和 `failed` 继承 `ymodem_receiver::error`，`timeout` 继承 `failed`，
最终都继承 `std::exception`。

注意：

- `cancel()` 在 `idle/finished` 状态下是空操作；活动传输中取消会发 CAN、进入 `finished`，并抛 `cancelled`。
- `cancel()` 之后再调用 `start_link()/receive()/finish_batch()` 会抛 `cancelled`。
- 回调内部抛出的异常会原样传播，类不会把它转换成 `failed`。

### start_link()

| 返回值 | 含义 |
|---|---|
| `true` | 连接成功；`filename()/filesize()` 可用 |
| `false` | 已发出 `'C'`，或 Block 0 尚未收完，继续轮询 |

- `phase` 处于 `data_phase / awaiting_eot` 时重复调用恒返回 `true`。
- 正常结束后重复调用返回 `true`；异常结束后重复调用抛对应异常。
- Block 0 收到一半且超过 `set_timeout()` 时间没有新字节时，发 NAK、丢弃半帧，
  继续等发送方重发；连续 3 次超时无进展抛 `timeout`。
- 等待 Block 0 期间完全没有收到任何字节时，每隔 `set_timeout()` 自动重发一次
  `'C'`；连续 3 次仍无进展时发 CAN 并抛 `timeout`。
- 收到过 Block 0 的任何字节后不再重发 `'C'`，半帧或坏帧只通过 NAK 重试。

### receive()

| 返回值 | 含义 |
|---|---|
| `0` | EOT 已 ACK；或已经正常结束；不会再发送任何字节 |
| `kUnknown` (SIZE_MAX) | 处理中：半帧、等待 EOT、文件大小未知，或最后一帧已交付 |
| `1 .. kUnknown-1` | 剩余待交付的文件字节数 |

- 自启动：`phase == idle` 时内部先执行一次 `start_link()` 并返回 `kUnknown`，因此不调用 `start_link()` 也能工作，只是握手阶段的异常会从 `receive()` 抛出。
- 自启动后的 Block 0 等待与 `start_link()` 共用同一套 `'C'` 心跳；只调用 `receive()` 也会自动重发 `'C'`。
- 数据帧收到一半超时：发 NAK、丢弃半帧，等发送方重发；连续 3 次超时无进展抛 `timeout`。
- 远端中止时先回发 8 个 CAN，再抛 `cancelled`。
- 已知文件大小但未收满就收到 EOT 时，先 ACK EOT，再抛 `failed`。
- 已正常结束或 `phase == finished` 时调用不会回发任何字节。

### finish_batch()

```cpp
bool finish_batch(this auto &&self);
```

- 只在 `receive()` 返回 `0` 且未抛异常后调用；其他阶段调用返回 `false`，不发任何字节。
- 非阻塞轮询：第一次调用发送 `'C'`，之后每次调用解析已到达的字节；收到空 Block 0 并回 ACK 后返回 `true`。
- 完成后重复调用返回 `true`，不再发送字节。
- 不修改 `filename()/filesize()`，已接收文件的信息仍可查询。
- 发送方还有下一个文件（非空 Block 0）时，发 8 个 CAN 并抛 `failed`；远端 CAN/CTRL-C 时发 8 个 CAN 并抛 `cancelled`。
- 发送方不发送空 Block 0 时，会一直返回 `false`，需要外层超时后停止轮询。

### cancel() / reset()

- `cancel()`：`idle` 或 `finished` 状态下是空操作；其余状态发送 8 个 `0x18`，进入 `finished`，并抛 `cancelled`。
- `reset()`：回到 `idle`，清空文件名、大小、已收字节数、帧缓冲和超时计数。用于同一会话中接收下一个文件，或中止后重新开始。

## 典型流程

```cpp
serial_io rx;
rx.set_timeout(3000); // 可选：'C' 重发周期/半帧超时，按对端调整

try
{
    while(!rx.start_link())
    {
        if(user_pressed_ctrl_c())
            rx.cancel();   // 活动传输中抛 cancelled，由外层统一处理
        // 'C' 重发与握手超时已由 start_link() 内部处理；
        // 这里只需让出 CPU 或做取消/整体调度。
    }

    while(rx.receive([&](std::span<const std::byte> data) {
              file.write(reinterpret_cast<const char *>(data.data()), data.size());
          }) != 0)
    {
        if(user_pressed_ctrl_c())
        {
            rx.cancel();
            return;
        }
        // 在这里检查整体超时
    }

    // 可选：对端是 YMODEM 批处理发送端（sz --ymodem / sb）时，
    // 完成结束握手，让发送端打印完成并退出。不支持批处理的对端可跳过。
    while(!rx.finish_batch())
    {
        if(batch_end_timeout())
            break;
    }

    commit(file);   // 正常结束
}
catch(const xeno::ymodem_receiver::timeout &) // 先捕获 timeout（继承 failed）
{
    discard(file);
}
catch(const xeno::ymodem_receiver::cancelled &)
{
    discard(file);
}
catch(const xeno::ymodem_receiver::failed &)
{
    discard(file);
}
```

回调参数是**只读**数据，可声明为以下任意一种形式（span 或 C 式指针 + 长度），
类会按回调可接受的形式交付：

```cpp
std::span<const std::byte>     /  (const std::byte *, std::size_t)
std::span<const std::uint8_t>  /  (const std::uint8_t *, std::size_t)
std::span<const char>          /  (const char *, std::size_t)
std::span<const std::int8_t>   /  (const std::int8_t *, std::size_t)
```

可写版本（如 `std::span<std::byte>`、`std::byte *`）不接受，因为数据位于类内部
缓冲区，回调返回后还会被复用。`data` **只在回调返回前有效**；需要留存必须拷贝。

`finish_batch()` 是可选的：`receive()` 返回 0 时文件数据已经完整，即使 `finish_batch()` 抛异常，也可以由调用方决定是否保留文件。

## 状态机

```
idle ──start_link()/receive()──> wait_block0 ──Block 0 ok──> data_phase
                                  │
                                  ├── EOT ─────────────> finished (failed)
                                  ├── 空文件头 ────────> finished (failed)
                                  └── CAN/Ctrl+C ──────> finished (cancelled)

data_phase ──已收满 filesize──> awaiting_eot ──EOT──> finished (ok)

data_phase / awaiting_eot ──CAN/Ctrl+C──> finished (cancelled)
data_phase ──数据未收满就 EOT──────────> finished (failed)

finished (ok) ──finish_batch()──> batch_end（发 'C'，等空 Block 0）
batch_end ──空 Block 0──> finished (batch done)
batch_end ──EOT 重传──> batch_end（再 ACK 并补发 'C'）
batch_end ──CAN/Ctrl+C──> finished (cancelled)
batch_end ──非空 Block 0──> finished (failed)

任意非 finished 状态 ──cancel()──> finished (cancelled)
```

## 协议细节

### 帧

```
[SOH|STX][seq][~seq][data 128|1024][crc_hi][crc_lo]
```

- `SOH=0x01` 为 128 字节数据帧，`STX=0x02` 为 1024 字节数据帧。
- `nseq == (uint8_t)~seq`。
- CRC16/XMODEM：多项式 `0x1021`，初值 `0x0000`，不反射，无最终异或；只覆盖数据段，不含帧头和序号。标准校验值 `"123456789" -> 0x31C3`。
- 重复帧（`seq == expected - 1`）：直接 ACK 并丢弃，不回调。若重复的是 Block 0（尚未接受过数据帧），ACK 后再补发一个 `'C'`，因为发送方此时仍在等它。
- 乱序帧 / CRC 错误 / 序号反码错误：NAK，重新解析下一帧。

### Block 0

payload 布局：`filename \0 filesize_decimal \0 [mtime ...]`

- 只解析 filename 和 filesize，忽略 mtime 及其后所有字段。
- filesize 字段为空 → `filesize() == kUnknown`。
- filesize 字段为 `"0"` → 0 字节文件。
- 非数字或溢出 → `kUnknown`。
- Block 0 **永不**触发数据回调。
- 成功：`expected_seq = 1`，回 `ACK + 'C'`。
- 文件名为空（全零文件头）在 `start_link()` 阶段视为空传输，抛 `failed`；在 `finish_batch()` 阶段则视为批处理正常结束。

### 数据交付

- 按 `min(payload.size(), filesize - received)` 截断，`0x1A` 填充不会出现在回调中。
- `filesize == kUnknown` 时无法截断，交付整帧原始 payload，直到 EOT。
- 已收满 filesize 后进入 `awaiting_eot`。此时若又收到新的数据帧（filesize 偏小），会 ACK 但不交付；这是协议层面无法区分的异常数据。
- 最后一帧的 ACK 丢失、发送方重发时，`awaiting_eot` 会把它当作重复帧 ACK，不会重复交付。

### 中止

- 远端 `CAN/CTRL_C` 只在**帧头首字节**检测；数据段中的 `0x18/0x03` 是合法数据。
- 检测到远端中止后回发 8 个 CAN，抛 `cancelled`。
- 本地 `cancel()` 同样回发 8 个 CAN。
- 远端中止后，对端 CAN 串的剩余字节可能还在接收缓冲里；重新开始前应由外层 flush 输入。

## 设计说明

- 按本项目要求使用 C++23 deducing this（`this auto &&self`）绑定派生类 IO；函数模板使用 C++20 简写形式（`auto&&` 参数）。
- 类内所有成员访问都通过 `self` 完成——显式对象参数成员函数没有隐式 `this`。
- 回调前先 `reset_frame()`：回调抛异常时不会把同一帧重复交付，也不会回 ACK，发送方会重发该帧。
- 半帧可重入：`hdr_pos_/body_pos_/body_len_` 都是成员，下一次调用从断点继续。
- 除文件名使用 `std::string` 外不使用动态内存；帧缓冲是对象内的固定数组。

## 已知限制

- 单文件模式：一次会话只接收一个文件；接收下一个文件需要 `reset()` 后重新 `start_link()`。
- 默认不在 EOT ACK 时自动完成批处理结束握手：`receive()` 返回 0 后即结束。部分批处理发送端（如 lrzsz `sz`/`sb`）会继续等待接收方发 `'C'` 并 ACK 空文件头；需要调用可选的 `finish_batch()` 才能让发送端打印完成信息并退出。不调用时直接结束会话即可。
- 不支持仅 128B 的 XMODEM 模式；握手固定发 `'C'`。
- `filesize == kUnknown` 时尾部 `0x1A` 填充无法去除，结束由 EOT 决定。
- 帧内超时只覆盖“已经收到半帧”的情况；数据阶段整帧（包括首字节）全部丢失时仍需
  调用方的整体超时机制。
- 握手阶段只用“完全没有收到字节”判定发送方未启动。如果发送方已经发出 Block 0
  但整帧丢失，接收端无法区分这两种情况；重发 `'C'` 不保证能让对端重传，仍可能
  需要外层超时或重新发起。
- 默认 `time()` 使用 `std::chrono::steady_clock`；裸机没有 chrono 时由派生类覆盖。
- 默认超时 1000ms，可通过 `set_timeout()` 调整；该值要大于串口流控可能造成的最大暂停。
- `try_getchar()` 一次只返回一个字节，不支持批量读取。
- 文件大小偏小：多出的数据帧会被 ACK 并丢弃，不会报错；文件大小偏大：提前收到 EOT 会抛 `failed`。
- `filename()` 直接来自发送方；落盘前应自行校验路径、长度和非法字符。
- 非线程安全：`start_link()/receive()/finish_batch()/cancel()/reset()` 应在同一执行上下文调用，不能在不同线程里并发操作同一个对象。

## 验证

已在项目环境中验证：

- 编译器：`arm-none-eabi-g++ 16.2.0`，`-std=gnu++26`
- 运行环境：QEMU `mps2-an500` + 半主机（`./build.sh` 的 `sim` 目标）
- 测试代码：[`test/ymodem_test.cc`](../../test/ymodem_test.cc)（复制为 `arm_simulator/main.cc` 后执行 `./build.sh`）

覆盖的用例包括：1K/SOH 正常传输、0 字节文件、未知大小、CRC/序号错误重传、重复帧、乱序帧、最后一帧重复、mtime 字段忽略、序号回绕、远端 CAN、远端 Ctrl+C、本地取消、Block 0 收到 EOT、空文件头、提前 EOT、reset 后接收第二个文件、不调用 `start_link()` 自启动、整包大小、1K 填充截断、非法大小字段、重复 Block 0、非法帧头、逐字节喂数据、数据段包含 `0x18/0x03`、回调抛异常、正常结束后取消、握手 `'C'` 自动重发与超时、半帧超时与超时异常，以及 `finish_batch()` 的正常完成、EOT 重传、下一文件、CAN 中止、未结束调用和逐字节喂数据。

结果：43 组测试全部通过。

另外用真实 `lrzsz 0.12.21` 做了联调（socketpair + 非阻塞读）：

- 发送端使用 `sz --ymodem -b` 和 `sb -b`（注意：不带 `--ymodem` 的 `sz` 是 ZMODEM，不适用于本类）。
- 文件大小 0、1024、5000 字节（随机内容），接收到的字节数与内容完全一致，CRC 无错误。
- `receive()` 返回 0 后调用 `finish_batch()` 完成结束握手，`sz`/`sb` 均打印 `Transfer complete` 并以 0 退出。
