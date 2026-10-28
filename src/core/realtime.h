// SPDX-License-Identifier: GPL-3.0-or-later
// Lock-free hand-over between the GUI thread and the audio thread (handbook 11.3, 11.4, 5.3).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace ap {

// Sequence lock: one writer (GUI) publishes a trivially copyable value; one reader (audio) takes a consistent copy
// without waiting, or keeps its previous copy until the next try.
template <class T>
class SeqLockSlot {
    static_assert(std::is_trivially_copyable<T>::value, "SeqLockSlot needs a trivially copyable type");

public:
    void publish(const T& value) {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed); // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        slot_ = value;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release); // even: stable
    }
    // Copies the newest value into `out` if there is one the reader has not seen. Never blocks.
    bool take(T& out) {
        const uint32_t s = seq_.load(std::memory_order_acquire);
        if (s == seen_ || (s & 1u)) return false;
        out = slot_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s) return false; // written meanwhile: next time
        seen_ = s;
        return true;
    }

private:
    std::atomic<uint32_t> seq_{0};
    uint32_t seen_ = 0;
    T slot_{};
};

// Single-producer / single-consumer ring. Storage allocated once in the constructor (on the heap: objects holding
// a ring may live on a thread's stack).
template <class T, int Capacity>
class SpscRing {
public:
    SpscRing() : items_(new T[Capacity]()) {}
    // Producer (audio thread). Overwrites the oldest item when the reader fell behind.
    void push(const T& item) {
        const uint64_t w = written_.load(std::memory_order_relaxed);
        items_[w % Capacity] = item;
        written_.store(w + 1, std::memory_order_release);
    }
    // Consumer: copies at most `max` new items, oldest first. Skips ahead when it fell too far behind.
    int pop(T* out, int max) {
        const uint64_t w = written_.load(std::memory_order_acquire);
        if (w - readPos_ > static_cast<uint64_t>(Capacity - 16)) readPos_ = w - (Capacity - 16);
        int n = 0;
        while (readPos_ < w && n < max) out[n++] = items_[readPos_++ % Capacity];
        return n;
    }
    // Consumer: forget everything written so far.
    void skipAll() { readPos_ = written_.load(std::memory_order_acquire); }

private:
    std::unique_ptr<T[]> items_;
    std::atomic<uint64_t> written_{0};
    uint64_t readPos_ = 0;
};

} // namespace ap
