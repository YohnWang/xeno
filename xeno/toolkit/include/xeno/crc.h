#pragma once

#include<array>
#include<concepts>
#include<cstddef>
#include<cstdint>
#include<cstring>
#include<limits>
#include<ranges>

// GCC 15+ 提供 __builtin_crc{N}_data{M} / __builtin_rev_crc{N}_data{M}。
// 定义 XENO_CRC_NO_BUILTIN=1 可强制使用查表回退实现；该宏必须在所有
// 翻译单元中保持一致。
#ifndef XENO_CRC_NO_BUILTIN
#  define XENO_CRC_NO_BUILTIN 0
#endif

#if !XENO_CRC_NO_BUILTIN && defined(__has_builtin)
#  if __has_builtin(__builtin_crc8_data8) \
   && __has_builtin(__builtin_rev_crc8_data8) \
   && __has_builtin(__builtin_crc16_data8) \
   && __has_builtin(__builtin_crc16_data16) \
   && __has_builtin(__builtin_rev_crc16_data8) \
   && __has_builtin(__builtin_rev_crc16_data16) \
   && __has_builtin(__builtin_crc32_data8) \
   && __has_builtin(__builtin_crc32_data16) \
   && __has_builtin(__builtin_crc32_data32) \
   && __has_builtin(__builtin_rev_crc32_data8) \
   && __has_builtin(__builtin_rev_crc32_data16) \
   && __has_builtin(__builtin_rev_crc32_data32) \
   && __has_builtin(__builtin_crc64_data8) \
   && __has_builtin(__builtin_crc64_data16) \
   && __has_builtin(__builtin_crc64_data32) \
   && __has_builtin(__builtin_crc64_data64) \
   && __has_builtin(__builtin_rev_crc64_data8) \
   && __has_builtin(__builtin_rev_crc64_data16) \
   && __has_builtin(__builtin_rev_crc64_data32) \
   && __has_builtin(__builtin_rev_crc64_data64)
#    define XENO_CRC_USE_BUILTIN 1
#  endif
#endif

#ifndef XENO_CRC_USE_BUILTIN
#  define XENO_CRC_USE_BUILTIN 0
#endif

namespace xeno
{

namespace crc_detail
{

template<std::unsigned_integral T>
[[nodiscard]] constexpr T bit_reverse(T value) noexcept
{
    T result = 0;
    constexpr std::size_t bits = std::numeric_limits<T>::digits;
    for(std::size_t i = 0; i < bits; ++i)
    {
        result = static_cast<T>((result << 1) | (value & T{1}));
        value = static_cast<T>(value >> 1);
    }
    return result;
}

// 只接受字节型、连续存储的范围，避免不同 span 重载之间的二义性。
template<class R>
concept byte_range = std::ranges::contiguous_range<R>
    && (std::same_as<std::ranges::range_value_t<R>, std::byte>
        || std::same_as<std::ranges::range_value_t<R>, std::uint8_t>
        || std::same_as<std::ranges::range_value_t<R>, char>
        || std::same_as<std::ranges::range_value_t<R>, signed char>
        || std::same_as<std::ranges::range_value_t<R>, unsigned char>);

// 把 W 个字节组装成内建函数一次消费的数据字。
// RefIn=true 时首字节在最低字节（LSB-first），否则在最高字节（MSB-first）。
template<std::size_t W, std::unsigned_integral T, bool RefIn>
[[nodiscard]] constexpr T gather(const std::uint8_t *p) noexcept
{
    T word = 0;
    if constexpr(RefIn)
    {
        for(std::size_t i = 0; i < W; ++i)
            word = static_cast<T>(word | static_cast<T>(static_cast<T>(p[i]) << (8 * i)));
    }
    else
    {
        for(std::size_t i = 0; i < W; ++i)
            word = static_cast<T>((word << 8) | p[i]);
    }
    return word;
}

#if XENO_CRC_USE_BUILTIN

template<std::unsigned_integral T, T Poly, bool RefIn>
struct crc_engine
{
    template<std::size_t W>
    [[nodiscard]] static T update_bytes(T crc, const std::uint8_t *p) noexcept
    {
        static_assert(W == 1 || W == 2 || W == 4 || W == 8,
                      "crc_engine: chunk width must be 1/2/4/8 bytes");
        static_assert(W <= sizeof(T), "crc_engine: chunk wider than CRC");

        const T word = gather<W, T, RefIn>(p);
        if constexpr(std::same_as<T, std::uint8_t>)
        {
            static_assert(W == 1);
            if constexpr(RefIn)
                return __builtin_rev_crc8_data8(crc, static_cast<std::uint8_t>(word), Poly);
            else
                return __builtin_crc8_data8(crc, static_cast<std::uint8_t>(word), Poly);
        }
        else if constexpr(std::same_as<T, std::uint16_t>)
        {
            if constexpr(W == 1)
            {
                if constexpr(RefIn)
                    return __builtin_rev_crc16_data8(crc, static_cast<std::uint8_t>(word), Poly);
                else
                    return __builtin_crc16_data8(crc, static_cast<std::uint8_t>(word), Poly);
            }
            else
            {
                static_assert(W == 2);
                if constexpr(RefIn)
                    return __builtin_rev_crc16_data16(crc, static_cast<std::uint16_t>(word), Poly);
                else
                    return __builtin_crc16_data16(crc, static_cast<std::uint16_t>(word), Poly);
            }
        }
        else if constexpr(std::same_as<T, std::uint32_t>)
        {
            if constexpr(W == 1)
            {
                if constexpr(RefIn)
                    return __builtin_rev_crc32_data8(crc, static_cast<std::uint8_t>(word), Poly);
                else
                    return __builtin_crc32_data8(crc, static_cast<std::uint8_t>(word), Poly);
            }
            else if constexpr(W == 2)
            {
                if constexpr(RefIn)
                    return __builtin_rev_crc32_data16(crc, static_cast<std::uint16_t>(word), Poly);
                else
                    return __builtin_crc32_data16(crc, static_cast<std::uint16_t>(word), Poly);
            }
            else
            {
                static_assert(W == 4);
                if constexpr(RefIn)
                    return __builtin_rev_crc32_data32(crc, static_cast<std::uint32_t>(word), Poly);
                else
                    return __builtin_crc32_data32(crc, static_cast<std::uint32_t>(word), Poly);
            }
        }
        else
        {
            static_assert(std::same_as<T, std::uint64_t>);
            if constexpr(W == 1)
            {
                if constexpr(RefIn)
                    return __builtin_rev_crc64_data8(crc, static_cast<std::uint8_t>(word), Poly);
                else
                    return __builtin_crc64_data8(crc, static_cast<std::uint8_t>(word), Poly);
            }
            else if constexpr(W == 2)
            {
                if constexpr(RefIn)
                    return __builtin_rev_crc64_data16(crc, static_cast<std::uint16_t>(word), Poly);
                else
                    return __builtin_crc64_data16(crc, static_cast<std::uint16_t>(word), Poly);
            }
            else if constexpr(W == 4)
            {
                if constexpr(RefIn)
                    return __builtin_rev_crc64_data32(crc, static_cast<std::uint32_t>(word), Poly);
                else
                    return __builtin_crc64_data32(crc, static_cast<std::uint32_t>(word), Poly);
            }
            else
            {
                static_assert(W == 8);
                if constexpr(RefIn)
                    return __builtin_rev_crc64_data64(crc, static_cast<std::uint64_t>(word), Poly);
                else
                    return __builtin_crc64_data64(crc, static_cast<std::uint64_t>(word), Poly);
            }
        }
    }
};

#else

template<std::unsigned_integral T, T Poly, bool RefIn>
struct crc_engine
{
    template<std::size_t W>
    [[nodiscard]] static constexpr T update_bytes(T crc, const std::uint8_t *p) noexcept
    {
        for(std::size_t i = 0; i < W; ++i)
            crc = update_byte(crc, p[i]);
        return crc;
    }

private:
    static constexpr std::size_t width = std::numeric_limits<T>::digits;
    static constexpr T msb = static_cast<T>(T{1} << (width - 1));
    static constexpr T reflected_poly = bit_reverse(Poly);

    static constexpr std::array<T, 256> make_table() noexcept
    {
        std::array<T, 256> table{};
        for(std::size_t i = 0; i < table.size(); ++i)
        {
            T c;
            if constexpr(RefIn)
            {
                c = static_cast<T>(i);
                for(int bit = 0; bit < 8; ++bit)
                    c = (c & T{1}) ? static_cast<T>((c >> 1) ^ reflected_poly)
                                   : static_cast<T>(c >> 1);
            }
            else
            {
                c = static_cast<T>(static_cast<T>(i) << (width - 8));
                for(int bit = 0; bit < 8; ++bit)
                    c = (c & msb) ? static_cast<T>((c << 1) ^ Poly)
                                  : static_cast<T>(c << 1);
            }
            table[i] = c;
        }
        return table;
    }

    inline static constexpr std::array<T, 256> table = make_table();

    [[nodiscard]] static constexpr T update_byte(T crc, std::uint8_t byte) noexcept
    {
        if constexpr(RefIn)
            return static_cast<T>((crc >> 8)
                ^ table[static_cast<std::uint8_t>(crc ^ byte)]);
        else
            return static_cast<T>((crc << 8)
                ^ table[static_cast<std::uint8_t>((crc >> (width - 8)) ^ byte)]);
    }
};

#endif

} // namespace crc_detail

// Rocksoft/RevEng 参数模型。Poly 使用正规形式（如 CRC-32 为 0x04C11DB7）；
// Init 使用目录中的 init 值，RefIn=true 时内部会位反转后装入寄存器。
template<std::unsigned_integral T, T Poly, T Init, bool RefIn, bool RefOut, T XorOut>
class basic_crc
{
    static_assert(std::numeric_limits<T>::digits == 8
                  || std::numeric_limits<T>::digits == 16
                  || std::numeric_limits<T>::digits == 32
                  || std::numeric_limits<T>::digits == 64,
                  "basic_crc: T must be an 8/16/32/64-bit unsigned integer");
    static_assert(Poly != T{0}, "basic_crc: polynomial must not be zero");

public:
    using value_type = T;

    static constexpr std::size_t width = std::numeric_limits<T>::digits;
    static constexpr T polynomial = Poly;
    static constexpr T initial_value = Init;
    static constexpr bool reflect_in = RefIn;
    static constexpr bool reflect_out = RefOut;
    static constexpr T xor_out = XorOut;

    constexpr basic_crc() noexcept = default;

    constexpr void reset() noexcept
    {
        reg_ = initial_register;
        pending_count_ = 0;
    }

    void write(const void *data, std::size_t size) noexcept
    {
        if(size == 0)
            return;
        const auto *p = static_cast<const std::uint8_t *>(data);

        if constexpr(sizeof(T) > 1)
        {
            constexpr std::size_t chunk = sizeof(T);

            if(pending_count_ != 0)
            {
                const std::size_t need = chunk - pending_count_;
                const std::size_t take = size < need ? size : need;
                std::memcpy(pending_.data() + pending_count_, p, take);
                pending_count_ = static_cast<std::uint8_t>(pending_count_ + take);
                p += take;
                size -= take;
                if(pending_count_ == chunk)
                {
                    reg_ = engine::template update_bytes<chunk>(reg_, pending_.data());
                    pending_count_ = 0;
                }
            }

            while(size >= chunk)
            {
                reg_ = engine::template update_bytes<chunk>(reg_, p);
                p += chunk;
                size -= chunk;
            }

            if(size != 0)
            {
                std::memcpy(pending_.data(), p, size);
                pending_count_ = static_cast<std::uint8_t>(size);
            }
        }
        else
        {
            while(size-- > 0)
                reg_ = engine::template update_bytes<1>(reg_, p++);
        }
    }

    template<crc_detail::byte_range R>
    void write(const R &data) noexcept
    {
        write(static_cast<const void *>(std::ranges::data(data)),
              static_cast<std::size_t>(std::ranges::size(data)));
    }

    void write(std::uint8_t byte) noexcept
    {
        write(&byte, 1);
    }

    void write(std::byte byte) noexcept
    {
        const std::uint8_t value = std::to_integer<std::uint8_t>(byte);
        write(&value, 1);
    }

    // 内部寄存器（含暂存尾部的补算结果，但不做 refout/xorout 终结）。
    [[nodiscard]] constexpr T raw() const noexcept
    {
        T value = reg_;
        if constexpr(sizeof(T) > 1)
        {
            if(pending_count_ != 0)
            {
                std::size_t pos = 0;
                std::size_t left = pending_count_;
                if constexpr(sizeof(T) >= 4)
                {
                    if(left >= 4)
                    {
                        value = engine::template update_bytes<4>(value, pending_.data());
                        pos += 4;
                        left -= 4;
                    }
                }
                if constexpr(sizeof(T) >= 2)
                {
                    if(left >= 2)
                    {
                        value = engine::template update_bytes<2>(value,
                                                                 pending_.data() + pos);
                        pos += 2;
                        left -= 2;
                    }
                }
                if(left != 0)
                    value = engine::template update_bytes<1>(value,
                                                             pending_.data() + pos);
            }
        }
        return value;
    }

    [[nodiscard]] constexpr T get() const noexcept
    {
        T value = raw();
        if constexpr(RefOut != RefIn)
            value = crc_detail::bit_reverse(value);
        return static_cast<T>(value ^ XorOut);
    }

    template<crc_detail::byte_range R>
    [[nodiscard]] static T compute(const R &data) noexcept
    {
        basic_crc crc;
        crc.write(data);
        return crc.get();
    }

    [[nodiscard]] static T compute(const void *data, std::size_t size) noexcept
    {
        basic_crc crc;
        crc.write(data, size);
        return crc.get();
    }

private:
    static constexpr T initial_register_value() noexcept
    {
        if constexpr(RefIn)
            return crc_detail::bit_reverse(Init);
        else
            return Init;
    }

    using engine = crc_detail::crc_engine<T, Poly, RefIn>;
    static constexpr T initial_register = initial_register_value();

    T reg_ = initial_register;
    std::uint8_t pending_count_ = 0;
    // 暂存不满一块的输入；容量取一整块，填满即处理。
    std::array<std::uint8_t, sizeof(T)> pending_{};
};

template<std::uint8_t Poly, std::uint8_t Init, bool RefIn, bool RefOut, std::uint8_t XorOut>
using basic_crc8 = basic_crc<std::uint8_t, Poly, Init, RefIn, RefOut, XorOut>;

template<std::uint16_t Poly, std::uint16_t Init, bool RefIn, bool RefOut, std::uint16_t XorOut>
using basic_crc16 = basic_crc<std::uint16_t, Poly, Init, RefIn, RefOut, XorOut>;

template<std::uint32_t Poly, std::uint32_t Init, bool RefIn, bool RefOut, std::uint32_t XorOut>
using basic_crc32 = basic_crc<std::uint32_t, Poly, Init, RefIn, RefOut, XorOut>;

class crc8_atm final
    : public basic_crc8<0x07, 0x00, false, false, 0x00> {};

class crc8_maxim final
    : public basic_crc8<0x31, 0x00, true, true, 0x00> {};

class crc8_rohc final
    : public basic_crc8<0x07, 0xFF, true, true, 0x00> {};

class crc16_xmodem final
    : public basic_crc16<0x1021, 0x0000, false, false, 0x0000> {};

class crc16_ccitt_false final
    : public basic_crc16<0x1021, 0xFFFF, false, false, 0x0000> {};

class crc16_arc final
    : public basic_crc16<0x8005, 0x0000, true, true, 0x0000> {};

class crc16_modbus final
    : public basic_crc16<0x8005, 0xFFFF, true, true, 0x0000> {};

class crc32_iso_hdlc final
    : public basic_crc32<0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF> {};

class crc32_mpeg_2 final
    : public basic_crc32<0x04C11DB7, 0xFFFFFFFF, false, false, 0x00000000> {};

class crc32_bzip2 final
    : public basic_crc32<0x04C11DB7, 0xFFFFFFFF, false, false, 0xFFFFFFFF> {};

class crc32c final
    : public basic_crc32<0x1EDC6F41, 0xFFFFFFFF, true, true, 0xFFFFFFFF> {};

} // namespace xeno
