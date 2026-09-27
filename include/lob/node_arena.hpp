#pragma once
//-----------------------------------------------------------------------------
// Per-book node arena.
//
// Every resting order costs container nodes -- a list node, a hash-map node,
// and a map node when it opens a price level -- and every cancel or fill frees
// them. Through malloc, that traffic is a large share of the p99 and p99.9
// tail. This is the allocator that replaces it, owned by one book:
//
//   - Blocks are carved from slabs and recycled through one free list per
//     16-byte size class, LIFO, so the node a cancel frees is the next one an
//     add reuses, still warm in cache. No headers, no locks, no bin searches:
//     the containers pass the size back on deallocation.
//   - Every slab is touched page by page when it is allocated, and slabs can be
//     provisioned up front (Config::expected_orders), so the matching path
//     neither calls the system allocator nor takes a page fault.
//   - Memory goes back to the system only when the book is destroyed.
//
// No global state: each book owns its arena, which keeps the engine a pure
// function of its input and lets distinct books run on distinct threads.
//-----------------------------------------------------------------------------
#include <array>
#include <cstddef>
#include <cstring>
#include <new>

namespace lob {

class NodeArena {
public:
    static constexpr std::size_t kGranule = 16;         //Size-class step, and block alignment
    static constexpr std::size_t kMaxBlock = 256;       //Larger requests go to the system
    static constexpr std::size_t kSlabBytes = 64 * 1024;

    //Allocates and touches at least `reserve_bytes` of slab space up front.
    explicit NodeArena(std::size_t reserve_bytes = 0);
    ~NodeArena();
    NodeArena(const NodeArena&) = delete;
    NodeArena& operator=(const NodeArena&) = delete;

    void* allocate(std::size_t bytes) {
        if (bytes > kMaxBlock) return ::operator new(bytes);
        const std::size_t c = size_class(bytes);
        if (FreeBlock* b = free_list[c]) {
            free_list[c] = b->next;
            return b;
        }
        return carve(c);
    }

    void deallocate(void* p, std::size_t bytes) noexcept {
        if (bytes > kMaxBlock) {
            ::operator delete(p, bytes);
            return;
        }
        const std::size_t c = size_class(bytes);
#ifndef NDEBUG
        std::memset(p, 0xDD, c * kGranule);     //A dangling node reads as garbage
#endif
        auto* b = static_cast<FreeBlock*>(p);
        b->next = free_list[c];
        free_list[c] = b;
    }

    std::size_t slab_bytes() const {return slab_total;}

private:
    struct FreeBlock {FreeBlock* next;};
    struct SlabHeader {                         //First granule of every slab
        SlabHeader* prev;
        std::size_t bytes;
    };
    static_assert(sizeof(SlabHeader) <= kGranule);

    static std::size_t size_class(std::size_t bytes) {
        return bytes == 0 ? 1 : (bytes + kGranule - 1) / kGranule;
    }

    std::array<FreeBlock*, kMaxBlock / kGranule + 1> free_list{};
    std::byte* cursor = nullptr;                //Unused tail of the newest slab
    std::byte* limit = nullptr;
    SlabHeader* newest = nullptr;
    std::size_t slab_total = 0;

    void* carve(std::size_t size_class);        //May add a slab
    void add_slab(std::size_t bytes);
};

//Standard allocator over a NodeArena, or over the system allocator when the
//arena pointer is null. Deliberately not default-constructible, so a
//container can never quietly end up on the wrong allocator.
template <typename T>
class ArenaAllocator {
public:
    using value_type = T;

    explicit ArenaAllocator(NodeArena* a) noexcept : arena(a) {}
    template <typename U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena(other.arena) {}

    T* allocate(std::size_t n) {
        static_assert(alignof(T) <= NodeArena::kGranule);
        const std::size_t bytes = n * sizeof(T);
        return static_cast<T*>(arena ? arena->allocate(bytes) : ::operator new(bytes));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        const std::size_t bytes = n * sizeof(T);
        if (arena) arena->deallocate(p, bytes);
        else       ::operator delete(p, bytes);
    }

    friend bool operator==(const ArenaAllocator& a, const ArenaAllocator& b) noexcept {
        return a.arena == b.arena;
    }

private:
    template <typename U> friend class ArenaAllocator;
    NodeArena* arena;
};

}
