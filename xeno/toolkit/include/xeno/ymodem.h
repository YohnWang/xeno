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

// YMODEM-1K 接收端（单文件）。
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
// try_link()/try_receive()/try_finish_batch() 非阻塞；
// link()/receive()/finish_batch() 阻塞并处理重试与超时。
class ymodem_receiver
{
    template<typename F>
    static constexpr bool is_consumer =
        std::invocable<F &, std::span<const std::byte>>
        || std::invocable<F &, const std::byte *, std::size_t>
        || std::invocable<F &, std::span<const std::uint8_t>>
        || std::invocable<F &, const std::uint8_t *, std::size_t>
        || std::invocable<F &, std::span<const char>>
        || std::invocable<F &, const char *, std::size_t>
        || std::invocable<F &, std::span<const std::int8_t>>
        || std::invocable<F &, const std::int8_t *, std::size_t>;

    template<typename T>
    static constexpr bool is_data_element =
        std::same_as<T, std::byte> || std::same_as<T, std::uint8_t>
        || std::same_as<T, char> || std::same_as<T, std::int8_t>;

    static constexpr std::chrono::milliseconds poll_interval{10};

public:
    static constexpr std::size_t kUnknown = std::numeric_limits<std::size_t>::max();

    // 传输非正常结束。link()/receive()/finish_batch() 在终止状态下抛出。
    struct error : std::exception
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer error";
        }
    };

    // 本地 cancel()、远端 CAN/CTRL-C，或中止后再次调用 link()/receive()/finish_batch()。
    struct cancelled : error
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer cancelled";
        }
    };

    // 握手阶段收到 EOT、空文件头、未知文件大小，或数据未收完就收到 EOT。
    struct failed : error
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer failed";
        }
    };

    // 阻塞接口重试耗尽。
    struct timeout : failed
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer timeout";
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

    // 可配置延时接口；link()/receive()/finish_batch() 使用它。
    template<typename Rep, typename Period>
    void sleep_for(const std::chrono::duration<Rep, Period> &d)
    {
        std::this_thread::sleep_for(d);
    }

    // 单参数：总时长，使用当前 retry interval 计算重试次数；
    // 双参数：单次重试间隔 + 重试次数。
    template<typename Rep, typename Period>
    void set_link_timeout(this auto &&self,
                          const std::chrono::duration<Rep, Period> &total)
    {
        using namespace std::chrono;
        auto total_ms = duration_cast<milliseconds>(total).count();
        if(total_ms <= 0)
        {
            self.link_retry_interval = milliseconds{0};
            self.link_retries = 0;
            return;
        }
        if(self.link_retry_interval.count() <= 0)
            self.link_retry_interval = milliseconds{1000};
        auto interval_ms = self.link_retry_interval.count();
        auto retries = (static_cast<std::uint64_t>(total_ms) +
                        static_cast<std::uint64_t>(interval_ms) - 1) /
                       static_cast<std::uint64_t>(interval_ms);
        self.link_retries = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(retries,
                                    std::numeric_limits<std::uint32_t>::max()));
    }

    // 双参数：单次重试间隔 + 重试次数。
    template<typename Rep, typename Period>
    void set_link_timeout(this auto &&self,
                          const std::chrono::duration<Rep, Period> &retry_interval,
                          std::uint32_t retries)
    {
        self.link_retry_interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(retry_interval);
        self.link_retries = retries == 0 ? 1 : retries;
    }

    template<typename Rep, typename Period>
    void set_receive_timeout(this auto &&self,
                             const std::chrono::duration<Rep, Period> &total)
    {
        using namespace std::chrono;
        auto total_ms = duration_cast<milliseconds>(total).count();
        if(total_ms <= 0)
        {
            self.receive_retry_interval = milliseconds{0};
            self.receive_retries = 0;
            return;
        }
        if(self.receive_retry_interval.count() <= 0)
            self.receive_retry_interval = milliseconds{1000};
        auto interval_ms = self.receive_retry_interval.count();
        auto retries = (static_cast<std::uint64_t>(total_ms) +
                        static_cast<std::uint64_t>(interval_ms) - 1) /
                       static_cast<std::uint64_t>(interval_ms);
        self.receive_retries = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(retries,
                                    std::numeric_limits<std::uint32_t>::max()));
    }

    template<typename Rep, typename Period>
    void set_receive_timeout(this auto &&self,
                             const std::chrono::duration<Rep, Period> &retry_interval,
                             std::uint32_t retries)
    {
        self.receive_retry_interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(retry_interval);
        self.receive_retries = retries == 0 ? 1 : retries;
    }

    // ---- 建立连接（非阻塞单次处理，不抛 timeout） ----
    bool try_link(this auto &&self)
    {
        switch(self.ph_)
        {
        case phase::idle:
            self.put_byte(CRC_REQ);
            self.last_link_tick_ = self.clock_value();
            self.link_retry_count_ = 0;
            self.progressed_ = false;
            self.ph_ = phase::wait_block0;
            self.reset_frame();
            return false;
        case phase::wait_block0:
            return self.drive_block0();
        case phase::data_phase:
        case phase::awaiting_eot:
            return true;
        case phase::finished:
            if(self.end_ == end_state::timeout)
                return false;
            self.throw_if_ended_except_timeout();
            return true;
        }
        return true;
    }

    // ---- 阻塞建立连接；超时发 CAN 并抛 timeout ----
    void link(this auto &&self)
    {
        self.link_retry_count_ = 0;
        self.last_link_tick_ = self.clock_value();
        self.progressed_ = false;

        for(;;)
        {
            if(self.try_link())
                return;
            self.throw_if_ended();

            if(self.progressed_)
            {
                self.progressed_ = false;
                self.link_retry_count_ = 0;
                self.last_link_tick_ = self.clock_value();
            }

            if(self.link_retry_interval.count() <= 0)
            {
                self.sleep_for(poll_interval);
                continue;
            }

            auto now = self.clock_value();
            if(self.timeout_expired(self.last_link_tick_, self.link_retry_interval))
            {
                self.retry_link();
                self.last_link_tick_ = now;
                auto max_retries = self.link_retries == 0 ? 1u : self.link_retries;
                if(++self.link_retry_count_ >= max_retries)
                {
                    self.send_cancel();
                    self.ph_ = phase::finished;
                    self.end_ = end_state::timeout;
                    throw timeout{};
                }
            }
            self.sleep_for(poll_interval);
        }
    }

    // ---- 接收（非阻塞单次处理，不抛 timeout） ----
    template<typename T>
    int try_receive(this auto &&self, std::span<const T> &data)
        requires(is_data_element<T>)
    {
        data = {};
        return receive_result(self.try_receive_impl([&](const std::byte *p, std::size_t n)
        {
            data = std::span<const T>{reinterpret_cast<const T *>(p), n};
        }));
    }

    template<typename F>
    int try_receive(this auto &&self, F &&f)
        requires(is_consumer<F>)
    {
        return receive_result(self.try_receive_impl([&](const std::byte *p, std::size_t n)
        {
            deliver_bytes(f, p, n);
        }));
    }

    // ---- 阻塞接收；至少交付一段非空数据或文件结束 ----
    template<typename T>
    int receive(this auto &&self, std::span<const T> &data)
        requires(is_data_element<T>)
    {
        data = {};
        return self.receive_impl([&](const std::byte *p, std::size_t n)
        {
            data = std::span<const T>{reinterpret_cast<const T *>(p), n};
        });
    }

    template<typename F>
    int receive(this auto &&self, F &&f)
        requires(is_consumer<F>)
    {
        return self.receive_impl([&](const std::byte *p, std::size_t n)
        {
            deliver_bytes(f, p, n);
        });
    }

    // ---- 批处理结束握手（非阻塞单次处理，不抛 timeout） ----
    bool try_finish_batch(this auto &&self)
    {
        if(self.ph_ != phase::finished)
            return false;
        if(self.end_ == end_state::timeout)
            return false;
        self.throw_if_ended_except_timeout();

        if(self.batch_ == batch_state::done)
            return true;
        if(self.batch_ == batch_state::none)
        {
            self.put_byte(CRC_REQ);
            self.batch_ = batch_state::waiting;
            self.expected_seq_ = 0;
            self.reset_frame();
            return false;
        }

        auto r = self.read_frame();
        switch(r)
        {
        case read_result::pending:
        case read_result::error:
        case read_result::duplicate:
            return false;
        case read_result::cancelled:
            self.send_cancel();
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case read_result::eot:
            self.put_byte(ACK);
            self.put_byte(CRC_REQ);
            return false;
        case read_result::ok:
            break;
        }

        // 空 Block 0 的第一个 payload 字节为 0；不调用 parse_block0，
        // 以保留已接收文件的 filename()/filesize()。
        bool empty_header = self.body_[0] == std::byte{0};
        self.reset_frame();
        if(empty_header)
        {
            self.put_byte(ACK);
            self.batch_ = batch_state::done;
            return true;
        }

        // 单文件接收端不能再收下一个文件，中止整个批处理。
        self.send_cancel();
        self.end_ = end_state::failed;
        throw failed{};
    }

    // ---- 阻塞完成批处理结束握手；超时发 CAN 并抛 timeout ----
    void finish_batch(this auto &&self)
    {
        if(self.ph_ != phase::finished)
            throw failed{};
        self.throw_if_ended();

        self.receive_retry_count_ = 0;
        self.last_rx_tick_ = self.clock_value();
        self.progressed_ = false;

        for(;;)
        {
            if(self.try_finish_batch())
                return;
            self.throw_if_ended();

            if(self.progressed_)
            {
                self.progressed_ = false;
                self.receive_retry_count_ = 0;
                self.last_rx_tick_ = self.clock_value();
            }

            if(self.receive_retry_interval.count() <= 0)
            {
                self.sleep_for(poll_interval);
                continue;
            }

            auto now = self.clock_value();
            if(self.timeout_expired(self.last_rx_tick_, self.receive_retry_interval))
            {
                self.retry_link();
                self.last_rx_tick_ = now;
                auto max_retries = self.receive_retries == 0 ? 1u : self.receive_retries;
                if(++self.receive_retry_count_ >= max_retries)
                {
                    self.send_cancel();
                    self.end_ = end_state::timeout;
                    throw timeout{};
                }
            }
            self.sleep_for(poll_interval);
        }
    }

    // ---- 本地用户按 Ctrl+C 时调用：向发送方发 CAN，并抛 cancelled ----
    // idle/finished 状态下是空操作，不抛异常。
    void cancel(this auto &&self)
    {
        if(self.ph_ == phase::idle || self.ph_ == phase::finished)
            return;
        self.ph_ = phase::finished;
        self.end_ = end_state::cancelled;
        self.reset_frame();
        self.send_cancel();
        throw cancelled{};
    }

    void reset()
    {
        ph_ = phase::idle;
        end_ = end_state::none;
        reset_frame();
        expected_seq_ = 0;
        data_started_ = false;
        batch_ = batch_state::none;
        filename_.clear();
        filesize_ = kUnknown;
        received_ = 0;
        last_link_tick_ = 0;
        link_retry_count_ = 0;
        last_rx_tick_ = 0;
        receive_retry_count_ = 0;
        progressed_ = false;
    }

private:
    static constexpr std::uint8_t SOH = 0x01, STX = 0x02, EOT = 0x04;
    static constexpr std::uint8_t ACK = 0x06, NAK = 0x15;
    static constexpr std::uint8_t CAN = 0x18, CTRL_C = 0x03, CRC_REQ = 0x43;
    static constexpr std::size_t PKT_128 = 128, PKT_1K = 1024;
    static constexpr std::size_t HDR_SIZE = 3, CRC_SIZE = 2;
    static constexpr int CAN_BURST = 8; // 与 lrzsz 一致

    enum class phase
    {
        idle,
        wait_block0,
        data_phase,
        awaiting_eot,
        finished
    };

    enum class read_result
    {
        pending,
        ok,
        error,
        duplicate,
        eot,
        cancelled
    };

    enum class end_state
    {
        none,
        cancelled,
        failed,
        timeout
    };

    enum class batch_state
    {
        none,
        waiting,
        done
    };

    struct receive_step
    {
        std::size_t countdown = 0; // 本次调用开始前的剩余文件字节数
        bool delivered = false;
        bool done = false;
    };

    phase ph_ = phase::idle;
    end_state end_ = end_state::none;
    batch_state batch_ = batch_state::none;
    std::uint8_t expected_seq_ = 0;
    bool data_started_ = false; // 是否已接受过数据帧（区分 Block 0 重传与序号回绕）

    std::string filename_;
    std::size_t filesize_ = kUnknown;
    std::size_t received_ = 0;

    std::array<std::uint8_t, HDR_SIZE> hdr_{};
    std::size_t hdr_pos_ = 0;
    std::array<std::byte, PKT_1K + CRC_SIZE> body_{};
    std::size_t body_pos_ = 0;
    std::size_t body_len_ = 0;

    std::chrono::milliseconds link_retry_interval{1000};
    std::uint32_t link_retries = 30;
    std::chrono::milliseconds receive_retry_interval{1000};
    std::uint32_t receive_retries = 3;

    std::uint64_t last_link_tick_ = 0;
    std::uint32_t link_retry_count_ = 0;
    std::uint64_t last_rx_tick_ = 0;
    std::uint32_t receive_retry_count_ = 0;
    bool progressed_ = false;

    int get_byte(this auto &&self)
    {
        static_assert(std::is_same_v<decltype(self.try_getchar()), int>,
                      "try_getchar() must return int; char/int8_t sign-extends bytes 0x80..0xFF");
        return self.try_getchar();
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

    bool timeout_expired(this auto &&self, std::uint64_t last_tick,
                         std::chrono::milliseconds timeout)
    {
        using T = std::remove_cvref_t<decltype(self.time())>;
        using U = std::make_unsigned_t<T>;
        auto now = static_cast<U>(self.clock_value());
        auto last = static_cast<U>(last_tick);
        return static_cast<U>(now - last) >=
               static_cast<U>(timeout.count());
    }

    std::size_t remaining() const
    {
        return filesize_ - received_;
    }

    static int receive_result(const receive_step &step)
    {
        if(step.delivered)
            return static_cast<int>(step.countdown);
        if(step.done)
            return 0;
        return -1;
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

    void reset_frame()
    {
        hdr_pos_ = body_pos_ = body_len_ = 0;
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

    void throw_if_ended_except_timeout() const
    {
        if(end_ == end_state::cancelled)
            throw cancelled{};
        if(end_ == end_state::failed)
            throw failed{};
    }

    void send_cancel(this auto &&self)
    {
        for(int i = 0; i < CAN_BURST; ++i)
            self.put_byte(CAN);
    }

    void retry_link(this auto &&self)
    {
        if(self.hdr_pos_ != 0 || self.body_pos_ != 0)
        {
            self.put_byte(NAK);
            self.reset_frame();
            return;
        }
        self.put_byte(CRC_REQ);
    }

    void retry_receive(this auto &&self)
    {
        self.put_byte(NAK);
        if(self.hdr_pos_ != 0 || self.body_pos_ != 0)
            self.reset_frame();
    }

    static std::uint16_t crc16(std::span<const std::byte> data)
    {
        std::uint16_t crc = 0;
        for(auto b: data)
        {
            crc ^= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(b)) << 8;
            for(int i = 0; i < 8; ++i)
                crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x1021)
                                     : std::uint16_t(crc << 1);
        }
        return crc;
    }

    void parse_block0(std::span<const std::byte> payload)
    {
        filename_.clear();
        filesize_ = kUnknown;

        auto nul = std::find(payload.begin(), payload.end(), std::byte{0});
        filename_.assign(reinterpret_cast<const char *>(payload.data()),
                         static_cast<std::size_t>(nul - payload.begin()));
        if(nul == payload.end())
            return;

        auto s = nul + 1;
        if(s == payload.end() || *s == std::byte{0})
            return;

        std::size_t v = 0;
        bool digits = false;
        for(auto it = s; it != payload.end() && *it != std::byte{0}; ++it)
        {
            auto c = std::to_integer<unsigned char>(*it);
            if(c < '0' || c > '9')
                break;
            digits = true;
            if(v > (kUnknown - static_cast<std::size_t>(c - '0')) / 10)
            {
                filesize_ = kUnknown;
                return;
            }
            v = v * 10 + static_cast<std::size_t>(c - '0');
        }
        if(digits)
            filesize_ = v;
    }

    read_result read_frame(this auto &&self)
    {
        while(self.hdr_pos_ < HDR_SIZE)
        {
            int c = self.get_byte();
            if(c < 0)
                return read_result::pending;
            self.progressed_ = true;
            self.hdr_[self.hdr_pos_++] = static_cast<std::uint8_t>(c);

            // EOT/CAN 是单字节信号，必须在帧头首字节立即判断。
            // 数据段里的 0x18/0x03 是合法数据，只有这里检测。
            if(self.hdr_pos_ == 1)
            {
                switch(self.hdr_[0])
                {
                case EOT:
                    self.reset_frame();
                    return read_result::eot;
                case CAN:
                case CTRL_C:
                    self.reset_frame();
                    return read_result::cancelled;
                case SOH:
                    self.body_len_ = PKT_128 + CRC_SIZE;
                    break;
                case STX:
                    self.body_len_ = PKT_1K + CRC_SIZE;
                    break;
                default:
                    self.put_byte(NAK);
                    self.reset_frame();
                    return read_result::error;
                }
            }
        }

        while(self.body_pos_ < self.body_len_)
        {
            int c = self.get_byte();
            if(c < 0)
                return read_result::pending;
            self.progressed_ = true;
            self.body_[self.body_pos_++] = static_cast<std::byte>(c);
        }

        std::size_t data_len = self.body_len_ - CRC_SIZE;
        std::span<const std::byte> payload{self.body_.data(), data_len};
        std::uint16_t recv_crc =
            (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(self.body_[data_len])) << 8) |
            std::to_integer<std::uint8_t>(self.body_[data_len + 1]);

        std::uint8_t seq = self.hdr_[1], nseq = self.hdr_[2];
        if(self.crc16(payload) != recv_crc || nseq != static_cast<std::uint8_t>(~seq))
        {
            self.put_byte(NAK);
            self.reset_frame();
            return read_result::error;
        }
        if(seq == self.expected_seq_)
            return read_result::ok;
        if(self.ph_ != phase::wait_block0 &&
           seq == static_cast<std::uint8_t>(self.expected_seq_ - 1))
        {
            self.put_byte(ACK);
            // Block 0 重传：发送方还在等 'C'，补发一次。
            if(seq == 0 && !self.data_started_)
                self.put_byte(CRC_REQ);
            self.reset_frame();
            return read_result::duplicate;
        }
        self.put_byte(NAK);
        self.reset_frame();
        return read_result::error;
    }

    bool drive_block0(this auto &&self)
    {
        auto r = self.read_frame();
        switch(r)
        {
        case read_result::pending:
        case read_result::error:
        case read_result::duplicate:
            return false;
        case read_result::cancelled:
            self.send_cancel();
            self.ph_ = phase::finished;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case read_result::eot:
            self.put_byte(ACK);
            self.ph_ = phase::finished;
            self.end_ = end_state::failed;
            throw failed{};
        case read_result::ok:
            break;
        }

        std::size_t data_len = self.body_len_ - CRC_SIZE;
        std::span<const std::byte> payload{self.body_.data(), data_len};
        self.parse_block0(payload);
        self.reset_frame();

        if(self.filename_.empty())
        {
            // 空文件头是批处理的结束标记；单文件接收视为失败。
            self.put_byte(ACK);
            self.ph_ = phase::finished;
            self.end_ = end_state::failed;
            throw failed{};
        }

        // 不再支持未知文件大小；没有大小时直接中止。
        if(self.filesize_ == kUnknown)
        {
            self.send_cancel();
            self.ph_ = phase::finished;
            self.end_ = end_state::failed;
            throw failed{};
        }

        // 返回值为 int，文件总长必须能放进正数倒计时。
        if(self.filesize_ >
           static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            self.send_cancel();
            self.ph_ = phase::finished;
            self.end_ = end_state::failed;
            throw failed{};
        }

        self.expected_seq_ = 1;
        self.ph_ = phase::data_phase;
        self.put_byte(ACK);
        self.put_byte(CRC_REQ);
        return true;
    }

    template<typename Deliver>
    receive_step try_receive_impl(this auto &&self, Deliver &&deliver)
    {
        switch(self.ph_)
        {
        case phase::idle:
        case phase::wait_block0:
            throw failed{};
        case phase::finished:
            if(self.end_ == end_state::timeout)
                return {self.remaining(), false, false};
            self.throw_if_ended_except_timeout();
            return {0, false, true};
        case phase::data_phase:
        case phase::awaiting_eot:
            break;
        }

        std::size_t countdown = self.remaining();
        auto r = self.read_frame();
        switch(r)
        {
        case read_result::pending:
        case read_result::error:
        case read_result::duplicate:
            return {countdown, false, false};
        case read_result::cancelled:
            self.send_cancel();
            self.ph_ = phase::finished;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case read_result::eot:
            self.put_byte(ACK);
            self.ph_ = phase::finished;
            if(self.received_ < self.filesize_)
            {
                self.end_ = end_state::failed;
                throw failed{};
            }
            return {0, false, true};
        case read_result::ok:
            break;
        }

        std::size_t data_len = self.body_len_ - CRC_SIZE;
        std::span<const std::byte> payload{self.body_.data(), data_len};
        std::size_t deliver_len =
            std::min(data_len, self.filesize_ - self.received_);

        // 先消费帧再回调：回调抛异常时不会把同一帧再交付一次，也不回 ACK，
        // 发送方会重发该帧。
        self.reset_frame();
        if(deliver_len != 0)
            deliver(payload.data(), deliver_len);
        self.received_ += deliver_len;
        self.data_started_ = true;
        self.expected_seq_ = static_cast<std::uint8_t>(self.expected_seq_ + 1);
        self.put_byte(ACK);

        if(self.received_ >= self.filesize_)
            self.ph_ = phase::awaiting_eot;
        return {countdown, deliver_len != 0, false};
    }

    template<typename Deliver>
    int receive_impl(this auto &&self, Deliver &&deliver)
    {
        if(self.ph_ == phase::idle || self.ph_ == phase::wait_block0)
            throw failed{};
        self.throw_if_ended();

        self.receive_retry_count_ = 0;
        self.last_rx_tick_ = self.clock_value();
        self.progressed_ = false;

        for(;;)
        {
            auto step = self.try_receive_impl(deliver);
            if(step.delivered)
                return static_cast<int>(step.countdown);
            if(step.done)
                return 0;
            self.throw_if_ended();

            if(self.progressed_)
            {
                self.progressed_ = false;
                self.receive_retry_count_ = 0;
                self.last_rx_tick_ = self.clock_value();
            }

            if(self.receive_retry_interval.count() <= 0)
            {
                self.sleep_for(poll_interval);
                continue;
            }

            auto now = self.clock_value();
            if(self.timeout_expired(self.last_rx_tick_, self.receive_retry_interval))
            {
                self.retry_receive();
                self.last_rx_tick_ = now;
                auto max_retries = self.receive_retries == 0 ? 1u : self.receive_retries;
                if(++self.receive_retry_count_ >= max_retries)
                {
                    self.send_cancel();
                    self.ph_ = phase::finished;
                    self.end_ = end_state::timeout;
                    throw timeout{};
                }
            }
            self.sleep_for(poll_interval);
        }
    }
};

}
