# lob — a limit order book

A price-time priority limit order book in C++20 whose matching path never
allocates, never does I/O and never walks a tree: price levels sit in a flat
array, orders in a pool of 32-byte slots linked by 32-bit index, and order ids
in an open-addressed table. On a realistic mixed flow over a 5,000-order book
it handles an operation in **50 ns at p50 and 190 ns at p99**, down
from 100 ns and 461 ns for the `std::map` + `std::list` book it
started as (Ryzen 7 7735HS, one pinned core).

## Build and run

CMake 3.20 or later and a C++20 compiler. The first configure fetches Catch2.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j   # build
./build/tests                                                              # run the tests
./build/bench                                                              # benchmark: 5 runs x 2M timed operations
```

And for going further:

```sh
./build/bench --depth 200000 --ops 1000000   # a book that outgrows the caches
./build/difftest 1000000                     # differential fuzzing against a naive reference book
python3 tools/journey.py                     # rebuild every optimization pass from its commit and time them together
```

A Debug build (`-DCMAKE_BUILD_TYPE=Debug`) checks every invariant after every
operation, and `bench` refuses to run in one. Sanitizer builds are under
[Correctness](#correctness).

```cpp
lob::OrderBook book;
book.add_limit(1, /*owner=*/7, lob::Side::Sell, 100, 10);
auto rep = book.add_limit(2, /*owner=*/8, lob::Side::Buy, 100, 4);
book.drain_trades([](const lob::Trade& t) { /* ... */ });
```

Trades print at the resting (maker) order's price. `modify` is cancel plus add:
the order keeps its id and owner, loses time priority, and may cross.

## Design

Three structures carry the whole book, and each replaces a standard container
that costs a pointer chase per access:

```
bids   PriceLadder: one level per price, indexed by price - base      (asks: the mirror image)
       +---------+---------+---------+---------+---------+
  ...  |  9,997  |  9,998  |  9,999  | 10,000  |  empty  |  ...      best bid: a stored index
       +---------+---------+---------+----+----+---------+
                                          |   level: total qty, head, tail -- 16 bytes
                                          v
orders SlotPool:            [17] <---> [4] <---> [90]                 the level's FIFO queue
                             ^    slot: id, price, qty, owner, prev, next -- 32 bytes
                             |
ids    IdIndex:   order id --+--> slot << 1 | side                    open addressing
```

| Operation | Cost | What it touches |
|---|---|---|
| Add that rests | O(1) | Id index insert, a slot off the free list, append at the level's tail |
| Cancel | O(1) expected | Id index lookup and erase, unlink from the queue, slot back on the free list |
| Match | O(fills + levels emptied) | Per fill: the queue's head, an id index erase, one trade record |
| Best bid or ask | O(1) | A stored index |
| Best level empties | O(ticks to the next level) | A scan outward, bounded by the window |
| Price outside the window | O(log levels) | The fallback `std::map` |

### Price levels: a flat array indexed by price

Each side of the book is a `PriceLadder`: an array of price levels covering a
window of consecutive prices, indexed by `price - base`. A `std::map` is a
red-black tree, and each lookup is a chain of dependent loads through nodes
scattered across the heap. The array makes a level **one subtraction and one
load**, with neighbouring prices in adjacent cache lines, four 16-byte levels to
a line. The best price is a stored index. When that level empties, the ladder
scans outward to the next occupied price, which in a live market is a tick or
two away.

The window is a real bound, and these are its tradeoffs:

- **Prices outside it fall back to a `std::map`.** They trade correctly and
  appear in book order, but pay the tree's cost.
  `levels_opened_outside_window()` counts every level that took that path; the
  benchmark reports 0.
- **The window never moves.** It is centred on the first price that rests, or
  pinned with `Config::price_base`. Re-centring would move every resting level
  at once, a stop-the-world pause in the middle of trading. A deployment should
  size the window from the instrument's known bounds instead: its tick size,
  daily price bands, and limit-up and limit-down levels.
- **Memory is spent up front,** 16 bytes per price per side, occupied or not.
  The default 1,024 levels are 16 KiB a side; the benchmark's 4,096 are 64 KiB.
- **A thin book scans further.** Finding a new best walks every empty level in
  between, bounded by the window rather than by the number of levels. An
  occupancy bitmap would make it a few word scans, but on this flow the next
  level is almost always adjacent.
- **Prices are ticks.** A feed with decimal prices converts on the way in, as
  `(price - base_price) / tick_size`.

`Config::price_levels = 0` puts every level in the map, so one binary can
measure the array against its absence (`bench --price-levels 0`).

### Orders: a slot pool, queued by intrusive 32-bit links

Every resting order is one 32-byte slot in a `SlotPool`: a single contiguous
block, allocated and touched at construction and sized by
`Config::expected_orders`. A slot holds exactly what matching reads -- id,
price, quantity and owner, 24 bytes -- plus `prev` and `next` indices. Each
price level's FIFO queue is threaded through those links, and the level itself
holds only the head, the tail and the total quantity. Appending, cancelling from
the middle and filling from the front are each a few index writes: O(1), with no
list node and no allocator call. Freed slots go on an intrusive free list, LIFO,
so the slot a cancel frees is the next one an add takes, still in cache.

Why this shape rather than `std::list`:

- **Indices, not pointers.** A 32-bit link is half a pointer, so more of each
  cache line is order. Indices also survive the block moving, which is what
  lets the pool grow.
- **32 bytes, aligned to 32.** Two slots to a cache line and none straddles
  one. `static_assert` holds both the size and the alignment.
- **Only hot fields.** The side is the one field matching never reads: it knows
  the side from the ladder it walks, and cancel, modify and `find()` reach an
  order through the id index. So the side rides in bit 0 of the index entry,
  which a cancel has already loaded. The owner stays in the slot, because
  self-trade prevention compares it at every resting order a taker touches.
  With no timestamps or flags there is no cold data, so there is no cold array.

The tradeoffs:

- **Growth is a stall, not a failure.** An exhausted pool doubles and copies
  itself once. `order_pool_growths()` counts it, and the benchmark reports it:
  never, on its flow. Size `expected_orders` for the peak book and it never
  happens while matching.
- **`find()` returns a copy.** An order's fields live in its slot and its index
  entry, so there is no stored `Order` to point at, and a growth moves every
  slot anyway.
- **Memory is reserved up front,** 32 bytes a slot, used or not.
- **A book holds at most 2^31 resting orders,** since the index spends one bit
  on the side. Past that it throws rather than wrapping.

### Cancel by id: an open-addressed table

`IdIndex` maps an order id to its slot and side. `std::unordered_map` chains:
every lookup loads the bucket array, then follows a pointer to a node elsewhere
on the heap, two dependent cache misses on a big book, plus a node allocated per
add and freed per cancel. `IdIndex` is one array of 16-byte entries, four to a
cache line:

- **The hash keeps sequential ids sequential:** `(id ^ id >> log2(capacity))`,
  masked. Venues hand out ids in order, so consecutive adds land in consecutive
  entries, in lines already fetched, and cancels of recent orders hit lines still
  in cache. Folding in the high bits stops ids exactly a table apart from all
  landing on one entry.
- **Linear probing.** A collision moves to the next entry, usually in the same
  line and otherwise the one the prefetcher is already fetching.
- **Power-of-two capacity, at most half full.** Probe runs stay short, and the
  home entry is a mask rather than a division.
- **Backward-shift deletion.** An erase pulls the entries after it back into the
  gap instead of leaving a tombstone, so millions of adds and cancels never slow
  the table down.

The hash was chosen by measurement. Fibonacci hashing, the textbook choice,
scatters sequential ids evenly and won on the default book. On a
200,000-order book it more than doubled p50, 70 → 170 ns, because the table there is 16 MiB and every
scattered add lands on a cold line. The [journey](#optimization-journey) has the
numbers.

The tradeoffs:

- **Growth rehashes the whole table** once, when it passes half full.
  `id_index_growths()` counts it: never, on the benchmark's flow.
  `Config::expected_orders` sizes the table up front.
- **2 to 4 entries per expected order.** The benchmark's 10,000-order provision
  is a 32,768-entry table, 512 KiB, mostly empty. A lookup touches one line.
- **An erase scans to the end of its run.** Sequential ids make the newest
  orders one dense run, so a cancel checks the orders added after it, a handful
  on this flow. Robin Hood ordering would let it stop sooner.
- **Ids nobody chose to collide.** Crafted keys defeat this hash easily, which is
  acceptable because ids come from the venue, not from a client choosing them.
  Client-supplied ids would want a keyed hash.

### Big arrays ask for huge pages

On a 200,000-order book the pool and the id index are each over 10 MiB. A
random access into 16 MiB of 4 KiB pages lands on one of 4,096 pages, more than
the second-level TLB holds, so a lookup can miss the TLB as well as the cache.
The pool, the index and the price window allocate through `HugePageAllocator`:

- **Blocks of 2 MiB or more are mapped fresh** with `mmap`, trimmed to a 2 MiB
  boundary and marked `MADV_HUGEPAGE`, so the kernel backs them with 2 MiB pages
  from the first touch. Heap memory from malloc may already be faulted in as
  small pages, which the advice cannot change after the fact.
- **Smaller blocks take `std::allocator`'s own path, exactly.** A huge page for
  a few hundred KiB would mostly be waste.
- **It is advice.** With transparent huge pages off, or none free, the memory is
  ordinary pages and everything still works. The benchmark reports what the
  kernel actually did, from `/proc/self/smaps_rollup`: 30 MiB of the
  200,000-order book on 2 MiB pages, none of the default book.
- **Rounding up wastes up to 2 MiB per array,** which is noise beside the
  arrays that qualify.

### The fallback map's nodes come from a per-book arena

The one node-based container left is the fallback `std::map`, and its nodes come
from `lob::NodeArena`: 64 KiB slabs, one LIFO free list per 16-byte size class,
no headers (the container hands the size back), provisioned and touched at
construction. One arena per book keeps the engine free of global state. Before
the slot pool and the flat id index it served every list and hash node too, and
it was the first change measured. `Config::pool_nodes = false` puts the nodes
back on malloc.

### Trades go to a ring buffer

Fills are written to a fixed-capacity `TradeRing` that the caller drains between
orders. The matching loop does no formatting, no I/O and no allocation, and
`ExecReport` holds no container: it names its fills as `[first_seq, first_seq +
trade_count)`. When the ring is full a trade is **dropped and counted**
(`TradeRing::dropped()`), never lost quietly. Matching itself stays correct
either way.

### Self-trade prevention cancels the resting order

When an incoming order would match a resting order with the same `owner`, the
default `SelfTradePolicy::CancelResting` removes the resting order without
printing a trade, and the taker keeps walking the book.

| Policy | Trade prints? | Resting order | Incoming order |
|---|---|---|---|
| `CancelResting` *(default)* | No | Cancelled | Keeps matching deeper |
| `CancelIncoming` | No | Untouched | Stops, remainder dropped, never rests |
| `Allow` | Yes (wash) | Filled normally | Filled normally |

The aggressor has just expressed current intent, while the resting order is a
stale quote from the same desk. Cancelling the maker honours the taker, which
still fills against everyone else at that level. The cost is that other
participants see liquidity vanish with no print, so `stp_cancelled` is reported
on every `ExecReport`. Orders owned by `kAnonymous` (0) never trigger
prevention.

### The engine is a pure function of its input

No clocks, no randomness, no global or static state. Time priority is arrival
order, and the trade sequence counter belongs to the book. The same orders in
produce the same trades out, which is checked by replaying one sequence into two
books, and holds across processes and across `-O0` and `-O2`. It is what makes
replay testing and A/B benchmarking meaningful.

## Optimization journey

One change at a time, each measured against the commit before it. The table
re-measures every pass together: each built from its own commit (the rejected
ones from their parent plus a patch in `tools/journey/`), 15 interleaved rounds
of one run each, 2M timed operations per run on the default 5,000-order book,
latency over all operations. Every cell is the median of 15 runs, in ns. A
change is judged against the row it was built on, paired by round with a sign
test, and the notes cite only differences that test marks significant.

| Pass | p50 | p99 | p99.9 | Note |
|---|---|---|---|---|
| Baseline (`std::map` + `std::list`) | 100 | 461 | 1,092 | Levels in a red-black tree, a `std::list` of orders per level, ids in `std::unordered_map`, every node from malloc |
| *Rejected: `std::pmr` pool* | *140* | *491* | *1,092* | *Worse at every p50 (+40%, lost all 15 rounds). glibc's per-thread cache is already a fast free list, and a general-purpose pool only adds bookkeeping to it* |
| Pooled allocation: node arena | 80 | 311 | 731 | The same containers, their nodes from a per-book LIFO free list. p99 and p99.9 −33%, winning all 15 rounds, and every operation type improved: malloc and free were a third of the tail |
| Flat price array | 70 | 291 | 691 | A level is a subtraction and a load, not a tree walk: cancel p50 70 → 40, modify 160 → 110. Marketable orders did not move, because a sweep spends its time erasing orders and writing trades, not finding levels |
| Intrusive lists (slot pool) | 70 | 271 | 671 | Orders in 40-byte pool slots, queued by 32-bit links. Mostly add p50 (−12%) and the add, cancel and modify p99s (−7 to −12%): with malloc already gone and a 5,000-order book in L2 either way, all that was left to win was size and locality |
| *Rejected: Fibonacci-hashed id index* | *50* | *180* | *471* | *The best p99 of any row on this book, and still rejected: on the 200,000-order book, below, it more than doubled p50, 70 → 170 ns, in every round. Scattered ids put every add on a cold line of a 16 MiB table* |
| Cache layout: flat id index | 50 | 200 | 451 | Open addressing with a locality-preserving hash. p99 −26%, p99.9 −33%, marketable p99.9 −44%: two dependent heap hops and a node per order gone. Cancel p50 got a step slower (40 → 50), because an erase shifts back the run of newer entries |
| Cache layout: 32-byte hot slot | 50 | 210 | 461 | Cancel p50 50 → 40 and nothing else: 10,000 slots fit in L2 at 40 bytes or at 32 |
| Cache layout: 16-byte levels | 50 | 200 | 501 | Nothing. 4,096 levels are 96 KiB or 64 KiB, in L2 either way, and a sweep touches a few adjacent levels regardless. Kept because it is smaller |
| *Rejected: specialize on side* | *50* | *210* | *511* | *Worse: marketable p50 90 → 100 (lost 9 rounds, won none) and cancel p50 40 → 50 (lost 9, won 1). The side test it removed was already perfectly predicted, so it won nothing and moved code around* |
| Branch tuning: profiled hints | 50 | 190 | 461 | `[[likely]]`/`[[unlikely]]` only where a gcov profile showed a branch at least 95% one way, and the self-trade test hoisted out of the loop. Add and modify p99 −8%, add and cancel p99.9 −9% and −12% |
| *Rejected: branchless unlink* | *50* | *200* | *461* | *Modify p50 90 → 80 and nothing else. Its two branches look unpredictable by count (80/20, 62/38) but follow a pattern -- cancels take a queue's tail, fills its head -- that the predictor learns, so they were nearly free already* |
| Huge pages | 50 | 190 | 441 | No array on this book reaches 2 MiB, so it allocates exactly as before. Add and modify p99.9 still lost 11 and 12 rounds by about a step, which the change's own A/B did not show: layout, or chance. It is aimed at the deep book, below, where p99 fell 9% |

From the baseline to the last row, p50 halved, p99 fell 59% and p99.9 60%.

The order differs from the usual one -- flat array, then pooling, then intrusive
lists -- because the tail histogram pointed at malloc before any pass began, so
pooling came first. Here that was the arena under the standard containers. The
intrusive lists then came in the same change as the slot pool that holds them,
which also retired the arena for everything but the fallback map.

### The same passes on a book that outgrows the cache

The default book fits in L2, which hides whatever a change does to memory
traffic. At `--depth 200000 --ops 1000000` the order pool and id index are
13 and 16 MiB, as large as the L3, and the passes separate differently:

| Pass | p50 | p99 | p99.9 | On this book |
|---|---|---|---|---|
| Baseline (`std::map` + `std::list`) | 110 | 1,182 | 2,996 | |
| *Rejected: `std::pmr` pool* | *230* | *1,343* | *3,407* | *p50 more than doubled, add p99 +83%, modify p99 +31%: worse still than on the small book* |
| Pooled allocation: node arena | 80 | 992 | 2,355 | p99 −16% and p99.9 −21%, winning 14 and 15 of 15 rounds. Half the default book's gain, because cache misses now share the tail with malloc |
| Flat price array | 70 | 932 | 2,314 | Cancel p50 60 → 40 and p99 −11%, the same shape as on the default book |
| Intrusive lists (slot pool) | 70 | 932 | 2,385 | Add better at every percentile, p99 −16%. Cancel p50 a step slower (lost 9 rounds, won none) and marketable p99 +5%. Cause not found: the obvious suspect, a cancel waiting on the slot's line to learn its level, was tested by putting price and side in the index entry, and nothing changed |
| *Rejected: Fibonacci-hashed id index* | *170* | *611* | *1,323* | *p50 70 → 170, add p50 70 → 180 and add p99 +22%, losing every round. The table is 16 MiB, and a scattered add lands on a cold line every time. This is the run that decided the hash* |
| Cache layout: flat id index | 50 | 541 | 1,032 | The largest step on either book: p99 −42%, p99.9 −57%, marketable p99.9 −65%, every round. A lookup was two dependent misses and is now one |
| Cache layout: 32-byte hot slot | 50 | 531 | 1,032 | One significant cell, marketable p99.9 −1%, and nothing else: each operation still reads one slot line either way |
| Cache layout: 16-byte levels | 50 | 521 | 992 | Nothing |
| *Rejected: specialize on side* | *50* | *521* | *1,062* | *Modify p50 a step faster and nothing else. The default book's losses decided it* |
| Branch tuning: profiled hints | 50 | 531 | 1,002 | Modify p50 90 → 80, nothing else. Branches are not what limits a book that misses the cache |
| *Rejected: branchless unlink* | *50* | *521* | *1,032* | *Cancel and modify p99 a step faster, in 12 of 14 and 12 of 15 rounds. Its own A/B saw those cells lean the other way, so the gain does not hold across sessions, and a step is not worth the obscure code* |
| Huge pages | 50 | 481 | 992 | p99 −9% (13 of 15 rounds), cancel p99 −12%, modify −9%. A cancel makes three dependent accesses -- index entry, slot, level -- and huge pages take the TLB miss out of each. Modify p50 a step slower |

From the baseline to the last row, p50 fell 55%, p99 59% and p99.9 67%.

What is left on this book is those three dependent accesses. Smaller structures
did not make them fewer, and huge pages only made each cheaper. Making them
fewer is the next lever; Robin Hood ordering in the id index, which would cut
the scan an erase makes, is another.

### The allocation tail, before and after

The arena's own commit, `4c94921`, with the same binary, flow and seed: every
container node on malloc (`--system-alloc`), then on the node arena. Three runs
of 2M operations each:

![Latency histogram: system malloc vs node arena](docs/latency-histogram.svg)

On log-log axes the tail is visible rather than a sliver beside a spike. The two
distributions peak together near 100 ns. From about 200 ns to 5 µs the malloc
curve sits two to three times higher, and that gap is malloc and free: nothing
else differs between the two runs. Past 10 µs both meet the gray curve, a
busy-wait with no engine at all -- interrupts and hypervisor exits, the machine
rather than the code.

![Latency by percentile: system malloc vs node arena](docs/latency-percentiles.svg)

By percentile, the curves separate from p90 onward and stay apart until about
p99.98, where all three jump to the machine's floor together. On today's
engine `--system-alloc` moves only the fallback map's nodes, since orders no
longer live in container nodes, so the figures are redrawn from that commit's
build:

```sh
python3 tools/journey.py --rounds 0 --book default    # builds every pass, running none
B=build-journey/arena/build/bench
$B --runs 3 --system-alloc --dump build-journey/malloc.csv
$B --runs 3 --dump build-journey/arena.csv
python3 tools/plot_latency.py --out docs/latency \
    "system malloc=build-journey/malloc.csv" "node arena=build-journey/arena.csv"
```

[docs/optimization-log.md](docs/optimization-log.md) has each change's full
comparison as it was made, by operation type, with the branch profile behind
the hints.

## Measurement methodology

`bench` times every operation on its own and reports the distribution, never
the mean. A mean of 80 ns with a p99.9 of 40 µs describes a worse engine than a
mean of 120 ns with a p99.9 of 300 ns, and an average cannot tell them apart.

### Machine and build

| | |
|---|---|
| CPU | AMD Ryzen 7 7735HS (Zen 3+), 8 cores, 16 threads. 32 KiB L1d and 512 KiB L2 per core, 16 MiB L3 shared |
| OS | Linux 6.6.87.2 under WSL2 on Windows, 30 GiB visible. Transparent huge pages: `madvise` |
| Compiler | GCC 15.2.0, CMake 4.2.3 |
| Flags | CMake's `Release`: `-O3 -DNDEBUG -std=gnu++20`, plus `-Wall -Wextra -Wpedantic`. No `-march=native`, no LTO, no PGO |
| Clock | TSC at 3.194 GHz, read with `rdtscp`. It advances in steps of 32 ticks here, so every sample is a multiple of 10 ns, and a 10% change at p50 is one step |

WSL2 has no cpufreq interface -- the Windows host owns the clock -- so the
governor and boost cannot be fixed from inside it. For tails worth quoting, use
bare-metal Linux with `cpupower frequency-set -g performance`, boost off, and an
isolated core (`isolcpus`, `nohz_full`) passed as `--cpu`.

### The flow looks like a real feed

A benchmark only describes the flow it runs. Uniformly random orders mostly miss
each other, so a naive generator times a book that does little but insert.
`bench/flow.hpp` generates this instead:

| Operation | Share | Shape |
|---|---|---|
| Add | 50% | Passive. 80% rest within a few ticks of the touch, geometrically; the rest spread up to 100 ticks deep |
| Cancel | 40% | 80% hit one of the newest orders, each step further back less likely; the rest hit any live order |
| Modify | 5% | Re-priced near the touch (cancel plus add) |
| Marketable | 5% | Market orders and limits priced through the touch -- the only operations that trade |

- **Prices** are offsets from a mid that random-walks one tick at a time, about
  once every hundred operations. The spread is one tick at p50 and two at p99.
- **Depth holds steady.** 5,000 passive adds build the book first. After that,
  each marketable order is sized to take about two resting orders, more when the
  book is thick and fewer when it is thin, so depth stays between 4,432 and
  5,532 orders over 2M operations. `--depth 200000` builds the deep book the
  same way.
- **Every operation does what its label says.** Generation drives its own copy
  of the engine, so cancels and modifies always name a resting order, passive
  adds never cross, and marketable orders are sized against real depth. A
  cancel of an unknown id is a single hash miss, and counting those would
  flatter the cancel numbers.
- **Pre-generated and reproducible.** The whole sequence goes into one flat
  array before timing starts. Generation is integer-only, so a seed gives the
  same flow everywhere; the report prints a flow hash, and every run's trade
  stream is hashed and checked against the generator's.

`tests/test_flow.cpp` holds the generator to all of the above.

### Timing one operation

| Rule | What `bench` does | How the output shows it |
|---|---|---|
| Keep the optimizer from deleting work | Every result passes through `do_not_optimize()`, an empty inline-asm barrier (the trick behind `benchmark::DoNotOptimize`), inside the timed bracket | Built with `-flto` as a check, every engine call stays inside the timed loop and the trade streams still match |
| Warm up | 5,000 depth-building adds, then 300,000 mixed operations (`--warmup`) through the same never-inlined timing loop, so the branch predictor is trained on the code that is then timed | The header states it |
| Pin to a core | `pthread_setaffinity_np` to `--cpu`, or else the last CPU allowed, since CPU 0 takes the most interrupts | The header names the CPU; it is re-checked after every run |
| Hold the clock steady | Reads the governor and boost; warns unless they are `performance` and off. Spins 200 ms at full load right before the first run | The header |
| Pre-fault memory | Flow and sample buffers are written before timing. The book is provisioned for twice its target depth, so its pool, index and arena are touched at construction. On glibc the heap is grown by 64 MiB (`--prefault`), touched, and trimming turned off | Page faults inside each timed pass are counted: 0 in every run |
| `rdtscp`, not `std::chrono` | Each sample is one engine call between `lfence; rdtsc; lfence` and `rdtscp; lfence`, stored in ticks and converted once, in the report | The header prints the TSC rate and resolution |
| Subtract the timer | The median cost of an empty bracket, 96 ticks (30 ns) here, is subtracted from every sample | The header |
| Repeat and report variance | 5 runs (`--runs`) of 2M timed operations, each on a fresh book. The report gives the median of each percentile across runs, and each cell's spread | The spread table |

Options: `--ops`, `--warmup`, `--runs`, `--seed`, `--depth`, `--cpu` or
`--no-pin`, `--prefault`, `--price-levels` (0 switches the flat array off),
`--system-alloc` (the fallback map's nodes on malloc), and `--dump FILE`,
which writes every sample, engine and noise floor, as `run,index,kind,ns` for
`tools/plot_latency.py`.

### What a run prints

`./build/bench` on the final commit, as it prints, after its header:

```
runs (ns over all timed ops; page faults and context switches while timing)
  run 1  p50 50  p99 210  p99.9 611  p99.99 2,314  max 516,441   faults 0  switches 0
  run 2  p50 50  p99 200  p99.9 521  p99.99 2,655  max 1,343,185   faults 0  switches 0
  run 3  p50 50  p99 190  p99.9 521  p99.99 2,074  max 714,370   faults 0  switches 0
  run 4  p50 50  p99 200  p99.9 491  p99.99 12,785  max 566,118   faults 0  switches 2
  run 5  p50 50  p99 200  p99.9 571  p99.99 1,994  max 329,948   faults 0  switches 0
  trade stream 27a1985ada10feed in every run, matching the generator
  huge pages   0 MiB of the book on 2 MiB pages (transparent huge pages: madvise)
  orders       pool and id index never outgrew 10,000 orders
  prices       flat array of 4,096 levels from 97,957; 0 levels opened outside it

latency (ns), median of 5 runs
                    count      p50      p99    p99.9   p99.99        max
  all           2,000,000       50      200      521    2,314    566,118
  add             999,655       50      130      371    1,483    298,307
  cancel          799,831       40      130      481    1,603    516,441
  modify          100,354       90      250      812   17,013     91,497
  marketable      100,160       90      451    1,242   20,129    140,553
  (spin)        2,000,000       70      100      120   11,823    871,094

spread across runs, (max - min) / median
                                p50      p99    p99.9   p99.99        max
  all                           0%      10%      23%     466%       179%
  add                           0%       8%      27%     962%       390%
  cancel                        0%      23%      42%     674%       120%
  modify                        0%       8%      26%      36%       318%
  marketable                    0%      16%      10%      76%       189%
  (spin)                        0%      20%     100%     100%       192%
  A difference smaller than its cell's spread is noise, not a result.
```

**p50 through p99.9 are the engine.** The `(spin)` row busy-waits for as long
as the median operation, as many times, with no engine involved. Interrupts and
hypervisor exits land in a window in proportion to its length, so that row is
the machine's own contribution at each percentile, and through p99.9 it stays
well below the operations.

**p99.99 and max are the machine.** Spinning with no engine involved reaches
the same microseconds at p99.99 that modify and marketable do. Add and cancel
are short enough that interrupts land in roughly 0.01% of them, so their p99.99
swings between sessions from about 2 µs to the machine's floor above 10 µs.
Neither is a result.

### Comparing versions

A spread table answers "how noisy is one configuration?" An optimization asks
something else: is B faster than A? Comparing two separate five-run reports
answers that badly, because one disturbed run widens a spread enough to hide a
real 30% gain. So both tools that compare builds interleave them:

- **`tools/ab_compare.py`** runs two builds alternately, one run each per pair,
  so drift in the machine lands on both sides. For each cell it reports the
  median across pairs and how many pairs the change won. If the change did
  nothing, each pair is a coin flip, and a two-sided sign test turns the win
  count into a p-value; a cell is marked only under 0.05. At 15 pairs that takes
  12 wins. Every change in the journey was measured this way against its parent
  commit, and [docs/optimization-log.md](docs/optimization-log.md) keeps those
  tables.
- **`tools/journey.py`** does the same for every pass at once. It builds each
  from the commit that introduced it, or its parent plus a patch from
  `tools/journey/` for the rejected ones, with the same compiler and flags. It
  then runs rounds in which every build runs once, rotating the order, and
  pairs each pass with its parent by round.

Both check that every run timed the same flow, by its hash.

Pairing matters more than it sounds. In the journey's default-book session the
first five rounds ran on a noisier machine than the last ten, lifting every
build's p99.9 in those rounds. The flat id index then lowered cancel p99.9 in
12 of 15 rounds while the two builds' medians, taken separately, went *up*,
341 → 361 ns. The round-by-round pairing sees through the drift; the medians do
not.

Two limits of the sign test are worth stating. It asks only whether B wins
consistently, not by how much, and a p99.9 that moves by one 10 ns step still
counts. And each comparison tests 15 cells at 5%, so a change that did nothing
still earns a star somewhere fairly often. A result is a pattern of stars, or
one that repeats in a second session, not a lone one.

```sh
python3 tools/ab_compare.py --pairs 15 ../old/build/bench build/bench
python3 tools/ab_compare.py --pairs 15 ../old/build/bench build/bench -- --depth 200000 --ops 1000000
python3 tools/journey.py --rounds 15      # 13 builds x 15 rounds x 2 books: about 25 minutes
```

## Correctness

Four layers, each catching what the one above it misses.

| Layer | How much | Where it runs |
|---|---|---|
| Unit tests (Catch2) | 122 test cases, 201,845 assertions | Debug, Release, ASan + UBSan, TSan |
| Differential tests in the suite | 13,600 random sequences of 40 to 250 operations, checked against a naive reference book | All four builds |
| Differential campaigns (`difftest`) | 2,000,000 sequences of 40 operations and 400,000 of 200: 160 million operations, no divergence, on the final commit | Release |
| Invariant checks | The whole book, after every mutating call | Debug and sanitizer builds |

Before every optimization commit: the Debug and Release suites, `difftest` at
1,000,000 sequences of 40 operations and 200,000 of 200 in Release, the ASan +
UBSan suite with 20,000 more sequences, and the TSan suite.

### Unit tests

One behaviour per test, written as a scenario: set up a book, send one order,
assert on the trades and the resulting book. They cover every operation and the
edges -- zero quantities, duplicate ids across sides, price `0` and
`UINT64_MAX`, level volume past 2^32, a level emptied and reused, ring capacity
boundaries, each self-trade policy. Each data structure is also tested alone:

| File | Tests | Covers |
|---|---|---|
| `test_order_book.cpp` | 44 | Matching, cancel, modify, self-trade prevention, the trade ring, determinism |
| `test_differential.cpp` | 17 | The differential sweeps below, and checks that the harness itself catches a divergence |
| `test_price_window.cpp` | 15 | The flat array, its edges, and the fallback map |
| `test_flow.cpp` | 13 | The benchmark's generator does what it claims |
| `test_slot_pool.cpp` | 11 | Free-list order, growth that keeps every index, alignment, the book on a tiny pool |
| `test_node_arena.cpp` | 10 | The arena, and zero calls to `operator new` during a benchmark flow |
| `test_id_index.cpp` | 7 | Lookups, growth, strided ids, and 60,000 random inserts and deletes on a small table against `std::unordered_map`, checking every entry is reachable after each |
| `test_huge_pages.cpp` | 3 | Alignment on both allocation paths, and a vector growing across the 2 MiB threshold |
| `test_threading.cpp` | 2 | Distinct books on distinct threads, and one book handed between threads |

### Differential testing

This is the layer that finds the real bugs. `tests/reference_book.hpp` is a
second, deliberately naive book: every resting order in one flat vector, every
operation a linear scan. It shares no data structure with the engine, so when
the two disagree one of them is genuinely wrong. Both are driven with the same
random sequence, and three things must match: the trade streams byte for byte,
the `ExecReport` of every operation, and the resulting queues order for order.

The generator is biased toward the cases that exercise matching:

- prices in a 7-tick band, so orders cross constantly;
- few distinct prices and small quantities, for deep queues and a mix of exact
  and partial fills;
- three participants plus anonymous orders, so self-trade prevention fires
  constantly, under all three policies;
- cancels and modifies aimed at recently touched ids, which have often just
  filled, plus never-issued and duplicate ids;
- price windows of every shape against that band -- narrower than it, one level
  wide, absent, missing it entirely, against the top of the price range -- so
  levels open on both sides of the window's edges;
- an order pool that starts at one slot, so it grows, and moves every order,
  throughout.

A diverging sequence is shrunk by greedy delta debugging, chunks and then single
operations, and printed as pasteable C++:

```
DIVERGENCE at seed 1: shrunk 40 ops -> 3
lob::OrderBook book(lob::Config{lob::SelfTradePolicy::Allow, 4096});
book.add_limit(23, 3, Side::Sell, 997, 6);
book.add_limit(24, 3, Side::Sell, 997, 5);
book.add_limit(29, 3, Side::Buy, 997, 3);
```

The layer is checked by planting bugs and confirming it catches them: a trade at
the taker's price, LIFO instead of FIFO, a halted taker resting its remainder, a
best-price lookup that ignores the fallback map, an id index erase that skips
the backward shift, an unlink that leaves a stale back link, a slot leaked
instead of freed. Each was caught. The matching and window bugs shrink to two or
three operations; the index and pool bugs trip the invariant checks first.
Chasing the halted taker also surfaced a real flaw: it used to report
`remaining == 0`, indistinguishable from a complete fill.

```sh
./build/difftest [sequences] [first_seed] [ops_per_sequence]   # exits non-zero on the first divergence
```

### Invariants

`OrderBook::check_invariants()` runs after every mutating call when `NDEBUG` is
not defined and compiles to nothing when it is, so corruption is caught at the
operation that caused it. It checks that:

- best bid is strictly below best ask;
- each level's total equals the sum of its orders, no level is empty, and every
  resting order has quantity above zero;
- each queue's links agree in both directions, and its tail is its last order;
- the id index holds exactly one entry per resting order, naming its slot and
  side, and every entry is reachable by probing from its home;
- the pool's free list is acyclic and, with the live slots, accounts for every
  slot ever handed out;
- the price window's occupied count and best index are right, and no price
  inside the window is filed in the map.

Level totals are 64-bit while an order's quantity is 32-bit, because one level
can hold many near-maximum orders, and an invariant that summed in the narrower
type would wrap along with the bug.

### Sanitizers

Each sanitizer gets its own tree, since ASan and TSan cannot share a binary, and
the flag is applied before Catch2 is fetched so the framework is instrumented
too.

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZER=address && cmake --build build-asan -j
./build-asan/tests && ./build-asan/difftest 20000
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZER=thread && cmake --build build-tsan -j
./build-tsan/tests
```

- **AddressSanitizer + UndefinedBehaviorSanitizer.** The engine keeps slot
  indices and releases slots mid-match, the shape of code that produces
  use-after-free. The pool poisons each released slot until it is handed out
  again, so a stale index that reads one is reported. The node arena is
  bypassed, so map nodes go through ASan's own allocator, poisoned and
  quarantined. Two tests skip here by design: the zero-allocation check, which
  needs the arena, and a test that corrupts the free list on purpose.
- **ThreadSanitizer.** A book is not thread-safe, but distinct books share no
  state and one book can move between threads with caller-supplied
  synchronization. `tests/test_threading.cpp` checks both, and under TSan any
  hidden shared state becomes a reported race. It is not vacuous: removing the
  lock from the hand-off makes TSan report a race in `OrderBook::submit`.
