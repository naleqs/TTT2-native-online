#pragma once

// Kernel objects (events, semaphores, mutants, timers, threads, files) and waits.
//
// All signal state lives on the host under one mutex; waiters sleep on one condition
// variable. That keeps WaitForMultipleObjects (any/all, with timeouts) simple and correct.
// Guest code reaches objects two ways:
//   * by handle (Nt* functions), from the handle table
//   * by pointer to a guest dispatcher structure (Ke* functions on a KEVENT the game
//     allocated itself); the host object is created on first use from the header type

#include "../guest.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace tt2::kernel
{
    using Clock = std::chrono::steady_clock;

    enum class ObjectType
    {
        Event,
        Semaphore,
        Mutant,
        Timer,
        Thread,
        File,
        Other,
    };

    struct Object : std::enable_shared_from_this<Object>
    {
        virtual ~Object() = default;
        virtual ObjectType type() const = 0;

        // Wait support (called with the wait mutex held).
        virtual bool is_signaled(uint32_t thread_id) const { return false; }
        virtual void on_acquired(uint32_t thread_id) {}

        // Guest-visible body (ObReferenceObjectByHandle returns its address).
        uint32_t body = 0;
        // Guest dispatcher header mirrored on signal changes (0 = none).
        uint32_t guest_header = 0;
        void mirror_signal_state(int32_t state);
    };

    struct Event final : Object
    {
        bool manual_reset;
        bool signaled;
        Event(bool manual, bool initial) : manual_reset(manual), signaled(initial) {}
        ObjectType type() const override { return ObjectType::Event; }
        bool is_signaled(uint32_t) const override { return signaled; }
        void on_acquired(uint32_t) override
        {
            if (!manual_reset)
                signaled = false;
        }
    };

    struct Semaphore final : Object
    {
        int32_t count, limit;
        Semaphore(int32_t c, int32_t l) : count(c), limit(l) {}
        ObjectType type() const override { return ObjectType::Semaphore; }
        bool is_signaled(uint32_t) const override { return count > 0; }
        void on_acquired(uint32_t) override { count--; }
    };

    struct Mutant final : Object
    {
        uint32_t owner = 0;
        int32_t recursion = 0;
        ObjectType type() const override { return ObjectType::Mutant; }
        bool is_signaled(uint32_t tid) const override { return owner == 0 || owner == tid; }
        void on_acquired(uint32_t tid) override
        {
            owner = tid;
            recursion++;
        }
    };

    struct Timer final : Object
    {
        bool manual_reset;
        bool signaled = false;
        Clock::time_point due{};
        std::chrono::milliseconds period{ 0 };
        bool armed = false;
        uint32_t apc_routine = 0, apc_context = 0;
        explicit Timer(bool manual) : manual_reset(manual) {}
        ObjectType type() const override { return ObjectType::Timer; }
        bool is_signaled(uint32_t) const override;
        void on_acquired(uint32_t) override
        {
            if (!manual_reset)
                signaled = false;
        }
    };

    // Handles
    uint32_t create_handle(std::shared_ptr<Object> object);
    std::shared_ptr<Object> object_from_handle(uint32_t handle);
    bool close_handle(uint32_t handle);
    uint32_t duplicate_handle(uint32_t handle);

    template <typename T>
    std::shared_ptr<T> handle_as(uint32_t handle)
    {
        return std::dynamic_pointer_cast<T>(object_from_handle(handle));
    }

    // Object behind a guest pointer: a registered body, or a dispatcher header the game owns.
    std::shared_ptr<Object> object_from_pointer(uint32_t guest);
    void register_body(const std::shared_ptr<Object>& object, uint32_t size);
    void forget_pointer(uint32_t guest);

    // Waits. `timeout` is the NT 100 ns interval (negative = relative, positive = absolute
    // system time, null = infinite).
    std::optional<Clock::time_point> deadline_from_nt(const be<int64_t>* timeout);
    uint32_t wait(const std::vector<std::shared_ptr<Object>>& objects, bool wait_all, std::optional<Clock::time_point> deadline, bool alertable);

    // Signal changes go through these so waiters wake up.
    void with_lock(const std::function<void()>& fn);
    std::mutex& wait_mutex();
    void notify_waiters();

    uint32_t current_thread_id();
}
