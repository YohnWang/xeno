# xeno::zmodem 使用说明

单文件 ZMODEM 接收端，header-only，位于 `xeno/toolkit/include/xeno/zmodem.h`。
支持 hex / 16-bit CRC binary / 32-bit CRC binary 三种帧头，支持 ZDLE 转义、四种
数据子包结尾（ZCRCE/ZCRCG/ZCRCQ/ZCRCW）以及批处理结束握手。

所有文件内容都按原始二进制交付，不做 CR/LF、字符集或文本转换。

## IO 接入

通过继承 + deducing this 在编译期绑定 IO，不需要虚函数或函数指针：

```cpp
struct serial_io : xeno::zmodem
{
    // 非阻塞：返回 0..255 表示一个字节，返回负数表示暂无数据。
    int try_getchar()
    {
        return uart_rx_ready() ? uart_read() : -1;
    }

    // 发送一个字节。
    void putchar(int c)
    {
        uart_write(static_cast<std::uint8_t>(c));
    }
};
```

`try_getchar()` 必须是单字节、非阻塞的。类内不做任何等待、超时或重试计时。

## 公共接口

```cpp
static constexpr std::size_t kUnknown;

struct options
{
    std::size_t resume_offset = 0; // 0 = 从头传；非 0 = 请求 ZRPOS(offset)
    std::size_t window = 1;        // ZRINIT 接收缓冲 = window*1024，限制 1..8
};

std::string filename() const;   // ZFILE 解析成功后有效
std::size_t filesize() const;   // kUnknown 表示 ZFILE 未提供大小

bool        start_link(this auto &&self, const options &opt = {});
std::size_t receive(this auto &&self, auto &&f);
bool        finish_batch(this auto &&self);
void        cancel(this auto &&self);
void        reset();
```

### 异常

| 异常 | 触发条件 |
|---|---|
| `zmodem::cancelled` | 本地 `cancel()`；远端 CAN 串 / ZCAN / ZABORT；中止后再次调用 |
| `zmodem::failed` | 协议错误、ZFIN 提前出现、ZFERR、不支持的 ZCOMMAND/压缩/加密等 |

两者都继承 `zmodem::error`，`zmodem::error` 继承 `std::exception`。

- 本地 `cancel()` 在活跃会话中发完 10 个 CAN + 10 个 BS 后抛 `cancelled`；
  `idle/done` 状态下是空操作，不抛异常。
- 远端取消会先回发 CAN 串，再抛 `cancelled`。
- 回调内部抛出的异常原样传播。

### start_link()

| 返回值 | 含义 |
|---|---|
| `true` | 已收到 ZFILE、解析出文件信息并发出 ZRPOS |
| `false` | 已发 ZRINIT，或 ZFILE 数据尚未收完，继续轮询 |

- 第一次调用发送 ZRINIT，通告 `window` 指定的接收缓冲和
  `CANFC32 | CANFDX | CANOVIO | ESCCTL` 能力。
- 会处理 ZRQINIT、ZSINIT、ZFREECNT、ZCHALLENGE 等握手帧。
- 握手超时由调用方处理；需要重新发起时执行 `reset()` 再 `start_link()`。

### receive()

| 返回值 | 含义 |
|---|---|
| `0` | ZEOF 且文件结束 |
| `kUnknown` (SIZE_MAX) | 处理中：等待头部/数据、子包未收完、文件大小未知 |
| `1 .. kUnknown-1` | 剩余待交付文件字节数（filesize 已知时） |

- `phase == idle` 时自启动：内部先执行一次默认参数的 `start_link()` 并返回
  `kUnknown`。需要 resume/window 时请显式调用 `start_link(options)`。
- 数据子包先完成 CRC 校验，再交付给回调；CRC 错误时回 ZRPOS 请求重传。
- 回调拿到的 `data` 指向类内部 8KB 缓冲区，**只在回调返回前有效**；需要留存必须拷贝。

### finish_batch()

- 在 `receive()` 返回 `0` 后调用，非阻塞轮询。
- 第一次调用发 ZRINIT；之后如果发送端还有文件，读取其 ZFILE 元数据并回 ZSKIP；
  收到 ZFIN 后回 ZFIN 并返回 `true`。
- 单文件模式下不会交付后续文件，只把它们跳过，让发送端（`sz`/`sb` 的批处理流程）
  能打印完成信息并正常退出。
- 发送端不支持批处理结束握手时，会一直返回 `false`，需要外层超时后停止轮询。

### cancel() / reset()

- `cancel()`：活跃会话中发 10 个 `0x18` + 10 个 `0x08`，进入 `done`，然后抛
  `cancelled`；`idle/done` 状态下是空操作。
- `reset()`：清空会话、文件信息、位置、解析器和缓冲区，回到 `idle`。

## options

### resume_offset（断点续传）

- 默认 `0`：从文件头开始，收到 ZFILE 后发 `ZRPOS(0)`，发送端从头重传。
- 非 `0`：收到 ZFILE 后发 `ZRPOS(resume_offset)`，发送端从该偏移开始发送；
  `receive()` 只交付该偏移之后的数据。
- 当前实现**不做 ZCRC 校验**，调用方需要保证本地已保存的前缀与发送端一致。
- `resume_offset` 大于已知文件大小时按 `0` 处理。

### window（滑动窗口）

- ZMODEM 真正的滑动窗口由发送端控制（`sz -w N`），接收端无法强制。
- 这里的 `window` 设置 ZRINIT 通告的接收缓冲区大小：`window * 1024` 字节，
  默认 `1`（1KB），上限 `8`（8KB）。发送端会据此限制块大小。
- 接收端会按子包结尾及时回 ZACK：
  - `ZCRCQ`：回 ZACK，继续读子包
  - `ZCRCW`：回 ZACK，本帧结束，等待下一个头部
  - `ZCRCG`：不回 ACK，继续读子包
  - `ZCRCE`：不回 ACK，本帧结束，等待下一个头部
- 多线程场景可以在回调里把数据交给写线程；类本身仍按子包顺序处理，不额外缓存多个子包。

## 典型流程

```cpp
serial_io rx;
xeno::zmodem::options opt;
opt.resume_offset = 0; // 从头传；断点续传时填已有字节数
opt.window = 1;        // ZRINIT 接收缓冲 1KB

try
{
    while(!rx.start_link(opt))
    {
        if(user_pressed_ctrl_c())
            rx.cancel();
        // ZRINIT 重发/超时计时
    }

    while(rx.receive([&](std::span<std::byte> data) {
              file.write(reinterpret_cast<const char *>(data.data()), data.size());
          }) != 0)
    {
        if(user_pressed_ctrl_c())
            rx.cancel();
        // 整体超时检查
    }

    // 可选：让批处理发送端干净结束
    while(!rx.finish_batch())
    {
        if(batch_end_timeout())
            break;
    }

    commit(file);
}
catch(const xeno::zmodem::cancelled &)
{
    discard(file);
}
catch(const xeno::zmodem::failed &)
{
    discard(file);
}
```

## 状态机

```
idle ──start_link()──> wait_file ──ZFILE 元数据 ok──> data
                           │
                           ├── ZSINIT ──> ACK ──> wait_file
                           ├── ZRQINIT ──> ZRINIT
                           ├── ZFREECNT/ZCHALLENGE ──> ZACK
                           ├── ZCAN/ZABORT ──> done (cancelled)
                           └── ZFIN/ZFERR ──> done (failed)

data ──ZDATA + 子包──> data（ZCRCG/ZCRCQ）
data ──ZCRCW/ZCRCE──> 等待下一个头部
data ──ZEOF(position == received)──> file_done
data ──ZEOF(position != received)/数据 CRC 错/位置错──> 发 ZRPOS，继续 data

file_done ──finish_batch()──> batch_end
batch_end ──ZFILE（后续文件）──> ZSKIP ──> batch_end
batch_end ──ZFIN──> 回 ZFIN ──> done (ok)
batch_end ──ZCAN/ZABORT──> done (cancelled)

任意非 idle/done ──cancel()──> done (cancelled)
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
info[0..1] = 接收缓冲字节数（小端）
info[2]    = ZF1 标志（0）
info[3]    = ZF0 标志 = CANFDX | CANOVIO | CANFC32 | ESCCTL
```

发送端据此决定是否使用 32 位 CRC、是否转义控制字符，以及最大块大小。

### ZFILE

ZFILE 头后跟一个 ZCRCW 数据子包，内容是：

```
filename \0 size_decimal mtime_octal mode_octal ...
```

- 只解析 filename 和 size，忽略 mtime/mode 及其后字段。
- size 缺失或非法 → `filesize() == kUnknown`。
- `ZF2`（压缩/加密/RLE）或 `ZF3`（稀疏扩展）非 0 时直接拒绝，避免交付非原始内容。
- ZF0 的文本转换标志被忽略：内容始终按原始字节交付。
- 解析完成后发送 ZRPOS（`resume_offset` 或 0）并进入数据阶段。

### ZDATA / 子包

`ZDATA` 头的 info 是本次数据在文件中的起始位置。接收端要求它等于当前
`received_`；不一致时丢弃该帧并在帧尾发 ZRPOS 重新同步。

每个数据子包：

```
data... ZDLE <end> crc...
```

- `end` 为 `ZCRCE`/`ZCRCG`/`ZCRCQ`/`ZCRCW`。
- CRC 覆盖 data + end，不含 ZDLE。
- 先完整读取并校验，再回调；`0x1A` 填充在 ZMODEM 中不存在，不会截断。
- filesize 已知时最多交付到 filesize，超出部分仍然消费但不交付（用于位置同步）。
- `filesize == kUnknown` 时交付全部数据，结束由 ZEOF 位置决定。

### ZEOF / ZFIN

- ZEOF 的 info 是文件总位置。等于 `received_` 时当前文件结束，`receive()` 返回 0。
- 位置不一致时回 ZRPOS，继续等待重传。
- `finish_batch()` 发 ZRINIT 请求后续内容；后续 ZFILE 用 ZSKIP 跳过；收到 ZFIN
  后回 ZFIN，发送端打印完成并退出。

### 取消

- 本地 `cancel()` / 远端 CAN 串 / ZCAN / ZABORT 都会发 CAN 串并抛 `cancelled`。
- CAN 只在头部搜索或 ZDLE 转义上下文中识别；普通数据字节经过 ZDLE 转义，
  不会误判。
- 远端取消后对端可能还有 CAN 字节在接收缓冲里；重新开始前应由外层 flush。

## 设计说明

- 使用 C++23 deducing this（`this auto &&self`）+ 继承绑定 IO；函数模板使用
  C++20 简写形式。所有显式对象参数成员访问都通过 `self` 完成。
- 非阻塞、可重入：头部解析器、ZDLE 状态、数据子包 CRC 位置都保存在成员里，
  半帧/半子包可以在下一次调用继续。
- 头部解析成功后保留一个 pushback 字节，用于把 hex 头后的非 CR/LF 字节交还
  给下一个解析阶段。
- 固定 8KB 数据缓冲区，覆盖 lrzsz 的最大块（MAX_BLOCK=8192）。
- 不使用动态内存（文件名 `std::string` 除外）；会话状态全在对象内。

## 已知限制

- 单文件模式：`finish_batch()` 会跳过后续文件，不交付给回调。
- 断点续传只发 ZRPOS，不做 ZCRC 校验；需要调用方保证前缀一致。
- 不支持压缩（LZW）、加密、RLE、稀疏文件扩展；遇到时抛 `failed`。
- 不执行 ZCOMMAND 远程命令；收到时发 CAN 并抛 `failed`。
- ZMODEM 真正的滑动窗口在发送端（`sz -w`）；`options.window` 只控制接收端
  通告的缓冲大小。
- 最大数据子包 8KB，ZRINIT 通告的缓冲也限制在 8KB。
- 不限制重传次数；连续 NAK/ZRPOS 或长时间无数据需要外层超时机制。
- `filename()` 直接来自发送方；落盘前应校验路径、长度和非法字符。
- 非线程安全：`start_link()/receive()/finish_batch()/cancel()/reset()` 应在同一
  执行上下文调用。
- hex 头假定按标准以 CR/LF（或 lrzsz 的 CR/0x8A）结束；非标准发送端可能不兼容。

## 验证

模拟测试：[`test/zmodem_test.cc`](../../test/zmodem_test.cc)

- 在 host（g++-16）和 `arm_simulator`（arm-none-eabi-g++ 16.2.0 + QEMU
  mps2-an500）上均通过。
- 覆盖：CRC16/CRC32 校验值、hex 头、ZBIN 头、ZBIN32 头、ZDLE 全控制字符转义、
  四种 ZCRCE/ZCRCG/ZCRCQ/ZCRCW 结尾、ZFILE 元数据、0 字节文件、未知大小、
  ZSINIT、ZRQINIT、ZFREECNT、ZCHALLENGE、ZCAN、ZABORT、ZCOMMAND、ZSTDERR、
  提前 ZEOF、压缩/加密标志拒绝、坏头 CRC 重同步、坏数据 CRC/ZRPOS、ZDATA
  位置错重同步、resume、window、批处理 ZSKIP、逐字节喂数据、本地取消。

真实 lrzsz 联调：[`test/zmodem_sz_test.cc`](../../test/zmodem_sz_test.cc)

- `lrzsz 0.12.21` 的 `sz -b`（ZMODEM）。
- 文件大小 0、1024、5000、100000 字节，内容随机；接收内容逐字节一致。
- `-e`（转义）、`-w 4096`、`-w 8192`（发送端窗口）均通过。
- `options.resume_offset = 3000/5000/99999` 均通过；只接收偏移之后的数据。
- 两个文件批处理：接收第一个，`finish_batch()` 跳过第二个，发送端退出码 0。
- `finish_batch()` 后发送端打印 `Transfer complete` 并正常退出。
