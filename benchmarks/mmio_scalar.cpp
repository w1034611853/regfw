// Standalone MMIO dispatch/callback benchmark; see Performance-MMIO-Scalar.md.
#include <regfw/regfw.hpp>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <time.h>
template <unsigned N>
struct Registers : reg::RegEntityBundle {
    REG_BUNDLE(Registers);
    REG(VALUES, std::uint32_t, reg::at(0).array(reg::dims(N), reg::strides(4)));
};
template <unsigned N>
struct Views : reg::Bundle {
    REG_BUNDLE(Views);
    REG_ENTITY(regs, Registers<N>);
    REG_LAYOUT([&] {
        return reg::address_space("ROOT").map(reg::view("REGS", N * 4).map(regs), 0);
    });
};
static double now() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        std::abort();
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}
template <unsigned N, bool Space>
void run(std::string_view mode,
         bool finish,
         std::string_view callback,
         bool write,
         std::uint64_t iterations,
         bool logger) {
    using Root = std::conditional_t<Space, Views<N>, Registers<N>>;
    const auto base = mode == "alias" ? N * 4 : 0;
    reg::RegisterBank<Root> bank(reg::space(N * 4 + base).name("BENCH"));
    if (base)
        bank.map().map_alias_window(base, 0, N * 4, "MIRROR");
    auto& array = [&]() -> auto& {
        if constexpr (Space)
            return bank->regs.VALUES;
        else
            return bank->VALUES;
    }();
    std::uint64_t calls = 0, info_sum = 0;
    reg::RegAccessInfo saved;
    if (callback == "light") {
        if (write)
            array.on_write([&] {
                ++calls;
            });
        else
            array.on_read([&] {
                ++calls;
            });
    } else if (callback == "info") {
        auto fn = [&](const reg::RegAccessInfo& info) {
            ++calls;
            info_sum += info.address + info.index.linear() + info.stored_before;
        };
        if (write)
            array.on_write(fn);
        else
            array.on_read(fn);
    } else if (callback == "copy") {
        auto fn = [&](const reg::RegAccessInfo& info) {
            ++calls;
            saved = info;
        };
        if (write)
            array.on_write(fn);
        else
            array.on_read(fn);
    } else if (callback != "none")
        std::abort();
    if (logger) {
        auto* log = hlog_register_logger("scalar-bench", "regfw-scalar-bench", HLOG_LEVEL_ERROR);
        if (!log)
            std::abort();
        bank.map().set_logger(log);
    }
    if (finish)
        bank.finish();
    for (unsigned i = 0; i < N; ++i)
        array.data()[i] = i + 1;
    auto& map = bank.map();
    if (map.mmio_read(base) != 1 || !map.mmio_write(base, 1).success())
        std::abort();
    calls = info_sum = 0;
    std::uint64_t sum = 0;
    const auto begin = now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const auto address = base + ((i * 13) & (N - 1)) * 4;
        if (write)
            sum += unsigned(map.mmio_write(address, std::uint32_t(i)).result);
        else
            sum += map.mmio_read(address);
    }
    const auto elapsed = now() - begin;
    if (calls != (callback == "none" ? 0 : iterations))
        std::abort();
    for (unsigned i = 0; i < N; ++i)
        sum += array.data()[i];
    if (callback == "copy")
        info_sum = saved.address + saved.index.linear() + saved.stored_before;
    std::cout << N << ',' << mode << ',' << finish << ',' << callback << ',' << write << ','
              << logger << ',' << std::fixed << std::setprecision(4) << elapsed * 1e9 / iterations
              << ',' << sum << ',' << calls << ',' << info_sum << '\n';
}
int main(int argc, char** argv) {
    if (argc != 7 && argc != 8)
        return 2;
    const bool logger = argc == 8 && std::atoi(argv[7]);
    const auto n = std::strtoul(argv[1], nullptr, 10);
    const std::string_view mode = argv[2];
    if (mode != "space" && mode != "legacy" && mode != "alias")
        return 2;
    const bool finish = std::atoi(argv[3]), write = std::atoi(argv[5]);
    const auto iterations = std::strtoull(argv[6], nullptr, 10);
    auto select = [&]<unsigned N>() {
        if (mode == "space")
            run<N, true>(mode, finish, argv[4], write, iterations, logger);
        else
            run<N, false>(mode, finish, argv[4], write, iterations, logger);
    };
    if (n == 4)
        select.template operator()<4>();
    else if (n == 256)
        select.template operator()<256>();
    else if (n == 4096)
        select.template operator()<4096>();
    else
        return 2;
}
