# xeno::ymodem_receiver 使用说明

单文件 YMODEM-1K 接收端，header-only，位于 `xeno/toolkit/include/xeno/ymodem.h`。
支持 SOH(128B) / STX(1KB) 帧、CRC16/XMODEM、Block 0 文件名与大小解析。

所有文件内容都按原始二进制交付，不做 CR/LF 或字符集转换。不支持未知文件大小。

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

    // 可选：默认 std::chrono::steady_clock；返回 int 或 int64_t 毫秒。
    std::int64_t time() const
    {
        return board_millis();
    }

    // 可选：阻塞接口用它等待；默认 std::this_thread::sleep_for。
    template<typename Rep, typename Period>
    void sleep_for(const std::chrono::duration<Rep, Period> &d)
    {
        // 可以在这里接入 RTOS 延时。
    }
};
```

`try_getchar()` 必须是单字节、非阻塞的，返回类型由类内 `static_assert` 强制为
`int`。若底层缓冲区是 `char`/`int8_t`，返回前务必转 `unsigned char`。

## 公共接口

```cpp
static constexpr std::size_t kUnknown; // 仅 link 成功前 filesize() 使用

std::string filename() const;   // link 成功后有效
std::size_t filesize() const;   // link 成功后为实际大小

template<typename Rep, typename Period>
void set_link_timeout(const std::chrono::duration<Rep, Period> &total);
template<typename Rep, typename Period>
void set_link_timeout(const std::chrono::duration<Rep, Period> &retry_interval,
                      std::uint32_t retries);
template<typename Rep, typename Period>
void set_receive_timeout(const std::chrono::duration<Rep, Period> &total);
template<typename Rep, typename Period>
void set_receive_timeout(const std::chrono::duration<Rep, Period> &retry_interval,
                         std::uint32_t retries);

bool try_link(this auto &&self);    // 非阻塞单次处理，不抛 timeout
void link(this auto &&self);        // 阻塞，超时抛 timeout

// span 出参：有数据时非空，无数据/结束时为空。
int try_receive(this auto &&self, std::span<const T> &data);
int try_receive(this auto &&self, auto &&f);
int receive(this auto &&self, std::span<const T> &data);
int receive(this auto &&self, auto &&f);

bool try_finish_batch(this auto &&self);
void finish_batch(this auto &&self);

void cancel(this auto &&self, std::string reason = {}); // 发 CAN 并抛 cancelled
```

span 出参支持与回调一致的四种只读元素类型：

```cpp
std::span<const std::byte> &     std::span<const std::uint8_t> &
std::span<const char> &          std::span<const std::int8_t> &
```

`data` 指向类内部帧缓冲区，只在下次调用前有效；需要留存必须拷贝。

### 返回值与结束判断

- `try_receive()` / `receive()` 返回 `int`，使用 POSIX `read()` 风格：
  - 正数：本次调用开始前的剩余文件字节数，等于
    “本次交付长度 + 交付后剩余长度”，是一个倒计时；
  - `0`：本次处理了 EOT，文件结束；
  - `-1`：仅 `try_receive()`，本次没有取得完整数据，也没有处理结束帧。
- `receive()` 永不返回 -1；没有数据时内部等待/重试，超时抛 `timeout`。
- 最后一帧返回自身长度（正数）；下一次成功处理结束帧的调用返回 0。
- span 非空 / 回调被调用，当且仅当返回正数。
- 文件大小上限为 `INT_MAX`；未知大小或超限会在 `try_link()` 中抛 `failed`。

### 异常

| 异常 | 触发条件 |
|---|---|
| `cancelled` | 本地 `cancel()`；远端 CAN/CTRL-C；中止后再次调用 |
| `failed` | 握手收到 EOT、空文件头、未知文件大小、数据未收完就 EOT、批处理还有下一个文件等 |
| `timeout` | 阻塞接口重试耗尽；继承 `failed` |

- `try_link()`、`try_receive()`、`try_finish_batch()` **不抛 timeout**；
  `cancelled` / `failed` 和回调抛出的异常仍然传播。
- `cancel(reason)` 抛出的 `cancelled::what()` 为
  `"ymodem: transfer cancelled: " + reason`；`reason` 为空或远端中止时返回
  `"ymodem: transfer cancelled"`。异常基类是 `std::runtime_error`。
- `link()`、`receive()`、`finish_batch()` 超时后发 8 个 CAN，设置
  `end_state::timeout` 并抛 `timeout`。
- `receive()` / `try_receive()` 必须在 `link()` / `try_link()` 成功后调用；
  未连接时抛 `failed`。
- `finish_batch()` 在 `receive()` 返回 0 后调用；未完成时抛 `failed`。
  `receive()` 返回 0 时已经处理了结束帧；`finish_batch()` 只负责可选的
  批处理结束（空 Block 0 / ZFIN）。

## 超时配置

基类私有默认成员：

```cpp
std::chrono::milliseconds link_retry_interval{1000};
std::uint32_t link_retries = 30;       // 约 30s
std::chrono::milliseconds receive_retry_interval{1000};
std::uint32_t receive_retries = 3;     // 约 3s
```

- 单参数 `set_link_timeout(total)` / `set_receive_timeout(total)`：
  使用当前配置的 retry interval（默认 1s）计算重试次数，
  `retries = ceil(total / interval)`；不足一个间隔向上取整。
  `total <= 0` 关闭该阶段超时；之后正数设置时若 interval 为 0，恢复为 1s。
- 双参数 `set_*_timeout(interval, retries)`：设置单次重试间隔和重试次数；
  `interval <= 0` 关闭，`retries == 0` 按 1 次处理。
- `link()` 使用 link 配置；`receive()` 和 `finish_batch()` 使用 receive 配置。
- 某阶段 interval 为 0 时关闭自动超时/重试，对应阻塞接口只轮询而不主动超时。

### 派生类覆盖配置

基类参数是私有成员；派生类声明同名 public 成员即可覆盖，基类通过 `self.`
读取，不需要调用 setter：

```cpp
struct serial_io : xeno::ymodem_receiver
{
    std::chrono::milliseconds link_retry_interval{500};
    std::uint32_t link_retries = 10;
    std::chrono::milliseconds receive_retry_interval{2000};
    std::uint32_t receive_retries = 5;
    // ...
};
```

## try_link / link

### try_link()

单次非阻塞处理，不抛 timeout：

- idle：发 `'C'`，进入等待 Block 0。
- 等待阶段：只解析当前已有字节，不按时间重发，不检查超时。
- 成功：Block 0 完整且文件大小已知，返回 true。
- 未完成：返回 false。
- 未知文件大小：发 CAN、设置 failed、抛 `failed`。
- 取消/协议失败：按现有逻辑发 CAN，抛 `cancelled` / `failed`。

### link()

阻塞直到连接成功或超时：

- 循环调用 `try_link()`；成功返回。
- 用 `sleep_for()` 轮询，单次最多 10ms。
- 每经过 `link_retry_interval`：
  - 存在半帧：发 NAK，丢弃半帧；
  - 没有半帧：重发 `'C'`。
- 收到字节/推进时清零重试计数；达到 `link_retries` 后发 CAN 并抛 timeout。
- 未知大小、取消和协议失败原样传播。

## try_receive / receive

### try_receive()

单次非阻塞处理：

- 只解析当前已有字节，不按时间重试，不检查超时。
- 最多交付一帧；有数据时设置 span（非空）或调用回调。
- 有数据返回正数；文件结束时返回 0；没有取得数据/结束帧时返回 -1。
- 取消/协议失败照常抛异常；不抛 timeout。

### receive()

阻塞到交付一段非空数据或文件结束：

- 循环调用 `try_receive()`，用 `sleep_for()` 轮询（单次最多 10ms）。
- 有数据：返回正数（调用前剩余字节数，含本次交付）；span 非空 /
  回调已调用。
- 文件结束：返回 0；span 为空，不调用回调。
- `receive()` 永不返回 -1；无数据时内部等待/重试。
- 无数据时每经过 `receive_retry_interval` 主动重试：
  - 有半帧：发 NAK 并丢弃半帧；
  - 没有半帧：发 NAK 请求对方重传。
- 收到字节推进时清零重试计数；达到 `receive_retries` 后发 CAN 并抛 timeout。

## try_finish_batch / finish_batch

- `try_finish_batch()`：非阻塞单次处理，不抛 timeout。第一次调用发 `'C'`；
  收到空 Block 0 并 ACK 后返回 true。未完成 receive 时返回 false。
- `finish_batch()`：阻塞轮询，使用 receive 超时配置，成功返回；
  超时发 CAN 并抛 timeout。未完成 receive 时抛 `failed`。
- 发送方还有下一个文件时发 CAN 并抛 `failed`。

## 典型流程

```cpp
serial_io rx;

try
{
    rx.link(); // 内部自动重发 'C' 并处理超时

    std::span<const std::byte> data;
    while(rx.receive(data))
    {
        file.write(reinterpret_cast<const char *>(data.data()), data.size());
    }

    rx.finish_batch(); // 可选：批处理结束握手
    commit(file);
}
catch(const xeno::ymodem_receiver::timeout &)
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

回调版：

```cpp
rx.link();
while(rx.receive([&](std::span<const std::byte> data)
{
    file.write(reinterpret_cast<const char *>(data.data()), data.size());
}))
{
}
rx.finish_batch();
```

非阻塞 try 版：

```cpp
for(;;)
{
    int n = rx.try_receive(data);
    if(n < 0)
        continue; // 暂无数据/结束帧
    if(n == 0)
        break; // 文件结束
    file.write(reinterpret_cast<const char *>(data.data()), data.size());
}
rx.finish_batch();
```

## 状态机

```
idle ──try_link()/link()──> wait_block0 ──Block 0 ok──> data_phase
                                  │
                                  ├── EOT ─────────────> finished (failed)
                                  ├── 空文件头 ────────> finished (failed)
                                  ├── 未知大小 ────────> finished (failed)
                                  └── CAN/Ctrl+C ──────> finished (cancelled)

data_phase ──已收满 filesize──> awaiting_eot ──EOT──> finished (ok)
data_phase / awaiting_eot ──CAN/Ctrl+C──> finished (cancelled)
data_phase ──数据未收满就 EOT──────────> finished (failed)

finished (ok) ──try_finish_batch()/finish_batch()──> 批处理结束
```

## 协议细节

### 帧

```
[SOH|STX][seq][~seq][data 128|1024][crc_hi][crc_lo]
```

- `SOH=0x01` 为 128 字节数据帧，`STX=0x02` 为 1KB 数据帧。
- `nseq == (uint8_t)~seq`。
- CRC16/XMODEM：多项式 `0x1021`，初值 0，不反射，无最终异或；只覆盖数据段，
  不含帧头和序号。标准校验值 `"123456789" -> 0x31C3`。
- 重复帧（`seq == expected - 1`）：直接 ACK 并丢弃，不回调。若重复的是
  Block 0（尚未接受过数据帧），ACK 后再补发一个 `'C'`。
- 乱序帧 / CRC 错误 / 序号反码错误：NAK，重新解析下一帧。

### Block 0

payload 布局：`filename \0 filesize_decimal \0 [mtime ...]`

- 只解析 filename 和 filesize，忽略 mtime 及其后字段。
- **不再支持未知大小**：filesize 缺失、非数字或溢出都会在 `try_link()` 中
  发 CAN、设置 failed 并抛 `failed`。
- filesize 为 `"0"` 表示 0 字节文件。
- Block 0 永不触发数据回调。
- 成功：`expected_seq = 1`，回 `ACK + 'C'`。
- 文件名为空（全零文件头）在连接阶段视为失败。

### 数据交付

- 按 `min(payload.size(), filesize - received)` 截断，`0x1A` 填充不会出现在回调中。
- 已收满 filesize 后进入 `awaiting_eot`；多出的数据帧仍会 ACK 但不交付。
- 最后一帧的 ACK 丢失、发送方重发时，`awaiting_eot` 会把它当作重复帧 ACK，
  不会重复交付。
- `receive()` 无数据且没有半帧时会主动发 NAK，避免双方都等对方的重传。

### 中止

- 远端 `CAN/CTRL_C` 只在帧头首字节检测；数据段中的 `0x18/0x03` 是合法数据。
- 检测到远端中止后回发 8 个 CAN，抛 `cancelled`。
- 本地 `cancel(reason)` 同样先回发 8 个 CAN，再抛 `cancelled`；`reason`
  非空时追加为 `...cancelled: <reason>`。发送方收到 CAN 串后中止本次传输。

## 已知限制

- 单文件模式：一次会话只接收一个文件；接收下一个文件需要 `reset()` 后重新 `link()`。
- 不支持仅 128B 的 XMODEM 模式；握手固定发 `'C'`。
- 不支持未知文件大小。
- 单文件大小上限为 `INT_MAX`；超过时 link 阶段直接失败。
- 数据阶段整帧（包括首字节）全部丢失时，依赖阻塞接口的主动重试；
  如果对端完全不响应，最终由 receive 超时异常结束。
- `try_*` 不负责计时和超时；需要自动重试时使用阻塞接口。
- 超时值要大于串口流控可能造成的最大暂停。
- `try_getchar()` 一次只返回一个字节，不支持批量读取。
- 文件大小偏小：多出的数据帧会被 ACK 并丢弃；文件大小偏大：提前 EOT 会抛 `failed`。
- `filename()` 来自发送方；落盘前应自行校验路径、长度和非法字符。
- 非线程安全：同一对象上的接口应在同一执行上下文调用。

## 验证

测试：[`test/ymodem_test.cc`](../../test/ymodem_test.cc)

- host（g++-16）和 `arm_simulator`（arm-none-eabi-g++ 16.2.0 + QEMU
  mps2-an500）均通过，50 组用例。
- 覆盖：正常传输、Block 0、CRC/序号错误、重复/乱序帧、帧缓冲、回调类型、
  span 出参、try/link 与 try/receive 的非阻塞/阻塞行为、主动 NAK 重试、
  链路/接收超时、配置覆盖、批处理结束、取消和未知大小拒绝等。

真实 lrzsz 联调：[`test/ymodem_sb_test.cc`](../../test/ymodem_sb_test.cc)

- `lrzsz 0.12.21rc` 的 `sb -b`。
- 文件大小 0、1024、5000 字节，内容逐字节一致。
- `link()`、`receive()`、`finish_batch()` 正常完成，发送端退出码 0。
