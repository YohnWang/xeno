# xeno::zmodem_receiver 使用说明

单文件 ZMODEM 接收端，header-only，位于 `xeno/toolkit/include/xeno/zmodem.h`。
支持 hex / 16-bit CRC binary / 32-bit CRC binary 三种帧头，支持 ZDLE 转义、四种
数据子包结尾（ZCRCE/ZCRCG/ZCRCQ/ZCRCW）以及批处理结束握手。

所有文件内容都按原始二进制交付，不做 CR/LF、字符集或文本转换。不支持未知文件大小。

## IO 接入

通过继承 + deducing this 在编译期绑定 IO，不需要虚函数或函数指针：

```cpp
struct serial_io : xeno::zmodem_receiver
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
    }
};
```

`try_getchar()` 必须是单字节、非阻塞的，返回类型由类内 `static_assert` 强制为
`int`。若底层缓冲区是 `char`/`int8_t`，返回前务必转 `unsigned char`。

## 公共接口

```cpp
static constexpr std::size_t kUnknown; // 仅 link 成功前 filesize() 使用

struct options
{
    std::size_t resume_offset = 0; // 0 = 从头传；非 0 = 请求 ZRPOS(offset)
};

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

bool try_link(this auto &&self, const options &opt = {});
void link(this auto &&self, const options &opt = {});

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

`data` 指向类内部 8KB 缓冲区，只在下次调用前有效；需要留存必须拷贝。

### 返回值与结束判断

- `try_receive()` / `receive()` 返回 `int`，使用 POSIX `read()` 风格：
  - 正数：本次调用开始前的剩余文件字节数，等于
    “本次交付长度 + 交付后剩余长度”，是一个倒计时；
  - `0`：本次处理了 ZEOF，文件结束；
  - `-1`：仅 `try_receive()`，本次没有取得完整数据，也没有处理结束帧。
- `receive()` 永不返回 -1；没有数据时内部等待/重试，超时抛 `timeout`。
- 最后一帧返回自身长度（正数）；下一次成功处理结束帧的调用返回 0。
- span 非空 / 回调被调用，当且仅当返回正数。
- 文件大小上限为 `INT_MAX`；未知大小或超限会在 `try_link()` 中抛 `failed`。

### 异常

| 异常 | 触发条件 |
|---|---|
| `cancelled` | 本地 `cancel()`；远端 CAN 串 / ZCAN / ZABORT；中止后再次调用 |
| `failed` | 协议错误、ZFIN 提前出现、ZFERR、未知文件大小、不支持的 ZCOMMAND/压缩/加密等 |
| `timeout` | 阻塞接口重试耗尽；继承 `failed` |

- `try_link()`、`try_receive()`、`try_finish_batch()` **不抛 timeout**；
  `cancelled` / `failed` 和回调抛出的异常仍然传播。
- `cancel(reason)` 抛出的 `cancelled::what()` 为
  `"zmodem: transfer cancelled: " + reason`；`reason` 为空或远端中止时返回
  `"zmodem: transfer cancelled"`。异常基类是 `std::runtime_error`。
- `link()`、`receive()`、`finish_batch()` 超时后发 10 个 CAN + 10 个 BS，
  设置 `end_state::timeout` 并抛 `timeout`。
- `receive()` / `try_receive()` 必须在 link 成功后调用；未连接时抛 `failed`。
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

### 派生类覆盖配置

基类参数是私有成员；派生类声明同名 public 成员即可覆盖，基类通过 `self.`
读取：

```cpp
struct serial_io : xeno::zmodem_receiver
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

- idle：发 ZRINIT，进入等待 ZFILE。
- 等待阶段：只解析当前已有字节，不按时间重发，不检查超时。
- 成功：ZFILE 元数据完整且文件大小已知，返回 true。
- 未完成：返回 false。
- 未知文件大小：发 CAN、设置 failed、抛 `failed`。
- 取消/协议失败：抛 `cancelled` / `failed`。

### link()

阻塞直到连接成功或超时：

- 循环调用 `try_link(opt)`；成功返回。
- 用 `sleep_for()` 轮询，单次最多 10ms。
- 每经过 `link_retry_interval`：
  - 存在半帧头部：发 ZNAK，重置解析器；
  - 存在半帧 ZFILE 元数据：发 ZNAK，丢弃子包；
  - 没有半帧：重发 ZRINIT。
- 收到字节/推进时清零重试计数；达到 `link_retries` 后发 CAN 并抛 timeout。

## try_receive / receive

### try_receive()

单次非阻塞处理：

- 只解析当前已有字节，不按时间重试，不检查超时。
- 最多交付一个数据子包；有数据时设置 span（非空）或调用回调。
- 有数据返回正数；ZEOF 后返回 0；没有取得数据/结束帧时返回 -1。
- 取消/协议失败照常抛异常；不抛 timeout。

### receive()

阻塞到交付一段非空数据或文件结束：

- 循环调用 `try_receive()`，用 `sleep_for()` 轮询（单次最多 10ms）。
- 有数据：返回正数（调用前剩余字节数，含本次交付）；span 非空 /
  回调已调用。
- ZEOF：返回 0；span 为空，不调用回调。
- 无数据时每经过 `receive_retry_interval` 主动重试：
  - 有半帧头部：ZNAK + 重置解析器；
  - 有半帧数据：ZRPOS(received_) 或 ZNAK（元数据子包）并清理；
  - 没有半帧：ZRPOS(received_)，请求对方从已提交位置重传。
- 收到字节推进时清零重试计数；达到 `receive_retries` 后发 CAN 并抛 timeout。

## try_finish_batch / finish_batch

- `try_finish_batch()`：非阻塞单次处理，不抛 timeout。第一次调用发 ZRINIT；
  后续文件用 ZSKIP 跳过；收到 ZFIN 并回 ZFIN 后返回 true。
  未完成 receive 时返回 false。
- `finish_batch()`：阻塞轮询，使用 receive 超时配置，成功返回；
  超时发 CAN 并抛 timeout。未完成 receive 时抛 `failed`。

## options

### resume_offset（断点续传）

- 默认 `0`：从文件头开始，收到 ZFILE 后发 `ZRPOS(0)`。
- 非 `0`：收到 ZFILE 后发 `ZRPOS(resume_offset)`，发送端从该偏移开始发送；
  `receive()` 只交付该偏移之后的数据。
- 当前实现**不做 ZCRC 校验**，调用方需保证本地已保存的前缀与发送端一致。
- `resume_offset` 大于文件大小时按 `0` 处理。

### 接收缓冲与窗口

- ZRINIT 的 `ZP0/ZP1` 固定为 `0`，表示允许连续 I/O（streaming）。
  WindTerm 等终端据此自动识别接收请求并弹出文件选择框；填非 0 值可能不再被
  自动识别。
- 接收缓冲固定 8KB，是单个数据子包的硬上限；超过 8KB 的子包会触发 ZRPOS。
- 滑动窗口完全由发送端控制（`sz -w N`），接收端只能配合。
- 子包结束的 ACK 行为：
  - `ZCRCQ`：回 ZACK，继续读子包
  - `ZCRCW`：回 ZACK，本帧结束，等待下一个头部
  - `ZCRCG`：不回 ACK，继续读子包
  - `ZCRCE`：不回 ACK，本帧结束，等待下一个头部
- 多线程场景可以在回调里把数据交给写线程；类本身仍按子包顺序处理。

## 典型流程

```cpp
serial_io rx;
xeno::zmodem_receiver::options opt;
opt.resume_offset = 0;

try
{
    rx.link(opt); // 内部重发 ZRINIT、处理超时

    std::span<const std::byte> data;
    while(rx.receive(data))
    {
        file.write(reinterpret_cast<const char *>(data.data()), data.size());
    }

    rx.finish_batch(); // 可选：批处理结束
    commit(file);
}
catch(const xeno::zmodem_receiver::timeout &)
{
    discard(file);
}
catch(const xeno::zmodem_receiver::cancelled &)
{
    discard(file);
}
catch(const xeno::zmodem_receiver::failed &)
{
    discard(file);
}
```

回调版：

```cpp
rx.link(opt);
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
idle ──try_link()/link()──> wait_file ──ZFILE 元数据 ok──> data
                                │
                                ├── ZSINIT ──> ZACK ──> wait_file
                                ├── ZRQINIT ──> ZRINIT
                                ├── ZFREECNT/ZCHALLENGE ──> ZACK
                                ├── ZCAN/ZABORT ──> done (cancelled)
                                └── ZFIN/ZFERR/未知大小 ──> done (failed)

data ──ZDATA + 子包──> data（ZCRCG/ZCRCQ）
data ──ZCRCW/ZCRCE──> 等待下一个头部
data ──ZEOF(position == received)──> file_done
data ──ZEOF(position != received)/数据 CRC 错/位置错──> ZRPOS，继续 data

file_done ──try_finish_batch()/finish_batch()──> batch_end
batch_end ──ZFILE（后续文件）──> ZSKIP ──> batch_end
batch_end ──ZFIN──> 回 ZFIN ──> done (ok)
```

## 协议细节

### 帧头

接收端识别：

- `ZPAD ZPAD ZDLE ZHEX`：hex 头，CRC16
- `ZPAD ZDLE ZBIN`：binary 头，CRC16
- `ZPAD ZDLE ZBIN32`：binary 头，CRC32

`ZPAD` 可以是 `0x2A` 或 `0xAA`，允许连续多个 `ZPAD`。
hex 头后的 CR/LF（或 lrzsz 使用的 CR/0x8A）会被消费，不会混入数据。
binary 头中的 type/info/CRC 以及数据子包都按 ZDLE 转义/反转义：

- `ZDLE` 后跟 `(byte ^ 0x40)` 表示原始字节
- `ZDLE 'l'` / `ZDLE 'm'` 表示 `0x7F` / `0xFF`
- 原始 XON/XOFF 作为流控字符忽略
- `ZDLE CAN` 或连续 CAN 表示取消

CRC：

- CRC16：多项式 `0x1021`，初值 0，不反射；接收端检查
  `CRC16(type+info+crc) == 0`，数据子包检查 `CRC16(data+end+crc) == 0`。
- CRC32：反射多项式 `0xEDB88320`，初值 `0xFFFFFFFF`，发送值取反；
  接收端检查残余值 `0xDEBB20E3`。

发送端回包使用 hex 头 + CRC16（与 lrzsz 的 `zshhdr` 兼容），并在非 ZFIN/ZACK
后附加 XON。

### ZRINIT

接收端主动发 ZRINIT，`info` 布局：

```
info[0..1] = 接收缓冲字节数（小端，固定 0）
info[2]    = ZF1 标志（0）
info[3]    = ZF0 标志 = CANFDX | CANOVIO | CANFC32 | ESCCTL
```

等待 ZFILE 期间，阻塞 `link()` 会按 link 超时配置重发 ZRINIT。

### ZFILE

ZFILE 头后跟一个 ZCRCW 数据子包，内容是：

```
filename \0 size_decimal mtime_octal mode_octal ...
```

- 只解析 filename 和 size，忽略 mtime/mode 及其后字段。
- **不再支持未知大小**：size 缺失或非法时发 CAN、设置 failed 并抛 `failed`。
- `ZF2`（压缩/加密/RLE）或 `ZF3`（稀疏扩展）非 0 时直接拒绝。
- ZF0 的文本转换标志被忽略：内容始终按原始字节交付。
- 解析完成后发送 ZRPOS（`resume_offset` 或 0）并进入数据阶段。

### ZDATA / 子包

`ZDATA` 头的 info 是本次数据在文件中的起始位置。接收端要求它等于当前
`received_`；不一致时丢弃该帧并请求 ZRPOS 重新同步。

每个数据子包：

```
data... ZDLE <end> crc...
```

- `end` 为 `ZCRCE`/`ZCRCG`/`ZCRCQ`/`ZCRCW`。
- CRC 覆盖 data + end，不含 ZDLE。
- 先完整读取并校验，再回调；`0x1A` 填充在 ZMODEM 中不存在，不会截断。
- 交付到 filesize 为止，超出部分仍然消费但不交付（用于位置同步）。

### ZEOF / ZFIN

- ZEOF 的 info 是文件总位置。等于 `received_` 时当前文件结束；
  位置不一致时回 ZRPOS，继续等待重传。
- `finish_batch()` 发 ZRINIT 请求后续内容；后续 ZFILE 用 ZSKIP 跳过；
  收到 ZFIN 后回 ZFIN，发送端打印完成并退出。

### 取消

- 本地 `cancel(reason)` 先发 10 个 CAN + 10 个 0x08，再抛 `cancelled`；
  `reason` 非空时追加为 `...cancelled: <reason>`。发送方收到 CAN 串后中止本次传输。
- 远端 CAN 串 / ZCAN / ZABORT 抛默认消息的 `cancelled`。
- CAN 只在头部搜索或 ZDLE 转义上下文中识别；普通数据字节经过 ZDLE 转义，
  不会误判。

## 设计说明

- 使用 C++23 deducing this（`this auto &&self`）+ 继承绑定 IO；函数模板使用
  C++20 简写形式。
- 非阻塞接口 `try_*` 只解析当前字节；阻塞接口负责 sleep、定时重发和超时。
- 固定 8KB 数据缓冲区，覆盖 lrzsz 的最大块（MAX_BLOCK=8192）。
- 不使用动态内存（文件名 `std::string` 除外）；会话状态全在对象内。

## 已知限制

- 单文件模式：`finish_batch()` 会跳过后续文件，不交付给回调。
- 断点续传只发 ZRPOS，不做 ZCRC 校验；需要调用方保证前缀一致。
- 不支持压缩（LZW）、加密、RLE、稀疏文件扩展；遇到时抛 `failed`。
- 不支持未知文件大小。
- 单文件大小上限为 `INT_MAX`；超过时 link 阶段直接失败。
- 不执行 ZCOMMAND 远程命令；收到时发 CAN 并抛 `failed`。
- 接收缓冲固定 8KB，ZRINIT 通告 0（允许连续 I/O）；超过 8KB 的数据子包会触发
  ZRPOS 重传。
- 滑动窗口在发送端（`sz -w`），接收端只能配合。
- `try_*` 不负责计时和超时；需要自动重试时使用阻塞接口。
- 默认超时值要大于串口流控可能造成的最大暂停。
- CRC/协议错误的 NAK/ZRPOS 重传由阻塞接口计数；`try_*` 只处理当前状态。
- `filename()` 直接来自发送方；落盘前应校验路径、长度和非法字符。
- 非线程安全：同一对象上的接口应在同一执行上下文调用。

## 验证

模拟测试：[`test/zmodem_test.cc`](../../test/zmodem_test.cc)

- host（g++-16）和 `arm_simulator`（arm-none-eabi-g++ 16.2.0 + QEMU
  mps2-an500）均通过，35 组用例。
- 覆盖 CRC16/CRC32、hex/ZBIN/ZBIN32 头、ZDLE 全控制字符转义、四种子包结尾、
  ZFILE 元数据、0 字节文件、未知大小拒绝、ZSINIT、ZRQINIT、ZFREECNT、
  ZCHALLENGE、ZCAN、ZABORT、ZCOMMAND、ZSTDERR、提前 ZEOF、坏 CRC/位置重同步、
  resume、接收缓冲通告、批处理 ZSKIP、逐字节喂数据、本地取消、span/回调类型、
  try/link 与 receive 的阻塞/非阻塞行为、主动 ZRPOS/半帧重试、超时异常和
  派生类超时配置覆盖。

真实 lrzsz 联调：[`test/zmodem_sz_test.cc`](../../test/zmodem_sz_test.cc)

- `lrzsz 0.12.21rc` 的 `sz -b`。
- 文件大小 0、1024、5000、100000 字节，内容逐字节一致。
- `-e`、`-w 4096` 通过。
- `resume_offset = 3000` 通过；只接收偏移之后的数据。
- `link()`、`receive()`、`finish_batch()` 正常完成，发送端退出码 0。
