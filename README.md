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

The full suite and the fuzzer are run under AddressSanitizer and
UndefinedBehaviorSanitizer, and both are clean. This matters here because the
engine hands out `std::list` iterators as locators and erases list nodes during
matching -- exactly the shape of code that produces use-after-free, and the same
shape the intrusive lists and object pools of a later phase will have.

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-asan -j
./build-asan/tests
./build-asan/difftest 20000
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/tests
```
