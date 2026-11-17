// Build with matching SDK headers/library; see Performance-Handles.md.
#include <regfw/regfw.hpp>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <time.h>
#if defined(__GLIBC__)
#include <sys/single_threaded.h>
#endif

union NativeValue {
    std::uint32_t u32;
    struct {
        std::uint32_t count : 24;
        std::uint32_t flags : 8;
    } bits;
};
struct Registers : reg::RegEntityBundle {
    REG_BUNDLE(Registers);
    REG(ITEMS, NativeValue, reg::at(0).array(reg::dims(256), reg::strides(4)));
};

static double cpu_time() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        std::abort();
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}
template <class F>
__attribute__((noinline)) void measure(std::string_view name, std::uint64_t iterations, F access) {
    std::uint64_t checksum = 0;
    const auto begin = cpu_time();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        // Force every access to observe memory, equally for raw and view paths.
        asm volatile("" ::: "memory");
        checksum += access(std::size_t((i * 13) & 255));
    }
    const auto elapsed = cpu_time() - begin;
    std::cout << name << ',' << std::fixed << std::setprecision(4)
              << elapsed * 1e9 / double(iterations) << ',' << checksum << '\n';
}
int main(int argc, char** argv) {
    const auto iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000'000;
    reg::RegisterBank<Registers> bank(reg::space(0x1000).name("HANDLES").finalize());
    auto& items = bank->ITEMS;
    auto* raw = items.data();
    for (std::size_t i = 0; i != 256; ++i)
        raw[i].u32 = std::uint32_t(i + 1);
#if defined(__GLIBC__)
    std::cerr << "libc_single_threaded=" << int(__libc_single_threaded) << '\n';
#endif
    measure("raw_read", iterations, [&](auto i) {
        return raw[i].bits.count;
    });
    measure("at_read", iterations, [&](auto i) {
        return items.at(i).bits.count;
    });
    measure("ref_read", iterations, [&](auto i) {
        return items.at(i).ref().bits.count;
    });
    measure("raw_write", iterations, [&](auto i) {
        return ++raw[i].bits.count;
    });
    for (std::size_t i = 0; i != 256; ++i)
        raw[i].u32 = std::uint32_t(i + 1);
    measure("at_write", iterations, [&](auto i) {
        return ++items.at(i).bits.count;
    });
    for (std::size_t i = 0; i != 256; ++i)
        raw[i].u32 = std::uint32_t(i + 1);
    measure("ref_write", iterations, [&](auto i) {
        return ++items.at(i).ref().bits.count;
    });
}
