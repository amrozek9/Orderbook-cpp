#pragma once
//-----------------------------------------------------------------------------
// A pool of fixed-size slots in one contiguous block, addressed by 32-bit
// index.
//
// Each slot carries the two links of an intrusive doubly linked list, so a
// price level's queue is threaded through the pool itself: appending or
// removing an order is a few index writes, with no allocation and no separate
// list node. Freed slots go on an intrusive free list, LIFO, so the slot a
// cancel frees is the next one an add takes, still in cache.
//
// Indices rather than pointers: 4 bytes instead of 8 per link, so more of each
// cache line is order, and they survive the block moving when it grows.
// Growth doubles the block and copies it -- a one-time stall, counted by
// growths() so a benchmark can prove it never happened. Size the pool up front
// and the matching path never allocates at all.
//
// Under AddressSanitizer a freed slot is poisoned until it is handed out
// again, so a stale index that reads a released order is still reported. A
// stale index into a slot that has since been reused is invisible, as it is to
// any allocator that recycles memory; the book's invariant checks, which tie
// every queued slot to the id index, are what catch that.
//-----------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "lob/huge_pages.hpp"

#if defined(__SANITIZE_ADDRESS__)
#define LOB_SLOT_POOL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOB_SLOT_POOL_ASAN 1
#endif
#endif

#if defined(LOB_SLOT_POOL_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace lob {

using SlotIndex = std::uint32_t;
inline constexpr SlotIndex kNoSlot = std::numeric_limits<SlotIndex>::max();

//`Align` lets a caller whose slot is a power-of-two size align it to that
//size, so no slot straddles a cache line.
template <typename T, std::size_t Align = alignof(T)>
class SlotPool {
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied when the pool grows");

public:
    //One alignas with the maximum spelled out: GCC 15 silently kept only the
    //last of several alignas specifiers here, leaving 32-byte slots 8-aligned.
    struct alignas(std::max({Align, alignof(T), alignof(SlotIndex)})) Slot {
        T value;
        SlotIndex prev = kNoSlot;       //Queue links while in use...
        SlotIndex next = kNoSlot;       //...and the free-list link while free
    };

    //Allocates and touches `capacity` slots up front (at least one).
    explicit SlotPool(std::size_t capacity)
        : slots(capacity == 0 ? 1 : capacity) {}

    ~SlotPool() {unpoison_all();}
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    //A slot for a new value; its links are reset.
    SlotIndex acquire() {
        SlotIndex i;
        if (free_head != kNoSlot) [[likely]] {     //Churn keeps the free list stocked
            i = free_head;
            unpoison(i);
            free_head = slots[i].next;
        } else {
            if (used == slots.size()) [[unlikely]] grow();
            i = static_cast<SlotIndex>(used++);
        }
        slots[i].prev = slots[i].next = kNoSlot;
        ++live;
        return i;
    }

    void release(SlotIndex i) {
#ifndef NDEBUG
        std::memset(static_cast<void*>(&slots[i].value), 0xDD, sizeof(T));
#endif
        slots[i].prev = kNoSlot;
        slots[i].next = free_head;
        free_head = i;
        --live;
        poison(i);
    }

    Slot& operator[](SlotIndex i) {return slots[i];}
    const Slot& operator[](SlotIndex i) const {return slots[i];}

    std::size_t capacity() const {return slots.size();}
    std::size_t size() const {return live;}             //Slots handed out and not released
    std::uint64_t growths() const {return grow_count;}

    //Every free-list entry is a slot that was handed out once, the list has
    //no cycle, and the free list plus the live slots account for every slot
    //ever used. Returns false on any inconsistency. Walks the whole free list
    //without allocating: each entry has one successor, so a repeated slot
    //would be a cycle, and a walk longer than `used` is caught by the bound.
    bool consistent() const {
        std::size_t free_count = 0;
        for (SlotIndex i = free_head; i != kNoSlot;) {
            if (i >= used || ++free_count > used) return false;
            unpoison(i);
            const SlotIndex next = slots[i].next;
            poison(i);
            i = next;
        }
        return free_count + live == used;
    }

private:
    std::vector<Slot, HugePageAllocator<Slot>> slots;   //Huge pages once past 2 MiB
    std::size_t used = 0;               //High-water mark: slots [0, used) handed out at least once
    std::size_t live = 0;
    SlotIndex free_head = kNoSlot;
    std::uint64_t grow_count = 0;

    void grow() {
        //kNoSlot is reserved, so the largest usable index is one below it.
        constexpr std::size_t kMax = kNoSlot;
        if (slots.size() >= kMax) throw std::length_error("SlotPool: 32-bit index space exhausted");
        const std::size_t bigger = slots.size() > kMax / 2 ? kMax : slots.size() * 2;
        //Only called with the free list empty, so no slot is poisoned while
        //the block is copied.
        slots.resize(bigger);
        ++grow_count;
    }

#if defined(LOB_SLOT_POOL_ASAN)
    void poison(SlotIndex i) const {ASAN_POISON_MEMORY_REGION(&slots[i], sizeof(Slot));}
    void unpoison(SlotIndex i) const {ASAN_UNPOISON_MEMORY_REGION(&slots[i], sizeof(Slot));}
    void unpoison_all() const {
        if (!slots.empty()) ASAN_UNPOISON_MEMORY_REGION(slots.data(), slots.size() * sizeof(Slot));
    }
#else
    void poison(SlotIndex) const {}
    void unpoison(SlotIndex) const {}
    void unpoison_all() const {}
#endif
};

}
