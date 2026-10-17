#pragma once

// Rollback state of the recompiled game: snapshots of guest memory, one per frame.
//
// Tekken6-rollback (Ekspym) tracks which guest pages the game writes during a frame and saves
// only those. On Windows the kernel can do that tracking for us: memory reserved with
// MEM_WRITE_WATCH records every page written since the last reset (GetWriteWatch), with no
// page faults on the hot path.
//
// Saving frame k copies, for every page dirtied during frame k-1, its *previous* content
// (kept in a shadow copy) into an undo record, then refreshes the shadow. Loading frame j
// replays the undo records from the newest back to j. Cost per frame is proportional to the
// pages the game actually touched, not to the size of its memory.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tt2::rollback
{
    class SnapshotRing
    {
    public:
        // `base`/`size`: the write-watched reservation (the whole guest address space).
        // `depth`: how many frames back load() can go.
        SnapshotRing(uint8_t* base, size_t size, int depth);
        ~SnapshotRing();

        // Guest ranges that are game state (heap, image .data/.bss, game thread stacks).
        // Everything else is ignored even when written.
        void track(uint32_t begin, uint32_t end);
        // Ranges inside tracked ones that must never roll back (audio stream buffers, GPU
        // ring buffer, file read buffers).
        void exclude(uint32_t begin, uint32_t end);

        // Takes the full shadow copy; frame `frame` becomes the oldest loadable frame.
        void begin(int frame);
        bool active() const { return active_; }

        // State at the start of `frame` (called once per frame boundary, in order; after a
        // load(), saving continues from the loaded frame).
        void save(int frame);
        // Back to the start of `frame`. False when that frame is no longer (or not yet) held.
        bool load(int frame);

        // Something changed that a snapshot cannot restore (guest memory map changed, kernel
        // object created): older frames become unloadable.
        void invalidate(int frame);

        // Diagnostics
        size_t pages_last_save() const { return pages_last_save_; }
        size_t shadow_bytes() const { return shadow_.size() * kPage; }
        uint64_t hash_tracked() const;

        static constexpr size_t kPage = 4096;

    private:
        struct Record
        {
            int frame = -1;                 // this record restores the state of `frame`
            std::vector<uint32_t> pages;    // guest page numbers
            std::vector<uint8_t> data;      // their content at `frame`, kPage bytes each
        };

        bool is_tracked(uint32_t page) const;
        void collect_dirty(std::vector<uint32_t>& out);
        uint8_t* shadow_page(uint32_t page);

        uint8_t* base_;
        size_t size_;
        int depth_;
        bool active_ = false;
        int newest_ = -1;                   // frame of the newest save
        int oldest_ = -1;

        std::vector<std::pair<uint32_t, uint32_t>> tracked_; // page ranges [begin, end)
        std::vector<std::pair<uint32_t, uint32_t>> excluded_;
        std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> shadow_;
        std::vector<Record> ring_;
        std::vector<void*> watch_buffer_;
        size_t pages_last_save_ = 0;
    };
}
