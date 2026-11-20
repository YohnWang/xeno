# xeno::basic_crc 使用说明

header-only CRC 计算组件，位于 `xeno/toolkit/include/xeno/crc.h`。底层优先使用
GCC 15+ 的 CRC 内建函数（`__builtin_crc{N}_data{M}` / `__builtin_rev_crc{N}_data{M}`），
不可用时回退到 256 项查表实现。无动态分配、无异常、无 RTTI。

支持 Rocksoft/RevEng 参数模型，覆盖 CRC-8/16/32/64；内置常用算法，也可以用
公共基类配置新算法。

## 快速开始

```cpp
#include<xeno/crc.h>

xeno::crc32_iso_hdlc crc;
crc.write(data, size);                  // 可多次、任意长度
std::uint32_t value = crc.get();        // 最终 CRC（已应用 xorout）

std::uint32_t same = xeno::crc32_iso_hdlc::compute(data, size); // 一次性
```

复用对象时调用 `reset()`：

```cpp
crc.reset();
crc.write(other, other_size);
```

## 参数模型

```cpp
template<std::unsigned_integral T, T Poly, T Init, bool RefIn, bool RefOut, T XorOut>
class basic_crc;
```

| 参数 | 含义 |
|---|---|
| `T` | 无符号整数类型，宽度为 8/16/32/64 位 |
| `Poly` | 正规形式多项式（省略最高位），如 CRC-32 用 `0x04C11DB7`，不是 `0xEDB88320` |
| `Init` | 目录中的初始寄存器值（Rocksoft/RevEng 约定） |
| `RefIn` | 输入字节是否按 LSB-first 反射 |
| `RefOut` | 输出是否反射 |
| `XorOut` | 最终异或值 |

`Poly` 必须是编译期常量：GCC 内建要求模板参数或 `constexpr` 常量，因此不支持
运行时多项式。

### Init 与反射

`RefIn=true` 时，`Init` 会先做位反转再装入右移寄存器。这是 Rocksoft 模型的
语义，也是非回文 init 算法容易算错的地方。例如：

```cpp
// CRC-16/TMS37157：目录 init=0x89EC，反射寄存器初值=0x3791
using tms37157 = xeno::basic_crc16<0x1021, 0x89EC, true, true, 0x0000>;
// CRC-16/RIELLO：目录 init=0xB2AA，反射寄存器初值=0x554D
using riello = xeno::basic_crc16<0x1021, 0xB2AA, true, true, 0x0000>;
```

内置算法的 init 都是回文（0、0xFF、0xFFFF、0xFFFFFFFF），所以 `reset()` 后的
`raw()` 与目录值相同；自定义算法不要假设这一点。

## 公共接口

```cpp
constexpr basic_crc() noexcept;
constexpr void reset() noexcept;

void write(const void *data, std::size_t size) noexcept;
template<byte_range R> void write(const R &data) noexcept;
void write(std::uint8_t byte) noexcept;
void write(std::byte byte) noexcept;

[[nodiscard]] constexpr T get() const noexcept;
[[nodiscard]] constexpr T raw() const noexcept;

template<byte_range R> [[nodiscard]] static T compute(const R &data) noexcept;
[[nodiscard]] static T compute(const void *data, std::size_t size) noexcept;
```

- `byte_range` 接受连续存储的 `std::byte` / `std::uint8_t` / `char` 等字节范围：
  `std::span`、`std::vector`、`std::array`、`std::string_view`、原生数组都可以。
  直接传字符串字面量会包含结尾 `'\0'`，文本场景请用 `std::string_view`。
- `write(nullptr, 0)` 是无操作；`size != 0` 时 `data` 必须有效。
- `get()`：若 `RefOut != RefIn` 先反转寄存器，再异或 `XorOut`。
- `raw()`：内部寄存器（未做 refout/xorout）。`RefIn=true` 时是右移/反射域的值；
  ZMODEM 的残差校验（`0xDEBB20E3`）用它。
- `write` / `compute` 不是 `constexpr`（GCC 内建不能常量求值）；构造、`reset()`
  以及空对象的 `get()` / `raw()` 可用于常量表达式。

### 不足一块的暂存

`write` 会尽量用最宽的内建形式一次处理一整块：CRC-8 用 `data8`，CRC-16 用
`data16`，CRC-32 用 `data32`。不足一块的尾部（CRC-32 最多 3 字节）先暂存在对象
内；`get()` / `raw()` 时在寄存器副本上用 `data16`/`data8` 补算，不修改流式状态。
因此：

- 多次取结果、取完继续 `write` 都是正确的；
- 对象大小会包含最多 `sizeof(T)` 字节的暂存区（见下表）；
- `get()` / `raw()` 在存在暂存字节时有少量额外计算。

## 内置算法

`get()` 对 `"123456789"` 的结果：

| 类型 | Width | Poly | Init | RefIn | RefOut | XorOut | Check |
|---|---:|---:|---:|:---:|:---:|---:|---:|
| `crc8_atm` | 8 | 0x07 | 0x00 | false | false | 0x00 | 0xF4 |
| `crc8_maxim` | 8 | 0x31 | 0x00 | true | true | 0x00 | 0xA1 |
| `crc8_rohc` | 8 | 0x07 | 0xFF | true | true | 0x00 | 0xD0 |
| `crc16_xmodem` | 16 | 0x1021 | 0x0000 | false | false | 0x0000 | 0x31C3 |
| `crc16_ccitt_false` | 16 | 0x1021 | 0xFFFF | false | false | 0x0000 | 0x29B1 |
| `crc16_arc` | 16 | 0x8005 | 0x0000 | true | true | 0x0000 | 0xBB3D |
| `crc16_modbus` | 16 | 0x8005 | 0xFFFF | true | true | 0x0000 | 0x4B37 |
| `crc32_iso_hdlc` | 32 | 0x04C11DB7 | 0xFFFFFFFF | true | true | 0xFFFFFFFF | 0xCBF43926 |
| `crc32_mpeg_2` | 32 | 0x04C11DB7 | 0xFFFFFFFF | false | false | 0x00000000 | 0x0376E6E7 |
| `crc32_bzip2` | 32 | 0x04C11DB7 | 0xFFFFFFFF | false | false | 0xFFFFFFFF | 0xFC891918 |
| `crc32c` | 32 | 0x1EDC6F41 | 0xFFFFFFFF | true | true | 0xFFFFFFFF | 0xE3069283 |

每个算法都是从对应位宽公共基类派生出的独立类：

```cpp
class crc16_xmodem final
    : public basic_crc16<0x1021, 0x0000, false, false, 0x0000> {};
```

## 自定义算法

按位宽使用 `basic_crc8/16/32`（64 位直接用 `basic_crc<std::uint64_t, ...>`）：

```cpp
// CRC-16/RIELLO，目录 init=0xB2AA；内部反射后的寄存器初值是 0x554D。
using crc16_riello = xeno::basic_crc16<0x1021, 0xB2AA, true, true, 0x0000>;

// compute 不是 constexpr；check 值见上表。
std::uint16_t check =
    crc16_riello::compute(std::string_view{"123456789"}); // 0x63D0
```

需要给算法加常量或成员时，从公共基类派生：

```cpp
class my_crc final
    : public xeno::basic_crc16<0x1021, 0xFFFF, false, false, 0x0000>
{
public:
    static constexpr std::uint16_t check = 0x29B1;
};
```

## GCC 内建与回退

头文件通过 `__has_builtin` 检测 `__builtin_crc{N}_data{M}` 与
`__builtin_rev_crc{N}_data{M}` 是否齐全：

- 支持时：`XENO_CRC_USE_BUILTIN == 1`，`write` 用最宽内建块处理。
- 不支持时：自动回退到 256 项查表实现。
- 定义 `XENO_CRC_NO_BUILTIN=1` 可强制回退（测试或省空间时使用）。该宏必须在
  所有翻译单元中保持一致，否则不同 TU 的 `write` 定义不同，属于 ODR 违规。

两条路径语义完全一致，测试用同一组向量覆盖。

### Cortex-M7 上的代码大小

Cortex-M7 没有这些内建可用的 CRC 指令，`-Os` 下 GCC 会把内建展开为 256 项查表：
每个（位宽、Poly、方向）一份，CRC-8 256 B、CRC-16 512 B、CRC-32 1 KiB。回退实现
的 rodata 占用基本相同，因此强制回退不是省 flash 的手段；避免实例化不用的算法
才有效。相同 width/poly/方向的算法共享同一张表（例如 XMODEM 与 CCITT-FALSE、
ARC 与 MODBUS、MPEG-2 与 BZIP2）。

对象大小（ARM Cortex-M7，含暂存区）：

| 类型 | `sizeof` |
|---|---:|
| `crc8_*` | 3 B |
| `crc16_*` | 6 B |
| `crc32_*` | 12 B |

## toolkit 中的使用

- `gzip.h`：CRC-32/ISO-HDLC；FHCRC 用 `header_crc_.get() & 0xFFFF`，trailer 用
  `crc_.get()`。
- `ymodem.h`：`crc16_xmodem::compute(payload)` 校验帧数据。
- `zmodem.h`：CRC-16/XMODEM 与 CRC-32/ISO-HDLC；接收侧残差校验用 `raw()`，
  分别与 `0` 和 `0xDEBB20E3` 比较，不能用会应用 xorout 的 `get()`。

## 限制

- 不提供运行时多项式；`Poly` 必须是模板参数。
- 本版本不提供 CRC-64 算法类型，但 `basic_crc<std::uint64_t, ...>` 可用。
- `raw()` 不是线路上的标准 CRC（对 `RefIn=true` 尤其如此）；常规场景用 `get()`。
- `get()` / `raw()` 的常量求值仅适用于没有暂存字节的对象。

## 验证

```bash
# 主机端，内建与回退两条路径
g++-16 -std=gnu++26 -O2 -Wall -Wextra -Wpedantic \
    -I xeno/toolkit/include test/crc_test.cc -o /tmp/crc_test
/tmp/crc_test

g++-16 -std=gnu++26 -O2 -Wall -Wextra -Wpedantic -DXENO_CRC_NO_BUILTIN=1 \
    -I xeno/toolkit/include test/crc_test.cc -o /tmp/crc_test_fallback
/tmp/crc_test_fallback

# gzip 与 zlib 对拍
g++-16 -std=gnu++26 -O2 -I xeno/toolkit/include test/gzip_test.cc -lz \
    -o /tmp/gzip_test && /tmp/gzip_test
```

已知检查值、分块一致性、残差、非回文 init 以及内建/回退对比都在
`test/crc_test.cc` 中。
