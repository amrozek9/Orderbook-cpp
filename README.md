# lob — a limit order book

Price-time priority matching engine. Each side of the book is a flat array of
price levels indexed by price, with a `std::map` for prices outside it. Each
level is a FIFO queue of orders threaded by 32-bit index through a pre-allocated
pool of order slots, and an `unordered_map<OrderId, slot>` makes cancel-by-id
O(1). The index's nodes, and the map's, come from a per-book arena rather than
malloc.

```cpp
lob::OrderBook book;
book.add_limit(1, /*owner=*/7, lob::Side::Sell, 100, 10);
auto rep = book.add_limit(2, /*owner=*/8, lob::Side::Buy, 100, 4);
book.drain_trades([](const lob::Trade& t) { /* ... */ });
```

Trades print at the resting (maker) order's price. `modify` is cancel plus add:
the order keeps its id and owner, loses time priority, and may cross.

## Design decisions

### Self-trade prevention: cancel the resting order

When an incoming order would match a resting order with the same `owner`, the
default `SelfTradePolicy::CancelResting` **removes the resting order without
printing a trade** and lets the taker keep walking the book. All three policies
are available via `Config::self_trade`:

| Policy | Trade prints? | Resting order | Incoming order |
|---|---|---|---|
| `CancelResting` *(default)* | No | Cancelled | Keeps matching deeper |
| `CancelIncoming` | No | Untouched | Stops dead, remainder dropped, never rests |
| `Allow` | Yes (wash) | Filled normally | Filled normally |

Why `CancelResting` is the default: the aggressor has just expressed current
intent, while the resting order is a stale quote from the same desk. Cancelling
the maker honours the taker's intent — it still gets filled by everyone else at
that level — and removes the stale quote from the book, which is what the owner
would have wanted anyway. The cost is that a third party's view of available
liquidity shrinks with no print to explain it, which is why `stp_cancelled` is
reported on every `ExecReport` rather than hidden.

`CancelIncoming` is the conservative alternative: it never touches the book, so
resting queue positions are safe from anyone else's mistakes, at the cost of
silently killing an order the taker meant to send. `Allow` exists for replaying
venue data where the wash trades are already in the tape.

Orders owned by `kAnonymous` (0) never trigger prevention, which is the default
for the short `add_limit(id, side, price, qty)` overloads.

### Trade output goes through a ring buffer, never I/O

Fills are written to a fixed-capacity `TradeRing` that the caller drains between
orders. The matching loop does no formatting and no I/O -- and, with the node
arena below, no allocation. A `std::cout` or a per-order `std::vector` in there
would dominate and invalidate every measurement of the path. `ExecReport` holds no container either; it
identifies this order's fills as `[first_seq, first_seq + trade_count)`.

The ring allocates once, at construction. When it fills, the trade is **dropped
and counted** (`TradeRing::dropped()`) rather than lost quietly — a non-zero
count means the caller undersized the ring or is draining too rarely, not that
the book mismatched. Matching itself is unaffected: `ExecReport::filled` and the
book state stay correct regardless.

### Price levels live in a flat array

Each side of the book is a `PriceLadder`: an array of price levels covering a
window of consecutive prices, indexed by `price - base`. A `std::map` is a
red-black tree, and every lookup in it is a chain of dependent loads through
nodes scattered across the heap. The array replaces that with:

- **One subtraction and one load** to reach any level, with neighbouring levels
  in adjacent cache lines.
- **Top of book as a field read.** The best level is tracked as an index. When
  it empties, the ladder scans outward to the next occupied price, which in a
  live market is a tick or two away.

The window is a real bound, and these are the tradeoffs it brings:

- **Prices outside the window fall back to a `std::map`.** They trade correctly
  and appear in book order, but pay the tree's cost.
  `levels_opened_outside_window()` counts every level that took that path, and
  the benchmark reports it: 0 on its flow.
- **The window never moves.** It is centred on the first price that rests, or
  pinned with `Config::price_base`. A market that trends out of it keeps
  working at map speed. Re-centring would mean moving every resting level at
  once, a stop-the-world pause in the middle of trading. A deployment should
  instead size the window from the instrument's known bounds: its tick size,
  its daily price bands, and the limit-up and limit-down levels.
- **Memory is spent up front.** Each level costs 24 bytes per side, occupied or
  not. The default of 1,024 levels is 24 KiB per side; the benchmark's 4,096 is
  96 KiB, which competes for L2 with the orders themselves.
- **A thin book scans further.** The scan to a new best walks every empty slot
  between the old best and the next occupied one. That is bounded by the window
  rather than by the number of levels. An occupancy bitmap would turn it into a
  handful of word scans, but on this flow the next level is almost always
  adjacent.
- **Prices are ticks.** The engine indexes by `price - base` with a tick size
  of 1. A feed with decimal prices converts on the way in, as
  `(price - base_price) / tick_size`, before the order reaches the book.

`Config::price_levels = 0` puts every level in the map, which is how the same
binary measures the difference (`bench --price-levels 0`). The invariant
checker covers the window as well:

- the occupied count matches the array;
- the best index is the best occupied level;
- empty slots hold no quantity;
- no price inside the window is filed in the map.

### Orders live in a slot pool, linked by 32-bit index

Every resting order occupies one 40-byte slot in a `SlotPool`: a single
contiguous block, allocated and touched at construction and sized by
`Config::expected_orders`. A slot holds the order plus two 32-bit links, and a
price level's queue is threaded through those links: head, tail and count, 24
bytes a level. Appending, cancelling from the middle, or filling from the front
is a few index writes. There is no list node and no allocator call, not even
the arena's free list. Freed slots go on an intrusive free list, LIFO, so the
slot a cancel frees is the next one an add takes.

| | `std::list` in the arena | Slot pool |
|---|---|---|
| Per order | 48-byte list node, anywhere in the arena's slabs | 40-byte slot, contiguous with every other order |
| Id index entry | 24-byte locator (price, side, iterator); 48-byte node | 4-byte slot index; 32-byte node |
| Price level | 40 bytes, so 4,096 levels take 160 KiB per side | 24 bytes, so 96 KiB per side |
| Link | 8-byte pointer | 4-byte index |

Indices rather than pointers halve every link, so more of each cache line is
order. They also survive the block moving, which is what makes growth possible:

- **Growth is a stall, not a failure.** An exhausted pool doubles and copies
  itself once. `order_pool_growths()` counts it, and the benchmark reports it:
  never, on its flow. Size `expected_orders` for the peak book and it never
  happens on the matching path.
- **`find()` pointers are short-lived.** They are valid until the next call that
  changes the book, since a growth moves every order. The engine holds only
  indices across calls.
- **Memory is reserved up front,** 40 bytes a slot, whether used or not.
- **32-bit indices cap a book** at about 4.29 billion resting orders. Past that
  the pool throws rather than wrapping.

The guide this follows says `std::list` calls `operator new` for every order.
Here the arena had already taken the system allocator off the hot path, so what
the pool buys is size and locality. Orders now need no allocator at all. The id
index's hash nodes still come from the arena: one per resting order, and the
last per-order node left. A flat, open-addressed id index would remove it.

Measured against the `std::list` it replaced, the pool bought p50 (70 → 60 ns)
and add's p99.9; p99 overall moved within noise. The
[optimization log](#optimization-log) has the full comparison and the reasons.

The pool keeps the sanitizer story intact. Under AddressSanitizer a released
slot is poisoned until it is handed out again, so a stale index that reads it
is reported; debug builds scribble released slots with `0xDD`. The invariant
checker walks every queue both ways and ties each queued slot to the id index.
It also requires the free list and the live slots to account for every slot
ever used.

### Index and map nodes come from a per-book arena

Every resting order costs a hash-map node in the id index, plus a map node if it
opens a level outside the price window, and every cancel or fill frees them.
Before the order pool, each order cost a `std::list` node too. Through malloc,
that traffic was a large share of the latency tail -- measured, not assumed;
see [Before and after](#before-and-after-the-allocation-tail). `lob::NodeArena`
replaced it:

- **One arena per book,** so there is still no global state, and distinct books
  still run on distinct threads.
- **LIFO free lists.** Nodes are carved from 64 KiB slabs and recycled through
  one free list per 16-byte size class. The node a cancel frees is the next one
  an add reuses, still in cache. There are no headers and no searches: the
  containers hand the size back on deallocation.
- **Provisioned up front.** Slabs are touched page by page as they are
  allocated, and `Config::expected_orders` provisions them, and the id index's
  buckets, at construction. A book that stays within its provision never calls
  the system allocator or rehashes while matching. `tests/test_node_arena.cpp`
  counts every call to `operator new` during a benchmark flow to prove it, and
  runs the same flow on malloc to prove the counter works.

The containers take a stateful allocator and are otherwise the standard ones,
`std::map` and `std::unordered_map`, so no logic changed to use the arena.
`Config::pool_nodes = false` puts their nodes back on malloc, which is how the
benchmark produces both sides of a comparison from one binary. Under AddressSanitizer the arena is always bypassed,
so freed nodes stay poisoned and quarantined and a dangling locator is still
caught; debug builds scribble freed blocks with `0xDD` instead.

The standard answer, `std::pmr::unsynchronized_pool_resource`, was tried first
and made every percentile *worse*, p50 included (100 to 140 ns): glibc's
per-thread cache is already fast, and a general-purpose pool's bookkeeping
loses to it. What wins is a dedicated free list that does nothing else.

### The engine is a pure function of its input sequence

No clocks, no randomness, no global or static state. Time priority comes from
arrival order, not wall time, and the trade `seq` counter is per-book rather than
per-process. The same orders in produce the same trades out — verified in
`tests/test_order_book.cpp` by replaying one sequence into two books and
comparing the streams field by field, and confirmed to be identical across
separate processes and across `-O0`/`-O2`.

Keep it that way: it is what makes replay testing and A/B benchmarking of this
path meaningful.

### Invariants are asserted in debug builds

`OrderBook::check_invariants()` runs after every mutating call when `NDEBUG` is
not defined, and compiles to nothing when it is. Corruption is caught at the
operation that caused it rather than three operations later. It checks that:

- best bid is strictly below best ask, so the book never stays crossed;
- each level's `total_qty` equals the sum of its orders' quantities;
- no level exists with zero orders or zero quantity;
- each level's queue links agree in both directions, and its tail and count
  match the orders actually in it;
- the id index holds exactly one entry per resting order, and each entry names
  the slot where that order is queued;
- the order pool's free list is acyclic, and together with the live slots it
  accounts for every slot ever handed out;
- every resting order has quantity greater than zero.

Level aggregates are 64-bit (`lob::Volume`) while a single order's quantity is
32-bit, because one level can hold many near-max orders. Summing them in the
narrower type wraps, and an invariant that computed the sum the same way would
not notice.

## Testing

Three layers, each catching what the one above it misses.

### Unit tests

One behaviour per test, each written as a scenario: set up a book, send one
order, assert on the trades and on the resulting book state. They cover every
operation plus the edge cases -- zero quantities, duplicate ids across sides,
price `0` and `UINT64_MAX`, level volume past 2^32, emptying and reusing a
level, exact level exhaustion, ring capacity boundaries, and each self-trade
policy.

### Randomized differential testing

This is the layer that finds the real bugs. `tests/reference_book.hpp` is a
second, deliberately naive order book: every resting order lives in one flat
vector and every operation is a linear scan. It shares no data structure with
the engine, so when the two disagree, one of them is genuinely wrong.

`difftest` drives both with the same random operation sequence and requires
three things to match: the trade streams byte for byte, the `ExecReport`
returned by every single operation, and the resulting queues order for order
rather than just membership. Comparing the reports matters because a report can
lie while the book itself stays correct. The generator is biased toward the cases that
actually exercise matching:

- prices in a 7-tick band around a single reference price, so orders cross
  constantly -- uniform prices over a wide range almost never cross, and the
  matching path would barely run;
- a handful of distinct prices and small quantities, producing deep queues at
  identical prices and a mix of exact and partial fills;
- three participants with frequent anonymous orders, so self-trade prevention
  fires constantly;
- cancels and modifies weighted toward recently touched ids, which are the ones
  most likely to have just been filled, exercising cancel-of-dead-id and index
  cleanup; plus occasional never-issued ids and duplicate live ids;
- price windows of every shape against that band. The reference has no window,
  so the flat array must be invisible. Windows are placed narrower than the
  band, centred on a varying first price, one level wide, absent, entirely
  missing the band, and pressed against the top of the price range. Levels open
  on both sides of the window's edges, and the best price keeps crossing them;
- an order pool that starts at one slot, so it grows, and moves every order,
  over and over while the sequence runs.

On a 40-operation sequence that yields about 12 trades, and every sequence
produces at least one.

When a sequence diverges it is shrunk by greedy delta debugging -- chunks first,
then single operations -- down to the smallest sequence that still diverges, and
printed as pasteable C++. Planted bugs shrink from 40 operations to two or three:

```
DIVERGENCE at seed 1: shrunk 40 ops -> 3
lob::OrderBook book(lob::Config{lob::SelfTradePolicy::Allow, 4096});
book.add_limit(23, 3, Side::Sell, 997, 6);
book.add_limit(24, 3, Side::Sell, 997, 5);
book.add_limit(29, 3, Side::Buy, 997, 3);
```

The Catch2 suite runs several thousand sequences on every build. Longer
campaigns run clean at 2,000,000 sequences of 40 operations and 400,000
sequences of 200 operations -- 160 million operations, no divergence.

The layer is checked by planting bugs in the engine and confirming they are
caught: a trade printed at the taker's price instead of the maker's, LIFO
instead of FIFO at a price level, and a self-trade-halted taker resting its
remainder. The first two shrink to two and three operations; the third trips the
crossed-book invariant in debug builds and the differential comparison in
release. Chasing that third one is also what surfaced a real flaw -- a halted
taker used to report `remaining == 0`, indistinguishable from a complete fill.

The price window was checked the same way, with two planted bugs: a best-price
lookup that ignores the fallback map, and a best-first walk that lists outside
levels in the wrong place. Both shrink to two or three operations. The order
pool was checked with two more: a removal that leaves the next order's back link
stale, and a release that leaks the slot instead of freeing it. Both abort at the
first invariant check in debug builds; in release, the leak fails five test
cases, and the stale link sends the match loop round a cycle.

For longer campaigns:

```sh
./build/difftest [sequences] [first_seed] [ops_per_sequence]
./build/difftest 2000000 1 40
```

It is deterministic: a seed always reproduces the same sequence, and it exits
non-zero on the first divergence. It rotates the self-trade policy and the price
window's shape by seed, so a long campaign covers every combination.

### Sanitizers

Sanitized builds each get their own tree, selected with `LOB_SANITIZER`, since
AddressSanitizer and ThreadSanitizer cannot be linked into the same binary. The
option is applied before Catch2 is fetched, so the test framework is
instrumented too.

**AddressSanitizer + UndefinedBehaviorSanitizer.** The full suite and the fuzzer
run clean. This matters here because the engine keeps slot indices into its
order pool and releases slots during matching -- exactly the shape of code that
produces use-after-free. Two things keep ASan able to see it. The node arena is
bypassed in this build, so every index and map node goes through ASan's own
allocator, poisoned and quarantined when freed. The order pool cannot be
bypassed that way, so it poisons each released slot until it is handed out
again. The one test that needs the arena, the zero-allocation check, is skipped
here.

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZER=address
cmake --build build-asan -j
./build-asan/tests
./build-asan/difftest 20000
```

**ThreadSanitizer.** An `OrderBook` is not thread-safe. What it does promise is
that distinct books share no state, so they can run concurrently, and that one
book can move between threads given caller-supplied synchronization.
`tests/test_threading.cpp` checks both against a single-threaded run, and under
TSan any hidden shared state -- a static cache, a global counter -- becomes a
reported race instead of an occasional wrong answer. The build runs clean, and
it is not vacuous: removing the lock from the hand-off pattern makes TSan report
a data race in `OrderBook::submit`.

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZER=thread
cmake --build build-tsan -j
./build-tsan/tests
```

TSan only sees code that actually runs concurrently. When the engine grows real
concurrency -- a feed handler thread, a lock-free queue in front of the book --
its tests belong in `test_threading.cpp` so this build keeps meaning something.

## Benchmarking

`bench` times every operation on its own and reports the distribution, never
the mean. A mean of 80 ns with a p99.9 of 40 µs describes a worse engine than a
mean of 120 ns with a p99.9 of 300 ns, and an average cannot tell them apart.

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
./build-rel/bench                          # 5 runs x 2M timed ops
./build-rel/bench --runs 10 --ops 10000000 --depth 20000
```

Options: `--ops`, `--warmup`, `--runs`, `--seed`, `--depth`, `--cpu` or
`--no-pin`, `--prefault`, `--system-alloc`, `--price-levels`, and `--dump`,
which writes every sample -- engine and noise floor -- as `run,index,kind,ns`.
`--system-alloc` puts the index and map nodes back on malloc, and
`--price-levels 0` switches off the flat price array, so one binary can measure
either against its absence. Orders always live in the slot pool.

`tools/plot_latency.py` turns dumps into the two figures below. It needs only
Python 3:

```sh
./build-rel/bench --runs 3 --system-alloc --dump build-rel/malloc.csv
./build-rel/bench --runs 3 --dump build-rel/arena.csv
python3 tools/plot_latency.py --out docs/latency \
    "system malloc=build-rel/malloc.csv" "node arena=build-rel/arena.csv"
```

A dump is about 40 bytes per sample, so three runs of 2M operations come to
roughly 250 MB. Keep dumps in the build tree. It refuses to run in a debug or sanitized build, where
`check_invariants()` walks the whole book after every operation and would be all
it measured.

### The flow looks like a real feed

A benchmark only describes the flow it runs. Uniformly random orders mostly miss
each other, so a naive generator times a book that does little but insert, and
tuning that makes the wrong path fast. `bench/flow.hpp` generates this instead:

| Operation | Share | Shape |
|---|---|---|
| Add | 50% | Passive. 80% rest within a few ticks of the touch, geometrically; the rest spread up to 100 ticks deep |
| Cancel | 40% | 80% hit one of the newest orders, each step further back less likely; the rest hit any live order |
| Modify | 5% | Re-priced near the touch (cancel plus add) |
| Marketable | 5% | Market orders and limits priced through the touch -- the only operations that trade |

- **Prices** are offsets from a mid that random-walks one tick at a time, about
  once every hundred operations. The spread stays at one tick at p50 and two at
  p99, while depth builds up behind it.
- **Depth holds steady.** Passive adds first build the book to its target depth,
  5,000 orders by default. From then on, each marketable order is sized to take
  about two resting orders, which balances 50% adds against 40% cancels. It
  takes more when the book is thick and fewer when it is thin, so depth stays
  near its target over any run length instead of drifting away: 4,432 to 5,532
  orders over 2M operations.
- **Every operation does what its label says.** Generation drives its own copy
  of the engine, so:
  - cancels and modifies always name an order resting at that moment;
  - passive adds are priced so they never cross;
  - marketable orders are sized against the depth that is actually there.

  A cancel of an unknown id is a single hash miss; counting those would flatter
  the cancel numbers.
- **Flat and pre-generated.** The whole sequence -- depth build, warm-up and
  timed operations -- goes into one flat array before timing starts, so none of
  the generation cost is measured.
- **Reproducible.** Generation uses integer arithmetic only, so a seed gives the
  same flow on every platform. The report prints a flow hash, which confirms
  that two runs timed identical input. Every run's trade stream is hashed too,
  and checked against the generator's own run before anything is reported.

`tests/test_flow.cpp` holds the generator to all of the above.

### Methodology

Each rule below says what could go wrong, what `bench` does about it, and how
the output shows it worked.

| Rule | What `bench` does | How it is checked |
|---|---|---|
| Stop the optimizer deleting work | Every result passes through `do_not_optimize()`, an empty inline-asm barrier (the trick behind `benchmark::DoNotOptimize`), inside the timed bracket | Built with `-flto`, every engine call is still inside the timed loop, and the trade streams still match |
| Warm up | Each run replays 300,000 mixed operations (`--warmup`) through the same never-inlined timing loop before recording, so the branch predictor is trained on the exact code that is then timed | The header states the warm-up each run gets |
| Pin to a core | `pthread_setaffinity_np` to `--cpu`, or else the last CPU allowed, since CPU 0 tends to take the most interrupts | The header names the CPU; it is re-checked after every run, and a run found elsewhere is flagged |
| Fix the clock frequency | Reads the cpufreq governor and boost setting; warns unless the governor is `performance` and boost is off. Calibration spins for 200 ms right before the first run, so the core is at full speed | Printed in the header |
| Pre-fault memory | Flow and sample buffers are written before timing. The book is provisioned for twice the target depth, so its order pool and its arena's slabs are allocated and touched at construction. On glibc the heap is also grown by 64 MiB (`--prefault`), every page touched, and trimming turned off, for anything still on malloc | Page faults during each timed pass are counted: 0 in every run |
| `rdtscp`, not `std::chrono` | Each sample brackets one engine call between `lfence; rdtsc; lfence` and `rdtscp; lfence`, and is stored in ticks. Conversion to nanoseconds happens once, in the report | The header prints the TSC rate and resolution |
| Subtract timer overhead | The median cost of an empty bracket, 96 ticks (30 ns) here, is subtracted from every sample | Printed in the header |
| Repeat and report variance | 5 runs (`--runs`), each on a fresh book. The report gives the median of every percentile across runs, and each cell's spread | The spread table |

Both pre-faults are guards for books larger than the default. At 5,000 orders
the depth build already maps everything the timed pass touches, and faults are 0
without either. At `--depth 200000` on malloc (`--system-alloc`), the first run
takes 13-15 page faults inside the timed pass without the heap pre-fault, even
after warm-up, and 0 with it. The arena's provisioning does the same job for the
book on its own: 0 faults at that depth even with `--prefault 0`.

On the clock:

- The fenced bracket costs about twice a bare back-to-back `rdtsc` here,
  because the fences are what stop the timed call from overlapping the clock
  reads.
- On this Zen 3 part the TSC advances in steps of 32 ticks, so every sample is a
  multiple of 10 ns. A 10% spread at p50 is one clock step.

The `(spin)` row busy-waits for as long as the median operation, as many times,
with no engine involved. Interrupts and hypervisor exits land in a window in
proportion to its length, so that row is the machine's own contribution at each
percentile.

### Comparing two versions

The spread table answers "how noisy is one configuration?" An optimization asks
something else: is B faster than A? Comparing two separate five-run reports
answers that badly on a noisy machine, because one disturbed run widens a
max-minus-min spread enough to hide a real 30% gain.

`tools/ab_compare.py` asks the question directly:

- It runs the two builds alternately, one run each per pair, so drift in the
  machine lands on both sides equally.
- It checks that every run timed the same flow.
- For each cell it reports the median across pairs and how many pairs the
  change won.

If the change did nothing, each pair is a coin flip. A sign test turns the win
count into the chance of a result at least that lopsided, and a cell is marked
only when that chance is under 5%. At 11 pairs, that takes 10 wins.

```sh
python3 tools/ab_compare.py --pairs 11 ../old/build-rel/bench build-rel/bench
python3 tools/ab_compare.py "build-rel/bench --system-alloc" build-rel/bench
```

Build the old side from the commit before the change, with the same compiler
flags. A `git worktree` with its own release tree does it.

### Results

Ryzen 7 7735HS under WSL2, GCC 15 `-O3`, 5 runs of 2M operations over a book of
about 5,000 orders, on the order pool, the node arena and the flat price array:

```
runs (ns over all timed ops; page faults and context switches while timing)
  run 1  p50 70  p99 291  p99.9 761  p99.99 18,616  max 2,132,806   faults 0  switches 0
  run 2  p50 70  p99 281  p99.9 711  p99.99 18,736  max 290,580   faults 0  switches 0
  run 3  p50 60  p99 281  p99.9 751  p99.99 13,396  max 160,138   faults 0  switches 1
  run 4  p50 70  p99 291  p99.9 751  p99.99 19,888  max 858,463   faults 0  switches 0
  run 5  p50 70  p99 281  p99.9 701  p99.99 18,586  max 1,661,059   faults 0  switches 0
  orders       pool never outgrew its 10,000 slots
  prices       flat array of 4,096 levels from 97,957; 0 levels opened outside it

latency (ns), median of 5 runs
                    count      p50      p99    p99.9   p99.99        max
  all           2,000,000       70      281      751   18,616    858,463
  add             999,655       70      180      561   18,135    290,580
  cancel          799,831       40      150      421   13,466    377,086
  modify          100,354      110      271      822   22,293     94,983
  marketable      100,160      130      701    1,773   24,688    107,156
  (spin)        2,000,000      100      110      190   15,179    575,408

spread across runs, (max - min) / median
                                p50      p99    p99.9   p99.99        max
  all                          14%       4%       8%      35%       230%
  add                           0%       6%      12%      37%       259%
  cancel                       25%      13%      33%     101%       530%
  modify                        0%       4%      20%      36%       731%
  marketable                    8%       6%      24%      46%       239%
  (spin)                       30%       9%      47%      29%      1315%
```

**p50 through p99.9 are the engine.** The spin row stays at 100-190 ns there,
while the operations are several times that.

**p99.99 and max are the machine.** Spinning for 100 ns with no engine involved
still reaches 15 µs at p99.99, the same range as every operation type. A
standalone check agrees: the count of multi-microsecond spikes grows in
proportion to the length of the timed window, which is the signature of
interrupts rather than of anything the code does.

Cancel sits right on the threshold. It is short enough that interrupts land in
roughly 0.01% of cancels, so cancel's p99.99 swings from session to session. It
has come in near 2 µs, below the machine's floor, and at the floor itself, 13
µs here. Neither value is a result.

**What a result has to beat.** In this session overall p99 moves by 4% between
runs and per-type p99.9 by up to 33%; in the last session, 19% and 108%. A
single disturbed run widens either. Read against this table alone, a change is real only when
it exceeds the spread of the cell it claims to improve, so a 3% gain in cancel
p99.9 is noise here. Comparing two versions is a different question, answered
better by `tools/ab_compare.py`, described above.

WSL2 has no cpufreq interface -- the Windows host owns the clock -- so the
governor cannot be fixed from inside it. For tails worth quoting, use bare-metal
Linux:

- `sudo cpupower frequency-set -g performance`, with boost off;
- an isolated core (`isolcpus`, `nohz_full`), passed as `--cpu`.

What the engine rows say:

- **Modify costs more than an add,** because it is a cancel plus an add.
- **Marketable orders have more than three times an add's p99.** A sweep
  erases several orders and sometimes whole levels. The flat price array barely
  moved them: their cost is the trades and the list erases, not finding the
  level.

### Optimization log

One change at a time, each measured against the commit before it with
`tools/ab_compare.py`: 11 interleaved pairs, the default flow, every operation
type. Only cells the sign test marks count.

| Change | p50 | p99 | p99.9 | Where it landed |
|---|---|---|---|---|
| Node arena instead of malloc | 100 → 80 | 451 → 321 (−29%) | 1,132 → 812 (−28%) | Every type. Marketable p99 −33%, add −34% |
| Flat price array instead of `std::map` | 80 → 70 | 301 → 271 (−10%) | 711 → 651, not significant | p99 and p99.9: cancel −33% and −34%, modify −31% and −30%, add −22% and −18%. Marketable unchanged |
| Order slot pool with 32-bit links instead of `std::list` | 70 → 60 | 261 → 250, not significant | 681 → 641, not significant | add p50 −12% and p99.9 −10%; nothing else significant |

Each row is its own paired measurement, so a row's "before" need not equal the
previous row's "after". The machine drifts between sessions, which is why a
change is judged only against its own baseline, measured alongside it. The flat
array's full comparison:

```
                                    p50                         p99                       p99.9
all              80 ->    70  -12% 11/11*     301 ->   271  -10% 11/11*     711 ->   651   -8%  8/11 
add              90 ->    80  -11% 11/11*     230 ->   180  -22% 11/11*     571 ->   471  -18%  9/11*
cancel           70 ->    40  -43% 11/11*     210 ->   140  -33% 11/11*     471 ->   311  -34% 10/11*
modify          160 ->   110  -31% 11/11*     361 ->   250  -31% 11/11*     872 ->   611  -30% 10/11*
marketable      130 ->   130   +0%  5/11      671 ->   651   -3%  6/11    1,503 -> 1,313  -13%  6/11 
```

Where the array helped most tracks what each operation does. A cancel or modify
used to find its level by walking the tree, and now reaches it with a
subtraction and a load. An add found its level the same way. A marketable order
spends its time erasing orders and emitting trades, which the array does not
touch.

The order pool's comparison is the other kind of result, and just as worth
recording:

```
                                    p50                         p99                       p99.9
all              70 ->    60  -14%  9/11*     261 ->   250   -4%  7/11      681 ->   641   -6%  7/11 
add              80 ->    70  -12% 11/11*     170 ->   160   -6%  7/11*     511 ->   461  -10%  9/11*
cancel           40 ->    40   +0%  0/11      140 ->   130   -7%  9/11      331 ->   311   -6%  7/11 
modify          110 ->   100   -9%  7/11*     240 ->   230   -4%  8/11      681 ->   661   -3%  7/11 
marketable      130 ->   130   +0%  3/11      641 ->   641   +0%  6/11    1,543 -> 1,503   -3%  7/11 
```

It bought p50, and add's p99.9, and little else. The arena had already taken
the system allocator off the hot path, so the pool's gains are size and
locality. A 5,000-order book mostly fits in cache either way. On a
200,000-order book, 9 pairs gave the same shape:

- **Add improved**, −12% at p50 and at p99.
- **Cancel's p50 moved up one clock step**, 40 → 50 ns, winning no pair.
- **Nothing else moved significantly.**

One explanation for the cancel step was that a cancel now waits for the slot to
load before it can find the level, where the old locator carried price and
side. Storing price and side beside the slot index took that load off the path,
at no memory cost, and changed nothing. So the index stays slot-only.

### Before and after: the allocation tail

The same binary, the same flow and the same seed, with container nodes on malloc
(`--system-alloc`) and then on the node arena. Three runs of 2M operations each:

![Latency histogram: system malloc vs node arena](docs/latency-histogram.svg)

On log-log axes the tail is visible rather than a sliver beside a spike. The two
distributions peak together near 100 ns. From about 200 ns to 5 µs the malloc
curve sits two to three times higher, and that gap is malloc and free: nothing
else differs between the two runs. Past 10 µs both curves meet the gray one,
which is a busy-wait with no engine at all -- interrupts and hypervisor exits,
the machine rather than the code.

![Latency by percentile: system malloc vs node arena](docs/latency-percentiles.svg)

The same data by percentile. The curves separate from p90 onward and stay apart
until about p99.98, where all three jump to the machine's floor together.

Median of 5 runs each (ns). Only changes larger than both allocators'
run-to-run spread are counted as results:

| | p50 | p99 | p99.9 |
|---|---|---|---|
| all | 100 → 80 | 421 → 301 (−29%) | 972 → 661 (−32%) |
| add | 100 → 90 | 331 → 230 (−31%) | 761 → 481 (−37%) |
| cancel | 90 → 70 | 261 → 220 (−16%) | 501 → 401, within noise (35% spread) |
| modify | 190 → 160 | 431 → 371 (−14%) | 902 → 681, within noise (53% spread) |
| marketable | 160 → 130 | 1,002 → 661 (−34%) | 2,034 → 1,212, within noise (42% spread) |

The allocator was not the whole tail, and neither was the price map or the list
nodes. At `--depth 200000`, where the working set outgrows the caches, the flat
array trimmed p99 from 982 to 912 ns and the order pool left it there. The
tail stays near 1 µs. Every operation still makes two dependent hops through
the id index: bucket array to node, then node to slot. A flat, open-addressed
id index is the next target, and this benchmark is how to tell whether it
works.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/tests
```
