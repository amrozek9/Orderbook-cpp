#include "lob/node_arena.hpp"

#include <algorithm>

namespace lob {

namespace {
    //Touching one byte every 4 KiB faults in every page, whatever the page size.
    constexpr std::size_t kTouchStride = 4096;
}

NodeArena::NodeArena(std::size_t reserve_bytes) {
    if (reserve_bytes > 0) add_slab(std::max(reserve_bytes + kGranule, kSlabBytes));
}

NodeArena::~NodeArena() {
    while (newest) {
        SlabHeader* prev = newest->prev;
        ::operator delete(static_cast<void*>(newest), newest->bytes);
        newest = prev;
    }
}

void* NodeArena::carve(std::size_t size_class) {
    const std::size_t bytes = size_class * kGranule;
    if (static_cast<std::size_t>(limit - cursor) < bytes) add_slab(kSlabBytes);
    void* p = cursor;
    cursor += bytes;
    return p;
}

void NodeArena::add_slab(std::size_t bytes) {
    auto* slab = static_cast<std::byte*>(::operator new(bytes));
    //Fault every page in now, so none faults in later on the matching path.
    for (std::size_t off = 0; off < bytes; off += kTouchStride)
        static_cast<volatile unsigned char*>(static_cast<void*>(slab))[off] = 0;

    newest = new (slab) SlabHeader{newest, bytes};
    slab_total += bytes;
    //The previous slab's tail, too small for the block that did not fit, is
    //abandoned: at most kMaxBlock bytes per slab.
    cursor = slab + kGranule;
    limit = slab + bytes;
}

}
