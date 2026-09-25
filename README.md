# lob — a limit order book

Price-time priority matching engine. Bids and asks are `std::map` keyed by price
with opposite comparators, each price level holds a `std::list<Order>` in arrival
order, and an `unordered_map<OrderId, Locator>` makes cancel-by-id O(1).

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
orders. The matching loop does no allocation, no formatting, and no I/O — a
`std::cout` or a per-order `std::vector` in there would dominate and invalidate
every measurement of the path. `ExecReport` holds no container either; it
identifies this order's fills as `[first_seq, first_seq + trade_count)`.

The ring allocates once, at construction. When it fills, the trade is **dropped
and counted** (`TradeRing::dropped()`) rather than lost quietly — a non-zero
count means the caller undersized the ring or is draining too rarely, not that
the book mismatched. Matching itself is unaffected: `ExecReport::filled` and the
book state stay correct regardless.

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
- the id index holds exactly one entry per resting order, and every locator
  points at a live order with the price and side it claims;
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
  cleanup; plus occasional never-issued ids and duplicate live ids.

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

For longer campaigns:

```sh
./build/difftest [sequences] [first_seed] [ops_per_sequence]
./build/difftest 2000000 1 40
```

It is deterministic: a seed always reproduces the same sequence, and it exits
non-zero on the first divergence.

### Sanitizers

Sanitized builds each get their own tree, selected with `LOB_SANITIZER`, since
AddressSanitizer and ThreadSanitizer cannot be linked into the same binary. The
option is applied before Catch2 is fetched, so the test framework is
instrumented too.

**AddressSanitizer + UndefinedBehaviorSanitizer.** The full suite and the fuzzer
run clean. This matters here because the engine hands out `std::list` iterators
as locators and erases list nodes during matching -- exactly the shape of code
that produces use-after-free, and the same shape the intrusive lists and object
pools of a later phase will have.

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
./build-rel/bench --runs 10 --ops 10000000 --depth 20000 --dump build-rel/samples.csv
```

Options: `--ops`, `--warmup`, `--runs`, `--seed`, `--depth`, `--cpu` or
`--no-pin`, `--prefault`, and `--dump`, which writes every timed sample as
`run,index,kind,ns`. It refuses to run in a debug or sanitized build, where
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
| Pre-fault memory | Flow and sample buffers are written before timing. On glibc the heap is grown by 64 MiB (`--prefault`), every page touched, and trimming turned off, so engine allocations land on resident pages | Page faults during each timed pass are counted: 0 in every run |
| `rdtscp`, not `std::chrono` | Each sample brackets one engine call between `lfence; rdtsc; lfence` and `rdtscp; lfence`, and is stored in ticks. Conversion to nanoseconds happens once, in the report | The header prints the TSC rate and resolution |
| Subtract timer overhead | The median cost of an empty bracket, 96 ticks (30 ns) here, is subtracted from every sample | Printed in the header |
| Repeat and report variance | 5 runs (`--runs`), each on a fresh book. The report gives the median of every percentile across runs, and each cell's spread | The spread table |

The pre-fault is a guard for books larger than the default. At 5,000 orders the
depth build already maps everything the timed pass touches, and faults are 0
without it. With `--depth 200000`, the first run takes 13-15 page faults inside
the timed pass without it, even after warm-up, and 0 with it.

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

### Results

Ryzen 7 7735HS under WSL2, GCC 15 `-O3`, 5 runs of 2M operations over a book of
about 5,000 orders:

```
runs (ns over all timed ops; page faults and context switches while timing)
  run 1  p50 100  p99 441  p99.9 1,042  p99.99 20,538  max 1,574,932   faults 0  switches 1
  run 2  p50 100  p99 431  p99.9 1,062  p99.99 20,027  max 1,321,548   faults 0  switches 1
  run 3  p50 100  p99 441  p99.9 1,012  p99.99 20,808  max 1,620,885   faults 0  switches 1
  run 4  p50 100  p99 441  p99.9 1,062  p99.99 20,888  max 2,177,633   faults 0  switches 0
  run 5  p50 100  p99 431  p99.9 982  p99.99 20,377  max 1,684,380   faults 0  switches 0

latency (ns), median of 5 runs
                    count      p50      p99    p99.9   p99.99        max
  all           2,000,000      100      441    1,042   20,538  1,620,885
  add             999,655      100      351      822   20,848    768,626
  cancel          799,831       90      270      631   19,215  1,000,020
  modify          100,354      190      461    1,082   21,249    260,201
  marketable      100,160      160    1,032    2,164   21,860    449,044
  (spin)        2,000,000      110      140      200   17,933    781,452

spread across runs, (max - min) / median
                                p50      p99    p99.9   p99.99        max
  all                           0%       2%       8%       4%        53%
  add                          10%       3%       6%       4%       187%
  cancel                        0%       4%      27%      23%       116%
  modify                        0%      11%      18%      25%       228%
  marketable                    6%       5%      18%      44%       351%
  (spin)                        0%      21%      35%       6%       284%
```

**p50 through p99.9 are the engine.** The spin row stays at 110-200 ns there,
while the operations are several times that.

**p99.99 and max are the machine.** Spinning for 110 ns with no engine involved
still reaches 18 µs at p99.99, the same range as every operation type. A
standalone check agrees: the count of multi-microsecond spikes grows in
proportion to the length of the timed window, which is the signature of
interrupts rather than of anything the code does.

**What a result has to beat.** Overall p99 is stable to 2%, but per-type p99.9
moves by up to 27% between runs. A change to the engine is real only when it
exceeds the spread of the cell it claims to improve. For example, a 3% gain in
cancel p99.9 is noise here.

WSL2 has no cpufreq interface -- the Windows host owns the clock -- so the
governor cannot be fixed from inside it. For tails worth quoting, use bare-metal
Linux:

- `sudo cpupower frequency-set -g performance`, with boost off;
- an isolated core (`isolcpus`, `nohz_full`), passed as `--cpu`.

What the engine rows already say:

- Modify costs about twice an add, because it is a cancel plus an add.
- Marketable orders have a p99 three times an add's, because a sweep erases
  several orders and sometimes whole price levels.
- Every add allocates a list node and a hash-map node, plus a map node when it
  opens a new level. Every cancel frees them. Those allocations are the first
  suspects for the p99 and p99.9, and the obvious target for the next round of
  work. When that work brings object pools, they must be touched at
  construction, for the same reason the heap is pre-faulted now.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/tests
```
