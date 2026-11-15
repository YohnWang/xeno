#pragma once

#include<algorithm>
#include<array>
#include<climits>
#include<cstddef>
#include<cstdint>
#include<string_view>

namespace xeno
{

// gzip 流式解压错误码。
enum class gzip_errc : std::uint8_t
{
    none,
    bad_magic,              // ID1/ID2 不是 1f 8b
    unsupported_method,     // CM != 8
    reserved_flags,         // FLG 保留位 5..7 非 0
    header_crc,             // FHCRC 不匹配
    invalid_block_type,     // BTYPE == 3
    invalid_stored_length,  // LEN/NLEN 不互补
    invalid_huffman,        // 码树过订阅/不完整/符号数越界
    invalid_symbol,         // litlen 286/287、dist 30/31、repeat 越界
    distance_too_far,       // 距离 > 已产生历史或 > W
    crc_mismatch,           // trailer CRC32 不匹配
    size_mismatch,          // trailer ISIZE 不匹配
    input_closed,           // finished() 后仍 write
    invalid_argument,       // 非零长度 + 空指针
};

[[nodiscard]] constexpr std::string_view to_string(gzip_errc e) noexcept
{
    switch(e)
    {
    case gzip_errc::none:                  return "ok";
    case gzip_errc::bad_magic:             return "bad gzip magic";
    case gzip_errc::unsupported_method:    return "unsupported compression method";
    case gzip_errc::reserved_flags:        return "reserved gzip flags set";
    case gzip_errc::header_crc:            return "gzip header crc mismatch";
    case gzip_errc::invalid_block_type:    return "invalid deflate block type";
    case gzip_errc::invalid_stored_length: return "invalid stored block length";
    case gzip_errc::invalid_huffman:       return "invalid huffman table";
    case gzip_errc::invalid_symbol:        return "invalid deflate symbol";
    case gzip_errc::distance_too_far:      return "deflate distance too far";
    case gzip_errc::crc_mismatch:          return "gzip crc32 mismatch";
    case gzip_errc::size_mismatch:         return "gzip isize mismatch";
    case gzip_errc::input_closed:          return "write after finished";
    case gzip_errc::invalid_argument:      return "invalid argument";
    }
    return {};
}

namespace gzip_detail
{

constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for(std::size_t i = 0; i < table.size(); ++i)
    {
        std::uint32_t c = static_cast<std::uint32_t>(i);
        for(int bit = 0; bit < 8; ++bit)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        table[i] = c;
    }
    return table;
}

inline constexpr auto crc32_table = make_crc32_table();

constexpr std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data,
                                     std::size_t size) noexcept
{
    while(size-- > 0)
        crc = (crc >> 8) ^ crc32_table[static_cast<std::uint8_t>(crc ^ *data++)];
    return crc;
}

// RFC 1951 固定哈夫曼树长度。
inline constexpr auto fixed_litlen_lengths = [] {
    std::array<std::uint8_t, 288> lengths{};
    for(std::size_t i = 0; i < 144; ++i)
        lengths[i] = 8;
    for(std::size_t i = 144; i < 256; ++i)
        lengths[i] = 9;
    for(std::size_t i = 256; i < 280; ++i)
        lengths[i] = 7;
    for(std::size_t i = 280; i < 288; ++i)
        lengths[i] = 8;
    return lengths;
}();

inline constexpr auto fixed_dist_lengths = [] {
    std::array<std::uint8_t, 32> lengths{};
    lengths.fill(5);
    return lengths;
}();

inline constexpr std::array<std::uint16_t, 29> length_base{
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

inline constexpr std::array<std::uint8_t, 29> length_extra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

inline constexpr std::array<std::uint16_t, 30> distance_base{
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577,
};

inline constexpr std::array<std::uint8_t, 30> distance_extra{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

inline constexpr std::array<std::uint8_t, 19> code_length_order{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

} // namespace gzip_detail

// gzip 流式解压器。
//
// N 是压缩输入环的大小，W 是 DEFLATE 历史窗口大小（默认标准 32 KiB）。
// write() 分块写入压缩数据，read() 分块取出解压数据；两者都不阻塞。
// 对象约 32 KiB + N + 2 KiB，栈空间紧张时用 std::make_unique<gzdecore>()
// 或 static/全局对象创建。
template<std::size_t N, std::size_t W = 32768>
class gzdecoder_basic
{
    static_assert(N > 0, "gzdecoder_basic: input buffer must not be empty");
    static_assert(N <= static_cast<std::size_t>(INT_MAX),
                  "gzdecoder_basic: N must fit the int return type");
    static_assert(W > 0 && W <= 32768,
                  "gzdecoder_basic: window must be in [1, 32768]");

    enum class tree_kind : std::uint8_t
    {
        litlen,
        distance,
        codelen,
    };

    enum class step_result : std::uint8_t
    {
        progress,
        need_input,
        error,
    };

    enum class decode_result : std::uint8_t
    {
        ok,
        need_input,
        invalid,
    };

    template<std::size_t A, std::size_t P>
    struct huffman_table
    {
        // (len << 9) | symbol；0 表示该前缀没有 <= P 位的码，需要 canonical 回退。
        std::array<std::uint16_t, std::size_t{1} << P> lut_{};
        std::array<std::uint16_t, 16> counts_{};
        std::array<std::uint16_t, A> symbols_{}; // 按 (长度, 符号) 排序
        std::uint16_t symbol_count_ = 0;
        std::uint8_t max_len_ = 0;               // 0 仅对空 distance 树合法
    };

    enum class state : std::uint8_t
    {
        header_fixed,
        header_extra_len,
        header_extra_skip,
        header_name,
        header_comment,
        header_fhcrc,
        block_header,
        stored_header,
        stored_copy,
        dynamic_counts,
        dynamic_codelen,
        dynamic_lengths,
        symbol,
        length_extra,
        distance,
        distance_extra,
        match,
        trailer,
        done,
        failed,
    };

    struct read_ctx
    {
        std::uint8_t *dst;
        std::size_t limit;
        std::size_t produced;
        std::size_t crc_pos;
    };

public:
    static constexpr std::size_t input_capacity  = N;
    static constexpr std::size_t window_capacity = W;

    gzdecoder_basic() noexcept = default;

    // 写入压缩数据；返回实际接收的字节数，输入环满时返回 0，失败/已结束时返回 -1。
    [[nodiscard]] int write(const void *src, std::size_t len) noexcept
    {
        if(len == 0)
            return 0;
        if(state_ == state::failed)
            return -1;
        if(state_ == state::done)
        {
            err_ = gzip_errc::input_closed;
            state_ = state::failed;
            return -1;
        }
        if(src == nullptr)
        {
            err_ = gzip_errc::invalid_argument;
            state_ = state::failed;
            return -1;
        }

        const std::size_t space = N - in_used_;
        const std::size_t take = std::min(len, space);
        if(take == 0)
            return 0;

        const auto *p = static_cast<const std::uint8_t *>(src);
        const std::size_t first = std::min(take, N - in_tail_);
        std::copy_n(p, first, in_.data() + in_tail_);
        if(take > first)
            std::copy_n(p + first, take - first, in_.data());
        in_tail_ += take;
        if(in_tail_ >= N)
            in_tail_ -= N;
        in_used_ += take;
        return static_cast<int>(take);
    }

    // 解压到 dst；返回本次产出的字节数。0 表示暂时没有输出（需要更多输入，
    // 或已经 finished()），-1 表示错误。出错时 dst 内容不可用。
    [[nodiscard]] int read(void *dst, std::size_t len) noexcept
    {
        if(len == 0)
            return 0;
        if(state_ == state::failed)
            return -1;
        if(state_ == state::done)
            return 0;
        if(dst == nullptr)
        {
            err_ = gzip_errc::invalid_argument;
            state_ = state::failed;
            return -1;
        }

        read_ctx c{static_cast<std::uint8_t *>(dst),
                   std::min(len, static_cast<std::size_t>(INT_MAX)), 0, 0};
        while(c.produced < c.limit)
        {
            const step_result r = step(c);
            if(r == step_result::need_input)
                break;
            if(r == step_result::error)
                return -1;
        }
        flush_crc(c);
        return static_cast<int>(c.produced);
    }

    [[nodiscard]] bool finished() const noexcept { return state_ == state::done; }
    [[nodiscard]] bool failed() const noexcept { return err_ != gzip_errc::none; }
    [[nodiscard]] gzip_errc error() const noexcept { return err_; }
    [[nodiscard]] std::size_t input_size() const noexcept { return in_used_; }
    [[nodiscard]] std::size_t input_free() const noexcept { return N - in_used_; }
    [[nodiscard]] std::uint32_t output_size() const noexcept { return output_size_; }

    void reset() noexcept
    {
        in_head_ = 0;
        in_tail_ = 0;
        in_used_ = 0;
        bits_ = 0;
        bit_count_ = 0;
        win_pos_ = 0;
        win_filled_ = 0;
        hdr_ = {};
        hdr_pos_ = 0;
        flg_ = 0;
        opt_ = {};
        opt_pos_ = 0;
        extra_remaining_ = 0;
        header_crc_ = 0xFFFFFFFFu;
        bfinal_ = false;
        stored_hdr_ = {};
        stored_pos_ = 0;
        stored_remaining_ = 0;
        hlit_ = 0;
        hdist_ = 0;
        hclen_ = 0;
        dyn_phase_ = 0;
        cl_lengths_ = {};
        cl_index_ = 0;
        dyn_lengths_ = {};
        dyn_index_ = 0;
        dyn_total_ = 0;
        last_length_ = 0;
        pending_len_symbol_ = 0;
        match_length_ = 0;
        match_distance_ = 0;
        match_remaining_ = 0;
        pending_dist_symbol_ = 0;
        trailer_ = {};
        trailer_pos_ = 0;
        crc_ = 0xFFFFFFFFu;
        output_size_ = 0;
        state_ = state::header_fixed;
        err_ = gzip_errc::none;
    }

private:
    // ---- 输入环 ----
    std::array<std::uint8_t, N> in_;
    std::size_t in_head_ = 0;
    std::size_t in_tail_ = 0;
    std::size_t in_used_ = 0;

    // ---- 位蓄水池（DEFLATE 为 LSB-first）----
    std::uint64_t bits_ = 0;
    std::uint8_t bit_count_ = 0;

    // ---- 历史窗口；win_filled_ 界定有效区 ----
    std::array<std::uint8_t, W> window_;
    std::size_t win_pos_ = 0;
    std::size_t win_filled_ = 0;

    // ---- gzip 头 ----
    std::array<std::uint8_t, 10> hdr_{};
    std::uint8_t hdr_pos_ = 0;
    std::uint8_t flg_ = 0; // 逐位清掉已处理的可选字段
    std::array<std::uint8_t, 2> opt_{};
    std::uint8_t opt_pos_ = 0;
    std::size_t extra_remaining_ = 0;
    std::uint32_t header_crc_ = 0xFFFFFFFFu;

    // ---- block / dynamic header ----
    bool bfinal_ = false;
    std::array<std::uint8_t, 4> stored_hdr_{};
    std::uint8_t stored_pos_ = 0;
    std::uint16_t stored_remaining_ = 0;

    std::uint16_t hlit_ = 0;
    std::uint16_t hdist_ = 0;
    std::uint16_t hclen_ = 0;
    std::uint8_t dyn_phase_ = 0;
    std::array<std::uint8_t, 19> cl_lengths_{};
    std::uint8_t cl_index_ = 0;
    // litlen 长度放在 [0, 288)，distance 长度放在 [288, 320)。
    // 两块都先清零，避免固定字母表大小的构建读到上一块的残留。
    std::array<std::uint8_t, 288 + 32> dyn_lengths_{};
    std::uint16_t dyn_index_ = 0; // 打包序列中的位置
    std::uint16_t dyn_total_ = 0;
    std::uint8_t last_length_ = 0;

    // ---- 哈夫曼表 ----
    huffman_table<288, 9> litlen_;
    huffman_table<32, 6> dist_;
    huffman_table<19, 7> codelen_;

    // ---- 可跨 read() 恢复的符号/匹配状态 ----
    std::uint16_t pending_len_symbol_ = 0;
    std::uint16_t match_length_ = 0;
    std::uint16_t match_distance_ = 0;
    std::uint16_t match_remaining_ = 0;
    std::uint8_t pending_dist_symbol_ = 0;

    // ---- trailer / 统计 ----
    std::array<std::uint8_t, 8> trailer_{};
    std::uint8_t trailer_pos_ = 0;
    std::uint32_t crc_ = 0xFFFFFFFFu;
    std::uint32_t output_size_ = 0;

    state state_ = state::header_fixed;
    gzip_errc err_ = gzip_errc::none;

    // ---- 位/字节读取 ----
    bool refill_one() noexcept
    {
        if(in_used_ == 0)
            return false;
        bits_ |= static_cast<std::uint64_t>(in_[in_head_]) << bit_count_;
        in_head_ = (in_head_ + 1 == N) ? 0 : in_head_ + 1;
        --in_used_;
        bit_count_ = static_cast<std::uint8_t>(bit_count_ + 8);
        return true;
    }

    bool ensure_bits(std::uint8_t n) noexcept
    {
        while(bit_count_ < n)
        {
            if(!refill_one())
                return false;
        }
        return true;
    }

    void consume_bits(std::uint8_t n) noexcept
    {
        bits_ >>= n;
        bit_count_ = static_cast<std::uint8_t>(bit_count_ - n);
    }

    // 字节流读取：对齐后蓄水池里的整字节是真实的流字节（可能已预取 trailer），
    // 必须先于输入环消费。
    int get_byte_stream() noexcept
    {
        if(bit_count_ >= 8)
        {
            const std::uint8_t b = static_cast<std::uint8_t>(bits_ & 0xFFu);
            bits_ >>= 8;
            bit_count_ = static_cast<std::uint8_t>(bit_count_ - 8);
            return b;
        }
        if(in_used_ == 0)
            return -1;
        const std::uint8_t b = in_[in_head_];
        in_head_ = (in_head_ + 1 == N) ? 0 : in_head_ + 1;
        --in_used_;
        return b;
    }

    void align_byte() noexcept
    {
        const std::uint8_t drop = static_cast<std::uint8_t>(bit_count_ & 7u);
        bits_ >>= drop;
        bit_count_ = static_cast<std::uint8_t>(bit_count_ - drop);
    }

    // ---- 输出 ----
    void emit(std::uint8_t b, read_ctx &c) noexcept
    {
        c.dst[c.produced++] = b;
        window_[win_pos_] = b;
        win_pos_ = (win_pos_ + 1 == W) ? 0 : win_pos_ + 1;
        if(win_filled_ < W)
            ++win_filled_;
        ++output_size_;
    }

    void flush_crc(read_ctx &c) noexcept
    {
        if(c.produced != c.crc_pos)
        {
            crc_ = gzip_detail::crc32_update(crc_, c.dst + c.crc_pos,
                                             c.produced - c.crc_pos);
            c.crc_pos = c.produced;
        }
    }

    step_result fail(gzip_errc e) noexcept
    {
        err_ = e;
        state_ = state::failed;
        return step_result::error;
    }

    static std::uint32_t le32(const std::uint8_t *p) noexcept
    {
        return static_cast<std::uint32_t>(p[0])
             | (static_cast<std::uint32_t>(p[1]) << 8)
             | (static_cast<std::uint32_t>(p[2]) << 16)
             | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    // ---- 哈夫曼 ----
    template<std::size_t A, std::size_t P>
    static bool build_table(huffman_table<A, P> &t, const std::uint8_t *lengths,
                            tree_kind kind) noexcept
    {
        t.counts_.fill(0);
        t.lut_.fill(0);
        t.symbol_count_ = 0;
        t.max_len_ = 0;

        std::uint8_t max_len = 0;
        bool has_eob = false;
        for(std::size_t s = 0; s < A; ++s)
        {
            const std::uint8_t l = lengths[s];
            if(l > 15)
                return false;
            if(l != 0)
            {
                ++t.counts_[l];
                if(l > max_len)
                    max_len = l;
            }
            if(kind == tree_kind::litlen && s == 256 && l != 0)
                has_eob = true;
        }

        t.max_len_ = max_len;

        if(kind == tree_kind::litlen && !has_eob)
            return false;
        if(kind == tree_kind::codelen && max_len == 0)
            return false;

        int left = 1;
        for(std::size_t l = 1; l <= 15; ++l)
        {
            left <<= 1;
            left -= static_cast<int>(t.counts_[l]);
            if(left < 0)
                return false;
        }

        if(left > 0)
        {
            if(kind == tree_kind::codelen)
                return false;
            if(kind == tree_kind::distance && max_len == 0)
                return true; // 空 distance 树：本 block 不允许匹配
            if(max_len != 1)
                return false;
        }

        if(max_len == 0)
            return true;

        std::array<std::uint16_t, 16> next{};
        std::uint32_t code = 0;
        for(std::size_t l = 1; l <= 15; ++l)
        {
            code = (code + t.counts_[l - 1]) << 1;
            next[l] = static_cast<std::uint16_t>(code);
        }

        std::uint16_t k = 0;
        for(std::size_t l = 1; l <= 15; ++l)
        {
            for(std::size_t s = 0; s < A; ++s)
            {
                if(lengths[s] == static_cast<std::uint8_t>(l))
                    t.symbols_[k++] = static_cast<std::uint16_t>(s);
            }
        }
        t.symbol_count_ = k;

        constexpr std::uint16_t lut_size =
            static_cast<std::uint16_t>(std::size_t{1} << P);
        for(std::size_t s = 0; s < A; ++s)
        {
            const std::uint8_t l = lengths[s];
            if(l == 0 || l > P)
                continue;
            std::uint16_t c = next[l]++;
            std::uint16_t rev = 0;
            for(std::uint8_t i = 0; i < l; ++i)
            {
                rev = static_cast<std::uint16_t>((rev << 1) | (c & 1u));
                c = static_cast<std::uint16_t>(c >> 1);
            }
            const std::uint16_t entry = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(l) << 9) | static_cast<std::uint16_t>(s));
            const std::uint16_t step =
                static_cast<std::uint16_t>(std::uint16_t{1} << l);
            for(std::uint16_t i = rev; i < lut_size;
                i = static_cast<std::uint16_t>(i + step))
            {
                t.lut_[i] = entry;
            }
        }
        return true;
    }

    template<std::size_t A, std::size_t P>
    decode_result decode_symbol(const huffman_table<A, P> &t,
                                std::uint16_t &sym) noexcept
    {
        if(t.max_len_ == 0)
            return decode_result::invalid;
        if(!ensure_bits(t.max_len_))
            return decode_result::need_input;

        const std::uint16_t entry = t.lut_[
            static_cast<std::size_t>(bits_ & ((std::size_t{1} << P) - 1))];
        if(entry != 0)
        {
            consume_bits(static_cast<std::uint8_t>(entry >> 9));
            sym = static_cast<std::uint16_t>(entry & 0x1FFu);
            return decode_result::ok;
        }

        // canonical 回退。已保证 max_len_ 位可用，循环上限不能写死 15。
        std::int32_t code = 0;
        std::int32_t first = 0;
        std::int32_t index = 0;
        for(std::uint8_t l = 1; l <= t.max_len_; ++l)
        {
            code |= static_cast<std::int32_t>(bits_ & 1u);
            consume_bits(1);
            if(code - first < static_cast<std::int32_t>(t.counts_[l]))
            {
                sym = t.symbols_[static_cast<std::size_t>(index + (code - first))];
                return decode_result::ok;
            }
            index += static_cast<std::int32_t>(t.counts_[l]);
            first = (first + static_cast<std::int32_t>(t.counts_[l])) << 1;
            code <<= 1;
        }
        return decode_result::invalid;
    }

    // ---- 状态机 ----
    step_result step(read_ctx &c) noexcept
    {
        switch(state_)
        {
        case state::header_fixed:       return step_header_fixed();
        case state::header_extra_len:   return step_header_extra_len();
        case state::header_extra_skip:  return step_header_extra_skip();
        case state::header_name:
        case state::header_comment:     return step_header_string();
        case state::header_fhcrc:       return step_header_fhcrc();
        case state::block_header:       return step_block_header();
        case state::stored_header:      return step_stored_header();
        case state::stored_copy:        return step_stored_copy(c);
        case state::dynamic_counts:     return step_dynamic_counts();
        case state::dynamic_codelen:    return step_dynamic_codelen();
        case state::dynamic_lengths:    return step_dynamic_lengths();
        case state::symbol:             return step_symbol(c);
        case state::length_extra:       return step_length_extra();
        case state::distance:           return step_distance();
        case state::distance_extra:     return step_distance_extra();
        case state::match:              return step_match(c);
        case state::trailer:            return step_trailer(c);
        case state::done:               return step_result::need_input;
        case state::failed:             return step_result::error;
        }
        return step_result::error;
    }

    state next_header_state() noexcept
    {
        if((flg_ & 0x04u) != 0)
        {
            flg_ = static_cast<std::uint8_t>(flg_ & ~0x04u); // FEXTRA
            opt_pos_ = 0;
            return state::header_extra_len;
        }
        if((flg_ & 0x08u) != 0)
        {
            flg_ = static_cast<std::uint8_t>(flg_ & ~0x08u); // FNAME
            return state::header_name;
        }
        if((flg_ & 0x10u) != 0)
        {
            flg_ = static_cast<std::uint8_t>(flg_ & ~0x10u); // FCOMMENT
            return state::header_comment;
        }
        if((flg_ & 0x02u) != 0)
        {
            flg_ = static_cast<std::uint8_t>(flg_ & ~0x02u); // FHCRC
            opt_pos_ = 0;
            return state::header_fhcrc;
        }
        return state::block_header; // FTEXT 只提示内容为文本，无需处理
    }

    step_result step_header_fixed() noexcept
    {
        const int b = get_byte_stream();
        if(b < 0)
            return step_result::need_input;
        if(hdr_pos_ == 0 && b != 0x1F)
            return fail(gzip_errc::bad_magic);
        if(hdr_pos_ == 1 && b != 0x8B)
            return fail(gzip_errc::bad_magic);
        hdr_[hdr_pos_++] = static_cast<std::uint8_t>(b);
        if(hdr_pos_ < hdr_.size())
            return step_result::progress;

        if(hdr_[2] != 8)
            return fail(gzip_errc::unsupported_method);
        if((hdr_[3] & 0xE0u) != 0)
            return fail(gzip_errc::reserved_flags);
        flg_ = hdr_[3];
        header_crc_ = gzip_detail::crc32_update(0xFFFFFFFFu, hdr_.data(),
                                                hdr_.size());
        state_ = next_header_state();
        return step_result::progress;
    }

    step_result step_header_extra_len() noexcept
    {
        const int b = get_byte_stream();
        if(b < 0)
            return step_result::need_input;
        opt_[opt_pos_++] = static_cast<std::uint8_t>(b);
        header_crc_ = gzip_detail::crc32_update(header_crc_,
                                                opt_.data() + opt_pos_ - 1, 1);
        if(opt_pos_ < opt_.size())
            return step_result::progress;
        extra_remaining_ = static_cast<std::size_t>(opt_[0])
                         | (static_cast<std::size_t>(opt_[1]) << 8);
        opt_pos_ = 0;
        state_ = state::header_extra_skip;
        return step_result::progress;
    }

    step_result step_header_extra_skip() noexcept
    {
        if(extra_remaining_ == 0)
        {
            state_ = next_header_state();
            return step_result::progress;
        }
        const int b = get_byte_stream();
        if(b < 0)
            return step_result::need_input;
        const std::uint8_t v = static_cast<std::uint8_t>(b);
        header_crc_ = gzip_detail::crc32_update(header_crc_, &v, 1);
        --extra_remaining_;
        if(extra_remaining_ == 0)
            state_ = next_header_state();
        return step_result::progress;
    }

    step_result step_header_string() noexcept
    {
        const int b = get_byte_stream();
        if(b < 0)
            return step_result::need_input;
        const std::uint8_t v = static_cast<std::uint8_t>(b);
        header_crc_ = gzip_detail::crc32_update(header_crc_, &v, 1);
        if(v == 0)
            state_ = next_header_state();
        return step_result::progress;
    }

    step_result step_header_fhcrc() noexcept
    {
        const int b = get_byte_stream();
        if(b < 0)
            return step_result::need_input;
        opt_[opt_pos_++] = static_cast<std::uint8_t>(b);
        if(opt_pos_ < opt_.size())
            return step_result::progress;

        const std::uint16_t expected =
            static_cast<std::uint16_t>((header_crc_ ^ 0xFFFFFFFFu) & 0xFFFFu);
        const std::uint16_t actual = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(opt_[0])
            | (static_cast<std::uint16_t>(opt_[1]) << 8));
        opt_pos_ = 0;
        if(actual != expected)
            return fail(gzip_errc::header_crc);
        state_ = state::block_header;
        return step_result::progress;
    }

    step_result step_block_header() noexcept
    {
        if(!ensure_bits(3))
            return step_result::need_input;
        const std::uint32_t v = static_cast<std::uint32_t>(bits_ & 0x07u);
        consume_bits(3);
        bfinal_ = (v & 1u) != 0;

        const std::uint32_t type = (v >> 1) & 3u;
        if(type == 0)
        {
            align_byte();
            stored_pos_ = 0;
            state_ = state::stored_header;
        }
        else if(type == 1)
        {
            if(!build_table(litlen_, gzip_detail::fixed_litlen_lengths.data(),
                            tree_kind::litlen)
               || !build_table(dist_, gzip_detail::fixed_dist_lengths.data(),
                               tree_kind::distance))
            {
                return fail(gzip_errc::invalid_huffman);
            }
            state_ = state::symbol;
        }
        else if(type == 2)
        {
            dyn_phase_ = 0;
            state_ = state::dynamic_counts;
        }
        else
        {
            return fail(gzip_errc::invalid_block_type);
        }
        return step_result::progress;
    }

    step_result step_stored_header() noexcept
    {
        const int b = get_byte_stream();
        if(b < 0)
            return step_result::need_input;
        stored_hdr_[stored_pos_++] = static_cast<std::uint8_t>(b);
        if(stored_pos_ < stored_hdr_.size())
            return step_result::progress;

        const std::uint16_t len = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(stored_hdr_[0])
            | (static_cast<std::uint16_t>(stored_hdr_[1]) << 8));
        const std::uint16_t nlen = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(stored_hdr_[2])
            | (static_cast<std::uint16_t>(stored_hdr_[3]) << 8));
        if(nlen != static_cast<std::uint16_t>(len ^ 0xFFFFu))
            return fail(gzip_errc::invalid_stored_length);
        stored_remaining_ = len;
        stored_pos_ = 0;
        state_ = state::stored_copy;
        return step_result::progress;
    }

    step_result step_stored_copy(read_ctx &c) noexcept
    {
        while(stored_remaining_ != 0 && c.produced < c.limit)
        {
            const int b = get_byte_stream();
            if(b < 0)
                return step_result::need_input;
            emit(static_cast<std::uint8_t>(b), c);
            --stored_remaining_;
        }
        if(stored_remaining_ == 0)
            state_ = bfinal_ ? state::trailer : state::block_header;
        return step_result::progress;
    }

    step_result step_dynamic_counts() noexcept
    {
        if(dyn_phase_ == 0)
        {
            if(!ensure_bits(5))
                return step_result::need_input;
            const std::uint32_t v = static_cast<std::uint32_t>(bits_ & 0x1Fu);
            consume_bits(5);
            if(v + 257u > 286u)
                return fail(gzip_errc::invalid_huffman);
            hlit_ = static_cast<std::uint16_t>(v + 257u);
            dyn_phase_ = 1;
            return step_result::progress;
        }
        if(dyn_phase_ == 1)
        {
            if(!ensure_bits(5))
                return step_result::need_input;
            const std::uint32_t v = static_cast<std::uint32_t>(bits_ & 0x1Fu);
            consume_bits(5);
            if(v + 1u > 30u)
                return fail(gzip_errc::invalid_huffman);
            hdist_ = static_cast<std::uint16_t>(v + 1u);
            dyn_phase_ = 2;
            return step_result::progress;
        }

        if(!ensure_bits(4))
            return step_result::need_input;
        const std::uint32_t v = static_cast<std::uint32_t>(bits_ & 0x0Fu);
        consume_bits(4);
        hclen_ = static_cast<std::uint16_t>(v + 4u);
        cl_lengths_.fill(0);
        cl_index_ = 0;
        dyn_phase_ = 0;
        state_ = state::dynamic_codelen;
        return step_result::progress;
    }

    step_result step_dynamic_codelen() noexcept
    {
        while(cl_index_ < hclen_)
        {
            if(!ensure_bits(3))
                return step_result::need_input;
            const std::uint8_t v = static_cast<std::uint8_t>(bits_ & 0x07u);
            consume_bits(3);
            cl_lengths_[gzip_detail::code_length_order[cl_index_]] = v;
            ++cl_index_;
        }

        if(!build_table(codelen_, cl_lengths_.data(), tree_kind::codelen))
            return fail(gzip_errc::invalid_huffman);

        dyn_lengths_.fill(0);
        dyn_index_ = 0;
        dyn_total_ = static_cast<std::uint16_t>(hlit_ + hdist_);
        last_length_ = 0;
        state_ = state::dynamic_lengths;
        return step_result::progress;
    }

    step_result step_dynamic_lengths() noexcept
    {
        while(dyn_index_ < dyn_total_)
        {
            // 一次保证 codelen 符号 + 最坏 7 个重复 extra bits 都可用，
            // 避免符号已消费而 extra bits 不足时丢失位置。
            if(!ensure_bits(static_cast<std::uint8_t>(codelen_.max_len_ + 7u)))
                return step_result::need_input;

            std::uint16_t sym = 0;
            const decode_result r = decode_symbol(codelen_, sym);
            if(r == decode_result::need_input)
                return step_result::need_input;
            if(r == decode_result::invalid)
                return fail(gzip_errc::invalid_huffman);

            if(sym <= 15)
            {
                last_length_ = static_cast<std::uint8_t>(sym);
                store_length(last_length_);
                continue;
            }

            std::uint8_t extra = 0;
            std::uint16_t repeat = 0;
            if(sym == 16)
            {
                if(dyn_index_ == 0)
                    return fail(gzip_errc::invalid_symbol);
                extra = 2;
                repeat = 3;
            }
            else if(sym == 17)
            {
                extra = 3;
                repeat = 3;
            }
            else if(sym == 18)
            {
                extra = 7;
                repeat = 11;
            }
            else
            {
                return fail(gzip_errc::invalid_symbol);
            }

            const std::uint64_t mask = (std::uint64_t{1} << extra) - 1;
            repeat = static_cast<std::uint16_t>(
                repeat + static_cast<std::uint16_t>(bits_ & mask));
            consume_bits(extra);

            if(static_cast<std::uint32_t>(dyn_index_) + repeat > dyn_total_)
                return fail(gzip_errc::invalid_symbol);

            const std::uint8_t fill = (sym == 16)
                ? last_length_ : std::uint8_t{0};
            for(std::uint16_t i = 0; i < repeat; ++i)
                store_length(fill);
            last_length_ = fill;
        }

        if(!build_table(litlen_, dyn_lengths_.data(), tree_kind::litlen))
            return fail(gzip_errc::invalid_huffman);
        if(!build_table(dist_, dyn_lengths_.data() + 288, tree_kind::distance))
            return fail(gzip_errc::invalid_huffman);
        state_ = state::symbol;
        return step_result::progress;
    }

    void store_length(std::uint8_t value) noexcept
    {
        if(dyn_index_ < hlit_)
            dyn_lengths_[dyn_index_] = value;
        else
            dyn_lengths_[288u + (dyn_index_ - hlit_)] = value;
        ++dyn_index_;
    }

    step_result step_symbol(read_ctx &c) noexcept
    {
        std::uint16_t sym = 0;
        const decode_result r = decode_symbol(litlen_, sym);
        if(r == decode_result::need_input)
            return step_result::need_input;
        if(r == decode_result::invalid)
            return fail(gzip_errc::invalid_huffman);

        if(sym < 256)
        {
            emit(static_cast<std::uint8_t>(sym), c);
            return step_result::progress;
        }
        if(sym == 256)
        {
            state_ = bfinal_ ? state::trailer : state::block_header;
            return step_result::progress;
        }
        if(sym <= 285)
        {
            pending_len_symbol_ = static_cast<std::uint16_t>(sym - 257);
            state_ = state::length_extra;
            return step_result::progress;
        }
        return fail(gzip_errc::invalid_symbol);
    }

    step_result step_length_extra() noexcept
    {
        const std::uint8_t extra = gzip_detail::length_extra[pending_len_symbol_];
        if(!ensure_bits(extra))
            return step_result::need_input;
        const std::uint64_t mask = (std::uint64_t{1} << extra) - 1;
        const std::uint16_t add = static_cast<std::uint16_t>(bits_ & mask);
        consume_bits(extra);
        match_length_ = static_cast<std::uint16_t>(
            gzip_detail::length_base[pending_len_symbol_] + add);
        state_ = state::distance;
        return step_result::progress;
    }

    step_result step_distance() noexcept
    {
        std::uint16_t sym = 0;
        const decode_result r = decode_symbol(dist_, sym);
        if(r == decode_result::need_input)
            return step_result::need_input;
        if(r == decode_result::invalid)
            return fail(gzip_errc::invalid_symbol);
        if(sym > 29)
            return fail(gzip_errc::invalid_symbol);
        pending_dist_symbol_ = static_cast<std::uint8_t>(sym);
        state_ = state::distance_extra;
        return step_result::progress;
    }

    step_result step_distance_extra() noexcept
    {
        const std::uint8_t extra = gzip_detail::distance_extra[pending_dist_symbol_];
        if(!ensure_bits(extra))
            return step_result::need_input;
        const std::uint64_t mask = (std::uint64_t{1} << extra) - 1;
        const std::uint16_t add = static_cast<std::uint16_t>(bits_ & mask);
        consume_bits(extra);
        match_distance_ = static_cast<std::uint16_t>(
            gzip_detail::distance_base[pending_dist_symbol_] + add);
        if(match_distance_ == 0 || match_distance_ > win_filled_
           || match_distance_ > W)
        {
            return fail(gzip_errc::distance_too_far);
        }
        match_remaining_ = match_length_;
        state_ = state::match;
        return step_result::progress;
    }

    step_result step_match(read_ctx &c) noexcept
    {
        while(match_remaining_ != 0 && c.produced < c.limit)
        {
            const std::size_t src = win_pos_ >= match_distance_
                ? win_pos_ - match_distance_
                : win_pos_ + W - match_distance_;
            emit(window_[src], c);
            --match_remaining_;
        }
        if(match_remaining_ == 0)
            state_ = state::symbol;
        return step_result::progress;
    }

    step_result step_trailer(read_ctx &c) noexcept
    {
        align_byte();
        while(trailer_pos_ < trailer_.size())
        {
            const int b = get_byte_stream();
            if(b < 0)
                return step_result::need_input;
            trailer_[trailer_pos_++] = static_cast<std::uint8_t>(b);
        }

        flush_crc(c);
        const std::uint32_t stored_crc = le32(trailer_.data());
        const std::uint32_t stored_size = le32(trailer_.data() + 4);
        if((crc_ ^ 0xFFFFFFFFu) != stored_crc)
            return fail(gzip_errc::crc_mismatch);
        if(output_size_ != stored_size)
            return fail(gzip_errc::size_mismatch);
        state_ = state::done;
        return step_result::progress;
    }
};

using gzdecore  = gzdecoder_basic<4096>;
using gzdecoder = gzdecore;

} // namespace xeno
