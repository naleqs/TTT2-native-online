#pragma once

// Little-endian serialization for netplay packets. Explicit byte order so the wire format
// does not depend on the host compiler's struct layout.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tt2::netplay::wire
{
    class Writer
    {
    public:
        void u8(uint8_t v) { buf_.push_back(v); }
        void u16(uint16_t v) { put(v, 2); }
        void u32(uint32_t v) { put(v, 4); }
        void i32(int32_t v) { put(uint32_t(v), 4); }
        void u64(uint64_t v) { put(v, 8); }

        const uint8_t* data() const { return buf_.data(); }
        size_t size() const { return buf_.size(); }

    private:
        void put(uint64_t v, int n)
        {
            for (int i = 0; i < n; i++)
                buf_.push_back(uint8_t(v >> (8 * i)));
        }

        std::vector<uint8_t> buf_;
    };

    class Reader
    {
    public:
        Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

        uint8_t u8() { return uint8_t(get(1)); }
        uint16_t u16() { return uint16_t(get(2)); }
        uint32_t u32() { return uint32_t(get(4)); }
        int32_t i32() { return int32_t(uint32_t(get(4))); }
        uint64_t u64() { return get(8); }

        // False once a read went past the end (all later reads return 0).
        bool ok() const { return ok_; }

    private:
        uint64_t get(int n)
        {
            if (end_ - p_ < n)
            {
                ok_ = false;
                p_ = end_;
                return 0;
            }
            uint64_t v = 0;
            for (int i = 0; i < n; i++)
                v |= uint64_t(p_[i]) << (8 * i);
            p_ += n;
            return v;
        }

        const uint8_t* p_;
        const uint8_t* end_;
        bool ok_ = true;
    };
}
