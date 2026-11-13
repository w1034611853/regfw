// Optional Linux/x86-64 diagnostic sampler. Link with -no-pie, set
// REGFW_SAMPLE_IPS=/path/to/samples.txt. Do not use sampled runs for timings.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "This diagnostic sampler requires Linux x86-64"
#endif

namespace {
struct Bucket {
    std::atomic<std::uintptr_t> ip{};
    std::atomic<unsigned> count{};
};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
constexpr unsigned capacity = 1 << 18;
Bucket buckets[capacity];
void sample(int, siginfo_t*, void* ptr) {
    const auto ip =
        static_cast<std::uintptr_t>(static_cast<ucontext_t*>(ptr)->uc_mcontext.gregs[REG_RIP]);
    auto slot = (ip >> 2) & (capacity - 1);
    for (unsigned tried = 0; tried < capacity; ++tried) {
        auto expected = std::uintptr_t{0};
        if (buckets[slot].ip.compare_exchange_strong(expected, ip, std::memory_order_relaxed) ||
            expected == ip) {
            buckets[slot].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        slot = (slot + 1) & (capacity - 1);
    }
}
__attribute__((constructor)) void start() {
    if (!std::getenv("REGFW_SAMPLE_IPS"))
        return;
    struct sigaction sa {};
    sa.sa_sigaction = sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    itimerval timer{{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &timer, nullptr);
}
__attribute__((destructor)) void finish() {
    const char* path = std::getenv("REGFW_SAMPLE_IPS");
    if (!path)
        return;
    itimerval timer{};
    setitimer(ITIMER_PROF, &timer, nullptr);
    auto* file = std::fopen(path, "w");
    if (!file)
        return;
    for (const auto& b : buckets)
        if (const auto count = b.count.load(std::memory_order_relaxed))
            std::fprintf(file, "%lx %u\n", b.ip.load(std::memory_order_relaxed), count);
    std::fclose(file);
}
} // namespace
