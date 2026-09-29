# Optimization log

The per-change record behind the README's [optimization
journey](../README.md#optimization-journey). The README's table re-measures
every pass together, in one session, with `tools/journey.py`. This log is the
other half: each change measured when it was made, against the commit before
it, with `tools/ab_compare.py`. At least 11 interleaved pairs, the default flow,
every operation type. Only cells the sign test marks count.

Each row is its own paired measurement, so a row's "before" need not equal the
previous row's "after". The machine drifts between sessions, which is why a
change is judged only against its own baseline, measured alongside it. For the
same reason these numbers need not match the journey table's to the clock step.

In the comparison tables each cell reads *before -> after, change, pairs won*.
`*` marks p < 0.05 by a two-sided sign test, in either direction, so a
consistent slowdown is marked as well.

| Change | p50 | p99 | p99.9 | Where it landed |
|---|---|---|---|---|
| Node arena instead of malloc | 100 → 80 | 451 → 321 (−29%) | 1,132 → 812 (−28%) | Every type. Marketable p99 −33%, add −34% |
| Flat price array instead of `std::map` | 80 → 70 | 301 → 271 (−10%) | 711 → 651, not significant | p99 and p99.9: cancel −33% and −34%, modify −31% and −30%, add −22% and −18%. Marketable unchanged |
| Order slot pool with 32-bit links instead of `std::list` | 70 → 60 | 261 → 250, not significant | 681 → 641, not significant | add p50 −12% and p99.9 −10%; nothing else significant |
| Flat id index instead of `std::unordered_map` (15 pairs) | 70 → 50 | 311 → 240 (−23%) | 832 → 581 (−30%) | add and marketable about −30% at p99 and p99.9; cancel within noise. On a 200,000-order book: p99 992 → 561 (−43%), p99.9 2,735 → 1,182 (−57%) |
| 32-byte hot order slot instead of 40 (15 pairs) | 50 → 50 | 210 → 210, not significant | 611 → 561, not significant | cancel p50 50 → 40; nothing else significant, and no change on a 200,000-order book |
| 16-byte price levels instead of 24 (15 pairs) | 50 → 50 | 190 → 190, not significant | 401 → 401, not significant | nothing significant at either depth |
| *Rejected:* order path specialized on side (15 pairs) | 50 → 50 | 190 → 190, not significant | 421 → 431, not significant | marketable p50 90 → 100 and p99 401 → 421, both significantly **worse**. Reverted |
| Profile-guided branch hints (15 pairs) | 50 → 50 | 190 → 190, not significant | 401 → 411, not significant | add p99 −8% and p99.9 −7%; on a 200,000-order book, add p99 −6% and modify p50 −11% |
| *Rejected:* branchless queue unlink (15 pairs) | 50 → 50 | 220 → 220, not significant | 581 → 571, not significant | modify p50 90 → 80; nothing at 200,000 orders, where p99 leaned slightly worse. Not adopted |
| Huge pages for blocks of 2 MiB or more (15 pairs) | 50 → 50 | 210 → 210, not significant | 551 → 541, not significant | Nothing on the default book, where no array qualifies. On a 200,000-order book: p99 571 → 511 (−11%), p99.9 1,272 → 1,092 (−14%), cancel p99.9 −31%; modify p50 one step slower |

## Node arena instead of malloc

The same binary, the same flow and the same seed, with container nodes on
malloc (`--system-alloc`) and then on the node arena. Median of 5 runs each
(ns). This was measured before `tools/ab_compare.py` existed, so only changes
larger than both allocators' run-to-run spread are counted as results:

| | p50 | p99 | p99.9 |
|---|---|---|---|
| all | 100 → 80 | 421 → 301 (−29%) | 972 → 661 (−32%) |
| add | 100 → 90 | 331 → 230 (−31%) | 761 → 481 (−37%) |
| cancel | 90 → 70 | 261 → 220 (−16%) | 501 → 401, within noise (35% spread) |
| modify | 190 → 160 | 431 → 371 (−14%) | 902 → 681, within noise (53% spread) |
| marketable | 160 → 130 | 1,002 → 661 (−34%) | 2,034 → 1,212, within noise (42% spread) |

The standard answer, `std::pmr::unsynchronized_pool_resource`, was tried first
and made every percentile *worse*, p50 included (100 to 140 ns): glibc's
per-thread cache is already fast, and a general-purpose pool's bookkeeping
loses to it. What wins is a dedicated free list that does nothing else.

## Flat price array instead of `std::map`

11 pairs:

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

## Order slot pool instead of `std::list`

11 pairs:

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

## Flat id index instead of `std::unordered_map`

The flat id index is the biggest step so far, and the first to move the deep
book's tail. The default book, 15 pairs:

```
                                    p50                         p99                       p99.9
all              70 ->    50  -29% 13/15*     311 ->   240  -23% 14/15*     832 ->   581  -30% 14/15*
add              70 ->    50  -29% 14/15*     200 ->   140  -30% 14/15*     591 ->   401  -32% 15/15*
cancel           40 ->    50  +25%  2/15      190 ->   180   -5%  8/15      601 ->   551   -8% 13/15*
modify          110 ->    90  -18% 13/15*     321 ->   301   -6% 10/15      942 ->   852  -10% 11/15 
marketable      130 ->   100  -23% 14/15*     802 ->   561  -30% 14/15*   1,914 -> 1,353  -29% 14/15*
```

And 200,000 resting orders, 9 pairs:

```
                                    p50                         p99                       p99.9
all              70 ->    50  -29%  9/9*     992 ->   561  -43%  9/9*   2,735 -> 1,182  -57%  9/9*
add              70 ->    50  -29%  9/9*     381 ->   190  -50%  9/9*     731 ->   451  -38%  9/9*
cancel           50 ->    40  -20%  6/9      832 ->   531  -36%  9/9*   1,733 -> 1,112  -36%  9/9*
modify          110 ->    90  -18%  9/9*   1,062 ->   711  -33%  9/9*   2,314 -> 1,583  -32%  8/9*
marketable      301 ->   180  -40%  9/9*   3,016 -> 1,172  -61%  9/9*   6,713 -> 2,304  -66%  9/9*
```

Both chained-table hops are gone, and so is the node allocated per add and
freed per cancel or fill. Marketable orders gain the most, because every fill
used to free a hash node. Cancel gains least, and its p50 is a clock step worse
on the default book: an erase scans the run of newer entries after it, which
the old chained table never had to do.

The first version of this change is worth recording too. It hashed with a
multiply that scatters sequential ids across the table. On the default book it
looked excellent: all p99 −29%, cancel p99 −50%. On the deep book it more than
doubled p50 for all operations (70 → 160 ns) and for adds (70 → 180 ns). The
table there is 16 MiB, and a scattered add touches a cold line every time. A
locality-preserving hash fixed the deep book and kept most of the default
book's gain. Without the deep run, the regression would have shipped.

## 32-byte hot order slot

The 32-byte hot slot measured almost nothing, 15 pairs on the default book:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  0/15      210 ->   210   +0%  5/15      611 ->   561   -8%  8/15 
add              50 ->    50   +0%  0/15      140 ->   140   +0%  4/15      421 ->   401   -5%  7/15 
cancel           50 ->    40  -20% 12/15*     150 ->   150   +0%  6/15      571 ->   511  -11%  9/15 
modify           90 ->    90   +0%  1/15      281 ->   281   +0%  5/15      892 ->   831   -7%  9/15 
marketable      100 ->   100   +0%  6/15      491 ->   471   -4%  6/15    1,272 -> 1,192   -6%  9/15 
```

On the 200,000-order book, 9 pairs, no cell moved. There are two reasons:

- **The default book's pool fits in L2 at either slot size.** 10,000 slots are
  400 KiB at 40 bytes and 320 KiB at 32.
- **On the deep book, each operation still reads one slot line either way.**
  The costs that dominate are elsewhere: the index entry, the price level, and,
  for a sweep, the trades.

Building it turned up a compiler trap. `SlotPool` first declared its slot with
three `alignas` specifiers, meaning the strictest wins, and GCC 15 silently kept
only the last: the "32-byte" slots were 8-aligned, and half of them straddled a
line. A test that checks real slot addresses caught it; a size check alone would
not have. The slot now takes a single `alignas` of the explicit maximum, and a
`static_assert` covers alignment as well as size.

The layout is kept anyway: it is smaller, never straddles a line, and building
it exposed that trap, which would have quietly undermined any later layout
work.

## 16-byte price levels

Dense 16-byte price levels measured nothing either, at both depths. The
benchmark's 4,096-level window fits in L2 at either size (96 KiB against 64
KiB), and a sweep or a scan for the next best price touches a few adjacent
levels either way. Dropping the stored order count took nothing from the
matching path, since only `order_count_at()` read it.

## Rejected: specializing the order path on side

Specializing the order path on side was tried and rejected. The guide this
follows recommends it: pick `submit<Side::Buy>` or `submit<Side::Sell>` once at
the API, so no loop tests the side. Measured, it made marketable orders a clock
step slower:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  1/15      190 ->   190   +0%  1/15      421 ->   431   +2%  4/15 
add              50 ->    50   +0%  1/15      120 ->   120   +0%  5/15      281 ->   271   -4%  8/15 
cancel           40 ->    40   +0%  0/15      120 ->   120   +0%  2/15      291 ->   301   +3%  6/15 
modify           90 ->    90   +0%  7/15*     240 ->   230   -4%  6/15      511 ->   521   +2%  7/15 
marketable       90 ->   100  +11%  0/15*     401 ->   421   +5%  1/15*     832 ->   832   +0%  5/15 
```

The branch it removed, the side test inside the crossing check, was already
perfectly predicted: every level an order visits takes it the same way. So
there was nothing to win, and the specialized code inlined and laid out
differently, which cost marketable orders a clock step. The code got smaller,
not larger, so this is layout rather than instruction-cache pressure. The diff
was reverted rather than committed.

## Branch hints from a profile

Hints follow a profile, not intuition. No hardware counters are available
under WSL2, so the profile comes from `gcov`. The engine is compiled with
`--coverage` at `-O0`, so each source branch keeps its own counter, and one
benchmark pass counts which way every branch went:

```sh
mkdir -p build-prof && cd build-prof
c++ -O0 -DNDEBUG -std=gnu++20 --coverage -I../include -c ../src/order_book.cpp -o order_book.o
c++ -O2 -DNDEBUG -std=gnu++20 -I../include -I../bench ../src/node_arena.cpp \
    ../bench/flow.cpp ../bench/bench_main.cpp order_book.o --coverage -o bench-prof -pthread
./bench-prof --runs 1 --ops 1000000 --warmup 100000
gcov -b -c -o . ../src/order_book.cpp     # then read order_book.cpp.gcov
```

A branch got a hint only if the profile put it at least 95% one way:

| Branch | Profiled | Hint |
|---|---|---|
| Level checked by a taker does not cross | 95.5% of 1.28M checks | `[[likely]]` on the early exit |
| Taker and maker share an owner (self-trade) | 2% of 280,000 makers touched | `[[unlikely]]` |
| A sweep empties the level | 5% | `[[unlikely]]` |
| A cancel empties the level | 0.5% | `[[unlikely]]` |
| A cancel finds its order | 100% of 989,000 | `[[unlikely]]` on the miss |
| Duplicate id or zero quantity on submit | 0 of 1.33M | `[[unlikely]]` |
| Appending to a non-empty level | 99% | `[[likely]]` |
| An add opens a new level | 0.9% | `[[unlikely]]` |
| Price inside the flat array; fallback map in use | 100%; never | `[[likely]]`; `[[unlikely]]` |
| Free slot available in the pool; pool or index must grow | ~100%; never | `[[likely]]`; `[[unlikely]]` |

Not hinted: a maker filled completely (80%), the first trade of a taker (40%),
and the two link fixups when a queue entry is unlinked (80/20 and 62/38). The
predictor learns those patterns on its own, and a hint would only move code.

The same change hoisted the loop-invariant half of the self-trade test: whether
the taker is owned under a preventing policy is computed once per order, not
once per maker. The result is small and all on the good side:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  2/15      190 ->   190   +0%  4/15      401 ->   411   +2%  5/15 
add              50 ->    50   +0%  2/15      120 ->   110   -8% 10/15*     281 ->   261   -7% 11/15*
cancel           40 ->    40   +0%  5/15      130 ->   130   +0%  6/15      261 ->   271   +4%  5/15 
modify           90 ->    90   +0%  2/15      240 ->   240   +0%  9/15      431 ->   441   +2%  8/15 
marketable      100 ->    90  -10%  7/15*     411 ->   421   +2%  5/15      711 ->   701   -1%  5/15 
```

On a 200,000-order book, 15 pairs:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  0/15      531 ->   511   -4%  8/15    1,012 -> 1,012   +0%  9/15 
add              50 ->    50   +0%  0/15      170 ->   160   -6% 11/15*     361 ->   351   -3% 11/15*
cancel           40 ->    40   +0%  0/15      511 ->   501   -2%  7/15      771 ->   741   -4%  9/15 
modify           90 ->    80  -11% 11/15*     691 ->   671   -3%  8/15    1,172 -> 1,062   -9%  7/15 
marketable      170 ->   160   -6%  7/15    1,072 -> 1,062   -1%  9/15    1,924 -> 1,904   -1% 12/15*
```

## Rejected: branchless queue unlink

The least predictable branches left are the two link fixups when an order is
unlinked from its queue, 80/20 and 62/38 by count. They were the natural
candidate for branch-free code. A plain ternary did not get there: GCC turned it
back into jumps, and added some. Only selecting the address with a bit mask
compiled to two conditional moves and no jumps. Measured against the hinted
build, it bought one modify p50 step and nothing else:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  0/15      220 ->   220   +0%  8/15      581 ->   571   -2%  9/15 
add              50 ->    50   +0%  0/15      130 ->   130   +0%  3/15      391 ->   381   -3%  9/15 
cancel           40 ->    40   +0%  4/15      160 ->   160   +0%  6/15      521 ->   521   +0%  7/15 
modify           90 ->    80  -11%  9/15*     281 ->   270   -4%  6/15      831 ->   791   -5%  6/15 
marketable      100 ->    90  -10%  3/15      521 ->   501   -4%  5/15    1,262 -> 1,312   +4%  5/15 
```

And on the 200,000-order book:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  0/15      591 ->   611   +3%  5/15    1,362 -> 1,342   -1%  9/15 
add              50 ->    50   +0%  0/15      190 ->   200   +5%  4/15      441 ->   431   -2%  7/15 
cancel           40 ->    40   +0%  0/15      581 ->   591   +2%  5/15    1,533 -> 1,473   -4%  9/15 
modify           80 ->    80   +0%  3/15      751 ->   761   +1%  6/15    1,974 -> 1,783  -10%  9/15 
marketable      180 ->   180   +0%  3/15    1,212 -> 1,242   +2%  5/15    2,444 -> 2,444   +0%  9/15 
```

The counts looked unpredictable, but the patterns are not. A cancel usually
unlinks one of the newest orders, the tail of its queue, and a fill always
takes the head. The predictor learns that, so the branches were nearly free,
and the masked version was not adopted.

## Huge pages for the big arrays

Huge pages were the last change, and the only one aimed at the deep book. On the 200,000-order book, where 30 MiB of the pool and index sit on
2 MiB pages, 15 pairs:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  0/15      571 ->   511  -11% 15/15*   1,272 -> 1,092  -14% 14/15*
add              50 ->    50   +0%  0/15      190 ->   180   -5% 11/15*     441 ->   421   -5% 10/15 
cancel           40 ->    40   +0%  1/15      551 ->   471  -15% 15/15*   1,262 ->   872  -31% 14/15*
modify           80 ->    90  +12%  0/15*     731 ->   651  -11% 15/15*   1,653 -> 1,302  -21% 14/15*
marketable      170 ->   170   +0%  8/15    1,162 -> 1,112   -4% 12/15*   2,264 -> 2,164   -4% 12/15*
```

Every p99 and most p99.9 cells improved, with cancel's p99.9 down a third. The
one cost is modify's p50, a clock step slower. On the default book no array
reaches 2 MiB, so nothing changes, and nothing measured did:

```
                                    p50                         p99                       p99.9
all              50 ->    50   +0%  0/15      210 ->   210   +0% 10/15      551 ->   541   -2%  8/15 
add              50 ->    50   +0%  0/15      130 ->   130   +0%  5/15      371 ->   381   +3%  5/15 
cancel           40 ->    50  +25%  2/15      150 ->   140   -7%  9/15      491 ->   491   +0%  8/15 
modify           90 ->    90   +0%  0/15      260 ->   260   +0%  8/15      771 ->   752   -2%  8/15 
marketable      100 ->   100   +0%  3/15      481 ->   471   -2%  9/15    1,222 -> 1,162   -5%  8/15 
```

Blocks under 2 MiB take `std::allocator`'s own path, exactly. A first version
used aligned `new` for them too, and even a book with no huge pages then
measured a clock step slower on cancel p50. Allocation placement alone moved
it.

## What is left

The allocator was not the whole tail, and neither was the price map or the list
nodes. At `--depth 200000`, where the working set outgrows the caches, the flat
array trimmed p99 from 982 to 912 ns, and the order pool left it there. The
flat id index finally moved it, to 561 ns, by removing the chained table's two
dependent hops per lookup. The layout passes after it -- a 32-byte order slot,
16-byte price levels -- made each structure smaller and left the tail where it
was. On the deep book a cancel still makes three dependent accesses: its index
entry, its order slot and its price level, each likely a miss once the book
outgrows the caches. Smaller structures do not make those accesses fewer.
Huge pages made each one cheaper instead, by taking the TLB miss out of it:
p99 571 → 511 ns and p99.9 1,272 → 1,092 ns. Making them fewer is the next
lever, and this benchmark is how to tell whether it works.

A second candidate is the id index's erase. Sequential ids make the newest
orders one dense run of occupied entries, and backward-shift deletion has to
check every entry after the gap. A cancel therefore scans the orders added
after it, a handful on this flow, which is why cancel gained least from the
flat id index. Robin Hood ordering would let the scan stop at the first entry
sitting at its home.
