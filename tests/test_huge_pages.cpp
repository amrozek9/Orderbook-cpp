//-----------------------------------------------------------------------------
// The huge-page allocator: big blocks start on a 2 MiB boundary and are fully
// usable, small ones keep their natural alignment, and a container moves
// across the threshold without losing anything. Whether the kernel actually
// backs a block with huge pages is its decision; bench reports that.
//-----------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "lob/huge_pages.hpp"

#include <cstdint>
#include <vector>

using lob::HugePageAllocator;
using lob::kHugePage;

TEST_CASE("huge pages: a big block starts on a 2 MiB boundary and is usable end to end") {
    HugePageAllocator<std::uint64_t> alloc;
    const std::size_t n = (3 * kHugePage) / sizeof(std::uint64_t) + 7;     //Not a whole number of pages
    std::uint64_t* p = alloc.allocate(n);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % kHugePage == 0);
    for (std::size_t i = 0; i < n; ++i) p[i] = i;
    CHECK(p[0] == 0);
    CHECK(p[n - 1] == n - 1);
    alloc.deallocate(p, n);
}

TEST_CASE("huge pages: a small block keeps its type's alignment") {
    struct alignas(32) Wide {char bytes[32];};
    HugePageAllocator<Wide> alloc;
    for (int k = 0; k < 16; ++k) {
        Wide* p = alloc.allocate(3);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % 32 == 0);
        alloc.deallocate(p, 3);
    }
}

TEST_CASE("huge pages: a vector grows past the threshold and keeps its contents") {
    std::vector<std::uint32_t, HugePageAllocator<std::uint32_t>> v;
    for (std::uint32_t i = 0; i < 2'000'000; ++i) v.push_back(i);      //8 MB: crosses 2 MiB
    CHECK(v.size() == 2'000'000);
    CHECK(reinterpret_cast<std::uintptr_t>(v.data()) % kHugePage == 0);
    CHECK(v[1'234'567] == 1'234'567);
}
