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

// YMODEM-1K 接收端（单文件）。
//
// IO 由派生类提供：
//   int  try_getchar(); // 非阻塞，返回 0..255，负数表示暂无数据
//   void putchar(int);  // 发送单字节
// 通过 deducing this + 继承在编译期绑定，不检查返回值以外的 IO 状态。
// 超时与重发 'C' 由调用方计时，类内不做等待。
class ymodem
{
public:
    static constexpr std::size_t kUnknown = std::numeric_limits<std::size_t>::max();

    // 传输非正常结束。start_link()/receive()/finish_batch() 在终止状态下抛出。
    struct error : std::exception
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer error";
        }
    };

    // 本地 cancel()、远端 CAN/CTRL-C，或中止后再次调用 start_link()/receive()/finish_batch()。
    struct cancelled : error
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer cancelled";
        }
    };

    // 握手阶段收到 EOT、空文件头，或数据未收完就收到 EOT。
    struct failed : error
    {
        const char *what() const noexcept override
        {
            return "ymodem: transfer failed";
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
    // true 表示连接成功；false 表示继续轮询（已发 'C' 或 Block 0 未收完）。
    bool start_link(this auto &&self)
    {
        auto get = [&]() -> int
        {
            return self.try_getchar();
        };
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };

        switch(self.ph_)
        {
        case phase::idle:
            put(CRC_REQ);
            self.ph_ = phase::wait_block0;
            self.reset_frame();
            return false;
        case phase::wait_block0:
            return self.drive_block0(get, put);
        case phase::data_phase:
        case phase::awaiting_eot:
            return true;
        case phase::finished:
            self.throw_if_ended();
            return true;
        }
        return true;
    }

    // ---- 接收 ----
    // 返回 0 表示 EOT 已确认；返回 kUnknown 表示继续；其余为剩余字节数。
    // 远端中止抛 cancelled，提前收到 EOT 抛 failed。
    std::size_t receive(this auto &&self, auto &&f)
    {
        auto get = [&]() -> int
        {
            return self.try_getchar();
        };
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };

        if(self.ph_ == phase::idle)
        {
            (void)self.start_link();
            return kUnknown;
        }
        if(self.ph_ == phase::wait_block0)
        {
            (void)self.drive_block0(get, put);
            return kUnknown;
        }
        self.throw_if_ended();
        if(self.ph_ == phase::finished)
            return 0;

        auto r = self.read_frame(get, put);
        switch(r)
        {
        case read_result::pending:
        case read_result::error:
        case read_result::duplicate:
            return kUnknown;
        case read_result::cancelled:
            send_cancel(put);
            self.ph_ = phase::finished;
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case read_result::eot:
            put(ACK);
            self.ph_ = phase::finished;
            if(self.filesize_ != kUnknown && self.received_ < self.filesize_)
            {
                self.end_ = end_state::failed;
                throw failed{};
            }
            return 0;
        case read_result::ok:
            break;
        }

        std::size_t data_len = self.body_len_ - CRC_SIZE;
        std::span<std::byte> payload{self.body_.data(), data_len};
        std::size_t deliver = payload.size();
        if(self.filesize_ != kUnknown)
            deliver = std::min(deliver, self.filesize_ - self.received_);

        // 先消费帧再回调：回调抛异常时不会把同一帧再交付一次，也不回 ACK，
        // 发送方会重发该帧。
        self.reset_frame();
        if(deliver != 0)
            f(payload.first(deliver));
        if(self.filesize_ != kUnknown)
            self.received_ += deliver;
        self.data_started_ = true;
        self.expected_seq_ = static_cast<std::uint8_t>(self.expected_seq_ + 1);
        put(ACK);

        if(self.filesize_ != kUnknown && self.received_ >= self.filesize_)
        {
            self.ph_ = phase::awaiting_eot;
            return kUnknown;
        }
        if(self.filesize_ == kUnknown)
            return kUnknown;
        return self.filesize_ - self.received_;
    }

    // ---- 完成 YMODEM 批处理结束握手 ----
    // 在 receive() 返回 0 且未中止后调用，非阻塞轮询：
    // 第一次调用发 'C'，收到空 Block 0 并 ACK 后返回 true。
    // 发送方还有下一个文件（非空 Block 0）时发 CAN 并抛 failed。
    bool finish_batch(this auto &&self)
    {
        if(self.ph_ != phase::finished)
            return false;
        self.throw_if_ended();

        auto get = [&]() -> int
        {
            return self.try_getchar();
        };
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };

        if(self.batch_ == batch_state::done)
            return true;
        if(self.batch_ == batch_state::none)
        {
            put(CRC_REQ);
            self.batch_ = batch_state::waiting;
            self.expected_seq_ = 0;
            self.reset_frame();
            return false;
        }

        auto r = self.read_frame(get, put);
        switch(r)
        {
        case read_result::pending:
        case read_result::error:
        case read_result::duplicate:
            return false;
        case read_result::cancelled:
            send_cancel(put);
            self.end_ = end_state::cancelled;
            throw cancelled{};
        case read_result::eot:
            // EOT ACK 丢失后发送方重发 EOT：再 ACK 并补发 'C'。
            put(ACK);
            put(CRC_REQ);
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
            put(ACK);
            self.batch_ = batch_state::done;
            return true;
        }

        // 单文件接收端不能再收下一个文件，中止整个批处理。
        send_cancel(put);
        self.end_ = end_state::failed;
        throw failed{};
    }

    // ---- 本地用户按 Ctrl+C 时调用：向发送方发 CAN，并抛 cancelled ----
    // idle/finished 状态下是空操作，不抛异常。
    void cancel(this auto &&self)
    {
        if(self.ph_ == phase::idle || self.ph_ == phase::finished)
            return;
        auto put = [&](std::uint8_t c)
        {
            self.putchar(c);
        };
        self.ph_ = phase::finished;
        self.end_ = end_state::cancelled;
        self.reset_frame();
        send_cancel(put);
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
        failed
    };

    enum class batch_state
    {
        none,
        waiting,
        done
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

    void reset_frame()
    {
        hdr_pos_ = body_pos_ = body_len_ = 0;
    }

    void throw_if_ended() const
    {
        if(end_ == end_state::cancelled)
            throw cancelled{};
        if(end_ == end_state::failed)
            throw failed{};
    }

    static void send_cancel(auto &&put)
    {
        for(int i = 0; i < CAN_BURST; ++i)
            put(CAN);
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

    read_result read_frame(auto &&get, auto &&put)
    {
        while(hdr_pos_ < HDR_SIZE)
        {
            int c = get();
            if(c < 0)
                return read_result::pending;
            hdr_[hdr_pos_++] = static_cast<std::uint8_t>(c);

            // EOT/CAN 是单字节信号，必须在帧头首字节立即判断。
            // 数据段里的 0x18/0x03 是合法数据，只有这里检测。
            if(hdr_pos_ == 1)
            {
                switch(hdr_[0])
                {
                case EOT:
                    reset_frame();
                    return read_result::eot;
                case CAN:
                case CTRL_C:
                    reset_frame();
                    return read_result::cancelled;
                case SOH:
                    body_len_ = PKT_128 + CRC_SIZE;
                    break;
                case STX:
                    body_len_ = PKT_1K + CRC_SIZE;
                    break;
                default:
                    put(NAK);
                    reset_frame();
                    return read_result::error;
                }
            }
        }

        while(body_pos_ < body_len_)
        {
            int c = get();
            if(c < 0)
                return read_result::pending;
            body_[body_pos_++] = static_cast<std::byte>(c);
        }

        std::size_t data_len = body_len_ - CRC_SIZE;
        std::span<const std::byte> payload{body_.data(), data_len};
        std::uint16_t recv_crc =
            (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(body_[data_len])) << 8) |
            std::to_integer<std::uint8_t>(body_[data_len + 1]);

        std::uint8_t seq = hdr_[1], nseq = hdr_[2];
        if(crc16(payload) != recv_crc || nseq != static_cast<std::uint8_t>(~seq))
        {
            put(NAK);
            reset_frame();
            return read_result::error;
        }
        if(seq == expected_seq_)
            return read_result::ok;
        if(ph_ != phase::wait_block0 &&
           seq == static_cast<std::uint8_t>(expected_seq_ - 1))
        {
            put(ACK);
            // Block 0 重传：发送方还在等 'C'，补发一次。
            if(seq == 0 && !data_started_)
                put(CRC_REQ);
            reset_frame();
            return read_result::duplicate;
        }
        put(NAK);
        reset_frame();
        return read_result::error;
    }

    bool drive_block0(auto &&get, auto &&put)
    {
        auto r = read_frame(get, put);
        switch(r)
        {
        case read_result::pending:
        case read_result::error:
        case read_result::duplicate:
            return false;
        case read_result::cancelled:
            send_cancel(put);
            ph_ = phase::finished;
            end_ = end_state::cancelled;
            throw cancelled{};
        case read_result::eot:
            put(ACK);
            ph_ = phase::finished;
            end_ = end_state::failed;
            throw failed{};
        case read_result::ok:
            break;
        }

        std::size_t data_len = body_len_ - CRC_SIZE;
        std::span<const std::byte> payload{body_.data(), data_len};
        parse_block0(payload);
        reset_frame();

        if(filename_.empty())
        {
            // 空文件头是批处理的结束标记；单文件接收视为失败。
            put(ACK);
            ph_ = phase::finished;
            end_ = end_state::failed;
            throw failed{};
        }

        expected_seq_ = 1;
        ph_ = phase::data_phase;
        put(ACK);
        put(CRC_REQ);
        return true;
    }
};

}
