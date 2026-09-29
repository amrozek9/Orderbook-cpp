#pragma once
//-----------------------------------------------------------------------------
// A standard allocator that asks for transparent huge pages on big blocks.
//
// A random access into a 16 MiB table lands on one of 4,096 ordinary 4 KiB
// pages, more than the second-level TLB holds, so on a deep book a lookup can
// miss the TLB as well as the cache. Backed by 2 MiB pages, the same table is
// 8 translations.
//
// Blocks of at least 2 MiB are mapped fresh with mmap, trimmed to a 2 MiB
// boundary, and marked MADV_HUGEPAGE, so the kernel can back them with huge
// pages from the first touch. Going through malloc instead would risk getting
// heap memory that is already faulted in as small pages, which the advice
// cannot retroactively change. Smaller blocks take std::allocator's own path:
// a huge page for a few hundred KiB would mostly be waste.
//
// This is advice, not a guarantee. With transparent huge pages set to "never",
// or no huge page free, the memory is simply ordinary pages and everything
// still works. bench reports how much of the book huge pages actually backed.
//-----------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace lob {

inline constexpr std::size_t kHugePage = std::size_t{2} << 20;

template <typename T>
class HugePageAllocator {
public:
    using value_type = T;

    HugePageAllocator() noexcept = default;
    template <typename U>
    HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        if (bytes >= kHugePage) return static_cast<T*>(map_huge(round_up(bytes)));
        //Small blocks take exactly std::allocator's path, so a book too small
        //for huge pages is laid out just as it was without this allocator.
        if constexpr (kOverAligned)
            return static_cast<T*>(::operator new(bytes, std::align_val_t{alignof(T)}));
        else
            return static_cast<T*>(::operator new(bytes));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        const std::size_t bytes = n * sizeof(T);
        if (bytes >= kHugePage) {
            unmap_huge(p, round_up(bytes));
            return;
        }
        if constexpr (kOverAligned) ::operator delete(p, bytes, std::align_val_t{alignof(T)});
        else                        ::operator delete(p, bytes);
    }

    friend bool operator==(const HugePageAllocator&, const HugePageAllocator&) noexcept {return true;}

private:
    static constexpr bool kOverAligned = alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__;

    static std::size_t round_up(std::size_t bytes) {
        return (bytes + kHugePage - 1) / kHugePage * kHugePage;
    }

#if defined(__linux__)
    //Over-map by one huge page, then unmap the slack on either side so what
    //remains starts on a 2 MiB boundary.
    static void* map_huge(std::size_t size) {
        void* raw = mmap(nullptr, size + kHugePage, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) throw std::bad_alloc();
        const auto base = reinterpret_cast<std::uintptr_t>(raw);
        const std::uintptr_t start = (base + kHugePage - 1) / kHugePage * kHugePage;
        if (start > base) munmap(raw, start - base);
        const std::uintptr_t end = base + size + kHugePage;
        if (end > start + size) munmap(reinterpret_cast<void*>(start + size), end - (start + size));
        void* p = reinterpret_cast<void*>(start);
#if defined(MADV_HUGEPAGE)
        madvise(p, size, MADV_HUGEPAGE);        //Advice: failure just means small pages
#endif
        return p;
    }
    static void unmap_huge(void* p, std::size_t size) noexcept {munmap(p, size);}
#else
    static void* map_huge(std::size_t size) {
        return ::operator new(size, std::align_val_t{kHugePage});
    }
    static void unmap_huge(void* p, std::size_t size) noexcept {
        ::operator delete(p, size, std::align_val_t{kHugePage});
    }
#endif
};

}
