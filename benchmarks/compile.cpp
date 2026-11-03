#ifndef ELEMENTS
#define ELEMENTS 64
#endif
#include <regfw/regfw.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <time.h>
#include <sys/resource.h>
using Clock = std::chrono::steady_clock;
std::int64_t cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return std::int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
template <unsigned N, class T = std::uint32_t>
struct Arrays : reg::RegEntityBundle {
    REG_BUNDLE(Arrays);
    static constexpr auto gap = std::uint64_t(N) * sizeof(T) + 64;
#define ITEM(K) REG(A##K, T, reg::at(K* gap).array(reg::dims(N), reg::strides(sizeof(T))));
    ITEM(0) ITEM(1) ITEM(2) ITEM(3) ITEM(4) ITEM(5) ITEM(6) ITEM(7) ITEM(8) ITEM(9)
#undef ITEM
};
template <unsigned N, class T = std::uint32_t>
struct Storage : reg::Bundle {
    REG_BUNDLE(Storage);
    using Leaf = Arrays<N, T>;
    REG_REPEAT_ENTITY(vf, Leaf, 9);
};
struct Module : reg::RegEntityBundle {
    REG_BUNDLE(Module);
    REG(STATE, std::uint32_t, reg::at(0).array(reg::dims(32), reg::strides(4)));
};
REG_ACCESS_FIELD(Route, std::uint32_t);
struct Dag : reg::Bundle {
    REG_BUNDLE(Dag);
    REG_ENTITY(module, Module);
    using Business = Arrays<ELEMENTS>;
    REG_REPEAT_ENTITY(vf, Business, 9);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        auto business = [&](auto i) {
            return reg::view("BUSINESS", 0xe0000).map(layout.vf[i]);
        };
        auto pf = reg::view("PF", 0x1000000)
                      .map(reg::view("MODULE", 0x20000).map(layout.module))
                      .map(business(8), 0x20000)
                      .map_repeat("PF_VF", 0x200000, reg::repeat(8).stride(0x100000), [&](auto i) {
                          return reg::view("SLOT", 0x100000)
                              .identity(1)
                              .using_resolver(Route)
                              .pf_to_vf()
                              .reserve(0, 0x20000)
                              .map(business(i), 0x20000);
                      });
        return reg::address_space("DAG")
            .map(pf, 0xc100000000ULL)
            .map_repeat("VF", 0xff71f0000000ULL, reg::repeat(8).stride(0x200000), [&](auto i) {
                return reg::view("VF_WINDOW", 0x200000)
                    .identity(1)
                    .using_resolver(Route)
                    .vf_to_pf()
                    .map(reg::view("MODULE_RO", 0x20000)
                             .map(layout.module)
                             .read_only_ignore_writes())
                    .map(business(i), 0x20000);
            });
    });
};

#ifndef INCLUDE_ONLY
void construct() {
    reg::RegisterBank<Dag> bank(reg::space());
}
#endif
