#include "snapshot.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace tt2::rollback
{
    SnapshotRing::SnapshotRing(uint8_t* base, size_t size, int depth)
        : base_(base), size_(size), depth_(depth), ring_(size_t(depth) + 1)
    {
        watch_buffer_.resize(64 * 1024);
    }

    SnapshotRing::~SnapshotRing() = default;

    void SnapshotRing::track(uint32_t begin, uint32_t end)
    {
        tracked_.push_back({ begin / uint32_t(kPage), uint32_t((uint64_t(end) + kPage - 1) / kPage) });
    }

    void SnapshotRing::exclude(uint32_t begin, uint32_t end)
    {
        // Partially excluded pages stay tracked: excluding them would lose the rest of the page.
        const uint32_t first = uint32_t((uint64_t(begin) + kPage - 1) / kPage);
        const uint32_t last = end / uint32_t(kPage);
        if (first < last)
            excluded_.push_back({ first, last });
    }

    bool SnapshotRing::is_tracked(uint32_t page) const
    {
        for (auto [b, e] : excluded_)
            if (page >= b && page < e)
                return false;
        for (auto [b, e] : tracked_)
            if (page >= b && page < e)
                return true;
        return false;
    }

    uint8_t* SnapshotRing::shadow_page(uint32_t page)
    {
        auto& slot = shadow_[page];
        if (!slot)
        {
            // A page first seen now was zero when the snapshot began (fresh commit).
            slot = std::make_unique<uint8_t[]>(kPage);
            std::memset(slot.get(), 0, kPage);
        }
        return slot.get();
    }

    void SnapshotRing::collect_dirty(std::vector<uint32_t>& out)
    {
        for (auto [b, e] : tracked_)
        {
            uint8_t* start = base_ + size_t(b) * kPage;
            size_t length = size_t(e - b) * kPage;
            for (;;)
            {
                ULONG_PTR count = watch_buffer_.size();
                DWORD granularity = 0;
                if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, start, length, watch_buffer_.data(), &count, &granularity) != 0)
                    break; // range not (fully) committed yet: nothing written there
                for (ULONG_PTR i = 0; i < count; i++)
                {
                    const uint32_t page = uint32_t((static_cast<uint8_t*>(watch_buffer_[i]) - base_) / kPage);
                    if (is_tracked(page))
                        out.push_back(page);
                }
                if (count < watch_buffer_.size())
                    break;
                // Buffer full: the reset already happened for the reported pages, continue
                // after the last one.
                uint8_t* next = static_cast<uint8_t*>(watch_buffer_[count - 1]) + kPage;
                length -= size_t(next - start);
                start = next;
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void SnapshotRing::begin(int frame)
    {
        shadow_.clear();
        for (auto& r : ring_)
            r = Record{};

        // Committed pages of the tracked ranges become the shadow; reset the write watch so
        // the next save sees only what frame `frame` writes.
        for (auto [b, e] : tracked_)
        {
            for (uint32_t page = b; page < e; page++)
            {
                if (!is_tracked(page))
                    continue;
                MEMORY_BASIC_INFORMATION mbi;
                uint8_t* p = base_ + size_t(page) * kPage;
                if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
                {
                    // skip the rest of this uncommitted region in one go
                    const size_t region_end = (static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - base_) / kPage;
                    if (region_end > page + 1)
                        page = uint32_t(std::min<size_t>(region_end, e)) - 1;
                    continue;
                }
                auto copy = std::make_unique<uint8_t[]>(kPage);
                std::memcpy(copy.get(), p, kPage);
                shadow_[page] = std::move(copy);
            }
            ResetWriteWatch(base_ + size_t(b) * kPage, size_t(e - b) * kPage);
        }

        active_ = true;
        newest_ = oldest_ = frame;
    }

    void SnapshotRing::save(int frame)
    {
        if (!active_)
            return;

        std::vector<uint32_t> dirty;
        collect_dirty(dirty);
        pages_last_save_ = dirty.size();

        // The record of the previous frame keeps what these pages held then.
        Record& rec = ring_[size_t(frame) % ring_.size()];
        rec.frame = frame - 1;
        rec.pages = dirty;
        rec.data.resize(dirty.size() * kPage);
        for (size_t i = 0; i < dirty.size(); i++)
        {
            uint8_t* shadow = shadow_page(dirty[i]);
            std::memcpy(rec.data.data() + i * kPage, shadow, kPage);
            std::memcpy(shadow, base_ + size_t(dirty[i]) * kPage, kPage);
        }

        newest_ = frame;
        oldest_ = std::max(oldest_, frame - depth_);
    }

    bool SnapshotRing::load(int frame)
    {
        if (!active_ || frame < oldest_ || frame > newest_)
            return false;

        // 1. Writes since the newest save: back to the shadow (= state of newest_).
        std::vector<uint32_t> dirty;
        collect_dirty(dirty);
        for (uint32_t page : dirty)
            std::memcpy(base_ + size_t(page) * kPage, shadow_page(page), kPage);

        // 2. Undo whole frames, newest first.
        for (int f = newest_; f > frame; f--)
        {
            Record& rec = ring_[size_t(f) % ring_.size()];
            if (rec.frame != f - 1)
                return false; // cannot happen while oldest_ is maintained; refuse rather than corrupt
            for (size_t i = 0; i < rec.pages.size(); i++)
            {
                const uint8_t* old = rec.data.data() + i * kPage;
                std::memcpy(base_ + size_t(rec.pages[i]) * kPage, old, kPage);
                std::memcpy(shadow_page(rec.pages[i]), old, kPage);
            }
            rec = Record{};
        }

        // Our own restores dirtied the watch; start clean from the loaded frame.
        for (auto [b, e] : tracked_)
            ResetWriteWatch(base_ + size_t(b) * kPage, size_t(e - b) * kPage);
        newest_ = frame;
        return true;
    }

    void SnapshotRing::invalidate(int frame)
    {
        oldest_ = std::max(oldest_, frame);
    }

    uint64_t SnapshotRing::hash_tracked() const
    {
        // FNV-1a over the shadow (the state at the newest save), in page order.
        std::vector<uint32_t> pages;
        pages.reserve(shadow_.size());
        for (auto& [p, _] : shadow_)
            pages.push_back(p);
        std::sort(pages.begin(), pages.end());
        uint64_t h = 1469598103934665603ull;
        for (uint32_t p : pages)
        {
            const uint8_t* d = shadow_.at(p).get();
            for (size_t i = 0; i < kPage; i += 8)
            {
                uint64_t v;
                std::memcpy(&v, d + i, 8);
                h = (h ^ v) * 1099511628211ull;
            }
        }
        return h;
    }
}
