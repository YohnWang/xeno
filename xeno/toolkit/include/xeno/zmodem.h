#pragma once

#include<algorithm>
#include<array>
#include<cstddef>
#include<cstdint>
#include<exception>
#include<limits>
#include<span>
#include<string>

namespace xeno
{

// ZMODEM 接收端（单文件）。
//
// IO 由派生类提供：
//   int  try_getchar(); // 非阻塞，返回 0..255，负数表示暂无数据
//   void putchar(int);  // 发送单字节
// 通过 deducing this + 继承在编译期绑定，不检查返回值以外的 IO 状态。
// 超时与重试由调用方计时，类内不做等待。
//
// 所有文件内容都按原始二进制交付，不做 CR/LF 或字符集转换。
class zmodem
{
public:
    static constexpr std::size_t kUnknown = std::numeric_limits<std::size_t>::max();

    struct options
    {
        // 从文件的这个偏移开始接收；0 表示从头重传。
        // 非 0 时只发 ZRPOS，不做 ZCRC 校验，调用方需保证已有前缀一致。
        std::size_t resume_offset = 0;

        // ZRINIT 通告的接收缓冲大小 = window * 1024，限制在 1..8。
        // ZMODEM 真正的发送窗口由发送端控制，这里只影响它可用的块大小。
        std::size_t window = 1;
    };

    // 传输非正常结束。start_link()/receive()/finish_batch() 在终止状态下抛出。
    struct error : std::exception
    {
        const char *what() const noexcept override
        {
            return "zmodem: transfer error";
        }
    };

    // 本地 cancel()、远端 CAN/ZABORT，或中止后再次调用。
    struct cancelled : error
    {
        const char *what() const noexcept override
        {
            return "zmodem: transfer cancelled";
        }
    };

    // 协议错误、不支持的命令、提前结束等。
    struct failed : error
    {
        const char *what() const noexcept override
        {
            return "zmodem: transfer failed";
        }
    };

    std::string filename() const
    {
        return filename_;
    }

    std::size_t filesize() const
    {
        return filesize_;
    }

    // ---- 建立连接 ----
    // true 表示已收到 ZFILE 并发出 ZRPOS；false 表示继续轮询。
    bool start_link(this auto &&self, const options &opt = {})
    {
        auto get = [&]() -> int
        {
            if(self.pushback_ >= 0)
            {
                int c = self.pushback_;
                self.pushback_ = -1;
                return c;
            }
            return self.try_getchar();
        };
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };
        return self.start_link_impl(get, put, opt);
    }

    // ---- 接收当前文件 ----
    // 返回 0 表示 ZEOF 且文件结束；kUnknown 表示继续；
    // 其余为剩余待交付字节数（filesize 已知时）。
    std::size_t receive(this auto &&self, auto &&f)
    {
        auto get = [&]() -> int
        {
            if(self.pushback_ >= 0)
            {
                int c = self.pushback_;
                self.pushback_ = -1;
                return c;
            }
            return self.try_getchar();
        };
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };
        return self.receive_impl(get, put, f);
    }

    // ---- 结束 ZMODEM 批处理 ----
    // receive() 返回 0 后调用，非阻塞轮询：
    // 发 ZRINIT，后续文件用 ZSKIP 跳过，收到 ZFIN 并回 ZFIN 后返回 true。
    bool finish_batch(this auto &&self)
    {
        auto get = [&]() -> int
        {
            if(self.pushback_ >= 0)
            {
                int c = self.pushback_;
                self.pushback_ = -1;
                return c;
            }
            return self.try_getchar();
        };
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };
        return self.finish_batch_impl(get, put);
    }

    // ---- 本地用户按 Ctrl+C 时调用：发 CAN 串并抛 cancelled ----
    // idle/done 状态下是空操作，不抛异常。
    void cancel(this auto &&self)
    {
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };
        self.cancel_impl(put);
    }

    void reset()
    {
        ph_ = phase::idle;
        end_ = end_state::none;
        sub_ = sub_purpose::none;
        opt_ = options{};
        filename_.clear();
        filesize_ = kUnknown;
        received_ = 0;
        batch_started_ = false;
        data_active_ = false;
        discard_ = false;
        reset_data();
        reset_parser();
    }

private:
    static constexpr std::uint8_t ZPAD = 0x2a;
    static constexpr std::uint8_t ZDLE = 0x18;
    static constexpr std::uint8_t ZBIN = 0x41;   // 'A'
    static constexpr std::uint8_t ZHEX = 0x42;   // 'B'
    static constexpr std::uint8_t ZBIN32 = 0x43; // 'C'
    static constexpr std::uint8_t CAN = 0x18;
    static constexpr std::uint8_t XON = 0x11;
    static constexpr std::uint8_t XOFF = 0x13;
    static constexpr std::uint8_t ZCRCE = 0x68; // 'h'
    static constexpr std::uint8_t ZCRCG = 0x69; // 'i'
    static constexpr std::uint8_t ZCRCQ = 0x6a; // 'j'
    static constexpr std::uint8_t ZCRCW = 0x6b; // 'k'
    static constexpr std::uint8_t ZRUB0 = 0x6c; // 'l'
    static constexpr std::uint8_t ZRUB1 = 0x6d; // 'm'

    // 帧类型
    static constexpr std::uint8_t ZRQINIT = 0;
    static constexpr std::uint8_t ZRINIT = 1;
    static constexpr std::uint8_t ZSINIT = 2;
    static constexpr std::uint8_t ZACK = 3;
    static constexpr std::uint8_t ZFILE = 4;
    static constexpr std::uint8_t ZSKIP = 5;
    static constexpr std::uint8_t ZNAK = 6;
    static constexpr std::uint8_t ZABORT = 7;
    static constexpr std::uint8_t ZFIN = 8;
    static constexpr std::uint8_t ZRPOS = 9;
    static constexpr std::uint8_t ZDATA = 10;
    static constexpr std::uint8_t ZEOF = 11;
    static constexpr std::uint8_t ZFERR = 12;
    static constexpr std::uint8_t ZCRC = 13;
    static constexpr std::uint8_t ZCHALLENGE = 14;
    static constexpr std::uint8_t ZCOMPL = 15;
    static constexpr std::uint8_t ZCAN = 16;
    static constexpr std::uint8_t ZFREECNT = 17;
    static constexpr std::uint8_t ZCOMMAND = 18;
    static constexpr std::uint8_t ZSTDERR = 19;

    // ZRINIT 能力位
    static constexpr std::uint8_t CANFDX = 0x01;
    static constexpr std::uint8_t CANOVIO = 0x02;
    static constexpr std::uint8_t CANFC32 = 0x20;
    static constexpr std::uint8_t ESCCTL = 0x40;

    // 头信息数组下标：线上顺序 info[0..3] 是低位到高位
    static constexpr std::size_t ZF0 = 3, ZF1 = 2, ZF2 = 1, ZF3 = 0;
    static constexpr std::size_t ZP0 = 0, ZP1 = 1, ZP2 = 2, ZP3 = 3;

    static constexpr std::size_t MAX_BLOCK = 8192;
    static constexpr std::size_t HEADER_BYTES = 1 + 4; // type + info
    static constexpr int CAN_BURST = 10;
    static constexpr std::uint32_t CRC32_RESIDUE = 0xDEBB20E3u;

    enum class phase
    {
        idle,
        wait_file,
        data,
        file_done,
        done
    };

    enum class end_state
    {
        none,
        cancelled,
        failed
    };

    enum class sub_purpose
    {
        none,
        sinit,
        zfile,
        skip,
        discard
    };

    enum class hdr_state
    {
        search,
        pad,
        kind,
        bin,
        hex,
        hex_end
    };

    enum class esc_state
    {
        normal,
        after_zdle
    };

    enum class byte_status
    {
        pending,
        ok,
        frame_end,
        cancelled,
        error
    };

    enum class hdr_status
    {
        pending,
        ok,
        cancelled,
        error
    };

    enum class data_status
    {
        pending,
        ok,
        cancelled,
        error
    };

    struct header
    {
        std::uint8_t type = 0;
        std::uint8_t info[4]{};
        bool crc32 = false;
    };

    phase ph_ = phase::idle;
    end_state end_ = end_state::none;
    sub_purpose sub_ = sub_purpose::none;
    options opt_{};

    std::string filename_;
    std::size_t filesize_ = kUnknown;
    std::size_t received_ = 0;
    bool batch_started_ = false;

    std::array<std::byte, MAX_BLOCK + 1> buf_{};
    std::size_t data_len_ = 0;
    bool data_active_ = false;
    bool data_32_ = false;
    bool data_in_crc_ = false;
    bool discard_ = false;
    std::uint8_t data_end_ = 0;
    std::uint16_t data_crc16_ = 0;
    std::uint32_t data_crc32_ = 0xFFFFFFFFu;
    std::size_t data_crc_pos_ = 0;

    hdr_state hs_ = hdr_state::search;
    esc_state es_ = esc_state::normal;
    bool hdr_32_ = false;
    std::uint8_t hdr_buf_[HEADER_BYTES + 4]{};
    std::size_t hdr_pos_ = 0;
    std::uint16_t hdr_crc16_ = 0;
    std::uint32_t hdr_crc32_ = 0xFFFFFFFFu;
    int hex_nibble_ = -1;
    int can_count_ = 0;
    bool hex_lf_ = false;
    int pushback_ = -1;

    void throw_if_ended() const
    {
        if(end_ == end_state::cancelled)
            throw cancelled{};
        if(end_ == end_state::failed)
            throw failed{};
    }

    static std::uint16_t crc16_byte(std::uint16_t crc, std::uint8_t c)
    {
        crc ^= static_cast<std::uint16_t>(c) << 8;
        for(int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<std::uint16_t>(crc << 1);
        return crc;
    }

    static std::uint32_t crc32_byte(std::uint32_t crc, std::uint8_t c)
    {
        crc ^= c;
        for(int i = 0; i < 8; ++i)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        return crc;
    }

    static std::size_t position(const header &h)
    {
        return static_cast<std::size_t>(h.info[0]) |
               (static_cast<std::size_t>(h.info[1]) << 8) |
               (static_cast<std::size_t>(h.info[2]) << 16) |
               (static_cast<std::size_t>(h.info[3]) << 24);
    }

    static void set_position(std::uint8_t info[4], std::size_t pos)
    {
        info[ZP0] = static_cast<std::uint8_t>(pos);
        info[ZP1] = static_cast<std::uint8_t>(pos >> 8);
        info[ZP2] = static_cast<std::uint8_t>(pos >> 16);
        info[ZP3] = static_cast<std::uint8_t>(pos >> 24);
    }

    static int hex_value(std::uint8_t c)
    {
        if(c >= '0' && c <= '9')
            return c - '0';
        if(c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if(c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    static void put_hex_digit(auto &&put, std::uint8_t v)
    {
        static constexpr char digits[] = "0123456789abcdef";
        put(static_cast<std::uint8_t>(digits[(v >> 4) & 0x0F]));
        put(static_cast<std::uint8_t>(digits[v & 0x0F]));
    }

    // 发送 hex 头。type + info 用 CRC16，格式与 lrzsz 的 zshhdr 一致。
    static void send_hex_header(auto &&put, std::uint8_t type, const std::uint8_t info[4])
    {
        std::uint16_t crc = 0;
        put(ZPAD);
        put(ZPAD);
        put(ZDLE);
        put(ZHEX);

        type &= 0x7F;
        put_hex_digit(put, type);
        crc = crc16_byte(crc, type);
        for(int i = 0; i < 4; ++i)
        {
            put_hex_digit(put, info[i]);
            crc = crc16_byte(crc, info[i]);
        }
        put_hex_digit(put, static_cast<std::uint8_t>(crc >> 8));
        put_hex_digit(put, static_cast<std::uint8_t>(crc));

        put('\r');
        put('\n');
        if(type != ZFIN && type != ZACK)
            put(XON);
    }

    static void send_position_header(auto &&put, std::uint8_t type, std::size_t pos)
    {
        std::uint8_t info[4]{};
        set_position(info, pos);
        send_hex_header(put, type, info);
    }

    void send_zrinit(auto &&put) const
    {
        std::size_t blocks = opt_.window == 0 ? 1 : opt_.window;
        blocks = std::min<std::size_t>(blocks, MAX_BLOCK / 1024);
        std::uint16_t bufsize = static_cast<std::uint16_t>(blocks * 1024);

        std::uint8_t info[4]{};
        info[ZP0] = static_cast<std::uint8_t>(bufsize);
        info[ZP1] = static_cast<std::uint8_t>(bufsize >> 8);
        info[ZF1] = 0;
        info[ZF0] = CANFDX | CANOVIO | CANFC32 | ESCCTL;
        send_hex_header(put, ZRINIT, info);
    }

    static void send_cancel(auto &&put)
    {
        for(int i = 0; i < CAN_BURST; ++i)
            put(CAN);
        for(int i = 0; i < CAN_BURST; ++i)
            put(0x08);
    }

    // 读取一个经过 ZDLE 转义的逻辑字节。
    byte_status read_escaped(auto &&get, std::uint8_t &out)
    {
        for(;;)
        {
            int c = get();
            if(c < 0)
                return byte_status::pending;
            std::uint8_t b = static_cast<std::uint8_t>(c);

            if(es_ == esc_state::normal)
            {
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZDLE)
                {
                    es_ = esc_state::after_zdle;
                    continue;
                }
                out = b;
                return byte_status::ok;
            }

            // after_zdle
            if(b == XON || b == XOFF)
                continue; // 流控字符，ZDLE 仍在等待
            es_ = esc_state::normal;
            if(b == CAN)
                return byte_status::cancelled;
            if(b == ZRUB0)
            {
                out = 0x7F;
                return byte_status::ok;
            }
            if(b == ZRUB1)
            {
                out = 0xFF;
                return byte_status::ok;
            }
            if(b == ZCRCE || b == ZCRCG || b == ZCRCQ || b == ZCRCW)
            {
                out = b;
                return byte_status::frame_end;
            }
            out = static_cast<std::uint8_t>(b ^ 0x40);
            return byte_status::ok;
        }
    }

    void start_header(bool crc32)
    {
        hdr_32_ = crc32;
        hdr_pos_ = 0;
        hdr_crc16_ = 0;
        hdr_crc32_ = 0xFFFFFFFFu;
        hex_nibble_ = -1;
    }

    void header_append(std::uint8_t c)
    {
        if(hdr_pos_ < sizeof(hdr_buf_))
            hdr_buf_[hdr_pos_++] = c;
        if(hdr_32_)
            hdr_crc32_ = crc32_byte(hdr_crc32_, c);
        else
            hdr_crc16_ = crc16_byte(hdr_crc16_, c);
    }

    void commit_header(header &h)
    {
        h.type = static_cast<std::uint8_t>(hdr_buf_[0] & 0x7F);
        for(int i = 0; i < 4; ++i)
            h.info[i] = hdr_buf_[1 + i];
        h.crc32 = hdr_32_;
        hs_ = hdr_state::search;
        can_count_ = 0;
        hex_lf_ = false;
    }

    hdr_status finish_header(header &h)
    {
        bool ok;
        if(hdr_32_)
            ok = hdr_crc32_ == CRC32_RESIDUE;
        else
            ok = hdr_crc16_ == 0;
        if(!ok)
        {
            hs_ = hdr_state::search;
            return hdr_status::error;
        }
        commit_header(h);
        return hdr_status::ok;
    }

    hdr_status poll_header(auto &&get, header &h)
    {
        for(;;)
        {
            switch(hs_)
            {
            case hdr_state::search:
            {
                int c = get();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == CAN)
                {
                    if(++can_count_ >= 5)
                        return hdr_status::cancelled;
                    continue;
                }
                can_count_ = 0;
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZPAD || b == static_cast<std::uint8_t>(ZPAD | 0x80))
                {
                    hs_ = hdr_state::pad;
                    continue;
                }
                continue;
            }
            case hdr_state::pad:
            {
                int c = get();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZPAD || b == static_cast<std::uint8_t>(ZPAD | 0x80))
                    continue;
                // 这里必须先判断 ZDLE；ZDLE 和 CAN 同为 0x18，
                // 但 ZPAD 之后的 0x18 是帧头引导符，不是取消。
                if(b == ZDLE)
                {
                    can_count_ = 0;
                    hs_ = hdr_state::kind;
                    continue;
                }
                hs_ = hdr_state::search;
                continue;
            }
            case hdr_state::kind:
            {
                int c = get();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == CAN)
                {
                    if(++can_count_ >= 5)
                        return hdr_status::cancelled;
                    hs_ = hdr_state::search;
                    continue;
                }
                can_count_ = 0;
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZBIN)
                {
                    start_header(false);
                    hs_ = hdr_state::bin;
                    continue;
                }
                if(b == ZBIN32)
                {
                    start_header(true);
                    hs_ = hdr_state::bin;
                    continue;
                }
                if(b == ZHEX)
                {
                    start_header(false);
                    hs_ = hdr_state::hex;
                    continue;
                }
                hs_ = hdr_state::search;
                continue;
            }
            case hdr_state::bin:
            {
                std::uint8_t b;
                auto r = read_escaped(get, b);
                if(r == byte_status::pending)
                    return hdr_status::pending;
                if(r == byte_status::cancelled)
                    return hdr_status::cancelled;
                if(r == byte_status::frame_end || r == byte_status::error)
                {
                    hs_ = hdr_state::search;
                    return hdr_status::error;
                }
                header_append(b);
                std::size_t need = hdr_32_ ? 9 : 7;
                if(hdr_pos_ == need)
                    return finish_header(h);
                continue;
            }
            case hdr_state::hex:
            {
                int c = get();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == XON || b == XOFF)
                    continue;
                int nib = hex_value(b);
                if(nib < 0)
                {
                    hs_ = hdr_state::search;
                    return hdr_status::error;
                }
                if(hex_nibble_ < 0)
                {
                    hex_nibble_ = nib;
                    continue;
                }
                std::uint8_t v = static_cast<std::uint8_t>((hex_nibble_ << 4) | nib);
                hex_nibble_ = -1;
                header_append(v);
                if(hdr_pos_ == 7)
                {
                    if(hdr_crc16_ != 0)
                    {
                        hs_ = hdr_state::search;
                        return hdr_status::error;
                    }
                    hex_lf_ = false;
                    hs_ = hdr_state::hex_end;
                }
                continue;
            }
            case hdr_state::hex_end:
            {
                int c = get();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(!hex_lf_)
                {
                    if(b == 0x0D || b == 0x8D)
                    {
                        hex_lf_ = true;
                        continue;
                    }
                    if(b != 0x0A && b != 0x8A)
                        pushback_ = b;
                }
                else
                {
                    hex_lf_ = false;
                    if(b != 0x0A && b != 0x8A)
                        pushback_ = b;
                }
                commit_header(h);
                return hdr_status::ok;
            }
            }
        }
    }

    void reset_data()
    {
        data_len_ = 0;
        data_in_crc_ = false;
        data_end_ = 0;
        data_crc16_ = 0;
        data_crc32_ = 0xFFFFFFFFu;
        data_crc_pos_ = 0;
        es_ = esc_state::normal;
    }

    void reset_parser()
    {
        hs_ = hdr_state::search;
        es_ = esc_state::normal;
        hdr_32_ = false;
        hdr_pos_ = 0;
        hdr_crc16_ = 0;
        hdr_crc32_ = 0xFFFFFFFFu;
        hex_nibble_ = -1;
        can_count_ = 0;
        hex_lf_ = false;
        pushback_ = -1;
    }

    data_status poll_data(auto &&get, std::uint8_t &end)
    {
        for(;;)
        {
            std::uint8_t b;
            auto r = read_escaped(get, b);
            if(r == byte_status::pending)
                return data_status::pending;
            if(r == byte_status::cancelled)
                return data_status::cancelled;
            if(r == byte_status::error)
                return data_status::error;

            if(!data_in_crc_)
            {
                if(r == byte_status::frame_end)
                {
                    data_end_ = b;
                    data_in_crc_ = true;
                    data_crc_pos_ = 0;
                    if(data_32_)
                        data_crc32_ = crc32_byte(data_crc32_, b);
                    else
                        data_crc16_ = crc16_byte(data_crc16_, b);
                    continue;
                }
                if(data_len_ >= MAX_BLOCK)
                    return data_status::error;
                buf_[data_len_++] = static_cast<std::byte>(b);
                if(data_32_)
                    data_crc32_ = crc32_byte(data_crc32_, b);
                else
                    data_crc16_ = crc16_byte(data_crc16_, b);
                continue;
            }

            if(r == byte_status::frame_end)
                return data_status::error;
            if(data_32_)
                data_crc32_ = crc32_byte(data_crc32_, b);
            else
                data_crc16_ = crc16_byte(data_crc16_, b);
            ++data_crc_pos_;

            std::size_t need = data_32_ ? 4 : 2;
            if(data_crc_pos_ == need)
            {
                bool ok = data_32_ ? data_crc32_ == CRC32_RESIDUE
                                   : data_crc16_ == 0;
                if(!ok)
                    return data_status::error;
                end = data_end_;
                data_in_crc_ = false;
                return data_status::ok;
            }
        }
    }

    void clear_data()
    {
        data_active_ = false;
        discard_ = false;
        sub_ = sub_purpose::none;
        reset_data();
    }

    // 解析 ZFILE 的 filename\0 size mtime mode ...，只保留 filename 和 size。
    void parse_file_info()
    {
        filename_.clear();
        filesize_ = kUnknown;
        if(data_len_ == 0)
            return;

        const char *s = reinterpret_cast<const char *>(buf_.data());
        std::size_t n = data_len_;
        std::size_t nul = 0;
        while(nul < n && s[nul] != '\0')
            ++nul;
        filename_.assign(s, nul);
        if(nul == n)
            return;

        std::size_t i = nul + 1;
        while(i < n && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if(i == n || s[i] < '0' || s[i] > '9')
            return;

        std::size_t v = 0;
        bool digits = false;
        while(i < n && s[i] >= '0' && s[i] <= '9')
        {
            digits = true;
            std::size_t d = static_cast<std::size_t>(s[i] - '0');
            if(v > (kUnknown - d) / 10)
            {
                filesize_ = kUnknown;
                return;
            }
            v = v * 10 + d;
            ++i;
        }
        if(digits)
            filesize_ = v;
    }

    void apply_file_and_zrpos(auto &&put)
    {
        parse_file_info();

        std::size_t offset = opt_.resume_offset;
        if(filesize_ != kUnknown && offset > filesize_)
            offset = 0;
        received_ = offset;
        send_position_header(put, ZRPOS, received_);
        ph_ = phase::data;
        data_active_ = false;
        discard_ = false;
        sub_ = sub_purpose::none;
        reset_data();
    }

    // 处理跟随头部的数据子包（ZSINIT/ZFILE/ZSTDERR/跳过后续文件）。
    bool drive_sub(auto &&get, auto &&put)
    {
        std::uint8_t end;
        auto r = poll_data(get, end);
        switch(r)
        {
        case data_status::pending:
            return false;
        case data_status::cancelled:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case data_status::error:
            send_position_header(put, ZNAK, 0);
            clear_data();
            return false;
        case data_status::ok:
            break;
        }

        switch(sub_)
        {
        case sub_purpose::sinit:
            send_position_header(put, ZACK, 1);
            clear_data();
            return false;
        case sub_purpose::zfile:
            apply_file_and_zrpos(put);
            return true;
        case sub_purpose::skip:
            send_position_header(put, ZSKIP, 0);
            clear_data();
            return false;
        case sub_purpose::discard:
        case sub_purpose::none:
            clear_data();
            return false;
        }
        clear_data();
        return false;
    }

    bool drive_wait_file(auto &&get, auto &&put)
    {
        if(sub_ != sub_purpose::none)
            return drive_sub(get, put);

        header h;
        auto r = poll_header(get, h);
        switch(r)
        {
        case hdr_status::pending:
            return false;
        case hdr_status::cancelled:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case hdr_status::error:
            send_position_header(put, ZNAK, 0);
            return false;
        case hdr_status::ok:
            break;
        }

        switch(h.type)
        {
        case ZRQINIT:
            send_zrinit(put);
            return false;
        case ZSINIT:
            sub_ = sub_purpose::sinit;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return false;
        case ZFILE:
            if(h.info[ZF2] != 0 || h.info[ZF3] != 0)
            {
                // 不支持压缩/加密/RLE/稀疏扩展，避免把非原始内容交付给调用方。
                send_cancel(put);
                ph_ = phase::done;
                end_ = end_state::failed;
                throw failed{};
            }
            sub_ = sub_purpose::zfile;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return false;
        case ZSTDERR:
            sub_ = sub_purpose::discard;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return false;
        case ZFREECNT:
            send_position_header(put, ZACK, 0);
            return false;
        case ZCHALLENGE:
            send_position_header(put, ZACK, position(h));
            return false;
        case ZFIN:
            send_position_header(put, ZFIN, 0);
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZCAN:
        case ZABORT:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case ZFERR:
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZCOMMAND:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        default:
            return false;
        }
    }

    std::size_t remaining() const
    {
        if(filesize_ == kUnknown || received_ >= filesize_)
            return kUnknown;
        return filesize_ - received_;
    }

    bool drive_data(auto &&get, auto &&put, auto &&f)
    {
        std::uint8_t end;
        auto r = poll_data(get, end);
        switch(r)
        {
        case data_status::pending:
            return false;
        case data_status::cancelled:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case data_status::error:
            send_position_header(put, ZRPOS, received_);
            data_active_ = false;
            discard_ = false;
            reset_data();
            return false;
        case data_status::ok:
            break;
        }

        if(discard_)
        {
            bool done = end == ZCRCW || end == ZCRCE || end == ZCRCQ;
            if(done)
            {
                send_position_header(put, ZRPOS, received_);
                data_active_ = false;
                discard_ = false;
            }
            reset_data();
            return false;
        }

        std::size_t deliver = data_len_;
        if(filesize_ != kUnknown && received_ < filesize_)
            deliver = std::min(deliver, filesize_ - received_);
        else if(filesize_ != kUnknown)
            deliver = 0;

        if(deliver != 0)
            f(std::span<std::byte>{buf_.data(), deliver});
        received_ += data_len_;

        if(end == ZCRCQ)
            send_position_header(put, ZACK, received_);
        else if(end == ZCRCW)
        {
            send_position_header(put, ZACK, received_);
            data_active_ = false;
        }
        else if(end == ZCRCE)
            data_active_ = false;
        // ZCRCG: 继续读下一个子包

        reset_data();
        return false;
    }

    std::size_t receive_impl(auto &&get, auto &&put, auto &&f)
    {
        if(ph_ == phase::idle)
        {
            (void)start_link_impl(get, put, options{});
            return kUnknown;
        }
        if(ph_ == phase::wait_file)
        {
            (void)start_link_impl(get, put, opt_);
            return kUnknown;
        }
        throw_if_ended();
        if(ph_ == phase::file_done || ph_ == phase::done)
            return 0;
        if(ph_ != phase::data)
            return kUnknown;

        if(sub_ != sub_purpose::none)
        {
            (void)drive_sub(get, put);
            return remaining();
        }

        if(data_active_)
        {
            (void)drive_data(get, put, f);
            return remaining();
        }

        header h;
        auto r = poll_header(get, h);
        switch(r)
        {
        case hdr_status::pending:
            return kUnknown;
        case hdr_status::cancelled:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case hdr_status::error:
            send_position_header(put, ZNAK, 0);
            return kUnknown;
        case hdr_status::ok:
            break;
        }

        switch(h.type)
        {
        case ZDATA:
        {
            std::size_t pos = position(h);
            discard_ = pos != received_;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return kUnknown;
        }
        case ZEOF:
            if(position(h) == received_)
            {
                if(filesize_ != kUnknown && received_ < filesize_)
                {
                    // 声明的文件大小还没收满就 EOF：中止并报告失败。
                    send_cancel(put);
                    ph_ = phase::done;
                    end_ = end_state::failed;
                    throw failed{};
                }
                ph_ = phase::file_done;
                return 0;
            }
            send_position_header(put, ZRPOS, received_);
            return kUnknown;
        case ZCAN:
        case ZABORT:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case ZFERR:
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZCOMMAND:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZFIN:
            send_position_header(put, ZFIN, 0);
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZFILE:
        case ZSINIT:
            // 单文件接收过程中又出现文件/初始化头，视为协议错误。
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZSTDERR:
            sub_ = sub_purpose::discard;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return kUnknown;
        case ZFREECNT:
            send_position_header(put, ZACK, 0);
            return kUnknown;
        case ZCHALLENGE:
            send_position_header(put, ZACK, position(h));
            return kUnknown;
        default:
            return kUnknown;
        }
    }

    bool start_link_impl(auto &&get, auto &&put, const options &opt)
    {
        switch(ph_)
        {
        case phase::idle:
            opt_ = opt;
            send_zrinit(put);
            ph_ = phase::wait_file;
            sub_ = sub_purpose::none;
            data_active_ = false;
            reset_parser();
            reset_data();
            return false;
        case phase::wait_file:
            return drive_wait_file(get, put);
        case phase::data:
            return true;
        case phase::file_done:
        case phase::done:
            throw_if_ended();
            return true;
        }
        return true;
    }

    bool finish_batch_impl(auto &&get, auto &&put)
    {
        if(ph_ == phase::done)
        {
            throw_if_ended();
            return true;
        }
        if(ph_ != phase::file_done)
            return false;

        if(!batch_started_)
        {
            send_zrinit(put);
            batch_started_ = true;
            return false;
        }

        if(sub_ != sub_purpose::none)
        {
            (void)drive_sub(get, put);
            return false;
        }

        header h;
        auto r = poll_header(get, h);
        switch(r)
        {
        case hdr_status::pending:
            return false;
        case hdr_status::cancelled:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case hdr_status::error:
            send_position_header(put, ZNAK, 0);
            return false;
        case hdr_status::ok:
            break;
        }

        switch(h.type)
        {
        case ZFILE:
            sub_ = sub_purpose::skip;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return false;
        case ZSINIT:
            sub_ = sub_purpose::sinit;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return false;
        case ZSTDERR:
            sub_ = sub_purpose::discard;
            data_active_ = true;
            data_32_ = h.crc32;
            reset_data();
            return false;
        case ZFIN:
            send_position_header(put, ZFIN, 0);
            ph_ = phase::done;
            return true;
        case ZRQINIT:
            send_zrinit(put);
            return false;
        case ZEOF:
            send_zrinit(put);
            return false;
        case ZCAN:
        case ZABORT:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::cancelled;
            throw cancelled{};
        case ZFERR:
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        case ZCOMMAND:
            send_cancel(put);
            ph_ = phase::done;
            end_ = end_state::failed;
            throw failed{};
        default:
            return false;
        }
    }

    void cancel_impl(auto &&put)
    {
        if(ph_ == phase::idle || ph_ == phase::done)
            return;
        send_cancel(put);
        ph_ = phase::done;
        end_ = end_state::cancelled;
        reset_parser();
        reset_data();
        throw cancelled{};
    }
};

}
