# xeno::gzdecoder_basic 使用说明

流式 gzip 解压器，header-only，位于 `xeno/toolkit/include/xeno/gzip.h`。
完整实现 RFC 1951 DEFLATE（stored / fixed / dynamic、32 KiB 回溯窗口）和
RFC 1952 gzip 容器，结束前校验 trailer 的 CRC32 与 ISIZE。

```cpp
template<std::size_t N, std::size_t W = 32768>
class gzdecoder_basic;

using gzdecore  = gzdecoder_basic<4096>;
using gzdecoder = gzdecore;
```

- `N`：压缩输入环的大小，`write()` 把原始 gzip 字节写进这块缓冲区。
- `W`：DEFLATE 历史窗口大小，默认 32768（标准最大值）。RAM 紧张时可以缩小，
  但距离超过 `W` 的合法流会报 `distance_too_far`。

解压器是单成员的：trailer 校验通过即 `finished()==true`，不支持
`cat a.gz b.gz` 这种串联成员，成员之后的字节会被忽略。

## 快速开始

对象本身包含 32 KiB 窗口和约 2 KiB 码表，N=4096 时约 39 KiB。栈空间紧张
（多数 MCU 的默认任务栈）时用 `std::make_unique` 把整个对象放到堆上：

```cpp
#include<memory>
#include<xeno/gzip.h>

auto dec = std::make_unique<xeno::gzdecore>();
```

也可以放在 static/全局区：

```cpp
static xeno::gzdecore dec;
```

内部不再做堆分配；`make_unique` 是唯一一次分配。

## 流式泵循环

`write()` 只接收数据，不驱动解码。输入环满时返回 0，必须调用 `read()`
腾出空间。下面的循环适用于任意分块的 gzip 数据：

```cpp
#include<algorithm>
#include<cstdint>
#include<memory>
#include<span>
#include<xeno/gzip.h>

// consume 返回 0 表示继续；返回非 0 时中止解压。
int pump_gzip(std::span<const std::uint8_t> gz,
              int (*consume)(const void *, std::size_t))
{
    auto dec = std::make_unique<xeno::gzdecore>();
    std::uint8_t out[512];
    std::size_t pos = 0;

    for(;;)
    {
        if(pos < gz.size())
        {
            int w = dec->write(gz.data() + pos,
                               std::min<std::size_t>(gz.size() - pos, 1024));
            if(w < 0)                       // 格式错误 / finished 后误写
                return -1;
            pos += static_cast<std::size_t>(w);
        }

        int r = dec->read(out, sizeof out);
        if(r < 0)                           // CRC/ISIZE/码流错误
            return -2;
        if(r > 0)
        {
            if(consume(out, static_cast<std::size_t>(r)) != 0)
                return -3;
            continue;
        }

        if(dec->finished())
            return 0;
        if(pos == gz.size())
            return -4;                      // 输入已写完但未结束：文件被截断
        // 否则只是暂时需要更多输入，继续写
    }
}
```

要点：

- `write` 可以只接受一部分（返回值 < 请求长度），按返回值推进 `pos`。
- `read` 返回 0 有两种含义：暂时需要更多输入，或已经 `finished()`；
  用 `finished()` 区分。
- 单成员模式下，输入写完但 `finished()==false` 就是截断；解码器不会单独报
  `truncated`。
- 出错后看 `error()`，用 `xeno::to_string(dec->error())` 打印原因。
  `read` 出错时 dst 中可能已有部分数据，不应使用。
- 复用：`reset()` 后可以解压下一个 gzip，不需要重新分配对象。

## 从文件解压

按“读一块压缩数据 → 立即泵出解压数据”的方式驱动。xeno::vfs 或 stdio 都适用，
关键仍是交替调用 `write` / `read`：

```cpp
std::uint8_t in[1024], out[512];
std::size_t n;
while((n = fread(in, 1, sizeof in, fp)) > 0)
{
    std::size_t off = 0;
    while(off < n)
    {
        int w = dec->write(in + off, n - off);
        if(w < 0)
            return -1;  // dec->error() 给出原因
        off += static_cast<std::size_t>(w);

        int r;
        while((r = dec->read(out, sizeof out)) > 0)
            consume(out, static_cast<std::size_t>(r));
        if(r < 0)
            return -2;
        // w == 0 说明输入环已满；上面的 read 已消费掉一部分输入，下一轮可继续写。
    }
}

// EOF 后把剩余输出读完
int r;
while((r = dec->read(out, sizeof out)) > 0)
    consume(out, static_cast<std::size_t>(r));
if(r < 0)
    return -2;
if(!dec->finished())
    return -4;  // 截断：输入已写完但未校验 trailer
```

## 公共接口

```cpp
static constexpr std::size_t input_capacity;   // = N
static constexpr std::size_t window_capacity;  // = W

int write(const void *src, std::size_t len) noexcept;
int read(void *dst, std::size_t len) noexcept;

bool      finished() const noexcept;  // trailer 校验通过
bool      failed() const noexcept;
gzip_errc error() const noexcept;
std::size_t input_size() const noexcept;   // 当前输入环中的字节数
std::size_t input_free() const noexcept;   // N - input_size()
std::uint32_t output_size() const noexcept; // 已解压字节数（ISIZE，mod 2^32）
void reset() noexcept;

constexpr std::string_view to_string(gzip_errc) noexcept;
```

返回值语义：

| 调用 | 返回值 |
|---|---|
| `write(src, len)` | 实际接收的字节数（0..len）；输入环满时 0；失败/已结束时 -1 |
| `read(dst, len)` | 本次产出的字节数；0 表示暂无输出（需要更多输入或已结束）；-1 表示错误 |
| `write(nullptr, 0)` / `read(nullptr, 0)` | 0，不改变状态 |
| 非零长度 + 空指针 | -1，`invalid_argument` |
| `read` 的 `len > INT_MAX` | 内部钳制到 `INT_MAX` |

`write` 在 `finished()` 之后调用会返回 -1（`input_closed`）并进入失败状态。
解码器非线程安全，按单生产者/单消费者使用。

## 错误码

| `gzip_errc` | 含义 |
|---|---|
| `none` | 无错误 |
| `bad_magic` | gzip ID1/ID2 不是 `1f 8b` |
| `unsupported_method` | CM != 8（非 deflate） |
| `reserved_flags` | FLG 保留位 5..7 非 0 |
| `header_crc` | FHCRC 不匹配 |
| `invalid_block_type` | DEFLATE BTYPE == 3 |
| `invalid_stored_length` | stored block 的 LEN/NLEN 不互补 |
| `invalid_huffman` | 码树过订阅/不完整，或 HLIT/HDIST 越界 |
| `invalid_symbol` | litlen 286/287、distance 30/31、重复码越界 |
| `distance_too_far` | 距离超过已产生历史或超过 W |
| `crc_mismatch` | trailer CRC32 不匹配 |
| `size_mismatch` | trailer ISIZE 不匹配 |
| `input_closed` | `finished()` 之后仍调用 `write` |
| `invalid_argument` | 非零长度 + 空指针 |

## 内存与性能

默认 `gzdecore` 的内存组成：

| 部分 | 大小 |
|---|---|
| DEFLATE 历史窗口 `W` | 32768 B |
| 压缩输入环 `N` | 4096 B |
| 哈夫曼表（9/6/7 位主 LUT + canonical 回退） | 约 2.1 KiB |
| 其他状态 | 约 200 B |
| CRC32 表 | 1 KiB rodata（不占 RAM） |

约 39 KiB。窗口和输入环是对象内的数组，构造/reset 不需要清空，内部计数保证
旧内容不可达。

实现上的取舍：

- 哈夫曼解码走主 LUT；长度超过主表位数的码回退到 canonical 逐位解码。
- 输出 CRC32 按每次 `read()` 连续产出的区间批量更新，literal/match 热路径不
  逐字节算 CRC。
- 回溯匹配逐字节复制，天然支持 `distance < length` 的重叠匹配；这是正确性
  优先的选择，长匹配没有做非重叠批量拷贝快路径。
- 输入环满时 `write` 返回 0，不会阻塞或内部循环解码。

## 限制与注意事项

- **单成员**：不支持串联 gzip 成员；第一个成员的 trailer 校验通过后
  `finished()` 为 true，后续字节被忽略。
- **截断检测**：解码器没有输入结束信号，截断流表现为 `read()` 一直返回 0。
  调用方在写完全部输入后检查 `finished()` 即可判断。
- **CRC 时机**：解压数据在 trailer 校验之前就已经交给调用方；CRC32/ISIZE
  错误只能在最后一次 `read()` 时报告。
- **非线程安全**：不要从多个线程同时调用 `write`/`read`。
- **解压炸弹**：不设输出上限，调用方通过 read 缓冲区和停止调用控制。
- **W 缩小**：`W < 32768` 时，距离超过 W 的流会报 `distance_too_far`；
  标准 gzip 文件可能合法地使用更大的距离，默认值请保持 32768。
- **严格性**：保留 FLG 位和非法 gzip magic 都会报错；FTEXT 只提示文本内容，
  被忽略；FNAME/FCOMMENT 被跳过，不暴露给调用方；FHCRC 存在时会校验。
- 对象较大，不要直接作为大局部变量。用 `std::make_unique<gzdecore>()`、
  static/全局，或把对象放在专门的静态存储区。

## 测试向量生成

给 MCU/QEMU 冒烟测试准备固定向量时，可以用下面的 Python 片段生成
stored / fixed / dynamic 以及带可选字段的 gzip：

```python
import zlib, struct

def member(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY,
           extra=b'', name=b'', comment=b'', fhcrc=False):
    co = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    body = co.compress(data) + co.flush()
    flg = (4 if extra else 0) | (8 if name else 0) | (16 if comment else 0) | (2 if fhcrc else 0)
    h = bytearray([0x1f, 0x8b, 8, flg, 0, 0, 0, 0, 0, 0xff])
    if extra:
        h += struct.pack('<H', len(extra)) + extra
    if name:
        h += name + b'\0'
    if comment:
        h += comment + b'\0'
    if fhcrc:
        h += struct.pack('<H', zlib.crc32(bytes(h)) & 0xffff)
    return bytes(h) + body + struct.pack(
        '<II', zlib.crc32(data) & 0xffffffff, len(data) & 0xffffffff)

# stored: level=0
# fixed:  strategy=zlib.Z_FIXED
# dynamic: 默认 level=6 + 可压缩数据
```

## 测试

主机端测试位于 `test/gzip_test.cc`，用 zlib 生成参考数据，覆盖 stored/fixed/
dynamic、32 KiB 距离、可选头字段、CRC/ISIZE 错误、手工构造的畸形 deflate、
缩减窗口、随机分块和 API 边界：

```bash
g++-16 -std=gnu++26 -O2 -Wall -Wextra -Wpedantic \
    -I xeno/toolkit/include test/gzip_test.cc -lz -o /tmp/gzip_test
/tmp/gzip_test
```

ARM 编译检查（不链接 zlib）：

```bash
printf '#include<xeno/gzip.h>\n#include<memory>\nvoid f(){ auto d=std::make_unique<xeno::gzdecore>(); char b[16]; d->write("",0); d->read(b,sizeof b); }\n' \
  | arm-none-eabi-g++ -std=gnu++26 -Os -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
      -fno-exceptions -fno-rtti -I xeno/toolkit/include -x c++ - -c -o /tmp/gzip_arm.o
```
