//-----------------------------------------------------------------------------
// Per-operation latency benchmark.
//
// Throughput hides what matters here. A mean of 80 ns with a p99.9 of 40 us is
// a worse engine than a mean of 120 ns with a p99.9 of 300 ns, and an average
// cannot tell them apart. So every operation is timed on its own, every sample
// is kept, and the report gives the distribution -- p50, p99, p99.9, p99.99 and
// max -- overall and per operation type. No mean is reported.
//
//   ./bench [--ops N] [--warmup N] [--runs R] [--seed S] [--depth D]
//           [--cpu C | --no-pin] [--prefault MiB] [--dump FILE]
//
// Method, and how each part is checked rather than assumed:
//
//   Optimizer   Every result passes through do_not_optimize(), an empty
//               inline-asm barrier (the trick behind benchmark::DoNotOptimize),
//               inside the timed bracket. The call cannot be deleted or sunk
//               past the closing clock read, even under LTO.
//   Warm-up     Each run replays --warmup mixed operations (default 300,000)
//               through the same timing loop before recording, so caches,
//               branch predictors and the allocator start warm.
//   Pinning     The thread is pinned to one CPU with pthread_setaffinity_np:
//               --cpu, or else the last CPU it may use, since CPU 0 tends to
//               take the most interrupts. It is re-checked after every run.
//   Frequency   The cpufreq governor and boost setting are read and reported,
//               with a warning unless they are fixed. Clock calibration spins
//               for 200 ms just before the first run, so the core is at full
//               clock when recording starts.
//   Memory      The flow and sample buffers are written before timing. On
//               glibc the heap is grown by --prefault MiB, every page touched,
//               and trimming disabled, so engine allocations land on resident
//               pages. Page faults and context switches are counted over each
//               timed pass and reported.
//   Clock       rdtscp, not std::chrono. Each sample brackets one engine call
//               between fenced TSC reads and is stored in ticks; conversion to
//               nanoseconds happens once, in the report. The median cost of an
//               empty bracket is subtracted, and the TSC resolution is printed.
//   Variance    The whole run repeats --runs times (default 5) on a fresh book.
//               The report gives the median of each percentile across runs
//               and its spread; a difference smaller than the spread is noise.
//   Noise floor A "(spin)" row busy-waits as long as the median operation, as
//               many times, with no engine involved. Interrupts and hypervisor
//               exits land in a window in proportion to its length, so that row
//               is what the machine alone adds at each percentile.
//   Output      Every run's trade stream is hashed and checked against the
//               generator's before anything is reported.
//
// Refuses to run in a debug or sanitized build: check_invariants() walks the
// whole book after every operation there, so the numbers would measure that.
//-----------------------------------------------------------------------------
#include "flow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace {

//--- compiler barrier --------------------------------------------------------

//Forces `value` to be materialized here, so the optimizer can neither delete
//the code that produced it nor move that code past this point. Emits no
//instructions of its own.
template <typename T>
inline void do_not_optimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

//--- clock -------------------------------------------------------------------

#if defined(__x86_64__) || defined(__i386__)
//lfence keeps earlier instructions from drifting into the bracket and the
//timed call from starting before the first read; rdtscp waits for the call to
//retire before reading. The asm barriers stop the compiler moving work across.
inline std::uint64_t ticks_begin() {
    asm volatile("" ::: "memory");
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    asm volatile("" ::: "memory");
    return t;
}

inline std::uint64_t ticks_end() {
    asm volatile("" ::: "memory");
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    asm volatile("" ::: "memory");
    return t;
}
#else
inline std::uint64_t ticks_begin() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::uint64_t ticks_end() {return ticks_begin();}
#endif

//Ticks per nanosecond, measured against steady_clock over ~200 ms. The spin
//doubles as a ramp-up, bringing the core to full clock before timing.
double calibrate() {
    using clock = std::chrono::steady_clock;
    const auto c0 = clock::now();
    const std::uint64_t t0 = ticks_begin();
    while (clock::now() - c0 < std::chrono::milliseconds(200)) {}
    const std::uint64_t t1 = ticks_end();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - c0).count();
    return static_cast<double>(t1 - t0) / static_cast<double>(ns);
}

//Smallest step the clock can show, in ticks. Some CPUs advance the TSC in
//coarse increments (32 ticks, ~10 ns, on AMD Zen 3), and every sample is
//quantized to it, so it belongs next to the numbers.
std::uint64_t clock_resolution() {
    std::uint64_t g = 0;
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t a = ticks_begin();
        const std::uint64_t b = ticks_begin();
        g = std::gcd(g, b - a);
    }
    return g == 0 ? 1 : g;
}

std::uint64_t median(std::vector<std::uint64_t> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

//Median cost of an empty bracket, in ticks.
std::uint64_t timer_overhead() {
    std::vector<std::uint64_t> samples(200'000);
    for (auto& s : samples) {
        const std::uint64_t t0 = ticks_begin();
        const std::uint64_t t1 = ticks_end();
        s = t1 - t0;
    }
    return median(std::move(samples));
}

//--- machine setup -----------------------------------------------------------

//The last CPU this thread may run on, or -1 if affinity is unsupported.
int default_cpu() {
#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) != 0) return -1;
    for (int c = CPU_SETSIZE - 1; c >= 0; --c)
        if (CPU_ISSET(c, &set)) return c;
#endif
    return -1;
}

bool pin_to(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

int current_cpu() {
#if defined(__linux__)
    return sched_getcpu();
#else
    return -1;
#endif
}

std::string read_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

//How frequency scaling is set up, plus a warning for anything not fixed.
std::vector<std::string> frequency_report(int cpu) {
    std::vector<std::string> lines;
    const std::string gov = read_line("/sys/devices/system/cpu/cpu" +
                                      std::to_string(cpu < 0 ? 0 : cpu) +
                                      "/cpufreq/scaling_governor");
    if (gov.empty()) {
        lines.push_back("no cpufreq interface (VM or container) -- set the governor on the host");
        return lines;
    }
    std::string desc = "governor " + gov;
    bool boost_on = false;
    if (const std::string b = read_line("/sys/devices/system/cpu/cpufreq/boost"); !b.empty()) {
        boost_on = b == "1";
        desc += boost_on ? ", boost on" : ", boost off";
    } else if (const std::string nt = read_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
               !nt.empty()) {
        boost_on = nt == "0";
        desc += boost_on ? ", turbo on" : ", turbo off";
    }
    lines.push_back(desc);
    if (gov != "performance")
        lines.push_back("WARNING: clocks will vary -- sudo cpupower frequency-set -g performance");
    if (boost_on)
        lines.push_back("WARNING: boost clocks depend on temperature -- disable boost for A/B runs");
    return lines;
}

//Grows the heap by about `mib` MiB, touches every page, and returns it to
//malloc with trimming disabled, so it stays resident. The engine's node
//allocations during timing then land on pages that are already mapped instead
//of faulting them in one at a time. Returns the MiB actually pre-faulted.
std::size_t prefault_heap(std::size_t mib) {
#if defined(__GLIBC__)
    constexpr std::size_t kChunk = std::size_t{16} << 20;
    mallopt(M_TRIM_THRESHOLD, INT_MAX);         //Never give pages back mid-run
    mallopt(M_MMAP_THRESHOLD, 32 << 20);        //Keep 16 MiB chunks on the heap
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));

    std::vector<char*> blocks;
    for (std::size_t done = 0; done < (mib << 20); done += kChunk) {
        auto* p = static_cast<char*>(std::malloc(kChunk));
        if (!p) break;
        for (std::size_t off = 0; off < kChunk; off += page)
            static_cast<volatile char*>(p)[off] = 1;
        blocks.push_back(p);
    }
    //Newest first, so each block merges into the top of the heap.
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) std::free(*it);
    return blocks.size() * (kChunk >> 20);
#else
    (void)mib;
    return 0;
#endif
}

struct Usage {
    long minor_faults = 0;
    long major_faults = 0;
    long switches = 0;          //Voluntary plus involuntary context switches
};

Usage usage_now() {
    Usage u;
#if defined(__linux__)
    rusage r{};
    getrusage(RUSAGE_THREAD, &r);
    u.minor_faults = r.ru_minflt;
    u.major_faults = r.ru_majflt;
    u.switches = r.ru_nvcsw + r.ru_nivcsw;
#endif
    return u;
}

Usage operator-(const Usage& a, const Usage& b) {
    return {a.minor_faults - b.minor_faults, a.major_faults - b.major_faults,
            a.switches - b.switches};
}

//--- measurement -------------------------------------------------------------

constexpr std::size_t kPoints = 5;
constexpr std::array<std::uint64_t, kPoints> kPointPpm = {500'000, 990'000, 999'000, 999'900, 1'000'000};

//Rows of the report: every op, each kind, then the noise floor.
constexpr std::size_t kRows = flow::kKinds + 2;
constexpr std::size_t kAllRow = 0;
constexpr std::size_t kSpinRow = kRows - 1;

std::string row_name(std::size_t row) {
    if (row == kAllRow) return "all";
    if (row == kSpinRow) return "(spin)";
    return flow::kind_name(static_cast<flow::Kind>(row - 1));
}

using Points = std::array<std::uint64_t, kPoints>;      //Ticks at p50 .. max

//Nearest-rank percentiles of a sample, which this sorts.
Points percentiles(std::vector<std::uint64_t>& v) {
    Points p{};
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    for (std::size_t j = 0; j < kPoints; ++j) {
        const std::uint64_t rank = (v.size() * kPointPpm[j] + 999'999) / 1'000'000;
        p[j] = v[rank == 0 ? 0 : rank - 1];
    }
    return p;
}

struct RunResult {
    std::array<Points, kRows> rows{};
    Usage usage;                //Over the timed operations only
    bool stayed_on_cpu = true;
    std::uint64_t trade_hash = 0;
    std::uint64_t dropped = 0;
};

class Bench {
public:
    Bench(const flow::Flow& fl, const flow::Config& c)
        : f(fl), cfg(c),
          samples(std::max({f.build, f.warmup, f.timed_count()})),
          spin(f.timed_count()) {
        for (std::size_t k = 0; k < flow::kKinds; ++k) by_kind[k].reserve(f.stats.count[k]);
    }

    void set_overhead(std::uint64_t ticks) {overhead = ticks;}

    //One complete pass on a fresh book: build depth, warm up, time, then
    //measure the noise floor over the same number of samples.
    RunResult run(int cpu, std::FILE* dump, std::size_t index, double ticks_per_ns) {
        RunResult res;
        lob::OrderBook book(lob::Config{cfg.self_trade, flow::kTradeCapacity});
        std::uint64_t hash = flow::kHashSeed;

        time_ops(book, 0, f.build, hash);
        time_ops(book, f.build, f.timed_begin(), hash);     //Recorded, then overwritten

        const Usage before = usage_now();
        time_ops(book, f.timed_begin(), f.ops.size(), hash);
        res.usage = usage_now() - before;

        const std::size_t n = f.timed_count();
        const auto timed_end = samples.begin() + static_cast<std::ptrdiff_t>(n);
        const std::uint64_t window = median({samples.begin(), timed_end});
        for (auto& s : spin) {
            const std::uint64_t t0 = ticks_begin();
            while (ticks_begin() - t0 < window) {}
            const std::uint64_t dt = ticks_end() - t0;
            s = dt > overhead ? dt - overhead : 0;
        }

        res.stayed_on_cpu = cpu < 0 || current_cpu() == cpu;
        res.trade_hash = hash;
        res.dropped = book.trades().dropped();

        if (dump) {
            for (std::size_t i = 0; i < n; ++i)
                std::fprintf(dump, "%zu,%zu,%s,%.1f\n", index + 1, i,
                             flow::kind_name(f.ops[f.timed_begin() + i].kind),
                             static_cast<double>(samples[i]) / ticks_per_ns);
        }

        for (auto& v : by_kind) v.clear();
        for (std::size_t i = 0; i < n; ++i)
            by_kind[static_cast<std::size_t>(f.ops[f.timed_begin() + i].kind)].push_back(samples[i]);
        for (std::size_t k = 0; k < flow::kKinds; ++k) res.rows[k + 1] = percentiles(by_kind[k]);

        std::vector<std::uint64_t> all(samples.begin(), timed_end);
        res.rows[kAllRow] = percentiles(all);
        res.rows[kSpinRow] = percentiles(spin);
        return res;
    }

private:
    const flow::Flow& f;
    flow::Config cfg;
    std::uint64_t overhead = 0;
    std::vector<std::uint64_t> samples;     //Ticks, one per op of the current pass
    std::vector<std::uint64_t> spin;
    std::array<std::vector<std::uint64_t>, flow::kKinds> by_kind;

    //Times ops [begin, end) one engine call at a time into samples[0, ...).
    //Never inlined: the warm-up and the timed pass then run the same machine
    //code at the same addresses, so the branch predictor state the warm-up
    //builds is the state the timed pass uses.
    [[gnu::noinline]] void time_ops(lob::OrderBook& book, std::size_t begin,
                                    std::size_t end, std::uint64_t& trade_hash) {
        for (std::size_t i = begin; i < end; ++i) {
            const flow::Op& op = f.ops[i];
            std::uint64_t t0 = 0, t1 = 0;
            switch (op.kind) {
                case flow::Kind::Add: {
                    t0 = ticks_begin();
                    const lob::ExecReport rep = book.add_limit(op.id, op.owner, op.side, op.price, op.qty);
                    do_not_optimize(rep);
                    t1 = ticks_end();
                    break;
                }
                case flow::Kind::Cancel: {
                    t0 = ticks_begin();
                    const bool ok = book.cancel(op.id);
                    do_not_optimize(ok);
                    t1 = ticks_end();
                    break;
                }
                case flow::Kind::Modify: {
                    t0 = ticks_begin();
                    const lob::ExecReport rep = book.modify(op.id, op.price, op.qty);
                    do_not_optimize(rep);
                    t1 = ticks_end();
                    break;
                }
                case flow::Kind::Marketable: {
                    if (op.market) {
                        t0 = ticks_begin();
                        const lob::ExecReport rep = book.add_market(op.id, op.owner, op.side, op.qty);
                        do_not_optimize(rep);
                        t1 = ticks_end();
                    } else {
                        t0 = ticks_begin();
                        const lob::ExecReport rep = book.add_limit(op.id, op.owner, op.side, op.price, op.qty);
                        do_not_optimize(rep);
                        t1 = ticks_end();
                    }
                    break;
                }
            }
            const std::uint64_t dt = t1 - t0;
            samples[i - begin] = dt > overhead ? dt - overhead : 0;

            lob::Trade t;
            while (book.trades().pop(t)) trade_hash = flow::hash_trade(trade_hash, t);
        }
    }
};

//--- reporting ---------------------------------------------------------------

std::string grouped(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

std::string ns(std::uint64_t ticks, double ticks_per_ns) {
    return grouped(static_cast<std::uint64_t>(static_cast<double>(ticks) / ticks_per_ns + 0.5));
}

double percent(std::size_t part, std::size_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) return line.substr(colon + 2);
        }
    }
    return "unknown cpu";
}

//Median and relative spread, (max - min) / median, of one cell across runs.
struct Cell {
    std::uint64_t median;
    double spread_percent;
};

Cell across_runs(const std::vector<RunResult>& runs, std::size_t row, std::size_t point) {
    std::vector<std::uint64_t> v;
    for (const RunResult& r : runs) v.push_back(r.rows[row][point]);
    std::sort(v.begin(), v.end());
    const std::uint64_t med = v[(v.size() - 1) / 2];
    const double spread = med == 0 ? 0.0 : 100.0 * static_cast<double>(v.back() - v.front()) /
                                               static_cast<double>(med);
    return {med, spread};
}

void print_summary(const std::vector<RunResult>& runs, const flow::Flow& f, double ticks_per_ns) {
    const auto count = [&](std::size_t row) -> std::size_t {
        if (row == kAllRow || row == kSpinRow) return f.timed_count();
        return f.stats.count[row - 1];
    };

    std::printf("\nlatency (ns), median of %zu run%s\n", runs.size(), runs.size() == 1 ? "" : "s");
    std::printf("                    count      p50      p99    p99.9   p99.99        max\n");
    for (std::size_t row = 0; row < kRows; ++row) {
        std::printf("  %-11s %11s", row_name(row).c_str(), grouped(count(row)).c_str());
        for (std::size_t j = 0; j < kPoints; ++j)
            std::printf(j + 1 == kPoints ? " %10s" : " %8s",
                        ns(across_runs(runs, row, j).median, ticks_per_ns).c_str());
        std::printf("\n");
    }

    if (runs.size() < 2) return;
    std::printf("\nspread across runs, (max - min) / median\n");
    std::printf("                                p50      p99    p99.9   p99.99        max\n");
    for (std::size_t row = 0; row < kRows; ++row) {
        std::printf("  %-11s %11s", row_name(row).c_str(), "");
        for (std::size_t j = 0; j < kPoints; ++j)
            std::printf(j + 1 == kPoints ? " %9.0f%%" : " %7.0f%%",
                        across_runs(runs, row, j).spread_percent);
        std::printf("\n");
    }
    std::printf("  A difference smaller than its cell's spread is noise, not a result.\n");
}

[[noreturn]] void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--ops N] [--warmup N] [--runs R] [--seed S] [--depth D]\n"
        "          [--cpu C | --no-pin] [--prefault MiB] [--dump FILE]\n"
        "  --ops       timed operations per run (default 2000000)\n"
        "  --warmup    mixed operations run before recording (default 300000)\n"
        "  --runs      repetitions, each on a fresh book (default 5)\n"
        "  --seed      flow seed (default 1)\n"
        "  --depth     target resting orders (default 5000)\n"
        "  --cpu       pin to this CPU (default: the last one allowed)\n"
        "  --no-pin    do not pin\n"
        "  --prefault  MiB of heap to pre-fault on glibc (default 64)\n"
        "  --dump      write every timed sample as CSV: run,index,kind,ns\n", argv0);
    std::exit(2);
}

}

#if !defined(NDEBUG) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kMeasurable = false;
#else
constexpr bool kMeasurable = true;
#endif

int main(int argc, char** argv) {
    if (!kMeasurable) {
        std::fprintf(stderr,
            "bench: refusing to run in a debug or sanitized build -- invariant checks\n"
            "and instrumentation would dominate every sample. Build a release tree:\n"
            "  cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release\n"
            "  cmake --build build-rel -j && ./build-rel/bench\n");
        return 2;
    }

    flow::Config cfg;
    std::size_t runs = 5;
    std::size_t prefault_mib = 64;
    int cpu = default_cpu();
    const char* dump_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if      (!std::strcmp(argv[i], "--ops"))      cfg.ops = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--warmup"))   cfg.warmup_ops = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--runs"))     runs = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed"))     cfg.seed = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--depth"))    cfg.target_depth = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--cpu"))      cpu = std::atoi(value());
        else if (!std::strcmp(argv[i], "--no-pin"))   cpu = -1;
        else if (!std::strcmp(argv[i], "--prefault")) prefault_mib = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--dump"))     dump_path = value();
        else usage(argv[0]);
    }
    if (cfg.ops == 0 || runs == 0) usage(argv[0]);

    if (cpu >= 0 && !pin_to(cpu)) {
        std::fprintf(stderr, "bench: could not pin to cpu %d\n", cpu);
        return 1;
    }

    //Order matters: the flow and the sample buffers are allocated and written
    //first, so the pre-faulted heap is left free for the engine alone.
    const auto g0 = std::chrono::steady_clock::now();
    const flow::Flow f = flow::generate(cfg);
    const double gen_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g0).count();
    const flow::Stats& st = f.stats;
    Bench bench(f, cfg);
    const std::size_t prefaulted = prefault_heap(prefault_mib);

    std::FILE* dump = nullptr;
    if (dump_path) {
        dump = std::fopen(dump_path, "w");
        if (!dump) {std::perror(dump_path); return 1;}
        std::fprintf(dump, "run,index,kind,ns\n");
    }

    //Last before timing: calibration's 200 ms spin brings the core to speed.
    const double ticks_per_ns = calibrate();
    const std::uint64_t resolution = clock_resolution();
    const std::uint64_t overhead = timer_overhead();
    bench.set_overhead(overhead);

    std::printf("lob latency benchmark\n");
    std::printf("  cpu         %s, %s\n", cpu_model().c_str(),
                cpu >= 0 ? ("pinned to cpu " + std::to_string(cpu)).c_str() : "NOT pinned");
    for (const std::string& line : frequency_report(cpu >= 0 ? cpu : current_cpu()))
        std::printf("  frequency   %s\n", line.c_str());
    std::printf("  clock       rdtscp at %.3f GHz, resolution %.1f ns; empty bracket %llu ticks "
                "(%.1f ns), subtracted\n", ticks_per_ns,
                static_cast<double>(resolution) / ticks_per_ns, (unsigned long long)overhead,
                static_cast<double>(overhead) / ticks_per_ns);
    if (prefaulted)
        std::printf("  memory      %zu MiB of heap pre-faulted, trimming off\n", prefaulted);
    else
        std::printf("  memory      heap pre-faulting skipped (--prefault 0, or not glibc)\n");

    std::printf("\nflow  seed %llu, hash %016llx, generated in %.1f s\n",
                (unsigned long long)cfg.seed, (unsigned long long)st.flow_hash, gen_s);
    std::printf("  per run     %s depth-building adds, %s warm-up ops, then %s timed ops\n",
                grouped(f.build).c_str(), grouped(f.warmup).c_str(), grouped(f.timed_count()).c_str());
    std::printf("  mix         add %.1f%%  cancel %.1f%%  modify %.1f%%  marketable %.1f%%\n",
                percent(st.count[0], f.timed_count()), percent(st.count[1], f.timed_count()),
                percent(st.count[2], f.timed_count()), percent(st.count[3], f.timed_count()));
    std::printf("  book        %s-%s resting orders (target %s), spread p50 %llu tick%s, p99 %llu\n",
                grouped(st.depth_min).c_str(), grouped(st.depth_max).c_str(),
                grouped(cfg.target_depth).c_str(), (unsigned long long)st.spread_p50,
                st.spread_p50 == 1 ? "" : "s", (unsigned long long)st.spread_p99);
    std::printf("  trades      %s, %.1f per marketable op; %.0f%% sent as market orders\n",
                grouped(st.trades).c_str(),
                st.count[3] ? static_cast<double>(st.trades) / static_cast<double>(st.count[3]) : 0.0,
                percent(st.market_orders, st.count[3]));

    std::printf("\nruns (ns over all timed ops; page faults and context switches while timing)\n");
    std::vector<RunResult> results;
    for (std::size_t r = 0; r < runs; ++r) {
        const RunResult res = bench.run(cpu, dump, r, ticks_per_ns);
        if (res.trade_hash != st.trade_hash || res.dropped != 0) {
            std::fprintf(stderr, "bench: run %zu trade stream does not match the generator's "
                                 "(hash %016llx vs %016llx, %llu dropped) -- numbers discarded\n",
                         r + 1, (unsigned long long)res.trade_hash,
                         (unsigned long long)st.trade_hash, (unsigned long long)res.dropped);
            return 1;
        }
        const Points& all = res.rows[kAllRow];
        std::printf("  run %zu  p50 %s  p99 %s  p99.9 %s  p99.99 %s  max %s   "
                    "faults %ld  switches %ld%s\n", r + 1,
                    ns(all[0], ticks_per_ns).c_str(), ns(all[1], ticks_per_ns).c_str(),
                    ns(all[2], ticks_per_ns).c_str(), ns(all[3], ticks_per_ns).c_str(),
                    ns(all[4], ticks_per_ns).c_str(),
                    res.usage.minor_faults + res.usage.major_faults, res.usage.switches,
                    res.stayed_on_cpu ? "" : "  WARNING: left its cpu");
        std::fflush(stdout);
        results.push_back(res);
    }
    std::printf("  trade stream %016llx in every run, matching the generator\n",
                (unsigned long long)st.trade_hash);

    print_summary(results, f, ticks_per_ns);

    if (dump) {
        std::fclose(dump);
        std::printf("\nsamples written to %s\n", dump_path);
    }
    return 0;
}
