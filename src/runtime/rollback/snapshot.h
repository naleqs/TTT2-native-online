#pragma once

// Rollback state of the recompiled game: snapshots of guest memory, one per frame.
//
// Tracking follows Tekken6-rollback (Ekspym): after every save the tracked pages are made
// read-only. The first write to a page faults, and the faulting thread decides what the page
// is:
//   * the simulation thread (the game's main thread): the page is game state; it becomes
//     writable and is saved at the next frame boundary;
//   * any other thread (movie/audio/streaming workers, the null GPU): the page is "foreign"
//     and is never rolled back - restoring it under that thread's feet would corrupt it.
//
// Saving frame k copies, for every page dirtied during frame k-1, its previous content (kept
// in a shadow copy) into an undo record, then refreshes the shadow. Loading frame j replays
// the undo records from the newest back to j. Cost per frame is proportional to the pages the
// game actually touched.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tt2::rollback
{
    class SnapshotRing
    {
    public:
        // `base`/`size`: the guest address space. `depth`: how many frames back load() can go.
        SnapshotRing(uint8_t* base, size_t size, int depth);
        ~SnapshotRing();

        // Guest ranges that are game state; may be called later for new allocations (their
        // content at the next save becomes the shadow).
        void track(uint32_t begin, uint32_t end);
        // Ranges that must never roll back (applied by begin()).
        void exclude(uint32_t begin, uint32_t end);

        // Takes the shadow copy and write-protects the tracked pages; frame `frame` becomes
        // the oldest loadable frame. Call from the simulation thread.
        void begin(int frame);
        bool active() const { return active_; }

        // State at the start of `frame` (simulation thread, at the frame boundary).
        void save(int frame);
        // Back to the start of `frame`. False when that frame is not held.
        bool load(int frame);

        // Write fault at `host_address` from the current thread. True when handled (it was a
        // protected tracked page and is writable now): the access can simply be retried.
        bool on_write_fault(uintptr_t host_address, bool simulation_thread);
        // The OS is about to write [address, address+size) on behalf of the current thread
        // (ReadFile into protected memory fails inside the kernel instead of faulting).
        void before_host_write(uint32_t address, uint32_t size, bool simulation_thread);

        // Diagnostics
        size_t pages_last_save() const { return pages_last_save_; }
        size_t shadow_bytes() const { return shadow_.size() * kPage; }
        size_t foreign_pages() const { return foreign_count_; }
        // Hash of what the last saved frame changed (page numbers and contents).
        uint64_t last_delta_hash() const { return last_delta_hash_; }
        // Pages changed by the last saved frame with a hash of their new content (diagnostics).
        const std::vector<std::pair<uint32_t, uint64_t>>& last_changes() const { return last_changes_; }

        static constexpr size_t kPage = 4096;

    private:
        enum : uint8_t
        {
            kUntracked = 0,
            kClean = 1,   // tracked, write-protected
            kDirty = 2,   // written by the simulation thread since the last save
            kForeign = 3, // written by another thread: never rolled back
        };

        struct Record
        {
            int frame = -1;                 // this record restores the state of `frame`
            std::vector<uint32_t> pages;
            std::vector<uint8_t> data;
        };

        void protect(uint32_t first_page, uint32_t count, bool writable);
        bool handle_page(uint32_t page, bool simulation_thread);
        uint8_t* shadow_page(uint32_t page);
        void track_now(uint32_t first, uint32_t last);
        bool is_excluded(uint32_t page) const;

        uint8_t* base_;
        size_t size_;
        int depth_;
        std::atomic<bool> active_{ false };
        int newest_ = -1;
        int oldest_ = -1;

        std::vector<uint8_t> state_;        // per guest page
        std::vector<std::pair<uint32_t, uint32_t>> tracked_, pending_track_, excluded_;
        std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> shadow_;
        std::vector<Record> ring_;

        std::mutex mutex_;                  // state_ and dirty_ (faulting threads take it)
        std::vector<uint32_t> dirty_;
        size_t foreign_count_ = 0;
        size_t pages_last_save_ = 0;
        uint64_t last_delta_hash_ = 0;
        std::vector<std::pair<uint32_t, uint64_t>> last_changes_;
    };
}
