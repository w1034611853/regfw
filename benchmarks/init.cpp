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
    using Business = Arrays<64>;
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
template <class Layout>
void sample(const std::string& which, unsigned repeats) {
    for (unsigned k = 0; k < repeats; ++k) {
        const auto w0 = Clock::now();
        const auto c0 = cpu_ns();
        reg::RegisterBank<Layout> bank(reg::space().name("INIT_BENCH"));
        const auto c1 = cpu_ns();
        const auto w1 = Clock::now();
        if constexpr (std::is_same_v<Layout, Dag>)
            bank.map().set_identity_resolver(Route, [](const auto& r) {
                return r.request_id;
            });
        const auto c2 = cpu_ns();
        bank.finish();
        const auto c3 = cpu_ns();
        const auto w3 = Clock::now();
        std::size_t text = 0;
        const auto c4 = cpu_ns();
        if (which == "dag-summary")
            text = bank.map().describe_layout(reg::LayoutDetail::Summary).size();
        if (which == "dag-full")
            text = bank.map().describe_layout(reg::LayoutDetail::Full).size();
        const auto c5 = cpu_ns();
        // Outside timed sections: storage, aliased MMIO, complete snapshot semantics.
        if constexpr (std::is_same_v<Layout, Dag>) {
            auto& m = bank.map();
            if (!m.mmio_write_sized(0xc100220000ULL, 77, 4).success() ||
                m.mmio_read_sized(0xff71f0020000ULL, 4) != 77)
                throw std::runtime_error("alias mismatch");
        }
        auto snap = bank.save();
        bank.reset();
        bank.load(snap);
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << "RESULT " << which << ' ' << k << ' ' << c1 - c0 << ' ' << c2 - c1 << ' '
                  << c3 - c2 << ' ' << c5 - c4 << ' '
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count() << ' '
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(w3 - w0).count() << ' '
                  << bank.map().entity_count() << ' ' << bank.map().total_node_data_count() << ' '
                  << text << ' ' << usage.ru_maxrss << '\n';
    }
}

int main(int argc, char** argv) try {
    std::string which = argc > 1 ? argv[1] : "dag";
    unsigned n = argc > 2 ? std::stoul(argv[2]) : 5;
    if (which == "storage128")
        sample<Storage<128>>(which, n);
    else if (which == "storage1024")
        sample<Storage<1024>>(which, n);
    else if (which == "storage4096")
        sample<Storage<4096>>(which, n);
    else if (which == "wide256")
        sample<Storage<256, reg::u512>>(which, n);
    else
        sample<Dag>(which, n);
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
