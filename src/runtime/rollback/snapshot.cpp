#include "snapshot.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace tt2::rollback
{
    namespace
    {
        bool page_is_zero(const uint8_t* p)
        {
            for (size_t o = 0; o < SnapshotRing::kPage; o += 8)
            {
                uint64_t v;
                std::memcpy(&v, p + o, 8);
                if (v)
                    return false;
            }
            return true;
        }
    }

    SnapshotRing::SnapshotRing(uint8_t* base, size_t size, int depth)
        : base_(base), size_(size), depth_(depth), state_(size / kPage, kUntracked), ring_(size_t(depth) + 1)
    {
    }

    SnapshotRing::~SnapshotRing()
    {
        // Leave the memory writable for whoever runs next.
        std::lock_guard lock(mutex_);
        for (auto [b, e] : tracked_)
            protect(b, e - b, true);
    }

    void SnapshotRing::protect(uint32_t first_page, uint32_t count, bool writable)
    {
        if (!count)
            return;
        DWORD old;
        VirtualProtect(base_ + size_t(first_page) * kPage, size_t(count) * kPage, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
    }

    bool SnapshotRing::is_excluded(uint32_t page) const
    {
        for (auto [b, e] : excluded_)
            if (page >= b && page < e)
                return true;
        return false;
    }

    void SnapshotRing::track(uint32_t begin, uint32_t end)
    {
        const uint32_t first = begin / uint32_t(kPage);
        const uint32_t last = uint32_t((uint64_t(end) + kPage - 1) / kPage);
        std::lock_guard lock(mutex_);
        if (active_)
            pending_track_.push_back({ first, last });
        else
            tracked_.push_back({ first, last });
    }

    void SnapshotRing::exclude(uint32_t begin, uint32_t end)
    {
        // Partially excluded pages stay tracked: excluding them would lose the rest of the page.
        const uint32_t first = uint32_t((uint64_t(begin) + kPage - 1) / kPage);
        const uint32_t last = end / uint32_t(kPage);
        if (first < last)
            excluded_.push_back({ first, last });
    }

    uint8_t* SnapshotRing::shadow_page(uint32_t page)
    {
        auto& slot = shadow_[page];
        if (!slot)
        {
            slot = std::make_unique<uint8_t[]>(kPage);
            std::memset(slot.get(), 0, kPage);
        }
        return slot.get();
    }

    // Starts tracking [first, last): the current content is the state of the newest frame.
    void SnapshotRing::track_now(uint32_t first, uint32_t last)
    {
        uint32_t run = 0, run_start = 0;
        for (uint32_t page = first; page < last && page < state_.size(); page++)
        {
            const bool take = state_[page] == kUntracked && !is_excluded(page);
            if (take)
            {
                const uint8_t* p = base_ + size_t(page) * kPage;
                if (!page_is_zero(p))
                    std::memcpy(shadow_page(page), p, kPage);
                else
                    shadow_.erase(page);
                state_[page] = kClean;
                if (!run)
                    run_start = page;
                run++;
            }
            if (!take && run)
            {
                protect(run_start, run, false);
                run = 0;
            }
        }
        if (run)
            protect(run_start, run, false);
    }

    void SnapshotRing::begin(int frame)
    {
        std::lock_guard lock(mutex_);
        active_ = true; // faults on pages protected below are handled (after this lock)
        shadow_.clear();
        for (auto& r : ring_)
            r = Record{};
        dirty_.clear();
        for (auto [b, e] : tracked_)
            track_now(b, e);
        newest_ = oldest_ = frame;
    }

    bool SnapshotRing::handle_page(uint32_t page, bool simulation_thread)
    {
        std::lock_guard lock(mutex_);
        switch (state_[page])
        {
        case kClean:
            if (simulation_thread)
            {
                state_[page] = kDirty;
                dirty_.push_back(page);
            }
            else
            {
                state_[page] = kForeign;
                foreign_count_++;
            }
            protect(page, 1, true);
            return true;
        case kDirty:
        case kForeign:
            return true; // made writable by another thread a moment ago: just retry
        default:
            return false;
        }
    }

    bool SnapshotRing::on_write_fault(uintptr_t host_address, bool simulation_thread)
    {
        const uintptr_t b = reinterpret_cast<uintptr_t>(base_);
        if (!active_ || host_address < b || host_address >= b + size_)
            return false;
        return handle_page(uint32_t((host_address - b) / kPage), simulation_thread);
    }

    void SnapshotRing::before_host_write(uint32_t address, uint32_t size, bool simulation_thread)
    {
        if (!active_ || !size)
            return;
        const uint32_t first = address / uint32_t(kPage);
        const uint32_t last = uint32_t((uint64_t(address) + size + kPage - 1) / kPage);
        for (uint32_t page = first; page < last && page < state_.size(); page++)
            if (state_[page] == kClean)
                handle_page(page, simulation_thread);
    }

    void SnapshotRing::save(int frame)
    {
        if (!active_)
            return;
        std::lock_guard lock(mutex_);

        // New allocations since the last save join the tracked set.
        for (auto [b, e] : pending_track_)
        {
            track_now(b, e);
            tracked_.push_back({ b, e });
        }
        pending_track_.clear();

        std::vector<uint32_t> dirty;
        dirty.swap(dirty_);
        std::sort(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        pages_last_save_ = dirty.size();

        // The record of the previous frame keeps what these pages held then.
        Record& rec = ring_[size_t(frame) % ring_.size()];
        rec.frame = frame - 1;
        rec.pages.clear();
        rec.data.resize(dirty.size() * kPage);
        uint64_t h = 1469598103934665603ull;
        size_t n = 0;
        last_changes_.clear();
        for (uint32_t page : dirty)
        {
            if (state_[page] != kDirty)
                continue;
            uint8_t* shadow = shadow_page(page);
            const uint8_t* now = base_ + size_t(page) * kPage;
            std::memcpy(rec.data.data() + n * kPage, shadow, kPage);
            rec.pages.push_back(page);
            n++;
            if (std::memcmp(shadow, now, kPage) != 0)
            {
                uint64_t ph = 1469598103934665603ull;
                for (size_t o = 0; o < kPage; o += 8)
                {
                    uint64_t v;
                    std::memcpy(&v, now + o, 8);
                    ph = (ph ^ v) * 1099511628211ull;
                }
                h = ((h ^ page) * 1099511628211ull ^ ph) * 1099511628211ull;
                last_changes_.push_back({ page, ph });
            }
            std::memcpy(shadow, now, kPage);
            state_[page] = kClean;
            protect(page, 1, false);
        }
        rec.data.resize(n * kPage);
        last_delta_hash_ = h;

        newest_ = frame;
        oldest_ = std::max(oldest_, frame - depth_);
    }

    bool SnapshotRing::load(int frame)
    {
        if (!active_ || frame < oldest_ || frame > newest_)
            return false;
        std::lock_guard lock(mutex_);

        // 1. Writes since the newest save: back to the shadow (= state of newest_).
        for (uint32_t page : dirty_)
        {
            if (state_[page] != kDirty)
                continue;
            std::memcpy(base_ + size_t(page) * kPage, shadow_page(page), kPage);
            state_[page] = kClean;
            protect(page, 1, false);
        }
        dirty_.clear();

        // 2. Undo whole frames, newest first. Foreign pages stay as their owner left them.
        for (int f = newest_; f > frame; f--)
        {
            Record& rec = ring_[size_t(f) % ring_.size()];
            if (rec.frame != f - 1)
                return false;
            for (size_t i = 0; i < rec.pages.size(); i++)
            {
                const uint32_t page = rec.pages[i];
                if (state_[page] != kClean)
                    continue;
                const uint8_t* old = rec.data.data() + i * kPage;
                protect(page, 1, true);
                std::memcpy(base_ + size_t(page) * kPage, old, kPage);
                protect(page, 1, false);
                std::memcpy(shadow_page(page), old, kPage);
            }
            rec = Record{};
        }

        newest_ = frame;
        return true;
    }
}
