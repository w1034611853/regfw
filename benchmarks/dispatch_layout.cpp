#include <regfw/regfw.hpp>
#include <time.h>
#include <sys/resource.h>
#include <malloc.h>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::uint32_t view_count = 1, reserve_count = 0;
static bool local_reserve = false, bounded_space = false;
static constexpr std::uint64_t bus_base = 0x100000;
static std::uint64_t cpu_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        std::abort();
    return std::uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
static std::size_t heap_bytes() {
    auto m = mallinfo2();
    return m.uordblks + m.hblkhd;
}
static void require(bool value) {
    if (!value)
        throw std::runtime_error("benchmark check failed");
}
template <unsigned N, std::uint64_t Stride, class T = std::uint32_t>
struct Values : reg::RegEntityBundle {
    REG_BUNDLE(Values);
    static constexpr std::uint64_t second = std::uint64_t(N) * Stride + 64;
    static constexpr std::uint64_t span = second + std::uint64_t(N) * Stride;
    REG(A, T, reg::at(0).array(reg::dims(N), reg::strides(Stride)));
    REG(B, T, reg::at(second).array(reg::dims(N), reg::strides(Stride)));
};
template <unsigned N, std::uint64_t Stride, unsigned Views, class T = std::uint32_t>
struct Device : reg::Bundle {
    REG_BUNDLE(Device);
    using Leaf = Values<N, Stride, T>;
    REG_REPEAT_ENTITY(vf, Leaf, Views);
    static constexpr std::uint64_t step = Leaf::span + 0x1000;
    REG_LAYOUT([&] {
        auto space = reg::address_space("DISPATCH");
        for (unsigned i = 0; i < reserve_count; ++i)
            space = space.map(reg::view("LOW_RESERVE", 4).reserve(0, 4), std::uint64_t(i) * 16);
        for (unsigned i = 0; i < view_count; ++i) {
            auto view = reg::view("REGS", Leaf::span).map(vf[i]);
            // One local reservation in the gap between A and B. It overlaps
            // the EntityView envelope but no register storage.
            if (local_reserve)
                view = view.reserve(std::uint64_t(N) * Stride, 4);
            space = space.map(view, bus_base + std::uint64_t(i) * step);
        }
        return space;
    });
};
template <unsigned N, std::uint64_t Stride, unsigned Views, class T = std::uint32_t>
void run(std::uint64_t iterations, bool writing, bool callback, bool memory) {
    using Root = Device<N, Stride, Views, T>;
    const auto heap0 = heap_bytes();
    const auto t0 = cpu_ns();
    reg::RegisterBank<Root> bank(
        (bounded_space ? reg::space(1ULL << 32) : reg::space()).name("DISPATCH"));
    const auto t1 = cpu_ns();
    std::uint64_t calls = 0, info_sum = 0;
    if constexpr (std::same_as<T, std::uint32_t>) {
        if (callback)
            for (unsigned v = 0; v < view_count; ++v) {
                auto fn = [&](const reg::RegAccessInfo& info) {
                    ++calls;
                    info_sum += info.index.linear() + info.address + info.stored_before;
                };
                if (writing) {
                    bank->vf[v].A.on_write(fn);
                    bank->vf[v].B.on_write(fn);
                } else {
                    bank->vf[v].A.on_read(fn);
                    bank->vf[v].B.on_read(fn);
                }
            }
    }
    const auto t2 = cpu_ns();
    bank.finish();
    const auto t3 = cpu_ns();
    const auto heap1 = heap_bytes();
    std::uint64_t sum = 0;
    auto& map = bank.map();
    if constexpr (std::same_as<T, std::uint32_t>) {
        std::vector<std::uint32_t> expected(std::size_t(view_count) * 2 * N);
        for (unsigned v = 0; v < view_count; ++v)
            for (unsigned a = 0; a < 2; ++a)
                for (unsigned i = 0; i < N; ++i) {
                    const auto value = 1 + v * 2 * N + a * N + i;
                    expected[(v * 2 + a) * N + i] = value;
                    auto* data = a ? bank->vf[v].B.data() : bank->vf[v].A.data();
                    data[i] = value;
                }
        require(map.mmio_read(bus_base) == 1);
        // Validate reserve holes and sparse misses outside the timed loop.
        if (reserve_count)
            require(!map.try_mmio_read_sized(0, 4).success());
        if (local_reserve)
            require(!map.try_mmio_read_sized(bus_base + std::uint64_t(N) * Stride, 4).success());
        if constexpr (Stride > 4)
            require(!map.try_mmio_read_sized(bus_base + 4, 4).success());
        calls = info_sum = 0;
        const auto begin = cpu_ns();
        for (std::uint64_t k = 0; k < iterations; ++k) {
            const auto x = std::uint32_t(k * 0x9e3779b9u);
            const auto i = (x >> 8) & (N - 1);
            const auto v = (x >> 24) % view_count;
            const auto a = (x >> 3) & 1;
            const auto address = bus_base + std::uint64_t(v) * Root::step + a * Root::Leaf::second +
                                 std::uint64_t(i) * Stride;
            if (writing)
                sum += unsigned(map.mmio_write(address, std::uint32_t(k)).result);
            else
                sum += map.mmio_read(address);
        }
        const auto elapsed = cpu_ns() - begin;
        std::uint64_t wanted = 0;
        for (std::uint64_t k = 0; k < iterations; ++k) {
            const auto x = std::uint32_t(k * 0x9e3779b9u);
            const auto i = (x >> 8) & (N - 1), v = (x >> 24) % view_count, a = (x >> 3) & 1;
            if (writing)
                expected[(v * 2 + a) * N + i] = std::uint32_t(k);
            else
                wanted += expected[(v * 2 + a) * N + i];
        }
        require(writing ? sum == 0 : sum == wanted);
        require(calls == (callback ? iterations : 0));
        for (unsigned v = 0; v < view_count; ++v)
            for (unsigned a = 0; a < 2; ++a)
                for (unsigned i = 0; i < N; ++i) {
                    auto* data = a ? bank->vf[v].B.data() : bank->vf[v].A.data();
                    require(data[i] == expected[(v * 2 + a) * N + i]);
                    sum += data[i];
                }
        std::cout << "ACCESS " << std::fixed << std::setprecision(4) << double(elapsed) / iterations
                  << ' ' << sum << ' ' << calls << ' ' << info_sum << '\n';
    } else {
        bank->vf[0].A.at(0).ref() = T{123};
        require(map.mmio_read(bus_base) == 123);
        sum = 123;
    }
    const auto report0 = cpu_ns();
    const auto summary = memory ? map.describe_layout(reg::LayoutDetail::Summary).size() : 0;
    const auto report1 = cpu_ns();
    const auto full = memory ? map.describe_layout(reg::LayoutDetail::Full).size() : 0;
    const auto report2 = cpu_ns();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "INIT " << t1 - t0 << ' ' << t3 - t2 << ' ' << heap1 - heap0 << ' '
              << usage.ru_maxrss << ' ' << summary << ' ' << full << ' ' << report1 - report0 << ' '
              << report2 - report1 << ' ' << map.entity_count() << ' '
              << map.total_node_data_count() << '\n';
}
int main(int argc, char** argv) try {
    if (argc != 9)
        return 2;
    const std::string mode = argv[1];
    view_count = std::stoul(argv[2]);
    reserve_count = std::stoul(argv[3]);
    local_reserve = std::stoul(argv[4]);
    const bool write = std::stoul(argv[5]), callback = std::stoul(argv[6]);
    const auto n = std::stoull(argv[7]);
    const bool memory = std::stoul(argv[8]);
    if (!view_count || view_count > 64 || reserve_count > 1024 || !n)
        return 2;
    if (mode == "bounded") {
        bounded_space = true;
        run<256, 4, 64>(n, write, callback, memory);
    } else if (mode == "dense")
        run<256, 4, 64>(n, write, callback, memory);
    else if (mode == "sparse")
        run<256, 64, 64>(n, write, callback, memory);
    else if (mode == "resonant")
        run<256, 5385076, 64>(n, write, callback, memory);
    else if (mode == "high")
        run<256, (1ULL << 40), 64>(n, write, callback, memory);
    else if (mode == "large" && view_count <= 9)
        run<8192, 4, 9>(n, write, callback, memory);
    else if (mode == "wide" && view_count <= 9)
        run<1024, 256, 9, reg::u512>(n, write, callback, memory);
    else
        return 2;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
