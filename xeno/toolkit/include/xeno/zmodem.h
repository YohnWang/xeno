#pragma once

#include<algorithm>
#include<array>
#include<chrono>
#include<concepts>
#include<cstddef>
#include<cstdint>
#include<exception>
#include<limits>
#include<span>
#include<string>
#include<thread>
#include<type_traits>

namespace xeno
{

// ZMODEM 接收端（单文件）。
//
// IO 由派生类提供：
//   int  try_getchar(); // 必须返回 int；0..255 为数据，负数表示暂无数据
//                       // 从 char/int8_t 取字节时先转 unsigned char，
//                       // 否则 0x80..0xFF 会被符号扩展成负数。
//   void putchar(int);  // 发送单字节
// 可选覆盖：
//   std::int64_t time() const;              // 默认 steady_clock，毫秒
//   sleep_for(std::chrono::duration);       // 默认 this_thread::sleep_for
// 通过 deducing this + 继承在编译期绑定，不检查返回值以外的 IO 状态。
// 半帧长时间无新字节时类内发 ZNAK/ZRPOS 重试，连续失败抛 timeout；
// 握手阶段超时仍由调用方处理。
//
// 所有文件内容都按原始二进制交付，不做 CR/LF 或字符集转换。
class zmodem_receiver
{
public:
    static constexpr std::size_t kUnknown = std::numeric_limits<std::size_t>::max();

    struct options
    {
        // 从文件的这个偏移开始接收；0 表示从头重传。
        // 非 0 时只发 ZRPOS，不做 ZCRC 校验，调用方需保证已有前缀一致。
        std::size_t resume_offset = 0;
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

    // 半帧长时间无新字节，且重试耗尽。
    struct timeout : failed
    {
        const char *what() const noexcept override
        {
            return "zmodem: transfer timeout";
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

    // 可配置毫秒时钟；派生类可以隐藏它，返回 int 或 int64_t。
    std::int64_t time() const
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // 可配置延时接口；本类内部不会调用，供调用方或派生类使用。
    template<typename Rep, typename Period>
    void sleep_for(const std::chrono::duration<Rep, Period> &d)
    {
        std::this_thread::sleep_for(d);
    }

    // 半帧无进展超时（毫秒）；0 关闭，默认 1000ms。
    void set_timeout(std::uint32_t ms)
    {
        timeout_ms_ = ms;
    }

    // ---- 建立连接 ----
    // true 表示已收到 ZFILE 并发出 ZRPOS；false 表示继续轮询。
    bool start_link(this auto &&self, const options &opt = {})
    {
        self.check_timeout();
        self.progressed_ = false;
        bool r = self.start_link_impl(opt);
        self.note_progress();
        return r;
    }

    // ---- 接收当前文件 ----
    // 返回 0 表示 ZEOF 且文件结束；kUnknown 表示继续；
    // 其余为剩余待交付字节数（filesize 已知时）。
    std::size_t receive(this auto &&self, auto &&f)
    {
        self.check_timeout();
        self.progressed_ = false;
        auto r = self.receive_impl(f);
        self.note_progress();
        return r;
    }

    // ---- 结束 ZMODEM 批处理 ----
    // receive() 返回 0 后调用，非阻塞轮询：
    // 发 ZRINIT，后续文件用 ZSKIP 跳过，收到 ZFIN 并回 ZFIN 后返回 true。
    bool finish_batch(this auto &&self)
    {
        self.check_timeout();
        self.progressed_ = false;
        bool r = self.finish_batch_impl();
        self.note_progress();
        return r;
    }

    // ---- 本地用户按 Ctrl+C 时调用：发 CAN 串并抛 cancelled ----
    // idle/done 状态下是空操作，不抛异常。
    void cancel(this auto &&self)
    {
        self.cancel_impl();
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
        last_rx_tick_ = 0;
        timeout_count_ = 0;
        retry_pending_ = false;
        retry_zrpos_ = false;
        progressed_ = false;
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
        failed,
        timeout
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

    std::uint64_t last_rx_tick_ = 0;
    std::uint32_t timeout_ms_ = 1000;
    std::uint32_t timeout_count_ = 0;
    bool retry_pending_ = false;
    bool retry_zrpos_ = false;
    bool progressed_ = false;
    static constexpr std::uint32_t MAX_TIMEOUTS = 3;

    int get_byte(this auto &&self)
    {
        static_assert(std::is_same_v<decltype(self.try_getchar()), int>,
                      "try_getchar() must return int; char/int8_t sign-extends bytes 0x80..0xFF");
        if(self.pushback_ >= 0)
        {
            int c = self.pushback_;
            self.pushback_ = -1;
            self.progressed_ = true;
            return c;
        }
        int c = self.try_getchar();
        if(c >= 0)
            self.progressed_ = true;
        return c;
    }

    void put_byte(this auto &&self, std::uint8_t c)
    {
        self.putchar(c);
    }

    std::uint64_t clock_value(this auto &&self)
    {
        using T = std::remove_cvref_t<decltype(self.time())>;
        static_assert(std::is_same_v<T, int> || std::is_same_v<T, std::int64_t>,
                      "time() must return int or int64_t");
        using U = std::make_unsigned_t<T>;
        return static_cast<std::uint64_t>(static_cast<U>(self.time()));
    }

    bool timeout_expired(this auto &&self)
    {
        using T = std::remove_cvref_t<decltype(self.time())>;
        using U = std::make_unsigned_t<T>;
        auto now = static_cast<U>(self.clock_value());
        auto last = static_cast<U>(self.last_rx_tick_);
        return static_cast<U>(now - last) >= static_cast<U>(self.timeout_ms_);
    }

    void note_progress(this auto &&self)
    {
        if(self.progressed_)
        {
            self.last_rx_tick_ = self.clock_value();
            self.timeout_count_ = 0;
            self.retry_pending_ = false;
            self.progressed_ = false;
        }
    }

    void check_timeout(this auto &&self)
    {
        if(self.timeout_ms_ == 0 || self.ph_ == phase::done)
            return;
        bool partial_header = self.hs_ != hdr_state::search;
        bool partial_data = self.data_active_;
        if(!partial_header && !partial_data && !self.retry_pending_)
            return;
        if(!self.timeout_expired())
            return;

        if(partial_data)
        {
            if(self.sub_ != sub_purpose::none)
            {
                self.send_position_header(ZNAK, 0);
                self.clear_data();
                self.retry_zrpos_ = false;
            }
            else
            {
                self.send_position_header(ZRPOS, self.received_);
                self.data_active_ = false;
                self.discard_ = false;
                self.reset_data();
                self.retry_zrpos_ = true;
            }
        }
        else if(partial_header)
        {
            self.send_position_header(ZNAK, 0);
            self.reset_parser();
            self.retry_zrpos_ = false;
        }
        else if(self.retry_zrpos_)
            self.send_position_header(ZRPOS, self.received_);
        else
            self.send_position_header(ZNAK, 0);

        self.retry_pending_ = true;
        self.last_rx_tick_ = self.clock_value();
        if(++self.timeout_count_ >= MAX_TIMEOUTS)
        {
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::timeout;
            throw timeout{};
        }
    }

    template<typename F>
    static void deliver_bytes(F &&f, const std::byte *data, std::size_t size)
    {
        if constexpr(std::invocable<F &, std::span<const std::byte>>)
            f(std::span<const std::byte>{data, size});
        else if constexpr(std::invocable<F &, const std::byte *, std::size_t>)
            f(data, size);
        else if constexpr(std::invocable<F &, std::span<const std::uint8_t>>)
            f(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t *>(data), size});
        else if constexpr(std::invocable<F &, const std::uint8_t *, std::size_t>)
            f(reinterpret_cast<const std::uint8_t *>(data), size);
        else if constexpr(std::invocable<F &, std::span<const char>>)
            f(std::span<const char>{reinterpret_cast<const char *>(data), size});
        else if constexpr(std::invocable<F &, const char *, std::size_t>)
            f(reinterpret_cast<const char *>(data), size);
        else if constexpr(std::invocable<F &, std::span<const std::int8_t>>)
            f(std::span<const std::int8_t>{
                reinterpret_cast<const std::int8_t *>(data), size});
        else if constexpr(std::invocable<F &, const std::int8_t *, std::size_t>)
            f(reinterpret_cast<const std::int8_t *>(data), size);
        else
            static_assert(std::invocable<F &, std::span<const std::byte>>,
                          "receive callback must accept std::span<const T> "
                          "or (const T *, std::size_t)");
    }

    void throw_if_ended() const
    {
        if(end_ == end_state::cancelled)
            throw cancelled{};
        if(end_ == end_state::timeout)
            throw timeout{};
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

    void put_hex_digit(this auto &&self, std::uint8_t v)
    {
        static constexpr char digits[] = "0123456789abcdef";
        self.put_byte(static_cast<std::uint8_t>(digits[(v >> 4) & 0x0F]));
        self.put_byte(static_cast<std::uint8_t>(digits[v & 0x0F]));
    }

    // 发送 hex 头。type + info 用 CRC16，格式与 lrzsz 的 zshhdr 一致。
    void send_hex_header(this auto &&self, std::uint8_t type, const std::uint8_t info[4])
    {
        std::uint16_t crc = 0;
        self.put_byte(ZPAD);
        self.put_byte(ZPAD);
        self.put_byte(ZDLE);
        self.put_byte(ZHEX);

        type &= 0x7F;
        self.put_hex_digit(type);
        crc = crc16_byte(crc, type);
        for(int i = 0; i < 4; ++i)
        {
            self.put_hex_digit(info[i]);
            crc = crc16_byte(crc, info[i]);
        }
        self.put_hex_digit(static_cast<std::uint8_t>(crc >> 8));
        self.put_hex_digit(static_cast<std::uint8_t>(crc));

        self.put_byte('\r');
        self.put_byte('\n');
        if(type != ZFIN && type != ZACK)
            self.put_byte(XON);
    }

    void send_position_header(this auto &&self, std::uint8_t type, std::size_t pos)
    {
        std::uint8_t info[4]{};
        set_position(info, pos);
        self.send_hex_header(type, info);
    }

    void send_zrinit(this auto &&self)
    {
        // ZP0/ZP1 = 0：按 ZMODEM 规范表示接收端允许连续 I/O（streaming），
        // 与 lrzsz rz 发出的规范 ZRINIT 一致；终端（如 WindTerm）据此自动
        // 发起文件选择。写非 0 会切换为分段传输，可能不被自动识别。
        // 内部仍以 8KB 缓冲为硬上限，超长数据子包会触发 ZRPOS。
        std::uint8_t info[4]{};
        info[ZP0] = 0;
        info[ZP1] = 0;
        info[ZF1] = 0;
        info[ZF0] = CANFDX | CANOVIO | CANFC32 | ESCCTL;
        self.send_hex_header(ZRINIT, info);
    }

    void send_cancel(this auto &&self)
    {
        for(int i = 0; i < CAN_BURST; ++i)
            self.put_byte(CAN);
        for(int i = 0; i < CAN_BURST; ++i)
            self.put_byte(0x08);
    }

    // 读取一个经过 ZDLE 转义的逻辑字节。
    byte_status read_escaped(this auto &&self, std::uint8_t &out)
    {
        for(;;)
        {
            int c = self.get_byte();
            if(c < 0)
                return byte_status::pending;
            std::uint8_t b = static_cast<std::uint8_t>(c);

            if(self.es_ == esc_state::normal)
            {
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZDLE)
                {
                    self.es_ = esc_state::after_zdle;
                    continue;
                }
                out = b;
                return byte_status::ok;
            }

            // after_zdle
            if(b == XON || b == XOFF)
                continue; // 流控字符，ZDLE 仍在等待
            self.es_ = esc_state::normal;
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

    hdr_status poll_header(this auto &&self, header &h)
    {
        for(;;)
        {
            switch(self.hs_)
            {
            case hdr_state::search:
            {
                int c = self.get_byte();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == CAN)
                {
                    if(++self.can_count_ >= 5)
                        return hdr_status::cancelled;
                    continue;
                }
                self.can_count_ = 0;
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZPAD || b == static_cast<std::uint8_t>(ZPAD | 0x80))
                {
                    self.hs_ = hdr_state::pad;
                    continue;
                }
                continue;
            }
            case hdr_state::pad:
            {
                int c = self.get_byte();
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
                    self.can_count_ = 0;
                    self.hs_ = hdr_state::kind;
                    continue;
                }
                self.hs_ = hdr_state::search;
                continue;
            }
            case hdr_state::kind:
            {
                int c = self.get_byte();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == CAN)
                {
                    if(++self.can_count_ >= 5)
                        return hdr_status::cancelled;
                    self.hs_ = hdr_state::search;
                    continue;
                }
                self.can_count_ = 0;
                if(b == XON || b == XOFF)
                    continue;
                if(b == ZBIN)
                {
                    self.start_header(false);
                    self.hs_ = hdr_state::bin;
                    continue;
                }
                if(b == ZBIN32)
                {
                    self.start_header(true);
                    self.hs_ = hdr_state::bin;
                    continue;
                }
                if(b == ZHEX)
                {
                    self.start_header(false);
                    self.hs_ = hdr_state::hex;
                    continue;
                }
                self.hs_ = hdr_state::search;
                continue;
            }
            case hdr_state::bin:
            {
                std::uint8_t b;
                auto r = self.read_escaped(b);
                if(r == byte_status::pending)
                    return hdr_status::pending;
                if(r == byte_status::cancelled)
                    return hdr_status::cancelled;
                if(r == byte_status::frame_end || r == byte_status::error)
                {
                    self.hs_ = hdr_state::search;
                    return hdr_status::error;
                }
                self.header_append(b);
                std::size_t need = self.hdr_32_ ? 9 : 7;
                if(self.hdr_pos_ == need)
                    return self.finish_header(h);
                continue;
            }
            case hdr_state::hex:
            {
                int c = self.get_byte();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(b == XON || b == XOFF)
                    continue;
                int nib = self.hex_value(b);
                if(nib < 0)
                {
                    self.hs_ = hdr_state::search;
                    return hdr_status::error;
                }
                if(self.hex_nibble_ < 0)
                {
                    self.hex_nibble_ = nib;
                    continue;
                }
                std::uint8_t v = static_cast<std::uint8_t>((self.hex_nibble_ << 4) | nib);
                self.hex_nibble_ = -1;
                self.header_append(v);
                if(self.hdr_pos_ == 7)
                {
                    if(self.hdr_crc16_ != 0)
                    {
                        self.hs_ = hdr_state::search;
                        return hdr_status::error;
                    }
                    self.hex_lf_ = false;
                    self.hs_ = hdr_state::hex_end;
                }
                continue;
            }
            case hdr_state::hex_end:
            {
                int c = self.get_byte();
                if(c < 0)
                    return hdr_status::pending;
                std::uint8_t b = static_cast<std::uint8_t>(c);
                if(!self.hex_lf_)
                {
                    if(b == 0x0D || b == 0x8D)
                    {
                        self.hex_lf_ = true;
                        continue;
                    }
                    if(b != 0x0A && b != 0x8A)
                        self.pushback_ = b;
                }
                else
                {
                    self.hex_lf_ = false;
                    if(b != 0x0A && b != 0x8A)
                        self.pushback_ = b;
                }
                self.commit_header(h);
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

    data_status poll_data(this auto &&self, std::uint8_t &end)
    {
        for(;;)
        {
            std::uint8_t b;
            auto r = self.read_escaped(b);
            if(r == byte_status::pending)
                return data_status::pending;
            if(r == byte_status::cancelled)
                return data_status::cancelled;
            if(r == byte_status::error)
                return data_status::error;

            if(!self.data_in_crc_)
            {
                if(r == byte_status::frame_end)
                {
                    self.data_end_ = b;
                    self.data_in_crc_ = true;
                    self.data_crc_pos_ = 0;
                    if(self.data_32_)
                        self.data_crc32_ = self.crc32_byte(self.data_crc32_, b);
                    else
                        self.data_crc16_ = self.crc16_byte(self.data_crc16_, b);
                    continue;
                }
                if(self.data_len_ >= MAX_BLOCK)
                    return data_status::error;
                self.buf_[self.data_len_++] = static_cast<std::byte>(b);
                if(self.data_32_)
                    self.data_crc32_ = self.crc32_byte(self.data_crc32_, b);
                else
                    self.data_crc16_ = self.crc16_byte(self.data_crc16_, b);
                continue;
            }

            if(r == byte_status::frame_end)
                return data_status::error;
            if(self.data_32_)
                self.data_crc32_ = self.crc32_byte(self.data_crc32_, b);
            else
                self.data_crc16_ = self.crc16_byte(self.data_crc16_, b);
            ++self.data_crc_pos_;

            std::size_t need = self.data_32_ ? 4 : 2;
            if(self.data_crc_pos_ == need)
            {
                bool ok = self.data_32_ ? self.data_crc32_ == CRC32_RESIDUE
                                        : self.data_crc16_ == 0;
                if(!ok)
                    return data_status::error;
                end = self.data_end_;
                self.data_in_crc_ = false;
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

    void apply_file_and_zrpos(this auto &&self)
    {
        self.parse_file_info();

        std::size_t offset = self.opt_.resume_offset;
        if(self.filesize_ != kUnknown && offset > self.filesize_)
            offset = 0;
        self.received_ = offset;
        self.send_position_header(ZRPOS, self.received_);
        self.ph_ = phase::data;
        self.data_active_ = false;
        self.discard_ = false;
        self.sub_ = sub_purpose::none;
        self.reset_data();
    }

    // 处理跟随头部的数据子包（ZSINIT/ZFILE/ZSTDERR/跳过后续文件）。
    bool drive_sub(this auto &&self)
    {
        std::uint8_t end;
        auto r = self.poll_data(end);
        switch(r)
        {
        case data_status::pending:
            return false;
        case data_status::cancelled:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case data_status::error:
            self.send_position_header(ZNAK, 0);
            self.clear_data();
            return false;
        case data_status::ok:
            break;
        }

        switch(self.sub_)
        {
        case sub_purpose::sinit:
            self.send_position_header(ZACK, 1);
            self.clear_data();
            return false;
        case sub_purpose::zfile:
            self.apply_file_and_zrpos();
            return true;
        case sub_purpose::skip:
            self.send_position_header(ZSKIP, 0);
            self.clear_data();
            return false;
        case sub_purpose::discard:
        case sub_purpose::none:
            self.clear_data();
            return false;
        }
        self.clear_data();
        return false;
    }

    bool drive_wait_file(this auto &&self)
    {
        if(self.sub_ != sub_purpose::none)
            return self.drive_sub();

        header h;
        auto r = self.poll_header(h);
        switch(r)
        {
        case hdr_status::pending:
            return false;
        case hdr_status::cancelled:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case hdr_status::error:
            self.send_position_header(ZNAK, 0);
            return false;
        case hdr_status::ok:
            break;
        }

        switch(h.type)
        {
        case ZRQINIT:
            self.send_zrinit();
            return false;
        case ZSINIT:
            self.sub_ = sub_purpose::sinit;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return false;
        case ZFILE:
            if(h.info[ZF2] != 0 || h.info[ZF3] != 0)
            {
                // 不支持压缩/加密/RLE/稀疏扩展，避免把非原始内容交付给调用方。
                self.send_cancel();
                self.ph_ = phase::done;
                self.end_ = end_state::failed;
                throw failed{};
            }
            self.sub_ = sub_purpose::zfile;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return false;
        case ZSTDERR:
            self.sub_ = sub_purpose::discard;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return false;
        case ZFREECNT:
            self.send_position_header(ZACK, 0);
            return false;
        case ZCHALLENGE:
            self.send_position_header(ZACK, position(h));
            return false;
        case ZFIN:
            self.send_position_header(ZFIN, 0);
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZCAN:
        case ZABORT:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case ZFERR:
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZCOMMAND:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
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

    bool drive_data(this auto &&self, auto &&f)
    {
        std::uint8_t end;
        auto r = self.poll_data(end);
        switch(r)
        {
        case data_status::pending:
            return false;
        case data_status::cancelled:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case data_status::error:
            self.send_position_header(ZRPOS, self.received_);
            self.data_active_ = false;
            self.discard_ = false;
            self.reset_data();
            return false;
        case data_status::ok:
            break;
        }

        if(self.discard_)
        {
            bool done = end == ZCRCW || end == ZCRCE || end == ZCRCQ;
            if(done)
            {
                self.send_position_header(ZRPOS, self.received_);
                self.data_active_ = false;
                self.discard_ = false;
            }
            self.reset_data();
            return false;
        }

        std::size_t deliver = self.data_len_;
        if(self.filesize_ != kUnknown && self.received_ < self.filesize_)
            deliver = std::min(deliver, self.filesize_ - self.received_);
        else if(self.filesize_ != kUnknown)
            deliver = 0;

        if(deliver != 0)
            self.deliver_bytes(f, self.buf_.data(), deliver);
        self.received_ += self.data_len_;

        if(end == ZCRCQ)
            self.send_position_header(ZACK, self.received_);
        else if(end == ZCRCW)
        {
            self.send_position_header(ZACK, self.received_);
            self.data_active_ = false;
        }
        else if(end == ZCRCE)
            self.data_active_ = false;
        // ZCRCG: 继续读下一个子包

        self.reset_data();
        return false;
    }

    std::size_t receive_impl(this auto &&self, auto &&f)
    {
        if(self.ph_ == phase::idle)
        {
            (void)self.start_link_impl(options{});
            return kUnknown;
        }
        if(self.ph_ == phase::wait_file)
        {
            (void)self.start_link_impl(self.opt_);
            return kUnknown;
        }
        self.throw_if_ended();
        if(self.ph_ == phase::file_done || self.ph_ == phase::done)
            return 0;
        if(self.ph_ != phase::data)
            return kUnknown;

        if(self.sub_ != sub_purpose::none)
        {
            (void)self.drive_sub();
            return self.remaining();
        }

        if(self.data_active_)
        {
            (void)self.drive_data(f);
            return self.remaining();
        }

        header h;
        auto r = self.poll_header(h);
        switch(r)
        {
        case hdr_status::pending:
            return kUnknown;
        case hdr_status::cancelled:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case hdr_status::error:
            self.send_position_header(ZNAK, 0);
            return kUnknown;
        case hdr_status::ok:
            break;
        }

        switch(h.type)
        {
        case ZDATA:
        {
            std::size_t pos = position(h);
            self.discard_ = pos != self.received_;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return kUnknown;
        }
        case ZEOF:
            if(position(h) == self.received_)
            {
                if(self.filesize_ != kUnknown && self.received_ < self.filesize_)
                {
                    // 声明的文件大小还没收满就 EOF：中止并报告失败。
                    self.send_cancel();
                    self.ph_ = phase::done;
                    self.end_ = end_state::failed;
                    throw failed{};
                }
                self.ph_ = phase::file_done;
                return 0;
            }
            self.send_position_header(ZRPOS, self.received_);
            return kUnknown;
        case ZCAN:
        case ZABORT:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case ZFERR:
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZCOMMAND:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZFIN:
            self.send_position_header(ZFIN, 0);
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZFILE:
        case ZSINIT:
            // 单文件接收过程中又出现文件/初始化头，视为协议错误。
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZSTDERR:
            self.sub_ = sub_purpose::discard;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return kUnknown;
        case ZFREECNT:
            self.send_position_header(ZACK, 0);
            return kUnknown;
        case ZCHALLENGE:
            self.send_position_header(ZACK, position(h));
            return kUnknown;
        default:
            return kUnknown;
        }
    }

    bool start_link_impl(this auto &&self, const options &opt)
    {
        switch(self.ph_)
        {
        case phase::idle:
            self.opt_ = opt;
            self.send_zrinit();
            self.ph_ = phase::wait_file;
            self.sub_ = sub_purpose::none;
            self.data_active_ = false;
            self.reset_parser();
            self.reset_data();
            return false;
        case phase::wait_file:
            return self.drive_wait_file();
        case phase::data:
            return true;
        case phase::file_done:
        case phase::done:
            self.throw_if_ended();
            return true;
        }
        return true;
    }

    bool finish_batch_impl(this auto &&self)
    {
        if(self.ph_ == phase::done)
        {
            self.throw_if_ended();
            return true;
        }
        if(self.ph_ != phase::file_done)
            return false;

        if(!self.batch_started_)
        {
            self.send_zrinit();
            self.batch_started_ = true;
            return false;
        }

        if(self.sub_ != sub_purpose::none)
        {
            (void)self.drive_sub();
            return false;
        }

        header h;
        auto r = self.poll_header(h);
        switch(r)
        {
        case hdr_status::pending:
            return false;
        case hdr_status::cancelled:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case hdr_status::error:
            self.send_position_header(ZNAK, 0);
            return false;
        case hdr_status::ok:
            break;
        }

        switch(h.type)
        {
        case ZFILE:
            self.sub_ = sub_purpose::skip;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return false;
        case ZSINIT:
            self.sub_ = sub_purpose::sinit;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return false;
        case ZSTDERR:
            self.sub_ = sub_purpose::discard;
            self.data_active_ = true;
            self.data_32_ = h.crc32;
            self.reset_data();
            return false;
        case ZFIN:
            self.send_position_header(ZFIN, 0);
            self.ph_ = phase::done;
            return true;
        case ZRQINIT:
            self.send_zrinit();
            return false;
        case ZEOF:
            self.send_zrinit();
            return false;
        case ZCAN:
        case ZABORT:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case ZFERR:
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        case ZCOMMAND:
            self.send_cancel();
            self.ph_ = phase::done;
            self.end_ = end_state::failed;
            throw failed{};
        default:
            return false;
        }
    }

    void cancel_impl(this auto &&self)
    {
        if(self.ph_ == phase::idle || self.ph_ == phase::done)
            return;
        self.send_cancel();
        self.ph_ = phase::done;
        self.end_ = end_state::cancelled;
        self.reset_parser();
        self.reset_data();
        throw cancelled{};
    }
};

}
