#pragma once
//-----------------------------------------------------------------------------
// Order id -> value, as an open-addressed hash table.
//
// std::unordered_map is a chained table: a lookup loads the bucket array, then
// follows a pointer to a node elsewhere on the heap -- two dependent cache
// misses on a big book, and one node per order to allocate and free. This is
// a single flat array of entries, 16 bytes each for a 4-byte value:
//
//   - The hash keeps sequential ids sequential: (id ^ id >> log2 capacity),
//     masked. Exchanges hand out ids in order, so consecutive adds land in
//     consecutive entries -- cache lines already fetched -- and cancels of
//     recent orders hit lines still in cache. Folding in the high bits stops
//     ids exactly a table apart from all piling onto one slot.
//
//     A multiplicative hash was tried first and rejected on measurement. It
//     scatters sequential ids across the table, so on a 200,000-order book,
//     with a 16 MiB table, every add became a DRAM miss: add p50 went from
//     70 to 180 ns.
//   - Collisions probe linearly, so a probe walks forward through memory the
//     prefetcher is already pulling in.
//   - Capacity is a power of two, kept at most half full: probe runs stay
//     short, and the home slot is a shift rather than a division.
//   - Erase shifts the following entries back instead of leaving tombstones,
//     so a table that sees millions of adds and cancels never degrades. The
//     cost is that an erase scans to the end of its run of occupied entries.
//     Sequential ids make the newest orders one run, so a cancel scans the
//     orders added after it: a handful on the benchmark's flow. Robin Hood
//     ordering would let that scan stop at the first entry sitting at its
//     home.
//
// Growth doubles the table and rehashes every entry -- a stall, counted by
// growths(). Provision for the peak book and it never happens while matching.
//-----------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace lob {

template <typename Value>
class IdIndex {
    static_assert(std::is_trivially_copyable_v<Value>, "entries are moved by plain copy");

public:
    using Key = std::uint64_t;

    //Room for `expected` ids without growing (at least 8).
    explicit IdIndex(std::size_t expected) {
        std::size_t cap = 16;
        while (cap / 2 < expected) cap *= 2;
        resize_table(cap);
    }

    Value* find(Key id) {
        for (std::size_t i = home(id);; i = (i + 1) & mask) {
            Entry& e = table[i];
            if (!e.used) return nullptr;
            if (e.id == id) return &e.value;
        }
    }
    const Value* find(Key id) const {return const_cast<IdIndex*>(this)->find(id);}
    bool contains(Key id) const {return find(id) != nullptr;}

    //Adds an id the caller knows is absent.
    void insert(Key id, const Value& value) {
        if ((count + 1) * 2 > table.size()) grow();
        place(id, value);
        ++count;
    }

    //Removes `id`, copying its value to `out`. False if it was absent.
    bool take(Key id, Value& out) {
        std::size_t i = home(id);
        for (;; i = (i + 1) & mask) {
            if (!table[i].used) return false;
            if (table[i].id == id) break;
        }
        out = table[i].value;
        close_gap(i);
        --count;
        return true;
    }

    bool erase(Key id) {
        Value ignored;
        return take(id, ignored);
    }

    std::size_t size() const {return count;}
    std::size_t capacity() const {return table.size();}
    std::uint64_t growths() const {return grow_count;}

    //Every entry is reachable from its home slot without crossing an empty
    //one, and the count matches. Allocation-free; walks the whole table.
    bool consistent() const {
        std::size_t seen = 0;
        for (std::size_t p = 0; p < table.size(); ++p) {
            if (!table[p].used) continue;
            ++seen;
            for (std::size_t i = home(table[p].id); i != p; i = (i + 1) & mask)
                if (!table[i].used) return false;
        }
        return seen == count;
    }

private:
    struct Entry {
        Key id;
        Value value;
        bool used;
    };

    std::vector<Entry> table;
    std::size_t mask = 0;
    unsigned shift = 0;             //64 - log2(capacity)
    std::size_t count = 0;
    std::uint64_t grow_count = 0;

    std::size_t home(Key id) const {
        return static_cast<std::size_t>(id ^ (id >> (64 - shift))) & mask;
    }

    void resize_table(std::size_t cap) {
        table.assign(cap, Entry{});
        mask = cap - 1;
        shift = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) --shift;
    }

    void place(Key id, const Value& value) {
        std::size_t i = home(id);
        while (table[i].used) i = (i + 1) & mask;
        table[i] = Entry{id, value, true};
    }

    //Backward-shift deletion: after emptying slot `hole`, pull each following
    //entry back into it unless that would move the entry before its home.
    void close_gap(std::size_t hole) {
        for (std::size_t j = (hole + 1) & mask; table[j].used; j = (j + 1) & mask) {
            const std::size_t h = home(table[j].id);
            //The entry at j may stay only if its home lies cyclically in (hole, j].
            const bool stays = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!stays) {
                table[hole] = table[j];
                hole = j;
            }
        }
        table[hole].used = false;
    }

    void grow() {
        if (table.size() > (std::size_t{1} << 62)) throw std::length_error("IdIndex: table too large");
        std::vector<Entry> old;
        old.swap(table);
        resize_table(old.size() * 2);
        for (const Entry& e : old)
            if (e.used) place(e.id, e.value);
        ++grow_count;
    }
};

}
